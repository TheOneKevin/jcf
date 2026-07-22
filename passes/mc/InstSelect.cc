#include <queue>
#include <vector>

#include "mc/InstSelectNode.h"
#include "mc/MCFunction.h"
#include "mc/Patterns.h"
#include "../IRPasses.h"
#include "target/TargetDesc.h"
#include "utils/PassManager.h"

using std::string_view;
using utils::Pass;
using utils::PassManager;
using namespace mc;

class InstSelect : public Pass {
public:
   InstSelect(PassManager& PM) noexcept : Pass(PM) {}
   string_view Name() const override { return "isel"; }
   string_view Desc() const override { return "Instruction selection on MIR DAG"; }
   void Init() override {
      dumpDot = PM().GetExistingOption("--debug-mc")->count();
   }
   void Run() override;

private:
   /**
    * @brief Performs instruction selection on the machine IR root node
    * for each basic block DAG subgraph.
    */
   void selectInstructions(mc::MCFunction*);

   /**
    * @brief Uses maximal munch to match the pattern starting from the root
    * node. Returns constructed new node with the matched pattern and linked
    * parameters. Will destroy the matched node.
    *
    * @param root The node to begin matching at
    * @return mc::InstSelectNode* The new node with the matched pattern
    */
   InstSelectNode* matchAndReplace(InstSelectNode* root);

private:
   void ComputeDependencies() override {
      AddDependency(GetPass<passes::IRContext>());
      AddDependency(GetPass("mirbuilder"));
   }

   target::TargetDesc const* TD;
   bool dumpDot;
};

/* ===--------------------------------------------------------------------=== */
// InstSelect implementation
/* ===--------------------------------------------------------------------=== */

void InstSelect::Run() {
   auto& IRCP = GetPass<passes::IRContext>();
   TD = &IRCP.TD();
   for(auto* F : IRCP.CU().functions()) {
      if(!F->hasBody()) continue;
      auto* MCF = IRCP.FindMIRFunction(F);
      selectInstructions(MCF);
      if(dumpDot) {
         std::ofstream out{std::string{F->name()} + ".dag.isel.dot"};
         MCF->printDot(out);
      }
   }
}

void InstSelect::selectInstructions(mc::MCFunction* F) {
   std::queue<InstSelectNode*> worklist;
   std::unordered_set<InstSelectNode*> visited;
   for(auto& mbb : F->subgraphs()) worklist.emplace(mbb.root);
   while(!worklist.empty()) {
      auto* root = worklist.front();
      worklist.pop();
      if(visited.count(root)) continue;
      visited.emplace(root);
      if(root->arity() > 0 && root->kind() != NodeKind::MachineInstr) {
         root = matchAndReplace(root);
      }
      for(auto* child : root->childNodes()) {
         if(child) {
            worklist.emplace(child);
         }
      }
   }
}

InstSelectNode* InstSelect::matchAndReplace(InstSelectNode* root) {
   // Arrays for matching, see mc::MatchOptions
   std::vector<InstSelectNode*> operands;
   std::vector<InstSelectNode*> nodesToDelete;
   for(auto* def : TD->patternProvider().getPatternFor(root->kind())) {
      for(auto* pat : def->patterns()) {
         operands.resize(def->adjustOperandIndex(def->numInputs(), *TD));
         // Important! Clear the operands array after resize.
         std::fill(operands.begin(), operands.end(), nullptr);
         nodesToDelete.clear();
         mc::MatchOptions MO{*TD, def, operands, nodesToDelete, root};
         if(pat->matches(MO)) {
            // Now we build the new node
            return root->selectPattern(MO);
         }
      }
   }
   return root;
}

REGISTER_PASS(InstSelect);
