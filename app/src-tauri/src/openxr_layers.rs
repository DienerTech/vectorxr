use serde::{Deserialize, Serialize};
use serde_json::Value;
use std::collections::HashMap;
use std::env;
use std::fs;
use std::io::{BufRead, BufReader, Write};
use std::net::{TcpListener, TcpStream};
use std::path::{Path, PathBuf};
use std::process::{Command, Stdio};
use std::sync::{Mutex, OnceLock};
use std::time::{Duration, Instant, SystemTime, UNIX_EPOCH};

#[cfg(windows)]
use std::os::windows::process::CommandExt;
#[cfg(windows)]
use windows::core::PWSTR;
#[cfg(windows)]
use windows::Win32::Foundation::{
    CloseHandle, HANDLE, ERROR_ACCESS_DENIED, ERROR_FILE_NOT_FOUND, ERROR_MORE_DATA,
    ERROR_NO_MORE_ITEMS, WIN32_ERROR,
};
#[cfg(windows)]
use windows::Win32::Security::{GetTokenInformation, TokenElevation, TOKEN_ELEVATION, TOKEN_QUERY};
#[cfg(windows)]
use windows::Win32::System::Threading::{GetCurrentProcess, OpenProcessToken};
#[cfg(windows)]
use windows::Win32::System::Registry::{
    RegCloseKey, RegDeleteValueW, RegEnumValueW, RegFlushKey, RegOpenKeyExW, RegSetValueExW, HKEY,
    HKEY_CURRENT_USER, HKEY_LOCAL_MACHINE, KEY_READ, KEY_SET_VALUE, KEY_WOW64_32KEY,
    KEY_WOW64_64KEY, REG_DWORD, REG_SAM_FLAGS, REG_VALUE_TYPE,
};

#[cfg(windows)]
const CREATE_NO_WINDOW: u32 = 0x08000000;

#[derive(Debug, Clone, Copy, Deserialize, Serialize, PartialEq, Eq)]
#[serde(rename_all = "camelCase")]
pub enum MoveDirection {
    Up,
    Down,
}

#[derive(Debug, Clone, Serialize)]
#[serde(rename_all = "camelCase")]
pub struct OpenXrLayerSnapshot {
    process_elevated: bool,
    slices: Vec<OpenXrLayerRegistrySlice>,
}

#[derive(Debug, Clone, Serialize)]
#[serde(rename_all = "camelCase")]
pub struct OpenXrLayerRegistrySlice {
    id: String,
    label: String,
    registry_path: String,
    recommended: bool,
    uncommon: bool,
    requires_elevation_for_writes: bool,
    layers: Vec<OpenXrLayerEntry>,
}

#[derive(Debug, Clone, Serialize)]
#[serde(rename_all = "camelCase")]
pub struct OpenXrLayerEntry {
    slice: String,
    manifest_path: String,
    layer_name: String,
    description: String,
    library_path: Option<String>,
    enabled: bool,
    order: usize,
    signature_status: String,
    signature_status_description: String,
    signature_signer_subject: Option<String>,
    signature_signer_issuer: Option<String>,
    signature_signer_thumbprint: Option<String>,
    signature_signer_not_before: Option<String>,
    signature_signer_not_after: Option<String>,
    is_vector_xr: bool,
    manifest_exists: bool,
    library_exists: bool,
    error: Option<String>,
}

#[derive(Debug, Deserialize, Serialize)]
#[serde(rename_all = "camelCase")]
struct ElevatedOpenXrRequest {
    operation: ElevatedOpenXrOperation,
}

#[derive(Debug, Deserialize, Serialize)]
#[serde(rename_all = "camelCase", tag = "type")]
enum ElevatedOpenXrOperation {
    Ping,
    SetValue {
        slice: String,
        manifest_path: String,
        value: i64,
    },
    // The helper reads the key itself and moves the one value, so the new
    // order is computed from what is in the registry at that moment rather
    // than from a list this process read earlier.
    MoveValue {
        slice: String,
        manifest_path: String,
        direction: MoveDirection,
    },
    DeleteValue {
        slice: String,
        manifest_path: String,
    },
}

#[derive(Debug, Deserialize, Serialize)]
#[serde(rename_all = "camelCase")]
struct ElevatedOpenXrResponse {
    ok: bool,
    error: Option<String>,
}

struct ElevatedOpenXrHelper {
    reader: BufReader<TcpStream>,
    writer: TcpStream,
}

struct ElevatedRequestError {
    message: String,
    kind: ElevatedRequestErrorKind,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
enum ElevatedRequestErrorKind {
    // The request failed, but the connection is still in step.
    Failed,
    // The connection broke or the helper stopped; a new helper can retry.
    Disconnected,
    // The helper did not answer in time and may still make the change.
    TimedOut,
}

#[derive(Debug, Clone, PartialEq, Eq)]
struct RegistryValue {
    name: String,
    // The raw type and data, so a reorder writes every value back exactly as
    // it was, whatever its type.
    value_type: u32,
    data: Vec<u8>,
}

impl RegistryValue {
    // The OpenXR loader reads each value into a DWORD and loads the layer when
    // it reads 0. It asks for no type, so only the size and bytes matter.
    fn enabled(&self) -> bool {
        self.data.len() == 4 && self.data.iter().all(|byte| *byte == 0)
    }

    // A value larger than a DWORD ends the loader's enumeration of the key
    // (ERROR_MORE_DATA), so neither it nor any value after it is loaded.
    fn stops_loader(&self) -> bool {
        self.data.len() > 4
    }
}

#[derive(Debug, Clone, Copy)]
enum RegistryHive {
    LocalMachine,
    CurrentUser,
}

#[derive(Debug, Clone, Copy)]
enum RegistryView {
    Registry64,
    Registry32,
}

#[derive(Debug, Clone, Copy)]
struct SliceDefinition {
    id: &'static str,
    label: &'static str,
    hive: RegistryHive,
    view: RegistryView,
    key_path: &'static str,
    recommended: bool,
    uncommon: bool,
}

const IMPLICIT_KEY: &str = r"Software\Khronos\OpenXR\1\ApiLayers\Implicit";
const ELEVATED_HELPER_ARG: &str = "--vectorxr-openxr-layer-helper";
const ELEVATED_HELPER_TIMEOUT: Duration = Duration::from_secs(120);
const ELEVATED_HELPER_RESPONSE_TIMEOUT: Duration = Duration::from_secs(30);
static ELEVATED_HELPER: OnceLock<Mutex<Option<ElevatedOpenXrHelper>>> = OnceLock::new();
static SIGNATURE_CACHE: OnceLock<Mutex<HashMap<String, SignatureCacheEntry>>> = OnceLock::new();
// HKCU\Software is shared by 64-bit and 32-bit processes (it is not
// redirected under WOW6432Node), so per-user registrations are one slice that
// both kinds of OpenXR app read. Listing a 32-bit per-user view showed and
// counted the same values twice.
const SLICES: [SliceDefinition; 3] = [
    SliceDefinition {
        id: "hklm64",
        label: "Machine-wide 64-bit",
        hive: RegistryHive::LocalMachine,
        view: RegistryView::Registry64,
        key_path: IMPLICIT_KEY,
        recommended: true,
        uncommon: false,
    },
    SliceDefinition {
        id: "hkcu64",
        label: "Per-user",
        hive: RegistryHive::CurrentUser,
        view: RegistryView::Registry64,
        key_path: IMPLICIT_KEY,
        recommended: false,
        uncommon: true,
    },
    SliceDefinition {
        id: "hklm32",
        label: "Machine-wide 32-bit",
        hive: RegistryHive::LocalMachine,
        view: RegistryView::Registry32,
        key_path: IMPLICIT_KEY,
        recommended: false,
        uncommon: true,
    },
];

pub fn load_openxr_layers() -> Result<OpenXrLayerSnapshot, String> {
    load_openxr_layers_with_signatures(false)
}

pub fn load_openxr_layers_with_signatures(
    include_signatures: bool,
) -> Result<OpenXrLayerSnapshot, String> {
    let slices = SLICES
        .iter()
        .map(|definition| read_slice(definition, include_signatures))
        .collect::<Result<Vec<_>, _>>()?;

    Ok(OpenXrLayerSnapshot {
        process_elevated: process_is_elevated(),
        slices,
    })
}

// Check the process token, not membership in Administrators: UAC can give
// an administrator a non-elevated token. A failed probe keeps controls locked.
#[cfg(windows)]
fn process_is_elevated() -> bool {
    unsafe {
        let mut token = HANDLE::default();
        if OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &mut token).is_err() {
            return false;
        }
        let mut elevation = TOKEN_ELEVATION::default();
        let mut returned_size = 0;
        let result = GetTokenInformation(
            token,
            TokenElevation,
            Some((&mut elevation as *mut TOKEN_ELEVATION).cast()),
            std::mem::size_of::<TOKEN_ELEVATION>() as u32,
            &mut returned_size,
        );
        let _ = CloseHandle(token);
        result.is_ok() && elevation.TokenIsElevated != 0
    }
}

