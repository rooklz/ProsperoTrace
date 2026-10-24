#pragma once

#include <stdint.h>

/*
 * Connect-out event streamer.
 *
 * The producer (the target's trampoline stub, via pt_log_call) formats one
 * JSON line into an SPSC ring; a consumer thread batches slots into TCP
 * sends. The producer never blocks: a full ring increments the drop counter,
 * which the consumer reports as {"type":"drops"} records.
 */

struct pt_stream;

struct pt_stream *pt_stream_connect(const char *host, uint16_t port);
void pt_stream_close(struct pt_stream *s);

/* producer-side; safe to call from the trampoline context */
void pt_log_call(struct pt_stream *s,
                 int tid, const char *mod, const char *fn,
                 uint64_t nid, const uint64_t args[6], uint64_t ret);

/* consumer-side; internal */
void pt_log_hello(struct pt_stream *s, int pid, const char *title, const char *fw);
