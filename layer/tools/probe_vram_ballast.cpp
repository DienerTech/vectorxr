// vectorxr_hardware_probe --vram-ballast-mb: committed Direct3D 12 buffers in the default heap on the session's
// adapter, made before the session and held until the end of the run (see VramBallast in probe_common.h).
#include "probe_common.h"

#include <d3d12.h>
#include <dxgi1_4.h>
#include <wrl/client.h>

#include <algorithm>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <stdexcept>

#include "test_adapter.h"

using Microsoft::WRL::ComPtr;

namespace probe {

namespace {

constexpr uint64_t kMegabyte = 1024ull * 1024ull; // as Frame Generation's log counts them
constexpr uint32_t kBufferMegabytes = 256;

bool EnvironmentIsOne(const char* name) {
    std::array<char, 8> value{};
    const DWORD length = GetEnvironmentVariableA(name, value.data(), static_cast<DWORD>(value.size()));
    return length == 1 && value[0] == '1';
}

std::string Megabytes(int64_t bytes) {
    return std::to_string(bytes / static_cast<int64_t>(kMegabyte)) + " MB";
}

class D3D12VramBallast final : public VramBallast {
public:
    D3D12VramBallast(LUID luid, uint32_t megabytes) {
        for (const char* debug : {"VECTORXR_D3D11_BRIDGE_DEBUG", "VECTORXR_VULKAN_BRIDGE_DEBUG"}) {
            if (EnvironmentIsOne(debug)) {
                throw std::runtime_error(std::string("--vram-ballast-mb cannot run with ") + debug +
                                         "=1: the bridge would enable the Direct3D 12 debug layer after the ballast's "
                                         "device exists, which removes that device");
            }
        }
        ComPtr<IDXGIFactory4> factory;
        ThrowIfFailed(CreateDXGIFactory1(IID_PPV_ARGS(&factory)), "CreateDXGIFactory1");
        ThrowIfFailed(factory->EnumAdapterByLuid(luid, IID_PPV_ARGS(&adapter_)), "EnumAdapterByLuid (VRAM ballast)");
        DXGI_ADAPTER_DESC1 desc{};
        ThrowIfFailed(adapter_->GetDesc1(&desc), "IDXGIAdapter1::GetDesc1");
        name_ = test_adapter::Utf8(desc.Description);
        const std::optional<DXGI_QUERY_VIDEO_MEMORY_INFO> before = Query();
        Report("before the VRAM ballast");

        // Direct3D 12 keeps one device per adapter in a process: Frame Generation's bridge, or a Direct3D 12 game's
        // own device, is this one. Committed resources are resident from their creation on, so they count in the
        // process's usage at once.
        ThrowIfFailed(D3D12CreateDevice(adapter_.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device_)),
                      "D3D12CreateDevice (VRAM ballast)");
        D3D12_HEAP_PROPERTIES heap{};
        heap.Type = D3D12_HEAP_TYPE_DEFAULT;
        for (const uint64_t bytes : BallastBufferBytes(megabytes)) {
            D3D12_RESOURCE_DESC buffer_desc{};
            buffer_desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
            buffer_desc.Width = bytes;
            buffer_desc.Height = 1;
            buffer_desc.DepthOrArraySize = 1;
            buffer_desc.MipLevels = 1;
            buffer_desc.SampleDesc.Count = 1;
            buffer_desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
            ComPtr<ID3D12Resource> buffer;
            const HRESULT result = device_->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &buffer_desc,
                                                                    D3D12_RESOURCE_STATE_COMMON, nullptr,
                                                                    IID_PPV_ARGS(&buffer));
            if (FAILED(result)) {
                std::ostringstream message;
                message << "--vram-ballast-mb " << megabytes << ": only " << Megabytes(static_cast<int64_t>(held_))
                        << " could be allocated on \"" << name_ << "\" (CreateCommittedResource failed with HRESULT 0x"
                        << std::hex << static_cast<uint32_t>(result) << ")";
                throw std::runtime_error(message.str());
            }
            buffer->SetName(L"VectorXR hardware probe VRAM ballast");
            buffers_.push_back(buffer);
            held_ += bytes;
        }
        std::ostringstream line;
        line << "VRAM ballast: " << Megabytes(static_cast<int64_t>(held_)) << " in " << buffers_.size()
             << " committed buffers on \"" << name_ << "\"";
        const std::optional<DXGI_QUERY_VIDEO_MEMORY_INFO> after = Query();
        if (before && after) {
            line << "; the process's usage rose by "
                 << Megabytes(static_cast<int64_t>(after->CurrentUsage) - static_cast<int64_t>(before->CurrentUsage));
        }
        std::cout << line.str() << "\n";
        Report("with the VRAM ballast");
    }

    void Report(std::string_view when) const override {
        const std::optional<DXGI_QUERY_VIDEO_MEMORY_INFO> info = Query();
        std::ostringstream line;
        line << "Video memory " << when << ": ";
        if (!info) {
            line << "QueryVideoMemoryInfo failed";
        } else {
            const int64_t usage = static_cast<int64_t>(info->CurrentUsage);
            const int64_t budget = static_cast<int64_t>(info->Budget);
            line << "usage " << Megabytes(usage) << ", budget " << Megabytes(budget) << " ("
                 << (usage <= budget ? Megabytes(budget - usage) + " to spare" : Megabytes(usage - budget) + " over")
                 << "; this process, local segment group)";
        }
        std::cout << line.str() << "\n";
    }

private:
    std::optional<DXGI_QUERY_VIDEO_MEMORY_INFO> Query() const {
        DXGI_QUERY_VIDEO_MEMORY_INFO info{};
        if (FAILED(adapter_->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &info))) {
            return std::nullopt;
        }
        return info;
    }

    ComPtr<IDXGIAdapter3> adapter_;
    std::string name_;
    ComPtr<ID3D12Device> device_;
    std::vector<ComPtr<ID3D12Resource>> buffers_;
    uint64_t held_{0};
};

} // namespace

std::vector<uint64_t> BallastBufferBytes(uint32_t megabytes) {
    std::vector<uint64_t> buffers;
    for (uint32_t left = megabytes; left > 0;) {
        const uint32_t size = std::min(left, kBufferMegabytes);
        buffers.push_back(size * kMegabyte);
        left -= size;
    }
    return buffers;
}

std::unique_ptr<VramBallast> CreateVramBallast(LUID adapter, uint32_t megabytes) {
    return std::make_unique<D3D12VramBallast>(adapter, megabytes);
}

} // namespace probe
