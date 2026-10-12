// SPDX-License-Identifier: GPL-3.0-only
// x4rsim: CPU oracle for lifted CM kernels (LLVM IR with llvm.genx.* intrinsics). Straight byte-level semantics,
// independent of the HLSL translators. Runs one workgroup (all CM threads) on the same pseudo-random buffers as
// vkrun and writes a per-instruction trace in the layout of the GPU debug buffer for comparison.
//
// usage: x4rsim kernel.ll rootsig.txt LX LY LZ trace.txt out.dbg
//   trace.txt: output of x4rwave in trace mode (first line = words per thread, then "off words U|D text")
#include "llvm/IR/Constants.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"
#include "llvm/IRReader/IRReader.h"
#include "llvm/Support/SourceMgr.h"
#include "llvm/Support/raw_ostream.h"
#include <barrier>
#include <functional>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

using namespace llvm;
using std::string;
using std::vector;

[[noreturn]] static void die(const string& m) { fprintf(stderr, "x4rsim: %s\n", m.c_str()); exit(2); }

// ---- values: little-endian bytes of the whole (vector) value; i1 vectors are one byte per element ----
struct Val { vector<uint8_t> b; };

static unsigned elemBytes(Type* t)
{
    t = t->getScalarType();
    if (t->isHalfTy()) return 2;
    if (t->isFloatTy()) return 4;
    if (auto* it = dyn_cast<IntegerType>(t)) { unsigned w = it->getBitWidth(); return w == 1 ? 1 : w / 8; }
    die("elem type");
}
static unsigned nElem(Type* t) { return isa<FixedVectorType>(t) ? cast<FixedVectorType>(t)->getNumElements() : 1; }
static bool isI1(Type* t) { return t->getScalarType()->isIntegerTy(1); }
static bool isHalf(Type* t) { return t->getScalarType()->isHalfTy(); }
static bool isFlt(Type* t) { return t->getScalarType()->isFloatTy(); }
static unsigned bitsOf(Type* t) { t = t->getScalarType(); if (t->isHalfTy()) return 16; if (t->isFloatTy()) return 32; return cast<IntegerType>(t)->getBitWidth(); }

static uint64_t getU(const Val& v, Type* t, unsigned i)
{
    unsigned eb = elemBytes(t); uint64_t r = 0;
    memcpy(&r, &v.b[i * eb], eb);
    return r;
}
static void setU(Val& v, Type* t, unsigned i, uint64_t x)
{
    unsigned eb = elemBytes(t);
    if (isI1(t)) x &= 1;
    memcpy(&v.b[i * eb], &x, eb);
}
static float h2f(uint16_t h)
{
    uint32_t s = (h >> 15) & 1, e = (h >> 10) & 31, m = h & 1023, f;
    if (e == 0) { if (m == 0) f = s << 31; else { e = 1; while (!(m & 1024)) { m <<= 1; e--; } m &= 1023; f = (s << 31) | ((e + 112) << 23) | (m << 13); } }
    else if (e == 31) f = (s << 31) | 0x7f800000 | (m << 13);
    else f = (s << 31) | ((e + 112) << 23) | (m << 13);
    float r; memcpy(&r, &f, 4); return r;
}
static uint16_t f2h(float x)
{
    uint32_t f; memcpy(&f, &x, 4);
    uint32_t s = (f >> 16) & 0x8000, e = ((f >> 23) & 0xff), m = f & 0x7fffff;
    if (e == 255) return s | 0x7c00 | (m ? 0x200 : 0);
    int ne = (int)e - 127 + 15;
    if (ne >= 31) return s | 0x7c00;
    if (ne <= 0)
    {
        if (ne < -10) return s;
        m |= 0x800000; unsigned sh = 14 - ne; uint32_t hm = m >> sh; uint32_t rem = m & ((1u << sh) - 1), half = 1u << (sh - 1);
        if (rem > half || (rem == half && (hm & 1))) hm++;
        return s | hm;
    }
    uint32_t hm = m >> 13, rem = m & 0x1fff;
    uint32_t r = s | (ne << 10) | hm;
    if (rem > 0x1000 || (rem == 0x1000 && (hm & 1))) r++;
    return r;
}
static float ftz(float f) { return fabsf(f) < 1.17549435e-38f ? copysignf(0.0f, f) : f; }   // GPU flushes fp32 denormals
static float getF(const Val& v, Type* t, unsigned i)
{
    if (isHalf(t)) return h2f((uint16_t)getU(v, t, i));
    uint32_t u = (uint32_t)getU(v, t, i); float f; memcpy(&f, &u, 4); return ftz(f);
}
static void setF(Val& v, Type* t, unsigned i, float f)
{
    if (isHalf(t)) { setU(v, t, i, f2h(f)); return; }
    f = ftz(f); uint32_t u; memcpy(&u, &f, 4); setU(v, t, i, u);
}
static int64_t sext(uint64_t x, unsigned bits) { if (bits >= 64) return (int64_t)x; return (int64_t)(x << (64 - bits)) >> (64 - bits); }

