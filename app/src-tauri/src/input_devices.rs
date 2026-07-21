use serde::Serialize;
use std::sync::atomic::{AtomicU64, Ordering};

#[derive(Debug, Clone, Serialize)]
#[serde(rename_all = "camelCase")]
pub struct InputDeviceInfo {
    pub device_guid: String,
    pub product_guid: String,
    pub device_name: String,
    pub product_name: String,
    pub button_count: u32,
    pub hat_count: u32,
}

#[derive(Debug, Clone, Serialize)]
#[serde(rename_all = "camelCase")]
pub struct CapturedDeviceBinding {
    pub device_guid: String,
    pub product_guid: String,
    pub device_name: String,
    pub input_path: String,
    pub input_label: String,
    pub input_index: u32,
}

static NEXT_CAPTURE_ID: AtomicU64 = AtomicU64::new(1);
static ACTIVE_CAPTURE_ID: AtomicU64 = AtomicU64::new(0);

pub fn begin_device_binding_capture() -> u64 {
    let capture_id = NEXT_CAPTURE_ID.fetch_add(1, Ordering::Relaxed);
    ACTIVE_CAPTURE_ID.store(capture_id, Ordering::Release);
    capture_id
}

pub fn cancel_device_binding_capture() {
    ACTIVE_CAPTURE_ID.store(0, Ordering::Release);
}

fn capture_is_active(capture_id: u64) -> bool {
    capture_id != 0 && ACTIVE_CAPTURE_ID.load(Ordering::Acquire) == capture_id
}

fn finish_capture(capture_id: u64) {
    let _ = ACTIVE_CAPTURE_ID.compare_exchange(capture_id, 0, Ordering::AcqRel, Ordering::Acquire);
}

#[cfg(windows)]
mod platform {
    use super::{capture_is_active, CapturedDeviceBinding, InputDeviceInfo};
    use std::ffi::c_void;
    use std::mem;
    use std::ptr;
    use std::thread;
    use std::time::{Duration, Instant};

    use windows::core::{IUnknown, Interface, GUID};
    use windows::Win32::Devices::HumanInterfaceDevice::{
        DirectInput8Create, IDirectInput8W, IDirectInputDevice8W, DI8DEVCLASS_GAMECTRL,
        DIDATAFORMAT, DIDEVCAPS, DIDEVICEINSTANCEW, DIDFT_BUTTON, DIDFT_POV, DIDF_ABSAXIS,
        DIEDFL_ATTACHEDONLY, DIERR_INPUTLOST, DIERR_NOTACQUIRED, DIOBJECTDATAFORMAT,
        DISCL_BACKGROUND, DISCL_NONEXCLUSIVE,
    };
    use windows::Win32::Foundation::{HINSTANCE, HWND, TRUE};
    use windows::Win32::System::LibraryLoader::GetModuleHandleW;

    const DIRECTINPUT_VERSION: u32 = 0x0800;
    const DEFAULT_CAPTURE_TIMEOUT_MS: u64 = 15_000;
    const MAX_BUTTONS: usize = 128;
    const MAX_HATS: usize = 4;
    const HAT_CENTERED: u32 = u32::MAX;
    const GUID_BUTTON: GUID = GUID::from_u128(0xa36d02f0_c9f3_11cf_bfc7_444553540000);
    const GUID_POV: GUID = GUID::from_u128(0xa36d02f2_c9f3_11cf_bfc7_444553540000);

    #[derive(Debug, Clone)]
    struct DeviceInstance {
        device_guid: GUID,
        product_guid: GUID,
        device_name: String,
        product_name: String,
    }

    #[repr(C)]
    #[derive(Clone, Copy)]
    struct CaptureState {
        buttons: [u8; MAX_BUTTONS],
        hats: [u32; MAX_HATS],
    }

    impl Default for CaptureState {
        fn default() -> Self {
            Self {
                buttons: [0; MAX_BUTTONS],
                hats: [HAT_CENTERED; MAX_HATS],
            }
        }
    }

