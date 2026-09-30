#include <windows.h>
#include <d3d11.h>
#include <wrl/client.h>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include "depthxr/osd_composite.h"
using namespace depthxr;
using Microsoft::WRL::ComPtr;
void Check(bool ok,const char* message) { if(!ok){std::cerr<<message<<'\n';std::exit(1);} }
struct Vec {
    double x,y,z;
    Vec operator+(Vec v) const {return {x+v.x,y+v.y,z+v.z};}
    Vec operator-(Vec v) const {return {x-v.x,y-v.y,z-v.z};}
    Vec operator*(double s) const {return {x*s,y*s,z*s};}
    double Dot(Vec v) const {return x*v.x+y*v.y+z*v.z;}
    Vec Cross(Vec v) const {return {y*v.z-z*v.y,z*v.x-x*v.z,x*v.y-y*v.x};}
};
Vec Rotate(Vec v,XrQuaternionf q) {const Vec u{q.x,q.y,q.z};return v+u.Cross(v)*(2*q.w)+u.Cross(u.Cross(v))*2;}
Vec Position(XrPosef p) {return {p.position.x,p.position.y,p.position.z};}
int main() {
    ComPtr<ID3D11Device> device;ComPtr<ID3D11DeviceContext> context;
    Check(SUCCEEDED(D3D11CreateDevice(nullptr,D3D_DRIVER_TYPE_WARP,nullptr,0,nullptr,0,D3D11_SDK_VERSION,&device,nullptr,&context)),"WARP device");
    D3D11_TEXTURE2D_DESC desc{};desc.Width=desc.Height=256;desc.MipLevels=desc.ArraySize=desc.SampleDesc.Count=1;
    desc.Format=DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;desc.BindFlags=D3D11_BIND_RENDER_TARGET;
    ComPtr<ID3D11Texture2D> target,staging;ComPtr<ID3D11RenderTargetView> rtv;
    Check(SUCCEEDED(device->CreateTexture2D(&desc,nullptr,&target)),"Target texture");
    Check(SUCCEEDED(device->CreateRenderTargetView(target.Get(),nullptr,&rtv)),"Target view");
    desc.Usage=D3D11_USAGE_STAGING;desc.BindFlags=0;desc.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
    Check(SUCCEEDED(device->CreateTexture2D(&desc,nullptr,&staging)),"Readback");
    const float background[4]={.25f,.25f,.25f,1};
    std::array<XrCompositionLayerProjectionView,2> eyes{};
    for(unsigned eye=0;eye<2;++eye) {
        eyes[eye].pose.orientation.w=1;eyes[eye].pose.position.x=eye==0?-.032f:.032f;
        eyes[eye].fov={-.785398163f,.785398163f,.785398163f,-.785398163f};
    }
    const XrPosef panel{{0,0,0,1},{0,0,-1}};
    OsdComposite composite;std::string error;
    for(bool rgba:{false,true}) {
        auto bitmap=std::make_shared<OsdBitmap>();bitmap->height=OsdBitmap::texture_height;
        // Linear red=.5, alpha=128/255, encoded as sRGB 188. R != B catches channel swaps.
        bitmap->pixels.assign(OsdBitmap::width*bitmap->height,rgba?0x800000bcu:0x80bc0000u);
        Check(composite.Prepare(device.Get(),bitmap,rgba,panel,{1,1},eyes,error),error.c_str());
        double centers[2]{};
        for(unsigned eye=0;eye<2;++eye) {
            context->ClearRenderTargetView(rtv.Get(),background);
            D3D11_VIEWPORT saved{17,19,23,29,0,1};context->RSSetViewports(1,&saved);
            composite.Draw(eye,rtv.Get(),256,256);
            D3D11_VIEWPORT actual{};UINT count=1;context->RSGetViewports(&count,&actual);
            Check(count==1 && actual.TopLeftX==17 && actual.TopLeftY==19 && actual.Width==23,"Application viewport not restored");
            context->CopyResource(staging.Get(),target.Get());D3D11_MAPPED_SUBRESOURCE map{};
            Check(SUCCEEDED(context->Map(staging.Get(),0,D3D11_MAP_READ,0,&map)),"Readback map");
            double sum=0;unsigned covered=0;
            for(unsigned y=0;y<256;++y)for(unsigned x=0;x<256;++x) {
                const auto* p=static_cast<unsigned char*>(map.pData)+y*map.RowPitch+x*4;
                Check(p[3]==255,"Opaque eye alpha changed");
                if(p[0]>180) {
                    Check(std::abs(int(p[0])-207)<=1 && std::abs(int(p[1])-99)<=1 && p[1]==p[2],"Incorrect linear premultiplied blend or channel order");
                    ++covered;sum+=x+.5;
                } else Check(p[0]==137 && p[1]==137 && p[2]==137,"Pixels outside panel changed");
            }
            context->Unmap(staging.Get(),0);
            Check(covered==128*128,"Panel projection size differs from FOV geometry");
            centers[eye]=sum/covered;
        }
        Check(std::abs(centers[0]-132)<.01 && std::abs(centers[1]-124)<.01,"Incorrect stereo parallax");
        auto behind=panel;behind.position.z=1;
        Check(!composite.Prepare(device.Get(),bitmap,rgba,behind,{1,1},eyes,error),"Behind-eye panel accepted");
        context->ClearRenderTargetView(rtv.Get(),background);composite.Draw(0,rtv.Get(),256,256);
        context->CopyResource(staging.Get(),target.Get());D3D11_MAPPED_SUBRESOURCE map{};
        Check(SUCCEEDED(context->Map(staging.Get(),0,D3D11_MAP_READ,0,&map)),"Invalid preparation readback");
        const auto* center=static_cast<unsigned char*>(map.pData)+128*map.RowPitch+128*4;
        Check(center[0]==137 && center[1]==137 && center[2]==137,"Failed preparation still drew old panel");context->Unmap(staging.Get(),0);
    }
    // Independent ray/plane intersections verify rotated panels in all four
    // quadrants with asymmetric FOVs and canted, displaced eyes.
    auto bitmap=std::make_shared<OsdBitmap>();bitmap->height=OsdBitmap::texture_height;
    bitmap->pixels.assign(OsdBitmap::width*bitmap->height,0x800000bcu);
    for(int horizontal:{-1,1})for(int vertical:{-1,1}) {
        const float yaw=horizontal*.32f,pitch=vertical*.21f;
        XrPosef placed{{std::cos(yaw/2)*std::sin(pitch/2),std::sin(yaw/2)*std::cos(pitch/2),
            -std::sin(yaw/2)*std::sin(pitch/2),std::cos(yaw/2)*std::cos(pitch/2)},
            {horizontal*.3f,vertical*.2f,-1}};
        for(unsigned eye=0;eye<2;++eye) {
            const float cant=eye==0?-.08f:.08f;
            eyes[eye].pose.orientation={0,std::sin(cant/2),0,std::cos(cant/2)};
            eyes[eye].fov={-.72f,.81f,.75f,-.68f};
        }
        Check(composite.Prepare(device.Get(),bitmap,true,placed,{.8f,.5f},eyes,error),error.c_str());
        const Vec normal=Rotate({0,0,1},placed.orientation),right=Rotate({1,0,0},placed.orientation),up=Rotate({0,1,0},placed.orientation);
        for(unsigned eye=0;eye<2;++eye) {
            context->ClearRenderTargetView(rtv.Get(),background);composite.Draw(eye,rtv.Get(),256,256);
            context->CopyResource(staging.Get(),target.Get());D3D11_MAPPED_SUBRESOURCE map{};
            Check(SUCCEEDED(context->Map(staging.Get(),0,D3D11_MAP_READ,0,&map)),"Rotated readback");
            const auto& f=eyes[eye].fov;unsigned checks=0,inside_count=0;
            for(unsigned y=1;y<256;y+=3)for(unsigned x=1;x<256;x+=3) {
                const double u=(x+.5)/256,v=(y+.5)/256;
                const Vec ray=Rotate({std::tan(f.angleLeft)*(1-u)+std::tan(f.angleRight)*u,
                    std::tan(f.angleUp)*(1-v)+std::tan(f.angleDown)*v,-1},eyes[eye].pose.orientation);
                const Vec origin=Position(eyes[eye].pose);
                const double t=(Position(placed)-origin).Dot(normal)/ray.Dot(normal);
                const Vec hit=origin+ray*t-Position(placed);
                const double local_x=std::abs(hit.Dot(right)),local_y=std::abs(hit.Dot(up));
                if(std::abs(local_x-.4)<.01 || std::abs(local_y-.25)<.01)continue;
                const bool inside=t>0 && local_x<.4 && local_y<.25;
                const auto* pixel=static_cast<unsigned char*>(map.pData)+y*map.RowPitch+x*4;
                Check((pixel[0]>180)==inside,"Rotated stereo footprint differs from independent ray/plane geometry");
                ++checks;inside_count+=inside;
            }
            context->Unmap(staging.Get(),0);Check(checks>6000 && inside_count>100,"Insufficient rotated coverage");
        }
    }
    composite.Reset();
    std::cout<<"OSD eye-image blend: RGBA/BGRA, linear alpha, stereo projection, untouched background, state restoration and failure isolation passed\n";
}
