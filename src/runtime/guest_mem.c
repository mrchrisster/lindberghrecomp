/*
 * guest_mem.c - the guest's 4 GB address space on a 64-bit host.
 *
 * A Lindbergh game is a 32-bit process: every pointer it holds, stores or
 * compares is 32 bits wide. On a 32-bit host that is simply a host pointer. On
 * a 64-bit host (macOS, the Quest) there is no address 0x08048000 to map the
 * image at, so instead one 4 GB window is reserved and a guest address is an
 * offset into it - cpu.h's G2H() adds the base on every access.
 *
 * Everything the game can hold a pointer to has to live inside that window:
 * its image, its stacks, its heap, and any host object a library call hands
 * back to it. guest_alloc() is the heap for all of those, and guest_addr_of()
 * is the check at the boundary - a host pointer that is NOT in the window has
 * no 32-bit name, and truncating it silently is exactly the kind of fault that
 * shows up as a crash somewhere unrelated much later.
 *
 * Layout, chosen to match where Linux/i386 puts things:
 *   0x00000000..0x0000FFFF   never mapped: a null dereference faults
 *   0x08048000..             the ELF image, then its brk heap
 *   0x20000000..0xB0000000   guest_alloc() heap (dlmalloc mspace, 2.25 GB);
 *                            in deterministic mode (netplay.h) the game's own
 *                            malloc/new get 0x20000000..0x98000000 to
 *                            themselves, a heap per thread (guest_alloc_game),
 *                            and the runtime's allocations take the rest, so
 *                            the game's heap layout depends on the game alone
 *   0xBF7F0000..0xBFFF0000   the main thread's stack (guest.c)
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#ifdef _WIN32
#include <windows.h>
#else
#include <sys/mman.h>
#endif

#include "lindbergh_rt.h"
#include "netplay.h"

#ifdef _WIN32
#define FORCEINLINE inline
#endif
#define ONLY_MSPACES 1
#define HAVE_MMAP 0
#define HAVE_MORECORE 0
#define USE_LOCKS 1
#define USE_DL_PREFIX 1
#define MALLOC_FAILURE_ACTION
#define FOOTERS 0
#include "../../third_party/dlmalloc/malloc.c"

#define WINDOW      (1ull << 32)
#define GUARD       0x10000u          /* overrun past 0xFFFFFFFF lands here */
#define HEAP_LO     0x20000000u
#define HEAP_HI     0xB0000000u

uint8_t *g_guest_base;
static mspace g_heap;
/* Deterministic mode: the game's malloc alone, and one heap per guest thread -
 * the loading threads allocate while the game's main thread does, and in one
 * shared heap the order (and so every address) changed from run to run. Each
 * thread's own sequence of requests is the same every run, so its heap is.
 * Heap 0 is the main thread's, 1..15 the threads the game creates in that
 * order, the last one the rest (host threads calling in) and any overflow.
 * A job pool hands its jobs to whichever worker is free, so the game's pool
 * gets one worker in deterministic mode (hle_sega.c: VT3's nWorkerThreads). */
#define GAME_HEAP_HI 0x98000000u        /* the runtime's heap above: 384 MB */
#define GH_N         17
#define GH_OTHER     16
/* the game's main thread (the first to allocate) 256 MB; the first four
 * threads it makes (its job pool's workers and the loaders: one of them reads
 * every asset) 352 MB each; the next eleven (the sound and movie library's)
 * 8 MB each; the rest 168 MB */
static uint32_t gh_lo(int i)
{
    return i == 0 ? HEAP_LO : i <= 4 ? 0x30000000u + (uint32_t)(i - 1) * 0x16000000u
         : i < GH_OTHER ? 0x88000000u + (uint32_t)(i - 5) * 0x00800000u : 0x8D800000u;
}
static uint32_t gh_hi(int i)
{
    return i == 0 ? 0x30000000u : i <= 4 ? gh_lo(i) + 0x16000000u : i < GH_OTHER ? gh_lo(i) + 0x00800000u : GAME_HEAP_HI;
}
static mspace g_gh[GH_N];
static mspace g_game_heap;              /* set when the heaps are split: the main thread's */
static int g_zero_fill;
static __thread int t_heap = -1;
static int gh_of(const void *p)
{
    uint32_t a = (uint32_t)((const uint8_t *)p - g_guest_base);
    for (int i = 0; i < GH_N; i++) if (a >= gh_lo(i) && a < gh_hi(i)) return i;
    return -1;
}
void guest_heap_thread(int n) { t_heap = n > 0 && n < GH_OTHER ? n : GH_OTHER; }

