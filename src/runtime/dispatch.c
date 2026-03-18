#include "claw/dispatch.h"
#include "claw/tool_types.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static uint64_t monotonic_ms(void)
{
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) return 0;
    return (uint64_t)ts.tv_sec * 1000ULL + (uint64_t)ts.tv_nsec / 1000000ULL;
}

void claw_dispatch_init(claw_dispatch_t *d, claw_registry_t *registry,
    claw_metrics_t *metrics, claw_trace_t *trace)
{
    if (!d) return;
    d->registry = registry;
    d->metrics = metrics;
    d->trace = trace;
}

static void record_dispatch(claw_dispatch_t *d, claw_op_kind_t op, const char *target,
    int rc, uint64_t started_ms, size_t response_bytes)
{
    uint64_t duration_ms = monotonic_ms() - started_ms;
    if (!d) return;
    claw_metrics_record_dispatch(d->metrics, op, rc, duration_ms);
    (void)claw_trace_write_dispatch(d->trace, d->metrics, op, target ? target : "", rc, duration_ms, response_bytes);
}

static int append_json_quoted(claw_response_t *resp, const char *s)
{
    const unsigned char *p;
    char tmp[7];
    if (claw_response_append(resp, "\"") != 0) return -1;
    if (!s) s = "";
    for (p = (const unsigned char *)s; *p; ++p) {
        switch (*p) {
        case '"': if (claw_response_append(resp, "\\\"") != 0) return -1; break;
        case '\\': if (claw_response_append(resp, "\\\\") != 0) return -1; break;
        case '\b': if (claw_response_append(resp, "\\b") != 0) return -1; break;
        case '\f': if (claw_response_append(resp, "\\f") != 0) return -1; break;
        case '\n': if (claw_response_append(resp, "\\n") != 0) return -1; break;
        case '\r': if (claw_response_append(resp, "\\r") != 0) return -1; break;
        case '\t': if (claw_response_append(resp, "\\t") != 0) return -1; break;
        default:
            if (*p < 0x20) {
                snprintf(tmp, sizeof(tmp), "\\u%04x", (unsigned int)*p);
                if (claw_response_append(resp, tmp) != 0) return -1;
            } else if (claw_response_append_mem(resp, (const char *)p, 1) != 0) return -1;
        }
    }
    return claw_response_append(resp, "\"");
}

static int append_json_array_names(claw_response_t *resp, const char *key, const char *const *names, size_t n)
{
    size_t i;
    if (claw_response_append(resp, "\"") != 0 ||
        claw_response_append(resp, key) != 0 ||
        claw_response_append(resp, "\":[") != 0) {
        return -1;
    }
    for (i = 0; i < n; ++i) {
        if (i != 0 && claw_response_append(resp, ",") != 0) return -1;
        if (append_json_quoted(resp, names[i] ? names[i] : "") != 0) return -1;
    }
    if (claw_response_append(resp, "]") != 0) return -1;
    return 0;
}


static int tool_error_json(claw_response_t *resp, const char *tool, const char *code, const char *field, const char *message)
{
    if (!resp) return -1;
    if (claw_response_append(resp, "{\"ok\":false,\"tool\":") != 0) return -1;
    if (append_json_quoted(resp, tool ? tool : "") != 0) return -1;
    if (claw_response_append(resp, ",\"error\":{\"code\":") != 0) return -1;
    if (append_json_quoted(resp, code ? code : "error") != 0) return -1;
    if (claw_response_append(resp, ",\"field\":") != 0) return -1;
    if (field && *field) {
        if (append_json_quoted(resp, field) != 0) return -1;
    } else if (claw_response_append(resp, "null") != 0) return -1;
    if (claw_response_append(resp, ",\"message\":") != 0) return -1;
    if (append_json_quoted(resp, message ? message : "") != 0) return -1;
    return claw_response_append(resp, "}}");
}

static const char *tool_http_method(const char *tool_name)
{
    (void)tool_name;
    return "POST";
}

static const char *tool_http_path(const char *tool_name)
{
    if (!tool_name) return "/v1/tool/invoke";
    if (strcmp(tool_name, "exec") == 0) return "/v1/tools/exec";
    if (strcmp(tool_name, "fs.read") == 0) return "/v1/fs/read";
    if (strcmp(tool_name, "fs.write") == 0) return "/v1/fs/write";
    if (strcmp(tool_name, "fs.list") == 0) return "/v1/fs/list";
    if (strcmp(tool_name, "shell") == 0) return "/v1/tool/shell";
    return "/v1/tool/invoke";
}

