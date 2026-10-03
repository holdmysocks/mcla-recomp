/* PS5 go/no-go probe 1: can a homebrew process build the guest memory layout
 * the ReXGlue runtime needs?
 *
 *   A. a 0x120000000-byte shared memory object (4.5 GiB)
 *   B. a 4.5 GiB address-space reservation, and views of the shared object
 *      mapped inside it, with two views aliasing the same pages (the Xbox 360
 *      physical-memory mirrors)
 *   C. mprotect + SIGSEGV with a usable ucontext, resuming after the handler
 *      (GPU write-watches)
 *   D. timer resolution and core count
 *
 * Allocates and frees memory inside its own process only. Touches no files.
 * Always exits: explicit cleanup at the end, a 30 s watchdog, and a fault-loop
 * limit. Process exit releases anything left (SHM_ANON has no name to leak).
 *
 * v1 forced the reservation to 0x100000000 with MAP_FIXED|MAP_EXCL and the
 * process died without output, so MAP_EXCL is not to be trusted here. v2 never
 * uses MAP_FIXED outside a range it has reserved itself.
 */
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/types.h>
#include <time.h>
#include <ucontext.h>
#include <unistd.h>

#define ARENA_SIZE 0x120000000ull

static volatile int g_faults;
static volatile uintptr_t g_fault_addr;
static volatile uintptr_t g_fault_rip;
static void *g_watch_page;
static size_t g_page;

static void on_fault(int sig, siginfo_t *info, void *ctx) {
    ucontext_t *uc = (ucontext_t *)ctx;
    g_fault_addr = (uintptr_t)info->si_addr;
    g_fault_rip = (uintptr_t)uc->uc_mcontext.mc_rip;
    /* Never spin: a fault we cannot clear ends the process, which frees everything. */
    if (++g_faults > 5000) _exit(3);
    mprotect(g_watch_page, g_page, PROT_READ | PROT_WRITE);
    /* Also unprotect the page that actually faulted, in case it is an alias. */
    mprotect((void *)(g_fault_addr & ~(uintptr_t)(g_page - 1)), g_page, PROT_READ | PROT_WRITE);
}

static void result(const char *name, int ok, const char *detail) {
    printf("%-34s %s  %s\n", name, ok ? "PASS" : "FAIL", detail);
}

/* Does the kernel honour an address hint (no MAP_FIXED) for a full-size reservation? */
static void try_hint(uintptr_t hint) {
    char name[48], d[96];
    void *p = mmap((void *)hint, ARENA_SIZE, PROT_NONE, MAP_PRIVATE | MAP_ANON, -1, 0);
    snprintf(name, sizeof name, "B0 hint 0x%llx", (unsigned long long)hint);
    snprintf(d, sizeof d, "got=%p errno=%d", p, p == MAP_FAILED ? errno : 0);
    result(name, p == (void *)hint, d);
    if (p != MAP_FAILED) munmap(p, ARENA_SIZE);
}