// ---- shared state ----
struct Res { string kind; unsigned reg = 0; };
static std::map<Argument*, int> argRes;
static vector<Res> resv;
static vector<vector<uint8_t>> bufs;   // 0..46 as in vkrun: SRV t -> t, UAV u -> 16+u, CBV b -> 32+b
static vector<uint8_t> slm;
static const unsigned IMW = 128, IMH = 128;
static std::map<int, vector<float>> imgs;   // binding -> RGBA32F texels
static unsigned LX, LY, LZ;
static unsigned trLo = 0, trHi = ~0u;

static uint32_t rs;
static uint32_t rnd() { rs ^= rs << 13; rs ^= rs >> 17; rs ^= rs << 5; return rs; }

static const uint64_t kBudget = 30000000;   // per-thread instruction budget: prove termination before the GPU ever runs the kernel
struct Thread
{
    uint64_t steps = 0;
    int tid;
    std::map<Value*, Val> vals;
    std::map<Instruction*, Val>* trace;
    std::map<Value*, int> surf;
    Function* F;
    std::barrier<>* bar;

    Val constVal(Constant* c)
    {
        Type* t = c->getType(); Val v; v.b.assign(elemBytes(t) * nElem(t), 0);
        unsigned n = nElem(t);
        for (unsigned i = 0; i < n; ++i)
        {
            Constant* e = isa<FixedVectorType>(t) ? c->getAggregateElement(i) : c;
            if (!e || isa<UndefValue>(e)) continue;
            if (auto* ci = dyn_cast<ConstantInt>(e)) setU(v, t, i, ci->getZExtValue());
            else if (auto* cf = dyn_cast<ConstantFP>(e)) setF(v, t, i, cf->getValueAPF().convertToFloat());
        }
        return v;
    }
    Val& get(Value* x)
    {
        auto it = vals.find(x);
        if (it != vals.end()) return it->second;
        if (auto* c = dyn_cast<Constant>(x)) { vals[x] = constVal(c); return vals[x]; }
        die("value not computed");
    }
    int64_t cint(Value* x) { return cast<ConstantInt>(x)->getSExtValue(); }
    uint32_t u32(Value* x) { return (uint32_t)getU(get(x), x->getType(), 0); }

    uint8_t* mem(int s, uint32_t addr, unsigned n)
    {
        auto& b = bufs[s];
        if ((uint64_t)addr + n > b.size()) die("buffer access out of range");
        return &b[addr];
    }
    int surfaceOf(Value* x) { auto it = surf.find(x); if (it == surf.end()) die("surface"); return it->second; }
    int bufIndex(int s) { const Res& r = resv[s]; return r.kind == "SRV" ? r.reg : r.kind == "UAV" ? 16 + r.reg : 32 + r.reg; }

    void ew1(Instruction& I, Val& r, std::function<void(unsigned)> f) { unsigned n = nElem(I.getType()); for (unsigned i = 0; i < n; ++i) f(i); }

