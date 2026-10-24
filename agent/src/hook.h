#pragma once

#include <stdint.h>
#include <sys/types.h>

#include "stream.h"

/*
 * PLT/GOT hooking in the attached target.
 *
 * Parses the target's main module (.rela.plt and .rela.dyn), and for every
 * function import (JUMP_SLOT or GLOB_DAT) the filter allows, maps a
 * trampoline into the target and points the GOT slot at it. The trampoline
 * records arguments and return value in a ring inside the target, which a
 * reader thread copies out and streams through `st`.
 *
 * The target must be stopped (attached) during install and remove.
 *
 * Install is all-or-nothing: any failure restores every slot already
 * patched before returning an error (docs/DESIGN.md, failure rules).
 */

/* load base and _DYNAMIC of the target's main ELF when the platform loader
 * did not map it (elfldr payloads, whose headers are not in memory);
 * 0, 0 = look up eboot.bin and parse its headers */
void pt_hooks_set_base(uint64_t base, uint64_t dynamic);

int pt_hooks_install(pid_t pid, struct pt_stream *st);
/* restore = 0 when the target is gone: only stop the reader */
int pt_hooks_remove(pid_t pid, int restore);

/* number of GOT slots currently patched, -1 when not installed */
int pt_hooks_count(void);
