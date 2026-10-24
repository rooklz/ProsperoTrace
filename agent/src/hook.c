#include "hook.h"
#include "filter.h"
#include "nids.h"
#include "ring.h"
#include "ring_decode.h"
#include "target.h"
#include "trampoline.h"

#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/mman.h>

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

#define MAX_MODULES   64
#define MAX_HOOKS     512

#define RING_PAGES  (PT_RING_SIZE / 0x1000 + 2)

/* ------------------------------------------------------------------ */

struct hook_rec {
    uint64_t slot_addr;          /* GOT address in the target */
    uint64_t original;           /* original slot value        */
    uint64_t nid;
    char mod[32];
    char fn[64];
    uint8_t *code;               /* agent-side emitted trampoline */
    uint64_t code_target;        /* target VA once armed         */
    int patched;
};

static struct hook_rec g_hooks[MAX_HOOKS];
static int g_hook_count = -1;

static struct pt_ring *g_ring;          /* agent-side view of the shared ring */
static pthread_t g_reader;
static int g_reader_run;

/* module enumeration: platform's dynamic linker keeps per-process metadata
 * behind libkernel's dynlib helpers. Resolved the way other payloads do it —
 * handle 0 is the main executable; the loaded PRX set follows (M2.1). */
static uint64_t module_base(pid_t pid, int handle)
{
    (void)pid;
    (void)handle;
    return 0;   /* glue: dynlib handle -> ELF base; see docs/DESIGN.md */
}

/* ---- target memory helpers with validation ----------------------- */

static int read_or_fail(pid_t pid, uint64_t addr, void *buf, size_t len, const char *what)
{
    if (pt_read(pid, addr, buf, len) != 0) {
        printf("prosperotrace: read %s @0x%llx failed\n", what, (unsigned long long)addr);
        return -1;
    }
    return 0;
}

/* ---- ELF/dynamic parsing ------------------------------------------- */

struct dyninfo {
    uint64_t jmprel;
    uint64_t pltrelsz;
    uint64_t pltgot;
    uint64_t strtab;
    uint64_t symtab;
    uint64_t relaent;
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

/* The platform names imports by NID: each JUMP_SLOT symbol carries the NID
 * in st_name under Sony's extended info scheme. */
static uint64_t nid_of_reloc(pid_t pid, const struct dyninfo *di, const Elf64_Rela *r)
{
    Elf64_Sym sym;
    uint64_t sym_addr = di->symtab + ELF64_R_SYM(r->r_info) * sizeof(sym);
    if (pt_read(pid, sym_addr, &sym, sizeof(sym)) != 0)
        return 0;
    return sym.st_name;
}

static int hook_module(pid_t pid, uint64_t base)
{
    struct dyninfo di;
    int skipped = 0;
    if (read_dynamic(pid, base, &di) != 0)
        return 0;   /* not a dynamic module (no PLT) — skip */

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
        const char *mod = e ? e->mod : "?";
        const char *fn = e ? e->fn : "nid_unresolved";

        if (!pt_filter_allows(mod, fn)) {
            skipped++;
            continue;
        }

        uint64_t slot = di.pltgot + 24 + i * 8;  /* skip resolver+linkmap */
        uint64_t orig;
        if (pt_read(pid, slot, &orig, sizeof(orig)) != 0)
            return -1;

        struct hook_rec *h = &g_hooks[g_hook_count];
        h->slot_addr = slot;
        h->original = orig;
        h->nid = nid;
        snprintf(h->mod, sizeof(h->mod), "%s", mod);
        if (e)
            snprintf(h->fn, sizeof(h->fn), "%s", e->fn);
        else
            snprintf(h->fn, sizeof(h->fn), "nid_0x%llx", (unsigned long long)nid);
        h->code = 0;
        h->code_target = 0;
        h->patched = 0;
        g_hook_count++;
        printf("  hook cand: %-24s 0x%llx\n", h->fn, (unsigned long long)slot);
    }
    return skipped;
}

/* ---- ring service thread -------------------------------------------- */

struct emit_ctx { struct pt_stream *st; };

static void on_pair(uint32_t idx, const uint64_t args[6], uint64_t ret, void *ctx)
{
    if (idx >= (uint32_t)MAX_HOOKS || g_hooks[idx].fn[0] == 0)
        return;
    struct emit_ctx *ec = ctx;
    pt_log_call(ec->st, 0, g_hooks[idx].mod, g_hooks[idx].fn,
                g_hooks[idx].nid, args, ret);
}

static void *reader_thread(void *arg)
{
    struct emit_ctx *ec = arg;
    while (g_reader_run) {
        ring_decode(g_ring, on_pair, ec);
        struct timespec ts = { 0, 2000000 };  /* 2 ms */
        nanosleep(&ts, 0);
    }
    ring_decode(g_ring, on_pair, ec);         /* best-effort final flush */
    return 0;
}

static struct emit_ctx g_emit_ctx;

