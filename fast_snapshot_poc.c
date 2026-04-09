/*
 * fast_snapshot_poc.c
 *
 * Simulates demand-paging snapshot restore using userfaultfd.
 * Generates a mapped-ram-style snapshot file, then compares
 * eager (read-all-first) vs demand (fault-driven) loading.
 *
 * Usage:
 *   sudo ./fast_snapshot_poc eager  [size_mb]
 *   sudo ./fast_snapshot_poc demand [size_mb]
 *   sudo ./fast_snapshot_poc both   [size_mb]
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <time.h>
#include <pthread.h>
#include <poll.h>
#include <sys/mman.h>
#include <sys/ioctl.h>
#include <sys/syscall.h>
#include <sys/eventfd.h>
#include <linux/userfaultfd.h>

#ifndef UFFD_USER_MODE_ONLY
#define UFFD_USER_MODE_ONLY 1
#endif

#define PAGE_SIZE        4096
#define SNAPSHOT_FILE    "/tmp/fast_snapshot_poc.dat"
#define ZERO_RATIO       30

/*
 * Simplified mapped-ram file layout:
 *
 *   [MappedRamHeader]
 *   [bitmap at bitmap_offset: 1 bit per page, 1=data 0=zero]
 *   [page data at pages_offset: page N at pages_offset + N * page_size]
 */
struct MappedRamHeader {
    uint64_t page_size;
    uint64_t num_pages;
    uint64_t bitmap_offset;
    uint64_t pages_offset;
};

static int          snap_fd;
static int          uffd;
static int          quit_efd;
static void        *region;
static uint64_t     num_pages;
static uint8_t     *file_bmap;
static uint64_t    *loaded_bmap;
static uint64_t     g_pages_offset;

static _Atomic long stat_faults;
static _Atomic long stat_prefetch;
static _Atomic long stat_done;

static double now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000.0 + ts.tv_nsec / 1e6;
}

static void die(const char *msg) { perror(msg); exit(1); }

static inline int is_data_page(uint64_t pg)
{
    return file_bmap[pg / 8] & (1u << (pg % 8));
}

/* ---- snapshot generation ---- */

static void create_snapshot(uint64_t np)
{
    num_pages = np;
    uint64_t bmap_bytes = (np + 7) / 8;

    struct MappedRamHeader hdr = {
        .page_size     = PAGE_SIZE,
        .num_pages     = np,
        .bitmap_offset = sizeof(hdr),
        .pages_offset  = (sizeof(hdr) + bmap_bytes + PAGE_SIZE - 1)
                         & ~((uint64_t)PAGE_SIZE - 1),
    };

    int fd = open(SNAPSHOT_FILE, O_CREAT | O_RDWR | O_TRUNC, 0644);
    if (fd < 0) die("create snapshot");
    if (write(fd, &hdr, sizeof(hdr)) < 0) die("write header");

    uint8_t *bmap = calloc(1, bmap_bytes);
    srand(42);
    uint64_t data_n = 0;
    for (uint64_t i = 0; i < np; i++) {
        if (rand() % 100 >= ZERO_RATIO) {
            bmap[i / 8] |= (1u << (i % 8));
            data_n++;
        }
    }
    if (pwrite(fd, bmap, bmap_bytes, hdr.bitmap_offset) < 0)
        die("write bitmap");

    char page[PAGE_SIZE];
    for (uint64_t i = 0; i < np; i++) {
        memset(page, 0, PAGE_SIZE);
        if (bmap[i / 8] & (1u << (i % 8)))
            snprintf(page, PAGE_SIZE, "PAGE %05u", (unsigned)(i & 0xFFFFFFFF));
        if (pwrite(fd, page, PAGE_SIZE, hdr.pages_offset + i * PAGE_SIZE) < 0)
            die("write page");
    }

    free(bmap);
    close(fd);

    printf("Snapshot: %lu MB  (%lu data pages, %lu zero pages, %d%% zero)\n",
           (unsigned long)(np * PAGE_SIZE / (1024 * 1024)),
           (unsigned long)data_n,
           (unsigned long)(np - data_n), ZERO_RATIO);
}

/* ---- eager load (baseline) ---- */