int main(void) {
    char d[160];
    int stack_var = 0;
    alarm(30); /* watchdog: default SIGALRM action terminates the process */
    setvbuf(stdout, NULL, _IONBF, 0);
    g_page = (size_t)sysconf(_SC_PAGESIZE);
    printf("memprobe v3: page=%zu cores=%ld pid=%d\n", g_page, sysconf(_SC_NPROCESSORS_ONLN), getpid());
    void *heap_var = malloc(64);
    printf("layout: code=%p stack=%p heap=%p\n", (void *)main, (void *)&stack_var, heap_var);
    free(heap_var);

    /* A. shared object */
    int fd = shm_open(SHM_ANON, O_RDWR | O_CREAT, 0600);
    snprintf(d, sizeof d, "fd=%d errno=%d (%s)", fd, fd < 0 ? errno : 0, fd < 0 ? strerror(errno) : "");
    result("A1 shm_open(SHM_ANON)", fd >= 0, d);
    int tr = fd >= 0 ? ftruncate(fd, (off_t)ARENA_SIZE) : -1;
    snprintf(d, sizeof d, "size=0x%llx errno=%d (%s)", ARENA_SIZE, tr ? errno : 0, tr ? strerror(errno) : "");
    result("A2 ftruncate 4.5 GiB", tr == 0, d);

    /* B. address space */
    try_hint(0x100000000ull);
    try_hint(0x1000000000ull);
    try_hint(0x4000000000ull);

    /* How much anonymous PROT_NONE can be reserved at all? (informational) */
    for (size_t gib = 4; gib >= 1; gib /= 2) {
        void *p = mmap(NULL, gib << 30, PROT_NONE, MAP_PRIVATE | MAP_ANON, -1, 0);
        snprintf(d, sizeof d, "%zu GiB anon PROT_NONE: got=%p errno=%d", gib, p, p == MAP_FAILED ? errno : 0);
        result("B1a anon reservation", p != MAP_FAILED, d);
        if (p != MAP_FAILED) munmap(p, gib << 30);
    }

    /* Reservation method 1: FreeBSD guard mapping (address space only). */
    uint8_t *guard = mmap(NULL, ARENA_SIZE, PROT_NONE, MAP_GUARD, -1, 0);
    snprintf(d, sizeof d, "at=%p errno=%d", (void *)guard, guard == MAP_FAILED ? errno : 0);
    result("B1b reserve 4.5 GiB MAP_GUARD", guard != MAP_FAILED, d);
    if (guard != MAP_FAILED && fd >= 0 && tr == 0) {
        uint8_t *gv = mmap(guard + 0x10000000, 0x100000, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_FIXED, fd, 0);
        snprintf(d, sizeof d, "view=%p errno=%d", (void *)gv, gv == MAP_FAILED ? errno : 0);
        result("B1c shared view over guard", gv != MAP_FAILED, d);
    }
    if (guard != MAP_FAILED) munmap(guard, ARENA_SIZE);

    /* Reservation method 2: the whole shared object mapped PROT_NONE. */
    uint8_t *base = MAP_FAILED;
    if (fd >= 0 && tr == 0) base = mmap(NULL, ARENA_SIZE, PROT_NONE, MAP_SHARED, fd, 0);
    snprintf(d, sizeof d, "at=%p errno=%d (%s)", (void *)base, base == MAP_FAILED ? errno : 0,
             base == MAP_FAILED ? strerror(errno) : "");
    result("B1d reserve 4.5 GiB via shm map", base != MAP_FAILED, d);

    if (fd >= 0 && tr == 0 && base != MAP_FAILED) {
        /* Guest 0x00000000 view and the 0xA0000000 physical mirror both map file offset 0. */
        size_t len = 0x20000000;
        printf("B2 mapping views...\n");
        uint8_t *v0 = mmap(base, len, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_FIXED, fd, 0);
        uint8_t *v1 = mmap(base + 0xA0000000ull, len, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_FIXED, fd, 0);
        snprintf(d, sizeof d, "v0=%p v1=%p errno=%d", (void *)v0, (void *)v1, errno);
        int mapped = v0 != MAP_FAILED && v1 != MAP_FAILED;
        result("B2 two fixed MAP_SHARED views", mapped, d);
        if (mapped) {
            v0[0x1234567] = 0x5A;
            int alias = v1[0x1234567] == 0x5A;
            result("B3 views alias same pages", alias, "");

            /* Touch 512 MiB to see whether that much can actually be committed. */
            printf("B4 touching 512 MiB...\n");
            size_t touched = 0;
            for (size_t off = 0; off < len; off += g_page) { v0[off] = 1; touched += g_page; }
            snprintf(d, sizeof d, "touched=%zu MiB", touched >> 20);
            result("B4 commit 512 MiB", touched == len, d);

            /* C. write watch */
            struct sigaction sa;
            memset(&sa, 0, sizeof sa);
            sa.sa_sigaction = on_fault;
            sa.sa_flags = SA_SIGINFO;
            sigemptyset(&sa.sa_mask);
            int s1 = sigaction(SIGSEGV, &sa, NULL);
            int s2 = sigaction(SIGBUS, &sa, NULL);
            snprintf(d, sizeof d, "segv=%d bus=%d errno=%d", s1, s2, errno);
            result("C1 sigaction SIGSEGV/SIGBUS", s1 == 0 && s2 == 0, d);

            g_watch_page = v0 + 0x400000;
            int mp = mprotect(g_watch_page, g_page, PROT_READ);
            snprintf(d, sizeof d, "errno=%d (%s)", mp ? errno : 0, mp ? strerror(errno) : "");
            result("C2 mprotect read-only", mp == 0, d);
            if (mp == 0 && s1 == 0) {
                printf("C3 writing to protected page...\n");
                ((volatile uint8_t *)g_watch_page)[16] = 7;
                snprintf(d, sizeof d, "faults=%d addr=%p rip=%p", g_faults, (void *)g_fault_addr,
                         (void *)g_fault_rip);
                result("C3 fault caught and resumed",
                       g_faults == 1 && g_fault_addr == (uintptr_t)g_watch_page + 16 && g_fault_rip != 0, d);

                /* Per-view protection: is the alias still writable while v0 is read-only? */
                mprotect(g_watch_page, g_page, PROT_READ);
                int before = g_faults;
                v1[0x400000 + 32] = 9;
                snprintf(d, sizeof d, "faults during alias write=%d", g_faults - before);
                result("C4 protection is per view", g_faults == before, d);
                mprotect(g_watch_page, g_page, PROT_READ | PROT_WRITE);

                /* Write-watch cost: mprotect + fault + mprotect round trips. */
                struct timespec t0, t1;
                clock_gettime(CLOCK_MONOTONIC, &t0);
                for (int i = 0; i < 2000; i++) {
                    mprotect(g_watch_page, g_page, PROT_READ);
                    ((volatile uint8_t *)g_watch_page)[0] = (uint8_t)i;
                }
                clock_gettime(CLOCK_MONOTONIC, &t1);
                double us = ((t1.tv_sec - t0.tv_sec) * 1e9 + (t1.tv_nsec - t0.tv_nsec)) / 2000.0 / 1000.0;
                snprintf(d, sizeof d, "%.2f us per protect+fault round trip", us);
                result("C5 write-watch cost", 1, d);
            }
        }
    }

    /* PROT_EXEC is not needed (code is static), but record whether RWX anon works. */
    void *rwx = mmap(NULL, g_page, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANON, -1, 0);
    snprintf(d, sizeof d, "errno=%d (informational)", rwx == MAP_FAILED ? errno : 0);
    result("X1 anonymous RWX page", rwx != MAP_FAILED, d);

    /* D. timers */
    struct timespec r, a, b, req = {0, 1000000};
    clock_getres(CLOCK_MONOTONIC, &r);
    clock_gettime(CLOCK_MONOTONIC, &a);
    for (int i = 0; i < 100; i++) nanosleep(&req, NULL);
    clock_gettime(CLOCK_MONOTONIC, &b);
    double ms = ((b.tv_sec - a.tv_sec) * 1e9 + (b.tv_nsec - a.tv_nsec)) / 100.0 / 1e6;
    snprintf(d, sizeof d, "clock res=%ld ns, nanosleep(1ms) averages %.3f ms", r.tv_nsec, ms);
    result("D1 timers", ms < 2.5, d);

    /* Cleanup: drop handlers, unmap everything, close the shared object. */
    signal(SIGSEGV, SIG_DFL);
    signal(SIGBUS, SIG_DFL);
    if (rwx != MAP_FAILED) munmap(rwx, g_page);
    if (base != MAP_FAILED) munmap(base, ARENA_SIZE);
    if (fd >= 0) close(fd);
    alarm(0);
    printf("memprobe: done, cleaned up\n");
    return 0;
}
