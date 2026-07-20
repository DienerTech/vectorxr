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
    ERROR_ACCESS_DENIED, ERROR_FILE_NOT_FOUND, ERROR_MORE_DATA, ERROR_NO_MORE_ITEMS, WIN32_ERROR,
};
#[cfg(windows)]
use windows::Win32::System::Registry::{
    RegCloseKey, RegDeleteValueW, RegEnumValueW, RegFlushKey, RegOpenKeyExW, RegSetValueExW, HKEY,
    HKEY_CURRENT_USER, HKEY_LOCAL_MACHINE, KEY_READ, KEY_SET_VALUE, KEY_WOW64_32KEY,
    KEY_WOW64_64KEY, REG_DWORD, REG_SAM_FLAGS,
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
    RewriteValues {
        slice: String,
        values: Vec<RegistryValue>,
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
    recoverable: bool,
}

#[derive(Debug, Clone, Deserialize, Serialize)]
#[serde(rename_all = "PascalCase")]
struct RegistryValue {
    name: String,
    value: i64,
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
static ELEVATED_HELPER: OnceLock<Mutex<Option<ElevatedOpenXrHelper>>> = OnceLock::new();
static SIGNATURE_CACHE: OnceLock<Mutex<HashMap<String, SignatureCacheEntry>>> = OnceLock::new();
const SLICES: [SliceDefinition; 4] = [
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
        label: "Per-user 64-bit",
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
    SliceDefinition {
        id: "hkcu32",
        label: "Per-user 32-bit",
        hive: RegistryHive::CurrentUser,
        view: RegistryView::Registry32,
        key_path: IMPLICIT_KEY,
        recommended: false,
        uncommon: true,
    },
];

pub fn load_openxr_layers() -> Result<OpenXrLayerSnapshot, String> {
    let slices = SLICES
        .iter()
        .map(read_slice)
        .collect::<Result<Vec<_>, _>>()?;

    Ok(OpenXrLayerSnapshot { slices })
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

    write_registry_value(definition, &manifest_path, if enabled { 0 } else { 1 })?;
    load_openxr_layers()
}

pub fn move_openxr_layer(
    slice: String,
    manifest_path: String,
    direction: MoveDirection,
) -> Result<OpenXrLayerSnapshot, String> {
    let definition = find_slice(&slice)?;
    let mut values = read_registry_values(definition)?;
    let Some(index) = values
        .iter()
        .position(|value| value.name.eq_ignore_ascii_case(&manifest_path))
    else {
        return Err("Layer manifest is no longer registered in that OpenXR slice".into());
    };

    let target_index = match direction {
        MoveDirection::Up if index > 0 => index - 1,
        MoveDirection::Down if index + 1 < values.len() => index + 1,
        _ => return Ok(load_openxr_layers()?),
    };

    values.swap(index, target_index);
    rewrite_registry_values(definition, &values)?;
    load_openxr_layers()
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

fn read_slice(definition: &SliceDefinition) -> Result<OpenXrLayerRegistrySlice, String> {
    let registry_values = read_registry_values(definition)?;
    let layers = registry_values
        .iter()
        .enumerate()
        .map(|(index, value)| layer_entry_from_registry_value(definition, value, index + 1))
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
) -> OpenXrLayerEntry {
    let manifest_path = value.name.clone();
    let enabled = value.value == 0;
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
        .filter(|_| library_exists)
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

    let prefix = match definition.view {
        RegistryView::Registry64 => "",
        RegistryView::Registry32 => r"WOW6432Node\",
    };

    format!(r"{hive}\{prefix}{}", definition.key_path)
}

fn escape_powershell_single_quoted(value: &str) -> String {
    value.replace('\'', "''")
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
    let helper_lock = ELEVATED_HELPER.get_or_init(|| Mutex::new(None));
    let mut helper_guard = helper_lock
        .lock()
        .map_err(|_| "OpenXR elevated helper state is unavailable".to_string())?;

    let had_helper = helper_guard.is_some();
    if !had_helper {
        *helper_guard = Some(start_elevated_helper()?);
    }

    let request = ElevatedOpenXrRequest { operation };

    match send_elevated_request(
        helper_guard
            .as_mut()
            .expect("elevated helper was just initialized"),
        &request,
    ) {
        Ok(()) => Ok(()),
        Err(first_error) if first_error.recoverable && had_helper => {
            *helper_guard = None;
            let mut helper = start_elevated_helper()?;
            let retry = send_elevated_request(&mut helper, &request);
            *helper_guard = Some(helper);
            retry.map_err(|error| error.message)
        }
        Err(first_error) if first_error.recoverable => {
            std::thread::sleep(Duration::from_millis(150));
            send_elevated_request(
                helper_guard
                    .as_mut()
                    .expect("elevated helper should still be available"),
                &request,
            )
            .map_err(|error| error.message)
        }
        Err(error) => Err(error.message),
    }
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

    let started = Instant::now();
    while started.elapsed() < ELEVATED_HELPER_TIMEOUT {
        match listener.accept() {
            Ok((stream, _)) => {
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
    let payload = serde_json::to_string(request).map_err(recoverable_helper_error)?;
    helper
        .writer
        .write_all(payload.as_bytes())
        .and_then(|_| helper.writer.write_all(b"\n"))
        .and_then(|_| helper.writer.flush())
        .map_err(recoverable_helper_error)?;

    let mut response_line = String::new();
    helper
        .reader
        .read_line(&mut response_line)
        .map_err(recoverable_helper_error)?;

    if response_line.trim().is_empty() {
        return Err(ElevatedRequestError {
            message: "OpenXR elevated helper stopped before the change completed".into(),
            recoverable: true,
        });
    }

    let response = serde_json::from_str::<ElevatedOpenXrResponse>(response_line.trim())
        .map_err(recoverable_helper_error)?;

    if response.ok {
        Ok(())
    } else {
        Err(ElevatedRequestError {
            message: response
                .error
                .unwrap_or_else(|| "OpenXR elevated registry operation failed".into()),
            recoverable: false,
        })
    }
}

fn recoverable_helper_error(error: impl ToString) -> ElevatedRequestError {
    ElevatedRequestError {
        message: error.to_string(),
        recoverable: true,
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
        ElevatedOpenXrOperation::RewriteValues { slice, values } => {
            let definition = slice_for_elevated_operation(&slice)?;
            rewrite_registry_values_native(definition, &values)
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

    loop {
        let mut name_buffer = vec![0u16; 32768];
        let mut name_len = name_buffer.len() as u32;
        let mut value_type = 0u32;
        let mut data = [0u8; 4];
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

        if status == ERROR_NO_MORE_ITEMS {
            break;
        }

        if status == ERROR_MORE_DATA {
            return Err("OpenXR registry value was larger than expected".into());
        }

        if status != WIN32_ERROR(0) {
            return Err(registry_error("enumerate OpenXR registry values", status));
        }

        let name = String::from_utf16_lossy(&name_buffer[..name_len as usize]);
        let value = if value_type == REG_DWORD.0 && data_len >= 4 {
            u32::from_le_bytes(data) as i64
        } else {
            0
        };
        values.push(RegistryValue { name, value });
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
fn rewrite_registry_values_native(
    definition: &SliceDefinition,
    values: &[RegistryValue],
) -> Result<(), String> {
    let existing = read_registry_values_native(definition)?;

    {
        let key = open_registry_key(definition, KEY_READ | KEY_SET_VALUE)
            .map_err(|error| registry_error("open OpenXR registry key for reordering", error))?;

        for value in existing {
            let name = wide_null(&value.name);
            let status = unsafe { RegDeleteValueW(key.0, PWSTR(name.as_ptr() as *mut u16)) };
            if status != WIN32_ERROR(0) && status != ERROR_FILE_NOT_FOUND {
                return Err(registry_error("delete OpenXR registry value", status));
            }
        }

        flush_registry_key(&key)?;
    }

    let key = open_registry_key(definition, KEY_SET_VALUE)
        .map_err(|error| registry_error("reopen OpenXR registry key for reordering", error))?;

    for value in values {
        set_registry_dword(&key, &value.name, value.value as u32)?;
    }

    flush_registry_key(&key)
}

#[cfg(not(windows))]
fn rewrite_registry_values_native(
    _definition: &SliceDefinition,
    _values: &[RegistryValue],
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

fn rewrite_registry_values(
    definition: &SliceDefinition,
    values: &[RegistryValue],
) -> Result<(), String> {
    if matches!(definition.hive, RegistryHive::LocalMachine) {
        run_elevated_registry_operation(ElevatedOpenXrOperation::RewriteValues {
            slice: definition.id.into(),
            values: values.to_vec(),
        })
    } else {
        rewrite_registry_values_native(definition, values)
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

    let Ok(output) = run_powershell(&script) else {
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
