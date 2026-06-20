# Using VectorXR

This guide walks through configuring VectorXR after installation. For install steps and the
project overview, see the [README](../README.md).

VectorXR has two parts working together: the **desktop app**, where you configure everything,
and the **OpenXR API layer**, which applies your settings at runtime while a VR app is running.
You do not need to keep the desktop app open for the layer to work — the layer reads the saved
config file directly.

## First run

1. Launch VectorXR from the Start menu or desktop shortcut.
2. The **Home** tab shows suite status: whether the VectorXR layer is registered, which
   Enhancements are active, and quick links into each module.
3. Make sure the suite is enabled. Each Enhancement (Depth, Pivot, Quadviews) also has its own
   on/off state, and individual profiles can opt specific apps in even when a module default is
   off.

Settings are stored locally under `%LOCALAPPDATA%\VectorXR`.

## Profiles and the Application Registry

VectorXR applies settings **per application**. The model has two layers:

- **Default profile** — the baseline for a module. Ships off by default.
- **Custom profiles** — target one or more registered applications. The first enabled custom
  profile that targets the running app turns that Enhancement on and applies its settings, even
  if the module default is off.

To set up a per-game profile:

1. Open the **Application Registry**. Register the VR app you want to target — either by adding
   its executable name manually, or by promoting an app VectorXR has already **seen** (the layer
   records executables it attaches to, so you can pick from real apps instead of guessing names).
2. In a module tab (Depth, Pivot, or Quadviews), add a custom profile and point it at that
   registered application.
3. Tune the profile's settings and save.

If two enabled profiles target the same app, the first enabled one wins; the app warns you about
the conflict so it is never silent.

## Depth

The Depth module adjusts stereo separation and convergence to change perceived scale and comfort.

- **Stereo boost** and **convergence** use bipolar sliders centered at neutral — move left or
  right of center to push the effect in either direction.
- Values are shown as percentages relative to neutral.
- Some headset-native OpenXR runtimes (such as Pimax's PiOpenXR) can ignore submitted depth
  geometry. If Depth seems to have no effect, open the in-app **Depth Compatibility** notice and
  try routing through the SteamVR pipeline instead.

## Pivot

The Pivot module enhances head rotation for seated and flight-sim VR, letting you see further to
the side or up/down than your physical neck allows.

- Configure **yaw** (left/right) and **pitch** (up/down) independently: rotation multiplier,
  deadzone, and maximum extra degrees.
- A shared **smoothing** setting softens the motion for both axes.
- Pivot eases in and out when toggled rather than snapping, controlled by the **activation ramp**
  (default 0.35s) in the Pivot *General* section.
- **Activation** can be *hold* or *toggle*, bound to a keyboard chord or a detected input device
  (joystick / HOTAS). Device bindings are stored by stable device identity so reconnecting in a
  different order does not break them.

## Quadviews

The Quadviews module drives foveated-style rendering, concentrating detail where you are looking.

- **Foveate** (inner) and **Peripheral** (outer) resolution are entered as a percentage of your
  headset's resolution (100% = headset default).
- A color-coded **performance budget** chip (green / amber / red) estimates render cost *before*
  you launch a game, with a blinking "Detrimental" warning when the budget exceeds 100%.
- Quadviews depends on OpenXR layer order. See the next section if it is not behaving.

## OpenXR layer management

The **OpenXR Layers** tab inspects the implicit API layers installed on your system. You can:

- see each layer's name, path, and signature status
- enable or disable layers
- reorder layers

Layer order matters for features like Pivot with quad views. If something is not working, check
that VectorXR sits in a sensible position relative to other OpenXR layers here.

## Settings, import/export, and logs

- **Import / export** your full configuration to move it between machines or back it up.
- **Validate** checks your config for problems; **reset** restores defaults.
- View **runtime logs** from inside the app to diagnose what the layer did during a session.
- Switch between **light, dark, and system** themes.
- Manage whether VectorXR tracks discovered (seen) apps.

## Updates

The **About** tab can check GitHub for the latest published release and open the release page
when a newer build is available. VectorXR does not auto-download updates — install them manually
from [GitHub Releases](https://github.com/DienerTech/vectorxr/releases/latest).

## Disabling or removing VectorXR

- Turn the suite off from Home to stop all Enhancements without uninstalling.
- Disable the VectorXR layer from the OpenXR Layers tab to take it out of the pipeline entirely.
- Uninstall from Windows to remove the app and unregister the API layer.