static double eager_load(void)
{
    double t0 = now_ms();

    struct MappedRamHeader hdr;
    snap_fd = open(SNAPSHOT_FILE, O_RDONLY);
    if (snap_fd < 0) die("open");
    if (pread(snap_fd, &hdr, sizeof(hdr), 0) != sizeof(hdr)) die("read hdr");

    num_pages      = hdr.num_pages;
    g_pages_offset = hdr.pages_offset;
    uint64_t bsz   = (num_pages + 7) / 8;
    file_bmap      = malloc(bsz);
    if (pread(snap_fd, file_bmap, bsz, hdr.bitmap_offset) != (ssize_t)bsz)
        die("read bitmap");

    region = mmap(NULL, num_pages * PAGE_SIZE,
                  PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (region == MAP_FAILED) die("mmap");

    char *ram = region;
    for (uint64_t i = 0; i < num_pages; i++) {
        if (is_data_page(i))
            if (pread(snap_fd, ram + i * PAGE_SIZE, PAGE_SIZE,
                      g_pages_offset + i * PAGE_SIZE) != PAGE_SIZE)
                die("eager pread");
    }

    double ttfi = now_ms() - t0;

    int err = 0;
    for (uint64_t i = 0; i < num_pages; i += (num_pages / 200 + 1)) {
        char exp[32];
        if (is_data_page(i)) {
            snprintf(exp, sizeof(exp), "PAGE %05u", (unsigned)(i & 0xFFFFFFFF));
            if (strncmp(ram + i * PAGE_SIZE, exp, 10)) err++;
        } else {
            if (ram[i * PAGE_SIZE] != 0) err++;
        }
    }

    printf("\n--- Eager Load ---\n");
    printf("TTFI:       %8.1f ms  (VM blocked until ALL RAM loaded)\n", ttfi);
    printf("Verify:     %s\n", err ? "ERRORS" : "OK");

    munmap(region, num_pages * PAGE_SIZE);
    free(file_bmap);
    close(snap_fd);
    return ttfi;
}

/* ---- demand paging helpers ---- */

/*
 * Atomic claim: returns 1 if this thread won the page,
 * 0 if another thread already got it.
 */
static inline int claim_page(uint64_t pg)
{
    uint64_t idx  = pg / 64;
    uint64_t mask = 1UL << (pg % 64);
    uint64_t old  = __atomic_fetch_or(&loaded_bmap[idx], mask, __ATOMIC_ACQ_REL);
    return !(old & mask);
}

/*
 * Read page from snapshot into tmp buf, then UFFDIO_COPY to install it.
 * Zero pages skip the pread -- just copy a zeroed buffer.
 */
static int install_page(uint64_t pg, char *tmp)
{
    if (is_data_page(pg)) {
        if (pread(snap_fd, tmp, PAGE_SIZE,
                  g_pages_offset + pg * PAGE_SIZE) != PAGE_SIZE)
            return -1;
    } else {
        memset(tmp, 0, PAGE_SIZE);
    }

    struct uffdio_copy uc = {
        .dst  = (unsigned long)region + pg * PAGE_SIZE,
        .src  = (unsigned long)tmp,
        .len  = PAGE_SIZE,
        .mode = 0,
    };
    if (ioctl(uffd, UFFDIO_COPY, &uc) < 0) {
        if (errno == EEXIST) return 0;
        return -1;
    }
    return 1;
}

/* ---- fault handler thread ---- */

static void *fault_handler_thread(void *arg)
{
    (void)arg;
    char tmp[PAGE_SIZE] __attribute__((aligned(PAGE_SIZE)));
    struct pollfd pfd[2] = {
        { .fd = uffd,     .events = POLLIN },
        { .fd = quit_efd, .events = POLLIN },
    };

    for (;;) {
        if (poll(pfd, 2, -1) < 0) {
            if (errno == EINTR) continue;
            break;
        }
        if (pfd[1].revents & POLLIN)
            break;
        if (!(pfd[0].revents & POLLIN))
            continue;

        struct uffd_msg msg;
        if (read(uffd, &msg, sizeof(msg)) != sizeof(msg))
            continue;
        if (msg.event != UFFD_EVENT_PAGEFAULT)
            continue;

        uint64_t pg = (msg.arg.pagefault.address
                       - (unsigned long)region) / PAGE_SIZE;

        if (!claim_page(pg))
            continue;

        if (install_page(pg, tmp) < 0)
            abort();

        __atomic_fetch_add(&stat_faults, 1, __ATOMIC_RELAXED);
        long d = __atomic_add_fetch(&stat_done, 1, __ATOMIC_RELAXED);
        if ((uint64_t)d >= num_pages) {
            uint64_t v = 1;
            (void)write(quit_efd, &v, sizeof(v));
        }
    }
    return NULL;
}

/* ---- background prefetch thread ---- */

static void *prefetch_thread_fn(void *arg)
{
    (void)arg;
    char tmp[PAGE_SIZE] __attribute__((aligned(PAGE_SIZE)));

    for (uint64_t i = 0; i < num_pages; i++) {
        if (!claim_page(i))
            continue;

        if (install_page(i, tmp) < 0)
            continue;

        __atomic_fetch_add(&stat_prefetch, 1, __ATOMIC_RELAXED);
        long d = __atomic_add_fetch(&stat_done, 1, __ATOMIC_RELAXED);
        if ((uint64_t)d >= num_pages) {
            uint64_t v = 1;
            (void)write(quit_efd, &v, sizeof(v));
            break;
        }
    }
    return NULL;
}

/* ---- demand load ---- */

static double demand_load(void)
{
    double t0 = now_ms();

    struct MappedRamHeader hdr;
    snap_fd = open(SNAPSHOT_FILE, O_RDONLY);
    if (snap_fd < 0) die("open");
    if (pread(snap_fd, &hdr, sizeof(hdr), 0) != sizeof(hdr)) die("read hdr");

    num_pages      = hdr.num_pages;
    g_pages_offset = hdr.pages_offset;
    uint64_t bsz   = (num_pages + 7) / 8;
    file_bmap      = malloc(bsz);
    if (pread(snap_fd, file_bmap, bsz, hdr.bitmap_offset) != (ssize_t)bsz)
        die("read bitmap");

    loaded_bmap = calloc((num_pages + 63) / 64, sizeof(uint64_t));
    __atomic_store_n(&stat_faults,   0, __ATOMIC_RELAXED);
    __atomic_store_n(&stat_prefetch, 0, __ATOMIC_RELAXED);
    __atomic_store_n(&stat_done,     0, __ATOMIC_RELAXED);

    double t_parse = now_ms() - t0;

    region = mmap(NULL, num_pages * PAGE_SIZE,
                  PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (region == MAP_FAILED) die("mmap");

    uffd = (int)syscall(__NR_userfaultfd,
                        O_CLOEXEC | O_NONBLOCK | UFFD_USER_MODE_ONLY);
    if (uffd < 0) {
        uffd = (int)syscall(__NR_userfaultfd, O_CLOEXEC | O_NONBLOCK);
        if (uffd < 0) die("userfaultfd (try sudo)");
    }

    struct uffdio_api api = { .api = UFFD_API, .features = 0 };
    if (ioctl(uffd, UFFDIO_API, &api) < 0) die("UFFDIO_API");

    struct uffdio_register reg = {
        .range = { .start = (unsigned long)region,
                   .len   = num_pages * PAGE_SIZE },
        .mode  = UFFDIO_REGISTER_MODE_MISSING,
    };
    if (ioctl(uffd, UFFDIO_REGISTER, &reg) < 0) die("UFFDIO_REGISTER");

    quit_efd = eventfd(0, EFD_NONBLOCK);
    if (quit_efd < 0) die("eventfd");

    pthread_t ft;
    pthread_create(&ft, NULL, fault_handler_thread, NULL);

    pthread_t pt;
    pthread_create(&pt, NULL, prefetch_thread_fn, NULL);

    volatile char *ram = region;
    (void)ram[0];
    double ttfi = now_ms() - t0;

    int err = 0;
    for (uint64_t i = 0; i < num_pages; i++) {
        char c = ram[i * PAGE_SIZE];
        if (is_data_page(i)) {
            if (c != 'P') err++;
        } else {
            if (c != 0) err++;
        }
    }

    pthread_join(pt, NULL);
    uint64_t v = 1;
    (void)write(quit_efd, &v, sizeof(v));
    pthread_join(ft, NULL);

    double ttr = now_ms() - t0;

    long fc = __atomic_load_n(&stat_faults,   __ATOMIC_RELAXED);
    long pc = __atomic_load_n(&stat_prefetch, __ATOMIC_RELAXED);

    printf("\n--- Demand Paging (Fast Load) ---\n");
    printf("Parse:      %8.2f ms  (header + bitmap only)\n", t_parse);
    printf("TTFI:       %8.2f ms  (first page access succeeded)\n", ttfi);
    printf("Total:      %8.1f ms  (all pages loaded)\n", ttr);
    printf("Faults:     %8ld     (resolved by fault handler)\n", fc);
    printf("Prefetch:   %8ld     (pre-loaded by background thread)\n", pc);
    printf("Verify:     %s\n", err ? "ERRORS" : "OK");

    ioctl(uffd, UFFDIO_UNREGISTER, &reg);
    close(uffd);
    close(quit_efd);
    munmap(region, num_pages * PAGE_SIZE);
    free(file_bmap);
    free(loaded_bmap);
    close(snap_fd);
    return ttfi;
}

int main(int argc, char *argv[])
{
    if (argc < 2) {
        fprintf(stderr,
            "Usage: %s <eager|demand|both> [size_mb]\n\n"
            "  eager  -- load all RAM, then start VM  (baseline)\n"
            "  demand -- demand-paging via userfaultfd (fast load)\n"
            "  both   -- run eager then demand, show comparison\n"
            "  default size: 128 MB\n", argv[0]);
        return 1;
    }

    int mb = argc >= 3 ? atoi(argv[2]) : 128;
    uint64_t np = (uint64_t)mb * 1024 * 1024 / PAGE_SIZE;

    printf("=== Fast Snapshot Load POC ===\n");
    create_snapshot(np);

    if (strcmp(argv[1], "eager") == 0) {
        eager_load();
    } else if (strcmp(argv[1], "demand") == 0) {
        demand_load();
    } else if (strcmp(argv[1], "both") == 0) {
        double e = eager_load();
        double d = demand_load();
        printf("\n--- Comparison ---\n");
        printf("Eager  TTFI: %8.2f ms\n", e);
        printf("Demand TTFI: %8.2f ms\n", d);
        printf("Speedup:     %8.0fx\n", e / d);
    } else {
        fprintf(stderr, "Unknown mode: %s\n", argv[1]);
        return 1;
    }

    unlink(SNAPSHOT_FILE);
    return 0;
}
