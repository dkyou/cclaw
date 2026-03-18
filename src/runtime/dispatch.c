#include "claw/dispatch.h"

#include <stdio.h>
#include <string.h>

void claw_dispatch_init(claw_dispatch_t *d, claw_registry_t *registry)
{
    if (!d) return;
    d->registry = registry;
}

static int append_json_array_names(claw_response_t *resp, const char *key, const char *const *names, size_t n)
{
    size_t i;
    if (claw_response_append(resp, "\"") != 0 ||
        claw_response_append(resp, key) != 0 ||
        claw_response_append(resp, "\":[") != 0) {
        return -1;
    }
    for (i = 0; i < n; ++i) {
        if (i != 0 && claw_response_append(resp, ",") != 0) return -1;
        if (claw_response_append(resp, "\"") != 0 ||
            claw_response_append(resp, names[i] ? names[i] : "") != 0 ||
            claw_response_append(resp, "\"") != 0) {
            return -1;
        }
    }
    if (claw_response_append(resp, "]") != 0) return -1;
    return 0;
}

int claw_dispatch_list_modules(claw_dispatch_t *d, claw_response_t *resp)
{
    size_t i;
    const char *names[CLAW_MAX_TOOLS];
    claw_registry_t *r;

    if (!d || !d->registry || !resp) return -1;
    r = d->registry;

    if (claw_response_append(resp, "{") != 0) return -1;

    for (i = 0; i < r->provider_count; ++i) {
        names[i] = r->providers[i] ? r->providers[i]->name : "";
    }
    if (append_json_array_names(resp, "providers", names, r->provider_count) != 0) return -1;
    if (claw_response_append(resp, ",") != 0) return -1;

    for (i = 0; i < r->memory_count; ++i) {
        names[i] = r->memories[i] ? r->memories[i]->name : "";
    }
    if (append_json_array_names(resp, "memories", names, r->memory_count) != 0) return -1;
    if (claw_response_append(resp, ",") != 0) return -1;

    for (i = 0; i < r->tool_count; ++i) {
        names[i] = r->tools[i] ? r->tools[i]->name : "";
    }
    if (append_json_array_names(resp, "tools", names, r->tool_count) != 0) return -1;
    if (claw_response_append(resp, ",") != 0) return -1;

    for (i = 0; i < r->channel_count; ++i) {
        names[i] = r->channels[i] ? r->channels[i]->name : "";
    }
    if (append_json_array_names(resp, "channels", names, r->channel_count) != 0) return -1;

    if (claw_response_append(resp, "}") != 0) return -1;
    return 0;
}

int claw_dispatch_chat(claw_dispatch_t *d, const char *provider_name, const char *message, claw_response_t *resp)
{
    const claw_provider_api_t *provider;
    claw_request_t req;
    if (!d || !d->registry || !provider_name || !message || !resp) return -1;
    provider = claw_registry_get_provider(d->registry, provider_name);
    if (!provider) return -2;
    req.input = message;
    return provider->chat(&req, resp);
}

int claw_dispatch_memory_put(claw_dispatch_t *d, const char *memory_name, const char *key, const char *value)
{
    const claw_memory_api_t *memory;
    if (!d || !d->registry || !memory_name || !key || !value) return -1;
    memory = claw_registry_get_memory(d->registry, memory_name);
    if (!memory) return -2;
    return memory->put(key, value);
}

int claw_dispatch_memory_get(claw_dispatch_t *d, const char *memory_name, const char *key, claw_response_t *resp)
{
    const claw_memory_api_t *memory;
    if (!d || !d->registry || !memory_name || !key || !resp) return -1;
    memory = claw_registry_get_memory(d->registry, memory_name);
    if (!memory) return -2;
    return memory->get(key, resp);
}

int claw_dispatch_memory_search(claw_dispatch_t *d, const char *memory_name, const char *query, claw_response_t *resp)
{
    const claw_memory_api_t *memory;
    if (!d || !d->registry || !memory_name || !query || !resp) return -1;
    memory = claw_registry_get_memory(d->registry, memory_name);
    if (!memory || !memory->search) return -2;
    return memory->search(query, resp);
}

int claw_dispatch_tool_invoke(claw_dispatch_t *d, const char *tool_name, const char *json_args, claw_response_t *resp)
{
    const claw_tool_api_t *tool;
    if (!d || !d->registry || !tool_name || !json_args || !resp) return -1;
    tool = claw_registry_get_tool(d->registry, tool_name);
    if (!tool) return -2;
    return tool->invoke(json_args, resp);
}
