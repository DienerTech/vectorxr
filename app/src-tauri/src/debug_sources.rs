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

    fn log_file(&mut self, path: &Path) {
        let name = path.file_name().unwrap().to_string_lossy().into_owned();
        self.file(path, format!("logs/{name}"), true);
    }

    fn logs(&mut self, paths: Vec<std::path::PathBuf>) {
        // The newest sessions are exported complete. Every remaining main log
        // comes before older recorder history, so the size budget can never
        // drop a session's ordinary log in favour of another's recorder files.
        const COMPLETE_SESSIONS: usize = 2;
        let existing = |path: &Path, extensions: &[&str]| {
            extensions
                .iter()
                .map(|extension| path.with_extension(extension))
                .filter(|snapshot| snapshot.exists())
                .collect::<Vec<_>>()
        };
        let companions = |path: &Path| {
            let mut files = existing(
                path,
                &[
                    "flight-recorder.stall.log",
                    "flight-recorder.stall-1.log",
                    "flight-recorder.stall-2.log",
                ],
            );
            let mut recorders = (0..2)
                .map(|part| path.with_extension(format!("flight-recorder.{part}.log")))
                .filter(|recorder| recorder.exists())
                .collect::<Vec<_>>();
            recorders.sort_by_key(|recorder| {
                std::cmp::Reverse(fs::metadata(recorder).and_then(|m| m.modified()).ok())
            });
            files.extend(recorders);
            // Frame Generation snapshots come last, so the size budget never
            // drops incident snapshots or recent history in their favour.
            files.extend(existing(
                path,
                &[
                    "flight-recorder.framegen.log",
                    "flight-recorder.framegen-1.log",
                ],
            ));
            files
        };
        for (index, path) in paths.iter().enumerate() {
            self.log_file(path);
            if index < COMPLETE_SESSIONS {
                for companion in companions(path) {
                    self.log_file(&companion);
                }
            }
        }
        for path in paths.iter().skip(COMPLETE_SESSIONS) {
            for companion in companions(path) {
                self.log_file(&companion);
            }
        }
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
        Ok(paths) => snapshot.logs(paths),
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
    fn includes_main_log_then_snapshots_then_recorder_parts_then_framegen_snapshots() {
        let root = std::env::temp_dir().join(format!(
            "vectorxr-recorder-export-{}-{}",
            std::process::id(),
            super::super::unix_milliseconds()
        ));
        fs::create_dir_all(&root).unwrap();
        let main = root.join("vectorxr-20261008-190626.log");
        fs::write(&main, "main log").unwrap();
        fs::write(
            main.with_extension("flight-recorder.0.log"),
            "older flight history",
        )
        .unwrap();
        fs::write(
            main.with_extension("flight-recorder.1.log"),
            "recent flight history",
        )
        .unwrap();
        fs::write(
            main.with_extension("flight-recorder.stall.log"),
            "latest frozen history",
        )
        .unwrap();
        fs::write(
            main.with_extension("flight-recorder.stall-1.log"),
            "previous frozen history",
        )
        .unwrap();
        fs::write(
            main.with_extension("flight-recorder.framegen.log"),
            "latest frame generation history",
        )
        .unwrap();
        fs::write(
            main.with_extension("flight-recorder.framegen-1.log"),
            "previous frame generation history",
        )
        .unwrap();
        fs::write(root.join("unrelated.flight-recorder.0.log"), "unrelated").unwrap();
        fs::write(
            root.join("unrelated.flight-recorder.framegen.log"),
            "unrelated",
        )
        .unwrap();
        let mut snapshot = DebugSourceSnapshot::default();
        snapshot.logs(vec![main]);
        assert_eq!(snapshot.files.len(), 7);
        assert_eq!(snapshot.files[0].content.as_deref(), Some("main log"));
        assert_eq!(
            snapshot.files[1].content.as_deref(),
            Some("latest frozen history")
        );
        assert_eq!(
            snapshot.files[2].content.as_deref(),
            Some("previous frozen history")
        );
        assert!(snapshot.files[3..5]
            .iter()
            .all(|f| f.archive_path.contains("flight-recorder.")
                && f.content.as_deref().unwrap().ends_with("flight history")));
        assert_eq!(
            snapshot.files[5].archive_path,
            "logs/vectorxr-20261008-190626.flight-recorder.framegen.log"
        );
        assert_eq!(
            snapshot.files[6].content.as_deref(),
            Some("previous frame generation history")
        );
        assert!(snapshot.files.iter().all(|f| f.error.is_none()));
        fs::remove_dir_all(root).unwrap();
    }

    #[test]
    fn older_main_logs_come_before_older_recorder_history() {
        let root = std::env::temp_dir().join(format!(
            "vectorxr-recorder-priority-{}-{}",
            std::process::id(),
            super::super::unix_milliseconds()
        ));
        fs::create_dir_all(&root).unwrap();
        let sessions = [
            "vectorxr-20261009-120000.log",
            "vectorxr-20261009-110000.log",
            "vectorxr-20261009-100000.log",
        ]
        .map(|name| root.join(name));
        for session in &sessions {
            fs::write(session, "main").unwrap();
            fs::write(session.with_extension("flight-recorder.0.log"), "recorder").unwrap();
        }
        let mut snapshot = DebugSourceSnapshot::default();
        snapshot.logs(sessions.to_vec());
        let order = snapshot
            .files
            .iter()
            .map(|f| f.archive_path.trim_start_matches("logs/").to_string())
            .collect::<Vec<_>>();
        assert_eq!(
            order,
            vec![
                "vectorxr-20261009-120000.log",
                "vectorxr-20261009-120000.flight-recorder.0.log",
                "vectorxr-20261009-110000.log",
                "vectorxr-20261009-110000.flight-recorder.0.log",
                "vectorxr-20261009-100000.log",
                "vectorxr-20261009-100000.flight-recorder.0.log",
            ]
        );
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