    Val exec(Instruction& I)
    {
        Type* rt = I.getType();
        Val r;
        if (!rt->isVoidTy()) r.b.assign(elemBytes(rt) * nElem(rt), 0);
        unsigned n = rt->isVoidTy() ? 0 : nElem(rt);
        unsigned bits = rt->isVoidTy() ? 0 : bitsOf(rt);
        auto op = I.getOpcode();
        auto A = [&](unsigned k) -> Val& { return get(I.getOperand(k)); };
        auto AT = [&](unsigned k) { return I.getOperand(k)->getType(); };
        switch (op)
        {
        case Instruction::Add: case Instruction::Sub: case Instruction::Mul: case Instruction::And: case Instruction::Or: case Instruction::Xor:
        case Instruction::UDiv: case Instruction::URem: case Instruction::SDiv: case Instruction::SRem: case Instruction::Shl: case Instruction::LShr: case Instruction::AShr:
            for (unsigned i = 0; i < n; ++i)
            {
                uint64_t a = getU(A(0), AT(0), i), b = getU(A(1), AT(1), i), x = 0;
                switch (op) {
                case Instruction::Add: x = a + b; break; case Instruction::Sub: x = a - b; break; case Instruction::Mul: x = a * b; break;
                case Instruction::And: x = a & b; break; case Instruction::Or: x = a | b; break; case Instruction::Xor: x = a ^ b; break;
                case Instruction::UDiv: x = b ? a / b : 0; break; case Instruction::URem: x = b ? a % b : 0; break;
                case Instruction::SDiv: x = sext(b, bits) ? (uint64_t)(sext(a, bits) / sext(b, bits)) : 0; break;
                case Instruction::SRem: x = sext(b, bits) ? (uint64_t)(sext(a, bits) % sext(b, bits)) : 0; break;
                case Instruction::Shl: x = b >= bits ? 0 : a << b; break; case Instruction::LShr: x = b >= bits ? 0 : a >> b; break;
                default: x = (uint64_t)(sext(a, bits) >> (b >= bits ? bits - 1 : b)); break;
                }
                setU(r, rt, i, bits == 64 ? x : x & ((1ull << bits) - 1));
            }
            return r;
        case Instruction::FAdd: case Instruction::FSub: case Instruction::FMul: case Instruction::FDiv: case Instruction::FRem:
            for (unsigned i = 0; i < n; ++i)
            {
                float a = getF(A(0), AT(0), i), b = getF(A(1), AT(1), i), x;
                switch (op) { case Instruction::FAdd: x = a + b; break; case Instruction::FSub: x = a - b; break; case Instruction::FMul: x = a * b; break;
                              case Instruction::FDiv: x = a / b; break; default: x = fmodf(a, b); }
                setF(r, rt, i, x);
            }
            return r;
        case Instruction::FNeg: for (unsigned i = 0; i < n; ++i) setF(r, rt, i, -getF(A(0), AT(0), i)); return r;
        case Instruction::ICmp: {
            auto p = cast<ICmpInst>(&I)->getPredicate(); unsigned ob = bitsOf(AT(0));
            for (unsigned i = 0; i < n; ++i)
            {
                uint64_t a = getU(A(0), AT(0), i), b = getU(A(1), AT(1), i); int64_t sa = sext(a, ob), sb = sext(b, ob); bool x = false;
                switch (p) { case CmpInst::ICMP_EQ: x = a == b; break; case CmpInst::ICMP_NE: x = a != b; break; case CmpInst::ICMP_ULT: x = a < b; break; case CmpInst::ICMP_ULE: x = a <= b; break;
                             case CmpInst::ICMP_UGT: x = a > b; break; case CmpInst::ICMP_UGE: x = a >= b; break; case CmpInst::ICMP_SLT: x = sa < sb; break; case CmpInst::ICMP_SLE: x = sa <= sb; break;
                             case CmpInst::ICMP_SGT: x = sa > sb; break; default: x = sa >= sb; }
                setU(r, rt, i, x);
            }
            return r;
        }
        case Instruction::FCmp: {
            auto p = cast<FCmpInst>(&I)->getPredicate();
            for (unsigned i = 0; i < n; ++i)
            {
                float a = getF(A(0), AT(0), i), b = getF(A(1), AT(1), i); bool nan = std::isnan(a) || std::isnan(b), x = false;
                switch (p) { case CmpInst::FCMP_OEQ: x = !nan && a == b; break; case CmpInst::FCMP_ONE: x = !nan && a != b; break; case CmpInst::FCMP_OLT: x = !nan && a < b; break;
                             case CmpInst::FCMP_OLE: x = !nan && a <= b; break; case CmpInst::FCMP_OGT: x = !nan && a > b; break; case CmpInst::FCMP_OGE: x = !nan && a >= b; break;
                             case CmpInst::FCMP_ORD: x = !nan; break; case CmpInst::FCMP_UNO: x = nan; break; case CmpInst::FCMP_UEQ: x = nan || a == b; break;
                             case CmpInst::FCMP_UNE: x = nan || a != b; break; case CmpInst::FCMP_ULT: x = nan || a < b; break; case CmpInst::FCMP_ULE: x = nan || a <= b; break;
                             case CmpInst::FCMP_UGT: x = nan || a > b; break; default: x = nan || a >= b; }
                setU(r, rt, i, x);
            }
            return r;
        }
        case Instruction::Select: {
            bool vc = isa<FixedVectorType>(AT(0));
            for (unsigned i = 0; i < n; ++i)
            {
                bool c = getU(A(0), AT(0), vc ? i : 0) & 1;
                unsigned eb = elemBytes(rt);
                memcpy(&r.b[i * eb], &(c ? A(1) : A(2)).b[i * eb], eb);
            }
            return r;
        }
        case Instruction::Trunc: case Instruction::ZExt: case Instruction::SExt:
            for (unsigned i = 0; i < n; ++i)
            {
                uint64_t a = getU(A(0), AT(0), i);
                setU(r, rt, i, op == Instruction::SExt ? (uint64_t)sext(a, bitsOf(AT(0))) : a);
            }
            // mask
            for (unsigned i = 0; i < n; ++i) { uint64_t x = getU(r, rt, i); if (bits < 64) x &= (1ull << bits) - 1; setU(r, rt, i, x); }
            return r;
        case Instruction::FPToUI: case Instruction::FPToSI:
            for (unsigned i = 0; i < n; ++i) { float f = getF(A(0), AT(0), i); setU(r, rt, i, op == Instruction::FPToSI ? (uint64_t)(int64_t)f : (uint64_t)f); }
            return r;
        case Instruction::UIToFP: case Instruction::SIToFP:
            for (unsigned i = 0; i < n; ++i) { uint64_t a = getU(A(0), AT(0), i); setF(r, rt, i, op == Instruction::SIToFP ? (float)sext(a, bitsOf(AT(0))) : (float)a); }
            return r;
        case Instruction::FPExt: case Instruction::FPTrunc: for (unsigned i = 0; i < n; ++i) setF(r, rt, i, getF(A(0), AT(0), i)); return r;
        case Instruction::BitCast: r.b = A(0).b; return r;
        case Instruction::ExtractElement: { unsigned ix = (unsigned)getU(A(1), AT(1), 0); unsigned eb = elemBytes(rt); memcpy(&r.b[0], &A(0).b[ix * eb], eb); return r; }
        case Instruction::InsertElement: { r = A(0); unsigned ix = (unsigned)getU(A(2), AT(2), 0); unsigned eb = elemBytes(rt); memcpy(&r.b[ix * eb], &A(1).b[0], eb); return r; }
        case Instruction::ShuffleVector: {
            auto* S = cast<ShuffleVectorInst>(&I); unsigned n1 = nElem(AT(0)), eb = elemBytes(rt);
            for (unsigned i = 0; i < n; ++i) { int m = S->getMaskValue(i); if (m < 0) continue; const Val& s = (unsigned)m < n1 ? A(0) : A(1); memcpy(&r.b[i * eb], &s.b[((unsigned)m < n1 ? m : m - n1) * eb], eb); }
            return r;
        }
        case Instruction::Freeze: return A(0);
        case Instruction::Call: return call(cast<CallInst>(I), r);
        default: die(string("unsupported instruction ") + I.getOpcodeName());
        }
    }

