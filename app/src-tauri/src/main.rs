#![cfg_attr(not(debug_assertions), windows_subsystem = "windows")]

use serde::{Deserialize, Serialize};
use std::env;
use std::fs;
use std::path::{Path, PathBuf};
use std::process::Command;
use std::sync::atomic::{AtomicU64, Ordering};
use std::time::Duration;
use std::time::SystemTime;
use std::time::UNIX_EPOCH;

mod debug_sources;
mod gpu_capabilities;
mod input_devices;
mod openxr_layers;
mod single_instance;
mod tray;
mod user_error;

use user_error::user_error;

fn default_true() -> bool {
    true
}

fn default_false() -> bool {
    false
}

fn default_version() -> u32 {
    3
}

fn default_stereo_boost() -> f64 {
    1.0
}

fn default_convergence() -> f64 {
    0.0
}

fn default_log_level() -> String {
    "info".into()
}

fn default_log_retention_files() -> u32 {
    7
}

fn default_rotation_multiplier() -> f64 {
    1.5
}

fn default_activation_binding() -> InputBinding {
    InputBinding::None
}

#[derive(Debug, Clone, Default, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
struct SoundFeedback {
    #[serde(default)]
    enabled: bool,
    #[serde(default)]
    activate_sound: String,
    #[serde(default)]
    deactivate_sound: String,
}

impl SoundFeedback {
    // Configs that never opted into sounds round-trip without an empty `sound` block.
    fn is_unset(&self) -> bool {
        !self.enabled && self.activate_sound.is_empty() && self.deactivate_sound.is_empty()
    }
}

#[derive(Debug, Clone, Serialize, Deserialize)]
#[serde(tag = "type", rename_all = "camelCase")]
enum InputBinding {
    None,
    #[serde(rename_all = "camelCase")]
    Keyboard {
        #[serde(default)]
        chord: Vec<String>,
        #[serde(default, skip_serializing_if = "SoundFeedback::is_unset")]
        sound: SoundFeedback,
    },
    #[serde(rename_all = "camelCase")]
    Device {
        #[serde(default)]
        device_guid: String,
        #[serde(default = "default_input_path")]
        input_path: String,
        #[serde(default, skip_serializing_if = "String::is_empty")]
        product_guid: String,
        #[serde(default, skip_serializing_if = "String::is_empty")]
        device_name: String,
        #[serde(default, skip_serializing_if = "String::is_empty")]
        input_label: String,
        #[serde(default, skip_serializing_if = "SoundFeedback::is_unset")]
        sound: SoundFeedback,
    },
}
#[derive(Deserialize)]
#[serde(untagged)]
enum InputBindingsValue {
    One(InputBinding),
    Many(Vec<InputBinding>),
}

fn deserialize_input_bindings<'de, D>(deserializer: D) -> Result<Vec<InputBinding>, D::Error>
where
    D: serde::Deserializer<'de>,
{
    let value = InputBindingsValue::deserialize(deserializer)?;
    let mut bindings = match value {
        InputBindingsValue::One(binding) => vec![binding],
        InputBindingsValue::Many(bindings) => bindings,
    };
    bindings.retain(|binding| !matches!(binding, InputBinding::None));
    Ok(bindings)
}

// A toggle's bindings (any one of them acts; one per device, say a HOTAS's
// and a HOSAS's) as a config file holds them: one, or none, under the single
// legacy key, which layers from before binding lists also read; two or more
// under the plural key alone. Neither key present means the toggle's default
// (`absent`), as it does to the layer.
fn normalize_toggle_bindings(
    single: &mut Option<InputBinding>,
    list: &mut Option<Vec<InputBinding>>,
    absent: impl FnOnce() -> InputBinding,
) {
    let present = single.is_some() || list.is_some();
    let mut bindings: Vec<InputBinding> = single
        .take()
        .into_iter()
        .chain(list.take().unwrap_or_default())
        .filter(|binding| !matches!(binding, InputBinding::None))
        .collect();
    if !present {
        bindings = vec![absent()];
    }
    if bindings.len() <= 1 {
        *single = Some(bindings.pop().unwrap_or(InputBinding::None));
    } else {
        *list = Some(bindings);
    }
}

#[derive(Debug, Clone, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
struct PivotActivationBinding {
    behavior: String,
    binding: InputBinding,
}

#[derive(Debug, Clone, Serialize, Deserialize)]
#[serde(untagged)]
enum PivotActivationEntry {
    Canonical(PivotActivationBinding),
    Legacy(InputBinding),
}

#[derive(Deserialize)]
#[serde(untagged)]
enum PivotActivationBindingsValue {
    One(PivotActivationEntry),
    Many(Vec<PivotActivationEntry>),
}

fn deserialize_pivot_activation_bindings<'de, D>(
    deserializer: D,
) -> Result<Vec<PivotActivationEntry>, D::Error>
where
    D: serde::Deserializer<'de>,
{
    let value = PivotActivationBindingsValue::deserialize(deserializer)?;
    let mut bindings = match value {
        PivotActivationBindingsValue::One(binding) => vec![binding],
        PivotActivationBindingsValue::Many(bindings) => bindings,
    };
    bindings.retain(|entry| !matches!(entry, PivotActivationEntry::Legacy(InputBinding::None)));
    Ok(bindings)
}

fn default_input_path() -> String {
    "button-1".into()
}

fn default_smoothing() -> f64 {
    0.2
}

fn default_activation_ramp_seconds() -> f64 {
    0.35
}

fn default_deadzone_degrees() -> f64 {
    8.0
}

fn default_max_extra_yaw_degrees() -> f64 {
    120.0
}

fn default_pitch_rotation_multiplier() -> f64 {
    1.5
}

fn default_pitch_deadzone_degrees() -> f64 {
    8.0
}

fn default_max_extra_pitch_degrees() -> f64 {
    120.0
}

fn default_quadviews_tracking_mode() -> String {
    "eye".into()
}

fn default_focus_horizontal_size_percent() -> f64 {
    40.0
}

fn default_focus_vertical_size_percent() -> f64 {
    40.0
}

fn default_focus_scale() -> f64 {
    1.1
}

fn default_peripheral_scale() -> f64 {
    0.35
}

fn default_foveate_sharpness() -> f64 {
    50.0
}

fn default_transition_thickness_percent() -> f64 {
    25.0
}

fn default_gaze_smoothing() -> f64 {
    0.15
}

fn default_gaze_deadzone_degrees() -> f64 {
    1.5
}

#[derive(Debug, Clone, Serialize, Deserialize)]
struct ProfileMatch {
    exe: String,
}

impl Default for ProfileMatch {
    fn default() -> Self {
        Self {
            exe: "Game.exe".into(),
        }
    }
}

#[derive(Debug, Clone, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
struct RegisteredApplication {
    id: String,
    name: String,
    #[serde(default = "default_true")]
    enabled: bool,
    #[serde(default)]
    r#match: ProfileMatch,
}

fn default_sound_volume() -> u32 {
    100
}

#[derive(Debug, Clone, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
struct SoundSettings {
    #[serde(default = "default_sound_volume")]
    volume: u32,
}

impl Default for SoundSettings {
    fn default() -> Self {
        Self {
            volume: default_sound_volume(),
        }
    }
}

#[derive(Debug, Clone, Serialize, Deserialize)]
#[serde(rename_all = "camelCase", default)]
struct OsdSettings {
    enabled: bool,
    visible_on_start: bool,
    compact: bool,
    horizontal_degrees: f64,
    vertical_degrees: f64,
    distance_meters: f64,
    scale: f64,
    opacity: u32,
    update_hz: u32,
    show_graph: bool,
    show_runtime: bool,
    show_brand: bool,
    show_app: bool,
    show_turbo: bool,
    show_modules: bool,
    show_clock: bool,
    show_pivot: bool,
    show_framegen: bool,
    show_framegen_fps: bool,
    show_performance: bool,
    compact_show_runtime: bool,
    compact_show_brand: bool,
    compact_show_app: bool,
    compact_metrics: String,
    compact_show_clock: bool,
    compact_show_turbo: bool,
    compact_show_pivot: bool,
    compact_show_framegen: bool,
    compact_show_performance: bool,
    clock_format: String,
    // The expanded rows in order: the rows 0.19.0-0.19.2 layers know, in
    // bodyOrder, and the full order in rowOrder (split_osd_row_order).
    body_order: Vec<String>,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    row_order: Option<Vec<String>>,
    custom_color: String,
    custom_presets: Vec<serde_json::Value>,
    accent: String,
    // The OSD's language: a code, or "app" for the app's, which the page
    // writes as app_language. 0.19 layers ignore both keys.
    language: String,
    #[serde(default, skip_serializing_if = "String::is_empty")]
    app_language: String,
    // Toggle binding lists (normalize_toggle_bindings); absent: the defaults.
    #[serde(default, skip_serializing_if = "Option::is_none")]
    toggle_binding: Option<InputBinding>,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    toggle_bindings: Option<Vec<InputBinding>>,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    cycle_binding: Option<InputBinding>,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    cycle_bindings: Option<Vec<InputBinding>>,
}

fn default_osd_toggle_binding() -> InputBinding {
    InputBinding::Keyboard {
        chord: vec!["Ctrl".into(), "Alt".into(), "F10".into()],
        sound: SoundFeedback::default(),
    }
}

fn default_osd_cycle_binding() -> InputBinding {
    InputBinding::Keyboard {
        chord: vec!["Ctrl".into(), "Alt".into(), "F11".into()],
        sound: SoundFeedback::default(),
    }
}

impl Default for OsdSettings {
    fn default() -> Self {
        let binding = |key: &str| InputBinding::Keyboard {
            chord: vec!["Ctrl".into(), "Alt".into(), key.into()],
            sound: SoundFeedback::default(),
        };
        Self {
            enabled: false,
            visible_on_start: true,
            compact: false,
            horizontal_degrees: 20.0,
            vertical_degrees: -12.0,
            distance_meters: 1.2,
            scale: 100.0,
            opacity: 90,
            update_hz: 5,
            show_graph: true,
            show_runtime: true,
            show_brand: true,
            show_app: true,
            show_turbo: true,
            show_modules: true,
            show_clock: true,
            show_pivot: true,
            show_framegen: true,
            show_framegen_fps: true,
            show_performance: true,
            compact_show_runtime: true,
            compact_show_brand: true,
            compact_show_app: true,
            compact_metrics: "all".into(),
            compact_show_clock: true,
            compact_show_turbo: false,
            compact_show_pivot: false,
            compact_show_framegen: false,
            compact_show_performance: false,
            clock_format: "24".into(),
            // Already split (split_osd_row_order): the default file is
            // written without normalize_config.
            body_order: LEGACY_OSD_ROWS.iter().map(|row| row.to_string()).collect(),
            row_order: Some(
                ["graph", "performance", "turbo", "framegen", "pivot", "modules"]
                    .iter()
                    .map(|row| row.to_string())
                    .collect(),
            ),
            custom_color: "#51ddbd".into(),
            custom_presets: vec![],
            accent: "teal".into(),
            language: "app".into(),
            app_language: String::new(),
            toggle_binding: Some(binding("F10")),
            toggle_bindings: None,
            cycle_binding: Some(binding("F11")),
            cycle_bindings: None,
        }
    }
}

fn default_screenshot_eyes() -> String {
    "both".to_owned()
}

// Full-resolution screenshots: bindings that work in every game, which eyes
// to save ("both", "left" or "right") and an absolute folder; an empty folder
// is the default one (default_screenshot_folder) and is left out of the file.
// Optional in older configs: a missing block has no binding.
#[derive(Debug, Clone, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
struct ScreenshotSettings {
    #[serde(default, deserialize_with = "deserialize_input_bindings")]
    bindings: Vec<InputBinding>,
    #[serde(default = "default_screenshot_eyes")]
    eyes: String,
    #[serde(default, skip_serializing_if = "String::is_empty")]
    folder: String,
}

impl Default for ScreenshotSettings {
    fn default() -> Self {
        Self {
            bindings: Vec::new(),
            eyes: default_screenshot_eyes(),
            folder: String::new(),
        }
    }
}

#[derive(Debug, Clone, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
struct CoreConfig {
    #[serde(default)]
    osd: OsdSettings,
    #[serde(default)]
    screenshot: ScreenshotSettings,
    #[serde(default = "default_true")]
    enabled: bool,
    #[serde(default = "default_log_level")]
    log_level: String,
    #[serde(default = "default_log_retention_files")]
    log_retention_files: u32,
    #[serde(default = "default_true")]
    track_seen_apps: bool,
    #[serde(default)]
    sound: SoundSettings,
}

impl Default for CoreConfig {
    fn default() -> Self {
        Self {
            enabled: true,
            log_level: default_log_level(),
            osd: OsdSettings::default(),
            screenshot: ScreenshotSettings::default(),
            log_retention_files: default_log_retention_files(),
            track_seen_apps: true,
            sound: SoundSettings::default(),
        }
    }
}

#[derive(Debug, Clone, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
struct DepthXRSettings {
    #[serde(default = "default_stereo_boost")]
    stereo_boost: f64,
    #[serde(default = "default_convergence")]
    convergence: f64,
    #[serde(default = "default_false")]
    depth_anchor: bool,
}

impl Default for DepthXRSettings {
    fn default() -> Self {
        Self {
            stereo_boost: default_stereo_boost(),
            convergence: default_convergence(),
            // Fresh profiles/configs opt into Depth Lock. The field-level
            // serde default remains false so pre-0.14 configs retain their
            // previous submission behavior when the property is absent.
            depth_anchor: true,
        }
    }
}

#[derive(Debug, Clone, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
struct DepthXRProfileConfig {
    #[serde(default)]
    name: String,
    #[serde(default = "default_true")]
    enabled: bool,
    #[serde(default)]
    application_ids: Vec<String>,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    r#match: Option<ProfileMatch>,
    #[serde(default)]
    settings: DepthXRSettings,
}

#[derive(Debug, Clone, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
struct DepthXRModuleConfig {
    #[serde(default = "default_false")]
    enabled: bool,
    #[serde(default)]
    defaults: DepthXRSettings,
    #[serde(default)]
    bindings: DepthXRBindings,
    #[serde(default)]
    profiles: Vec<DepthXRProfileConfig>,
}

#[derive(Debug, Clone, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
struct DepthXRBindings {
    // Toggle binding lists (normalize_toggle_bindings).
    #[serde(default, skip_serializing_if = "Option::is_none")]
    toggle_enabled: Option<InputBinding>,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    toggle_enabled_bindings: Option<Vec<InputBinding>>,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    toggle_anchor: Option<InputBinding>,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    toggle_anchor_bindings: Option<Vec<InputBinding>>,
}

impl Default for DepthXRBindings {
    fn default() -> Self {
        Self {
            toggle_enabled: Some(default_depth_toggle_binding()),
            toggle_enabled_bindings: None,
            toggle_anchor: Some(default_depth_toggle_binding()),
            toggle_anchor_bindings: None,
        }
    }
}

fn default_depth_toggle_binding() -> InputBinding {
    InputBinding::None
}

impl Default for DepthXRModuleConfig {
    fn default() -> Self {
        Self {
            enabled: false,
            defaults: DepthXRSettings::default(),
            bindings: DepthXRBindings::default(),
            profiles: Vec::new(),
        }
    }
}

#[derive(Debug, Clone, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
struct PivotXRSettings {
    #[serde(default = "default_smoothing")]
    smoothing: f64,
    #[serde(default = "default_activation_ramp_seconds")]
    activation_ramp_seconds: f64,
    #[serde(default = "default_rotation_multiplier")]
    rotation_multiplier: f64,
    #[serde(default = "default_deadzone_degrees")]
    deadzone_degrees: f64,
    #[serde(default = "default_max_extra_yaw_degrees")]
    max_extra_yaw_degrees: f64,
    #[serde(default = "default_pitch_rotation_multiplier")]
    pitch_rotation_multiplier: f64,
    #[serde(default = "default_pitch_deadzone_degrees")]
    pitch_deadzone_degrees: f64,
    #[serde(default = "default_max_extra_pitch_degrees")]
    max_extra_pitch_degrees: f64,
    // Newer pivot settings (response mode, stepped params, advanced axes, and
    // anything added later) round-trip untouched; the frontend and the layer
    // own their schema.
    #[serde(flatten)]
    extra: serde_json::Map<String, serde_json::Value>,
}

impl Default for PivotXRSettings {
    fn default() -> Self {
        Self {
            smoothing: default_smoothing(),
            activation_ramp_seconds: default_activation_ramp_seconds(),
            rotation_multiplier: default_rotation_multiplier(),
            deadzone_degrees: default_deadzone_degrees(),
            max_extra_yaw_degrees: default_max_extra_yaw_degrees(),
            pitch_rotation_multiplier: default_pitch_rotation_multiplier(),
            pitch_deadzone_degrees: default_pitch_deadzone_degrees(),
            max_extra_pitch_degrees: default_max_extra_pitch_degrees(),
            extra: serde_json::Map::new(),
        }
    }
}

fn default_pivot_profile_behavior() -> String {
    "enhancedMotion".into()
}

#[derive(Debug, Clone, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
struct PivotXRProfileConfig {
    #[serde(default)]
    id: String,
    #[serde(default)]
    name: String,
    #[serde(default = "default_true")]
    enabled: bool,
    #[serde(default)]
    application_ids: Vec<String>,
    #[serde(default = "default_pivot_profile_behavior")]
    behavior: String,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    snap_turn_preference: Option<String>,
    #[serde(default)]
    nudge_set_id: String,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    allow_inactive_nudges: Option<bool>,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    r#match: Option<ProfileMatch>,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    activation_mode: Option<String>,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    always_active: Option<bool>,
    #[serde(
        default,
        alias = "activationBinding",
        deserialize_with = "deserialize_pivot_activation_bindings"
    )]
    activation_bindings: Vec<PivotActivationEntry>,
    #[serde(
        default,
        alias = "setOriginBinding",
        deserialize_with = "deserialize_input_bindings"
    )]
    set_origin_bindings: Vec<InputBinding>,
    #[serde(
        default,
        alias = "releaseOriginBinding",
        deserialize_with = "deserialize_input_bindings"
    )]
    release_origin_bindings: Vec<InputBinding>,
    #[serde(default)]
    settings: PivotXRSettings,
    #[serde(default)]
    view_controls: serde_json::Map<String, serde_json::Value>,
}

#[derive(Debug, Clone, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
struct PivotXRModuleConfig {
    #[serde(default = "default_false")]
    enabled: bool,
    #[serde(default)]
    defaults: PivotXRSettings,
    #[serde(default = "default_pivot_profile_behavior")]
    behavior: String,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    snap_turn_preference: Option<String>,
    #[serde(default)]
    nudge_set_id: String,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    allow_inactive_nudges: Option<bool>,
    #[serde(default)]
    nudge_sets: Vec<serde_json::Value>,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    activation_mode: Option<String>,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    always_active: Option<bool>,
    #[serde(
        default,
        alias = "activationBinding",
        deserialize_with = "deserialize_pivot_activation_bindings"
    )]
    activation_bindings: Vec<PivotActivationEntry>,
    #[serde(
        default,
        alias = "setOriginBinding",
        deserialize_with = "deserialize_input_bindings"
    )]
    set_origin_bindings: Vec<InputBinding>,
    #[serde(
        default,
        alias = "releaseOriginBinding",
        deserialize_with = "deserialize_input_bindings"
    )]
    release_origin_bindings: Vec<InputBinding>,
    #[serde(default)]
    view_controls: serde_json::Map<String, serde_json::Value>,
    #[serde(default)]
    profiles: Vec<PivotXRProfileConfig>,
}

impl Default for PivotXRModuleConfig {
    fn default() -> Self {
        Self {
            enabled: false,
            defaults: PivotXRSettings::default(),
            behavior: default_pivot_profile_behavior(),
            snap_turn_preference: None,
            nudge_set_id: String::new(),
            nudge_sets: Vec::new(),
            allow_inactive_nudges: None,
            activation_mode: None,
            always_active: Some(false),
            activation_bindings: Vec::new(),
            set_origin_bindings: Vec::new(),
            release_origin_bindings: Vec::new(),
            view_controls: serde_json::Map::new(),
            profiles: Vec::new(),
        }
    }
}

fn default_eye_tracking_correction() -> String {
    "default".to_owned()
}

#[derive(Debug, Clone, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
struct QuadViewsSettings {
    #[serde(default = "default_eye_tracking_correction")]
    eye_tracking_correction: String,
    #[serde(default = "default_quadviews_tracking_mode")]
    tracking_mode: String,
    #[serde(default = "default_focus_horizontal_size_percent")]
    focus_horizontal_size_percent: f64,
    #[serde(default = "default_focus_vertical_size_percent")]
    focus_vertical_size_percent: f64,
    #[serde(default = "default_focus_scale")]
    focus_scale: f64,
    #[serde(default = "default_peripheral_scale")]
    peripheral_scale: f64,
    #[serde(default = "default_foveate_sharpness")]
    foveate_sharpness: f64,
    #[serde(default = "default_transition_thickness_percent")]
    transition_thickness_percent: f64,
    #[serde(default)]
    horizontal_offset_degrees: f64,
    #[serde(default)]
    vertical_offset_degrees: f64,
    #[serde(default = "default_gaze_smoothing")]
    gaze_smoothing: f64,
    #[serde(default = "default_gaze_deadzone_degrees")]
    gaze_deadzone_degrees: f64,
}

