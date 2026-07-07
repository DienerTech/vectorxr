# Turbo Frame Pacing: Auto Discovery Design

Status: implemented (first draft) on `feature-branch-twelve`, July 2026.

## Problem

Turbo 0.12 pipelined the runtime `xrWaitFrame` on a background thread launched
right after each submit (**async pacing**). Field reports showed that several
runtimes — Oculus (Quest 3 via Link), Varjo, and Pimax's PiOpenXR — *interlock*
`xrWaitFrame` with frame submission: the pipelined wait intermittently stalls
until the next `xrEndFrame` goes down. Each stall costs the 250 ms drain
timeout, and the safety circuit (5 timeouts / 30 s) correctly suspended turbo —
making turbo unusable on roughly half the runtimes while working perfectly on
SteamVR / Steam Link.

Diagnostic signature in debug logs: `turboDrainAndBegin max` pinned at ~250 ms,
`runtimeWaitFrame` completing 0–3 times per 5 s window against hundreds of
`fabricatedWaits`.

## Pacing strategies

- **Async** — background-thread wait, drained at the next EndFrame. Best
  overlap (the pacing wait runs concurrently with the app's next-frame CPU
  work). Fails on interlocking runtimes.
- **Sequenced** — the real wait+begin run *synchronously on the app's frame
  thread* inside EndFrame, right after the submit. From the runtime's view
  this is the standard single-threaded frame loop (End → Wait → Begin), which
  every conformant runtime must release — so no timeout is needed. This is
  the OpenXR-Toolkit turbo design. Two field-test lessons are baked in:
  - **Thread identity matters** (Pimax test 2): PiOpenXR pins frame pacing to
    the calling thread. A draft that ran the wait on an async worker and
    merely *joined* it from the submit thread stalled ~360 ms per frame (our
    stacked timeouts); the same wait on the frame thread returns in ~10 ms.
  - **Apps overlap WaitFrame with EndFrame** (Pimax test 3): DCS calls
    xrWaitFrame from its sim thread concurrently with xrEndFrame on its
    render thread (spec-legal). Sequenced pacing is therefore a state machine
    (`kInactive → kEngaging → kActive → kUnwinding`) whose fabrication shield
    never has a per-frame gap, and whose engage/release run *through the
    app's own WaitFrame call* (handshake/unwind on the app's wait thread, the
    following xrBeginFrame passing through via `turbo_begin_owed_`) so we
    never issue a runtime wait that duplicates one the app already has in
    flight — a draft that did deadlocked DCS on the second pipelined frame.
    A fabricated wait that raced a state transition is compensated with a
    wait+begin pair at the next EndFrame, and every real xrWaitFrame we issue
    or forward is serialized behind `turbo_runtime_wait_mutex_`.

  A sequenced wait that blocks ≥250 ms is judged after the fact and counts
  toward level 2.

  **Structural pipeline + pacing valve:** field tests showed steady-state
  sequenced turbo runs clean (full DCS flight at 85–90 fps on PiOpenXR), but
  a mid-session unwind followed by re-engage hardlocks DCS inside the runtime
  — outside every instrumented layer call — because PiOpenXR's per-thread
  frame pacing wedges when the wait-issuing thread migrates a second time
  (engage moves waits sim→render, unwind moves them back). Async mode never
  crashed on toggles precisely because it never changed the app's thread
  topology; its off-thread wait just stalled gracefully. So the sequenced
  pipeline is **structural**: established once per session (cadence gate +
  handshake) and kept until session end. The turbo toggle operates a **pacing
  valve** instead of tearing anything down — valve open: app waits fabricate
  instantly (decoupled); valve closed: each app wait blocks consuming a
  pacing token posted by that frame's real pre-wait, re-coupling the app to
  genuine runtime pacing with zero topology change (a bounded 100 ms cv
  timeout falls back to fabrication so an app that stops submitting can never
  deadlock against the valve). Suspend/re-arm ride the same valve. Mid-session
  pacing-mode changes are deferred to the next session once the pipeline is
  established.

## Cadence gate (added after the first field test)

