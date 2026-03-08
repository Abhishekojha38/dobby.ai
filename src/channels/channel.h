#ifndef CHANNEL_H
#define CHANNEL_H

#include "../bus/bus.h"
#include "../bus/agent_worker.h"
#include "../session/session.h"
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#define CHANNEL_NAME_MAX     32
#define CHANNEL_REG_MAX      16

typedef struct channel channel_t;

struct channel {
    char  name[CHANNEL_NAME_MAX];
    bool  (*start)(channel_t *self, bus_t *bus);
    void  (*stop) (channel_t *self);
    void  (*send) (channel_t *self, outbound_msg_t *msg);
    void  *priv;
};

void       channel_register(channel_t *ch);
channel_t *channel_find(const char *name);
int        channel_count(void);

bool worker_start(bus_t *bus, session_manager_t *sm);
void worker_stop(void);

bool channels_start_threads(bus_t *bus, session_manager_t *sm);
void channels_stop_threads(void);

#ifdef __cplusplus
}
#endif

#endif /* CHANNEL_H */
