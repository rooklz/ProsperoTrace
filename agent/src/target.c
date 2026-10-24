#include "target.h"

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <sys/sysctl.h>
#include <sys/wait.h>
#include <machine/reg.h>
#include <sys/ptrace.h>
#include <ps5/kernel.h>
#include <ps5/mdbg.h>

/* Orbis ptrace requests (values match the platform headers) */

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

/* ptrace with the debugger credentials the SDK's test_privileges sample
 * uses, restoring the agent's own afterwards */
static int dbg_ptrace(int req, pid_t pid, void *addr, int data)
{
    static const uint8_t privcaps[16] = {
        0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
        0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
    };
    pid_t me = getpid();
    uint8_t caps[16];
    uint64_t authid = kernel_get_ucred_authid(me);
    if (!authid || kernel_get_ucred_caps(me, caps) != 0)
        return -1;
    if (kernel_set_ucred_authid(me, 0x4800000000010003ull) != 0 ||
        kernel_set_ucred_caps(me, privcaps) != 0)
        return -1;
    int ret = (int)__syscall(SYS_ptrace, req, pid, addr, data);
    kernel_set_ucred_authid(me, authid);
    kernel_set_ucred_caps(me, caps);
    return ret;
}

int pt_attach(pid_t pid)
{
    if (dbg_ptrace(PT_ATTACH, pid, 0, 0) != 0)
        return -1;
    int status = 0;
    if (waitpid(pid, &status, 0) < 0)
        return -1;
    return 0;
}

int pt_detach(pid_t pid)
{
    return dbg_ptrace(PT_DETACH, pid, (void *)1, 0);
}

int pt_resume(pid_t pid, int sig)
{
    return dbg_ptrace(PT_CONTINUE, pid, (void *)1, sig);
}

int pt_stop(pid_t pid)
{
    if (kill(pid, SIGSTOP) != 0)
        return -1;
    int status = 0;
    for (;;) {
        if (waitpid(pid, &status, 0) != pid)
            return -1;
        if (!WIFSTOPPED(status))
            return -1;                       /* target exited meanwhile */
        if (WSTOPSIG(status) == SIGSTOP)
            return 0;
        pt_resume(pid, WSTOPSIG(status));    /* deliver anything else */
    }
}

/* PT_READ_D/PT_WRITE_D move an int, not a word; the SDK's mdbg calls copy
 * whole buffers through the kernel debug interface. */
int pt_read(pid_t pid, uint64_t addr, void *buf, size_t len)
{
    return mdbg_copyout(pid, (intptr_t)addr, buf, len) == 0 ? 0 : -1;
}

/* page by page; where the debug interface refuses (fresh anonymous pages,
 * read-only GOT pages), fall back to a kernel-side copy */
int pt_write(pid_t pid, uint64_t addr, const void *buf, size_t len)
{
    const uint8_t *p = buf;
    while (len) {
        size_t n = 0x1000 - (addr & 0xfff);
        if (n > len)
            n = len;
        struct ptrace_io_desc io = { PIOD_WRITE_D, (void *)(uintptr_t)addr, (void *)p, n };
        if (mdbg_copyin(pid, p, (intptr_t)addr, n) != 0 &&
            kernel_proc_copyin(pid, p, (intptr_t)addr, n) != 0 &&
            (dbg_ptrace(PT_IO, pid, (void *)&io, 0) != 0 || io.piod_len != n)) {
            printf("prosperotrace: write of %zu bytes at 0x%llx failed\n", n, (unsigned long long)addr);
            return -1;
        }
        addr += n;
        p += n;
        len -= n;
    }
    return 0;
}

int pt_target_alive(pid_t pid)
{
    int status = 0;
    pid_t r = waitpid(pid, &status, WNOHANG);
    if (r == 0)
        return 1;
    if (r != pid)
        return 0;
    if (!WIFSTOPPED(status))
        return 0;                            /* exited or killed */
    int sig = WSTOPSIG(status);
    pt_resume(pid, (sig == SIGSTOP || sig == SIGTRAP) ? 0 : sig);
    return 1;
}

/* Run one syscall inside the stopped target: point rip at a `syscall`
 * instruction in its own libkernel, single-step it, restore everything. */
