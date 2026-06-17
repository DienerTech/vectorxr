# Performance Monitoring Design Notes

Status: MVP infrastructure started. This document captures the shared design
direction for a profile-scoped VectorXR performance monitor and how recorded
session insights can inform Quadviews tuning without making Quadviews own the
telemetry system.

## MVP Implementation Status

Initial implementation now includes:

- profile-scoped `modules.performance.profiles` config in the UI model, Tauri
  config model, JSON schema, native parser, and native resolver
- native `xrWaitFrame` and `xrBeginFrame` interception alongside the existing
  `xrEndFrame` hook
- fixed-size in-memory counters and frame-time sample ring for the active
  monitored session
- session-start and session-end native log summaries
- native resolver coverage for opt-in profile matching and disabled-profile
  behavior

Still pending:

- full Performance UI tab and per-game profile editor
- persisted session history
- charting and notable-event history
- background worker publication path
- Quadviews tuning workflow integration

## Goals

- Track per-game OpenXR frame performance when the user explicitly enables
  collection for that game.
- Make frame pacing, target frame time, and notable performance events visible
  in the UI.
- Provide low-overhead session data that future tuning workflows can analyze.
- Let Quadviews tuning and future dynamic modes derive from the shared monitor
  rather than carrying private timing logic.
- Keep monitoring, aggregation, charting, and logging off the frame hot path.

## Non-Goals

- Do not collect performance data for unknown applications under a default
  profile.
- Do not attempt to expose perfect app GPU frame time from OpenXR core alone.
- Do not write logs, JSON, or UI events directly from per-frame OpenXR hooks.
- Do not make dynamic Quadviews depend on backend-specific GPU timing being
  available.

## Profile Model

Performance monitoring should be tied to registered applications and explicit
game profiles. A default profile is intentionally avoided because performance
history only makes sense when the data can be attributed to a known application.

Suggested profile fields:

- `enabled`: turns monitoring on for the matched application.
- `applicationIds`: registered applications covered by the profile.
- `collectionMode`: `summary` by default, with an optional short-lived
  `diagnostic` mode for denser capture.
- `retention`: controls how many sessions or days of data are kept.

Unknown apps should only appear as candidates in the seen-apps workflow. The UI
can invite the user to create a monitoring profile, but should not collect
performance data until the user opts in.

## Session Model

Each launch should be stored as a separate session. Session metadata should
include enough context to compare runs without mixing incompatible conditions:

- application id, executable name, and resolved profile name
- session start/end time and duration
- OpenXR runtime name when available
- detected target frame time and approximate refresh rate
- active VectorXR enhancements and relevant settings fingerprints
- graphics backend details when known
- whether Quadviews native composition, copy fallbacks, or dynamic mode were
  active

By default, persist compact session aggregates and notable events. Raw or
near-raw frame samples should be bounded and reserved for temporary diagnostic
captures.

## Metrics

The core monitor should be graphics-API independent by sampling OpenXR frame
lifecycle calls:

- `xrWaitFrame`: target frame time from `predictedDisplayPeriod`, predicted
  display cadence, `shouldRender`, and wait/block duration.
- `xrBeginFrame`: begin-frame timing and frame lifecycle state.
- `xrEndFrame`: app CPU submit span, layer submission counts, active enhancement
  state, and VectorXR processing time.

Useful UI metrics:

- target frame time and detected refresh rate, such as `11.1 ms @ 90 Hz`
- average, p50, p95, p99, and worst frame timing
- percent of frames over budget
- frame pacing stability and stutter count
- `shouldRender=false` count
- session min/max/avg summaries
- active enhancement timeline
- notable runtime or VectorXR events

Percentiles and target-relative stats should be favored over averages. Averages
are still useful, but they hide pacing issues that VR users actually feel.

## Backend Coverage

The core monitor works across DX11, DX12, Vulkan, and OpenGL because it is based
on OpenXR frame calls, not graphics API hooks.

Backend-specific timing can be layered in where available:

- DX11: existing D3D11 timestamp query groundwork can measure VectorXR's native
  Quadviews compositor GPU span.
- DX12: would require timestamp query heaps, command queue integration, and
  fence-safe result collection.
- Vulkan: would require timestamp query pools and queue/device function plumbing.
- OpenGL: could use timer queries if that backend becomes relevant.

