#pragma once

#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

/*
 * Target process access via the kernel debugging interface (ptrace, with
 * the debugger authid raised around each request; see target.c).
 */

int  pt_find_pid(const char *name_substr);
int  pt_attach(pid_t pid);
int  pt_detach(pid_t pid);

/* resume a stopped target (delivering sig, 0 for none) / stop it again */
int  pt_resume(pid_t pid, int sig);
int  pt_stop(pid_t pid);

/* target memory I/O (any length); returns -1 on error */
int  pt_read(pid_t pid, uint64_t addr, void *buf, size_t len);
int  pt_write(pid_t pid, uint64_t addr, const void *buf, size_t len);

/* nonzero while the target is alive; forwards signals that stopped it */
int  pt_target_alive(pid_t pid);

/* while the target is stopped: run syscall nr inside it (-1 on failure),
 * or map size bytes of anonymous memory in it (0 on failure) */
/* use the syscall instruction in the stub at fn (e.g. a resolved import) */
int      pt_syscall_hint(pid_t pid, uint64_t fn);
int64_t  pt_remote_syscall(pid_t pid, long nr, const uint64_t a[6]);
uint64_t pt_remote_mmap(pid_t pid, size_t size, int prot);
