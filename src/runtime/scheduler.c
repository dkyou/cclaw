#define _GNU_SOURCE
#include "claw/scheduler.h"

#include <ctype.h>
#include <errno.h>
#include <signal.h>
#include <sqlite3.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/stat.h>
#include <sys/timerfd.h>
#include <time.h>
#include <unistd.h>

static volatile sig_atomic_t g_scheduler_stop = 0;

static void scheduler_signal_handler(int signo)
{
    (void)signo;
    g_scheduler_stop = 1;
}

static int env_bool_default(const char *name, int fallback)
{
    const char *v = getenv(name);
    if (!v || !*v) return fallback;
    if (strcmp(v, "1") == 0 || strcmp(v, "true") == 0 || strcmp(v, "yes") == 0) return 1;
    if (strcmp(v, "0") == 0 || strcmp(v, "false") == 0 || strcmp(v, "no") == 0) return 0;
    return fallback;
}

static int env_int_default(const char *name, int fallback, int min_v, int max_v)
{
    const char *v = getenv(name);
    char *end = NULL;
    long n;
    if (!v || !*v) return fallback;
    n = strtol(v, &end, 10);
    if (!end || *end != '\0' || n < min_v || n > max_v) return fallback;
    return (int)n;
}

static const char *env_or_default(const char *name, const char *fallback)
{
    const char *v = getenv(name);
    return (v && *v) ? v : fallback;
}

static uint64_t monotonic_ms(void)
{
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) return 0;
    return (uint64_t)ts.tv_sec * 1000ULL + (uint64_t)ts.tv_nsec / 1000000ULL;
}

static int mkdirs_for_path(const char *path)
{
    char tmp[PATH_MAX];
    char *p;
    if (!path || !*path) return -1;
    if (snprintf(tmp, sizeof(tmp), "%s", path) >= (int)sizeof(tmp)) return -1;
    for (p = tmp + 1; *p; ++p) {
        if (*p == '/') {
            *p = '\0';
            if (mkdir(tmp, 0755) != 0 && errno != EEXIST) return -1;
            *p = '/';
        }
    }
    return 0;
}

static claw_task_kind_t parse_kind(const char *s)
{
    if (!s) return 0;
    if (strcmp(s, "chat") == 0) return CLAW_TASK_CHAT;
    if (strcmp(s, "tool") == 0) return CLAW_TASK_TOOL;
    if (strcmp(s, "memory_put") == 0) return CLAW_TASK_MEMORY_PUT;
    return 0;
}

static const char *kind_name(claw_task_kind_t k)
{
    switch (k) {
    case CLAW_TASK_CHAT: return "chat";
    case CLAW_TASK_TOOL: return "tool";
    case CLAW_TASK_MEMORY_PUT: return "memory_put";
    default: return "unknown";
    }
}

static claw_schedule_type_t parse_schedule_type(const char *s)
{
    if (!s || !*s || strcmp(s, "interval") == 0) return CLAW_SCHEDULE_INTERVAL;
    if (strcmp(s, "oneshot") == 0 || strcmp(s, "one-shot") == 0 || strcmp(s, "once") == 0) return CLAW_SCHEDULE_ONESHOT;
    if (strcmp(s, "cron") == 0) return CLAW_SCHEDULE_CRON;
    return 0;
}

static const char *schedule_type_name(claw_schedule_type_t t)
{
    switch (t) {
    case CLAW_SCHEDULE_INTERVAL: return "interval";
    case CLAW_SCHEDULE_ONESHOT: return "oneshot";
    case CLAW_SCHEDULE_CRON: return "cron";
    default: return "unknown";
    }
}

static const char *default_scheduler_timezone(void)
{
    return env_or_default("CLAW_SCHEDULER_TZ", "");
}

static int append_json_escaped(claw_response_t *resp, const char *s)
{
    const unsigned char *p;
    char tmp[7];
    if (!resp || !s) return -1;
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
            } else if (claw_response_append_mem(resp, (const char *)p, 1) != 0) {
                return -1;
            }
            break;
        }
    }
    return 0;
}

static int db_exec(sqlite3 *db, const char *sql)
{
    return sqlite3_exec(db, sql, NULL, NULL, NULL) == SQLITE_OK ? 0 : -1;
}

static int db_has_column(sqlite3 *db, const char *table, const char *column)
{
    sqlite3_stmt *stmt = NULL;
    char sql[256];
    int rc;
    int found = 0;
    if (!db || !table || !column) return 0;
    if (snprintf(sql, sizeof(sql), "PRAGMA table_info(%s);", table) >= (int)sizeof(sql)) return 0;
    rc = sqlite3_prepare_v2(db, sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK) return 0;
    while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
        const unsigned char *name = sqlite3_column_text(stmt, 1);
        if (name && strcmp((const char *)name, column) == 0) { found = 1; break; }
    }
    sqlite3_finalize(stmt);
    return found;
}


static int cron_parse_number(const char *s, int *out)
{
    char *end = NULL;
    long v;
    if (!s || !*s || !out) return -1;
    v = strtol(s, &end, 10);
    if (!end || *end != '\0') return -1;
    *out = (int)v;
    return 0;
}

static int cron_parse_named_value(const char *s, int is_month, int is_dow, int *out)
{
    static const char *months[] = {"JAN","FEB","MAR","APR","MAY","JUN","JUL","AUG","SEP","OCT","NOV","DEC"};
    static const char *dows[] = {"SUN","MON","TUE","WED","THU","FRI","SAT"};
    char up[16];
    size_t i, n;
    if (!s || !*s || !out) return -1;
    n = strlen(s);
    if (n == 0 || n >= sizeof(up)) return -1;
    for (i = 0; i < n; ++i) up[i] = (char)toupper((unsigned char)s[i]);
    up[n] = '\0';
    if (is_month) {
        for (i = 0; i < 12; ++i) if (strcmp(up, months[i]) == 0) { *out = (int)i + 1; return 0; }
    }
    if (is_dow) {
        for (i = 0; i < 7; ++i) if (strcmp(up, dows[i]) == 0) { *out = (int)i; return 0; }
    }
    return cron_parse_number(s, out);
}

