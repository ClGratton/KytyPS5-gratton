# Host memory: when the emulator needs more than your RAM

The emulator reserves a large guest memory area (13.5 GB) and uses up to about 9.6 GB of RAM at the
worst point of a level load. On a computer with 16 GB of RAM that, plus a desktop and a browser, can
run out of memory, and the system starts to stall. The launcher has a **Host memory** window that
looks at your computer, tells you whether a change helps, and applies it with one click.

It opens by itself at startup when a change is recommended (tick "Do not suggest this at startup" to
stop that), and it is always reachable from the "Host memory: ... details" line at the bottom of the
launcher.

Nothing is changed without a click and a password (Linux: the system's administrator prompt, through
`pkexec`; Windows: the UAC prompt). **Preview** shows exactly what Apply would do without changing
anything. **Undo** removes what the window installed and puts the previous settings back.

## When it does nothing

The window shows "OK" and offers nothing when your RAM covers the emulator plus a desktop with
margin. The threshold is derived from the emulator's expected peak (about 17 GB of RAM in total), so
a 24 GB or 32 GB computer is left alone.

## Linux

### What the window checks

- total RAM and what is free now;
- the swap devices: zram (compressed swap *in RAM*), a swap partition or a swapfile on disk, sizes,
  and whether the swap sits on a spinning disk;
- zswap: on or off, compressor, pool size;
- whether systemd and `pkexec` are present, and the filesystem and free space where a swapfile would go.

### Why zram can make it worse

zram swap lives in RAM. When the emulator's memory is swapped out, the pages stay in RAM, compressed,
so on a 16 GB machine the level load reached 0.07 GB free with 3 to 13 GB in zram. **zswap** keeps a
compressed cache in front of a *disk* swap instead: the coldest pages are written to the disk and
leave RAM. On the same machine, with zswap and the existing swapfile (zram off), the level load
swapped 0.05 GB to disk and never went below 1.1 GB available.

### What it proposes

| Your computer | The window | What Apply does |
|---|---|---|
| RAM covers the emulator + desktop (about 17 GB or more) | OK, nothing to do | nothing |
| Less RAM, a disk swap of 4 GB or more (partition or file), zswap off or zram in front | Recommended | zswap on (zstd, pool a percentage of RAM sized for about 3 GB), zram swap off |
| Less RAM, zswap already on and no zram | OK, already set up | nothing |
| Less RAM, no disk swap or a very small one, zram present | Optional | create a swapfile (half of RAM, 8 to 16 GB, always leaving 16 GB of disk free), zswap on, zram off |
| Less RAM, no swap at all | Recommended | the same swapfile + zswap |
| Less RAM, no disk swap, and no room (less than 4 GB left after the 16 GB reserve) or a filesystem other than ext4, xfs or btrfs | Not changed | nothing, zram stays |
| Swap on a spinning disk | Not changed | nothing: writing compressed pages to a hard disk would stall the game |
| Kernel without zswap, or no systemd | Not available | nothing |

zram is only ever switched off when a disk swap exists to take over.

### What it installs, and where

| What | Where |
|---|---|
| the helper (root-owned, mode 755), installed once by the first Apply or Undo | `/usr/local/libexec/kyty/kyty-host-memory`, or `/usr/libexec/kyty/kyty-host-memory` when `/usr/local` is writable by a group (Debian) |
| a polkit action that names that exact path, `auth_admin` (best effort; without it the password prompt has the generic text) | `/usr/share/polkit-1/actions/org.kyty.host-memory.policy` |
| a one-shot service that sets the zswap parameters at every boot | `/etc/systemd/system/kyty-host-memory.service` |
| the script that service runs (compressor, pool size, swapon of the swapfile, zram off) | `/var/lib/kyty-host-memory/boot.sh` |
| the previous state, for Undo | `/var/lib/kyty-host-memory/state` |
| a drop-in that stops zram-generator from creating the zram device (only when zram is switched off and zram-generator is installed), one section per configured device | `/etc/systemd/zram-generator.conf.d/99-kyty-host-memory.conf` |
| the swapfile (only when one is created; a separate btrfs subvolume with copy-on-write off on btrfs) | `/var/lib/kyty-host-memory/swap/swapfile` |

