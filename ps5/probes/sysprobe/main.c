/* PS5 probe 2: three questions left open by memprobe.
 *
 *   A. Where are the faulting instruction pointer and registers inside the
 *      signal context? memprobe read uc_mcontext.mc_rip and got a stack
 *      address, so the SDK's ucontext layout cannot be trusted. Marker values
 *      are loaded into callee-saved registers before a deliberate fault, and
 *      the handler scans the context for them and for a code-range pointer.
 *   B. How much memory can the process commit? Shared memory is touched in
 *      256 MiB steps up to 2 GiB (the PC build uses about 1.5 GiB).
 *   C. Can it run 100 threads (the PC build runs about 80)?
 *
 * Allocates and frees memory inside its own process only. Touches no files.
 * Always exits: explicit cleanup at the end, a 60 s watchdog, and a fault
 * limit. If the kernel ends the process during B, the last printed line shows
 * how far it got and process exit releases everything.
 */
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/types.h>
#include <time.h>
#include <ucontext.h>
#include <unistd.h>

#define M12 0x1212121212121212ull
#define M13 0x1313131313131313ull
#define M14 0x1414141414141414ull
#define M15 0x1515151515151515ull
#define MBX 0x0b0b0b0b0b0b0b0bull

#define CTX_WORDS 160

static size_t g_page;
static uint8_t *g_watch;
static volatile int g_faults;
static uint64_t g_ctx[CTX_WORDS];
static uint64_t g_info_addr;
static size_t g_ctx_size;

static void on_fault(int sig, siginfo_t *info, void *ctx) {
    if (++g_faults > 100) _exit(3);
    if (g_faults == 1) {
        memcpy(g_ctx, ctx, sizeof g_ctx);
        g_info_addr = (uint64_t)(uintptr_t)info->si_addr;
        g_ctx_size = sizeof(ucontext_t);
    }
    mprotect(g_watch, g_page, PROT_READ | PROT_WRITE);
}

/* Load markers into callee-saved registers, then write to the protected page. */
__attribute__((noinline)) static void fault_with_markers(volatile uint8_t *p) {
    __asm__ volatile(
        "movabs $0x1212121212121212, %%r12\n"
        "movabs $0x1313131313131313, %%r13\n"
        "movabs $0x1414141414141414, %%r14\n"
        "movabs $0x1515151515151515, %%r15\n"
        "movabs $0x0b0b0b0b0b0b0b0b, %%rbx\n"
        "movb $7, (%%rdi)\n"
        : : "D"(p) : "r12", "r13", "r14", "r15", "rbx", "memory");
}

static void *idle_thread(void *arg) {
    volatile int *stop = arg;
    while (!*stop) usleep(2000);
    return NULL;
}

static void find(const char *name, uint64_t value) {
    int found = 0;
    for (int i = 0; i < CTX_WORDS; i++) {
        if (g_ctx[i] == value) {
            printf("  %-8s at ucontext offset 0x%03x\n", name, i * 8);
            found = 1;
        }
    }
    if (!found) printf("  %-8s NOT FOUND\n", name);
}

int main(void) {
    alarm(60);
    setvbuf(stdout, NULL, _IONBF, 0);
    g_page = (size_t)sysconf(_SC_PAGESIZE);
    printf("sysprobe: page=%zu pid=%d code=%p\n", g_page, getpid(), (void *)main);

    /* A. signal context layout */
    g_watch = mmap(NULL, g_page, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    if (g_watch != MAP_FAILED) {
        struct sigaction sa;
        memset(&sa, 0, sizeof sa);
        sa.sa_sigaction = on_fault;
        sa.sa_flags = SA_SIGINFO;
        sigemptyset(&sa.sa_mask);
        sigaction(SIGSEGV, &sa, NULL);
        sigaction(SIGBUS, &sa, NULL);
        mprotect(g_watch, g_page, PROT_READ);
        fault_with_markers(g_watch);
        printf("A context: faults=%d si_addr=%p (expected %p) sizeof(ucontext_t)=%zu\n", g_faults,
               (void *)(uintptr_t)g_info_addr, (void *)g_watch, g_ctx_size);
        printf("  SDK header offsets: mc_rip=0x%zx mc_rsp=0x%zx mc_r12=0x%zx mc_rdi=0x%zx\n",
               offsetof(ucontext_t, uc_mcontext.mc_rip), offsetof(ucontext_t, uc_mcontext.mc_rsp),
               offsetof(ucontext_t, uc_mcontext.mc_r12), offsetof(ucontext_t, uc_mcontext.mc_rdi));
        find("r12", M12);
        find("r13", M13);
        find("r14", M14);
        find("r15", M15);
        find("rbx", MBX);
        find("rdi", (uint64_t)(uintptr_t)g_watch);
        uintptr_t lo = (uintptr_t)fault_with_markers, hi = lo + 128;
        int rip_found = 0;
        for (int i = 0; i < CTX_WORDS; i++) {
            if (g_ctx[i] >= lo && g_ctx[i] < hi) {
                printf("  rip      at ucontext offset 0x%03x (value %p, function at %p)\n", i * 8,
                       (void *)(uintptr_t)g_ctx[i], (void *)lo);
                rip_found = 1;
            }
        }
        if (!rip_found) printf("  rip      NOT FOUND in first %d bytes\n", CTX_WORDS * 8);
        signal(SIGSEGV, SIG_DFL);
        signal(SIGBUS, SIG_DFL);
        munmap(g_watch, g_page);
    }

    /* C. threads (before B, so a kill during B does not hide this result) */
    enum { NTHREADS = 100 };
    pthread_t threads[NTHREADS];
    volatile int stop = 0;
    int created = 0;
    for (; created < NTHREADS; created++) {
        if (pthread_create(&threads[created], NULL, idle_thread, (void *)&stop) != 0) break;
    }
    printf("C threads: created %d of %d (errno=%d)\n", created, NTHREADS, created < NTHREADS ? errno : 0);
    stop = 1;
    for (int i = 0; i < created; i++) pthread_join(threads[i], NULL);

    /* B. commit budget */
    const size_t step = 256ull << 20, limit = 2048ull << 20;
    int fd = shm_open(SHM_ANON, O_RDWR | O_CREAT, 0600);
    uint8_t *mem = MAP_FAILED;
    if (fd >= 0 && ftruncate(fd, (off_t)limit) == 0) {
        mem = mmap(NULL, limit, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    }
    printf("B commit: fd=%d map=%p errno=%d\n", fd, (void *)mem, mem == MAP_FAILED ? errno : 0);
    if (mem != MAP_FAILED) {
        for (size_t done = 0; done < limit; done += step) {
            for (size_t off = 0; off < step; off += g_page) mem[done + off] = 1;
            printf("B committed %zu MiB\n", (done + step) >> 20);
        }
        munmap(mem, limit);
    }
    if (fd >= 0) close(fd);

    alarm(0);
    printf("sysprobe: done, cleaned up\n");
    return 0;
}
