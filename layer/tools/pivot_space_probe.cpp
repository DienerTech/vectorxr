#include <Windows.h>
#include <d3d11.h>
#include <d3dcompiler.h>
#include <dxgi1_2.h>
#include <DirectXMath.h>
#include <wrl/client.h>

#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <exception>
#include <future>
#include <iomanip>
#include <iostream>
#include <memory>
#include <mutex>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

using Microsoft::WRL::ComPtr;
using namespace DirectX;

namespace {

constexpr XrViewConfigurationType kViewConfiguration =
    XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
constexpr float kNearZ = 0.05f;
constexpr float kFarZ = 100.0f;
constexpr uint32_t kMaximumEyeDimension = 1280;

enum class ProbeMode {
    SourceExact,
    CrossSpace,
    CrossSpaceCached,
};

struct Options {
    ProbeMode mode{ProbeMode::CrossSpace};
    double seconds{20.0};
    bool threaded_wait{false};
    bool self_test{false};
    bool help{false};
};

const char* ModeName(ProbeMode mode) {
    switch (mode) {
    case ProbeMode::SourceExact:
        return "source-exact";
    case ProbeMode::CrossSpace:
        return "cross-space";
    case ProbeMode::CrossSpaceCached:
        return "cross-space-cached";
    }
    return "unknown";
}

void PrintUsage() {
    std::cout
        << "VectorXR real-runtime Pivot space probe\n\n"
        << "Usage:\n"
        << "  vectorxr_hardware_probe [options]\n\n"
        << "Options:\n"
        << "  --mode source-exact|cross-space|cross-space-cached\n"
        << "      source-exact        Locate and submit in source LOCAL space.\n"
        << "      cross-space         Locate in source LOCAL, submit in an offset LOCAL space.\n"
        << "      cross-space-cached  Also locate the offset space at the same display time.\n"
        << "  --seconds N             Active render duration (default: 20).\n"
        << "  --threaded-wait         Overlap the next xrWaitFrame with the current xrEndFrame.\n"
        << "  --self-test             Validate argument-independent pose math without a headset.\n"
        << "  --help                  Show this help.\n\n"
        << "Press Escape in the console window to end an active run early.\n";
}

Options ParseOptions(int argc, char** argv) {
    Options options;
    for (int i = 1; i < argc; ++i) {
        const std::string_view argument = argv[i];
        if (argument == "--help" || argument == "-h") {
            options.help = true;
        } else if (argument == "--self-test") {
            options.self_test = true;
        } else if (argument == "--threaded-wait") {
            options.threaded_wait = true;
        } else if (argument == "--mode") {
            if (++i >= argc) {
                throw std::runtime_error("--mode requires a value");
            }
            const std::string_view value = argv[i];
            if (value == "source-exact") {
                options.mode = ProbeMode::SourceExact;
            } else if (value == "cross-space") {
                options.mode = ProbeMode::CrossSpace;
            } else if (value == "cross-space-cached") {
                options.mode = ProbeMode::CrossSpaceCached;
            } else {
                throw std::runtime_error("unknown --mode value: " + std::string(value));
            }
        } else if (argument == "--seconds") {
            if (++i >= argc) {
                throw std::runtime_error("--seconds requires a value");
            }
            options.seconds = std::stod(argv[i]);
            if (!std::isfinite(options.seconds) || options.seconds <= 0.0 ||
                options.seconds > 3600.0) {
                throw std::runtime_error("--seconds must be between 0 and 3600");
            }
        } else {
            throw std::runtime_error("unknown argument: " + std::string(argument));
        }
    }
    return options;
}

void ThrowIfFailed(HRESULT result, std::string_view operation) {
    if (FAILED(result)) {
        std::ostringstream stream;
        stream << operation << " failed with HRESULT 0x" << std::hex
               << static_cast<uint32_t>(result);
        throw std::runtime_error(stream.str());
    }
}

void CheckXr(XrInstance instance, XrResult result, std::string_view operation) {
    if (XR_SUCCEEDED(result)) {
        return;
    }
    char result_text[XR_MAX_RESULT_STRING_SIZE]{};
    if (instance != XR_NULL_HANDLE) {
        xrResultToString(instance, result, result_text);
    }
    std::ostringstream stream;
    stream << operation << " failed: " << static_cast<int32_t>(result);
    if (result_text[0] != '\0') {
        stream << " (" << result_text << ")";
    }
    throw std::runtime_error(stream.str());
}

XrPosef IdentityPose() {
    return {{0.0f, 0.0f, 0.0f, 1.0f}, {0.0f, 0.0f, 0.0f}};
}

XrQuaternionf NormalizeQuaternion(const XrQuaternionf& quaternion) {
    const float magnitude = std::sqrt(
        quaternion.x * quaternion.x + quaternion.y * quaternion.y +
        quaternion.z * quaternion.z + quaternion.w * quaternion.w);
    if (magnitude <= 1.0e-8f) {
        return IdentityPose().orientation;
    }
    return {quaternion.x / magnitude, quaternion.y / magnitude,
            quaternion.z / magnitude, quaternion.w / magnitude};
}

XrQuaternionf MultiplyQuaternion(const XrQuaternionf& lhs,
                                 const XrQuaternionf& rhs) {
    return NormalizeQuaternion({
        lhs.w * rhs.x + lhs.x * rhs.w + lhs.y * rhs.z - lhs.z * rhs.y,
        lhs.w * rhs.y - lhs.x * rhs.z + lhs.y * rhs.w + lhs.z * rhs.x,
        lhs.w * rhs.z + lhs.x * rhs.y - lhs.y * rhs.x + lhs.z * rhs.w,
        lhs.w * rhs.w - lhs.x * rhs.x - lhs.y * rhs.y - lhs.z * rhs.z,
    });
}

XrVector3f RotateVector(const XrQuaternionf& rotation, const XrVector3f& vector) {
    const XMVECTOR quaternion = XMVectorSet(
        rotation.x, rotation.y, rotation.z, rotation.w);
    const XMVECTOR source = XMVectorSet(vector.x, vector.y, vector.z, 0.0f);
    XMFLOAT3 rotated{};
    XMStoreFloat3(&rotated, XMVector3Rotate(source, quaternion));
    return {rotated.x, rotated.y, rotated.z};
}

// Conventional rigid composition: parent_in_world * local_in_parent.
XrPosef ComposePose(const XrPosef& parent, const XrPosef& local) {
    const XrVector3f rotated = RotateVector(parent.orientation, local.position);
    return {
        MultiplyQuaternion(parent.orientation, local.orientation),
        {parent.position.x + rotated.x,
         parent.position.y + rotated.y,
         parent.position.z + rotated.z},
    };
}

XrPosef InvertPose(const XrPosef& pose) {
    const XrQuaternionf inverse_rotation =
        NormalizeQuaternion({-pose.orientation.x, -pose.orientation.y,
                             -pose.orientation.z, pose.orientation.w});
    const XrVector3f inverse_position = RotateVector(
        inverse_rotation,
        {-pose.position.x, -pose.position.y, -pose.position.z});
    return {inverse_rotation, inverse_position};
}

bool NearlyEqual(float lhs, float rhs, float epsilon = 1.0e-4f) {
    return std::abs(lhs - rhs) <= epsilon;
}

bool NearlyIdentity(const XrPosef& pose) {
    return NearlyEqual(pose.orientation.x, 0.0f) &&
           NearlyEqual(pose.orientation.y, 0.0f) &&
           NearlyEqual(pose.orientation.z, 0.0f) &&
           NearlyEqual(std::abs(pose.orientation.w), 1.0f) &&
           NearlyEqual(pose.position.x, 0.0f) &&
           NearlyEqual(pose.position.y, 0.0f) &&
           NearlyEqual(pose.position.z, 0.0f);
}

void RunSelfTest() {
    constexpr float kPi = 3.14159265358979323846f;
    const float half_yaw = 0.5f * kPi / 2.0f;
    const XrPosef target{
        {0.0f, std::sin(half_yaw), 0.0f, std::cos(half_yaw)},
        {1.25f, 0.35f, -0.75f},
    };
    const XrPosef view{
        NormalizeQuaternion({0.11f, -0.23f, 0.07f, 0.96f}),
        {-0.4f, 1.6f, -2.5f},
    };
    const XrPosef transformed = ComposePose(target, view);
    const XrPosef round_trip = ComposePose(InvertPose(target), transformed);
    const XrPosef error = ComposePose(InvertPose(view), round_trip);
    if (!NearlyIdentity(error)) {
        throw std::runtime_error("pose composition round-trip failed");
    }
    std::cout << "vectorxr_hardware_probe self-test passed\n";
}

struct Vertex {
    XMFLOAT3 position;
    XMFLOAT3 color;
};

struct SceneConstants {
    XMFLOAT4X4 view_projection;
};

class DiagnosticRenderer {
public:
    void Initialize(ID3D11Device* device, ID3D11DeviceContext* context) {
        device_ = device;
        context_ = context;

        static constexpr char kShaderSource[] = R"(
cbuffer SceneConstants : register(b0) {
    row_major float4x4 viewProjection;
};

struct VertexInput {
    float3 position : POSITION;
    float3 color : COLOR0;
};

struct VertexOutput {
    float4 position : SV_POSITION;
    float3 color : COLOR0;
};

VertexOutput VSMain(VertexInput input) {
    VertexOutput output;
    output.position = mul(float4(input.position, 1.0), viewProjection);
    output.color = input.color;
    return output;
}

float4 PSMain(VertexOutput input) : SV_TARGET {
    return float4(input.color, 1.0);
}
)";