static const char *tool_help_text(const char *tool_name) {
  if (!tool_name)
    return "typed tool";
  if (strcmp(tool_name, "exec") == 0)
    return "Execute a program without shell expansion using argv, with "
           "optional env, cwd, and timeout_ms.";
  if (strcmp(tool_name, "fs.read") == 0)
    return "Read a file under allowed roots with optional max_bytes limit.";
  if (strcmp(tool_name, "fs.write") == 0)
    return "Write a file under allowed roots using overwrite or append mode.";
  if (strcmp(tool_name, "fs.list") == 0)
    return "List a directory under allowed roots with optional max_entries "
           "limit.";
  if (strcmp(tool_name, "shell") == 0)
    return "Execute a shell command or argv payload under stricter policy "
           "controls. Prefer exec when shell features are not needed.";
  return "typed tool";
}

static const char *tool_examples_json(const char *tool_name) {
  if (!tool_name)
    return "[]";
  if (strcmp(tool_name, "exec") == 0)
    return "[{\"title\":\"basic\",\"request\":{\"argv\":[\"printf\",\"hello\"],"
           "\"cwd\":\".\"}},{\"title\":\"with_env\",\"request\":{\"argv\":[\"/"
           "usr/bin/env\"],\"env\":{\"FOO\":\"BAR\"},\"cwd\":\".\"}}]";
  if (strcmp(tool_name, "fs.read") == 0)
    return "[{\"title\":\"read\",\"request\":{\"path\":\"./"
           "README.md\",\"max_bytes\":4096}}]";
  if (strcmp(tool_name, "fs.write") == 0)
    return "[{\"title\":\"overwrite\",\"request\":{\"path\":\"./"
           "out.txt\",\"content\":\"hello\",\"mode\":\"overwrite\"}},{"
           "\"title\":\"append\",\"request\":{\"path\":\"./"
           "out.txt\",\"content\":\"more\",\"mode\":\"append\"}}]";
  if (strcmp(tool_name, "fs.list") == 0)
    return "[{\"title\":\"list\",\"request\":{\"path\":\".\",\"max_entries\":"
           "20}}]";
  if (strcmp(tool_name, "shell") == 0)
    return "[{\"title\":\"command\",\"request\":{\"command\":\"printf "
           "hello\",\"cwd\":\".\"}},{\"title\":\"argv\",\"request\":{\"argv\":["
           "\"printf\",\"hello\"],\"cwd\":\".\"}}]";
  return "[]";
}

static const char *tool_error_codes_json(const char *tool_name) {
  if (!tool_name)
    return "[]";
  if (strcmp(tool_name, "exec") == 0)
    return "[{\"code\":\"schema_validation\",\"http_status\":400,\"meaning\":"
           "\"request body failed schema "
           "validation\"},{\"code\":\"permission_denied\",\"http_status\":403,"
           "\"meaning\":\"cwd rejected by allowed-roots "
           "policy\"},{\"code\":\"command_rejected\",\"http_status\":403,"
           "\"meaning\":\"argv[0] rejected by allowlist or "
           "denylist\"},{\"code\":\"execution_timeout\",\"http_status\":408,"
           "\"meaning\":\"process exceeded "
           "timeout_ms\"},{\"code\":\"payload_too_large\",\"http_status\":413,"
           "\"meaning\":\"captured output exceeded response limits\"}]";
  if (strcmp(tool_name, "fs.read") == 0 || strcmp(tool_name, "fs.write") == 0 ||
      strcmp(tool_name, "fs.list") == 0)
    return "[{\"code\":\"schema_validation\",\"http_status\":400,\"meaning\":"
           "\"request body failed schema "
           "validation\"},{\"code\":\"path_rejected\",\"http_status\":403,"
           "\"meaning\":\"path rejected by allowed-roots "
           "policy\"},{\"code\":\"not_found\",\"http_status\":404,\"meaning\":"
           "\"path does not "
           "exist\"},{\"code\":\"payload_too_large\",\"http_status\":413,"
           "\"meaning\":\"result exceeded response limits\"}]";
  if (strcmp(tool_name, "shell") == 0)
    return "[{\"code\":\"schema_validation\",\"http_status\":400,\"meaning\":"
           "\"request body failed schema "
           "validation\"},{\"code\":\"command_rejected\",\"http_status\":403,"
           "\"meaning\":\"command rejected by "
           "policy\"},{\"code\":\"execution_timeout\",\"http_status\":408,"
           "\"meaning\":\"process exceeded timeout_ms\"}]";
  return "[]";
}

