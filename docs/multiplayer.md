# Multiplayer architecture and prototype status

This document describes KCD2Online **v0.1.5**. The implementation is an active
prototype and is not intended for production servers or valuable saves.

## Versioning and compatibility

KCD2Online has one semantic project version shared by:

- the native client;
- the dedicated server;
- Windows build metadata;
- packaged artifacts; and
- the multiplayer handshake.

The current version is `0.1.5`. There is no separate public protocol or KCSE C
ABI version. The KCSE query boundary reads the same generated major, minor, and
patch components as the rest of the project. During the prototype phase, all
components must match exactly; a mismatch is rejected before authentication or
world loading begins.

Internal formats such as the persistence schema and Address Library remain
independently versioned implementation details.

Wire messages still evolve with the project, but their compatibility boundary
is the KCD2Online version. This avoids a numeric wire version carrying a different,
unrelated application version. Version changes are recorded in the
project [changelog](../CHANGELOG.md).

## Connection lifecycle

A normal connection follows this sequence:

1. `ClientHello`
2. `ServerChallenge`
3. `ClientAuthenticate`
4. `ServerBootstrap`
5. `ClientWorldReady` or `ClientWorldFailed`
6. `ServerAccepted`

`ClientHello` contains the KCD2Online version and native runtime descriptor. The
server verifies the game fingerprint, KCSE/libKCD2 release, Address Library,
required capabilities, level, and content hash. Only control traffic is
accepted while the client loads; gameplay updates start after
`ServerAccepted`.

The client loads a save through KCD2's own UI before joining. KCD2Online adopts the
already loaded world only when its level matches the server. It does not load,
copy, upload, or modify native save files.

## Transport lanes

Each connection configures five outbound GameNetworkingSockets lanes. Chat is
the only strictly prioritized lane. Reliable protocol state remains in one
ordered lane, while absolute player and NPC motion use separate unreliable
lanes with weighted bandwidth sharing. This keeps chat and player movement
responsive during NPC bursts without weakening the ordering guarantees used by
handshake, entity lifecycle, inventory, and world transactions.

The lane mapping is symmetric but configured independently in each direction:

- `interactive`: reliable chat send/broadcast traffic;
- `ordered_state`: all other reliable and causally ordered messages;
- `player_realtime`: client transforms and server world snapshots; and
- `npc_realtime`: NPC authority update batches and server NPC motion.
- `voice_realtime`: unreliable, no-delay Opus voice frames and synchronized
  viseme weights.

Lanes share one connection and congestion controller; they prioritize queued
traffic but do not create bandwidth. Realtime queues use per-lane backpressure
so NPC congestion does not cause player-motion packets to be discarded.

## Proximity voice

Hold `V` for normal speech, `Ctrl+V` to whisper, or `Shift+V` to shout. The
client captures the Windows communications microphone through WASAPI, converts
it to 48 kHz mono, and sends one Opus VOIP frame every 20 milliseconds. Silence
is not transmitted when push-to-talk is released.

The server derives the sender from the authenticated connection and selects
recipients from authoritative transforms. Default voice radii are 3, 15, and
40 metres and can be changed in `[voice]`. Voice packets are rate limited and
never enter the reliable state lane.

Receivers keep an adaptive three-to-six-frame jitter target, use Opus packet-loss
concealment for gaps, and feed decoded PCM into a lock-free FMOD user stream.
Each speaker owns one 3D channel routed through `bus:/dieg/w_obj`; its position
tracks the remote avatar's head. Fifteen quantized viseme weights share the
audio frame sequence. The current safe presentation maps speech energy onto
KCD2's `facial_chewing_01` transition animation; direct phoneme injection into
the retail `LipSync_TransitionQueue` remains deliberately disabled until its
KCD2 1.5.6 ABI has been verified.

## Roleplay chat and emotes

Normal messages are local `/say` speech. The server selects recipients from its
authoritative transforms; clients cannot claim their own chat radius. Supported
commands are `/w` or `/whisper`, `/s` or `/say`, `/y` or `/shout`, `/me`, `/do`,
and the optional global `/ooc` channel. Default radii are 3, 15, and 40 metres
and can be changed in `[chat]` without changing the protocol.

Holding `G` opens the in-game emote wheel. Its initial audited catalog contains
the non-looping player fragments `Greetings` (bow), `Soldier_Cheers` (cheer),
`CrowdPeasantPoint` (point), and `NoWeaponSurrender` (surrender).
The ABI accepts only the catalog enum, never a caller-supplied Lua snippet or
animation fragment. The existing animation observation and replication path
then presents that one-shot to nearby remote avatars.

## Server authority and persistence

The dedicated server owns the canonical multiplayer state. Its
`world_directory` contains:

- `session.toml` for server/session identity, world configuration, spawn, and
  revision metadata;
