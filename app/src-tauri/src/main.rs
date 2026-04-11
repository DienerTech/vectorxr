#![cfg_attr(not(debug_assertions), windows_subsystem = "windows")]

use serde::{Deserialize, Serialize};
use std::env;
use std::fs;
use std::path::{Path, PathBuf};
use std::time::UNIX_EPOCH;

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
    1.10
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

#[derive(Debug, Clone, Serialize, Deserialize)]
#[serde(tag = "type", rename_all = "camelCase")]
enum InputBinding {
    None,
    #[serde(rename_all = "camelCase")]
    Keyboard {
        #[serde(default)]
        chord: Vec<String>,
    },
    #[serde(rename_all = "camelCase")]
    Device {
        #[serde(default)]
        device_guid: String,
        #[serde(default = "default_input_path")]
        input_path: String,
    },
}

fn default_input_path() -> String {
    "button-1".into()
}

fn default_smoothing() -> f64 {
    0.2
}

fn default_deadzone_degrees() -> f64 {
    8.0
}

fn default_max_extra_yaw_degrees() -> f64 {
    25.0
}

fn default_pitch_rotation_multiplier() -> f64 {
    1.0
}

fn default_pitch_smoothing() -> f64 {
    0.2
}

fn default_pitch_deadzone_degrees() -> f64 {
    12.0
}