static int cron_value_normalize(int value, int is_dow)
{
    if (is_dow && value == 7) return 0;
    return value;
}

static int cron_field_is_star(const char *expr)
{
    if (!expr) return 0;
    while (*expr == ' ' || *expr == '\t') ++expr;
    return expr[0] == '*' && expr[1] == '\0';
}

static int cron_localtime_tz(const char *tz, time_t ts, struct tm *out)
{
    const char *old = getenv("TZ");
    char backup[128];
    int had_old = (old && *old) ? 1 : 0;
    int rc;
    if (!out) return -1;
    if (had_old) {
        snprintf(backup, sizeof(backup), "%s", old);
        old = backup;
    }
    if (tz && *tz) setenv("TZ", tz, 1);
    else unsetenv("TZ");
    tzset();
    rc = localtime_r(&ts, out) ? 0 : -1;
    if (had_old) setenv("TZ", old, 1);
    else unsetenv("TZ");
    tzset();
    return rc;
}

static int cron_parse_value_token(const char *s, int is_month, int is_dow, int *out)
{
    int v;
    if (cron_parse_named_value(s, is_month, is_dow, &v) != 0) return -1;
    *out = cron_value_normalize(v, is_dow);
    return 0;
}

static int cron_segment_matches(const char *seg, int value, int min_v, int max_v, int is_month, int is_dow)
{
    char base[64];
    const char *slash;
    int step = 1;
    int start, end, v;
    size_t n;
    if (!seg || !*seg) return 0;
    slash = strchr(seg, '/');
    if (slash) {
        char stepbuf[32];
        size_t sn = strlen(slash + 1);
        if (sn == 0 || sn >= sizeof(stepbuf)) return 0;
        memcpy(stepbuf, slash + 1, sn + 1);
        if (cron_parse_number(stepbuf, &step) != 0 || step <= 0) return 0;
        n = (size_t)(slash - seg);
    } else {
        n = strlen(seg);
    }
    if (n == 0 || n >= sizeof(base)) return 0;
    memcpy(base, seg, n);
    base[n] = '\0';

    value = cron_value_normalize(value, is_dow);
    if (strcmp(base, "*") == 0) {
        start = min_v;
        end = max_v;
    } else {
        char *dash = strchr(base, '-');
        if (dash) {
            *dash = '\0';
            if (cron_parse_value_token(base, is_month, is_dow, &start) != 0 ||
                cron_parse_value_token(dash + 1, is_month, is_dow, &end) != 0) return 0;
        } else {
            if (cron_parse_value_token(base, is_month, is_dow, &v) != 0) return 0;
            start = v;
            end = v;
        }
    }

    if (start < min_v || start > max_v || end < min_v || end > max_v || start > end) return 0;
    if (value < start || value > end) return 0;
    return ((value - start) % step) == 0;
}

static int cron_field_matches(const char *expr, int value, int min_v, int max_v, int is_month, int is_dow)
{
    const char *p = expr;
    if (!expr || !*expr) return 0;
    while (*p) {
        char seg[64];
        size_t n = 0;
        while (*p == ' ' || *p == '\t') ++p;
        while (p[n] && p[n] != ',') {
            if (n + 1 >= sizeof(seg)) return 0;
            seg[n] = p[n];
            ++n;
        }
        seg[n] = '\0';
        if (cron_segment_matches(seg, value, min_v, max_v, is_month, is_dow)) return 1;
        p += n;
        if (*p == ',') ++p;
    }
    return 0;
}

static int cron_split_fields(const char *expr, char fields[6][64], int *field_count_out)
{
    const char *p = expr;
    int count = 0;
    if (!expr || !field_count_out) return -1;
    while (*p && count < 6) {
        size_t n = 0;
        while (*p == ' ' || *p == '\t') ++p;
        if (!*p) break;
        while (p[n] && p[n] != ' ' && p[n] != '\t') {
            if (n + 1 >= 64) return -1;
            fields[count][n] = p[n];
            ++n;
        }
        fields[count][n] = '\0';
        p += n;
        ++count;
    }
    while (*p == ' ' || *p == '\t') ++p;
    if (*p != '\0') return -1;
    if (count != 5 && count != 6) return -1;
    *field_count_out = count;
    return 0;
}

