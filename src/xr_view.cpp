#include <windows.h>
#include <d3d11.h>
#include <d3dcompiler.h>
#include <dxgi1_2.h>
#include <wrl/client.h>

#include "openxr_capture_protocol.hpp"
#include "shared_protocol.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <iomanip>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <cctype>
#include <sstream>
#include <string>
#include <thread>

using Microsoft::WRL::ComPtr;

namespace {

constexpr wchar_t kWindowClass[] = L"VRFusionXRViewWindow";
constexpr wchar_t kWindowTitle[] = L"VRFusion OpenXR Spectator";
constexpr float kPi = 3.14159265358979323846f;

std::string Trim(std::string v) {
    auto ws=[](unsigned char c){ return std::isspace(c)!=0; };
    while(!v.empty() && ws(static_cast<unsigned char>(v.front()))) v.erase(v.begin());
    while(!v.empty() && ws(static_cast<unsigned char>(v.back()))) v.pop_back();
    return v;
}

std::filesystem::path IniPath() {
    wchar_t buf[32768]{};
    DWORD n=GetModuleFileNameW(nullptr,buf,static_cast<DWORD>(std::size(buf)));
    if(n>0 && n<std::size(buf)) return std::filesystem::path(std::wstring(buf,n)).parent_path()/L"vrfusion.ini";
    return L"vrfusion.ini";
}

struct Vec3 { float x=0, y=0, z=0; };
struct Quat { float x=0, y=0, z=0, w=1; };
struct Mat3 { float m[3][3]{}; };
struct Bounds { float left=-1, right=1, down=-1, up=1; };

Quat Normalize(Quat q) {
    const float l = std::sqrt(q.x*q.x + q.y*q.y + q.z*q.z + q.w*q.w);
    if (l < 1e-8f) return {};
    const float s = 1.0f/l;
    return {q.x*s,q.y*s,q.z*s,q.w*s};
}
float Dot(Quat a, Quat b) { return a.x*b.x+a.y*b.y+a.z*b.z+a.w*b.w; }
Quat Slerp(Quat a, Quat b, float t) {
    a=Normalize(a); b=Normalize(b); float d=Dot(a,b);
    if (d < 0) { b={-b.x,-b.y,-b.z,-b.w}; d=-d; }
    if (d > .9995f) return Normalize({a.x+t*(b.x-a.x),a.y+t*(b.y-a.y),a.z+t*(b.z-a.z),a.w+t*(b.w-a.w)});
    const float th=std::acos(std::clamp(d,-1.0f,1.0f));
    const float s=std::sin(th); const float wa=std::sin((1-t)*th)/s, wb=std::sin(t*th)/s;
    return Normalize({a.x*wa+b.x*wb,a.y*wa+b.y*wb,a.z*wa+b.z*wb,a.w*wa+b.w*wb});
}
Quat Conjugate(Quat q) { return {-q.x,-q.y,-q.z,q.w}; }
float QuatAngle(Quat a, Quat b) {
    a = Normalize(a); b = Normalize(b);
    const float d = std::clamp(std::abs(Dot(a, b)), 0.0f, 1.0f);
    return 2.0f * std::acos(d);
}
Vec3 Rotate(Quat q, Vec3 v) {
    q=Normalize(q);
    const Vec3 u{q.x,q.y,q.z};
    const float s=q.w;
    const float dotuv=u.x*v.x+u.y*v.y+u.z*v.z;
    const float dotuu=u.x*u.x+u.y*u.y+u.z*u.z;
    const Vec3 cross{u.y*v.z-u.z*v.y,u.z*v.x-u.x*v.z,u.x*v.y-u.y*v.x};
    return {2*dotuv*u.x+(s*s-dotuu)*v.x+2*s*cross.x,
            2*dotuv*u.y+(s*s-dotuu)*v.y+2*s*cross.y,
            2*dotuv*u.z+(s*s-dotuu)*v.z+2*s*cross.z};
}
Mat3 MatFromQuat(Quat q) {
    q=Normalize(q); const float x=q.x,y=q.y,z=q.z,w=q.w;
    Mat3 r{};
    r.m[0][0]=1-2*(y*y+z*z); r.m[0][1]=2*(x*y-z*w); r.m[0][2]=2*(x*z+y*w);
    r.m[1][0]=2*(x*y+z*w); r.m[1][1]=1-2*(x*x+z*z); r.m[1][2]=2*(y*z-x*w);
    r.m[2][0]=2*(x*z-y*w); r.m[2][1]=2*(y*z+x*w); r.m[2][2]=1-2*(x*x+y*y);
    return r;
}
Mat3 Transpose(const Mat3& a) { Mat3 r{}; for(int i=0;i<3;i++) for(int j=0;j<3;j++) r.m[i][j]=a.m[j][i]; return r; }
Mat3 Mul(const Mat3&a,const Mat3&b){ Mat3 r{}; for(int i=0;i<3;i++)for(int j=0;j<3;j++)r.m[i][j]=a.m[i][0]*b.m[0][j]+a.m[i][1]*b.m[1][j]+a.m[i][2]*b.m[2][j]; return r; }
Vec3 Add(Vec3 a,Vec3 b){return{a.x+b.x,a.y+b.y,a.z+b.z};}
Vec3 Mul(Vec3 a,float s){return{a.x*s,a.y*s,a.z*s};}

Quat ToQuat(const vrfusion::xripc::Pose& p){ return {p.orientation[0],p.orientation[1],p.orientation[2],p.orientation[3]}; }
Vec3 ToPos(const vrfusion::xripc::Pose& p){ return {p.position[0],p.position[1],p.position[2]}; }
Bounds ToBounds(const vrfusion::xripc::Fov& f){ return {std::tan(f.angleLeft),std::tan(f.angleRight),std::tan(f.angleDown),std::tan(f.angleUp)}; }

Bounds CropAspect(Bounds b, float aspect, float zoom) {
    float cx=.5f*(b.left+b.right), cy=.5f*(b.down+b.up);
    float hw=.5f*(b.right-b.left), hh=.5f*(b.up-b.down);
    if (hw/hh > aspect) hw=hh*aspect; else hh=hw/aspect;
    hw/=std::max(1.0f,zoom); hh/=std::max(1.0f,zoom);
    return {cx-hw,cx+hw,cy-hh,cy+hh};
}

Bounds ComputeOutputBounds(const vrfusion::xripc::SharedCaptureInfo& info, Quat centerQ, float aspect, float zoom) {
    Bounds out{1e9f,-1e9f,1e9f,-1e9f};
    const Quat invCenter=Conjugate(centerQ);
    auto includeEye=[&](const vrfusion::xripc::Pose& pose,const vrfusion::xripc::Fov& f){
        const Quat q=ToQuat(pose); const Bounds b=ToBounds(f);
        const float xs[2]={b.left,b.right}, ys[2]={b.down,b.up};
        for(float x:xs) for(float y:ys){
            Vec3 layer=Rotate(q,{x,y,-1});
            Vec3 c=Rotate(invCenter,layer);
            if(c.z>=-1e-5f) continue;
            float tx=c.x/-c.z, ty=c.y/-c.z;
            out.left=std::min(out.left,tx); out.right=std::max(out.right,tx);
            out.down=std::min(out.down,ty); out.up=std::max(out.up,ty);
        }
    };
    includeEye(info.leftPose,info.leftFov); includeEye(info.rightPose,info.rightFov);
    if(!(out.left<out.right && out.down<out.up)) out={-1,1,-.5625f,.5625f};
    return CropAspect(out,aspect,zoom);
}

DXGI_FORMAT ColorSrvFormat(DXGI_FORMAT f){
    switch(f){
        case DXGI_FORMAT_R8G8B8A8_TYPELESS:return DXGI_FORMAT_R8G8B8A8_UNORM;
        case DXGI_FORMAT_B8G8R8A8_TYPELESS:return DXGI_FORMAT_B8G8R8A8_UNORM;
        case DXGI_FORMAT_B8G8R8X8_TYPELESS:return DXGI_FORMAT_B8G8R8X8_UNORM;
        case DXGI_FORMAT_R16G16B16A16_TYPELESS:return DXGI_FORMAT_R16G16B16A16_FLOAT;
        default:return f;
    }
}
DXGI_FORMAT DepthSrvFormat(DXGI_FORMAT f){
    switch(f){
        case DXGI_FORMAT_R16_TYPELESS:
        case DXGI_FORMAT_D16_UNORM:return DXGI_FORMAT_R16_UNORM;
        case DXGI_FORMAT_R24G8_TYPELESS:
        case DXGI_FORMAT_D24_UNORM_S8_UINT:return DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
        case DXGI_FORMAT_R32_TYPELESS:
        case DXGI_FORMAT_D32_FLOAT:return DXGI_FORMAT_R32_FLOAT;
        case DXGI_FORMAT_R32G8X24_TYPELESS:
        case DXGI_FORMAT_D32_FLOAT_S8X24_UINT:return DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS;
        default:return f;
    }
}
bool IsSrgb(DXGI_FORMAT f){
    return f==DXGI_FORMAT_R8G8B8A8_UNORM_SRGB || f==DXGI_FORMAT_B8G8R8A8_UNORM_SRGB || f==DXGI_FORMAT_B8G8R8X8_UNORM_SRGB;
}

struct SharedInput {
    uint64_t handle=0;
    bool depth=false;
    ComPtr<ID3D11Texture2D> shared;
    ComPtr<IDXGIKeyedMutex> mutex;
    ComPtr<ID3D11Texture2D> local;
    ComPtr<ID3D11ShaderResourceView> srv;
    uint32_t width=0,height=0;

