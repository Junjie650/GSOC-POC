/*
 * fast_snapshot_poc.c — v2
 *
 * Demand-paging snapshot restore using userfaultfd.
 * Compares three loading strategies with a mapped-ram-style snapshot file:
 *
 *   eager    — read all RAM before starting (baseline)
 *   demand   — UFFDIO_REGISTER_MODE_MISSING + UFFDIO_COPY/ZEROPAGE
 *   continue — UFFDIO_REGISTER_MODE_MINOR on memfd + UFFDIO_CONTINUE
 *
 * Usage:
 *   ./fast_snapshot_poc eager    [size_mb]
 *   ./fast_snapshot_poc demand   [size_mb]
 *   ./fast_snapshot_poc continue [size_mb]
 *   ./fast_snapshot_poc all      [size_mb]
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <time.h>
#include <limits.h>
#include <pthread.h>
#include <poll.h>
#include <sys/mman.h>
#include <sys/ioctl.h>
#include <sys/syscall.h>
#include <sys/eventfd.h>
#include <linux/userfaultfd.h>
#include <endian.h>

#ifndef UFFD_USER_MODE_ONLY
#define UFFD_USER_MODE_ONLY 1
#endif

#define PG_SIZE                 4096
#define BITS_PER_LONG           (sizeof(unsigned long) * 8)
#define MAPPED_RAM_HDR_VERSION  1
#define FILE_OFFSET_ALIGN       0x100000        /* 1 MB */
#define PREFETCH_BUF_SIZE       0x100000        /* 1 MB */
#define MAX_BLOCKS              8
#define SNAPSHOT_FILE           "/tmp/fast_snapshot_poc.dat"
#define ZERO_RATIO              30
#define HIST_BUCKETS            16

#define ROUND_UP(x, a)    (((x) + (a) - 1) & ~((uint64_t)(a) - 1))
#define BITS_TO_LONGS(n)  (((n) + BITS_PER_LONG - 1) / BITS_PER_LONG)

/* ---- mapped-ram header (mirrors migration/ram.c:3005-3024) ---- */

struct MappedRamHeader {
    uint32_t version;
    uint64_t page_size;
    uint64_t bitmap_offset;
    uint64_t pages_offset;
} __attribute__((packed));

/* ---- per-block state (mirrors RAMBlock + receivedmap) ---- */

typedef struct RAMBlockInfo {
    char     idstr[64];
    uint64_t used_length;
    void    *host;
    int      memfd;
    unsigned long *file_bmap;
    unsigned long *recv_bmap;
    uint64_t bitmap_offset;
    uint64_t pages_offset;
    uint64_t num_pages;
} RAMBlockInfo;

/* ---- central state (mirrors MigrationIncomingState) ---- */

typedef struct SnapshotLoadState SnapshotLoadState;
typedef int (*install_fn)(SnapshotLoadState *, RAMBlockInfo *,
                          uint64_t pg, const char *data);

struct SnapshotLoadState {
    int          snap_fd;
    int          uffd;
    int          quit_efd;
    RAMBlockInfo blocks[MAX_BLOCKS];
    int          num_blocks;

    _Atomic long stat_data_faults;
    _Atomic long stat_zero_faults;
    _Atomic long stat_prefetch;

    _Atomic long hist[HIST_BUCKETS];
    _Atomic long hist_total_ns;
    _Atomic long hist_min_ns;
    _Atomic long hist_max_ns;

    install_fn   install_data;
    install_fn   install_zero;
};

typedef struct { const char *idstr; uint64_t size; } BlockDef;
enum LoadMode { MODE_COPY, MODE_CONTINUE };

static char zero_buf[PG_SIZE] __attribute__((aligned(PG_SIZE)));

/* ---- utilities ---- */

static double now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000.0 + ts.tv_nsec / 1e6;
}

static void die(const char *msg) { perror(msg); exit(1); }

/* ---- bitmap helpers (mirrors kernel find_*_bit) ---- */

static inline int bmap_test(const unsigned long *bm, uint64_t pg)
{
    return (bm[pg / BITS_PER_LONG] >> (pg % BITS_PER_LONG)) & 1;
}

static inline int claim_page(RAMBlockInfo *b, uint64_t pg)
{
    unsigned long mask = 1UL << (pg % BITS_PER_LONG);
    unsigned long old = __atomic_fetch_or(&b->recv_bmap[pg / BITS_PER_LONG],
                                          mask, __ATOMIC_ACQ_REL);
    return !(old & mask);
}