    struct CaptureDevice {
        info: InputDeviceInfo,
        device: IDirectInputDevice8W,
        previous_state: CaptureState,
        suppressed_buttons: [bool; MAX_BUTTONS],
        suppressed_hats: [bool; MAX_HATS],
        button_count: usize,
        hat_count: usize,
    }

    pub fn list_input_devices() -> Result<Vec<InputDeviceInfo>, String> {
        let direct_input = create_direct_input()?;
        let instances = enumerate_instances(&direct_input)?;
        instances
            .into_iter()
            .map(|instance| {
                let (button_count, hat_count) = create_device(&direct_input, &instance.device_guid)
                    .and_then(|device| device_counts(&device))
                    .unwrap_or((0, 0));

                Ok(InputDeviceInfo {
                    device_guid: format_guid(&instance.device_guid),
                    product_guid: format_guid(&instance.product_guid),
                    device_name: instance.device_name,
                    product_name: instance.product_name,
                    button_count,
                    hat_count,
                })
            })
            .collect()
    }

    pub fn capture_device_binding_platform(
        timeout_ms: Option<u64>,
        capture_id: u64,
    ) -> Result<Option<CapturedDeviceBinding>, String> {
        let direct_input = create_direct_input()?;
        let instances = enumerate_instances(&direct_input)?;
        let mut devices = Vec::new();

        for instance in instances {
            if !capture_is_active(capture_id) {
                return Ok(None);
            }

            let device = match create_device(&direct_input, &instance.device_guid) {
                Ok(device) => device,
                Err(_) => continue,
            };

            let (button_count, hat_count) = device_counts(&device).unwrap_or((0, 0));
            let button_count = button_count.min(MAX_BUTTONS as u32) as usize;
            let hat_count = hat_count.min(MAX_HATS as u32) as usize;
            if button_count == 0 && hat_count == 0 {
                continue;
            }

            if configure_device(&device, button_count, hat_count).is_err() {
                continue;
            }

            let Ok(previous_state) = read_stable_state(&device) else {
                continue;
            };
            let suppressed_buttons = pressed_button_mask(&previous_state.buttons, button_count);
            let suppressed_hats = active_hat_mask(&previous_state.hats, hat_count);
            devices.push(CaptureDevice {
                info: InputDeviceInfo {
                    device_guid: format_guid(&instance.device_guid),
                    product_guid: format_guid(&instance.product_guid),
                    device_name: instance.device_name,
                    product_name: instance.product_name,
                    button_count: button_count as u32,
                    hat_count: hat_count as u32,
                },
                device,
                previous_state,
                suppressed_buttons,
                suppressed_hats,
                button_count,
                hat_count,
            });
        }

        if devices.is_empty() {
            return Err(
                "No DirectInput joystick devices with buttons or HAT switches were found".into(),
            );
        }

        let timeout = Duration::from_millis(timeout_ms.unwrap_or(DEFAULT_CAPTURE_TIMEOUT_MS));
        let started = Instant::now();
        while started.elapsed() < timeout && capture_is_active(capture_id) {
            for capture_device in &mut devices {
                let state = match read_state(&capture_device.device) {
                    Ok(state) => state,
                    Err(_) => continue,
                };

                for index in 0..capture_device.button_count {
                    const PRESSED: u8 = 0x80;
                    const RELEASED: u8 = 0x00;
                    const BUTTON_OFFSET: u32 = 1;
                    let was_down =
                        (capture_device.previous_state.buttons[index] & PRESSED) != RELEASED;
                    let is_down = (state.buttons[index] & PRESSED) != RELEASED;

                    if capture_device.suppressed_buttons[index] {
                        if !is_down {
                            capture_device.suppressed_buttons[index] = false;
                        }
                        continue;
                    }

                    if is_down && !was_down {
                        let button_number = index as u32 + BUTTON_OFFSET;
                        return Ok(Some(CapturedDeviceBinding {
                            device_guid: capture_device.info.device_guid.clone(),
                            product_guid: capture_device.info.product_guid.clone(),
                            device_name: capture_device.info.device_name.clone(),
                            input_path: format!("button-{button_number}"),
                            input_label: format!("Button {button_number}"),
                            input_index: button_number,
                        }));
                    }
                }

                for index in 0..capture_device.hat_count {
                    let previous_direction =
                        hat_direction(capture_device.previous_state.hats[index]);
                    let direction = hat_direction(state.hats[index]);

                    if capture_device.suppressed_hats[index] {
                        if direction.is_none() {
                            capture_device.suppressed_hats[index] = false;
                        }
                        continue;
                    }

                    if let Some(direction) = direction {
                        if Some(direction) != previous_direction {
                            let hat_number = index as u32 + 1;
                            return Ok(Some(CapturedDeviceBinding {
                                device_guid: capture_device.info.device_guid.clone(),
                                product_guid: capture_device.info.product_guid.clone(),
                                device_name: capture_device.info.device_name.clone(),
                                input_path: format!("hat-{hat_number}-{}", direction.path),
                                input_label: format!("HAT {hat_number} {}", direction.label),
                                input_index: hat_number,
                            }));
                        }
                    }
                }

                capture_device.previous_state = state;
            }

            thread::sleep(Duration::from_millis(25));
        }

        Ok(None)
    }

