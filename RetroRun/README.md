# RetroRun

RetroRun is a small Windows tray program for removable disks: floppy and SuperDisk disks, USB drives, memory
cards, CDs and DVDs, and external hard drives. For the kinds of drives you choose, it:

- **shows disks' own names and icons in File Explorer.** Explorer shows every floppy drive as "Floppy Disk Drive"
  with the stock icon, whatever disk is in it. RetroRun reads the disk and shows its autorun.inf label or volume
  label, and its autorun.inf icon or the first icon file in its root.
- **starts autorun.inf programs** as a disk goes in, as AutoRun did before Windows limited it to CDs and DVDs.
- **turns off Windows' AutoPlay** while it runs, so that AutoPlay and RetroRun don't both act on a disc.

It runs as the current user, needs no administrator rights, and needs Windows 10 or 11.

## The tray menu

| Item | What it does |
| --- | --- |
| One line per drive | The drives RetroRun follows, and what it found in each |
| Read the disks now | Reads every followed drive again |
| Display names and icons (submenu) | The kinds of drives whose disks' names and icons are shown; floppy and SuperDisk drives at first |
| Display autorun.inf icon | Shows the icon that the disk's autorun.inf names |
| Display autorun.inf label | Shows the label that the disk's autorun.inf names |
| Display first icon on disk (no autorun.inf) | Shows the first `.ico` file in the disk's root when no autorun.inf icon is shown |
| Display volume label (no autorun.inf label) | Shows the volume label when no autorun.inf label is shown |
| Launch autorun.inf applications (submenu) | The kinds of drives whose autorun.inf programs are started |
| Disable native AutoPlay handling (submenu) | The kinds of drives that Windows' AutoPlay is kept from acting on |
| Look for disks (submenu) | The kinds of drives that are asked every 2 seconds whether a disk is in; all at first |
| Start with Windows | Starts RetroRun when you sign in |
| Exit | Ends RetroRun, which gives the drives their usual names back |

Each submenu lists the kinds of drives, with the letters of those present: floppy disk drives, SuperDisk drives,
USB drives, memory cards, other removable disks, CD and DVD drives, and external hard drives. Its "All removable
disks" item ticks them all, or unticks them all when all are ticked.

## Names and icons

RetroRun gives Explorer what it found through the drive letter's per-user key,
`HKCU\Software\Classes\Applications\Explorer.exe\Drives\<letter>` (`DefaultLabel` and `DefaultIcon`). It marks
the keys it writes with a `RetroRun` value and never changes a key without that mark. Icons are copied from the
disks to `%LOCALAPPDATA%\RetroRun\Icons`.

Explorer reads the disks in drives other than floppy drives itself, and what it finds comes first: the volume
label before `DefaultLabel`, and a disc's autorun.inf icon and label before both. On those drives RetroRun fills
only what Explorer leaves out, such as the first icon file of a disc whose autorun.inf names no icon.

## Launching

When a disk goes into a drive of a kind ticked under **Launch autorun.inf applications**, RetroRun reads its
autorun.inf as Windows does: the `[autorun.amd64]` section on 64-bit Windows if the file has one, and `[autorun]`
otherwise. It starts the `shellexecute=` entry, which may name a document, or else the `open=` entry, from the
disk's root. `open=start file`, the form that Windows 95's START program ran, opens the file.

Nothing is started:

- when the file sets `UseAutoPlay=1`;
- when Shift is held as the disk goes in;
- when the window in front, or a program registered with AutoPlay, cancels AutoRun;
- when Windows' AutoPlay is set to start a disc's program itself, unless native AutoPlay handling is disabled
  for the drive;
- for a disk that is already in when RetroRun starts or when its kind is ticked. A floppy controller's drive tells
  of no disk until Windows mounts it, so a disk there is started when it is first read.

## Disabling native AutoPlay handling

