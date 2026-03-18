#define _GNU_SOURCE
#include "claw/plugin_api.h"
#include "claw/tool_types.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define CLAW_EXEC_DEFAULT_CPU_SEC 5L
#define CLAW_EXEC_DEFAULT_AS_MB 256L
#define CLAW_EXEC_DEFAULT_FSIZE_MB 8L
#define CLAW_EXEC_DEFAULT_NOFILE 32L

static long env_long_or_default(const char *name, long fallback) {
  const char *v = getenv(name);
  char *end = NULL;
  long n;
  if (!v || !*v)
    return fallback;
  n = strtol(v, &end, 10);
  if (!end || *end != '\0' || n <= 0)
    return fallback;
  return n;
}
static int env_bool_or_default(const char *name, int fallback) {
  const char *v = getenv(name);
  if (!v || !*v)
    return fallback;
  if (strcmp(v, "1") == 0 || strcmp(v, "true") == 0 || strcmp(v, "yes") == 0)
    return 1;
  if (strcmp(v, "0") == 0 || strcmp(v, "false") == 0 || strcmp(v, "no") == 0)
    return 0;
  return fallback;
}
static int set_cloexec(int fd) {
  int flags = fcntl(fd, F_GETFD, 0);
  if (flags < 0)
    return -1;
  return fcntl(fd, F_SETFD, flags | FD_CLOEXEC);
}
static int set_nonblock(int fd) {
  int flags = fcntl(fd, F_GETFL, 0);
  if (flags < 0)
    return -1;
  return fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}
