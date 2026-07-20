# Using VectorXR

This guide walks you through VectorXR from a fresh install — nothing registered, nothing
turned on — to a working per-game setup. For install steps and the project overview, see the
[README](../README.md).

VectorXR has two parts working together:

- the **desktop app**, where you configure everything, and
- the **OpenXR API layer**, which applies your settings at runtime while a VR app is running.

You do not need to keep the desktop app open for the layer to work — the layer reads the saved
config file directly. Settings are stored locally under `%LOCALAPPDATA%\VectorXR`.

## What you see on first launch

![VectorXR Home tab on first run](screenshots/home.png)

On a clean install, the **Home** tab is a status dashboard and everything is off:

- The **Runtime Enabled** and **Layer Enabled** pills (top right) confirm the OpenXR layer is
  registered and ready for the next VR app launch. **System Health** summarizes the same thing.
- The **Active overview** lists the three Enhancements — **Quadviews**, **Pivot**, and
  **Depth**. On first run each shows *Default Profile: Disabled*, *Custom Profiles: 0 / 0
  enabled*, and *Status: Inactive*. That is expected — you haven't configured anything yet.
- The left sidebar holds app sections (Home, Settings, Application Registry, OpenXR Layers,
  About) and the three Enhancements, each with its own on/off toggle.
- The bar at the bottom is the **save bar**. Changes you make are staged until you click **Save
  Changes** (or **Discard**). Watch this bar — nothing you change takes effect until it's saved.

## How VectorXR decides what to apply

This is the core idea, so it's worth understanding before you start clicking. For each
Enhancement, settings resolve in layers:

1. **Runtime master switch** — *VectorXR Enabled* in **Settings**. If this is off, no
   Enhancement does anything, regardless of profiles. It is on by default.
2. **Default Profile** — one baseline per Enhancement. It *applies to applications without an
   enabled custom profile*. Ships **off**. Turn it on when you want an Enhancement to apply
   broadly to anything you haven't given a specific profile.
3. **Custom Profiles** — each targets one or more registered applications. **The first enabled
   custom profile that matches the running app wins** — it turns that Enhancement on for that
   app and applies its settings, *even if the Default Profile is off*.

So there are two ways to turn an Enhancement on for a game:

- **Default Profile on** → applies everywhere that doesn't have a custom profile (simple, global).
- **A custom profile targeting that game** → applies only to that game, and overrides the default.

If two enabled profiles target the same app, the first enabled one wins, and the app warns you
about the conflict so it is never silent.

## Step 1 — Confirm the runtime is enabled

![VectorXR Settings tab](screenshots/settings.png)

Open **Settings** and check that **VectorXR Enabled** is on (it is by default). This is the
single master switch for all Enhancements at runtime.

While you're here:

- **Track Discovered XR apps** is on by default. It records the executable name of OpenXR apps
  you launch so you can register them in one click later (see Step 2). All data stays local.
- Set your **theme** (System / Light / Dark), **log level**, and **log retention**.
- **Import / Export Config** moves your full configuration between machines or backs it up, and
  **Reset to Default** rebuilds the config and clears discovery data.

## Step 2 — Register the app you want to tune

![VectorXR Application Registry tab](screenshots/application-registry.png)

Custom profiles target apps by executable name, so first register the app in the **Application
Registry**. On first run it's empty ("No applications registered yet").

Two ways to add one:

- **Add Application** — enter the app manually (name + executable, e.g. `DCS.exe`).
- **From discovery (recommended)** — the **Where new apps come from** panel lists OpenXR apps
  VectorXR has *seen*. Launch your VR app once while VectorXR is active, come back, click
  **Refresh**, and register it in one click — no need to guess the executable name. Nothing here
  changes your settings until you actually register an app.

## Step 3 — Turn on an Enhancement

Open an Enhancement from the sidebar. Each one has the same shape: a **Default Profile** at the
top and a **Custom Profiles** list below. To apply it to the app you just registered, click
**Add Profile**, point the profile at that application, tune the values, and save. To apply it
broadly instead, just turn the **Default Profile** on.

The three Enhancements:

### Depth

![VectorXR Depth tab](screenshots/depth.png)

Depth adjusts stereo separation and convergence to change perceived scale and comfort.

- **Stereo Boost** and **Convergence** are bipolar sliders centered at neutral — move left or
  right of center to push the effect either way. Values read as a percentage relative to neutral.