static int append_tool_http_doc(claw_response_t *resp,
                                const claw_tool_api_t *tool) {
  if (!resp || !tool)
    return -1;
  if (claw_response_append(resp, ",\"http\":{\"method\":") != 0)
    return -1;
  if (append_json_quoted(resp, tool_http_method(tool->name)) != 0)
    return -1;
  if (claw_response_append(resp, ",\"path\":") != 0)
    return -1;
  if (append_json_quoted(resp, tool_http_path(tool->name)) != 0)
    return -1;
  if (claw_response_append(resp, ",\"help\":") != 0)
    return -1;
  if (append_json_quoted(resp, tool_help_text(tool->name)) != 0)
    return -1;
  if (claw_response_append(resp, ",\"examples\":") != 0)
    return -1;
  if (claw_response_append(resp, tool_examples_json(tool->name)) != 0)
    return -1;
  if (claw_response_append(resp, ",\"error_codes\":") != 0)
    return -1;
  if (claw_response_append(resp, tool_error_codes_json(tool->name)) != 0)
    return -1;
  return claw_response_append(resp, "}");
}

static int json_has_top_level_field(const char *json, const char *field, char *type_out, size_t type_cap)
{
    char key[128];
    const char *p;
    if (!json || !field) return 0;
    if (snprintf(key, sizeof(key), "\"%s\"", field) >= (int)sizeof(key)) return 0;
    p = strstr(json, key);
    if (!p) return 0;
    p = strchr(p, ':');
    if (!p) return 0;
    p = claw_tt_skip_ws(p + 1);
    if (type_out && type_cap) {
        const char *t = "unknown";
        if (*p == '"') t = "string";
        else if (*p == '{') t = "object";
        else if (*p == '[') t = "array";
        else if (*p == 't' || *p == 'f') t = "boolean";
        else if (*p == 'n') t = "null";
        else if ((*p >= '0' && *p <= '9') || *p == '-') t = "integer";
        snprintf(type_out, type_cap, "%s", t);
    }
    return 1;
}

static int schema_extract_required(const char *schema, char names[][64], size_t max_names, size_t *count_out)
{
    const char *p, *end;
    size_t n = 0;
    if (count_out) *count_out = 0;
    if (!schema || !count_out) return -1;
    p = strstr(schema, "\"required\":[");
    if (!p) return 0;
    p = strchr(p, '[');
    if (!p) return -1;
    end = strchr(p, ']');
    if (!end) return -1;
    ++p;
    while (p < end && n < max_names) {
        const char *q1 = strchr(p, '"');
        const char *q2;
        if (!q1 || q1 >= end) break;
        q2 = strchr(q1 + 1, '"');
        if (!q2 || q2 > end) break;
        snprintf(names[n], 64, "%.*s", (int)(q2 - (q1 + 1)), q1 + 1);
        ++n;
        p = q2 + 1;
    }
    *count_out = n;
    return 0;
}

static int schema_extract_property_type(const char *schema, const char *field, char *type_out, size_t cap)
{
    char pat1[128], pat2[128];
    const char *p;
    if (!schema || !field || !type_out || cap == 0) return -1;
    snprintf(pat1, sizeof(pat1), "\"%s\":{\"type\":\"", field);
    snprintf(pat2, sizeof(pat2), "\"%s\":{", field);
    p = strstr(schema, pat1);
    if (!p) {
        p = strstr(schema, pat2);
        if (!p) return -2;
        p = strstr(p, "\"type\":\"");
        if (!p) return -2;
        p += strlen("\"type\":\"");
    } else p += strlen(pat1);
    {
        const char *q = strchr(p, '"');
        if (!q) return -3;
        snprintf(type_out, cap, "%.*s", (int)(q - p), p);
    }
    return 0;
}

