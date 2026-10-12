// SPDX-License-Identifier: GPL-3.0-only
// x4rpress: register pressure of a lifted CM kernel in VGPR units (one GRF = 16 dwords = one VGPR per CM thread), by liveness over the CFG.
#include "llvm/IR/Constants.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"
#include "llvm/IRReader/IRReader.h"
#include "llvm/Support/SourceMgr.h"
#include "llvm/Support/raw_ostream.h"
#include <map>
#include <set>
#include <vector>
using namespace llvm;
static unsigned weight(Type* t)
{
    if (t->isVoidTy() || t->isTargetExtTy()) return 0;
    if (!isa<FixedVectorType>(t)) return 1;
    auto* v = cast<FixedVectorType>(t); Type* e = v->getElementType(); unsigned n = v->getNumElements(), w;
    if (e->isIntegerTy(1)) w = n; else if (e->isIntegerTy(8)) w = (n + 3) / 4; else if (e->isIntegerTy(16) || e->isHalfTy()) w = (n + 1) / 2; else if (e->isIntegerTy(64)) w = 2 * n; else w = n;
    return (w + 15) / 16;
}
int main(int argc, char** argv)
{
    LLVMContext ctx; SMDiagnostic err; auto M = parseIRFile(argv[1], err, ctx);
    Function* F = nullptr; for (auto& f : *M) if (f.getCallingConv() == CallingConv::SPIR_KERNEL) F = &f;
    // per-block use/def and live-in/out via iterative dataflow
    std::map<BasicBlock*, std::set<Instruction*>> in, out;
    bool ch = true;
    auto isVal = [](Value* v) { return isa<Instruction>(v) && weight(v->getType()) > 0; };
    while (ch)
    {
        ch = false;
        std::vector<BasicBlock*> order; for (auto& b : *F) order.push_back(&b);
        for (auto it = order.rbegin(); it != order.rend(); ++it)
        {
            BasicBlock* B = *it; std::set<Instruction*> o;
            for (auto* S : successors(B))
            {
                for (auto* x : in[S]) o.insert(x);
                for (auto& I : *S) { auto* p = dyn_cast<PHINode>(&I); if (!p) break; o.erase(p); if (auto* v = dyn_cast<Instruction>(p->getIncomingValueForBlock(B))) if (isVal(v)) o.insert(v); }
            }
            std::set<Instruction*> live = o;
            for (auto i = B->rbegin(); i != B->rend(); ++i)
            {
                Instruction* I = &*i; live.erase(I);
                if (isa<PHINode>(I)) continue;
                for (auto& op : I->operands()) if (auto* d = dyn_cast<Instruction>(op.get())) if (isVal(d)) live.insert(d);
            }
            // phis defined here are live-in
            std::set<Instruction*> li = live;
            for (auto& I : *B) { if (isa<PHINode>(I)) li.insert(&I); else break; }
            if (o != out[B]) { out[B] = o; ch = true; }
            if (li != in[B]) { in[B] = li; ch = true; }
        }
    }
    unsigned maxP = 0; std::string where;
    for (auto& B : *F)
    {
        std::set<Instruction*> live = out[&B];
        for (auto i = B.rbegin(); i != B.rend(); ++i)
        {
            Instruction* I = &*i;
            unsigned p = 0; for (auto* x : live) p += weight(x->getType());
            if (!isa<PHINode>(I)) p += isVal(I) ? weight(I->getType()) : 0;
            if (p > maxP) { maxP = p; where.clear(); raw_string_ostream o(where); I->print(o); }
            live.erase(I);
            if (!isa<PHINode>(I)) for (auto& op : I->operands()) if (auto* d = dyn_cast<Instruction>(op.get())) if (isVal(d)) live.insert(d);
        }
    }
    printf("max pressure %u VGPRs at: %s\n", maxP, where.substr(0, 160).c_str());
}
