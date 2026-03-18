#define _GNU_SOURCE
#include "claw/plugin_api.h"
#include "claw/tool_types.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/epoll.h>
#include <sys/signalfd.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <sys/timerfd.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#define CLAW_HTTP_DEFAULT_BIND "127.0.0.1"
#define CLAW_HTTP_DEFAULT_PORT 8080
#define CLAW_HTTP_MAX_REQ 65536
#define CLAW_HTTP_MAX_RESP 65536
#define CLAW_HTTP_MAX_EVENTS 64
#define CLAW_HTTP_DEFAULT_MAX_CLIENTS 64
#define CLAW_HTTP_MAX_RUNS 64

enum source_type {
    SRC_LISTENER = 1,
    SRC_SIGNAL   = 2,
    SRC_CLIENT   = 3,
    SRC_TIMER    = 4,
    SRC_STREAM   = 5
};

typedef struct http_client http_client_t;

typedef struct {
    int type;
    int fd;
} event_source_t;

typedef struct stream_source {
    int type;
    int fd;
    http_client_t *client;
} stream_source_t;

struct http_client {
    int type;
    int fd;
    size_t req_len;
    int headers_done;
    size_t body_offset;
    int content_length;
    char reqbuf[CLAW_HTTP_MAX_REQ + 1];
    char sendbuf[CLAW_HTTP_MAX_RESP + 1024];
    size_t send_len;
    size_t send_off;
    int streaming;
    int stream_eof;
    int stream_fd;
    stream_source_t *stream_src;
};

static volatile sig_atomic_t g_stop = 0;

typedef struct {
    int used;
    long id;
    long created_at_unix;
    long finished_at_unix;
    int http_status;
    char op[64];
    char status[32];
    char target[128];
    char result[8192];
    int result_is_json;
} http_run_t;

static http_run_t g_runs[CLAW_HTTP_MAX_RUNS];
static long g_next_run_id = 1;

static const char *env_or_default(const char *name, const char *fallback)
{
    const char *v = getenv(name);
    return (v && *v) ? v : fallback;
}

static int env_port_or_default(const char *name, int fallback)
{
    const char *v = getenv(name);
    char *end = NULL;
    long n;
    if (!v || !*v) return fallback;
    n = strtol(v, &end, 10);
    if (!end || *end != '\0' || n < 1 || n > 65535) return fallback;
    return (int)n;
}

static int env_int_or_default(const char *name, int fallback)
{
    const char *v = getenv(name);
    char *end = NULL;
    long n;
    if (!v || !*v) return fallback;
    n = strtol(v, &end, 10);
    if (!end || *end != '\0' || n < 1 || n > 4096) return fallback;
    return (int)n;
}


static int send_response_build(char *out, size_t out_cap, int code, const char *content_type, const char *body)
{
    char header[512];
    const char *reason = "OK";
    size_t body_len = body ? strlen(body) : 0;
    int header_len;

    switch (code) {
    case 200: reason = "OK"; break;
    case 400: reason = "Bad Request"; break;
    case 403: reason = "Forbidden"; break;
    case 404: reason = "Not Found"; break;
    case 405: reason = "Method Not Allowed"; break;
    case 408: reason = "Request Timeout"; break;
    case 413: reason = "Payload Too Large"; break;
    case 500: reason = "Internal Server Error"; break;
    default: break;
    }

    header_len = snprintf(header, sizeof(header),
        "HTTP/1.1 %d %s\r\n"
        "Content-Type: %s\r\n"
        "Content-Length: %zu\r\n"
        "Connection: close\r\n"
        "\r\n",
        code, reason, content_type ? content_type : "text/plain; charset=utf-8", body_len);
    if (header_len < 0 || (size_t)header_len >= sizeof(header)) return -1;
    if ((size_t)header_len + body_len + 1 > out_cap) return -1;
    memcpy(out, header, (size_t)header_len);
    if (body_len) memcpy(out + header_len, body, body_len);
    out[header_len + body_len] = '\0';
    return header_len + (int)body_len;
}

static int parse_content_length(const char *headers)
{
    const char *p = headers;
    while (p && *p) {
        const char *line_end = strstr(p, "\r\n");
        size_t line_len = line_end ? (size_t)(line_end - p) : strlen(p);
        if (line_len >= 15 && strncasecmp(p, "Content-Length:", 15) == 0) {
            const char *v = p + 15;
            while (*v == ' ' || *v == '\t') ++v;
            return atoi(v);
        }
        if (!line_end) break;
        p = line_end + 2;
    }
    return 0;
}

