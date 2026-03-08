/*
 * email_channel.h — IMAP-poll / SMTP-reply email channel for Dobby
 *
 * Uses libcurl for both IMAP (receive) and SMTP (send).
 * Each sender email address becomes a separate session: email:<addr>
 *
 * Configuration (dobby.conf [email] section):
 *   imap_url      = imaps://imap.gmail.com:993
 *   smtp_url      = smtps://smtp.gmail.com:465
 *   address       = your@email.com
 *   password      = <app-password or IMAP_PASSWORD env>
 *   poll_interval = 60          ; seconds between IMAP polls
 *   inbox         = INBOX       ; mailbox to watch
 *   subject_tag   = [Dobby]     ; tag in Subject for outgoing mail
 */
#ifndef EMAIL_CHANNEL_H
#define EMAIL_CHANNEL_H

#include "../../bus/bus.h"
#include "../../core/config.h"
#include "../../security/allowlist.h"
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct email_channel email_channel_t;

/* Create and configure the channel from config.
 * al may be NULL (disables email address enforcement).
 * Returns NULL if email is not configured (imap_url missing). */
email_channel_t *email_channel_create(config_t *cfg, bus_t *bus, allowlist_t *al);

/* Start the background poll thread. */
bool email_channel_start(email_channel_t *ch);

/* Stop and free. */
void email_channel_destroy(email_channel_t *ch);

/* Check if an email address is allowed by the email allowlist.
 * Returns true if allowed (or if no allowlist is configured). */
bool email_channel_is_allowed(email_channel_t *ch, const char *address);

/* Fill a human-readable status snapshot (for /email status CLI command).
 * buf must be at least 512 bytes. */
void email_channel_status(email_channel_t *ch, char *buf, size_t buf_size);

/* Send an email directly via the configured SMTP server.
 * Used by the send_email tool so the agent can send outbound mail.
 * Returns true on success. ch must not be NULL. */
bool email_channel_send_direct(email_channel_t *ch,
                               const char *to,
                               const char *subject,
                               const char *body);

#ifdef __cplusplus
}
#endif

#endif /* EMAIL_CHANNEL_H */