#[cfg(not(windows))]
fn process_is_elevated() -> bool {
    false
}

pub fn ensure_openxr_layer_elevation() -> Result<(), String> {
    run_elevated_registry_operation(ElevatedOpenXrOperation::Ping)
}

pub fn set_openxr_layer_enabled(
    slice: String,
    manifest_path: String,
    enabled: bool,
) -> Result<OpenXrLayerSnapshot, String> {
    let definition = find_slice(&slice)?;
    let values = read_registry_values(definition)?;

    if !values
        .iter()
        .any(|value| value.name.eq_ignore_ascii_case(&manifest_path))
    {
        return Err("Layer manifest is no longer registered in that OpenXR slice".into());
    }

    if enabled {
        let snapshot = load_openxr_layers()?;
        if let Some((other_slice, other)) =
            conflicting_vector_xr_registration(&snapshot.slices, &slice, &manifest_path)
        {
            return Err(format!(
                "Another VectorXR registration is already enabled ({}: {}). OpenXR apps crash when two copies of VectorXR load, so disable that one first.",
                other_slice.label, other.manifest_path
            ));
        }
    }

    write_registry_value(definition, &manifest_path, if enabled { 0 } else { 1 })?;
    load_openxr_layers()
}

// An enabled VectorXR registration other than the one about to be enabled.
// A dev build, an old install or a per-user copy can each register VectorXR,
// and the loader would load every enabled one into the same process. One
// whose manifest is gone cannot load, so it does not block.
fn conflicting_vector_xr_registration<'a>(
    slices: &'a [OpenXrLayerRegistrySlice],
    slice: &str,
    manifest_path: &str,
) -> Option<(&'a OpenXrLayerRegistrySlice, &'a OpenXrLayerEntry)> {
    let is_target = |candidate: &OpenXrLayerEntry| {
        candidate.slice == slice && candidate.manifest_path.eq_ignore_ascii_case(manifest_path)
    };
    let target_is_vector_xr = slices
        .iter()
        .flat_map(|candidate| candidate.layers.iter())
        .any(|candidate| is_target(candidate) && candidate.is_vector_xr);
    if !target_is_vector_xr {
        return None;
    }

    slices.iter().find_map(|candidate_slice| {
        candidate_slice
            .layers
            .iter()
            .find(|candidate| {
                candidate.is_vector_xr
                    && candidate.enabled
                    && candidate.manifest_exists
                    && !is_target(candidate)
            })
            .map(|candidate| (candidate_slice, candidate))
    })
}

pub fn move_openxr_layer(
    slice: String,
    manifest_path: String,
    direction: MoveDirection,
) -> Result<OpenXrLayerSnapshot, String> {
    let definition = find_slice(&slice)?;
    let values = read_registry_values(definition)?;
    if !values
        .iter()
        .any(|value| value.name.eq_ignore_ascii_case(&manifest_path))
    {
        return Err("Layer manifest is no longer registered in that OpenXR slice".into());
    }

    move_registry_value(definition, &manifest_path, direction)?;
    load_openxr_layers()
}

// Registry values enumerate in creation order, and that order is the loader's
// layer order. Moving one value changes the position of only the values from
// the first one that moves onward, so only that tail is rewritten. Returns the
// tail's start and its new order, or None when the value is already at the end
// it is moving toward.
fn plan_registry_move(
    existing: &[RegistryValue],
    manifest_path: &str,
    direction: MoveDirection,
) -> Result<Option<(usize, Vec<RegistryValue>)>, String> {
    let Some(index) = existing
        .iter()
        .position(|value| value.name.eq_ignore_ascii_case(manifest_path))
    else {
        return Err("Layer manifest is no longer registered in that OpenXR slice".into());
    };
    let target = match direction {
        MoveDirection::Up if index > 0 => index - 1,
        MoveDirection::Down if index + 1 < existing.len() => index + 1,
        _ => return Ok(None),
    };

    let first = index.min(target);
    let mut tail = existing[first..].to_vec();
    tail.swap(index - first, target - first);
    Ok(Some((first, tail)))
}

trait RegistryWriter {
    // Deleting a value that is already gone succeeds.
    fn delete(&mut self, name: &str) -> Result<(), String>;
    fn set(&mut self, value: &RegistryValue) -> Result<(), String>;
}

// Replaces `old_tail` with `new_tail`. If any step fails, the old values are
// written back in their old order, so a failed move never drops a layer (another
// vendor's included) from the registry.
fn rewrite_registry_tail(
    writer: &mut impl RegistryWriter,
    old_tail: &[RegistryValue],
    new_tail: &[RegistryValue],
) -> Result<(), String> {
    fn replace(
        writer: &mut impl RegistryWriter,
        from: &[RegistryValue],
        to: &[RegistryValue],
    ) -> Result<(), String> {
        for value in from {
            writer.delete(&value.name)?;
        }
        for value in to {
            writer.set(value)?;
        }
        Ok(())
    }

    let Err(error) = replace(writer, old_tail, new_tail) else {
        return Ok(());
    };
    match replace(writer, old_tail, old_tail) {
        Ok(()) => Err(format!("{error}. The previous layer order was kept.")),
        Err(restore_error) => Err(format!(
            "{error}. Restoring the previous layer order also failed: {restore_error}"
        )),
    }
}

// Notes for values the OpenXR loader cannot read as a DWORD. The loader skips
// a value of another size, and stops reading the key at a larger one.
fn registry_value_notes(values: &[RegistryValue]) -> Vec<Option<String>> {
    let mut stopped_at: Option<String> = None;
    values
        .iter()
        .map(|value| {
            if let Some(name) = &stopped_at {
                return Some(format!(
                    "The OpenXR loader stops reading this registry key at {name}, which is not a DWORD value, so this layer is not loaded."
                ));
            }
            if value.stops_loader() {
                stopped_at = Some(display_name_from_path(&value.name));
                return Some("This registry value is not a DWORD, so the OpenXR loader stops reading the key here and loads neither this layer nor any listed below it. Disable or enable it to rewrite it as a DWORD.".into());
            }
            (value.data.len() != 4).then(|| {
                "This registry value is not a DWORD, so the OpenXR loader does not load this layer. Disable or enable it to rewrite it as a DWORD.".into()
            })
        })
        .collect()
}

pub fn delete_openxr_layer(
    slice: String,
    manifest_path: String,
) -> Result<OpenXrLayerSnapshot, String> {
    let definition = find_slice(&slice)?;
    let values = read_registry_values(definition)?;

    if !values
        .iter()
        .any(|value| value.name.eq_ignore_ascii_case(&manifest_path))
    {
        return Err("Layer manifest is no longer registered in that OpenXR slice".into());
    }

    delete_registry_value(definition, &manifest_path)?;
    load_openxr_layers()
}

