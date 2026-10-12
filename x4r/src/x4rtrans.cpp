// SPDX-License-Identifier: GPL-3.0-only
// x4rtrans: translate an Intel CM (C-for-Metal) kernel, lifted to LLVM IR with llvm.genx.* intrinsics, to HLSL.
//
// Execution model (first version, correctness before speed): one HLSL invocation executes one CM *thread*, i.e. the
// whole kernel body. Every CM vector lives in a per-invocation `uint[]` in its packed little-endian memory layout
// (so bitcasts are plain copies); an element is read/written through shift+mask accessors. numthreads = the CM
// local size, SV_GroupID = CM group id, SLM = groupshared, barrier = GroupMemoryBarrierWithGroupSync.
//
// usage: x4rtrans kernel.ll rootsig.txt LX LY LZ out.hlsl
#include "llvm/IR/Constants.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/InstIterator.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/IntrinsicInst.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"
#include "llvm/IRReader/IRReader.h"
#include "llvm/Support/SourceMgr.h"
#include "llvm/Support/raw_ostream.h"
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <functional>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <vector>

using namespace llvm;
using std::string;
using std::to_string;

[[noreturn]] static void die(const string& m)
{
    fprintf(stderr, "x4rtrans: %s\n", m.c_str());
    exit(2);
}

enum EK { B1, I8, I16, I32, I64, F16, F32 };

static EK ekOf(Type* t)
{
    t = t->getScalarType();
    if (t->isHalfTy()) return F16;
    if (t->isFloatTy()) return F32;
    if (auto* it = dyn_cast<IntegerType>(t))
        switch (it->getBitWidth()) { case 1: return B1; case 8: return I8; case 16: return I16; case 32: return I32; case 64: return I64; }
    die("unsupported element type");
}
static unsigned bitsOf(EK k) { switch (k) { case B1: return 1; case I8: return 8; case I16: case F16: return 16; case I32: case F32: return 32; case I64: return 64; } return 0; }
static bool isF(EK k) { return k == F16 || k == F32; }
static unsigned lanesOf(Type* t) { return isa<FixedVectorType>(t) ? cast<FixedVectorType>(t)->getNumElements() : 1; }
static unsigned wordsOf(Type* t)
{
    unsigned n = lanesOf(t);
    switch (ekOf(t)) {
    case B1: return n; case I8: return (n + 3) / 4; case I16: case F16: return (n + 1) / 2; case I32: case F32: return n; case I64: return 2 * n;
    }
    return 0;
}

static string hex32(uint32_t v) { char b[32]; snprintf(b, sizeof b, "0x%xu", v); return b; }

struct Res { string kind; unsigned reg = 0; bool image = false; };   // per kernel argument (excluding samplers)

struct Tx
{
    Function* F = nullptr;
    unsigned LX, LY, LZ;
    std::map<Value*, int> ids;
    std::map<BasicBlock*, int> bbIds;
    std::map<Value*, int> surfaceArg;           // result of convert_int(surface arg) -> resource index
    std::vector<Res> res;                       // by resource index
    std::map<Argument*, int> argRes;
    std::ostringstream globals, body, decls;
    int nconst = 0, ntmp = 0;
    unsigned slmBytes = 0;
    bool usesSampler = false;
    bool trace = false;
    std::map<Instruction*, unsigned> tOff;
    unsigned tTotal = 0;
    string ind = "      ";

    // ---------- naming ----------
    string nm(Value* v)
    {
        auto it = ids.find(v);
        if (it == ids.end()) { int id = ids.size(); ids[v] = id; it = ids.find(v); declare(v, id); }
        return "v" + to_string(it->second);
    }
    void declare(Value* v, int id)
    {
        if (!isa<Instruction>(v)) return;
        Type* t = v->getType();
        if (t->isVoidTy()) return;
        decls << "  uint v" << id << "[" << wordsOf(t) << "];\n";
    }

    // ---------- constants ----------
    uint64_t constElemBits(Constant* c, unsigned i)
    {
        Constant* e = isa<FixedVectorType>(c->getType()) ? c->getAggregateElement(i) : c;
        if (!e || isa<UndefValue>(e)) return 0;
        if (auto* ci = dyn_cast<ConstantInt>(e)) return ci->getZExtValue();
        if (auto* cf = dyn_cast<ConstantFP>(e)) return cf->getValueAPF().bitcastToAPInt().getZExtValue();
        if (isa<ConstantPointerNull>(e)) return 0;
        die("unsupported constant element");
    }
    string litFor(EK k, uint64_t bits)
    {
        if (k == I64) return "(uint64_t(" + hex32((uint32_t)bits) + ")|(uint64_t(" + hex32((uint32_t)(bits >> 32)) + ")<<32))";
        if (k == F32) return "asfloat(" + hex32((uint32_t)bits) + ")";
        if (k == F16) return "f16tof32(" + hex32((uint32_t)bits) + ")";
        return hex32((uint32_t)bits);
    }
    // name of a global static const array holding the packed constant
    string constArray(Constant* c)
    {
        Type* t = c->getType();
        unsigned n = lanesOf(t), w = wordsOf(t);
        EK k = ekOf(t);
        std::vector<uint32_t> W(w, 0);
        for (unsigned i = 0; i < n; ++i)
        {
            uint64_t b = constElemBits(c, i);
            switch (k) {
            case B1: W[i] = b & 1; break;
            case I8: W[i / 4] |= (uint32_t)(b & 0xff) << (8 * (i % 4)); break;
            case I16: case F16: W[i / 2] |= (uint32_t)(b & 0xffff) << (16 * (i % 2)); break;
            case I32: case F32: W[i] = (uint32_t)b; break;
            case I64: W[2 * i] = (uint32_t)b; W[2 * i + 1] = (uint32_t)(b >> 32); break;
            }
        }
        string name = "c" + to_string(nconst++);
        globals << "static const uint " << name << "[" << w << "] = {";
        for (unsigned i = 0; i < w; ++i) globals << (i ? "," : "") << hex32(W[i]);
        globals << "};\n";
        return name;
    }
    bool isSplatConst(Constant* c, uint64_t& bits)
    {
        unsigned n = lanesOf(c->getType());
        bits = constElemBits(c, 0);
        for (unsigned i = 1; i < n; ++i) if (constElemBits(c, i) != bits) return false;
        return true;
    }
    std::map<Constant*, string> constNames;

    // ---------- element access ----------
    // read element `idx` (an HLSL expression of type uint) of value v; result is uint (ints, zero extended) or float
    string rd(Value* v, const string& idx)
    {
        EK k = ekOf(v->getType());
        if (auto* c = dyn_cast<Constant>(v))
        {
            uint64_t sb;
            if (!isa<FixedVectorType>(c->getType()) || isSplatConst(c, sb))
            {
                if (isa<FixedVectorType>(c->getType()) || true) return litFor(k, constElemBits(c, 0));
            }
            auto it = constNames.find(c);
            if (it == constNames.end()) it = constNames.emplace(c, constArray(c)).first;
            return rdArr(it->second, k, idx);
        }
        if (auto* a = dyn_cast<Argument>(v)) die("argument used as data: " + std::to_string(a->getArgNo()));
        return rdArr(nm(v), k, idx);
    }
    string rdArr(const string& a, EK k, const string& idx)
    {
        switch (k) {
        case B1: case I32: return a + "[" + idx + "]";
        case F32: return "asfloat(" + a + "[" + idx + "])";
        case I8: return "((" + a + "[(" + idx + ")>>2]>>(((" + idx + ")&3u)<<3))&0xffu)";
        case I16: return "((" + a + "[(" + idx + ")>>1]>>(((" + idx + ")&1u)<<4))&0xffffu)";
        case F16: return "f16tof32((" + a + "[(" + idx + ")>>1]>>(((" + idx + ")&1u)<<4))&0xffffu)";
        case I64: return "(uint64_t(" + a + "[(" + idx + ")*2u]) | (uint64_t(" + a + "[(" + idx + ")*2u+1u])<<32))";
        }
        return "";
    }
    // statement writing expression `e` (uint or float) to element idx of array `a`
    string wr(const string& a, EK k, const string& idx, const string& e)
    {
        switch (k) {
        case B1: return a + "[" + idx + "] = (" + e + ")&1u;";
        case I32: return a + "[" + idx + "] = " + e + ";";
        case F32: return a + "[" + idx + "] = asuint(" + e + ");";
        case I8: return "X4R_SET8(" + a + "," + idx + "," + e + ");";
        case I16: return "X4R_SET16(" + a + "," + idx + "," + e + ");";
        case F16: return "X4R_SET16(" + a + "," + idx + ",f32tof16(" + e + "));";
        case I64: return "{ uint64_t e64_ = " + e + "; " + a + "[(" + idx + ")*2u] = uint(e64_); " + a + "[(" + idx + ")*2u+1u] = uint(e64_>>32); }";
        }
        return "";
    }