static int cron_matches_expr_tz(const char *expr, const char *tz, time_t ts)
{
    char fields[6][64];
    struct tm tmv;
    int field_count = 0;
    int sec_match, min_match, hour_match, dom_match, mon_match, dow_match;
    int dom_star, dow_star;
    if (cron_split_fields(expr, fields, &field_count) != 0) return 0;
    if (cron_localtime_tz((tz && *tz) ? tz : NULL, ts, &tmv) != 0) return 0;
    if (field_count == 6) {
        sec_match = cron_field_matches(fields[0], tmv.tm_sec, 0, 59, 0, 0);
        min_match = cron_field_matches(fields[1], tmv.tm_min, 0, 59, 0, 0);
        hour_match = cron_field_matches(fields[2], tmv.tm_hour, 0, 23, 0, 0);
        dom_match = cron_field_matches(fields[3], tmv.tm_mday, 1, 31, 0, 0);
        mon_match = cron_field_matches(fields[4], tmv.tm_mon + 1, 1, 12, 1, 0);
        dow_match = cron_field_matches(fields[5], tmv.tm_wday, 0, 6, 0, 1);
        dom_star = cron_field_is_star(fields[3]);
        dow_star = cron_field_is_star(fields[5]);
    } else {
        sec_match = 1;
        min_match = cron_field_matches(fields[0], tmv.tm_min, 0, 59, 0, 0);
        hour_match = cron_field_matches(fields[1], tmv.tm_hour, 0, 23, 0, 0);
        dom_match = cron_field_matches(fields[2], tmv.tm_mday, 1, 31, 0, 0);
        mon_match = cron_field_matches(fields[3], tmv.tm_mon + 1, 1, 12, 1, 0);
        dow_match = cron_field_matches(fields[4], tmv.tm_wday, 0, 6, 0, 1);
        dom_star = cron_field_is_star(fields[2]);
        dow_star = cron_field_is_star(fields[4]);
    }
    if (!(sec_match && min_match && hour_match && mon_match)) return 0;
    if (dom_star && dow_star) return 1;
    if (dom_star) return dow_match;
    if (dow_star) return dom_match;
    return dom_match || dow_match;
}

static int cron_next_due(const char *expr, const char *tz, time_t after_ts, time_t *next_out)
{
    char fields[6][64];
    int field_count = 0;
    time_t base;
    int i;
    int step_sec;
    if (!expr || !*expr || !next_out) return -1;
    if (cron_split_fields(expr, fields, &field_count) != 0) return -1;
    step_sec = (field_count == 6) ? 1 : 60;
    base = after_ts + step_sec;
    if (field_count == 5) base = ((base + 59) / 60) * 60;
    for (i = 0; i < 366 * 24 * 60 * ((field_count == 6) ? 60 : 1); ++i) {
        time_t cand = base + (time_t)i * step_sec;
        if (cron_matches_expr_tz(expr, tz, cand)) {
            *next_out = cand;
            return 0;
        }
    }
    return -1;
}

static int scheduler_ensure_schema(sqlite3 *db)
{
    if (!db) return -1;
    if (db_exec(db,
        "PRAGMA journal_mode=WAL;"
        "PRAGMA synchronous=NORMAL;"
        "CREATE TABLE IF NOT EXISTS scheduler_tasks ("
        " id INTEGER PRIMARY KEY AUTOINCREMENT,"
        " enabled INTEGER NOT NULL DEFAULT 1,"
        " paused INTEGER NOT NULL DEFAULT 0,"
        " schedule_type TEXT NOT NULL DEFAULT 'interval',"
        " interval_sec INTEGER NOT NULL DEFAULT 0,"
        " run_at_unix INTEGER NOT NULL DEFAULT 0,"
        " cron_expr TEXT NOT NULL DEFAULT '',"
        " timezone TEXT NOT NULL DEFAULT '',"
        " kind TEXT NOT NULL,"
        " target TEXT NOT NULL,"
        " arg1 TEXT NOT NULL DEFAULT '',"
        " arg2 TEXT NOT NULL DEFAULT '',"
        " next_due_unix INTEGER NOT NULL DEFAULT 0,"
        " last_run_unix INTEGER NOT NULL DEFAULT 0,"
        " last_rc INTEGER NOT NULL DEFAULT 0,"
        " run_count INTEGER NOT NULL DEFAULT 0,"
        " created_at INTEGER NOT NULL DEFAULT 0,"
        " updated_at INTEGER NOT NULL DEFAULT 0"
        ");") != 0) return -2;
    if (!db_has_column(db, "scheduler_tasks", "paused") && db_exec(db, "ALTER TABLE scheduler_tasks ADD COLUMN paused INTEGER NOT NULL DEFAULT 0;") != 0) return -3;
    if (!db_has_column(db, "scheduler_tasks", "run_count") && db_exec(db, "ALTER TABLE scheduler_tasks ADD COLUMN run_count INTEGER NOT NULL DEFAULT 0;") != 0) return -4;
    if (!db_has_column(db, "scheduler_tasks", "created_at") && db_exec(db, "ALTER TABLE scheduler_tasks ADD COLUMN created_at INTEGER NOT NULL DEFAULT 0;") != 0) return -5;
    if (!db_has_column(db, "scheduler_tasks", "updated_at") && db_exec(db, "ALTER TABLE scheduler_tasks ADD COLUMN updated_at INTEGER NOT NULL DEFAULT 0;") != 0) return -6;
    if (!db_has_column(db, "scheduler_tasks", "schedule_type") && db_exec(db, "ALTER TABLE scheduler_tasks ADD COLUMN schedule_type TEXT NOT NULL DEFAULT 'interval';") != 0) return -7;
    if (!db_has_column(db, "scheduler_tasks", "run_at_unix") && db_exec(db, "ALTER TABLE scheduler_tasks ADD COLUMN run_at_unix INTEGER NOT NULL DEFAULT 0;") != 0) return -8;
    if (!db_has_column(db, "scheduler_tasks", "cron_expr") && db_exec(db, "ALTER TABLE scheduler_tasks ADD COLUMN cron_expr TEXT NOT NULL DEFAULT '';") != 0) return -9;
    if (!db_has_column(db, "scheduler_tasks", "timezone") && db_exec(db, "ALTER TABLE scheduler_tasks ADD COLUMN timezone TEXT NOT NULL DEFAULT '';") != 0) return -10;
    if (db_exec(db,
        "UPDATE scheduler_tasks SET created_at = strftime('%s','now') WHERE created_at = 0;"
        "UPDATE scheduler_tasks SET updated_at = strftime('%s','now') WHERE updated_at = 0;"
        "UPDATE scheduler_tasks SET schedule_type = 'interval' WHERE schedule_type = '';"
        "UPDATE scheduler_tasks SET timezone = '' WHERE timezone IS NULL;"
        "CREATE INDEX IF NOT EXISTS idx_scheduler_tasks_enabled_due ON scheduler_tasks(enabled, paused, next_due_unix);") != 0) return -11;
    return 0;
}

