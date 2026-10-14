# Runtime relay

The runtime relay is VectorXR's low-frequency control plane between the desktop app and layer processes. It is separate from saved settings: settings describe persistent launch behavior, while relay state belongs to one live OpenXR session.

The initial capability controls the Quadviews diagnostic visualization. The protocol is intentionally general enough to carry more live controls and status later.

## Transport

The relay uses atomic JSON sidecars under `%LOCALAPPDATA%/VectorXR/runtime`:

- `status/<sessionId>.json` is written by a layer process.
- `control/<sessionId>.json` is written by the desktop app.
- `VECTORXR_RUNTIME_RELAY_PATH` overrides the root for development and tests.

Filesystem polling is appropriate for human-scale controls and status updates. It is inspectable, works across unrelated processes without installing a service, and keeps all I/O on the existing watcher thread. It is not intended for per-frame input, high-volume telemetry, or latency-sensitive streaming; those should use a different transport behind the same session/capability model.

## Protocol version 1

Status documents identify one process-local OpenXR session and advertise only the controls that can currently work:

```json
{
  "protocolVersion": 1,
  "sessionId": "4242-1785600000000-1",
  "processId": 4242,
  "application": "DCS.exe",
  "updatedAtUnixMilliseconds": 1785600000000,
  "acknowledgedRevision": 1785600000000123,
  "capabilities": {
    "quadviewsDiagnosticVisualization": true
  },
  "state": {
    "quadviewsDiagnosticVisualization": false,
    "turboState": "framegen",
    "turboReason": "Frame Generation owns frame pacing in this session.",
    "framegenState": "active",
    "framegenReason": "Interpolate · Dynamic · 45 → 90 fps · GPU 3.1 ms"
  }
}
```

`framegenState` is `off`, `waiting`, `active` (generating frames), `standby` (on, but the game
holds the refresh rate itself, so its frames pass straight through), `paused`, or `unavailable`.
`framegenReason` is a short line in the On-Screen Display's words: the mode, frame rate, and
frame rates while active; the game's rate on standby; or why it is paused or unavailable.
`turboState` reads `framegen` while Turbo yields to Frame Generation.

Control documents target that exact session and express desired state rather than a toggle event:

```json
{
  "protocolVersion": 1,
  "targetSessionId": "4242-1785600000000-1",
  "revision": 1785600000000124,
  "expiresAtUnixMilliseconds": 1785600005000,
  "desired": {
    "quadviewsDiagnosticVisualization": true
  }
}
```

## Invariants

- Session IDs change for every OpenXR session. A stale command cannot affect a later game launch.
- Commands expire quickly and revisions are applied at most once.
- Desired-state commands are idempotent, so retries do not invert state accidentally.
- The layer acknowledges a revision only after applying it. The app treats status as authoritative.
- Status heartbeats let the app reject crashed or exited processes without trusting leftover files.
- A session's files go when it ends. A game that exits without OpenXR teardown still has them removed when its process exits normally, and the next game to start a session removes the files of sessions whose process has exited (a crash, a forced close or a power loss).
- Capabilities are runtime facts. The app must disable a control when the active session does not advertise it.
- File reads, parsing, and writes never occur on the render path.
- Atomic replacement prevents either side from consuming partially written JSON.

## Extending the relay

Additive capabilities and desired-state fields can remain on protocol version 1 when older peers can safely ignore them. Increment `protocolVersion` for incompatible meaning or structural changes.

Prefer desired state for switches, modes, and numeric controls. A future one-shot action needs a command ID, explicit acknowledgement/result, bounded retention, and queue semantics; it should not be represented as a boolean edge. Likewise, high-rate metrics should not be added to the heartbeat document.

The relay is scoped to the current Windows user and is not an authentication boundary. Validate session IDs, command ranges, expiry, capability support, and protocol version in the receiving process. If future controls become security-sensitive or cross-user, replace the transport with an authenticated named pipe or broker while retaining the protocol's targeting and acknowledgement rules.
