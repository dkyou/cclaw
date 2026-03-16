#ifndef CLAW_PLUGIN_API_H
#define CLAW_PLUGIN_API_H

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define CLAW_PLUGIN_ABI_VERSION 1u
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
claw_response_append(claw_response_t *r, const char *s)
{
    size_t n;
    if (!r || !r->buf || !s) return -1;
    n = strlen(s);
    if (r->len + n + 1 > r->cap) return -1;
    memcpy(r->buf + r->len, s, n);
    r->len += n;
    r->buf[r->len] = '\0';
    return 0;
}

typedef struct {
    const char *name;
    int (*chat)(const claw_request_t *req, claw_response_t *resp);
} claw_provider_api_t;

typedef struct {
    const char *name;
    int (*put)(const char *key, const char *value);
    int (*get)(const char *key, claw_response_t *resp);
} claw_memory_api_t;

typedef struct {
    const char *name;
    int (*invoke)(const char *json_args, claw_response_t *resp);
} claw_tool_api_t;

typedef struct {
    const char *name;
    int (*start)(void);
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
