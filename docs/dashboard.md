# Dedicated-server dashboard

The KCD2Online server includes a lightweight, read-only operations dashboard.
It is embedded in `KCD2OnlineServer.exe`, so it does not depend on a separate
web runtime, external assets, analytics scripts, or a cloud service.

## First start

The packaged `start_server.bat` creates `dashboard.toml` from
`dashboard.toml.example`. The default configuration:

- enables the dashboard at `http://127.0.0.1:8080`;
- creates a cryptographically random token in `dashboard-token.txt`;
- requires that token for every telemetry request;
- refreshes once per second; and
- limits each source address to 240 requests per minute.

Open the address on the server machine and paste the token when prompted. The
token is kept in the browser tab's session storage and disappears when the tab
is closed.

## Configuration

The dashboard deliberately uses its own file, separate from `server.toml`:

```toml
[dashboard]
enabled = true
bind_address = "127.0.0.1"
port = 8080
allow_remote = false
require_token = true
token_file = "dashboard-token.txt"
refresh_interval_ms = 1000
max_requests_per_minute = 240
```

Relative token paths are resolved next to `dashboard.toml`. The refresh
interval must be between 250 milliseconds and 60 seconds. The server rejects
remote binds unless `allow_remote = true`, and it never allows a remote bind
without token authentication.

## Security boundary

The embedded endpoint provides only static dashboard assets and one aggregate
telemetry snapshot. It has no command, moderation, configuration-write, file,
or console API. Snapshots omit player names, account IDs, IP addresses, tokens,
world contents, chat, and server/backend credentials.

Responses disable caching, framing, MIME sniffing, referrers, browser device
permissions, cross-site access, and all content sources except the dashboard
itself. Requests have a small header limit, short read timeout, method allowlist,
constant-time token comparison, and per-source rate limiting.

The embedded listener does not provide TLS. For access from another machine,
prefer a private VPN. An HTTPS reverse proxy is also suitable when it limits
access to trusted operators. Do not expose the HTTP port directly to the public
internet, and never publish `dashboard-token.txt`.

## Performance model

The HTTP listener runs on a separate thread. The game-server thread publishes a
small immutable JSON snapshot at the configured interval; web requests only
copy that prepared snapshot. No dashboard request calls into the authoritative
world simulation. Connection quality and lane-queue statistics are sampled at
the same interval rather than for every packet.

## Available telemetry

In addition to player, tick, latency, packet-loss, traffic, and queue data, the
dashboard reports operational resource pressure for the dedicated-server
process and its Windows host:

- normalized CPU utilization for the server process and total host CPU;
- process working set, private memory, and peak working set;
- total, available, and used host memory;
- accumulated process CPU time, process ID, handle count, and logical cores;
- current tick-budget utilization and the accumulated number of tick-budget
  overruns.

CPU and memory counters are sampled only when the immutable dashboard snapshot
is refreshed. Server CPU is normalized across all logical processors, matching
the whole-machine percentage shown by Windows Task Manager.