static const char *skip_ws(const char *p)
{
    while (p && (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n')) ++p;
    return p;
}

static int hex_value(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return 10 + (c - 'a');
    if (c >= 'A' && c <= 'F') return 10 + (c - 'A');
    return -1;
}

static int utf8_from_codepoint(unsigned cp, char out[4], size_t *n)
{
    if (cp <= 0x7F) {
        out[0] = (char)cp; *n = 1;
    } else if (cp <= 0x7FF) {
        out[0] = (char)(0xC0 | (cp >> 6));
        out[1] = (char)(0x80 | (cp & 0x3F));
        *n = 2;
    } else if (cp <= 0xFFFF) {
        out[0] = (char)(0xE0 | (cp >> 12));
        out[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
        out[2] = (char)(0x80 | (cp & 0x3F));
        *n = 3;
    } else {
        out[0] = (char)(0xF0 | (cp >> 18));
        out[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
        out[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
        out[3] = (char)(0x80 | (cp & 0x3F));
        *n = 4;
    }
    return 0;
}

static const char *find_json_string_field(const char *json, const char *field)
{
    char key[128];
    const char *p;
    if (!json || !field) return NULL;
    if (snprintf(key, sizeof(key), "\"%s\"", field) >= (int)sizeof(key)) return NULL;
    p = strstr(json, key);
    if (!p) return NULL;
    p = strchr(p, ':');
    if (!p) return NULL;
    p = skip_ws(p + 1);
    if (*p != '"') return NULL;
    return p;
}

static int decode_json_string(const char *quoted, claw_response_t *out)
{
    const char *p = quoted;
    if (!p || *p != '"' || !out) return -1;
    ++p;
    while (*p) {
        if (*p == '"') return 0;
        if (*p == '\\') {
            char c;
            ++p;
            if (!*p) return -2;
            switch (*p) {
            case '"': c = '"'; break;
            case '\\': c = '\\'; break;
            case '/': c = '/'; break;
            case 'b': c = '\b'; break;
            case 'f': c = '\f'; break;
            case 'n': c = '\n'; break;
            case 'r': c = '\r'; break;
            case 't': c = '\t'; break;
            case 'u': {
                int h1, h2, h3, h4;
                unsigned cp;
                char utf8[4];
                size_t n = 0;
                if (!p[1] || !p[2] || !p[3] || !p[4]) return -3;
                h1 = hex_value(p[1]); h2 = hex_value(p[2]);
                h3 = hex_value(p[3]); h4 = hex_value(p[4]);
                if (h1 < 0 || h2 < 0 || h3 < 0 || h4 < 0) return -4;
                cp = (unsigned)((h1 << 12) | (h2 << 8) | (h3 << 4) | h4);
                utf8_from_codepoint(cp, utf8, &n);
                if (claw_response_append_mem(out, utf8, n) != 0) return -5;
                p += 4;
                ++p;
                continue;
            }
            default:
                return -6;
            }
            if (claw_response_append_mem(out, &c, 1) != 0) return -7;
        } else if (claw_response_append_mem(out, p, 1) != 0) {
            return -8;
        }
        ++p;
    }
    return -9;
}

static int json_get_string(const char *json, const char *field, char *out, size_t out_cap)
{
    claw_response_t resp;
    const char *quoted = find_json_string_field(json, field);
    if (!quoted || !out || out_cap == 0) return -1;
    claw_response_init(&resp, out, out_cap);
    return decode_json_string(quoted, &resp);
}

static int json_get_long(const char *json, const char *field, long *value_out)
{
    char key[128];
    const char *p;
    char *end = NULL;
    long v;
    if (!json || !field || !value_out) return -1;
    if (snprintf(key, sizeof(key), "\"%s\"", field) >= (int)sizeof(key)) return -1;
    p = strstr(json, key);
    if (!p) return -2;
    p = strchr(p, ':');
    if (!p) return -3;
    p = skip_ws(p + 1);
    if (*p == '"') {
        char tmp[64];
        claw_response_t resp;
        claw_response_init(&resp, tmp, sizeof(tmp));
        if (decode_json_string(p, &resp) != 0) return -4;
        v = strtol(tmp, &end, 10);
        if (!end || *end != '\0') return -5;
    } else {
        v = strtol(p, &end, 10);
        if (p == end) return -6;
    }
    *value_out = v;
    return 0;
}

static int path_starts_with(const char *path, const char *prefix)
{
    size_t n;
    if (!path || !prefix) return 0;
    n = strlen(prefix);
    return strncmp(path, prefix, n) == 0;
}

static int query_get_long(const char *path, const char *key, long *value_out)
{
    char needle[64];
    const char *q;
    char *end = NULL;
    long v;
    size_t needle_len;
    if (!path || !key || !value_out) return -1;
    q = strchr(path, '?');
    if (!q) return -2;
    ++q;
    if (snprintf(needle, sizeof(needle), "%s=", key) >= (int)sizeof(needle)) return -3;
    needle_len = strlen(needle);
    while (*q) {
        if (strncmp(q, needle, needle_len) == 0) {
            q += needle_len;
            v = strtol(q, &end, 10);
            if (q == end) return -4;
            *value_out = v;
            return 0;
        }
        q = strchr(q, '&');
        if (!q) break;
        ++q;
    }
    return -5;
}
static int query_get_string(const char *path, const char *key, char *out, size_t out_cap)
{
    char needle[64];
    const char *p, *start;
    size_t len = 0;
    if (!path || !key || !out || out_cap == 0) return -1;
    if (snprintf(needle, sizeof(needle), "%s=", key) >= (int)sizeof(needle)) return -1;
    p = strchr(path, '?');
    if (!p) return -2;
    ++p;
    while (*p) {
        if (strncmp(p, needle, strlen(needle)) == 0) {
            start = p + strlen(needle);
            while (start[len] && start[len] != '&') ++len;
            if (len + 1 > out_cap) return -3;
            memcpy(out, start, len);
            out[len] = '\0';
            return 0;
        }
        p = strchr(p, '&');
        if (!p) break;
        ++p;
    }
    return -4;
}

static const char *skip_json_value_end(const char *p)
{
    int depth_obj = 0, depth_arr = 0, in_str = 0, esc = 0;
    if (!p) return NULL;
    if (*p == '"') {
        ++p;
        while (*p) {
            if (esc) esc = 0;
            else if (*p == '\\') esc = 1;
            else if (*p == '"') return p + 1;
            ++p;
        }
        return NULL;
    }
    if (*p == '{' || *p == '[') {
        depth_obj = *p == '{' ? 1 : 0;
        depth_arr = *p == '[' ? 1 : 0;
        ++p;
        while (*p) {
            if (in_str) {
                if (esc) esc = 0;
                else if (*p == '\\') esc = 1;
                else if (*p == '"') in_str = 0;
                ++p;
                continue;
            }
            if (*p == '"') {
                in_str = 1;
                ++p;
                continue;
            }
            if (*p == '{') depth_obj++;
            else if (*p == '}') {
                if (--depth_obj == 0 && depth_arr == 0) return p + 1;
            } else if (*p == '[') depth_arr++;
            else if (*p == ']') {
                if (--depth_arr == 0 && depth_obj == 0) return p + 1;
            }
            ++p;
        }
        return NULL;
    }
    while (*p && *p != ',' && *p != '}' && *p != ']' &&
           *p != ' ' && *p != '\t' && *p != '\r' && *p != '\n')
        ++p;
    return p;
}

static int json_get_top_level_field_raw(const char *json, const char *field,
    char *out, size_t out_cap)
{
    char key[128];
    const char *p;
    const char *value_start, *value_end;
    if (!json || !field || !out || out_cap == 0) return -1;
    if (snprintf(key, sizeof(key), "\"%s\"", field) >= (int)sizeof(key)) return -1;
    p = strstr(json, key);
    if (!p) return -2;
    p = strchr(p, ':');
    if (!p) return -3;
    value_start = skip_ws(p + 1);
    value_end = skip_json_value_end(value_start);
    if (!value_end || value_end < value_start) return -4;
    if ((size_t)(value_end - value_start) + 1 > out_cap) return -5;
    memcpy(out, value_start, (size_t)(value_end - value_start));
    out[value_end - value_start] = '\0';
    return 0;
}

static int json_copy_without_top_level_field(const char *json, const char *field,
    char *out, size_t out_cap)
{
    const char *p;
    claw_response_t resp;
    int first = 1;
    if (!json || !field || !out || out_cap == 0) return -1;
    p = skip_ws(json);
    if (!p || *p != '{') return -2;
    ++p;
    claw_response_init(&resp, out, out_cap);
    if (claw_response_append(&resp, "{") != 0) return -3;
    while (*p) {
        const char *name_start, *name_end, *value_start, *value_end;
        size_t name_len;
        p = skip_ws(p);
        if (*p == '}') break;
        if (*p == ',') {
            ++p;
            continue;
        }
        if (*p != '"') return -4;
        name_start = ++p;
        while (*p) {
            if (*p == '\\' && p[1]) {
                p += 2;
                continue;
            }
            if (*p == '"') break;
            ++p;
        }
        if (*p != '"') return -5;
        name_end = p++;
        p = skip_ws(p);
        if (*p != ':') return -6;
        value_start = skip_ws(p + 1);
        value_end = skip_json_value_end(value_start);
        if (!value_end) return -7;
        name_len = (size_t)(name_end - name_start);
        if (!(strlen(field) == name_len && strncmp(name_start, field, name_len) == 0)) {
            if (!first && claw_response_append(&resp, ",") != 0) return -8;
            if (claw_response_append_mem(&resp, "\"", 1) != 0 ||
                claw_response_append_mem(&resp, name_start, name_len) != 0 ||
                claw_response_append(&resp, "\":") != 0 ||
                claw_response_append_mem(&resp, value_start,
                    (size_t)(value_end - value_start)) != 0) return -8;
            first = 0;
        }
        p = value_end;
        p = skip_ws(p);
        if (*p == ',') ++p;
    }
    if (claw_response_append(&resp, "}") != 0) return -9;
    return 0;
}

static int build_structured_tool_json_args(const char *body, char *out, size_t out_cap)
{
    if (!body || !out || out_cap == 0) return -1;
    if (json_get_top_level_field_raw(body, "args", out, out_cap) == 0) return 0;
    if (json_get_string(body, "json_args", out, out_cap) == 0) return 0;
    return json_copy_without_top_level_field(body, "tool", out, out_cap);
}

static int append_json_escaped(claw_response_t *resp, const char *s);
static int append_run_json(claw_response_t *resp, const http_run_t *run);

static int body_looks_like_json(const char *body)
{
    body = skip_ws(body);
    return body && (*body == '{' || *body == '[');
}

static http_run_t *run_alloc_slot(void)
{
    size_t i;
    http_run_t *slot = NULL;
    for (i = 0; i < CLAW_HTTP_MAX_RUNS; ++i) {
        if (!g_runs[i].used) {
            slot = &g_runs[i];
            break;
        }
    }
    if (!slot) slot = &g_runs[0];
    memset(slot, 0, sizeof(*slot));
    slot->used = 1;
    slot->id = g_next_run_id++;
    slot->created_at_unix = (long)time(NULL);
    snprintf(slot->status, sizeof(slot->status), "running");
    return slot;
}

static http_run_t *run_find(long id)
{
    size_t i;
    for (i = 0; i < CLAW_HTTP_MAX_RUNS; ++i) {
        if (g_runs[i].used && g_runs[i].id == id) return &g_runs[i];
    }
    return NULL;
}

static int run_store_result(http_run_t *run, int http_status, const char *result_body)
{
    if (!run) return -1;
    run->finished_at_unix = (long)time(NULL);
    run->http_status = http_status;
    snprintf(run->status, sizeof(run->status), http_status >= 400 ? "failed" : "completed");
    if (!result_body) result_body = "";
    run->result_is_json = body_looks_like_json(result_body);
    if (snprintf(run->result, sizeof(run->result), "%s", result_body) >= (int)sizeof(run->result))
        return -1;
    return 0;
}

static int ensure_dir_recursive(const char *path)
{
    char tmp[PATH_MAX];
    char *p;
    if (!path) return -1;
    if (snprintf(tmp, sizeof(tmp), "%s", path) >= (int)sizeof(tmp)) return -1;
    for (p = tmp + 1; *p; ++p) {
        if (*p == '/') {
            *p = '\0';
            if (mkdir(tmp, 0777) != 0 && errno != EEXIST) return -1;
            *p = '/';
        }
    }
    if (mkdir(tmp, 0777) != 0 && errno != EEXIST) return -1;
    return 0;
}

static int runs_dir_path(char *out, size_t out_cap)
{
    const char *dir = env_or_default("CLAW_RUNS_DIR", "./state/runs");
    if (!out || out_cap == 0) return -1;
    if (snprintf(out, out_cap, "%s", dir) >= (int)out_cap) return -1;
    return 0;
}

static int run_file_path(long id, char *out, size_t out_cap)
{
    char dir[PATH_MAX];
    if (runs_dir_path(dir, sizeof(dir)) != 0) return -1;
    if (ensure_dir_recursive(dir) != 0) return -1;
    if (!out || out_cap == 0) return -1;
    if (snprintf(out, out_cap, "%s/run-%ld.json", dir, id) >= (int)out_cap) return -1;
    return 0;
}

static int persist_run_file(const http_run_t *run)
{
    char path[PATH_MAX];
    char buf[CLAW_HTTP_MAX_RESP];
    claw_response_t resp;
    FILE *fp;
    if (!run) return -1;
    if (run_file_path(run->id, path, sizeof(path)) != 0) return -1;
    claw_response_init(&resp, buf, sizeof(buf));
    if (append_run_json(&resp, run) != 0) return -1;
    fp = fopen(path, "wb");
    if (!fp) return -1;
    if (fwrite(buf, 1, resp.len, fp) != resp.len) {
        fclose(fp);
        return -1;
    }
    fclose(fp);
    return 0;
}

static int append_persisted_run_json(claw_response_t *resp, long id)
{
    char path[PATH_MAX];
    FILE *fp;
    size_t n;
    char buf[2048];
    if (!resp) return -1;
    if (run_file_path(id, path, sizeof(path)) != 0) return -1;
    fp = fopen(path, "rb");
    if (!fp) return -1;
    while ((n = fread(buf, 1, sizeof(buf), fp)) > 0) {
        if (claw_response_append_mem(resp, buf, n) != 0) {
            fclose(fp);
            return -1;
        }
    }
    fclose(fp);
    return resp->len > 0 ? 0 : -1;
}

static int append_run_json(claw_response_t *resp, const http_run_t *run)
{
    char tmp[256];
    if (!resp || !run) return -1;
    if (snprintf(tmp, sizeof(tmp),
        "{\"id\":%ld,\"op\":\"%s\",\"status\":\"%s\",\"target\":\"",
        run->id, run->op, run->status) < 0) return -1;
    if (claw_response_append(resp, tmp) != 0) return -1;
    if (append_json_escaped(resp, run->target) != 0) return -1;
    if (snprintf(tmp, sizeof(tmp),
        "\",\"http_status\":%d,\"created_at_unix\":%ld,\"finished_at_unix\":%ld,\"result\":",
        run->http_status, run->created_at_unix, run->finished_at_unix) < 0) return -1;
    if (claw_response_append(resp, tmp) != 0) return -1;
    if (run->result_is_json && run->result[0]) {
        if (claw_response_append(resp, run->result) != 0) return -1;
    } else {
        if (claw_response_append(resp, "\"") != 0) return -1;
        if (append_json_escaped(resp, run->result) != 0) return -1;
        if (claw_response_append(resp, "\"") != 0) return -1;
    }
    return claw_response_append(resp, "}");
}

static int append_sse_event(claw_response_t *resp, const char *event_name, const char *data_json)
{
    if (!resp || !event_name || !data_json) return -1;
    if (claw_response_append(resp, "event: ") != 0) return -1;
    if (claw_response_append(resp, event_name) != 0) return -1;
    if (claw_response_append(resp, "\ndata: ") != 0) return -1;
    if (claw_response_append(resp, data_json) != 0) return -1;
    return claw_response_append(resp, "\n\n");
}

static int execute_run_request(const claw_host_api_t *host, const char *body,
    http_run_t *run, char *result_buf, size_t result_cap, int *http_status_out)
{
    char op[64];
    claw_response_t result;
    int rc = 0;
    int http_status = 200;
    if (!host || !body || !run || !result_buf || result_cap == 0 || !http_status_out) return -1;
    if (json_get_string(body, "op", op, sizeof(op)) != 0) return -2;
    claw_response_init(&result, result_buf, result_cap);
    snprintf(run->op, sizeof(run->op), "%s", op);
    if (strcmp(op, "chat") == 0) {
        char provider[128], message[4096];
        if (json_get_string(body, "provider", provider, sizeof(provider)) != 0 ||
            json_get_string(body, "message", message, sizeof(message)) != 0) return -3;
        snprintf(run->target, sizeof(run->target), "%s", provider);
        rc = host->chat ? host->chat(provider, message, &result) : -1;
        if (rc != 0 && result.len == 0) {
            http_status = 500;
            claw_response_append(&result, "chat failed");
        }
    } else if (strcmp(op, "tool.invoke") == 0) {
        char tool[128], json_args[8192];
        if (json_get_string(body, "tool", tool, sizeof(tool)) != 0) return -4;
        if (build_structured_tool_json_args(body, json_args, sizeof(json_args)) != 0) return -5;
        snprintf(run->target, sizeof(run->target), "%s", tool);
        rc = host->tool_invoke ? host->tool_invoke(tool, json_args, &result) : -1;
        if (rc != 0 && result.len == 0) {
            http_status = rc == -2 ? 404 : 500;
            claw_response_append(&result, rc == -2 ? "{\"ok\":false,\"error\":\"tool not found\"}" :
                "{\"ok\":false,\"error\":\"tool invoke failed\"}");
        } else {
            http_status = claw_tt_http_status_from_json(result.buf);
            if (http_status == 500 && rc == -2) http_status = 404;
        }
    } else if (strcmp(op, "scheduler.status") == 0) {
        snprintf(run->target, sizeof(run->target), "scheduler");
        rc = host->scheduler_status ? host->scheduler_status(&result) : -1;
        if (rc != 0 && result.len == 0) {
            http_status = 500;
            claw_response_append(&result, "{\"ok\":false,\"error\":\"scheduler status failed\"}");
        }
    } else if (strcmp(op, "scheduler.tasks") == 0) {
        snprintf(run->target, sizeof(run->target), "scheduler");
        rc = host->scheduler_tasks_json ? host->scheduler_tasks_json(&result) : -1;
        if (rc != 0 && result.len == 0) {
            http_status = 500;
            claw_response_append(&result, "{\"ok\":false,\"error\":\"scheduler tasks failed\"}");
        }
    } else {
        return -6;
    }
    *http_status_out = http_status;
    return 0;
}

static int append_json_escaped(claw_response_t *resp, const char *s)
{
    const unsigned char *p;
    char tmp[7];
    if (!resp || !s) return -1;
    for (p = (const unsigned char *)s; *p; ++p) {
        switch (*p) {
        case '"':
            if (claw_response_append(resp, "\\\"") != 0) return -1;
            break;
        case '\\':
            if (claw_response_append(resp, "\\\\") != 0) return -1;
            break;
        case '\b':
            if (claw_response_append(resp, "\\b") != 0) return -1;
            break;
        case '\f':
            if (claw_response_append(resp, "\\f") != 0) return -1;
            break;
        case '\n':
            if (claw_response_append(resp, "\\n") != 0) return -1;
            break;
        case '\r':
            if (claw_response_append(resp, "\\r") != 0) return -1;
            break;
        case '\t':
            if (claw_response_append(resp, "\\t") != 0) return -1;
            break;
        default:
            if (*p < 0x20) {
                snprintf(tmp, sizeof(tmp), "\\u%04x", (unsigned int)*p);
                if (claw_response_append(resp, tmp) != 0) return -1;
            } else if (claw_response_append_mem(resp, (const char *)p, 1) != 0) {
                return -1;
            }
            break;
        }
    }
    return 0;
}

static __attribute__((unused)) int build_shell_json_args(const char *body, char *out, size_t out_cap)
{
    char command[4096];
    char cwd[1024];
    long timeout_ms;
    claw_response_t resp;
    int have_cwd = 0;
    int have_timeout = 0;

    if (!body || !out || out_cap == 0) return -1;
    if (strstr(body, "\"argv\"") || strstr(body, "\"env\"")) {
        if (snprintf(out, out_cap, "%s", body) >= (int)out_cap) return -2;
        return 0;
    }
    if (json_get_string(body, "command", command, sizeof(command)) != 0) return -1;
    if (json_get_string(body, "cwd", cwd, sizeof(cwd)) == 0) have_cwd = 1;
    if (json_get_long(body, "timeout_ms", &timeout_ms) == 0) have_timeout = 1;

    claw_response_init(&resp, out, out_cap);
    if (claw_response_append(&resp, "{") != 0) return -2;
    if (claw_response_append(&resp, "\"command\":\"") != 0) return -2;
    if (append_json_escaped(&resp, command) != 0) return -2;
    if (claw_response_append(&resp, "\"") != 0) return -2;
    if (have_cwd) {
        if (claw_response_append(&resp, ",\"cwd\":\"") != 0) return -2;
        if (append_json_escaped(&resp, cwd) != 0) return -2;
        if (claw_response_append(&resp, "\"") != 0) return -2;
    }
    if (have_timeout) {
        char tmp[64];
        if (snprintf(tmp, sizeof(tmp), ",\"timeout_ms\":%ld", timeout_ms) <= 0) return -2;
        if (claw_response_append(&resp, tmp) != 0) return -2;
    }
    if (claw_response_append(&resp, "}") != 0) return -2;
    return 0;
}

static __attribute__((unused)) int build_passthrough_json_args(const char *body, char *out, size_t out_cap, const char *required_field)
{
    if (!body || !out || out_cap == 0) return -1;
    if (required_field && *required_field) {
        char needle[64];
        if (snprintf(needle, sizeof(needle), "\"%s\"", required_field) <= 0) return -1;
        if (!strstr(body, needle)) return -2;
    }
    if (snprintf(out, out_cap, "%s", body) >= (int)out_cap) return -3;
    return 0;
}


static int handle_request(const claw_host_api_t *host, const char *method, const char *path, const char *body,
    char *resp_buf, size_t resp_cap, int *status, const char **content_type)
{
    claw_response_t resp;
    char provider[128], message[4096], memory[128], key[512], value[4096], query[1024];
    char tool[128], json_args[8192];
    int rc;

    claw_response_init(&resp, resp_buf, resp_cap);
    *status = 200;
    *content_type = "text/plain; charset=utf-8";

    if (strcmp(method, "GET") == 0 && strcmp(path, "/health") == 0) {
        *content_type = "application/json; charset=utf-8";
        return claw_response_append(&resp, "{\"ok\":true}");
    }
    if (strcmp(method, "GET") == 0 && strcmp(path, "/v1/list") == 0) {
        *content_type = "application/json; charset=utf-8";
        return host->list_modules(&resp);
    }
    if (strcmp(method, "GET") == 0 && path_starts_with(path, "/v1/runs")) {
        long id = 0;
        http_run_t *run;
        *content_type = "application/json; charset=utf-8";
        if (query_get_long(path, "id", &id) != 0 || id <= 0) {
            *status = 400;
            return claw_response_append(&resp, "{\"ok\":false,\"error\":\"missing run id\"}");
        }
        if (append_persisted_run_json(&resp, id) == 0) return 0;
        run = run_find(id);
        if (!run) {
            *status = 404;
            return claw_response_append(&resp, "{\"ok\":false,\"error\":\"run not found\"}");
        }
        return append_run_json(&resp, run);
    }
    if (strcmp(method, "GET") == 0 && (strcmp(path, "/v1/tools/schemas") == 0 || strcmp(path, "/v1/tools/docs") == 0)) {
        *content_type = "application/json; charset=utf-8";
        return host->tool_schemas ? host->tool_schemas(&resp) : claw_response_append(&resp, "[]");
    }
    if (strcmp(method, "GET") == 0 && (path_starts_with(path, "/v1/tools/schema") || path_starts_with(path, "/v1/tools/doc"))) {
        char name[128];
        *content_type = "application/json; charset=utf-8";
        if (query_get_string(path, "name", name, sizeof(name)) != 0 || name[0] == '\0') {
            *status = 400;
            return claw_response_append(&resp, "{\"ok\":false,\"error\":\"missing tool name\"}");
        }
        rc = host->tool_schema_get ? host->tool_schema_get(name, &resp) : -1;
        if (rc != 0) {
            *status = 404;
            if (resp.len == 0) return claw_response_append(&resp, "{\"ok\":false,\"error\":\"tool schema not found\"}");
        }
        return 0;
    }
    if (strcmp(method, "GET") == 0 && strcmp(path, "/v1/runtime/schemas") == 0) {
        *content_type = "application/json; charset=utf-8";
        return host->runtime_schemas ? host->runtime_schemas(&resp) : claw_response_append(&resp, "[]");
    }
    if (strcmp(method, "GET") == 0 && path_starts_with(path, "/v1/runtime/schema")) {
        char name[128];
        *content_type = "application/json; charset=utf-8";
        if (query_get_string(path, "name", name, sizeof(name)) != 0 || name[0] == '\0') {
            *status = 400;
            return claw_response_append(&resp, "{\"ok\":false,\"error\":\"missing runtime schema name\"}");
        }
        rc = host->runtime_schema_get ? host->runtime_schema_get(name, &resp) : -1;
        if (rc != 0) {
            *status = 404;
            if (resp.len == 0) return claw_response_append(&resp, "{\"ok\":false,\"error\":\"runtime schema not found\"}");
        }
        return 0;
    }
    if (strcmp(method, "GET") == 0 && (strcmp(path, "/openapi.json") == 0 || strcmp(path, "/v1/openapi.json") == 0)) {
        *content_type = "application/json; charset=utf-8";
        return host->openapi_json ? host->openapi_json(&resp) : claw_response_append(&resp, "{}");
    }
    if (strcmp(method, "GET") == 0 && strcmp(path, "/v1/metrics") == 0) {
        *content_type = "application/json; charset=utf-8";
        return host->metrics_json ? host->metrics_json(&resp) : claw_response_append(&resp, "{}");
    }
    if (strcmp(method, "GET") == 0 && strcmp(path, "/metrics") == 0) {
        *content_type = "text/plain; version=0.0.4; charset=utf-8";
        return host->metrics_prometheus ? host->metrics_prometheus(&resp) : claw_response_append(&resp, "");
    }
    if (strcmp(method, "GET") == 0 && strcmp(path, "/v1/scheduler/status") == 0) {
        *content_type = "application/json; charset=utf-8";
        return host->scheduler_status ? host->scheduler_status(&resp) : claw_response_append(&resp, "{}");
    }
    if (strcmp(method, "GET") == 0 && strcmp(path, "/v1/scheduler/tasks") == 0) {
        *content_type = "application/json; charset=utf-8";
        return host->scheduler_tasks_json ? host->scheduler_tasks_json(&resp) : claw_response_append(&resp, "[]");
    }
    if (strcmp(method, "GET") == 0 && path_starts_with(path, "/v1/scheduler/task")) {
        long id = 0;
        *content_type = "application/json; charset=utf-8";
        if (query_get_long(path, "id", &id) != 0 || id <= 0) {
            *status = 400;
            *content_type = "text/plain; charset=utf-8";
            return claw_response_append(&resp, "missing valid id");
        }
        rc = host->scheduler_task_get_json ? host->scheduler_task_get_json(id, &resp) : -1;
        if (rc != 0) {
            *status = 404;
            *content_type = "text/plain; charset=utf-8";
            if (resp.len == 0) return claw_response_append(&resp, "scheduler task not found");
        }
        return 0;
    }

    if (strcmp(method, "POST") != 0) {
        *status = 405;
        return claw_response_append(&resp, "method not allowed");
    }
    if (!body) body = "{}";

    if (strcmp(path, "/v1/runs") == 0 || strcmp(path, "/v1/runs/stream") == 0) {
        http_run_t *run = run_alloc_slot();
        char run_result[CLAW_HTTP_MAX_RESP];
        char event_json[1024];
        char output_json[CLAW_HTTP_MAX_RESP];
        if (!run) {
            *status = 500;
            return claw_response_append(&resp, "{\"ok\":false,\"error\":\"run allocation failed\"}");
        }
        if (execute_run_request(host, body, run, run_result, sizeof(run_result), status) != 0) {
            *status = 400;
            return claw_response_append(&resp, "{\"ok\":false,\"error\":\"invalid run request\"}");
        }
        if (run_store_result(run, *status, run_result) != 0) {
            *status = 500;
            return claw_response_append(&resp, "{\"ok\":false,\"error\":\"run persistence failed\"}");
        }
        if (persist_run_file(run) != 0) {
            *status = 500;
            return claw_response_append(&resp, "{\"ok\":false,\"error\":\"run file persistence failed\"}");
        }
        if (strcmp(path, "/v1/runs/stream") == 0) {
            claw_response_t out_resp;
            *content_type = "text/event-stream; charset=utf-8";
            if (snprintf(event_json, sizeof(event_json),
                "{\"id\":%ld,\"op\":\"%s\",\"status\":\"running\"}",
                run->id, run->op) < 0) return -1;
            if (append_sse_event(&resp, "run.started", event_json) != 0) return -1;
            claw_response_init(&out_resp, output_json, sizeof(output_json));
            if (append_run_json(&out_resp, run) != 0) return -1;
            if (append_sse_event(&resp, "run.output", output_json) != 0) return -1;
            if (snprintf(event_json, sizeof(event_json),
                "{\"id\":%ld,\"status\":\"%s\",\"http_status\":%d}",
                run->id, run->status, run->http_status) < 0) return -1;
            return append_sse_event(&resp, "run.completed", event_json);
        }
        *content_type = "application/json; charset=utf-8";
        return append_run_json(&resp, run);
    }

    if (strcmp(path, "/v1/chat") == 0) {
        if (json_get_string(body, "provider", provider, sizeof(provider)) != 0 ||
            json_get_string(body, "message", message, sizeof(message)) != 0) {
            *status = 400;
            return claw_response_append(&resp, "missing provider or message");
        }
        rc = host->chat(provider, message, &resp);
        if (rc != 0) {
            *status = 500;
            if (resp.len == 0) return claw_response_append(&resp, "chat failed");
        }
        return 0;
    }

    if (strcmp(path, "/v1/memory/get") == 0) {
        if (json_get_string(body, "memory", memory, sizeof(memory)) != 0 ||
            json_get_string(body, "key", key, sizeof(key)) != 0) {
            *status = 400;
            return claw_response_append(&resp, "missing memory or key");
        }
        rc = host->memory_get(memory, key, &resp);
        if (rc != 0) {
            *status = 500;
            if (resp.len == 0) return claw_response_append(&resp, "memory get failed");
        }
        return 0;
    }

    if (strcmp(path, "/v1/memory/put") == 0) {
        *content_type = "application/json; charset=utf-8";
        if (json_get_string(body, "memory", memory, sizeof(memory)) != 0 ||
            json_get_string(body, "key", key, sizeof(key)) != 0 ||
            json_get_string(body, "value", value, sizeof(value)) != 0) {
            *status = 400;
            *content_type = "text/plain; charset=utf-8";
            return claw_response_append(&resp, "missing memory, key, or value");
        }
        rc = host->memory_put(memory, key, value);
        if (rc != 0) {
            *status = 500;
            *content_type = "text/plain; charset=utf-8";
            return claw_response_append(&resp, "memory put failed");
        }
        return claw_response_append(&resp, "{\"ok\":true}");
    }

    if (strcmp(path, "/v1/memory/search") == 0) {
        *content_type = "application/json; charset=utf-8";
        if (json_get_string(body, "memory", memory, sizeof(memory)) != 0 ||
            json_get_string(body, "query", query, sizeof(query)) != 0) {
            *status = 400;
            *content_type = "text/plain; charset=utf-8";
            return claw_response_append(&resp, "missing memory or query");
        }
        rc = host->memory_search(memory, query, &resp);
        if (rc != 0) {
            *status = 500;
            if (resp.len == 0) {
                *content_type = "text/plain; charset=utf-8";
                return claw_response_append(&resp, "memory search failed");
            }
        }
        return 0;
    }

    if (strcmp(path, "/v1/tools/validate") == 0) {
        *content_type = "application/json; charset=utf-8";
        if (json_get_string(body, "tool", tool, sizeof(tool)) != 0) {
            *status = 400;
            return claw_response_append(&resp, "{\"ok\":false,\"error\":\"missing tool\"}");
        }
        if (build_structured_tool_json_args(body, json_args, sizeof(json_args)) != 0) {
            *status = 400;
            return claw_response_append(&resp, "{\"ok\":false,\"error\":\"invalid validation request body\"}");
        }
        rc = host->tool_validate ? host->tool_validate(tool, json_args, &resp) : -1;
        if (rc != 0 && resp.len == 0) {
            *status = rc == -2 ? 404 : 500;
            return claw_response_append(&resp, rc == -2 ?
                "{\"ok\":false,\"error\":\"tool not found\"}" :
                "{\"ok\":false,\"error\":\"tool validation failed\"}");
        }
        *status = claw_tt_http_status_from_json(resp.buf);
        if (*status == 500 && rc == -2) *status = 404;
        return 0;
    }

    if (strcmp(path, "/v1/tools/invoke") == 0 || strcmp(path, "/v1/tool/invoke") == 0) {
        *content_type = "application/json; charset=utf-8";
        if (json_get_string(body, "tool", tool, sizeof(tool)) != 0) {
            *status = 400;
            return claw_response_append(&resp, "{\"ok\":false,\"error\":\"missing tool\"}");
        }
        if (build_structured_tool_json_args(body, json_args, sizeof(json_args)) != 0) {
            *status = 400;
            return claw_response_append(&resp, "{\"ok\":false,\"error\":\"invalid tool invoke body\"}");
        }
        rc = host->tool_invoke(tool, json_args, &resp);
        if (rc != 0 && resp.len == 0) {
            *status = rc == -2 ? 404 : 500;
            return claw_response_append(&resp, rc == -2 ?
                "{\"ok\":false,\"error\":\"tool not found\"}" :
                "{\"ok\":false,\"error\":\"tool invoke failed\"}");
        }
        *status = claw_tt_http_status_from_json(resp.buf);
        if (*status == 500 && rc == -2) *status = 404;
        return 0;
    }

    if (strcmp(path, "/v1/tool/shell") == 0) {
        *content_type = "application/json; charset=utf-8";
        snprintf(json_args, sizeof(json_args), "%s", body ? body : "{}");
        rc = host->tool_invoke("shell", json_args, &resp);
        *status = claw_tt_http_status_from_json(resp.buf);
        return 0;
    }

    if (strcmp(path, "/v1/tools/exec") == 0) {
        *content_type = "application/json; charset=utf-8";
        if (!body || !strstr(body, "\"argv\"")) {
            *status = 400;
            return claw_response_append(&resp, "{\"ok\":false,\"tool\":\"exec\",\"error\":{\"code\":\"schema_validation\",\"field\":\"argv\",\"message\":\"argv is required\"}}");
        }
        snprintf(json_args, sizeof(json_args), "%s", body);
        rc = host->tool_invoke("exec", json_args, &resp);
        *status = claw_tt_http_status_from_json(resp.buf);
        return 0;
    }

    if (strcmp(path, "/v1/fs/read") == 0) {
        *content_type = "application/json; charset=utf-8";
        if (!body || !strstr(body, "\"path\"")) {
            *status = 400;
            return claw_response_append(&resp, "{\"ok\":false,\"tool\":\"fs.read\",\"error\":{\"code\":\"schema_validation\",\"field\":\"path\",\"message\":\"path is required\"}}");
        }
        snprintf(json_args, sizeof(json_args), "%s", body);
        rc = host->tool_invoke("fs.read", json_args, &resp);
        *status = claw_tt_http_status_from_json(resp.buf);
        return 0;
    }

    if (strcmp(path, "/v1/fs/write") == 0) {
        if (build_passthrough_json_args(body, json_args, sizeof(json_args), "path") != 0 || !strstr(body, "\"content\"")) {
            *status = 400;
            return claw_response_append(&resp, "missing path/content or invalid fs.write body");
        }
        rc = host->tool_invoke("fs.write", json_args, &resp);
        if (rc != 0) {
            *status = 500;
            if (resp.len == 0) return claw_response_append(&resp, "fs.write failed");
        }
        return 0;
    }

    if (strcmp(path, "/v1/fs/list") == 0) {
        *content_type = "application/json; charset=utf-8";
        snprintf(json_args, sizeof(json_args), "%s", body ? body : "{}");
        rc = host->tool_invoke("fs.list", json_args, &resp);
        *status = claw_tt_http_status_from_json(resp.buf);
        return 0;
    }

    if (strcmp(path, "/v1/scheduler/task/upsert") == 0) {
        long id = 0, enabled = 1, paused = 0, interval_sec = 0, run_at_unix = 0, delay_sec = 0;
        char schedule_type[32], kind[64], target2[128], arg1_2[4096], arg2_2[4096], cron_expr[128], timezone[64];
        if (json_get_long(body, "id", &id) != 0) id = 0;
        if (json_get_long(body, "enabled", &enabled) != 0) enabled = 1;
        if (json_get_long(body, "paused", &paused) != 0) paused = 0;
        if (json_get_string(body, "schedule_type", schedule_type, sizeof(schedule_type)) != 0) snprintf(schedule_type, sizeof(schedule_type), "interval");
        if (json_get_string(body, "kind", kind, sizeof(kind)) != 0 ||
            json_get_string(body, "target", target2, sizeof(target2)) != 0 ||
            json_get_string(body, "arg1", arg1_2, sizeof(arg1_2)) != 0) {
            *status = 400;
            return claw_response_append(&resp, "missing kind, target, or arg1");
        }
        if (json_get_long(body, "interval_sec", &interval_sec) != 0) interval_sec = 0;
        if (json_get_long(body, "run_at_unix", &run_at_unix) != 0) run_at_unix = 0;
        if (json_get_long(body, "delay_sec", &delay_sec) == 0 && delay_sec > 0) run_at_unix = time(NULL) + delay_sec;
        if (json_get_string(body, "cron_expr", cron_expr, sizeof(cron_expr)) != 0) cron_expr[0] = '\0';
        if (json_get_string(body, "timezone", timezone, sizeof(timezone)) != 0) timezone[0] = '\0';
        if (json_get_string(body, "arg2", arg2_2, sizeof(arg2_2)) != 0) arg2_2[0] = '\0';
        rc = host->scheduler_task_upsert ? host->scheduler_task_upsert(id, (int)enabled, (int)paused, schedule_type,
            (int)interval_sec, run_at_unix, cron_expr, timezone, kind, target2, arg1_2, arg2_2) : -1;
        if (rc != 0) {
            *status = 500;
            return claw_response_append(&resp, "scheduler task upsert failed");
        }
        *content_type = "application/json; charset=utf-8";
        return claw_response_append(&resp, "{\"ok\":true}");
    }

    if (strcmp(path, "/v1/scheduler/task/delete") == 0) {
        long id = 0;
        if (json_get_long(body, "id", &id) != 0 || id <= 0) {
            *status = 400;
            return claw_response_append(&resp, "missing valid id");
        }
        rc = host->scheduler_task_delete ? host->scheduler_task_delete(id) : -1;
        if (rc != 0) {
            *status = 500;
            return claw_response_append(&resp, "scheduler task delete failed");
        }
        *content_type = "application/json; charset=utf-8";
        return claw_response_append(&resp, "{\"ok\":true}");
    }

    if (strcmp(path, "/v1/scheduler/task/enable") == 0 || strcmp(path, "/v1/scheduler/task/disable") == 0) {
        long id = 0;
        if (json_get_long(body, "id", &id) != 0 || id <= 0) {
            *status = 400;
            return claw_response_append(&resp, "missing valid id");
        }
        rc = host->scheduler_task_set_enabled ? host->scheduler_task_set_enabled(id, strcmp(path, "/v1/scheduler/task/enable") == 0 ? 1 : 0) : -1;
        if (rc != 0) {
            *status = 500;
            return claw_response_append(&resp, "scheduler task enable/disable failed");
        }
        *content_type = "application/json; charset=utf-8";
        return claw_response_append(&resp, "{\"ok\":true}");
    }

    if (strcmp(path, "/v1/scheduler/task/pause") == 0 || strcmp(path, "/v1/scheduler/task/resume") == 0) {
        long id = 0;
        if (json_get_long(body, "id", &id) != 0 || id <= 0) {
            *status = 400;
            return claw_response_append(&resp, "missing valid id");
        }
        rc = host->scheduler_task_set_paused ? host->scheduler_task_set_paused(id, strcmp(path, "/v1/scheduler/task/pause") == 0 ? 1 : 0) : -1;
        if (rc != 0) {
            *status = 500;
            return claw_response_append(&resp, "scheduler task pause/resume failed");
        }
        *content_type = "application/json; charset=utf-8";
        return claw_response_append(&resp, "{\"ok\":true}");
    }

    *status = 404;
    return claw_response_append(&resp, "not found");
}

static void client_close(int epfd, http_client_t *c)
{
    if (!c) return;
    if (c->stream_src) {
        if (c->stream_src->fd >= 0) {
            epoll_ctl(epfd, EPOLL_CTL_DEL, c->stream_src->fd, NULL);
            close(c->stream_src->fd);
        }
        free(c->stream_src);
        c->stream_src = NULL;
    } else if (c->stream_fd >= 0) {
        close(c->stream_fd);
        c->stream_fd = -1;
    }
    if (c->fd >= 0) {
        epoll_ctl(epfd, EPOLL_CTL_DEL, c->fd, NULL);
        close(c->fd);
        c->fd = -1;
    }
    free(c);
}

static int client_update_events(int epfd, http_client_t *c, int want_out)
{
    struct epoll_event ev;
    if (!c) return -1;
    memset(&ev, 0, sizeof(ev));
    ev.events = EPOLLRDHUP | EPOLLHUP | EPOLLERR | (want_out ? EPOLLOUT : 0);
    if (!c->streaming) ev.events |= EPOLLIN;
    ev.data.ptr = c;
    return epoll_ctl(epfd, EPOLL_CTL_MOD, c->fd, &ev);
}

static int stream_prepare_headers(http_client_t *c)
{
    int n;
    if (!c) return -1;
    n = snprintf(c->sendbuf, sizeof(c->sendbuf),
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: text/event-stream; charset=utf-8\r\n"
        "Cache-Control: no-cache\r\n"
        "Connection: close\r\n\r\n");
    if (n < 0 || (size_t)n >= sizeof(c->sendbuf)) return -1;
    c->send_len = (size_t)n;
    c->send_off = 0;
    return 0;
}

static int client_begin_run_stream(int epfd, http_client_t *c,
    const claw_host_api_t *host, const char *req_body)
{
    int pipefd[2] = {-1, -1};
    pid_t pid;
    http_run_t *run;
    stream_source_t *src = NULL;
    if (!c || !host || !req_body) return -1;
    run = run_alloc_slot();
    if (!run) return -1;
    if (pipe2(pipefd, O_NONBLOCK | O_CLOEXEC) != 0) return -1;
    pid = fork();
    if (pid < 0) {
        close(pipefd[0]);
        close(pipefd[1]);
        return -1;
    }
    if (pid == 0) {
        char run_result[CLAW_HTTP_MAX_RESP];
        char event_json[1024];
        char output_json[CLAW_HTTP_MAX_RESP];
        claw_response_t out_resp;
        int http_status = 200;
        close(pipefd[0]);
        if (c->fd >= 0) close(c->fd);
        dprintf(pipefd[1], "event: run.started\ndata: {\"id\":%ld,\"status\":\"running\"}\n\n",
            run->id);
        if (execute_run_request(host, req_body, run, run_result, sizeof(run_result), &http_status) != 0) {
            _exit(1);
        }
        if (run_store_result(run, http_status, run_result) != 0) _exit(1);
        (void)persist_run_file(run);
        claw_response_init(&out_resp, output_json, sizeof(output_json));
        if (append_run_json(&out_resp, run) == 0)
            dprintf(pipefd[1], "event: run.output\ndata: %s\n\n", output_json);
        snprintf(event_json, sizeof(event_json),
            "{\"id\":%ld,\"status\":\"%s\",\"http_status\":%d}",
            run->id, run->status, run->http_status);
        dprintf(pipefd[1], "event: run.completed\ndata: %s\n\n", event_json);
        close(pipefd[1]);
        _exit(0);
    }
    close(pipefd[1]);
    src = (stream_source_t *)calloc(1, sizeof(*src));
    if (!src) {
        close(pipefd[0]);
        return -1;
    }
    c->streaming = 1;
    c->stream_fd = pipefd[0];
    c->stream_src = src;
    src->type = SRC_STREAM;
    src->fd = pipefd[0];
    src->client = c;
    if (stream_prepare_headers(c) != 0) return -1;
    {
        struct epoll_event ev;
        memset(&ev, 0, sizeof(ev));
        ev.events = EPOLLIN | EPOLLHUP | EPOLLERR;
        ev.data.ptr = src;
        if (epoll_ctl(epfd, EPOLL_CTL_ADD, src->fd, &ev) != 0) return -1;
    }
    if (client_update_events(epfd, c, 1) != 0) return -1;
    return 0;
}

static int client_prepare_error(http_client_t *c, int code, const char *msg)
{
    int n = send_response_build(c->sendbuf, sizeof(c->sendbuf), code, "text/plain; charset=utf-8", msg);
    if (n < 0) return -1;
    c->send_len = (size_t)n;
    c->send_off = 0;
    return 0;
}

static int client_request_complete(http_client_t *c)
{
    char *header_end;
    if (!c) return -1;
    header_end = strstr(c->reqbuf, "\r\n\r\n");
    if (!header_end) return 0;
    if (!c->headers_done) {
        char saved = *header_end;
        c->headers_done = 1;
        c->body_offset = (size_t)(header_end - c->reqbuf) + 4;
        *header_end = '\0';
        c->content_length = parse_content_length(c->reqbuf);
        *header_end = saved;
        if (c->content_length < 0 || c->content_length > CLAW_HTTP_MAX_REQ / 2) return -2;
    }
    return c->req_len >= c->body_offset + (size_t)c->content_length ? 1 : 0;
}

static int client_try_parse_request(http_client_t *c, char **method_out, char **path_out, char **body_out)
{
    char *header_end;
    char *dummy_method = NULL;
    char *dummy_path = NULL;
    char *dummy_body = NULL;
    if (!method_out) method_out = &dummy_method;
    if (!path_out) path_out = &dummy_path;
    if (!body_out) body_out = &dummy_body;

    header_end = strstr(c->reqbuf, "\r\n\r\n");
    if (!header_end) return 0;
    if (!c->headers_done) {
        c->headers_done = 1;
        c->body_offset = (size_t)(header_end - c->reqbuf) + 4;
        *header_end = '\0';
        c->content_length = parse_content_length(c->reqbuf);
        if (c->content_length < 0 || c->content_length > CLAW_HTTP_MAX_REQ / 2) return -2;
        header_end[0] = '\r';
    }
    if (c->req_len < c->body_offset + (size_t)c->content_length) return 0;

    c->reqbuf[c->req_len] = '\0';
    header_end = strstr(c->reqbuf, "\r\n\r\n");
    if (!header_end) return -3;
    *header_end = '\0';
    *body_out = header_end + 4;
    (*body_out)[c->content_length] = '\0';
    *method_out = c->reqbuf;
    *path_out = strchr(c->reqbuf, ' ');
    if (!*path_out) return -4;
    **path_out = '\0';
    ++(*path_out);
    {
        char *path_end = strchr(*path_out, ' ');
        if (!path_end) return -5;
        *path_end = '\0';
    }
    return 1;
}

static int client_build_app_response(int epfd, http_client_t *c, const claw_host_api_t *host)
{
    char body[CLAW_HTTP_MAX_RESP];
    char *method = NULL, *path = NULL, *req_body = NULL;
    int status = 500;
    const char *content_type = "text/plain; charset=utf-8";
    int rc;
    int n;

    body[0] = '\0';
    rc = client_try_parse_request(c, &method, &path, &req_body);
    if (rc <= 0) {
        return client_prepare_error(c, rc == -2 ? 413 : 400, rc == -2 ? "payload too large" : "bad request");
    }
    if (strcmp(method, "POST") == 0 && strcmp(path, "/v1/runs/stream") == 0) {
        return client_begin_run_stream(epfd, c, host, req_body);
    }
    rc = handle_request(host, method, path, req_body, body, sizeof(body), &status, &content_type);
    if (rc != 0 && body[0] == '\0') {
        strcpy(body, "internal error");
        status = 500;
        content_type = "text/plain; charset=utf-8";
    }
    n = send_response_build(c->sendbuf, sizeof(c->sendbuf), status, content_type, body);
    if (n < 0) return client_prepare_error(c, 500, "internal error");
    c->send_len = (size_t)n;
    c->send_off = 0;
    return 0;
}

static int client_handle_read(int epfd, http_client_t *c, const claw_host_api_t *host)
{
    for (;;) {
        ssize_t n = recv(c->fd, c->reqbuf + c->req_len, sizeof(c->reqbuf) - c->req_len - 1, 0);
        if (n > 0) {
            c->req_len += (size_t)n;
            c->reqbuf[c->req_len] = '\0';
            if (c->req_len + 1 >= sizeof(c->reqbuf)) {
                client_prepare_error(c, 413, "request too large");
                break;
            }
            {
                int ready = client_request_complete(c);
                if (ready == 1) break;
                if (ready < 0) {
                    client_prepare_error(c, ready == -2 ? 413 : 400, ready == -2 ? "payload too large" : "bad request");
                    break;
                }
            }
            continue;
        }
        if (n == 0) return -1;
        if (errno == EINTR) continue;
        if (errno == EAGAIN || errno == EWOULDBLOCK) return 0;
        return -1;
    }

    if (client_build_app_response(epfd, c, host) != 0) return -1;
    if (client_update_events(epfd, c, 1) != 0) return -1;
    return 0;
}

static int client_handle_write(http_client_t *c)
{
    while (c->send_off < c->send_len) {
        ssize_t n = send(c->fd, c->sendbuf + c->send_off, c->send_len - c->send_off, 0);
        if (n > 0) {
            c->send_off += (size_t)n;
            continue;
        }
        if (n < 0 && errno == EINTR) continue;
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) return 0;
        return -1;
    }
    c->send_len = 0;
    c->send_off = 0;
    if (c->streaming) {
        if (c->stream_eof) return 1;
        return 0;
    }
    return 1;
}

static int listener_accept_loop(int epfd, int listen_fd, int max_clients, int *client_count)
{
    while (*client_count < max_clients) {
        http_client_t *c;
        struct epoll_event ev;
        int client_fd = accept4(listen_fd, NULL, NULL, SOCK_NONBLOCK | SOCK_CLOEXEC);
        if (client_fd < 0) {
            if (errno == EINTR) continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK) return 0;
            return -1;
        }
        c = (http_client_t *)calloc(1, sizeof(*c));
        if (!c) {
            close(client_fd);
            return -1;
        }
        c->type = SRC_CLIENT;
        c->fd = client_fd;
        c->stream_fd = -1;
        memset(&ev, 0, sizeof(ev));
        ev.events = EPOLLIN | EPOLLRDHUP | EPOLLHUP | EPOLLERR;
        ev.data.ptr = c;
        if (epoll_ctl(epfd, EPOLL_CTL_ADD, client_fd, &ev) != 0) {
            close(client_fd);
            free(c);
            return -1;
        }
        ++(*client_count);
    }
    return 0;
}

static int http_channel_serve(const claw_host_api_t *host)
{
    int server_fd = -1;
    int epfd = -1;
    int signal_fd = -1;
    int timer_fd = -1;
    int client_count = 0;
    int max_clients = env_int_or_default("CLAW_HTTP_MAX_CLIENTS", CLAW_HTTP_DEFAULT_MAX_CLIENTS);
    struct sockaddr_in addr;
    const char *bind_ip = env_or_default("CLAW_HTTP_BIND", CLAW_HTTP_DEFAULT_BIND);
    int port = env_port_or_default("CLAW_HTTP_PORT", CLAW_HTTP_DEFAULT_PORT);
    sigset_t mask;
    event_source_t listener_src = { SRC_LISTENER, -1 };
    event_source_t signal_src = { SRC_SIGNAL, -1 };
    event_source_t timer_src = { SRC_TIMER, -1 };

    if (!host) return -1;

    server_fd = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (server_fd < 0) return -2;
    {
        int opt = 1;
        setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    }

    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons((unsigned short)port);
    if (inet_pton(AF_INET, bind_ip, &addr.sin_addr) != 1) {
        close(server_fd);
        return -3;
    }
    if (bind(server_fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        perror("bind");
        close(server_fd);
        return -4;
    }
    if (listen(server_fd, 128) != 0) {
        perror("listen");
        close(server_fd);
        return -5;
    }

    sigemptyset(&mask);
    sigaddset(&mask, SIGINT);
    sigaddset(&mask, SIGTERM);
    signal(SIGCHLD, SIG_IGN);
    if (sigprocmask(SIG_BLOCK, &mask, NULL) != 0) {
        close(server_fd);
        return -6;
    }
    signal_fd = signalfd(-1, &mask, SFD_NONBLOCK | SFD_CLOEXEC);
    if (signal_fd < 0) {
        close(server_fd);
        return -7;
    }
    timer_fd = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);
    if (timer_fd < 0) {
        close(signal_fd);
        close(server_fd);
        return -7;
    }
    {
        struct itimerspec its;
        memset(&its, 0, sizeof(its));
        its.it_value.tv_sec = 1;
        its.it_interval.tv_sec = 1;
        if (timerfd_settime(timer_fd, 0, &its, NULL) != 0) {
            close(timer_fd);
            close(signal_fd);
            close(server_fd);
            return -7;
        }
    }

    epfd = epoll_create1(EPOLL_CLOEXEC);
    if (epfd < 0) {
        close(timer_fd);
        close(signal_fd);
        close(server_fd);
        return -8;
    }

    listener_src.fd = server_fd;
    signal_src.fd = signal_fd;
    timer_src.fd = timer_fd;
    {
        struct epoll_event ev;
        memset(&ev, 0, sizeof(ev));
        ev.events = EPOLLIN;
        ev.data.ptr = &listener_src;
        if (epoll_ctl(epfd, EPOLL_CTL_ADD, server_fd, &ev) != 0) goto fail;
        memset(&ev, 0, sizeof(ev));
        ev.events = EPOLLIN;
        ev.data.ptr = &signal_src;
        if (epoll_ctl(epfd, EPOLL_CTL_ADD, signal_fd, &ev) != 0) goto fail;
        memset(&ev, 0, sizeof(ev));
        ev.events = EPOLLIN;
        ev.data.ptr = &timer_src;
        if (epoll_ctl(epfd, EPOLL_CTL_ADD, timer_fd, &ev) != 0) goto fail;
    }

    fprintf(stderr, "http runtime(epoll+scheduler) listening on http://%s:%d\n", bind_ip, port);
    while (!g_stop) {
        struct epoll_event events[CLAW_HTTP_MAX_EVENTS];
        int n = epoll_wait(epfd, events, CLAW_HTTP_MAX_EVENTS, -1);
        if (n < 0) {
            if (errno == EINTR) continue;
            perror("epoll_wait");
            goto fail;
        }
        for (int i = 0; i < n; ++i) {
            event_source_t *src = (event_source_t *)events[i].data.ptr;
            if (!src) continue;
            if (src->type == SRC_SIGNAL) {
                struct signalfd_siginfo si;
                while (read(signal_fd, &si, sizeof(si)) == (ssize_t)sizeof(si)) {
                    g_stop = 1;
                }
                continue;
            }
            if (src->type == SRC_TIMER) {
                uint64_t expirations = 0;
                while (read(timer_fd, &expirations, sizeof(expirations)) == (ssize_t)sizeof(expirations)) { }
                if (host->scheduler_tick) (void)host->scheduler_tick();
                continue;
            }
            if (src->type == SRC_LISTENER) {
                if (listener_accept_loop(epfd, server_fd, max_clients, &client_count) != 0) goto fail;
                continue;
            }
            if (src->type == SRC_CLIENT) {
                http_client_t *c = (http_client_t *)src;
                int rc;
                if (events[i].events & (EPOLLRDHUP | EPOLLHUP | EPOLLERR)) {
                    client_close(epfd, c);
                    --client_count;
                    continue;
                }
                if (events[i].events & EPOLLIN) {
                    rc = client_handle_read(epfd, c, host);
                    if (rc != 0) {
                        client_close(epfd, c);
                        --client_count;
                        continue;
                    }
                }
                if (events[i].events & EPOLLOUT) {
                    rc = client_handle_write(c);
                    if (rc != 0) {
                        client_close(epfd, c);
                        --client_count;
                        continue;
                    }
                    if (client_update_events(epfd, c, 0) != 0) {
                        client_close(epfd, c);
                        --client_count;
                        continue;
                    }
                }
                continue;
            }
            if (src->type == SRC_STREAM) {
                stream_source_t *ss = (stream_source_t *)src;
                http_client_t *c = ss->client;
                ssize_t m;
                if (!c) continue;
                if (c->send_len != 0) continue;
                m = read(ss->fd, c->sendbuf, sizeof(c->sendbuf));
                if (m > 0) {
                    c->send_len = (size_t)m;
                    c->send_off = 0;
                    if (client_update_events(epfd, c, 1) != 0) {
                        client_close(epfd, c);
                        --client_count;
                    }
                    continue;
                }
                if (m == 0) {
                    c->stream_eof = 1;
                    epoll_ctl(epfd, EPOLL_CTL_DEL, ss->fd, NULL);
                    close(ss->fd);
                    ss->fd = -1;
                    free(ss);
                    c->stream_src = NULL;
                    c->stream_fd = -1;
                    if (c->send_len == 0) {
                        client_close(epfd, c);
                        --client_count;
                    }
                    continue;
                }
                if (m < 0 && !(errno == EAGAIN || errno == EWOULDBLOCK)) {
                    client_close(epfd, c);
                    --client_count;
                }
            }
        }
    }

    close(epfd);
    close(timer_fd);
    close(signal_fd);
    close(server_fd);
    return 0;

fail:
    if (epfd >= 0) close(epfd);
    if (timer_fd >= 0) close(timer_fd);
    if (signal_fd >= 0) close(signal_fd);
    if (server_fd >= 0) close(server_fd);
    return -9;
}

static int http_channel_stop(void)
{
    g_stop = 1;
    return 0;
}

static const claw_channel_api_t HTTP_CHANNEL = {
    .name = "http",
    .serve = http_channel_serve,
    .stop = http_channel_stop,
};

static const claw_module_descriptor_t HTTP_DESC = {
    .abi_version = CLAW_PLUGIN_ABI_VERSION,
    .kind = CLAW_MOD_CHANNEL,
    .api.channel = &HTTP_CHANNEL,
};

__attribute__((visibility("default")))
const claw_module_descriptor_t *claw_plugin_init(void)
{
    return &HTTP_DESC;
}
