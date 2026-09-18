# Turbo timing experiments

Open **Turbo → Experimental timing → Open experiments** to compare optional timing changes for selected applications. Experiments are disabled by default. Empty application selection means neither experiments nor detailed timing capture applies anywhere.

Select a registered application and ensure Turbo is enabled for it on the main Turbo page. Changes on the experimental page take effect at the next OpenXR session; save and relaunch the VR application for each comparison.

## Capture a baseline

Leave **Enable timing experiments** off and enable **Capture detailed timing logs**. Run a repeatable scene, then use **Export Debug**. This records normal VectorXR behavior with additional diagnostics.

Timing capture works at Info or Debug log level. It records one-second bursts at session startup, every 30 seconds, and when Turbo's effective on/off state changes. Capture is capped at 30,000 events per session. A bounded queue drops excess events rather than waiting for disk; dropped events and exhausted capture budgets are reported. Logging has some overhead, so use the same capture setting on both sides of a comparison.

## Change one control at a time

With experiments enabled, selected applications use Async pacing and bypass Auto pacing decisions. Experimental sessions do not update the remembered Auto results. Persistent recovery and live failure protection continue to apply according to your existing safety settings.

| Control | Default | Effect while experimental Turbo is active |
| --- | --- | --- |
| Wait for an overlapping submission | Off | Waits for an in-progress submission and handoff before returning the next app frame prediction. A 50 ms timeout falls back to the existing protected handoff. This is a bounded experiment, not a reproduction of Toolkit's locking implementation. |
| Sample prediction timing at wait entry | Off | Samples elapsed app-wait time before internal waiting instead of when returning a fabricated prediction. |
| Prediction dampening | 100% | Values from 50–99% shorten a fabricated prediction's horizon toward the current runtime clock. Requires the runtime's Windows clock conversion extension; otherwise the log reports that dampening is bypassed. Predictions remain strictly increasing. |
| Application frame cap | 0 (off) | Caps app frame-wait cadence at 20–240 FPS while Turbo is active. It does not enable ASW, change headset refresh rate, or guarantee compositor cadence. |

These options target different timing effects. Enabling several simultaneously makes it harder to identify which caused an improvement or regression. The persistent worker, runtime-wait serialization, visibility handling, and handoff protection remain in place.

Choose **Restore normal timing**, save, and relaunch to disable experiments and reset all behavioral controls. Application selection and detailed capture are retained, allowing a new baseline capture.

## Reading the results

Performance Diagnostics includes the timing configuration latched for each new capture session. Older results without this metadata remain readable. A session's averages include all captured conditions, including in-game Turbo toggles, scene changes, and reprojection transitions. Use separate launches and exports for a controlled comparison.

Detailed events appear as `Turbo-trace` records in the normal logs included in the debug ZIP. Their `ns` field is monotonic time since capture startup; `thread` and `id` correlate operations. The trace header documents event fields. It includes app/runtime waits, returned predictions, handoff generations, runtime begins/submissions, and pose lookup display times. The writer runs separately from the frame threads.

These measurements do not report ASW activity, actual displayed frames, or visual smoothness. Include the runtime's compositor counters and describe head movement when reporting a presentation problem.
