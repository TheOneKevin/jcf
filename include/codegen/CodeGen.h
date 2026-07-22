#pragma once

#include <vector>
#include "ast/AST.h"
#include "ast/Decl.h"
#include "ast/DeclContext.h"
#include "ast/Stmt.h"
#include "semantic/HierarchyChecker.h"
#include "semantic/NameResolver.h"
#include "tir/Constant.h"
#include "tir/IRBuilder.h"
#include "tir/Instructions.h"
#include "tir/TIR.h"
#include "tir/Type.h"

namespace codegen {

class CGExprEvaluator;

/**
 * @brief Returns if the AST type will be mapped to a pointer in IR or not.
 *
 * @param ty The AST type
 * @return true True for strings, null, arrays and references.
 * @return false False for primitive types.
 */
static bool isAstTypeReference(ast::Type const* ty) {
   return ty->isString() || ty->isNull() || ty->isReference() || ty->isArray();
}

class CodeGenerator {
   friend class CGExprEvaluator;

public:
   CodeGenerator(tir::Context& ctx, tir::CompilationUnit& cu,
                 semantic::NameResolver& nr, semantic::HierarchyChecker& hc);
   CodeGenerator(CodeGenerator const&) = delete;
   CodeGenerator(CodeGenerator&&) = delete;
   CodeGenerator& operator=(CodeGenerator const&) = delete;
   CodeGenerator& operator=(CodeGenerator&&) = delete;

   // Emit the AST linking unit
   void run(ast::LinkingUnit const* lu);
   // Emit the type corresponding to the AST type
   tir::Type* emitType(ast::Type const* type);
   // Gets the array struct type used
   tir::StructType* arrayType() const { return arrayType_; }

private:
   void emitStmt(ast::Stmt const* stmt);
   tir::Value* emitExpr(ast::Expr const* expr);
   // Emit the function body, assuming the declaration is there
   void emitFunction(ast::MethodDecl const* decl);
   // Emit just the declaration, no body
   void emitFunctionDecl(ast::MethodDecl const* decl);
   // Emit the class type and static fields, no body
   void emitClassDecl(ast::ClassDecl const* decl);
   // Emit the class body (methods and field initializers)
   void emitClass(ast::ClassDecl const* decl);
   // Populate the RTTI mappings
   void populateRtti(ast::LinkingUnit const* lu);
   // Populate the method index table
   void populateMethodIndexTable(ast::LinkingUnit const* lu);
   void colorInterferenceGraph(
         std::unordered_map<const ast::MethodDecl*,
                            std::unordered_set<const ast::MethodDecl*>>& graph);
   // Emit the vtable in the IR for the class
   void emitVTable(ast::ClassDecl const* decl);

private:
   // Given a pointer to the array struct, read out the size
   tir::Value* emitGetArraySz(tir::Value* ptr);
   // Given a pointer to the array struct, read out the pointer
   tir::Value* emitGetArrayPtr(tir::Value* ptr);
   // Given a pointer to the array struct, set the size
   void emitSetArraySz(tir::Value* ptr, tir::Value* sz);
   // Given a pointer to the array struct, set the pointer
   void emitSetArrayPtr(tir::Value* ptr, tir::Value* arr);

private:
   void emitReturnStmt(ast::ReturnStmt const* stmt);
   void emitForStmt(ast::ForStmt const* stmt);
   void emitBlockStmt(ast::BlockStatement const* stmt);
   void emitDeclStmt(ast::DeclStmt const* stmt);
   void emitExprStmt(ast::ExprStmt const* stmt);
   void emitIfStmt(ast::IfStmt const* stmt);
   void emitWhileStmt(ast::WhileStmt const* stmt);

private:
   tir::Context& ctx;
   tir::CompilationUnit& cu;
   tir::Function* curFn{nullptr};
   // Local AST local decl -> IR alloca
   std::unordered_map<ast::VarDecl const*, tir::AllocaInst*> valueMap{};
   // Global static AST func/field -> IR global value
   std::unordered_map<ast::Decl const*, tir::Value*> gvMap{};
   // Global AST Class -> IR struct type map
   std::unordered_map<ast::ClassDecl const*, tir::StructType*> typeMap{};
   // Array type (cache)
   tir::StructType* arrayType_{nullptr};
   // AST class -> TypeID map
   std::unordered_map<ast::Decl const*, int> rttiMap{};
   // MxM table for rtti, where M is the total number of types
   std::vector<std::vector<bool>> rttiTable{};
   // AST class field -> index in the IR struct type for that class (typeMap)
   std::unordered_map<ast::FieldDecl const*, int> fieldIndexMap{};
   // AST class -> VTable IR struct type map
   std::unordered_map<ast::ClassDecl const*, tir::Value*> vtableMap{};
   // AST class method -> VTable index
   std::unordered_map<ast::MethodDecl const*, int> vtableIndexMap{};
   tir::IRBuilder builder{ctx};
   semantic::NameResolver& nr;
   semantic::HierarchyChecker& hc;
};

} // namespace codegen
