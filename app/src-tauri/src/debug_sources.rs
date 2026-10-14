use serde::Serialize;
use std::fs;
use std::io::{Read, Seek, SeekFrom};
use std::path::Path;

const MAX_FILE_BYTES: u64 = 8 * 1024 * 1024;
const MAX_TOTAL_BYTES: u64 = 64 * 1024 * 1024;
// An oversized log keeps up to this much of its start, which holds the session
// header (version, runtime, GPU, Frame Generation mode), and spends the rest
// of its limit on the end.
const HEAD_BYTES: u64 = 256 * 1024;
// Below this limit an oversized log keeps only its end.
const MIN_HEAD_AND_TAIL_BYTES: u64 = 4 * 1024;
const OMITTED_MARKER_BYTES: u64 = 128;

// The layer logs this, early, once the runtime created the game's session
// (every layer version since June 2026).
const SESSION_MARKER: &[u8] = b"xrCreateSession succeeded downstream";
const SESSION_MARKER_SCAN_BYTES: u64 = 1024 * 1024;

fn reached_session(path: &Path) -> bool {
    let mut head = Vec::new();
    fs::File::open(path)
        .and_then(|file| file.take(SESSION_MARKER_SCAN_BYTES).read_to_end(&mut head))
        .is_ok()
        && head
            .windows(SESSION_MARKER.len())
            .any(|window| window == SESSION_MARKER)
}

