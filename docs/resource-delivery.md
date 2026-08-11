# Loading and distributing server resources

After the normal version and account handshake, the server sends a manifest containing the generation, root SHA-256 hash, package sizes, and hashes of all client resources. The client checks its local content-addressed cache and requests only missing packages, in 48 KiB chunks, over the existing encrypted and reliable GameNetworkingSockets channel.

Each complete package is hashed, and every contained file is then checked against its own hash. Only after these checks does the client store and activate the package atomically. The client acknowledges the generation and root hash; the server sends the world bootstrap only after receiving that acknowledgment. A mismatched hash, incorrect sequence, oversized package, or invalid path aborts the join.

The cache is stored under `%LOCALAPPDATA%\KCD2Online\resources`. Packages are named by SHA-256 hash and can be reused across servers. An activation file maps a server only to its currently confirmed hashes. Changes produce new hashes and are downloaded automatically on the next join.

The client does not need a separate Lua installation. The server release contains the statically linked server runtime; the standard client release contains the client runtime and ImGui renderer. A server operator needs only the contents of the server ZIP: the executable, configuration, `resources/`, documentation, and locally generated `game_data`.

Server code is never transferred. Client code must necessarily be executable on the client and can therefore always be inspected. Obfuscation is not a security boundary. Keep secrets and authoritative rules exclusively under `server/`.

## Updates and operations

- Changes under `client/`, changes to allowlisted `shared/` files, or client manifest changes generate new package and root hashes.
- Server-only changes deliberately do not trigger a new client download.
- Restart the dedicated server after making a change. Connected players retain the old state until they disconnect; live resource hot reload is not supported.
- Corrupt or locally modified cache blobs are discarded and downloaded again during the next join.
- A resource directory without `[client]` does not appear in the download manifest.

The server ZIP includes `resources/` with examples and `docs/` with these guides. Copy production resources directly into that directory; no repository checkout or build system is required.
