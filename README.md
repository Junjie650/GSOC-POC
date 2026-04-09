# Fast Snapshot Load POC

Standalone simulation of demand-paging snapshot restore using Linux
userfaultfd.  Generates a mapped-ram-style snapshot file, then compares
eager (read-all-first) vs demand (fault-driven) loading.

## Build & Run

```bash
make
./fast_snapshot_poc both 512
```

If you get a permission error, either run with `sudo` or:

```bash
sudo sysctl vm.unprivileged_userfaultfd=1
```

## How It Works

**Snapshot file layout** (simplified mapped-ram format):

```
[MappedRamHeader]          page_size, num_pages, bitmap_offset, pages_offset
[bitmap]                   1 bit per page: 1=data, 0=zero
[page data region]         page N at: pages_offset + N * page_size
```

**Eager mode** reads all data pages via `pread()` into an mmap'd region
before any access.  TTFI equals total load time.

**Demand mode**:

1. Read header + bitmap only
2. `mmap(MAP_ANONYMOUS)` + `UFFDIO_REGISTER(MISSING)`
3. Start fault handler thread (`poll` uffd → `pread` → `UFFDIO_COPY`)
4. Start background prefetch thread (sequential bitmap walk → `pread` → `UFFDIO_COPY`)
5. Access pages from main thread — first access = TTFI
6. All pages loaded → signal quit via eventfd → unregister uffd

The fault handler and prefetch thread coordinate via an atomic bitmap —
`__atomic_fetch_or` on bitmap words ensures at most one thread loads any
given page, with no per-page lock.

Zero pages are identified by the file bitmap and skip `pread()` entirely.

## Results

Measured on Intel NUC (i7-14700, NVMe, Linux 6.17, GCC 13.3).

| Size | Eager TTFI | Demand TTFI | Speedup | Demand Total |
|------|-----------|-------------|---------|-------------|
| 64 MB | 19.1 ms | 0.14 ms | 137x | 28.8 ms |
| 128 MB | 31.9 ms | 0.12 ms | 273x | 52.0 ms |
| 256 MB | 63.7 ms | 0.12 ms | 531x | 100.1 ms |
| 512 MB | 123.4 ms | 0.13 ms | 942x | 193.6 ms |
| 1024 MB | 246.1 ms | 0.20 ms | 1252x | 385.5 ms |

**TTFI** = time from start to first successful memory access.

Note that **demand total time is ~60% slower** than eager — every page
pays the `UFFDIO_COPY` ioctl overhead.  The win is purely in TTFI:
the simulated VM can start accessing memory immediately instead of
waiting for all RAM to be loaded.

## Caveats

This POC isolates the userfaultfd demand-paging mechanism.
Real-world numbers will differ due to:

- **Device state loading** — a fixed cost shared by both modes that
  narrows the gap
- **Storage latency** — cold reads from NVMe/SSD are slower per page,
  though the ratio stays similar
- **Multifd** — QEMU's eager path can parallelize I/O across threads
