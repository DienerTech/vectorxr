#![cfg_attr(not(debug_assertions), windows_subsystem = "windows")]

use serde::{Deserialize, Serialize};
use std::env;
use std::fs;
use std::path::{Path, PathBuf};
use std::process::Command;
use std::time::UNIX_EPOCH;

mod input_devices;
mod openxr_layers;

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

fn default_activation_mode() -> String {
    "toggle".into()
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
#[serde(rename_all = "camelCase")]
struct CoreConfig {
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
            depth_anchor: false,
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
    #[serde(default = "default_depth_toggle_binding")]
    toggle_enabled: InputBinding,
}

impl Default for DepthXRBindings {
    fn default() -> Self {
        Self {
            toggle_enabled: default_depth_toggle_binding(),
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
    #[serde(default, skip_serializing_if = "Option::is_none")]
    r#match: Option<ProfileMatch>,
    #[serde(default = "default_activation_mode")]
    activation_mode: String,
    #[serde(default = "default_activation_binding")]
    activation_binding: InputBinding,
    #[serde(default = "default_activation_binding")]
    set_origin_binding: InputBinding,
    #[serde(default = "default_activation_binding")]
    release_origin_binding: InputBinding,
    #[serde(default)]
    settings: PivotXRSettings,
}

#[derive(Debug, Clone, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
struct PivotXRModuleConfig {
    #[serde(default = "default_false")]
    enabled: bool,
    #[serde(default)]
    defaults: PivotXRSettings,
    #[serde(default = "default_activation_mode")]
    activation_mode: String,
    #[serde(default = "default_activation_binding")]
    activation_binding: InputBinding,
    #[serde(default = "default_activation_binding")]
    set_origin_binding: InputBinding,
    #[serde(default = "default_activation_binding")]
    release_origin_binding: InputBinding,
    #[serde(default)]
    profiles: Vec<PivotXRProfileConfig>,
}

impl Default for PivotXRModuleConfig {
    fn default() -> Self {
        Self {
            enabled: false,
            defaults: PivotXRSettings::default(),
            activation_mode: default_activation_mode(),
            activation_binding: default_activation_binding(),
            set_origin_binding: default_activation_binding(),
            release_origin_binding: default_activation_binding(),
            profiles: Vec::new(),
        }
    }
}

#[derive(Debug, Clone, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
struct QuadViewsSettings {
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
    #[serde(default)]
    defaults: QuadViewsSettings,
    #[serde(default)]
    profiles: Vec<QuadViewsProfileConfig>,
}

impl Default for QuadViewsModuleConfig {
    fn default() -> Self {
        Self {
            enabled: false,
            defaults: QuadViewsSettings::default(),
            profiles: Vec::new(),
        }
    }
}

#[derive(Debug, Clone, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
struct TurboProfileConfig {
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
    #[serde(default = "default_false")]
    enabled: bool,
    #[serde(default = "default_activation_binding")]
    toggle_binding: InputBinding,
    #[serde(default = "default_turbo_pacing_mode")]
    pacing_mode: String,
    #[serde(default)]
    runtime_pins: std::collections::BTreeMap<String, String>,
    #[serde(default = "default_turbo_metrics_mode")]
    metrics_mode: String,
    #[serde(default = "default_activation_binding")]
    metrics_binding: InputBinding,
    #[serde(default)]
    profiles: Vec<TurboProfileConfig>,
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
            enabled: false,
            toggle_binding: default_activation_binding(),
            pacing_mode: default_turbo_pacing_mode(),
            runtime_pins: std::collections::BTreeMap::new(),
            metrics_mode: default_turbo_metrics_mode(),
            metrics_binding: default_activation_binding(),
            profiles: Vec::new(),
        }
    }
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

fn default_config() -> VectorXRConfig {
    VectorXRConfig {
        version: 3,
        core: CoreConfig::default(),
        applications: Vec::new(),
        modules: VectorXRModules::default(),
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

fn normalize_config(mut config: VectorXRConfig) -> VectorXRConfig {
    if config.version != 3 {
        return default_config();
    }

    config.version = 3;

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
    fs::read_to_string(path)
        .ok()
        .and_then(|content| serde_json::from_str::<RuntimePacingDocument>(&content).ok())
        .unwrap_or_else(|| RuntimePacingDocument {
            version: 1,
            observations: Vec::new(),
        })
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

    match fs::rename(&temporary_path, path) {
        Ok(()) => Ok(()),
        Err(first_error) => {
            if path.exists() {
                fs::remove_file(path).map_err(|error| {
                    let _ = fs::remove_file(&temporary_path);
                    format!(
                        "Unable to replace {}: {}; original rename error: {}",
                        path.to_string_lossy(),
                        error,
                        first_error
                    )
                })?;

                fs::rename(&temporary_path, path).map_err(|error| {
                    let _ = fs::remove_file(&temporary_path);
                    error.to_string()
                })
            } else {
                let _ = fs::remove_file(&temporary_path);
                Err(first_error.to_string())
            }
        }
    }
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
    const MAX_BYTES: usize = 120_000;

    let Ok(content) = fs::read_to_string(path) else {
        return "Unable to read this log file.".into();
    };

    if content.len() <= MAX_BYTES {
        return content;
    }

    let start = content.len().saturating_sub(MAX_BYTES);
    format!(
        "... truncated to the most recent log output ...\n{}",
        &content[start..]
    )
}

#[tauri::command]
fn load_config() -> Result<ConfigEnvelope, String> {
    let path = resolve_config_path();
    ensure_default_file(&path)?;

    let content = fs::read_to_string(&path).map_err(|error| error.to_string())?;
    let config = normalize_config(
        serde_json::from_str::<VectorXRConfig>(&content).map_err(|error| error.to_string())?,
    );

    Ok(ConfigEnvelope {
        path: path.to_string_lossy().into_owned(),
        config,
    })
}

#[tauri::command]
fn save_config(config: VectorXRConfig) -> Result<String, String> {
    let path = resolve_config_path();

    let content = serde_json::to_string_pretty(&normalize_config(config))
        .map_err(|error| error.to_string())?;
    write_text_safely(&path, &content)?;
    Ok(path.to_string_lossy().into_owned())
}

#[tauri::command]
fn reset_stored_data() -> Result<ResetStoredDataEnvelope, String> {
    let config_path = resolve_config_path();
    let seen_apps_path = resolve_seen_apps_path();
    let config = default_config();

    let config_content =
        serde_json::to_string_pretty(&config).map_err(|error| error.to_string())?;
    write_text_safely(&config_path, &config_content)?;

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

#[tauri::command]
fn list_input_devices() -> Result<Vec<input_devices::InputDeviceInfo>, String> {
    input_devices::list_input_devices()
}

#[tauri::command]
fn capture_device_binding(
    timeout_ms: Option<u64>,
) -> Result<Option<input_devices::CapturedDeviceBinding>, String> {
    input_devices::capture_device_binding(timeout_ms)
}

#[tauri::command]
fn load_log_snapshot() -> Result<LogSnapshot, String> {
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
        return Err(format!(
            "Directory does not exist: {}",
            directory.to_string_lossy()
        ));
    }

    #[cfg(target_os = "windows")]
    {
        Command::new("explorer")
            .arg(directory)
            .spawn()
            .map_err(|error| error.to_string())?;
        return Ok(());
    }

    #[cfg(target_os = "macos")]
    {
        Command::new("open")
            .arg(directory)
            .spawn()
            .map_err(|error| error.to_string())?;
        return Ok(());
    }

    #[cfg(all(unix, not(target_os = "macos")))]
    {
        Command::new("xdg-open")
            .arg(directory)
            .spawn()
            .map_err(|error| error.to_string())?;
        return Ok(());
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
    let path = resolve_runtime_pacing_path();
    let mut document = read_runtime_pacing_document(&path);
    document
        .observations
        .retain(|observation| observation.runtime_name != runtime_name);
    let content = serde_json::to_string_pretty(&document).map_err(|error| error.to_string())?;
    write_text_safely(&path, &content)?;
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

#[tauri::command]
fn load_openxr_layers() -> Result<openxr_layers::OpenXrLayerSnapshot, String> {
    openxr_layers::load_openxr_layers()
}

#[tauri::command]
fn ensure_openxr_layer_elevation() -> Result<(), String> {
    openxr_layers::ensure_openxr_layer_elevation()
}

#[tauri::command]
fn set_openxr_layer_enabled(
    slice: String,
    manifest_path: String,
    enabled: bool,
) -> Result<openxr_layers::OpenXrLayerSnapshot, String> {
    openxr_layers::set_openxr_layer_enabled(slice, manifest_path, enabled)
}

#[tauri::command]
fn move_openxr_layer(
    slice: String,
    manifest_path: String,
    direction: openxr_layers::MoveDirection,
) -> Result<openxr_layers::OpenXrLayerSnapshot, String> {
    openxr_layers::move_openxr_layer(slice, manifest_path, direction)
}

#[tauri::command]
fn delete_openxr_layer(
    slice: String,
    manifest_path: String,
) -> Result<openxr_layers::OpenXrLayerSnapshot, String> {
    openxr_layers::delete_openxr_layer(slice, manifest_path)
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
        let size =
            u32::from_le_bytes([bytes[pos + 4], bytes[pos + 5], bytes[pos + 6], bytes[pos + 7]])
                as usize;
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
fn play_test_sound(
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
        return Err(format!(
            "Sound file not found: {}",
            resolved.to_string_lossy()
        ));
    }

    #[cfg(target_os = "windows")]
    {
        use std::os::windows::ffi::OsStrExt;
        use windows::core::PCWSTR;
        use windows::Win32::Media::Audio::{
            PlaySoundW, SND_FILENAME, SND_MEMORY, SND_NODEFAULT,
        };

        let gain = (volume.unwrap_or(100).min(100) as f32) / 100.0;

        // Scaled in-memory playback is synchronous (no SND_ASYNC) so the buffer
        // stays valid for the lifetime of the call; the clip is a fraction of a
        // second, so briefly blocking this command thread is fine.
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
    use super::DepthXRSettings;

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
}
fn main() {
    if let Some(result) = openxr_layers::run_elevated_helper_from_args() {
        if let Err(error) = result {
            eprintln!("OpenXR elevated helper failed: {error}");
        }
        return;
    }

    tauri::Builder::default()
        .plugin(tauri_plugin_dialog::init())
        .invoke_handler(tauri::generate_handler![
            load_config,
            save_config,
            reset_stored_data,
            load_log_snapshot,
            open_file_directory,
            open_external_url,
            load_seen_apps,
            clear_seen_apps,
            load_runtime_pacing,
            clear_runtime_pacing_observation,
            load_turbo_metrics,
            clear_turbo_metrics,
            list_input_devices,
            capture_device_binding,
            load_openxr_layers,
            ensure_openxr_layer_elevation,
            set_openxr_layer_enabled,
            move_openxr_layer,
            delete_openxr_layer,
            play_test_sound
        ])
        .run(tauri::generate_context!())
        .expect("failed to run VectorXR Tauri app");
}