    Val call(CallInst& C, Val r)
    {
        Function* f = C.getCalledFunction(); string nme = f->getName().str(); Type* rt = C.getType();
        auto starts = [&](const char* p) { return nme.rfind(p, 0) == 0; };
        auto A = [&](unsigned k) -> Val& { return get(C.getArgOperand(k)); };
        auto AT = [&](unsigned k) { return C.getArgOperand(k)->getType(); };
        unsigned n = rt->isVoidTy() ? 0 : nElem(rt);
        if (nme.find("convert_int") != string::npos && C.arg_size() == 1 && AT(0)->isTargetExtTy())
        {
            auto it = argRes.find(cast<Argument>(C.getArgOperand(0)));
            int s = it != argRes.end() ? it->second : -1; surf[&C] = s; uint32_t v = s < 0 ? 0 : s; memcpy(&r.b[0], &v, 4); return r;
        }
        auto set64 = [&](uint64_t v) { memcpy(&r.b[0], &v, 8); return r; };
        if (nme == "_Z12get_group_idj") return set64(0);
        if (nme == "_Z12get_local_idj") { unsigned d = u32(C.getArgOperand(0)); return set64(d == 0 ? tid % LX : d == 1 ? (tid / LX) % LY : tid / (LX * LY)); }
        if (nme == "_Z14get_local_sizej") { unsigned d = u32(C.getArgOperand(0)); return set64(d == 0 ? LX : d == 1 ? LY : LZ); }
        if (nme == "_Z19get_local_linear_idv" || nme == "_Z20get_global_linear_idv") return set64(tid);
        if (nme == "_Z7barrierj") { bar->arrive_and_wait(); return r; }
        if (starts("_Z") && nme.find("native_") != string::npos)
        {
            for (unsigned i = 0; i < n; ++i)
            {
                float a = getF(A(0), AT(0), i), x = 0;
                if (nme.find("log2") != string::npos) x = log2f(a); else if (nme.find("exp2") != string::npos) x = exp2f(a); else if (nme.find("sqrt") != string::npos) x = sqrtf(a);
                else if (nme.find("recip") != string::npos) x = 1.0f / a; else if (nme.find("powr") != string::npos) x = powf(a, getF(A(1), AT(1), i));
                else if (nme.find("cos") != string::npos) x = cosf(a); else if (nme.find("sin") != string::npos) x = sinf(a); else die("native " + nme);
                setF(r, rt, i, x);
            }
            return r;
        }
        if (starts("llvm.vector.reduce.mul")) { uint32_t p = 1; for (unsigned i = 0; i < nElem(AT(0)); ++i) p *= (uint32_t)getU(A(0), AT(0), i); memcpy(&r.b[0], &p, 4); return r; }
        if (starts("llvm.lifetime") || starts("llvm.assume")) return r;
        if (starts("llvm.genx.fmax") || starts("llvm.genx.fmin")) { for (unsigned i = 0; i < n; ++i) { float a = getF(A(0), AT(0), i), b = getF(A(1), AT(1), i); setF(r, rt, i, nme.find("max") != string::npos ? fmaxf(a, b) : fminf(a, b)); } return r; }
        if (starts("llvm.genx.smax") || starts("llvm.genx.smin") || starts("llvm.genx.umax") || starts("llvm.genx.umin"))
        {
            bool sg = nme[10] == 's', mx = nme.find("max") != string::npos; unsigned b = bitsOf(rt);
            for (unsigned i = 0; i < n; ++i) { uint64_t x = getU(A(0), AT(0), i), y = getU(A(1), AT(1), i); bool lt = sg ? sext(x, b) < sext(y, b) : x < y; setU(r, rt, i, mx ? (lt ? y : x) : (lt ? x : y)); }
            return r;
        }
        if (starts("llvm.genx.rndd") || starts("llvm.genx.rndu") || starts("llvm.genx.rnde") || starts("llvm.genx.rndz"))
        { for (unsigned i = 0; i < n; ++i) { float a = getF(A(0), AT(0), i); setF(r, rt, i, starts("llvm.genx.rndd") ? floorf(a) : starts("llvm.genx.rndu") ? ceilf(a) : starts("llvm.genx.rnde") ? nearbyintf(a) : truncf(a)); } return r; }
        if (starts("llvm.genx.fptosi.sat") || starts("llvm.genx.fptoui.sat"))
        {
            bool sg = starts("llvm.genx.fptosi"); unsigned b = bitsOf(rt);
            for (unsigned i = 0; i < n; ++i)
            {
                double a = trunc((double)getF(A(0), AT(0), i)); if (std::isnan(a)) a = 0;
                double lo = sg ? -(double)(1ull << (b - 1)) : 0, hi = sg ? (double)((1ull << (b - 1)) - 1) : (double)((1ull << b) - 1);
                a = a < lo ? lo : a > hi ? hi : a;
                setU(r, rt, i, sg ? (uint64_t)(int64_t)a : (uint64_t)a);
            }
            return r;
        }
        if (starts("llvm.genx.sstrunc.sat") || starts("llvm.genx.ustrunc.sat") || starts("llvm.genx.sutrunc.sat"))
        {
            bool srcS = starts("llvm.genx.ss") || starts("llvm.genx.su"), dstS = starts("llvm.genx.ss"); unsigned b = bitsOf(rt), sb = bitsOf(AT(0));
            for (unsigned i = 0; i < n; ++i)
            {
                uint64_t a = getU(A(0), AT(0), i); int64_t s = srcS ? sext(a, sb) : (int64_t)a;
                int64_t lo = dstS ? -(1ll << (b - 1)) : 0, hi = dstS ? (1ll << (b - 1)) - 1 : (1ll << b) - 1;
                s = s < lo ? lo : s > hi ? hi : s; setU(r, rt, i, (uint64_t)s);
            }
            return r;
        }
        if (starts("llvm.genx.rdregion"))
        {
            unsigned eb = elemBytes(rt); int64_t vs = cint(C.getArgOperand(1)), w = cint(C.getArgOperand(2)), st = cint(C.getArgOperand(3));
            uint64_t off = getU(A(4), AT(4), 0); unsigned tot = nElem(AT(0));
            for (unsigned j = 0; j < n; ++j)
            {
                int64_t e = (int64_t)(off / eb) + (int64_t)(j / w) * vs + (int64_t)(j % w) * st;
                if (e >= 0 && (uint64_t)e < tot) memcpy(&r.b[j * eb], &A(0).b[e * eb], eb);
            }
            return r;
        }
        if (starts("llvm.genx.wrregion"))
        {
            r = A(0); unsigned eb = elemBytes(rt); int64_t vs = cint(C.getArgOperand(2)), w = cint(C.getArgOperand(3)), st = cint(C.getArgOperand(4));
            uint64_t off = getU(A(5), AT(5), 0); unsigned nn = nElem(AT(1)), tot = nElem(rt);
            if (!(getU(A(7), AT(7), 0) & 1)) return r;
            for (unsigned j = 0; j < nn; ++j)
            {
                int64_t e = (int64_t)(off / eb) + (int64_t)(j / w) * vs + (int64_t)(j % w) * st;
                if (e >= 0 && (uint64_t)e < tot) memcpy(&r.b[e * eb], &A(1).b[j * eb], eb);
            }
            return r;
        }
        if (starts("llvm.genx.dpas2") || starts("llvm.genx.dpas.nosrc0"))
        {
            bool has = starts("llvm.genx.dpas2"); unsigned pB, pA, rep;
            if (has) { pB = cint(C.getArgOperand(3)); pA = cint(C.getArgOperand(4)); rep = cint(C.getArgOperand(6)); }
            else { uint32_t info = (uint32_t)cint(C.getArgOperand(2)); pB = info & 0xff; pA = (info >> 8) & 0xff; rep = info >> 24; }
            const Val& B = A(has ? 1 : 0); const Val& Am = A(has ? 2 : 1);
            for (unsigned rr = 0; rr < rep; ++rr) for (unsigned nn = 0; nn < 16; ++nn)
            {
                int32_t s = has ? (int32_t)getU(A(0), AT(0), rr * 16 + nn) : 0;
                for (unsigned d = 0; d < 8; ++d) for (unsigned k = 0; k < 4; ++k)
                {
                    uint8_t ab = Am.b[(rr * 8 + d) * 4 + k], bb = B.b[(d * 16 + nn) * 4 + k];
                    int a = pA == 8 ? (int8_t)ab : ab, b = pB == 8 ? (int8_t)bb : bb;
                    s += a * b;
                }
                setU(r, rt, rr * 16 + nn, (uint32_t)s);
            }
            return r;
        }
        auto blk = [&](int s, bool isSlm, uint32_t addr, unsigned bytes, uint8_t* data, bool st)
        {
            uint8_t* p;
            if (isSlm) { if ((uint64_t)addr + bytes > slm.size()) die("slm out of range"); p = &slm[addr]; }
            else { int bi = bufIndex(s); if (bufs[bi].size() < (uint64_t)addr + bytes) die("buffer oob"); p = &bufs[bi][addr]; }
            if (st) memcpy(p, data, bytes); else memcpy(data, p, bytes);
        };
        if (starts("llvm.genx.oword.ld")) { int s = surfaceOf(C.getArgOperand(1)); uint32_t off = u32(C.getArgOperand(2)); if (nme.find("unaligned") == string::npos) off *= 16; blk(s, false, off, r.b.size(), r.b.data(), false); return r; }
        if (starts("llvm.genx.oword.st")) { int s = surfaceOf(C.getArgOperand(0)); Val d = A(2); blk(s, false, u32(C.getArgOperand(1)) * 16, d.b.size(), d.b.data(), true); return r; }
        if (starts("llvm.genx.lsc.load.bti") || starts("llvm.genx.lsc.load.slm"))
        {
            bool isSlm = nme.find(".slm") != string::npos; uint32_t addr = u32(C.getArgOperand(10)) * (uint32_t)cint(C.getArgOperand(4)) + (uint32_t)cint(C.getArgOperand(5));
            blk(isSlm ? 0 : surfaceOf(C.getArgOperand(11)), isSlm, addr, r.b.size(), r.b.data(), false); return r;
        }
        if (starts("llvm.genx.lsc.store.bti") || starts("llvm.genx.lsc.store.slm"))
        {
            bool isSlm = nme.find(".slm") != string::npos; uint32_t addr = u32(C.getArgOperand(10)) * (uint32_t)cint(C.getArgOperand(4)) + (uint32_t)cint(C.getArgOperand(5));
            Val d = A(11); blk(isSlm ? 0 : surfaceOf(C.getArgOperand(12)), isSlm, addr, d.b.size(), d.b.data(), true); return r;
        }
        if (starts("llvm.genx.fence")) return r;
        if (starts("llvm.genx.gather.masked.scaled2") || starts("llvm.genx.gather.scaled2"))
        {
            uint32_t g = u32(C.getArgOperand(3));
            for (unsigned i = 0; i < n; ++i) { bool m = C.arg_size() > 5 ? (getU(A(5), AT(5), i) & 1) : true; uint32_t a = g + (uint32_t)getU(A(4), AT(4), i); if (m) memcpy(&r.b[i * 4], &slm[a], 4); }
            return r;
        }
        if (starts("llvm.genx.scatter.scaled"))
        {
            uint32_t g = u32(C.getArgOperand(4));
            for (unsigned i = 0; i < nElem(AT(5)); ++i) if (getU(A(0), AT(0), i) & 1) { uint32_t a = g + (uint32_t)getU(A(5), AT(5), i); memcpy(&slm[a], &A(6).b[i * 4], 4); }
            return r;
        }
        if (starts("llvm.genx.lsc.xatomic.bti"))
        {
            int sub = cint(C.getArgOperand(1)); int s = surfaceOf(C.getArgOperand(13)); uint32_t imm = (uint32_t)cint(C.getArgOperand(5));
            for (unsigned i = 0; i < nElem(AT(0)); ++i)
            {
                if (!(getU(A(0), AT(0), i) & 1)) continue;
                uint32_t a = (uint32_t)getU(A(10), AT(10), i) + imm; uint8_t* p = mem(bufIndex(s), a, 4);
                uint32_t v; memcpy(&v, &A(11).b[i * 4], 4);
                if (sub == 11) memcpy(p, &v, 4);
                else if (sub == 0x13) { uint32_t o; memcpy(&o, p, 4); float x; memcpy(&x, &o, 4); float y; memcpy(&y, &v, 4); x += y; memcpy(p, &x, 4); if (!rt->isVoidTy()) memcpy(&r.b[i * 4], &o, 4); }
                else die("atomic subop");
            }
            return r;
        }
        if (starts("llvm.genx.3d.load") || starts("llvm.genx.3d.sample"))
        {
            bool smp = starts("llvm.genx.3d.sample");
            int ch = cint(C.getArgOperand(2)); int sidx = surfaceOf(C.getArgOperand(smp ? 5 : 4)); unsigned ub = smp ? 6 : 5;
            auto& im = imgs[bufIndex(sidx)]; if (im.empty()) die("3d op on missing image");
            vector<int> chans; for (int c = 0; c < 4; ++c) if (ch & (1 << c)) chans.push_back(c);
            unsigned lanes = nElem(AT(1));
            for (unsigned l = 0; l < lanes; ++l)
            {
                if (!(getU(A(1), AT(1), l) & 1)) continue;
                int x, y;
                if (smp) { float u = getF(A(ub), AT(ub), l), v = getF(A(ub + 1), AT(ub + 1), l); x = (int)floorf(u * IMW); y = (int)floorf(v * IMH); }   // harness sampler = nearest
                else { x = (int)getU(A(ub), AT(ub), l); y = (int)getU(A(ub + 1), AT(ub + 1), l); }
                x = x < 0 ? 0 : x >= (int)IMW ? IMW - 1 : x; y = y < 0 ? 0 : y >= (int)IMH ? IMH - 1 : y;
                for (size_t q = 0; q < chans.size(); ++q) setF(r, rt, q * lanes + l, im[(y * IMW + x) * 4 + chans[q]]);
            }
            return r;
        }
        if (starts("llvm.genx.lsc.store.quad.typed.bti") || starts("llvm.genx.lsc.load.merge.quad.typed.bti"))
        {
            bool st = starts("llvm.genx.lsc.store");
            int ch = cint(C.getArgOperand(3)); int sidx = surfaceOf(C.getArgOperand(4));
            auto& im = imgs[bufIndex(sidx)]; if (im.empty()) die("typed op on missing image");
            vector<int> chans; for (int c = 0; c < 4; ++c) if (ch & (1 << c)) chans.push_back(c);
            unsigned lanes = nElem(AT(0)); Val& data = A(9);
            if (!st) r = data;
            for (unsigned l = 0; l < lanes; ++l)
            {
                if (!(getU(A(0), AT(0), l) & 1)) continue;
                unsigned x = (unsigned)getU(A(5), AT(5), l), y = (unsigned)getU(A(6), AT(6), l);
                if (x >= IMW || y >= IMH) continue;
                for (size_t q = 0; q < chans.size(); ++q)
                {
                    float& t = im[(y * IMW + x) * 4 + chans[q]];
                    if (st) t = getF(data, AT(9), q * lanes + l); else setF(r, rt, q * lanes + l, t);
                }
            }
            return r;
        }
        die("unsupported call " + nme);
    }

