#pragma once
#include <array>
#include <memory>
#include <string>
#include <openxr/openxr.h>
#include "depthxr/osd_renderer.h"

struct ID3D11Device;
struct ID3D11RenderTargetView;
namespace depthxr {
// Diagnostic: blend into layer-owned eye images before runtime composition.
// Prepare validates both eyes before Draw can modify either image.
class OsdComposite {
public:
    OsdComposite();
    ~OsdComposite();
    void Reset();
    bool Prepare(ID3D11Device*, std::shared_ptr<const OsdBitmap>, bool rgba,
                 const XrPosef& panel_in_layer, const XrExtent2Df& size,
                 const std::array<XrCompositionLayerProjectionView,2>& eyes, std::string& error);
    void Draw(unsigned eye, ID3D11RenderTargetView*, unsigned width, unsigned height);
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
