#include "hook.h"
#include "nids.h"
#include "target.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ELF64 definitions needed locally (toolchain headers vary on these) */
typedef struct {
    unsigned char e_ident[16];
    uint16_t e_type, e_machine;
    uint32_t e_version;
    uint64_t e_entry, e_phoff, e_shoff;
    uint32_t e_flags;
    uint16_t e_ehsize, e_phentsize, e_phnum;
    uint16_t e_shentsize, e_shnum, e_shstrndx;
} Elf64_Ehdr;

typedef struct {
    uint32_t p_type, p_flags;
    uint64_t p_offset, p_vaddr, p_paddr, p_filesz, p_memsz, p_align;
} Elf64_Phdr;

typedef struct { int64_t d_tag; union { uint64_t d_val, d_ptr; } d_un; } Elf64_Dyn;
typedef struct { uint64_t r_offset, r_info; int64_t r_addend; } Elf64_Rela;
typedef struct { uint64_t st_name; uint8_t st_info, st_other; uint16_t st_shndx; uint64_t st_value, st_size; } Elf64_Sym;

#define PT_DYNAMIC  2
#define DT_NULL     0
#define DT_PLTRELSZ 2
#define DT_PLTGOT   3
#define DT_STRTAB   5
#define DT_SYMTAB   6
#define DT_RELAENT  9
#define DT_JMPREL   23

#define ELF64_R_SYM(i)      ((i) >> 32)
#define ELF64_R_TYPE(i)     ((i) & 0xffffffff)
#define R_X86_64_JUMP_SLOT  7

#define ELFMAG   "\177ELF"
#define SELFMAG  4

/*
 * Module enumeration: the platform's dynamic linker keeps per-process
 * module metadata behind libkernel's dynlib helpers. We resolve the two
 * entry points we need (list handles, map a handle to its ELF base) the
 * same way other payloads do — by lookup against the already-resolved
 * libkernel imports of our own process. Handles are small integers, and
 * handle 0 is the main executable.
 */

#define MAX_MODULES   64
#define MAX_HOOKS     512
#define TRAMP_PAGE    0x4000

struct hook_rec {
    uint64_t slot_addr;   /* GOT address in the target */
    uint64_t original;    /* original slot value */
};

static struct hook_rec g_hooks[MAX_HOOKS];
static int g_hook_count = -1;
static struct pt_stream *g_stream;

/* ---- target memory helpers with validation -------------------------- */

static int read_or_fail(pid_t pid, uint64_t addr, void *buf, size_t len, const char *what)
{
    if (pt_read(pid, addr, buf, len) != 0) {
        printf("prosperotrace: read %s @0x%llx failed\n", what, (unsigned long long)addr);
        return -1;
    }
    return 0;
}

/* ---- ELF/dynamic parsing -------------------------------------------- */

struct dyninfo {
    uint64_t jmprel;      /* DT_JMPREL */
    uint64_t pltrelsz;    /* DT_PLTRELSZ */
    uint64_t pltgot;      /* DT_PLTGOT */
    uint64_t strtab;      /* DT_STRTAB */
    uint64_t symtab;      /* DT_SYMTAB */
    uint64_t relaent;     /* DT_RELAENT (24 expected) */
};

static int read_dynamic(pid_t pid, uint64_t base, struct dyninfo *di)
{
    Elf64_Ehdr eh;
    if (read_or_fail(pid, base, &eh, sizeof(eh), "ehdr") != 0)
        return -1;
    if (memcmp(eh.e_ident, ELFMAG, SELFMAG) != 0)
        return -1;

    Elf64_Phdr ph[16];
    if (eh.e_phnum > 16)
        return -1;
    if (read_or_fail(pid, base + eh.e_phoff, ph, eh.e_phnum * sizeof(Elf64_Phdr), "phdr") != 0)
        return -1;

    uint64_t dyn_addr = 0;
    for (int i = 0; i < eh.e_phnum; i++)
        if (ph[i].p_type == PT_DYNAMIC)
            dyn_addr = base + ph[i].p_vaddr;
    if (!dyn_addr)
        return -1;

    memset(di, 0, sizeof(*di));
    di->relaent = sizeof(Elf64_Rela);
    for (;;) {
        Elf64_Dyn d;
        if (read_or_fail(pid, dyn_addr, &d, sizeof(d), "dyn") != 0)
            return -1;
        dyn_addr += sizeof(d);
        if (d.d_tag == DT_NULL)
            break;
        switch (d.d_tag) {
        case DT_JMPREL:   di->jmprel   = base + d.d_un.d_ptr; break;
        case DT_PLTRELSZ: di->pltrelsz = d.d_un.d_val;        break;
        case DT_PLTGOT:   di->pltgot   = base + d.d_un.d_ptr; break;
        case DT_STRTAB:   di->strtab   = base + d.d_un.d_ptr; break;
        case DT_SYMTAB:   di->symtab   = base + d.d_un.d_ptr; break;
        }
    }
    return (di->jmprel && di->pltrelsz && di->pltgot) ? 0 : -1;
}

