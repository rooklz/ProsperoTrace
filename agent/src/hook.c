#include "hook.h"
#include "stream.h"
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
#include <ps5/kernel.h>

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
typedef struct { uint32_t st_name; uint8_t st_info, st_other; uint16_t st_shndx; uint64_t st_value, st_size; } Elf64_Sym;

#define PT_DYNAMIC  2
#define DT_NULL     0
#define DT_NEEDED   1
#define DT_PLTRELSZ 2
#define DT_PLTGOT   3
#define DT_STRTAB   5
#define DT_SYMTAB   6
#define DT_RELA     7
#define DT_RELASZ   8
#define DT_RELAENT  9
#define DT_JMPREL   23

#define ELF64_R_SYM(i)      ((i) >> 32)
#define ELF64_R_TYPE(i)     ((i) & 0xffffffff)
#define R_X86_64_GLOB_DAT   6
#define R_X86_64_JUMP_SLOT  7
#define STT_OBJECT          1

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

static struct pt_ring *g_ring;          /* agent-side snapshot of the shared ring */
static uint64_t g_ring_tva;             /* the ring inside the target */
static pid_t g_pid;
static pthread_t g_reader;
static int g_reader_run;
static uint64_t g_main_base, g_main_dyn;

void pt_hooks_set_base(uint64_t base, uint64_t dynamic)
{
    g_main_base = base;
    g_main_dyn = dynamic;
}

/* Main executable base: given by the caller (elfldr payloads are not in the
 * dynlib list), else the eboot the platform loader mapped. */
static uint64_t module_base(pid_t pid)
{
    if (g_main_base)
        return g_main_base;
    uint32_t h;
    if (kernel_dynlib_handle(pid, "eboot.bin", &h) != 0)
        return 0;
    return (uint64_t)kernel_dynlib_mapbase_addr(pid, h);
}

/* Calls a trampoline cannot wrap: they return twice, never return, or
 * switch stacks, and the wrapper's frame would be left dangling. */
static int never_hook(const char *fn)
{
    static const char *deny[] = {
        "setjmp", "_setjmp", "sigsetjmp", "__sigsetjmp", "longjmp", "_longjmp",
        "siglongjmp", "fork", "vfork", "rfork", "getcontext", "setcontext",
        "swapcontext", "makecontext", "exit", "_exit", "abort", "pthread_exit",
        "__cxa_throw", "__cxa_rethrow", "_Unwind_RaiseException", "_Unwind_Resume",
        "sceKernelSetjmp", "sceKernelLongjmp",
    };
    for (size_t i = 0; i < sizeof(deny) / sizeof(deny[0]); i++)
        if (strcmp(fn, deny[i]) == 0)
            return 1;
    return 0;
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

#define MAX_NEEDED 32

struct dyninfo {
    uint64_t needed[MAX_NEEDED];    /* strtab offsets of DT_NEEDED names */
    int nneeded;
    uint64_t jmprel;
    uint64_t pltrelsz;
    uint64_t rela;
    uint64_t relasz;
    uint64_t pltgot;
    uint64_t strtab;
    uint64_t symtab;
    uint64_t relaent;
};

static int read_dynamic(pid_t pid, uint64_t base, uint64_t dyn_addr, struct dyninfo *di)
{
    if (dyn_addr)
        goto have_dynamic;      /* loader did not map the headers (elfldr) */
    Elf64_Ehdr eh;
    if (read_or_fail(pid, base, &eh, sizeof(eh), "ehdr") != 0)
        return -1;
    if (memcmp(eh.e_ident, ELFMAG, SELFMAG) != 0) {
        printf("prosperotrace: no ELF header at 0x%llx\n", (unsigned long long)base);
        return -1;
    }

    Elf64_Phdr ph[16];
    if (eh.e_phnum > 16)
        return -1;
    if (read_or_fail(pid, base + eh.e_phoff, ph, eh.e_phnum * sizeof(Elf64_Phdr), "phdr") != 0)
        return -1;

    for (int i = 0; i < eh.e_phnum; i++)
        if (ph[i].p_type == PT_DYNAMIC)
            dyn_addr = base + ph[i].p_vaddr;
    if (!dyn_addr) {
        printf("prosperotrace: no PT_DYNAMIC at 0x%llx\n", (unsigned long long)base);
        return -1;
    }

have_dynamic:
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
        case DT_NEEDED:   if (di->nneeded < MAX_NEEDED) di->needed[di->nneeded++] = d.d_un.d_val; break;
        case DT_JMPREL:   di->jmprel   = base + d.d_un.d_ptr; break;
        case DT_PLTRELSZ: di->pltrelsz = d.d_un.d_val;        break;
        case DT_PLTGOT:   di->pltgot   = base + d.d_un.d_ptr; break;
        case DT_STRTAB:   di->strtab   = base + d.d_un.d_ptr; break;
        case DT_SYMTAB:   di->symtab   = base + d.d_un.d_ptr; break;
        case DT_RELA:     di->rela     = base + d.d_un.d_ptr; break;
        case DT_RELASZ:   di->relasz   = d.d_un.d_val;        break;
        case DT_RELAENT:  if (d.d_un.d_val) di->relaent = d.d_un.d_val; break;
        }
    }
    if (di->symtab && di->strtab && ((di->jmprel && di->pltrelsz) || (di->rela && di->relasz)))
        return 0;
    printf("prosperotrace: dynamic section lacks relocations (symtab %d strtab %d jmprel %d rela %d)\n",
           !!di->symtab, !!di->strtab, !!di->jmprel, !!di->rela);
    return -1;
}

