#include "claw/core.h"
#include "claw/dispatch.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static claw_dispatch_t *g_dispatch = NULL;

static void usage(const char *prog)
{
    fprintf(stderr,
        "Usage:\n"
        "  %s <plugin_dir> list\n"
        "  %s <plugin_dir> chat <provider_name> <message>\n"
        "  %s <plugin_dir> mem-put <memory_name> <key> <value>\n"
        "  %s <plugin_dir> mem-get <memory_name> <key>\n"
        "  %s <plugin_dir> mem-search <memory_name> <query>\n"
        "  %s <plugin_dir> tool-invoke <tool_name> <json_args>\n"
        "  %s <plugin_dir> http-serve <channel_name>\n\n"
        "Examples:\n"
        "  %s build/plugins list\n"
        "  %s build/plugins chat echo \"hello runtime\"\n"
        "  %s build/plugins mem-put sqlite greeting hello\n"
        "  %s build/plugins mem-get sqlite greeting\n"
        "  %s build/plugins mem-search sqlite greet\n"
        "  %s build/plugins tool-invoke shell '{\"command\":\"pwd\"}'\n"
        "  CLAW_HTTP_PORT=8080 %s build/plugins http-serve http\n",
        prog, prog, prog, prog, prog, prog, prog,
        prog, prog, prog, prog, prog, prog, prog);
}

static int host_list_modules(claw_response_t *resp)
{
    if (!g_dispatch) return -1;
    return claw_dispatch_list_modules(g_dispatch, resp);
}

static int host_chat(const char *provider_name, const char *message, claw_response_t *resp)
{
    if (!g_dispatch) return -1;
    return claw_dispatch_chat(g_dispatch, provider_name, message, resp);
}

static int host_memory_put(const char *memory_name, const char *key, const char *value)
{
    if (!g_dispatch) return -1;
    return claw_dispatch_memory_put(g_dispatch, memory_name, key, value);
}

static int host_memory_get(const char *memory_name, const char *key, claw_response_t *resp)
{
    if (!g_dispatch) return -1;
    return claw_dispatch_memory_get(g_dispatch, memory_name, key, resp);
}

static int host_memory_search(const char *memory_name, const char *query, claw_response_t *resp)
{
    if (!g_dispatch) return -1;
    return claw_dispatch_memory_search(g_dispatch, memory_name, query, resp);
}

static int host_tool_invoke(const char *tool_name, const char *json_args, claw_response_t *resp)
{
    if (!g_dispatch) return -1;
    return claw_dispatch_tool_invoke(g_dispatch, tool_name, json_args, resp);
}

static const claw_host_api_t HOST_API = {
    .list_modules = host_list_modules,
    .chat = host_chat,
    .memory_put = host_memory_put,
    .memory_get = host_memory_get,
    .memory_search = host_memory_search,
    .tool_invoke = host_tool_invoke,
};

int main(int argc, char **argv)
{
    claw_registry_t registry;
    claw_dispatch_t dispatch;
    char outbuf[65536];
    claw_response_t resp;
    int rc = 0;

    if (argc < 3) {
        usage(argv[0]);
        return 1;
    }

    claw_registry_init(&registry);
    if (claw_registry_load_dir(&registry, argv[1]) != 0) {
        fprintf(stderr, "failed to load plugins from %s\n", argv[1]);
        claw_registry_destroy(&registry);
        return 2;
    }

    claw_dispatch_init(&dispatch, &registry);
    g_dispatch = &dispatch;
    claw_response_init(&resp, outbuf, sizeof(outbuf));

    if (strcmp(argv[2], "list") == 0) {
        rc = claw_dispatch_list_modules(&dispatch, &resp);
        if (rc != 0) {
            fprintf(stderr, "list failed: %d\n", rc);
            rc = 3;
            goto out;
        }
        puts(outbuf);
    } else if (strcmp(argv[2], "chat") == 0) {
        if (argc < 5) {
            usage(argv[0]);
            rc = 4;
            goto out;
        }
        rc = claw_dispatch_chat(&dispatch, argv[3], argv[4], &resp);
        if (rc != 0) {
            fprintf(stderr, "provider chat failed: %d\n", rc);
            if (outbuf[0] != '\0') fprintf(stderr, "provider error: %s\n", outbuf);
            rc = 5;
            goto out;
        }
        puts(outbuf);
    } else if (strcmp(argv[2], "mem-put") == 0) {
        if (argc < 6) {
            usage(argv[0]);
            rc = 6;
            goto out;
        }
        rc = claw_dispatch_memory_put(&dispatch, argv[3], argv[4], argv[5]);
        if (rc != 0) {
            fprintf(stderr, "memory put failed: %d\n", rc);
            rc = 7;
            goto out;
        }
        puts("OK");
    } else if (strcmp(argv[2], "mem-get") == 0) {
        if (argc < 5) {
            usage(argv[0]);
            rc = 8;
            goto out;
        }
        rc = claw_dispatch_memory_get(&dispatch, argv[3], argv[4], &resp);
        if (rc != 0) {
            fprintf(stderr, "memory get failed: %d\n", rc);
            rc = 9;
            goto out;
        }
        puts(outbuf);
    } else if (strcmp(argv[2], "mem-search") == 0) {
        if (argc < 5) {
            usage(argv[0]);
            rc = 10;
            goto out;
        }
        rc = claw_dispatch_memory_search(&dispatch, argv[3], argv[4], &resp);
        if (rc != 0) {
            fprintf(stderr, "memory search failed: %d\n", rc);
            rc = 11;
            goto out;
        }
        puts(outbuf);
    } else if (strcmp(argv[2], "tool-invoke") == 0) {
        if (argc < 5) {
            usage(argv[0]);
            rc = 12;
            goto out;
        }
        rc = claw_dispatch_tool_invoke(&dispatch, argv[3], argv[4], &resp);
        if (rc != 0) {
            fprintf(stderr, "tool invoke failed: %d\n", rc);
            if (outbuf[0] != '\0') fprintf(stderr, "tool error/output: %s\n", outbuf);
            rc = 13;
            goto out;
        }
        puts(outbuf);
    } else if (strcmp(argv[2], "http-serve") == 0) {
        const claw_channel_api_t *channel;
        if (argc < 4) {
            usage(argv[0]);
            rc = 14;
            goto out;
        }
        channel = claw_registry_get_channel(&registry, argv[3]);
        if (!channel) {
            fprintf(stderr, "channel not found: %s\n", argv[3]);
            rc = 15;
            goto out;
        }
        fprintf(stderr, "starting channel '%s'\n", channel->name);
        rc = channel->serve(&HOST_API);
        if (rc != 0) {
            fprintf(stderr, "channel serve failed: %d\n", rc);
            rc = 16;
            goto out;
        }
    } else {
        usage(argv[0]);
        rc = 17;
    }

out:
    g_dispatch = NULL;
    claw_registry_destroy(&registry);
    return rc;
}
