import json
from pathlib import Path
import subprocess
import sys
import tempfile


binary = str(Path(sys.argv[1]).resolve())


def run(*args, ok=True):
    result = subprocess.run([binary, *map(str, args)], capture_output=True, text=True)
    if (result.returncode == 0) != ok:
        raise AssertionError((args, result.stdout, result.stderr))
    return result.stdout


with tempfile.TemporaryDirectory(prefix="pfs-cli-test-") as parent:
    store = Path(parent) / "store"
    run("v2", "init", store, "--objects-per-page", "1024")
    info = json.loads(run("inspect", store))
    assert info["format_version"] == 2
    assert info["objects_per_page"] == 1024
    before = {p.name: p.read_bytes() for p in store.iterdir()}
    assert json.loads(run("fsck", store, "--check"))["objects"] == 1
    after = {p.name: p.read_bytes() for p in store.iterdir()}
    assert before == after, "read-only fsck modified files"
    root = json.loads(run("inspect", store, "--object", "0"))
    assert root["object_id"] == 0 and root["kind"] == 2
    run("compact-dir", store, "0")
    run("fsck", store, "--repair")
    run("init", store, ok=False)
    run("init", Path(parent) / "bad", "--objects-per-page", "0", ok=False)
    run("init", Path(parent) / "bad", "--objects-per-page", "16385", ok=False)
    run("inspect", store, "--object", "-1", ok=False)
    run("fsck", store, ok=False)
    run("mount", store, Path(parent) / "mount", "--unknown", ok=False)
    damaged = bytearray((store / "superblock").read_bytes())
    damaged[20] ^= 1
    (store / "superblock").write_bytes(damaged)
    run("fsck", store, "--check", ok=False)
print("v2 CLI tests passed")
