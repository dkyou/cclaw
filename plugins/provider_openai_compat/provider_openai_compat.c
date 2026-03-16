#include "claw/plugin_api.h"

#include <ctype.h>
#include <curl/curl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CLAW_DEFAULT_BASE_URL  "https://api.openai.com"
#define CLAW_DEFAULT_CHAT_PATH "/v1/chat/completions"
#define CLAW_DEFAULT_MODEL     "gpt-4o-mini"

typedef struct {
    char  *data;
    size_t len;
    size_t cap;
} dynbuf_t;

static void dynbuf_init(dynbuf_t *b)
{
    if (!b) return;
    b->data = NULL;
    b->len = 0;
    b->cap = 0;
}

static void dynbuf_free(dynbuf_t *b)
{
    if (!b) return;
    free(b->data);
    b->data = NULL;
    b->len = 0;
    b->cap = 0;
}

static int dynbuf_reserve(dynbuf_t *b, size_t need)
{
    char *p;
    size_t new_cap;
    if (!b) return -1;
    if (need <= b->cap) return 0;
    new_cap = b->cap ? b->cap : 256;
    while (new_cap < need) {
        if (new_cap > ((size_t)-1) / 2) return -1;
        new_cap *= 2;
    }
    p = (char *)realloc(b->data, new_cap);
    if (!p) return -1;
    b->data = p;
    b->cap = new_cap;
    return 0;
}

static int dynbuf_append_mem(dynbuf_t *b, const char *src, size_t n)
{
    if (!b || (!src && n != 0)) return -1;
    if (dynbuf_reserve(b, b->len + n + 1) != 0) return -1;
    if (n) memcpy(b->data + b->len, src, n);
    b->len += n;
    b->data[b->len] = '\0';
    return 0;
}

static int dynbuf_append_cstr(dynbuf_t *b, const char *s)
{
    if (!s) return -1;
    return dynbuf_append_mem(b, s, strlen(s));
}

static int dynbuf_append_json_escaped(dynbuf_t *b, const char *s)
{
    const unsigned char *p;
    char tmp[7];
    if (!b || !s) return -1;
    for (p = (const unsigned char *)s; *p; ++p) {
        switch (*p) {
        case '"':
            if (dynbuf_append_cstr(b, "\\\"") != 0) return -1;
            break;
        case '\\':
            if (dynbuf_append_cstr(b, "\\\\") != 0) return -1;
            break;
        case '\b':
            if (dynbuf_append_cstr(b, "\\b") != 0) return -1;
            break;
        case '\f':
            if (dynbuf_append_cstr(b, "\\f") != 0) return -1;
            break;
        case '\n':
            if (dynbuf_append_cstr(b, "\\n") != 0) return -1;
            break;
        case '\r':
            if (dynbuf_append_cstr(b, "\\r") != 0) return -1;
            break;
        case '\t':
            if (dynbuf_append_cstr(b, "\\t") != 0) return -1;
            break;
        default:
            if (*p < 0x20) {
                snprintf(tmp, sizeof(tmp), "\\u%04x", (unsigned int)*p);
                if (dynbuf_append_cstr(b, tmp) != 0) return -1;
            } else {
                if (dynbuf_append_mem(b, (const char *)p, 1) != 0) return -1;
            }
            break;
        }
    }
    return 0;
}

static size_t curl_write_cb(char *ptr, size_t size, size_t nmemb, void *userdata)
{
    size_t total = size * nmemb;
    dynbuf_t *b = (dynbuf_t *)userdata;
    if (!b || !ptr) return 0;
    if (dynbuf_append_mem(b, ptr, total) != 0) return 0;
    return total;
}

static const char *skip_ws(const char *p)
{
    while (p && *p && isspace((unsigned char)*p)) ++p;
    return p;
}

