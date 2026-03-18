#ifndef CLAW_RUNTIME_TRACE_H
#define CLAW_RUNTIME_TRACE_H

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
#include <stdint.h>
#include <time.h>

#include "claw/runtime_metrics.h"

typedef struct {
    int enabled;
    char path[PATH_MAX];
    void *fp;
} claw_trace_t;

void claw_trace_init(claw_trace_t *t);
void claw_trace_close(claw_trace_t *t);
int claw_trace_write_dispatch(claw_trace_t *t, claw_metrics_t *m,
    claw_op_kind_t op, const char *target, int rc, uint64_t duration_ms, size_t response_bytes);
int claw_trace_write_scheduler(claw_trace_t *t, claw_metrics_t *m,
    const char *event, const char *target, int rc, uint64_t duration_ms, const char *detail);
const char *claw_trace_path(const claw_trace_t *t);

#endif
