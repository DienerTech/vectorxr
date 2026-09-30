# Turbo timing experiments

Experimental timing is set per custom profile: open **Turbo → Custom Profiles**, then choose the **Toolkit-style** or **Custom** strategy for a profile. Custom exposes the individual controls under **Advanced timing**. Profiles use the **VectorXR** strategy by default, and timing applies only to the profile's assigned applications. Debug logging works independently of the profile strategy.

Assign a registered application to an enabled custom profile. Strategy changes take effect at the next OpenXR session; save and relaunch the VR application for each comparison.

## Capture a baseline

Leave the profile on the **VectorXR** strategy and select **Settings → Log Level → Debug**. Save, launch the VR application, run a repeatable scene, then use **Export Debug**. This records normal VectorXR behavior with additional diagnostics. There is no separate Turbo logging toggle.

Debug records one-second bursts when enabled, every 30 seconds while enabled, on Turbo's effective on/off transitions, and after runtime frame errors, drain timeouts, or handoff cancellation. Capture is capped at 30,000 events per session. A bounded queue drops excess events rather than waiting for disk; dropped events and exhausted capture budgets are reported at Debug level. Logging has some overhead, so use the same log level on both sides of a comparison.

Changing Log Level applies live: Info pauses detailed timing and Debug resumes it with the same session clock, correlation IDs, and remaining budget. Launch with Debug enabled to request optional runtime clock conversion. If it was unavailable or not enabled at startup, live capture still works but runtime clock fields are zero. The removed `experimental.timingTrace` configuration field is accepted for compatibility and ignored.

## If OpenXR Toolkit felt smoother

Set the game's custom profile to the **Toolkit-style** strategy. It uses submission waiting **on**, entry sampling **on**, prediction **100%**, and frame cap **0 (off)**, and leaves the central log level unchanged. Save and relaunch. Changing any value under **Advanced timing** switches the profile to **Custom**.

Toolkit sampled before its shared frame lock and serialized overlapping frame calls. Its background wait completion and repeated-poll handling changed predictions dynamically; this path did not search automatically for optimal settings. Dampening defaulted to 100% and throttling to off. See the [inspected Toolkit source revision](https://github.com/mbucchia/OpenXR-Toolkit/blob/6b9ecb69a4b2dc714b14a86407868af315d02531/XR_APILAYER_MBUCCHIA_toolkit/layer.cpp#L1876).

The Toolkit-style strategy approximates those timing effects. VectorXR retains its bounded submission wait, persistent worker, runtime visibility state, and safety handling. Toolkit used different limiter scheduling and applied dampening to ordinary runtime waits too; VectorXR dampens fabricated Turbo predictions. Your Toolkit version, settings, and rendering path may differ, so this is a comparison starting point rather than identical compatibility.

1. Record the VectorXR strategy in a repeatable scene. Keep resolution, refresh rate, reprojection settings, and logging the same.
2. Compare the starting point using both head rotation and translation, observing cockpit and terrain. If it worsens presentation, switch back to the VectorXR strategy.
3. To isolate the useful difference, turn submission waiting off for one run, restore it, then turn entry sampling off for another. Keep the better combination.
4. Only then consider optional adjustments: for head-motion wobble, test 90% prediction, then 80%, returning toward 100% if tracking feels delayed or worse. For a known reprojection target, test a matching application FPS cap (45 only for a 45 FPS target), returning to 0 if pacing worsens. These are exploratory comparisons, not established fixes, and the cap does not control ASW.

## Change one control at a time

With the Toolkit-style or Custom strategy, the profile's applications use Async pacing and bypass Auto pacing decisions. Experimental sessions do not update the remembered Auto results. Persistent recovery and live failure protection continue to apply according to your existing safety settings.

| Control | Default | Effect while experimental Turbo is active |
| --- | --- | --- |
| Wait for an overlapping submission | Off | Waits for an in-progress submission and handoff before returning the next app frame prediction. A 50 ms timeout falls back to the existing protected handoff. This is a bounded experiment, not a reproduction of Toolkit's locking implementation. |
| Sample prediction timing at wait entry | Off | Samples elapsed app-wait time before internal waiting instead of when returning a fabricated prediction. |
| Prediction dampening | 100% | Values from 50–99% shorten a fabricated prediction's horizon toward the current runtime clock. Requires the runtime's Windows clock conversion extension; otherwise the log reports that dampening is bypassed. Predictions remain strictly increasing. |
| Application frame cap | 0 (off) | Caps app frame-wait cadence at 20–240 FPS while Turbo is active. It does not enable ASW, change headset refresh rate, or guarantee compositor cadence. |

These options target different timing effects. Enabling several simultaneously makes it harder to identify which caused an improvement or regression. The persistent worker, runtime-wait serialization, visibility handling, and handoff protection remain in place.

Set the profile's strategy back to **VectorXR**, save, and relaunch to disable experimental timing and reset all behavioral controls. The profile's applications and the central log level are retained, allowing a new baseline capture.

## Reading the results

Performance Diagnostics labels each new capture session with its strategy (for example **VectorXR · Async** or **Toolkit-style**) and shows the full experimental timing configuration where one applies. Older results without this metadata remain readable. A session's averages include all captured conditions, including in-game Turbo toggles, scene changes, and reprojection transitions. Use separate launches and exports for a controlled comparison.

Detailed events appear as Debug-level `Turbo-trace` records in the normal logs included in the debug ZIP. Their `ns` field is monotonic time since trace session startup; `thread` and `id` correlate operations. The trace header documents event fields. It includes app/runtime waits, returned predictions, handoff generations, runtime begins/submissions, and pose lookup display times. Additional events separate worker queuing, execution and completion; repeated-poll and drain waits; extrapolation, dampening and monotonic clamping; and the submitted display target's lead relative to the runtime clock. That lead is not a measured compositor deadline. The writer runs separately from the frame threads.

These measurements do not report ASW activity, actual displayed frames, or visual smoothness. Include the runtime's compositor counters and describe head movement when reporting a presentation problem.
