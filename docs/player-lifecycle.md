# Player lifecycle, respawn, and combat authority

This document defines the ownership and state boundaries that player death,
respawn, PvP, knockouts, and paired stealth actions must preserve.

## Native ownership invariant

The local player is adopted from `CCryAction::GetClientEntity()` and resolved
through the game's ActorSystem. A lifecycle transition must keep all of these
identities stable:

- client `IEntity` pointer and EntityId;
- native `C_Player` pointer;
- `C_Soul` pointer and Soul GUID;
- Shared-Soul GUID; and
- the process-global PlayerModule and PlayerManager ownership.

Death, unconsciousness, revival, and appearance changes are state transitions
on this graph. They are not reasons to remove the Entity or create a second
`Player` actor. Remote players remain Human/NPC representations and must not
receive local input, camera, or PlayerModule ownership.

## In-place respawn transaction

The native runtime performs respawn on the game thread in this order:

1. capture the complete local Entity/Actor/Soul identity;
2. verify the ActorSystem and CryAction client mappings agree;
3. release any interrupted local minigame/activity handle;
4. invoke `player.actor:Revive(false)` on the existing `C_Player`;
5. restore maximum health and verify that the actor is alive and conscious;
6. verify that the Entity, Actor, Soul, Soul GUID, and Shared-Soul GUID did not
   change;
7. apply the server-authoritative spawn transform with native readback;
8. mirror Vanilla's initial-spawn `PlayerSetViewAngles` step;
9. verify life state and identity again; and
10. hide the native game-over screen.

Ragdoll, collision, input, interaction, and other Player-owned controller state
is left to the audited native `C_Player::Revive` path. Extra manual resets must
only be added when a live trace proves that a specific subsystem remains stale;
blindly resetting individual controllers can conflict with the engine's death
and end-combat transitions.

## Authoritative life states

PvP must replace the current pair of unrelated `dead` booleans with a
revisioned server-owned state:

```text
Alive ───────────────→ Unconscious ─────────→ Alive
  │                         │
  └──────────────→ Dead ←───┘
                         │
                         ↓
                    Respawning
                         │
                         └──────────────→ Alive
```

Recommended wire states are `ALIVE`, `UNCONSCIOUS`, `DEAD`, and
`RESPAWNING`. Each state update needs:

- a monotonic life revision;
- the server tick at which the transition became effective;
- canonical health and stamina;
- the combat/event ID that caused the transition;
- an optional wake-up tick for unconscious players; and
- an optional respawn transform while `RESPAWNING`.

Snapshots make late join and packet-loss recovery possible. One-shot combat
events remain separately sequenced and idempotent; replaying a snapshot must
never replay damage or a takedown.

## PvP event flow

The owning client sends an action intent, not a claim that another player took
damage. An intent identifies the attacker, target, action kind, weapon set,
attack direction, local action sequence, client tick, and sampled transforms.

The server then:

1. rejects stale, duplicate, impossible, or disallowed intents;
2. rewinds bounded transform history to the claimed tick;
3. validates range, facing, line of sight, weapon state, action timing,
   cooldowns, and both players' life states;
4. resolves a canonical result with one event ID;
5. updates health, stamina, and life state atomically; and
6. broadcasts the result reliably.

The target applies the result through native combat/Soul damage APIs. Its later
health observation confirms the application but does not override the server's
decision. Attack and hit animation fragments are presentation attached to the
authoritative event; they are never accepted as proof of a hit.

## Unconsciousness and knockouts

Unconsciousness is not low-health death and must not use the respawn path. The
server transitions `ALIVE -> UNCONSCIOUS`, blocks movement and new combat
intents, and supplies either a wake-up tick or a later explicit wake event.

The owning client must enter and leave the native PlayerUnconscious controller
through an audited game API. Remote representations receive the semantic state
and corresponding authored presentation. If no safe native entry point is
available, the feature remains disabled rather than being approximated by a
permanent ragdoll.

Damage received while unconscious can transition the same life revision to
`DEAD`; waking can only transition to `ALIVE`. A respawn request is valid only
from `DEAD`.

## Stealth kills and choke-outs

Stealth actions are paired, server-granted sessions rather than ordinary attack
fragments. Before granting one, the server validates:

- attacker and target are alive and not already in another paired action;
- distance, rear arc, vertical difference, and line of sight;
- the target is not actively defending or otherwise immune;
- required weapon/perk/ruleset conditions; and
- both transforms can be aligned without crossing blocked geometry.

After both clients acknowledge that the paired action is ready, the server
commits one of two results:

- stealth kill: target life state becomes `DEAD`; or
- choke/knockout: target life state becomes `UNCONSCIOUS`.

The commit carries the animation variant, aligned transforms, start tick,
duration, and result event ID. Disconnect, timeout, damage from a third party,
or invalidated geometry cancels the uncommitted session. Once committed, the
life transition is authoritative even if one client misses the presentation.

## Implementation order

1. Add the revisioned player life snapshot to `PlayerSnapshot` and world
   snapshots.
2. Replace empty `ClientDeath` trust with server-owned life transitions while
   retaining native observations for reconciliation.
3. Add idempotent combat intent/result IDs and bounded rewind validation.
4. Apply ordinary damage through the existing native combat/Soul boundary.
5. Audit the native PlayerUnconscious entry/exit path and implement knockout.
6. Add paired-action sessions for stealth kill and choke-out.
7. Add live two-client tests for death during combat, unconscious wake-up,
   interrupted paired actions, late join, reconnect, and respawn.