fn default_max_extra_pitch_degrees() -> f64 {
    20.0
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

#[derive(Debug, Clone, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
struct CoreConfig {
    #[serde(default = "default_true")]
    enabled: bool,
    #[serde(default = "default_log_level")]
    log_level: String,
    #[serde(default = "default_log_retention_files")]
    log_retention_files: u32,
}

impl Default for CoreConfig {
    fn default() -> Self {
        Self {
            enabled: true,
            log_level: default_log_level(),
            log_retention_files: default_log_retention_files(),
        }
    }
}

#[derive(Debug, Clone, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
struct DepthXRSettings {
    #[serde(default = "default_true")]
    stereo_boost_enabled: bool,
    #[serde(default = "default_true")]
    convergence_enabled: bool,
    #[serde(default = "default_stereo_boost")]
    stereo_boost: f64,
    #[serde(default = "default_convergence")]
    convergence: f64,
}

impl Default for DepthXRSettings {
    fn default() -> Self {
        Self {
            stereo_boost_enabled: true,
            convergence_enabled: true,
            stereo_boost: default_stereo_boost(),
            convergence: default_convergence(),
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
    #[serde(default = "default_true")]
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
            enabled: true,
            defaults: DepthXRSettings::default(),
            bindings: DepthXRBindings::default(),
            profiles: Vec::new(),
        }
    }
}

#[derive(Debug, Clone, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
struct PivotXRSettings {
    #[serde(default = "default_rotation_multiplier")]
    rotation_multiplier: f64,
    #[serde(default = "default_smoothing")]
    smoothing: f64,
    #[serde(default = "default_deadzone_degrees")]
    deadzone_degrees: f64,
    #[serde(default = "default_max_extra_yaw_degrees")]
    max_extra_yaw_degrees: f64,
    #[serde(default = "default_pitch_rotation_multiplier")]
    pitch_rotation_multiplier: f64,
    #[serde(default = "default_pitch_smoothing")]
    pitch_smoothing: f64,
    #[serde(default = "default_pitch_deadzone_degrees")]
    pitch_deadzone_degrees: f64,
    #[serde(default = "default_max_extra_pitch_degrees")]
    max_extra_pitch_degrees: f64,
}

impl Default for PivotXRSettings {
    fn default() -> Self {
        Self {
            rotation_multiplier: default_rotation_multiplier(),
            smoothing: default_smoothing(),
            deadzone_degrees: default_deadzone_degrees(),
            max_extra_yaw_degrees: default_max_extra_yaw_degrees(),
            pitch_rotation_multiplier: default_pitch_rotation_multiplier(),
            pitch_smoothing: default_pitch_smoothing(),
            pitch_deadzone_degrees: default_pitch_deadzone_degrees(),
            max_extra_pitch_degrees: default_max_extra_pitch_degrees(),
        }
    }
}

#[derive(Debug, Clone, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
struct PivotXRProfileConfig {
    #[serde(default)]
    name: String,
    #[serde(default = "default_true")]
    enabled: bool,
    #[serde(default)]
    application_ids: Vec<String>,
    #[serde(default = "default_activation_mode")]
    activation_mode: String,
    #[serde(default = "default_activation_binding")]
    activation_binding: InputBinding,
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

fn default_config() -> VectorXRConfig {
    VectorXRConfig {
        version: 3,
        core: CoreConfig::default(),
        applications: Vec::new(),
        modules: VectorXRModules::default(),
    }
}

fn sanitize_profile_name(exe: &str) -> String {
    let basename = exe
        .rsplit(['/', '\\'])
        .next()
        .unwrap_or(exe)
        .trim();

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
        if profile.name.trim().is_empty() {
            let first_application = profile
                .application_ids
                .first()
                .and_then(|application_id| config.applications.iter().find(|application| &application.id == application_id));

            profile.name = first_application
                .map(|application| application.name.clone())
                .unwrap_or_else(|| "New Profile".into());
        }

        profile.r#match = None;
    }

    for profile in &mut config.modules.pivotxr.profiles {
        if profile.name.trim().is_empty() {
            let first_application = profile
                .application_ids
                .first()
                .and_then(|application_id| config.applications.iter().find(|application| &application.id == application_id));

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
                .join("vectorxr-layer.log");
        }
    }

    env::current_dir()
        .unwrap_or_else(|_| PathBuf::from("."))
        .join("logs")
        .join("vectorxr-layer.log")
}

fn ensure_parent(path: &Path) -> Result<(), String> {
    if let Some(parent) = path.parent() {
        fs::create_dir_all(parent).map_err(|error| error.to_string())?;
    }
    Ok(())
}

fn ensure_default_file(path: &Path) -> Result<(), String> {
    if path.exists() {
        return Ok(());
    }

    ensure_parent(path)?;
    let json = serde_json::to_string_pretty(&default_config()).map_err(|error| error.to_string())?;
    fs::write(path, json).map_err(|error| error.to_string())
}

fn log_series_paths(base_path: &Path) -> Result<Vec<PathBuf>, String> {
    let directory = base_path.parent().unwrap_or_else(|| Path::new("."));
    if !directory.exists() {
        return Ok(Vec::new());
    }

    let stem = base_path
        .file_stem()
        .and_then(|value| value.to_str())
        .unwrap_or("vectorxr-layer")
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

        if file_stem == stem || file_stem.starts_with(&(stem.clone() + "-")) {
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
    format!("... truncated to the most recent log output ...\n{}", &content[start..])
}

#[tauri::command]
fn load_config() -> Result<ConfigEnvelope, String> {
    let path = resolve_config_path();
    ensure_default_file(&path)?;

    let content = fs::read_to_string(&path).map_err(|error| error.to_string())?;
    let config = normalize_config(serde_json::from_str::<VectorXRConfig>(&content).map_err(|error| error.to_string())?);

    Ok(ConfigEnvelope {
        path: path.to_string_lossy().into_owned(),
        config,
    })
}

#[tauri::command]
fn save_config(config: VectorXRConfig) -> Result<String, String> {
    let path = resolve_config_path();
    ensure_parent(&path)?;

    let content = serde_json::to_string_pretty(&normalize_config(config)).map_err(|error| error.to_string())?;
    fs::write(&path, content).map_err(|error| error.to_string())?;
    Ok(path.to_string_lossy().into_owned())
}

#[tauri::command]
fn load_log_snapshot() -> Result<LogSnapshot, String> {
    let base_path = resolve_log_path();
    let directory = base_path.parent().unwrap_or_else(|| Path::new(".")).to_path_buf();
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

fn main() {
    tauri::Builder::default()
        .invoke_handler(tauri::generate_handler![load_config, save_config, load_log_snapshot])
        .run(tauri::generate_context!())
        .expect("failed to run VectorXR Tauri app");
}
