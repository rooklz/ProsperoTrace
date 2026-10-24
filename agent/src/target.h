#pragma once

#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

/*
 * Target process access via the kernel debugging interface.
 *
 * The SDK headers don't declare ptrace on this platform, so the prototype
 * is provided here; payloads resolve it through libc like any other call.
 * (ps5debug/WebRTE use the same interface.)
 */
int ptrace(int request, pid_t pid, void *addr, int data);

int  pt_find_pid(const char *name_substr);
int  pt_attach(pid_t pid);
int  pt_detach(pid_t pid);

/* word-granular target memory I/O; returns -1 on error */
int  pt_read(pid_t pid, uint64_t addr, void *buf, size_t len);
int  pt_write(pid_t pid, uint64_t addr, const void *buf, size_t len);

/* nonzero while the target is alive (PT_EVENT/wait status based) */
int  pt_target_alive(pid_t pid);