- `players/<player_id>.pb` for authenticated player profiles;
- `world_objects.pb` for doors and synchronized containers; and
- `world_items.pb` for dropped-item state and tombstones;
- `permissions.json` for persistent UUID-to-scope grants; and
- `admin-audit.jsonl` for append-only GM action records.

The `[permissions].owners` UUIDs implicitly receive `*`. Other grants support
exact scopes and suffix wildcards such as `admin.*`. Available GM scopes are
`admin.players`, `admin.announce`, `admin.kick`, `admin.teleport`,
`admin.freeze`, and `admin.permissions`. They guard `/players`, `/announce`,
`/kick`, `/goto`, `/bring`, `/freeze`, `/unfreeze`, and `/perm` on the server.
The dedicated console can bootstrap access with `permission grant <player_id>
<scope>`; console and in-game changes are audited.

When central account authentication is enabled, token introspection also returns the
current network role. Supporters, moderators, admins, and owners bypass the server
password, player limit, and backend-managed server membership ban. The server copies
the role into authoritative player snapshots and chat broadcasts so clients can show
a non-spoofable badge. Network admins and owners additionally receive effective `*`
permission without an entry in `permissions.json`. Version, game-build, content, and
protocol checks are never bypassed.

Servers can opt into the backend-managed membership whitelist with
`[auth].whitelist_enabled = true`. Regular accounts then need `whitelisted=true` in
their server membership record; network staff retain their join bypass.

Persistence uses a temporary sibling followed by atomic replacement. Player
IDs, object identities, item instance UUIDs, and revisions survive restarts.

Identity tokens contain 256 random bits. The client stores them with Windows
DPAPI; the server stores only their SHA-256 hashes. Recovery codes are
single-use, expire after ten minutes, and are printed only when explicitly
requested by an administrator. The token or recovery code identifies the
player; the display name is mutable. Supplying a new available display name
during authentication updates the same persistent profile and does not create
a new identity.

## Player profiles

An authoritative player profile currently includes:

- money;
- 10 canonical stats;
- 35 skills and progress values;
- inventory instances;
- quick-access assignments;
- equipment slots; and
- avatar appearance and weapon state.

Profiles use optimistic revisions. A central server item ledger assigns every
live instance to exactly one player, synchronized container, or world location.
Malformed values, stale revisions, definition changes on an existing instance,
unsourced new UUIDs, stack inflation, or duplicate ownership are rejected.
Stack growth is accepted only when the same transaction identifies matching
source stacks; split stacks receive a new UUID and leave a non-empty source.
The client applies the returned canonical state through the same native
reconciler used for capture. Failed mutations roll back; a failed rollback
initiates a safe world unload.

Weapon presentation distinguishes the native primary, secondary, and oversized
sets. A drawn empty primary set is transmitted as `UNARMED`, not as no weapon.
The owner also reports the native `CombatActor` combat-mode and active-combat
flags. These values are observations: authoritative profile reconciliation does
not write draw, holster, or combat state back onto the owning player and cannot
interrupt native end-combat transitions.

Quests, dialogue, reputation, crimes, perks, horses, buffs, nutrition, fatigue,
and general savegame state are outside the current profile contract.

## Remote players

Remote players are represented by native actors managed behind the KCSE runtime
boundary. The lifecycle is:

1. allocate the representation;
2. register it as an isolation exception;
3. wait for Entity, Actor, Soul, Human, and Inventory readiness;
4. apply appearance and equipment;
5. consume interpolated movement snapshots; and
6. transactionally remove all native state on leave or disconnect.

Movement snapshots carry world velocity plus local velocity, acceleration,
facing, turn rate, strafing intent, and walk/run/sprint classification. The
remote Actor feeds these values to KCD2's audited movement controller so the
game selects and blends locomotion. Interpolated world transforms are recovery
anchors: small divergence is corrected with exponential damping, while errors
above five metres snap to the authoritative position.

The local Actor's current Mannequin fragment is observed and sequenced. Safe
non-combat one-shots (for example jump, gesture, and interaction fragments) are
replayed through the native `C_ScriptBindHuman::PlayAnim`/`StopAnim` API.
Locomotion and combat-named fragments are rejected so they cannot fight the
movement controller or accidentally become an unauthoritative combat system.

Equipment changes are ordered, validated, and recoverable. Reconciliation is
incremental: unchanged native item instances remain equipped and only stale or
new slots are mutated. Draw and holster transitions use the native weapon-set
controller so KCD2 can play its authored transition instead of swapping a
detached visual. Avatar
materialization advances monotonically through Human, Soul, Soul-stabilization,
and Inventory readiness. A regression, timeout, or failed desired Soul fails
the client closed; the live runtime does not substitute the default Soul.

