// SPDX-License-Identifier: GPL-3.0-only
// x4rwave: translate an Intel CM (C-for-Metal) kernel, lifted to LLVM IR with llvm.genx.* intrinsics, to HLSL.
//
// Execution model: this mirrors how the kernel runs on Intel hardware. One CM *thread* is 16 HLSL invocations (the 16
// lanes of an Xe SIMD16 register). A CM vector is a sequence of 32-bit "dwords"; dword w lives in lane (w % 16),
// slot (w / 16) (slot = one GRF = one VGPR per lane here). Elements narrower than 32 bits are packed in the dword
// exactly like in memory, so bitcasts are renames and elementwise math is lane-local. Data movement between
// elements (regions, shuffles, inserts) is a static gather made of lane shuffles. CM scalars are uniform across the
// 16 lanes of a thread. SV_GroupIndex = 16 * cm_thread + lane; SLM = groupshared; barrier = group barrier.
//
// usage: x4rwave kernel.ll rootsig.txt LX LY LZ out.hlsl
#include "llvm/IR/Constants.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/Analysis/LoopInfo.h"
#include "llvm/Analysis/PostDominators.h"
#include "llvm/IR/Dominators.h"
#include "llvm/IR/Function.h"
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
#include <memory>
#include <set>
#include <sstream>
#include <string>
#include <vector>

using namespace llvm;
using std::string;
using std::to_string;

[[noreturn]] static void die(const string& m)
{
    fprintf(stderr, "x4rwave: %s\n", m.c_str());
    exit(2);
}
static string str(Value* v) { string s; raw_string_ostream o(s); v->print(o); return s; }

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
static bool isF(EK k) { return k == F16 || k == F32; }
static unsigned ebits(EK k) { switch (k) { case B1: return 1; case I8: return 8; case I16: case F16: return 16; case I32: case F32: return 32; case I64: return 64; } return 0; }

// layout of a value: U = uniform scalar, D = distributed vector
struct Lay
{
    bool U; EK k; unsigned N, epd, bw, W, S;
};
static Lay layOf(Type* t)
{
    Lay l;
    l.U = !isa<FixedVectorType>(t);
    l.k = ekOf(t);
    l.N = l.U ? 1 : cast<FixedVectorType>(t)->getNumElements();
    switch (l.k) {
    case B1: l.epd = 1; l.bw = 32; l.W = l.N; break;
    case I8: l.epd = 4; l.bw = 8; l.W = (l.N + 3) / 4; break;
    case I16: case F16: l.epd = 2; l.bw = 16; l.W = (l.N + 1) / 2; break;
    case I32: case F32: l.epd = 1; l.bw = 32; l.W = l.N; break;
    case I64: l.epd = 1; l.bw = 32; l.W = 2 * l.N; break;
    }
    l.S = (l.W + 15) / 16;
    if (l.S == 0) l.S = 1;
    return l;
}
static string hex32(uint32_t v) { char b[32]; snprintf(b, sizeof b, "0x%xu", v); return b; }
static uint32_t maskOf(unsigned bits) { return bits >= 32 ? 0xffffffffu : ((1u << bits) - 1); }

struct Res { string kind; unsigned reg = 0; bool image = false; };

// where one piece (a field of a destination dword) comes from
struct Pc
{
    enum K { Zero, Keep, Lit, SrcU, SrcD } k = Zero;
    Value* v = nullptr;
    string base;   // raw variable base name (temporaries); overrides v
    unsigned dw = 0, sh = 0, bits = 32;
    uint32_t lit = 0;
};

struct Tx
{
    Function* F = nullptr;
    unsigned LX, LY, LZ;
    std::map<Value*, int> ids;
    std::map<BasicBlock*, int> bbIds;
    std::map<Value*, int> surfaceArg;
    std::vector<Res> res;
    std::map<Argument*, int> argRes;
    std::ostringstream globals, body, decls;
    int nconst = 0, ntmp = 0;
    unsigned slmBytes = 0;
    bool usesSampler = false;
    bool trace = false;
    std::set<Value*> eform;   // narrow vectors kept one element per lane (no packing) because no consumer needs the packed form
    std::map<string,unsigned> slotCounts;
    struct ExpTile { Value* source; uint32_t scale; string temp; bool emitted = false; };
    std::vector<ExpTile> expTiles;
    std::map<Value*,std::pair<unsigned,unsigned>> expSlices;
    bool isE(Value* v) { return eform.count(v) != 0; }
    // Keeping region/insert chains unpacked helps input_processing. Convolution
    // register pressure is sensitive to this, so leave other kernels unchanged.
    bool moveEForms() {
        if (const char* option = getenv("X4R_MOVE_EFORM")) return atoi(option) != 0;
        return F && F->getName() == "input_processing";
    }
    unsigned trLo = 0, trHi = ~0u;   // trace window: ordinals of traced instructions in [trLo, trHi)
    std::map<Instruction*, unsigned> tOff;
    unsigned tTotal = 0;
    string ind = "        ";

    std::string curCat; std::map<std::string, double> costBy; std::map<std::string, int> cntBy;
    static double lineCost(const string& s)
    {
        double c = 0; size_t n = s.size();
        for (size_t i = 0; i < n; ++i)
        {
            char ch = s[i];
            if (ch == '+' || ch == '*' || ch == '|' || ch == '^' || ch == '~' || ch == '?') c += 1;
            else if (ch == '-' && i + 1 < n && s[i + 1] != '>') c += 1;
            else if (ch == '&' && !(i + 1 < n && s[i + 1] == '&')) c += 1;
            else if ((ch == '<' || ch == '>') && i + 1 < n && s[i + 1] == ch) { c += 1; ++i; }
        }
        for (const char* f : {"x4r_sh(", "x4r_rl(", "WaveReadLaneAt", "dot4add", "f16tof32", "f32tof16", "x4r_f2h", "asfloat", "asuint", "clamp(", "sqrt(", "exp2(", "log2(", "pow("})
            for (size_t p = s.find(f); p != string::npos; p = s.find(f, p + 1)) c += (f[0] == 'x' || f[0] == 'd' || f[0] == 'W') ? 2 : 0.3;
        if (s.find("Load(") != string::npos || s.find("Store(") != string::npos) c += 2;
        return c;
    }
    void emit(const string& s) { body << ind << s << "\n"; if (!curCat.empty()) { costBy[curCat] += lineCost(s); } }
    string newTmp(unsigned slots)
    {
        string t = "t" + to_string(ntmp++);
        slotCounts[t] = slots;
        for (unsigned s = 0; s < slots; ++s) decls << "  uint " << t << "_" << s << ";\n";
        return t;
    }

    // ---------------- naming ----------------
    string nm(Value* v)
    {
        auto it = ids.find(v);
        if (it == ids.end())
        {
            int id = ids.size(); ids[v] = id;
            if (isa<Instruction>(v) && !v->getType()->isVoidTy())
            {
                Lay l = layOf(v->getType());
                if (l.U) decls << (l.k == I64 ? "  uint64_t v" : "  uint v") << id << ";\n";
                else { unsigned nS = eform.count(v) ? std::max(1u, (l.N + 15) / 16) : l.S; slotCounts["v" + to_string(id)] = nS; for (unsigned s = 0; s < nS; ++s) decls << "  uint v" << id << "_" << s << ";\n"; }
            }
            it = ids.find(v);
        }
        return "v" + to_string(it->second);
    }
    string slot(const string& base, unsigned s) { return base + "_" + to_string(s); }

    // ---------------- constants ----------------
    uint64_t constBits(Constant* c, unsigned i)
    {
        Constant* e = isa<FixedVectorType>(c->getType()) ? c->getAggregateElement(i) : c;
        if (!e || isa<UndefValue>(e)) return 0;
        if (auto* ci = dyn_cast<ConstantInt>(e)) return ci->getZExtValue();
        if (auto* cf = dyn_cast<ConstantFP>(e)) return cf->getValueAPF().bitcastToAPInt().getZExtValue();
        if (isa<ConstantPointerNull>(e)) return 0;
        die("unsupported constant element: " + str(e));
    }
    bool isSplat(Constant* c)
    {
        if (!isa<FixedVectorType>(c->getType())) return true;
        unsigned n = cast<FixedVectorType>(c->getType())->getNumElements();
        uint64_t b = constBits(c, 0);
        for (unsigned i = 1; i < n; ++i) if (constBits(c, i) != b) return false;
        return true;
    }
    int64_t cint(Value* v)
    {
        auto* c = dyn_cast<ConstantInt>(v);
        if (!c) die("expected constant integer: " + str(v));
        return c->getSExtValue();
    }

    // ---------------- typed element access on raw bits ----------------
    string toF(EK k, const string& b) { return k == F16 ? "f16tof32(" + b + ")" : "asfloat(" + b + ")"; }
    string fromF(EK k, const string& f) { return k == F16 ? "x4r_f2h(" + f + ")" : "asuint(" + f + ")"; }
    string sx(EK k, const string& b)
    {
        if (k == I32 || k == B1) return "asint(" + b + ")";
        if (k == I64) return "int64_t(" + b + ")";
        return "x4r_sx(" + b + "," + to_string(ebits(k)) + "u)";
    }

    // bits (uint expression, zero extended to the element width) of element (slot s, sub q) of operand v; the operand
    // is read with `epd` elements per dword (must equal its own epd when it is distributed)
    string opBits(Value* v, unsigned s, unsigned q)
    {
        Lay l = layOf(v->getType());
        if (auto* c = dyn_cast<Constant>(v))
        {
            if (isSplat(c)) return litStr(l.k, constBits(c, 0));
            string arr = "c" + to_string(nconst++);
            globals << "static const uint " << arr << "[16] = {";
            for (unsigned ln = 0; ln < 16; ++ln)
            {
                unsigned e = (16 * s + ln) * l.epd + q;
                globals << (ln ? "," : "") << hex32(e < l.N ? (uint32_t)constBits(c, e) : 0);
            }
            globals << "};\n";
            return arr + "[lane]";
        }
        if (l.U) return nm(v);
        string r = slot(nm(v), s);
        if (isE(v)) return r;
        if (l.epd == 1) return r;
        return "((" + r + ">>" + to_string(q * l.bw) + "u)&" + hex32(maskOf(l.bw)) + ")";
    }
    string litStr(EK k, uint64_t b)
    {
        if (k == I64) return "(uint64_t(" + hex32((uint32_t)b) + ")|(uint64_t(" + hex32((uint32_t)(b >> 32)) + ")<<32))";
        return hex32((uint32_t)b);
    }

    // ---------------- data movement: pieces and gather ----------------
    // location of element e (of at most 32 bits) of the vector value v: dword, shift, bits
    int fwdOverride = -1;
    int fwdMode() { static int m = getenv("X4R_FWD") ? atoi(getenv("X4R_FWD")) : 0; return fwdOverride >= 0 ? fwdOverride : m; }
    bool bitPiece(Value* v, unsigned offset, unsigned bits, Pc& out, int depth)
    {
        if (depth > 32 || bits > 32) return false;
        if (auto* bc = dyn_cast<BitCastInst>(v)) return bitPiece(bc->getOperand(0),offset,bits,out,depth+1);
        Lay l = layOf(v->getType());
        unsigned width = ebits(l.k);
        if (l.k == B1 || l.k == I64 || offset+bits > l.N*width) return false;
        if (bits <= width && offset%width+bits <= width) {
            out = elemPiece(v,offset/width,depth+1);
            unsigned shift = offset%width;
            if (out.k == Pc::Lit) { out.lit = (out.lit >> shift) & maskOf(bits); return true; }
            if (out.k != Pc::SrcD && out.k != Pc::SrcU && out.k != Pc::Zero) return false;
            out.sh += shift; out.bits = bits;
            return true;
        }
        if (isE(v) || l.U || offset%32+bits > 32) return false;
        // Whole dwords can sometimes be recovered through narrow region views.
        if (bits == 32 && offset%32 == 0 && width < 32) {
            Pc first; bool same = true, literals = true; uint32_t literal = 0;
            for (unsigned q = 0; q < 32/width; ++q) {
                Pc p = elemPiece(v,offset/width+q,depth+1);
                if (!q) first = p;
                if (p.k != Pc::Lit) literals = false;
                else literal |= (p.lit & maskOf(width)) << (q*width);
                if (p.k != first.k || p.v != first.v || p.base != first.base || p.dw != first.dw || p.sh != q*width) same = false;
            }
            if (literals) { out = Pc(); out.k = Pc::Lit; out.lit = literal; return true; }
            if (same && first.k == Pc::SrcD) { out = first; out.bits = 32; return true; }
        }
        out = Pc(); out.k = Pc::SrcD; out.v = v; out.dw = offset/32; out.sh = offset%32; out.bits = bits;
        return true;
    }
    Pc elemPiece(Value* v, unsigned e, int depth = 0)
    {
        Pc p;
        auto exponential = expSlices.find(v);
        if (exponential != expSlices.end()) {
            p.k = Pc::SrcD; p.base = expTiles[exponential->second.first].temp;
            p.dw = exponential->second.second + e; p.bits = 32;
            return p;
        }
        if (auto* c = dyn_cast<Constant>(v))
        {
            p.k = Pc::Lit; p.lit = (uint32_t)constBits(c, e) & maskOf(std::min(32u, ebits(ekOf(v->getType()))));
            return p;
        }
        if (getenv("X4R_BITCAST_FWD")) if (auto* bc = dyn_cast<BitCastInst>(v)) {
            Lay l = layOf(v->getType());
            if (e >= l.N) return p;
            if (l.k != B1 && l.k != I64 && bitPiece(bc->getOperand(0),e*ebits(l.k),ebits(l.k),p,depth+1)) return p;
        }
        // forward through insertelement / shufflevector with constant indices
        if (auto* ie = dyn_cast<InsertElementInst>(v))
            if (auto* ci = dyn_cast<ConstantInt>(ie->getOperand(2)))
            {
                if (ci->getZExtValue() == e) return elemPiece(ie->getOperand(1), 0, depth + 1);
                return elemPiece(ie->getOperand(0), e, depth + 1);
            }
        if (auto* sv = dyn_cast<ShuffleVectorInst>(v))
        {
            int m = sv->getMaskValue(e);
            if (m < 0) return p;
            unsigned n1 = cast<FixedVectorType>(sv->getOperand(0)->getType())->getNumElements();
            return (unsigned)m < n1 ? elemPiece(sv->getOperand(0), m, depth + 1) : elemPiece(sv->getOperand(1), m - n1, depth + 1);
        }
        // see through constant-region reads/writes so an element comes straight from its original source (one move instead of a chain of shuffles)
        int forwardDepth = getenv("X4R_FWD_DEPTH") ? atoi(getenv("X4R_FWD_DEPTH")) : 24;
        if (depth < std::min(256,forwardDepth) && !getenv("X4R_NO_FORWARD"))
            if (auto* C = dyn_cast<CallInst>(v))
                if (Function* f = C->getCalledFunction())
                {
                    string fn = f->getName().str();
                    Type* rty = C->getType();
                    if (isa<FixedVectorType>(rty))
                    {
                        EK rk = ekOf(rty); unsigned esz = std::max(1u, ebits(rk) / 8);
                        if (fn.rfind("llvm.genx.rdregion", 0) == 0 && rk != I64 && fwdMode() >= 1)
                        {
                            auto *vs = dyn_cast<ConstantInt>(C->getArgOperand(1)), *w = dyn_cast<ConstantInt>(C->getArgOperand(2)), *st = dyn_cast<ConstantInt>(C->getArgOperand(3)), *co = dyn_cast<ConstantInt>(C->getArgOperand(4));
                            Value* src = C->getArgOperand(0);
                            if (vs && w && st && co && isa<FixedVectorType>(src->getType()))
                            {
                                long ix = (long)(co->getZExtValue() / esz) + (long)(e / w->getSExtValue()) * vs->getSExtValue() + (long)(e % w->getSExtValue()) * st->getSExtValue();
                                if (ix < 0 || (unsigned long)ix >= layOf(src->getType()).N) return Pc();
                                return elemPiece(src, (unsigned)ix, depth + 1);
                            }
                        }
                        if (fn.rfind("llvm.genx.wrregion", 0) == 0 && rk != I64 && fwdMode() >= 2)
                        {
                            auto *vs = dyn_cast<ConstantInt>(C->getArgOperand(2)), *w = dyn_cast<ConstantInt>(C->getArgOperand(3)), *st = dyn_cast<ConstantInt>(C->getArgOperand(4)), *co = dyn_cast<ConstantInt>(C->getArgOperand(5)), *mk = dyn_cast<ConstantInt>(C->getArgOperand(7));
                            Value *old = C->getArgOperand(0), *nw = C->getArgOperand(1);
                            if (vs && w && st && co && mk && mk->isOne() && isa<FixedVectorType>(nw->getType()) && isa<FixedVectorType>(old->getType()))
                            {
                                long off = (long)(co->getZExtValue() / esz); unsigned nn = layOf(nw->getType()).N;
                                long rel = (long)e - off, m = -1;
                                if (rel >= 0)
                                {
                                    if (vs->getSExtValue() == 0 && w->getSExtValue() == (long)nn && st->getSExtValue() == 1) { if (rel < (long)nn) m = rel; }
                                    else for (unsigned mm = 0; mm < nn; ++mm) if ((long)(mm / w->getSExtValue()) * vs->getSExtValue() + (long)(mm % w->getSExtValue()) * st->getSExtValue() == rel) { m = mm; break; }
                                }
                                if (m >= 0) return elemPiece(nw, (unsigned)m, depth + 1);
                                if (fwdMode() >= 3) return elemPiece(old, e, depth + 1);
                                // else fall through: the element stays in this wrregion's own result (Keep semantics)
                            }
                        }
                    }
                }
        Lay l = layOf(v->getType());
        if (l.k == I64) die("64-bit element piece");
        p.v = v; p.bits = ebits(l.k) == 1 ? 32 : ebits(l.k);
        if (l.U) { if (e != 0) die("element of scalar"); p.k = Pc::SrcU; return p; }
        if (e >= l.N) return Pc();
        if (isE(v)) { p.k = Pc::SrcD; p.dw = e; p.sh = 0; return p; }
        p.k = Pc::SrcD; p.dw = e / l.epd; p.sh = (e % l.epd) * l.bw;
        return p;
    }
    Pc dwordPiece(Value* v, unsigned dw)   // a whole dword of v (for 64-bit element handling / bitcast style copies)
    {
        Pc p; p.k = Pc::SrcD; p.v = v; p.dw = dw; p.sh = 0; p.bits = 32; return p;
    }

