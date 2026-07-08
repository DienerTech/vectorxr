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

  **MSFS2024 establishment lessons (2026-07-07):** the first MSFS2024 VR test
  froze the game on PiOpenXR on the first pipelined frame — its Zouna engine
  orders frame calls differently than DCS: the establishment handshake
  completed *while the previous frame's EndFrame was already in flight*, so
  the owed begin was on course to reach the runtime as Begin(N+1) before
  End(N). Three hardenings (all on `turbo-metrics`):
  - An owed establishment begin arriving while ForwardEndFrame is mid-submit
    is **deferred** (swallowed, flagged) and issued on the frame thread right
    after the submit returns — the runtime always sees End(N) → Begin(N+1).
  - The compensation wait+begin is **skipped while the establishment begin is
    still owed** (it would duplicate the handshake's real wait from the
    submit thread — the PiOpenXR wedge class).
  - The first engage additionally requires **≥5 s of session age**: MSFS
    renders at full rate through its VR-mode transition, so the healthy-frame
    streak alone passed within a second and engaged turbo mid-transition.
  Every runtime call in the turbo window now has budgeted `Turbo-diag`
  markers (pre-submit state snapshot, drain, compensation, owed/deferred
  begin, wait pass-through, valve re-coupling), so any future wedge
  pinpoints its exact call site from the log tail.

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

## Field findings, 2026-07-07 (metrics-instrumented)

- **The wait/submit interlock is a driver property, not a runtime property.**
  SteamVR driving a Crystal through Pimax's `aapvr` driver interlocks exactly
  like PiOpenXR — the async preset stalled twice and level-1 fell back to
  sequenced within 2 frames (as designed, no suspend). "SteamVR → async" in
  the seed table really means "SteamVR with a non-interlocking driver".
- **Sequenced pacing cannot lift a wait-enforced fps lock.** Measured with
  Pimax's 45 fps lock + smart smoothing: wait-block 16.5 ms/frame with turbo
  off, 15.8 ms with turbo on, fps identical — the synchronous post-submit
  wait absorbs the pacing gate on the frame thread. On interlocking drivers
  turbo is therefore a wait-latency/CPU-headroom tool (measured 2.9 ms/frame
  reclaimed at the 90 Hz cap), never an fps uplift. Uplift requires async,
  i.e. a non-interlocking driver.
- **Known-unsupported combo**: DCS + SteamVR runtime + synthesized quadviews
  renders exactly one (perfectly composited) frame, then silently submits
  layerCount=0 forever — black screen with a healthy 45 fps frame loop and
  no error anywhere. Undiagnosed; no benefit to the config on Pimax hardware,
  so parked. Black-screen forensics (runtime EndFrame failure logging,
  shouldRender transitions, submitting→empty transitions, all budgeted
  per session) were added so a future repro self-diagnoses.

## Metrics (turbo-metrics sidecar, branch `turbo-metrics`)

Measures the effect of each pacing strategy on real frame pacing and shows it
in the Turbo panel. Per EndFrame the layer buckets the frame by state — `off`
(toggled off / suspended / not yet engaged), `async`, `sequenced` — and
accumulates: frame count, frame-interval sum/max plus a 0.5 ms histogram (for
p99), app-visible runtime-pacing block time (WaitFrame pass-through waits,
the valve re-coupling wait, and the EndFrame drain/sequenced wait), fabricated
waits, and drain timeouts. Intervals ≥ 1 s are excluded (counted as
`discardedFrames`) so loading stalls don't drown the averages.

- **Collection modes** (`turbo.metricsMode`): `always` (default), `binding`
  (record only while the capture binding — `turbo.metricsBinding` — is armed,
  so the user can cut loading screens/menus out of the data), `off`.
- **Sidecar** `turbo-metrics.json` (seen-apps pattern, next to
  runtime-pacing.json): one entry per capture session with per-state buckets
  of display-ready stats (avg fps, avg/p99/max frame ms, avg wait-block ms).
  Sessions are newest-first, retention-capped at 8, so history display is
  possible later without a schema change. Flushed every ~15 s via a detached
  async write (skipped if the previous write is in flight — the frame thread
  never touches the filesystem) and synchronously with `live:false` at
  session teardown. Ships in debug reports.
- **UI**: Metrics section on the Turbo tab — collection dropdown, capture
  binding panel, session picker, per-state table (avg fps, 1% low ≈
  1000/p99, avg/p99 frame, wait blocked, stalls), and a "+X% avg fps vs off"
  callout shown only when both states have ≥ 30 s captured in the same
  session. Comparisons across states are honest only if the scenes were
  comparable; the UI says so.