fn read_slice(
    definition: &SliceDefinition,
    include_signatures: bool,
) -> Result<OpenXrLayerRegistrySlice, String> {
    let registry_values = read_registry_values(definition)?;
    let notes = registry_value_notes(&registry_values);
    let layers = registry_values
        .iter()
        .zip(notes)
        .enumerate()
        .map(|(index, (value, note))| {
            let mut entry =
                layer_entry_from_registry_value(definition, value, index + 1, include_signatures);
            if let Some(note) = note {
                entry.error = Some(match entry.error.take() {
                    Some(manifest_error) => format!("{note} {manifest_error}"),
                    None => note,
                });
            }
            entry
        })
        .collect();

    Ok(OpenXrLayerRegistrySlice {
        id: definition.id.into(),
        label: definition.label.into(),
        registry_path: registry_path_label(definition),
        recommended: definition.recommended,
        uncommon: definition.uncommon,
        requires_elevation_for_writes: matches!(definition.hive, RegistryHive::LocalMachine),
        layers,
    })
}

fn layer_entry_from_registry_value(
    definition: &SliceDefinition,
    value: &RegistryValue,
    order: usize,
    include_signatures: bool,
) -> OpenXrLayerEntry {
    let manifest_path = value.name.clone();
    let enabled = value.enabled();
    let manifest = read_manifest(&manifest_path);
    let layer_name = manifest
        .as_ref()
        .ok()
        .and_then(|manifest| manifest.layer_name.clone())
        .unwrap_or_else(|| display_name_from_path(&manifest_path));
    let description = manifest
        .as_ref()
        .ok()
        .and_then(|manifest| manifest.description.clone())
        .unwrap_or_else(|| "Manifest could not be parsed".into());
    let library_path = manifest
        .as_ref()
        .ok()
        .and_then(|manifest| manifest.library_path.clone());
    let library_exists = library_path
        .as_ref()
        .is_some_and(|path| Path::new(path).exists());
    let signature_info = library_path
        .as_ref()
        .filter(|_| library_exists && include_signatures)
        .map(|path| signature_info(path))
        .unwrap_or_else(SignatureInfo::unknown);
    let is_vector_xr = layer_name.to_ascii_lowercase().contains("vectorxr")
        || manifest_path.to_ascii_lowercase().contains("vectorxr");
    let error = manifest.err();

    OpenXrLayerEntry {
        slice: definition.id.into(),
        manifest_path,
        layer_name,
        description,
        library_path,
        enabled,
        order,
        signature_status: signature_info.status,
        signature_status_description: signature_info.status_description,
        signature_signer_subject: signature_info.signer_subject,
        signature_signer_issuer: signature_info.signer_issuer,
        signature_signer_thumbprint: signature_info.signer_thumbprint,
        signature_signer_not_before: signature_info.signer_not_before,
        signature_signer_not_after: signature_info.signer_not_after,
        is_vector_xr,
        manifest_exists: Path::new(&value.name).exists(),
        library_exists,
        error,
    }
}

#[derive(Debug, Clone)]
struct ManifestInfo {
    layer_name: Option<String>,
    description: Option<String>,
    library_path: Option<String>,
}

fn read_manifest(manifest_path: &str) -> Result<ManifestInfo, String> {
    let path = PathBuf::from(manifest_path);
    let content =
        fs::read_to_string(&path).map_err(|error| format!("Unable to read manifest: {error}"))?;
    let json = serde_json::from_str::<Value>(&content)
        .map_err(|error| format!("Manifest is not valid JSON: {error}"))?;
    let api_layer = json
        .get("api_layer")
        .and_then(Value::as_object)
        .ok_or_else(|| "Manifest does not contain an api_layer object".to_string())?;
    let layer_name = api_layer
        .get("name")
        .and_then(Value::as_str)
        .map(str::to_string);
    let description = api_layer
        .get("description")
        .and_then(Value::as_str)
        .map(str::to_string);
    let library_path = api_layer
        .get("library_path")
        .and_then(Value::as_str)
        .map(|library_path| resolve_library_path(&path, library_path));

    Ok(ManifestInfo {
        layer_name,
        description,
        library_path,
    })
}

fn resolve_library_path(manifest_path: &Path, library_path: &str) -> String {
    let path = PathBuf::from(library_path);
    if path.is_absolute() {
        return path.to_string_lossy().into_owned();
    }

    manifest_path
        .parent()
        .unwrap_or_else(|| Path::new("."))
        .join(path)
        .to_string_lossy()
        .into_owned()
}

fn display_name_from_path(path: &str) -> String {
    Path::new(path)
        .file_stem()
        .and_then(|value| value.to_str())
        .unwrap_or("Unknown OpenXR layer")
        .into()
}

fn find_slice(slice: &str) -> Result<&'static SliceDefinition, String> {
    SLICES
        .iter()
        .find(|definition| definition.id == slice)
        .ok_or_else(|| "Unknown OpenXR registry slice".to_string())
}

fn slice_for_elevated_operation(slice: &str) -> Result<&'static SliceDefinition, String> {
    let definition = find_slice(slice)?;
    if !matches!(definition.hive, RegistryHive::LocalMachine) {
        return Err("Elevated OpenXR helper only accepts machine-wide registry operations".into());
    }

    Ok(definition)
}

fn registry_path_label(definition: &SliceDefinition) -> String {
    let hive = match definition.hive {
        RegistryHive::LocalMachine => "HKLM",
        RegistryHive::CurrentUser => "HKCU",
    };

    // The 32-bit view of HKLM\Software lives at HKLM\Software\WOW6432Node.
    let key_path = match definition.view {
        RegistryView::Registry64 => definition.key_path.to_string(),
        RegistryView::Registry32 => match definition.key_path.split_once('\\') {
            Some((software, rest)) => format!(r"{software}\WOW6432Node\{rest}"),
            None => definition.key_path.to_string(),
        },
    };

    format!(r"{hive}\{key_path}")
}

fn escape_powershell_single_quoted(value: &str) -> String {
    value.replace('\'', "''")
}

fn run_signature_powershell(script: &str) -> Result<String, String> {
    use std::io::Read;
    let mut command = Command::new("powershell.exe");
    command
        .args(["-NoProfile", "-NonInteractive", "-Command", script])
        .stdin(Stdio::null())
        .stdout(Stdio::piped())
        .stderr(Stdio::null());
    #[cfg(windows)]
    command.creation_flags(CREATE_NO_WINDOW);
    let mut child = command.spawn().map_err(|error| error.to_string())?;
    let mut stdout = child.stdout.take().ok_or("Missing signature output")?;
    let reader = std::thread::spawn(move || {
        let mut output = Vec::new();
        let mut chunk = [0u8; 4096];
        while let Ok(count) = stdout.read(&mut chunk) {
            if count == 0 {
                break;
            }
            if output.len() + count <= 65536 {
                output.extend_from_slice(&chunk[..count]);
            }
        }
        output
    });
    let start = Instant::now();
    loop {
        match child.try_wait() {
            Ok(Some(status)) => {
                let output = reader.join().map_err(|_| "Signature reader failed")?;
                return if status.success() {
                    Ok(String::from_utf8_lossy(&output).into_owned())
                } else {
                    Err("Signature check failed".into())
                };
            }
            Ok(None) if start.elapsed() < Duration::from_secs(4) => {
                std::thread::sleep(Duration::from_millis(20))
            }
            _ => {
                let _ = child.kill();
                let _ = child.wait();
                return Err("Signature check timed out".into());
            }
        }
    }
}

#[test]
#[cfg(windows)]
fn signature_process_is_bounded_when_it_hangs() {
    let start = Instant::now();
    let result = run_signature_powershell("Start-Sleep -Seconds 30");
    assert_eq!(result.unwrap_err(), "Signature check timed out");
    assert!(start.elapsed() < Duration::from_secs(8));
}

fn run_powershell(script: &str) -> Result<String, String> {
    let mut command = Command::new("powershell.exe");
    command
        .arg("-NoProfile")
        .arg("-NonInteractive")
        .arg("-ExecutionPolicy")
        .arg("Bypass")
        .arg("-Command")
        .arg(script)
        .stdin(Stdio::null());

    #[cfg(windows)]
    command.creation_flags(CREATE_NO_WINDOW);

    let output = command.output().map_err(|error| error.to_string())?;

    if output.status.success() {
        Ok(String::from_utf8_lossy(&output.stdout).trim().to_string())
    } else {
        let error = String::from_utf8_lossy(&output.stderr).trim().to_string();
        if error.to_ascii_lowercase().contains("access") {
            Err("This OpenXR layer change requires elevation. Restart VectorXR as administrator, then try again.".into())
        } else if error.is_empty() {
            Err("OpenXR registry operation failed".into())
        } else {
            Err(error)
        }
    }
}

