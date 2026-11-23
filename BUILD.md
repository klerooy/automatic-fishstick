# Building and validating RRTool

All paths below are relative to `RRTool_Public` unless a command first changes
directories. Build and inspect the user-mode overlay before studying privileged
components. The included binaries are not evidence that a new source build passed.

## Prerequisites

| Component | Required tools or inputs |
|---|---|
| Overlay | Windows x64, MSVC C/C++ tools and Windows SDK |
| Menu at runtime | Microsoft Edge WebView2 Runtime |
| WebView2 at build time | Bundled SDK in `2_src_port/WebView2SDK` |
| Driver | MSVC x64 plus Windows Driver Kit headers and matching libraries |
| Mapper | MSBuild, the v143 C++ toolset and Windows SDK |
| Offset tools | Python 3 and external dump inputs described below |

The overlay script searches VS 18 and VS 2022 Community/Professional in their
standard installation paths and calls `vcvars64.bat`. It does not search every
edition or custom path; adjust its detection list for another installation.
The driver script has its own detection list and selects an installed MSVC
toolset. It prefers WDK/SDK `10.0.26100.0`, with a fallback include-directory
search. Confirm that the selected headers and x64 libraries belong together.

## Build the overlay

From the project root in PowerShell:

```powershell
Push-Location .\2_src_port
cmd /c build_port.bat
Pop-Location
```

The output is `2_src_port/RobRunPort.exe`, copied to `1_run/RobRunPort.exe`. The
entry point is `WinMain` in `rr_esp.c`; `rr_main.c` belonged to an earlier
all-in-one attempt and is not part of this build. The script compiles the C
reader, drawing, aim, geometry, scan and process helpers, plus the C++ GPU and
WebView2 hosts. `rc.exe` embeds `menu.html` and `app.manifest` into `app.res`.

The build uses `/O2 /W3 /MT` for C and adds `/EHsc` for C++. The C/C++ runtime
is linked statically. The WebView2 loader is linked through
`WebView2Loader.dll.lib`, so **WebView2Loader.dll must remain beside the exe**.
WebView2 Runtime is a separate installation. The embedded menu needs no loose
HTML file at runtime.

Rebuild the launcher afterwards (`build_aio.bat`) so `RobRunAIO.exe` embeds the
new overlay. `1_run` keeps two rollback copies: `RobRunPort.exe.orig` is the
original binary before the menu edit, and `RobRunPort.exe.patched_menu` is that
original code with the current menu.

Two discovery timings were changed in `rr_game.c` so a fresh attach does not
stall: `game_init()` only uses the dictionary path and leaves the heap fallback
to the scan worker, and that fallback runs in bounded 3-second passes from a
rotating region cursor instead of blocking the worker for a whole sweep.

## Check the overlay offline

```powershell
Push-Location .\2_src_port
.\RobRunPort.exe --selftest --nohide --no-pause
.\RobRunPort.exe --menu --nohide
.\RobRunPort.exe --demo --nohide
.\RobRunPort.exe --demo --nohide --demo-seconds 10
Pop-Location
```

For a GUI executable, PowerShell may return before the process exits. To wait
for the self-test and inspect its exit code:

```powershell
$check = Start-Process .\2_src_port\RobRunPort.exe `
    -ArgumentList '--selftest','--nohide','--no-pause' `
    -WorkingDirectory .\2_src_port -WindowStyle Hidden -Wait -PassThru
$check.ExitCode
Get-Content .\2_src_port\selftest.log
```

Exit code zero and `SELFTEST OK` mean the built-in checks passed. They cover
configuration round trips, layout constants, projection and chain parsing.
They do not verify the current game, driver, rig or server behavior. Menu/demo
are interactive visual checks; `--demo-seconds N` closes the demo after N
seconds so an automated pass can render real frames without a manual END. All
modes use the same manifest and therefore request elevation. No game or driver
is needed for these checks, and `--nohide` skips the startup visibility request.

`--probe`, write tests, memory searches and other diagnostics have different
requirements and side effects. Read their dispatch branches and implementations
in `rr_esp.c` before running them.

## Loot labels and item probes

Loot names share one screen-space budget with the player labels. `rr_label_layout.h`
reserves a rectangle for every placed label; player name badges and the radar
panel are reserved as fixed anchors, so a queued loot name steps to the nearest
free line instead of printing over a player or the minimap. When every line
within five steps is blocked the label is dropped rather than stacked.
`tests/aim_checks.c` drives that header directly (`LABEL LAYOUT CHECKS OK`).