static const char *path_basename_const(const char *s) {
  const char *slash;
  if (!s)
    return "";
  slash = strrchr(s, '/');
  return slash ? slash + 1 : s;
}
static int list_contains_token(const char *list, const char *token) {
  const char *p = list;
  size_t tok_len;
  if (!list || !*list || !token || !*token)
    return 0;
  tok_len = strlen(token);
  while (*p) {
    const char *start;
    size_t len = 0;
    while (*p == ' ' || *p == '\t' || *p == ',' || *p == ':')
      ++p;
    start = p;
    while (*p && *p != ',' && *p != ':' && *p != ' ' && *p != '\t') {
      ++p;
      ++len;
    }
    if (len == tok_len && strncmp(start, token, tok_len) == 0)
      return 1;
    while (*p == ' ' || *p == '\t')
      ++p;
    if (*p == ',' || *p == ':')
      ++p;
  }
  return 0;
}
static int enforce_program_policy(const char *program, claw_response_t *resp) {
  const char *allow = getenv("CLAW_TOOL_SHELL_COMMAND_ALLOWLIST");
  const char *deny = getenv("CLAW_TOOL_SHELL_COMMAND_DENYLIST");
  const char *base;
  if (!program || !*program) {
    return claw_tt_error_json(resp, "exec", "schema_validation", "argv",
                              "missing program");
  }
  base = path_basename_const(program);
  if (deny && *deny &&
      (list_contains_token(deny, program) || list_contains_token(deny, base))) {
    return claw_tt_error_json(resp, "exec", "command_rejected", "argv[0]",
                              "command rejected by denylist policy");
  }
  if (allow && *allow &&
      !(list_contains_token(allow, program) ||
        list_contains_token(allow, base))) {
    return claw_tt_error_json(resp, "exec", "command_rejected", "argv[0]",
                              "command rejected by allowlist policy");
  }
  return 0;
}
static int set_limit(int resource, rlim_t soft, rlim_t hard) {
  struct rlimit rl;
  rl.rlim_cur = soft;
  rl.rlim_max = hard;
  return setrlimit(resource, &rl);
}
static void child_apply_limits(void) {
  long cpu_sec = env_long_or_default("CLAW_TOOL_SHELL_CPU_LIMIT_SEC",
                                     CLAW_EXEC_DEFAULT_CPU_SEC);
  long as_mb = env_long_or_default("CLAW_TOOL_SHELL_AS_LIMIT_MB",
                                   CLAW_EXEC_DEFAULT_AS_MB);
  long fsize_mb = env_long_or_default("CLAW_TOOL_SHELL_FSIZE_LIMIT_MB",
                                      CLAW_EXEC_DEFAULT_FSIZE_MB);
  long nofile = env_long_or_default("CLAW_TOOL_SHELL_NOFILE_LIMIT",
                                    CLAW_EXEC_DEFAULT_NOFILE);
  if (cpu_sec > 0)
    (void)set_limit(RLIMIT_CPU, (rlim_t)cpu_sec, (rlim_t)cpu_sec);
  if (as_mb > 0)
    (void)set_limit(RLIMIT_AS, (rlim_t)as_mb * 1024 * 1024,
                    (rlim_t)as_mb * 1024 * 1024);
  if (fsize_mb > 0)
    (void)set_limit(RLIMIT_FSIZE, (rlim_t)fsize_mb * 1024 * 1024,
                    (rlim_t)fsize_mb * 1024 * 1024);
  if (nofile > 0)
    (void)set_limit(RLIMIT_NOFILE, (rlim_t)nofile, (rlim_t)nofile);
  (void)set_limit(RLIMIT_CORE, 0, 0);
}
static int find_env_slot(char keys[][CLAW_EXEC_ENV_KEY_CAP], size_t count,
                         const char *key) {
  size_t i;
  for (i = 0; i < count; ++i)
    if (strcmp(keys[i], key) == 0)
      return (int)i;
  return -1;
}
static int set_env_entry(char keys[][CLAW_EXEC_ENV_KEY_CAP],
                         char vals[][CLAW_EXEC_ENV_VAL_CAP], size_t *count,
                         const char *key, const char *value) {
  int idx;
  if (!keys || !vals || !count || !key || !value || !claw_tt_env_key_valid(key))
    return -1;
  idx = find_env_slot(keys, *count, key);
  if (idx < 0) {
    if (*count >= CLAW_EXEC_MAX_ENV + 8)
      return -2;
    idx = (int)(*count);
    *count += 1;
  }
  snprintf(keys[idx], CLAW_EXEC_ENV_KEY_CAP, "%s", key);
  snprintf(vals[idx], CLAW_EXEC_ENV_VAL_CAP, "%s", value);
  return 0;
}
static int build_envp(const char *cwd_real, const claw_env_kv_t extra[],
                      size_t extra_count, char keys[][CLAW_EXEC_ENV_KEY_CAP],
                      char vals[][CLAW_EXEC_ENV_VAL_CAP], char *envp[]) {
  size_t i, count = 0;
  if (set_env_entry(keys, vals, &count, "PATH", "/usr/bin:/bin") != 0)
    return -1;
  if (set_env_entry(keys, vals, &count, "LANG", "C") != 0)
    return -1;
  if (set_env_entry(keys, vals, &count, "HOME", cwd_real) != 0)
    return -1;
  if (set_env_entry(keys, vals, &count, "PWD", cwd_real) != 0)
    return -1;
  for (i = 0; i < extra_count; ++i)
    if (set_env_entry(keys, vals, &count, extra[i].key, extra[i].value) != 0)
      return -1;
  for (i = 0; i < count; ++i) {
    size_t k = strlen(keys[i]), v = strlen(vals[i]);
    if (k + 1 + v + 1 > CLAW_EXEC_ENV_VAL_CAP)
      return -1;
    memmove(vals[i] + k + 1, vals[i], v + 1);
    memcpy(vals[i], keys[i], k);
    vals[i][k] = '=';
    envp[i] = vals[i];
  }
  envp[count] = NULL;
  return 0;
}
static int drain_pipe_limited(int fd, char *buf, size_t *captured,
                              size_t max_output, int *eof, int *truncated) {
  char tmp[4096];
  for (;;) {
    size_t remaining, to_read;
    ssize_t n;
    if (!buf || !captured || !eof || !truncated)
      return -2;
    if (*captured >= max_output) {
      *truncated = 1;
      return 0;
    }
    remaining = max_output - *captured;
    to_read = remaining < sizeof(tmp) ? remaining : sizeof(tmp);
    n = read(fd, tmp, to_read);
    if (n > 0) {
      memcpy(buf + *captured, tmp, (size_t)n);
      *captured += (size_t)n;
      if (*captured >= max_output) {
        *truncated = 1;
        return 0;
      }
      continue;
    }
    if (n == 0) {
      *eof = 1;
      return 0;
    }
    if (errno == EINTR)
      continue;
    if (errno == EAGAIN || errno == EWOULDBLOCK)
      return 0;
    return -2;
  }
}