fn run_elevated_registry_operation(operation: ElevatedOpenXrOperation) -> Result<(), String> {
    if process_is_elevated() {
        return run_elevated_operation(operation);
    }

    let helper_lock = ELEVATED_HELPER.get_or_init(|| Mutex::new(None));
    let mut helper_guard = helper_lock
        .lock()
        .map_err(|_| "OpenXR elevated helper state is unavailable".to_string())?;

    send_with_elevated_helper(
        &mut helper_guard,
        &ElevatedOpenXrRequest { operation },
        start_elevated_helper,
    )
}

fn send_with_elevated_helper(
    helper_slot: &mut Option<ElevatedOpenXrHelper>,
    request: &ElevatedOpenXrRequest,
    start_helper: impl Fn() -> Result<ElevatedOpenXrHelper, String>,
) -> Result<(), String> {
    let had_helper = helper_slot.is_some();
    if !had_helper {
        *helper_slot = Some(start_helper()?);
    }

    let error = match send_elevated_request(
        helper_slot
            .as_mut()
            .expect("elevated helper was just initialized"),
        request,
    ) {
        Ok(()) => return Ok(()),
        Err(error) => error,
    };
    if error.kind == ElevatedRequestErrorKind::Failed {
        return Err(error.message);
    }

    // The helper may still answer this request, so never send on that
    // connection again: the next request would read this one's response.
    *helper_slot = None;
    if had_helper && error.kind == ElevatedRequestErrorKind::Disconnected {
        // The helper kept from an earlier change has stopped; start a new one.
        return send_with_elevated_helper(helper_slot, request, start_helper);
    }
    Err(error.message)
}

fn start_elevated_helper() -> Result<ElevatedOpenXrHelper, String> {
    let listener = TcpListener::bind("127.0.0.1:0").map_err(|error| error.to_string())?;
    listener
        .set_nonblocking(true)
        .map_err(|error| error.to_string())?;
    let address = listener
        .local_addr()
        .map_err(|error| error.to_string())?
        .to_string();
    let token = helper_token();
    launch_elevated_helper_process(&address, &token)?;
    accept_elevated_helper(&listener, &token)
}

fn accept_elevated_helper(
    listener: &TcpListener,
    token: &str,
) -> Result<ElevatedOpenXrHelper, String> {
    let started = Instant::now();
    while started.elapsed() < ELEVATED_HELPER_TIMEOUT {
        match listener.accept() {
            Ok((stream, _)) => {
                // Windows gives the accepted socket the listener's non-blocking
                // mode, so a read would fail before the helper answered. Wait
                // for each answer instead, but not forever on a stuck helper.
                stream
                    .set_nonblocking(false)
                    .and_then(|_| stream.set_read_timeout(Some(ELEVATED_HELPER_RESPONSE_TIMEOUT)))
                    .and_then(|_| stream.set_write_timeout(Some(ELEVATED_HELPER_RESPONSE_TIMEOUT)))
                    .map_err(|error| error.to_string())?;
                let mut reader = BufReader::new(stream);
                let mut received_token = String::new();
                reader
                    .read_line(&mut received_token)
                    .map_err(|error| error.to_string())?;

                if received_token.trim() != token {
                    return Err("OpenXR elevated helper authentication failed".into());
                }

                let writer = reader
                    .get_ref()
                    .try_clone()
                    .map_err(|error| error.to_string())?;
                return Ok(ElevatedOpenXrHelper { reader, writer });
            }
            Err(error) if error.kind() == std::io::ErrorKind::WouldBlock => {
                std::thread::sleep(Duration::from_millis(50));
            }
            Err(error) => return Err(error.to_string()),
        }
    }

    Err("OpenXR layer change was canceled at the elevation prompt or the elevated helper did not start.".into())
}

fn launch_elevated_helper_process(address: &str, token: &str) -> Result<(), String> {
    let executable = env::current_exe().map_err(|error| error.to_string())?;
    let argument_list = format!(
        "'{}','{}','{}'",
        ELEVATED_HELPER_ARG,
        address.replace('\'', "''"),
        token.replace('\'', "''")
    );
    let script = format!(
        r#"
$process = Start-Process -FilePath '{executable}' -ArgumentList @({argument_list}) -Verb RunAs -WindowStyle Hidden -PassThru
if ($null -eq $process) {{ exit 1 }}
"#,
        executable = escape_powershell_single_quoted(&executable.to_string_lossy()),
        argument_list = argument_list,
    );

    run_powershell(&script).map(|_| ())
}

fn helper_token() -> String {
    let suffix = SystemTime::now()
        .duration_since(UNIX_EPOCH)
        .map(|duration| duration.as_nanos())
        .unwrap_or_default();
    format!("{}-{suffix}", std::process::id())
}

fn send_elevated_request(
    helper: &mut ElevatedOpenXrHelper,
    request: &ElevatedOpenXrRequest,
) -> Result<(), ElevatedRequestError> {
    let payload = serde_json::to_string(request).map_err(|error| ElevatedRequestError {
        message: error.to_string(),
        kind: ElevatedRequestErrorKind::Failed,
    })?;
    helper
        .writer
        .write_all(payload.as_bytes())
        .and_then(|_| helper.writer.write_all(b"\n"))
        .and_then(|_| helper.writer.flush())
        .map_err(helper_connection_error)?;

    let mut response_line = String::new();
    helper
        .reader
        .read_line(&mut response_line)
        .map_err(helper_connection_error)?;

    if response_line.trim().is_empty() {
        return Err(ElevatedRequestError {
            message: "OpenXR elevated helper stopped before the change completed".into(),
            kind: ElevatedRequestErrorKind::Disconnected,
        });
    }

    let response =
        serde_json::from_str::<ElevatedOpenXrResponse>(response_line.trim()).map_err(|error| {
            ElevatedRequestError {
                message: error.to_string(),
                kind: ElevatedRequestErrorKind::Disconnected,
            }
        })?;

    if response.ok {
        Ok(())
    } else {
        Err(ElevatedRequestError {
            message: response
                .error
                .unwrap_or_else(|| "OpenXR elevated registry operation failed".into()),
            kind: ElevatedRequestErrorKind::Failed,
        })
    }
}

fn helper_connection_error(error: std::io::Error) -> ElevatedRequestError {
    if matches!(
        error.kind(),
        std::io::ErrorKind::TimedOut | std::io::ErrorKind::WouldBlock
    ) {
        ElevatedRequestError {
            message: "OpenXR elevated helper did not respond in time".into(),
            kind: ElevatedRequestErrorKind::TimedOut,
        }
    } else {
        ElevatedRequestError {
            message: error.to_string(),
            kind: ElevatedRequestErrorKind::Disconnected,
        }
    }
}

pub fn run_elevated_helper_from_args() -> Option<Result<(), String>> {
    let mut args = env::args().skip(1);
    if args.next().as_deref() != Some(ELEVATED_HELPER_ARG) {
        return None;
    }

    let Some(address) = args.next() else {
        return Some(Err("OpenXR elevated helper address is missing".into()));
    };
    let Some(token) = args.next() else {
        return Some(Err("OpenXR elevated helper token is missing".into()));
    };

    Some(run_elevated_helper(address, token))
}

fn run_elevated_helper(address: String, token: String) -> Result<(), String> {
    let mut stream = TcpStream::connect(address).map_err(|error| error.to_string())?;
    stream
        .write_all(format!("{token}\n").as_bytes())
        .and_then(|_| stream.flush())
        .map_err(|error| error.to_string())?;

    let reader_stream = stream.try_clone().map_err(|error| error.to_string())?;
    let mut reader = BufReader::new(reader_stream);

    loop {
        let mut line = String::new();
        let bytes_read = reader
            .read_line(&mut line)
            .map_err(|error| error.to_string())?;
        if bytes_read == 0 {
            return Ok(());
        }

        let response = match serde_json::from_str::<ElevatedOpenXrRequest>(line.trim()) {
            Ok(request) => match run_elevated_operation(request.operation) {
                Ok(()) => ElevatedOpenXrResponse {
                    ok: true,
                    error: None,
                },
                Err(error) => ElevatedOpenXrResponse {
                    ok: false,
                    error: Some(error),
                },
            },
            Err(error) => ElevatedOpenXrResponse {
                ok: false,
                error: Some(error.to_string()),
            },
        };

        let payload = serde_json::to_string(&response).map_err(|error| error.to_string())?;
        stream
            .write_all(payload.as_bytes())
            .and_then(|_| stream.write_all(b"\n"))
            .and_then(|_| stream.flush())
            .map_err(|error| error.to_string())?;
    }
}

