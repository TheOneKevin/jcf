#include "semantic/ExprStaticChecker.h"

#include "ast/AST.h"
#include "ast/ExprNode.h"
#include "diagnostics/Diagnostics.h"

namespace semantic {

using ETy = ExprStaticCheckerData;
using ESC = ExprStaticChecker;
namespace ex = ast::exprnode;
using namespace diagnostics;

// FIXME(kevin): We seem to use this in a few places, maybe move to header?
static bool isDeclStatic(ast::Decl const* decl) {
   if(auto* field = dyn_cast<ast::FieldDecl>(decl)) {
      return field->modifiers().isStatic();
   } else if(auto* method = dyn_cast<ast::MethodDecl>(decl)) {
      return method->modifiers().isStatic();
   }
   return false;
}

void ESC::Evaluate(ast::Expr* expr, bool isStaticContext,
                   bool isInstFieldInitializer) {
   this->isStaticContext = isStaticContext;
   this->isInstFieldInitializer = isInstFieldInitializer;
   loc_ = expr->location();
   ETy single = this->EvaluateList(expr->list());
   // Handle the case of a single member access
   checkInstanceVar(single);
}

ETy ESC::mapValue(ExprValue& node) const {
   // If node is "this", reject immediately
   if(dyn_cast<ex::ThisNode>(node) && isStaticContext) {
      throw diag.ReportError(loc_) << "cannot use 'this' in a static context";
   }

   // 1. This node is a pure type node
   // 2. This node is a value node, with type and decl
   // 3. This node is a literal, with type but no decl
   assert(dyn_cast<ex::MethodName>(node) || node.isTypeResolved());
   if(dyn_cast<ex::LiteralNode>(node)) {
      return ETy{nullptr, node.type(), true, false};
   } else if(dyn_cast<ex::TypeNode>(node)) {
      return ETy{nullptr, node.type(), false, false};
   } else {
      bool isParentClass = dyn_cast<ast::ClassDecl>(node.decl()->parent());
      bool isStaticModifier = isDeclStatic(node.decl());
      bool isInstanceVar = isParentClass && !isStaticModifier;
      return ETy{node.decl(), node.type(), true, isInstanceVar};
   }
}

ETy ESC::evalBinaryOp(BinaryOp& op, ETy lhs, ETy rhs) const {
   assert(op.resultType());
   checkInstanceVar(lhs);
   checkInstanceVar(rhs);
   return ETy{nullptr, op.resultType(), true, false};
}

ETy ESC::evalUnaryOp(UnaryOp& op, ETy val) const {
   assert(op.resultType());
   checkInstanceVar(val);
   return ETy{nullptr, op.resultType(), true, false};
}

ETy ESC::evalMemberAccess(DotOp& op, ETy lhs, ETy field) const {
   assert(op.resultType());
   // LHS may never be a type (it might not have a decl for ex. temporaries)
   assert(lhs.isValue);
   // RHS must be a field and have a resolved declaration
   assert(field.isValue && field.decl);
   // Only check if LHS is an instance variable
   checkInstanceVar(lhs);
   // The field must not be static because this is "instance . field"
   if(isDeclStatic(field.decl)) {
      throw diag.ReportError(loc_)
            << "cannot access a static field through an instance variable";
   }
   // See NOTE in ETy on why instance var is false here!
   return ETy{field.decl, op.resultType(), true, false};
}

ETy ESC::evalMethodCall(MethodOp& op, ETy method, const op_array& args) const {
   // Method must be value and have a resolved declaration
   assert(method.isValue && method.decl);
   // Accessing instance method in a static context
   checkInstanceVar(method);
   // And also all the arguments
   for(auto arg : args) {
      assert(arg.isValue);
      checkInstanceVar(arg);
   }
   // We don't assert op.resultType() because it can be null i.e., void
   return ETy{nullptr, op.resultType(), true, false};
}

ETy ESC::evalNewObject(NewOp& op, ETy ty, const op_array& args) const {
   assert(!ty.isValue && ty.type);
   for(auto arg : args) {
      assert(arg.isValue);
      checkInstanceVar(arg);
   }
   assert(op.resultType());
   return ETy{nullptr, op.resultType(), true, false};
}

ETy ESC::evalNewArray(NewArrayOp& op, ETy type, ETy size) const {
   assert(!type.isValue && type.type);
   assert(size.isValue);
   checkInstanceVar(size);
   assert(op.resultType());
   return ETy{nullptr, op.resultType(), true, false};
}

ETy ESC::evalArrayAccess(ArrayAccessOp& op, ETy arr, ETy idx) const {
   assert(arr.isValue);
   assert(idx.isValue);
   checkInstanceVar(arr);
   checkInstanceVar(idx);
   assert(op.resultType());
   return ETy{nullptr, op.resultType(), true, false};
}

ETy ESC::evalCast(CastOp& op, ETy type, ETy obj) const {
   assert(!type.isValue && type.type);
   assert(obj.isValue);
   assert(op.resultType());
   checkInstanceVar(obj);
   return ETy{nullptr, op.resultType(), true, false};
}

void ESC::checkInstanceVar(ETy var) const {
   if(!var.isInstanceVar) return;
   // Instance variable must not be accessed in a static context
   if(isStaticContext) {
      throw diag.ReportError(loc_)
            << "cannot access or invoke instance members in a static context";
   }
   // Instance variable accessed in a field initializer must satisfy
   // lexical order
   if(isInstFieldInitializer) {
   }
}

} // namespace semantic