static int exec_tool_invoke(const char *json_args, claw_response_t *resp) {
  claw_exec_request_t req;
  claw_exec_response_t result;
  char cwd_real[PATH_MAX];
  char *argv_exec[CLAW_EXEC_MAX_ARGV + 1];
  int pipefd[2] = {-1, -1}, devnull = -1, child_done = 0, status = 0, eof = 0;
  pid_t pid;
  struct pollfd pfd;
  struct timespec start, now;
  if (!json_args || !resp)
    return -1;
  memset(&result, 0, sizeof(result));
  snprintf(result.mode, sizeof(result.mode), "argv");
  if (!env_bool_or_default("CLAW_TOOL_EXEC_ENABLE", 1))
    return claw_tt_error_json(resp, "exec", "disabled", NULL,
                              "tool exec disabled by CLAW_TOOL_EXEC_ENABLE=0");
  if (claw_tt_exec_request_parse(json_args, &req, resp) != 0)
    return -2;
  if (req.cwd[0] == '\0')
    strcpy(req.cwd, ".");
  if (claw_tt_resolve_realpath_or_cwd(req.cwd, cwd_real) != 0)
    return claw_tt_error_json(resp, "exec", "invalid_path", "cwd",
                              "invalid cwd");
  if (!claw_tt_path_allowed(cwd_real))
    return claw_tt_error_json(resp, "exec", "permission_denied", "cwd",
                              "cwd rejected by allowed-roots policy");
  for (size_t i = 0; i < req.argc; ++i)
    argv_exec[i] = req.argv[i];
  argv_exec[req.argc] = NULL;
  if (enforce_program_policy(argv_exec[0], resp) != 0)
    return -3;
  if (pipe(pipefd) != 0)
    return claw_tt_error_json(resp, "exec", "internal_error", NULL,
                              "pipe failed");
  if (set_cloexec(pipefd[0]) != 0 || set_cloexec(pipefd[1]) != 0 ||
      set_nonblock(pipefd[0]) != 0) {
    close(pipefd[0]);
    close(pipefd[1]);
    return claw_tt_error_json(resp, "exec", "internal_error", NULL,
                              "pipe setup failed");
  }
  devnull = open("/dev/null", O_RDONLY | O_CLOEXEC);
  if (devnull < 0) {
    close(pipefd[0]);
    close(pipefd[1]);
    return claw_tt_error_json(resp, "exec", "internal_error", NULL,
                              "open /dev/null failed");
  }
  pid = fork();
  if (pid < 0) {
    close(devnull);
    close(pipefd[0]);
    close(pipefd[1]);
    return claw_tt_error_json(resp, "exec", "internal_error", NULL,
                              "fork failed");
  }
  if (pid == 0) {
    char env_keys[CLAW_EXEC_MAX_ENV + 8][CLAW_EXEC_ENV_KEY_CAP];
    char env_storage[CLAW_EXEC_MAX_ENV + 8][CLAW_EXEC_ENV_VAL_CAP];
    char *envp[CLAW_EXEC_MAX_ENV + 9];
    int maxfd, fd;
    (void)setsid();
    umask(077);
    child_apply_limits();
    if (chdir(cwd_real) != 0)
      _exit(126);
    if (dup2(devnull, STDIN_FILENO) < 0)
      _exit(126);
    if (dup2(pipefd[1], STDOUT_FILENO) < 0)
      _exit(126);
    if (dup2(pipefd[1], STDERR_FILENO) < 0)
      _exit(126);
    close(devnull);
    close(pipefd[0]);
    close(pipefd[1]);
    maxfd = (int)sysconf(_SC_OPEN_MAX);
    if (maxfd < 16)
      maxfd = 256;
    for (fd = 3; fd < maxfd; ++fd)
      close(fd);
    if (build_envp(cwd_real, req.env, req.envc, env_keys, env_storage, envp) !=0)
      _exit(126);
    execvpe(argv_exec[0], argv_exec, envp);
    _exit(127);
  }
  close(devnull);
  close(pipefd[1]);
  pipefd[1] = -1;
  pfd.fd = pipefd[0];
  pfd.events = POLLIN | POLLHUP | POLLERR;
  clock_gettime(CLOCK_MONOTONIC, &start);
  while (!child_done || !eof) {
    int wait_ms, prc;
    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0)
      break;
    long elapsed = (long)((now.tv_sec - start.tv_sec) * 1000L +
                          (now.tv_nsec - start.tv_nsec) / 1000000L);
    long rem = req.timeout_ms - elapsed;
    if (rem <= 0) {
      result.timed_out = 1;
      (void)kill(-pid, SIGKILL);
      (void)kill(pid, SIGKILL);
      wait_ms = 50;
    } else
      wait_ms = rem > 200 ? 200 : (int)rem;
    prc = poll(&pfd, 1, wait_ms);
    if (prc > 0 && (pfd.revents & (POLLIN | POLLHUP | POLLERR))) {
      if (drain_pipe_limited(pipefd[0], result.output, &result.output_len,
                             CLAW_EXEC_DEFAULT_MAX_OUTPUT, &eof,
                             &result.truncated) != 0) {
        close(pipefd[0]);
        (void)kill(-pid, SIGKILL);
        (void)kill(pid, SIGKILL);
        (void)waitpid(pid, NULL, 0);
        return claw_tt_error_json(resp, "exec", "internal_error", NULL,
                                  "pipe read failed");
      }
      if (result.truncated) {
        (void)kill(-pid, SIGKILL);
        (void)kill(pid, SIGKILL);
      }
    }
    if (!child_done) {
      pid_t w = waitpid(pid, &status, WNOHANG);
      if (w == pid)
        child_done = 1;
    }
  }
  if (pipefd[0] >= 0)
    close(pipefd[0]);
  if (!child_done)
    (void)waitpid(pid, &status, 0);
  result.output[result.output_len] = '\0';
  if (WIFEXITED(status))
    result.exit_code = WEXITSTATUS(status);
  else if (WIFSIGNALED(status))
    result.signal_no = WTERMSIG(status);
  if (result.timed_out) {
    result.exit_code = 124;
  }
  if (claw_response_append(resp, "{\"ok\":") != 0)
    return -1;
  if (claw_response_append(resp, (result.exit_code == 0 && !result.timed_out)
                                     ? "true"
                                     : "false") != 0)
    return -1;
  if (claw_response_append(resp,
                           ",\"tool\":\"exec\",\"request\":{\"argv\":[") != 0)
    return -1;
  for (size_t i = 0; i < req.argc; ++i) {
    if (i && claw_response_append(resp, ",") != 0)
      return -1;
    if (claw_response_append(resp, "\"") != 0 ||
        claw_tt_append_json_escaped(resp, req.argv[i]) != 0 ||
        claw_response_append(resp, "\"") != 0)
      return -1;
  }
  char tmp[64];
  snprintf(tmp, sizeof(tmp), "%ld", req.timeout_ms);
  if (claw_response_append(resp, "],\"cwd\":\"") != 0 ||
      claw_tt_append_json_escaped(resp, cwd_real) != 0 ||
      claw_response_append(resp, "\",\"timeout_ms\":") != 0 ||
      claw_response_append(resp, tmp) != 0)
    return -1;
  snprintf(tmp, sizeof(tmp), "%zu", req.envc);
  if (claw_response_append(resp, ",\"env_count\":") != 0 ||
      claw_response_append(resp, tmp) != 0 ||
      claw_response_append(resp, "},\"result\":{\"mode\":\"") != 0 ||
      claw_tt_append_json_escaped(resp, result.mode) != 0)
    return -1;
  snprintf(tmp, sizeof(tmp), "%d", result.exit_code);
  if (claw_response_append(resp, "\",\"exit_code\":") != 0 ||
      claw_response_append(resp, tmp) != 0 ||
      claw_response_append(resp, ",\"timed_out\":") != 0 ||
      claw_response_append(resp, result.timed_out ? "true" : "false") != 0 ||
      claw_response_append(resp, ",\"truncated\":") != 0 ||
      claw_response_append(resp, result.truncated ? "true" : "false") != 0)
    return -1;
  snprintf(tmp, sizeof(tmp), "%d", result.signal_no);
  if (claw_response_append(resp, ",\"signal\":") != 0 ||
      claw_response_append(resp, tmp) != 0 ||
      claw_response_append(resp, ",\"output\":\"") != 0 ||
      claw_tt_append_json_escaped(resp, result.output) != 0 ||
      claw_response_append(resp, "\"}") != 0)
    return -1;
  if (result.exit_code != 0 || result.timed_out) {
    if (claw_response_append(resp, ",\"error\":{\"code\":\"") != 0)
      return -1;
    if (claw_response_append(resp, result.timed_out ? "execution_timeout"
                                                    : "execution_failed") != 0)
      return -1;
    if (claw_response_append(resp, "\",\"message\":\"") != 0)
      return -1;
    if (claw_tt_append_json_escaped(
            resp, result.timed_out
                      ? "process timed out"
                      : "process exited with non-zero status") != 0)
      return -1;
    if (claw_response_append(resp, "\"}") != 0)
      return -1;
  }
  if (claw_response_append(resp, "}") != 0)
    return -1;
  return result.exit_code == 0 && !result.timed_out ? 0 : -12;
}

