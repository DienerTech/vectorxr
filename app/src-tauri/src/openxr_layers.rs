use serde::{Deserialize, Serialize};
use serde_json::Value;
use std::collections::HashMap;
use std::env;
use std::fs;
use std::path::{Path, PathBuf};
use std::process::{Command, Stdio};
use std::time::{SystemTime, UNIX_EPOCH};

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
    is_vector_xr: bool,
    manifest_exists: bool,
    library_exists: bool,
    error: Option<String>,
}

#[derive(Debug, Clone, Deserialize)]
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

fn powershell_registry_hive(hive: RegistryHive) -> &'static str {
    match hive {
        RegistryHive::LocalMachine => "LocalMachine",
        RegistryHive::CurrentUser => "CurrentUser",
    }
}

fn powershell_registry_view(view: RegistryView) -> &'static str {
    match view {
        RegistryView::Registry64 => "Registry64",
        RegistryView::Registry32 => "Registry32",
    }
}

fn escape_powershell_single_quoted(value: &str) -> String {
    value.replace('\'', "''")
}

fn run_powershell(script: &str) -> Result<String, String> {
    let output = Command::new("powershell.exe")
        .arg("-NoProfile")
        .arg("-NonInteractive")
        .arg("-ExecutionPolicy")
        .arg("Bypass")
        .arg("-Command")
        .arg(script)
        .stdin(Stdio::null())
        .output()
        .map_err(|error| error.to_string())?;

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

fn run_powershell_elevated(script: &str) -> Result<(), String> {
    let suffix = SystemTime::now()
        .duration_since(UNIX_EPOCH)
        .map(|duration| duration.as_millis())
        .unwrap_or_default();
    let script_path = env::temp_dir().join(format!(
        "vectorxr-openxr-layer-write-{}-{suffix}.ps1",
        std::process::id()
    ));
    let result_path = env::temp_dir().join(format!(
        "vectorxr-openxr-layer-write-{}-{suffix}.txt",
        std::process::id()
    ));
    let wrapped_script = format!(
        r#"
$ErrorActionPreference = 'Stop'
try {{
{script}
  Set-Content -LiteralPath '{result_path}' -Value 'OK' -Encoding UTF8
  exit 0
}} catch {{
  Set-Content -LiteralPath '{result_path}' -Value ('ERROR: ' + $_.Exception.Message) -Encoding UTF8
  exit 1
}}
"#,
        script = script,
        result_path = escape_powershell_single_quoted(&result_path.to_string_lossy())
    );

    fs::write(&script_path, wrapped_script).map_err(|error| error.to_string())?;

    let launch_script = format!(
        r#"
$process = Start-Process -FilePath 'powershell.exe' -ArgumentList '-NoProfile -NonInteractive -ExecutionPolicy Bypass -File "{script_path}"' -Verb RunAs -Wait -PassThru
if ($null -eq $process) {{ exit 1 }}
exit $process.ExitCode
"#,
        script_path = script_path.to_string_lossy().replace('"', "`\""),
    );

    let launch_result = run_powershell(&launch_script);
    let result_content = fs::read_to_string(&result_path).unwrap_or_default();
    let _ = fs::remove_file(&script_path);
    let _ = fs::remove_file(&result_path);

    if let Err(error) = launch_result {
        if result_content.trim().starts_with("ERROR:") {
            return Err(result_content.trim().to_string());
        }

        if error.to_ascii_lowercase().contains("canceled") {
            return Err("OpenXR layer change was canceled at the elevation prompt.".into());
        }

        return Err(error);
    }

    if result_content.trim() == "OK" {
        Ok(())
    } else if result_content.trim().starts_with("ERROR:") {
        Err(result_content.trim().to_string())
    } else {
        Err("OpenXR layer change was canceled or did not complete.".into())
    }
}

fn read_registry_values(definition: &SliceDefinition) -> Result<Vec<RegistryValue>, String> {
    let script = format!(
        r#"
$base = [Microsoft.Win32.RegistryKey]::OpenBaseKey([Microsoft.Win32.RegistryHive]::{hive}, [Microsoft.Win32.RegistryView]::{view})
$key = $base.OpenSubKey('{key}', $false)
if ($null -eq $key) {{
  '[]'
  exit 0
}}
$items = foreach ($name in $key.GetValueNames()) {{
  [pscustomobject]@{{ Name = $name; Value = [int]$key.GetValue($name) }}
}}
$items | ConvertTo-Json -Compress
"#,
        hive = powershell_registry_hive(definition.hive),
        view = powershell_registry_view(definition.view),
        key = escape_powershell_single_quoted(definition.key_path),
    );
    let output = run_powershell(&script)?;
    parse_registry_values(&output)
}

fn parse_registry_values(output: &str) -> Result<Vec<RegistryValue>, String> {
    let trimmed = output.trim();
    if trimmed.is_empty() || trimmed == "null" {
        return Ok(Vec::new());
    }

    if trimmed.starts_with('[') {
        serde_json::from_str::<Vec<RegistryValue>>(trimmed).map_err(|error| error.to_string())
    } else {
        serde_json::from_str::<RegistryValue>(trimmed)
            .map(|value| vec![value])
            .map_err(|error| error.to_string())
    }
}

fn write_registry_value(
    definition: &SliceDefinition,
    manifest_path: &str,
    value: i64,
) -> Result<(), String> {
    let script = format!(
        r#"
$base = [Microsoft.Win32.RegistryKey]::OpenBaseKey([Microsoft.Win32.RegistryHive]::{hive}, [Microsoft.Win32.RegistryView]::{view})
$key = $base.OpenSubKey('{key}', $true)
if ($null -eq $key) {{ throw 'OpenXR implicit API layer registry key does not exist.' }}
$key.SetValue('{name}', {value}, [Microsoft.Win32.RegistryValueKind]::DWord)
"#,
        hive = powershell_registry_hive(definition.hive),
        view = powershell_registry_view(definition.view),
        key = escape_powershell_single_quoted(definition.key_path),
        name = escape_powershell_single_quoted(manifest_path),
        value = value,
    );

    if matches!(definition.hive, RegistryHive::LocalMachine) {
        run_powershell_elevated(&script)
    } else {
        run_powershell(&script).map(|_| ())
    }
}

fn rewrite_registry_values(
    definition: &SliceDefinition,
    values: &[RegistryValue],
) -> Result<(), String> {
    let mut names_literal = String::new();
    for value in values {
        names_literal.push_str(&format!(
            "[pscustomobject]@{{ Name = '{}'; Value = {} }},",
            escape_powershell_single_quoted(&value.name),
            value.value
        ));
    }

    let script = format!(
        r#"
$base = [Microsoft.Win32.RegistryKey]::OpenBaseKey([Microsoft.Win32.RegistryHive]::{hive}, [Microsoft.Win32.RegistryView]::{view})
$key = $base.OpenSubKey('{key}', $true)
if ($null -eq $key) {{ throw 'OpenXR implicit API layer registry key does not exist.' }}
$values = @({values})
foreach ($name in @($key.GetValueNames())) {{ $key.DeleteValue($name, $false) }}
foreach ($item in $values) {{ $key.SetValue($item.Name, [int]$item.Value, [Microsoft.Win32.RegistryValueKind]::DWord) }}
"#,
        hive = powershell_registry_hive(definition.hive),
        view = powershell_registry_view(definition.view),
        key = escape_powershell_single_quoted(definition.key_path),
        values = names_literal,
    );

    if matches!(definition.hive, RegistryHive::LocalMachine) {
        run_powershell_elevated(&script)
    } else {
        run_powershell(&script).map(|_| ())
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
}

#[derive(Debug, Clone)]
struct SignatureInfo {
    status: String,
    status_description: String,
    signer_subject: Option<String>,
    signer_issuer: Option<String>,
    signer_thumbprint: Option<String>,
}

impl SignatureInfo {
    fn unknown() -> Self {
        Self {
            status: "unknown".into(),
            status_description: "Windows signature information is unavailable for this binary.".into(),
            signer_subject: None,
            signer_issuer: None,
            signer_thumbprint: None,
        }
    }
}

fn signature_info(path: &str) -> SignatureInfo {
    let script = format!(
        r#"
$signature = Get-AuthenticodeSignature -LiteralPath '{path}'
[pscustomobject]@{{
  Status = $signature.Status.ToString()
  StatusMessage = $signature.StatusMessage
  SignerSubject = if ($null -ne $signature.SignerCertificate) {{ $signature.SignerCertificate.Subject }} else {{ $null }}
  SignerIssuer = if ($null -ne $signature.SignerCertificate) {{ $signature.SignerCertificate.Issuer }} else {{ $null }}
  SignerThumbprint = if ($null -ne $signature.SignerCertificate) {{ $signature.SignerCertificate.Thumbprint }} else {{ $null }}
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

    let status_description = raw
        .status_message
        .filter(|message| !message.trim().is_empty())
        .unwrap_or_else(|| match status.as_str() {
            "signed" => "Windows reports a valid Authenticode signature.".into(),
            "unsigned" => "Windows reports that this binary is not Authenticode signed.".into(),
            "invalid" => "Windows reports that this binary's Authenticode signature is invalid.".into(),
            _ => "Windows could not determine this binary's signature status.".into(),
        });

    SignatureInfo {
        status,
        status_description,
        signer_subject: raw.signer_subject,
        signer_issuer: raw.signer_issuer,
        signer_thumbprint: raw.signer_thumbprint,
    }
}

#[allow(dead_code)]
fn _preserve_registry_value_order(values: &[RegistryValue]) -> HashMap<String, usize> {
    values
        .iter()
        .enumerate()
        .map(|(index, value)| (value.name.clone(), index))
        .collect()
}
