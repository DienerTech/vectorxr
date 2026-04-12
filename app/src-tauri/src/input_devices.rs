use serde::Serialize;

#[derive(Debug, Clone, Serialize)]
#[serde(rename_all = "camelCase")]
pub struct InputDeviceInfo {
    pub device_guid: String,
    pub product_guid: String,
    pub device_name: String,
    pub product_name: String,
    pub button_count: u32,
}

#[derive(Debug, Clone, Serialize)]
#[serde(rename_all = "camelCase")]
pub struct CapturedDeviceBinding {
    pub device_guid: String,
    pub product_guid: String,
    pub device_name: String,
    pub input_path: String,
    pub input_label: String,
    pub button_index: u32,
}

#[cfg(windows)]
mod platform {
    use super::{CapturedDeviceBinding, InputDeviceInfo};
    use std::ffi::c_void;
    use std::mem;
    use std::ptr;
    use std::thread;
    use std::time::{Duration, Instant};

    use windows::core::{GUID, Interface, IUnknown};
    use windows::Win32::Devices::HumanInterfaceDevice::{
        DirectInput8Create, DIDEVCAPS, DIDEVICEINSTANCEW, DIDATAFORMAT, DIOBJECTDATAFORMAT, DI8DEVCLASS_GAMECTRL,
        DIEDFL_ATTACHEDONLY, DIDFT_BUTTON, DIDF_ABSAXIS, DISCL_BACKGROUND, DISCL_NONEXCLUSIVE, IDirectInput8W,
        IDirectInputDevice8W, DIERR_INPUTLOST, DIERR_NOTACQUIRED,
    };
    use windows::Win32::Foundation::{HINSTANCE, HWND, TRUE};
    use windows::Win32::System::LibraryLoader::GetModuleHandleW;

    const DIRECTINPUT_VERSION: u32 = 0x0800;
    const DEFAULT_CAPTURE_TIMEOUT_MS: u64 = 15_000;
    const MAX_BUTTONS: usize = 128;
    const GUID_BUTTON: GUID = GUID::from_u128(0xa36d02f0_c9f3_11cf_bfc7_444553540000);

    #[derive(Debug, Clone)]
    struct DeviceInstance {
        device_guid: GUID,
        product_guid: GUID,
        device_name: String,
        product_name: String,
    }

    struct CaptureDevice {
        info: InputDeviceInfo,
        device: IDirectInputDevice8W,
        previous_buttons: [u8; MAX_BUTTONS],
        suppressed_buttons: [bool; MAX_BUTTONS],
        button_count: usize,
    }

    pub fn list_input_devices() -> Result<Vec<InputDeviceInfo>, String> {
        let direct_input = create_direct_input()?;
        let instances = enumerate_instances(&direct_input)?;
        instances
            .into_iter()
            .map(|instance| {
                let button_count = create_device(&direct_input, &instance.device_guid)
                    .and_then(|device| button_count(&device))
                    .unwrap_or(0);

                Ok(InputDeviceInfo {
                    device_guid: format_guid(&instance.device_guid),
                    product_guid: format_guid(&instance.product_guid),
                    device_name: instance.device_name,
                    product_name: instance.product_name,
                    button_count,
                })
            })
            .collect()
    }