fn run_elevated_operation(operation: ElevatedOpenXrOperation) -> Result<(), String> {
    match operation {
        ElevatedOpenXrOperation::Ping => Ok(()),
        ElevatedOpenXrOperation::SetValue {
            slice,
            manifest_path,
            value,
        } => {
            let definition = slice_for_elevated_operation(&slice)?;
            write_registry_value_native(definition, &manifest_path, value)
        }
        ElevatedOpenXrOperation::MoveValue {
            slice,
            manifest_path,
            direction,
        } => {
            let definition = slice_for_elevated_operation(&slice)?;
            move_registry_value_native(definition, &manifest_path, direction)
        }
        ElevatedOpenXrOperation::DeleteValue {
            slice,
            manifest_path,
        } => {
            let definition = slice_for_elevated_operation(&slice)?;
            delete_registry_value_native(definition, &manifest_path)
        }
    }
}

fn read_registry_values(definition: &SliceDefinition) -> Result<Vec<RegistryValue>, String> {
    read_registry_values_native(definition)
}

#[cfg(windows)]
struct RegistryKeyHandle(HKEY);

#[cfg(windows)]
impl Drop for RegistryKeyHandle {
    fn drop(&mut self) {
        unsafe {
            let _ = RegCloseKey(self.0);
        }
    }
}

#[cfg(windows)]
fn read_registry_values_native(definition: &SliceDefinition) -> Result<Vec<RegistryValue>, String> {
    let key = match open_registry_key(definition, KEY_READ) {
        Ok(key) => key,
        Err(error) if error == ERROR_FILE_NOT_FOUND => return Ok(Vec::new()),
        Err(error) => return Err(registry_error("open OpenXR registry key", error)),
    };

    let mut values = Vec::new();
    let mut index = 0;
    let mut name_buffer = vec![0u16; 32768];
    let name_capacity = name_buffer.len() as u32;

    loop {
        let mut name_len = name_capacity;
        let mut value_type = 0u32;
        let mut data = vec![0u8; 4];
        let mut data_len = data.len() as u32;
        let mut enumerate = |data: &mut Vec<u8>, name_len: &mut u32, data_len: &mut u32| unsafe {
            RegEnumValueW(
                key.0,
                index,
                Some(PWSTR(name_buffer.as_mut_ptr())),
                name_len,
                None,
                Some(&mut value_type),
                Some(data.as_mut_ptr()),
                Some(data_len),
            )
        };
        let mut status = enumerate(&mut data, &mut name_len, &mut data_len);

        // Anything larger than a DWORD (a string, a QWORD) is read again with
        // room for its data, so one odd value no longer fails the whole list.
        if status == ERROR_MORE_DATA {
            data = vec![0u8; data_len as usize];
            name_len = name_capacity;
            data_len = data.len() as u32;
            status = enumerate(&mut data, &mut name_len, &mut data_len);
        }

        if status == ERROR_NO_MORE_ITEMS {
            break;
        }

        if status != WIN32_ERROR(0) {
            return Err(registry_error("enumerate OpenXR registry values", status));
        }

        let name = String::from_utf16_lossy(&name_buffer[..name_len as usize]);
        data.truncate(data_len as usize);
        values.push(RegistryValue {
            name,
            value_type,
            data,
        });
        index += 1;
    }

    Ok(values)
}

#[cfg(not(windows))]
fn read_registry_values_native(
    _definition: &SliceDefinition,
) -> Result<Vec<RegistryValue>, String> {
    Ok(Vec::new())
}

#[cfg(windows)]
fn write_registry_value_native(
    definition: &SliceDefinition,
    manifest_path: &str,
    value: i64,
) -> Result<(), String> {
    let key = open_registry_key(definition, KEY_SET_VALUE)
        .map_err(|error| registry_error("open OpenXR registry key for writing", error))?;
    set_registry_dword(&key, manifest_path, value as u32)?;
    flush_registry_key(&key)
}

#[cfg(not(windows))]
fn write_registry_value_native(
    _definition: &SliceDefinition,
    _manifest_path: &str,
    _value: i64,
) -> Result<(), String> {
    Err("OpenXR registry writes are only supported on Windows".into())
}

#[cfg(windows)]
fn delete_registry_value_native(
    definition: &SliceDefinition,
    manifest_path: &str,
) -> Result<(), String> {
    let key = open_registry_key(definition, KEY_SET_VALUE)
        .map_err(|error| registry_error("open OpenXR registry key for removal", error))?;
    let name = wide_null(manifest_path);
    let status = unsafe { RegDeleteValueW(key.0, PWSTR(name.as_ptr() as *mut u16)) };
    if status != WIN32_ERROR(0) && status != ERROR_FILE_NOT_FOUND {
        return Err(registry_error("remove OpenXR registry value", status));
    }
    flush_registry_key(&key)
}

#[cfg(not(windows))]
fn delete_registry_value_native(
    _definition: &SliceDefinition,
    _manifest_path: &str,
) -> Result<(), String> {
    Err("OpenXR registry writes are only supported on Windows".into())
}

#[cfg(windows)]
struct NativeRegistryWriter<'a>(&'a RegistryKeyHandle);

#[cfg(windows)]
impl RegistryWriter for NativeRegistryWriter<'_> {
    fn delete(&mut self, name: &str) -> Result<(), String> {
        let name = wide_null(name);
        let status = unsafe { RegDeleteValueW(self.0 .0, PWSTR(name.as_ptr() as *mut u16)) };
        if status != WIN32_ERROR(0) && status != ERROR_FILE_NOT_FOUND {
            return Err(registry_error("delete OpenXR registry value", status));
        }
        Ok(())
    }

    fn set(&mut self, value: &RegistryValue) -> Result<(), String> {
        let name = wide_null(&value.name);
        let status = unsafe {
            RegSetValueExW(
                self.0 .0,
                PWSTR(name.as_ptr() as *mut u16),
                None,
                REG_VALUE_TYPE(value.value_type),
                Some(&value.data),
            )
        };
        if status == WIN32_ERROR(0) {
            Ok(())
        } else {
            Err(registry_error("set OpenXR registry value", status))
        }
    }
}

// Reads the key fresh, so the move applies to what is registered now (an
// installer may have added a layer since the app last read it), rewrites
// only the values whose position changes, and flushes once at the end.
#[cfg(windows)]
fn move_registry_value_native(
    definition: &SliceDefinition,
    manifest_path: &str,
    direction: MoveDirection,
) -> Result<(), String> {
    let existing = read_registry_values_native(definition)?;
    let Some((first, new_tail)) = plan_registry_move(&existing, manifest_path, direction)? else {
        return Ok(());
    };
    let key = open_registry_key(definition, KEY_READ | KEY_SET_VALUE)
        .map_err(|error| registry_error("open OpenXR registry key for reordering", error))?;
    let result = rewrite_registry_tail(
        &mut NativeRegistryWriter(&key),
        &existing[first..],
        &new_tail,
    );
    let flushed = flush_registry_key(&key);
    result.and(flushed)
}

#[cfg(not(windows))]
fn move_registry_value_native(
    _definition: &SliceDefinition,
    _manifest_path: &str,
    _direction: MoveDirection,
) -> Result<(), String> {
    Err("OpenXR registry writes are only supported on Windows".into())
}

