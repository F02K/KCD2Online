# Controlled NPC API

The former Lua-backed controlled-NPC API was removed in protocol/runtime v4.
It used Game Lua for XGen spawning, inventory and equipment, and therefore is
not a valid implementation source for the native multiplayer client.

Remote players now belong exclusively to `KCD2OnlineKCSEClient.dll`. A replacement
general-purpose mod API is intentionally not exposed. The multiplayer runtime
uses the complete internal native lifecycle:

`spawn → Actor/Soul/Human readiness → transform/motion → equipment → remove`

Raw `IEntitySystem::SpawnEntity`, Entity names, and Game-Lua calls are not
accepted as substitutes. The KCSE client actively probes the native path and
reports a concrete lifecycle error if any stage fails.

The Human Soul catalog remains available as read-only data for UI and server
allowlist validation. Native Entity IDs, WUIDs, and pointers never cross the UI
ABI or network protocol.