    string srcName(const Pc& p) { return p.base.empty() ? nm(p.v) : p.base; }
    string laneTbl16(const std::vector<unsigned>& v)   // 16 entries of 4 bits -> expression
    {
        uint32_t lo = 0, hi = 0;
        for (int i = 0; i < 16; ++i) { if (i < 8) lo |= (v[i] & 15u) << (4 * i); else hi |= (v[i] & 15u) << (4 * (i - 8)); }
        return "x4r_tb(" + hex32(lo) + "," + hex32(hi) + ",lane)";
    }
    // Describe a lane map with constant/selected/inverted input bits. Only
    // member lanes matter: the gather masks all other lanes after the shuffle.
    string laneBitExpr(const std::vector<unsigned>& values, unsigned bits, uint32_t members)
    {
        uint32_t fixed = 0, flipped = 0;
        std::map<int,uint32_t> masks;
        for (unsigned b = 0; b < bits; ++b) {
            bool zero = true, one = true;
            for (unsigned l = 0; l < 16; ++l) if ((members >> l) & 1u) {
                if ((values[l] >> b) & 1u) zero = false; else one = false;
            }
            if (zero) continue;
            if (one) { fixed |= 1u << b; continue; }
            bool found = false;
            for (unsigned j = 0; j < 4 && !found; ++j) for (unsigned inv = 0; inv < 2 && !found; ++inv) {
                bool match = true;
                for (unsigned l = 0; l < 16; ++l) if ((members >> l) & 1u)
                    if (((values[l] >> b) & 1u) != (((l >> j) & 1u) ^ inv)) match = false;
                if (match) { masks[(int)b-(int)j] |= 1u << j; if (inv) flipped |= 1u << b; found = true; }
            }
            if (!found) return "";
        }
        string expr;
        for (auto& kv : masks) {
            string term = "(WaveGetLaneIndex() & " + hex32(kv.second) + ")";
            if (kv.first > 0) term = "(" + term + " << " + to_string(kv.first) + "u)";
            if (kv.first < 0) term = "(" + term + " >> " + to_string(-kv.first) + "u)";
            expr += (expr.empty() ? "" : " | ") + term;
        }
        if (expr.empty()) expr = "0u";
        if (flipped) expr = "((" + expr + ") ^ " + hex32(flipped) + ")";
        if (fixed) expr = "((" + expr + ") | " + hex32(fixed) + ")";
        return expr;
    }

    bool routeRegisters(const string& dst, unsigned W, unsigned epd, const string& keep,
                        const std::function<Pc(unsigned,unsigned)>& pieceAt)
    {
        if (!getenv("X4R_REG_ROUTE") || epd != 1 || !keep.empty() || W < 16 || (W & (W-1))) return false;
        std::vector<unsigned> words;
        string source;
        for (unsigned d = 0; d < W; ++d) {
            Pc p = pieceAt(d,0);
            if (p.k != Pc::SrcD || p.sh || p.bits != 32) return false;
            string name = srcName(p);
            if (source.empty()) source = name;
            if (source != name) return false;
            words.push_back(p.dw);
        }
        unsigned slots = slotCounts[source];
        if (!slots || slots > 8 || (slots & (slots-1)) || W > slots*16) return false;
        unsigned nbits = 0, dbits = 0;
        while ((1u << nbits) < slots*16) ++nbits;
        while ((1u << dbits) < W) ++dbits;
        std::vector<int> target(nbits,-1), constantBits;
        unsigned flips = 0;
        for (unsigned b = 0; b < nbits; ++b) {
            bool zero = true, one = true;
            for (unsigned w : words) { if ((w >> b)&1) zero = false; else one = false; }
            if (zero || one) { constantBits.push_back(b); if (one) flips |= 1u << b; continue; }
            int match = -1;
            for (unsigned j = 0; j < dbits && match < 0; ++j) for (unsigned inv = 0; inv < 2 && match < 0; ++inv) {
                bool equal = true;
                for (unsigned d = 0; d < W; ++d) if (((words[d]>>b)&1) != (((d>>j)&1)^inv)) equal = false;
                if (equal) { match = j; if (inv) flips |= 1u << b; }
            }
            if (match < 0 || target[match] >= 0) return false;
            target[match] = b;
        }
        for (unsigned j = 0; j < dbits; ++j) if (target[j] < 0) return false;
        if (constantBits.size() != nbits-dbits) return false;
        for (unsigned j = dbits; j < nbits; ++j) target[j] = constantBits[j-dbits];
        for (unsigned d = 0; d < W; ++d) {
            unsigned index = flips;
            for (unsigned j = 0; j < nbits; ++j) if ((d >> j)&1) index ^= 1u << target[j];
            if (index != words[d]) return false;
        }
        std::vector<string> current;
        for (unsigned s = 0; s < slots; ++s) current.push_back(slot(source,s));
        std::vector<int> axes;
        for (unsigned j = 0; j < nbits; ++j) axes.push_back(j);
        unsigned swaps = 0;
        for (unsigned a = 0; a < nbits; ++a) {
            if (axes[a] == target[a]) continue;
            unsigned b = a+1;
            while (axes[b] != target[a]) ++b;
            std::vector<string> next(slots);
            if (a >= 4 && b >= 4) {
                for (unsigned s = 0; s < slots; ++s) {
                    unsigned other = s;
                    if (((s>>(a-4))&1) != ((s>>(b-4))&1)) other ^= (1u<<(a-4)) | (1u<<(b-4));
                    next[s] = current[other];
                }
            } else {
                string tmp = newTmp(slots);
                for (unsigned s = 0; s < slots; ++s) {
                    string shuffled = newTmp(1), predicate;
                    unsigned other = s, mask;
                    if (b < 4) {
                        mask = (1u<<a)|(1u<<b);
                        predicate = "(((WaveGetLaneIndex() >> " + to_string(a) + "u) ^ (WaveGetLaneIndex() >> " + to_string(b) + "u)) & 1u) == 0u";
                    } else {
                        mask = 1u << a;
                        other ^= 1u << (b-4);
                        predicate = "((WaveGetLaneIndex() >> " + to_string(a) + "u) & 1u) == " + to_string((s>>(b-4))&1) + "u";
                    }
                    emit(slot(shuffled,0) + " = WaveReadLaneAt(" + current[other] + ", WaveGetLaneIndex() ^ " + to_string(mask) + "u);");
                    emit(slot(tmp,s) + " = (" + predicate + ") ? " + current[s] + " : " + slot(shuffled,0) + ";");
                    next[s] = slot(tmp,s);
                }
            }
            current = next;
            std::swap(axes[a],axes[b]); ++swaps;
        }
        unsigned outputFlip = 0;
        for (unsigned j = 0; j < nbits; ++j) if ((flips >> target[j])&1) outputFlip |= 1u << j;
        for (unsigned s = 0; s < W/16; ++s) {
            string value = current[s^(outputFlip>>4)];
            if (outputFlip&15) value = "WaveReadLaneAt(" + value + ", WaveGetLaneIndex() ^ " + to_string(outputFlip&15) + "u)";
            emit(slot(dst,s) + " = " + value + ";");
        }
        if (getenv("X4R_COST") && (swaps || flips)) fprintf(stderr,"REG_ROUTE %u -> %u words, %u swaps\n",slots*16,W,swaps);
        return true;
    }
    // Build destination dwords. dst variable base `dst` has W dwords; pieceAt(d,q) describes field q (of epd) of dword d.
    void gather(const string& dst, unsigned W, unsigned epd, const string& keep, const std::function<Pc(unsigned, unsigned)>& pieceAt)
    {
        if (getenv("X4R_WORD_GATHER") && epd > 1 && keep.empty()) {
            // A packed word routed as one unit needs one lane exchange, not
            // one exchange plus masking/packing for each byte or halfword.
            std::vector<Pc> words;
            bool complete = true;
            unsigned width = 32/epd;
            for (unsigned dw = 0; dw < W && complete; ++dw) {
                Pc first = pieceAt(dw,0);
                bool literal = first.k == Pc::Zero || first.k == Pc::Lit;
                uint32_t value = first.k == Pc::Lit ? first.lit & maskOf(width) : 0;
                for (unsigned q = 0; q < epd; ++q) {
                    Pc p = pieceAt(dw,q);
                    if (literal) {
                        if (p.k != Pc::Zero && p.k != Pc::Lit) { complete = false; break; }
                        if (p.k == Pc::Lit) value |= (p.lit & maskOf(width)) << (q*width);
                    } else if (first.k != Pc::SrcD || p.k != Pc::SrcD || srcName(p) != srcName(first) || p.dw != first.dw || p.sh != q*width || p.bits != width) { complete = false; break; }
                }
                if (literal) { first = Pc(); first.k = Pc::Lit; first.lit = value; }
                else { first.sh = 0; first.bits = 32; }
                words.push_back(first);
            }
            if (complete) { gather(dst,W,1,"",[&](unsigned dw,unsigned) { return words[dw]; }); return; }
        }
        if (routeRegisters(dst,W,epd,keep,pieceAt)) return;
        unsigned bw = 32 / epd;
        unsigned S = std::max(1u, (W + 15) / 16);
        for (unsigned s = 0; s < S; ++s)
        {
            std::vector<std::vector<Pc>> P(16, std::vector<Pc>(epd));
            std::vector<bool> valid(16, false);
            for (unsigned l = 0; l < 16; ++l)
            {
                unsigned d = 16 * s + l;
                if (d >= W) continue;
                valid[l] = true;
                for (unsigned q = 0; q < epd; ++q) P[l][q] = pieceAt(d, q);
            }
            // --- fast paths ---
            bool allKeep = true, allZero = true, direct = true; string dv; unsigned dslot = 0; bool any = false;
            for (unsigned l = 0; l < 16 && (allKeep || allZero || direct); ++l)
            {
                if (!valid[l]) continue;
                any = true;
                for (unsigned q = 0; q < epd; ++q)
                {
                    const Pc& p = P[l][q];
                    if (p.k != Pc::Keep) allKeep = false;
                    if (p.k != Pc::Zero) allZero = false;
                    if (p.k == Pc::SrcD && p.bits == bw && p.sh == q * bw && p.dw % 16 == l && (epd == 1 || p.bits == bw) && !(epd == 1 && p.sh != 0))
                    {
                        if (dv.empty()) { dv = srcName(p); dslot = p.dw / 16; }
                        else if (dv != srcName(p) || dslot != p.dw / 16) direct = false;
                    }
                    else direct = false;
                }
            }
            if (!any) continue;
            string D = slot(dst, s);
            if (allKeep) { if (keep.empty()) emit(D + " = 0u;"); else if (keep != dst) emit(D + " = " + slot(keep, s) + ";"); continue; }
            if (allZero) { emit(D + " = 0u;"); continue; }
            if (direct && !dv.empty()) { emit(D + " = " + slot(dv, dslot) + ";"); continue; }

            // --- generic ---
            string total;
            for (unsigned q = 0; q < epd; ++q)
            {
                // group lanes by source
                struct Grp { string key; Pc rep; uint32_t mask = 0; std::vector<Pc> perLane; };
                std::vector<Grp> groups;
                for (unsigned l = 0; l < 16; ++l)
                {
                    if (!valid[l]) continue;
                    const Pc& p = P[l][q];
                    string key = p.k == Pc::Zero ? "Z" : p.k == Pc::Keep ? "K" : p.k == Pc::Lit ? "L" : p.k == Pc::SrcU ? "U" + srcName(p) : "D" + srcName(p) + "_" + to_string(p.dw / 16);
                    Grp* g = nullptr;
                    for (auto& x : groups) if (x.key == key) g = &x;
                    if (!g) { groups.push_back(Grp()); g = &groups.back(); g->key = key; g->rep = p; g->perLane.resize(16); }
                    g->mask |= 1u << l; g->perLane[l] = p;
                }
                string E;
                for (size_t gi = 0; gi < groups.size(); ++gi)
                {
                    Grp& g = groups[gi];
                    string x;
                    const Pc& r = g.rep;
                    unsigned fmask = bw;   // result field width
                    if (r.k == Pc::Zero) x = "0u";
                    else if (r.k == Pc::Keep) x = epd == 1 ? (keep.empty() ? "0u" : slot(keep, s)) : (keep.empty() ? "0u" : "((" + slot(keep, s) + ">>" + to_string(q * bw) + "u)&" + hex32(maskOf(bw)) + ")");
                    else if (r.k == Pc::Lit)
                    {
                        bool same = true; for (unsigned l = 0; l < 16; ++l) if ((g.mask >> l & 1) && g.perLane[l].lit != r.lit) same = false;
                        if (same) x = hex32(r.lit);
                        else
                        {
                            string arr = "c" + to_string(nconst++);
                            globals << "static const uint " << arr << "[16] = {";
                            for (unsigned l = 0; l < 16; ++l) globals << (l ? "," : "") << hex32((g.mask >> l & 1) ? g.perLane[l].lit : 0);
                            globals << "};\n";
                            x = arr + "[lane]";
                        }
                    }
                    else if (r.k == Pc::SrcU)
                    {
                        string u = srcName(r);
                        x = r.sh ? "((" + u + ">>" + to_string(r.sh) + "u)&" + hex32(maskOf(r.bits)) + ")" : (r.bits >= 32 ? u : "(" + u + "&" + hex32(maskOf(r.bits)) + ")");
                    }
                    else   // SrcD
                    {
                        string src = slot(srcName(r), r.dw / 16);
                        std::vector<unsigned> lanes(16, 0), shs(16, 0);
                        bool ident = true, constLane = true, constSh = true;
                        int firstLane = -1;
                        for (unsigned l = 0; l < 16; ++l)
                        {
                            if (!(g.mask >> l & 1)) { lanes[l] = l; continue; }
                            lanes[l] = g.perLane[l].dw % 16; shs[l] = g.perLane[l].sh;
                            if (lanes[l] != l) ident = false;
                            if (firstLane < 0) firstLane = lanes[l]; else if ((int)lanes[l] != firstLane) constLane = false;
                            if (shs[l] != r.sh) constSh = false;
                        }
                        // don't-care lanes (not in this group) may take any source: make the whole map a rotation / xor if the member lanes agree,
                        // so the driver can use a single DPP move instead of ds_bpermute
                        int rotK = -1, xorK = -1;
                        if (!ident && !constLane)
                        {
                            for (int k = 0; k < 16 && rotK < 0; ++k) { bool ok = true; for (unsigned l = 0; l < 16; ++l) if ((g.mask >> l & 1) && lanes[l] != (unsigned)((l + k) & 15)) ok = false; if (ok) rotK = k; }
                            for (int k = 1; k < 16 && xorK < 0; ++k) { bool ok = true; for (unsigned l = 0; l < 16; ++l) if ((g.mask >> l & 1) && lanes[l] != (l ^ k)) ok = false; if (ok) xorK = k; }
                            if (rotK >= 0) for (unsigned l = 0; l < 16; ++l) lanes[l] = (l + rotK) & 15;
                            else if (xorK >= 0) for (unsigned l = 0; l < 16; ++l) lanes[l] = l ^ xorK;
                        }
                        string v;
                        if (ident) v = src;
                        else if (constLane) v = "x4r_rl(" + src + "," + to_string(firstLane) + "u)";
                        else v = "x4r_sh(" + src + "," + laneTbl16(lanes) + ")";
                        if (getenv("X4R_SIMPLE_SHUFFLES") && !ident && !constLane) {
                            if (xorK >= 0) v = "WaveReadLaneAt(" + src + ", WaveGetLaneIndex() ^ " + to_string(xorK) + "u)";
                            else if (rotK >= 0) v = "WaveReadLaneAt(" + src + ", (WaveGetLaneIndex() & ~15u) | ((WaveGetLaneIndex() + " + to_string(rotK) + "u) & 15u))";
                            else {
                                string simple = laneBitExpr(lanes, 4, g.mask);
                                if (!simple.empty()) v = "WaveReadLaneAt(" + src + ", (WaveGetLaneIndex() & ~15u) | (" + simple + "))";
                            }
                        }
                        if (getenv("X4R_SHUF_STATS"))
                        {
                            // classify the lane map over the lanes of this group: rotate by k, xor by k, constant, other
                            string cls = "other";
                            for (int k = 1; k < 16 && cls == "other"; ++k) { bool ok = true; for (unsigned l = 0; l < 16; ++l) if ((g.mask >> l & 1) && lanes[l] != (l + k) % 16) ok = false; if (ok) cls = "rotate+" + to_string(k); }
                            for (int k = 1; k < 16 && cls == "other"; ++k) { bool ok = true; for (unsigned l = 0; l < 16; ++l) if ((g.mask >> l & 1) && lanes[l] != (l ^ k)) ok = false; if (ok) cls = "xor" + to_string(k); }
                            { bool ok = true; for (unsigned l = 0; l < 16; ++l) if ((g.mask >> l & 1) && lanes[l] != (l & ~7u)) ok = false; if (ok && cls == "other") cls = "bcast8"; }
                            { string tl; for (unsigned l = 0; l < 16; ++l) tl += ((g.mask >> l & 1) ? to_string(lanes[l]) : string("-")) + (l < 15 ? "," : ""); fprintf(stderr, "SHUF %s n=%u map=%s\n", cls.c_str(), (unsigned)__builtin_popcount(g.mask), tl.c_str()); }
                        }
                        if (!ident)
                        {
                            // shuffles must run with all lanes active (inactive source lanes read as undefined): hoist
                            string tmpv = "sh" + to_string(ntmp++);
                            decls << "  uint " << tmpv << ";\n";
                            emit(tmpv + " = " + v + ";");
                            v = tmpv;
                        }
                        string shExpr;
                        if (constSh) shExpr = r.sh ? to_string(r.sh) + "u" : "";
                        else
                        {
                            uint32_t t = 0; for (unsigned l = 0; l < 16; ++l) t |= (shs[l] / 8 & 3u) << (2 * l);
                            shExpr = "(((" + hex32(t) + ">>(lane*2u))&3u)*8u)";
                            if (getenv("X4R_SIMPLE_SHUFFLES")) {
                                std::vector<unsigned> bytes = shs;
                                for (unsigned& byte : bytes) byte /= 8u;
                                string simple = laneBitExpr(bytes, 2, g.mask);
                                if (!simple.empty()) shExpr = "((" + simple + ") * 8u)";
                            }
                        }
                        x = shExpr.empty() ? v : "(" + v + ">>" + shExpr + ")";
                        if (r.bits < 32 || !shExpr.empty()) x = "(" + x + "&" + hex32(maskOf(r.bits)) + ")";
                    }
                    (void)fmask;
                    if (gi == 0) E = x;
                    else E = "((((" + hex32(g.mask) + ">>lane)&1u)!=0u) ? " + x + " : " + E + ")";
                }
                if (E.empty()) E = "0u";
                string term = q == 0 ? E : "(" + E + "<<" + to_string(q * bw) + "u)";
                total += (q ? " | " : "") + term;
            }
            emit(D + " = " + total + ";");
        }
    }

