# Native-game server mode

`[simulation].mode = "native_game"` selects a single server-owned KCD2
process as the source of native world simulation. It is a wire-visible mode,
not a presentation setting for the standalone dedicated server.

## Configuration and discovery

```toml
[simulation]
mode = "native_game"
game_root = ""
auto_find_game = true
hide_game_window = true
startup_timeout_seconds = 180
```

An explicit `game_root` points at the `KingdomComeDeliverance2` directory. The
binary directory and `KingdomCome.exe` itself are also accepted and normalized.
With automatic discovery enabled, the launcher reads Valve's registered Steam
root, `libraryfolders.vdf`, and app manifest `1771300`; it does not accept an
unmanifested directory merely because it has a familiar name. A valid install
must contain both `KingdomCome.exe` and `WHGame.dll` below
`Bin/Win64MasterMasterSteamPGO`.

After the public listener is ready, the server starts exactly one game process
with its binary directory as the working directory. It supplies the selected
server endpoint, config path, level, and `native_game_host` role through private
process environment values. `hide_game_window = true` uses a hidden startup
state and keeps any subsequently created top-level game window hidden, leaving
only the server console visible. Stopping the server first asks that owned game
process to close and terminates it only if it does not exit within five seconds.

The selected mode is sent in both `ServerChallenge` and `ServerBootstrap`.
Clients in native-game mode remain observers of NPC, world-object, and
world-item state. They do not send discovery, lease, door/container, or loose
item updates. The server also refuses those client messages and does not assign
player leases. Loss or absence of the native simulation host is therefore
fail-closed instead of silently falling back to client authority.

## One process, multiple anchors

Native-game mode uses one KCD2 process. Every connected player contributes a
simulation anchor containing its authoritative transform and stable player ID.
The native host must project that set into two distinct engine mechanisms:

1. level/entity streaming positions, so terrain and authored Entities remain
   materialized around every player;
2. scheduler roots, so NPC behavior selection remains player-relative around
   every player rather than only around the hidden local host actor.

The two mechanisms must be probed independently. CryEngine precache points can
help resource availability but are not evidence that KCD2's entity streaming or
NPC scheduler accepted a new player root.

## Reverse-engineering baseline

The following observations are tied to supported Steam WHGame build
`1308617_856` and must be fingerprint-gated before they become writable hooks:

- `wh_ai_PlayerSchedulerProxy` is stored at string RVA `0x3FF2658` and its CVar
  storage is registered at RVA `0x491E970`.
- `wh_ai_PlayerHorseSchedulerProxy` is stored at string RVA `0x3FF2780`; its
  CVar storage is at RVA `0x491E978` and the observed getter is at RVA
  `0xA6B990`.
- The level-streaming update path logs `Updating level streaming priorities for
  %zu cameras (LevelFrameId = %d)` from RVA `0x9D5E3F`.
- In that path, the camera-position collection begins at object offset `+0x670`,
  ends at `+0x678`, and is iterated with an observed element stride of `0x34`.
  The current binary calls RVA `0x42F7DC` for entries in the second pass.
- The public CryEngine interface also exposes `I3DEngine::AddPrecachePoint`, but
  this is considered a resource-precache probe only until entity and scheduler
  activity are demonstrated at the same anchor.

The next safe runtime probe is read-only: hook the multi-camera streaming update,
record the native camera array and compare it with loaded Entity/NPC activity
while moving a synthetic anchor. A write path may be enabled only after the
container owner, lifetime, capacity rules, and downstream consumers are
confirmed. Scheduler work should then trace creation and link ownership of the
two protected Vanilla proxy Entities and determine whether one system accepts a
set of proxy roots or whether proxy instances must be cloned with independent
link graphs.

## Readiness contract

The public listener may authenticate clients while KCD2 starts, but it must send
`BOOTSTRAP_MODE_WAIT` until all of these are true:

- supported WHGame fingerprint and Address Library are active;
- the configured level reached `DataLoaded`;
- the native host control channel is authenticated;
- at least one streaming anchor was accepted by both streaming and scheduler
  probes; and
- NPC observation advances from the native host without a player lease.

The server must freeze native-dependent mutation and report the host failure in
`status` if any invariant regresses.

## Current implementation boundary

The configuration, discovery, single-process launcher, hidden-window lifecycle,
wire negotiation, client observer behavior, and fail-closed server authority
rules are implemented. The native host control connection deliberately cannot
mark the process ready yet: the streaming-camera container and scheduler proxy
ownership are still in the fingerprint-gated, read-only reversing phase above.
Consequently, the experimental mode will time out instead of admitting players
until that channel and both anchor probes are implemented. This prevents an
apparently working server from silently running NPCs or items on player clients.