static uint64_t g_syscall_insn;

int pt_syscall_hint(pid_t pid, uint64_t fn)
{
    uint8_t code[32];
    if (g_syscall_insn || pt_read(pid, fn, code, sizeof(code)) != 0)
        return g_syscall_insn ? 0 : -1;
    for (size_t k = 0; k + 1 < sizeof(code); k++)
        if (code[k] == 0x0f && code[k + 1] == 0x05) {
            g_syscall_insn = fn + k;
            return 0;
        }
    return -1;
}

static int find_syscall_insn(pid_t pid)
{
    static const char *libs[] = { "libkernel.sprx", "libkernel_web.sprx", "libkernel_sys.sprx" };
    for (size_t i = 0; i < sizeof(libs) / sizeof(libs[0]); i++) {
        uint32_t h;
        if (kernel_dynlib_handle(pid, libs[i], &h) != 0)
            continue;
        intptr_t fn = kernel_dynlib_dlsym(pid, h, "getpid");
        if (!fn)
            continue;
        uint8_t code[32];
        if (pt_read(pid, (uint64_t)fn, code, sizeof(code)) != 0)
            continue;
        for (size_t k = 0; k + 1 < sizeof(code); k++)
            if (code[k] == 0x0f && code[k + 1] == 0x05) {
                g_syscall_insn = (uint64_t)fn + k;
                return 0;
            }
    }
    return -1;
}

int64_t pt_remote_syscall(pid_t pid, long nr, const uint64_t a[6])
{
    if (!g_syscall_insn && find_syscall_insn(pid) != 0) {
        printf("prosperotrace: no syscall instruction found in the target\n");
        return -1;
    }
    struct reg saved, r;
    if (dbg_ptrace(PT_GETREGS, pid, (void *)&saved, 0) != 0) {
        printf("prosperotrace: remote syscall: getregs failed\n");
        return -1;
    }
    r = saved;
    r.r_rip = (int64_t)g_syscall_insn;
    r.r_rax = nr;
    r.r_rdi = (int64_t)a[0];
    r.r_rsi = (int64_t)a[1];
    r.r_rdx = (int64_t)a[2];
    r.r_r10 = (int64_t)a[3];
    r.r_rcx = (int64_t)a[3];
    r.r_r8 = (int64_t)a[4];
    r.r_r9 = (int64_t)a[5];
    int64_t ret = -1;
    int status = 0;
    const char *fail = 0;
    if (dbg_ptrace(PT_SETREGS, pid, (void *)&r, 0) != 0)
        fail = "setregs";
    else if (dbg_ptrace(PT_STEP, pid, (void *)1, 0) != 0)
        fail = "step";
    else if (waitpid(pid, &status, 0) != pid || !WIFSTOPPED(status))
        fail = "wait";
    else if (dbg_ptrace(PT_GETREGS, pid, (void *)&r, 0) != 0)
        fail = "getregs";
    else if (r.r_rip != (int64_t)g_syscall_insn + 2)
        fail = "unexpected rip";
    else if (r.r_rflags & 1)
        printf("prosperotrace: remote syscall %ld: errno %ld\n", nr, (long)r.r_rax);
    else
        ret = r.r_rax;
    if (fail)
        printf("prosperotrace: remote syscall %ld: %s failed\n", nr, fail);
    if (dbg_ptrace(PT_SETREGS, pid, (void *)&saved, 0) != 0)
        return -1;
    return ret;
}

uint64_t pt_remote_mmap(pid_t pid, size_t size, int prot)
{
    const uint64_t a[6] = { 0, size, (uint64_t)prot, MAP_PRIVATE | MAP_ANON, (uint64_t)-1, 0 };
    int64_t va = pt_remote_syscall(pid, SYS_mmap, a);
    if (va <= 0)
        return 0;
    /* fault the pages in: fresh anonymous memory cannot be written from
     * outside until the target has touched it */
    const uint64_t l[6] = { (uint64_t)va, size, 0, 0, 0, 0 };
    if (pt_remote_syscall(pid, SYS_mlock, l) != 0)
        printf("prosperotrace: remote mlock failed, writes may not land\n");
    return (uint64_t)va;
}