    void run()
    {
        // arguments are surfaces; nothing to set up
        BasicBlock* bb = &F->getEntryBlock(); BasicBlock* prev = nullptr;
        for (;;)
        {
            // phis (parallel)
            std::vector<std::pair<PHINode*, Val>> ph;
            for (auto& I : *bb) { auto* p = dyn_cast<PHINode>(&I); if (!p) break; ph.push_back({p, get(p->getIncomingValueForBlock(prev))}); }
            for (auto& x : ph) vals[x.first] = x.second;
            BasicBlock* next = nullptr;
            for (auto& I : *bb)
            {
                if (isa<PHINode>(I)) continue;
                if (++steps > kBudget) { fprintf(stderr, "x4rsim: NONTERMINATING (instruction budget exceeded in thread %d)\n", tid); _Exit(5); }
                if (auto* br = dyn_cast<BranchInst>(&I))
                {
                    next = br->isUnconditional() ? br->getSuccessor(0) : ((getU(get(br->getCondition()), br->getCondition()->getType(), 0) & 1) ? br->getSuccessor(0) : br->getSuccessor(1));
                    break;
                }
                if (isa<ReturnInst>(I)) return;
                Val v = exec(I);
                if (!I.getType()->isVoidTy()) { vals[&I] = v; (*trace)[&I] = v; }
            }
            prev = bb; bb = next;
        }
    }
};