static int schema_validate_request(const claw_tool_api_t *tool, const char *json_args, claw_response_t *resp)
{
    char req_names[16][64], actual_type[32], expect_type[32];
    size_t nreq = 0, i;
    const char *schema;
    if (!tool || !json_args || !resp) return -1;
    schema = tool->request_schema_json;
    if (!schema || !*schema) return 0;
    if (schema_extract_required(schema, req_names, 16, &nreq) != 0) return 0;
    for (i = 0; i < nreq; ++i) {
        if (!json_has_top_level_field(json_args, req_names[i], NULL, 0)) {
            char msg[160];
            snprintf(msg, sizeof(msg), "field is required");
            (void)tool_error_json(resp, tool->name, "schema_validation", req_names[i], msg); return -10;
        }
    }
    for (i = 0; i < nreq; ++i) {
        if (schema_extract_property_type(schema, req_names[i], expect_type, sizeof(expect_type)) != 0) continue;
        if (!json_has_top_level_field(json_args, req_names[i], actual_type, sizeof(actual_type))) continue;
        if (strcmp(expect_type, actual_type) != 0) {
            char msg[192];
            snprintf(msg, sizeof(msg), "field must match declared schema type");
            (void)tool_error_json(resp, tool->name, "schema_validation", req_names[i], msg); return -10;
        }
    }
    if (strstr(schema, "\"additionalProperties\":false")) {
        const char *p = json_args;
        int depth = 0, in_str = 0, esc = 0;
        while (*p) {
            char c = *p;
            if (in_str) {
                if (esc) esc = 0;
                else if (c == '\\') esc = 1;
                else if (c == '"') in_str = 0;
                ++p; continue;
            }
            if (c == '"') {
                const char *q = strchr(p + 1, '"');
                if (depth == 1 && q) {
                    char field[64], t[32];
                    const char *colon = q + 1;
                    snprintf(field, sizeof(field), "%.*s", (int)(q - (p + 1)), p + 1);
                    while (*colon == ' ' || *colon == '\t' || *colon == '\r' || *colon == '\n') ++colon;
                    if (*colon == ':') {
                        if (schema_extract_property_type(schema, field, t, sizeof(t)) != 0) {
                            char msg[192];
                            snprintf(msg, sizeof(msg), "%s is not allowed by schema", field);
                            (void)tool_error_json(resp, tool->name, "schema_validation", field, msg); return -10;
                        }
                    }
                }
                in_str = 1; ++p; continue;
            }
            if (c == '{') depth++;
            else if (c == '}') depth--;
            ++p;
        }
    }
    return 0;
}


static int schema_extract_int_range(const char *schema, const char *field, long *min_out, int *has_min, long *max_out, int *has_max)
{
    char pat[128];
    const char *p, *obj_end, *q;
    if (min_out) *min_out = 0;
    if (max_out) *max_out = 0;
    if (has_min) *has_min = 0;
    if (has_max) *has_max = 0;
    snprintf(pat, sizeof(pat), "\"%s\":{", field);
    p = strstr(schema, pat);
    if (!p) return -1;
    obj_end = strchr(p, '}');
    if (!obj_end) return -1;
    q = strstr(p, "\"minimum\":");
    if (q && q < obj_end) {
        if (min_out) *min_out = strtol(q + strlen("\"minimum\":"), NULL, 10);
        if (has_min) *has_min = 1;
    }
    q = strstr(p, "\"maximum\":");
    if (q && q < obj_end) {
        if (max_out) *max_out = strtol(q + strlen("\"maximum\":"), NULL, 10);
        if (has_max) *has_max = 1;
    }
    return 0;
}

static int schema_extract_enum_strings(const char *schema, const char *field, char vals[][64], size_t max_vals, size_t *count_out)
{
    char pat[128];
    const char *p, *end;
    size_t n = 0;
    if (count_out) *count_out = 0;
    snprintf(pat, sizeof(pat), "\"%s\":{", field);
    p = strstr(schema, pat);
    if (!p) return -1;
    p = strstr(p, "\"enum\":[");
    if (!p) return -1;
    p = strchr(p, '[');
    if (!p) return -1;
    end = strchr(p, ']');
    if (!end) return -1;
    ++p;
    while (p < end && n < max_vals) {
        const char *q1 = strchr(p, '"');
        const char *q2;
        if (!q1 || q1 >= end) break;
        q2 = strchr(q1 + 1, '"');
        if (!q2 || q2 > end) break;
        snprintf(vals[n], 64, "%.*s", (int)(q2 - (q1 + 1)), q1 + 1);
        ++n;
        p = q2 + 1;
    }
    if (count_out) *count_out = n;
    return n ? 0 : -1;
}

