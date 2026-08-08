# Join crash investigation

## Scope and baselines

- KCD2Online: `master` at `8d4b913`.
- Bundled libKCD2 fork: `800f6aea241a22afe0dd592d905df89765a43734`.
- libKCD2 upstream comparison point: `6d92dc3` (`upstream/master`).
- Nested KCSE loader: `9279808`, identical on fork and upstream.
- Runtime target: WHGame/KCD2 1.5.6, release index 15693.

The fork diff is 24 files (`+495/-59`). The current multiplayer runtime does
not use a Lua `System.SpawnEntity` call. It calls:

```text
IActorSystem::CreateActor(
    channelId = 0,
    name = "KCD2Online_Remote_<epoch>_<playerId>_<handle>",
    actorClass = "NPC",
    pos,
    rot,
    scale = (1,1,1),
    entityId = 0)
```

`CreateActor` internally enters the engine entity-spawn path. There is no
`fileNPCName` or entity-template parameter in this code path. The selected
remote appearance is a validated shared-Soul GUID, applied after the Actor's
Soul becomes available.

Likewise, the native KCSE runtime does not dereference Lua `g_localActor`.
Its equivalent readiness chain is `CCryAction::GetClientEntity()` plus
`S_GameContext::GetActorById(entity->GetId())`; both pointers and the current
EntitySystem pointer are traced before critical accesses.

## Main finding

The 2026-07-31 reproduction identifies the join fault. It is not in remote
Puppet spawning. The active native ABI probe completed Actor creation, physics,
Soul application, transform, locomotion, inventory creation, and equipment
successfully before the connection opened.

The server then sent `BOOTSTRAP_MODE_INITIALIZE` (`mode=1`). This mode is used
when the server has no canonical spawn yet: the initializer must use its
current engine-default player transform and return it in `ClientWorldReady`.
The client verified that `m_local_transform` existed, but its spawn selection
ignored that value:

```text
profile.transform_valid ? profile.last_transform : bootstrap.spawn
```

For an uninitialized session, both the new profile's `transform_valid` and the
manifest's `spawn_valid` are false. Reading the absent protobuf
`bootstrap.spawn()` produces its default value, including a zero-length
quaternion. Copying that value into the profile while setting
`transform_valid=true` changes a valid wire profile into an invalid local
target. This explains the durable trace line:

```text
join.sandbox.profile-apply.failed |
success=false rollback_succeeded=false error="target profile is invalid"
```

A second control-flow bug converted that harmless pre-validation rejection into
the access violation. `reconcile_profile` correctly reports
`rollback_attempted=false` because no native mutation occurred. The caller
looked only at `rollback_succeeded=false`, interpreted it as a failed rollback,
and called `begin_native_unload()`. Its first engine call was
`IGameFramework::EndGameContext()`, after which the PostUpdate guard caught
`0xC0000005` at `0x7ffa1da062d3`.

The fix:

1. selects profile transform, valid server spawn, or (for `INITIALIZE`) the
   local engine-default transform, in that order;
2. validates the selected transform and complete target profile before
   disabling save/load or touching the native profile backend;
3. unloads a modified world only when
   `rollback_attempted && !rollback_succeeded`;
4. logs and locally SEH-guards `EndGameContext` for a genuine future rollback
   failure.

The initializer selection and no-mutation rollback semantics have regression
tests.

### Follow-up: equipped hunting-sword alias

After the bootstrap fix, the next join reached native baseline-profile capture
and failed on equipped item `b867dd0e-1bfe-40e9-b114-4b126a3ff1b0`.
The installed `Tables.pak` defines it as:

```xml
<ItemAlias
  SourceItemId="c164f346-0463-4116-b790-094b11274e5e"
  Id="b867dd0e-1bfe-40e9-b114-4b126a3ff1b0" />
```

The equipment catalog previously indexed only item nodes carrying their own
armor or weapon metadata. It skipped `ItemAlias`, so the native inventory could
not map this equipped hunting-sword variant to `PrimaryMainHand`. The parser now
resolves `SourceItemId` chains after collecting direct definitions and inherits
slot, layer, and weapon class while retaining the alias GUID. A regression test
uses this exact alias, and the resulting test executable also succeeds against
the locally installed game `Tables.pak`.

### Follow-up: fractional Groschen