/*
 * The platform names imports by NID, not by string: each JUMP_SLOT's symbol
 * carries the NID in st_name's upper bits under Sony's extended info scheme.
 * nid_of_reloc() is where that extraction lives; the seed table carries
 * known (nid -> name) pairs and anything unresolved is logged by raw NID.
 */
static uint64_t nid_of_reloc(pid_t pid, const struct dyninfo *di, const Elf64_Rela *r)
{
    Elf64_Sym sym;
    uint64_t sym_addr = di->symtab + ELF64_R_SYM(r->r_info) * sizeof(sym);
    if (pt_read(pid, sym_addr, &sym, sizeof(sym)) != 0)
        return 0;
    return sym.st_name;   /* platform stores the NID here for dynlink imports */
}

static int hook_module(pid_t pid, uint64_t base)
{
    struct dyninfo di;
    if (read_dynamic(pid, base, &di) != 0)
        return 0;   /* not a dynamic module (e.g. PRX without PLT) — skip */

    size_t count = di.pltrelsz / di.relaent;
    for (size_t i = 0; i < count && g_hook_count < MAX_HOOKS; i++) {
        Elf64_Rela r;
        uint64_t raddr = di.jmprel + i * di.relaent;
        if (pt_read(pid, raddr, &r, sizeof(r)) != 0)
            return -1;
        if (ELF64_R_TYPE(r.r_info) != R_X86_64_JUMP_SLOT)
            continue;

        uint64_t nid = nid_of_reloc(pid, &di, &r);
        const struct nid_entry *e = nid ? nid_lookup(nid) : 0;

        uint64_t slot = di.pltgot + 24 + i * 8;  /* skip resolver+linkmap entries */
        uint64_t orig;
        if (pt_read(pid, slot, &orig, sizeof(orig)) != 0)
            return -1;

        /* M2: trampolines are emitted into a page allocated in the target
         * (PT_SYSCTRL mmap). Each trampoline captures the six argument
         * registers, calls pt_log_call via the agent's shared stub, then
         * tail-jumps to `orig`. Until the emitter lands we only record the
         * slot — no writes happen yet. */
        g_hooks[g_hook_count].slot_addr = slot;
        g_hooks[g_hook_count].original = orig;
        g_hook_count++;
        printf("  hook cand: %-18s 0x%llx -> %s.%s\n",
               "", (unsigned long long)slot,
               e ? e->mod : "?", e ? e->fn : "(unnamed nid)");
    }
    return 0;
}

int pt_hooks_install(pid_t pid, struct pt_stream *st)
{
    g_stream = st;
    g_hook_count = 0;

    /* module bases: dynlib handle walk — handle 0 (main executable) first,
     * then the loaded PRX set. Platform glue: resolve the handle list via
     * libkernel's dynlib iterator; until then we instrument the main
     * executable only. */
    uint64_t bases[MAX_MODULES] = { 0 };
    size_t nbases = 0;

    uint64_t main_base = 0;  /* TODO(M2): from target's auxv/dynlink state */
    if (main_base)
        bases[nbases++] = main_base;

    for (size_t i = 0; i < nbases; i++)
        if (hook_module(pid, bases[i]) != 0) {
            pt_hooks_remove(pid);
            return -1;
        }

    printf("prosperotrace: %d hook candidates mapped (writes pending M2 trampoline emitter)\n",
           g_hook_count);
    return 0;
}

int pt_hooks_remove(pid_t pid)
{
    int rc = 0;
    for (int i = 0; i < g_hook_count; i++) {
        uint64_t cur;
        if (pt_read(pid, g_hooks[i].slot_addr, &cur, sizeof(cur)) == 0 &&
            cur != g_hooks[i].original &&
            pt_write(pid, g_hooks[i].slot_addr, &g_hooks[i].original, 8) != 0)
            rc = -1;
    }
    g_hook_count = -1;
    g_stream = 0;
    return rc;
}

int pt_hooks_count(void)
{
    return g_hook_count;
}