    void Reset(){ handle=0; shared.Reset(); mutex.Reset(); local.Reset(); srv.Reset(); width=height=0; }
    bool Open(ID3D11Device* device,uint64_t newHandle,bool isDepth){
        if(handle==newHandle && shared && local && srv) return true;
        Reset(); depth=isDepth; if(!newHandle) return false;
        HANDLE h=reinterpret_cast<HANDLE>(static_cast<uintptr_t>(newHandle));
        HRESULT hr=device->OpenSharedResource(h,IID_PPV_ARGS(&shared)); if(FAILED(hr)) return false;
        hr=shared.As(&mutex); if(FAILED(hr)) {Reset();return false;}
        D3D11_TEXTURE2D_DESC sd{}; shared->GetDesc(&sd); width=sd.Width;height=sd.Height;
        D3D11_TEXTURE2D_DESC ld=sd; ld.MiscFlags=0; ld.ArraySize=1; ld.MipLevels=1; ld.BindFlags=D3D11_BIND_SHADER_RESOURCE; ld.CPUAccessFlags=0; ld.Usage=D3D11_USAGE_DEFAULT;
        hr=device->CreateTexture2D(&ld,nullptr,&local); if(FAILED(hr)){Reset();return false;}
        D3D11_SHADER_RESOURCE_VIEW_DESC sv{}; sv.Format=isDepth?DepthSrvFormat(ld.Format):ColorSrvFormat(ld.Format); sv.ViewDimension=D3D11_SRV_DIMENSION_TEXTURE2D; sv.Texture2D.MipLevels=1;
        hr=device->CreateShaderResourceView(local.Get(),&sv,&srv); if(FAILED(hr)){Reset();return false;}
        handle=newHandle; return true;
    }
    bool Refresh(ID3D11DeviceContext* ctx){
        if(!mutex||!shared||!local) return false;
        if(mutex->AcquireSync(1,0)!=S_OK) return false;
        ctx->CopyResource(local.Get(),shared.Get());
        ctx->Flush();
        mutex->ReleaseSync(0);
        return true;
    }
};

struct alignas(16) FusionParams {
    std::array<float,4> leftFov{},rightFov{},outFov{},leftUv{},rightUv{},settings{};
    std::array<float,4> left0{},left1{},left2{},right0{},right1{},right2{};
};
struct alignas(16) MeshParams {
    std::array<float,4> fov{},colorUv{},depthUv{},depthRange{};
    std::array<float,4> eyePosAlpha{},eyeQuat{},centerPos{},invCenterQuat{},outFov{},grid{};
};

class App {
public:
    bool Init(HINSTANCE inst){
        inst_=inst;
        LoadConfig();
        mapping_=OpenFileMappingW(FILE_MAP_READ,FALSE,vrfusion::xripc::kCaptureMapName);
        if(!mapping_){MessageBoxW(nullptr,L"No VRFusion OpenXR capture layer is publishing yet.\n\nEnable/install the layer, start the OpenXR game, then launch VRFusionXRView again.",L"VRFusion 0.5",MB_OK|MB_ICONINFORMATION);return false;}
        capture_=reinterpret_cast<const vrfusion::xripc::SharedCaptureInfo*>(MapViewOfFile(mapping_,FILE_MAP_READ,0,0,sizeof(vrfusion::xripc::SharedCaptureInfo)));
        if(!capture_) return false;
        event_=OpenEventW(SYNCHRONIZE,FALSE,vrfusion::xripc::kCaptureEventName);
        vrfusion::xripc::SharedCaptureInfo snap{};
        for(int i=0;i<100;i++){ if(Snapshot(snap) && (snap.flags&vrfusion::xripc::Capture_D3D11Backend)) break; std::this_thread::sleep_for(std::chrono::milliseconds(20)); }
        if(snap.magic!=vrfusion::xripc::kCaptureMagic){MessageBoxW(nullptr,L"OpenXR capture metadata is not ready yet.",L"VRFusion 0.5",MB_OK|MB_ICONWARNING);return false;}
        if(!CreateWindow()) return false;
        if(!CreateDevice(snap)) return false;
        if(!CreateTargets()) return false;
        if(!CreatePipeline()) return false;
        PublishOutput();
        const bool headless=wcsstr(GetCommandLineW(),L"--headless")!=nullptr; ShowWindow(hwnd_,headless?SW_HIDE:SW_SHOW); if(!headless) UpdateWindow(hwnd_);
        return true;
    }
    ~App(){
        if(sharedInfo_)UnmapViewOfFile(sharedInfo_); if(sharedMap_)CloseHandle(sharedMap_); if(sharedEvent_)CloseHandle(sharedEvent_);
        if(capture_)UnmapViewOfFile(capture_); if(mapping_)CloseHandle(mapping_); if(event_)CloseHandle(event_);
    }
    int Run(){
        MSG msg{}; auto lastTitle=std::chrono::steady_clock::now(); uint32_t frames=0; auto stats=lastTitle;
        while(running_){
            while(PeekMessageW(&msg,nullptr,0,0,PM_REMOVE)){ if(msg.message==WM_QUIT){running_=false;break;} TranslateMessage(&msg);DispatchMessageW(&msg); }
            if(!running_)break;
            if(event_)WaitForSingleObject(event_,8); else std::this_thread::sleep_for(std::chrono::milliseconds(4));
            vrfusion::xripc::SharedCaptureInfo info{}; if(!Snapshot(info)){continue;}
            if((info.flags&(vrfusion::xripc::Capture_LeftColorValid|vrfusion::xripc::Capture_RightColorValid)) != (vrfusion::xripc::Capture_LeftColorValid|vrfusion::xripc::Capture_RightColorValid)){SetWindowTextW(hwnd_,L"VRFusion OpenXR Spectator - waiting for both projection views");continue;}
            if(!EnsureInputs(info)) continue;
            leftColor_.Refresh(ctx_.Get()); rightColor_.Refresh(ctx_.Get());
            if(info.flags&vrfusion::xripc::Capture_LeftDepthValid) leftDepth_.Refresh(ctx_.Get());
            if(info.flags&vrfusion::xripc::Capture_RightDepthValid) rightDepth_.Refresh(ctx_.Get());
            Render(info); ++frames;
            auto now=std::chrono::steady_clock::now(); if(now-stats>=std::chrono::seconds(1)){double sec=std::chrono::duration<double>(now-stats).count();fps_=frames/sec;frames=0;stats=now;UpdateTitle(info);} 
        }
        return 0;
    }
private:
    void LoadConfig(){
        std::ifstream in(IniPath());
        if(!in) return;
        std::string line;
        while(std::getline(in,line)){
            line=Trim(line); if(line.empty()||line[0]=='#'||line[0]==';') continue;
            const auto eq=line.find('='); if(eq==std::string::npos) continue;
            std::string key=Trim(line.substr(0,eq)), value=Trim(line.substr(eq+1));
            for(char& c:key)c=static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            try{
                if(key=="width") width_=std::clamp<uint32_t>(static_cast<uint32_t>(std::stoul(value)),640u,7680u);
                else if(key=="height") height_=std::clamp<uint32_t>(static_cast<uint32_t>(std::stoul(value)),360u,4320u);
                else if(key=="zoom") zoom_=std::clamp(std::stof(value),1.0f,1.5f);
                else if(key=="stabilization") stabilization_=(value!="0"&&value!="false"&&value!="off");
                else if(key=="stabilization_ms") stabilizationMs_=std::clamp(std::stof(value),0.0f,500.0f);
                else if(key=="max_stabilization_deg") maxStabilizationDeg_=std::clamp(std::stof(value),0.0f,45.0f);
                else if(key=="openxr_mesh_step") meshStep_=std::clamp<uint32_t>(static_cast<uint32_t>(std::stoul(value)),1u,8u);
                else if(key=="openxr_depth_discontinuity_ratio") depthDiscontinuityRatio_=std::clamp(std::stof(value),1.01f,4.0f);
            }catch(...){}
        }
    }
    bool Snapshot(vrfusion::xripc::SharedCaptureInfo& out){
        if(!capture_||capture_->magic!=vrfusion::xripc::kCaptureMagic||capture_->version!=vrfusion::xripc::kCaptureVersion)return false;
        for(int i=0;i<4;i++){
            LONG64 a=capture_->sequence; if(a&1){YieldProcessor();continue;} MemoryBarrier();
            std::memcpy(&out,capture_,sizeof(out)); MemoryBarrier();
            LONG64 b=capture_->sequence; if(a==b && !(b&1)) return true;
        } return false;
    }
    bool CreateWindow(){
        WNDCLASSEXW wc{}; wc.cbSize=sizeof(wc); wc.lpfnWndProc=&WndProc; wc.hInstance=inst_; wc.hCursor=LoadCursorW(nullptr,IDC_ARROW); wc.hbrBackground=(HBRUSH)GetStockObject(BLACK_BRUSH); wc.lpszClassName=kWindowClass;
        if(!RegisterClassExW(&wc)&&GetLastError()!=ERROR_CLASS_ALREADY_EXISTS)return false;
        RECT r{0,0,(LONG)width_,(LONG)height_}; AdjustWindowRect(&r,WS_OVERLAPPEDWINDOW,FALSE);
        hwnd_=CreateWindowExW(0,kWindowClass,kWindowTitle,WS_OVERLAPPEDWINDOW,CW_USEDEFAULT,CW_USEDEFAULT,r.right-r.left,r.bottom-r.top,nullptr,nullptr,inst_,this); return hwnd_!=nullptr;
    }
    bool CreateDevice(const vrfusion::xripc::SharedCaptureInfo& info){
        ComPtr<IDXGIFactory1> fac; if(FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&fac))))return false;
        LUID want{info.adapterLuidLow,info.adapterLuidHigh};
        for(UINT i=0;;i++){ComPtr<IDXGIAdapter1>a;if(fac->EnumAdapters1(i,&a)==DXGI_ERROR_NOT_FOUND)break;DXGI_ADAPTER_DESC1 d{};a->GetDesc1(&d);if(d.AdapterLuid.LowPart==want.LowPart&&d.AdapterLuid.HighPart==want.HighPart){adapter_=a;adapterDesc_=d;break;}}
        if(!adapter_){MessageBoxW(nullptr,L"Could not find the GPU used by the OpenXR game.",L"VRFusion 0.5",MB_OK|MB_ICONERROR);return false;}
        D3D_FEATURE_LEVEL levels[]={D3D_FEATURE_LEVEL_11_1,D3D_FEATURE_LEVEL_11_0},made{}; UINT flags=D3D11_CREATE_DEVICE_BGRA_SUPPORT;
        HRESULT hr=D3D11CreateDevice(adapter_.Get(),D3D_DRIVER_TYPE_UNKNOWN,nullptr,flags,levels,2,D3D11_SDK_VERSION,&device_,&made,&ctx_);if(FAILED(hr))return false;
        ComPtr<IDXGIFactory2> f2; if(FAILED(fac.As(&f2)))return false;
        DXGI_SWAP_CHAIN_DESC1 sd{};sd.Width=width_;sd.Height=height_;sd.Format=DXGI_FORMAT_R8G8B8A8_UNORM;sd.SampleDesc.Count=1;sd.BufferUsage=DXGI_USAGE_RENDER_TARGET_OUTPUT;sd.BufferCount=2;sd.SwapEffect=DXGI_SWAP_EFFECT_FLIP_DISCARD;sd.AlphaMode=DXGI_ALPHA_MODE_IGNORE;
        ComPtr<IDXGISwapChain1>s1;hr=f2->CreateSwapChainForHwnd(device_.Get(),hwnd_,&sd,nullptr,nullptr,&s1);if(FAILED(hr))return false;hr=s1.As(&swap_);if(FAILED(hr))return false;fac->MakeWindowAssociation(hwnd_,DXGI_MWA_NO_ALT_ENTER);
        return SUCCEEDED(swap_->GetBuffer(0,IID_PPV_ARGS(&back_)));
    }
    bool CreateTargets(){
        D3D11_TEXTURE2D_DESC od{};od.Width=width_;od.Height=height_;od.MipLevels=1;od.ArraySize=1;od.Format=DXGI_FORMAT_R8G8B8A8_UNORM;od.SampleDesc.Count=1;od.BindFlags=D3D11_BIND_RENDER_TARGET|D3D11_BIND_SHADER_RESOURCE;
        if(FAILED(device_->CreateTexture2D(&od,nullptr,&output_))||FAILED(device_->CreateRenderTargetView(output_.Get(),nullptr,&outputRtv_)))return false;
        D3D11_TEXTURE2D_DESC dd=od;dd.Format=DXGI_FORMAT_D32_FLOAT;dd.BindFlags=D3D11_BIND_DEPTH_STENCIL;if(FAILED(device_->CreateTexture2D(&dd,nullptr,&depthBuffer_))||FAILED(device_->CreateDepthStencilView(depthBuffer_.Get(),nullptr,&dsv_)))return false;
        D3D11_TEXTURE2D_DESC sh=od;sh.BindFlags=D3D11_BIND_SHADER_RESOURCE;sh.MiscFlags=D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX;if(FAILED(device_->CreateTexture2D(&sh,nullptr,&sharedOut_)))return false;
        ComPtr<IDXGIResource> r;if(FAILED(sharedOut_.As(&r))||FAILED(r->GetSharedHandle(&sharedHandle_))||FAILED(sharedOut_.As(&sharedMutex_)))return false;
        return true;
    }
    bool Compile(const char* src,const char* entry,const char* profile,ID3DBlob** out){ComPtr<ID3DBlob>err;HRESULT hr=D3DCompile(src,std::strlen(src),nullptr,nullptr,nullptr,entry,profile,D3DCOMPILE_OPTIMIZATION_LEVEL3,0,out,&err);if(FAILED(hr)){if(err)OutputDebugStringA((char*)err->GetBufferPointer());return false;}return true;}
    bool CreatePipeline(){
        static const char* fusion=R"HLSL(
cbuffer P:register(b0){float4 lf,rf,of,luv,ruv,cfg;float4 l0,l1,l2,r0,r1,r2;}
Texture2D L:register(t0);Texture2D R:register(t1);SamplerState S:register(s0);
struct O{float4 p:SV_POSITION;float2 uv:TEXCOORD0;};
O VS(uint id:SV_VertexID){O o;float2 p=id==0?float2(-1,-1):(id==1?float2(-1,3):float2(3,-1));o.p=float4(p,0,1);o.uv=float2((p.x+1)*.5,1-(p.y+1)*.5);return o;}
float3 rows(float3 v,float4 a,float4 b,float4 c){return float3(dot(v,a.xyz),dot(v,b.xyz),dot(v,c.xyz));}
bool uvEye(float3 r,float4 f,float4 rect,out float2 uv){if(r.z>=-.0001){uv=0;return false;}float tx=r.x/-r.z,ty=r.y/-r.z;float2 e=float2((tx-f.x)/(f.y-f.x),(f.w-ty)/(f.w-f.z));uv=rect.xy+e*rect.zw;return all(e>=0)&&all(e<=1);}
float3 toSrgb(float3 x){x=max(x,0);float3 lo=x*12.92;float3 hi=1.055*pow(x,1.0/2.4)-0.055;return lerp(hi,lo,step(x,0.0031308));}
float4 PS(O i):SV_TARGET{float tx=lerp(of.x,of.y,i.uv.x),ty=lerp(of.w,of.z,i.uv.y);float3 ray=float3(tx,ty,-1);float2 a,b;bool va=uvEye(rows(ray,l0,l1,l2),lf,luv,a),vb=uvEye(rows(ray,r0,r1,r2),rf,ruv,b);float4 c;if(!va&&!vb)c=float4(0,0,0,1);else if(va&&!vb)c=L.Sample(S,a);else if(!va&&vb)c=R.Sample(S,b);else{float4 A=L.Sample(S,a),B=R.Sample(S,b);float diff=dot(abs(A.rgb-B.rgb),1.0/3.0);float feather=lerp(cfg.x,max(cfg.x*.12,.0005),saturate(diff*cfg.y));float w=smoothstep(cfg.z-feather,cfg.z+feather,tx);c=lerp(A,B,w);}if(cfg.w>.5)c.rgb=toSrgb(c.rgb);return c;}
)HLSL";
        static const char* mesh=R"HLSL(