    void emit(const string& s) { body << ind << s << "\n"; }
    // emit a loop over n lanes: fn(i) returns statement text, `i` is the loop variable name
    void forLanes(unsigned n, const std::function<string(const string&)>& fn)
    {
        if (n == 1) { emit(fn("0u")); return; }
        if (n <= 4) { for (unsigned i = 0; i < n; ++i) emit(fn(to_string(i) + "u")); return; }
        emit("[loop] for (uint i = 0; i < " + to_string(n) + "; i++) { " + fn("i") + " }");
    }
    void copyWords(const string& dst, const string& src, unsigned w)
    {
        if (w <= 4) { for (unsigned i = 0; i < w; ++i) emit(dst + "[" + to_string(i) + "] = " + src + "[" + to_string(i) + "];"); }
        else emit("[loop] for (uint i = 0; i < " + to_string(w) + "; i++) { " + dst + "[i] = " + src + "[i]; }");
    }
    // source array name of any value (constants materialised)
    string arr(Value* v)
    {
        if (auto* c = dyn_cast<Constant>(v))
        {
            auto it = constNames.find(c);
            if (it == constNames.end()) it = constNames.emplace(c, constArray(c)).first;
            return it->second;
        }
        return nm(v);
    }
    // scalar i32 value as uint expression
    string u32(Value* v) { return rd(v, "0u"); }
    int64_t cint(Value* v)
    {
        auto* c = dyn_cast<ConstantInt>(v);
        if (!c) die("expected constant integer operand");
        return c->getSExtValue();
    }

    // ---------- surfaces ----------
    int surfaceOf(Value* v)
    {
        auto it = surfaceArg.find(v);
        if (it == surfaceArg.end()) die("surface operand is not a convert_int(arg): " + [&] { string s; raw_string_ostream o(s); v->print(o); return s; }());
        return it->second;
    }

    string signedOf(EK k, const string& e)
    {
        if (k == I32) return "asint(" + e + ")";
        if (k == B1) return "asint(" + e + ")";
        if (k == I64) return "int64_t(" + e + ")";
        return "x4r_sx(" + e + "," + to_string(bitsOf(k)) + ")";
    }

    // ---------- instruction translation ----------
    void binop(Instruction& I, const char* ints, const char* floats)
    {
        Type* t = I.getType();
        EK k = ekOf(t);
        unsigned n = lanesOf(t);
        string d = nm(&I);
        if (isF(k) ? !floats : !ints) die("bad binop");
        forLanes(n, [&](const string& i) {
            string a = rd(I.getOperand(0), i), b = rd(I.getOperand(1), i);
            string e = string("(") + a + ") " + (isF(k) ? floats : ints) + " (" + b + ")";
            return wr(d, k, i, e);
        });
    }

    string icmpExpr(CmpInst::Predicate p, EK k, const string& a, const string& b)
    {
        string sa = signedOf(k, a), sb = signedOf(k, b);
        switch (p) {
        case CmpInst::ICMP_EQ: return "((" + a + ")==(" + b + "))";
        case CmpInst::ICMP_NE: return "((" + a + ")!=(" + b + "))";
        case CmpInst::ICMP_ULT: return "((" + a + ")<(" + b + "))";
        case CmpInst::ICMP_ULE: return "((" + a + ")<=(" + b + "))";
        case CmpInst::ICMP_UGT: return "((" + a + ")>(" + b + "))";
        case CmpInst::ICMP_UGE: return "((" + a + ")>=(" + b + "))";
        case CmpInst::ICMP_SLT: return "(" + sa + "<" + sb + ")";
        case CmpInst::ICMP_SLE: return "(" + sa + "<=" + sb + ")";
        case CmpInst::ICMP_SGT: return "(" + sa + ">" + sb + ")";
        case CmpInst::ICMP_SGE: return "(" + sa + ">=" + sb + ")";
        default: die("icmp predicate");
        }
    }
    string fcmpExpr(CmpInst::Predicate p, const string& a, const string& b)
    {
        string nan = "(isnan(" + a + ")||isnan(" + b + "))";
        switch (p) {
        case CmpInst::FCMP_OEQ: return "((" + a + ")==(" + b + "))";
        case CmpInst::FCMP_ONE: return "(((" + a + ")!=(" + b + "))&&!" + nan + ")";
        case CmpInst::FCMP_OLT: return "((" + a + ")<(" + b + "))";
        case CmpInst::FCMP_OLE: return "((" + a + ")<=(" + b + "))";
        case CmpInst::FCMP_OGT: return "((" + a + ")>(" + b + "))";
        case CmpInst::FCMP_OGE: return "((" + a + ")>=(" + b + "))";
        case CmpInst::FCMP_ORD: return "(!" + nan + ")";
        case CmpInst::FCMP_UNO: return nan;
        case CmpInst::FCMP_UEQ: return "(((" + a + ")==(" + b + "))||" + nan + ")";
        case CmpInst::FCMP_UNE: return "(((" + a + ")!=(" + b + "))||" + nan + ")";
        case CmpInst::FCMP_ULT: return "(((" + a + ")<(" + b + "))||" + nan + ")";
        case CmpInst::FCMP_ULE: return "(((" + a + ")<=(" + b + "))||" + nan + ")";
        case CmpInst::FCMP_UGT: return "(((" + a + ")>(" + b + "))||" + nan + ")";
        case CmpInst::FCMP_UGE: return "(((" + a + ")>=(" + b + "))||" + nan + ")";
        default: die("fcmp predicate");
        }
    }

