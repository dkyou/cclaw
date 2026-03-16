#include "claw/core.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void usage(const char *prog)
{
    fprintf(stderr,
        "Usage:\n"
        "  %s <plugin_dir> chat <provider_name> <message>\n"
        "  %s <plugin_dir> mem-put <memory_name> <key> <value>\n"
        "  %s <plugin_dir> mem-get <memory_name> <key>\n\n"
        "Examples:\n"
        "  %s build/plugins chat echo \"hello runtime\"\n"
        "  %s build/plugins mem-put sqlite greeting hello\n"
        "  %s build/plugins mem-get sqlite greeting\n",
        prog, prog, prog, prog, prog, prog);
}

int main(int argc, char **argv)
{
    claw_registry_t registry;
    char outbuf[8192];
    claw_response_t resp;
    int rc = 0;

    if (argc < 4) {
        usage(argv[0]);
        return 1;
    }

    claw_registry_init(&registry);
    if (claw_registry_load_dir(&registry, argv[1]) != 0) {
        fprintf(stderr, "failed to load plugins from %s\n", argv[1]);
        claw_registry_destroy(&registry);
        return 2;
    }

    claw_response_init(&resp, outbuf, sizeof(outbuf));

    if (strcmp(argv[2], "chat") == 0) {
        const claw_provider_api_t *provider;
        claw_request_t req;
        if (argc < 5) {
            usage(argv[0]);
            rc = 3;
            goto out;
        }
        provider = claw_registry_get_provider(&registry, argv[3]);
        if (!provider) {
            fprintf(stderr, "provider not found: %s\n", argv[3]);
            rc = 4;
            goto out;
        }
        req.input = argv[4];
        rc = provider->chat(&req, &resp);
        if (rc != 0) {
            fprintf(stderr, "provider chat failed: %d\n", rc);
            if (outbuf[0] != '\0') {
                fprintf(stderr, "provider error: %s\n", outbuf);
            }
            rc = 5;
            goto out;
        }
        printf("%s\n", outbuf);
    } else if (strcmp(argv[2], "mem-put") == 0) {
        const claw_memory_api_t *memory;
        if (argc < 6) {
            usage(argv[0]);
            rc = 6;
            goto out;
        }
        memory = claw_registry_get_memory(&registry, argv[3]);
        if (!memory) {
            fprintf(stderr, "memory not found: %s\n", argv[3]);
            rc = 7;
            goto out;
        }
        rc = memory->put(argv[4], argv[5]);
        if (rc != 0) {
            fprintf(stderr, "memory put failed: %d\n", rc);
            rc = 8;
            goto out;
        }
        puts("OK");
    } else if (strcmp(argv[2], "mem-get") == 0) {
        const claw_memory_api_t *memory;
        if (argc < 5) {
            usage(argv[0]);
            rc = 9;
            goto out;
        }
        memory = claw_registry_get_memory(&registry, argv[3]);
        if (!memory) {
            fprintf(stderr, "memory not found: %s\n", argv[3]);
            rc = 10;
            goto out;
        }
        rc = memory->get(argv[4], &resp);
        if (rc != 0) {
            fprintf(stderr, "memory get failed: %d\n", rc);
            rc = 11;
            goto out;
        }
        printf("%s\n", outbuf);
    } else {
        usage(argv[0]);
        rc = 12;
    }

out:
    claw_registry_destroy(&registry);
    return rc;
}