static int scheduler_ensure_db(claw_scheduler_t *s)
{
    sqlite3 *db;
    int rc;
    if (!s) return -1;
    if (s->db) return 0;
    if (mkdirs_for_path(s->db_path) != 0) return -2;
    if (sqlite3_open(s->db_path, &db) != SQLITE_OK) {
        if (db) sqlite3_close(db);
        return -3;
    }
    rc = scheduler_ensure_schema(db);
    if (rc != 0) { sqlite3_close(db); return rc; }
    s->db = db;
    return 0;
}

static int scheduler_compute_next_due(const claw_scheduler_task_t *t, time_t after_ts, time_t *next_out)
{
    if (!t || !next_out) return -1;
    switch (t->schedule_type) {
    case CLAW_SCHEDULE_INTERVAL:
        if (t->interval_sec < 1) return -1;
        *next_out = after_ts + t->interval_sec;
        return 0;
    case CLAW_SCHEDULE_ONESHOT:
        if (t->last_run_unix > 0) { *next_out = 0; return 0; }
        *next_out = t->run_at_unix;
        return t->run_at_unix > 0 ? 0 : -1;
    case CLAW_SCHEDULE_CRON:
        return cron_next_due(t->cron_expr, t->timezone, after_ts, next_out);
    default:
        return -1;
    }
}

static int scheduler_update_task_state(claw_scheduler_t *s, const claw_scheduler_task_t *t)
{
    sqlite3_stmt *stmt = NULL;
    sqlite3 *db = (sqlite3 *)s->db;
    int rc;
    static const char *SQL =
        "UPDATE scheduler_tasks SET enabled=?, next_due_unix=?, last_run_unix=?, last_rc=?, run_count=?, updated_at=? WHERE id=?;";
    if (!s || !s->db || !t || t->id <= 0) return -1;
    rc = sqlite3_prepare_v2(db, SQL, -1, &stmt, NULL);
    if (rc != SQLITE_OK) return -2;
    sqlite3_bind_int(stmt, 1, t->enabled ? 1 : 0);
    sqlite3_bind_int64(stmt, 2, (sqlite3_int64)t->next_due_unix);
    sqlite3_bind_int64(stmt, 3, (sqlite3_int64)t->last_run_unix);
    sqlite3_bind_int(stmt, 4, t->last_rc);
    sqlite3_bind_int(stmt, 5, t->run_count);
    sqlite3_bind_int64(stmt, 6, (sqlite3_int64)time(NULL));
    sqlite3_bind_int64(stmt, 7, (sqlite3_int64)t->id);
    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    return rc == SQLITE_DONE ? 0 : -3;
}


static int format_unix_local_tz(time_t ts, const char *tz, char *out, size_t out_cap)
{
    struct tm tmv;
    if (!out || out_cap == 0) return -1;
    out[0] = '\0';
    if (ts <= 0) return 0;
    if (cron_localtime_tz((tz && *tz) ? tz : NULL, ts, &tmv) != 0) return -1;
    if (strftime(out, out_cap, "%Y-%m-%d %H:%M:%S %Z", &tmv) == 0) return -1;
    return 0;
}

static int scheduler_append_task_json(claw_response_t *resp, const claw_scheduler_task_t *t)
{
    char tmp[896];
    char next_due_local[128];
    char last_run_local[128];
    char created_local[128];
    char updated_local[128];
    int n;
    if (!resp || !t) return -1;
    (void)format_unix_local_tz(t->next_due_unix, t->timezone, next_due_local, sizeof(next_due_local));
    (void)format_unix_local_tz(t->last_run_unix, t->timezone, last_run_local, sizeof(last_run_local));
    (void)format_unix_local_tz(t->created_at, t->timezone, created_local, sizeof(created_local));
    (void)format_unix_local_tz(t->updated_at, t->timezone, updated_local, sizeof(updated_local));
    n = snprintf(tmp, sizeof(tmp),
        "{\"id\":%ld,\"enabled\":%d,\"paused\":%d,\"schedule_type\":\"%s\",\"interval_sec\":%d,\"run_at_unix\":%lld,\"cron_expr\":\"",
        t->id, t->enabled, t->paused, schedule_type_name(t->schedule_type), t->interval_sec, (long long)t->run_at_unix);
    if (n < 0 || (size_t)n >= sizeof(tmp)) return -1;
    if (claw_response_append(resp, tmp) != 0) return -1;
    if (append_json_escaped(resp, t->cron_expr) != 0 ||
        claw_response_append(resp, "\",\"timezone\":\"") != 0 ||
        append_json_escaped(resp, t->timezone) != 0 ||
        claw_response_append(resp, "\",\"kind\":\"") != 0 ||
        claw_response_append(resp, kind_name(t->kind)) != 0 ||
        claw_response_append(resp, "\",\"target\":\"") != 0 ||
        append_json_escaped(resp, t->target) != 0 ||
        claw_response_append(resp, "\",\"arg1\":\"") != 0 ||
        append_json_escaped(resp, t->arg1) != 0 ||
        claw_response_append(resp, "\",\"arg2\":\"") != 0 ||
        append_json_escaped(resp, t->arg2) != 0 ||
        claw_response_append(resp, "\",\"next_due_local\":\"") != 0 ||
        append_json_escaped(resp, next_due_local) != 0 ||
        claw_response_append(resp, "\",\"last_run_local\":\"") != 0 ||
        append_json_escaped(resp, last_run_local) != 0 ||
        claw_response_append(resp, "\",\"created_at_local\":\"") != 0 ||
        append_json_escaped(resp, created_local) != 0 ||
        claw_response_append(resp, "\",\"updated_at_local\":\"") != 0 ||
        append_json_escaped(resp, updated_local) != 0) return -1;
    n = snprintf(tmp, sizeof(tmp),
        "\",\"next_due_unix\":%lld,\"last_run_unix\":%lld,\"last_rc\":%d,\"run_count\":%d,\"created_at\":%lld,\"updated_at\":%lld}",
        (long long)t->next_due_unix, (long long)t->last_run_unix, t->last_rc, t->run_count,
        (long long)t->created_at, (long long)t->updated_at);
    if (n < 0 || (size_t)n >= sizeof(tmp)) return -1;
    return claw_response_append(resp, tmp);
}