#ifdef _WIN32
/* Reserve without charging 4 GB against the Windows commit limit. Commit a
 * 64 KB block on its first access, like mmap(MAP_NORESERVE) on POSIX. Only
 * faults in our reservation are handled; the guest null guard still faults.
 * VirtualAlloc is safe when two guest threads first touch the same block. */
static LONG CALLBACK guest_commit_fault(EXCEPTION_POINTERS *e)
{
    EXCEPTION_RECORD *r = e->ExceptionRecord;
    if (r->ExceptionCode != EXCEPTION_ACCESS_VIOLATION || r->NumberParameters < 2 ||
        r->ExceptionInformation[0] == 8 || !g_guest_base)
        return EXCEPTION_CONTINUE_SEARCH;
    uintptr_t addr = r->ExceptionInformation[1], base = (uintptr_t)g_guest_base;
    if (addr < base + GUARD || addr >= base + WINDOW + GUARD)
        return EXCEPTION_CONTINUE_SEARCH;
    void *page = (void *)(addr & ~(uintptr_t)(GUARD - 1));
    if (!VirtualAlloc(page, GUARD, MEM_COMMIT, PAGE_READWRITE))
        return EXCEPTION_CONTINUE_SEARCH;
    return EXCEPTION_CONTINUE_EXECUTION;
}
#endif