    fn pressed_button_mask(
        buttons: &[u8; MAX_BUTTONS],
        button_count: usize,
    ) -> [bool; MAX_BUTTONS] {
        let mut mask = [false; MAX_BUTTONS];
        for index in 0..button_count.min(MAX_BUTTONS) {
            mask[index] = (buttons[index] & 0x80) != 0;
        }
        mask
    }

    fn active_hat_mask(hats: &[u32; MAX_HATS], hat_count: usize) -> [bool; MAX_HATS] {
        let mut mask = [false; MAX_HATS];
        for index in 0..hat_count.min(MAX_HATS) {
            mask[index] = hat_direction(hats[index]).is_some();
        }
        mask
    }

    #[derive(Clone, Copy, PartialEq, Eq)]
    struct HatDirection {
        path: &'static str,
        label: &'static str,
    }

    fn hat_direction(value: u32) -> Option<HatDirection> {
        if value == HAT_CENTERED {
            return None;
        }

        const DIRECTIONS: [HatDirection; 8] = [
            HatDirection {
                path: "up",
                label: "Up",
            },
            HatDirection {
                path: "up-right",
                label: "Up Right",
            },
            HatDirection {
                path: "right",
                label: "Right",
            },
            HatDirection {
                path: "down-right",
                label: "Down Right",
            },
            HatDirection {
                path: "down",
                label: "Down",
            },
            HatDirection {
                path: "down-left",
                label: "Down Left",
            },
            HatDirection {
                path: "left",
                label: "Left",
            },
            HatDirection {
                path: "up-left",
                label: "Up Left",
            },
        ];

        // DirectInput reports hundredths of a degree clockwise from up.
        // Round to the nearest of eight discrete directions so both 4-way and
        // 8-way HAT switches bind predictably.
        let octant = (((value % 36_000) + 2_250) / 4_500) % 8;
        Some(DIRECTIONS[octant as usize])
    }

    fn create_direct_input() -> Result<IDirectInput8W, String> {
        unsafe {
            let module = GetModuleHandleW(None).map_err(|error| error.to_string())?;
            let mut direct_input_raw: *mut c_void = ptr::null_mut();
            DirectInput8Create(
                HINSTANCE(module.0),
                DIRECTINPUT_VERSION,
                &IDirectInput8W::IID,
                &mut direct_input_raw,
                None::<&IUnknown>,
            )
            .map_err(|error| error.to_string())?;

            if direct_input_raw.is_null() {
                return Err("DirectInput8Create returned no DirectInput interface".into());
            }

            Ok(IDirectInput8W::from_raw(direct_input_raw))
        }
    }