Native KCD2 money stacks count tenths of one Groschen. An early prototype stored only
whole Groschen and rejected any native total not divisible by ten. Rounding
would make transaction rollback lossy, so a subsequent prototype milestone added
`PlayerProfile.money_subunits` constrained to `0..9`. Native capture now splits
the exact amount into whole Groschen and subunits; native apply combines both
fields back into the exact stack amount. Equality checks, validation, server
persistence, and rollback all include the subunit field. Starter-profile TOML
continues to express whole Groschen and therefore defaults to zero subunits.

### Follow-up: starter inventory and native equipment slots

The next initializer reached the profile transaction but failed before the
money step. The starter profile assigned `quality = 100.0` to
`kettleFood_wineBarrel`, while native KCD2 item quality is a grade and this
non-equippable Food item exposes its definition quality instead of mutable
equippable runtime data. `update_item()` therefore returned `false` during its
readback check without setting an error. The rollback repeated that failure;
the resulting emergency `EndGameContext()` call raised the guarded access
violation.

Starter item quality is now `1.0` and starter templates accept only native
quality grades `0..4`. `equipped_slot` remains optional: an item without it is
inventory-only. Before any destructive profile mutation, the native backend now
verifies that every slotted item is equippable and that its catalog slot matches
the requested slot. Readback failures include the item instance and actual versus
expected count, condition, and quality. Persisted revision-1 profiles whose
initial world bootstrap never completed are refreshed from the corrected starter
template on their next authentication.

### Follow-up: first skill during profile apply

After inventory and money reconciliation succeeded, the transaction completed
all ten stat writes and raised an access violation on the first skill
(`stealth`). The first correction replaced the stat factory with address-library
ID `66710`, the raw `C_SkillXPEffect` constructor at Steam RVA `0xC65AD0`. That
constructor was still called with the return value of event-manager vtable slot
1 as its storage pointer. The native AddStatXP/AddSkillXP paths use that slot to
prepare the manager and ignore its return value; their effect factories allocate
the 0x40-byte cause object themselves. The invalid skill effect later crashed in
native code at Steam RVA `0x2429DC8`.

Skill XP events now use ID `66709`, the verified self-allocating
`C_SkillXPEffect` factory at Steam RVA `0xC65A20`, paired with the existing stat
factory ID `66740`. `dispatch_xp()` mirrors the native prepare, resolve, prepare,
construct, dispatch, and smart-pointer release sequence. Per-value begin,
complete, and failure trace entries identify the exact RPG value in future join
logs.

The next reproduction no longer raised an exception in the skill event, but
the first `stealth` write was rejected by readback. The absolute-XP calculation
had the native curves reversed: skills use the generic cumulative curve at ID
`66688` (Steam RVA `0xC64A4C`), while stats use the ID-specific cumulative curve
at ID `85622` (Steam RVA `0xFA30DC`). The old skill path also summed cumulative
totals as though they were per-level costs, which produced zero XP for target
level 1. Absolute writes now interpolate between the cumulative total for the
requested level and the next level. The intermediate implementation dispatched
through the same unmodified-XP factories used by the game's
`AdvanceToSkillLevel` (ID `129572`) and `AdvanceToStatLevel` (ID `85625`) paths.
Failed readback traces include both the requested and actual level/progress.

A subsequent trace proved that the skill progression event is not a universal
snapshot setter: `stealth` and `horse_riding` accepted it, but `fencing` kept its
live cell at level/progress zero. The second dword of each `S_StatCell` is the XP
accumulated inside the current level (it is read by the native skill-progress
getter), not unused padding. Authoritative skill restores therefore write the
requested level and interpolated within-level XP directly to both the live and
base/snapshot cells. This covers all 35 stored skill slots, including special and
obsolete entries, and avoids progression-event side effects on other RPG state.
Stats continue to use the verified native stat event path.