Exact combat replay is deliberately deferred. A later combat protocol must
carry authoritative action identity, phase/timing, target, hit validation,
interrupts, stamina, and damage results; the presentation-only Mannequin stream
must not be extended into combat by merely forwarding attack fragments.
The ownership-preserving respawn flow and the proposed server-authoritative
life/PvP state machine are specified in
[player lifecycle, respawn, and combat authority](player-lifecycle.md).

## Doors and loot containers

Doors and containers use a revisioned, server-authoritative world-object stream
keyed by stable Entity GUID. Their native implementation is isolated in
`native_world_object_sync.*`.

Container discovery covers:

- regular `Stash` containers and chests;
- `CartStash`;
- `StashCorpse`;
- bird nests; and
- destructible `ShootableStashBase` variants.

While a container is open, its complete item set is captured with instance and
definition UUID, stack count, quality, and condition. Revision conflicts return
the canonical state. Deposits and withdrawals commit the container revision,
ledger location, and authoritative player profile together. Whole-instance
moves preserve the UUID; unambiguous split and merge deltas preserve total item
count. The acknowledgement carries the resulting profile so a concurrent
periodic snapshot cannot reintroduce an item.

Unknown future script classes are not assumed to be compatible merely because
they are visually chest-like. They require an inventory accessor matching one
of the supported native paths or an explicit adapter.

## Dropped items

Version `0.0.9` adds a separate world-item stream keyed by the persistent item
instance UUID. Local CryEngine Entity IDs are never used as network identity,
because each game process creates its own visual/pickable Entity.

Each canonical world item contains:

- instance and definition UUID;
- count, quality, and condition;
- position and rotation;
- presence/tombstone state; and
- revision.

At synchronization start, the native client records pickups already present in
the loaded save as a local baseline. Those items remain local save content and
are not uploaded as multiplayer drops. They are deliberately not accepted as a
new authoritative profile instance without a synchronized container/world
origin; this prevents a local save from minting multiplayer items.

Remote creation uses the game's Human `PlaceItem` path so the normal world
inventory and pickable extension are established. Failed creation is rolled
back instead of leaving a partial item in the player's inventory. Pickup emits
a tombstone, and dropping the same UUID again reactivates its canonical record
instead of creating another identity.

Ownership transfer is instance-atomic at the server boundary:

- dropping removes the instance from the sender profile and any synchronized
  container before exposing it in the world;
- partial drops identify their source UUID and transfer count, create one new
  world UUID, and decrement the source stack in the same commit;
- pickup tombstones the world representation and updates the receiving profile
  in the same commit;
- definition, count, quality, and condition must match the ledger source; and
- a second player cannot retain an instance already owned elsewhere.

The client polls item observations before considering its periodic profile
snapshot. This ordering prevents a drop from being misclassified as item
consumption. Late acknowledgements and corrections are idempotent when a newer
authoritative revision has already arrived.

## Environment

The server owns monotonic Calendar world time, its real-time anchor, time scale,
weather, and associated revisions. Clients advance time locally and apply
forward-only corrections through the game's Calendar API. Admin changes and
shared sleep therefore retain day boundaries instead of moving RPG time
backwards.

Weather transitions use a separate revision so a new transition is not
restarted on every snapshot. Clients periodically reassert the authoritative
weather profile without a transition before the vanilla random-preset interval
can elapse. Weather still uses the game's native weather command so profile
blending remains intact.

## NPC synchronization

Shared-NPC replication synchronizes human and animal lifecycle, transform,
gameplay state, and simulation ownership. The generated `npc_world_catalog.json`
is the production-level allowlist: its authored Entity GUID becomes the stable
server NPC ID with generation 1. Runtime Entity IDs and pointers never leave a
client. Custom levels without a generated catalog may still adopt positively
classified authored GUIDs dynamically.

Runtime-spawned Actors are reported with their local GUID only as a discovery
token. A production server distinguishes authored catalog hits from runtime
spawns, allocates the latter a canonical high-range NPC ID, and sends the Actor
class/name descriptor to peers. Observers create a local Actor when no authored
Entity exists and bind that local Entity to the canonical ID; local runtime IDs
still never cross the network.

For production levels, the generated catalog seeds authored NPC identity,
class, initial transform, and a conservative gameplay state on the server
before clients connect. A client's first valid in-range observation updates the
catalog transform to the NPC's current streamed schedule position. Custom and
runtime actors continue through discovery. The server maintains 120-metre
enter and 150-metre leave radii, sends reliable enter/leave and lease changes,
and sends revisioned transforms in unreliable snapshots. At most one
interested client receives a two-second simulation lease. Valid transform
reports renew that lease; disconnects, interest changes, and expiry reassign it
to the nearest interested player. Stale lease updates are ignored.

