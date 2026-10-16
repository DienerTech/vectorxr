// The notification-area (system tray) icon: health at a glance, the master
// switch, the log level, and the way into the debug export, while the main
// window is behind a game or closed to the tray.
//
// The page owns the settings and the health checks. The tray only shows what
// the page last reported (update_tray_status) and sends clicks back to it as
// "tray-action" events, so a tray change is saved the same way as a change
// made on the page. Every tray call runs on the main thread (synchronous
// commands, menu and window events), so the menu is never touched from two
// threads at once.

use serde::{Deserialize, Serialize};
use std::sync::Mutex;
use tauri::image::Image;
use tauri::menu::{CheckMenuItem, Menu, MenuEvent, MenuItem, PredefinedMenuItem, Submenu};
use tauri::tray::{MouseButton, MouseButtonState, TrayIcon, TrayIconBuilder, TrayIconEvent};
use tauri::{AppHandle, Emitter, Manager, State, Window, WindowEvent, Wry};

pub const MAIN_WINDOW: &str = "main";
const TRAY_ID: &str = "vectorxr";
const ACTION_EVENT: &str = "tray-action";

const ITEM_HEALTH: &str = "tray-health";
const ITEM_ENABLED: &str = "tray-enabled";
const ITEM_LOG_INFO: &str = "tray-log-info";
const ITEM_LOG_DEBUG: &str = "tray-log-debug";
const ITEM_EXPORT: &str = "tray-export";
const ITEM_OPEN: &str = "tray-open";
const ITEM_QUIT: &str = "tray-quit";

#[derive(Clone, Copy, Debug, Default, PartialEq, Eq, Deserialize, Serialize)]
#[serde(rename_all = "camelCase")]
pub enum TrayMode {
    // No icon.
    Off,
    // An icon while the app is open; closing the window quits, as before.
    #[default]
    On,
    // Closing the window hides it; the app keeps running in the tray until
    // Quit.
    KeepRunning,
}

// What the page reports. `available` is false until the settings have loaded
// (or while they cannot be read), and the settings items stay disabled.
#[derive(Clone, Debug, Default, PartialEq, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct TrayStatus {
    pub available: bool,
    pub health_label: String,
    pub enabled: bool,
    pub log_level: String,
}

// A click the page acts on.
#[derive(Clone, Debug, PartialEq, Serialize)]
#[serde(tag = "action", rename_all = "camelCase")]
pub enum TrayAction {
    SetEnabled { enabled: bool },
    SetLogLevel { level: String },
    ExportDebug,
    ShowHealth,
    // The pointer is over the icon: the health may be stale while the window
    // is hidden, so the page reads it again (it limits how often).
    Refresh,
}

struct TrayMenu {
    health: MenuItem<Wry>,
    enabled: CheckMenuItem<Wry>,
    log_level: Submenu<Wry>,
    log_info: CheckMenuItem<Wry>,
    log_debug: CheckMenuItem<Wry>,
    export: MenuItem<Wry>,
    icon: Image<'static>,
    icon_disabled: Image<'static>,
}

#[derive(Default)]
struct TrayInner {
    mode: TrayMode,
    status: TrayStatus,
    tray: Option<TrayIcon<Wry>>,
    menu: Option<TrayMenu>,
}

#[derive(Default)]
pub struct TrayState(Mutex<TrayInner>);

pub fn health_text(status: &TrayStatus) -> String {
    if !status.available {
        return "Health: settings not loaded".to_string();
    }
    format!("Health: {}", status.health_label)
}

pub fn log_level_text(status: &TrayStatus) -> String {
    if !status.available {
        return "Log level".to_string();
    }
    match status.log_level.as_str() {
        "debug" => "Log level: Debug".to_string(),
        _ => "Log level: Info".to_string(),
    }
}

pub fn tooltip_text(status: &TrayStatus) -> String {
    if !status.available {
        "VectorXR".to_string()
    } else if !status.enabled {
        "VectorXR: disabled".to_string()
    } else {
        format!("VectorXR: {}", status.health_label)
    }
}

// The icon in grey, for while VectorXR is disabled: luminance, alpha kept.
pub fn greyscale(rgba: &[u8]) -> Vec<u8> {
    rgba.chunks_exact(4)
        .flat_map(|pixel| {
            let luma = (u32::from(pixel[0]) * 299 + u32::from(pixel[1]) * 587 + u32::from(pixel[2]) * 114) / 1000;
            // Lighter than plain luminance, so the grey icon reads as off
            // rather than as a dark shape on a dark taskbar.
            let grey = (luma / 2 + 96).min(255) as u8;
            [grey, grey, grey, pixel[3]]
        })
        .collect()
}

pub fn show_main_window(app: &AppHandle) {
    if let Some(window) = app.get_webview_window(MAIN_WINDOW) {
        let _ = window.unminimize();
        let _ = window.show();
        let _ = window.set_focus();
    }
}

