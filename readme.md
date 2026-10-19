# PersonalFS v2

PersonalFS exposes a virtual POSIX namespace through FUSE. Each file is an ordinary
host file with a short numeric name. Directory objects contain binary append-only
membership logs. There is no disk image, database, staging area, resizing, or global
compaction. File contents go directly through `pread` / `pwrite` on open descriptors.

This branch implements the v2 prototype described in [PersonalFSv2_SPEC.md](PersonalFSv2_SPEC.md).
It is independent of v1 and does not read its configuration or images.
The on-disk format is experimental; see [docs/FORMAT.md](docs/FORMAT.md).

## Build and test

Linux dependencies: C++20 compiler, CMake 3.24+, Ninja, pkg-config, libfuse3-dev,
and pthreads; the default test build also requires Python 3. Mounting also requires `/dev/fuse` and the normal FUSE permissions.
No Btrfs tools, loop devices, or privileged internal mount are required.

```sh
./scripts/build-debug.sh
ctest --test-dir build/debug --output-on-failure
./scripts/build-release.sh
```

`personalfs_tests` exercises the backend without requiring a FUSE device. A separate
test-only library enables SIGKILL and I/O fault injection, including an exhaustive
sweep of every instrumented interruption boundary; these environment hooks
are absent from the production executable.

## Use

Keep the **entire store** inside the encrypted filesystem if encryption is needed.
The daemon user must own the store, with permissions `0700`. The mountpoint is the
public namespace to share through Samba; clients must never modify the physical store.

```sh
build/release/personalfs init /protected/alice
mkdir -p /srv/shares/alice
build/release/personalfs mount /protected/alice /srv/shares/alice --foreground
```

The parent of the store must exist. `init` accepts an absent or empty directory,
checks permissions, physical path/component limits, and probes backend filename
acceptance. Choose a shorter store path if it reports `ENAMETOOLONG`.

Use `--allow-other` for other users or Samba, subject to `/etc/fuse.conf` and the
normal FUSE policy. Kernel `default_permissions` is always enabled. Other libfuse
options can be passed with `-o OPTIONS`.

```sh
build/release/personalfs mount /protected/alice /srv/shares/alice \
  --foreground --allow-other --cache-bytes 67108864
build/release/personalfs stats /srv/shares/alice
fusermount3 -u /srv/shares/alice
```

Omit `--foreground` to use libfuse daemonization. The store directory is held by
file descriptor, so the daemon's change of working directory does not affect it.
Normal unmount calls `syncfs`, propagates errors, and saves aggregate statistics
inside the store as `stats.json`. A successful `write` alone is not a durability
promise; use `fsync` / `fdatasync` as with other Linux filesystems.

There is no configuration file. Cache budget is a mount option; zero disables it.
Page size is persisted, immutable, and defaults to 4096 objects. Experimental stores
can use `init STORE --objects-per-page N` (1–16384) for page-size benchmarks.
The optional CLI prefix `v2` is accepted, e.g. `personalfs v2 init STORE`.

Offline maintenance (requires exclusive access; unmount first):

```sh
build/release/personalfs inspect /protected/alice
build/release/personalfs inspect /protected/alice --object 17
build/release/personalfs fsck /protected/alice --check
build/release/personalfs fsck /protected/alice --repair
build/release/personalfs compact-dir /protected/alice 17
```

`--check` never changes the namespace or repairs records. A pending journal requires
`--repair`. Repair only redoes the journal, collects durable unlinked tombstones, and
removes abandoned internal temporaries. It then checks the whole namespace. It does
not guess names, discard bad confirmed checksums, or salvage arbitrary corruption.
Mount performs the same recovery before serving requests. If BEGIN was durable, an
operation interrupted before returning to its caller may appear completed after recovery.

## Supported behavior

- Regular files: create/open/read/write/flush/release/fsync, truncate/ftruncate.
- Directories: mkdir/rmdir, enumeration with stable per-open snapshot cookies.
- Rename: same or different parent, replacement, `RENAME_NOREPLACE`, cycle rejection.
  IDs remain stable; directory rename does not visit descendants.
