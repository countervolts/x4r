// SPDX-License-Identifier: GPL-3.0-only
// x4r_hip.h: what x4rwave's HLSL needs, as HIP device code (clang -x hip --cuda-device-only, no HIP headers), so tools/hlsl2hip.py
// can turn a translated kernel into an AMDGPU code object for native Windows (x4r on AMD's HIP runtime).
//
// Kernel ABI: one kernel argument, X4rArgs, by value: slot[i] is the 64-bit address of binding i (vkrun's numbering: t<n> -> n,
// u<n> -> 16 + n, b<n> -> 32 + n, s<n> -> 52 + n, the debug buffer u31 -> 47) and size[i] its size in bytes (buffers). A buffer is
// read and written through an AMDGPU buffer descriptor of that size, so out-of-range accesses read 0 and drop writes, as CM
// surfaces and D3D12 buffer views do. A texture slot points at a 64-byte record: the sampled image descriptor, then the storage one;
// a sampler slot at its 16-byte descriptor. A constant buffer slot is a plain address.
// Arithmetic follows DXC + AMD's compiler where HLSL leaves it open: division as written, transcendental functions on the hardware's
// approximations (exp2, log2, sin, cos, sqrt, rcp), f32 -> f16 with round-to-nearest-even.
#pragma once

#define __global__ __attribute__((global))
#define __device__ __attribute__((device))
#define __shared__ __attribute__((shared))
#define __constant__ __attribute__((constant))
#define X4R_FN __device__ static inline __attribute__((always_inline))

typedef unsigned int uint;
typedef unsigned long long uint64_t;
typedef long long int64_t;
typedef _Float16 float16_t;
typedef _Float16 half;
typedef unsigned short uint16_t;
typedef float float2 __attribute__((ext_vector_type(2)));
typedef float float3 __attribute__((ext_vector_type(3)));
typedef float float4 __attribute__((ext_vector_type(4)));
typedef int int2 __attribute__((ext_vector_type(2)));
typedef int int3 __attribute__((ext_vector_type(3)));
typedef int int4 __attribute__((ext_vector_type(4)));
typedef uint uint2 __attribute__((ext_vector_type(2)));
typedef uint uint3 __attribute__((ext_vector_type(3)));
typedef uint uint4 __attribute__((ext_vector_type(4)));

// HLSL's vector constructors (hlsl2hip.py renames float4(...) to X4R_V_float4(...))
X4R_FN float2 X4R_V_float2(float a, float b) { return float2{a, b}; }
X4R_FN float2 X4R_V_float2(float a) { return float2{a, a}; }
X4R_FN float3 X4R_V_float3(float a, float b, float c) { return float3{a, b, c}; }
X4R_FN float4 X4R_V_float4(float a, float b, float c, float d) { return float4{a, b, c, d}; }
X4R_FN float4 X4R_V_float4(float a) { return float4{a, a, a, a}; }
X4R_FN int2 X4R_V_int2(int a, int b) { return int2{a, b}; }
X4R_FN int3 X4R_V_int3(int a, int b, int c) { return int3{a, b, c}; }
X4R_FN int4 X4R_V_int4(int a, int b, int c, int d) { return int4{a, b, c, d}; }
X4R_FN uint2 X4R_V_uint2(uint a, uint b) { return uint2{a, b}; }
X4R_FN uint3 X4R_V_uint3(uint a, uint b, uint c) { return uint3{a, b, c}; }
X4R_FN uint3 X4R_V_uint3(uint2 ab, uint c) { return uint3{ab.x, ab.y, c}; }
X4R_FN int3 X4R_V_int3(int2 ab, int c) { return int3{ab.x, ab.y, c}; }
X4R_FN uint4 X4R_V_uint4(uint a, uint b, uint c, uint d) { return uint4{a, b, c, d}; }

X4R_FN float asfloat(uint x) { return __builtin_bit_cast(float, x); }
X4R_FN float asfloat(int x) { return __builtin_bit_cast(float, x); }
X4R_FN float asfloat(float x) { return x; }
X4R_FN uint asuint(float x) { return __builtin_bit_cast(uint, x); }
X4R_FN uint asuint(uint x) { return x; }
X4R_FN uint asuint(int x) { return uint(x); }
X4R_FN int asint(float x) { return __builtin_bit_cast(int, x); }
X4R_FN int asint(uint x) { return int(x); }
X4R_FN int asint(int x) { return x; }
X4R_FN uint16_t asuint16(half x) { return __builtin_bit_cast(uint16_t, x); }
X4R_FN float f16tof32(uint x) { return float(__builtin_bit_cast(half, uint16_t(x & 0xffffu))); }
X4R_FN bool isnan(float x) { return __builtin_isnan(x); }