/* One import slot. Retail modules name imports by NID string; payloads
 * built with ps5-payload-sdk use plain names, which elfldr resolves. */
static int hook_reloc(pid_t pid, uint64_t base, const struct dyninfo *di, const Elf64_Rela *r)
{
    uint32_t type = (uint32_t)ELF64_R_TYPE(r->r_info);
    if (type != R_X86_64_JUMP_SLOT && type != R_X86_64_GLOB_DAT)
        return 0;
    Elf64_Sym sym;
    if (pt_read(pid, di->symtab + ELF64_R_SYM(r->r_info) * sizeof(sym), &sym, sizeof(sym)) != 0)
        return -1;
    if ((sym.st_info & 0xf) == STT_OBJECT || sym.st_shndx != 0)
        return 0;                             /* data, or defined locally */
    char name[64];
    if (pt_read(pid, di->strtab + sym.st_name, name, sizeof(name)) != 0)
        return -1;
    name[sizeof(name) - 1] = 0;
    char *hash = strchr(name, '#');
    if (hash)
        *hash = 0;                            /* NID#lib#module suffix */
    if (!name[0] || never_hook(name))
        return 0;

    const struct nid_entry *e = nid_lookup_name(name);
    const char *mod = e ? e->mod : "?";   /* filtered once modules are known */

    uint64_t slot = base + r->r_offset;
    uint64_t orig;
    if (pt_read(pid, slot, &orig, sizeof(orig)) != 0)
        return -1;
    if (!orig)
        return 0;                             /* unresolved weak import */

    struct hook_rec *h = &g_hooks[g_hook_count++];
    memset(h, 0, sizeof(*h));
    h->slot_addr = slot;
    h->original = orig;
    h->nid = e ? e->nid : 0;
    snprintf(h->mod, sizeof(h->mod), "%s", mod);
    snprintf(h->fn, sizeof(h->fn), "%s", name);
    return 0;
}

static int hook_table(pid_t pid, uint64_t base, const struct dyninfo *di,
                      uint64_t table, uint64_t size)
{
    int skipped = 0;
    for (uint64_t off = 0; table && off + sizeof(Elf64_Rela) <= size && g_hook_count < MAX_HOOKS;
         off += di->relaent) {
        Elf64_Rela r;
        if (pt_read(pid, table + off, &r, sizeof(r)) != 0)
            return -1;
        int rc = hook_reloc(pid, base, di, &r);
        if (rc < 0)
            return -1;
        skipped += rc;
    }
    return skipped;
}

/* Imports named without a NID carry no module. Attribute each to the
 * DT_NEEDED library whose mapping starts closest below its resolved
 * address; "libkernel_web.sprx" becomes "libkernel_web". */
