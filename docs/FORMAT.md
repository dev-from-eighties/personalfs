# PersonalFS v2 prototype format (2.0)

This document records implementation decisions supplementing `PersonalFSv2_SPEC.md`.
Format 2.0 is experimental and is not compatible with v1. All integers are unsigned
little-endian except timestamp seconds, which use signed 64-bit two's-complement.
No native C++ structs are written to disk. Strings carry explicit lengths and raw
bytes. Checksums use CRC32C/Castagnoli, reflected polynomial `0x82f63b78`, initial
and final XOR `0xffffffff` (`123456789` → `0xe3069283`).

## Physical layout

```
STORE/superblock
STORE/lock
STORE/journal
STORE/0.obj
STORE/0.meta
STORE/0000000000000000/1.obj
STORE/0000000000000000/1.meta
STORE/0000000000000001/4097.obj
STORE/0000000000000001/4097.meta
STORE/stats.json                  # optional aggregate instrumentation
STORE/events.log                  # optional structured I/O error diagnostics
```

Page names are exactly 16 lowercase hex digits, `(id-1)/objects_per_page`; IDs in
filenames are canonical unsigned decimal. Root ID 0 lives directly in STORE.
There is exactly one physical page level. Pages are mode 0700 and files 0600.
Physical access uses retained directory descriptors and `openat`/`renameat`, with
`O_NOFOLLOW` on physical page/file opens. The daemon retains an exclusive flock
on `lock`. Offline readers also require this lock to obtain a coherent view.

Temporary names start `.pfs-`, are unique to the process/operation, and end `.tmp`.
They are created beside the publication target, never outside STORE. Publication
uses write, fsync(temp), renameat(temp,target), fsync(containing directory).
Root and pages are fsynced when created. Init also syncs the store's parent.
Each object uses at most two permanent page entries; inspect includes temporary
entries when reporting occupancy.

## Binary envelopes

Every structure below except raw file/symlink payloads uses this envelope:

| Field | Encoding |
|---|---|
| magic | 8 bytes, no terminator |
| payload length | u32 |
| payload | exactly length bytes |
| checksum | u32 CRC32C over magic, length and payload |

Readers require exact lengths and valid checksums. Superblock/sidecars are capped
at 4096 bytes, journal at 16 MiB, directory record payload at 1024 bytes. There is
no accepted trailing garbage in a published fixed structure.

### Superblock: `PFS2SUPR`

Payload: `major:u32=2, minor:u32=0, objects_per_page:u32, page_encoding:u32=16,
root_id:u64=0, next_object_id:u64, next_transaction_id:u64, flags:u64=0`.
Total size: 64 bytes. Here encoding 16 means lowercase hexadecimal, width 16;
no other encoding is accepted. Unsupported versions/flags are rejected.

Both counters are reserved by atomic superblock replacement **before** BEGIN.
IDs start at 1, never recycle, and exhaustion fails. Crash gaps are allowed.
Page size comes only from the stored superblock, never from a mount default.

### Sidecar: `PFS2META`

Payload in order:

```
version:u32=2
object_id:u64
kind:u8                  # 1 regular, 2 directory, 3 symlink
mode:u32                 # permission bits only
uid:u32, gid:u32
nlink:u32                # 1 linked / 0 durable tombstone, no hardlinks
parent_id:u64
metadata_generation:u64
directory_log_sequence:u64
child_directory_count:u64
atime_sec:i64, atime_nsec:u32
mtime_sec:i64, mtime_nsec:u32
ctime_sec:i64, ctime_nsec:u32
```

Total size: 113 bytes. Regular file length, data atime/mtime and host ctime come
from fstat of the data object; virtual ctime is the later of host/sidecar ctime.
Directory timestamps come from its sidecar. Directory st_nlink is 2 plus the
child-directory count while linked, zero after unlink. Replay/fsck verifies the
membership log and fsck checks child-directory counts. Sidecar generations advance
on virtual metadata changes, independently of the directory record sequence.

### Directory object

Header envelope `PFS2DIRH`, payload `object_id:u64, base_sequence:u64` (32 bytes
including envelope). Then consecutive `PFS2DREC` envelopes:

```
opcode:u8                # 1 ADD, 2 REMOVE
sequence:u64
txid:u64
name_length:u32
name:name_length bytes
child_id:u64
kind_hint:u8
```