        ComPtr<ID3DBlob> vertex_blob;
        ComPtr<ID3DBlob> pixel_blob;
        CompileShader(kShaderSource, "VSMain", "vs_5_0", &vertex_blob);
        CompileShader(kShaderSource, "PSMain", "ps_5_0", &pixel_blob);
        ThrowIfFailed(device_->CreateVertexShader(
                          vertex_blob->GetBufferPointer(), vertex_blob->GetBufferSize(),
                          nullptr, &vertex_shader_),
                      "CreateVertexShader");
        ThrowIfFailed(device_->CreatePixelShader(
                          pixel_blob->GetBufferPointer(), pixel_blob->GetBufferSize(),
                          nullptr, &pixel_shader_),
                      "CreatePixelShader");

        const D3D11_INPUT_ELEMENT_DESC elements[] = {
            {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0,
             static_cast<UINT>(offsetof(Vertex, position)),
             D3D11_INPUT_PER_VERTEX_DATA, 0},
            {"COLOR", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0,
             static_cast<UINT>(offsetof(Vertex, color)),
             D3D11_INPUT_PER_VERTEX_DATA, 0},
        };
        ThrowIfFailed(device_->CreateInputLayout(
                          elements, static_cast<UINT>(std::size(elements)),
                          vertex_blob->GetBufferPointer(), vertex_blob->GetBufferSize(),
                          &input_layout_),
                      "CreateInputLayout");

        const std::vector<Vertex> vertices = BuildScene();
        vertex_count_ = static_cast<UINT>(vertices.size());
        D3D11_BUFFER_DESC vertex_desc{};
        vertex_desc.ByteWidth = static_cast<UINT>(vertices.size() * sizeof(Vertex));
        vertex_desc.Usage = D3D11_USAGE_IMMUTABLE;
        vertex_desc.BindFlags = D3D11_BIND_VERTEX_BUFFER;
        D3D11_SUBRESOURCE_DATA vertex_data{};
        vertex_data.pSysMem = vertices.data();
        ThrowIfFailed(device_->CreateBuffer(&vertex_desc, &vertex_data, &vertex_buffer_),
                      "CreateBuffer(vertex)");

        D3D11_BUFFER_DESC constant_desc{};
        constant_desc.ByteWidth = sizeof(SceneConstants);
        constant_desc.Usage = D3D11_USAGE_DYNAMIC;
        constant_desc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        constant_desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        ThrowIfFailed(device_->CreateBuffer(&constant_desc, nullptr, &constant_buffer_),
                      "CreateBuffer(constants)");

