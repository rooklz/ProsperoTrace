#pragma once

/*
 * Hook selection filter. Empty/unset filter hooks everything in the NID
 * table; otherwise only `mod.fn` pairs matching at least one comma-separated
 * pattern are hooked. Patterns are shell-style globs: `*` and `?`.
 *
 *   PT_FILTER="libkernel.sceKernelOpen,libSceVideoOut.*"
 *   PT_FILTER="*"           (equivalent to unset)
 */

int pt_filter_is_set(void);
int pt_set_filter(const char *patterns);
int pt_filter_allows(const char *mod, const char *fn);

int pt_glob_match(const char *pattern, const char *s);