A stalled runtime wait is only evidence against a pacing mode when the app is
pacing normally. The first DCS test engaged turbo on the 2 fps loading screen,
"discovered" 9 stalls in 11 seconds, and recorded a poisoned `unsupported`
verdict. So:

- Turbo stays **passive** until the app delivers 90 consecutive frames under
  50 ms of app-only time (our own drain/join blocking is subtracted, so a
  turbo-induced stall cannot masquerade as an app hitch and dodge the
  circuit). Once pipelining is established the gate only pauses **counting
  and stability accrual** during hitches — it must never tear the pipeline
  down and re-engage it: every unwind/re-engage transition races the app's
  loose frame threading (a mission-load pause/resume cycle hardlocked DCS on
  PiOpenXR while the same session's first engage ran clean for 28 s), and
  riding through a hitch is harmless since the pipelined wait simply runs at
  the app's own loading pace.
- Timeouts are counted **only at the drain** and only while engaged past the
  gate. The sequenced join never counts (a slow join simply defers to the next
  drain — counting both double-counted one stall) and its timeout is 100 ms.
- The stability window is **accumulated healthy engaged time** (60 s), not
  wall-clock, so loading screens neither earn nor destroy verdicts.

Side effect: this also removes turbo's engage-at-app-startup hitches on every
runtime — pipelining now starts a second or two into stable rendering.

## Two-level safety circuit

1. **Async trips** (rolling window; hair-trigger threshold of 2 while probing,
   5 otherwise) → switch to sequenced for the session (`kFallback`), no sound,
   log only. Never suspends under Auto.
2. **Sequenced trips** (3 / 30 s) → suspend turbo for the session with the
   audible cue, and during discovery record an `unsupported` verdict so the
   next session suspends up front instead of replaying the stutter. The toggle
   binding re-arms and a clean run overwrites the verdict.

Forced/pinned modes skip level 1 (the user opted out of adaptation) and go
straight to suspend, preserving pre-0.13 behavior.

## Resolution precedence (per session, when turbo first engages)

1. `turbo.pacingMode` forced to async/sequenced in settings → use it, no
   discovery, no sidecar writes.
2. `turbo.runtimePins[runtimeName]` (Auto only) → pinned mode.
3. Recorded verdict in `runtime-pacing.json` → use it (refreshes `lastUsed`).
4. Seed table: Oculus / Varjo / PiOpenXR / Pimax → sequenced; SteamVR → async.
   Recorded as a `preset` verdict after a stable window.
5. Unknown runtime → probe async with the hair trigger; fall back to
   sequenced on stalls. Recorded as `discovered` after a stable window.

A **stable window** is 60 s of engaged turbo with zero drain timeouts. Config
changes to `pacingMode`/`runtimePins` re-resolve mid-session.

## State split (who owns what)

- `settings.json` (user intent, app-written): `turbo.pacingMode`
  (`auto|async|sequenced`, default `auto`) and `turbo.runtimePins`
  (`{ runtimeName: async|sequenced }`).
- `runtime-pacing.json` sidecar (facts, layer-written, seen-apps pattern):
  one observation per runtime — name, version, mode
  (`async|sequenced|unsupported`), source (`preset|discovered`), layer
  version, first/last used, probe stats. Ships in debug reports. The app's
  "Re-discover" removes one runtime's row; the layer re-probes next session.

This split avoids the layer and the app racing over `settings.json` and keys
learned behavior by runtime, since users hop between Link / Virtual Desktop /
SteamVR freely.

## UI

Turbo tab gains a **Frame Pacing** card: the Auto/Async/Sequenced dropdown, a
runtimes table (observed runtimes + pinned runtimes; mode, source badge
Preset/Discovered/Pinned/Suspended, last used, per-runtime pin dropdown,
Re-discover). The active runtime (from the `ActiveRuntime` registry value,
fuzzy-matched by name token) is highlighted. Forced mode dims the table.
A layer-conflict advisory (OpenXR Toolkit enabled while turbo is in use)
appears on both the Turbo tab and the OpenXR Layers page.

## Telemetry

The 5 s frame-pacing debug line now includes `pacing=async|sequenced` while
turbo is engaged. Resolution, fallback, verdicts, and suspends are logged at
info level.