- The **Depth Toggle Binding** at the top lets you toggle Depth on/off at runtime for quick A/B
  comparisons in headset.
- Start with small changes. Games with a **Force IPD**, virtual-IPD, stereo-separation, or
  world-scale setting can override Stereo Boost; disable that setting before testing Depth.
  In DCS, uncheck **Force IPD Distance** under **Options > VR**, fully restart DCS, and open
  **Depth Troubleshooting** for additional guidance if Stereo Boost still seems inactive.

### Pivot

![VectorXR Pivot tab](screenshots/pivot.png)

Pivot enhances head rotation for seated and flight-sim VR, letting you see further to the side or
up/down than your physical neck allows.

- In the **Activation** section, choose an **Activation Mode**. *Toggle* and *hold* require an
  activation **Binding** — a keyboard chord or a detected input device (joystick / HOTAS).
  *Always on* engages Pivot automatically without a binding. If you assign one, pressing it
  suspends and resumes automatic engagement.
- Tune **Yaw** and **Pitch** independently: rotation **Multiplier**, **Deadzone**, and **Max
  Extra** degrees. A shared **Smoothing** softens the motion for both axes.
- The **Activation Ramp** (default 0.35s) eases Pivot in and out when it engages or disengages
  rather than snapping the view.

Pivot and Quadviews are built to work together: because VectorXR computes both in one layer, the
foveated focus region stays locked to your gaze even while Pivot rotates your view. This
combination is VectorXR's signature capability — see [Why VectorXR](../README.md#why-vectorxr).

### Quadviews

![VectorXR Quadviews tab](screenshots/quadviews.png)

Quadviews drives foveated-style rendering, concentrating detail where you are looking. It is
marked **Experimental** and currently targets **DX11 quadview-capable apps**.

- **Focus Window** sets the size and offset of the high-detail region; **Resolution** sets
  **Foveate** (inner) and **Peripheral** (outer) resolution as a percentage of your headset's
  resolution (100% = headset default).
- The budget indicator at the top right estimates render cost *before* you launch — it reads as a
  **% of stereo pixels** and is color-coded, with a "Detrimental" warning when you exceed budget.
  Watch it while tuning to keep performance positive.
- **Tracking** controls eye-tracked focus (mode, smoothing, deadzone). Quadviews also depends on
  OpenXR layer order — see the next section if it misbehaves.

Quadviews and Pivot are designed to compose: unlike running separate foveation and neck-assist
layers, VectorXR keeps the foveated focus region aligned with your gaze while Pivot rotates the
view. See [Why VectorXR](../README.md#why-vectorxr).

When you're happy, click **Save Changes** in the bottom bar. Back on **Home**, the Active
overview will now show that Enhancement as **Active** for your app.

## OpenXR layer management

![VectorXR OpenXR Layer Manager tab](screenshots/openxr-layer-manager.png)

The **OpenXR Layers** tab manages the implicit API layers installed on your system across the
four Windows registry slices (Machine-wide / Per-user × 64-bit / 32-bit — *Machine-wide 64-bit*
is the recommended one for most PCVR). For each layer you can see its name, path, and signature
status, and you can enable, disable, or reorder it.

**Order matters.** For Pivot with quad views, the app notes that **Quad-Views-Foveated should be
above VectorXR**. If Pivot or Quadviews isn't behaving, check VectorXR's position here relative to
other OpenXR layers. (VectorXR's own layer currently shows as *unsigned* during the beta — that's
expected; see the README's status section.)

## Updates

![VectorXR About tab](screenshots/about.png)

The **About** tab shows project info, support links, the latest patch notes, and a **Release
status** panel that checks GitHub for the newest published release. VectorXR does not auto-download
updates — install them manually from
[GitHub Releases](https://github.com/DienerTech/vectorxr/releases/latest).

## Disabling or removing VectorXR

VectorXR is designed to be reversible at every level:

- **Pause everything** — turn *VectorXR Enabled* off in Settings to stop all Enhancements without
  uninstalling.
- **Per Enhancement** — toggle any Enhancement off from the sidebar, or turn off its profiles.
- **Take it out of the pipeline** — disable the VectorXR layer from the OpenXR Layers tab.
- **Remove it** — uninstall from Windows to remove the app and unregister the API layer.
