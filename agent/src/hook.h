#pragma once

#include <sys/types.h>

#include "stream.h"

/*
 * PLT/GOT hooking in the attached target.
 *
 * Walks the target's loaded modules, parses each module's .rela.plt, and
 * for every R_X86_64_JUMP_SLOT relocation whose symbol NID is in our table,
 * replaces the resolved GOT slot with a trampoline that logs through `st`
 * and tail-calls the original function.
 *
 * Install is all-or-nothing: any failure restores every slot already
 * patched before returning an error (docs/DESIGN.md, failure rules).
 */

int pt_hooks_install(pid_t pid, struct pt_stream *st);
int pt_hooks_remove(pid_t pid);

/* number of GOT slots currently patched, -1 when not installed */
int pt_hooks_count(void);
