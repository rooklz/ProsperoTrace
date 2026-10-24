/*
 * ProsperoTrace agent entry point.
 *
 * Configuration comes from the environment when the loader sets one, else
 * from /data/prosperotrace.conf (KEY=VALUE lines, # comments), since elfldr
 * starts payloads without an environment and drops their stdout; in that
 * case progress goes to /data/prosperotrace.log:
 *   PT_HOST      receiver address (required)
 *   PT_PORT      receiver port (default 9029)
 *   PT_PID       attach to this pid, or
 *   PT_NAME      substring of the target's process name, or
 *   PT_PID_FILE  file holding "<pid> [<load base> [<_DYNAMIC>]]" written by
 *                the target
 *   PT_BASE, PT_DYNAMIC  main ELF load base and _DYNAMIC address (needed
 *                for elfldr payloads; may come from PT_PID_FILE)
 *   PT_FILTER    comma-separated mod.fn globs
 *   PT_WAIT      seconds to wait for PT_PID_FILE to appear (default 30)
 */
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <ps5/kernel.h>

#include "conf.h"
#include "filter.h"
#include "hook.h"
#include "stream.h"
#include "target.h"

#define PT_DEFAULT_PORT 9029
#define PT_CONF "/data/prosperotrace.conf"
#define PT_LOG  "/data/prosperotrace.log"

static char g_conf[2048];

static void load_conf(void)
{
    FILE *f = fopen(PT_CONF, "r");
    if (!f)
        return;
    size_t n = fread(g_conf, 1, sizeof(g_conf) - 1, f);
    fclose(f);
    g_conf[n] = 0;
}

static const char *conf(const char *key)
{
    const char *v = getenv(key);
    return v ? v : pt_conf_get(g_conf, key);
}

static pid_t pid_from_file(const char *path, unsigned wait_s, uint64_t *base, uint64_t *dyn)
{
    for (unsigned i = 0; i <= wait_s * 10; i++) {
        FILE *f = fopen(path, "r");
        if (f) {
            int pid = 0;
            unsigned long long b = 0, d = 0;
            int n = fscanf(f, "%d %llx %llx", &pid, &b, &d);
            fclose(f);
            if (n >= 1 && pid > 0) {
                if (n >= 2)
                    *base = b;
                if (n >= 3)
                    *dyn = d;
                return pid;
            }
        }
        usleep(100000);
    }
    return -1;
}

static volatile sig_atomic_t g_stop;

static void on_term(int sig)
{
    (void)sig;
    g_stop = 1;
}

int main(void)
{
    load_conf();
    if (g_conf[0] && freopen(PT_LOG, "w", stdout))
        setvbuf(stdout, 0, _IONBF, 0);
    const char *host = conf("PT_HOST");
    const char *name = conf("PT_NAME");
    const char *pid_s = conf("PT_PID");
    const char *pid_file = conf("PT_PID_FILE");
    const char *port_s = conf("PT_PORT");
    const char *base_s = conf("PT_BASE");
    const char *wait_s = conf("PT_WAIT");
    const char *filt = conf("PT_FILTER");
    unsigned port = port_s ? (unsigned)strtoul(port_s, 0, 10) : PT_DEFAULT_PORT;
    const char *dyn_s = conf("PT_DYNAMIC");
    uint64_t base = base_s ? strtoull(base_s, 0, 16) : 0;
    uint64_t dyn = dyn_s ? strtoull(dyn_s, 0, 16) : 0;

    if (filt && pt_set_filter(filt) != 0) {
        printf("prosperotrace: bad PT_FILTER\n");
        return 1;
    }
    if (pt_filter_is_set())
        printf("prosperotrace: filter: %s\n", filt);

    if (!host || (!name && !pid_s && !pid_file)) {
        printf("prosperotrace: need PT_HOST and one of PT_PID / PT_NAME / PT_PID_FILE\n");
        return 1;
    }

    pid_t pid = -1;
    if (pid_s)
        pid = (pid_t)strtol(pid_s, 0, 10);
    else if (pid_file)
        pid = pid_from_file(pid_file, wait_s ? (unsigned)atoi(wait_s) : 30, &base, &dyn);
    else
        pid = pt_find_pid(name);
    if (pid <= 0) {
        printf("prosperotrace: target not found\n");
        return 1;
    }
    printf("prosperotrace: attaching pid %d\n", pid);

    struct pt_stream *st = pt_stream_connect(host, (uint16_t)port);
    if (!st) {
        printf("prosperotrace: cannot reach receiver %s:%u\n", host, port);
        return 1;
    }
    if (pt_attach(pid) != 0) {
        printf("prosperotrace: attach failed\n");
        pt_stream_close(st);
        return 1;
    }
    uint32_t v = kernel_get_fw_version();
    char fw[16];
    snprintf(fw, sizeof(fw), "%x.%02x", (v >> 24) & 0xff, (v >> 16) & 0xff);
    pt_log_hello(st, pid, name ? name : (pid_file ? pid_file : "?"), fw);

    pt_hooks_set_base(base, dyn);
    if (pt_hooks_install(pid, st) != 0) {
        printf("prosperotrace: hook install failed, detaching\n");
        pt_hooks_remove(pid, 1);
        pt_detach(pid);
        pt_stream_close(st);
        return 1;
    }

    signal(SIGTERM, on_term);
    signal(SIGINT, on_term);
    pt_resume(pid, 0);
    while (!g_stop && pt_target_alive(pid))
        sleep(1);

    if (g_stop && pt_stop(pid) == 0) {
        pt_hooks_remove(pid, 1);   /* target stopped: no thread is mid-swap */
        pt_detach(pid);
    } else {
        pt_hooks_remove(pid, 0);   /* target gone: just stop the reader */
    }
    pt_stream_close(st);
    printf("prosperotrace: done\n");
    return 0;
}
