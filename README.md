# YamOS

YamOS is an ambitious x86_64 operating system built around **YamKernel**, a graph-based hybrid kernel. It boots with Limine, brings up a modern kernel core, and layers a small desktop/userland on top of YamGraph, a live resource graph where tasks, memory, devices, files, channels, and capabilities are modeled as connected nodes.

Current tree: **v0.5.0 development line** — **Phase 3 (Connected Ecosystem)** in progress. (Some in-tree banners or shell strings may still show older version labels until refreshed.)


<img width="1920" height="1032" alt="YamOS desktop environment screenshot" src="https://github.com/user-attachments/assets/dfebb446-79b5-413f-805d-fae61ffc8fdd" />


## Not Complete Yet

YamOS cannot yet run arbitrary x86_64 Linux/Windows software or install large
browser engines and language runtimes as-is. These are still missing or incomplete for a general-purpose desktop OS:

- production-style installed root/system volume (single coherent layout); optional virtio FAT32 can persist `/home`, `/var`, and `/usr/local` when attached
- full POSIX/Linux syscall compatibility
- fully proven `execve` semantics and a stronger process/thread model
- dynamic linker / shared libraries
- threads and signals
- real permissions/users/security
- TLS/HTTPS + certificate store
- `epoll`, fuller socket options, and Unix-domain sockets
- package manager/downloader
- full browser engine support
- language runtime porting layer
- real hardware driver coverage beyond the current QEMU-focused path

Important missing or incomplete surfaces include fully tested process
replacement (`execve`) semantics, dynamic linking, pthread/thread-group
teardown, full TLS program-header support, signals, file permissions,
file-backed `mmap`, `epoll`, PTYs/termios, TLS/certificates, package
signatures, browser-engine storage/sandboxing, and broader hardware drivers.

## What Is In This Repo

| Area | Status | Notes |
| --- | --- | --- |
| Boot | Stable | Limine BIOS/UEFI ISO, YamBoot menu, framebuffer splash, module loading. |
| CPU | In-tree | GDT/IDT/TSS, CPUID, MSRs, SYSCALL/SYSRET, NX/SMEP/SMAP/UMIP/WP, APIC/IOAPIC, PIT/RTC, HPET discovery, TSC capability reporting. |
| SMP | Partial | Limine starts AP cores; APs initialize and park. Local APIC IPI primitives exist; scheduler currently uses CPU 0 only. |
| Memory | In-tree | Zone-aware PMM, VMM, heap, slab, CoW/fork support, per-task `brk`, anonymous `mmap`, `mprotect`, kernel/user stack guards, boot kernel PML4 snapshot for `vmm_get_kernel_pml4()` (correct CR3 reload onto kernel threads), full user page-table destruction for process cleanup, and TLB shootdown hooks. |
| Scheduler | In-tree | CFS-style scheduler, wait queues, mutexes, futexes, cgroups, OOM, idle/power hooks, task lifetime cleanup counters, deferred orphan/dead-task reaping, and conservative shared-address-space retention until full thread-group teardown lands. |
| Syscalls | In-tree | File, process, memory, scheduler, Wayland, driver, AI, touch, YamGraph IPC, OS info, app registry, TCP sockets, VFS ELF spawn, first `execve`/PT_INTERP path, user-space `waitpid` status copy-out, `stat`/`fstat`, `ftruncate`, `rename`, first `*at` path calls, `unlink`, `readdir`, `select()` (backed by `poll`), `fcntl()` descriptor/non-blocking pieces, and `SYS_SCHED_INFO` cleanup counters. |
| Filesystems | In-tree | VFS with initrd root, writable ramfs mounts, devfs, procfs, per-process cwd, relative paths, `openat`-style dirfd resolution, open existence checks, `O_APPEND` writes, metadata-backed `SEEK_END`, directory listing, delete, rename, create/truncate/ftruncate, basic file metadata, FAT32 read/write/unlink driver code, 1MB LRU block cache with dirty tracking, block core with QEMU virtio-blk and AHCI disk registration, MBR/GPT FAT32 discovery, auto-mounted block FAT32 volumes under `/mnt`, and per-user `/home/<username>` directory auto-creation at login. |
| Networking | In-tree | e1000 path plus ARP, IPv4, ICMP, UDP, DHCP, DNS, TCP state-machine with non-blocking connect/recv/accept (`EAGAIN`/`EINPROGRESS`), `SOCK_NONBLOCK`, fd-backed TCP socket ABI, plain HTTP, certificate-store bootstrap, and bounded TLS ClientHello probe. |
| USB/Input | In-tree | XHCI controller path, USB core, HID, keyboard, mouse, evdev, touch, gestures. |
| PCI/Drivers | In-tree | Bridge-aware PCI scan, command/status helpers, safe BAR sizing, MSI/MSI-X capability discovery, AHCI SATA storage driver, and driver inventory binding. |
| Desktop | In-tree | Wayland-style compositor with multi-user support (up to 8 accounts, per-user home dirs auto-created at login); polished first-boot setup/login; File Manager (user-home-aware, sidebar with Home/Users/Root/Temp/System/Volumes, address bar, search, sort, new file/folder, text editor); calendar/time/status bar; standalone Ethernet/Wi-Fi/Bluetooth/Sound/Display settings windows; DRM damage tracking (dirty rectangles); Bochs VBE runtime resolution modesetting; top menu; dock/taskbar; standard window controls; maximize/restore; VTTY mode. |
| Userland | In-tree | Static Ring 3 ELF apps/services for `authd`, `/bin/hello`, `/bin/exec-test`, `/bin/orphan-test`, libc/libyam syscall support, native app manifests, argv/envp-aware VFS-backed spawn, PID 1 **`boot-probes`** (`exec-test` exec ABI + fork/exec burst + scheduler deltas; `orphan-test` detach/deferred-reap counters), and kernel app registry. Main desktop tools are compositor-native kernel services. |
| AI/ML | In-tree | Tensor allocation and accelerator abstraction syscalls. |

