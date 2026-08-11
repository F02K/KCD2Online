# Resource delivery and cache

[Back to scripting overview](../README.md)

KCD2Online downloads client resources as part of the normal server join. Server operators do not need an HTTP host, separate Lua runtime, or custom downloader.

## Join sequence

1. The client completes the normal version and account handshake.
2. The server sends a resource manifest containing the generation, root SHA-256 hash, and metadata for every resource with `[client]`.
3. The client checks its content-addressed local cache.
4. Missing packages are requested in 48 KiB chunks over the existing encrypted, reliable GameNetworkingSockets connection.
5. The client verifies the complete package hash and then every contained file hash.
6. Verified packages are stored atomically and activated.
7. The client acknowledges the generation and root hash.
8. Only then does the server send the world bootstrap.

Invalid paths, mismatched hashes, incorrect chunks, oversized content, or an inconsistent acknowledgment abort the join.

## What gets downloaded

| Resource contents | Downloaded? |
| --- | --- |
| Resource without `[client]` | No package is advertised |
| `server/**` | Never |
| `client/**` | Yes, when `[client]` exists |
| Paths listed by `[shared].client_paths` | Yes, when `[client]` exists |
| Unlisted `shared/**` files | No |
| `resource.toml` source file | Not as a raw file; required client metadata is encoded in the package manifest |

Server-only changes deliberately do not change the client package hash. A client entry, client module, allowlisted shared file, event declaration, dependency, version, or other encoded client metadata can produce a new hash.

## Cache location

```text
%LOCALAPPDATA%\KCD2Online\resources
```

Packages are named by SHA-256 content hash and may be reused across servers when their content is identical. A separate activation record maps a server to its currently confirmed root and package hashes. Corrupt or locally changed cache blobs fail verification and are downloaded again.

## Deploying an update

1. Stop or drain the dedicated server.
2. Replace or edit the resource directory.
3. Start the server and check the console for manifest or Lua errors.
4. Reconnecting clients automatically receive changed client packages.

Live resource hot reload is not supported. Already connected clients retain their current resource state until they disconnect.

## Server ZIP layout

The packaged dedicated server includes:

```text
KCD2OnlineServer.exe
server.toml.example
resources/
  event_example/
  welcome_ui/
docs/
  scripting/
    README.md
    ...
```

The complete scripting documentation and runnable examples are included recursively. A server operator needs no repository checkout, compiler, SDK, KCSE installation, or separate Lua installation.

## Source visibility

Hashing protects integrity during normal caching; it does not hide client code. Any code that executes on a player's computer can be inspected or replaced by that player. Obfuscation does not change this trust boundary. Keep secrets and authoritative logic exclusively on the server.
