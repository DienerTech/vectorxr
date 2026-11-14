# Game gaze status in the headset

The **GAME GAZE** row in VectorXR's expanded and compact OSD reports application gaze reception.
It observes the application's standard OpenXR eye-gaze action spaces, even with
VectorXR Quadviews disabled. No extra actions, sessions or runtime queries are
created, and the game's pose results are not modified by the monitor.

- **Not requested by game:** the application did not request `XR_EXT_eye_gaze_interaction`.
- **Extension unavailable:** the application requested it but it was not enabled downstream.
- **Enabled / no pose queries yet:** enabled, but no application `xrLocateSpace` calls on a recognized eye-gaze space have been observed.
- **Receiving / poses changing** (green): recent valid, tracked orientations returned to the game have changed.
- **Receiving / poses unchanged** (amber): valid, tracked orientations are arriving but have not changed recently. This can mean fixation, headset removal or retained runtime data; it is not proof of failure.
- **Valid pose / not tracked** (amber): orientation is valid but the runtime does not mark it tracked.
- **Game receiving invalid pose** (red): the latest recognized gaze locate failed or returned an invalid orientation.
- **No recent game pose queries** (amber): no recognized gaze locate in two seconds. Loading, pause or focus changes may cause this.

The monitor compares only successive queries using the same gaze and base spaces.
World-relative pose changes may include head movement. Test with your head still.
This observes the API boundary, not calibration accuracy, physical camera sample
freshness, or whether the game uses gaze for its rendered focus region. Confirm
the last point with the game's own gaze visualization.

Only the standard action-space `xrLocateSpace` path is observed. Vendor gaze APIs,
batched space-location APIs and VectorXR's private Quadviews gaze queries are not
reported as application gaze reception. The row follows the existing OSD visibility
and compact/expanded controls. Its labels are currently English.
