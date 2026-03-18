#ifndef CLAW_TOOL_TYPES_H
#define CLAW_TOOL_TYPES_H

#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif
#ifndef _XOPEN_SOURCE
#define _XOPEN_SOURCE 700
#endif

#include "claw/plugin_api.h"

#include <limits.h>
#ifndef PATH_MAX
#include <linux/limits.h>
#endif
#ifndef PATH_MAX
#define PATH_MAX 4096
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define CLAW_EXEC_MAX_ARGV 64
#define CLAW_EXEC_ARG_CAP 1024
#define CLAW_EXEC_MAX_ENV 32
#define CLAW_EXEC_ENV_KEY_CAP 128
#define CLAW_EXEC_ENV_VAL_CAP 1024
#define CLAW_EXEC_DEFAULT_TIMEOUT_MS 5000L
#define CLAW_EXEC_MAX_TIMEOUT_MS 30000L
#define CLAW_EXEC_DEFAULT_MAX_OUTPUT 65536L

typedef struct {
  char key[CLAW_EXEC_ENV_KEY_CAP];
  char value[CLAW_EXEC_ENV_VAL_CAP];
} claw_env_kv_t;

typedef struct {
  char argv[CLAW_EXEC_MAX_ARGV][CLAW_EXEC_ARG_CAP];
  size_t argc;
  claw_env_kv_t env[CLAW_EXEC_MAX_ENV];
  size_t envc;
  char cwd[PATH_MAX];
  long timeout_ms;
} claw_exec_request_t;

typedef struct {
  int exit_code;
  int signal_no;
  int timed_out;
  int truncated;
  char mode[16];
  char output[CLAW_EXEC_DEFAULT_MAX_OUTPUT + 1];
  size_t output_len;
} claw_exec_response_t;

typedef struct {
  char path[PATH_MAX];
  long max_bytes;
} claw_fs_read_request_t;

typedef struct {
  char path[PATH_MAX];
  char content[65536];
  char mode[32];
} claw_fs_write_request_t;

typedef struct {
  char path[PATH_MAX];
  long max_entries;
} claw_fs_list_request_t;

