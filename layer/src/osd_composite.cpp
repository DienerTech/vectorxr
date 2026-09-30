#include <windows.h>
#include <d3d11_1.h>
#include <d3dcompiler.h>
#include <DirectXMath.h>
#include <wrl/client.h>
#include <cmath>
#include <cstring>
#include <algorithm>
#include "depthxr/osd_composite.h"

namespace depthxr {
using Microsoft::WRL::ComPtr;
using namespace DirectX;
namespace {
struct Constants { XMFLOAT4 position[4]; XMFLOAT4 uv; };
const char* shader=R"(
cbuffer Params : register(b0) { float4 positions[4]; float4 uvRect; };
Texture2D image : register(t0);
SamplerState linearSampler : register(s0);
struct V { float4 p:SV_Position; float2 uv:TEXCOORD0; };
V VSMain(uint id:SV_VertexID) {
    uint indices[6]={0,1,2,2,1,3};
    float2 uvs[4]={float2(0,0),float2(1,0),float2(0,1),float2(1,1)};
    uint i=indices[id]; V v; v.p=positions[i]; v.uv=uvRect.xy+uvs[i]*uvRect.zw; return v;
}
float4 PSMain(V v):SV_Target { return image.Sample(linearSampler,v.uv); }
)";
XMMATRIX Pose(const XrPosef& p) {
    return XMMatrixRotationQuaternion(XMVectorSet(p.orientation.x,p.orientation.y,p.orientation.z,p.orientation.w))*
           XMMatrixTranslation(p.position.x,p.position.y,p.position.z);
}
}
struct OsdComposite::Impl {
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext1> context;
    ComPtr<ID3DDeviceContextState> state;
    ComPtr<ID3D11VertexShader> vs;
    ComPtr<ID3D11PixelShader> ps;
    ComPtr<ID3D11Buffer> buffer;
    ComPtr<ID3D11SamplerState> sampler;
    ComPtr<ID3D11BlendState> blend;
    ComPtr<ID3D11RasterizerState> raster;
    ComPtr<ID3D11Texture2D> texture;
    ComPtr<ID3D11ShaderResourceView> srv;
    std::shared_ptr<const OsdBitmap> bitmap;
    bool rgba{}, prepared{};
    Constants constants[2]{};
};
OsdComposite::OsdComposite()=default;
OsdComposite::~OsdComposite()=default;
void OsdComposite::Reset() { impl_.reset(); }
bool OsdComposite::Prepare(ID3D11Device* device, std::shared_ptr<const OsdBitmap> bitmap, bool rgba,
                           const XrPosef& panel, const XrExtent2Df& size,
                           const std::array<XrCompositionLayerProjectionView,2>& eyes, std::string& error) {
    if(impl_)impl_->prepared=false;
    if(!device || !bitmap || !bitmap->Valid() || !(size.width>0) || !(size.height>0)) {
        error="Invalid composite source";return false;
    }
    auto check=[&](HRESULT hr,const char* operation){if(SUCCEEDED(hr))return true;error=std::string(operation)+" HRESULT="+std::to_string(hr);return false;};
    if(!impl_ || impl_->device.Get()!=device) {
        auto p=std::make_unique<Impl>();p->device=device;
        ComPtr<ID3D11Device1> device1;
        ComPtr<ID3D11DeviceContext> context;
        device->GetImmediateContext(&context);
        if(!check(device->QueryInterface(IID_PPV_ARGS(&device1)),"Composite device1") ||
           !check(context.As(&p->context),"Composite context1"))return false;
        const D3D_FEATURE_LEVEL level=device->GetFeatureLevel();
        if(!check(device1->CreateDeviceContextState(0,&level,1,D3D11_SDK_VERSION,__uuidof(ID3D11Device),nullptr,&p->state),"Composite context state"))return false;
        ComPtr<ID3DBlob> vertex,pixel,diagnostics;
        if(!check(D3DCompile(shader,std::strlen(shader),"OSD composite",nullptr,nullptr,"VSMain","vs_5_0",D3DCOMPILE_ENABLE_STRICTNESS,0,&vertex,&diagnostics),"Composite VS compile") ||
           !check(D3DCompile(shader,std::strlen(shader),"OSD composite",nullptr,nullptr,"PSMain","ps_5_0",D3DCOMPILE_ENABLE_STRICTNESS,0,&pixel,&diagnostics),"Composite PS compile") ||
           !check(device->CreateVertexShader(vertex->GetBufferPointer(),vertex->GetBufferSize(),nullptr,&p->vs),"Composite VS") ||
           !check(device->CreatePixelShader(pixel->GetBufferPointer(),pixel->GetBufferSize(),nullptr,&p->ps),"Composite PS"))return false;
        D3D11_BUFFER_DESC cb{};cb.ByteWidth=sizeof(Constants);cb.BindFlags=D3D11_BIND_CONSTANT_BUFFER;
        D3D11_SAMPLER_DESC ss{};ss.Filter=D3D11_FILTER_MIN_MAG_MIP_LINEAR;ss.AddressU=ss.AddressV=ss.AddressW=D3D11_TEXTURE_ADDRESS_CLAMP;ss.MaxLOD=D3D11_FLOAT32_MAX;
        D3D11_BLEND_DESC bs{};auto& b=bs.RenderTarget[0];b.BlendEnable=TRUE;b.SrcBlend=b.SrcBlendAlpha=D3D11_BLEND_ONE;b.DestBlend=b.DestBlendAlpha=D3D11_BLEND_INV_SRC_ALPHA;b.BlendOp=b.BlendOpAlpha=D3D11_BLEND_OP_ADD;b.RenderTargetWriteMask=D3D11_COLOR_WRITE_ENABLE_ALL;
        D3D11_RASTERIZER_DESC rs{};rs.FillMode=D3D11_FILL_SOLID;rs.CullMode=D3D11_CULL_NONE;rs.DepthClipEnable=TRUE;
        if(!check(device->CreateBuffer(&cb,nullptr,&p->buffer),"Composite constants") ||
           !check(device->CreateSamplerState(&ss,&p->sampler),"Composite sampler") ||
           !check(device->CreateBlendState(&bs,&p->blend),"Composite blend") ||
           !check(device->CreateRasterizerState(&rs,&p->raster),"Composite rasterizer"))return false;
        impl_=std::move(p);
    }
    auto& p=*impl_;
    Constants constants[2]{};
    for(unsigned eye=0;eye<2;++eye) {
        const auto& f=eyes[eye].fov;
        const float left=std::tan(f.angleLeft),right=std::tan(f.angleRight),down=std::tan(f.angleDown),up=std::tan(f.angleUp);
        if(!(right>left) || !(up>down)){error="Invalid composite FOV";return false;}
        const auto transform=Pose(panel)*XMMatrixInverse(nullptr,Pose(eyes[eye].pose));
        for(unsigned corner=0;corner<4;++corner) {
            const auto point=XMVector3TransformCoord(XMVectorSet((corner%2?.5f:-.5f)*size.width,(corner<2?.5f:-.5f)*size.height,0,1),transform);
            const float x=XMVectorGetX(point),y=XMVectorGetY(point),w=-XMVectorGetZ(point);
            // OpenXR uses -Z forward; clip-space XY is derived from tangent FOV bounds.
            constants[eye].position[corner]={ (2*x-(right+left)*w)/(right-left),
                (2*y-(up+down)*w)/(up-down), .5f*w, w };
            const auto& v=constants[eye].position[corner];
            if(!std::isfinite(v.x)||!std::isfinite(v.y)||!std::isfinite(w)||w<=.001f) {
                error="Composite panel crosses eye plane";return false;
            }
        }
        constants[eye].uv={1.f/OsdBitmap::width,1.f/OsdBitmap::texture_height,
            (bitmap->content_width-2.f)/OsdBitmap::width,(bitmap->height-2.f)/OsdBitmap::texture_height};
    }
    if(!p.texture || p.rgba!=rgba) {
        D3D11_TEXTURE2D_DESC td{};td.Width=OsdBitmap::width;td.Height=OsdBitmap::texture_height;td.MipLevels=td.ArraySize=td.SampleDesc.Count=1;
        td.Format=rgba?DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;td.BindFlags=D3D11_BIND_SHADER_RESOURCE;
        ComPtr<ID3D11Texture2D> texture;ComPtr<ID3D11ShaderResourceView> srv;
        if(!check(device->CreateTexture2D(&td,nullptr,&texture),"Composite texture") ||
           !check(device->CreateShaderResourceView(texture.Get(),nullptr,&srv),"Composite texture view"))return false;
        p.texture=std::move(texture);p.srv=std::move(srv);p.rgba=rgba;p.bitmap.reset();
    }
    if(p.bitmap!=bitmap) {
        std::vector<std::uint32_t> pixels(OsdBitmap::width*OsdBitmap::texture_height);
        std::copy(bitmap->pixels.begin(),bitmap->pixels.end(),pixels.begin());
        p.context->UpdateSubresource(p.texture.Get(),0,nullptr,pixels.data(),OsdBitmap::width*4,0);
        p.bitmap=std::move(bitmap);
    }
    if(!check(device->GetDeviceRemovedReason(),"Composite device"))return false;
    std::copy(std::begin(constants),std::end(constants),std::begin(p.constants));p.prepared=true;return true;
}
void OsdComposite::Draw(unsigned eye, ID3D11RenderTargetView* target, unsigned width, unsigned height) {
    if(!impl_ || !impl_->prepared || eye>1 || !target || !width || !height)return;
    auto& p=*impl_;ComPtr<ID3DDeviceContextState> previous;
    p.context->SwapDeviceContextState(p.state.Get(),&previous);
    p.context->ClearState();
    p.context->UpdateSubresource(p.buffer.Get(),0,nullptr,&p.constants[eye],0,0);
    p.context->OMSetRenderTargets(1,&target,nullptr);
    p.context->OMSetBlendState(p.blend.Get(),nullptr,0xffffffff);
    p.context->RSSetState(p.raster.Get());
    const D3D11_VIEWPORT viewport{0,0,static_cast<float>(width),static_cast<float>(height),0,1};
    p.context->RSSetViewports(1,&viewport);
    p.context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    p.context->VSSetShader(p.vs.Get(),nullptr,0);p.context->VSSetConstantBuffers(0,1,p.buffer.GetAddressOf());
    p.context->PSSetShader(p.ps.Get(),nullptr,0);p.context->PSSetShaderResources(0,1,p.srv.GetAddressOf());
    p.context->PSSetSamplers(0,1,p.sampler.GetAddressOf());
    p.context->Draw(6,0);
    p.context->ClearState();
    p.context->SwapDeviceContextState(previous.Get(),nullptr);
}
}
