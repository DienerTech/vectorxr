#![cfg_attr(not(debug_assertions), windows_subsystem = "windows")]

use serde::{Deserialize, Serialize};
use std::env;
use std::fs;
use std::path::{Path, PathBuf};

fn default_true() -> bool {
    true
}

fn default_false() -> bool {
    false
}

fn default_version() -> u32 {
    2
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

fn default_smoothing() -> f64 {
    0.2
}

fn default_deadzone_degrees() -> f64 {
    8.0
}

#[derive(Debug, Clone, Serialize, Deserialize)]
struct ProfileMatch {
    exe: String,
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
    r#match: ProfileMatch,
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
    profiles: Vec<DepthXRProfileConfig>,
}

impl Default for DepthXRModuleConfig {
    fn default() -> Self {
        Self {
            enabled: true,
            defaults: DepthXRSettings::default(),
            profiles: Vec::new(),
        }
    }
}

#[derive(Debug, Clone, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
struct PivotXRDefaults {
    #[serde(default = "default_activation_mode")]
    activation_mode: String,
    #[serde(default = "default_rotation_multiplier")]
    rotation_multiplier: f64,
    #[serde(default = "default_smoothing")]
    smoothing: f64,
    #[serde(default = "default_deadzone_degrees")]
    deadzone_degrees: f64,
}

impl Default for PivotXRDefaults {
    fn default() -> Self {
        Self {
            activation_mode: default_activation_mode(),
            rotation_multiplier: default_rotation_multiplier(),
            smoothing: default_smoothing(),
            deadzone_degrees: default_deadzone_degrees(),
        }
    }
}

#[derive(Debug, Clone, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
struct PivotXRModuleConfig {
    #[serde(default = "default_false")]
    enabled: bool,
    #[serde(default)]
    defaults: PivotXRDefaults,
}

impl Default for PivotXRModuleConfig {
    fn default() -> Self {
        Self {
            enabled: false,
            defaults: PivotXRDefaults::default(),
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
    modules: VectorXRModules,
}

#[derive(Debug, Clone, Serialize)]
struct ConfigEnvelope {
    path: String,
    config: VectorXRConfig,
}

fn default_config() -> VectorXRConfig {
    VectorXRConfig {
        version: 2,
        core: CoreConfig::default(),
        modules: VectorXRModules::default(),
    }
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

#[tauri::command]
fn load_config() -> Result<ConfigEnvelope, String> {
    let path = resolve_config_path();
    ensure_default_file(&path)?;

    let content = fs::read_to_string(&path).map_err(|error| error.to_string())?;
    let config = serde_json::from_str::<VectorXRConfig>(&content).map_err(|error| error.to_string())?;

    Ok(ConfigEnvelope {
        path: path.to_string_lossy().into_owned(),
        config,
    })
}

#[tauri::command]
fn save_config(config: VectorXRConfig) -> Result<String, String> {
    let path = resolve_config_path();
    ensure_parent(&path)?;

    let content = serde_json::to_string_pretty(&config).map_err(|error| error.to_string())?;
    fs::write(&path, content).map_err(|error| error.to_string())?;
    Ok(path.to_string_lossy().into_owned())
}

fn main() {
    tauri::Builder::default()
        .invoke_handler(tauri::generate_handler![load_config, save_config])
        .run(tauri::generate_context!())
        .expect("failed to run VectorXR Tauri app");
}
