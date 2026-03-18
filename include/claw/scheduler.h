#ifndef CLAW_SCHEDULER_H
#define CLAW_SCHEDULER_H

#ifndef _XOPEN_SOURCE
#define _XOPEN_SOURCE 700
#endif

#include <limits.h>
#ifndef PATH_MAX
#include <linux/limits.h>
#endif
#ifndef PATH_MAX
#define PATH_MAX 4096
#endif
#include <time.h>

#include "claw/dispatch.h"
#include "claw/runtime_metrics.h"
#include "claw/runtime_trace.h"

#define CLAW_SCHEDULER_MAX_TASKS 128
#define CLAW_SCHEDULER_STR 4096

typedef enum {
    CLAW_TASK_CHAT = 1,
    CLAW_TASK_TOOL = 2,
    CLAW_TASK_MEMORY_PUT = 3
} claw_task_kind_t;

typedef enum {
    CLAW_SCHEDULE_INTERVAL = 1,
    CLAW_SCHEDULE_ONESHOT = 2,
    CLAW_SCHEDULE_CRON = 3
} claw_schedule_type_t;

typedef struct {
    long id;
    int enabled;
    int paused;
    claw_schedule_type_t schedule_type;
    int interval_sec;
    time_t run_at_unix;
    char cron_expr[128];
    char timezone[64];
    claw_task_kind_t kind;
    char target[128];
    char arg1[CLAW_SCHEDULER_STR];
    char arg2[CLAW_SCHEDULER_STR];
    time_t next_due_unix;
    time_t last_run_unix;
    int last_rc;
    int run_count;
    time_t created_at;
    time_t updated_at;
} claw_scheduler_task_t;

typedef struct {
    claw_dispatch_t *dispatch;
    claw_metrics_t *metrics;
    claw_trace_t *trace;

    int running;
    int heartbeat_enabled;
    int heartbeat_interval_sec;
    int heartbeat_stderr;
    time_t heartbeat_last_unix;
    time_t heartbeat_next_unix;

    char db_path[PATH_MAX];
    void *db;
    claw_scheduler_task_t tasks[CLAW_SCHEDULER_MAX_TASKS];
    size_t task_count;
} claw_scheduler_t;

void claw_scheduler_init(claw_scheduler_t *s, claw_dispatch_t *dispatch,
    claw_metrics_t *metrics, claw_trace_t *trace);
int claw_scheduler_reload_tasks(claw_scheduler_t *s);
int claw_scheduler_status_json(const claw_scheduler_t *s, claw_response_t *resp);
int claw_scheduler_tasks_json(const claw_scheduler_t *s, claw_response_t *resp);
int claw_scheduler_task_get_json(const claw_scheduler_t *s, long id, claw_response_t *resp);
int claw_scheduler_task_upsert(claw_scheduler_t *s, long id, int enabled, int paused, const char *schedule_type,
    int interval_sec, time_t run_at_unix, const char *cron_expr, const char *timezone,
    const char *kind, const char *target, const char *arg1, const char *arg2);
int claw_scheduler_task_delete(claw_scheduler_t *s, long id);
int claw_scheduler_task_set_enabled(claw_scheduler_t *s, long id, int enabled);
int claw_scheduler_task_set_paused(claw_scheduler_t *s, long id, int paused);
int claw_scheduler_tick(claw_scheduler_t *s);
int claw_scheduler_run(claw_scheduler_t *s);
const char *claw_scheduler_db_path(const claw_scheduler_t *s);

#endif
