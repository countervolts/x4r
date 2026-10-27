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
#include <algorithm>
#include "xess/xess_d3d12.h"
#include "xess/xess_debug.h"
using Microsoft::WRL::ComPtr;
static void hr(HRESULT r) { if(FAILED(r)) throw std::runtime_error("D3D12 HRESULT="+std::to_string((unsigned)r)); }
static void xe(xess_result_t r) { if(r<0) throw std::runtime_error("XeSS result="+std::to_string(r)); }
static float halton(unsigned i,unsigned b) { float v=0,s=1; while(i){s/=b;v+=(i%b)*s;i/=b;} return v-.5f; }
int main(int argc,char** argv) try {
    if(argc!=3&&argc!=4) throw std::runtime_error("quality.exe <scene dir> <output dir> [motion]");
    const bool motionMode=argc==4;   // moving-camera fixture: per-frame inputs and motion vectors, last frame captured
    constexpr UINT W=2560,H=1440;
    const UINT IW=getenv("X4R_IW")?atoi(getenv("X4R_IW")):1280,IH=getenv("X4R_IH")?atoi(getenv("X4R_IH")):720;   // input size; 856x480 is the 3x setup of Ghost of Tsushima
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
    if(getenv("XESS_LOG")) xe(xessSetLoggingCallback(ctx,XESS_LOGGING_LEVEL_DEBUG,[](const char* m,xess_logging_level_t l){printf("xess[%d] %s\n",(int)l,m);fflush(stdout);}));   // XESS_LOG=1: XeSS' own log on stdout
    xe(xessForceLegacyScaleFactors(ctx,true));
    if(getenv("XESS_MODEL")) xe(xessSelectNetworkModel(ctx,(xess_network_model_t)atoi(getenv("XESS_MODEL"))));   // 0 = KPSS, which OptiScaler selects // Explicit requested 2x input, independent of SDK defaults.
    xess_d3d12_init_params_t init={}; init.outputResolution={W,H}; init.qualitySetting=getenv("XESS_QUALITY")?(xess_quality_settings_t)atoi(getenv("XESS_QUALITY")):XESS_QUALITY_SETTING_PERFORMANCE; init.initFlags=getenv("XESS_FLAGS")?(unsigned)atoi(getenv("XESS_FLAGS")):XESS_INIT_FLAG_HIGH_RES_MV;   // XESS_FLAGS=70: the flags OptiScaler uses in Ghost of Tsushima (LDR, exposure texture, inverted depth, low-res MV)
    const bool hiMV=init.initFlags&XESS_INIT_FLAG_HIGH_RES_MV,useDepth=(init.initFlags&XESS_INIT_FLAG_INVERTED_DEPTH)||!(init.initFlags&XESS_INIT_FLAG_HIGH_RES_MV),useExpo=init.initFlags&XESS_INIT_FLAG_EXPOSURE_SCALE_TEXTURE,useMask=init.initFlags&XESS_INIT_FLAG_RESPONSIVE_PIXEL_MASK;
    const UINT MW=hiMV?W:IW,MH=hiMV?H:IH;
    if(getenv("XESS_MVSCALE")) xe(xessSetVelocityScale(ctx,(float)atof(getenv("XESS_MVSCALE")),(float)atof(getenv("XESS_MVSCALE"))));
    if(getenv("XESS_BUILD")) xe(xessD3D12BuildPipelines(ctx,nullptr,true,init.initFlags));   // OptiScaler builds the pipelines explicitly, with the init flags
    xe(xessD3D12Init(ctx,&init));
    xess_2d_t size; xe(xessGetInputResolution(ctx,&init.outputResolution,init.qualitySetting,&size));
    printf("recommended input=%ux%u; fixture input=%ux%u\n",size.x,size.y,IW,IH);fflush(stdout);
    if(!getenv("X4R_IW")&&(size.x!=IW||size.y!=IH)) throw std::runtime_error("Unexpected input resolution");
    auto resource=[&](UINT w,UINT h,DXGI_FORMAT fmt,D3D12_RESOURCE_FLAGS flags,D3D12_RESOURCE_STATES state,D3D12_HEAP_TYPE heap,UINT64 bytes=0) {
        D3D12_RESOURCE_DESC d={}; d.Dimension=bytes?D3D12_RESOURCE_DIMENSION_BUFFER:D3D12_RESOURCE_DIMENSION_TEXTURE2D; d.Width=bytes?bytes:w; d.Height=bytes?1:h; d.DepthOrArraySize=1;d.MipLevels=1;d.Format=fmt;d.SampleDesc.Count=1;d.Layout=bytes?D3D12_TEXTURE_LAYOUT_ROW_MAJOR:D3D12_TEXTURE_LAYOUT_UNKNOWN;d.Flags=flags;
        D3D12_HEAP_PROPERTIES hp={};hp.Type=heap; ComPtr<ID3D12Resource> r;hr(dev->CreateCommittedResource(&hp,D3D12_HEAP_FLAG_NONE,&d,state,nullptr,IID_PPV_ARGS(&r)));return r;
    };
    auto color=resource(IW,IH,DXGI_FORMAT_R16G16B16A16_FLOAT,D3D12_RESOURCE_FLAG_NONE,D3D12_RESOURCE_STATE_COPY_DEST,D3D12_HEAP_TYPE_DEFAULT);
    auto motion=resource(MW,MH,DXGI_FORMAT_R16G16_FLOAT,D3D12_RESOURCE_FLAG_NONE,D3D12_RESOURCE_STATE_COPY_DEST,D3D12_HEAP_TYPE_DEFAULT);
    auto output=resource(W,H,DXGI_FORMAT_R16G16B16A16_FLOAT,D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_HEAP_TYPE_DEFAULT);
    auto layout=[&](ID3D12Resource* r,UINT64& bytes) {D3D12_PLACED_SUBRESOURCE_FOOTPRINT f;auto d=r->GetDesc();dev->GetCopyableFootprints(&d,0,1,0,&f,nullptr,nullptr,&bytes);return f;};
    UINT64 cb,mb,ob;auto cf=layout(color.Get(),cb),mf=layout(motion.Get(),mb),of=layout(output.Get(),ob);
    auto upload=resource(0,0,DXGI_FORMAT_UNKNOWN,D3D12_RESOURCE_FLAG_NONE,D3D12_RESOURCE_STATE_GENERIC_READ,D3D12_HEAP_TYPE_UPLOAD,cb);
    auto mupload=resource(0,0,DXGI_FORMAT_UNKNOWN,D3D12_RESOURCE_FLAG_NONE,D3D12_RESOURCE_STATE_GENERIC_READ,D3D12_HEAP_TYPE_UPLOAD,mb);
    auto read=resource(0,0,DXGI_FORMAT_UNKNOWN,D3D12_RESOURCE_FLAG_NONE,D3D12_RESOURCE_STATE_COPY_DEST,D3D12_HEAP_TYPE_READBACK,ob);
    auto depth=resource(IW,IH,DXGI_FORMAT_R32_FLOAT,D3D12_RESOURCE_FLAG_NONE,D3D12_RESOURCE_STATE_COPY_DEST,D3D12_HEAP_TYPE_DEFAULT);
    auto expo=resource(1,1,DXGI_FORMAT_R32_FLOAT,D3D12_RESOURCE_FLAG_NONE,D3D12_RESOURCE_STATE_COPY_DEST,D3D12_HEAP_TYPE_DEFAULT);
    UINT64 db,eb;auto df=layout(depth.Get(),db),ef=layout(expo.Get(),eb);
    auto dupload=resource(0,0,DXGI_FORMAT_UNKNOWN,D3D12_RESOURCE_FLAG_NONE,D3D12_RESOURCE_STATE_GENERIC_READ,D3D12_HEAP_TYPE_UPLOAD,db);
    auto eupload=resource(0,0,DXGI_FORMAT_UNKNOWN,D3D12_RESOURCE_FLAG_NONE,D3D12_RESOURCE_STATE_GENERIC_READ,D3D12_HEAP_TYPE_UPLOAD,eb);
    auto mask=resource(IW,IH,DXGI_FORMAT_R8_UNORM,D3D12_RESOURCE_FLAG_NONE,D3D12_RESOURCE_STATE_COPY_DEST,D3D12_HEAP_TYPE_DEFAULT);
    UINT64 rb;auto rf=layout(mask.Get(),rb);
    auto rupload=resource(0,0,DXGI_FORMAT_UNKNOWN,D3D12_RESOURCE_FLAG_NONE,D3D12_RESOURCE_STATE_GENERIC_READ,D3D12_HEAP_TYPE_UPLOAD,rb);
    std::vector<char> depthpix(IW*IH*4);
    void* ptr;D3D12_RANGE empty={0,0};hr(mupload->Map(0,&empty,&ptr));memset(ptr,0,mb);mupload->Unmap(0,nullptr);
    auto barrier=[&](ID3D12Resource* r,D3D12_RESOURCE_STATES before,D3D12_RESOURCE_STATES after) {D3D12_RESOURCE_BARRIER b={};b.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;b.Transition={r,D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,before,after};cmd->ResourceBarrier(1,&b);};
    auto copyin=[&](ID3D12Resource* dst,ID3D12Resource* src,D3D12_PLACED_SUBRESOURCE_FOOTPRINT f) {D3D12_TEXTURE_COPY_LOCATION a={},b={};a.pResource=dst;a.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;b.pResource=src;b.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;b.PlacedFootprint=f;cmd->CopyTextureRegion(&a,0,0,0,&b,nullptr);};
    ComPtr<ID3D12QueryHeap> tsheap; ComPtr<ID3D12Resource> tsread; UINT64 tsfreq=0; std::vector<double> spans;
    if(getenv("X4R_FRAMES")) { D3D12_QUERY_HEAP_DESC qh={};qh.Type=D3D12_QUERY_HEAP_TYPE_TIMESTAMP;qh.Count=2;hr(dev->CreateQueryHeap(&qh,IID_PPV_ARGS(&tsheap)));tsread=resource(0,0,DXGI_FORMAT_UNKNOWN,D3D12_RESOURCE_FLAG_NONE,D3D12_RESOURCE_STATE_COPY_DEST,D3D12_HEAP_TYPE_READBACK,16);hr(queue->GetTimestampFrequency(&tsfreq)); }
    std::vector<char> pixels(IW*IH*8);
    const unsigned frames=getenv("X4R_FRAMES")?(unsigned)atoi(getenv("X4R_FRAMES")):(motionMode?20u:128u);   // X4R_FRAMES=400: timing run, the 20 motion frames repeat
    const bool pipelined=getenv("X4R_AGE_PROBE")!=nullptr;
    std::vector<ComPtr<ID3D12CommandAllocator>> frameAllocators;
    if(pipelined) {
        frameAllocators.resize(frames);
        for(auto& a:frameAllocators) hr(dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,IID_PPV_ARGS(&a)));
    }
    ComPtr<ID3D12Resource> ageUpload,ageRead;void* agePtr=nullptr;
    ComPtr<ID3D12Fence> deferredFence;
    if(getenv("X4R_AGE_DEFERRED_SIGNAL")) hr(dev->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&deferredFence)));
    if(pipelined) {
        // Deliberately reuse a CPU-written upload word while GPU work is queued.
        // This models the suspected resource-lifetime hazard; it is not evidence
        // that the game itself violates this lifetime requirement.
        ageUpload=resource(0,0,DXGI_FORMAT_UNKNOWN,D3D12_RESOURCE_FLAG_NONE,D3D12_RESOURCE_STATE_GENERIC_READ,D3D12_HEAP_TYPE_UPLOAD,4);
        ageRead=resource(0,0,DXGI_FORMAT_UNKNOWN,D3D12_RESOURCE_FLAG_NONE,D3D12_RESOURCE_STATE_COPY_DEST,D3D12_HEAP_TYPE_READBACK,frames*4);
        hr(ageUpload->Map(0,&empty,&agePtr));
    }
    const unsigned nin=motionMode?20u:32u;
    std::vector<char> mvpix(motionMode?MW*MH*4:0);
    for(unsigned frame=0;frame<frames;++frame) {
        char name[64];snprintf(name,sizeof(name),"/input-%02u.rgba16f",motionMode?frame%nin:frame%32);
        std::ifstream in(std::string(argv[1])+name,std::ios::binary);if(!in.read(pixels.data(),pixels.size())) throw std::runtime_error("Cannot read scene input");
        hr(upload->Map(0,&empty,&ptr));for(UINT y=0;y<IH;++y) memcpy((char*)ptr+cf.Offset+y*cf.Footprint.RowPitch,pixels.data()+y*IW*8,IW*8);upload->Unmap(0,nullptr);
        if(pipelined) hr(cmd->Reset(frameAllocators[frame].Get(),nullptr));
        else {hr(alloc->Reset());hr(cmd->Reset(alloc.Get(),nullptr));}
        if(frame) barrier(color.Get(),D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,D3D12_RESOURCE_STATE_COPY_DEST);
        copyin(color.Get(),upload.Get(),cf);barrier(color.Get(),D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        if(motionMode) {
            snprintf(name,sizeof(name),hiMV?"/mv-%02u.rg16f":"/mvlo-%02u.rg16f",frame%nin);
            std::ifstream mi(std::string(argv[1])+name,std::ios::binary);if(!mi.read(mvpix.data(),mvpix.size())) throw std::runtime_error("Cannot read motion vectors");
            hr(mupload->Map(0,&empty,&ptr));for(UINT y=0;y<MH;++y) memcpy((char*)ptr+mf.Offset+y*mf.Footprint.RowPitch,mvpix.data()+y*MW*4,MW*4);mupload->Unmap(0,nullptr);
            if(frame) barrier(motion.Get(),D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,D3D12_RESOURCE_STATE_COPY_DEST);
            copyin(motion.Get(),mupload.Get(),mf);barrier(motion.Get(),D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        }
        else if(!frame) {copyin(motion.Get(),mupload.Get(),mf);barrier(motion.Get(),D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);}
        if(motionMode&&useDepth) {
            snprintf(name,sizeof(name),"/depth-%02u.r32f",frame%nin);
            std::ifstream di(std::string(argv[1])+name,std::ios::binary);if(!di.read(depthpix.data(),depthpix.size())) throw std::runtime_error("Cannot read depth");
            hr(dupload->Map(0,&empty,&ptr));for(UINT y=0;y<IH;++y) memcpy((char*)ptr+df.Offset+y*df.Footprint.RowPitch,depthpix.data()+y*IW*4,IW*4);dupload->Unmap(0,nullptr);
            if(frame) barrier(depth.Get(),D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,D3D12_RESOURCE_STATE_COPY_DEST);
            copyin(depth.Get(),dupload.Get(),df);barrier(depth.Get(),D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        }
        if(motionMode&&useExpo&&!frame) {
            float one=1.0f;hr(eupload->Map(0,&empty,&ptr));memcpy((char*)ptr+ef.Offset,&one,4);eupload->Unmap(0,nullptr);
            copyin(expo.Get(),eupload.Get(),ef);barrier(expo.Get(),D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        }
        if(useMask&&!frame) {
            float mv=getenv("X4R_MASK_VALUE")?(float)atof(getenv("X4R_MASK_VALUE")):0.0f;
            unsigned char value=(unsigned char)(std::clamp(mv,0.0f,1.0f)*255.0f+0.5f);
            hr(rupload->Map(0,&empty,&ptr));memset(ptr,value,rb);rupload->Unmap(0,nullptr);
            copyin(mask.Get(),rupload.Get(),rf);barrier(mask.Get(),D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        }
        xess_d3d12_execute_params_t p={};if(motionMode&&useDepth) p.pDepthTexture=depth.Get();if(motionMode&&useExpo) p.pExposureScaleTexture=expo.Get();if(useMask) p.pResponsivePixelMaskTexture=mask.Get();p.pColorTexture=color.Get();p.pVelocityTexture=motion.Get();p.pOutputTexture=output.Get();p.inputWidth=IW;p.inputHeight=IH;p.exposureScale=1;p.resetHistory=(frame==0);
        // CPU image evaluates pixel centre + jitter in top-left texture coordinates.
        // SDK guide: effective sample displacement is the negative of API jitter.
        {float js=getenv("XESS_JSIGN")?(float)atof(getenv("XESS_JSIGN")):1.0f,jx=getenv("XESS_JXSIGN")?(float)atof(getenv("XESS_JXSIGN")):1.0f;p.jitterOffsetX=-js*jx*halton(frame%32+1,2);p.jitterOffsetY=-js*halton(frame%32+1,3);}
        if(tsheap) cmd->EndQuery(tsheap.Get(),D3D12_QUERY_TYPE_TIMESTAMP,0);
        {const int rep=getenv("X4R_REPEAT")?atoi(getenv("X4R_REPEAT")):1;for(int r=0;r<rep;++r){xe(xessD3D12Execute(ctx,cmd.Get(),&p));p.resetHistory=false;}}   // X4R_REPEAT: several executes per submission keep the GPU busy, so its clocks are steady
        if(pipelined) {memcpy(agePtr,&frame,4);cmd->CopyBufferRegion(ageRead.Get(),frame*4,ageUpload.Get(),0,4);}
        if(tsheap) {cmd->EndQuery(tsheap.Get(),D3D12_QUERY_TYPE_TIMESTAMP,1);cmd->ResolveQueryData(tsheap.Get(),D3D12_QUERY_TYPE_TIMESTAMP,0,2,tsread.Get(),0);}
        bool capture=getenv("X4R_FRAMES")&&!getenv("X4R_CAPTURE_FRAMES")?false:motionMode?frame+1==frames:(frame==31||frame==63||frame==127);
        if(capture) {barrier(output.Get(),D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_STATE_COPY_SOURCE);D3D12_TEXTURE_COPY_LOCATION a={},b={};a.pResource=read.Get();a.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;a.PlacedFootprint=of;b.pResource=output.Get();b.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;cmd->CopyTextureRegion(&a,0,0,0,&b,nullptr);barrier(output.Get(),D3D12_RESOURCE_STATE_COPY_SOURCE,D3D12_RESOURCE_STATE_UNORDERED_ACCESS);}
        hr(cmd->Close());ID3D12CommandList* lists[]={cmd.Get()};
        if(deferredFence) hr(queue->Wait(deferredFence.Get(),frame+1));
        queue->ExecuteCommandLists(1,lists);
        if(deferredFence) hr(deferredFence->Signal(frame+1));
        if(!pipelined||capture) wait();
        if(tsheap&&frame>=100) {UINT64* t;D3D12_RANGE r={0,16};hr(tsread->Map(0,&r,(void**)&t));spans.push_back((t[1]-t[0])*1000.0/tsfreq/(getenv("X4R_REPEAT")?atoi(getenv("X4R_REPEAT")):1));D3D12_RANGE e={0,0};tsread->Unmap(0,&e);}
        if(capture) {D3D12_RANGE range={0,(SIZE_T)ob};hr(read->Map(0,&range,&ptr));snprintf(name,sizeof(name),"/frame-%03u.rgba16f",frame);std::ofstream out(std::string(argv[2])+name,std::ios::binary);for(UINT y=0;y<H;++y)out.write((char*)ptr+of.Offset+y*of.Footprint.RowPitch,W*8);if(!out)throw std::runtime_error("Cannot write output");read->Unmap(0,&empty);printf("captured %u\n",frame);fflush(stdout);}
    }
    if(!spans.empty()) {std::sort(spans.begin(),spans.end());printf("GPU XeSS span: median=%.3f ms p10=%.3f p90=%.3f (%zu frames)\n",spans[spans.size()/2],spans[spans.size()/10],spans[spans.size()*9/10],spans.size());fflush(stdout);}
    wait();
    if(pipelined) {
        UINT* ages=nullptr;D3D12_RANGE r={0,frames*4};hr(ageRead->Map(0,&r,(void**)&ages));unsigned mismatches=0;
        std::ofstream out(std::string(argv[2])+"/frame-age.csv");out<<"submitted_frame,gpu_read_frame\n";
        for(unsigned f=0;f<frames;++f) {out<<f<<","<<ages[f]<<"\n";mismatches+=ages[f]!=f;}
        ageRead->Unmap(0,&empty);ageUpload->Unmap(0,nullptr);
        printf("frame-age probe mismatches=%u/%u\n",mismatches,frames);fflush(stdout);
    }
    xe(xessDestroyContext(ctx));CloseHandle(event);return 0;
} catch(const std::exception& e) {fprintf(stderr,"quality: %s\n",e.what());return 1;}