While RetroRun runs, it registers with AutoPlay to be asked first: an `IQueryCancelAutoPlay` object in the running
object table, whose class it lists under
`HKCU\Software\Microsoft\Windows\CurrentVersion\Explorer\AutoplayHandlers\CancelAutoplay\CLSID`. For drives of
the kinds ticked under **Disable native AutoPlay handling**, it tells AutoPlay not to act, so that AutoPlay
neither asks what to do nor does anything, and doesn't start a disc's program either. The listing is removed when
RetroRun exits. Explorer still reads discs itself, for their autorun.inf icon and label, and no per-user setting
changes that.

## How disks are noticed

Drives are recognized by their devices, whatever their letters; SuperDisk drives by their model (LS-120 or
LS-240). How a disk is noticed depends on the drive:

- A drive that describes itself, as SuperDisk, CD and other ATAPI and USB drives do, can tell from its own state
  whether a disk is in. While **Look for disks** is ticked for its kind, it is asked every 2 seconds, until it
  reports its disks by itself.
- A floppy controller's drive can tell only by running its motor, so its disk is read when Windows mounts it, as
  when you open the drive, or when you choose **Read the disks now**.
- An external hard drive's disk never changes, so there is nothing to look for.
- Drives other than floppy drives also tell of their disks through Windows' device broadcasts.

A drive whose volume another program has locked is left alone until that program lets it go. That includes a
program that had the drive before RetroRun did, as an emulator does that takes a drive as soon as it appears:
RetroRun is not told of that lock, but the volume refuses to be read, and its tray menu shows the drive as in use
by another program until it is let go.

## Settings and files

The settings are DWORD values under `HKCU\Software\RetroRun`, which the menu writes. `Display`, `Launch`,
`Native` and `Look` hold the ticked kinds of drives as bits: 1 floppy disk drives, 2 SuperDisk drives, 4 USB
drives, 8 memory cards, 16 other removable disks, 32 CD and DVD drives, 64 external hard drives. Two settings
have no menu item: `CheckMs`, how often drives are asked whether a disk is in (2000 milliseconds, at least 500),
and `RefreshAll` (0 or 1), which has Explorer redraw every icon instead of only the drive's.

RetroRun's log is `%LOCALAPPDATA%\RetroRun\RetroRun.log`, started afresh when it grows past 512 KiB.

## Building

RetroRun builds with CMake 3.21 or later. `CMakePresets.json` has a preset for each toolchain, which takes its
compiler from `PATH`:

| Preset | Toolchain |
| --- | --- |
| `vs2022` | Visual Studio 2022 or its Build Tools: MSVC, x64. CMake finds Visual Studio itself. |
| `mingw` | MinGW-w64 GCC (`g++` and `windres`), such as MSYS2's `mingw64`, `ucrt64` or 32-bit `mingw32` |
| `clang` | `clang++`: LLVM for Windows, which uses Visual Studio's libraries, or a MinGW-w64 Clang |
| `clang-cl` | `clang-cl`, from a Visual Studio developer prompt |

The `mingw`, `clang` and `clang-cl` presets use Ninja. To build and test with one:

```
cmake --preset vs2022
cmake --build --preset vs2022
ctest --preset vs2022
```

The program is `build/<preset>/Release/RetroRun.exe`. Add `--config Debug` to the build command for a debug
build, and configure with `cmake --preset vs2022 -A Win32` for 32-bit x86. Visual Studio can also open the folder
and build from the presets.

The tests check how autorun.inf files are read, how their commands are split into a program and its parameters,
and how drives are told apart by model. They start nothing.

## Installing

There is no installer. Copy `RetroRun.exe` to a folder of your choice, such as `%LOCALAPPDATA%\Programs\RetroRun`,
run it, and tick **Start with Windows** in its tray menu. `RetroRun.exe /exit` asks a running copy to exit.

To remove it, untick **Start with Windows** and choose **Exit**; RetroRun takes its drive keys and its AutoPlay
listing back as it exits. Then delete the program, the key `HKCU\Software\RetroRun` and the folder
`%LOCALAPPDATA%\RetroRun`.
