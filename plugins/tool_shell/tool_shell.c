#define _GNU_SOURCE
#include "claw/plugin_api.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
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

#define CLAW_TOOL_SHELL_DEFAULT_TIMEOUT_MS 5000L
#define CLAW_TOOL_SHELL_MAX_TIMEOUT_MS 30000L
#define CLAW_TOOL_SHELL_DEFAULT_MAX_OUTPUT 65536L
#define CLAW_TOOL_SHELL_DEFAULT_MAX_COMMAND 2048L
#define CLAW_TOOL_SHELL_DEFAULT_CPU_SEC 5L
#define CLAW_TOOL_SHELL_DEFAULT_AS_MB 256L
#define CLAW_TOOL_SHELL_DEFAULT_FSIZE_MB 8L
#define CLAW_TOOL_SHELL_DEFAULT_NOFILE 32L

static const char *skip_ws(const char *p)
{
    while (p && (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n')) ++p;
    return p;
}

static int hex_value(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return 10 + (c - 'a');
    if (c >= 'A' && c <= 'F') return 10 + (c - 'A');
    return -1;
}

static int utf8_from_codepoint(unsigned cp, char out[4], size_t *n)
{
    if (cp <= 0x7F) {
        out[0] = (char)cp; *n = 1;
    } else if (cp <= 0x7FF) {
        out[0] = (char)(0xC0 | (cp >> 6));
        out[1] = (char)(0x80 | (cp & 0x3F));
        *n = 2;
    } else if (cp <= 0xFFFF) {
        out[0] = (char)(0xE0 | (cp >> 12));
        out[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
        out[2] = (char)(0x80 | (cp & 0x3F));
        *n = 3;
    } else {
        out[0] = (char)(0xF0 | (cp >> 18));
        out[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
        out[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
        out[3] = (char)(0x80 | (cp & 0x3F));
        *n = 4;
    }
    return 0;
}

static const char *find_json_string_field(const char *json, const char *field)
{
    char key[128];
    const char *p;
    if (!json || !field) return NULL;
    if (snprintf(key, sizeof(key), "\"%s\"", field) >= (int)sizeof(key)) return NULL;
    p = strstr(json, key);
    if (!p) return NULL;
    p = strchr(p, ':');
    if (!p) return NULL;
    p = skip_ws(p + 1);
    if (*p != '"') return NULL;
    return p;
}

static int decode_json_string(const char *quoted, claw_response_t *out)
{
    const char *p = quoted;
    if (!p || *p != '"' || !out) return -1;
    ++p;
    while (*p) {
        if (*p == '"') return 0;
        if (*p == '\\') {
            char c;
            ++p;
            if (!*p) return -2;
            switch (*p) {
            case '"': c = '"'; break;
            case '\\': c = '\\'; break;
            case '/': c = '/'; break;
            case 'b': c = '\b'; break;
            case 'f': c = '\f'; break;
            case 'n': c = '\n'; break;
            case 'r': c = '\r'; break;
            case 't': c = '\t'; break;
            case 'u': {
                int h1, h2, h3, h4;
                unsigned cp;
                char utf8[4];
                size_t n = 0;
                if (!p[1] || !p[2] || !p[3] || !p[4]) return -3;
                h1 = hex_value(p[1]); h2 = hex_value(p[2]);
                h3 = hex_value(p[3]); h4 = hex_value(p[4]);
                if (h1 < 0 || h2 < 0 || h3 < 0 || h4 < 0) return -4;
                cp = (unsigned)((h1 << 12) | (h2 << 8) | (h3 << 4) | h4);
                utf8_from_codepoint(cp, utf8, &n);
                if (claw_response_append_mem(out, utf8, n) != 0) return -5;
                p += 4;
                ++p;
                continue;
            }
            default:
                return -6;
            }
            if (claw_response_append_mem(out, &c, 1) != 0) return -7;
        } else if (claw_response_append_mem(out, p, 1) != 0) {
            return -8;
        }
        ++p;
    }
    return -9;
}

static int json_get_string(const char *json, const char *field, char *out, size_t out_cap)
{
    claw_response_t resp;
    const char *quoted = find_json_string_field(json, field);
    if (!quoted || !out || out_cap == 0) return -1;
    claw_response_init(&resp, out, out_cap);
    return decode_json_string(quoted, &resp);
}

static int json_get_long(const char *json, const char *field, long *value_out)
{
    char key[128];
    const char *p;
    char *end = NULL;
    long v;
    if (!json || !field || !value_out) return -1;
    if (snprintf(key, sizeof(key), "\"%s\"", field) >= (int)sizeof(key)) return -1;
    p = strstr(json, key);
    if (!p) return -2;
    p = strchr(p, ':');
    if (!p) return -3;
    p = skip_ws(p + 1);
    if (*p == '"') {
        char tmp[64];
        claw_response_t resp;
        claw_response_init(&resp, tmp, sizeof(tmp));
        if (decode_json_string(p, &resp) != 0) return -4;
        v = strtol(tmp, &end, 10);
        if (!end || *end != '\0') return -5;
    } else {
        v = strtol(p, &end, 10);
        if (p == end) return -6;
    }
    *value_out = v;
    return 0;
}

static long env_long_or_default(const char *name, long fallback)
{
    const char *v = getenv(name);
    char *end = NULL;
    long n;
    if (!v || !*v) return fallback;
    n = strtol(v, &end, 10);
    if (!end || *end != '\0' || n <= 0) return fallback;
    return n;
}

static int env_bool_or_default(const char *name, int fallback)
{
    const char *v = getenv(name);
    if (!v || !*v) return fallback;
    if (strcmp(v, "1") == 0 || strcmp(v, "true") == 0 || strcmp(v, "yes") == 0) return 1;
    if (strcmp(v, "0") == 0 || strcmp(v, "false") == 0 || strcmp(v, "no") == 0) return 0;
    return fallback;
}

static long clamp_timeout_ms(long requested)
{
    long max_timeout = env_long_or_default("CLAW_TOOL_SHELL_MAX_TIMEOUT_MS", CLAW_TOOL_SHELL_MAX_TIMEOUT_MS);
    long timeout_ms = requested > 0 ? requested : env_long_or_default("CLAW_TOOL_SHELL_TIMEOUT_MS", CLAW_TOOL_SHELL_DEFAULT_TIMEOUT_MS);
    if (timeout_ms <= 0) timeout_ms = CLAW_TOOL_SHELL_DEFAULT_TIMEOUT_MS;
    if (max_timeout <= 0) max_timeout = CLAW_TOOL_SHELL_MAX_TIMEOUT_MS;
    if (timeout_ms > max_timeout) timeout_ms = max_timeout;
    return timeout_ms;
}

static int append_status_line(claw_response_t *resp, const char *prefix, int value)
{
    char tmp[64];
    if (claw_response_append(resp, "\n[") != 0) return -1;
    if (claw_response_append(resp, prefix) != 0) return -1;
    if (snprintf(tmp, sizeof(tmp), "%d", value) <= 0) return -1;
    if (claw_response_append(resp, tmp) != 0) return -1;
    if (claw_response_append(resp, "]") != 0) return -1;
    return 0;
}

static int append_note(claw_response_t *resp, const char *note)
{
    if (claw_response_append(resp, "\n[") != 0) return -1;
    if (claw_response_append(resp, note) != 0) return -1;
    if (claw_response_append(resp, "]") != 0) return -1;
    return 0;
}

static int set_cloexec(int fd)
{
    int flags = fcntl(fd, F_GETFD, 0);
    if (flags < 0) return -1;
    return fcntl(fd, F_SETFD, flags | FD_CLOEXEC);
}

static int set_nonblock(int fd)
{
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0) return -1;
    return fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

static int path_is_under_root(const char *path, const char *root)
{
    size_t root_len;
    if (!path || !root) return 0;
    root_len = strlen(root);
    if (root_len == 0) return 0;
    if (strncmp(path, root, root_len) != 0) return 0;
    if (root[root_len - 1] == '/') return 1;
    return path[root_len] == '\0' || path[root_len] == '/';
}

static int resolve_realpath_or_cwd(const char *input, char out[PATH_MAX])
{
    if (!input || !*input) {
        if (!getcwd(out, PATH_MAX)) return -1;
        return 0;
    }
    if (!realpath(input, out)) return -1;
    return 0;
}

static int split_allowed_roots(char roots[][PATH_MAX], size_t *count)
{
    const char *env = getenv("CLAW_TOOL_SHELL_ALLOWED_ROOTS");
    char cwd[PATH_MAX];
    size_t n = 0;

    if (!count) return -1;
    *count = 0;

    if (!env || !*env) {
        if (!getcwd(cwd, sizeof(cwd))) return -1;
        strncpy(roots[0], cwd, PATH_MAX - 1);
        roots[0][PATH_MAX - 1] = '\0';
        *count = 1;
        return 0;
    }

    while (*env && n < 16) {
        char token[PATH_MAX];
        size_t len = 0;
        while (env[len] && env[len] != ':') {
            if (len + 1 >= sizeof(token)) return -1;
            token[len] = env[len];
            ++len;
        }
        token[len] = '\0';
        if (len != 0) {
            if (!realpath(token, roots[n])) return -1;
            ++n;
        }
        env += len;
        if (*env == ':') ++env;
    }
    if (n == 0) return -1;
    *count = n;
    return 0;
}

static int cwd_allowed(const char *cwd_real)
{
    char roots[16][PATH_MAX];
    size_t i, n = 0;
    if (!cwd_real) return 0;
    if (split_allowed_roots(roots, &n) != 0) return 0;
    for (i = 0; i < n; ++i) {
        if (path_is_under_root(cwd_real, roots[i])) return 1;
    }
    return 0;
}

static int set_limit(int resource, rlim_t soft, rlim_t hard)
{
    struct rlimit rl;
    rl.rlim_cur = soft;
    rl.rlim_max = hard;
    return setrlimit(resource, &rl);
}

static void child_apply_limits(void)
{
    long cpu_sec = env_long_or_default("CLAW_TOOL_SHELL_CPU_LIMIT_SEC", CLAW_TOOL_SHELL_DEFAULT_CPU_SEC);
    long as_mb = env_long_or_default("CLAW_TOOL_SHELL_AS_LIMIT_MB", CLAW_TOOL_SHELL_DEFAULT_AS_MB);
    long fsize_mb = env_long_or_default("CLAW_TOOL_SHELL_FSIZE_LIMIT_MB", CLAW_TOOL_SHELL_DEFAULT_FSIZE_MB);
    long nofile = env_long_or_default("CLAW_TOOL_SHELL_NOFILE_LIMIT", CLAW_TOOL_SHELL_DEFAULT_NOFILE);

    if (cpu_sec < 1) cpu_sec = CLAW_TOOL_SHELL_DEFAULT_CPU_SEC;
    if (as_mb < 32) as_mb = CLAW_TOOL_SHELL_DEFAULT_AS_MB;
    if (fsize_mb < 1) fsize_mb = CLAW_TOOL_SHELL_DEFAULT_FSIZE_MB;
    if (nofile < 8) nofile = CLAW_TOOL_SHELL_DEFAULT_NOFILE;

    set_limit(RLIMIT_CPU, (rlim_t)cpu_sec, (rlim_t)cpu_sec);
    set_limit(RLIMIT_AS, (rlim_t)as_mb * 1024u * 1024u, (rlim_t)as_mb * 1024u * 1024u);
    set_limit(RLIMIT_FSIZE, (rlim_t)fsize_mb * 1024u * 1024u, (rlim_t)fsize_mb * 1024u * 1024u);
    set_limit(RLIMIT_NOFILE, (rlim_t)nofile, (rlim_t)nofile);
    set_limit(RLIMIT_CORE, 0, 0);
}

static int drain_pipe_limited(int fd, claw_response_t *resp, size_t *captured, size_t max_output, int *eof, int *truncated)
{
    char buf[4096];
    for (;;) {
        ssize_t n = read(fd, buf, sizeof(buf));
        if (n > 0) {
            size_t keep = (size_t)n;
            if (*captured >= max_output) {
                *truncated = 1;
                continue;
            }
            if (*captured + keep > max_output) {
                keep = max_output - *captured;
                *truncated = 1;
            }
            if (keep > 0 && claw_response_append_mem(resp, buf, keep) != 0) return -1;
            *captured += keep;
            continue;
        }
        if (n == 0) {
            *eof = 1;
            return 0;
        }
        if (errno == EINTR) continue;
        if (errno == EAGAIN || errno == EWOULDBLOCK) return 0;
        return -2;
    }
}

static int shell_tool_invoke(const char *json_args, claw_response_t *resp)
{
    char command[4096];
    char cwd_input[PATH_MAX];
    char cwd_real[PATH_MAX];
    long timeout_ms;
    long max_output = env_long_or_default("CLAW_TOOL_SHELL_MAX_OUTPUT_BYTES", CLAW_TOOL_SHELL_DEFAULT_MAX_OUTPUT);
    long max_command = env_long_or_default("CLAW_TOOL_SHELL_MAX_COMMAND_BYTES", CLAW_TOOL_SHELL_DEFAULT_MAX_COMMAND);
    int pipefd[2] = {-1, -1};
    int devnull = -1;
    pid_t pid;
    int child_done = 0;
    int status = 0;
    int timed_out = 0;
    int truncated = 0;
    int eof = 0;
    size_t captured = 0;
    struct pollfd pfd;
    struct timespec start, now;

    if (!json_args || !resp) return -1;
    if (!env_bool_or_default("CLAW_TOOL_SHELL_ENABLE", 1)) {
        claw_response_append(resp, "tool_shell disabled by CLAW_TOOL_SHELL_ENABLE=0");
        return -20;
    }
    if (json_get_string(json_args, "command", command, sizeof(command)) != 0 || command[0] == '\0') {
        claw_response_append(resp, "missing command");
        return -2;
    }
    if ((long)strlen(command) > max_command) {
        claw_response_append(resp, "command exceeds max length policy");
        return -3;
    }
    if (json_get_string(json_args, "cwd", cwd_input, sizeof(cwd_input)) != 0) cwd_input[0] = '\0';
    if (resolve_realpath_or_cwd(cwd_input, cwd_real) != 0) {
        claw_response_append(resp, "invalid cwd");
        return -4;
    }
    if (!cwd_allowed(cwd_real)) {
        claw_response_append(resp, "cwd rejected by allowed-roots policy");
        return -5;
    }

    timeout_ms = clamp_timeout_ms(env_long_or_default("CLAW_TOOL_SHELL_TIMEOUT_MS", CLAW_TOOL_SHELL_DEFAULT_TIMEOUT_MS));
    {
        long requested;
        if (json_get_long(json_args, "timeout_ms", &requested) == 0) timeout_ms = clamp_timeout_ms(requested);
    }
    if (max_output < 1024) max_output = CLAW_TOOL_SHELL_DEFAULT_MAX_OUTPUT;

    if (pipe(pipefd) != 0) {
        claw_response_append(resp, "pipe failed");
        return -6;
    }
    if (set_cloexec(pipefd[0]) != 0 || set_cloexec(pipefd[1]) != 0 || set_nonblock(pipefd[0]) != 0) {
        claw_response_append(resp, "pipe setup failed");
        close(pipefd[0]);
        close(pipefd[1]);
        return -7;
    }

    devnull = open("/dev/null", O_RDONLY | O_CLOEXEC);
    if (devnull < 0) {
        claw_response_append(resp, "open /dev/null failed");
        close(pipefd[0]);
        close(pipefd[1]);
        return -8;
    }

    pid = fork();
    if (pid < 0) {
        claw_response_append(resp, "fork failed");
        close(devnull);
        close(pipefd[0]);
        close(pipefd[1]);
        return -9;
    }

    if (pid == 0) {
        char *const argv[] = {"sh", "-lc", command, NULL};
        char path_env[] = "PATH=/usr/bin:/bin";
        char lang_env[] = "LANG=C";
        char home_env[PATH_MAX + 6];
        char pwd_env[PATH_MAX + 5];
        char *envp[5];
        int envc = 0;
        int maxfd;

        (void)setsid();
        umask(077);
        child_apply_limits();

        if (chdir(cwd_real) != 0) _exit(126);
        if (dup2(devnull, STDIN_FILENO) < 0) _exit(126);
        if (dup2(pipefd[1], STDOUT_FILENO) < 0) _exit(126);
        if (dup2(pipefd[1], STDERR_FILENO) < 0) _exit(126);
        close(devnull);
        close(pipefd[0]);
        close(pipefd[1]);

        maxfd = (int)sysconf(_SC_OPEN_MAX);
        if (maxfd < 16) maxfd = 256;
        for (int fd = 3; fd < maxfd; ++fd) close(fd);

        snprintf(home_env, sizeof(home_env), "HOME=%s", cwd_real);
        snprintf(pwd_env, sizeof(pwd_env), "PWD=%s", cwd_real);
        envp[envc++] = path_env;
        envp[envc++] = lang_env;
        envp[envc++] = home_env;
        envp[envc++] = pwd_env;
        envp[envc] = NULL;

        execve("/bin/sh", argv, envp);
        _exit(127);
    }

    close(devnull);
    close(pipefd[1]);
    pipefd[1] = -1;

    pfd.fd = pipefd[0];
    pfd.events = POLLIN | POLLHUP | POLLERR;
    clock_gettime(CLOCK_MONOTONIC, &start);

    while (!child_done || !eof) {
        int wait_ms;
        int prc;
        if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) break;
        {
            long elapsed = (long)((now.tv_sec - start.tv_sec) * 1000L + (now.tv_nsec - start.tv_nsec) / 1000000L);
            long remaining = timeout_ms - elapsed;
            if (remaining <= 0) {
                timed_out = 1;
                kill(-pid, SIGKILL);
                kill(pid, SIGKILL);
                timeout_ms = 0;
                wait_ms = 50;
            } else {
                wait_ms = remaining > 200 ? 200 : (int)remaining;
            }
        }

        prc = poll(&pfd, 1, wait_ms);
        if (prc > 0 && (pfd.revents & (POLLIN | POLLHUP | POLLERR))) {
            if (drain_pipe_limited(pipefd[0], resp, &captured, (size_t)max_output, &eof, &truncated) != 0) {
                close(pipefd[0]);
                kill(-pid, SIGKILL);
                kill(pid, SIGKILL);
                waitpid(pid, NULL, 0);
                claw_response_append(resp, "\n[pipe read failed]");
                return -10;
            }
            if (truncated) {
                kill(-pid, SIGKILL);
                kill(pid, SIGKILL);
                timed_out = timed_out || 0;
            }
        }
        if (!child_done) {
            pid_t w = waitpid(pid, &status, WNOHANG);
            if (w == pid) child_done = 1;
        }
    }

    close(pipefd[0]);
    if (!child_done) waitpid(pid, &status, 0);

    if (truncated) append_note(resp, "truncated_output");
    if (timed_out) {
        append_note(resp, "timed_out");
        return -11;
    }
    if (WIFEXITED(status)) {
        int exit_code = WEXITSTATUS(status);
        append_status_line(resp, "exit_code=", exit_code);
        return exit_code == 0 ? 0 : -12;
    }
    if (WIFSIGNALED(status)) {
        append_status_line(resp, "signal=", WTERMSIG(status));
        return -13;
    }
    claw_response_append(resp, "\n[unknown_status]");
    return -14;
}

static const claw_tool_api_t SHELL_TOOL = {
    .name = "shell",
    .invoke = shell_tool_invoke,
};

static const claw_module_descriptor_t SHELL_DESC = {
    .abi_version = CLAW_PLUGIN_ABI_VERSION,
    .kind = CLAW_MOD_TOOL,
    .api.tool = &SHELL_TOOL,
};

__attribute__((visibility("default")))
const claw_module_descriptor_t *claw_plugin_init(void)
{
    return &SHELL_DESC;
}
