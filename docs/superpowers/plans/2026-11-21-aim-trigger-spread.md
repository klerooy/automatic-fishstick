# Aim, TriggerBot and spread visualization implementation plan

**Goal:** Integrate the requested input and visual features into the existing external C/C++ overlay and rebuild both executable targets.
**Architecture:** Keep the existing snapshot, projection, rig selection, calibrated steering and WmPort reader. Run a single aim/trigger update from the input worker. Use the existing integer settings schema with percentage smoothing, relation bitmasks and legacy config aliases.
**Spec:** User-supplied code and requirements in this conversation; existing project layout and dump_analysis/01_Player_Entity.cs and 04_EntityManager.cs are the field-layout evidence.

## Constraints

- Work only in C:/Programs/RobRunScripts/RRTool_Public_NoKey; Desktop deployment remains a separate checked operation.
- Aimbot and TriggerBot use SendInput, never game-memory writes.
- Input is gated by game focus, closed menu, real data and fresh snapshots; demo and offline checks never emit real input.
- TriggerBot requires CLEAR LOS; UNKNOWN or BLOCKED suppresses firing. It works independently of aimbot enable. Key zero follows the aim key.
- Delay means continuous acquisition of the same target before the first click; subsequent clicks are rate limited.
- Read local spread via PrivateModel +0x228 -> PrimitiveModel +0x10 -> FloatData +0x18.
- Resolve PlayerClientModel through BattleEntitiesModel +0xD0 -> PlayerEntityModel +0x18, checking ServerModel +0x20 against the local character.
- FakeSpread at client +0x2C0 contains Spread/Air/Move at +0x10/+0x14/+0x18. Air/Move are spread contributions, not verified recoil angles; label the marker accordingly.
- Preserve the existing menu appearance and existing advanced aim options.

## Tasks

- [x] 1. Settings and input: migrate primary aim names to aim_enabled/aim_key/aim_fov/aim_smooth/aim_targets, retain legacy config aliases, add trigger fields. Expose rr_aim_update and rr_aim_is_active, reuse selection/steering, integrate focus/staleness gates and mandatory TriggerBot LOS. Add pure target and delay helpers for offline checks.
- [x] 2. Snapshot and drawing: add checked local spread fields/read helper and offset constants; add the circle and Air/Move marker using existing drawing primitives. Include these values in the frame signature.
- [x] 3. Menu: wire Aimbot, TriggerBot key/delay/relations, and visualizer controls in the Aim tab. Serialize masks and preserve half-step speed settings and existing key capture. Run the UI detector once and inspect available browser rendering.
- [x] 4. Verification and delivery: extend runnable offline checks with mocked input/LOS/memory, exercise stale/invalid inputs, relation masks, acquisition delays and partial SendInput. Build port and AIO, run selftests, verify embedded payload and refresh update manifests/source diff without overwriting Desktop edits.

## Added scope and verification

- Implemented weapon PNG/SVG atlas, WIC/Direct2D bitmap caching, cached fonts and bold role labels.
- Implemented health bars, armor shields, weapon-class colors, role colors and skeleton widths/distance fade.
- Implemented NPC-description membership at WorldEntitiesModel +0x3C0, independent NPC/role filters and configurable label distance (zero means unlimited).
- Port and AIO builds passed. Offline menu/config, memory, aim/trigger/LOS, NPC, ESP style and PNG tests passed; real SendInput was mocked.
- The actual game and GPU rendering remain unverified. Headless Chrome failed to initialize its GPU, and the native browser-control pipe was unavailable. No settings were changed to bypass either limitation.
- User confirmed that visualization should be named Weapon Spread and Movement Penalty; actual recoil-angle visualization is outside the available external fields.

Latest changes: removed speedhack entirely from source/settings/menu; added explicit weapon MK labels, per-item NPC/base price selector with conservative read-only delegate resolution, and verified Party membership using both member public ID and current entity ID. Unsupported callbacks and unverified market prices remain unavailable. Both builds and offline checks passed; live game validation is still outstanding.


## Vehicle ownership

- [x] Read CarModel/HelicopterModel.CharacterOwner -> VehicleOwnerModel public ID using verified metadata; resolve nicknames through the roster or slots. Keep unknown and empty ownership distinct, and never substitute the driver.
- [x] Add Players -> Vehicles -> Vehicle owner, snapshot fields, owner label and redraw signature; respect the configurable label distance.
- [x] Check cars/helicopters, 64-bit IDs, unavailable/empty data, nickname fallback and settings; rebuild Port/AIO and refresh checked delivery files.

Vehicle owner implementation verified: Port/AIO builds, owner memory/label/config checks and all existing offline checks passed (SELFTEST OK, zero failures). AIO payload matches the latest overlay; actual in-game drawing remains unverified.
