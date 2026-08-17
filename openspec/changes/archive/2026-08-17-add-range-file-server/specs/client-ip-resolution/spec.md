# Spec: client-ip-resolution

## ADDED Requirements

### Requirement: Real client IP from X-Forwarded-For, rightmost trusted entry
The server SHALL resolve the client IP from the `X-Forwarded-For` header by selecting the `trust_hops`-th entry counting from the right (default `trust_hops = 1`, i.e. the rightmost entry — the one appended by the nearest trusted proxy). The resolved IP SHALL be the single identity used for both ACL enforcement and bandwidth limiting.

#### Scenario: Single proxy appends the real IP
- **WHEN** the request carries `X-Forwarded-For: 1.2.3.4, 5.6.7.8` (client-forged value followed by proxy-appended real IP) and `trust_hops = 1`
- **THEN** the resolved client IP is `5.6.7.8`

#### Scenario: Client forgery of the left side is ineffective
- **WHEN** a client sends a forged `X-Forwarded-For` whose leftmost entry is a whitelisted IP but the trusted proxy appended the client's real IP at the right
- **THEN** ACL and rate limiting evaluate the real (right-selected) IP, not the forged entry

#### Scenario: Multiple trusted hops
- **WHEN** `trust_hops = 2` and the header is `X-Forwarded-For: a, b, c`
- **THEN** the resolved client IP is entry `b` (second from the right)

### Requirement: Fallback to socket peer address
When the `X-Forwarded-For` header is absent, the server SHALL resolve the client IP from the connection's socket peer address (Workflow `get_peer_addr()`), supporting both IPv4 and IPv6.

#### Scenario: Direct connection without XFF
- **WHEN** a client connects directly (no proxy) and sends no `X-Forwarded-For` header
- **THEN** the resolved client IP is the socket peer address of the connection

### Requirement: Unparseable selected entry falls back to peer address
When the header is present but the entry selected by `trust_hops` cannot be parsed as an IPv4/IPv6 address (including the case where the header has fewer entries than `trust_hops`), the server SHALL fall back to the socket peer address.

#### Scenario: Selected entry is garbage
- **WHEN** `X-Forwarded-For` is present but the rightmost entry is not a valid IP literal
- **THEN** the resolved client IP is the socket peer address

#### Scenario: Header shorter than trust_hops
- **WHEN** `trust_hops = 3` but the header contains a single entry
- **THEN** the resolved client IP is the socket peer address

### Requirement: IPv4 and IPv6 normalization
The resolver SHALL parse and normalize both IPv4 and IPv6 literals, so that equivalent textual forms of the same address resolve to the same identity for ACL matching and rate limiting.

#### Scenario: IPv6 forms match consistently
- **WHEN** the selected XFF entry is `2001:db8::1` and the ACL contains `2001:0db8:0000:0000:0000:0000:0000:0001/128`
- **THEN** the entry matches the ACL rule
