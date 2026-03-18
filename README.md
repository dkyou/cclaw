## v11 additions

- typed tool names: `exec`, `fs.read`, `fs.write`, `fs.list`
- dedicated HTTP routes: `/v1/tools/exec`, `/v1/fs/read`, `/v1/fs/write`, `/v1/fs/list`
- legacy aliases preserved: `tool_exec`, `file_read`, `file_write`, `dir_list`

# cclaw runtime v9

新增能力：

- tool_shell 结构化 ABI
  - `argv`
  - `argv + env`
  - `argv + env + cwd`
  - `command + env + cwd`
- scheduler 更完整 cron 语义
  - 5 段或 6 段 cron
  - `*`
  - `*/n`
  - 单值
  - 范围
  - 逗号列表
  - 月份/星期英文缩写，例如 `JAN`, `MON-FRI`
  - DOM/DOW 更接近标准 cron 的匹配语义
- scheduler 时区支持
  - 每个任务可单独指定 `timezone`
  - 默认时区可由 `CLAW_SCHEDULER_TZ` 提供

## 构建

```bash
make
```

## tool_shell：结构化 argv/env/cwd

CLI：

```bash
export CLAW_TOOL_SHELL_ALLOWED_ROOTS="$PWD"
./build/cclaw build/plugins tool-invoke shell '{"argv":["/usr/bin/env"],"env":{"FOO":"BAR"},"cwd":"."}'
```

HTTP：

```bash
curl -X POST http://127.0.0.1:8080/v1/tool/shell \
  -H 'Content-Type: application/json' \
  -d '{"argv":["/usr/bin/env"],"env":{"FOO":"BAR"},"cwd":"."}'
```

## scheduler：cron + timezone

CLI：

```bash
export CLAW_RUNTIME_DB_PATH=./state/runtime.db
./build/cclaw build/plugins scheduler-add-cron "*/5 * * * *" chat echo "hello cron"
./build/cclaw build/plugins scheduler-add-cron-tz "0 9 * * MON-FRI" Asia/Tokyo chat echo "weekday 9am"
./build/cclaw build/plugins scheduler-tasks
```

HTTP：

```bash
curl -X POST http://127.0.0.1:8080/v1/scheduler/task/upsert \
  -H 'Content-Type: application/json' \
  -d '{"schedule_type":"cron","cron_expr":"0 9 * * MON-FRI","timezone":"Asia/Tokyo","kind":"chat","target":"echo","arg1":"hello tz"}'
```


## v10 additions

Structured tools added:

- `exec`: pure argv execution with optional `env`, `cwd`, `timeout_ms`
- `file_read`: read a file under allowed roots
- `file_write`: write or append a file under allowed roots
- `dir_list`: list directory entries under allowed roots

Examples:

```bash
./build/cclaw build/plugins tool-invoke exec '{"argv":["printf","hi"],"cwd":"."}'
./build/cclaw build/plugins tool-invoke file_write '{"path":"./tmp/test.txt","content":"hello","mode":"overwrite"}'
./build/cclaw build/plugins tool-invoke file_read '{"path":"./tmp/test.txt"}'
./build/cclaw build/plugins tool-invoke dir_list '{"path":"./tmp"}'
```

Scheduler task JSON now includes local-time observability fields:

- `next_due_local`
- `last_run_local`
- `created_at_local`
- `updated_at_local`
- `scheduler_now_local` in scheduler status


## v12 typed tool ABI

Typed tools now return structured JSON with schema validation errors:
- exec
- fs.read
- fs.write
- fs.list

Examples:

```bash
./build/cclaw build/plugins tool-invoke exec '{"argv":["printf","hi"],"cwd":"."}'
./build/cclaw build/plugins tool-invoke fs.write '{"path":"./tmp.txt","content":"abc","mode":"overwrite"}'
./build/cclaw build/plugins tool-invoke fs.read '{"path":"./tmp.txt"}'
./build/cclaw build/plugins tool-invoke fs.list '{"path":".","max_entries":5}'
```

HTTP typed routes:

```bash
curl -X POST http://127.0.0.1:8080/v1/tools/exec   -H 'Content-Type: application/json'   -d '{"argv":["printf","via_http"],"cwd":"."}'

curl -X POST http://127.0.0.1:8080/v1/fs/write   -H 'Content-Type: application/json'   -d '{"path":"./http.txt","content":"hello","mode":"overwrite"}'
```


## v13 additions

- Tool schema registry with versioned typed ABI for tools.
- `list` now exposes `tool_schemas` automatically.
- runtime non-tool endpoints now expose `runtime_schemas` automatically.
- New CLI:
  - `tool-schemas`
  - `tool-schema <name>`
- New HTTP:
  - `POST /v1/runs` with `{ "op": "tool.invoke", ... }`
  - `GET /v1/runs?id=<id>`
  - `POST /v1/runs/stream` for SSE-style run event output
  - `POST /v1/tools/invoke` with `{ "tool": "...", "args": { ... } }`
  - `GET /v1/tools/schemas`
  - `GET /v1/tools/schema?name=<tool>`
  - `GET /v1/runtime/schemas`
  - `GET /v1/runtime/schema?name=<endpoint>`
  - `POST /v1/tools/validate` with `{ "tool": "...", "args": { ... } }`

Examples:

```bash
curl -X POST http://127.0.0.1:8080/v1/runs \
  -H 'Content-Type: application/json' \
  -d '{"op":"tool.invoke","tool":"exec","args":{"argv":["printf","hello"],"cwd":"."}}'

curl -X POST http://127.0.0.1:8080/v1/runs/stream \
  -H 'Content-Type: application/json' \
  -d '{"op":"scheduler.status"}'

curl -X POST http://127.0.0.1:8080/v1/tools/invoke \
  -H 'Content-Type: application/json' \
  -d '{"tool":"exec","args":{"argv":["printf","hello"],"cwd":"."}}'

curl -X POST http://127.0.0.1:8080/v1/tools/validate \
  -H 'Content-Type: application/json' \
  -d '{"tool":"exec","args":{"argv":["printf","hello"],"cwd":"."}}'

curl http://127.0.0.1:8080/v1/runtime/schemas
curl 'http://127.0.0.1:8080/v1/runtime/schema?name=scheduler.task.upsert'
```
