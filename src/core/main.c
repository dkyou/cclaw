#include "claw/core.h"
#include "claw/dispatch.h"
#include "claw/runtime_metrics.h"
#include "claw/runtime_trace.h"
#include "claw/scheduler.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static claw_dispatch_t *g_dispatch = NULL;
static claw_metrics_t *g_metrics = NULL;
static claw_scheduler_t *g_scheduler = NULL;
static int g_runtime_serving = 0;

static void usage(const char *prog)
{
    fprintf(stderr, "Usage:\n");
    fprintf(stderr, "  %s <plugin_dir> list\n", prog);
    fprintf(stderr, "  %s <plugin_dir> metrics\n", prog);
    fprintf(stderr, "  %s <plugin_dir> metrics-prom\n", prog);
    fprintf(stderr, "  %s <plugin_dir> scheduler-status\n", prog);
    fprintf(stderr, "  %s <plugin_dir> scheduler-tasks\n", prog);
    fprintf(stderr, "  %s <plugin_dir> scheduler-get <id>\n", prog);
    fprintf(stderr, "  %s <plugin_dir> scheduler-add <interval_sec> <kind> <target> <arg1> [arg2]\n", prog);
    fprintf(stderr, "  %s <plugin_dir> scheduler-add-oneshot <delay_sec> <kind> <target> <arg1> [arg2]\n", prog);
    fprintf(stderr, "  %s <plugin_dir> scheduler-add-cron <cron_expr> <kind> <target> <arg1> [arg2]\n", prog);
    fprintf(stderr, "  %s <plugin_dir> scheduler-add-cron-tz <cron_expr> <timezone> <kind> <target> <arg1> [arg2]\n", prog);
    fprintf(stderr, "  %s <plugin_dir> scheduler-upsert <id> <enabled> <paused> <interval_sec> <kind> <target> <arg1> [arg2]\n", prog);
    fprintf(stderr, "  %s <plugin_dir> scheduler-upsert-oneshot <id> <enabled> <paused> <delay_sec> <kind> <target> <arg1> [arg2]\n", prog);
    fprintf(stderr, "  %s <plugin_dir> scheduler-upsert-cron <id> <enabled> <paused> <cron_expr> <kind> <target> <arg1> [arg2]\n", prog);
    fprintf(stderr, "  %s <plugin_dir> scheduler-upsert-cron-tz <id> <enabled> <paused> <cron_expr> <timezone> <kind> <target> <arg1> [arg2]\n", prog);
    fprintf(stderr, "  %s <plugin_dir> scheduler-enable <id>\n", prog);
    fprintf(stderr, "  %s <plugin_dir> scheduler-disable <id>\n", prog);
    fprintf(stderr, "  %s <plugin_dir> scheduler-pause <id>\n", prog);
    fprintf(stderr, "  %s <plugin_dir> scheduler-resume <id>\n", prog);
    fprintf(stderr, "  %s <plugin_dir> scheduler-del <id>\n", prog);
    fprintf(stderr, "  %s <plugin_dir> scheduler-run\n", prog);
    fprintf(stderr, "  %s <plugin_dir> chat <provider_name> <message>\n", prog);
    fprintf(stderr, "  %s <plugin_dir> mem-put <memory_name> <key> <value>\n", prog);
    fprintf(stderr, "  %s <plugin_dir> mem-get <memory_name> <key>\n", prog);
    fprintf(stderr, "  %s <plugin_dir> mem-search <memory_name> <query>\n", prog);
    fprintf(stderr, "  %s <plugin_dir> tool-invoke <tool_name> <json_args>\n", prog);
    fprintf(stderr, "  %s <plugin_dir> tool-validate <tool_name> <json_args>\n", prog);
    fprintf(stderr, "  %s <plugin_dir> openapi\n", prog);
    fprintf(stderr, "  %s <plugin_dir> http-serve <channel_name>\n", prog);
    fprintf(stderr, "  %s <plugin_dir> runtime-serve <channel_name>\n\n", prog);
    fprintf(stderr, "Examples:\n");
    fprintf(stderr, "  %s build/plugins scheduler-add 10 chat echo \"hello interval\"\n", prog);
    fprintf(stderr, "  %s build/plugins scheduler-add-oneshot 5 chat echo \"hello once\"\n", prog);
    fprintf(stderr, "  %s build/plugins scheduler-add-cron \"*/5 * * * *\" chat echo \"hello cron\"\n", prog);
    fprintf(stderr, "  %s build/plugins tool-invoke shell '{\"argv\":[\"printf\",\"hi\"]}'\n", prog);
}