## Current Desktop Behavior

- First boot opens a full setup experience for computer name, username, and password.
- Setup now persists to `/var/lib/yamos/system.pro` after account creation or bypass; later boots load the saved device/account profile and go to login instead of asking for computer name, username, and password again.
- Press `Ctrl+Shift+Y` on the setup or login screen to auto-create default users and enter the desktop.
- Bypass defaults:
  - `root / password`
  - `guest / guest`
- Terminal, Browser, and Calculator currently launch as compositor-native apps for reliable drawing.
- Browser (YamBrowser, MIT in-tree) loads **`about:yamos`** by default (built-in OSS info page); plain HTTP works through kernel DNS/TCP/HTTP with toolbar, history, and static HTML paint. Full HTTPS, JavaScript, full CSS layout, real images, and Firefox-class engines remain future work.
- Calculator is a compositor-native desktop utility with standard/scientific modes, fixed-decimal arithmetic, memory register, history, keyboard input, and compositor clipboard copy/paste.
- File Manager launches from the dock or File menu with a full Explorer-style shell: sidebar locations (Home for the current user, Users `/home`, Root, Temp, System, Volumes), back/forward/up navigation, editable address bar and search field, sortable details view (name/type/size), item details pane, new file/folder dialogs, file delete, and an integrated text editor. It opens in the logged-in user's own home directory (`/home/<username>`) and reads that path from the live compositor session on launch.
- The desktop bar shows BDT calendar/time, wired network status from the kernel network interface, Wi-Fi radio state, Bluetooth radio state, and audio mixer state.
- Top-bar status chips open their own settings directly: Ethernet opens Ethernet Settings, Wi-Fi opens Wi-Fi Settings, Bluetooth opens Bluetooth Settings, Sound opens Sound Settings, and the clock opens Calendar. Re-clicking an already-open settings chip focuses/restores that same window instead of spawning duplicates. The old shared Quick Settings popover path has been removed from the compositor. The detailed settings windows use standalone layouts instead of one shared/sidebar shell, and expose DHCP renewal, scan/connect or scan/pair actions, and sound mixer controls while reporting honest blockers: QEMU currently has wired e1000 networking, real Wi-Fi needs firmware/MAC work, Bluetooth needs a USB HCI backend, and audio output needs a real device driver.
- Wi-Fi and Bluetooth scan/connect/pair controls now consume explicit kernel driver operation results. If the radio is off, no supported adapter/controller is present, firmware is missing, or USB HCI transport is pending, the settings window and `net` shell command report the exact blocker instead of implying the connection attempt succeeded.
- The dock launcher now uses a two-column layout with applications on the left and standalone settings shortcuts on the right for Ethernet, Wi-Fi, Bluetooth, Sound, and Display.
- Display Settings opens from the View menu and shows compositor/framebuffer resolution, stride, active surface count, focused window, plus refresh and debug-overlay controls.
- Terminal file commands now use the same VFS: `ls`, `cat`, `mkdir`, `touch`, `rm`, and `write /home/root/file.txt text`.
- Terminal can launch the sample static ELF app with `run /bin/hello arg...` or direct `hello arg...`.
- Terminal `sched` shows scheduler lifetime/cleanup counters, including task-object frees, kernel-stack frees, pml4/VMA/fd/YamGraph cleanup, and orphan/deferred reaping counters.
- Terminal supports `cd` and `pwd`; VFS paths now resolve relative to each task's current working directory.
- App windows use right-side minimize, maximize/restore, and close controls; minimized apps restore from the dock.
- The visible Terminal is `src/os/services/compositor/wl_terminal.c`.
- Language-specific runtime installers are not part of the current tree; the
  project is focused on kernel, drivers, memory, storage, networking, and OS
  services.