        D3D11_DEPTH_STENCIL_DESC depth_state_desc{};
        depth_state_desc.DepthEnable = TRUE;
        depth_state_desc.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
        depth_state_desc.DepthFunc = D3D11_COMPARISON_LESS;
        ThrowIfFailed(device_->CreateDepthStencilState(&depth_state_desc, &depth_state_),
                      "CreateDepthStencilState");
    }

    void Render(ID3D11RenderTargetView* render_target,
                ID3D11DepthStencilView* depth_target,
                uint32_t width,
                uint32_t height,
                const XrPosef& view_pose,
                const XrFovf& fov,
                uint32_t eye) {
        const float clear_color[4] = {
            eye == 0 ? 0.015f : 0.025f,
            0.02f,
            eye == 0 ? 0.03f : 0.015f,
            1.0f,
        };
        context_->ClearRenderTargetView(render_target, clear_color);
        context_->ClearDepthStencilView(depth_target, D3D11_CLEAR_DEPTH, 1.0f, 0);

        const XMVECTOR orientation = XMVectorSet(
            view_pose.orientation.x, view_pose.orientation.y,
            view_pose.orientation.z, view_pose.orientation.w);
        const XMMATRIX world_from_view =
            XMMatrixRotationQuaternion(orientation) *
            XMMatrixTranslation(view_pose.position.x,
                                view_pose.position.y,
                                view_pose.position.z);
        const XMMATRIX view_from_world = XMMatrixInverse(nullptr, world_from_view);
        const float left = std::tan(fov.angleLeft) * kNearZ;
        const float right = std::tan(fov.angleRight) * kNearZ;
        const float bottom = std::tan(fov.angleDown) * kNearZ;
        const float top = std::tan(fov.angleUp) * kNearZ;
        const XMMATRIX projection = XMMatrixPerspectiveOffCenterRH(
            left, right, bottom, top, kNearZ, kFarZ);

        SceneConstants constants{};
        XMStoreFloat4x4(&constants.view_projection, view_from_world * projection);
        D3D11_MAPPED_SUBRESOURCE mapped{};
        ThrowIfFailed(context_->Map(constant_buffer_.Get(), 0,
                                    D3D11_MAP_WRITE_DISCARD, 0, &mapped),
                      "Map(constants)");
        std::memcpy(mapped.pData, &constants, sizeof(constants));
        context_->Unmap(constant_buffer_.Get(), 0);

        D3D11_VIEWPORT viewport{};
        viewport.Width = static_cast<float>(width);
        viewport.Height = static_cast<float>(height);
        viewport.MinDepth = 0.0f;
        viewport.MaxDepth = 1.0f;
        context_->RSSetViewports(1, &viewport);
        context_->OMSetRenderTargets(1, &render_target, depth_target);
        context_->OMSetDepthStencilState(depth_state_.Get(), 0);
        context_->IASetInputLayout(input_layout_.Get());
        context_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_LINELIST);
        const UINT stride = sizeof(Vertex);
        const UINT offset = 0;
        ID3D11Buffer* vertex_buffer = vertex_buffer_.Get();
        context_->IASetVertexBuffers(0, 1, &vertex_buffer, &stride, &offset);
        context_->VSSetShader(vertex_shader_.Get(), nullptr, 0);
        ID3D11Buffer* constant_buffer = constant_buffer_.Get();
        context_->VSSetConstantBuffers(0, 1, &constant_buffer);
        context_->PSSetShader(pixel_shader_.Get(), nullptr, 0);
        context_->Draw(vertex_count_, 0);
    }

private:
    static void CompileShader(const char* source,
                              const char* entry,
                              const char* profile,
                              ID3DBlob** output) {
        ComPtr<ID3DBlob> errors;
        const HRESULT result = D3DCompile(
            source, std::strlen(source), "pivot_space_probe.hlsl", nullptr, nullptr,
            entry, profile, D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, output, &errors);
        if (FAILED(result)) {
            const std::string detail = errors
                ? std::string(static_cast<const char*>(errors->GetBufferPointer()),
                              errors->GetBufferSize())
                : std::string{};
            throw std::runtime_error("D3DCompile failed for " + std::string(entry) +
                                     ": " + detail);
        }
    }

    static std::vector<Vertex> BuildScene() {
        std::vector<Vertex> vertices;
        const auto line = [&](XMFLOAT3 start, XMFLOAT3 end, XMFLOAT3 color) {
            vertices.push_back({start, color});
            vertices.push_back({end, color});
        };

        const XMFLOAT3 grid{0.18f, 0.22f, 0.27f};
        for (int x = -10; x <= 10; ++x) {
            line({static_cast<float>(x), -1.5f, -1.0f},
                 {static_cast<float>(x), -1.5f, -21.0f}, grid);
        }
        for (int z = 1; z <= 21; ++z) {
            line({-10.0f, -1.5f, -static_cast<float>(z)},
                 {10.0f, -1.5f, -static_cast<float>(z)}, grid);
        }

        const XMFLOAT3 origin{0.0f, -1.5f, -4.0f};
        line(origin, {2.5f, -1.5f, -4.0f}, {1.0f, 0.1f, 0.1f});
        line(origin, {0.0f, 1.0f, -4.0f}, {0.1f, 1.0f, 0.1f});
        line(origin, {0.0f, -1.5f, -7.0f}, {0.1f, 0.35f, 1.0f});

        const auto beacon = [&](float x, float z, XMFLOAT3 color) {
            line({x, -1.5f, z}, {x, 2.5f, z}, color);
            line({x - 0.4f, 2.1f, z}, {x + 0.4f, 2.1f, z}, color);
            line({x, 1.7f, z - 0.4f}, {x, 1.7f, z + 0.4f}, color);
        };
        beacon(-3.0f, -7.0f, {1.0f, 0.85f, 0.1f});
        beacon(4.0f, -12.0f, {1.0f, 0.1f, 0.85f});
        return vertices;
    }

    ID3D11Device* device_{nullptr};
    ID3D11DeviceContext* context_{nullptr};
    ComPtr<ID3D11VertexShader> vertex_shader_;
    ComPtr<ID3D11PixelShader> pixel_shader_;
    ComPtr<ID3D11InputLayout> input_layout_;
    ComPtr<ID3D11Buffer> vertex_buffer_;
    ComPtr<ID3D11Buffer> constant_buffer_;
    ComPtr<ID3D11DepthStencilState> depth_state_;
    UINT vertex_count_{0};
};

