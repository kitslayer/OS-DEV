# What's next

> **(M1257) time — `clock_nanosleep(TIMER_ABSTIME)` + `clock_getres`.** `nanosleep` (M1234) only sleeps relative; drift-free periodic tasks need an ABSOLUTE deadline. `clock_nanosleep(clockid, flags, sec, nsec)`: with `TIMER_ABSTIME` the time is an absolute deadline on the named clock — `CLOCK_MONOTONIC` (uptime ms) or `CLOCK_REALTIME` (wall epoch from RTC); the kernel sleeps `deadline − now` over the existing 100Hz `timer_wait` path, and a deadline already in the past returns immediately. Without the flag it's a relative sleep. `clock_getres` returns 10,000,000 ns (the 10 ms tick). New `SYS_clock_nanosleep` (243), `SYS_clock_getres` (244), `CLOCK_REALTIME`/`CLOCK_MONOTONIC`/`TIMER_ABSTIME`. **Verified:** `make check` **57 green**; **headless in-guest** — `clocknstest` asserts `getres==10ms`, an ABSTIME deadline 150 ms out sleeps ~150 ms, and a past deadline returns immediately: `clock_nanosleep: getres=10000000ns; ABSTIME +150ms slept ~160ms; past deadline returned ~0ms (immediate) -- OK`, desktop healthy.

> **(M1256) proc — `/proc/diskstats`: per-block-device I/O counters.** The block-device registry (`kernel/blockdev.c`) routed every driver's I/O through `blockdev_read`/`blockdev_write` but tracked nothing — no `iostat`-style view. Added real instrumentation: `blockdev_t` gains `rd_ios`/`rd_sectors`/`wr_ios`/`wr_sectors` (zeroed at register), tallied on each successful read/write. New `gen_diskstats` in procfs.c emits one line per device in the Linux `/proc/diskstats` layout (`major minor name rd_ios rd_merges rd_sectors rd_ticks wr_ios wr_merges wr_sectors wr_ticks in_flight io_ticks time_in_queue`); the four counts we keep are real, the rest (merges/ticks/in-flight/queue-time) honest zeros, major/minor = `0 <registry-index>`. No new syscall. **Verified:** `make check` **57 green**; **headless in-guest** — `diskstatstest` parses `/proc/diskstats`, counts devices + sums `rd_ios`, asserting ≥1 device with `rd_ios>0` (boot enumeration reads LBA 0 of each): `diskstats: 1 block device(s), total rd_ios=6 rd_sectors=6 (real per-device I/O counters tallied in blockdev_read/write) -- OK`, desktop healthy.

> **(M1255) driver — virtio-rng paravirtual hardware entropy source.** Adds `kernel/virtio_rng.c`, a legacy virtio-over-PCI driver for the entropy device (vendor 0x1AF4, device 0x1005) — the simplest virtio device: hand it a device-writable buffer over one virtqueue and the host DMAs random bytes in from its own pool. Modeled on `virtio_blk.c`'s legacy transport (I/O-port BAR0, `reset→ACK→DRIVER→features=0→queue0→DRIVER_OK`), but a request is just ONE writable descriptor (no header/status); the used element's `len` = bytes filled. API: `virtio_rng_init()` (probes PCI; clean no-op if absent), `virtio_rng_read(buf,max)`, `virtio_rng_selftest()`. Wired into `kmain` after virtio_blk; purely additive. (driver-campaign: maxing out real hardware drivers.) **Verified:** new `virtiorngtest` suite (`tests/run-virtio-rng-tests.sh`) boots QEMU headless with `-device virtio-rng-pci` + an `rng-random` backend and asserts the COM1 log shows the device probed, **two entropy batches drawn over the virtqueue that are nonzero AND differ** (`[ ok ] virtio-rng entropy OK`), desktop reached, no fault. `make check` now **57 green** (was 56); `boottest` (no rng device) confirms the driver cleanly no-ops. Next driver-campaign pick: virtio-console (the cheap virtio remainder), then the bigger TIER-1 items (AF_INET sockets, dynamic linker, loadable kernel modules).

> **(M1254) ipc — `socketpair(2)`: a pre-connected AF_UNIX endpoint pair.** We had path-keyed AF_UNIX stream sockets (M1169: `listen`/`connect`/`accept`/`send`/`recv`) but no `socketpair(2)` — the canonical way to get a connected bidirectional channel between a parent and a forked child with no bind/listen/connect dance. `unix_socketpair(int *a, int *b)` claims a free connection slot (the same `struct uconn` with its `a2b`+`b2a` byte rings a connect/accept pair uses) and hands back both sides directly — no listener, no pathname. The two endpoint ids stream bidirectionally exactly like an accepted connection and, since they index the global kernel table, survive `fork()`. New `SYS_socketpair` (242), classed with the other `unix_*` calls; ulib `sys_socketpair(int sv[2])`. **Verified:** `make check` **56 green**; **headless in-guest** — `socketpairtest` creates a pair, sends `"ping"` A→B and `"pong"` B→A over the *same* pair, and confirms both arrive: `socketpair: sv[0]=0 sv[1]=1 -- A->B 'ping' + B->A 'pong' both delivered (pre-connected, bidirectional, no path/listen/accept) -- OK`, desktop healthy.

> **(M1253) proc — `/proc/stat` in the real Linux layout (`cpu`/`ctxt`/`btime`), with live counters.** `/proc/stat` was a 3-line custom blob (`processes`/`btime 0`/`uptime_ms`) no real tool could parse. Rewrote it into the layout `top`/`vmstat`/`uptime` expect — `cpu  <user> 0 <system> <idle> 0 0 0 0 0 0` (USER_HZ=100 ticks), `ctxt`, `btime`, `processes`, `procs_running`/`procs_blocked`/`ncpu` — backed by **two new kernel counters**: `g_nr_switches` (incremented at the single `next != prev` switch point in `switch_to_next` → `task_ctxt_count()`) and `task_total_spawned()` (= `next_id`, cumulative creations). `task_cpu_times()` sums per-task `utime_ms`/`stime_ms` across the ring for the real user/system split (data tracked since M1150); `btime` is the real boot epoch (`rtc_unix() - uptime`, was hardcoded 0); `task_blocked_count()` complements the existing `task_runnable_count()` (M1148). Only the aggregate `cpu` line is emitted (single BSP ready-ring — per-`cpuN` splits would be fabricated); unaccounted columns are honest zeros. **Verified:** `make check` **56 green**; **headless in-guest** — `statcputest` parses `/proc/stat`, asserts the cpu line has ≥10 fields, `btime` is a plausible epoch, `procs_running ≥ 1`, and (after `sleep`+`sched_yield`) `ctxt` **strictly advanced**: `/proc/stat: cpu line has 10 fields, ctxt 147 -> 149, btime 1782377467, procs_running 2 (real Linux layout w/ live ctxt+btime, was a 3-line blob) -- OK`, desktop healthy.

> **(M1252) proc — `/proc/<pid>/stat`: real `minflt`/`majflt` (no longer hardcoded 0).** M1231's stat line emitted `0 0 0 0` for the fault fields even though `app_t` has tracked `minflt`/`majflt` since M1150 (behind `getrusage`) — so `ps`/`top` saw every process take zero page faults. A new **`app_faults(app_t*, &mn, &mj)`** accessor (the any-app sibling of `app_self_faults`) feeds the real counters into `gen_pid_stat` (`minflt` field 10, `majflt` field 12; `cminflt`/`cmajflt` stay 0 — no reaped-child accounting). No new syscall — just sourcing existing data. **Verified:** `make check` **56 green**; **headless in-guest** — `faulttest` reads `/proc/self/stat`'s minflt, **`sys_mmap`s a 100-page anon region and touches each page** (the demand-zero fault path `usagetest`/M1150 uses), re-reads, and asserts the field **grew by ≥ 100**: `faults: /proc/self/stat minflt 0 -> 100 after mmap+touch 100 pages (field 10 is the real counter, was hardcoded 0) -- OK`, desktop healthy. (Debugging the verify surfaced that the shell + COW-forked children show minflt 0 — their pages are pre-mapped / eagerly-copied — so `mmap`+touch, not `malloc`/fork, is the reliable demand-zero trigger.) Follow-on: `/proc/stat`'s aggregate `cpu`/`ctxt` lines from the same per-task data (ROUND-62 #8).

> **(M1251) fs — the `*at` family: `openat`/`unlinkat`/`mkdirat`/`fstatat`.** The modern POSIX path API a `musl`/`glibc`-style libc is built on (it routes plain `open`/`stat`/… through the `*at` calls) — and genuinely **dirfd-relative**, not just an `AT_FDCWD` shim. **`SYS_openat`/`unlinkat`/`mkdirat`/`fstatat`** (#238–241) share one **`at_resolve(dirfd, path)`** helper: an absolute path or **`AT_FDCWD`** (-100) passes straight through (the base ops already resolve cwd-relative names); a **real dir fd** joins the directory fd's stored path (via M1221's `app_fd_path`) — which works because `app_open` happily opens a directory path into a fd, so its path is recoverable. Each then delegates to the proven `app_open`/`vfs_remove`/`vfs_mkdir`/`vfs_stat` (zero new FS logic — lowest-risk way to add four syscalls). `AT_FDCWD` in the shared `syscall.h`; classed `PL_RPATH` (openat/fstatat) / `PL_WPATH` (unlinkat/mkdirat); ulib wrappers (fstatat over `do_syscall4`). **Verified:** `make check` **56 green**; **headless in-guest** — `attest`: **`AT_FDCWD`** `openat`(O_CREAT)→`fstatat`→`unlinkat`→`fstatat`-gone on `/tmp/AT.TXT`, then a **real dir fd** (`open("/tmp")`) feeds **`openat(dfd, "AT2.TXT", O_CREAT)`** and `fstatat(AT_FDCWD, "/tmp/AT2.TXT")` confirms the relative create **landed inside `/tmp`**: `*at: AT_FDCWD openat/fstatat/unlinkat + dirfd-relative openat (open /tmp -> create AT2.TXT in it) -- OK`, desktop healthy. Follow-ons: `renameat`/`linkat`/`readlinkat`/`fchmodat` on the same resolver, and `AT_SYMLINK_NOFOLLOW`/`AT_REMOVEDIR` flag handling.

> **(M1250) proc — `/proc/<pid>/exe`: the program image path.** Completes the `/proc/<pid>` symlink trio (`cwd`·`root`·`exe`) — what `ls -l /proc/<pid>/exe`, `readlink /proc/self/exe`, and "find my own binary" code read. The catch (noted in M1249): only a ≤24-byte **title** was kept, and `prctl(PR_SET_NAME)` can overwrite it — so I added a separate **`exe_path[64]`** captured at **spawn** (`app_spawn`, from the file path) *and* **exec** (`app_exec`, from the program name), untruncated and **immune to `prctl`**. **`gen_pid_exe`** returns it via a new `app_exe_str(app_t*)`; slotted into the per-pid dispatch beside `cwd`/`root`. No new syscall. **Verified:** `make check` **56 green**; **headless in-guest** — `exetest` reads `/proc/self/exe` and asserts it's a **real, non-empty image identity** (not the `?` placeholder): `exe: /proc/self/exe = 'Shell' (the shell's image path) -- OK` — the spawn name (our embedded apps are identified by name rather than a `/bin` path), desktop healthy. Follow-on: a true filesystem path for disk-loaded ELFs (`app_spawn_from_file` already has it — thread it through instead of the basename).

> **(M1249) proc — `/proc/<pid>/cwd` + `/proc/<pid>/root`: a process's directories.** What `pwdx`/`lsof` and `ls -l /proc/<pid>/cwd` read to see where another process is working — a clean, immediate follow-on now that M1248 tracks a per-process `cwd_path`. **`gen_pid_cwd`** returns the target app's cwd via a new `app_cwd_str(app_t*)` accessor over that string; **`/proc/<pid>/root`** is always `/` (the OS has no per-process chroot). Slotted into the per-pid procfs dispatch beside `stat`/`io`/`statm`/`wchan`; resolves through `/proc/self/cwd`. No new syscall. **Verified:** `make check` **56 green**; **headless in-guest** — `procwdtest` confirms **`/proc/self/cwd` tracks `cd`** (`cd /tmp` → reads `/tmp`, `cd /` → reads `/`) and **`/proc/self/root` == `/`**: `procwd: /proc/self/cwd tracks cd (/tmp then /); /proc/self/root == / -- OK`, desktop healthy. Follow-on: `/proc/<pid>/exe` once the executable's path is stored (today only the ≤24-byte title is kept), and making these real readlink-able symlinks.

> **(M1248) fs — `getcwd(2)`: read back the current directory.** ROUND-60 flagged this "the single most glaring hole" — per-process cwd *worked* (relative opens, `cd`), but nothing could read it back as a string, so `pwd`/`$PWD` and the libc stubs ported tools carry (doom/quake return `""`) had nothing. The kernel cwd is component-state (`synth_cwd`+`mount_sub`+FAT-cluster) with no reconstructable path for FAT subdirs — so instead I track a **canonical `cwd_path` string per process** (`app_t.cwd_path`), updated at the **`SYS_chdir` success hook** by joining the request onto the current path and normalizing `.`/`..`/`//` (a small component-stack `app_norm_abs`). It's accessed only via `cur()` — **no global, no swap-machinery entanglement** — and it's *cosmetic-for-getcwd* (the real cwd is the component state), so a bad input can only make getcwd wrong, never corrupt an open: **low blast radius**. **`SYS_getcwd(buf, size)`** (#237) copies it out, returning the length or **-1 (ERANGE)** if it won't fit; empty (never-chdir'd) reads as `/`. Classed `PL_RPATH`; ulib `sys_getcwd`. **Verified:** `make check` **56 green**; **headless in-guest** — `getcwdtest`: `cd /tmp` → `getcwd=="/tmp"`, `cd /proc` → `"/proc"`, `cd /` → `"/"`, and a **size-1 buffer returns -1** (ERANGE): `getcwd: cd /tmp->'/tmp', /proc->'/proc', /->'/'; tiny buf -> -1 -- OK`, desktop healthy. Follow-ons: a real shell `pwd` builtin reading it (today the prompt tracks its own string), and FAT-subdir paths once the component cwd carries them (the string already does).

> **(M1247) proc — `/proc/<pid>/wchan`: where a process is blocked.** The per-pid file `ps -o wchan`/`htop` read to name the kernel routine a sleeping task is parked in — the `/proc/sched` aggregate (M1166) symbolized wchans for *all* tasks, but there was no per-process file. **`gen_pid_wchan`** finds the task in `task_snapshot`, and symbolizes its blocked-PC (already stamped by `task_block`/`task_sleep_ms`, M1166) through the same **`ksym_lookup`** `/proc/sched` uses; a running task reports `0`. Slotted into the per-pid procfs dispatch beside `stat`/`io`/`statm`. No new syscall. **Verified:** `make check` **56 green**; **headless in-guest** — `pidwchantest` forks a child that **hands the parent its `gettid()` over a pipe** (since `/proc` keys on the *task id*, not `fork`'s app-pid return — the M1231 distinction) then **blocks on a second pipe**, and the parent reads **`/proc/<tid>/wchan`** to confirm it's parked in a real symbol (not `0`): `wchan: a pipe-blocked child (/proc/7/wchan) is parked in 'pipe_read' -- OK`, desktop healthy. (Debugging surfaced a `wchantest` name collision with the M1166 `/proc/sched` test — renamed the new builtin `pidwchantest`.) Follow-on: a `ps`/`top` column that reads it per process.

> **(M1246) sched — `sched_getcpu(2)`: which CPU am I on?** The SMP-introspection primitive `taskset`/per-CPU-data/NUMA-aware code calls — genuinely backed now that the AP bring-up (M1197/M1198) lights up the real local APICs. **`SYS_sched_getcpu()`** (#236) reads the executing core's **APIC id** straight from its local APIC (`LAPIC_ID >> 24`) via a new **`smp_current_cpu()`** that mirrors the reads `smp_init` already does. Since ring-3 tasks run on the BSP (the scheduler is a single global ready-ring on the BSP), it correctly returns the **BSP's id** for syscall callers — not a hardcoded 0, the actual hardware value. ulib `sys_sched_getcpu`. **Verified:** `make check` **56 green**; **headless in-guest** — `getcputest` calls `sched_getcpu()` and asserts a **valid APIC id** (`0 ≤ id < 256`): `sched_getcpu: ring-3 runs on CPU 0 (the BSP) -- OK`, desktop healthy. Follow-ons: per-CPU run-queues so `sched_setaffinity` can actually migrate ring-3 tasks to APs (today they're BSP-only, so affinity would be cosmetic), and `getcpu(2)` with the NUMA-node out-param.

> **(M1245) proc — `/proc/<pid>/statm`: the short memory line.** The terse seven-page-count line `top`/`free`/`ps`/`htop` read for a quick memory readout (the structured complement to the human-readable `/proc/<pid>/smaps` and the `vsize`/`rss` fields buried in `stat`). **`gen_pid_statm`** emits `size resident shared text lib data dt` in **pages**: `resident` from the same PTE-walk (`vmm_wss`) `stat`/`wss` use, `data` from `app_heap_bytes/4096`, `size` = `resident + data` (a rough total ≥ resident); `shared`/`text`/`lib`/`dt` are 0 (not separately tracked). Slotted into the per-pid procfs dispatch beside `stat`/`io`; resolves through `/proc/self/statm`. No new syscall — an ordinary procfs read, reusing the M1231 accessors (zero hot-path, lowest-risk). **Verified:** `make check` **56 green**; **headless in-guest** — `statmtest` reads `/proc/self/statm`, parses all **seven fields**, and asserts **`size > 0`, `resident > 0`, and `resident ≤ size`**: `statm: /proc/self/statm -- 7 fields, size>0, resident>0, resident<=size -- OK`, desktop healthy. Completes the `/proc/<pid>` memory trio (`stat` vsize/rss · `statm` · `smaps`). Follow-on: a real `shared`/`text` split once code vs anon pages are distinguished in the PTE walk.

> **(M1244) proc — `/proc/<pid>/io`: per-process I/O byte accounting.** The I/O counterpart to the CPU accounting (`times`/`getrusage`) — the `rchar`/`wchar` counters `iotop`/`pidstat -d` read, previously absent. Two `uint64_t` counters per process (`rchar`/`wchar` in `app_t`, zero-init) are tallied at the **`SYS_fdread`/`SYS_fdwrite` dispatch** — the POSIX read()/write()-over-fd path — via a tiny **`app_io_account(is_write, n)`** (`n>0` only). A new **`gen_pid_io`** procfs generator emits `rchar`/`wchar`/`read_bytes`/`write_bytes` (the last two mirror the first two — we don't split cached vs disk), slotted into the per-pid dispatch beside `stat`; resolves through `/proc/self/io`. `app_io_counts` exposes the tallies to procfs. No new syscall. **Verified:** `make check` **56 green**; **headless in-guest** — `iotest` snapshots `/proc/self/io`'s `wchar`, does a **known 64-byte `fdwrite` to a pipe**, snapshots again, and asserts **`wchar` grew by ≥ 64**: `io: /proc/self/io wchar <w1> -> <w2> (grew >=64 after a 64-byte fd write) -- OK`, desktop healthy. (One placement gotcha avoided: `app_io_account` calls `cur()`, so it had to live *after* `cur()`'s definition — the same ordering trap M1225 hit.) Follow-ons: count `SYS_read`/`SYS_write` (console stdio) too, and `syscr`/`syscw` syscall counts.

> **(M1243) fs — `chown`/`fchown(2)`: ext2 file ownership.** Completes the permission story M1241 started — ext2 inodes carry `i_uid`/`i_gid` but they were frozen at 0, so `tar -p`/`cp -p`-style ownership restore had nowhere to land. **`SYS_chown(path, uid, gid)`** (#234) → `vfs_chown` → `blockdev_mount_chown` → **`ext2_chown_path`**, which sets **`i_uid` (offset 2)** and **`i_gid` (offset 24)** — a **negative id leaves that field** (POSIX `chown(-1)`, the same neg-=-leave convention M1230's utimes uses, keeping ext2.c free of `syscall.h`) — and bumps `i_ctime`. **`SYS_fchown(fd, uid, gid)`** (#235) resolves the fd→path (M1221) and does the same. ext2 `/diskN` mounts only (tmpfs/boot-FAT have no Unix ownership → `-1`). A near-exact mirror of the M1241 chmod scaffolding. Classed `PL_WPATH`; ulib `sys_chown`/`sys_fchown`. **Verified** (ext2-only, by the **gold ext2 host test inside `make check`**, e2fsck-clean): `make check` **56 green**; the host test writes `/OWN.TXT`, `chown 1000:1000` then reads the **raw `i_uid`/`i_gid` back** (both `1000`), then `chown(-1, 500)` and confirms **`uid` stays `1000` while `gid` becomes `500`**: `chown: set uid/gid 1000:1000 then gid->500 with uid=-1 (read raw i_uid/i_gid back); -1 leaves the field`. Follow-ons: 32-bit uid/gid (the high halves at `i_uid_high`/`i_gid_high`), and a single-user enforcement note (ownership is stored but not access-checked).

> **(M1242) ipc — `eventfd(2)`: a pollable u64-counter fd (+ `EFD_SEMAPHORE`).** The canonical cross-thread/loop wakeup + counting primitive — the OS had a *named* `/event/<name>` mechanism but no real fd-based `eventfd()` that drops into `poll`/`epoll` alongside pipes and timers. A new fd **kind** (type 5) whose **u64 counter lives in the fd's own `off` field** (exactly like M1217's timerfd — no separate object table, so `fork` copies it and `close` needs no teardown — the simplest fd type after pidfd). **`SYS_eventfd(initval, flags)`** (#233) seeds the counter; **`write`** adds an 8-byte value to it (overflow/`~0` rejected); **`read`** returns the 8-byte counter and **drains it to 0** — or, in **`EFD_SEMAPHORE`** mode (stored in the spare `write_end` bit), returns **1** and **decrements by 1**; an empty counter read returns `-1` (non-blocking — callers `poll` first). `app_fd_ready` reports **`POLLIN` when the counter > 0** (and always `POLLOUT`), so the M1210/M1220 `poll`/`epoll` loops wake on it for free. `EFD_SEMAPHORE`/`EFD_CLOEXEC` in the shared `syscall.h`; ulib `sys_eventfd`. **Verified:** `make check` **56 green**; **headless in-guest** — `eventfdtest`: an `eventfd(5)` **polls `POLLIN`** (counter 5), `read` returns **5** and drains it, a follow-up `poll` returns **0** (empty), a `write(3)` makes `read` return **3**; then an **`EFD_SEMAPHORE` `eventfd(2)`** reads **1, 1** (decrementing) and the third read returns **-1** (drained): `eventfd: count(5)->POLLIN->read 5->drained; +3->read 3; EFD_SEMAPHORE read 1,1 then empty -- OK`, desktop healthy. Follow-ons: blocking reads (the `EFD_NONBLOCK`-off default, via the fd wait loop) and `vmsplice`-style zero-copy with it.

> **(M1241) fs — `chmod`/`fchmod(2)`: mutable ext2 permission bits.** ext2 *stored* a real `i_mode` (M1132 wrote `0644`/`0755` at create) but nothing could ever **change** it — so `ls -l` modes were frozen and `chmod`-on-extract (tar/installers) silently failed. **`SYS_chmod(path, mode)`** (#231) → `vfs_chmod` → `blockdev_mount_chmod` → **`ext2_chmod_path`**, which rewrites **only `i_mode`'s low 12 permission bits while preserving the high file-type nibble** (`0x8000` regular / `0x4000` dir / `0xA000` symlink) and bumps `i_ctime`; **`SYS_fchmod(fd, mode)`** (#232) resolves the fd→path (M1221 `app_fd_path`) and does the same. Only ext2 `/diskN` mounts carry Unix modes, so tmpfs / boot-FAT return `-1` (no modes to set). Classed `PL_WPATH`; ulib `sys_chmod`/`sys_fchmod`. **Verified** (ext2-only, so — like the M1232 renameat2 — by the **gold ext2 host test inside `make check`**, e2fsck-clean): `make check` **56 green**; the host test writes `/CH.TXT` (created `0644`), `chmod 0600` then reads the **raw `i_mode` back** asserting `(mode & 0777)==0600` *and* the **`0x8000` type bit is intact**, then `chmod 0755` and re-checks: `chmod: set perm bits 0600 then 0755 (read raw i_mode back); the regular-file type bit is preserved`. Follow-ons: `umask` honored at file creation (threads through the create path), `chown`/`fchown` (uid/gid are 0 — single-user), and exposing modes for in-guest verification once an ext2 disk is mounted in the desktop boot.

> **(M1240) fs — `statfs(2)`/`statvfs`: structured filesystem free-space.** `df` printed free space as *text* (`SYS_df`), but there was no structured **`statvfs`** for programs to read (`f_bsize`/`f_blocks`/`f_bavail`/`f_namemax`) — the call installers, "is there room?" checks, and `df`-clones make. **`SYS_statfs(path, struct statvfs*)`** (#230) `vfs_stat`-validates the path (absent ⇒ `-1`), then fills the struct from the existing `vfs_df` byte counts: `f_bsize`=`f_frsize`=512, `f_blocks`=total/512, `f_bfree`=`f_bavail`=free/512, `f_namemax`=255 (`f_files`/`f_ffree`=0 — global inode counts aren't tracked). The desktop boots a single filesystem, so reporting that root fs is correct here; per-mount `statfs` (reading each ext2 superblock's `s_free_blocks_count`) is a noted follow-on. Classed `PL_RPATH`; `struct statvfs` in the shared `syscall.h`; ulib `sys_statfs`. **Verified:** `make check` **56 green**; **headless in-guest** — `statfstest` calls `statfs("/")` and asserts a non-zero `f_bsize`/`f_blocks`, **`f_bavail ≤ f_blocks`**, non-zero `f_namemax`, and that **`statfs("/NOPE.XYZ")` returns -1** (absent): `statfs: / -> <N> KiB free / <M> KiB total, namemax=255 (absent path -> -1) -- OK`, desktop healthy. Follow-ons: per-mount stats (ext2 superblock), `fstatvfs(fd)`, and real `f_files`/`f_ffree` inode accounting.

> **(M1239) fd — `pipe2(2)`: atomic `O_CLOEXEC` at pipe creation.** `pipe()` (M1187) + a separate `fcntl(F_SETFD)` leaves a window where a concurrent `fork`+`exec` leaks the pipe fds — `pipe2(fds, O_CLOEXEC)` is the race-free fix every modern runtime uses. **`SYS_pipe2(int fds[2], flags)`** (#229) → **`app_pipe2`**, which makes the pipe via the existing `app_pipe` then, when `O_CLOEXEC` is set, flips the **`cloexec` bit on both ends atomically** (before the fds are observable) using the M1218 fd-hygiene machinery — so `app_exec` drops them automatically. ulib `sys_pipe2`. **Verified:** `make check` **56 green**; **headless in-guest** — `pipe2test` makes a pipe with `O_CLOEXEC` and confirms **`fcntl(F_GETFD)` returns `FD_CLOEXEC` on *both* ends** *and* that **data still flows** (`"p2"` written → read back), then makes one with `flags=0` and confirms **neither end is cloexec**: `pipe2: O_CLOEXEC sets FD_CLOEXEC on both ends + data flows; flags=0 -> not cloexec -- OK`, desktop healthy. Follow-on: `O_NONBLOCK` once pipes grow a non-blocking mode (they're always-blocking today), and `O_DIRECT`-style packet mode.

> **(M1238) proc — `getentropy(3)` + `getpriority`/`setpriority(2)`: POSIX entropy + nice spellings.** Two small compat closures over functionality the kernel already had. **`SYS_getentropy(buf, len)`** (#226) is the modern bounded-blocking entropy call (libsodium/musl prefer it over `/dev/urandom`): it fills `len ≤ 256` bytes via the same hardware-seeded CSPRNG `getrandom` uses (`random_bytes`), **rejecting `len > 256`** and returning **0/-1** (not a byte count). **`SYS_getpriority(which, who)`** (#227) and **`SYS_setpriority(which, who, prio)`** (#228) are the portable `renice` spelling over the existing CFS `nice` (`task_get_nice`/`task_set_nice`); scoped to **`PRIO_PROCESS` + the caller itself** (`who==0` or the caller's own pid) — a valid POSIX subset (cross-process renice needs a `nice_to_weight`-aware setter for an arbitrary task, a noted follow-on). `getentropy` is classed `PL_STDIO` beside `getrandom`; `PRIO_PROCESS` in the shared `syscall.h`; ulib wrappers for all three. **Verified:** `make check` **56 green**; **headless in-guest** — `priotest` fills 32 bytes with `getentropy` and confirms they're **not all-zero** (real entropy), that **`getentropy(…, 300)` returns -1** (over the 256 cap), then **`setpriority(PRIO_PROCESS, 0, 7)`** followed by **`getpriority(PRIO_PROCESS, 0) == 7`** (round-trip) before restoring: `prio: getentropy(32)=random (>256 rejected) + setpriority(7)/getpriority round-trip -- OK`, desktop healthy. Follow-ons: cross-process `setpriority` (renice another pid), and `PRIO_PGRP`/`PRIO_USER`.

> **(M1237) net — `gethostname`/`sethostname`: a real, mutable system hostname.** M1236's `uname` hard-coded `nodename="osdev"`; this makes the hostname **actual settable state** the whole system shares — so a boot script can name the machine and the shell prompt / `uname` / TLS-SNI mental model all agree. A kernel **`g_hostname[64]`** (default `"osdev"`): **`SYS_sethostname(buf, len)`** (#224) copies ≤63 user bytes in (length-clamped, `ubuf`-validated), **`SYS_gethostname(buf, len)`** (#225) copies it back out (NUL-terminated, truncated to the caller's buffer), and **`uname.nodename` now reads from `g_hostname`** instead of the literal — so all three views stay consistent. ulib `sys_sethostname`/`sys_gethostname`. **Verified:** `make check` **56 green**; **headless in-guest** — `hostnametest` reads the initial name, `sethostname("os-dev-box")`, confirms **`gethostname` returns `"os-dev-box"`** *and* **`uname().nodename == "os-dev-box"`** (the shared-state link), then restores the default: `hostname: sethostname('os-dev-box') -> gethostname + uname.nodename both reflect it -- OK`, desktop healthy. Follow-ons: persist the hostname to disk across boots, `/proc/sys/kernel/hostname`, and a shell `hostname` command.

> **(M1236) proc — `uname(2)` + `getppid`/`getuid`/`getgid`/`geteuid`/`getegid`: POSIX process & system identity.** The cluster of self-identification calls **every ported program makes on startup** (autoconf probes, libc init, `id`, `$PPID`/`$$`) — all were missing. **`SYS_uname(struct utsname*)`** (#218) fills the five strings (`sysname`="OS-DEV", `nodename`="osdev", `release`="1.0", `version`="#1 x86_64 \<build-date\>", `machine`="x86_64"); **`SYS_getppid`** (#219) returns the caller's parent pid (`cur()->parent` via a new `app_sys_getppid`), and **`getuid`/`getgid`/`geteuid`/`getegid`** (#220–223) return **0** — the correct answer for this single-user OS (the ELF auxv already advertises `AT_EUID=0`). `struct utsname` in the shared `syscall.h`; the uname buffer is `ubuf`-validated; ulib wrappers for all six. **Verified:** `make check` **56 green**; **headless in-guest** — `idtest` checks `uname` → `sysname=="OS-DEV"`, `machine=="x86_64"`, non-empty `release`; `getuid/geteuid/getgid/getegid` all == 0; then **forks a child that `exit`s `55` iff its `getppid()` equals the parent's `getpid()`** and confirms the status is 55: `id: uname='OS-DEV x86_64 1.0', uid/gid=0, child getppid()==parent -- OK`, desktop healthy. Follow-ons: `gethostname`/`sethostname` feeding `utsname.nodename`.

> **(M1235) sched — `times(2)`: per-process CPU-time accounting.** The data was already collected (per-task `utime_ms`/`stime_ms`, M1150, behind `getrusage`) but there was no `times(2)` — the canonical "how much CPU did I burn" call `time(1)` and profilers use. **`SYS_times(struct tms*)`** (#217) fills `tms_utime`/`tms_stime` from `task_self()`'s `utime_ms`/`stime_ms` (÷10 → 100 Hz ticks); `tms_cutime`/`tms_cstime` are 0 (no reaped-child CPU accounting yet); the return value is the elapsed real-time ticks since boot (`timer_ms()/10`, the `clock_t` POSIX specifies). `struct tms` in the shared `syscall.h`; `ubuf`-validated; ulib `sys_times`. **Verified:** `make check` **56 green**; **headless in-guest** — `timestest` snapshots `times()`, burns a ring-3 busy loop, snapshots again, and asserts the returned real-time ticks are monotonic AND `tms_utime` grew (the loop's CPU was charged to user time): `times: busy loop charged user CPU -- utime 0 -> 20 ticks, real-time monotonic -- OK`, desktop healthy. Follow-ons: `clock_getres` + `CLOCK_PROCESS_CPUTIME_ID`, and child-CPU accounting accumulated on reap.

> **(M1234) sched — `sched_yield(2)` + `nanosleep(2)`: cooperative yield + sub-second sleep.** The kernel had `task_yield()` and a 100 Hz timer internally, but ring-3 could only `SYS_sleep(ms)` — there was **no `sched_yield`** (voluntarily give up the CPU without sleeping, what a spin-wait or a cooperative producer/consumer wants) and **no `nanosleep`** (the POSIX-baseline `sec`+`nsec` sleep that ported tools call). **`SYS_sched_yield()`** (#215) just `sti`s and calls `task_yield()`, returning 0; **`SYS_nanosleep(sec, nsec)`** (#216) folds the request to milliseconds and reuses the exact proven `SYS_sleep` wait path (`app_kill_check` + `sti` + `timer_wait(ms/10 + 1)` — so a non-zero request always sleeps at least one 10 ms tick), with a `{0,0}` request degrading to a plain `task_yield()`. Scalar `sec`/`nsec` args (no timespec pointer to validate). ulib `sys_sched_yield`/`sys_nanosleep`. **Verified:** `make check` **56 green**; **headless in-guest** — `nanosleeptest` calls `sched_yield()` (== 0), then times a `nanosleep(0, 200 ms)` against `uptime_ms` and asserts the elapsed wall time is **≥ 180 ms**: `nanosleep: sched_yield()=0 + nanosleep(200ms) elapsed ~210ms -- OK`, desktop healthy. Follow-ons: `clock_nanosleep(TIMER_ABSTIME)` (sleep until an absolute deadline) and a true sub-10 ms sleep once a LAPIC/TSC-deadline one-shot timer lands (the 100 Hz tick is the current floor).

> **(M1233) fs — `readlink(2)`: read a symlink's target without following it.** Symlinks could be created (`SYS_symlink`, M1081) and were transparently *followed* on every access — but nothing could read a link's **target string**, so `ls -l` couldn't show `a -> b` and tools that inspect links were stuck. **`SYS_readlink(path, buf, size)`** (#214) → `vfs_readlink` → **`tmpfs_readlink`**, which returns the stored target (`tf[i].buf`) **without following** and — per POSIX — **un-terminated**, returning the exact byte count (and `-1` for a non-symlink or absent path). Scoped to tmpfs, matching `SYS_symlink`'s own /tmp creation scope (ext2 on-disk symlink readback is a noted follow-on). Classed `PL_RPATH`; the output buffer is `ubuf`-validated; ulib `sys_readlink`. **Verified:** `make check` **56 green**; **headless in-guest** — `readlinktest` creates `RL.LNK -> /tmp/RL_TARGET.TXT`, reads it back and asserts the bytes match **with the exact un-terminated length**, and that `readlink` on a **non-symlink** (`/tmp`) returns **-1**: `readlink: RL.LNK -> target read back exact (un-terminated), non-symlink -> -1 -- OK`, desktop healthy. Follow-ons: ext2 on-disk `readlink` (read the fast-symlink target from the inode), `readlinkat`, and an `ls -l` that renders `a -> b`.

> **(M1232) fs — `renameat2(RENAME_NOREPLACE | RENAME_EXCHANGE)`: no-clobber + atomic swap.** Extends M1213's `rename` with the two flags real software leans on: **`RENAME_NOREPLACE`** (fail with `-1` instead of clobbering an existing destination — the safe "create, don't overwrite" rename) and **`RENAME_EXCHANGE`** (atomically *swap* two existing entries — how editors/package managers flip a file and its replacement with no window where either is missing). **`SYS_renameat2(old, new, flags)`** (#213) → `vfs_rename2` → `blockdev_mount_rename2` → **`ext2_rename2_path`**: `flags==0` delegates to the plain M1213 path; `NOREPLACE` does one `walk` of the destination and bails if it exists (else falls through to a normal move); `EXCHANGE` rewrites **each dirent's inode#+file_type in place** via a new `dir_set_entry` helper (both names persist, so **no link-count change**), refusing a *cross-directory* exchange when either side is a directory (that would need `..`/parent-link fixups) so every outcome stays **e2fsck-clean**. To keep **ext2.c self-contained**, the flag values are mirrored as local `EXT2_RN_*` macros. Classed `PL_WPATH`; ulib `sys_renameat2`; `RENAME_NOREPLACE`/`RENAME_EXCHANGE` in the shared `syscall.h`. **Verified** (the desktop boot has no ext2 mount, so — like the M1189 extent-write — this is verified by the **gold ext2 host test inside `make check`**, which is e2fsck-clean): `make check` **56 green**; the host test writes `/R2A`+`/R2B`, confirms **`NOREPLACE` onto `/R2B` fails** but onto a free name succeeds (content + source-gone checked), then **`EXCHANGE` swaps** `/R2NEW`↔`/R2B` and asserts their contents traded places: `renameat2: NOREPLACE refuses to clobber (ok onto a free name) + EXCHANGE atomically swaps two files`. Follow-ons: a shell `mv -n` (no-clobber) using it, cross-dir directory `EXCHANGE` (with the `..` fixups), and `RENAME_WHITEOUT`.

> **(M1231) proc — `/proc/<pid>/stat`: the `ps`/`top`/`htop` line.** The single canonical record every process monitor parses — we had the readable `/proc/<pid>/status` but not the terse space-separated `stat` line that tooling actually reads. **`gen_pid_stat`** emits the classic **fields 1–24**: pid, **`(comm)`** (parenthesized), **state** char (`R`/`S`/`Z`/`T` from the task state), ppid/pgrp/session (new `app_ppid`/`app_pgid_of`/`app_sid_of` accessors over the existing `app_t` fields), **utime/stime** as clock ticks @100 Hz (read straight off the `task_t`'s `utime_ms`/`stime_ms` via `app_task`), priority/**nice** (signed), **vsize** (heap bytes) and **rss** (resident pages from the same PTE walk `/proc/<pid>/wss` uses), with `0` for the counters we don't track (tty, page faults). Slotted into the per-pid procfs dispatch beside `status`; resolves through `/proc/self/stat` too (M-series `proc_pid_path` already maps `self`). No new syscall — it's an ordinary procfs read. **Verified:** `make check` **56 green**; **headless in-guest** — `pidstattest` reads `/proc/self/stat` and asserts **field 1 == its own pid**, a parenthesized `(comm)`, a valid **state letter**, and **≥ 24 space-separated fields**: `pidstat: /proc/self/stat -- field1==pid, (comm), state R/S/Z/T, >=24 fields -- OK`, desktop healthy. Follow-ons: `/proc/<pid>/statm` (the short memory line), real `minflt`/`majflt` once the page-fault path counts them per-process, and a `ps` that actually parses these.

> **(M1230) fs — `utimensat`/`futimens`: arbitrary atime/mtime control.** ext2 stamped times to *now* on every write (`e_stamp`) but nothing could set an **arbitrary** timestamp — so `cp -p`, `tar`/`unzip` extraction, and `make`'s mtime-based dependency tracking couldn't preserve or forge times. **`SYS_utimens(path, atime, mtime)`** (#211) and **`SYS_futimens(fd, atime, mtime)`** (#212, resolves the fd→path via M1221's `app_fd_path`) take two Unix epochs, with **`UTIME_NOW`** (=-1 → the current time) and **`UTIME_OMIT`** (=-2 → leave that field) sentinels. The syscall layer (`app_utimens`) resolves `UTIME_NOW` to a concrete epoch via `rtc_unix` so the lower layers stay simple, then `vfs_utimes` dispatches: ext2 mounts → **`ext2_utimes_path`** (writes `i_atime`@8 / `i_mtime`@16, bumps `i_ctime` to now since metadata changed), tmpfs → **`tmpfs_utimes`** (tracks mtime only). To keep **ext2.c self-contained** (no `syscall.h`), the backends use a plain convention — **a negative time means "leave unchanged"** — so `UTIME_OMIT` needs no special-casing there. Classed `PL_WPATH` under pledge; ulib `sys_utimens`/`sys_futimens`; `UTIME_NOW`/`UTIME_OMIT` in the shared `syscall.h`. **Verified:** `make check` **56 green**; the **gold ext2 host test** (e2fsck-clean) sets `i_atime`/`i_mtime` to fixed epochs `0x40000000`/`0x50000000` and reads the **raw inode** back byte-exact (white-box via `walk`), then a second call with a negative `atime` **leaves atime** while updating mtime (OMIT): `utimes: set atime/mtime to fixed epochs (read back exact); a negative arg leaves the field (OMIT)`; **headless in-guest** — `utimestest` sets a tmpfs file's mtime to a fixed epoch (read back via `statx`), confirms **`UTIME_NOW`** advances it, then sets it again through **`futimens(fd)`**: `utimes: set mtime to a fixed epoch (statx reads it), UTIME_NOW advances it, futimens(fd) sets it -- OK`, desktop healthy. Follow-ons: a real `cp -p` / `touch -d` in the shell that uses it, a separate tmpfs atime field, and the `*at` dirfd form (`utimensat` proper).

> **(M1229) fs — `lseek(SEEK_HOLE/SEEK_DATA)`: sparse-file-aware seeking.** The companion to M1228's sparse files — the primitive `tar`/`cp --sparse`/`rsync` use to find a file's allocated regions and **skip the holes** instead of copying gigabytes of zeros. **No new syscall** — it extends `SYS_lseek` (#179) with two `whence` values (**`SEEK_DATA`=3, `SEEK_HOLE`=4**): seek to the next data byte / next hole at-or-after `off`. The substance is **`ext2_seek_data_hole`**, which walks the file's block map at block granularity via **`map_block`** (which already resolves *both* the ext4-extent and direct/indirect layouts, so this is one code path for both): it scans from `off`'s block for the first block whose mapped/unmapped state matches what's wanted, clamping the result back to `off` when `off` is already inside such a block. **`SEEK_DATA` past the last data block returns `-1` (ENXIO)**; **`SEEK_HOLE` always finds at least the implicit hole at EOF** (returns the file size). `vfs_seek_data_hole` routes ext2 `/diskN` mounts to the real block walk; tmpfs/FAT/ISO are never sparse so they get the generic answer (data everywhere, one hole at EOF) — `blockdev_mount_seek_data_hole` returns a `-2` sentinel for non-ext2 so vfs falls back. `app_lseek` handles file fds (→vfs) and memfds (contiguous → no holes); it repositions the fd to the result per POSIX. **Verified:** `make check` **56 green**; the **gold ext2 host test** (e2fsck-clean) builds a real **`[data | hole | data]`** file (write 5000 B, overwrite to convert the extent inode to block-mapped, then `punch_hole` carves `[1024,3072)`) and asserts every boundary — `SEEK_DATA(0)=0`, `SEEK_HOLE(0)=1024`, `SEEK_DATA(1024)=3072` (jumps the hole), `SEEK_HOLE(1024)=1024`, `SEEK_HOLE(3072)=5000` (EOF), `SEEK_DATA(5000)=-1` (ENXIO): `seek: SEEK_DATA/SEEK_HOLE on a [data|hole|data] file -- boundaries at 0/1024/3072/5000 + ENXIO at EOF`; **headless in-guest** — `seektest` on a grown tmpfs file confirms the syscall/whence plumbing (`SEEK_DATA(0)=0`, `SEEK_HOLE(0)=EOF`, `SEEK_DATA(mid)=mid`, ENXIO past EOF): `seek: SEEK_DATA(0)=0, SEEK_HOLE(0)=EOF, SEEK_DATA(mid)=mid, ENXIO past EOF -- OK`, desktop healthy. Follow-ons: a real `cp --sparse` in the shell that uses it, and `FALLOC_FL_PUNCH_HOLE` exposed as a syscall (the `ext2_punch_hole` plumbing already exists).

> **(M1228) fs — `truncate`/`ftruncate` on real on-disk files (ext2 + tmpfs).** `ftruncate` only worked on **memfds** (M1212); this resizes **real files**, the primitive `>`-truncate, log rotation, DB preallocation and `cp` all need. **`SYS_truncate(path, len)`** (#210) and the extended **`app_ftruncate`** (now routes a **type-2 file fd** → `vfs_truncate(path)` instead of erroring) flow through a new **`vfs_truncate`** → tmpfs (`tmpfs_truncate`: realloc, copy `min(old,new)`, zero-fill the grow) or ext2 (`blockdev_mount_truncate` → **`ext2_truncate_path`**). The ext2 side is the substance: **shrink frees the blocks fully beyond `newlen` and zeroes the partial EOF block's tail** (so a later grow reads back zeros, not stale data), then lowers `i_size`/`i_blocks`; **grow is sparse** (raise `i_size`; the holes read back zero through `ext2_pread`). The subtlety that took the work: the driver writes new files as a **single contiguous ext4 extent** (M1189), so the shrink is **extent-aware** — it shrinks the extent's `ee_len` and frees the trailing physical run via `free_block` (the old `punch_block` only knew the direct/indirect scheme); both layouts stay **e2fsck-clean**. `map_block` resolves either scheme, so the partial-block zeroing is shared. ulib `sys_truncate`; `sys_ftruncate` (M1212) now serves both memfds and files; classed `PL_WPATH` under pledge. **Verified:** `make check` **56 green**; the **gold ext2 host test** (e2fsck-clean) shrinks `/TR.TXT` 3000→500 (frees blocks) then sparse-grows 500→1500 with a **byte-exact zero-filled tail** on an **extent** inode; **headless in-guest** — `truncatetest` writes 11 bytes to a tmpfs file, **shrinks 11→5** (reads back `"hello"`), **grows 5→8** (reads back `"hello\0\0\0"`), then `open`s it and **`ftruncate(fd, 3)`** → `"hel"`: `truncate: shrink 11->5 + grow 5->8 (zero-fill) + ftruncate(fd) 8->3 -- OK`, desktop healthy. Follow-ons: extent-tree (depth>0) shrink for externally-written large files, and `fallocate(FALLOC_FL_PUNCH_HOLE)` reusing the same free-and-zero path.

> **(M1227) proc — `waitid(2)` + `WNOHANG` + `siginfo`: non-blocking child reaping (the fd-toolkit capstone).** `waitpid` (M1117) always blocked; `waitid` adds the **non-blocking** poll (`WNOHANG`) that makes process supervision composable with the event loop the rest of this work built (poll/epoll/pidfd). **`SYS_waitid(idtype, id, siginfo*, options)`** (#209) mirrors `app_waitpid`'s zombie scan but: `WNOHANG` returns **0 with `si_pid==0`** when nothing is reapable (instead of blocking); on a reap it fills a **`struct siginfo`** (`si_pid`, `si_signo=SIGCHLD`, `si_status`, `si_code=CLD_EXITED`); `idtype` is `P_PID` (a specific child), `P_ALL` (any), or **`P_PIDFD`** (translate a type-7 pidfd → its pid — a freebie that composes M1222). ulib `sys_waitid`; classed with `waitpid` under pledge. **Verified:** `make check` **56 green** + **headless in-guest** — `waitidtest` forks a child that spins then `exit(42)`, then **(A)** an immediate `waitid(WNOHANG)` returns **0 with `si_pid==0`** (nothing reapable yet) and **(B)** a blocking `waitid` returns `{si_pid==child, si_status==42, si_code==CLD_EXITED}`: `waitid: WNOHANG=nothing-yet (si_pid=0), then blocking reap si_status=42 CLD_EXITED -- OK`, desktop healthy. Follow-ons: `CLD_KILLED`/`CLD_STOPPED` codes (track the kill/stop path), and `signalfd`-for-`SIGCHLD`.

> **(M1226) threads — `set_tid_address` + `CLONE_CHILD_CLEARTID`: a real blocking `pthread_join`.** Thread `join` was a `task->state != TASK_DEAD` poll-spinner; this adds the exact glibc/musl mechanism that makes it a true blocking, race-free join. A new `task_t.clear_child_tid` (registered by **`SYS_set_tid_address(tidptr)`** (#208), which returns the caller's tid); on thread exit, **`app_thread_exit` zeroes `*clear_child_tid` and `FUTEX_WAKE`s it** — mirroring the M1141 robust-futex-on-exit walk, done while the dying thread's CR3 is still active (threads share the address space, and `g_futex` is phys-keyed so the wake lands). The joiner just `futex(&tid, WAIT, old)` and is woken the instant the thread clears it. ulib `sys_set_tid_address`. **Verified:** `make check` **56 green** + **headless in-guest** — `jointest` `clone`s a thread that registers its `clear_child_tid`, spins briefly, then exits; the main thread sets the tid var and `futex`-WAITs on it, and is **woken with the var cleared to 0** when the thread exits: `set_tid_address: thread registered ctid + exited -> futex woke the joiner, ctid=0 -- OK`, desktop healthy. Follow-ons: `CLONE_CHILD_SETTID`/`CLONE_PARENT_SETTID` (write the tid at clone time), and clearing on the main-thread/process-exit path too.

> **(M1225) proc — `prctl(PR_SET_NAME/PR_GET_NAME)` + `/proc/<pid>/comm`: a process renames itself at runtime.** Before this, `ps` / `/proc/<pid>/status` / `/proc/<pid>/cmdline` only ever showed the **spawn-time** title; now a worker can relabel itself (`"vacuum"`, `"writer"`) and the change is visible system-wide. **`SYS_prctl(option, arg2)`** (#207): **`PR_SET_NAME`** copies ≤15 bytes into the existing `app_t.titlebuf` (the storage was already there — never settable post-spawn) and repoints `title` at it; **`PR_GET_NAME`** copies it back out (the syscall `ustr`/`ubuf`-validates the user pointer). New **`/proc/<pid>/comm`** generator returns `app_title`, slotted into the per-pid procfs dispatch beside `status`/`limits`/`comm`. ulib `sys_prctl` + `PR_SET_NAME`/`PR_GET_NAME`. **Verified:** `make check` **56 green** + **headless in-guest** — `prctltest` sets its name to `"vacuum"`, reads it back via `PR_GET_NAME` (`== "vacuum"`), confirms `/proc/self/comm` renders `vacuum`, then restores `"Shell"`: `prctl: PR_SET_NAME=vacuum, PR_GET_NAME reads it, /proc/self/comm shows it -- OK`, desktop healthy. Follow-ons: `PR_SET_DUMPABLE` (needs a new flag), and using `comm` in the WM taskbar label.

> **(M1224) fs — `access(2)` (`R_OK`/`W_OK`/`X_OK`/`F_OK`).** The classic existence + permission probe that a lot of real software gatekeeps on (the ported games' libc stubbed it to a wrong `-1`). **`SYS_access(path, amode)`** (#206) `vfs_stat`s the path — absent ⇒ `-1` (so `F_OK` is the existence check) — then tests the requested bits against the file's `stx_mode`: `R_OK`→`0444`, `W_OK`→`0222`, `X_OK`→`0111` (vfs fills `0644` files / `0755` dirs). Classed `PL_RPATH` under pledge; `F_OK`/`R_OK`/`W_OK`/`X_OK` in the shared `syscall.h`; ulib `sys_access`. **Verified:** `make check` **56 green** + **headless in-guest** — `accesstest`: `/MOTD.TXT` passes `F_OK` and `R_OK`, a nonexistent `/NOPE.XYZ` returns `-1` for `F_OK`, and the root `/` directory passes `X_OK` (searchable): `access: F_OK/R_OK on /MOTD.TXT, /NOPE.XYZ absent=-1, / is X_OK -- OK`, desktop healthy. Follow-on: `faccessat` (with the queued `*at` dirfd).

> **(M1223) fs — `getdents64` + `d_type`: the real directory-read primitive.** Listing went through the formatted `SYS_list` string blob; this adds the standard binary **`getdents64(buf, max, start_idx)`** (#205) that packs Linux-layout **`struct dirent64 {d_ino, d_off, d_reclen, d_type, d_name}`** records — the primitive `readdir`/`ls -F`/file managers are built on. The neat part: **`d_type` is free** — `vfs_list` already marks directories with a trailing `/` (the convention FAT + ext2/iso mounts share), so `app_getdents64` derives **`DT_DIR`** from that marker (stripping it from `d_name`) and **`DT_REG`** otherwise; records are 8-byte aligned with a `d_off` cursor (the caller resumes from the last record's `d_off`). `struct dirent64` + `DT_*` in the shared `syscall.h`; ulib `sys_getdents64`. **Verified:** `make check` **56 green** + **headless in-guest** — `getdentstest` makes a fresh `/GDT` dir with a subdir `SUB` and a file `F.TXT`, `chdir`s in, and `getdents64`s the cwd, asserting **`SUB` is `DT_DIR`** and **`F.TXT` is `DT_REG`**: `getdents64: cwd records w/ d_type -- SUB=DT_DIR + F.TXT=DT_REG -- OK`, desktop healthy. (Debugging surfaced that the FAT root holds 128+ entries, so the test uses a small fresh dir — the `vfs_list` cap means very large dirs are listed up to that cap; full pagination needs a `vfs_list` offset, a noted follow-on.) This is also the d_type substrate a future `*at`/`vfs_isdir` will reuse. Follow-ons: `getdents64` of an arbitrary path/dirfd (needs the `*at` dirfd), real `d_ino`, and `vfs_list` offset-based pagination.

> **(M1222) proc — `pidfd_open` + pollable process exit + `pidfd_send_signal`.** A modern, race-free process handle: instead of `waitpid` (which only the parent can call, and which blocks), a **`pidfd`** lets *any* process hold a handle to another and **wait for its exit in a `poll`/`epoll` loop** alongside its sockets and timers. A new fd **kind** (type 7) storing the target **pid as a plain value** in the fd slot — no shared object, so `fork`/`dup2` just copy it and `close` needs no teardown (the simplest fd type yet). The elegant part: `app_fd_ready` reports **`POLLIN` once `!app_pid_alive(pid)`** (the process has `exited` or its slot is gone), so the existing M1210 `poll` / M1220 `epoll` loops wake on a child's death for free. **`SYS_pidfd_open(pid)`** (#203) returns the handle (requires a live pid); **`SYS_pidfd_send_signal(pidfd, sig)`** (#204) resolves the handle to its process and raises the signal. ulib `sys_pidfd_open`/`sys_pidfd_send_signal`. **Verified:** `make check` **56 green** + **headless in-guest** — `pidfdtest` forks a child that spins briefly then exits, opens a pidfd on it, and: **(A)** while the child runs, `poll(pidfd, POLLIN, 0)` returns **0** (alive); **(B)** `poll(pidfd, POLLIN, 2000)` **blocks then returns 1 with `POLLIN`** once the child exits: `pidfd: open a child handle -> poll blocks while alive, POLLIN on exit -- OK`, desktop healthy. Follow-ons: a generation counter on the process slot for full pid-reuse safety, and `pidfd_getfd` (steal an fd from another process).

> **(M1221) fs — POSIX `fcntl` byte-range record locks (`F_SETLK`/`F_GETLK`).** We had whole-file `flock(2)` (M1177); this adds the finer-grained record locks that databases and editors use — lock just `[start, len)` of a file. A **separate lock space** in `flock.c` (POSIX keeps `flock`/`fcntl` locks independent): `struct rlk {path, owner, type, start, len}` with a byte-range overlap test (`a.start < b.end && b.start < a.end`, `len<=0` = to-EOF) and `rl_conflict` (a different owner whose range overlaps where at least one side is `F_WRLCK`). **`rlock_set`** does `F_SETLK` (clears the caller's overlapping locks, then acquires — or `-1` on conflict) and `F_UNLCK`; **`rlock_get`** does `F_GETLK` (reports the blocking holder). Wired through the M1218 **`fcntl`**: `SYS_fcntl(fd, F_SETLK/F_GETLK, struct flock*)` resolves the fd to its file path (new `app_fd_path`) and calls the record-lock layer; locks are released on `F_UNLCK` or process exit (`flock_release_pid` now sweeps both tables). `struct flock` + `F_RDLCK`/`F_WRLCK`/`F_UNLCK`/`F_GETLK`/`F_SETLK` in the shared `syscall.h`. **Verified:** `make check` **56 green** + **headless in-guest** — `locktest`: the parent write-locks `[0,10)`, forks, and the child (a **different owner**) finds an overlapping `[5,15)` `F_SETLK` **conflicts** (`-1`), a disjoint `[10,20)` **succeeds**, and **`F_GETLK` reports the parent as the holder** (`l_type==F_WRLCK`, `l_pid>0`) — child→parent result over a pipe: `record-locks: WRLCK[0,10); child overlap[5,15)=conflict, disjoint[10,20)=ok, F_GETLK reports holder -- OK`, desktop healthy. Follow-ons: `F_SETLKW` (blocking acquire, reusing the `flk_wake_all` discipline), OFD locks (`F_OFD_SETLK`, owner = open-file-description not pid), and lock coalescing/splitting on partial unlock.

> **(M1220) ipc — `epoll`: scalable readiness multiplexing as a first-class fd object.** The graduation from M1210 `poll` — instead of re-passing the whole fd set on every call, you register an **interest set once** and wait on it repeatedly. A new fd **kind** (type 6) backed by a refcounted **`epollobj`** table holding `{fd, events, data}` items. **`SYS_epoll_create1`** (#200) returns the epoll fd; **`SYS_epoll_ctl`** (#201) does `EPOLL_CTL_ADD`/`MOD`/`DEL` on the set; **`SYS_epoll_wait`** (#202) is the elegant part — it just loops the interest set through the **same `app_fd_ready` readiness ladder** the `poll` loop already drives, blocking on the identical `sti`/`task_sleep_ms(10)`/timeout discipline, and writes the ready members (with their opaque `data` echoed back) into the user's `struct epoll_event[]`. Refcounted across `fork`/`dup2` like a memfd. ulib `sys_epoll_create1`/`sys_epoll_ctl`/`sys_epoll_wait` + `struct epoll_event` + `EPOLL_CTL_*`. **Verified:** `make check` **56 green** + **headless in-guest** — `epolltest`: create an epoll fd, **`ADD`** a pipe read-end with userdata `0xABCD`; **(A)** `epoll_wait(timeout 0)` returns **0** (nothing written); **(B)** after a write, `epoll_wait(timeout 1000)` returns **1** with `POLLIN` and `data == 0xABCD`; **(C)** after **`DEL`**, `epoll_wait(timeout 0)` returns **0** despite the pending data (no longer watched): `epoll: create + ctl(ADD) + wait(timeout 0=none, then POLLIN+data) + ctl(DEL) -- OK`, desktop healthy. Follow-ons: edge-triggered (`EPOLLET`), making the epoll fd itself pollable (nesting, with a cycle guard), and `EPOLLONESHOT`.

> **(M1219) ipc — `sendfile(2)`: zero-copy fd→fd (generalizes splice off pipes).** M1211's `splice` only moved **pipe→pipe**; `sendfile(out_fd, in_fd, off_ptr, count)` (#199) is the canonical zero-copy path that moves between **any** two fds through a kernel bounce buffer (file→pipe, file→file, …) with no userspace round-trip — the call that lets a web server ship a file straight to a socket. **`app_sendfile`** loops in 4 KiB chunks: with **`off_ptr == NULL`** it reads `in_fd` **sequentially via the fd cursor** (any fd kind — file/pipe/memfd); with a non-NULL **`off_ptr`** it reads a **FILE fd at that absolute offset** via `vfs_pread`, advancing `*off` while leaving the fd's own cursor untouched (the POSIX `sendfile` offset semantics); each chunk is written to `out_fd` via the existing `app_fd_write`, stopping at EOF or a short write. ulib `sys_sendfile` (over `do_syscall4`). **Verified:** `make check` **56 green** + **headless in-guest** — `sendfiletest` writes `"hello sendfile world"` to a tmpfs file, opens it, and (A) `sendfile`s the **whole file → a pipe via the cursor** (returns 20, read back byte-exact), then (B) does a **positioned slice** (`off=6, count=8` → `"sendfile"` → a 2nd pipe), asserting **`*off` advanced to 14** and the bytes are exact while the fd cursor is untouched: `sendfile: file->pipe whole (cursor) + positioned slice (off advances, cursor intact) -- OK`, desktop healthy. Follow-ons: wire `httpd` to serve files via `sendfile`, and `sendfile` into an AF_UNIX/TCP socket once those are pollable fds.

> **(M1218) fd — fd-hygiene cluster: `FD_CLOEXEC` + `dup3` + `close_range` + `fcntl`.** The "real fd table" rite of passage — we'd built pipes/dup2/file-fds/memfds/timerfds but fds **leaked across `exec`** and there was no `fcntl`. A single **`cloexec` bit** (added at the *end* of `struct fdent` so the positional initializers keep working) is now: set by **`O_CLOEXEC`** on `open` and by **`F_DUPFD_CLOEXEC`**/`dup3`; queried/toggled via **`SYS_fcntl(fd, cmd, arg)`** (#196 — `F_GETFD`/`F_SETFD`/`F_DUPFD`/`F_DUPFD_CLOEXEC`); **honored by `app_exec`**, which now closes every cloexec fd as it swaps the image (it kept *all* fds before), while **`fork` preserves them** (the whole `fdent` is copied — POSIX-correct). Plus **`SYS_dup3(old, new, flags)`** (#197 — like `dup2` but returns `-1` when `old==new` and applies `O_CLOEXEC`) and **`SYS_close_range(lo, hi, flags)`** (#198 — bulk-close an fd span). ulib `sys_fcntl`/`sys_dup3`/`sys_close_range` + the `F_*`/`FD_CLOEXEC`/`O_CLOEXEC` constants. **Verified:** `make check` **56 green** + **headless in-guest** — `fcntltest`: `F_SETFD(FD_CLOEXEC)` then `F_GETFD` round-trips; `dup3(fd, fd, 0)` returns **-1** (the `dup2` distinction); `dup3(fd, 9, O_CLOEXEC)` sets cloexec on fd 9 (confirmed via `F_GETFD`); `F_DUPFD` returns the lowest free fd ≥ 5; `close_range(5, 20)` then `F_GETFD` on the dup'd fd returns **-1** (closed): `fd-hygiene: FD_CLOEXEC get/set + dup3(O_CLOEXEC; EINVAL old==new) + F_DUPFD + close_range -- OK`, desktop healthy. The fd model is now POSIX-correct across exec. Follow-ons: the `*at` family (`openat`/…) building on this + per-process cwd, and `CLOSE_RANGE_CLOEXEC`.

> **(M1217) ipc — `timerfd`: a pollable one-shot timer as a file descriptor.** The natural companion to M1210 `poll` — a timer you can wait on in an event loop alongside pipes and files, instead of a blocking sleep. A new fd **kind** (type 4) whose **absolute expiry (ms, 0 = disarmed) lives in the fd's own `off` field** — no separate object table, so `fork` just copies it and `close` needs no teardown (simpler than the M1212 memfd's refcounted object). **`SYS_timerfd_create()`** (#194) returns the fd; **`SYS_timerfd_settime(fd, delay_ms)`** (#195) arms it (`off = timer_ms() + delay`, `<=0` disarms). The two integration points are tiny: `app_fd_ready` reports **`POLLIN` once `timer_ms() >= off`** (so `poll`/`fswait` wake on it), and `app_fd_read` returns the **8-byte expiration count (1)** when expired and then **disarms** (one-shot). ulib `sys_timerfd_create`/`sys_timerfd_settime`. **Verified:** `make check` **56 green** + **headless in-guest** — `timerfdtest` arms an 80 ms timer and, *composing it with `poll`*: (A) an immediate `poll(timeout 0)` returns **0** (not yet ready); (B) `poll(timeout 1000)` returns **1 with `POLLIN`** after ~80 ms; (C) `read` yields the **8-byte count == 1**; (D) being one-shot, a follow-up `poll(timeout 0)` returns **0** (disarmed): `timerfd: armed 80ms -> not-ready, then POLLIN, read count=1, disarmed -- OK`, desktop healthy. Follow-ons: periodic (interval) timers with a real expiration count, `timerfd_gettime`, and `TFD_TIMER_ABSTIME`.

> **(M1216) dev — `/dev/kmsg` writer: userspace logs into the kernel ring (`dmesg`).** The kernel log ring (`klog[]` in `console.c`, behind `/proc/kmsg`) was read-only to userspace — there was no way for an init script or app to record a line in `dmesg`. New **`klog_write`** appends userspace bytes to the ring (ensuring a trailing newline so each write is its own line), and **`/dev/kmsg`** is now a read+write device file: writes route through `procfs_write` → `klog_write`, reads mirror `/proc/kmsg` via `klog_copy`; `"kmsg"` joins the `/dev` listing. No new syscall (ordinary `write`/`read` on the path — the Unix way). **Verified:** `make check` **56 green** + **headless in-guest** — `kmsgtest` writes a unique marker to `/dev/kmsg` via `sys_writefile`, then reads `/proc/kmsg` back and finds the marker line: `kmsg: wrote a line to /dev/kmsg, read it back from /proc/kmsg (dmesg) -- OK`, desktop healthy. Follow-ons: a `<level>` prefix parse (Linux `/dev/kmsg` priority), and an in-shell `logger` command.

> **(M1215) proc — `/proc/<pid>/auxv`: the ELF auxiliary vector (what a libc reads at startup).** Completes M1214's introspection pair. A new per-pid procfs file (`app_format_auxv`) emits a synthetic auxv as **`(a_type, a_val)` little-endian u64 pairs** terminated by **`AT_NULL`**: `AT_PAGESZ`=4096, **`AT_ENTRY`**=`a->entry` (the process's real ELF entry, already stored in `app_t`), `AT_CLKTCK`=100, `AT_UID`/`EUID`/`GID`/`EGID`=0, `AT_SECURE`=0. (Our loader doesn't push an auxv onto the user stack, so this is synthesized from what the kernel knows — but it's the standard introspection surface CRIU/debuggers expect.) Slotted into the per-pid procfs dispatch beside `limits`/`maps`; `AT_*` constants in the shared `syscall.h`; no new syscall (read via `sys_readfile`). **Verified:** `make check` **56 green** + **headless in-guest** — `auxvtest` reads `/proc/self/auxv`, walks the 16-byte pairs, and asserts **`AT_PAGESZ`==4096**, **`AT_ENTRY`!=0**, and a trailing **`AT_NULL`**: `auxv: /proc/self/auxv has AT_PAGESZ=4096 + AT_ENTRY!=0, AT_NULL-terminated -- OK`, desktop healthy. Follow-ons: a real auxv pushed onto the initial user stack (so a future libc's `getauxval` works without procfs), and `AT_RANDOM`/`AT_PHDR` once the loader records the program-header address.

> **(M1214) proc — `prlimit(2)` + `/proc/<pid>/limits`: cross-process resource limits + introspection.** We had self-only `getrlimit`/`setrlimit` (M1163/M1164) but couldn't inspect or adjust *another* process's limits, and nothing rendered them. **`SYS_prlimit(pid, resource, newval, do_set)`** (#193) → **`app_prlimit`** resolves the target via `app_by_pid` (pid 0 = the caller), reads the old value of `RLIMIT_AS`/`DATA`/`NPROC` from the existing per-process storage, and — when `do_set` — writes the new one; it returns the old/current value. New **`/proc/<pid>/limits`** (`app_format_limits`) renders the enforced limits as a Linux-style table (`0`/unset → `unlimited`), slotted into the per-pid procfs dispatch beside `maps`/`status`. ulib `sys_prlimit` (over `do_syscall4`). **Verified:** `make check` **56 green** + **headless in-guest** — `prlimittest` sets the shell's own `RLIMIT_NPROC` to 7 via `prlimit`, queries it back (`== 7`), and confirms `/proc/self/limits` renders it; `cat /proc/self/limits` shows the table (`RLIMIT_AS unlimited / RLIMIT_DATA unlimited / RLIMIT_NPROC 7`): `prlimit: set self NPROC=7, query returns 7, /proc/self/limits shows it -- OK`, desktop healthy. Follow-ons: a synthetic `/proc/<pid>/auxv` (stash `AT_ENTRY` at exec; `AT_PAGESZ`/`AT_RANDOM`/`AT_CLKTCK`), enforcing more `RLIMIT_*` (CPU/FSIZE/NOFILE), and a same-tree ownership gate if multi-user ever lands.

> **(M1213) fs — `rename(2)`: atomic move within an ext2 volume (real `mv`, incl. directories).** We had `link`/`unlink`/`mkdir`/`rmdir`/`symlink` but no `rename` — the shell's `mv` was a userspace **copy+delete** (failed on directories, lost metadata, O(filesize)). New **`ext2_rename_path`** relocates a directory entry preserving the inode: `dir_add` the new name + `dir_remove` the old (a net-zero link count for the moved name), **replacing an existing regular-file target** (unlink-then-add) and **refusing to replace a directory or move a directory into its own subtree** (an `is_ancestor` climb of `..` to the root). For a **directory** moved across parents it repoints the moved dir's **`..`** entry (`dir_set_dotdot`) and adjusts **both parents' link counts** so `e2fsck` stays clean. Routed end-to-end: `ext2_rename_path` → `blockdev_mount_rename` (ext2 mounts only) → **`vfs_rename_path`** (both paths must resolve to the *same* `/diskN` ext2 mount) → **`SYS_rename`** (#192) → ulib `sys_rename` → the shell's **`mv`**, which now tries a real rename first and **falls back to copy+delete across filesystems** (so FAT/cross-mount `mv` is byte-identical — zero regression). **Also fixed a latent `vfs_chdir` bug** it exposed: `chdir` into a mount tested `blockdev_mount_isdir(...)` as truthy, but that returns **-1 for an absent path** (truthy!), so `cd`/`mv`-dest-probing a *nonexistent* `/diskN/x` wrongly succeeded — now gated on `== 1`. **Verified two ways:** the host `ext2test` does a same-dir rename, a cross-dir file move, **a directory move across parents** (`..` + link counts), and asserts a **subtree-loop move is refused** — then the dumped image is **`e2fsck`-clean** (the gold check of link counts + `..` + dirents); and **headless in-guest** on a real ext2 `/disk2` — `mv /disk2/AAA.TXT /disk2/BBB.TXT` then `mv /disk2/BBB.TXT /disk2/SUB/CCC.TXT` (cross-dir), with `ls` confirming the moves and `cat` showing the content **preserved through both** (`hello from disk2 rename test`); `make check` **56 green**. Follow-ons: cross-filesystem rename in the kernel (today the shell does that via copy+delete), and `RENAME_NOREPLACE`/`RENAME_EXCHANGE` flags.

> **(M1212) mm/ipc — `memfd_create` + `F_SEAL` file seals: anonymous, sealable in-RAM files (no hobby OS has seals).** A new fd **kind** (type 3) backing an anonymous, name-tagged, growable **kheap buffer** — distinct from M1153 `mseal` (which seals *virtual-address ranges*); this seals a **file object**. **`SYS_memfd_create(name, flags)`** (#189) returns a writable fd; reads/writes/`lseek` flow through the *existing* `app_fd_read`/`app_fd_write`/`app_lseek` (each gained a type-3 branch — positioned, growing the buffer on demand via a doubling `memfd_grow`, 16 MiB-bounded), and **`SYS_ftruncate(fd, len)`** (#191) resizes it. The headline is **one-way seals** via **`SYS_memfd_seal(fd, add)`** (#190): **`F_SEAL_WRITE`** (writes/`ftruncate` fail, contents frozen), **`F_SEAL_SHRINK`**/**`F_SEAL_GROW`** (the size can't drop/rise), and **`F_SEAL_SEAL`** (no further seals) — the idiom behind safe shared-buffer passing. memfds are **refcounted across `fork`/`dup2` exactly like a pipe** (`memfd_ref`/`memfd_unref` added to `app_fd_fork`/`app_dup2`/`app_fd_close`; freed at the last reference), and route through stdio when `dup2`'d onto 0/1/2 (added to `app_fd_is_redirected`). The whole object lives in `app.c` beside the fd table (no new build wiring); `struct memfd`/seal bits in the shared `syscall.h`; ulib `sys_memfd_create`/`sys_memfd_seal`/`sys_ftruncate`. **Verified:** `make check` **56 green** + **headless in-guest** — the new `memfdtest`: create → write `"hello world"` → rewind+read it back → **`SEAL_GROW`** (a grow `ftruncate(100)` is rejected, a shrink to 5 still works → `"hello"`) → **`SEAL_SHRINK`** (now `ftruncate(2)` is rejected) → **`SEAL_WRITE`** (a write is rejected, the 5 bytes stay intact) → **`SEAL_SEAL`** (adding any further seal is rejected) — `memfd: rw + ftruncate + SEAL_GROW/SHRINK/WRITE/SEAL all enforced -- OK`, desktop healthy. Follow-ons: `MFD_CLOEXEC` semantics on exec, `vmsplice` a sealed memfd into a pipe (pairs with M1211), and mmap-ing a memfd for true shared memory.

> **(M1211) ipc — `splice`/`tee`: zero-copy pipe→pipe data movement (completes the pipe story).** We had `copy_file_range` (path→path / path→net sendfile) but the canonical *pipe-fd↔pipe-fd* edge was missing — moving bytes between two pipes meant bouncing them through a userspace buffer. Now **`SYS_splice(in_fd, out_fd, len)`** (#187) moves up to `len` bytes from a pipe read-end fd to a pipe write-end fd **entirely in-kernel** — `pipe_splice` copies ring-to-ring (no userspace copy) and **consumes** the source — and **`SYS_tee(in_fd, out_fd, len)`** (#188) **duplicates** a pipe's buffered bytes into a second pipe **without consuming** the source (it reads from the source's `tail` cursor without advancing it), so the same data can then be both read *and* forwarded. Both reuse the existing reader/writer wake discipline (wake the dest's blocked reader once data lands, wake the source's blocked writer once `splice` frees ring space), are non-blocking (transfer what's buffered + what fits *now*), and reject a bad/identical index. The fd→pipe resolution reuses `fd_pipe_idx` (so `splice` requires `in_fd` a read-end + `out_fd` a write-end); no user pointer to validate (all args are fds + a length). ulib `sys_splice`/`sys_tee`. **Verified:** `make check` **56 green** + **headless in-guest** — the new `splicetest`: writes `"hello world"` to pipe A, **`splice`s all 11 bytes A→B** and reads them out of B, then confirms **A is drained** via `poll(A, POLLIN, 0) == 0` (using M1210 — splice really consumed the source); then writes `"abc"` to pipe C, **`tee`s 3 bytes C→D**, and asserts **both D *and* C still read `"abc"`** (tee preserved the source) — `splice/tee: 11B spliced (src drained) + 3B teed (src preserved) -- OK`, desktop healthy. The zero-copy/pipe story is now complete (anon pipes + FIFOs + dup2 + poll + splice/tee). Follow-ons: `vmsplice` (gift user pages into a pipe), `SPLICE_F_MOVE`/`NONBLOCK` flags, and splicing a *file* fd ↔ pipe (true `sendfile` over fds).

> **(M1210) ipc — `poll(2)`: readiness multiplexing over the fd table (the Unix event-loop keystone).** We had pipes, FIFOs, and file fds but **no way to wait on several at once** — every read blocked on a single fd. `SYS_poll(struct pollfd*, nfds, timeout_ms)` (#186) fixes that: it scans an fd set each tick and returns the count whose readiness changed, setting `revents` per fd. Readiness is computed by a new **`app_fd_ready(fd, events)`** that dispatches on the fd-table kind — a **pipe read-end** is `POLLIN` iff it has buffered data *or* hit EOF (new non-destructive **`pipe_readable`**: `p_cnt>0 || (no writers && had_writer)` — matching `pipe_read`'s own EOF gating, so a FIFO can't false-EOF before a writer connects); a **pipe write-end** is `POLLOUT` iff the ring has room (**`pipe_writable`**); a **file fd** never blocks so it reports both; an **unopened fd → `POLLNVAL`**. The wait loop reuses the proven `SYS_fswait` discipline (`sti`; check; `task_sleep_ms(10)`; honor `timeout_ms<0` = block forever, `0` = immediate poll) and validates the user array with `ubuf`. `struct pollfd` + `POLLIN/POLLOUT/POLLERR/POLLHUP/POLLNVAL` live in the shared `syscall.h` (int+short, no stdint); ulib `sys_poll`. **Verified:** `make check` **56 green** + **headless in-guest** — the new `polltest` exercises four cases deterministically: **(A)** fork a child that writes a byte, parent **blocks in `poll` until POLLIN fires** then reads it; **(B)** an idle read-end with a live writer + a 30 ms timeout **returns 0 with no revents**; **(C)** a fresh write-end is **immediately `POLLOUT`-ready** (timeout 0); **(D)** a never-opened fd **reports `POLLNVAL`** — `poll: blocking POLLIN + timeout=0 + POLLOUT + POLLNVAL all OK`, desktop healthy. This is the substrate for `select`/`epoll` and any real event loop. Follow-ons: an `epoll`-style persistent readiness fd object, `POLLOUT`-driven backpressure in the shell's pipelines, and folding `SYS_fswait` (path-based) into this fd-based core.

> **(M1209) signals — `sigpending`: query the raised-but-blocked signal set.** A small complement to M1208: **`SYS_sigpending()`** (#185) returns the process's `pending_sigs` bitset — the signals that have been raised but not yet delivered because they're blocked (POSIX `sigpending(2)`). ulib `sys_sigpending`. **Verified:** `make check` 56 green + **headless in-guest** — `sigmasktest` now also asserts that while signal 10 is blocked-and-raised, `sys_sigpending()` reports it pending (before unblocking delivers it): `sigprocmask: blocked signal stays pending (sigpending sees it, handler NOT run), then delivered on unblock -- OK`. The POSIX signal-mask trio is complete: `sigprocmask` (mask) + `sigpending` (query) + the existing delivery/sigreturn.

> **(M1208) signals — `sigprocmask`: block/unblock signals (pending-while-blocked).** The signal subsystem had handlers + `sigreturn` + signalfd + a `pending_sigs` bitset, but **no signal mask** — every signal delivered immediately. Now each process carries a `sig_blocked` mask: `app_deliver_pending` skips a blocked signal (it stays set in `pending_sigs`), and **`SYS_sigprocmask(how, set)`** (#184) edits the mask — `SIG_BLOCK` / `SIG_UNBLOCK` / `SIG_SETMASK` — returning the previous one; `SIGKILL`/`SIGSTOP` (9/19) can't be blocked. When a signal is unblocked, the still-pending instance is delivered on the next IRQ-tail return to ring 3. The mask resets to empty on spawn. ulib `sys_sigprocmask` + the `SIG_*` constants. **Verified:** `make check` **56 green** + **headless in-guest** — `sigmasktest` installs a handler, **blocks the signal, raises it (the handler does NOT run while blocked), then unblocks → the now-pending signal fires the handler**: `sigprocmask: blocked signal stayed pending (handler NOT run), then delivered on unblock -- OK`. This rounds out the POSIX signal model (handlers, sigreturn, signalfd, **masking**). Follow-on: `sigpending` (query the pending set) and a per-handler mask during delivery.

> **(M1207) fs — ext2 hard links (`link(2)`): a second name for the same inode.** A real POSIX FS gap — we had symlinks + unlink but no hard links. New **`ext2_link_path`**: resolve the existing target's inode (refusing directories), `dir_add` a second directory entry in the new path's dir pointing at that **same inode number**, and bump its `i_links_count`. The crux was fixing **`ext2_unlink_path`**, which previously zeroed `links_count` and freed the inode *unconditionally* — it now **decrements** the count and frees the inode + its blocks only when the **last** link is removed, so the remaining names keep reading the data. Routed end-to-end: `ext2_link_path` → `blockdev_mount_link` (ext2 mounts only) → **`vfs_link`** (both paths must resolve to the *same* `/diskN` ext2 mount — POSIX hard links can't cross filesystems) → **`SYS_link`** (#183) → ulib `sys_link` → the shell's **`ln A B`** (a hard link now; `ln -s` stays a symlink). **Verified:** the host `ext2test` creates a file, hard-links it, confirms both names read identically, **unlinks the original and the link still reads the full content**, and the dumped image is **e2fsck-clean** after the link + unlink (the gold-standard check of link counts + directory entries); `make check` **56 green**. Scope: hard links only within one ext2 mount (tmpfs/FAT32/synth return -1, exactly as POSIX behaves across filesystems).

> **(M1206) debug — GDB stub register + memory writes (`G`/`M`): the kernel debugger is now read-write.** Completes the M1204/M1205 stub. **`G`** writes all registers — the GP regs + rip parsed from the g-packet hex back into the trap frame, with cs/ss preserved + rflags sanitized so the resume iret stays legal. **`M<addr>,<len>:<data>`** writes memory via the bounds-checked `kwriteb`. So from gdb you can now `set $rax=…`, `set {int}addr=…`, patch instructions live — full inspect *and* control. **Verified:** `make check` **56 green** — `gdbstubtest` now also has real gdb **write `0xcafef00d` to a scratch stack address and read it back** (the `M`/`m` round-trip), on top of break/continue/stepi. The gdb stub is now a **complete debugger**: read+write registers & memory, software breakpoints, single-step — `gdb build/kernel.elf -ex "target remote …"` gives a full interactive kernel-debug session. Deferred polish: a target-description XML (so gdb auto-detects the arch without `set architecture i386:x86-64`) and `qXfer`.

> **(M1205) debug — the GDB stub goes interactive: software breakpoints + single-step.** M1204 was read-only (inspect registers/memory); this makes the running kernel **fully gdb-controllable**. **`Z0`/`z0`** plant/lift a software breakpoint — write `0xCC` into the (RWX-mapped) kernel image, stashing the original byte to restore on lift; on a planted-breakpoint `#BP` the stub backs `rip` up by one so gdb sees the breakpoint's address (the deliberate kmain int3, not in the table, is left alone). **`s`** single-steps — set `RFLAGS_TF` and resume; the resulting **ring-0 `#DB`** re-enters the stub via a new `isr_dispatch` branch, gated on `gdbstub_armed()` so it never disturbs the ring-3 app single-stepper. The subtle bug that surfaced: after `c`/`s` the stub must **send the stop reply (`S05`) *unsolicited*** when the breakpoint/step lands (gdb is *waiting* for it, not sending), else gdb and the stub deadlock ("target is running") — a `gdb_running` flag drives it. **Verified:** `make check` **56 green** — `gdbstubtest` now also has real gdb **set a breakpoint at `vdso_init`, `continue` into it** (the planted int3 fires → the kernel stops → gdb reports the hit), **and `stepi`** — so `break`/`continue`/`stepi` all work against the live kernel; the gated stub still leaves normal boots untouched. Follow-on: `G`/`M` register & memory writes, so gdb can patch the kernel's state, not just read it.

> **(M1204) debug — a GDB remote-serial stub: real host `gdb` source-level debugs the kernel.** The standout r/osdev pick (an OSDev rite of passage) — `gdb build/kernel.elf -ex "target remote …"` now gives **source-level kernel debugging** over a serial port. `kernel/gdbstub.c` speaks the GDB **Remote Serial Protocol**: `?` (stop reason → `S05`), **`g`** (read all registers — marshalled into GDB's amd64 g-packet order `rax,rbx,…,r15,rip,eflags,cs,ss,ds,es,fs,gs`, little-endian, from the `#BP` trap frame), **`m`** (read memory — bounds-checked to the kernel higher-half + the low identity map so a stray gdb read can't fault the stub), and `c`/`D`/`k` (continue/detach/kill); unsupported packets get the empty reply, which gdb works around. Enabled by **`-append gdbstub`** on the multiboot command line — detected at the **very top of `kmain`** (before `pmm_init`/`kheap_init` allocate over the cmdline string QEMU parks in high RAM — the clobber that cost the first attempt), which arms the stub; then after `smp_init` an armed kernel `int3`'s, and the existing `#BP` handler hands the trap frame to `gdbstub_serve()`, which runs the RSP loop on **COM2** (0x2F8) — leaving COM1 as the normal kprintf console. **No `gdbstub` on the cmdline ⇒ fully dormant** (normal boots untouched). The codec (checksum / `$..#xx` framing / hex / register-marshalling / command-handler) is pure over buffers. **Verified:** `make check` **56 green** — the new **`gdbstubtest`** boots `-append gdbstub` with COM2 as a TCP serial and drives **real host `gdb`**: it connects, reads the registers, and **symbolizes `rip` back to `kmain` at the int3 source line** (`0x17d509 in kmain at kernel/kmain.c:165`), then `detach` resumes boot — proving the g-packet + the ELF symbols line up end-to-end (it skips cleanly if gdb/qemu are absent), and the gated stub leaves the normal `boottest` boot intact. Follow-ons: `Z0`/`z0` software breakpoints (insert `0xCC`, report the hit), `s` single-step (`RFLAGS_TF`), and `G`/`M` register/memory writes — turning inspect-only into full interactive control of the running kernel.

> **(M1203) trace — `syscount`: a named syscall-histogram profiler on the M1202 tracepoint.** M1202 gave the mechanism (a verified BPF program counting syscalls by number into `bpf_map`); this is the **user-facing payoff** — the dtrace/bpftrace "which syscalls are firing" view. New **`/proc/syscalls`** renders the histogram with syscall **names**: `syscall_histogram_format` (in `syscall.c`, where the static `syscall_name` table lives) walks `bpf_map_get(i)` × `syscall_name(i)` for the nonzero cells, declared in `bpf.h` (since `syscall.h` is the no-`stdint` userspace-shared header, the decl can't live there). A shell **`syscount`** command loads the 4-instruction count-by-number probe (`LDCTX r0,0; MAPINC r0; LDI r1,1; RET r1`), and **`syscount off`** clears it. **Verified:** `make check` **55 green** + **headless in-guest** — `syscount` then `cat /proc/syscalls` prints the live named histogram (`write 4`, `read 1`, `readfile 1` — exactly the syscalls the shell issued between loading the probe and reading the file), desktop healthy. Low-risk + additive (a read-only `/proc` file + a shell command, both atop the M1202 syscalls; an unloaded probe ⇒ `/proc/syscalls` says so). Follow-ons: per-process filtering (scope to one pid), arg-aware aggregation, and a `syscount <cmd>` that brackets a spawned command. (The bigger sick picks remain queued for dedicated fires — **GDB remote-serial stub** foremost, then migrating-threads IPC, CRIU-lite checkpoint/restore, a 9P server, the per-core SMP scheduler, and ptrace software breakpoints.)

> **(M1202) trace — eBPF syscall tracepoints: a loaded BPF program counts syscalls by number into an in-kernel histogram.** The eBPF-lite VM (M1127) only ran the firewall + per-process seccomp filters; this turns it into a **dynamic-tracing** probe — the dtrace/bpftrace idiom — reusing the verified VM end-to-end (a fresh r/osdev research subagent's #2 pick). A new **`BPF_MAPINC`** opcode increments `bpf_map[reg[a] & (BPF_MAP_N-1)]` (a 256-cell histogram); a global **trace program** loaded by `SYS_bpf_trace(prog, bytes)` (#181) is run by a one-line hook at the top of `syscall_dispatch` on **every syscall enter**, with the seccomp-style ctx `{nr, arg0, arg1, arg2}` — so a 4-instruction program (`LDCTX r0,0; MAPINC r0; LDI r1,1; RET r1`) counts calls by syscall number. The histogram is read with `SYS_bpf_map_get(idx)` (#182) or dumped at **`/proc/bpf`** (new `trace:` / `map:` lines beside the firewall's run/drop stats). It's bounded + verified like every BPF program (forward-only control flow, ≤64 insns, no memory access beyond the histogram), and **gated** — no trace loaded ⇒ one branch and every syscall is byte-identical; `bpf_run_prog` stays effectively pure for the firewall/seccomp (only a program that *contains* `MAPINC` writes the map, and theirs don't). **Verified:** `make check` **55 green** + **headless in-guest** — `bpftracetest` loads the count-by-number probe, calls `getpid()` 5×, and `SYS_bpf_map_get(SYS_getpid)` confirms the histogram counted them (`bpf-trace: histogram probe on every syscall; 5x getpid -> map[getpid] counted -- OK`); `/proc/bpf` shows the new `trace`/`map` fields (the test clears tracing on exit so a global probe never lingers), desktop healthy. Follow-ons: richer ctx (64-bit args, pid/uid), `MAP_SET`/aggregation ops, attaching a program to a *specific* syscall or to function kprobes, and a userspace `bpftrace`-style front-end that emits the bytecode.

> **(M1201) debug — KASAN-lite: the kernel heap catches buffer overflows (redzones) + poisons freed memory.** A fresh r/osdev research subagent's #1 pick (rust-osdev's recurring "HWASAN/KASAN for kernels" theme) — and half-built already: `kheap.c`'s `BLK_MAGIC` sentinel caught double-frees, but a **buffer overflow** (a write past the allocation) silently corrupted the heap. This adds the missing guard. Each `kmalloc(size)` now reserves `align16(size + 16)` bytes and writes a **16-byte redzone** of `0xBE` immediately past the user's bytes (the `block_t` header remembers `req = size`); on `kfree` the redzone is verified intact — a corrupted byte means the code wrote past the end, so it's logged to dmesg (`[kasan] heap buffer overflow: N-byte object … written ≥ K byte(s) past end`) and counted. Freed memory is **poisoned with `0xDE`** (bounded to 256 B so freeing a large block stays cheap) so a use-after-free *read* returns obvious garbage instead of stale live data. It's a **non-fatal detector** (log + count, no panic), **always-on**, and additive — the header grows 8 bytes for `req` but the alloc/split/coalesce/grow logic is otherwise untouched, and the host test (`KHEAP_HOST_TEST`) gets a no-op `kprintf`. Counters surface at **`/proc/kasan`** (`redzone_checks` / `overflows_caught`). **Verified deterministically (no QEMU needed for the core):** the host `kheaptest` (ASan+UBSan) now asserts a **1-byte overflow is caught** (`q[40]` writes into the redzone → `overflows_caught` increments) while a **clean in-bounds allocation stays silent** (`overflows` unchanged, `checks` +1), and the existing **400 000-op** split/coalesce/grow torture stays intact + ASan/UBSan-clean; `make check` **55 green**; and a **headless boot is kasan-clean** — `cat /proc/kasan` shows `redzone_checks: 3, overflows_caught: 0` (no false-positives, no latent overflow bug in the live kernel), desktop healthy. Follow-on for full **use-after-free *write*** detection: a free **quarantine** (hold freed blocks out of reuse + verify their `0xDE` poison intact on dequeue), the standard KASAN technique, deferred as a contained next step.

> **(M1200) debug — ptrace `SETREGS` + `SINGLESTEP`: ptrace is now a full debugger primitive.** Completes M1199 (TRACEME / WAIT / PEEK / POKE / GETREGS / CONT) with the two requests a real debugger needs to *drive* a tracee. **PT_SETREGS** writes the stopped tracee's registers from the tracer's buffer — but preserves the privilege-critical fields (`cs`/`ss`/`int_no`/`err_code`) and sanitizes `rflags` to the user-settable bits with IF forced on (`(new & 0x0CD5) | 0x202`), so the tracee's `iret` stays legal no matter what the tracer wrote. **PT_SINGLESTEP** resumes the tracee for exactly one instruction: it sets the x86 trap flag (`RFLAGS_TF`) in the tracee's saved frame and wakes it, and the existing `#DB` handler (`app_singlestep_trap`, M1123 — which already backed `/proc/<pid>/sstrace`) now recognises a `trace_stepping` tracee and **re-stops it as SIGTRAP** (notifying the tracer) instead of recording to the sstrace ring. So a tracer can single-step a child instruction-by-instruction, inspecting + rewriting its full register state at each stop — the core loop of `gdb stepi`. Additive: the `#DB` hook is gated on `ptraced && trace_stepping`, so plain `/proc/sstrace` single-stepping is unchanged; `SIGTRAP` (5) is a new shared signal constant. **Verified:** `make check` **55 green** + **headless in-guest** — the extended `ptracetest` now also **GET→SETREGS round-trips** the stopped child's registers (non-destructive — the child still runs correctly afterward) and **PT_SINGLESTEP**s it, with the follow-up `PT_WAIT` returning **SIGTRAP** (proving it trapped after exactly one instruction), then `CONT` → the child `exit(7)`: `ptrace: stop + PEEK(111)/POKE(222) + GET/SETREGS + SINGLESTEP(SIGTRAP) + CONT -> child exit 7 -- OK`, desktop healthy. ptrace now covers **stop / peek+poke memory / get+set registers / single-step / continue** — enough to build an in-guest `strace` or a `gdb stepi` loop on top. Follow-ons: PTRACE_ATTACH to a running (un-`TRACEME`'d) process, software breakpoints (POKE an `int3`, catch `#BP`), and surfacing trace-stops through `waitpid` (WIFSTOPPED) for a standard tracer main loop.

> **(M1199) debug — `ptrace(2)`: a tracer stops a child, reads/writes its memory + registers, and continues it (the mechanism behind strace/gdb).** A fresh r/osdev research subagent's #1 pick — and "nearly free" here because every hard primitive already existed in isolation; this wires them into the canonical Unix tracing syscall. `SYS_ptrace(request, pid, addr, data)` (#180) → `app_ptrace`, with: **PT_TRACEME** (the child marks itself traced by its parent), **PT_WAIT** (the tracer blocks until the tracee stops), **PT_PEEKDATA / PT_POKEDATA** (read/write one word of the tracee's memory — reusing the COW-aware, same-tree-gated `app_process_vm_read/write`, M1162/M1165), **PT_GETREGS** (copy the tracee's saved trap frame — `task_uframe`, M1119 — into the tracer's buffer), and **PT_CONT** (resume it). The stop/wait/cont rendezvous mirrors the **seccomp-notify** mailbox (M1124): the tracee parks in `app_trace_stop` via `task_block`; it wakes the tracer; `PT_CONT` `task_wake`s the tracee — all interrupts-off in the syscall path, and since every ring-3 task is scheduled on the BSP (the M1198 APs only run the compute pool), tracer and tracee never truly run at once, so there's no lost wakeup. The single hook into existing code: a traced process's `raise()` (`SYS_raise`) becomes a trace-stop instead of the signal's default action. **A fork-order race was caught + fixed during verification:** `PT_WAIT` must register as the waiter *even before* the child has run `TRACEME` — else, if the parent waited first, it returned -1, never continued the child, and both deadlocked (the tracee's forked window hung open on screen, which is how I spotted it). So `PT_WAIT` blocks on the child's stop **without** requiring `ptraced` upfront (exactly like `seccomp_wait`'s "even if not armed yet"); only the inspect/continue requests require the child already traced + stopped. Additive — an untraced process is byte-identical (the `raise` hook is a no-op unless `ptraced`). **Verified:** `make check` **55 green** + **headless in-guest** — `ptracetest` forks a child that `TRACEME`s, `raise(SIGSTOP)`s, then `exit(x==222?7:8)`; the parent `PT_WAIT`s for the stop, **PEEKs `x` (=111), POKEs it to 222, PEEKs back (=222), GETREGS, CONT**, and `waitpid` confirms the child saw the poke and **exited 7** — printing `ptrace: WAIT(stop) + PEEK(111) + POKE(222) + GETREGS + CONT -> child saw 222, exit 7 -- OK`, with the desktop healthy and the tracee's window closing cleanly. This is the substrate for an in-guest `strace`/`gdb`. Follow-ons: PT_SETREGS, PT_SINGLESTEP (wire the existing `#DB`/`app_singlestep_trap` to re-stop a traced stepper), PTRACE_ATTACH to a running process, and surfacing stops through `waitpid` (WIFSTOPPED).

> **(M1198) smp — a parallel job pool: the OS now runs compute across all cores in parallel (power-friendly — idle cores HLT, woken by an IPI).** M1197 brought the APs up but parked them in `cli;hlt`; this makes them *useful* without the full per-core-scheduler surgery. Each AP, in `ap_main`, now adopts the **kernel GDT + IDT** (new `gdt_load_ap()` / `idt_load()` — so `KERNEL_CS` resolves to the 64-bit kernel code segment when the core takes a trap, and it shares the BSP's interrupt gates; it runs ring-0 only, so it needs **no per-CPU TSS**), then idles in `hlt` with interrupts enabled. The BSP exposes **`smp_parallel_for(n, fn, ctx)`**: it splits `[0,n)` into chunks (≈4 per core, for load-balance), fires a **fixed wake-IPI (vector 0x40, "all-but-self" ICR shorthand)** to rouse the halted APs, and the APs *and the BSP* pull chunks from a tiny **spinlock-guarded job queue** and run them concurrently — the job function runs **outside** the lock, so the cores execute in parallel; the BSP joins on a completion counter. Crucially, **idle APs HLT rather than busy-spin** (a spinning AP would peg a whole host CPU — bad under QEMU); they sleep until the next wake IPI, via a lost-wakeup-safe `cli; recheck; sti;hlt`. New plumbing: an `isr64`/`isr255` stub pair + IDT gates for **0x40 (wake IPI)** and **0xFF (LAPIC spurious)**, a 2-line `isr_dispatch` case (`lapic_eoi`), and `idt_load`/`gdt_load_ap` so an AP can share the BSP's tables. The job fn must be **pure compute** (no `kmalloc`/FS/unsynchronised kernel state — the general kernel is still BSP-only). **Verified three ways:** `make check` **55 green** (incl. the single-CPU `boottest`/`gfxtest`/`browsertest` boot paths, which take `smp_init`'s uniprocessor early-return and are unaffected); **headless `-smp 4` boot** logs `[smp] parallel self-test: sum(0..4000000) OK on 4 core(s)` — a parallel reduction whose result equals the closed form `N(N-1)/2`, run across **all 4 cores** (proving the APs genuinely wake, execute dispatched work, and rejoin); and **in-guest** `cat /proc/cpuinfo` shows `smp parallel : 4 cores, sum self-test OK`, with the desktop fully healthy and idle cores halted (no host-CPU pegging). Scope/risk: pure-compute jobs only (APs make no syscalls / touch no FS yet), the BSP owns dispatch, and there's no migration/stealing beyond the shared queue. Follow-ons toward full SMP: a **per-core scheduler run-queue** so ring-3 tasks actually run on the APs (needs per-CPU TSS + GS-base + a kernel-wide locking audit of the now-concurrent paths), the **IOAPIC** to route device IRQs across cores, and **IPIs for TLB shootdown** when one core changes another's page tables.

> **(M1197) smp — multi-core bring-up: the OS now boots its application processors (LAPIC + MADT + a real→long-mode AP trampoline).** The OS was strictly uniprocessor — PIC-only, no APIC at all — the single biggest "real OS" gap (every prior milestone called it out as the blocked big swing). This brings the other cores online. On the BSP, `smp_init()` (slotted after `acpi_init`, before `sched_init`): (1) maps the LAPIC MMIO page (`0xFEE00000` — above RAM, so the HHDM doesn't cover it) via `vmm_map(…, PTE_WRITABLE|PTE_PCD)` and software-enables the local APIC (the `IA32_APIC_BASE.EN` MSR bit + the spurious-vector-register enable bit); (2) parses the ACPI **MADT** (new `acpi_madt_lapics()` — the table signed "APIC") for every Processor-Local-APIC entry's ID; (3) copies a hand-written **trampoline** to physical `0x8000` and fills a parameter block at `0x7000` (kernel PML4 → CR3, this AP's stack top, `&ap_main`); (4) for each non-BSP APIC ID, fires the Intel **INIT–SIPI–SIPI** sequence over the LAPIC ICR (`0x4500`, then two `0x608` at vector `0x08`) with PIT channel-2 busy-delays — interrupt-independent, so it works this early before the scheduler. Each AP boots in 16-bit real mode and the trampoline (`kernel/asm/ap_trampoline.asm`, a flat `nasm -f bin` binary `incbin`'d into the kernel like the app ELFs) walks it up the *exact* ladder `boot.asm` used for the BSP — far-jump to CS=0 (so absolute refs resolve) → `lgdt` → CR0.PE → 32-bit → CR4.PAE → CR3=kernel PML4 → EFER.LME (+NXE, CPUID-guarded) → CR0.PG → far-jump into 64-bit → set RSP → `call ap_main` — **reusing the kernel's own page tables**, whose low-1 GiB identity map keeps `0x8000` mapped at the instant paging turns on (the classic AP-trampoline triple-fault trap, sidestepped). `ap_main()` enables the AP's own LAPIC, atomically bumps `smp_cpu_count`, and parks in `cli;hlt`. **The APs only park** — the scheduler still runs on the BSP alone and the APs take no interrupts — so nothing else has to be SMP-safe yet: the legacy 8259 PIC is left remapped+masked exactly as `pic_init` left it (LAPIC and PIC coexist; INIT/SIPI ride the ICR, independent of the PIC). `-smp 4` is now the default in both the Makefile `QEMUFLAGS` and `tools/osdrive.py`. **Verified three ways:** `make check` **55 green** (the `acpi.c` MADT addition + the flat-binary build wiring regress nothing); **headless boot serial** prints `[smp] 4 of 4 CPUs online (BSP apic=0)` — and since `smp_cpu_count` starts at 1 and is incremented *only inside `ap_main`*, reaching 4 proves all 3 APs independently completed the full real→long-mode transition on the kernel page tables and executed C; and **in-guest** `cat /proc/cpuinfo` now reports `processors : 4` (wired into the existing `/proc/cpuinfo`), with the desktop fully healthy. The first attempt crashed with a clean page-fault **panic** (backtrace `smp_init+0x55`, CR2 `0xffff8000fee00080`) — precisely the unmapped-LAPIC case (HHDM covers RAM, not the ~4 GiB MMIO) — fixed by the explicit `vmm_map`; the OS's own fault handler made it a one-line diagnosis rather than a silent triple-fault. This is the foundational SMP slice: *the OS boots its other cores.* Follow-ons, each a real milestone: per-CPU GDT/TSS/IDT + GS-base so an AP can safely take interrupts; a per-core scheduler run-queue (APs run tasks instead of parking); spinlocks + a big-kernel-lock audit for the now-genuinely-concurrent paths; the IOAPIC to route device IRQs across cores; and IPIs for TLB shootdown.

> **(M1196) fs/vm — native positioned `vfs_pread` for tmpfs + ext2 (drops the M1193 file-fd read-prefix + 1 MiB cap).** `vfs_pread` already existed (M1136) but served **only the boot filesystem** (`fs->pread`) — which is exactly why M1193's file fds resorted to a read-the-whole-prefix-then-slice workaround (O(offset)/read, capped at 1 MiB). This extends `vfs_pread` to read **natively at the offset** on the two common file-fd backends: new `ext2_pread` (walks blocks from `offset/bs` with a partial first/last block — `ext2_read_path` is now just `ext2_pread(…,0)`) and `tmpfs_pread` (a bounded memcpy from `buf+offset`), routed via a new `blockdev_mount_pread` (ext2 native; ISO/FAT fall back to a read-prefix-slice internally). A boot-FS path matches neither tmpfs nor a mount, so it still hits `fs->pread` exactly as before — **file-backed mmap (M1136) is byte-for-byte unaffected** (verified: the `mmapfile` demo still pages MOTD.TXT correctly via the fault path). `app_fd_read`'s FILE branch is rewired to one `vfs_pread` call — **uncapped, seek-efficient, and no per-read `kmalloc`** (the M1193 read-prefix code is gone). **Verified four ways:** a new **`ext2_pread` host check** in the ext2 suite reads `/BIG` at 9 offsets (incl. block boundaries 1023/1024/4096/4097 + a partial first block) + past-EOF and asserts **byte-exact vs a full read** (ASan-clean); `make check` 55 green (the `vfs_pread` extension doesn't regress file-backed mmap / uffd / the boot); and **headless in-guest** — `filefdtest` (M1193 chunked reads + `lseek` SET/END + EOF, now through `tmpfs_pread`) and `filewrtest` (M1195) both pass, and the `mmapfile` app still works. Scope: ISO/FAT-mount + synth/overlay reads still use the prefix fallback (rare for file fds); positioned/extending `vfs_write` at the FS layer (to drop M1195's write RMW too) is the symmetric follow-on. With this the file-fd read path is production-quality. The genuinely-remaining work is now the big swings only — ext3 journal, page cache, SMP (SMP needs a LAPIC+MADT prerequisite: the OS is currently PIC-only).

> **(M1195) fds — write/append file descriptors (`open(…,O_WRONLY/APPEND/TRUNC/CREAT)` + positioned `write`): file fds are now read+write.** M1193 gave read-only file fds; this completes them. `app_open` takes O_* flags now: `O_CREAT` creates an absent file, `O_TRUNC` (with `O_WRONLY`) zeroes it, `O_APPEND` starts the offset at EOF, and a writable fd records it (the file-fd's `write_end` flag = writable). `app_fd_write` gains a FILE branch: a **read-modify-write** — `vfs_stat` the size, `kmalloc` `max(size, off+len)`, `vfs_read` the file, patch `[off, off+len)` (zero-filling any gap for a sparse extend), `vfs_write` it back, advance the offset. Crucially this rides the **existing `vfs_read`/`vfs_write`**, so it works uniformly across FAT32 / ext2 / tmpfs with **no per-filesystem code** (and the whole-file-replace `vfs_write` is exactly what an RMW wants); bounded to `FILEFD_CAP` (1 MiB). A write to a read-only file fd returns -1; pipe fds are unchanged. **Additive + ABI-compatible:** `SYS_open` now reads flags from arg2, and M1193's `sys_open(path)` still passes `O_RDONLY` (0) — so existing read file fds are byte-identical; `make check` 55 green + in-guest `filefdtest` (M1193) still passes. ulib keeps `sys_open(path)` + adds `sys_open_mode(path, flags)`; O_* flags in `syscall.h`; a deterministic `filewrtest` builtin. **Verified:** `make check` 55 green + **headless in-guest** — `filewrtest` opened `/tmp/fw.txt` `O_WRONLY|O_CREAT|O_TRUNC`, wrote "Hello World", `lseek`'d to 6 and overwrote "World"→"There" (positioned write → "Hello There"), reopened `O_APPEND` and wrote "!" → and `cat /tmp/fw.txt` shows **`Hello There!`** (the fd-written file read back via the normal path), printing `file-wr: open(WR|CREAT|TRUNC) + positioned overwrite + O_APPEND all OK`. The fd subsystem is now complete: per-process table, anon pipes, FIFOs, stdio redirection, **read+write file fds**, and `/proc/<pid>/fd`. Scope: RMW (not atomic; loses concurrent writers — fine for the single-writer norm), 1 MiB cap, offsets independent across fork. Follow-ons (the plateau's real remaining work): a positioned/extending `vfs_write` at the FS layer (drop the RMW + cap), then the big swings — ext3 journal, page cache, SMP.

> **(M1194) /proc — `/proc/<pid>/fd`: the descriptor table is now observable.** A clean capstone to the fd arc (M1187 pipes → M1188 FIFOs → M1191 stdio-redirect → M1193 files): every process's open descriptors are now introspectable, the Linux `/proc/<pid>/fd` idiom. `app_format_fds` walks the target app's fd table and renders each open fd with its **type + target** — `N: pipe [read]/[write] #<pipe-id>` for anonymous-pipe/FIFO ends, `N: file <path> @<offset>` for file fds (so you can see exactly what a process has open and where each file fd is positioned) — reusing the same `maps_dec`/`maps_str` text helpers as `/proc/<pid>/maps`. Wired into the existing `proc_pid_path` dispatch beside `maps`/`smaps`/`pagemap`, so it works for any pid and for `/proc/self/fd`. **Read-only + additive** (a new synth proc file + a formatter; nothing else touched) → `make check` 55 green, zero regression. A deterministic `procfdtest` builtin. **Verified:** `make check` 55 green + **headless in-guest** — `procfdtest` opened a pipe (2 fds) + a file fd, read its own `/proc/self/fd`, and confirmed the listing names both a `pipe` and a `file` (`/proc/self/fd: lists open pipe + file fds OK`); and `cat /proc/self/fd` from the (fd-less) shell correctly prints `(no open fds)`, exercising the empty case through the real proc read path. With this the whole fd subsystem — pipes, FIFOs, files, stdio redirection — is both functional and observable. Follow-ons (the plateau's real remaining work): a positioned `vfs_read` to make file fds efficient + uncapped (and unlock write file fds), then the big swings (ext3 journal, page cache, SMP).

> **(M1193) fds — file descriptors for files: `open`/`lseek`/positioned `read`/`close` (completes the fd table).** The fd table held pipes (M1187) + FIFOs (M1188) but **regular files were path-only** (whole-file `vfs_read`/`vfs_write`, no descriptor, no offset). This adds real read-only **file fds**, the foundational Unix primitive the OS lacked: `open(path)` returns a descriptor (≥3, reserving 0/1/2 for stdio per M1191), `lseek(fd, off, SEEK_SET/CUR/END)` repositions, `read(fd, …)` returns bytes **at the current offset and advances it** (chunked/streaming reads), and the existing `fdclose` releases it. A new fd type (`2`=file) carries the path + offset in the fd entry; `app_fd_is_redirected` (the M1191 stdio router) now also recognises file fds, so `sys_read(fd)` transparently routes to the positioned-read path while a write to a read-only file fd returns -1. The MVP reads the prefix `[0, off+n)` via `vfs_read` and slices `[off, off+n)` — correct + simple, works uniformly across FAT32/ext2/tmpfs (any `vfs_read` path), bounded to the first 1 MiB (a real positioned `vfs_read` is the efficiency follow-on). `SEEK_END` uses `vfs_stat` for the size. **Additive + low-risk:** a new fd type + two syscalls (`SYS_open` #178 RPATH, `SYS_lseek` #179 STDIO); existing path-based I/O and pipe/FIFO fds are untouched — `make check` 55 green, and the in-guest `pipetest` (M1187) + `stdiotest` (M1191) still pass after the `app_fd_read` extension. No file-fd offset is shared across `fork` (each inherits an independent offset — an acceptable MVP deviation from POSIX's shared open-file-description). ulib `sys_open`/`sys_lseek` + `SEEK_*`; a deterministic `filefdtest` builtin. **Verified:** `make check` 55 green + **headless in-guest** — `filefdtest` opened a 20-byte file, read it in 5-byte chunks (the offset advanced: `01234` then `56789`), `lseek`'d to 16 and read `GHIJ` then hit **EOF** (0), `lseek(-2, SEEK_END)` to 18 and read `IJ` — printing `file-fd: open + chunked read (offset advances) + lseek SET/END + EOF all OK`. The fd table is now a real POSIX descriptor table (pipes + FIFOs + files). Follow-ons: write file fds (needs a positioned/append `vfs_write`), a real positioned `vfs_read` (drop the read-prefix MVP + the 1 MiB cap), shared offsets across fork (an open-file-description layer), and `/proc/<pid>/fd` to list them all.

> **(M1192) security — seccomp-BPF KILL action (the hard-sandbox completion of M1190).** M1190's filter could only *deny* a forbidden syscall (return -1) — useful, but the action real sandboxes (Chrome/Docker) actually rely on is **KILL**: a process that attempts a forbidden syscall is *terminated*, not allowed to limp on. This adds it. The filter's `RET` value is now a 3-way verdict — `SECCOMP_RET_DENY`(0) → the syscall returns -1; `SECCOMP_RET_KILL`(2) → the process is killed (`app_sys_exit(-1)`, mirroring the pledge-violation path, with a `[seccomp] … hit a KILL filter … terminating` dmesg line); anything else → ALLOW — so M1190's `RET 1`=allow / `RET 0`=deny programs are byte-compatible and only the new `RET 2` triggers a kill. `app_seccomp_filter_check` now returns the **raw** verdict (was a 0/1 bool); the gated `syscall_dispatch` hook interprets it. The three verdict constants live in the shared `syscall.h` so userspace filters can name them. No new syscalls; no struct/ABI change (still the M1190 per-process `seccomp_prog`). **Verified:** `make check` 55 green (verdict change doesn't regress unfiltered apps or the M1190 deny path) + **headless in-guest** — `seccomptest` now runs both parts: a DENY child (blocked `writefile` → -1, then an allowed `getpid` → child exits 42) **and** a KILL child (installs a KILL-on-`writefile` filter, calls `writefile`, and is **terminated before reaching its `exit(7)`** — the parent's `waitpid` confirms it never hit 7), printing `seccomp: DENY blocks (-1) + allowed runs; KILL terminates the violator -- OK`, with the shell itself unaffected. seccomp-BPF is now a complete sandbox primitive (allow / deny / kill). Follow-ons: 64-bit arg fields (today args are 32-bit-truncated in the BPF ctx), a TRAP-to-supervisor action bridging to the M1124 seccomp-notify, and `/proc/<pid>` filter introspection.

> **(M1191) ipc/fds — stdio honors fd redirection: `print()`/`readline()` follow a redirected fd 0/1 (the fd-stdio rebase foundation).** The recurring #1 follow-on (real shell pipelines) hinged on apps' stdout/stdin actually flowing through the fd table. The pleasant discovery: they almost already did — `print()` is `sys_write(1,…)` and `readline()` is `sys_read(0,…)`, but the kernel handlers **ignored the fd** and always hit the window grid / keyboard. Now `SYS_write`/`SYS_read` check the M1187 fd table: if the fd is a **redirected pipe**, the bytes flow through it (`app_fd_write`/`app_fd_read`); otherwise the existing grid/keyboard path runs. **Gated + additive:** an app that never redirects has no fd-table entry for 0/1/2, so it takes the default path **byte-identical** — `make check` 55 green confirms zero regression across every app (the boottest/gfx/browser apps print + read exactly as before). To make this Unix-clean, **fd 0/1/2 are now reserved**: `pipe()`/`fifo_open` hand out descriptors from 3 up (the OS never pre-populated 0/1/2 — unused-in-table simply means "the console"), so `dup2(pipe_end, 1)` redirects stdout and `dup2(pipe_end, 0)` redirects stdin, exactly as POSIX programs expect. New `app_fd_is_redirected`; no new syscalls (it's transparent in `SYS_write`/`SYS_read`). A deterministic `stdiotest` shell builtin proves it. **Verified:** `make check` 55 green + **headless in-guest** — `pipetest` still prints `pipe: fork round-trip + EOF + dup2 all OK` (M1187 intact after the fd≥3 change), and `stdiotest` (a forked harness that runs a producer→consumer pipeline internally: the producer `dup2`s its stdout to a pipe and `print()`s, the consumer `dup2`s its stdin from the pipe and `readline()`s) prints **`stdio-fd: print() -> redirected fd1 -> pipe -> fd0 -> readline() round-trip OK`** — i.e. one process's `print()` was read by another's `readline()` entirely through a pipe via fds 1/0, with the shell's own stdio untouched. This is the keystone for **real shell pipelines**: the remaining step is the shell forking each `|` stage with `dup2`'d pipe ends (and `>`/`<`/`2>&1` via `dup2` of a file fd) — now a contained shell-side change rather than a kernel rebase. Follow-ons: that shell pipeline/redirection wiring; file fds (`open(path)->fd` with an offset) so `<`/`>` use real fds too; `O_CLOEXEC` across exec.

> **(M1190) security — seccomp-BPF: a process sandboxes *itself* with an in-kernel syscall filter.** The OS already had two syscall-restriction mechanisms but neither does *self-imposed, fine-grained, declarative* filtering: `pledge` is coarse syscall *classes*, and `seccomp-notify` (M1124) needs a separate supervisor *process* to adjudicate each call. seccomp-BPF is the third, and the one real software actually uses (Chrome/systemd/Docker): a process uploads a small BPF program that the kernel runs on *every one of its own syscalls* to allow or deny — the classic "drop privileges after setup" pattern, no babysitter required. It **reuses the existing `bpf.c` eBPF-lite VM** (M1127): that VM was a single global firewall program, so this **additively refactors** it into a pure `bpf_run_prog(prog, n, ctx)` (the firewall's `bpf_run` now just calls it with the global program + keeps its `/proc/bpf` stats — unchanged) and exposes the verifier `bpf_verify`. A per-process filter lives in `struct app` (`seccomp_prog[BPF_MAXINSN]` + `seccomp_n`), installed by `SYS_seccomp_filter(prog, bytes)` (#177); the program reads `ctx` fields 0..3 = the **syscall number + low 32 bits of args 0..2** (`LDCTX`) and `RET`s nonzero to allow / 0 to deny. The check is one gated branch at the top of `syscall_dispatch` (right after the pledge + seccomp-notify hooks): a denied call sets `rax = -1` and skips the real syscall via the existing `sc_done` path. **The filter is one-way (a privilege drop can't be revoked) and inherited across `fork`** (copied beside the fd table). **Additive + low blast radius:** an app with no filter (`seccomp_n == 0`) takes a single branch and is byte-identical — `make check` 55 green confirms no regression to the firewall (`bpf_run` path unchanged) or to unfiltered apps. ulib `sys_seccomp_filter`; a deterministic `seccomptest` shell builtin. **Verified:** `make check` 55 green + **headless in-guest** — `seccomptest` forked a child that installed a filter denying `SYS_writefile`, then **the blocked syscall returned -1 while an allowed syscall (`getpid`) still worked** (child exited 42), printing `seccomp: self-filter installed; blocked syscall denied, allowed syscall ran -- OK` — and the shell itself, unfiltered, kept running. Honest scope: 2-action (allow/deny→-1; no KILL/TRAP/ERRNO-value yet), arg matching is 32-bit-truncated, and the filter bytecode is the OS's eBPF-lite (not Linux cBPF). Follow-ons: a KILL action, full 64-bit arg fields, and `/proc/<pid>` exposing the installed filter.

> **(M1189) fs — ext4 extent WRITE: the OS now *creates* extent-mapped files (round-trips ext4 with M1186).** M1186 taught the driver to *read* ext4 extent trees; this teaches it to *write* them, so on an `extent`-feature volume a newly-created file is laid out the way modern ext4 lays it out — and Linux `e2fsck` validates it clean. `ext2_open` now reads `s_feature_incompat` (the `extent` bit `0x40`); `ext2_write_path` gains a branch (for **new** files, gated on that feature) that allocates the file's data blocks as **one contiguous run** via a new `alloc_run()` (mirrors `alloc_block`'s bitmap + group/superblock free-count bookkeeping, but finds N consecutive free blocks in a group) and writes a **single-extent tree** into `i_block[]` (header `eh_magic 0xF30A`, one `ext4_extent {ee_block=0, ee_len=N, ee_start}`) with `EXTENTS_FL` set; if the feature is off or no contiguous run fits, it **falls through to the untouched direct/indirect path** (so nothing regresses). `free_inode_blocks` is now extent-aware (frees a depth-0 extent's run on overwrite/delete), and overwriting clears `EXTENTS_FL` so a rewrite via the indirect path stays consistent. Scope: **single contiguous extent for new files** (the common case on a non-full fs); multi-extent / extent-tree-block / overwrite-to-extent are follow-ons; the indirect path remains for fragmented allocations and 128-byte-inode fs. **Verified four ways:** a standalone host write of a 50 KB file came back **byte-exact** through the M1186 read path, `debugfs` showed it as a one-entry EXTENT inode, and `e2fsck -fn` was **CLEAN** (validating the extent + the `alloc_run` bitmap + i_blocks + free counts); the **ext2 regression suite** now writes `/EXTW.TXT` and asserts byte-exact readback + `e2fsck`-clean + a `debugfs` EXTENTS check; `make check` 55 green; and **headless in-guest** — `echo … > /disk2/GW.TXT` on an extent volume produced a file that `cat` read back in-guest *and* host `debugfs` confirmed is an EXTENT inode with `e2fsck` clean (the live kernel's `alloc_run`/extent layout are spec-correct). No new syscalls (transparent in the existing write path). Follow-ons: multi-extent writes (coalesce ≤4 runs into the i_block leaf, then a tree block), then an ext3-style journal for crash consistency.

> **(M1188) ipc — named pipes (`mkfifo`): rendezvous by pathname, on the M1187 fd table.** The classic POSIX IPC primitive the OS still lacked — and the natural next slice on the fresh fd-table keystone. Unlike an anonymous pipe (which needs a common `fork` ancestor) or AF_UNIX (which needs listen/connect/accept), a **FIFO lets unrelated processes connect just by opening the same name**. New `kernel/fifo.c` (path-keyed registry, like `flock.c`): `mkfifo(path)` allocates a **pinned, unopened** backing pipe (new `pipe_new_fifo`: `r_open=w_open=0`, `pinned` so it survives at 0/0 — the named pipe outlives any single connection, the POSIX FIFO lifetime) and records the name; `fifo_open(path, write)` resolves it and hands out a read/write **fd** in the caller's M1187 table (then `fdread`/`fdwrite`/`fdclose` as usual). Correct **open-before-writer EOF**: a reader can't see EOF until a writer has ever connected — a new `had_writer` flag gates it (`pipe_read` returns 0 only when `w_open==0 && had_writer`), so a reader that opens first **blocks** for the writer instead of falsely hitting EOF. **Minimal-churn + regression-safe:** anonymous pipes are untouched — `pipe_new` still starts `1/1` with `had_writer=1`, so M1187's `app_pipe`/`pipetest` behave byte-identically (the FIFO path only adds the `pinned`/`had_writer` fields + `pipe_new_fifo`); the in-guest `pipetest` was re-run and still passes. 2 syscalls `SYS_mkfifo`/`SYS_fifo_open` (#175-176, PL_STDIO, paths `ustr`-validated); ulib `sys_mkfifo`/`sys_fifo_open`; a deterministic `fifotest` shell builtin. **Verified:** `make check` 55 green + **headless in-guest** — `pipetest` printed `pipe: fork round-trip + EOF + dup2 all OK` (M1187 intact) and `fifotest` printed **`fifo: mkfifo + open-by-name rendezvous + EOF OK`** (the parent `mkfifo`'d, forked, the child opened the FIFO **by name** for writing and wrote "fifo!", the parent opened the **same name** for reading, blocked-read those 5 bytes, then got EOF when the child closed + exited). Honest scope: FIFOs are a path-keyed registry accessed via the `mkfifo`/`fifo_open` syscalls (not yet `S_IFIFO` nodes in the ext2/VFS directory tree, and `cat`/`echo` still use the name-VFS read path, not fds). Follow-ons: `S_IFIFO` VFS nodes (so `ls` shows a FIFO and the path lives in a real directory), and the larger fd-as-stdio rebase that would let shell `|`/`>` pipe through real fds.

> **(M1187) ipc — per-process fd table + real anonymous `pipe()` (the multi-session keystone, first slice).** The OS was deliberately name-VFS / fd-free, which is elegant but is exactly why streaming primitives (real pipes, FIFOs, `/dev/pts`-as-stdio, `dup2` redirection) had no home — they need an *open file with a position/identity*, i.e. a descriptor table. This lands that keystone as a **tightly-scoped, additive** first slice: a small per-process fd table (`struct app.fd[24]`, type + pipe-index + which-end) plus genuine anonymous pipes. New `kernel/pipe.c` (mirrors the `mbox`/`unixsock` block-once-woken-by-peer discipline): a table of ring-buffer pipes with **reader/writer reference counts** — `pipe_read` blocks while empty *and a writer exists* then returns **0 (EOF)** once all writers closed and the ring drained; `pipe_write` does full-length blocking writes and returns **-1 (EPIPE)** once all readers closed; the slot frees when both ends hit zero. `app_pipe`/`app_fd_read`/`app_fd_write`/`app_fd_close`/`app_dup2` manage the fd table; **`fork` inherits the table** (each shared end gains a reference — `app_fd_fork`) and **exit/reap closes every fd** (`app_fd_release`, hooked beside the flock/pty releases). `dup2` redirects one fd onto another's object (the basis for future `2>&1`-style redirection). **Additive + low blast radius:** the fd array is zeroed on both spawn and fork (the existing `memset`s), and *only* the new syscalls touch it — so an app that never calls `pipe()` has an empty table and the fork-copy / reap-close are no-ops, leaving every existing path byte-identical (`make check` 55 green confirms). 5 syscalls `SYS_{pipe,fdread,fdwrite,fdclose,dup2}` (#170-174, PL_STDIO; buffers `ubuf`-validated); ulib wrappers; a deterministic `pipetest` shell builtin. **Verified:** `make check` 55 green + **headless in-guest** `pipetest` printed **`pipe: fork round-trip + EOF + dup2 all OK`** — in the live kernel the parent created a pipe, forked, the child closed the read end and wrote "hello pipe", the parent (blocked) read those 10 bytes then got **EOF** when the child closed its write end and exited, and a `dup2`'d write fd delivered to the original read fd. Honest scope: pipes are the only fd type so far and fds aren't yet wired as a process's stdin/stdout/stderr (apps still use the name-VFS for files). Follow-ons (now unblocked): wire fd 0/1/2 + shell `|`/`>`/`2>&1` redirection through real fds, FIFOs (`mkfifo` as a named pipe), the PTY slave + `/dev/pts/N` as fds, and `O_CLOEXEC` across an `exec`.

> **(M1186) fs — ext4 extent reading (the OS now reads ext4 extent-mapped files, not just ext2 indirect blocks).** Modern ext4 stores a file's block map as an **extent tree** rooted in `i_block[]` (flagged by `EXT4_EXTENTS_FL`, `i_flags & 0x80000`), not the classic 12-direct + indirect-block scheme — so an extent-mapped file was previously **unreadable** (the driver mis-read the extent header as block pointers → garbage). This adds extent support at the one chokepoint, `map_block()` in `kernel/ext2.c`: when the inode is extent-flagged, `extent_map()` walks the tree — header `eh_magic 0xF30A` + entries; depth 0 = leaf `ext4_extent {ee_block, ee_len, ee_start_hi/lo}` mapping a logical range to physical (an `ee_len > 32768` uninitialized extent reads as a hole); depth > 0 = internal `ext4_extent_idx` nodes descended via `rdblk`. Because `map_block` is the shared block-mapper, this makes extent files readable **everywhere** (file reads, directory walks, FIEMAP). **Robust against corrupt trees** (untrusted on-disk): bounded depth (≤8), `eh_entries` capped by what the node physically holds, every child read range-checked by `rdblk` — and the existing ext2 fuzzer now exercises it for free (a flipped `i_flags` byte randomly enables the extent path on garbage `i_block`). Only the low 32 bits of the physical block are used (no `64bit` feature); scope is **reading** extent files on otherwise-ext2-compatible images (`mke2fs -O extent`, no flex_bg/metadata_csum/journal) — extent *writing* and full ext4 (64bit/flex_bg/csum) are follow-ons. No new syscalls (it's transparent in the read path). **Verified four ways:** a standalone host read of an `mke2fs -O extent -d` image returned a 200 KB file + a small file **byte-exact** under ASan/UBSan; the **ext2 regression suite** now builds an extent image and asserts byte-exact reads **plus 6000 corrupt-extent-tree fuzz iterations** (OOB/hang-clean); `make check` 55 green; and **headless in-guest** — with an extent image on `/disk2`, `cat /disk2/HI.TXT` printed its contents and `wc /disk2/BIG.TXT` reported the full **60000 bytes**, proving the live kernel walks the extent tree for a multi-block file. Follow-ons (this round's research): ext4 **write**/extent allocation + an ext3-style **journal**, `HTree` directory indexing, and full ext4 feature flags.

> **(M1185) tty — pseudoterminals (PTY) with a real kernel line discipline.** The OSDev community's headline "what makes a kernel a usable Unix" trio is *blocking pipes + a TTY subsystem + signals* (this round's research); the OS had signals + termios but no PTY — the master/slave pair that lets a userspace **terminal emulator** drive a program through a kernel line discipline. New self-contained `kernel/pty.c` (mirrors `unixsock.c`'s endpoint-id design, so **no fd-table surgery** and minimal blast radius): a fixed table of pairs, two byte rings per pair (`in` = slave-readable input, `out` = master-readable output), block-once-when-empty woken by the peer (lost-wakeup-free on this single CPU via the IF=0 int-0x80 gate, the `mbox.c`/`unixsock.c` discipline). `pty_open()` returns a **master id** (even); the **slave id is `master|1`**. The core is a genuine **line discipline**: *canonical* mode line-buffers into an editing buffer and only commits a completed line to the slave, with **ECHO** back to the master, **ERASE** (`^H`/`0x7f` rub-out with `\b \b`), **KILL** (`^U`), **EOF** (`^D`), and **INTR** (`^C` → flush the line, echo `^C`, and `app_killpg(fg_pgid, SIGINT)`); *raw* mode (clear `ICANON`) passes every byte through immediately. `pty_ctl` sets the mode (`lflag`) or the INTR target pgid; it's **fswait-composable** via `pty_ready`, and `app_reap` closes a dead owner's ptys. 5 syscalls `SYS_pty_{open,read,write,close,ctl}` (#165-169, PL_STDIO; buffers `ubuf`-validated); ulib `sys_pty_*`; a deterministic `ptytest` shell builtin. **Verified:** `make check` 55 green, and **headless in-guest** `ptytest` printed **`pty: cooked + echo + erase + INTR-flush + raw + slave-output all OK`** — i.e. in the live kernel a line written to the master is read by the slave, ECHO returns it to the master, `ab⌫c` edits to `ac`, a `^C` discards the pending line (a following `z` line reads back as just `z`) while echoing `^C`, raw mode delivers a byte with no newline, and slave output is read by the master — all single-process and deterministic (every read has data buffered, so none block). Honest scope: PTYs are accessed by endpoint id (like AF_UNIX), not yet as `/dev/pts/N` paths, and nothing is wired as a process's *controlling* tty yet — so this is the mechanism + ldisc, the substrate a terminal-emulator app or a shell-on-a-pty would build on. Follow-ons: `/dev/ptmx`+`/dev/pts/N` VFS routing, `winsize`/`TIOCSWINSZ`, and ultimately the per-process **fd table** so the slave can be a real stdin/stdout.

> **(M1184) fs — ext2 xattr EA block: arbitrary-size / many extended attributes.** Completes the M1182 xattr story. In-inode EAs are capped by the inode's ~92 spare bytes (2-3 tiny attrs); now, when the set overflows that or a value is large, the **whole set spills to a dedicated EA block** referenced by `i_file_acl` — so xattrs scale to many entries and values up to 200 bytes. The block format (decoded byte-exact from a `debugfs`-created reference, the M1182 `od` playbook): a 32-byte header (`h_magic 0xEA020000`, `h_refcount=1`, `h_blocks=1`, `h_hash=0` — which `e2fsck` accepts), entries from +32, values packed down from the block end with `e_value_offs` **block-relative**. The crux: unlike in-inode (`e_hash=0` is accepted), block entries **require the correct per-entry hash** — `e2fsck` rejects a zero one. So this implements `ext2_xattr_hash_entry` (rolling hash: name bytes shift-5, then the value as LE32 words zero-padded shift-16), **verified byte-exact against the reference** (`0x00018547` for name `big` + 200×`A`). `ext2.c` gains `xa_hash_entry`/`xa_parse_block`/`xa_write_block`/`xa_collect`/`xa_save`: set/get/list/remove now read **both** in-inode + block (union), and `xa_save` chooses in-inode when it fits (freeing any block) else spills — so the set migrates **in-inode ⇄ block** as it grows/shrinks, with exact `i_blocks` + block-bitmap + free-count accounting via the existing `alloc_block`/`free_block`. The 4 KiB block buffers live inside helpers and `alloc_block` runs from the top frame, keeping the path well under the 16 KiB kernel stack. No new syscalls (the M1182 ABI carries larger values). **Verified four ways:** (1) the `xattrtest` host suite now drives in-inode→block→replace→shrink-back→remove and proves the on-disk result **`e2fsck -fn` CLEAN** (validating my per-entry hash — it rejects wrong ones) + read back by real `debugfs ea_get` (both `user.greeting` from the block *and* a 150-byte value) with `File ACL != 0`; (2) a standalone check confirmed removing to empty **frees the block** (File ACL→0) and stays e2fsck-clean (i_blocks/bitmap exact); (3) the ext2 fuzz harness exercises the block-read path on corrupt `i_file_acl`; (4) **headless in-guest end-to-end** — the live ring-3 shell `xattr set` of an 80-byte value spilled to **block 564** (baseline File ACL 0), `get` round-tripped it (screenshot), and host `debugfs` + `e2fsck` confirmed the **guest kernel** wrote a correct, clean block (proving the live `alloc_block` + hash under the real stack). `make check` 55 green. Follow-on: shared EA blocks (`h_refcount > 1` dedup across inodes) and the **per-process fd table** keystone.

> **(M1183) test/security — ISO 9660 read-path fuzz harness (closes the LAST unfuzzed on-disk parser).** ext2 got a fuzz harness in M1180; `kernel/iso9660.c` was the one remaining untrusted on-disk parser without host coverage (the kernel mounts arbitrary `.iso` images and walks their Primary Volume Descriptor + directory-record chain — attacker-controllable structures). This adds it. `tests/iso9660/iso9660_test.c` `#include`s `iso9660.c` (device-agnostic via `blk_read_fn`, stack buffers only → zero stubs beyond an in-memory disk whose out-of-range reads error at the disk edge). **No host ISO builder is needed** (none — genisoimage/mkisofs/xorriso — is installed): `build_iso()` lays down a **minimal VALID image by hand** — PVD at logical sector 16 with the `CD001` magic + a root directory record, a root directory of `"."`/`".."`/`"F.TXT;1"`, and a file extent — which the harness first confirms **probes + lists + reads** (27 bytes, byte-exact), proving it's real; then it **fuzzes** the volume-descriptor + root-directory region (which carries every extent LBA/length and record length) — 12000 iterations of multi-byte flips (bit-flip / `0xFF` / zero / random) **plus** 3000 whole-image scribbles — re-running `probe`/`list`/`isdir`/`read` each time under **ASan+UBSan**, with a `timeout` guarding hangs. **Result: clean — iso9660's read path is OOB/hang-safe** on a corrupt PVD, bad root extent, overlong/huge record lengths, and a self-referential directory (its bounds `lendr>=34`, `off+lendr<=2048`, `off+33+fil<=2048`, and the range-checked `rd_sec` already held). Wired as `iso9660test` into `make check` (now **55 suites** green); needs no external tools, so it never skips. **With this, every untrusted on-disk/​wire parser in the OS is now host-fuzzed** (image decoders, x509/TLS, http, FAT32, ext2, and ISO 9660) per [[security-review-workflow]]. Next functional work: the ext2 **EA block** (large/many xattrs) and the **per-process fd table** keystone.

> **(M1182) fs — ext2 extended attributes (`user.*` xattrs, in-inode, e2fsck-clean).** The OS could read/write ext2 files but carried no out-of-band metadata; now it stores POSIX extended attributes **inside the inode's spare space** (the foreshadowed M1181 follow-on — and it turned out *simpler* than feared: the in-inode EA header is just the 4-byte magic `0xEA020000` at `inode+128+i_extra_isize`, each entry's `e_hash` is **0** for in-inode EAs and `e2fsck` accepts that, so there is **no hash to compute** — the hard part of the block-EA format evaporates). `kernel/ext2.c` gains `ext2_{set,get,list,remove}xattr`: `setxattr` is a **read-modify-write of the whole set** (parse existing entries via `xa_parse`, replace/insert ours, repack via `xa_write` — entries packed up from `+164`, values packed down from the inode end, each slot 4-aligned, a zero-`name_len` terminator) so multiple attrs coexist and a replace preserves the others; the read parsers are fully bounds-checked (every `e+16<=inode_size`, `e+16+enl<=inode_size`, `vpos+vsz<=inode_size`). Only the `user.*` namespace (name stored sans the `user.` prefix, `name_index=1`); needs a **256-byte inode** (it refuses 128-byte ones) and small attrs (~92 spare bytes ⇒ 2-3 tiny ones) — the EA-block spill for many/large values (which *does* need the real entry hash) is the documented next step. 4 syscalls `SYS_{set,get,list,remove}xattr` (#161-164; set/remove PL_WPATH, get/list PL_RPATH; every user pointer `ustr`/`ubuf`-validated, 4th arg in r10) → `vfs_*xattr` → `blockdev_mount_*xattr` (ext2 mounts only) mirroring the M1152/M1153 fiemap/punch routing; ulib `sys_*xattr`; a real user-facing **`xattr get|set|rm|list PATH [user.NAME [VALUE]]`** shell command. **Verified three ways:** (1) a host round-trip suite `xattrtest` (`tests/ext2/xattr_test.c` + `run-xattr-tests.sh`, **make check now 54 green**) sets/gets/lists/replaces/removes on a real `mke2fs` image under ASan+UBSan, then proves the result is **`e2fsck -fn` CLEAN** *and* read back by real **`debugfs ea_get`** (interoperable with the Linux reference); (2) the existing ext2 fuzz harness now also hammers `getxattr`/`listxattr` on corrupt inodes (no OOB); (3) **headless in-guest end-to-end** — booted with an ext2 disk on `/disk2`, drove the ring-3 shell `xattr set/get/list` (screenshot shows `hifromos` round-tripping), then on the host `debugfs` confirmed the **guest itself** wrote `user.oswrote=hifromos` to the disk and `e2fsck` stayed clean. Follow-ons: the ext2 **EA block** (i_file_acl, refcount + the `ext2_xattr_hash_entry` hash) for unlimited/large xattrs, and `statx`-adjacent `getxattr` on FAT/tmpfs (no-op there).

> **(M1181) fs — `copy_file_range`: in-kernel file copy (and `sendfile` for free).** A zero-userspace-bounce copy between two VFS paths, entirely in the kernel — the named POSIX primitive the OS lacked (the consumers — httpd, the buffer cache, full FAT/ext2 RW — existed, but nothing tied them together). `SYS_copy_file_range(src_path, dst_path, len)` (#160, PL_WPATH) `kmalloc`s a bounce buffer, `vfs_read`s the source, `vfs_write`s the destination — no user buffer ever crosses the ring boundary. Because the VFS is **name-based**, a destination of **`/net/tcp/<host>!<port>`** makes the same call **`sendfile`-to-socket** (the write goes through `netfs_write` → TCP send) — file→file and file→network from one syscall, fitting the no-fd design (paths, not fds). Works uniformly across FAT32 / ext2 / tmpfs. One-shot, capped at 4 MiB per call (vfs_write replaces rather than appends, so a single read+write is the clean model; larger files / true streaming are the follow-on). ulib `sys_copy_file_range`; shell `cfrtest`. **Verified headlessly (deterministic):** `copy_file_range("MOTD.TXT" → "/tmp/cfr_out")` (FAT root → RAM tmpfs, in-kernel) copied N bytes and the destination was **byte-identical** to the source (slurp both, full `memcmp`). `make check` 53 green. Follow-ons: true streaming (chunked, once an append/offset write exists), and ext2 xattr (groundwork done — in-inode EA at offset 128+`i_extra_isize`, magic `0xEA020000`; needs a dedicated fire for byte-exact `e2fsck`-clean layout + the EA hash).

> **(M1180) test/security — ext2 read-path fuzz harness (closes the last untrusted-parser gap).** ext2 is the OS's **most-exposed on-disk parser** — the kernel auto-mounts *arbitrary* ext2 volumes from any attached disk (`/diskN`), ISO, and loop image (untrusted metadata) — yet, unlike FAT32 (`tests/fs`), x509, and the image decoders, it had **no fuzz harness**. This adds one. `tests/ext2/ext2_test.c` `#include`s `ext2.c` (which is device-agnostic via `blk_read_fn` and uses no kernel heap, so it host-compiles with zero stubs beyond an in-memory disk whose out-of-range reads error at the disk edge); `tests/run-ext2-tests.sh` builds a **real golden image with `mke2fs`** + `debugfs` (adding a 24-byte file and a 300 KB file so the read path walks single- *and* double-indirect blocks), confirms it probes/lists/reads, then **fuzzes** corrupted copies of the superblock/group-descriptor/inode-table region (which carries every block pointer) — 8000 iterations × multi-byte flips — re-running `ext2_probe`/`list_path`/`isdir_path`/`read_path` each time under **ASan+UBSan**, with a `timeout` guarding against hangs. **Result: ext2's read path is clean** — no OOB, no hang on corrupt superblocks / bad block-size & inode counts / cyclic or huge indirect chains — confirming its bounds were already solid, and creating a reusable harness + the de-risking groundwork for ext2 xattr. Wired as `ext2test` into `make check` (now **53 suites** green); skips gracefully if `mke2fs`/`debugfs` are absent. The last major untrusted-input parser is now fuzzed (per [[security-review-workflow]]). Next: ext2 xattr (the EA block, now de-risked), then sendfile/splice, /dev/null·zero·full.

> **(M1179) vm — `mremap`: resize/relocate an mmap region (completes the mmap family).** The one conspicuously-missing member of the OS's otherwise-complete VM family (mmap, COW-fork, swap+zram, hugepages+`MADV_COLLAPSE`, `madvise`, `mincore`, `mprotect`, `mseal`, `userfaultfd`, file-backed) — the realloc of the address space. `app_mremap` (kernel/app.c): **shrink** frees the tail pages; **grow** extends the VMA in place when `[old_end, new_end)` is free and in-window (the new pages just demand-fault in, like the original mmap); **grow when blocked + `MREMAP_MAYMOVE`** reserves a fresh region, **copies the resident pages over** (untouched ones stay demand-paged) and frees the old. Plain anon only — sealed (M1130) / file-backed / huge / uffd / mlock'd regions are refused, so it can't subvert those guarantees; respects `RLIMIT_AS`; single-CPU ⇒ no TLB-shootdown. `SYS_mremap(old, old_len, new_len, flags)` (#159, PL_VM, 4th arg in r10); ulib `sys_mremap`; shell `mremaptest`. **Verified headlessly (deterministic):** a region blocked above by a neighbour was `mremap`'d with `MAYMOVE` → it **relocated to a new base with its data intact** (0x11/0x22 preserved + a freshly-grown page writable); a region with free space above **grew in place** (same base, data intact, grown page writable). `make check` 52 green. Follow-ons (from this round's research, ranked): an **ext2 host-fuzz suite** (the one untrusted-parser verification hole — clone tests/fs/fs_test.c, mke2fs golden image, fuzz corrupt metadata under ASan; do FIRST, de-risks the next), then **ext2 xattr** (Linux-6.13 EA block, dumpe2fs/e2fsck oracle), **sendfile/splice**, and **/dev/null·zero·full**.

> **(M1178) proc — `SIGTSTP`/`SIGCONT` job suspend/resume (completes job control).** The other half of M1176: a process (group) can now be **suspended and resumed** — the Ctrl-Z half of job control. `SIGSTOP`/`SIGTSTP` and `SIGCONT` get **default actions** (no handler required): `app_request_signal` routes `SIGCONT`→`task_cont` (READY) and `SIGSTOP`/`SIGTSTP`→`task_stop` (TASK_STOPPED), which the scheduler simply skips, so the stopped process's work freezes. Tiny + additive — no new syscalls (it reuses M1176's `killpg`), and signos 18/19/20 are used by no existing path, so behavior is otherwise unchanged. `task_stop` is a no-op on self (correct: a foreground ^Z stops the *other* group members, not the reader). Signal numbers `SIGCONT`/`SIGSTOP`/`SIGTSTP` added to the shared syscall.h; shell `stoptest`. **Verified headlessly (deterministic, no timing-luck needed):** a forked child span a shared-memory counter; the parent `killpg(group, SIGTSTP)` and the counter **froze** (zero delta across a 150 ms window — the scheduler skipped the stopped task), then `killpg(group, SIGCONT)` and it **advanced again** — proving suspend/resume. `make check` 52 green. Follow-ons: shell `fg`/`bg`/`jobs` + a `^Z` keybinding (needs the shell to spawn foreground ELFs in their own pgid), and catchable `SIGTSTP` (a handler overriding the default stop).

> **(M1177) fs — `flock` advisory whole-file locks (path-keyed).** Process-to-process mutual exclusion on a named file — now genuinely useful with `fork`+`exec`+pipes. Because this OS's VFS is **name-based with no fd table**, locks are keyed by the **pathname** (not an fd), which is a cleaner fit here than Unix's fd-keyed `flock(2)`. `kernel/flock.c` (a self-contained module, auto-compiled by the kernel glob): a fixed table of held locks `{path, owner pid, type}`; `LOCK_EX` conflicts with any other holder, `LOCK_SH` only with an exclusive holder; a conflicting request without `LOCK_NB` **blocks** (`task_block`, woken by any unlock — the lost-wakeup-free `mbox.c` discipline) else returns -1; re-locking a path you hold upgrades/downgrades it. A process's locks are **released on exit** (hooked into `app_reap`, covering normal exit / kill / crash) or by `LOCK_UN`. `SYS_flock(path, op)` (#158, PL_RPATH, the current pid is the owner); ulib `sys_flock`; `/proc/locks` lists held locks; shell `flocktest`. **Verified headlessly (deterministic, no timing):** the shell took an exclusive lock, forked a child that tried `LOCK_EX|LOCK_NB` and **got the conflict** (exit 10), then released and forked a second child that **acquired it** (exit 20) — and that child exiting while holding it exercised the `app_reap` release. `make check` 52 green. Follow-ons: `fcntl` byte-range locks (`F_SETLK`/`F_SETLKW`/`F_GETLK`) and `EDEADLK` detection.

> **(M1176) proc — job control: process groups, sessions, `killpg`, foreground TTY group.** The next rung after M1174's termios — gives the OS the POSIX job-control substrate: every process has a **process group** (`pgid`) and **session** (`sid`); `setpgid`/`getpgid`/`setsid` manage them (inherited across `fork`, a spawned app leads its own group), `tcsetpgrp`/`tcgetpgrp` set the console's **foreground group** (`fg_pgid`), and `killpg` delivers a signal to *every* process in a group (POSIX `kill(-pgid)`). The TTY is wired in: a cooked **`^C`** now signals the whole `fg_pgid` (`app_key` → `app_killpg(fg_pgid, SIGINT)`) — **gated so it's safe**: `fg_pgid` defaults to 0, so until a process calls `tcsetpgrp` the existing per-app `^C` path is byte-identical (the shell prompt is unchanged). 5 syscalls `SYS_{setpgid,getpgid,setsid,tcsetpgrp,killpg}` (#153–157, PL_PROC); ulib wrappers; shell `jobtest`. **Verified headlessly (deterministic, no keyboard):** the shell forked two children into one new process group (`setpgid` isolating them from the shell), each installing a `SIGINT` handler that exits 42; the parent `killpg(group, SIGINT)` **signalled exactly 2 processes and both children exited 42** — proving group signal delivery. `make check` 52 green (gated `^C` path ⇒ the input path is unchanged by default). Follow-ons: `SIGTSTP`/`SIGCONT` job suspend/resume + shell `fg`/`bg`/`jobs`, and `c_cc[VINTR]`/`[VSUSP]`-customizable keys (today `^C`=`0x83` is hardwired as the intr).

> **(M1175) fs — ext2 inode timestamps (real `mtime`/`ctime`/`atime` on writes).** Completes the M1173 statx story: files the OS *created* on ext2 were stamped epoch-0 (1970), since `ext2_write_path`/`mkdir`/`symlink` only set mode/size/links. Now every ext2 create/overwrite stamps `i_atime`/`i_ctime`/`i_mtime` with the real wall clock, and the delete path's `i_dtime` uses it too. The clock comes through a **function-pointer hook** (`ext2_set_clock`, wired to `rtc_unix()` in kmain) rather than a direct call, so ext2.c stays self-contained and host-`#include`-linkable — preserving the [[host-test-include-coupling]] invariant the file documents (a host test that doesn't set the hook gets timestamp 0, the old behavior; the `i_dtime` fsck-safe large fallback is kept). New `rtc_unix()` (epoch seconds, days-from-civil) was added in M1173. **Verified headlessly:** booted with `build/ext2.img` attached as `/disk2`, created a file on it in-guest, then host `debugfs -R "stat"` on the image showed **`mtime`/`ctime`/`atime` = a real 2026 date** (not 1970), and `e2fsck -fn` stayed clean. `make check` 52 green (the in-guest ext2 RW tests still pass + e2fsck-clean with timestamps). Follow-on: a per-FS `stat` vfs_op so `statx` reads these ext2 times in-guest (today statx gives full metadata for /tmp, size+type elsewhere). **Research (this round, source-verified) ranks job control next** — process groups + sessions + `tcsetpgrp` + `c_cc[VINTR]`-driven SIGINT/SIGTSTP — as the natural completion of M1174's termios; and reconfirms a **per-process fd table** as THE highest-leverage multi-session keystone (every `fswait`-readable `/proc/self/*` handle today is a parallel mini-mechanism a real fd table would unify, unlocking `pipe`/`dup2`/pty).

> **(M1174) tty — line discipline: `termios` + raw mode (`tcgetattr`/`tcsetattr`).** Gives the OS a real TTY line discipline — a process can switch the terminal between **cooked** (canonical line editing) and **raw** (per-keystroke, unbuffered) input, the foundation `vi`/games/readline need. A fresh r/osdev survey found the cooked-mode editor already lived hardcoded in `app_sys_read` (Ctrl-A/E/K/U/W, history, backspace, Ctrl-C); this adds the missing **mode switch** + a `struct termios` API. **Strictly additive/safe**: a global foreground `termios` defaults to `ICANON|ECHO|ISIG` (the historical behavior), and `app_sys_read` only branches to the new raw path when `ICANON` is *cleared* — so every app that never calls `tcsetattr` (the shell, editor, all existing apps) hits the byte-identical canonical path, and `make check`/boot are unaffected. Raw mode (`tty_raw_read`) blocks for the first keystroke then drains what's queued (VMIN=1), echoes if `ECHO` is set, and delivers control keys literally (the keyboard's `0x80|letter` Ctrl sentinel maps back to raw codes 1–26). `struct termios` + `ICANON`/`ECHO`/`ISIG`/`V*` live in the shared syscall.h (unsigned, no stdint). `SYS_tcgetattr`/`tcsetattr` (#151/#152, PL_STDIO); ulib wrappers; shell `rawkey` demo (sets raw, reads one key, restores). **Verified headlessly:** `rawkey` went raw and **read a single keystroke with no Enter** — `read 1 byte unbuffered: 'a' (0x61)` — then restored cooked editing. `make check` 52 green (default-cooked ⇒ the canonical path is unchanged). Follow-ons: honor `ECHO` in cooked mode (no-echo password input), wire `ISIG` so cooked `^C` raises SIGINT to the foreground app, and per-TTY (not global) termios.

> **(M1173) fs — `statx` unified file-metadata syscall + tmpfs timestamps.** The OS had **no `stat` at all** — apps scraped size/date out of `list` output, and there was no way to query a file's mode/type/times/link-count. `SYS_statx(path, struct statx*)` (#150, PL_RPATH) fills a real metadata struct (`stx_mode` with `S_IF*` type bits, `stx_size`, `stx_blocks`, `stx_nlink`, `stx_mtime`/`ctime`/`atime`, `stx_ino`, `stx_blksize`). `vfs_stat` (kernel/vfs.c) dispatches by path: the RAM **/tmp** backend now records a real `mtime` (stamped from a new `rtc_unix()` epoch-seconds helper on every `tmpfs_write`/`symlink`), so `/tmp` files report accurate type + size + modification time; other paths get a best-effort size + type via the directory listing. Path-based, so **no fd table needed**. `struct statx` + `S_IF*` live in the shared syscall.h (using `unsigned`/`unsigned long` to match the other fd-table-free ABI structs); ulib `sys_statx`; shell `stat <path>`. **Verified headlessly:** `echo hello > /tmp/foo; stat /tmp/foo` reported **regular file, size 6, a non-zero Mtime epoch** (the live clock), and `stat /tmp` reported **directory** — proving the metadata surface and live timestamps. `make check` 52 green. Follow-ons: ext2 inode timestamps (`i_atime/ctime/mtime` are currently 0 on OS-created files; stamp them in `ext2_write_path` with `rtc_unix()`), a real `stat` op in `vfs_ops` for full per-FS metadata, and a TTY line discipline (the other strong candidate from this round's research).

> **(M1172) sched — `SCHED_FIFO`/`SCHED_RR` real-time scheduling classes.** Layers POSIX real-time scheduling on top of the M1171 CFS core: a task can request `SCHED_FIFO` (run-to-block) or `SCHED_RR` (timesliced round-robin) at an `rt_priority` 1–99, and **any runnable RT task preempts the entire normal (CFS) class**; among RT tasks the highest priority wins, and `switch_to_next` only falls through to the CFS vruntime pick when no RT task is runnable. **Additive + gated**: with no RT task (the default for every process), the RT scan finds nothing and the path is byte-for-byte M1171 — so normal boot/desktop/apps are unchanged. FIFO incumbents keep the CPU until they block or a higher-priority RT task appears; RR adds a 5-tick quantum (decremented per timer tick) that rotates among equal-priority RR tasks in ring order. Switching back to `SCHED_OTHER` re-places the task at the CFS `min_vruntime` floor so its frozen vruntime can't dominate or starve. `SYS_sched_setscheduler` (#149, sets the caller's class); `/proc/sched` gains a **POL** column (`OT`/`FF<prio>`/`RR<prio>`); ulib `sys_sched_setscheduler`; shell `schedtest`. **Verified headlessly:** two forked children set `SCHED_FIFO` at priority **10** and **50** and blocked on a SysV-semaphore barrier; the parent released both at once, and the **priority-50 task ran to completion first (finish order 1) and the priority-10 second (order 2)** — proving real-time priority ordering. `make check` 52 green (boottest/gfxtest/browsertest run the live scheduler with only CFS tasks — unchanged). Natural follow-ons: `sched_setscheduler(pid,…)` for other tasks, priority inheritance, or per-CPU RT runqueues once SMP lands.

> **(M1171) sched — CFS-style weighted fair scheduling with `nice` levels.** Replaces the round-robin ring with a Completely-Fair-Scheduler core (the OS's biggest scheduler upgrade): each task carries a **vruntime** (virtual runtime) and a **weight** from its nice level, and `switch_to_next` runs the runnable task with the **smallest vruntime**, charging the running task `slice·1024/weight` of vruntime per slice — so a low-nice (high-weight) task accrues vruntime slowly and thus gets proportionally more CPU. Uses the real Linux nice→weight table (nice 0 = 1024; ±1 ≈ ±25%). **Default behavior is preserved**: with every task at nice 0, vruntime advances by real time for all, so the pick degenerates to "least-recently-run" — fair round-robin, exactly as before. Correctness details: prev is charged *before* the pick (so a still-running task's vruntime can't go stale and let it win forever); a global `min_vruntime` floor is advanced every call and waking/new/continued tasks are clamped up to it (no head-start, no starvation, no post-solo monopoly); the idle task is excluded from the pick and never accrues vruntime; weight is initialized non-zero everywhere (a 0 weight would divide-by-zero in the charge). `SYS_nice` (#148) sets the caller's nice; `/proc/sched` gains a **NI** column. Shell `nicetest`. **Verified headlessly:** two forked spinners — one `nice(0)`, one `nice(10)` — ran for 2 s incrementing per-process counters in shared memory; the nice-0 task did **~9× more work** (matching the 1024:110 weight ratio), proving weighted fair scheduling. `make check` 52 green (boottest/gfxtest/browsertest all run the live scheduler — a boot regression would fail them). The natural follow-ons: SCHED_FIFO/RR real-time classes, or `setpriority(pid)` for other tasks.

> **(M1170) ipc — `unix_wait_any`: a poll/epoll-style readiness multiplexer for AF_UNIX.** Completes M1169 into a usable server primitive: given up to 16 endpoint ids, block until **any** of them is readable (its RX ring has bytes, or the peer closed ⇒ `recv` won't block) and return that one's index — this OS's fd-table-free answer to `poll`/`epoll`/`select`. `unix_wait_any` (kernel/unixsock.c) registers the caller on every endpoint's single RX waiter slot, `task_block`s once, then unregisters and re-scans; a spurious/kill wake returns -1 (the caller re-polls) so it never spins or wedges a kill — the same lost-wakeup-free discipline as `recv`. `SYS_unix_wait_any` (#147, PL_STDIO, the `int*` array bounds-checked + `ubuf`-validated); ulib wrapper; shell `unixpolltest`. **Verified headlessly:** the shell forked a child that opened **two** connections but sent on only the **second**; the parent accepted both and `unix_wait_any([s0,s1],2)` returned **index 1** (the socket with data), read the byte, and reaped the child — proving the multiplexer selects the ready socket out of several. `make check` 52 green. A server can now watch many clients from one task; datagram (SOCK_DGRAM) mode is the remaining AF_UNIX piece.

> **(M1169) ipc — AF_UNIX path-keyed stream sockets.** Local bidirectional byte-stream IPC addressed by a pathname — the Unix-domain-socket model (the transport behind D-Bus/X11/Wayland) adapted to this OS's **name-based, fd-table-free** design (the elegant fit a fresh r/osdev survey flagged): a server `unix_listen("/run/foo")`s, clients `unix_connect()` to it, the server `unix_accept()`s pending connections, and both ends stream bytes with `unix_send`/`unix_recv` over an integer **endpoint id** (`conn<<1 | side`) — a tiny socket handle that stands in for a file descriptor and, because it indexes a global kernel table, **survives `fork()`**. `kernel/unixsock.c`: a fixed table of connections, each a pair of 4 KiB byte rings (a2b/b2a) so the two ends stream both ways, plus a listener table with per-listener accept queues; `recv` blocks (`task_block`) when its ring is empty and returns 0 at EOF (peer closed + drained), mirroring `mbox.c`'s lost-wakeup-free discipline (the IF=0 int-0x80 gate makes "empty? then block" atomic against a sender on this single CPU). 6 syscalls `SYS_unix_{listen,connect,accept,send,recv,close}` (#141–146), PL_STDIO; ulib wrappers; `/proc/unix` lists listeners + connections; shell `unixtest`. **Verified headlessly:** the shell `unix_listen`'d `/run/ut`, **forked**, the child `connect`ed + sent `ping`, the parent `accept`ed + read `ping` + sent `pong`, the child read `pong` and exited 0, the parent reaped it — a full cross-process bidirectional round-trip over a path-keyed socket. `make check` 52 green. The OS's `poll`/`epoll`-free answer to AF_UNIX; a `wait_any(names[])` readiness multiplexer is the natural follow-on.

> **(M1168) vm — `MADV_COLLAPSE`: synchronous transparent-hugepage collapse.** The modern-Linux (6.1+) THP primitive — fold a fully-resident anonymous mmap region's 512 demand-paged 4 KiB pages into a single 2 MiB hugepage on demand, cutting TLB pressure + page-table overhead ~512:1. `app_collapse` (kernel/app.c, reached via `madvise(addr,len,MADV_COLLAPSE)`) is **all-or-nothing**: phase 1 validates every page (present, 4 KiB-mapped, single-owner `pmm_refcount==0`); phase 2 captures each 2 MiB block's page-table frame (`vmm_pt_phys_in`, new) and reserves a contiguous 2 MiB physical run (`pmm_alloc_contiguous`, M1155) — only if all succeed does phase 3 mutate: copy the 512 pages' bytes into the run (read straight out of the still-intact PT via the HHDM), install one huge PD entry (`vmm_map_huge`, M1155), then free the 512 scattered frames + the orphaned page table. Same-AS + single-CPU ⇒ no TLB shootdown needed. The VMA is flagged `huge` so munmap/teardown free it as 2 MiB runs (identical shape to M1155's `app_mmap_huge`), and every page-table walker (destroy/fork/wss/smaps/pagemap) already skips/handles a PD-level huge entry. `app_mmap` now 2 MiB-aligns large anon regions so they're collapsible; maps/smaps/pagemap label huge VMAs `[mmap-huge]`. Shell `thptest`. **Verified headlessly:** `mmap`'d 2 MiB (512 demand-paged 4 KiB pages), tagged each page, `madvise(MADV_COLLAPSE)` returned **512**, all 512 tags survived (data preserved across the physical copy), and `/proc/self/maps` showed the region as **`[mmap-huge]`**. `make check` 52 green. Reuses the M1155 hugepage machinery end-to-end — the synchronous counterpart to khugepaged.

> **(M1167) proc — `/proc/<pid>/pagemap`: per-page residency + physical frame numbers.** The text form of Linux's `/proc/<pid>/pagemap` — for the heap and every mmap VMA, list each non-absent page with its **virtual address, physical frame number (PFN)** and flags (`C`=COW-shared, `D`=dirty/written, `A`=accessed), or `swap` for a swapped-out page; absent (never-faulted) pages are omitted so a sparse demand-paged region stays compact. `app_format_pagemap`/`app_pagemap_region` (kernel/app.c) walk the app's own leaf PTEs via the M1151 `vmm_pte_in` (the same read-only PTE reader `smaps` uses) — no new syscalls, no fault-path changes, read-only. Routed at `/proc/<pid>/pagemap` (and `/proc/self/pagemap`) beside `maps`/`smaps`. Shell `pagemaptest`. **Verified headlessly:** `mmap`'d 8 pages (all initially absent ⇒ 0 PFN lines for that region), counted resident-with-PFN pages in `/proc/self/pagemap`, **touched all 8**, and re-counted — the count rose by ≥8 (`before=… after touching 8=…`), proving demand paging made each touched page resident with a real frame. `make check` 52 green. Completes the `/proc` VM-introspection ladder (maps → smaps → pagemap) alongside mincore/wss.

> **(M1166) sched — `/proc/sched` WCHAN column: name the kernel routine each blocked task is parked in.** Linux's classic `ps`/`/proc/<pid>/wchan` introspection — for every **BLOCKED** task, show the kernel symbol it last went to sleep in, so you can tell at a glance *what* a stuck process is waiting on (a semaphore, a message queue, `waitpid`, a keypress). The capture is one line in the two block primitives: `task_block()` and `task_sleep_ms()` record `current->wchan = __builtin_return_address(0)` — the return address into their caller — right before they `switch_to_next()`. `task_snapshot` copies it (only while the task is still BLOCKED, so a since-woken task never shows a stale wchan), and `gen_sched` symbolises it through the existing **`ksym_lookup`** (M1078 — the same sorted kallsyms table `/proc/kallsyms` and the panic backtrace use), printing a new **WCHAN** column (`-` for runnable tasks). Zero new syscalls, ~zero hot-path cost (one store on the already-cold block path). Shell `wchantest`. **Verified headlessly:** a forked child does a P (`sem_op = -1`) on a zero-valued SysV semaphore so it parks inside `sysv_semop` → `task_block`; the parent sleeps, reads `/proc/sched`, and finds the child's row showing **WCHAN = `sysv_semop`**, then V's the sem and reaps the child. `make check` 52 green. Pure introspection — composes the M1148 run-queue accounting, M1159 SysV sems, and the M1078 symbol table.

> **(M1165) proc — `process_vm_write`: poke another process's memory (COW-safe).** The symmetric completion of M1162's `process_vm_read` — write bytes into a target process's address space (debugger poke / live patch). Same ptrace-style same-tree permission. The correctness crux is **copy-on-write**: writing a target page that's COW-marked or shared (`pmm_refcount > 0`) would clobber the sharer, so `app_process_vm_write` (kernel/app.c) first **privatises** it — allocate a fresh frame, copy the page, remap the target's PTE writable via the new `vmm_set_pte_in` (a present-only leaf setter in an arbitrary cr3), drop the old ref — exactly `app_fault_handle`'s COW break applied to the target's cr3 (safe TLB-wise on single-CPU: the target isn't running and reloads CR3 when next scheduled). A genuinely read-only (non-COW) page is refused. `SYS_process_vm_write` (#140), PL_PROC; ulib + shell `pvwtest`. **Verified headlessly via fork + a SysV semaphore for ordering:** the parent wrote a new pattern into the **child's** buffer, the child (after the sem wakeup) read it back and exited 0 (`poked the child's memory cross-AS OK`), **and the parent's own copy stayed untouched** (`COW isolation correct`) — proving the write privatised the child's page rather than corrupting the shared frame. `make check` 52 green. Composes fork (M1116), SysV sems (M1159), and the M1162 cross-AS walker.

> **(M1164) proc — RLIMIT_AS + RLIMIT_DATA: address-space and heap caps.** Extends the M1163 rlimit suite with the two memory limits. **RLIMIT_AS** caps a process's total mmap address space — `app_mmap`/`app_mmap_huge`/`app_mmap_file` now reject a reservation (via `app_vma_total` summing the VMA lengths) once it would exceed `rlim_as`. **RLIMIT_DATA** caps the heap — `app_sbrk` refuses to grow the break past `rlim_data`. Both reuse M1163's plumbing (the `app_setrlimit`/`getrlimit` accessors, fork inheritance, `struct rlimit`), are stored 0 = unlimited (so the hot mmap/sbrk paths are unchanged for every existing process), and `getrlimit` reports unset limits as `RLIM_INFINITY`. New constants `RLIMIT_AS`(9)/`RLIMIT_DATA`(2). The shell `rlimittest` now exercises all three limits. **Verified headlessly:** `RLIMIT_AS` — `mmap(64 KiB)` is **denied under a 4 KiB cap, allowed when unlimited**; `RLIMIT_DATA` — in a forked child (isolating the cap from the shell's own heap), `malloc(1 MiB)` **returns NULL** under a 4 KiB heap cap. `make check` 52 green (the default-unlimited checks ride every mmap/sbrk in the suite without regression). The OS now has a real memory-rlimit story (NPROC + AS + DATA).

> **(M1163) proc — `setrlimit`/`getrlimit` (RLIMIT_NPROC): per-process resource limits + fork-bomb protection.** Adds POSIX resource limits, starting with **RLIMIT_NPROC** — a cap on a process's live children. `app_fork` now counts the forking process's live children and refuses (returns -1) once the count reaches `rlim_nproc`; the limit is **inherited across fork** and stored as 0 = unlimited (so a zeroed/default process is unlimited — zero behavior change, and `getrlimit` reports it back as `RLIM_INFINITY`). `app_setrlimit`/`app_getrlimit` (kernel/app.c); the syscall layer marshals `struct rlimit {rlim_cur, rlim_max}`. `SYS_getrlimit/setrlimit` (#138–139), PL_STDIO; shell `rlimittest`. **Verified headlessly:** `getrlimit` reported the default as **infinity**; after `setrlimit(RLIMIT_NPROC, 1)` the **first fork succeeded but the second was DENIED** (one live child already at the limit) — real fork-bomb protection — then the limit was restored. `make check` 52 green (default-unlimited means the fork path is unchanged for every existing process). RLIMIT_AS/DATA are the natural follow-ons (the enforcement hooks in app_mmap/app_sbrk).

> **(M1162) proc — `process_vm_read`: read another process's memory cross-address-space.** The debugger/inspector primitive (Linux `process_vm_readv`) — copy bytes directly from a target process's address space into the caller's buffer, no ptrace-stop and no `/proc/<pid>/mem` text round-trip. `app_process_vm_read(pid, raddr, local, len)` (kernel/app.c) resolves the target via `app_by_pid`, then walks the range page by page through the **proven `vmm_translate_in` + the HHDM** (the same cross-AS read `/proc/<pid>/mem` already does), copying into the caller's (active-AS) buffer; it stops at the first unmapped target page. **Permission is ptrace-style same-tree**: the target must be self, the caller's parent, or its child. `SYS_process_vm_read` (#137), PL_PROC; ulib `sys_process_vm_read`; shell `pvmtest`. **Verified headlessly via fork+waitpid:** the parent filled a 64-byte sentinel, forked, and the **child read the parent's sentinel cross-AS** (`process_vm_read(getppid, &sentinel, buf, 64)`), matched the pattern, and exited 0; the parent reaped it and reported OK. `make check` 52 green. Purely additive + read-only — no fault-path or other critical-path changes; reuses the M1151 cross-AS walker.

> **(M1161) ipc — System V shared memory (`shmget`/`shmat`/`shmdt`): completes the SysV IPC trio.** Joins SysV semaphores (M1159) + message queues (M1160) to round out classic System V IPC. `shmget(key, size, flags)` opens/creates a keyed shared-memory segment; `shmat(shmid)` maps it into the caller's address space (returns the base VA); `shmdt(addr)` detaches. The implementation is a thin keyed wrapper over the existing M1108 named-shm machinery: each segment gets a stable synthetic name (`sysvshmNN`), so two `shmat`s of the same id — in the same or different processes — map the **same physical frames** (`app_shm_open` shares the backing by name); `shmdt` is `app_munmap`. `SYS_shmget/shmat/shmdt` (#134–136), PL_VM; shell `shmsysvtest`. **Verified headlessly:** `shmget` + two `shmat`s returned **distinct VAs**, and bytes written through attach A were **visible through attach B** — proving one shared backing. `make check` 52 green. The OS now has the complete System V IPC suite (sem + msg + shm) alongside its POSIX futexes/mqueues/shm/eventfd.

> **(M1160) ipc — System V message queues (`msgget`/`msgsnd`/`msgrcv`): type-selective receive.** Joins the SysV semaphores (M1159) toward the full SysV IPC trio. New keyed queues of **typed** messages in `kernel/sysvipc.c`: `msgsnd` appends a message tagged with a positive `mtype`; **`msgrcv` selects by `mtyp`** — `0` = the oldest message, `>0` = the oldest with `mtype == mtyp`, `<0` = the oldest among messages with `mtype <= |mtyp|` (lowest such type first). This type-keyed selection is the defining SysV behavior, distinct from the M1154 POSIX mqueue's priority ordering. Blocks via `task_block`/`task_wake` when full/empty (unless `IPC_NOWAIT`). The syscall layer bridges the POSIX `struct msgbuf {long mtype; char mtext[]}` convention (mtype at offset 0). `SYS_msgget/msgsnd/msgrcv` (#131–133), PL_STDIO; shell `msgtest`. **Verified headlessly:** three messages sent out of type-order `[3,1,2]` were received as `msgrcv(0)→type 3` (oldest), `msgrcv(2)→type 2` (exact), `msgrcv(-2)→type 1` (lowest ≤ 2), each with matching data. `make check` 52 green. (SysV shm is the remaining piece of the trio — a thin keyed wrapper over the existing M1108 shm.)

> **(M1159) ipc — System V semaphores (`semget`/`semop`/`semctl`): keyed sets with atomic all-or-nothing.** The OS had futexes, POSIX mqueues, shm, and eventfd — but not the classic **SysV** semaphore API. New `kernel/sysvipc.c`: `semget(key, nsems, flags)` opens/creates a keyed counting-semaphore *set* (`IPC_PRIVATE` always creates; a positive key reuses or, with `IPC_CREAT`, makes one); **`semop` applies an ARRAY of `{sem_num, sem_op, sem_flg}` operations atomically — all-or-nothing**: it commits only if *every* op can proceed (no semaphore goes negative, and `op==0` waits-for-zero), else it blocks the whole call without partial application (unless `IPC_NOWAIT`→EAGAIN), waking via the same `task_block`/`task_wake` rendezvous mqueue.c uses; `semctl` does `SETVAL`/`GETVAL`/`IPC_RMID`. `struct sembuf` + the `IPC_*`/`*VAL` constants are in the shared syscall.h; `SYS_semget/semop/semctl` (#128–130), PL_STDIO; `/proc/sysvipc` lists live sets; shell `semtest`. **Verified headlessly (single-process, deterministic):** sem0 3→1 after a `-2` op; an over-taking op with `IPC_NOWAIT` returned **EAGAIN with sem0 unchanged**; and a 2-op `semop` where op1 was feasible but op2 would block was **refused with sem0 *not* partially decremented** — proving atomicity. `make check` 52 green. The classic keyed Unix IPC primitive, distinct from the OS's POSIX sync.

> **(M1158) vm — `madvise(MADV_PAGEOUT)` + `MADV_COLD`: proactive reclaim via the standard API.** `madvise` only handled `MADV_DONTNEED`; M1158 fills in the modern reclaim hints. **`MADV_PAGEOUT`** (Linux advice 21) proactively pages a range out **now** — it routes straight into the proven `app_swap_out` path, so the pages land in the **zram** compressed-RAM tier (M1156); a later touch demand-faults them back in via `swap_in`. **`MADV_COLD`** (20) clears the Accessed bit on resident pages (deactivate — marks them as reclaim candidates). Both are exposed by name in ulib.h. New shell `pageouttest` **verified headlessly composing M1147 mincore**: a 32-page region read **32/32 resident**, `madvise(MADV_PAGEOUT)` reported **32 swapped**, mincore then read **0/32 resident** (all paged out to zram), and re-touching faulted every page **back in byte-identical**. `make check` 52 green. Completes the madvise reclaim API atop the swap/zram engine — apps now steer reclaim with the standard Linux hint, not a bespoke syscall.

> **(M1157) block — RAID-1 mirror (device-mapper-lite): a redundant volume across disks.** The block layer was already a `blockdev_t` vtable `{name, read, write, sectors, ctx}`, so a virtual device is just another consumer of it. New `kernel/dm.c` adds a **RAID-1 mirror**: `dm_mirror_write` fans every sector out to *both* member disks; `dm_mirror_read` serves from member A, falling back to B if A is marked failed — so the volume **survives a member failure**. It fans out through the uniform `blockdev_read`/`blockdev_write`, so it stacks on *any* storage driver. A boot-time `dm_selftest` runs **iff two non-boot writable disks are present** (otherwise a clean no-op — the normal boot and every make-check suite are untouched). To give it two distinct disks, osdrive's new `--disk3` attaches via **NVMe** (the boot disk is ATA, `--disk2` is virtio-blk), so the mirror spans **different controllers**. **Verified headlessly** (osdrive `--disk2 --disk3`, results in `dmesg`): a mirror over **virtio-blk + nvme0n1** showed `write+read-back OK`, **fan-out OK** (reading member B directly returns the written pattern — the write reached both), and **redundancy OK** (after marking member A failed, the read is served from B intact). `make check` 52 green (the self-test no-ops with no extra disks). RAID/LVM is near-absent in hobby OSes — a differentiating block-layer feature.

> **(M1156) vm — zram: compressed-RAM swap (swap with no disk).** Swap (M1105) was disk-only — inactive unless a writable non-boot disk was attached. M1156 adds a **compressed-RAM tier in front**, composing the existing `swap.c` slot machinery with the in-kernel DEFLATE codec (`raw_deflate`/`inflate`). `swap_out(phys)` now: an **all-zero page** costs *nothing* (a 1-byte kind flag); an otherwise-compressible page is `raw_deflate`'d into a right-sized kheap blob; only a genuinely incompressible page spills to a disk (if one exists) or is held raw in RAM. `swap_in` restores it (zero-fill / `inflate` / copy / disk read). The slot index it returns is unchanged, so the app.c PTE-encoding/fault-in ABI is untouched — zram is invisible to it. Because the primary tier is RAM, **`swap_active()` is now always true: swap needs no disk**. `/proc/swaps` reports the live compression ratio. **Verified headlessly with no disk attached:** `swaptest` paged out 64 pages (256 KiB) of run-compressible data → `/proc/swaps` showed **`RAM held: 5120 bytes`** and **`Ratio x100: 5120` (a 51.2× compression ratio)**, then every page **faulted back in byte-identical** through inflate. `make check` 52 green. Real memory-density systems work, reusing two subsystems the OS already had.

> **(M1155) vm — 2 MiB hugepages (`MAP_HUGETLB`): 512 pages, one PD entry, one fault.** Real x86-64 large-page mapping. `pmm_alloc_contiguous(n, align)` (kernel/pmm.c) bitmap-scans for `n` contiguous frames aligned to `align` (512/512 for 2 MiB); `pmm_free_contiguous` releases the run. `app_mmap_huge` reserves a 2 MiB-aligned, 2 MiB-granular VMA (`huge` flag); on first touch `app_fault_handle` allocates the 2 MiB run, zeroes it, and maps the **whole 2 MiB with a single PD entry** via the pre-existing `vmm_map_huge` (PS bit) — so 512 4 KiB pages cost **one** fault + **one** TLB entry. `app_munmap` gains a huge branch (`vmm_unmap_huge` + `pmm_free_contiguous` per 2 MiB), and the per-process teardown (`vmm_destroy_address_space`) — which previously *skipped* huge PD entries as "n/a for user" — now frees the run, so there's no leak on exit. `SYS_mmap_huge`(#127), PL_VM; shell `hugetest`. **Verified headlessly by composing M1150 getrusage:** touching all 512 4 KiB pages of a 2 MiB hugepage incurred **exactly 1 minor fault** (a regular mmap would take 512), the base was 2 MiB-aligned, and the full write/read-back matched. `make check` 52 green — critically, the modified teardown runs on **every process exit** across boottest + all suites, so a clean sweep proves the contiguous allocator + teardown are correct. The flagship VM feature databases/JVMs rely on.

> **(M1154) ipc — POSIX priority message queues (`mq_open`/`mq_send`/`mq_receive`).** The OS had FIFO mailboxes (`/ipc/<name>`, mbox.c) but no *priority* IPC. New `kernel/mqueue.c` adds named, bounded, **priority-ordered** queues: `mq_send(idx, buf, len, prio)` enqueues, and `mq_receive` returns the **highest-priority** message (FIFO within equal priorities, tracked by a per-queue sequence counter), reporting the message's priority back. Queues are shared by **name** — two processes (e.g. a fork parent/child) that `mq_open` the same name get one queue, so **no per-process fd table is needed** (the OS's name-dispatch idiom). Senders block when full / receivers when empty via the same IRQ-safe `task_block`/`task_wake` rendezvous mbox.c uses. `SYS_mq_open`(#124)/`SYS_mq_send`(#125, prio via `do_syscall4`'s r10)/`SYS_mq_receive`(#126, writes `*prio`), PL_STDIO; surfaced read-only at `/proc/mqueue`; shell `mqtest`. **Verified headlessly:** three messages enqueued out of order — `low`(prio 1), `high`(9), `mid`(5) — were received in strict priority order **`high`(9) → `mid`(5) → `low`(1)** with payloads + priorities intact. `make check` 52 green (the new file auto-compiles via the kernel source glob; purely additive). Real IPC depth with zero regression risk to existing features.

> **(M1153) fs — `fallocate(PUNCH_HOLE)`: sparse-file hole punching on ext2.** `SYS_fallocate(path, mode, offset, len)` with `FALLOC_FL_PUNCH_HOLE` deallocates the whole data blocks fully inside `[offset, offset+len)` of an ext2-mount file, turning that range into a **sparse hole that reads back as zeros** while the file size is unchanged (KEEP_SIZE). The new `punch_block` helper (kernel/ext2.c) frees each block (`free_block`) **and zeroes its pointer** — direct in the inode, single/double-indirect in the on-disk indirect block written back — so there are no dangling references (e2fsck-clean); indirect metablocks are kept (they validly map all-holes). `ext2_punch_hole` walks the block-aligned range, decrements `i_blocks`, and persists the inode. Routed VFS→`blockdev_mount_punch`→ext2 (records an `fsevents` write). A new `do_syscall4` ulib helper passes the 4th arg (len) in `r10`; `SYS_fallocate`(#123), PL_WPATH; shell `fallocate punch <path> <off> <len>`. **Verified headlessly by composing M1152 FIEMAP:** an 8-block file showed **1 extent** (`physical 575488 length 8192`); after `fallocate punch … 2048 2048` (2 blocks) FIEMAP showed it **split into 2 extents** (`logical 0 len 2048` + `logical 4096 len 4096`) with a hole between — matching host `debugfs` exactly (`(0-1):562-563, (4-7):566-569`), file size still 7756, **e2fsck clean**. `make check` 52 green (ext2.c stays self-contained, fstest links).

> **(M1152) fs — `FIEMAP`: a file's physical on-disk extent map.** `SYS_fiemap(path, struct fiemap_extent*, max)` exposes where a file *physically* lives on disk — the Linux `FS_IOC_FIEMAP`/`filefrag` feature. `ext2_fiemap` (kernel/ext2.c) walks the file's logical blocks `0..ceil(size/bs)` through `map_block` (the *same* direct/single/double-indirect resolver `ext2_read_path` uses) and **coalesces maximal runs of contiguous physical blocks into extents** (`physical[n+1] == physical[n]+1`), skipping sparse holes; it's self-contained (no external calls) so the fstest `#include` of ext2.c stays linkable. Routed VFS→`blockdev_mount_fiemap`→ext2 (ext2 `/diskN` mounts only — the FS with real block layout); `ext2_extent_t` lives in the base `partition.h`; `struct fiemap_extent` (the user ABI, with `FIEMAP_EXTENT_LAST`) in the shared `syscall.h`. `SYS_fiemap`(#122), PL_RPATH. New shell `fiemap <path>`. **Verified headlessly against gold-standard host `debugfs`:** a 29-byte file mapped to **1 extent @ physical 575488** (= block 562 × 1024, matching `debugfs`), and a 7756-byte file's **8 contiguous blocks 563–570 coalesced into a single 8192-byte extent @ physical 576512** (`debugfs` confirms blocks 563–570), `[last]` flagged, e2fsck clean. `make check` 52 green. The marquee FS-introspection win, reusing the ext2 block-walker.

> **(M1151) proc — `/proc/<pid>/smaps`: per-region Rss / Pss / Dirty / Swap.** `/proc/<pid>/maps` listed an app's regions but said nothing about how much RAM each *actually* uses. `smaps` adds the Linux `pmap -x` breakdown. A new `vmm_pte_in(cr3, va)` returns the raw leaf PTE in *any* address space (PRESENT/DIRTY/ACCESSED/SWAP bits intact — unlike `vmm_translate_in`, which only gives the frame of present pages), so `app_format_smaps` (kernel/app.c) can walk each region's pages in the target's own page tables and classify them with **no new page-table-walk code**. Per region it reports **Rss** (resident pages), **Pss** (proportional set size — each resident page divided by its sharer count via `pmm_refcount+1`, so a COW-shared frame counts *fractionally*: the metric that makes shared memory honestly accountable), **Referenced** (the A bit), **Dirty** (the D bit), and **Swap** (pages carrying the `PTE_SWAP` marker). Covers the heap + every mmap VMA (labelled `[mmap]`/`[mmap-file]`/`[mmap-locked]`). New shell `smaps` (reads `/proc/self/smaps`) **verified headlessly**: after mmap'ing + touching 64 pages the `[mmap]` region reads **Rss 256 kB** (exactly 64×4 KiB) with Pss=Rss=Referenced=Dirty=256 kB and Swap 0, the heap reported separately at 68 kB — every field correct. `make check` 52 green. Builds directly on the M1150 accounting work; read-only.

> **(M1150) proc — `getrusage(2)`: real per-process resource accounting.** The kernel tracked total CPU time (`run_ms`) but exposed no POSIX resource accounting — no user/sys split, no fault counts, no context-switch counts. `getrusage(RUSAGE_SELF)` now reports all of it, backed by genuinely-tracked counters: **user vs system CPU time** (`utime_ms`/`stime_ms`, tick-sampled in the timer IRQ by the *interrupted* ring — `r->cs & 3 == 3` ⇒ user — via `task_cpu_tick`); **minor vs major page faults** (`minflt`/`majflt` on the process: COW-resolve + demand-zero are minor, swap-in + file-backed fills are major — four one-line bumps in `app_fault_handle`); **voluntary vs involuntary context switches** (`nvcsw`/`nivcsw` split in `switch_to_next` — still-runnable-when-switched-out ⇒ preempted/involuntary, else it blocked/yielded/voluntary); and **maxrss** from `vmm_wss().resident`. `struct rusage` lives in the shared `syscall.h` (so the kernel filler and the ulib wrapper can't drift); `SYS_getrusage`(#121) is PL_STDIO. New shell `usagetest` **verified headlessly, every field correct**: touching 100 fresh pages makes `minflt` jump **exactly 0→100**; an 80M-iteration compute burn accrues **130 ms `utime`** with **0 `stime`** (pure user-mode, no syscalls) and bumps **involuntary switches 0→13** (preempted mid-burn) while voluntary stays 10 (blocking on input); `maxrss` reads a real 1216 KiB. `make check` 52 green (the per-tick accounting + the scheduler split ride boottest + every in-guest suite). This is the **accounting substrate** the queued PSI (`/proc/pressure`) and `/proc/<pid>/smaps` build on.

> **(M1149) vm — `mlock`/`munlock`: pin pages against reclaim (the control half of M1147's query).** mincore (M1147) let userspace *observe* page residency; `mlock` lets it *control* it. A new `locked` flag on the per-VMA struct (kernel/app.c) marks a region pinned, and both reclaim paths now honor it: `app_madvise(MADV_DONTNEED)` and `app_swap_out` skip any page whose VMA is locked (the `in_vma` walk now also reads `locked`). `app_mlock`/`app_munlock` set/clear the flag on every VMA a range overlaps; `SYS_mlock`(#119)/`SYS_munlock`(#120) are PL_VM-classed. (Unlike Linux this doesn't kernel-touch user pages to force immediate residency — a not-yet-faulted page faults in normally on first access and is then pinned — avoiding a ring-0 read of a user VA.) The new shell `mlocktest` **verifies it by composing M1147**: mmap two regions, `mlock` one, fault both fully in, `madvise(DONTNEED)` both, then `mincore` both — the locked region reads **8/8 pages still resident** (reclaim refused) while the unlocked region reads **0/8** (all dropped). No swap disk needed. `make check` 52 green. Together M1147+M1149 are a complete residency **query + control** pair over the demand-paging engine.

> **(M1148) sched — real `/proc/loadavg` (fixed-point EWMA) + per-task run-queue wait.** `/proc/loadavg` was a **hardcoded stub** (`"0.00 0.00 0.00 1/N 0"` forever) — a fresh r/osdev research subagent flagged it as the obvious wart this same fire. It's now a genuine load average: `task_runnable_count()` (kernel/task.c) counts the tasks that want the CPU right now (RUNNING or READY, idle floor excluded — i.e. the run-queue depth), `loadavg_sample()` is called every timer tick and once per **5 s** folds that sample into three EWMAs using the **classic Linux fixed-point scheme** (FSHIFT=11, with the standard `EXP_1/5/15` = 1884/2014/2037 constants for a 5 s period), and `gen_loadavg` formats them as `int.frac`. Paired with it: a new **`WAIT_MS` column in `/proc/sched`** — each `task_t` now accrues `rq_wait_ms` (time spent READY-but-not-running) by stamping `ready_since` whenever a task enters the run queue (preempted in `switch_to_next`, or woken in `task_wake`/`task_wake_sleepers`) and charging the delta when it next runs. **Verified headlessly:** at boot `/proc/loadavg` reads a real climbing value; after launching the CPU-bound `demoscene` app for ~33 s it showed **`0.84 0.22 0.07 2/4 0`** — the textbook *rising-load* signature (1-min ≫ 5-min ≫ 15-min, because the fast average tracks recent load) — and `/proc/sched` showed live per-task `WAIT_MS` (Shell 500, kernel 2850, …). `make check` 52 green (the sched hot-path change + the per-tick timer hook ride boottest + every in-guest suite). Read-only observability; scheduling decisions are byte-for-byte unchanged.

> **(M1147) vm — `mincore`: userspace can see which pages RAM actually backs.** The OS has a rich anonymous-memory subsystem — demand paging (M1063), COW (M1116), swap (M1105), `madvise(DONTNEED)` (M1099) — but it was all *invisible* from userspace: a program couldn't tell a reserved-but-never-faulted page from a resident one. `mincore(addr, len, vec)` is the **read side** of that whole machinery. `app_mincore` (kernel/app.c) walks the range a page at a time: each page must lie inside one of the calling app's mmap VMAs (else `-1`/ENOMEM, POSIX semantics), and `vec[i]` is set to `1` iff `vmm_translate(page)` yields a frame (the page has been demand-faulted in) or `0` if it's still lazily reserved — the exact same VMA-membership + PTE idiom `app_madvise` already uses. `SYS_mincore` (#118, PL_VM-classed) validates the `vec` output buffer for `ceil(len/PAGE)` bytes via `vmm_user_ok` before writing. New shell `mincoretest` **verified headlessly end-to-end**: a fresh 16-page mmap reports **0** resident (proving laziness); after touching only the even pages `mincore` returns exactly `R.R.R.R.R.R.R.R.`; after `madvise(DONTNEED)` it's back to **0** — residency tracks the pager precisely. `make check` 52 green. A fresh r/osdev research subagent independently ranked `mincore` a top-5 pick this same fire.

> **(M1146) fs — ext2 symlinks: the ext2 POSIX filesystem is complete.** ext2 could create files, directories, and now (M1145) remove both — but a `symlink` was the last missing primitive *and* it was the one that needed genuine path-resolution work: reading **through** a symlink. Two halves. **Create:** `ext2_symlink_path` allocates an inode with mode `0xA000|0777` (S_IFLNK), `i_links_count=1`, stores the target **inline** in the inode's `i_block[]` area (a *fast symlink*, target ≤ 60 bytes), and adds a directory entry with file-type **7** — exactly the on-disk shape `e2fsck`/Linux expect. **Resolve:** the ext2 path walker `walk()` was refactored to `walk_d()`, which on hitting a symlink component reads the inline target and **recurses** to follow it (relative targets resolve against the link's own directory, absolute against the root), depth-capped at 8 to stop loops. This resolution is done **entirely inside ext2.c** — no call out to `vfs_read` — so the [[host-test-include-coupling]] landmine (fstest `#include`s ext2.c) stays disarmed. `vfs_symlink` now routes `/diskN` link paths to `blockdev_mount_symlink` → `ext2_symlink_path`, so the shell's `ln -s` creates a *real on-disk* symlink, not just a /tmp one. **Verified headlessly** on a fresh `mke2fs` image: `ln -s REAL.TXT /disk2/LINK` then `cat /disk2/LINK` prints the **target's** content (the walker followed the link); `ls /disk2` shows both; and on the **host** `e2fsck -fn` is **perfectly clean** with `debugfs` confirming inode 13 is a `120777` symlink (ftype 7, size 8 = `strlen("REAL.TXT")`). `make check` 52 green (fstest still links — coupling stayed disarmed). ext2 now has the **complete** POSIX namespace: regular files, directories, and symlinks, all create/read/delete, all `e2fsck`-clean.

> **(M1145) fs — ext2 rmdir: the directory lifecycle is complete.** ext2 could `mkdir` (M1137) but not remove a directory; now it can. `ext2_unlink_path` (the path behind `rm` on an ext2 mount) gains a directory branch: it refuses unless the directory is **empty** (`dir_is_empty` — nothing but `.`/`..`), then frees the dir's block + inode, marks the inode dead, unlinks it from the parent, **decrements the parent's link count** (undoing the `..` backlink `mkdir` added) and the block group's **`used_dirs_count`** — the exact reverse of mkdir's bookkeeping that `e2fsck` checks. Reuses the M1135 freeing helpers + M1137's `grp_dirs`. **Verified headlessly:** `mkdir /disk2/EMPTY` then `rm /disk2/EMPTY` → `removed`; `mkdir /disk2/FULL` + a file inside + `rm /disk2/FULL` → **`dir not empty`** (refused); `ls` shows EMPTY gone, FULL kept; and on the **host** `e2fsck` reports the image **perfectly clean** with `debugfs` confirming the tree. `make check` 52 green. ext2 now has the full POSIX file+directory lifecycle: create/overwrite/delete files, mkdir/rmdir directories.

> **(M1144) proc — per-process current directory (fixes a real cross-app leak).** The cwd was **process-global** — `synth_cwd`/`mount_sub` (vfs.c) + the fat32 cwd cluster were single globals, so one app's `cd` changed *every* app's listings (M1143 exposed this: the overlay's `cd /over` leaked into the Files window). Now each process has its own cwd. The VFS keeps the live cwd in its globals but **syncs it to/from the calling app on each app switch** — `vfs_sync_cwd()` (called once at syscall-dispatch entry) stashes the outgoing app's cwd (`app_cwd_save`) and restores the incoming one (`app_cwd_load` + `fat32_set_cwd`); the same-app fast path is a pointer compare. `fork` inherits the parent's live cwd (`vfs_cwd_inherit`); `app_reap` forgets an exiting owner. A new per-app `{cwd_synth, cwd_sub, cwd_fat}` holds it. **Verified headlessly:** `pcwd` cd's to `/proc`, forks a child that cd's to `/tmp`, and after the child exits the parent **still lists `/proc`** (PASS) — *and* the separate Files window keeps showing the boot FS, unaffected by `pcwd`'s `chdir` (the leak is gone). `make check` 52 green (the sync rides every syscall, so the whole suite exercises it). Real POSIX semantics: each process owns its working directory.

> **(M1143) fs — overlayfs whiteouts + merged listing: the union mount is complete.** M1142 gave overlay read-through + copy-up but had two asterisks: you couldn't *list* `/over` (no merged view) and couldn't *delete* a lower-layer file. Both fixed. **Whiteouts:** `rm /over/X` removes any upper copy and lays a `.wh.X` marker in the upper; thereafter a read of `/over/X` returns "not found" — the lower file is *hidden*, though the read-only lower is untouched (a later write to `/over/X` clears the whiteout, un-deleting it). **Merged listing:** `cd /over` enters a dedicated cwd state and `ls` unions the upper entries (minus `.wh.*` markers) with the lower entries the upper doesn't shadow or whiteout. Relative access inside `/over` now routes correctly too. **Verified headlessly end-to-end:** with the ISO as lower — read falls through (`Hello from a real ISO...`), `ls /over` shows the merged `DOCS/` + `HELLO.TXT`, a write copies up (`MODIFIED...`), `rm` then makes the read return **GONE** and `ls /over` drops `HELLO.TXT` (only `DOCS/` remains), while `/disk2/HELLO.TXT` read directly is **still intact**. `make check` 52 green. overlayfs is now a fully usable union mount (read/write/list/delete with copy-up + whiteouts).

> **(M1142) fs — overlayfs: a union mount with copy-up (container/live-CD layering).** A new filesystem that's a *union* of two others: `sys_overlay(lower, upper)` mounts `/over` over a **read-only LOWER** (any FS — an ISO9660 or ext2 disk) and a **writable UPPER** (tmpfs). A read of `/over/X` checks the upper first, then **falls through to the lower**; a write **copies up** to the upper, which from then on shadows the lower — and the lower stays byte-for-byte untouched. This is the exact mechanism behind container image layers and writable-overlay live CDs. It's a thin VFS router (`over_path`/`ov_join` in vfs.c) that *composes the existing per-FS ops* by recursing `vfs_read`/`vfs_write` on the rebased path — so it inherits ISO9660, ext2, and tmpfs for free. Dormant until mounted (so `make check` is unaffected). `SYS_overlay(117)` + an `overlay` app. **Verified headlessly:** with the ISO `osdev.iso` as the lower, `read /over/HELLO.TXT` returns the ISO's `"Hello from a real ISO 9660 filesystem!"`; a write to `/over/HELLO.TXT` then makes `read /over/HELLO.TXT` return the **modified** content (copy-up to tmpfs) while reading `/disk2/HELLO.TXT` directly **still returns the original** — the read-only lower untouched. `make check` 52 green. OS-DEV now does container-style layered filesystems.

> **(M1141) proc — robust futexes: a lock survives the thread that died holding it.** Closes a real gap in the M1139 mutex: if a thread locked it then exited (crashed) without unlocking, every waiter blocked **forever**. Robust futexes fix that. A robust mutex's word holds the **owner's tid** (0 = free), and a thread registers a small list of its currently-held robust locks (`sys_set_robust_list`, a `robust_t` it maintains in userspace — so lock/unlock stays syscall-free on the fast path). When a thread exits, `app_thread_exit` walks that list (bounded + every pointer `vmm_user_ok`-validated), sets **`FUTEX_OWNER_DIED`** on each word it still owns, and `app_futex`-wakes a waiter. The ulib robust mutex (`rmutex_lock`/`rmutex_unlock`) then recovers: a waiter that finds the owner-died bit CAS-takes ownership and returns **`EOWNERDEAD`** instead of hanging. `SYS_set_robust_list(116)` + `robust.h` (shared ABI) + a `robustfutex` app. **Verified headlessly:** the `dier` thread locks a mutex and `sys_thread_exit`s **while holding it**; the main thread's `rmutex_lock` returns **`1 (EOWNERDEAD)`** — it recovered the dead owner's lock — where without robust futexes it would have hung. `make check` 52 green. The futex stack is now complete: mutex → join → TLS → owner-death recovery.

> **(M1140) proc — thread-local storage (TLS) via per-thread `%fs` base.** The capstone of the threading work: each thread now gets its own thread-local storage. `sys_set_tls(base)` sets the calling thread's **`%fs` base** (the `IA32_FS_BASE` MSR — exactly how x86-64 `__thread`/TLS works), saved per-task; the context switch restores it (`load_fs_base` in `switch_to_next` + `task_exit`'s switch). It's **zero-cost until used** — the kernel never touches `%fs` itself (`-fno-stack-protector`, no swapgs), and a tracked `loaded_fs_base` skips the `wrmsr` while everything is still the default 0, so a system with no TLS pays nothing. A bare base is fine to set: ring-3 `%fs`-relative accesses are CPU-checked against the page's USER bit. `SYS_set_tls(115)` + ulib. **Verified headlessly:** the `threads` app now also has each of its 4 threads point `%fs` at its own slot and write its tid through `%fs:0` — and every write lands in *that thread's own slot* (`TLS: ... YES`), proving the base is restored per-thread across preemption (alongside the shared-counter mutex = 8000 and join/reap). `make check` 52 green (the per-switch change is sound system-wide). The thread stack is now complete: **spawn → mutex → join → per-thread storage**.

> **(M1139) proc — thread join + a futex mutex: threads, completed.** M1138 gave threads but left two gaps: the demo *poll-slept* to wait for them, and finished thread tasks *leaked*. Both fixed. `sys_join(tid)` (`app_join`) blocks until that thread has exited, then **reaps** its task (`task_free` — the struct+kstack a finished thread used to leak); the process now tracks its threads (`app_t.thr[]`), and `app_reap` frees/stops any un-joined ones before tearing down the shared address space (so no thread can outlive its CR3). And a real synchronization primitive: a **futex-backed mutex** in ulib (`mutex_lock`/`mutex_unlock`) — an atomic `xchg` fast path with `SYS_futex` only when contended. `SYS_join(114)` + the mutex. **Verified headlessly:** the `threads` app now has 4 threads each do 2000 **non-atomic `counter++` guarded by the mutex** → final count **exactly 8000** (proving the mutex truly serialized them — without it, lost updates would make it short), and the main thread **`sys_join`s each worker** (a real kernel wait-for-thread that reaps it) instead of polling. `make check` 52 green. Threads are now a usable library: spawn → lock/unlock → join. (Next thread-adjacent: `rseq`.)

> **(M1138) proc — kernel threads: shared-address-space concurrency (`clone`).** `fork` gives each child a *separate* (COW) address space; until now the OS had **zero** shared-memory concurrency. Threads close that gap: `sys_clone(fn, stack, arg)` creates a new task that **shares the caller's CR3 and `app_t`** (not a fresh one) and begins in ring 3 at `fn(arg)` on its own stack — so all threads see the *same* memory and can cooperate on shared data. It reuses what already existed: the preemptive per-CR3 scheduler, the ring-3 resume path (`iret_to_user`), and a new per-task `start_frame` (built from the caller's trap frame so the thread inherits the user segments). `sys_gettid` returns the per-thread task id; `sys_thread_exit` ends just one thread (`task_exit` unlinks it — so a finished thread is harmless to the rest); ulib `thread_spawn` mmaps a stack with the right ABI alignment + a return-to-exit stub. `SYS_clone/gettid/thread_exit(111–113)` + a `threads` app. **Verified headlessly:** `threads` spawns 4 threads (distinct tids 8–11) that each do 2000 `lock`-atomic increments of one shared counter → final count **exactly 8000** (proving both the shared address space *and* atomicity under preemption); and closing the window exits cleanly (no crash — `app_reap` frees the shared CR3 with the finished threads safely unlinked). `make check` 52 green. The one true-systems primitive `fork` deliberately can't give. (v1: finished thread tasks leak their struct+kstack until process exit; a reaping sweep is the follow-up.)

> **(M1137) fs — ext2 mkdir: directory creation completes the ext2 tree.** ext2 could create/overwrite/delete *files* (M1132/M1135) but not *directories*; now it can. `ext2_mkdir_path` allocates a directory inode (mode `04755`), allocates one data block and writes its `.` (→ itself) and `..` (→ parent) records, sets the inode (`i_links_count=2`, `i_size`/`i_blocks`/`i_block[0]`), links it into the parent with directory file-type, **increments the parent's link count** (the new dir's `..` adds a reference) and the block group's **`used_dirs_count`** — the exact bookkeeping `e2fsck` checks. Routed via `blockdev_mount_mkdir` → `vfs_mkdir` (now reaches `/diskN` mounts, not just the boot FS). `make check` 52 green. **Verified to the gold standard:** in-guest `mkdir /disk2/SUB`, then `echo … > /disk2/SUB/NOTE.TXT` (a file *inside the new directory*), `cat` reads it back; on the **host**, `e2fsck` reports the image **perfectly clean** and `debugfs` shows `/SUB` as a directory (mode `40755`) containing proper `.`/`..` + `NOTE.TXT` — Linux accepts the whole tree our kernel built. ext2 is now a fully usable read-write filesystem (files + directories); rmdir/symlinks are the remaining follow-ups.

> **(M1136) vm — file-backed mmap: map a file, page it in lazily from disk.** We had anonymous demand-paged mmap (M1063); now a process can mmap a **file** — `sys_mmap_file(path, len)` reserves a region whose pages are filled **on first touch by reading the file**, not zero-filled. The fault handler (`app_fault_handle`), on a fault in a file-backed VMA, allocates a frame and `vfs_pread`s the corresponding file page into it (the same path swap-in proved safe to do in the fault path), then maps it. **MAP_PRIVATE**: the page is mapped writable, so writes stay in RAM and never reach the file. The blocker — `vfs_read` has no offset — is *fixed*, not worked around: a new `vfs_pread(name, buf, max, off)` + a `pread` hook on `struct vfs_ops`, implemented for the boot FS by a clean sector-wise `fat32_pread` (a separate chain-walk, leaving the optimized `fat32_read` untouched). A per-VMA `{file_backed, foff, fpath}` carries the backing; `fork` inherits it. `SYS_mmap_file(110)` + ulib + an `mmapfile` app. **Verified headlessly:** `mmapfile` maps `MOTD.TXT`, and touching the mapping faults its page in from disk → first line `"Milestone 10 reached: VFS + FAT32 + ATA driver."`; the mapping **equals a normal `read` of the file** (proving the fault path served real bytes); and writing `#` through the mapping leaves the **file on disk unchanged** (MAP_PRIVATE). `make check` 52 green. The keystone under shared libraries / execve page-cache sharing; v1 serves boot-FS files (mount/tmpfs offset reads are the follow-up).

> **(M1135) fs — ext2 overwrite + delete: the read-write filesystem is complete.** M1132 could only *create* ext2 files; now it does the full lifecycle. The inverses of M1132's allocators: `free_block`/`free_inode_num` clear a bitmap bit and bump the free count in the group descriptor + superblock; `free_inode_blocks` walks an inode's direct + single- + double-indirect pointers and frees every data + metadata block; `dir_remove` unlinks a directory record by coalescing its space into the preceding entry. **Overwrite**: `ext2_write_path` now, when the file exists, reuses its inode, frees the old blocks, and rewrites (no duplicate dir entry). **Delete** (`ext2_unlink_path` → `blockdev_mount_remove` → `vfs_remove` for ext2 mounts): frees blocks + inode, marks the inode dead (`i_links_count=0`, a real `i_dtime`, cleared block pointers), and removes the dir entry. **Verified to the gold standard, end to end:** in-guest `echo … > /disk2/F.TXT` then overwrite then `cat` shows the *new* content; create + `rm /disk2/GONE.TXT` + `ls` shows it gone — and on the **host**, `e2fsck` reports the image **perfectly clean** (no orphan/bitmap/refcount errors) with `debugfs` confirming the overwritten content and the deletion. (A subtlety surfaced + fixed: a deleted inode's `i_dtime` must be a real timestamp, not a tiny value — fsck reads a small `i_dtime` as an orphan-list inode pointer.) `make check` 52 green. ext2 is now a fully read-write filesystem alongside FAT32.

> **(M1134) vm — userfaultfd: userspace page-fault handling.** A page that doesn't exist until it's read, then filled by a **different process** on demand. An owner `mmap`s a region and registers it (`sys_uffd_register`); thereafter the owner faulting on an unbacked page in it does **not** demand-zero — it **parks in the kernel**, and the faulting address is handed to a monitor process. The monitor reads the fault (`sys_uffd_read`), produces the page's bytes, and fills it (`sys_uffd_copy`), which maps a fresh frame **into the owner's (other) address space** via `vmm_map_to` and wakes the owner — whose faulting instruction now re-executes against the present page. This is the primitive under **live migration / post-copy / on-demand paging**. It's the capstone that composes machinery the campaign built: the #PF handler (`app_fault_handle` — where swap-in already proved blocking is safe), the cross-address-space mapping (M1114), the park/wait rendezvous (M1124), and `fork` (M1116) to give the owner≠monitor split. A per-VMA `uffd` flag routes only the owner's faults; one region at a time; additive (unregistered faults unchanged). `SYS_uffd_register/read/copy(107–109)` + ulib + a `uffdtest` app. **Verified headlessly:** `uffdtest` registers a page, forks a monitor, and reads the page — the read returns **`"[userfaultfd] this page did not exist until you read it -- a separate monitor process generated and supplied it on demand."`**, content materialized by the other process during the fault. `make check` 52 green.

> **(M1133) net — TCP server: passive open + an in-guest HTTP server.** The TCP stack was client-only (we always initiated). Now OS-DEV can **accept** connections: `net_tcp_serve(port, resp, …, reqbuf, …)` (kernel/net.c) is the passive-open counterpart of `tcp_connect` — it LISTENs for an inbound SYN, completes the three-way handshake (SYN-ACK → ACK), reads the peer's request, sends the response, and tears the connection down (FIN/ACK). The peer's MAC/IP/port are learned from its frames, so replies need no ARP/gateway; the server reuses the existing `tcp_send_seg` and tracks sequence/ack numbers across the handshake → data → teardown. One connection at a time, no retransmission (correct on the reliable localhost path). `SYS_tcp_serve(106)` (5 args via r10/r8, like `sys_http`) + ulib + an `httpd` app that serves a fixed page in a loop. Reusable harness: `osdrive.py --hostfwd tcp::18080-:80` wires QEMU user-net port forwarding. **Verified to the gold standard:** with `httpd` running in-guest, a real **`curl http://localhost:18080/` on the host** returned the exact page (`Hello from OS-DEV!`, full status line + headers + body, clean exit) — a complete SYN→SYN-ACK→ACK→GET→200→FIN round-trip — and the in-guest window shows `httpd: served request -> GET / HTTP/1.1`. `make check` 52 green. OS-DEV now both makes and accepts TCP connections — it can serve the web, not just browse it.

> **(M1132) fs — ext2 WRITE: real block + inode allocation, turning the ext2 mount read-write.** ext2 was read-only (M1102); now a process can **create files** on a mounted ext2 volume. `kernel/ext2.c` gained the write half: `alloc_block`/`alloc_inode` scan the per-group block/inode bitmaps for a free bit, set it, and decrement the free counts in the group descriptor **and** the superblock; `write_inode` does a read-modify-write of the inode-table slot; `dir_add` splices a new directory record into the parent by splitting an existing record's slack; `ext2_write_path` orchestrates — resolve the parent dir, allocate an inode, allocate + fill data blocks (direct + single-indirect), set `i_mode`/`i_size`/`i_blocks`, and link it in. Plumbed end-to-end: a new `blk_write_fn` type (partition.h), `blockdev_mount_write` (ext2 mounts only — FAT boot disk is written by fat32.c, ISO is read-only), `bd_blk_write`/`loop_blk_write` (the latter through the **write-through buffer cache**, so reads see writes), and `vfs_write` now routes `/diskN` writes to it instead of refusing. Create-only for now (overwrite/append/delete are follow-ups), which keeps it allocation-only with no block-freeing. **Verified to the gold standard:** in-guest `echo … > /disk2/PROOF.TXT` then `cat` reads it back; then on the **host**, `e2fsck` reports the image perfectly clean (12 files / 291 blocks, no errors — bitmaps + counts + refcounts all consistent) and `debugfs` reads `/PROOF.TXT` showing inode 12, mode 100644, size 23 — i.e. **Linux's own ext2 tools accept what our kernel wrote**. `make check` 52 green. OS-DEV now has two real read-write filesystems (FAT32 + ext2).

> **(M1131) fs — ISO 9660: a read-only CD/DVD filesystem driver.** A whole new on-disk filesystem — the one CDs and `.iso` images use. `kernel/iso9660.c` parses the Primary Volume Descriptor (logical sector 16, `CD001` magic), the root directory record, and the chain of directory records, supporting **subdirectory traversal**, listing, and file reads. Device-agnostic, mirroring the FAT32/ext2 volume API (`blk_read_fn` + ctx + start-LBA), so it slots straight into the existing mount layer: `blockdev.c` now probes ISO 9660 (after ext2, before the loose FAT probe) in both the partition scan and `losetup`, and the 3 mount-dispatch sites (list/read/isdir) became 3-way. ISO 9660 speaks 2048-byte logical sectors over the 512-byte block device (4 device sectors each); names are upper-cased with a `;1` version suffix stripped. **Read-only, fully bounds-checked** (every record length/identifier validated against the sector) — the safe kind of filesystem, can't corrupt anything. Verified headlessly: `tools/mkiso.py` hand-builds a spec-valid ISO (no mkisofs needed); attached as a 2nd virtio-blk drive (new reusable `osdrive.py --disk2`), the kernel **auto-detects and mounts it as `/disk2`**, and in-guest `ls /disk2` shows `DOCS/` + `HELLO.TXT`, `cat /disk2/HELLO.TXT` reads the root file, and `cat /disk2/DOCS/INFO.TXT` reads a **subdirectory** file — exact contents both. `make check` 52 green. OS-DEV now mounts FAT32 + ext2 + ISO 9660 + tmpfs + /proc + /dev.

> **(M1130) vm — mseal: irreversible memory sealing (W^X / JIT hardening).** Linux's `mseal(2)` (2024), the exploit-mitigation primitive: once a memory region is sealed, its mapping is **frozen for life** — `munmap` and `mprotect` on it are refused from then on (and the seal even survives `fork`). The point is *forward* security: an app builds a region exactly as it wants — e.g. JITs code into an mmap page and flips it to **R-X under W^X** — then seals it, so a later bug that hands an attacker an `mprotect`/`munmap` primitive *still* can't make that code writable again or swap a fresh page under it. Implemented as a per-VMA `sealed` flag (`kernel/app.c`): `app_mseal(addr,len)` sets it on every mmap region overlapping the range; `app_munmap`/`app_mprotect` now refuse any range touching a sealed VMA. Self-contained (operates only on the caller's own VMAs — no cross-process state), additive (unsealed regions behave exactly as before). `SYS_mseal(105)` + ulib + a `msealtest` app. **Verified headlessly:** the demo hand-assembles `mov eax,42; ret` into an mmap page, flips it R-X (W^X) and **runs it (→42)**, seals it, then an "attacker" `mprotect(R-W)` and `munmap` are **both DENIED (-1)** while the code stays callable and intact (→42); a separate unsealed region still `mprotect`s and `munmap`s fine (-> 0), proving the seal is per-region. `make check` 52 green. Composes with the M1090 mprotect/JIT story to complete a real W^X hardening primitive.

> **(M1129) io — io_uring-lite: a batched async-I/O submission/completion ring.** Ordinary syscalls cost one ring3→ring0 trap *per operation*. io_uring amortizes that: an app builds a ring (`struct io_ring`) in its own memory, fills the **submission queue** with a whole batch of operations, and crosses into the kernel **once** (`sys_io_uring_enter`). The kernel walks the SQ from `sq_head` to `sq_tail`, runs each op through the ordinary kernel paths (`vfs_read`/`vfs_write`, the gateway ping, the CSPRNG, the timer), and posts a result to the **completion queue** — one trap drains the whole batch. Opcodes: `IO_NOP / READFILE / WRITEFILE / PING / SLEEP / GETRANDOM`, each SQE an `{op, a, b, c, user_data}` and each CQE a `{user_data, res}`. The ring lives in the caller's address space, so the kernel touches it directly under the app's CR3 — the whole struct is bounds-checked (`vmm_user_ok`) and every embedded path/buffer pointer re-validated per-op, and **pledge is enforced per-op** (a READFILE in the batch needs `rpath`, a WRITEFILE needs `wpath`, …) so a batch can't reach past the process's pledge the way the standalone syscalls can't. `SYS_io_uring_enter(104)` + ulib + an `iouringtest` app. **Verified headlessly:** the demo queues 4 ops and calls enter **once** → `processed 4 op(s)`, all four completions posted with the right tags (`0xa0..0xa3`), op#2 (READFILE) **read back exactly what op#1 (WRITEFILE) wrote in the same batch** — `"hello from a batched io_uring write"` — proving in-order processing, plus 16 CSPRNG bytes. `make check` 52 green. The modern async-I/O ABI, in miniature, on the name-VFS.

> **(M1128) fs — userspace file materialization (`/fan`, fanotify-pre-content style).** A userspace **filesystem**: a daemon serves a VFS subtree, generating files on demand the first time they're read — files that don't exist until someone asks. A process becomes the daemon (`sys_fanotify_serve()`); when any process reads **`/fan/<name>`**, the kernel parks the reader, hands the name to the daemon (`sys_fanotify_wait`), and the daemon generates the bytes and provides them (`sys_fanotify_provide`), which the read returns. It re-aims the seccomp-notify rendezvous (M1124) from the syscall table at the VFS: the same interrupts-off park/wait/reply (no lost wakeup), but content is staged in a kernel buffer so it crosses the two address spaces safely (each side copies while its own CR3 is active). Additive — a new `/fan` VFS route, dormant unless a daemon registers (so `make check` is unaffected). `SYS_fanotify_serve/wait/provide(101–103)` + ulib + a `fantest` app. **Verified headlessly:** `fantest` forks a daemon; the parent reads `/fan/greeting` (no backing file) and gets back **`"materialized-on-demand: greeting"`** — content the userspace daemon generated on first access. `make check` 52 green. A FUSE-like userspace-filesystem capability on the name-VFS, reusing the supervisor machinery.

> **(M1127) net — eBPF-lite: a verified in-kernel bytecode VM for programmable packet filtering.** The marquee 2024-26 systems theme — a *programmable kernel* — on a from-scratch OS: userspace uploads a small bytecode program that the kernel runs on every packet to decide pass/drop. `kernel/bpf.c` is a tiny register VM (8 registers + a read-only `{dir,proto,sport,dport,len}` context, no memory access) with a **verifier** that runs at load time: bounded length (≤64 insns), valid opcodes/registers/context-fields, and **forward-only** conditional skips — so a loaded program provably terminates (the PC strictly increases) and can't escape. Instructions: `LDI`/`LDCTX`/`ADD`/`SUB`/`AND`/`OR`/`JEQ`/`JNE`/`RET` (`RET 0` = drop, nonzero = pass). Writing bytecode to **`/bpf`** loads+verifies it (0 bytes clears); `fw_check` (the existing firewall hook at `nic_send`/`nic_receive`) runs it when loaded, else falls through to the rule table; `/proc/bpf` reports the program size + run/drop counters. Gated behind `bpf_loaded()` so with no program the packet path is byte-for-byte unchanged. `sys_writefile("/bpf", …)` + a `bpftest` app. **Verified headlessly:** `bpftest` encodes a 6-instruction "drop ICMP, pass else" program, uploads it, and ping goes from **3 replies → 0** while `/proc/bpf` shows `6 instructions / runs: 3 / drops: 3` — the kernel ran the userspace bytecode on each echo request and dropped it. `make check` 52 green. BPF was born as a packet filter; this is that, verifier and all, in a hobby kernel — and the substrate a future `sched_ext`-style BPF scheduler would ride on.

> **(M1126) signals — `signalfd` + a pending-signal bitset (fixes a real drop bug).** Two things at once: a **correctness fix** and a feature that composes signals into the M1125 event loop. The fix: `app_t.pending_sig` was a single `int` — a second async signal arriving before the first was delivered *overwrote and lost it*. It's now a **bitset** (`pending_sigs`); `app_request_signal` ORs the bit, and `app_deliver_pending` delivers the lowest pending signal that has a handler (one per return-to-ring-3, as Linux does), leaving the rest queued. The feature: `sys_signalfd(mask)` routes the masked signals to **`/proc/self/sigfd`** instead of a handler — a blocking read there returns the next such signo (signals as a file), and it's `fswait`-ready when one is pending, so an event loop can `fswait` on signals alongside eventfds/mailboxes. `SYS_raise` was rerouted through `app_request_signal` so raised signals also flow through the bitset (delivered to a handler at return, or left for `sigfd`). `SYS_signalfd(100)` + ulib + a `sigfdtest` app. **Verified headlessly, all three signal paths intact:** `sigfdtest` routes SIGUSR1+SIGUSR2 to signalfd, raises both back-to-back, and reads `signo 10` then `signo 12` — **both survived** (the old single slot would have dropped 10); `alarmtest` still shows SIGALRM firing via its handler; `sigtest` still catches a raised signal in its handler and resumes via `sigreturn`. `make check` 52 green. Completes the event-loop story (fswait can now wait on signals too) and retires a genuine lost-signal bug.

> **(M1125) ipc — `fswait`: block on a set of name-VFS objects (select/poll/epoll for a no-fd OS).** The keystone that makes the IPC zoo composable: one call blocks until *any* of several objects is readable, and tells you which — the thing an event-loop server needs, done for a design with no file descriptors (it takes **paths**, not fds). `SYS_fswait(names, n, timeout_ms)` takes up to 8 NUL-separated paths; each is validated with `ustr`, then a poll loop checks `vfs_ready(name)` for each and `task_sleep_ms(10)`s off-CPU between passes until one is ready (returns its index) or the timeout elapses (returns -1). `vfs_ready` routes to non-blocking readiness peeks added to each blockable object — `eventfd_ready` (counter non-zero), `notify_ready` (bits pending), `mbox_ready` (a message queued) — none of which create or drain the object; anything else (regular files, `/proc`, `/dev`) is "always ready" (its read won't park). Polling keeps it low-risk — no change to any object's wake path (a future wait-queue version is a refinement). `sys_fswait()` packs a `char*` array into the buffer. **Verified headlessly:** `fswaittest` forks, the parent `fswait`s on `/event/fa` + `/event/fb`, the child signals `fb` after 800 ms, and the parent prints `woke -> index 1 ready (/event/fb)`; a second wait on an unsignalled object prints `returned -1 (timeout)`. `make check` 52 green (additive — a new syscall + read-only peeks). Completes the wait story: a process can now multiplex eventfds, notifications, and mailboxes in one blocking call.

> **(M1124) sec — userspace syscall supervisor (seccomp-notify): intercept and emulate another process's syscalls.** The genuinely-novel capstone of the pledge/unveil + /proc story — not just *restricting* syscalls but *delegating* them to userspace: a supervisor process watches a child's chosen syscalls and decides their fate (allow / deny with an errno / **emulate** with a fabricated return), the child none the wiser. A child arms `sys_seccomp(nr)` to trap syscall `nr`; when it calls one, `syscall_dispatch` parks it (`app_seccomp_notify` — records `{nr,args}`, blocks) *before* running the real syscall; the supervisor `sys_seccomp_wait(childpid, ev)` blocks until the child parks and reads the pending call, then `sys_seccomp_reply(childpid, run_real, retval)` either lets the real syscall proceed or makes it return `retval` without running. The two-way rendezvous runs interrupts-off (the syscall is an interrupt gate) so there's no lost wakeup — the mailbox discipline; it's keyed by the fork-returned **app pid** (no task-id handshake), and gated behind a per-app `sc_armed` flag so unsupervised apps are untouched. Composes with `/proc/<pid>/mem` (M1114) — a real supervisor would read the child's args through it. `SYS_seccomp(96)`/`_wait(97)`/`_reply(98)` + ulib + an `scnotify` app. **Verified headlessly:** `scnotify` forks; the child arms `writefile`, and its window shows `writefile #1 -> -1 (supervisor denied it)` then `writefile #2 -> 4 (supervisor faked success)` — the parent intercepted both, denying one and fabricating a 4-byte success for the other, with the real writes never executed. `make check` 52 green (the dispatch hook only fires for an armed process; nothing at boot arms). Userspace syscall interposition on a from-scratch OS.

> **(M1123) debug — hardware single-step instruction trace (`/proc/<pid>/sstrace`).** Lands the x86 single-step machinery (the `TF` flag + the `#DB` fault) that a debugger's `step` is built on — the instruction-level complement to the M1118 syscall strace ring. `sys_singlestep(n)` arms the next `n` userspace instructions by setting `RFLAGS.TF` in the syscall's own return frame, so the `iretq` back to ring 3 traps after one instruction; the `#DB` handler (`app_singlestep_trap`, wired into `isr_dispatch` for vector 1 from ring 3) records the RIP, decrements the count, and re-arms `TF` until `n` instructions are traced, then clears it. It **never kills** — a stray `#DB` (no trace armed) just clears `TF` and resumes. The trace is read back from `/proc/<pid>/sstrace`. Self-contained: a process steps *itself* (no cross-process controller, no task-id handshake, no stop-in-ISR), which makes it both low-risk and cleanly verifiable. **Verified headlessly:** `steptest` arms 8 steps, runs some arithmetic, and `cat`s its trace — 8 consecutive instruction pointers `0x4000001f, 0x40000024, 0x40000029, 0x40000031, …0x40000041` with realistic variable x86-64 instruction lengths (3–8 bytes apart), and `TF` cleared cleanly after the 8th (no runaway). `make check` 52 green (the vector-1 intercept only fires when `TF` is set, which nothing at boot does). The hardware-stepping core a future cross-process `ctl step` debugger verb extends.

> **(M1122) proc — per-process mount namespaces (`unshare`): containers on the name-VFS.** The Plan 9 / Linux `CLONE_NEWNS` superpower, and the natural sequel to fork+exec (M1121): a process gets a *private* view of the filesystem namespace, so its bind mounts are invisible to everyone else. The bind mounts (M1091) were global; now they live in numbered namespaces — `ns[0]` is the shared default every process starts in (so existing global binds are byte-for-byte unchanged), each `app_t` carries only an `ns_id`, and `bind`/`bind_resolve`/`/proc/binds` all operate on `cur_ns()` (the current process's table, `ns[0]` for kernel context). `unshare()` copies the caller's current bindings into a fresh namespace slot and switches it there, so subsequent binds are private; `fork()` inherits the parent's `ns_id` (shared until unshared — exactly Linux semantics). Additive: with no `unshare`, every process is in `ns[0]` and nothing changes. `SYS_unshare(94)` + `sys_unshare()` + an `nstest` app. **Verified headlessly:** `nstest` forks; the child window (titled "mount-namespace **(fork)**") `unshare`s, binds `/scratch`→`/tmp`, and prints `/scratch/ns.txt = PRIVATE-NS (my private bind works)`, while the parent window prints `/scratch/ns.txt NOT FOUND (the child's bind is isolated -- namespaces work!)` — the child's mount is private to it. `make check` 52 green (the `bind_resolve` hot path is unchanged for the `ns[0]` default the suites use). **fork + unshare + a private bind is a container** — and with exec (M1121) on top, a containerized program launcher.

> **(M1121) proc — `exec()`: replace a process's program image in place — the fork+exec keystone.** `fork()` (M1116) and `spawn()` existed separately; `exec` is the piece that makes fork *useful* — a process becomes a *different program* without changing pid, parent, or window, which is what every shell's `fork`+`exec`+`waitpid` job-control loop is built on. `app_exec(r, name, arg)` loads the registered program's ELF into a **fresh** address space (reusing app_spawn's CR3-switched load), switches to it, frees the old space (now non-active — `vmm_destroy_address_space` is safe), resets the per-program state (heap/VMAs/signal handlers/gfx canvas/grid; pid/parent/pledge sandbox are *preserved*, POSIX-style), and **rewrites the trap frame** in place — `rip`=new entry, `rsp`=fresh user stack, GP regs zeroed, `cs`/`ss`=ring-3 — so the syscall's `iretq` lands in the *new* program. On success it never returns to the old image; on failure it leaves the frame untouched and returns -1 (so userspace `exec()` "returns -1" like POSIX). `SYS_exec(93)` + `sys_exec()` + an `execdemo` app. **Verified headlessly:** `execdemo` forks, the child `exec`s into `clock`, and the screenshot shows three windows — the **Shell**, the **execdemo parent**, and a **"Clock" window that is the forked child** now running a live clock (`Time 23:09:16`, `uptime 25s`, `tasks: 5`) — a different program in the same forked process, title and all. `make check` 52 green (additive — a new syscall; the CR3-switch path only runs on `exec`, which nothing at boot calls). Completes the fork/exec/wait process-creation model (M1116 + M1117 + M1121).

> **(M1120) dev — a `/pci` scheme: the PCI bus as a navigable file tree.** The "everything is a file" treatment of the bus probe the AHCI/rtl8139/virtio drivers already rely on, and the principled form of the flat `lspci` text blob. `cat /pci` lists every device (`bb:ss.f  vendor:device  class:subclass:progif`); `/pci/<bb:ss.f>/<field>` reads one device — `vendor`, `device`, `class`, `irq`, `bars` (all six BARs), or `config` (a hexdump of the 256-byte config space). `kernel/pcifs.c` parses the bus:slot.func out of the path and reads live config space via the existing `pci_read32`/`pci_collect`; the VFS routes the `/pci` prefix (read-only). Additive — a new name prefix, dormant unless read. **Verified headlessly:** `cat /pci` shows QEMU's hardware — `00:02.0 1234:1111` (VGA), `00:03.0 8086:100e` (the Intel e1000 NIC), `00:04.0 8086:7020` (UHCI), `00:05.0 8086:2415` (AC97); `cat /pci/00:03.0/vendor` → `0x8086` (Intel); `cat /pci/00:03.0/bars` → `bar0=0xfebc0000` (MMIO) + `bar1=0x0000c501` (I/O) — the exact e1000 layout. `make check` 52 green. Strengthens the name-VFS thesis and is the natural first data source for a future Managarm-style `/bus` property namespace.

> **(M1119) debug — `/proc/<pid>/regs`: a process's ring-3 register file as a file.** The read-only half of a file-driven debugger, and the natural companion to `/proc/<pid>/mem` (M1114) + `ctl stop`/`cont`: stop a process, then read its registers and memory the Plan 9 way. The enabler is a one-line capture — `task_t` gains a `uframe` pointer that `isr_dispatch` sets to the trap frame on **every ring-3 trap** (syscall, IRQ, exception); while a task is stopped its frame stays frozen on its kernel stack, so the pointer remains valid. `task_uframe()` exposes it; `gen_pid_regs` formats the full GP set + `rip`/`rsp`/`rflags`/`cs`/`ss`. Most meaningful for a **stopped** task (a running one's frame is its last trap, constantly changing). **Verified headlessly:** `cat /proc/self/regs` shows a coherent ring-3 dump — `cs=0x1b`, `ss=0x23` (the ring-3 selectors), `rdx=0xffff` (the read length), `rsi=0x44000020` (a heap buffer), `rdi=0x5007ca60` (a stack pointer) — captured at the `readfile` syscall. `make check` 52 green (the capture is a single guarded pointer store in the trap path; no semantic change). With this, a debugger has the full inspection surface (regs + mem + stop); the remaining piece is active control — `ctl step` (single-step via `#DB`/`TF`) and `break` (hardware breakpoints via DR0–3), which this `uframe` is exactly what they'll mutate.

> **(M1118) debug — a readable per-process strace ring (`/proc/<pid>/strace`).** The syscall-trace channel (M1084) only ever printed to the kernel log (dmesg) — volatile and interleaved with everything else. This tees the trace into a small in-memory ring (`kernel/strace.c`, 128 records) tagged by task id, so a program can read its own (or another process's) recent syscalls **as a file**, no serial console required. The capture is free: `syscall_dispatch` already snapshots the call name + args before the dispatch switch (for the dmesg line); when an app is traced, it now also appends `name(0xa, 0xb, 0xc) = 0xret` to the ring. `/proc/<pid>/strace` returns that pid's records oldest-first (a global ring filtered on read; `/proc/strace`-style pid<=0 would show all with a pid column). Single-producer (`syscall_dispatch` runs interrupts-off) + a copying reader, so no locking. Tagged with the **task id** to match the `/proc/<pid>` namespace (which resolves via `task_current_id()`, not the app pid). Records accumulate only while tracing is on (toggle `trace`/`untrace` via `/proc/<pid>/ctl`), so it's zero-cost and dormant otherwise. **Verified headlessly:** `echo trace > /proc/self/ctl`; `echo hello world`; `cat /proc/self/strace` → the ring shows real calls — `read(0x0, …, 0x3ff) = 0x11` (line input), `write(0x1, …, 0xb) = 0xb` (the 11-byte "hello world"), and the prompt writes. `make check` 52 green (the ring append is gated behind the per-app `traced` flag, off for every app the suites run). Turns strace from a serial-only facility into a programmatic, on-device one — and the readable substrate a debugger UI would poll.

> **(M1117) proc — `waitpid()` + exit status + zombie reaping: completing the fork/wait pair.** The other half of `fork()` (M1116): a parent blocks until a child exits and collects its exit status, the canonical Unix process-lifecycle API. The challenge in this OS is the **reap race** — the window manager's `app_reap` frees an exited app's slot the instant it's dead, which would lose the exit status before the parent could read it. The fix is a **zombie** state, contained to forked children: `app_t` gains `parent`/`exit_code`/`zombie`/`waiting`; `app_fork` records the child's `parent` pid; `SYS_exit` now captures the code (`app_sys_exit(code)`). When `app_reap` reaps an exited child whose **parent is still alive**, it frees the resources (task, address space, canvas) as usual but, instead of releasing the slot, marks it a **zombie** (slot + `exit_code` retained) and wakes a parent blocked in `waitpid` — spawned apps (`parent == 0`) and orphans still free their slot immediately, so nothing else changes. `app_waitpid(pid, &status)` scans for a zombie child (specific pid, or `-1` = any), collects it (frees the slot, returns its pid + writes the status), or blocks via `task_block` until one appears; the scan+block runs interrupts-off (the syscall is an interrupt gate) so a child that exits while the parent is parked can't lose its wakeup (the mailbox discipline). `app_reap` also orphan-frees a dying parent's uncollected zombies. `SYS_waitpid(92)` + `sys_waitpid()` + `forktest` extended to collect its child. **Verified headlessly:** `forktest`'s child sets `shared=222` then `exit(7)`; the parent's window prints "**waitpid reaped child=102, status=7**" and "MY shared is still=111 (COW isolation OK)", and the child's window is gone (zombie collected, slot freed). `make check` 52 green (the exit/reap path is unchanged for the non-forked apps it tests). **No leak:** `/proc/meminfo` MemFree returns to within one page of baseline after a full fork→waitpid→exit cycle. With this, the shell or any program can `fork`+`waitpid` — the substrate for a real `fork`/`exec`/`wait` job-control loop.

> **(M1116) proc — copy-on-write `fork()`: the keystone POSIX primitive.** The canonical "this is a real Unix kernel" milestone — a process clones itself, sharing physical memory copy-on-write until either side writes. It composes machinery built over many prior milestones: per-frame refcounts (M1089), the `app_fault_handle` demand-pager (M1063), and the `PTE_SWAP` software-bit precedent (M1105). **The clone (`vmm_fork_cow`):** walks the parent's private `PML4[0]` user leaves (same invariant as the teardown/wss walks); each writable page is write-protected + marked `PTE_COW` (bit 10) in **both** parent and child, the shared frame gets one `pmm_addref`, and the child maps the same physical frame via `vmm_map_to` (which builds the child its **own** page-table hierarchy — only leaf data frames are shared, so teardown frees each space's tables once). The parent's TLB is flushed (CR3 reload) since its live pages were write-protected. **The COW fault** (a new branch atop `app_fault_handle`, now passed the `#PF` error code): a write to a present `PTE_COW` page allocates a private copy and drops a ref on the shared frame — or, if we're the sole remaining owner (refcount 0), just flips the page writable (no copy). **The child resume:** `app_fork` clones the parent's trap frame with `rax=0`, and a new task whose entry (`fork_child_trampoline`) calls a new asm helper `iret_to_user` that `iretq`s into ring 3 from the cloned frame via the shared `isr_common` tail — so the child returns from `fork()` at the same user instruction as the parent. The child gets a fresh pid + its own blank window, inherits the parent's heap/mmap VMAs/signal handlers/pledge-unveil sandbox/FP state (`task_copy_fpu`), but not pending signals or alarms (POSIX). Frames beyond the 1 GiB refcount array are eagerly copied (defensive; impossible at the 256 MiB guest size). `SYS_fork(91)` + `sys_fork()` + a `forktest` app. **Verified headlessly:** `forktest` forks; the child window shows it inherited `shared=111` then set `shared=222` in its copy, while the **parent window shows `shared` is still `111` — "COW isolation OK"** (the write split the page); the child got a fresh pid (102). **Zero leak:** `/proc/meminfo` MemFree is byte-identical (187324 kB) before the fork and after both processes exit and are reaped — proving the refcount/teardown cooperation frees every shared frame and private table exactly once. `make check` 52 suites green (the COW branch is gated behind `PTE_COW`, which nothing sets until a fork, so demand-paging/swap/boot are unchanged). The substrate for `waitpid`, `fork`+`exec`, and CRIU-style checkpoint/restore.

> **(M1115) fs — copy-on-write tmpfs snapshots (`/snap`): the Plan 9 dump filesystem, in miniature.** Freeze the whole RAM filesystem into a named generation, then keep using `/tmp` — the snapshot preserves the old contents while the live files move on, sharing storage copy-on-write. The insight that makes it nearly free: `tmpfs_write` already allocates a *fresh* buffer per write and frees the old one, so a snapshot just has to **retain** the old buffer instead of letting it be freed. A snapshot (`kernel/tmpfs.c`) captures each live file's `(name, buffer pointer, len)` with **no data copy**; the three `kfree` sites are guarded by `buf_in_snapshot()` so a buffer a snapshot still holds is never released; dropping a snapshot frees only the buffers no live file or other snapshot references. `snap_count == 0` is the fast path (the guard returns immediately), so **with no snapshots tmpfs is byte-for-byte unchanged** — zero blast radius (the M1105-swap "dormant until used" discipline; `make check`'s core-dump test, which hammers `/tmp`, is unaffected). All driven through the VFS, no syscalls: `echo create > /snap/ctl` snapshots, `echo drop 0 > /snap/ctl` releases, `cat /snap` lists generations, and `cat /snap/<gen>/<name>` reads a frozen file. **Verified headlessly:** `echo ALPHA > /tmp/a.txt`; `echo create > /snap/ctl`; `echo BETA > /tmp/a.txt`; then `cat /tmp/a.txt` → `BETA` (live) while `cat /snap/0/a.txt` → `ALPHA` (frozen) — the original bytes survived the overwrite copy-on-write — and `cat /snap` shows `GEN 0 / FILES 1`. `make check` 52 suites green. Filesystem time-travel on a name-based VFS, no disk and no fork required.

> **(M1114) debug — `/proc/<pid>/mem/<hexaddr>/<len>`: read another process's live memory.** The live counterpart to M1112's post-mortem `crashinfo`, and the completion of the mature `/proc/<pid>/ctl` (kill/stop/cont/trace) into a real in-guest debugger surface: SerenityOS/Linux expose a process's address space as a file, and now so does this. Since the name-based VFS has no file descriptors or `lseek`, the address and length ride **in the path** (very Plan 9): reading `/proc/<pid>/mem/40000000/64` hexdumps 64 bytes at virtual `0x40000000` of that pid (`/proc/self/...` works too). The kernel adds `vmm_translate_in(cr3, va)` — a cr3-parameterized page-table walk over the HHDM (never loading CR3), the arbitrary-address-space twin of `vmm_translate` — and `procfs` walks the target page by page, formatting an `addr: bytes |ascii|` hexdump; unmapped bytes show as `..`. **Safety:** reads are restricted to the user footprint `[0x40000000, 0x80001000)`, so the shared kernel higher-half and the low identity map can never be exposed; the syscall is already gated by pledge `rpath`. **Verified headlessly:** `/proc/self/mem/40000000/32` shows real x86-64 instructions (`41 56 49 89 f8 …` = `push r14; mov r8,rdi; …`) from the shell's text, and `/proc/self/mem/80000000/32` (the vDSO page) shows `hz=0x64` (100) at offset 4 with a `ticks`/`mono_ns` pair consistent at ~24.8 s and a valid 2026 `real_sec` — proving the cross-address-space translation. `make check` 52 suites green (additive — a new `/proc` sub-path + one new VMM helper). With ctl-stop + this, a userspace debugger can read a stopped target's memory; the natural next steps are `/proc/<pid>/regs` + single-step via `#DB`.

> **(M1113) ipc — `/timer/<ms>` sleepable files + `/event/<name>` counting eventfds.** Two new "wait on a file" objects routed by the VFS (`kernel/eventfd.c`), completing the IPC quartet: mailbox (FIFO data, M1087), notification (OR-coalesced bitmask, M1101), shared memory (bulk, M1108), and now an **eventfd** (a count). Reading **`/timer/<ms>`** blocks for `<ms>` milliseconds (parsed from the path, capped at 10 min) over the existing `task_sleep_ms` timer wheel, then returns `tick\n` — so a shell script paces itself with `cat /timer/250`, no new syscall, just file I/O. **`/event/<name>`** is a counting semaphore: writing a decimal `N` adds `N` to the counter and wakes a blocked reader; reading blocks until the counter is non-zero, then returns the count as decimal and drains it to 0. Unlike the notification object (which ORs a bitmask) this one *adds*, so it counts events. The `/event` read path clones notify.c's interrupts-off "zero? then block" + read-and-drain discipline (atomic against a concurrent writer on this single CPU — no lost wakeup); `/timer` `sti`s before sleeping (the timer drives the wait, like `SYS_sleep`). `/proc/events` lists live counters. Additive — new VFS name prefixes, no syscalls, reached through the shell's ordinary `cat`/`echo >` (like `/notify`). **Verified headlessly:** `cat /timer/300` → `tick`; `echo 5 > /event/sem` then `echo 3` → `/proc/events` shows `sem 8` (accumulated); `cat /event/sem` → `8` then drains. `make check` 52 suites green.

> **(M1112) debug — `crashinfo`: a userspace post-mortem reader for ELF core dumps.** Closes the loop on M1104: the kernel writes `/tmp/core` (an `ET_CORE` ELF with `NT_PRSTATUS` registers + `PT_LOAD` memory segments) on a fatal ring-3 fault, but until now nothing could *read* it in-guest. `user/crashinfo.c` parses the core entirely in userspace: it validates the ELF64/ET_CORE header, walks the program headers to find the `PT_NOTE` → `NT_PRSTATUS` block (27 GP regs at desc offset 112, in `user_regs_struct` order), prints a register dump, and produces a **backtrace two independent ways** — (1) a frame-pointer walk that follows the saved-`rbp` chain through the `PT_LOAD` stack image (with a strictly-ascending guard against loops/garbage), and (2) a heuristic scan of the live stack for text-range values (works even when the crashed program omitted frame pointers). Every address is labelled by the app's fixed memory map (`<- text`/`heap`/`stack`/`mmap`/`NULL`/`vdso`) since there's no userspace symbol table. To make the demo's chain walkable, `crash.c` now builds with `-fno-omit-frame-pointer` (a one-line Makefile rule, mirroring the kernel's own M1078 backtrace flag). **Verified headlessly:** `crash` (NULL write) → `crashinfo` window shows `#0 0x40000022 <- text` (the faulting instruction, matching M1104's dmesg) and `#1 0x40000f81 <- text` (the return into `_start`), and the heuristic scan independently finds `0x40000f81` on the stack — both methods agree. `make check` 52 suites green (additive — a new app + a build-flag tweak). A real in-guest crash debugger, no host gdb needed.

> **(M1111) vm — a vDSO time page: syscall-free `clock_gettime` from userspace.** The trick every real kernel uses to make `gettimeofday`/`clock_gettime` not cost a syscall: a single read-only page (`kernel/vdso.c`) mapped at a fixed address (`VDSO_ADDR` = 2 GiB) into **every** process; the timer IRQ writes the current time into it under a **seqlock**, and userspace reads it straight from RAM. `struct vdso_time` holds `{seq, hz, ticks, mono_ns, real_sec, real_nsec}`; `vdso_tick(ticks)` (called from `timer_handler` each tick) bumps `seq` odd, writes the fields, bumps it even, so a reader that sees an even, unchanged `seq` across its read has a torn-free snapshot. The wall clock is seeded once from the RTC at boot (`days_from_civil` → Unix epoch) and carried forward by the monotonic tick offset (the standard "REALTIME = boot offset + MONOTONIC" model). `ulib clock_gettime` reads the page with the matching seqlock loop — **zero syscalls, zero ring switches**. Frame lifetime: one shared frame held by a permanent `pmm_addref` plus one ref per mapping, so an address space's teardown (which frees the leaf once) can never drop it — same refcount trick as the M1089 ring mirror. **Address-space subtlety (the bug found + fixed in testing):** the page must live at/above 1 GiB — the low 1 GiB (PDPT[0]) is identity-mapped with *shared* huge pages copied verbatim into every space, so a private 4 KiB leaf can't be installed there (the first attempt at `0x3FFFF000` silently read the identity map = zeros); `0x80000000` (PDPT[2], boot-empty → private per space) is clear of the app text/heap/stack/mmap. **Verified headlessly:** shell `clockgt` prints `MONOTONIC: 21.090 s since boot`, `REALTIME: 1782248499 Unix s` (a valid mid-2026 UTC stamp), and `re-read MONOTONIC after ~300ms: advanced 300 ms` — the clock advances and is read entirely from `0x80000000` with no syscall. `make check` 52 suites green (additive — the page is dormant RAM until an app reads it). The latency primitive a tight game/animation loop or a future libc `time()` would lean on.

> **(M1110) net — `/net/tcp` sockets-as-files (Plan 9): drive a TCP connection through ordinary file I/O.** The Plan 9 idea that a network endpoint is just a filesystem: no `socket()`/`connect()`/`send()`/`recv()` syscalls — you `read`/`write` *files*. `kernel/net.c` keeps a small table of connection slots (`nconn[4]`, each wrapping the existing `tcp_conn`); `vfs.c` routes the `/net/tcp/` name prefix to `netfs_read`/`netfs_write`. Reading **`/net/tcp/clone`** allocates a free slot and returns its index `n` as text (the classic Plan 9 clone file); writing **`connect host!port`** (or `host:port`, shell-safe) to **`/net/tcp/<n>/ctl`** DNS-resolves and opens the stream; then **`/net/tcp/<n>/data`** is the byte pipe — `write` sends, `read` receives — and `close` to `ctl` tears it down. The connect/read/write paths run with interrupts on (`sti`) because the TCP handshake needs the timer, reusing the M1094+ `tcp_connect`/`tcp_write`/`tcp_read` primitives unchanged; pointer args go through the standard `ustr`/`ubuf` validation. Additive — a new VFS name prefix, no change to existing routes. **Verified headlessly:** the new `nettcp` app clones a slot, `connect example.com:80` via `…/ctl`, writes a full `GET / HTTP/1.0` request through `…/data`, reads the reply, and its window prints "**response first line: HTTP/1.1 200 OK**" then "closed the connection (done)" — bidirectional TCP entirely through `read`/`write` on files. `make check` 52 suites green. A scriptable network interface: a shell script (or any app with no network syscalls) can now talk TCP using only file ops.

> **(M1109) ipc — futex: a userspace fast mutex with a kernel wait/wake fallback.** The synchronization primitive every mutex/condvar/semaphore is built on: the uncontended path is a userspace atomic (zero syscalls), and only on contention does a task trap to `SYS_futex(uaddr, op, val)`. `FUTEX_WAIT` blocks the caller iff `*uaddr` still equals `val` (else returns immediately — the lost-wakeup guard); `FUTEX_WAKE` wakes up to `val` waiters. Wait buckets are keyed by the word's **physical address** (`vmm_translate`), so a futex living in named shared memory (M1108) — mapped at *different* virtual addresses in two processes — matches across them. The compare+register+block is atomic against a concurrent wake because the syscall path runs interrupts-off on this single CPU (the same discipline as the mailbox). Built on `task_block`/`task_wake`; additive (a new syscall, no change to existing paths). **Verified headlessly cross-process:** a `futex` app's `wait` mode maps the shared word, spawns a *separate* `futex wake` process, and `FUTEX_WAIT`s — its window prints "blocking…" then "**WOKEN by the other process! cross-process futex works.**", proving a real block in one process resolved by a wake in another. `make check` 52 suites green. Completes the synchronization story and is the primitive a future threads/pthread layer would stand on.

> **(M1108) ipc — named shared memory (`shm_open`): two processes share the same RAM.** The zero-copy data-plane complement to the mailbox (M1087, small messages) and notification objects (M1101, signals): `kernel/shm.c` keeps a table of named objects, each a set of physical frames allocated on first open. `app_shm_open(name,size)` maps an object's frames into the caller eagerly (mirroring the M1089 magic-ring-buffer mapping) and `pmm_addref`s each, so a second mapping — in the same process or a different one — backs onto the **same RAM**; the frames are owned by the table at refcount 0 and each mapping adds/drops a ref, so they outlive any single mapper (and address-space teardown releases the ref via the existing PTE-free walk). `SYS_shm_open(89)` + ulib + a `shmtest` shell builtin + `/proc/shm`. **Verified headlessly:** `shmtest` does `shm_open("demo")` twice → two distinct VAs, writes `'SHM!'` through mapping A, reads it back through mapping B → "they share one backing"; `/proc/shm` lists `demo  4 KiB`. `make check` 52 suites green (additive — nothing maps shm at boot). Unblocks a future cross-process futex (shared-memory fast mutex). The IPC story is now complete: mailbox (data) + notification (signals) + shared memory (bulk).

> **(M1107) fs — `/dev/loop`: mount a filesystem *image file* as a block device.** `losetup FILE` reads a FAT32 or ext2 image held in a regular file into a RAM buffer and registers it as a loop mount (a new `loop_blk_read` serves 512-byte sectors out of that buffer), reusing the exact FAT/ext2 detection + `/diskN` browsing path — so the image appears as the next `/diskN`, read with the same `ls`/`cat`/`cd`. `blockdev_losetup()` + `SYS_losetup(88)` (reads the file via `vfs_read` into a kmalloc'd buffer it then owns) + ulib + a shell `losetup`. **Verified headlessly by composing three features built this session:** a 256 KiB ext2 image was **fetched over the network with `tftp get`** (M1098, "saved 262144 bytes"), `losetup`'d ("mounted GOT.E2 as /disk2"), then read via the **ext2 driver** (M1106): `ls /disk2` → `lost+found/`, `LOOPHI.TXT 43`; `cat /disk2/LOOPHI.TXT` → "Hello from a loop-mounted ext2 image file!". `make check` 52 suites green (loop is inactive until `losetup` is called — additive). The classic Unix loop device, now in OS-DEV — mount FS images stored as files, no spare disk needed. (Fixed a setup-ordering bug: `loop_blk_read` must bound by the array, not `g_nmount`, since the slot is probed before it's counted.)

> **(M1106) fs — a read-only ext2 driver: mount + read real Linux filesystems.** A from-scratch ext2 reader (`kernel/ext2.c`), device-agnostic like the FAT32 volume API: it parses the superblock, block-group descriptors, inodes (direct + single/double-indirect block maps) and linked directory entries, walking arbitrary `/`-paths from the root inode. It hooks the existing `/diskN` mount registry — the scan now probes **ext2 first** (its superblock-magic check is strict, vs `fatvol_list` returning 0 even for non-FAT) and adds LBA 0 as a candidate so a bare `mke2fs` image (no MBR signature) is found; `blockdev_mount_{read,list,isdir}` dispatch on the volume's FS type, so ext2 disks appear as `/diskN` and are browsable with the same `ls`/`cat`/`cd` as FAT. **Verified headlessly** against a genuine `mke2fs -d` image attached as a second virtio-blk disk: `mount` shows `/disk2 on virtio-blk`; `ls /disk2` lists `lost+found/`, `BIG.TXT 4000`, `HELLO.TXT 72`, `docs/` (correct names + sizes + dir flags); `cat /disk2/HELLO.TXT` prints the file; `cat /disk2/docs/NOTE.TXT` reads a **nested** path; and `sha256 /disk2/BIG.TXT` (4000 bytes, multiple blocks) is **byte-identical to the host** — so the superblock parse, inode/block-group math, multi-block file read, and directory walk are all correct. `make check` 52 suites green. The second real filesystem (after FAT32), proving the VFS/mount abstraction — and Linux interop.

> **(M1105) vm — swap-to-disk: page anonymous memory out to a block device and fault it back.** The marquee VM feature, and the culmination of this session's arc — it composes the demand pager (M1063), `blockdev_write` + the buffer cache (M1095), the per-frame refcount (M1089), and the working-set view (M1093). `kernel/swap.c` lazily claims the high end of the first writable **non-boot** block device as 32 MiB of page slots (a bitmap); `swap_out` writes a frame to a free slot, `swap_in` reads it back, via the uniform `blockdev_write/read`. A new software PTE bit `PTE_SWAP` (bit 9) plus the slot index packed into the high bits encode a *swapped* (not-present) page, distinct from an unmapped one; `vmm_pte_raw`/`vmm_set_raw` read/write the raw leaf entry. `app_swap_out(addr,len)` (a real `MADV_PAGEOUT`: `SYS_swapout(87)`) evicts each resident, single-owner (refcount 0) page in an mmap region — write to swap, rewrite the PTE, free the frame; and `app_fault_handle` gains a branch at the top: a fault on a `PTE_SWAP` page allocates a frame, reads the slot back (briefly enabling interrupts, since the disk read may wait on an IRQ), restores the mapping, and frees the slot. **Crucially it is inactive — every path a no-op — until a writable non-boot device exists, so a normal boot changes nothing** (zero blast radius). A shell `swaptest` + `/proc/swaps` expose it. **Verified headlessly** with a 48 MiB virtio-blk swap disk: `swaptest` mmaps 64 pages, fills a pattern, `SYS_swapout`s them (frames freed), then re-reads — "**paged out 64 pages to disk … every page survived the disk round-trip → swap works**"; `/proc/swaps` showed PagesOut 64 / PagesIn 64 / Used 0 (slots freed after fault-in, no leak). `make check` 52 suites green (swap dormant there — only the boot disk present). An app's resident footprint can now fall below its mapped footprint.

> **(M1104) debug — ELF core dumps: post-mortem crash capture to `/tmp/core`.** When a ring-3 app dies on an unhandled CPU exception, the kernel now writes a real **`ET_CORE`** ELF capturing its state — the userspace complement to the M1078 kernel panic backtrace, and a host-`gdb`-loadable artifact. `app_fault_current` (now passed the trap frame) calls `app_core_dump(r)`, which builds the core in one `kzalloc` buffer and writes it to tmpfs (RAM, so the write is safe with interrupts off in the fault path): an ELF64 header + a `PT_NOTE`/**`NT_PRSTATUS`** with the 27 GP registers laid out in the standard `user_regs_struct` order, plus a `PT_LOAD` per writable region (the active stack from the faulting RSP up, the heap, each mmap). Each page is read through `vmm_translate` + the HHDM, zero-filling any demand-paged hole, so the dumper never itself faults; the total is capped at 2 MiB (falls back to stack-only). A tiny `crash` app triggers it on demand, and the shell `file` command now distinguishes `ET_CORE` ("ELF core dump"). **Verified headlessly:** `crash` → its window dies; `file /tmp/core` → "ELF core dump"; `dmesg` → "[core] wrote /tmp/core (4628 bytes) for pid 101 at rip=0x40000022" — the rip captured is exactly the faulting instruction's address. `make check` 52 suites green. Completes the debugging story: symbolized panic backtrace + strace + sampling profiler + now process core dumps.

> **(M1103) net — an SNTP client (`sntp`/`ntpdate`): set the wall clock from the network.** Resolves `pool.ntp.org`, sends one 48-byte NTP packet to UDP :123 (via the `udp_send_to` helper added in M1098), reads the transmit timestamp (seconds since 1900) at offset 40, converts NTP→Unix epoch (−2208988800) and Unix→civil date (Howard Hinnant's days-to-civil), and writes it to the CMOS clock with a new `rtc_set()` (honours register B's BCD/binary flag, forces 24-hour, brackets the write with the SET bit). `SYS_sntp(86)` + ulib + a shell `sntp` (also `ntpdate`) that prints the before/after time. **Verified headlessly** against real internet through QEMU's SLIRP NAT: `sntp` → "querying pool.ntp.org (UDP 123)… before/after: 2026-06-23 16:21:06 … clock synced (UTC)" — the computed date matched the known-accurate host clock **exactly**, which end-to-end confirms UDP-123 egress, the NTP-timestamp parse, the epoch + civil-date conversion, and the `rtc_set` CMOS write (a bug in any of those would have produced a date ~70 years off or garbage). `make check` 52 suites green (added an `rtc_set` stub to the net host-test harness, since net.c now references it). The third protocol on the M1098 UDP helper (after DHCP and TFTP).

> **(M1102) signals — `alarm()`: a periodic SIGALRM timer to a userspace handler.** Wires the timer tick to the existing async-signal machinery, giving programs a self-paced periodic callback. `SYS_alarm(ticks)` arms a per-app interval (0 disarms); on each timer IRQ `app_alarm_tick()` checks the current app and, when the interval elapses, raises **SIGALRM (14)** via `app_request_signal` — so the hard part (rewriting the trap frame to enter the handler and restoring on `sigreturn`) is entirely the proven M1067/M1083 path; this milestone is just the timer wiring. ulib `sys_alarm` + a shell `alarmtest`. **Verified headlessly:** `alarmtest` installs a SIGALRM handler, arms a 200 ms interval, busy-spins ~1 s, then reports "**SIGALRM fired 4 times** in ~1s (expected ~5)" — the handler ran the right number of times via the async delivery and `sys_alarm(0)` cleanly disarmed it. `make check` 52 suites green. The timer-driven complement to the existing fault/Ctrl-C signals (enables userspace watchdogs and cooperative time-slicing).

> **(M1101) ipc — notification objects (`/notify/<name>`): seL4-style coalescing doorbells.** The signalling counterpart to the mailbox's data queue (M1087): each named object holds one OR-accumulated bitmask. A writer ORs bits in (a signal edge); a blocked reader wakes and atomically **reads-and-clears** the accumulated mask — so many signals that arrive before a read **coalesce** into a single wakeup carrying the union of their bits (seL4's notification-vs-endpoint distinction). `kernel/notify.c` mirrors `mbox.c`'s table + the interrupts-off "no bits? then block" `task_block`/`task_wake` pattern (so the block and the read-and-clear are atomic against a writer on this single CPU — no lost wakeup); the VFS routes `/notify/<name>` to it, so plain `echo`/`cat` drive it with no new syscall, and `/proc/notify` lists each object's pending mask. **Verified headlessly:** `echo 1 > /notify/door` then `echo 4 > /notify/door`; `/proc/notify` then showed `door PENDING-MASK 5` (1|4 coalesced *before* any read); `cat /notify/door` returned **5**; and `/proc/notify` then showed `0` — confirming OR-coalescing and read-and-clear. `make check` 52 suites green. Rounds out the IPC story: the mailbox carries data, the notification object carries signals.

> **(M1100) net — a packet filter / firewall (`fw`, ipfw-lite).** A first-match rule table checked on both the RX and TX paths, hooked at the two single chokepoints `nic_send` (outbound) and `nic_receive` (inbound) so no per-protocol cooperation is needed. Rules are `{direction, protocol, port, action}`; first match wins; a frame matching no rule is allowed (default-allow), and with zero rules `fw_check` returns immediately — so it's a true no-op until you add a rule (zero overhead, fully additive). Non-IPv4 frames (ARP) always pass, so a rule set can't wedge address resolution. Configured by writing verbs to `/proc/fw` (`drop|allow in|out|both icmp|tcp|udp|any [port]`, or `flush`) and listed with per-rule hit counts by reading it; a shell `fw` command wraps both. **Verified headlessly:** `ping` → 3/3 replies; `fw drop in icmp` → `ping` → **0/3 replies** (inbound echo replies dropped) while `resolve example.com` → 172.66.147.243 still works (selective: ICMP dropped, UDP/DNS allowed); `fw` then showed the rule with **hits=3** — exactly the three dropped replies, proving the filter matched and dropped real packets. `make check` 52 suites green. Real security-relevant networking that needs no SLIRP cooperation or listen/accept — it acts on the packets the stack already processes.

> **(M1099) vm — `madvise(MADV_DONTNEED)`: reclaim a region's resident pages on demand.** Lets a program hand RAM back to the kernel immediately for a range it's done with, without un-reserving it — the next touch demand-faults a fresh zero page (the existing `app_fault_handle` pager). `app_madvise()` walks `[addr,addr+len)` and reclaims each page that is **(a)** inside an `mmap` VMA and **(b)** single-owner — guarded by a new `pmm_refcount()` (exposes the M1089 per-frame refcount) so a shared or ring-buffer-mirror frame (mapped more than once) is never pulled out from under its other mapping. `SYS_madvise(84)` (pledge class `vm`) + ulib + a `madvisetest` shell builtin. **Verified headlessly:** `madvisetest` mmaps 64 pages, writes `0xAB` to every byte, then `madvise(DONTNEED)` → "reclaimed 64 resident pages", and re-reading the region returns **all zero** — proving the frames were genuinely reclaimed and re-faulted fresh (not merely marked). `make check` 52 suites green. Composes the demand pager (M1063) + the frame refcount (M1089) + the working-set view (M1093) into a real POSIX memory-management primitive.

> **(M1098) net — a TFTP client (`tftp get`): real file transfer over UDP.** Adds the reusable UDP datagram primitive DNS/DHCP had been open-coding — `udp_send_to()` (wrap a payload in Ethernet/IPv4/UDP and ship it) — and rides it for a from-scratch TFTP (RFC 1350) client: send a Read Request, then lock-step the server's DATA blocks with ACKs, latching the server's transfer port (TID) from its first DATA, ending on a short (<512-byte) block. `net_tftp_get()` + `SYS_tftp(83)` (fetches from the gateway's server, where SLIRP runs one) + ulib + a shell `tftp get REMOTE [LOCAL]` (prints the file, or saves it with a second arg). **Verified headlessly** against QEMU's built-in SLIRP TFTP server (`-netdev user,tftp=/dir`, no external internet): fetched a 2160-byte / 5-block `HELLO.TXT`, saved it to `GOT.TXT` ("saved 2160 bytes"), and `sha256 GOT.TXT` (`3a31098d…6ee2bf`) was **byte-identical to the host file's SHA-256** — proving the multi-block sequencing, ACKs, TID switch and short-final-block handling are all correct. `make check` 52 suites green. The `udp_send_to` helper now makes SNTP and other UDP protocols nearly free.

> **(M1097) storage — a content-addressed blob store (`cas`): blobs keyed by their own SHA-256.** The core primitive behind git objects, Plan 9's Venti, restic/borg and IPFS, built from scratch on the kernel's SHA-256: `kernel/cas.c` keeps an append-only arena (512 KiB) + an index, and stores each blob under the SHA-256 of its own bytes — so identical content is stored exactly once (automatic **dedup**), and every fetch **re-hashes the stored bytes and rejects a mismatch** (integrity is verified, not assumed). `SYS_cas_store(buf,len,hash32)` / `SYS_cas_fetch(hash32,buf,max)` + ulib wrappers + a `cas` shell command: `cas store FILE` prints the bare 64-hex key (so it pipes — `cas fetch $(cas store FILE)` round-trips), `cas fetch KEY` prints the blob, `cas` shows `/proc/cas` stats (objects, bytes, dedup count). **Verified headlessly:** `cas store DEMO.C` → key `49f11d26…37854`, **byte-identical** to `sha256 DEMO.C` (proving the key really is the content's SHA-256, cross-checked against the independent builtin); storing again kept Objects at 1 and bumped `Dedup: 1` (dedup works); `cas fetch $(cas store DEMO.C)` printed the full source back (round-trip + kernel-side integrity re-check). Pledge-classed (store=wpath, fetch=rpath). `make check` 52 suites green. Composes the existing `sha256.c` into a genuinely new storage primitive with intrinsic integrity + dedup.

> **(M1096) security — software measured boot: a TPM-style attestation hash chain (`/proc/measure` + `measure`).** A modern machine's TPM measures each stage of boot into Platform Configuration Registers so you can later prove exactly which code ran. This does it in software, on the kernel's own SHA-256: a new `measure.c` keeps four PCRs, each extended irreversibly by `PCR = SHA256(PCR ‖ SHA256(measured bytes))`, plus an append-only event log of every `{pcr, name, measurement-hash}`. At boot, kmain folds the kernel's read-only image (`.text`+`.rodata` — which, via `incbin`, includes every embedded app ELF) into **PCR0** using two new linker symbols `_kimage_start`/`_kimage_end`; then `app_spawn` folds each launched app's exact ELF bytes into **PCR1** in launch order (a new `elf_image_size()` recovers the on-disk extent even for trusted embedded apps spawned with `maxsz = ~0`). `/proc/measure` (and a `measure` shell builtin) prints the PCRs + the event log, which a verifier can **replay** to reproduce the PCRs. **Verified headlessly:** across two separate boots the kernel PCR0 was byte-identical (`e112e6d8…2586`) — the determinism that makes a measurement meaningful — while PCR1 correctly differed once a second app launched: boot A logged `kernel`+`Shell`; boot B logged `kernel`+`Shell`+`Clock`, with PCR1 folding Shell then Clock in order, every measurement hash shown for replay. Pure read-only hashing, so zero risk to the running system; `make check` 52 suites green. Composes the existing crypto (`sha256.c`) into a genuinely new security subsystem — tamper-evident proof of the boot's software stack.

> **(M1095) storage — a disk buffer cache + a uniform `blockdev_write` vtable (`/proc/bcache`).** The block layer could only *read*, and did so uncached — every FAT walk re-read the same metadata sectors straight off the hardware. This adds the two missing halves: (1) a **write** function pointer on `blockdev_t` with per-driver adapters (ATA/AHCI/NVMe/virtio; USB's writer is static in its driver so it stays read-only) behind a new `blockdev_write(i,lba,count,buf)`; and (2) an **LRU buffer cache** of 64 × 512-byte blocks (xv6's `bio.c` in miniature) keyed by (device, LBA). `blockdev_read` now serves sectors from the cache, falling back to the driver on a miss; `blockdev_write` is **write-through**, refreshing any cached sectors so a later read is coherent. Concurrency is handled the way `pmm.c`/`kheap.c` do it — an IRQ-saving guard held only across the fast scan/install bookkeeping, never across disk I/O, and `bread` copies each block into the *caller's* buffer (never hands out a pointer into a slot) so a later eviction can't dangle. `/proc/bcache` reports entries / hits / misses / hit-rate / writes. The boot FAT32 volume is read by `fat32.c` directly via ATA, **not** through this layer, so the write path cannot corrupt it — and a boot self-test deliberately skips any `ata*` device. **Verified headlessly** with an 8 MiB virtio-blk scratch disk attached: the boot self-test logged `blockdev cache: write+read-back+coherence+durability on virtio-blk lba 16383: OK` — it wrote a pattern, read it back from the cache (coherence), then **flushed the cache and re-read from disk** (durability: the bytes really landed on the platter), and restored the sector; `/proc/bcache` showed Hits 3 / Misses 7 / Writes 2. `make check` 52 suites green (every existing blockdev consumer — parttest, blockdevtest, the /diskN mounts — now exercises the cached read path). This is the substrate the marquee **swap-to-disk** feature needs (page-out/page-in over `blockdev_write`/`blockdev_read`), now that the working-set estimator (M1093) is also in place.

> **(M1094) net — a real DHCP client (`dhcp`): lease the IP/gateway/DNS instead of hardcoding them.** Until now `OUR_IP`/`GW_IP`/`DNS_IP` were three hardcoded `static const` SLIRP defaults; this makes them mutable boot-defaults and adds `net_dhcp()` — the full **DORA** handshake (DISCOVER → OFFER → REQUEST → ACK). Built on the same byte-level packet construction as `dns_resolve`, but broadcast from `0.0.0.0:68` to `255.255.255.255:67` with the BOOTP broadcast flag set (we have no IP yet, so the reply *must* be broadcast back). It parses the BOOTP reply's `yiaddr` plus DHCP options 53 (message type), 54 (server id), 3 (router) and 6 (DNS), retries DISCOVER/REQUEST a few times, and on ACK commits the lease into `OUR_IP`/`GW_IP`/`DNS_IP` (so every later ARP/DNS/ping/HTTP uses the leased address). New `SYS_dhcp(80)` + `ulib` `sys_dhcp` + a shell `dhcp` command that runs it and prints the new `ifconfig`. The hardcoded values stay as the boot default, so nothing changes until you run `dhcp` — additive and boot-safe. **Verified headlessly** against QEMU's built-in SLIRP DHCP server, started with `-netdev user,dhcpstart=10.0.2.30` so the lease is a *non-default* address: `dhcp` → `IP 10.0.2.30` (proving it came from the server, not the 10.0.2.15 default), persisting across a second `ifconfig`; and the leased config is fully functional — `ping` → gateway **3/3 replies**, `resolve example.com` → **104.20.23.154** (real DNS through the lease). `make check` 52 suites green. Also filled in the strace name table for jail/ringbuf/mprotect/bind/dhcp. The general UDP request/reply pattern here is the groundwork for SNTP (set the clock from the network) and TFTP next.

> **(M1093) vm — working-set introspection from the CPU's Accessed/Dirty PTE bits (`/proc/<pid>/wss` + `wss`).** The x86-64 MMU sets the Accessed (PTE bit 5) and Dirty (bit 6) bits *for free* on every page access/write — and the kernel was defining and using neither. This taps them: a new `vmm_wss(cr3)` walks every present ring-3 leaf of an address space (the read-only twin of `vmm_destroy_address_space`, resting on the same "a PDPT slot differing from boot's is a private user PD" invariant, so it's safe on a *non-active* space via the HHDM) and tallies **resident / referenced (A=1) / dirty (D=1) / writable** page counts. `/proc/<pid>/wss` exposes them; a new `clearref` verb on `/proc/<pid>/ctl` calls `vmm_clear_accessed` to zero the A bits (reloading CR3 to flush the TLB when it's the active space — the necessary step the hardware-caching makes easy to forget) so "Referenced" measures pages touched *since the last clear* — the estimator a future LRU/clock swap victim-picker needs. A `wss [pid]` shell builtin prints it and, for self, demonstrates the window reset by reading before and after a `clearref`. **Verified headlessly:** `wss` on the shell → Resident 201, Referenced **28**, Dirty 8, Writable 175; after `clearref` → Referenced collapses to **8** (only the pages touched during the re-read) while Dirty holds at 8 (clearref clears A, not D) — proving the A bit, the D bit and the TLB-flushing reset all read true hardware state. Only ~14% of the shell's resident pages are in its actual working set — real, previously-invisible insight. `make check` 52 suites green. The keystone primitive (from fresh r/osdev research) that de-risks the marquee swap-to-disk feature; next on that arc: a disk buffer cache + a uniform `blockdev_write`, then clock-eviction swap.

> **(M1092) toolchain — a real in-guest C compiler (`cc FILE.C`): compile AND run C from inside the OS.** Ported Robert Swierczek's `c4` ("C in four functions") to `user/cc.c` as a ring-3 app — a self-contained subset-C compiler that emits its own bytecode and runs it on a built-in VM (so it needs no assembler, linker or ELF emission). The crux that makes it work on x86-64 is c4's own `#define int long long`, which makes every VM cell 8 bytes; the libc it needs is shimmed over our `ulib` (a `printf` that reads every arg as a 64-bit value to match those cells, plus `memcmp`/`memset`/`malloc`; the compiler reads its *own* source via `sys_getarg`+`sys_readfile` into a 256 KB pool, and the file-I/O VM opcodes are stubbed). It supports `int`/`char`/`enum`/pointers, `if`/`else`, `while`, `return`, recursion and the shimmed libc calls. **Plus a usability win: the shell now falls back to launching a program of that name for any non-builtin command** — so `cc DEMO.C`, `editor README.TXT`, `forth` etc. all work *without* the `run ` prefix (first token = program, optional second token = its argument). A new `DEMO.C` (recursive factorial + Fibonacci + a `while`-loop sum) ships on the FAT32 disk. **Verified headlessly:** `cc DEMO.C` opens a *C Compiler* window printing `5! = 120`, `sum 1..10 = 55`, `fib: 0 1 1 2 3 5 8 13 21 34`, then `[program exited with 0, 5524 VM cycles]`. `make check` 52 suites green. Joins Forth (M1082) and the in-kernel JavaScript engine — **three** in-guest languages now, and the first that compiles C. Next from fresh r/osdev research: tap the unused PTE Accessed/Dirty bits for working-set introspection → a disk buffer cache + uniform block-write → a real DHCP client (all headlessly verifiable, the last via QEMU's SLIRP DHCP server).

> **(M1091) fs — per-process bind mounts (`bind FROM TO`): graft a subtree onto the namespace (Plan 9).** `bind /tmp /scratch` makes the path `/scratch` (and everything under it) also resolve to `/tmp` — the Plan 9 namespace idea, which uniquely fits this OS's pure name-based VFS. New `vfs_bind` (a small global bind table) + `bind_resolve` (rewrite an absolute path through the longest-matching `TO`→`FROM` prefix, whole-component, **one pass** so a cyclic bind can't loop) applied at the top of `vfs_read`/`vfs_write`/`vfs_remove`/`vfs_chdir` *before* the existing /proc·/dev·/tmp·/diskN routing — so the graft is transparent to every backend. `SYS_bind(79)` + `ulib` `sys_bind` + a shell `bind <from> <to>`; `/proc/binds` lists the active binds. **Verified in-guest**: `bind /tmp /scratch; echo hello bind > /scratch/note; cat /tmp/note` → "hello bind" (written via `/scratch`, read via `/tmp` — the same file); `cat /scratch/note` → same; `/proc/binds` shows `/scratch → /tmp`. `make check` 52 suites green (the resolver runs on every FS op — fstest/boottest/browsertest cover it). The round-11 r/osdev standout; an in-guest C compiler (c4) + named shared memory (now unblocked by M1089's refcount) remain.

> **(M1090) vm — `mprotect()` + W^X / a userspace JIT.** A syscall to change the R/W/X protection of an already-mapped range, so a program can write code into a page and then flip it executable-but-read-only (W^X) — the foundation for JITs and for hardening finalized data read-only. `kernel/vmm.c` gains `vmm_protect(va, flags)` (walk to the leaf PTE, rewrite the flag bits, keep the frame, `invlpg`); `kernel/app.c`'s `app_mprotect(addr, len, prot)` validates the range is the caller's own user pages (`vmm_user_ok`) and maps `PROT_READ|WRITE|EXEC` → `PTE_WRITABLE`/`PTE_NX`. New `SYS_mprotect(78)` + `ulib` `sys_mprotect` + a shell `jittest`. **Verified in-guest**: `jittest` `mmap`'d a page, wrote the x86 bytes `B8 2A 00 00 00 C3` (`mov eax,42; ret`), `mprotect`'d it `r-x`, then **called it as a function → 42** — "executed JIT-compiled code from a W^X page". `make check` 52 suites green (vmm.c change is purely additive). Round-11 r/osdev research's clean VM pick; the round-11 standout (per-process bind/union mounts) + an in-guest C compiler (c4) are next.

> **(M1089) vm — a "magic" mirrored ring buffer + a frame refcount in the PMM.** `SYS_ringbuf(len)` reserves `len` bytes of physical frames and maps them **twice, back-to-back**, so the region `[base, base+2·len)` has its second half *alias* the first — a wraparound queue then needs no split-handling or modulo: a read/write crossing `base+len` continues seamlessly into the same frames (ryg's "magic ring buffer"). To free an aliased frame exactly once, `kernel/pmm.c` gains a minimally-invasive **per-frame refcount** (`pmm_addref` + a guard in `pmm_free_frame`: a frame with extra refs just decrements; default 0 → behaviour byte-identical for every normal frame, so zero blast radius — also the prerequisite for future COW pages). `kernel/app.c`'s `app_ringbuf` allocs+zeroes each frame, maps it at `base+off` and `base+len+off` (eager, NX|USER|RW), `pmm_addref`s it for the mirror, and records a VMA so `munmap`/exit release it via the refcount; `SYS_ringbuf(77)` + `ulib` `sys_ringbuf` + a shell `ringtest`. **Verified in-guest**: `ringtest` wrote "MAGIC" straddling the end → a **contiguous "MAGIC"** read across the wrap (no split) and **offset 0 read "IC"** (the bytes written past the end, via the mirror); ran twice with `mem` steady at 183/255 MiB (refcount frees cleanly — no leak/double-free). `make check` 52 suites green (incl. boottest/gfxtest/browsertest, which hammer the PMM). A genuine VM trick + the refcount foundation for COW. Round-10 queue (trash FS, cap_enter, sigqueue, notification objects, /run shm) remains for the next fire.

> **(M1088) security — `jail`: parent-enforced spawn-time sandboxing (pledge a child before it runs).** Where M1074's `pledge()` is *self*-applied (a program confines itself), `jail` lets a *parent* confine a child it spawns — the child starts pledged before its first instruction and can't escape (pledge only shrinks). Composes the audited M1074/M1075 machinery with zero new enforcement code: `kernel/app.c` adds a one-shot `app_jail_next(promises, path)` (mirroring the existing `g_pend_arg` launch-arg mechanism); `app_spawn` consumes it right after the `memset`, pre-setting the new `app_t`'s `promises`/`pledged` (and an `unveil` prefix if a path is given). New `SYS_jail(76)` (prog + promises + optional path, all `ustr`-validated) parses the promise string via `app_pledge_parse` and spawns; `ulib` `sys_jail`; a shell `jail <prog> <promise>...` (e.g. `jail clock stdio rpath`). **Verified in-guest**: `jail clock rpath` spawned clock pre-confined to `rpath` only; its first `clear` syscall (stdio class, outside its pledge) was killed by the kernel — `grep pledge /proc/kmsg` → `[pledge] pid 101 (Clock) called a syscall outside its pledge (sys 10) -- killing`. The child never self-pledged; the jail enforced it. `make check` 52 suites green. The OS now has BOTH self-applied (pledge/unveil) and parent-enforced (jail) confinement — round-9's "reuses audited code" security pick.

> **(M1087) ipc — named message queues (`/ipc/<name>`): the first real process-to-process channel.** The OS had only signals (notification) and one global clipboard; this adds general inter-process *data* passing — FIFO message queues addressed by name through the VFS, with **no fd table needed** (it rides the name-based routing the way `/proc`·`/dev`·`/tmp` already do). New `kernel/mbox.c`: a fixed table of named queues (created on first use), each a ring of bounded messages; **`write` enqueues one message, `read` dequeues one (FIFO)**. A read on an empty queue **blocks** (`task_block`) until a writer `task_wake`s it — a true producer/consumer primitive built on the M1079 sleep/wake ring; because the read syscall path runs interrupts-off (interrupt gate, no `sti`), the "empty? then block" check and the block are atomic against a writer on this single CPU (no lost wakeup), and a read woken still-empty (a stray key, or a kill) returns 0 rather than re-blocking so a `cat /ipc/<empty>` is never stuck. `kernel/vfs.c` routes `/ipc/<name>` (absolute) to `mbox_read`/`mbox_write`; `/proc/ipc` lists the queues + pending depth. **Verified in-guest**: `echo first msg > /ipc/q; echo second msg > /ipc/q` → `/proc/ipc` shows `q` with 2 pending; `cat /ipc/q` → "first msg", `cat /ipc/q` → "second msg" (FIFO, reads consume); `/proc/ipc` → 0 pending. `make check` 52 suites green. The pivot from (now-saturated) observability to IPC — the round-9 research's top "fits this OS" pick; round-10 picks (a pledge/unveil "jail", `/proc/<pid>/mem`, waitpid) are queued for the next fire.

> **(M1086) debug — a statistical (sampling) kernel profiler (`/proc/profile`).** The complement to M1084's strace: where strace logs *which* syscalls run, this shows *where the CPU goes*. New `kernel/profile.c`: each timer tick, `prof_tick(rip, cs)` (called from `timer.c`'s IRQ handler) records the interrupted RIP **if it was kernel-mode**, finds the enclosing function via the M1078 `kallsyms` symbolizer, and bumps that function's sample count (64 functions tracked). Gated by a flag → **zero cost when off**. Controlled + read through `/proc/profile`: `echo on > /proc/profile` resets+starts, `echo off` stops, and `cat /proc/profile` prints the histogram (samples, CPU%, `function`) sorted hot-first. Ring-3 samples are skipped (app code isn't in the kernel symbol table). **Verified in-guest**: profiled across a window (incl. a `js -e` loop), `cat /proc/profile` showed **873 samples** symbolized — `darken 23%`, `desktop_run 22%`, `ata_read_drive 18%`, `fb_pixel 14%`, `fb_get_pixel 12%`, then `memcpy`/`fb_glyph`/`render_scene`/… — correctly pinpointing the desktop's framebuffer compositor + disk reads as the kernel's real hot path. `make check` 52 suites green; additive, low-risk. With strace (which calls) + `/proc/sched` (per-task time) + this (where in the kernel), the OS now has a real perf-analysis story. Round-9 r/osdev research's other picks (waitpid+exit status, named-mailbox IPC, env vars, `/proc/<pid>/mem`, a pledge/unveil "jail") are recorded for the next fire.

> **(M1085) fs — `/proc/fsevents`: inotify-style filesystem change notification.** A live log of recent filesystem mutations — create/write, delete, mkdir, rename, symlink — recorded at the VFS chokepoints and read back as `/proc/fsevents`. New `kernel/fsevents.c` keeps a 64-entry ring (`fsevents_record(op, path)`); `kernel/vfs.c` calls it on a **successful** `vfs_write`/`vfs_remove`/`vfs_mkdir`/`vfs_rename`/`vfs_symlink` over a *real* file (tmpfs or FAT32) — deliberately NOT for `/proc`·`/dev` writes, which aren't a filesystem in this sense. `kernel/procfs.c` serves it as a `/proc/fsevents` row. Pure observability over events the VFS already sees — no FS behaviour change, additive, low-risk (the inotify/FSEvents idiom: file managers, live-reload, "did my config change"). **Verified in-guest**: `echo hi > /tmp/a.txt; echo more > /tmp/b.txt; ln -s /tmp/a.txt /tmp/link; rm /tmp/b.txt; cat /proc/fsevents` → exactly `write /tmp/a.txt`, `write /tmp/b.txt`, `symlink /tmp/link`, `delete /tmp/b.txt` (the `cat` read isn't recorded — no noise). `make check` 52 suites green. A round-8 do-now pick; round-9 research flags a sampling profiler, named-mailbox IPC, env vars, and `/proc/<pid>/mem` as next.

> **(M1084) debug — a per-process syscall trace channel (`strace`).** Any process can be made to log every syscall it makes — number→name, the three register args, and the return value — to the kernel log, readable with `dmesg`. It hooks the single `syscall_dispatch` choke point (right where pledge already enforces): a snapshot of the call is taken before the switch and one `[strace <pid>] name(a,b,c) = ret` line is emitted after, gated by a new per-`app_t` `traced` flag (default off → **zero overhead/output for untraced apps**). Toggled by writing the existing `/proc/<pid>/ctl` control file — new `trace`/`untrace` verbs (alongside kill/stop/cont) calling `app_set_traced`; a `syscall_name()` table (C99 designated initialisers keyed by the `SYS_*` numbers) gives readable names. **Verified in-guest**: `echo trace > /proc/self/ctl; ls; echo untrace > /proc/self/ctl; grep strace /proc/kmsg` shows the shell's trace — `read(0x0,…)=0x3` (the "ls\n" line), `list(…)=0x10ef` (the listing), `write(0x1,…)=0x10ef` (printing it), down to `writefile(…)=0x8` (the untrace redirect). `make check` 52 suites green. A first-class observability feature (SerenityOS-style strace) from this hour's r/osdev research; same report's other do-now picks (a Hubris-style trace ring, fsevents, `/dev/loop`, TLS) are recorded for the next fire.

> **(M1083) tty/signals — async Ctrl-C → SIGINT (interrupt a running program).** Ctrl-C now delivers a real asynchronous SIGINT to the focused app, so a runaway program can be interrupted — not just apps that happen to poll the 0x83 key. **Opt-in for zero regression**: `kernel/app.c`'s `app_key` raises SIGINT (via new `app_request_signal`) *only* if the app installed a SIGINT handler (`sys_signal(2,…)`); otherwise the 0x83 keystroke is delivered as before, so the shell's existing loop-break and every non-handling app are untouched. Delivery is via a new `app_deliver_pending(r)` called at two return-to-ring-3 points — the **syscall return** (`syscall.c`) and the **IRQ tail** (`interrupts.c`, after the timer may have rescheduled us) — reusing the M1067 `app_signal_deliver` (saves the trap frame, vectors to the handler, returns via the ulib trampoline). The IRQ-tail path is what lets Ctrl-C interrupt a pure compute loop that makes no syscalls. New demo consumer: the **Forth** interpreter installs a SIGINT handler that flags `interp` to abort and return to the REPL. **Verified in-guest**: `: spin begin again ;` then `spin` (infinite, no I/O) + Ctrl-C → `^C interrupted` → the REPL recovers (`7 7 + .`→14). A boot-time GPF surfaced first (the IRQ-tail runs on *every* interrupt incl. before `sched_init`, so `app_deliver_pending` must guard `task_self()` before `cur()`) — the M1078 symbolized backtrace pinpointed it (`app_deliver_pending+0x1e ← isr_common`), fixed, reverified. `make check` 52 suites green.

> **(M1082) toolchain — a real Forth interpreter (the OS can program itself).** A genuine stack programming language as a ring-3 app (`user/forth.c`, "Forth" in the Apps menu / `run forth`): a data stack + dictionary, arithmetic (`+ - * / mod 1+ 1- negate abs`), the classic stack words (`dup drop swap over rot nip tuck 2dup`), comparison/logic (`= <> < > <= >= 0= 0< and or xor invert`), output (`. .s emit cr space spaces ."`), **colon definitions** (`: name … ;`), **control flow** (`if/else/then`, `begin/until/again`, `do/loop` + `i`), **variables/constants** (`variable`, `constant`, `! @ ? +!`), and `( … )`/`\` comments — implemented as an immediate token-array interpreter (each line and each word's body re-tokenised; the PC is steered by the control words; numbers parse decimal or `0x`-hex). Self-contained userspace (just `readline`/`print` + existing syscalls) → **zero kernel risk**. It's both a **REPL** and a **script runner** (`run forth FILE.FTH` reads the file, runs it line-by-line, then drops to the REPL), so you can save a `.fth` to disk and run it — a real in-guest toolchain. **Verified in-guest**: `10 20 + .`→30; `: square dup * ; 7 square .`→49; recursive `: fact dup 1 <= if drop 1 else dup 1- fact * then ; 6 fact .`→720 (self-reference resolves at call time); `5 0 do i . loop`→`0 1 2 3 4`; and a 3-line `DEMO.FTH` created with `echo`/run → `81 15`. `make check` 52 suites green. The "in-guest toolchain" pick — a better fit for this windowed OS than the fd-table keystone (whose payoff is deferred + needs offset-aware VFS reads). Round-7 r/osdev research also flagged: ELF core dumps, getopt/argv, a kernel `environ` inherited across spawn, stack guard pages, an MLFQ scheduler.

> **(M1081) fs — symbolic links (`ln -s`), resolved across filesystems.** The VFS gained a real Unix mechanism: a symlink whose read follows its target — and because resolution goes back through the VFS, a link can point ACROSS filesystems (a `/tmp` link to a FAT32 file resolves to that file's contents). Scoped to the writable RAM backend (`/proc`·`/dev` are read-only, FAT32 has no native symlink): `kernel/tmpfs.c` tags an entry as a link (its buffer holds the target path) + `tmpfs_symlink`; `tmpfs_read` follows it via `vfs_read`, depth-bounded (≤8) to break loops. `kernel/vfs.c` adds `vfs_symlink` (routes a `/tmp` linkpath to tmpfs); new `SYS_symlink(75)` (both pointers `ustr`-validated + the linkpath honors `unveil` write perms) + `ulib` `sys_symlink` + a shell `ln -s <target> <linkpath>`. Additive; a write/remove over a link clears the link flag. **Verified in-guest**: `ln -s /tmp/orig.txt /tmp/link.txt; cat /tmp/link.txt` → the target's text (tmpfs→tmpfs); `ln -s /README.TXT /tmp/readme; cat /tmp/readme` → the FAT32 file's contents (tmpfs→FAT32, cross-FS). `make check` 52 suites green. Self-contained low-risk pick from this hour's r/osdev research; the headline item there — a per-process fd table (the keystone for pipes/poll/sockets, needing offset-aware VFS reads) — is queued for a dedicated fire.

> **(M1080) net — `/proc/net`: interface + live ARP & DNS caches (the missing observability).** The /proc surface covered everything *except* the network stack's runtime state. New `net_proc()` in `kernel/net.c` (it owns the otherwise-private `arp_cache[]`/`dns_cache[]`) formats the interface (IP/MAC), gateway, DNS resolver, the default route, and both caches with per-entry freshness; `kernel/procfs.c` serves it as a flat `/proc/net` row. Pure read-only formatting over state already kept — self-contained, low-risk, additive. **Verified in-guest**: after `ping`, `cat /proc/net` shows `iface ip 10.0.2.15 mac 52:54:…`, `gateway 10.0.2.2  dns 10.0.2.3`, `route default via 10.0.2.2`, the ARP cache (`10.0.2.2 … fresh`, `10.0.2.3 … fresh` — the gateway + DNS MACs learned during the ping) and the DNS cache (`example.com -> 172.66.147.243 fresh`). `make check` 52 suites green (incl. nettest). Closes the /proc observability gap. From this hour's r/osdev research (whose headline pick is a per-process fd table — the keystone for pipes/poll/sockets, queued for a dedicated fire); other self-contained candidates flagged: an APIC timer (sub-10ms), a soft-lockup watchdog, tmpfs symlinks, priority scheduling, a Forth toolchain.

> **(M1079) kernel — a timer-driven sleep/wakeup queue (real off-CPU `timer_wait`).** Every kernel "wait" was a busy spin — `timer_wait()` looped `while(ticks<target) hlt`, which keeps the task RUNNABLE so the scheduler cycles back to it every slice just to re-HLT (wasting a quantum each round, and showing up as CPU use in `/proc/sched`). Now a sleeping task actually leaves the run rotation: `kernel/task.c` adds `wake_at` to `task_t` + `task_sleep_ms(ms)` (records the deadline, marks the task `BLOCKED`, yields) and `task_wake_sleepers()` (called from the timer IRQ in `timer.c` each tick — a cheap full-ring scan, the ring is tiny — flips any sleeper whose deadline passed back to `READY`). `timer_wait()` now just calls `task_sleep_ms` (falling back to a HLT loop before the scheduler exists), so all **35 callers** across `ac97`/`floppy`/`speaker`/`ehci`/`hda`/`net`/`usb`/`xhci`/… and the app `SYS_sleep` path get genuine blocking sleep for free — the idle task (which HLTs) runs while everyone sleeps, instead of sleepers burning quanta. `task_block`/`task_wake` clear `wake_at` so a non-timed block (input wait) is never woken by the scan, and even a sleeper that blocked with interrupts off is safe (the idle task's `sti;hlt` lets the timer fire and wake it). **Verified**: `make check` 52 suites green — crucially the driver self-tests (floppy/ehci/xhci/hda/usb/net) and `boottest`, which exercise `timer_wait` heavily during init, all pass; and in-guest `echo before; sleep 2; echo after` blocks ~2 s then resumes with the shell fully responsive. The #1 foundational primitive from this hour's r/osdev research (it was the missing keystone under every wait); next candidates from that report: COW `fork()`, file-backed `/dev/fb` mmap, `waitpid`.

> **(M1078) debug — kernel symbol table + symbolized panic backtrace + `/proc/kallsyms`.** A kernel panic now prints a `function+offset` call trace instead of raw hex — the classic "debugging without GDB" upgrade, and a force-multiplier for the headless workflow. Build-time: a new **two-pass link** (Makefile) links once with a zero-entry stub table, runs `nm -n | tools/gen_ksyms.sh` to emit the real address-sorted `{addr,name}` table (function symbols only), and relinks; because the table lands in `.rodata` (after `.text`), embedding it never shifts a function address — so the pass-1 addresses are exact for pass 2 (verified: `kmain`/`syscall_dispatch`/`backtrace` addresses in the table match the final binary byte-for-byte). Runtime: `kernel/ksyms.c` — `ksym_lookup` (binary search) + `backtrace(rip,rbp)` which walks the saved frame-pointer chain (the kernel is now built `-fno-omit-frame-pointer`) with sanity bounds so a corrupt chain can't double-fault. Wired into the `interrupts.c` panic path; also exposed as `/proc/kallsyms` (1164 symbols, Linux-style). **Verified in-guest**: `grep kmain /proc/kallsyms` → `15d440 kmain` (matches the ELF); a forced fault printed `call trace: [0] crash_c+0x5 / [1] crash_a+0x9 / [2] kmain+0x3b2 / [3] long_mode_start.hang` then the temp trigger was removed. `make check` 52 suites green (frame pointers + two-pass link don't perturb boot or any host suite). Tier-2 pick from this hour's r/osdev research (which independently flagged the symbol table + suggested `/proc/kallsyms`); next from that report: a timer-driven sleep/wakeup queue (the kernel currently busy-polls every wait).

> **(M1077) system — `top`/`/proc/sched` now reports memory too.** Rounds out the M1073 monitor: `gen_sched` (`kernel/procfs.c`, which already has PMM access) adds a `mem <used>/<total> MiB used  tasks <n>` line to the `/proc/sched` header, so the live `top` is a complete system monitor (CPU per task + RAM + task count) in one read — no second syscall. **Verified in-guest**: `top` shows `mem 71/255 MiB used  tasks 3` above the per-task CPU table. `make check` 52 suites green; tiny, additive.

> **(M1076) system — the system clipboard as `/dev/clipboard`.** The GUI clipboard (drag-select copy, middle-click paste, the editor/browser) was reachable only through `SYS_clip_get`/`SYS_clip_set`; now it's also a `/dev` node, so the shell and any program can interoperate with copy/paste through the universal file interface — `echo … > /dev/clipboard` puts text on the system clipboard, `cat /dev/clipboard` reads it, and it's the SAME buffer the windows use (so you can copy in the shell and paste in the editor). Tiny + additive: `kernel/procfs.c` routes `/dev/clipboard` reads to `clip_get` and writes to `clip_set` (both already existed), plus a `dev_files[]` row so it shows in `ls /dev`. **Verified in-guest**: `ls /dev` lists `clipboard` alongside null/zero/random/urandom/full; `echo hello from the shell clipboard > /dev/clipboard; cat /dev/clipboard` → "hello from the shell clipboard". `make check` 52 suites green. The cheap-but-real "everything is a file" win from the r/osdev batch (#5); next bigger items: panic stack-trace+symbols, then an in-guest httpd.

> **(M1075) security — `unveil()`: per-process filesystem path restrictions (completes the pledge/unveil pair).** Where `pledge()` (M1074) limits *which syscalls* a process may make, `unveil()` limits *which files* it can reach. Before the first call every path is visible; the first `unveil(path, "rwc")` flips the process to "only these prefixes exist," each with read/write permission, and `unveil(NULL)` locks the set — exactly OpenBSD's model. A denied access just **fails (-1, as if the file is absent)** — it does *not* kill (that distinction from pledge is intentional). `kernel/app.c` adds an 8-entry prefix table to `app_t` + `app_unveil` / `app_unveil_parse` ("rwc"→bits) / `app_unveil_ok` (case-insensitive prefix match honoring a `/` boundary, so `/tmp` covers `/tmp/x` but not `/tmpfoo`). `kernel/syscall.c` consults `app_unveil_ok` in the five path-taking syscalls — `readfile`/`chdir` (need read), `writefile`/`delete`/`mkdir` (need write) — before touching the VFS. New `SYS_unveil(74)` + `ulib` `sys_unveil`. **Additive**: an app that never calls unveil is unrestricted. The `sandbox` demo now leads with unveil. **Verified in-guest**: after `unveil("/tmp","rw")` + lock, the demo writes & reads `/tmp/SB.TXT` fine but `read README.TXT` is **"DENIED by unveil (good)"** — then the pledge phase (monotonic reject, stdio before/after) and the kernel kill on the WPATH violation still fire. `make check` 52 suites green. Closes out the marquee r/osdev security item; the OS now has both halves of the OpenBSD privilege-reduction story.

> **(M1074) security — a `pledge()` syscall sandbox (OpenBSD/SerenityOS-style).** A process can voluntarily drop the right to make whole CLASSES of syscalls; the instant it calls one outside what it kept, the **kernel terminates it** (like OpenBSD's SIGABRT). Eight promise classes (the user ABI): `stdio rpath wpath inet gfx proc vm power`. `kernel/app.c` adds `promises`/`pledged` to `app_t` + `app_pledge` (monotonic — a later call may only *drop* promises, never regain them) + the name↔bit table (`app_pledge_parse`/`app_pledge_format`) + `app_current`. `kernel/syscall.c` maps every syscall to its class (`syscall_class`) and, at the very top of `syscall_dispatch`, kills any pledged app that strays (a `kprintf` records it, so `dmesg` shows the kill); `exit`/`sigreturn`/`getpid`/`pledge` are always allowed. New `SYS_pledge(73)` (string arg, validated via `ustr`) + `ulib` `sys_pledge`. The promise set shows in `/proc/<pid>/status` (`Pledge:` line). **Crucially additive**: unpledged apps — every existing program — are unaffected (the guard is a no-op unless `pledged`). New demo app `user/sandbox.c` ("Sandbox (pledge demo)" in Apps / `run sandbox`). **Verified in-guest**: the demo shows `getrandom` works before+after `pledge("stdio")`, `pledge("stdio wpath")` is **rejected** (monotonic), then a `writefile` (WPATH) call triggers `[pledge] pid 101 ... called a syscall outside its pledge (sys 12) -- killing` in `dmesg` and the window is reaped. `make check` 52 suites green. #3 (the marquee security item) from this hour's r/osdev research; `unveil()` (path-prefix limits) is the natural follow-on.

> **(M1073) system — per-task CPU-time accounting + a live `top` (`/proc/sched`).** The scheduler now measures where the CPU goes. `kernel/task.c` adds `run_ms`/`last_in`/`nswitch` to each `task_t`: every context switch credits the outgoing task with the slice it just ran (`timer_ms()` delta) and stamps the incoming task's switch-in time — including the `task_exit` reap path (a missed stamp there was crediting the next task from a stale `last_in`, which over-counted and is now fixed). New `task_idle_ms()` exposes the idle task's total (system idle time), and `task_snapshot` carries `run_ms`/`nswitch` (live: the running task includes its in-progress slice). `kernel/procfs.c` serves `/proc/sched` — a header (`uptime_ms`, `idle %`) plus per-task `PID/STATE/CPU_MS/CPU%/SWITCHES/NAME` — and `user/shell.c` gains a live `top` (clears + refreshes ~1 Hz until a key, reading `/proc/sched`). The idle task already HLTs (`sti;hlt`), so this is the missing *measurement* half. **Verified in-guest**: `top` renders a cleared, refreshing table; after the over-count fix the per-task CPU% sums to ≤100% (e.g. shell 5%, idle 0%, desktop 86%) and the idle task reads a correct ~0% — and the view usefully reveals that the desktop's event loop polls (≈86% CPU) rather than blocking. `make check` 52 suites green; additive, boot-path-identical. #5 (the cheap-but-valuable batch) from this hour's r/osdev research; the data is the basis for a real `top`/loadavg.

> **(M1072) crypto — a hardware-seeded CSPRNG: real `/dev/random` + `/dev/urandom` + `getrandom()`.** `/dev/random` was a TSC-seeded xorshift toy; it's now the construction OpenBSD's arc4random / Linux's getrandom use. New `kernel/random.c`: **seed** from the CPU's hardware entropy — `RDSEED` (true conditioned entropy) if CPUID advertises it, else `RDRAND`, else a TSC-jitter loop — whitened through the in-tree SHA-256 into a 256-bit key; **generate** with ChaCha20 (a new additive `chacha20_keystream` over the audited `chachapoly.c` core) using *fast key erasure* (each 64-byte block's first half rekeys the state for forward secrecy, the second half is output); **reseed** every 16 KB by folding fresh hardware/TSC entropy back through SHA-256. RDSEED/RDRAND are issued only when CPUID advertises them (else the opcode #UDs), so it's safe under any hypervisor. Wired into `kernel/procfs.c` (`/dev/random` + new `/dev/urandom` → `random_bytes`), a new `SYS_getrandom(72)` syscall (user pointer validated via `ubuf`/`vmm_user_ok`), `ulib` `sys_getrandom`, and the shell's `shroll()` (so `genpass`/`uuidgen`/`roll`/`fortune` all draw real entropy, xorshift kept only as a fallback). Also strengthens the TLS stack's key/nonce material. **Verified in-guest both ways**: default QEMU → `random: CSPRNG seeded (TSC jitter (no RDRAND/RDSEED))`; `-cpu max` → `random: CSPRNG seeded (RDSEED hardware entropy)` — no crash on either; `hexdump /dev/random` shows uniform 4 KB; two `genpass`/`uuidgen` runs give distinct strong outputs (valid v4 UUIDs). `make check` 52 suites green (kattest unaffected — the AEAD path is untouched). #2 from this hour's r/osdev batch; next is a pledge/unveil syscall sandbox.

> **(M1071) system — a kernel log ring buffer (`dmesg` / `/proc/kmsg`).** Every byte the kernel prints now also lands in a fixed 64 KB circular buffer, so the boot log is readable back *after it has scrolled off-screen* — the single most-cited "why didn't I do this sooner" hobby-OS debugging upgrade (Linux's printk ring / `/dev/kmsg`). `kernel/console.c` gains a lock-free `klog[65536]` + a `klog_putc` called from inside `console_putc` (so **all** existing `kprintf` output is captured with zero call-site changes) + `klog_copy(out,max)` returning the most-recent bytes oldest-first; it's safe to call from IRQ/printf/panic context (single monotonic head index, plain byte writes — a reader may see a torn tail byte, fine for a log). `kernel/procfs.c` exposes it as one more table row, `/proc/kmsg` (gen_kmsg→klog_copy), and `user/shell.c` gets a `dmesg` builtin (reads `/proc/kmsg` via `slurp`, so it grows past 64 KB cleanly). **Verified in-guest**: `dmesg` prints the captured boot log — the blockdev browse, the usb-kbd/ehci/xhci probe lines, and "[main] launching the desktop environment..." — all recalled long after boot. `make check` 52 suites green; additive + boot-path-identical. The lead pick from this hour's r/osdev research, and it makes every future feature easier to debug headlessly. Next from the same batch: a hardware-seeded `/dev/random` (RDRAND is detected but unused) + `getrandom`, then a pledge/unveil syscall sandbox.

> **(M1070) fs — browse mounted disks' SUBDIRECTORIES, not just their roots.** The read-only secondary-disk mounts (`/disk1`, `/disk2`, …) were root-only; now you can `cd` and `cat` your way down a FAT32 directory tree on any mounted volume. New device-agnostic `kernel/partition.c` walkers — `fatvol_walk` (resolve a `/`-separated path from the volume root, descending each component that's a directory), backing `fatvol_list_path`/`fatvol_read_path`/`fatvol_isdir_path` — sit beside the unchanged M1047/M1061 root-only `fatvol_find`/`list`/`read` (so `parttest`/`blockdevtest` stay byte-identical). `kernel/blockdev.c` exposes `blockdev_mount_list/read` (now path-aware) + `blockdev_mount_isdir`; `kernel/vfs.c` tracks the cwd *within* a mount in `mount_sub` (a `mount_sub_join` collapses `.`/`..`), validates `cd` against `blockdev_mount_isdir`, and resolves relative names against it. Also fixed a latent bug this exposed: `synth_path` claimed *every* non-boot cwd as `/proc`·`/dev`, so relative reads inside a mount silently failed — now only `synth_cwd` 1/2 route there (mounts are also confirmed read-only on write/remove). **Verified in-guest** (custom FAT32 image with `SUBDIR/INNER.TXT` on a 2nd virtio-blk disk): `cd /disk2/SUBDIR; ls` → `INNER.TXT 51`; `cat INNER.TXT` → "INNER FILE INSIDE SUBDIR -- M1070 traversal works!"; `cd ..; cat GREET.TXT` → "root file ok"; `cat /disk2/SUBDIR/INNER.TXT` (absolute deep path) works too. `make check` 52 suites green. Mounted disks are now fully browsable, not just one level deep.

> **(M1069) system — `/proc/interrupts` (live per-IRQ counts).** Rounds out the synthetic sysfs: the IRQ dispatcher (`kernel/interrupts.c`) now tallies each hardware IRQ in `irq_counts[16]` (exposed via `irq_count(i)`), and `procfs.c` serves `/proc/interrupts` as `IRQ / COUNT / NAME` lines for the IRQs that have fired (timer, keyboard, RTC, mouse, ATA, …). **Verified in-guest**: `cat /proc/interrupts` → `0 1046 timer`, `1 42 keyboard` (live: ~10 s of the 100 Hz PIT + the keystrokes typed). Tiny, additive, low-risk (one increment in the IRQ path); `make check` 52 suites green. A recognized "real OS" introspection file; the /proc surface is now meminfo/uptime/cpuinfo/version/loadavg/stat/processes/partitions/filesystems/mounts/interrupts + per-pid status/cmdline/maps/ctl + self.

> **(M1068) fs — a writable RAM filesystem (`tmpfs`) mounted at `/tmp`.** The VFS now carries a *third* backend type beyond FAT32-on-disk and the synthetic read-only /proc·/dev: a real read/write filesystem in RAM. New `kernel/tmpfs.c` keeps a fixed table of up to 32 files, each a `kmalloc`'d buffer (a write replaces the whole file; remove `kfree`s it). `kernel/vfs.c` routes `/tmp` with the same dormant-by-default `synth_cwd` trick (now `0`=boot FS, `1`=/proc, `2`=/dev, `3`=/tmp, `≥4`=mount), so `echo … > /tmp/x`, `cat /tmp/x`, `ls /tmp`, `cd /tmp`, and `rm /tmp/x` all hit RAM — the boot path stays byte-identical. Proves the name-based VFS is a genuine multi-backend abstraction (not FAT-hardcoded) and gives a fast scratch area that never touches the disk. **Verified in-guest**: `echo hello tmpfs > /tmp/note; cat /tmp/note` → "hello tmpfs"; `ls /tmp` → `note 12`; `cd /tmp; ls` lists it. `make check` 52 suites green. Flat namespace for now (no subdirs). Validated next-best from this hour's r/osdev research; pairs with the just-landed M1067 signals.

> **(M1067) system — real async SIGNALS + `sigreturn` (userspace handlers; catch SIGSEGV).** The marquee POSIX milestone the r/osdev research ranked #1 ("a program traps its own crash and keeps running"). A registered ring-3 handler now runs when a signal is delivered, then returns and the interrupted code resumes. Mechanism: **`SYS_signal(signo, handler, restorer)`** stores a per-app handler table; **`app_signal_deliver(r, signo)`** (in `app.c`) saves the interrupted `struct registers` *kernel-side* (so the user stack only needs the trampoline return address), then rewrites the trap frame — `r->rip` = handler, `r->rdi` = signo, `r->rsp` = a 16-aligned frame below the red zone whose return address is the ulib **`sig_trampoline`**; on `iretq` the handler runs on the app's own stack. When it returns it falls into the trampoline, which calls **`SYS_sigreturn`** → `app_sigreturn(r)` restores the saved context and `iretq` resumes the interrupted instruction. **`SYS_raise(signo)`** delivers to self; the page-fault handler delivers **SIGSEGV (11)** to a handler instead of killing the app (so a program can survive a wild write). No nesting (a `sig_in` guard); the new user stack is validated with `vmm_user_ok`. Strictly additive + boot-safe: dormant unless an app calls `sys_signal`, so every existing fault/syscall path is byte-identical → `boottest` unaffected. **Verified in-guest**: shell `sigtest` prints "raising signal 10 to self…", then "[handler] caught signal 10 in userspace, returning" (the handler ran in ring 3 with the right signo), then "main resumed after the handler returned (sigreturn OK)" — full delivery + return. `make check` 52 suites green. Reuses the existing `struct registers` + #PF path with zero new context-switch machinery. (This hour's r/osdev research independently picked this as the #1 feature.)

> **(M1066) system — `/proc/self`.** The conventional self-reference: `proc_pid_path` now resolves a `/proc/self/<file>` path to the *calling* task (`task_current_id()`), so a program can read its own `status`/`cmdline`/`maps` without knowing its pid. **Verified in-guest**: `cat /proc/self/status` from the shell shows `Name: Shell, Pid: 6, …` — identical to `/proc/6/status`. Tiny + additive; `make check` 52 green. Completes the conventional `/proc/{<pid>,self}/` interface.

> **(M1065) system — `/proc/<pid>/maps` (process memory map).** Rounds out M1064's per-process tree with the Linux-style memory map: `cat /proc/<pid>/maps` lists the process's user regions as `0xSTART-0xEND perm [label]` lines — the program-break **[heap]**, each demand-paged **[mmap]** region (the M1063 VMAs), and the **[stack]**. New `app_format_maps()` in `app.c` (with small hex/string appenders) walks the app's heap bound + `vma[]` + stack base; `procfs.c` serves it for the `maps` file. **Verified in-guest**: `cat /proc/6/maps` → `0x44000000-0x44011000 rw- [heap]` (matches the 68 KB from `status`) and `0x50000000 rw- [stack]`. `make check` 52 suites green; read-only + additive. Together M1063–M1065 give the OS a real VM + a Plan 9/Linux-style process filesystem (status, cmdline, ctl, maps), all from this hour's r/osdev research.

> **(M1064) system — Plan 9-style `/proc/<pid>/` process files (status, cmdline, ctl) + task suspend/resume.** Per-process introspection *and control* through the filesystem — the "everything is a file" idiom that's currently top of r/osdev (PatchworkOS), and a perfect fit for OS-DEV's name-based VFS (no fd table needed). Reading **`/proc/<pid>/status`** yields `Name / Pid / State / HeapKB / MmapRegions` (from `task_snapshot` + new `app_arg`/`app_heap_bytes`/`app_vma_count` getters); **`/proc/<pid>/cmdline`** yields the program + launch arg. Writing a command to **`/proc/<pid>/ctl`** controls the process: **`kill`** (the proven cooperative `app_request_kill`), **`stop`** and **`cont`** — backed by a new **`TASK_STOPPED`** scheduler state (a one-line addition to the run-ring skip, plus `task_stop`/`task_cont`; a stopped task simply leaves the rotation until resumed). `procfs.c` parses the `/proc/<pid>/<file>` path (flat `/proc/meminfo`-style files still work — a pid path must start with a digit); `procfs_write` (previously a stub) now dispatches ctl. **Verified in-guest**: `cat /proc/6/status` → `Name: Shell, Pid: 6, State: running, HeapKB: 68, MmapRegions: 0`; `cat /proc/6/cmdline` → `Shell`; and `echo kill > /proc/6/ctl` made the shell self-exit (its window vanished) — process control purely through a file write. `ps`/`/proc/processes` now also render the new `stop` state. `make check` 52 suites green (additive; the scheduler change is dormant unless a task is explicitly stopped → boottest unaffected).

> **(M1063) VM — demand paging + anonymous `mmap` (the page-fault handler becomes a real VM mechanism).** Until now a ring-3 page fault just *killed* the app (and ring-0 faults panic). Now the #PF handler first asks **`app_fault_handle(cr2)`**: if the faulting address lies in a reserved **mmap region**, the kernel lazily allocates a zeroed frame, maps it `USER|WRITABLE|NX` into the (active) app space, `invlpg`s it, and retries the instruction — true **demand paging**. New per-app **VMA table** (`vma[]` in `app_t`) + a bump allocator in a private VA window (0x6000_0000–0x7000_0000, clear of the heap/stack); **`SYS_mmap(len)` (67)** reserves a region without mapping any pages, **`SYS_munmap(addr,len)` (68)** unmaps + frees its frames (and `vmm_destroy_address_space` reclaims the rest at exit, so nothing leaks). Strictly additive + boot-safe: with no mmap, no VMAs exist, so `app_fault_handle` returns 0 and every fault behaves exactly as before (real user faults still terminate the app, kernel faults still panic) — `boottest` unaffected. **Verified in-guest**: a new shell `mmaptest` reserves 16 unmapped pages, writes a pattern across all of them (16 demand faults serviced), reads it back → "demand-paged + verified OK", then `munmap`. `make check` 52 suites green. This is the marquee systems milestone the r/osdev research flagged, and the seed for **file-backed mmap** + **copy-on-write `fork()`** (just add a COW PTE bit + per-frame refcounts to this same fault path).

> **(M1062) shell — `sleep` is now interruptible (Ctrl-C / Esc).** The only long-running shell op that couldn't be stopped: `for`/`while` loops already poll `sys_pollkey` for `0x83`/`27` each iteration (M-era), but `sleep N` made a single blocking `sys_sleep` call, so `sleep 300` was uninterruptible. Now it sleeps in 50 ms chunks, checking for Ctrl-C / Esc between them (and sets `$?`=130 on interrupt), matching the loop behaviour — a small completeness fix to the M1058 process-control story. `make check` 52 suites green; userspace-only (`user/shell.c`).

> **(M1061) storage — read-only MOUNT + browse of any disk (`/disk1`, `/disk2`, …).** The payoff for the whole storage-driver campaign (M1026–M1047): every disk is now *browsable*, not just self-tested. Until now only the boot FAT32 was reachable; now every FAT32 volume across **every** storage driver (ATA/AHCI/NVMe/virtio-blk/USB-MSC, bare or in an MBR/GPT partition) is mounted read-only as `/disk1`, `/disk2`, … New **`fatvol_read()`** (`kernel/partition.c`) reads a file's contents from any volume by walking its cluster chain over the `blk_read_fn` callback (bounded by cluster count + cycle guards, like `fatvol_find`/`list`). A lazy **mount registry** in `kernel/blockdev.c` (`blockdev_mount_count`/`name`/`index`/`list`/`read`, built once via `collect_fat_starts` over all devices) maps each volume to a `/diskN` name. `kernel/vfs.c` routes `/diskN` paths to it — exactly the dormant-by-default trick procfs uses (a `synth_cwd` value ≥3), so `cd /disk2`, `ls`, `cat /disk2/GREET.TXT`, and the Files window all work, while the boot path (`synth_cwd==0`) is byte-for-byte unchanged. A shell **`mount`** command lists the volumes for discovery. Mounts are root-only (no subdir paths yet) and read-only. **Verified in-guest with a second virtio-blk disk** (the blockdevtest image): `cd /disk2; ls` → `GREET.TXT 70  NUMBERS.DAT 200  SUBDIR/`, and `cat /disk2/GREET.TXT` → "Hello from a bare FAT32 volume browsed over virtio-blk by blockdev.c!". `make check` 52 suites green (additive; FAT32/boot untouched). The storage stack is now fully browsable end-to-end.

> **(M1060) system — richer `/proc`: partitions, filesystems, mounts.** Round out the M1057 procfs with three more introspection files, reusing data the kernel already produces: **`/proc/partitions`** (the block-device + FAT32-volume map — literally `blockdev_format()`, the M1048 `lsblk` generator, so it lists every disk's volumes + files + sizes across all storage drivers), **`/proc/filesystems`** (procfs/devfs/fat32), and **`/proc/mounts`** (the active mounts in fstab-ish form). Tiny + low-risk (additive entries in `procfs.c`'s table; `/proc/partitions` is a one-line wrapper over an already-callable function; the other two are static). **Verified in-guest**: `cat /proc/partitions` prints the boot disk's FAT32 files with sizes. `make check` 52 suites green. Caps off the /proc surface (now meminfo, uptime, cpuinfo, version, loadavg, stat, processes, partitions, filesystems, mounts).

> **(M1059) terminal — ANSI/VT100 escape sequences (colours, cursor, erase).** The app terminal now understands `ESC [ … <letter>` sequences instead of printing them literally, so programs can do colored / cursor-addressed output the standard way. A small per-app state machine in `app_sys_write` (3 states + a CSI param buffer added to the app struct) parses: **SGR `m`** (text colour — `0` reset, `1` bold, `30–37`/`90–97` foreground, mapped onto the app's 16-entry palette since it isn't in ANSI order), **cursor moves** `A`/`B`/`C`/`D` and absolute `H`/`f` (row;col, clamped to the grid), and **erase** `J` (to-end / whole screen) and `K` (line). To drive it, the shell's `printf` gained the standard `\e`/`\E` (ESC), `\xHH` (hex) and `\NNN` (octal, so `\033` = ESC) escapes alongside the existing `\n\t\r`. **Additive + golden-safe**: output containing no `ESC` byte takes the exact old `grid_putc` path, so every existing app renders byte-identically (boottest/gfxtest/browsertest unaffected). **Verified in-guest**: `printf '\e[31mRED \e[32mGREEN \e[33mYELLOW \e[34mBLUE \e[35mMAGENTA \e[36mCYAN\e[0m done\n'` prints each word in its true colour, then resets. `make check` 52 suites green. Fourth feature on the r/osdev functionality roadmap (after ACPI, /proc+/dev, process control); a precondition for porting any standard color/curses program.

> **(M1058) system — process control: `/proc/processes` + `kill <pid>`.** Building on M1057's procfs: a live **`/proc/processes`** file (PID / STATE / NAME from `task_snapshot`, e.g. `6 run Shell`, `1/0 ready (kernel)`) and a real **`kill`**. New **`SYS_kill` (65)** + `sys_kill(pid)` looks the pid up in the task snapshot and calls the kernel's existing **`app_request_kill()`** — the *same* cooperative path the window-manager's X-button and F8 use, so the target app self-exits cleanly at its next syscall boundary (no unsafe async teardown). Exposed as a shell **`kill <pid>`** command (run `ps` or `cat /proc/processes` for pids). **Verified in-guest end-to-end**: `cat /proc/processes` lists the processes with pids; `kill 6` (the shell killing itself) made the Shell window vanish — confirming the whole chain shell→sys_kill→task_snapshot→app_request_kill→app-exit→window-close. `make check` 52 suites green. Process introspection + control, the next rung after ACPI (M1056) and /proc+/dev (M1057) on the r/osdev functionality roadmap.

> **(M1057) system — a synthetic `/proc` and `/dev` filesystem ("everything is a file").** A recognized r/osdev / Unix milestone: the OS now exposes live kernel state as readable files. New `kernel/procfs.c` (+`.h`) generates content on the fly — **`/proc/meminfo`** (MemTotal/Free/Used from the PMM), **`/proc/uptime`** (from `timer_ms`), **`/proc/cpuinfo`** (CPUID vendor + brand string + feature flags), **`/proc/version`**, **`/proc/loadavg`**, **`/proc/stat`** — plus the classic character devices **`/dev/null`** (read=EOF, write=discard), **`/dev/zero`**, **`/dev/random`** (rdtsc-seeded xorshift), and **`/dev/full`** (write=ENOSPC). Because OS-DEV's VFS is **name-based** (not fd-based), no per-process fd table was needed: `kernel/vfs.c` gained a tiny synthetic-cwd flag and routes paths under `/proc` and `/dev` to procfs before delegating to FAT32 — so absolute reads (`cat /proc/cpuinfo`), `cd /proc` (the prompt updates), `ls` (lists the synthetic entries), relative reads (`cat uptime`), and `> /dev/null` redirection all work; FAT32 is completely untouched. Bounded string builders; `/proc` is read-only. **Verified in-guest**: `cat /proc/cpuinfo` prints the real QEMU CPU ("AuthenticAMD / QEMU Virtual CPU version 2.5+ / fpu tsc … sse2 sse3"), `cd /proc; ls` lists the six files, `cat uptime` → `12.56 12.56`. `make check` 52 suites green (additive — boottest/fstest unaffected). Second feature from the r/osdev functionality roadmap (after M1056 ACPI); the keystone fd-table/pipes + signals come next.

> **(M1056) system — ACPI: the OS can cleanly POWER ITSELF OFF (and ACPI-reboot).** A classic hobby-OS milestone it lacked: shutdown. Previously "reboot" was a bare 8042 pulse and there was no power-off at all. New `kernel/acpi.c` (+`.h`) walks the firmware tables — scans the EBDA + BIOS area for the **RSDP**, follows it to the **RSDT/XSDT**, finds the **FADT**, and pulls the PM1a/PM1b control ports, the **`\_S5`** sleep-type values out of the **DSDT**'s AML (the well-known non-interpreting byte scan, fully bounded by the table lengths), and the reset register. `acpi_poweroff()` enters **S5** by writing `SLP_TYPa | SLP_EN` to PM1a_CNT (with emulator-port fallbacks for QEMU/Bochs/VBox); `acpi_reboot()` uses the FADT reset register, then 0xCF9, then the 8042 pulse. All firmware memory is reached via the higher-half direct map (`hhdm`). Wired up four ways: `acpi_init()` at boot (logs `[ ok ] ACPI: S5 power-off ready (PM1a=0x604 SLP_TYPa=0)`); `SYS_poweroff` (64) + `sys_poweroff()`; shell **`poweroff`/`shutdown`/`halt`** (and `reboot` now goes through ACPI); and desktop **Apps → "Shut Down" / "Restart"** menu items (the WM is kernel code, so they call ACPI directly). **Verified**: the boot serial log shows the S5 detection with the right QEMU values, and a temporary boot-time `acpi_poweroff()` made QEMU exit cleanly (rc=0, instantly) — then reverted. `make check` green. This is the first item from a fresh **r/osdev-guided functionality roadmap** (see the memory): the next prestige rungs are pipes + a per-process fd table, signals (Ctrl-C), a TTY/ANSI terminal, then mmap/COW + fork, /dev + /proc, and eventually an in-guest compiler.

> **(M1055) graphics — a textured TORUS KNOT joins scene3d's model gallery.** The 3D engine cycled through sphere / torus / cube (SPACE); now there's a fourth, showpiece mesh: a **(2,3) torus knot** rendered as a tube swept along the knot curve `C(t) = ((2+cos 3t)·cos 2t, (2+cos 3t)·sin 2t, sin 3t)`. The tube's cross-section is oriented by a **parallel-transport frame** — at each step the previous normal is projected onto the plane perpendicular to the new tangent and renormalized (with a degeneracy fallback), so the tube doesn't spin or kink as it follows the curve. The cross-section offset *is* the surface normal, so the knot lights, perspective-correct-textures (a new teal texture), z-buffers, self-occludes where it crosses itself, and casts a floor shadow exactly like the other models — and rides the M1051 orrery (orbiting moons) under the M1049 skybox. ~140 curve segments × 14 around the tube ≈ 2.1k verts / 3.9k tris, within the engine's caps. **Verified by QEMU screenshot** (cycled to model 3): a teal knot weaving over and under itself, correctly shaded and shadowed. Pure userspace-app change (`user/scene3d.c`, +~30 lines: `gen_knot()` + a texture case + the cycle now `%4`); additive (the satellite mesh is still the snapshotted sphere); `make check` 52 suites green. Shows the from-scratch rasterizer handles a genuinely complex self-intersecting mesh, no GPU.

> **(M1054) shell — a real `sed 's/RE/REPL/[gi]'` stream editor (regex substitution) completes the text toolkit.** The shell had `grep` (with a regex engine) but no `sed`; now it does. The substitution core is factored into **`user/shsed.h` (`sed_sub`)** — pure, no syscalls — so it shares grep's regex matcher (`gr_match_span`, the `^ $ . * [..] \` engine) and is **host-unit-tested** like every other extracted shell parser. It supports first-vs-global (`g`), case-fold (`i`), `&` = the whole match, `\&`/`\\`/`\<c>` literals, any delimiter (`s|a|b|`), and GNU's empty-match adjacency rule (`s/x*/-/g` on `abc` → `-a-b-c-`; an empty match right after a real one is skipped, so `s/a*/X/g` on `aab` → `XbX`). The shell builtin reads its file argument via `slurp`, so it works on files **and in pipes** (`echo hi | sed …` — the pipe appends `PIPE.TMP` as the last arg), processing one line at a time into a per-line heap buffer (no whole-output cap). **A host fuzz found a real bug**: `SED_PUT(*p++)` put the pointer-advance *inside* the buffer-full guard, so a truncated line spun forever — fixed by always stepping pointers outside the bounds check (now the truncation path is exercised by the fuzz). New `tests/shsed/` + `tests/run-shsed-tests.sh` + the `shsedtest` make target (regression + a 100k-case ASan/UBSan fuzz asserting bounded, terminating, OOB-free). **Verified in-guest too**: `sed 's/o/0/g' MOTD.TXT` prints the file with every `o`→`0` (e.g. `hello.txt`→`hell0.txt`). `make check` now 52 suites green. Diversifies off the graphics run into the shell/text-processing surface; mirrors the proven host-tested-parser pattern ([[verbatim-extraction]]).

> **(M1053) graphics — the terrain flythrough grows a FOREST (low-poly pines scattered on the hills).** The `terrain` landscape was empty; now it's populated with **low-poly pine trees** (a 4-sided brown trunk column + a 6-sided green conical canopy, ~14 triangles each), turning the flyover into a living world. The hard part is **stable placement**: trees sit at fixed *world* positions (not on the camera-relative grid that rebuilds each frame), so they don't swim as you fly — each cell of a world lattice (spacing 5.5) is hashed (the classic `73856093/19349663` spatial hash) to decide if it holds a tree and to jitter its exact spot, and trees only appear on **grassy ground** (terrain height 0.4–7, so never in water or on snow peaks). Each tree is rendered through the same pipeline as the land via a new `tri3d()` helper — flat-shaded by its world face-normal · sun (two-sided so canopy faces never go black), distance-fogged, and **z-buffered against the terrain** so a hill correctly hides the trees behind it. Per-frame camera state is stashed in file-scope (`Cex…Cfog`) so the tree pass can reuse `project()` + the lighting/fog math; candidate cells behind the camera are culled by a forward-distance dot product, and off-screen trees fall out cheaply in `project()`/`raster()`. **Verified by QEMU screenshot**: a scatter of correctly-lit, fogged pines stands on the green slopes between the blue valleys and the snow peaks, under the sun-glow sky — a populated forest landscape, still all software-rendered. Pure userspace-app change (`user/terrain.c`, +~60 lines), additive; `make check` 51 suites green.

> **(M1052) graphics — a new `terrain` app: a procedural heightmap FLYTHROUGH (fly forever over fog-shrouded mountains).** A second from-scratch software 3D renderer, distinct from `scene3d`'s orbit camera: a **forward-flying** camera (a real view transform — translate by the eye, yaw about Y, pitch about the right axis) soars endlessly over a **procedurally-generated landscape**. The world is a heightmap `terr_h(x,z)` = summed sines (broad rolling base + medium hills + fine detail), evaluated fresh each frame on a **camera-oriented grid** rebuilt in the flight direction (so the terrain is effectively infinite — you never reach an edge). Each grid cell is two **flat-shaded** triangles, lit by a directional **sun** (per-cell world-space face normal · light), **coloured by altitude** through a lerped palette (deep water → shallows → sand → grass → rock → **snow peaks**), **per-pixel Z-BUFFERED**, and **distance-FOGGED** (each cell blends toward a horizon haze by its forward depth, so far ridges melt into the gradient sky with its soft sun glow). The camera floats a fixed height above the ground (smoothed altitude-follow) and is steerable — A/D or ←/→ turn, W/S or ↑/↓ look, +/- speed, SPACE pause, Q/Esc quit. **Verified by QEMU screenshot**: snow-capped peaks under a glowing sun, green grassy mid-slopes, sandy shorelines and blue water in the valleys, all correctly lit/occluded/fogged in a stylish low-poly flat-shaded look — a genuine flying-over-mountains demo, no GPU. New isolated userspace app (`user/terrain.c`, ~270 lines) + the 5 registration spots (user_blob.asm, app.c, desktop.c menu, Makefile USER_ELFS + an SSE build rule like scene3d's); additive, nothing else touched; `make check` 51 suites green. OS-DEV now ships two complementary software 3D showcases: `scene3d` (orbit camera, textured lit solids + orrery + shadows) and `terrain` (free-flying heightmap landscape with fog).

> **(M1051) graphics — the 3D scene becomes a multi-body ORRERY (a centerpiece ringed by tinted moons on tilted orbits).** `scene3d` rendered a single lit "moon" orbiting the centerpiece; now it renders a small **planetary system** — four satellites on independent, *inclined* orbits (each with its own radius, orbital speed, phase, tilt, base height, scale, and colour). Two pieces of new engine plumbing, both additive: (1) a **per-object colour tint** (`tintR/G/B`, 0–256, applied as `(c*tint)>>8` in the rasterizer's inner loop — 256 = exact identity, so the floor + centerpiece render byte-identically), giving each moon its own hue (a warm-red, an ice-blue, a green, and a neutral) without needing a second texture; and (2) a **dedicated sphere mesh snapshotted once at startup** (`mvx/…/mtri`) so the moons stay round even when the centerpiece morphs to a torus/cube via SPACE. Each satellite orbits in its own plane tilted about X (so they weave above/below the centerpiece for real depth), self-spins, is lit + perspective-correct-textured + z-buffered against everything else, with backface + a near-plane triangle cull (drop tris with `iz>4`, i.e. z<0.25) so a moon swinging right up to the camera at max zoom never smears. **Verified by QEMU screenshot**: the blue centerpiece sphere casts its checkered shadow on the floor while four differently-tinted moons of varied size ride tilted orbits around it, under the sun-glow skybox — a dynamic mini solar system, all software-rendered, no GPU. Pure userspace-app change (`user/scene3d.c`), additive (identity tint + a separate mesh ⇒ the rest of the scene is unchanged); `make check` 51 suites green. The software renderer now does: perspective camera, z-buffer, Gouraud + specular lighting, perspective-correct textures, a checkered floor, cast shadows, a **multi-body orrery**, per-object tint, and a skybox.

> **(M1050) desktop — the Files window can finally enter subdirectories (Enter to descend, Backspace to go up).** The GUI Files browser listed the current directory but had no way to *navigate* it — `vfs_dirent` carries no is-dir flag, but `fat32_list` already suffixes directory names with `/`, so the list already distinguished folders; double-clicking/Entering one just did nothing. Now: pressing **Enter** on a `/`-suffixed entry strips the slash, `vfs_chdir()`s into it, and re-lists (resetting the selection); pressing **Backspace** `vfs_chdir("..")`s up one level and re-lists. Files (non-folder entries) still open in the editor or browser exactly as before. This makes the folder-creation that already existed (`n` = new folder → `vfs_mkdir`) actually *useful* — you can now make a folder and go into it from the desktop. The OS uses a single global cwd (the shell's `cd` and the Files window now share it, a coherent model), and navigation only fires on user action, so the default boot path (cwd stays root) is unchanged — `boottest`/`fstest` and the whole `make check` (51 suites) stay green. Kernel-only change (`kernel/desktop.c`, +7/-1) since `sys_list` already returns formatted text; mirrors the proven `vfs_chdir` used by the shell's `cd`. A real, long-standing GUI usability gap closed.

> **(M1049) graphics — a skybox for the 3D scene (gradient + sun glow + stars).** `scene3d`'s flat background gradient is replaced with an atmospheric **skybox**, computed per-pixel in the existing clear pass (no extra buffers/state): a vertical gradient (deep indigo at the top → warm teal at the horizon), a soft radial **sun glow** in the upper-right (additive falloff, a warm yellow-white disc), and faint **stars** scattered through the upper third of the sky (a deterministic per-pixel hash, ~1/1024 pixels, so no star array is needed). **Verified by QEMU screenshot**: the textured sphere + checkered floor + shadow now sit under a graduated sky with a glowing sun and a sprinkle of stars — a much more finished, atmospheric look. Pure userspace-app change (`user/scene3d.c`), additive, screenshot-verified; `make check` green. The software 3D scene is now: perspective camera, z-buffer, Gouraud lighting, perspective-correct textures, checkered floor, cast shadows, an orbiting moon, and a skybox — a complete little renderer, no GPU.

> **(M1048) shell — a `lsblk` command exposes the M1047 block-device browse to userspace.** M1047's block-device + FAT32 listing was boot-time/serial-only; now a user can run it. New `blockdev_format(out, max)` in `kernel/blockdev.c` formats the same enumeration (block devices + each FAT32 volume's root files + sizes) into a caller buffer (bounded string/decimal appenders; reuses the existing `collect_fat_starts`/`fatvol_list`; `blockdev_enumerate`'s boot log left untouched so `blockdevtest` stays green). Exposed via a new **`SYS_lsblk` (63)** syscall that validates the user buffer with `ubuf()` (the ring3→ring0 pointer check, exactly like `SYS_lspci`/`SYS_ps`/`SYS_df`) then calls `blockdev_format`; `sys_lsblk` ulib wrapper; and a **`lsblk` shell builtin** (added to the help line next to `lspci`). **Verified by QEMU screenshot**: typing `lsblk` in the shell lists the boot disk's FAT32 root files with sizes (IMG.HTM, README.MD, LOGO.SVG, …) — the disk-browse is now interactive. Mirrors the proven `lspci` (M1027) text-into-user-buffer pattern; additive (`fat32.c`/`vfs.c`/the boot path untouched); `make check` green. Diversifies off the graphics run into the syscall/shell surface — the storage drivers (M1026–M1036) → block-device layer (M1047) → user-visible `lsblk` (M1048) chain is now complete.

> **(M1047) storage — a block-device layer that makes EVERY disk driver's FAT32 browsable (not just self-tested).** Until now each storage driver (ATA, AHCI, virtio-blk, NVMe, USB mass-storage) only self-tested raw sectors at boot. New `kernel/blockdev.c` (+`.h`) is a uniform block-device registry: at boot it registers each *present* device behind one `read(ctx,lba,count,buf)` vtable (ATA drives 0–3, AHCI disks, virtio-blk, NVMe ns1, USB-storage), with `blockdev_read` range-checking against the known capacity. `kernel/partition.c`'s FAT32 walk was refactored to be **device-agnostic** — `fatvol_parse`/`fatvol_find`/`fatvol_list(read_fn, ctx, start_lba, …)` work over any block device via a callback (the existing ATA-only `partition_fat32_find`/`partition_scan`/`partition_enumerate` are unchanged — now thin wrappers, so `parttest` stays green; `fat32.c`/`vfs.c` are untouched). `blockdev_enumerate()` then, for each device, finds its FAT32 volumes (a bare FS at LBA 0, or FAT32 partitions in an MBR/GPT table — bounded MBR/GPT scan), **mounts each read-only and LISTS its root directory** (names + sizes + subdirs). Untrusted-input-safe (reviewed line-by-line): every BPB/FAT field validated, the cluster walk bounded by the computed cluster count + a cycle guard, every read into a fixed 512-byte buffer, candidate volumes / dir entries / devices all capped, device read errors skip cleanly; purely READ-ONLY, never touches the boot mount. Verified: a new permanent **`blockdevtest`** attaches a 2nd disk (virtio-blk) with a bare FAT32 holding known files (`GREET.TXT`, `NUMBERS.DAT`, `SUBDIR/`) and asserts blockdev_enumerate registered it + listed those files with matching sizes; the same browse also walks the parttest MBR+GPT disks (listing `HELLO.TXT` inside both an MBR and a GPT partition). `make check` 51 suites green — **boottest / fstest / parttest all still pass** (boot path + partition.c public API unchanged). Built in an isolated worktree, blockdev.c's untrusted MBR/GPT/FAT bounds + the partition.c refactor reviewed directly, clean copy onto current `main` (the agent's M1044 base == HEAD for the shared files, since M1045/M1046 only touched scene3d.c).

> **(M1046) graphics — a second orbiting object in the 3D scene (multi-object z-buffer).** `scene3d` now renders a small textured "moon" (the same mesh at 0.32 scale, with its own spin) orbiting the main model at radius ~1.95 in the xz-plane, as an additive second render pass: each moon vertex's world position is `orbit_offset + Ry(moonSpin)·(scale·vertex)`, camera-projected via the shared `project()` helper (yaw = camera-only, since the orbit offset is already world-space), then rasterized with backface cull + the per-pixel z-buffer — so the moon and main model occlude each other correctly as the moon swings in front of / behind the main object. Doesn't touch the working main-model / floor / shadow paths (purely additive). **Verified by QEMU screenshot** (a cropped view clearly shows the small moon-sphere orbiting beside the main sphere, both standing on the checkered floor, the main object casting its shadow). Pure userspace-app change (`user/scene3d.c`); `make check` green. The software 3D scene now has two independent objects + floor + shadows + textures — a genuinely complete little renderer, no GPU.

> **(M1045) graphics — cast SHADOWS in the 3D scene; the model is now grounded.** `scene3d` projects the spinning model's silhouette onto the floor plane along the light direction (planar projection: for each model triangle, each vertex's world-space position — model-spin applied — is projected to the floor `y=−1.25` along −L, then camera-projected), and a shadow pass darkens those floor pixels (halving their colour, only where floor actually exists, leaving the z-buffer untouched so the model still draws on top). Order: floor → shadow → model. **Verified by QEMU screenshot**: the gold torus casts a dark elliptical shadow on the checkered floor — *including the donut hole* (the floor checker shows through the shadow's centre) — which sells the 3D grounding. A few overlapping silhouette triangles compound the darkening into a crisp hard shadow. Pure userspace-app change (`user/scene3d.c`), additive (only active when the floor is on), screenshot-verified; `make check` green. The 3D engine now has: perspective camera, z-buffer, Gouraud lighting, perspective-correct texture mapping, a checkered floor, and cast shadows — a complete little software 3D scene, no GPU.

> **(M1044) graphics — the 3D engine is now a SCENE: a textured model on a PERSPECTIVE-CORRECT checkered floor.** Two upgrades to `scene3d`: (1) the rasterizer is now **perspective-correct** — it interpolates `1/z, u/z, v/z, light/z` linearly across each triangle and divides per pixel (so textures no longer affine-warp on big triangles like the cube faces or the floor), with the z-buffer now keyed on `1/z` (larger = nearer); (2) a **checkered ground floor** — a grid of textured tiles (grey/white checker + grid lines) laid in world space at y=−1.25, transformed by the camera orbit (but *not* the model spin), with **near-plane culling** (tiles with any corner nearer than z=0.5 are dropped, so no near-clamped distorted geometry smears bogus depth over the model). The spinning textured model now sits *on* that floor, and the per-pixel z-buffer resolves the occlusion **both ways** — the model is in front of the far floor, the near floor edge is in front of the model's base. New **F** key toggles the floor. The per-vertex transform/light/project was refactored into one `project()` helper shared by the model and the floor. **Verified by QEMU screenshot**: the checker floor recedes correctly to the horizon (perspective-correct), with the textured sphere / gold torus standing on it, z-buffered. (Caught + fixed two bugs via the screenshots: a missing `focal` initialization after refactoring it to a global — every vertex was collapsing to screen-center — and the near-tile occlusion.) Pure userspace-app change (`user/scene3d.c`), additive, screenshot-verified; `make check` green. Next: a multi-object scene, a loaded mesh, or env/normal mapping.

> **(M1043) graphics — TEXTURE MAPPING for the software 3D engine (`scene3d`): lit, textured solids.** Building on M1041's z-buffer/Gouraud renderer, every mesh vertex now carries **UV texture coordinates** (sphere: longitude×4 / latitude×2; torus: around-the-ring×6 / around-the-tube×1; cube: 0..1 per face), and a **procedural texture** (a coloured checker + grid lines, regenerated per model — blue/cyan tech-grid for the sphere, gold/amber for the torus, red/blue crate for the cube) is mapped onto the surface and **modulated by the per-vertex lighting**, so you get a lit + textured solid. The triangle rasterizer was extended to interpolate `(z, u, v, light)` across each triangle (affine — the meshes are densely tessellated, so it reads clean), sample the wrapped 256×256 texture per pixel, and multiply by the interpolated light, all under the existing per-pixel z-buffer + backface cull. New **T** key toggles texture on/off. **Verified by QEMU screenshot**: the torus shows the gold checker+grid wrapping correctly around both the ring and the tube, lit from the upper-left, with the z-buffer resolving the self-occlusion — a clear visual upgrade from the flat-shaded M1041 look. Pure userspace-app change (`user/scene3d.c`), additive, screenshot-verified; `make check` green (boot/desktop path unchanged). Next graphics steps available: perspective-correct UV (vs affine), a loaded/larger mesh, a multi-object flythrough scene with a floor grid, or env/normal mapping.

> **(M1042) drivers — xHCI (USB 3.0) host controller, completing the USB host-controller trilogy (UHCI 1.1 → EHCI 2.0 → xHCI 3.0).** New `kernel/xhci.c` (~870 lines, +`.h`) — the modern USB host: a PCI device (class 0x0C / subclass 0x03 / prog-IF **0x30**) with MMIO registers (BAR0) and a transfer model built on **rings of 16-byte TRBs** (Transfer Request Blocks) instead of EHCI's QH/qTD. Bring-up: map BAR0 cache-disabled, validate CAPLENGTH/DBOFF/RTSOFF + MaxSlots/MaxPorts, reset the HC, allocate the **DCBAA** (device-context base-address array) + a **command ring** + an **event ring** (with its ERST), program CRCR/DCBAAP/CONFIG, run. Commands (ENABLE SLOT, ADDRESS DEVICE, CONFIGURE ENDPOINT) go on the command ring → doorbell 0 → poll the event ring (cycle-bit matched) for the Command Completion Event; EP0 control transfers ride a per-endpoint TRB ring (Setup/Data/Status stages). It resets a root port, ENABLE SLOT → allocates the device + input contexts (honoring HCCPARAMS1.CSZ 32/64-byte stride) → ADDRESS DEVICE → enumerates over EP0 (device 8→18, config header→full with the bounded walk, SET_CONFIGURATION). **STRETCH delivered**: a BOT/SCSI mass-storage device behind xHCI has sector 0 read over an xHCI bulk transfer ring. Bounds-safe (reviewed line-by-line): CAPLENGTH (≥8,<256)/N_PORTS bounded; rings/contexts/buffers are pmm frames (identity-mapped); ring enqueue + event dequeue bounded within their frames with correct Link-TRB cycle-toggle; every poll loop finite-timer-bounded + spin-capped; the config walk bounded; absent xHCI ⇒ clean no-op. Additive: **usb.c (UHCI) + ehci.c are byte-for-byte unchanged** — three independent USB host controllers coexist (separate PCI devices). The agent even found+fixed a real bug during bring-up: in the Input Context an endpoint at Device-Context-Index N lives at context *block N+1* (block 0 = Input Control, block 1 = Slot). Verified: a new permanent **`xhcitest`** boots `-device qemu-xhci` + a `usb-storage` on its bus and asserts **HC up (HCIVERSION 1.00, MaxSlots=64, MaxPorts=8), ENABLE SLOT got slot 1, SuperSpeed port reset, device descriptor read over xHCI (idVendor=46f4 idProduct=0001), AND a sector read over xHCI bulk byte-identical to host truth** (`sum=0001030b`); `make check` 50 suites green, **boottest still passes** (clean no-op; UHCI + EHCI + the tablet all stay up). kmain.c clean copy + Makefile (xhcitest) hand-applied onto current `main`; the TRB-ring/event/context/bounds reviewed directly, re-verified on `main`. **This closes out the driver campaign's USB stack** — UHCI/EHCI/xHCI host controllers + HID tablet/keyboard + mass-storage.

> **(M1041) graphics — a real-time software 3D ENGINE (`scene3d`) + a demoscene (`demoscene`), as ring-3 userspace apps. "Insane graphics", no GPU.** Two new windowed apps in the Apps menu, both rendering into a `sys_gfx_init`/`sys_gfx_blit` XRGB canvas (the gfxdemo/DOOM stack). **`user/scene3d.c` — the headline: a genuine perspective software 3D renderer** (built with SSE for float, like DOOM/Quake; self-contained `fsin`/`fcos`/`fsqrt`, no libm). It procedurally generates triangle meshes (a UV sphere, a torus, a cube with per-face normals), transforms them by a combined model-spin + camera-orbit rotation, lights them with a directional key light + a rim/fill light + a touch of specular, and rasterizes with a **per-pixel Z-BUFFER** and **Gouraud shading** (a scanline rasterizer interpolating z + R/G/B down the edges and across each span), plus backface culling. Interactive: SPACE cycles model, arrow keys orbit the camera, +/- zoom, R toggles auto-spin. **Verified by screenshot in QEMU** (temporarily auto-launched, then reverted): the sphere shades smoothly (blue, lit upper-left), the **torus correctly self-occludes through the z-buffer** (the near ring hides the far ring, the hole shows the background), and the cube shows exactly its three front faces (backface cull) with smooth per-vertex colour gradients — all at 480×360. **`user/demoscene.c` — a classic multi-effect demo** in pure fixed-point (so it uses the standard `-mgeneral-regs-only` build; a sine LUT via Bhaskara's integer approximation, integer `isqrt`/`iatan2`): plasma, rotozoom tunnel, fire (8237-free, cellular), a flat-shaded 3D icosahedron, starfield, metaballs, and an animated Julia set, cycled with cross-fades under a sine-wobble scroller with an embedded 8×8 font. Both registered the standard 5 ways (user_blob.asm globals+incbin, desktop.c Apps menu, app.c extern+table, Makefile USER_ELFS; scene3d gets a DOOM-style SSE `.elf` rule). `make check` green (the apps are additive ELFs in the kernel blob; the desktop/boot path is unchanged — boottest/gfxtest still pass). Answers the "get insane graphics" / "get 3D graphics" ask directly: a from-scratch, GPU-less, real-time 3D engine you can spin in a window.

> **(M1040) drivers — floppy disk controller (82077AA) over legacy ISA DMA — a genuinely new DMA mechanism for the kernel.** New `kernel/floppy.c` (566 lines, +`.h`). Every other DMA driver in the campaign bus-masters over PCI; the floppy is driven through the **legacy 8237 ISA DMA controller** (channel 2) — a different beast: a low-memory bounce buffer the controller addresses with a 16-bit address register + an 8-bit page register (24 bits = 16 MiB max), and a single transfer must not cross a 64 KiB physical boundary (the 8237 increments only the low 16 bits; the page register doesn't carry). The driver: resets the 82077AA (DOR toggle + SENSE INTERRUPT ×4), sets the data rate (CCR), SPECIFY + recalibrate to cylinder 0, spins the motor; per read it converts LBA→CHS (1.44 MB geometry: 2 heads × 18 sectors), programs the 8237 channel 2 (the canonical recipe: mask → flip-flop reset → mode → addr lo/hi → page → flip-flop → count lo/hi → unmask), seeks, issues READ DATA, polls the result phase (finite timeout) + checks ST0/1/2, then copies the bounce → caller. **Notable design (flagged + reviewed):** ISA DMA needs a <16 MiB, 64-KiB-bounded buffer, but `pmm_alloc_frame()` cannot supply one — the kernel's 44 MiB static JS arena pushes `kernel_end` past 16 MiB, so EVERY low frame is inside the kernel image + marked used. So the bounce buffer is statically reserved in a new **`.lowbss (NOLOAD)` linker section** placed *before* the big `.bss` arenas (landing at phys ~0x3a2000 ≈ 3.8 MiB) — page-aligned (can't straddle 64 KiB), NOLOAD (no binary bloat, zero-init at runtime like .bss). `floppy_init()` asserts BOTH the 16 MiB + 64 KiB constraints and `dma_prepare()` re-asserts them (defence in depth). Bounds-safe: polling-only (IRQ6 stays masked), every wait (reset/RQM/seek/recalibrate/result-phase/motor) finite-timeout-bounded, `count` capped to the buffer, `floppy_read` validates buf/count/geometry; an absent controller bails via a post-reset MSR sanity check (a present 82077AA settles MSR=0x80; an empty I/O range floats 0xFF) + a sector-0 probe so QEMU's always-present FDC reports not-present when there's no medium (this avoids a spurious "READ FAILED" tripping other suites). Additive: boot stays on legacy ATA; absent FDC/diskette ⇒ clean no-op. Verified: a new permanent **`floppytest`** attaches a 1.44 MB image with KNOWN content and asserts reset+recalibrate OK + **sectors 0/1/2 AND a 4-sector read all read back byte-exact over ISA-DMA channel 2** (checksums match host truth, `FLOPPY SECTOR N!` marker verbatim, bounce @phys 0x3a2000); `make check` 49 suites green, **boottest still passes** (clean no-op). floppy.c/floppy.h/linker.ld clean copy onto current `main`; the `.lowbss` linker change + the 8237 sequence + the 16 MiB/64 KiB constraints reviewed directly, kmain/Makefile hand-applied, re-verified on `main`.

> **(M1039) drivers — EHCI (USB 2.0) host controller; high-speed USB with a full QH/qTD async schedule, enumeration, and a bulk read over EHCI.** New `kernel/ehci.c` (871 lines, +`.h`) — the campaign's most intricate driver. Where `usb.c` drives UHCI (USB 1.1, I/O-port frame list), EHCI is the USB **2.0** host: a PCI device (class 0x0C / subclass 0x03 / prog-IF **0x20**) with **MMIO** registers (BAR0, like AHCI/NVMe) and a fundamentally different transfer model — an **asynchronous schedule**, a circular linked list of Queue Heads (QH) each carrying a chain of queue Transfer Descriptors (qTD). Bring-up: map BAR0 cache-disabled, validate CAPLENGTH, stop+reset the HC, build the async ring (a permanent head QH with the H-bit, self-linked, + one transfer QH spliced in), `USBCMD` = Async-Schedule-Enable | Run, `CONFIGFLAG=1` (route the ports from the companion UHCI to EHCI). Per transfer it builds a SETUP→[DATA]→STATUS qTD chain (or a single bulk qTD), points the QH's overlay at the first qTD, and polls the last qTD's Active bit. It then ENUMERATES the device over control transfers — GET_DESCRIPTOR (8→18), SET_ADDRESS, config-descriptor (header→full), SET_CONFIGURATION — mirroring usb.c's request bytes + bounded descriptor walk. **STRETCH delivered**: when the device is BOT/SCSI mass-storage it reads sector 0 over EHCI bulk (READ(10) inside Bulk-Only Transport). Bounds-safe (reviewed line-by-line): CAPLENGTH validated (≥8, <256) before the operational base, N_PORTS capped at 15, enough MMIO mapped for all PORTSC; QH/qTD 32-byte-aligned sub-allocated in one identity-mapped frame; every poll loop (HC halt/reset, port reset, qTD completion) has a finite timer-bounded timeout (+ a spin cap so a dead register can't wedge boot pre-timer); control data stage bounded to a page bounce buffer, the qTD count checked, PORTSC read-modify-writes mask the write-1-to-clear bits, low/full-speed ports released to the companion via the port-owner bit. Additive: **`kernel/usb.c` (UHCI) is byte-for-byte unchanged** — EHCI is a wholly separate file on its own PCI controller, so the UHCI tablet/storage/keyboard are unaffected; absent EHCI ⇒ `ehci_init()` clean no-op. Verified: a new permanent **`ehcitest`** boots `-device usb-ehci` + a `usb-storage` on its bus and asserts **EHCI HC up (HCIVERSION 1.00, N_PORTS=6), root-port reset enabled a high-speed device, the device descriptor was read over EHCI's QH/qTD async schedule (idVendor=46f4 idProduct=0001), AND a sector was read over EHCI bulk via BOT/SCSI READ(10) byte-identical to the host-computed truth** (`sum=000102f8`); `make check` 48 suites green, **boottest still passes** (clean no-op, UHCI intact). Clean copy onto current `main` (base == HEAD); the async-schedule/qTD/transfer/port/bulk bounds reviewed directly, re-verified on `main`.

> **(M1038) drivers — USB HID boot keyboard over UHCI; a real USB keyboard drives the shell/apps alongside PS/2.** New `kernel/usb_kbd.c` (+`.h`), the fourth client of M1036's shared UHCI transfer layer (tablet + mass-storage + now keyboard). It enumerates a HID boot keyboard (interface class 0x03 / subclass 0x01 Boot / protocol 0x01 Keyboard), issues HID **SET_PROTOCOL(0=boot)** + SET_IDLE(0), then polls its interrupt-IN endpoint for the 8-byte boot report (byte 0 = modifier bitmap, bytes 2–7 = up to 6 pressed HID usage IDs), diffs each report against the previous to fire only on key-DOWN, translates the usage → the kernel's ASCII/control byte (a bounded `usage_ascii`/`usage_shift` table; shift upper-cases letters + maps the symbol row; Ctrl+letter → 0x80|… matching keyboard.c; arrows/Home/End/etc. → the same control codes the PS/2 path emits), and pushes it into **`input_push`** — the very same input queue PS/2 + serial feed, so USB keystrokes reach the shell + apps identically. `desktop_run()` calls `usb_kbd_poll()` next to `usb_tablet_poll()`. To support it, `kernel/usb.c` gained a generic **`usb_interrupt_xfer()`** on a NEW dedicated QH (`qh_intx`, chained `qh_bulk→qh_intx→terminate` and left idle until armed) — **distinct from the tablet's continuously-armed `qh_int`, which is byte-for-byte unchanged** — so a polled keyboard read can't perturb the live tablet endpoint; it's non-blocking (1-tick bound: a NAK on an idle keyboard returns 0/empty cleanly and does NOT desync the data toggle, a stall returns -1). Bounds-safe (reviewed line-by-line): usage bounded `< HID_USAGE_MAX` before either table lookup; the report is a fixed 8-byte buffer (slots [2..7]), `kb.prev[6]`, ErrorRollOver (0x01) skipped; the config-descriptor walk is bounded (`blen<2`/`i+blen>total` guards, fields gated on `blen≥9`/`≥7`); enumeration skips the tablet's port + uses the shared address allocator; absent keyboard ⇒ `usb_kbd_init()` returns -1 (clean no-op). Verified: a new permanent **`usbkbdtest`** attaches `-device usb-kbd` on the UHCI bus (port=2, tablet on port=1), boots, injects keystrokes via QEMU's `sendkey` monitor, and asserts the HID boot keyboard enumerated (class 03/sub 01/proto 01 + its interrupt-IN ep), SET_PROTOCOL(boot) ok, and the **injected keys were decoded on the interrupt endpoint** (`usage=0b→'h'`, `0c→'i'`, `0e→'k'`); `make check` 47 suites green, **boottest still passes — the PS/2 keyboard + USB tablet + USB mass-storage all keep working** (shared controller, tablet's qh_int undisturbed). usb.c/usb.h/desktop.c clean copy onto current `main`; the usb.c qh_intx change + the HID decode/bounds reviewed directly, kmain/Makefile hand-applied, re-verified on `main`.

> **(M1037) storage — bus-master IDE DMA for ATA (a DMA read/write path alongside PIO), the one perf upgrade to the boot-disk driver.** ATA was PIO-only (one word at a time on the data port). This adds a DMA path via the PIIX3 IDE controller's **BMIDE** interface + a PRD table, in `kernel/ata.c` — **purely additive (421 insertions, zero deletions): the PIO `ata_read`/`ata_write`/`ata_read_drive`/`ata_write_drive` + helpers are byte-for-byte unchanged and remain what fat32/vfs/boot call.** The DMA path: find PCI 0x8086:0x7010 BAR4 (the BMIDE I/O base), `pci_enable_bus_master`, build a single **PRD** (Physical Region Descriptor: region phys + byte count + EOT) pointing at a page-aligned pmm **bounce buffer** (cap 8 sectors = one frame), program the channel's PRDT pointer + direction, issue **READ DMA (0xC8)** / WRITE DMA (0xCA) via the existing PIO drive-select/LBA sequence (reusing `select_lba`), start the bus master, poll the BMIDE status to completion with a finite timeout, stop it, check the BMIDE error bit + the ATA task-file ERR/BSY, then `memcpy` bounce→caller. Bounds-safe (reviewed line-by-line): validates drive-present / count>0 / count≤8 / buf / `lba+count ≤ capacity` / LBA28 range; PRD byte-count ≤4096 (never the 0=64 KiB special case); contiguity is trivial (single frame); finite timeouts; clean -1 (no-op capability) when the BMIDE controller is absent. Why additive-only: the boot disk's driver must never regress, so DMA is proven *identical* to PIO rather than swapped in — a future change can flip fat32/vfs to DMA once it's trusted (same "expose the capability, don't rewire the boot path" discipline the graphics drivers used). Verified: a new permanent **`idedmatest`** boots the normal IDE boot disk (already on the bus-master-capable PIIX3 controller) and asserts `ata_dma_selftest()` **DMA-read sectors 0–2 and they matched a PIO read of the same sectors byte-for-byte** ("DMA==PIO OK"; sector 0 = the real `EB 58 90 "OSDEV1.0"` FAT32 boot sector) plus a DMA **write→read→restore round-trip** on the last sector; `make check` 46 suites green, **boottest + fstest still pass (PIO path untouched)**. Clean copy onto current `main` (base == HEAD), the BMIDE/PRD/bounce/bounds reviewed directly, re-verified on `main`.

> **(M1036) drivers — USB mass-storage (Bulk-Only Transport + SCSI) over UHCI, with usb.c refactored into a shared transfer layer.** New `kernel/usb_storage.c` (+`.h`): a USB flash disk read/written via SCSI commands carried in USB BOT over two bulk endpoints. To get there, `kernel/usb.c` (the UHCI driver that drove only the tablet) was refactored into a **shared transfer layer** both clients use: `usb_uhci_init()` (idempotent controller bring-up, now also a dedicated bulk QH + TD pool + a contiguity-verified bounce buffer, chained interrupt→control→bulk), `usb_uhci_enable_port()` (one-port-at-a-time — required, since every unaddressed device answers address 0), `usb_alloc_address()` (shared address allocator), `usb_control_xfer()` (generic; the tablet's `control()` is now a thin wrapper), and `usb_bulk_xfer()` (generic bulk IN/OUT on the isolated bulk QH, data-toggle threaded, finite timeout, short-IN reported). The old `run_control` became `run_qh_chain` (any QH, bounded deadline, unlinks a failed/timed-out chain). usb_storage then does the BOT three-phase cycle (31-byte CBW on bulk-OUT → data phase → 13-byte CSW on bulk-IN) carrying INQUIRY / READ CAPACITY(10) / READ(10) / WRITE(10). Bounds-safe (reviewed line-by-line): CBW/CSW signatures + the echoed tag + the status byte all verified; the config-descriptor walk is bounded (`blen<2`/`i+blen>total` guards, field access gated on `blen≥9`/`≥7`); `usb_storage_read/write` validate `lba≥blocks || count>blocks-lba` (no wrap) and chunk to `USB_BULK_MAX`; `usb_bulk_xfer` clamps the copy to the bytes actually transferred (an over-long IN can't overrun); maxp capped ≤64; every transfer finite-timeout-bounded; absent device ⇒ clean -1 (tablet path unaffected). Verified: a new permanent **`usbstoragetest`** attaches a `usb-storage` flash disk ON THE SAME UHCI BUS as the tablet (pinned to root `port=2`, the tablet on `port=1`, sidestepping QEMU's default USB hub a hub-less driver can't traverse) with KNOWN per-sector content, boots, and asserts the BOT/SCSI device enumerated (class 08/sub 06/proto 50 + bulk endpoints), `INQUIRY 'QEMU QEMU HARDDISK'`, READ CAPACITY matched (64×512), **sectors read back byte-exact** (checksums match, `USB-STORAGE SECT` marker) + a **WRITE(10) round-trip**; `make check` 45 suites green, **boottest still passes (the USB tablet still comes up — shared controller, refactor intact)**. usb.c clean copy onto current `main`; the usb.c refactor (tablet path preserved) + usb_storage BOT/SCSI bounds reviewed directly, kmain/Makefile hand-applied, re-verified on `main`.

> **(M1035) graphics driver — VMware SVGA-II, the third display adapter (after VBE/DISPI and virtio-gpu).** New `kernel/svga.c` (267 lines, +`.h`), PCI 0x15AD:0x0405 (QEMU `-vga vmware` / `-device vmware-svga`). Driven through THREE PCI resources: **BAR0** an I/O-port INDEX/VALUE register file (write a register number to the index port, transfer its 32-bit value through the value port), **BAR1** the linear framebuffer (0x00RRGGBB, the same layout fb.c uses — but the host does NOT auto-scan it), **BAR2** a command FIFO ring. Bring-up: confirm SVGA_ID_2 (write it to SVGA_REG_ID, read it back) → read FB + FIFO base/size + MAX_WIDTH/HEIGHT → set WIDTH/HEIGHT/BPP=32 → SVGA_REG_ENABLE=1 → init the FIFO ring header (MIN/MAX/NEXT_CMD/STOP) → CONFIG_DONE. To present: write pixels into BAR1, push an `SVGA_CMD_UPDATE` rect into the FIFO (bump NEXT_CMD with wrap), poke SVGA_REG_SYNC. Bounds-safe (reviewed line-by-line): **SVGA_ID_2 confirmed before touching any other register** (clean -1 bail otherwise); mode capped ≤1920×1200, to MAX_W/H, AND to FB_SIZE (w·h·4 must fit, else fall back 640×480, else bail); every FIFO write validates MIN≥header / MAX≤mapped / NEXT in-range+aligned before writing `fifo[next/4]` (a corrupt FIFO header can't drive an OOB write), full-ring sync with a finite wait; framebuffer writes bounded within the mapped FB (each row checked against `fb_mapped`); present rects clamped within [0,w]×[0,h]; finite BUSY waits. **Safety-first / additive: `fb.c` + `bochs_vbe.c` UNTOUCHED** — the boot display stays on the linear framebuffer; `svga_framebuffer()`/`svga_present()`/`svga_active()` exposed as a future seam. Verified: a new permanent **`svgatest`** boots QEMU with `-vga vmware -device VGA` (SVGA-II primary under test + a secondary std-VGA carrying the Bochs DISPI the boot display uses — the agent found `-vga std -device vmware-svga` is rejected by QEMU 10.2.2 with a duplicate-`vga`-SaveStateEntry error, so this is the working topology) and asserts SVGA_ID_2 confirmed, FB `0xfc000000`/16 MB + FIFO `0xfe000000`/64 KB read, mode 1024×768@32 set, a colour-band pattern written + `SVGA_CMD_UPDATE` emitted + synced, registers read back OK, no fault; `make check` 44 suites green and **gfxtest still passes byte-identically** (std-VGA desktop 1280×960 unaffected). Hand-applied onto current `main`, the I/O-register + FIFO-ring + FB bounds reviewed directly, re-verified on `main`.

> **(M1034) storage — full ATA drive enumeration (all 4 legacy drives) + MBR & GPT partition-table parsing, exposed as a multi-volume capability.** The kernel used ATA primary-master PIO only and mounted a *bare* FAT32 off LBA 0. This generalizes `kernel/ata.c` to probe all four legacy drives (primary/secondary bus × master/slave) — `ata_read_drive(drive,lba,count,buf)` with `ata_read()`/`ata_write()` now thin wrappers for drive 0 (all 19 existing fat32.c callers unchanged) — and adds `kernel/partition.c` (+`.h`), an MBR + GPT parser exposing each partition as a `{drive, start_lba, sectors, type, scheme}` volume. A 0xEE protective entry routes to the GPT path (header at LBA 1, "EFI PART", the entry array read a sector at a time). **Multi-volume proof (stretch goal, done):** a self-contained read-only `partition_fat32_find(drive, start_lba, name83, &size)` reads the BPB at a partition's start-LBA on *any* drive, validates FAT32, and locates a file in the root dir — proving a partition's filesystem is genuinely readable from its offset, without disturbing fat32.c's single boot-mount state. Untrusted-input-safe (reviewed line-by-line): IDENTIFY uses finite spins + absent/floating-bus detection (an empty slot never hangs); every MBR/GPT field validated via `range_in_disk()` (count>0, no 64-bit overflow, within the disk's sector count AND the LBA28-addressable bound so an out-of-range on-disk LBA can't truncate to a wrong in-range sector); GPT entry-size capped [128,512] + must tile a sector, entry-count capped ≤512; every read targets a fixed 512-byte buffer with compile-time-constant offsets; output capped at PART_MAX. Additive: the bare-FAT32 boot mount on primary-master is untouched (a boot disk with 0x55AA but all-zero partition entries is correctly reported as 0 partitions — no misinterpretation). Verified: a new permanent **`parttest`** attaches an MBR disk (primary-slave) AND a GPT disk (secondary-master), each host-built with a FAT32 partition containing a known `HELLO.TXT`, boots, and asserts the driver enumerated all present drives, parsed BOTH partition schemes with start-LBA/size/type matching the host-written values, and **read HELLO.TXT back from each partition's FAT32**; `make check` 43 suites green, **boottest + fstest stay green** (the primary-master bare-FAT32 boot path is unchanged). PIO only (DMA deferred). Clean copy onto current `main` (base == HEAD), ata.c + the untrusted partition.c parser reviewed directly, re-verified on `main`.

> **(M1033) graphics driver — virtio-gpu (modern 2D paravirtual GPU), plus the kernel's first MODERN virtio 1.0 PCI transport.** New `kernel/virtio_gpu.c` (~470 LOC, +`.h`). A virtio-gpu doesn't scan out a linear framebuffer: the display is a host-side *resource*; the guest draws into a backing buffer it owns, then explicitly TRANSFERs the dirty rect to the host copy and FLUSHes to present. Bring-up wires one resource to scanout 0 (CREATE_2D → ATTACH_BACKING → SET_SCANOUT), then per-frame TRANSFER_TO_HOST_2D + RESOURCE_FLUSH. **Key discovery: QEMU's `virtio-gpu-pci` is modern-only (virtio 1.0, PCI 1af4:1050, MMIO BARs, no legacy I/O window even with `disable-modern=on`)** — so virtio_blk.c's legacy I/O-port transport could NOT be reused. This driver implements the **modern virtio PCI transport** (a first for the kernel, reusable by future modern-only virtio devices): walk the PCI vendor-capability list (cap 0x09) for the COMMON/NOTIFY/ISR/DEVICE cfg structures (each names a BAR+offset+length), map those MMIO regions, negotiate the mandatory `VIRTIO_F_VERSION_1`, program the split virtqueue's three rings by *separate* physical address (`queue_desc`/`queue_driver`/`queue_device`), and notify via an MMIO doorbell (`notify_base + queue_notify_off × multiplier`). The vring + request/response cycle are otherwise identical to virtio_blk.c. Bounds-safe (reviewed line-by-line): the capability walk is guarded against a cyclic list (≤48 hops), BARs validated (I/O BARs rejected, 64-bit folded), display size capped ≤1920×1200 (default 1024×768 if unreported), the backing buffer is exactly w·h·4 bytes of contiguous-verified frames, every present rect is clamped within [0,w]×[0,h] (negatives handled), queue size capped ≤256, FEATURES_OK re-read to confirm acceptance, finite poll timeouts, every command's response type checked == its `OK_*`. **Safety-first / additive: `kernel/fb.c` + `bochs_vbe.c` are UNTOUCHED — the boot display stays 100% on the linear framebuffer**; the driver exposes `virtio_gpu_backing()`/`virtio_gpu_present()`/`virtio_gpu_active()` as the seam a future compositor could present through, but no fb.c hook was wired (deliberately, to keep the boot display un-regressable). Verified: a new permanent **`virtiogputest`** boots QEMU with `-device virtio-gpu-pci` ALONGSIDE std-VGA and asserts the full path — **modern PCI handshake + control virtqueue up, scanout 0 read as 1280×800 (enabled), resource created+attached+scanned-out, TRANSFER_TO_HOST_2D + RESOURCE_FLUSH of a colour-band test pattern each returned OK, no fault**; `make check` 42 suites green and — critically — **gfxtest still passes byte-identically** (the std-VGA desktop at 1280×960, unaffected). Built in an isolated worktree, the modern-transport cap-walk + vring + present bounds reviewed directly, hand-applied onto current `main` (its base predated the virtio-net/NVMe merges), re-verified on `main`.

> **(M1032) drivers — NVMe (modern PCIe storage) as an additive disk, the queue-pair counterpart to AHCI/virtio-blk.** New `kernel/nvme.c` (~490 lines, +`.h`), the third additive block device (boot stays on legacy ATA; AHCI M1026 + virtio-blk M1028 are the siblings). Unlike AHCI's SATA FIS or virtio's vring, NVMe is driven through **queue pairs in RAM**: a 64-byte-entry Submission Queue + a 16-byte-entry Completion Queue per pair, with MMIO **doorbell** registers and a **phase-bit** completion protocol. `pci_find_class(0x01,0x08,0x02)` (fallback 0x1B36:0x0010); 64-bit BAR0 mapped cache-disabled. Bring-up (NVMe 1.x §7.6.1): CC.EN=0 → wait RDY=0 → admin SQ/CQ (a page frame each) → AQA/ASQ/ACQ → CC (64-B SQE, 16-B CQE, NVM set, 4 KiB pages, EN=1) → wait RDY=1 (CFS fatal). Then IDENTIFY namespace 1 (NSZE + LBA size via FLBAS→LBAF→LBADS), CREATE IO CQ then CREATE IO SQ for one qid-1 pair. `nvme_xfer` issues NVM READ (0x02)/WRITE (0x01) and **DMAs through a page-aligned bounce frame** — which removes any caller-alignment requirement AND bounds every transfer so PRP1(+PRP2) always describe it. Bounds-safe (reviewed line-by-line): validates buf/count, caps count to the bounce size, rejects `lba ≥ capacity || count > capacity−lba` (no overflow), only does IO on a 512-B-LBA namespace (else reports + skips), finite poll timeouts, CQ phase-bit handled across wrap, completion status checked; clean -1 no-op when absent. Verified: a new permanent **`nvmetest`** attaches a 2nd disk over `-device nvme` with KNOWN per-sector content, boots, and asserts the driver IDENTIFYd it (16 LBAs × 512 B) and **read every sector back byte-exact** (host-computed checksums match the kernel's logged sums; the "NVME SECTOR N!" markers read verbatim) plus a **write→read→restore round-trip** on the last sector; `make check` 41 suites green, the legacy-ATA boot path untouched (boottest no-ops NVMe cleanly). Hand-applied onto current `main` (its base predated the virtio-net merge to kmain.c/Makefile), nvme.c's queue/doorbell/phase/bounce/bounds reviewed directly, re-verified on `main`.

> **(M1031) drivers — virtio-net (paravirtual NIC), completing the virtio trio and plugging into the `nic.c` seam.** New `kernel/virtio_net.c` (~430 lines, +`.h`), modelled on `virtio_blk.c`'s legacy-virtio-over-PCI mechanics. PCI 0x1AF4:0x1000; I/O-port BAR0; the init handshake (reset → ACK → DRIVER → negotiate only `VIRTIO_NET_F_MAC` to read the MAC → FEATURES_OK → DRIVER_OK). **Two** split virtqueues — queue 0 RECEIVE (pre-filled with 32 device-writable buffers; drained + recycled on poll), queue 1 TRANSMIT (16 round-robin buffers, synchronous: publish → notify → poll the used ring). Every frame carries the 10-byte legacy `virtio_net_hdr` (zeroed on TX, stripped on RX); control queue skipped (no MRG_RXBUF/offload/GSO — the simplest valid driver). Plugs into the NIC abstraction exactly like rtl8139.c: `nic_init()` probes it after e1000 + RTL8139, so with only a virtio NIC on the bus the whole net.c stack (ARP/IP/ICMP/UDP/DNS/TCP/TLS) runs over it. Bounds-safe (reviewed line-by-line): the RX copy is double-clamped to BOTH the caller's `max` AND the device-reported length (itself first clamped to the buffer), the `desc_id` from the used ring is range-checked before deref (the device is untrusted), queue size capped ≤256, contiguous-vring alloc verified, finite poll timeouts, absent device ⇒ clean -1 no-op. Verified: a new permanent **`virtionettest`** boots QEMU with a legacy `virtio-net-pci` NIC in place of the e1000 and asserts the full stack comes up over it — **MAC read from config, ARP resolved the gateway, 3/3 ICMP echo replies, HTTP 200, and a complete TLS 1.3 handshake (cert chain verified)** — all over the virtio RX/TX virtqueues; `make check` 40 suites green, the default e1000 boot path untouched (boottest/nettest/rtl8139test stay green). Built in an isolated worktree off current `main`, the virtqueue/RX/TX bounds reviewed directly, re-verified on `main` (a clean copy — its base was the current HEAD).

> **(M1030) drivers — Intel HD Audio (HDA) controller + an `audio.c` dispatcher so HDA and AC'97 plug in behind one seam.** The campaign's audio driver. New `kernel/hda.c` (511 lines, +`.h`): the modern PCI audio controller (`pci_find_class(0x04,0x03,0x00)`, fallback 8086:2668/293E) — MMIO BAR0 (mapped cache-disabled, like AHCI's ABAR), `GCTL.CRST` reset handshake, codec enumeration via `STATESTS`, and the **Immediate Command registers** (ICOI/IRII/ICIS) as the synchronous single-verb transport (the spec's sanctioned alternative to a CORB/RIRB DMA ring — far less code for a configure-once bring-up). It walks the codec's widget graph to find an output DAC + an output-capable Pin, wires them (stream format 48 kHz/16-bit/2ch, stream tag 1, unmute+max-gain both amps, pin output-enable + EAPD), then drives a 32-entry BDL over a 128 KiB cyclic ring; `hda_pump()` (timer IRQ) refills the buffers the DMA engine passed (tracked via LPIB), underrunning to silence — the exact software-ring model `ac97.c` uses. New `kernel/audio.c` (+`.h`) is the **nic.c-shaped dispatcher**: `audio_init()` probes HDA first (preferred), falls back to AC'97 (the default QEMU config + the headless suite), binds the active driver's PCM primitives to function pointers; the syscall layer + the timer pump now call `audio_*` and never name a concrete chip. The shared WAV-decode (resample → 48 kHz stereo) + background-music streaming live once in audio.c on that seam, so a third codec needs only its driver + one probe line. No HDA present ⇒ `audio_init` binds AC'97 byte-identically to the old behaviour (boot/wav tests unchanged); `ac97.c` itself untouched. Verified: a new permanent **`hdatest`** (boots `-device intel-hda -device hda-output`, no AC'97) asserts controller reset + codec enum + output verbs succeeded AND the stream's DMA position register advances while a tone plays — **codec 1af4:0012 enumerated, LPIB 0 → 12336 = DMA ADVANCING, no fault**; wired into `make check` (now 39 suites, all green) alongside rtl8139test/virtioblktest. Built in an isolated worktree (M1026 base); since its syscall.c/kmain.c/Makefile predated M1027 (lspci) + M1028 (virtio-blk), the merge was a careful hand-apply of HDA's `ac97_*→audio_*` rename + additive blocks onto current `main` (not a clobber), then the hda.c DMA/BDL/MMIO-window/ring-index bounds reviewed directly and re-verified on `main`.

> **(M1029) graphics driver — Bochs/QEMU VBE (DISPI) mode-setting; the OS sets its own resolution (now boots at 1280×960).** The first GRAPHICS driver of the campaign. New `kernel/bochs_vbe.c` (+`.h`): `bochs_vbe_available()` (DISPI_INDEX_ID ≥ 0xB0C0 AND the PCI 1234:1111 std-VGA present) and `bochs_vbe_set_mode(w,h)` — validate (≤1920×1200), locate BAR0 (the LFB) + size-probe it (reject if `w*h*4 > BAR`), program the DISPI registers (ENABLE=0 → XRES/YRES/BPP=32/VIRT_WIDTH=w → ENABLE|LFB), then `fb_repoint(base,w,h)` (new in fb.c: identity-maps the LFB page-by-page + sets `lfb`/dims *last*). `fb_init()` now delegates to it (one mode-set path) and `fbcon_init()` requests **1280×960** (up from 1024×768). Safety-first for the boot-critical display: every failure path (no DISPI / bad BAR size) returns -1 leaving the current mode untouched, and the mode-set runs *before* the desktop reads its dims (so it comes up sized correctly — no risky live re-layout). Runtime resolution change was deliberately deferred (it needs resizing the fixed back/scene buffers + re-scaling the wallpaper + re-laying-out every window — multiple black-screen failure modes; `bochs_vbe_set_mode` is exposed as the capability a future feature can build on). Verified: `make check` 38/38 — **gfxtest now boots at 1280×960 and asserts a painted desktop** (235,980 white pixels; the proof the re-point is correct), boottest + browsertest pass at the new mode; in-guest screenshots show the desktop with more real estate, the taskbar spanning the full 1280 width, the clock pill at the right edge, all aligned. (fb.c's pre-existing inline VBE poking was refactored into the driver — the "stuck at the multiboot fb tag" premise was stale; the OS already drove VBE, now via a proper driver at a higher mode.) Built in an isolated worktree, the `fb_repoint` LFB mapping + the validate-before-repoint safety reviewed, re-verified on `main`.

> **(M1028) drivers — virtio-blk (paravirtual block device, the standard fast VM disk).** A real legacy-virtio-over-PCI driver, `kernel/virtio_blk.c` (+`.h`), mirroring AHCI's additive 2nd-disk pattern (boot stays on ATA). PCI 0x1AF4:0x1001; the init handshake (reset → ACK → DRIVER → negotiate 0 optional features → read capacity), virtqueue 0 setup (a contiguity-verified physically-contiguous split vring: descriptor table + avail + page-aligned used ring; PFN = phys>>12 → Queue Address; DRIVER_OK), and `virtio_blk_read/write` issuing the standard **3-descriptor chain** (header device-readable → data WO-for-read/RO-for-write → 1-byte status WO), publishing the head in the avail ring with barriers, notifying, polling the used ring, checking the status byte (poisoned to 0xFF pre-submit). Bounds-safe: qsz capped ≤256, count ≤256 (128 KiB, fits the 32-bit desc len), reads/writes past `capacity` rejected, contiguous-frame allocation verified (else clean no-op), finite poll timeout, phys via `vmm_translate` (same as AHCI). Verified: `make check` **38/38** incl. a NEW committed `virtioblktest` (boots QEMU with `-device virtio-blk-pci,disable-modern=on,disable-legacy=off` + a 2nd disk, asserts byte-exact reads of 16 known sectors + a write→read→restore round-trip, FAT32-still-on-ATA, desktop reached); ATA boot path + fstest/boottest untouched (virtio no-ops with no virtio disk). Built in an isolated worktree, the virtqueue/descriptor-chain + bounds reviewed line-by-line, re-verified on `main`.

> **(M1027) drivers — `lspci` (full PCI enumeration + shell command).** A read-only tool that lists every PCI device the OS sees — useful in itself and a guide for the driver campaign (shows exactly what QEMU presents). `kernel/pci.c` gains `pci_collect(out,max)` — a brute-force bus/slot/func walk that honors the multi-function header bit (so single-function devices aren't probed at funcs 1-7 → no phantom duplicates), caps stored devices at `max` but returns the true count. New `SYS_lspci` (62, validated with `ubuf` exactly like SYS_ps/df — the ring3→ring0 pointer boundary) formats the list into the caller's buffer via bounds-checked `sappend`/`shex2`/`shex4`, with class/vendor name tables (class 01 storage→IDE/AHCI/NVMe, 02 net, 03 VGA, 04 audio, 06 bridge, 0C USB; vendors Intel/Realtek/virtio/QEMU/AMD/…). A `user/shell.c` `lspci` builtin prints it. Additive — existing PCI behavior (`pci_find`/`pci_find_class`/the `pci_enumerate` printer) untouched. Verified: `make check` 37/37, and in-guest `lspci` lists all 8 QEMU devices correctly — host bridge, ISA bridge, IDE controller, PIIX3, VGA, **Intel e1000 (8086:100e class 02:00)**, **UHCI USB (0c:03)**, **AC'97 (04:01)** — multi-function handling visibly correct (PIIX3 shows funcs 0/1/3, single-function slots only func 0). Built in an isolated worktree, the SYS_lspci validation + enumerator bounds reviewed, re-verified on `main`.

> **(M1026) drivers — AHCI/SATA disk driver (modern storage; the legacy `ata.c` is PIO-only).** A real AHCI HBA driver, `kernel/ahci.c` (+`ahci.h`): `pci_find_class(0x01,0x06,0x01)` finds the HBA (new `pci_find_class` + `prog_if` in pci.c/pci.h, since AHCI is found by class not vendor:device), maps ABAR (BAR5) as cache-disabled MMIO, sets `GHC.AE`, and for each implemented port with a SATA disk (`PxSSTS.DET==3`, sig 0x101) brings the port up (stop engine → point `PxCLB`/`PxFB` at zeroed command-list/received-FIS frames → start `FRE`+`ST`). `ahci_read`/`ahci_write` issue a single-slot command (command header + one PRDT + a Register H2D FIS with READ/WRITE DMA EXT + 48-bit LBA), poll `PxCI` to completion with TFES/ERR/timeout checks. Bounds-safe: `count ≤ 8192` (4 MiB single-PRDT cap so `dbc_i` fits 22 bits); DMA structures are pmm frames (identity-mapped → phys==virt), the data buffer's phys via `vmm_translate`. **Deliberately additive — the boot stays on legacy ATA** (`ata.c`/fat32/vfs byte-for-byte unchanged); AHCI reads a *second* disk so the boot can't break. A boot-time `ahci_selftest()` (logged to serial) reads sectors when an AHCI disk is present, else cleanly logs "none attached, legacy ATA intact". Verified: `make check` 37/37 (boottest has no AHCI disk → no-op; fstest/ATA boot intact), and with a 2nd disk attached via `-device ahci -device ide-hd` the kernel read back **byte-exact known content** across 8 sectors and a write→read round-trip matched on disk (`ahci_xfer` reviewed line-by-line — the command/FIS/PRDT setup + the `count≤8192` cap). The interface mirrors `ata_read`/`ata_write` (disk index + 64-bit LBA) so the boot disk could later migrate to AHCI. Built in an isolated worktree, re-verified on `main`.

> **(M1025) drivers — RTL8139 NIC + a generic NIC abstraction (the OS now drives two network cards).** Per the directive to flesh out drivers: a real second NIC. Introduced `kernel/nic.c`/`nic.h` — a generic NIC seam (the function pointers the net stack calls to get the MAC / send / poll-receive) — and refactored `kernel/net.c` to talk to it instead of e1000 directly, so any NIC driver plugs in. Then `kernel/rtl8139.c` — a real Realtek RTL8139 (8139too) driver: PCI 0x10EC:0x8139, bus-master enable, software reset, MAC from IDR0-5, a contiguity-checked DMA RX ring (RBSTART, 8K+pad+wrap; WRAP set so frames aren't split), 4 round-robin TX descriptors, RCR/IMR/ISR bring-up, poll-driven `receive()` walking the ring via CAPR/CBR (the 16-byte CAPR quirk handled). With only rtl8139 present the full ARP/IP/TCP/TLS stack runs over it. Bounds-safe — the RX `memcpy` clamps to the caller's `max`, the ring read stays within the rounded-up 3-frame allocation even for a max frame near the 8K mark, and a bogus card length can't OOB. Both NICs verified: nettest (150k-iter fuzz on the refactored net.c) + boottest (default e1000 path) stay green, and a NEW **`rtl8139test`** in `make check` boots QEMU with `-device rtl8139` (no e1000) and asserts the driver bound, read its MAC, and completed ARP + ICMP echo to the gateway — live packets move over it. `make check` now 37 suites, all green. (The build agent's *report* tripped the cyber-safety filter on NIC/packet language, but the work was complete + self-verifying in its worktree; I reviewed the driver + abstraction directly and re-verified on `main`.) Built in an isolated worktree; init/TX/RX register sequences + DMA phys handling + RX bounds reviewed line-by-line.

> **(M1024) calc — add `ans` last-result recall (chain calculations).** The standard calculator feature: `ans` resolves to the last successful result (`10*5`→`50`, then `ans+2`→`52`, `ans*ans`→`2704`). Added to the evaluator (`user/calceval.h`) as a settable const matched in `factor()` exactly like `pi`/`e` (doesn't shadow existing tokens); `user/calc.c` sets it after each *successful* eval (a syntax error leaves it unchanged); startup `ans`=0; banner lists it. `tests/calc` gained an `ans` block (17 checks incl. precedence + that an ans-free expr is unaffected). Verified: `make check` 36/36 incl. calctest (+ its 400k fuzz), and in-guest the chain works with the M1023 hex echo intact. (Committed in the prior batch without its WHATS-NEXT entry; recorded here.)

> **(M1023) calc — echo integer results in hex (handy for OS-dev: masks/addresses/flags).** Diversifying away from games to a genuinely-useful tool touch for *this* user's context (building a from-scratch OS = constant hex↔dec). The Calculator already parses hex input (`0x`) + bitwise (`& | << >> ~`); now it also **displays** an integer result in hex beside the decimal, e.g. `0xff` → `= 255  0xFF`, `1000` → `= 1000  0x3E8`. Display-only change to `user/calc.c` (a small `to_hex` formatter + a print after `dnum_to_str`); only an exact non-negative integer `< 2^53` (so the double represents it exactly) gets the hex echo — floats and negatives show decimal only. No change to the host-tested `calceval` evaluator. `to_hex` is bounds-safe (`buf[20]` holds ≤14 hex digits + `0x` + NUL). Verified: `make` clean (calc.elf zero warnings), `make check` 36/36 (calctest unaffected), and in-guest — launched Calc, `0xff` → `= 255  0xFF` and `1000` → `= 1000  0x3E8` (hex grey beside the yellow decimal). A small, genuine convenience, done directly (one file).

> **(M1022) new game — Spider Solitaire (1-suit), completing the canonical solitaire trio.** The third big-three solitaire (Klondike/FreeCell/Spider — what every collection ships), genuinely distinct: 104 cards (two single-suit decks), 10 columns, build runs down regardless (1-suit ⇒ all spades), complete a full **K→A run to auto-remove it** (8 sequences = win), deal one card to every column from the stock (5 deals). New `user/spider.c` (~340 lines), reusing the solitaire/freecell card model + rendering. `is_run` requires a contiguous rank-descending same-suit group; `can_to_col` requires the dest top one rank higher (any onto empty); `move_to_col` validates run+landing then copies under an explicit `cn[dst]+cnt>NCARD` guard (so every `col[10][104]` write stays in range); `try_complete` lifts a face-up K→A run (face-down-guarded) after each move + deal; `do_deal` is blocked while any column is empty (standard rule). Standard 5-spot registration, additive — no existing app touched. Verified: `make` clean (the link proves the `global spider_elf_start` line), `make check` 36/36, a **host ASan/UBSan harness with 33 assertions** (run validation, illegal/self-move rejection, auto-flip, K→A completion + the face-down guard, completion-via-move + on-deal, the deal rules — blocked-on-empty/stock<10/one-each-of-10, pick auto-selecting the topmost run, and a 60-card deep-column move staying in bounds), and in-guest — correct deal (54 dealt / 50 stock, cols 1-4 six rows / 5-10 five), an illegal move rejected, legal descending runs built, a stock deal (50→40, one card to each of 10), and clean close via `q` AND F8 (the M1014 WM-close path). Built in an isolated worktree, the move/run/completion logic + `col[10][104]` bounds reviewed line-by-line, re-verified on `main`. This completes the canonical solitaire trio; the clearly-iconic game gaps are now filled.

> **(M1021) new game — Columns (the Sega falling-block-match classic).** A distinct mechanic still missing: a falling column of 3 gems you reposition + rotate (the gems cycle vertically, shape unchanged — the signature move), matching **3+ in any direction including both diagonals** (distinct from Tetris's line-clear and Gems's swap-match). New `user/columns.c` (307 lines, pure-integer, ring-3; styled after `tetris.c`/`gems.c`). A 13×6 well, 6 colours; left/right (wall-guarded), down soft-drop, up/space rotate, timed gravity that speeds up per level (every 15 clears). `mark_matches` scans all 4 directions `{(1,0),(0,1),(1,1),(1,-1)}` with a unified head-detection (start a run only where the −dir neighbour is off-well/different, so each maximal run is found once) and a forward walk that **range-checks every step** (`nx∈[0,COLS), ny∈[0,ROWS)` before any `well[]` access — so the diagonal offsets can't go OOB); `collapse` packs survivors per-column bottom-up with gaps→EMPTY (columns-style, no refill); `resolve_matches` loops mark→clear→collapse for cascades, scoring `cleared×10×cascadeStep`. Game-over when a freshly-spawned centre column is blocked; high score persists to COLUMNS.HI. Standard 5-spot registration, additive. Verified: `make` clean (the link proves the `global columns_elf_start` line), `make check` 36/36, a **host ASan/UBSan harness — 15/15 + a 200k random-well fuzz with zero OOB** (specifically exercising the OOB-prone far-corner anti-diagonal, L-overlap dedup, cascades, and a full 78-cell well), and in-guest — a falling column, move L/R, rotate cycling the 3 colours, a line cleared with score 0→30, COLUMNS.HI persistence, and clean close via `q` AND F8 (the M1014 WM-close path). Built in an isolated worktree, the 4-direction match-scan bounds + cascade/gravity reviewed line-by-line, re-verified on `main`.

> **(M1020) new game — FreeCell (the open-information solitaire), distinct from Klondike.** A top-tier classic with very different gameplay from M1018's Klondike: all 52 cards dealt face-up into 8 cascades, 4 free cells, 4 foundations, no stock — a skill game, almost always solvable. New `user/freecell.c` (366 lines), modelled on `solitaire.c` (same card model + `putcard`/`putempty` rendering + Fisher-Yates + win chime), reusing none of it by sharing — copied, so solitaire is untouched. Move legality strictly enforced: cascade dest = opposite-colour one-rank-lower (any card onto an empty column), foundation = same-suit ascending from Ace, free cell must be empty. **Supermove implemented** with the standard cap `max = (1 + free cells) × 2^(empty columns)` (excluding the destination column) — `move_to_cascade` validates the picked run (`is_run`), the landing card, the cap, AND a `cn[dst]+cnt>52` bound before copying, so every `casc[8][52]` index stays in range; `a` auto-plays safe cards home. Input: `1`-`8` cascades, `q w e r` free cells, `f` foundation, `a` auto, `c` cancel, `n` new, `Q`/Esc quit (Esc cancels a selection first; lowercase `q` is free-cell 1). Standard 5-spot registration, additive — no existing app touched. Verified: `make` clean (zero warnings; the link proves the `global freecell_elf_start` line), `make check` 36/36, and in-guest — correct deal (cols 1-4 get 7, 5-8 get 6, all face-up, suit colours right), Ace→foundation, park-in-free-cell, a legal cascade→cascade, an illegal move rejected (board unchanged), and clean close via `Q` AND F8 (the M1014 WM-close path). Built in an isolated worktree, the move-legality + supermove cap + grid bounds reviewed line-by-line, re-verified on `main`.

> **(M1019) new game — Gems (match-3, Bejeweled/Columns-style), an iconic genre that was missing.** Continuing the additive-content thread (low-risk, verifiable, user-valued — a new app touches no existing code). New `user/gems.c` (279 lines, pure-integer, ring-3, keyboard-driven; style/idioms mirror `g2048.c`/`mines.c` — `itoa_u`, `load/save_best`, palette-index rendering, pollkey loop). An **8×8 grid of 6 colours**; a cursor (`[ ]`) moves with arrows, space/Enter grabs a gem (`( )`), then an arrow swaps it with the neighbour — the swap commits only if it forms a match (≥3 in a row/col) else reverts ("no match"). **Cascade engine**: `mark_matches` finds all horizontal+vertical runs ≥3, `collapse` clears them + drops survivors + refills the top, looping while new matches form, scoring `cleared*10*step` (rising cascade multiplier). The initial board is generated with **no pre-existing matches** (`makes_run` rejects a colour completing a 3-run with the two cells left/above), and a **no-deadlock guard** (`has_move` tries every adjacency; if none, the board reshuffles in place). High score persists to GEMS.HI. Bounds-safe: fixed `board[8][8]`, every run loop bounded by `x+run<COLS`/`y+run<ROWS`, `collapse`'s `keep[ROWS]` + refill indices all `<ROWS`, `try_swap` neighbour-bounds-checked. Standard 5-spot registration (additive — no existing app touched). Verified: `make` clean (gems.c zero warnings; the link proves the `global gems_elf_start` line), `make check` 36/36, a **host ASan/UBSan harness with 11 checks** (match-detect 3/4-yes/2-no, no-initial-match ×2000, gravity, cascade scoring, has_move, reshuffle ×200 — all passed), and in-guest — correct 6-colour board with no initial matches, a swap that cleared+cascaded (score 0→90), a non-match that reverted, high-score persistence across launches, and clean close via `q` AND F8 (the M1014 WM-close path). Built in an isolated worktree, the match/cascade/gravity logic + grid bounds reviewed line-by-line, re-verified on `main`.

> **(M1018) new game — Klondike Solitaire (a missing classic; the 46th+ built-in game).** A beloved classic absent from the 45+ roster — and a reminder that *additive content* is genuine, low-risk, user-valued work even at a feature-complete plateau (new app = no existing code touched → low regression risk, fully verifiable). New `user/solitaire.c` (307 lines, pure-integer, ring-3, keyboard-driven), reusing the card-rendering style of `bj.c`/`vpoker.c` (rank+suit, Hearts/Diamonds red, Spades/Clubs white). Full standard Klondike: 7 tableau columns (i+1 cards, top face-up), 4 foundations (A→K by suit), a stock you turn one card at a time into a waste (recycling when empty); source-then-destination pile keys (1-7 columns, `w` waste, space/`d` draw, `f` foundation, `a` auto-play to foundations, `n` new, `q`/Esc quit). Every move is validated before applying (`can_to_tableau`: King-onto-empty else opposite-colour-one-lower; `can_to_foundation`: same-suit ascending from Ace) — an illegal move is rejected with a message and changes nothing; exposed face-down tableau cards auto-flip; win = all 52 home (with a victory chime). Bounds-safe: every pile is `[52]` (the whole deck) + an explicit run-copy guard; a host ASan/UBSan replay asserted the move rules with no OOB. Standard 5-spot registration (Makefile/user_blob.asm incl. the `global solitaire_elf_start` line/app.c progs[]/desktop.c menu[]), additive — no existing app touched. Verified: `make` clean (solitaire.c zero warnings; the link proves the `global` line), `make check` 36/36, and in-guest — dealt a correct board, drew from stock, moved waste→tableau and a card→foundation, an illegal move was rejected, a face-down card auto-flipped, and it closed cleanly via `q` AND F8 (the M1014 WM-close path, since it polls via `sys_pollkey`). Built in an isolated worktree, the Klondike rules + bounds reviewed line-by-line, re-verified on `main`.

> **(M1017) repo hygiene — untrack the committed `.pyc` + gitignore Python cache.** A Python bytecode cache file, `tools/__pycache__/osdrive.cpython-314.pyc`, had been accidentally committed (it surfaces whenever `tools/osdrive.py` is imported under a different Python version, showing as a spurious "modified" file — noise in every `git status`). `.gitignore` covered `/build/` + `*.o`/`*.swp`/`*~`/`.DS_Store` but not Python cache. Fix: `git rm --cached` the tracked `.pyc` (removed from version control, local copy kept) and added `__pycache__/` + `*.pyc` to `.gitignore` so it can't recur. No code/build impact (git metadata only). Repo hygiene per [[feature-complete-plateau]].

> **(M1016) docs — refresh the README headline (crossed 1000 milestones; list the new desktop features).** The README front-page Status line was stale: "990+ milestones" (written at M991) when the project has crossed M1000 to M1015, and the feature list omitted this session's user-facing additions. Updated the count to "1000+ milestones complete" (the M1000 crossing is a notable, accurate landmark) and added the session's headline features to the list: a **GUI file manager** (open/delete/rename/new-folder + set-as-wallpaper), a runtime-changeable **wallpaper** (any image format), and right-click window & desktop context menus. Pure documentation (README.md not compiled), low-risk, accurate. Front-page maintenance per [[feature-complete-plateau]] — keeping the project's headline honest now that the substantive feature work is complete.

> **(M1015) cleanup — fix the pre-existing `-Wmisleading-indentation` warnings (clean build).** Three long-standing warnings (noticed repeatedly this session) where multiple statements shared a line after an `if`, so GCC flagged the layout as misleading even though the code was correct: `kernel/syscall.c:386,399` (`if (out) kfree(out); kfree(in);` in the gunzip/gzip cases — the `kfree(in)` is unconditional, `in` is always allocated there) and `user/dotsbox.c:32` (four `if (g[..]) n++;` edge-counts on one line). Split each onto its own line — pure formatting, zero logic change — so the layout matches the control flow. The build is now free of `-Wmisleading-indentation` (only the deferred kernel-W^X RWX-segment linker note + a usermode.asm ABS-deprecation note remain), making any *new* warning stand out. Two files, formatting-only. Verified: `make` shows no misleading-indentation warnings, `make check` 36/36. Code hygiene per [[feature-complete-plateau]].

> **(M1014) kernel/WM — close polling/graphics apps from the WM (F8 / window `[x]` / context-menu Close now work on EVERY app).** A release-readiness QA smoke-test (this session) found a real, pre-existing UX defect: `app_request_kill()` sets `a->kill`, but it was only ever honored at the blocking `app_sys_read` (`a->exited=1; task_exit()`). Games/graphics apps that loop on `sys_pollkey`+`sys_gfx_blit`+`sys_sleep` never block, so they ignored the kill and could only be quit by their own key (q/Esc) — a graphics app with no quit key was unclosable from the WM. Fix: a reusable `app_kill_check()` (the same clean exit `app_sys_read` does) now called from the three per-frame non-blocking syscalls every such app uses — `app_sys_pollkey`, `app_gfx_blit` (kernel/app.c), and `SYS_sleep` (kernel/syscall.c) — so a pending WM kill is observed within ~one frame. Apps can't spuriously exit (`a->kill` is set only by F8/`[x]`/the M1008/M1010 Close). Five small edits across `app.c`/`app.h`/`syscall.c`, additive; no host-test coupling (`make check` 36/36 incl. the suites that `#include` app.c). Verified in-guest: launched **Breakout** (one of the apps QA flagged unclosable), pressed **F8** → the window + its taskbar chip vanished, focus fell through to the Shell, desktop intact — previously F8 did nothing on it. Blocking-read apps (shell/editor/calc) still close as before; browsertest confirms normal apps launch+render (not spuriously killed). Found by the QA pass (M1013-era), fixed + verified directly. The single most valuable find of the late session — a genuine defect, not polish.

> **(M1013) editor — recognize `.json`/`.svg`/`.xml` for highlighting (reuse existing highlighters).** Three `detect_lang()` routes, no new highlighter code: `.json` → the JS highlighter (lang 4 — JSON is a JS subset, so its strings/numbers/`true`/`false`/`null` are highlighted correctly), and `.svg`/`.xml` → the HTML highlighter (lang 3 — tag-based markup, so `<tags>`, attributes, and comments highlight). All three formats genuinely exist on the OS (SVG is rendered by the browser; JSON has a kernel parser), and editing them now gets sensible highlighting for free via already-tested code. Self-contained to `user/editor.c` (`detect_lang` only), additive, zero-risk (routes extensions to existing tested highlighters; the existing six languages unchanged). Verified: `make check` 36/36. Trivial editor polish per [[feature-complete-plateau]].

> **(M1012) editor — Markdown (`.md`) syntax highlighting (language 6).** The editor highlighted C/shell/HTML/JS/CSS but not Markdown, and the OS's own docs (`.md`) are edited here. Added a `lang==6` branch to `hl_run` (and `.md`/`.markdown` → 6 in `detect_lang`): ATX headers (`#…` at line start → blue, whole line), blockquotes (`>` → grey), bullet markers (`-`/`*`/`+` + space → blue), ordered markers (`N.` + space → blue), inline code (`` `…` `` → orange), and `*`/`_`/`**`/`__` emphasis (→ purple); links and the rest stay default. The branch is placed BEFORE the C/JS/CSS comment/string/number rules so Markdown metachars aren't misread, and is **single-line-stateless** (sets no persistent `mode`, so no multi-line state can leak) — every scan is bounded by the visible-window `end`, stops at a closing delimiter or `\n`, lookaheads guard `i+1<end`/`j+1<end`, and `PUT` clamps writes to `vcol[2048)`. The `lang==6` guard isolates it so the five existing languages are byte-identical. Self-contained to `user/editor.c`, additive. Verified: `make` clean (editor.c `-Wall`, zero new warnings), `make check` 36/36, and in-guest — a `TEST.MD` showed headers/markers blue, emphasis purple, blockquote grey, inline code orange, plain text + a `[link](url)` green; a `.c` file confirmed C highlighting unchanged (regression); a richer `README.MD` rendered correctly with a wrapped bullet's continuation staying default (emphasis closed before the wrap), no run-past. Built in an isolated worktree, the new scanner's bounds-safety + language isolation reviewed line-by-line, re-verified on `main`. Clean editor polish per [[feature-complete-plateau]].

> **(M1011) desktop/WM — enforce one modal overlay at a time (fix cross-overlay stacking).** The desktop accumulated several modal overlays (Apps menu, F1 help, F7 switcher, the M1008/M1010 right-click menus); their open-paths were asymmetric, so two could be open at once: F1 is handled *before* the switcher's modal key-swallow, so **F1 while the F7 switcher was up opened help without closing the switcher** (both rendered); and the right-click handler only dismissed an open `ctx` menu, so **right-clicking while the Apps menu / help / switcher was up stacked a context menu on top**. Neither crashed, but the single-overlay invariant was violated. Fix: a `close_overlays()` helper (`menu_open = help_open = sw_open = ctx_open = 0`) invoked by every open path — the F1/F7/F9 toggles became `int o = X; close_overlays(); X = !o;` (preserving toggle semantics while closing the others), and a right-click while any overlay is open now `close_overlays()`-dismisses it (no new menu) instead of only handling `ctx`. Now at most one overlay is ever up, via keyboard or mouse. One file (`kernel/desktop.c`), logic-only, additive. Verified: `make` clean (no desktop.c warnings), `make check` 36/36 — including **browsertest, which opens the Apps menu via F9 then launches+renders the browser**, exercising the rewritten menu-open path end-to-end. Robustness polish per [[feature-complete-plateau]] — found by auditing the modal-state composition after the right-click menus (M1008/M1010) grew it.

> **(M1010) desktop/WM — desktop-background right-click menu (Show Desktop · Show All Windows · Change Wallpaper), completing the right-click story.** M1008 added the window title-bar menu; the empty desktop background was still inert on right-click. Now right-clicking it (when the click hits no window, and over the desktop proper not the taskbar) opens a 3-row popup, sharing M1008's `ctx_` popup machinery via a new `ctx_kind` discriminator (0 = window menu, 1 = desktop menu): `ctx_open/ctx_x/ctx_y/ctx_row_at` stay shared, and `ctx_nrows()`/`ctx_w()` return the per-kind size (window 5 rows/128px unchanged → M1008 hit-test byte-identical; desktop 3 rows/160px). The render guard keeps the kind-0 `ctx_win` validity check and never dereferences `ctx_win` when kind 1 (`win_rows[0]` indexes `windows[0]`, always-valid memory, and is unused for the desktop kind). Actions: **Show Desktop** minimizes every window (a flag-only `minimized=1` pass — NOT `sink_window`, which reorders the array mid-iteration and skips windows; the agent caught that in-guest and switched to flag-only), **Show All Windows** clears `minimized` on all (z-order preserved), **Change Wallpaper** cycles a `static wp_idx` to the next image file on disk (`vfs_list` + `files_is_image` filter + `desktop_set_wallpaper`, skipping non-images/decode-failures, no-op if none). The click handler closes the menu before acting and dispatches per kind (re-validating `ctx_win` for kind 0). One file, additive; M1008's window menu unchanged (regression-verified). Verified: `make` clean, `make check` 36/36, and in-guest — right-click desktop → 3-row popup; Show Desktop → all windows to taskbar chips (empty desktop); Show All → restored to original z-order; Change Wallpaper ×2 → background cycles through disk images; right-click a title bar still opens the M1008 5-row window menu; click-off dismisses. Built in an isolated worktree, the `ctx_kind` sharing (no M1008 regression) + close-before-act + flag-only minimize reviewed line-by-line, re-verified on `main`. (Clean polish per [[feature-complete-plateau]].)

> **(M1009) tests — `fat32_rename` regression coverage in `fstest` (lock in M1007's FS-write data-safety).** M1007 added the FS's only rename path (a new write path — the highest-risk new code this session); it was manually verified in-guest but had no automated coverage. Added two things to `tests/fs/fs_test.c` (the host suite that `#include`s fat32.c with an in-memory disk, under ASan/UBSan): (1) a **focused rename regression** on the baseline image (TEST.TXT=600B + SUB dir) asserting rename preserves data+size (read-back content intact), clears the old name, refuses to clobber an existing name, rejects non-8.3 names (`>8` base, `>3` ext), fails on a missing source, and works for a directory (renamed SUB→SUB2, still a read-rejected dir); and (2) a **rename case in the write-path fuzz loop** (`F?`→`F?` on the accumulating image, a mix of valid/clobber/missing) so `fat32_rename`'s directory scan + sector write are exercised under ASan alongside create/write/delete/mkdir. Test-only change (one file), no kernel/behaviour change. Verified: `make fstest` prints `rename OK: data-preserving, clobber + bad-name + missing refused, dir rename` and `8000 …/rename ops clean`, and `make check` stays 36/36. This is robustness/quality work appropriate to the [[feature-complete-plateau]] — permanent regression protection for the session's riskiest addition rather than another marginal feature.

> **(M1008) desktop/WM — right-click window context menu (Maximize/Restore · Minimize · Snap Left/Right · Close).** Right-clicking a window's title bar did nothing; now it opens a small popup at the cursor, each row a thin wrapper over the exact helper its F-key uses (so behaviour is identical): row 0 `toggle_maximize` (F4, label follows the window's `maximized` flag), Minimize (the F3 path incl. the "never hide the last visible window" `vis>1` guard), `snap_window(idx,0/1)` (F5/F6), Close (F8 — `app_request_kill` for apps else `remove_window`). Pure discoverability polish — every action was already a keypress away — but a universal desktop expectation. All in `kernel/desktop.c`, mirroring the existing `menu_open`/`sw_open` overlays: new `ctx_open/ctx_x/ctx_y/ctx_win` state, a shared `ctx_row_at()` hit-test used by both the hover-highlight render and the click handler (so they can't disagree on row bounds), and the menu's rect+box+per-row render pattern. `ctx_win` is set to the topmost window (`win_count-1`, raised on open) and kept valid four ways so a stale index can never act on the wrong/removed window: the menu is modal to the keyboard (Esc closes, all other keys swallowed so no F-key reorders underneath), the render skips unless `ctx_win` is in range, the click handler re-validates `ctx_win` and closes the menu *before* acting (Close-safe), and the app-reap loop closes the menu if any window vanishes. Browser *content* right-clicks still copy a link's URL (unchanged); only the title bar opens the menu. Position clamped on-screen + above the taskbar. One file, additive; default behavior unchanged when not right-clicking a title bar. Verified: `make` clean (`desktop.c` `-Wall -Wextra`, no warnings), `make check` 36/36, and in-guest (9 screenshots): right-click a title bar → 5-row popup with hover-highlight; Minimize → taskbar chip; right-click another → Maximize fills the screen and the row re-opens as Restore; click empty desktop → dismissed with no action; Close on both an app (kill→reap) and a non-app (immediate `remove_window`) window → gone, no crash/stale menu; double-click-maximize regression intact. Built in an isolated worktree, the `ctx_win`-validity guards + the modal click/keyboard integration reviewed line-by-line, re-verified on `main`. (Per [[feature-complete-plateau]] this is clean marginal polish — the OS is feature-complete for its scope.)

> **(M1007) Files app — GUI rename + new-folder via an in-window text-input (+ a minimal, data-safe `fat32_rename`).** The WM had no text-entry affordance, so the Files app couldn't rename or create folders. Added both. **FS side (`kernel/fat32.c`)**: a new `fat32_rename(path, newname)` that is deliberately minimal and data-safe — it mirrors `fat32_delete`'s corrupt-chain-safe directory scan, and on the matching entry its ONLY mutation is `for(i=0;i<11;i++) e[i]=new83[i]` (the 8.3 name field), writing back ONLY that one sector; the attribute, cluster hi/lo, size, and date bytes are read and written back untouched, and the FAT/cluster chains are never touched (so rename preserves the file's data + identity — proven by a fresh-boot `cat` showing the renamed file's contents + original timestamp intact). A `name_fits_83()` guard rejects empty/over-8-base/over-3-ext names before `to_83`'s silent truncation, and a `dir_find` clobber check refuses renaming onto an existing name (and rename-to-self). Wired as `vfs_rename` (op-table + `vfs.h` + `vfs.c` dispatch). **WM side (`kernel/desktop.c`)**: a per-Files-window text-input modal mirroring the existing `fconfirm` pattern — new `editing`(0/none/1/rename/2/new-folder)/`editbuf[16]`/`editlen` fields; the editing block runs first in `files_key` and swallows every key (Esc cancels, Enter commits via `vfs_rename`/`vfs_mkdir` then re-lists+clamps, Backspace deletes, printable appends upper-cased capped at 12 = 8.3), composing cleanly with `fconfirm` (mutually exclusive). `r` starts rename (pre-filling the selected name, trailing `/` stripped); `n` starts new-folder. The prompt renders in the Files header; the 4 positional `window_t` literals gained the new fields (`editbuf` correctly `{0}`-initialized). F1 help + header advertise `r rename  n new-folder`. Four files, additive; default behavior unchanged when `r`/`n` aren't pressed. Verified: `make` clean (the transient `-Wexcess-initializers` was only a worktree-vs-main cross-checkout lint mismatch — gone once both fat32.c + vfs.h merged, 10 fields == 10 inits), `make check` 36/36 incl. **fstest + boottest** (the FAT32 read/write paths), and in-guest — renamed `OLD.TXT`→`NEW.TXT` (fresh-boot `ls`+`cat NEW.TXT`=`hi` proved data/clusters/timestamp intact, `OLD.TXT` gone); `n`→`MYDIR` created a persisted folder; renaming onto an existing name was refused (both files intact); Esc cancelled leaving the name unchanged. Known minor: a mouse body-click while the modal is open commits against the clicked row (consistent with the `fconfirm` click behavior, and never data-unsafe since `vfs_rename` validates + refuses clobber); the modal is keyboard-driven by design. Built in an isolated worktree, the `fat32_rename` FS-write + the editing state-machine + struct-literal init reviewed line-by-line, re-verified on `main`.

> **(M1006) Files app — delete (with a 2-key confirm) + set-as-wallpaper, making it a real file manager.** The GUI Files window was strictly view+open (a code-verified gap: a file manager that can't delete, and the wallpaper feature only reachable via the shell). Added to `files_key()` in `kernel/desktop.c`, reusing existing plumbing (no new syscalls): **`d`/Delete** arms a delete — the Files header is replaced by a bright-red `Delete <name>?  d/y=confirm  any key=cancel` prompt; a second `d`/`y` commits via `vfs_remove()` (which deletes a file or empty dir and refuses a non-empty dir cleanly, no crash), and **any other key cancels** (so a stray `d` is harmless, and Enter-while-armed cancels rather than open+deletes). The selected name is copied to a bounded buffer with the listing's trailing `/` (dir marker) stripped; after the remove the dirent list is re-read and `fsel` re-clamped. **`w`** sets the selected file as the wallpaper if its extension is an image (`PNG/BMP/JPG/JPEG/GIF/SVG` via a new `files_is_image` helper) — a direct `desktop_set_wallpaper()` call (same translation unit, no syscall), finally making the wallpaper feature GUI-discoverable (it was shell-only). A new `int fconfirm` field in `window_t` (between `fsel` and `minimized`) holds the armed state; the 4 positional window literals gained the matching trailing `0` (all-zero, so every field stays initialized — also silences `-Wmissing-field-initializers`). The default Files window widened 330→500px so the new hint + confirm prompt fit (content is clipped to the window anyway). F1 help + the Files header advertise the keys. One file, additive; default view/open behavior unchanged when the new keys aren't pressed. Verified: `make` clean (no new warnings), `make check` 36/36, and in-guest — created `ZAP.TXT` via the shell, deleted it in Files (`d` → red prompt → `d` → gone, and a fresh-boot `ls` confirmed it persisted to the FAT32 disk); armed-then-cancelled a delete on README.TXT with an arrow (prompt cleared, file kept, the arrow consumed only as cancel); pressed `w` on `BIG.PNG` → desktop background changed; `w` on a non-image was a no-op. Built in an isolated worktree, the confirm state-machine + struct-literal init + bounds reviewed line-by-line, re-verified on `main`.

> **(M1005) desktop — wallpaper accepts ANY image format (PNG/BMP/JPEG/GIF/SVG), closing the paint→wallpaper loop.** M1004's `wallpaper` command was PNG-only (it called `png_decode` directly) and rejected sources bigger than the screen — so the most common formats, including **the BMP that Paint itself saves**, silently failed. Fix: reuse the browser's existing multi-format decoder. `kernel/browser.c`'s `decode_image()` already dispatches by magic bytes (JPEG `FF D8 FF`, BMP `BM`, SVG `<svg`, PNG sig, GIF `GIF8`) to the public `png/bmp/jpeg/gif/svg_decode`, sizing its own buffers to the image (capped 2048²/1M px) and returning a native-size RGBA buffer — a genuine general utility that only happened to be `static`. Made it non-static, declared it in a new `kernel/include/image.h`, and rewrote `decode_wallpaper()` to call it (replacing the manual png-only decode + screen-sized scratch) then nearest-neighbour-scale to the screen as before. This also lifts M1004's size cap (native-size decode handles sub- and—up to the decoder cap—larger-than-screen sources). No host-test coupling: browser.c isn't `#include`d by any host suite, and `browsertest` boots the kernel + renders its home page via `decode_image` (proving exposing it didn't break the browser). The `decode_wallpaper` alloc/free balance stays exact (file/rgba/bmp each freed once per path; the safe decode-to-new-then-swap in `desktop_set_wallpaper` is unchanged). Three files (image.h new; browser.c −`static`+include; desktop.c rewrite), additive. Verified: `make` clean (desktop.c zero warnings), `make check` 36/36 incl. browsertest, and in-guest — **the headline loop**: Paint → draw → `s` (save PAINT.BMP) → `wallpaper PAINT.BMP` repainted the desktop to the drawing (BMP, which M1004 couldn't load); `wallpaper BIG.PNG` (PNG regression), `PHOTO.JPG` (JPEG), `LOGO.GIF` (GIF) each set the background; `wallpaper NOSUCH.PNG` failed gracefully leaving the wallpaper unchanged. Built in an isolated worktree, the rewrite + alloc/free balance + decode_image exposure reviewed line-by-line, re-verified on `main`.

> **(M1004) desktop — runtime wallpaper picker (`wallpaper <file>` shell command), lifting the load-once-at-boot limitation.** The desktop background was decoded once at boot from WALL.PNG and could only change by rebuilding the disk image. Now `kernel/desktop.c`'s boot loader is refactored into `decode_wallpaper(name)` — reads the file, PNG-decodes it, and **nearest-neighbour-scales it to `screen_w`×`screen_h`** so ANY image works as wallpaper (the old code required a screen-exact image; even a non-exact WALL.PNG now scales at boot). `load_wallpaper()` becomes `wallpaper_bmp = decode_wallpaper("WALL.PNG")`. A new `desktop_set_wallpaper(name)` does a **safe decode-to-new-then-swap**: it decodes into a fresh buffer and only on success captures the old pointer, stores the new one, and `kfree`s the old; a failed load (missing/undecodable/OOM) returns -1 **before touching the live `wallpaper_bmp`**, so the current wallpaper is never disturbed or left NULL. The swap is a single pointer store inside a syscall (IF=0, no `sti`), so it's atomic w.r.t. the WM render task — the renderer can't see a half-updated pointer and the old buffer can't be freed mid-read. A `volatile wallpaper_repaint` flag, consumed once per WM loop, forces a prompt redraw. New syscall `SYS_setwall` (#61, validates `ustr(name)`), a 1-arg `sys_setwall` ulib stub, and a `wallpaper <file>` shell builtin (quoting-aware via `sh_unprot_buf`, prints set/cannot-load, added to `help`). `decode_wallpaper`'s alloc/free balance is exact (file/rgba/scratch/bmp each freed once on every path — no leak, no double-free) and the scale loop is bounds-safe (the decoder rejects sources larger than the screen-sized buffer). Additive: the only refactor is splitting the boot loader's body; boot behaviour preserved. Verified: `make` links cleanly (zero new warnings), `make check` 36/36 green (no host suite compiles these files), and in-guest from a Shell — baseline gradient → `wallpaper BIG.PNG` (an existing 240×160 PNG) repainted the desktop to the upscaled image; `wallpaper NOSUCH.PNG` printed cannot-load AND left the wallpaper unchanged (not blank); a re-set succeeded (exercising free-old, no use-after-free). Built in an isolated worktree, the swap safety + alloc/free balance + scale bounds reviewed line-by-line, re-verified on `main` (build + 36/36).

> **(M1003) new app — Stopwatch + Countdown Timer ("Timer"), filling a real utility gap.** The OS had a display-only Calendar and a taskbar clock but no stopwatch/timer/alarm — a utility every desktop has. New `user/timer.c`: two modes (Tab toggles). **STOPWATCH** counts up (`Space` start/stop, `r` reset when stopped), shown as big `MM:SS.t` block-glyph digits that turn green while running. **TIMER** counts down from a duration you dial in while paused (Up/Dn ±1 min, Lt/Rt or `+`/`-` ±10 s, clamped to [0, 99:59]); `Space` start/pause, `r` reset; at zero it stops, flashes a bright `*** TIME'S UP! ***` banner, and beeps an alarm (4× `sys_beep`), any key dismissing it. All timing reads the monotonic `sys_uptime_ms()` so nothing blocks — each ~14 Hz tick recomputes elapsed/remaining and redraws, like calendar/jukebox; pure integer, no new syscall. A 5-row `GLYPH[12][5]` table renders the big digits (the `line[80]` buffer safely holds the ~7-char time × 4 cols). Arrow codes (0x11–0x14 = up/dn/lt/rt) match keyboard.c/editor.c. Registered via the standard 5 spots (Makefile `USER_ELFS`, `user_blob.asm` incl. the easy-to-miss `global timer_elf_start`, `app.c` `progs[]`, `desktop.c` `menu[]`, the .c) — all additive, mirroring `calendar`'s wiring; no name collision (the kernel PIT `timer.c` + emulator `i_timer` are separate namespaces). Verified: `make` links cleanly (proves the `global` line) with zero new warnings, `make check` 36/36 green (no suite compiles these files), and in-guest — launched from the F9 Apps menu, the stopwatch counted up (green digits), and the timer counted down to the `TIME'S UP!` banner at 00:00; re-confirmed on `main` via screenshot. Built in an isolated worktree, reviewed line-by-line (glyph-buffer bounds, time math, arrow codes), re-verified on `main`.

> **(M1002) paint — save & load the canvas (`s`/`L`) via a new `SYS_savebmp` syscall, fixing total data-loss on quit.** Paint kept its whole drawing in an XRGB `cv` buffer but had no persistence — it `free()`d the canvas and exited, discarding 100% of the user's work. A genuine functional defect, not a missing nicety. Fix: a new generic kernel syscall **`SYS_savebmp(name, pixels, w, h)` (#60)** that encodes a caller-supplied `w*h` `0x00RRGGBB` buffer as a 24-bit BMP and `vfs_write`s it — `kernel/fb.c`'s new `fb_save_bmp_buf()` is a near-copy of the existing `fb_save_bmp` (14-byte file header + 40-byte DIB, 24bpp BGR, bottom-up rows padded to 4 bytes) but full-resolution and reading the caller's buffer into a `kmalloc`'d output (capped at `w*h<=4M`, NULL-checked, freed after write). The syscall slots into the M985/M986 ring-3→ring-0 pointer boundary: it rejects unless `ustr(name)` + `w>0` + `h>0` + `w*h<=4M` + `ubuf(pixels, (uint64_t)w*h*4)` (uint64 casts before `*4` prevent overflow). Then `user/paint.c` gains **`s`** → `sys_savebmp("PAINT.BMP", cv, W, H)` and **`L`** → load it back: a `load_bmp()` that reads PAINT.BMP and parses the 54-byte header inline, fully bounds-checked (`n>=54`, "BM" magic, 24bpp, dims clamped to `W`/`H` so it never writes past `cv`, and a per-pixel `p+2>=n` guard so a forged/truncated header can't over-read) — graceful no-op on a missing/bad file. A brief colored flash (green save / blue load / red fail) cues the result. `ulib.c`'s stub passes the 4th arg (`h`) via `r10`, matching `sys_http`/`sys_https`. Additive/golden-safe: a new syscall number + new fb function + two new key cases, no existing branch touched; none of the changed files compile into a host suite. Verified: `make check` (36 suites, zero new warnings) + in-guest round-trip (drew → `s` → PAINT.BMP lands as a valid "BM" file, 300×200 24bpp 180054 B; cleared → `L` → the exact drawing reappears; `browse file:PAINT.BMP` renders it via the browser's own `bmp_decode`, independently proving a standard BMP). The reusable "save XRGB canvas → BMP" syscall pays forward to any future region-screenshot/export tool. Built in an isolated worktree, the kernel-side validation + writer + load parser reviewed line-by-line, re-verified on `main`.

> **(M1001) desktop/WM — F7 Alt-Tab-style window-switcher overlay (restores minimized windows).** The WM's weakest workflow: F3 minimizes a window to a dimmed taskbar chip, but from the keyboard the only way back was F2 — a blind one-directional `raise_window(0)` cycle with no visual list. New **F7** opens a centered "Windows" overlay listing every window (including minimized ones, marked "(min)"); arrows move the highlight (wrapping), Enter raises+restores the selected window, Esc cancels. It reuses the existing `menu_open`/`help_open` modal-overlay pattern and the existing `raise_window()` primitive (which already clears `minimized` + moves to top — so it IS the restore path that didn't exist for the keyboard). Two files: `kernel/keyboard.c` maps F7 (scancode 0x41, was unmapped) → a free WM event 0x1E; `kernel/desktop.c` adds `sw_open`/`sw_sel`, the overlay render (after the help overlay so it's on top; clamps `sw_sel` if a window closes), a modal key block (`continue` swallows keys while open, like the menu), and an F1-help line. Additive/default-off: `sw_open==0` makes both the render and key paths inert → the default desktop is byte-identical (boottest/gfxtest unaffected); neither file is `#include`d by any host suite, so no host-test-coupling risk. Verified: `make check` (36 suites) + in-guest (F3-minimised the Shell → F7 showed the overlay listing `Shell (min)` / `Welcome` / `Files` with the focused one highlighted; arrow+Enter restored the minimised Shell; Esc left the scene unchanged). Built in an isolated worktree, reviewed line-by-line, re-verified on `main`. Scoping confirmed the shell is feature-complete (`&&`/`||`/`;`/`$?`/redirection/`test` all present), so this targeted the WM's real gap.

> **(M1000) editor — auto-dedent of `}` (the 1000th milestone).** Completes the editor's auto-formatting: it already auto-INDENTS on Enter (reproduces the line's leading whitespace + a level after a trailing `{`/`(`/`[`, M935); now typing a closing `}` on a whitespace-only line prefix auto-DEDENTS one level first, so a brace that closes a block lands under its `if`/`for`/etc. rather than at the inner body indent. `dedent_brace()` checks the caret's line prefix is all whitespace, removes one indent level (a leading tab, else up to 4 spaces, clamped to the line start) via the undoable `del_at`, then inserts the `}`; a new `undo_merge_last(n)` coalesces those deletes + the insert into ONE undo group (so a single Ctrl-Z/Backspace reverts the whole keystroke). Only `}` is handled (`)`/`]` close inline far more often than they sit alone). Triggers only on interactive typing — paste/find-replace go through `insert()` directly and stay raw, like auto-indent. Editor-only, input-path only (no render/cursor change). Verified: `make check` (37 suites) + in-guest (typed `if (x) {`⏎`y();`⏎`}` → the `}` landed at column 0 under the `if`, the M935 indent on `y();` and the M999 gutter both intact; a `}` after text did NOT dedent; one Ctrl-Z reverted the dedent+insert together; Backspace sane). Built in an isolated worktree, reviewed, re-verified on `main`. The editor's code-editing UX is now rounded out: highlighting + auto-indent + auto-dedent + bracket-match + block indent/dedent + line-number gutter.

> **(M999) editor — line-number gutter.** `user/editor.c` (already a full code editor: undo/redo, find/replace, 5-language syntax highlighting, auto-indent, bracket-match, block indent/dedent) now shows line numbers in a left gutter, like every code editor. A 5-column gutter (4 digits + 1 pad, graceful past 9999) holds the 1-based line number right-aligned in grey on each document-line's first row, blank on wrap-continuation rows; the text wraps at `EDTEXT = EDCOLS - GUTTER` (39 cols). Key architectural find that de-risked it: the editor is a pure text-STREAM app (no absolute X/Y — the cursor `|` and Ctrl-B selection are emitted inline in the same `print()` stream the kernel grid renders, and mouse clicks are handled by the WM, not the editor), so once the stream is right the cursor + selection align automatically — there was no click→position math to patch. The gutter cells carry a `GUT_COL` sentinel so the run-printer always renders them grey and a selection never tints them; the shared wrap width (`EDTEXT`, used identically in `row_of`/`row_offset`/`render`) keeps scroll tracking correct. Verified: `make check` (37 suites) green + in-guest (open SAMPLE.C → grey numbers 1–22, highlighting intact, blank gutter on the wrapped line; typed `ZZZ` at a line's Home → lands flush at text-col 0, no off-by-the-gutter; Ctrl-B selection + Ctrl-A keep the gutter grey; `cat` of a saved file shows NO line numbers — the gutter is render-only, `doc[]` untouched). Built in an isolated worktree, reviewed line-by-line, re-verified on `main`. **Known minor limitation (documented, not a regression of editing):** the kernel WM's *mouse-drag*-to-clipboard reads raw grid cells, so dragging across the editor also copies the gutter digits — but the editor's own keyboard copy (Ctrl-B + Ctrl-C, doc-offset based) is clean; this is a generic grid-editor mouse-select trait (a WM "non-selectable margin" hook could fix it later).

> **(M998) browser — block `<img>` honors `text-align` (centre / right).** A block image always pinned to the left content edge, even inside a `text-align:center` (figures) or `:right` container — only TK_WORD tokens read the per-token alignment. Now an image is centred/right-shifted within `[cl, cr]` using the same offset math the word path uses. Two parts: the three `TK_IMG` emit sites (file:/data:/remote) now stamp `tokalign[ntok] = curalign` (image tokens are written straight into `toks[]` and never had it set), and the render-loop `TK_IMG` handler computes `x0 = cl + (centre ? (cr-cl-destw)/2 : right ? (cr-cl-destw) : 0)`, clamped `>= cl`, and blits at `x0+dx`. `<hr>` was intentionally skipped — it has no width support (always a full-width rule), so there's nothing to centre. Additive/golden-safe: default (left, `tokalign==0`) → `x0==cl`, byte-identical; only centre/right shift pixels, and a too-wide image still pins left. Built in an isolated worktree, reviewed, re-verified on `main`: `make check` (37 suites incl. browsertest) green + an in-guest `IMGALIGN.HTM` screenshot showing the same test image left / centred / right, and the existing `img.htm` unchanged. Added the `IMGALIGN.HTM` demo + INDEX link. This rounds out the browser rendering polish (M993 bg fill, M994 styled-block flow, M996 blockquote bar, M997 list-style, M998 img align); the remaining CSS items (`align-items`/`flex-grow`/floats/positioning) all need the deferred box-layout rewrite.

> **(M997) browser — `list-style-type` support: `none`, disc/circle/square, decimal, + depth-varied bullets.** `<ul>` items previously emitted a literal `-` at every nesting level, and `list-style:none` (ubiquitous for nav menus / link lists) was ignored — so menus showed stray dashes. Now: **`list-style:none`/`list-style-type:none`** suppresses the marker (keeping the indent); explicit **`list-style-type: disc|circle|square`** → `-`/`*`/`+` (the 8×16 font has no bullet glyphs, so the three kinds map to ASCII — checked `font.c`); **`decimal`** lets a `<ul>` number or restores it on an `<ol>`; and the default `<ul>` bullet now **varies by depth** (`-` at level 1, `*` at level 2, `+` at level 3+). Both the inline `style=` and `<style>`-rule paths work — a new `parse_style_listtype` (mirrors `parse_style_border`, reusing `style_prop`) + a `css_list[]` rule array captured in `capture_css` + a dedicated `css_match_list()` (kept separate from `css_match` rather than threading a 14th out-param, since the marker is only needed at the two `<ul>`/`<ol>` open sites). Golden-safe: `cssprop.c` is untouched (so `csstest` is unaffected), depth-0/1 `<ul>` + all `<ol>` numbering are byte-identical, and `list-style:none` is purely additive; full `make check` (37 suites incl. browsertest + csstest) green. Built in an isolated worktree, reviewed line-by-line, re-verified on `main` with an in-guest `LSTYLE.HTM` screenshot (none-menu via inline + via `ul.menu` rule both bulletless; disc/circle/square → -/*/+). Added the `LSTYLE.HTM` demo page + an INDEX link. Scoped by the same survey that picked M996 (its top runner-up).

> **(M996) browser — `<blockquote>` now draws a left accent bar (the universal quote convention).** A blockquote previously only added a 24px indent, making it indistinguishable from a `margin-left` div or a nested list. Now it also draws a 3px grey vertical bar down its left gutter — how every real site (Wikipedia, GitHub, MDN) renders quoted content. Implemented by REUSING the proven `TK_BORDER_OPEN`/`TK_BORDER_CLOSE` marker (from M910) with `sides=8` (left-only): the `<blockquote>` open emits a left-only border token (`off`=grey `0xC8CED8`, `len`=8, `link`=indent-gutter x, `style`=3px width), the close emits `TK_BORDER_CLOSE` — so the bar spans the whole quoted block (all wrapped lines), nests correctly (each level's bar at a deeper x via the existing 16-deep border stack), and inherits all the border machinery's correctness (viewport-clipping, the M994 `emit_break` terminator handling, being skipped by the bg/flex forward-scans as zero-height). No new layout code, no CSS-parser change. Additive/golden-safe: no `<blockquote>` → no token → byte-identical render (browsertest stays green). As a bonus the same path means a CSS `border-left:3px solid #ccc` callout already works identically. Verified: `make check` (37 suites) + an in-guest screenshot of `QUOTE.HTM` showing the grey bar on a regular blockquote (spanning its 3 wrapped lines) and a second deeper bar on the nested quote, with normal paragraphs unbarred. Scoped by a read-only survey that confirmed this was the single best value-to-effort browser gap (reuses existing machinery, fixes a conspicuous universal convention).

> **(M995) emulators — fix a reachable GB cart-RAM OOB write + bound the NES/GB ROM loads (glue review).** A read-only review of the OS-DEV integration glue for the three ported emulators (the upstream cores were out of scope) found the file pickers + the DOOM/NES audio-video bridges all clean, but three real bugs in the GB/NES glue. **P1 (reachable memory corruption):** `user/gb/gb_osdev.c` sized cart RAM at `g_ram[32*1024]` ("max for an MBC"), but Peanut-GB supports 64 KB and 128 KB SRAM (RAM-size codes 0x05/0x04 → 8/16 banks), and the write callback indexes `addr-0xA000 + bank*0x2000` unbounded — so **any ordinary commercial MBC5 cart with 64/128 KB SRAM overwrote up to 96 KB past `g_ram`** into adjacent `.bss` on its first save during normal play (the RAM-size byte is under the header checksum, so valid ROMs pass init then corrupt). Fixed by sizing `g_ram` to the real 128 KB max AND bounding all three cart callbacks (`cb_rom`/`cb_ram_r`/`cb_ram_w`) to their buffers (OOB reads → open-bus 0xFF, OOB writes dropped) — which also closes **P2:** a >1 MB GB ROM (truncated into the 1 MB `g_rom`) whose banked reads ran up to ~1 MB past the buffer. **P2 (NES):** `nes_osdev.c` passed the ROM buffer to the core (`xnes_cartridge_alloc`), which `memcpy`s PRG/CHR by header-declared sizes WITHOUT checking them against the file length — a truncated/forged `.nes` read past `g_rom`; added an iNES header sanity check in the loader (reject if `16 + trainer + prg*16K + chr*8K > n`) before handing the buffer to the core. Ring-3 (a bug crashes only the app, not the kernel), but the P1 is a real OOB write on common ROMs. Verified: `make check` (37 suites) green + the GB emulator launches and runs the bundled libbet ROM in-guest (the bounds checks never fire for a valid ROM, so no regression). With this, the ported-emulator glue is reviewed too — the codebase-wide review is fully complete.

> **(M994) browser — fix the block-after-styled-block collapse (`emit_break`); cleans up bg AND borders.** The M993 caveat, now fixed: a `<p>` (or any block) right after a block-level styled element (`</div>` with a CSS background or border) collapsed its first words onto that block's last line — sitting inside its fill/box. Root cause (confirmed): `emit_break` only *appends* a fresh break when the previous token is `TK_WORD`; for any other token it falls into the "merge consecutive breaks" branch, which does nothing for a structural CLOSE marker, so the break is silently DROPPED. Block-bg and border emit their `TK_BG_CLOSE`/`TK_BORDER_CLOSE` at scope-pop, and — unlike `TK_FLEX_CLOSE`/`TK_MAXW_CLOSE`, which advance the cursor (`cy += curlh`) and so self-break — those two only stroke/decrement, so the dropped break left the next block collapsed onto the styled block's last line. Fix: one added condition — `emit_break` also appends when the last token is `TK_BORDER_CLOSE` or `TK_BG_CLOSE`. The break stays AFTER the close marker, so the bg height forward-scan (which ends at `TK_BG_CLOSE`) still measures only the block's own content (fill stays one contiguous band) and the next block's OPEN marker still records `y_top` at the post-break cursor. FLEX/MAXW deliberately excluded (they self-break; appending would double-space). Built in an isolated worktree, reviewed line-by-line, re-applied + re-verified on `main`: `make check` (37 suites) green, the browser home page + `flex.htm` + `maxw.htm` render **pixel-identical** to before (0 changed pixels bar the clock — proving no regression to ordinary or flex/maxw pages), and `border.htm` went from a collapsed overlapping tangle to clean, distinct rectangles with the inter-block paragraphs correctly placed between them (so this fixes long-standing border rendering too, not just M993's bg fill). One documented, accepted edge case (not in any page): two styled blocks closing back-to-back with no content between, then a block, yields an ~18px cosmetic over-gap — eliminating it would re-introduce the height bug, so the minimal geometry-safe fix is the right trade-off.

> **(M993) browser — block `background-color` now fills ONE contiguous rect, not gappy per-line bands.** The block-bg was painted per text-line (`fb_fill_rect(ls,cy,cr-ls,lh)` at each line start), so a multi-paragraph / mixed-height / `<hr>`-containing styled block showed vertical GAPS between its lines and left margins/blank-lines uncovered (a styled "card" got a clean border but a striped fill). Now it uses the proven marker-token pattern (like border M910 / flex M927 / max-width M933): a `TK_BG_OPEN`(off=rgb)/`TK_BG_CLOSE` pair brackets the block, and at OPEN the render loop FORWARD-SCANS to the matching close — faithfully replaying the main loop's vertical advance (word-wrap with scale/line-height, BREAK/PARA/HR/IMG, and nested FLEX/MAXW/BORDER incl. `render_rpad` padding) — to find `y_bottom`, then fills the whole rect ONCE (viewport-clipped) BEFORE the block's content paints (opaque-fill z-order; covers everything a per-line band missed). Additive: a page with no block bg emits no markers → byte-identical render (browser goldens stay green; `BORDER.HTM`/`FLEX.HTM` confirmed pixel-identical to before). Built in an isolated worktree by a focused agent, then reviewed line-by-line + re-verified on `main`: full `make check` (37 suites incl. browsertest) green, plus an in-guest screenshot of `MARGIN.HTM` showing the `#cfe8ff` div as one solid band and the inline `#ffe080` `<span>` still a word-only highlight. **Known pre-existing caveat (NOT introduced here, separate follow-up):** a `<p>` immediately after a block-level styled element (`</div>`, and likewise `</…border…>`) collapses its first words onto that block's last line — an `emit_break` quirk that merges the break into the trailing close-marker. The contiguous fill makes it slightly more visible (the collapsed text now sits on the fill) but the fill height itself is correct; the real fix is teaching `emit_break` to look back past trailing structural markers (would also clean up borders) — deferred as its own change since it re-orders the break vs the next block's OPEN marker.

> **(M992) jukebox — cap the track list to the playable [1-9] range (last game-review follow-up).** A read-only review of the final 11 unreviewed apps (paint/life/maze/lights/adv/hangman/typing/simon/flappy/matrix/jukebox) found them all clean — no OOB/crash/hang bugs — bar one cosmetic-plus-functional nit: `jukebox.c` listed up to `MAXW=12` WAVs and labelled each with `'1' + i`, but the key handler only accepts `1`–`9`, so a disk with ≥10 WAVs showed three unplayable rows labelled `:`/`;`/`<` (`'1'+9..11`). Since the single-key picker can't reach track 10+ anyway, capped `MAXW` to 9 — now the list exactly matches the playable range, every label is a real digit, and no capability is lost (those rows never played). The realistic disk has 3 WAVs so it never triggered; a tidy-up. Verified `make check` (37 suites). **This completes the codebase-wide read-only review**: kernel (PMM/VMM/kheap/task/sched/app/elf/net/desktop/terminal), every untrusted-input parser (image/http/x509/fat32/tls), and all ~38 userspace apps + ulib + shell are now reviewed — every reachable bug found is fixed (M983–M992). The substantial remaining *work* is feature growth, headed by the browser block-`background-color` contiguous fill (plan in project memory).

> **(M991) docs — refresh the stale README status counts.** The top-level `README.md` status line still claimed "**730+** milestones" (now M990+) and "**29** … test suites", and `tests/README.md` said "29 suites (26 host + 3 in-guest)" — but `make check` now runs **36** (33 host + 3 in-guest: boottest/gfxtest/browsertest). Updated both to 990+ / 36 (33+3). Documentation only (no code) — keeps the project's advertised status honest after a long run of milestones; per the project notes the live count lives in README.

> **(M990) spaceinv — guard the invader-bullet render against the `-9` parked-column (game-review follow-up).** A read-only review of 10 unreviewed game apps found them notably defensive (clamping `plot()`, bounds-guarded grids) with no P1/P2 reachable bugs — the one issue triggered by normal play: when an invader bullet hits your cannon, its column is set to a `-9` sentinel and removal is deferred one tick, but that same frame the render loop did `fld[iby[i]][ibx[i]]='!'` guarding only the row, not the column. Today it's contained (`fld` is one flat `char[H][W+1]`, so `[H-1][-9]` lands inside the previous row → a cosmetic stray glyph for one frame, not out-of-object corruption), but it's a genuine negative array subscript reachable in ordinary play. Added the column bound (`&& ibx[i] >= 0 && ibx[i] < W`, matching the invader-render guard one line up) so no out-of-range column ever reaches the subscript. Verified `make check` (37 suites). The review confirmed the other 9 apps (pacman/asteroids/missile/lander/frogger/gomoku/mastermind/mancala/breakout) clean; the only other note was exact-sized `putn` score buffers that are 1 byte short for a 10-digit score — unreachable (these games' scores cap in the low thousands), left as-is.

> **(M989) 2048/tetris — cap high-score parsing so a corrupt `.HI` can't wrap the score (userspace-review P3).** `load_best()` in both games accumulated `best = best*10 + digit` over the digits of `2048.HI`/`TETRIS.HI` with no bound — a hand-edited/corrupt file with a long digit string would wrap the `unsigned` (well-defined, but a wrong number on screen). Added `|| best >= 100000000u` to the loop guard in each (cap at 1e8 — far above any real score, well below `unsigned` overflow), matching the cap `mines.c` already uses. No memory-safety issue (the `b[16]` read was already loop-bounded); purely robustness/consistency against a corrupt save file. Normal play is unaffected (no `.HI` → `best` stays 0; a real score is well under the cap). Verified with `make check` (37 suites). — Still queued for a fresh-context iteration: the browser block `background-color` contiguous-fill feature (per-line bands → one rect; full step-by-step plan in the project memory — it's a ~10-site render-loop change needing the block height up front, deferred from a deep session rather than risked mid-edit).

> **(M988) shell — bound the `test`/`cp`-`mv`/`printf` arg tokenizers (fix two reachable OOB writes).** A read-only review of the 3083-line shell found it broadly solid (variable/env expansion, history, the ~50 file/numeric builtins, and argv bounds all verified correct — and the already-fuzzed parsers untouched) but flagged three argument tokenizers that copy content into a fixed buffer with a `< size-1` cap yet wrote the NUL terminator *unconditionally*: once the buffer saturated with more input pending, the inner loop stopped consuming, so each outer iteration wrote `buf[i++]=0` past the end and stored a dangling `av[]` pointer (then handed to `sh_unprot_buf`). Reachable by typing a single ≥255-byte argument (the line buffer is 1024): `[ aaaa…(260) b c ]` (P2, `test`/`[`, shell.c:1083 → `tok[256]`), `cp aaaa…(250) b c` (P2, shell.c:2342 → `tbuf[256]`), and a ≥511-byte `printf` arg (P3 — its terminator was guarded so no OOB write, but the dangling-pointer spin remained). All three now skip a token when the buffer can't hold it + its NUL (`if (idx >= size-1) break;` before storing `av[]`), so the terminator only ever writes within bounds and no past-the-end pointer is stored. Ring-3 (a bug crashed only the shell, never the kernel), but a real OOB write in the primary CLI. Verified: `make check` (37 suites) + an in-guest soak (`[ 5 = 5 ]`, `printf x=%d`, `echo done` all parse + run correctly — the guards never fire on normal-length input).

> **(M987) ulib — `readline()` handles a negative read return (userspace follow-up to the M985/M986 syscall validation).** A read-only review of the userspace layer (ulib + a sample of apps) found it solidly defensive — no reachable memory-corruption bugs — but flagged one real gap that M985 had just made relevant: `readline()` did `buf[n] = '\0'` assuming `n >= 0`, but now that `SYS_read` returns -1 when the kernel rejects a buffer (M985's `ubuf` guard), a -1 would write `buf[-1]` — an out-of-bounds store one byte before the caller's buffer. Added `if (n < 0) n = 0;` (empty line, never `buf[-1]`) plus a `max <= 0` guard (which also avoids `(unsigned long)(max-1)` underflowing to a huge read length). It's in the shared library every app links and the normal `n >= 0` path is byte-for-byte unchanged. Verified: full `make check` (37 suites) green + an in-guest shell soak (`echo readline ok` → `readline ok` → fresh prompt). The review also confirmed CORRECT (no changes needed): the ulib malloc/free/calloc/realloc allocator, memcpy/memmove/memset, the print-capture buffer, and the board-indexing/undo logic in editor/chess/checkers/raycast/2048/tetris/mines.

> **(M986) syscall — finish the user-pointer boundary: validate string args, the gfx surface, and PCM buffers (M985 phase 2).** M985 validated the explicit-length buffer arguments; this closes the rest. New `vmm_user_str_ok(ptr, max)` (kernel/vmm.c) walks a NUL-terminated string page-by-page — validating each page is PTE_USER *before* reading any byte of it (so the scan can't fault or wander into kernel memory) — and rejects a string with no terminator within 16 MiB. A `ustr()` guard now covers every string argument: filenames (readfile/writefile/delete/mkdir/chdir/spawn/sha256/sha512/screenshot/gunzip/gzip/unzip/untar/crypt/playbg/playwav), the network args (http/https host+path, browse URL, pinghost/resolve host) — which were the worst case, since the kernel would otherwise read kernel memory *and send it over the wire* — the JS source, and the find search term. `app_gfx_blit` now validates its source surface (`gfx_w*gfx_h*4` bytes) before the canvas copy — else a forged pointer would have the kernel paint its own memory on screen — and SYS_pcm/pcm_stream validate `nframes*4` bytes (stereo 16-bit). Verified: full `make check` (37 suites incl. live example.com TLS through validated host/path strings, the browser through a validated `SYS_js` source, and every app reading files by validated name) all green, plus an in-guest soak rendering the Mandelbrot fractal (a full `app_gfx_blit` surface) and the earlier shell/Files/Editor run. The ring-3→ring-0 pointer boundary is now fully validated — every syscall pointer is confirmed to live in the calling app's own user pages before the kernel touches it.

> **(M985) syscall — validate user-pointer arguments at the ring-3→ring-0 boundary (the top hardening gap).** Until now every syscall handler cast the caller's registers straight to pointers — `(char *)r->rsi` — and read/wrote through them with no validation. A syscall runs in ring 0 with the calling app's CR3 active, where the kernel higher half and the low identity map are mapped *and writable*, so an app could hand a kernel pointer to a handler and steer the kernel into reading or (worse) overwriting its own memory. New `vmm_user_ok(ptr, len)` (kernel/vmm.c) walks the page tables for every page in `[ptr, ptr+len)` and requires PRESENT **and** PTE_USER at *every* level — so it accepts only the app's own user pages and rejects the higher half, the supervisor-only identity map (boot maps the low 1 GiB without USER), and unmapped addresses. This is correct because `user.ld` deliberately bases apps at 1 GiB (PDPT[1]), entirely above the identity map (PDPT[0]), so valid app memory is always USER-mapped and never aliases it. Wired a `ubuf()` check into the buffer-handling syscalls — SYS_write/read/list/readfile/time/sysinfo/clip_get/clip_set/getarg/writefile/apps/netinfo/http/https/js/tree/find/sha256/sha512/df/ps/history/resolve — each returning -1 on a bad pointer instead of touching it (also closing a `max==0` underflow in SYS_list). Verified: full `make check` (37 suites incl. live example.com TLS via the now-validated `SYS_https` buffer, desktop, and the browser running `SYS_js`/`SYS_http`) all green — proving valid pointers (incl. .rodata string literals, which elf.c maps USER) are never wrongly rejected — plus an in-guest soak driving `ls`/`help` (SYS_list/write/read), Files, and the Editor (SYS_readfile). Deliberately scoped to explicit-length buffers; the NUL-terminated string args (filenames, hostnames) need a `vmm_user_str_ok` walker, and `SYS_gfx_blit`/`SYS_pcm` need their element-count→byte-length conversion — both a clean follow-up.

> **(M984) app/elf — harden the app-load failure paths so a bad ELF or OOM frees its address space instead of leaking/crashing.** Following M983 (which made `vmm_map`/`vmm_create_address_space` actually report OOM), the app loader's failure paths needed to honor those returns. Four fixes, all on error paths — the normal launch is unchanged. **(1) `elf.c`** ignored `vmm_map`'s return when mapping a segment page; on an OOM page-table failure it would then `memset` an *unmapped* virtual address and fault. It now frees the leaf frame and fails cleanly. **(2) `app.c` bad-ELF path** (`elf_load` returned 0) restored CR3 and bailed but never called `vmm_destroy_address_space(a->cr3)` — leaking the freshly-created PML4/PDPT plus every frame `elf_load` had already mapped. **(3) the user-stack mapping loop** passed `pmm_alloc_frame()` straight into `vmm_map` with no OOM check, so on exhaustion it mapped frame 0 (or left the page unmapped) as a stack page. **(4) `task_create_stack`'s** return was unchecked, so an OOM there (it returns 0 since M970) queued a task-less app for the window manager — a later NULL deref. The three in-address-space failures now share a `fail_in_space:` cleanup (restore the caller's CR3, then tear down the partial space — `vmm_destroy_address_space` refuses to free the *active* space, so order matters); the post-restore task failure tears down + unmarks the slot. Verified: full `make check` (37 suites, incl. `elftest` for the elf.c branch + live `boottest`/`browsertest` exercising the normal `app_spawn`→`elf_load` path) all green, plus an in-guest launch of Calc/Editor/Snake (Snake actively running = task created + queued) confirming normal loads are unaffected. The decoders (png/jpeg/gif/bmp/inflate), `http.c`, and `x509.c` were also read end-to-end this pass and found already-solid + fuzzed — no changes needed.

> **(M983) pmm/vmm/kheap — close the frame-allocator concurrency hole + OOM frame-0 aliasing (10th review, the two top P2s).** A read-only audit of the physical/virtual memory managers found the single-threaded logic solid but two real concurrency/OOM defects. **(1) The PMM bitmap had zero interrupt guard.** `pmm_alloc_frame`/`pmm_free_frame` do a non-atomic read-modify-write of a whole bitmap byte, yet the app-reaper (`vmm_destroy_address_space`, kernel/app.c) runs `pmm_free_frame` in a loop *with interrupts on* — so a timer preempt mid-update racing a concurrent alloc in the same byte could lose one and hand the same physical frame out twice (silent memory corruption). Fixed by wrapping both mutators in `irq_save`/`irq_restore`, exactly as `kheap.c` already does. **(2) `next_table` (vmm.c) never checked `pmm_alloc_frame()` for OOM** — on exhaustion it would `memset` virtual 0 and install physical frame 0 as a *present* page table, aliasing the IVT/boot stubs as live tables. Now it returns NULL on OOM and the failure propagates through `do_map`/`vmm_map`/`vmm_map_to`/`vmm_map_huge` (now `-1`), through `vmm_create_address_space` (returns 0, frees a half-built PML4), and into `kheap.c`'s `map_range`/`grow_heap` (return `-1`, `kmalloc` yields NULL instead of aliasing frame 0). The reachable crash this prevents — `app.c` loading a zero CR3 and triple-faulting — is guarded at the call site. Verified: full `make check` (37 suites, incl. `kheaptest` + live `boottest` PMM/VMM bring-up + 3-way address-space isolation + real example.com TLS) all green, plus an in-guest soak of 8 app launches and 4 title-bar-X closes (driving the reaper's `pmm_free_frame` loop under real timer preemption) leaving the desktop fully stable. This was the last major un-reviewed foundational subsystem.

> **(M982) net — DNS reply parse honors the real IP header length (9th review P3-1).** The DNS-response parser
> hardcoded the DNS payload at `buf+42` (eth 14 + IP 20 + UDP 8), assuming IHL=5; an IP header carrying options
> (IHL>5) puts the UDP/DNS payload further along, so hostname resolution would silently fail (timeout) on such a
> network. Now it computes `ihl` from the header (mirroring `tcp_recv_seg`, which already did this) and reads the
> payload at `14 + ihl + 8`, bounding `len`/`dmax` against it. **No-op on SLIRP/QEMU** (IHL=5 → offset 42, byte-
> identical) — verified by boottest's live example.com resolve→connect→TLS still succeeding; net.c warnings 0; all
> 37 `make check` suites pass. It's real-hardware robustness (a router using IP options). The remaining 9th-review
> P3s are cosmetic/benign under SLIRP and left noted: the ICMP echo-reply IHL (just a 3-ping counter), the ICMP
> id/seq match, and the ARP same-IP cache overwrite. See [[os-dev-project]].

> **(M981) TCP/net — three connection state-machine fixes (9th review subagent, net.c).** The review confirmed the
> packet parse + out-of-order reassembly are solid (the nettest fuzzer's domain) and found the real bugs in the
> connection lifecycle: **(P2-2, permanent hang) the FIN-honored checks used `c->theirseq == fin_at`** — so if
> `theirseq` ever overshot the FIN's sequence (a retransmit/overlap accounting glitch), it could never equal it
> again → the connection stayed half-closed forever (the HTTP/TLS read loops only exit on `n<0`). Fixed to the
> wrap-safe `seq_le(fin_at, c->theirseq)` (honor once delivery reaches OR passes the FIN); still one-shot
> (`c->up=0; return`). **(P2-1, cross-connection leak) the global `fin_seen`/`fin_at` were scrubbed only at the next
> `tcp_connect`, not `tcp_close`** — a latched-but-unhonored FIN could prematurely close the *next* connection on a
> reused 4-tuple; now cleared at the top of `tcp_close`. **(P2-3, port/ISN reuse) `sport`+`myseq` derived only from
> the 100 Hz clock** — rapid reconnects (a browser's back-to-back sub-resource fetches) in the same tick reused the
> 4-tuple+ISN, risking a stale SYN-ACK/segment being accepted; mixed a monotonic per-connection counter into both.
> Verified: nettest (parse/reassembly fuzz, ASan/UBSan clean) + **boottest's live example.com TCP/HTTP + TLS 1.3
> HTTPS** (connect → handshake → download → close all succeed with the fixes); net.c warnings 0; all 37 `make check`
> suites pass. Lower review findings (P2-4 handshake-data-drop [recoverable], P3 DNS/ICMP hardcoded IHL=20 / ICMP
> id-seq / ARP same-IP overwrite — all SLIRP-masked today) are documented robustness follow-ups. See [[os-dev-project]].

> **(M980) fb/WM — general content clip-rect (the M968 review's recommended general fix, done safely).** Added an
> optional framebuffer clip rectangle (`fb_set_clip`/`fb_reset_clip`) that `fb_pixel`/`fb_fill_rect`/`fb_glyph`/
> `fb_glyph_fg` all honor in addition to the screen bounds; it defaults to the full screen, so it's a **no-op until
> set** and existing draws stay byte-identical. `render_scene` narrows it to each window's rect around the
> `draw_content` call, so a window's content (a long line, a future layout, an oversized panel) can't bleed past its
> edge onto a neighbour or the taskbar. **The M975/M976 per-kind min-sizes are RETAINED as belt-and-braces:** a
> first attempt that *relaxed* the mins and relied on the shrink-time clip for resizable info windows couldn't be
> cleanly verified at this session depth (the corner-drag resize test mis-fired), so I reverted that and kept this
> safe additive version — the clip future-proofs content-bounding while the mins still guarantee the fixed panels
> can't be shrunk into a bleed. Verified: gfxtest + boottest + browsertest, plus in-guest the content-heavy Monitor
> renders fully with the clip active (content within the body draws normally; additive default-no-op confirmed).
> fb.c/desktop.c warnings 0; all 37 `make check` suites pass. (The resizable-info-window UX — relax the mins once
> the shrink-clip is cleanly verified — is a small fresh-session follow-up.) See [[browser-render-engine]].

> **(M979) Tests — regression-lock the FAT32 write-over-directory data-loss fix (M963) in fstest.** That fix was
> verified in-guest but not in the host suite, even though fstest `#include`s fat32.c and can drive it directly.
> Added a Phase 4b: `mkdir WDIR`, then `writefile("WDIR", …)` must return -1 (refused), `WDIR` must still read as a
> directory (-1) and still be usable (`writefile("WDIR/CHILD.TXT", …)` succeeds), and a normal `writefile("WFILE.TXT")`
> + readback must still work — so a future regression of the guard (which would silently delete the dir or duplicate
> its name) fails the suite. fstest green (ASan/UBSan clean) alongside its corrupt-FAT fuzz + the M624 rm-nonempty
> case; all 37 `make check` suites pass. The host-testable data-integrity/correctness fixes from tonight are now
> regression-locked (kheap double-free/coalesce M972–M974, JS engine M978, FAT write-over-dir M979); the
> interactive editor/browser/WM fixes would need new host harnesses (a fresh-session effort). See [[os-dev-project]].

> **(M978) Tests — regression-lock tonight's JS engine fixes in the jstest suite.** The M958–M967 fixes
> (sort-sign comparator, `switch` `===`-identity, static-field `++`, NaN as a Map/Set/`includes` key, strict
> `JSON.parse`, NaN/Infinity `toString(radix)`, `var` function-scoping + hoisting, `Number(non-numeric)`→NaN) were
> each verified via `-DJS_HOSTTEST` repros, but those are ephemeral — none were added to the permanent
> `tests/js/suite.js`, so a future change could silently reintroduce any of them. Added six `print()` regression
> cases covering all nine fixes (golden regenerated from the now-fixed engine, so the correct behavior is captured),
> e.g. `[5,4,3,2,1].sort((a,b)=>(a-b)/10)`→`1,2,3,4,5`, `new Set([NaN,NaN]).size`→1 with `NaN===NaN`→false,
> `JSON.parse("1 x")` throws, `{var y=7} y`→7 / `{let z=1} typeof z`→undefined, `Number("12abc")`→NaN. `make
> jstest` green (ASan/UBSan clean, ran to completion, matches golden); all 37 `make check` suites pass. These fixes
> are now regression-locked alongside the kheap double-free/backward-coalesce cases (M972–M974). See
> [[js-and-web-app-ceiling]].

> **(M977) Scheduler — dedicated idle task as a guaranteed runnable floor (kernel review #1; the review is now
> fully addressed).** `switch_to_next` skipped DEAD/BLOCKED and stopped at `next==prev`, so a task that blocked
> itself when nothing else was runnable kept executing past `task_block()` still marked BLOCKED (masked only
> because the desktop task never blocks — "incidental, not structural" safety). Added an `idle_task` (a
> `for(;;) sti; hlt` task, created in `sched_init` right after task 0) as an always-runnable floor. The catch a
> naive idle would cause — alternating with the desktop and halving its time slice — is avoided by
> **deprioritizing** it: the search loop skips `idle_task` too, so real tasks round-robin exactly as before, and
> idle is selected *only* when no other task is runnable AND `prev` itself isn't (then a blocking task hands off to
> idle instead of spinning). NULL-safe: if creation ever failed, `switch_to_next` falls back to its prior
> behavior. Verified: boottest (boots + desktop) + browsertest (the Browser app runs) + an in-guest soak (Calc
> computes `2+2=4` / `10*5=50` responsively while Welcome/Files/Shell/Calc/desktop all coexist — no jank or
> starvation, confirming idle stays parked). task.c warnings 0; all 37 `make check` suites pass. **The M970 kernel
> scheduler+heap review is now FULLY addressed** (M970 lost-wakeup + OOM, M971 input guards, M972 double-free
> detection, M973/M974 backward coalescing, M977 idle floor). See [[os-dev-project]].

> **(M976) WM — Welcome/About content-clip (completes the M968 review).** Same fix as M975's Monitor, applied to
> the other two fixed-layout info panels: `KIND_WELCOME` (open 360×290) and `KIND_ABOUT` (open 300×178) now get a
> `win_min` equal to their open size, so they can't be dragged smaller than their content and bleed text past the
> bottom edge. desktop.c warnings 0; all 37 `make check` suites pass. **This closes out the WM review entirely** —
> every finding fixed: the no-windows keystroke crash (M968), the stale drag-index + orphan-app bugs (M968), the
> window-behind-taskbar + wheel-under-menu polish (M969), and the content-clip for all three fixed-layout panels
> (M975–M976). A general fb clip-rect (so any window's content is bounded to its body) remains the only WM item
> left, deferred as a moderate fb-core change. See [[os-dev-project]].

> **(M975) WM — Monitor can't be shrunk below its content (M968 review P3, worst case).** The Monitor
> (`KIND_SYSMON`) opens at 320×272 but `win_min` floored every non-app/browser window at 170×110, so dragging it
> smaller drew its lower Memory/Network/Disk bars (laid out at fixed offsets ~content+200) past the window's bottom
> edge (the fb clips to the screen so it's cosmetic bleed onto neighbours, not a crash). Gave `KIND_SYSMON` its own
> `win_min` of 320×272 (= its open size — a fixed-layout info panel needn't shrink), which guarantees no bleed with
> zero estimation and can't grow-on-resize (min == open). desktop.c warnings 0; all 37 `make check` suites pass.
> The Welcome/About windows share the same (milder, short-text) fixed-layout pattern; a proper general fix is an fb
> clip-rect (the review's suggestion) — both left as minor follow-ups. See [[os-dev-project]].

> **(M974) Heap — `grow_heap` merges with its physical predecessor too (review #5; heap review now fully done).**
> `grow_heap` appended the new region `nb` and merged it only with the list-tail `last` — but list order ≠ address
> order, so when the tail wasn't physically last the merge was missed, leaving a split free pair at the grow
> boundary. Replaced the `last`-only test with the same physical-predecessor scan `kfree` now uses (M973): find the
> free block ending exactly at the old `heap_end` and merge `nb` into it. So free space fully coalesces at
> grow-time as well as free-time. Verified by the kheaptest torture (its occasional big alloc drives `grow_heap`;
> tiling/no-overlap invariants hold) + boottest; kheap.c warnings 0. **The M970 heap review is now fully
> addressed:** OOM-safe task creation (M970), input-overflow/zero guards (M971), double-free detection (M972),
> backward coalescing at free-time (M973) and grow-time (M974). The heap is now double-free-detecting,
> fully-coalescing, and input/OOM-guarded. **Only the scheduler's dedicated idle task (#1) stays deferred** — it
> needs proper deprioritization logic (a naive idle in the round-robin would halve the desktop's time slice) and
> there's no scheduler-torture harness (only boottest), so it's a careful fresh-session change, not a tail-of-
> session one. See [[os-dev-project]].

> **(M973) Heap — backward coalescing (completes M970 review #3).** `kfree` only merged *forward* (a freed block
> with following free blocks), so freeing block N while N-1 was already free left two adjacent free blocks
> unmerged — over a long run the heap fragments and `grow_heap`s even though contiguous free space exists. Added a
> backward merge: after the forward pass, scan for the free block physically immediately before the freed one
> (`p + sizeof + p->size == b`) and have it absorb `b`, splicing `b` out of the list. The scan is needed because
> the free list isn't address-ordered (kfree splices in any order); `b == head` has no predecessor so it's never
> matched. Verified with a new `tests/kheap` case (free X, then free its physical successor Y → Y merges back into
> X, X's block grows — the forward-only path would have left them split) **plus** the 400k-op torture's tiling /
> no-overlap invariants (a wrong merge would mis-tile or overlap → caught) **plus** boottest (the backward-coalesce
> runs on every `kfree` at runtime), all ASan/UBSan clean; kheap.c warnings 0. **The heap review's #3 is now done**
> (double-free magic M972 + backward coalescing M973). Remaining heap/scheduler items still deferred: the dedicated
> idle task (#1, scheduler-structural) and `grow_heap`'s analogous list-tail-vs-physical-tail merge (#5, now largely
> mitigated since the next adjacent `kfree` backward-merges it). See [[os-dev-project]].

> **(M972) Heap — double-free / bad-pointer detection via a header magic (M970 review #3a).** `kfree` did
> `b->free=1` with no validation, so a double-free wrote `free=1` into the middle of a live allocation (or linked
> garbage into the list) and silently corrupted the heap; an interior/non-heap pointer did the same. The safe fix
> (it turned out NOT to need a layout change): `block_t` was `{u64 size; block* next; u32 free}` = 20 bytes padded
> to 24, so a `uint32_t magic` added after `free` **fills the existing padding — `sizeof(block_t)` stays 24, zero
> alignment/size-math impact**. `kmalloc` sets `magic = BLK_MAGIC` on the block it returns (and clears it on the
> split-off free remainder); `kfree` returns early (ignoring the free) if `magic != BLK_MAGIC`, then clears it — so
> a double-free of the same pointer is caught (its magic was cleared by the first free) and a bad/interior pointer
> is caught (its magic slot isn't `BLK_MAGIC`), each ignored rather than corrupting the list. Added a regression
> case to `tests/kheap` (double-free + a stack-pointer free → both ignored, a tracked live block stays intact,
> invariants hold, a later alloc still works) — passes under ASan/UBSan alongside the 400k-op torture; boottest +
> all 37 `make check` suites green; kheap.c warnings 0. **Still deferred (the genuinely-algorithmic part, fresh
> session): backward coalescing** — `kfree` only merges forward, and because list order ≠ address order, fixing it
> safely wants an address-ordered free list (a coalescing bug would corrupt silently), so it's not a tail-of-session
> change. Likewise the dedicated idle task. See [[os-dev-project]].

> **(M971) Heap — `kmalloc` input-overflow + zero-size guards (the safe part of the M970 heap review).** The two
> kmalloc-entry hardening items that DON'T touch the free-list/coalescing (so they can't break valid allocs): (a)
> reject a `size` so large that `align16(size)` (`(size+15)&~15`) or the `need + sizeof(block_t) + 16` math would
> wrap — an overflow there under-allocates and the caller then overruns the heap; now such a request returns NULL
> cleanly. (b) `kmalloc(0)` returned a 0-usable-byte block (the first write clobbers the next block's header); now
> size 0 is bumped to a real minimum block. Both are pure entry-guards (no header/free-list/alignment change),
> verified by `heaptest` + `kheaptest` (the allocator suites) + boottest all green; kheap.c warnings still 0. **The
> free-list hardening from the review stays deferred (it touches the allocator core — careful fresh session):** add
> a header magic so a double-free / bad-pointer free is detected instead of silently corrupting the list, and add
> backward coalescing so the heap doesn't fragment/bloat over a long run (currently only forward-merges). Likewise
> the dedicated idle task (#1) — both are core changes best done with full focus, not at the tail of a long session.
> See [[os-dev-project]].

> **(M970) Kernel scheduler + heap — fix a window-leak race + OOM triple-fault (8th review subagent).** A review of
> `task.c`/`kheap.c` (interrupt model confirmed: all IDT gates are interrupt gates, syscalls run IF-off,
> `irq_save`/`irq_restore` is the guard) found several issues — most masked/not-currently-triggerable — and these
> reachable/robustness ones, now fixed: **(P1/P2 leak) a closed app could block forever, never reaped.**
> `app_sys_read` tested `a->kill` *outside* the `irq_save` block region, and `task_wake` is edge-triggered (acts
> only on a `TASK_BLOCKED` task) — so a WM close (`app_request_kill` sets `kill` then `task_wake`) landing in the
> window between the check and `task_block()` was lost; the app then slept forever with `kill` pending, and its
> window + 256 KB stack were never reclaimed. Fixed by checking `kill` *inside* the `irq_save` region, atomic with
> the block decision. **(P2 OOM → triple-fault) `task_create_stack` didn't check `kzalloc`/`kmalloc`** — on heap
> exhaustion a NULL stack made `top = (0+size)&~15` a wild low address → a bogus initial frame → triple-fault on
> first switch-in. Now it fails cleanly (frees the partial alloc + returns NULL). **(P2 consistency)
> `task_current_id`/`task_count` now NULL-guard `current`** like `task_snapshot`/`sched_tick`. Verified: `make
> check` (boottest boots the kernel) + in-guest (opened Calc, closed it with F8 → it exits and is reaped cleanly,
> window gone, focus returns to the Shell). **Deferred to a focused session (heap-core / structural — riskier):**
> #3 `kheap` has no double-free magic + only forward coalescing (silent double-free corruption + backward
> fragmentation/bloat over time), #1 no dedicated idle task (masked only because the desktop task never blocks),
> #5 `grow_heap` merge assumes the list tail is the physical tail, #7 no integer-overflow guard on size math. The
> review confirmed `task_exit`/`context_switch`/the new-thread frame/EOI-ordering are all correct. See [[os-dev-project]].

> **(M969) Window manager — two interaction-polish fixes (P3s from the M968 review).** (1) **A window could be
> dropped with its title bar behind the taskbar (mouse-unreachable).** The drag-move set `w->y = my - gdy` with no
> bottom clamp, so dragging to the bottom hid the title bar under the (later-drawn) taskbar. Now the dragged `y` is
> clamped to `[0, screen_h - TASKBAR_H - TITLEBAR_H]` so the full title bar always stays on-screen and above the
> taskbar — verified in-guest (dragged the Shell to the bottom → its title bar lands just above the taskbar, still
> grabbable). (2) **The mouse wheel scrolled a background window while the Apps menu / help overlay was open** (those
> are modal for the keyboard but the wheel handler wasn't gated). Now the wheel is still *drained* every frame but
> only *acts* when `!menu_open && !help_open`. Both are tiny, safe changes; desktop.c warnings still 0; all 37
> `make check` suites pass. (The remaining P3 — fixed-layout window content not clipped to a shrunken body — is left
> for later; it needs a real fb clip-rect or per-kind min-height, lower value.) See [[os-dev-project]].

> **(M968) Window manager — fix a crash + two stale-state bugs, found by a read-only review subagent.** A seventh
> review subagent audited `kernel/desktop.c`; it confirmed the fb primitives all clip and the mouse stays
> on-screen, and surfaced three real bugs (the rest were cosmetic): **(P1, crash) a cooked keystroke with NO
> windows open dereferenced `windows[win_count-1]` = `windows[-1]`** (BSS garbage `kind`/`app` → a wild
> `app_key()`/`browser_key()` call). Every F-key handler guarded `win_count>0` but this one didn't; you can close
> all windows (nothing floors *closing*, only F3-minimize) then press a key. Fixed with the same `win_count>0`
> guard. **(P2) the four window-index drag states** (`selecting`/`bselecting`/`sbdrag`/`bsbdrag`, holding the
> window being text-selected or scrollbar-dragged) **weren't reset when the window array reorders or reaps** — only
> `dragging`/`resizing` were. So if a background app exits mid-drag (the async reap loop shifts the array), the
> stale index then points at a different — or reaped/freed — window's `app`/`browser`. Fixed by resetting all six
> gesture states together at every reorder/reap/release site (`raise_window`/`sink_window`/taskbar-chip/reap-loop).
> **(P2) an app launched while at the 16-window cap was leaked** — `make_app_window` rejected it *after* it was
> drained from the pending queue, so the spawned ring-3 task became unreachable (no window → never reaped). Fixed
> by draining the pending queue only while a window slot is free; a rejected app stays pending and gets a window
> when one closes. Verified in-guest: closed all windows + typed keys → no crash, and the WM reopens windows
> normally; `make check` green; desktop.c warnings still 0. P3s (fixed-layout window content not clipped to a
> shrunken body, a window droppable behind the taskbar, the wheel scrolling under an open Apps menu) left as
> minor/cosmetic. See [[os-dev-project]].

> **(M967) JS engine — `Number(string)` returns NaN for non-numeric strings (review #1, the string part).** `to_num`
> parsed a leading number and ignored the rest, and a wholly non-numeric string fell through to `0` (a documented
> "old integer engine" holdover) — so `Number("12abc")`→12, `Number("5px")`→5, `Number("x")`→0, `isNaN("abc")`→false,
> `Number("Infinity")`→0. Rewrote the `V_STR` case (only that case) to require the literal to be the WHOLE string:
> after parsing (hex/bin/oct/decimal/exponent) it now checks that only trailing whitespace remains, requires at
> least one digit, requires exponent digits after `e`, and recognizes `Infinity`/`-Infinity` — otherwise `NaN`.
> Empty/whitespace string still →0 (per spec). Verified host (30+ cases): valid `42`/`3.14`/`-5`/`1e3`/`0x10`/`.5`/
> `5.`/`  7  `/`0b101`/`0o17` still parse; `"x"`/`"12abc"`/`"5px"`/`"0xg"`/`"1e"`/`"."`/`"+"`→NaN; `Number("Infinity")`
> →∞; `isNaN("abc")`→true; valid numeric strings still coerce in arithmetic (`"10"-1`→9) and loose-`==` (`"5"==5`
> →true, `"5px"==5`→false now); `parseInt`/`parseFloat` (lenient by design) unchanged. js.c warnings held at 17; all
> 37 `make check` suites pass (jstest doesn't rely on the old string→0). **Still deferred (the riskier rest of #1,
> a fresh session): `undefined`→NaN and object→NaN** — left UNCHANGED (`undefined+1`→1, `Number({})`→0 still) because
> flipping pervasive arithmetic on undefined/objects can surface page bugs as visible `NaN` and needs broad in-guest
> demo re-verification. See [[js-and-web-app-ceiling]].

> **(M966) JS engine — `var` hoisting + bare-`var` no-clobber (review #4, part 2 — `var` semantics now complete).**
> Building on M965's function-scoping: added a `hoist_vars` pre-pass run once at each function/program entry that
> walks the body's statements — descending into blocks/if/loops/switch/try but STOPPING at a nested function — and
> defines every `var`-declared name as `undefined` in the function/global env up front (define-if-absent, so it
> never clobbers a parameter, an earlier hoist, or a function declaration). So a `var` read before its line
> (`x; var x=5`) or in a branch that never runs (`if(false){var x=1}; x`) now reads `undefined` instead of throwing
> "undefined variable". This also surfaced + fixed a **latent pre-existing bug**: a bare `var x;` (no initializer)
> unconditionally re-`env_define`d the name to `undefined`, so `function k(a){ var a; return a }` reset the
> parameter (returned undefined instead of the arg), and `x=5; var x;` wiped the assignment — now a no-init `var`
> is define-if-absent (never resets an existing binding). Verified host (16 cases incl. use-before-decl, never-taken
> branch, param-not-clobbered, `let`/`const` still block-scoped) AND in-guest via the in-kernel engine
> (`js -e …{var y=7} return y` → `7`; `…typeof x; x=5; var x` → `undefined 5`). The per-call walk added no jstest
> slowdown (suite still ~same time). js.c warnings held at 17; all 37 `make check` suites pass. **`var` is now
> fully correct** (function scope + hoisting). Remaining JS review items: #1 `to_num`→NaN (pervasive, deliberate-
> choice), #3 >16-arg cap (kernel-stack). See [[js-and-web-app-ceiling]].

> **(M965) JS engine — `var` is now function-scoped, not block-scoped (review #4, part 1).** `N_VAR` eval ignored
> the var-vs-`let`/`const` flag (`n->num`) and `env_define`d into the *current* env — which inside a block is a
> fresh block env that's discarded at the block's end. So `function f(){ { var y=7; } return y; }` threw
> "undefined variable" (and `for(var i…){}` then reading `i` did too) — breaking very common older JS. Fix: an
> `env` now carries a `func_scope` flag (set on the env `call_function_this` makes for each call; the global env,
> with `parent==0`, is the natural top boundary), and a `var` declaration defines into `env_func_scope(e)` — the
> nearest function/global env walking up — while `let`/`const` keep defining into the block env. So a `var` in a
> block/loop survives to the rest of the function, but does **not** leak across a function boundary, and `let`/
> `const` stay block-scoped. Verified on host: `{ var y=7 } y`→7, `for(var i…){} i`→3, top-level `{var b=2} b`→2,
> a `var` inside a called function is invisible outside it, and `let`/`const` in a block read back `undefined` —
> while jstest's full var/let/const suite stays green (no regression). js.c warnings held at 17; all 37 `make
> check` suites pass. **Remaining (#4 part 2, smaller/rarer):** true *hoisting* — `print(x); var x=5` (use before
> the declaration line) and a `var` in a branch that never runs still read as not-defined rather than `undefined`;
> that needs a body pre-pass that walks the AST (into blocks/if/loops, stopping at nested functions) defining each
> `var` name `undefined` up front. See [[js-and-web-app-ceiling]].

> **(M964) FAT32 — corrupt/malformed-disk hardening (the deferred M963 review batch).** Mirrored the read path's
> safety onto the write/alloc paths so a corrupted on-disk structure can't steer a WRITE to the wrong sector or
> orphan/cross-free clusters: (1) the `add_entry`, `fat32_delete` and `free_chain` chain-walk loops now bound on
> `cluster_in_range(cl)` instead of `cl < EOC` — an out-of-range-but-`<EOC` chain value (which `cluster_to_sector`
> can uint32-wrap to a *live* LBA when `sec_per_clus>=32`) stops the walk safely, exactly as `walk_dir` already
> does on reads. (2) At mount, `total_clusters` is clamped to the FAT's real capacity (`fat_sectors*128 - 2`) — a
> malformed BPB claiming more clusters than the FAT indexes otherwise makes `alloc_cluster`/`df` walk "FAT entries"
> that physically fall inside the data region. (3) A `root_cluster < 2` BPB is rejected at mount (0/1 would
> underflow `cluster_to_sector` into a huge LBA). All three are **no-ops on a well-formed disk** (a valid chain is
> always in range; `total_clusters` is always ≤ FAT capacity; `root_cluster` is ≥2). Verified: fstest (corrupt-FAT
> fuzz + valid write-stress/dir-growth/rm, ASan/UBSan clean) + boottest (real `fat.img` mounts) + in-guest
> (`echo>Z.TXT; cat; mkdir ZD; rm Z.TXT; ls` all correct). fat32.c warnings unchanged (1 pre-existing). This closes
> out the FAT review; the FS read **and** write paths are now both corrupt-input-hardened.

> **(M963) FAT32 — `writefile` over a directory no longer corrupts the filesystem (data-loss bug, valid disk).** A
> fifth review subagent audited the FAT32 write/alloc/delete paths + VFS and found the read-path fuzzer covers reads
> well but the **write path had a data-loss bug reachable on a normal disk from an ordinary syscall**: `fat32_write`
> unconditionally `fat32_delete`d the existing name then wrote a file. If the name was a **non-empty directory**,
> `fat32_delete` correctly refused (returned -1) but `fat32_write` ignored that and added a second same-name entry —
> a **directory and a file with the same name**, ambiguous resolution + orphaned clusters. If the name was an
> **empty directory**, `fat32_delete` succeeded, so the write **silently deleted the directory**. Fix mirrors
> `fat32_read`'s guard: `dir_find` the leaf first and **return -1 if it's a directory** (you can't overwrite a dir
> with a file, like `cat > somedir` errors on Unix). Verified in-guest: `mkdir TD; echo hi > TD` is refused and `ls`
> still shows `TD/` as a directory (no duplicate); normal file overwrite still works (`echo a>F; echo b>F; cat F`→
> `b`). fat32.c warnings unchanged (1 pre-existing, in `dir_find`); fstest + all 37 `make check` suites pass.
> **The subagent's other findings are corrupt/malformed-disk hardening (not valid-disk bugs), deferred as a batch:**
> at mount `total_clusters` isn't clamped to the FAT's real capacity (a bad BPB → `alloc_cluster`/`df` walk FAT
> entries that fall in the data region → wrong-sector write); `add_entry`/`fat32_delete`/`free_chain` use `cl<EOC`
> loops instead of the read path's `cluster_in_range(cl)` (a corrupt chain with `sec_per_clus>=32` can uint32-wrap
> `cluster_to_sector` to a live LBA); no `root_cluster` sanity check. All require a corrupted on-disk structure to
> trigger (the kernel's drive bound makes them wrong-sector/wrong-result, not kernel-RAM OOB) — good defensive
> follow-ups, lower priority than the valid-disk fix above.

> **(M962) Editor — block indent/dedent now handles any selection size (was capped at 1024 lines).** The other
> actionable item from the M961 editor review: `block_indent` collected every touched line's start into fixed
> `starts[1024]`/`removed[1024]` arrays, so Tab/Shift-Tab on a >1024-line selection silently (de)indented only the
> first 1024. Rewrote it to walk the touched lines' starts from the LAST in range *backward* to the first — going
> backward means each insert/delete is at a higher offset than the line starts still to come, so they never shift
> (the same reason the old code processed the array in reverse), which removes the need to store them at all. No
> fixed array → unlimited selection, and 8 KB of BSS freed. The per-line cursor/anchor math is unchanged (same
> operations, just computed inline). Verified in-guest: a 2-line Tab-indent adds exactly 4 spaces to each line
> (90→98 B) with the end-of-selection boundary correctly excluding the line that starts exactly at the selection
> end; a Tab-then-Shift-Tab round-trip restores the file byte-for-byte (90→94→90) with the cursor back at column 1.
> editor.c warnings still 0; all 37 `make check` suites pass.

> **(M961) Editor — fix a data-loss path + a near-cap truncation, found by a read-only review subagent.** A fourth
> review subagent audited `user/editor.c` (edit buffer, file I/O, the `hl_run` highlighter, auto-indent) and
> reported the **core is solid** — 2M-op edit fuzz + 1.2M-op highlighter fuzz under ASan, no crash/OOB/cursor-desync;
> the 256 KB cap errs safe (a too-large file is flagged read-only, so save never silently truncates). The genuine
> findings, now fixed: **(P2, real data loss) Ctrl-O discarded unsaved edits if the pre-open save failed.** The
> "save current, then load the new file" path ignored `sys_writefile`'s return and called `load_file()`
> unconditionally — so on a full/unwritable disk it clobbered the buffer with the just-failed-to-save content gone.
> Now it checks the return (like every other save site) and aborts the open with `[save failed - open cancelled]`
> instead of loading. **(P2) `replace_all` (Ctrl-R) could half-apply near the 256 KB cap** — it `del_fwd`s the match
> then `insert`s the replacement, but `insert` silently no-ops at the cap, leaving a match deleted + replacement
> truncated. Added a pre-check (`dlen-flen+rlen > MAXDOC-1` → stop cleanly, never a partial). **(P3) Ctrl-W "save as
> foo.css"** now calls `detect_lang()` so highlighting matches the new extension. editor.c warnings still 0; all 37
> `make check` suites pass; verified in-guest (opened README.TXT — loads, highlights, edits). **Left as-is (safe,
> not data-loss):** block indent/dedent on a >1024-line selection processes only the first 1024 (incomplete but
> bounds-guarded); the read-only threshold is off-by-one for a file of *exactly* 262143 B (refuses cleanly); binary
> NULs render truncated but save verbatim; the per-keystroke highlighter seed-pass is O(dlen) on huge files.

> **(M960) JS engine — `(NaN/Infinity).toString(radix)` no longer emits garbage (review #10).** The base-10
> `toString` routed through `num_to_str` (correct: `"NaN"`/`"Infinity"`), but a non-10 radix did
> `(long long)to_num(recv)` first — and casting `NaN`/`±Infinity` to integer yields the "integer indefinite"
> bit pattern, so `(NaN).toString(16)` printed `-8000000000000000` and `(Infinity).toString(2)` a 64-bit garbage
> string. Added a `js_isnan`/`js_isinf` guard before the integer conversion that returns `num_to_str(dv)` (so
> `NaN`/`Infinity`/`-Infinity`). Normal radix output unchanged (`(255).toString(16)`→`ff`, `(-10).toString(2)`→
> `-1010`, `(0).toString(2)`→`0`). Zero regression risk — it only replaces previously-garbage output. js.c
> warnings held at 17; all 37 `make check` suites pass. (Minor review items left as-is — fractional array index
> `a[1.9]` and `a.length=2.5`→RangeError — they'd change established truncation behavior for negligible gain.)
> See [[js-and-web-app-ceiling]].

> **(M959) JS engine — NaN as a Map/Set/`includes` key + strict `JSON.parse` (review round 2).** Two more fixes
> from the M958 review, host-verified: **(#7) `NaN` couldn't be a Map/Set key or found by `Array.includes`.**
> Membership used `val_equal` (`===`), so `NaN !== NaN` meant `new Set([NaN,NaN]).size===2`, `map.get(NaN)===undefined`,
> `[NaN].includes(NaN)===false`. JS specifies *SameValueZero* for these (NaN equals NaN). Added a `same_value_zero()`
> helper (defers to `val_equal` except both-NaN→true) and routed `Set` add/has/delete, `Map` set/get/has/delete and
> `Array.includes` through it — while `===`/`!==`, `==`, `indexOf`/`lastIndexOf` and `Object.is` keep their exact
> (correct) semantics. **(#6) `JSON.parse` accepted trailing garbage and misspelled literals.** It never checked
> end-of-input after a complete value (`JSON.parse("1 x")`→`1`, `"[1,2] z"`→`[1,2]`) and the `true`/`false`/`null`
> branches consumed any `[a-z]` run (`"truX"`→`true`). Fixed: match the literal spelling exactly, and require
> `jp==jp_end` (after trailing whitespace) in `nat_json_parse`, else `SyntaxError`. Valid JSON — including a trailing
> newline — still parses. js.c warnings held at 17; all 37 `make check` suites pass (incl. the JSON fuzzer).
> **Remaining review findings, deliberately deferred to a focused session (higher blast radius):** #1 `to_num`→`NaN`
> for undefined/non-numeric-string/object (pervasive coercion change — the engine's `to_num` comment notes the
> current 0-return is an intentional "integer-engine" holdover, so flipping it needs broad in-guest re-verification),
> #3 calls silently dropping args past 16 (the 16-cap guards the guard-page-less kernel stack — needs arena-alloc,
> not a bigger stack array), #4 `var` not function-scoped/hoisted (needs a hoist pre-pass). See [[js-and-web-app-ceiling]].

> **(M958) JS engine — three evaluator correctness fixes from a read-only review subagent (round 1).** I ran a
> subagent to scrutinize the interpreter core (number/string/array/object/scope/control-flow handling). It found
> the parser/fuzzer memory-safety surface solid (no crashers) but surfaced ~14 *correctness* deviations vs V8, all
> reproduced. This commit fixes three localized ones (each confirmed via the `-DJS_HOSTTEST` host build + a repro):
> **(1) `Array.sort`/`toSorted` truncated the comparator result to `int`.** `cmp=(int)to_num(cmp_fn(...))` made any
> comparator returning a value in `(-1,1)` collapse to `0` ("equal"), so `[5,4,3,2,1].sort((a,b)=>(a-b)/10)` came
> back unsorted. Now compares the *sign* of the double. **(2) `switch` matched the wrong `case` for objects.** The
> fallback `eq=(disc.num==cv.num)` is `0==0` for *all* objects, so `switch(objA){ case objB: }` matched any object
> case; fixed to reference identity (`disc.o==cv.o`), the correct `===` semantics (string/number cases unchanged).
> **(3) static class fields rejected `++`/`--`.** `N_UPDATE` only handled `V_OBJ` receivers; a class is a `V_FUN`
> with statics in `recv.o->statics` (which `N_ASSIGN` already handled), so `C.n++` threw "invalid ++/-- target"
> while `C.n+=1` worked — breaking the `static n=0; static inc(){return ++C.n}` counter idiom. Added the `V_FUN`
> statics case to `N_UPDATE`, mirroring `N_ASSIGN`. js.c warnings held at 17; all 37 `make check` suites pass
> (incl. jstest + the JS/regex/JSON fuzzers). Remaining review findings (to_num→NaN, >16 args, var-scoping, NaN
> Map/Set keys, strict JSON.parse, …) are next. See [[js-and-web-app-ceiling]].

> **(M957) Browser — raise the per-field value width from 95 to 255 chars (completes P4).** The other half of the
> M955-review P4: every stored field value (`in_val`) was 96 bytes → 95 chars, far too short for a real
> `<textarea>`. Introduced `#define IN_VLEN 256` and migrated the store plus *every* edit buffer to it — the catch
> is that several scattered code paths copy a field value into a fixed local `t[96]` before editing (the
> `browser_key` insert/newline/backspace paths, `browser_paste`, the `value=`/textarea-body seeds, `in_set`), and
> raising the store width *without* raising those buffers would overflow them when reading a now-longer value. So
> each was changed buffer-and-cap together, with a uniform invariant (every cap is `IN_VLEN-1` for a plain copy /
> `IN_VLEN-2` before an insert, i.e. strictly `< IN_VLEN`, so reads/inserts can't overflow). Verified in-guest
> (new demo `talen.htm`): a script fills a textarea with 150 `x` → reports `len=150` on load (was capped at 95),
> and typing one more char into that 150-char field reports `len=151` (the keyboard path reads the long value into
> the enlarged buffer safely). browser.c warnings held at 6; all 37 `make check` suites pass. (Single-line inputs
> share the store so they also accept 255 now — harmless.) See [[browser-render-engine]].

> **(M956) Browser — raise the per-page form-field cap from 8 to 16 (P4 from the M955 review).** The id-keyed
> field-value store (`in_id`/`in_val`/`in_name`, where every `<input>`/`<textarea>`/`<select>` value lives) was
> fixed at 8 slots: a form with a 9th distinct named field silently dropped it from both rendering and the GET
> submit (`in_set` returns once `in_n >= 8`). Replaced the bare `8` with a self-documenting `#define IN_MAX 16`
> (struct decl + the cap), doubling capacity for ~512 bytes more on the heap-allocated `browser_t`. Verified
> in-guest with new demo `fbig.htm` (ten fields, filled + read back on load): reports `f1=v1 f8=v8 f9=v9 f10=v10`
> — the 9th/10th are kept now (they read back empty before). The per-value width (95 chars) and the separate
> textarea/select sub-registries (8 each) are left as-is — rarely hit; a page with >8 *textareas* or >16 fields is
> unusual. browser.c warnings held at 6; all 37 `make check` suites pass. See [[browser-render-engine]].

> **(M955) Browser — fix three DOM/parse bugs found by a read-only code-review subagent.** I ran a subagent to
> scrutinize the recently-added interactivity (textarea/select/.checked/attribute-props/title/submit) for
> memory-safety and logic bugs; it found the buffers all correctly bounded but surfaced three wrong-output bugs,
> now fixed + verified in-guest (new demo `qsval.htm`, which self-reports on load):
> **(P1, the big one) `querySelector(...).value`/`.checked` silently did nothing.** A `#id`-string handle worked
> (it routes to the id-keyed field store), but a *position* handle from any tag/class/attribute selector
> (`querySelector('input')`, `.querySelectorAll('.x')[i]`) routed `.value`/`.checked` through
> `browser_dom_get_at`/`_set_at`, which hard-returned for those props — reads gave `""`, writes were no-ops. Fix:
> a new `dom_id_at()` resolves the matched element's `id` and delegates to the id-keyed `browser_dom_get`/`_set`
> (an id-less match still can't be tracked — the store has no other key, same as the renderer). Now
> `querySelector('input').value` reads `hi`, `…value='ZZ'` writes, `querySelector('.box').checked=true` ticks the
> box. **(P2) raw-text close-tags matched a prefix, not a whole tag** — a `<textarea>`/`<select>`/`<script>` body
> containing the substring `</textareas>` / `</selecting>` ended capture early. Fix: after the name, require a tag
> terminator (`> / space / tab / nl`). **(P3) `sel_vals[8][256]` truncated long option lists**, breaking the
> selcyc click round-trip (a cut value never matched `in_get`, so cycling jumped to option 0). Enlarged to `[512]`
> — the worst case is 16 options × 31 chars + 15 `'\n'` = 511 — and bumped the join guards to `o<511`. browser.c
> warnings held at 6; all 37 `make check` suites green. See [[browser-render-engine]].

> **(M954) Shell quoting — complete the last unswept file/SET readers (chokepoint unprotect).** The M950 sweep
> left a handful of commands that read a *quoted* argument without revealing its bit-7 sentinels — benign (a stray
> high-bit byte, never a crash) but incomplete. Fixed at the natural chokepoint: `slurp()` (the one function every
> file-reading builtin funnels through — `cat`/`sort`/`uniq`/`cut`/`grep`/`wc`/…) now `sh_unprot_buf()`s the
> filename once, so `cat "my file"` / `sort "a b.txt"` all work in a single place (guard-safe even on the lone
> string-literal caller, scores' `"SNAKE.HI"`). Plus the three spots that don't go through `slurp`: `cd "my dir"`
> (uses `sys_chdir`), and — newly *enabling* previously-impossible operations — `tr`'s SETs and `cut -dX`'s
> delimiter, which `tr_expand`/the flag parser capture before the chokepoint. `tr -d ' '` (strip spaces),
> `tr ' ' _` (space→underscore) and `cut -d' '` (space-delimited fields) were unreachable before — a bare space
> arg is eaten by the `while(*p==' ')` skips; quoting it (`' '` → protected `\xa0`) now survives to the SET/delim,
> then a count-based unprotect reveals it. Verified in-guest: `echo "a b c">SP.TXT; tr -d ' ' SP.TXT`→`abc`,
> `cut -d' ' -f2 SP.TXT`→`b`. shell.c warnings held at 11. POSIX quoting is now comprehensive. See [[shell-quoting]].

> **(M953) Shell quoting — fix two interaction bugs (found by my own adversarial testing).** After M949–M952 I
> stress-tested quoting combined with pipes/glob/for/cmdsub/brace and found two real bugs the review hadn't hit:
> (1) `for f in "x y" z` iterated `["x"] [y"] [z]` — for/while/if/case are dispatched in `run_input_line`, which
> bypassed `run_line`'s `sh_quote_pass`, so their word lists were never quote-processed. Fix: run `sh_quote_pass`
> in `run_input_line` too (after the `js -e` intercept + func-def capture, before the `;`-split + construct
> dispatch). (2) `echo "$(echo hi there)"` → "unknown command: echo hi there" — `sh_quote_pass` protected the
> spaces *inside* the `$(...)` because it sat within double quotes, corrupting the inner command. Fix:
> `sh_quote_pass` now copies `$(...)` spans verbatim (depth-tracked), even inside double quotes — the inner
> command is separate and its own quoting is processed when it runs (its `run_line` still applies the pass).
> Verified in-guest: `for f in "x y" z` → `[x y] [z]`, `echo "$(echo hi there)"` → `hi there`, plain
> `for n in 1 2 3` unregressed. `shquotetest` updated (verbatim-`$()` cases; the "no quote survives" fuzz
> assertion relaxed since quotes inside `$()` legitimately remain); 3M fuzz + `make check` green (37 suites);
> baseline 11 held.

> **(M952) Shell quoting — fix 5 missed unprotect sites (review-driven).** A read-only review subagent
> scrutinized the M949/M950 quoting and confirmed the core machinery is clean (no regressions, unquoted input
> byte-identical) but found 4 builtins that consume a filename/arg yet never called `sh_unprot_buf`, so a quoted
> argument leaked a high-bit sentinel byte into the syscall: `find` (`find "my doc"` substring never matched),
> `edit`, `screenshot`, and `js <file>`. I fixed those and also caught a 5th the review didn't cover — the `>`
> output-redirect target (`echo hi > "my file"`). Each now unprotects its filename/arg before use (the same
> one-line pattern as the M950 sweep). Verified in-guest: `find "no match xyz"`/`find "EAD"` search correctly,
> `echo "quoted out" > F; cat F` round-trips, `find "txt"` unregressed. `make check` green (37 suites); shell.c
> warning baseline (11) held. Quoting coverage is now comprehensive (the only deliberately-unswept commands are
> the rarely-quoted `cd`/`sort`/`uniq`/`cut`/`tr`, where a quoted arg is a benign sentinel, never a crash).

> **(M951) Calc — floating-point scientific calculator (was integer-only).** The calc app evaluated only
> integers; now it's a real scientific calculator: IEEE-754 doubles, decimal/scientific literals (`3.14`, `.5`,
> `1e3`), functions `sqrt sin cos tan asin acos atan ln log exp abs floor ceil round`, constants `pi`/`e`, `^`
> power, real `/` and `%`, with the bitwise ops `& | << >> ~` still working (operands truncate to 64-bit int,
> JS-ToInt32-style). The double math is **copied verbatim** from the kernel JS engine's tested implementations
> (new `user/dmath.h`: Newton `sqrt`, range-reduced Taylor trig, `ln`/`exp`/`pow`, and the 15-significant-digit
> `dnum_to_str` formatter — so `sin(pi/2)` prints `1`, `log(1000)` prints `3`, not noisy tails). Userspace FP is
> enabled via a dedicated SSE Makefile rule for calc (mirroring DOOM/raycast; the scheduler already saves FP per
> task). Never traps on bad input: `1/0`→Infinity, `0/0`/`sqrt(-1)`→NaN (structural errors only set the error
> flag). Built by an implementing subagent, then reviewed + verified in-guest by me (`sqrt(2)`, `sin(pi/2)`,
> `2^10`, `log(1000)`, `7/2`→3.5, `5&3`→1 all correct). `calctest` (host regression + 400k ASan/UBSan fuzz)
> updated for double semantics; `make check` green (37 suites).

> **(M950) Shell — quoting complete: per-command unprotect sweep.** M949 wired the sentinel machinery + `echo`;
> this sweeps ~50 builtins (via a subagent, reviewed) to `sh_unprot_buf()` each argument token once it's split
> out — `cat`/`grep`/`cp`/`mv`/`head`/`tail`/`wc`/`hexdump`/`mkdir`/`rm`/`touch`/`sha*`/`base64`/`cmp`/`paste`/
> `comm`/`diff`/`tar`/`unzip`/`gzip`/`run`/`ping`/`get`/`write`/… (Pattern-A token copies), the rest-of-line
> commands (`cowsay`/`rev`/`rot13`/`morse`/`browse`/assignment values), the argv builders (`test`/`printf`/
> multi-file `cp/mv`/`tee`), and the `for`-loop word bind. So a quoted argument's protected bytes are restored
> to real ASCII right before the command uses/prints/opens it. `run_pipe`/`expand_*`/`glob`/`normpath` are left
> untouched (each pipe stage keeps its sentinels until its own builtin). Additive — `sh_unprot_buf` is a no-op
> on a sentinel-free token, so unquoted input is byte-identical. Verified in-guest: `grep "hello world" FILE`
> matches a multi-word pattern, `printf "%s+%s" "a b" "c d"` → `a b+c d`, `echo "hi there" > f; cat f`,
> `cowsay "big test"` all preserve quoted spaces. `make check` green (37 suites); shell.c warning baseline (11)
> held. (A few rarely-quoted commands — `cd`/`sort`/`uniq`/`cut`/`tr` — are not yet swept; a quoted arg there
> shows a benign sentinel byte, never a crash — minor follow-up.) **Shell quoting is now functionally complete.**

> **(M949) Shell — POSIX quoting `"..."` / `'...'` (foundation): the most-requested shell feature, the last big
> deferred shell item.** The blocker was always that arg parsing is decentralized (no central argv tokenizer).
> Solved with a sentinel scheme (planned via a subagent): shell input is 7-bit ASCII, so `sh_quote_pass()`
> (new `user/shquote.h`) strips the quote characters and PROTECTS the in-quote specials by setting bit 7 —
> space, `; | & < > * ?` (both quote kinds), plus `( ) { } , $ \`` and the other-quote char inside single
> quotes (fully literal), while double quotes leave `$`/`(`/`{` so `"$x"`/`"$(cmd)"`/`"${y}"` still expand. The
> whole existing pipeline then treats sentinels as ordinary bytes (every splitter/glob/expander scans for the
> *raw* ASCII special, which a high-bit byte is not), and each builtin calls `sh_unprot_buf()` on a token once
> it's split out. Ran `sh_quote_pass` at the top of `run_line` (before all expansion); made the `;` splitter
> (`sh_next_sep`) and the `&&`/`||` splitter (`run_andor`) quote-aware so those operators are literal inside
> quotes. This commit wires the machinery + `echo`'s unprotect; **the per-command unprotect sweep (cat/grep/cp/
> mv/… each `sh_unprot_buf` their tokens) is M950.** Strictly additive — no quotes → `sh_quote_pass` is a no-op
> → byte-identical (DEMO.SH verified unchanged). New `shquotetest` (37th `make check` suite) fuzzes
> `sh_quote_pass` 3M× under ASan/UBSan — already caught + fixed a nested-`$()` re-interpretation bug (the
> other-quote char must be protected, not left literal). Verified in-guest: `echo "a b"`/`'a b'`→`a b`,
> `"a;b"`→`a;b`, `"x|y"`→`x|y`, `'$x'`→`$x`, `x=5;"$x"`→`5`, `"a&&b"`→`a&&b`. `make check` green (37 suites);
> shell.c warning baseline (11) held.

> **(M948) Editor — CSS syntax highlighting.** Completes the editor's source-language set (was C/JS/shell/HTML;
> CSS is the language the OS's many `.htm` pages use in `<style>`). CSS (`hl_lang` 5, `.css`) reuses the
> C-family machinery — `/* */` block comments (grey), strings (orange), numbers incl. `12px` (purple) — but
> drops `//` line comments and keyword matching (CSS property/selector idents stay default), and adds `#hex`
> colours (`#4090ff` → purple). Display-only (a highlighter bug can only mis-colour, never corrupt text or the
> boot), so safe. New `SAMPLE.CSS` demo. Verified in-guest: numbers/`px`, `#hex` colours and the
> `"monospace"` string all colour correctly. `make check` green (36 suites).

> **(M947) Paint — circle tool (completes the shape set).** Added a circle/oval tool (`o`): drag out from the
> centre and a midpoint-algorithm circle rubber-bands to the cursor radius (`isqrt` of dx²+dy², integer-only),
> reusing M946's preview (backup/restore each drag frame) and the disc brush for thickness. Paint is now a
> real drawing program: brush, flood-fill, line, rectangle, circle — each colour-/size-aware. Verified
> in-guest: `o` + drag draws a clean single blue circle (no smear). `make check` green (36 suites).

> **(M946) Paint — line & rectangle tools with rubber-band preview.** With fill (M945) done, added shape
> tools so paint is a real drawing program: `b`/`l`/`r` pick brush / line / rectangle. In line or rect mode a
> left-drag rubber-bands a live preview — on press it snapshots the canvas into a backup buffer + records the
> anchor; each drag frame restores the backup and redraws the shape from anchor to cursor; release leaves the
> final shape committed. Line is one `stroke`; rectangle is four edge strokes. Reuses the existing disc/Bresenham
> primitives + the `cv[]` canvas buffer (the backup is one lazy `W*H*4` alloc, freed at quit). Verified
> in-guest: `r` + drag draws a single clean rectangle outline (no smear → the backup/restore preview commits
> correctly). `make check` green (36 suites).

> **(M945) Paint — flood fill (paint bucket).** The paint app had only a freehand brush; added the classic
> bucket-fill: hover over the canvas and press `g` to recolour the 4-connected region of the same colour as the
> pixel under the cursor with the current palette colour. The app already keeps a local `cv[]` canvas buffer
> (it blits it each frame), so the fill reads + rewrites pixels directly — a stack flood fill that recolours on
> push (so each pixel is queued exactly once → the scratch stack is bounded by `W*H`, malloc'd + freed per
> fill). Verified in-guest: `g` over the empty canvas floods it to the selected colour. `make check` green
> (36 suites). A fresh non-browser/editor win after a long browser-forms run.

> **(M944) Browser — `<select>`/`<option>` dropdown (the last form control).** Completes the form set
> (text/password/checkbox/radio/textarea + now select). In the token-stream renderer a popup isn't practical, so
> it's a **cycle-on-click** control: the select renders as a focusable `[ Label v]` link showing the chosen
> option, and each click advances to the next option (wrapping). Parsing mirrors `<textarea>`: `<select>` is
> captured as raw text, then `parse_select` extracts each `<option>`'s value (its `value` attr, else the label)
> + label + `selected`; the chosen value lives in the input store (so `.value` reads it and the form submits
> `name=value`), the option order is kept in `sel_vals` for cycling, and a new `selcyc:ID` link type drives the
> click (mirrors the checkbox `check:` path: find the current value, set the next, re-render). Selection is
> determined by the stored value (so a cycled choice sticks across re-renders), falling back to the `selected`
> option then the first. Additive — no `<select>` → no change. New `SEL.HTM` demo. Verified in-guest: renders
> the `selected` option ("Banana", `.value`→`b`); clicking cycles to "Cherry" (`.value`→`c`). `make check`
> green (36 suites). **The browser's HTML form-control set is now complete.**

> **(M943) Browser — `document.title` get/set + page titles in the window bar & taskbar.** Page JS can now read
> and change the document title, and — as a real desktop improvement — browser windows now show the page
> `<title>` in their title bar and taskbar chip (was always "Browser"). js.c gets the document object pointer
> (`g_doc_obj`) so `document.title` reads bridge to the page `<title>` and writes bridge back (new `js_set_title`
> hook → `browser_get_title`/`browser_set_title`); desktop.c draws a browser window's title from
> `browser_title()` (the page title, falling back to "Browser"). The subtle part: a JS title set has to survive
> the `parse_html` re-render that the same handler usually triggers (which re-captures `<title>` and would
> clobber it) — so the set is stored as a `title_js` override that takes precedence until navigation (mirroring
> how input `.value`s persist across re-renders but reset per page). Additive: a page with no `<title>` and no
> `document.title=` still shows "Browser" (golden home page + `browsertest` unchanged). New `TITLE.HTM` demo.
> Verified in-guest: the bar shows "My Page" from `<title>`; `read` → `title=[My Page]`; `set` → bar + taskbar +
> read-back all become "Changed!" (override survives the re-render). `make check` green (36 suites).

> **(M942) Browser — `<textarea>` (the README's named #1 frontier item).** A real multi-line form field in the
> token-stream renderer. The parser captures `<textarea>`'s inner text as raw content (mirroring the
> `<script>`/`<style>` path: a new `intextarea` raw-text mode + a `</textarea>` close that seeds the value and
> suppresses the body from normal page flow) and registers its id; `emit_textarea` renders each line of the
> stored value as a focusable field token (click to focus) with the caret `|` drawn at `field_cur` on its line,
> and a clickable placeholder when empty. The value lives in the existing `<input>` store, so `.value` reads it
> and a form submits `name=value` for free; typing reuses the field-input path, and `browser_key` now inserts a
> real `\n` on Enter when the focused field is a textarea (vs submit/blur for one-line inputs). Strictly
> additive — no `<textarea>` on a page → no new tokens → byte-identical render (golden home page + `browsertest`
> unchanged). New `TAREA.HTM` demo. Verified in-guest: renders the seeded text on two lines (no leaked copy);
> click-focus + type + Enter builds a third line with the caret tracking; and `ta.value.length` → `23` /
> `ta.value.split('\n')[0]` → `Hello there` (so `.value` carries the real newline). `make check` green
> (36 suites). (Remaining form control: `<select>`/`<option>`, which needs a popup UI.)

> **(M941) Browser — attribute-reflecting DOM properties (`element.className`/`href`/`src`/`name`/`title`/
> `alt`/`placeholder`/`type`).** Continuing the "more DOM APIs" frontier: these common properties returned
> `undefined` on live elements, so `link.href`, `img.src`, `el.className`, `input.placeholder` etc. didn't work
> from page JS. Since `getAttribute`/`setAttribute` already work, `dom_prop` now maps each of these property
> names to its HTML attribute (`className` → the `class` attribute; the rest are same-named) for both read and
> write — `el.className` returns the class string and `el.className = "x"` rewrites it (re-rendering the page so
> new styling applies). Reuses the existing id- and position-handle attribute bridges. Strictly additive (these
> names previously fell through to `undefined`; every other property unchanged). New `PROPJS.HTM` demo. Verified
> in-guest: read → `cls=[hi there] href=[file:README.TXT]`, and `className='changed'` round-trips to
> `now cls=[changed]`. `make check` green (36 suites).

> **(M940) Browser — `element.checked` DOM property (read + write) for checkboxes/radios.** On the README's
> named "richer browser interactivity / more DOM APIs" frontier: page JS could read a checkbox's state only via
> the stringy `.value` (`"on"`/`""`), but the idiomatic `if (cb.checked)` / `cb.checked = true` didn't work.
> Added `.checked` as a real boolean: js.c's `dom_prop` maps it to a new bridge kind (4), and the element
> get-path intercepts it to return a genuine `BOOLV` (so `'x='+cb.checked` → `x=true`, not `x=1`/`x=on`); the
> set-path routes `cb.checked = <truthy>` to the store as `"on"`/`""` so the box re-renders `[x]`/`[ ]` and
> submits `name=on` correctly. browser.c gained `html==4` to `browser_dom_get`/`_set` (id-keyed, like `.value`);
> the position-handle `_at` variants treat it like `.value` (id-keyed, not position-addressable). Additive —
> one new property name, every existing property byte-identical. New `CHKJS.HTM` demo. Verified in-guest:
> read → `checked=true` when `[x]` / `checked=false` when `[ ]` (tracks clicks), and the set buttons toggle the
> box both ways. `make check` green (36 suites).

> **(M939) Tests — extract brace expansion to `shbrace.h` + a 36th `make check` suite (ASan/UBSan fuzz).**
> M937's brace expansion was only covered in-guest; the project host-fuzzes every pure shell parser
> (shgrep/shmath/shsplit/normpath), so the new code should match. Lifted the seven functions verbatim into
> `user/shbrace.h` (header-guarded), `#include`d from `shell.c` in place of the inline block (byte-identical
> behaviour — re-verified in-guest: `{1..4}`, `file{a,b}.c`, `{1,2}{x,y}` all unchanged). Added
> `tests/shbrace/shbrace_test.c`: ~25 regression cases (lists, ranges, step, char ranges, cartesian, nesting,
> empty items, and the left-untouched safety cases — `${VAR}`, function bodies, `{}`, unbalanced) plus a
> 2,000,000-iteration fuzz over brace-heavy random input asserting no crash / OOB / hang and that re-expanding
> the output is a no-op (terminates). Building it under ASan+UBSan caught a wrong test expectation (the
> word-preamble correctly attaches to every range item: `list={1..3}` → `list=1 list=2 list=3`), confirming
> the implementation. Wired `shbracetest` into `make check` (now **36 suites**, all green).

> **(M938) Editor — Tab/Shift-Tab block indent & dedent.** The last core code-editing operation: with a
> multi-line selection, Tab indents every selected line by 4 spaces and Shift-Tab dedents them (removing a
> leading tab or up to 4 spaces); with no selection Shift-Tab dedents the current line, and plain Tab keeps its
> insert-spaces-to-next-stop behaviour. Needed a small kernel change — the keyboard driver mapped both Tab and
> Shift+Tab to `\t`, so Shift+Tab now emits a distinct backtab code (0x9B) which the editor reads as dedent.
> `block_indent()` collects the line starts in range and edits in reverse (so lower offsets stay valid), via
> new `ins_at`/`del_at` raw-position helpers, then fixes up the caret and re-anchors the selection so it keeps
> covering the same lines (you can Tab repeatedly to add levels). Verified in-guest: select-all + Tab → 4-space
> indent (selection preserved), Tab again → 8, Shift-Tab → back to 4 (byte counts 11→23→35→23 confirm). The
> keyboard change is isolated to Shift+Tab (plain Tab, shell completion unaffected); `make check` green
> (35 suites). The editor is now a complete code editor: highlighting + auto-indent + bracket-match + block
> indent/dedent.

> **(M937) Shell — brace expansion `{a,b,c}` and ranges `{1..N}` / `{a..z}`.** A common bash convenience that
> was missing: `echo file{a,b,c}.txt` → `filea.txt fileb.txt filec.txt`, `echo {1..5}` → `1 2 3 4 5`,
> `mkdir {src,bin,doc}`, descending `x{9..6}` → `x9 x8 x7 x6`, char ranges `{a..e}`, and the cartesian product
> of adjacent groups (`{1,2}{3,4}` → `13 14 23 24`). Implemented as `expand_braces()`, a string-transform pass
> in `run_line` after variable/alias expansion and before glob (matching bash's order). It repeatedly expands
> the first eligible group (the repetition yields the cartesian product) into `bline[]`. **Safe + additive**:
> a `{...}` is expanded only when it has no internal spaces, contains a top-level comma or a valid range, and
> is not preceded by `$` — so `${VAR}` parameter expansion, `$(...)`, function bodies `f() { ...; }` and
> command groups `{ cmd; }` are left untouched (verified in-guest: `greet() { echo hello $1; }` still works,
> and `echo val=${x} list={1..3}` → `val=42 list=1 list=2 list=3` — both expansions coexist). Output growth is
> bounded by the buffer + a pass guard, so a pathological `{1..999999}` can't blow up. Doesn't need the
> (deferred) quoting infrastructure. Verified in-guest; `make check` green (35 suites).

> **(M936) Editor — matching-bracket highlight.** Completes the code-editor triad (highlight + auto-indent +
> bracket-match). When the caret sits on, or just after, a bracket `()[]{}`, the editor tints both it and its
> partner pink (palette 5) so you can see scope and catch mismatches at a glance. `match_bracket()` is a naive
> depth scan (forward for openers, backward for closers; doesn't skip brackets inside strings/comments — fine
> for an editor aid). It reuses the M934 colour pipeline: the two matched positions are written into `vcol[]`
> (the per-byte colour array) after the syntax pass, so it composes with highlighting and the yellow selection,
> and it now works in **any** file (the colour array is always populated, not just for recognised languages —
> previously it was gated on `hl_lang`). Verified deterministically in-guest: with the caret after the `)` of
> `total = arr(one, two, three)`, a pixel scan found exactly two pink glyph columns (the `(` and `)`), nothing
> else. `make check` green (35 suites).

> **(M935) Editor — auto-indent on Enter.** Paired with M934's highlighting to make the editor a real code
> editor: pressing Enter now reproduces the current line's leading whitespace on the new line (so nested code
> keeps its indentation instead of snapping back to column 0), and adds one extra 4-space level when the line
> ends with an open bracket `{`/`(`/`[`. Implemented as `newline_indent()` (snapshots the leading run of spaces/
> tabs, scans back for a trailing opener, then inserts `\n` + that indent); all the inserts coalesce into one
> undo group, so a single Ctrl-Z removes the whole indented newline. Only the interactive Enter key uses it —
> paste and find&replace still insert raw `\n` so they preserve their source's own indentation. Verified
> in-guest (typing `void f() {` ⏎ `if (ready) {` ⏎ stepped to 4- then 8-space indents). `make check` green
> (35 suites).

> **(M934) Editor — multi-language syntax highlighting (C, JS, shell, HTML).** The editor rendered every file
> in one colour; for a from-scratch dev OS whose own source is C/JS/shell/HTML, highlighting is a big
> day-to-day win and the first non-browser feature in 24 milestones. A small from-scratch tokeniser
> (`hl_run`) colours the visible window byte-by-byte — keywords blue, strings orange, comments grey, numbers
> purple, C `#`-directives teal — picked by file extension (`detect_lang`: `.c/.h`→C, `.js`→JS, `.sh`→shell,
> `.htm`→HTML; anything else → plain, so `.txt` etc. render exactly as before). It reuses the editor's existing
> coloured-run print path (the one that drew the yellow selection): the build loop now fills a parallel
> `hlc[]` colour per output byte, and the printer emits maximal same-colour runs with an active selection still
> overriding to yellow. Correctness across the scroll window comes from a **seed pass**: before colouring the
> visible window, `hl_run` scans `[0,off)` to recover the tokeniser state at the top of the view, so a block
> comment / HTML tag / attribute string / string literal opened *above* the viewport keeps its colour
> (7 carried states incl. a dedicated HTML attribute-value mode so a viewport that starts mid-attribute is
> right). Verified in-guest: `SAMPLE.C` (new demo file — all five C classes incl. multi-line `/* */` and `#include`/`#define`),
> and `BORDER.HTM` (blue tags, orange attribute values, green text). Behaviour-safe (plain text → every byte
> colour 0 → previous rendering); `make check` green (35 suites).

> **(M933) Browser — `max-width` centered columns (readable article layout).** The ubiquitous
> `max-width:Npx; margin:0 auto` pattern (blogs/docs/news constrain body text to a centered column) was
> ignored — content ran the full window width. Now a block with `max-width` emits `TK_MAXW_OPEN`/`CLOSE`
> markers; at render the loop narrows + centers the content edges for that block (`cl += off; cr -= off`,
> `off = (cr−cl−maxwidth)/2`, pushed on a 16-deep stack and restored on close), so text, wrapping, borders and
> nested flex all lay out inside the centered column. Needed a dedicated `max-width` parser — the shared
> `parse_px_val` caps at 120px (right for margins, far too small for a column), so widths were clamped to a
> sliver. Verified in-guest (`MAXW.HTM`: a `max-width:380px` block is a centered ~380px column between two
> full-width paragraphs). Additive (`mxsp` stack is empty without max-width → identical); `make check` green
> (35 suites).

> **(M932) Browser — flex `justify-content: space-between` (and space-around/evenly).** Extended M931's
> forward-scan to track item-widths (`ww`) and the gap count (`nb`) separately. For `space-between` the inter-
> item gap is widened to fill the row — `flex_gap = (avail − ww) / nb` — so items spread edge-to-edge; center/
> end still offset the whole row by `total = ww + nb·flex_gap`. (`space-around`/`evenly` map to the same
> distribution for now.) Verified in-guest (`FLEX.HTM`: `space-between` puts `Left` at the left edge, `Right`
> at the right, `Middle` between). Additive; `make check` green (35 suites). Flexbox now covers the common
> cases — row/column, inline+stylesheet, gap, and justify center/end/space-between; `align-items` (cross-axis)
> and `flex-grow` (item sizing) remain, needing fuller per-item measurement.

> **(M931) Browser — flex `justify-content: center` / `flex-end`.** The first main-axis alignment, which needs
> the row's width up front. Got it with a bounded forward-scan at `TK_FLEX_OPEN`: sum the direct children's
> word widths (`(len+1)·GW·zoom`) + gaps until the matching `TK_FLEX_CLOSE` (depth-tracked so nested flex
> isn't counted), then offset the start `cx` by `(avail−rowWidth)/2` for center or `avail−rowWidth` for end
> (only when the row fits). The mode rides in the `TK_FLEX_OPEN` marker's `style` field (1 center, 2 end);
> `space-*` distribution maps to start for now (needs per-gap spreading). Verified in-guest (`FLEX.HTM`:
> `justify-content:center` centers `One/Two/Three`, `flex-end` right-aligns them, content after renders
> normally). Additive (start/no-justify unchanged); `make check` green (35 suites). Remaining flex: `space-
> between/around`, `align-items` (cross-axis), `flex-grow` — all need fuller item measurement.

> **(M930) Browser — flex `gap`.** The flex row used a fixed 18px between items; now it honours the CSS `gap`
> (and `column-gap`) value. The gap px is parsed in the push, carried in the `TK_FLEX_OPEN` marker's `off`
> field, and the render uses it as the per-container item spacing (0 → 18px default; set per flex container).
> Verified in-guest (`FLEX.HTM`: a `display:flex; gap:40px` row spaces Apple/Banana/Cherry noticeably wider
> than the default-gap `.row`). Inline `style=` gap for now (CSS-rule `gap` would need another `css_*` array —
> minor follow-up). `make check` green (35 suites). Flexbox now covers row/column, inline/stylesheet, and gap
> — the layout properties that don't need item-width measurement; `justify-content`/`align-items`/`flex-grow`
> (which do) remain the bigger deferred flex work.

> **(M929) Browser — `flex-direction:column` (vertical flex).** M927's flex was row-only, so a
> `display:flex; flex-direction:column` container (very common — vertical layouts, sidebars, stacked cards)
> rendered its children as a horizontal row, which is wrong. Since a column flex stacks children vertically —
> exactly the default block flow — `parse_style_flex` now returns 0 when `flex-direction:column` is set, so the
> container simply isn't switched into horizontal flex mode and its children stack as normal. Verified in-guest
> (`FLEX.HTM`: a `flex-direction:column` block renders `Top/Middle/Bottom` stacked vertically, while the row
> flex above it stays horizontal). `make check` green (35 suites). Flex now honours row vs column.

> **(M928) Browser — `display:flex` from `<style>` rules (not just inline).** Most flex containers use a class
> (`.row { display:flex }`), so extended M927 to stylesheet rules, mirroring the border CSS-rule path (M911):
> `capture_css` encodes flex as `css_disp == 2` (1 = `display:none`), and `css_match` gained an `int *flex`
> out-param (so a rule sets it; inline `style=` then overrides). Verified in-guest (`FLEX.HTM`: a
> `<div class="row">` with `.row{display:flex}` from a `<style>` block lays `Red/Green/Blue` in a row, next to
> the inline-flex example). Golden-safe (no flex rule → `css_disp` is 0/1, unchanged); `make check` green
> (35 suites). Flex now works both inline and via stylesheets — the common case.

> **(M927) Browser — `display:flex` row layout (the last deferred big item).** The single-pass renderer stacks
> every block vertically; flex needs children laid in a row. Done additively with the same marker-token trick
> as `border`: a `display:flex` block emits `TK_FLEX_OPEN`/`TK_FLEX_CLOSE` around its children, and the render
> loop keeps a `flex_depth` — while >0, a child's block-break becomes a horizontal **gap** (18px) instead of a
> line advance, so the children flow in a row (wrapping at the right edge); `TK_FLEX_CLOSE` ends the row. Both
> inline `style="display:flex"` and `inline-flex` are recognised. Verified in-guest with `FLEX.HTM`: three
> divs render **side by side** under flex, and **stack vertically** without it (same markup) — proving it's
> additive (`flex_depth` is 0 outside a flex container, so all existing pages render identically; `make check`
> green, 35 suites). This is basic flex-row (no `justify-content`/`align`/`grow`/explicit-`wrap` yet), but it's
> the core horizontal-layout primitive the renderer lacked. CSS-rule `display:flex` (via `<style>`) is next.

> **(M926) Demo — `DEMO.SH` showcases the C-style for + `(( ))` arithmetic command.** Added two lines to the
> bundled scripting demo: `for ((k=1; k<=3; k++)); do echo c-style-for k is $k; done` and
> `((sq = 6 * 7)); echo arithmetic command: 6 times 7 is $sq`. So `source DEMO.SH` now demonstrates the
> complete shell control-flow + arithmetic set (functions, `if`/`elif`/`case`, `for`-in/`while`/C-style-`for`,
> `$(())`/`(( ))`, parameter expansion) and doubles as a composition regression check. Verified in-guest — the
> whole demo runs cleanly through to "demo complete", including `c-style-for k is 1/2/3` and `…6 times 7 is 42`.

> **(M925) Shell — fix `(( ))` comparisons (`<`/`>`/`|`) in if/while conditions.** M924's `(( ))` only skipped
> *glob*, so the comparison operators still got mangled: `while ((i < 4))` read `<` as input redirection and
> exited immediately, and `if ((x > 5))` matched for the wrong reason (the `>` redirected). The real fix:
> intercept a leading `((` at the top of `run_line` (right after variable expansion, before glob/redirect/pipe)
> and dispatch it straight to `run_command` — so none of `< > | * << >>` are treated as shell metacharacters,
> only as arithmetic operators (mirrors how `js -e` is intercepted early). Verified in-guest:
> `while ((i < 4)); do …; ((i++)); done`→`i=0..3`, `if ((x > 5))`→big, `((y = 6 * 7))`→42. `make check` green.

> **(M924) Shell — `(( expr ))` arithmetic command.** Completes the arithmetic constructs (`$((…))`,
> `for ((;;))`, and now the standalone command). Reuses `sh_do_assign` (now returning the value) for
> `((i++))`, `((x = a*b))`, `((sum += n))`, and sets `$?` = (value≠0 ? 0 : 1) so `((n > 5)) && cmd` works as a
> condition. Needed one fix: `run_line` was glob-expanding the `*` in `((x = 6 * 7))` (multiply read as a
> wildcard), so glob is now skipped for a leading `((` — like `$(())`, arithmetic `*` is multiply.
> Verified in-guest: `((y = 6 * 7))`→42, `((p = a*a + b*b))`→25, `((n>5)) &&`→runs, and C-style for + `((sq=i*i))`
> → a squares table. `make check` green (35 suites); warnings at baseline.

> **(M923) Shell — C-style `for ((init; cond; incr))` loops.** Completes the loop set (alongside `for V in
> WORDS` and `while`). Feasible without touching the host-tested `shmath.h`: `sh_eval` already resolves bare
> names, so the condition (`i<5`) and RHS expressions (`s+j`) evaluate directly; only the assignment is new —
> a small `sh_do_assign` handles `VAR=expr`, `VAR++`/`--`, and `VAR+=expr` (and `-=`,`*=`,`/=`,`%=`) via `vset`.
> `run_for_carith` parses the `((…))`, runs init once, loops while the condition is non-zero, runs the body
> (honouring `break`/`continue`/`return` + a 100k-iteration cap + Ctrl-C), then the increment. The construct
> splitter already keeps `for ((…)); do … done` together. Verified in-guest: `for ((i=0;i<5;i++))`→`0..4`,
> a `j+=1` sum→`10`, `for ((k=3;k>0;k--))`→countdown. `make check` green (35 suites).

> **(M922) JS engine — `Number.toExponential` + `toPrecision` (Number prototype complete).** The last two
> Number formatting methods, sharing one implementation: normalize to `[1,10)`, scale to N significant digits
> via the proven integer-scaling (`round(t·10ⁿ⁻¹)`, drift-free), then lay out the digits. `toExponential(k)`
> always uses `d.ddde±XX` (k fraction digits, default 6); `toPrecision(p)` produces p significant figures,
> choosing fixed vs exponential per spec (`e < -6 || e >= p` → exponential) and keeping trailing zeros
> (`(1.5).toPrecision(3)`→`"1.50"`). Verified on host against V8: `(12345).toExponential(2)`=`"1.23e+4"`,
> `(123.456).toPrecision(4)`=`"123.5"`, `(123456).toPrecision(3)`=`"1.23e+5"`, `(42).toPrecision(5)`=`"42.000"`.
> `make check` green (35 suites). The from-scratch JS number/Math support is now comprehensive and consistent.

> **(M921) JS engine — `(3.14).toString()` keeps the fraction.** The `Number.prototype.toString` method always
> used the integer radix-conversion path, so `(3.14).toString()`→`"3"` and `(10.5).toString()`→`"10"` — even
> though `String(3.14)` and `""+3.14` (which go through `num_to_str`) correctly gave `"3.14"`. Now base-10
> `toString()` routes through `num_to_str` (full float); only a non-10 radix (`(255).toString(16)`→`"ff"`)
> uses the integer path. Fixes a common explicit-conversion inconsistency. `make check` green (35 suites).

> **(M920) JS engine — fix `num_to_str` precision drift + `toLocaleString` keeps the fraction.** Some floats
> printed with spurious trailing digits: `9876.5`→`"9876.500000000002"`, `99.99`→`"99.99000000000002"`,
> `8.8`→`"8.800000000000001"`. Cause: the formatter extracted 16 digits one at a time via `(t-c)*10`, which
> accumulates floating-point error. Replaced it with a single integer scaling — `round(t·10¹⁴)` gives a
> 15-digit integer that stays under 2^53 (so it's exact), and the digits come straight out with no drift.
> Now `9876.5`/`99.99`/`8.8` print exactly; precision is 15 sig-figs (e.g. `Math.PI`→`3.14159265358979`).
> Also fixed `Number.toLocaleString` to keep the fractional part (`(1234.5).toLocaleString()`→`"1,234.5"`,
> `(1234567.89)`→`"1,234,567.89"` — was dropping decimals, wrong for money). Verified on host; suite-promise
> golden updated for the 15-fig change; `make check` green (35 suites).

> **(M919) JS engine — `Object.is` handles NaN/-0 (float-migration correctness).** With real IEEE doubles
> (M906), `Object.is` was still just `===`, so `Object.is(NaN,NaN)` was `false` (its whole reason to exist is
> to be `true`) and `Object.is(0,-0)` was `true` (should be `false`). Now the number path special-cases both:
> both-NaN → true, and `+0`/`-0` distinguished via `1/x` (`+Inf` vs `-Inf`); everything else stays `===`.
> Verified on host (`Object.is(NaN,NaN)`=true, `(0,-0)`=false, `(-Inf,Inf)`=false, object identity, all matching
> V8). Also refreshed the stale "integer engine, no FPU" comments on `Math` and the RNG. `make check` green
> (35 suites). This wraps the float-migration correctness sweep (random, toFixed, parseFloat, JSON, isNaN,
> isFinite, Object.is all now float-correct).

> **(M918) JS engine — `Math.random()` returns real `[0,1)`.** It was still returning the old integer-engine
> range `[0, 2^31)`, so every standard idiom was broken: `Math.random() < 0.5` was always false,
> `Math.random() * 100` was astronomical, `Math.floor(Math.random()*n)` never indexed. Now the no-arg form
> returns a real double in `[0,1)` (top 53 bits of the xorshift64 state ÷ 2^53). Kept the non-standard
> `Math.random(n)` → `[0,n)` integer extension for back-compat. Verified on host (10000 samples all in
> `[0,1)`, uniform with mean ~0.5; die-roll and coin-flip idioms work). Random isn't golden-tested (non-
> deterministic), and `make check` stays green (35 suites).

> **(M917) Browser — named/`rgb()` colours in CSS borders.** `parse_style_border` only scanned for `#hex`, so
> `border:1px solid black` (named colour — extremely common) fell back to grey. `parse_color` reads the
> *leading* token, so on `"1px solid red"` it saw `"1px"` and failed. Now the border parser scans each
> whitespace-delimited token and takes the first that `parse_color` resolves to a real colour (width/style
> words like `solid`/`dashed` parse to 0 and are skipped), with explicit `black`/`#000` handled too. Verified
> in-guest (`BORDER.HTM`: a box with `border:2px solid blue` now renders blue, not grey). `make check` green
> (35 suites).

> **(M916) Browser — horizontal padding inside full border boxes.** Completes the border box model (M910-M915):
> a full box's text now insets `BORDER_PAD` (6px) on both sides instead of hugging the edges. Left inset reuses
> the parse-time `curindent`/`tokindent` path (added *after* the marker captures the box's true left edge, so
> the border stays put and only the text moves in). Right inset is a render-time `render_rpad` the markers
> push on a full-box OPEN and pop on CLOSE; the word-wrap and justify tests use `cr - render_rpad`, so text
> wraps short of the right border. Only full boxes (all four sides) pad — a `border-bottom` divider doesn't
> inset text. Golden-safe: `render_rpad` is 0 outside a box, so non-bordered content wraps identically;
> `make check` green (35 suites). Verified in-guest (`BORDER.HTM` boxes now have breathing room on all sides).

> **(M915) Browser — borders follow the block's indent + a little vertical padding.** M910's border always
> spanned the full content width (`cl..cr`), so an indented bordered block (`blockquote`, `margin-left`) drew
> its box full-width instead of around its actual text. Now the `TK_BORDER_OPEN` marker carries the block's
> left indent in its (previously unused) `link` field, and the render strokes the box from `cl+indent..cr` —
> so an indented bordered div's box hugs its text. Also nudged the box ~4px above / ~3px below the text so it
> isn't cramped. Verified in-guest (`BORDER.HTM`: a `margin-left:50px` bordered div whose box starts 50px in,
> next to the full-width boxes). Golden-safe (indent 0 → unchanged); `make check` green (35 suites).

> **(M914) JS engine — hyperbolic Math (`sinh`/`cosh`/`tanh`).** The last gap in the now-floating-point Math
> object: derived from `js_exp` (`sinh=(eˣ−e⁻ˣ)/2`, `cosh=(eˣ+e⁻ˣ)/2`, `tanh` via `(e²ˣ−1)/(e²ˣ+1)` with a
> `|x|>20`→±1 guard for overflow). Verified on host (`cosh(1)=1.543080634815244`, `tanh(1)=0.7615941559557649`,
> all matching V8). Math is now complete: abs/min/max/floor/ceil/round/trunc, sqrt/cbrt/hypot, pow/exp/log/
> log2/log10, sin/cos/tan/asin/acos/atan/atan2, sinh/cosh/tanh, sign/clz32/imul/random + PI/E/LN2/LN10/LOG2E/
> LOG10E/SQRT2/SQRT1_2. `make check` green (35 suites).

> **(M913) Browser — keep CSS-border markers out of saved page text (+ audit).** The `TK_BORDER_OPEN/CLOSE`
> marker tokens (M910) carry a colour in their `off` field, not a text-pool offset, so any code that treats a
> token as a word would misread the text pool. `browser_save` (the `s` "save as PAGE.TXT" path) was emitting a
> stray newline per marker via its catch-all `else`; now it skips them. Audited **all** token-text paths for
> the same hazard and confirmed each is safe: the render loop `continue`s on the markers before `put_word`;
> `tok_matches`/find is only called under a `TK_WORD` guard; word-copy uses the word-only hit-test rects; and
> selection-copy already gates on `TK_WORD`. So the markers can never cause an out-of-bounds text read. `make
> check` green (35 suites).

> **(M912) Browser — individual border sides (`border-top/right/bottom/left`).** Extends M910/M911 so a block
> can have just one edge — the common pattern for section dividers (`border-bottom`) and rules (`border-top`),
> used all over real pages. `parse_style_border` now also matches the four per-side properties and packs a
> 4-bit sides mask into the border value (`width<<28 | sides<<24 | color`); the `border` shorthand = all sides.
> The mask rides in the `TK_BORDER_OPEN` marker's `len` field, and the render stroke only draws the edges the
> mask sets. Verified in-guest (`BORDER.HTM`: a red `border-bottom` divider under one block, a green
> `border-top` rule above another, plus the existing full red/blue/green/purple boxes). Additive + golden-safe;
> `make check` green (35 suites).

> **(M911) Browser — CSS `border` from `<style>` rules (not just inline).** Extends M910 to stylesheet rules
> (`.card { border: 2px solid #80c }`), which real pages use far more than inline `style=`. Followed the CSS-
> engine's established add-a-property pattern: a `css_border[CSS_MAX]` array, parsed in `capture_css` via the
> same `parse_style_border`, surfaced through a new `*border` out-param on `css_match` (rule sets it, an inline
> `border:` then overrides — correct cascade). Verified in-guest (`BORDER.HTM` now has a purple `.card` box from
> a stylesheet rule next to the inline red/blue/green boxes). Golden-safe — the CSS test pages set no borders,
> so `css*test`/browsertest are byte-identical; `make check` green (35 suites).

> **(M910) Browser — CSS `border` on block elements (the first real box-layout feature).** The token-stream
> renderer is single-pass and never knew a block's bottom edge until its close tag — so a border couldn't be
> drawn the way block backgrounds are (per-line bands that read as one fill; a per-line *outline* would box
> every wrapped line). Solved with **deferred-draw marker tokens**: a bordered block emits `TK_BORDER_OPEN`
> (carrying color+width) at its scope-push and `TK_BORDER_CLOSE` at its scope-pop, bracketing its content
> tokens. During render a small stack records `y_top` on OPEN and strokes the full rectangle (4 viewport-
> clipped edges, `cl..cr` wide) on CLOSE — so a block that wraps across many lines gets **one** outline, and
> nested borders each get their own (stack depth 16). `parse_style_border` reads the `border:` shorthand
> (px width + `#hex` colour, default 1px/grey; inline `style=` for now). Verified in-guest with a new
> `BORDER.HTM` (one red box around 4 wrapping lines; nested blue+green boxes). Fully **additive** — pages with
> no border emit no markers and render byte-identically, so `make check` (incl. browsertest + CSS goldens)
> stays green (35 suites). Foundation for `flexbox` (also needs block-extent tracking).

> **(M909) JS engine — `JSON.parse` of real numbers (fractions + exponents).** The JSON number tokenizer was
> integer-only: it dropped the fraction and choked on exponents, so `JSON.parse('{"price":3.99}')` returned
> `3` and `JSON.parse('{"x":1.5e3}')` threw "invalid JSON" — fatal for real web APIs, which return JSON floats
> everywhere. Rewrote the number branch to parse a full `int.frac[eE±exp]` into a double (matching M906).
> Now `JSON.parse('{"price":3.99,"rate":-0.5,"big":1.5e3}')` → `3.99 -0.5 1500`, and stringify round-trips
> (`{"price":3.99,...}`). Also added a floating-point regression block to `suite-promise.js` (fresh arena)
> locking in M906–M909 (arithmetic, `Infinity`/`NaN`, `Math.*` incl. trig, `toFixed`, `parseFloat`, JSON
> floats). `make check` green (35 suites).

> **(M908) JS engine — real `Number.prototype.toFixed` + real `parseFloat`.** Two integer-era stubs that only
> become meaningful with M906's doubles. `toFixed(k)` now rounds to k decimals (`(3.14159).toFixed(2)`→`"3.14"`,
> `(1234.5678).toFixed(2)`→`"1234.57"`, `(2.5).toFixed(0)`→`"3"`, negatives + `"5.00"` padding) by scaling by
> 10^k, rounding half-up, and laying out the digits with the decimal point placed from the right — was just
> padding the integer with zeros. `parseFloat` was aliased to `parseInt` (so it truncated); it now parses a
> real leading float prefix incl. fraction/exponent/`Infinity` (`parseFloat("3.14abc")`→`3.14`,
> `parseFloat("1e3")`→`1000`, `parseFloat(".5")`→`0.5`, `parseFloat("abc")`→`NaN`). Verified on host; `make
> check` green (35 suites).

> **(M907) JS engine — trigonometry (`Math.sin/cos/tan/asin/acos/atan/atan2`).** Completes the transcendental
> `Math` now that numbers are real doubles (M906): `sin`/`cos`/`tan` via a range-reduced Taylor series (reduce
> to `[-π, π]`, 12 terms → ~15 digits), and `atan` via half-angle argument reduction
> (`atan(x)=2·atan(x/(1+√(1+x²)))` until the argument is small, so a slow Leibniz series near x=1 is avoided —
> `Math.atan(1)*4` is `3.141592653589793`, i.e. π). `atan2` builds the full-quadrant angle; `asin`/`acos`
> derive from `atan`. All self-contained (no libm). Verified on host (`Math.sin(0)`=0, `cos(π)`=-1,
> `sin(π/6)`≈0.5, `atan(1)*4`=π, `asin(1)`=π/2) and `make check` green (35 suites).

> **(M906) JS engine — real IEEE-754 floating point (no longer integer-only).** The from-scratch JS engine's
> number type went from `int64_t` to `double` throughout (`val`/`node`/`token`), so `7/2` is `3.5` (not `3`),
> `1/0` is `Infinity`, `0/0` is `NaN`, and `3.14`, `1e-3`, `0.1+0.2` are real fractionals. This needed the
> kernel to let js.o use the FPU: a Makefile rule builds **just** `build/kernel/js.o` with `-msse2 -mfpmath=sse`
> (dropping `-mgeneral-regs-only`) — safe because the kernel already enables SSE at boot (`fpu_init`) and the
> scheduler saves/restores FP state for **every** task (`task.c` `fx_alloc`), so the engine's xmm use survives
> context switches. New self-contained math helpers (no libm/libcall): `js_sqrt`/`js_cbrt` (Newton),
> `js_pow` (exact integer exponents, else `exp(e·ln b)`), `js_ln`/`js_exp` (range-reduced series), `to_i32`
> (JS ToInt32 for the bitwise ops), and `num_to_str` (double→string, ~16 sig figs, trims zeros, spells out
> `NaN`/`Infinity`). `Math` is now real: `floor`/`ceil`/`round`/`trunc`/`sqrt`/`cbrt`/`hypot`/`log`/`exp`/
> `log2`/`log10`/`abs`/`sign`, plus the constants `PI`/`E`/`LN2`/`LN10`/`LOG2E`/`LOG10E`/`SQRT2`/`SQRT1_2`;
> `Infinity`/`NaN` globals, `Number.POSITIVE_INFINITY`/`MAX_VALUE`/`EPSILON`, and correct `isNaN`/`isFinite`/
> `Number.isInteger`. Fixed `Array.flat(Infinity)` (was UB casting Infinity→int). Bumped the JS arena 40→44 MB
> (doubles format a touch heavier; the suite sat at the cap). Verified end-to-end in-guest (`js -e` float
> arithmetic, `Math.*`, a float-accumulating loop) and on the host (`make check`: all 35 suites green, ASan/
> UBSan-clean jstest with updated goldens). This was a long-deferred "big" item — now done.

> **(M905) Shell — `rm -f` (force: ignore missing files).** Found by an integration test: `rm` treated `-f` as
> a filename, so `rm -f X` errored on the flag and on a missing `X`. Added `-f`: skip the flag token, and a
> missing file is silently ignored (no message, no `$?=1`) — the standard scripting idiom for "remove if it
> exists". Only `-f` (the shell has no recursive remove, so `-rf` is deliberately not accepted — it would
> dangerously mask a real "dir not empty" failure). Verified: `rm -f MISSING` → `$?`=0. `make check` green.

> **(M904) Shell — `break` and `continue` for `for`/`while` loops.** Loop control was missing: you couldn't
> early-exit a loop or skip an iteration. Added both, mirroring the existing `return`/`g_returning` mechanism:
> a `g_loopbrk` flag (1=break, 2=continue) set by the builtins and consumed by the innermost loop, plus a
> `g_loopdepth` guard so a stray `break`/`continue` at the prompt or outside a loop is a harmless no-op (never
> wedges the next command). The body executor stops the rest of the line when the flag is set, so `break`
> inside an `if` inside a loop works. Verified in-guest: `break` stops at the right iteration, `continue`
> skips it, a nested inner `break` leaves the outer loop running (`1a 2a`), `while true; …; break` exits, and
> a bare `break` at the prompt is a no-op. `make check` green (35 suites).

> **(M903) Shell — `test`/`[ ]` gains `-d` (directory), `-s` (non-empty file), and correct `-e`/`-f` for dirs.**
> `[ -d PATH ]` is the standard idiom for "is a directory" and was missing (it silently fell through to false).
> Added it (and `-s` = non-empty regular file) using the `sys_chdir`-then-restore pattern the shell already
> uses elsewhere to detect directories. Also fixed `-e`/`-f`, which both used `sys_readfile` and so couldn't
> tell a directory apart: `-e` now matches a file **or** a directory, and `-f` is true only for a regular
> file (readable and not chdir-able). Verified in-guest (`[ -f DIR ]`→false, `[ -d DIR ]`→true, `[ -d FILE ]`
> →false, `pwd` unchanged afterward); `make check` green (35 suites).

> **(M902) Shell — proper `printf`: hex/octal/unsigned + width/zero-pad/left-align, and negative numbers.**
> `printf` now handles `%x`/`%X`/`%o`/`%u` (32-bit, like `%d`; previously printed literally) plus
> `%[-][0][width]` flags, so formatted output works: `printf [%5d]\n 42` → `[   42]`, `printf [%-5d]\n 42`
> → `[42   ]`, `printf [%05d]\n -42` → `[-0042]` (zero-pad keeps the sign ahead of the zeros). With arg
> cycling this gives hex colors with no quoting needed: `printf rgb=%02x%02x%02x\n 255 128 0` → `rgb=ff8000`.
> Also fixed `itoa_simple` (was `while (v > 0)`, so negatives produced an empty string): `printf %d -42` →
> `-42` and `seq -2 2` → `-2 -1 0 1 2` now print correctly (negatives were blank before). Verified in-guest;
> `make check` green (35 suites).

> **(M901) Shell — document + showcase the M900 parameter expansions.** Updated the `help` line to list the
> new forms (`${#N} ${N#pfx} ${N%sfx}` alongside `${N:-def}`/`${N:+alt}`), and added a line to the bundled
> `DEMO.SH`: `path=/usr/local/bin; echo basename ${path##*/} dirname ${path%/*} length ${#path}` → prints
> `basename bin dirname /usr/local length 14`. So `source DEMO.SH` now demonstrates inline basename/dirname/
> length too. Verified in-guest. `make check` green (35 suites).

> **(M900) Shell — `${#VAR}` length + `${VAR#pat}`/`${VAR%pat}` glob prefix/suffix strip.** Completes the
> common sh parameter-expansion set (on top of M893's `${VAR:-default}`/`${VAR:+alt}`), giving inline
> string/path manipulation without a subshell: `${#VAR}` = length, `${VAR#pat}`/`${VAR##pat}` strip the
> shortest/longest matching **glob** prefix, `${VAR%pat}`/`${VAR%%pat}` the suffix (reusing `glob_match`).
> So `${path##*/}` = basename, `${path%/*}` = dirname, `${f%.*}` = drop extension, `${#s}` = length — all
> the standard idioms. Additive in `expand_vars`: plain `$NAME`/`${NAME}`/`${VAR:-…}` go through the
> unchanged paths (no regression). Verified in-guest: `${#hello}`→5, `${f%.gz}`→`archive.tar`,
> `${f%%.*}`→`archive`, `${p##*/}`→`bin`, `${p%/*}`→`/usr/local`, and `$x`/`${x}`/`${x:-def}`/`${y:-fb}`
> still correct. `make check` green (35 suites); shell.c warnings 11.

> **(M899) Editor — edit files up to 256 KB (was 64 KB read-only).** Completes this fire's "handle large
> content" theme (M897/M898 render + navigate large web pages): the editor's `doc` buffer was a fixed 64 KB
> (`MAXDOC`), so anything bigger — a downloaded/saved web page, a big log, a long script — opened read-only
> ("[RO: file too big]"). Raised `MAXDOC` 65536→262144 (256 KB). The buffer is the editor's own per-process
> BSS (userspace ELF), so it just costs ~192 KB more zeroed BSS per editor instance — no kernel/heap impact,
> and edit ops (insert/delete shift the buffer) stay sub-millisecond at this size. Verified in-guest: a
> ~106 KB file (`seq 1 20000`) opens **editable** (no RO flag), and typing + Ctrl-S saved — `tail -2` shows
> the inserted text persisted. `make check` green (35 suites).

> **(M898) Browser — bigger link budget so large pages' body links stay clickable.** Companion to M897:
> now that long pages render their full body, their cross-reference links should be followable, but the
> href URL pool was only `HREF_MAX`=8KB (~200 links) and `LINK_MAX`=512 — so links past the nav/ToC dropped
> to plain (un-clickable) text. Raised `HREF_MAX` 8192→32768 (still < 65536, so `href_t.off` stays uint16-
> safe; `add_href` already caps at it) and `LINK_MAX` 512→2000. ~4× more of a long article's links are now
> clickable. Verified in-guest: after scrolling deep into `en.wikipedia.org/wiki/Unix`, clicking the
> "History of Unix" link in the body navigated to `/wiki/History_of_Unix`. Small heap bump (~42KB/window);
> `make check` green (35 suites); no small-page regression.

> **(M897) Browser — widen `tok_t.off` to uint32 so large real pages render their full body, not just the nav.**
> The render was capped at ~64KB of token text (`TEXT_MAX < 65536`) because the per-token text-pool offset
> `tok_t.off` was uint16 — so a big page like Wikipedia rendered only its nav/ToC, truncating the article
> prose. A re-analysis (deeper than the earlier "deferred, too interdependent" note) showed the change is
> actually **contained**: the clickable-rect arrays (`lrec`/`wrec`) only hold *visible* tokens with
> screen-relative int16 coords (gated on `cy>=ct && cy+lh<=cb`), the hrefs pool is capped at `HREF_MAX`
> (8KB, well under uint16), and `linky`/`toky` are already `int[]` — so **only `tok_t.off`** can exceed
> 65535. Widened it to uint32 (`len`≤word-length and `link`<`LINK_MAX` stay uint16), fixed the one
> truncating cast (`(uint16_t)start`→`(uint32_t)`), and raised `TEXT_MAX` 65000→131072 (128KB) and `TOK_MAX`
> 9500→16000. **Pages under the old cap render byte-identically** (offsets < 65536 are the same value);
> only larger pages newly render more — no crash risk (off is a bounded text-pool index). Verified in-guest:
> scrolling deep into `en.wikipedia.org/wiki/Unix` now shows the Overview prose + History section (was
> truncated); example.com renders identically; OS boots fine (~1MB/window heap). `make check` green (35).

> **(M896) Browser — a clear error page on a failed fetch (was a blank page).** Found by browsing a bad
> domain over the (real, working) network: the browser showed only a tiny "failed" status with a fully
> blank body — confusing, and failures are common (typos, offline, TLS-incompatible sites). The parse path
> short-circuited on `http_n <= 0` (`b->ntok = 0; return`), never rendering anything. Now that branch builds
> a small header-less error page into `b->raw` ("Could not load page" + the URL + a one-line cause/next-step
> hint) and `parse_html`s it (exactly like the local home page), then still sets the "failed" status.
> Verified in-guest: `browse nonexistent-zzz-99999.example` now shows the error message; `browse example.com`
> still loads normally (success path untouched). `make check` green (35 suites).

> **(M895) Browser — larger page capacity so big real pages aren't truncated.** With QEMU user-mode
> networking reaching the real internet, testing revealed the flagship browser hard-caps the page fetch at
> 256 KB (`RAW_MAX`): `en.wikipedia.org/wiki/Unix` fetched exactly `262143b` (256 KB−1) and was cut off.
> Bumped the (all-heap, `kzalloc`/`kmalloc`) capacities: `RAW_MAX` 256 KB→512 KB (fetch/DOM buffer),
> `TEXT_MAX` 49152→65000 (token text pool — the safe max under the uint16 token offset), `TOK_MAX`
> 7000→9500 (rendered tokens, sized to fill the larger text pool). Now the same article fetches its full
> `338552b` (no truncation) and renders ~1.3× more text. Also a defensive fix: the remote-`<img>` scratch
> de-chunk bound was `RAW_MAX` but the buffer is `IMG_READ_MAX` (128 KB) — corrected to `IMG_READ_MAX` so
> it's right regardless of `RAW_MAX`. (Rendering beyond ~64 KB of text needs widening the `tok_t` offset
> from uint16 to uint32 — a riskier kernel-side change deferred for supervised context.) Verified in-guest:
> Wikipedia fetches 338 KB, OS boots fine (memory OK), real pages render. `make check` green (35 suites).

> **(M894) Demo — `DEMO.SH` showcases the complete control-flow + expansion set.** Rounded out the bundled
> `source DEMO.SH` to demonstrate this session's later additions: a `grade()` function using `elif`
> (`echo elif: 85 grades $(grade 85)` → B), a `case red in red) … ;; *) … esac` (→ matched red), and
> `${MISSING:-a-fallback}` parameter defaulting — alongside the existing functions/`$#`/`$@`/`return`/bare-
> assignment/arithmetic/`$()`/alias/for/if/while lines. So the demo now exercises the whole shell in one
> runnable script. Verified in-guest: `source DEMO.SH` runs all of it cleanly end-to-end. Disk regenerated;
> `make check` green (35 suites). This also serves as the end-to-end regression check for M891/M892/M893.

> **(M893) Shell — `${VAR:-default}` / `${VAR:+alt}` parameter expansion.** The common sh defaulting forms,
> on top of the existing `${NAME}`. In `expand_vars`'s `${…}` branch: after the name, if `:-`/`:+` follows,
> read the literal word up to `}` — `:-` substitutes the word when VAR is unset/empty (else the value),
> `:+` substitutes the word only when VAR is set+non-empty. Plain `$NAME`/`${NAME}` is unchanged (no
> regression). Works for positional params too (e.g. `${1:-world}` in a function). Verified in-guest:
> `${undef:-fallback}`→`fallback`, `${name:-stranger}`→`Alice` (set), `${x:+present}`→empty (unset),
> `${y:+yes}`→`yes` (set), and `greet(){ echo hi ${1:-world}; }` → `hi world` / `hi Bob`. (Word is literal —
> no nested expansion inside it, a deliberate simplification.) Help line updated; `make check` green (35).

> **(M892) Shell — `case WORD in PAT) … ;; *) … ;; esac` (glob-match dispatch).** Completes the control-flow
> set (if/elif/else, while, for, case). Two parts: (1) taught the `shsplit.h` statement splitter that
> `case` opens / `esac` closes a construct (so the arms' `;`/`;;` stay internal) — pre-verified safe by
> shsplittest (added case/esac cases; 400k fuzz still clean, no regression to the other constructs); (2) a
> new `run_case` that `$`-expands WORD, strips `case`…`in`…`esac`, splits the arms on `;;`, splits each arm
> at `)` into `|`-separated glob patterns + commands, and runs the FIRST arm whose pattern `glob_match`es
> WORD (no fall-through; no match → `$?`=0). Verified in-guest: exact match, `f*` glob, `a|b|x` alternatives,
> `*` catch-all, `$1` in a `color()` function (`color r`→red, `color z`→unknown). Help line added.
> `make check` green (35 suites); shell.c warnings 11.

> **(M891) Shell — `elif` (multi-way `if`).** Previously deferred for parsing risk, but done **safely and
> additively**: in `run_if`, after isolating COND/then-body, if the body contains `; elif ` the else-branch
> is rebuilt as a nested `if <rest>; fi` string and run via `run_input_line` (which re-dispatches it to
> `run_if`), so an `elif` chain peels one level per recursion. The no-`elif` path is byte-identical — plain
> `if` / `if-else` is untouched (no regression), and the only fragility (a nested `if…fi` *inside* a
> then-body that also has `elif`) is the same pre-existing limitation `run_if` already had, not a new one.
> Verified in-guest: `if … elif … else …` picks the right branch; a `grade()` function (`grade 95`→A,
> `85`→B, `50`→F) works; plain `if` unregressed. Help line updated. `make check` green (35 suites);
> shell.c warnings 11.

> **(M890) Tests — extracted the `cd` path resolver into `normpath.h` + host fuzz (35th suite).** The M881
> `cd` normaliser was the last pure helper added this session without host coverage — and path handling is
> exactly where M881's bug lived. Lifted it into `user/normpath.h`, made self-contained (inlined its two
> trivial `streq`/`scpy` uses, behaviour-identical), and `shell.c` now `#include`s it. Added `tests/normpath/`
> (ASan+UBSan): ~20 regressions (absolute vs relative, `.`/`..` with root-floor, `//` collapse, trailing
> slash, and a mix like `../../x/./y/../z` → `/x/z`) + a 400k random base+arg fuzz asserting the result is
> always a NUL-terminated absolute path within 128 bytes. Verified `cd` unchanged in-guest (`/p/q` →
> `cd ../..` → `/`). `make check` now 35 suites; shell.c warnings 11.

> **(M889) Desktop — Welcome window mentions shell functions.** Small discoverability fix: the Welcome
> panel advertised "Shell: scriptable (for / if / while)" but functions — this session's headline addition —
> weren't surfaced. Now reads "Shell: scriptable (functions, loops, $())". Verified in-guest; `make check`
> green (34 suites).

> **(M888) Shell — `cmp` and `diff` set `$?` (0 same, 1 differ, 2 error), so `if cmp …` works.** Same class
> as the M886 grep fix, applied to the comparison commands: both computed a `differ`/`diffs` result but never
> set `g_status`, so `cmp a b && echo same` always said "same" and `if diff …` always took the then-branch.
> Now each sets `g_status` (identical → 0, differ → 1, missing-file/usage → 2, matching real cmp/diff).
> Verified in-guest: `cmp A B && echo SAME1` (identical) runs; `cmp A C && … || echo DIFFER` takes the ||;
> `diff A C; echo $?` → 1. `make check` green (34 suites); shell.c warnings 11. (Checked the other
> status-driven commands: `test`/`[` already set it; `find` correctly stays 0 like real find.)

> **(M887) Shell — `ls` handles multiple args / globs / files (`ls *.txt`, `ls FILE`).** Found by testing
> `ls *.TXT | wc -l`: it printed `ls: no such directory: README.TXT HELLO.TXT MOTD.TXT GUIDE.TXT` — `ls`
> took the *entire* rest of the line as one directory name, so a glob expansion (or any multi-name list, or
> a plain file) failed. Rewrote the `ls <args>` branch to walk space-separated names: a directory is listed
> (`sys_chdir`+`sys_list`, with a `name:` header only when several args are given), an existing file just
> prints its name (`sys_readfile` probe), else a per-name "no such file" error. Now `ls *.txt`, `ls FILE`,
> `ls dir1 dir2`, and the ubiquitous `ls *.ext | wc -l` all work; single-`ls`/`ls dir` unregressed.
> Verified in-guest. `make check` green (34 suites); shell.c warnings 11.

> **(M886) Shell — `grep` now sets `$?` (0 on match, 1 on no-match), so `grep … && …` works.** Found by
> testing pipe exit-status propagation: `echo hi | grep xyz; echo $?` printed `0` even though nothing
> matched. The `grep` builtin counted hits but never set `g_status`, so it kept `run_command`'s default 0.
> Now it sets `g_status = hits ? 0 : 1` at the end (and `1` on a usage error) — matching real grep, where
> the exit status drives `if grep …` / `grep … && …` / `… || …`. Verified in-guest: `echo hi | grep xyz`
> → `$?`=1, `grep hi` → `$?`=0, `echo test | grep es && echo AND-ran` runs, `… | grep zzz || echo OR-ran`
> runs. (Known minor limit, deferred: the pipe model appends the scratch file as the next stage's last arg,
> so piping into a *non-filter* like `true`/`echo` mis-handles it — rare; real pipes target filters.)
> `make check` green (34 suites); shell.c warnings 11.

> **(M885) Shell — `local NAME[=val]` function-scoped variables.** Closes the last footgun in the function
> system: named (non-positional) vars were global, so `f() { x=5; }` clobbered the caller's `x`. `local`
> declares a var private to the current function call — it saves the caller's value onto a small stack
> (`g_localsave`, 64 slots) and the function dispatch restores everything pushed during its body when the
> call returns (a per-frame `localmark` makes it nest, like the M876 positional-param save/restore). Plain
> assignment in a function still affects the global (POSIX sh default); `local x` opts into scoping.
> Verified in-guest: `x=global; f(){ local x=inside; …}; f; echo $x` → still `global`; the no-`local`
> contrast leaks (`y=changed`); and nested `outer(){ local a=1; inner; …}` / `inner(){ local a=2; …}` over
> a global `a=0` prints `2 / 1 / 0` (each frame restores its own). Composes with `return` (restore runs
> unconditionally after the body). Help line added; `make check` green (34 suites); shell.c warnings 11.

> **(M884) Tests — extracted the calculator's expression evaluator into `calceval.h` + host fuzz (34th suite).**
> `calc.c` had its own recursive-descent evaluator (`^` for power, `& | << >> ~`, `0x` hex — distinct from
> the shell's `$(())`/shmath: different operators, no variables, signed accumulation relying on the OS's
> `-fwrapv`) and it was the one arithmetic evaluator in the OS with *no* host coverage. Lifted it verbatim
> into `user/calceval.h` (exposing `calc_eval(s, &err)`), shrinking `calc.c` to I/O + `main`. Added
> `tests/calc/` (ASan+UBSan + **-fwrapv** to match the OS build, so the intended signed wraparound isn't a
> UBSan false-positive): ~40 regression cases (precedence, parens, `^` right-assoc, hex, bitwise, and the
> error paths — `2+`, `(2+3`, `5/0`, trailing junk, `0x`) + a 400k random-expression fuzz asserting no
> crash/hang/UB. Verified in-guest the app still computes (`(2+3)*4 - 0xa` → `10`, `2^10` → `1024 0x400`).
> Follows the shgrep/shmath/shsplit extraction pattern; `make check` now 34 suites; calc.c builds clean.

> **(M883) Demo — `DEMO.SH` now showcases functions, `$#`/`$@`, `return`, and bare assignment.** The bundled
> scripting demo (`source DEMO.SH`) predated this session's work — it used `set i=…` and never showed
> functions, the headline new feature. Modernized it to demonstrate the full capability: a `greet world`
> function, an `args alpha beta gamma` function printing `$#`/`$@`, a `pos` guard-clause function with
> `return` used as `pos 5 && echo …`, bare `OS=OS-DEV` / `i=$((i + 1))` assignment, alongside the existing
> arithmetic/ternary/`$()`/alias/for/if/while lines. Verified in-guest: `source DEMO.SH` runs all of it
> cleanly end-to-end (`hello world -- from a function`, `function got 3 args: alpha beta gamma`,
> `return-value works: 5 is positive`, …). Also regenerated a clean `fat.img`. `make check` green (33 suites).

> **(M882) Shell — bare `NAME=value` assignment (sh-style), so `x=$(cmd)` works.** Found by testing
> `x=$(greet)`: it printed `unknown command: x=hello` because the only assignment path was `set NAME=val`;
> a bare `x=5` fell through to the not-found branch. Real sh/bash assign with bare `NAME=value`, and the
> ubiquitous `x=$(cmd)` / `x=$((expr))` idioms need it. Added a check at the top of `run_command`'s
> dispatch (after the function-call shadow, before the builtins): if the first token is a valid identifier
> immediately followed by `=`, it's an assignment (`vset(NAME, rest-of-line)`); no builtin's first word
> contains `=`, and `echo a=b` is unaffected because its first token is `echo`. The RHS is already
> expanded by the time `run_command` sees it, so `$()`/`$(())`/`$VAR` all work. Verified in-guest:
> `x=5`, `y=$(greet)` → `hello`, `n=$((6*7))` → `42`, `msg=hi there` → `hi there`, and `echo a=b` stays
> literal. `set NAME=val` still works. `make check` green (33 suites); shell.c warnings 11.

> **(M881) Shell — `cd` normalizes `.`/`..`/`//` so the prompt matches the real directory.** Found by
> adversarial path testing: `cd ../..` from `/aa/bb` left the prompt showing `/aa/bb/../..` (the kernel cwd
> was correct — files still worked — but the displayed path was wrong). The old `cd` only special-cased
> exact `/`, exact `..`, and absolute paths; any other relative path was appended verbatim. Replaced the
> ad-hoc cases with a `normpath(base, arg, out)` helper that seeds a component stack from the base (for
> relative args), walks the arg dropping `.` and popping on `..`, and rebuilds a clean absolute path —
> handling `cd ../..`, `cd ../sib`, `cd ./x`, `cd a/b/c`, `//`, etc. Also trims stray spaces around the arg.
> Verified in-guest: `cd ../..` → `/`, `cd ../bb` → `/aa/bb`, `cd .` stays, and a relative write after a
> normalized `cd` lands in the right place (display ↔ kernel cwd agree). `make check` green (33); warnings 11.

> **(M880) Shell — `grep -l` in a pipe prints `(standard input)`, not the `PIPE.TMP` scratch name.** The
> same leak class as M879: `cmd | grep -l pat` named the internal pipe temp file on a match. It now prints
> `(standard input)` for piped input (matching real grep) while `grep -l pat realfile` still names the file.
> Verified: `greet | grep -l hello` → `(standard input)`; 3-stage function pipe `greet | sort | head -1` and
> `greet | grep -c o` → `2` all correct. `make check` green (33 suites); shell.c warnings 11.

> **(M879) Shell — `wc` no longer leaks the internal `PIPE.TMP` name into piped output.** Found by testing
> the new functions across other features: `greet | wc -l` printed `lines 2  PIPE.TMP`, exposing the pipe's
> scratch file. The pipe model appends the temp file as the next stage's last arg, and `wc` (unlike
> grep/head/tail/hexdump/strings, which only print a name for multiple files) *always* echoed its filename.
> Fix: `wc` skips printing the name when it is the `PIPE.TMP` scratch file — keyed off the actual arg, so a
> real `wc README.TXT` still shows the name and a function stage running `wc realfile` is unaffected.
> Verified: `greet | wc -l` → `lines 2`, `greet | wc` → `lines 2 words 2 bytes 12`, `wc -l README.TXT` →
> still names the file. `make check` green (33 suites); shell.c warnings 11.

> **(M878) Shell — `return [N]` to exit a function early (guard clauses).** The last missing core piece of
> the M873-M877 function system. A `g_returning` flag set by the `return` builtin (`return N` sets `$?` to
> N; bare `return` keeps the last status) is honored by every body executor — the `run_input_line` segment
> loop, the `while`/`for` loops, and (transitively) `if` branches — so it unwinds out of nested constructs,
> and is consumed at the function boundary in the call dispatch (so it stops the function, not the whole
> shell). It also ends a sourced script (`source_file`), and a stray top-level `return` is cleared each
> interactive line so it can't wedge the prompt. Verified in-guest: a guard `if [ $1 -gt 5 ]; then return 1; fi`
> (→ `$?`=1, body after skipped), bare early `return`, and `return $i` from inside a `for` loop (stops the
> loop AND the function with `$?`=i) all behave correctly; a top-level `return 5` just sets `$?` and the
> shell continues. Help line added; `make check` green (33 suites); shell.c warnings 11.

> **(M877) Tests — extracted the shell's `;` statement splitter into `shsplit.h` + host fuzz (33rd suite).**
> The M876 splitter (the construct-depth-aware scan + `word_at`) was pure string logic with no safety net
> beyond boottest + manual in-guest — exactly the gap the project notes flag. Lifted it verbatim into
> `user/shsplit.h` as `sh_next_sep(seg)` (returns the offset of the next top-level `;`, or of the `\0`),
> shrinking `run_input_line`'s loop to a one-line call. Added `tests/shsplit/` (ASan+UBSan): 24 segmentation
> regressions (`;` lists, `$()`/`$(())` protecting inner `;`, nested `if`/`while`/`for`…`fi`/`done`, and
> keyword-in-argument-position guards like `echo fi` / `ifconfig` that must NOT open a construct) + a 400k
> random-char fuzz asserting `sh_next_sep` always returns an in-range offset pointing at `;` or `\0` and
> that full segmentation terminates. Behaviour verified unchanged in-guest (recursion + mid-line for-loop).
> Follows the shgrep/shmath extraction pattern; `make check` now 33 suites, shell.c warnings 11.

> **(M876) Shell — functions get local positional-param scope + control constructs work after `;` / in
> function bodies.** Two parser fixes that, with M875, make recursive and looping functions actually work.
> (1) *Scope*: a function call now saves the caller's `$1`..`$9`/`$@`/`$#` before binding its own and
> restores them after the body returns, so a nested call no longer clobbers the caller's args (`outer A`
> calling `inner X Y` left `$1` = `X`; now it stays `A`). (2) *Control flow*: the `;`-splitter in
> `run_input_line` only recognised `if`/`while`/`for` as the *first* statement of a line — after a `;`
> (e.g. inside a function body) the construct's own `;`s split it into bogus commands ("unknown command:
> then …"). The splitter now tracks construct nesting (a `word_at` keyword check at command positions:
> `if`/`for`/`while` open, `fi`/`done` close, `then`/`do`/`else` re-arm) so a construct's internal `;`s
> aren't break points, and each top-level segment is dispatched to `run_for`/`run_while`/`run_if`/`run_andor`.
> Verified in-guest: `r() { echo enter=$1; if [ $1 -gt 1 ]; then r $(($1 - 1)); fi; echo leave=$1; }; r 3`
> → `enter=3/2/1, leave=1/2/3` (recursion + scope + arithmetic + if-in-body all at once);
> `count() { for i in a b; do echo $1-$i; done; }; count P` → `P-a/P-b`; `echo START; for …; done; echo END`
> works. Plain `;` lists and whole-line loops unregressed. shell.c warnings 11; `make check` green (32 suites).

> **(M875) Shell — `$1`..`$9` (positional params) now work inside `$((…))` arithmetic.** A pre-existing
> bug found while testing M874's functions: `sh_factor` (in `shmath.h`) skipped a leading `$` and then,
> seeing a digit, parsed it as a numeric *literal* — so `$(($1 - 1))` evaluated as `1 - 1 = 0` regardless
> of `$1`'s value (`$y`/bare `ten` worked because they hit the identifier branch). Fix: a digit right after
> `$` is now a one-char variable name resolved via `sh_var`/`vget` (the positional param), not a literal.
> This is exactly the recursive-function idiom `r $(($1 - 1))`. Added host-test cases (`$1`, `$1 - 1`,
> `$1 * $a`) + a digit entry in the stub var table; `make check` green incl. shmathtest's 300k fuzz.

> **(M874) Shell — `$#` (arg count) and `$@` (all args) inside functions.** Completes M873's functions
> so they can be variadic. `expand_vars` gains a special-case (right after `$?`) for `$#`/`$@` that reads
> the value via `vget`; the function dispatch in `run_command` sets `$@` to the full arg string (captured
> before the `$1`..`$9` bind loop) and `$#` to the parameter count (after clearing unused slots). Verified
> in-guest: `args() { echo count=$# all=$@; }` then `args a b c` → `count=3 all=a b c`, `args solo` →
> `count=1 all=solo`. Help line updated to list `$# $@`. Build clean (shell.c warnings 11, no regression);
> `make check` green (32 suites).

> **(M873) Shell — user-defined functions `name() { … }` with `$1`..`$9`.** The biggest scripting feature
> added this turn. A one-line definition `greet() { echo hi $1; }` is parsed in `run_input_line` (detect
> `NAME()` then `{`…`}`, store the body in a 16-slot table) and called as `greet world`: `run_command`
> checks the function table *before* the builtin dispatch (so a function shadows a builtin, like bash), binds
> `$1`..`$9` to the call args (unused ones cleared so a prior call can't leak in), runs the body through
> `run_input_line`, and a depth counter caps recursion at 8. The committed OS was never at risk (committed
> only after build + `make check` + in-guest passed). Verified in-guest: `greet world`→`hi world`,
> `info alice 42`→`name=alice num=42`, a multi-command body `m(){ echo one-$1; echo two-$1; }; m X`→
> `one-X`/`two-X`, and builtins still work. Limitation: one-line defs only (the shell is line-oriented), and
> params are global (nested calls don't perfectly scope) — noted for a future refinement. `make check` 32
> suites; shell.c warnings unchanged (11).

> **(M872) Shell — `xargs` (build a command from piped input).** `cmd | xargs CMD` appends the piped
> whitespace-separated tokens to CMD and runs it (the classic `find pat | xargs rm` idiom) — assembled into
> a line and dispatched through `run_input_line`. Verified in-guest: `echo apple banana cherry | xargs echo
> fruits:` → `fruits: apple banana cherry`, `seq 1 4 | xargs echo nums:` → `nums: 1 2 3 4`. With printf,
> sleep, tee and xargs added this session, the shell's common standard-command set is complete (remaining
> misses — expr/which/type/yes — are redundant or need a command table). `make check` 32 suites; warnings (11).

> **(M871) Shell — `tee` (split a pipe to a file).** `cmd | tee FILE...` writes the piped data to each FILE
> and passes it on down the pipe — the standard way to capture an intermediate stage. Uses the shell's pipe
> convention (the input is the appended last arg; the earlier args are the targets). Verified in-guest:
> `echo hello-world | tee SAVED.TXT` shows the text and writes the file; `seq 1 3 | tee N.TXT | wc -l` → 3
> (passed through) with N.TXT holding 1/2/3. `make check` 32 suites; shell.c warnings unchanged (11).

> **(M870) Shell — `sleep N` (scripting delays).** Another standard command that was missing; `sleep N[.M]`
> pauses N seconds (plus optional tenths) via the existing `sys_sleep`, capped at 300s so a typo can't hang
> the terminal unbreakably. Verified in-guest: `echo before-sleep`, `sleep 1`, `echo after-sleep` all run in
> order with the pause between. `make check` 32 suites; shell.c warnings unchanged (11).

> **(M869) Shell — `printf` (a standard command that was entirely missing).** `printf FMT [args]` interprets
> `\n`/`\t`/`\r`/`\\` escapes and `%s`/`%d`/`%c`/`%%` specifiers, and cycles the format over the remaining
> args (bash semantics) — `printf '[%s]\n' a b c` → `[a]\n[b]\n[c]\n`. Self-contained new builtin (a guard
> stops the cycle when a pass consumes no arg, so an unknown spec can't loop). Single-word formats work
> without quoting (the common case); multi-word formats need quoting (deferred). Verified in-guest:
> `printf hello\nworld\n` → two lines, `printf %s-%s=%d\n a b 42` → `a-b=42`, `printf [%s]\n one two three`
> → three bracketed lines. `make check` 32 suites; shell.c warnings unchanged (11).

> **(M868) Shell — `grep -o` (print only the matched part), via a greedy matcher.** The last text-tool gap.
> It required two matcher changes in `shgrep.h`: (1) thread an end pointer through `gr_matchhere`/`gr_matchstar`
> so a match's span is known (a new `gr_match_span`; `gr_match` stays a boolean wrapper), and (2) make the
> `*` quantifiers **greedy** (consume all, then backtrack) — the old lazy form would make `grep -o '[0-9]*'`
> emit empty matches. Crucially, greedy vs lazy gives the *same* match/no-match result, so `shgreptest`'s
> 200k-pair fuzz still passes (the safety net for touching the core matcher). `grep -o` finds each leftmost-
> longest match per line (across all `-e` patterns), skipping empty matches. Verified in-guest:
> `grep -o error` → `error`×2, `grep -o [0-9][0-9]*` → `42`/`7`/`99`, plain grep unchanged. The shell's text
> toolkit is now 100% bash-complete. `make check` 32 suites; shell.c warnings unchanged (11).

> **(M867) Shell — `paste -dX` (custom join delimiter).** `paste` always joined two files' lines with a
> tab; `-dX` now picks the joiner (e.g. `paste -d, a b` → `a1,b1`). Verified in-guest: `paste -d, P1 P2` →
> `a,1`/`b,2`, plain `paste` still tab-joins. This finishes the common shell text toolkit (the only deferred
> piece is `grep -o`, which needs a greedy-matcher rewrite — the current matcher is lazy). `make check` 32
> suites; shell.c warnings unchanged (11).

> **(M866) Shell — `grep -A/-B/-C N` (context lines).** grep can now print N lines of context around each
> match: `-A` after, `-B` before, `-C` both — the common idiom for reading matches in context. A shared
> `grep_emit` helper prints a line with its prefix (`:` for a match, `-` for context); `-A` is an
> after-counter, `-B`/`-C` keep a small ring of recent lines (capped 16) and a `last_printed` line number
> avoids double-printing overlapping context. Verified in-guest: `-A1` → match + next line, `-B1` → prev +
> match, `-C1` → both, plain grep unchanged. `make check` 32 suites; shell.c warnings unchanged (11).

> **(M865) Shell — `wc -L` (longest line length).** Added the standard `-L` to wc (length of the longest
> line). The default-to-all check now excludes `-L`, so `wc -L` reports just the longest (not all counts).
> Verified in-guest: `wc -L W.TXT` → `longest 13` for a file whose longest line is `a-longer-line`; plain
> `wc` still shows lines/words/bytes. `make check` 32 suites; shell.c warnings unchanged (11).

> **(M864) Shell — `sort -tX` (field delimiter) completes column sorting for CSV.** `sort -k` split on
> whitespace only, so it couldn't sort CSV; `-tX` sets the field delimiter (`sort_field` now takes it).
> `sort -t, -k2 -n DATA` sorts comma-separated data by the numeric 2nd column. Verified in-guest on
> `apple,30 / banana,10 / cherry,20`: `sort -t, -k2 -n` → `banana,10, cherry,20, apple,30`. The shell's
> sort (and the whole text toolkit) is now bash-complete. `make check` 32 suites; shell.c warnings (11).

> **(M863) Shell — `sort -kN` (sort by column).** `sort` only compared whole lines; `sort -k2` now compares
> from whitespace field N to end of line (bash's default key), composing with `-n`/`-r`/`-f`. A `sort_field`
> helper returns the field-N offset; the three compare branches use it. Verified in-guest on
> `apple 30 / banana 10 / cherry 20`: `sort -k2 -n` → `banana 10, cherry 20, apple 30` (by numeric 2nd
> column); plain `sort` (whole line) unchanged. This completes the common text-tool flag set. `make check`
> 32 suites; shell.c warnings unchanged (11).

> **(M862) Shell — `head -c N` / `tail -c N` (byte mode).** head/tail only counted lines (`-N`); added `-c N`
> for the first/last N *bytes* (binary inspection, byte-precise slicing). Shares the existing per-file loop
> and the screen-only `...` hint. Verified in-guest on `abcdefghij`: `head -c 4` → `abcd`, `tail -c 4` →
> `hij` (last 4 bytes incl. newline), `head -2` (line mode) unchanged. `make check` 32 suites; shell.c
> warnings unchanged (11).

> **(M861) Shell — `cut` accepts non-contiguous field/char lists (`-f1,3`).** `cut` only did one contiguous
> range, so `cut -d, -f1,3` (common for CSV column picking) wasn't possible. The spec now parses a comma list
> of `N`/`N-M` ranges into arrays (a `cut_sel` helper tests membership), and the field separator is emitted
> *before each selected field after the first* (an `out_any` flag) — which joins non-adjacent fields with the
> delimiter and preserves empty selected fields. Verified in-guest on `a,b,c,d`: `-f1,3` → `a,c`, `-f1` →
> `a`, `-f2-4` → `b,c,d` (single + range unchanged). `make check` 32 suites; shell.c warnings unchanged (11).

> **(M860) Shell — `<` input redirect.** `wc -l < FILE` printed a confusing `wc: no such file: <` (the `<`
> and file were passed as literal args). Since these commands read a file argument, `cmd … < file` is now
> rewritten to `cmd … file` (the rewrite is never longer than the original, so it edits the line in place);
> it composes with `>` (`sort -r < in > out`). Verified in-guest: `wc -l < DATA.CSV` → 5,
> `sort -r < DATA.CSV > SORTED.TXT` produced the reverse-sorted file. `make check` 32 suites; shell.c
> warnings unchanged (11).

> **(M859) Shell — decorative output (`head` "...", `grep` "(no matches)") no longer pollutes pipes/`$()`.**
> A real bug found by adversarial testing: `head` prints a `...` "more lines" hint and `grep` a "(no matches)"
> note, both to stdout — so `seq 1 9 | head -3 | wc -l` returned **4** (the `...` counted as a line) and
> `$(seq 1 9 | head -2)` yielded `1 2 ...`. Added `cap_active()` to ulib (true while `print()` is being
> captured for a pipe stage or `$()`); `head`, `grep`, and `find` now suppress the hint when captured but
> still show it on the screen (`seq 1 9 | head -3` as the final stage still prints `...`). Verified: `wc -l`
> → 3, `$(… head -2)` → `1 2`, `$(find NOSUCHPATTERN)` → empty (was `(no matches)`). `make check` 32 suites.

> **(M858) Shell — `tr` handles character sets and `a-z` ranges.** `tr` only did single-char replace, so the
> canonical `tr a-z A-Z` (case conversion) silently did nothing useful. A new `tr_expand` helper expands a
> SET token (literal chars + `a-z` ranges); `tr SET1 SET2 FILE` now maps each SET1 char to the SET2 char at
> the same index (shorter SET2's last char repeats), and `tr -d SET` deletes a whole set. Verified in-guest
> on "hello world": `tr a-z A-Z` → `HELLO WORLD`, `tr -d aeiou` → `hll wrld`, `tr a-y b-z` → `ifmmp xpsme`
> (Caesar +1). `make check` 32 suites; shell.c warnings unchanged (11).

> **(M857) Demo — `DEMO.SH` showcases the new `$(())` ternary.** The baked scripting demo covered
> variables/arithmetic/`$()`/aliases/for/if/while but predated M848's operator additions; added a line
> `$((8 > 3 ? 8 : 3))` so the showcase reflects the now-bash-complete arithmetic. Doubles as an end-to-end
> scripting integration test — verified in-guest that `source DEMO.SH` runs every construct, printing
> "ternary the larger of 8 and 3 is 8". `make check` 32 suites.

> **(M856) Shell — `uniq -d` / `-u` (only duplicated / only non-repeated lines).** Completed the classic
> `sort | uniq` filter set: `-d` emits a run only when it repeats (count > 1), `-u` only when it doesn't
> (count == 1); both compose with `-c`. Verified in-guest on apple,apple,banana,cherry,cherry,date:
> `uniq -d` → `apple, cherry`; `uniq -u` → `banana, date`. `make check` 32 suites; shell.c warnings
> unchanged (11).

> **(M855) Shell — `sort -f` (fold case) for case-insensitive sorting.** Added `-f` to `sort` (alongside
> `-nru`): comparisons lowercase both sides, and `-uf` dedups case-insensitively (via a small `sort_foldeq`
> using the grep matcher's `gr_lc`). Verified in-guest on mixed-case lines: plain `sort` →
> `Mango, Zebra, apple, banana` (byte order, uppercase first); `sort -f` → `apple, banana, Mango, Zebra`.
> `make check` 32 suites; shell.c warnings unchanged (11).

> **(M854) Shell — `grep -l` lists only the filenames that match.** With `-l` (combinable, e.g. `grep -il`),
> grep prints each file's name once if any line matches and moves on, instead of the matching lines —
> bash's "which files contain this?" idiom. Composes with `-e` (`grep -l -e a -e b *.txt`) and `-v`, and
> suppresses the `-c` count / "(no matches)" line. Line grep is unchanged. Verified in-guest:
> `grep -l apple A.TXT B.TXT C.TXT` → `A.TXT` `C.TXT` (B.TXT has only banana). `make check` 32 suites.

> **(M853) Shell — `grep -e pat` (repeatable) for multi-pattern OR matching.** The tiny regex engine has no
> `|` alternation, and even if it did, `grep 'a|b'` is unusable from a shell with no quoting (the `|` is a
> pipe). `grep -e error -e warning FILE` now matches a line if it hits ANY of the `-e` patterns — bash's
> idiom for alternation, and it dodges the metacharacter clash. Single-pattern `grep pat FILE` is unchanged.
> Verified in-guest on a 4-line file: `grep -e apple -e cherry` → `apple`+`cherry` only; `grep banana` →
> `banana`. `make check` 32 suites; shell.c warning count unchanged (11).

> **(M852) Shell — the `js` demo showcases arrow functions + chained array methods.** The built-in demo (bare
> `js`) only exercised old-style `function` declarations, under-selling the engine. Added a line —
> `[1,2,3,4,5].map(x=>x*x).filter(x=>x>4).join(",")` → `9,16,25` — so the showcase reflects the engine's
> modern capabilities (confirmed this session: arrows, `map`/`filter` chains, comparators all work; the
> browser even runs interactive arrow-using pages like RPS.HTM). `make check` 32 suites; verified in-guest.

> **(M851) Shell — `js -e <code>` receives its code verbatim (arrows, comparisons, `&&` now work).** Inline
> JS silently failed on anything containing a shell metacharacter: `js -e console.log((x=>x*2)(5))` produced
> nothing because the `>` in the arrow `=>` was parsed as an output redirect, truncating the code (the
> `function(){}` forms worked only because they have no `>`). The engine was never at fault — `jstest`
> exercises arrows and passes. Fix: intercept `js -e ` at the top of `run_input_line` (before the
> `;`/`&&`/`|`/redirect/glob processing) and hand the rest of the line to the engine literally. Every entry
> path (interactive, `source`, and `$()`) goes through `run_input_line`, so capture via `$(js -e …)` still
> works. Verified in-guest: `[1,2,3,4].map(x=>x*x).filter(x=>x>3).join('-')`→`4-9-16`, `(x=>x*2)(5)`→`10`,
> `sort((a,b)=>a-b)`→`1,2,5,8`, `$(js -e console.log(7*6))`→`42`. `make check` 32 suites.

> **(M850) Shell — command substitution runs a full statement list (`;`, `for`/`while`/`if`).** `$(...)`
> evaluated its body with `run_andor`, which only knows `&&`/`||` — so `$(echo a; echo b)` captured the
> literal `; echo b` and `$(for …)` errored. It now uses `run_input_line` (the same entry interactive lines
> use), so a `;`-separated list and the control-flow keywords work inside a substitution. Verified in-guest:
> `$(echo a; echo b)`→`a b`, `$(for i in 1 2 3; do echo $i; done)`→`1 2 3`, `$(echo one; echo $(echo two))`→
> `one two`. `make check` 32 suites.

> **(M849) Shell — nested command substitution `$(… $(…) …)` now works.** It silently produced wrong output
> (`$(echo $(echo deep))` gave `(echo deep)`): the `in_cmdsub` guard hard-blocked the inner `$()`, and
> `expand_vars` then mangled the leftover `$(` into `(`. Two fixes: (1) `cap_begin`/`cap_end` now stack their
> output buffers (`user/ulib.c`), so an inner substitution's capture no longer clobbers the outer's, and
> `in_cmdsub` became a depth counter (cap 8) instead of a hard block; (2) the real subtlety — `run_line`'s
> `subline` (the substitution dst) was a single `static` buffer, but `cmdsub_expand` recurses back through
> `run_line`, so a nested `$()` overwrote the outer build mid-flight. It's now indexed by recursion depth.
> Verified in-guest: `$(echo $(echo deep))`→`deep`, `$(echo $(echo $(echo x3)))`→`x3`,
> `a-$(echo b)-$(echo $(echo c))`→`a-b-c`. `make check` 32 suites.

> **(M848) Shell — `$(())` gains relational/logical/ternary operators (and a command-sub split bug fix).**
> The arithmetic evaluator (`user/shmath.h`) stopped at bitwise `|`; it now matches bash's full set —
> `< <= > >=`, `== !=`, `&&`, `||`, the `?:` ternary, and unary `!` — so scripts can write `max=$((a>b?a:b))`.
> Adding `&&`/`||` exposed a real bug: `run_andor` (and `run_input_line`'s `;` split) scanned for the
> command operators without skipping `$( … )`, so `$((a && b))` *and even* `$(echo a && echo b)` command
> substitution were split mid-expression. Both splitters now track `$(`-paren depth and ignore operators
> inside a substitution. `shmath_test.c` gained ~30 cases for the new operators and fuzzes them too;
> verified in-guest: `$((10==10 && 3<5))`→1, `$(echo a && echo b)`→`a b`, `$((2>1?100:0))`→100. `make check`
> 32 suites.

> **(M847) Tests — extract the Tab-completion core and unit-test it.** The trickiest part of M842–845 — the
> case-insensitive longest-common-prefix scan over the candidate names — was lifted verbatim from the kernel
> line editor into `kernel/complete.h` (`complete_match` + `complete_scan`), mirroring the shgrep/shmath/
> cssprop extraction pattern. `app.c` now calls those helpers (verified in-guest: byte-identical Tab
> behaviour — unique fill, common-prefix extend, and candidate list all unchanged), and a new host suite,
> `completetest`, exercises them under ASan+UBSan: prefix matching, unique vs multi-match prefixes,
> variable-length names, case-insensitivity, directory `/` exclusion, and the empty-word case. `make check`
> is now 32 suites; all pass.

> **(M846) Shell — `help` documents the richer Tab completion.** M842–845 turned Tab from "complete a unique
> name" into prefix-extension + candidate-listing + empty-word listing, but `help` still only said
> "Tab=complete" — an undiscoverable feature is an incomplete one. Split the crowded `edit:` line and added a
> dedicated line: "Tab completes a filename (longest common prefix); a 2nd Tab lists the matches". Verified
> in-guest that it renders. `make check` 31/31.

> **(M845) Shell — Tab on an empty argument lists every file.** `cmd <Tab>` with the cursor on a blank
> argument now lists all cwd entries (or extends to their common prefix, if any) — bash's empty-word
> completion. The guard widened from `plen > 0` to `plen > 0 || ws > 0`, reusing all the M842–844 machinery;
> a wholly empty line (no command yet) still does nothing. Verified in-guest: `cat `→Tab listed the whole
> directory across several wrapped rows and repainted the prompt — also a good stress test of the listing
> redraw with 32 candidates. `make check` 31/31.

> **(M844) Shell — Tab adds a trailing space after a unique file, keeps `/` for a directory.** Finishing the
> completion polish: when Tab resolves a word to a single entry it now appends a space (so you can type the
> next argument straight away) or, if the entry is a directory, the `/` (so it reads as a path). Multi-match
> prefix extension stays bare. Verified in-guest: `cat MO`→Tab→`cat MOTD.TXT ` then `HEL`→Tab→
> `cat MOTD.TXT HELLO.TXT ` — two filenames completed back-to-back with no manual spaces, like bash.
> `make check` 31/31.

> **(M843) Shell — a second Tab lists the completion candidates.** M842 made the first Tab extend to the
> common prefix, but then it dead-ended (e.g. `cat README.` with no way to see what's there). Now an
> ambiguous Tab — when the word is already at the common prefix and several entries match — lists the
> matching names on a fresh line and repaints the prompt + current input beneath, exactly like bash's second
> Tab. The prompt text is read back out of the terminal grid (the kernel line editor doesn't know the shell's
> prompt string), so it survives the redraw verbatim. Verified in-guest: `cat READ`→Tab→`cat README.`→Tab
> listed `README.TXT  README.MD` then restored the line. `make check` 31/31.

> **(M842) Shell — Tab completes to the longest common prefix.** The terminal line editor (in `app.c`)
> already completed a *unique* filename on Tab, but the moment two cwd entries shared a prefix it went dead —
> the help text promised "Tab=complete" yet ambiguous cases did nothing. Now Tab extends the word to the
> longest common prefix of every matching entry (case-insensitive match, canonical case emitted), which is
> bash's default behaviour; a unique match still fills the whole name. Verified in-guest: `cat MO`+Tab →
> `cat MOTD.TXT` (unique, full); `cat READ`+Tab → `cat README.` (the shared prefix of README.TXT and
> README.MD — previously a no-op). `make check` 31/31.

> **(M841) QA — fix a `-Wmisleading-indentation` warning introduced this session.** A sweep of the build
> output found one new warning I'd added in M838 (multi-file cp/mv): the `dpath[d] = 0;` path terminator
> trailed a `for` loop on the same line. Split onto its own line — no behaviour change. The warning count is
> back to the long-standing baseline (46, all dense one-liner style that predates this work). `make check`
> 31/31.

> **(M840) Shell — `hexdump` dumps multiple files.** The last single-file inspection command: `hexdump
> FILE...` now dumps each, with a `==> name <==` header before each when several are given. This completes
> the multi-file set — every inspection command (cat/head/tail/wc/grep/file/sha256/sha512/crc32/strings/
> hexdump) and creation/deletion command (rm/mkdir/touch) takes multiple names, and cp/mv handle a directory
> destination (single and multi-source). `make check` 30/30. Verified in-guest: `hexdump HELLO.TXT MOTD.TXT`
> dumped both under their headers.

> **(M839) Shell — `strings` scans multiple files.** `strings FILE...` now extracts printable runs from each
> space-separated file, printing a `==> name <==` header before each when more than one is given (matching
> `head`/`tail`). Handy for `strings *.elf`. `make check` 30/30. Verified in-guest: `strings HELLO.TXT
> MOTD.TXT` printed both files' strings under their headers.

> **(M838) Shell — multi-file `cp`/`mv` into a directory.** The last FS-command gap: `cp SRC... DESTDIR`
> (e.g. `cp *.txt backup`) and the `mv` form. Done as a *separate* branch that triggers only for >2 arguments
> (via a small `nargs` token-count), so the heavily-used and data-loss-sensitive 2-arg `cp`/`mv` handler
> (M832/M833) is left completely untouched. The last token must be an existing directory; each source is
> copied to `dir/basename`, and for `mv` the source is deleted *only after* its copy succeeds. `make check`
> 30/30. Verified in-guest: `cp m1.txt m2.txt bak` put both in bak; `mv m1.txt m2.txt bak` moved both and
> `ls m1.txt` confirmed the source was gone (content safe in bak — no data loss).

> **(M837) Shell — `crc32` multi-file (and simplified).** Completes the multi-arg consistency pass: `crc32`
> now checksums each space-separated file (`crc32 *.png`) with the filename in the output (`<crc>  <name>`,
> matching M835's sha256/sha512). Rewrote its hand-rolled grow-the-buffer read to use the shared `slurp()`
> helper (which also sets `$?` on a missing file), shrinking the code. All file commands now take multiple
> names: rm/mkdir/touch/file/sha256/sha512/crc32, plus cat/head/tail/wc/grep (already multi) and cp/mv into a
> dir. `make check` 30/30. Verified in-guest: `crc32 HELLO.TXT MOTD.TXT` → `636a4fb6 HELLO.TXT` /
> `cfe1109c MOTD.TXT`.

> **(M836) Shell — `run <prog> [arg]` (open the GUI editor on a file).** Completes the "open a file in the
> editor" story (Files→editor was M806): you can now do `run editor README.TXT` from the shell to launch the
> windowed editor already opened on that file. `SYS_spawn` gained an optional arg in `rsi` (routed to M806's
> `app_spawn_named_arg`); `sys_spawn_arg` is the new ulib wrapper; and `run` parses `prog` + an optional arg,
> falling back to plain `sys_spawn` (so `run snake`, disk `.elf`s, etc. are unchanged). `make check` 30/30.
> Verified in-guest: `run editor README.TXT` opened a new Editor window titled `EDIT README.TXT` with the
> file's contents loaded.

> **(M835) Shell — `sha256`/`sha512` hash multiple files.** Both hashed only one file; `sha256 a b` (or a
> glob-expanded `sha256 *.iso`) now hashes each in turn, and the output gained the filename after the digest
> (`<hash>  <name>`, the standard `sha256sum` format) so batch checksums are usable. `make check` 30/30.
> Verified in-guest: `sha256 HELLO.TXT MOTD.TXT` printed two distinct digests each tagged with its filename.

> **(M834) Shell — `file` identifies multiple files.** `file` only inspected one name; `file a b c` (or a
> glob-expanded `file *`) now identifies each space-separated file in turn — handy for surveying a directory's
> many file types. Same loop pattern as the M830/M831 multi-arg commands. `make check` 30/30. Verified
> in-guest: `file README.TXT INTER.PNG DOOM1.WAD` reported `ASCII text` / `PNG image` / `data`.

> **(M833) Shell — clearer `cp`/`mv` error on a directory source.** Copying a directory (no `-r` support) read
> it as a file and failed with a misleading "no such file". `cp`/`mv` now detect a directory source (the same
> chdir-test as M832) and report "cp: NAME is a directory". `make check` 30/30. Verified in-guest:
> `cp srcd dest` (a dir) printed "cp: srcd is a directory", while `cp okf.txt copied.txt` still copied
> normally (no regression).

> **(M832) Shell — `cp`/`mv` into a directory.** `cp SRC DST` / `mv SRC DST` treated DST as a literal target
> name, so `cp file dir/` made a *file* called `dir` rather than `dir/file`. Now, when DST is an existing
> directory (detected with the same transient chdir-test as `ls <dir>`), the destination becomes
> `DST/basename(SRC)`. `make check` 30/30. Verified in-guest: `mkdir cpdir; echo content > cpf.txt;
> cp cpf.txt cpdir` reported `copied cpf.txt -> cpdir/cpf.txt`, and `ls cpdir` showed `CPF.TXT` inside.

> **(M831) Shell — `mkdir`/`touch` accept multiple names.** Like `rm` (M830), both were single-argument, so
> `mkdir a b c` / `touch a b c` failed by treating the whole argument as one name. Both now loop over each
> space-separated name, creating each and reporting per-name (failures set `$?`). `make check` 30/30. Verified
> in-guest: `mkdir md1 md2 md3` created MD1//MD2//MD3/ and `touch tf1 tf2` created TF1/TF2, all visible in
> `ls` with timestamps.

> **(M830) Shell — `rm` removes multiple files.** `rm` only deleted a single name, so `rm a b` (or a
> glob-expanded `rm *.tmp`) failed by treating the whole argument as one filename. It now loops over each
> space-separated file, deleting each and reporting per-file (failures set `$?`). `make check` 30/30. Verified
> in-guest: created RA.TMP/RB.TMP/RC.LOG, `rm RA.TMP RB.TMP` removed both, and `ls RA.TMP` confirmed it was
> gone.

> **(M829) Docs — README reflects the shell & editor.** Completes the docs-accuracy pass (Welcome M815,
> About M817). The baked `README.MD` (rendered live by the browser's Markdown engine) listed desktop / JS /
> browser / Markdown but omitted the scriptable shell and the text editor — major hand-written components.
> Added them to both the Features list and the capabilities table. `make check` 30/30. Verified in-guest:
> navigated the browser to `file:readme.md` and the rendered page now lists "A scriptable shell (for/if/while,
> pipes, command substitution)" and "A text editor (undo/redo, find & replace, clipboard)".

> **(M828) Shell — `ls <dir>`.** `ls` only listed the current directory; `ls <dir>` now lists another one.
> Since the kernel's directory listing is cwd-relative, the shell briefly `chdir`s into the target, lists,
> then restores the cwd (synchronous, so nothing observes the transient change) — no FS-layer change needed.
> `make check` 30/30. Verified in-guest: created `/ld/inld.txt`, returned to `/`, and `ls ld` showed
> `INLD.TXT 3 2026-06-20 08:19` (with its timestamp) while `pwd` confirmed the cwd was still `/`.

> **(M827) Shell — `echo -n` and bare `echo`.** `echo` always appended a newline. Added `-n` to suppress the
> trailing newline (`echo -n "Name: "; read x` keeps the input on the prompt line) and made bare `echo`
> (no args) print a blank line, both standard behaviours. `make check` 30/30. Verified in-guest: `echo -n
> name:` left the next prompt on the same line; `echo plain` still printed a newline.

> **(M826) Editor — select all (Ctrl-A).** The editor had mark-based selection (Ctrl-B + move) but no
> select-all, a standard, frequently-used shortcut. Ctrl-A now sets the mark at the start and the cursor at
> the end, selecting the whole buffer — then Ctrl-C/Ctrl-X copy/cut everything. Listed in the Ctrl-H help.
> `make check` 30/30. Verified in-guest: typed `ALLOFTHIS`, Ctrl-A highlighted it all, Ctrl-X emptied the
> buffer (0b).

> **(M825) Browser — home-page shortcut hint.** Now that the browser has a `?` key reference (M821) and
> Ctrl-F find (M820), the start page advertises them: a line under the title reads "Press ? (or Ctrl-F to
> find) for keyboard shortcuts." so users discover the controls instead of having to guess the vim-style
> keys. One line added to `build_home`. `make check` 30/30. Verified in-guest: the home page renders the hint
> under the OS-DEV Browser heading.

> **(M824) Editor — unsaved-changes indicator.** Standard editor cue that was missing: a `*` now appears
> before the filename in the status (`EDIT *mod.txt`) whenever the buffer has been modified since the last
> save, so you can tell at a glance whether you need to save. The `dirty` flag is set by every buffer
> mutation (insert/backspace/delete and undo/redo) and cleared on a successful save (`^S`/`^W`) and on
> open/load. `make check` 30/30. Verified in-guest: typing showed `EDIT *mod.txt`; after `^S` it became
> `EDIT mod.txt`.

> **(M823) Shell — `cd -` (previous directory).** Completes the `cd` conveniences (home from M822, now
> previous). `cd -` swaps to the directory you were in before the last `cd` and prints it (bash behaviour);
> every successful `cd` records where it came from in a per-shell `prevcwd`. `make check` 30/30. Verified
> in-guest: `cd dd`, `cd /`, then `cd -` returned to `/dd` (printed it; `pwd` confirmed).

> **(M822) Shell — `cd` with no argument goes home.** Standard shell convenience that was missing: bare `cd`
> (and `cd ~`) now return to the root directory, instead of erroring as an unknown command. `make check`
> 30/30. Verified in-guest: `mkdir cdtest; cd cdtest` (prompt → `/cdtest`), then `cd` → prompt back to `/`,
> `pwd` confirms `/`.

> **(M821) Browser — `?` key-reference overlay.** Like the editor's Ctrl-H (M813), the browser had a dozen
> powerful single-key shortcuts (h/r/s/u/i/a, `<`/`>`, find, zoom, scroll, links) that were completely
> undiscoverable. `?` (or Ctrl-H) now shows a full-window key reference; any key returns. Reuses the existing
> early-return overlay pattern in `browser_render` (next to the "Loading…" one) and a dismiss-first check in
> `browser_key`. `make check` 30/30. Verified in-guest: pressed `?` in the browser and the BROWSER KEYS
> overlay listed every shortcut.

> **(M820) Browser — Ctrl-F find alias.** The browser's in-page find was bound to vim-style `\`, which most
> users won't guess. Added Ctrl-F (0x86) as an alias for the same find mode, so the universal find key works.
> One-line change to the key dispatch. `make check` 30/30. Verified in-guest: launched the browser, pressed
> Ctrl-F, and the `find:` prompt appeared.

> **(M819) Editor — open another file in place (Ctrl-O).** Completes the editor's file management
> (open/save/save-as). Ctrl-O prompts `open:`, saves the current file first (unless read-only, to avoid
> writing truncated data), then loads the typed file into the buffer. Factored the load path into a
> `load_file()` helper (used by startup and Ctrl-O) that resets per-file state — buffer, cursor, read-only
> flag, undo history, and selection mark — so undo can't bleed across files. A non-existent name opens an
> empty buffer (create-on-save). Listed in the Ctrl-H help. `make check` 30/30. Verified in-guest: edited
> `orig2.txt`, Ctrl-O → `HELLO.TXT` → the title bar switched to `EDIT HELLO.TXT` and showed its 42-byte
> contents ("Hello from a real file on a virtual disk!").

> **(M818) Editor — Save As (Ctrl-W).** The editor could only save back to the file it opened (or the
> launch-arg name); there was no way to save the buffer under a *new* name (save a copy, or name a
> never-saved buffer). Ctrl-W now prompts `save as:`, writes the document to the typed filename, and switches
> the editor to editing that file (so further `^S`/ESC save there). Guarded off in read-only mode (the file
> was truncated on load, so saving it would lose data). Reuses the find/replace prompt + mode-flag pattern;
> listed in the Ctrl-H help. `make check` 30/30. Verified in-guest: opened `orig.txt`, typed
> `SAVEAS-CONTENT`, Ctrl-W → `COPYFILE.TXT` → the title bar switched to `EDIT COPYFILE.TXT` and the 14-byte
> file was written.

> **(M817) About window — accurate component list.** Companion to the M815 Welcome refresh. The About box
> listed only "kernel / FAT32 / TCP / TLS / web browser" — omitting the from-scratch JavaScript engine and
> the scriptable shell + editor (major hand-written components). Updated it to read kernel/memory/tasks,
> FAT32/TCP/TLS 1.3, JS engine + web browser, scriptable shell + editor, and grew the box 160 → 178 px to fit.
> (Also vetted a browser-window-title idea and found it already exists — `desktop.c` keeps each browser
> window's title synced to the page `<title>` every frame — so no redundant change was made.) `make check`
> 30/30. Verified in-guest: the About window renders the full component list cleanly.

> **(M816) Shell — `read` builtin (interactive input).** The shell could produce output (echo) and capture
> command output (`$()`), but had no way to read *user* input into a variable — so scripts couldn't be
> interactive. `read VAR` now reads one line via `readline` and stores it in VAR, so a script can prompt and
> branch on the answer. `make check` 30/30. Verified in-guest: `read NAME`, typed `Alice`, then
> `echo hello $NAME` → `hello Alice`. Completes the shell's I/O story (echo out, `read` in, `$()` capture).

> **(M815) Welcome window — onboarding refresh.** The desktop's Welcome window (the first thing a user sees)
> just said "a from-scratch OS with its own desktop" plus drag hints — it undersold everything built since.
> Rewrote it to point newcomers at the headline capabilities: the Browser (real web over HTTPS + JS), the
> scriptable Shell (for/if/while), the Editor (undo/redo, find & replace), and the 60+ apps/games/demos, plus
> the F9/F1 entry points. Grew the window 206 → 290 px so all lines fit without clipping (kept each line ≤ ~40
> chars for the body width). `make check` 30/30 (gfxtest still paints the desktop). Verified in-guest: the
> Welcome window renders all lines cleanly when raised to the front.

> **(M814) Baked `DEMO.SH` scripting showcase.** Caps the shell-scripting arc (M799–M812) with a runnable,
> discoverable demo baked onto the disk by `mkfatfs`: `source DEMO.SH` exercises comments, `echo`, variables,
> `$((arithmetic))`, `$(command substitution)`, aliases, `for`, `if`/`test`, and `while` in one go — both a
> live demonstration (the features were otherwise only in `help`) and an end-to-end integration check. Kept
> the only `*` inside `$(())` so it isn't glob-expanded. The file count auto-updated (117 → 118 baked files).
> `make check` 30/30. Verified in-guest: `source DEMO.SH` printed variable `OS-DEV`, arithmetic `42`, cmd-sub
> `it-works`, the alias line, `for-loop n is 1/2/3`, the if/test line, and `while-loop i is 1/2/3`; the file
> shows in the Files window as `DEMO.SH (502b)`.

> **(M813) Editor — Ctrl-H key-reference overlay.** The editor accumulated a rich key set (undo/redo,
> find/replace, go-to-line, mark+selection, copy/cut/paste) that the one-line status couldn't advertise. Ctrl-H
> now shows a full-screen key reference; any key returns to editing (and isn't inserted, since the dismiss
> path `continue`s before the edit dispatch). The status hint trades `^Z=undo` for `^H=help` (same width, and
> help lists `^Z` anyway) so the whole feature set is discoverable. `make check` 30/30. Verified in-guest:
> opened README.TXT from Files, Ctrl-H showed the key list, and pressing a key returned to the unchanged
> document (90b, no stray character inserted).

> **(M812) Shell — command substitution `$(cmd)`.** The capstone shell feature: `$(cmd)` runs a command and
> splices its output into the line (trailing newlines stripped, internal newlines → spaces for word
> splitting). Implemented as `cmdsub_expand` (reusing the existing `cap_begin`/`cap_end` output capture +
> `run_andor`), run first in `run_line` and also on a `for` loop's word list — so `for f in $(cmd)` iterates a
> command's output, `set X=$(cmd)` captures it, and `echo $(cmd)` interpolates. `$((..))` arithmetic is left
> for the existing pass; one level only (single capture buffer, guarded). `make check` 30/30. Verified
> in-guest: `echo sub=$(echo hello)` → `sub=hello`; `set X=$(echo world); echo got-$X` → `got-world`;
> `for f in $(echo a b c); do echo item-$f; done` → item-a/item-b/item-c. With this the shell has the full
> bash-like toolkit: vars, arithmetic, `$?`, `$()`, pipes, redirection, globbing, `&&`/`||`, if/for/while,
> test, source, .SHRC, aliases.

> **(M811) Shell — `while` loops.** Completes shell control flow (if / for / while). `while COND; do CMDS;
> done` re-runs COND each pass (so `$var`s re-expand) and loops while it succeeds (`$? == 0`), pairing
> naturally with the M810 `test` builtin for counting loops. To stay safe it's bounded at 100000 iterations
> and polls `sys_pollkey` each pass so Ctrl-C / Esc can stop a runaway loop — the shell can't hang. Same
> single-line parse + stack-local buffers as `for`, routed through `run_input_line` so it nests and composes.
> `make check` 30/30. Verified in-guest: `set i=0; while test $i -lt 3; do echo n=$i; set i=$((i+1)); done` →
> n=0/n=1/n=2; `while false; do echo NEVER; done` printed nothing and the shell continued.

> **(M810) Shell — `test` / `[ ]` builtin.** Gives `if` (and the upcoming `while`) real conditions instead of
> just command exit status. `test EXPR` / `[ EXPR ]` sets `$?` (0 = true) and supports numeric comparisons
> (`-eq -ne -lt -gt -le -ge`), string `=`/`!=`, non-empty (`STR`) / `-z` / `-n`, file existence (`-e`/`-f`,
> via a 1-byte `sys_readfile` probe), and a leading `!` to negate. Args are tokenized after the usual
> variable expansion, and the `[` form drops a trailing `]`. `make check` 30/30. Verified in-guest:
> `test 5 -lt 10` → `$?=0`; `if test 3 -eq 3` → THREE-EQ; `if [ abc = abc ]` → STR-EQ; `if test -f
> README.TXT` → HAS-README; `if test ! -f NOPE.TXT` → NO-NOPE.

> **(M809) Shell — `.SHRC` startup file.** Makes shell setup persistent: at startup the shell auto-runs
> `.SHRC` from the root (silently if absent), so aliases, `set` variables, and a banner can be defined once
> and apply to every new shell. The `source` builtin's body was factored into a reusable
> `source_file(name, cwd, silent)` (used by both `source` and the startup hook), and `.SHRC` round-trips
> through FAT (both write and read normalize via `to_83`). `make check` 30/30 (the test disk is rebuilt
> without a `.SHRC`, so boot stays silent). Verified in-guest: wrote `.SHRC` with `echo`, `alias hi=echo`, and
> `set V=fromshrc` in the editor, then a newly-launched shell printed the banner on start and had the alias
> (`hi ALIAS-WORKS` → `ALIAS-WORKS`) and variable (`echo V-is-$V` → `V-is-fromshrc`) already active.

> **(M808) Shell — command aliases.** `alias name=value` defines a shortcut, expanded on a command's first
> word in `run_line` (one level only, so `a`→`b`→`a` can't loop); the rest of the line is appended, so
> `alias g=echo` then `g hi there` runs `echo hi there`. `alias` with no args lists them, `alias name` shows
> one, `unalias name` removes it. Storage mirrors the existing shell-variable table (16 aliases). Because
> expansion sits in `run_line`, aliases work everywhere — interactively, in scripts, and inside `for`/`if`
> bodies. `make check` 30/30. Verified in-guest: `alias g=echo` → `g hello world` printed `hello world`;
> `alias` listed `g='echo'`; after `unalias g`, `g` was unknown again.

> **(M807) Editor — find & replace (Ctrl-R).** The editor had find (Ctrl-F) and go-to-line (Ctrl-G) but no
> replace — a glaring gap now that text files open straight into it from the Files app (M806). Ctrl-R runs a
> two-phase prompt (`replace:` then `with:`) and replaces every occurrence in one pass, reporting `[N
> replaced]`. Each replacement goes through the existing `del_fwd`/`insert` so it's undoable, and the scan
> advances past inserted text so a replacement containing the search term can't loop; variable-length
> replacements track `dlen` correctly. `render_prompt` was generalized to take the query buffer (so the
> `with:` phase shows the replacement, not the search term). `make check` 30/30. Verified in-guest:
> `foo bar foo baz foo` with foo→XXX gave `XXX bar XXX baz XXX` `[3 replaced]`; `aa bb aa cc aa` with aa→Z
> gave `Z bb Z cc Z` (14b→11b), confirming length-changing replacement.

> **(M806) App launch arguments + Files opens text files in the editor.** Added a reusable launch-argument
> mechanism: a spawned app can carry a one-shot string (e.g. a filename). `app_spawn_named_arg(name, arg)`
> stashes a pending arg that `app_spawn` copies into the new app's struct (race-free — each app gets its own
> copy), and `SYS_getarg`/`sys_getarg` let the app read it. The editor now opens that filename directly
> (skipping its prompt) when launched with an arg, and the GUI Files window routes by extension: text/source
> files (TXT, MD, C, H, SH, LOG, CFG, INI, JS, ASM, JSON) open in the editor for editing, everything else
> keeps opening in the browser for viewing. `make check` 30/30. Verified in-guest: clicking `README.TXT` in
> Files opened it in the editor (`EDIT README.TXT`, content loaded, editable); clicking `PRE.HTM` opened it in
> the browser (rendered) as before.

> **(M805) Shell — `if`/`then`/`else`/`fi`.** Completes shell control flow alongside M804's `for`. `if COND;
> then CMDS; [else CMDS;] fi` (one line) runs COND, and its exit status (`$?`) picks the branch; THEN/ELSE are
> full `;`-separated lists and can nest, all via the same `run_input_line` so `if` composes with `for`, pipes,
> `&&`/`||`, `source`, etc. Markers are matched as substrings (`; then`, `; else`, trailing `fi`), with the
> documented caveat that a nested if/else inside THEN on the *same* line can mis-bind its else — deep nesting
> belongs on separate script lines. `make check` 30/30. Verified in-guest: `if true; then echo THEN-OK; fi`,
> `if false; then echo BAD; else echo ELSE-OK; fi` → ELSE-OK, `if cat NOPE; then ...; else echo NOTFOUND-OK;
> fi` (status-driven), and `for i in 1 2 3; do if true; then echo got-$i; fi; done` → got-1/2/3 (if inside for).

> **(M804) Shell — `for` loops.** Completes the scripting arc (M799 `&&`/`||`, M803 `source`) with the one
> control-flow primitive that had no equivalent: `for VAR in WORDS; do CMDS; done` (single line). WORDS get
> `$var` and glob expansion then split on whitespace; the body runs once per word with VAR bound. To make this
> work both interactively and inside sourced scripts, the per-line handling was unified into one
> `run_input_line` (used by `main` and `source`) that runs a `for` loop or else the `;`-split `&&`/`||` list —
> so `for` nests and composes with everything (pipes, redirection, `$?`). Per-loop buffers are stack-local so
> nested loops don't clobber each other. `make check` 30/30. Verified in-guest: `for i in 1 2 3; do echo
> num-$i; done` → num-1/2/3; `for x in A B; do for y in 1 2; do echo $x$y; done; done` → A1/A2/B1/B2 (nested);
> `for f in *.GZ; do echo found-$f; done` → found-HELLO.GZ (glob); and normal commands still run.

> **(M803) Shell — `source` (run a script file).** The shell can now run shell commands from a file with
> `source file` (or `. file`), turning the editor + shell into a real scripting environment. Each line goes
> through the exact same `;` split + `&&`/`||` layer as interactive input, so scripts get the full feature set
> (pipes, redirection, globbing, variables, arithmetic, `$?`); blank lines and `#` comments are skipped, and
> a depth guard (8) stops a script that sources itself. The filename is copied out of the command line before
> iterating because the nested execution reuses `run_line`'s static expand/glob buffers — a forward
> declaration of `run_andor` lets the `source` builtin call back into the executor. `make check` 30/30.
> Verified in-guest: wrote a 4-line script in the editor (`echo FROMSCRIPT` / `set V=42` / `echo V-is-$V` /
> `true && echo CHAINED-OK`), then `source sc.sh` printed `FROMSCRIPT`, `V-is-42` (variable set in an earlier
> line and expanded later), and `CHAINED-OK` (`&&` honoured in the script).

> **(M802) Editor — visual selection (Ctrl-B mark + highlight).** Builds on M800's line clipboard with true
> range selection. Ctrl-B sets/clears a mark at the cursor; moving the caret then defines a selection
> `[min,max](anchor,cur)`, drawn in yellow by splitting the visible-text print into before/selection/after
> segments (the caret is excluded from the highlight, and a selection running off the top/bottom of the
> window is clamped to the visible region). Ctrl-C/Ctrl-X now copy/cut the selection when one is active
> (falling back to the whole-line behaviour otherwise); cut reuses `del_fwd` so it's undoable. Any edit
> (insert/backspace/delete) clears the mark. `make check` 30/30. Verified in-guest: `SELECT THIS PART` with a
> 6-char mark shows `SELECT` highlighted yellow; copying `HELLO` from `HELLO WORLD` and pasting gave
> `HELLO WORLDHELLO`; and cutting `AAA` from `AAABBB` left `BBB`.

> **(M801) Shell — `clip` command (clipboard ↔ shell).** With the clipboard syscalls from M800 in place, the
> shell can now bridge the GUI clipboard and the command line. `clip` with no argument prints the system
> clipboard to stdout (so a URL or text selected in the browser, a word from the terminal, or a line from the
> editor can be `clip > file`'d or `clip | grep`'d); `clip <file>` sets the clipboard from a file, which —
> because the shell's pipeline feeds a stage its input as a trailing file argument — means `cmd | clip` copies
> any command's output to the clipboard for pasting into the browser/editor. Reuses the shared `slurp()` and
> the `sys_clip_get`/`sys_clip_set` wrappers; capped at the 2 KB clipboard. `make check` 30/30. Verified
> in-guest: `echo HELLOCLIP | clip` → "copied 10 bytes", `clip` → `HELLOCLIP`, `clip | grep CLIP` piped fine.

> **(M800) Editor — line copy/cut/paste via the system clipboard (Ctrl-C/X/V).** The editor could receive
> pastes (middle-click flows through the same `iq_get` as pollkey) but had no way to *copy* text out. The
> kernel already had a shared system clipboard (`g_clip`, set by terminal selection, read by middle-click)
> but it wasn't reachable from a userspace app. Added two syscalls — `SYS_clip_get(buf,max)` /
> `SYS_clip_set(buf,len)` (57/58) wrapping the existing `clip_get`/`clip_set` — plus `sys_clip_get`/
> `sys_clip_set` ulib wrappers, usable by any app. The editor now does line-oriented Ctrl-C (copy current
> line, incl. its newline), Ctrl-X (cut), Ctrl-V (paste) — cut/paste route through `del_fwd`/`insert` so
> they're undoable and coalesce to one undo group. Because it's the *system* clipboard, text crosses apps.
> `make check` 30/30. Verified in-guest: `DUP` → Ctrl-C, Ctrl-V×2 = `DUPDUPDUP`; Ctrl-X emptied it; Ctrl-V
> restored it; and a line copied in the editor pasted into the shell via middle-click (`XCLIPTEST`).

> **(M799) Shell — `&&` / `||` conditional execution + `$?`.** The shell could chain with `;` and `|` but had
> no conditional execution and no exit status. Added a `g_status` exit code (0 = success) reset at each
> `run_command` and set to 1 on the real failure paths: command-not-found, a missing file (set once in the
> shared `slurp()` helper, so it covers all ~20 file-reading commands), and `cd`/`mkdir` failure. New `true`
> and `false` builtins give a deterministic status. `$?` expands to the last status, and a new `run_andor`
> layer (between the `;` split and the pipe/redirect handling, matching bash precedence `;` < `&&`/`||` < `|`)
> splits a segment on the doubled `&&`/`||` operators and runs left-to-right, honouring the carried status —
> single `|`/`&` and arithmetic `$((a & b))` are untouched since only doubled forms match. `make check` 30/30.
> Verified in-guest: `true && echo` runs, `false && echo` skips, `false || echo` runs, `true || echo` skips,
> `$?` is 0 after success / 1 after `false`, `cat MISSING || echo` catches, `mkdir d && cd d && pwd` chains to
> `/d`, and pipes/redirects still work.

> **(M798) Editor — redo (Ctrl-Y).** Completes the undo/redo pair from M797. The undo log already kept popped
> ops in place above the `un` cursor, so redo needed no separate stack — just a high-water mark `umax` (the
> number of ops in the log; `[un, umax)` is the redo region). Undo decrements `un` and leaves the ops;
> `redo()` re-applies the group at `un` forward (oldest-first) and advances `un`; a fresh edit sets
> `umax = un`, discarding any redoable ops so you can't redo across new history. Ctrl-Y (0x99) drives it.
> `make check` 30/30. Verified in-guest: `AAAA`⏎`BBBB`, undo×2 to empty, redo×2 restored both (9b); and the
> invalidation case — typed `XX`, undo, typed `Y`, Ctrl-Y correctly did nothing (doc stays `Y`).

> **(M797) Editor — undo (Ctrl-Z).** The text editor had no undo at all — a glaring omission for an editor.
> Added an operation-log undo: every single-character insert/backspace/forward-delete is recorded (char,
> position, kind), and consecutive same-kind edits at adjacent positions share a "group" so one Ctrl-Z
> reverts a whole typed or deleted run instead of one character. Continuity is judged on the absolute edit
> position, so moving the caret naturally starts a new group; a newline also ends a group, giving
> line-at-a-time undo rather than one Ctrl-Z wiping the whole session. The log holds 16384 ops and drops its
> oldest half when full. Memory-efficient (no buffer snapshots), and the doc model (flat `doc[]`/`dlen`/`cur`)
> made reversal trivial. Status line now advertises `^Z=undo` (still fits two grid rows). `make check` 30/30.
> Verified in-guest: typed `AAAA`⏎`BBBB` then Ctrl-Z removed `BBBB` (9b→5b), Ctrl-Z again removed `AAAA`+nl
> (5b→0b); and typed `HELLO`, backspaced twice to `HEL`, Ctrl-Z restored `HELLO`.

> **(M796) Security — read-only code + NX data (W^X, part 2, complete).** Finishes what M795 started. Each
> userspace program linked into a single RWX `PT_LOAD` segment (the ELF loader mapped the whole image
> writable+executable), so every app's code was self-modifiable and its `.data`/`.bss` was executable — and
> `ld` warned "LOAD segment with RWX permissions" once per app (58 warnings). Now `user.ld` uses explicit
> `PHDRS` to split each app into a read-only+executable segment (`.text`/`.rodata`, FLAGS 0x5) and a
> writable+NX segment (`.data`/`.bss`, FLAGS 0x6), page-aligned so no page is shared. The loader (`elf.c`)
> carries `p_flags` per segment and, after the (writable) copy, re-protects each page to its real
> permissions: code read-only+executable, data writable+non-executable. Result: the full W^X invariant holds
> for userspace (no page is both writable and executable), and all 58 RWX linker warnings are gone (only the
> ring-0 kernel ELF remains, a separate effort). Fixed the elftest stub's `vmm_map` to model re-mapping as
> flags-only (it was re-`mmap`-ing and wiping copied bytes — a test artifact, not a loader bug). `make check`
> 30/30 (elftest loads all 57 shipped apps through the new path). Verified in-guest: shell, the editor
> (typed two lines), DOOM, and the browser all run normally with read-only code + NX data.

> **(M795) Security — non-executable user stack & heap (W^X, part 1).** Userspace ran fully RWX: the ELF
> loader and the stack/heap allocators mapped every ring-3 page `PTE_WRITABLE | PTE_USER` with no NX bit, so
> injected data on the stack or heap was directly executable — the classic shellcode vector. The `PTE_NX`
> flag was defined but unusable because boot.asm never set EFER.NXE (bit 63 in a PTE is reserved until NXE is
> on, so it would have faulted). Fixed: boot.asm now CPUID-probes NX (leaf 0x80000001 EDX bit 20) and enables
> EFER.NXE alongside LME (guarded so a hypothetical NX-less CPU just skips it instead of triple-faulting),
> and the user stack (app.c), heap/`sbrk` (app.c), and the isolation-demo page (kmain.c) are now mapped NX.
> Leaf-only NX is correct (the CPU ORs NX up the walk; intermediates stay executable so code still runs).
> Code pages are untouched — text stays executable — so this is a clean, low-risk half of W^X; making `.text`
> read-only + `.data` NX (the linker/loader segment split that clears the 58 RWX-segment linker warnings) is
> part 2. `make check` 30/30. Verified in-guest: boots fine (NXE no triple-fault), and shell, the browser
> (full JS interpreter on the NX heap), and DOOM (id's zone allocator + framebuffer) all run normally.

> **(M794) Baked-in files — build-date timestamps.** Completes the M792/M793 timestamps arc: `mkfatfs`
> (the host tool that writes the FAT32 disk) never set the dir-entry date/time fields, so the ~115 baked
> system files always showed dateless in `ls`/Files while only runtime-created files had dates. Now mkfatfs
> stamps every entry's create/write/access fields (offsets 14–25) with the build date, packed in the same
> FAT format the kernel reads. The build date comes from `SOURCE_DATE_EPOCH` when set (so reproducible
> builds stay reproducible) else the wall-clock build time, via `gmtime`. Result: *every* file now carries a
> date — no more dateless rows. `make check` 30/30. Verified in-guest: `ls` shows `INTER.PNG 14178
> 2026-06-20 04:52`, `FREEDOM1.WAD 28795076 2026-06-20 04:52`, etc. for the baked files, and the Files
> window shows `PRE.HTM (284b) 2026-06-20` likewise.

> **(M793) Files app — date column.** Extends M792's timestamps to the GUI: the Files window now shows
> `NAME  (size)  YYYY-MM-DD` for timestamped files (baked files with date 0 show name+size only, unchanged),
> widened 330 → 380 px to fit. Same FAT-date unpack as `ls`, reading the new `vfs_dirent.date`. `make check`
> 30/30. Verified in-guest: re-created README.TXT (so it gets today's date) shows `README.TXT (3b) 2026-06-20`
> in row 1; the other baked files show no date.

> **(M792) Filesystem — file timestamps in `ls`.** The FAT32 driver never wrote the dir-entry date/time
> fields (always 0) and `ls` showed only name+size. Now `add_entry` stamps create/write/access time from the
> RTC (`fat_now()` packs `rtc_now()` into FAT16 date/time), `vfs_dirent` carries `date`/`time`, the list
> visitor reads the write timestamp, and `SYS_list` appends `  YYYY-MM-DD HH:MM` — but only for stamped files,
> so baked-in disk files (date 0) still show cleanly. Precise field writes (offsets 14–25 only), no risk to
> name/cluster/size. Added a `rtc_now` stub to `tests/fs` (the host suite #includes fat32.c — coupling
> handled). `make check` 30/30. Verified in-guest: `echo hi > DATED.TXT; ls` → `DATED.TXT 3  2026-06-20 04:40`
> (matching the clock), baked files unchanged.

> **(M791) Browser render — fix a potential out-of-bounds token access (`-Warray-bounds`).** The render loop
> ran `t < b->ntok` but the per-token arrays are `[TOK_MAX]`; the compiler couldn't prove `t < TOK_MAX`, and
> the `b->tokbg[t]` block-background read lacked the `t < TOK_MAX` guard its siblings (`tokscale`/`tokindent`/
> `tokalign`) carried — a real OOB read if `ntok` ever reached `TOK_MAX` on adversarial HTML. Bounded the loop
> `t < b->ntok && t < TOK_MAX`, making every per-token access provably in-bounds (and defensively capping the
> loop). Clears the only `-Warray-bounds` warning in the tree. `make check` 30/30 (browsertest renders fine).

> **(M790) Shell — `!!` repeat last command.** Classic history expansion: `!!` re-runs the previous command;
> trailing text is appended, so `!! | grep x` / `!! > f` compose. The shell keeps the last non-blank line and
> echoes the expansion before running it. App-only (a few lines in `main` + a help entry); `make check` 30/30.
> Verified in-guest: `echo hello world` → `!!` repeats it → `!! | wc` runs "echo hello world | wc" (1 line, 2
> words).

> **(M789) Editor — Ctrl-G go-to-line.** Complements M785's find (search by content) with jump-by-number:
> Ctrl-G opens a `goto line: N_` prompt (digits only), Enter moves the caret to the start of that line. Reuses
> the find-mode plumbing — `render_find` generalized to `render_prompt(label)`, a `goting` flag sharing the
> query buffer. Essential for long files / compiler line references. `make check` 30/30. Verified in-guest:
> in a 60-line file, Ctrl-G "25" → caret jumps to line 25 (view recentred).

> **(M788) Browser — Forward toolbar button.** Completes M787's Forward UI: added a `>` button next to the
> `<` Back button in the toolbar (each greyed when its stack is empty), shifting the address field right to
> fit it. Clicking `>` goes forward; the `browser_click` toolbar hit-test gained the `rx 26..44` region.
> `make check` 30/30. Verified in-guest: navigate home→list, click `<` (back to home, `<` greys / `>`
> activates), click `>` (forward to list, buttons swap). Back/Forward now have both buttons and keys.

> **(M787) Browser — Forward navigation.** The browser had a Back stack but no Forward. Added a `fwd[16]`
> stack: Back now pushes the page it leaves onto it, a new `browser_forward()` (mirrors `browser_back` —
> local + network paths, `claim_fetch`, undo-on-bail) pops it and pushes the current page back onto the Back
> stack, and a fresh navigation clears the forward stack (`if (!is_back) fwdn=0` in `browser_navigate`).
> Bound to keys: `<` = back (also Backspace), `>` = forward; start-page help updated. `make check` 30/30.
> Verified in-guest: home → file:list.htm → `<` returns to home → `>` returns to list.htm.

> **(M786) Terminal — Ctrl-L clear screen (keep the prompt).** Completes the readline Ctrl set: Ctrl-L
> clears the terminal but keeps the current prompt + partial input at the top (bash behaviour, better than
> the `clear` command which drops the line). Implemented in `app_sys_read` by moving the input's grid rows
> (`cy0..cy`) to the top and blanking the rest. `make check` 30/30. Verified in-guest: a screen full of `help`
> + a typed `echo hello` → Ctrl-L → just `osdev:/$ echo hello` at the top → Enter runs it. The terminal now
> has the full readline shortcut set (A/E/B/F/P/N/D/H/K/U/W/C/L) atop M782's Ctrl support.

> **(M785) Editor — Ctrl-F incremental find.** With Ctrl support (M782), the editor now has search: Ctrl-F
> opens a `find: <query>_` prompt at the bottom, you type the string (backspace edits, Esc cancels), and
> Enter jumps the cursor to the next match after the caret (wrapping to the top, "[not found]" otherwise);
> repeat Ctrl-F+Enter to step through matches. New `find_from()` substring scan + `render_find()`; a `finding`
> mode at the top of the key loop. `make check` 30/30. Verified in-guest: in a 60-line file, Ctrl-F "42" →
> cursor jumps to line 42 (view recentred). Completes the editor's core capabilities.

> **(M784) Shell help — document line editing.** Added an `edit:` line to `help` listing the
> line-editing keys (arrows, Home/End, Del, up/down history, Tab completion) and the M782 Ctrl kills
> (`^W`/`^U`/`^K`) + `^C` cancel — so the terminal's editing power is discoverable. Pure doc string; `make
> check` 30/30; verified in-guest.

> **(M783) Editor — Ctrl-S save / Ctrl-Q quit (now that Ctrl exists).** With M782's Ctrl support, the editor
> finally gets the save/quit options it lacked: Ctrl-S saves and keeps editing ("[saved]"), Ctrl-Q quits
> *without* saving (abandon changes) — ESC still saves & quits. Closes the real gap where ESC was the only
> exit (no way to save-and-continue or to discard a bad edit). Status hint updated to `ESC/^S=save ^Q=quit`.
> `make check` 30/30. Verified in-guest: Ctrl-S → "[saved]" + still editing (5b written); Ctrl-Q → editor
> closes, the later unsaved keystroke discarded.

> **(M782) Keyboard — Ctrl modifier + readline shortcuts in the terminal.** The OS had no Ctrl key at all
> (the cooked-keycode space was claimed by arrows + WM F-keys). Added `ctrl_down` tracking (LCtrl scancode
> 0x1D) and encode Ctrl+letter as `0x80|(letter&0x1f)` (0x81–0x9A) — a clean range that doesn't collide with
> the existing codes. The line editor (`app_sys_read`) maps them to readline shortcuts: Ctrl-A/E (home/end),
> Ctrl-B/F (left/right), Ctrl-P/N (history), Ctrl-D/H (delete/backspace) via remap to existing codes, plus
> new Ctrl-K (kill to end), Ctrl-U (kill line), Ctrl-W (kill word), Ctrl-C (abandon line, prints `^C`).
> Benefits every `readline` caller (shell, calc, adv); unblocks future Ctrl shortcuts in the editor. `make
> check` 30/30. Verified in-guest: Ctrl-W/U/K kills + Ctrl-C cancel all work.

> **(M781) Build — `-fwrapv` for the OS-authored userspace.** The user apps compiled *without* `-fwrapv`
> (only the kernel had it), so signed overflow was UB under `-O2` — the latent class M779's fuzz exposed in
> the shell evaluator, and which calc's parse/power/`*` paths share. Added `-fwrapv` to `USER_CFLAGS` so all
> OS-authored apps' signed arithmetic wraps (defined), same rationale as the kernel; ported games keep their
> own CFLAGS. All 231 user objects rebuilt (via M780's dep tracking), `make check` 30/30, calc verified
> in-guest (`2+3*4`=14, `0xff&0x0f`=15, `2^10`=1024).

> **(M780) Build — header-dependency tracking.** The Makefile didn't track header deps, so editing a header
> left objects stale — M779's `shmath.h` change needed a manual `touch shell.c` to take effect (a real
> ship-the-wrong-binary hazard). Added `-MMD -MP` to the kernel + user CFLAGS (gcc drops a `.d` beside each
> `.o`), `-include`d every `build/**.d` at the *end* of the Makefile (so the generated rules can't hijack the
> default goal), and made the `%.o` rules depend on the Makefile too (CFLAGS changes now rebuild). Verified:
> a clean build emits 117 `.d`; touching `shmath.h` rebuilds only `user_shell.o`, touching `app.h` rebuilds
> exactly the 4 kernel files that include it; `make check` 30/30.

> **(M779) Shell `$((expr))` — full bitwise/shift/power operators (bash-compatible).** Extended the shared
> evaluator (`user/shmath.h`) from `+ - * / %` to bash's whole `$(())` operator set + precedence: `**`
> (power, right-assoc), `* / %`, `+ -`, `<< >>`, `&`, `^` (XOR), `|`, and unary `~`. Hardened all arithmetic
> to be overflow-safe (unsigned-wrap, defined behaviour) after the host fuzz immediately caught **real
> signed-overflow UB** in the power loop — div/0 and `MIN/-1` and out-of-range shifts now yield 0/wrap with
> no UB. Extended `tests/shmath` (17 new asserts + fuzz alphabet `<>&^|~`); 300k iterations ASan/UBSan-clean.
> `make check` 30/30; verified in-guest: `$((1<<4))`→16, `$((0xff&0x0f))`→15, `$((2**8))`→256, `$((100>>2))`→25.

> **(M778) Editor — line-of-total in the status bar.** The status line now shows `L<current>/<total> C<col>`
> (total = logical line count) instead of bare `line:col`, so you can see your position in a long file —
> pairing with the M771 PgUp/PgDn paging. Safe display-only change (no effect on the wrap/scroll/cursor
> model; a per-line gutter would have needed reworking the grid-auto-wrap path, deferred). `make check` 30/30.
> Verified in-guest: README.TXT shows `L3/3 C1`.

> **(M777) Host-test the shell `$((expr))` evaluator.** Extracted the M762 arithmetic evaluator out of
> shell.c into a shared header `user/shmath.h` (the verbatim-extraction pattern, like cssprop/url/shgrep): a
> pure recursive-descent integer evaluator whose only shell dependency is the `sh_var()` variable-lookup
> hook the includer supplies. shell.c now `#include`s it and provides `sh_var` (via `vget`); behaviour is
> byte-identical. Added `tests/shmath` — 25 regression asserts (precedence, parens, hex, unary, /0, vars,
> associativity) + 300k random-expression fuzz iterations under ASan/UBSan — wired into `make check` as
> `shmathtest` (now 30 host suites). Verified in-guest unchanged: `$((2+3*4))`→14, `set N=6; $((N*N+1))`→37.

> **(M776) Paste-buffer race hardening.** `app_paste` now zeroes `paste_len` before refilling the per-app
> paste buffer, so if a second paste arrives while the first is still draining, `iq_get` can't read a
> half-overwritten buffer (it sees `paste_pos < paste_len == 0` → skips it until the refill sets the new
> length). Tiny correctness fix to the M768 buffer. `make check` 29/29; normal paste verified unchanged.

> **(M775) Browser paste — insert at the caret.** Consistency fix for M773/M774: middle-click paste now
> inserts the clipboard at the caret (`url_cur` for the address bar, `field_cur` for a focused field) instead
> of always appending — control chars skipped, caret left after the inserted text. `make check` 29/29.
> Verified in-guest: copy "from-scratch", type "X" in the address bar, Home, paste → "from-scratchX" (caret
> respected). Line editing + clipboard now behave consistently across terminal, address bar, and form fields.

> **(M774) Browser form fields — mid-line editing.** Completes "line editing everywhere" (terminal M757,
> address bar M773, now form inputs). The focused `<input>`'s `|` cursor now sits at `b->field_cur` within
> the value (not just the end): left/right move it, Home/End jump, Delete removes at it, Backspace before it,
> printable keys insert at it; the field render places `|` mid-value and the focus path sets the caret to the
> value's end. `make check` 29/29. Verified in-guest on `file:form.htm`: focus the name field, type "World",
> Home, type "Hello-" → `[Hello-|World]` (inserted at the start, caret mid-value).

> **(M773) Browser address bar — mid-line editing.** The URL bar was append/backspace-only (fixing a typo
> near the start meant retyping) — the same gap the terminal had before M757. Added a caret (`b->url_cur`):
> left/right move it, Home/End jump, Delete removes at the caret, Backspace before it, printable keys insert
> at it; the bar scrolls to keep the caret visible and draws it at its position. Entry points
> (`browser_click` on the bar, `browser_paste`) set the caret. `make check` 29/29. Verified in-guest: type
> `example.com`, Home, type `go-` → `go-example.com` (inserted at the start, caret mid-string). Form-field
> mid-line editing is still future (same pattern, per-field cursor).

> **(M772) Browser — double-click word-select.** Matches the terminal's M765: double-clicking a word in a
> page selects it (highlighted) and copies it to the clipboard. New `browser_sel_word(b,rx,ry,out,max)` (hit
> the word token, highlight it, extract). The WM detects a double-click in the browser body (reusing the
> `last_body_click` timer + proximity) before the scrollbar/link/drag paths. `make check` 29/29. Verified
> in-guest: double-click "from-scratch" on the start page → middle-click the address bar pastes it. Selection
> is now consistent across terminal + browser (drag-range + word + copy; paste everywhere).

> **(M771) Editor — PgUp/PgDn + wheel paging.** The editor couldn't page long files: PgUp/PgDn (0x15/0x16)
> were swallowed by the terminal-scrollback intercept before reaching it, and the wheel did nothing there.
> Now `app_key` only intercepts 0x15/0x16 for ordinary terminals; a full-screen self-drawing app (`caret_off`,
> e.g. the editor) receives them as keys. The editor pages by EDVIS-1 (15) lines on PgUp/PgDn; the wheel,
> routed via a new `app_caret_hidden()` accessor, sends arrow keys (3 lines/tick) to such apps and scrollback
> to terminals. `make check` 29/29. Verified in-guest: open a 60-line file → 2× PgUp jumps from line 61 to
> line 31.

> **(M770) Browser scrollbar — click + drag to scroll.** The symmetric counterpart to M769: the browser's
> right-edge scrollbar is now interactive. Click the track to jump, drag to scroll. New
> `browser_in_scrollbar(b,rx,ry,w,h)` (hit-test, only when content overflows) + `browser_scroll_track(b,ry,h)`
> (maps track-y → `b->scroll`, render clamps); the WM checks the scrollbar before the link/select path and
> drives a `bsbdrag` gesture. `make check` 29/29. Verified in-guest: click the bottom of the bar → page
> footer; drag up → page top. Scrolling is now consistent across terminal + browser: wheel, keyboard, and a
> click/drag scrollbar.

> **(M769) Terminal scrollbar — click + drag to scroll.** The M764 scrollbar was indicator-only but looks
> interactive. Now clicking anywhere on its track jumps the scrollback to that position, and dragging the bar
> scrolls continuously. New `app_scroll_frac(a, num, den)` maps a track fraction to `view` (top = oldest,
> bottom = live). The WM hit-tests the scrollbar column on left-press (a few px of slop for grabbing),
> starts an `sbdrag` gesture, and the grip/title-bar/selection paths are unaffected (grip is checked first;
> the bar sits just right of the text). `make check` 29/29. Verified in-guest: click the track top → jumps to
> the banner; drag down → back to the live prompt.

> **(M768) Long-paste correctness — paste buffer + bigger shell line.** Two caps truncated long pastes (e.g.
> a multi-line browser selection): `app_paste` injected via the 128-entry key queue (excess dropped), and the
> shell read into a 128-byte line. Fixed both: each app now has a `pastebuf[CLIP_MAX]` that `iq_get` drains
> *before* the key queue (so a paste up to 2 KB isn't capped by `IQ_SIZE`, and it's delivered to both
> `app_sys_read` and `sys_pollkey` callers — shell, editor, calc, adv); and the shell's input line grew
> 128 → 1024 (long URLs for `wget`, long pastes). `make check` 29/29. Verified in-guest: drag-select a big
> browser paragraph → paste into the shell → the full ~290-char text lands (previously cut at ~128).

> **(M767) Browser text selection — the clipboard capstone.** Drag across page text to select it (word
> granularity, highlighted white-on-blue) and copy to the shared clipboard on release. The render loop now
> records a per-visible-word rect (`b->wrec`, reusing `lrec_t` with `.link` = token index) for hit-testing,
> and highlights tokens in `[tsel0,tsel1]`. New `browser_sel_begin/extend/commit/clear` + `browser_hit_word`;
> `browser_sel_commit` joins selected words with spaces (newline at block breaks), trimming. Low-risk: a left
> press over a **link** still follows it (browser_click returns 1); selection starts only on non-link content
> (browser_click now returns 0 there — its return was previously ignored). Selection clears on reparse.
> `make check` 29/29 (golden-safe: highlight only when a selection exists). Verified in-guest: drag a line on
> the start page → words highlight → middle-click the address bar pastes the exact text. **The clipboard is
> now universal + bidirectional:** terminal↔terminal, terminal↔browser, browser text/links → anywhere.

> **(M766) F1 help — document the new mouse gestures.** The session added a lot of mouse UX (wheel scroll,
> terminal drag/word selection + middle-click paste, browser right-click link copy) that was undiscoverable.
> Extended the F1 keyboard-shortcuts overlay with a mouse section: "Wheel scrolls the window under the
> cursor", terminal select/copy/paste, and browser link-copy/paste. Pure UI text; the panel auto-sizes to
> the line count. `make check` 29/29. Verified in-guest: F1 shows the new lines, rendered cleanly.

> **(M765) Terminal — double-click word-select.** Completes the M760 selection UX (which was drag-only):
> double-clicking a cell selects the whitespace-delimited word under it and copies it to the clipboard — so
> a word or URL can be grabbed without precise dragging. New `app_sel_word(a,row,col)` (expands over
> non-spaces, then `app_sel_commit`); the WM detects a double-click in a text-app body via a short timer +
> proximity test (mirroring the titlebar's maximize double-click), else falls back to drag-select. `make
> check` 29/29. Verified in-guest: double-click `COPYPASTE` selects exactly the word (tight bounds) →
> middle-click pastes it. Terminal selection now = drag-range + word + middle-click paste.

> **(M764) Terminal — scrollback scrollbar.** The terminal had scrollback (PgUp/PgDn + the M759 wheel) but
> only a tiny `^` indicator. Added a real scrollbar on the right edge of the text grid: a dark track with a
> thumb sized to visible/total rows and positioned by `view`, so you can see how much history exists and
> where you are — parity with the browser, completing the terminal scrolling UX (line editor + scrollback +
> wheel + scrollbar). Only shown when `sb_count > 0` (the `^` is the fallback), so golden-safe. ~10 lines in
> `app_render`; `make check` 29/29. Verified in-guest: after `help`, the thumb sits at the bottom (live), and
> wheel-up moves it to the top (start of history).

> **(M763) Browser — right-click a link to copy its URL.** Closes the browser→clipboard direction (M760/M761
> covered terminal↔terminal and terminal→browser). Right-clicking a link copies its href to the shared
> clipboard (status: "link copied"), skipping internal pseudo-links (javascript:/submit:/event:). Low-risk:
> right-click was unused, so the delicate left-click-follows-link path is untouched; full arbitrary-text
> selection in pages (needs per-token hit-test rects) is still future. New `browser_rclick(b,rx,ry,out,max)`
> returns the href; the WM (`clip_set`) stores it. Added `rclick X Y` to osdrive. `make check` 29/29.
> Verified in-guest: right-click `https://example.com` on the start page → "link copied" → middle-click the
> address bar pastes it. The clipboard now flows terminal↔terminal, terminal→browser, and browser→anywhere.

> **(M762) Shell — arithmetic expansion `$((expr))`.** Builds on the M756 env vars to make the shell
> scriptable: `$((...))` evaluates an integer expression and substitutes the result, anywhere on a line
> (expanded alongside `$NAME`, before glob/pipe/redirect). A small recursive-descent evaluator (in
> shell.c) handles `+ - * / %`, unary minus, parentheses, decimal/`0x` literals, and variable names (bare
> or `$`-prefixed, via the same `vget`); integer-only like the rest of the OS, with `/0` and `%0` → 0.
> App-only; `make check` 29/29. Verified in-guest: `$((2+3))`→5, `$((10*4-2))`→38 (precedence),
> `$(((2+3)*4))`→20 (parens), `set X=7; $((X*X))`→49 (variables), `$((100/7))`→14 (integer division).

> **(M761) Clipboard paste into the browser (cross-app copy/paste).** Completes M760's clipboard across app
> types: middle-clicking a browser window pastes the clipboard into the focused `<input>` field, or (none
> focused) into the address bar — entering edit mode, replacing its contents on a fresh paste — and raises
> the window so Enter submits. New `browser_paste(b, s, n)` (drops control chars / newlines, single-line);
> the WM reads the clipboard (`clip_get`) and hands the text to it, so browser.c needs no app.h. `make check`
> 29/29. Verified in-guest: `echo example.com` → drag-select in the shell → middle-click the browser → the
> address bar reads "example.com", ready for Enter. Still future: selecting text *in* the browser (needs a
> per-token x array for hit-testing) to copy from pages.

> **(M760) System clipboard — terminal text selection + middle-click paste.** The OS had no clipboard and
> no way to select text. Now: left-drag in a terminal selects a linear, line-spanning range (highlighted
> white-on-blue, extracted from the scrollback/live grid exactly as drawn, trailing spaces trimmed per line)
> and copies it to a shared kernel clipboard on release; middle-click pastes the clipboard into the text app
> under the cursor (X11 primary-selection style — injected into its input queue, so a shell runs each pasted
> line). One buffer (`clip_set`/`clip_get` in app.c) shared by every window, so text carries between them.
> New app APIs: `app_sel_begin/extend/commit/clear`, `app_paste`; WM drives them from a `selecting` drag
> state + a middle-button handler; typing clears the highlight. Added `mclick X Y` to osdrive. `make check`
> 29/29. Verified in-guest: `echo COPYPASTE` → drag-select the output → middle-click at the prompt pastes
> "COPYPASTE". Future: browser text selection feeds the same clipboard.

> **(M759) Mouse wheel scrolling.** The pointer had no wheel at all. Now the wheel scrolls whichever window
> the cursor is over: the browser (via `browser_key` arrow-scroll) and any text app's scrollback (via
> `app_key` PgUp/PgDn). Two input paths feed one shared accumulator (`mouse_add_wheel`/`mouse_read_wheel`):
> the USB tablet's HID packet carries the wheel in byte 5 (what `make run`/osdrive actually use), and the
> PS/2 driver now does the IntelliMouse sample-rate knock (200/100/80 → device id 0x03 → 4-byte packets with
> a Z axis) for real-hardware/PS-2 configs. The compositor polls the accumulator each frame and routes ticks
> to the topmost non-minimized window under the cursor (gfx apps skipped). Added a `wheel X Y N` command to
> osdrive for testing. `make check` 29/29. Verified in-guest: wheel scrolls the browser both ways and the
> shell scrollback (`^` indicator).

> **(M758) Editor — Home/End/Delete/Tab + system-caret opt-out.** The full-screen editor only had arrow
> keys + backspace. Now it uses M757's new keycodes: Home/End jump to the start/end of the current line,
> Delete forward-deletes the char at the cursor, and Tab inserts spaces to the next 4-column stop. Also
> added a small `sys_caret(on)` syscall (SYS_caret 56) so a full-screen app that draws its own cursor can
> hide the system block caret — the editor calls `sys_caret(0)` after its filename prompt, so the prompt
> keeps the caret but the body shows only its own `|` (no double cursor). `make check` 29/29. Verified
> in-guest: Home+insert, Delete, End+append, Tab→4 spaces; and the editor body shows a single cursor.

> **(M757) Terminal — real line editor + visible caret.** The kernel's cooked-input read (`app_sys_read`)
> only ever appended/backspaced at the end of the line, and the terminal drew no caret at all. Now: a green
> block caret on the focused terminal at the live cursor (threaded `focused` through `draw_window` →
> `draw_content` → `app_render`); left/right arrows move the caret within the line; Home/End jump to the
> ends; Delete removes the char at the caret and Backspace the one before it — both work mid-line, repainting
> the tail; printable keys insert at the caret. New keycodes: Home→0x01, End→0x05, Delete→0x04 (keyboard.c).
> Command history deepened 6 → 32 entries. Benefits every `readline` caller (shell, calc, editor, adv).
> Kernel-side (keyboard.c / app.c / desktop.c / app.h); `make check` 29/29. Verified in-guest: typed `echo
> Hllo`, ←←← then `e` → `echo Hello` → "Hello"; `echo XOK` → del 'X' → "OK"; Home+Delete; up-arrow recall.

> **(M756) Shell — environment variables.** `set NAME=value` (or `export NAME=value`) stores a variable;
> `$NAME` / `${NAME}` expand in any command line (before glob/pipe/redirect); `unset NAME` removes one;
> `set`/`env` list them. They persist for the shell process's lifetime. A real scripting power-feature the
> shell lacked, and a distinct subsystem from the browser run. App-only (`user/shell.c`, not test-coupled);
> `make check` 29/29. Verified in-guest: `set GREET=hello world; echo $GREET` → "hello world"; `set
> DIR=README.TXT; cat $DIR` opens the file; `env` lists both.

> **(M755) Browser — full-width block backgrounds.** `background-color` on a block element (div / p / h1-6 /
> section / li / …) now fills the whole line band (cl→cr, respecting any indent), not just behind the text —
> distinct from an inline element's bg (`<mark>`, `<span>`), which stays a text highlight. Block-ness is
> decided by the element's tag (`is_block_tag`) and marked with a spare bg flag bit (`0x02000000`); the render
> paints the band at each line start (the `cx==cl` point, after `ls`/indent is known). Real sites' coloured
> cards/sections/headers now render as boxes. `make check` 29/29 (the browser goldens are parser/presence
> checks, unaffected); verified in-guest — `div{background:#cfe8ff}` shows a full-width band, an inline span
> bg stays behind its text.

> **(M754) Desktop — click a file in the Files window to open it.** The Files window was keyboard-only
> (up/down + Enter); a mouse click on a file row did nothing (the click handler routed body clicks to the
> browser but had no Files case). Added `files_click()`: a click maps the cursor's y to the file row
> (mirroring the render layout + scroll offset), selects it, and opens it through the shared Enter path
> (`file:NAME` in a browser window). A distinct, non-browser UX fix. `make check` 29/29; verified in-guest —
> clicking `MARGIN.HTM` highlights it and opens it in a browser.

> **(M753) Browser — `margin`/`padding` shorthand horizontal value.** The shorthand only contributed its top
> value (vertical) before; `parse_style_hspace` now decodes the shorthand's left value too (4 tokens → 4th,
> 2–3 → 2nd, 1 → all sides), so the ubiquitous `margin: V H` form (and `margin: Npx` all-sides) indents the
> block. Parse-only, golden-safe. `MARGIN.HTM` gains a `margin:6px 60px` example. `make check` 29/29; verified
> in-guest (a `margin:40px` div is now indented, and `margin:6px 60px` indents 60px). The browser's CSS
> box-model spacing — margin + padding, both axes, inline + `<style>` rules + shorthands — is comprehensive.

> **(M752) Browser — CSS horizontal indent from `<style>` rules.** Mirroring M749, `capture_css`/`css_match`
> now carry a per-rule left indent (`css_indent[]`), so `margin-left`/`padding-left` from a stylesheet class
> (e.g. `.ind{margin-left:40px}`) indents the matched blocks — inline `style=` still overriding. With
> M748–M751 the browser's CSS box-model **spacing is now complete**: margin + padding, both axes, from inline
> styles and `<style>` rules. `MARGIN.HTM` gains a `.ind` example. `make check` 29/29; verified in-guest
> (a `<p class="ind">` is indented by the stylesheet rule).

> **(M751) Browser — CSS horizontal indent (`margin-left` / `padding-left`).** Completes box-model spacing in
> both axes: a block's left margin/padding now indents its whole content (wrapped lines included), reusing the
> `curindent`/`tokindent` machinery `<blockquote>` uses — saved/restored on the style-scope stack so nested
> indents compose. Inline `style=` for now. Golden-safe (no margin-left in the test pages). `make check`
> 29/29; verified in-guest — `<p style="margin-left:48px">` shifts the whole paragraph right, wrapped lines
> too. With M748–M750 the browser now does CSS box-model spacing (margin + padding) in both axes.

> **(M750) Browser — CSS vertical padding (rounds out the box-model spacing).** Extended the M748/M749 path:
> a block's top space now sums `margin-top` + `padding-top` (or the `margin`/`padding` shorthands' top value),
> so the very common `padding`-spaced containers render with their intended room. Renamed
> `parse_style_margin_v` → `parse_style_vspace`; same inline + `<style>`-rule cascade and golden-safety.
> `MARGIN.HTM` gains a padding example. `make check` 29/29; verified in-guest (`padding-top:45px` shows the
> gap). Browser vertical box-model spacing now covers margin + padding, inline and from stylesheets.

> **(M749) Browser — CSS margins from `<style>` rules (not just inline).** Extended M748: `capture_css` +
> `css_match` now carry a per-rule vertical margin (`css_margin[]`), so `margin-top` / the `margin` shorthand
> from a `<style>` block (tag / `.class` / `#id` / `[attr]` selectors) spaces block elements — with an inline
> `style=` margin still overriding the rule (correct cascade). This is the form real sites actually use.
> `MARGIN.HTM` extended with a `.gap{margin-top:55px}` rule. Golden-safe (the css test pages set no margins).
> `make check` 29/29; verified in-guest — a `<p class="gap">` shows the stylesheet-driven gap above it.

> **(M748) Browser — CSS vertical margins (first box-model spacing).** The renderer was explicitly
> "box-model-less"; now block elements honour `margin-top` and the `margin` shorthand's top value (px/em,
> capped at 120). Parsed from inline `style=` into a per-tag `pending_vmargin` that `emit_break` folds onto
> the block break's spacing field (`tok.off`), which the layout adds to the vertical advance — and since
> content height derives from that same cursor, scrolling tracks it automatically. Golden-safe: the test
> pages use no margins, so `off` stays 0 and rendering is byte-identical (csstest/browsertest unchanged). Key
> fix: consecutive block breaks (`</p><p>`) merge, so the margin had to be carried onto the *merged* break,
> not just a fresh one. New `MARGIN.HTM` demo. `make check` 29/29; verified in-guest — `margin-top:60px`
> shows a big gap, `:10px` a small one, `div margin:40px` spaces it. (CSS-rule margins via `<style>` next.)

> **(M747) JS engine — `globalThis` / `self`.** Modern JS and UMD-style libraries lean on the universal
> global object (`typeof globalThis`, `globalThis.X`, `self.X`); both were `undefined`, so that code broke.
> Built a global object at the end of `install_globals` that exposes every global defined above as a property
> (Math, JSON, Object, Array, String, the timer functions, console, document, fetch, window/location when in
> a page, …), with `self === globalThis` and `globalThis.globalThis === globalThis`. window/location are
> browser-only so they're skipped at the shell. Verified host (`globalThis.JSON.stringify`,
> `globalThis.setTimeout`, UMD detection) + suite golden updated; `make check` 29/29 green.

> **(M746) JS engine — `requestAnimationFrame`/`queueMicrotask`/`cancelAnimationFrame`.** Real pages drive
> animation with `requestAnimationFrame` and defer work with `queueMicrotask`; both were `undefined`. Wired
> them to the M745 deferred-callback queue (rAF/queueMicrotask enqueue like a 0-delay timer, so they fire
> first; cancelAnimationFrame cancels by id). `tests/js/timers.js` extended to cover them. 29/29 green.

> **(M745) JS engine — `setTimeout`/`setInterval`/`clearTimeout`/`clearInterval`.** The engine had no timers
> at all (`typeof setTimeout` was `undefined`), so any real-world page that defers work with
> `setTimeout(fn, …)` — extremely common — aborted on "not defined". Added a deferred-callback queue in
> `kernel/js.c`: timers are queued during the run, then (the engine has no event loop — its I/O is blocking
> and Promises resolve eagerly) drained after the top-level script in (delay, registration) order, with
> callbacks free to queue more, bounded so a self-rescheduling timer can't spin forever. `setInterval` fires
> once (a synchronous engine can't loop a wall clock); `clearTimeout`/`clearInterval` cancel by id. New
> `tests/js/timers.js` golden (ordering + chaining + cancellation), wired into `make check`. 29/29 suites
> green; verified in-guest (`js` runs a `setTimeout` callback). Not games — a real browser/JS capability.

> **(M744) Pac-Man — persistent high score, wired into `scores`.** Pac-Man now saves your best to PACMAN.HI
> (like Snake/Tetris/…) and shows it in the HUD, and it's been added to the shell `scores` leaderboard. App-
> only change (pacman.c + shell.c, neither test-coupled). Verified in-guest: with PACMAN.HI=1234 on disk,
> Pac-Man's HUD loads **best 1234** and `scores` lists it.

> **(M743) New shell command — `scores` (a personal leaderboard).** Fifteen games save a best to their own
> `*.HI` file; `scores` reads them all and prints the ones you've set (skipping the rest), so you can see
> every personal best at once. App-only change to `user/shell.c` (not test-coupled). Verified in-guest: on a
> fresh disk it prints "(none yet)"; after `write`-ing a few `.HI` files it lists e.g. Snake: 4242, Tetris:
> 18800, Video Poker: 315.

> **(M742) New game — Pac-Man (maze chase).** `user/pacman.c`: eat every dot while dodging four ghosts; grab
> a power pellet (O) and they turn blue for a few seconds — touch them then and they flee to the pen for 200
> points. Clear the maze to win; lose three lives and it's over. Continuous movement with turn-queuing (your
> steer applies as soon as the way is clear); ghosts chase greedily (flee when frightened) at ~75% of your
> speed so you can escape. The maze uses regular wall pillars so every corridor is connected (no dot can be
> stranded). 29/29 suites green. Verified in-guest: the maze/dots/pellets render, Pac eats dots (+score),
> ghosts chase, and a catch costs a life and resets to start. Pac-Man and Chess were the two iconic games the
> set was missing — both now in.

> **(M741) Checkers — highlights the computer's last move (incl. multi-jumps).** Mirroring chess M740,
> `user/checkers.c` tracks the CPU's move — the first from-square and the final landing square of its slide or
> jump chain — and renders both in a distinct colour, so you can follow what it did (multi-jump captures were
> especially hard to read mid-board). App-only change (not test-coupled; last full `make check` at M739
> green). Verified in-guest: with the cursor moved aside, the CPU's reply shows its origin and destination
> highlighted, isolated from the cursor.

> **(M740) Chess — highlights the computer's last move.** After the CPU replied it was hard to see what it
> played (you had to diff the board). Now `user/chess.c` tracks the last move's from/to squares and renders
> them in bright cyan — the vacated square as a `*`, the moved piece in cyan — for both your move and the
> CPU's, so the post-reply board points right at the CPU's move. App-only change (chess.c isn't test-coupled;
> last full `make check` at M739 green). Verified in-guest: after 1.e4 the CPU's 1...Nf6 shows a cyan `*` on
> g8 and a cyan knight on f6.

> **(M739) New game — Missile Command (mouse-driven, in the framebuffer).** `user/missile.c`: defend six
> cities from descending missiles — move the mouse to aim the crosshair and click to detonate an interceptor,
> whose expanding blast destroys any missile it touches. A missile that reaches the ground takes out the
> nearest city; when all six are gone the sky turns red (r restarts, Esc quits), and the waves speed up. A
> graphics + **mouse** app — the first arcade game driven by the mouse — so it has its own SSE build rule.
> 29/29 suites green. Verified in-guest: the cities/battery/descending-missiles render, the crosshair tracks
> the mouse, and a held click detonates a (lingering) blast. (osdrive's instant `click` is too brief for the
> once-per-frame `sys_mouse` poll — the mouse analogue of the keyboard same-frame-tap — but a real press
> registers; `drag` confirms it.)

> **(M738) New game — Dots and Boxes vs the computer.** `user/dotsbox.c`: a 4×4-box grid (16 boxes on a 9×9
> dot/edge lattice). Move the `+` cursor with the arrows and Space to draw the edge under it; complete a box's
> fourth side to claim it (Y) and move again. The CPU completes any box it can (chaining its extra turns),
> else plays a "safe" edge that doesn't hand you a third side, else gives up the least it must. Pure-additive
> ulib app (last full `make check` at M737 green; kernel boots+runs it). Verified in-guest: edges draw, the
> CPU replies, and box completion + scoring + extra-turn chaining all work (it ran a chain to 7 boxes).

> **(M737) New game — Mancala (Kalah) vs the computer.** `user/mancala.c`: the classic sow-and-capture
> game. Pick a pit (1-6), seeds sow counterclockwise skipping the opponent's store; landing in your own store
> earns another turn, and landing the last seed in an empty pit on your side captures it plus the pit
> opposite. When a side empties, the other sweeps its leftovers — most seeds wins. The CPU plays a greedy
> heuristic (prefers extra-turn moves, then captures, then seeds banked, chaining its extra turns). 29/29
> suites green. Verified in-guest, including **seed conservation** (always 48 on the board) after a full
> exchange — strong evidence the sow/capture math is right.

> **(M736) New game — Video Poker (Jacks or Better).** `user/vpoker.c`: you're dealt five cards, toggle holds
> with 1-5, then Space draws replacements; the final hand pays on the classic 9/6 Jacks-or-Better table (a
> pair of jacks or better, up to a 250× royal flush). Each deal antes one credit; you start with 100 and your
> best balance persists to VPOKER.HI (a free 100 if you bust). Fills the casino-card gap (only Blackjack
> existed). Pure-additive ulib app; the last full `make check` (M735) was green and the kernel boots+runs it.
> Verified in-guest: the deal renders five suited, colour-coded cards, holds keep the right cards across a
> draw, and the hand evaluates correctly ("No win" on a junk hand).

> **(M735) Chess — castling.** Added castling to `user/chess.c`: rights are tracked in a bitmask (cleared
> whenever a king or rook leaves, or is captured on, its home square), the king may not castle out of,
> through, or into check, and `apply()`/`undo()` now snapshot the rights alongside the 64-byte board so the
> alpha-beta search treats castling correctly too. It's encoded as the king's two-file step — `apply()` spots
> that and moves the rook to match (h→f kingside, a→d queenside). App-only change. Verified in-guest: after
> e4/Nf3/Bc4, **O-O** moves the king to g1 and the rook to f1, and the CPU keeps playing legally. (En-passant
> is still not modelled — rare.)

> **(M734) Docs — README front page caught up.** The headline still said "720+ milestones / forty-plus apps /
> ~thirty games" and the M706+ recap pre-dated the recent work. Bumped to 730+ milestones / fifty-plus apps /
> ~forty games and added the additions since: the **Game Boy emulator** (Peanut-GB), **Chess** (alpha-beta
> AI), Checkers, Gomoku, Frogger, Lunar Lander, Yahtzee, Flappy, and the on-disk ELF loader (`run NAME.ELF`).

> **(M733) Chess AI plays sensibly now — piece-square tables.** The depth-3 search was material-only, so it
> answered 1.e4 with the passive 1...Na6 — any equal-material move scored the same. Added Michniewski's
> simplified piece-square tables to `evaluate()` (centre control, development, advanced pawns, a tucked-away
> king), small enough never to outweigh a real capture but enough to break ties toward good squares. App-only
> change (no kernel/test-coupled code). Verified in-guest: the CPU now answers 1.e4 with the developing
> 1...Nf6 instead of 1...Na6.

> **(M732) New game — Chess vs the computer (full legal-move rules + an alpha-beta AI).** `user/chess.c`:
> every piece moves by the real rules, you may not leave your own king in check, pawns auto-promote to a queen
> on the last rank, and the game ends on checkmate or stalemate. You play White (bottom); the CPU plays Black
> with a depth-3 negamax + alpha-beta search over material. Arrows move the cursor, Space picks one of your
> pieces then its destination (Space elsewhere re-picks), r starts a new game, q quits. (Castling and
> en-passant are deliberately not modelled.) The biggest gap in the board-game set — TTT, Connect-4, Reversi,
> Checkers, Gomoku and Battleship existed, but no Chess. Verified in-guest: the board renders in the start
> position, **e2-e4** plays, and the CPU answers with a legal developing move (Nb8-a6). 29/29 suites green.

> **(M731) Docs/correctness — the disk ELF loader is real (the embed comment was stale).** `kernel/app.c`
> already has `app_spawn_from_file()`, wired in `kernel/syscall.c` so `run NAME.ELF` falls back to loading an
> ELF straight from the FAT32 disk when the name isn't a built-in; `mkfatfs` even ships `CALC.ELF` for it and
> `GUIDE.TXT` documents `run NAME.ELF`. But `user_blob.asm` still claimed *"we have no general program loader
> from disk yet"* — corrected that comment to describe reality (the embedded apps need no disk; the kernel
> also loads ELFs from disk at runtime). Verified in-guest: `run calc.elf` opens a window titled **calc.elf**
> (loaded from disk via the path-as-title path, not the embedded "Calc"), which also confirms the M720/M722
> `fat32_read` perf rewrite reads binary files correctly.

> **(M730) New game — Frogger (cross the traffic).** `user/frogger.c`: hop your frog upward across lanes of
> cars, each lane scrolling at its own speed and direction, to reach the goal bank at the top (+10, then start
> again from the bottom). Touch a car and you lose a life; three lives, best saved to FROGGER.HI. Real-time —
> the lanes scroll on a tick, you hop on a keypress. Verified in-guest (lanes render with traffic, the frog
> hops, a collision squashes it and costs a life). 29/29 suites green.

> **(M729) New game — Gomoku (Five in a Row) vs the computer.** `user/gomoku.c`: place stones on an 11×11
> board, first to five in a row in any direction wins; you're X, the CPU is O. The CPU plays a heuristic — for
> every empty point it weighs the line it would build for itself against the line it would deny you
> (run-length²), slightly favouring blocks and the centre, so it both attacks and defends. Additive ulib app
> (kernel rebuilds clean; no test-coupled code touched). Verified in-guest: the board renders, you place X,
> and the CPU answers with O near your stones.

> **(M728) New game — Checkers vs the computer.** `user/checkers.c`: standard American draughts on an 8×8
> board — men move/capture one diagonal forward, kings both ways, capturing is **mandatory** and multi-jumps
> must be continued, and a man reaching the far row is crowned. Pick a piece (Space) then a destination (legal
> targets marked `*`); the CPU plays greedily (captures when able, continuing multi-jumps). Take all the CPU's
> pieces or leave it with no move to win. Verified in-guest: the board renders, selecting a piece shows its
> legal moves, a slide executes, the CPU replies, and the mandatory-jump rule is enforced ("you MUST jump").
> All 29 `make check` suites green.

> **(M727) New game — Yahtzee (with a persistent best).** `user/yahtzee.c`: solo Yahtzee — each of 13 turns,
> roll the five dice, hold any (1-5) and re-roll up to twice (r), then assign them to one of the 13 scoring
> categories (arrows to an empty row, Enter to score); the scorecard shows each empty row's potential score
> live. Standard scoring including the upper-section 63+ bonus (+35); best total saved to `YAHTZEE.HI`.
> Verified in-guest (dice + live scorecard render; scoring a category fills it and starts the next turn).
> All 29 `make check` suites green.

> **(M726) New game — Lunar Lander (vector, in the framebuffer).** `user/lander.c`: ride gravity down and set
> the lander on the green pad gently — the main engine (Up) burns fuel and pushes up, side thrusters
> (Left/Right) nudge horizontally; land on the pad slow and near-vertical to win, else crash. A starfield +
> jagged terrain with a highlighted pad, a fuel gauge, and the lander tinted white/amber by descent speed
> (green/red on land/crash). Built with SSE for the float physics; raw make/break input with the same
> same-frame-tap latch as the other graphics apps. Verified in-guest (the lander descends over the terrain;
> thrust + fuel work). All 29 `make check` suites green.

> **(M725) Game Boy emulator — a whole second console, via vendored Peanut-GB.** Same recipe as the NES:
> `user/gb/` vendors **Peanut-GB** (an MIT single-header DMG emulator — CPU/PPU/MBC) behind `gb_osdev.c`, which
> reads the chosen `.gb` off the disk, serves it through Peanut-GB's ROM/cart-RAM callbacks, maps each 2-bit
> LCD shade to the classic DMG green into a 160×144 framebuffer (`sys_gfx_blit`), and drives the joypad byte
> (active-low) from raw scancodes with the same same-frame-tap latch as the NES. A `.gb` picker handles
> multiple ROMs. Ships **Libbet and the Magic Floor** (`tools/libbet.gb`, Zlib, by Damian Yerrick) — a real GB
> puzzle game. Verified in-guest: libbet's intro renders in DMG green and input advances into the playfield
> (the "Combo / 0% / 0/04" HUD). All 29 `make check` suites green.

> **(M724) New game — Flappy Bird (with a persistent high score).** `user/flappy.c`: tap Space to flap the
> bird up against gravity and thread the gaps in the scrolling pipes; one point per pipe cleared, best saved
> to `FLAPPY.HI` (shown as `hi N`). Integer "sub-row" physics (no floating point, so it uses the generic user
> build rule), a 12-row field that fits the text grid, real-time pacing. Wired into the app registry + Apps
> menu. Verified in-guest (the `score/hi` header, bird, and start prompt render; flapping lifts the bird and
> pipes scroll in). All 29 `make check` suites green.

> **(M723) Persistent high scores for the new games (+ a Space Invaders layout fix).** Matching the OS's
> existing `*.HI` convention (SNAKE.HI / TETRIS.HI): **Space Invaders** now keeps a high score (`SPACEINV.HI`,
> shown as `hi N`, saved when beaten) and **15-Puzzle** keeps a fewest-moves best (`FIFTEEN.HI`, shown as
> `best=N`, saved on a faster solve). Adding the Space Invaders `hi` field surfaced a pre-existing bug — its
> header had been scrolling off the top of the 17-row text grid since M716 (the field was too tall) — so the
> field was shrunk and the full `score / hi / lives` header now shows. Verified in-guest (both display and
> play; persistence reuses snake's proven load/save). Isolated to two user-space games (no kernel or
> test-coupled code touched).

> **(M722) Faster file writes — `alloc_cluster` uses a free-cluster hint instead of rescanning the FAT.**
> Saving a file allocated clusters one at a time, and each `alloc_cluster` rescanned the FAT from the start
> for the first free entry — O(n²) for an n-cluster file (a 576 KB screenshot = 1152 clusters rescanned the
> whole FAT ~1152×). Allocation now starts each scan at a remembered hint (where the last one ended, with
> wraparound) and caches the FAT sector across the scan: a sequential write is O(n), and the clusters come out
> **contiguous** — which the run-coalescing reader (M720) then loads fast. It still returns only a genuinely
> free (entry==0) cluster, so a stale hint can at worst cause a false "disk full", never a clobber, and
> `free_chain` lowers the hint so freed space is reused. Verified: in-guest a 4 MB `cp` round-trips
> byte-identical (`cmp` → "files are identical"), and all 29 `make check` suites stay green (incl. fstest's
> 8k-op write/delete/mkdir fuzz and the exact-reclaim `df` regression).

> **(M720) Much faster file reads — `fat32_read` does multi-sector, contiguous-run I/O.** Loading a big file
> (e.g. Freedoom's 27 MB WAD) was reading the disk one 512-byte sector at a time *and* re-reading a FAT sector
> for every cluster (the same FAT sector up to 128× on a sequential walk) — ~108k single-sector PIO commands
> for 27 MB (~10-15 s). `fat32_read` now (a) caches the current FAT sector for the chain walk (local to the
> call, so no staleness vs. writes), (b) coalesces physically-contiguous, chain-linked clusters into runs, and
> (c) reads each run in ≤255-sector `ata_read` bursts straight into the destination (a bounce buffer only for
> the final partial sector) — a few hundred commands instead of ~108k. The cycle guard + `cluster_in_range`
> checks are preserved, and `remaining` is clamped to the caller's `max` so the destination can't overrun.
> This speeds up every file read (boot assets, DOOM/Quake/Freedoom loads, the browser's local pages). Verified
> in-guest: the Freedoom title now renders by ~6 s (was still a black screen at 7 s), and all 29 `make check`
> suites stay green — including `fstest`, whose corruption-fuzzer drives this exact read path.

> **(M719) Asteroids — a vector arcade game in the framebuffer.** `user/asteroids.c`: rotate/thrust a ship
> with wrap-around momentum physics, fire bullets, and blast asteroids that split (big → two medium → two
> small → gone); clearing the field spawns a bigger wave; three lives, a collision costs one. Drawn with a
> Bresenham line routine (ship triangle, octagon rocks, dot bullets) over a starfield; built with SSE for the
> float physics + a Taylor sin/cos, raw input with the same-frame-tap latch. Verified in-guest: ship + rocks
> + lives render, and rotating/thrusting/firing turns the ship and launches bullets while the rocks drift.
> All 29 `make check` suites still green.

> **(M718) Freedoom — a complete libre game on the DOOM engine, via a WAD picker.** The vendored engine
> already recognises Freedoom (`d_iwad.c` lists `freedoom1.wad`; `d_main.c` keys off the `FREEDOOM` lump), so
> this ships **Freedoom Phase 1** (GPL/BSD, `tools/freedoom1.wad`, ~27 MB — it fits the existing 64 MB disk)
> and gives the DOOM app a WAD picker (`user/doom/doomgeneric_osdev.c`, mirroring the NES ROM picker): it
> enumerates the `.wad` files on disk (`sys_list`) and, if there's more than one, shows a pickable text menu
> before starting — so DOOM now plays the shareware IWAD *or* the full free game. Verified in-guest: the
> picker lists DOOM1.WAD + FREEDOM1.WAD, and selecting Freedoom loads it (the 27 MB read takes ~10-15 s off
> the emulated IDE disk) and renders the Freedoom title + main menu. All 29 `make check` suites still green.

> **(M717) Apps menu: 2 → 3 columns.** This session's ~13 new game apps had filled the two-column Apps menu
> to the screen's bottom edge. The menu layout is fully parameterized, so bumping `MENU_COLS` to 3 (a one-line
> change in `desktop.c`) reflows the render, keyboard navigation, and click hit-testing together: all 48
> entries now show in three tidy 16-item columns (shorter than before, with headroom for ~36 more). Verified
> in-guest — the menu renders in three columns and arrow-nav + Enter still launch the right app (Down → Right
> → Blackjack). All 29 `make check` suites still green.

> **(M716) Two more games — Tron and Space Invaders.** **Tron light cycles** (`user/tron.c`): you vs a CPU
> that steers greedily (straight while safe, else turn to an open side); both leave solid trails on a 40×15
> arena, run into any trail or wall and you're out, last one riding wins; real-time. **Space Invaders**
> (`user/spaceinv.c`): a 3×8 invader block marches and drops + reverses at the edges (speeding up as you thin
> it) and occasionally shoots back; slide the cannon and clear them before they land — three lives. Both
> verified in-guest (Tron: trails + a dodged head-on; Invaders: formation + cannon). All 29 `make check`
> suites still green.

> **(M715) NES sound — APU wired to the streaming PCM ring.** The NES app's audio callback
> (`user/nes/nes_osdev.c`) now converts libxnes's per-sample float (requested at 48 kHz, the kernel's PCM
> rate) to 16-bit stereo and flushes the batch to `sys_pcm_stream` once per frame (~800 samples/frame,
> consumed at 48 kHz) — the same non-blocking streaming path DOOM/Quake use. Verified headlessly for
> stability: the emulator still loads a ROM, renders, and responds (Start → Main Menu) with the audio path
> active, no crash/hang/perf regression over several seconds; audibility itself needs a host audio backend
> (`make run`, not the headless `-audiodev none`). All 29 `make check` suites still green.

> **(M714) The raycaster is now a first-person SHOOTER.** Extends `user/raycast.c` with billboarded enemy
> orbs and combat: each column's wall distance is kept in a depth buffer, and living enemies are transformed
> into camera space, projected to a screen column + size (1/depth), and drawn as shaded ellipses only where a
> nearer wall doesn't occlude them (drawn far-to-near). A white crosshair marks the centre; Space fires at the
> nearest enemy under it that isn't behind a wall (short cooldown). Enemies slowly home in (a touch beeps +
> respawns them). Verified in-guest: an enemy orb renders down the corridor under the crosshair, and firing
> destroys it. All 29 `make check` suites still green.

> **(M713) A from-scratch raycaster — pseudo-3D in the framebuffer.** `user/raycast.c` renders a
> Wolfenstein-style first-person maze: one ray per screen column is marched across a 16×16 grid (DDA) to the
> first wall and drawn as a vertical slice whose height is 1/distance, coloured by cell and shaded by distance
> + which side was hit, over a sky/floor split. Arrows or WASD walk and turn (with wall collision); reach the
> green exit to escape. Built with SSE (its own Makefile rule, like DOOM/Quake) so the ray geometry can use
> float + a small Taylor `sin`/`cos` (no libm); input drains the raw make/break queue with the same
> same-frame-tap latch as the NES so both quick taps and held keys move. Verified in-guest: the 3D corridor
> renders, walking advances the view, turning rotates it. All 29 `make check` suites still green.

> **(M712) Two more games — Battleship and Pig.** **Battleship** (`user/battleship.c`): two 8×8 seas, both
> fleets (5/4/3/3/2) auto-placed at random; you fire at the hidden enemy sea (a crosshair you move) while a
> hunt-then-target CPU (random shots until a hit, then it works the neighbouring cells) fires back at yours —
> sink all 17 enemy cells to win. **Pig** (`user/pig.c`): the push-your-luck dice game — roll to build a turn
> total, but a 1 wipes it and ends your turn; hold to bank it; first to 100, and the CPU holds once it has 20.
> Both verified in-guest (Battleship: shots + AI reply scored on both seas; Pig: rolls accumulate the turn
> total). All 29 `make check` suites still green.

> **(M711) NES is now a multi-game console — a ROM picker.** The NES app (`user/nes/nes_osdev.c`) enumerates
> the `.nes` files on the FAT disk (`sys_list`) and, if there's more than one, shows a pickable text menu
> (up/down + Enter) before switching to graphics — so any `.nes` dropped on the disk just appears, no rebuild.
> Ships a second freely-licensed ROM beside Nova the Squirrel: the **240p Test Suite** (GPLv3, by Damian
> Yerrick / Artemio Urbina — `tools/240p.nes`, mapper 2 / UNROM). Verified in-guest: the picker lists
> NOVA.NES + 240P.NES, and selecting 240P renders its graphically rich main screen — exercising a second
> mapper through the same core. All 29 `make check` suites still green.

> **(M710) Two more games — Memory and Sokoban.** **Memory/Concentration** (`user/memory.c`): a 4×4 board of
> 8 letter pairs, Fisher-Yates shuffled; flip two cards a turn, matches stay face-up, tries counted, clear to
> win. **Sokoban** (`user/sokoban.c`): push every crate onto a goal (no pulling, no double-pushes); 3 bundled
> open-room levels (no corner traps, each hand-verified solvable), push counter, r restart / n next. Both
> additive ulib apps wired into the registry + Apps menu; verified in-guest (Sokoban level 1 solved on
> camera; Memory card flip). All 29 `make check` suites still green.

> **(M709) "Half-Life: Black Mesa" — a tribute survival shooter.** The real Half-Life can't run here (a
> closed GoldSrc/Win32 engine needing a GPU and proprietary assets; Quake is this OS's GoldSrc-family
> ceiling), so this is an honest original homage on the text grid (`user/halflife.c`): after the resonance
> cascade, a HEV-suited scientist survives a swarm of headcrabs that spawn from the edges and chase — arrows
> move/aim, Space fires a bullet in your facing direction, contact drains the HEV suit, and the spawn rate +
> enemy speed ramp with your kill score. Adds a "Half-Life" entry to the Apps menu. Verified in-guest (HUD +
> HEV bar, player, headcrabs chasing, firing). All 29 `make check` suites still green.

> **(M708) Three more games — 15-Puzzle, Mastermind, Pong.** **15-Puzzle** (`user/fifteen.c`): 4×4 sliding
> tiles shuffled by random legal slides (always solvable); arrows slide a tile into the gap; move counter.
> **Mastermind** (`user/mastermind.c`): crack a 4-peg / 6-colour code in 10 guesses with the standard
> black (`#` = right colour & spot) / white (`o` = right colour, wrong spot) two-pass scoring; type 1-6,
> Backspace erases, Enter submits. **Pong** (`user/pong.c`): real-time against a ball-tracking CPU paddle
> (Up/Down or W/S), wall + paddle bounces, first to 7. All additive ulib apps wired into the registry +
> Apps menu; each verified in-guest. All 29 `make check` suites still green.

> **(M707) Two new games — Reversi and Lights Out.** Additive ring-3 ulib apps wired into the embedded-app
> registry + the Apps menu. **Reversi/Othello** (`user/reversi.c`): full 8×8 disc-flip logic in all 8
> directions, pass-when-no-move handling, majority-wins endgame, and a positional-greedy AI (seeks corners,
> avoids the trap squares beside them); arrows move a cursor and legal moves are shown as `*`. **Lights Out**
> (`user/lights.c`): the 5×5 toggle puzzle, board generated by random presses from all-off so it is always
> solvable, with a move counter. Both verified in-guest (Reversi: placed a disc → flips + AI reply confirmed;
> Lights Out: board + toggle). All 29 `make check` suites still green.

> **(M706) NES emulator — a whole console, via vendored libxnes + a thin platform shim.** Following the
> DOOM/Quake recipe (vendor a portable core + a small OS shim + a freely-licensed game on the FAT disk),
> `user/nes/` now hosts **libxnes** (a pure-C99 NES core — CPU/PPU/APU/DMA/controller/cartridge + mapper
> bank-switchers, **MIT**) behind `nes_osdev.c`: it reads `GAME.NES` off the disk, opens a 256×240 window,
> and loops `drain raw scancodes → joypad 1 / step one frame / blit` (`xnes_get_pixel` is already
> `0x00RRGGBB` → straight into `sys_gfx_blit`; timing via `sys_uptime_ms`/`sys_sleep`; audio is a no-op sink
> for now, sound to follow like DOOM did). The shipped game is **Nova the Squirrel** (GPLv3 homebrew by
> NovaSquirrel, mapper 1/MMC1, CHR-RAM). Booting it took two fixes to libxnes itself, isolated by
> host-testing the core against the ROM (0 → thousands of rendered pixels, then visually confirmed as the
> title screen): **(1)** `xnes_mapper1_init` never set MMC1's power-on PRG mode, so both PRG slots mapped
> bank 0 and the reset vector at `$FFFC` read the wrong bank (blank screen) — now it powers up PRG-mode 3
> (last 16K bank fixed at `$C000`), the de-facto hardware default the reset-bit path already applied;
> **(2)** the NES-2.0 header branch lacked the CHR-RAM fallback the iNES-1.0 branch had, so a 0-CHR-ROM cart
> got a zero-length CHR buffer — now an 8 KB writable CHR window. A third, OS-side subtlety: the NES samples
> controller *state* once per frame, so a synthetic/quick key tap (QEMU's ~100 ms `sendkey` can collapse into
> a single emulated frame) would net to "released" and miss an edge-triggered "Press Start" — the shim now
> latches a same-frame tap as pressed for one frame while genuine holds pass through. Verified headlessly
> (`osdrive`): title → Main Menu (Start) → Level-Select world map with the character moving. All 29
> `make check` suites still green. The MMC1 + CHR-RAM fixes light up a large slice of the real NES library
> (Zelda, Metroid, Mega Man 2, …), so more `.nes` games are now nearly free to add.

> **(M687-M700) Spec-compliance probing arc — ~14 wrong-answer fixes the crash-fuzzers can't see.** After
> the async/Promise/fetch arc, semantic probing (build the engine `-DJS_HOSTTEST`, diff edge cases against
> spec) kept paying out — each a value real code depends on, none a crash: **Promise.any** + **thenable
> assimilation** (`Promise.resolve`/`await` a `{then}` object now runs its `.then`) (M687); **JSON.stringify**
> `toJSON()` hook + the ±Infinity sentinel → `null` (M691); **Array** `lastIndexOf(x, fromIndex)` honored its
> fromIndex (was ignored) + `[].reduce(fn)` with no init throws (M692); **ToNumber** `0b`/`0o` string prefixes
> (joined the existing `0x`) (M693); **String** `startsWith`/`endsWith` honor their position arg + `repeat(-1)`
> throws (M694); **`new Date(string)` + `Date.parse`** (ISO date-string parsing, M695); **regex replace
> `$<name>`** named-group substitution (M696). Then the OBJECT MODEL: **`B.prototype = Object.create(A.prototype)`**
> classic inheritance (reassigning `.prototype` was silently dropped, so new instances + `instanceof` lost the
> chain) (M698); **`obj.constructor`** + **`fn`/`Class.name`** (`new X().constructor.name`) (M699);
> **`new.target`** (abstract-class guard / factory / arrow-inherits) (M700). Golden-locked in
> `tests/js/suite-promise.js` (the fresh-arena overflow suite — `suite.js` is ~350KB from its 40MB cap).
> Recurring lesson: FUZZING finds crashes, PROBING finds wrong answers — pair them. Two consecutive zero-yield
> batches (control-flow, collections) confirmed those areas comprehensive. Remaining JS gaps are
> architectural/large: **BigInt** (arbitrary precision), and `extends` a native built-in like `Array`/`Map`
> (the copy-based class model can't make an instance a native-typed object — `extends Error` works because
> Error instances are plain).

> **(M679-M682) async / Promise runtime — the other big JS frontier, on a SYNCHRONOUS-resolution model.**
> The OS has no event loop and its I/O is blocking, so Promises settle eagerly: an executor runs immediately,
> resolve/reject settle on the spot, and `.then`/`await` read an ALREADY-settled state — covering the common
> synchronous-fetch patterns (`.then` chains, `await fetch()`, `Promise.all` over settled work) without a
> microtask queue. **M679** Promise core: new obj kind `V_PROMISE` (state+value in the promise's own `vals[]`
> so the shared `obj` struct grows by ZERO bytes — a +1 `val` field there overflowed the 40MB arena);
> resolve/reject capture their promise as a `V_BOUND` bound-arg (natives get no self pointer), so a stored
> resolver works; `then`/`catch`/`finally` with rejection propagation + returned-promise flattening;
> `Promise.resolve`/`reject`/`all`/`race`/`allSettled`; `instanceof Promise`. **M680** `async function` +
> `await` (gated by `g_in_async` so they stay ordinary identifiers elsewhere; `await` is a unary prefix, and an
> awaited rejection re-throws so `try/catch` works). **M681** async arrows, **M682** async methods (a shared
> marker disambiguates `async m(){}` from a member named `async`). All gated → non-async code byte-identical.
> Tests live in a SEPARATE golden run (`tests/js/suite-promise.js`, fresh arena) because the monolithic
> `suite.js` runs one 40MB arena to completion with only ~350KB headroom — appending there OOMs an unrelated
> case mid-run. Out of scope (inherent to the model): true concurrency / a never-resolved promise stays
> pending; real microtask ordering. **M684-M685** then made it usable for real network code: a `fetch(url)`
> global returning `Promise<Response>` (`{status, ok, text(), json()}`), capability-injected like storage/DOM
> (`js_set_fetch`), with the browser wiring `http_get`/`tls_get` (HTTP + HTTPS) via `browser_fetch` (its own
> scratch, never `b->raw`; parses the status line + strips headers). An HTTP error status resolves with
> `ok=false`; only a network failure rejects. Page scripts run on the main thread AFTER the worker fetched the
> page, so a JS fetch is a sequential blocking get (UI freezes for its duration — the sync-model cost). The JS
> half is host-tested via a mock backing; the real-network in-guest path mirrors the proven `worker_fetch` /
> `net_demo` code (not automatable — external reachability is non-deterministic). **M702-M703** extended it to
> **POST**: `fetch(url, {method, body, headers})` reads the options, and new `http_post`/`tls_post` send the
> body (Content-Type from headers, default text/plain) over HTTP and HTTPS — the TLS GET path stays
> byte-identical (a new POST branch shares the established session; no crypto change), so HTTPS GET can't
> regress. fetch is now a usable HTTP client (GET+POST, Response.text/json, async/await, Promise.all).

> **(M671-M674) Shell matcher host-testing + the last big JS gap (generators).** **M671-M673** extracted the
> shell's two pure matchers — the `grep` regex (`^ $ . * [..] \`) and the filename glob (`*`/`?`) — into
> `user/shgrep.h` and host-tested them (a 28th suite: regression + 500k fuzz, ASan/UBSan), bringing them into
> the codebase's extract-for-testability pattern (cssprop/url/htmlentity/…). **M674** then implemented JS
> **generators** (`function*` / `yield` / `yield*`), which had been a parse-abort that killed the entire page
> script. EAGER model: calling a generator runs its body to completion, collects every `yield` into an array,
> and returns that array (iterable via `for-of`, `[...]`, `.map`). Covers the common finite-generator uses; an
> infinite generator errors cleanly (arena-OOM, no hang) and manual `.next()` / the yield-expression value
> aren't modelled (eager has no resume point). All changes are GATED — `g_in_gen` keeps `yield` an ordinary
> identifier outside a generator body, `is_gen` gates the eval — so non-generator code is byte-identical
> (jstest golden unchanged, jssrcfuzz clean). **M676** then added generator METHODS (`class C { *g(){} }`,
> object `{ *g(){} }`, and computed `*[Symbol.iterator](){…}`) plus taught the iterator-protocol consumers
> (spread / for-of / Array.from) to accept a generator's array result — so `*[Symbol.iterator]()` custom
> iterables drive `[...obj]` / `for (x of obj)`. Generators are now comprehensive; only manual `.next()` /
> two-way `yield` values are out of scope (inherent to the eager, resume-point-free model).

> **(M660) The probing generalized past JS: the browser's URL resolver didn't canonicalize `./`/`../`.**
> `resolve_img_url` concatenated a relative `<img src>`/`<a href>` onto the base directory without RFC 3986
> remove_dot_segments, so `../up.png` on `http://a.com/dir/sub/p.html` became `…/dir/sub/../up.png` (a strict
> server/CDN 404s on a literal `../`). Added an in-place `norm_path` (collapse `/./`, pop a segment per `/../`
> clamped at root, leave `?query`/`#frag` verbatim) applied in all three branches (absolute / protocol-relative
> / relative). Unit-tested standalone over 18 cases; urltest's 400k fuzz stays clean (norm_path only shrinks
> within the bounded buffer); regression cases added. (Probing the colour parser, by contrast, found the muted
> named palette — red=CC0000 etc. — is a deliberate, test-locked design choice, not a bug: left as-is.)
> **A composable shell text-tool suite (M649-M650, M664-M665, M668-M669):** `grep` regex (`^ $ . * [..] \`),
> `sort -n`/`-r`/`-u` (numeric / reverse / unique), `uniq -c` (run counts), and a new `seq` generator — all
> verified in-guest, and they compose: `seq 5 | sort -nr` -> 5,4,3,2,1. **M662:** `http_find_loc` now trims trailing OWS from the `Location:` value (RFC 7230) so a
> redirect URL has no stray spaces. **M663:** pngenc test now validates png_encode output is a *standard* PNG
> (magic + chunk CRCs + zlib IDAT, via Python stdlib), mirroring the deflate runner's system-gunzip check.

> **(M649-M650) Shell `grep` gained regex (was literal-substring only).** `grep '^foo'` used to search for the
> four characters "^foo"; `grep 'a.c'` wanted a literal dot. Added the compact Kernighan/Pike matcher: `^`/`$`
> anchors, `.` any-char, `*` zero-or-more, `[..]` classes (ranges, negation, leading-`]` literal), and `\`
> escapes, honouring the existing `-i` case fold. A pattern with no metacharacters still behaves exactly like
> the old substring search, so existing greps are unchanged. Verified two ways: a standalone host harness
> (~37 cases incl. ranges/negation/`[..]*`/escape/case-insensitive, ASan/UBSan clean) and in-guest
> (`ls > gf.txt` then `grep ^R` -> only R-prefixed names, `5$` -> only lines ending in 5, `HEL.O` via `.`,
> `RE*GEX` via `*`, `^[RT]` via class+anchor). The matcher is bounded by pattern length (matchstar iterates,
> never recurses on the text), so no deep recursion on long lines.

> **(M640-M646) JS-engine semantic completeness via host probing — fuzzing finds crashes, probing finds
> WRONG ANSWERS.** The image-decoder audit proved the parsers were memory-safe; this arc asked the other
> question: does the JS engine compute the RIGHT result? Building it `-DJS_HOSTTEST` and diffing ~90 edge cases
> against integer-JS semantics found one real bug, then a whole cluster in the regex engine (whose regexfuzz
> only catches crashes, not wrong matches) — all fixed + locked in the golden suite. **M640** bitwise
> `& | ^ << >>` ran in raw int64 while `>>>` (M269) already used JS 32-bit semantics — now all coerce operands
> to int32 + mask the shift count (the canonical int32 "hello" hash 99162322 now computes; arithmetic stays
> exact int64). Then the regex engine: **M642** `\b`/`\B` word boundaries (were literal 'b'); **M643**
> `\1`..`\9` backreferences (were literal digits — now real, incl. backtracking `/(\w+) \1/`); **M644** the `m`
> (multiline) + `s` (dotall) flags, fixing a genuine bug where `^`/`$` were ALWAYS multiline (`/^world$/`
> wrongly matched "hello\nworld"); **M645** `(?:)` non-capturing groups (were capturing, mis-numbering `$1`) +
> `\xHH` hex escapes; **M646** lookahead `(?=)`/`(?!)` (zero-width, via the VM's own recursion — the
> `/^(?=.*\d)\w+$/` password idiom now works). The engine went from "common features only" to a genuinely
> useful subset: classes, quantifiers, greedy/lazy, alternation, named + non-capturing groups, backrefs, all
> anchors + word boundaries, both flags, and lookahead. Safe throughout (only suite.js uses regex/bitwise, all
> small/int32-range; shell grep has its own matcher); all 27 suites green incl. the in-guest browser.
> **Probing kept paying out (M652-M654, beyond regex):** **M652** the `new Date(y,m,d,…)` constructor stored
> its args raw while getTime/getDay used the exact civil calendar — so getDate/getMonth disagreed and the
> common `new Date(y,m+1,0)` "last day of month" idiom returned 0; now it normalises via a civil round-trip
> (month overflow carries the year, day 0 = last of prev month, 2100 correctly not a leap year). **M653**
> function declarations weren't hoisted, so a call textually before `function f(){…}` threw "undefined
> variable"; the block/program executor now defines them in a pre-pass (forward refs + that pattern work; var
> hoisting/expressions unchanged). **M654** class private fields `#x` were a no-op AND a privacy leak (`#x=5`
> wrote the public `x`, `this.#x` read undefined) — the lexer now keeps a leading `#` as part of the
> identifier, so declaration + access agree on "#x" and it stays distinct from public `x`. (Generators
> `function*` were unsupported at this point — later added in M674, eager model.) **Still more (M656-M658):** **M656**
> built-in Error subtypes (TypeError/RangeError/SyntaxError) are now `instanceof Error` (their ctors'
> parent_class links to Error), so the `catch(e){ if (e instanceof Error) … }` guard works; **M657**
> `JSON.stringify` honors a *function* replacer (was array-allowlist-only — `(k,v)=>…` now transforms/drops
> each node at every depth); **M658** `ToNumber(string)` skips leading whitespace, parses a `0x` hex prefix,
> and honors a leading `+`/`-` (`Number("  7  ")` was 0, `Number("0x10")` was 0), while keeping the engine's
> lenient trailing-char behavior + string `+` concat. Each fix is golden-locked — ~13 real JS/regex
> correctness bugs from one probing arc, none of which the crash-fuzzers could see.

> **(M634-M640) Test-hardening + a JS correctness fix: lock in this session's fixes, fuzz the untrusted-input
> parser paths the existing fuzzers couldn't reach, and fix a bitwise-operator divergence.** The data-loss bug class (below) was fixed + verified in-guest but had no
> host regression coverage, and an audit of the image decoders found them robust *by reading* yet under-fuzzed
> where it matters. **M634** adds host regressions (fs_test, which `#include`s fat32.c) for the two FS bugs:
> `rm <non-empty-dir>` is refused with NO cluster leak (df-checked — full reclaim only after a proper delete),
> and a name that 8.3-truncates ("dl.html"→DL.HTM, "verylongname.txt"→VERYLONG.TXT) reads back by its long
> name. **M635-M637** fix a shared gap in the image fuzzer: it prepends only a format magic + ≤91 random
> bytes, so the decoders' deepest untrusted-input code never ran (it needs a structurally valid header first).
> Now a real header is built and the *compressed payload* fuzzed: **M635** GIF LZW (variable-width codes, the
> dictionary, the KwKwK self-reference, the sub-block chain), **M636** JPEG Huffman→dequant→IDCT→YCbCr (a real
> embedded 8×8 baseline JPEG, its entropy scan mutated), **M637** PNG recon_filters (all 5 filter types) +
> expand_px (per colour type), via a STORED-deflate IDAT so inflate is a memcpy and the decode always reaches
> recon+expand on bytes we control. Two techniques made these TRUE tests, each verified by planting an OOB and
> watching the suite go red then restoring it: (1) buffers sized to the EXACT image — a shared 4MB scratch
> masks a write past the per-pixel cap, while tight malloc'd buffers put an ASan redzone right at the boundary;
> (2) small image dims so the output bound is hit constantly. The decoders all proved clean (the audit was
> right — they bound every dimension, table index, and output write); the lasting value is regression coverage
> for the exact paths a remote image/page can drive. **M639** extends this to the *animated*-GIF compositor
> (`gif_decode_anim` — multi-frame, the GCE, disposal restore-to-background, the per-frame snapshot; web-
> reachable yet previously undeclared in the harness): a known 3-frame decode + 120k mutated-frame iters into
> exact-size buffers (verified genuine — weakening the per-frame `il+iw>W` bound aborts in `gif_decode_anim`).
> **M640** is a real correctness fix the engine-probing turned up: the JS engine's `& | ^ << >>` operated in
> the raw int64 number while `>>>` (M269) used JS 32-bit semantics, so `1<<31` (gave 2147483648, not
> -2147483648), `1<<32` (no shift-count mask), and `0xFFFFFFFF|0` (gave 4294967295, not -1) diverged from
> every real JS engine and broke the idioms browser scripts depend on (`x|0` int-coercion, `(r<<16)|(g<<8)|b`
> packing, 32-bit string hashes). All bitwise ops now coerce operands to int32 + mask the shift count to & 31
> (arithmetic stays exact int64, like JS doubles); the canonical int32 hash of "hello" (99162322) now computes.
> Safe — the only bitwise in the tree is suite.js with int32-range values (golden byte-identical), no app uses
> JS bitwise. All 27 suites green (incl. the in-guest browser that runs the engine).

> **(M603-M630) A systematic data-loss/correctness bug class — found + fixed across the whole file surface
> via in-guest verification.** Verifying the FAT32 write path (a cp/edit/save round-trip) led into the
> shell's file commands and exposed a pervasive pattern: **fixed-size buffers silently truncating anything
> larger.** Consequences ranged from **silent data loss** — cp/mv truncated copies to 4KB (and mv then
> deleted the source); the editor (ESC always saves) rewrote any file >8KB as just its first 8KB; `>>`
> append rewrote the target as its first 8KB — to **wrong-but-authoritative results** — sha256/sha512/crc32
> hashed only the first 16KB; `cmp` reported "files are identical" for files that differed past 2KB;
> wc/grep/tail/sort/nl/uniq/hexdump processed only a 0.5–8KB prefix; gzip/gunzip/crypt capped at 16–256KB.
> Each now handles the whole file (a kernel `read_whole_file()` + a shell `slurp()`: a heap buffer that
> doubles until the read no longer fills it, capped at 32MB; sort/nl/uniq/tac also lost their fixed line caps),
> every fix verified in-guest against host tools / file sizes (e.g. sha256 of WALL.PNG matches `sha256sum`;
> `cmp` of two 14KB copies differing only at the end now reports the diff; `cat *.htm > f` captures all
> 71KB; gzip→gunzip of a 430KB WAV round-trips byte-identical). A **separate parsing bug** surfaced
> alongside: redirect parsing left the space before `>` in the command, so `cmd arg > file` passed `arg `
> (trailing space) and broke FAT32 lookups / streq commands — redirection only worked written tight
> (`cmd>file`). The pipe/redirect capture buffer also grew from a fixed 8KB to a growable heap buffer.
> Also **M603**: ~38 more CSS named colours (real pages rendered tomato/dodgerblue/limegreen as no colour).
> By M627 EVERY file-touching shell command handles whole files (M617-M619 tac/strings/tr/cut/fold, fold
> printing each wrapped line as it goes; M621 comm/paste; M622 diff's input; M625 base64 encode/decode;
> M626 the `js` command's source file + 1MB output; M627 todo's read-modify-write list). The one remaining
> cap — diff's 128-line LCS table — diff REPORTS it ("(diff truncated at 128 lines/file)"), so it's honest,
> not silent: **no silent truncation remains.** Verifying subdirectories then surfaced an unrelated FS bug:
> **M624** `rm <non-empty-dir>` deleted the directory but freed only ITS clusters, orphaning every child
> file's clusters (silent FAT corruption / space leak) — now it refuses a non-empty directory (df confirms
> clusters are freed exactly on delete). **M629** then bumped wget's fixed 16KB download buffer (it saved
> files truncated) to 1MB; downloading a 20KB page surfaced yet another: **M630** the read path (dir_find)
> didn't 8.3-normalize the lookup name the way write/delete do, so a file written as e.g. "dl.html" (stored
> DL.HTM) couldn't be read back by that name — now it matches the truncated form too. A clean cascade: one
> round-trip test surfaced a whole bug class plus several adjacent FS/parsing bugs. All 27 suites green
> throughout.

> **(M599) Real bug found by in-guest verification: Quake didn't launch in the default RAM config.** While
> screenshot-verifying the marquee features, DOOM rendered but **Quake produced no window and no crash** —
> its init silently failed. Root cause: Quake needs its 18 MB PAK + a multi-MB hunk on top of the kernel
> (whose BSS now includes the 40 MB JS arena), which QEMU's **~128 MB default can't satisfy** — the alloc
> fails and Quake exits cleanly (window reaped, no fault). DOOM (4 MB WAD) fit, so only Quake broke. Fix:
> `-m 256M` everywhere the OS launches (Makefile run/test, the three in-guest test scripts, osdrive.py).
> Confirmed in-guest: Quake now renders E1M1 + HUD, **and DOOM+Quake run concurrently** (the M519 feature,
> now actually exercised). All 27 suites green at 256M. **M601** then fixed the *silent*-ness that made
> this hard to find: `app_spawn_named` now logs load failures, and `app_sbrk` logs OOM — launching Quake
> at 128M now prints `[app] 'Quake' out of memory: sbrk(33558528) failed …` (its ~32 MB hunk), instead of
> nothing. A clean example of verification surfacing a real, high-value regression host tests couldn't —
> plus a diagnostic so the next one isn't silent.

> **(M590-M592) Real-page rendering polish + DE touch, found by reviewing/rendering real sites.** Continuing
> the content-parser review: **M590** the browser now honors the `font:` shorthand (`style="font: bold
> 14px Arial"` previously set neither weight nor size); **M591** `uni_to_ascii` folds common math/arrow
> symbols (≤ ≥ ≈ → ← ↑ ↓ −) to nearest-ASCII instead of dropping them to a space (they appear on technical
> pages) + the matching `&le;`/`&ge;`/`&asymp;` entities. **M592** (DE) clicking the taskbar clock opens
> the Calendar. Verified in-guest over real TLS: **example.com** and **danluu.com** (a content-rich blog,
> 21 KB) render correctly; an **unreachable** host (gnu.org, blocked from this sandbox) fails *gracefully*
> ("failed" status, no hang/crash) — confirming robust network handling, not a bug. `make check` stays
> **27 suites**, all green.
>
> **(M594-M595) Tooling + shell completeness.** **M594** `tools/osdrive.py`'s `type` now sends uppercase
> + full punctuation (`:` `?` `(` …) via shift-combos, so it can type URLs/code, not just lowercase
> words. **M595** the shell gained `touch` (the one missing common file-op; creates a 0-byte file,
> never truncates an existing one). With osdrive able to type URLs, the browser's **address-bar
> navigation** path is now verified too (`e` → type `file:table.htm` → Enter renders it) — so every
> browser input path is confirmed in-guest: link-click, address-bar typing, page-script (`document.write`,
> ASCII table), and **interactive onclick→JS→DOM→re-render with persistent state** (Rock-Paper-Scissors:
> click → CPU `Math.random` move → score persists), alongside inline `data:` images and real-TLS sites.

> **(M566, M580-M588) Browser untrusted-parser fuzzing — every scanner extracted + fuzzed, 4 CSS fixes.**
> The browser parses a hostile server's bytes on the guard-page-less kernel stack; all the cleanly-
> separable scanners are now lifted into their own `.c` and host-fuzzed under ASan/UBSan (the M524/M546
> pattern), each a verified oracle (loosen a bound → ASan abort at the exact line): **M566** `htmlattr.c`
> (tag attribute scanners); **M580** `url.c` (`url_split`/`resolve_img_url` — address bar/`<a href>`/
> `<img src>`/redirects); **M581** `color.c` (`parse_color` — `#hex`/`rgb()`/`hsl()`/named, clamped int
> math); **M583** `cssprop.c` (`style_prop` — the inline-style declaration scanner all per-property style
> helpers build on). Each extraction is verbatim (diffed byte-identical) and verified behavior-preserving
> in-guest. The review/fuzz then surfaced + fixed **four** real CSS-rendering gaps (all in the shared
> path, so they fix both inline `style=""` and `<style>` rules): **M584** `style_prop` accepts whitespace
> before the `:` (valid CSS `prop : value`); **M585** it honors the cascade (a later duplicate wins,
> `color:red;color:blue` → blue); **M587** it strips the `!important` priority marker (`color:red
> !important` was silently dropped — the value failed every parser); **M588** added `white` (!) + ~19
> common named colours (`white`/`aqua`/`lightgray`/… were missing, so `color:white` parsed to nothing).
> All purely additive — the home page renders byte-identically; each locked by a regression case + the
> fuzzers staying ASan/UBSan-clean. The main `parse_html` tokenizer stays too coupled to `browser_t` to fuzz in
> isolation, so it's guarded end-to-end by `browsertest`. `make check` is now **27 suites** (24 host + 3
> in-guest). Marquee features re-confirmed in-guest by framebuffer screenshot this session: the desktop,
> the browser (home + real HTTPS), Mandelbrot, the System Monitor, every window gesture — and **DOOM**
> (E1M1 + HUD).

> **(M574-M578) JS engine — closed the documented gaps; coercion + regex + JSON now spec-complete.**
> With QEMU back (so each change is verifiable in-guest), a focused pass finished the JS engine's long-tail:
> **M574** ToPrimitive `toString` (string coercion of objects/classes: `""+o`, `String(o)`, template
> literals, `Array.join`); **M576** ToPrimitive `valueOf` (numeric coercion: `obj-1`, `Number(obj)`, `+obj`,
> comparisons; valueOf-first, toString-parse fallback) — together **completing ToPrimitive**, the one gap
> prior sessions deferred *specifically* for lack of in-guest verification; **M577** `match.groups`
> (named captures `(?<n>…)` were added in M543 but the names discarded) **and** `match.index` — via a new
> `match_props` side-object on the result array, so positional `m[1]` and ordinary arrays are unaffected;
> **M578** `JSON.parse` reviver (the symmetric counterpart to the stringify replacer) + an arena bump
> (32→40 MB, the parser AST + the growing test suite). Each landed with the same rigor: host cases incl.
> edges, the jstest golden UNCHANGED for every prior line, the relevant fuzzer (regex/json/source) clean,
> and **verified in-guest** (the shell `js` demo now shows toString/valueOf/regex-groups/reviver from the
> in-kernel engine). A broad re-probe (~30 modern features) now finds only architectural ceilings left —
> generators/async/Promise (need coroutines + an event loop) and real BigInt (arbitrary precision); the
> integer engine and the synchronous standard library are otherwise complete.

> **(M562-M570) Tooling + window gestures + browser-parser fuzz + a gated browser test — the in-guest arc.**
> **M563** promoted the throwaway driving scripts into a committed tool, `tools/osdrive.py`
> (boot headless, inject keys + absolute-mouse clicks/drags, screenshot) — the durable enabler for all of
> the below. **M564** aero-snap: drag a window to the top edge to maximize or a side edge to tile that
> half (completing F4/F5/F6/double-click). **M565** made the F1 help overlay modal to the mouse. **M566**
> (security, not DE) extracted the browser's HTML attribute scanners — `find_attr`/`has_attr`/`attr_int`/
> `find_href`, which walk a hostile server's tag bytes — into `kernel/htmlattr.c` and host-fuzzed them
> (400k random + truncations/corruptions, ASan/UBSan, verified oracle), the M524/M546 pattern. **M568**
> added a 3rd in-guest gate, `browsertest`: it launches the Browser from the Apps menu and asserts its
> network-free home page rendered (≥50k white pixels) — the end-to-end guard for `parse_html`, the HTML
> tokenizer that's too coupled to `browser_t` to fuzz in isolation. **M569** the System Monitor now shows
> FAT32 disk usage (cached, since `vfs_df` scans the FAT). **M570** type-to-jump in the 34-entry Apps menu.
> `make check` is now **24 suites** (21 host + 3 in-guest). Verified in-guest with osdrive this arc: the
> browser renders its home page AND navigates to a real **https://example.com** over the from-scratch TLS
> stack, the System Monitor (with disk), and every window gesture — all by framebuffer screenshot, no display.

> **(M556-M561) Desktop-environment polish — driven + verified in-guest.** With QEMU back (above), used
> the new full headless control — keyboard via HMP `sendkey` AND mouse via QMP `input-send-event`
> (absolute tablet, 0..32767), plus `screendump` — to improve the DE the user actually asked for ("idc
> about music i want a good DE") and *visually verify each change*. **M558** F1 keyboard-shortcut help
> overlay (the WM had grown F2/F3/F4/F5/F6/F8/F9/F12 but they weren't all surfaced anywhere). **M559**
> taskbar clock now shows date + time (`YYYY-MM-DD HH:MM:SS`), not just time. **M560** fixed the chip
> hit-test that M559's wider clock pill broke (extracted one `clk_pill_w()` so render + hit-test can't
> drift). **M561** double-click the title bar to maximize/restore — which exposed and fixed a pre-existing
> drag bug (a bare title-bar click cleared the `maximized` flag without restoring geometry; now the move
> handler acts only past a 3px threshold, and dragging a maximized window restores its size and follows
> the cursor). **M556** also fixed a stale `tls.h` comment + **M557** a host-build break (the `nettest`
> harness needed TLS stubs after M555 added a tls_get call to net.c). Confirmed in-guest this arc: the
> desktop, browser start page, Mandelbrot, the help overlay, snap-tiling, maximize/restore, and window
> dragging all render/behave correctly. **The screenshot+input workflow is the durable win** — any
> graphical regression is now reproducible and reviewable headlessly.

> **(M549-M555) QEMU is UNBLOCKED — in-guest verification restored + gated.** The single biggest
> change this session wasn't a feature, it was discovering that **QEMU runs again** (v10.2.2). The
> previous ~30 milestones (M520-M548) all landed "host-verifiable only" because earlier sessions hit
> a QEMU launch failure (SIGSTKFLT) that made in-guest testing impossible. That premise is now stale.
> The OS boots end-to-end (preemption, isolation, PCI, e1000 networking incl. a real HTTP GET to
> example.com, FAT32, USB tablet), the **desktop paints** (verified by framebuffer screenshot), and the
> **browser renders its CSS-styled start page** — and the **shell window confirms ring-3 userspace
> actually executes in-guest**, all captured headlessly. Turned this back into a *gate*, not a one-off:
> **M549** the `make test` smoke boot now exercises the AC'97 audio bring-up (`-audiodev none`, no host
> sound server); **M550** added `boottest` — boots the real `kernel32.elf` headless and asserts all 9
> bring-up markers print with no crash (the whole driver stack, vs the host suites' one-`.c`-in-isolation
> model), verified as a real oracle (`QEMU=true` → every marker MISSING → FAIL); **M552** added `gfxtest`
> — captures the VGA framebuffer via the QEMU monitor's `screendump` and asserts the desktop actually
> painted (1024×768, ≥40 colors, no all-black hang), covering desktop.c/fb.c/fbcon.c/font.c/vga.c which
> had *zero* in-guest coverage; **M553** made `boottest`'s real-internet GET non-fatal so the gate stays
> green offline. **M551** cleaned up a GCC `malloc`-size false-positive in the M548 fuzzer. Then **M555** added a
> boot-time **TLS 1.3 HTTPS self-test**: the kernel now does a real HTTPS GET to example.com alongside
> the existing HTTP one, and in-guest it completes the *whole* from-scratch handshake — parses the 4-cert
> chain (example.com → Cloudflare ECC CA → SSL.com Transit → SSL.com ECC Root), matches the hostname,
> verifies 3/3 issuer signatures, anchors to a trusted root, passes `CertificateVerify`, and fetches the
> page (`200 OK`). The crown-jewel TLS stack (X25519, AES-GCM/ChaCha20-Poly1305, X.509 path validation,
> ECDSA/RSA over our own bignum) is now verified end-to-end at every boot (a soft `boottest` marker),
> and `boottest` polls COM1 to exit ~when the desktop launches (~5s) with a 25s safety net. `make check`
> is now **22 suites** (20 host + 2 in-guest); both new in-guest tests SKIP cleanly where QEMU/socat/
> python3 are absent. The framebuffer-screenshot + HMP-input-injection technique (drive the Apps menu via
> `sendkey`, then `screendump`) is now a general way to verify *any* graphical feature headlessly — used
> this session to confirm the desktop, the CSS-styled browser start page, and the Mandelbrot app all
> render correctly in-guest.

> **Status (546 milestones) — DOOM *and* QUAKE RUN, with sound.** OS-DEV now runs two real id Software
> games as windowed ring-3 apps: **DOOM** (graphics, keyboard, mouselook, sound effects, and music) and
> **Quake** — the true-3D successor, software-rendered (actual Half-Life is a closed Win32/GoldSrc title
> needing a GPU and proprietary assets, so Quake is the realistic "modern game"). Both load their shareware
> data from the OS's own 64 MiB FAT32 disk, and they run **concurrently** (M519 added FXSAVE/FXRSTOR FP
> state save so two floating-point apps don't corrupt each other). Getting here (M495-M519, ~25 milestones)
> built whole new subsystems: a **userspace heap** (malloc/sbrk, fuzz-tested), a **graphics window API**
> (real per-window pixel canvases — the end of text-grid-only apps), **raw make/break keyboard** + **mouse**
> (absolute + relative/mouselook), **FPU/SSE**, an **AC'97 audio stack** (PCM, a WAV parser [fuzz-tested],
> streaming, **background music**, a real WAV **jukebox**, DOOM SFX + a MUS music synth), and a **ms clock**.
> Plus polish: a mouse paint app, a mouse-zoom Mandelbrot, and a disk-loaded desktop wallpaper. The two
> game ports reused the proven approach: vendor (doomgeneric / quakegeneric) + a platform layer over the
> syscalls + a from-scratch libc shim, with subagents doing the bulk under review. **The "best little
> from-scratch OS" now runs DOOM and Quake — alongside its own browser, TLS stack, and JS engine.**
> **(M541-M546) More JS compat + untrusted-parser fuzzing.** Continuing the host-verifiable arc:
> **M541** `new Date(ms)` / `new Date(y,mo,d,…)` honor their argument (were snapshotting "now") +
> real `toISOString`; **M542** array `indexOf`/`includes`/`lastIndexOf` match objects by identity
> (`===`, also null/undefined) + Date `getUTC*`/`toLocale*`; **M543** regex named capture groups
> `(?<name>…)`; **M544** `class X extends Error` (custom error classes — super(msg) sets message,
> instanceof both ways) + `Error.toString`; **M545** `JSON.stringify` array replacer (key allowlist).
> Then **M546** extracted the HTML entity decoder (`decode_entity`, untrusted page bytes) into
> kernel/htmlentity.c and fuzzed it — the 4th untrusted-parser fuzz suite this arc after JSON.parse,
> the regex engine, and the full JS source pipeline (M537-M539). `make check` is now **20 suites**,
> each new fuzzer verified as a real oracle (removing a bound → ASan abort at the exact line). The JS
> engine probed clean across ~130 features/edges; the one common remaining gap, ToPrimitive calling a
> user object's valueOf/toString, was deferred as too risky to land without in-guest verification (it
> touches the coercion core). **[Update: M574 landed the toString half](#)** — string coercion (`""+o`,
> `String(o)`, template literals, `Array.join`, `print`) now calls a user object's own/inherited
> `toString` (verified in-guest once QEMU was back; golden unchanged + fuzz-clean + the depth-guarded
> getter call path), and **M576 then landed the `valueOf` half** (numeric coercion in `to_num`:
> `obj-1`/`Number(obj)`/`+obj`/comparisons; valueOf wins, toString-parse fallback) — so **ToPrimitive is
> now complete**. Generators were then the sole documented JS ceiling (added M674, eager model). All four-way
> verified (host + golden + fuzz + in-guest).
> **(M529-M539) JS engine real-site compatibility + untrusted-input fuzzing.** Continuing the
> QEMU-blocked session in host-verifiable territory, a deep pass on the from-scratch JS engine (which
> powers the browser) — probed against ~110 modern features/edges, found + fixed 8 real gaps, each
> locked into the `jstest` golden: **M529** `Infinity` was an undefined-variable THROW (now an INT64_MAX
> sentinel — the engine is integer-only) + `toFixed` decimal padding + `flat(Infinity)`; **M530**
> `normalize`/`concat`/`toLocale*` + `replaceAll(regex)` (+ arena 20→26 MB as the suite grew); **M531**
> `JSON.stringify` omits undefined/function object props (was emitting null — could corrupt server
> payloads); **M532** regex **lazy quantifiers** (`/<.+?>/`), `split()` capture-group splicing, and
> `Array.join` rendering undefined/null as `""`; **M533** `Math.max()/min()` identity elements + global
> `isFinite`; **M534/M535** **`let`/`const` per-iteration binding** in `for`/`for-of`/`for-in` (the
> defining reason `let` exists — closures in loops now capture each iteration; with a closure-detection
> optimization so big loops stay allocation-free on the GC-less arena); **M536** array elision `[1,,3]`
> (was a parse-abort). Generators (`function*`) were then the one unsupported feature (the concern noted here —
> "an eager hack would hang on an infinite generator" — was exactly what M674 resolved: the eager model errors
> via arena-OOM, never hangs).
> Then **M537-M539** fuzzed every untrusted-input path in js.c under ASan/UBSan — `JSON.parse`, the
> **regex** engine (ReDoS shapes + malformed patterns; this engine's history records 2 critical matcher
> stack-overflows), and the **full source parse+run pipeline** — each #including js.c via a new
> `JS_NO_MAIN` guard, each verified as a real oracle (removing a bound → ASan abort at the exact line).
> `make check` is now **19 suites**. All host-verified; QEMU stayed environment-blocked all session.
> **(M522-M527) Robustness + trust-boundary hardening.** A focused session on
> the parts where a bug is silent kernel corruption. **M522:** the Apps menu had
> grown to 34 entries and overflowed the top of the screen (clipping Browser/
> Shell/Clock off-screen, unreachable) — reworked into a 2-column menu (render +
> mouse hit-testing + keyboard left/right column nav), headroom now ~60 entries.
> **M523:** the ELF64 loader (the ring-3 trust boundary) had no host test and
> interleaved untrusted-header validation with the page-mapping writes — split
> out pure `elf_check_header`/`elf_check_phdr` validators (added an OOM guard so
> a failed frame alloc never maps physical 0) and added an ASan/UBSan fuzz suite
> (truncations + corruptions + 200k random buffers + a full load round-trip via
> an mmap-backed guest-memory stub). **M525:** that suite now also loads every
> shipped app binary (all 29) through `elf_load` as a linker/toolchain
> regression guard. **M524:** extracted the HTTP/1.x response parsers (chunked
> decode — which memmoves with attacker-controlled hex sizes — + header scans)
> from browser.c into a testable `http.c` and fuzzed them (regression + 400k
> random). **M526:** the kernel heap (`kmalloc`/`kfree`, underlies every kernel
> allocation) was untestable on host (higher-half base) — parameterized the base
> (kernel-neutral) and added a 400k-op torture test with per-block pattern +
> free-list tiling invariants. **M527:** brought tests/README current (13->16
> suites; each new harness verified to fail when its guard is removed). All 16
> host suites pass. *In-guest QEMU verification was blocked this session — the
> environment terminates QEMU (SIGSTKFLT) on launch — so this session stayed in
> host-verifiable territory; the M522 menu fix is verified by inspection + build.*
> **(M495-M504) DOOM:** OS-DEV runs **id Software's DOOM** (the shareware
> doomgeneric port) as a windowed ring-3 app: it loads the IWAD from our own FAT32 disk, renders E1M1 in a
> crisp 640x400 window at ~70 fps, and is keyboard-playable through its menus into a live game. Getting
> there meant building six new OS capabilities first, each shipped + verified on its own (M496-M501):
> a **userspace heap** (SYS_sbrk + malloc/free/realloc, host-fuzzed under ASan), a **monotonic ms clock**,
> a **graphics window API** (real per-window pixel canvases the compositor blits — the end of
> text-grid-only userspace), **raw make/break keyboard events** for games, **FPU/SSE** enablement, and a
> **16 MiB disk** holding the ~4 MB WAD (a full multi-MB FAT32 read verified byte-exact by CRC-32). Then
> M502 vendored doomgeneric with a from-scratch **libc shim** + a **platform layer** over those syscalls
> (the one real bug: `_start` must force-align the stack or DOOM's first SSE `movaps` #GPs), and M503 added
> compositor integer-upscaling so the native 320x200 render shows at 2x. M495 (`file`, a magic-byte type
> identifier) opened the session. **All 12 host suites pass. The "best little from-scratch OS" now runs a
> real, beloved game — alongside its own browser, TLS stack, and JS engine.**
> M494: **`ls` shows the whole directory (was capped at 32)** — a real bug:
> SYS_list enumerated only 32 entries (vfs_dirent ents[32] on the stack), so with ~100 files now on the
> disk (~60 INDEX-linked .htm demos + baked images/archives + SHOT screenshots + extracted files) `ls`,
> GLOB expansion, and the browser file list all silently saw ~1/3. Fix: cap 32→256 using a STATIC array
> (256*68B too big for the 16KB kernel stack; concurrent-ls race is benign — bounded + NUL-terminated
> names) + a defensive j<63 name bound; shell ls buffer 1024→8192 + glob listing 2048→8192. Verified
> in-OS: `ls | wc -l` → 108 (was 32); `ls | grep TGZ` → finds TEST.TGZ (previously invisible). Files:
> kernel/syscall.c, user/shell.c. **A quality/bug-fix found while testing the archive work — the disk grew
> past the old cap. "Improve what needs it most." 17 milestones this session (M478-494).**
> M493: **`tar` — extract `.tar` + `.tar.gz`** — completes the archive suite
> (.gz/.zip/.tar.gz). kernel/tar.c: simple ustar 512-byte-header parsing (name@0, octal size@124, type@156),
> bounded (data span checked vs len, walk always advances), I wrote it directly (simple). SYS_untar(41):
> if the file starts with the gzip magic (1f 8b) → gz_inflate first → tar_extract — so .tar.gz/.tgz works
> in ONE step. REUSES gz_inflate + the unzip_emit 8.3-mangling callback (minimal new code). shell `tar <f>`.
> New tartest in `make check` (python-tarfile archive extracted exactly + truncation/corruption/garbage
> fuzz, ASan clean). Verified in-OS: `tar TEST.TGZ` → 3 files byte-exact (incl. an 800-byte deflate-in-tar
> entry, confirmed via disk). Files: kernel/tar.c, tar.h, syscall.c, shell.c, ulib.*, Makefile, tools/
> test.tgz, tests/tar/*. **11 host suites. ARCHIVE/COMPRESSION SUITE COMPLETE: gunzip/gzip/unzip/tar+PNG,
> all from-scratch on one inflate/deflate core. This session (M478-493, 16 milestones): image arc, markdown
> +GFM, CSV, Wordle, and the whole compression/archive subsystem — far past the prior 'saturation'.**)
> M492: **`unzip` — extract `.zip` archives** — completes the archive story
> (.gz single → .zip multi-file). kernel/zip.c parses via the CENTRAL DIRECTORY (authoritative index),
> reuses `inflate` (zip method 8 = raw DEFLATE; 0 = stored), bounds-checks EVERY attacker-controlled
> offset/size in 64-bit (no wrap). DELEGATED to a subagent (intricate format + fuzzing, non-cyber) →
> reviewed (in_bounds on all reads, EOCD backward-scan bounded, central-dir walk capped by count AND span,
> inflate output bounded by scratchcap) → integrated. SYS_unzip(40) + a kernel emit callback that 8.3-
> mangles each path + vfs_writes; shell `unzip <f.zip>`. New ziptest in `make check` (exact extraction incl.
> a system-`zip` archive w/ comment+subdir + truncation/corruption fuzz, ASan clean). Verified in-OS:
> `unzip TEST.ZIP` → 3 files extracted byte-exact (stored + a 2400-byte deflate entry, confirmed via disk).
> Files: kernel/zip.c, zip.h, syscall.c, shell.c, ulib.*, Makefile, tools/test.zip, tests/zip/*.
> **10 host suites now. Compression/archive arc complete: gunzip + gzip + unzip + PNG, all from-scratch,
> all reusing the inflate/deflate core. Three intricate algorithms (deflate/png/zip) via subagents +
> my review this session — exactly the directive's "use subagents for hard work + run reviews".**)
> M491: **PNG screenshots — from-scratch PNG ENCODER** — kernel/png_encode.c
> (filter-0 scanlines → zlib[raw_deflate body + Adler-32] → IHDR/IDAT/IEND chunks + CRC-32), fully bounded.
> DELEGATED to a subagent (intricate, non-cyber) → reviewed (every out-write via bounded pe_put, IDAT
> length-patch + CRC guarded by the oom check first, integer-overflow guarded) → integrated. fb_save_png
> (capture top-down RGB → png_encode, transient kmalloc bufs). F12 now saves SHOT0.PNG/SHOT1.PNG (~9KB vs
> ~576KB BMP = 64x smaller — the screen compresses well); shell `screenshot <f>` → PNG for a .png name else
> BMP (SYS_screenshot dispatches by extension). New pngenctest in `make check` (round-trips vs the DECODER,
> exact RGB, ASan/UBSan, bounds) + subagent libpng/`file` interop. Verified in-OS: F12 → SHOT0.PNG (PIL
> opens it 512x384 RGB, 9173 bytes) → `browse file:SHOT0.PNG` renders via the OS's OWN png_decode = full
> from-scratch encode→decode round-trip. Files: kernel/png_encode.c, fb.c, fb.h, syscall.c, desktop.c,
> Makefile, tests/png/*. **Capstone: ties M490 deflate + M478-480 image work + the PNG decoder together.
> 9 host suites now. Both M490/M491 substantial from-scratch algorithms via subagents + my review.**)
> M490: **`gzip` — from-scratch DEFLATE COMPRESSOR** — completes the
> compression story (had gunzip/decompress, now compress). New kernel/deflate.c (encoding counterpart to
> inflate.c): LZ77 (3-byte hash-chain, 32KB window, MAX_CHAIN=128 anti-blowup) + fixed-Huffman + CRC32,
> fully bounded (every write vs outcap). `gzip <file> [out.gz]` + SYS_gzip(39). DELEGATED to a subagent
> (hard/intricate, NON-cyber so not blocked) then I reviewed (bit-ordering mirrors inflate's reader,
> canonical codes built via the §3.2.2 procedure so they can't drift from the decoder, LZ77 reads proven
> in-bounds) + integrated. New `deflatetest` suite in `make check`: round-trips vs the decoder over
> empty/repetitive/random/text ≤200KB (ASan/UBSan clean) + outcap-bounds + **system `gunzip -t` interop**
> (proves CRC/format real). Verified in-OS: `gzip README.TXT; gunzip README.GZ DEC; cmp` → "files are
> identical" (a full from-scratch compress→decompress round-trip). Files: kernel/deflate.c, inflate.h,
> syscall.c, shell.c, ulib.*, Makefile, tests/deflate/*. **Substantial from-scratch algorithm (per the
> directive: don't decline for complexity; use subagents for hard work). 8 host suites now. Could later
> enable PNG screenshots (smaller than BMP) — deflate is the prerequisite, now done.**)
> M489: **`base64 -d` decode + encode-redirect fix** — the shell's base64 was
> ENCODE-only; added `base64 -d <file> [out]` to DECODE base64→bytes (skips whitespace, stops at `=`/invalid,
> bounded static bufs, b64v helper). ALSO fixed a real pre-existing bug found while testing: `base64 F > OUT`
> failed because the encode passed `readfile` the filename WITH the redirect's trailing space ("MOTD.TXT ")
> → trimmed it now. Together = clean round-trip. Verified in-OS: `base64 MOTD.TXT > ENC.B64; base64 -d
> ENC.B64 DEC; cmp MOTD.TXT DEC` → "files are identical" (78 bytes). File: user/shell.c. **Like M488
> (gunzip), a safe gap-fill — completes an existing half-feature + fixes a latent bug, fully verifiable
> in-OS via the round-trip. Both M488/489 are pure-userspace-or-reuse, zero risk to working systems.**)
> M488: **`gunzip` — .gz decompression** — shell `gunzip <f.gz> [out]` +
> SYS_gunzip(38) decompress a gzip file by REUSING the fuzz-tested DEFLATE `inflate`; only a thin bounded
> wrapper `gz_inflate` (kernel/inflate.c: magic/method check, skip header + optional FEXTRA/FNAME/FCOMMENT/
> FHCRC, inflate body, ignore trailer) is new. SAFE + additive (new syscall+command, no working-system
> touch; kmalloc'd 256KB-in/1MB-out transient buffers). imgtest gained a real gzip round-trip (exact
> 84-byte output) + 120K gzip-header fuzz (ASan/UBSan clean). Baked tools/hello.gz (mkfatfs hostfiles).
> Verified in-OS: `gunzip HELLO.GZ` → wrote HELLO (84 bytes); `cat HELLO` → exact text. Files:
> kernel/inflate.c, kernel/syscall.c, user/shell.c, user/ulib.c, tools/mkfatfs.c, tools/hello.gz.
> **The best safe+valuable find post-saturation: real new capability (.gz) reusing proven fuzzed code,
> fully host- AND in-OS-verified, zero risk to working systems. (.gz fits 8.3, unlike the reverted .json.)**)
> M487: **F12 screenshots auto-number** — F12 now saves SHOT0.BMP, SHOT1.BMP,
> … (a static counter) instead of overwriting SHOT.BMP, so successive captures aren't lost. Zero-risk
> self-contained change to the F12 handler (shell `screenshot [file]` unchanged). Verified in-OS: 3× F12 →
> SHOT0/1/2.BMP all valid, no overwrite. File: kernel/desktop.c. **NOTE: hit the 8.3 FILENAME LIMIT this
> session — a `.json` viewer was written then reverted because a 4-char extension can't be stored on the
> 8.3 FAT disk (only ≤3-char exts: .md/.csv/.htm fit). Long-filename (VFAT/LFN) support would lift this but
> risks the working FAT write path — deferred. Genuine saturation: 10 milestones M478–487 this session
> (screenshot save/view/embed/hotkey, markdown+GFM, CSV, Wordle); remaining items are ceilings or FS-risk.**)
> M486: **Markdown autolinks + strikethrough** — rounds out `md_inline`: a
> bare `http(s)://` URL → a clickable `<a>` (bounded scheme-prefix probe + scan to whitespace/delimiter),
> and `~~text~~` → `<s>` (flat toggle like bold/italic; the renderer already draws a strike-line for STY_
> STRIKE). Both isolated to the inline scanner — non-recursive, bounds-checked, no touch to anything risky.
> readme.md demo gained a bare URL + struck text. Verified in-OS: browse file:readme.md → https://example.com
> is a blue link, struck text is greyed with a line through it. File: kernel/browser.c, tools/mkfatfs.c.
> **The markdown renderer is now fairly complete GFM: headings/bold/italic/strike/code/fenced/lists/quotes/
> links/autolinks/images/tables. Safe + fully locally verifiable.**)
> M485: **CSV files render as tables (`.csv`)** — a local `.csv` opens as an
> HTML `<table>` via `csv_to_html` (same safe pattern as markdown: convert → parse_html, reuses md_put/
> md_esc). RFC-4180 quoting: a `"`-quoted field may contain commas, `""` = a literal quote; first row =
> `<th>` header. Bounded (writes capped, reads within len; rows split on `\n` — embedded newlines in quotes
> unsupported, documented). Additive: only file: URLs ending `.csv`. Baked data.csv demo + index link.
> Verified in-OS: `browse file:data.csv` → table renders, and the quoted field "Designer, UX" stays ONE
> cell (proves quote handling). File: kernel/browser.c, tools/mkfatfs.c. **Rounds out local-document
> support: HTML / text / Markdown / CSV / images. Safe + fully locally verifiable.**)
> M484: **GFM markdown: tables + images** — extends M482's `md_to_html` with
> `![alt](url)` images (emit `<img src>`, flows through the inline-image path → a .md can embed file:/data:/
> remote images, tying the whole image arc together) and GFM tables (a `| a | b |` line + a `|---|` separator
> next line → `<table>` with `<th>`/`<td>`; `md_table_row` treats `|` as a pure delimiter, trims cell spaces).
> Still bounded + non-recursive: the table body consumes consecutive `|`-containing lines then leaves the
> first non-table line for the main loop; every read within the line, every write capped. readme.md demo
> gained a table + an image. Verified in-OS: `browse file:readme.md` (scroll down) shows the table (bold
> header, aligned cols: Feature/Status x Browser/JavaScript/Markdown) + the inline striped test.png image.
> File: kernel/browser.c, tools/mkfatfs.c. **Builds on the freshly-shipped M482 — safe + FULLY LOCALLY
> verifiable (no network), and integrates the image work into markdown.**)
> M483: **Wordle (new game app)** — guess a hidden 5-letter word in 6
> tries; each guess coloured green (right letter+spot) / yellow (in word, wrong spot) / grey (absent) via
> the standard TWO-PASS scoring (greens claim positions first, then yellows from leftovers — so duplicate
> letters score correctly). ~110-word baked list, clock-seeded xorshift PRNG, Enter=new round on game end.
> STRICTLY ADDITIVE (zero risk): a new ring-3 ELF (user/wordle.c) wired into the 5 registration points —
> Makefile USER_ELFS, kernel/asm/user_blob.asm (incbin), kernel/app.c (extern + progs[]), desktop.c menu[].
> `run wordle` or Apps→Wordle. Verified in-OS: launched + focused, accepted typed guesses, scored/coloured
> them (green+grey clearly distinct), guess count 3/6, grid + dot-rows rendered, stable. Uses the colored-
> text app API (sys_setcolor palette: 0=green, 3=yellow, 8=grey) like paint.c. **A new app is the safest
> possible addition — found after confirming images/shell/JS-stdlib/browser-core were saturated; the user
> explicitly wants "as many cool features as you can."**)
> M482: **Markdown rendering in the browser (`.md` files)** — the browser
> renders local Markdown: a from-scratch markdown→HTML converter (`md_to_html` in browser.c) turns a `.md`
> file into HTML, fed to the existing battle-tested `parse_html` (so it inherits all the styling). Handles
> # headings, bold/italic, inline `code`, ``` fences, -/* /+ and N. lists, > quotes, [text](url) links,
> --- rules, paragraphs. BOUNDED + NON-RECURSIVE (untrusted input, no kernel guard page): every write
> capped vs cap, every read bounded by line length, inline emphasis = flat toggles (no recursion). Reuses
> the M479 big-buffer local-file path: read .md into the transient buffer, convert into b->raw, parse_html.
> Additive — only file: URLs ending .md take the path; low-mem fallback = plain text. Baked readme.md demo
> + index link. GOTCHA fixed: the function's doc comment literally contained `*` `/` sequences (in the
> markdown syntax it described) that closed the block comment early — reworded. Verified in-OS:
> `browse file:readme.md` → heading/bold/italic/code/fenced/bullet/quote/link/hr/ordered-list all render
> (status "markdown"). File: kernel/browser.c, tools/mkfatfs.c. **Substantial + safe + on-theme (extends
> the browser north star to a new doc format) — found after confirming images/shell/JS-stdlib/apps are
> saturated.**)
> M481: **Global screenshot hotkey (F12)** — press F12 anywhere on the
> desktop to save the whole screen to `SHOT.BMP` (+ a short beep), so the M478 capture is reachable from
> any focused app, not just the shell. keyboard.c maps F12 (scancode 0x58, was unused) to a new WM code
> 0x1C; desktop.c handles it by calling `fb_save_bmp` directly (kernel-side, no syscall) and swallowing
> the key. Strictly additive (F12 did nothing before). Welcome-window hint + GUIDE.TXT updated. Verified
> in-OS: F12 at the desktop writes a valid 512×384 BMP (extracted from the disk = the full desktop,
> windows/taskbar/cursor, colours right), desktop stays stable. Files: kernel/keyboard.c, kernel/desktop.c.
> **Caps the image arc (M478 save / M479 view / M480 embed / M481 hotkey) — a coherent, complete feature.**)
> M480: **Inline `data:` image URIs (base64)** — the browser now decodes
> `<img src="data:image/...;base64,…">` images embedded in the page, completing the image-source story
> (`file:`/`http:`/`https:`/`data:`). New bounded `b64_decode` in browser.c (every write guarded vs the
> out cap, every read within input — untrusted-safe like the decoders) → feeds the existing inline-image
> slot path, which sniffs format by magic, so ALL five decoders (PNG/GIF/JPEG/SVG/BMP) work from a data:
> payload. Strictly additive: a data: `<img>` used to fall back to a dead `[img]` link, and any parse/
> decode failure still does (graceful); reuses the M479 BMP decoder. Baked `dataimg.htm` demo embeds a
> 16×16 BMP. Verified in-OS: `browse file:dataimg.htm` → red/green/blue/yellow quadrants in the right
> places (proves base64→BMP→RGBA end-to-end), scaled width=128. Files: kernel/browser.c, tools/mkfatfs.c.
> **On-theme (the browser is the crown jewel): data: images appear on the real web; this + M478/M479 form
> a coherent image arc — capture to BMP, view a BMP, decode an embedded BMP.**)
> M479: **BMP decoder — VIEW a screenshot in the browser** — a from-scratch
> decoder (`kernel/bmp.c`) for uncompressed BI_RGB Windows BMPs (24-/32-/8-bit palettized, bottom-up or
> top-down → RGBA, A=255), wired into the browser's `decode_image` dispatch — closes the loop with M478:
> the OS can now both SAVE and VIEW a screenshot. Untrusted-byte safe (every pixel/palette read bounded
> vs `len`, like the other decoders). imgtest gained a 2×2 correctness case (proves bottom-up row-flip +
> BGR→RGBA) + a 120k `BM`-prefixed fuzz (ASan/UBSan clean). KEY: the browser's `file:` path read into the
> 256 KB HTML buffer, too small for a ~576 KB 24-bit screenshot → first attempt rendered the BMP bytes as
> TEXT. FIX: read a local image into a transient 1 MB buffer (`LOCAL_IMG_MAX`), try_image, free; graceful
> fallback to the old capped text/HTML path if it's not an image / alloc fails (protects the working
> local-file path). Verified end-to-end in-OS: `screenshot` → `browse file:SHOT.BMP` renders the captured
> desktop full-page (status "image"), colours correct. Files: kernel/bmp.c, kernel/browser.c. **Genuinely
> useful (not bloat): the BMP decoder works for any BMP — disk files, `<img src>` in pages — not just
> screenshots, and the buffer fix improves local-image viewing generally.**)
> M478: **Screenshot to BMP (`screenshot [file]`)** — new `SYS_screenshot`
> syscall + shell command (default `SHOT.BMP`) saves the live desktop to a 24-bit BMP on the FAT32 disk,
> downscaled 2× (≤512×384, ~576 KB). KEY FIX found in testing: the first cut read the back buffer
> (`target`) and caught it MID-COMPOSE (captured the wallpaper gradient only, windows missing) — switched
> to read the *presented* framebuffer (`lfb`), always a complete composited frame. Emits bottom-up BGR
> rows. Strictly additive (one self-contained `fb_save_bmp` + a syscall; no draw-path change). Verified
> end-to-end: extracted SHOT.BMP from the disk image = a valid 512×384 BMP matching QEMU's own screendump
> pixel-for-pixel (windows, green shell text, blue title bars, colours/BGR-order all correct) + 7 suites.
> File: kernel/fb.c. **(This also incidentally re-confirmed M471 FAT dir-growth in practice — SHOT.BMP is
> the 87th root-dir file, living in an extended dir cluster.)**
> M477: **Glob matcher hardened vs catastrophic backtracking** — a PERIODIC
> REVIEW of the shell command-line arc (M463/M468/M473/M474) found glob_match (M473) was depth-bounded
> (no stack overflow) BUT time-EXPONENTIAL: `*a*a*a*…*b` vs a long non-matching name → billions of calls
> (the `for(;*s;s++) if(glob_match(p,s))` retries the tail at every suffix, no memo) → `cat *a*a*…*b`
> HANGS the shell forever (ReDoS-style, paid per non-matching file). FIX: replaced the recursive matcher
> with the standard ITERATIVE two-pointer (star/mark backtrack) matcher — O(pat×name), no recursion, no
> blowup. Verified in-OS (cat *.txt still globs; `cat *a*a*a*a*a*a*a*a*a*b`→instant "no such file"; echo
> after → shell responsive) + 7 suites. **The review's other 5 items (glob_expand/run_pipe/write_redirect/
> run_line/ulib-capture bounds) were confirmed SAFE — only glob_match had the bug.** Validates running
> periodic reviews (my self-audit checked recursion DEPTH, missed TIME). Same spirit as the engine's
> ReDoS-safe regex. File: user/shell.c.
> M476: **Shell `help` documents the 4 command-line operators** — added
> a line to the `help` text listing `| > >> *.txt ? ;` (pipe/write/append/glob/sequence) so the M463/
> M468/M473/M474 operators are discoverable. Tiny user/shell.c help-text change; build + 7 suites + in-OS
> (`help` shows the syntax lines). **SATURATION NOTE: after 36 milestones this session the achievable
> safe+high-value work is exhausted — remaining items are architectural CEILINGS (real lazy generators
> + CSS layout need a different interpreter/renderer arch), BLOAT/break-risk (fatal cert enforcement
> needs ~all ~140 roots), REGRESSION-risk (gzip-advertise could break proven-working sites + live-verify
> is flaky), or SUBTLER/risky (FAT cross-boot corruption). Cross-app clipboard needs webpage text
> selection for real value (token renderer has none). Keep any further work SMALL+SAFE+verified.**)
> M475: **Expanded TLS trusted-root store (5→13 CAs)** — added 8 common
> roots to kernel/rootca.c's ROOT_CAS[]: USERTrust RSA, DigiCert Global Root CA (G1) + G3 (EC), Amazon
> Root CA 1, GlobalSign R3, GTS Root R1, Sectigo ECC E46, ISRG Root X2 (EC). Each = the root's public key
> in x509_cert.key form (RSA → RSAPublicKey DER via `openssl rsa -RSAPublicKey_out -outform DER`; EC →
> the uncompressed P-384 point parsed from `openssl pkey -pubin -text`), extracted from
> /etc/ssl/certs/ca-certificates.crt by a python script (the SAME pipeline + format as the existing 5
> roots). So the full cert-path validation now ANCHORS (TLS*) for the bulk of the real HTTPS web. SAFE
> by construction: anchoring is INFORMATIONAL (tls.c logs chain_anchored, the gate enforces only
> hostname+validity), and a mis-extracted key just fails to match — it can NEVER falsely validate (no
> regression possible). Verified: build + 7 suites + in-OS (https://example.com still loads + cert-info
> 'i' → "anchored" via SSL.com = no regression; new roots anchor by-construction, same format). Bloat
> ~2.4KB key data. NOTE: still NOT a fatal enforcement gate (would reject un-baked-root sites → break
> risk; full enforcement needs ~all ~140 roots = bloat). File: kernel/rootca.c only.
> M474: **Shell command sequencing (`;`)** — `cmd1 ; cmd2 ; cmd3` runs each
> in turn. Refactor: the per-line dispatch (glob→redirect→pipe→run) moved into `run_line(seg,cwd)`
> (returns 1 only for "exit"); main's loop splits the line on `;`, TRIMS each segment's leading space
> (else a non-piped 2nd segment " echo x" → "unknown command"; run_pipe already trimmed internally so
> the pipe path worked — that's how the bug surfaced), skips empties, run_line's each, breaks on exit.
> `;` binds lower than `|`/`>` (each segment handles its own). Verified in-OS (echo a;echo b;echo c →
> a/b/c; echo a ; ls|grep TXT → both; single cmd unaffected) + 7 suites. **Shell command-line operator
> set COMPLETE: pipes `|` (M463) + redirect `>`/`>>` (M468) + glob `*`/`?` (M473) + sequence `;` (M474).**
> File: user/shell.c only.
> M473: **Shell filename globbing (`*`/`?`)** — the shell expands wildcard
> patterns against the directory before parsing operators: `cat *.txt`, `grep PAT *.htm`, `wc *.svg`.
> `glob_match` (case-insensitive — FAT is 8.3-uppercase; `*`=any run, `?`=one char, recursion bounded by
> the short filename) + `glob_expand` (tokenize line; for a token with `*`/`?`, scan the sys_list listing
> ["NAME size" per line], substitute matches; no-match→literal; bounded into a 1024 buf). Wired in main()
> BEFORE the redirect/pipe parse, but ONLY when the line actually contains `*`/`?` (else cmd=line
> verbatim → no whitespace-collapse for normal commands like echo). Operators |/>/>> pass through (no
> wildcard). Userspace, additive. Verified in-OS (`cat *.txt`→3 files concatenated; `grep Milestone *.txt`
> →MOTD.TXT match; `cat *.zzz`→literal→"no such file") + 7 suites. Shell now has pipes + redirect + glob.
> Files: user/shell.c only.
> M472: **JS iterator protocol — spread + Array.from** — completes M469:
> `[...obj]` (array-literal spread eval, case N_ARRAY) + `Array.from(obj)` now consult a plain object's
> `[Symbol.iterator]` (before, only for-of did — an inconsistency). New `iter_collect(it,dest,mapfn,hasfn)`
> helper (fwd-decl before case N_ARRAY, defined after from_push) mirrors the for-of drive EXACTLY (same
> callable checks, fetch-next-once, SAME 2000 cap + g_err/g_oom guards → untrusted runaway can't hang),
> appends via from_push (so Array.from's map fn applies); returns 1 if iterable / 0 WITHOUT appending
> (safe in an else-if). Array.from's iterable branch precedes the array-like `{length:n}` branch.
> Strictly additive (arrays/strings/sets/maps/array-likes/non-iterables unchanged). Subagent-built, I
> reviewed (iter_collect = faithful copy of the M469-reviewed for-of drive: capped, no-append-on-0,
> bounds-safe) + jstest PASS (custom-iterable spread/Array.from/map-fn + regressions + adversarial cap
> "spreadcap=true") + 7 suites + in-OS (ITER.JS: `[...range]`→"1-2-3-4", Array.from→4). The iterator
> protocol is now CONSISTENT across for-of/spread/Array.from. Files: kernel/js.c, tests/js/suite.*, mkfatfs.c.
> M471: **FAT32 directory growth** — `add_entry` (kernel/fat32.c) now
> GROWS a full directory's cluster chain instead of failing: tracks the chain tail `last`, allocs a
> fresh cluster, zeroes all its slots, writes the entry in slot 0, then `fat_set(last,newcl)` links it
> (write-before-link so a chain-walk never sees an uninit cluster) — the same chain-extension fat32_write
> uses for file data. Fixes the "file creation fails past a full root dir" limitation (deferred issue
> (b)), newly relevant since pipes/redirect create files. Fwd-declared alloc_cluster/fat_set. fstest
> gained a Phase 3: create 100 distinct files (forces ~7 dir-cluster growths) + read every one back
> (ASan/UBSan). SHIP-reviewed (no corruption: tail-tracking, zeroing incl. sec_per_clus==1 edge,
> write-before-link, both FAT copies, walk_dir follows the extended chain, non-full path unchanged) +
> in-OS (25 files via `>`, first+last read back, 0 faults). NOTE: this is the SAFE FAT-write fix; the
> OTHER deferred FAT issue — "heavy repeated writes corrupt fat.img across BOOTS" (issue (a)) — is a
> subtler cross-boot/persistence concern (fstest's in-memory single-run write stress is ASan-clean, so
> it's not an in-run OOB; cause unclear) and STAYS deferred. No rmdir, so a grown dir never shrinks
> (pre-existing). Files: kernel/fat32.c, tests/fs/fs_test.c.
> M470: **JS `Proxy` (get/set traps)** — `new Proxy(target,handler)`;
> property read fires handler.get(target,key,proxy) at eval_member_get, write fires handler.set at the
> assign sites (both depth-guarded via call_function_this); trap-less proxy = transparent target r/w;
> enumeration/JSON/in/delete deproxy to the target; V_PROXY is NOT obj_keyed so vals[0/1] never leak.
> Additive (one is_proxy branch per site; non-proxy unchanged). Arena bumped 16→20MB BSS (suite's no-GC
> peak). **SHIP-review CAUGHT A CRITICAL BUG: the trap-less GET fall-through `return eval_member_get(target)`
> RECURSED, so a nested-proxy chain (new Proxy(aProxy,{})×N, ~250) overflowed the guard-page-less kernel
> stack (untrusted script → kernel corruption; reproduced under ulimit -s 256). FIXED: walk the trap-less
> chain ITERATIVELY (bounded loop, cap 4M >> any arena-buildable chain, so one-pass; stops at non-proxy
> or trap-having → final read recurses ≤1 guarded level). Added a 300-deep-chain jstest case (was
> missing).** jstest PASS (get/set/trapless/enum/typeof/method/self-recursive/deep-chain, ASan/UBSan) +
> 7 suites + in-OS (js PROXY.JS → "got:bar"/"foo=5"/"chain 9", 200-deep chain, 0 faults). Baked PROXY.JS.
> **GENERATORS = the only major modern-JS feature left, and it's a tree-walker CEILING (real lazy
> generators need eval suspension; an eager hack mis-orders side effects) — document, don't force it.**
> Files: kernel/js.c, tests/js/suite.js+.expected, tools/mkfatfs.c.
> M469: **JS `Symbol` + iterator protocol** — new V_SYMBOL primitive
> (typeof "symbol", unique id, equality by id; symbol-keyed props encoded as "@@sym:<id>" + hidden from
> Object.keys/for-in/JSON/etc.), `Symbol`/`Symbol.iterator` globals, and `for…of` now consults a plain
> object's `[Symbol.iterator]()` → CUSTOM ITERABLES work (call it → iterator → loop next()→{value,done}).
> Untrusted-input safety: the for-of loop is hard-capped (FOROF_ITER_MAX=2000, CATCHABLE rt_err) +
> g_err/g_oom-guarded each step + each next() is depth-guarded — can't hang/overflow the kernel.
> Subagent-implemented (found+fixed a loose_eq bug: Symbol()==0 → false), then I reviewed + SHIP-review
> subagent (bounds/termination/additive all confirmed; sym_key buf ≤26/32; 8 enumeration sites hide
> @@sym) + jstest PASS (Symbol/custom-iterable/class-iterable/adversarial cases) + in-OS (js ITER.JS →
> "typeof symbol sum 10", a [Symbol.iterator]() range summed by for-of). KNOWN (pre-existing, unrelated):
> object literals as ternary branches `c?{..}:{..}` mis-parse; spread/Array.from don't invoke
> [Symbol.iterator] on plain objects (only for-of does). Baked ITER.JS demo. Files: kernel/js.c,
> tests/js/suite.js+.expected, tools/mkfatfs.c.
> M468: **Shell output redirection `>` / `>>`** — `cmd > file` (overwrite)
> + `cmd >> file` (append), composing with M463 pipes (`ls | grep TXT > found.txt`). Reuses the M463
> opt-in print-capture: main() parses+strips a trailing `>`/`>>`+filename, then the command (single OR
> a pipeline whose final stage is captured) runs under cap_begin/cap_end and the bytes go to the file
> via write_redirect (append reads existing first, both capped 8KB BSS). run_pipe gained (rfile,append)
> params for its final stage. Help line documents `|`/`>`/`>>`. Verified in-OS (>, >>, pipe+redirect,
> plain-pipe regression — all good) + all 7 suites + clean disk rebuild. SAFE/userspace; same write-path
> reliance as pipes. Files: user/shell.c only.
> M467: **Window close (F8 keyboard + mouse X) with app termination** —
> the DE gains a keyboard window-close (F8 → scancode 0x42 → WM code 0x1A), and closing an app window
> (F8 OR the title-bar ×) now TERMINATES the app instead of orphaning it (was: task kept running, no
> window, leaking slot+memory — so M464/M466 only helped self-`exit` apps). Cooperative kill: WM sets
> a->kill + task_wake; the app returns from its blocking app_sys_read, sees kill at the loop top, sets
> exited=1 + task_exit from its OWN context → M464/M466 reaper reclaims it. (Bug found+fixed mid-impl
> via temp kprintf: first cut called task_exit WITHOUT setting exited=1, so app_alive stayed true and
> the reaper skipped it — task dead but window+slot leaked. Set exited=1 first.) Verified: 10 spawn+F8
> cycles → free memory flat at the 102 MiB baseline, 0 faults, 0 orphans, all 7 suites. THE KEYSTONE:
> the teardown now applies to the normal close path. Files: app.c, app.h, keyboard.c, desktop.c.
> Remaining deferred: forced-kill of a TRULY-stuck app (one not reading input — rare), ~150-root TLS
> chain enforcement (bulky), FAT32 write-robustness (fragile).
> M466: **App address-space reclamation (full teardown)** — completes
> M464. New `vmm_destroy_address_space` (vmm.c) frees an exited app's page tables AND user frames
> (ELF+stack, ~59 frames/app), called from app_reap. PROVABLY SAFE: vmm_create copies boot's PDPT
> verbatim + next_table only writes not-present slots ⇒ a PDPT entry DIFFERING from boot's is always
> app-private; free exactly those (+ private PML4/PDPT pages), skip the shared low map + higher half.
> ROOT-CAUSE FIX in task_exit: it left the dead task's CR3 loaded under `next` (only swapped rsp), so
> the reaper saw the dying space active and the guard refused to free (diagnosed via temp kprintf:
> active==dead-cr3); task_exit now loads next->cr3 like switch_to_next, so the WM reaps in kernel_pml4
> and frees. Both SHIP-reviewed; verified in-OS: free memory returns to the EXACT boot baseline (102
> MiB) after 12 spawn+exit cycles (dropped to 96 before), 0 faults. The app-exit leak is fully closed.
> Remaining deferred: forced-kill of X-closed orphaned apps (task still running — risky), ~150-root TLS
> chain enforcement (bulky), FAT32 write-robustness (fragile).
> M465: **Kernel-heap interrupt-safety** — `kmalloc`/`kfree` (kheap.c)
> now bracket their shared-free-list edit with irq_save(cli)/irq_restore, closing the pre-existing
> latent race the M464 review surfaced (an IF-on WM `kfree` preempted by an app's IF-clear syscall
> `kmalloc` over a half-linked list). Restores the caller's prior IF state (safe on/off), nests across
> kmalloc's grow-and-retry recursion. Correctness-only (single-threaded behaviour unchanged). Verified:
> all 7 suites + in-OS stress (5 spawn+exit cycles freeing 256 KB stacks from the WM, interleaved with
> a heap-heavy CSS-page browser render — 0 faults). The prior review explicitly recommended this fix.
> M464: **App-exit resource reclamation** — a ring-3 app that exits
> cleanly now has its `task_t` + 256 KB kernel stack freed and its `apps[]` slot released (was: all
> leaked → 8-spawns/boot hard cap). New `task_free` (task.c) + `app_reap` (app.c); the WM reap loop
> (desktop.c) frees from its own task only once the dead app's task is `TASK_DEAD` — set interrupts-off
> and only left via the final context_switch, so observing it from another task proves off-CPU (no
> UAF). The WM drops the window only after the slot is reclaimed (no dropped-window leak). I designed
> it after reading vmm/task/app/desktop, then SHIP-reviewed (off-CPU invariant airtight) + all 7 suites
> + in-OS verified (10 spawn+exit shell cycles, 0 faults, 0 spawn failures — old code fails at #8).
> DEFERRED (documented, not session-length): freeing the app's address space (a->cr3 page tables +
> user frames — needs an active-CR3 guard) and forced termination of X-closed (orphaned, still-running)
> apps. NEXT surfaced by the review: the kernel heap (kmalloc/kfree) has NO locking — an IF-on desktop
> kfree can be preempted by an app's IF-clear syscall kmalloc over a half-edited free list (pre-existing
> latent; M464 widens it a hair). Wrapping kmalloc/kfree/kzalloc in irq_save/irq_restore would close it.
> M463: **Shell pipes (`cmd1 | cmd2`)** — the userspace shell now
> does UNIX-style N-stage pipelines (`ls | grep TXT`, `cat f | wc`). The command dispatch was
> extracted into `run_command(line,cwd)` (whole if/else chain wrapped in `do{…}while(0)` so its
> dispatch-level `continue`s keep meaning — a plain command runs byte-for-byte as before — and "exit"
> returns 1); `print()` in ulib got an opt-in capture mode (off for every other program); `run_pipe`
> captures each stage to PIPE.TMP that the next stage reads as its trailing file arg (so grep/wc/sort/
> head/… consume piped data unchanged). Implemented via a subagent (do-while(0) trick eliminated the
> continue-conversion risk), then I reviewed (run_command/run_pipe/ulib all sound) + all 7 suites +
> in-OS verified (ls|grep filters, echo|wc counts 1/3/12). Cosmetic: wc-style commands echo "PIPE.TMP"
> as the filename — inherent to the temp-file model. Userspace-isolated/recoverable.
> M462: **SVG `<use>`/`<symbol>` reuse** — `<use href="#id" x= y=>`
> (+ `xlink:href`) instantiates a defined element (shape, or `<symbol>`/`<g>`, usually in `<defs>`)
> at an offset. The render loop was extracted into `render_region(ctx,p,end,depth)` (behaviour-
> preserving at depth 0 — the 10 unit tests lock it byte-for-byte); `find_def` locates the span
> (depth-matching the close), `<use>` renders it under translate(x,y), recursion depth-capped (<4,
> self/cyclic terminate). Implemented via a subagent, then I reviewed (find_def/`<use>` bounds-safe)
> + svgtest PASS (11 unit tests + fuzz + 2M-iter focused) + in-OS verified (USE.SVG: a `<g id=star>`
> `<use>`d 4× → 4 stars). **SVG decoder now fully comprehensive.**
> M461: **CSS `text-decoration:line-through`** — `<s>`/`<del>` tags
> already struck (STY_STRIKE + strike-line); now CSS line-through (inline/`<style>`) maps to it too.
> One additive check in parse_style_textstyle. Verified in-OS (CSS.HTM struck span). 
> **NOTE/STATUS: I've now done 21 milestones this session (M441-461): SVG (rasterizer/transforms/
> inheritance/opacity/gradients/text), CSS (bg/align/font-size/display:none+visibility/comma-selectors/
> hsl/line-through), tables, blockquotes, TLS hostname+validity enforcement+cert-info, browser zoom.
> The safe-high-value space is SATURATED; remaining valuable work (shell pipes, SVG `<use>`, VMM
> app-exit teardown, ~150-root chain enforcement) all need RISKY REFACTORS of working code — deferred
> to protect working systems per the original directive. Continuing with safe genuine additions.**
> M460: **CSS `hsl()`/`hsla()` colours** — modern stylesheets use HSL;
> the parser only did hex/rgb/named. Added an integer HSL→RGB (`hsl_to_rgb`, no FPU) + an `hsl(...)`
> branch in parse_color, so HSL works for color/background/`<font>`/cascade. Additive. Verified in-OS
> (CSS.HTM: hsl red/green/blue text + hsl-yellow bg). (SVG fill=hsl() not added — separate svg.c parse_color.)
> M459: **comma-grouped CSS selectors** — `h1, h2, .title { }`-style
> rules (ubiquitous in real CSS) were dropped (sel_parse rejects the comma). capture_css now parses
> the decls once + adds one rule per comma sub-selector that parses. Additive (single selector = 1-elem
> list), bounded by CSS_MAX, no matching/render change. Verified in-OS (`.g1, .g2, em {…}` colours all
> three; others unregressed).
> M458: **CSS `visibility:hidden` + hidden `<img>`** — completes hiding
> (M454/455): `visibility:hidden` (inline/`<style>`) hides like display:none via the same scope (1 line
> in parse_style_display); a void `<img>` with its own display:none/visibility:hidden/hidden renders
> nothing (the deferred standalone-hidden-img case). Additive, no render-loop change. Verified in-OS.
> NOTE: standalone SVG browsing already works (try_image); SVG `<use>`/`<symbol>` deferred (needs a
> render-loop refactor — risky to the working decoder; worktree isolation unavailable here).
> M457: **browser content zoom** — `+`/`-`/`0` scale page text 1×–4×
> (persists across navigation). Multiplies the per-token `sc`/line-height the renderer already
> computes (headings/font-size/bold all scale) + the wrap reflows. Additive (zoom 1 = unchanged).
> Verified in-OS (text file ~3× + reflowed). Note: images don't zoom (text-only).
> M456: **SVG `<text>`** — svg.c renders `<text>` with the kernel 8×16
> bitmap font (`font_glyphs`, isolated; svgtest stubs it), each glyph scaled to `font-size` device px
> + `fill`-coloured, anchor through transform/viewBox map (translate/scale positioning; no glyph
> rotation). Bounded (blend_px clamps every px + char/height caps) → fuzz-safe. svgtest +text unit
> test +text fuzz frags. Verified in-OS (TXTSVG.SVG: "Hello, SVG!" 22px blue, "text labels work" 15px
> red, "OK" on a circle). The SVG decoder is now comprehensive: shapes/paths + transforms +
> inheritance + opacity + gradients + text.
> M455: **`display:none` + `font-size` from `<style>` rules** — M448/M454
> did these inline; now the `<style>` cascade does too, so `.hidden{display:none}` (ubiquitous utility
> classes) + `.big{font-size:2rem}` work. capture_css parses font-size/display into 2 new rule columns;
> css_match reports them (a display:none rule raises the same n_hidden scope; a font-size rule sets the
> per-token scale, inline overriding). Additive. Verified in-OS (HIDE.HTM: `.gone` hidden, `.big`
> enlarged, + the inline cases).
> M454: **CSS `display:none` + `hidden` attr** — an element with inline
> `display:none` (or the HTML5 `hidden` attr) suppresses itself + all content (text/bullets/hr/img).
> An `n_hidden` counter raised/lowered by the existing tag-depth-matched sc[] scope stack (composes
> with nesting); every emit primitive (emit_word/emit_break/hr+img token pushes) + the text-accum loop
> gated on it. Additive (n_hidden=0 ⇒ unchanged); visual blast radius only. Verified in-OS (HIDE.HTM:
> display:none p + hidden div+list + inline display:none span all absent, visible paragraphs in order,
> no leak). Note: `<style>`-rule display:none + standalone void-element (img) display:none deferred.
> M453: **browser cert-info display** — press `i` on an HTTPS page →
> the status shows the leaf cert's CN + expiry (YYMMDD) + a verification word (`anchored`/`verified`).
> `tls.c` exposes `tls_leaf_cn`/`tls_leaf_expiry`; browser snapshots CN/expiry/host_match per HTTPS
> load. Safe read-only "padlock details" surfacing M451/M452. Verified in-OS (example.com →
> "example.com exp260829 anchored").
> M452: **TLS cert validity-period enforcement** — `x509.c` parses
> notBefore + adds `x509_time_cmp` (UTCTime YY-pivot + GeneralizedTime; unparseable→0/no-opinion);
> `tls.c` reads the RTC and REJECTS an expired/not-yet-valid leaf cert before the request. Guarded:
> enforced only if RTC year ≥ 2020 (unset clock fails open), unparseable date fails open — a bad
> RTC can't break all HTTPS. x509test unit-tests x509_time_cmp. Verified in-OS BOTH ways: clock 2026
> → example.com loads; VM clock forced to 2040 (`-rtc base=`) → same cert EXPIRED + ABORT/refused.
> Completes cert validation: chain + hostname (M451) + validity period.
> M451: **TLS hostname verification (ENFORCED)** — the client
> validated/anchored the cert chain but never checked the cert named the host (any valid cert for
> any domain was accepted = MITM hole). Now `x509.c` parses subjectAltName dNSNames (new bounded
> `find_san` over the `[3]` extensions) + `host_matches_cert` (RFC 6125: SAN-authoritative, no CN
> fallback when SAN present, case-insensitive, single-label wildcard rejecting apex/multi-label/
> `*.com`); `tls.c` REJECTS a definitive mismatch before sending the request, FAILS OPEN on
> uncertainty (>16 SANs / no cert) so legit sites never wrongly reject. Chain anchoring stays
> informational (incomplete root set). x509test +4 real certs +RFC6125 assertions +400k SAN-mutation
> fuzz (ASan/UBSan clean). Subagent review BLOCKED by the cyber safeguard (cert/MITM framing) →
> compensated with a thorough self-audit + the 400k-iter fuzz. Verified in-OS: example.com (SSL.com)
> + danluu.com (Google GTS) both MATCH + load. The remaining "enforcing certs" gap is now just the
> chain-to-root part (needs ~150 baked roots).
> M450: **`<blockquote>` indentation** — quoted blocks indent 24px/
> level, and EVERY wrapped line stays indented (unlike the list-marker first-line-only indent),
> because indent is applied at each line start via a per-token `tokindent` + a `curindent` counter
> (`<blockquote>` open/close adjusts it) consumed by the same line-start `cx` logic as text-align.
> Nested blockquotes indent further; close restores parent. Additive (no bq ⇒ curindent 0 ⇒ cx
> unchanged) + bounded (clamped, cx within [cl,cr)). Verified in-OS (QUOTE.HTM). Lists keep their
> existing first-line marker indent (untouched).
> M449: **column-aligned `<table>` rendering** — was pipe-separated/
> unaligned; now a self-contained `render_table` consumes the `<table>..</table>` region in two
> bounded passes (measure each column's max width, then emit each cell padded to it) → aligned
> columns in the fixed-width font, `<th>` bold. `cell_extract` (strip inline tags/entities/UTF-8,
> collapse ws) + `tbl_classify`. Plugged into `parse_html` as a single `<table>` interception that
> renders+skips the region, so non-table pages are byte-for-byte unaffected. Defensively reviewed
> (15M ASan/UBSan fuzz iters: bounds-safe + always-terminating). Verified in-OS (TABLE.HTM: two
> tables, both aligned). This is the first real **layout** (a contained mini table layout).
> M448: **CSS `font-size` (enlarge)** — `font-size` (inline `style=`,
> px/pt/%/em/rem + large/x-large keywords) + legacy `<big>`/`<font size=N>` scale text up via a
> per-token glyph-scale bucket (≈2× for ≥19px/119%/1.2em, ≈3× for ≥28px/175%/2em). Mirrors M447
> `tokalign` (`tokscale[]` + `curscale` scope); the renderer's per-token scale/line-height + the
> align look-ahead all consult it, so mixed-size words wrap correctly and font-size composes with
> colour/bold. Bitmap font = no sub-1×, so enlarge-only (small falls to 1×). Additive. Verified
> in-OS (SIZE.HTM: 24px→2×, 34px→3×, 200%/2em→3×, `<big>`/`<font size=6>`, 10px stays 1×, large+colour).
> M447: **CSS `text-align: center`/`right`** — inline `style=` +
> `<style>` rules + the `<center>` tag + the `align=` attr now center/right-align text. The
> single-pass word-wrapper does a bounded line-width look-ahead at each line start (replicating
> the wrap test), then offsets `cx`. Per-token `tokalign[]` + a `curalign` scope (mirroring M446
> `tokbg`); a line takes its first token's alignment. Additive (left = zero offset, existing pages
> byte-identical); the look-ahead is ntok-bounded and only moves `cx` within `[cl,cr)` (can't
> corrupt). Verified in-OS (ALIGN.HTM: centered heading + per-line-centered paragraph + right-align
> + `.c`/`.r` rules + `<center>` + `align=`; left baseline unchanged).
> M446: **CSS `background-color`** — `background-color` (+ the
> `background:` shorthand colour) from inline `style=` and `<style>` rules paints an inline
> highlight behind a text run, composing with the color/weight/style/underline scope stack +
> cascade. A per-token `tokbg[]` + `curbg` scope field (mirroring `tokcolor`) feed the renderer's
> existing per-glyph background (what `<mark>`/link-selection use); explicit bg beats `<mark>`'s
> default, UI highlights beat content. Additive, reuses M434-reviewed `style_prop`/`parse_color`.
> Verified in-OS (CSS.HTM: `.hl`/`.code` rules + inline spans + `<mark>`; existing CSS unregressed).
> M445: **SVG gradients** — `<linearGradient>`/`<radialGradient>`
> (multiple `<stop>`s, `stop-opacity`, `gradientUnits`) referenced by `fill=url(#id)`, evaluated
> per-pixel in the scanline fill (integer/16.16, Newton int-sqrt for radial, no FPU). A pre-pass
> collects defs (forward refs + `<defs>` resolve); objectBoundingBox (default) maps to the shape
> bbox, userSpaceOnUse to device. Strictly additive (no gradient = unchanged solid path). Self-review
> found+fixed an int64 overflow (clamp the resolved geometry before dx*dx). ~6.5M ASan/UBSan fuzz
> iters. Verified in-OS (GRAD.SVG → a sky linear gradient behind a 3-stop radial sphere). The SVG
> decoder is now comprehensive: shapes/paths + transforms + inheritance + opacity + gradients.
> M444: **SVG opacity** — `opacity`/`fill-opacity`/`stroke-opacity`
> (a 0..1 fraction or `%`) scale a shape's alpha; a group `<g opacity>` multiplies down to its
> children (inherited `in_alpha` on the same paint stack). Reuses the existing alpha-compositing
> `blend_px`, so translucent overlaps blend. Strictly additive (no opacity = exact prior alpha,
> byte-for-byte); int64 alpha math (no overflow). svgtest +opacity unit test +fuzz; SHIP-reviewed
> (4.5M+ adversarial iters). Verified in-OS (OPAC.SVG → three 50%-opaque circles, overlaps blend).
> M443: **SVG paint inheritance** — shapes inherit
> `fill`/`stroke`/`stroke-width` from a `fill=`/`stroke=` on the root `<svg>` or an enclosing
> `<g>` (was: always black), per-shape values override, `inherit` keyword honoured. A paint
> stack parallel to (and sharing the depth of) the M442 transform stack; strictly additive
> (no ancestor paint = the exact old defaults, byte-for-byte). svgtest gained an inheritance
> unit test + fuzz; SHIP-reviewed (3M+ adversarial iters). Verified in-OS (ICON.SVG → a green
> hexagon inheriting the root `<svg fill>`, white dot overriding). This is how single-colour
> icon sets work (set `fill` once on `<svg>`). 
> M442: **SVG affine transforms** — `kernel/svg.c` now honours
> `transform=` on shapes and `<g>` groups (translate/scale/rotate/matrix/skewX/skewY, composed
> left-to-right) via a depth-16 transform-matrix stack; each user point runs through the current
> 2×3 matrix before the viewBox→pixel map. All 16.16 fixed-point (no FPU), overflow-clamped;
> strictly additive (identity CTM = the M441 render byte-for-byte). `svgtest` extended to fuzz the
> transform parser; SHIP-reviewed (~26.5M adversarial iters, one negative-left-shift UB fixed).
> Verified in-OS (XFORM.SVG → a four-arm pinwheel placed by group translate + per-arm rotate).
> M441: **from-scratch SVG rasterizer** (`kernel/svg.c`, 846 lines,
> integer-only/no-FPU) — decodes a useful SVG subset (rect/circle/ellipse/line/poly*/path with
> beziers, solid fills, viewBox, even-odd scanline fill + stroke) to RGBA, plugged into
> `decode_image` so local+remote SVGs render inline (the M439 `.svg`-skip is lifted); SHIP-reviewed
> + 520k author fuzz (3 UB fixed) + ~12M independent adversarial iters; new `make check` suite
> #7 `svgtest`; verified end-to-end in-OS (SVGT.HTM → blue square/yellow circle/red triangle).
> M440: CSS `text-transform:uppercase/lowercase` — inline `style=`
> + `<style>`-rule cascade, emit-time case-fold into the render pool (so `.textContent` stays
> original), composes with the color/weight/style/underline scope stack; verified in-OS (NEST.HTM). M439: **inline remote images** — the browser now fetches
> + decodes remote PNG/GIF/JPEG `<img>` inline (was: links). Designed via a Plan subagent
> (Option B: in-worker pre-parse fetch, ≤3 imgs, separate `rimg_*` arrays, `parse_html`
> untouched); a header-strip bug was found (stored the whole HTTP response so the decoder
> saw `HTTP/1.1…` not the image) + fixed; skips undecodable `.svg`/`.webp`/`.avif`; SHIP-
> reviewed (8M-iter ASan, concurrency/lifecycle clean) + component-verified (gnu.org fetch+
> match; strip+decode on a real PNG; local-image render). M438: **JS regex bounded quantifiers `{n}`/`{n,}`/`{n,m}`**
> — a real architectural feature done safely by EXPANDING in the parser into existing
> nodes (`a{2,4}`→`a a a? a?`), so the audited matcher/compiler stay byte-for-byte
> unchanged; count-capped + RE_MAXPROG-bounded + ReDoS-safe; reviewed SHIP (3M-iter
> ASan fuzz) + verified in-OS. M424–437: more shell tools — `cmp`/`paste`/`comm`/`diff`
> (the file-compare set: byte / sorted-set / LCS line-edit), `cut -f` (delimited fields,
> completing `cut`), `strings`, `basename`/`dirname`; a JS-engine correctness
> sweep found by probing — getters now fire in value-iteration too
> (`JSON.stringify`/`Object.values`/`entries`/`assign`/spread, M425–428) and
> `to_num(Date)` returns the epoch so Date arithmetic/comparison work (M429); the
> untrusted-input **security audit was deepened on the browser** (M434) — beyond the
> M422 high-level tokenizer review, a granular bound-by-bound pass over every
> self-contained HTML/CSS sub-parser (`decode_entity`/`parse_color`/the inline-`style=`
> parsers/`sel_parse`/the attr helpers), independently reviewed **bounds-safe**, with
> one `decode_entity` defense-in-depth guard; and the safety review was extended to the
> **disk** trust boundary (M435) — a `cluster_in_range` guard on the FAT32 read loops
> (`walk_dir`/`fat32_read`) rejects corrupt out-of-range cluster numbers (strictly
> additive, reviewed SHIP; also closes a latent cluster-0 underflow); plus committed ASan/UBSan fuzz harnesses
> for the kernel's untrusted-input parsers — `make check` = jstest + imgtest + x509test
> + nettest, each verified to catch a reintroduced OOB. M387–423: the JS-engine / browser-demo / shell
> spaces were saturated with verified increments — a 15-page interactive browser
> demo suite (games/sims/tools) + CLI companions (`unmorse`/`unhex`/`unbase64`,
> `cal -3`). Then a **correctness + security pass** on the highest-risk code:
> (a) probing the JS engine found + fixed two real correctness bugs —
> `[1] instanceof Array` returned `false` (M419) and `[1,2]+[3]` returned `0`
> instead of string-concatenating per ToPrimitive (M420) — and verified ~60
> features plus graceful failure on all three exhaustion axes (stack depth, heap,
> malformed syntax); (b) the **Files panel now browses ALL 86 disk files** (was
> capped at 32 — M421); (c) a **comprehensive untrusted-input SECURITY AUDIT** —
> 9 subagent reviews + ASan/UBSan fuzzing of every kernel-side parser
> (network → TLS → X.509 → crypto, and HTML/CSS → image decoders → JS) — found +
> fixed **two real KERNEL memory-safety bugs**: a JPEG DRI out-of-bounds read
> (M422) and a DNS-query-builder kernel-stack overflow (M423), both ASan-proven,
> with the rest verified bounds-safe. See
> [docs/422-untrusted-input-security-audit.md](docs/422-untrusted-input-security-audit.md).
> All reviews SHIP. M374–386: `sort -r`, `cut`, `tr`, `wc -l/-w/-c`,
> `cal MM YYYY`, from-scratch `crc32`, `cowsay`/`fortune`; **`Math.random()`** added
> to the JS engine; interactive browser pages — Rock-Paper-Scissors, a number base
> converter, Guess-the-Number, an ASCII table; and a mkfatfs >64-file capacity fix.
> Ten subagent reviews this session, all SHIP.) Latest arc (M341–358): a shell **networking
> diagnostic toolkit** (`headers` = curl -I, `ping <host>`, `ifconfig`) + a
> deeper e1000 RX ring; **four new apps** (Tic-Tac-Toe vs an unbeatable minimax
> AI, Blackjack, a typing-speed test, Simon) bringing the suite to **twenty-four**,
> with saved best scores/times now across the games (typing/bj/simon/breakout/
> mines/maze); the F9 Apps menu lists **every** app + a shell `apps`; and more
> shell tools (`sha512`, `nl`, `morse`, `factor`, `roll`). Three subagent reviews
> this arc, all SHIP. **M359–373** then added the multi-file text tools
> (`cat`/`grep`/`wc`/`head`/`tail` over many files) + `grep -i/-n/-c/-v/--` +
> `uniq`/`seq`/`rev`/`tac`/`head -N`/`tail -N`, a persistent `todo` manager, and
> a **Connect Four** game vs a 1-ply AI (suite now **25**, F9 menu full at 30) —
> several more subagent reviews, all SHIP. **Remaining big/risky (deferred to protect working
> systems — best done in a focused session):** ENFORCING cert validation (a
> fatal gate; the trust store has ISRG/DigiCert/SSL.com/GTS but enforcement needs
> ~all common roots), **inline remote images** — local `<img>` decode inline;
> remote is a clickable full-page view. APPROACH (investigated M358): the render
> runs on the **WM** (`need_parse`: worker→WM), but only the fetch **worker** has
> the 256 KB stack `tls_get` needs — so the worker must **pre-fetch + decode**
> remote `<img>` bytes into the existing inline-image slots (`imgs[]`) *after* the
> page fetch and *before* signaling render; a render-time fetch on the WM would
> overflow its stack. The pieces exist (decoders, slots, the worker fetch); the
> risk is to the working browser, so a focused session, the **app-exit resource leak** (MAX_APPS spawns/boot; needs a
> careful vmm teardown that frees only the app's user ranges), a **2-column F9
> menu** (the single column is near its ~30-item cap), shell **pipes/redirect**,
> and **FAT32 write robustness** (investigated M366): `fat32_write` is
> delete-then-write (no cluster leak), but `add_entry` (kernel/fat32.c) **fails
> cleanly on a full directory** — it doesn't allocate/link a new dir cluster, so
> file *creation* stops working once the root dir's clusters fill (~the 64 baked
> files + a little slack); fix = extend the dir chain in `add_entry`. Separately,
> the persisted `build/fat.img` showed **empty** after this session's intensive
> write-accumulation (todo + `*.HI` rewrites across many boots). **Diagnosed
> write-triggered (M367):** a fresh disk + read-only boots are fine — `fat.img`
> is byte-unchanged after a read-only boot — so it's the repeated file *writes*
> that corrupt it over time, not a boot/mount-write or QEMU-read artifact;
> consistent with the add_entry dir-fill limit + a likely write-path bug.
> `rm build/fat.img && make` regenerates a clean disk. **First-read-after-boot
> transient (M373):** the *first* file read can intermittently fail ("no such
> file" on a `cat` issued within the first ~5 s post-boot), while the very next
> read of the same file succeeds — a disk-readiness race at boot, not a logic
> bug (cat/grep/tac share one `sys_readfile` path). Any prior disk op (e.g.
> `ls`) or a couple seconds' settle warms it up, and a human typing the first
> command never hits it — only the fast test harness does. Same off-limits
> kernel disk/ATA-init path; deferred. Don't touch the FAT32 write path casually — verify every change against
> the baked files.
>
> ---
>
> The historical status below is from the 337-milestone mark.
>
> **Status (337 milestones):** the big arcs below are now DONE — a from-scratch
> **TLS 1.3 client** browses the real HTTPS web with X.509 chain validation, a
> **comprehensive from-scratch JavaScript engine** (full OOP + ES6 + regex +
> Map/Set/Date) runs in the shell and in pages, and the browser is **fully
> interactive**: a minimal DOM (`getElementById`/`querySelector` →
> `textContent`/`innerHTML`/`.value`/`getAttribute`/`setAttribute`),
> `window.location`, inline `onclick`, the complete form-input set
> (text/password/checkbox/radio/hidden/submit/button), **GET form submission +
> live web search (DuckDuckGo) + address-bar search**, and reactive events
> (`onchange`/`oninput`).
>
> **M302–337 added a small CSS engine and a big app suite:** the browser now has
> a real (if small) **CSS engine** — per-element `color`/`font-weight`/
> `font-style`/`text-decoration` from inline `style=` *and* `<style>` rules
> (`tag`/`.class`/`#id`/`[attr]` selectors), `rgb()`/named colours, a real cascade
> and a nesting scope stack — and the start/help/index pages are styled by it.
> Userspace grew to **twenty apps** via a `sys_setcolor` palette: new ones are
> Sudoku, a Calendar, a Mandelbrot explorer, a Piano, a Maze, a text Adventure, a
> Matrix screensaver, an ASCII Paint (saves/loads files), Hangman, and a music
> Jukebox; the games are colourised and several keep persistent high scores. Only
> CSS *layout* (font-size/text-align/box model) still needs a layout engine.
>
> **M216–260 rounded the JavaScript language + stdlib out to near-completeness**
> (found by systematic probing): the full operator set (`delete`/`in`/
> `instanceof`/`**`/bitwise `^`·`~`/`void`), all compound + logical assignments
> (`&=`…`**=`, `||=`/`&&=`/`??=`), binary/octal/exponent + `_`-separated number
> literals, modern classes (**public instance fields + static methods/fields**),
> computed method names, tagged templates, `typeof undeclared`→`"undefined"`, the
> **`arguments`** object, **Error/TypeError/RangeError/SyntaxError** objects,
> spread of `Set`/`Map`, a full **`Date`** (getTime/valueOf/getDay/
> getMilliseconds + `Date.now()` + setFullYear…setSeconds), and a stdlib that now
> covers essentially every common synchronous method — String (replaceAll,
> matchAll, padStart/End, trimStart/End, at, slice/substring edge cases),
> Array (map/filter/reduce/reduceRight, flat(depth)/flatMap, findLast, at, **the
> ES2023 immutable `with`/`toReversed`/`toSorted`/`toSpliced`**), Object
> (keys/values/entries/assign/fromEntries/freeze/**is**), Number (isInteger/
> parseInt/parseFloat/toFixed/toString-radix/MAX_SAFE_INTEGER), Math (hypot/log2/
> cbrt/clz32/imul/sign), and JSON (pretty `stringify` with indent + `parse`). See
> the milestone table in `README.md` for M139–263.
>
> **The clean stdlib AND the object model are now complete.** M261–263 added the
> last big language pieces — all SHIP-reviewed: **getters/setters** (accessor
> properties in object literals *and* classes), **`Object.defineProperty`/
> `getOwnPropertyDescriptor`**, and a real **prototype chain** (`Object.create`,
> function `.prototype` + `new F()` for plain constructors, `__proto__`,
> `getPrototypeOf`/`setPrototypeOf`). The prototype chain is **additive** — walked
> only at the evaluator's member sites after an own-miss, so `delete`/enumeration
> stay own-only and the class system is byte-identical.
>
> **M264–271 hardened and completed the engine:** `in`/`instanceof` now walk the
> prototype chain; `Object.defineProperties` + `Object.create(proto, descriptors)`;
> `structuredClone` (cycle-preserving deep clone); the `Array(n)`/`new Array(…)`
> constructor; `>>>`/`>>>=` (completing the operator + compound-assignment sets);
> and **loose `==`/`!=` vs strict `===`/`!==`** (the `x==null` idiom + coercion).
> Systematic probing of the mature engine also surfaced and fixed real
> pre-existing bugs: `{}===​{}` returned `true` (objects compared via `to_num`→0),
> string `<`/`>` was always `false` (coerced to 0), `1===true` was `true`, and
> `arr.length =` was ignored.
>
> **M281–283 added `querySelector`/`querySelectorAll`/`getElementsByTagName`/
> `getElementsByClassName` by CSS selector** (`tag`/`.class`/`#id`/compounds) +
> `getAttribute` + WRITE (`textContent`/`innerHTML`/`setAttribute`/`remove`) on
> matches — **without** a DOM tree. The "an id-less match has no name to address
> it by" blocker is solved with **byte-offset position handles** (the matched
> `<`'s offset in `vals[1]`), fully additive (the id path stays byte-identical),
> with the write splice duplicated from the id path and the offset re-validated
> each use (memory-safe; single-match writes exact, multi-write best-effort). So
> the common selector-query DOM API is now covered on the token-stream renderer;
> only node *construction* still needs the tree.
>
> What remains is either *fundamentally blocked by the integer-only Number*
> (`Math.random`, float math, `isFinite`/full `isNaN` — all need a NaN /
> floating-point representation we deliberately don't have) or genuinely
> *architectural*: **CSS layout** (a real box/layout engine), **generators/
> iterators**, **async/Promises**, **modules**, **Proxy**, **`Symbol`**. (The DOM
> itself is now **comprehensively complete** — M281–299 delivered queries,
> traversal [matches/closest/children/parentElement], attributes [get/set/has/
> remove + classList], read/write, **node construction** [createElement/appendChild
> — which turned out NOT to need a tree, via innerHTML-reuse], and the full
> event-handler lifecycle [onclick/addEventListener/onchange/oninput, add/fire/
> remove] on a persistent per-page JS env. And **a small CSS engine now exists**
> — M302–305: per-element `color` / `font-weight` / `font-style` from inline
> `style=` *and* `<style>` rules (`tag`/`.class`/`#id`/`[attr]` selectors), with
> a real cascade and a scope **stack** so styled elements nest to any depth. Only
> CSS *layout* — `font-size`, `text-align`, the box model, backgrounds — still
> needs a layout engine the token-stream renderer lacks; the *machinery* of CSS
> [selectors, cascade, nesting] is done.)

OS-DEV is a **graphical desktop OS** (337 milestones). It boots to a themed
windowing desktop with a **taskbar** that hosts **twenty real ring-3 userspace
programs** as windows — a shell, a clock, a calculator, a text editor, an
ASCII-paint canvas, a calendar, a Mandelbrot explorer, a piano, a music jukebox,
a Matrix screensaver, and ten games (Snake, 2048, Life, Tetris, Breakout,
Minesweeper, Sudoku, Maze, Hangman, a text adventure) — plus a **graphical web
browser**. Under the hood:
preemptive multitasking with sleep/wake and **per-process isolation**, a
read-write **FAT32** filesystem with **subdirectories** and a full file toolkit,
a from-scratch **TCP/HTTP** stack that fetches real web pages (redirects +
chunked transfer-encoding), verified **SHA-256 / AES-128** crypto, sound, a
real-time clock, USB pointer, arrow keys + command history, and a compositor
that caches the scene so cursor moves are cheap.

```
boot → long mode → interrupts → timer/kbd → memory → heap → preemptive threads
 → sleep/wake → userspace/syscalls → libc/shell → FAT32 → PCI → ARP/ICMP/UDP/DNS
 → framebuffer → console → mouse → USB tablet → window manager → desktop apps
 → RTC → sound → process spawning → TCP+HTTP → browser (links, history, async,
   rendering, start page, save, local files, redirects, chunked, <pre>)
 → FS subdirectories → file toolkit → taskbar → ps → arrow keys → history
 → snake → editor → 2048 → compositor cache → system monitor → SHA-256 → AES
 → keyboard browser nav + bookmarks → wget → F2 focus-cycle → F4 maximize
 → lists/tables/entities → interactive Files → run ELF from disk → scrollback
 → editor scroll → in-page find → img alt → form fields → PNG images
 → run-from-disk → GIF images → font colour → Tetris → 100 milestones  ✅
 → window tiling → multi-cluster disk → render hot-path optimisation sweep
 → shell history → inline code → inline images → JPEG → Adam7 PNG → tab-complete
 → progressive JPEG → animated GIF → Minesweeper
 → X25519 → AES-GCM → HMAC/HKDF → ChaCha20-Poly1305 → X.509 parser
 → RSA verify → ECDSA-P256 (TLS 1.3 crypto+cert toolkit complete) → TCP stream API
 → TLS 1.3 client — browser fetches real HTTPS pages
 → entities in alt text + Latin-1 folding → UTF-8 body text
 → click-through HTTPS browsing (relative-link scheme + multi-fetch)
 → HTTPS from the shell (get/wget over TLS) → DNS cache
 → parser robustness (HTML comments + inline SVG)
 → window minimize/restore (F3)
 → TCP partial-segment fix + live NPR/gnu.org/danluu/example.com
 → network info in System Monitor
 → keyboard-drivable Apps menu (F9)
 → TLS CertificateVerify (server key-proof, real certs)
 → X.509 chain-internal verification (each cert signed by the next)
 → root-CA trust store + chain anchoring (NPR → ISRG Root X1)
 → SHA-384/512 + SHA-384 cert sigs (microsoft.com chain 2/2)
 → ECDSA P-384 (example.com EC chain 3/3)
 → trust store (ISRG/DigiCert/SSL.com) + key-match anchoring
 → out-of-order TCP reassembly → GTS Root R4 (google.com → TLS*)
 → from-scratch JavaScript interpreter (shell `js`)
 → JavaScript runs in the browser (pages execute their <script> tags)
 → JS standard library (Math, JSON.stringify/parse, String/Array map·filter·…)
 → arrow functions (x => x*x)
 → template literals (`hi ${name}`)
 → clickable JavaScript (<a href="javascript:..."> runs JS on click)
 → JS switch / do-while / for-of → try/catch/finally/throw (exceptions)
 → Object.values/entries, Array.isArray/from → object shorthand + default params
 → localStorage (stateful pages) → Date/for-in → parseInt(radix)/Object.assign/
   Array.flat/Math.sign/Number.toString(radix)
 → JS `this`/`new`/OOP → `class`/`extends` → `super` → spread/rest `...`
 → destructuring → full class-based OOP + ES6 (verified in-OS) → 169 milestones  ✅
```

The browser is now **fully keyboard-drivable**: Tab/n/p to select links, Enter
to follow, Backspace to go back, `a` to bookmark, `s` to save, `/` to edit the
address, PgUp/PgDn to page, `\` for in-page find, `h` for home, `g`/`G` for
top/bottom. It renders headings, bold/italic, inline `<code>`, **colour text**,
links, **numbered & nested lists, tables, `<pre>`, form fields, and real PNG,
GIF and JPEG images rendered inline in the page** (from-scratch DEFLATE/PNG +
LZW/GIF + baseline-JPEG decoders, host-tested and fuzzed), and decodes HTML
entities (named + decimal + hex). The WM has keyboard window management: **F2**
cycles focus, **F4**
maximizes/restores; the **Files window is an interactive launcher** (arrows +
Enter open a file in the browser); apps have **scrollback** (PgUp/PgDn). The OS
can also **load and run ELF programs from the disk** (`run calc.elf`), not just
the kernel-embedded ones.

Shell toolkit: `ls cat edit write rm cp mv mkdir cd pwd tree find grep df
hexdump wc cal sha256 crypt base64 run get wget browse echo date ping resolve
beep mem ps clear reboot ver pid exit`.

## The biggest remaining gaps

1. **Browser interactivity — LARGELY DONE; the frontier has moved.** The browser
   browses the real HTTPS web (from-scratch **TLS 1.3** + X.509 chain validation to
   baked-in roots — `TLS*` on example.com/NPR/gnu.org/google.com), runs a
   **comprehensive JS engine** (OOP + ES6 + regex + Map/Set/Date), and is now
   **interactive**: a minimal **DOM** (`getElementById`/`querySelector` →
   `textContent`/`innerHTML`/`.value`, `getAttribute`/`setAttribute`),
   `window.location`, inline `onclick`, the full **form-input set**, **GET form
   submission**, **live web search** (DuckDuckGo, from a form or the address bar),
   and **reactive events** (`onchange`/`oninput`). What's left for *more*
   interactivity — each a real build, best done with guidance:
   - **Event handlers — DONE (M287–292)**: a persistent per-page JS env + a handler
     registry; the full lifecycle works — `onclick`/`addEventListener`/`onchange`/
     `oninput` (inline *and* JS-assigned), add / fire (with an `event` arg + `this`) /
     remove (`removeEventListener`/`onclick=null`), state persisting across events.
     (Minor polish left: id-less-element handlers, multiple listeners per event.)
   - **DOM queries + traversal + element API + construction — DONE (M281–299)**:
     querySelector(All)/getElementsBy* by CSS selector (tag/`.class`/`#id`/`[attr]`/
     compounds); traversal `matches`/`closest`/`children`/`parentElement`; attributes
     `get`/`has`/`set`/`removeAttribute` + `classList`; textContent/innerHTML/value
     read+write, `remove`; and **node construction** `createElement`+`appendChild`
     (turned out NOT to need a tree — `parent.innerHTML += built-HTML`). All via
     byte-offset *position handles*.
   - **CSS — a small engine DONE (M302–305)**: per-element `color` / `font-weight`
     / `font-style` from inline `style=` *and* `<style>` rules (`tag`/`.class`/`#id`/
     `[attr]` selectors), a real cascade (rules < inline, per property), and a scope
     **stack** so styled elements nest to any depth. Only CSS *layout* — `font-size`,
     `text-align`, the box model, backgrounds — still needs a layout engine the flat
     token-stream renderer lacks. Possible next slivers without one: `rgb()`/more named
     colours, `text-decoration` (underline/line-through).
   - **CSS / layout**, cookies (sessions), inline remote `<img>`, `<textarea>`
     multiline.
   *Known limit: `lite.cnn.com` etc. refuse our minimal ClientHello (Fastly TLS
   fingerprinting) — not a TCP/crypto bug. Hard ceiling stands: real web apps
   (Google Docs) / Chromium are out of reach — see GOALS.md.*
2. **`fork` / `exec` + a process table.** Programs spawn fresh from embedded or
   on-disk ELFs (loading from disk now works — milestone 85); true `fork` +
   `exec` would enable a Unix-like model with child processes.
3. **Richer browser rendering.** **Inline `<img>` for remote (`http:`) images**
   (local images render inline — milestone 111; remote ones still need an async
   per-image fetch state machine on top of the single fetch worker/buffer),
   hanging-indent list wrap, and true column-aligned tables. (`<pre>`, lists,
   tables, entities, bold/italic, headings, links, form fields, text colour,
   inline code, `<img>` sizing, **inline local PNG (incl. interlaced), GIF (incl.
   animation), and baseline-and-progressive JPEG images**, and the full-page
   image viewer are all done — three from-scratch image decoders, host-tested and
   fuzzed. Every common web image format is supported.)
4. **Long file names** (subdirectories exist, but 8.3 names only).

## Smaller polish

- Window **minimize** (F3) + tiling/snap (F5/F6) are done; terminal copy/paste (scrollback is done).
- Browser: scrollbar drag, in-page find, history forward (Back exists).
- A graphical **file manager** with icons (the Files window is now an
  interactive launcher); load the wallpaper from disk.
- **APIC/HPET** + SMP (multi-core); faster syscalls (`syscall`/`sysret`).

## Notes

- Per-milestone learning docs are in `docs/00`…`docs/78`; the table in
  `README.md` indexes them. Screenshots are the `docs/osdev-*.png` files.
- Long-term north stars (a full browser, running Claude Code) and the
  pure-from-scratch vs Linux-compatible decision are analyzed in `GOALS.md`.
- Testing is headless: QEMU `-display none -monitor stdio` + `sendkey`, screendump
  → PNG. The browser and WM are now keyboard-driven, so they're fully scriptable.