- Terminal commands `installer status`, `install`, and `install kernel-net` inspect or request the generic OS capability probe.
- Shifted characters such as `Shift+9 = (`, `Shift+0 = )`, and uppercase letters are routed through evdev to focused apps.

## Quick Start

On Ubuntu/WSL (same environment **`scripts/test.ps1`** uses on Windows):

```bash
make setup
make iso
make run
```

From Windows, `.\scripts\test.ps1` (with `serial`, `headless`, `verify`, or `debug`) runs those Makefile targets inside WSL Ubuntu.

Useful targets (run from the repo in WSL/Linux):

```bash
make iso              # Build build/yamkernel.iso
make run              # Run QEMU BIOS mode
make run-uefi         # Run QEMU UEFI mode
make run-serial       # Write serial output to build/serial.log
make run-serial-only  # Headless serial console
make debug            # QEMU paused with GDB server on localhost:1234
make verify-log       # Bounded QEMU boot → build/verify.log (needs GNU coreutils `timeout`; same cues as test.ps1 verify)
make clean
```

Build outputs:

- `build/yamkernel.elf`
- `build/yamkernel.iso`
- `build/authd.elf`
- `build/hello.elf`
- `build/exec-test.elf`
- `build/orphan-test.elf`
- raw splash assets under `build/logo.bin` and `build/wallpaper.bin`

## Architecture

```text
+------------------------------- Ring 3 ---------------------------------+
| authd | hello | exec/orphan boot probes | libc + libyam | future apps      |
+-------------------- Syscall ABI (SYSCALL/SYSRET) ----------------------+
| process | memory | VFS/file | sockets/net | poll/select | app registry  |
| compositor/window | driver control | IPC/channel | AI accelerator       |
+------------------------------- Ring 0 ---------------------------------+
| Scheduler/CFS | VFS + FAT32 + initrd + procfs/devfs | TCP/IP stack      |
| PCI + block + e1000 + USB input | DRM + compositor | cgroups | OOM      |
+------------------------------- YamGraph --------------------------------+
| Nodes: tasks, memory, devices, files, channels, namespaces              |
| Edges: owns, maps, depends, channel, capability                         |
+-------------------------- Memory / CPU / Boot --------------------------+
| PMM | VMM | Heap | Slab | GDT/IDT/TSS | APIC/IOAPIC | ACPI | Limine     |
| YamBoot | framebuffer splash | module loading                            |
+-------------------------------------------------------------------------+
```