X4R_FN float max(float a, float b) { return __builtin_fmaxf(a, b); }
X4R_FN float min(float a, float b) { return __builtin_fminf(a, b); }
X4R_FN int max(int a, int b) { return a > b ? a : b; }
X4R_FN int min(int a, int b) { return a < b ? a : b; }
X4R_FN uint max(uint a, uint b) { return a > b ? a : b; }
X4R_FN uint min(uint a, uint b) { return a < b ? a : b; }
X4R_FN float clamp(float x, float lo, float hi) { return __builtin_fminf(__builtin_fmaxf(x, lo), hi); }
X4R_FN int clamp(int x, int lo, int hi) { return min(max(x, lo), hi); }
X4R_FN uint clamp(uint x, uint lo, uint hi) { return min(max(x, lo), hi); }
X4R_FN float abs(float x) { return __builtin_fabsf(x); }
X4R_FN int abs(int x) { return x < 0 ? -x : x; }
X4R_FN float trunc(float x) { return __builtin_truncf(x); }
X4R_FN float floor(float x) { return __builtin_floorf(x); }
X4R_FN float sqrt(float x) { return __builtin_amdgcn_sqrtf(x); }
X4R_FN float exp2(float x) { return __builtin_amdgcn_exp2f(x); }
X4R_FN float log2(float x) { return __builtin_amdgcn_logf(x); }
X4R_FN float pow(float x, float y) { return __builtin_amdgcn_exp2f(y * __builtin_amdgcn_logf(x)); }
// v_sin/v_cos take the angle in revolutions
X4R_FN float sin(float x) { return __builtin_amdgcn_sinf(x * 0.15915494309189535f); }
X4R_FN float cos(float x) { return __builtin_amdgcn_cosf(x * 0.15915494309189535f); }

// Waves are 32 lanes; a CM thread is 16 of them (x4rwave)
X4R_FN uint WaveGetLaneIndex() { return __builtin_amdgcn_mbcnt_lo(~0u, 0u); }
X4R_FN uint WaveReadLaneAt(uint x, uint lane) { return uint(__builtin_amdgcn_ds_bpermute(int(lane << 2), int(x))); }
X4R_FN void GroupMemoryBarrierWithGroupSync() {
    __builtin_amdgcn_fence(__ATOMIC_RELEASE, "workgroup");
    __builtin_amdgcn_s_barrier();
    __builtin_amdgcn_fence(__ATOMIC_ACQUIRE, "workgroup");
}
X4R_FN void GroupMemoryBarrier() { __builtin_amdgcn_fence(__ATOMIC_SEQ_CST, "workgroup"); }
X4R_FN void AllMemoryBarrier() { __builtin_amdgcn_fence(__ATOMIC_SEQ_CST, "agent"); }

// signed int8 x int8 dot product with accumulate (the AMD instruction differs per generation; RDNA3/4: the mixed-sign one with both
// operands declared signed)
X4R_FN int dot4add_i8packed(uint a, uint b, int acc) {
#if defined(__gfx1030__) || defined(__gfx1031__) || defined(__gfx1032__) || defined(__gfx1033__) || defined(__gfx1034__) || \
    defined(__gfx1035__) || defined(__gfx1036__)
    return __builtin_amdgcn_sdot4(int(a), int(b), acc, false);
#else
    return __builtin_amdgcn_sudot4(true, int(a), true, int(b), acc, false);
#endif
}
X4R_FN uint dot4add_u8packed(uint a, uint b, uint acc) { return __builtin_amdgcn_udot4(a, b, acc, false); }

// DWORD 3 of a raw buffer descriptor: 32-bit format, out-of-range checks on the byte offset
#if defined(__gfx1030__) || defined(__gfx1031__) || defined(__gfx1032__) || defined(__gfx1033__) || defined(__gfx1034__) || \
    defined(__gfx1035__) || defined(__gfx1036__)
#define X4R_BUF_DW3 0x31014000
#else
#define X4R_BUF_DW3 0x31004000
#endif

struct X4rArgs { uint64_t slot[56]; uint size[56]; };

