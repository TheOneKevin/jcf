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
   // 1. Emit the function declarations (methods and constructors)
   for(auto* method : decl->methods()) emitFunctionDecl(method);
   for(auto* ctor : decl->constructors()) emitFunctionDecl(ctor);
   // 2. Emit any static fields as globals
   // 3. Construct the class struct type as well
   std::vector<tir::Type*> fieldTypes{};
   // 3a) Add the VTable pointer field
   fieldTypes.push_back(tir::Type::getPointerTy(ctx));
   // 3b) Instance fields, in a stable order (inherited first, then this class's
   //     own) so a field keeps the same struct slot in a subclass as in its
   //     declaring class. getInheritedMembersInOrder() bundles the inherited
   //     members together with this class's own fields, and it includes static
   //     fields as well; statics are class-level globals rather than part of the
   //     object layout, so we skip them here and create their globals in 3c.
   for(auto* field : hc.getInheritedMembersInOrder(decl)) {
      if(field->modifiers().isStatic()) continue;
      fieldTypes.push_back(emitType(field->type()));
      if(fieldIndexMap.contains(field)) {
         assert(fieldIndexMap[field] == static_cast<int>(fieldTypes.size() - 1));
      } else {
         fieldIndexMap[field] = fieldTypes.size() - 1;
      }
   }
   // 3c) This class's own static fields become globals. (Instance fields were
   //     already laid out in 3b, which includes this class's own fields, so we
   //     must not add them to the struct a second time.)
   for(auto* field : decl->fields()) {
      if(!field->modifiers().isStatic()) continue;
      Mangler m{nr};
      m.MangleDecl(field);
      gvMap[field] = cu.CreateGlobalVariable(emitType(field->type()),
                                             m.getMangledName());
   }
   // 4. Create the struct type and map it
   if(!fieldTypes.empty()) {
      typeMap[decl] = tir::StructType::get(ctx, fieldTypes);
   }
}

void CodeGenerator::emitClass(ast::ClassDecl const* decl) {
   // NOTE: vtables are emitted in a separate earlier pass (see run()) so that
   // vtableMap is complete before any method body constructs an object.
   // 2. Emit all non-abstract method bodies (static and instance).
   //    emitFunction skips native methods internally.
   for(auto* method : decl->methods()) {
      if(method->modifiers().isAbstract()) continue;
      emitFunction(method);
   }
   // 3. Emit constructor bodies.
   for(auto* ctor : decl->constructors()) {
      emitFunction(ctor);
   }
}

ast::ClassDecl const* CodeGenerator::directSuperClass(
      ast::ClassDecl const* decl) const {
   // superClasses()[0] is the explicit `extends` class; [1] is the implicit
   // java.lang.Object superclass used when there is no explicit extends.
   auto supers = decl->superClasses();
   ast::ReferenceType* super = supers[0] ? supers[0] : supers[1];
   if(!super) return nullptr;
   return dyn_cast<ast::ClassDecl>(super->decl());
}

void CodeGenerator::emitCtorPrologue(ast::MethodDecl const* ctor) {
   auto* cls = cast<ast::ClassDecl>(ctor->parent());
   auto* thisArg = curFn->args().front();
   // 1. Implicit super(): call the direct superclass's zero-argument
   //    constructor. Joos has no explicit super(...)/this(...) calls, so this
   //    matches Java's implicit-super semantics.
   if(auto* superCls = directSuperClass(cls)) {
      ast::MethodDecl const* superCtor = nullptr;
      for(auto* c : superCls->constructors())
         if(c->parameters().empty()) {
            superCtor = c;
            break;
         }
      if(superCtor && gvMap.count(superCtor)) {
         builder.createCallInstr(cast<tir::Function>(gvMap[superCtor]),
                                 {thisArg});
      }
   }
   // 2. Instance field initializers, in declaration order.
   for(auto* field : cls->fields()) {
      if(field->modifiers().isStatic() || !field->hasInit()) continue;
      auto idx = static_cast<unsigned>(fieldIndexMap.at(field));
      auto* gep = builder.createGEPInstr(thisArg, typeMap[cls], {idx});
      builder.createStoreInstr(emitExpr(field->init()), gep);
   }
}

} // namespace codegen
