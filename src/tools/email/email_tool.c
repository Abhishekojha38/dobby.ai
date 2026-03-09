/*
 * email_tool.c — send_email tool for Dobby
 *
 * Registers a send_email tool so the agent can send email directly
 * through the configured SMTP channel.  This means:
 *
 *   - The agent never needs `mail`, `sendmail`, `mutt`, or any shell command.
 *   - It works the same whether email was the inbound channel or not.
 *   - If email is not configured, the tool returns a clear error.
 *
 * Tool schema:
 *   send_email(to, subject, body)
 *
 * Example agent call:
 *   send_email({
 *     "to":      "alice@example.com",
 *     "subject": "Disk usage report",
 *     "body":    "Root partition is 42% full."
 *   })
 */
#include "email_tool.h"
#include "../../core/dobby.h"
#include "../../core/log.h"
#include "../../core/cJSON.h"
#include "../../agent/tool_registry.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static email_channel_t *g_email_ch = NULL;

static char *json_result(bool ok, const char *message) {
    cJSON *obj = cJSON_CreateObject();
    cJSON_AddBoolToObject(obj, "ok", ok);
    cJSON_AddStringToObject(obj, ok ? "message" : "error", message);
    char *s = cJSON_PrintUnformatted(obj);
    cJSON_Delete(obj);
    return s;
}

static char *email_tool_execute(const cJSON *args, void *user_data) {
    (void)user_data;

    if (!g_email_ch) {
        return json_result(false, "Email not configured. No [email] section in dobby.conf.");
    }

    const char *to = cJSON_GetStringValue(cJSON_GetObjectItem(args, "to"));
    const char *subject = cJSON_GetStringValue(cJSON_GetObjectItem(args, "subject"));
    const char *body = cJSON_GetStringValue(cJSON_GetObjectItem(args, "body"));

    if (!to || !*to)
        return json_result(false, "'to' parameter is required");
    if (!subject || !*subject)
        return json_result(false, "'subject' parameter is required");
    if (!body || !*body)
        return json_result(false, "'body' parameter is required");

    LOG_DEBUG("send_email: to=%s subject=%s", to, subject);

    if (!email_channel_is_allowed(g_email_ch, to)) {
        char msg[512];
        snprintf(msg, sizeof(msg),
            "BLOCKED by email allowlist: '%s' is not a permitted recipient. "
            "Fix: add it to allowlist.conf under [email] allow = ..., "
            "or run: /email allow %s", to, to);
        return json_result(false, msg);
    }

    bool ok = email_channel_send_direct(g_email_ch, to, subject, body);

    if (ok) {
        char msg[512];
        snprintf(msg, sizeof(msg), "Email sent to %s", to);
        return json_result(true, msg);
    } else {
        return json_result(false,
            "SMTP send failed. The email was not sent. "
            "Possible causes: wrong smtp_url, wrong EMAIL_PASSWORD, "
            "network error, or recipient blocked by allowlist. "
            "Check [gateway:warn] logs for details.");
    }
}

void email_tool_register(email_channel_t *ch) {
    g_email_ch = ch;   /* NULL = disabled stub, still registers for clear errors */

    static const tool_t tool = {
        .name        = "send_email",
        .description =
            "Send an email via Dobby's configured SMTP channel. "
            "Use this tool whenever you need to send an email — "
            "never use shell commands like `mail`, `sendmail`, or `mutt`. "
            "Parameters: to (recipient address), subject, body (plain text).",
        .parameters_schema =
            "{\"type\":\"object\",\"properties\":{"
            "\"to\":{\"type\":\"string\","
              "\"description\":\"Recipient email address\"},"
            "\"subject\":{\"type\":\"string\","
              "\"description\":\"Email subject line\"},"
            "\"body\":{\"type\":\"string\","
              "\"description\":\"Plain text email body\"}"
            "},\"required\":[\"to\",\"subject\",\"body\"]}",
        .execute   = email_tool_execute,
        .user_data = NULL,
    };
    tool_register(&tool);
    LOG_DEBUG("send_email tool registered (email_ch=%s)",
              ch ? "configured" : "NULL/disabled");
}
