#include "claw/runtime_trace.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

static const char *env_or_default(const char *name, const char *fallback)
{
    const char *v = getenv(name);
    return (v && *v) ? v : fallback;
}

static int env_bool_default(const char *name, int fallback)
{
    const char *v = getenv(name);
    if (!v || !*v) return fallback;
    if (strcmp(v, "1") == 0 || strcmp(v, "true") == 0 || strcmp(v, "yes") == 0) return 1;
    if (strcmp(v, "0") == 0 || strcmp(v, "false") == 0 || strcmp(v, "no") == 0) return 0;
    return fallback;
}

static int mkdirs_parent(const char *path)
{
    char buf[PATH_MAX];
    char *p;
    if (!path || !*path) return -1;
    if (snprintf(buf, sizeof(buf), "%s", path) >= (int)sizeof(buf)) return -1;
    p = strrchr(buf, '/');
    if (!p) return 0;
    *p = '\0';
    if (buf[0] == '\0') return 0;
    for (p = buf + 1; *p; ++p) {
        if (*p == '/') {
            *p = '\0';
            if (mkdir(buf, 0755) != 0 && errno != EEXIST) return -1;
            *p = '/';
        }
    }
    if (mkdir(buf, 0755) != 0 && errno != EEXIST) return -1;
    return 0;
}

static uint64_t now_unix_ms(void)
{
    struct timespec ts;
    if (clock_gettime(CLOCK_REALTIME, &ts) != 0) return 0;
    return (uint64_t)ts.tv_sec * 1000ULL + (uint64_t)ts.tv_nsec / 1000000ULL;
}

static int json_escape_to_file(FILE *fp, const char *s)
{
    const unsigned char *p;
    if (!fp || !s) return 0;
    for (p = (const unsigned char *)s; *p; ++p) {
        switch (*p) {
        case '"': if (fputs("\\\"", fp) == EOF) return -1; break;
        case '\\': if (fputs("\\\\", fp) == EOF) return -1; break;
        case '\b': if (fputs("\\b", fp) == EOF) return -1; break;
        case '\f': if (fputs("\\f", fp) == EOF) return -1; break;
        case '\n': if (fputs("\\n", fp) == EOF) return -1; break;
        case '\r': if (fputs("\\r", fp) == EOF) return -1; break;
        case '\t': if (fputs("\\t", fp) == EOF) return -1; break;
        default:
            if (*p < 0x20) {
                if (fprintf(fp, "\\u%04x", (unsigned int)*p) < 0) return -1;
            } else if (fputc((int)*p, fp) == EOF) {
                return -1;
            }
        }
    }
    return 0;
}

void claw_trace_init(claw_trace_t *t)
{
    FILE *fp;
    const char *path;
    if (!t) return;
    memset(t, 0, sizeof(*t));
    t->enabled = env_bool_default("CLAW_TRACE_ENABLED", 1);
    path = env_or_default("CLAW_TRACE_PATH", "./state/runtime-trace.jsonl");
    if (snprintf(t->path, sizeof(t->path), "%s", path) >= (int)sizeof(t->path)) {
        t->enabled = 0;
        t->path[0] = '\0';
        return;
    }
    if (!t->enabled) return;
    if (mkdirs_parent(t->path) != 0) {
        t->enabled = 0;
        return;
    }
    fp = fopen(t->path, "a");
    if (!fp) {
        t->enabled = 0;
        return;
    }
    setvbuf(fp, NULL, _IOLBF, 0);
    t->fp = fp;
}

void claw_trace_close(claw_trace_t *t)
{
    FILE *fp;
    if (!t) return;
    fp = (FILE *)t->fp;
    if (fp) fclose(fp);
    t->fp = NULL;
}

const char *claw_trace_path(const claw_trace_t *t)
{
    return (t && t->path[0]) ? t->path : "";
}

static int trace_begin(FILE *fp, const char *kind)
{
    if (!fp || !kind) return -1;
    if (fprintf(fp, "{\"ts_unix_ms\":%llu,\"kind\":\"", (unsigned long long)now_unix_ms()) < 0) return -1;
    if (json_escape_to_file(fp, kind) != 0) return -1;
    if (fputs("\"", fp) == EOF) return -1;
    return 0;
}

static int trace_append_kv_str(FILE *fp, const char *key, const char *value)
{
    if (!fp || !key || !value) return -1;
    if (fprintf(fp, ",\"%s\":\"", key) < 0) return -1;
    if (json_escape_to_file(fp, value) != 0) return -1;
    if (fputs("\"", fp) == EOF) return -1;
    return 0;
}

static int trace_append_kv_i64(FILE *fp, const char *key, long long value)
{
    return (!fp || !key) ? -1 : (fprintf(fp, ",\"%s\":%lld", key, value) < 0 ? -1 : 0);
}

static int trace_finish(FILE *fp)
{
    if (!fp) return -1;
    if (fputs("}\n", fp) == EOF) return -1;
    return fflush(fp);
}

int claw_trace_write_dispatch(claw_trace_t *t, claw_metrics_t *m,
    claw_op_kind_t op, const char *target, int rc, uint64_t duration_ms, size_t response_bytes)
{
    FILE *fp;
    int ok = 0;
    if (!t || !t->enabled || !(fp = (FILE *)t->fp)) return 0;
    if (trace_begin(fp, "dispatch") == 0 &&
        trace_append_kv_str(fp, "op", claw_metrics_op_name(op)) == 0 &&
        trace_append_kv_str(fp, "target", target ? target : "") == 0 &&
        trace_append_kv_i64(fp, "rc", rc) == 0 &&
        trace_append_kv_i64(fp, "duration_ms", (long long)duration_ms) == 0 &&
        trace_append_kv_i64(fp, "response_bytes", (long long)response_bytes) == 0 &&
        trace_finish(fp) == 0) {
        ok = 1;
    }
    claw_metrics_record_trace_write(m, ok);
    return ok ? 0 : -1;
}

int claw_trace_write_scheduler(claw_trace_t *t, claw_metrics_t *m,
    const char *event, const char *target, int rc, uint64_t duration_ms, const char *detail)
{
    FILE *fp;
    int ok = 0;
    if (!t || !t->enabled || !(fp = (FILE *)t->fp)) return 0;
    if (trace_begin(fp, "scheduler") == 0 &&
        trace_append_kv_str(fp, "event", event ? event : "") == 0 &&
        trace_append_kv_str(fp, "target", target ? target : "") == 0 &&
        trace_append_kv_i64(fp, "rc", rc) == 0 &&
        trace_append_kv_i64(fp, "duration_ms", (long long)duration_ms) == 0 &&
        trace_append_kv_str(fp, "detail", detail ? detail : "") == 0 &&
        trace_finish(fp) == 0) {
        ok = 1;
    }
    claw_metrics_record_trace_write(m, ok);
    return ok ? 0 : -1;
}
