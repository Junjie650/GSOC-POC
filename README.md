# Fast Snapshot Load POC

Standalone demand-paging simulation for QEMU's proposed "Fast Snapshot
Load" feature.  Exercises the core userfaultfd mechanisms end to end
with a mapped-ram-style snapshot file format.

## Changelog

- **v2** (2026-04-20): Multi-RAMBlock layout, UFFDIO_ZEROPAGE, 1 MB
  chunked prefetch, mapped-ram header format alignment, memfd +
  UFFDIO_CONTINUE as third benchmark mode, per-fault latency histogram.
- **v1** (2026-03-14): Single-region UFFDIO_COPY demand-paging with
  atomic bitmap coordination.  Validated the basic mechanism.

## Build & Run

```bash
make
./fast_snapshot_poc all 512
```

If you get a permission error:

```bash
sudo sysctl vm.unprivileged_userfaultfd=1
```

## Three Loading Modes

| Mode | Registration | Fault resolution | Zero pages |
|------|-------------|-----------------|------------|
| **eager** | — | — (all pre-loaded) | zero-fill (anonymous mmap) |
| **demand** | `MODE_MISSING` | `UFFDIO_COPY` / `UFFDIO_ZEROPAGE` | kernel shared zero page |
| **continue** | `MODE_MINOR` on memfd | `UFFDIO_CONTINUE` (PTE only) | shmem hole (no ioctl) |

## Snapshot File Format

Mirrors QEMU's mapped-ram layout (`migration/ram.c:3005-3024`):

```
[num_blocks: uint32_t, big-endian]
Per block:
  [idstr: 64 bytes]  [used_length: uint64_t, big-endian]
  [MappedRamHeader]   <-- version, page_size, bitmap_offset, pages_offset
                          packed, big-endian, pages_offset 1 MB-aligned
  [bitmap]            <-- 1 bit/page, unsigned long granularity
  [page data]         <-- data pages only; zero pages are sparse holes
```

Three blocks model a typical x86 guest: `pc.ram` (user-specified),
`pc.bios` (256 KB), `pc.rom` (128 KB).

## Demand Mode (UFFDIO_COPY)

Models the postcopy path (`postcopy-ram.c:1520-1628`):

1. Parse headers + bitmaps from snapshot file
2. `mmap(MAP_ANONYMOUS)` each block, `UFFDIO_REGISTER(MODE_MISSING)`
3. Fault handler: `poll` uffd → `pread` snapshot → `UFFDIO_COPY` (data)
   or `UFFDIO_ZEROPAGE` (zero, falls back to `UFFDIO_COPY` if
   unsupported — mirrors `postcopy_place_page_zero`)
4. Background prefetch: bitmap walk with `find_first_set` /
   `find_next_clear` to detect contiguous data page runs, batch
   `pread` up to 1 MB, per-page `UFFDIO_COPY` (mirrors
   `read_ramblock_mapped_ram` at `ram.c:4089`)
5. Atomic `recv_bmap` ensures at most one thread resolves each page

## Continue Mode (UFFDIO_CONTINUE + memfd)

Explores an alternative uffd path not currently used in QEMU:

1. `memfd_create` per block, `pwrite` data pages from snapshot into
   memfd (populates page cache without creating PTEs)
2. `mmap(MAP_SHARED, memfd)`, `UFFDIO_REGISTER(MODE_MINOR)`
3. First access to a data page → MINOR fault → `UFFDIO_CONTINUE`
   (PTE install only, no data copy)
4. Zero pages are shmem holes — read as zero without any fault or ioctl
5. Prefetch thread proactively CONTINUE's data pages

**Key finding**: per-fault latency is ~3x lower than COPY mode (PTE
setup vs 4 KB data copy).  The populate step has the same I/O cost as
eager loading, so the TTFI advantage only materializes when the backing
file is already warm (repeated loads from the same snapshot, or
mmapping the snapshot file directly).

## Results

Measured on Intel NUC (i7-14700, NVMe, Linux 6.17, GCC 13.3).

### TTFI (Time To First Instruction)

| Size    | Eager      | Demand (COPY) | COPY Speedup | CONTINUE   |
|---------|------------|---------------|--------------|------------|
| 64 MB   |   58.1 ms  | 0.69 ms       |   84x        | ~58 ms *   |
| 128 MB  |  110.0 ms  | 0.77 ms       |  143x        | ~110 ms *  |
| 256 MB  |  216.0 ms  | 0.78 ms       |  278x        | ~218 ms *  |
| 512 MB  |  444.6 ms  | 0.90 ms       |  493x        | ~445 ms *  |
| 1024 MB |  861.5 ms  | 1.57 ms       |  550x        | ~862 ms *  |

\* CONTINUE TTFI ≈ eager because the memfd populate step has the same
I/O cost.  The advantage is per-fault latency, not TTFI — see below.

### Per-fault Latency (COPY vs CONTINUE)

| Metric       | COPY        | CONTINUE    |
|-------------|-------------|-------------|
| Avg latency | ~5.0 us     | ~1.8 us     |
| Zero faults | ~30% total  | 0           |
| Data copy   | 4 KB/fault  | none (PTE)  |

### Sample Histogram Output

```
  Fault latency histogram (us):
    [    0,     2) |################################| 22398  ( 88.5%)
    [    2,     4) |#                               | 1250  (  4.9%)
    [    4,     8) |#                               |   89  (  0.4%)
    [    8,    16) |#                               |   96  (  0.4%)
    [   16,    32) |#                               |  209  (  0.8%)
    [   32,    64) |#                               |  424  (  1.7%)
    [   64,   128) |#                               |  723  (  2.9%)
    [  128,   256) |#                               |  128  (  0.5%)
    min=0.3 us  avg=5.4 us  max=369.0 us
```

## Caveats

- Real QEMU TTFI includes device state loading (typically 5-50 ms), a
  fixed cost shared by all modes
- CONTINUE populate step has the same cost as eager loading — per-fault
  win matters for repeated snapshot loads or pre-warmed backing files
- QEMU's eager path uses multifd for parallel I/O; this POC is
  single-threaded for both eager and prefetch
- `posix_fadvise(DONTNEED)` flushes page cache between runs