It does not touch the bootloader, the kernel command line, `fstab` or the sysctl settings. The change
also takes effect immediately, so no restart is needed. If the free memory is too small to move the
contents of zram back to RAM right now, zram goes off at the next boot instead; Apply says so.

The helper refuses to run while an emulator is running.

### Why the helper is installed as root

Running a script as root that a normal user can modify (or that sits in a directory a normal user can
modify) would let any program running as that user take over the machine the moment you type the
password. So:

- The helper text is compiled into the launcher together with its SHA-256. The first time you click
  Apply or Undo, one password prompt runs an install step that checks the text against that hash and
  installs it root-owned into a root-only directory (above). The prompt for that step names `/bin/sh`,
  because the install step is a script passed to the shell.
- Before every later password prompt, the launcher checks that the installed helper and every
  directory above it are owned by root and not writable by group or others, and refuses to run it
  otherwise. If a launcher update carries a different helper, the next Apply replaces it (same prompt).
- Preview runs the compiled-in text as you, with `--dry-run`, so it changes nothing.

The launcher binary itself is what you trust: install it where only you can write.

### Removing the helper

Undo removes the service, the drop-in, the swapfile and the settings, but leaves the helper installed.
To remove it as well (as root):

```
rm -r /usr/local/libexec/kyty /usr/libexec/kyty /usr/share/polkit-1/actions/org.kyty.host-memory.policy
```

On image-based systems with a read-only `/usr` (Fedora Atomic, Bazzite) the helper still installs
under `/usr/local`, but the polkit action cannot be written; the password prompt then has the
generic text.

### Costs

- Compression uses some CPU time while memory is tight (zstd; a few percent of one core).
- When the compressed pool fills, the coldest pages are written to the disk swap. A level load
  wrote a few hundred MB; the SSD sees some extra writes.
- A created swapfile takes its size from your disk.

### Doing it by hand

```
launcher --print-host-helper > kyty-host-memory              # the text compiled into the launcher
sudo bash kyty-host-memory apply --zswap-percent 21 --disable-zram
sudo bash kyty-host-memory undo
```
Add `--dry-run` to see the actions without running them. `--swapfile-mib N` creates a swapfile first.
From a terminal, `launcher --host-memory` prints what the window would show; `launcher --print-host-helper --sha256`
prints the hash the install step checks.

## Windows

*The Windows part has not been tested on a real Windows machine.*

### Why it matters

The emulator's guest memory is a section backed by the pagefile (`CreateFileMapping` with
`INVALID_HANDLE_VALUE`, 13.5 GB). Windows charges its **whole size to the commit limit** (RAM plus
pagefile) when it is created, even though only the part the game touches is resident. If the commit
limit leaves less than about 15 GB free, creating it fails with "paging file is too small"
(error 1455) and the emulator does not start. The commit limit is too small when the pagefile is
turned off, or fixed to a small size (some "debloat" guides do this).

Windows memory compression, on by default, keeps idle memory compressed in RAM instead of paging it.
Some tweaking tools turn it off.

### What the window checks

total RAM, the commit limit and how much of it is in use, the pagefile mode (system managed, fixed
size or none) and whether memory compression is on.

| Your computer | The window | What Apply does |
|---|---|---|
| pagefile system-managed, or a commit limit with 15 GB free | OK | nothing |
| pagefile fixed or off and less than about 15 GB of commit free | Recommended | let Windows manage the pagefile size |
| memory compression off and less than about 17 GB of RAM | Recommended | turn memory compression on |

Both changes take effect after a restart. Apply records the previous settings in
`%ProgramData%\KytyHostMemory\state.json`; Undo restores them (the previous fixed pagefile entries
and the compression setting). The settings themselves persist in Windows; no service or scheduled
task is installed. The helper is `kyty-host-memory.ps1`, compiled into the launcher and passed to an elevated
PowerShell as an encoded command after the UAC prompt, so no file on disk is what runs as administrator. The
state folder is made writable by administrators only, and a pre-existing folder owned by someone else is refused.

### Costs

A system-managed pagefile can grow on the system drive (up to three times RAM, only when needed).
Memory compression uses some CPU time while memory is tight.
