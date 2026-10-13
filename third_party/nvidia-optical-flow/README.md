# NVIDIA Optical Flow interface headers

`nvOpticalFlowCommon.h` and `nvOpticalFlowD3D12.h`, unchanged (line endings
normalized) from the NvOFInterface folder of the
[NVIDIA Optical Flow SDK](https://developer.nvidia.com/optical-flow-sdk) 5.0.7.

Each header carries its own MIT license notice from NVIDIA Corporation, which
applies to that header file only. No other part of the SDK is included.

VectorXR's Frame Generation uses them to call NVIDIA's optical flow
accelerator on Direct3D 12. The optical flow library itself, `nvofapi64.dll`,
comes with the NVIDIA driver and is loaded at run time.