static int host_list_modules(claw_response_t *resp) {
  return g_dispatch ? claw_dispatch_list_modules(g_dispatch, resp) : -1;
}
static int host_chat(const char *provider_name, const char *message,
                     claw_response_t *resp) {
  return g_dispatch
             ? claw_dispatch_chat(g_dispatch, provider_name, message, resp)
             : -1;
}
static int host_memory_put(const char *memory_name, const char *key,
                           const char *value) {
  return g_dispatch
             ? claw_dispatch_memory_put(g_dispatch, memory_name, key, value)
             : -1;
}
static int host_memory_get(const char *memory_name, const char *key,
                           claw_response_t *resp) {
  return g_dispatch
             ? claw_dispatch_memory_get(g_dispatch, memory_name, key, resp)
             : -1;
}
static int host_memory_search(const char *memory_name, const char *query,
                              claw_response_t *resp) {
  return g_dispatch
             ? claw_dispatch_memory_search(g_dispatch, memory_name, query, resp)
             : -1;
}
static int host_tool_invoke(const char *tool_name, const char *json_args,
                            claw_response_t *resp) {
  return g_dispatch
             ? claw_dispatch_tool_invoke(g_dispatch, tool_name, json_args, resp)
             : -1;
}
static int host_tool_schemas(claw_response_t *resp) {
  return g_dispatch ? claw_dispatch_tool_schemas(g_dispatch, resp) : -1;
}
static int host_tool_schema_get(const char *tool_name, claw_response_t *resp) {
  return g_dispatch ? claw_dispatch_tool_schema_get(g_dispatch, tool_name, resp)
                    : -1;
}
static int host_tool_validate(const char *tool_name, const char *json_args,
                              claw_response_t *resp) {
  return g_dispatch ? claw_dispatch_tool_validate(g_dispatch, tool_name,
                                                  json_args, resp)
                    : -1;
}
static int host_runtime_schemas(claw_response_t *resp) {
  return g_dispatch ? claw_dispatch_runtime_schemas(g_dispatch, resp) : -1;
}
static int host_runtime_schema_get(const char *name, claw_response_t *resp) {
  return g_dispatch ? claw_dispatch_runtime_schema_get(g_dispatch, name, resp)
                    : -1;
}
static int host_openapi_json(claw_response_t *resp) {
  return g_dispatch ? claw_dispatch_openapi(g_dispatch, resp) : -1;
}
static int host_metrics_json(claw_response_t *resp) {
  return g_metrics ? claw_metrics_snapshot_json(g_metrics, resp) : -1;
}
static int host_metrics_prometheus(claw_response_t *resp) {
  return g_metrics ? claw_metrics_snapshot_prometheus(g_metrics, resp) : -1;
}

static int host_scheduler_status(claw_response_t *resp)
{
    if (!g_scheduler) return -1;
    g_scheduler->running = g_runtime_serving ? 1 : g_scheduler->running;
    return claw_scheduler_status_json(g_scheduler, resp);
}

static int host_scheduler_tasks_json(claw_response_t *resp)
{
    if (!g_scheduler) return -1;
    if (claw_scheduler_reload_tasks(g_scheduler) != 0) return -2;
    return claw_scheduler_tasks_json(g_scheduler, resp);
}

static int host_scheduler_task_get_json(long id, claw_response_t *resp)
{
    if (!g_scheduler) return -1;
    if (claw_scheduler_reload_tasks(g_scheduler) != 0) return -2;
    return claw_scheduler_task_get_json(g_scheduler, id, resp);
}

static int host_scheduler_tick(void)
{
    if (!g_scheduler) return -1;
    if (g_runtime_serving) {
        g_scheduler->running = 1;
        if (g_metrics) claw_metrics_set_scheduler_running(g_metrics, 1);
    }
    return claw_scheduler_tick(g_scheduler);
}