impl Default for QuadViewsSettings {
    fn default() -> Self {
        Self {
            eye_tracking_correction: default_eye_tracking_correction(),
            tracking_mode: default_quadviews_tracking_mode(),
            focus_horizontal_size_percent: default_focus_horizontal_size_percent(),
            focus_vertical_size_percent: default_focus_vertical_size_percent(),
            focus_scale: default_focus_scale(),
            peripheral_scale: default_peripheral_scale(),
            foveate_sharpness: default_foveate_sharpness(),
            transition_thickness_percent: default_transition_thickness_percent(),
            horizontal_offset_degrees: 0.0,
            vertical_offset_degrees: 0.0,
            gaze_smoothing: default_gaze_smoothing(),
            gaze_deadzone_degrees: default_gaze_deadzone_degrees(),
        }
    }
}

#[derive(Debug, Clone, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
struct QuadViewsProfileConfig {
    #[serde(default)]
    name: String,
    #[serde(default = "default_true")]
    enabled: bool,
    #[serde(default)]
    application_ids: Vec<String>,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    r#match: Option<ProfileMatch>,
    #[serde(default)]
    settings: QuadViewsSettings,
}

#[derive(Debug, Clone, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
struct QuadViewsModuleConfig {
    #[serde(default = "default_false")]
    enabled: bool,
    #[serde(default = "default_activation_binding")]
    diagnostic_visualization_binding: InputBinding,
    #[serde(default)]
    defaults: QuadViewsSettings,
    #[serde(default)]
    profiles: Vec<QuadViewsProfileConfig>,
}

impl Default for QuadViewsModuleConfig {
    fn default() -> Self {
        Self {
            enabled: false,
            diagnostic_visualization_binding: InputBinding::None,
            defaults: QuadViewsSettings::default(),
            profiles: Vec::new(),
        }
    }
}

#[derive(Debug, Clone, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
struct TurboProfileConfig {
    #[serde(default, skip_serializing_if = "Option::is_none")]
    experimental: Option<TurboExperimentalSettings>,
    #[serde(default)]
    disable_safety: bool,
    #[serde(default)]
    id: String,
    #[serde(default)]
    name: String,
    #[serde(default = "default_true")]
    enabled: bool,
    #[serde(default)]
    application_ids: Vec<String>,
}

#[derive(Debug, Clone, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
struct TurboModuleConfig {
    #[serde(default)]
    experimental: TurboExperimentalSettings,
    #[serde(default = "default_true")]
    interrupted_session_recovery: bool,
    #[serde(default = "default_false")]
    enabled: bool,
    // Toggle binding lists (normalize_toggle_bindings), as is metrics_binding.
    #[serde(default, skip_serializing_if = "Option::is_none")]
    toggle_binding: Option<InputBinding>,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    toggle_bindings: Option<Vec<InputBinding>>,
    #[serde(default = "default_turbo_pacing_mode")]
    pacing_mode: String,
    #[serde(default)]
    runtime_pins: std::collections::BTreeMap<String, String>,
    #[serde(default = "default_turbo_metrics_mode")]
    metrics_mode: String,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    metrics_binding: Option<InputBinding>,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    metrics_bindings: Option<Vec<InputBinding>>,
    #[serde(default)]
    profiles: Vec<TurboProfileConfig>,
}

#[derive(Debug, Clone, Serialize, Deserialize)]
#[serde(rename_all = "camelCase", default)]
struct TurboExperimentalSettings {
    enabled: bool,
    application_ids: Vec<String>,
    wait_for_submit: bool,
    prediction_percent: u32,
    frame_limit: u32,
}

impl Default for TurboExperimentalSettings {
    fn default() -> Self {
        Self { enabled: false, application_ids: Vec::new(), wait_for_submit: false,
            prediction_percent: 100, frame_limit: 0 }
    }
}

fn default_turbo_pacing_mode() -> String {
    "auto".into()
}

fn default_turbo_metrics_mode() -> String {
    "always".into()
}

impl Default for TurboModuleConfig {
    fn default() -> Self {
        Self {
            interrupted_session_recovery: true,
            enabled: false,
            toggle_binding: Some(default_activation_binding()),
            toggle_bindings: None,
            experimental: TurboExperimentalSettings::default(),
            pacing_mode: default_turbo_pacing_mode(),
            runtime_pins: std::collections::BTreeMap::new(),
            metrics_mode: default_turbo_metrics_mode(),
            metrics_binding: Some(default_activation_binding()),
            metrics_bindings: None,
            profiles: Vec::new(),
        }
    }
}

fn default_framegen_synthesis_mode() -> String {
    "interpolate".into()
}

fn default_framegen_flow_quality() -> String {
    "balanced".into()
}

fn default_framegen_motion_estimation() -> String {
    "shader".into()
}

fn default_framegen_frame_multiplier() -> String {
    "dynamic".into()
}

fn default_framegen_artifact_suppression() -> i64 {
    10
}

fn default_framegen_motion_limit_percent() -> i64 {
    10
}

fn default_framegen_diagnostic_view() -> String {
    "off".into()
}

fn is_true(value: &bool) -> bool {
    *value
}

fn is_false(value: &bool) -> bool {
    !*value
}

// Frame Generation tuning. extendHeadPrediction is written only when on;
// absent means off in the layer and in the app (a profile does not inherit
// it from the defaults).
#[derive(Debug, Clone, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
struct FrameGenSettings {
    #[serde(default = "default_framegen_synthesis_mode")]
    synthesis_mode: String,
    #[serde(default = "default_framegen_flow_quality")]
    flow_quality: String,
    // "shader" or "hardware" (NVIDIA's optical flow accelerator, experimental).
    #[serde(default = "default_framegen_motion_estimation")]
    motion_estimation: String,
    #[serde(default = "default_framegen_interpolate_flow")]
    interpolate_flow: String,
    // "auto", "dynamic", "2x", "3x" or "4x": headset frames per real application frame.
    #[serde(default = "default_framegen_frame_multiplier")]
    frame_multiplier: String,
    #[serde(default = "default_framegen_artifact_suppression")]
    artifact_suppression: i64,
    #[serde(default = "default_framegen_motion_limit_percent")]
    motion_limit_percent: i64,
    // Head prediction past the runtime's limit.
    #[serde(default, skip_serializing_if = "is_false")]
    extend_head_prediction: bool,
    // Edge fill: margins past the game's field of view (written only when on).
    #[serde(default, skip_serializing_if = "is_false")]
    edge_fill: bool,
    // Use the game's depth (written only when off; absent means on).
    #[serde(default = "default_true", skip_serializing_if = "is_true")]
    use_game_depth: bool,
    // Settings this app does not know (another edition's, or a later
    // version's) round-trip untouched.
    #[serde(flatten)]
    extra: serde_json::Map<String, serde_json::Value>,
}

fn default_framegen_interpolate_flow() -> String { "auto".to_owned() }

impl Default for FrameGenSettings {
    fn default() -> Self {
        Self {
            synthesis_mode: default_framegen_synthesis_mode(),
            flow_quality: default_framegen_flow_quality(),
            motion_estimation: default_framegen_motion_estimation(),
            interpolate_flow: default_framegen_interpolate_flow(),
            frame_multiplier: default_framegen_frame_multiplier(),
            artifact_suppression: default_framegen_artifact_suppression(),
            motion_limit_percent: default_framegen_motion_limit_percent(),
            extend_head_prediction: false,
            edge_fill: false,
            use_game_depth: true,
            extra: serde_json::Map::new(),
        }
    }
}

#[derive(Debug, Clone, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
struct FrameGenProfileConfig {
    #[serde(default)]
    name: String,
    #[serde(default = "default_true")]
    enabled: bool,
    #[serde(default)]
    application_ids: Vec<String>,
    #[serde(default)]
    settings: FrameGenSettings,
}

// Optional in older configs: a missing `framegen` block resolves to Off.
#[derive(Debug, Clone, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
struct FrameGenModuleConfig {
    #[serde(default = "default_false")]
    enabled: bool,
    // A toggle binding list (normalize_toggle_bindings).
    #[serde(default, skip_serializing_if = "Option::is_none")]
    toggle_binding: Option<InputBinding>,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    toggle_bindings: Option<Vec<InputBinding>>,
    #[serde(default = "default_framegen_diagnostic_view")]
    diagnostic_view: String,
    #[serde(default)]
    defaults: FrameGenSettings,
    #[serde(default)]
    profiles: Vec<FrameGenProfileConfig>,
}

impl Default for FrameGenModuleConfig {
    fn default() -> Self {
        Self {
            enabled: false,
            toggle_binding: Some(default_activation_binding()),
            toggle_bindings: None,
            diagnostic_view: default_framegen_diagnostic_view(),
            defaults: FrameGenSettings::default(),
            profiles: Vec::new(),
        }
    }
}

fn bounded(value: f64, minimum: f64, maximum: f64, default: f64) -> f64 {
    if value.is_finite() {
        value.clamp(minimum, maximum)
    } else {
        default
    }
}


// Stabilized mirror window: the layer's own desktop window in the game's
// process, showing one eye's image with head shake smoothed out, for streaming
// and recording. The layer rejects the file when smoothing is outside 0-100,
// margin outside 0-40, width outside 320-3840, frameRate outside 15-120, or
// the eye ("left" | "right") or aspect ("16:9" | "16:10" | "4:3" | "1:1" |
// "21:9") is not one it knows. fill_monitor makes the picture fill `monitor`
// (a monitor's device name as the system reports it, such as \\.\DISPLAY2;
// empty for none chosen), borderless, instead of a resizable window.
#[derive(Debug, Clone, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
struct MirrorSettings {
    #[serde(default = "default_mirror_smoothing")]
    smoothing: f64,
    #[serde(default = "default_false")]
    level_horizon: bool,
    #[serde(default = "default_mirror_eye")]
    eye: String,
    #[serde(default = "default_mirror_margin")]
    margin: f64,
    #[serde(default = "default_mirror_aspect")]
    aspect: String,
    // A number written by hand as 1280.0, or one out of range, is not a
    // reason to refuse the whole file: it is rounded here and brought into
    // range with the rest (normalize_mirror_settings).
    #[serde(default = "default_mirror_width", deserialize_with = "deserialize_mirror_width")]
    width: u32,
    #[serde(default = "default_mirror_frame_rate", deserialize_with = "deserialize_mirror_frame_rate")]
    frame_rate: u32,
    #[serde(default = "default_false")]
    fill_monitor: bool,
    #[serde(default)]
    monitor: String,
}

fn default_mirror_smoothing() -> f64 {
    60.0
}

fn default_mirror_eye() -> String {
    "left".to_owned()
}

fn default_mirror_margin() -> f64 {
    15.0
}

fn default_mirror_aspect() -> String {
    "16:9".to_owned()
}

fn default_mirror_width() -> u32 {
    1280
}

fn default_mirror_frame_rate() -> u32 {
    60
}

// Any JSON number, rounded and kept to u32; anything else is the default.
fn lenient_u32(value: serde_json::Value, fallback: u32) -> u32 {
    match value.as_f64() {
        Some(number) if number.is_finite() => number.round().clamp(0.0, f64::from(u32::MAX)) as u32,
        _ => fallback,
    }
}

fn deserialize_mirror_width<'de, D: serde::Deserializer<'de>>(deserializer: D) -> Result<u32, D::Error> {
    Ok(lenient_u32(serde_json::Value::deserialize(deserializer)?, default_mirror_width()))
}

fn deserialize_mirror_frame_rate<'de, D: serde::Deserializer<'de>>(deserializer: D) -> Result<u32, D::Error> {
    Ok(lenient_u32(serde_json::Value::deserialize(deserializer)?, default_mirror_frame_rate()))
}

const MIRROR_EYES: [&str; 2] = ["left", "right"];
const MIRROR_ASPECTS: [&str; 5] = ["16:9", "16:10", "4:3", "1:1", "21:9"];
// The longest monitor name the layer accepts.
const MIRROR_MONITOR_MAX_LENGTH: usize = 64;

// Brings the mirror window's settings to what the layer accepts, as the app's
// own normalization does: numbers into their range as whole numbers, an
// unknown eye or aspect back to the default, and a monitor name longer than
// the layer takes to none chosen.
fn normalize_mirror_settings(settings: &mut MirrorSettings) {
    settings.smoothing = bounded(settings.smoothing, 0.0, 100.0, default_mirror_smoothing()).round();
    settings.margin = bounded(settings.margin, 0.0, 40.0, default_mirror_margin()).round();
    settings.width = settings.width.clamp(320, 3840);
    settings.frame_rate = settings.frame_rate.clamp(15, 120);
    if !MIRROR_EYES.contains(&settings.eye.as_str()) {
        settings.eye = default_mirror_eye();
    }
    if !MIRROR_ASPECTS.contains(&settings.aspect.as_str()) {
        settings.aspect = default_mirror_aspect();
    }
    if settings.monitor.encode_utf16().count() > MIRROR_MONITOR_MAX_LENGTH {
        settings.monitor.clear();
    }
}

impl Default for MirrorSettings {
    fn default() -> Self {
        Self {
            smoothing: default_mirror_smoothing(),
            level_horizon: false,
            eye: default_mirror_eye(),
            margin: default_mirror_margin(),
            aspect: default_mirror_aspect(),
            width: default_mirror_width(),
            frame_rate: default_mirror_frame_rate(),
            fill_monitor: false,
            monitor: String::new(),
        }
    }
}

#[derive(Debug, Clone, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
struct MirrorProfileConfig {
    #[serde(default)]
    name: String,
    #[serde(default = "default_true")]
    enabled: bool,
    #[serde(default)]
    application_ids: Vec<String>,
    #[serde(default)]
    settings: MirrorSettings,
}

// Optional in older configs: a missing `mirror` block resolves to Off. Any one
// of `bindings` shows or hides the window in the running game.
#[derive(Debug, Clone, Serialize, Deserialize, Default)]
#[serde(rename_all = "camelCase")]
struct MirrorModuleConfig {
    #[serde(default = "default_false")]
    enabled: bool,
    #[serde(default, deserialize_with = "deserialize_input_bindings")]
    bindings: Vec<InputBinding>,
    #[serde(default)]
    defaults: MirrorSettings,
    #[serde(default)]
    profiles: Vec<MirrorProfileConfig>,
}

#[derive(Debug, Clone, Serialize, Deserialize, Default)]
#[serde(rename_all = "camelCase")]
struct VectorXRModules {
    #[serde(default)]
    depthxr: DepthXRModuleConfig,
    #[serde(default)]
    pivotxr: PivotXRModuleConfig,
    #[serde(default)]
    quadviews: QuadViewsModuleConfig,
    #[serde(default)]
    turbo: TurboModuleConfig,
    #[serde(default)]
    framegen: FrameGenModuleConfig,
    #[serde(default)]
    mirror: MirrorModuleConfig,
    // Modules this app does not know (another edition's, or a later
    // version's) round-trip untouched.
    #[serde(flatten)]
    extra: serde_json::Map<String, serde_json::Value>,
}

#[derive(Debug, Clone, Serialize, Deserialize)]
struct VectorXRConfig {
    #[serde(default = "default_version")]
    version: u32,
    #[serde(default)]
    core: CoreConfig,
    #[serde(default)]
    applications: Vec<RegisteredApplication>,
    #[serde(default)]
    modules: VectorXRModules,
    // Anything else of another edition's or a later version's, untouched.
    #[serde(flatten)]
    extra: serde_json::Map<String, serde_json::Value>,
}

#[derive(Debug, Clone, Serialize)]
struct ConfigEnvelope {
    path: String,
    config: VectorXRConfig,
}

#[derive(Debug, Clone, Serialize)]
#[serde(rename_all = "camelCase")]
struct ResetStoredDataEnvelope {
    config_path: String,
    seen_apps_path: String,
    config: VectorXRConfig,
}

#[derive(Debug, Clone, Serialize)]
#[serde(rename_all = "camelCase")]
struct LogFileEntry {
    name: String,
    path: String,
    modified_unix_seconds: u64,
    content: String,
}

#[derive(Debug, Clone, Serialize)]
#[serde(rename_all = "camelCase")]
struct LogSnapshot {
    directory: String,
    active_path: String,
    files: Vec<LogFileEntry>,
}

#[derive(Debug, Clone, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
struct SeenApplication {
    #[serde(default)]
    exe: String,
    #[serde(default)]
    first_seen_unix_seconds: u64,
    #[serde(default)]
    last_seen_unix_seconds: u64,
    #[serde(default)]
    launch_count: u64,
}

#[derive(Debug, Clone, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
struct SeenAppsDocument {
    #[serde(default = "default_seen_apps_version")]
    version: u32,
    #[serde(default)]
    observations: Vec<SeenApplication>,
}

#[derive(Debug, Clone, Serialize)]
#[serde(rename_all = "camelCase")]
struct SeenAppsEnvelope {
    path: String,
    observations: Vec<SeenApplication>,
}

fn default_seen_apps_version() -> u32 {
    1
}

// Layer-written runtime-pacing sidecar (seen-apps pattern): per-runtime turbo
// pacing verdicts recorded by Auto discovery. Read-only facts for the UI —
// user intent (pacing mode, pins) lives in the config document.
#[derive(Debug, Clone, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
struct RuntimePacingObservation {
    #[serde(default)]
    test_started_at: u64,
    #[serde(default)]
    runtime_name: String,
    #[serde(default)]
    runtime_version: String,
    #[serde(default)]
    system_name: String,
    #[serde(default)]
    vendor_id: u32,
    #[serde(default)]
    graphics_api: String,
    #[serde(default)]
    mode: String,
    #[serde(default)]
    source: String,
    #[serde(default)]
    layer_version: String,
    #[serde(default)]
    first_used_unix_seconds: u64,
    #[serde(default)]
    last_used_unix_seconds: u64,
    #[serde(default)]
    probe_timeouts: u64,
    #[serde(default)]
    stable_seconds: u64,
}

#[derive(Debug, Clone, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
struct RuntimePacingDocument {
    #[serde(default = "default_seen_apps_version")]
    version: u32,
    #[serde(default)]
    observations: Vec<RuntimePacingObservation>,
}

#[derive(Debug, Clone, Serialize)]
#[serde(rename_all = "camelCase")]
struct RuntimePacingEnvelope {
    path: String,
    observations: Vec<RuntimePacingObservation>,
    active_runtime: Option<openxr_layers::ActiveRuntimeInfo>,
}

// Layer-written turbo-metrics sidecar (seen-apps pattern): per-session frame
// pacing stats segmented by pacing state (off/async/sequenced). Read-only
// facts for the Turbo panel's session metrics card.
#[derive(Debug, Clone, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
struct TurboMetricsBucket {
    #[serde(default)]
    state: String,
    #[serde(default)]
    frames: i64,
    #[serde(default)]
    seconds: f64,
    #[serde(default)]
    avg_fps: f64,
    #[serde(default)]
    avg_frame_ms: f64,
    #[serde(default)]
    p99_frame_ms: f64,
    #[serde(default)]
    max_frame_ms: f64,
    #[serde(default)]
    avg_wait_block_ms: f64,
    #[serde(default)]
    fabricated_waits: i64,
    #[serde(default)]
    drain_timeouts: i64,
    #[serde(default)]
    discarded_frames: i64,
}

#[derive(Debug, Clone, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
struct TurboMetricsSession {
    #[serde(default)]
    timing_configuration: String,
    #[serde(default)]
    session_id: String,
    #[serde(default)]
    app_name: String,
    #[serde(default)]
    runtime_name: String,
    #[serde(default)]
    layer_version: String,
    #[serde(default)]
    collection_mode: String,
    #[serde(default)]
    live: bool,
    #[serde(default)]
    started_unix_seconds: i64,
    #[serde(default)]
    updated_unix_seconds: i64,
    #[serde(default)]
    buckets: Vec<TurboMetricsBucket>,
}

#[derive(Debug, Clone, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
struct TurboMetricsDocument {
    #[serde(default = "default_seen_apps_version")]
    version: u32,
    #[serde(default)]
    sessions: Vec<TurboMetricsSession>,
}

#[derive(Debug, Clone, Serialize)]
#[serde(rename_all = "camelCase")]
struct TurboMetricsEnvelope {
    path: String,
    sessions: Vec<TurboMetricsSession>,
}

const RUNTIME_RELAY_PROTOCOL_VERSION: u32 = 1;
const RUNTIME_STATUS_FRESHNESS_MILLISECONDS: u64 = 3_500;
static RUNTIME_CONTROL_REVISION: AtomicU64 = AtomicU64::new(0);

#[derive(Debug, Clone, Serialize, Deserialize, Default)]
#[serde(rename_all = "camelCase")]
struct RuntimeCapabilities {
    #[serde(default)]
    osd: bool,
    #[serde(default)]
    quadviews_diagnostic_visualization: bool,
}

#[derive(Debug, Clone, Serialize, Deserialize, Default)]
#[serde(rename_all = "camelCase")]
struct RuntimeState {
    // The app tells a layer that does not report a feature ("Needs the
    // updated VectorXR layer") from one that reports it off by the state key
    // being absent, so the four feature states are left out when empty.
    #[serde(default)]
    osd_visible: bool,
    #[serde(default)]
    osd_compact: bool,
    #[serde(default)]
    osd_message: String,
    #[serde(default)]
    turbo_state: String,
    #[serde(default)]
    turbo_reason: String,
    // "off" | "waiting" | "active" | "paused" | "unavailable"; empty from
    // layers without Frame Generation.
    #[serde(default, skip_serializing_if = "String::is_empty")]
    framegen_state: String,
    #[serde(default)]
    framegen_reason: String,
    // "in-use" | "checking" | "not-sent" | "not-readable" | "empty" |
    // "unusable"; empty before generated frames and from older layers.
    #[serde(default)]
    framegen_depth: String,
    #[serde(default)]
    framegen_flow: String,
    #[serde(default)]
    framegen_flow_mode: String,
    #[serde(default)]
    framegen_flow_reason: String,
    // "off" | "pending" | "active" | "hidden" | "unsupported" | "failed";
    // empty from layers without the stabilized mirror window. The reason is
    // English text.
    #[serde(default, skip_serializing_if = "String::is_empty")]
    mirror_state: String,
    #[serde(default)]
    mirror_reason: String,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    mirror_visible: Option<bool>,
    #[serde(default)]
    quadviews_diagnostic_visualization: bool,
}