`--lootdump` prints each cached entry and, for items, the candidate
`InventoryItemDescription` fields listed in `offsets/item_pricing_fields.json`
(`Type` 0x20, `StackSize` 0x2C, `Rarity` 0x30, `IsStackable` 0x34,
`WeaponFunc` 0x50, `AmmoFunc` 0x68, `BasePriceFunc` 0x78, `SellPriceFunc` 0x88,
`Durability` 0x90). Those offsets were dumped from the same GameAssembly build
as the rest of the header, but the renderer still classifies dropped weapons by
display-name tokens: only a live `--lootdump` run can prove which offset really
holds rarity on the current build. The legacy probe values are printed on the
`item legacy` line so an older dump stays comparable.

`StackSize` is the template cap shared by every instance of an item, not a live
quantity. `--itemset <value> <filter>` writes it; the write is verified in
client memory and says nothing about whether the game obeys or persists it.

## Inventory probing and field access

`--classscan` takes comma-separated class names. A short term matches any class
name containing that text, so `--classscan Inventory` prints the inventory type
names, and the exact name from that output lists up to six live instances with
their first 0x80 bytes. That is the discovery path for the per-slot quantity
field: `InventoryItemDescription` is the shared template, the slot data is a
different type (see the static harvest below).

`--peek <hexaddr> [len]` hexdumps up to 0x200 bytes and `--poke <hexaddr>
<value> [size]` writes 1, 2, 4 or 8 bytes and reads them back. Both talk to the
game process only; neither calls `game_init`, so any address from a previous
dump can be examined without a live world.

The replication layer is `Framework.Replication.Data` primitives and reference
maps. A value written into a replicated object is local state: the next
authoritative update replaces it. Whether a write survives therefore answers a
different question than whether it works — it tells you who owns the field.

## Static signature harvest

`tools/harvest_il2cpp.py` reads the installed `global-metadata.dat` (v39) and the
type/field tables of `GameAssembly.dll` without running the game, and reports
every type, field offset and method name:

```bat
python tools\harvest_il2cpp.py find Inventory            :: type names matching a regex
python tools\harvest_il2cpp.py dump Models.Models.InventorySlotStackModel.InventorySlotStackModel
python tools\harvest_il2cpp.py methods Split             :: method names across all types
python tools\harvest_il2cpp.py strings "stack size"      :: UTF-8 literals with context
python tools\harvest_il2cpp.py bulk offsets\map.json "Models\.Messages\."
```

Generated artifacts: `offsets/type_index.txt` (38,195 type names),
`offsets/inventory_map.json` (406 inventory / replication / systems types),
`offsets/messages_map.json` (616 message / network / replication types). The
layout details, the verified inventory chain and the wire-protocol map are in
`docs/il2cpp_signatures.md`.

Two limits are worth knowing before trusting an offset from the dump:

* generic wrappers (`IPrimitiveData<T>`, `PrimitiveModel<T>`) report every field
  at offset 0 in metadata; resolve those on a live instance with `--offsets`;
* method *names* and tokens resolve, native addresses do not — this build keeps
  its code-pointer tables keyed by codegen module and the module name strings are
  unreferenced, so use `--classscan` / `--offsets` for live addresses.

Stack counts are bit fields inside the packed `InventorySlot` int
(`PayloadBits` / `PayloadShift` / `PayloadMask`), not a standalone `int`; the
masks are statics on the class, in `Il2CppClass.static_fields` at `+0xB8`.

## Duplication probes

Three modes instrument the quantity-race surface the harvest left open. They do
not send messages — the overlay has no send-path address — so the levers are the
client's predicted stack value and the input timeline.

```bat
:: every transition of one packed slot int, 1 ms polls, 30 s
RobRunPort.exe --dupewatch 1A2B3C4018 30000 1

:: same poll, but write the first good value back (hold mode)
RobRunPort.exe --dupehold  1A2B3C4018 30000 1

:: replay an input pattern: double | split | drag
RobRunPort.exe --dupeburst double 960 540 0 0 20 30 40
RobRunPort.exe --dupeburst split  800 610 1010 610 10 25 40
RobRunPort.exe --dupeburst drag   800 610 1010 610 4  40 60
```

`<addr>` is `InventorySlotStackModel + 0x10` (the inline `_data`). Find the
instance with `--classscan InventorySlotStackModel`, then `--peek <obj+0x10> 16`
until the value matches the count on screen.

