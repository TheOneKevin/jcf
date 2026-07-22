#include <deque>
#include <unordered_set>
#include <memory_resource>

#include "../IRPasses.h"
#include "../analysis/DominatorTree.h"
#include "diagnostics/Diagnostics.h"
#include "tir/BasicBlock.h"
#include "tir/Constant.h"
#include "tir/Instructions.h"
#include "utils/BumpAllocator.h"
#include "utils/PassManager.h"

using std::string_view;
using utils::PassManager;
using DE = diagnostics::DiagnosticEngine;
using namespace tir;

// Enclose everything but the pass in an anonymous namespace
namespace {

/* ===--------------------------------------------------------------------=== */
// HoistAlloca pass definition
/* ===--------------------------------------------------------------------=== */

/**
 * @brief Hoist alloca instructions into registers by placing phi nodes
 * in the dominance frontier of the alloca.
 * Citation: Simple and Efficient Construction of Static Single Assignment Form
 * Authors: Ron Cytron, et al.
 * URL: https://pages.cs.wisc.edu/~fischer/cs701.f14/ssa.pdf
 * Also referenced: https://c9x.me/compile/bib/braun13cc.pdf
 */
class HoistAlloca final {
public:
   HoistAlloca(analysis::DominatorTree*, tir::Function*,
               diagnostics::DiagnosticEngine&, BumpAllocator&) noexcept;
   std::ostream& print(std::ostream& os) const;
   void placePHINodes(AllocaInst* alloca);
   bool canAllocaBeReplaced(AllocaInst* alloca);
   void replaceUses(BasicBlock* alloca);

public:
   analysis::DominatorTree* DT;
   BumpAllocator& alloc;
   // Stores and loads to rewrite
   std::pmr::unordered_set<Instruction*> storesToRewrite{alloc};
   std::pmr::unordered_set<Instruction*> loadsToRewrite{alloc};
   // Phi -> Alloca map
   std::pmr::unordered_map<PhiNode*, AllocaInst*> phiAllocaMap{alloc};
   // Variable stack
   std::pmr::unordered_map<Value*, std::pmr::vector<Value*>> varStack{alloc};
};

/* ===--------------------------------------------------------------------=== */
// HoistAlloca implementation
/* ===--------------------------------------------------------------------=== */

HoistAlloca::HoistAlloca(analysis::DominatorTree* DT, tir::Function* Fn,
                         diagnostics::DiagnosticEngine& Diag,
                         BumpAllocator& Alloc) noexcept
      : DT{DT}, alloc{Alloc} {
   // 1. Print out the dominator tree
   if(Diag.Verbose(2)) {
      auto dbg = Diag.ReportDebug();
      DT->print(dbg.get());
   }
   // 2. Place PHI nodes for each alloca and record the uses
   for(auto alloca : Fn->allocas()) {
      if(canAllocaBeReplaced(alloca)) {
         placePHINodes(alloca);
         for(auto* user : alloca->users()) {
            if(auto* store = dyn_cast<StoreInst>(user)) {
               storesToRewrite.insert(store);
            } else if(auto* load = dyn_cast<LoadInst>(user)) {
               loadsToRewrite.insert(load);
            }
         }
      }
   }
   // 3. Print out the alloca and phi nodes
   if(Diag.Verbose()) {
      print(Diag.ReportDebug().get());
   }
   // 4. Replace the uses of the alloca with the phi nodes
   replaceUses(Fn->getEntryBlock());
   // 5. Do DCE on the stores and loads
   for(auto store : storesToRewrite) {
      assert(store->uses().size() == 0);
      store->eraseFromParent();
   }
   for(auto load : loadsToRewrite) {
      assert(load->uses().size() == 0);
      load->eraseFromParent();
   }
}

bool HoistAlloca::canAllocaBeReplaced(AllocaInst* alloca) {
   // 1. The allocas must be a scalar type
   if(!alloca->type()->isIntegerType() && !alloca->type()->isPointerType())
      return false;
   // 2. Each use must be a load or store instruction
   for(auto user : alloca->users()) {
      if(!dyn_cast<LoadInst>(user) && !dyn_cast<StoreInst>(user)) return false;
   }
   return true;
}

// Ref from paper, Figure 4. Placement of PHI-functions
void HoistAlloca::placePHINodes(AllocaInst* V) {
   std::pmr::unordered_set<BasicBlock*> DFPlus{alloc};
   std::pmr::unordered_set<BasicBlock*> Work{alloc};
   std::pmr::deque<BasicBlock*> W{alloc};
   // NOTE: A(V) = set of stores to V
   for(auto user : V->users()) {
      if(auto X = dyn_cast<StoreInst>(user)) {
         Work.insert(X->parent());
         W.push_back(X->parent());
      }
   }
   while(!W.empty()) {
      BasicBlock* X = W.front();
      W.pop_front();
      for(auto Y : DT->DF(X)) {
         if(DFPlus.contains(Y)) continue;
         auto phi = PhiNode::Create(V->ctx(), V->allocatedType(), {}, {});
         phi->setName("phi");
         Y->insertBeforeBegin(phi);
         phiAllocaMap[phi] = V;
         DFPlus.insert(Y);
         if(!Work.contains(Y)) {
            Work.insert(Y);
            W.push_back(Y);
         }
      }
   }
}

// Ref from paper, Figure 5. Construction of SSA form
void HoistAlloca::replaceUses(BasicBlock* X) {
   std::pmr::vector<Value*> pushed_vars{alloc};
   for(auto phi : X->phis()) {
      auto alloca = phiAllocaMap[phi];
      pushed_vars.push_back(alloca);
      varStack[alloca].push_back(phi);
   }
   for(auto inst : *X) {
      // If inst is an alloca, push it onto the stack and mark it as undef
      // and DO NOT add it to varStack[] (i.e., do not pop)
      if(auto alloca = dyn_cast<AllocaInst>(inst)) {
         varStack[alloca].push_back(
               Undef::Create(alloca->ctx(), alloca->allocatedType()));
         continue;
      }
      if(storesToRewrite.contains(inst)) { // "LHS"
         auto alloca = cast<AllocaInst>(inst->getChild(1));
         pushed_vars.push_back(alloca);
         varStack[alloca].push_back(inst->getChild(0));
      } else if(loadsToRewrite.contains(inst)) { // "RHS"
         auto alloca = cast<AllocaInst>(inst->getChild(0));
         auto newVar = varStack[alloca].back();
         inst->replaceAllUsesWith(newVar);
      }
   }
   for(auto Y : X->successors()) {
      for(auto phi : Y->phis()) {
         phi->replaceOrAddOperand(X, varStack[phiAllocaMap[phi]].back());
      }
   }
   for(auto Y : DT->getChildren(X)) {
      replaceUses(Y);
   }
   for(auto V : pushed_vars) varStack[V].pop_back();
}

std::ostream& HoistAlloca::print(std::ostream& os) const {
   os << "*** PHI node insertion points ***\n";
   for(auto [phi, alloca] : phiAllocaMap) {
      os << "  ";
      phi->printName(os) << " -> ";
      alloca->printName(os) << std::endl;
   }
   return os;
}

/* ===--------------------------------------------------------------------=== */
// MemToReg pass wrapper around HoistAlloca
/* ===--------------------------------------------------------------------=== */

class MemToReg final : public passes::Function {
public:
   MemToReg(PassManager& PM) noexcept : passes::Function(PM) {}
   string_view Name() const override { return "mem2reg"; }
   string_view Desc() const override { return "Promote memory to register"; }

private:
   void runOnFunction(tir::Function* Fn) override {
      assert(Fn->getEntryBlock());
      auto DT = &GetPass<passes::DominatorTreeWrapper>().DT();
      HoistAlloca{DT, Fn, PM().Diag(), NewAlloc(Lifetime::Temporary)};
   }
   void ComputeMoreDependencies() override {
      AddDependency(GetPass<passes::DominatorTreeWrapper>());
   }
};

} // namespace

REGISTER_PASS(MemToReg);
