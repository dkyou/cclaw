#include "claw/plugin_api.h"

static int echo_chat(const claw_request_t *req, claw_response_t *resp)
{
    if (!req || !req->input || !resp) return -1;
    if (claw_response_append(resp, "echo: ") != 0) return -2;
    if (claw_response_append(resp, req->input) != 0) return -3;
    return 0;
}

static const claw_provider_api_t ECHO_PROVIDER = {
    .name = "echo",
    .chat = echo_chat,
};

static const claw_module_descriptor_t ECHO_DESC = {
    .abi_version = CLAW_PLUGIN_ABI_VERSION,
    .kind = CLAW_MOD_PROVIDER,
    .api.provider = &ECHO_PROVIDER,
};

__attribute__((visibility("default")))
const claw_module_descriptor_t *claw_plugin_init(void)
{
    return &ECHO_DESC;
}
