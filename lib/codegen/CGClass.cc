#include <vector>
#include "ast/AstNode.h"
#include "ast/Type.h"
#include "codegen/CodeGen.h"
#include "codegen/Mangling.h"
#include "tir/Type.h"
#include "tir/Value.h"

namespace codegen {

void CodeGenerator::emitVTable(ast::ClassDecl const* decl) {
   int numEntries = 0;
   for(auto* method : hc.getInheritedMethods(decl)) {
      numEntries = std::max(numEntries, vtableIndexMap[method]);
   }
   std::vector<tir::Type*> fieldTypes{(unsigned)numEntries + 1};
   // 1. TypeID
   fieldTypes[0] = (tir::Type::getInt32Ty(ctx));
   // 2. Fill the rest with function pointers
   for(int i = 1; i < numEntries + 1; i++) {
      fieldTypes[i] = tir::Type::getPointerTy(ctx);
   }
   tir::StructType* vtableType = tir::StructType::get(ctx, fieldTypes);
   tir::Value* vtableGlobal;
   // Create a vtable global variable for the class (mangled)
   {
      Mangler m{nr};
      m.MangleVTable(decl);
      vtableGlobal = cu.CreateGlobalVariable(vtableType, m.getMangledName());
      vtableMap[decl] = vtableGlobal;
   }

   // Create a function called "void @jcf.vtable.ctor.<class-name>()"
   tir::Function* F;
   {
      Mangler m{nr};
      m.MangleDecl(decl);
      F = cu.CreateFunction(
            tir::FunctionType::get(ctx, tir::Type::getVoidTy(ctx), {}),
            "jcf.vtable.ctor." + m.getMangledName());
   }

   // TODO(larry): Emit ctor into F
   tir::IRBuilder builder{ctx};
   auto bb = builder.createBasicBlock(F);
   builder.setInsertPoint(bb->begin());
   // vtable_global_value[1] = func is basically:
   //    %gep = getelementpointer %vtable_global_value, i64 1
   //    store %func, %gep
   // builder.createStoreInstr(/* Value you're storing */ func, /* Where are you
   // storing it? */ gep);
   for(auto* method : hc.getInheritedMethods(decl)) {
      auto gep = builder.createGEPInstr(
            vtableGlobal, vtableType, {vtableIndexMap[method]});
      builder.createStoreInstr(gvMap[method], gep);
   }
   builder.createReturnInstr();
}

void CodeGenerator::emitClassDecl(ast::ClassDecl const* decl) {
   // 1. Emit the function declarations
   for(auto* method : decl->methods()) emitFunctionDecl(method);
   // 2. Emit any static fields as globals
   // 3. Construct the class struct type as well
   std::vector<tir::Type*> fieldTypes{};
   // 3a) Add the VTable pointer field
   fieldTypes.push_back(tir::Type::getPointerTy(ctx));
   // 3b) Inherited members first
   for(auto* field : hc.getInheritedMembersInOrder(decl)) {
      fieldTypes.push_back(emitType(field->type()));
      if(fieldIndexMap.contains(field)) {
         assert(fieldIndexMap[field] == static_cast<int>(fieldTypes.size() - 1));
      } else {
         fieldIndexMap[field] = fieldTypes.size() - 1;
      }
   }
   // 3c) Member and static fields
   for(auto* field : decl->fields()) {
      auto ty = emitType(field->type());
      // 3c.i) Static fields
      if(field->modifiers().isStatic()) {
         Mangler m{nr};
         m.MangleDecl(field);
         gvMap[field] = cu.CreateGlobalVariable(ty, m.getMangledName());
      }
      // 3c.ii) Member fields
      else {
         fieldTypes.push_back(ty);
         fieldIndexMap[field] = fieldTypes.size() - 1;
      }
   }
   // 4. Create the struct type and map it
   if(!fieldTypes.empty()) {
      typeMap[decl] = tir::StructType::get(ctx, fieldTypes);
   }
}

void CodeGenerator::emitClass(ast::ClassDecl const* decl) {
   // 1. Emit vtable and its ctor function
   //    But we shouldn't emit abstract class members and vtables
   if(!decl->modifiers().isAbstract()) {
      emitVTable(decl);
   }
   // 2. Emit the class methods
   for(auto* method : decl->methods()) {
      if(method->modifiers().isStatic()) {
         emitFunction(method);
      }
   }
}

} // namespace codegen