The lease owner leaves the native NPC active so KCD2 can simulate its routines.
Observers adopt the same authored Entity, deactivate its local simulation, and
apply the server transform. Dynamic states also look up and adopt their local
discovery GUID before a spawn is attempted; the server maps a discovery token
globally so multiple reporters cannot create stacked identities. An
out-of-interest NPC is hidden and inactive, then reused when it re-enters
interest. Disconnect and world teardown restore the Entity's original
active/hidden state.

The lease owner also reports native health/death, combat mode, inventory, and
dialog participation. The server canonicalizes gameplay, inventory, and dialog
revisions. A health decrease becomes a monotonic combat result attributed to
the lease owner, raises that player's Aggro, and selects the player as combat
target; Aggro decays while the NPC is outside combat. Observers apply damage
through `C_CombatSoul::DealDamage`, reconcile inventory through native
`C_Inventory` operations, and establish a resolvable remote-player opponent
through `C_CombatActor::SetOpponent`.

The native hot path is tiered to protect frame time. Transform, health, and
combat state remain on the 200 ms lease-update path. Full Entity/RTTI roster
discovery runs at most once per second; observer NPCs are not sampled at all;
and only the lease owner reads an NPC inventory, staggered at five-second
intervals. Unchanged gameplay keeps its revision, inventory payloads are omitted
from ordinary snapshots and refreshed periodically, and snapshot size accounting
is linear instead of repeatedly serializing a growing envelope.

Behavior replication is intentionally semantic: idle, travel, investigate,
combat, flee, dialogue, and dead, with an optional locomotion target/speed.
Dialogue replication carries active session ID and phase. It does not serialize
process-local dialogue objects or force an unverified branch-selection API.

The server can independently disable human NPCs, animal NPCs, or both. Disabled
categories are removed from the registry and are not discovered or replicated.
Classification uses native
`C_Human` and `C_Animal` RTTI. Human removal additionally requires a registered
AI object; the local client Entity/Actor, every engine-recognized player, remote
player Entities, the native player/horse scheduler proxy Entities named by
`wh_ai_PlayerSchedulerProxy` and `wh_ai_PlayerHorseSchedulerProxy`, unknown
Actors, and non-AI helpers are always excluded.

Isolation prevents each client from independently presenting and simulating its
own unsynchronized population while retaining the Actor, Soul, scheduler, and
authored GUID ownership graph. A positively classified NPC is hidden instead of
being force-removed. If its native `CombatActor` is in combat mode, isolation is
deferred until the engine has completed its opponent and end-combat lifecycle.
The multiplayer layer does not stop player combat actions, clear the player's
opponent, or force a holster based on weapon stance or remote-player distance.

Raw behavior-tree nodes, full schedules, exact dialogue branch playback, crime,
reputation, quests, and story consequences remain outside this stage.

## Native safety boundary

The in-game client performs an active capability probe before publishing
readiness. It verifies transform access, Actor/Soul/Human/Inventory readiness,
item lifecycle operations, equipment transactions, Entity cleanup, runtime
epoch, and Address Library identity.

Game objects are accessed only on the game thread. Networking uses bounded
queues and plain protocol values. LoadGame, SaveGame, NewGame, and DataLoaded
events advance the runtime epoch and invalidate stale handles.

The client fails closed when a required native capability is missing. Removed
signature, generated-Lua, console, and guessed-name fallbacks are not used for
multiplayer state mutation.

## Automated verification

The test suites cover:

- handshake/version validation and malformed or oversized messages;
- authentication, identity recovery, capacity, reconnect, and timeouts;
- profile revisions, inventory ownership, reconciliation, and rollback;
- remote-avatar lifecycle phases, regression/timeout handling, strict
  no-fallback behavior, opt-in fallback diagnostics, and cleanup;
- door/container conflicts, persistence, and late-join replay;
- dropped-item revisions, ownership transfer, tombstones, and restart replay;
- environment validation and updates;
- NPC protocol validation, interest hysteresis, lease ownership/handoff, and
  disabled-category handling;
- bounded queues and reliable/unreliable networking;
- Address Library coverage and native signature resolution; and
- build-tool discovery, auditing, and atomic deployment.

## Known prototype limitations

- NPC synchronization remains unreliable, and a known identity/spawn issue can
  still cause the same NPC to spawn multiple times
- NPC dialogue sync exposes session/phase state but does not yet replay exact
  dialogue branches or cinematic timing on every peer
- No shared quests, schedules, crime, reputation, or story progression
- No compatibility promise between different KCD2Online versions
- One explicitly supported Steam/WHGame build
- Direct-IP hosting without matchmaking or relay service
- Remaining multi-client gameplay and long-duration checks require manual
  in-game validation

These limitations are release blockers, not hidden production features.