Reading guide:
- Ring 3 runs user-space services and apps through the public syscall ABI.
- Ring 0 provides kernel scheduling, storage, networking, drivers, and compositor services.
- YamGraph tracks ownership and capability relationships across runtime resources.

## Source Tree Highlights

```text
src/boot/                 YamBoot menu
src/kernel/               kernel entry, shell, panic handling
src/kernel/api/           syscall numbers and public ABI structs
src/cpu/                  GDT/IDT/APIC/ACPI/SMP/syscall/security
src/mem/                  PMM, VMM, heap, slab, OOM
src/sched/                scheduler, wait queues, cgroups, user entry
src/fs/                   VFS, FAT32, initrd, ELF loader, poll
src/ipc/                  IPC scaffolding
src/net/                  ARP/IP/ICMP/UDP/TCP/DHCP/DNS stack,
                          non-blocking socket layer, select/poll
src/drivers/              PCI, USB, input, serial, timer, video, DRM, net
src/nexus/                YamGraph, channels, capabilities
src/os/bin/               PID 1 init (`init_task`), boot-time probes wiring
src/os/apps/              authd, hello, exec-test, orphan-test, user linker script
src/os/lib/               libc and libyam
src/os/README.md          OS-layer layout and app-facing structure rules
src/os/dev/               devfs and virtual TTYs
src/os/proc/              procfs
src/os/services/          compositor and OS services
```

## Application Architecture

- `src/kernel/api/syscall.h` is the public ABI source of truth for kernel and userspace.
- `src/os/lib/libyam/app.h` exposes the native app SDK: `yam_os_info()`, `yam_app_register()`, `yam_app_query()`, and `yam_spawn()`.
- `src/os/services/app_registry/` records Ring 3 app/service manifests in the kernel with PID, YamGraph node, app type, requested permissions, name, publisher, version, and description.
- `authd` now registers itself as a native YamOS service before using YamGraph IPC.
- `/bin/hello` is packaged as `hello.elf`, registered into initrd, and launched through the argv/envp-aware VFS-backed ELF spawn path. **`exec-test.elf`** (`/bin/exec-test`) covers missing-path exec, same-PID replacement, cwd, fd inheritance, FD_CLOEXEC, a **`fork` → `execve(/bin/hello)` → `waitpid`** burst with **`SYS_SCHED_INFO`** deltas asserted by init, and is wired through **`limine.conf`**. **`orphan-test.elf`** (`/bin/orphan-test`) validates orphan / deferred-reap counters when the parent exits without `waitpid`. Bare names resolve through `/bin`, `/usr/local/bin`, `/opt/yamos/packages`, and `/home/root/bin`.
- Userland now has TCP socket ABI with `socket`, `bind`, `connect`, `listen`, `accept`, `send`, and `recv`; `sendto`/`recvfrom` syscall numbers exist but still return honest failure until UDP/fd semantics are completed. Non-blocking I/O is supported via `SOCK_NONBLOCK`, `fcntl(fd, F_SETFL, O_NONBLOCK)`, and `select()` with a 128-fd `fd_set`. `EAGAIN`/`EINPROGRESS` are returned on non-blocking operations with no data.

## Boot Flow

1. Limine loads the kernel, framebuffer, and ELF/assets modules.
2. YamBoot shows Normal, Safe Mode, and Reboot choices.
3. The kernel initializes framebuffer, CPU tables, security flags, memory, ACPI/APIC/SMP, HPET/TSC detection, and syscalls.
4. YamGraph is initialized and core subsystems are registered.
5. Drivers and subsystems start unless Safe Mode was selected.
6. **Scheduler**, **cgroups**, **OOM**, **power**, and **AI** subsystems are initialized; preemptive ticks run when **`YAM_PREEMPTIVE`** is on. **PID 1 `init`** is spawned (`sched_spawn("init", …)`). With **`YAM_WAYLAND`** enabled, the Wayland compositor task is spawned next; it runs **concurrently** with init (not after probes finish). Init registers `/bin/hello`, **`exec-test`**, and **`orphan-test`** from Limine modules when present, then starts autostart services including **`boot-probes`** (runs `/bin/exec-test` then **`sched_drain_deferred_reaps()`** then `/bin/orphan-test`).
7. The BSP idle loop yields; AP cores stay initialized and parked.