static unsigned long find_first_set(const unsigned long *addr,
                                    unsigned long size)
{
    for (unsigned long i = 0; i * BITS_PER_LONG < size; i++) {
        if (addr[i]) {
            unsigned long r = i * BITS_PER_LONG + __builtin_ctzl(addr[i]);
            return r < size ? r : size;
        }
    }
    return size;
}

static unsigned long find_next_set(const unsigned long *addr,
                                   unsigned long size, unsigned long off)
{
    if (off >= size) return size;
    unsigned long idx = off / BITS_PER_LONG;
    unsigned long w = addr[idx] & ~((1UL << (off % BITS_PER_LONG)) - 1);
    if (w) {
        unsigned long r = idx * BITS_PER_LONG + __builtin_ctzl(w);
        return r < size ? r : size;
    }
    for (idx++; idx * BITS_PER_LONG < size; idx++) {
        if (addr[idx]) {
            unsigned long r = idx * BITS_PER_LONG + __builtin_ctzl(addr[idx]);
            return r < size ? r : size;
        }
    }
    return size;
}

static unsigned long find_next_clear(const unsigned long *addr,
                                     unsigned long size, unsigned long off)
{
    if (off >= size) return size;
    unsigned long idx = off / BITS_PER_LONG;
    unsigned long w = ~addr[idx] & ~((1UL << (off % BITS_PER_LONG)) - 1);
    if (w) {
        unsigned long r = idx * BITS_PER_LONG + __builtin_ctzl(w);
        return r < size ? r : size;
    }
    for (idx++; idx * BITS_PER_LONG < size; idx++) {
        if (~addr[idx]) {
            unsigned long r = idx * BITS_PER_LONG + __builtin_ctzl(~addr[idx]);
            return r < size ? r : size;
        }
    }
    return size;
}

/* ---- latency histogram (mirrors postcopy blocktime, postcopy-ram.c:1126) ---- */

static void record_latency(SnapshotLoadState *s, uint64_t ns)
{
    uint64_t us = ns / 1000;
    int bucket = 0;
    if (us >= 2)
        bucket = 63 - __builtin_clzl(us);
    if (bucket >= HIST_BUCKETS)
        bucket = HIST_BUCKETS - 1;
    __atomic_fetch_add(&s->hist[bucket], 1, __ATOMIC_RELAXED);
    __atomic_fetch_add(&s->hist_total_ns, (long)ns, __ATOMIC_RELAXED);

    long cur = __atomic_load_n(&s->hist_min_ns, __ATOMIC_RELAXED);
    while ((long)ns < cur)
        if (__atomic_compare_exchange_n(&s->hist_min_ns, &cur, (long)ns,
                                        1, __ATOMIC_RELAXED, __ATOMIC_RELAXED))
            break;
    cur = __atomic_load_n(&s->hist_max_ns, __ATOMIC_RELAXED);
    while ((long)ns > cur)
        if (__atomic_compare_exchange_n(&s->hist_max_ns, &cur, (long)ns,
                                        1, __ATOMIC_RELAXED, __ATOMIC_RELAXED))
            break;
}

static void print_histogram(SnapshotLoadState *s)
{
    long total = 0, max_c = 0;
    for (int i = 0; i < HIST_BUCKETS; i++) {
        long c = s->hist[i];
        total += c;
        if (c > max_c) max_c = c;
    }
    if (!total) { printf("  (no faults recorded)\n"); return; }

    printf("  Fault latency histogram (us):\n");
    for (int i = 0; i < HIST_BUCKETS; i++) {
        long c = s->hist[i];
        if (!c) continue;
        uint64_t lo = (i == 0) ? 0 : (1UL << i);
        int bar = max_c ? (int)(c * 32 / max_c) : 0;
        if (bar < 1 && c > 0) bar = 1;
        char b[33];
        memset(b, '#', bar);
        memset(b + bar, ' ', 32 - bar);
        b[32] = '\0';
        if (i < HIST_BUCKETS - 1)
            printf("    [%5lu,%6lu) |%s| %4ld  (%5.1f%%)\n",
                   lo, 1UL << (i + 1), b, c, 100.0 * c / total);
        else
            printf("    [%5lu,  inf) |%s| %4ld  (%5.1f%%)\n",
                   lo, b, c, 100.0 * c / total);
    }
    long mn = s->hist_min_ns, mx = s->hist_max_ns, tn = s->hist_total_ns;
    printf("    min=%.1f us  avg=%.1f us  max=%.1f us\n",
           mn / 1000.0, (double)tn / total / 1000.0, mx / 1000.0);
}