static int schema_extract_array_limits(const char *schema, const char *field, long *min_items, int *has_min, long *max_items, int *has_max, int *items_string)
{
    char pat[128];
    const char *p, *obj_end, *q;
    if (min_items) *min_items = 0;
    if (max_items) *max_items = 0;
    if (has_min) *has_min = 0;
    if (has_max) *has_max = 0;
    if (items_string) *items_string = 0;
    snprintf(pat, sizeof(pat), "\"%s\":{", field);
    p = strstr(schema, pat);
    if (!p) return -1;
    obj_end = strchr(p, '}');
    if (!obj_end) return -1;
    q = strstr(p, "\"minItems\":");
    if (q && q < obj_end) {
        if (min_items) *min_items = strtol(q + strlen("\"minItems\":"), NULL, 10);
        if (has_min) *has_min = 1;
    }
    q = strstr(p, "\"maxItems\":");
    if (q && q < obj_end) {
        if (max_items) *max_items = strtol(q + strlen("\"maxItems\":"), NULL, 10);
        if (has_max) *has_max = 1;
    }
    q = strstr(p, "\"items\":{\"type\":\"string\"}");
    if (q && q < obj_end) {
        if (items_string) *items_string = 1;
    }
    return 0;
}

static size_t json_count_top_level_array_items(const char *json, const char *field)
{
    const char *p = claw_tt_find_json_array_field(json, field);
    size_t count = 0;
    int in_str = 0, esc = 0, depth = 0, saw_value = 0;
    if (!p) return 0;
    ++p;
    p = claw_tt_skip_ws(p);
    if (*p == ']') return 0;
    for (; *p; ++p) {
        char c = *p;
        if (in_str) {
            if (esc) esc = 0;
            else if (c == '\\') esc = 1;
            else if (c == '"') in_str = 0;
            saw_value = 1;
            continue;
        }
        if (c == '"') { in_str = 1; saw_value = 1; continue; }
        if (c == '[' || c == '{') { depth++; saw_value = 1; continue; }
        if (c == ']' && depth == 0) { if (saw_value) count++; break; }
        if ((c == ']' || c == '}') && depth > 0) { depth--; continue; }
        if (c == ',' && depth == 0) { count++; saw_value = 0; continue; }
        if (c != ' ' && c != '\t' && c != '\r' && c != '\n') saw_value = 1;
    }
    return count;
}

static int validate_schema_details(const claw_tool_api_t *tool, const char *json_args, claw_response_t *resp)
{
    char req_names[16][64], actual_type[32], expect_type[32], sval[1024], enums[16][64];
    size_t nreq = 0, i, ec = 0;
    const char *schema;
    long v, minv, maxv;
    int has_min, has_max, items_string;
    size_t acount;
    if (!tool || !json_args || !resp) return -1;
    schema = tool->request_schema_json;
    if (!schema || !*schema) return 0;
    if (schema_extract_required(schema, req_names, 16, &nreq) != 0) return 0;
    for (i = 0; i < nreq; ++i) {
        if (schema_extract_property_type(schema, req_names[i], expect_type, sizeof(expect_type)) != 0) continue;
        if (!json_has_top_level_field(json_args, req_names[i], actual_type, sizeof(actual_type))) continue;
        if (strcmp(expect_type, "integer") == 0 && claw_tt_json_get_long(json_args, req_names[i], &v) == 0) {
            schema_extract_int_range(schema, req_names[i], &minv, &has_min, &maxv, &has_max);
            if (has_min && v < minv) return tool_error_json(resp, tool->name, "schema_validation", req_names[i], "integer below minimum"), -10;
            if (has_max && v > maxv) return tool_error_json(resp, tool->name, "schema_validation", req_names[i], "integer above maximum"), -10;
        } else if (strcmp(expect_type, "string") == 0 && claw_tt_json_get_string(json_args, req_names[i], sval, sizeof(sval)) == 0) {
            if (schema_extract_enum_strings(schema, req_names[i], enums, 16, &ec) == 0 && ec > 0) {
                size_t j; int found = 0;
                for (j = 0; j < ec; ++j) if (strcmp(sval, enums[j]) == 0) { found = 1; break; }
                if (!found) return tool_error_json(resp, tool->name, "schema_validation", req_names[i], "string must be one of enum values"), -10;
            }
        } else if (strcmp(expect_type, "array") == 0) {
            schema_extract_array_limits(schema, req_names[i], &minv, &has_min, &maxv, &has_max, &items_string);
            acount = json_count_top_level_array_items(json_args, req_names[i]);
            if (has_min && (long)acount < minv) return tool_error_json(resp, tool->name, "schema_validation", req_names[i], "array has fewer than minItems"), -10;
            if (has_max && (long)acount > maxv) return tool_error_json(resp, tool->name, "schema_validation", req_names[i], "array has more than maxItems"), -10;
            if (items_string && acount > 0) {
                char items[CLAW_EXEC_MAX_ARGV][CLAW_EXEC_ARG_CAP];
                size_t count_out = 0;
                if (claw_tt_json_get_string_array(json_args, req_names[i], items, CLAW_EXEC_MAX_ARGV, &count_out) != 0)
                    return tool_error_json(resp, tool->name, "schema_validation", req_names[i], "array items must be strings"), -10;
            }
        }
    }
    return 0;
}

