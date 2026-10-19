#!/usr/bin/env python3
"""Run the same real-I/O workload on a host directory, FUSE mount, or SMB mount."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import time
import uuid


def summary(samples):
    values = sorted(samples)
    if not values:
        return {"count": 0}
    return {
        "count": len(values),
        "p50_ms": values[(len(values) - 1) * 50 // 100] * 1000,
        "p95_ms": values[(len(values) - 1) * 95 // 100] * 1000,
        "p99_ms": values[(len(values) - 1) * 99 // 100] * 1000,
        "ops_s": len(values) / sum(values),
    }


def run(args):
    root = Path(args.directory).resolve()
    if not root.is_dir():
        raise ValueError("directory must already exist")
    work = root / ("pfs-benchmark-" + uuid.uuid4().hex)
    work.mkdir(mode=0o700)
    result = {"timestamp": time.time(), "bytes": args.bytes, "files": args.files,
              "layout": args.layout, "file_fsync": args.fsync_files}
    large = work / "large.bin"
    digest = hashlib.sha256()
    start = time.monotonic()
    with large.open("wb", buffering=0) as out:
        remaining = args.bytes
        while remaining:
            block = os.urandom(min(1024 * 1024, remaining))
            digest.update(block)
            view = memoryview(block)
            while view:
                written = out.write(view)
                if not written:
                    raise OSError("short zero-byte write")
                view = view[written:]
            remaining -= len(block)
        os.fsync(out.fileno())
    result["write_seconds"] = time.monotonic() - start
    # Advisory cache eviction is best-effort; no privileged global cache dropping.
    with large.open("rb", buffering=0) as source:
        if hasattr(os, "posix_fadvise"):
            os.posix_fadvise(source.fileno(), 0, 0, os.POSIX_FADV_DONTNEED)
        start = time.monotonic()
        actual = hashlib.sha256()
        while block := source.read(1024 * 1024):
            actual.update(block)
    result["read_seconds"] = time.monotonic() - start
    if actual.digest() != digest.digest():
        raise RuntimeError("SHA-256 mismatch; benchmark artifacts retained")
    result["sha256"] = digest.hexdigest()
    result["write_mib_s"] = args.bytes / 1048576 / result["write_seconds"]
    result["read_mib_s"] = args.bytes / 1048576 / result["read_seconds"]
    small = work / "small"
    small.mkdir()
    if args.layout == "spread":
        for i in range(100):
            (small / str(i)).mkdir()
    latencies = []
    for i in range(args.files):
        parent = small if args.layout == "flat" else small / str(i % 100)
        start = time.monotonic()
        with (parent / str(i)).open("wb", buffering=0) as out:
            out.write(b"PersonalFS benchmark\n" * 8)
            if args.fsync_files:
                os.fsync(out.fileno())
        latencies.append(time.monotonic() - start)
    result["create"] = summary(latencies)
    start = time.monotonic()
    count = sum(len(files) for _, _, files in os.walk(small))
    result["list_seconds"] = time.monotonic() - start
    if count != args.files:
        raise RuntimeError("directory enumeration mismatch")
    start = time.monotonic()
    renamed = work / "renamed"
    small.rename(renamed)
    result["directory_rename_seconds"] = time.monotonic() - start
    start = time.monotonic()
    shutil.rmtree(renamed)
    result["delete_seconds"] = time.monotonic() - start
    # Reports stay in the selected storage boundary, alongside the workload.
    (work / "result.json").write_text(json.dumps(result, indent=2) + "\n")
    print(json.dumps(result, indent=2))
    if not args.keep:
        large.unlink()
    return result


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("directory")
    parser.add_argument("--bytes", type=int, default=30 * 1024 ** 3)
    parser.add_argument("--files", type=int, default=100000)
    parser.add_argument("--layout", choices=["flat", "spread"], default="flat")
    parser.add_argument("--fsync-files", action=argparse.BooleanOptionalAction, default=True)
    parser.add_argument("--keep", action="store_true", help="retain the large file for SMB verification")
    args = parser.parse_args()
    if args.bytes < 0 or args.files < 0:
        parser.error("bytes and files must be nonnegative")
    run(args)
