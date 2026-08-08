# Table: rom.core

## Fields (1)

### `is_early_main`

Whether or not the early main Lua VM is running.
This Lua VM is there to allow Lua code to be injected into the game process before the CryEngine vanilla game Lua VM is init.
Some KCD2Online Lua api functions need to be executed under this early VM to do anything significant.
These functions are documented when such usage is needed and there are examples in the TestMod example mod files.

- Type: `boolean`

