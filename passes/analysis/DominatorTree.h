#pragma once

#include <memory>
#include <unordered_set>
#include <memory_resource>

#include "../IRPasses.h"
#include "tir/TIR.h"
#include "utils/BumpAllocator.h"

namespace analysis {

/**
 * @brief A class to compute and store the dominator tree and
 * dominance frontiers of a given function.
 * Citation: A Simple, Fast Dominance Algorithm
 * By: Keith D. Cooper, Timothy J. Harvey, and Ken Kennedy
 */
class DominatorTree final {
public:
   DominatorTree(tir::Function*, BumpAllocator&);
   // Print the dominator tree and dominance frontiers
   std::ostream& print(std::ostream& os) const;
   // Get the dominance frontier of block b
   auto& DF(tir::BasicBlock* b) { return frontiers[b]; }
   // Get the immediate dominator of block b, nullptr if it does not exist
   tir::BasicBlock* getIDom(tir::BasicBlock* b) {
      return doms.contains(b) ? doms.at(b) : nullptr;
   }
   // Get the children of the dominator tree
   auto& getChildren(tir::BasicBlock* b) { return domt[b]; }

private:
   tir::Function* func;
   BumpAllocator& alloc;
   std::pmr::unordered_map<tir::BasicBlock*, tir::BasicBlock*> doms{alloc};
   std::pmr::unordered_map<tir::BasicBlock*, int> poidx{alloc};
   std::pmr::unordered_map<tir::BasicBlock*,
                           std::pmr::unordered_set<tir::BasicBlock*>>
         frontiers{alloc};
   std::pmr::unordered_map<tir::BasicBlock*, std::pmr::vector<tir::BasicBlock*>>
         domt{alloc};

   void computePostorderIdx(tir::Function* func);
   void computeDominators(tir::Function* func);
   tir::BasicBlock* intersect(tir::BasicBlock* b1, tir::BasicBlock* b2);
   void computeFrontiers(tir::Function* func);
};

} // namespace analysis

namespace passes {

class DominatorTreeWrapper final : public Function {
public:
   DominatorTreeWrapper(utils::PassManager& PM) noexcept : passes::Function(PM) {}
   std::string_view Name() const override { return "dt"; }
   std::string_view Desc() const override { return "Dominator tree analysis"; }
   auto& DT() { return *dt_; }
   void GC() override { dt_ = nullptr; }

private:
   void runOnFunction(tir::Function* Fn) override {
      auto& alloc = NewAlloc(Lifetime::Managed);
      dt_ = std::make_unique<analysis::DominatorTree>(Fn, alloc);
   }
   std::unique_ptr<analysis::DominatorTree> dt_;
};

} // namespace passes
