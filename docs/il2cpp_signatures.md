# IL2CPP signature harvest — ROB RUN (Unity 6000.3.18f1, metadata v39)

Static harvest of the installed build. Everything below comes from
`ROB RUN_Data/il2cpp_data/Metadata/global-metadata.dat` plus the field-offset and
type tables of `GameAssembly.dll`. No runtime evidence is claimed for any entry
marked *unverified*.

## Target identity

| item | value |
|---|---|
| install | `C:\Program Files (x86)\Steam\steamapps\common\Rob Run` |
| metadata | v39 (magic `0xFAB11BAF`), 34,498,620 bytes |
| types | 38,195 |
| fields | 162,880 |
| methods | 265,397 |
| GameAssembly image size | `0x9050000` |
| code section | `.text` 8,028,672 + `il2cpp` 100,592,640 bytes |

Reproduce with:

```bat
python tools\harvest_il2cpp.py find <regex> [...]
python tools\harvest_il2cpp.py dump <exact.Namespace.TypeName>
python tools\harvest_il2cpp.py methods <regex>
python tools\harvest_il2cpp.py strings <regex> [...]
python tools\harvest_il2cpp.py bulk offsets\map.json <regex> [...]
```

Generated maps: `offsets/type_index.txt` (all type names), `offsets/inventory_map.json`
(406 inventory / replication / systems types), `offsets/messages_map.json`
(616 message / network / http / replication types).

## Metadata layout this build uses

Header triples are `(offset, byte size, count)`; the type-definition record is 82
bytes with the counts packed as `u16`:

| location | meaning |
|---|---|
| `+0x1A` | `fieldStart` |
| `+0x1E` | `methodStart` |
| `+0x3A` | `method_count` |
| `+0x3E` | `field_count` |
| `+0x44` | `vtable_count` |

`fieldStart + field_count` indexes the 12-byte field table (`nameIndex`,
`typeIndex`, `token`); the per-type offset array hangs off the metadata
registration at `+88`. Verified against `InventoryItemDescription`, whose table
reproduces the already-trusted `offsets/item_pricing_fields.json` byte for byte.

## Inventory object graph (instance offsets, classes)

```
InventoryModel                +0x10 Description : InventoryCellsSizeDescription
                              +0x18 Slots       : IInventorySlotsModel
InventorySlotModel            +0x10 Description
                              +0x18 ItemModel   : IInventoryItemModel
                              +0x20 StackModel  : IInventoryStackModel
                              +0x28 AmmoModel
                              +0x30 DurabilityModel
                              +0x38 CumulativePointsModel
InventorySlotStackModel       +0x10 _data       : InventorySlotData
                              +0x18 _itemModel  : IInventoryItemModel
InventorySlotItemModel        +0x10 _data       : InventorySlotData
CharacterInventoryModel       +0x20 _backpackSlotModel : IEquipSlotModel
                              +0x28 _startLinesCount
```

`InventorySlotsModel` itself exposes only a `get_Item` indexer; the slot storage
lives in the derived models (`CharacterInventoryModel`, `VehicleInventoryModel`,
`CargoInventoryModel`, `StorageBoxInventoryModel`, `LootEntityInventoryModel`, ...).

## Where the quantity lives

`Models.Models.InventoryStackModel.InventorySlotStackModel` is the stack owner:

```
get_Value()  / set_Value()          -> IInventoryStackModel
_data        (InventorySlotData)    -> packed InventorySlot, inline at +0x10
_itemModel   (IInventoryItemModel)  -> +0x18, item identity + IsEmpty
```

`Models.Models.InventorySlot.InventorySlot` is a bit-packed 32-bit struct, not a
`{itemId, count}` pair:

```
_raw               int   (packed)
DurabilityBits / DurabilityShift / DurabilityMask
PayloadBits    / PayloadShift    / PayloadMask
ItemBits       / ItemShift       / ItemMask
ExceptDurabilityMask / ExceptPayloadMask / ExceptItemMask
get_Durability() / get_Payload() / get_Item()
```

Consequences for the overlay:

* a stack count is a **bit field inside a packed int**, so an ESP that reads
  "count" cannot just dereference a wrapper and take `+0x18` as an `int`;
* the three masks/shifts are `static readonly`/`const` members, i.e. they sit in
  `Il2CppClass.static_fields` (`+0xB8`), which the overlay can already read;
