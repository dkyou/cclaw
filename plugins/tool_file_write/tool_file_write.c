#define _GNU_SOURCE
#include "claw/plugin_api.h"
#include "claw/tool_types.h"
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int mkdirs_for_path(const char *path) {
  char tmp[PATH_MAX];
  char *p;
  if (!path || !*path)
    return -1;
  if (snprintf(tmp, sizeof(tmp), "%s", path) >= (int)sizeof(tmp))
    return -1;
  for (p = tmp + 1; *p; ++p) {
    if (*p == '/') {
      *p = '\0';
      if (mkdir(tmp, 0700) != 0 && errno != EEXIST)
        return -1;
      *p = '/';
    }
  }
  return 0;
}

static int tool_invoke(const char *json_args, claw_response_t *resp) {
  claw_fs_write_request_t req;
  char target[PATH_MAX], parent[PATH_MAX], real_parent[PATH_MAX];
  char *slash;
  int flags, fd;
  ssize_t n;
  if (claw_tt_fs_write_request_parse(json_args, &req, resp) != 0)
    return -2;
  if (claw_tt_realpath_maybe_missing(req.path, target) != 0) {
    return claw_tt_error_json(resp, "fs.write", "invalid_path", "path",
                              "path cannot be resolved");
  }
  snprintf(parent, sizeof(parent), "%s", target);
  slash = strrchr(parent, '/');
  if (!slash)
    return claw_tt_error_json(resp, "fs.write", "invalid_path", "path",
                              "path must include parent directory");
  *slash = '\0';
  if (!realpath(parent, real_parent))
    return claw_tt_error_json(resp, "fs.write", "invalid_path", "path",
                              "parent directory cannot be resolved");
  if (!claw_tt_path_allowed(real_parent))
    return claw_tt_error_json(resp, "fs.write", "path_rejected", "path",
                              "path rejected by allowed-roots policy");
  if (mkdirs_for_path(target) != 0)
    return claw_tt_error_json(resp, "fs.write", "io_error", NULL,
                              "mkdirs failed");
  flags = O_WRONLY | O_CREAT |
          (strcmp(req.mode, "append") == 0 ? O_APPEND : O_TRUNC);
  fd = open(target, flags, 0600);
  if (fd < 0)
    return claw_tt_error_json(resp, "fs.write", "io_error", NULL,
                              strerror(errno));
  n = write(fd, req.content, strlen(req.content));
  close(fd);
  if (n < 0)
    return claw_tt_error_json(resp, "fs.write", "io_error", NULL,
                              "write failed");
  char tmp[64];
  snprintf(tmp, sizeof(tmp), "%zd", n);
  if (claw_response_append(
          resp,
          "{\"ok\":true,\"tool\":\"fs.write\",\"request\":{\"path\":\"") != 0 ||
      claw_tt_append_json_escaped(resp, req.path) != 0 ||
      claw_response_append(resp, "\",\"mode\":\"") != 0 ||
      claw_tt_append_json_escaped(resp, req.mode) != 0 ||
      claw_response_append(resp, "\"},\"result\":{\"path\":\"") != 0 ||
      claw_tt_append_json_escaped(resp, target) != 0 ||
      claw_response_append(resp, "\",\"bytes_written\":") != 0 ||
      claw_response_append(resp, tmp) != 0 ||
      claw_response_append(resp, ",\"mode\":\"") != 0 ||
      claw_tt_append_json_escaped(resp, req.mode) != 0 ||
      claw_response_append(resp, "\"}}") != 0)
    return -1;
  return 0;
}

static const char TOOL_REQUEST_SCHEMA[] =
    "{\"type\":\"object\",\"additionalProperties\":false,\"required\":["
    "\"path\",\"content\"],\"properties\":{\"path\":{\"type\":\"string\"},"
    "\"content\":{\"type\":\"string\"},\"mode\":{\"type\":\"string\",\"enum\":["
    "\"overwrite\",\"append\"]}}}";
static const char TOOL_RESPONSE_SCHEMA[] =
    "{\"type\":\"object\",\"required\":[\"ok\",\"tool\",\"request\",\"result\"]"
    ",\"properties\":{\"ok\":{\"type\":\"boolean\"},\"tool\":{\"const\":\"fs."
    "write\"},\"request\":{\"type\":\"object\"},\"result\":{\"type\":"
    "\"object\"},\"error\":{\"type\":\"object\"}}}";

static const claw_tool_api_t TOOL = {.name = "fs.write",
                                     .abi_name = "claw.tool.fs.write",
                                     .abi_version = 1u,
                                     .request_schema_json = TOOL_REQUEST_SCHEMA,
                                     .response_schema_json =
                                         TOOL_RESPONSE_SCHEMA,
                                     .invoke = tool_invoke};
static const claw_module_descriptor_t DESC = {.abi_version =
                                                  CLAW_PLUGIN_ABI_VERSION,
                                              .kind = CLAW_MOD_TOOL,
                                              .api.tool = &TOOL};
__attribute__((visibility("default"))) const claw_module_descriptor_t *
claw_plugin_init(void) {
  return &DESC;
}