Watcher output is one line per transition with the elapsed milliseconds, plus a
summary: `up`/`down` counts, `corrections` (a rise followed by a fall within
300 ms — the server overwriting a client prediction) and their latency, and
`fails`. A run with `fails > 0` is void: a failed read is not a zero and never
enters the transition stream.

`--dupehold` logs `foreign=` lines instead of transitions: those are the values
the game tried to write over the held one, and `blocked`/`writes` say how many
were pushed back. Holding only survives one round trip; the correction latency
measured in watch mode is that window.

The burst driver prints a cycle timestamp per cycle and the achieved gap
(mean/min/max) so its clock lines up with a watcher run started in a second
window. Coordinates are virtual-desktop pixels; the left button is always
released when the pattern ends, including after a failed drag.

The tactics these modes support, the message shapes they race and the
measurement table are in `docs/dupe_playbook.md`.

## Target filtering and model anti-aim

The aim and trigger filters treat a `RR_PLAYER` as a corpse when `dead` is set or
`health` reads zero. `rr_game.c` now reads `BattleCharacterModel.IsDead`
(`RR_OFF_CHAR_ISDEAD`, a `BoolData` handle) for every roster entry, and reads
`BattleCharacterPrivateData.Health` whenever health ESP, the aimbot or the
triggerbot is enabled. Without that, a body that is still in the roster kept a
valid aim point and a zero-health target could win a pick.

`rr_desync.h` plans the angle written into the local character's replicated
transform (`CharacterTransformModel.Yaw`, radians). Modes are spin, jitter,
static and random; rate maps 1-100 % onto 1-30 Hz; spin advances 45 degrees per
step. `rr_esp.c` writes the offset on top of the current camera yaw, so the
facing other clients interpolate is not the facing the shot comes from. The
`Stance flicker` switch toggles the crouch flag, which moves the head anchor
between two heights on clients that read it; the pre-flick stance is restored
when the feature is switched off. Both switches default to off and never write
in demo mode. `tests/aim_checks.c` covers the planner
(`MODEL ANTI-AIM CHECKS OK`).

The game re-derives the field from input every tick and the server may discard a
value it disagrees with, so this is a desync, not invulnerability.

## Trigger geometry

`rr_trigger_pick` tests the crosshair, not a metre radius around joint centres:
head, neck, chest and pelvis are projected and any sample within `trigger_fov`
pixels fires, which is the same at 5 m and at 90 m. The old sphere test needed a
near-perfect pixel hit and went silent as soon as the crosshair sat on a
shoulder. `trigger_los` keeps the captured-geometry trace optional, so a session
where collision capture has not finished can still fire. A dead local player
blocks both steering and clicks. `rr_aim_update` keeps a 500 ms sticky lock on
the acquired player and widens the acquisition ring to twice `aim_fov` while the
lock holds, so two bodies at the same screen distance cannot pull the view back
and forth.

## Build the driver

```powershell
Push-Location .\3_src_driver
cmd /c build_driver.bat
Pop-Location
```

The script invokes MSVC with `/kernel` and links against WDK x64 libraries.
It writes `x64/Release/driver.sys`, then copies that file to
`3_src_driver/driver.sys`. It prints deployment commands but does not execute
them. Building a driver does not load it.

Study `WM_PACKET`, the IOCTL definitions and `WmDispatch()` together
with `rr_mem.c`: request packing, field meanings, size caps and reply layout
must agree. METHOD_BUFFERED reuses the system buffer for request and reply,
so the driver copies request fields before overwriting that storage.

The included mapper loads a resident image without a normal unload lifecycle.
Closing the overlay or rebuilding the source does not replace that image.
The unload request removes access through the symbolic link; it does not unmap
the driver. A fresh Windows boot is needed to clear the mapped image.

## Build the mapper

In a Visual Studio Developer PowerShell with the v143 toolset installed:

```powershell
MSBuild .\4_src_mapper\kd2_src\kdmapper.sln `
    /p:Configuration=Release /p:Platform=x64 /m