    // ---- genx intrinsics ----
    void rdregion(CallInst& C)
    {
        Type* rt = C.getType();
        Value* src = C.getArgOperand(0);
        EK k = ekOf(rt);
        unsigned n = lanesOf(rt);
        int vs = cint(C.getArgOperand(1)), w = cint(C.getArgOperand(2)), st = cint(C.getArgOperand(3));
        unsigned esz = std::max(1u, bitsOf(k) / 8);
        string off;
        if (auto* co = dyn_cast<ConstantInt>(C.getArgOperand(4))) off = to_string(co->getZExtValue() / esz) + "u";
        else if (isa<FixedVectorType>(C.getArgOperand(4)->getType())) die("vector-indirect rdregion");
        else off = "(" + rd(C.getArgOperand(4), "0u") + "/" + to_string(esz) + "u)";
        string d = nm(&C);
        if (wordsOf(rt)) for (unsigned i = 0; i < wordsOf(rt) && k != B1; ++i) if (bitsOf(k) < 32) emit(d + "[" + to_string(i) + "] = 0u;");
        unsigned total = lanesOf(src->getType());
        auto* coff = dyn_cast<ConstantInt>(C.getArgOperand(4));
        long maxIdx = coff ? (long)(coff->getZExtValue() / esz) + (long)((n - 1) / w) * vs + (long)((n - 1) % w) * st : 1L << 30;
        bool guard = maxIdx >= (long)total;
        forLanes(n, [&](const string& i) {
            string idx = off + " + (" + i + "/" + to_string(w) + "u)*" + to_string(vs) + "u + (" + i + "%" + to_string(w) + "u)*" + to_string(st) + "u";
            if (n == 1) idx = off;
            if (guard) return "{ uint ix_ = " + idx + "; " + wr(d, k, i, "(ix_ < " + to_string(total) + "u ? " + rd(src, "ix_") + " : " + (isF(k) ? "0.0" : "0u") + ")") + " }";
            return wr(d, k, i, rd(src, idx));
        });
    }
    void wrregion(CallInst& C)
    {
        Type* rt = C.getType();
        Value* old = C.getArgOperand(0);
        Value* nw = C.getArgOperand(1);
        EK k = ekOf(rt);
        unsigned n = lanesOf(nw->getType());
        int vs = cint(C.getArgOperand(2)), w = cint(C.getArgOperand(3)), st = cint(C.getArgOperand(4));
        unsigned esz = std::max(1u, bitsOf(k) / 8);
        string off;
        if (auto* co = dyn_cast<ConstantInt>(C.getArgOperand(5))) off = to_string(co->getZExtValue() / esz) + "u";
        else if (isa<FixedVectorType>(C.getArgOperand(5)->getType())) die("vector-indirect wrregion");
        else off = "(" + rd(C.getArgOperand(5), "0u") + "/" + to_string(esz) + "u)";
        string d = nm(&C);
        string mask = "";
        if (auto* cm = dyn_cast<ConstantInt>(C.getArgOperand(7))) { if (!cm->isOne()) return copyOnly(C, old, rt); }
        else mask = rd(C.getArgOperand(7), "0u");
        if (!(isa<UndefValue>(old))) copyWords(d, arr(old), wordsOf(rt));
        else for (unsigned i = 0; i < wordsOf(rt); ++i) if (k == I8 || k == I16 || k == F16) emit(d + "[" + to_string(i) + "] = 0u;");
        string pre = mask.empty() ? "" : "if (" + mask + ") ";
        unsigned total = lanesOf(rt);
        auto* coff = dyn_cast<ConstantInt>(C.getArgOperand(5));
        long maxIdx = coff ? (long)(coff->getZExtValue() / esz) + (long)((n - 1) / w) * vs + (long)((n - 1) % w) * st : 1L << 30;
        bool guard = maxIdx >= (long)total;
        forLanes(n, [&](const string& i) {
            string idx = off + " + (" + i + "/" + to_string(w) + "u)*" + to_string(vs) + "u + (" + i + "%" + to_string(w) + "u)*" + to_string(st) + "u";
            if (n == 1) idx = off;
            if (guard) return "{ uint ix_ = " + idx + "; if (ix_ < " + to_string(total) + "u) " + pre + wr(d, k, "ix_", rd(nw, i)) + " }";
            return pre + wr(d, k, idx, rd(nw, i));
        });
    }
    void copyOnly(CallInst& C, Value* old, Type* rt) { copyWords(nm(&C), arr(old), wordsOf(rt)); }

    // dst[r][n] = acc[r][n] + sum_{d<8,k<4} A[r][d*4+k] * B[n][d*4+k]   (int8 DPAS, systolic depth 8)
    void dpas(CallInst& C, Value* acc, Value* B, Value* A, unsigned precB, unsigned precA, unsigned rep)
    {
        string d = nm(&C);
        // vISA precision enum: 7 = U8, 8 = S8
        auto isSigned = [&](unsigned p) { if (p == 8) return true; if (p == 7) return false; die("dpas: unsupported precision " + to_string(p)); };
        bool sB = isSigned(precB), sA = isSigned(precA);
        string a = arr(A), b = arr(B);
        string accr = acc ? arr(acc) : "";
        emit("[loop] for (uint r = 0; r < " + to_string(rep) + "; r++) {");
        emit("  [loop] for (uint n = 0; n < 16; n++) {");
        emit("    int s = " + (acc ? "asint(" + accr + "[r*16+n])" : string("0")) + ";");
        emit("    [loop] for (uint k = 0; k < 8; k++) {");
        if (sA && sB) emit("      s = dot4add_i8packed(" + a + "[r*8+k], " + b + "[k*16+n], s);");
        else if (!sA && !sB) emit("      s = asint(dot4add_u8packed(" + a + "[r*8+k], " + b + "[k*16+n], asuint(s)));");
        else
        {
            emit("      uint aw = " + a + "[r*8+k], bw = " + b + "[k*16+n];");
            for (int q = 0; q < 4; ++q)
            {
                string ae = sA ? "x4r_sx((aw>>" + to_string(8 * q) + ")&0xffu,8)" : "int((aw>>" + to_string(8 * q) + ")&0xffu)";
                string be = sB ? "x4r_sx((bw>>" + to_string(8 * q) + ")&0xffu,8)" : "int((bw>>" + to_string(8 * q) + ")&0xffu)";
                emit("      s += " + ae + " * " + be + ";");
            }
        }
        emit("    }");
        emit("    " + d + "[r*16+n] = asuint(s);");
        emit("  }");
        emit("}");
    }

    string byteAddr(Value* off, Value* imm = nullptr)
    {
        string e = u32(off);
        return e;
    }

    // load/store of `words` contiguous 32-bit words from surface `s` at byte address `addr` into/from array
    void bufLoad(int s, const string& addr, const string& dst, unsigned words)
    {
        const Res& r = res[s];
        if (r.kind == "CBV")
        {
            emit("[loop] for (uint i = 0; i < " + to_string(words) + "; i++) { uint wa = (" + addr + ")/4u + i; " + dst + "[i] = CBD" + to_string(s) + "[wa>>2][wa&3u]; }");
            return;
        }
        string R = "R" + to_string(s);
        if (words % 4 == 0 && words >= 4)
            emit("[loop] for (uint i = 0; i < " + to_string(words / 4) + "; i++) { uint4 q = " + R + ".Load4((" + addr + ") + i*16u); " + dst + "[i*4+0]=q.x; " + dst + "[i*4+1]=q.y; " + dst + "[i*4+2]=q.z; " + dst + "[i*4+3]=q.w; }");
        else
            emit("[loop] for (uint i = 0; i < " + to_string(words) + "; i++) { " + dst + "[i] = " + R + ".Load((" + addr + ") + i*4u); }");
    }
    void bufStore(int s, const string& addr, const string& src, unsigned words)
    {
        const Res& r = res[s];
        if (r.kind != "UAV") die("store to non-UAV surface " + to_string(s));
        string R = "R" + to_string(s);
        if (words % 4 == 0 && words >= 4)
            emit("[loop] for (uint i = 0; i < " + to_string(words / 4) + "; i++) { " + R + ".Store4((" + addr + ") + i*16u, uint4(" + src + "[i*4+0]," + src + "[i*4+1]," + src + "[i*4+2]," + src + "[i*4+3])); }");
        else
            emit("[loop] for (uint i = 0; i < " + to_string(words) + "; i++) { " + R + ".Store((" + addr + ") + i*4u, " + src + "[i]); }");
    }

    void lscLoadStore(CallInst& C, bool isStore, bool slm)
    {
        // args: pred, subop, l1, l3, scale, imm, dsize, vsize, order, chmask, offsets, [data,] surface
        int order = cint(C.getArgOperand(8));
        bool singleDword = order == 1 && cint(C.getArgOperand(7)) == 1 && cint(C.getArgOperand(6)) == 3;   // one dword: same as a transposed block
        if (order != 2 && !singleDword) die("non-transposed LSC access");
        unsigned dsz = cint(C.getArgOperand(6));
        (void)dsz;
        int64_t imm = cint(C.getArgOperand(5)), scale = cint(C.getArgOperand(4));
        string addr = "(" + u32(C.getArgOperand(10)) + ")*" + to_string(scale) + "u + " + to_string(imm) + "u";
        Value* data = isStore ? C.getArgOperand(11) : nullptr;
        Type* dt = isStore ? data->getType() : C.getType();
        unsigned words = wordsOf(dt);
        if (!slm)
        {
            int s = surfaceOf(C.getArgOperand(isStore ? 12 : 11));
            if (isStore) bufStore(s, addr, arr(data), words);
            else bufLoad(s, addr, nm(&C), words);
        }
        else
        {
            if (isStore) emit("[loop] for (uint i = 0; i < " + to_string(words) + "; i++) { slm[((" + addr + ")>>2) + i] = " + arr(data) + "[i]; }");
            else emit("[loop] for (uint i = 0; i < " + to_string(words) + "; i++) { " + nm(&C) + "[i] = slm[((" + addr + ")>>2) + i]; }");
        }
    }

