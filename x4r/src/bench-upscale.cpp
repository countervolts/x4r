// SPDX-License-Identifier: GPL-3.0-only
// Headless fixed-resolution XeSS timing; legacy 1.5x Quality matches DLSS/FSR scale.
// Run init-only and audit the shim kernel log before allowing a new preset to execute.
#include <algorithm>
#include <cstdint>
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_4.h>
#include <wrl/client.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <stdexcept>
#include <string>
#include <vector>
#include "xess/xess_d3d12.h"
#include "xess/xess_debug.h"
using Microsoft::WRL::ComPtr;
static void hr(HRESULT r) { if(FAILED(r)) throw std::runtime_error("D3D12 HRESULT="+std::to_string((unsigned)r)); }
static void xe(xess_result_t r) { if(r<0) throw std::runtime_error("XeSS result="+std::to_string(r)); }
static float halton(unsigned i,unsigned b) { float v=0,s=1; while(i){s/=b;v+=(i%b)*s;i/=b;} return v-.5f; }
int main(int argc,char** argv) try {
    if(argc!=4 && argc!=5) throw std::runtime_error("bench-upscale.exe width height quality|performance [init]");
    const UINT W=std::stoul(argv[1]),H=std::stoul(argv[2]);
    const bool quality=strcmp(argv[3],"quality")==0;
    if(!quality && strcmp(argv[3],"performance")!=0) throw std::runtime_error("Unknown preset");
    UINT IW=0,IH=0;
    ComPtr<IDXGIFactory4> factory; hr(CreateDXGIFactory2(0,IID_PPV_ARGS(&factory)));
    ComPtr<IDXGIAdapter1> adapter; hr(factory->EnumAdapters1(0,&adapter));
    DXGI_ADAPTER_DESC1 ad; hr(adapter->GetDesc1(&ad));
    printf("adapter vendor=%04x device=%04x\n",ad.VendorId,ad.DeviceId); fflush(stdout);
    ComPtr<ID3D12Device> dev; hr(D3D12CreateDevice(adapter.Get(),D3D_FEATURE_LEVEL_12_0,IID_PPV_ARGS(&dev)));
    ComPtr<ID3D12CommandQueue> queue; D3D12_COMMAND_QUEUE_DESC qd={}; hr(dev->CreateCommandQueue(&qd,IID_PPV_ARGS(&queue)));
    ComPtr<ID3D12CommandAllocator> alloc; hr(dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,IID_PPV_ARGS(&alloc)));
    ComPtr<ID3D12GraphicsCommandList> cmd; hr(dev->CreateCommandList(0,D3D12_COMMAND_LIST_TYPE_DIRECT,alloc.Get(),nullptr,IID_PPV_ARGS(&cmd))); hr(cmd->Close());
    ComPtr<ID3D12Fence> fence; hr(dev->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&fence)));
    HANDLE event=CreateEvent(nullptr,FALSE,FALSE,nullptr); UINT64 serial=0;
    auto wait=[&] {hr(queue->Signal(fence.Get(),++serial)); hr(fence->SetEventOnCompletion(serial,event)); if(WaitForSingleObject(event,120000)!=WAIT_OBJECT_0) throw std::runtime_error("GPU timeout");};
    xess_context_handle_t ctx=nullptr; xe(xessD3D12CreateContext(dev.Get(),&ctx));
    if(getenv("XESS_MODEL")) xe(xessSelectNetworkModel(ctx,(xess_network_model_t)atoi(getenv("XESS_MODEL"))));
    xe(xessForceLegacyScaleFactors(ctx,true)); // Explicit requested 2x input, independent of SDK defaults.
    xess_d3d12_init_params_t init={}; init.outputResolution={W,H}; init.qualitySetting=quality?XESS_QUALITY_SETTING_QUALITY:XESS_QUALITY_SETTING_PERFORMANCE; init.initFlags=getenv("XESS_FLAGS")?(unsigned)atoi(getenv("XESS_FLAGS")):XESS_INIT_FLAG_HIGH_RES_MV;
    if(getenv("XESS_BUILD")) xe(xessD3D12BuildPipelines(ctx,nullptr,true,init.initFlags));
    if(init.initFlags!=XESS_INIT_FLAG_HIGH_RES_MV && !(argc==5 && strcmp(argv[4],"init")==0)) throw std::runtime_error("Nondefault flags are supported for init-only capture, not benchmark dispatch");
    xe(xessD3D12Init(ctx,&init));
    xess_2d_t size; xe(xessGetInputResolution(ctx,&init.outputResolution,init.qualitySetting,&size));
    IW=size.x;IH=size.y;
    printf("mode=%s legacy_scale=1 input=%ux%u output=%ux%u\n",argv[3],IW,IH,W,H);fflush(stdout);
    if(argc==5 && strcmp(argv[4],"init")==0) {xe(xessDestroyContext(ctx));CloseHandle(event);return 0;}
    auto resource=[&](UINT w,UINT h,DXGI_FORMAT fmt,D3D12_RESOURCE_FLAGS flags,D3D12_RESOURCE_STATES state,D3D12_HEAP_TYPE heap,UINT64 bytes=0) {
        D3D12_RESOURCE_DESC d={}; d.Dimension=bytes?D3D12_RESOURCE_DIMENSION_BUFFER:D3D12_RESOURCE_DIMENSION_TEXTURE2D; d.Width=bytes?bytes:w; d.Height=bytes?1:h; d.DepthOrArraySize=1;d.MipLevels=1;d.Format=fmt;d.SampleDesc.Count=1;d.Layout=bytes?D3D12_TEXTURE_LAYOUT_ROW_MAJOR:D3D12_TEXTURE_LAYOUT_UNKNOWN;d.Flags=flags;
        D3D12_HEAP_PROPERTIES hp={};hp.Type=heap; ComPtr<ID3D12Resource> r;hr(dev->CreateCommittedResource(&hp,D3D12_HEAP_FLAG_NONE,&d,state,nullptr,IID_PPV_ARGS(&r)));return r;
    };
    auto color=resource(IW,IH,DXGI_FORMAT_R16G16B16A16_FLOAT,D3D12_RESOURCE_FLAG_NONE,D3D12_RESOURCE_STATE_COPY_DEST,D3D12_HEAP_TYPE_DEFAULT);
    auto motion=resource(W,H,DXGI_FORMAT_R16G16_FLOAT,D3D12_RESOURCE_FLAG_NONE,D3D12_RESOURCE_STATE_COPY_DEST,D3D12_HEAP_TYPE_DEFAULT);
    auto output=resource(W,H,DXGI_FORMAT_R16G16B16A16_FLOAT,D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_HEAP_TYPE_DEFAULT);
    auto layout=[&](ID3D12Resource* r,UINT64& bytes) {D3D12_PLACED_SUBRESOURCE_FOOTPRINT f;auto d=r->GetDesc();dev->GetCopyableFootprints(&d,0,1,0,&f,nullptr,nullptr,&bytes);return f;};
    UINT64 cb,mb,ob;auto cf=layout(color.Get(),cb),mf=layout(motion.Get(),mb),of=layout(output.Get(),ob);
    auto upload=resource(0,0,DXGI_FORMAT_UNKNOWN,D3D12_RESOURCE_FLAG_NONE,D3D12_RESOURCE_STATE_GENERIC_READ,D3D12_HEAP_TYPE_UPLOAD,cb);
    auto mupload=resource(0,0,DXGI_FORMAT_UNKNOWN,D3D12_RESOURCE_FLAG_NONE,D3D12_RESOURCE_STATE_GENERIC_READ,D3D12_HEAP_TYPE_UPLOAD,mb);
    auto read=resource(0,0,DXGI_FORMAT_UNKNOWN,D3D12_RESOURCE_FLAG_NONE,D3D12_RESOURCE_STATE_COPY_DEST,D3D12_HEAP_TYPE_READBACK,ob);
    void* ptr;D3D12_RANGE empty={0,0};hr(mupload->Map(0,&empty,&ptr));memset(ptr,0,mb);mupload->Unmap(0,nullptr);
    auto barrier=[&](ID3D12Resource* r,D3D12_RESOURCE_STATES before,D3D12_RESOURCE_STATES after) {D3D12_RESOURCE_BARRIER b={};b.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;b.Transition={r,D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,before,after};cmd->ResourceBarrier(1,&b);};
    auto copyin=[&](ID3D12Resource* dst,ID3D12Resource* src,D3D12_PLACED_SUBRESOURCE_FOOTPRINT f) {D3D12_TEXTURE_COPY_LOCATION a={},b={};a.pResource=dst;a.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;b.pResource=src;b.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;b.PlacedFootprint=f;cmd->CopyTextureRegion(&a,0,0,0,&b,nullptr);};
    ComPtr<ID3D12QueryHeap> timestamps;
    D3D12_QUERY_HEAP_DESC qh={};qh.Type=D3D12_QUERY_HEAP_TYPE_TIMESTAMP;qh.Count=2;
    hr(dev->CreateQueryHeap(&qh,IID_PPV_ARGS(&timestamps)));
    auto timingRead=resource(0,0,DXGI_FORMAT_UNKNOWN,D3D12_RESOURCE_FLAG_NONE,D3D12_RESOURCE_STATE_COPY_DEST,D3D12_HEAP_TYPE_READBACK,16);
    UINT64 frequency=0;hr(queue->GetTimestampFrequency(&frequency));
    hr(upload->Map(0,&empty,&ptr));
    for(UINT y=0;y<IH;++y) for(UINT x=0;x<IW;++x) {
        uint16_t rgba[4]={(uint16_t)(((x/7+y/11)&1)?0x3a00:0x3400),(uint16_t)(((x/31+y/23)&1)?0x3800:0x3000),(uint16_t)(0x3200+((x+y)&511)),0x3c00};
        memcpy((char*)ptr+cf.Offset+y*cf.Footprint.RowPitch+x*8,rgba,8);
    }
    upload->Unmap(0,nullptr);
    std::vector<double> timings;
    constexpr unsigned warmup=256,frames=1024;
    for(unsigned frame=0;frame<frames;++frame) {
        hr(alloc->Reset());hr(cmd->Reset(alloc.Get(),nullptr));
        if(!frame) {
            copyin(color.Get(),upload.Get(),cf);barrier(color.Get(),D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            copyin(motion.Get(),mupload.Get(),mf);barrier(motion.Get(),D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        }
        xess_d3d12_execute_params_t p={};p.pColorTexture=color.Get();p.pVelocityTexture=motion.Get();p.pOutputTexture=output.Get();p.inputWidth=IW;p.inputHeight=IH;p.exposureScale=1;p.resetHistory=(frame==0);
        // CPU image evaluates pixel centre + jitter in top-left texture coordinates.
        // SDK guide: effective sample displacement is the negative of API jitter.
        p.jitterOffsetX=0;p.jitterOffsetY=0;
        cmd->EndQuery(timestamps.Get(),D3D12_QUERY_TYPE_TIMESTAMP,0);
        xe(xessD3D12Execute(ctx,cmd.Get(),&p));
        cmd->EndQuery(timestamps.Get(),D3D12_QUERY_TYPE_TIMESTAMP,1);
        cmd->ResolveQueryData(timestamps.Get(),D3D12_QUERY_TYPE_TIMESTAMP,0,2,timingRead.Get(),0);
        hr(cmd->Close());ID3D12CommandList* lists[]={cmd.Get()};queue->ExecuteCommandLists(1,lists);wait();
        D3D12_RANGE tr={0,16};hr(timingRead->Map(0,&tr,&ptr));
        UINT64 ticks[2];memcpy(ticks,ptr,16);timingRead->Unmap(0,&empty);
        if(frame>=warmup) timings.push_back((ticks[1]-ticks[0])*1000.0/frequency);
    }
    std::sort(timings.begin(),timings.end());double sum=0;for(double t:timings)sum+=t;
    printf("GPU XeSS span: median=%.6f ms mean=%.6f ms p95=%.6f ms (%zu frames, %u warmup)\n",timings[timings.size()/2],sum/timings.size(),timings[timings.size()*95/100],timings.size(),warmup);fflush(stdout);
    wait();xe(xessDestroyContext(ctx));CloseHandle(event);return 0;
} catch(const std::exception& e) {fprintf(stderr,"bench-upscale: %s\n",e.what());return 1;}