static int append_openapi_json(claw_dispatch_t *d, claw_response_t *resp)
{
    claw_registry_t *r;
    size_t i;
    if (!d || !d->registry || !resp) return -1;
    r = d->registry;
    if (claw_response_append(resp, "{\"openapi\":\"3.1.0\",\"info\":{\"title\":\"cclaw runtime API\",\"version\":\"16\"},\"paths\":{") != 0) return -1;
    for (i = 0; i < r->tool_count; ++i) {
        const claw_tool_api_t *t = r->tools[i];
        char tmp[32];
        if (i != 0 && claw_response_append(resp, ",") != 0) return -1;
        if (append_json_quoted(resp, tool_http_path(t->name)) != 0) return -1;
        if (claw_response_append(resp, ":{\"post\":{\"summary\":") != 0) return -1;
        if (append_json_quoted(resp, tool_help_text(t->name)) != 0) return -1;
        if (claw_response_append(resp, ",\"requestBody\":{\"required\":true,\"content\":{\"application/json\":{\"schema\":") != 0) return -1;
        if (claw_response_append(resp, t->request_schema_json && *t->request_schema_json ? t->request_schema_json : "{}") != 0) return -1;
        if (claw_response_append(resp, "}}},\"responses\":{\"200\":{\"description\":\"OK\",\"content\":{\"application/json\":{\"schema\":") != 0) return -1;
        if (claw_response_append(resp, t->response_schema_json && *t->response_schema_json ? t->response_schema_json : "{}") != 0) return -1;
        if (claw_response_append(resp, "}}},\"400\":{\"description\":\"Bad Request\"},\"403\":{\"description\":\"Forbidden\"},\"408\":{\"description\":\"Request Timeout\"},\"413\":{\"description\":\"Payload Too Large\"}},\"x-claw-tool\":{\"name\":") != 0) return -1;
        if (append_json_quoted(resp, t->name) != 0) return -1;
        if (claw_response_append(resp, ",\"abi\":{\"name\":") != 0) return -1;
        if (append_json_quoted(resp, t->abi_name ? t->abi_name : t->name) != 0) return -1;
        snprintf(tmp, sizeof(tmp), "%u", t->abi_version);
        if (claw_response_append(resp, ",\"version\":") != 0 || claw_response_append(resp, tmp) != 0) return -1;
        if (claw_response_append(resp, "}}}") != 0) return -1;
        if (claw_response_append(resp, "}") != 0) return -1;
    }
    return claw_response_append(resp, "},\"components\":{\"schemas\":{}}}");
}

static int append_tool_schema_obj(claw_response_t *resp, const claw_tool_api_t *tool)
{
    char tmp[32];
    if (!resp || !tool) return -1;
    if (claw_response_append(resp, "{\"name\":") != 0) return -1;
    if (append_json_quoted(resp, tool->name) != 0) return -1;
    if (claw_response_append(resp, ",\"abi\":{\"name\":") != 0) return -1;
    if (append_json_quoted(resp, tool->abi_name ? tool->abi_name : tool->name) != 0) return -1;
    snprintf(tmp, sizeof(tmp), "%u", tool->abi_version);
    if (claw_response_append(resp, ",\"version\":") != 0 || claw_response_append(resp, tmp) != 0) return -1;
    if (claw_response_append(resp, "},\"request_schema\":") != 0) return -1;
    if (tool->request_schema_json && *tool->request_schema_json) {
        if (claw_response_append(resp, tool->request_schema_json) != 0) return -1;
    } else if (claw_response_append(resp, "null") != 0) return -1;
    if (claw_response_append(resp, ",\"response_schema\":") != 0) return -1;
    if (tool->response_schema_json && *tool->response_schema_json) {
        if (claw_response_append(resp, tool->response_schema_json) != 0) return -1;
    } else if (claw_response_append(resp, "null") != 0) return -1;
    if (append_tool_http_doc(resp, tool) != 0) return -1;
    return claw_response_append(resp, "}");
}

