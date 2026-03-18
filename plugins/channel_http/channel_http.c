#define _GNU_SOURCE
#include "claw/plugin_api.h"

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
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#define CLAW_HTTP_DEFAULT_BIND "127.0.0.1"
#define CLAW_HTTP_DEFAULT_PORT 8080
#define CLAW_HTTP_MAX_REQ 65536
#define CLAW_HTTP_MAX_RESP 65536
#define CLAW_HTTP_MAX_EVENTS 64
#define CLAW_HTTP_DEFAULT_MAX_CLIENTS 64

enum source_type {
    SRC_LISTENER = 1,
    SRC_SIGNAL   = 2,
    SRC_CLIENT   = 3
};

typedef struct {
    int type;
    int fd;
} event_source_t;

typedef struct {
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
} http_client_t;

static volatile sig_atomic_t g_stop = 0;

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
    case 404: reason = "Not Found"; break;
    case 405: reason = "Method Not Allowed"; break;
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

static int build_shell_json_args(const char *body, char *out, size_t out_cap)
{
    char command[4096];
    char cwd[1024];
    long timeout_ms;
    claw_response_t resp;
    int have_cwd = 0;
    int have_timeout = 0;

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

    if (strcmp(method, "POST") != 0) {
        *status = 405;
        return claw_response_append(&resp, "method not allowed");
    }
    if (!body) body = "{}";

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

    if (strcmp(path, "/v1/tool/invoke") == 0) {
        if (json_get_string(body, "tool", tool, sizeof(tool)) != 0 ||
            json_get_string(body, "json_args", json_args, sizeof(json_args)) != 0) {
            *status = 400;
            return claw_response_append(&resp, "missing tool or json_args");
        }
        rc = host->tool_invoke(tool, json_args, &resp);
        if (rc != 0) {
            *status = 500;
            if (resp.len == 0) return claw_response_append(&resp, "tool invoke failed");
        }
        return 0;
    }

    if (strcmp(path, "/v1/tool/shell") == 0) {
        if (build_shell_json_args(body, json_args, sizeof(json_args)) != 0) {
            *status = 400;
            return claw_response_append(&resp, "missing command or invalid tool body");
        }
        rc = host->tool_invoke("shell", json_args, &resp);
        if (rc != 0) {
            *status = 500;
            if (resp.len == 0) return claw_response_append(&resp, "shell tool failed");
        }
        return 0;
    }

    *status = 404;
    return claw_response_append(&resp, "not found");
}

static void client_close(int epfd, http_client_t *c)
{
    if (!c) return;
    if (c->fd >= 0) {
        epoll_ctl(epfd, EPOLL_CTL_DEL, c->fd, NULL);
        close(c->fd);
        c->fd = -1;
    }
    free(c);
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

static int client_build_app_response(http_client_t *c, const claw_host_api_t *host)
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

    if (client_build_app_response(c, host) != 0) return -1;
    {
        struct epoll_event ev;
        memset(&ev, 0, sizeof(ev));
        ev.events = EPOLLOUT | EPOLLRDHUP | EPOLLHUP | EPOLLERR;
        ev.data.ptr = c;
        if (epoll_ctl(epfd, EPOLL_CTL_MOD, c->fd, &ev) != 0) return -1;
    }
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
    int client_count = 0;
    int max_clients = env_int_or_default("CLAW_HTTP_MAX_CLIENTS", CLAW_HTTP_DEFAULT_MAX_CLIENTS);
    struct sockaddr_in addr;
    const char *bind_ip = env_or_default("CLAW_HTTP_BIND", CLAW_HTTP_DEFAULT_BIND);
    int port = env_port_or_default("CLAW_HTTP_PORT", CLAW_HTTP_DEFAULT_PORT);
    sigset_t mask;
    event_source_t listener_src = { SRC_LISTENER, -1 };
    event_source_t signal_src = { SRC_SIGNAL, -1 };

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
    if (sigprocmask(SIG_BLOCK, &mask, NULL) != 0) {
        close(server_fd);
        return -6;
    }
    signal_fd = signalfd(-1, &mask, SFD_NONBLOCK | SFD_CLOEXEC);
    if (signal_fd < 0) {
        close(server_fd);
        return -7;
    }

    epfd = epoll_create1(EPOLL_CLOEXEC);
    if (epfd < 0) {
        close(signal_fd);
        close(server_fd);
        return -8;
    }

    listener_src.fd = server_fd;
    signal_src.fd = signal_fd;
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
    }

    fprintf(stderr, "http channel(epoll) listening on http://%s:%d\n", bind_ip, port);
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
                }
            }
        }
    }

    close(epfd);
    close(signal_fd);
    close(server_fd);
    return 0;

fail:
    if (epfd >= 0) close(epfd);
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