#[derive(Debug, Clone, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
struct RuntimeStatusDocument {
    #[serde(default)]
    quadviews_dimensions: Vec<QuadViewDimensions>,
    #[serde(default)]
    quadviews_dimensions_at: u64,
    protocol_version: u32,
    session_id: String,
    process_id: u32,
    application: String,
    updated_at_unix_milliseconds: u64,
    #[serde(default)]
    acknowledged_revision: u64,
    #[serde(default)]
    capabilities: RuntimeCapabilities,
    #[serde(default)]
    state: RuntimeState,
}

#[derive(Debug, Clone, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
struct QuadViewDimensions {
    width: u32,
    height: u32,
    allocated_width: u32,
    allocated_height: u32,
}

#[derive(Debug, Clone, Serialize)]
#[serde(rename_all = "camelCase")]
struct RuntimeStatusEnvelope {
    faults: Vec<serde_json::Value>,
    recovery: Vec<serde_json::Value>,
    sessions: Vec<RuntimeStatusDocument>,
}

#[derive(Debug, Clone, Serialize)]
#[serde(rename_all = "camelCase")]
struct RuntimeControlDocument {
    protocol_version: u32,
    target_session_id: String,
    revision: u64,
    expires_at_unix_milliseconds: u64,
    desired: RuntimeControlAction,
}

#[derive(Debug, Clone, Serialize)]
#[serde(rename_all = "camelCase")]
enum RuntimeControlAction {
    QuadviewsDiagnosticVisualization(bool),
    MirrorVisible(bool),
}

fn default_config() -> VectorXRConfig {
    VectorXRConfig {
        version: 3,
        core: CoreConfig::default(),
        applications: Vec::new(),
        modules: VectorXRModules::default(),
        extra: serde_json::Map::new(),
    }
}

fn sanitize_profile_name(exe: &str) -> String {
    let basename = exe.rsplit(['/', '\\']).next().unwrap_or(exe).trim();

    let without_extension = basename
        .rsplit_once('.')
        .map(|(stem, _)| stem)
        .unwrap_or(basename)
        .trim();

    if without_extension.is_empty() {
        "New Profile".into()
    } else {
        without_extension.into()
    }
}

fn sanitize_application_id(value: &str) -> String {
    let mut id = String::new();
    let mut last_was_dash = false;

    for character in value.trim().to_lowercase().chars() {
        if character.is_ascii_alphanumeric() {
            id.push(character);
            last_was_dash = false;
        } else if !last_was_dash && !id.is_empty() {
            id.push('-');
            last_was_dash = true;
        }
    }

    while id.ends_with('-') {
        id.pop();
    }

    if id.is_empty() {
        "application".into()
    } else {
        id
    }
}

fn normalize_exe_name(value: &str) -> String {
    value
        .rsplit(['/', '\\'])
        .next()
        .unwrap_or(value)
        .trim()
        .to_lowercase()
}

fn unique_application_id(base: &str, applications: &[RegisteredApplication]) -> String {
    let stem = sanitize_application_id(base);
    if !applications
        .iter()
        .any(|application| application.id.eq_ignore_ascii_case(&stem))
    {
        return stem;
    }

    let mut index = 2;
    loop {
        let candidate = format!("{stem}-{index}");
        if !applications
            .iter()
            .any(|application| application.id.eq_ignore_ascii_case(&candidate))
        {
            return candidate;
        }
        index += 1;
    }
}

