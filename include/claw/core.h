#ifndef CLAW_CORE_H
#define CLAW_CORE_H

#include <stddef.h>
#include "claw/plugin_api.h"

#define CLAW_MAX_PROVIDERS 16
#define CLAW_MAX_MEMORYS   8
#define CLAW_MAX_TOOLS     32
#define CLAW_MAX_CHANNELS  8
#define CLAW_MAX_HANDLES   64

typedef struct {
    const claw_provider_api_t *providers[CLAW_MAX_PROVIDERS];
    size_t provider_count;

    const claw_memory_api_t *memories[CLAW_MAX_MEMORYS];
    size_t memory_count;

    const claw_tool_api_t *tools[CLAW_MAX_TOOLS];
    size_t tool_count;

    const claw_channel_api_t *channels[CLAW_MAX_CHANNELS];
    size_t channel_count;

    void *handles[CLAW_MAX_HANDLES];
    size_t handle_count;
} claw_registry_t;

void claw_registry_init(claw_registry_t *r);
void claw_registry_destroy(claw_registry_t *r);
int  claw_registry_load_dir(claw_registry_t *r, const char *dirpath);

const claw_provider_api_t *claw_registry_get_provider(
    const claw_registry_t *r, const char *name);
const claw_memory_api_t *claw_registry_get_memory(
    const claw_registry_t *r, const char *name);

#endif
