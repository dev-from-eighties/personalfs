#!/usr/bin/env python3
"""POSIX smoke test against an already mounted, disposable PersonalFS v2 mount."""
import hashlib
import os
from pathlib import Path
import shutil
import sys
import uuid


root = Path(sys.argv[1]) / ("pfs-posix-test-" + uuid.uuid4().hex)
root.mkdir()
try:
    a, b = root / "a", root / "b"
    a.mkdir()
    b.mkdir()
    payload = os.urandom(8 * 1024 * 1024)
    original = a / ("x" * 255)
    with original.open("w+b", buffering=0) as stream:
        stream.write(payload)
        os.fsync(stream.fileno())
        inode = os.fstat(stream.fileno()).st_ino
        target = b / "renamed"
        original.rename(target)
        assert target.stat().st_ino == inode
        target.unlink()
        stream.seek(0)
        assert hashlib.sha256(stream.read()).digest() == hashlib.sha256(payload).digest()
        assert os.fstat(stream.fileno()).st_nlink == 0
    source = a / "source"
    source.write_bytes(payload)
    target = b / "destination"
    target.write_bytes(b"old")
    source.replace(target)
    assert target.read_bytes() == payload
    os.truncate(target, 1234)
    assert target.stat().st_size == 1234
    os.chmod(target, 0o640)
    assert target.stat().st_mode & 0o777 == 0o640
    os.utime(target, ns=(1000000000123, 2000000000456))
    assert target.stat().st_mtime_ns == 2000000000456
    link = root / "link"
    link.symlink_to("b/destination")
    assert link.read_bytes() == payload[:1234]
    raw = os.fsencode(a) + b"/non-utf8-\xff"
    fd = os.open(raw, os.O_CREAT | os.O_WRONLY, 0o600)
    os.close(fd)
    assert b"non-utf8-\xff" in os.listdir(os.fsencode(a))
    cursor = a
    for _ in range(20):
        cursor = cursor / ("deep" * 20)
        cursor.mkdir()
    (cursor / "file").write_bytes(b"nested")
    a.rename(root / "moved")
    assert len(list(root.iterdir())) == 3
    print("Mounted POSIX smoke test passed")
finally:
    shutil.rmtree(root)
