// SPDX-License-Identifier: GPL-3.0-only
// Deterministic headless XeSS quality capture. Build with tools/quality.sh.
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
using Microsoft::WRL::ComPtr;
static void hr(HRESULT r) { if(FAILED(r)) throw std::runtime_error("D3D12 HRESULT="+std::to_string((unsigned)r)); }
static void xe(xess_result_t r) { if(r<0) throw std::runtime_error("XeSS result="+std::to_string(r)); }
static float halton(unsigned i,unsigned b) { float v=0,s=1; while(i){s/=b;v+=(i%b)*s;i/=b;} return v-.5f; }
int main(int argc,char** argv) try {
    if(argc!=3) throw std::runtime_error("quality.exe <scene dir> <output dir>");
    constexpr UINT W=2560,H=1440,IW=1280,IH=720;
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
    xe(xessForceLegacyScaleFactors(ctx,true)); // Explicit requested 2x input, independent of SDK defaults.
    xess_d3d12_init_params_t init={}; init.outputResolution={W,H}; init.qualitySetting=XESS_QUALITY_SETTING_PERFORMANCE; init.initFlags=XESS_INIT_FLAG_HIGH_RES_MV;
    xe(xessD3D12Init(ctx,&init));
    xess_2d_t size; xe(xessGetInputResolution(ctx,&init.outputResolution,init.qualitySetting,&size));
    printf("recommended input=%ux%u; fixture input=%ux%u\n",size.x,size.y,IW,IH);fflush(stdout);
    if(size.x!=IW||size.y!=IH) throw std::runtime_error("Unexpected input resolution");
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
    std::vector<char> pixels(IW*IH*8);
    for(unsigned frame=0;frame<128;++frame) {
        char name[64];snprintf(name,sizeof(name),"/input-%02u.rgba16f",frame%32);
        std::ifstream in(std::string(argv[1])+name,std::ios::binary);if(!in.read(pixels.data(),pixels.size())) throw std::runtime_error("Cannot read scene input");
        hr(upload->Map(0,&empty,&ptr));for(UINT y=0;y<IH;++y) memcpy((char*)ptr+cf.Offset+y*cf.Footprint.RowPitch,pixels.data()+y*IW*8,IW*8);upload->Unmap(0,nullptr);
        hr(alloc->Reset());hr(cmd->Reset(alloc.Get(),nullptr));
        if(frame) barrier(color.Get(),D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,D3D12_RESOURCE_STATE_COPY_DEST);
        copyin(color.Get(),upload.Get(),cf);barrier(color.Get(),D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        if(!frame) {copyin(motion.Get(),mupload.Get(),mf);barrier(motion.Get(),D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);}
        xess_d3d12_execute_params_t p={};p.pColorTexture=color.Get();p.pVelocityTexture=motion.Get();p.pOutputTexture=output.Get();p.inputWidth=IW;p.inputHeight=IH;p.exposureScale=1;p.resetHistory=(frame==0);
        // CPU image evaluates pixel centre + jitter in top-left texture coordinates.
        // SDK guide: effective sample displacement is the negative of API jitter.
        p.jitterOffsetX=-halton(frame%32+1,2);p.jitterOffsetY=-halton(frame%32+1,3);
        xe(xessD3D12Execute(ctx,cmd.Get(),&p));
        bool capture=frame==31||frame==63||frame==127;
        if(capture) {barrier(output.Get(),D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_STATE_COPY_SOURCE);D3D12_TEXTURE_COPY_LOCATION a={},b={};a.pResource=read.Get();a.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;a.PlacedFootprint=of;b.pResource=output.Get();b.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;cmd->CopyTextureRegion(&a,0,0,0,&b,nullptr);barrier(output.Get(),D3D12_RESOURCE_STATE_COPY_SOURCE,D3D12_RESOURCE_STATE_UNORDERED_ACCESS);}
        hr(cmd->Close());ID3D12CommandList* lists[]={cmd.Get()};queue->ExecuteCommandLists(1,lists);wait();
        if(capture) {D3D12_RANGE range={0,(SIZE_T)ob};hr(read->Map(0,&range,&ptr));snprintf(name,sizeof(name),"/frame-%03u.rgba16f",frame);std::ofstream out(std::string(argv[2])+name,std::ios::binary);for(UINT y=0;y<H;++y)out.write((char*)ptr+of.Offset+y*of.Footprint.RowPitch,W*8);if(!out)throw std::runtime_error("Cannot write output");read->Unmap(0,&empty);printf("captured %u\n",frame);fflush(stdout);}
    }
    wait();xe(xessDestroyContext(ctx));CloseHandle(event);return 0;
} catch(const std::exception& e) {fprintf(stderr,"quality: %s\n",e.what());return 1;}