/* ---- snapshot generation ---- */

static void create_snapshot(BlockDef *defs, int ndefs)
{
    int fd = open(SNAPSHOT_FILE, O_CREAT | O_RDWR | O_TRUNC, 0644);
    if (fd < 0) die("create snapshot");

    uint32_t nb = htobe32((uint32_t)ndefs);
    pwrite(fd, &nb, 4, 0);
    uint64_t fpos = 4;

    srand(42);
    for (int i = 0; i < ndefs; i++) {
        char idstr[64] = {0};
        strncpy(idstr, defs[i].idstr, 63);
        uint64_t used_len = defs[i].size;
        uint64_t np = used_len / PG_SIZE;
        uint64_t bmap_longs = BITS_TO_LONGS(np);
        uint64_t bmap_bytes = bmap_longs * sizeof(unsigned long);

        pwrite(fd, idstr, 64, fpos);
        uint64_t used_be = htobe64(used_len);
        pwrite(fd, &used_be, 8, fpos + 64);
        fpos += 72;

        uint64_t bmap_off = fpos + sizeof(struct MappedRamHeader);
        uint64_t pages_off = ROUND_UP(bmap_off + bmap_bytes, FILE_OFFSET_ALIGN);

        struct MappedRamHeader hdr = {
            .version       = htobe32(MAPPED_RAM_HDR_VERSION),
            .page_size     = htobe64(PG_SIZE),
            .bitmap_offset = htobe64(bmap_off),
            .pages_offset  = htobe64(pages_off),
        };
        pwrite(fd, &hdr, sizeof(hdr), fpos);
        fpos += sizeof(hdr);

        unsigned long *bmap = calloc(bmap_longs, sizeof(unsigned long));
        uint64_t data_n = 0;
        for (uint64_t p = 0; p < np; p++) {
            if (rand() % 100 >= ZERO_RATIO) {
                bmap[p / BITS_PER_LONG] |= 1UL << (p % BITS_PER_LONG);
                data_n++;
            }
        }
        pwrite(fd, bmap, bmap_bytes, bmap_off);

        char page[PG_SIZE];
        for (uint64_t p = 0; p < np; p++) {
            if ((bmap[p / BITS_PER_LONG] >> (p % BITS_PER_LONG)) & 1) {
                memset(page, 0, PG_SIZE);
                snprintf(page, PG_SIZE, "[%s] PAGE %lu",
                         defs[i].idstr, (unsigned long)p);
                pwrite(fd, page, PG_SIZE, pages_off + p * PG_SIZE);
            }
        }

        ftruncate(fd, pages_off + used_len);
        fpos = pages_off + used_len;
        free(bmap);

        printf("  %-8s: %7.1f %s  (%lu data, %lu zero)\n",
               defs[i].idstr,
               used_len >= 1048576 ? used_len / 1048576.0 : used_len / 1024.0,
               used_len >= 1048576 ? "MB" : "KB",
               (unsigned long)data_n, (unsigned long)(np - data_n));
    }
    close(fd);
}

/* ---- snapshot loading: parse headers + bitmaps ---- */

static void open_snapshot(SnapshotLoadState *s)
{
    s->snap_fd = open(SNAPSHOT_FILE, O_RDONLY);
    if (s->snap_fd < 0) die("open snapshot");

    uint32_t nb;
    if (pread(s->snap_fd, &nb, 4, 0) != 4) die("read num_blocks");
    s->num_blocks = (int)be32toh(nb);

    uint64_t pos = 4;
    for (int i = 0; i < s->num_blocks; i++) {
        RAMBlockInfo *b = &s->blocks[i];
        b->memfd = -1;

        pread(s->snap_fd, b->idstr, 64, pos);
        uint64_t used_be;
        pread(s->snap_fd, &used_be, 8, pos + 64);
        b->used_length = be64toh(used_be);
        pos += 72;

        struct MappedRamHeader hdr;
        if (pread(s->snap_fd, &hdr, sizeof(hdr), pos) != sizeof(hdr))
            die("read mapped-ram header");
        if (be32toh(hdr.version) > MAPPED_RAM_HDR_VERSION) {
            fprintf(stderr, "Unsupported mapped-ram version %u\n",
                    be32toh(hdr.version));
            exit(1);
        }
        b->bitmap_offset = be64toh(hdr.bitmap_offset);
        b->pages_offset  = be64toh(hdr.pages_offset);
        b->num_pages     = b->used_length / be64toh(hdr.page_size);
        pos += sizeof(hdr);

        uint64_t bmap_bytes = BITS_TO_LONGS(b->num_pages) * sizeof(unsigned long);
        b->file_bmap = malloc(bmap_bytes);
        pread(s->snap_fd, b->file_bmap, bmap_bytes, b->bitmap_offset);
        b->recv_bmap = calloc(BITS_TO_LONGS(b->num_pages), sizeof(unsigned long));

        pos = b->pages_offset + b->used_length;
    }
}