```

The Release x64 project places `kdmapper.exe` in
`4_src_mapper/kd2_src/build`. The runtime package names that artifact `kd2.exe`.
The solution retains legacy channel blocks and upstream dependencies; those
blocks are not a required overlay transport. Building is separate from mapping.

## Runtime artifact map

| Built or bundled file | Distribution location |
|---|---|
| `2_src_port/RobRunPort.exe` | `1_run/RobRunPort.exe` |
| `2_src_port/WebView2Loader.dll` | `1_run/WebView2Loader.dll` |
| `3_src_driver/driver.sys` | `1_run/driver.sys` and `1_run/kernel-mode.sys` |
| `4_src_mapper/kd2_src/build/kdmapper.exe` | `1_run/kd2.exe` |

Close an executable before replacing its deployed copy. Keep compatible driver
and client builds together. The launch scripts are executable examples to read:
`START.bat` checks files and launches the overlay, while `MAP-DRIVER.bat` performs
mapping. `START-MASQ.bat` copies and renames an executable; it does not copy all
runtime dependencies or verify successful operation. None is needed for offline
source lessons.

## Understand the offset inputs

`robrun_offsets.h` and `robrun_sdk_offsets.h` are already included, so a normal
overlay build needs neither Python nor external game files. Their hashes and
source references identify the snapshots from which they were generated.

The maintenance tools consume inputs that are **not included** in this public
folder. Supply the exact dump and matching binary rather than reusing sample
heap addresses or assuming that a matching field proves runtime compatibility.

### CSV dump to base header

`gen_offsets.py` needs `types.csv`, `fields.csv`, `methods.csv`, `manifest.json`
and the matching `GameAssembly.dll`. The dump manifest's binary hash is checked.

```powershell
python .\tools\gen_offsets.py `
    --root 'C:\Path\To\Game' `
    --dump 'C:\Path\To\Dump' `
    --header .\2_src_port\robrun_offsets.h
```

Always supply `--header`: the script retains a legacy default under the absent
`2_src_cheat` directory. The report goes to `offsets/robrun_offsets.json`.
Review every missing field and static-global candidate. Candidate vote counts
do not identify active instances by themselves.

### CppSDK to supplemental header

`sdk_offsets.py` needs `Models.hpp`, `Battle.hpp`, `Game.hpp`, `Global.hpp`
and the matching `DumpManifest.sha256` in that directory's parent. The manifest
entries use paths such as `CppSDK/Models.hpp`.

```powershell
python .\tools\sdk_offsets.py --sdk 'C:\Path\To\CppSDK'
python .\tools\sdk_offsets.py --sdk 'C:\Path\To\CppSDK' --check
python .\tools\audit_unityentry_offsets.py `
    --sdk 'C:\Path\To\CppSDK' `
    --binary 'C:\Path\To\Game\GameAssembly.dll'
```

The first command rewrites the supplemental header and JSON report. `--check`
compares them without writing. The audit compares managed fields across both
headers, checks source fingerprints, and optionally checks the binary hash.
Review mismatches before rebuilding. Native Unity offsets, generic payloads,
method implementations and live object identities need separate verification.

Rig headers were exported from offline assets by a tool not included here.
`offsets/robrun_rig.json` preserves the palette and weighted/optional bone
provenance. Do not manually reorder palette indices to address a runtime issue.

## Build the all-in-one launcher

`RobRunAIO.exe` is one executable for the whole start-up order: it refuses to
run while VBS, Hyper-V or Memory Integrity is active, extracts its payload, maps
the driver with `kd2.exe --auto`, waits for `ROB RUN.exe` and its main window,
starts the overlay, then supervises it until game data appears. Its manifest
requests elevation.

```powershell
Push-Location .\2_src_port
cmd /c build_aio.bat
Pop-Location
```

The script embeds the current `1_run` binaries (`kd2.exe`, `kernel-mode.sys`,
`RobRunPort.exe`, `WebView2Loader.dll`) as resources, so rebuild those first if
the driver or overlay changed. Output: `2_src_port/RobRunAIO.exe`, copied to
`1_run/RobRunAIO.exe`.

At runtime the payload is unpacked to `payload/` beside the launcher, or to
`%LOCALAPPDATA%\RRAIO\payload` when that folder is read-only. The overlay writes
`CoreRR.cfg` and its logs there; a `CoreRR.cfg` beside the launcher is copied in
once. `status.log` and `hotkeys.log` rotate to `<name>.1` when they grow past
512 KiB, so a long session leaves the recent tail instead of an unbounded file.
A driver image matching the rejected 2026-11-07 hash is never mapped.

### Menu wait and reattach

The overlay is not started during the engine's boot. The launcher first waits
for the game's own top-level window, then for a configurable settle delay, so
the overlay starts on the main menu. After that it reads the overlay's
`status.log`: if the game is in a match, visible and not minimized, but no
snapshot with players appears within the timeout, the overlay is terminated and
restarted ("reattached"), with `robrun_runtime.txt` moved aside so discovery
runs a clean scan. The last instance is left running when the attempts run out.

Settings live in `RobRunAIO.ini` beside the launcher (all optional):

