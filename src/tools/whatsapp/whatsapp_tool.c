/*
 * whatsapp_tool.c — send_whatsapp tool for Dobby
 *
 * Lets the agent send WhatsApp messages via the configured bridge.
 * Mirrors the send_email tool pattern exactly.
 */
#include "whatsapp_tool.h"
#include "../../core/log.h"
#include "../../core/cJSON.h"
#include "../../agent/tool_registry.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static whatsapp_channel_t *g_wa_ch = NULL;

static char *json_result(bool ok, const char *message) {
    cJSON *obj = cJSON_CreateObject();
    cJSON_AddBoolToObject(obj, "ok", ok);
    cJSON_AddStringToObject(obj, ok ? "message" : "error", message);
    char *s = cJSON_PrintUnformatted(obj);
    cJSON_Delete(obj);
    return s;
}

static char *wa_tool_execute(const cJSON *args, void *user_data) {
    (void)user_data;

    if (!g_wa_ch)
        return json_result(false, "WhatsApp not configured. Add [whatsapp] bridge_host to dobby.conf.");

    const char *to   = cJSON_GetStringValue(cJSON_GetObjectItem(args, "to"));
    const char *text = cJSON_GetStringValue(cJSON_GetObjectItem(args, "text"));

    if (!to   || !*to)   return json_result(false, "'to' is required (phone number or JID)");
    if (!text || !*text) return json_result(false, "'text' is required");

    if (!whatsapp_channel_is_allowed(g_wa_ch, to)) {
        char msg[512];
        snprintf(msg, sizeof(msg),
            "BLOCKED by allowlist: '%s' is not a permitted WhatsApp recipient.", to);
        return json_result(false, msg);
    }

    LOG_DEBUG("send_whatsapp: to=%s", to);

    bool ok = whatsapp_channel_send(g_wa_ch, to, text);
    if (ok) {
        char msg[256];
        snprintf(msg, sizeof(msg), "WhatsApp message sent to %s", to);
        return json_result(true, msg);
    }
    return json_result(false, "Send failed — is the bridge running and connected?");
}

void whatsapp_tool_register(whatsapp_channel_t *ch) {
    g_wa_ch = ch;

    static const tool_t tool = {
        .name        = "send_whatsapp",
        .description =
            "Send a WhatsApp message via Dobby's connected bridge. "
            "Use this instead of any shell command for WhatsApp. "
            "Parameters: to (phone number like '919876543210' or full JID), text (message body).",
        .parameters_schema =
            "{\"type\":\"object\",\"properties\":{"
            "\"to\":{\"type\":\"string\","
              "\"description\":\"Recipient phone number (digits only, with country code) or full WhatsApp JID\"},"
            "\"text\":{\"type\":\"string\","
              "\"description\":\"Message text to send\"}"
            "},\"required\":[\"to\",\"text\"]}",
        .execute   = wa_tool_execute,
        .user_data = NULL,
    };
    tool_register(&tool);
    LOG_DEBUG("send_whatsapp tool registered (ch=%s)", ch ? "configured" : "NULL");
}