static inline const char *claw_tt_skip_ws(const char *p) {
  while (p && (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n'))
    ++p;
  return p;
}
static inline int claw_tt_hex_value(char c) {
  if (c >= '0' && c <= '9')
    return c - '0';
  if (c >= 'a' && c <= 'f')
    return 10 + c - 'a';
  if (c >= 'A' && c <= 'F')
    return 10 + c - 'A';
  return -1;
}
static inline int claw_tt_utf8_from_codepoint(unsigned cp, char out[4],
                                              size_t *n) {
  if (cp <= 0x7F) {
    out[0] = (char)cp;
    *n = 1;
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

static inline int claw_tt_append_json_escaped(claw_response_t *resp,
                                              const char *s) {
  const unsigned char *p;
  char tmp[7];
  if (!resp || !s)
    return -1;
  for (p = (const unsigned char *)s; *p; ++p) {
    switch (*p) {
    case '"':
      if (claw_response_append(resp, "\\\"") != 0)
        return -1;
      break;
    case '\\':
      if (claw_response_append(resp, "\\\\") != 0)
        return -1;
      break;
    case '\b':
      if (claw_response_append(resp, "\\b") != 0)
        return -1;
      break;
    case '\f':
      if (claw_response_append(resp, "\\f") != 0)
        return -1;
      break;
    case '\n':
      if (claw_response_append(resp, "\\n") != 0)
        return -1;
      break;
    case '\r':
      if (claw_response_append(resp, "\\r") != 0)
        return -1;
      break;
    case '\t':
      if (claw_response_append(resp, "\\t") != 0)
        return -1;
      break;
    default:
      if (*p < 0x20) {
        snprintf(tmp, sizeof(tmp), "\\u%04x", (unsigned int)*p);
        if (claw_response_append(resp, tmp) != 0)
          return -1;
      } else if (claw_response_append_mem(resp, (const char *)p, 1) != 0)
        return -1;
    }
  }
  return 0;
}

static inline int claw_tt_decode_json_string(const char *quoted,
                                             claw_response_t *out) {
  const char *p = quoted;
  if (!p || *p != '"' || !out)
    return -1;
  ++p;
  while (*p) {
    if (*p == '"')
      return 0;
    if (*p == '\\') {
      char c;
      ++p;
      if (!*p)
        return -2;
      switch (*p) {
      case '"':
        c = '"';
        break;
      case '\\':
        c = '\\';
        break;
      case '/':
        c = '/';
        break;
      case 'b':
        c = '\b';
        break;
      case 'f':
        c = '\f';
        break;
      case 'n':
        c = '\n';
        break;
      case 'r':
        c = '\r';
        break;
      case 't':
        c = '\t';
        break;
      case 'u': {
        int h1, h2, h3, h4;
        unsigned cp;
        char utf8[4];
        size_t n = 0;
        if (!p[1] || !p[2] || !p[3] || !p[4])
          return -3;
        h1 = claw_tt_hex_value(p[1]);
        h2 = claw_tt_hex_value(p[2]);
        h3 = claw_tt_hex_value(p[3]);
        h4 = claw_tt_hex_value(p[4]);
        if (h1 < 0 || h2 < 0 || h3 < 0 || h4 < 0)
          return -4;
        cp = (unsigned)((h1 << 12) | (h2 << 8) | (h3 << 4) | h4);
        claw_tt_utf8_from_codepoint(cp, utf8, &n);
        if (claw_response_append_mem(out, utf8, n) != 0)
          return -5;
        p += 4;
        ++p;
        continue;
      }
      default:
        return -6;
      }
      if (claw_response_append_mem(out, &c, 1) != 0)
        return -7;
    } else if (claw_response_append_mem(out, p, 1) != 0)
      return -8;
    ++p;
  }
  return -9;
}

static inline const char *claw_tt_find_json_string_field(const char *json,
                                                         const char *field) {
  char key[128];
  const char *p;
  if (!json || !field)
    return NULL;
  if (snprintf(key, sizeof(key), "\"%s\"", field) >= (int)sizeof(key))
    return NULL;
  p = strstr(json, key);
  if (!p)
    return NULL;
  p = strchr(p, ':');
  if (!p)
    return NULL;
  p = claw_tt_skip_ws(p + 1);
  return *p == '"' ? p : NULL;
}
static inline const char *claw_tt_find_json_array_field(const char *json,
                                                        const char *field) {
  char key[128];
  const char *p;
  if (!json || !field)
    return NULL;
  if (snprintf(key, sizeof(key), "\"%s\"", field) >= (int)sizeof(key))
    return NULL;
  p = strstr(json, key);
  if (!p)
    return NULL;
  p = strchr(p, ':');
  if (!p)
    return NULL;
  p = claw_tt_skip_ws(p + 1);
  return *p == '[' ? p : NULL;
}
static inline const char *claw_tt_find_json_object_field(const char *json,
                                                         const char *field) {
  char key[128];
  const char *p;
  if (!json || !field)
    return NULL;
  if (snprintf(key, sizeof(key), "\"%s\"", field) >= (int)sizeof(key))
    return NULL;
  p = strstr(json, key);
  if (!p)
    return NULL;
  p = strchr(p, ':');
  if (!p)
    return NULL;
  p = claw_tt_skip_ws(p + 1);
  return *p == '{' ? p : NULL;
}
static inline int claw_tt_json_get_string(const char *json, const char *field,
                                          char *out, size_t cap) {
  claw_response_t r;
  const char *q = claw_tt_find_json_string_field(json, field);
  if (!q || !out || cap == 0)
    return -1;
  claw_response_init(&r, out, cap);
  return claw_tt_decode_json_string(q, &r);
}
static inline int claw_tt_json_get_long(const char *json, const char *field,
                                        long *outv) {
  char key[128];
  const char *p;
  char *end = NULL;
  long v;
  if (!json || !field || !outv)
    return -1;
  if (snprintf(key, sizeof(key), "\"%s\"", field) >= (int)sizeof(key))
    return -1;
  p = strstr(json, key);
  if (!p)
    return -2;
  p = strchr(p, ':');
  if (!p)
    return -3;
  p = claw_tt_skip_ws(p + 1);
  v = strtol(p, &end, 10);
  if (p == end)
    return -4;
  *outv = v;
  return 0;
}
static inline int claw_tt_env_key_valid(const char *key) {
  size_t i;
  if (!key || !key[0])
    return 0;
  if (!((key[0] >= 'A' && key[0] <= 'Z') || (key[0] >= 'a' && key[0] <= 'z') ||
        key[0] == '_'))
    return 0;
  for (i = 1; key[i]; ++i) {
    if (!((key[i] >= 'A' && key[i] <= 'Z') ||
          (key[i] >= 'a' && key[i] <= 'z') ||
          (key[i] >= '0' && key[i] <= '9') || key[i] == '_'))
      return 0;
  }
  return 1;
}
static inline int claw_tt_json_get_string_array(const char *json,
                                                const char *field,
                                                char items[][CLAW_EXEC_ARG_CAP],
                                                size_t max_items,
                                                size_t *count_out) {
  const char *p = claw_tt_find_json_array_field(json, field);
  size_t count = 0;
  if (!p || !items || !count_out || max_items == 0)
    return -1;
  ++p;
  for (;;) {
    claw_response_t tmp;
    const char *q;
    p = claw_tt_skip_ws(p);
    if (*p == ']') {
      *count_out = count;
      return 0;
    }
    if (*p != '"' || count >= max_items)
      return -2;
    claw_response_init(&tmp, items[count], CLAW_EXEC_ARG_CAP);
    if (claw_tt_decode_json_string(p, &tmp) != 0)
      return -3;
    q = p + 1;
    while (*q) {
      if (*q == '"') {
        ++q;
        break;
      }
      if (*q == '\\') {
        ++q;
        if (!*q)
          return -4;
        if (*q == 'u') {
          if (!q[1] || !q[2] || !q[3] || !q[4])
            return -5;
          q += 4;
        }
      }
      ++q;
    }
    if (*(q - 1) != '"')
      return -6;
    ++count;
    p = claw_tt_skip_ws(q);
    if (*p == ',') {
      ++p;
      continue;
    }
    if (*p == ']') {
      *count_out = count;
      return 0;
    }
    return -7;
  }
}
static inline int claw_tt_json_get_env_object(const char *json,
                                              const char *field,
                                              claw_env_kv_t items[],
                                              size_t max_items,
                                              size_t *count_out) {
  const char *p = claw_tt_find_json_object_field(json, field);
  size_t count = 0;
  if (!p || !items || !count_out)
    return -1;
  ++p;
  for (;;) {
    claw_response_t tmp;
    const char *q;
    p = claw_tt_skip_ws(p);
    if (*p == '}') {
      *count_out = count;
      return 0;
    }
    if (*p != '"' || count >= max_items)
      return -2;
    claw_response_init(&tmp, items[count].key, sizeof(items[count].key));
    if (claw_tt_decode_json_string(p, &tmp) != 0 ||
        !claw_tt_env_key_valid(items[count].key))
      return -3;
    q = p + 1;
    while (*q) {
      if (*q == '"') {
        ++q;
        break;
      }
      if (*q == '\\') {
        ++q;
        if (!*q)
          return -4;
        if (*q == 'u') {
          if (!q[1] || !q[2] || !q[3] || !q[4])
            return -5;
          q += 4;
        }
      }
      ++q;
    }
    p = claw_tt_skip_ws(q);
    if (*p != ':')
      return -6;
    p = claw_tt_skip_ws(p + 1);
    if (*p != '"')
      return -7;
    claw_response_init(&tmp, items[count].value, sizeof(items[count].value));
    if (claw_tt_decode_json_string(p, &tmp) != 0)
      return -8;
    q = p + 1;
    while (*q) {
      if (*q == '"') {
        ++q;
        break;
      }
      if (*q == '\\') {
        ++q;
        if (!*q)
          return -9;
        if (*q == 'u') {
          if (!q[1] || !q[2] || !q[3] || !q[4])
            return -10;
          q += 4;
        }
      }
      ++q;
    }
    ++count;
    p = claw_tt_skip_ws(q);
    if (*p == ',') {
      ++p;
      continue;
    }
    if (*p == '}') {
      *count_out = count;
      return 0;
    }
    return -11;
  }
}

static inline int claw_tt_path_is_under_root(const char *path,
                                             const char *root) {
  size_t n;
  if (!path || !root)
    return 0;
  n = strlen(root);
  if (n == 0)
    return 0;
  if (strncmp(path, root, n) != 0)
    return 0;
  if (root[n - 1] == '/')
    return 1;
  return path[n] == '\0' || path[n] == '/';
}
static inline int claw_tt_split_allowed_roots(char roots[][PATH_MAX],
                                              size_t *count) {
  const char *env = getenv("CLAW_TOOL_SHELL_ALLOWED_ROOTS");
  char cwd[PATH_MAX];
  size_t n = 0;
  if (!count)
    return -1;
  *count = 0;
  if (!env || !*env) {
    if (!getcwd(cwd, sizeof(cwd)))
      return -1;
    snprintf(roots[0], PATH_MAX, "%s", cwd);
    *count = 1;
    return 0;
  }
  while (*env && n < 16) {
    char token[PATH_MAX];
    size_t len = 0;
    while (env[len] && env[len] != ':') {
      if (len + 1 >= sizeof(token))
        return -1;
      token[len] = env[len];
      ++len;
    }
    token[len] = '\0';
    if (len != 0) {
      if (!realpath(token, roots[n]))
        return -1;
      ++n;
    }
    env += len;
    if (*env == ':')
      ++env;
  }
  if (n == 0)
    return -1;
  *count = n;
  return 0;
}
static inline int claw_tt_resolve_realpath_or_cwd(const char *input,
                                                  char out[PATH_MAX]) {
  if (!input || !*input) {
    if (!getcwd(out, PATH_MAX))
      return -1;
    return 0;
  }
  if (!realpath(input, out))
    return -1;
  return 0;
}
static inline int claw_tt_path_allowed(const char *path_real) {
  char roots[16][PATH_MAX];
  size_t i, n = 0;
  if (claw_tt_split_allowed_roots(roots, &n) != 0)
    return 0;
  for (i = 0; i < n; ++i)
    if (claw_tt_path_is_under_root(path_real, roots[i]))
      return 1;
  return 0;
}
static inline int claw_tt_realpath_maybe_missing(const char *path,
                                                 char out[PATH_MAX]) {
  char parent[PATH_MAX], real_parent[PATH_MAX];
  const char *base;
  char *slash;
  if (!path || !*path || !out)
    return -1;
  if (realpath(path, out))
    return 0;
  if (strncmp(path, "/", 1) == 0) {
    snprintf(parent, sizeof(parent), "%s", path);
  } else {
    char cwd[PATH_MAX];
    if (!getcwd(cwd, sizeof(cwd)))
      return -1;
    if (snprintf(parent, sizeof(parent), "%s/%s", cwd, path) >=
        (int)sizeof(parent))
      return -1;
  }
  slash = strrchr(parent, '/');
  if (!slash)
    return -1;
  *slash = '\0';
  base = slash + 1;
  if (!realpath(parent, real_parent))
    return -1;
  if (snprintf(out, PATH_MAX, "%s/%s", real_parent, base) >= PATH_MAX)
    return -1;
  return 0;
}

static inline int claw_tt_error_json(claw_response_t *resp, const char *tool,
                                     const char *code, const char *field,
                                     const char *message) {
  if (!resp)
    return -1;
  if (claw_response_append(resp, "{\"ok\":false,\"tool\":\"") != 0)
    return -1;
  if (claw_tt_append_json_escaped(resp, tool ? tool : "") != 0)
    return -1;
  if (claw_response_append(resp, "\",\"error\":{\"code\":\"") != 0)
    return -1;
  if (claw_tt_append_json_escaped(resp, code ? code : "error") != 0)
    return -1;
  if (claw_response_append(resp, "\",\"field\":") != 0)
    return -1;
  if (field && *field) {
    if (claw_response_append(resp, "\"") != 0)
      return -1;
    if (claw_tt_append_json_escaped(resp, field) != 0)
      return -1;
    if (claw_response_append(resp, "\"") != 0)
      return -1;
  } else {
    if (claw_response_append(resp, "null") != 0)
      return -1;
  }
  if (claw_response_append(resp, ",\"message\":\"") != 0)
    return -1;
  if (claw_tt_append_json_escaped(resp, message ? message : "") != 0)
    return -1;
  return claw_response_append(resp, "\"}}");
}

static inline int claw_tt_exec_request_parse(const char *json,
                                             claw_exec_request_t *req,
                                             claw_response_t *resp) {
  if (!json || !req || !resp)
    return -1;
  memset(req, 0, sizeof(*req));
  if (claw_tt_json_get_string_array(json, "argv", req->argv, CLAW_EXEC_MAX_ARGV,
                                    &req->argc) != 0 ||
      req->argc == 0)
    return claw_tt_error_json(resp, "exec", "schema_validation", "argv",
                              "argv must be a non-empty array of strings");
  if (claw_tt_json_get_env_object(json, "env", req->env, CLAW_EXEC_MAX_ENV,
                                  &req->envc) != 0)
    req->envc = 0;
  if (claw_tt_json_get_string(json, "cwd", req->cwd, sizeof(req->cwd)) != 0)
    req->cwd[0] = '\0';
  if (claw_tt_json_get_long(json, "timeout_ms", &req->timeout_ms) != 0)
    req->timeout_ms = CLAW_EXEC_DEFAULT_TIMEOUT_MS;
  if (req->timeout_ms <= 0 || req->timeout_ms > CLAW_EXEC_MAX_TIMEOUT_MS)
    return claw_tt_error_json(resp, "exec", "schema_validation", "timeout_ms",
                              "timeout_ms must be between 1 and 30000");
  return 0;
}
static inline int claw_tt_fs_read_request_parse(const char *json,
                                                claw_fs_read_request_t *req,
                                                claw_response_t *resp) {
  if (!json || !req || !resp)
    return -1;
  memset(req, 0, sizeof(*req));
  if (claw_tt_json_get_string(json, "path", req->path, sizeof(req->path)) !=
          0 ||
      req->path[0] == '\0')
    return claw_tt_error_json(resp, "fs.read", "schema_validation", "path",
                              "path is required");
  if (claw_tt_json_get_long(json, "max_bytes", &req->max_bytes) != 0)
    req->max_bytes = 65536;
  if (req->max_bytes <= 0 || req->max_bytes > 1048576)
    return claw_tt_error_json(resp, "fs.read", "schema_validation", "max_bytes",
                              "max_bytes must be between 1 and 1048576");
  return 0;
}
static inline int claw_tt_fs_write_request_parse(const char *json,
                                                 claw_fs_write_request_t *req,
                                                 claw_response_t *resp) {
  if (!json || !req || !resp)
    return -1;
  memset(req, 0, sizeof(*req));
  if (claw_tt_json_get_string(json, "path", req->path, sizeof(req->path)) !=
          0 ||
      req->path[0] == '\0')
    return claw_tt_error_json(resp, "fs.write", "schema_validation", "path",
                              "path is required");
  if (claw_tt_json_get_string(json, "content", req->content,
                              sizeof(req->content)) != 0)
    return claw_tt_error_json(resp, "fs.write", "schema_validation", "content",
                              "content is required and must be a string");
  if (claw_tt_json_get_string(json, "mode", req->mode, sizeof(req->mode)) != 0)
    snprintf(req->mode, sizeof(req->mode), "overwrite");
  if (strcmp(req->mode, "overwrite") != 0 && strcmp(req->mode, "append") != 0)
    return claw_tt_error_json(resp, "fs.write", "schema_validation", "mode",
                              "mode must be overwrite or append");
  return 0;
}
static inline int claw_tt_fs_list_request_parse(const char *json,
                                                claw_fs_list_request_t *req,
                                                claw_response_t *resp) {
  if (!json || !req || !resp)
    return -1;
  memset(req, 0, sizeof(*req));
  if (claw_tt_json_get_string(json, "path", req->path, sizeof(req->path)) != 0)
    req->path[0] = '\0';
  if (claw_tt_json_get_long(json, "max_entries", &req->max_entries) != 0)
    req->max_entries = 200;
  if (req->max_entries <= 0 || req->max_entries > 2000)
    return claw_tt_error_json(resp, "fs.list", "schema_validation",
                              "max_entries",
                              "max_entries must be between 1 and 2000");
  return 0;
}

static inline int claw_tt_http_status_from_json(const char *body) {
  if (!body)
    return 500;
  if (strstr(body, "\"schema_validation\""))
    return 400;
  if (strstr(body, "\"permission_denied\"") ||
      strstr(body, "\"path_rejected\"") || strstr(body, "\"command_rejected\""))
    return 403;
  if (strstr(body, "\"not_found\"") || strstr(body, "\"invalid_path\""))
    return 404;
  if (strstr(body, "\"execution_timeout\""))
    return 408;
  if (strstr(body, "\"payload_too_large\""))
    return 413;
  return strstr(body, "\"ok\":false") ? 400 : 200;
}

#endif