The following reproduction completed the entire profile transaction and entered
the multiplayer sandbox. It then stalled while applying `ServerEntityControl`.
The native isolation loop resolved the local player for every Entity and applied
AI/physics/visibility mutations without checking whether the Entity was actually
AI-controlled. This contradicted the server contract and could touch UI, camera,
particle, equipment, trigger, and other engine-helper Entities. Enumeration also
used an unbounded `IsEnd()` loop instead of the engine-observed `Next() == null`
termination. Isolation now caches the local Entity id, requires the native
`has-AI` flag plus Human/Animal RTTI, follows the observed `MoveFirst`/`Next`
iterator protocol, and is strictly bounded by the Entity count captured before
mutations begin. The later visibility-only implementation proved unsafe because
`IEntity::Hide` disabled Entity physics/update state without removing the Actor
from AI, combat, dialog, and interaction registries. The later `Invisible`
experiment kept Actors updating but did not resolve the player's orphaned combat
mode, so isolation returned to bounded native `IEntitySystem::RemoveEntity`
teardown. A multiplayer-only recovery now detects the local combat flag, drawn
weapon stance, and still-running combat actions without a remote player inside
50 meters. It interrupts the actions through the engine's regular `Stop` path,
clears the opponent through the null-safe `C_CombatActor::SetOpponent(nullptr)`
path, and requests holstering.
KCD's AI-backed player and horse scheduler proxies also match the broad
`C_Human + HasAI + !IsPlayer` shape. They are now resolved through
`wh_ai_PlayerSchedulerProxy`/`wh_ai_PlayerHorseSchedulerProxy` and excluded;
mutating either proxy stalls player action transitions and player-relative
MonsterLOD processing.

## Fork versus upstream

### Spawn and bindings

- `Offsets::IActorSystem::CreateActor` is unchanged between fork and upstream.
  Its parameter order matches the CryEngine SDK declaration. The custom
  `void*` reference parameters retain the same Win64 ABI (addresses in the same
  argument registers).
- `IEntitySystem::SpawnEntity(SEntitySpawnParams&, bool)` is unchanged and is
  not called directly by KCD2Online.
- The fork changes `Offsets::IEntitySystem::AddSink` from two declared
  parameters to three and adds the complete typed `IEntitySystemSink` vtable.
  The runtime calls the new signature with an event mask of zero.
- `IEntity` changes only name previously anonymous vtable slots
  (`Activate/IsActive`, `Hide/IsHidden`) and marks `SetWorldTM` verified. Slot
  positions do not change.

### Struct/layout changes

- `C_Actor` gains a method only; no data members move. Existing size/offset
  assertions remain (`sizeof=0x9C0`, Soul at `+0x668`, movement controller at
  `+0x180`).
- `C_Soul+0x300` changes from an opaque 16-byte field to `CryGUID`; size remains
  16 bytes and subsequent fields do not move (`sizeof=0xD20`).
- `C_Human`, `CEntity`, and `C_InventoryBase` gain methods only.
- No fork diff changes `S_GameContext::m_pActorSystem` (`+0x180`) or the
  `CreateActor` vtable slot.

There is therefore no static evidence of a parameter-order or data-size
mismatch in the actual CreateActor call. Risk remains in the fork's new raw
relocation/vtable helper implementations listed above.

## Candidate exclusions and remaining risk

- **Remote Puppet spawn:** excluded by the reproduction. The probe completed
  every spawn, first transform/animation, Soul, and equipment operation before
  networking began.
- **Wrong thread:** excluded for the observed path. Networking callbacks
  decode and enqueue protobuf messages only. Engine work is drained from
  KCSE's `CCryAction::PostUpdate` hook. Every trace line includes thread ID and
  role; the faulting path says `role=kcse-post-update`.
- **D3D12 callback:** excluded for the current native path. The D3D12 module is
  the UI/client proxy; it does not spawn remote Actors.
- **Local Actor readiness:** excluded for this reproduction. It was guarded
  before Connect and repeatedly traced with non-null Entity and Actor pointers.
  The runtime requires DataLoaded, a PostUpdate frame, local entity/Actor, a
  readable transform, and the full active ABI probe before network connect.
- **Invalid `fileNPCName`:** not applicable in the current path. Class is the
  literal `NPC`; the shared-Soul GUID must exist in the runtime catalog.
- **Initialization timing / fork offsets:** not the cause of this fault. The
  trace proves those calls returned successfully. Raw relocations remain a
  general runtime risk, but they are downstream of no failing operation here.
- **Initialization order:** KCSE installs its PostUpdate hook before queued
  multiplayer frames execute, and `can_start_join()` requires DataLoaded and a
  observed frame. The nested KCSE checkout is byte-for-byte at the same commit
  as its upstream. No fork-specific order inversion was found.