fn application_id_for_profile_match(
    applications: &mut Vec<RegisteredApplication>,
    profile_name: &str,
    profile_match: &Option<ProfileMatch>,
) -> Option<String> {
    let exe = profile_match.as_ref()?.exe.trim();
    if exe.is_empty() {
        return None;
    }

    let normalized_exe = normalize_exe_name(exe);
    if let Some(application) = applications
        .iter()
        .find(|application| normalize_exe_name(&application.r#match.exe) == normalized_exe)
    {
        return Some(application.id.clone());
    }

    let name = if profile_name.trim().is_empty() || profile_name.trim() == "New Profile" {
        sanitize_profile_name(exe)
    } else {
        profile_name.trim().into()
    };

    let id = unique_application_id(&name, applications);
    applications.push(RegisteredApplication {
        id: id.clone(),
        name,
        enabled: true,
        r#match: ProfileMatch { exe: exe.into() },
    });

    Some(id)
}

// The OSD rows 0.19.0-0.19.2 layers know. They fail the whole config on any
// other name in bodyOrder (and in a preset's), turning every module off, but
// ignore an unknown key: so bodyOrder keeps only these rows, in order, and
// rowOrder holds the full order whenever it names another row.
const LEGACY_OSD_ROWS: [&str; 5] = ["graph", "turbo", "framegen", "pivot", "modules"];

fn split_osd_row_order(full: Vec<String>) -> (Vec<String>, Option<Vec<String>>) {
    let legacy: Vec<String> = full
        .iter()
        .filter(|row| LEGACY_OSD_ROWS.contains(&row.as_str()))
        .cloned()
        .collect();
    let row_order = (legacy != full).then_some(full);
    (legacy, row_order)
}

fn rows(value: Option<&serde_json::Value>) -> Option<Vec<String>> {
    value?
        .as_array()?
        .iter()
        .map(|row| row.as_str().map(str::to_string))
        .collect()
}

// A saved OSD layout (a custom preset's settings), split the same way.
fn split_preset_row_order(preset: &mut serde_json::Value) {
    let Some(settings) = preset.get_mut("settings").and_then(serde_json::Value::as_object_mut) else {
        return;
    };
    let Some(full) = rows(settings.get("rowOrder")).or_else(|| rows(settings.get("bodyOrder"))) else {
        return;
    };
    let (legacy, row_order) = split_osd_row_order(full);
    settings.insert("bodyOrder".into(), serde_json::json!(legacy));
    match row_order {
        Some(row_order) => settings.insert("rowOrder".into(), serde_json::json!(row_order)),
        None => settings.remove("rowOrder"),
    };
}

fn normalize_config(mut config: VectorXRConfig) -> VectorXRConfig {
    if config.version != 3 {
        return default_config();
    }

    config.version = 3;

    let osd = &mut config.core.osd;
    let full = osd.row_order.take().unwrap_or_else(|| osd.body_order.clone());
    (osd.body_order, osd.row_order) = split_osd_row_order(full);
    osd.custom_presets.iter_mut().for_each(split_preset_row_order);
    normalize_toggle_bindings(&mut osd.toggle_binding, &mut osd.toggle_bindings, default_osd_toggle_binding);
    normalize_toggle_bindings(&mut osd.cycle_binding, &mut osd.cycle_bindings, default_osd_cycle_binding);
    let screenshot = &mut config.core.screenshot;
    if !matches!(screenshot.eyes.as_str(), "both" | "left" | "right") {
        screenshot.eyes = default_screenshot_eyes();
    }
    screenshot.folder = screenshot.folder.trim().to_owned();
    let depth = &mut config.modules.depthxr.bindings;
    normalize_toggle_bindings(&mut depth.toggle_enabled, &mut depth.toggle_enabled_bindings, default_depth_toggle_binding);
    normalize_toggle_bindings(&mut depth.toggle_anchor, &mut depth.toggle_anchor_bindings, default_depth_toggle_binding);
    let turbo = &mut config.modules.turbo;
    normalize_toggle_bindings(&mut turbo.toggle_binding, &mut turbo.toggle_bindings, default_activation_binding);
    normalize_toggle_bindings(&mut turbo.metrics_binding, &mut turbo.metrics_bindings, default_activation_binding);
    let framegen = &mut config.modules.framegen;
    normalize_toggle_bindings(&mut framegen.toggle_binding, &mut framegen.toggle_bindings, default_activation_binding);

    for application in &mut config.applications {
        application.enabled = true;
        if application.id.trim().is_empty() {
            application.id = sanitize_application_id(&application.name);
        }
        if application.name.trim().is_empty() {
            application.name = sanitize_profile_name(&application.r#match.exe);
        }
        if application.r#match.exe.trim().is_empty() {
            application.r#match.exe = "Game.exe".into();
        }
    }

    for profile in &mut config.modules.depthxr.profiles {
        if profile.application_ids.is_empty() {
            if let Some(application_id) = application_id_for_profile_match(
                &mut config.applications,
                &profile.name,
                &profile.r#match,
            ) {
                profile.application_ids.push(application_id);
            }
        }

        if profile.name.trim().is_empty() {
            let first_application = profile.application_ids.first().and_then(|application_id| {
                config
                    .applications
                    .iter()
                    .find(|application| &application.id == application_id)
            });

            profile.name = first_application
                .map(|application| application.name.clone())
                .unwrap_or_else(|| "New Profile".into());
        }

        profile.r#match = None;
    }

    for profile in &mut config.modules.pivotxr.profiles {
        if profile.application_ids.is_empty() {
            if let Some(application_id) = application_id_for_profile_match(
                &mut config.applications,
                &profile.name,
                &profile.r#match,
            ) {
                profile.application_ids.push(application_id);
            }
        }

        if profile.name.trim().is_empty() {
            let first_application = profile.application_ids.first().and_then(|application_id| {
                config
                    .applications
                    .iter()
                    .find(|application| &application.id == application_id)
            });

            profile.name = first_application
                .map(|application| application.name.clone())
                .unwrap_or_else(|| "New Profile".into());
        }

        profile.r#match = None;
    }

    for profile in &mut config.modules.quadviews.profiles {
        if profile.application_ids.is_empty() {
            if let Some(application_id) = application_id_for_profile_match(
                &mut config.applications,
                &profile.name,
                &profile.r#match,
            ) {
                profile.application_ids.push(application_id);
            }
        }

        if profile.name.trim().is_empty() {
            let first_application = profile.application_ids.first().and_then(|application_id| {
                config
                    .applications
                    .iter()
                    .find(|application| &application.id == application_id)
            });

            profile.name = first_application
                .map(|application| application.name.clone())
                .unwrap_or_else(|| "New Profile".into());
        }

        profile.r#match = None;
    }

    for profile in &mut config.modules.framegen.profiles {
        if profile.name.trim().is_empty() {
            let first_application = profile.application_ids.first().and_then(|application_id| {
                config
                    .applications
                    .iter()
                    .find(|application| &application.id == application_id)
            });

            profile.name = first_application
                .map(|application| application.name.clone())
                .unwrap_or_else(|| "New Profile".into());
        }
    }
    // Numeric Frame Generation fields are clamped so hand-edited values do not
    // cause the layer to reject the whole config file.
    let framegen = &mut config.modules.framegen;
    framegen.defaults.artifact_suppression = framegen.defaults.artifact_suppression.clamp(0, 100);
    framegen.defaults.motion_limit_percent = framegen.defaults.motion_limit_percent.clamp(2, 25);
    for profile in &mut framegen.profiles {
        profile.settings.artifact_suppression = profile.settings.artifact_suppression.clamp(0, 100);
        profile.settings.motion_limit_percent = profile.settings.motion_limit_percent.clamp(2, 25);
    }
    normalize_mirror_settings(&mut config.modules.mirror.defaults);
    for profile in &mut config.modules.mirror.profiles {
        normalize_mirror_settings(&mut profile.settings);
        if profile.name.trim().is_empty() {
            let first_application = profile.application_ids.first().and_then(|application_id| {
                config
                    .applications
                    .iter()
                    .find(|application| &application.id == application_id)
            });

            profile.name = first_application
                .map(|application| application.name.clone())
                .unwrap_or_else(|| "New Profile".into());
        }
    }

    config
}

fn resolve_config_path() -> PathBuf {
    if let Ok(env_path) = env::var("VECTORXR_CONFIG_PATH") {
        if !env_path.trim().is_empty() {
            return PathBuf::from(env_path);
        }
    }

    #[cfg(target_os = "windows")]
    {
        if let Ok(local_app_data) = env::var("LOCALAPPDATA") {
            return PathBuf::from(local_app_data)
                .join("VectorXR")
                .join("config")
                .join("settings.json");
        }
    }

    env::current_dir()
        .unwrap_or_else(|_| PathBuf::from("."))
        .join("config")
        .join("vectorxr.settings.json")
}

fn resolve_log_path() -> PathBuf {
    if let Ok(env_path) = env::var("VECTORXR_LOG_PATH") {
        if !env_path.trim().is_empty() {
            return PathBuf::from(env_path);
        }
    }

    #[cfg(target_os = "windows")]
    {
        if let Ok(local_app_data) = env::var("LOCALAPPDATA") {
            return PathBuf::from(local_app_data)
                .join("VectorXR")
                .join("logs")
                .join("vectorxr.log");
        }
    }

    env::current_dir()
        .unwrap_or_else(|_| PathBuf::from("."))
        .join("logs")
        .join("vectorxr.log")
}

fn resolve_seen_apps_path() -> PathBuf {
    if let Ok(env_path) = env::var("VECTORXR_SEEN_APPS_PATH") {
        if !env_path.trim().is_empty() {
            return PathBuf::from(env_path);
        }
    }

    #[cfg(target_os = "windows")]
    {
        if let Ok(local_app_data) = env::var("LOCALAPPDATA") {
            return PathBuf::from(local_app_data)
                .join("VectorXR")
                .join("config")
                .join("seen-apps.json");
        }
    }

    env::current_dir()
        .unwrap_or_else(|_| PathBuf::from("."))
        .join("config")
        .join("seen-apps.json")
}

fn resolve_runtime_pacing_path() -> PathBuf {
    if let Ok(env_path) = env::var("VECTORXR_RUNTIME_PACING_PATH") {
        if !env_path.trim().is_empty() {
            return PathBuf::from(env_path);
        }
    }

    #[cfg(target_os = "windows")]
    {
        if let Ok(local_app_data) = env::var("LOCALAPPDATA") {
            return PathBuf::from(local_app_data)
                .join("VectorXR")
                .join("config")
                .join("runtime-pacing.json");
        }
    }

    env::current_dir()
        .unwrap_or_else(|_| PathBuf::from("."))
        .join("config")
        .join("runtime-pacing.json")
}

fn resolve_turbo_metrics_path() -> PathBuf {
    if let Ok(env_path) = env::var("VECTORXR_TURBO_METRICS_PATH") {
        if !env_path.trim().is_empty() {
            return PathBuf::from(env_path);
        }
    }

    #[cfg(target_os = "windows")]
    {
        if let Ok(local_app_data) = env::var("LOCALAPPDATA") {
            return PathBuf::from(local_app_data)
                .join("VectorXR")
                .join("config")
                .join("turbo-metrics.json");
        }
    }

    env::current_dir()
        .unwrap_or_else(|_| PathBuf::from("."))
        .join("config")
        .join("turbo-metrics.json")
}

fn resolve_runtime_relay_root() -> PathBuf {
    if let Ok(env_path) = env::var("VECTORXR_RUNTIME_RELAY_PATH") {
        if !env_path.trim().is_empty() {
            return PathBuf::from(env_path);
        }
    }
    #[cfg(target_os = "windows")]
    if let Ok(local_app_data) = env::var("LOCALAPPDATA") {
        return PathBuf::from(local_app_data)
            .join("VectorXR")
            .join("runtime");
    }
    env::current_dir()
        .unwrap_or_else(|_| PathBuf::from("."))
        .join("runtime")
}

fn unix_milliseconds() -> u64 {
    SystemTime::now()
        .duration_since(UNIX_EPOCH)
        .map(|duration| duration.as_millis() as u64)
        .unwrap_or_default()
}

fn read_turbo_metrics_document(path: &Path) -> TurboMetricsDocument {
    if !path.exists() {
        return TurboMetricsDocument {
            version: 1,
            sessions: Vec::new(),
        };
    }
    fs::read_to_string(path)
        .ok()
        .and_then(|content| serde_json::from_str(&content).ok())
        .unwrap_or(TurboMetricsDocument {
            version: 1,
            sessions: Vec::new(),
        })
}

fn read_runtime_pacing_document(path: &Path) -> RuntimePacingDocument {
    if !path.exists() {
        return RuntimePacingDocument {
            version: 1,
            observations: Vec::new(),
        };
    }
    let mut document = fs::read_to_string(path)
        .ok()
        .and_then(|content| serde_json::from_str::<RuntimePacingDocument>(&content).ok())
        .unwrap_or_else(|| RuntimePacingDocument {
            version: 1,
            observations: Vec::new(),
        });
    document.observations.retain(|observation| {
        let name: String = observation
            .runtime_name
            .as_bytes()
            .iter()
            .map(|byte| format!("{byte:02x}"))
            .collect();
        let reset_path = path
            .parent()
            .unwrap_or_else(|| Path::new("."))
            .join("runtime-pacing-resets")
            .join(format!("{name}.txt"));
        let reset = if reset_path.exists() {
            fs::read_to_string(reset_path)
                .ok()
                .and_then(|text| text.trim().parse::<u64>().ok())
                .unwrap_or(u64::MAX)
        } else {
            0
        };
        observation.test_started_at > reset
            && observation.source == "discovered"
            && observation.stable_seconds >= 60
            && (observation.mode == "async" || observation.mode == "sequenced")
    });
    document
}

fn ensure_parent(path: &Path) -> Result<(), String> {
    if let Some(parent) = path.parent() {
        fs::create_dir_all(parent).map_err(|error| error.to_string())?;
    }
    Ok(())
}

fn temporary_write_path(path: &Path) -> PathBuf {
    let file_name = path
        .file_name()
        .and_then(|value| value.to_str())
        .unwrap_or("vectorxr.tmp");
    let suffix = std::time::SystemTime::now()
        .duration_since(UNIX_EPOCH)
        .map(|duration| duration.as_nanos())
        .unwrap_or_default();

    path.with_file_name(format!(
        ".{file_name}.{}.{}.tmp",
        std::process::id(),
        suffix
    ))
}

fn write_text_safely(path: &Path, content: &str) -> Result<(), String> {
    ensure_parent(path)?;
    let temporary_path = temporary_write_path(path);

    if let Err(error) = fs::write(&temporary_path, content) {
        let _ = fs::remove_file(&temporary_path);
        return Err(error.to_string());
    }

    // Replacing fails for a moment while another process has either file open
    // without delete sharing: the layer reloading the settings, or an
    // antivirus scanning the file just written. Retry briefly, and never
    // delete the original first: the old fallback did, and when the replace
    // then failed again (the scan still running) both files were gone.
    const REPLACE_ATTEMPTS: u32 = 20;
    const REPLACE_RETRY_DELAY: std::time::Duration = std::time::Duration::from_millis(25);
    let mut attempt = 1;
    loop {
        match fs::rename(&temporary_path, path) {
            Ok(()) => return Ok(()),
            Err(_) if attempt < REPLACE_ATTEMPTS => {
                attempt += 1;
                std::thread::sleep(REPLACE_RETRY_DELAY);
            }
            Err(error) => {
                let _ = fs::remove_file(&temporary_path);
                return Err(user_error(
                    "configReplace",
                    &[("path", path.to_string_lossy().into_owned()), ("detail", error.to_string())],
                    format!("Unable to replace {}: {error}", path.to_string_lossy()),
                ));
            }
        }
    }
}

fn config_backup_path(path: &Path) -> PathBuf {
    let stem = path
        .file_stem()
        .and_then(|value| value.to_str())
        .unwrap_or("settings");
    let extension = path
        .extension()
        .and_then(|value| value.to_str())
        .map(|value| format!(".{value}"))
        .unwrap_or_default();
    path.with_file_name(format!("{stem}.backup{extension}"))
}

// Keep the exact config that predates the first 0.16 save. The backup is
// deliberately write-once so later saves cannot replace the user's migration
// recovery point with an already-normalized document.
fn ensure_config_backup(path: &Path) -> Result<(), String> {
    if !path.exists() {
        return Ok(());
    }

    let backup_path = config_backup_path(path);
    if backup_path.exists() {
        return Ok(());
    }

    ensure_parent(&backup_path)?;
    fs::copy(path, &backup_path).map_err(|error| {
        format!(
            "Unable to preserve the existing config at {}: {error}",
            backup_path.to_string_lossy()
        )
    })?;
    Ok(())
}

fn normalize_exe_display(value: &str) -> String {
    value
        .rsplit(['/', '\\'])
        .next()
        .unwrap_or(value)
        .trim()
        .into()
}

fn normalize_seen_apps_document(mut document: SeenAppsDocument) -> SeenAppsDocument {
    document.version = 1;
    let mut observations = Vec::<SeenApplication>::new();

    for mut observation in document.observations {
        observation.exe = normalize_exe_display(&observation.exe);
        if observation.exe.is_empty() {
            continue;
        }

        if observation.first_seen_unix_seconds == 0 {
            observation.first_seen_unix_seconds = observation.last_seen_unix_seconds;
        }
        if observation.last_seen_unix_seconds == 0 {
            observation.last_seen_unix_seconds = observation.first_seen_unix_seconds;
        }
        if observation.launch_count == 0 {
            observation.launch_count = 1;
        }

        let normalized_exe = normalize_exe_name(&observation.exe);
        if let Some(existing) = observations
            .iter_mut()
            .find(|existing| normalize_exe_name(&existing.exe) == normalized_exe)
        {
            existing.first_seen_unix_seconds = existing
                .first_seen_unix_seconds
                .min(observation.first_seen_unix_seconds);
            existing.last_seen_unix_seconds = existing
                .last_seen_unix_seconds
                .max(observation.last_seen_unix_seconds);
            existing.launch_count += observation.launch_count;
            continue;
        }

        observations.push(observation);
    }

    observations.sort_by(|lhs, rhs| rhs.last_seen_unix_seconds.cmp(&lhs.last_seen_unix_seconds));
    document.observations = observations;
    document
}

fn ensure_default_file(path: &Path) -> Result<(), String> {
    if path.exists() {
        return Ok(());
    }

    ensure_parent(path)?;
    let json =
        serde_json::to_string_pretty(&default_config()).map_err(|error| error.to_string())?;
    write_text_safely(path, &json)
}

fn is_log_timestamp_suffix(value: &str) -> bool {
    value.len() == 15
        && value.chars().enumerate().all(|(index, character)| {
            if index == 8 {
                character == '-'
            } else {
                character.is_ascii_digit()
            }
        })
}

fn log_series_paths(base_path: &Path) -> Result<Vec<PathBuf>, String> {
    let directory = base_path.parent().unwrap_or_else(|| Path::new("."));
    if !directory.exists() {
        return Ok(Vec::new());
    }

    let stem = base_path
        .file_stem()
        .and_then(|value| value.to_str())
        .unwrap_or("vectorxr")
        .to_string();
    let extension = base_path
        .extension()
        .and_then(|value| value.to_str())
        .map(|value| format!(".{value}"))
        .unwrap_or_else(|| ".log".into());

    let mut files = Vec::new();
    for entry in fs::read_dir(directory).map_err(|error| error.to_string())? {
        let entry = entry.map_err(|error| error.to_string())?;
        let path = entry.path();
        if !path.is_file() {
            continue;
        }

        let Some(file_stem) = path.file_stem().and_then(|value| value.to_str()) else {
            continue;
        };
        let file_extension = path
            .extension()
            .and_then(|value| value.to_str())
            .map(|value| format!(".{value}"))
            .unwrap_or_default();

        if file_extension != extension {
            continue;
        }

        let timestamp_suffix = file_stem
            .strip_prefix(&(stem.clone() + "-"))
            .is_some_and(is_log_timestamp_suffix);

        if file_stem == stem || timestamp_suffix {
            files.push(path);
        }
    }

    files.sort_by(|lhs, rhs| {
        let lhs_modified = fs::metadata(lhs)
            .and_then(|metadata| metadata.modified())
            .unwrap_or(UNIX_EPOCH);
        let rhs_modified = fs::metadata(rhs)
            .and_then(|metadata| metadata.modified())
            .unwrap_or(UNIX_EPOCH);
        rhs_modified.cmp(&lhs_modified)
    });

    Ok(files)
}

fn read_log_preview(path: &Path) -> String {
    use std::io::{Read, Seek, SeekFrom};
    const MAX_BYTES: u64 = 120_000;
    let read_tail = || -> std::io::Result<(bool, Vec<u8>)> {
        let mut file = fs::File::open(path)?;
        let length = file.metadata()?.len();
        file.seek(SeekFrom::Start(length.saturating_sub(MAX_BYTES)))?;
        let mut bytes = Vec::new();
        file.take(MAX_BYTES).read_to_end(&mut bytes)?;
        Ok((length > MAX_BYTES, bytes))
    };
    match read_tail() {
        Ok((truncated, bytes)) => format!(
            "{}{}",
            if truncated {
                "... truncated to the most recent log output ...\n"
            } else {
                ""
            },
            String::from_utf8_lossy(&bytes)
        ),
        Err(_) => "Unable to read this log file.".into(),
    }
}

#[tauri::command]
fn load_config() -> Result<ConfigEnvelope, String> {
    let path = resolve_config_path();
    ensure_default_file(&path)?;

    // Name the file: the app shows this text, and a JSON error's line and
    // column mean nothing without it.
    let content = fs::read_to_string(&path).map_err(|error| {
        user_error(
            "configRead",
            &[("path", path.to_string_lossy().into_owned()), ("detail", error.to_string())],
            format!("Unable to read {}: {error}", path.to_string_lossy()),
        )
    })?;
    let config = normalize_config(parse_config(&content).map_err(|error| {
        user_error(
            "configParse",
            &[("path", path.to_string_lossy().into_owned()), ("detail", error.to_string())],
            format!("Unable to parse {}: {error}", path.to_string_lossy()),
        )
    })?);

    Ok(ConfigEnvelope {
        path: path.to_string_lossy().into_owned(),
        config,
    })
}

// The layer reads a whole number written as a float (a hand-edited
// "opacity": 90.0) into its integer settings; serde rejects it for the app's
// u32 fields, so the app could not open a config the layer runs with. Such
// numbers are read as integers on a second try; float fields take integers
// unchanged. The first try's error is returned, as it carries the line and
// column.
fn parse_config(content: &str) -> Result<VectorXRConfig, serde_json::Error> {
    fn whole_floats_as_integers(value: &mut serde_json::Value) {
        match value {
            serde_json::Value::Number(number) if number.is_f64() => {
                let float = number.as_f64().unwrap_or(f64::NAN);
                if float.fract() == 0.0 && float.abs() <= 9_007_199_254_740_992.0 {
                    *value = serde_json::Value::from(float as i64);
                }
            }
            serde_json::Value::Array(items) => items.iter_mut().for_each(whole_floats_as_integers),
            serde_json::Value::Object(fields) => {
                fields.values_mut().for_each(whole_floats_as_integers)
            }
            _ => {}
        }
    }

    let error = match serde_json::from_str::<VectorXRConfig>(content) {
        Ok(config) => return Ok(config),
        Err(error) => error,
    };
    let Ok(mut value) = serde_json::from_str::<serde_json::Value>(content) else {
        return Err(error);
    };
    whole_floats_as_integers(&mut value);
    serde_json::from_value::<VectorXRConfig>(value).map_err(|_| error)
}

// Copies a config the app cannot parse to settings.unreadable-<ms>.json
// before it is replaced. The pre-0.16 backup is written once, so it does not
// hold a later hand edit (a JSON slip, or a value of the wrong type).
fn preserve_unreadable_config(path: &Path) -> Result<Option<PathBuf>, String> {
    if !path.exists() {
        return Ok(None);
    }
    let readable = fs::read_to_string(path)
        .ok()
        .is_some_and(|content| parse_config(&content).is_ok());
    if readable {
        return Ok(None);
    }

    let stem = path
        .file_stem()
        .and_then(|value| value.to_str())
        .unwrap_or("settings");
    let extension = path
        .extension()
        .and_then(|value| value.to_str())
        .unwrap_or("json");
    let preserved = path.with_file_name(format!(
        "{stem}.unreadable-{}.{extension}",
        unix_milliseconds()
    ));
    fs::copy(path, &preserved).map_err(|error| {
        format!(
            "Unable to keep a copy of the unreadable config at {}: {error}",
            preserved.to_string_lossy()
        )
    })?;
    Ok(Some(preserved))
}

fn write_config_document(path: &Path, content: &str) -> Result<(), String> {
    preserve_unreadable_config(path)?;
    ensure_config_backup(path)?;
    write_text_safely(path, content)
}

#[tauri::command]
fn save_config(config: VectorXRConfig) -> Result<String, String> {
    let path = resolve_config_path();

    let content = serde_json::to_string_pretty(&normalize_config(config))
        .map_err(|error| error.to_string())?;
    write_config_document(&path, &content)?;
    Ok(path.to_string_lossy().into_owned())
}

#[tauri::command]
fn reset_stored_data() -> Result<ResetStoredDataEnvelope, String> {
    let config_path = resolve_config_path();
    let seen_apps_path = resolve_seen_apps_path();
    let config = default_config();

    let config_content =
        serde_json::to_string_pretty(&config).map_err(|error| error.to_string())?;
    write_config_document(&config_path, &config_content)?;

    let seen_apps_content = serde_json::to_string_pretty(&SeenAppsDocument {
        version: 1,
        observations: Vec::new(),
    })
    .map_err(|error| error.to_string())?;
    write_text_safely(&seen_apps_path, &seen_apps_content)?;

    Ok(ResetStoredDataEnvelope {
        config_path: config_path.to_string_lossy().into_owned(),
        seen_apps_path: seen_apps_path.to_string_lossy().into_owned(),
        config,
    })
}

// Whether any graphics card has NVIDIA's optical flow hardware (Frame
// Generation's motion source), checked once, off the UI thread.
#[tauri::command]
async fn load_gpu_capabilities() -> gpu_capabilities::GpuCapabilities {
    tauri::async_runtime::spawn_blocking(gpu_capabilities::load)
        .await
        .unwrap_or_else(|error| gpu_capabilities::GpuCapabilities {
            probed: false,
            adapters: Vec::new(),
            optical_flow: false,
            error: format!("the graphics card check failed ({error})"),
        })
}

// Tauri runs a command that is not async on the app's main thread, which
// also runs the window: enumerating DirectInput devices (about 240 ms, every
// 5 s while a device binding is shown), a UAC prompt with its elevated helper
// (up to 120 s), or a sound preview froze the whole app. These run on a
// blocking worker instead.
async fn run_blocking<T: Send + 'static>(
    work: impl FnOnce() -> Result<T, String> + Send + 'static,
) -> Result<T, String> {
    tauri::async_runtime::spawn_blocking(work)
        .await
        .map_err(|error| error.to_string())?
}

#[tauri::command]
async fn list_input_devices() -> Result<Vec<input_devices::InputDeviceInfo>, String> {
    run_blocking(input_devices::list_input_devices).await
}

#[tauri::command]
async fn capture_device_binding(
    timeout_ms: Option<u64>,
    device_guid: String,
) -> Result<Option<input_devices::CapturedDeviceBinding>, String> {
    let capture_id = input_devices::begin_device_binding_capture();
    tauri::async_runtime::spawn_blocking(move || {
        input_devices::capture_device_binding(timeout_ms, capture_id, device_guid)
    })
    .await
    .map_err(|error| format!("Device binding capture task failed: {error}"))?
}

#[tauri::command]
fn cancel_device_binding_capture() {
    input_devices::cancel_device_binding_capture();
}

#[tauri::command]
async fn load_log_snapshot() -> Result<LogSnapshot, String> {
    tauri::async_runtime::spawn_blocking(read_log_snapshot)
        .await
        .map_err(|error| error.to_string())?
}

fn read_log_snapshot() -> Result<LogSnapshot, String> {
    let base_path = resolve_log_path();
    let directory = base_path
        .parent()
        .unwrap_or_else(|| Path::new("."))
        .to_path_buf();
    let files = log_series_paths(&base_path)?
        .into_iter()
        .take(10)
        .map(|path| {
            let modified_unix_seconds = fs::metadata(&path)
                .and_then(|metadata| metadata.modified())
                .ok()
                .and_then(|time| time.duration_since(UNIX_EPOCH).ok())
                .map(|duration| duration.as_secs())
                .unwrap_or_default();

            let name = path
                .file_name()
                .and_then(|value| value.to_str())
                .unwrap_or("log")
                .to_string();

            LogFileEntry {
                name,
                path: path.to_string_lossy().into_owned(),
                modified_unix_seconds,
                content: read_log_preview(&path),
            }
        })
        .collect::<Vec<_>>();

    let active_path = files
        .first()
        .map(|entry| entry.path.clone())
        .unwrap_or_else(|| base_path.to_string_lossy().into_owned());

    Ok(LogSnapshot {
        directory: directory.to_string_lossy().into_owned(),
        active_path,
        files,
    })
}

#[tauri::command]
fn open_file_directory(path: String) -> Result<(), String> {
    let path = PathBuf::from(path);
    let directory = path
        .parent()
        .ok_or_else(|| "Unable to determine file directory".to_string())?;

    if !directory.exists() {
        return Err(user_error(
            "directoryMissing",
            &[("path", directory.to_string_lossy().into_owned())],
            format!("Directory does not exist: {}", directory.to_string_lossy()),
        ));
    }

    show_directory(directory)
}

// Shows a folder in the file manager. On Windows the shell opens the folder
// itself: explorer.exe's command line would read a comma or a leading slash
// in the path as its own options (/select, /root and the like).
#[cfg(target_os = "windows")]
fn show_directory(directory: &Path) -> Result<(), String> {
    use std::os::windows::ffi::OsStrExt;
    use windows::core::{w, PCWSTR};
    use windows::Win32::UI::Shell::ShellExecuteW;
    use windows::Win32::UI::WindowsAndMessaging::SW_SHOWNORMAL;

    let wide: Vec<u16> = directory.as_os_str().encode_wide().chain(std::iter::once(0)).collect();
    if wide[..wide.len() - 1].contains(&0) {
        return Err(format!("Unable to open {}", directory.to_string_lossy()));
    }
    // The shell may hand the folder to its extensions, which need COM on the
    // calling thread (a worker's, not the UI's).
    let com = unsafe {
        windows::Win32::System::Com::CoInitializeEx(
            None,
            windows::Win32::System::Com::COINIT_APARTMENTTHREADED | windows::Win32::System::Com::COINIT_DISABLE_OLE1DDE,
        )
    };
    let result = unsafe {
        ShellExecuteW(None, w!("open"), PCWSTR(wide.as_ptr()), PCWSTR::null(), PCWSTR::null(), SW_SHOWNORMAL)
    };
    if com.is_ok() {
        unsafe { windows::Win32::System::Com::CoUninitialize() };
    }
    // ShellExecute reports success as a value above 32.
    if result.0 as isize > 32 {
        Ok(())
    } else {
        Err(format!("Unable to open {} (shell error {})", directory.to_string_lossy(), result.0 as isize))
    }
}

#[cfg(not(target_os = "windows"))]
fn show_directory(directory: &Path) -> Result<(), String> {
    #[cfg(target_os = "macos")]
    let program = "open";
    #[cfg(not(target_os = "macos"))]
    let program = "xdg-open";

    Command::new(program)
        .arg(directory)
        .spawn()
        .map(|_| ())
        .map_err(|error| error.to_string())
}

// The Windows Pictures folder, wherever the user keeps it (it can be moved
// off the profile, or into a synced folder), as the layer resolves it.
#[cfg(target_os = "windows")]
fn pictures_folder() -> Option<PathBuf> {
    use windows::Win32::System::Com::CoTaskMemFree;
    use windows::Win32::UI::Shell::{FOLDERID_Pictures, SHGetKnownFolderPath, KF_FLAG_DEFAULT};

    let known = unsafe { SHGetKnownFolderPath(&FOLDERID_Pictures, KF_FLAG_DEFAULT, None) }
        .ok()
        .and_then(|path| {
            let text = unsafe { path.to_string() }.ok();
            unsafe { CoTaskMemFree(Some(path.as_ptr() as *const core::ffi::c_void)) };
            text
        })
        .filter(|path| !path.trim().is_empty())
        .map(PathBuf::from);
    known.or_else(|| {
        env::var("USERPROFILE")
            .ok()
            .filter(|profile| !profile.trim().is_empty())
            .map(|profile| PathBuf::from(profile).join("Pictures"))
    })
}

#[cfg(not(target_os = "windows"))]
fn pictures_folder() -> Option<PathBuf> {
    env::var("HOME")
        .ok()
        .filter(|home| !home.trim().is_empty())
        .map(|home| PathBuf::from(home).join("Pictures"))
}

// %LOCALAPPDATA%, where the layer keeps its own files.
fn local_app_data_folder() -> Option<PathBuf> {
    env::var("LOCALAPPDATA")
        .ok()
        .filter(|path| !path.trim().is_empty())
        .map(PathBuf::from)
}

// Where screenshots go, as the layer decides it (layer/src/screenshot.cpp,
// ScreenshotFolder): `custom` (core.screenshot.folder) when it is a full
// path, else VectorXR under `pictures`, else VectorXR\screenshots under
// `local_app_data` when there is no Pictures folder. The layer adds a folder
// per game under either.
#[derive(Debug, PartialEq)]
struct ScreenshotFolder {
    path: PathBuf,
    // The folder VectorXR makes for itself, not one the user named.
    is_default: bool,
}

fn screenshot_folder_in(
    pictures: Option<PathBuf>,
    local_app_data: Option<PathBuf>,
    custom: Option<&str>,
) -> Result<ScreenshotFolder, String> {
    if let Some(path) = custom.and_then(custom_screenshot_folder) {
        return Ok(ScreenshotFolder { path, is_default: false });
    }
    pictures
        .map(|pictures| pictures.join("VectorXR"))
        .or_else(|| local_app_data.map(|local| local.join("VectorXR").join("screenshots")))
        .map(|path| ScreenshotFolder { path, is_default: true })
        .ok_or_else(|| "Unable to find the Pictures folder".to_string())
}

// A folder from the settings as a path to open: forward slashes become
// backslashes on Windows and "." and ".." are resolved, without touching the
// disk. None for a path that is not a full one (the layer does not use it and
// writes to the default folder) or that holds a character no path can.
fn custom_screenshot_folder(custom: &str) -> Option<PathBuf> {
    let text = custom.trim();
    if text.is_empty() || text.chars().any(|character| character.is_control()) {
        return None;
    }
    #[cfg(target_os = "windows")]
    let text = text.replace('/', "\\");
    let folder = PathBuf::from(text);
    if !folder.is_absolute() {
        return None;
    }
    std::path::absolute(&folder).ok().filter(|path| path.is_absolute())
}

// Makes sure the folder is there to open. VectorXR's default folder is
// created: no screenshot may have been taken yet. A folder the user named is
// not, because it may not be saved yet (or ever): the layer creates it with
// the first screenshot.
fn prepare_screenshot_folder(folder: &ScreenshotFolder) -> Result<(), String> {
    let text = || folder.path.to_string_lossy().into_owned();
    if folder.is_default {
        return fs::create_dir_all(&folder.path).map_err(|error| {
            user_error(
                "folderCreate",
                &[("path", text()), ("detail", error.to_string())],
                format!("Unable to create {}: {error}", text()),
            )
        });
    }
    if folder.path.is_dir() {
        return Ok(());
    }
    Err(user_error(
        "directoryMissing",
        &[("path", text())],
        format!("Directory does not exist: {}", text()),
    ))
}

// The folder screenshots go to when core.screenshot.folder is not set.
#[tauri::command]
fn default_screenshot_folder() -> Result<String, String> {
    screenshot_folder_in(pictures_folder(), local_app_data_folder(), None)
        .map(|folder| folder.path.to_string_lossy().into_owned())
}

// Opens the screenshot folder in the file manager. Returns the folder opened.
// Off the UI thread: a network folder whose server is gone can take as long
// as the network's timeout to answer.
#[tauri::command]
async fn open_screenshot_folder(folder: Option<String>) -> Result<String, String> {
    tauri::async_runtime::spawn_blocking(move || {
        let folder = screenshot_folder_in(pictures_folder(), local_app_data_folder(), folder.as_deref())?;
        prepare_screenshot_folder(&folder)?;
        show_directory(&folder.path)?;
        Ok(folder.path.to_string_lossy().into_owned())
    })
    .await
    .map_err(|error| error.to_string())?
}

// The language chosen in the installer (windows/nsis-hooks.nsh writes its
// code next to the app), which the page uses until the first pick in
// Settings. None without one.
#[tauri::command]
fn installer_language() -> Option<String> {
    let directory = env::current_exe().ok()?.parent()?.to_path_buf();
    installer_language_in(&directory)
}

fn installer_language_in(directory: &Path) -> Option<String> {
    let content = fs::read_to_string(directory.join("installer-language.txt")).ok()?;
    let code = content.trim().trim_start_matches('\u{feff}');
    let valid = !code.is_empty() && code.len() <= 16 && code.chars().all(|c| c.is_ascii_alphanumeric() || c == '-');
    valid.then(|| code.to_string())
}

// A local override for the published community config (community.json), so a
// build can preview a change, such as the Patreon promotion, before it is
// published. Lives beside settings.json; absent in normal use.
#[tauri::command]
fn load_community_override() -> Result<Option<String>, String> {
    let path = resolve_config_path().with_file_name("community.override.json");
    match fs::metadata(&path) {
        Ok(metadata) if metadata.is_file() && metadata.len() <= 64 * 1024 => {
            fs::read_to_string(&path).map(Some).map_err(|error| error.to_string())
        }
        Ok(_) => Ok(None),
        Err(error) if error.kind() == std::io::ErrorKind::NotFound => Ok(None),
        Err(error) => Err(error.to_string()),
    }
}

#[tauri::command]
fn open_external_url(url: String) -> Result<(), String> {
    let trimmed_url = url.trim();
    if trimmed_url.is_empty() || trimmed_url.chars().any(char::is_whitespace) {
        return Err("External link is not a valid URL".into());
    }

    if !trimmed_url.starts_with("https://") && !trimmed_url.starts_with("http://") {
        return Err("External link must use http or https".into());
    }

    #[cfg(target_os = "windows")]
    {
        Command::new("rundll32.exe")
            .arg("url.dll,FileProtocolHandler")
            .arg(trimmed_url)
            .spawn()
            .map_err(|error| error.to_string())?;
        return Ok(());
    }

    #[cfg(target_os = "macos")]
    {
        Command::new("open")
            .arg(trimmed_url)
            .spawn()
            .map_err(|error| error.to_string())?;
        return Ok(());
    }

    #[cfg(all(unix, not(target_os = "macos")))]
    {
        Command::new("xdg-open")
            .arg(trimmed_url)
            .spawn()
            .map_err(|error| error.to_string())?;
        return Ok(());
    }
}

#[tauri::command]
fn load_seen_apps() -> Result<SeenAppsEnvelope, String> {
    let path = resolve_seen_apps_path();
    if !path.exists() {
        return Ok(SeenAppsEnvelope {
            path: path.to_string_lossy().into_owned(),
            observations: Vec::new(),
        });
    }

    let content = fs::read_to_string(&path).map_err(|error| error.to_string())?;
    let document = serde_json::from_str::<SeenAppsDocument>(&content)
        .map(normalize_seen_apps_document)
        .unwrap_or_else(|_| SeenAppsDocument {
            version: 1,
            observations: Vec::new(),
        });

    Ok(SeenAppsEnvelope {
        path: path.to_string_lossy().into_owned(),
        observations: document.observations,
    })
}

#[tauri::command]
fn clear_seen_apps() -> Result<String, String> {
    let path = resolve_seen_apps_path();
    let content = serde_json::to_string_pretty(&SeenAppsDocument {
        version: 1,
        observations: Vec::new(),
    })
    .map_err(|error| error.to_string())?;
    write_text_safely(&path, &content)?;
    Ok(path.to_string_lossy().into_owned())
}

#[tauri::command]
fn load_runtime_pacing() -> Result<RuntimePacingEnvelope, String> {
    let path = resolve_runtime_pacing_path();
    let document = read_runtime_pacing_document(&path);
    Ok(RuntimePacingEnvelope {
        path: path.to_string_lossy().into_owned(),
        observations: document.observations,
        active_runtime: openxr_layers::read_active_runtime(),
    })
}

// "Re-discover": drop one runtime's recorded verdict so Auto probes it again
// at the next session. Leaves other runtimes' verdicts intact.
#[tauri::command]
fn clear_runtime_pacing_observation(runtime_name: String) -> Result<RuntimePacingEnvelope, String> {
    let _guard = TURBO_SAFETY_IO.lock().map_err(|error| error.to_string())?;
    let path = resolve_runtime_pacing_path();
    request_runtime_retest(&path, &runtime_name)?;
    let mut document = read_runtime_pacing_document(&path);
    document
        .observations
        .retain(|observation| observation.runtime_name != runtime_name);
    let content = serde_json::to_string_pretty(&document).map_err(|error| error.to_string())?;
    write_text_safely(&path, &content)?;
    for record in read_turbo_recovery() {
        if record["runtime"].as_str() == Some(runtime_name.as_str()) {
            if let Some(fingerprint) = record["fingerprint"].as_str() {
                clear_turbo_safety_at(
                    &resolve_runtime_relay_root().join("turbo-recovery"),
                    fingerprint,
                )?;
            }
        }
    }
    Ok(RuntimePacingEnvelope {
        path: path.to_string_lossy().into_owned(),
        observations: document.observations,
        active_runtime: openxr_layers::read_active_runtime(),
    })
}

#[tauri::command]
fn load_turbo_metrics() -> Result<TurboMetricsEnvelope, String> {
    let path = resolve_turbo_metrics_path();
    let document = read_turbo_metrics_document(&path);
    Ok(TurboMetricsEnvelope {
        path: path.to_string_lossy().into_owned(),
        sessions: document.sessions,
    })
}

// Drops all recorded metric sessions so the next capture starts from a clean
// slate (useful when settings changed and old comparisons no longer apply).
#[tauri::command]
fn clear_turbo_metrics() -> Result<TurboMetricsEnvelope, String> {
    let path = resolve_turbo_metrics_path();
    let document = TurboMetricsDocument {
        version: 1,
        sessions: Vec::new(),
    };
    let content = serde_json::to_string_pretty(&document).map_err(|error| error.to_string())?;
    write_text_safely(&path, &content)?;
    Ok(TurboMetricsEnvelope {
        path: path.to_string_lossy().into_owned(),
        sessions: document.sessions,
    })
}

fn recovery_process_alive(pid: u32, created: &str) -> bool {
    #[cfg(windows)]
    unsafe {
        use windows::Win32::Foundation::{CloseHandle, FILETIME, WAIT_OBJECT_0};
        use windows::Win32::System::Threading::{
            GetProcessTimes, OpenProcess, WaitForSingleObject, PROCESS_QUERY_LIMITED_INFORMATION,
            PROCESS_SYNCHRONIZE,
        };
        let process = match OpenProcess(
            PROCESS_QUERY_LIMITED_INFORMATION | PROCESS_SYNCHRONIZE,
            false,
            pid,
        ) {
            Ok(process) => process,
            Err(error) => return error.code().0 as u32 != 0x80070057,
        };
        let mut start = FILETIME::default();
        let mut exited = FILETIME::default();
        let mut kernel = FILETIME::default();
        let mut user = FILETIME::default();
        let known =
            GetProcessTimes(process, &mut start, &mut exited, &mut kernel, &mut user).is_ok();
        let wait = WaitForSingleObject(process, 0);
        let _ = CloseHandle(process);
        let actual = ((start.dwHighDateTime as u64) << 32) | start.dwLowDateTime as u64;
        let expected = created.parse::<u64>().unwrap_or(0);
        return wait != WAIT_OBJECT_0 && (!known || expected == 0 || actual == expected);
    }
    #[cfg(not(windows))]
    {
        let _ = (pid, created);
        true
    }
}

fn archive_turbo_fault(path: &std::path::Path, value: &serde_json::Value) -> Result<(), String> {
    let root = path
        .parent()
        .ok_or("Missing recovery directory")?
        .join("faults");
    fs::create_dir_all(&root).map_err(|error| error.to_string())?;
    let stem = path
        .file_stem()
        .and_then(|s| s.to_str())
        .ok_or("Invalid record name")?;
    let timestamp = value["updatedAtUnixMilliseconds"]
        .as_u64()
        .ok_or("Missing fault time")?;
    let destination = root.join(format!("{stem}-{timestamp}.json"));
    if root
        .parent()
        .unwrap()
        .join("faults-cleared")
        .join(format!("{stem}-{timestamp}.txt"))
        .exists()
    {
        return Ok(());
    }
    if !destination.exists() {
        // Publish a complete document so polling readers never see partial JSON.
        let temporary = destination.with_extension("tmp");
        fs::write(
            &temporary,
            serde_json::to_vec(value).map_err(|error| error.to_string())?,
        )
        .map_err(|error| error.to_string())?;
        fs::rename(&temporary, &destination).map_err(|error| error.to_string())?;
    }
    Ok(())
}

fn is_turbo_interruption(value: &serde_json::Value) -> bool {
    if value["version"].as_u64() != Some(1) {
        return false;
    }
    match value["state"].as_str() {
        Some("failed") => true,
        Some("armed") => {
            let Some(pid) = value["processId"]
                .as_u64()
                .and_then(|n| u32::try_from(n).ok())
            else {
                return false;
            };
            let Some(created) = value["processCreated"].as_str() else {
                return false;
            };
            !recovery_process_alive(pid, created)
        }
        _ => false,
    }
}

static TURBO_SAFETY_IO: std::sync::Mutex<()> = std::sync::Mutex::new(());

#[tauri::command]
async fn load_debug_sources() -> Result<debug_sources::DebugSourceSnapshot, String> {
    tauri::async_runtime::spawn_blocking(|| {
        let _guard = TURBO_SAFETY_IO
            .lock()
            .unwrap_or_else(|error| error.into_inner());
        debug_sources::collect()
    })
    .await
    .map_err(|error| error.to_string())
}

fn clear_turbo_fault_logs_at(root: &Path) -> Result<(), String> {
    let directory = root.join("faults");
    if !directory.exists() {
        return Ok(());
    }
    fs::create_dir_all(root.join("faults-cleared")).map_err(|error| error.to_string())?;
    for entry in fs::read_dir(directory).map_err(|error| error.to_string())? {
        let entry = entry.map_err(|error| error.to_string())?;
        if !entry
            .file_type()
            .map_err(|error| error.to_string())?
            .is_file()
            || entry.path().extension().and_then(|s| s.to_str()) != Some("json")
        {
            continue;
        }
        // Mark only this archived event: a live session can crash later even
        // when its last armed timestamp predates the clear operation.
        let marker = root
            .join("faults-cleared")
            .join(entry.file_name())
            .with_extension("txt");
        write_text_safely(&marker, "")?;
        match fs::remove_file(entry.path()) {
            Ok(()) => (),
            Err(error) if error.kind() == std::io::ErrorKind::NotFound => (),
            Err(error) => return Err(error.to_string()),
        }
    }
    Ok(())
}

#[tauri::command]
async fn clear_turbo_fault_logs() -> Result<(), String> {
    tauri::async_runtime::spawn_blocking(|| {
        let _guard = TURBO_SAFETY_IO.lock().map_err(|error| error.to_string())?;
        read_turbo_recovery();
        clear_turbo_fault_logs_at(&resolve_runtime_relay_root().join("turbo-recovery"))
    })
    .await
    .map_err(|error| error.to_string())?
}

fn read_turbo_faults() -> Vec<serde_json::Value> {
    let cleared = resolve_runtime_relay_root().join("turbo-recovery/faults-cleared");
    let Ok(entries) = fs::read_dir(resolve_runtime_relay_root().join("turbo-recovery/faults"))
    else {
        return Vec::new();
    };
    let mut faults: Vec<serde_json::Value> = entries
        .filter_map(Result::ok)
        .filter_map(|entry| {
            if entry.path().extension().and_then(|s| s.to_str()) != Some("json")
                || entry.metadata().ok()?.len() > 32 * 1024
                || cleared
                    .join(entry.file_name())
                    .with_extension("txt")
                    .exists()
            {
                return None;
            }
            let value: serde_json::Value =
                serde_json::from_slice(&fs::read(entry.path()).ok()?).ok()?;
            (value["version"].as_u64() == Some(1)).then_some(value)
        })
        .collect();
    faults.sort_by_key(|value| {
        std::cmp::Reverse(value["updatedAtUnixMilliseconds"].as_u64().unwrap_or(0))
    });
    faults
}

fn request_runtime_retest(pacing_path: &std::path::Path, runtime: &str) -> Result<(), String> {
    let name: String = runtime
        .as_bytes()
        .iter()
        .map(|byte| format!("{byte:02x}"))
        .collect();
    let directory = pacing_path
        .parent()
        .ok_or("Missing pacing directory")?
        .join("runtime-pacing-resets");
    fs::create_dir_all(&directory).map_err(|error| error.to_string())?;
    write_text_safely(
        &directory.join(format!("{name}.txt")),
        &unix_milliseconds().to_string(),
    )
}

fn clear_turbo_safety_at(root: &std::path::Path, fingerprint: &str) -> Result<(), String> {
    if !root.exists() {
        return Ok(());
    }
    for entry in fs::read_dir(root).map_err(|error| error.to_string())? {
        let entry = entry.map_err(|error| error.to_string())?;
        if entry.path().extension().and_then(|s| s.to_str()) != Some("json") {
            continue;
        }
        if entry.metadata().map_err(|error| error.to_string())?.len() > 32 * 1024 {
            continue;
        }
        let Ok(value) = serde_json::from_slice::<serde_json::Value>(
            &fs::read(entry.path()).map_err(|error| error.to_string())?,
        ) else {
            continue;
        };
        if value["fingerprint"].as_str() != Some(fingerprint) || !is_turbo_interruption(&value) {
            continue;
        }
        archive_turbo_fault(&entry.path(), &value)?;
        fs::remove_file(entry.path()).map_err(|error| error.to_string())?;
    }
    Ok(())
}

#[tauri::command]
async fn clear_turbo_safety_block(fingerprint: String) -> Result<(), String> {
    tauri::async_runtime::spawn_blocking(move || {
        let _guard = TURBO_SAFETY_IO.lock().map_err(|error| error.to_string())?;
        let path = resolve_runtime_pacing_path();
        let mut document = read_runtime_pacing_document(&path);
        for record in read_turbo_recovery()
            .iter()
            .filter(|record| record["fingerprint"].as_str() == Some(fingerprint.as_str()))
        {
            if let Some(runtime) = record["runtime"].as_str() {
                request_runtime_retest(&path, runtime)?;
                document
                    .observations
                    .retain(|observation| observation.runtime_name != runtime);
            }
        }
        write_text_safely(
            &path,
            &serde_json::to_string_pretty(&document).map_err(|error| error.to_string())?,
        )?;
        clear_turbo_safety_at(
            &resolve_runtime_relay_root().join("turbo-recovery"),
            &fingerprint,
        )
    })
    .await
    .map_err(|error| error.to_string())?
}

fn read_turbo_recovery() -> Vec<serde_json::Value> {
    let Ok(entries) = fs::read_dir(resolve_runtime_relay_root().join("turbo-recovery")) else {
        return Vec::new();
    };
    let mut records: Vec<serde_json::Value> = entries
        .filter_map(Result::ok)
        .filter_map(|entry| {
            if entry.path().extension().and_then(|s| s.to_str()) != Some("json")
                || entry.metadata().ok()?.len() > 32 * 1024
            {
                return None;
            }
            let value: serde_json::Value =
                serde_json::from_str(&fs::read_to_string(entry.path()).ok()?).ok()?;
            if value.get("version")?.as_u64()? != 1 {
                return None;
            }
            let state = value.get("state")?.as_str()?;
            let pid = u32::try_from(value.get("processId")?.as_u64()?).ok()?;
            let created = value.get("processCreated")?.as_str()?;
            if state != "failed" && (state != "armed" || recovery_process_alive(pid, created)) {
                return None;
            }
            let _ = archive_turbo_fault(&entry.path(), &value);
            Some(value)
        })
        .collect();
    records.sort_by_key(|v| {
        std::cmp::Reverse(
            v.get("updatedAtUnixMilliseconds")
                .and_then(|v| v.as_u64())
                .unwrap_or(0),
        )
    });
    records
}

// The layer rewrites a live session's status file every second, so a file
// last written well before the freshness window cannot hold a live session
// and is not read at all.
const RUNTIME_STATUS_UNREAD_AGE: Duration =
    Duration::from_millis(RUNTIME_STATUS_FRESHNESS_MILLISECONDS + 10_000);
// Status and control files of sessions that ended without OpenXR teardown
// (every Star Citizen and MSFS exit; a hard shutdown can leave one
// zero-filled) are deleted once this old. Export Debug still gets recent ones.
const STALE_RUNTIME_RELAY_FILE_AGE: Duration = Duration::from_secs(3 * 24 * 60 * 60);
static RUNTIME_RELAY_PRUNED: std::sync::Once = std::sync::Once::new();

fn file_age(metadata: &fs::Metadata) -> Option<Duration> {
    metadata
        .modified()
        .ok()
        .and_then(|modified| SystemTime::now().duration_since(modified).ok())
}

fn prune_stale_runtime_relay_files(directory: &Path, max_age: Duration) {
    let Ok(entries) = fs::read_dir(directory) else {
        return;
    };
    for entry in entries.filter_map(Result::ok) {
        let Ok(metadata) = entry.metadata() else {
            continue;
        };
        if metadata.is_file() && file_age(&metadata).is_some_and(|age| age > max_age) {
            let _ = fs::remove_file(entry.path());
        }
    }
}

fn read_live_runtime_statuses() -> Vec<RuntimeStatusDocument> {
    let relay = resolve_runtime_relay_root();
    RUNTIME_RELAY_PRUNED.call_once(|| {
        for directory in ["status", "control"] {
            prune_stale_runtime_relay_files(&relay.join(directory), STALE_RUNTIME_RELAY_FILE_AGE);
        }
    });
    read_live_runtime_statuses_in(&relay.join("status"))
}

fn read_live_runtime_statuses_in(status_directory: &Path) -> Vec<RuntimeStatusDocument> {
    let Ok(entries) = fs::read_dir(status_directory) else {
        return Vec::new();
    };
    let now = unix_milliseconds();
    let mut sessions = entries
        .filter_map(Result::ok)
        .filter_map(|entry| {
            let path = entry.path();
            if path.extension().and_then(|value| value.to_str()) != Some("json") {
                return None;
            }
            if entry
                .metadata()
                .ok()
                .and_then(|metadata| file_age(&metadata))
                .is_some_and(|age| age > RUNTIME_STATUS_UNREAD_AGE)
            {
                return None;
            }
            let content = fs::read_to_string(&path).ok()?;
            let status = serde_json::from_str::<RuntimeStatusDocument>(&content).ok()?;
            let file_session_id = path.file_stem()?.to_str()?;
            let age = now.saturating_sub(status.updated_at_unix_milliseconds);
            if status.protocol_version != RUNTIME_RELAY_PROTOCOL_VERSION
                || status.session_id != file_session_id
                || age > RUNTIME_STATUS_FRESHNESS_MILLISECONDS
                || status.updated_at_unix_milliseconds > now.saturating_add(2_000)
            {
                return None;
            }
            Some(status)
        })
        .collect::<Vec<_>>();
    sessions.sort_by(|lhs, rhs| {
        rhs.updated_at_unix_milliseconds
            .cmp(&lhs.updated_at_unix_milliseconds)
    });
    sessions
}

#[tauri::command]
async fn load_runtime_status() -> Result<RuntimeStatusEnvelope, String> {
    tauri::async_runtime::spawn_blocking(|| {
        let _guard = TURBO_SAFETY_IO
            .lock()
            .unwrap_or_else(|error| error.into_inner());
        let recovery = read_turbo_recovery();
        RuntimeStatusEnvelope {
            sessions: read_live_runtime_statuses(),
            recovery,
            faults: read_turbo_faults(),
        }
    })
    .await
    .map_err(|error| error.to_string())
}

#[tauri::command]
fn set_runtime_quadviews_diagnostic_visualization(
    session_id: String,
    enabled: bool,
) -> Result<u64, String> {
    write_runtime_control(session_id, RuntimeControlAction::QuadviewsDiagnosticVisualization(enabled))
}

#[tauri::command]
fn set_runtime_mirror_visible(session_id: String, visible: bool) -> Result<u64, String> {
    write_runtime_control(session_id, RuntimeControlAction::MirrorVisible(visible))
}

fn write_runtime_control(session_id: String, desired: RuntimeControlAction) -> Result<u64, String> {
    if session_id.is_empty()
        || !session_id.chars().all(|character| {
            character.is_ascii_alphanumeric()
                || character == '-'
                || character == '_'
                || character == '.'
        })
    {
        return Err("Invalid runtime session id".into());
    }
    let session = read_live_runtime_statuses()
        .into_iter()
        .find(|status| status.session_id == session_id)
        .ok_or_else(|| user_error("sessionInactive", &[], "The OpenXR session is no longer active".into()))?;
    if matches!(desired, RuntimeControlAction::MirrorVisible(_)) && session.state.mirror_visible.is_none() {
        return Err(user_error(
            "mirrorUnavailable",
            &[],
            "The mirror window is unavailable for this OpenXR session".into(),
        ));
    }
    if matches!(desired, RuntimeControlAction::QuadviewsDiagnosticVisualization(_)) && !session.capabilities.quadviews_diagnostic_visualization {
        return Err(user_error(
            "diagnosticUnavailable",
            &[],
            "Diagnostic visualization is unavailable for this OpenXR session".into(),
        ));
    }

    let now = unix_milliseconds();
    // Keep revisions exactly representable in JavaScript while reducing
    // collisions between multiple VectorXR app processes.
    let revision_floor = now.saturating_mul(1_000) + u64::from(std::process::id() % 1_000);
    let revision = RUNTIME_CONTROL_REVISION
        .fetch_update(Ordering::SeqCst, Ordering::SeqCst, |previous| {
            Some(previous.saturating_add(1).max(revision_floor))
        })
        .map(|previous| previous.saturating_add(1).max(revision_floor))
        .unwrap_or(revision_floor);
    let control = RuntimeControlDocument {
        protocol_version: RUNTIME_RELAY_PROTOCOL_VERSION,
        target_session_id: session_id.clone(),
        revision,
        expires_at_unix_milliseconds: now.saturating_add(5_000),
        desired,
    };
    let content = serde_json::to_string_pretty(&control).map_err(|error| error.to_string())?;
    let path = resolve_runtime_relay_root()
        .join("control")
        .join(format!("{session_id}.json"));
    write_text_safely(&path, &content)?;
    Ok(revision)
}

// Rescan inputs: one request for every running game (the layer's
// InputRescanRequestPath); each takes a request once, while it is fresh.
fn write_input_rescan_request(relay_root: &Path, requested_at: u64) -> Result<(), String> {
    let request = serde_json::json!({
        "protocolVersion": RUNTIME_RELAY_PROTOCOL_VERSION,
        "requestedAtUnixMilliseconds": requested_at,
    });
    let content = serde_json::to_string_pretty(&request).map_err(|error| error.to_string())?;
    write_text_safely(&relay_root.join("input-rescan.json"), &content)
}

// Asks the running games to look again for bound input devices that were not
// connected (a hot-plugged controller Windows did not report). Returns how
// many games are running with VectorXR.
#[tauri::command]
fn request_input_rescan() -> Result<usize, String> {
    write_input_rescan_request(&resolve_runtime_relay_root(), unix_milliseconds())?;
    Ok(read_live_runtime_statuses().len())
}

#[tauri::command]
async fn load_openxr_layers(
    include_signatures: Option<bool>,
) -> Result<openxr_layers::OpenXrLayerSnapshot, String> {
    tauri::async_runtime::spawn_blocking(move || {
        openxr_layers::load_openxr_layers_with_signatures(include_signatures.unwrap_or(false))
    })
    .await
    .map_err(|error| error.to_string())?
}

#[tauri::command]
async fn ensure_openxr_layer_elevation() -> Result<(), String> {
    run_blocking(openxr_layers::ensure_openxr_layer_elevation).await
}

#[tauri::command]
async fn set_openxr_layer_enabled(
    slice: String,
    manifest_path: String,
    enabled: bool,
) -> Result<openxr_layers::OpenXrLayerSnapshot, String> {
    run_blocking(move || openxr_layers::set_openxr_layer_enabled(slice, manifest_path, enabled))
        .await
}

#[tauri::command]
async fn move_openxr_layer(
    slice: String,
    manifest_path: String,
    direction: openxr_layers::MoveDirection,
) -> Result<openxr_layers::OpenXrLayerSnapshot, String> {
    run_blocking(move || openxr_layers::move_openxr_layer(slice, manifest_path, direction)).await
}

#[tauri::command]
async fn delete_openxr_layer(
    slice: String,
    manifest_path: String,
) -> Result<openxr_layers::OpenXrLayerSnapshot, String> {
    run_blocking(move || openxr_layers::delete_openxr_layer(slice, manifest_path)).await
}

// Loads a WAV into memory and scales 16-bit PCM samples by `gain`. Returns None
// for formats we don't scale (the caller then plays the file at full volume) or
// if the file isn't a parseable RIFF/WAVE container.
#[cfg(target_os = "windows")]
fn load_and_scale_wav(path: &Path, gain: f32) -> Option<Vec<u8>> {
    let mut bytes = fs::read(path).ok()?;
    if bytes.len() < 44 || &bytes[0..4] != b"RIFF" || &bytes[8..12] != b"WAVE" {
        return None;
    }

    let mut audio_format = 0u16;
    let mut bits = 0u16;
    let mut data_range: Option<(usize, usize)> = None;
    let mut pos = 12usize;
    while pos + 8 <= bytes.len() {
        let id = [bytes[pos], bytes[pos + 1], bytes[pos + 2], bytes[pos + 3]];
        let size = u32::from_le_bytes([
            bytes[pos + 4],
            bytes[pos + 5],
            bytes[pos + 6],
            bytes[pos + 7],
        ]) as usize;
        let body = pos + 8;
        if &id == b"fmt " && body + 16 <= bytes.len() {
            audio_format = u16::from_le_bytes([bytes[body], bytes[body + 1]]);
            bits = u16::from_le_bytes([bytes[body + 14], bytes[body + 15]]);
        } else if &id == b"data" {
            data_range = Some((body, (body + size).min(bytes.len())));
        }
        pos = body + size + (size & 1);
    }

    let (start, end) = data_range?;
    if audio_format != 1 || bits != 16 {
        return None;
    }

    if (gain - 1.0).abs() > f32::EPSILON {
        let mut i = start;
        while i + 1 < end {
            let sample = i16::from_le_bytes([bytes[i], bytes[i + 1]]) as f32;
            let scaled = (sample * gain).round().clamp(-32768.0, 32767.0) as i16;
            let out = scaled.to_le_bytes();
            bytes[i] = out[0];
            bytes[i + 1] = out[1];
            i += 2;
        }
    }

    Some(bytes)
}

/// Plays a .wav so the user can preview their choice from the config UI. An empty
/// path falls back to the bundled default for the given transition; `volume`
/// (0-100) scales the preview to match the configured level.
#[tauri::command]
async fn play_test_sound(
    app: tauri::AppHandle,
    path: Option<String>,
    activate: bool,
    volume: Option<u32>,
    default_name: Option<String>,
) -> Result<(), String> {
    run_blocking(move || play_test_sound_blocking(app, path, activate, volume, default_name)).await
}

fn play_test_sound_blocking(
    app: tauri::AppHandle,
    path: Option<String>,
    activate: bool,
    volume: Option<u32>,
    default_name: Option<String>,
) -> Result<(), String> {
    use tauri::Manager;

    let resolved = match path {
        Some(value) if !value.trim().is_empty() => PathBuf::from(value),
        _ => {
            // default_name selects an action-specific bundled cue; only known
            // bundled files are honored.
            let name = match default_name.as_deref() {
                Some("origin-set.wav") => "sounds/origin-set.wav",
                Some("origin-release.wav") => "sounds/origin-release.wav",
                Some("turbo-on.wav") => "sounds/turbo-on.wav",
                Some("turbo-off.wav") => "sounds/turbo-off.wav",
                Some("metrics-on.wav") => "sounds/metrics-on.wav",
                Some("metrics-off.wav") => "sounds/metrics-off.wav",
                Some("depth-lock-on.wav") => "sounds/depth-lock-on.wav",
                Some("depth-lock-off.wav") => "sounds/depth-lock-off.wav",
                _ => {
                    if activate {
                        "sounds/activate.wav"
                    } else {
                        "sounds/deactivate.wav"
                    }
                }
            };
            app.path()
                .resolve(name, tauri::path::BaseDirectory::Resource)
                .map_err(|error| error.to_string())?
        }
    };

    if !resolved.exists() {
        return Err(user_error(
            "soundMissing",
            &[("path", resolved.to_string_lossy().into_owned())],
            format!("Sound file not found: {}", resolved.to_string_lossy()),
        ));
    }

    #[cfg(target_os = "windows")]
    {
        use std::os::windows::ffi::OsStrExt;
        use windows::core::PCWSTR;
        use windows::Win32::Media::Audio::{PlaySoundW, SND_FILENAME, SND_MEMORY, SND_NODEFAULT};

        let gain = (volume.unwrap_or(100).min(100) as f32) / 100.0;

        // Scaled in-memory playback is synchronous (no SND_ASYNC) so the buffer
        // stays valid for the lifetime of the call; it blocks only this worker
        // thread, not the window.
        if let Some(image) = load_and_scale_wav(&resolved, gain) {
            let _ = unsafe {
                PlaySoundW(
                    PCWSTR(image.as_ptr() as *const u16),
                    None,
                    SND_MEMORY | SND_NODEFAULT,
                )
            };
            return Ok(());
        }

        let wide: Vec<u16> = resolved
            .as_os_str()
            .encode_wide()
            .chain(std::iter::once(0))
            .collect();
        let _ = unsafe { PlaySoundW(PCWSTR(wide.as_ptr()), None, SND_FILENAME | SND_NODEFAULT) };
        Ok(())
    }

    #[cfg(not(target_os = "windows"))]
    {
        let _ = volume;
        Err("Sound preview is only supported on Windows".into())
    }
}

#[cfg(test)]
mod tests {
    // Settings another edition (or a later version) wrote are kept when this
    // app saves; a hand-written 1280.0 mirror width does not refuse the file.
    #[test]
    fn unknown_settings_round_trip_and_mirror_numbers_are_lenient() {
        let content = r#"{
            "version": 3,
            "laterTopLevel": {"kept": true},
            "modules": {
                "crop": {"enabled": true, "defaults": {"top": 10}},
                "framegen": {"defaults": {"predictionDampening": 50}},
                "mirror": {"defaults": {"width": 1280.0, "frameRate": -5}}
            }
        }"#;
        let config = super::normalize_config(super::parse_config(content).unwrap());
        let saved: serde_json::Value = serde_json::to_value(&config).unwrap();
        assert_eq!(saved["laterTopLevel"]["kept"], true);
        assert_eq!(saved["modules"]["crop"]["defaults"]["top"], 10);
        assert_eq!(saved["modules"]["framegen"]["defaults"]["predictionDampening"], 50);
        assert_eq!(saved["modules"]["mirror"]["defaults"]["width"], 1280);
        assert!(saved["modules"]["mirror"]["defaults"]["frameRate"].as_u64().unwrap() >= 1);
    }

    // The request the layer reads (ReadInputRescanRequest): its protocol
    // version and the press's time, at the relay root.
    #[test]
    fn input_rescan_request_is_what_the_layer_reads() {
        let root = std::env::temp_dir().join(format!(
            "vectorxr-rescan-{}-{}",
            std::process::id(),
            super::unix_milliseconds()
        ));
        std::fs::create_dir_all(&root).unwrap();
        super::write_input_rescan_request(&root, 1_700_000_000_123).unwrap();
        let written: serde_json::Value =
            serde_json::from_str(&std::fs::read_to_string(root.join("input-rescan.json")).unwrap()).unwrap();
        assert_eq!(written["protocolVersion"], 1);
        assert_eq!(written["requestedAtUnixMilliseconds"], 1_700_000_000_123u64);
        super::write_input_rescan_request(&root, 1_700_000_005_000).unwrap();
        let again: serde_json::Value =
            serde_json::from_str(&std::fs::read_to_string(root.join("input-rescan.json")).unwrap()).unwrap();
        assert_eq!(again["requestedAtUnixMilliseconds"], 1_700_000_005_000u64, "a later press replaces the request");
        let _ = std::fs::remove_dir_all(&root);
    }

    #[test]
    fn osd_defaults_and_round_trip() {
        let legacy: super::CoreConfig = serde_json::from_str(r#"{"enabled":true,"logLevel":"info"}"#).unwrap();
        assert!(!legacy.osd.enabled);
        assert_eq!(legacy.osd.update_hz, 5);
        assert!(legacy.osd.show_framegen && !legacy.osd.compact_show_framegen);
        assert!(legacy.osd.show_framegen_fps);
        assert!(legacy.osd.show_performance && !legacy.osd.compact_show_performance);
        assert_eq!(legacy.osd.body_order, ["graph", "turbo", "framegen", "pivot", "modules"]);
        assert_eq!(
            legacy.osd.row_order.as_deref(),
            Some(&["graph", "performance", "turbo", "framegen", "pivot", "modules"].map(String::from)[..])
        );
        let mut input = serde_json::json!({"enabled":true,"compact":true,"visibleOnStart":false,
            "horizontalDegrees":-30.0,"verticalDegrees":15.0,"distanceMeters":2.0,"scale":75.0,
            "opacity":70,"updateHz":10,"showGraph":false,"showRuntime":false,"showTurbo":false,
            "showModules":false,"showClock":false,"accent":"blue","toggleBinding":{"type":"none"},
            "showPivot":false,"clockFormat":"12","customColor":"#aB12Ef","bodyOrder":["pivot","framegen","turbo","modules","graph","performance"],
            "showFramegen":false,"compactShowFramegen":true,"showFramegenFps":false,
            "compactShowRuntime":true,"compactShowClock":true,"compactShowTurbo":true,"compactShowPivot":true,
            "compactShowBrand":false,"compactShowApp":false,"compactMetrics":"fps",
            "showBrand":false,"showApp":false,
            "customPresets":[{"name":"Small cockpit","settings":{"scale":25,"accent":"custom","customColor":"#aB12Ef"}}],
            "cycleBinding":{"type":"keyboard","chord":["Ctrl","F11"]}});
        // Past json!'s recursion limit in the literal.
        input["showPerformance"] = serde_json::json!(false);
        input["compactShowPerformance"] = serde_json::json!(true);
        let osd: super::OsdSettings = serde_json::from_value(input.clone()).unwrap();
        let output = serde_json::to_value(osd).unwrap();
        for (key,value) in input.as_object().unwrap() {
            if key != "cycleBinding" { assert_eq!(&output[key], value, "{key}"); }
        }
        assert_eq!(output["cycleBinding"]["chord"], input["cycleBinding"]["chord"]);
    }

    // 0.19.0-0.19.2 layers fail the whole config on a bodyOrder row they do
    // not know: every file this app writes keeps bodyOrder (presets' too) to
    // their rows, and the full order goes in rowOrder, which they ignore.
    #[test]
    fn saved_osd_row_orders_stay_readable_by_older_layers() {
        use serde_json::json;
        let legacy_rows = ["graph", "turbo", "framegen", "pivot", "modules"];
        let readable = |order: &serde_json::Value| {
            order.as_array().unwrap().iter().all(|row| legacy_rows.contains(&row.as_str().unwrap()))
        };
        let default = serde_json::to_value(super::default_config()).unwrap();
        assert!(readable(&default["core"]["osd"]["bodyOrder"]));
        assert_eq!(default["core"]["osd"]["rowOrder"][1], "performance");

        let mut config = default.clone();
        let osd = config["core"]["osd"].as_object_mut().unwrap();
        osd.remove("rowOrder");
        osd.insert("bodyOrder".into(), json!(["pivot", "performance", "graph", "turbo", "framegen", "modules"]));
        osd.insert("customPresets".into(), json!([
            { "name": "Cockpit", "settings": { "bodyOrder": ["performance", "modules", "graph", "turbo", "framegen", "pivot"] } },
            { "name": "Old", "settings": { "bodyOrder": ["modules", "graph"] } }
        ]));
        let output = saved(config);
        let osd = &output["core"]["osd"];
        assert_eq!(osd["bodyOrder"], json!(["pivot", "graph", "turbo", "framegen", "modules"]));
        assert_eq!(osd["rowOrder"], json!(["pivot", "performance", "graph", "turbo", "framegen", "modules"]));
        let preset = &osd["customPresets"][0]["settings"];
        assert_eq!(preset["bodyOrder"], json!(["modules", "graph", "turbo", "framegen", "pivot"]));
        assert_eq!(preset["rowOrder"][0], "performance");
        assert!(osd["customPresets"][1]["settings"].get("rowOrder").is_none());
        // Read back, rowOrder wins, and saving again changes nothing.
        assert_eq!(saved(output.clone()), output);
    }

    #[test]
    fn runtime_retest_invalidates_legacy_and_late_session_results() {
        let root = std::env::temp_dir().join(format!(
            "vectorxr-retest-{}-{}",
            std::process::id(),
            super::unix_milliseconds()
        ));
        std::fs::create_dir_all(&root).unwrap();
        let path = root.join("runtime-pacing.json");
        let tested = serde_json::json!({"runtimeName":"SteamVR/OpenXR","mode":"async","source":"discovered","testStartedAt":1,"stableSeconds":60});
        let preset = serde_json::json!({"runtimeName":"Pimax OpenXR","mode":"sequenced","source":"preset","testStartedAt":1,"stableSeconds":60});
        std::fs::write(
            &path,
            serde_json::json!({"version":2,"observations":[tested.clone(),preset]}).to_string(),
        )
        .unwrap();
        assert_eq!(
            super::read_runtime_pacing_document(&path)
                .observations
                .len(),
            1
        );
        super::request_runtime_retest(&path, "SteamVR/OpenXR").unwrap();
        // Even a stale writer replacing the JSON after reset cannot revive it.
        std::fs::write(
            &path,
            serde_json::json!({"version":2,"observations":[tested.clone()]}).to_string(),
        )
        .unwrap();
        assert!(super::read_runtime_pacing_document(&path)
            .observations
            .is_empty());
        let mut fresh = tested;
        fresh["testStartedAt"] = (super::unix_milliseconds() + 1).into();
        std::fs::write(
            &path,
            serde_json::json!({"version":2,"observations":[fresh]}).to_string(),
        )
        .unwrap();
        assert_eq!(
            super::read_runtime_pacing_document(&path)
                .observations
                .len(),
            1
        );
        std::fs::remove_dir_all(root).unwrap();
    }

    #[test]
    fn safety_clear_preserves_history_and_other_setups() {
        let root = std::env::temp_dir().join(format!(
            "vectorxr-safety-clear-{}-{}",
            std::process::id(),
            super::unix_milliseconds()
        ));
        std::fs::create_dir_all(&root).unwrap();
        let failed = serde_json::json!({"version":1,"state":"failed","fingerprint":"dcs","application":"DCS.exe","processId":0,"processCreated":"0","updatedAtUnixMilliseconds":42});
        let mut other = failed.clone();
        other["fingerprint"] = "other".into();
        std::fs::write(root.join("dcs.json"), failed.to_string()).unwrap();
        std::fs::write(root.join("other.json"), other.to_string()).unwrap();
        let live = serde_json::json!({"version":1,"state":"armed","fingerprint":"dcs","processId":std::process::id(),"processCreated":"0","updatedAtUnixMilliseconds":43});
        std::fs::write(root.join("live.json"), live.to_string()).unwrap();
        super::clear_turbo_safety_at(&root, "dcs").unwrap();
        assert!(!root.join("dcs.json").exists());
        assert!(root.join("other.json").exists());
        assert!(root.join("live.json").exists());
        assert_eq!(
            serde_json::from_slice::<serde_json::Value>(
                &std::fs::read(root.join("faults/dcs-42.json")).unwrap()
            )
            .unwrap(),
            failed
        );
        super::clear_turbo_safety_at(&root, "dcs").unwrap();
        assert_eq!(std::fs::read_dir(root.join("faults")).unwrap().count(), 1);
        std::fs::remove_dir_all(root).unwrap();
    }

    #[test]
    fn fault_log_clear_preserves_blocks_and_does_not_hide_later_crashes() {
        let root = std::env::temp_dir().join(format!(
            "vectorxr-fault-clear-{}-{}",
            std::process::id(),
            super::unix_milliseconds()
        ));
        std::fs::create_dir_all(&root).unwrap();
        let failed = serde_json::json!({"version":1,"state":"failed","fingerprint":"dcs","updatedAtUnixMilliseconds":42});
        let live = serde_json::json!({"version":1,"state":"armed","fingerprint":"live","updatedAtUnixMilliseconds":40});
        std::fs::write(root.join("dcs.json"), failed.to_string()).unwrap();
        std::fs::write(root.join("live.json"), live.to_string()).unwrap();
        super::archive_turbo_fault(&root.join("dcs.json"), &failed).unwrap();
        super::clear_turbo_fault_logs_at(&root).unwrap();
        assert_eq!(
            std::fs::read_to_string(root.join("dcs.json")).unwrap(),
            failed.to_string()
        );
        assert_eq!(
            std::fs::read_to_string(root.join("live.json")).unwrap(),
            live.to_string()
        );
        assert_eq!(std::fs::read_dir(root.join("faults")).unwrap().count(), 0);
        // Polling and Clear & retest must not restore a cleared fault.
        super::archive_turbo_fault(&root.join("dcs.json"), &failed).unwrap();
        assert_eq!(std::fs::read_dir(root.join("faults")).unwrap().count(), 0);
        super::clear_turbo_safety_at(&root, "dcs").unwrap();
        assert_eq!(std::fs::read_dir(root.join("faults")).unwrap().count(), 0);
        // A later process death still records its older armed marker.
        super::archive_turbo_fault(&root.join("live.json"), &live).unwrap();
        assert!(root.join("faults/live-40.json").exists());
        let mut fresh = failed;
        fresh["updatedAtUnixMilliseconds"] = 43.into();
        super::archive_turbo_fault(&root.join("dcs.json"), &fresh).unwrap();
        assert!(root.join("faults/dcs-43.json").exists());
        std::fs::remove_dir_all(root).unwrap();
    }

    #[test]
    fn turbo_profile_safety_override_round_trips() {
        let old: super::TurboProfileConfig = serde_json::from_value(serde_json::json!({})).unwrap();
        assert!(!old.disable_safety);
        let profile: super::TurboProfileConfig =
            serde_json::from_value(serde_json::json!({"disableSafety":true})).unwrap();
        assert_eq!(
            serde_json::to_value(profile).unwrap()["disableSafety"],
            true
        );
    }

    #[test]
    fn turbo_experiments_survive_backend_round_trip() {
        let defaults: super::TurboModuleConfig = serde_json::from_str("{}").unwrap();
        assert!(!defaults.experimental.enabled);
        assert!(defaults.experimental.application_ids.is_empty());
        assert_eq!(defaults.experimental.prediction_percent, 100);
        let input = serde_json::json!({"experimental": {
            "enabled": true, "applicationIds": ["dcs"], "waitForSubmit": true,
            "predictionPercent": 75, "frameLimit": 45
        }});
        let config: super::TurboModuleConfig = serde_json::from_value(input.clone()).unwrap();
        let output = serde_json::to_value(config).unwrap();
        assert_eq!(output["experimental"], input["experimental"]);
        let profiles = serde_json::json!({"profiles": [
            {"id":"dcs","name":"DCS","enabled":true,"applicationIds":["dcs"],"experimental": {
                "enabled":true,"waitForSubmit":true,"sampleAtEntry":true,"predictionPercent":90,"frameLimit":45
            }}
        ]});
        let migrated: super::TurboModuleConfig = serde_json::from_value(profiles).unwrap();
        let saved=serde_json::to_value(migrated).unwrap();
        assert_eq!(saved["profiles"][0]["experimental"]["predictionPercent"],90);
        assert_eq!(saved["profiles"][0]["experimental"]["enabled"],true);

        let session: super::TurboMetricsSession = serde_json::from_value(serde_json::json!({
            "timingConfiguration": "Experimental Async; cap=45"
        })).unwrap();
        assert_eq!(serde_json::to_value(session).unwrap()["timingConfiguration"], "Experimental Async; cap=45");
    }

    #[test]
    fn framegen_defaults_and_round_trip() {
        let defaults: super::FrameGenModuleConfig = serde_json::from_str("{}").unwrap();
        assert!(!defaults.enabled);
        assert!(defaults.toggle_binding.is_none() && defaults.toggle_bindings.is_none());
        assert_eq!(defaults.diagnostic_view, "off");
        assert_eq!(defaults.defaults.synthesis_mode, "interpolate");
        assert_eq!(defaults.defaults.flow_quality, "balanced");
        assert_eq!(defaults.defaults.motion_estimation, "shader");
        assert_eq!(defaults.defaults.frame_multiplier, "dynamic");
        assert_eq!(defaults.defaults.artifact_suppression, 10);
        assert_eq!(defaults.defaults.motion_limit_percent, 10);
        assert!(!defaults.defaults.extend_head_prediction);
        assert!(!defaults.defaults.edge_fill);
        assert!(defaults.defaults.use_game_depth);
        assert!(defaults.profiles.is_empty());

        // Older layers reject unknown keys (current ones ignore them with a
        // warning), so the saved shape must match exactly.
        let input = serde_json::json!({
            "enabled": true,
            "toggleBinding": { "type": "keyboard", "chord": ["Ctrl", "F7"] },
            "diagnosticView": "tint",
            "defaults": {
                "synthesisMode": "extrapolate", "flowQuality": "quality", "motionEstimation": "hardware", "interpolateFlow": "auto",
                "frameMultiplier": "4x", "artifactSuppression": 80, "motionLimitPercent": 4
            },
            "profiles": [{
                "name": "DCS", "enabled": false, "applicationIds": ["dcs"],
                "settings": {
                    "synthesisMode": "interpolate", "flowQuality": "performance", "motionEstimation": "shader", "interpolateFlow": "shader",
                    "frameMultiplier": "3x", "artifactSuppression": 0, "motionLimitPercent": 25,
                    "extendHeadPrediction": true,
                    "edgeFill": true,
                    "useGameDepth": false
                }
            }]
        });
        let config: super::FrameGenModuleConfig = serde_json::from_value(input.clone()).unwrap();
        assert!(config.profiles[0].settings.extend_head_prediction);
        assert!(config.profiles[0].settings.edge_fill);
        assert!(!config.profiles[0].settings.use_game_depth);
        assert!(config.defaults.use_game_depth);
        assert_eq!(serde_json::to_value(config).unwrap(), input);

        // Settings saved without a frame multiplier load as Dynamic and save it.
        let older: super::FrameGenSettings = serde_json::from_value(serde_json::json!({
            "synthesisMode": "extrapolate", "flowQuality": "quality",
            "artifactSuppression": 80, "motionLimitPercent": 4
        }))
        .unwrap();
        assert_eq!(older.frame_multiplier, "dynamic");
        assert_eq!(older.motion_estimation, "shader");
        assert_eq!(older.interpolate_flow, "auto");
        assert_eq!(serde_json::to_value(older).unwrap()["frameMultiplier"], "dynamic");
        assert_eq!(
            serde_json::to_value(super::FrameGenSettings::default()).unwrap()["frameMultiplier"],
            "dynamic"
        );

        // Configs written before Frame Generation load with it off and save the defaults.
        let old: super::VectorXRConfig = serde_json::from_value(serde_json::json!({
            "version": 3,
            "core": { "enabled": true, "logLevel": "info", "logRetentionFiles": 7, "trackSeenApps": true },
            "applications": [{ "id": "dcs", "name": "DCS", "enabled": true, "match": { "exe": "DCS.exe" } }],
            "modules": { "turbo": { "enabled": true } }
        }))
        .unwrap();
        assert!(!old.modules.framegen.enabled);
        assert!(old.modules.turbo.enabled);
        let saved = serde_json::to_value(super::normalize_config(old)).unwrap();
        assert_eq!(
            saved["modules"]["framegen"],
            serde_json::to_value(super::FrameGenModuleConfig::default()).unwrap()
        );
        assert_eq!(saved["modules"]["framegen"]["toggleBinding"]["type"], "none");
        assert_eq!(saved["core"]["osd"]["showFramegen"], true);
        assert_eq!(saved["core"]["osd"]["compactShowFramegen"], false);
        assert_eq!(saved["core"]["osd"]["showFramegenFps"], true);

        let unnamed: super::VectorXRConfig = serde_json::from_value(serde_json::json!({
            "applications": [{ "id": "dcs", "name": "DCS", "enabled": true, "match": { "exe": "DCS.exe" } }],
            "modules": { "framegen": { "profiles": [{ "name": " ", "applicationIds": ["dcs"] }] } }
        }))
        .unwrap();
        let profile = &super::normalize_config(unnamed).modules.framegen.profiles[0];
        assert_eq!(profile.name, "DCS");
        assert!(profile.enabled);
        assert_eq!(profile.settings.flow_quality, "balanced");
        assert_eq!(profile.settings.frame_multiplier, "dynamic");
    }

    #[test]
    fn screenshot_defaults_and_round_trip() {
        let defaults: super::ScreenshotSettings = serde_json::from_str("{}").unwrap();
        assert!(defaults.bindings.is_empty());
        assert_eq!(defaults.eyes, "both");
        assert!(defaults.folder.is_empty());

        // The layer parses exactly this shape; it must survive unchanged.
        let input = serde_json::json!({
            "bindings": [
                { "type": "keyboard", "chord": ["Ctrl", "Alt", "F9"], "sound": { "enabled": true, "activateSound": "", "deactivateSound": "" } },
                { "type": "device", "deviceGuid": "guid-1", "inputPath": "button-7" }
            ],
            "eyes": "left",
            "folder": "D:/Shots"
        });
        let settings: super::ScreenshotSettings = serde_json::from_value(input.clone()).unwrap();
        assert_eq!(settings.bindings.len(), 2);
        assert_eq!(serde_json::to_value(settings).unwrap(), input);

        // Configs written before screenshots load with no binding, and the
        // default folder is not written.
        let old: super::VectorXRConfig = serde_json::from_value(serde_json::json!({
            "version": 3,
            "core": { "enabled": true }
        }))
        .unwrap();
        let saved = serde_json::to_value(super::normalize_config(old)).unwrap();
        assert_eq!(
            saved["core"]["screenshot"],
            serde_json::json!({ "bindings": [], "eyes": "both" })
        );
        assert_eq!(
            serde_json::to_value(super::default_config()).unwrap()["core"]["screenshot"],
            saved["core"]["screenshot"]
        );

        // The whole config keeps the block; an unknown eye choice becomes
        // both eyes and the folder loses stray spaces.
        let config: super::VectorXRConfig = serde_json::from_value(serde_json::json!({
            "version": 3,
            "core": { "screenshot": {
                "bindings": [{ "type": "keyboard", "chord": ["F9"] }, { "type": "none" }],
                "eyes": "center",
                "folder": "  D:/Shots  "
            } }
        }))
        .unwrap();
        let saved = serde_json::to_value(super::normalize_config(config)).unwrap();
        assert_eq!(
            saved["core"]["screenshot"],
            serde_json::json!({
                "bindings": [{ "type": "keyboard", "chord": ["F9"] }],
                "eyes": "both",
                "folder": "D:/Shots"
            })
        );
    }

    #[test]
    fn screenshot_folder_is_the_custom_one_or_vectorxr_under_pictures() {
        let pictures = || Some(std::env::temp_dir().join("Pictures"));
        let local = || Some(std::env::temp_dir().join("Local"));
        let folder = |pictures, local, custom| super::screenshot_folder_in(pictures, local, custom).unwrap();
        let default = super::ScreenshotFolder {
            path: std::env::temp_dir().join("Pictures").join("VectorXR"),
            is_default: true,
        };
        assert_eq!(folder(pictures(), local(), None), default);
        assert_eq!(folder(pictures(), local(), Some("  ")), default);
        // A relative folder is never opened: the layer would not write there.
        assert_eq!(folder(pictures(), local(), Some("Shots")), default);
        assert_eq!(folder(pictures(), local(), Some("..\\Shots")), default);
        assert_eq!(folder(pictures(), local(), Some("D:\\Shots\u{0}\\x")), default);

        let custom = super::ScreenshotFolder { path: std::env::temp_dir().join("Shots"), is_default: false };
        let text = format!(" {} ", custom.path.to_string_lossy());
        assert_eq!(folder(pictures(), local(), Some(&text)), custom);
        assert_eq!(folder(None, None, Some(&text)), custom);

        // Without a Pictures folder the layer writes under %LOCALAPPDATA%.
        assert_eq!(
            folder(None, local(), None),
            super::ScreenshotFolder {
                path: std::env::temp_dir().join("Local").join("VectorXR").join("screenshots"),
                is_default: true,
            }
        );
        assert!(super::screenshot_folder_in(None, None, None).is_err());
    }

    #[cfg(target_os = "windows")]
    #[test]
    fn a_custom_screenshot_folder_is_made_a_plain_windows_path() {
        let path = |text| super::custom_screenshot_folder(text);
        assert_eq!(path("D:/Shots/VR"), Some(std::path::PathBuf::from("D:\\Shots\\VR")));
        assert_eq!(path("D:\\Shots\\.\\a\\..\\VR\\"), Some(std::path::PathBuf::from("D:\\Shots\\VR\\")));
        assert_eq!(path("D:/Shots,/select"), Some(std::path::PathBuf::from("D:\\Shots,\\select")));
        assert_eq!(path("//server/share/Shots"), Some(std::path::PathBuf::from("\\\\server\\share\\Shots")));
        // Not a full path: a drive-relative one, a rooted one without a drive,
        // an explorer option.
        for text in ["D:Shots", "\\Shots", "/select,D:\\Shots", "Shots", ""] {
            assert_eq!(path(text), None, "{text}");
        }
    }

    #[test]
    fn only_the_default_screenshot_folder_is_created_to_be_opened() {
        let root = std::env::temp_dir().join(format!("vectorxr-screenshot-folder-{}", std::process::id()));
        let _ = std::fs::remove_dir_all(&root);

        let default = super::ScreenshotFolder { path: root.join("Pictures").join("VectorXR"), is_default: true };
        super::prepare_screenshot_folder(&default).unwrap();
        assert!(default.path.is_dir());

        // A folder the user named and has not saved is left alone.
        let custom = super::ScreenshotFolder { path: root.join("Unsaved").join("Shots"), is_default: false };
        let error = super::prepare_screenshot_folder(&custom).unwrap_err();
        assert!(crate::user_error::plain(&error).starts_with("Directory does not exist: "), "{error}");
        assert!(!root.join("Unsaved").exists());

        std::fs::create_dir_all(&custom.path).unwrap();
        super::prepare_screenshot_folder(&custom).unwrap();
        // A file is not a folder to open.
        let file = super::ScreenshotFolder { path: root.join("file.txt"), is_default: false };
        std::fs::write(&file.path, b"x").unwrap();
        assert!(super::prepare_screenshot_folder(&file).is_err());

        let _ = std::fs::remove_dir_all(&root);
    }

    #[test]
    fn framegen_numbers_are_clamped_on_save() {
        let config: super::VectorXRConfig = serde_json::from_value(serde_json::json!({
            "applications": [{ "id": "dcs", "name": "DCS", "enabled": true, "match": { "exe": "DCS.exe" } }],
            "modules": {
                "framegen": {
                    "defaults": { "artifactSuppression": 150, "motionLimitPercent": 50 },
                    "profiles": [{ "name": "DCS", "applicationIds": ["dcs"], "settings": { "artifactSuppression": -10, "motionLimitPercent": 0 } }]
                }
            }
        }))
        .unwrap();
        let saved = serde_json::to_value(super::normalize_config(config)).unwrap();
        let framegen = &saved["modules"]["framegen"];
        assert_eq!(framegen["defaults"]["artifactSuppression"], 100);
        assert_eq!(framegen["defaults"]["motionLimitPercent"], 25);
        assert_eq!(framegen["profiles"][0]["settings"]["artifactSuppression"], 0);
        assert_eq!(framegen["profiles"][0]["settings"]["motionLimitPercent"], 2);
    }

    #[test]
    fn mirror_defaults_and_round_trip() {
        let default_settings = serde_json::json!({
            "smoothing": 60.0, "levelHorizon": false, "eye": "left", "margin": 15.0,
            "aspect": "16:9", "width": 1280, "frameRate": 60, "fillMonitor": false, "monitor": ""
        });
        let defaults: super::MirrorModuleConfig = serde_json::from_str("{}").unwrap();
        assert!(!defaults.enabled);
        assert!(defaults.bindings.is_empty());
        assert_eq!(serde_json::to_value(&defaults.defaults).unwrap(), default_settings);
        assert!(defaults.profiles.is_empty());

        // The layer parses exactly this shape; it must survive unchanged.
        let input = serde_json::json!({
            "enabled": true,
            "bindings": [
                { "type": "keyboard", "chord": ["Ctrl", "Alt", "M"] },
                { "type": "device", "deviceGuid": "guid-1", "inputPath": "button-7" }
            ],
            "defaults": {
                "smoothing": 0.0, "levelHorizon": true, "eye": "right", "margin": 40.0,
                "aspect": "21:9", "width": 3840, "frameRate": 120,
                "fillMonitor": true, "monitor": "\\\\.\\DISPLAY2"
            },
            "profiles": [{
                "name": "DCS", "enabled": false, "applicationIds": ["dcs"],
                "settings": {
                    "smoothing": 100.0, "levelHorizon": false, "eye": "left", "margin": 0.0,
                    "aspect": "1:1", "width": 320, "frameRate": 15,
                    "fillMonitor": false, "monitor": ""
                }
            }]
        });
        let config: super::MirrorModuleConfig = serde_json::from_value(input.clone()).unwrap();
        assert_eq!(config.bindings.len(), 2);
        assert_eq!(config.defaults.eye, "right");
        assert!(config.defaults.fill_monitor);
        assert_eq!(config.defaults.monitor, r"\\.\DISPLAY2");
        assert_eq!(config.profiles[0].settings.width, 320);
        assert_eq!(serde_json::to_value(config).unwrap(), input);

        // Whole numbers from a hand-written file load too; missing values are the
        // defaults. One binding is a list of one, and None entries are dropped.
        let partial: super::MirrorSettings =
            serde_json::from_value(serde_json::json!({ "smoothing": 30, "aspect": "4:3" })).unwrap();
        assert_eq!(partial.smoothing, 30.0);
        assert_eq!(partial.aspect, "4:3");
        assert_eq!(partial.margin, 15.0);
        assert_eq!(partial.eye, "left");
        assert_eq!(partial.width, 1280);
        assert_eq!(partial.frame_rate, 60);
        assert!(!partial.fill_monitor);
        assert!(partial.monitor.is_empty());
        let single: super::MirrorModuleConfig = serde_json::from_value(serde_json::json!({
            "bindings": { "type": "keyboard", "chord": ["F7"] }
        }))
        .unwrap();
        assert_eq!(single.bindings.len(), 1);
        let none: super::MirrorModuleConfig =
            serde_json::from_value(serde_json::json!({ "bindings": [{ "type": "none" }] })).unwrap();
        assert!(none.bindings.is_empty());

        // Configs written before the mirror window load with it off and save the defaults.
        let old: super::VectorXRConfig = serde_json::from_value(serde_json::json!({
            "version": 3,
            "applications": [{ "id": "dcs", "name": "DCS", "enabled": true, "match": { "exe": "DCS.exe" } }],
            "modules": { "framegen": { "enabled": true } }
        }))
        .unwrap();
        assert!(!old.modules.mirror.enabled);
        let saved = serde_json::to_value(super::normalize_config(old)).unwrap();
        assert_eq!(
            saved["modules"]["mirror"],
            serde_json::json!({ "enabled": false, "bindings": [], "defaults": default_settings, "profiles": [] })
        );
        assert_eq!(
            serde_json::to_value(super::default_config()).unwrap()["modules"]["mirror"],
            saved["modules"]["mirror"]
        );

        let unnamed: super::VectorXRConfig = serde_json::from_value(serde_json::json!({
            "applications": [{ "id": "dcs", "name": "DCS", "enabled": true, "match": { "exe": "DCS.exe" } }],
            "modules": { "mirror": { "profiles": [{ "name": " ", "applicationIds": ["dcs"] }] } }
        }))
        .unwrap();
        let profile = &super::normalize_config(unnamed).modules.mirror.profiles[0];
        assert_eq!(profile.name, "DCS");
        assert!(profile.enabled);
        assert_eq!(profile.settings.smoothing, 60.0);
        assert_eq!(profile.settings.aspect, "16:9");
    }

    #[test]
    fn turbo_recovery_defaults_on_and_preserves_opt_out() {
        let old: super::TurboModuleConfig =
            serde_json::from_value(serde_json::json!({ "enabled": true })).unwrap();
        assert!(old.interrupted_session_recovery);
        let mut config = super::default_config();
        config.modules.turbo.interrupted_session_recovery = false;
        let saved = serde_json::to_value(&config).unwrap();
        assert_eq!(
            saved["modules"]["turbo"]["interruptedSessionRecovery"],
            false
        );
        let restored: super::VectorXRConfig = serde_json::from_value(saved).unwrap();
        assert!(!restored.modules.turbo.interrupted_session_recovery);
    }

    #[test]
    fn log_preview_bounds_reads_and_accepts_split_unicode() {
        let path =
            std::env::temp_dir().join(format!("vectorxr-unicode-tail-{}.log", std::process::id()));
        let content = "€".repeat(50_000) + "x";
        std::fs::write(&path, content).unwrap();
        let preview = super::read_log_preview(&path);
        assert!(preview.starts_with("... truncated"));
        assert!(preview.ends_with('x'));
        assert!(preview.len() < 120_100);
        std::fs::remove_file(path).unwrap();
    }

    #[test]
    fn visibility_commands_serialize_only_the_requested_feature() {
        use super::RuntimeControlAction::*;
        assert_eq!(serde_json::to_value(MirrorVisible(false)).unwrap(), serde_json::json!({"mirrorVisible": false}));
        assert_eq!(serde_json::to_value(QuadviewsDiagnosticVisualization(true)).unwrap(),
                   serde_json::json!({"quadviewsDiagnosticVisualization": true}));
    }

    #[test]
    fn runtime_status_accepts_old_and_new_protocol_fields() {
        let mut value = serde_json::json!({
            "protocolVersion": 1, "sessionId": "test", "processId": 1,
            "application": "DCS.exe", "updatedAtUnixMilliseconds": 1,
            "acknowledgedRevision": 0,
            "capabilities": { "quadviewsDiagnosticVisualization": true },
            "state": { "quadviewsDiagnosticVisualization": false }
        });
        let old: super::RuntimeStatusDocument = serde_json::from_value(value.clone()).unwrap();
        assert!(old.quadviews_dimensions.is_empty());
        assert!(!old.capabilities.osd && !old.state.osd_visible);
        value["capabilities"]["osd"] = true.into();
        value["state"]["osdVisible"] = true.into();
        value["state"]["osdCompact"] = true.into();
        value["state"]["osdMessage"] = "Visible".into();
        value["state"]["turboState"] = "recovery-disabled".into();
        value["state"]["turboReason"] = "Previous interruption".into();
        value["state"]["framegenState"] = "unavailable".into();
        value["state"]["framegenReason"] = "Runtime refresh rate unknown".into();
        value["state"]["framegenDepth"] = "empty".into();
        value["state"]["framegenFlow"] = "shader".into();
        value["state"]["framegenFlowMode"] = "auto".into();
        value["state"]["framegenFlowReason"] = "hardware late".into();
        value["state"]["mirrorState"] = "hidden".into();
        value["state"]["mirrorReason"] = "The window was closed".into();
        value["quadviewsDimensions"] = serde_json::json!([{"width": 1000, "height": 900, "allocatedWidth": 1200, "allocatedHeight": 1000}]);
        let current: super::RuntimeStatusDocument = serde_json::from_value(value).unwrap();
        assert_eq!(current.state.turbo_state, "recovery-disabled");
        assert!(current.capabilities.osd && current.state.osd_visible && current.state.osd_compact);
        assert_eq!(current.state.osd_message,"Visible");
        assert_eq!(current.state.framegen_state, "unavailable");
        assert_eq!(current.state.framegen_reason, "Runtime refresh rate unknown");
        assert!(old.state.framegen_state.is_empty() && old.state.framegen_reason.is_empty());
        assert_eq!(current.state.framegen_depth, "empty");
        assert!(old.state.framegen_depth.is_empty());
        assert_eq!(current.state.framegen_flow, "shader");
        assert_eq!(current.state.framegen_flow_mode, "auto");
        assert_eq!(current.state.framegen_flow_reason, "hardware late");
        assert!(old.state.framegen_flow.is_empty());
        assert_eq!(current.state.mirror_state, "hidden");
        assert_eq!(current.state.mirror_reason, "The window was closed");
        assert!(old.state.mirror_state.is_empty() && old.state.mirror_reason.is_empty());

        // What the app's pages get: a state an older layer did not report is
        // absent, not an empty string, and a reported one is there.
        let old_state = serde_json::to_value(&old.state).unwrap();
        let current_state = serde_json::to_value(&current.state).unwrap();
        for key in ["framegenState", "mirrorState"] {
            assert!(old_state.get(key).is_none(), "{key} from an older layer");
            assert!(current_state.get(key).is_some_and(|state| state.is_string()), "{key} from this layer");
        }
        assert_eq!(current.quadviews_dimensions[0].allocated_width, 1200);
    }

    #[test]
    fn runtime_status_polls_skip_and_prune_files_of_ended_sessions() {
        let root = std::env::temp_dir().join(format!(
            "vectorxr-relay-prune-{}-{}",
            std::process::id(),
            super::unix_milliseconds()
        ));
        std::fs::create_dir_all(&root).unwrap();
        let status = |session: &str| {
            serde_json::json!({
                "protocolVersion": super::RUNTIME_RELAY_PROTOCOL_VERSION, "sessionId": session,
                "processId": 1, "application": "StarCitizen.exe",
                "updatedAtUnixMilliseconds": super::unix_milliseconds(), "acknowledgedRevision": 0,
                "capabilities": { "quadviewsDiagnosticVisualization": false },
                "state": { "quadviewsDiagnosticVisualization": false }
            })
            .to_string()
        };
        let age = |name: &str, seconds: u64| {
            let file = std::fs::File::options().write(true).open(root.join(name)).unwrap();
            file.set_modified(std::time::SystemTime::now() - std::time::Duration::from_secs(seconds))
                .unwrap();
        };
        std::fs::write(root.join("live.json"), status("live")).unwrap();
        // A file nobody has written for a minute is not even read, whatever
        // it says inside.
        std::fs::write(root.join("ended.json"), status("ended")).unwrap();
        age("ended.json", 60);
        std::fs::write(root.join("old.json"), status("old")).unwrap();
        age("old.json", 4 * 24 * 60 * 60);
        std::fs::write(root.join("zeroed.json"), vec![0u8; 512]).unwrap();
        age("zeroed.json", 4 * 24 * 60 * 60);

        let live = super::read_live_runtime_statuses_in(&root);
        assert_eq!(live.len(), 1);
        assert_eq!(live[0].session_id, "live");

        super::prune_stale_runtime_relay_files(&root, super::STALE_RUNTIME_RELAY_FILE_AGE);
        let mut left = std::fs::read_dir(&root)
            .unwrap()
            .map(|entry| entry.unwrap().file_name().to_string_lossy().into_owned())
            .collect::<Vec<_>>();
        left.sort();
        assert_eq!(left, ["ended.json", "live.json"]);
        std::fs::remove_dir_all(root).unwrap();
    }

    #[test]
    #[cfg(windows)]
    fn recovery_does_not_mistake_a_live_process_for_a_crash() {
        assert!(super::recovery_process_alive(std::process::id(), "0"));
        assert!(!super::recovery_process_alive(u32::MAX - 1, "0"));
    }
    use super::{
        config_backup_path, default_config, ensure_config_backup, DepthXRBindings, DepthXRSettings,
        PivotXRModuleConfig, PivotXRProfileConfig, PivotXRSettings,
    };

    #[test]
    fn config_backup_preserves_the_pre_migration_document() {
        let suffix = std::time::SystemTime::now()
            .duration_since(std::time::UNIX_EPOCH)
            .expect("system time should be after the Unix epoch")
            .as_nanos();
        let directory = std::env::temp_dir().join(format!(
            "vectorxr-config-backup-{}-{suffix}",
            std::process::id()
        ));
        std::fs::create_dir_all(&directory).expect("temporary directory should be created");
        let config_path = directory.join("settings.json");
        std::fs::write(&config_path, "pre-0.16 config")
            .expect("legacy config fixture should be written");

        ensure_config_backup(&config_path).expect("the first backup should succeed");
        let backup_path = config_backup_path(&config_path);
        assert_eq!(
            std::fs::read_to_string(&backup_path).expect("backup should be readable"),
            "pre-0.16 config"
        );

        std::fs::write(&config_path, "normalized 0.16 config")
            .expect("updated config fixture should be written");
        ensure_config_backup(&config_path).expect("an existing backup should be retained");
        assert_eq!(
            std::fs::read_to_string(&backup_path).expect("backup should still be readable"),
            "pre-0.16 config"
        );
        std::fs::remove_dir_all(directory).expect("temporary directory should be removed");
    }

    #[test]
    fn saving_over_an_unreadable_config_keeps_a_copy_of_it() {
        let directory = std::env::temp_dir().join(format!(
            "vectorxr-unreadable-config-{}-{}",
            std::process::id(),
            super::unix_milliseconds()
        ));
        std::fs::create_dir_all(&directory).expect("temporary directory should be created");
        let config_path = directory.join("settings.json");
        let saved = serde_json::to_string_pretty(&default_config()).unwrap();
        let unreadable_files = || {
            std::fs::read_dir(&directory)
                .unwrap()
                .filter_map(|entry| entry.ok())
                .filter(|entry| entry.file_name().to_string_lossy().contains(".unreadable-"))
                .map(|entry| entry.path())
                .collect::<Vec<_>>()
        };

        // A readable config is replaced without an extra copy.
        std::fs::write(&config_path, &saved).unwrap();
        super::write_config_document(&config_path, &saved).unwrap();
        assert!(unreadable_files().is_empty());

        // A hand edit the app cannot read (here a value of the wrong type) is
        // kept before the app's defaults replace it.
        let hand_edited = saved.replacen("\"opacity\": 90", "\"opacity\": \"90%\"", 1);
        assert_ne!(hand_edited, saved, "fixture should change the OSD opacity");
        std::fs::write(&config_path, &hand_edited).unwrap();
        super::write_config_document(&config_path, &saved).unwrap();
        let kept = unreadable_files();
        assert_eq!(kept.len(), 1);
        assert_eq!(std::fs::read_to_string(&kept[0]).unwrap(), hand_edited);
        assert_eq!(std::fs::read_to_string(&config_path).unwrap(), saved);
        std::fs::remove_dir_all(directory).expect("temporary directory should be removed");
    }

    #[test]
    #[cfg(windows)]
    fn saving_waits_out_a_brief_lock_and_never_deletes_the_original() {
        use std::os::windows::fs::OpenOptionsExt;
        const FILE_SHARE_READ: u32 = 1;
        let directory = std::env::temp_dir().join(format!(
            "vectorxr-replace-{}-{}",
            std::process::id(),
            super::unix_milliseconds()
        ));
        std::fs::create_dir_all(&directory).unwrap();
        let path = directory.join("settings.json");
        std::fs::write(&path, "original").unwrap();
        let open_without_delete_sharing = || {
            std::fs::OpenOptions::new()
                .read(true)
                .share_mode(FILE_SHARE_READ)
                .open(&path)
                .unwrap()
        };

        // Another process reads the file for a moment (the layer reloading).
        let reader = open_without_delete_sharing();
        let release = std::thread::spawn(move || {
            std::thread::sleep(std::time::Duration::from_millis(150));
            drop(reader);
        });
        super::write_text_safely(&path, "saved").expect("the save should wait out a brief lock");
        release.join().unwrap();
        assert_eq!(std::fs::read_to_string(&path).unwrap(), "saved");

        // A lock that outlasts the retries fails the save but keeps the file.
        let reader = open_without_delete_sharing();
        assert!(super::write_text_safely(&path, "lost").is_err());
        drop(reader);
        assert_eq!(std::fs::read_to_string(&path).unwrap(), "saved");
        assert_eq!(std::fs::read_dir(&directory).unwrap().count(), 1, "no temporary file left");
        std::fs::remove_dir_all(directory).unwrap();
    }

    #[test]
    fn whole_numbers_written_as_floats_load_like_the_layer_reads_them() {
        let saved = serde_json::to_string_pretty(&default_config()).unwrap();
        let hand_edited = saved
            .replacen("\"opacity\": 90", "\"opacity\": 80.0", 1)
            .replacen("\"updateHz\": 5", "\"updateHz\": 10.0", 1);
        assert_ne!(hand_edited, saved, "fixture should change the OSD");
        let config = super::parse_config(&hand_edited).expect("90.0-style integers should load");
        assert_eq!(config.core.osd.opacity, 80);
        assert_eq!(config.core.osd.update_hz, 10);

        // A fraction is still an error, with the first parse's line and column.
        let fractional = saved.replacen("\"opacity\": 90", "\"opacity\": 80.5", 1);
        let error = super::parse_config(&fractional).unwrap_err().to_string();
        assert!(error.contains("line"), "{error}");
    }

    #[test]
    fn new_configs_enable_depth_anchor_by_default() {
        assert!(default_config().modules.depthxr.defaults.depth_anchor);
    }

    #[test]
    fn depth_anchor_survives_the_config_save_round_trip() {
        let settings: DepthXRSettings = serde_json::from_value(serde_json::json!({
            "stereoBoost": 1.2,
            "convergence": 0.07,
            "depthAnchor": true
        }))
        .expect("Depth settings should deserialize");

        let serialized = serde_json::to_value(settings).expect("Depth settings should serialize");
        assert_eq!(serialized["depthAnchor"], true);
    }

    #[test]
    fn depth_anchor_defaults_off_for_older_configs() {
        let settings: DepthXRSettings = serde_json::from_value(serde_json::json!({
            "stereoBoost": 1.2,
            "convergence": 0.0
        }))
        .expect("Legacy Depth settings should deserialize");

        assert!(!settings.depth_anchor);
    }

    #[test]
    fn legacy_pivot_binding_serializes_as_a_canonical_binding_list() {
        let profile: PivotXRProfileConfig = serde_json::from_value(serde_json::json!({
            "name": "Legacy",
            "activationBinding": {
                "type": "keyboard",
                "chord": ["F8"]
            }
        }))
        .expect("Legacy Pivot binding should deserialize");

        let serialized = serde_json::to_value(profile).expect("Pivot profile should serialize");
        assert!(serialized.get("activationBinding").is_none());
        assert_eq!(serialized["activationBindings"][0]["type"], "keyboard");
        assert_eq!(serialized["activationBindings"][0]["chord"][0], "F8");
    }

    #[test]
    fn pivot_binding_lists_preserve_multiple_bindings_and_drop_none() {
        let profile: PivotXRProfileConfig = serde_json::from_value(serde_json::json!({
            "name": "Multiple",
            "activationBindings": [
                { "type": "keyboard", "chord": ["F8"] },
                { "type": "none" },
                {
                    "type": "device",
                    "deviceGuid": "{ABC}",
                    "inputPath": "hat-1-left"
                }
            ]
        }))
        .expect("Pivot binding list should deserialize");

        let serialized = serde_json::to_value(profile).expect("Pivot profile should serialize");
        let bindings = serialized["activationBindings"]
            .as_array()
            .expect("Activation bindings should serialize as an array");
        assert_eq!(bindings.len(), 2);
        assert_eq!(bindings[0]["chord"][0], "F8");
        assert_eq!(bindings[1]["inputPath"], "hat-1-left");
    }

    #[test]
    fn canonical_pivot_activation_bindings_preserve_behavior_and_baseline() {
        let profile: PivotXRProfileConfig = serde_json::from_value(serde_json::json!({
            "name": "Mixed",
            "alwaysActive": true,
            "activationBindings": [
                { "behavior": "toggle", "binding": { "type": "keyboard", "chord": ["F8"] } },
                { "behavior": "hold", "binding": { "type": "keyboard", "chord": ["F9"] } }
            ]
        }))
        .expect("Canonical Pivot bindings should deserialize");

        let serialized = serde_json::to_value(profile).expect("Pivot profile should serialize");
        assert_eq!(serialized["alwaysActive"], true);
        assert_eq!(serialized["activationBindings"][0]["behavior"], "toggle");
        assert_eq!(
            serialized["activationBindings"][0]["binding"]["chord"][0],
            "F8"
        );
        assert_eq!(serialized["activationBindings"][1]["behavior"], "hold");
    }

    #[test]
    fn legacy_always_on_baseline_remains_available_for_frontend_migration() {
        let profile: PivotXRProfileConfig = serde_json::from_value(serde_json::json!({
            "name": "Legacy Always",
            "activationMode": "alwaysOn",
            "activationBindings": [{ "type": "keyboard", "chord": ["F10"] }]
        }))
        .expect("Legacy Always On profile should deserialize");

        let serialized = serde_json::to_value(profile).expect("Pivot profile should serialize");
        assert_eq!(serialized["activationMode"], "alwaysOn");
        assert!(serialized.get("alwaysActive").is_none());
        assert_eq!(serialized["activationBindings"][0]["type"], "keyboard");
    }

    #[test]
    fn expanded_pivot_settings_survive_the_config_save_round_trip() {
        let original = serde_json::json!({
            "applyMotionToSnapViews": true, "deadzoneTransitionDegrees": 4.0,
            "pitchDeadzoneTransitionDegrees": 2.0,
            "yawLeft": { "deadzoneTransitionDegrees": 7.0 },
            "positionAssist": {"enabled": true, "advancedDirections": true,
                "yaw": {"rightCm": 10.0, "upCm": 4.0, "startAngleDegrees": 15.0, "fullAngleDegrees": 60.0},
                "pitchUp": {"upCm": 12.0, "forwardCm": -5.0}}
        });
        let settings: PivotXRSettings = serde_json::from_value(original.clone()).unwrap();
        let saved = serde_json::to_value(settings).unwrap();
        for (key, value) in original.as_object().unwrap() { assert_eq!(&saved[key], value, "{key}"); }
    }

    #[test]
    fn pivot_step_settings_survive_the_config_save_round_trip() {
        let settings: PivotXRSettings = serde_json::from_value(serde_json::json!({
            "responseMode": "stepped",
            "stepGlideMode": "glide",
            "stepGlideSeconds": 0.18,
            "yawStep": {
                "deadzoneDegrees": 7.0,
                "triggerDegrees": 11.0,
                "amountDegrees": 17.0,
                "hysteresisDegrees": 3.0,
                "maxExtraDegrees": 80.0
            },
            "pitchDownStep": {
                "deadzoneDegrees": 12.0,
                "triggerDegrees": 16.0,
                "amountDegrees": 22.0,
                "hysteresisDegrees": 6.0,
                "maxExtraDegrees": 40.0
            }
        }))
        .expect("Pivot stepped settings should deserialize");

        let serialized =
            serde_json::to_value(settings).expect("Pivot stepped settings should serialize");
        assert_eq!(serialized["responseMode"], "stepped");
        assert_eq!(serialized["stepGlideMode"], "glide");
        assert_eq!(serialized["stepGlideSeconds"], 0.18);
        assert_eq!(serialized["yawStep"]["triggerDegrees"], 11.0);
        assert_eq!(serialized["pitchDownStep"]["amountDegrees"], 22.0);
    }

    #[test]
    fn pivot_behavior_nudges_and_view_controls_survive_the_config_save_round_trip() {
        let pivot: PivotXRModuleConfig = serde_json::from_value(serde_json::json!({
            "enabled": true,
            "behavior": "snapViews",
            "snapTurnPreference": "right",
            "nudgeSetId": "shared-hat",
            "nudgeSets": [{
                "id": "shared-hat",
                "name": "Shared HAT",
                "allowWhileInactive": true,
                "settings": {
                    "yawStepDegrees": 30.0,
                    "pitchStepDegrees": 20.0,
                    "transitionSeconds": 0.12,
                    "yawLeftBindings": [{ "type": "keyboard", "chord": ["Q"] }],
                    "yawRightBindings": [],
                    "pitchUpBindings": [],
                    "pitchDownBindings": [],
                    "centerBindings": []
                }
            }],
            "profiles": [{
                "name": "Accessible Views",
                "behavior": "snapViews",
                "snapTurnPreference": "left",
                "nudgeSetId": "shared-hat",
                "viewControls": {
                "nudges": {
                    "yawStepDegrees": 30.0,
                    "pitchStepDegrees": 20.0,
                    "transitionSeconds": 0.12,
                    "yawLeftBindings": [{ "type": "keyboard", "chord": ["Q"] }],
                    "yawRightBindings": [],
                    "pitchUpBindings": [],
                    "pitchDownBindings": [],
                    "centerBindings": []
                },
                "quickViews": [{
                    "id": "check-six",
                    "name": "Check Six",
                    "yawDegrees": 180.0,
                    "pitchDegrees": 0.0,
                    "positionRightCm": 0.0,
                    "positionUpCm": 0.0,
                    "positionForwardCm": 0.0,
                    "transitionSeconds": 0.18,
                    "turnDirection": "right",
                    "activationBindings": [{
                        "behavior": "toggle",
                        "binding": { "type": "keyboard", "chord": ["F9"] }
                    }]
                }]
            }}]
        }))
        .expect("Pivot configuration should deserialize");

        let serialized = serde_json::to_value(pivot).expect("Pivot configuration should serialize");
        assert_eq!(serialized["behavior"], "snapViews");
        assert_eq!(serialized["snapTurnPreference"], "right");
        assert_eq!(serialized["nudgeSetId"], "shared-hat");
        assert_eq!(serialized["nudgeSets"][0]["allowWhileInactive"], true);
        assert_eq!(
            serialized["nudgeSets"][0]["settings"]["yawLeftBindings"][0]["chord"][0],
            "Q"
        );
        assert_eq!(serialized["profiles"][0]["behavior"], "snapViews");
        assert_eq!(serialized["profiles"][0]["snapTurnPreference"], "left");
        assert_eq!(serialized["profiles"][0]["nudgeSetId"], "shared-hat");
        assert_eq!(
            serialized["profiles"][0]["viewControls"]["nudges"]["yawStepDegrees"],
            30.0
        );
        assert_eq!(
            serialized["profiles"][0]["viewControls"]["nudges"]["yawLeftBindings"][0]["chord"][0],
            "Q"
        );
        assert_eq!(
            serialized["profiles"][0]["viewControls"]["quickViews"][0]["name"],
            "Check Six"
        );
        assert_eq!(
            serialized["profiles"][0]["viewControls"]["quickViews"][0]["activationBindings"][0]
                ["behavior"],
            "toggle"
        );
    }

    #[test]
    fn depth_anchor_toggle_binding_survives_the_config_save_round_trip() {
        let bindings: DepthXRBindings = serde_json::from_value(serde_json::json!({
            "toggleEnabled": { "type": "none" },
            "toggleAnchor": {
                "type": "keyboard",
                "chord": ["F9"]
            }
        }))
        .expect("Depth bindings should deserialize");

        let serialized = serde_json::to_value(bindings).expect("Depth bindings should serialize");
        assert_eq!(serialized["toggleAnchor"]["type"], "keyboard");
        assert_eq!(serialized["toggleAnchor"]["chord"][0], "F9");
    }

    fn saved(config: serde_json::Value) -> serde_json::Value {
        let config: super::VectorXRConfig = serde_json::from_value(config).expect("config should deserialize");
        serde_json::to_value(super::normalize_config(config)).expect("config should serialize")
    }

    // A toggle keeps one binding, or none, under its single legacy key, which
    // layers from before binding lists read too; two or more go under the
    // plural key alone.
    #[test]
    fn toggle_binding_lists_are_written_so_older_layers_read_single_bindings() {
        let hotas = serde_json::json!({ "type": "device", "deviceGuid": "{1}", "inputPath": "button-23" });
        let hosas = serde_json::json!({ "type": "device", "deviceGuid": "{2}", "inputPath": "button-5" });
        let output = saved(serde_json::json!({
            "version": 3,
            "core": { "osd": { "toggleBindings": [hotas, hosas], "cycleBindings": [] } },
            "applications": [],
            "modules": {
                "depthxr": { "bindings": { "toggleEnabledBindings": [hotas], "toggleAnchorBindings": [hotas, hosas] } },
                "turbo": { "toggleBindings": [], "metricsBindings": [hosas] },
                "framegen": { "toggleBindings": [hotas, hosas] }
            }
        }));
        let osd = &output["core"]["osd"];
        assert_eq!(osd["toggleBindings"].as_array().map(Vec::len), Some(2));
        assert!(osd.get("toggleBinding").is_none());
        assert_eq!(osd["cycleBinding"]["type"], "none", "an emptied list is written as none, not left to the default");
        assert!(osd.get("cycleBindings").is_none());
        let depth = &output["modules"]["depthxr"]["bindings"];
        assert_eq!(depth["toggleEnabled"]["inputPath"], "button-23");
        assert!(depth.get("toggleEnabledBindings").is_none());
        assert_eq!(depth["toggleAnchorBindings"][1]["inputPath"], "button-5");
        assert!(depth.get("toggleAnchor").is_none());
        let turbo = &output["modules"]["turbo"];
        assert_eq!(turbo["toggleBinding"]["type"], "none");
        assert_eq!(turbo["metricsBinding"]["inputPath"], "button-5");
        assert!(turbo.get("toggleBindings").is_none() && turbo.get("metricsBindings").is_none());
        let framegen = &output["modules"]["framegen"];
        assert_eq!(framegen["toggleBindings"].as_array().map(Vec::len), Some(2));
        assert!(framegen.get("toggleBinding").is_none());
    }

    #[test]
    fn toggles_without_either_key_keep_their_defaults() {
        let output = saved(serde_json::json!({ "version": 3, "core": { "osd": {} }, "applications": [], "modules": {} }));
        assert_eq!(output["core"]["osd"]["toggleBinding"]["chord"], serde_json::json!(["Ctrl", "Alt", "F10"]));
        assert_eq!(output["core"]["osd"]["cycleBinding"]["chord"], serde_json::json!(["Ctrl", "Alt", "F11"]));
        assert_eq!(output["modules"]["depthxr"]["bindings"]["toggleEnabled"]["type"], "none");
        assert_eq!(output["modules"]["framegen"]["toggleBinding"]["type"], "none");
        // A legacy binding beside a list (a hand edit) joins it.
        let merged = saved(serde_json::json!({
            "version": 3, "applications": [],
            "modules": { "turbo": {
                "toggleBinding": { "type": "keyboard", "chord": ["F7"] },
                "toggleBindings": [{ "type": "keyboard", "chord": ["F8"] }]
            } }
        }));
        assert_eq!(merged["modules"]["turbo"]["toggleBindings"].as_array().map(Vec::len), Some(2));
        assert!(merged["modules"]["turbo"].get("toggleBinding").is_none());
    }
}

fn main() {
    if let Some(result) = openxr_layers::run_elevated_helper_from_args() {
        if let Err(error) = result {
            eprintln!("OpenXR elevated helper failed: {error}");
        }
        return;
    }

    let instance = match single_instance::claim() {
        single_instance::Claim::Signalled => return,
        single_instance::Claim::First(instance) => Some(instance),
        single_instance::Claim::Unclaimed => None,
    };

    tauri::Builder::default()
        .plugin(tauri_plugin_dialog::init())
        .manage(tray::TrayState::default())
        .on_window_event(tray::on_window_event)
        .setup(move |app| {
            if let Some(instance) = instance {
                let handle = app.handle().clone();
                instance.listen(move || {
                    let app = handle.clone();
                    let _ = handle.run_on_main_thread(move || tray::show_main_window(&app));
                });
            }
            Ok(())
        })
        .invoke_handler(tauri::generate_handler![
            load_config,
            save_config,
            reset_stored_data,
            load_log_snapshot,
            open_file_directory,
            default_screenshot_folder,
            open_screenshot_folder,
            open_external_url,
            load_community_override,
            installer_language,
            load_seen_apps,
            clear_seen_apps,
            load_runtime_pacing,
            clear_runtime_pacing_observation,
            load_turbo_metrics,
            clear_turbo_metrics,
            load_runtime_status,
            clear_turbo_safety_block,
            clear_turbo_fault_logs,
            load_debug_sources,
            set_runtime_quadviews_diagnostic_visualization,
            set_runtime_mirror_visible,
            request_input_rescan,
            load_gpu_capabilities,
            list_input_devices,
            capture_device_binding,
            cancel_device_binding_capture,
            load_openxr_layers,
            ensure_openxr_layer_elevation,
            set_openxr_layer_enabled,
            move_openxr_layer,
            delete_openxr_layer,
            play_test_sound,
            tray::configure_tray,
            tray::update_tray_status
        ])
        .run(tauri::generate_context!())
        .expect("failed to run VectorXR Tauri app");
}