struct X4rBuf {
    __amdgpu_buffer_rsrc_t r;
    X4R_FN X4rBuf make(uint64_t p, uint bytes) { return X4rBuf{__builtin_amdgcn_make_buffer_rsrc((void*)p, (short)0, int(bytes), X4R_BUF_DW3)}; }
    __device__ uint Load(uint a) const { return __builtin_amdgcn_raw_buffer_load_b32(r, int(a), 0, 0); }
    __device__ uint2 Load2(uint a) const { return __builtin_amdgcn_raw_buffer_load_b64(r, int(a), 0, 0); }
    __device__ uint4 Load4(uint a) const { return __builtin_amdgcn_raw_buffer_load_b128(r, int(a), 0, 0); }
    __device__ void Store(uint a, uint v) const { __builtin_amdgcn_raw_buffer_store_b32(v, r, int(a), 0, 0); }
    __device__ void Store2(uint a, uint2 v) const { __builtin_amdgcn_raw_buffer_store_b64(v, r, int(a), 0, 0); }
    __device__ void Store4(uint a, uint4 v) const { __builtin_amdgcn_raw_buffer_store_b128(v, r, int(a), 0, 0); }
    __device__ void InterlockedAdd(uint a, uint v, uint& old) const { old = __builtin_amdgcn_raw_ptr_buffer_atomic_add_i32(int(v), r, int(a), 0, 0); }
    __device__ void InterlockedAdd(uint a, uint v) const { (void)__builtin_amdgcn_raw_ptr_buffer_atomic_add_i32(int(v), r, int(a), 0, 0); }
};

typedef __attribute__((address_space(4))) const __amdgpu_texture_t* X4rSrd;
struct X4rSmp { uint64_t p; };
struct X4rTex {
    uint64_t rec;
    __device__ float4 Load(int3 c) const { return __builtin_amdgcn_image_load_2d_v4f32_i32(15, c.x, c.y, *(X4rSrd)rec, 0, 0); }
    __device__ float4 operator[](uint2 c) const { return __builtin_amdgcn_image_load_2d_v4f32_i32(15, int(c.x), int(c.y), *(X4rSrd)rec, 0, 0); }
    __device__ float4 SampleLevel(X4rSmp s, float2 uv, float lod) const {
        typedef __attribute__((address_space(4))) const uint4* SampDesc;
        const uint4 sd = *(SampDesc)s.p;
        return __builtin_amdgcn_image_sample_l_2d_v4f32_f32(15, uv.x, uv.y, lod, *(X4rSrd)rec, sd, false, 0, 0);
    }
};
struct X4rTexel {
    uint64_t rec; uint2 c;
    __device__ operator float4() const { return __builtin_amdgcn_image_load_2d_v4f32_i32(15, int(c.x), int(c.y), *(X4rSrd)(rec + 32), 0, 0); }
    __device__ const X4rTexel& operator=(float4 v) const { __builtin_amdgcn_image_store_2d_v4f32_i32(v, 15, int(c.x), int(c.y), *(X4rSrd)(rec + 32), 0, 0); return *this; }
};
struct X4rRwTex {
    uint64_t rec;
    __device__ X4rTexel operator[](uint2 c) const { return X4rTexel{rec, c}; }
};

