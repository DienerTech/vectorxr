// Frame Generation's Vulkan bridge through the layer's own hooks
// (xrCreateApiLayerInstance on) with a Vulkan game against a stand-in runtime
// whose swapchain images are Direct3D 12 textures. Needs a GPU whose Vulkan
// device and Direct3D 12 adapter are the same (77 without one).

#include <cstdio>

int main() {
    std::printf("The Vulkan bridge's layer cases are not written yet\n");
    return 77;
}
