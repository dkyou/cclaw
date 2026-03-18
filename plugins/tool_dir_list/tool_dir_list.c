#define _GNU_SOURCE
#include "claw/plugin_api.h"
#include "claw/tool_types.h"
#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static const char *entry_type_name(unsigned char t) {
  switch (t) {
  case DT_DIR:
    return "dir";
  case DT_REG:
    return "file";
  case DT_LNK:
    return "link";
  default:
    return "other";
  }
}

static int tool_invoke(const char *json_args, claw_response_t *resp) {
  claw_fs_list_request_t req;
  char real[PATH_MAX];
  DIR *dir;
  struct dirent *ent;
  int first = 1, truncated = 0;
  long count = 0;

  if (claw_tt_fs_list_request_parse(json_args, &req, resp) != 0)
    return -2;
  if (claw_tt_resolve_realpath_or_cwd(req.path, real) != 0)
    return claw_tt_error_json(resp, "fs.list", "invalid_path", "path",
                              "path does not exist or cannot be resolved");
  if (!claw_tt_path_allowed(real))
    return claw_tt_error_json(resp, "fs.list", "path_rejected", "path",
                              "path rejected by allowed-roots policy");
  dir = opendir(real);
  if (!dir)
    return claw_tt_error_json(resp, "fs.list",
                              errno == ENOENT ? "not_found" : "io_error",
                              "path", strerror(errno));

  if (claw_response_append(
          resp, "{\"ok\":true,\"tool\":\"fs.list\",\"request\":{\"path\":\"") !=
          0 ||
      claw_tt_append_json_escaped(resp, req.path) != 0) {
    closedir(dir);
    return -1;
  }
  char tmp[64];
  snprintf(tmp, sizeof(tmp), "%ld", req.max_entries);
  if (claw_response_append(resp, "\",\"max_entries\":") != 0 ||
      claw_response_append(resp, tmp) != 0 ||
      claw_response_append(resp, "},\"result\":{\"path\":\"") != 0 ||
      claw_tt_append_json_escaped(resp, real) != 0 ||
      claw_response_append(resp, "\",\"entries\":[") != 0) {
    closedir(dir);
    return -1;
  }

  while ((ent = readdir(dir)) != NULL) {
    if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0)
      continue;
    if (count >= req.max_entries) {
      truncated = 1;
      break;
    }
    if (!first && claw_response_append(resp, ",") != 0) {
      closedir(dir);
      return -1;
    }
    first = 0;
    if (claw_response_append(resp, "{\"name\":\"") != 0 ||
        claw_tt_append_json_escaped(resp, ent->d_name) != 0 ||
        claw_response_append(resp, "\",\"type\":\"") != 0 ||
        claw_tt_append_json_escaped(resp, entry_type_name(ent->d_type)) != 0 ||
        claw_response_append(resp, "\"}") != 0) {
      closedir(dir);
      return -1;
    }
    ++count;
  }
  closedir(dir);
  snprintf(tmp, sizeof(tmp), "%ld", count);
  if (claw_response_append(resp, "],\"count\":") != 0 ||
      claw_response_append(resp, tmp) != 0 ||
      claw_response_append(resp, ",\"truncated\":") != 0 ||
      claw_response_append(resp, truncated ? "true" : "false") != 0 ||
      claw_response_append(resp, "}}") != 0)
    return -1;
  return 0;
}

static const char TOOL_REQUEST_SCHEMA[] =
    "{\"type\":\"object\",\"additionalProperties\":false,\"properties\":{"
    "\"path\":{\"type\":\"string\"},\"max_entries\":{\"type\":\"integer\","
    "\"minimum\":1,\"maximum\":2000}}}";
static const char TOOL_RESPONSE_SCHEMA[] =
    "{\"type\":\"object\",\"required\":[\"ok\",\"tool\",\"request\",\"result\"]"
    ",\"properties\":{\"ok\":{\"type\":\"boolean\"},\"tool\":{\"const\":\"fs."
    "list\"},\"request\":{\"type\":\"object\"},\"result\":{\"type\":\"object\"}"
    ",\"error\":{\"type\":\"object\"}}}";

static const claw_tool_api_t TOOL = {.name = "fs.list",
                                     .abi_name = "claw.tool.fs.list",
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
const claw_module_descriptor_t* claw_plugin_init(void) 
{
  return &DESC;
}