    void call(CallInst& C)
    {
        Function* f = C.getCalledFunction();
        if (!f) die("indirect call");
        string n = f->getName().str();
        Type* rt = C.getType();
        auto starts = [&](const char* p) { return n.rfind(p, 0) == 0; };
        string d = rt->isVoidTy() ? "" : nm(&C);

        // ---- OpenCL builtins ----
        if (n.find("convert_int") != string::npos && C.arg_size() == 1 && C.getArgOperand(0)->getType()->isTargetExtTy())
        {
            auto* a = cast<Argument>(C.getArgOperand(0));
            auto it = argRes.find(a);
            if (it != argRes.end()) surfaceArg[&C] = it->second;
            else surfaceArg[&C] = -1;   // sampler
            emit(d + "[0] = " + to_string(surfaceArg[&C] < 0 ? 0 : surfaceArg[&C]) + "u;");
            return;
        }
        if (n == "_Z12get_group_idj") { emit(d + "[0] = gid[" + u32(C.getArgOperand(0)) + "]; " + d + "[1] = 0u;"); return; }
        if (n == "_Z12get_local_idj") { emit(d + "[0] = tid[" + u32(C.getArgOperand(0)) + "]; " + d + "[1] = 0u;"); return; }
        if (n == "_Z14get_local_sizej") { emit(d + "[0] = lsz[" + u32(C.getArgOperand(0)) + "]; " + d + "[1] = 0u;"); return; }
        if (n == "_Z19get_local_linear_idv") { emit(d + "[0] = tid.x + tid.y*" + to_string(LX) + "u + tid.z*" + to_string(LX * LY) + "u; " + d + "[1] = 0u;"); return; }
        if (n == "_Z20get_global_linear_idv")
        {
            emit(d + "[0] = (gid.x + gid.y*ngx + gid.z*ngx*ngy)*" + to_string(LX * LY * LZ) + "u + tid.x + tid.y*" + to_string(LX) + "u + tid.z*" + to_string(LX * LY) + "u; " + d + "[1] = 0u;");
            return;
        }
        if (n == "_Z7barrierj") { emit("GroupMemoryBarrierWithGroupSync();"); return; }
        if (starts("_Z") && n.find("native_") != string::npos)
        {
            EK k = ekOf(rt);
            string fn = n.find("log2") != string::npos ? "log2" : n.find("exp2") != string::npos ? "exp2" : n.find("sqrt") != string::npos ? "sqrt" :
                        n.find("recip") != string::npos ? "x4r_rcp" : n.find("powr") != string::npos ? "pow" : n.find("cos") != string::npos ? "cos" :
                        n.find("sin") != string::npos ? "sin" : "";
            if (fn.empty()) die("unknown native function " + n);
            unsigned cnt = lanesOf(rt);
            bool two = C.arg_size() == 2;
            forLanes(cnt, [&](const string& i) {
                string a = rd(C.getArgOperand(0), i);
                string e = two ? fn + "(" + a + "," + rd(C.getArgOperand(1), i) + ")" : fn + "(" + a + ")";
                return wr(d, k, i, e);
            });
            return;
        }
        if (starts("_Z") && n.find("convert_") != string::npos)
        {
            // convert_<dst><N>(<src vector>) with the src/dst element types taken from the IR types
            Type* st = C.getArgOperand(0)->getType();
            EK sk = ekOf(st), dk = ekOf(rt);
            // signedness of the source from the mangled suffix: c s i l = signed, h t j m = unsigned, f/Dh float
            bool srcSigned = n.size() > 0 && (n.back() == 'c' || n.back() == 's' || n.back() == 'i' || n.back() == 'l');
            // destination signedness from the name prefix: convert_uint / convert_uchar ...
            if (sk == I64 || dk == I64)
            {
                // only zero/sign extension from i32 and truncation to i32 are used
                if (dk == I64 && sk == I32)
                {
                    forLanes(lanesOf(rt), [&](const string& i) {
                        string v = rd(C.getArgOperand(0), i);
                        string hi = srcSigned ? "((" + v + ")>>31 ? 0xffffffffu : 0u)" : "0u";
                        return d + "[(" + i + ")*2] = " + v + "; " + d + "[(" + i + ")*2+1] = " + hi + ";";
                    });
                    return;
                }
                if (sk == I64 && dk == I32)
                {
                    string s = arr(C.getArgOperand(0));
                    forLanes(lanesOf(rt), [&](const string& i) { return d + "[" + i + "] = " + s + "[(" + i + ")*2];"; });
                    return;
                }
                die("unsupported 64-bit convert " + n);
            }
            forLanes(lanesOf(rt), [&](const string& i) {
                string v = rd(C.getArgOperand(0), i), e;
                if (isF(sk) && isF(dk)) e = v;
                else if (isF(dk)) e = srcSigned ? "float(" + signedOf(sk, v) + ")" : "float(" + v + ")";
                else if (isF(sk))
                {
                    bool dstUnsigned = n.find("convert_u") != string::npos;
                    // saturating (as OpenCL convert_ without _sat is UB out of range; clamp to be safe)
                    unsigned bits = bitsOf(dk);
                    if (dstUnsigned) e = "uint(clamp(trunc(" + v + "), 0.0, " + (bits == 32 ? "4294967040.0" : to_string((1u << bits) - 1) + ".0") + "))";
                    else e = "asuint(int(clamp(trunc(" + v + "), " + (bits == 32 ? "-2147483648.0" : to_string(-(1 << (bits - 1))) + ".0") + ", " + (bits == 32 ? "2147483520.0" : to_string((1 << (bits - 1)) - 1) + ".0") + ")))";
                }
                else  // int -> int
                    e = srcSigned ? "asuint(" + signedOf(sk, v) + ")" : v;
                return wr(d, dk, i, e);
            });
            return;
        }

        // ---- generic llvm intrinsics ----
        if (starts("llvm.vector.reduce.mul"))
        {
            Value* x = C.getArgOperand(0);
            unsigned cnt = lanesOf(x->getType());
            string e = rd(x, "0u");
            for (unsigned i = 1; i < cnt; ++i) e += "*" + rd(x, to_string(i) + "u");
            emit(d + "[0] = " + e + ";");
            return;
        }
        if (starts("llvm.fmuladd") || starts("llvm.fma"))
        {
            EK k = ekOf(rt);
            forLanes(lanesOf(rt), [&](const string& i) { return wr(d, k, i, "mad(" + rd(C.getArgOperand(0), i) + "," + rd(C.getArgOperand(1), i) + "," + rd(C.getArgOperand(2), i) + ")"); });
            return;
        }
        if (starts("llvm.sqrt") || starts("llvm.fabs") || starts("llvm.floor") || starts("llvm.ceil") || starts("llvm.trunc") || starts("llvm.exp2") || starts("llvm.log2") || starts("llvm.rint"))
        {
            EK k = ekOf(rt);
            string fn = starts("llvm.sqrt") ? "sqrt" : starts("llvm.fabs") ? "abs" : starts("llvm.floor") ? "floor" : starts("llvm.ceil") ? "ceil" : starts("llvm.trunc") ? "trunc" : starts("llvm.exp2") ? "exp2" : starts("llvm.log2") ? "log2" : "round";
            forLanes(lanesOf(rt), [&](const string& i) { return wr(d, k, i, fn + "(" + rd(C.getArgOperand(0), i) + ")"); });
            return;
        }
        if (starts("llvm.maxnum") || starts("llvm.minnum") || starts("llvm.maximum") || starts("llvm.minimum"))
        {
            EK k = ekOf(rt);
            string fn = (n.find("max") != string::npos) ? "max" : "min";
            forLanes(lanesOf(rt), [&](const string& i) { return wr(d, k, i, fn + "(" + rd(C.getArgOperand(0), i) + "," + rd(C.getArgOperand(1), i) + ")"); });
            return;
        }
        if (starts("llvm.smax") || starts("llvm.smin") || starts("llvm.umax") || starts("llvm.umin") || starts("llvm.abs"))
        {
            EK k = ekOf(rt);
            bool sg = n[5] == 's' || starts("llvm.abs");
            string fn = n.find("max") != string::npos ? "max" : n.find("min") != string::npos ? "min" : "abs";
            forLanes(lanesOf(rt), [&](const string& i) {
                string a = rd(C.getArgOperand(0), i);
                if (fn == "abs") return wr(d, k, i, "asuint(abs(" + signedOf(k, a) + "))");
                string b = rd(C.getArgOperand(1), i);
                return wr(d, k, i, sg ? "asuint(" + fn + "(" + signedOf(k, a) + "," + signedOf(k, b) + "))" : fn + "(" + a + "," + b + ")");
            });
            return;
        }
        if (starts("llvm.lifetime") || starts("llvm.assume") || starts("llvm.dbg")) return;

        // ---- genx intrinsics ----
        if (starts("llvm.genx.rdregion")) { rdregion(C); return; }
        if (starts("llvm.genx.wrregion")) { wrregion(C); return; }
        if (starts("llvm.genx.vload") || starts("llvm.genx.vstore")) die("vload/vstore must be lowered first (prelower.py)");
        if (starts("llvm.genx.fmax") || starts("llvm.genx.fmin") || starts("llvm.genx.smax") || starts("llvm.genx.smin") || starts("llvm.genx.umax") || starts("llvm.genx.umin"))
        {
            EK k = ekOf(rt);
            bool sg = n[11] == 's';
            string fn = n.find("max") != string::npos ? "max" : "min";
            forLanes(lanesOf(rt), [&](const string& i) {
                string a = rd(C.getArgOperand(0), i), b = rd(C.getArgOperand(1), i);
                if (isF(k)) return wr(d, k, i, fn + "(" + a + "," + b + ")");
                return wr(d, k, i, sg ? "asuint(" + fn + "(" + signedOf(k, a) + "," + signedOf(k, b) + "))" : fn + "(" + a + "," + b + ")");
            });
            return;
        }
        if (starts("llvm.genx.rndd") || starts("llvm.genx.rndu") || starts("llvm.genx.rnde") || starts("llvm.genx.rndz"))
        {
            EK k = ekOf(rt);
            string fn = starts("llvm.genx.rndd") ? "floor" : starts("llvm.genx.rndu") ? "ceil" : starts("llvm.genx.rnde") ? "round" : "trunc";
            forLanes(lanesOf(rt), [&](const string& i) { return wr(d, k, i, fn + "(" + rd(C.getArgOperand(0), i) + ")"); });
            return;
        }
        if (starts("llvm.genx.fptosi.sat") || starts("llvm.genx.fptoui.sat"))
        {
            EK k = ekOf(rt);
            bool sg = starts("llvm.genx.fptosi");
            unsigned bits = bitsOf(k);
            double lo = sg ? -(double)(1ull << (bits - 1)) : 0.0, hi = sg ? (double)((1ull << (bits - 1)) - 1) : (double)((1ull << bits) - 1);
            if (bits == 32) { hi = sg ? 2147483520.0 : 4294967040.0; }
            char lb[64], hb[64]; snprintf(lb, sizeof lb, "%.1f", lo); snprintf(hb, sizeof hb, "%.1f", hi);
            forLanes(lanesOf(rt), [&](const string& i) {
                string v = "clamp(trunc(" + rd(C.getArgOperand(0), i) + "), " + lb + ", " + hb + ")";
                return wr(d, k, i, sg ? "asuint(int(" + v + "))" : "uint(" + v + ")");
            });
            return;
        }
        if (starts("llvm.genx.sstrunc.sat") || starts("llvm.genx.ustrunc.sat") || starts("llvm.genx.sutrunc.sat") || starts("llvm.genx.ustrunc"))
        {
            Type* st = C.getArgOperand(0)->getType();
            EK sk = ekOf(st), dk = ekOf(rt);
            bool sSrc = starts("llvm.genx.ss"), sDst = starts("llvm.genx.ss") || starts("llvm.genx.su") == false ? starts("llvm.genx.ss") : false;
            // ss: signed->signed, us: unsigned->unsigned, su: signed->unsigned
            bool dstSigned = starts("llvm.genx.ss");
            unsigned bits = bitsOf(dk);
            (void)sDst;
            forLanes(lanesOf(rt), [&](const string& i) {
                string a = rd(C.getArgOperand(0), i);
                string e;
                if (sSrc || starts("llvm.genx.su"))
                {
                    string s = signedOf(sk, a);
                    if (dstSigned) e = "asuint(clamp(" + s + "," + to_string(-(1 << (bits - 1))) + "," + to_string((1 << (bits - 1)) - 1) + "))";
                    else e = "uint(clamp(" + s + ",0," + to_string((1 << bits) - 1) + "))";
                }
                else e = "min(" + a + "," + to_string((1u << bits) - 1) + "u)";
                return wr(d, dk, i, e);
            });
            return;
        }
        if (starts("llvm.genx.dpas2"))
        {
            dpas(C, C.getArgOperand(0), C.getArgOperand(1), C.getArgOperand(2), cint(C.getArgOperand(3)), cint(C.getArgOperand(4)), cint(C.getArgOperand(6)));
            return;
        }
        if (starts("llvm.genx.dpas.nosrc0"))
        {
            uint32_t info = (uint32_t)cint(C.getArgOperand(2));
            dpas(C, nullptr, C.getArgOperand(0), C.getArgOperand(1), info & 0xff, (info >> 8) & 0xff, (info >> 24) & 0xff);
            return;
        }
        if (starts("llvm.genx.oword.ld"))
        {
            // (i32 modifier, i32 surface, i32 offset (bytes for .unaligned, owords for .ld))
            int s = surfaceOf(C.getArgOperand(1));
            string off = u32(C.getArgOperand(2));
            if (n.find("unaligned") == string::npos) off = "(" + off + ")*16u";
            bufLoad(s, off, d, wordsOf(rt));
            return;
        }
        if (starts("llvm.genx.oword.st"))
        {
            int s = surfaceOf(C.getArgOperand(0));
            bufStore(s, "(" + u32(C.getArgOperand(1)) + ")*16u", arr(C.getArgOperand(2)), wordsOf(C.getArgOperand(2)->getType()));
            return;
        }
        if (starts("llvm.genx.lsc.load.bti")) { lscLoadStore(C, false, false); return; }
        if (starts("llvm.genx.lsc.store.bti")) { lscLoadStore(C, true, false); return; }
        if (starts("llvm.genx.lsc.load.slm")) { lscLoadStore(C, false, true); return; }
        if (starts("llvm.genx.lsc.store.slm")) { lscLoadStore(C, true, true); return; }
        if (starts("llvm.genx.fence")) { emit("AllMemoryBarrier();"); return; }
        if (starts("llvm.genx.gather.masked.scaled2") || starts("llvm.genx.gather.scaled2"))
        {
            // (log2 nblocks, scale, surface, global offset, element offsets, [mask]); surface 254 = SLM
            int64_t surf = cint(C.getArgOperand(2));
            if (surf != 254) die("gather from non-SLM surface");
            EK k = ekOf(rt);
            string goff = u32(C.getArgOperand(3));
            string mask = C.arg_size() > 5 ? "" : "";
            forLanes(lanesOf(rt), [&](const string& i) {
                string a = "(" + goff + " + " + rd(C.getArgOperand(4), i) + ")";
                string cond = C.arg_size() > 5 ? "if (" + rd(C.getArgOperand(5), i) + ") " : "";
                return cond + d + "[" + i + "] = slm[" + a + ">>2];";
            });
            (void)k;
            return;
        }
        if (starts("llvm.genx.scatter.scaled"))
        {
            // (mask, log2 nblocks, scale, surface, global offset, element offsets, data)
            int64_t surf = cint(C.getArgOperand(3));
            if (surf != 254) die("scatter to non-SLM surface");
            string goff = u32(C.getArgOperand(4));
            forLanes(lanesOf(C.getArgOperand(5)->getType()), [&](const string& i) {
                string a = "(" + goff + " + " + rd(C.getArgOperand(5), i) + ")";
                return "if (" + rd(C.getArgOperand(0), i) + ") slm[" + a + ">>2] = " + arr(C.getArgOperand(6)) + "[" + i + "];";
            });
            return;
        }
        if (starts("llvm.genx.lsc.xatomic.bti"))
        {
            // (pred, subop, l1, l3, scale, imm, dsize, vsize, order, chmask, offsets, src0, src1, surface, old)
            int sub = cint(C.getArgOperand(1));
            int s = surfaceOf(C.getArgOperand(13));
            string R = "R" + to_string(s);
            unsigned cnt = lanesOf(C.getArgOperand(0)->getType());
            Value* o = C.getArgOperand(10);
            Value* src0 = C.getArgOperand(11);
            string imm = to_string(cint(C.getArgOperand(5))) + "u";
            if (sub == 11)   // LSC_ATOMIC_STORE
            {
                forLanes(cnt, [&](const string& i) { return "if (" + rd(C.getArgOperand(0), i) + ") " + R + ".Store(" + rd(o, i) + " + " + imm + ", asuint(" + rd(src0, i) + "));"; });
                if (!rt->isVoidTy()) copyWords(d, arr(C.getArgOperand(14)), wordsOf(rt));
                return;
            }
            if (sub == 10)   // LSC_ATOMIC_LOAD
            {
                forLanes(cnt, [&](const string& i) { return "if (" + rd(C.getArgOperand(0), i) + ") " + d + "[" + i + "] = " + R + ".Load(" + rd(o, i) + " + " + imm + ");"; });
                return;
            }
            if (sub == 0x13)   // FADD
            {
                forLanes(cnt, [&](const string& i) {
                    return "if (" + rd(C.getArgOperand(0), i) + ") { uint a = " + rd(o, i) + " + " + imm + "; uint ov = " + R + ".Load(a), nv; [loop] while (true) { nv = asuint(asfloat(ov) + " + rd(src0, i) + "); uint pv; " + R + ".InterlockedCompareExchange(a, ov, nv, pv); if (pv == ov) break; ov = pv; } " + d + "[" + i + "] = ov; }";
                });
                return;
            }
            if (sub == 12)   // IADD
            {
                forLanes(cnt, [&](const string& i) { return "if (" + rd(C.getArgOperand(0), i) + ") { uint ov; " + R + ".InterlockedAdd(" + rd(o, i) + " + " + imm + ", " + rd(src0, i) + ", ov); " + d + "[" + i + "] = ov; }"; });
                return;
            }
            die("unsupported atomic subop " + to_string(sub));
        }
        if (starts("llvm.genx.3d.load"))
        {
            // (op, mask, chmask, aoffimm, surface, u, v, r, lod, ...): per-lane Texture2D.Load; result is channel-major
            int ch = cint(C.getArgOperand(2));
            int s = surfaceOf(C.getArgOperand(4));
            unsigned lanes = lanesOf(C.getArgOperand(1)->getType());
            int nch = 0; for (int c = 0; c < 4; ++c) if (!(ch & (1 << c))) {}   // note: for 3d.load the mask is "channels to read"
            nch = __builtin_popcount(ch & 15);
            if (lanesOf(rt) != lanes * nch) die("3d.load: result size does not match channel mask");
            EK k = ekOf(rt);
            string R = "R" + to_string(s);
            // enabled channels are the set bits, returned compactly in channel order
            std::vector<int> chans; for (int c = 0; c < 4; ++c) if (ch & (1 << c)) chans.push_back(c);
            forLanes(lanes, [&](const string& i) {
                string o = "if (" + rd(C.getArgOperand(1), i) + ") { float4 t = " + R + ".Load(int3(" + signedOf(I32, rd(C.getArgOperand(5), i)) + "," + signedOf(I32, rd(C.getArgOperand(6), i)) + ",0)); ";
                for (size_t q = 0; q < chans.size(); ++q) o += wr(d, k, "(" + to_string(q * lanes) + "u + " + i + ")", string("t.") + "xyzw"[chans[q]]) + " ";
                return o + "}";
            });
            return;
        }
        if (starts("llvm.genx.3d.sample"))
        {
            int ch = cint(C.getArgOperand(2));
            int smp = surfaceArg.count(C.getArgOperand(4)) ? 0 : 0;
            (void)smp;
            int s = surfaceOf(C.getArgOperand(5));
            unsigned lanes = lanesOf(C.getArgOperand(1)->getType());
            usesSampler = true;
            EK k = ekOf(rt);
            std::vector<int> chans; for (int c = 0; c < 4; ++c) if (ch & (1 << c)) chans.push_back(c);
            if (lanesOf(rt) != lanes * chans.size()) die("3d.sample: result size does not match channel mask");
            string R = "R" + to_string(s);
            forLanes(lanes, [&](const string& i) {
                string o = "if (" + rd(C.getArgOperand(1), i) + ") { float4 t = " + R + ".SampleLevel(smp0, float2(" + rd(C.getArgOperand(6), i) + "," + rd(C.getArgOperand(7), i) + "), 0.0); ";
                for (size_t q = 0; q < chans.size(); ++q) o += wr(d, k, "(" + to_string(q * lanes) + "u + " + i + ")", string("t.") + "xyzw"[chans[q]]) + " ";
                return o + "}";
            });
            return;
        }
        if (starts("llvm.genx.lsc.store.quad.typed.bti") || starts("llvm.genx.lsc.load.merge.quad.typed.bti"))
        {
            bool st = starts("llvm.genx.lsc.store");
            // (pred, i8, i8, chmask, surface, u, v, r, lod, data/merge)
            int ch = cint(C.getArgOperand(3));
            int s = surfaceOf(C.getArgOperand(4));
            unsigned lanes = lanesOf(C.getArgOperand(0)->getType());
            std::vector<int> chans; for (int c = 0; c < 4; ++c) if (ch & (1 << c)) chans.push_back(c);
            string R = "R" + to_string(s);
            Value* data = C.getArgOperand(9);
            EK k = ekOf(data->getType());
            if (st)
            {
                if (lanesOf(data->getType()) != lanes * 4 && lanesOf(data->getType()) != lanes * chans.size()) die("typed store size");
                forLanes(lanes, [&](const string& i) {
                    string t = "float4(";
                    for (int c = 0; c < 4; ++c)
                    {
                        bool en = ch & (1 << c);
                        // data is channel-major over the enabled channels
                        size_t q = 0; for (size_t z = 0; z < chans.size(); ++z) if (chans[z] == c) q = z;
                        t += (c ? "," : "") + (en ? rd(data, "(" + to_string(q * lanes) + "u + " + i + ")") : string("0.0"));
                    }
                    t += ")";
                    return "if (" + rd(C.getArgOperand(0), i) + ") " + R + "[uint2(" + rd(C.getArgOperand(5), i) + "," + rd(C.getArgOperand(6), i) + ")] = " + t + ";";
                });
            }
            else
            {
                copyWords(d, arr(data), wordsOf(rt));
                forLanes(lanes, [&](const string& i) {
                    string o = "if (" + rd(C.getArgOperand(0), i) + ") { float4 t = " + R + "[uint2(" + rd(C.getArgOperand(5), i) + "," + rd(C.getArgOperand(6), i) + ")]; ";
                    for (size_t q = 0; q < chans.size(); ++q) o += wr(d, k, "(" + to_string(q * lanes) + "u + " + i + ")", string("t.") + "xyzw"[chans[q]]) + " ";
                    return o + "}";
                });
            }
            return;
        }
        die("unsupported call: " + n);
    }

