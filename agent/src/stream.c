#include "stream.h"
#include "jsonesc.h"

#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <pthread.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>

#define SLOT_CAP  256          /* bytes per event */
#define SLOT_COUNT 256         /* ring capacity: 64 KiB */
#define BATCH_CAP 8192

struct pt_stream {
    int fd;
    pthread_t consumer;
    int stop;
    char ring[SLOT_COUNT][SLOT_CAP];
    _Atomic uint32_t head;    /* producer writes */
    _Atomic uint32_t tail;    /* consumer reads */
    _Atomic uint32_t dropped;
    _Atomic uint64_t seq;
};

static int send_all(int fd, const void *buf, size_t len)
{
    const char *p = buf;
    while (len) {
        ssize_t n = send(fd, p, len, 0);
        if (n <= 0)
            return -1;
        p += n;
        len -= (size_t)n;
    }
    return 0;
}

static void *consumer_main(void *arg)
{
    struct pt_stream *s = arg;
    char batch[BATCH_CAP];
    size_t used = 0;

    while (!s->stop || s->head != s->tail) {
        uint32_t tail = atomic_load_explicit(&s->tail, memory_order_relaxed);
        uint32_t head = atomic_load_explicit(&s->head, memory_order_acquire);
        if (tail == head) {
            struct timespec ts = { 0, 1000000 }; /* 1 ms */
            nanosleep(&ts, 0);
            /* fall through: still flush a partial batch and any drop count */
        }

        while (tail != head && used + SLOT_CAP <= sizeof(batch)) {
            size_t n = strlen(s->ring[tail % SLOT_COUNT]);
            memcpy(batch + used, s->ring[tail % SLOT_COUNT], n);
            used += n;
            tail++;
        }
        atomic_store_explicit(&s->tail, tail, memory_order_release);

        uint32_t dropped = atomic_exchange(&s->dropped, 0);
        if (dropped && used + 64 <= sizeof(batch))
            used += (size_t)snprintf(batch + used, sizeof(batch) - used,
                                     "{\"type\":\"drops\",\"seq\":%llu,\"n\":%u}\n",
                                     (unsigned long long)atomic_fetch_add(&s->seq, 1), dropped);

        if (used) {
            if (send_all(s->fd, batch, used) < 0)
                break;          /* host gone; keep draining until stop */
            used = 0;
        }
    }
    return 0;
}

struct pt_stream *pt_stream_connect(const char *host, uint16_t port)
{
    struct pt_stream *s = calloc(1, sizeof(*s));
    if (!s)
        return 0;

    s->fd = socket(AF_INET, SOCK_STREAM, 0);
    if (s->fd < 0)
        goto fail;

    struct sockaddr_in sa = { 0 };
    sa.sin_family = AF_INET;
    sa.sin_port = htons(port);
    if (inet_pton(AF_INET, host, &sa.sin_addr) != 1)
        goto fail;
    if (connect(s->fd, (struct sockaddr *)&sa, sizeof(sa)) < 0)
        goto fail;

    int one = 1;
    setsockopt(s->fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));

    if (pthread_create(&s->consumer, 0, consumer_main, s) != 0)
        goto fail;
    return s;

fail:
    if (s->fd >= 0)
        close(s->fd);
    free(s);
    return 0;
}

void pt_stream_close(struct pt_stream *s)
{
    if (!s)
        return;
    s->stop = 1;
    pthread_join(s->consumer, 0);
    close(s->fd);
    free(s);
}

void pt_log_call(struct pt_stream *s,
                 int tid, const char *mod, const char *fn,
                 uint64_t nid, const uint64_t args[6], uint64_t ret)
{
    uint32_t head = atomic_load_explicit(&s->head, memory_order_relaxed);
    uint32_t tail = atomic_load_explicit(&s->tail, memory_order_acquire);
    if (head - tail >= SLOT_COUNT) {
        atomic_fetch_add_explicit(&s->dropped, 1, memory_order_relaxed);
        return;
    }
    uint64_t seq = atomic_fetch_add_explicit(&s->seq, 1, memory_order_relaxed);
    char emod[48], efn[96];
    pt_json_escape(emod, sizeof(emod), mod);
    pt_json_escape(efn, sizeof(efn), fn);
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    uint64_t usec = (uint64_t)now.tv_sec * 1000000u + (uint64_t)now.tv_nsec / 1000u;
    char *slot = s->ring[head % SLOT_COUNT];
    snprintf(slot, SLOT_CAP,
             "{\"type\":\"call\",\"seq\":%llu,\"ts\":%llu,\"tid\":%d,"
             "\"mod\":\"%s\",\"fn\":\"%s\",\"nid\":\"0x%llx\","
             "\"args\":[\"0x%llx\",\"0x%llx\",\"0x%llx\",\"0x%llx\",\"0x%llx\",\"0x%llx\"],"
             "\"ret\":\"0x%llx\"}\n",
             (unsigned long long)seq, (unsigned long long)usec,
             tid, emod, efn, (unsigned long long)nid,
             (unsigned long long)args[0], (unsigned long long)args[1],
             (unsigned long long)args[2], (unsigned long long)args[3],
             (unsigned long long)args[4], (unsigned long long)args[5],
             (unsigned long long)ret);
    atomic_store_explicit(&s->head, head + 1, memory_order_release);
}

void pt_log_hello(struct pt_stream *s, int pid, const char *title, const char *fw)
{
    uint64_t seq = atomic_fetch_add(&s->seq, 1);
    char etitle[128], efw[32], line[256];
    pt_json_escape(etitle, sizeof(etitle), title);
    pt_json_escape(efw, sizeof(efw), fw);
    int n = snprintf(line, sizeof(line),
                     "{\"type\":\"hello\",\"pid\":%d,\"title\":\"%s\",\"fw\":\"%s\",\"seq\":%llu}\n",
                     pid, etitle, efw, (unsigned long long)seq);
    if (n > 0)
        send_all(s->fd, line, (size_t)n);
}
