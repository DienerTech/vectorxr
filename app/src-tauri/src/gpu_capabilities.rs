// What the PC's graphics cards can do for Frame Generation, for the settings
// page: whether any of them has NVIDIA's optical flow hardware, asked of the
// NVIDIA driver on every card (not the first one listed, which may be an
// integrated GPU). The check is the layer's own: the driver creating an
// optical flow session on a Direct3D 12 device of the card. It runs once, on
// first request, and its result is kept for the app's lifetime.

use serde::Serialize;
use std::sync::OnceLock;

#[derive(Clone, Serialize)]
#[serde(rename_all = "camelCase")]
pub struct GpuAdapter {
    pub name: String,
    pub vendor_id: u32,
    pub device_id: u32,
    pub optical_flow: bool,
    // Why it has none (empty when it has it).
    pub detail: String,
}

#[derive(Clone, Serialize)]
#[serde(rename_all = "camelCase")]
pub struct GpuCapabilities {
    // False: the cards could not be listed, so nothing is known about them.
    pub probed: bool,
    pub adapters: Vec<GpuAdapter>,
    // Some card has NVIDIA's optical flow hardware.
    pub optical_flow: bool,
    pub error: String,
}

static CAPABILITIES: OnceLock<GpuCapabilities> = OnceLock::new();

pub fn load() -> GpuCapabilities {
    CAPABILITIES.get_or_init(probe).clone()
}

fn unknown(error: String) -> GpuCapabilities {
    GpuCapabilities {
        probed: false,
        adapters: Vec::new(),
        optical_flow: false,
        error,
    }
}

#[cfg(not(windows))]
fn probe() -> GpuCapabilities {
    unknown("graphics cards are only checked on Windows".into())
}

#[cfg(windows)]
fn probe() -> GpuCapabilities {
    use windows::Win32::Graphics::Dxgi::{CreateDXGIFactory1, IDXGIFactory1, DXGI_ADAPTER_FLAG_SOFTWARE, DXGI_ERROR_NOT_FOUND};

    const NVIDIA: u32 = 0x10DE;

    let factory: IDXGIFactory1 = match unsafe { CreateDXGIFactory1() } {
        Ok(factory) => factory,
        Err(error) => return unknown(format!("the graphics cards could not be listed ({error})")),
    };
    let optical_flow = windows_probe::OpticalFlowLibrary::load();
    let mut adapters = Vec::new();
    let mut error = String::new();
    for index in 0.. {
        let adapter = match unsafe { factory.EnumAdapters1(index) } {
            Ok(adapter) => adapter,
            Err(failure) if failure.code() == DXGI_ERROR_NOT_FOUND => break,
            Err(failure) => {
                error = format!("listing graphics cards stopped at card {index} ({failure})");
                break;
            }
        };
        let Ok(description) = (unsafe { adapter.GetDesc1() }) else {
            continue;
        };
        // The software rasterizer (Microsoft Basic Render Driver) is no card.
        if description.Flags & (DXGI_ADAPTER_FLAG_SOFTWARE.0 as u32) != 0 {
            continue;
        }
        let length = description.Description.iter().position(|&c| c == 0).unwrap_or(description.Description.len());
        let name = String::from_utf16_lossy(&description.Description[..length]);
        let (has, detail) = if description.VendorId != NVIDIA {
            (false, "not an NVIDIA GPU".to_string())
        } else {
            match &optical_flow {
                Ok(library) => match library.probe(&adapter) {
                    Ok(()) => (true, String::new()),
                    Err(reason) => (false, reason),
                },
                Err(reason) => (false, reason.clone()),
            }
        };
        adapters.push(GpuAdapter {
            name,
            vendor_id: description.VendorId,
            device_id: description.DeviceId,
            optical_flow: has,
            detail,
        });
    }
    let any = adapters.iter().any(|adapter| adapter.optical_flow);
    GpuCapabilities {
        probed: true,
        adapters,
        optical_flow: any,
        error,
    }
}

#[cfg(windows)]
mod windows_probe {
    use std::ffi::c_void;
    use windows::core::{s, w, Interface};
    use windows::Win32::Foundation::{FreeLibrary, HMODULE};
    use windows::Win32::Graphics::Direct3D::D3D_FEATURE_LEVEL_11_0;
    use windows::Win32::Graphics::Direct3D12::{D3D12CreateDevice, ID3D12Device};
    use windows::Win32::Graphics::Dxgi::IDXGIAdapter1;
    use windows::Win32::System::LibraryLoader::{GetProcAddress, LoadLibraryExW, LOAD_LIBRARY_SEARCH_SYSTEM32};