void claw_scheduler_init(claw_scheduler_t *s, claw_dispatch_t *dispatch,
    claw_metrics_t *metrics, claw_trace_t *trace)
{
    if (!s) return;
    memset(s, 0, sizeof(*s));
    s->dispatch = dispatch;
    s->metrics = metrics;
    s->trace = trace;
    s->heartbeat_enabled = env_bool_default("CLAW_HEARTBEAT_ENABLED", 1);
    s->heartbeat_interval_sec = env_int_default("CLAW_HEARTBEAT_INTERVAL_SEC", 30, 1, 86400);
    s->heartbeat_stderr = env_bool_default("CLAW_HEARTBEAT_STDERR", 1);
    if (snprintf(s->db_path, sizeof(s->db_path), "%s", env_or_default("CLAW_RUNTIME_DB_PATH", "./state/runtime.db")) >= (int)sizeof(s->db_path)) s->db_path[0] = '\0';
    if (metrics) {
        metrics->heartbeat_enabled = s->heartbeat_enabled;
        metrics->heartbeat_interval_sec = s->heartbeat_interval_sec;
    }
}

const char *claw_scheduler_db_path(const claw_scheduler_t *s)
{
    return s ? s->db_path : "";
}

int claw_scheduler_reload_tasks(claw_scheduler_t *s)
{
    sqlite3_stmt *stmt = NULL;
    sqlite3 *db;
    int rc;
    static const char *SQL =
        "SELECT id, enabled, paused, schedule_type, interval_sec, run_at_unix, cron_expr, timezone, kind, target, arg1, arg2, next_due_unix, last_run_unix, last_rc, run_count, created_at, updated_at FROM scheduler_tasks ORDER BY id ASC LIMIT ?;";
    if (!s) return -1;
    rc = scheduler_ensure_db(s);
    if (rc != 0) return rc;
    db = (sqlite3 *)s->db;
    s->task_count = 0;
    rc = sqlite3_prepare_v2(db, SQL, -1, &stmt, NULL);
    if (rc != SQLITE_OK) return -2;
    sqlite3_bind_int(stmt, 1, CLAW_SCHEDULER_MAX_TASKS);
    while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
        claw_scheduler_task_t *t = &s->tasks[s->task_count];
        const unsigned char *schedule_type = sqlite3_column_text(stmt, 3);
        const unsigned char *cron_expr = sqlite3_column_text(stmt, 6);
        const unsigned char *timezone = sqlite3_column_text(stmt, 7);
        const unsigned char *kind = sqlite3_column_text(stmt, 8);
        const unsigned char *target = sqlite3_column_text(stmt, 9);
        const unsigned char *arg1 = sqlite3_column_text(stmt, 10);
        const unsigned char *arg2 = sqlite3_column_text(stmt, 11);
        memset(t, 0, sizeof(*t));
        t->id = (long)sqlite3_column_int64(stmt, 0);
        t->enabled = sqlite3_column_int(stmt, 1) ? 1 : 0;
        t->paused = sqlite3_column_int(stmt, 2) ? 1 : 0;
        t->schedule_type = parse_schedule_type((const char *)(schedule_type ? schedule_type : (const unsigned char *)"interval"));
        t->interval_sec = sqlite3_column_int(stmt, 4);
        t->run_at_unix = (time_t)sqlite3_column_int64(stmt, 5);
        snprintf(t->cron_expr, sizeof(t->cron_expr), "%s", cron_expr ? (const char *)cron_expr : "");
        snprintf(t->timezone, sizeof(t->timezone), "%s", timezone ? (const char *)timezone : default_scheduler_timezone());
        t->kind = parse_kind((const char *)(kind ? kind : (const unsigned char *)""));
        if (!t->kind || !t->schedule_type) continue;
        snprintf(t->target, sizeof(t->target), "%s", target ? (const char *)target : "");
        snprintf(t->arg1, sizeof(t->arg1), "%s", arg1 ? (const char *)arg1 : "");
        snprintf(t->arg2, sizeof(t->arg2), "%s", arg2 ? (const char *)arg2 : "");
        t->next_due_unix = (time_t)sqlite3_column_int64(stmt, 12);
        t->last_run_unix = (time_t)sqlite3_column_int64(stmt, 13);
        t->last_rc = sqlite3_column_int(stmt, 14);
        t->run_count = sqlite3_column_int(stmt, 15);
        t->created_at = (time_t)sqlite3_column_int64(stmt, 16);
        t->updated_at = (time_t)sqlite3_column_int64(stmt, 17);
        if (t->next_due_unix <= 0) {
            time_t due = 0;
            if (scheduler_compute_next_due(t, time(NULL), &due) == 0) t->next_due_unix = due;
        }
        if (++s->task_count >= CLAW_SCHEDULER_MAX_TASKS) break;
    }
    sqlite3_finalize(stmt);
    return rc == SQLITE_DONE || rc == SQLITE_ROW ? 0 : -3;
}