#[cfg(windows)]
fn open_registry_key(
    definition: &SliceDefinition,
    access: REG_SAM_FLAGS,
) -> Result<RegistryKeyHandle, WIN32_ERROR> {
    let root = match definition.hive {
        RegistryHive::LocalMachine => HKEY_LOCAL_MACHINE,
        RegistryHive::CurrentUser => HKEY_CURRENT_USER,
    };
    let view = match definition.view {
        RegistryView::Registry64 => KEY_WOW64_64KEY,
        RegistryView::Registry32 => KEY_WOW64_32KEY,
    };
    let path = wide_null(definition.key_path);
    let mut key = HKEY(std::ptr::null_mut());
    let status = unsafe {
        RegOpenKeyExW(
            root,
            PWSTR(path.as_ptr() as *mut u16),
            None,
            access | view,
            &mut key,
        )
    };

    if status == WIN32_ERROR(0) {
        Ok(RegistryKeyHandle(key))
    } else {
        Err(status)
    }
}

#[cfg(windows)]
fn set_registry_dword(key: &RegistryKeyHandle, name: &str, value: u32) -> Result<(), String> {
    let name = wide_null(name);
    let bytes = value.to_le_bytes();
    let status = unsafe {
        RegSetValueExW(
            key.0,
            PWSTR(name.as_ptr() as *mut u16),
            None,
            REG_DWORD,
            Some(&bytes),
        )
    };

    if status == WIN32_ERROR(0) {
        Ok(())
    } else {
        Err(registry_error("set OpenXR registry value", status))
    }
}

#[cfg(windows)]
fn flush_registry_key(key: &RegistryKeyHandle) -> Result<(), String> {
    let status = unsafe { RegFlushKey(key.0) };
    if status == WIN32_ERROR(0) {
        Ok(())
    } else {
        Err(registry_error("flush OpenXR registry key", status))
    }
}

#[cfg(windows)]
fn wide_null(value: &str) -> Vec<u16> {
    value.encode_utf16().chain(std::iter::once(0)).collect()
}

#[cfg(windows)]
fn registry_error(action: &str, error: WIN32_ERROR) -> String {
    if error == ERROR_ACCESS_DENIED {
        return "This OpenXR layer change requires elevation.".into();
    }

    format!("Unable to {action}: Windows error {}", error.0)
}

fn write_registry_value(
    definition: &SliceDefinition,
    manifest_path: &str,
    value: i64,
) -> Result<(), String> {
    if matches!(definition.hive, RegistryHive::LocalMachine) {
        run_elevated_registry_operation(ElevatedOpenXrOperation::SetValue {
            slice: definition.id.into(),
            manifest_path: manifest_path.into(),
            value,
        })
    } else {
        write_registry_value_native(definition, manifest_path, value)
    }
}

fn move_registry_value(
    definition: &SliceDefinition,
    manifest_path: &str,
    direction: MoveDirection,
) -> Result<(), String> {
    if matches!(definition.hive, RegistryHive::LocalMachine) {
        run_elevated_registry_operation(ElevatedOpenXrOperation::MoveValue {
            slice: definition.id.into(),
            manifest_path: manifest_path.into(),
            direction,
        })
    } else {
        move_registry_value_native(definition, manifest_path, direction)
    }
}

fn delete_registry_value(definition: &SliceDefinition, manifest_path: &str) -> Result<(), String> {
    if matches!(definition.hive, RegistryHive::LocalMachine) {
        run_elevated_registry_operation(ElevatedOpenXrOperation::DeleteValue {
            slice: definition.id.into(),
            manifest_path: manifest_path.into(),
        })
    } else {
        delete_registry_value_native(definition, manifest_path)
    }
}

#[derive(Debug, Clone, Deserialize)]
#[serde(rename_all = "PascalCase")]
struct RawSignatureInfo {
    status: String,
    status_message: Option<String>,
    signer_subject: Option<String>,
    signer_issuer: Option<String>,
    signer_thumbprint: Option<String>,
    signer_not_before: Option<String>,
    signer_not_after: Option<String>,
}

#[derive(Debug, Clone)]
struct SignatureInfo {
    status: String,
    status_description: String,
    signer_subject: Option<String>,
    signer_issuer: Option<String>,
    signer_thumbprint: Option<String>,
    signer_not_before: Option<String>,
    signer_not_after: Option<String>,
}

#[derive(Debug, Clone)]
struct SignatureCacheEntry {
    file_len: u64,
    modified: Option<SystemTime>,
    info: SignatureInfo,
}

impl SignatureInfo {
    fn unknown() -> Self {
        Self {
            status: "unknown".into(),
            status_description: "Windows signature information is unavailable for this binary."
                .into(),
            signer_subject: None,
            signer_issuer: None,
            signer_thumbprint: None,
            signer_not_before: None,
            signer_not_after: None,
        }
    }
}

fn signature_info(path: &str) -> SignatureInfo {
    let metadata = fs::metadata(path).ok();
    let file_len = metadata
        .as_ref()
        .map(|value| value.len())
        .unwrap_or_default();
    let modified = metadata.and_then(|value| value.modified().ok());
    let cache_key = path.to_ascii_lowercase();
    let cache = SIGNATURE_CACHE.get_or_init(|| Mutex::new(HashMap::new()));

    if let Ok(entries) = cache.lock() {
        if let Some(entry) = entries.get(&cache_key) {
            if entry.file_len == file_len && entry.modified == modified {
                return entry.info.clone();
            }
        }
    }

    let info = query_signature_info(path);
    if let Ok(mut entries) = cache.lock() {
        entries.insert(
            cache_key,
            SignatureCacheEntry {
                file_len,
                modified,
                info: info.clone(),
            },
        );
    }
    info
}

fn query_signature_info(path: &str) -> SignatureInfo {
    let script = format!(
        r#"
$signature = Get-AuthenticodeSignature -LiteralPath '{path}'
[pscustomobject]@{{
  Status = $signature.Status.ToString()
  StatusMessage = $signature.StatusMessage
  SignerSubject = if ($null -ne $signature.SignerCertificate) {{ $signature.SignerCertificate.Subject }} else {{ $null }}
  SignerIssuer = if ($null -ne $signature.SignerCertificate) {{ $signature.SignerCertificate.Issuer }} else {{ $null }}
  SignerThumbprint = if ($null -ne $signature.SignerCertificate) {{ $signature.SignerCertificate.Thumbprint }} else {{ $null }}
  SignerNotBefore = if ($null -ne $signature.SignerCertificate) {{ $signature.SignerCertificate.NotBefore.ToString('yyyy-MM-dd') }} else {{ $null }}
  SignerNotAfter = if ($null -ne $signature.SignerCertificate) {{ $signature.SignerCertificate.NotAfter.ToString('yyyy-MM-dd') }} else {{ $null }}
}} | ConvertTo-Json -Compress
"#,
        path = escape_powershell_single_quoted(path),
    );

    let Ok(output) = run_signature_powershell(&script) else {
        return SignatureInfo::unknown();
    };
    let Ok(raw) = serde_json::from_str::<RawSignatureInfo>(&output) else {
        return SignatureInfo::unknown();
    };

    let status = match raw.status.as_str() {
        "Valid" => "signed",
        "NotSigned" => "unsigned",
        "HashMismatch" | "NotTrusted" | "NotSupportedFileFormat" | "UnknownError" => "invalid",
        _ => "unknown",
    }
    .to_string();

    let status_description = match status.as_str() {
        "signed" => "Windows reports a valid Authenticode signature.".into(),
        "unsigned" => "Windows did not find an Authenticode signature. This does not mean the binary cannot run.".into(),
        "invalid" => raw
            .status_message
            .filter(|message| !message.trim().is_empty())
            .map(|message| strip_urls(&message))
            .unwrap_or_else(|| "Windows reports that this binary's Authenticode signature is invalid.".into()),
        _ => "Windows could not determine this binary's signature status.".into(),
    };

    SignatureInfo {
        status,
        status_description,
        signer_subject: raw.signer_subject,
        signer_issuer: raw.signer_issuer,
        signer_thumbprint: raw.signer_thumbprint,
        signer_not_before: raw.signer_not_before,
        signer_not_after: raw.signer_not_after,
    }
}

fn strip_urls(value: &str) -> String {
    value
        .split_whitespace()
        .filter(|part| !part.starts_with("http://") && !part.starts_with("https://"))
        .collect::<Vec<_>>()
        .join(" ")
}