- Unlinked open files and directories remain backed until their final release.
- chmod/chown, nanosecond utimens, byte-preserving names, symlink/readlink, statfs.
- Hard links, special nodes, ACLs/xattrs, fallocate, `RENAME_EXCHANGE` and
  `RENAME_WHITEOUT` return `EOPNOTSUPP`. The sole synthetic xattr is
  `user.personalfs.stats` on the mount root. setuid and regular-file setgid are
  explicitly unsupported; directory setgid inheritance is supported.
- Directory atime is not advanced on lookup/readdir. Explicit utimens is supported.
  Linux VFS name/path limits remain in force; physical path length is independent
  of virtual depth, but this does not extend the kernel's syscall limits.

## Concurrency and resource costs

Data I/O on an open file does not take the namespace/journal locks, load a directory,
or rewrite a sidecar. Each directory has its own lock. Multiobject changes lock IDs
in increasing order; readers of affected directories see the whole transaction.

The initial journal permits **one active metadata transaction**. Mutations serialize
through durable BEGIN/apply/COMMIT/cleanup; unrelated data I/O and unrelated cached
lookups continue. There is no group commit. This intentionally conservative metadata
path is a performance limit to measure, especially for 100,000 small files on HDD.
It does not promise the metadata concurrency of a production filesystem.

The LRU cache accounts conservatively for tree nodes, names, map/control structures,
and allocation overhead (256 bytes per directory plus 256 + twice the name length
per entry). The byte budget includes in-progress cache reconstruction; active shared
references pin entries against eviction. Oversized directories bypass the cache,
and lookup replays records with bounded scratch space. Cold lookup is linear in log
length. Access never triggers compaction.

An open directory owns an independent logical snapshot, so memory for open `readdir`
handles is proportional to their live entries and is **separate from the cache budget**.
Manual compaction and fsck also need memory proportional to the namespace they inspect.
Startup orphan sweeping visits physical objects. These are prototype scaling costs.

Manual compaction constructs a temporary snapshot, syncs it, and validates the captured
sequence under that directory's lock before atomic replacement. A concurrent change
causes `EAGAIN`, never publication of stale state. Automatic compaction is not enabled.

## Instrumentation and validation

`stats` returns JSON with a wall-clock timestamp, monotonic elapsed time, operation
counts, latency histograms (p50/p95/p99 upper bucket bounds), read/write sizes,
throughput, journal queue wait, sync timings, cache bytes/hits/misses/evictions,
recovery and compaction counters, and a count of operations exceeding one second.
Throughput in this snapshot is averaged over the daemon lifetime. Offline `inspect`
counts actual physical entries per page, including sidecars and temporaries.
Reports do not contain virtual filenames. Runtime I/O/integrity errors are recorded
as JSON lines in `events.log` inside the store; expected lookup/unsupported-operation
errors are counted without writing that log.

For an already mounted, disposable test filesystem:

```sh
python3 scripts/test-mounted.py /srv/shares/alice
python3 scripts/benchmark.py /srv/shares/alice --bytes 32212254720 --files 100000 --keep
python3 scripts/benchmark.py /srv/shares/alice --bytes 32212254720 --files 100000 --layout spread
```

Run the identical benchmark on the direct host filesystem and on a separately built
v1 mount. Repeat cold/warm runs on the same HDD, encryption layer, and SMB client.
The benchmark writes real random data, fsyncs, verifies SHA-256, measures create/list/
rename/delete, and writes `result.json` **inside the selected filesystem**. Random
number generation and hashing are included in end-to-end throughput. Advisory cache
eviction is best-effort, not proof of a cold disk cache. The report is retained; the
large test file is removed unless `--keep` is given. Use `--files 1000000 --layout spread`
for a real million-object workload, with sufficient time, inodes, and free space.

The local results and remaining checks are recorded in [docs/VALIDATION.md](docs/VALIDATION.md).

The local backend tests are not a substitute for mounted POSIX tests, a 30 GiB SMB
copy from Nautilus, HDD benchmarks, or controlled power-loss testing. SIGKILL does
not simulate storage losing previously acknowledged writes. P1 hardware/power-loss
validation remains a production gate from the specification.
