#ifndef WHATSAPP_TOOL_H
#define WHATSAPP_TOOL_H

#include "../../channels/whatsapp/whatsapp_channel.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Register the send_whatsapp tool. ch may be NULL (shows config error). */
void whatsapp_tool_register(whatsapp_channel_t *ch);

#ifdef __cplusplus
}
#endif

#endif /* WHATSAPP_TOOL_H */