static void attribute_modules(pid_t pid, const struct dyninfo *di, int first)
{
    struct { uint64_t base; char name[32]; } libs[MAX_NEEDED];
    int n = 0;
    for (int i = 0; i < di->nneeded; i++) {
        char name[64];
        uint32_t h;
        if (pt_read(pid, di->strtab + di->needed[i], name, sizeof(name)) != 0)
            continue;
        name[sizeof(name) - 1] = 0;
        /* libkernel is linked as one of several aliases of the same module */
        static const char *kernel_alias[] = { "libkernel.sprx", "libkernel_sys.sprx", "libkernel_web.sprx" };
        int found = kernel_dynlib_handle(pid, name, &h) == 0;
        for (size_t a = 0; !found && strncmp(name, "libkernel", 9) == 0 && a < 3; a++)
            found = kernel_dynlib_handle(pid, kernel_alias[a], &h) == 0;
        if (!found)
            continue;
        if (strncmp(name, "libkernel", 9) == 0)
            snprintf(name, sizeof(name), "libkernel");
        libs[n].base = (uint64_t)kernel_dynlib_mapbase_addr(pid, h);
        char *dot = strstr(name, ".sprx");
        if (dot)
            *dot = 0;
        snprintf(libs[n].name, sizeof(libs[n].name), "%s", name);
        if (libs[n].base)
            n++;
    }
    for (int k = first; k < g_hook_count; k++) {
        struct hook_rec *h = &g_hooks[k];
        if (strcmp(h->mod, "?") != 0)
            continue;
        int best = -1;
        for (int i = 0; i < n; i++)
            if (libs[i].base <= h->original && (best < 0 || libs[i].base > libs[best].base))
                best = i;
        if (best >= 0)
            snprintf(h->mod, sizeof(h->mod), "%s", libs[best].name);
    }
}

