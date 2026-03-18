#ifndef CLAW_DISPATCH_H
#define CLAW_DISPATCH_H

#include "claw/core.h"

typedef struct {
    claw_registry_t *registry;
} claw_dispatch_t;

void claw_dispatch_init(claw_dispatch_t *d, claw_registry_t *registry);

int claw_dispatch_list_modules(claw_dispatch_t *d, claw_response_t *resp);
int claw_dispatch_chat(claw_dispatch_t *d, const char *provider, const char *message, claw_response_t *resp);
int claw_dispatch_memory_put(claw_dispatch_t *d, const char *memory, const char *key, const char *value);
int claw_dispatch_memory_get(claw_dispatch_t *d, const char *memory, const char *key, claw_response_t *resp);
int claw_dispatch_memory_search(claw_dispatch_t *d, const char *memory, const char *query, claw_response_t *resp);
int claw_dispatch_tool_invoke(claw_dispatch_t *d, const char *tool, const char *json_args, claw_response_t *resp);

#endif