// Keeps whole lines from the start and the end of `file` within about
// `limit` bytes, with a marker saying how much of the middle was left out.
fn read_head_and_tail(file: &mut fs::File, length: u64, limit: u64) -> std::io::Result<Vec<u8>> {
    let head_limit = HEAD_BYTES.min(limit / 4);
    let tail_limit = limit - head_limit - OMITTED_MARKER_BYTES;
    let mut head = Vec::new();
    file.by_ref().take(head_limit).read_to_end(&mut head)?;
    if let Some(end) = head.iter().rposition(|byte| *byte == b'\n') {
        head.truncate(end + 1);
    }
    file.seek(SeekFrom::Start(length - tail_limit))?;
    let mut tail = Vec::new();
    file.take(tail_limit).read_to_end(&mut tail)?;
    if let Some(start) = tail.iter().position(|byte| *byte == b'\n') {
        tail.drain(..=start);
    }
    let omitted = length.saturating_sub((head.len() + tail.len()) as u64);
    let mut bytes = head;
    bytes.extend_from_slice(
        format!("... Debug Export left out {omitted} bytes from the middle of this file ...\n")
            .as_bytes(),
    );
    bytes.extend_from_slice(&tail);
    Ok(bytes)
}

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
            if entry.truncated && limit >= MIN_HEAD_AND_TAIL_BYTES {
                return read_head_and_tail(&mut file, entry.original_bytes, limit)
                    .map_err(|e| e.to_string());
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
        // The complete sessions' Frame Generation snapshots come after every
        // main log and after both complete sessions' incident snapshots and
        // recorder parts, so the budget never drops any of those in their
        // favour. An older session's come after its own recorder files.
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
            files
        };
        let framegen = |path: &Path| {
            existing(
                path,
                &[
                    "flight-recorder.framegen.log",
                    "flight-recorder.framegen-1.log",
                ],
            )
        };
        // MSFS 2024 opens a probe instance that never creates a session before
        // the real one, and each instance writes its own short log. The
        // complete sessions are the newest logs that reached xrCreateSession
        // (the newest logs when none did), so a probe never takes a slot from
        // the session that was flown.
        let reached = paths
            .iter()
            .map(|path| reached_session(path))
            .collect::<Vec<_>>();
        let mut complete = vec![false; paths.len()];
        (0..paths.len())
            .filter(|&index| reached[index] || !reached.contains(&true))
            .take(COMPLETE_SESSIONS)
            .for_each(|index| complete[index] = true);
        let mut deferred = Vec::new();
        for (index, path) in paths.iter().enumerate() {
            self.log_file(path);
            if complete[index] {
                for companion in companions(path) {
                    self.log_file(&companion);
                }
                deferred.extend(framegen(path));
            }
        }
        for snapshot in deferred {
            self.log_file(&snapshot);
        }
        // The newest complete session's Frame Generation trace (written with
        // debug logging): minutes of per-frame history where the recorder
        // holds seconds. It can reach hundreds of MB, so it gets a log's
        // capped start-and-end cut (the start holds the CSV header) and comes
        // after everything above.
        if let Some(trace) = paths
            .iter()
            .zip(&complete)
            .filter(|(_, complete)| **complete)
            .map(|(path, _)| path.with_extension("framegen-trace.csv"))
            .find(|trace| trace.exists())
        {
            self.log_file(&trace);
        }
        for (index, path) in paths.iter().enumerate() {
            if complete[index] {
                continue;
            }
            for companion in companions(path).into_iter().chain(framegen(path)) {
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
    fn includes_main_logs_then_snapshots_then_recorder_parts_then_framegen_snapshots() {
        let root = std::env::temp_dir().join(format!(
            "vectorxr-recorder-export-{}-{}",
            std::process::id(),
            super::super::unix_milliseconds()
        ));
        fs::create_dir_all(&root).unwrap();
        let main = root.join("vectorxr-20261008-190626.log");
        let previous = root.join("vectorxr-20261008-180000.log");
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
        // The previous session: its incident snapshot must not lose the size
        // budget to the newest session's Frame Generation snapshots.
        fs::write(&previous, "previous session's main log").unwrap();
        fs::write(
            previous.with_extension("flight-recorder.stall.log"),
            "previous session's frozen history",
        )
        .unwrap();
        fs::write(
            previous.with_extension("flight-recorder.0.log"),
            "previous session's flight history",
        )
        .unwrap();
        fs::write(
            previous.with_extension("flight-recorder.framegen.log"),
            "previous session's frame generation history",
        )
        .unwrap();
        fs::write(root.join("unrelated.flight-recorder.0.log"), "unrelated").unwrap();
        fs::write(
            root.join("unrelated.flight-recorder.framegen.log"),
            "unrelated",
        )
        .unwrap();
        let mut snapshot = DebugSourceSnapshot::default();
        snapshot.logs(vec![main, previous]);
        assert_eq!(snapshot.files.len(), 11);
        assert_eq!(snapshot.files[0].content.as_deref(), Some("main log"));
        assert_eq!(
            snapshot.files[1].content.as_deref(),
            Some("latest frozen history")
        );
        assert_eq!(
            snapshot.files[2].content.as_deref(),
            Some("previous frozen history")
        );
        assert!(snapshot.files[3..5].iter().all(|f| f
            .archive_path
            .starts_with("logs/vectorxr-20261008-190626.flight-recorder.")
            && f.content.as_deref().unwrap().ends_with("flight history")));
        let contents = snapshot.files[5..]
            .iter()
            .map(|f| f.content.as_deref().unwrap())
            .collect::<Vec<_>>();
        assert_eq!(
            contents,
            vec![
                "previous session's main log",
                "previous session's frozen history",
                "previous session's flight history",
                "latest frame generation history",
                "previous frame generation history",
                "previous session's frame generation history",
            ]
        );
        let position = |name: &str| {
            snapshot
                .files
                .iter()
                .position(|f| f.archive_path == format!("logs/{name}"))
                .unwrap()
        };
        assert!(
            position("vectorxr-20261008-180000.flight-recorder.stall.log")
                < position("vectorxr-20261008-190626.flight-recorder.framegen.log")
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
            fs::write(
                session.with_extension("flight-recorder.framegen.log"),
                "frame generation",
            )
            .unwrap();
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
                "vectorxr-20261009-120000.flight-recorder.framegen.log",
                "vectorxr-20261009-110000.flight-recorder.framegen.log",
                "vectorxr-20261009-100000.flight-recorder.0.log",
                "vectorxr-20261009-100000.flight-recorder.framegen.log",
            ]
        );
        fs::remove_dir_all(root).unwrap();
    }

    #[test]
    fn the_newest_frame_generation_trace_follows_the_complete_sessions() {
        let root = std::env::temp_dir().join(format!(
            "vectorxr-debug-trace-{}-{}",
            std::process::id(),
            super::super::unix_milliseconds()
        ));
        fs::create_dir_all(&root).unwrap();
        let sessions = [
            "vectorxr-20261014-120000.log",
            "vectorxr-20261014-110000.log",
            "vectorxr-20261014-100000.log",
        ]
        .map(|name| root.join(name));
        for session in &sessions {
            fs::write(session, "main").unwrap();
            fs::write(session.with_extension("flight-recorder.0.log"), "recorder").unwrap();
        }
        // The newest session ran without debug logging; the trace comes from
        // the previous one, and the oldest one's is never taken.
        fs::write(
            sessions[0].with_extension("flight-recorder.framegen.log"),
            "frame generation",
        )
        .unwrap();
        fs::write(
            sessions[1].with_extension("framegen-trace.csv"),
            "name,ns,thread,id,a,b,c,d,e,f\nsession,0,0,0,0,0,0,0,0,0\n",
        )
        .unwrap();
        fs::write(sessions[2].with_extension("framegen-trace.csv"), "old").unwrap();
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
                "vectorxr-20261014-120000.log",
                "vectorxr-20261014-120000.flight-recorder.0.log",
                "vectorxr-20261014-110000.log",
                "vectorxr-20261014-110000.flight-recorder.0.log",
                "vectorxr-20261014-100000.log",
                "vectorxr-20261014-120000.flight-recorder.framegen.log",
                "vectorxr-20261014-110000.framegen-trace.csv",
                "vectorxr-20261014-100000.flight-recorder.0.log",
            ]
        );
        assert!(snapshot.files[6]
            .content
            .as_deref()
            .unwrap()
            .starts_with("name,ns,thread,id,"));
        fs::remove_dir_all(root).unwrap();
    }

    #[test]
    fn probe_instances_do_not_take_a_complete_session_slot() {
        let root = std::env::temp_dir().join(format!(
            "vectorxr-debug-probe-{}-{}",
            std::process::id(),
            super::super::unix_milliseconds()
        ));
        fs::create_dir_all(&root).unwrap();
        let session = "[info] xrCreateSession succeeded downstream; configuring VectorXR session resources.\n";
        let logs = [
            ("vectorxr-20261014-102320.log", "[info] xrDestroyInstance requested by application.\n"),
            ("vectorxr-20261014-102300.log", session),
            ("vectorxr-20261014-092254.log", "[info] xrDestroyInstance requested by application.\n"),
            ("vectorxr-20261014-090000.log", session),
        ]
        .map(|(name, content)| {
            let path = root.join(name);
            fs::write(&path, format!("[info] VectorXR layer version: 0.19.1\n{content}")).unwrap();
            fs::write(path.with_extension("flight-recorder.0.log"), "recorder").unwrap();
            path
        });
        let mut snapshot = DebugSourceSnapshot::default();
        snapshot.logs(logs.to_vec());
        let order = snapshot
            .files
            .iter()
            .map(|f| f.archive_path.trim_start_matches("logs/").to_string())
            .collect::<Vec<_>>();
        assert_eq!(
            order,
            vec![
                "vectorxr-20261014-102320.log",
                "vectorxr-20261014-102300.log",
                "vectorxr-20261014-102300.flight-recorder.0.log",
                "vectorxr-20261014-092254.log",
                "vectorxr-20261014-090000.log",
                "vectorxr-20261014-090000.flight-recorder.0.log",
                "vectorxr-20261014-102320.flight-recorder.0.log",
                "vectorxr-20261014-092254.flight-recorder.0.log",
            ]
        );
        fs::remove_dir_all(root).unwrap();
    }

    #[test]
    fn oversized_logs_keep_their_session_header_and_their_end() {
        let root = std::env::temp_dir().join(format!(
            "vectorxr-debug-head-tail-{}-{}",
            std::process::id(),
            super::super::unix_milliseconds()
        ));
        fs::create_dir_all(&root).unwrap();
        let header = "[info] VectorXR 0.19.2 | runtime=Pimax OpenXR | GPU=RTX 5090 | FG=Interpolate\n";
        let mut log = header.to_string();
        let mut line = 0;
        while log.len() < (MAX_FILE_BYTES + MAX_FILE_BYTES / 2) as usize {
            log.push_str(&format!("[debug] frame {line} pacing sample with some padding text\n"));
            line += 1;
        }
        log.push_str("[info] last line before exit\n");
        let path = root.join("vectorxr-20261014-120000.log");
        fs::write(&path, &log).unwrap();

        let mut snapshot = DebugSourceSnapshot::default();
        snapshot.file(&path, "logs/vectorxr.log".into(), true);
        let file = &snapshot.files[0];
        let content = file.content.as_deref().unwrap();
        assert!(file.truncated);
        assert!(content.starts_with(header), "{}", &content[..200]);
        assert!(content.ends_with("[info] last line before exit\n"));
        assert!(content.contains("bytes from the middle of this file ..."));
        assert!(content.len() as u64 <= MAX_FILE_BYTES);
        // Both cuts fall on line boundaries.
        let marker = content.find("... Debug Export left out").unwrap();
        assert!(content[..marker].ends_with('\n'));
        let after = &content[marker..];
        let resumed = &after[after.find('\n').unwrap() + 1..];
        assert!(resumed.starts_with("[debug] frame "), "{}", &resumed[..80]);
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
