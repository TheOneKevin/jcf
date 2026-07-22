#pragma once

#include <memory_resource>
#include "mc/InstSelectNode.h"
#include "target/Target.h"
#include "utils/BumpAllocator.h"

namespace mc {

class MCFunction final {
public:
   struct MCBasicBlock {
      InstSelectNode* root;
      InstSelectNode* entry;
      InstSelectNode* successors[2];
   };

public:
   MCFunction(BumpAllocator& alloc, target::TargetInfo const& TI,
              target::TargetDesc const& TD)
         : alloc_{alloc}, TI_{TI}, TD_{TD}, graphs_{alloc} {}

   /// @brief Prints the DAG as a collection of subgraphs in DOT format
   std::ostream& printDot(std::ostream& os) const;
   /// @brief Gets the allocator
   BumpAllocator& alloc() const { return alloc_; }
   /// @brief Gets the target info
   auto const& TI() const { return TI_; }
   /// @brief Gets the target desc
   auto const& TD() const { return TD_; }
   /// @brief Adds a subgraph (basic block) to the function
   void addSubgraph(InstSelectNode* graph) {
      assert(graph->kind() == NodeKind::Entry && "Subgraph must be an entry node");
      graphs_.push_back(MCBasicBlock{graph, nullptr, {nullptr, nullptr}});
   }
   /// @brief Iterate the subgraphs of the function
   auto subgraphs() { return std::views::all(graphs_); }
   /// @brief Iterate the subgraphs of the function
   auto subgraphs() const { return std::views::all(graphs_); }

private:
   BumpAllocator& alloc_;
   target::TargetInfo const& TI_;
   target::TargetDesc const& TD_;
   std::pmr::vector<MCBasicBlock> graphs_;
};

} // namespace mc