static const char TOOL_REQUEST_SCHEMA[] =
    "{\"type\":\"object\",\"additionalProperties\":false,\"required\":["
    "\"argv\"],\"properties\":{\"argv\":{\"type\":\"array\",\"minItems\":1,"
    "\"items\":{\"type\":\"string\"}},\"env\":{\"type\":\"object\","
    "\"additionalProperties\":{\"type\":\"string\"}},\"cwd\":{\"type\":"
    "\"string\"},\"timeout_ms\":{\"type\":\"integer\",\"minimum\":1,"
    "\"maximum\":30000}}}";
static const char TOOL_RESPONSE_SCHEMA[] =
    "{\"type\":\"object\",\"required\":[\"ok\",\"tool\",\"request\",\"result\"]"
    ",\"properties\":{\"ok\":{\"type\":\"boolean\"},\"tool\":{\"const\":"
    "\"exec\"},\"request\":{\"type\":\"object\"},\"result\":{\"type\":"
    "\"object\"},\"error\":{\"type\":\"object\"}}}";

static const claw_tool_api_t TOOL = {.name = "exec",
                                     .abi_name = "claw.tool.exec",
                                     .abi_version = 1u,
                                     .request_schema_json = TOOL_REQUEST_SCHEMA,
                                     .response_schema_json =
                                         TOOL_RESPONSE_SCHEMA,
                                     .invoke = exec_tool_invoke};
static const claw_module_descriptor_t DESC = {.abi_version =
                                                  CLAW_PLUGIN_ABI_VERSION,
                                              .kind = CLAW_MOD_TOOL,
                                              .api.tool = &TOOL};
__attribute__((visibility("default"))) 
const claw_module_descriptor_t *claw_plugin_init(void) 
{
  return &DESC;
}