## Runtime Notes

- Current verification: `powershell -ExecutionPolicy Bypass -File .\scripts\test.ps1 verify` runs **`make iso`** then **`make verify-log`** inside WSL Ubuntu. The bounded QEMU boot writes **`build/verify.log`** (`Makefile`: **`timeout` 120s**, `-serial file:…`, **256M** RAM, **2 CPUs**). Expect **`[INIT] Process ABI probe PASS`** (fork/exec burst + exec ABI checks + scheduler **`forked+` / `created+` / `reaped+`** deltas) and **`[INIT] Orphan cleanup probe PASS`** (**`orphans_detached+`** / **`detached_reaped+`** style deltas). **`sched_drain_deferred_reaps()`** runs between probes from PID 1.
- **Scheduling + kernel page tables:** `vmm_init()` captures the BSP kernel PML4 once; `vmm_get_kernel_pml4()` returns that snapshot instead of reading CR3, so switching to kernel threads with `task_t.pml4 == NULL` always compares against the real kernel root and reloads CR3 after user tasks (fixes latent wedge after fork/exec bursts before the next `elf_spawn`).
- Safe Mode skips several driver/subsystem init paths for easier boot triage.
- `YAM_PREEMPTIVE` and `YAM_WAYLAND` are enabled in `src/kernel/main.c`.
- `YAM_DEMO_TASKS` is disabled by default.
- The desktop compositor starts with first-boot setup/login, can spawn apps, and includes a VTTY render mode.
- `Ctrl+Shift+Y` bypasses setup/login by creating default accounts and logging into the desktop.
- Terminal installer commands now route to the generic OS installer/capability service. TLS/certificate capability is probed with an outbound TLS ClientHello and ServerHello check; persistent package installation still needs package signatures and downloader/install transactions.
- Browser engines, language runtimes, and package tools should use the fd-backed socket ABI instead of private kernel-only network helpers as that ABI matures.
- First missing-compatibility slice added: per-process current working directory, `SYS_CHDIR`, `SYS_GETCWD`, libc `chdir/getcwd`, and relative VFS path resolution.
- File Manager writes through VFS. With the QEMU virtio FAT32 disk attached, `/home`, `/var`, and `/usr/local` are persistent; without it they fall back to ramfs. The File Manager opens in the current user's home directory (`/home/<username>`) and the sidebar exposes a `Users` shortcut to `/home` so an admin can browse all accounts' directories.
- VFS `open()` now returns failure for missing paths unless `O_CREAT` is supplied, while `/dev` and `/proc` only open known pseudo-files.
- libc `stat()`/`fstat()` now route to kernel VFS metadata instead of guessing file type in user space.
- libc `ftruncate()` now routes to `SYS_FTRUNCATE` for RAMFS/FAT32 file resizing.
- libc `rename()` now routes to `SYS_RENAME`; RAMFS renames nodes in place, and FAT32 regular-file rename currently uses copy-then-unlink.
- First libc `*at` wrappers now route to kernel dirfd-aware syscalls: `openat`, `fstatat`, `mkdirat`, `unlinkat`, and `renameat`.
- VFS honors `O_APPEND` before each regular file write, so libc append modes such as `fopen(path, "a")` append instead of overwriting from offset zero.
- `lseek(fd, 0, SEEK_END)` now uses VFS metadata through `fstat`, so EOF seeking works for initrd, RAMFS, FAT32, and other stat-backed files.
- `open(path, O_CREAT | O_EXCL, ...)` now fails if the path already exists, enabling basic lock-file and exclusive-create patterns.
- QEMU runs attach `build/yamos-fat32.disk` as `vd0`; YamOS auto-mounts FAT32-compatible virtio disks at `/mnt/vd0`, exposes mounted volumes in `/mnt`, and promotes `/home`, `/var`, and `/usr/local` to the FAT32 disk when available.
- PID 1 registers **`/bin/hello`** from its Limine module when present (and may install **`/usr/local/bin/hello-local`** on writable roots) but **does not auto-run hello at boot** (stability mode). Ring 3 **`hello`** coverage is exercised by **`/bin/exec-test`** (`fork` / **`execve("/bin/hello")`** / **`waitpid`**) and by the desktop Terminal (`run` / bare **`hello`**).
- First `execve` work is in tree: `SYS_EXECVE` routes to the ELF loader, static process replacement exists, PT_INTERP maps a main ELF plus interpreter, argv/envp/auxv stack setup exists, replaced address spaces are destroyed, and FD_CLOEXEC descriptors are closed on exec. PID 1 **`boot-probes`** run **`/bin/exec-test`** (missing-path exec, same-PID replacement, cwd, fds, FD_CLOEXEC, fork/exec burst + **`SYS_SCHED_INFO`** deltas) then **`/bin/orphan-test`** (orphan/deferred-reap counters). Dynamic-loader/TLS details still need focused tests.
- Process lifetime cleanup now frees reaped child fd tables, VMA metadata, non-shared pml4s, FPU state, kernel stacks, YamGraph task nodes, and task objects. Forked tasks clone VMA metadata and get their own YamGraph nodes. Parentless dead tasks can be queued for deferred cleanup, and forced-dead runqueue tasks are cleaned instead of only decrementing counters. Shared thread address spaces are retained conservatively until full thread-group teardown exists.
- AP cores are initialized and can receive kernel IPIs, but full multi-core task scheduling remains disabled until address-space switching and run-queue ownership are audited.
- TSC-deadline is detected when the CPU exposes it. The current timer path still uses the calibrated periodic APIC timer unless a later platform-specific timer switch is added.
- CPU exceptions print register state, and the panic path has a register-frame variant for fatal exception debugging.
- Phase 2 (Performance & Hardware Foundation) complete: The storage stack has been refactored from monolithic memory mapping to a page-based LRU block cache, and the display stack now supports hardware-accelerated 2D damage tracking and runtime resolution modesetting.
- Phase 3 (Connected Ecosystem) in progress:
  - **Non-blocking socket ABI complete** — `SOCK_NONBLOCK`, `fcntl(F_SETFL/F_GETFL)` (SYS_FCNTL=93), and `select()` (SYS_SELECT=92) are live. TCP `connect`/`recv`/`accept` return `EAGAIN`/`EINPROGRESS` when non-blocking. Userland `fd_set` (128-bit, 2×u64) and `FD_ZERO/SET/CLR/ISSET` macros in `libc/sys/socket.h`.
  - **Process ABI hardening in progress** — first `execve`, PT_INTERP loading, FD_CLOEXEC, per-task `brk`, task cleanup counters, kernel-stack freeing, fork VMA metadata cloning, deferred orphan cleanup, **boot-time fork/exec burst + orphan counter probes**, and **kernel PML4 snapshot + CR3 reload correctness** for kernel threads (see Runtime Notes). Static exec verification covers failed exec, same-PID replacement, cwd, fds, and FD_CLOEXEC.
  - **Multi-user file manager** — File Manager opens in `/home/<current_user>`, resolved from the compositor session at launch. Home dirs (`/home/<username>`) are created by `sys_mkdir` automatically at first-boot setup, skip-setup, and at every successful login.
  - Next: thread-group teardown, PT_TLS/dynamic-loader smoke tests, full TLS/HTTPS handshake, and ypkg package manager.

## Documentation

- `README.md` - Primary project status, architecture, and capability notes.
- `developer.html` - Current architecture and subsystem reference.
- `DEBUGGING.md` - Build, QEMU, serial, GDB, and triage guide.
- `future_plan.txt` - Updated roadmap from the v0.4 tree toward v1.0.
- `implementation_plan.md` and `task.md` - Implementation planning and history.

## License

MIT
