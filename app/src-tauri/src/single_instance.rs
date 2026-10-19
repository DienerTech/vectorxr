// One app per installed copy. With the tray keeping the app running after its
// window closes, starting VectorXR again would otherwise add a second window
// and a second tray icon; instead the new process brings the running app's
// window back and exits.
//
// The running app holds a named mutex and waits on a named event. A second
// process finds the mutex, sets the event, lets the running app take the
// foreground, and exits. Both names carry a hash of the executable's path, so
// a development build and the installed app run side by side. Anything that
// fails here only means two instances can run, as before.

use std::path::Path;

// FNV-1a: stable across Rust versions and builds, unlike std's hasher, so an
// installed app and its update agree on the names.
fn fnv1a(text: &str) -> u64 {
    text.bytes().fold(0xcbf2_9ce4_8422_2325, |hash, byte| {
        (hash ^ u64::from(byte)).wrapping_mul(0x0000_0100_0000_01b3)
    })
}

pub fn object_names(executable: &Path) -> (String, String) {
    let key = fnv1a(&executable.to_string_lossy().to_lowercase());
    (
        format!("Local\\VectorXR.App.{key:016x}"),
        format!("Local\\VectorXR.App.{key:016x}.Show"),
    )
}

pub enum Claim {
    // This is the only instance; wake it with `listen`.
    First(Instance),
    // The running instance was asked to show its window; exit.
    Signalled,
    // Could not tell: run anyway.
    Unclaimed,
}

pub struct Instance {
    #[cfg(windows)]
    event: isize,
}

#[cfg(windows)]
pub fn claim() -> Claim {
    use windows::core::HSTRING;
    use windows::Win32::Foundation::{CloseHandle, GetLastError, ERROR_ALREADY_EXISTS};
    use windows::Win32::System::Threading::{CreateEventW, CreateMutexW, OpenEventW, SetEvent, EVENT_MODIFY_STATE};
    use windows::Win32::UI::WindowsAndMessaging::{AllowSetForegroundWindow, ASFW_ANY};

    let Ok(executable) = std::env::current_exe() else {
        return Claim::Unclaimed;
    };
    let (mutex_name, event_name) = object_names(&executable);
    unsafe {
        // Never closed: held for the life of the process.
        let Ok(_mutex) = CreateMutexW(None, false, &HSTRING::from(mutex_name)) else {
            return Claim::Unclaimed;
        };
        if GetLastError() == ERROR_ALREADY_EXISTS {
            let Ok(event) = OpenEventW(EVENT_MODIFY_STATE, false, &HSTRING::from(event_name)) else {
                // Starting at the same moment, or another user's or an
                // elevated instance the event cannot be opened from.
                return Claim::Unclaimed;
            };
            let _ = AllowSetForegroundWindow(ASFW_ANY);
            let signalled = SetEvent(event).is_ok();
            let _ = CloseHandle(event);
            return if signalled { Claim::Signalled } else { Claim::Unclaimed };
        }
        match CreateEventW(None, false, false, &HSTRING::from(event_name)) {
            Ok(event) => Claim::First(Instance { event: event.0 as isize }),
            Err(_) => Claim::Unclaimed,
        }
    }
}

#[cfg(not(windows))]
pub fn claim() -> Claim {
    Claim::Unclaimed
}

impl Instance {
    // Calls `wake` each time another process asks for the window.
    pub fn listen<F: Fn() + Send + 'static>(self, wake: F) {
        #[cfg(windows)]
        {
            use windows::Win32::Foundation::{HANDLE, WAIT_OBJECT_0};
            use windows::Win32::System::Threading::{WaitForSingleObject, INFINITE};

            let event = self.event;
            let _ = std::thread::Builder::new().name("single-instance".to_string()).spawn(move || loop {
                let wait = unsafe { WaitForSingleObject(HANDLE(event as *mut _), INFINITE) };
                if wait != WAIT_OBJECT_0 {
                    return;
                }
                wake();
            });
        }
        #[cfg(not(windows))]
        let _ = wake;
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn names_depend_on_the_path_but_not_its_case() {
        let installed = object_names(Path::new(r"C:\Program Files\VectorXR\vectorxr-app.exe"));
        let shouted = object_names(Path::new(r"C:\PROGRAM FILES\VectorXR\VECTORXR-APP.EXE"));
        let dev = object_names(Path::new(r"C:\src\vectorxr\app\src-tauri\target\debug\vectorxr-app.exe"));
        assert_eq!(installed, shouted);
        assert_ne!(installed.0, dev.0);
        assert!(installed.0.starts_with("Local\\VectorXR.App.") && installed.1 == format!("{}.Show", installed.0));
    }

    #[test]
    fn fnv1a_matches_the_reference_values() {
        assert_eq!(fnv1a(""), 0xcbf2_9ce4_8422_2325);
        assert_eq!(fnv1a("a"), 0xaf63_dc4c_8601_ec8c);
    }
}