// The system's active OpenXR runtime, read from the registry so the UI can
// highlight the matching runtime-pacing row without needing a live session.
#[derive(Debug, Clone, Serialize)]
#[serde(rename_all = "camelCase")]
pub struct ActiveRuntimeInfo {
    pub manifest_path: String,
    // Optional: runtime manifests are not required to carry a name. The
    // authoritative name comes from xrGetInstanceProperties (layer sidecar);
    // this is display/matching metadata only.
    pub name: Option<String>,
}

pub fn read_active_runtime() -> Option<ActiveRuntimeInfo> {
    read_active_runtime_native()
}

#[cfg(windows)]
fn read_active_runtime_native() -> Option<ActiveRuntimeInfo> {
    let path = wide_null(r"SOFTWARE\Khronos\OpenXR\1");
    let mut key = HKEY(std::ptr::null_mut());
    let status = unsafe {
        RegOpenKeyExW(
            HKEY_LOCAL_MACHINE,
            PWSTR(path.as_ptr() as *mut u16),
            None,
            KEY_READ | KEY_WOW64_64KEY,
            &mut key,
        )
    };
    if status != WIN32_ERROR(0) {
        return None;
    }
    let key = RegistryKeyHandle(key);

    let mut index = 0;
    loop {
        let mut name_buffer = vec![0u16; 512];
        let mut name_len = name_buffer.len() as u32;
        let mut value_type = 0u32;
        let mut data = vec![0u8; 65536];
        let mut data_len = data.len() as u32;
        let status = unsafe {
            RegEnumValueW(
                key.0,
                index,
                Some(PWSTR(name_buffer.as_mut_ptr())),
                &mut name_len,
                None,
                Some(&mut value_type),
                Some(data.as_mut_ptr()),
                Some(&mut data_len),
            )
        };
        if status != WIN32_ERROR(0) {
            return None;
        }
        let name = String::from_utf16_lossy(&name_buffer[..name_len as usize]);
        if name.eq_ignore_ascii_case("ActiveRuntime") {
            let wide: Vec<u16> = data[..data_len as usize]
                .chunks_exact(2)
                .map(|chunk| u16::from_le_bytes([chunk[0], chunk[1]]))
                .collect();
            let mut manifest_path = String::from_utf16_lossy(&wide);
            while manifest_path.ends_with('\0') {
                manifest_path.pop();
            }
            let name = runtime_name_from_manifest(&manifest_path);
            return Some(ActiveRuntimeInfo {
                manifest_path,
                name,
            });
        }
        index += 1;
    }
}

#[cfg(not(windows))]
fn read_active_runtime_native() -> Option<ActiveRuntimeInfo> {
    None
}

fn runtime_name_from_manifest(manifest_path: &str) -> Option<String> {
    let content = fs::read_to_string(manifest_path).ok()?;
    let value: Value = serde_json::from_str(&content).ok()?;
    value
        .get("runtime")?
        .get("name")?
        .as_str()
        .map(|name| name.to_string())
}

#[cfg(test)]
mod slice_tests {
    use super::*;

    #[test]
    fn slices_name_real_and_distinct_registry_keys() {
        let paths = SLICES
            .iter()
            .map(|definition| (definition.id, registry_path_label(definition)))
            .collect::<Vec<_>>();
        assert_eq!(
            paths,
            vec![
                ("hklm64", r"HKLM\Software\Khronos\OpenXR\1\ApiLayers\Implicit".to_string()),
                ("hkcu64", r"HKCU\Software\Khronos\OpenXR\1\ApiLayers\Implicit".to_string()),
                (
                    "hklm32",
                    r"HKLM\Software\WOW6432Node\Khronos\OpenXR\1\ApiLayers\Implicit".to_string()
                ),
            ]
        );
        // HKCU\Software is not redirected, so a 32-bit per-user view would
        // read the same values as the per-user slice.
        assert!(SLICES.iter().all(|definition| !matches!(
            (definition.hive, definition.view),
            (RegistryHive::CurrentUser, RegistryView::Registry32)
        )));
    }

    fn entry(slice: &str, manifest_path: &str, enabled: bool) -> OpenXrLayerEntry {
        OpenXrLayerEntry {
            slice: slice.into(),
            manifest_path: manifest_path.into(),
            layer_name: display_name_from_path(manifest_path),
            description: String::new(),
            library_path: None,
            enabled,
            order: 1,
            signature_status: "unknown".into(),
            signature_status_description: String::new(),
            signature_signer_subject: None,
            signature_signer_issuer: None,
            signature_signer_thumbprint: None,
            signature_signer_not_before: None,
            signature_signer_not_after: None,
            is_vector_xr: manifest_path.to_ascii_lowercase().contains("vectorxr"),
            manifest_exists: true,
            library_exists: true,
            error: None,
        }
    }

    fn slice(id: &str, layers: Vec<OpenXrLayerEntry>) -> OpenXrLayerRegistrySlice {
        OpenXrLayerRegistrySlice {
            id: id.into(),
            label: id.into(),
            registry_path: String::new(),
            recommended: false,
            uncommon: false,
            requires_elevation_for_writes: false,
            layers,
        }
    }

    #[test]
    fn a_second_vector_xr_registration_cannot_be_enabled() {
        let installed = r"C:\Program Files\VectorXR\vectorxr-layer\XR_APILAYER_DIENERTECH_VECTORXR.json";
        let dev = r"C:\dev\build\layer\Release\XR_APILAYER_DIENERTECH_VECTORXR.json";
        let slices = vec![
            slice(
                "hklm64",
                vec![
                    entry("hklm64", dev, false),
                    entry("hklm64", r"C:\layers\XR_APILAYER_other.json", true),
                    entry("hklm64", installed, true),
                ],
            ),
            slice("hkcu64", vec![entry("hkcu64", dev, false)]),
        ];

        let conflict = conflicting_vector_xr_registration(&slices, "hklm64", dev)
            .map(|(slice, layer)| (slice.id.as_str(), layer.manifest_path.as_str()));
        assert_eq!(conflict, Some(("hklm64", installed)));
        // The same file registered per-user would load a second copy too.
        assert_eq!(
            conflicting_vector_xr_registration(&slices, "hkcu64", dev)
                .map(|(slice, _)| slice.id.as_str()),
            Some("hklm64")
        );
        // Re-enabling the active one, or enabling another vendor's layer, is fine.
        assert!(conflicting_vector_xr_registration(&slices, "hklm64", installed).is_none());
        // An enabled registration whose manifest is gone cannot load.
        let mut removed_install = slices.clone();
        removed_install[0].layers[2].manifest_exists = false;
        assert!(conflicting_vector_xr_registration(&removed_install, "hklm64", dev).is_none());
        assert!(conflicting_vector_xr_registration(
            &slices,
            "hklm64",
            r"C:\layers\XR_APILAYER_other.json"
        )
        .is_none());
    }
}

#[cfg(test)]
mod registry_order_tests {
    use super::*;

    const DWORD: u32 = 4;
    const SZ: u32 = 1;

    fn dword(name: &str, value: u32) -> RegistryValue {
        RegistryValue {
            name: name.into(),
            value_type: DWORD,
            data: value.to_le_bytes().to_vec(),
        }
    }

    // An ordered stand-in for one registry key that can fail one write.
    struct FakeKey {
        values: Vec<RegistryValue>,
        fail_set_of: Option<String>,
    }

    impl RegistryWriter for FakeKey {
        fn delete(&mut self, name: &str) -> Result<(), String> {
            self.values.retain(|value| value.name != name);
            Ok(())
        }

        fn set(&mut self, value: &RegistryValue) -> Result<(), String> {
            if self.fail_set_of.as_deref() == Some(value.name.as_str()) {
                self.fail_set_of = None;
                return Err("Unable to set OpenXR registry value: Windows error 5".into());
            }
            match self.values.iter_mut().find(|existing| existing.name == value.name) {
                Some(existing) => *existing = value.clone(),
                None => self.values.push(value.clone()),
            }
            Ok(())
        }
    }

    fn names(values: &[RegistryValue]) -> Vec<&str> {
        values.iter().map(|value| value.name.as_str()).collect()
    }