    pub fn capture_device_binding(timeout_ms: Option<u64>) -> Result<Option<CapturedDeviceBinding>, String> {
        let direct_input = create_direct_input()?;
        let instances = enumerate_instances(&direct_input)?;
        let mut devices = Vec::new();

        for instance in instances {
            let device = match create_device(&direct_input, &instance.device_guid) {
                Ok(device) => device,
                Err(_) => continue,
            };

            let button_count = button_count(&device).unwrap_or(0).min(128) as usize;
            if button_count == 0 {
                continue;
            }

            if configure_device(&device, button_count).is_err() {
                continue;
            }

            let Ok(previous_buttons) = read_stable_buttons(&device) else {
                continue;
            };
            let suppressed_buttons = pressed_button_mask(&previous_buttons, button_count);
            devices.push(CaptureDevice {
                info: InputDeviceInfo {
                    device_guid: format_guid(&instance.device_guid),
                    product_guid: format_guid(&instance.product_guid),
                    device_name: instance.device_name,
                    product_name: instance.product_name,
                    button_count: button_count as u32,
                },
                device,
                previous_buttons,
                suppressed_buttons,
                button_count,
            });
        }

        if devices.is_empty() {
            return Err("No DirectInput joystick devices with buttons were found".into());
        }

        let timeout = Duration::from_millis(timeout_ms.unwrap_or(DEFAULT_CAPTURE_TIMEOUT_MS));
        let started = Instant::now();
        while started.elapsed() < timeout {
            for capture_device in &mut devices {
                let state = match read_buttons(&capture_device.device) {
                    Ok(state) => state,
                    Err(_) => continue,
                };

                for index in 0..capture_device.button_count {
                    const PRESSED: u8 = 0x80;
                    const RELEASED: u8 = 0x00;
                    const BUTTON_OFFSET: u32 = 1;
                    let was_down = (capture_device.previous_buttons[index] & PRESSED) != RELEASED;
                    let is_down = (state[index] & PRESSED) != RELEASED;

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
                            button_index: button_number,
                        }));
                    }
                }

                capture_device.previous_buttons = state;
            }

            thread::sleep(Duration::from_millis(25));
        }

        Ok(None)
    }

    fn pressed_button_mask(buttons: &[u8; MAX_BUTTONS], button_count: usize) -> [bool; MAX_BUTTONS] {
        let mut mask = [false; MAX_BUTTONS];
        for index in 0..button_count.min(MAX_BUTTONS) {
            mask[index] = (buttons[index] & 0x80) != 0;
        }
        mask
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
        unsafe extern "system" fn enum_callback(instance: *mut DIDEVICEINSTANCEW, context: *mut c_void) -> windows::core::BOOL {
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

    fn create_device(direct_input: &IDirectInput8W, guid: &GUID) -> Result<IDirectInputDevice8W, String> {
        let mut device = None;
        unsafe {
            direct_input
                .CreateDevice(guid, &mut device, None::<&IUnknown>)
                .map_err(|error| error.to_string())?;
        }
        device.ok_or_else(|| "DirectInput returned no device interface".into())
    }

    fn configure_device(device: &IDirectInputDevice8W, button_count: usize) -> Result<(), String> {
        let object_count = button_count.min(MAX_BUTTONS);
        let mut object_formats = (0..object_count)
            .map(|index| DIOBJECTDATAFORMAT {
                pguid: &GUID_BUTTON,
                dwOfs: index as u32,
                dwType: DIDFT_BUTTON | ((index as u32) << 8),
                dwFlags: 0,
            })
            .collect::<Vec<_>>();

        let mut data_format = DIDATAFORMAT {
            dwSize: mem::size_of::<DIDATAFORMAT>() as u32,
            dwObjSize: mem::size_of::<DIOBJECTDATAFORMAT>() as u32,
            dwFlags: DIDF_ABSAXIS,
            dwDataSize: MAX_BUTTONS as u32,
            dwNumObjs: object_formats.len() as u32,
            rgodf: object_formats.as_mut_ptr(),
        };

        unsafe {
            device.SetDataFormat(&mut data_format).map_err(|error| error.to_string())?;
            let _ = device.SetCooperativeLevel(HWND(ptr::null_mut()), DISCL_BACKGROUND | DISCL_NONEXCLUSIVE);
            let _ = device.Acquire();
        }
        Ok(())
    }

    fn button_count(device: &IDirectInputDevice8W) -> Result<u32, String> {
        let mut caps = DIDEVCAPS::default();
        caps.dwSize = mem::size_of::<DIDEVCAPS>() as u32;
        unsafe {
            device.GetCapabilities(&mut caps).map_err(|error| error.to_string())?;
        }
        Ok(caps.dwButtons)
    }

    fn read_buttons(device: &IDirectInputDevice8W) -> windows::core::Result<[u8; MAX_BUTTONS]> {
        let mut buttons = [0; MAX_BUTTONS];
        unsafe {
            let poll_result = device.Poll();
            if poll_result.is_err() {
                let _ = device.Acquire();
                let _ = device.Poll();
            }

            match device.GetDeviceState(buttons.len() as u32, buttons.as_mut_ptr() as *mut c_void) {
                Ok(()) => Ok(buttons),
                Err(error) if error.code() == DIERR_INPUTLOST || error.code() == DIERR_NOTACQUIRED => {
                    let _ = device.Acquire();
                    device.GetDeviceState(buttons.len() as u32, buttons.as_mut_ptr() as *mut c_void)?;
                    Ok(buttons)
                }
                Err(error) => Err(error),
            }
        }
    }

    fn read_stable_buttons(device: &IDirectInputDevice8W) -> windows::core::Result<[u8; MAX_BUTTONS]> {
        let mut last_state = read_buttons(device)?;
        for _ in 0..3 {
            thread::sleep(Duration::from_millis(25));
            last_state = read_buttons(device)?;
        }
        Ok(last_state)
    }

    fn wide_array_to_string(value: &[u16]) -> String {
        let length = value.iter().position(|character| *character == 0).unwrap_or(value.len());
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
}

#[cfg(not(windows))]
mod platform {
    use super::{CapturedDeviceBinding, InputDeviceInfo};

    pub fn list_input_devices() -> Result<Vec<InputDeviceInfo>, String> {
        Ok(Vec::new())
    }

    pub fn capture_device_binding(_timeout_ms: Option<u64>) -> Result<Option<CapturedDeviceBinding>, String> {
        Err("Joystick binding capture is only available on Windows".into())
    }
}

pub use platform::{capture_device_binding, list_input_devices};
