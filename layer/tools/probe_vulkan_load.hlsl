// vectorxr_hardware_probe --gpu-ms with --graphics vulkan: the Direct3D 11 load's compute shader (probe_d3d11.cpp)
// for Vulkan, compiled to SPIR-V at build time by the pinned DXC. Each thread loops a dependent sequence
// `iterations` times, so the dispatch's time grows linearly with the iterations and hardly with the GPU's width.

struct LoadConstants {
    uint iterations;
};

[[vk::push_constant]] LoadConstants constants;
[[vk::binding(0, 0)]] RWStructuredBuffer<float> values;

[numthreads(64, 1, 1)]
void CSMain(uint3 id : SV_DispatchThreadID) {
    float v = values[id.x];
    // The values carry over from dispatch to dispatch: start again long before they could overflow (the buffer
    // starts out filled with FLT_MAX, so the first dispatch seeds it as the Direct3D 11 load's initial data does).
    if (!(abs(v) < 1.0e4)) {
        v = 1.0 + id.x * 1.0e-6;
    }
    [loop]
    for (uint i = 0; i < constants.iterations; ++i) {
        v = mad(v, 1.000001, sin(v) * 1.0e-3);
    }
    values[id.x] = v;
}
