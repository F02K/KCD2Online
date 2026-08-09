# KCD2Online account client

The KCD2Online account is a technical multiplayer identity. It has no display
name, public profile, password, e-mail address, Steam ID, Discord account, or
social features. Player names remain server-scoped and are sent only when a
specific game server is joined.

## Consent and menu flow

The account flow is rendered with KCD2's native Scaleform menu API. It does not
use the ImGui overlay.

On the first visit to **Multiplayer**, the client explains what the account is
used for and offers these choices:

- **Enable** creates the cryptographic identity and registers it with the
  configured account service.
- **Decline** records the choice locally. No device evidence is derived and no
  account-service request is made. Multiplayer remains unavailable.
- A declined choice can be changed later from the same native menu.

Registration runs outside the game/UI thread. The menu updates automatically
when it completes and offers retry handling for network or service failures.
The full account ID can be copied to the Windows clipboard; only an abbreviated
form is shown in the normal menu.

## Local identity

After consent, the client creates a P-256 signing key and derives versioned
device evidence from Windows and firmware identifiers. The raw identifiers are
not sent to the service; only their SHA-256 digest is submitted during account
creation.

The account ID, credential ID, private signing key, and service registration
data are stored at:

```text
%LOCALAPPDATA%\KCD2Online\account.bin
```

The complete record is protected with Windows DPAPI for the current Windows
user and is replaced atomically. Declining the service later disables online
use but deliberately retains this encrypted local identity, so enabling it
again does not silently create another account.

## Transport and configuration

The client uses WinHTTP with bounded timeouts and response sizes. HTTPS is
required for remote hosts. Plain HTTP is accepted only for `localhost`,
`127.0.0.1`, and `::1` development instances.

The default account-service origin is:

```text
https://api.kingdom-online.cc
```

It is exposed as `Multiplayer / Account Service URL` in the client settings so
the private backend can still be replaced by a local development instance.
Production packages use the official HTTPS origin by default. They should stop
exposing arbitrary service replacement once official-service dependency becomes
part of the multiplayer trust model.

## Authentication boundary

The account client implements registration and challenge-based login. Login
signs a server-provided nonce with the local P-256 key and returns a short-lived
access token for a requested audience.

The native menu currently gates ordinary direct-connect attempts on a ready
KCD2Online account. Dedicated-server token verification is not yet connected to
the existing multiplayer handshake. Until that follow-up is implemented, the
menu gate is a client UX rule rather than a security boundary: a modified client
or an older protocol implementation cannot be rejected by the account service
at the dedicated server.

The server-side follow-up should:

1. obtain an audience-specific access token immediately before connecting;
2. include it in the authenticated client hello;
3. validate it through the private service (or a verifiable signed-token key);
4. use the service account ID as the stable multiplayer player ID; and
5. fail closed when the token is invalid, expired, or issued for another
   audience.