| Key | Default | Meaning |
|---|---|---|
| `menu_wait_window` | `1` | Wait for the game window before the delay starts |
| `menu_delay` | `30` | Seconds to wait after the window appears |
| `attach_timeout` | `60` | Seconds the overlay gets to report game data |
| `reattach_attempts` | `3` | Restarts before the launcher gives up |
| `hide_launcher` | `0` | Hide through the driver's process-list unlink (off: PatchGuard checks it) |
| `masquerade` | `1` | Run under a random benign name each launch |

### Masquerade

Process hiding by unlinking `ActiveProcessLinks` is not usable on this Windows
build: PatchGuard cross-checks `PsActiveProcessHead` against `KiProcessListHead`
and `PspCidTable`, so the unlink bugchecks `0x109`. The driver therefore
refuses veil requests by default (`SYSTEM\WmPort\Veil` = 0).

Instead, with `masquerade=1` the launcher copies itself to `%TEMP%` under a
random name from a small pool (`dllhost`, `taskhostw`, `RuntimeBroker`,
`SearchProtocolHost`, `backgroundTaskHost`, `WmiPrvSE`, `smartscreen`,
`ctfmon`, `sihost`), starts that copy with `--masq --ini <original ini>
--payload <payload folder>` and exits. The overlay is likewise copied to
`%TEMP%`, together with `WebView2Loader.dll` and (once) `CoreRR.cfg`, and is
started from there, so its `status.log`, `robrun_runtime.txt` and settings live
in `%TEMP%` as well; the launcher reads supervision data from that folder. The
previous copies are removed on the next launch (`masq.txt` beside the launcher,
`masq_overlay.txt` in the payload folder). If a copy is still locked
(`ERROR_SHARING_VIOLATION`) while an earlier instance starts or a scanner holds
the new file, the launcher retries briefly and then uses a unique name, so a
busy `%TEMP%` no longer leaves the overlay running under its real name. The
window title follows the chosen name, and the overlay's window is a tool
window, so neither appears in the Apps list.

`%TEMP%` remains visible as the process path in Task Manager's Details column,
but neither the executable name nor the tool folder is.

### Menu edits

The overlay loads its menu from resource 101 (RCDATA) inside `RobRunPort.exe`,
not from a loose file. Edit `2_src_port/menu.html`, then rebuild:

```powershell
Push-Location .\2_src_port
cmd /c build_port.bat
cmd /c build_aio.bat
Pop-Location
```

`update_menu.ps1` remains available for patching an existing binary without a
rebuild (`.\update_menu.ps1 -Rebuild`): it replaces the resource, verifies the
read-back byte for byte, keeps `RobRunPort.exe.orig` and rebuilds the launcher.

Check the launcher without touching the kernel:

```powershell
$check = Start-Process .\1_run\RobRunAIO.exe -ArgumentList '--selftest' -Wait -PassThru
$check.ExitCode
Get-Content .\1_run\payload\aio.log
```

`--selftest` extracts the payload, hashes the driver image and reports the VBS
state, then exits without mapping a driver or waiting for the game. The manifest
still requests elevation.

| Flag | Effect |
|---|---|
| `--dry-run` | Detect the process and window and wait the menu delay, then exit |
| `--no-map` | Skip driver mapping but keep going (UI diagnostics) |
| `--no-menu-wait` | Start the overlay as soon as the process is found |
| `--menu-delay <s>` | Override `menu_delay` |
| `--attach-timeout <s>` | Override `attach_timeout` |
| `--reattach-attempts <n>` | Override `reattach_attempts` |
| `--payload <dir>` | Use a different payload folder |
| `--overlay <path>` | Start a specific overlay executable (diagnostics) |
| `--ignore-vbs` | Bypass the VBS/HVCI refusal (diagnostics only) |

## Build troubleshooting

| Error | Check |
|---|---|
| `vcvars64.bat` or `cl.exe` missing | C++ workload and script installation paths |
| `rc.exe` or Windows libraries missing | Windows SDK and initialized developer environment |
| `ntddk.h` or kernel libraries missing | WDK installation and selected SDK version |
| MSBuild cannot find v143 | Install that toolset or deliberately retarget the project |
| WebView2 header or loader missing | Bundled SDK path and x64 loader files |
| Python reports missing dump files | Explicit external input paths; dumps are not bundled |
| Hash or field audit fails | Matching source snapshot, binary and generated artifacts |

A successful compilation verifies source and link compatibility with the selected
toolchain. Offline checks verify their stated cases. Neither establishes live-game
compatibility or stability of a privileged operation.