static void close_snapshot(SnapshotLoadState *s)
{
    for (int i = 0; i < s->num_blocks; i++) {
        free(s->blocks[i].file_bmap);
        free(s->blocks[i].recv_bmap);
    }
    close(s->snap_fd);
}

static RAMBlockInfo *find_block_by_addr(SnapshotLoadState *s, uint64_t addr)
{
    for (int i = 0; i < s->num_blocks; i++) {
        uint64_t start = (uint64_t)(uintptr_t)s->blocks[i].host;
        if (addr >= start && addr < start + s->blocks[i].used_length)
            return &s->blocks[i];
    }
    return NULL;
}

/* ---- COPY mode page installers (mirrors postcopy-ram.c:1650-1696) ---- */

static int install_data_copy(SnapshotLoadState *s, RAMBlockInfo *b,
                             uint64_t pg, const char *data)
{
    struct uffdio_copy uc = {
        .dst  = (unsigned long)b->host + pg * PG_SIZE,
        .src  = (unsigned long)data,
        .len  = PG_SIZE,
        .mode = 0,
    };
    if (ioctl(s->uffd, UFFDIO_COPY, &uc) < 0)
        return errno == EEXIST ? 0 : -1;
    return 1;
}

static int install_zero_copy(SnapshotLoadState *s, RAMBlockInfo *b,
                             uint64_t pg, const char *data)
{
    (void)data;
    void *dst = (char *)b->host + pg * PG_SIZE;
    struct uffdio_zeropage uz = {
        .range = { .start = (unsigned long)dst, .len = PG_SIZE },
        .mode  = 0,
    };
    if (ioctl(s->uffd, UFFDIO_ZEROPAGE, &uz) < 0) {
        if (errno == EEXIST) return 0;
        struct uffdio_copy uc = {
            .dst  = (unsigned long)dst,
            .src  = (unsigned long)zero_buf,
            .len  = PG_SIZE,
            .mode = 0,
        };
        if (ioctl(s->uffd, UFFDIO_COPY, &uc) < 0)
            return errno == EEXIST ? 0 : -1;
    }
    return 1;
}

/* ---- CONTINUE mode page installer (memfd + UFFDIO_CONTINUE) ----
 *
 * MINOR faults on shmem fire when a page has backing data in the page
 * cache but no PTE in this mapping.  Holes (pages never written) read
 * as zero without faulting — perfect for zero pages.
 *
 * Data pages are pre-populated into the memfd (via pwrite from the
 * snapshot file) before uffd registration.  This fills the page cache
 * without creating PTEs.  Each first access triggers a MINOR fault,
 * and UFFDIO_CONTINUE just wires up the PTE — no data copy at all.
 *
 * The prefetch thread proactively CONTINUE's pages.  The fault handler
 * only fires for pages accessed before the prefetch reaches them.
 */

static int install_data_continue(SnapshotLoadState *s, RAMBlockInfo *b,
                                 uint64_t pg, const char *data)
{
    (void)data;
    struct uffdio_continue uc = {
        .range = { .start = (unsigned long)b->host + pg * PG_SIZE,
                   .len = PG_SIZE },
        .mode  = 0,
    };
    if (ioctl(s->uffd, UFFDIO_CONTINUE, &uc) < 0)
        return errno == EEXIST ? 0 : -1;
    return 1;
}

/* ---- fault handler thread (mirrors postcopy_ram_fault_thread) ---- */

