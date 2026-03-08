#define _GNU_SOURCE
#include "bus.h"
#include "../core/log.h"
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define BUS_QUEUE_DEPTH 64

inbound_msg_t *inbound_msg_new(const char *channel, const char *sender_id,
                                const char *chat_id, const char *content,
                                const char *metadata)
{
    inbound_msg_t *m = calloc(1, sizeof(*m));
    if (!m) return NULL;
    m->channel   = strdup(channel   ? channel   : "");
    m->sender_id = strdup(sender_id ? sender_id : "");
    m->chat_id   = strdup(chat_id   ? chat_id   : "");
    m->content   = strdup(content   ? content   : "");
    m->metadata  = metadata ? strdup(metadata) : NULL;
    if (!m->channel || !m->sender_id || !m->chat_id || !m->content)
        { inbound_msg_free(m); return NULL; }
    return m;
}

void inbound_msg_free(inbound_msg_t *msg)
{
    if (!msg) return;
    free(msg->channel); free(msg->sender_id);
    free(msg->chat_id); free(msg->content);
    free(msg->metadata); free(msg);
}

outbound_msg_t *outbound_msg_new(const char *channel, const char *chat_id,
                                  const char *content, const char *metadata)
{
    outbound_msg_t *m = calloc(1, sizeof(*m));
    if (!m) return NULL;
    m->channel  = strdup(channel  ? channel  : "");
    m->chat_id  = strdup(chat_id  ? chat_id  : "");
    m->content  = strdup(content  ? content  : "");
    m->metadata = metadata ? strdup(metadata) : NULL;
    if (!m->channel || !m->chat_id || !m->content)
        { outbound_msg_free(m); return NULL; }
    return m;
}

void outbound_msg_free(outbound_msg_t *msg)
{
    if (!msg) return;
    free(msg->channel); free(msg->chat_id);
    free(msg->content); free(msg->metadata); free(msg);
}

typedef struct {
    void           *items[BUS_QUEUE_DEPTH];
    int             head, tail, count;
    pthread_mutex_t lock;
    pthread_cond_t  not_empty, not_full;
} bq_t;

static void bq_init(bq_t *q) {
    memset(q, 0, sizeof(*q));
    pthread_mutex_init(&q->lock, NULL);
    pthread_cond_init(&q->not_empty, NULL);
    pthread_cond_init(&q->not_full, NULL);
}

static void bq_destroy(bq_t *q) {
    for (int i = 0; i < q->count; i++) free(q->items[(q->head + i) % BUS_QUEUE_DEPTH]);
    pthread_cond_destroy(&q->not_full);
    pthread_cond_destroy(&q->not_empty);
    pthread_mutex_destroy(&q->lock);
}

static bool q_push(bq_t *q, void *item, volatile bool *shut_down) {
    pthread_mutex_lock(&q->lock);
    while (q->count >= BUS_QUEUE_DEPTH && !*shut_down)
        pthread_cond_wait(&q->not_full, &q->lock);
    if (*shut_down) { pthread_mutex_unlock(&q->lock); free(item); return false; }
    q->items[q->tail] = item;
    q->tail = (q->tail + 1) % BUS_QUEUE_DEPTH;
    q->count++;
    pthread_cond_signal(&q->not_empty);
    pthread_mutex_unlock(&q->lock);
    return true;
}

static void *q_pop(bq_t *q, volatile bool *shut_down) {
    pthread_mutex_lock(&q->lock);
    while (q->count == 0 && !*shut_down)
        pthread_cond_wait(&q->not_empty, &q->lock);
    if (q->count == 0) { pthread_mutex_unlock(&q->lock); return NULL; }
    void *item = q->items[q->head];
    q->head = (q->head + 1) % BUS_QUEUE_DEPTH;
    q->count--;
    pthread_cond_signal(&q->not_full);
    pthread_mutex_unlock(&q->lock);
    return item;
}

static int q_size(bq_t *q) {
    pthread_mutex_lock(&q->lock);
    int n = q->count;
    pthread_mutex_unlock(&q->lock);
    return n;
}

static void bq_broadcast(bq_t *q) {
    pthread_mutex_lock(&q->lock);
    pthread_cond_broadcast(&q->not_empty);
    pthread_cond_broadcast(&q->not_full);
    pthread_mutex_unlock(&q->lock);
}

struct bus { bq_t inbound, outbound; volatile bool shut_down; };

bus_t *bus_create(void) {
    bus_t *b = calloc(1, sizeof(*b));
    if (!b) return NULL;
    bq_init(&b->inbound); bq_init(&b->outbound);
    LOG_DEBUG("Bus created (depth=%d)", BUS_QUEUE_DEPTH);
    return b;
}

void bus_shutdown(bus_t *bus) {
    if (!bus) return;
    bus->shut_down = true;
    bq_broadcast(&bus->inbound); bq_broadcast(&bus->outbound);
    LOG_DEBUG("Bus shutdown signalled");
}

void bus_destroy(bus_t *bus) {
    if (!bus) return;
    bus_shutdown(bus);
    bq_destroy(&bus->inbound); bq_destroy(&bus->outbound);
    free(bus);
}

bool bus_publish_inbound(bus_t *bus, inbound_msg_t *msg) {
    if (!bus || !msg) return false;
    LOG_DEBUG("Bus inbound: channel='%s'", msg->channel);
    return q_push(&bus->inbound, msg, &bus->shut_down);
}
inbound_msg_t *bus_consume_inbound(bus_t *bus) {
    return bus ? q_pop(&bus->inbound, &bus->shut_down) : NULL;
}
bool bus_publish_outbound(bus_t *bus, outbound_msg_t *msg) {
    if (!bus || !msg) return false;
    return q_push(&bus->outbound, msg, &bus->shut_down);
}
outbound_msg_t *bus_consume_outbound(bus_t *bus) {
    return bus ? q_pop(&bus->outbound, &bus->shut_down) : NULL;
}

void response_pair_init(response_pair_t *rp) {
    memset(rp, 0, sizeof(*rp));
    pthread_mutex_init(&rp->lock, NULL);
    pthread_cond_init(&rp->ready, NULL);
}

void response_pair_destroy(response_pair_t *rp) {
    if (!rp) return;
    pthread_cond_destroy(&rp->ready);
    pthread_mutex_destroy(&rp->lock);
    free(rp->response); rp->response = NULL;
}

void response_pair_deliver(response_pair_t *rp, char *resp) {
    if (!rp) { free(resp); return; }
    pthread_mutex_lock(&rp->lock);
    free(rp->response);
    rp->response = resp; rp->done = true;
    pthread_cond_signal(&rp->ready);
    pthread_mutex_unlock(&rp->lock);
}

char *response_pair_wait(response_pair_t *rp) {
    if (!rp) return NULL;
    pthread_mutex_lock(&rp->lock);
    while (!rp->done) pthread_cond_wait(&rp->ready, &rp->lock);
    char *resp = rp->response; rp->response = NULL;
    pthread_mutex_unlock(&rp->lock);
    return resp;
}

response_pair_t *response_pair_from_meta(const char *metadata) {
    if (!metadata) return NULL;
    const char *p = strstr(metadata, "rp=");
    if (!p) return NULL;
    unsigned long long ptr = 0;
    if (sscanf(p + 3, "%llx", &ptr) != 1) return NULL;
    return (response_pair_t *)(uintptr_t)ptr;
}

int bus_inbound_size(bus_t *bus)  { return bus ? q_size(&bus->inbound)  : 0; }
int bus_outbound_size(bus_t *bus) { return bus ? q_size(&bus->outbound) : 0; }