    // From third_party/nvidia-optical-flow (NVIDIA Optical Flow SDK 5.0):
    // NV_OF_API_VERSION, and NV_OF_D3D12_API_FUNCTION_LIST up to nvOFDestroy.
    const NV_OF_API_VERSION: u32 = (5 << 4) | 0;
    const NV_OF_SUCCESS: i32 = 0;
    type Create = unsafe extern "system" fn(device: *mut c_void, handle: *mut *mut c_void) -> i32;
    type Destroy = unsafe extern "system" fn(handle: *mut c_void) -> i32;
    type Unused = Option<unsafe extern "system" fn()>;
    #[repr(C)]
    #[derive(Default)]
    struct FunctionList {
        create_optical_flow_d3d12: Option<Create>,
        init: Unused,
        get_surface_format_count_d3d12: Unused,
        get_surface_format_d3d12: Unused,
        register_resource_d3d12: Unused,
        unregister_resource_d3d12: Unused,
        execute_d3d12: Unused,
        destroy: Option<Destroy>,
        get_last_error: Unused,
        get_caps: Unused,
    }
    type CreateInstance = unsafe extern "system" fn(api_version: u32, functions: *mut FunctionList) -> i32;

    pub struct OpticalFlowLibrary {
        module: HMODULE,
        functions: FunctionList,
    }

    impl OpticalFlowLibrary {
        // NVIDIA's driver installs it in System32: only from there, as the layer loads it.
        pub fn load() -> Result<Self, String> {
            let module = unsafe { LoadLibraryExW(w!("nvofapi64.dll"), None, LOAD_LIBRARY_SEARCH_SYSTEM32) }
                .map_err(|_| "the NVIDIA driver's optical flow library (nvofapi64.dll) is not installed".to_string())?;
            // Freed when this returns an error (Drop).
            let mut library = Self {
                module,
                functions: FunctionList::default(),
            };
            let Some(entry) = (unsafe { GetProcAddress(module, s!("NvOFAPICreateInstanceD3D12")) }) else {
                return Err("the NVIDIA driver's optical flow library has no Direct3D 12 interface".into());
            };
            let create_instance: CreateInstance = unsafe { std::mem::transmute(entry) };
            let status = unsafe { create_instance(NV_OF_API_VERSION, &mut library.functions) };
            if status != NV_OF_SUCCESS
                || library.functions.create_optical_flow_d3d12.is_none()
                || library.functions.destroy.is_none()
            {
                return Err(format!("the NVIDIA driver does not offer optical flow API 5.0 (status {status})"));
            }
            Ok(library)
        }

        // The driver makes an optical flow session on a Direct3D 12 device of
        // the card only when the card has the hardware.
        pub fn probe(&self, adapter: &IDXGIAdapter1) -> Result<(), String> {
            let mut device: Option<ID3D12Device> = None;
            unsafe { D3D12CreateDevice(adapter, D3D_FEATURE_LEVEL_11_0, &mut device) }
                .map_err(|error| format!("no Direct3D 12 device could be made on it ({error})"))?;
            let device = device.ok_or_else(|| "no Direct3D 12 device could be made on it".to_string())?;
            let (Some(create), Some(destroy)) = (self.functions.create_optical_flow_d3d12, self.functions.destroy) else {
                return Err("the NVIDIA driver's optical flow library is incomplete".into());
            };
            let mut handle: *mut c_void = std::ptr::null_mut();
            let status = unsafe { create(device.as_raw(), &mut handle) };
            if status != NV_OF_SUCCESS || handle.is_null() {
                return Err("the NVIDIA driver offers no optical flow hardware on it".into());
            }
            unsafe { destroy(handle) };
            Ok(())
        }
    }

    impl Drop for OpticalFlowLibrary {
        fn drop(&mut self) {
            let _ = unsafe { FreeLibrary(self.module) };
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    // Runs on the machine's own cards: `cargo test gpu_capabilities -- --nocapture` lists them.
    #[test]
    fn every_card_is_checked_and_any_one_counts() {
        let capabilities = probe();
        for adapter in &capabilities.adapters {
            println!(
                "{} (vendor {:#06x}, device {:#06x}): optical flow {}{}",
                adapter.name,
                adapter.vendor_id,
                adapter.device_id,
                adapter.optical_flow,
                if adapter.detail.is_empty() { String::new() } else { format!(" ({})", adapter.detail) }
            );
            assert_eq!(adapter.optical_flow, adapter.detail.is_empty());
        }
        assert_eq!(capabilities.optical_flow, capabilities.adapters.iter().any(|adapter| adapter.optical_flow));
        if !capabilities.probed {
            assert!(capabilities.adapters.is_empty() && !capabilities.error.is_empty());
        }
    }
}
