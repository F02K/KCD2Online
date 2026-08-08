# Native vendor integration

KCD2Online includes [F02K/libKCD2](https://github.com/F02K/libKCD2) as the
`libKCD2` Git submodule. The superproject pins the exact libKCD2 commit used by
the runtime. libKCD2 in turn pins
[F02K/KCSE-for-kcd2](https://github.com/F02K/KCSE-for-kcd2) as its nested
submodule. KCD2Online also pins
[F02K/Address-Library-For-KCSE](https://github.com/F02K/Address-Library-For-KCSE)
at `vendor/Address-Library-For-KCSE`.

Clone KCD2Online and initialize all vendor repositories:

```powershell
git clone https://github.com/F02K/KCD2Online.git
Set-Location KCD2Online
powershell -ExecutionPolicy Bypass -File tools/init_vendor.ps1
```

The helper initializes libKCD2 and the Address Library, configures the nested
KCSE dependency to use the F02K HTTPS fork, and initializes KCSE at the commit
pinned by libKCD2. It is safe to run again after pulling KCD2Online.

`git clone --recurse-submodules` also initializes the complete vendor tree.

Update libKCD2 deliberately:

```powershell
git -C libKCD2 fetch origin
git -C libKCD2 checkout <tested-libKCD2-commit>
git add libKCD2
```

The `libKCD2` checkout must remain clean. KCD2Online defines the `kcd_re` and
`KCSE` build targets in its root `CMakeLists.txt`, so no KCD2Online-specific patch
is applied inside the vendor repository.

Update the Address Library with the build TUI's **Update Address Library**
button (or `Ctrl+L`). The updater refuses a dirty checkout and non-fast-forward
history, validates every KASL file, and audits all native `REL::ID` call sites
against the Steam, GOG, and Epic tables. It leaves the new submodule pointer as
a normal superproject change so it can be reviewed, tested, and committed.

CMake copies every `kcd_addresslib_*.bin` table from the pinned submodule to
the runtime artifact directory. The deploy action then installs all tables to
`<game-root>/KCSE/addresslib`; KCSE selects the matching distribution and build
at runtime. The legacy frontend's signature registry remains separate because
it resolves modloader hooks, while the libKCD2/KCSE native paths use
`REL::ID`/`REL::Relocation` and the Address Library.

The KCD2Online-specific KCSE plugin lives in `src/kcse/plugin.cpp`. It exposes a
versioned C ABI to the independently loaded KCD2Online DLL; no C++ standard-library
objects or allocator ownership cross the DLL boundary.
