# PersonalFS

PersonalFS automates the following:

- It creates and manages, for you, a personal.img file with a btrfs inside.
- The img file grows while you use it up to the quota you assigned to it.
- The reported filesystem size is capped by the configured quota and by the
  available space in the parent filesystem.
- The internal fs grows while you add files, and shrinks while you delete files.
- This allows for private fs for multiple users with adaptive space usage and max growth limit.
- Ideal for NAS and file servers.
- It gives you very large paths and filenames.
- It mounts in user space.

## MVP design

PersonalFS has two mount layers:

- The internal layer mounts `personal.img` as btrfs in the work directory.
- The public layer is FUSE. This is the filesystem seen by applications,
  SMB/NFS, users, and tools like `df`.

The FUSE layer reports the smaller value between the configured quota and what
the parent filesystem can still provide. In practice, reported capacity is
`min(quota, used-inside-personalfs + free-space-in-parent-fs)`. The image below
it grows and shrinks adaptively, but clients never see a capacity larger than
PersonalFS can realistically satisfy.

There are no background resize tasks. Resize decisions happen while serving IO:

- Writes, creates, directory creation, links, and symlinks check free space and
  grow the image when free space is below the configured threshold.
- Deletes and truncates may shrink the image only when enough space can be
  reclaimed to justify the operation.
- `adaptive_shrink = false` disables shrink checks during normal IO. With
  `shrink_on_unmount = true`, PersonalFS still shrinks once while unmounting.
  You can also run `personalfs shrink <workdir>` explicitly during server
  shutdown/restart scripts.

Resize can run in two modes:

- `fibonacci`: the default. It uses a Fibonacci-sized step derived from the current image size,
  capped by `fibonacci_max_step` so a large image does not suddenly jump by
  hundreds of GB unless configured that way.
- `static`: grows and shrinks by a fixed `static_step`.

## Work directory layout

The layout follows the same idea as virtualvaultfs: the work directory is not
the share. It contains backend state, while the FUSE mount point is the path to
share.

```text
workdir/
  pfs-data/
    config.toml
    personal.img
    mounted-img/
```

Where virtualvaultfs stores a SQLite database, PersonalFS stores `personal.img`.
`mounted-img/` is the private internal btrfs mount point.

If you want encryption, intended way of achieve is to put "workdir" inside of
a gocryptfs, or similar, fs.

Put the FUSE mount point where you want, then share it. Note that if you are
also sharing the encrypted fs root, the FUSE mount point of personalfs must be
outside to be shared as an independent resource.

## Build

Dependencies:

```sh
sudo apt install build-essential cmake ninja-build pkg-config libfuse3-dev
sudo apt install btrfs-progs util-linux
```

Build:

```sh
./scripts/build-debug.sh
```

The binary will be at `build/debug/personalfs`.

## Usage

Initialize a work directory:

```sh
build/debug/personalfs initialize /srv/personalfs/alice --quota 500G --initial-size 10G
```

Mount it:

```sh
mkdir -p /srv/shares/alice
sudo build/debug/personalfs mount /srv/personalfs/alice /srv/shares/alice
```

Mount with `allow_other`, useful when the mount is served by SMB/NFS:

```sh
sudo build/debug/personalfs mount /srv/personalfs/alice /srv/shares/alice --allow-other
```

or:

```sh
sudo build/debug/personalfs mount /srv/personalfs/alice /srv/shares/alice -o allow_other
```

For debugging FUSE operations:

```sh
sudo build/debug/personalfs mount /srv/personalfs/alice /srv/shares/alice --allow-other --verbose
```

`--verbose` logs paths and filesystem operations. It is useful while testing
SMB/Windows clients, but it can expose private filenames in logs.

For `allow_other`, `/etc/fuse.conf` must contain:

```text
user_allow_other
```

Run a manual shrink:

```sh
build/debug/personalfs shrink /srv/personalfs/alice
```

## Configuration

`initialize` creates `workdir/pfs-data/config.toml`. Example:

```toml
storage_root = "pfs-data"
image_name = "personal.img"
runtime_mount_name = "mounted-img"

quota_size = "100G"
initial_size = "1G"
resize_mode = "fibonacci"
static_step = "1G"
fibonacci_max_step = "32G"
grow_free_threshold = "256M"

adaptive_shrink = true
shrink_on_unmount = true
shrink_free_threshold = "2G"
shrink_min_reclaim = "1G"
```

Current MVP expects Linux tools available in `PATH`: `mkfs.btrfs`, `btrfs`,
`mount`, `umount`, `findmnt`, and `losetup`. Mounting the btrfs image usually
requires root or an equivalent system policy.

`--allow-other` is only for the public FUSE mount. It does not grant permission
to create the internal loop device used by the btrfs image. If mount fails with
an error about configuring a loop device, run the mount command through a
root/systemd service context or another setup with `CAP_SYS_ADMIN`.
