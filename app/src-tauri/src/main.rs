#![cfg_attr(not(debug_assertions), windows_subsystem = "windows")]

use serde::{Deserialize, Serialize};
use std::env;
use std::fs;
use std::path::{Path, PathBuf};

fn default_enabled() -> bool {
    true
}

fn default_stereo_boost() -> f64 {
    1.10
}

fn default_convergence() -> f64 {
    0.0
}

fn default_world_scale() -> f64 {
    1.0
}

fn default_fov_scale() -> f64 {
    1.0
}

fn default_log_level() -> String {
    "info".into()
}

#[derive(Debug, Clone, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
struct SettingsBlock {
    #[serde(default = "default_enabled")]
    enabled: bool,
    #[serde(default = "default_enabled")]
    stereo_boost_enabled: bool,
    #[serde(default = "default_enabled")]
    convergence_enabled: bool,
    #[serde(default = "default_enabled")]
    world_scale_enabled: bool,
    #[serde(default = "default_enabled")]
    fov_scale_enabled: bool,
    #[serde(default = "default_stereo_boost")]
    stereo_boost: f64,
    #[serde(default = "default_convergence")]
    convergence: f64,
    #[serde(default = "default_world_scale")]
    world_scale: f64,
    #[serde(default = "default_fov_scale")]
    fov_scale: f64,
    #[serde(default = "default_log_level")]
    log_level: String,
}

#[derive(Debug, Clone, Serialize, Deserialize)]
struct ProfileMatch {
    exe: String,
}

#[derive(Debug, Clone, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
struct ProfileConfig {
    #[serde(flatten)]
    settings: SettingsBlock,
    r#match: ProfileMatch,
}

#[derive(Debug, Clone, Serialize, Deserialize)]
struct DepthXRConfig {
    version: u32,
    global: SettingsBlock,
    profiles: Vec<ProfileConfig>,
}

#[derive(Debug, Clone, Serialize)]
struct ConfigEnvelope {
    path: String,
    config: DepthXRConfig,
}

fn default_config() -> DepthXRConfig {
    DepthXRConfig {
        version: 1,
        global: SettingsBlock {
            enabled: true,
            stereo_boost_enabled: true,
            convergence_enabled: true,
            world_scale_enabled: true,
            fov_scale_enabled: true,
            stereo_boost: 1.10,
            convergence: 0.0,
            world_scale: 1.0,
            fov_scale: 1.0,
            log_level: "info".into(),
        },
        profiles: Vec::new(),
    }
}

fn resolve_config_path() -> PathBuf {
    if let Ok(env_path) = env::var("DEPTHXR_CONFIG_PATH") {
        if !env_path.trim().is_empty() {
            return PathBuf::from(env_path);
        }
    }

    #[cfg(target_os = "windows")]
    {
        if let Ok(local_app_data) = env::var("LOCALAPPDATA") {
            return PathBuf::from(local_app_data)
                .join("DepthXR")
                .join("config")
                .join("settings.json");
        }
    }

    env::current_dir()
        .unwrap_or_else(|_| PathBuf::from("."))
        .join("config")
        .join("depthxr.settings.json")
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
    let config = serde_json::from_str::<DepthXRConfig>(&content).map_err(|error| error.to_string())?;

    Ok(ConfigEnvelope {
        path: path.to_string_lossy().into_owned(),
        config,
    })
}

#[tauri::command]
fn save_config(config: DepthXRConfig) -> Result<String, String> {
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
        .expect("failed to run DepthXR Tauri app");
}
