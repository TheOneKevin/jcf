#include "codegen/CodeGen.h"

#include <unordered_map>
#include <utility>
#include <vector>

#include "ast/AstNode.h"
#include "ast/Type.h"
#include "tir/Type.h"

namespace codegen {

using CG = CodeGenerator;

CG::CodeGenerator(tir::Context& ctx, tir::CompilationUnit& cu,
                  semantic::NameResolver& nr, semantic::HierarchyChecker& hc)
      : ctx{ctx}, cu{cu}, nr{nr}, hc{hc} {
   arrayType_ = tir::StructType::get(ctx,
                                     {// Length
                                      tir::Type::getInt32Ty(ctx),
                                      // Pointer
                                      tir::Type::getPointerTy(ctx)});
}

tir::Type* CG::emitType(ast::Type const* type) {
   using Kind = ast::BuiltInType::Kind;
   if(!type) {
      return tir::Type::getVoidTy(ctx);
   } else if(isAstTypeReference(type)) {
      return tir::Type::getPointerTy(ctx);
   } else if(type->isPrimitive()) {
      auto ty = cast<ast::BuiltInType>(type);
      switch(ty->getKind()) {
         case Kind::Boolean:
            return tir::Type::getInt1Ty(ctx);
         case Kind::Char:
            return tir::Type::getInt16Ty(ctx);
         case Kind::Short:
            return tir::Type::getInt16Ty(ctx);
         case Kind::Int:
            return tir::Type::getInt32Ty(ctx);
         case Kind::Byte:
            return tir::Type::getInt8Ty(ctx);
         default:
            std::unreachable();
      }
   }
   std::unreachable();
}

void CG::run(ast::LinkingUnit const* lu) {
   // 1. Populate the RTTI mappings
   populateRtti(lu);
   // 2. Populate method index table
   populateMethodIndexTable(lu);
   // 2. Generate the class structs
   for(auto* cu : lu->compliationUnits()) {
      for(auto* decl : cu->decls()) {
         if(auto* classDecl = dyn_cast<ast::ClassDecl>(decl)) {
            // We should still emit abstract class static fields
            emitClassDecl(classDecl);
         }
      }
   }
   // 3. Generate the class member functions
   for(auto* cu : lu->compliationUnits()) {
      for(auto* decl : cu->decls()) {
         if(auto* classDecl = dyn_cast<ast::ClassDecl>(decl)) {
            emitClass(classDecl);
         }
      }
   }
}

tir::Value* CG::emitGetArraySz(tir::Value* ptr) {
   using namespace tir;
   auto gepSz = builder.createGEPInstr(ptr, arrayType_, {0});
   gepSz->setName("arr.gep.sz");
   auto sz = builder.createLoadInstr(Type::getInt32Ty(ctx), gepSz);
   sz->setName("arr.sz");
   return sz;
}

tir::Value* CG::emitGetArrayPtr(tir::Value* ptr) {
   using namespace tir;
   auto gepPtr = builder.createGEPInstr(ptr, arrayType_, {1});
   gepPtr->setName("arr.gep.ptr");
   auto arrPtr = builder.createLoadInstr(Type::getPointerTy(ctx), gepPtr);
   arrPtr->setName("arr.ptr");
   return arrPtr;
}

void CG::emitSetArraySz(tir::Value* ptr, tir::Value* sz) {
   auto gepSz = builder.createGEPInstr(ptr, arrayType_, {0});
   gepSz->setName("arr.gep.sz");
   builder.createStoreInstr(sz, gepSz);
}

void CG::emitSetArrayPtr(tir::Value* ptr, tir::Value* arr) {
   auto gepPtr = builder.createGEPInstr(ptr, arrayType_, {1});
   gepPtr->setName("arr.gep.ptr");
   builder.createStoreInstr(arr, gepPtr);
}

void CG::populateRtti(ast::LinkingUnit const* lu) {
   int highestRtti = 0;
   // For each class and interface, add an entry to the RTTI map
   for(auto* cu : lu->compliationUnits()) {
      for(auto* decl : cu->decls()) {
         if(auto* classDecl = dyn_cast<ast::ClassDecl>(decl)) {
            rttiMap[classDecl] = highestRtti++;
         } else if(auto* iface = dyn_cast<ast::InterfaceDecl>(decl)) {
            rttiMap[iface] = highestRtti++;
         }
      }
   }
   // Create the RTTI table
   rttiTable.resize(highestRtti, std::vector<bool>(highestRtti, false));
   // Now populate the RTTI table (Ti, Tj)
   for(auto& [key, val] : rttiMap) {
      for(auto& [key2, val2] : rttiMap) {
         rttiTable[val][val2] = hc.isSubType(key, key2);
      }
   }
}

void CG::populateMethodIndexTable(ast::LinkingUnit const* lu) {
   auto inferenceGraph =
         std::unordered_map<const ast::MethodDecl*,
                            std::unordered_set<const ast::MethodDecl*>>{};
   // 1. Build the interface method graph
   for(auto* cu : lu->compliationUnits()) {
      for(auto* decl : cu->decls()) {
         if(auto* id = dyn_cast<ast::InterfaceDecl>(decl)) {
            for(auto* method : hc.getInheritedMethods(id)) {
               for(auto* method2 : hc.getInheritedMethods(id)) {
                  if(method == method2) continue;
                  inferenceGraph[method].insert(method2);
               }
            }
         } else if(auto* cd = dyn_cast<ast::ClassDecl>(decl)) {
            for(auto* method : hc.getInheritedMethods(cd)) {
               for(auto* method2 : hc.getInheritedMethods(cd)) {
                  if(method == method2) continue;
                  inferenceGraph[method].insert(method2);
               }
            }
         }
      }
   }
   // 2. Colour the graph and assign indices
   vtableIndexMap.clear();
   colorInterferenceGraph(inferenceGraph);
}

void CG::colorInterferenceGraph(
      std::unordered_map<const ast::MethodDecl*,
                         std::unordered_set<const ast::MethodDecl*>>& graph) {
         for (auto& [key, val] : graph) {
            if (vtableIndexMap.count(key)) continue; // Already coloured
            if (val.empty()) {
               vtableIndexMap[key] = 1;
               // std::cout << key->name() << " -> 0\n";
               continue;
            } // No neighbours, color it 1
            std::unordered_set<int> usedColors;
            for(auto* neighbour : val) {
               if(vtableIndexMap.count(neighbour)) {
                  usedColors.insert(vtableIndexMap[neighbour]);
               }
            }
            // colour it with the first available colour
            for(int i = 1; ; ++i) {
               if(usedColors.count(i) == 0) {
                  vtableIndexMap[key] = i;
                  // std::cout << key->name() << " -> " << i << "\n";
                  break;
               }
            }
         }
      }

} // namespace codegen