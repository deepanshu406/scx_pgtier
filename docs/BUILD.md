# Building and running scx_pgtier

Contents: [Requirements](#requirements) · [Build](#build) · [Run](#run-it-by-hand) ·
[Clean VM from scratch](#clean-vm-from-scratch)

## Requirements

**Kernel.** Linux with sched_ext and BTF:

- `CONFIG_SCHED_CLASS_EXT=y` and `CONFIG_DEBUG_INFO_BTF=y` (check: `ls /sys/kernel/btf/vmlinux`)
- cgroup v2
- Tested on Ubuntu 24.04 with kernel `7.0.0-38-generic`, on x86_64. aarch64 is accepted by
  the Makefile but untested.

**Tools used for the tested build:**

| What | Version tested |
|---|---|
| clang / llvm | 18.1.3 |
| bpftool | v7.7.0 (Ubuntu package `linux-tools-$(uname -r)`) |
| libbpf | v1.7.0, built from source (see below) |
| PostgreSQL | 16 (server, client tools, `pgbench`, server headers) |

```
sudo apt install build-essential git pkg-config clang llvm \
     libelf-dev zlib1g-dev libzstd-dev linux-tools-$(uname -r) \
     postgresql-16 postgresql-contrib postgresql-server-dev-16
```

`pgbench` is in `postgresql-contrib` on some releases. `postgresql-server-dev-16` is needed to
build the PostgreSQL extension.

## Build

The scheduler does not build from this repository alone. The Makefile takes the sched_ext
headers and `vmlinux.h` from `scx-c-examples`, and links a locally built libbpf so that the
headers, bpftool and library versions match.

**1. scx-c-examples** (headers and `vmlinux.h`), at the commit this was built against:

```
git clone https://github.com/sched-ext/scx-c-examples ~/work/scx-c-examples
git -C ~/work/scx-c-examples checkout 370b1cea95c6bb668d5b9261f069d433142b9986
```

**2. libbpf v1.7.0**, installed into a private prefix:

```
git clone https://github.com/libbpf/libbpf ~/work/libbpf
git -C ~/work/libbpf checkout v1.7.0
make -C ~/work/libbpf/src install PREFIX=$HOME/opt/libbpf
```

**3. The scheduler:**

```
make
```

This produces `build/scx_pgtier`. If your checkouts are elsewhere:

```
make SCX_DIR=/path/to/scx-c-examples LIBBPF_PREFIX=/path/to/libbpf/install
```

If `make` warns that libbpf was not found in `LIBBPF_PREFIX`, it used the system libbpf, which may
not match bpftool.

**4. The PostgreSQL extension** (`pgext/`), which puts each backend into its tier's cgroup:

```
make -C pgext
sudo make -C pgext install
```

Run `pg_config --pgxs` first if you want to check which PostgreSQL it will install into.

## Run it by hand

1. **cgroups.** Create the cgroup tree and give your PostgreSQL user ownership of it:
   ```
   sudo pgext/setup_cgroups.sh setup <postgres-os-user>
   ```
   This turns on the `cpu` controller in the root cgroup (`cgroup.subtree_control`) if it is not
   on yet. That is a system-wide setting, so try it in the VM first. To remove the tree again,
   run `pgext/setup_cgroups.sh remove`; stop PostgreSQL first, because a cgroup that still holds
   processes cannot be removed. Stop the scheduler before you remove the cgroups.
2. **PostgreSQL inside the tree.** The postmaster must itself be started inside
   `/sys/fs/cgroup/ufs-pg/main`, so that backends inherit it. Move the shell there first, then
   drop to the PostgreSQL user (see the `PostgreSQL` step of `scripts/vm_setup.sh` for a working
   example). Check with `cat /proc/<postmaster pid>/cgroup`, which must print `0::/ufs-pg/main`.
3. **Extension and roles.** Load the extension in every session and give each role a tier:
   ```
   ALTER SYSTEM SET session_preload_libraries = 'pgtier';
   ALTER ROLE oltp_app    SET pgtier.tier = 'ts';   -- time-sensitive
   ALTER ROLE report_user SET pgtier.tier = 'bg';   -- background
   ```
   Then reload the configuration. A new connection as `oltp_app` runs in a `ufs-ts-*` cgroup.
4. **Start the scheduler** as root: `sudo ./build/scx_pgtier`. Stop it with Ctrl-C and the kernel
   returns to its default scheduler. `cat /sys/kernel/sched_ext/state` says `enabled` while it runs.

The scheduler runs in *partial* mode: only tasks switched to the sched_ext class are affected.

## Clean VM from scratch

This is the setup the benchmarks were run in. It uses [virtme-ng](https://github.com/arighi/virtme-ng)
(`vng`), which boots a kernel as a throwaway VM that shares the host's root filesystem.

**1. On the host**, install the packages from [Requirements](#requirements), then build as
described above. The VM sees the host's files, so PostgreSQL has to be installed on the host.
Stop the host's own PostgreSQL service so that it does not compete with the test one:

```
sudo systemctl disable --now postgresql
```

**2. Boot the VM.** From the repository directory on the host:

```
vng -r --cpus 8 --memory 8G --user root
```

`-r` boots the kernel that is running on the host (check: `uname -r` inside the VM). You get a
root shell in the same directory.

**3. Inside the VM**, set everything up with one script:

```
scripts/vm_setup.sh            # cgroups, PostgreSQL, roles, data, scheduler
scripts/vm_setup.sh --no-sched # same, but leave the scheduler off (EEVDF baseline)
```

It is safe to run again: every step checks before it changes anything. It refuses to run outside
a virtme-ng VM unless you pass `--force`, because it changes cgroups and system settings.
Settings can be overridden with environment variables (`PGUSER_OS`, `PGBIN`, `PGDATA`, `PORT`,
`SOCK`, `DB`, `SCALE`); they are listed at the top of the script.

Then continue with [BENCHMARK.md](BENCHMARK.md).

Things to know about the VM:

- **State is not persistent.** After you leave the VM, assume the cluster, the cgroups and the
  scheduler are gone. Run `scripts/vm_setup.sh` again after every boot.
- **Disk space is small.** `/home` and `/tmp` are overlays of about 3.9 GB. Table bloat, WAL and
  trace files can fill them ("No space left on device"). `scripts/bench_funcs.sh` resets the small
  pgbench tables before every run for this reason.
- **Rebuilt binaries can be stale inside the VM.** After a rebuild on the host the VM may still
  hold the old file in its cache and crash at start-up. The scripts run a private copy of the
  binary (`/tmp/scx_pgtier.run`) after dropping the page cache. Do the same if you start it by
  hand.
- **Rebuilding `pgtier.so` while PostgreSQL is running** in the VM can crash backends, for the
  same reason. Stop PostgreSQL first.