struct EyeSwapchain {
    XrSwapchain handle{XR_NULL_HANDLE};
    uint32_t width{0};
    uint32_t height{0};
    int64_t format{0};
    std::vector<XrSwapchainImageD3D11KHR> images;
    std::vector<ComPtr<ID3D11RenderTargetView>> render_targets;
    ComPtr<ID3D11Texture2D> depth_texture;
    ComPtr<ID3D11DepthStencilView> depth_target;
};

struct WaitedFrame {
    XrResult result{XR_SUCCESS};
    XrFrameState state{XR_TYPE_FRAME_STATE};
};

class WaitFrameWorker {
public:
    explicit WaitFrameWorker(XrSession session) : session_(session) {
        thread_ = std::thread([this] { Run(); });
    }

    ~WaitFrameWorker() {
        {
            std::scoped_lock lock(mutex_);
            stop_ = true;
        }
        condition_.notify_all();
        if (thread_.joinable()) {
            thread_.join();
        }
    }

    void Request() {
        std::scoped_lock lock(mutex_);
        if (requested_ || result_ready_) {
            throw std::runtime_error("threaded xrWaitFrame request overlapped another request");
        }
        call_started_ = false;
        requested_ = true;
        condition_.notify_all();
    }

    void WaitUntilCallStarted() {
        std::unique_lock lock(mutex_);
        condition_.wait(lock, [this] { return call_started_ || result_ready_ || stop_; });
        if (!call_started_ && !result_ready_) {
            throw std::runtime_error("threaded xrWaitFrame worker stopped before entering the call");
        }
    }

    WaitedFrame Wait() {
        std::unique_lock lock(mutex_);
        condition_.wait(lock, [this] { return result_ready_ || stop_; });
        if (!result_ready_) {
            throw std::runtime_error("threaded xrWaitFrame worker stopped without a result");
        }
        WaitedFrame result = result_;
        result_ready_ = false;
        return result;
    }

private:
    void Run() {
        for (;;) {
            {
                std::unique_lock lock(mutex_);
                condition_.wait(lock, [this] { return requested_ || stop_; });
                if (stop_) {
                    return;
                }
                requested_ = false;
                call_started_ = true;
                condition_.notify_all();
            }

            WaitedFrame waited;
            const XrFrameWaitInfo wait_info{XR_TYPE_FRAME_WAIT_INFO};
            waited.result = xrWaitFrame(session_, &wait_info, &waited.state);

            {
                std::scoped_lock lock(mutex_);
                result_ = waited;
                result_ready_ = true;
            }
            condition_.notify_all();
        }
    }

    XrSession session_{XR_NULL_HANDLE};
    std::thread thread_;
    std::mutex mutex_;
    std::condition_variable condition_;
    bool requested_{false};
    bool call_started_{false};
    bool result_ready_{false};
    bool stop_{false};
    WaitedFrame result_{};
};

class ProbeApp {
public:
    explicit ProbeApp(Options options) : options_(options) {}

    ~ProbeApp() {
        for (EyeSwapchain& swapchain : swapchains_) {
            swapchain.depth_target.Reset();
            swapchain.depth_texture.Reset();
            swapchain.render_targets.clear();
            swapchain.images.clear();
            if (swapchain.handle != XR_NULL_HANDLE) {
                xrDestroySwapchain(swapchain.handle);
            }
        }
        swapchains_.clear();
        renderer_.reset();
        context_.Reset();
        device_.Reset();
        if (target_space_ != XR_NULL_HANDLE) {
            xrDestroySpace(target_space_);
        }
        if (source_space_ != XR_NULL_HANDLE) {
            xrDestroySpace(source_space_);
        }
        if (session_ != XR_NULL_HANDLE) {
            xrDestroySession(session_);
        }
        if (instance_ != XR_NULL_HANDLE) {
            xrDestroyInstance(instance_);
        }
    }

    void Run() {
        CreateInstance();
        CreateSystemAndDevice();
        CreateSession();
        CreateSpaces();
        CreateSwapchains();
        WaitForSessionReady();

        std::cout << "Probe active: mode=" << ModeName(options_.mode)
                  << ", threadedWait=" << (options_.threaded_wait ? "yes" : "no")
                  << ", duration=" << options_.seconds << "s\n"
                  << "Use a Pivot profile with both yaw and pitch for the strongest visual test.\n";

        RenderLoop();
        RequestAndDrainExit();

        std::cout << "Probe complete: frames=" << rendered_frames_
                  << ", invalidViewFrames=" << invalid_view_frames_
                  << ", spaceRelationFailures=" << space_relation_failures_ << "\n";
    }

private:
    void CreateInstance() {
        uint32_t extension_count = 0;
        CheckXr(XR_NULL_HANDLE,
                xrEnumerateInstanceExtensionProperties(nullptr, 0, &extension_count, nullptr),
                "xrEnumerateInstanceExtensionProperties(count)");
        std::vector<XrExtensionProperties> extensions(
            extension_count, {XR_TYPE_EXTENSION_PROPERTIES});
        CheckXr(XR_NULL_HANDLE,
                xrEnumerateInstanceExtensionProperties(
                    nullptr, extension_count, &extension_count, extensions.data()),
                "xrEnumerateInstanceExtensionProperties");
        const bool has_d3d11 = std::any_of(
            extensions.begin(), extensions.end(), [](const XrExtensionProperties& extension) {
                return std::strcmp(extension.extensionName,
                                   XR_KHR_D3D11_ENABLE_EXTENSION_NAME) == 0;
            });
        if (!has_d3d11) {
            throw std::runtime_error("active OpenXR runtime does not expose XR_KHR_D3D11_enable");
        }

        const char* enabled_extensions[] = {XR_KHR_D3D11_ENABLE_EXTENSION_NAME};
        XrInstanceCreateInfo create_info{XR_TYPE_INSTANCE_CREATE_INFO};
        strcpy_s(create_info.applicationInfo.applicationName,
                 XR_MAX_APPLICATION_NAME_SIZE,
                 "VectorXR Hardware Probe");
        create_info.applicationInfo.applicationVersion = 1;
        strcpy_s(create_info.applicationInfo.engineName,
                 XR_MAX_ENGINE_NAME_SIZE,
                 "VectorXR Probe");
        create_info.applicationInfo.engineVersion = 1;
        create_info.applicationInfo.apiVersion = XR_CURRENT_API_VERSION;
        create_info.enabledExtensionCount = 1;
        create_info.enabledExtensionNames = enabled_extensions;
        CheckXr(XR_NULL_HANDLE, xrCreateInstance(&create_info, &instance_),
                "xrCreateInstance");

        XrInstanceProperties properties{XR_TYPE_INSTANCE_PROPERTIES};
        CheckXr(instance_, xrGetInstanceProperties(instance_, &properties),
                "xrGetInstanceProperties");
        std::cout << "Runtime: " << properties.runtimeName
                  << " " << XR_VERSION_MAJOR(properties.runtimeVersion) << "."
                  << XR_VERSION_MINOR(properties.runtimeVersion) << "."
                  << XR_VERSION_PATCH(properties.runtimeVersion) << "\n";
    }

