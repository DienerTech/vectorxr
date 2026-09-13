use serde::Serialize;
use std::fs;
use std::io::{Read, Seek, SeekFrom};
use std::path::Path;

const MAX_FILE_BYTES: u64 = 8 * 1024 * 1024;
const MAX_TOTAL_BYTES: u64 = 64 * 1024 * 1024;

#[derive(Serialize)]
#[serde(rename_all = "camelCase")]
pub struct DebugSourceFile {
    archive_path: String,
    source_path: String,
    content: Option<String>,
    original_bytes: u64,
    truncated: bool,
    error: Option<String>,
}

#[derive(Default, Serialize)]
#[serde(rename_all = "camelCase")]
pub struct DebugSourceSnapshot {
    files: Vec<DebugSourceFile>,
    warnings: Vec<String>,
    #[serde(skip)]
    captured_bytes: u64,
}

impl DebugSourceSnapshot {
    fn file(&mut self, path: &Path, archive_path: String, tail: bool) {
        let mut entry = DebugSourceFile {
            archive_path,
            source_path: path.to_string_lossy().into_owned(),
            content: None,
            original_bytes: 0,
            truncated: false,
            error: None,
        };
        let mut read = || -> Result<Vec<u8>, String> {
            let metadata = fs::symlink_metadata(path).map_err(|e| e.to_string())?;
            if !metadata.file_type().is_file() {
                return Err("Not a regular file".into());
            }
            let mut file = fs::File::open(path).map_err(|e| e.to_string())?;
            entry.original_bytes = file.metadata().map_err(|e| e.to_string())?.len();
            let limit = MAX_FILE_BYTES.min(MAX_TOTAL_BYTES.saturating_sub(self.captured_bytes));
            entry.truncated = entry.original_bytes > limit;
            if entry.truncated && (!tail || limit == 0) {
                return Err("Omitted: debug export size limit reached".into());
            }
            if tail {
                file.seek(SeekFrom::Start(entry.original_bytes.saturating_sub(limit)))
                    .map_err(|e| e.to_string())?;
            }
            let mut bytes = Vec::new();
            file.take(limit)
                .read_to_end(&mut bytes)
                .map_err(|e| e.to_string())?;
            Ok(bytes)
        };
        match read() {
            Ok(bytes) => {
                self.captured_bytes += bytes.len() as u64;
                entry.content = Some(String::from_utf8_lossy(&bytes).into_owned());
            }
            Err(error) => entry.error = Some(error),
        }
        self.files.push(entry);
    }

    fn directory(&mut self, directory: &Path, archive_directory: &str, extension: &str) {
        let entries = match fs::read_dir(directory) {
            Ok(entries) => entries,
            Err(error) => {
                self.warnings
                    .push(format!("{}: {error}", directory.display()));
                return;
            }
        };
        let mut paths = Vec::new();
        for entry in entries {
            match entry {
                Ok(entry)
                    if entry.path().extension().and_then(|s| s.to_str()) == Some(extension) =>
                {
                    paths.push(entry.path())
                }
                Ok(_) => (),
                Err(error) => self
                    .warnings
                    .push(format!("{}: {error}", directory.display())),
            }
        }
        paths.sort();
        for path in paths {
            let name = path.file_name().unwrap().to_string_lossy();
            self.file(&path, format!("{archive_directory}/{name}"), false);
        }
    }
}

pub fn collect() -> DebugSourceSnapshot {
    let mut snapshot = DebugSourceSnapshot::default();
    // Explicit sources only; do not recursively gather unrelated user files.
    for (path, name) in [
        (super::resolve_config_path(), "settings.json"),
        (super::resolve_seen_apps_path(), "seen-apps.json"),
        (super::resolve_runtime_pacing_path(), "runtime-pacing.json"),
        (super::resolve_turbo_metrics_path(), "turbo-metrics.json"),
    ] {
        snapshot.file(&path, format!("raw/{name}"), false);
    }
    let relay = super::resolve_runtime_relay_root();
    for directory in [
        "status",
        "control",
        "turbo-recovery",
        "turbo-recovery/faults",
    ] {
        snapshot.directory(
            &relay.join(directory),
            &format!("raw/runtime/{directory}"),
            "json",
        );
    }
    snapshot.directory(
        &relay.join("turbo-recovery/faults-cleared"),
        "raw/runtime/turbo-recovery/faults-cleared",
        "txt",
    );
    let pacing = super::resolve_runtime_pacing_path();
    snapshot.directory(
        &pacing
            .parent()
            .unwrap_or(Path::new("."))
            .join("runtime-pacing-resets"),
        "raw/runtime-pacing-resets",
        "txt",
    );
    match super::log_series_paths(&super::resolve_log_path()) {
        Ok(paths) => {
            for path in paths {
                let name = path.file_name().unwrap().to_string_lossy();
                snapshot.file(&path, format!("logs/{name}"), true);
            }
        }
        Err(error) => snapshot
            .warnings
            .push(format!("Unable to enumerate logs: {error}")),
    }
    snapshot
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn captures_raw_documents_and_logs_beyond_preview_and_reports_missing_files() {
        let root = std::env::temp_dir().join(format!(
            "vectorxr-debug-{}-{}",
            std::process::id(),
            super::super::unix_milliseconds()
        ));
        fs::create_dir_all(&root).unwrap();
        let raw = "{malformed but useful for debugging}";
        fs::write(root.join("record.json"), raw).unwrap();
        let log = "x".repeat(150_000);
        fs::write(root.join("vectorxr.log"), &log).unwrap();
        fs::write(root.join("unrelated.txt"), "do not export").unwrap();
        let mut snapshot = DebugSourceSnapshot::default();
        snapshot.directory(&root, "raw", "json");
        snapshot.file(&root.join("vectorxr.log"), "logs/vectorxr.log".into(), true);
        snapshot.file(&root.join("missing.json"), "raw/missing.json".into(), false);
        assert_eq!(snapshot.files.len(), 3);
        assert_eq!(snapshot.files[0].content.as_deref(), Some(raw));
        assert_eq!(snapshot.files[1].content.as_deref(), Some(log.as_str()));
        assert!(!snapshot.files[1].truncated);
        assert!(snapshot.files[2].error.is_some());
        fs::remove_dir_all(root).unwrap();
    }

    #[test]
    fn size_limits_keep_log_tail_and_report_omitted_json() {
        let root = std::env::temp_dir().join(format!(
            "vectorxr-debug-limits-{}-{}",
            std::process::id(),
            super::super::unix_milliseconds()
        ));
        fs::create_dir_all(&root).unwrap();
        fs::write(root.join("log"), "0123456789").unwrap();
        let mut snapshot = DebugSourceSnapshot {
            captured_bytes: MAX_TOTAL_BYTES - 4,
            ..Default::default()
        };
        snapshot.file(&root.join("log"), "logs/log".into(), true);
        assert_eq!(snapshot.files[0].content.as_deref(), Some("6789"));
        assert!(snapshot.files[0].truncated);
        snapshot.file(&root.join("log"), "raw/data.json".into(), false);
        assert!(snapshot.files[1].content.is_none());
        assert!(snapshot.files[1].error.is_some());
        fs::remove_dir_all(root).unwrap();
    }
}