static int hook_module(pid_t pid, uint64_t base, uint64_t dyn_addr)
{
    struct dyninfo di;
    if (read_dynamic(pid, base, dyn_addr, &di) != 0)
        return 0;   /* not a dynamic module — skip */
    int first = g_hook_count;
    int a = hook_table(pid, base, &di, di.jmprel, di.pltrelsz);
    int b = a < 0 ? -1 : hook_table(pid, base, &di, di.rela, di.relasz);
    if (b < 0)
        return -1;
    attribute_modules(pid, &di, first);

    int kept = first, skipped = 0;
    for (int k = first; k < g_hook_count; k++) {
        if (pt_filter_allows(g_hooks[k].mod, g_hooks[k].fn))
            g_hooks[kept++] = g_hooks[k];
        else
            skipped++;
    }
    g_hook_count = kept;
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

/* Copy the target's ring into the local snapshot; tail and the drop count
 * are the reader's own and stay local. */
static int snapshot_ring(void)
{
    uint32_t tail = g_ring->tail, dropped = g_ring->dropped;
    if (pt_read(g_pid, g_ring_tva, g_ring, PT_RING_SIZE) != 0)
        return -1;
    g_ring->tail = tail;
    g_ring->dropped = dropped;
    return 0;
}

#define STALL_POLLS 250     /* x 2 ms: give up on a call blocked this long */

static void *reader_thread(void *arg)
{
    struct emit_ctx *ec = arg;
    int stalled = 0;
    uint32_t last_tail = 0;
    while (g_reader_run) {
        if (snapshot_ring() == 0) {
            ring_decode(g_ring, on_pair, ec, stalled >= STALL_POLLS);
            if (g_ring->tail == last_tail && (g_ring->head & ~1u) != g_ring->tail)
                stalled++;
            else
                stalled = 0;
            last_tail = g_ring->tail;
            if (g_ring->dropped) {
                pt_log_drops(ec->st, g_ring->dropped);
                g_ring->dropped = 0;
            }
        }
        struct timespec ts = { 0, 2000000 };  /* 2 ms */
        nanosleep(&ts, 0);
    }
    if (snapshot_ring() == 0)                 /* final flush */
        ring_decode(g_ring, on_pair, ec, 0);
    return 0;
}

static struct emit_ctx g_emit_ctx;

/* Map size bytes in the stopped target and copy local into them. The
 * platform refuses RWX for ordinary processes, so code pages are written
 * while RW and then flipped to RX with the SDK's kernel_mprotect. */
static int inject_remote(pid_t pid, const void *local, size_t size, int exec, uint64_t *out_va)
{
    uint64_t va = pt_remote_mmap(pid, size, PROT_READ | PROT_WRITE);
    if (!va) {
        printf("prosperotrace: remote mmap of %zu bytes failed\n", size);
        return -1;
    }
    if (pt_write(pid, va, local, size) != 0) {
        printf("prosperotrace: copy into target page 0x%llx failed\n", (unsigned long long)va);
        return -1;
    }
    if (exec) {
        int want = PROT_READ | PROT_EXEC;
        if (kernel_mprotect(pid, (intptr_t)va, size, want) != 0 ||
            (kernel_get_vmem_protection(pid, (intptr_t)va, size) & want) != want) {
            printf("prosperotrace: could not make 0x%llx executable (prot %d)\n",
                   (unsigned long long)va, kernel_get_vmem_protection(pid, (intptr_t)va, size));
            return -1;
        }
    }
    *out_va = va;
    return 0;
}

/* ---- public interface ------------------------------------------------ */

int pt_hooks_install(pid_t pid, struct pt_stream *st)
{
    g_emit_ctx.st = st;
    g_hook_count = 0;
    g_ring = 0;
    g_ring_tva = 0;
    g_pid = pid;
    g_reader_run = 0;
    size_t skipped_total = 0;

    /* module set: main executable first (M2.1 adds the loaded PRXs) */
    uint64_t bases[MAX_MODULES];
    size_t nbases = 0;
    uint64_t main_base = module_base(pid);
    if (main_base)
        bases[nbases++] = main_base;

    for (size_t i = 0; i < nbases; i++) {
        int sk = hook_module(pid, bases[i], i == 0 ? g_main_dyn : 0);
        if (sk < 0) {
            pt_hooks_remove(pid, 1);
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

    for (int i = 0; i < g_hook_count; i++)
        if (strcmp(g_hooks[i].fn, "getpid") == 0 || strcmp(g_hooks[i].fn, "sceKernelGetpid") == 0)
            pt_syscall_hint(pid, g_hooks[i].original);

    /* the ring first: the trampolines embed its target address */
    uint64_t ring_tva = 0, code_tva = 0;
    int has_tvm = inject_remote(pid, g_ring, RING_PAGES * 0x1000, 0, &ring_tva) == 0;
    g_ring_tva = ring_tva;

    size_t emitted = 0, armed = 0;
    for (int i = 0; i < g_hook_count; i++) {
        struct hook_rec *h = &g_hooks[i];
        size_t len = 0;
        h->code = code + (size_t)i * PT_TRAMPOLINE_MAX;
        if (trampoline_emit(has_tvm ? ring_tva : (uint64_t)(uintptr_t)g_ring,
                            h->original, (uint32_t)i,
                            h->code, PT_TRAMPOLINE_MAX, &len) == 0)
            emitted++;
        else
            h->code = 0;
    }
    has_tvm = has_tvm && inject_remote(pid, code, code_pages, 1, &code_tva) == 0;
    munmap(code, code_pages);

    for (int i = 0; has_tvm && i < g_hook_count; i++) {
        struct hook_rec *h = &g_hooks[i];
        if (!h->code)
            continue;
        h->code_target = code_tva + (uint64_t)i * PT_TRAMPOLINE_MAX;
        if (pt_write(pid, h->slot_addr, &h->code_target, 8) == 0) {
            h->patched = 1;
            armed++;
            printf("  hooked %s.%s\n", h->mod, h->fn);
        }
    }

    if (armed) {
        g_reader_run = 1;
        if (pthread_create(&g_reader, 0, reader_thread, &g_emit_ctx) != 0)
            g_reader_run = 0;
    }

    printf("prosperotrace: %d/%d slots emitted, %lu armed, %zu filtered%s\n",
           (int)emitted, g_hook_count, armed, skipped_total,
           has_tvm ? "" : " (could not map pages in the target)");
    return 0;
}

int pt_hooks_remove(pid_t pid, int restore)
{
    if (g_reader_run) {
        g_reader_run = 0;
        pthread_join(g_reader, 0);
    }
    int rc = 0;
    for (int i = 0; i < g_hook_count; i++) {
        if (g_hooks[i].patched && restore) {
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