static int host_scheduler_task_upsert(long id, int enabled, int paused, const char *schedule_type,
    int interval_sec, long run_at_unix, const char *cron_expr, const char *timezone,
    const char *kind, const char *target, const char *arg1, const char *arg2)
{
    return g_scheduler ? claw_scheduler_task_upsert(g_scheduler, id, enabled, paused, schedule_type,
        interval_sec, (time_t)run_at_unix, cron_expr, timezone, kind, target, arg1, arg2) : -1;
}

static int host_scheduler_task_delete(long id) {
  return g_scheduler ? claw_scheduler_task_delete(g_scheduler, id) : -1;
}
static int host_scheduler_task_set_enabled(long id, int enabled) {
  return g_scheduler ? claw_scheduler_task_set_enabled(g_scheduler, id, enabled)
                     : -1;
}
static int host_scheduler_task_set_paused(long id, int paused) {
  return g_scheduler ? claw_scheduler_task_set_paused(g_scheduler, id, paused)
                     : -1;
}

static const claw_host_api_t HOST_API = {
    .list_modules = host_list_modules,
    .chat = host_chat,
    .memory_put = host_memory_put,
    .memory_get = host_memory_get,
    .memory_search = host_memory_search,
    .tool_invoke = host_tool_invoke,
    .tool_schemas = host_tool_schemas,
    .tool_schema_get = host_tool_schema_get,
    .tool_validate = host_tool_validate,
    .runtime_schemas = host_runtime_schemas,
    .runtime_schema_get = host_runtime_schema_get,
    .openapi_json = host_openapi_json,
    .metrics_json = host_metrics_json,
    .metrics_prometheus = host_metrics_prometheus,
    .scheduler_status = host_scheduler_status,
    .scheduler_tasks_json = host_scheduler_tasks_json,
    .scheduler_task_get_json = host_scheduler_task_get_json,
    .scheduler_tick = host_scheduler_tick,
    .scheduler_task_upsert = host_scheduler_task_upsert,
    .scheduler_task_delete = host_scheduler_task_delete,
    .scheduler_task_set_enabled = host_scheduler_task_set_enabled,
    .scheduler_task_set_paused = host_scheduler_task_set_paused,
};

static long parse_long_arg(const char *s, long fallback)
{
    char *end = NULL;
    long v;
    if (!s) return fallback;
    v = strtol(s, &end, 10);
    if (!end || *end != '\0') return fallback;
    return v;
}

