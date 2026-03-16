# cclaw_runtime

一个最小可运行的 C 版 agent runtime 骨架，包含：

- `provider_openai_compat.so`：OpenAI 兼容 Chat Completions provider
- `memory_sqlite.so`：SQLite KV memory plugin
- `provider_echo.so`：本地测试 provider

## 目录

- `include/claw/plugin_api.h`：插件 ABI
- `include/claw/core.h`：registry 接口
- `src/core/`：插件加载与 CLI 驱动
- `plugins/provider_openai_compat/`：真实 provider
- `plugins/memory_sqlite/`：SQLite memory

## 编译

系统需要：

- C 编译器
- `pkg-config`
- `libcurl`
- `sqlite3`

编译：

```bash
make
```

## 运行

### 1. 本地 echo 测试

```bash
./build/cclaw build/plugins chat echo "hello runtime"
```

### 2. SQLite memory

默认数据库路径：`./cclaw.db`

```bash
./build/cclaw build/plugins mem-put sqlite greeting hello
./build/cclaw build/plugins mem-get sqlite greeting
```

自定义数据库路径：

```bash
export CLAW_SQLITE_PATH=/tmp/cclaw.db
./build/cclaw build/plugins mem-put sqlite greeting hello
./build/cclaw build/plugins mem-get sqlite greeting
```

### 3. OpenAI-compatible provider

支持所有兼容 `POST /v1/chat/completions` 的接口。

必要环境变量：

```bash
export CLAW_OPENAI_API_KEY="your_api_key"
```

可选环境变量：

```bash
export CLAW_OPENAI_BASE_URL="https://api.openai.com"
export CLAW_OPENAI_CHAT_PATH="/v1/chat/completions"
export CLAW_OPENAI_MODEL="gpt-4o-mini"
```

调用：

```bash
./build/cclaw build/plugins chat openai_compat "Tell me a joke."
```

### OpenRouter 示例

```bash
export CLAW_OPENAI_API_KEY="your_openrouter_key"
export CLAW_OPENAI_BASE_URL="https://openrouter.ai/api"
export CLAW_OPENAI_CHAT_PATH="/v1/chat/completions"
export CLAW_OPENAI_MODEL="openai/gpt-4o-mini"
./build/cclaw build/plugins chat openai_compat "hello"
```

## 当前限制

- provider 只做了非流式 chat completions
- JSON 解析是轻量定制实现，不是完整 JSON 解析器
- memory 当前是 KV 读写，还没有 FTS5 / embedding / search
- 主程序还是 CLI 驱动，HTTP gateway 还没接
