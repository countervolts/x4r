// SPDX-License-Identifier: GPL-3.0-only
// hiprun: vkrun for HIP (native Windows): runs a kernel code object made by tools/hlsl2hip.py on the same pseudo-random resources as
// vkrun and x4rsim (same generator, same fill order, 128x128 RGBA32F images, nearest/clamp sampler) and dumps the storage buffers
// and the debug buffer the same way, so tracecheck's comparison applies unchanged.
// usage: hiprun k.hsaco LX LY LZ gx gy gz out.bin [seed] [layout-file]      (X4R_TIME=<n>: GPU time over n runs)
// LX LY LZ: the CM workgroup (as x4rsim takes it); the launch uses the kernel's own flat workgroup size (16 lanes per CM thread).
// Built against the HIP SDK (ROCm 7.2): clang-cl -D__HIP_PLATFORM_AMD__ -I"%HIP_PATH%include" hiprun.cpp "%HIP_PATH%lib\amdhip64.lib"
#include <hip/hip_runtime.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <vector>

#define CK(x) do { hipError_t e_ = (x); if (e_) { std::fprintf(stderr, "%s: %s\n", #x, hipGetErrorString(e_)); return 1; } } while (0)

enum { T_SBUF, T_UBO, T_TEX, T_RWTEX, T_SAMP, T_DBG, T_NONE };
constexpr int NB = 56, IMW = 128, IMH = 128;
constexpr uint32_t BUFSZ = 4u << 20;
static uint32_t rs;
static uint32_t rnd() { rs ^= rs << 13; rs ^= rs >> 17; rs ^= rs << 5; return rs; }

struct Args { uint64_t slot[56]; uint32_t size[56]; };