    fn enumerate_instances(direct_input: &IDirectInput8W) -> Result<Vec<DeviceInstance>, String> {
        unsafe extern "system" fn enum_callback(
            instance: *mut DIDEVICEINSTANCEW,
            context: *mut c_void,
        ) -> windows::core::BOOL {
            if instance.is_null() || context.is_null() {
                return TRUE;
            }

            let devices = &mut *(context as *mut Vec<DeviceInstance>);
            let instance = *instance;
            devices.push(DeviceInstance {
                device_guid: instance.guidInstance,
                product_guid: instance.guidProduct,
                device_name: wide_array_to_string(&instance.tszInstanceName),
                product_name: wide_array_to_string(&instance.tszProductName),
            });

            TRUE
        }

        let mut devices = Vec::new();
        unsafe {
            direct_input
                .EnumDevices(
                    DI8DEVCLASS_GAMECTRL,
                    Some(enum_callback),
                    &mut devices as *mut Vec<DeviceInstance> as *mut c_void,
                    DIEDFL_ATTACHEDONLY,
                )
                .map_err(|error| error.to_string())?;
        }
        Ok(devices)
    }

    fn create_device(
        direct_input: &IDirectInput8W,
        guid: &GUID,
    ) -> Result<IDirectInputDevice8W, String> {
        let mut device = None;
        unsafe {
            direct_input
                .CreateDevice(guid, &mut device, None::<&IUnknown>)
                .map_err(|error| error.to_string())?;
        }
        device.ok_or_else(|| "DirectInput returned no device interface".into())
    }

    fn configure_device(
        device: &IDirectInputDevice8W,
        button_count: usize,
        hat_count: usize,
    ) -> Result<(), String> {
        let button_count = button_count.min(MAX_BUTTONS);
        let hat_count = hat_count.min(MAX_HATS);
        let mut object_formats = Vec::with_capacity(button_count + hat_count);

        for index in 0..button_count {
            object_formats.push(DIOBJECTDATAFORMAT {
                pguid: &GUID_BUTTON,
                dwOfs: index as u32,
                dwType: DIDFT_BUTTON | ((index as u32) << 8),
                dwFlags: 0,
            });
        }
        for index in 0..hat_count {
            object_formats.push(DIOBJECTDATAFORMAT {
                pguid: &GUID_POV,
                dwOfs: (MAX_BUTTONS + index * mem::size_of::<u32>()) as u32,
                dwType: DIDFT_POV | ((index as u32) << 8),
                dwFlags: 0,
            });
        }

        let mut data_format = DIDATAFORMAT {
            dwSize: mem::size_of::<DIDATAFORMAT>() as u32,
            dwObjSize: mem::size_of::<DIOBJECTDATAFORMAT>() as u32,
            dwFlags: DIDF_ABSAXIS,
            dwDataSize: mem::size_of::<CaptureState>() as u32,
            dwNumObjs: object_formats.len() as u32,
            rgodf: object_formats.as_mut_ptr(),
        };

        unsafe {
            device
                .SetDataFormat(&mut data_format)
                .map_err(|error| error.to_string())?;
            let _ = device
                .SetCooperativeLevel(HWND(ptr::null_mut()), DISCL_BACKGROUND | DISCL_NONEXCLUSIVE);
            let _ = device.Acquire();
        }
        Ok(())
    }

    fn device_counts(device: &IDirectInputDevice8W) -> Result<(u32, u32), String> {
        let mut caps = DIDEVCAPS::default();
        caps.dwSize = mem::size_of::<DIDEVCAPS>() as u32;
        unsafe {
            device
                .GetCapabilities(&mut caps)
                .map_err(|error| error.to_string())?;
        }
        Ok((caps.dwButtons, caps.dwPOVs))
    }