    #[test]
    fn moving_a_layer_rewrites_only_the_values_that_change_position() {
        let existing = vec![dword("a", 0), dword("b", 1), dword("c", 0), dword("d", 0)];
        let (first, tail) = plan_registry_move(&existing, "C", MoveDirection::Up)
            .unwrap()
            .unwrap();
        assert_eq!(first, 1);
        assert_eq!(names(&tail), ["c", "b", "d"]);
        assert!(plan_registry_move(&existing, "a", MoveDirection::Up).unwrap().is_none());
        assert!(plan_registry_move(&existing, "d", MoveDirection::Down).unwrap().is_none());
        assert!(plan_registry_move(&existing, "gone", MoveDirection::Up).is_err());

        let mut key = FakeKey {
            values: existing.clone(),
            fail_set_of: None,
        };
        rewrite_registry_tail(&mut key, &existing[first..], &tail).unwrap();
        assert_eq!(names(&key.values), ["a", "c", "b", "d"]);
        assert_eq!(key.values[2], dword("b", 1));
    }

    #[test]
    fn a_failed_move_puts_every_layer_back_in_its_old_order() {
        let existing = vec![
            dword("a", 0),
            dword("b", 0),
            RegistryValue {
                name: "odd".into(),
                value_type: SZ,
                data: b"0\0".to_vec(),
            },
            dword("d", 1),
        ];
        let (first, tail) = plan_registry_move(&existing, "b", MoveDirection::Down)
            .unwrap()
            .unwrap();
        let mut key = FakeKey {
            values: existing.clone(),
            fail_set_of: Some("b".into()),
        };
        let error = rewrite_registry_tail(&mut key, &existing[first..], &tail).unwrap_err();
        assert!(error.contains("previous layer order was kept"), "{error}");
        assert_eq!(key.values, existing);
    }

    #[test]
    fn values_the_loader_cannot_read_are_flagged_not_failed() {
        let values = vec![
            dword("a", 0),
            RegistryValue {
                name: "short".into(),
                value_type: SZ,
                data: b"\0\0".to_vec(),
            },
            RegistryValue {
                name: r"C:\layers\long.json".into(),
                value_type: SZ,
                data: "enabled".encode_utf16().flat_map(u16::to_le_bytes).collect(),
            },
            dword("d", 0),
        ];
        assert!(values[0].enabled());
        assert!(!values[1].enabled() && !values[2].enabled());
        let notes = registry_value_notes(&values);
        assert!(notes[0].is_none());
        assert!(notes[1].as_deref().unwrap().contains("does not load this layer"));
        assert!(notes[2].as_deref().unwrap().contains("stops reading the key here"));
        assert!(notes[3].as_deref().unwrap().contains("at long"));
    }
}

#[cfg(test)]
mod elevated_helper_tests {
    use super::*;
    use std::sync::atomic::{AtomicUsize, Ordering};
    use std::sync::Arc;

    // Stands in for the elevated process: answers one request per delay, then
    // disconnects. A DeleteValue is answered with an error naming its manifest,
    // so a test can tell which request a response belongs to.
    fn start_fake_helper(
        delays: Vec<Duration>,
        runs: Arc<AtomicUsize>,
    ) -> Result<ElevatedOpenXrHelper, String> {
        let listener = TcpListener::bind("127.0.0.1:0").map_err(|error| error.to_string())?;
        listener
            .set_nonblocking(true)
            .map_err(|error| error.to_string())?;
        let address = listener.local_addr().map_err(|error| error.to_string())?;
        std::thread::spawn(move || {
            let mut stream = TcpStream::connect(address).unwrap();
            stream.write_all(b"fake-token\n").unwrap();
            let mut reader = BufReader::new(stream.try_clone().unwrap());
            for delay in delays {
                let mut line = String::new();
                if reader.read_line(&mut line).unwrap_or(0) == 0 {
                    return;
                }
                std::thread::sleep(delay);
                runs.fetch_add(1, Ordering::SeqCst);
                let error = match serde_json::from_str::<ElevatedOpenXrRequest>(line.trim()) {
                    Ok(ElevatedOpenXrRequest {
                        operation: ElevatedOpenXrOperation::DeleteValue { manifest_path, .. },
                    }) => Some(manifest_path),
                    _ => None,
                };
                let response = ElevatedOpenXrResponse {
                    ok: error.is_none(),
                    error,
                };
                let payload = serde_json::to_string(&response).unwrap();
                if stream.write_all(format!("{payload}\n").as_bytes()).is_err() {
                    return;
                }
            }
        });
        accept_elevated_helper(&listener, "fake-token")
    }

    fn ping() -> ElevatedOpenXrRequest {
        ElevatedOpenXrRequest {
            operation: ElevatedOpenXrOperation::Ping,
        }
    }

    fn delete(manifest_path: &str) -> ElevatedOpenXrRequest {
        ElevatedOpenXrRequest {
            operation: ElevatedOpenXrOperation::DeleteValue {
                slice: "hklm64".into(),
                manifest_path: manifest_path.into(),
            },
        }
    }

    // The accepted socket used to keep the listener's non-blocking mode, so the
    // first read failed before the helper answered, the request was sent again
    // and every later change read the previous change's response.
    #[test]
    fn each_request_reads_its_own_response() {
        let runs = Arc::new(AtomicUsize::new(0));
        let starts = AtomicUsize::new(0);
        let start = || {
            starts.fetch_add(1, Ordering::SeqCst);
            start_fake_helper(vec![Duration::from_millis(20); 4], runs.clone())
        };
        let mut helper = None;

        assert_eq!(
            send_with_elevated_helper(&mut helper, &ping(), &start),
            Ok(())
        );
        for index in 0..3 {
            let manifest_path = format!("layer-{index}.json");
            assert_eq!(
                send_with_elevated_helper(&mut helper, &delete(&manifest_path), &start),
                Err(manifest_path)
            );
        }
        assert_eq!(runs.load(Ordering::SeqCst), 4);
        assert_eq!(starts.load(Ordering::SeqCst), 1);
        assert!(helper.is_some());
    }

    #[test]
    fn stopped_helper_is_replaced_once() {
        let runs = Arc::new(AtomicUsize::new(0));
        let starts = AtomicUsize::new(0);
        let start = || {
            starts.fetch_add(1, Ordering::SeqCst);
            start_fake_helper(vec![Duration::ZERO], runs.clone())
        };
        let mut helper = None;

        assert_eq!(
            send_with_elevated_helper(&mut helper, &ping(), &start),
            Ok(())
        );
        assert_eq!(
            send_with_elevated_helper(&mut helper, &delete("layer.json"), &start),
            Err("layer.json".to_string())
        );
        assert_eq!(runs.load(Ordering::SeqCst), 2);
        assert_eq!(starts.load(Ordering::SeqCst), 2);
    }

    #[test]
    fn timed_out_helper_is_dropped_without_sending_again() {
        let runs = Arc::new(AtomicUsize::new(0));
        let starts = AtomicUsize::new(0);
        let start = || -> Result<ElevatedOpenXrHelper, String> {
            // The first helper answers its second request too late.
            let delays = if starts.fetch_add(1, Ordering::SeqCst) == 0 {
                vec![Duration::ZERO, Duration::from_millis(500)]
            } else {
                vec![Duration::ZERO]
            };
            let helper = start_fake_helper(delays, runs.clone())?;
            helper
                .reader
                .get_ref()
                .set_read_timeout(Some(Duration::from_millis(100)))
                .map_err(|error| error.to_string())?;
            Ok(helper)
        };
        let mut helper = None;

        assert_eq!(
            send_with_elevated_helper(&mut helper, &ping(), &start),
            Ok(())
        );
        assert_eq!(
            send_with_elevated_helper(&mut helper, &delete("late.json"), &start),
            Err("OpenXR elevated helper did not respond in time".to_string())
        );
        assert!(helper.is_none());
        assert_eq!(starts.load(Ordering::SeqCst), 1);

        assert_eq!(
            send_with_elevated_helper(&mut helper, &delete("layer.json"), &start),
            Err("layer.json".to_string())
        );
        assert_eq!(starts.load(Ordering::SeqCst), 2);
    }
}