int main(int argc, char** argv) {
    if (argc < 9) { std::fprintf(stderr, "usage: hiprun k.hsaco LX LY LZ gx gy gz out.bin [seed] [layout]\n"); return 2; }
    const int lx = std::atoi(argv[2]), ly = std::atoi(argv[3]), lz = std::atoi(argv[4]);
    const int gx = std::atoi(argv[5]), gy = std::atoi(argv[6]), gz = std::atoi(argv[7]);
    rs = argc > 9 ? uint32_t(std::atoi(argv[9])) : 12345;
    int type[NB];
    for (int i = 0; i < NB; ++i) type[i] = i < 32 ? T_SBUF : i < 47 ? T_UBO : i == 47 ? T_DBG : T_NONE;
    if (argc > 10) {
        FILE* lf = std::fopen(argv[10], "r");
        char t[32]; int b;
        while (lf && std::fscanf(lf, "%d %31s", &b, t) == 2)
            type[b] = !std::strcmp(t, "sbuf") ? T_SBUF : !std::strcmp(t, "ubo") ? T_UBO : !std::strcmp(t, "tex") ? T_TEX : !std::strcmp(t, "rwtex") ? T_RWTEX : T_SAMP;
        if (lf) std::fclose(lf);
    }
    std::ifstream f(argv[1], std::ios::binary);
    std::vector<char> co((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    if (co.empty()) { std::fprintf(stderr, "cannot read %s\n", argv[1]); return 1; }
    CK(hipInit(0));
    hipModule_t mod; hipFunction_t fn;
    CK(hipModuleLoadData(&mod, co.data()));
    CK(hipModuleGetFunction(&fn, mod, "x4r_main"));
    int block = 0;
    CK(hipFuncGetAttribute(&block, HIP_FUNC_ATTRIBUTE_MAX_THREADS_PER_BLOCK, fn));
    if (block != 16 * lx * ly * lz) std::fprintf(stderr, "hiprun: workgroup %d, expected %d (16 x %d x %d x %d)\n", block, 16 * lx * ly * lz, lx, ly, lz);

    Args a{};
    std::vector<std::vector<uint32_t>> host(NB);
    void* dev[NB] = {};
    for (int i = 0; i < NB; ++i) {
        if (type[i] != T_SBUF && type[i] != T_UBO && type[i] != T_DBG) continue;
        const uint32_t size = type[i] == T_UBO ? 65536 : type[i] == T_DBG ? (128u << 20) : BUFSZ;
        host[i].assign(size / 4, 0);
        if (type[i] != T_DBG)
            for (uint32_t k = 0; k < size / 4; ++k) {
                const uint32_t r = rnd();
                if (type[i] == T_UBO) host[i][k] = 4 + (r % 12);
                else if (i >= 16) host[i][k] = 0;
                else { float v = (int32_t(r & 0xffff) - 32768) / 16384.0f; std::memcpy(&host[i][k], &v, 4); if (rnd() & 1) host[i][k] = r; }
            }
        CK(hipMalloc(&dev[i], size));
        CK(hipMemcpy(dev[i], host[i].data(), size, hipMemcpyHostToDevice));
        a.slot[i] = uint64_t(dev[i]); a.size[i] = size;
    }
    // images: pitch-linear RGBA32F; the record holds the texture object's image descriptor twice (sampled, storage)
    std::vector<float> stg(IMW * IMH * 4);
    void* recs = nullptr;
    CK(hipMalloc(&recs, NB * 64));
    std::vector<uint32_t> rec(NB * 16, 0);
    for (int i = 0; i < NB; ++i) {
        if (type[i] != T_TEX && type[i] != T_RWTEX) continue;
        for (int k = 0; k < IMW * IMH * 4; ++k) stg[k] = type[i] == T_TEX ? (rnd() & 0xffff) / 65536.0f : 0.0f;
        CK(hipMalloc(&dev[i], IMW * IMH * 16));
        CK(hipMemcpy(dev[i], stg.data(), IMW * IMH * 16, hipMemcpyHostToDevice));
        hipResourceDesc r{}; r.resType = hipResourceTypePitch2D; r.res.pitch2D.devPtr = dev[i]; r.res.pitch2D.desc = hipCreateChannelDesc<float4>();
        r.res.pitch2D.width = IMW; r.res.pitch2D.height = IMH; r.res.pitch2D.pitchInBytes = IMW * 16;
        hipTextureDesc td{}; td.addressMode[0] = td.addressMode[1] = hipAddressModeClamp; td.filterMode = hipFilterModePoint; td.readMode = hipReadModeElementType;
        hipTextureObject_t t; CK(hipCreateTextureObject(&t, &r, &td, nullptr));
        uint32_t srd[8]; CK(hipMemcpy(srd, (void*)t, 32, hipMemcpyDeviceToHost));
        std::memcpy(&rec[i * 16], srd, 32); std::memcpy(&rec[i * 16 + 8], srd, 32);
        a.slot[i] = uint64_t(recs) + uint64_t(i) * 64;
    }
    {   // the sampler (nearest, clamp, normalised coordinates) for sampler and unused slots
        void* back; CK(hipMalloc(&back, 256));
        hipResourceDesc r{}; r.resType = hipResourceTypePitch2D; r.res.pitch2D.devPtr = back; r.res.pitch2D.desc = hipCreateChannelDesc<float>();
        r.res.pitch2D.width = 1; r.res.pitch2D.height = 1; r.res.pitch2D.pitchInBytes = 256;
        hipTextureDesc td{}; td.addressMode[0] = td.addressMode[1] = td.addressMode[2] = hipAddressModeClamp; td.filterMode = hipFilterModePoint;
        td.readMode = hipReadModeElementType; td.normalizedCoords = 1;
        hipTextureObject_t t; CK(hipCreateTextureObject(&t, &r, &td, nullptr));
        uint32_t w[16]; CK(hipMemcpy(w, (void*)t, 64, hipMemcpyDeviceToHost));
        for (int i = 0; i < NB; ++i)
            if (type[i] == T_SAMP || type[i] == T_NONE) { std::memcpy(&rec[i * 16], w + 12, 16); a.slot[i] = uint64_t(recs) + uint64_t(i) * 64; }
    }
    CK(hipMemcpy(recs, rec.data(), rec.size() * 4, hipMemcpyHostToDevice));

    size_t sz = sizeof a;
    void* cfg[] = {HIP_LAUNCH_PARAM_BUFFER_POINTER, &a, HIP_LAUNCH_PARAM_BUFFER_SIZE, &sz, HIP_LAUNCH_PARAM_END};
    const char* te = std::getenv("X4R_TIME");
    const int reps = te ? std::max(1, std::atoi(te)) : 1;
    hipEvent_t e0, e1; CK(hipEventCreate(&e0)); CK(hipEventCreate(&e1));
    double best = 1e30, sum = 0;
    for (int r = 0; r < reps; ++r) {
        CK(hipEventRecord(e0, nullptr));
        CK(hipModuleLaunchKernel(fn, gx, gy, gz, block, 1, 1, 0, nullptr, nullptr, cfg));
        CK(hipEventRecord(e1, nullptr));
        CK(hipEventSynchronize(e1));
        float ms = 0; CK(hipEventElapsedTime(&ms, e0, e1));
        best = std::min(best, double(ms)); sum += ms;
    }
    CK(hipDeviceSynchronize());
    if (te) std::printf("GPU time: best %.3f ms avg %.3f ms over %d runs\n", best, sum / reps, reps);
    FILE* o = std::fopen(argv[8], "wb");
    for (int i = 16; i < 31; ++i)
        if (dev[i] && type[i] == T_SBUF) { CK(hipMemcpy(host[i].data(), dev[i], BUFSZ, hipMemcpyDeviceToHost)); std::fwrite(host[i].data(), 1, BUFSZ, o); }
    std::fclose(o);
    if (dev[47]) {
        CK(hipMemcpy(host[47].data(), dev[47], 128u << 20, hipMemcpyDeviceToHost));
        std::string nm = std::string(argv[8]) + ".dbg";
        FILE* d = std::fopen(nm.c_str(), "wb"); std::fwrite(host[47].data(), 1, 128u << 20, d); std::fclose(d);
    }
    return 0;
}