/* Remote page injection — the one piece of platform-firmware glue left.
 * PT_SYSCTRL carries the remote-map request the payload wrappers expose.
 * While it returns -1 the install runs in candidates-only mode and the
 * trampolines are emitted and byte-checked but never armed. */
static int inject_remote(pid_t pid, const void *local, size_t size, uint64_t *out_va)
{
    (void)pid;
    (void)local;
    (void)size;
    (void)out_va;
    return -1;
}

/* ---- public interface ------------------------------------------------ */

int pt_hooks_install(pid_t pid, struct pt_stream *st)
{
    g_emit_ctx.st = st;
    g_hook_count = 0;
    g_ring = 0;
    g_reader_run = 0;
    size_t skipped_total = 0;

    /* module set: main executable first (M2.1 adds the loaded PRXs) */
    uint64_t bases[MAX_MODULES];
    size_t nbases = 0;
    uint64_t main_base = module_base(pid, 0);
    if (main_base)
        bases[nbases++] = main_base;

    for (size_t i = 0; i < nbases; i++) {
        int sk = hook_module(pid, bases[i]);
        if (sk < 0) {
            pt_hooks_remove(pid);
            return -1;
        }
        skipped_total += (size_t)sk;
    }

    if (g_hook_count == 0) {
        printf("prosperotrace: no hook candidates%s\n",
               skipped_total ? " (all filtered out)" : "");
        return 0;
    }

    /* shared ring (agent side; mirrored into the target once glue lands) */
    g_ring = mmap(0, RING_PAGES * 0x1000, PROT_READ | PROT_WRITE,
                  MAP_PRIVATE | MAP_ANON, -1, 0);
    if (g_ring == MAP_FAILED) {
        g_ring = 0;
        printf("prosperotrace: ring mmap failed\n");
        return -1;
    }
    memset(g_ring, 0, RING_PAGES * 0x1000);

    /* trampoline page */
    size_t code_pages = (size_t)MAX_HOOKS * PT_TRAMPOLINE_MAX;
    code_pages = (code_pages / 0x1000 + 1) * 0x1000;
    uint8_t *code = mmap(0, code_pages, PROT_READ | PROT_WRITE | PROT_EXEC,
                         MAP_PRIVATE | MAP_ANON, -1, 0);
    if (code == MAP_FAILED) {
        printf("prosperotrace: code mmap failed\n");
        munmap(g_ring, RING_PAGES * 0x1000);
        g_ring = 0;
        return -1;
    }

    uint64_t ring_tva = 0, code_tva = 0;
    int has_tvm = (inject_remote(pid, g_ring, RING_PAGES * 0x1000, &ring_tva) == 0) &&
                  (inject_remote(pid, code, code_pages, &code_tva) == 0);

    size_t emitted = 0, armed = 0;
    for (int i = 0; i < g_hook_count; i++) {
        struct hook_rec *h = &g_hooks[i];
        size_t len = 0;
        h->code = code + (size_t)i * PT_TRAMPOLINE_MAX;
        if (trampoline_emit(has_tvm ? ring_tva : (uint64_t)(uintptr_t)g_ring,
                            h->original, (uint32_t)i,
                            h->code, PT_TRAMPOLINE_MAX, &len) == 0) {
            emitted++;
            if (has_tvm) {
                h->code_target = code_tva + (uint64_t)i * PT_TRAMPOLINE_MAX;
                if (pt_write(pid, h->slot_addr, &h->code_target, 8) == 0) {
                    h->patched = 1;
                    armed++;
                }
            }
        }
    }

    if (armed) {
        g_reader_run = 1;
        if (pthread_create(&g_reader, 0, reader_thread, &g_emit_ctx) != 0)
            g_reader_run = 0;
    }

    printf("prosperotrace: %d/%d slots emitted, %lu armed, %zu filtered%s\n",
           (int)emitted, g_hook_count, armed, skipped_total,
           has_tvm ? "" : " (remote page injection glue pending — see docs/DESIGN.md)");
    return 0;
}

int pt_hooks_remove(pid_t pid)
{
    if (g_reader_run) {
        g_reader_run = 0;
        pthread_join(g_reader, 0);
    }
    int rc = 0;
    for (int i = 0; i < g_hook_count; i++) {
        if (g_hooks[i].patched) {
            uint64_t cur;
            if (pt_read(pid, g_hooks[i].slot_addr, &cur, sizeof(cur)) == 0 &&
                cur != g_hooks[i].original &&
                pt_write(pid, g_hooks[i].slot_addr, &g_hooks[i].original, 8) != 0)
                rc = -1;
            g_hooks[i].patched = 0;
        }
    }
    memset(&g_emit_ctx, 0, sizeof(g_emit_ctx));
    if (g_ring) {
        munmap(g_ring, RING_PAGES * 0x1000);
        g_ring = 0;
    }
    g_hook_count = -1;
    return rc;
}

int pt_hooks_count(void)
{
    return g_hook_count;
}