    void CreateSystemAndDevice() {
        const XrSystemGetInfo system_info{
            XR_TYPE_SYSTEM_GET_INFO, nullptr, XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY};
        CheckXr(instance_, xrGetSystem(instance_, &system_info, &system_id_),
                "xrGetSystem");

        XrSystemProperties system_properties{XR_TYPE_SYSTEM_PROPERTIES};
        CheckXr(instance_, xrGetSystemProperties(instance_, system_id_, &system_properties),
                "xrGetSystemProperties");
        std::cout << "System: " << system_properties.systemName << "\n";

        PFN_xrGetD3D11GraphicsRequirementsKHR get_requirements = nullptr;
        CheckXr(instance_, xrGetInstanceProcAddr(
                               instance_, "xrGetD3D11GraphicsRequirementsKHR",
                               reinterpret_cast<PFN_xrVoidFunction*>(&get_requirements)),
                "xrGetInstanceProcAddr(xrGetD3D11GraphicsRequirementsKHR)");
        XrGraphicsRequirementsD3D11KHR requirements{
            XR_TYPE_GRAPHICS_REQUIREMENTS_D3D11_KHR};
        CheckXr(instance_, get_requirements(instance_, system_id_, &requirements),
                "xrGetD3D11GraphicsRequirementsKHR");

        ComPtr<IDXGIFactory1> factory;
        ThrowIfFailed(CreateDXGIFactory1(IID_PPV_ARGS(&factory)),
                      "CreateDXGIFactory1");
        ComPtr<IDXGIAdapter1> selected_adapter;
        for (UINT index = 0;; ++index) {
            ComPtr<IDXGIAdapter1> candidate;
            if (factory->EnumAdapters1(index, &candidate) == DXGI_ERROR_NOT_FOUND) {
                break;
            }
            DXGI_ADAPTER_DESC1 description{};
            ThrowIfFailed(candidate->GetDesc1(&description), "IDXGIAdapter1::GetDesc1");
            if (description.AdapterLuid.HighPart == requirements.adapterLuid.HighPart &&
                description.AdapterLuid.LowPart == requirements.adapterLuid.LowPart) {
                selected_adapter = candidate;
                std::wcout << L"Graphics adapter: " << description.Description << L"\n";
                break;
            }
        }
        if (!selected_adapter) {
            throw std::runtime_error("could not find the D3D11 adapter requested by the runtime");
        }

        const std::array<D3D_FEATURE_LEVEL, 7> feature_levels{
            D3D_FEATURE_LEVEL_12_1, D3D_FEATURE_LEVEL_12_0,
            D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0,
            D3D_FEATURE_LEVEL_10_1, D3D_FEATURE_LEVEL_10_0,
            D3D_FEATURE_LEVEL_9_3,
        };
        D3D_FEATURE_LEVEL created_level{};
        ThrowIfFailed(D3D11CreateDevice(
                          selected_adapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr,
                          D3D11_CREATE_DEVICE_BGRA_SUPPORT,
                          feature_levels.data(), static_cast<UINT>(feature_levels.size()),
                          D3D11_SDK_VERSION, &device_, &created_level, &context_),
                      "D3D11CreateDevice");
        if (created_level < requirements.minFeatureLevel) {
            throw std::runtime_error("created D3D11 device does not meet the runtime feature-level requirement");
        }
    }

    void CreateSession() {
        XrGraphicsBindingD3D11KHR graphics_binding{
            XR_TYPE_GRAPHICS_BINDING_D3D11_KHR};
        graphics_binding.device = device_.Get();
        XrSessionCreateInfo create_info{XR_TYPE_SESSION_CREATE_INFO};
        create_info.next = &graphics_binding;
        create_info.systemId = system_id_;
        CheckXr(instance_, xrCreateSession(instance_, &create_info, &session_),
                "xrCreateSession");

        uint32_t blend_count = 0;
        CheckXr(instance_, xrEnumerateEnvironmentBlendModes(
                               instance_, system_id_, kViewConfiguration,
                               0, &blend_count, nullptr),
                "xrEnumerateEnvironmentBlendModes(count)");
        std::vector<XrEnvironmentBlendMode> modes(blend_count);
        CheckXr(instance_, xrEnumerateEnvironmentBlendModes(
                               instance_, system_id_, kViewConfiguration,
                               blend_count, &blend_count, modes.data()),
                "xrEnumerateEnvironmentBlendModes");
        const auto opaque = std::find(
            modes.begin(), modes.end(), XR_ENVIRONMENT_BLEND_MODE_OPAQUE);
        if (modes.empty()) {
            throw std::runtime_error("runtime returned no environment blend modes");
        }
        blend_mode_ = opaque != modes.end() ? *opaque : modes.front();
    }

