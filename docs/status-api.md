# Status API for other programs

VectorXR publishes its live state in one JSON file that other programs may read, such as tools that
tune a game's settings while Frame Generation runs. This page is the whole contract.

## The file

```
%LOCALAPPDATA%\VectorXR\api\status.json
```

- The VectorXR layer in the game that has a live OpenXR session writes it about once a second,
  replacing it in one step: a reader sees the previous document or the new one, never part of one.
- When that session ends, the game removes the file, but only while the file still names its own
  process. A game shutting down never removes the file of a game started after it.
- A game that crashes, or exits without ending its OpenXR session (Microsoft Flight Simulator and
  Star Citizen often do), leaves its last file behind. Detect that by its update time (below).
- No file means no game with a VectorXR session is running, or the running game has not written
  one yet (the first write follows the session's start within about a second).
- Every OpenXR game loads its own copy of VectorXR. If two run sessions at once (an overlay app, or
  a game relaunched before the old one has quit), each replaces the file in turn. Check
  `processId` (and `application`) to know which one you are reading.

Read only `status.json`. Files named `status.json.tmp.*` beside it are writes in progress. The
`%LOCALAPPDATA%\VectorXR\runtime\` folder is VectorXR's internal channel to its own app: do not read
it, as its format changes between versions without notice.

### Reading it

- Read the whole file, then close it. Open it with read, write and delete sharing
  (`FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE`; in .NET,
  `new FileStream(path, FileMode.Open, FileAccess.Read, FileShare.ReadWrite | FileShare.Delete)`),
  so that your read never holds up VectorXR's next write. A reader without delete sharing (such as
  .NET's `File.ReadAllText`) works too, but VectorXR's write waits for it, and gives up for that
  second if the file stays open.
- Opening can fail for a moment while the file is being replaced or removed: try again at your next
  poll.
- Poll once a second or less often. The values are averages over about a second.
- Parse it as JSON and ignore fields you do not know.

### Stale files

The file is current while `updatedAtUnixMilliseconds` is recent. Compare it with the current time
(milliseconds since 1970-01-01 UTC): a file not updated for 3 seconds or more is stale. Its game has
crashed, exited without ending its session, or hung. (The write runs on a thread of its own, so a
loading screen that holds up the game's frames does not stop it.) Also treat the file as stale when
no process with its `processId` is running.

## Compatibility

- `schema` is always `"vectorxr.status"`.
- `version` is `1`. Version 1 only ever gains fields: none is removed, renamed, or changes its
  meaning or units. New fields may appear in any release, also inside `framegen`.
- A change that cannot keep that promise would write a second file (`status-v2.json`) beside this
  one for a while, so that existing readers keep working.

## Document

```json
{
  "schema": "vectorxr.status",
  "version": 1,
  "processId": 4242,
  "application": "FlightSimulator2024.exe",
  "sessionId": "4242-1785600000000-1",
  "updatedAtUnixMilliseconds": 1785600001234,
  "framegen": {
    "state": "active",
    "mode": "interpolate",
    "frameRate": "auto",
    "generating": true,
    "multiplier": 2,
    "refreshHz": 90.00,
    "targetAppFps": 45.00,
    "appFps": 44.96,
    "outputFps": 90.00,
    "distinctFps": 89.50,
    "refreshesPerRealFrame": 2.00,
    "holding": true,
    "autoStep": 2,
    "autoStepUpBelowFps": 36.00,
    "autoStepDownAboveFps": 76.50
  }
}
```

### Identity (always present)

| Field | Meaning |
| --- | --- |
| `schema` | `"vectorxr.status"`. |
| `version` | `1` (see [Compatibility](#compatibility)). |
| `processId` | The Windows process ID of the game that wrote the file. |
| `application` | The game's executable file name, such as `FlightSimulator2024.exe`. |
| `sessionId` | Identifies the game's OpenXR session; a new session (in a relaunched game, or in the same game) gets a new one. Compare it for equality only: its format may change. |
| `updatedAtUnixMilliseconds` | When the file was written, in milliseconds since 1970-01-01 UTC. |

### Frame Generation (`framegen`)

Present while the session runs with Frame Generation turned on. Absent when Frame Generation is off
for the game, and when it was turned on after the game started (the game must restart to use it).

| Field | Meaning |
| --- | --- |
| `state` | `active` (generated frames are shown), `standby` (frames pass straight through: the game holds the refresh rate on its own, Auto is at full rate, or generated frames could not be shown for a moment), `waiting` (starting), `paused` (after errors; it may retry), `unavailable`, or `off`. More values may be added: treat an unknown one as not generating. |
| `mode` | `interpolate` or `extrapolate`. |
| `frameRate` | The frame rate setting: `2x`, `3x`, `4x`, `auto` or `dynamic`. |
| `generating` | True while generated frames are shown (`state` is `active`). |
| `multiplier` | Headset refreshes per real frame aimed for: the fixed rate, Auto's current step (1 at full rate), or under Dynamic the typical value (1 while standing by). |
| `refreshHz` | The headset's refresh rate. 0 until the runtime has reported it. |
| `targetAppFps` | The game's frame rate to aim for now. While generating, `refreshHz / multiplier`: a fixed rate's hold, Auto's current step, or the rate at which Dynamic's typical multiplier holds. While not generating, `refreshHz`: the game's frames pass straight through. 0 only until `refreshHz` is known. |
| `appFps` | Real frames per second from the game. |
| `outputFps` | Frames per second VectorXR submits to the runtime. |
| `distinctFps` | Of those, frames showing something new (repeats left out). |
| `refreshesPerRealFrame` | Headset refreshes each real frame is shown for, measured frame by frame and smoothed over about ten frames. 0 until measured. At a fixed or Auto rate, a game slower than `targetAppFps` gets more refreshes per frame than `multiplier`, and every refresh is still filled. |
| `holding` | True while the game keeps up with `targetAppFps`: `refreshesPerRealFrame` is at most 10% over the refreshes aimed for (`multiplier` while generating, 1 otherwise). False until measured. |

Only while `frameRate` is `auto` (or `dynamic` falling back to Auto, where the system cannot run
Dynamic):

| Field | Meaning |
| --- | --- |
| `autoStep` | Auto's current step, the same as `multiplier`: 1 is full rate (nothing generated), 2 to 4 generate. |
| `autoStepUpBelowFps` | Auto steps up once the game's frame rate stays below this (more than 30% of its frames in a second take more than 1.25 times their refreshes). 0 when there is no higher step (4x, or video memory too short for more). |
| `autoStepDownAboveFps` | Auto tries one step down after five steady seconds (at most 2% of frames late) once the game's own frame rate, its frame time without Frame Generation's hold, reaches this. Where Frame Generation presents on the game's thread, holding the current step steady is enough, and this is `refreshHz / autoStep`. 0 at full rate. |

A tool that tunes the game's settings and runs alongside Auto should keep the game's rate clear of
both thresholds, so that its own adjustments and Auto's steps do not chase each other across the
boundary. Under a fixed rate, aim for `appFps` at `targetAppFps` and watch `holding`.

## Testing without a headset

`VECTORXR_EXTERNAL_STATUS_PATH` set to a file path in the game's environment makes VectorXR write
there instead. It is meant for development and tests.
