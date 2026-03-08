/*
 * email_tool.h — send_email tool registration
 *
 * Gives the agent a send_email tool so it can send email directly
 * through Dobby's configured SMTP channel without needing the `mail`
 * command or any external program.
 */
#ifndef EMAIL_TOOL_H
#define EMAIL_TOOL_H

#include "../../channels/email/email_channel.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Register the send_email tool.
 * ch is the running email_channel_t — pass NULL to register a disabled stub
 * (tool will return an error explaining email is not configured). */
void email_tool_register(email_channel_t *ch);

#ifdef __cplusplus
}
#endif

#endif /* EMAIL_TOOL_H */