int main(int argc, char** argv)
{
    if (argc < 8) die("usage: x4rsim kernel.ll rootsig.txt LX LY LZ trace.txt out.dbg");
    LLVMContext ctx; SMDiagnostic err;
    auto M = parseIRFile(argv[1], err, ctx);
    if (!M) { err.print("x4rsim", errs()); return 1; }
    Function* K = nullptr;
    for (auto& F : *M) if (F.getCallingConv() == CallingConv::SPIR_KERNEL) K = &F;
    if (argc > 9) { trLo = atoi(argv[8]); trHi = atoi(argv[9]); }
    LX = atoi(argv[3]); LY = atoi(argv[4]); LZ = atoi(argv[5]);
    unsigned T = LX * LY * LZ;
    // root signature -> resources
    vector<Res> params;
    { std::ifstream in(argv[2]); string line;
      while (std::getline(in, line)) { std::istringstream ls(line); string w, kind; ls >> w; if (w != "param") continue; unsigned pi; ls >> pi >> kind; Res r;
          if (kind == "table") { string range; ls >> range; char t[16]; unsigned reg, sp, num; long long o; sscanf(range.c_str(), "%15[A-Z]:%u:%u:%u:%lld", t, &reg, &sp, &num, &o); r.kind = t; r.reg = reg; } else r.kind = "CONST";
          if (params.size() <= pi) params.resize(pi + 1); params[pi] = r; } }
    int ri = 0;
    for (auto& A : K->args()) { string tn; raw_string_ostream o(tn); A.getType()->print(o); o.flush(); if (tn.find("spirv.Sampler") != string::npos) continue; resv.push_back(params[ri + 1]); argRes[&A] = ri++; }
    // resources: identical to vkrun (seed 12345): buffers first (types from the layout file), then sampled images
    enum { T_SBUF, T_UBO, T_TEX, T_RWTEX, T_SAMP, T_DBG, T_NONE };
    vector<int> type(56); for (int i = 0; i < 56; ++i) type[i] = i < 32 ? T_SBUF : i < 47 ? T_UBO : i == 47 ? T_DBG : T_NONE;
    if (argc > 10) { std::ifstream lf(argv[10]); int b; string t; while (lf >> b >> t) type[b] = t == "sbuf" ? T_SBUF : t == "ubo" ? T_UBO : t == "tex" ? T_TEX : t == "rwtex" ? T_RWTEX : T_SAMP; }
    rs = 12345; bufs.resize(56);
    for (int i = 0; i < 56; ++i)
    {
        if (type[i] != T_SBUF && type[i] != T_UBO) continue;
        bool ubo = type[i] == T_UBO; unsigned size = ubo ? 65536 : (4u << 20); bufs[i].assign(size, 0);
        for (unsigned k = 0; k < size / 4; ++k)
        {
            uint32_t r = rnd(), v;
            if (ubo) v = 4 + (r % 12);
            else if (i >= 16) v = 0;
            else { float f = ((int32_t)(r & 0xffff) - 32768) / 16384.0f; memcpy(&v, &f, 4); if (rnd() & 1) v = r; }
            memcpy(&bufs[i][k * 4], &v, 4);
        }
    }
    for (int i = 0; i < 56; ++i)
    {
        if (type[i] != T_TEX && type[i] != T_RWTEX) continue;
        auto& im = imgs[i]; im.assign(IMW * IMH * 4, 0.0f);
        if (type[i] == T_TEX) for (auto& f : im) f = (rnd() & 0xffff) / 65536.0f;
    }
    unsigned slmBytes = 0;
    if (K->hasFnAttribute("VCSLMSize")) slmBytes = atoi(K->getFnAttribute("VCSLMSize").getValueAsString().str().c_str());
    slm.assign(std::max(slmBytes, 4u) + 64, 0);
    // trace layout
    std::ifstream tf(argv[6]); string l; std::getline(tf, l); unsigned tot = atoi(l.c_str());
    std::map<string, std::pair<unsigned, unsigned>> layout; // not needed: offsets recomputed in instruction order
    vector<std::map<Instruction*, Val>> traces(T);
    std::barrier<> bar(T);
    vector<std::thread> th; vector<Thread*> ts;
    for (unsigned t = 0; t < T; ++t) { auto* x = new Thread(); x->tid = t; x->trace = &traces[t]; x->F = K; x->bar = &bar; ts.push_back(x); th.emplace_back([x] { x->run(); }); }
    for (auto& t : th) t.join();
    // write per-instruction words in the GPU debug layout
    vector<uint32_t> dbg((size_t)tot * T, 0);
    for (unsigned t = 0; t < T; ++t)
    {
        unsigned off = 0, ord = 0;
        for (auto& BB : *K) for (auto& I : BB)
        {
            if (I.getType()->isVoidTy() || isa<PHINode>(I) || isa<BranchInst>(I)) continue;
            unsigned o = ord++;
            if (o < trLo || o >= trHi) continue;
            Type* ty = I.getType(); unsigned words;
            if (!isa<FixedVectorType>(ty)) words = bitsOf(ty) == 64 ? 2 : 1;
            else if (isI1(ty)) words = nElem(ty);
            else words = (nElem(ty) * elemBytes(ty) + 3) / 4;
            auto it = traces[t].find(&I);
            if (it != traces[t].end())
            {
                const Val& v = it->second;
                if (isa<FixedVectorType>(ty) && isI1(ty)) for (unsigned i = 0; i < words; ++i) dbg[(size_t)t * tot + off + i] = v.b[i] & 1;
                else { vector<uint8_t> tmp(words * 4, 0); memcpy(tmp.data(), v.b.data(), std::min<size_t>(v.b.size(), tmp.size())); if (!isa<FixedVectorType>(ty) && isI1(ty)) { tmp.assign(4, 0); tmp[0] = v.b[0] & 1; } memcpy(&dbg[(size_t)t * tot + off], tmp.data(), words * 4); }
            }
            off += words;
        }
        if (off != tot) die("trace size mismatch: " + std::to_string(off) + " vs " + std::to_string(tot));
    }
    std::ofstream o(argv[7], std::ios::binary); o.write((const char*)dbg.data(), dbg.size() * 4);
    // dump output buffers (u0..u14 at 16..30) for end-to-end comparison
    std::ofstream ob(string(argv[7]) + ".out", std::ios::binary);
    for (int i = 16; i < 31; ++i) if (type[i] == T_SBUF) ob.write((const char*)bufs[i].data(), bufs[i].size());
    return 0;
}