    void phiCopies(BasicBlock* from, BasicBlock* to)
    {
        std::vector<PHINode*> phis;
        for (auto& I : *to) { if (auto* p = dyn_cast<PHINode>(&I)) phis.push_back(p); else break; }
        if (phis.empty()) return;
        // incoming values that are themselves phis of `to` need their old values: go through temporaries
        std::set<Value*> phiSet(phis.begin(), phis.end());
        bool needTmp = false;
        for (auto* p : phis) if (phiSet.count(p->getIncomingValueForBlock(from))) needTmp = true;
        if (!needTmp)
        {
            for (auto* p : phis) copyWords(nm(p), arr(p->getIncomingValueForBlock(from)), wordsOf(p->getType()));
            return;
        }
        std::vector<string> tmps;
        for (auto* p : phis)
        {
            string t = "t" + to_string(ntmp++);
            decls << "  uint " << t << "[" << wordsOf(p->getType()) << "];\n";
            copyWords(t, arr(p->getIncomingValueForBlock(from)), wordsOf(p->getType()));
            tmps.push_back(t);
        }
        for (size_t q = 0; q < phis.size(); ++q) copyWords(nm(phis[q]), tmps[q], wordsOf(phis[q]->getType()));
    }

    void instr(Instruction& I)
    {
        switch (I.getOpcode())
        {
        case Instruction::PHI: nm(&I); return;
        case Instruction::Add: binop(I, "+", nullptr); return;
        case Instruction::Sub: binop(I, "-", nullptr); return;
        case Instruction::Mul: binop(I, "*", nullptr); return;
        case Instruction::And: binop(I, "&", nullptr); return;
        case Instruction::Or: binop(I, "|", nullptr); return;
        case Instruction::Xor: binop(I, "^", nullptr); return;
        case Instruction::UDiv: binop(I, "/", nullptr); return;
        case Instruction::URem: binop(I, "%", nullptr); return;
        case Instruction::FAdd: binop(I, nullptr, "+"); return;
        case Instruction::FSub: binop(I, nullptr, "-"); return;
        case Instruction::FMul: binop(I, nullptr, "*"); return;
        case Instruction::FDiv: binop(I, nullptr, "/"); return;
        case Instruction::FRem: binop(I, nullptr, "%"); return;
        case Instruction::FNeg: {
            EK k = ekOf(I.getType()); string d = nm(&I);
            forLanes(lanesOf(I.getType()), [&](const string& i) { return wr(d, k, i, "-(" + rd(I.getOperand(0), i) + ")"); });
            return;
        }
        case Instruction::Shl: case Instruction::LShr: case Instruction::AShr: case Instruction::SDiv: case Instruction::SRem: {
            EK k = ekOf(I.getType()); string d = nm(&I);
            unsigned bits = bitsOf(k);
            forLanes(lanesOf(I.getType()), [&](const string& i) {
                string a = rd(I.getOperand(0), i), b = rd(I.getOperand(1), i), e;
                switch (I.getOpcode()) {
                case Instruction::Shl: e = "((" + b + ")>=" + to_string(bits) + "u ? 0u : ((" + a + ")<<(" + b + ")))"; break;
                case Instruction::LShr: e = "((" + b + ")>=" + to_string(bits) + "u ? 0u : ((" + a + ")>>(" + b + ")))"; break;
                case Instruction::AShr: e = (k == I64 ? "uint64_t(" : "asuint(") + signedOf(k, a) + ">>min(" + b + "," + to_string(bits - 1) + "u))"; break;
                case Instruction::SDiv: e = (k == I64 ? "uint64_t(" : "asuint(") + signedOf(k, a) + "/" + signedOf(k, b) + ")"; break;
                default: e = (k == I64 ? "uint64_t(" : "asuint(") + signedOf(k, a) + "%" + signedOf(k, b) + ")"; break;
                }
                return wr(d, k, i, e);
            });
            return;
        }
        case Instruction::ICmp: {
            auto& C = cast<ICmpInst>(I); EK ok = ekOf(C.getOperand(0)->getType()); string d = nm(&I);
            forLanes(lanesOf(I.getType()), [&](const string& i) { return wr(d, B1, i, icmpExpr(C.getPredicate(), ok, rd(C.getOperand(0), i), rd(C.getOperand(1), i))); });
            return;
        }
        case Instruction::FCmp: {
            auto& C = cast<FCmpInst>(I); string d = nm(&I);
            forLanes(lanesOf(I.getType()), [&](const string& i) { return wr(d, B1, i, fcmpExpr(C.getPredicate(), rd(C.getOperand(0), i), rd(C.getOperand(1), i))); });
            return;
        }
        case Instruction::Select: {
            EK k = ekOf(I.getType()); string d = nm(&I);
            if (k == I64) { copyWords(d, arr(I.getOperand(2)), wordsOf(I.getType())); emit("if (" + rd(I.getOperand(0), "0u") + ") {"); copyWords(d, arr(I.getOperand(1)), wordsOf(I.getType())); emit("}"); return; }
            bool vc = isa<FixedVectorType>(I.getOperand(0)->getType());
            forLanes(lanesOf(I.getType()), [&](const string& i) {
                string c = rd(I.getOperand(0), vc ? i : "0u");
                return wr(d, k, i, "((" + c + ") ? (" + rd(I.getOperand(1), i) + ") : (" + rd(I.getOperand(2), i) + "))");
            });
            return;
        }
        case Instruction::Trunc: case Instruction::ZExt: case Instruction::SExt: {
            EK sk = ekOf(I.getOperand(0)->getType()), dk = ekOf(I.getType()); string d = nm(&I);
            if (sk == I64 || dk == I64)
            {
                if (sk == I64 && dk != I64) { string s = arr(I.getOperand(0)); forLanes(lanesOf(I.getType()), [&](const string& i) { return wr(d, dk, i, s + "[(" + i + ")*2]"); }); return; }
                if (dk == I64 && sk != I64) {
                    bool sg = I.getOpcode() == Instruction::SExt;
                    forLanes(lanesOf(I.getType()), [&](const string& i) {
                        string v = rd(I.getOperand(0), i);
                        string lo = sg ? "asuint(" + signedOf(sk, v) + ")" : v;
                        string hi = sg ? "(" + signedOf(sk, v) + "<0 ? 0xffffffffu : 0u)" : "0u";
                        return d + "[(" + i + ")*2] = " + lo + "; " + d + "[(" + i + ")*2+1] = " + hi + ";";
                    });
                    return;
                }
                die("i64 cast");
            }
            bool sx = I.getOpcode() == Instruction::SExt;
            forLanes(lanesOf(I.getType()), [&](const string& i) {
                string v = rd(I.getOperand(0), i);
                return wr(d, dk, i, sx ? "asuint(" + signedOf(sk, v) + ")" : v);
            });
            return;
        }
        case Instruction::FPToUI: case Instruction::FPToSI: {
            EK dk = ekOf(I.getType()); string d = nm(&I);
            bool sg = I.getOpcode() == Instruction::FPToSI;
            forLanes(lanesOf(I.getType()), [&](const string& i) { string v = rd(I.getOperand(0), i); return wr(d, dk, i, sg ? "asuint(int(" + v + "))" : "uint(" + v + ")"); });
            return;
        }
        case Instruction::UIToFP: case Instruction::SIToFP: {
            EK sk = ekOf(I.getOperand(0)->getType()), dk = ekOf(I.getType()); string d = nm(&I);
            bool sg = I.getOpcode() == Instruction::SIToFP;
            forLanes(lanesOf(I.getType()), [&](const string& i) { string v = rd(I.getOperand(0), i); return wr(d, dk, i, sg ? "float(" + signedOf(sk, v) + ")" : "float(" + v + ")"); });
            return;
        }
        case Instruction::FPExt: case Instruction::FPTrunc: {
            EK dk = ekOf(I.getType()); string d = nm(&I);
            forLanes(lanesOf(I.getType()), [&](const string& i) { return wr(d, dk, i, rd(I.getOperand(0), i)); });
            return;
        }
        case Instruction::BitCast: {
            Type *st = I.getOperand(0)->getType(), *dt = I.getType();
            if (ekOf(st) == B1 || ekOf(dt) == B1) die("i1 bitcast");
            copyWords(nm(&I), arr(I.getOperand(0)), wordsOf(dt));
            return;
        }
        case Instruction::ExtractElement: {
            EK k = ekOf(I.getType()); string d = nm(&I);
            if (k == I64) { string s = arr(I.getOperand(0)); string ix = u32(I.getOperand(1)); emit(d + "[0] = " + s + "[(" + ix + ")*2]; " + d + "[1] = " + s + "[(" + ix + ")*2+1];"); return; }
            emit(wr(d, k, "0u", rd(I.getOperand(0), u32(I.getOperand(1)))));
            return;
        }
        case Instruction::InsertElement: {
            EK k = ekOf(I.getType()); string d = nm(&I);
            Type* t = I.getType();
            if (k == I64) die("insertelement i64");
            if (!isa<UndefValue>(I.getOperand(0))) copyWords(d, arr(I.getOperand(0)), wordsOf(t));
            else for (unsigned i = 0; i < wordsOf(t); ++i) if (k == I8 || k == I16 || k == F16) emit(d + "[" + to_string(i) + "] = 0u;");
            emit(wr(d, k, u32(I.getOperand(2)), rd(I.getOperand(1), "0u")));
            return;
        }
        case Instruction::ShuffleVector: {
            auto& S = cast<ShuffleVectorInst>(I);
            EK k = ekOf(I.getType()); string d = nm(&I);
            if (k == I64) die("shufflevector i64");
            unsigned n1 = lanesOf(S.getOperand(0)->getType());
            // splat fast path: all mask entries equal
            auto mask = S.getShuffleMask();
            for (unsigned i = 0; i < mask.size(); ++i)
            {
                int m = mask[i];
                string e = m < 0 ? (isF(k) ? "0.0" : "0u") : (unsigned)m < n1 ? rd(S.getOperand(0), to_string(m) + "u") : rd(S.getOperand(1), to_string(m - n1) + "u");
                if (bitsOf(k) < 32 && k != B1 && i == 0) for (unsigned w = 0; w < wordsOf(I.getType()); ++w) emit(d + "[" + to_string(w) + "] = 0u;");
                emit(wr(d, k, to_string(i) + "u", e));
            }
            return;
        }
        case Instruction::Call: call(cast<CallInst>(I)); return;
        case Instruction::Freeze: copyWords(nm(&I), arr(I.getOperand(0)), wordsOf(I.getType())); return;
        default: die(string("unsupported instruction ") + I.getOpcodeName());
        }
    }

