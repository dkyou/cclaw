#ifndef CLAW_PLUGIN_API_H
#define CLAW_PLUGIN_API_H

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define CLAW_PLUGIN_ABI_VERSION 16u
#define CLAW_NAME_MAX 64

typedef enum {
    CLAW_MOD_PROVIDER = 1,
    CLAW_MOD_MEMORY   = 2,
    CLAW_MOD_TOOL     = 3,
    CLAW_MOD_CHANNEL  = 4
} claw_module_kind_t;

typedef struct {
    const char *input;
} claw_request_t;

typedef struct {
    char   *buf;
    size_t  cap;
    size_t  len;
} claw_response_t;

static inline void
claw_response_init(claw_response_t *r, char *buf, size_t cap)
{
    if (!r) return;
    r->buf = buf;
    r->cap = cap;
    r->len = 0;
    if (buf && cap > 0) buf[0] = '\0';
}

static inline int
claw_response_append_mem(claw_response_t *r, const char *s, size_t n)
{
    if (!r || !r->buf || (!s && n != 0)) return -1;
    if (r->len + n + 1 > r->cap) return -1;
    if (n) memcpy(r->buf + r->len, s, n);
    r->len += n;
    r->buf[r->len] = '\0';
    return 0;
}

static inline int
claw_response_append(claw_response_t *r, const char *s)
{
    if (!s) return -1;
    return claw_response_append_mem(r, s, strlen(s));
}

typedef struct {
    const char *name;
    int (*chat)(const claw_request_t *req, claw_response_t *resp);
} claw_provider_api_t;

typedef struct {
    const char *name;
    int (*put)(const char *key, const char *value);
    int (*get)(const char *key, claw_response_t *resp);
    int (*search)(const char *query, claw_response_t *resp);
} claw_memory_api_t;

typedef struct {
    const char *name;
    const char *abi_name;
    uint32_t abi_version;
    const char *request_schema_json;
    const char *response_schema_json;
    int (*invoke)(const char *json_args, claw_response_t *resp);
} claw_tool_api_t;

typedef struct claw_host_api {
    int (*list_modules)(claw_response_t *resp);
    int (*chat)(const char *provider, const char *message, claw_response_t *resp);
    int (*memory_put)(const char *memory, const char *key, const char *value);
    int (*memory_get)(const char *memory, const char *key, claw_response_t *resp);
    int (*memory_search)(const char *memory, const char *query, claw_response_t *resp);
    int (*tool_invoke)(const char *tool, const char *json_args, claw_response_t *resp);
    int (*tool_schemas)(claw_response_t *resp);
    int (*tool_schema_get)(const char *tool, claw_response_t *resp);
    int (*tool_validate)(const char *tool, const char *json_args, claw_response_t *resp);
    int (*runtime_schemas)(claw_response_t *resp);
    int (*runtime_schema_get)(const char *name, claw_response_t *resp);
    int (*openapi_json)(claw_response_t *resp);
    int (*metrics_json)(claw_response_t *resp);
    int (*metrics_prometheus)(claw_response_t *resp);
    int (*scheduler_status)(claw_response_t *resp);
    int (*scheduler_tasks_json)(claw_response_t *resp);
    int (*scheduler_task_get_json)(long id, claw_response_t *resp);
    int (*scheduler_tick)(void);
    int (*scheduler_task_upsert)(long id, int enabled, int paused, const char *schedule_type,
        int interval_sec, long run_at_unix, const char *cron_expr, const char *timezone,
        const char *kind, const char *target, const char *arg1, const char *arg2);
    int (*scheduler_task_delete)(long id);
    int (*scheduler_task_set_enabled)(long id, int enabled);
    int (*scheduler_task_set_paused)(long id, int paused);
} claw_host_api_t;

typedef struct {
    const char *name;
    int (*serve)(const claw_host_api_t *host);
    int (*stop)(void);
} claw_channel_api_t;

typedef union {
    const claw_provider_api_t *provider;
    const claw_memory_api_t   *memory;
    const claw_tool_api_t     *tool;
    const claw_channel_api_t  *channel;
    const void                *raw;
} claw_module_api_u;

typedef struct {
    uint32_t            abi_version;
    claw_module_kind_t  kind;
    claw_module_api_u   api;
} claw_module_descriptor_t;

typedef const claw_module_descriptor_t *(*claw_plugin_init_fn)(void);

#endif