int claw_dispatch_list_modules(claw_dispatch_t *d, claw_response_t *resp)
{
    uint64_t started_ms = monotonic_ms();
    int rc = 0;
    size_t i;
    const char *names[CLAW_MAX_TOOLS];
    claw_registry_t *r;

    if (!d || !d->registry || !resp) return -1;
    r = d->registry;

    if (claw_response_append(resp, "{") != 0) rc = -1;
    if (rc == 0) {
        for (i = 0; i < r->provider_count; ++i) names[i] = r->providers[i] ? r->providers[i]->name : "";
        rc = append_json_array_names(resp, "providers", names, r->provider_count);
    }
    if (rc == 0 && claw_response_append(resp, ",") != 0) rc = -1;
    if (rc == 0) {
        for (i = 0; i < r->memory_count; ++i) names[i] = r->memories[i] ? r->memories[i]->name : "";
        rc = append_json_array_names(resp, "memories", names, r->memory_count);
    }
    if (rc == 0 && claw_response_append(resp, ",") != 0) rc = -1;
    if (rc == 0) {
        for (i = 0; i < r->tool_count; ++i) names[i] = r->tools[i] ? r->tools[i]->name : "";
        rc = append_json_array_names(resp, "tools", names, r->tool_count);
    }
    if (rc == 0 && claw_response_append(resp, ",\"tool_schemas\":[") != 0) rc = -1;
    if (rc == 0) {
        for (i = 0; i < r->tool_count; ++i) {
            if (i != 0 && claw_response_append(resp, ",") != 0) { rc = -1; break; }
            if (append_tool_schema_obj(resp, r->tools[i]) != 0) { rc = -1; break; }
        }
    }
    if (rc == 0 && claw_response_append(resp, "],\"openapi\":{\"path\":\"/openapi.json\"},") != 0) rc = -1;
    if (rc == 0) {
        for (i = 0; i < r->channel_count; ++i) names[i] = r->channels[i] ? r->channels[i]->name : "";
        rc = append_json_array_names(resp, "channels", names, r->channel_count);
    }
    if (rc == 0 && claw_response_append(resp, "}") != 0) rc = -1;

    record_dispatch(d, CLAW_OP_LIST, "registry", rc, started_ms, resp->len);
    return rc;
}

int claw_dispatch_chat(claw_dispatch_t *d, const char *provider_name, const char *message, claw_response_t *resp)
{
    const claw_provider_api_t *provider;
    claw_request_t req;
    int rc;
    uint64_t started_ms = monotonic_ms();
    if (!d || !d->registry || !provider_name || !message || !resp) return -1;
    provider = claw_registry_get_provider(d->registry, provider_name);
    if (!provider) {
        record_dispatch(d, CLAW_OP_CHAT, provider_name, -2, started_ms, resp ? resp->len : 0);
        return -2;
    }
    req.input = message;
    rc = provider->chat(&req, resp);
    record_dispatch(d, CLAW_OP_CHAT, provider_name, rc, started_ms, resp->len);
    return rc;
}

int claw_dispatch_memory_put(claw_dispatch_t *d, const char *memory_name, const char *key, const char *value)
{
    const claw_memory_api_t *memory;
    int rc;
    uint64_t started_ms = monotonic_ms();
    if (!d || !d->registry || !memory_name || !key || !value) return -1;
    memory = claw_registry_get_memory(d->registry, memory_name);
    if (!memory) {
        record_dispatch(d, CLAW_OP_MEMORY_PUT, memory_name, -2, started_ms, 0);
        return -2;
    }
    rc = memory->put(key, value);
    record_dispatch(d, CLAW_OP_MEMORY_PUT, memory_name, rc, started_ms, 0);
    return rc;
}

int claw_dispatch_memory_get(claw_dispatch_t *d, const char *memory_name, const char *key, claw_response_t *resp)
{
    const claw_memory_api_t *memory;
    int rc;
    uint64_t started_ms = monotonic_ms();
    if (!d || !d->registry || !memory_name || !key || !resp) return -1;
    memory = claw_registry_get_memory(d->registry, memory_name);
    if (!memory) {
        record_dispatch(d, CLAW_OP_MEMORY_GET, memory_name, -2, started_ms, 0);
        return -2;
    }
    rc = memory->get(key, resp);
    record_dispatch(d, CLAW_OP_MEMORY_GET, memory_name, rc, started_ms, resp->len);
    return rc;
}

