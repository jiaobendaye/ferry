# ip-access-control Specification

## Purpose

Enforce IP-based access control on the file server: blacklist/whitelist rules with blacklist priority, CIDR matching for IPv4 and IPv6, hot reload of the ACL file via mtime polling, fail-closed startup loading, and denial responses that leak no file information.

## Requirements

### Requirement: Blacklist takes priority
The server SHALL reject any request whose resolved client IP matches a blacklist entry with `403 Forbidden`, regardless of whether the IP also matches a whitelist entry.

#### Scenario: Blacklisted IP also present in whitelist
- **WHEN** an IP matches both the blacklist and the whitelist
- **THEN** the server responds `403 Forbidden`

### Requirement: Whitelist gates access when configured
When the whitelist is non-empty, the server SHALL reject with `403 Forbidden` any request whose resolved client IP does not match a whitelist entry (after the blacklist check). When the whitelist is empty, all non-blacklisted IPs SHALL be allowed.

#### Scenario: IP outside a configured whitelist
- **WHEN** the whitelist is non-empty and the client IP matches no whitelist entry and no blacklist entry
- **THEN** the server responds `403 Forbidden`

#### Scenario: Empty whitelist allows everyone else
- **WHEN** the whitelist is empty and the client IP matches no blacklist entry
- **THEN** the request is allowed to proceed

### Requirement: CIDR matching for IPv4 and IPv6
ACL entries SHALL support CIDR notation for both IPv4 and IPv6. A bare IP address entry SHALL be treated as a host route (`/32` for IPv4, `/128` for IPv6).

#### Scenario: IPv4 subnet entry
- **WHEN** the blacklist contains `192.168.1.0/24` and the client IP is `192.168.1.57`
- **THEN** the entry matches and the request is rejected

#### Scenario: IPv6 prefix entry
- **WHEN** the blacklist contains `2001:db8::/32` and the client IP is `2001:db8:1::1`
- **THEN** the entry matches and the request is rejected

#### Scenario: Bare IP entry
- **WHEN** the whitelist contains `10.0.0.5` and the client IP is `10.0.0.5`
- **THEN** the entry matches (as `10.0.0.5/32`)

### Requirement: Hot reload by mtime polling
The server SHALL poll the ACL file's modification time every `acl_poll_interval_sec` seconds (default 5). When a change is detected the file SHALL be parsed fully and the active rule set replaced atomically, so concurrent requests always see either the complete old or the complete new rule set. If the changed file fails to parse, the server SHALL keep the previous rule set and record an error in the log.

#### Scenario: Rules updated without restart
- **WHEN** an operator adds an IP to the blacklist file and saves it
- **THEN** within one poll interval the server rejects that IP with `403`, without a restart

#### Scenario: Broken file keeps old rules
- **WHEN** the ACL file is modified to contain an unparseable line
- **THEN** the previously loaded rules remain in effect and an error is logged

### Requirement: Startup loading fails closed on errors
At startup the server SHALL load the ACL file and SHALL refuse to start if the file is missing when configured or contains unparseable entries. An empty (zero-entry) ACL file SHALL be valid and result in no restrictions.

#### Scenario: Missing ACL file aborts startup
- **WHEN** the configuration names an ACL file that does not exist
- **THEN** the server refuses to start with a clear error

#### Scenario: Empty ACL file permits all
- **WHEN** the ACL file exists but defines no entries
- **THEN** the server starts and allows all client IPs

### Requirement: Denial response
Rejected requests SHALL receive `403 Forbidden` with no file content. The denial SHALL occur before any filesystem access for the requested path.

#### Scenario: No information leak on denial
- **WHEN** a blacklisted IP requests an existing file
- **THEN** the server responds `403` and does not open, read, or reveal metadata of the file
