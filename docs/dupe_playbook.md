# Duplication kit — instrumentation and playbook

What the overlay can do about quantity duplication today: measure a stack slot's
packed value at millisecond resolution, keep the client's predicted copy alive
long enough to issue a second command, and drive the input side of a race with a
known, logged cadence. What it cannot do yet: emit a message. There is no
send-path address on this build (see `docs/il2cpp_signatures.md`, "Method
addresses"), so everything below works on the two surfaces that are actually in
hand — predicted state and input timing.

## What the binary settled

| fact | source | consequence |
|---|---|---|
| slot contents are written by `Models.Messages.*` messages | 410 message types, `InventoryTakeMessage` / `InventorySwapMessage` / 12 `FromXToYMoveMessage` | no client-owned quantity to raise locally |
| the packed slot int is a prediction, replaced by the next snapshot | `InventorySlotStackModel._data` at `+0x10`, `InventorySlot._raw` bit fields | a write survives one round trip at most |
| every applied change carries a transaction id | `UsedTransactionsModel.Contains/Add/Remove`, `RemoveUsedTransactionMessage` | identical replays are the guard's job, not a client counter |
| `SetInventoryItemCheatMessage` is the only wholesale slot write | message map | server-gated; the one true "set it" surface |
| client mirrors the server's amount rules | literals `Item amount must be greater then 0`, `... must be less than stack size` | the client refuses the second draw once its prediction is empty |

The race that remains is the one the guard was built to stop: two commands
validated against the same pre-state. Its inputs are (a) the client still
believing the stack exists, and (b) the two commands arriving close together.

## Finding the slot

```bat
RobRunPort.exe --classscan InventorySlotStackModel
RobRunPort.exe --peek 1A2B3C4010 16
```

`--classscan` prints `klass <addr> instances=<n>` and up to six `obj <addr>`
lines in its summary. For each object, `--peek <obj+0x10> 16` shows the packed
int; the one whose decoded count matches the number on screen is the slot being
watched. The chain, for orientation:

```
CharacterInventoryModel +0x18 Slots -> IInventorySlotsModel   (get_Item only)
InventorySlotModel     +0x20 StackModel -> InventorySlotStackModel
InventorySlotStackModel +0x10 _data      packed InventorySlot (the int)
InventorySlotStackModel +0x18 _itemModel item identity / IsEmpty
```

The indexer is a method, not a field, so the overlay cannot enumerate slots by
walking; instance discovery through `--classscan` is the accessible path.

## Watching a value

```bat
RobRunPort.exe --dupewatch 1A2B3C4010 30000 1
```

| field | meaning |
|---|---|
| `[t=+Nms] base=0x...` | first good read, the reference point |
| `0x... -> 0x... (+delta)` | transition; `delta` is the signed change of the whole packed int |
| `correction +delta after Nms` | a rise followed by a fall inside 300 ms — the server's copy landing on top of a client prediction |
| `samples` / `fails` | poll health; `fails > 0` voids the run, a failed read is never a zero |
| `net=+N` | `last - first`; a positive net with no matching source action is the only thing that counts as a dupe landing |
| `correction latency: ... (mean ...)` | round-trip estimate, i.e. the window a hold can span |

Because the value is packed, `delta` is not "items gained". Read the raw hex
before and after and compare against the visible count; the bit fields
(`ItemBits`, `PayloadBits`, `DurabilityBits`) all live in that int.

## Holding the prediction

```bat
RobRunPort.exe --dupehold 1A2B3C4010 30000 1
```

Every poll: read, compare with the value captured at `base`, and write the
captured value back if they differ. `foreign=0x... kept=0x...` lines are the
game's writes being pushed back; `blocked`, `writes`, `write_fails` are the
summary.

What it buys: the client keeps predicting a non-empty stack, so the UI still
lets you issue a draw from it. What it costs: every write is a prediction, not
authority, and the holder is fighting the same snapshot that corrects a plain
`--poke`. The hold survives one correction window, which is why the watch
latency matters — that number tells you how much room the second click has.

Run order for a hold attempt: watch first for 10-15 s to get the latency, then
hold with `--dupeburst` firing inside the first window.

## Input patterns

| pattern | steps | races |
|---|---|---|
| `double` | move, click, click | ground pickup: `InventoryTakeMessage` for a loot entity that the server is also about to delete. The node's removal and the take are separate messages |
| `split` | move A, click, move B, click | split/transfer confirm: `InventoryTakeCommand{EntityIdIndex +0x10, Type +0x18, FromSlot +0x20, ToSlotIndex +0x28, Count +0x2C}`. Two confirms, one pre-state |
| `drag` | press A, interpolate, release B | the same move expressed as `InventorySwapMessage` / a `FromXToYMoveMessage`; the drag threshold forces real motion, so the pattern is slower per cycle than `double` |

`repeat` is clamped to 5000, `step_ms` to 1000, `cycle_ms` to 5000. Every cycle
prints `cycle N at +Nms`, and the summary prints the achieved gap
`mean/min/max` — the achieved gap is what the server sees, not `cycle_ms`.

The driver only sends left-button moves and clicks. A modifier (Ctrl for split
amounts, Shift for quick transfer) stays under your fingers: hold it while the
burst runs.

## Example: pickup race

```bat
:: window 1 — measure
RobRunPort.exe --dupewatch 1A2B3C4010 20000 1

:: window 2 — drop the stack on the ground, then burst on it
RobRunPort.exe --dupeburst double 960 540 0 0 12 28 38
```

Win: the watcher shows `+N` with no `correction` line, and the ground node is
gone in the game's own view. Loss: `correction -N after <latency>` — the server
removed the take it did not authorise, and the latency number is the guard's
turnaround.

## Example: split confirm

```bat
RobRunPort.exe --dupehold 1A2B3C4010 20000 1
RobRunPort.exe --dupeburst split 800 610 1010 610 8 22 35
```

The hold keeps the source predicted non-empty between confirms; the burst fires
the confirm twice inside one window. Win: both destination slots show the count
and neither source is empty after the next snapshot. Loss: `foreign=` lines
every cycle, `blocked` equal to the number of writes the client attempted, and
no net change.

## Results table

Fill one row per attempt; the two logs share a millisecond clock.

| # | tactic | slot addr | before | after | net | corrections | latency | notes |
|---|---|---|---|---|---|---|---|---|
| 1 | pickup double | | | | | | | |
| 2 | split confirm | | | | | | | |
| 3 | drag move | | | | | | | |
| 4 | hold + split | | | | | | | |

## Next build step

The missing piece is the send path. `Il2CppClass` carries a `methods` array of
`MethodInfo*`, and `MethodInfo.methodPointer` is at `+0x00`; the class scan
already locates live `Il2CppClass` structures, so a `--methoda` mode that walks
`methods[]` and prints `methodPointer` per method name turns
`SetInventoryItemCheatMessage` / `InventoryTakeMessage` from names into callable
addresses. After that, `--sigscan` maps a disassembled function back to an RVA
once one is identified. Until then the kit stays on the prediction and input
surfaces documented above.

## Verification status

Compiled into `RobRunPort.exe` (`build_port.bat`) and linked by the offline
selftest runner. Live behaviour — poll rate under load, actual correction
latency, whether a hold can outrun a snapshot — is unmeasured; every number in
the table above is a slot to fill, not a result.