int claw_dispatch_memory_search(claw_dispatch_t *d, const char *memory_name, const char *query, claw_response_t *resp)
{
    const claw_memory_api_t *memory;
    int rc;
    uint64_t started_ms = monotonic_ms();
    if (!d || !d->registry || !memory_name || !query || !resp) return -1;
    memory = claw_registry_get_memory(d->registry, memory_name);
    if (!memory || !memory->search) {
        record_dispatch(d, CLAW_OP_MEMORY_SEARCH, memory_name, -2, started_ms, 0);
        return -2;
    }
    rc = memory->search(query, resp);
    record_dispatch(d, CLAW_OP_MEMORY_SEARCH, memory_name, rc, started_ms, resp->len);
    return rc;
}

int claw_dispatch_tool_invoke(claw_dispatch_t *d, const char *tool_name, const char *json_args, claw_response_t *resp)
{
    const claw_tool_api_t *tool;
    int rc;
    uint64_t started_ms = monotonic_ms();
    if (!d || !d->registry || !tool_name || !json_args || !resp) return -1;
    tool = claw_registry_get_tool(d->registry, tool_name);
    if (!tool) {
        record_dispatch(d, CLAW_OP_TOOL_INVOKE, tool_name, -2, started_ms, 0);
        return -2;
    }
    rc = schema_validate_request(tool, json_args, resp);
    if (rc == 0) rc = validate_schema_details(tool, json_args, resp);
    if (rc != 0) {
        record_dispatch(d, CLAW_OP_TOOL_INVOKE, tool_name, rc, started_ms, resp->len);
        return rc;
    }
    rc = tool->invoke(json_args, resp);
    record_dispatch(d, CLAW_OP_TOOL_INVOKE, tool_name, rc, started_ms, resp->len);
    return rc;
}

int claw_dispatch_tool_schemas(claw_dispatch_t *d, claw_response_t *resp)
{
    uint64_t started_ms = monotonic_ms();
    claw_registry_t *r;
    size_t i;
    int rc = 0;
    if (!d || !d->registry || !resp) return -1;
    r = d->registry;
    if (claw_response_append(resp, "[") != 0) rc = -1;
    for (i = 0; rc == 0 && i < r->tool_count; ++i) {
        if (i != 0 && claw_response_append(resp, ",") != 0) { rc = -1; break; }
        if (append_tool_schema_obj(resp, r->tools[i]) != 0) rc = -1;
    }
    if (rc == 0 && claw_response_append(resp, "]") != 0) rc = -1;
    record_dispatch(d, CLAW_OP_LIST, "tool_schemas", rc, started_ms, resp->len);
    return rc;
}

int claw_dispatch_tool_schema_get(claw_dispatch_t *d, const char *tool_name, claw_response_t *resp)
{
    const claw_tool_api_t *tool;
    uint64_t started_ms = monotonic_ms();
    int rc;
    if (!d || !d->registry || !tool_name || !resp) return -1;
    tool = claw_registry_get_tool(d->registry, tool_name);
    if (!tool) {
        record_dispatch(d, CLAW_OP_LIST, tool_name, -2, started_ms, 0);
        return -2;
    }
    rc = append_tool_schema_obj(resp, tool);
    record_dispatch(d, CLAW_OP_LIST, tool_name, rc, started_ms, resp->len);
    return rc;
}


int claw_dispatch_tool_validate(claw_dispatch_t *d, const char *tool_name, const char *json_args, claw_response_t *resp)
{
    const claw_tool_api_t *tool;
    uint64_t started_ms = monotonic_ms();
    int rc;
    if (!d || !d->registry || !tool_name || !json_args || !resp) return -1;
    tool = claw_registry_get_tool(d->registry, tool_name);
    if (!tool) { record_dispatch(d, CLAW_OP_LIST, tool_name, -2, started_ms, 0); return -2; }
    rc = schema_validate_request(tool, json_args, resp);
    if (rc == 0) rc = validate_schema_details(tool, json_args, resp);
    if (rc == 0) {
        if (claw_response_append(resp, "{\"ok\":true,\"tool\":") != 0) rc = -1;
        else if (append_json_quoted(resp, tool_name) != 0) rc = -1;
        else if (claw_response_append(resp, ",\"validation\":\"passed\"}") != 0) rc = -1;
    }
    record_dispatch(d, CLAW_OP_LIST, tool_name, rc, started_ms, resp->len);
    return rc;
}

int claw_dispatch_openapi(claw_dispatch_t *d, claw_response_t *resp)
{
    uint64_t started_ms = monotonic_ms();
    int rc = append_openapi_json(d, resp);
    record_dispatch(d, CLAW_OP_LIST, "openapi", rc, started_ms, resp ? resp->len : 0);
    return rc;
}
