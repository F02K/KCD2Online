# libKCD2/KCSE migration audit

This is the capability matrix for the native multiplayer client. The removed
KCD2Online implementation records intended behavior only; it is not an engine ABI
reference and is never used as a fallback.

| Capability | Native implementation | Verification |
|---|---|---|
| Game tick and lifecycle | KCSE `PostUpdate` plus DataLoaded/LoadGame/SaveGame/NewGame runtime epochs | Implemented and covered by epoch/stale-handle tests |
| Local player | `CCryAction::GetClientEntity` and `S_GameContext::GetActorById` | Typed slots and actor lookup |
| Transform read/write | `IEntity::GetWorldTMPtr` and `IEntity::SetWorldTM` | VTable slot audit plus identical-write/readback active probe |
| Entity isolation | Exact eight-slot `IEntitySystemSink`, verified Human/Animal AI-Actor classification, full `IEntitySystem::RemoveEntity` teardown, scheduler/player exclusions, and orphaned-combat recovery outside a 50 m PvP radius | Spawn/remove/reuse tests and audited sink/combat targets |
| Remote avatar | Native `IActorSystem::CreateActor("NPC")`, shared Soul materialization, monotonic Actor/Human/Soul/stabilization/Inventory readiness, strict desired-Soul policy | Phase-regression, timeout, no-fallback, opt-in fallback diagnostic, stale epoch, external destroy, and cleanup tests |
| Locomotion | Native Actor MovementController requests for Idle/Walk/Run plus interpolated world transform | Backend tests and runtime error propagation |
| Inventory/equipment | Native item creation, logical instance GUID, count/quality/condition, slot-ordered equip/unequip/delete, draw/holster | Transaction and rollback tests plus active probe |
| RPG profile | Absolute money, 10 stats, 35 skills, normalized progress, inventory, equipment, and avatar state | Full capture/apply/reconcile/rollback tests |
| Sandbox | Native save/load lock, deferred engine `unload`, lifecycle completion gate, and root-main-menu return | Console FIFO dispatch avoids unloading from KCSE `PostUpdate`; main-menu open is VTable-validated and SEH-guarded |
| UI boundary | Copied POD snapshots and commands, compatibility-gated by the shared KCD2Online version (`0.1.4`) | ABI size/project-version tests; no engine pointer crosses the boundary |

## Runtime ownership and capability publication

`KCD2OnlineKCSEClient.dll` owns the client state machine, network thread, identity
store, and all engine-facing state. `d3d12.dll` only consumes the versioned UI
ABI. Entity, Actor, Soul, item, and WUID values never cross the UI ABI or the
network protocol.

Only KCSE's game-thread task touches engine objects. Every runtime epoch
invalidates queued work and native handles. Before joining, the runtime runs an
active probe against the loaded save:

1. write the current local world matrix and verify the readback tolerance;
2. create a hidden Default-Soul NPC and wait for Entity, Actor, Soul, Human, and Inventory;
3. create, equip, unequip, and delete a real catalog item;
4. remove the probe Entity and confirm that it no longer resolves.

The complete capability mask is published only after this probe succeeds.
Failures identify the exact failed native stage and leave the client out of the
server world; they never select an alternate Lua, signature, console, or
Entity-name path.

## Static ABI gate

The offline audit is pinned to Steam content `23914554`, WHGame build
`1308617_856`, PE timestamp `0x6A350E20`, image size `0x5B2D000`, and SHA-256
`BDF8F9E4A11257A72B64C84700E284C29E4C4CCAF5B8D4BFA7D0B2A7294479F7`.
It validates the native relocation targets, vtable slots, prologues, and call
chains used by multiplayer. Other game binaries are rejected as a genuinely
different ABI.
