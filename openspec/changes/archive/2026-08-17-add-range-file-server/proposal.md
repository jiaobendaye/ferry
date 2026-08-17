# Proposal: add-range-file-server

## Why

We need an HTTP file download server that supports HTTP Range requests (resumable, parallel-friendly downloads), with IP-based access control and per-IP bandwidth limiting. It will be built on the Sogou Workflow async framework (local source at `~/lab/workflow`, v1.0.1, already built) and compiled with xmake. A follow-up multi-threaded download **client** is planned, so the server's capped-range responses must be self-describing (total size always discoverable) and the repo layout reserves a `client/` directory.

## What Changes

- New HTTP server (Sogou Workflow `WFHttpServer`) serving files from a configured root directory, with:
  - Full Range semantics: `206` partial content, suffix/open-ended ranges, `416` with `Content-Range: bytes */N` for unsatisfiable ranges, multi-range requests served as the first range only.
  - Per-response byte cap (configurable, default 8 MiB): ranges are truncated to the cap (RFC 9110 permits shorter ranges). Non-Range requests for files larger than the size threshold return `413`; small files return `200` whole.
  - Every response carries `Accept-Ranges: bytes`, `Content-Length`, `Content-Range` (206/416) and `Last-Modified`, so clients always learn the full file size.
  - `HEAD` support (headers only, no body buffering).
  - Path traversal protection (URL decode + containment check under root).
- IP access control: blacklist takes priority over whitelist; CIDR entries for IPv4 and IPv6; rules hot-reloaded by polling the ACL file's mtime.
- Per-IP bandwidth rate limiting via token bucket: when tokens are insufficient the request is soft-shaped by inserting a timer task into the Workflow series (client just sees slowness); wait capped at 30 s, beyond which `429` + `Retry-After` is returned. Idle bucket entries are periodically reclaimed.
- Client IP resolution for deployment behind a proxy: real IP taken from the **rightmost** `X-Forwarded-For` entry (trusted-proxy side, spoof-resistant), with configurable trust hops; falls back to the socket peer address when no XFF header is present. No HTTPS (plain HTTP; TLS terminates at the proxy).
- Build system: xmake project integrating the prebuilt Workflow library via `_include`/`_lib`; server sources live under `server/` (binary target), root `xmake.lua` ready to include a future `client/`.

## Capabilities

### New Capabilities

- `range-file-serving`: HTTP file serving with full Range request semantics, response byte cap, 200/206/413/416 behavior, HEAD support, response metadata (Content-Length / Content-Range / Accept-Ranges / Last-Modified), and path-safety checks.
- `ip-access-control`: IP blacklist/whitelist enforcement (blacklist priority), CIDR matching for IPv4/IPv6, hot reload via mtime polling of the ACL file.
- `bandwidth-rate-limiting`: per-IP token-bucket bandwidth limiting with soft shaping through Workflow series timers, bounded wait then 429, and idle-entry cleanup.
- `client-ip-resolution`: determining the real client IP behind a proxy from X-Forwarded-For (rightmost / trust-hops based) with peer-address fallback.

### Modified Capabilities

(none — greenfield project, no existing specs)

## Impact

- **New code**: `server/` sources (main, config parsing, ACL, rate limiter, range parsing, request handler), root and per-directory `xmake.lua`.
- **Dependencies**: Sogou Workflow v1.0.1 (prebuilt at `~/lab/workflow/_include` + `_lib`; links OpenSSL indirectly), pthread, dl. xmake ≥ 3.0 available locally.
- **Deployment assumption**: single instance behind a proxy that appends the real client IP to `X-Forwarded-For` (e.g. nginx `$proxy_add_x_forwarded_for`); rate limiting is per-instance semantics.
- **Operational files**: server config file + ACL rules file loaded at startup and (for ACL) reloaded at runtime.
- **Memory model**: peak memory ≈ concurrent responses × cap; `max_connections` and cap are coupled tuning knobs.