fn emit(app: &AppHandle, action: TrayAction) {
    let _ = app.emit_to(MAIN_WINDOW, ACTION_EVENT, action);
}

// Shows the reported status: a click only asks the page for a change, and the
// menu follows once the page saves it and reports again.
fn apply_status(menu: &TrayMenu, tray: &TrayIcon<Wry>, status: &TrayStatus) {
    let _ = menu.health.set_text(health_text(status));
    let _ = menu.enabled.set_enabled(status.available);
    let _ = menu.enabled.set_checked(status.available && status.enabled);
    let _ = menu.log_level.set_text(log_level_text(status));
    let _ = menu.log_level.set_enabled(status.available);
    let _ = menu.log_info.set_checked(status.available && status.log_level != "debug");
    let _ = menu.log_debug.set_checked(status.available && status.log_level == "debug");
    let _ = menu.export.set_enabled(status.available);
    let _ = tray.set_tooltip(Some(tooltip_text(status)));
    let icon = if status.available && !status.enabled { &menu.icon_disabled } else { &menu.icon };
    let _ = tray.set_icon(Some(icon.clone()));
}

fn on_menu_event(app: &AppHandle, event: MenuEvent) {
    let state = app.state::<TrayState>();
    let inner = state.0.lock().unwrap_or_else(|poisoned| poisoned.into_inner());
    let status = inner.status.clone();
    // Windows ticks a check item as it is clicked; put back what is saved
    // until the page reports the change.
    if let (Some(menu), Some(tray)) = (&inner.menu, &inner.tray) {
        apply_status(menu, tray, &status);
    }
    drop(inner);
    match event.id().as_ref() {
        ITEM_HEALTH => {
            show_main_window(app);
            emit(app, TrayAction::ShowHealth);
        }
        ITEM_ENABLED if status.available => emit(app, TrayAction::SetEnabled { enabled: !status.enabled }),
        ITEM_LOG_INFO if status.available => emit(app, TrayAction::SetLogLevel { level: "info".to_string() }),
        ITEM_LOG_DEBUG if status.available => emit(app, TrayAction::SetLogLevel { level: "debug".to_string() }),
        ITEM_EXPORT if status.available => {
            // Saving needs a click on the page (the save picker requires a
            // user gesture), so the export opens there.
            show_main_window(app);
            emit(app, TrayAction::ExportDebug);
        }
        ITEM_OPEN => show_main_window(app),
        ITEM_QUIT => app.exit(0),
        _ => {}
    }
}

fn on_icon_event(tray: &TrayIcon<Wry>, event: TrayIconEvent) {
    match event {
        TrayIconEvent::Click { button: MouseButton::Left, button_state: MouseButtonState::Up, .. } => {
            show_main_window(tray.app_handle());
        }
        TrayIconEvent::Enter { .. } => emit(tray.app_handle(), TrayAction::Refresh),
        _ => {}
    }
}

fn build(app: &AppHandle, status: &TrayStatus) -> tauri::Result<(TrayIcon<Wry>, TrayMenu)> {
    let health = MenuItem::with_id(app, ITEM_HEALTH, health_text(status), true, None::<&str>)?;
    let enabled = CheckMenuItem::with_id(app, ITEM_ENABLED, "VectorXR enabled", false, false, None::<&str>)?;
    let log_info = CheckMenuItem::with_id(app, ITEM_LOG_INFO, "Info", true, false, None::<&str>)?;
    let log_debug = CheckMenuItem::with_id(app, ITEM_LOG_DEBUG, "Debug", true, false, None::<&str>)?;
    let log_level = Submenu::with_items(app, log_level_text(status), false, &[&log_info, &log_debug])?;
    let export = MenuItem::with_id(app, ITEM_EXPORT, "Export Debug...", false, None::<&str>)?;
    let open = MenuItem::with_id(app, ITEM_OPEN, "Open VectorXR", true, None::<&str>)?;
    let quit = MenuItem::with_id(app, ITEM_QUIT, "Quit VectorXR", true, None::<&str>)?;
    let menu = Menu::with_items(
        app,
        &[
            &health,
            &PredefinedMenuItem::separator(app)?,
            &enabled,
            &log_level,
            &export,
            &PredefinedMenuItem::separator(app)?,
            &open,
            &quit,
        ],
    )?;
    let icon = app
        .default_window_icon()
        .cloned()
        .map(|icon| icon.to_owned())
        .ok_or_else(|| tauri::Error::AssetNotFound("window icon".to_string()))?;
    let icon_disabled = Image::new(&greyscale(icon.rgba()), icon.width(), icon.height()).to_owned();
    let tray = TrayIconBuilder::with_id(TRAY_ID)
        .icon(icon.clone())
        .tooltip(tooltip_text(status))
        .menu(&menu)
        .show_menu_on_left_click(false)
        .on_menu_event(on_menu_event)
        .on_tray_icon_event(on_icon_event)
        .build(app)?;
    let menu = TrayMenu { health, enabled, log_level, log_info, log_debug, export, icon, icon_disabled };
    apply_status(&menu, &tray, status);
    Ok((tray, menu))
}