int claw_scheduler_tasks_json(const claw_scheduler_t *s, claw_response_t *resp)
{
    size_t i;
    if (!s || !resp) return -1;
    if (claw_response_append(resp, "[") != 0) return -1;
    for (i = 0; i < s->task_count; ++i) {
        if (i != 0 && claw_response_append(resp, ",") != 0) return -1;
        if (scheduler_append_task_json(resp, &s->tasks[i]) != 0) return -1;
    }
    return claw_response_append(resp, "]");
}

int claw_scheduler_task_get_json(const claw_scheduler_t *s, long id, claw_response_t *resp)
{
    size_t i;
    if (!s || !resp || id <= 0) return -1;
    for (i = 0; i < s->task_count; ++i) {
        if (s->tasks[i].id == id) return scheduler_append_task_json(resp, &s->tasks[i]);
    }
    return -2;
}

int claw_scheduler_status_json(const claw_scheduler_t *s, claw_response_t *resp)
{
    char tmp[640];
    char scheduler_now_local[128];
    time_t now = time(NULL);
    size_t i;
    size_t enabled_count = 0, paused_count = 0, oneshot_count = 0, cron_count = 0;
    if (!s || !resp) return -1;
    (void)format_unix_local_tz(now, default_scheduler_timezone(), scheduler_now_local, sizeof(scheduler_now_local));
    for (i = 0; i < s->task_count; ++i) {
        if (s->tasks[i].enabled) ++enabled_count;
        if (s->tasks[i].paused) ++paused_count;
        if (s->tasks[i].schedule_type == CLAW_SCHEDULE_ONESHOT) ++oneshot_count;
        if (s->tasks[i].schedule_type == CLAW_SCHEDULE_CRON) ++cron_count;
    }
    if (snprintf(tmp, sizeof(tmp),
        "{\"running\":%d,\"heartbeat_enabled\":%d,\"heartbeat_interval_sec\":%d,\"heartbeat_last_unix\":%lld,\"heartbeat_next_unix\":%lld,\"scheduler_now_unix\":%lld,\"scheduler_now_local\":\"",
        s->running, s->heartbeat_enabled, s->heartbeat_interval_sec,
        (long long)s->heartbeat_last_unix, (long long)s->heartbeat_next_unix, (long long)now) >= (int)sizeof(tmp)) return -1;
    if (claw_response_append(resp, tmp) != 0) return -1;
    if (append_json_escaped(resp, scheduler_now_local) != 0) return -1;
    if (claw_response_append(resp, "\",\"db_path\":\"") != 0) return -1;
    if (append_json_escaped(resp, s->db_path) != 0) return -1;
    if (snprintf(tmp, sizeof(tmp),
        "\",\"task_count\":%zu,\"enabled_count\":%zu,\"paused_count\":%zu,\"oneshot_count\":%zu,\"cron_count\":%zu,\"tasks\":",
        s->task_count, enabled_count, paused_count, oneshot_count, cron_count) >= (int)sizeof(tmp)) return -1;
    if (claw_response_append(resp, tmp) != 0) return -1;
    if (claw_scheduler_tasks_json(s, resp) != 0) return -1;
    return claw_response_append(resp, "}");
}