// x4rwave's WMMA flavour (X4R_WMMA=1): SPIR-V cooperative matrices of 16 x 16 int8 (A row-major, B column-major) and int32 (C), loaded
// from and stored to shared memory by the whole wave (32 lanes), as the WMMA instruction of RDNA3 / RDNA4 wants them. Strides are in
// 32-bit words; mul-add is signed x signed + signed (cooperative matrix operands 15).
typedef int x4r_i8x4 __attribute__((ext_vector_type(4)));
typedef int x4r_i8x2 __attribute__((ext_vector_type(2)));
typedef int x4r_i32x8 __attribute__((ext_vector_type(8)));
#if defined(__gfx1200__) || defined(__gfx1201__)
struct X4RMatA { x4r_i8x2 v; };
struct X4RMatB { x4r_i8x2 v; };
#else
struct X4RMatA { x4r_i8x4 v; };
struct X4RMatB { x4r_i8x4 v; };
#endif
struct X4RMatC { x4r_i32x8 v; };
X4R_FN uint x4r_lane() { return __builtin_amdgcn_mbcnt_lo(~0u, 0u); }
// row (A) or column (B) l & 15 of a 16 x 16 int8 tile whose rows / columns are `stride` words apart
#if defined(__gfx1200__) || defined(__gfx1201__)
X4R_FN x4r_i8x2 x4r_tile8(const uint* p, uint stride) {   // RDNA4: lanes 0-15 hold k 0-7, lanes 16-31 k 8-15
    const uint l = x4r_lane();
    const uint* q = p + (l & 15u) * stride + (l >> 4) * 2u;
    return x4r_i8x2{int(q[0]), int(q[1])};
}
#else
X4R_FN x4r_i8x4 x4r_tile8(const uint* p, uint stride) {   // RDNA3: every lane holds all 16 k (lanes 16-31 repeat 0-15)
    const uint* q = p + (x4r_lane() & 15u) * stride;
    return x4r_i8x4{int(q[0]), int(q[1]), int(q[2]), int(q[3])};
}
#endif
X4R_FN X4RMatA cmLoadA(const uint& p, uint layout, uint stride) { (void)layout; return X4RMatA{x4r_tile8(&p, stride)}; }
X4R_FN X4RMatB cmLoadB(const uint& p, uint layout, uint stride) { (void)layout; return X4RMatB{x4r_tile8(&p, stride)}; }
// C/D: lane l holds column l & 15; RDNA3: element i is row 2i + l / 16, RDNA4: row i + 8 (l / 16)
X4R_FN uint x4r_crow(uint i, uint l) {
#if defined(__gfx1200__) || defined(__gfx1201__)
    return i + 8u * (l >> 4);
#else
    return 2u * i + (l >> 4);
#endif
}
X4R_FN X4RMatC cmLoadC(const uint& p, uint layout, uint stride) {
    (void)layout;
    const uint l = x4r_lane();
    X4RMatC m;
    for (uint i = 0; i < 8u; ++i) m.v[i] = int((&p)[x4r_crow(i, l) * stride + (l & 15u)]);
    return m;
}
X4R_FN void cmStoreC(uint& p, X4RMatC m, uint layout, uint stride) {
    (void)layout;
    const uint l = x4r_lane();
    for (uint i = 0; i < 8u; ++i) (&p)[x4r_crow(i, l) * stride + (l & 15u)] = uint(m.v[i]);
}
X4R_FN X4RMatC cmMulAdd(X4RMatA a, X4RMatB b, X4RMatC c, uint operands) {
    (void)operands;
#if defined(__gfx1200__) || defined(__gfx1201__)
    return X4RMatC{__builtin_amdgcn_wmma_i32_16x16x16_iu8_w32_gfx12(true, a.v, true, b.v, c.v, false)};
#else
    return X4RMatC{__builtin_amdgcn_wmma_i32_16x16x16_iu8_w32(true, a.v, true, b.v, c.v, false)};
#endif
}

// HLSL's uint(...) / int(...) (hlsl2hip.py renames them): from float they saturate as D3D12 and CM do (NaN -> 0, out of range -> the
// nearest representable value), which C++'s conversion leaves undefined; from integers they are plain conversions.
X4R_FN uint x4r_uint(float x) { return !(x > 0.0f) ? 0u : x >= 4294967296.0f ? 0xffffffffu : uint(x); }
X4R_FN uint x4r_uint(uint x) { return x; }
X4R_FN uint x4r_uint(int x) { return uint(x); }
X4R_FN uint x4r_uint(uint64_t x) { return uint(x); }
X4R_FN uint x4r_uint(int64_t x) { return uint(x); }
X4R_FN uint x4r_uint(bool x) { return x ? 1u : 0u; }
X4R_FN uint x4r_uint(uint16_t x) { return x; }
X4R_FN int x4r_int(float x) { return x != x ? 0 : x >= 2147483648.0f ? 0x7fffffff : x <= -2147483648.0f ? int(0x80000000u) : int(x); }
X4R_FN int x4r_int(int x) { return x; }
X4R_FN int x4r_int(uint x) { return int(x); }
X4R_FN int x4r_int(uint64_t x) { return int(x); }
X4R_FN int x4r_int(int64_t x) { return int(x); }
X4R_FN int x4r_int(bool x) { return x ? 1 : 0; }
// HLSL's 0.0 is a float literal, C++'s a double: such mixed expressions convert back to float first
X4R_FN uint x4r_uint(double x) { return x4r_uint(float(x)); }
X4R_FN int x4r_int(double x) { return x4r_int(float(x)); }
