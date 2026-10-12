// SPDX-License-Identifier: GPL-3.0-only
// x4rdep: for each dpas2/dpas.nosrc0, does operand A (activations) / B (weights) depend on the CM local thread id?
#include "llvm/IR/Constants.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"
#include "llvm/IRReader/IRReader.h"
#include "llvm/Support/SourceMgr.h"
#include <map>
#include <cstdio>
using namespace llvm;
std::map<Value*, int> memo;   // bit0: depends on local id, bit1: depends on loop-carried phi (conservative)
int dep(Value* v, int depth = 0)
{
    auto it = memo.find(v); if (it != memo.end()) return it->second;
    memo[v] = 0;   // break phi cycles
    int r = 0;
    if (auto* I = dyn_cast<Instruction>(v))
    {
        if (auto* C = dyn_cast<CallInst>(I))
            if (Function* f = C->getCalledFunction())
            {
                std::string n = f->getName().str();
                if (n == "_Z12get_local_idj") { auto* ci = dyn_cast<ConstantInt>(C->getArgOperand(0)); r |= 1 << (1 + (ci ? ci->getZExtValue() : 0)); }
                if (n == "_Z19get_local_linear_idv" || n == "_Z20get_global_linear_idv") r |= 1;
            }
        for (auto& op : I->operands()) r |= dep(op.get(), depth + 1);
    }
    memo[v] = r; return r;
}
int main(int argc, char** argv)
{
    LLVMContext ctx; SMDiagnostic err; auto M = parseIRFile(argv[1], err, ctx);
    int nA = 0, nB = 0, tot = 0, da = 0, db = 0;
    for (auto& F : *M) for (auto& BB : F) for (auto& I : BB)
        if (auto* C = dyn_cast<CallInst>(&I)) if (Function* f = C->getCalledFunction())
        {
            std::string n = f->getName().str();
            Value *A = nullptr, *B = nullptr;
            if (n.rfind("llvm.genx.dpas2", 0) == 0) { B = C->getArgOperand(1); A = C->getArgOperand(2); }
            else if (n.rfind("llvm.genx.dpas.nosrc0", 0) == 0) { B = C->getArgOperand(0); A = C->getArgOperand(1); }
            else continue;
            ++tot; da |= dep(A); db |= dep(B);
        }
    auto d = [](int m) { std::string r; if (m & 1) r += "linear "; if (m & 2) r += "x "; if (m & 4) r += "y "; if (m & 8) r += "z "; return r.empty() ? std::string("none") : r; }; printf("dpas %d: A depends on [%s] B depends on [%s]\n", tot, d(da).c_str(), d(db).c_str());
}