static const char *find_json_string_field(const char *start, const char *field)
{
    dynbuf_t key;
    const char *p;
    dynbuf_init(&key);
    if (dynbuf_append_cstr(&key, "\"") != 0 ||
        dynbuf_append_cstr(&key, field) != 0 ||
        dynbuf_append_cstr(&key, "\"") != 0) {
        dynbuf_free(&key);
        return NULL;
    }
    p = strstr(start, key.data);
    dynbuf_free(&key);
    if (!p) return NULL;
    p = strchr(p, ':');
    if (!p) return NULL;
    p++;
    p = skip_ws(p);
    if (*p != '"') return NULL;
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
    if (!out || !n) return -1;
    if (cp <= 0x7F) {
        out[0] = (char)cp;
        *n = 1;
    } else if (cp <= 0x7FF) {
        out[0] = (char)(0xC0 | (cp >> 6));
        out[1] = (char)(0x80 | (cp & 0x3F));
        *n = 2;
    } else if (cp <= 0xFFFF) {
        out[0] = (char)(0xE0 | (cp >> 12));
        out[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
        out[2] = (char)(0x80 | (cp & 0x3F));
        *n = 3;
    } else if (cp <= 0x10FFFF) {
        out[0] = (char)(0xF0 | (cp >> 18));
        out[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
        out[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
        out[3] = (char)(0x80 | (cp & 0x3F));
        *n = 4;
    } else {
        return -1;
    }
    return 0;
}

static int json_decode_string_at(const char *quoted, dynbuf_t *out)
{
    const char *p;
    if (!quoted || !out || *quoted != '"') return -1;
    p = quoted + 1;
    while (*p) {
        if (*p == '"') {
            return 0;
        }
        if (*p == '\\') {
            char esc;
            ++p;
            if (!*p) return -2;
            esc = *p;
            switch (esc) {
            case '"':
            case '\\':
            case '/':
                if (dynbuf_append_mem(out, &esc, 1) != 0) return -3;
                break;
            case 'b': {
                char c = '\b';
                if (dynbuf_append_mem(out, &c, 1) != 0) return -3;
                break;
            }
            case 'f': {
                char c = '\f';
                if (dynbuf_append_mem(out, &c, 1) != 0) return -3;
                break;
            }
            case 'n': {
                char c = '\n';
                if (dynbuf_append_mem(out, &c, 1) != 0) return -3;
                break;
            }
            case 'r': {
                char c = '\r';
                if (dynbuf_append_mem(out, &c, 1) != 0) return -3;
                break;
            }
            case 't': {
                char c = '\t';
                if (dynbuf_append_mem(out, &c, 1) != 0) return -3;
                break;
            }
            case 'u': {
                int h1, h2, h3, h4;
                unsigned cp;
                char utf8[4];
                size_t n = 0;
                if (!p[1] || !p[2] || !p[3] || !p[4]) return -4;
                h1 = hex_value(p[1]);
                h2 = hex_value(p[2]);
                h3 = hex_value(p[3]);
                h4 = hex_value(p[4]);
                if (h1 < 0 || h2 < 0 || h3 < 0 || h4 < 0) return -5;
                cp = (unsigned)((h1 << 12) | (h2 << 8) | (h3 << 4) | h4);
                if (utf8_from_codepoint(cp, utf8, &n) != 0) return -6;
                if (dynbuf_append_mem(out, utf8, n) != 0) return -7;
                p += 4;
                break;
            }
            default:
                return -8;
            }
        } else {
            if (dynbuf_append_mem(out, p, 1) != 0) return -9;
        }
        ++p;
    }
    return -10;
}

static int extract_openai_content(const char *json, dynbuf_t *out)
{
    const char *choices;
    const char *message;
    const char *content;
    const char *error;
    if (!json || !out) return -1;

    choices = strstr(json, "\"choices\"");
    if (choices) {
        message = strstr(choices, "\"message\"");
        if (message) {
            content = find_json_string_field(message, "content");
            if (content) {
                return json_decode_string_at(content, out);
            }
        }
        content = find_json_string_field(choices, "content");
        if (content) {
            return json_decode_string_at(content, out);
        }
        content = find_json_string_field(choices, "text");
        if (content) {
            return json_decode_string_at(content, out);
        }
    }

    error = strstr(json, "\"error\"");
    if (error) {
        content = find_json_string_field(error, "message");
        if (content) {
            return json_decode_string_at(content, out);
        }
    }
    return -2;
}

static int build_request_body(const char *model, const char *user_input, dynbuf_t *body)
{
    if (!model || !user_input || !body) return -1;
    if (dynbuf_append_cstr(body, "{\"model\":\"") != 0) return -1;
    if (dynbuf_append_json_escaped(body, model) != 0) return -1;
    if (dynbuf_append_cstr(body, "\",\"messages\":[{\"role\":\"user\",\"content\":\"") != 0) return -1;
    if (dynbuf_append_json_escaped(body, user_input) != 0) return -1;
    if (dynbuf_append_cstr(body, "\"}],\"stream\":false}") != 0) return -1;
    return 0;
}

static int append_error_to_response(claw_response_t *resp, const char *msg)
{
    if (!resp) return -1;
    if (claw_response_append(resp, msg ? msg : "unknown error") != 0) return -1;
    return 0;
}

static int openai_compat_chat(const claw_request_t *req, claw_response_t *resp)
{
    const char *api_key = getenv("CLAW_OPENAI_API_KEY");
    const char *base_url = getenv("CLAW_OPENAI_BASE_URL");
    const char *chat_path = getenv("CLAW_OPENAI_CHAT_PATH");
    const char *model = getenv("CLAW_OPENAI_MODEL");
    CURL *curl = NULL;
    CURLcode cc;
    long http_code = 0;
    struct curl_slist *headers = NULL;
    dynbuf_t url;
    dynbuf_t body;
    dynbuf_t response_body;
    dynbuf_t auth_header;
    dynbuf_t content;
    int rc = -1;

    if (!req || !req->input || !resp) return -1;
    if (!api_key || !*api_key) {
        return append_error_to_response(resp, "CLAW_OPENAI_API_KEY is not set");
    }
    if (!base_url || !*base_url) base_url = CLAW_DEFAULT_BASE_URL;
    if (!chat_path || !*chat_path) chat_path = CLAW_DEFAULT_CHAT_PATH;
    if (!model || !*model) model = CLAW_DEFAULT_MODEL;

    dynbuf_init(&url);
    dynbuf_init(&body);
    dynbuf_init(&response_body);
    dynbuf_init(&auth_header);
    dynbuf_init(&content);

    if (dynbuf_append_cstr(&url, base_url) != 0 ||
        dynbuf_append_cstr(&url, chat_path) != 0 ||
        build_request_body(model, req->input, &body) != 0 ||
        dynbuf_append_cstr(&auth_header, "Authorization: Bearer ") != 0 ||
        dynbuf_append_cstr(&auth_header, api_key) != 0) {
        append_error_to_response(resp, "failed to build provider request");
        rc = -2;
        goto cleanup;
    }

    curl = curl_easy_init();
    if (!curl) {
        append_error_to_response(resp, "curl_easy_init failed");
        rc = -3;
        goto cleanup;
    }

    headers = curl_slist_append(headers, "Content-Type: application/json");
    headers = curl_slist_append(headers, auth_header.data);
    if (!headers) {
        append_error_to_response(resp, "failed to build request headers");
        rc = -4;
        goto cleanup;
    }

    curl_easy_setopt(curl, CURLOPT_URL, url.data);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_POST, 1L);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body.data);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, (long)body.len);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curl_write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response_body);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, 120000L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT_MS, 10000L);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "cclaw/0.1");

    cc = curl_easy_perform(curl);
    if (cc != CURLE_OK) {
        append_error_to_response(resp, curl_easy_strerror(cc));
        rc = -5;
        goto cleanup;
    }

    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
    if (extract_openai_content(response_body.data ? response_body.data : "", &content) != 0) {
        append_error_to_response(resp, response_body.data ? response_body.data : "unparseable response");
        rc = -6;
        goto cleanup;
    }

    if (http_code < 200 || http_code >= 300) {
        append_error_to_response(resp, content.data ? content.data : "provider returned non-2xx");
        rc = -7;
        goto cleanup;
    }

    if (claw_response_append(resp, content.data ? content.data : "") != 0) {
        rc = -8;
        goto cleanup;
    }

    rc = 0;

cleanup:
    if (headers) curl_slist_free_all(headers);
    if (curl) curl_easy_cleanup(curl);
    dynbuf_free(&url);
    dynbuf_free(&body);
    dynbuf_free(&response_body);
    dynbuf_free(&auth_header);
    dynbuf_free(&content);
    return rc;
}

static const claw_provider_api_t OPENAI_COMPAT_PROVIDER = {
    .name = "openai_compat",
    .chat = openai_compat_chat,
};

static const claw_module_descriptor_t OPENAI_COMPAT_DESC = {
    .abi_version = CLAW_PLUGIN_ABI_VERSION,
    .kind = CLAW_MOD_PROVIDER,
    .api.provider = &OPENAI_COMPAT_PROVIDER,
};

__attribute__((constructor))
static void provider_openai_compat_ctor(void)
{
    curl_global_init(CURL_GLOBAL_DEFAULT);
}

__attribute__((destructor))
static void provider_openai_compat_dtor(void)
{
    curl_global_cleanup();
}

__attribute__((visibility("default")))
const claw_module_descriptor_t *claw_plugin_init(void)
{
    return &OPENAI_COMPAT_DESC;
}