    // gather the whole of vector value `v` into a new temporary in element-per-dword form (epd = 1)
    string unpackE(Value* v, unsigned& slots)
    {
        Lay l = layOf(v->getType());
        slots = std::max(1u, (l.N + 15) / 16);
        string t = newTmp(slots);
        gather(t, l.N, 1, "", [&](unsigned d, unsigned) { return elemPiece(v, d); });
        return t;
    }

    // ---------------- elementwise instructions ----------------
    // compute per element with function f over operand element-bit expressions; result written in result layout
    bool packEarly(const string& dst, const string& raw, const Lay& l)
    {
        if (!getenv("X4R_PACK_EARLY") || (l.epd != 2 && l.epd != 4) || l.N % l.epd) return false;
        // Pack inside each adjacent pair/quad before redistributing to CM
        // dword lanes. This replaces one cross-row shuffle per element with
        // one per packed dword; all cross-lane reads remain unconditional.
        unsigned sourceSlots = std::max(1u, (l.N + 15u) / 16u);
        string packed = newTmp(sourceSlots);
        for (unsigned s = 0; s < sourceSlots; ++s) {
            string expression;
            for (unsigned q = 0; q < l.epd; ++q) {
                string value = newTmp(1);
                string read = l.epd == 4
                    ? "QuadReadLaneAt(" + slot(raw,s) + ", " + to_string(q) + "u)"
                    : "WaveReadLaneAt(" + slot(raw,s) + ", (WaveGetLaneIndex() & ~1u) | " + to_string(q) + "u)";
                emit(slot(value,0) + " = " + read + ";");
                string field = "(" + slot(value,0) + " & " + hex32(maskOf(l.bw)) + ")";
                if (q) field = "(" + field + " << " + to_string(q*l.bw) + "u)";
                expression += (expression.empty() ? "" : " | ") + field;
            }
            emit(slot(packed,s) + " = " + expression + ";");
        }
        for (unsigned s = 0; s < l.S; ++s) {
            string expression;
            for (unsigned part = 0; part < l.epd; ++part) {
                unsigned source = s*l.epd+part;
                if (source >= sourceSlots) break;
                string shuffled = newTmp(1);
                emit(slot(shuffled,0) + " = WaveReadLaneAt(" + slot(packed,source) + ", (WaveGetLaneIndex() & ~15u) | ((WaveGetLaneIndex() * " + to_string(l.epd) + "u) & 15u));");
                if (expression.empty()) expression = slot(shuffled,0);
                else expression = "((lane / " + to_string(16/l.epd) + "u == " + to_string(part) + "u) ? " + slot(shuffled,0) + " : " + expression + ")";
            }
            if ((s+1)*16 > l.W) expression = "((" + to_string(s*16) + "u + lane < " + to_string(l.W) + "u) ? " + expression + " : 0u)";
            emit(slot(dst,s) + " = " + expression + ";");
        }
        return true;
    }
    void elementwise(Value* result, const std::vector<Value*>& ops, const std::function<string(const std::vector<string>&)>& f)
    {
        Lay rl = layOf(result->getType());
        string d = nm(result);
        if (rl.U)
        {
            std::vector<string> a;
            for (Value* o : ops) a.push_back(opBits(o, 0, 0));
            string e = f(a);
            emit(d + " = " + wrapMask(rl.k, e) + ";");
            return;
        }
        bool resE = isE(result);
        unsigned repd = resE ? 1 : rl.epd;
        unsigned rS = resE ? std::max(1u, (rl.N + 15) / 16) : rl.S;
        bool same = true;
        for (Value* o : ops)
        {
            Lay ol = layOf(o->getType());
            if (ol.U) continue;
            unsigned oe = isE(o) ? 1 : ol.epd;
            if (auto* c = dyn_cast<Constant>(o)) { if (!isSplat(c) && oe != repd) same = false; continue; }
            if (oe != repd) same = false;
        }
        if (same)
        {
            for (unsigned s = 0; s < rS; ++s)
            {
                string tot;
                for (unsigned q = 0; q < repd; ++q)
                {
                    std::vector<string> a;
                    for (Value* o : ops) a.push_back(opBits(o, s, q));
                    string e = wrapMask(rl.k, f(a));
                    tot += (q ? " | (" : "(") + e + (q ? ")<<" + to_string(q * rl.bw) + "u" : ")");
                }
                emit(slot(d, s) + " = " + tot + ";");
            }
            return;
        }
        // mismatched element sizes: compute with one element per dword, then repack
        std::vector<Value*> eo;
        std::vector<std::pair<string, unsigned>> tmps;   // unpacked temporaries
        std::map<Value*, string> unp;
        for (Value* o : ops)
        {
            Lay ol = layOf(o->getType());
            if (ol.U || isa<Constant>(o) || ol.epd == 1 || isE(o)) continue;
            unsigned sl; unp[o] = unpackE(o, sl);
        }
        unsigned nS = std::max(1u, (rl.N + 15) / 16);
        string rt = resE ? d : newTmp(nS);
        for (unsigned s = 0; s < nS; ++s)
        {
            std::vector<string> a;
            for (Value* o : ops)
            {
                Lay ol = layOf(o->getType());
                if (ol.U) a.push_back(opBits(o, 0, 0));
                else if (auto* c = dyn_cast<Constant>(o))
                {
                    if (isSplat(c)) a.push_back(litStr(ol.k, constBits(c, 0)));
                    else
                    {
                        string arr = "c" + to_string(nconst++);
                        globals << "static const uint " << arr << "[16] = {";
                        for (unsigned ln = 0; ln < 16; ++ln) globals << (ln ? "," : "") << hex32(16 * s + ln < ol.N ? (uint32_t)constBits(c, 16 * s + ln) : 0);
                        globals << "};\n";
                        a.push_back(arr + "[lane]");
                    }
                }
                else if (ol.epd == 1 || isE(o)) a.push_back(slot(nm(o), s));
                else a.push_back(slot(unp[o], s));
            }
            emit(slot(rt, s) + " = " + wrapMask(rl.k, f(a)) + ";");
        }
        if (resE) return;
        if (packEarly(d, rt, rl)) return;
        gather(d, rl.W, rl.epd, "", [&](unsigned dw, unsigned q) {
            Pc p; p.k = Pc::SrcD; unsigned e = dw * rl.epd + q;
            if (e >= rl.N) { p.k = Pc::Zero; return p; }
            p.v = nullptr; p.base = rt; p.dw = e; p.sh = 0; p.bits = rl.bw;
            return p;
        });
        (void)eo; (void)tmps;
    }
    string wrapMask(EK k, const string& e)
    {
        if (k == I32 || k == F32 || k == I64) return e;
        return "((" + e + ")&" + hex32(maskOf(ebits(k))) + ")";
    }

    string arith(Instruction& I, EK k, const std::vector<string>& a)
    {
        auto bin = [&](const char* op) { return "(" + a[0] + ")" + op + "(" + a[1] + ")"; };
        auto fbin = [&](const char* op) { return fromF(k, "(" + toF(k, a[0]) + ")" + op + "(" + toF(k, a[1]) + ")"); };
        switch (I.getOpcode()) {
        case Instruction::Add: return bin("+");
        case Instruction::Sub: return bin("-");
        case Instruction::Mul: return bin("*");
        case Instruction::And: return bin("&");
        case Instruction::Or: return bin("|");
        case Instruction::Xor: return bin("^");
        case Instruction::UDiv: return bin("/");
        case Instruction::URem: return bin("%");
        case Instruction::Shl: return "((" + a[1] + ")>=" + to_string(ebits(k) == 1 ? 1 : ebits(k)) + "u ? 0u : ((" + a[0] + ")<<(" + a[1] + ")))";
        case Instruction::LShr: return "((" + a[1] + ")>=" + to_string(ebits(k)) + "u ? 0u : ((" + a[0] + ")>>(" + a[1] + ")))";
        case Instruction::AShr: return string(k == I64 ? "uint64_t(" : "asuint(") + sx(k, a[0]) + ">>min(" + a[1] + "," + to_string(ebits(k) - 1) + "u))";
        case Instruction::SDiv: return string(k == I64 ? "uint64_t(" : "asuint(") + sx(k, a[0]) + "/" + sx(k, a[1]) + ")";
        case Instruction::SRem: return string(k == I64 ? "uint64_t(" : "asuint(") + sx(k, a[0]) + "%" + sx(k, a[1]) + ")";
        case Instruction::FAdd: return fbin("+");
        case Instruction::FSub: return fbin("-");
        case Instruction::FMul: return fbin("*");
        case Instruction::FDiv: return fbin("/");
        case Instruction::FRem: return fbin("%");
        case Instruction::FNeg: return fromF(k, "-(" + toF(k, a[0]) + ")");
        default: die("arith: unsupported opcode");
        }
    }