int claw_scheduler_task_upsert(claw_scheduler_t *s, long id, int enabled, int paused, const char *schedule_type,
    int interval_sec, time_t run_at_unix, const char *cron_expr, const char *timezone,
    const char *kind, const char *target, const char *arg1, const char *arg2)
{
    sqlite3_stmt *stmt = NULL;
    sqlite3 *db;
    time_t now = time(NULL);
    time_t next_due = 0;
    int rc;
    claw_task_kind_t k;
    claw_schedule_type_t st;
    const char *sched_name;
    if (!s || !kind || !target) return -1;
    k = parse_kind(kind);
    if (!k) return -2;
    st = parse_schedule_type(schedule_type);
    if (!st) return -3;
    switch (st) {
    case CLAW_SCHEDULE_INTERVAL:
        if (interval_sec < 1 || interval_sec > 2592000) return -4;
        next_due = now + interval_sec;
        cron_expr = "";
        run_at_unix = 0;
        break;
    case CLAW_SCHEDULE_ONESHOT:
        if (run_at_unix <= now) return -5;
        next_due = run_at_unix;
        interval_sec = 0;
        cron_expr = "";
        break;
    case CLAW_SCHEDULE_CRON:
        if (!cron_expr || !*cron_expr) return -6;
        if (!timezone) timezone = default_scheduler_timezone();
        if (cron_next_due(cron_expr, timezone, now, &next_due) != 0) return -6;
        interval_sec = 0;
        run_at_unix = 0;
        break;
    default:
        return -7;
    }
    sched_name = schedule_type_name(st);
    if (!timezone) timezone = default_scheduler_timezone();
    rc = scheduler_ensure_db(s);
    if (rc != 0) return rc;
    db = (sqlite3 *)s->db;
    if (id > 0) {
        static const char *SQL_UPD =
            "UPDATE scheduler_tasks SET enabled=?, paused=?, schedule_type=?, interval_sec=?, run_at_unix=?, cron_expr=?, timezone=?, kind=?, target=?, arg1=?, arg2=?, next_due_unix=?, updated_at=? WHERE id=?;";
        rc = sqlite3_prepare_v2(db, SQL_UPD, -1, &stmt, NULL);
        if (rc != SQLITE_OK) return -8;
        sqlite3_bind_int(stmt, 1, enabled ? 1 : 0);
        sqlite3_bind_int(stmt, 2, paused ? 1 : 0);
        sqlite3_bind_text(stmt, 3, sched_name, -1, SQLITE_TRANSIENT);
        sqlite3_bind_int(stmt, 4, interval_sec);
        sqlite3_bind_int64(stmt, 5, (sqlite3_int64)run_at_unix);
        sqlite3_bind_text(stmt, 6, cron_expr ? cron_expr : "", -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 7, timezone ? timezone : "", -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 8, kind, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 9, target, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 10, arg1 ? arg1 : "", -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 11, arg2 ? arg2 : "", -1, SQLITE_TRANSIENT);
        sqlite3_bind_int64(stmt, 12, enabled && !paused ? (sqlite3_int64)next_due : 0);
        sqlite3_bind_int64(stmt, 13, (sqlite3_int64)now);
        sqlite3_bind_int64(stmt, 14, (sqlite3_int64)id);
    } else {
        static const char *SQL_INS =
            "INSERT INTO scheduler_tasks(enabled, paused, schedule_type, interval_sec, run_at_unix, cron_expr, timezone, kind, target, arg1, arg2, next_due_unix, last_run_unix, last_rc, run_count, created_at, updated_at) VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?);";
        rc = sqlite3_prepare_v2(db, SQL_INS, -1, &stmt, NULL);
        if (rc != SQLITE_OK) return -9;
        sqlite3_bind_int(stmt, 1, enabled ? 1 : 0);
        sqlite3_bind_int(stmt, 2, paused ? 1 : 0);
        sqlite3_bind_text(stmt, 3, sched_name, -1, SQLITE_TRANSIENT);
        sqlite3_bind_int(stmt, 4, interval_sec);
        sqlite3_bind_int64(stmt, 5, (sqlite3_int64)run_at_unix);
        sqlite3_bind_text(stmt, 6, cron_expr ? cron_expr : "", -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 7, timezone ? timezone : "", -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 8, kind, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 9, target, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 10, arg1 ? arg1 : "", -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 11, arg2 ? arg2 : "", -1, SQLITE_TRANSIENT);
        sqlite3_bind_int64(stmt, 12, enabled && !paused ? (sqlite3_int64)next_due : 0);
        sqlite3_bind_int64(stmt, 13, 0);
        sqlite3_bind_int(stmt, 14, 0);
        sqlite3_bind_int(stmt, 15, 0);
        sqlite3_bind_int64(stmt, 16, (sqlite3_int64)now);
        sqlite3_bind_int64(stmt, 17, (sqlite3_int64)now);
    }
    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    if (rc != SQLITE_DONE) return -10;
    return claw_scheduler_reload_tasks(s);
}

static claw_scheduler_task_t *scheduler_find_loaded_task(claw_scheduler_t *s, long id)
{
    size_t i;
    if (!s || id <= 0) return NULL;
    for (i = 0; i < s->task_count; ++i) {
        if (s->tasks[i].id == id) return &s->tasks[i];
    }
    return NULL;
}

static int scheduler_patch_flag(claw_scheduler_t *s, long id, const char *column, int value)
{
    sqlite3_stmt *stmt = NULL;
    sqlite3 *db;
    int rc;
    time_t now = time(NULL);
    time_t next_due = 0;
    claw_scheduler_task_t *t;
    const char *SQL;
    if (!s || id <= 0 || !column) return -1;
    rc = scheduler_ensure_db(s);
    if (rc != 0) return rc;
    (void)claw_scheduler_reload_tasks(s);
    t = scheduler_find_loaded_task(s, id);
    if (!t) return -2;
    db = (sqlite3 *)s->db;
    if ((strcmp(column, "enabled") == 0 && value == 1) || (strcmp(column, "paused") == 0 && value == 0)) {
        if (scheduler_compute_next_due(t, now, &next_due) != 0) next_due = 0;
    } else {
        next_due = t->next_due_unix;
    }
    SQL = strcmp(column, "enabled") == 0 ?
        "UPDATE scheduler_tasks SET enabled=?, updated_at=?, next_due_unix=? WHERE id=?;" :
        "UPDATE scheduler_tasks SET paused=?, updated_at=?, next_due_unix=? WHERE id=?;";
    rc = sqlite3_prepare_v2(db, SQL, -1, &stmt, NULL);
    if (rc != SQLITE_OK) return -3;
    sqlite3_bind_int(stmt, 1, value ? 1 : 0);
    sqlite3_bind_int64(stmt, 2, (sqlite3_int64)now);
    sqlite3_bind_int64(stmt, 3, (sqlite3_int64)next_due);
    sqlite3_bind_int64(stmt, 4, (sqlite3_int64)id);
    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    if (rc != SQLITE_DONE) return -4;
    return claw_scheduler_reload_tasks(s);
}

int claw_scheduler_task_set_enabled(claw_scheduler_t *s, long id, int enabled)
{
    return scheduler_patch_flag(s, id, "enabled", enabled ? 1 : 0);
}

int claw_scheduler_task_set_paused(claw_scheduler_t *s, long id, int paused)
{
    return scheduler_patch_flag(s, id, "paused", paused ? 1 : 0);
}

int claw_scheduler_task_delete(claw_scheduler_t *s, long id)
{
    sqlite3_stmt *stmt = NULL;
    sqlite3 *db;
    int rc;
    if (!s || id <= 0) return -1;
    rc = scheduler_ensure_db(s);
    if (rc != 0) return rc;
    db = (sqlite3 *)s->db;
    rc = sqlite3_prepare_v2(db, "DELETE FROM scheduler_tasks WHERE id=?;", -1, &stmt, NULL);
    if (rc != SQLITE_OK) return -2;
    sqlite3_bind_int64(stmt, 1, (sqlite3_int64)id);
    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    if (rc != SQLITE_DONE) return -3;
    return claw_scheduler_reload_tasks(s);
}