    void CreateSpaces() {
        XrReferenceSpaceCreateInfo source_info{
            XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
        source_info.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
        source_info.poseInReferenceSpace = IdentityPose();
        CheckXr(instance_, xrCreateReferenceSpace(session_, &source_info, &source_space_),
                "xrCreateReferenceSpace(source LOCAL)");

        constexpr float kPi = 3.14159265358979323846f;
        const float half_angle = kPi / 4.0f;
        XrReferenceSpaceCreateInfo target_info{
            XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
        target_info.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
        target_info.poseInReferenceSpace = {
            {0.0f, std::sin(half_angle), 0.0f, std::cos(half_angle)},
            {1.25f, 0.35f, -0.75f},
        };
        CheckXr(instance_, xrCreateReferenceSpace(session_, &target_info, &target_space_),
                "xrCreateReferenceSpace(offset LOCAL)");
    }

    void CreateSwapchains() {
        uint32_t view_count = 0;
        CheckXr(instance_, xrEnumerateViewConfigurationViews(
                               instance_, system_id_, kViewConfiguration,
                               0, &view_count, nullptr),
                "xrEnumerateViewConfigurationViews(count)");
        if (view_count != 2) {
            throw std::runtime_error("probe requires a two-view PRIMARY_STEREO configuration");
        }
        view_configuration_views_.assign(
            view_count, {XR_TYPE_VIEW_CONFIGURATION_VIEW});
        CheckXr(instance_, xrEnumerateViewConfigurationViews(
                               instance_, system_id_, kViewConfiguration,
                               view_count, &view_count, view_configuration_views_.data()),
                "xrEnumerateViewConfigurationViews");

        uint32_t format_count = 0;
        CheckXr(instance_, xrEnumerateSwapchainFormats(
                               session_, 0, &format_count, nullptr),
                "xrEnumerateSwapchainFormats(count)");
        std::vector<int64_t> formats(format_count);
        CheckXr(instance_, xrEnumerateSwapchainFormats(
                               session_, format_count, &format_count, formats.data()),
                "xrEnumerateSwapchainFormats");
        const std::array<int64_t, 4> preferences{
            DXGI_FORMAT_R8G8B8A8_UNORM_SRGB,
            DXGI_FORMAT_B8G8R8A8_UNORM_SRGB,
            DXGI_FORMAT_R8G8B8A8_UNORM,
            DXGI_FORMAT_B8G8R8A8_UNORM,
        };
        const auto preferred = std::find_if(
            preferences.begin(), preferences.end(), [&](int64_t candidate) {
                return std::find(formats.begin(), formats.end(), candidate) != formats.end();
            });
        if (preferred == preferences.end()) {
            throw std::runtime_error("runtime exposes no supported RGBA/BGRA render-target format");
        }

        swapchains_.resize(view_count);
        for (uint32_t eye = 0; eye < view_count; ++eye) {
            EyeSwapchain& swapchain = swapchains_[eye];
            swapchain.width = std::min(
                view_configuration_views_[eye].recommendedImageRectWidth,
                kMaximumEyeDimension);
            swapchain.height = std::min(
                view_configuration_views_[eye].recommendedImageRectHeight,
                kMaximumEyeDimension);
            if (swapchain.width == 0 || swapchain.height == 0) {
                throw std::runtime_error("runtime returned a zero recommended view dimension");
            }
            swapchain.format = *preferred;

            XrSwapchainCreateInfo create_info{XR_TYPE_SWAPCHAIN_CREATE_INFO};
            create_info.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT;
            create_info.format = swapchain.format;
            create_info.sampleCount = 1;
            create_info.width = swapchain.width;
            create_info.height = swapchain.height;
            create_info.faceCount = 1;
            create_info.arraySize = 1;
            create_info.mipCount = 1;
            CheckXr(instance_, xrCreateSwapchain(session_, &create_info, &swapchain.handle),
                    "xrCreateSwapchain");

            uint32_t image_count = 0;
            CheckXr(instance_, xrEnumerateSwapchainImages(
                                   swapchain.handle, 0, &image_count, nullptr),
                    "xrEnumerateSwapchainImages(count)");
            swapchain.images.assign(image_count, {XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR});
            CheckXr(instance_, xrEnumerateSwapchainImages(
                                   swapchain.handle, image_count, &image_count,
                                   reinterpret_cast<XrSwapchainImageBaseHeader*>(
                                       swapchain.images.data())),
                    "xrEnumerateSwapchainImages");
            swapchain.render_targets.resize(image_count);
            for (uint32_t image = 0; image < image_count; ++image) {
                ThrowIfFailed(device_->CreateRenderTargetView(
                                  swapchain.images[image].texture, nullptr,
                                  &swapchain.render_targets[image]),
                              "CreateRenderTargetView");
            }

            D3D11_TEXTURE2D_DESC depth_desc{};
            depth_desc.Width = swapchain.width;
            depth_desc.Height = swapchain.height;
            depth_desc.MipLevels = 1;
            depth_desc.ArraySize = 1;
            depth_desc.Format = DXGI_FORMAT_D32_FLOAT;
            depth_desc.SampleDesc.Count = 1;
            depth_desc.Usage = D3D11_USAGE_DEFAULT;
            depth_desc.BindFlags = D3D11_BIND_DEPTH_STENCIL;
            ThrowIfFailed(device_->CreateTexture2D(
                              &depth_desc, nullptr, &swapchain.depth_texture),
                          "CreateTexture2D(depth)");
            ThrowIfFailed(device_->CreateDepthStencilView(
                              swapchain.depth_texture.Get(), nullptr,
                              &swapchain.depth_target),
                          "CreateDepthStencilView");
        }

        renderer_ = std::make_unique<DiagnosticRenderer>();
        renderer_->Initialize(device_.Get(), context_.Get());
        std::cout << "Probe swapchains: " << swapchains_[0].width << "x"
                  << swapchains_[0].height << " per eye\n";
    }

    bool PollEvents() {
        for (;;) {
            XrEventDataBuffer event{XR_TYPE_EVENT_DATA_BUFFER};
            const XrResult poll_result = xrPollEvent(instance_, &event);
            if (poll_result == XR_EVENT_UNAVAILABLE) {
                break;
            }
            CheckXr(instance_, poll_result, "xrPollEvent");
            if (event.type == XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED) {
                const auto* changed =
                    reinterpret_cast<const XrEventDataSessionStateChanged*>(&event);
                session_state_ = changed->state;
                std::cout << "Session state: " << static_cast<int>(session_state_) << "\n";
                if (session_state_ == XR_SESSION_STATE_READY && !session_running_) {
                    const XrSessionBeginInfo begin_info{
                        XR_TYPE_SESSION_BEGIN_INFO, nullptr, kViewConfiguration};
                    CheckXr(instance_, xrBeginSession(session_, &begin_info),
                            "xrBeginSession");
                    session_running_ = true;
                } else if (session_state_ == XR_SESSION_STATE_STOPPING && session_running_) {
                    CheckXr(instance_, xrEndSession(session_), "xrEndSession");
                    session_running_ = false;
                } else if (session_state_ == XR_SESSION_STATE_EXITING ||
                           session_state_ == XR_SESSION_STATE_LOSS_PENDING) {
                    return false;
                }
            } else if (event.type == XR_TYPE_EVENT_DATA_INSTANCE_LOSS_PENDING) {
                return false;
            }
        }
        return true;
    }

    void WaitForSessionReady() {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(60);
        while (!session_running_) {
            if (!PollEvents()) {
                throw std::runtime_error("runtime exited before the probe session became ready");
            }
            if (std::chrono::steady_clock::now() >= deadline) {
                throw std::runtime_error(
                    "timed out waiting for the headset session; wake or wear the headset and retry");
            }
            Sleep(10);
        }
    }

    WaitedFrame WaitOnCallingThread() {
        WaitedFrame waited;
        const XrFrameWaitInfo wait_info{XR_TYPE_FRAME_WAIT_INFO};
        waited.result = xrWaitFrame(session_, &wait_info, &waited.state);
        return waited;
    }

    bool LocateSourceViews(XrTime display_time,
                           std::array<XrView, 2>* views,
                           XrViewState* view_state) {
        *views = {XrView{XR_TYPE_VIEW}, XrView{XR_TYPE_VIEW}};
        *view_state = {XR_TYPE_VIEW_STATE};
        const XrViewLocateInfo locate_info{
            XR_TYPE_VIEW_LOCATE_INFO, nullptr, kViewConfiguration,
            display_time, source_space_};
        uint32_t count = 0;
        CheckXr(instance_, xrLocateViews(session_, &locate_info, view_state,
                                         static_cast<uint32_t>(views->size()),
                                         &count, views->data()),
                "xrLocateViews(source)");
        constexpr XrViewStateFlags kRequired =
            XR_VIEW_STATE_ORIENTATION_VALID_BIT | XR_VIEW_STATE_POSITION_VALID_BIT;
        return count == views->size() &&
               (view_state->viewStateFlags & kRequired) == kRequired;
    }

    void LocateTargetViewsForCache(XrTime display_time) {
        std::array<XrView, 2> target_views{
            XrView{XR_TYPE_VIEW}, XrView{XR_TYPE_VIEW}};
        XrViewState target_state{XR_TYPE_VIEW_STATE};
        const XrViewLocateInfo locate_info{
            XR_TYPE_VIEW_LOCATE_INFO, nullptr, kViewConfiguration,
            display_time, target_space_};
        uint32_t count = 0;
        CheckXr(instance_, xrLocateViews(session_, &locate_info, &target_state,
                                         static_cast<uint32_t>(target_views.size()),
                                         &count, target_views.data()),
                "xrLocateViews(offset target cache)");
        if (count != target_views.size()) {
            throw std::runtime_error("target-space xrLocateViews returned an unexpected view count");
        }
    }

    std::optional<XrPosef> LocateSourceInTarget(XrTime display_time) {
        XrSpaceLocation relation{XR_TYPE_SPACE_LOCATION};
        const XrResult result = xrLocateSpace(
            source_space_, target_space_, display_time, &relation);
        constexpr XrSpaceLocationFlags kRequired =
            XR_SPACE_LOCATION_ORIENTATION_VALID_BIT |
            XR_SPACE_LOCATION_POSITION_VALID_BIT;
        if (XR_FAILED(result) || (relation.locationFlags & kRequired) != kRequired) {
            ++space_relation_failures_;
            return std::nullopt;
        }
        return relation.pose;
    }

    void RenderEye(uint32_t eye, const XrView& view) {
        EyeSwapchain& swapchain = swapchains_[eye];
        uint32_t image_index = 0;
        const XrSwapchainImageAcquireInfo acquire_info{
            XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
        CheckXr(instance_, xrAcquireSwapchainImage(
                               swapchain.handle, &acquire_info, &image_index),
                "xrAcquireSwapchainImage");
        const XrSwapchainImageWaitInfo wait_info{
            XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO, nullptr, XR_INFINITE_DURATION};
        CheckXr(instance_, xrWaitSwapchainImage(swapchain.handle, &wait_info),
                "xrWaitSwapchainImage");
        renderer_->Render(swapchain.render_targets[image_index].Get(),
                          swapchain.depth_target.Get(),
                          swapchain.width, swapchain.height,
                          view.pose, view.fov, eye);
        const XrSwapchainImageReleaseInfo release_info{
            XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
        CheckXr(instance_, xrReleaseSwapchainImage(
                               swapchain.handle, &release_info),
                "xrReleaseSwapchainImage");
    }

    void RenderLoop() {
        std::unique_ptr<WaitFrameWorker> wait_worker;
        if (options_.threaded_wait) {
            wait_worker = std::make_unique<WaitFrameWorker>(session_);
            wait_worker->Request();
        }
        WaitedFrame waited = options_.threaded_wait
            ? wait_worker->Wait()
            : WaitOnCallingThread();

        const auto started = std::chrono::steady_clock::now();
        bool exit_requested = false;
        while (session_running_ && !exit_requested) {
            CheckXr(instance_, waited.result, "xrWaitFrame");
            const XrFrameBeginInfo begin_info{XR_TYPE_FRAME_BEGIN_INFO};
            CheckXr(instance_, xrBeginFrame(session_, &begin_info), "xrBeginFrame");

            std::array<XrView, 2> source_views{
                XrView{XR_TYPE_VIEW}, XrView{XR_TYPE_VIEW}};
            XrViewState view_state{XR_TYPE_VIEW_STATE};
            std::array<XrCompositionLayerProjectionView, 2> projection_views{
                XrCompositionLayerProjectionView{XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW},
                XrCompositionLayerProjectionView{XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW},
            };
            bool submit_projection = false;
            XrSpace projection_space = source_space_;

            if (waited.state.shouldRender == XR_TRUE &&
                LocateSourceViews(waited.state.predictedDisplayTime,
                                  &source_views, &view_state)) {
                if (options_.mode == ProbeMode::CrossSpaceCached) {
                    LocateTargetViewsForCache(waited.state.predictedDisplayTime);
                }

                std::optional<XrPosef> source_in_target;
                if (options_.mode != ProbeMode::SourceExact) {
                    source_in_target = LocateSourceInTarget(
                        waited.state.predictedDisplayTime);
                    projection_space = target_space_;
                }

                if (options_.mode == ProbeMode::SourceExact || source_in_target.has_value()) {
                    for (uint32_t eye = 0; eye < source_views.size(); ++eye) {
                        RenderEye(eye, source_views[eye]);
                        projection_views[eye].pose =
                            source_in_target.has_value()
                                ? ComposePose(*source_in_target, source_views[eye].pose)
                                : source_views[eye].pose;
                        projection_views[eye].fov = source_views[eye].fov;
                        projection_views[eye].subImage.swapchain =
                            swapchains_[eye].handle;
                        projection_views[eye].subImage.imageRect = {
                            {0, 0},
                            {static_cast<int32_t>(swapchains_[eye].width),
                             static_cast<int32_t>(swapchains_[eye].height)},
                        };
                        projection_views[eye].subImage.imageArrayIndex = 0;
                    }
                    submit_projection = true;
                }
            } else if (waited.state.shouldRender == XR_TRUE) {
                ++invalid_view_frames_;
            }

            XrCompositionLayerProjection projection{
                XR_TYPE_COMPOSITION_LAYER_PROJECTION};
            projection.space = projection_space;
            projection.viewCount = static_cast<uint32_t>(projection_views.size());
            projection.views = projection_views.data();
            const XrCompositionLayerBaseHeader* layers[] = {
                reinterpret_cast<const XrCompositionLayerBaseHeader*>(&projection)};

            const double elapsed = std::chrono::duration<double>(
                std::chrono::steady_clock::now() - started).count();
            exit_requested = elapsed >= options_.seconds ||
                             (GetAsyncKeyState(VK_ESCAPE) & 0x8000) != 0;
            bool next_wait_requested = false;
            if (options_.threaded_wait && !exit_requested) {
                // This is the intentional DCS-style overlap: the persistent
                // wait thread enters xrWaitFrame(N+1) while this thread is
                // about to submit xrEndFrame(N).
                wait_worker->Request();
                wait_worker->WaitUntilCallStarted();
                next_wait_requested = true;
                SwitchToThread();
            }

            XrFrameEndInfo end_info{XR_TYPE_FRAME_END_INFO};
            end_info.displayTime = waited.state.predictedDisplayTime;
            end_info.environmentBlendMode = blend_mode_;
            end_info.layerCount = submit_projection ? 1u : 0u;
            end_info.layers = submit_projection ? layers : nullptr;
            const XrResult end_result = xrEndFrame(session_, &end_info);
            ++rendered_frames_;

            if (next_wait_requested) {
                waited = wait_worker->Wait();
            }
            CheckXr(instance_, end_result, "xrEndFrame");
            if (!PollEvents()) {
                break;
            }
            if (exit_requested || !session_running_) {
                break;
            }
            if (!options_.threaded_wait) {
                waited = WaitOnCallingThread();
            }

            if (rendered_frames_ % 300 == 0) {
                std::cout << "Rendered " << rendered_frames_ << " frames\n";
            }
        }
    }

    void RequestAndDrainExit() {
        if (session_running_) {
            const XrResult request_result = xrRequestExitSession(session_);
            if (XR_FAILED(request_result) && request_result != XR_ERROR_SESSION_NOT_RUNNING) {
                CheckXr(instance_, request_result, "xrRequestExitSession");
            }
        }

        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (session_running_ && std::chrono::steady_clock::now() < deadline) {
            if (!PollEvents()) {
                break;
            }
            Sleep(10);
        }
    }

    Options options_;
    XrInstance instance_{XR_NULL_HANDLE};
    XrSystemId system_id_{XR_NULL_SYSTEM_ID};
    XrSession session_{XR_NULL_HANDLE};
    XrSpace source_space_{XR_NULL_HANDLE};
    XrSpace target_space_{XR_NULL_HANDLE};
    XrSessionState session_state_{XR_SESSION_STATE_UNKNOWN};
    XrEnvironmentBlendMode blend_mode_{XR_ENVIRONMENT_BLEND_MODE_OPAQUE};
    bool session_running_{false};
    ComPtr<ID3D11Device> device_;
    ComPtr<ID3D11DeviceContext> context_;
    std::vector<XrViewConfigurationView> view_configuration_views_;
    std::vector<EyeSwapchain> swapchains_;
    std::unique_ptr<DiagnosticRenderer> renderer_;
    uint64_t rendered_frames_{0};
    uint64_t invalid_view_frames_{0};
    uint64_t space_relation_failures_{0};
};

} // namespace

int main(int argc, char** argv) {
    try {
        const Options options = ParseOptions(argc, argv);
        if (options.help) {
            PrintUsage();
            return 0;
        }
        if (options.self_test) {
            RunSelfTest();
            return 0;
        }
        ProbeApp(options).Run();
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "vectorxr_hardware_probe: " << error.what() << "\n";
        return 1;
    }
}