cbuffer M:register(b0){float4 fov,colorUv,depthUv,dr,eyePosAlpha,eyeQ,centerPos,invCenterQ,outFov,grid;}
Texture2D D:register(t0);Texture2D C:register(t1);SamplerState PointS:register(s0);SamplerState LinearS:register(s1);
float3 qrot(float4 q,float3 v){float3 u=q.xyz;float s=q.w;return 2*dot(u,v)*u+(s*s-dot(u,u))*v+2*s*cross(u,v);}
float meters(float raw){float span=max(dr.y-dr.x,1e-7);float t=saturate((raw-dr.x)/span);float n=dr.z,f=dr.w;if(n<=0||f<=0)return -1;if(f>1e20)return n/max(1-t,1e-6);if(n>1e20)return f/max(t,1e-6);return (n*f)/max(f-t*(f-n),1e-7);}
struct V{float4 p:SV_POSITION;float2 uv:TEXCOORD0;float z:TEXCOORD1;float valid:TEXCOORD2;float2 eyeUv:TEXCOORD3;};
V VS(uint id:SV_VertexID){V o;o.valid=0;o.p=float4(0,0,2,1);o.uv=0;o.z=1e9;o.eyeUv=0;uint gw=(uint)grid.x,gh=(uint)grid.y;if(gw<2||gh<2)return o;uint q=id/6,v=id%6;uint cells=gw-1;uint x=q%cells,y=q/cells;if(y>=gh-1)return o;uint2 off=uint2(0,0);if(v==1||v==4)off=uint2(1,0);else if(v==2||v==3)off=uint2(0,1);else if(v==5)off=uint2(1,1);uint2 g=uint2(x,y)+off;float2 e=float2(g)/float2(gw-1,gh-1);float2 duv=depthUv.xy+e*depthUv.zw;float raw=D.SampleLevel(PointS,duv,0).r;float z=meters(raw);if(z<=0)return o;float tx=lerp(fov.x,fov.y,e.x),ty=lerp(fov.w,fov.z,e.y);float3 pe=float3(tx*z,ty*z,-z);float3 world=qrot(eyeQ,pe)+eyePosAlpha.xyz;float3 pc=qrot(invCenterQ,world-centerPos.xyz);if(pc.z>=-.005)return o;float cx=pc.x/-pc.z,cy=pc.y/-pc.z;if(cx<outFov.x||cx>outFov.y||cy<outFov.z||cy>outFov.w)return o;float ndx=(cx-outFov.x)/(outFov.y-outFov.x)*2-1;float ndy=(cy-outFov.z)/(outFov.w-outFov.z)*2-1;float dist=-pc.z;float zn=saturate((dist-.01)/1000.0);o.p=float4(ndx*dist,ndy*dist,zn*dist,dist);o.uv=colorUv.xy+e*colorUv.zw;o.eyeUv=e;o.z=dist;o.valid=1;return o;}
[maxvertexcount(3)]void GS(triangle V i[3],inout TriangleStream<V> s){if(i[0].valid<.5||i[1].valid<.5||i[2].valid<.5)return;float mn=min(i[0].z,min(i[1].z,i[2].z)),mx=max(i[0].z,max(i[1].z,i[2].z));if(mx/max(mn,.001)>grid.z)return;s.Append(i[0]);s.Append(i[1]);s.Append(i[2]);}
float3 toSrgb(float3 x){x=max(x,0);float3 lo=x*12.92;float3 hi=1.055*pow(x,1.0/2.4)-0.055;return lerp(hi,lo,step(x,0.0031308));}
float4 PS(V i):SV_TARGET{float4 c=C.Sample(LinearS,i.uv);if(grid.w>.5)c.rgb=toSrgb(c.rgb);float edge=min(min(i.eyeUv.x,1-i.eyeUv.x),min(i.eyeUv.y,1-i.eyeUv.y));float confidence=smoothstep(.012,.075,edge);c.a=eyePosAlpha.w*confidence;return c;}
)HLSL";
        ComPtr<ID3DBlob>a,b,c,d,e;if(!Compile(fusion,"VS","vs_5_0",&a)||!Compile(fusion,"PS","ps_5_0",&b)||!Compile(mesh,"VS","vs_5_0",&c)||!Compile(mesh,"GS","gs_5_0",&d)||!Compile(mesh,"PS","ps_5_0",&e))return false;
        if(FAILED(device_->CreateVertexShader(a->GetBufferPointer(),a->GetBufferSize(),nullptr,&fusionVs_))||FAILED(device_->CreatePixelShader(b->GetBufferPointer(),b->GetBufferSize(),nullptr,&fusionPs_))||FAILED(device_->CreateVertexShader(c->GetBufferPointer(),c->GetBufferSize(),nullptr,&meshVs_))||FAILED(device_->CreateGeometryShader(d->GetBufferPointer(),d->GetBufferSize(),nullptr,&meshGs_))||FAILED(device_->CreatePixelShader(e->GetBufferPointer(),e->GetBufferSize(),nullptr,&meshPs_)))return false;
        auto mkcb=[&](UINT n,ComPtr<ID3D11Buffer>&buf){D3D11_BUFFER_DESC bd{};bd.ByteWidth=(n+15)&~15u;bd.Usage=D3D11_USAGE_DYNAMIC;bd.BindFlags=D3D11_BIND_CONSTANT_BUFFER;bd.CPUAccessFlags=D3D11_CPU_ACCESS_WRITE;return SUCCEEDED(device_->CreateBuffer(&bd,nullptr,&buf));}; if(!mkcb(sizeof(FusionParams),fusionCb_)||!mkcb(sizeof(MeshParams),meshCb_))return false;
        D3D11_SAMPLER_DESC sl{};sl.Filter=D3D11_FILTER_MIN_MAG_MIP_LINEAR;sl.AddressU=sl.AddressV=sl.AddressW=D3D11_TEXTURE_ADDRESS_CLAMP;sl.MaxLOD=D3D11_FLOAT32_MAX;if(FAILED(device_->CreateSamplerState(&sl,&linear_)))return false;sl.Filter=D3D11_FILTER_MIN_MAG_MIP_POINT;if(FAILED(device_->CreateSamplerState(&sl,&point_)))return false;
        D3D11_BLEND_DESC bl{};bl.RenderTarget[0].BlendEnable=TRUE;bl.RenderTarget[0].SrcBlend=D3D11_BLEND_SRC_ALPHA;bl.RenderTarget[0].DestBlend=D3D11_BLEND_INV_SRC_ALPHA;bl.RenderTarget[0].BlendOp=D3D11_BLEND_OP_ADD;bl.RenderTarget[0].SrcBlendAlpha=D3D11_BLEND_ONE;bl.RenderTarget[0].DestBlendAlpha=D3D11_BLEND_ZERO;bl.RenderTarget[0].BlendOpAlpha=D3D11_BLEND_OP_ADD;bl.RenderTarget[0].RenderTargetWriteMask=D3D11_COLOR_WRITE_ENABLE_ALL;if(FAILED(device_->CreateBlendState(&bl,&blend_)))return false;
        D3D11_DEPTH_STENCIL_DESC ds{};ds.DepthEnable=TRUE;ds.DepthWriteMask=D3D11_DEPTH_WRITE_MASK_ALL;ds.DepthFunc=D3D11_COMPARISON_LESS_EQUAL;if(FAILED(device_->CreateDepthStencilState(&ds,&depthState_)))return false;
        return true;
    }
    bool UpdateBuffer(ID3D11Buffer* b,const void* data,size_t n){D3D11_MAPPED_SUBRESOURCE m{};if(FAILED(ctx_->Map(b,0,D3D11_MAP_WRITE_DISCARD,0,&m)))return false;std::memcpy(m.pData,data,n);ctx_->Unmap(b,0);return true;}
    std::array<float,4> NormRect(const vrfusion::xripc::SubImage&r,uint32_t w,uint32_t h){float fw=(float)std::max(1u,w),fh=(float)std::max(1u,h);return{r.offsetX/fw,r.offsetY/fh,r.width/fw,r.height/fh};}
    void Rows(const Mat3&m,std::array<float,4>&a,std::array<float,4>&b,std::array<float,4>&c){a={m.m[0][0],m.m[0][1],m.m[0][2],0};b={m.m[1][0],m.m[1][1],m.m[1][2],0};c={m.m[2][0],m.m[2][1],m.m[2][2],0};}
    bool EnsureInputs(const vrfusion::xripc::SharedCaptureInfo&i){
        if(!leftColor_.Open(device_.Get(),i.leftColorHandle,false)||!rightColor_.Open(device_.Get(),i.rightColorHandle,false))return false;
        if(i.flags&vrfusion::xripc::Capture_LeftDepthValid)leftDepth_.Open(device_.Get(),i.leftDepthHandle,true);else leftDepth_.Reset();
        if(i.flags&vrfusion::xripc::Capture_RightDepthValid)rightDepth_.Open(device_.Get(),i.rightDepthHandle,true);else rightDepth_.Reset();
        return true;
    }
    void DrawFusion(const vrfusion::xripc::SharedCaptureInfo&i,const Bounds&out,Quat centerQ){
        FusionParams p{};Bounds l=ToBounds(i.leftFov),r=ToBounds(i.rightFov);p.leftFov={l.left,l.right,l.down,l.up};p.rightFov={r.left,r.right,r.down,r.up};p.outFov={out.left,out.right,out.down,out.up};p.leftUv=NormRect(i.leftColorRect,leftColor_.width,leftColor_.height);p.rightUv=NormRect(i.rightColorRect,rightColor_.width,rightColor_.height);p.settings={.018f,4.0f,0.0f,IsSrgb(static_cast<DXGI_FORMAT>(i.colorFormat))?1.0f:0.0f};
        Mat3 C=MatFromQuat(centerQ),L=MatFromQuat(ToQuat(i.leftPose)),R=MatFromQuat(ToQuat(i.rightPose));Rows(Mul(Transpose(L),C),p.left0,p.left1,p.left2);Rows(Mul(Transpose(R),C),p.right0,p.right1,p.right2);UpdateBuffer(fusionCb_.Get(),&p,sizeof(p));
        ctx_->OMSetBlendState(nullptr,nullptr,0xffffffff);ctx_->OMSetDepthStencilState(nullptr,0);ctx_->VSSetShader(fusionVs_.Get(),nullptr,0);ctx_->GSSetShader(nullptr,nullptr,0);ctx_->PSSetShader(fusionPs_.Get(),nullptr,0);ID3D11ShaderResourceView* s[2]={leftColor_.srv.Get(),rightColor_.srv.Get()};ctx_->PSSetShaderResources(0,2,s);ID3D11SamplerState* sm=linear_.Get();ctx_->PSSetSamplers(0,1,&sm);ID3D11Buffer* cb=fusionCb_.Get();ctx_->PSSetConstantBuffers(0,1,&cb);ctx_->Draw(3,0);ID3D11ShaderResourceView*n[2]={nullptr,nullptr};ctx_->PSSetShaderResources(0,2,n);
    }
    void DrawDepthEye(const vrfusion::xripc::SharedCaptureInfo&i,bool left,const Bounds&out,Quat centerQ,Vec3 centerPos,float alpha){
        SharedInput& col=left?leftColor_:rightColor_;SharedInput& dep=left?leftDepth_:rightDepth_;if(!col.srv||!dep.srv)return;
        const auto& pose=left?i.leftPose:i.rightPose;const auto& fv=left?i.leftFov:i.rightFov;const auto& cr=left?i.leftColorRect:i.rightColorRect;const auto& drc=left?i.leftDepthRect:i.rightDepthRect;const auto& range=left?i.leftDepthRange:i.rightDepthRange;
        Bounds f=ToBounds(fv);MeshParams p{};p.fov={f.left,f.right,f.down,f.up};p.colorUv=NormRect(cr,col.width,col.height);p.depthUv=NormRect(drc,dep.width,dep.height);p.depthRange={range.minDepth,range.maxDepth,range.nearZ,range.farZ};Vec3 ep=ToPos(pose);Quat eq=ToQuat(pose),ic=Conjugate(centerQ);p.eyePosAlpha={ep.x,ep.y,ep.z,alpha};p.eyeQuat={eq.x,eq.y,eq.z,eq.w};p.centerPos={centerPos.x,centerPos.y,centerPos.z,0};p.invCenterQuat={ic.x,ic.y,ic.z,ic.w};p.outFov={out.left,out.right,out.down,out.up};
        uint32_t rw=std::max(2u,drc.width),rh=std::max(2u,drc.height);uint32_t gw=std::max(2u,(rw+meshStep_-1)/meshStep_+1),gh=std::max(2u,(rh+meshStep_-1)/meshStep_+1);p.grid={(float)gw,(float)gh,depthDiscontinuityRatio_,IsSrgb(static_cast<DXGI_FORMAT>(i.colorFormat))?1.0f:0.0f};UpdateBuffer(meshCb_.Get(),&p,sizeof(p));
        ctx_->OMSetBlendState(blend_.Get(),nullptr,0xffffffff);ctx_->OMSetDepthStencilState(depthState_.Get(),0);ctx_->VSSetShader(meshVs_.Get(),nullptr,0);ctx_->GSSetShader(meshGs_.Get(),nullptr,0);ctx_->PSSetShader(meshPs_.Get(),nullptr,0);ID3D11ShaderResourceView* d=dep.srv.Get();ctx_->VSSetShaderResources(0,1,&d);ID3D11ShaderResourceView* c=col.srv.Get();ctx_->PSSetShaderResources(1,1,&c);ID3D11SamplerState* ps=point_.Get();ctx_->VSSetSamplers(0,1,&ps);ID3D11SamplerState* ls=linear_.Get();ctx_->PSSetSamplers(1,1,&ls);ID3D11Buffer* cb=meshCb_.Get();ctx_->VSSetConstantBuffers(0,1,&cb);ctx_->GSSetConstantBuffers(0,1,&cb);ctx_->PSSetConstantBuffers(0,1,&cb);uint64_t verts=(uint64_t)(gw-1)*(gh-1)*6;if(verts<=0xffffffffull)ctx_->Draw((UINT)verts,0);ID3D11ShaderResourceView* null=nullptr;ctx_->VSSetShaderResources(0,1,&null);ctx_->PSSetShaderResources(1,1,&null);ctx_->GSSetShader(nullptr,nullptr,0);
    }
    Quat Stabilize(Quat target){
        target=Normalize(target);
        const auto now=std::chrono::steady_clock::now();
        if(!hasStabilized_){stabilizedQ_=target;lastStabilize_=now;hasStabilized_=true;return target;}
        float dt=std::chrono::duration<float>(now-lastStabilize_).count();lastStabilize_=now;dt=std::clamp(dt,0.0f,0.1f);
        if(!stabilization_){stabilizedQ_=target;return target;}
        const float tau=std::max(0.001f,stabilizationMs_/1000.0f);
        const float a=1.0f-std::exp(-dt/tau);
        stabilizedQ_=Slerp(stabilizedQ_,target,a);
        const float maxLag=maxStabilizationDeg_*kPi/180.0f;
        const float lag=QuatAngle(target,stabilizedQ_);
        if(lag>maxLag) stabilizedQ_=Slerp(target,stabilizedQ_,maxLag/lag);
        return stabilizedQ_;
    }
    void Render(const vrfusion::xripc::SharedCaptureInfo&i){
        LARGE_INTEGER renderStart{}; QueryPerformanceCounter(&renderStart);
        Quat lq=ToQuat(i.leftPose),rq=ToQuat(i.rightPose),rawCenterQ=Slerp(lq,rq,.5f),centerQ=Stabilize(rawCenterQ);Vec3 centerPos=Mul(Add(ToPos(i.leftPose),ToPos(i.rightPose)),.5f);Bounds out=ComputeOutputBounds(i,centerQ,(float)width_/height_,zoom_);
        float clear[4]={0,0,0,1};ctx_->ClearRenderTargetView(outputRtv_.Get(),clear);ctx_->ClearDepthStencilView(dsv_.Get(),D3D11_CLEAR_DEPTH,1.0f,0);D3D11_VIEWPORT vp{0,0,(float)width_,(float)height_,0,1};ctx_->RSSetViewports(1,&vp);ID3D11RenderTargetView* rt=outputRtv_.Get();ctx_->OMSetRenderTargets(1,&rt,dsv_.Get());ctx_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        DrawFusion(i,out,centerQ);
        bool ld=(i.flags&vrfusion::xripc::Capture_LeftDepthValid)&&leftDepth_.srv;bool rd=(i.flags&vrfusion::xripc::Capture_RightDepthValid)&&rightDepth_.srv;if(ld)DrawDepthEye(i,true,out,centerQ,centerPos,1.0f);if(rd)DrawDepthEye(i,false,out,centerQ,centerPos,ld?.5f:1.0f);
        ctx_->OMSetRenderTargets(0,nullptr,nullptr);ctx_->OMSetBlendState(nullptr,nullptr,0xffffffff);ctx_->OMSetDepthStencilState(nullptr,0);ctx_->CopyResource(back_.Get(),output_.Get());
        bool published=false;
        if(sharedMutex_&&sharedMutex_->AcquireSync(0,0)==S_OK){ctx_->CopyResource(sharedOut_.Get(),output_.Get());ctx_->Flush();sharedMutex_->ReleaseSync(1);published=true;}else if(sharedMutex_){++droppedPublishFrames_;}
        LARGE_INTEGER renderEnd{}; QueryPerformanceCounter(&renderEnd);
        if(sharedInfo_){
            InterlockedExchange64(&sharedInfo_->sourceFrameCounter, i.frameCounter);
            InterlockedExchange64(&sharedInfo_->producerQpc, renderEnd.QuadPart);
            InterlockedExchange64(&sharedInfo_->renderDurationQpc, renderEnd.QuadPart-renderStart.QuadPart);
            InterlockedExchange64(&sharedInfo_->droppedPublishFrames, static_cast<LONG64>(droppedPublishFrames_));
            sharedInfo_->flags = vrfusion::SharedFrame_ProducerAlive | vrfusion::SharedFrame_OpenXR | (((i.flags&vrfusion::xripc::Capture_DepthAwareReady)!=0)?vrfusion::SharedFrame_DepthAware:0u);
            if(published)InterlockedIncrement64(&sharedInfo_->frameCounter);
        }
        if(published&&sharedEvent_)SetEvent(sharedEvent_);swap_->Present(0,0);
    }
    void PublishOutput(){
        sharedMap_=CreateFileMappingW(INVALID_HANDLE_VALUE,nullptr,PAGE_READWRITE,0,sizeof(vrfusion::SharedFrameInfo),vrfusion::kSharedMapName);if(!sharedMap_)return;sharedInfo_=reinterpret_cast<vrfusion::SharedFrameInfo*>(MapViewOfFile(sharedMap_,FILE_MAP_ALL_ACCESS,0,0,sizeof(vrfusion::SharedFrameInfo)));if(!sharedInfo_)return;vrfusion::SharedFrameInfo x{};x.width=width_;x.height=height_;x.dxgiFormat=DXGI_FORMAT_R8G8B8A8_UNORM;x.producerPid=GetCurrentProcessId();x.adapterLuidLow=adapterDesc_.AdapterLuid.LowPart;x.adapterLuidHigh=adapterDesc_.AdapterLuid.HighPart;x.sharedHandle=(uint64_t)(uintptr_t)sharedHandle_;LARGE_INTEGER qpf{};if(QueryPerformanceFrequency(&qpf))x.qpcFrequency=qpf.QuadPart;x.flags=vrfusion::SharedFrame_ProducerAlive|vrfusion::SharedFrame_OpenXR;std::memcpy(sharedInfo_,&x,sizeof(x));sharedEvent_=CreateEventW(nullptr,FALSE,FALSE,vrfusion::kSharedEventName);
    }
    void UpdateTitle(const vrfusion::xripc::SharedCaptureInfo&i){std::wostringstream s;s<<L"VRFusion 0.5 OpenXR | "<<std::fixed<<std::setprecision(1)<<fps_<<L" FPS | "<<(((i.flags&vrfusion::xripc::Capture_DepthAwareReady)!=0)?L"DEPTH CENTER VIEW":L"COLOR FUSION FALLBACK")<<L" | STAB "<<(stabilization_?L"ON":L"OFF")<<L" | mesh "<<meshStep_<<L" | discontinuity "<<std::setprecision(2)<<depthDiscontinuityRatio_;if(sharedInfo_&&sharedInfo_->qpcFrequency>0){double ms=1000.0*double(sharedInfo_->renderDurationQpc)/double(sharedInfo_->qpcFrequency);s<<L" | "<<std::setprecision(2)<<ms<<L" ms | pubdrop "<<sharedInfo_->droppedPublishFrames;}SetWindowTextW(hwnd_,s.str().c_str());}
    static LRESULT CALLBACK WndProc(HWND h,UINT m,WPARAM w,LPARAM l){App* a=(App*)GetWindowLongPtrW(h,GWLP_USERDATA);if(m==WM_NCCREATE){auto*cs=(CREATESTRUCTW*)l;a=(App*)cs->lpCreateParams;SetWindowLongPtrW(h,GWLP_USERDATA,(LONG_PTR)a);}if(a&&m==WM_KEYDOWN){if(w==VK_ESCAPE)DestroyWindow(h);if(w=='S'){a->stabilization_=!a->stabilization_;a->hasStabilized_=false;}if(w==VK_OEM_4)a->meshStep_=std::min(8u,a->meshStep_+1);if(w==VK_OEM_6)a->meshStep_=std::max(1u,a->meshStep_-1);}if(m==WM_DESTROY){PostQuitMessage(0);return 0;}return DefWindowProcW(h,m,w,l);}

    HINSTANCE inst_{};HWND hwnd_{};bool running_=true,stabilization_=true,hasStabilized_=false;uint32_t width_=1920,height_=1080,meshStep_=3;float zoom_=1.08f,stabilizationMs_=75.0f,maxStabilizationDeg_=7.0f,depthDiscontinuityRatio_=1.25f;double fps_=0;Quat stabilizedQ_{};std::chrono::steady_clock::time_point lastStabilize_{};
    HANDLE mapping_{},event_{};const vrfusion::xripc::SharedCaptureInfo* capture_{};
    ComPtr<IDXGIAdapter1> adapter_;DXGI_ADAPTER_DESC1 adapterDesc_{};ComPtr<ID3D11Device> device_;ComPtr<ID3D11DeviceContext> ctx_;ComPtr<IDXGISwapChain2> swap_;ComPtr<ID3D11Texture2D> back_,output_,depthBuffer_;ComPtr<ID3D11RenderTargetView> outputRtv_;ComPtr<ID3D11DepthStencilView>dsv_;
    ComPtr<ID3D11VertexShader>fusionVs_,meshVs_;ComPtr<ID3D11PixelShader>fusionPs_,meshPs_;ComPtr<ID3D11GeometryShader>meshGs_;ComPtr<ID3D11Buffer>fusionCb_,meshCb_;ComPtr<ID3D11SamplerState>linear_,point_;ComPtr<ID3D11BlendState>blend_;ComPtr<ID3D11DepthStencilState>depthState_;
    SharedInput leftColor_,rightColor_,leftDepth_,rightDepth_;
    ComPtr<ID3D11Texture2D>sharedOut_;ComPtr<IDXGIKeyedMutex>sharedMutex_;HANDLE sharedHandle_{},sharedMap_{},sharedEvent_{};vrfusion::SharedFrameInfo*sharedInfo_{};uint64_t droppedPublishFrames_=0;
};

} // namespace

int WINAPI wWinMain(HINSTANCE h,HINSTANCE,LPWSTR,int){App a;if(!a.Init(h))return 1;return a.Run();}
