#include "claw/core.h"

#include <dirent.h>
#include <dlfcn.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

static int has_suffix(const char *s, const char *suffix)
{
    size_t ls, lf;
    if (!s || !suffix) return 0;
    ls = strlen(s);
    lf = strlen(suffix);
    if (ls < lf) return 0;
    return strcmp(s + ls - lf, suffix) == 0;
}

void claw_registry_init(claw_registry_t *r)
{
    if (!r) return;
    memset(r, 0, sizeof(*r));
}

void claw_registry_destroy(claw_registry_t *r)
{
    size_t i;
    if (!r) return;

    for (i = 0; i < r->handle_count; ++i) {
        if (r->handles[i]) dlclose(r->handles[i]);
    }
    memset(r, 0, sizeof(*r));
}

static int claw_registry_register_desc(
    claw_registry_t *r,
    const claw_module_descriptor_t *desc,
    void *handle)
{
    if (!r || !desc || !handle) return -1;
    if (desc->abi_version != CLAW_PLUGIN_ABI_VERSION) return -2;

    switch (desc->kind) {
    case CLAW_MOD_PROVIDER:
        if (!desc->api.provider || !desc->api.provider->name || !desc->api.provider->chat) return -6;
        if (r->provider_count >= CLAW_MAX_PROVIDERS) return -3;
        r->providers[r->provider_count++] = desc->api.provider;
        break;
    case CLAW_MOD_MEMORY:
        if (!desc->api.memory || !desc->api.memory->name || !desc->api.memory->put ||
            !desc->api.memory->get || !desc->api.memory->search) return -6;
        if (r->memory_count >= CLAW_MAX_MEMORYS) return -3;
        r->memories[r->memory_count++] = desc->api.memory;
        break;
    case CLAW_MOD_TOOL:
        if (!desc->api.tool || !desc->api.tool->name || !desc->api.tool->invoke) return -6;
        if (r->tool_count >= CLAW_MAX_TOOLS) return -3;
        r->tools[r->tool_count++] = desc->api.tool;
        break;
    case CLAW_MOD_CHANNEL:
        if (!desc->api.channel || !desc->api.channel->name || !desc->api.channel->serve) return -6;
        if (r->channel_count >= CLAW_MAX_CHANNELS) return -3;
        r->channels[r->channel_count++] = desc->api.channel;
        break;
    default:
        return -4;
    }

    if (r->handle_count >= CLAW_MAX_HANDLES) return -5;
    r->handles[r->handle_count++] = handle;
    return 0;
}

static int claw_registry_load_one(claw_registry_t *r, const char *fullpath)
{
    void *handle;
    claw_plugin_init_fn init_fn;
    const claw_module_descriptor_t *desc;

    handle = dlopen(fullpath, RTLD_NOW | RTLD_LOCAL);
    if (!handle) {
        fprintf(stderr, "dlopen failed for %s: %s\n", fullpath, dlerror());
        return -1;
    }

    dlerror();
    init_fn = (claw_plugin_init_fn)dlsym(handle, "claw_plugin_init");
    if (!init_fn) {
        fprintf(stderr, "dlsym(claw_plugin_init) failed for %s: %s\n", fullpath, dlerror());
        dlclose(handle);
        return -2;
    }

    desc = init_fn();
    if (!desc) {
        dlclose(handle);
        return -3;
    }

    if (claw_registry_register_desc(r, desc, handle) != 0) {
        dlclose(handle);
        return -4;
    }
    return 0;
}

int claw_registry_load_dir(claw_registry_t *r, const char *dirpath)
{
    DIR *dir;
    struct dirent *ent;
    char fullpath[PATH_MAX];

    if (!r || !dirpath) return -1;

    dir = opendir(dirpath);
    if (!dir) {
        perror("opendir");
        return -2;
    }

    while ((ent = readdir(dir)) != NULL) {
        if (ent->d_name[0] == '.') continue;
        if (!has_suffix(ent->d_name, ".so")) continue;

        if (snprintf(fullpath, sizeof(fullpath), "%s/%s", dirpath, ent->d_name) >= (int)sizeof(fullpath)) {
            fprintf(stderr, "plugin path too long: %s\n", ent->d_name);
            continue;
        }

        if (claw_registry_load_one(r, fullpath) != 0) {
            fprintf(stderr, "failed to load plugin: %s\n", fullpath);
        }
    }

    closedir(dir);
    return 0;
}

const claw_provider_api_t *claw_registry_get_provider(const claw_registry_t *r, const char *name)
{
    size_t i;
    if (!r || !name) return NULL;
    for (i = 0; i < r->provider_count; ++i) {
        if (r->providers[i] && r->providers[i]->name && strcmp(r->providers[i]->name, name) == 0) {
            return r->providers[i];
        }
    }
    return NULL;
}

const claw_memory_api_t *claw_registry_get_memory(const claw_registry_t *r, const char *name)
{
    size_t i;
    if (!r || !name) return NULL;
    for (i = 0; i < r->memory_count; ++i) {
        if (r->memories[i] && r->memories[i]->name && strcmp(r->memories[i]->name, name) == 0) {
            return r->memories[i];
        }
    }
    return NULL;
}

const claw_tool_api_t *claw_registry_get_tool(const claw_registry_t *r, const char *name)
{
    size_t i;
    if (!r || !name) return NULL;
    for (i = 0; i < r->tool_count; ++i) {
        if (r->tools[i] && r->tools[i]->name && strcmp(r->tools[i]->name, name) == 0) {
            return r->tools[i];
        }
    }
    return NULL;
}

const claw_channel_api_t *claw_registry_get_channel(const claw_registry_t *r, const char *name)
{
    size_t i;
    if (!r || !name) return NULL;
    for (i = 0; i < r->channel_count; ++i) {
        if (r->channels[i] && r->channels[i]->name && strcmp(r->channels[i]->name, name) == 0) {
            return r->channels[i];
        }
    }
    return NULL;
}