static void scheduler_heartbeat(claw_scheduler_t *s)
{
    time_t now = time(NULL);
    s->heartbeat_last_unix = now;
    s->heartbeat_next_unix = now + s->heartbeat_interval_sec;
    claw_metrics_record_heartbeat(s->metrics, now, s->heartbeat_next_unix, s->heartbeat_enabled, s->heartbeat_interval_sec);
    (void)claw_trace_write_scheduler(s->trace, s->metrics, "heartbeat", "runtime", 0, 0, "tick");
    if (s->heartbeat_stderr) {
        fprintf(stderr, "[heartbeat] ts=%lld next=%lld tasks=%zu db=%s\n",
            (long long)s->heartbeat_last_unix, (long long)s->heartbeat_next_unix, s->task_count, s->db_path);
    }
}

static int scheduler_run_task(claw_scheduler_t *s, claw_scheduler_task_t *t)
{
    char out[65536];
    claw_response_t resp;
    int rc;
    uint64_t started = monotonic_ms();
    claw_response_init(&resp, out, sizeof(out));
    if (t->kind == CLAW_TASK_CHAT) {
        rc = claw_dispatch_chat(s->dispatch, t->target, t->arg1, &resp);
    } else if (t->kind == CLAW_TASK_TOOL) {
        rc = claw_dispatch_tool_invoke(s->dispatch, t->target, t->arg1, &resp);
    } else {
        rc = claw_dispatch_memory_put(s->dispatch, t->target, t->arg1, t->arg2);
    }
    t->last_run_unix = time(NULL);
    t->last_rc = rc;
    t->run_count += 1;
    if (t->schedule_type == CLAW_SCHEDULE_INTERVAL) {
        t->next_due_unix = t->last_run_unix + t->interval_sec;
    } else if (t->schedule_type == CLAW_SCHEDULE_ONESHOT) {
        t->enabled = 0;
        t->next_due_unix = 0;
    } else if (scheduler_compute_next_due(t, t->last_run_unix, &t->next_due_unix) != 0) {
        t->next_due_unix = 0;
    }
    claw_metrics_record_scheduler_task(s->metrics, rc);
    (void)claw_trace_write_scheduler(s->trace, s->metrics, "task", t->target, rc,
        monotonic_ms() - started, t->kind == CLAW_TASK_MEMORY_PUT ? t->arg1 : (resp.buf ? resp.buf : ""));
    (void)scheduler_update_task_state(s, t);
    return rc;
}

int claw_scheduler_tick(claw_scheduler_t *s)
{
    size_t i;
    time_t now = time(NULL);
    if (!s || !s->dispatch) return -1;
    if (s->db == NULL && claw_scheduler_reload_tasks(s) != 0) return -2;
    if (s->heartbeat_enabled && (s->heartbeat_next_unix == 0 || now >= s->heartbeat_next_unix)) scheduler_heartbeat(s);
    for (i = 0; i < s->task_count; ++i) {
        if (!s->tasks[i].enabled || s->tasks[i].paused) continue;
        if (s->tasks[i].next_due_unix > 0 && now >= s->tasks[i].next_due_unix) (void)scheduler_run_task(s, &s->tasks[i]);
    }
    return 0;
}

int claw_scheduler_run(claw_scheduler_t *s)
{
    int epfd = -1, tfd = -1, rc = -1;
    struct epoll_event ev;
    struct itimerspec its;
    struct sigaction sa;
    if (!s || !s->dispatch) return -1;
    if (claw_scheduler_reload_tasks(s) != 0) return -2;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = scheduler_signal_handler;
    sigemptyset(&sa.sa_mask);
    if (sigaction(SIGINT, &sa, NULL) != 0) return -3;
    if (sigaction(SIGTERM, &sa, NULL) != 0) return -3;
    epfd = epoll_create1(EPOLL_CLOEXEC);
    if (epfd < 0) goto out;
    tfd = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);
    if (tfd < 0) goto out;
    memset(&its, 0, sizeof(its));
    its.it_value.tv_sec = 1;
    its.it_interval.tv_sec = 1;
    if (timerfd_settime(tfd, 0, &its, NULL) != 0) goto out;
    memset(&ev, 0, sizeof(ev));
    ev.events = EPOLLIN;
    ev.data.fd = tfd;
    if (epoll_ctl(epfd, EPOLL_CTL_ADD, tfd, &ev) != 0) goto out;
    g_scheduler_stop = 0;
    s->running = 1;
    claw_metrics_set_scheduler_running(s->metrics, 1);
    if (s->heartbeat_enabled && s->heartbeat_next_unix == 0) s->heartbeat_next_unix = time(NULL) + s->heartbeat_interval_sec;
    fprintf(stderr, "scheduler running: tasks=%zu heartbeat=%s interval=%d db=%s\n",
        s->task_count, s->heartbeat_enabled ? "on" : "off", s->heartbeat_interval_sec, s->db_path);
    while (!g_scheduler_stop) {
        struct epoll_event events[4];
        int n = epoll_wait(epfd, events, 4, 1000);
        int i;
        if (n < 0) {
            if (errno == EINTR) continue;
            goto out;
        }
        if (n == 0) { (void)claw_scheduler_tick(s); continue; }
        for (i = 0; i < n; ++i) {
            if (events[i].data.fd == tfd) {
                uint64_t expirations;
                while (read(tfd, &expirations, sizeof(expirations)) > 0) { }
                (void)claw_scheduler_tick(s);
            }
        }
    }
    rc = 0;
out:
    s->running = 0;
    claw_metrics_set_scheduler_running(s->metrics, 0);
    if (tfd >= 0) close(tfd);
    if (epfd >= 0) close(epfd);
    return rc;
}