static void *fault_handler_thread(void *arg)
{
    SnapshotLoadState *s = arg;
    char tmp[PG_SIZE] __attribute__((aligned(PG_SIZE)));
    struct pollfd pfd[2] = {
        { .fd = s->uffd,     .events = POLLIN },
        { .fd = s->quit_efd, .events = POLLIN },
    };

    for (;;) {
        if (poll(pfd, 2, -1) < 0) {
            if (errno == EINTR) continue;
            break;
        }
        if (pfd[1].revents & POLLIN) break;
        if (!(pfd[0].revents & POLLIN)) continue;

        struct uffd_msg msg;
        if (read(s->uffd, &msg, sizeof(msg)) != sizeof(msg)) continue;
        if (msg.event != UFFD_EVENT_PAGEFAULT) continue;

        struct timespec t0, t1;
        clock_gettime(CLOCK_MONOTONIC, &t0);

        uint64_t addr = msg.arg.pagefault.address;
        RAMBlockInfo *b = find_block_by_addr(s, addr);
        if (!b) abort();
        uint64_t pg = (addr - (uint64_t)(uintptr_t)b->host) / PG_SIZE;

        if (!claim_page(b, pg)) continue;

        int is_data = bmap_test(b->file_bmap, pg);
        if (is_data) {
            if (s->install_zero) {
                /* COPY mode: read page from snapshot then install */
                if (pread(s->snap_fd, tmp, PG_SIZE,
                          b->pages_offset + pg * PG_SIZE) != PG_SIZE)
                    abort();
                if (s->install_data(s, b, pg, tmp) < 0) abort();
            } else {
                /* CONTINUE mode: page cache already populated, just CONTINUE */
                if (s->install_data(s, b, pg, NULL) < 0) abort();
            }
            __atomic_fetch_add(&s->stat_data_faults, 1, __ATOMIC_RELAXED);
        } else {
            if (s->install_zero) {
                if (s->install_zero(s, b, pg, NULL) < 0) abort();
            }
            __atomic_fetch_add(&s->stat_zero_faults, 1, __ATOMIC_RELAXED);
        }

        clock_gettime(CLOCK_MONOTONIC, &t1);
        uint64_t ns = (t1.tv_sec - t0.tv_sec) * 1000000000ULL +
                      (t1.tv_nsec - t0.tv_nsec);
        record_latency(s, ns);
    }
    return NULL;
}

/* ---- prefetch thread (mirrors read_ramblock_mapped_ram, ram.c:4089) ----
 *
 * Walks the file bitmap per-block, reads contiguous data page runs in
 * PREFETCH_BUF_SIZE (1 MB) chunks, then installs page-by-page.
 * Zero pages: COPY mode installs them; CONTINUE mode skips them
 * (shmem holes read as zero without faulting).
 */

static void *prefetch_thread_fn(void *arg)
{
    SnapshotLoadState *s = arg;
    char *buf = aligned_alloc(PG_SIZE, PREFETCH_BUF_SIZE);
    if (!buf) return NULL;

    for (int bi = 0; bi < s->num_blocks; bi++) {
        RAMBlockInfo *b = &s->blocks[bi];
        unsigned long np = b->num_pages;
        if (!np) continue;

        unsigned long run_start = find_first_set(b->file_bmap, np);
        while (run_start < np) {
            unsigned long run_end = find_next_clear(b->file_bmap, np,
                                                    run_start + 1);
            uint64_t off = run_start * (uint64_t)PG_SIZE;
            uint64_t rem = (run_end - run_start) * (uint64_t)PG_SIZE;

            while (rem > 0) {
                uint64_t chunk = rem < PREFETCH_BUF_SIZE ? rem : PREFETCH_BUF_SIZE;
                ssize_t r = pread(s->snap_fd, buf, chunk,
                                  b->pages_offset + off);
                if (r != (ssize_t)chunk) goto done;

                for (uint64_t p = 0; p < chunk / PG_SIZE; p++) {
                    uint64_t pg = off / PG_SIZE + p;
                    if (!claim_page(b, pg)) continue;
                    if (s->install_data(s, b, pg, buf + p * PG_SIZE) < 0)
                        continue;
                    __atomic_fetch_add(&s->stat_prefetch, 1, __ATOMIC_RELAXED);
                }
                off += chunk;
                rem -= chunk;
            }
            run_start = find_next_set(b->file_bmap, np, run_end + 1);
        }
    }
done:
    free(buf);
    return NULL;
}

/* ---- eager load (baseline) ---- */

