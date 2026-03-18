CC         ?= cc
PKG_CONFIG ?= pkg-config

BASE_CFLAGS  ?= -D_POSIX_C_SOURCE=200809L -std=c11 -O2 -Wall -Wextra -Werror -fPIC -Iinclude
BASE_LDLIBS  ?= -ldl

BUILD_DIR  := build
PLUGIN_DIR := $(BUILD_DIR)/plugins
APP        := $(BUILD_DIR)/cclaw

APP_SRCS := src/core/main.c src/core/registry.c src/runtime/dispatch.c
APP_OBJS := $(APP_SRCS:%.c=$(BUILD_DIR)/%.o)

PLUGIN_ECHO_SRC    := plugins/provider_echo/provider_echo.c
PLUGIN_ECHO_SO     := $(PLUGIN_DIR)/provider_echo.so
PLUGIN_OPENAI_SRC  := plugins/provider_openai_compat/provider_openai_compat.c
PLUGIN_OPENAI_SO   := $(PLUGIN_DIR)/provider_openai_compat.so
PLUGIN_SQLITE_SRC  := plugins/memory_sqlite/memory_sqlite.c
PLUGIN_SQLITE_SO   := $(PLUGIN_DIR)/memory_sqlite.so
PLUGIN_HTTP_SRC    := plugins/channel_http/channel_http.c
PLUGIN_HTTP_SO     := $(PLUGIN_DIR)/channel_http.so
PLUGIN_SHELL_SRC   := plugins/tool_shell/tool_shell.c
PLUGIN_SHELL_SO    := $(PLUGIN_DIR)/tool_shell.so

HAVE_PKG_CONFIG := $(shell command -v $(PKG_CONFIG) >/dev/null 2>&1 && echo 1 || echo 0)
ifeq ($(HAVE_PKG_CONFIG),1)
CURL_CFLAGS   := $(shell $(PKG_CONFIG) --cflags libcurl 2>/dev/null)
CURL_LIBS     := $(shell $(PKG_CONFIG) --libs libcurl 2>/dev/null)
SQLITE_CFLAGS := $(shell $(PKG_CONFIG) --cflags sqlite3 2>/dev/null)
SQLITE_LIBS   := $(shell $(PKG_CONFIG) --libs sqlite3 2>/dev/null)
endif

HAVE_CURL   := $(if $(strip $(CURL_LIBS)),1,0)
HAVE_SQLITE := $(if $(strip $(SQLITE_LIBS)),1,0)

PLUGINS := $(PLUGIN_ECHO_SO) $(PLUGIN_HTTP_SO) $(PLUGIN_SHELL_SO)

ifeq ($(HAVE_CURL),1)
PLUGINS += $(PLUGIN_OPENAI_SO)
else
$(warning libcurl development files not found; skipping provider_openai_compat.so)
endif

ifeq ($(HAVE_SQLITE),1)
PLUGINS += $(PLUGIN_SQLITE_SO)
else
$(warning sqlite3 development files not found; skipping memory_sqlite.so)
endif

.PHONY: all clean run run-list run-mem run-search run-tool

all: $(APP) $(PLUGINS)

$(BUILD_DIR)/src/core/%.o: src/core/%.c
	mkdir -p $(dir $@)
	$(CC) $(BASE_CFLAGS) -c $< -o $@

$(BUILD_DIR)/src/runtime/%.o: src/runtime/%.c
	mkdir -p $(dir $@)
	$(CC) $(BASE_CFLAGS) -c $< -o $@

$(APP): $(APP_OBJS)
	mkdir -p $(dir $@)
	$(CC) $(APP_OBJS) -o $@ $(BASE_LDLIBS)

$(PLUGIN_ECHO_SO): $(PLUGIN_ECHO_SRC)
	mkdir -p $(dir $@)
	$(CC) $(BASE_CFLAGS) -shared $< -o $@

$(PLUGIN_OPENAI_SO): $(PLUGIN_OPENAI_SRC)
	mkdir -p $(dir $@)
	$(CC) $(BASE_CFLAGS) $(CURL_CFLAGS) -shared $< -o $@ $(CURL_LIBS)

$(PLUGIN_SQLITE_SO): $(PLUGIN_SQLITE_SRC)
	mkdir -p $(dir $@)
	$(CC) $(BASE_CFLAGS) $(SQLITE_CFLAGS) -shared $< -o $@ $(SQLITE_LIBS)

$(PLUGIN_HTTP_SO): $(PLUGIN_HTTP_SRC)
	mkdir -p $(dir $@)
	$(CC) $(BASE_CFLAGS) -shared $< -o $@

$(PLUGIN_SHELL_SO): $(PLUGIN_SHELL_SRC)
	mkdir -p $(dir $@)
	$(CC) $(BASE_CFLAGS) -shared $< -o $@

run: all
	./$(APP) $(PLUGIN_DIR) chat echo "hello runtime"

run-list: all
	./$(APP) $(PLUGIN_DIR) list

run-mem: all
	./$(APP) $(PLUGIN_DIR) mem-put sqlite greeting hello
	./$(APP) $(PLUGIN_DIR) mem-get sqlite greeting

run-search: all
	./$(APP) $(PLUGIN_DIR) mem-search sqlite greet

run-tool: all
	./$(APP) $(PLUGIN_DIR) tool-invoke shell '{"command":"pwd"}'

clean:
	rm -rf $(BUILD_DIR)
