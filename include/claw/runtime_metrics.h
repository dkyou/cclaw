#ifndef CLAW_RUNTIME_METRICS_H
#define CLAW_RUNTIME_METRICS_H

#include <stddef.h>
#include <stdint.h>
#include <time.h>

#include "claw/plugin_api.h"

typedef enum {
    CLAW_OP_LIST = 0,
    CLAW_OP_CHAT,
    CLAW_OP_MEMORY_PUT,
    CLAW_OP_MEMORY_GET,
    CLAW_OP_MEMORY_SEARCH,
    CLAW_OP_TOOL_INVOKE,
    CLAW_OP_COUNT
} claw_op_kind_t;

typedef struct {
    time_t started_at_unix;
    uint64_t dispatch_total;
    uint64_t dispatch_error_total;
    uint64_t dispatch_by_op[CLAW_OP_COUNT];
    uint64_t dispatch_errors_by_op[CLAW_OP_COUNT];
    uint64_t dispatch_last_duration_ms[CLAW_OP_COUNT];

    uint64_t heartbeat_total;
    uint64_t scheduler_task_runs_total;
    uint64_t scheduler_task_errors_total;

    uint64_t trace_events_total;
    uint64_t trace_write_errors_total;

    int scheduler_running;
    int heartbeat_enabled;
    int heartbeat_interval_sec;
    time_t heartbeat_last_unix;
    time_t heartbeat_next_unix;
} claw_metrics_t;

void claw_metrics_init(claw_metrics_t *m);
void claw_metrics_record_dispatch(claw_metrics_t *m, claw_op_kind_t op, int rc, uint64_t duration_ms);
void claw_metrics_record_trace_write(claw_metrics_t *m, int ok);
void claw_metrics_record_heartbeat(claw_metrics_t *m, time_t now_unix, time_t next_unix, int enabled, int interval_sec);
void claw_metrics_set_scheduler_running(claw_metrics_t *m, int running);
void claw_metrics_record_scheduler_task(claw_metrics_t *m, int rc);

const char *claw_metrics_op_name(claw_op_kind_t op);
uint64_t claw_metrics_uptime_seconds(const claw_metrics_t *m);
int claw_metrics_snapshot_json(const claw_metrics_t *m, claw_response_t *resp);
int claw_metrics_snapshot_prometheus(const claw_metrics_t *m, claw_response_t *resp);

#endif