static double eager_load(void)
{
    double t0 = now_ms();

    SnapshotLoadState s = {0};
    open_snapshot(&s);

    for (int i = 0; i < s.num_blocks; i++) {
        RAMBlockInfo *b = &s.blocks[i];
        b->host = mmap(NULL, b->used_length, PROT_READ | PROT_WRITE,
                       MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (b->host == MAP_FAILED) die("mmap eager");
    }

    char *buf = aligned_alloc(PG_SIZE, PREFETCH_BUF_SIZE);
    for (int i = 0; i < s.num_blocks; i++) {
        RAMBlockInfo *b = &s.blocks[i];
        unsigned long np = b->num_pages;
        unsigned long run_start = find_first_set(b->file_bmap, np);
        while (run_start < np) {
            unsigned long run_end = find_next_clear(b->file_bmap, np,
                                                    run_start + 1);
            uint64_t off = run_start * (uint64_t)PG_SIZE;
            uint64_t rem = (run_end - run_start) * (uint64_t)PG_SIZE;
            while (rem > 0) {
                uint64_t chunk = rem < PREFETCH_BUF_SIZE ? rem : PREFETCH_BUF_SIZE;
                pread(s.snap_fd, (char *)b->host + off, chunk,
                      b->pages_offset + off);
                off += chunk;
                rem -= chunk;
            }
            run_start = find_next_set(b->file_bmap, np, run_end + 1);
        }
    }
    free(buf);

    double ttfi = now_ms() - t0;

    int err = 0;
    for (int i = 0; i < s.num_blocks; i++) {
        RAMBlockInfo *b = &s.blocks[i];
        for (uint64_t pg = 0; pg < b->num_pages; pg++) {
            char *p = (char *)b->host + pg * PG_SIZE;
            if (bmap_test(b->file_bmap, pg)) {
                char exp[80];
                snprintf(exp, sizeof(exp), "[%s] PAGE %lu",
                         b->idstr, (unsigned long)pg);
                if (strncmp(p, exp, strlen(exp))) err++;
            } else {
                if (p[0] != 0) err++;
            }
        }
    }

    printf("\n--- Eager Load ---\n");
    printf("TTFI:       %8.2f ms  (VM blocked until ALL RAM loaded)\n", ttfi);
    printf("Verify:     %s (%d blocks)\n", err ? "ERRORS" : "OK", s.num_blocks);

    for (int i = 0; i < s.num_blocks; i++)
        munmap(s.blocks[i].host, s.blocks[i].used_length);
    close_snapshot(&s);
    return ttfi;
}

/* ---- demand load (COPY or CONTINUE) ---- */

static int open_uffd(uint64_t features)
{
    int fd = (int)syscall(__NR_userfaultfd,
                          O_CLOEXEC | O_NONBLOCK | UFFD_USER_MODE_ONLY);
    if (fd < 0)
        fd = (int)syscall(__NR_userfaultfd, O_CLOEXEC | O_NONBLOCK);
    if (fd < 0) die("userfaultfd");

    struct uffdio_api api = { .api = UFFD_API, .features = features };
    if (ioctl(fd, UFFDIO_API, &api) < 0) die("UFFDIO_API");
    return fd;
}

static bool check_minor_shmem(void)
{
    int fd = (int)syscall(__NR_userfaultfd,
                          O_CLOEXEC | O_NONBLOCK | UFFD_USER_MODE_ONLY);
    if (fd < 0)
        fd = (int)syscall(__NR_userfaultfd, O_CLOEXEC | O_NONBLOCK);
    if (fd < 0) return false;
    struct uffdio_api api = {
        .api = UFFD_API, .features = UFFD_FEATURE_MINOR_SHMEM
    };
    int ret = ioctl(fd, UFFDIO_API, &api);
    close(fd);
    return ret == 0 && (api.features & UFFD_FEATURE_MINOR_SHMEM);
}

static double demand_load(enum LoadMode mode)
{
    const char *label = (mode == MODE_CONTINUE)
        ? "Demand Paging (UFFDIO_CONTINUE + memfd)"
        : "Demand Paging (UFFDIO_COPY)";

    double t0 = now_ms();

    SnapshotLoadState s = {0};
    s.hist_min_ns = LONG_MAX;
    open_snapshot(&s);
    double t_parse = now_ms() - t0;

    if (mode == MODE_CONTINUE) {
        s.uffd = open_uffd(UFFD_FEATURE_MINOR_SHMEM);
        s.install_data = install_data_continue;
        s.install_zero = NULL;
    } else {
        s.uffd = open_uffd(0);
        s.install_data = install_data_copy;
        s.install_zero = install_zero_copy;
    }

    char *pbuf = NULL;
    if (mode == MODE_CONTINUE)
        pbuf = aligned_alloc(PG_SIZE, PREFETCH_BUF_SIZE);

    for (int i = 0; i < s.num_blocks; i++) {
        RAMBlockInfo *b = &s.blocks[i];
        if (mode == MODE_CONTINUE) {
            b->memfd = (int)syscall(__NR_memfd_create, b->idstr, MFD_CLOEXEC);
            if (b->memfd < 0) die("memfd_create");
            if (ftruncate(b->memfd, b->used_length) < 0) die("ftruncate memfd");

            /*
             * Pre-populate memfd page cache: pwrite data pages from snapshot.
             * This fills the shmem page cache WITHOUT creating PTEs in the
             * mapping we'll create below.  First read through the mapping will
             * trigger a MINOR fault — resolved by UFFDIO_CONTINUE (PTE only).
             * Zero pages are shmem holes: read as zero, no fault needed.
             */
            unsigned long np = b->num_pages;
            unsigned long rs = find_first_set(b->file_bmap, np);
            while (rs < np) {
                unsigned long re = find_next_clear(b->file_bmap, np, rs + 1);
                uint64_t off = rs * (uint64_t)PG_SIZE;
                uint64_t rem = (re - rs) * (uint64_t)PG_SIZE;
                while (rem > 0) {
                    uint64_t chunk = rem < PREFETCH_BUF_SIZE ? rem : PREFETCH_BUF_SIZE;
                    ssize_t r = pread(s.snap_fd, pbuf, chunk,
                                      b->pages_offset + off);
                    if (r != (ssize_t)chunk) die("pread populate memfd");
                    if (pwrite(b->memfd, pbuf, chunk, off) != (ssize_t)chunk)
                        die("pwrite populate memfd");
                    off += chunk;
                    rem -= chunk;
                }
                rs = find_next_set(b->file_bmap, np, re + 1);
            }

            b->host = mmap(NULL, b->used_length, PROT_READ | PROT_WRITE,
                           MAP_SHARED, b->memfd, 0);
        } else {
            b->memfd = -1;
            b->host = mmap(NULL, b->used_length, PROT_READ | PROT_WRITE,
                           MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        }
        if (b->host == MAP_FAILED) die("mmap demand");

        uint64_t reg_mode = (mode == MODE_CONTINUE)
            ? UFFDIO_REGISTER_MODE_MINOR
            : UFFDIO_REGISTER_MODE_MISSING;
        struct uffdio_register reg = {
            .range = { .start = (unsigned long)b->host,
                       .len   = b->used_length },
            .mode = reg_mode,
        };
        if (ioctl(s.uffd, UFFDIO_REGISTER, &reg) < 0) die("UFFDIO_REGISTER");
    }
    free(pbuf);
    double t_setup = now_ms() - t0;

    s.quit_efd = eventfd(0, EFD_NONBLOCK);
    if (s.quit_efd < 0) die("eventfd");

    pthread_t ft, pt;
    pthread_create(&ft, NULL, fault_handler_thread, &s);
    pthread_create(&pt, NULL, prefetch_thread_fn, &s);

    volatile char *ram = s.blocks[0].host;
    (void)ram[0];
    double ttfi = now_ms() - t0;

    int err = 0;
    for (int i = 0; i < s.num_blocks; i++) {
        RAMBlockInfo *b = &s.blocks[i];
        volatile char *p = b->host;
        for (uint64_t pg = 0; pg < b->num_pages; pg++) {
            char c = p[pg * PG_SIZE];
            if (bmap_test(b->file_bmap, pg)) {
                if (c != '[') err++;
            } else {
                if (c != 0) err++;
            }
        }
    }

    pthread_join(pt, NULL);
    uint64_t v = 1;
    (void)write(s.quit_efd, &v, sizeof(v));
    pthread_join(ft, NULL);
    double total = now_ms() - t0;

    printf("\n--- %s ---\n", label);
    printf("Parse:      %8.2f ms  (headers + bitmaps)\n", t_parse);
    if (mode == MODE_CONTINUE) {
        printf("Populate:   %8.2f ms  (pwrite snapshot data into memfd)\n",
               t_setup - t_parse);
        printf("Setup:      %8.2f ms  (mmap + uffd + memfd populate)\n", t_setup);
    } else {
        printf("Setup:      %8.2f ms  (mmap + uffd)\n", t_setup);
    }
    printf("TTFI:       %8.2f ms  (first page access)\n", ttfi);
    printf("Total:      %8.1f ms  (all pages resolved)\n", total);
    printf("  Data faults: %6ld     Zero faults: %6ld     Prefetch: %6ld\n",
           (long)s.stat_data_faults, (long)s.stat_zero_faults,
           (long)s.stat_prefetch);
    if (mode == MODE_CONTINUE)
        printf("  Per-fault avg: %.1f us  (PTE install only, no data copy)\n",
               total > 0 && s.stat_data_faults > 0
               ? (double)s.hist_total_ns / s.stat_data_faults / 1000.0
               : 0.0);
    printf("Verify:     %s (%d blocks)\n", err ? "ERRORS" : "OK",
           s.num_blocks);
    print_histogram(&s);

    for (int i = 0; i < s.num_blocks; i++) {
        RAMBlockInfo *b = &s.blocks[i];
        struct uffdio_register unreg = {
            .range = { .start = (unsigned long)b->host,
                       .len   = b->used_length },
        };
        ioctl(s.uffd, UFFDIO_UNREGISTER, &unreg);
        munmap(b->host, b->used_length);
        if (b->memfd >= 0) close(b->memfd);
    }
    close(s.uffd);
    close(s.quit_efd);
    close_snapshot(&s);
    return ttfi;
}

/* ---- main ---- */

static void drop_page_cache(void)
{
    int fd = open(SNAPSHOT_FILE, O_RDONLY);
    if (fd >= 0) {
        posix_fadvise(fd, 0, 0, POSIX_FADV_DONTNEED);
        close(fd);
    }
}

int main(int argc, char *argv[])
{
    if (argc < 2) {
        fprintf(stderr,
            "Usage: %s <eager|demand|continue|all> [size_mb]\n\n"
            "  eager    -- load all RAM then start (baseline)\n"
            "  demand   -- UFFDIO_COPY demand-paging\n"
            "  continue -- UFFDIO_CONTINUE + memfd demand-paging\n"
            "  all      -- run all three, show comparison\n"
            "  default size: 128 MB\n", argv[0]);
        return 1;
    }

    int mb = argc >= 3 ? atoi(argv[2]) : 128;
    BlockDef defs[] = {
        { "pc.ram",  (uint64_t)mb * 1024 * 1024 },
        { "pc.bios", 256 * 1024 },
        { "pc.rom",  128 * 1024 },
    };
    int ndefs = sizeof(defs) / sizeof(defs[0]);

    printf("=== Fast Snapshot Load POC v2 ===\n");
    printf("Snapshot: %d blocks, %.1f MB total\n", ndefs,
           (defs[0].size + defs[1].size + defs[2].size) / 1048576.0);
    create_snapshot(defs, ndefs);

    bool do_eager    = !strcmp(argv[1], "eager")    || !strcmp(argv[1], "all");
    bool do_demand   = !strcmp(argv[1], "demand")   || !strcmp(argv[1], "all");
    bool do_continue = !strcmp(argv[1], "continue") || !strcmp(argv[1], "all");

    double e = 0, d = 0, c = 0;

    if (do_eager) {
        drop_page_cache();
        e = eager_load();
    }
    if (do_demand) {
        drop_page_cache();
        d = demand_load(MODE_COPY);
    }
    if (do_continue) {
        drop_page_cache();
        if (check_minor_shmem()) {
            c = demand_load(MODE_CONTINUE);
        } else {
            printf("\n--- UFFDIO_CONTINUE ---\n");
            printf("  Not supported (kernel lacks UFFD_FEATURE_MINOR_SHMEM)\n");
        }
    }

    if (!strcmp(argv[1], "all")) {
        printf("\n--- Comparison ---\n");
        printf("%-14s %10s %10s %10s\n", "", "Eager", "COPY", "CONTINUE");
        printf("%-14s %9.2f ms %9.2f ms", "TTFI:", e, d);
        if (c > 0) printf(" %9.2f ms", c); else printf("       n/a");
        printf("\n");
        if (e > 0) {
            printf("%-14s %10s", "Speedup:", "1x");
            if (d > 0) printf(" %9.0fx", e / d);
            if (c > 0) printf(" %9.0fx", e / c);
            printf("\n");
        }
    }

    unlink(SNAPSHOT_FILE);
    return 0;
}
