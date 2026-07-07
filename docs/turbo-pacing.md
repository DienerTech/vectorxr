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
- **Sequenced** — the wait is joined *inside* EndFrame, immediately after the
  submit that unblocks it, and the next frame is pre-begun
  (`turbo_frame_prebegun_`). Strictly ordered on the submit thread, spec-clean,
  safe on interlocking runtimes; the cost is that EndFrame blocks for the
  pacing wait. This is the OpenXR-Toolkit turbo design.
- Sequenced still implements its wait as a joinable future with a timeout —
  if a runtime ever withholds a post-submit wait, we degrade to the async
  drain path next frame instead of deadlocking inside EndFrame.

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