int main(int argc, char **argv)
{
    claw_registry_t registry;
    claw_dispatch_t dispatch;
    claw_metrics_t metrics;
    claw_trace_t trace;
    claw_scheduler_t scheduler;
    char outbuf[65536];
    claw_response_t resp;
    int rc = 0;

    if (argc < 3) {
        usage(argv[0]);
        return 1;
    }

    claw_metrics_init(&metrics);
    claw_trace_init(&trace);
    claw_registry_init(&registry);
    if (claw_registry_load_dir(&registry, argv[1]) != 0) {
        fprintf(stderr, "failed to load plugins from %s\n", argv[1]);
        claw_registry_destroy(&registry);
        claw_trace_close(&trace);
        return 2;
    }

    claw_dispatch_init(&dispatch, &registry, &metrics, &trace);
    claw_scheduler_init(&scheduler, &dispatch, &metrics, &trace);
    (void)claw_scheduler_reload_tasks(&scheduler);
    g_dispatch = &dispatch;
    g_metrics = &metrics;
    g_scheduler = &scheduler;
    claw_response_init(&resp, outbuf, sizeof(outbuf));

    if (strcmp(argv[2], "list") == 0) {
        rc = claw_dispatch_list_modules(&dispatch, &resp);
        if (rc != 0) { fprintf(stderr, "list failed: %d\n", rc); rc = 3; goto out; }
        puts(outbuf);
    } else if (strcmp(argv[2], "tool-schemas") == 0) {
        rc = claw_dispatch_tool_schemas(&dispatch, &resp);
        if (rc != 0) { fprintf(stderr, "tool-schemas failed: %d\n", rc); rc = 4; goto out; }
        puts(outbuf);
    } else if (strcmp(argv[2], "tool-schema") == 0) {
        if (argc < 4) { usage(argv[0]); rc = 2; goto out; }
        rc = claw_dispatch_tool_schema_get(&dispatch, argv[3], &resp);
        if (rc != 0) { fprintf(stderr, "tool-schema failed: %d\n", rc); rc = 4; goto out; }
        puts(outbuf);
    } else if (strcmp(argv[2], "openapi") == 0) {
        rc = claw_dispatch_openapi(&dispatch, &resp);
        if (rc != 0) { fprintf(stderr, "openapi failed: %d\n", rc); rc = 4; goto out; }
        puts(outbuf);
    } else if (strcmp(argv[2], "tool-validate") == 0) {
        if (argc < 5) { usage(argv[0]); rc = 2; goto out; }
        rc = claw_dispatch_tool_validate(&dispatch, argv[3], argv[4], &resp);
        if (rc != 0 && outbuf[0] == '\0') { fprintf(stderr, "tool-validate failed: %d\n", rc); rc = 4; goto out; }
        puts(outbuf);
    } else if (strcmp(argv[2], "metrics") == 0) {
        rc = claw_metrics_snapshot_json(&metrics, &resp);
        if (rc != 0) { fprintf(stderr, "metrics failed: %d\n", rc); rc = 4; goto out; }
        puts(outbuf);
    } else if (strcmp(argv[2], "metrics-prom") == 0) {
        rc = claw_metrics_snapshot_prometheus(&metrics, &resp);
        if (rc != 0) { fprintf(stderr, "metrics-prom failed: %d\n", rc); rc = 5; goto out; }
        puts(outbuf);
    } else if (strcmp(argv[2], "scheduler-status") == 0) {
        rc = claw_scheduler_reload_tasks(&scheduler);
        if (rc != 0) { fprintf(stderr, "scheduler reload failed: %d\n", rc); rc = 6; goto out; }
        rc = claw_scheduler_status_json(&scheduler, &resp);
        if (rc != 0) { fprintf(stderr, "scheduler status failed: %d\n", rc); rc = 7; goto out; }
        puts(outbuf);
    } else if (strcmp(argv[2], "scheduler-tasks") == 0) {
        rc = claw_scheduler_reload_tasks(&scheduler);
        if (rc != 0) { fprintf(stderr, "scheduler reload failed: %d\n", rc); rc = 8; goto out; }
        rc = claw_scheduler_tasks_json(&scheduler, &resp);
        if (rc != 0) { fprintf(stderr, "scheduler tasks failed: %d\n", rc); rc = 9; goto out; }
        puts(outbuf);
    } else if (strcmp(argv[2], "scheduler-get") == 0) {
        long id;
        if (argc < 4) { usage(argv[0]); rc = 10; goto out; }
        id = parse_long_arg(argv[3], -1);
        rc = claw_scheduler_reload_tasks(&scheduler);
        if (rc != 0) { fprintf(stderr, "scheduler reload failed: %d\n", rc); rc = 11; goto out; }
        rc = claw_scheduler_task_get_json(&scheduler, id, &resp);
        if (rc != 0) { fprintf(stderr, "scheduler get failed: %d\n", rc); rc = 12; goto out; }
        puts(outbuf);
    } else if (strcmp(argv[2], "scheduler-add") == 0) {
        if (argc < 7) { usage(argv[0]); rc = 13; goto out; }
        rc = claw_scheduler_task_upsert(&scheduler, 0, 1, 0, "interval", (int)parse_long_arg(argv[3], -1), 0, "", "", argv[4], argv[5], argv[6], argc >= 8 ? argv[7] : "");
        if (rc != 0) { fprintf(stderr, "scheduler add failed: %d\n", rc); rc = 14; goto out; }
        puts("OK");
    } else if (strcmp(argv[2], "scheduler-add-oneshot") == 0) {
        time_t run_at;
        if (argc < 7) { usage(argv[0]); rc = 15; goto out; }
        run_at = time(NULL) + parse_long_arg(argv[3], -1);
        rc = claw_scheduler_task_upsert(&scheduler, 0, 1, 0, "oneshot", 0, run_at, "", "", argv[4], argv[5], argv[6], argc >= 8 ? argv[7] : "");
        if (rc != 0) { fprintf(stderr, "scheduler add-oneshot failed: %d\n", rc); rc = 16; goto out; }
        puts("OK");
    } else if (strcmp(argv[2], "scheduler-add-cron") == 0) {
        if (argc < 7) { usage(argv[0]); rc = 17; goto out; }
        rc = claw_scheduler_task_upsert(&scheduler, 0, 1, 0, "cron", 0, 0, argv[3], "", argv[4], argv[5], argv[6], argc >= 8 ? argv[7] : "");
        if (rc != 0) { fprintf(stderr, "scheduler add-cron failed: %d\n", rc); rc = 18; goto out; }
        puts("OK");
    } else if (strcmp(argv[2], "scheduler-add-cron-tz") == 0) {
        if (argc < 8) { usage(argv[0]); rc = 19; goto out; }
        rc = claw_scheduler_task_upsert(&scheduler, 0, 1, 0, "cron", 0, 0, argv[3], argv[4], argv[5], argv[6], argv[7], argc >= 9 ? argv[8] : "");
        if (rc != 0) { fprintf(stderr, "scheduler add-cron-tz failed: %d\n", rc); rc = 20; goto out; }
        puts("OK");
    } else if (strcmp(argv[2], "scheduler-upsert") == 0) {
        if (argc < 10) { usage(argv[0]); rc = 21; goto out; }
        rc = claw_scheduler_task_upsert(&scheduler, parse_long_arg(argv[3], -1), (int)parse_long_arg(argv[4], 1), (int)parse_long_arg(argv[5], 0),
            "interval", (int)parse_long_arg(argv[6], -1), 0, "", "", argv[7], argv[8], argv[9], argc >= 11 ? argv[10] : "");
        if (rc != 0) { fprintf(stderr, "scheduler upsert failed: %d\n", rc); rc = 22; goto out; }
        puts("OK");
    } else if (strcmp(argv[2], "scheduler-upsert-oneshot") == 0) {
        time_t run_at;
        if (argc < 10) { usage(argv[0]); rc = 23; goto out; }
        run_at = time(NULL) + parse_long_arg(argv[6], -1);
        rc = claw_scheduler_task_upsert(&scheduler, parse_long_arg(argv[3], -1), (int)parse_long_arg(argv[4], 1), (int)parse_long_arg(argv[5], 0),
            "oneshot", 0, run_at, "", "", argv[7], argv[8], argv[9], argc >= 11 ? argv[10] : "");
        if (rc != 0) { fprintf(stderr, "scheduler upsert-oneshot failed: %d\n", rc); rc = 24; goto out; }
        puts("OK");
    } else if (strcmp(argv[2], "scheduler-upsert-cron") == 0) {
        if (argc < 10) { usage(argv[0]); rc = 25; goto out; }
        rc = claw_scheduler_task_upsert(&scheduler, parse_long_arg(argv[3], -1), (int)parse_long_arg(argv[4], 1), (int)parse_long_arg(argv[5], 0),
            "cron", 0, 0, argv[6], "", argv[7], argv[8], argv[9], argc >= 11 ? argv[10] : "");
        if (rc != 0) { fprintf(stderr, "scheduler upsert-cron failed: %d\n", rc); rc = 26; goto out; }
        puts("OK");
    } else if (strcmp(argv[2], "scheduler-upsert-cron-tz") == 0) {
        if (argc < 11) { usage(argv[0]); rc = 27; goto out; }
        rc = claw_scheduler_task_upsert(&scheduler, parse_long_arg(argv[3], -1), (int)parse_long_arg(argv[4], 1), (int)parse_long_arg(argv[5], 0),
            "cron", 0, 0, argv[6], argv[7], argv[8], argv[9], argv[10], argc >= 12 ? argv[11] : "");
        if (rc != 0) { fprintf(stderr, "scheduler upsert-cron-tz failed: %d\n", rc); rc = 28; goto out; }
        puts("OK");
    } else if (strcmp(argv[2], "scheduler-enable") == 0 || strcmp(argv[2], "scheduler-disable") == 0) {
        long id;
        if (argc < 4) { usage(argv[0]); rc = 25; goto out; }
        id = parse_long_arg(argv[3], -1);
        rc = claw_scheduler_task_set_enabled(&scheduler, id, strcmp(argv[2], "scheduler-enable") == 0 ? 1 : 0);
        if (rc != 0) { fprintf(stderr, "scheduler enable/disable failed: %d\n", rc); rc = 26; goto out; }
        puts("OK");
    } else if (strcmp(argv[2], "scheduler-pause") == 0 || strcmp(argv[2], "scheduler-resume") == 0) {
        long id;
        if (argc < 4) { usage(argv[0]); rc = 27; goto out; }
        id = parse_long_arg(argv[3], -1);
        rc = claw_scheduler_task_set_paused(&scheduler, id, strcmp(argv[2], "scheduler-pause") == 0 ? 1 : 0);
        if (rc != 0) { fprintf(stderr, "scheduler pause/resume failed: %d\n", rc); rc = 28; goto out; }
        puts("OK");
    } else if (strcmp(argv[2], "scheduler-del") == 0) {
        long id;
        if (argc < 4) { usage(argv[0]); rc = 29; goto out; }
        id = parse_long_arg(argv[3], -1);
        rc = claw_scheduler_task_delete(&scheduler, id);
        if (rc != 0) { fprintf(stderr, "scheduler delete failed: %d\n", rc); rc = 30; goto out; }
        puts("OK");
    } else if (strcmp(argv[2], "scheduler-run") == 0) {
        rc = claw_scheduler_run(&scheduler);
        if (rc != 0) { fprintf(stderr, "scheduler run failed: %d\n", rc); rc = 31; goto out; }
    } else if (strcmp(argv[2], "chat") == 0) {
        if (argc < 5) { usage(argv[0]); rc = 32; goto out; }
        rc = claw_dispatch_chat(&dispatch, argv[3], argv[4], &resp);
        if (rc != 0) { fprintf(stderr, "provider chat failed: %d\n", rc); if (outbuf[0] != '\0') fprintf(stderr, "provider error: %s\n", outbuf); rc = 33; goto out; }
        puts(outbuf);
    } else if (strcmp(argv[2], "mem-put") == 0) {
        if (argc < 6) { usage(argv[0]); rc = 34; goto out; }
        rc = claw_dispatch_memory_put(&dispatch, argv[3], argv[4], argv[5]);
        if (rc != 0) { fprintf(stderr, "memory put failed: %d\n", rc); rc = 35; goto out; }
        puts("OK");
    } else if (strcmp(argv[2], "mem-get") == 0) {
        if (argc < 5) { usage(argv[0]); rc = 36; goto out; }
        rc = claw_dispatch_memory_get(&dispatch, argv[3], argv[4], &resp);
        if (rc != 0) { fprintf(stderr, "memory get failed: %d\n", rc); rc = 37; goto out; }
        puts(outbuf);
    } else if (strcmp(argv[2], "mem-search") == 0) {
        if (argc < 5) { usage(argv[0]); rc = 38; goto out; }
        rc = claw_dispatch_memory_search(&dispatch, argv[3], argv[4], &resp);
        if (rc != 0) { fprintf(stderr, "memory search failed: %d\n", rc); rc = 39; goto out; }
        puts(outbuf);
    } else if (strcmp(argv[2], "tool-invoke") == 0) {
        if (argc < 5) { usage(argv[0]); rc = 40; goto out; }
        rc = claw_dispatch_tool_invoke(&dispatch, argv[3], argv[4], &resp);
        if (rc != 0) { fprintf(stderr, "tool invoke failed: %d\n", rc); if (outbuf[0] != '\0') fprintf(stderr, "tool error/output: %s\n", outbuf); rc = 41; goto out; }
        puts(outbuf);
    } else if (strcmp(argv[2], "http-serve") == 0 || strcmp(argv[2], "runtime-serve") == 0) {
        const claw_channel_api_t *channel;
        if (argc < 4) { usage(argv[0]); rc = 42; goto out; }
        channel = claw_registry_get_channel(&registry, argv[3]);
        if (!channel) { fprintf(stderr, "channel not found: %s\n", argv[3]); rc = 43; goto out; }
        fprintf(stderr, "starting runtime channel '%s' with scheduler db=%s\n", channel->name, claw_scheduler_db_path(&scheduler));
        g_runtime_serving = strcmp(argv[2], "runtime-serve") == 0 ? 1 : 0;
        rc = channel->serve(&HOST_API);
        if (rc != 0) { fprintf(stderr, "channel serve failed: %d\n", rc); rc = 44; goto out; }
    } else {
        usage(argv[0]);
        rc = 45;
    }

out:
    g_runtime_serving = 0;
    g_scheduler = NULL;
    g_metrics = NULL;
    g_dispatch = NULL;
    claw_registry_destroy(&registry);
    claw_trace_close(&trace);
    return rc;
}