int guest_mem_init(void)
{
    if (g_guest_base) return 0;
    /* One reservation for the whole window plus a guard, so a 4-byte access at
     * 0xFFFFFFFE stays inside memory we own. MAP_NORESERVE: pages cost nothing
     * until the guest touches them. */
#ifdef _WIN32
    void *p = VirtualAlloc(NULL, WINDOW + GUARD, MEM_RESERVE, PAGE_READWRITE);
    if (!p) {
        fprintf(stderr, "[mem] reserving guest window failed: %lu\n", GetLastError());
        return -1;
    }
    g_guest_base = p;
    if (!AddVectoredExceptionHandler(1, guest_commit_fault)) {
        VirtualFree(p, 0, MEM_RELEASE);
        g_guest_base = NULL;
        return -1;
    }
#else
    void *p = mmap(NULL, WINDOW + GUARD, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    if (p == MAP_FAILED) {
        perror("[mem] reserving the 4 GB guest window");
        return -1;
    }
    g_guest_base = (uint8_t *)p;
    mprotect(p, GUARD, PROT_NONE);    /* guest null page */
#endif

    if (lg_deterministic()) {
        for (int i = 0; i < GH_N; i++) g_gh[i] = create_mspace_with_base(g_guest_base + gh_lo(i), gh_hi(i) - gh_lo(i), 1);
        g_game_heap = g_gh[0];
        g_heap = create_mspace_with_base(g_guest_base + GAME_HEAP_HI, HEAP_HI - GAME_HEAP_HI, 1);
        g_zero_fill = 1;
        if (g_game_heap) fprintf(stderr, "[mem] deterministic: the game's heaps %#x..%#x (one per thread, zero-filled), the runtime's %#x..%#x\n",
                                 HEAP_LO, GAME_HEAP_HI, GAME_HEAP_HI, HEAP_HI);
    } else
    g_heap = create_mspace_with_base(g_guest_base + HEAP_LO, HEAP_HI - HEAP_LO, 1);
    if (!g_heap) {
        fprintf(stderr, "[mem] cannot create the guest heap\n");
        return -1;
    }
    fprintf(stderr, "[mem] guest window at %p, heap %#x..%#x\n",
            p, HEAP_LO, HEAP_HI);
    return 0;
}

void *guest_alloc(size_t n, size_t align)
{
    void *p = align > 16 ? mspace_memalign(g_heap, align, n ? n : 1)
                         : mspace_malloc(g_heap, n ? n : 1);
#ifdef _WIN32
    /* Kernel I/O probes do not invoke the userspace fault handler. Buffers
     * handed to ReadFile/the CRT/graphics drivers must already be committed. */
    if (p && !VirtualAlloc(p, n ? n : 1, MEM_COMMIT, PAGE_READWRITE)) {
        mspace_free(g_heap, p);
        p = NULL;
    }
#endif
    if (!p) {
        fprintf(stderr, "[mem] guest heap exhausted: %zu bytes (align %zu)\n", n, align);
        fflush(stderr);
    }
    return p;
}


/* The game's own malloc / operator new (hle_libc.c). Normally guest_alloc;
 * in deterministic mode a heap of its own, zero-filled, so neither the
 * runtime's allocations nor leftovers of freed blocks change what it sees. */
void *guest_alloc_game(size_t n, size_t align)
{
    if (!g_game_heap) return guest_alloc(n, align);
    if (t_heap < 0) {       /* not a thread the game made: the first such to allocate is the game's main thread */
        static int main_taken;
        t_heap = __atomic_exchange_n(&main_taken, 1, __ATOMIC_ACQ_REL) ? GH_OTHER : 0;
    }
    void *p = NULL;
    for (int i = t_heap; ; i = GH_OTHER) {
        p = align > 16 ? mspace_memalign(g_gh[i], align, n ? n : 1) : mspace_malloc(g_gh[i], n ? n : 1);
        if (p || i == GH_OTHER) break;
        static int told[GH_N];
        if (!told[i]++) fprintf(stderr, "[mem] deterministic: thread heap %d is full (%zu bytes asked) - the shared one from here (addresses may differ between runs)\n", i, n);
    }
    if (!p) {
        fprintf(stderr, "[mem] the game's heap is exhausted: %zu bytes (align %zu); in use per heap (MB):", n, align);
        for (int i = 0; i < GH_N; i++) { struct mallinfo m = mspace_mallinfo(g_gh[i]); fprintf(stderr, " %d:%zu/%u", i, (size_t)m.uordblks >> 20, (gh_hi(i) - gh_lo(i)) >> 20); }
        fprintf(stderr, "\n"); fflush(stderr); return NULL;
    }
    if (g_zero_fill) memset(p, 0, n);
    return p;
}

void guest_free(void *p)
{
    if (!p) return;
    int i = g_game_heap ? gh_of(p) : -1;
    if (i >= 0) mspace_free(g_gh[i], p);
    else mspace_free(g_heap, p);
}

size_t guest_alloc_size(void *p)
{
    return p ? mspace_usable_size(p) : 0;
}

uint32_t guest_addr_of(const void *p, const char *file, int line)
{
    if (!p) return 0;
    uintptr_t off = (uintptr_t)((const uint8_t *)p - g_guest_base);
    if ((const uint8_t *)p < g_guest_base || off >= WINDOW) {
        fprintf(stderr,
                "[mem] %s:%d handed the guest a host pointer %p outside its 4 GB "
                "window (base %p). It has no 32-bit address; this is a port bug.\n",
                file, line, p, (void *)g_guest_base);
        fflush(stderr);
        guest_report_state("host pointer escaped into the guest");
        abort();
    }
    return (uint32_t)off;
}

/* Interned copies of host strings, for calls whose result the caller treats as
 * static storage. A small linear table: these are a few dozen distinct
 * strings (error texts, GL_VENDOR, locale names), not a hot path. */
static pthread_mutex_t g_str_lock = PTHREAD_MUTEX_INITIALIZER;
static struct { char *host; uint32_t guest; } g_strs[512];
static int g_nstrs;

uint32_t guest_strdup_static(const char *s)
{
    if (!s) return 0;
    pthread_mutex_lock(&g_str_lock);
    for (int i = 0; i < g_nstrs; i++)
        if (strcmp(g_strs[i].host, s) == 0) {
            uint32_t g = g_strs[i].guest;
            pthread_mutex_unlock(&g_str_lock);
            return g;
        }
    size_t n = strlen(s) + 1;
    char *copy = (char *)guest_alloc(n, 16);
    memcpy(copy, s, n);
    uint32_t g = H2G(copy);
    if (g_nstrs < (int)(sizeof g_strs / sizeof g_strs[0])) {
        g_strs[g_nstrs].host = copy;
        g_strs[g_nstrs].guest = g;
        g_nstrs++;
    }
    pthread_mutex_unlock(&g_str_lock);
    return g;
}

/* ---- FILE handles ----
 * A guest FILE* is a 16-byte guest block holding the host FILE*. Freed on
 * fclose; a handle that does not carry the magic is refused, not followed. */
#define FILE_MAGIC 0x46494C45u        /* "FILE" */

typedef struct { uint32_t magic, pad; FILE *f; } GuestFile;

uint32_t guest_file_wrap(FILE *f)
{
    if (!f) return 0;
    GuestFile *g = (GuestFile *)guest_alloc(sizeof *g, 16);
    g->magic = FILE_MAGIC;
    g->f = f;
    return H2G(g);
}

FILE *guest_file(uint32_t handle)
{
    if (!handle) return NULL;
    GuestFile *g = (GuestFile *)G2H(handle);
    if (g->magic != FILE_MAGIC) {
        fprintf(stderr, "[mem] %#010x is not a FILE handle\n", handle);
        return NULL;
    }
    return g->f;
}

void guest_file_release(uint32_t handle)
{
    if (!handle) return;
    GuestFile *g = (GuestFile *)G2H(handle);
    if (g->magic == FILE_MAGIC) { g->magic = 0; guest_free(g); }
}