- **Actual cause:** initializer spawn-selection plus incorrect interpretation
  of `rollback_succeeded=false` when `rollback_attempted=false`.

## Failure return behavior

Join/runtime failures no longer call `EndGameContext` directly from KCSE's
`PostUpdate` callback. The client queues CryEngine's native `unload` command in
the deferred console FIFO, waits until both the game context and client entity
are gone, and then opens the native root main menu. Expected unload lifecycle
events do not invalidate the stored failure cause. The native multiplayer page
opens automatically and renders a literal English heading and the original
English error text (also as its tooltip).

## Follow-up: joining an occupied server

`ServerAccepted` contains snapshots for players and server dummies that are
already present. Materializing one of those snapshots creates a native Human,
then replaces its Soul and waits several frames for the native inventory and
equipment managers to stabilize. The remote-avatar ABI probe survived this
same sequence because it immediately hid and deactivated its temporary Human.
A real remote Human, however, previously enabled physics, rendering, and Actor
updates directly in `spawn()`, before the shared-Soul transaction completed.
This exposed a partially materialized Actor to an engine update between join
frames and only occurred when the accepted player list was non-empty.

Remote Humans now start with physics disabled, hidden, and inactive. Their
display name, shared Soul, transform, inventory, and appearance are prepared
while staged. Only after native readiness and a successful appearance
transaction are they presented atomically; activity and locomotion begin on the
following frame. A presentation failure leaves the entity hidden and is routed
through the normal English connection-error return instead of ticking a partial
Actor.

## Follow-up: manual disconnect

The native Disconnect button no longer captures the player profile from its UI
callback. It only changes the client to `Closing` and records a shutdown
request. On the next KCSE game-thread frame, the client optionally captures and
queues the final profile update, then queues the reliable transport close.
Remote-avatar synchronization and server corrections accept only `Connected`,
so no native multiplayer mutation can race the shutdown. `Closing` immediately
starts the same deferred `unload` transition used for handled join failures;
after the game context is gone, KCD2Online opens the native main menu. No quit or
process-termination command is issued.

## Trace output and interpretation

`KCD2Online-join.log` is written next to `KCD2OnlineKCSEClient.dll`. Every line is
flushed immediately and contains:

- wall-clock timestamp;
- join sequence ID;
- process/thread ID and logical thread role;
- source filename, line, and function;
- one event and its parameters/result.

Important last-line mappings:

| Last durable event | The next operation / likely fault |
|---|---|
| `join.remote-spawn.engine-call.begin` | `IActorSystem::CreateActor` |
| `join.remote-spawn.engine-call.returned` | `IActor::GetEntity` |
| `join.remote-spawn.EnablePhysics.begin` | staging disables native physics |
| `join.remote-status.ApplySharedSoul.begin` | fork shared-Soul materialization |
| `join.entity.SetWorldTM.begin` | `IEntity::SetWorldTM` |
| `join.remote-animation.first-locomotion` | fork movement request |
| `join.remote-appearance.CreateItem.begin` | fork inventory item creation |
| `join.remote-animation.weapon-action.begin` | fork Human weapon action |
| `join.remote-presentation.begin` | fully materialized remote Human is being activated |
| `join.remote-presentation.complete` | remote Human is active, visible, and physical |
| `join.sandbox.spawn-selection.ok` | selected source and exact transform |
| `join.sandbox.target-profile.invalid` | target rejected before native mutation |
| `join.sandbox.profile-apply.failed` | includes attempted/succeeded rollback and unload decision |
| `join.sandbox.unload.command.queued` | deferred engine map unload is waiting in the console FIFO |
| `join.sandbox.unload.complete` | game context and client entity are gone |
| `join.sandbox.main-menu.opened` | native root main menu is visible |
| `join.sandbox.OpenMainMenu.seh` | guarded native main-menu open raised SEH and will be retried |
| `join.kcse-post-update.seh` | caught engine-side SEH; includes code/address |

For the verification reproduction, the expected initializer sequence is
`spawn-selection.ok source=local.engine-default`,
`target-profile.valid`, `profile-apply.ok`, and `sandbox.native.ready`. Preserve
the join log together with any game crash dump/callstack and `KCSE.log`.
