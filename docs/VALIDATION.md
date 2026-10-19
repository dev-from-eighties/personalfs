# v2 validation record — 2026-10-19

Branch: `lab_fast_answer`. This records prototype validation, not a production
certification or a comparison against v1 throughput.

## Automated checks

| Suite | Coverage | Result |
|---|---|---|
| `v2_store` | Binary envelopes/CRC32C/truncation, namespace, direct data bytes, 64-bit offsets, byte names, deep paths, permissions/timestamps, symlinks, rename/replacement/cycles, open unlink, directory snapshots, cache bypass/budgets, concurrent data and namespace operations, fsck/compaction, injected I/O errors, repeated REDO and damaged tails | Passed |
| `v2_crash_boundaries` | SIGKILL at every instrumented occurrence during create, cross-parent replacement rename, unlink and compaction, followed by recovery, identity assertions and fsck | Passed: 264 boundaries |
| `v2_cli` | Init, immutable page size, inspect JSON, read-only fsck byte preservation, repair, compaction, argument validation and corrupt superblock rejection | Passed |

The exhaustive sweep covers 78 create, 103 rename, 77 unlink and 6 compaction
boundaries. Each case uses a fresh store. Recovery is followed by another clean
open/check. Separate tests kill recovery itself and distinguish an unconfirmed torn
suffix, which can be redone, from corruption after COMMIT, which must fail.
The normal store suite also exercises 38 named SIGKILL scenarios and additional
recovery/tail scenarios.

`ENOSPC`, `EDQUOT`, `EIO`, `EACCES` and failed sync are injected in the **test-only**
core. Tests verify propagation, refusal of further namespace mutation after a failed
transaction, and successful redo on restart. These are deterministic error-path
tests; they do not simulate real exhausted filesystem inodes or a failing disk.

Release build and all three CTest suites pass. AddressSanitizer plus
UndefinedBehaviorSanitizer also pass all three suites with immediate error termination:

```sh
cmake -S . -B build/v2-sanitize -G Ninja -DCMAKE_BUILD_TYPE=Debug \
  -DCMAKE_CXX_FLAGS='-fsanitize=address,undefined -fno-omit-frame-pointer' \
  -DCMAKE_EXE_LINKER_FLAGS='-fsanitize=address,undefined'
cmake --build build/v2-sanitize
ASAN_OPTIONS=detect_leaks=0:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 \
  ctest --test-dir build/v2-sanitize --output-on-failure
```

LeakSanitizer could not run: its runtime reports that this instrumented environment
is under ptrace. Leak detection was therefore disabled; no leak-check result is claimed.
`git diff --check` passes, and builds use `-Wall -Wextra -Wpedantic` without warnings.
Production `personalfs` is built without the fault-injection environment hooks.

The benchmark script was smoke-tested with 8 MiB of real random data and 20 files
on the host filesystem. The POSIX smoke-test script was also exercised against the
host filesystem. These validate the scripts only, **not** FUSE behavior or v2 throughput.

## Required external validation

There is no `/dev/fuse` in the implementation environment. Consequently these checks
have not been performed here:

- Actual FUSE mounting and the mounted POSIX/permission matrix, including multiple users.
- A real, non-sparse 30 GiB transfer through v2 with SHA-256 verification.
- Nautilus → SMB → v2 on the intended HDD/encrypted backend.
- 100,000 small files, one million physical objects, and page-size comparisons on HDD/NVMe.
- Cold/warm throughput and latency comparisons against direct host I/O and v1.
- Real inode/space exhaustion and controlled power-loss tests on disposable storage.

Use `scripts/test-mounted.py` and `scripts/benchmark.py` as described in the README.
The 30 GiB offset test in the C++ suite is sparse and only verifies 64-bit offset
semantics; it is deliberately not presented as a large-file throughput test.

The format and recovery policy are documented in `FORMAT.md`. The single active
metadata transaction, linear cold directory replay, snapshot memory costs and
startup object sweep are explicit prototype limits to evaluate in those benchmarks.
