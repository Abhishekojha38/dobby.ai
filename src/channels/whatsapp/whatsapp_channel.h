/*
 * whatsapp_channel.h — WhatsApp channel for Dobby
 *
 * Connects to the wa-bridge sidecar over a plain TCP socket.
 * Each WhatsApp phone number becomes its own session: whatsapp:<pn>
 *
 * Configuration (dobby.conf [whatsapp] section):
 *   bridge_host = 127.0.0.1   ; bridge TCP host
 *   bridge_port = 3001         ; bridge TCP port
 *   bridge_token =             ; optional shared secret
 */
#ifndef WHATSAPP_CHANNEL_H
#define WHATSAPP_CHANNEL_H

#include "../../bus/bus.h"
#include "../../core/config.h"
#include "../../security/allowlist.h"
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct whatsapp_channel whatsapp_channel_t;

/* Create channel from config. Returns NULL if [whatsapp] not configured. */
whatsapp_channel_t *whatsapp_channel_create(config_t *cfg, bus_t *bus, allowlist_t *al);

/* Start background receive thread. */
bool whatsapp_channel_start(whatsapp_channel_t *ch);

/* Stop and free. */
void whatsapp_channel_destroy(whatsapp_channel_t *ch);

/* Send a WhatsApp message directly (used by send_whatsapp tool).
 * to: full JID e.g. "1234567890@s.whatsapp.net" or bare phone number.
 * Returns true on success. */
bool whatsapp_channel_send(whatsapp_channel_t *ch, const char *to, const char *text);

/* Check if a phone number is allowed by the allowlist. */
bool whatsapp_channel_is_allowed(whatsapp_channel_t *ch, const char *pn);

#ifdef __cplusplus
}
#endif

#endif /* WHATSAPP_CHANNEL_H */
