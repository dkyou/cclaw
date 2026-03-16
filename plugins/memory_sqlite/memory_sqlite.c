#include "claw/plugin_api.h"

#include <sqlite3.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define CLAW_SQLITE_DEFAULT_PATH "./cclaw.db"

typedef struct {
    sqlite3 *db;
    int initialized;
} sqlite_plugin_state_t;

static sqlite_plugin_state_t g_state = {0};

static const char *sqlite_db_path(void)
{
    const char *p = getenv("CLAW_SQLITE_PATH");
    return (p && *p) ? p : CLAW_SQLITE_DEFAULT_PATH;
}

static int sqlite_exec_simple(sqlite3 *db, const char *sql)
{
    char *errmsg = NULL;
    int rc;
    rc = sqlite3_exec(db, sql, NULL, NULL, &errmsg);
    if (rc != SQLITE_OK) {
        if (errmsg) {
            fprintf(stderr, "sqlite exec error: %s\n", errmsg);
            sqlite3_free(errmsg);
        }
        return rc;
    }
    return SQLITE_OK;
}

static int ensure_db_ready(void)
{
    int rc;
    if (g_state.initialized && g_state.db) return SQLITE_OK;

    rc = sqlite3_open(sqlite_db_path(), &g_state.db);
    if (rc != SQLITE_OK) {
        if (g_state.db) {
            fprintf(stderr, "sqlite open error: %s\n", sqlite3_errmsg(g_state.db));
            sqlite3_close(g_state.db);
            g_state.db = NULL;
        }
        return rc;
    }

    rc = sqlite_exec_simple(g_state.db,
        "PRAGMA journal_mode=WAL;"
        "PRAGMA synchronous=NORMAL;"
        "PRAGMA temp_store=MEMORY;"
        "CREATE TABLE IF NOT EXISTS memories ("
        "  k TEXT PRIMARY KEY,"
        "  v TEXT NOT NULL,"
        "  updated_at INTEGER NOT NULL"
        ");");
    if (rc != SQLITE_OK) {
        sqlite3_close(g_state.db);
        g_state.db = NULL;
        return rc;
    }

    g_state.initialized = 1;
    return SQLITE_OK;
}

static int sqlite_memory_put(const char *key, const char *value)
{
    sqlite3_stmt *stmt = NULL;
    static const char *SQL =
        "INSERT INTO memories(k, v, updated_at) VALUES(?, ?, ?) "
        "ON CONFLICT(k) DO UPDATE SET v=excluded.v, updated_at=excluded.updated_at;";
    int rc;

    if (!key || !value) return -1;
    rc = ensure_db_ready();
    if (rc != SQLITE_OK) return -2;

    rc = sqlite3_prepare_v2(g_state.db, SQL, -1, &stmt, NULL);
    if (rc != SQLITE_OK) return -3;

    sqlite3_bind_text(stmt, 1, key, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, value, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(stmt, 3, (sqlite3_int64)time(NULL));

    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    if (rc != SQLITE_DONE) return -4;
    return 0;
}

static int sqlite_memory_get(const char *key, claw_response_t *resp)
{
    sqlite3_stmt *stmt = NULL;
    static const char *SQL = "SELECT v FROM memories WHERE k = ? LIMIT 1;";
    const unsigned char *value;
    int rc;

    if (!key || !resp) return -1;
    rc = ensure_db_ready();
    if (rc != SQLITE_OK) return -2;

    rc = sqlite3_prepare_v2(g_state.db, SQL, -1, &stmt, NULL);
    if (rc != SQLITE_OK) return -3;

    sqlite3_bind_text(stmt, 1, key, -1, SQLITE_TRANSIENT);
    rc = sqlite3_step(stmt);
    if (rc == SQLITE_ROW) {
        value = sqlite3_column_text(stmt, 0);
        if (!value) {
            sqlite3_finalize(stmt);
            return -4;
        }
        if (claw_response_append(resp, (const char *)value) != 0) {
            sqlite3_finalize(stmt);
            return -5;
        }
        sqlite3_finalize(stmt);
        return 0;
    }

    sqlite3_finalize(stmt);
    if (rc == SQLITE_DONE) return -6;
    return -7;
}

static const claw_memory_api_t SQLITE_MEMORY = {
    .name = "sqlite",
    .put = sqlite_memory_put,
    .get = sqlite_memory_get,
};

static const claw_module_descriptor_t SQLITE_DESC = {
    .abi_version = CLAW_PLUGIN_ABI_VERSION,
    .kind = CLAW_MOD_MEMORY,
    .api.memory = &SQLITE_MEMORY,
};

__attribute__((destructor))
static void memory_sqlite_dtor(void)
{
    if (g_state.db) {
        sqlite3_close(g_state.db);
        g_state.db = NULL;
    }
    g_state.initialized = 0;
}

__attribute__((visibility("default")))
const claw_module_descriptor_t *claw_plugin_init(void)
{
    return &SQLITE_DESC;
}