These backend-specific timings should be treated as attribution signals for
VectorXR-owned work. They should not be presented as full app GPU frame time
unless a future backend path can prove that attribution.

## Hot Path Contract

Performance monitoring must be designed so enabling it has no meaningful impact
on the game or on other VectorXR features.

Allowed on the OpenXR frame hot path:

- a fast enabled check
- reading a monotonic timestamp
- updating fixed-size counters or appending a tiny sample to a preallocated ring
  buffer
- dropping samples when buffers are full

Avoid on the hot path:

- file I/O
- JSON serialization
- string formatting and stream construction
- logger calls
- heap allocation
- blocking locks
- UI event emission
- synchronous GPU query reads
- any GPU operation that flushes or waits for the app

Aggregation should run on a low-priority background worker. The worker can roll
up samples, compute percentiles, classify pressure, write snapshots, update the
UI bridge, and emit throttled logs.

## Data Flow

Suggested architecture:

```text
OpenXR frame hooks
  -> fixed-size sample/counter buffers
  -> background performance worker
  -> rolling per-game/session stats
  -> local session store and UI snapshot
```

The capture path and publish path should be separated. The UI and logs consume
background snapshots, not per-frame events.

## UI Surface

The UI should make performance monitoring feel like part of the per-game profile
workflow:

- A registered game can opt into `Collect performance data`.
- A performance view shows session history for that game.
- Charts show frame time against the detected target frame time.
- Summary panels show target FPS, avg/p95/p99/worst, over-budget percentage,
  stutter count, and session duration.
- An event log records notable moments such as refresh-rate changes,
  `shouldRender=false`, config reloads, active enhancement changes, Quadviews
  compositor fallback/copy paths, and dynamic Quadviews adjustments.
- Delete and export controls make it clear that performance history is local and
  user-controlled.

Comparison views should always show context such as refresh rate, runtime,
backend, active enhancements, and Quadviews settings. Comparing a 72 Hz session
to a 90 Hz session without that context would be misleading.

## Quadviews Workflow Integration

Quadviews tuning should start as a dedicated workflow over recorded performance
sessions, not as an implicit side effect of enabling monitoring. The monitor
records and summarizes the game session; the Quadviews workflow can then use
that evidence to recommend or apply profile changes.

Candidate tuning inputs:

```text
targetFrameMs
frameTimeP95Ms
frameTimeP99Ms
overBudgetPercent
cpuFrameSpanP95Ms
shouldRenderFalseCount
compositorGpuMs
appPixelBudget
pressureState
confidence
```

This lets Quadviews avoid chasing the wrong bottleneck. If frame pressure is
high but CPU span is also high, the workflow can hold quality and report likely
CPU limitation. If pressure is high, CPU span is low, and the Quadviews pixel
budget is heavy, reducing pixel cost is a better-supported recommendation.

If a future dynamic Quadviews mode applies changes automatically, it should be
enabled and configured from the Quadviews profile or a dedicated tuning flow.
Its decisions should be slow, explainable, and logged from the background or a
throttled control path:

- target frame time
- measured pressure state
- old and new effective settings
- reason and confidence
- whether the change is runtime-only or a recommendation for the next launch

## Open Questions

- What exact session store format should be used: compact JSON files, a small
  SQLite database, or one rolling summary file per application?
- Should detailed capture be time-limited automatically, for example the next
  launch only?
- How much per-frame history is needed for useful charts without creating large
  files?
- How should UI charts downsample long sessions?
- What should the first Quadviews tuning workflow do: recommend values, apply
  profile edits, or run a bounded adaptive test session?
- Can any runtime-specific APIs provide reliable reprojection or missed-frame
  stats without compromising portability?

## Implementation Phases

1. Add `xrWaitFrame` and `xrBeginFrame` interception alongside the existing
   `xrEndFrame` hook.
2. Implement an in-memory session monitor with fixed-size buffers and no
   per-frame allocation.
3. Emit throttled interval summaries and session-end summaries through the
   background worker.
4. Add profile-scoped UI controls and a local session history surface.
5. Add event logging for active enhancement changes, refresh-rate changes, and
   Quadviews compositor health.
6. Add a Quadviews tuning workflow that reads recorded session summaries.
7. Build any future dynamic Quadviews mode on top of that explicit workflow and
   the shared performance data.