* `InventoryItemModel` / `PrimitiveModel<T>` wrappers are `IPrimitiveData<T>`
  (`_value`), and for **open generic type definitions metadata reports every
  field at offset 0**, so generic wrapper offsets must come from a live
  instantiation (`--offsets <Class>`), never from this table.

Trustworthy `InventoryItemDescription` offsets are unchanged: `StackSize` `+0x2C`,
`Rarity` `+0x30`, `IsStackable` `+0x34`, `WeaponsFunc` `+0x50`, `SellPriceFunc`
`+0x88`, `Durability` `+0x90`, `IsSellAvailable` `+0x95`.

## Wire protocol surface

410 types under `Models.Messages.*`, 125 of them `Cheat*`. Every payload is built
from `Framework.Replication.Data.Primitive.IPrimitiveData<T>`, and the message
base class starts its fields at `+0x20` (`+0x10 Type`, `+0x18` message id slot).

Inventory-relevant messages:

* `Inventory.Take.InventoryTakeMessage` — batch: `_entityIds` (`ListData<int>`)
  plus `_commands` (`ListData<InventoryTakeCommandData>`);
  `InventoryTakeCommand` = `{EntityIdIndex +0x10, Type +0x18, FromSlot +0x20,
  ToSlotIndex +0x28, Count +0x2C}`.
* `Inventory.Swap.InventorySwapMessage` — `from`/`to` each carry
  `{Source, InventorySourceSlot, InventoryDestinationSlot, EquipSlot, PocketSlot,
  EntityId}`, so an inventory↔pocket↔equip↔external swap is one message.
* 12 `FromXToYMoveMessage` variants cover every container pair, plus
  `SellItemMessage`, `InventorySellItemMessage`, `UnloadAmmoMessage`,
  `InventoryUnsetMessage<T>`.
* `Inventory.SetInventoryItemCheatMessage` — `{SlotSource, InventorySlot,
  EquipSlot, PocketSlot, Item, Count, Ammo, Durability, CumulativePoints,
  EntityId}`; the only message shape that sets a slot's contents wholesale.

Server-authority evidence in the same harvest:

* `Models.Models.UsedTransactions.UsedTransactionsModel` with
  `Contains/Add/Remove/Clear` over `UsedTransactionsData` and
  `Transaction.RemoveUsedTransactionMessage` — a server-issued transaction log
  replicated to the client, i.e. replay protection, not a client counter.
* `Models.Http` style login/token stack (`Framework.Http.Dto.Request.Receipt.*`,
  `LoginResponse`, `OpsTokenResponse`) — the session is account-bound.
* Client-side validation strings recovered from the literal blob:
  `Item amount must be greater then 0: amount={0}, item={1}` and
  `Item amount must be less than stack size: amount={0}, stackSize={1}, item={2}`
  — the client predicts the same rules the server enforces.

## Dupe verdict, updated with binary evidence

The replication layer confirms the earlier reading and removes the last doubt:

1. Slot contents travel as **messages**, not as a client-writable replicated
   field. There is no `Count` property on a client-owned model that the server
   copies back.
2. Every applied change carries a transaction identity checked against
   `UsedTransactionsModel`. Two identical takes with the same transaction id are
   the only race with a real chance, and the check is exactly what it prevents.
3. `SetInventoryItemCheatMessage` is the only wholesale slot write, and it is a
   `Cheat*` message — server-side gating decides whether it is honoured.

So: no client-side quantity field to rewrite, no local clone that survives
replication, one narrow race surface, one cheat-message surface. Anything the
overlay could do to a local `InventorySlot` int is overwritten by the next
snapshot.

## Method addresses

Method *names* and tokens resolve for every type (`dump`), but native addresses
do not: this build keeps its code-pointer tables keyed by codegen module, and the
module name strings in `GameAssembly.dll` are not referenced from any pointer
table, so a static token→RVA walk is not available from the file alone.

Two supported ways to get an address:

* `rr_esp.exe --classscan <ClassName>` — finds the live `Il2CppClass` and dumps
  instances and field tables;
* `rr_esp.exe --offsets <ClassName>` — dumps the live field table with instance
  offsets, which is the authority for generic wrappers;
* `--peek` / `--poke` on any address the two above produce.

A future `--sigscan` (pattern → RVA inside the loaded `GameAssembly.dll`) is the
natural third step once a function is identified in a disassembler.