    void run(Function* F_, const string& rootsig, const string& outPath)
    {
        F = F_;
        // ---- resources from the root signature: kernel argument j (samplers excluded) <-> root parameter j+1 ----
        std::vector<Res> params;
        {
            std::ifstream in(rootsig);
            string line;
            while (std::getline(in, line))
            {
                std::istringstream ls(line);
                string w, id, kind;
                ls >> w;
                if (w != "param") continue;
                unsigned pi; ls >> pi >> kind;
                Res r;
                if (kind == "table")
                {
                    string range; ls >> range;
                    char t[16]; unsigned reg, space, num; long long offs;
                    if (sscanf(range.c_str(), "%15[A-Z]:%u:%u:%u:%lld", t, &reg, &space, &num, &offs) != 5) die("bad range: " + range);
                    r.kind = t; r.reg = reg;
                }
                else r.kind = "CONST";
                if (params.size() <= pi) params.resize(pi + 1);
                params[pi] = r;
            }
        }
        int ri = 0;
        for (auto& A : F->args())
        {
            string tn; { raw_string_ostream o(tn); A.getType()->print(o); }
            if (tn.find("spirv.Sampler") != string::npos) continue;
            if ((size_t)(ri + 1) >= params.size()) die("more kernel resources than root parameters");
            Res r = params[ri + 1];
            r.image = tn.find("spirv.Image") != string::npos;
            res.push_back(r);
            argRes[&A] = ri++;
        }
        if ((size_t)ri + 1 != params.size()) fprintf(stderr, "x4rtrans: warning: %d resources vs %zu root parameters\n", ri, params.size());

        for (auto& BB : *F) { int id = bbIds.size(); bbIds[&BB] = id; }
        if (trace)
            for (auto& BB : *F)
                for (auto& I : BB)
                {
                    if (I.getType()->isVoidTy() || isa<PHINode>(I) || isa<BranchInst>(I)) continue;
                    tOff[&I] = tTotal; tTotal += wordsOf(I.getType());
                }
        std::ostringstream cases;
        for (auto& BB : *F)
        {
            body.str(""); body.clear();
            ind = "        ";
            for (auto& I : BB)
            {
                if (isa<PHINode>(I)) { nm(&I); continue; }
                if (auto* br = dyn_cast<BranchInst>(&I))
                {
                    if (br->isUnconditional())
                    {
                        phiCopies(&BB, br->getSuccessor(0));
                        emit("bb = " + to_string(bbIds[br->getSuccessor(0)]) + "u;");
                    }
                    else
                    {
                        emit("if (" + rd(br->getCondition(), "0u") + ") {");
                        phiCopies(&BB, br->getSuccessor(0));
                        emit("  bb = " + to_string(bbIds[br->getSuccessor(0)]) + "u;");
                        emit("} else {");
                        phiCopies(&BB, br->getSuccessor(1));
                        emit("  bb = " + to_string(bbIds[br->getSuccessor(1)]) + "u;");
                        emit("}");
                    }
                    continue;
                }
                if (isa<ReturnInst>(I)) { emit("return;"); continue; }
                if (isa<UnreachableInst>(I)) { emit("return;"); continue; }
                instr(I);
                if (trace && !I.getType()->isVoidTy() && tOff.count(&I))
                {
                    unsigned w = wordsOf(I.getType()); string v = nm(&I);
                    emit("[loop] for (uint k = 0; k < " + to_string(w) + "; k++) DBG.Store(((gtid*" + to_string(tTotal) + "u) + " + to_string(tOff[&I]) + "u + k)*4u, " + v + "[k]);");
                }
            }
            cases << "      case " << bbIds[&BB] << ": {\n" << body.str() << "        break;\n      }\n";
        }

        std::ostringstream out;
        out << "// generated by x4rtrans from CM kernel " << F->getName().str() << "\n";
        out << "#define X4R_SET8(a,i,e) { uint s_ = ((i)&3u)<<3; (a)[(i)>>2] = ((a)[(i)>>2] & ~(0xffu<<s_)) | (((e)&0xffu)<<s_); }\n";
        out << "#define X4R_SET16(a,i,e) { uint s_ = ((i)&1u)<<4; (a)[(i)>>1] = ((a)[(i)>>1] & ~(0xffffu<<s_)) | (((e)&0xffffu)<<s_); }\n";
        out << "int x4r_sx(uint x, uint bits) { return int(x << (32u - bits)) >> (32u - bits); }\n";
        out << "float x4r_rcp(float x) { return 1.0 / x; }\n";
        for (size_t s = 0; s < res.size(); ++s)
        {
            const Res& r = res[s];
            string si = to_string(s), rg = to_string(r.reg);
            if (r.kind == "SRV") out << (r.image ? "Texture2D<float4> R" : "ByteAddressBuffer R") << si << " : register(t" << rg << ");\n";
            else if (r.kind == "UAV") out << (r.image ? "RWTexture2D<float4> R" : "RWByteAddressBuffer R") << si << " : register(u" << rg << ");\n";
            else if (r.kind == "CBV") out << "cbuffer CB" << si << " : register(b" << rg << ") { uint4 CBD" << si << "[4096]; };\n";
            else die("unsupported resource kind for arg " + si);
        }
        if (usesSampler) out << "SamplerState smp0 : register(s0);\n";
        if (trace) out << "RWByteAddressBuffer DBG : register(u31);\n";
        if (slmBytes) out << "groupshared uint slm[" << slmBytes / 4 << "];\n";
        out << globals.str();
        out << "[numthreads(" << LX << "," << LY << "," << LZ << ")]\n";
        out << "void main(uint3 gid : SV_GroupID, uint3 tid : SV_GroupThreadID, uint gtid : SV_GroupIndex) {\n";
        out << "  uint3 lsz = uint3(" << LX << "u," << LY << "u," << LZ << "u);\n  uint ngx = 1u, ngy = 1u;\n  uint bb = 0u;\n";
        out << decls.str();
        out << "  [loop] while (true) {\n    switch (bb) {\n" << cases.str() << "      default: return;\n    }\n  }\n}\n";
        std::ofstream f(outPath);
        f << out.str();
    }
};

int main(int argc, char** argv)
{
    if (argc < 7) { fprintf(stderr, "usage: x4rtrans kernel.ll rootsig.txt LX LY LZ out.hlsl [slm_bytes]\n"); return 1; }
    LLVMContext ctx;
    SMDiagnostic err;
    auto M = parseIRFile(argv[1], err, ctx);
    if (!M) { err.print("x4rtrans", errs()); return 1; }
    Function* K = nullptr;
    for (auto& F : *M) if (F.getCallingConv() == CallingConv::SPIR_KERNEL) K = &F;
    if (!K) die("no kernel in module");
    Tx t;
    t.LX = atoi(argv[3]); t.LY = atoi(argv[4]); t.LZ = atoi(argv[5]);
    if (K->hasFnAttribute("VCSLMSize")) t.slmBytes = atoi(K->getFnAttribute("VCSLMSize").getValueAsString().str().c_str());
    t.trace = argc > 7 && string(argv[7]) == "trace";
    t.run(K, argv[2], argv[6]);
    return 0;
}
