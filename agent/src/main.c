/*
 * ProsperoTrace agent entry point.
 *
 * Configuration is via environment (set by the loader before launch):
 *   PT_HOST   receiver address (required)
 *   PT_PORT   receiver port (default 9029)
 *   PT_PID    attach to this pid, or
 *   PT_NAME   substring of the target's process name
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "hook.h"
#include "stream.h"
#include "target.h"

#define PT_DEFAULT_PORT 9029

int main(void)
{
    const char *host = getenv("PT_HOST");
    const char *name = getenv("PT_NAME");
    const char *pid_s = getenv("PT_PID");
    const char *port_s = getenv("PT_PORT");
    unsigned port = port_s ? (unsigned)strtoul(port_s, 0, 10) : PT_DEFAULT_PORT;

    if (!host || (!name && !pid_s)) {
        printf("prosperotrace: need PT_HOST and one of PT_PID / PT_NAME\n");
        return 1;
    }

    pid_t pid = pid_s ? (pid_t)strtol(pid_s, 0, 10) : pt_find_pid(name);
    if (pid <= 0) {
        printf("prosperotrace: target not found\n");
        return 1;
    }
    printf("prosperotrace: attaching pid %d\n", pid);

    if (pt_attach(pid) != 0) {
        printf("prosperotrace: attach failed\n");
        return 1;
    }

    struct pt_stream *st = pt_stream_connect(host, (uint16_t)port);
    if (!st) {
        printf("prosperotrace: cannot reach receiver %s:%u\n", host, port);
        pt_detach(pid);
        return 1;
    }
    pt_log_hello(st, pid, name ? name : "?", "13.40");

    if (pt_hooks_install(pid, st) != 0)
        printf("prosperotrace: hook install incomplete, streaming lifecycle events only\n");

    while (pt_target_alive(pid))
        sleep(1);

    pt_hooks_remove(pid);
    pt_detach(pid);
    pt_stream_close(st);
    printf("prosperotrace: done\n");
    return 0;
}
