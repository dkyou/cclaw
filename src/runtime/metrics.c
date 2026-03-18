#include "claw/runtime_metrics.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

static uint64_t now_unix_seconds(void)
{
    return (uint64_t)time(NULL);
}

const char *claw_metrics_op_name(claw_op_kind_t op)
{
    switch (op) {
    case CLAW_OP_LIST: return "list";
    case CLAW_OP_CHAT: return "chat";
    case CLAW_OP_MEMORY_PUT: return "memory_put";
    case CLAW_OP_MEMORY_GET: return "memory_get";
    case CLAW_OP_MEMORY_SEARCH: return "memory_search";
    case CLAW_OP_TOOL_INVOKE: return "tool_invoke";
    default: return "unknown";
    }
}

void claw_metrics_init(claw_metrics_t *m)
{
    if (!m) return;
    memset(m, 0, sizeof(*m));
    m->started_at_unix = (time_t)now_unix_seconds();
}

uint64_t claw_metrics_uptime_seconds(const claw_metrics_t *m)
{
    uint64_t now;
    if (!m || m->started_at_unix == 0) return 0;
    now = now_unix_seconds();
    return now >= (uint64_t)m->started_at_unix ? now - (uint64_t)m->started_at_unix : 0;
}

void claw_metrics_record_dispatch(claw_metrics_t *m, claw_op_kind_t op, int rc, uint64_t duration_ms)
{
    if (!m || op < 0 || op >= CLAW_OP_COUNT) return;
    m->dispatch_total++;
    m->dispatch_by_op[op]++;
    m->dispatch_last_duration_ms[op] = duration_ms;
    if (rc != 0) {
        m->dispatch_error_total++;
        m->dispatch_errors_by_op[op]++;
    }
}

void claw_metrics_record_trace_write(claw_metrics_t *m, int ok)
{
    if (!m) return;
    if (ok) m->trace_events_total++;
    else m->trace_write_errors_total++;
}

void claw_metrics_record_heartbeat(claw_metrics_t *m, time_t now_unix, time_t next_unix, int enabled, int interval_sec)
{
    if (!m) return;
    m->heartbeat_total++;
    m->heartbeat_enabled = enabled;
    m->heartbeat_interval_sec = interval_sec;
    m->heartbeat_last_unix = now_unix;
    m->heartbeat_next_unix = next_unix;
}

void claw_metrics_set_scheduler_running(claw_metrics_t *m, int running)
{
    if (!m) return;
    m->scheduler_running = running ? 1 : 0;
}

void claw_metrics_record_scheduler_task(claw_metrics_t *m, int rc)
{
    if (!m) return;
    m->scheduler_task_runs_total++;
    if (rc != 0) m->scheduler_task_errors_total++;
}

static int append_uint64_field(claw_response_t *resp, const char *key, uint64_t value, int leading_comma)
{
    char tmp[128];
    int n = snprintf(tmp, sizeof(tmp), "%s\"%s\":%llu", leading_comma ? "," : "", key,
        (unsigned long long)value);
    if (n < 0 || (size_t)n >= sizeof(tmp)) return -1;
    return claw_response_append(resp, tmp);
}

static int append_int_field(claw_response_t *resp, const char *key, int value, int leading_comma)
{
    char tmp[128];
    int n = snprintf(tmp, sizeof(tmp), "%s\"%s\":%d", leading_comma ? "," : "", key, value);
    if (n < 0 || (size_t)n >= sizeof(tmp)) return -1;
    return claw_response_append(resp, tmp);
}

static int append_prom_one(claw_response_t *resp, const char *fmt, unsigned long long value)
{
    char tmp[160];
    int n = snprintf(tmp, sizeof(tmp), fmt, value);
    if (n < 0 || (size_t)n >= sizeof(tmp)) return -1;
    return claw_response_append(resp, tmp);
}

