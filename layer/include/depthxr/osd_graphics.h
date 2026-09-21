#pragma once
#include <cstdint>
#include <memory>
#include <string>
#include <vector>
#include <openxr/openxr.h>

struct ID3D11Device;
namespace depthxr {
struct OsdBitmap;
struct OsdDispatch;
// Upload enqueues on the session's graphics queue. Busy never waits on the CPU.
enum class OsdUpload { Complete, Busy, Failed };
class OsdGraphics {
public:
    virtual ~OsdGraphics() = default;
    virtual const char* Name() const = 0;
    virtual std::vector<std::int64_t> Formats() const = 0;
    virtual bool Rgba(std::int64_t format) const = 0;
    virtual bool Images(XrSwapchain, const OsdDispatch&, std::string& error) = 0;
    virtual OsdUpload Upload(std::uint32_t, const OsdBitmap&, std::string& error) = 0;
    // Only teardown/retry may wait for this backend's outstanding copy.
    virtual void Reset() = 0;
};
std::unique_ptr<OsdGraphics> CreateOsdGraphics(const void* binding_chain, std::string& error);
std::unique_ptr<OsdGraphics> CreateOsdD3D11(ID3D11Device*);
}