    fn read_state(device: &IDirectInputDevice8W) -> windows::core::Result<CaptureState> {
        let mut state = CaptureState::default();
        unsafe {
            let poll_result = device.Poll();
            if poll_result.is_err() {
                let _ = device.Acquire();
                let _ = device.Poll();
            }

            match device.GetDeviceState(
                mem::size_of::<CaptureState>() as u32,
                &mut state as *mut CaptureState as *mut c_void,
            ) {
                Ok(()) => Ok(state),
                Err(error)
                    if error.code() == DIERR_INPUTLOST || error.code() == DIERR_NOTACQUIRED =>
                {
                    let _ = device.Acquire();
                    device.GetDeviceState(
                        mem::size_of::<CaptureState>() as u32,
                        &mut state as *mut CaptureState as *mut c_void,
                    )?;
                    Ok(state)
                }
                Err(error) => Err(error),
            }
        }
    }

    fn read_stable_state(device: &IDirectInputDevice8W) -> windows::core::Result<CaptureState> {
        let mut last_state = read_state(device)?;
        for _ in 0..3 {
            thread::sleep(Duration::from_millis(25));
            last_state = read_state(device)?;
        }
        Ok(last_state)
    }

    fn wide_array_to_string(value: &[u16]) -> String {
        let length = value
            .iter()
            .position(|character| *character == 0)
            .unwrap_or(value.len());
        String::from_utf16_lossy(&value[..length])
    }

    fn format_guid(guid: &GUID) -> String {
        format!(
            "{{{:08x}-{:04x}-{:04x}-{:02x}{:02x}-{:02x}{:02x}{:02x}{:02x}{:02x}{:02x}}}",
            guid.data1,
            guid.data2,
            guid.data3,
            guid.data4[0],
            guid.data4[1],
            guid.data4[2],
            guid.data4[3],
            guid.data4[4],
            guid.data4[5],
            guid.data4[6],
            guid.data4[7],
        )
    }
    #[cfg(test)]
    mod tests {
        use super::{hat_direction, HAT_CENTERED};

        #[test]
        fn direct_input_hat_values_map_to_cardinal_and_diagonal_paths() {
            assert_eq!(hat_direction(0).map(|direction| direction.path), Some("up"));
            assert_eq!(
                hat_direction(4_500).map(|direction| direction.path),
                Some("up-right")
            );
            assert_eq!(
                hat_direction(27_000).map(|direction| direction.path),
                Some("left")
            );
            assert_eq!(
                hat_direction(35_999).map(|direction| direction.path),
                Some("up")
            );
            assert!(hat_direction(HAT_CENTERED).is_none());
        }
    }
}

#[cfg(not(windows))]
mod platform {
    use super::{CapturedDeviceBinding, InputDeviceInfo};

    pub fn list_input_devices() -> Result<Vec<InputDeviceInfo>, String> {
        Ok(Vec::new())
    }

    pub fn capture_device_binding_platform(
        _timeout_ms: Option<u64>,
        _capture_id: u64,
    ) -> Result<Option<CapturedDeviceBinding>, String> {
        Err("Joystick binding capture is only available on Windows".into())
    }
}

pub use platform::list_input_devices;

pub fn capture_device_binding(
    timeout_ms: Option<u64>,
    capture_id: u64,
) -> Result<Option<CapturedDeviceBinding>, String> {
    if !capture_is_active(capture_id) {
        return Ok(None);
    }

    let result = platform::capture_device_binding_platform(timeout_ms, capture_id);
    finish_capture(capture_id);
    result
}

#[cfg(test)]
mod capture_session_tests {
    use super::{begin_device_binding_capture, cancel_device_binding_capture, capture_is_active};

    #[test]
    fn capture_tokens_cancel_and_supersede_safely() {
        let first = begin_device_binding_capture();
        assert!(capture_is_active(first));

        let second = begin_device_binding_capture();
        assert!(!capture_is_active(first));
        assert!(capture_is_active(second));

        cancel_device_binding_capture();
        assert!(!capture_is_active(second));
    }
}
