#include <vector>
#include "../IRPasses.h"
#include "tir/CompilationUnit.h"
#include "tir/Constant.h"
#include "utils/PassManager.h"

using std::string_view;
using utils::PassManager;

class GlobalDCE final : public passes::CompilationUnit {
public:
   GlobalDCE(PassManager& PM) noexcept : passes::CompilationUnit(PM) {}
   string_view Name() const override { return "globaldce"; }
   string_view Desc() const override { return "Global Dead Code Elimination"; }

private:
   void runOnCompilationUnit(tir::CompilationUnit* CU) override {
      bool changed;
      do {
         changed = removeAllGlobals(*CU);
      } while(changed);
   }

   bool removeAllGlobals(tir::CompilationUnit& CU) {
      std::vector<std::string> toRemove;
      for(auto p : CU.global_objects_kv()) {
         auto [name, go] = p;
         if(go->isExternalLinkage()) continue;
         if(!go->uses().empty()) continue;
         // Destroy the global object
         if(auto fn = dyn_cast<tir::Function>(go)) {
            for(auto bb : fn->body()) {
               auto* inst = *bb->begin();
               while(inst) {
                  auto next = inst->next();
                  inst->eraseFromParent();
                  inst = next;
               }
            }
         }
         toRemove.push_back(std::string{name});
      }
      for(auto name : toRemove) {
         CU.removeGlobalObject(name);
      }
      return toRemove.size() > 0;
   }
};

REGISTER_PASS(GlobalDCE);