int claw_metrics_snapshot_json(const claw_metrics_t *m, claw_response_t *resp) {
  int i;
  if (!m || !resp)
    return -1;
  if (claw_response_append(resp, "{") != 0)
    return -1;
  if (append_uint64_field(resp, "uptime_sec", claw_metrics_uptime_seconds(m),
                          0) != 0)
    return -1;
  if (append_uint64_field(resp, "dispatch_total", m->dispatch_total, 1) != 0)
    return -1;
  if (append_uint64_field(resp, "dispatch_error_total", m->dispatch_error_total,
                          1) != 0)
    return -1;
  if (append_uint64_field(resp, "heartbeat_total", m->heartbeat_total, 1) != 0)
    return -1;
  if (append_uint64_field(resp, "scheduler_task_runs_total",
                          m->scheduler_task_runs_total, 1) != 0)
    return -1;
  if (append_uint64_field(resp, "scheduler_task_errors_total",
                          m->scheduler_task_errors_total, 1) != 0)
    return -1;
  if (append_uint64_field(resp, "trace_events_total", m->trace_events_total,
                          1) != 0)
    return -1;
  if (append_uint64_field(resp, "trace_write_errors_total",
                          m->trace_write_errors_total, 1) != 0)
    return -1;
  if (append_int_field(resp, "scheduler_running", m->scheduler_running, 1) != 0)
    return -1;
  if (append_int_field(resp, "heartbeat_enabled", m->heartbeat_enabled, 1) != 0)
    return -1;
  if (append_int_field(resp, "heartbeat_interval_sec",
                       m->heartbeat_interval_sec, 1) != 0)
    return -1;
  if (append_uint64_field(resp, "heartbeat_last_unix",
                          (uint64_t)m->heartbeat_last_unix, 1) != 0)
    return -1;
  if (append_uint64_field(resp, "heartbeat_next_unix",
                          (uint64_t)m->heartbeat_next_unix, 1) != 0)
    return -1;
  if (claw_response_append(resp, ",\"dispatch_by_op\":{") != 0)
    return -1;
  for (i = 0; i < CLAW_OP_COUNT; ++i) {
    char tmp[160];
    int n = snprintf(
        tmp, sizeof(tmp),
        "%s\"%s\":{\"total\":%llu,\"errors\":%llu,\"last_duration_ms\":%llu}",
        i == 0 ? "" : ",", claw_metrics_op_name((claw_op_kind_t)i),
        (unsigned long long)m->dispatch_by_op[i],
        (unsigned long long)m->dispatch_errors_by_op[i],
        (unsigned long long)m->dispatch_last_duration_ms[i]);
    if (n < 0 || (size_t)n >= sizeof(tmp))
      return -1;
    if (claw_response_append(resp, tmp) != 0)
      return -1;
  }
  if (claw_response_append(resp, "}}") != 0)
    return -1;
  return 0;
}

int claw_metrics_snapshot_prometheus(const claw_metrics_t *m,
                                     claw_response_t *resp) {
  int i;
  char tmp[256];
  if (!m || !resp)
    return -1;
  if (append_prom_one(resp, "cclaw_uptime_seconds %llu\n",
                      (unsigned long long)claw_metrics_uptime_seconds(m)) != 0)
    return -1;
  if (append_prom_one(resp, "cclaw_dispatch_total %llu\n",
                      (unsigned long long)m->dispatch_total) != 0)
    return -1;
  if (append_prom_one(resp, "cclaw_dispatch_error_total %llu\n",
                      (unsigned long long)m->dispatch_error_total) != 0)
    return -1;
  if (append_prom_one(resp, "cclaw_heartbeat_total %llu\n",
                      (unsigned long long)m->heartbeat_total) != 0)
    return -1;
  if (append_prom_one(resp, "cclaw_scheduler_task_runs_total %llu\n",
                      (unsigned long long)m->scheduler_task_runs_total) != 0)
    return -1;
  if (append_prom_one(resp, "cclaw_scheduler_task_errors_total %llu\n",
                      (unsigned long long)m->scheduler_task_errors_total) != 0)
    return -1;
  if (append_prom_one(resp, "cclaw_trace_events_total %llu\n",
                      (unsigned long long)m->trace_events_total) != 0)
    return -1;
  if (append_prom_one(resp, "cclaw_trace_write_errors_total %llu\n",
                      (unsigned long long)m->trace_write_errors_total) != 0)
    return -1;
  {
    int n = snprintf(tmp, sizeof(tmp),
                     "cclaw_scheduler_running %d\n"
                     "cclaw_heartbeat_enabled %d\n"
                     "cclaw_heartbeat_interval_seconds %d\n",
                     m->scheduler_running, m->heartbeat_enabled,
                     m->heartbeat_interval_sec);
    if (n < 0 || (size_t)n >= sizeof(tmp))
      return -1;
  }
  if (claw_response_append(resp, tmp) != 0)
    return -1;
  for (i = 0; i < CLAW_OP_COUNT; ++i) {
    int n = snprintf(tmp, sizeof(tmp),
                     "cclaw_dispatch_by_op_total{op=\"%s\"} %llu\n"
                     "cclaw_dispatch_by_op_errors_total{op=\"%s\"} %llu\n"
                     "cclaw_dispatch_last_duration_ms{op=\"%s\"} %llu\n",
                     claw_metrics_op_name((claw_op_kind_t)i),
                     (unsigned long long)m->dispatch_by_op[i],
                     claw_metrics_op_name((claw_op_kind_t)i),
                     (unsigned long long)m->dispatch_errors_by_op[i],
                     claw_metrics_op_name((claw_op_kind_t)i),
                     (unsigned long long)m->dispatch_last_duration_ms[i]);
    if (n < 0 || (size_t)n >= sizeof(tmp))
      return -1;
    if (claw_response_append(resp, tmp) != 0)
      return -1;
  }
  return 0;
}