#[tauri::command]
pub fn configure_tray(app: AppHandle, state: State<'_, TrayState>, mode: TrayMode) -> Result<(), String> {
    let mut inner = state.0.lock().unwrap_or_else(|poisoned| poisoned.into_inner());
    inner.mode = mode;
    if mode == TrayMode::Off {
        inner.menu = None;
        if let Some(tray) = inner.tray.take() {
            let _ = tray.set_visible(false);
        }
        app.remove_tray_by_id(TRAY_ID);
        return Ok(());
    }
    if inner.tray.is_none() {
        let (tray, menu) = build(&app, &inner.status).map_err(|error| error.to_string())?;
        inner.tray = Some(tray);
        inner.menu = Some(menu);
    }
    Ok(())
}

#[tauri::command]
pub fn update_tray_status(state: State<'_, TrayState>, status: TrayStatus) {
    let mut inner = state.0.lock().unwrap_or_else(|poisoned| poisoned.into_inner());
    if inner.status == status && inner.tray.is_some() {
        return;
    }
    inner.status = status;
    if let (Some(menu), Some(tray)) = (&inner.menu, &inner.tray) {
        apply_status(menu, tray, &inner.status);
    }
}

// Closing the main window hides it instead while the tray keeps the app
// running; otherwise the window closes and the app quits as it always has.
pub fn on_window_event(window: &Window, event: &WindowEvent) {
    if window.label() != MAIN_WINDOW {
        return;
    }
    if let WindowEvent::CloseRequested { api, .. } = event {
        let state = window.state::<TrayState>();
        let keep = {
            let inner = state.0.lock().unwrap_or_else(|poisoned| poisoned.into_inner());
            inner.mode == TrayMode::KeepRunning && inner.tray.is_some()
        };
        if keep {
            api.prevent_close();
            let _ = window.hide();
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn status(available: bool, health: &str, enabled: bool, log_level: &str) -> TrayStatus {
        TrayStatus {
            available,
            health_label: health.to_string(),
            enabled,
            log_level: log_level.to_string(),
        }
    }

    #[test]
    fn texts_follow_the_reported_status() {
        let ready = status(true, "Ready", true, "info");
        assert_eq!(health_text(&ready), "Health: Ready");
        assert_eq!(log_level_text(&ready), "Log level: Info");
        assert_eq!(tooltip_text(&ready), "VectorXR: Ready");
        let disabled = status(true, "Needs attention", false, "debug");
        assert_eq!(log_level_text(&disabled), "Log level: Debug");
        assert_eq!(tooltip_text(&disabled), "VectorXR: disabled");
        let unloaded = status(false, "Ready", true, "debug");
        assert_eq!(health_text(&unloaded), "Health: settings not loaded");
        assert_eq!(log_level_text(&unloaded), "Log level");
        assert_eq!(tooltip_text(&unloaded), "VectorXR");
    }

    #[test]
    fn modes_and_actions_use_the_page_names() {
        let modes: Vec<TrayMode> = serde_json::from_str(r#"["off", "on", "keepRunning"]"#).unwrap();
        assert_eq!(modes, vec![TrayMode::Off, TrayMode::On, TrayMode::KeepRunning]);
        assert_eq!(
            serde_json::to_value(TrayAction::SetEnabled { enabled: false }).unwrap(),
            serde_json::json!({ "action": "setEnabled", "enabled": false })
        );
        assert_eq!(
            serde_json::to_value(TrayAction::SetLogLevel { level: "debug".to_string() }).unwrap(),
            serde_json::json!({ "action": "setLogLevel", "level": "debug" })
        );
        assert_eq!(serde_json::to_value(TrayAction::ExportDebug).unwrap(), serde_json::json!({ "action": "exportDebug" }));
        let reported: TrayStatus = serde_json::from_value(serde_json::json!({
            "available": true, "healthLabel": "Ready", "enabled": true, "logLevel": "info"
        }))
        .unwrap();
        assert_eq!(reported, status(true, "Ready", true, "info"));
    }

    #[test]
    fn greyscale_keeps_alpha_and_lifts_dark_pixels() {
        let grey = greyscale(&[0, 0, 0, 0, 255, 255, 255, 255, 255, 0, 0, 128]);
        assert_eq!(grey, vec![96, 96, 96, 0, 223, 223, 223, 255, 134, 134, 134, 128]);
    }
}