Record size is `46 + name_length`. Names contain 1–255 arbitrary bytes excluding
NUL and slash; `.`/`..` are synthesized, never persisted. Each sequence equals
its predecessor plus one. ADD requires absence; REMOVE requires matching ID and
type. The final sequence must match the durable sidecar. No automatic tail trimming
is allowed without an outstanding transaction proving ownership of that suffix.

Compaction preserves the final sequence: its header base is `final_sequence -
live_entry_count`, followed by one ADD per live entry, txid=0. It does not change
object IDs or sidecars. The replacement is published only if the original sequence
is unchanged under the directory lock. It does not walk descendants.

### Data and symbolic links

Regular `.obj` files contain exact application bytes with no header and no staging.
Symlink `.obj` files contain target bytes. Host files remain ordinary regular files;
virtual symlink behavior is implemented by FUSE. Per-block data writes do not update
the journal, sidecars, or namespace. Normal host write/fsync semantics apply.

## Journal: one atomically published transaction

The v0.1 implementation chooses a **single active intent slot**, not a torn-tail
append journal. This resolves ambiguity between a torn BEGIN and corruption of a
confirmed BEGIN: a published slot was fully written and fsynced before rename.
An empty journal means no pending transaction. Nonempty journal envelope magic is
`PFS2JRNL`; payload is:

```
txid:u64
action_count:u32          # at most 64
repeat action_count:
    action:u8            # 1 CREATE_OBJECT, 2 REPLACE_META, 3 APPEND_DIRECTORY
    object_id:u64
    before:u64
    byte_count:u32
    result_bytes:byte_count bytes
committed:u8             # 0 BEGIN / 1 BEGIN plus COMMIT
```

The BEGIN and COMMIT states are each published via a fresh atomic replacement;
COMMIT retains the full redo bytes. This is semantically BEGIN/apply/COMMIT, with
no group commit and no data payload logging for ordinary application writes.
The empty DONE state is also atomically published after all result data and
containing directories have been synced.

- CREATE_OBJECT contains the initial payload. The unique reserved ID may be absent
  or contain an exact prefix of that payload. A different existing object is an
  integrity failure. The object and page are synced before proceeding.
- REPLACE_META contains the full target sidecar. `before` is the expected old
  metadata generation; `UINT64_MAX` means a new sidecar. An exact target sidecar is
  already applied; otherwise the old generation must match. CRC or identity errors
  are not silently accepted.
- APPEND_DIRECTORY contains exact encoded records and `before` is the original EOF.
  Redo writes at that offset, **never appends at the current EOF**. An exact prefix
  may be completed. A torn suffix belonging to uncommitted BEGIN can be rebuilt;
  a different valid/checksummed record is rejected. A COMMIT with a truncated or
  differing suffix is an integrity error. Bytes before the original EOF are never
  truncated or rewritten. After replay, normal log validation checks the result.

Recovery replays all actions, validates affected references, then publishes empty
DONE. Repeating recovery is idempotent. Directory locks are held through the whole
live transaction, in increasing ObjectID order. A journal coordinator serializes
mutations; file data I/O and independent directory readers do not acquire it.
A live transaction error poisons namespace access and further mutations until
restart, preventing clients from observing partially applied metadata.

Unlink first durably removes the name and marks the sidecar nlink=0 in the same
transaction. Open handles pin both files. Collection removes data, **fsyncs the
page**, then removes the tombstone and fsyncs again. Thus a crash cannot persist
sidecar removal while leaving unexplained data. On restart, unlinked tombstones
can be collected because no old process descriptors remain.

`fsck --check` requires a clean journal, validates all directory records, object
identities/types, parent links, unique reachability, link counts, placement and
allocator bounds. `--repair` redoes only documented pending work and removes
verified tombstones/internal temporaries; it never manufactures a lost namespace.

## Remaining validation gates

SIGKILL tests exercise process-crash boundaries. They do not model hardware write
reordering or devices that lie about fsync. Controlled power cuts, real ENOSPC/
inode exhaustion on the selected encrypted host filesystem, mounted POSIX tests,
and HDD/SMB workload measurements remain required before declaring this format
production-stable. See the repository specification for the P0/P1 acceptance matrix.
