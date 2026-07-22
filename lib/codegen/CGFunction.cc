#include <sstream>
#include <string_view>
#include <vector>

#include "ast/AstNode.h"
#include "ast/Decl.h"
#include "codegen/CodeGen.h"
#include "codegen/Mangling.h"
#include "tir/Constant.h"
#include "tir/IRBuilder.h"
#include "tir/Instructions.h"
#include "tir/Type.h"
#include "utils/Utils.h"

namespace codegen {

void CodeGenerator::emitFunctionDecl(ast::MethodDecl const* decl) {
   // 0. If it's abstract, exit now
   if(decl->modifiers().isAbstract()) return;
   // 1. Create the function signature type
   auto astRetTy = decl->returnTy().type;
   tir::Type* tirRetTy = tir::Type::getVoidTy(ctx);
   if(astRetTy) tirRetTy = emitType(astRetTy);
   std::vector<tir::Type*> paramTys;
   std::vector<std::string_view> paramNames;
   // a) If the function is not static, add "this" to the first parameter
   if(!decl->modifiers().isStatic()) {
      paramTys.push_back(tir::Type::getPointerTy(ctx));
      paramNames.push_back("this");
   }
   // b) Add the rest of the parameters
   for(auto* param : decl->parameters()) {
      paramTys.push_back(emitType(param->type()));
      paramNames.push_back(param->name());
   }
   // c) Create the function type
   auto* funcTy = tir::FunctionType::get(ctx, tirRetTy, paramTys);
   // 2. Create the function and set the argument names (for debugging)
   // 3. However, if it's a native function, we need to "native mangle" it.
   //    Native mangling is inferred from the "runtime.s" file.
   tir::Function* func = nullptr;
   if(decl->modifiers().isNative()) {
      std::ostringstream ss;
      ss << "NATIVE" << decl->getCanonicalName();
      func = cu.CreateFunction(funcTy, ss.str());
      func->setAttrs(tir::Function::Attrs{.external = true});
   } else {
      Mangler m{nr};
      m.MangleDecl(decl);
      func = cu.CreateFunction(funcTy, m.getMangledName());
      // FIXME(kevin): Better way to grab main function?
      if(decl->name() == "main" || decl->name() == "test") {
         func->setAttrs(tir::Function::Attrs{.external = true});
      }
   }
   gvMap[decl] = func;
   assert(func && "Function already exists");
   for(auto arg : func->args()) {
      arg->setName(paramNames[arg->index()]);
   }
}

void CodeGenerator::emitFunction(ast::MethodDecl const* decl) {
   // 0. If it's a native function, exit now
   if(decl->modifiers().isNative()) return;
   assert(decl->body() && "Function should not be abstract here");
   // 1. Grab the function from the gvMap and clear the valueMap
   auto func = cast<tir::Function>(gvMap[decl]);
   curFn = func;
   valueMap.clear();
   // 2. Emit the function body and add the allocas for the locals
   auto entry = builder.createBasicBlock(func);
   builder.setInsertPoint(entry->begin());
   unsigned paramNum = 0;
   for(auto* local : decl->decls()) {
      auto* const typedLocal = cast<ast::VarDecl>(local);
      auto* const localTy = emitType(typedLocal->type());
      auto* const val = func->createAlloca(localTy);
      val->setName(typedLocal->name());
      valueMap[typedLocal] = cast<tir::AllocaInst>(val);
      if(typedLocal->isArg()) {
         func->getEntryBlock()->appendAfterEnd(
               tir::StoreInst::Create(ctx, func->arg(paramNum++), val));
      }
   }
   emitStmt(decl->body());
   // 3. If the BB we're in does not end in a terminator, add a return
   if(builder.currentBlock()) {
      auto term = builder.currentBlock()->terminator();
      if(!term || !term->isTerminator()) {
         builder.createReturnInstr();
      }
   }
   // 4. End the function by clearing the curFn
   curFn = nullptr;
}

} // namespace codegen
