#define _GNU_SOURCE
#include "claw/plugin_api.h"
#include "claw/tool_types.h"
#include <errno.h>
#include <stdio.h>
#include <string.h>

static int tool_invoke(const char *json_args, claw_response_t *resp) {
  claw_fs_read_request_t req;
  char real[PATH_MAX];
  FILE *fp;
  char buf[4096];
  size_t total = 0;
  int truncated = 0;
  claw_response_t content;
  char content_buf[1048577];

  if (claw_tt_fs_read_request_parse(json_args, &req, resp) != 0)
    return -2;
  if (claw_tt_resolve_realpath_or_cwd(req.path, real) != 0) {
    return claw_tt_error_json(resp, "fs.read", "invalid_path", "path",
                              "path does not exist or cannot be resolved");
  }
  if (!claw_tt_path_allowed(real)) {
    return claw_tt_error_json(resp, "fs.read", "path_rejected", "path",
                              "path rejected by allowed-roots policy");
  }
  fp = fopen(real, "rb");
  if (!fp) {
    return claw_tt_error_json(resp, "fs.read",
                              errno == ENOENT ? "not_found" : "io_error",
                              "path", strerror(errno));
  }
  claw_response_init(&content, content_buf, sizeof(content_buf));
  while (!feof(fp)) {
    size_t n = fread(buf, 1, sizeof(buf), fp);
    if (n > 0) {
      size_t keep = n;
      if ((long)(total + n) > req.max_bytes) {
        keep = (size_t)((long)req.max_bytes - (long)total);
        truncated = 1;
      }
      if (keep > 0 && claw_response_append_mem(&content, buf, keep) != 0) {
        fclose(fp);
        return claw_tt_error_json(resp, "fs.read", "payload_too_large", NULL,
                                  "response buffer exhausted");
      }
      total += keep;
      if ((long)total >= req.max_bytes)
        break;
    }
    if (ferror(fp)) {
      fclose(fp);
      return claw_tt_error_json(resp, "fs.read", "io_error", NULL,
                                "read failed");
    }
  }
  fclose(fp);

  if (claw_response_append(
          resp, "{\"ok\":true,\"tool\":\"fs.read\",\"request\":{\"path\":\"") !=
          0 ||
      claw_tt_append_json_escaped(resp, req.path) != 0 ||
      claw_response_append(resp, "\",\"max_bytes\":") != 0)
    return -1;
  char tmp[64];
  snprintf(tmp, sizeof(tmp), "%ld", req.max_bytes);
  if (claw_response_append(resp, tmp) != 0 ||
      claw_response_append(resp, "},\"result\":{\"path\":\"") != 0 ||
      claw_tt_append_json_escaped(resp, real) != 0 ||
      claw_response_append(resp, "\",\"bytes_read\":") != 0)
    return -1;
  snprintf(tmp, sizeof(tmp), "%zu", total);
  if (claw_response_append(resp, tmp) != 0 ||
      claw_response_append(resp, ",\"truncated\":") != 0 ||
      claw_response_append(resp, truncated ? "true" : "false") != 0 ||
      claw_response_append(resp, ",\"content\":\"") != 0 ||
      claw_tt_append_json_escaped(resp, content.buf) != 0 ||
      claw_response_append(resp, "\"}}") != 0)
    return -1;
  return 0;
}

static const char TOOL_REQUEST_SCHEMA[] =
    "{\"type\":\"object\",\"additionalProperties\":false,\"required\":["
    "\"path\"],\"properties\":{\"path\":{\"type\":\"string\"},\"max_bytes\":{"
    "\"type\":\"integer\",\"minimum\":1,\"maximum\":1048576}}}";
static const char TOOL_RESPONSE_SCHEMA[] =
    "{\"type\":\"object\",\"required\":[\"ok\",\"tool\",\"request\",\"result\"]"
    ",\"properties\":{\"ok\":{\"type\":\"boolean\"},\"tool\":{\"const\":\"fs."
    "read\"},\"request\":{\"type\":\"object\"},\"result\":{\"type\":\"object\"}"
    ",\"error\":{\"type\":\"object\"}}}";

static const claw_tool_api_t TOOL = {.name = "fs.read",
                                     .abi_name = "claw.tool.fs.read",
                                     .abi_version = 1u,
                                     .request_schema_json = TOOL_REQUEST_SCHEMA,
                                     .response_schema_json =
                                         TOOL_RESPONSE_SCHEMA,
                                     .invoke = tool_invoke};
static const claw_module_descriptor_t DESC = {.abi_version =
                                                  CLAW_PLUGIN_ABI_VERSION,
                                              .kind = CLAW_MOD_TOOL,
                                              .api.tool = &TOOL};
__attribute__((visibility("default"))) 
const claw_module_descriptor_t *claw_plugin_init(void) 
{
  return &DESC;
}
