#include "target.h"

#include <stdlib.h>
#include <string.h>
#include <sys/sysctl.h>
#include <sys/wait.h>

/* Orbis ptrace requests (values match the platform headers) */
#define PT_ATTACH   10
#define PT_DETACH   11
#define PT_READ_D   2
#define PT_WRITE_D  5
#define PT_CONTINUE 7
#define PT_STEP     9

/*
 * kern.proc.all walk. The kernel returns an array of process-info records
 * in a firmware-specific layout; only the leading fields are stable enough
 * to rely on, so we scan for (pid, comm) pairs at the known offsets.
 *
 * NOTE: KP_* offsets are per-structure-version — verify against a live
 * system before trusting name matches (see docs/DESIGN.md, failure rules).
 */
#define KP_PID_OFF 0x48
#define KP_COMM_OFF 0x1BF
#define KP_REC_SIZE 0x300
#define KP_COMM_MAX 32

int pt_find_pid(const char *name_substr)
{
    size_t len = 0;
    if (sysctlbyname("kern.proc.all", 0, &len, 0, 0) != 0 || len == 0)
        return -1;
    char *buf = malloc(len);
    if (!buf)
        return -1;
    if (sysctlbyname("kern.proc.all", buf, &len, 0, 0) != 0) {
        free(buf);
        return -1;
    }
    pid_t found = -1;
    for (size_t off = 0; off + KP_REC_SIZE <= len; off += KP_REC_SIZE) {
        const char *rec = buf + off;
        char comm[KP_COMM_MAX + 1] = { 0 };
        memcpy(comm, rec + KP_COMM_OFF, KP_COMM_MAX);
        if (strstr(comm, name_substr)) {
            memcpy(&found, rec + KP_PID_OFF, sizeof(found));
            break;
        }
    }
    free(buf);
    return found;
}

int pt_attach(pid_t pid)
{
    if (ptrace(PT_ATTACH, pid, 0, 0) != 0)
        return -1;
    int status = 0;
    if (waitpid(pid, &status, 0) < 0)
        return -1;
    return 0;
}

int pt_detach(pid_t pid)
{
    return ptrace(PT_DETACH, pid, 0, 0);
}

static int xfers(pid_t pid, int req, uint64_t addr, void *buf, size_t len, void *pad)
{
    (void)pad;
    uint64_t *dst = buf;
    const uint64_t *src = buf;
    size_t words = (len + 7) / 8;
    for (size_t i = 0; i < words; i++) {
        if (req == PT_READ_D) {
            long v = ptrace(PT_READ_D, pid, (void *)(addr + i * 8), 0);
            if (v == -1)
                return -1;
            dst[i] = (uint64_t)v;
        } else {
            long v = ptrace(PT_WRITE_D, pid, (void *)(addr + i * 8), (int)src[i]);
            if (v == -1)
                return -1;
        }
    }
    return 0;
}

int pt_read(pid_t pid, uint64_t addr, void *buf, size_t len)
{
    return xfers(pid, PT_READ_D, addr, buf, len, 0);
}

int pt_write(pid_t pid, uint64_t addr, const void *buf, size_t len)
{
    /* ptrace word writes are read-modify-write safe for our aligned targets */
    return xfers(pid, PT_WRITE_D, addr, (void *)buf, len, 0);
}

int pt_target_alive(pid_t pid)
{
    int status = 0;
    pid_t r = waitpid(pid, &status, 1 /* WNOHANG */);
    if (r == 0)
        return 1;
    if (r == pid && (status & 0xff) != 0x7f)
        return 0;   /* exited or killed (not a stop) */
    return 1;
}