    string icmpExpr(CmpInst::Predicate p, EK k, const string& a, const string& b)
    {
        string sa = sx(k, a), sb = sx(k, b);
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
    string fcmpExpr(CmpInst::Predicate p, EK k, const string& a0, const string& b0)
    {
        string a = toF(k, a0), b = toF(k, b0);
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

    // ---------------- genx region intrinsics ----------------
    // destination element j of a region read/write with parameters; returns source element index or -1
    struct Region { long off; long vs, w, st; };

    void rdregion(CallInst& C)
    {
        Type* rt = C.getType();
        Value* src = C.getArgOperand(0);
        Lay rl = layOf(rt), sl = layOf(src->getType());
        EK k = rl.k;
        if (k == I64) die("rdregion on 64-bit elements");
        long vs = cint(C.getArgOperand(1)), w = cint(C.getArgOperand(2)), st = cint(C.getArgOperand(3));
        unsigned esz = std::max(1u, ebits(k) / 8);
        auto* co = dyn_cast<ConstantInt>(C.getArgOperand(4));
        if (!co) die("indirect rdregion");
        long off = co->getZExtValue() / esz;
        string d = nm(&C);
        auto srcIdx = [&](unsigned j) -> long { return off + (long)(j / w) * vs + (long)(j % w) * st; };
        if (rl.U)
        {
            long e = srcIdx(0);
            Pc p = (e >= 0 && (unsigned)e < sl.N) ? elemPiece(src, (unsigned)e) : Pc();
            emit(d + " = " + scalarFromPiece(p, rl.k) + ";");
            return;
        }
        unsigned epd = isE(&C) ? 1 : rl.epd;
        gather(d, isE(&C) ? rl.N : rl.W, epd, "", [&](unsigned dw, unsigned q) {
            unsigned j = dw * epd + q;
            if (j >= rl.N) return Pc();
            long e = srcIdx(j);
            if (e < 0 || (unsigned)e >= sl.N) return Pc();
            return elemPiece(src, (unsigned)e);
        });
    }

    // value of a piece as a uniform scalar (bits expression)
    string scalarFromPiece(const Pc& p, EK k)
    {
        unsigned bits = std::min(32u, ebits(k));
        switch (p.k) {
        case Pc::Zero: case Pc::Keep: return k == I64 ? "uint64_t(0)" : "0u";
        case Pc::Lit: return hex32(p.lit);
        case Pc::SrcU: { string u = nm(p.v); return p.bits >= 32 ? u : "(" + u + "&" + hex32(maskOf(p.bits)) + ")"; }
        case Pc::SrcD: {
            string x;
            if (getenv("X4R_DIRECT_CB") && p.base.empty()) if (auto* load = dyn_cast<CallInst>(p.v)) {
                Function* function = load->getCalledFunction();
                if (function && function->getName().str().rfind("llvm.genx.lsc.load.bti",0)==0 && load->arg_size()>11 && surfaceArg.count(load->getArgOperand(11))) {
                    int surface = surfaceArg[load->getArgOperand(11)];
                    if (surface>=0 && res[surface].kind=="CBV") {
                        string address = "((" + scalarExpr(load->getArgOperand(10)) + ")*" + to_string(cint(load->getArgOperand(4))) + "u + " + to_string(cint(load->getArgOperand(5))) + "u)";
                        string word = "(((" + address + ")>>2u) + " + to_string(p.dw) + "u)";
                        x = "CBD" + to_string(surface) + "[" + word + ">>2u][" + word + "&3u]";
                    }
                }
            }
            if (x.empty()) x = "x4r_rl(" + slot(nm(p.v), p.dw / 16) + "," + to_string(p.dw % 16) + "u)";
            if (p.sh) x = "(" + x + ">>" + to_string(p.sh) + "u)";
            if (p.bits < 32 || p.sh) x = "(" + x + "&" + hex32(maskOf(std::min(p.bits, bits))) + ")";
            return x;
        }
        }
        return "0u";
    }

    void wrregion(CallInst& C)
    {
        Type* rt = C.getType();
        Value* old = C.getArgOperand(0);
        Value* nw = C.getArgOperand(1);
        Lay rl = layOf(rt), nl = layOf(nw->getType());
        EK k = rl.k;
        if (k == I64) die("wrregion on 64-bit elements");
        long vs = cint(C.getArgOperand(2)), w = cint(C.getArgOperand(3)), st = cint(C.getArgOperand(4));
        unsigned esz = std::max(1u, ebits(k) / 8);
        if (auto* cm = dyn_cast<ConstantInt>(C.getArgOperand(7))) { if (!cm->isOne()) die("wrregion with false mask"); }
        else die("wrregion with variable mask");
        string d = nm(&C);
        bool oldConst = isa<Constant>(old);
        bool oldUndef = isa<UndefValue>(old);
        string keep = oldConst ? "" : nm(old);
        if (!oldConst && layOf(old->getType()).U) die("wrregion old scalar");
        auto oldPiece = [&](long j) -> Pc { if (oldConst) return j >= 0 && (unsigned)j < rl.N ? elemPiece(old, (unsigned)j) : Pc(); return keepPiece(); };
        unsigned n = nl.N;
        if (auto* co = dyn_cast<ConstantInt>(C.getArgOperand(5)))
        {
            long off = co->getZExtValue() / esz;
            unsigned epd = isE(&C) ? 1 : rl.epd;
            bool convertOld = isE(&C) || isE(old);
            auto preserved = [&](long j) -> Pc {
                if (convertOld) return j >= 0 && (unsigned)j < rl.N ? elemPiece(old, (unsigned)j) : Pc();
                return oldPiece(j);
            };
            gather(d, isE(&C) ? rl.N : rl.W, epd, convertOld ? "" : keep, [&](unsigned dw, unsigned q) {
                long j = (long)dw * epd + q;
                // is element j written by the region?
                long rel = j - off;
                if (rel < 0) return preserved(j);
                // region element m at off + (m/w)*vs + (m%w)*st == j
                long m = -1;
                if (vs == 0 && w == (long)n && st == 1) { if (rel < (long)n) m = rel; }
                else
                {
                    for (unsigned mm = 0; mm < n; ++mm)
                        if ((long)(mm / w) * vs + (long)(mm % w) * st == rel) { m = mm; break; }
                }
                if (m < 0) return preserved(j);
                return elemPiece(nw, (unsigned)m);
            });
            return;
        }
        // variable byte offset: only used to clear a contiguous range with zero
        if (!(isa<Constant>(nw) && constBits(cast<Constant>(nw), 0) == 0 && isSplat(cast<Constant>(nw)) && vs == 0 && w == (long)n && st == 1))
            die("indirect wrregion that is not a zero fill");
        Value* offv = C.getArgOperand(5);
        string offE = "(" + opBits(offv, 0, 0) + "/" + to_string(esz) + "u)";
        for (unsigned s = 0; s < rl.S; ++s)
        {
            string cur = oldConst ? phiSlotSrc(old, s) : slot(keep, s);
            if (rl.epd == 1)
                emit(slot(d, s) + " = x4r_clearrange(" + cur + ", " + to_string(s * 16) + "u + lane, " + offE + ", " + to_string(n) + "u);");
            else
            {
                string tot;
                for (unsigned q = 0; q < rl.epd; ++q)
                {
                    string e = "(" + to_string(s * 16 * rl.epd + q) + "u + lane*" + to_string(rl.epd) + "u)";
                    string kept = "(x4r_inrange(" + e + ", " + offE + ", " + to_string(n) + "u) ? 0u : ((" + cur + ">>" + to_string(q * rl.bw) + "u)&" + hex32(maskOf(rl.bw)) + "))";
                    tot += (q ? " | (" : "(") + kept + (q ? ")<<" + to_string(q * rl.bw) + "u" : ")");
                }
                emit(slot(d, s) + " = " + tot + ";");
            }
        }
    }
    static Pc keepPiece() { Pc p; p.k = Pc::Keep; return p; }

    // ---------------- memory ----------------
    int surfaceOf(Value* v)
    {
        auto it = surfaceArg.find(v);
        if (it == surfaceArg.end()) die("surface operand is not convert_int(arg): " + str(v));
        return it->second;
    }
    string scalarExpr(Value* v) { return opBits(v, 0, 0); }

    // load W dwords starting at byte address `addr` (uniform) into dst slots
    void blockLoad(int surf, bool slm, const string& addr, const string& dst, unsigned W)
    {
        unsigned S = std::max(1u, (W + 15) / 16);
        for (unsigned s = 0; s < S; ++s)
        {
            string ix = to_string(16 * s) + "u + lane";
            string a = "(" + addr + ") + (" + ix + ")*4u";
            string rd;
            if (slm) rd = "slm[((" + addr + ")>>2) + " + ix + "]";
            else
            {
                const Res& r = res[surf];
                if (r.kind == "CBV") rd = "CBD" + to_string(surf) + "[(((" + addr + ")>>2) + " + ix + ")>>2][(((" + addr + ")>>2) + " + ix + ")&3u]";
                else rd = "R" + to_string(surf) + ".Load(" + a + ")";
            }
            bool full = (s + 1) * 16 <= W;
            emit(slot(dst, s) + " = " + (full ? rd : "((" + to_string(16 * s) + "u + lane < " + to_string(W) + "u) ? " + rd + " : 0u)") + ";");
        }
    }
    void blockStore(int surf, bool slm, const string& addr, Value* data, unsigned W)
    {
        unsigned S = std::max(1u, (W + 15) / 16);
        for (unsigned s = 0; s < S; ++s)
        {
            string ix = to_string(16 * s) + "u + lane";
            string val = layOf(data->getType()).U ? opBits(data, 0, 0) : isa<Constant>(data) ? opBits(data, s, 0) : slot(nm(data), s);
            string st;
            if (slm) st = "slm[((" + addr + ")>>2) + " + ix + "] = " + val + ";";
            else
            {
                if (res[surf].kind != "UAV") die("store to non-UAV surface");
                st = "R" + to_string(surf) + ".Store((" + addr + ") + (" + ix + ")*4u, " + val + ");";
            }
            bool full = (s + 1) * 16 <= W;
            emit(full ? st : "if (" + to_string(16 * s) + "u + lane < " + to_string(W) + "u) { " + st + " }");
        }
    }
    string dataSlot(Value* data, unsigned s)
    {
        return isa<Constant>(data) ? opBits(data, s, 0) : slot(nm(data), s);
    }

    void lscLoadStore(CallInst& C, bool isStore, bool slm)
    {
        // order 1 (non-transposed) with one 32-bit element is the same access as a one-dword transposed block
        bool singleDword = cint(C.getArgOperand(8)) == 1 && cint(C.getArgOperand(7)) == 1 && cint(C.getArgOperand(6)) == 3;
        if (cint(C.getArgOperand(8)) != 2 && !singleDword) die("non-transposed LSC access");
        int64_t imm = cint(C.getArgOperand(5)), scale = cint(C.getArgOperand(4));
        string addr = "((" + scalarExpr(C.getArgOperand(10)) + ")*" + to_string(scale) + "u + " + to_string(imm) + "u)";
        Value* data = isStore ? C.getArgOperand(11) : nullptr;
        Type* dt = isStore ? data->getType() : C.getType();
        unsigned W = layOf(dt).W;
        int surf = slm ? -1 : surfaceOf(C.getArgOperand(isStore ? 12 : 11));
        if (isStore) blockStore(surf, slm, addr, data, W);
        else if (layOf(dt).U)
        {
            // a scalar result (<1 x T> returned as T) is one uniform dword
            string rd;
            if (slm) rd = "slm[(" + addr + ")>>2]";
            else if (res[surf].kind == "CBV") rd = "CBD" + to_string(surf) + "[((" + addr + ")>>2)>>2][((" + addr + ")>>2)&3u]";
            else rd = "R" + to_string(surf) + ".Load(" + addr + ")";
            emit(nm(&C) + " = " + rd + ";");
        }
        else blockLoad(surf, slm, addr, nm(&C), W);
    }

    // per-lane predicate expression of an <N x i1> mask at slot s ("" if constant true)
    string maskExpr(Value* m, unsigned s)
    {
        if (auto* c = dyn_cast<Constant>(m)) { if (isSplat(c) && constBits(c, 0) == 1) return ""; }
        return "(" + opBits(m, s, 0) + "!=0u)";
    }

    // ---------------- dpas ----------------
    bool mixedDotUsed = false;
    void dpas(CallInst& C, Value* acc, Value* B, Value* A, unsigned precB, unsigned precA, unsigned rep)
    {
        auto isSigned = [&](unsigned p) { if (p == 8) return true; if (p == 7) return false; die("dpas: unsupported precision " + to_string(p)); };
        bool sB = isSigned(precB), sA = isSigned(precA);
        if (wmmaEligible(C, B, rep)) { dpasWmma(C, acc, B, A, precB, precA, rep); return; }
        string d = nm(&C);
        if (getenv("X4R_NO_DPAS")) { for (unsigned r = 0; r < rep; ++r) emit(slot(d, r) + " = " + (acc ? dataSlot(acc, r) : dataSlot(B, r % 8)) + " ^ " + dataSlot(A, r % 4) + ";"); return; }
        // B: slot k lane n holds the 4 bytes k*4..k*4+3 of column n. A: row r dword k is uniform across lanes.
        for (unsigned r = 0; r < rep; ++r)
        {
            string s = acc ? "asint(" + dataSlot(acc, r) + ")" : "0";
            string cur = "ac" + to_string(ntmp++);
            decls << "  int " << cur << ";\n";
            emit(cur + " = " + s + ";");
            for (unsigned k = 0; k < 8; ++k)
            {
                unsigned aw = r * 8 + k;
                if (layOf(A->getType()).U) die("dpas A scalar");
                string a = "x4r_rl(" + dataSlot(A, aw / 16) + "," + to_string(aw % 16) + "u)";
                string b = dataSlot(B, k);
                if (sA && sB) emit(cur + " = dot4add_i8packed(" + a + ", " + b + ", " + cur + ");");
                else if (!sA && !sB) emit(cur + " = asint(dot4add_u8packed(" + a + ", " + b + ", asuint(" + cur + ")));");
                else if (getenv("X4R_NATIVE_MIXED_DOT"))
                {
                    mixedDotUsed = true;
                    emit(cur + " = " + cur + " + x4r_sudot(" + (sA ? a : b) + ", " + (sA ? b : a) + ", 0);");
                }
                else
                {
                    // mixed signedness: use the signed dot product plus a correction for the 'unsigned' operand's top bits:
                    //   sum(a_u*b) = sum(a_s*b) + 256*sum(hi(a)*b) with hi(a) = byte >> 7 (0/1);  (if only B is unsigned the roles swap)
                    string ta = "aw" + to_string(ntmp++), tb = "bw" + to_string(ntmp++);
                    decls << "  uint " << ta << ", " << tb << ";\n";
                    emit(ta + " = " + a + "; " + tb + " = " + b + ";");
                    string uOp = sA ? tb : ta, sOp = sA ? ta : tb;   // unsigned operand, signed operand
                    emit(cur + " = dot4add_i8packed(" + ta + ", " + tb + ", " + cur + ") + (dot4add_i8packed((" + uOp + ">>7u)&0x01010101u, " + sOp + ", 0) << 8);");
                }
            }
            emit(slot(d, r) + " = asuint(" + cur + ");");
        }
    }

    // Combine independent eight-element exp2 slices into full sixteen-lane
    // vectors. Preserve the original multiplication and exponential per value.
    void buildExpTiles()
    {
        if (!getenv("X4R_EXP_TILES")) return;
        struct Candidate { BasicBlock* block; Value* source; uint32_t scale; std::vector<std::pair<CallInst*,unsigned>> slices; std::set<unsigned> covered; };
        std::vector<Candidate> candidates;
        int saved = fwdOverride; fwdOverride = 3;
        for (auto& BB : *F) for (auto& I : BB) {
            auto* C = dyn_cast<CallInst>(&I);
            if (!C || !C->getCalledFunction() || C->getCalledFunction()->getName().str().find("native_exp2") == string::npos) continue;
            Lay result = layOf(C->getType());
            if (result.k != F32 || result.N != 8 || C->arg_size() != 1) continue;
            auto* mul = dyn_cast<BinaryOperator>(C->getArgOperand(0));
            if (!mul || mul->getOpcode() != Instruction::FMul) continue;
            Value* input = mul->getOperand(0); auto* scale = dyn_cast<Constant>(mul->getOperand(1));
            if (!scale) { input = mul->getOperand(1); scale = dyn_cast<Constant>(mul->getOperand(0)); }
            if (!scale || !isSplat(scale)) continue;
            Pc first = elemPiece(input,0); bool contiguous = first.k == Pc::SrcD && first.v && first.base.empty() && first.bits == 32 && !first.sh;
            for (unsigned e = 1; e < 8 && contiguous; ++e) {
                Pc p = elemPiece(input,e);
                contiguous = p.k == Pc::SrcD && p.v == first.v && p.base.empty() && p.bits == 32 && !p.sh && p.dw == first.dw+e;
            }
            if (!contiguous) continue;
            Lay source = layOf(first.v->getType());
            auto* producer = dyn_cast<Instruction>(first.v);
            if (source.k != F32 || source.U || source.N%16 || source.N>256 || !producer || producer->getParent()!=&BB || !producer->comesBefore(C)) continue;
            uint32_t bits = (uint32_t)constBits(scale,0);
            Candidate* group = nullptr;
            for (auto& g : candidates) if (g.block==&BB && g.source==first.v && g.scale==bits) { group=&g; break; }
            if (!group) { candidates.push_back({&BB,first.v,bits,{},{}}); group=&candidates.back(); }
            group->slices.push_back({C,first.dw});
            for (unsigned e = 0; e < 8; ++e) group->covered.insert(first.dw+e);
        }
        fwdOverride = saved;
        for (auto& g : candidates) {
            Lay l = layOf(g.source->getType());
            if (g.covered.size()!=l.N || *g.covered.begin()!=0 || *g.covered.rbegin()!=l.N-1) continue;
            unsigned id = expTiles.size();
            expTiles.push_back({g.source,g.scale,newTmp(l.S),false});
            for (auto& slice : g.slices) expSlices[slice.first]={id,slice.second};
            if (getenv("X4R_COST")) fprintf(stderr,"EXP_TILE %u elements, %zu slices\n",l.N,g.slices.size());
        }
    }
    // ---------------- calls ----------------
    void call(CallInst& C)
    {
        Function* f = C.getCalledFunction();
        if (!f) die("indirect call");
        string n = f->getName().str();
        Type* rt = C.getType();
        auto starts = [&](const char* p) { return n.rfind(p, 0) == 0; };
        string d = rt->isVoidTy() ? "" : nm(&C);
        auto exponential = expSlices.find(&C);
        if (exponential != expSlices.end()) {
            auto& tile = expTiles[exponential->second.first];
            if (!tile.emitted) {
                Lay l = layOf(tile.source->getType());
                for (unsigned s = 0; s < l.S; ++s)
                    emit(slot(tile.temp,s) + " = asuint(exp2(asfloat(" + dataSlot(tile.source,s) + ") * asfloat(" + hex32(tile.scale) + ")));");
                tile.emitted = true;
            }
            gather(d,8,1,"",[&](unsigned dw,unsigned) { return elemPiece(&C,dw); });
            return;
        }

        if (n.find("convert_int") != string::npos && C.arg_size() == 1 && C.getArgOperand(0)->getType()->isTargetExtTy())
        {
            auto* a = cast<Argument>(C.getArgOperand(0));
            auto it = argRes.find(a);
            surfaceArg[&C] = it != argRes.end() ? it->second : -1;
            emit(d + " = " + to_string(surfaceArg[&C] < 0 ? 0 : surfaceArg[&C]) + "u;");
            return;
        }
        if (n == "_Z12get_group_idj") { emit(d + " = uint64_t(gid[" + scalarExpr(C.getArgOperand(0)) + "]);"); return; }
        if (n == "_Z12get_local_idj") { emit(d + " = uint64_t(ltid[" + scalarExpr(C.getArgOperand(0)) + "]);"); return; }
        if (n == "_Z14get_local_sizej") { emit(d + " = uint64_t(lsz[" + scalarExpr(C.getArgOperand(0)) + "]);"); return; }
        if (n == "_Z19get_local_linear_idv") { emit(d + " = uint64_t(cmt);"); return; }
        if (n == "_Z20get_global_linear_idv") { emit(d + " = uint64_t((gid.x + gid.y*ngx + gid.z*ngx*ngy)*" + to_string(LX * LY * LZ) + "u + cmt);"); return; }
        if (n == "_Z7barrierj") { emit("GroupMemoryBarrierWithGroupSync();"); return; }
        if (starts("_Z") && n.find("native_") != string::npos)
        {
            EK k = ekOf(rt);
            string fn = n.find("log2") != string::npos ? "log2" : n.find("exp2") != string::npos ? "exp2" : n.find("sqrt") != string::npos ? "sqrt" :
                        n.find("recip") != string::npos ? "x4r_rcp" : n.find("powr") != string::npos ? "pow" : n.find("cos") != string::npos ? "cos" :
                        n.find("sin") != string::npos ? "sin" : "";
            if (fn.empty()) die("unknown native function " + n);
            std::vector<Value*> ops; for (unsigned i = 0; i < C.arg_size(); ++i) ops.push_back(C.getArgOperand(i));
            elementwise(&C, ops, [&](const std::vector<string>& a) {
                string e = a.size() == 2 ? fn + "(" + toF(k, a[0]) + "," + toF(k, a[1]) + ")" : fn + "(" + toF(k, a[0]) + ")";
                return fromF(k, e);
            });
            return;
        }
        if (starts("llvm.vector.reduce.mul"))
        {
            Value* x = C.getArgOperand(0);
            unsigned cnt = layOf(x->getType()).N;
            string e;
            for (unsigned i = 0; i < cnt; ++i) e += (i ? "*" : "") + scalarFromPiece(elemPiece(x, i), I32);
            emit(d + " = " + e + ";");
            return;
        }
        if (starts("llvm.lifetime") || starts("llvm.assume") || starts("llvm.dbg")) return;
        if (starts("llvm.fmuladd") || starts("llvm.fma"))
        {
            EK k = ekOf(rt);
            elementwise(&C, {C.getArgOperand(0), C.getArgOperand(1), C.getArgOperand(2)}, [&](const std::vector<string>& a) {
                return fromF(k, "mad(" + toF(k, a[0]) + "," + toF(k, a[1]) + "," + toF(k, a[2]) + ")");
            });
            return;
        }
        if (starts("llvm.sqrt") || starts("llvm.fabs") || starts("llvm.floor") || starts("llvm.ceil") || starts("llvm.trunc") || starts("llvm.exp2") || starts("llvm.log2") || starts("llvm.rint") ||
            starts("llvm.genx.rndd") || starts("llvm.genx.rndu") || starts("llvm.genx.rnde") || starts("llvm.genx.rndz"))
        {
            EK k = ekOf(rt);
            string fn = starts("llvm.sqrt") ? "sqrt" : starts("llvm.fabs") ? "abs" : starts("llvm.floor") || starts("llvm.genx.rndd") ? "floor" : starts("llvm.ceil") || starts("llvm.genx.rndu") ? "ceil" :
                        starts("llvm.trunc") || starts("llvm.genx.rndz") ? "trunc" : starts("llvm.exp2") ? "exp2" : starts("llvm.log2") ? "log2" : "round";
            elementwise(&C, {C.getArgOperand(0)}, [&](const std::vector<string>& a) { return fromF(k, fn + "(" + toF(k, a[0]) + ")"); });
            return;
        }
        if (starts("llvm.maxnum") || starts("llvm.minnum") || starts("llvm.maximum") || starts("llvm.minimum") || starts("llvm.genx.fmax") || starts("llvm.genx.fmin"))
        {
            EK k = ekOf(rt);
            string fn = n.find("max") != string::npos ? "max" : "min";
            elementwise(&C, {C.getArgOperand(0), C.getArgOperand(1)}, [&](const std::vector<string>& a) { return fromF(k, fn + "(" + toF(k, a[0]) + "," + toF(k, a[1]) + ")"); });
            return;
        }
        if (starts("llvm.smax") || starts("llvm.smin") || starts("llvm.umax") || starts("llvm.umin") || starts("llvm.genx.smax") || starts("llvm.genx.smin") || starts("llvm.genx.umax") || starts("llvm.genx.umin"))
        {
            EK k = ekOf(rt);
            bool sg = n.find(".s") != string::npos && (n.find("smax") != string::npos || n.find("smin") != string::npos);
            string fn = n.find("max") != string::npos ? "max" : "min";
            elementwise(&C, {C.getArgOperand(0), C.getArgOperand(1)}, [&](const std::vector<string>& a) {
                return sg ? "asuint(" + fn + "(" + sx(k, a[0]) + "," + sx(k, a[1]) + "))" : fn + "(" + a[0] + "," + a[1] + ")";
            });
            return;
        }
        if (starts("llvm.abs"))
        {
            EK k = ekOf(rt);
            elementwise(&C, {C.getArgOperand(0)}, [&](const std::vector<string>& a) { return "asuint(abs(" + sx(k, a[0]) + "))"; });
            return;
        }
        if (starts("llvm.genx.rdregion")) { rdregion(C); return; }
        if (starts("llvm.genx.wrregion")) { wrregion(C); return; }
        if (starts("llvm.genx.fptosi.sat") || starts("llvm.genx.fptoui.sat"))
        {
            EK k = ekOf(rt), sk = ekOf(C.getArgOperand(0)->getType());
            bool sg = starts("llvm.genx.fptosi");
            unsigned bits = ebits(k);
            double lo = sg ? -(double)(1ull << (bits - 1)) : 0.0, hi = sg ? (double)((1ull << (bits - 1)) - 1) : (double)((1ull << bits) - 1);
            if (bits == 32) hi = sg ? 2147483520.0 : 4294967040.0;
            char lb[64], hb[64]; snprintf(lb, sizeof lb, "%.1f", lo); snprintf(hb, sizeof hb, "%.1f", hi);
            elementwise(&C, {C.getArgOperand(0)}, [&](const std::vector<string>& a) {
                string v = "(isnan(" + toF(sk, a[0]) + ") ? 0.0 : clamp(trunc(" + toF(sk, a[0]) + "), " + lb + ", " + hb + "))";   // NaN -> 0 like mov.sat
                return sg ? "asuint(int(" + v + "))" : "uint(" + v + ")";
            });
            return;
        }
        if (starts("llvm.genx.sstrunc.sat") || starts("llvm.genx.ustrunc.sat") || starts("llvm.genx.sutrunc.sat") || starts("llvm.genx.ussat"))
        {
            EK sk = ekOf(C.getArgOperand(0)->getType()), dk = ekOf(rt);
            bool srcSigned = starts("llvm.genx.ss") || starts("llvm.genx.su"), dstSigned = starts("llvm.genx.ss");
            unsigned bits = ebits(dk);
            elementwise(&C, {C.getArgOperand(0)}, [&](const std::vector<string>& a) {
                if (srcSigned)
                {
                    string s = sx(sk, a[0]);
                    if (dstSigned) return "asuint(clamp(" + s + "," + to_string(-(1 << (bits - 1))) + "," + to_string((1 << (bits - 1)) - 1) + "))";
                    return "uint(clamp(" + s + ",0," + to_string((1 << bits) - 1) + "))";
                }
                return "min(" + a[0] + "," + to_string((1u << bits) - 1) + "u)";
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
            int s = surfaceOf(C.getArgOperand(1));
            string off = scalarExpr(C.getArgOperand(2));
            if (n.find("unaligned") == string::npos) off = "(" + off + ")*16u";
            blockLoad(s, false, off, d, layOf(rt).W);
            return;
        }
        if (starts("llvm.genx.oword.st"))
        {
            int s = surfaceOf(C.getArgOperand(0));
            blockStore(s, false, "((" + scalarExpr(C.getArgOperand(1)) + ")*16u)", C.getArgOperand(2), layOf(C.getArgOperand(2)->getType()).W);
            return;
        }
        if (starts("llvm.genx.lsc.load.bti")) { lscLoadStore(C, false, false); return; }
        if (starts("llvm.genx.lsc.store.bti")) { lscLoadStore(C, true, false); return; }
        if (starts("llvm.genx.lsc.load.slm")) { lscLoadStore(C, false, true); return; }
        if (starts("llvm.genx.lsc.store.slm")) { lscLoadStore(C, true, true); return; }
        if (starts("llvm.genx.fence")) { emit("AllMemoryBarrier();"); return; }
        if (starts("llvm.genx.gather.masked.scaled2") || starts("llvm.genx.gather.scaled2"))
        {
            // (log2 nblocks, scale, surface, global offset, element byte offsets, [mask]); surface 254 = SLM
            if (cint(C.getArgOperand(2)) != 254) die("gather from non-SLM surface");
            Lay rl = layOf(rt);
            if (rl.epd != 1) die("gather of narrow elements");
            string goff = scalarExpr(C.getArgOperand(3));
            for (unsigned s = 0; s < rl.S; ++s)
            {
                string a = "((" + goff + " + " + opBits(C.getArgOperand(4), s, 0) + ")>>2)";
                string m = C.arg_size() > 5 ? maskExpr(C.getArgOperand(5), s) : "";
                emit(slot(d, s) + " = " + (m.empty() ? "" : m + " ? ") + "slm[" + a + "]" + (m.empty() ? "" : " : 0u") + ";");
            }
            return;
        }
        if (starts("llvm.genx.scatter.scaled"))
        {
            // (mask, log2 nblocks, scale, surface, global offset, element byte offsets, data)
            if (cint(C.getArgOperand(3)) != 254) die("scatter to non-SLM surface");
            Lay ol = layOf(C.getArgOperand(5)->getType());
            string goff = scalarExpr(C.getArgOperand(4));
            for (unsigned s = 0; s < ol.S; ++s)
            {
                string m = maskExpr(C.getArgOperand(0), s);
                emit((m.empty() ? "" : "if " + m + " ") + string("slm[((") + goff + " + " + opBits(C.getArgOperand(5), s, 0) + ")>>2)] = " + dataSlot(C.getArgOperand(6), s) + ";");
            }
            return;
        }
        if (starts("llvm.genx.lsc.xatomic.bti"))
        {
            // (pred, subop, l1, l3, scale, imm, dsize, vsize, order, chmask, offsets, src0, src1, surface, old)
            int sub = cint(C.getArgOperand(1));
            int s = surfaceOf(C.getArgOperand(13));
            string R = "R" + to_string(s);
            Lay pl = layOf(C.getArgOperand(0)->getType());
            string imm = to_string(cint(C.getArgOperand(5))) + "u";
            for (unsigned sl = 0; sl < pl.S; ++sl)
            {
                string m = maskExpr(C.getArgOperand(0), sl);
                string off = "(" + opBits(C.getArgOperand(10), sl, 0) + " + " + imm + ")";
                string src0 = dataSlot(C.getArgOperand(11), sl);
                string pre = m.empty() ? "" : "if " + m + " ";
                if (sub == 11) { emit(pre + R + ".Store(" + off + ", " + src0 + ");"); if (!rt->isVoidTy()) emit(slot(d, sl) + " = 0u;"); }
                else if (sub == 10) emit(slot(d, sl) + " = " + (m.empty() ? "" : m + " ? ") + R + ".Load(" + off + ")" + (m.empty() ? "" : " : 0u") + ";");
                else if (sub == 12) emit(slot(d, sl) + " = 0u; " + pre + "{ uint ov_; " + R + ".InterlockedAdd(" + off + ", " + src0 + ", ov_); " + slot(d, sl) + " = ov_; }");
                else if (sub == 0x13)
                    emit(slot(d, sl) + " = 0u; " + pre + "{ uint a_ = " + off + "; uint ov_ = " + R + ".Load(a_), nv_, pv_; [loop] while (true) { nv_ = asuint(asfloat(ov_) + asfloat(" + src0 + ")); " + R + ".InterlockedCompareExchange(a_, ov_, nv_, pv_); if (pv_ == ov_) break; ov_ = pv_; } " + slot(d, sl) + " = ov_; }");
                else die("unsupported atomic subop " + to_string(sub));
            }
            return;
        }
        if (starts("llvm.genx.3d.load") || starts("llvm.genx.3d.sample"))
        {
            bool smp = starts("llvm.genx.3d.sample");
            // load:   (op, mask, chmask, aoffimm, surface, u, v, ...)   sample: (op, mask, chmask, aoffimm, sampler, surface, u, v, ...)
            int ch = cint(C.getArgOperand(2));
            int surf = surfaceOf(C.getArgOperand(smp ? 5 : 4));
            unsigned ubase = smp ? 6 : 5;
            Lay ml = layOf(C.getArgOperand(1)->getType());
            if (ml.N != 16) die("3d op with " + to_string(ml.N) + " lanes");
            std::vector<int> chans; for (int c = 0; c < 4; ++c) if (ch & (1 << c)) chans.push_back(c);
            Lay rl = layOf(rt);
            if (rl.N != 16 * chans.size() || rl.k != F32) die("3d op result shape");
            if (smp) usesSampler = true;
            string R = "R" + to_string(surf);
            string u = opBits(C.getArgOperand(ubase), 0, 0), v = opBits(C.getArgOperand(ubase + 1), 0, 0);
            string m = maskExpr(C.getArgOperand(1), 0);
            string tv = "tx" + to_string(ntmp++);
            decls << "  float4 " << tv << ";\n";
            string fetch = smp ? tv + " = " + R + ".SampleLevel(smp0, float2(asfloat(" + u + "), asfloat(" + v + ")), 0.0);"
                               : tv + " = " + R + ".Load(int3(asint(" + u + "), asint(" + v + "), 0));";
            emit(tv + " = float4(0,0,0,0);");
            emit(m.empty() ? "{ " + fetch + " }" : "if " + m + " { " + fetch + " }");
            for (size_t q = 0; q < chans.size(); ++q) emit(slot(d, q) + " = asuint(" + tv + "." + "xyzw"[chans[q]] + ");");
            return;
        }
        if (starts("llvm.genx.lsc.store.quad.typed.bti") || starts("llvm.genx.lsc.load.merge.quad.typed.bti"))
        {
            bool st = starts("llvm.genx.lsc.store");
            // (pred, i8, i8, chmask, surface, u, v, r, lod, data/merge)
            int ch = cint(C.getArgOperand(3));
            int surf = surfaceOf(C.getArgOperand(4));
            Lay pl = layOf(C.getArgOperand(0)->getType());
            std::vector<int> chans; for (int c = 0; c < 4; ++c) if (ch & (1 << c)) chans.push_back(c);
            string R = "R" + to_string(surf);
            Value* data = C.getArgOperand(9);
            for (unsigned sl = 0; sl < pl.S; ++sl)
            {
                string m = maskExpr(C.getArgOperand(0), sl);
                string u = opBits(C.getArgOperand(5), sl, 0), v = opBits(C.getArgOperand(6), sl, 0);
                if (st)
                {
                    string t = "float4(";
                    for (int c = 0; c < 4; ++c)
                    {
                        size_t q = 0; bool en = false;
                        for (size_t z = 0; z < chans.size(); ++z) if (chans[z] == c) { q = z; en = true; }
                        t += (c ? "," : "") + (en ? "asfloat(" + dataSlot(data, q * pl.S + sl) + ")" : string("0.0"));
                    }
                    t += ")";
                    emit((m.empty() ? "" : "if " + m + " ") + R + "[uint2(" + u + "," + v + ")] = " + t + ";");
                }
                else
                {
                    string tv = "tx" + to_string(ntmp++);
                    decls << "  float4 " << tv << ";\n";
                    emit(tv + " = float4(0,0,0,0);");
                    emit((m.empty() ? "" : "if " + m + " ") + tv + " = " + R + "[uint2(" + u + "," + v + ")];");
                    for (size_t q = 0; q < chans.size(); ++q)
                    {
                        string dflt = isa<UndefValue>(data) ? "0u" : dataSlot(data, q * pl.S + sl);
                        emit(slot(d, q * pl.S + sl) + " = " + (m.empty() ? "" : m + " ? ") + "asuint(" + tv + "." + "xyzw"[chans[q]] + ")" + (m.empty() ? "" : " : " + dflt) + ";");
                    }
                }
            }
            return;
        }
        die("unsupported call: " + n);
    }

    // ---------------- other instructions ----------------
    void phiCopies(BasicBlock* from, BasicBlock* to)
    {
        std::vector<PHINode*> phis;
        for (auto& I : *to) { if (auto* p = dyn_cast<PHINode>(&I)) phis.push_back(p); else break; }
        if (phis.empty()) return;
        std::set<Value*> phiSet(phis.begin(), phis.end());
        bool needTmp = false;
        for (auto* p : phis) if (phiSet.count(p->getIncomingValueForBlock(from))) needTmp = true;
        auto copy = [&](const string& dst, Value* src, Type* t) {
            Lay l = layOf(t);
            if (l.U) { emit(dst + " = " + opBits(src, 0, 0) + ";"); return; }
            for (unsigned s = 0; s < l.S; ++s) emit(slot(dst, s) + " = " + (isa<Constant>(src) ? opBits(src, s, 0) : slot(nm(src), s)) + ";");
        };
        // constants for non-splat vectors: opBits only supports epd-1 slot values; handle packed constants via per-lane table
        if (!needTmp) { for (auto* p : phis) copyPhi(p, p->getIncomingValueForBlock(from)); return; }
        std::vector<std::pair<string, Lay>> tmps;
        for (auto* p : phis)
        {
            Lay l = layOf(p->getType());
            string t = l.U ? "t" + to_string(ntmp++) : newTmp(l.S);
            if (l.U) decls << (l.k == I64 ? "  uint64_t " : "  uint ") << t << ";\n";
            Value* src = p->getIncomingValueForBlock(from);
            if (l.U) emit(t + " = " + opBits(src, 0, 0) + ";");
            else for (unsigned s = 0; s < l.S; ++s) emit(slot(t, s) + " = " + phiSlotSrc(src, s) + ";");
            tmps.push_back({t, l});
        }
        for (size_t q = 0; q < phis.size(); ++q)
        {
            Lay l = tmps[q].second;
            string dst = nm(phis[q]);
            if (l.U) emit(dst + " = " + tmps[q].first + ";");
            else for (unsigned s = 0; s < l.S; ++s) emit(slot(dst, s) + " = " + slot(tmps[q].first, s) + ";");
        }
        (void)copy;
    }
    // packed dword s of a (possibly constant) distributed value
    string phiSlotSrc(Value* src, unsigned s)
    {
        if (auto* c = dyn_cast<Constant>(src))
        {
            Lay l = layOf(c->getType());
            if (isSplat(c))
            {
                uint32_t w = 0; for (unsigned q = 0; q < l.epd; ++q) w |= (uint32_t)(constBits(c, 0) & maskOf(l.bw)) << (q * l.bw);
                if (l.k == I64) w = (uint32_t)constBits(c, 0);   // low half; high half handled by caller (not used)
                return hex32(w);
            }
            string arr = "c" + to_string(nconst++);
            globals << "static const uint " << arr << "[16] = {";
            for (unsigned ln = 0; ln < 16; ++ln)
            {
                uint32_t w = 0; unsigned dw = 16 * s + ln;
                if (l.k == I64) { unsigned e = dw / 2; if (e < l.N) w = (uint32_t)(constBits(c, e) >> (32 * (dw & 1))); }
                else for (unsigned q = 0; q < l.epd; ++q) { unsigned e = dw * l.epd + q; if (e < l.N) w |= (uint32_t)(constBits(c, e) & maskOf(l.bw)) << (q * l.bw); }
                globals << (ln ? "," : "") << hex32(w);
            }
            globals << "};\n";
            return arr + "[lane]";
        }
        return slot(nm(src), s);
    }
    void copyPhi(PHINode* p, Value* src)
    {
        Lay l = layOf(p->getType());
        string dst = nm(p);
        if (l.U) { emit(dst + " = " + opBits(src, 0, 0) + ";"); return; }
        for (unsigned s = 0; s < l.S; ++s) emit(slot(dst, s) + " = " + phiSlotSrc(src, s) + ";");
    }

    void castOp(Instruction& I)
    {
        Type *st = I.getOperand(0)->getType(), *dt = I.getType();
        Lay sl = layOf(st), dl = layOf(dt);
        unsigned op = I.getOpcode();
        if (sl.k == I64 || dl.k == I64)
        {
            if (!(sl.U && dl.U)) die("vector cast with 64-bit elements");
            string a = opBits(I.getOperand(0), 0, 0), d = nm(&I);
            if (sl.k == I64 && dl.k != I64) emit(d + " = uint(" + a + ") & " + hex32(maskOf(ebits(dl.k))) + ";");
            else if (dl.k == I64 && sl.k != I64) emit(d + " = " + (op == Instruction::SExt ? "uint64_t(int64_t(" + sx(sl.k, a) + "))" : "uint64_t(" + a + ")") + ";");
            else die("64-bit cast " + str(&I));
            return;
        }
        EK s = sl.k, dk = dl.k;
        elementwise(&I, {I.getOperand(0)}, [&](const std::vector<string>& a) -> string {
            switch (op) {
            case Instruction::Trunc: case Instruction::ZExt: return a[0];
            case Instruction::SExt: return "asuint(" + sx(s, a[0]) + ")";
            case Instruction::FPToUI: return "uint(" + toF(s, a[0]) + ")";
            case Instruction::FPToSI: return "asuint(int(" + toF(s, a[0]) + "))";
            case Instruction::UIToFP: return fromF(dk, "float(" + a[0] + ")");
            case Instruction::SIToFP: return fromF(dk, "float(" + sx(s, a[0]) + ")");
            case Instruction::FPExt: case Instruction::FPTrunc: return fromF(dk, toF(s, a[0]));
            default: die("cast");
            }
        });
    }

    void bitcastOp(Instruction& I)
    {
        Type *st = I.getOperand(0)->getType(), *dt = I.getType();
        Lay sl = layOf(st), dl = layOf(dt);
        if (sl.k == B1 || dl.k == B1) die("i1 bitcast");
        Value* src = I.getOperand(0);
        string d = nm(&I);
        if (sl.U && dl.U)
        {
            string a = opBits(src, 0, 0);
            emit(d + " = " + (dl.k == I64 || sl.k == I64 ? a : a) + ";");
            return;
        }
        if (sl.U != dl.U)
        {
            // scalar <-> vector: only up to one dword
            if (sl.U)
            {
                if (dl.W != 1) die("scalar->vector bitcast of >1 dword");
                emit(slot(d, 0) + " = " + opBits(src, 0, 0) + ";");
            }
            else
            {
                if (sl.W != 1 && dl.k != I64) die("vector->scalar bitcast of >1 dword");
                emit(d + " = " + scalarFromPiece(dwordPiece(src, 0), dl.k) + ";");
            }
            return;
        }
        if (sl.W != dl.W) die("bitcast changes size");
        gather(d, dl.W, 1, "", [&](unsigned dw, unsigned) {
            if (auto* c = dyn_cast<Constant>(src))
            {
                Pc p; p.k = Pc::Lit; uint32_t w = 0;
                if (sl.k == I64) { unsigned e = dw / 2; w = (uint32_t)(constBits(c, e) >> (32 * (dw & 1))); }
                else for (unsigned q = 0; q < sl.epd; ++q) { unsigned e = dw * sl.epd + q; if (e < sl.N) w |= (uint32_t)(constBits(c, e) & maskOf(sl.bw)) << (q * sl.bw); }
                p.lit = w; return p;
            }
            return dwordPiece(src, dw);
        });
    }

    void traceStore(Instruction& I)
    {
        if (!trace || I.getType()->isVoidTy() || !tOff.count(&I)) return;
        Lay l = layOf(I.getType());
        unsigned off = tOff[&I];
        string base = "((gtid*" + to_string(tTotal) + "u) + " + to_string(off) + "u";
        if (l.U)
        {
            if (l.k == I64) emit("if (lane == 0u) { DBG.Store(" + base + ")*4u, uint(" + nm(&I) + ")); DBG.Store(" + base + "+1u)*4u, uint(" + nm(&I) + ">>32)); }");
            else emit("if (lane == 0u) DBG.Store(" + base + ")*4u, " + nm(&I) + ");");
            return;
        }
        string src = nm(&I);
        if (isE(&I)) { src = newTmp(l.S); gather(src, l.W, l.epd, "", [&](unsigned dw, unsigned q) { unsigned e = dw * l.epd + q; if (e >= l.N) return Pc(); return elemPiece(&I, e); }); }
        for (unsigned s = 0; s < l.S; ++s)
            emit("if (" + to_string(16 * s) + "u + lane < " + to_string(l.W) + "u) DBG.Store(" + base + " + " + to_string(16 * s) + "u + lane)*4u, " + slot(src, s) + ");");
    }

    void instr(Instruction& I)
    {
        unsigned op = I.getOpcode();
        {
            string cat = I.getOpcodeName();
            if (auto* C = dyn_cast<CallInst>(&I)) if (Function* f = C->getCalledFunction())
            { string n = f->getName().str(); if (n.rfind("llvm.genx.", 0) == 0) { size_t e = n.find('.', 10); cat = n.substr(10, e == string::npos ? e : e - 10); } else cat = n.substr(0, std::min<size_t>(24, n.size())); }
            if (!I.getType()->isVoidTy()) { Lay l = layOf(I.getType()); cat += string(l.U ? ":scalar" : ":vec") + (l.U ? "" : to_string(l.S) + "slots"); }
            curCat = cat; cntBy[cat]++;
        }
        switch (op)
        {
        case Instruction::PHI: nm(&I); return;
        case Instruction::Add: case Instruction::Sub: case Instruction::Mul: case Instruction::And: case Instruction::Or: case Instruction::Xor:
        case Instruction::UDiv: case Instruction::URem: case Instruction::Shl: case Instruction::LShr: case Instruction::AShr: case Instruction::SDiv: case Instruction::SRem:
        case Instruction::FAdd: case Instruction::FSub: case Instruction::FMul: case Instruction::FDiv: case Instruction::FRem: {
            EK k = ekOf(I.getType());
            if (k == I64 && !layOf(I.getType()).U) die("64-bit vector arithmetic");
            elementwise(&I, {I.getOperand(0), I.getOperand(1)}, [&](const std::vector<string>& a) { return arith(I, k, a); });
            return;
        }
        case Instruction::FNeg: {
            EK k = ekOf(I.getType());
            elementwise(&I, {I.getOperand(0)}, [&](const std::vector<string>& a) { return arith(I, k, a); });
            return;
        }
        case Instruction::ICmp: {
            auto& C = cast<ICmpInst>(I); EK ok = ekOf(C.getOperand(0)->getType());
            elementwise(&I, {C.getOperand(0), C.getOperand(1)}, [&](const std::vector<string>& a) { return "(" + icmpExpr(C.getPredicate(), ok, a[0], a[1]) + " ? 1u : 0u)"; });
            return;
        }
        case Instruction::FCmp: {
            auto& C = cast<FCmpInst>(I); EK ok = ekOf(C.getOperand(0)->getType());
            elementwise(&I, {C.getOperand(0), C.getOperand(1)}, [&](const std::vector<string>& a) { return "(" + fcmpExpr(C.getPredicate(), ok, a[0], a[1]) + " ? 1u : 0u)"; });
            return;
        }
        case Instruction::Select: {
            Value* c = I.getOperand(0);
            elementwise(&I, {c, I.getOperand(1), I.getOperand(2)}, [&](const std::vector<string>& a) { return "((" + a[0] + "!=0u) ? (" + a[1] + ") : (" + a[2] + "))"; });
            return;
        }
        case Instruction::Trunc: case Instruction::ZExt: case Instruction::SExt: case Instruction::FPToUI: case Instruction::FPToSI:
        case Instruction::UIToFP: case Instruction::SIToFP: case Instruction::FPExt: case Instruction::FPTrunc:
            castOp(I); return;
        case Instruction::BitCast: bitcastOp(I); return;
        case Instruction::ExtractElement: {
            auto* ci = dyn_cast<ConstantInt>(I.getOperand(1));
            if (!ci) die("dynamic extractelement");
            emit(nm(&I) + " = " + scalarFromPiece(elemPiece(I.getOperand(0), (unsigned)ci->getZExtValue()), ekOf(I.getType())) + ";");
            return;
        }
        case Instruction::InsertElement: {
            auto* ci = dyn_cast<ConstantInt>(I.getOperand(2));
            if (!ci) die("dynamic insertelement");
            Lay l = layOf(I.getType());
            if (l.k == I64) die("insertelement i64");
            Value* vec = I.getOperand(0); Value* ev = I.getOperand(1);
            unsigned idx = ci->getZExtValue();
            bool constBase = isa<Constant>(vec);
            bool convertBase = isE(&I) || isE(vec);
            unsigned epd = isE(&I) ? 1 : l.epd;
            gather(nm(&I), isE(&I) ? l.N : l.W, epd, (constBase || convertBase) ? "" : nm(vec), [&](unsigned dw, unsigned q) {
                unsigned e = dw * epd + q;
                if (e >= l.N && convertBase) return Pc();
                if (e == idx) return elemPiece(ev, 0);
                if (constBase || convertBase) return elemPiece(vec, e);
                return keepPiece();
            });
            return;
        }
        case Instruction::ShuffleVector: {
            auto& S = cast<ShuffleVectorInst>(I);
            Lay l = layOf(I.getType());
            if (l.k == I64) die("shufflevector i64");
            unsigned n1 = layOf(S.getOperand(0)->getType()).N;
            unsigned epd = isE(&I) ? 1 : l.epd;
            gather(nm(&I), isE(&I) ? l.N : l.W, epd, "", [&](unsigned dw, unsigned q) {
                unsigned e = dw * epd + q;
                if (e >= l.N) return Pc();
                int m = S.getMaskValue(e);
                if (m < 0) return Pc();
                return (unsigned)m < n1 ? elemPiece(S.getOperand(0), m) : elemPiece(S.getOperand(1), m - n1);
            });
            return;
        }
        case Instruction::Call: call(cast<CallInst>(I)); return;
        case Instruction::Freeze: {
            Lay l = layOf(I.getType());
            if (l.U) { emit(nm(&I) + " = " + opBits(I.getOperand(0), 0, 0) + ";"); return; }
            for (unsigned s = 0; s < l.S; ++s) emit(slot(nm(&I), s) + " = " + phiSlotSrc(I.getOperand(0), s) + ";");
            return;
        }
        default: die(string("unsupported instruction ") + I.getOpcodeName());
        }
    }


    // which narrow-vector results can stay one element per lane: elementwise producers whose users all read elements (regions, shuffles, extracts, elementwise ops)
    static bool isElementwiseCallName(const string& n)
    {
        static const char* pre[] = {"llvm.fmuladd", "llvm.fma", "llvm.sqrt", "llvm.fabs", "llvm.floor", "llvm.ceil", "llvm.trunc", "llvm.exp2", "llvm.log2", "llvm.rint", "llvm.genx.rnd",
            "llvm.maxnum", "llvm.minnum", "llvm.maximum", "llvm.minimum", "llvm.genx.fmax", "llvm.genx.fmin", "llvm.smax", "llvm.smin", "llvm.umax", "llvm.umin",
            "llvm.genx.smax", "llvm.genx.smin", "llvm.genx.umax", "llvm.genx.umin", "llvm.abs", "llvm.genx.fptosi.sat", "llvm.genx.fptoui.sat", "llvm.genx.sstrunc.sat",
            "llvm.genx.ustrunc.sat", "llvm.genx.sutrunc.sat", "llvm.genx.ussat"};
        for (const char* p : pre) if (n.rfind(p, 0) == 0) return true;
        return n.rfind("_Z", 0) == 0 && n.find("native_") != string::npos;
    }
    static bool isElementwiseOpcode(unsigned op)
    {
        switch (op) {
        case Instruction::Add: case Instruction::Sub: case Instruction::Mul: case Instruction::And: case Instruction::Or: case Instruction::Xor: case Instruction::UDiv: case Instruction::URem:
        case Instruction::Shl: case Instruction::LShr: case Instruction::AShr: case Instruction::SDiv: case Instruction::SRem: case Instruction::FAdd: case Instruction::FSub: case Instruction::FMul:
        case Instruction::FDiv: case Instruction::FRem: case Instruction::FNeg: case Instruction::Select: case Instruction::Trunc: case Instruction::ZExt: case Instruction::SExt:
        case Instruction::FPToUI: case Instruction::FPToSI: case Instruction::UIToFP: case Instruction::SIToFP: case Instruction::FPExt: case Instruction::FPTrunc: return true;
        default: return false;
        }
    }
    void computeEForms()
    {
        if (getenv("X4R_NO_EFORM")) return;
        for (auto& BB : *F)
            for (auto& I : BB)
            {
                Type* t = I.getType();
                if (t->isVoidTy() || !isa<FixedVectorType>(t)) continue;
                Lay l = layOf(t);
                if (l.epd <= 1 || l.k == B1 || l.k == I64) continue;
                if (const char* minimum = getenv("X4R_EFORM_MIN")) if (l.N < (unsigned)atoi(minimum)) continue;
                bool producer = isElementwiseOpcode(I.getOpcode());
                if (auto* C = dyn_cast<CallInst>(&I)) { Function* f = C->getCalledFunction(); producer = f && isElementwiseCallName(f->getName().str()); }
                if (moveEForms()) {
                    if (I.getOpcode() == Instruction::InsertElement || I.getOpcode() == Instruction::ShuffleVector) producer = true;
                    if (auto* C = dyn_cast<CallInst>(&I)) if (Function* f = C->getCalledFunction()) {
                        string n = f->getName().str();
                        if (n.rfind("llvm.genx.rdregion", 0) == 0) producer = true;
                        if (n.rfind("llvm.genx.wrregion", 0) == 0 && isa<ConstantInt>(C->getArgOperand(5))) producer = true;
                    }
                }
                if (!producer) continue;
                bool friendly = true;
                for (Use& U : I.uses())
                {
                    auto* user = dyn_cast<Instruction>(U.getUser());
                    if (!user) { friendly = false; break; }
                    unsigned op = user->getOpcode(), no = U.getOperandNo();
                    if (isElementwiseOpcode(op) || op == Instruction::ICmp || op == Instruction::FCmp) continue;
                    if (op == Instruction::ShuffleVector || op == Instruction::ExtractElement) continue;
                    if (moveEForms() && op == Instruction::InsertElement) continue;
                    if (auto* C = dyn_cast<CallInst>(user))
                    {
                        Function* f = C->getCalledFunction(); if (!f) { friendly = false; break; }
                        string n = f->getName().str();
                        if (isElementwiseCallName(n)) continue;
                        if (n.rfind("llvm.genx.rdregion", 0) == 0 && no == 0) continue;
                        if (n.rfind("llvm.genx.wrregion", 0) == 0 && no == 1) continue;
                        if (moveEForms() && n.rfind("llvm.genx.wrregion", 0) == 0 && no == 0 && isa<ConstantInt>(C->getArgOperand(5))) continue;
                    }
                    friendly = false; break;
                }
                if (friendly) eform.insert(&I);
            }
        if (getenv("X4R_COST")) fprintf(stderr, "EFORM %zu values kept unpacked\n", eform.size());
    }


    // ---- analysis: how often is the same activation (A) / weight (B) tile reused across dpas? (X4R_DPASSTATS=1) ----
    string operandSig(Value* v)
    {
        Lay l = layOf(v->getType()); string sig;
        for (unsigned dw = 0; dw < l.W; ++dw)
        {
            Pc p = elemPiece(v, dw * l.epd, 0);
            if (p.k == Pc::SrcD || p.k == Pc::SrcU)
                sig += (p.k == Pc::SrcD ? "D" : "U") + to_string((uintptr_t)p.v) + ":" + p.base + ":" + to_string(p.dw) + ":" + to_string(p.sh) + ":" + to_string(p.bits) + ",";
            else if (p.k == Pc::Lit) sig += "L" + to_string(p.lit) + ",";
            else if (p.k == Pc::Zero) sig += "Z,";
            else sig += "?,";
        }
        return sig;
    }
    void dpasStats()
    {
        std::map<string, int> A, B; int n = 0, chained = 0; std::map<Value*, int> users;
        for (auto& BB : *F) for (auto& I : BB) if (auto* C = dyn_cast<CallInst>(&I)) if (Function* f = C->getCalledFunction())
        {
            string fn = f->getName().str(); Value *a = nullptr, *b = nullptr, *acc = nullptr;
            if (fn.rfind("llvm.genx.dpas2", 0) == 0) { acc = C->getArgOperand(0); b = C->getArgOperand(1); a = C->getArgOperand(2); }
            else if (fn.rfind("llvm.genx.dpas.nosrc0", 0) == 0) { b = C->getArgOperand(0); a = C->getArgOperand(1); }
            else continue;
            ++n; A[operandSig(a)]++; B[operandSig(b)]++;
            if (getenv("X4R_DPASSEQ")) { static std::map<string, int> ida, idb; string sa = operandSig(a), sb = operandSig(b); if (!ida.count(sa)) ida[sa] = ida.size(); if (!idb.count(sb)) idb[sb] = idb.size(); fprintf(stderr, "SEQ A%d B%d\n", ida[sa], idb[sb]); }
            if (acc) if (auto* AI = dyn_cast<CallInst>(acc)) if (Function* g = AI->getCalledFunction()) if (g->getName().str().rfind("llvm.genx.dpas", 0) == 0 && acc->hasOneUse()) ++chained;
        }
        int maxA = 0, maxB = 0; for (auto& kv : A) maxA = std::max(maxA, kv.second); for (auto& kv : B) maxB = std::max(maxB, kv.second);
        fprintf(stderr, "DPASSTATS dpas=%d distinctA=%zu (max reuse %d) distinctB=%zu (max reuse %d) chained-acc=%d\n", n, A.size(), maxA, B.size(), maxB, chained);
    }


    // ---- analysis: conditional branches around dpas whose condition depends on the CM thread's x/linear id (the two threads of a wave could diverge) ----
    std::map<Value*, int> depMemo;
    int idDep(Value* v)   // bit0: linear id, bit1: x, bit2: y, bit3: z
    {
        auto it = depMemo.find(v); if (it != depMemo.end()) return it->second;
        depMemo[v] = 0; int r = 0;
        if (auto* I = dyn_cast<Instruction>(v))
        {
            if (auto* C = dyn_cast<CallInst>(I)) if (Function* f = C->getCalledFunction())
            {
                string n = f->getName().str();
                if (n == "_Z12get_local_idj") { auto* ci = dyn_cast<ConstantInt>(C->getArgOperand(0)); r |= 1 << (1 + (ci ? ci->getZExtValue() : 0)); }
                if (n == "_Z19get_local_linear_idv" || n == "_Z20get_global_linear_idv") r |= 1;
            }
            for (auto& op : I->operands()) r |= idDep(op.get());
        }
        depMemo[v] = r; return r;
    }
    static bool isDpasCall(Instruction& I)
    {
        if (auto* C = dyn_cast<CallInst>(&I)) if (Function* f = C->getCalledFunction()) { string n = f->getName().str(); return n.rfind("llvm.genx.dpas", 0) == 0; }
        return false;
    }
    void divergenceStats()
    {
        DominatorTree DT(*F); int total = 0, risky = 0;
        std::vector<BasicBlock*> dpasBlocks;
        for (auto& BB : *F) for (auto& I : BB) if (isDpasCall(I)) { dpasBlocks.push_back(&BB); break; }
        for (auto& BB : *F)
        {
            auto* br = dyn_cast<BranchInst>(BB.getTerminator()); if (!br || br->isUnconditional()) continue;
            bool region = false;
            for (auto* D : dpasBlocks) if (wPDT && controlDependent(&BB, D)) region = true;
            if (!region) continue;
            ++total;
            if (idDep(br->getCondition()) & ~8) { ++risky; if (getenv("X4R_DPASSTATS")) { string t; raw_string_ostream o(t); br->getCondition()->print(o); fprintf(stderr, "  RISKY cond: %s\n", t.substr(0, 110).c_str()); } }
        }
        fprintf(stderr, "DIVERGENCE branches enclosing dpas=%d, depending on thread x/y/linear id=%d\n", total, risky);
    }


    // ---- matrix-unit dpas: eligibility (both CM threads of a wave must execute it together, and share the weights) ----
    std::unique_ptr<DominatorTree> wDT;
    std::unique_ptr<PostDominatorTree> wPDT;
    // block B is control dependent on the branch in block X if it runs under some outcomes of that branch only (B does not post-dominate X, but post-dominates a successor)
    bool controlDependent(BasicBlock* X, BasicBlock* B)
    {
        if (X == B) return false;
        if (wPDT->dominates(B, X)) return false;
        for (BasicBlock* S : successors(X)) if (B == S || wPDT->dominates(B, S)) return true;
        return false;
    }
    std::vector<BranchInst*> divergentBranches;
    bool wmmaUsed = false; bool dbgDeclared = false;
    struct OpTile { int id = 0; BasicBlock* bb = nullptr; int last = -100; };
    std::map<string, OpTile> aTiles, bTiles; int dseq = 0;
    unsigned wmmaWaves = 1;
    unsigned wmmaAStride() {
        const char* option = getenv("X4R_WMMA_ASTRIDE");
        unsigned stride = option ? (unsigned)atoi(option) : 8u;
        if (stride < 8u || stride > 32u) die("WMMA A stride must be 8..32 dwords");
        return stride;
    }
    unsigned wmmaBStride() {
        const char* option = getenv("X4R_WMMA_BSTRIDE");
        unsigned stride = option ? atoi(option) : 8u;
        if (stride < 8u || stride > 32u) die("WMMA B stride must be 8..32 dwords");
        return stride;
    }
    void prepareWmma()
    {
        wDT.reset(new DominatorTree(*F));
        wPDT.reset(new PostDominatorTree(*F));
        for (auto& BB : *F)
            if (auto* br = dyn_cast<BranchInst>(BB.getTerminator())) if (br->isConditional() && (idDep(br->getCondition()) & ~8)) divergentBranches.push_back(br);
        wmmaWaves = splitZ() ? 1u : std::max(1u, LX * LY * LZ / 2);
    }
    // X4R_SPLITZ: a (2,1,Z) group runs as Z one-wave groups (the shim multiplies the dispatch's group z by Z); only without SLM/barriers
    bool splitZ() const { return getenv("X4R_SPLITZ") && LX * LY == 2 && LZ > 1 && !slmBytes; }
    bool wmmaEligible(CallInst& C, Value* B, unsigned rep)
    {
        if (!getenv("X4R_WMMA")) return false;
        if (!wchainOf.count(&C) && !getenv("X4R_WMMA_SINGLE")) return false;
        if (rep != 8 && rep != 4) return false;
        if ((LX * LY * LZ) % 2 != 0) return false;
        if (idDep(B) & ~8) return false;                         // weights must be identical for the two threads of a wave
        BasicBlock* BB = C.getParent();
        for (auto* br : divergentBranches)
            if (controlDependent(br->getParent(), BB)) return false;
        return true;
    }
    // Fuse constant region/shuffle routing into the LDS tile address. Each
    // source lane writes its word directly, avoiding a register transpose.
    bool stageWmmaA(Value* A, unsigned words)
    {
        if (!getenv("X4R_WMMA_SCATTERA")) return false;
        struct Store { string source; unsigned slot; uint32_t members = 0; std::vector<unsigned> target = std::vector<unsigned>(16); };
        std::vector<Store> stores;
        int saved = fwdOverride; fwdOverride = 3;
        bool changed = false;
        for (unsigned d = 0; d < words; ++d) {
            Pc p;
            if (!bitPiece(A,d*32,32,p,0) || p.k != Pc::SrcD || p.bits != 32 || p.sh) { fwdOverride = saved; return false; }
            string source = srcName(p);
            unsigned lane = p.dw % 16, s = p.dw / 16;
            changed |= p.v != A || p.dw != d;
            Store* group = nullptr;
            for (auto& g : stores) if (g.source == source && g.slot == s && !(g.members & (1u << lane))) { group = &g; break; }
            if (!group) { stores.push_back(Store()); group = &stores.back(); group->source = source; group->slot = s; }
            group->members |= 1u << lane; group->target[lane] = d;
        }
        fwdOverride = saved;
        // Avoid replacing a cheap staging operation with many masked stores.
        if (!changed || stores.size() > (words+15)/16) return false;
        std::vector<string> addresses;
        for (auto& g : stores) {
            string address = laneBitExpr(g.target,7,g.members);
            if (address.empty()) return false;
            addresses.push_back(address);
        }
        for (unsigned i = 0; i < stores.size(); ++i) {
            auto& g = stores[i];
            string d = newTmp(1);
            emit(slot(d,0) + " = " + addresses[i] + ";");
            string store = "ldsA[wvA" + curAOff + " + (2u*(" + slot(d,0) + ">>3u)+tt)*" + to_string(wmmaAStride()) + "u + (" + slot(d,0) + "&7u)] = " + slot(g.source,g.slot) + ";";
            if (g.members != 65535u) store = "if ((" + hex32(g.members) + " >> lane) & 1u) { " + store + " }";
            emit(store);
        }
        if (getenv("X4R_COST")) fprintf(stderr,"WMMA_SCATTERA %u words, %zu stores\n",words,stores.size());
        return true;
    }
    // ---- batched WMMA emission ----
    // A batch is up to X4R_WMMA_BATCH independent dpas calls of one basic block (no member consumes another's result, nothing between
    // them consumes a member's result before the last one). They are deferred to the last member's position and emitted together: every
    // operand tile is staged into its own LDS region, one barrier, all loads, then all multiplies (independent accumulators in flight
    // per wave: a lone dependent pair of WMMAs leaves the matrix unit mostly idle), then all extractions.
    struct WPending { CallInst* C; Value* acc; Value* B; Value* A; unsigned precB, precA, rep; };
    std::vector<WPending> pendingW;
    std::map<CallInst*, std::pair<int, unsigned>> wgroupOf;    // call -> (group id, group size)
    string curAOff, curBOff;                                    // LDS region offsets while staging (empty: region 0)
    unsigned wmmaBatchSize() { static int n = -1; if (n < 0) { const char* o = getenv("X4R_WMMA_BATCH"); n = o ? atoi(o) : 1; if (n < 1) n = 1; if (n > 8) n = 8; } return (unsigned)n; }
    bool dpasArgs(CallInst& C, Value*& acc, Value*& B, Value*& A, unsigned& precB, unsigned& precA, unsigned& rep)
    {
        Function* f = C.getCalledFunction(); if (!f) return false;
        string n = f->getName().str();
        if (n.rfind("llvm.genx.dpas2", 0) == 0) { acc = C.getArgOperand(0); B = C.getArgOperand(1); A = C.getArgOperand(2); precB = cint(C.getArgOperand(3)); precA = cint(C.getArgOperand(4)); rep = cint(C.getArgOperand(6)); return true; }
        if (n.rfind("llvm.genx.dpas.nosrc0", 0) == 0) { uint32_t info = (uint32_t)cint(C.getArgOperand(2)); acc = nullptr; B = C.getArgOperand(0); A = C.getArgOperand(1); precB = info & 0xff; precA = (info >> 8) & 0xff; rep = (info >> 24) & 0xff; return true; }
        return false;
    }
    void buildWmmaGroups()
    {
        wgroupOf.clear();
        unsigned maxN = wmmaBatchSize();
        if (maxN < 2 || trace || !getenv("X4R_WMMA")) return;
        int gid = 0;
        for (auto& BB : *F)
        {
            std::vector<CallInst*> cur; std::set<Value*> taint;
            auto flush = [&]() {
                if (cur.size() >= 2) { for (auto* m : cur) wgroupOf[m] = {gid, (unsigned)cur.size()}; ++gid; }
                cur.clear(); taint.clear();
            };
            for (auto& I : BB)
            {
                if (isa<PHINode>(I)) continue;
                if (I.isTerminator()) break;
                auto* C = dyn_cast<CallInst>(&I);
                Value *acc = nullptr, *B = nullptr, *A = nullptr; unsigned pb = 0, pa = 0, rep = 0;
                bool member = C && dpasArgs(*C, acc, B, A, pb, pa, rep) && wmmaEligible(*C, B, rep);
                bool usesTaint = false;
                for (auto& op : I.operands()) if (taint.count(op.get())) usesTaint = true;
                if (member)
                {
                    if (usesTaint || cur.size() >= maxN) { flush(); }
                    cur.push_back(C); taint.insert(C);
                    continue;
                }
                if (usesTaint) { flush(); continue; }
            }
            flush();
        }
    }
    void dpasWmma(CallInst& C, Value* acc, Value* B, Value* A, unsigned precB, unsigned precA, unsigned rep)
    {
        auto g = wgroupOf.find(&C);
        if (g == wgroupOf.end()) { std::vector<WPending> one{{&C, acc, B, A, precB, precA, rep}}; dpasWmmaGroup(one); return; }
        pendingW.push_back({&C, acc, B, A, precB, precA, rep});
        if (pendingW.size() == g->second.second) { std::vector<WPending> batch; batch.swap(pendingW); dpasWmmaGroup(batch); }
    }
    void dpasWmmaGroup(std::vector<WPending>& ms)
    {
        wmmaUsed = true;
        auto isSigned = [&](unsigned p) { if (p == 8) return true; if (p == 7) return false; die("dpas: unsupported precision " + to_string(p)); };
        const unsigned R = wmmaBatchSize();
        const unsigned regionA = 16 * wmmaAStride(), regionB = 16 * wmmaBStride();
        struct Mem { WPending w; string d, cv; unsigned mask, aSlots; bool materialize; string nA, nB; bool hitA, hitB; };
        std::vector<Mem> mm;
        emit("{");
        // 1. accumulators
        unsigned cReg = 0;
        for (auto& w : ms)
        {
            CallInst& C = *w.C;
            Mem m; m.w = w;
            bool sB = isSigned(w.precB), sA = isSigned(w.precA);
            m.d = nm(&C);
            m.mask = (sA ? 1u : 0u) | (sB ? 2u : 0u) | 4u | 8u;
            m.aSlots = (w.rep * 8 + 15) / 16;
            auto it = wchainOf.find(&C);
            int chain = it == wchainOf.end() ? -1 : it->second.first, pos = it == wchainOf.end() ? 0 : it->second.second;
            bool last = chain < 0 || pos + 1 == (int)wchains[chain].m.size();
            m.materialize = last || trace;                       // intermediate chain results are only ever consumed by the next dpas
            m.cv = chain < 0 ? "ws" + to_string(ntmp++) : "wc" + to_string(chain);
            if (chain < 0 || pos == 0) { decls << "  X4RMatC " << m.cv << ";\n"; }
            if (chain < 0 || pos == 0)
            {
                if (getenv("X4R_WMMA_REGC")) {
                    // RADV gfx11 wave32 accumulator: row=2*component+tt, col=lane.
                    // This is exactly the CM output layout; keep C in registers.
                    emit("  " + m.cv + " = cmSplatC(0);");
                    if (w.acc) for (unsigned r = 0; r < w.rep; ++r)
                        emit("  " + m.cv + " = cmInsertC(asint(" + dataSlot(w.acc, r) + "), " + m.cv + ", " + to_string(r) + "u);");
                } else {
                    string off = ms.size() > 1 ? " + " + to_string(cReg * 256) + "u" : "";
                    for (unsigned r = 0; r < w.rep; ++r) emit("  ldsC[wvC" + off + " + " + to_string(2 * r) + "u*16u + tt*16u + lane] = " + (w.acc ? dataSlot(w.acc, r) : string("0u")) + ";");
                    emit("  GroupMemoryBarrier();");
                    emit("  " + m.cv + " = cmLoadC(ldsC[wvC" + off + "], 0u, 16u);");
                    ++cReg;
                }
            }
            mm.push_back(m);
        }
        // 2. stage the operand tiles of every member (operand reuse: the same tile often feeds a few consecutive dpas, stage and load each once)
        unsigned regA = 0, regB = 0;
        std::vector<std::pair<string, unsigned>> loadsA, loadsB;      // (tile name, region) to load after the barrier
        for (auto& m : mm)
        {
            CallInst& C = *m.w.C; Value *A = m.w.A, *B = m.w.B;
            int saved = fwdOverride; fwdOverride = 3;
            string sigA = operandSig(A), sigB = operandSig(B);
            fwdOverride = saved;
            BasicBlock* BBc = C.getParent();
            auto lookup = [&](std::map<string, OpTile>& cache, const string& sig, const char* kind) -> std::pair<OpTile*, bool>
            {
                auto it = cache.find(sig);
                const char* option = getenv("X4R_WMMA_CACHE");
                int distance = option ? atoi(option) : 6;
                bool hit = distance > 0 && it != cache.end() && it->second.bb == BBc && dseq - it->second.last <= distance;
                if (it == cache.end())
                {
                    OpTile t; t.id = (int)cache.size(); t.bb = BBc; t.last = dseq;
                    it = cache.emplace(sig, t).first;
                    decls << "  X4RMat" << kind << " w" << kind << (kind[0] == 'A' ? "a" : "b") << it->second.id << "_0, w" << kind << (kind[0] == 'A' ? "a" : "b") << it->second.id << "_1;\n";
                }
                it->second.bb = BBc; if (!hit) it->second.last = dseq; else it->second.last = dseq;
                return {&it->second, hit};
            };
            auto la = lookup(aTiles, sigA, "A"), lb = lookup(bTiles, sigB, "B");
            m.nA = "wAa" + to_string(la.first->id); m.nB = "wBb" + to_string(lb.first->id);
            m.hitA = la.second; m.hitB = lb.second;
            ++dseq;
            if (!m.hitA) {
                curAOff = R > 1 && regA ? " + " + to_string(regA * regionA) + "u" : "";
                if (getenv("X4R_WMMA_REGA")) {
                    // Transpose the 4 VGPR slot bits with the low 2 lane bits.
                    // Then a single wave shuffle per packed word selects the
                    // interleaved row and duplicates it across the wave halves.
                    string raw = newTmp(4), first = newTmp(4), second = newTmp(4);
                    for (unsigned s = 0; s < 4; ++s)
                        emit("  " + slot(raw,s) + " = " + (s < m.aSlots ? dataSlot(A,s) : string("0u")) + ";");
                    for (unsigned s = 0; s < 4; ++s) {
                        string shuffled = newTmp(1);
                        emit("  " + slot(shuffled,0) + " = WaveReadLaneAt(" + slot(raw,s^1u) + ", WaveGetLaneIndex() ^ 1u);");
                        emit("  " + slot(first,s) + " = ((lane & 1u) == " + to_string(s&1u) + "u) ? " + slot(raw,s) + " : " + slot(shuffled,0) + ";");
                    }
                    for (unsigned s = 0; s < 4; ++s) {
                        string shuffled = newTmp(1);
                        emit("  " + slot(shuffled,0) + " = WaveReadLaneAt(" + slot(first,s^2u) + ", WaveGetLaneIndex() ^ 2u);");
                        emit("  " + slot(second,s) + " = ((lane & 2u) == " + to_string(s&2u) + "u) ? " + slot(first,s) + " : " + slot(shuffled,0) + ";");
                    }
                    for (unsigned half = 0; half < 2; ++half) {
                        string matrix = m.nA + "_" + to_string(half), packed = newTmp(4);
                        emit("  " + matrix + " = cmSplatA(cmByte(0u));");
                        for (unsigned q = 0; q < 4; ++q)
                            emit("  " + slot(packed,q) + " = WaveReadLaneAt(" + slot(second,q) + ", (lane >> 2u) | ((lane & 2u) << 2u) | ((lane & 1u) << 4u) | " + to_string(half*4) + "u);");
                        for (unsigned e = 0; e < 16; ++e)
                            emit("  " + matrix + " = cmInsertA(cmByte(" + slot(packed,e/4) + " >> " + to_string((e%4)*8) + "u), " + matrix + ", " + to_string(e) + "u);");
                    }
                } else {
                    if (!stageWmmaA(A, m.aSlots*16)) {
                        for (unsigned s = 0; s < m.aSlots; ++s)
                            emit("  ldsA[wvA" + curAOff + " + (2u*((" + to_string(16 * s) + "u+lane)>>3)+tt)*" + to_string(wmmaAStride()) + "u + ((" + to_string(16 * s) + "u+lane)&7u)] = " + dataSlot(A, s) + ";");
                    }
                    loadsA.push_back({m.nA, regA});
                    ++regA;
                }
            }
            if (!m.hitB)
            {
                curBOff = R > 1 && regB ? " + " + to_string(regB * regionB) + "u" : "";
                if (getenv("X4R_WMMA_REGB")) {
                    // gfx11 B fragments contain a column's 16 bytes per lane;
                    // the identical CM weights already live in both wave halves.
                    for (unsigned half = 0; half < 2; ++half) {
                        string matrix = m.nB + "_" + to_string(half);
                        emit("  " + matrix + " = cmSplatB(cmByte(0u));");
                        for (unsigned e = 0; e < 16; ++e)
                            emit("  " + matrix + " = cmInsertB(cmByte(" + dataSlot(B, half * 4 + e / 4) + " >> " + to_string((e % 4) * 8) + "u), " + matrix + ", " + to_string(e) + "u);");
                    }
                }
                else {
                    if (getenv("X4R_WMMA_FULLB")) { for (unsigned k = 0; k < 8; ++k) emit("  ldsB[wvB" + curBOff + " + lane*" + to_string(wmmaBStride()) + "u + " + to_string(k) + "u] = " + dataSlot(B, k) + ";"); }
                    else for (unsigned q = 0; q < 4; ++q) emit("  ldsB[wvB" + curBOff + " + lane*" + to_string(wmmaBStride()) + "u + tt*4u + " + to_string(q) + "u] = (tt != 0u) ? " + dataSlot(B, 4 + q) + " : " + dataSlot(B, q) + ";");
                    loadsB.push_back({m.nB, regB});
                    ++regB;
                }
            }
        }
        curAOff.clear(); curBOff.clear();
        // 3. one barrier, then every load
        if (!loadsA.empty() || !loadsB.empty()) emit("  GroupMemoryBarrier();");
        for (auto& t : loadsA)
        {
            string off = R > 1 && t.second ? " + " + to_string(t.second * regionA) + "u" : "";
            emit("  " + t.first + "_0 = cmLoadA(ldsA[wvA" + off + "], 0u, " + to_string(wmmaAStride()) + "u); " + t.first + "_1 = cmLoadA(ldsA[wvA" + off + " + 4u], 0u, " + to_string(wmmaAStride()) + "u);");
        }
        for (auto& t : loadsB)
        {
            string off = R > 1 && t.second ? " + " + to_string(t.second * regionB) + "u" : "";
            emit("  " + t.first + "_0 = cmLoadB(ldsB[wvB" + off + "], 1u, " + to_string(wmmaBStride()) + "u); " + t.first + "_1 = cmLoadB(ldsB[wvB" + off + " + 4u], 1u, " + to_string(wmmaBStride()) + "u);");
        }
        // 4. the multiplies
        for (auto& m : mm)
            if (!getenv("X4R_WMMA_SKIPMUL"))   // profiling aid: keep all the data movement, drop the matrix instruction (output is wrong)
                emit("  " + m.cv + " = cmMulAdd(" + m.nA + "_0, " + m.nB + "_0, " + m.cv + ", " + to_string(m.mask) + "); " + m.cv + " = cmMulAdd(" + m.nA + "_1, " + m.nB + "_1, " + m.cv + ", " + to_string(m.mask) + ");");
        // 5. results
        unsigned cOut = 0;
        for (auto& m : mm)
        {
            if (!m.materialize) continue;
            if (getenv("X4R_WMMA_REGC")) {
                for (unsigned r = 0; r < m.w.rep; ++r)
                    emit("  " + slot(m.d, r) + " = asuint(cmExtractC(" + m.cv + ", " + to_string(r) + "u));");
            } else {
                string off = mm.size() > 1 ? " + " + to_string(cOut * 256) + "u" : "";
                emit("  cmStoreC(ldsC[wvC" + off + "], " + m.cv + ", 0u, 16u);");
                emit("  GroupMemoryBarrier();");
                for (unsigned r = 0; r < m.w.rep; ++r) emit("  " + slot(m.d, r) + " = ldsC[wvC" + off + " + " + to_string(2 * r) + "u*16u + tt*16u + lane];");
                ++cOut;
            }
        }
        emit("}");
    }

    // ---- analysis: accumulator chains between dpas calls (X4R_DPASCHAINS=1) ----
    void dpasChains()
    {
        std::map<Value*, int> idx; std::vector<CallInst*> list;
        for (auto& BB : *F) for (auto& I : BB) if (auto* C = dyn_cast<CallInst>(&I)) if (Function* f = C->getCalledFunction()) if (f->getName().str().rfind("llvm.genx.dpas2", 0) == 0) { idx[C] = list.size(); list.push_back(C); }
        std::vector<int> prev(list.size(), -1), next(list.size(), -1); int fromOther = 0;
        for (size_t i = 0; i < list.size(); ++i)
        {
            Value* acc = list[i]->getArgOperand(0);
            Pc p = elemPiece(acc, 0, 0); bool whole = true;
            Value* src = p.k == Pc::SrcD ? p.v : nullptr; unsigned dw0 = p.dw;
            for (unsigned e = 1; e < 128 && src; ++e) { Pc q = elemPiece(acc, e, 0); if (q.k != Pc::SrcD || q.v != src || q.dw != dw0 + e) whole = false; }
            if (src && whole && dw0 == 0 && idx.count(src)) { prev[i] = idx[src]; if (next[idx[src]] >= 0) fromOther++; next[idx[src]] = i; }
        }
        std::vector<int> len; for (size_t i = 0; i < list.size(); ++i) if (prev[i] < 0) { int n = 1; for (int j = next[i]; j >= 0; j = next[j]) ++n; len.push_back(n); }
        std::map<int, int> h; for (int n : len) h[n]++;
        string t; for (auto& kv : h) t += to_string(kv.second) + "x" + to_string(kv.first) + " ";
        fprintf(stderr, "DPASCHAINS dpas2=%zu chains=%zu lengths: %s(forks=%d)\n", list.size(), len.size(), t.c_str(), fromOther);
    }


    // ---- accumulator chains: consecutive dpas2 whose accumulator is exactly the previous result stay in matrix registers ----
    struct WChain { std::vector<CallInst*> m; };
    std::vector<WChain> wchains;
    std::map<CallInst*, std::pair<int, int>> wchainOf;   // call -> (chain, position)
    void buildChains()
    {
        wchains.clear(); wchainOf.clear();
        int saved = fwdOverride; fwdOverride = 3;
        std::vector<CallInst*> list; std::map<Value*, int> idx;
        for (auto& BB : *F) for (auto& I : BB) if (auto* C = dyn_cast<CallInst>(&I)) if (Function* f = C->getCalledFunction()) { string n = f->getName().str(); if (n.rfind("llvm.genx.dpas2", 0) == 0) { idx[C] = list.size(); list.push_back(C); } }
        std::vector<int> prev(list.size(), -1), next(list.size(), -1);
        for (size_t i = 0; i < list.size(); ++i)
        {
            Value* acc = list[i]->getArgOperand(0);
            Pc p = elemPiece(acc, 0, 0); if (p.k != Pc::SrcD || !idx.count(p.v) || p.dw != 0) continue;
            bool whole = true; for (unsigned e = 1; e < 128 && whole; ++e) { Pc q = elemPiece(acc, e, 0); if (q.k != Pc::SrcD || q.v != p.v || q.dw != e) whole = false; }
            int src = idx[p.v];
            if (whole && next[src] < 0 && list[src]->getParent() == list[i]->getParent()) { prev[i] = src; next[src] = i; }
        }
        for (size_t i = 0; i < list.size(); ++i) if (prev[i] < 0 && next[i] >= 0)
        {
            WChain c; for (int j = i; j >= 0; j = next[j]) c.m.push_back(list[j]);
            int id = wchains.size(); for (size_t k = 0; k < c.m.size(); ++k) wchainOf[c.m[k]] = {id, (int)k};
            wchains.push_back(c);
        }
        fwdOverride = saved;
    }

    // ---------------- structured emission ----------------
    struct Ctx { BasicBlock* header = nullptr; BasicBlock* exit = nullptr; Loop* loop = nullptr; };
    enum EdgeKind { E_STOP, E_CONT, E_BRK, E_INLINE };
    void emitBody(BasicBlock* B)
    {
        for (auto& I : *B)
        {
            if (isa<PHINode>(I)) { nm(&I); continue; }
            if (I.isTerminator()) break;
            instr(I);
            traceStore(I);
        }
    }
    void emitEdge(BasicBlock* From, BasicBlock* To, BasicBlock* stop, const Ctx& ctx, LoopInfo& LI, PostDominatorTree& PDT)
    {
        phiCopies(From, To);
        if (To == stop) return;
        if (ctx.loop && To == ctx.header) { emit("continue;"); return; }
        if (ctx.exit && To == ctx.exit) { emit("break;"); return; }
        emitFrom(To, stop, ctx, false, LI, PDT);
    }
    void emitFrom(BasicBlock* B, BasicBlock* stop, Ctx ctx, bool firstOfLoop, LoopInfo& LI, PostDominatorTree& PDT)
    {
        while (B)
        {
            if (B == stop) return;
            if (!firstOfLoop && ctx.loop && B == ctx.header) { emit("continue;"); return; }
            if (ctx.exit && B == ctx.exit) { emit("break;"); return; }
            Loop* L = LI.getLoopFor(B);
            if (!firstOfLoop && L && L->getHeader() == B)
            {
                BasicBlock* E = L->getUniqueExitBlock();
                if (!E) die("loop with several exit blocks");
                emit("[loop] while (true) {");
                std::string saved = ind; ind += "  ";
                Ctx c2; c2.header = B; c2.exit = E; c2.loop = L;
                emitFrom(B, nullptr, c2, true, LI, PDT);
                ind = saved;
                emit("}");
                B = E;
                continue;
            }
            firstOfLoop = false;
            emitBody(B);
            Instruction* T = B->getTerminator();
            if (isa<ReturnInst>(T) || isa<UnreachableInst>(T)) { emit("return;"); return; }
            auto* br = cast<BranchInst>(T);
            auto kind = [&](BasicBlock* S) -> EdgeKind {
                if (S == stop) return E_STOP;
                if (ctx.loop && S == ctx.header) return E_CONT;
                if (ctx.exit && S == ctx.exit) return E_BRK;
                return E_INLINE;
            };
            if (br->isUnconditional())
            {
                BasicBlock* S = br->getSuccessor(0);
                phiCopies(B, S);
                if (S == stop) return;
                if (ctx.loop && S == ctx.header) { emit("continue;"); return; }
                if (ctx.exit && S == ctx.exit) { emit("break;"); return; }
                B = S; continue;
            }
            BasicBlock *Tb = br->getSuccessor(0), *Fb = br->getSuccessor(1);
            string c = opBits(br->getCondition(), 0, 0);
            BasicBlock* M = nullptr;
            bool bothInline = kind(Tb) == E_INLINE && kind(Fb) == E_INLINE;
            if (bothInline) M = PDT.findNearestCommonDominator(Tb, Fb);
            BasicBlock* armStop = bothInline ? M : stop;
            emit(string(getenv("X4R_NOBRANCH") ? "" : "[branch] ") + "if (" + c + " != 0u) {");
            { std::string saved = ind; ind += "  "; emitEdge(B, Tb, armStop, ctx, LI, PDT); ind = saved; }
            emit("} else {");
            { std::string saved = ind; ind += "  "; emitEdge(B, Fb, armStop, ctx, LI, PDT); ind = saved; }
            emit("}");
            if (!bothInline || !M) return;
            B = M;
        }
    }

    void run(Function* F_, const string& rootsig, const string& outPath)
    {
        F = F_;
        std::vector<Res> params;
        {
            std::ifstream in(rootsig);
            string line;
            while (std::getline(in, line))
            {
                std::istringstream ls(line);
                string w, kind;
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
        if ((size_t)ri + 1 != params.size()) fprintf(stderr, "x4rwave: warning: %d resources vs %zu root parameters\n", ri, params.size());

        for (auto& BB : *F) { int id = bbIds.size(); bbIds[&BB] = id; }
        computeEForms();
        buildExpTiles();
        prepareWmma();
        if (getenv("X4R_WMMA")) { buildChains(); buildWmmaGroups(); }
        if (getenv("X4R_DPASSTATS")) { dpasStats(); divergenceStats(); }
        if (getenv("X4R_DPASCHAINS")) dpasChains();
        if (trace)
        {
            unsigned ord = 0;
            for (auto& BB : *F)
                for (auto& I : BB)
                {
                    if (I.getType()->isVoidTy() || isa<PHINode>(I) || isa<BranchInst>(I)) continue;
                    unsigned o = ord++;
                    if (o < trLo || o >= trHi) continue;
                    Lay l = layOf(I.getType());
                    tOff[&I] = tTotal; tTotal += l.U ? (l.k == I64 ? 2 : 1) : l.W;
                }
        }
        // ---- structured control flow: real if/else and while(true) loops, so the compiler sees the true liveness ----
        // (a while(true){switch(bb)} dispatcher keeps every value live to the end of the kernel and made the shaders spill thousands of registers)
        DominatorTree DT(*F); PostDominatorTree PDT(*F); LoopInfo LI(DT);
        body.str(""); body.clear();
        ind = "  ";
        if (getenv("X4R_DISPATCH"))
        {
            // old-style dispatcher (kept for A/B comparison): while(true){switch(bb){...}}
            std::ostringstream cases;
            for (auto& BB : *F)
            {
                body.str(""); body.clear(); ind = "        ";
                emitBody(&BB);
                auto* br = dyn_cast<BranchInst>(BB.getTerminator());
                if (!br) emit("return;");
                else if (br->isUnconditional()) { phiCopies(&BB, br->getSuccessor(0)); emit("bb = " + to_string(bbIds[br->getSuccessor(0)]) + "u;"); }
                else
                {
                    emit("if (" + opBits(br->getCondition(), 0, 0) + " != 0u) {");
                    phiCopies(&BB, br->getSuccessor(0)); emit("  bb = " + to_string(bbIds[br->getSuccessor(0)]) + "u;");
                    emit("} else {");
                    phiCopies(&BB, br->getSuccessor(1)); emit("  bb = " + to_string(bbIds[br->getSuccessor(1)]) + "u;");
                    emit("}");
                }
                cases << "      case " << bbIds[&BB] << ": {\n" << body.str() << "        break;\n      }\n";
            }
            body.str(""); body.clear();
            body << "  uint bb = 0u;\n  [loop] while (true) {\n    switch (bb) {\n" << cases.str() << "      default: return;\n    }\n  }\n";
        }
        else
            emitFrom(&F->getEntryBlock(), nullptr, Ctx(), false, LI, PDT);

        std::ostringstream out;
        out << "// generated by x4rwave from CM kernel " << F->getName().str() << "\n";
        out << "int x4r_sx(uint x, uint bits) { return int(x << (32u - bits)) >> (32u - bits); }\n";
        out << "float x4r_rcp(float x) { return 1.0 / x; }\n";
        out << "uint x4r_f2h(float f) { return uint(asuint16((float16_t)f)); }   // round-to-nearest-even like the CM hardware (f32tof16 is round-to-zero)\n";
        out << "uint x4r_tb(uint lo, uint hi, uint l) { return ((l < 8u ? lo : hi) >> ((l & 7u) * 4u)) & 15u; }\n";
        out << (getenv("X4R_FAKE_SH") ? "uint x4r_sh(uint x, uint srcLane) { return x; }\n" : "uint x4r_sh(uint x, uint srcLane) { return WaveReadLaneAt(x, (WaveGetLaneIndex() & ~15u) | srcLane); }\n");
        out << (getenv("X4R_FAKE_RL") ? (string(getenv("X4R_FAKE_RL")) == "2" ? "uint x4r_rl(uint x, uint c) { return WaveReadLaneAt(x, c); }\n" : "uint x4r_rl(uint x, uint c) { return x; }\n") : "uint x4r_rl(uint x, uint c) { return WaveReadLaneAt(x, (WaveGetLaneIndex() & ~15u) | c); }\n");
        out << "bool x4r_inrange(uint e, uint off, uint n) { return e >= off && e < off + n; }\n";
        out << "uint x4r_clearrange(uint cur, uint e, uint off, uint n) { return x4r_inrange(e, off, n) ? 0u : cur; }\n";
        for (size_t s = 0; s < res.size(); ++s)
        {
            const Res& r = res[s];
            string si = to_string(s), rg = to_string(r.reg);
            if (r.kind == "SRV") out << (r.image ? "Texture2D<float4> R" : "ByteAddressBuffer R") << si << " : register(t" << rg << ");\n";
            else if (r.kind == "UAV") out << (r.image ? "RWTexture2D<float4> R" : "RWByteAddressBuffer R") << si << " : register(u" << rg << ");\n";
            else if (r.kind == "CBV") out << "cbuffer CB" << si << " : register(b" << rg << ") { uint4 CBD" << si << "[4096]; };\n";
            else die("unsupported resource kind for arg " + si);
        }
        if (mixedDotUsed) {
            out << "[[vk::ext_extension(\"SPV_KHR_integer_dot_product\")]] [[vk::ext_capability(6018)]] [[vk::ext_capability(6019)]] [[vk::ext_instruction(4452)]] int x4r_sudot(uint signedBytes, uint unsignedBytes, [[vk::ext_literal]] uint format);\n";
        }
        if (wmmaUsed)
        {
            out << "#include \"vk/spirv.h\"\n";
            out << "typedef vk::SpirvOpaqueType<21, vk::Literal<vk::integral_constant<uint, 8> >, vk::Literal<vk::integral_constant<uint, 0> > > X4RI8;\n";
            out << "typedef vk::SpirvOpaqueType<4456, X4RI8, vk::integral_constant<uint, 3>, vk::integral_constant<uint, 16>, vk::integral_constant<uint, 16>, vk::integral_constant<uint, 0> > X4RMatA;\n";
            out << "typedef vk::SpirvOpaqueType<4456, X4RI8, vk::integral_constant<uint, 3>, vk::integral_constant<uint, 16>, vk::integral_constant<uint, 16>, vk::integral_constant<uint, 1> > X4RMatB;\n";
            out << "typedef vk::SpirvOpaqueType<4456, int, vk::integral_constant<uint, 3>, vk::integral_constant<uint, 16>, vk::integral_constant<uint, 16>, vk::integral_constant<uint, 2> > X4RMatC;\n";
            const char* cap = "[[vk::ext_extension(\"SPV_KHR_cooperative_matrix\")]] [[vk::ext_capability(6022)]] [[vk::ext_capability(39)]] ";
            out << cap << "[[vk::ext_instruction(4457)]] X4RMatA cmLoadA([[vk::ext_reference]] uint p, uint layout, uint stride);\n";
            out << cap << "[[vk::ext_instruction(4457)]] X4RMatB cmLoadB([[vk::ext_reference]] uint p, uint layout, uint stride);\n";
            out << cap << "[[vk::ext_instruction(4457)]] X4RMatC cmLoadC([[vk::ext_reference]] uint p, uint layout, uint stride);\n";
            out << cap << "[[vk::ext_instruction(4458)]] void cmStoreC([[vk::ext_reference]] uint p, X4RMatC m, uint layout, uint stride);\n";
            out << cap << "[[vk::ext_instruction(4459)]] X4RMatC cmMulAdd(X4RMatA a, X4RMatB b, X4RMatC c, [[vk::ext_literal]] uint operands);\n";
            if (getenv("X4R_WMMA_REGC")) {
                out << cap << "[[vk::ext_instruction(80)]] X4RMatC cmSplatC(int value);\n";
                out << cap << "[[vk::ext_instruction(82)]] X4RMatC cmInsertC(int value, X4RMatC m, [[vk::ext_literal]] uint index);\n";
                out << cap << "[[vk::ext_instruction(81)]] int cmExtractC(X4RMatC m, [[vk::ext_literal]] uint index);\n";
            }
            if (getenv("X4R_WMMA_REGB") || getenv("X4R_WMMA_REGA")) {
                out << cap << "[[vk::ext_instruction(113)]] X4RI8 cmByte(uint value);\n";
            }
            if (getenv("X4R_WMMA_REGB")) {
                out << cap << "[[vk::ext_instruction(80)]] X4RMatB cmSplatB(X4RI8 value);\n";
                out << cap << "[[vk::ext_instruction(82)]] X4RMatB cmInsertB(X4RI8 value, X4RMatB m, [[vk::ext_literal]] uint index);\n";
            }
            if (getenv("X4R_WMMA_REGA")) {
                out << cap << "[[vk::ext_instruction(80)]] X4RMatA cmSplatA(X4RI8 value);\n";
                out << cap << "[[vk::ext_instruction(82)]] X4RMatA cmInsertA(X4RI8 value, X4RMatA m, [[vk::ext_literal]] uint index);\n";
            }
            out << "groupshared uint ldsA[" << 16 * wmmaAStride() * wmmaWaves * wmmaBatchSize() << "];\ngroupshared uint ldsB[" << 16 * wmmaBStride() * wmmaWaves * wmmaBatchSize() << "];\ngroupshared uint ldsC[" << 256 * wmmaWaves * wmmaBatchSize() << "];\n";
        }
        if (usesSampler) out << "SamplerState smp0 : register(s0);\n";
        if (trace) out << "RWByteAddressBuffer DBG : register(u31);\n";
        if (slmBytes) out << "groupshared uint slm[" << slmBytes / 4 << "];\n";
        out << globals.str();
        unsigned threads = LX * LY * LZ;
        bool split = splitZ();
        out << "[WaveSize(32)]\n[numthreads(" << 16 * (split ? 2 : threads) << ",1,1)]\n";
        out << "void main(uint3 gidIn : SV_GroupID, uint gidx : SV_GroupIndex) {\n";
        if (split) out << "  uint3 gid = uint3(gidIn.xy, gidIn.z / " << LZ << "u);\n  uint lane = gidx & 15u;\n  uint cmt = (gidx >> 4) + 2u * (gidIn.z % " << LZ << "u);\n  uint gtid = cmt;\n";
        else out << "  uint3 gid = gidIn;\n  uint lane = gidx & 15u;\n  uint cmt = gidx >> 4;\n  uint gtid = cmt;\n";
        if (wmmaUsed) out << "  uint tt = cmt & 1u;\n  uint wvA = (gidx >> 5) * " << 16 * wmmaAStride() * wmmaBatchSize() << "u;\n  uint wvB = (gidx >> 5) * " << 16 * wmmaBStride() * wmmaBatchSize() << "u;\n  uint wvC = (gidx >> 5) * " << 256 * wmmaBatchSize() << "u;\n";
        out << "  uint3 ltid = uint3(cmt % " << LX << "u, (cmt / " << LX << "u) % " << LY << "u, cmt / " << LX * LY << "u);\n";
        out << "  uint3 lsz = uint3(" << LX << "u," << LY << "u," << LZ << "u);\n  uint ngx = 1u, ngy = 1u;\n";
        out << decls.str();
        out << body.str() << "}\n";
        if (getenv("X4R_COST"))
        {
            std::vector<std::pair<double, std::string>> v; double tot = 0;
            for (auto& kv : costBy) { v.push_back({kv.second, kv.first}); tot += kv.second; }
            std::sort(v.rbegin(), v.rend());
            fprintf(stderr, "COST total %.0f\n", tot);
            for (size_t i = 0; i < v.size() && i < 16; ++i) fprintf(stderr, "COST %6.0f %4.1f%% x%-4d %s\n", v[i].first, 100 * v[i].first / tot, cntBy[v[i].second], v[i].second.c_str());
        }
        std::ofstream f(outPath);
        f << out.str();
        {
            std::ofstream lf(outPath + ".layout");
            for (size_t i = 0; i < res.size(); ++i)
            {
                const Res& r = res[i];
                unsigned b = r.kind == "SRV" ? r.reg : r.kind == "UAV" ? 16 + r.reg : 32 + r.reg;
                lf << b << " " << (r.kind == "CBV" ? "ubo" : r.image ? (r.kind == "SRV" ? "tex" : "rwtex") : "sbuf") << "\n";
            }
            if (usesSampler) lf << "52 sampler\n";
        }
        if (trace)
        {
            std::ofstream tf(outPath + ".trace");
            tf << tTotal << "\n";
            for (auto& BB : *F) for (auto& I : BB) if (tOff.count(&I))
            {
                Lay l = layOf(I.getType());
                string t = str(&I); for (auto& c : t) if (c == '\n') c = ' ';
                tf << tOff[&I] << " " << (l.U ? (l.k == I64 ? 2 : 1) : l.W) << " " << (l.U ? "U" : "D") << (isF(l.k) && l.k == F32 ? "F" : "I") << " " << t << "\n";
            }
        }
    }
};

int main(int argc, char** argv)
{
    if (argc < 7) { fprintf(stderr, "usage: x4rwave kernel.ll rootsig.txt LX LY LZ out.hlsl\n"); return 1; }
    LLVMContext ctx;
    SMDiagnostic err;
    auto M = parseIRFile(argv[1], err, ctx);
    if (!M) { err.print("x4rwave", errs()); return 1; }
    Function* K = nullptr;
    for (auto& F : *M) if (F.getCallingConv() == CallingConv::SPIR_KERNEL) K = &F;
    if (!K) die("no kernel in module");
    Tx t;
    t.LX = atoi(argv[3]); t.LY = atoi(argv[4]); t.LZ = atoi(argv[5]);
    if (K->hasFnAttribute("VCSLMSize")) t.slmBytes = atoi(K->getFnAttribute("VCSLMSize").getValueAsString().str().c_str());
    t.trace = argc > 7 && string(argv[7]) == "trace";
    if (argc > 9) { t.trLo = atoi(argv[8]); t.trHi = atoi(argv[9]); }
    t.run(K, argv[2], argv[6]);
    return 0;
}
