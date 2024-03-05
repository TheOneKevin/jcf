#include "semantic/ExprTypeResolver.h"

#include "ast/AstNode.h"
#include "ast/DeclContext.h"
#include "ast/Expr.h"
#include "ast/ExprNode.h"
#include "ast/Type.h"
#include "diagnostics/Location.h"
#include "semantic/NameResolver.h"

namespace semantic {

using namespace ast;
using namespace ast::exprnode;

void ExprTypeResolver::resolve(ast::Expr* expr) {
   loc_ = expr->location();
   this->Evaluate(expr);
}

// 5.1.2 Widening Primitive Conversion
static bool isWiderThan(const BuiltInType* type, const BuiltInType* other) {
   using Kind = ast::BuiltInType::Kind;
   if(other->getKind() == Kind::Char || other->getKind() == Kind::Short) {
      return type->getKind() == Kind::Int;
   }
   if(other->getKind() == Kind::Byte) {
      return type->getKind() == Kind::Short || type->getKind() == Kind::Int;
   }
   return false;
}

// 1. Identity conversion
// 2. Widening Primitive Conversion
//    2.1 Null type can be cast to any class type, interface type, or array type.
// 3. Widening Reference Conversions
//    3.1 Class type to any super class, or interface that it implements.
//    3.2 Interface type to any super interface OR Object class
//    3.3 Array type
//       3.3.1 Array type to Object class
//       3.3.2 Array type to Cloneable interface
//       3.3.3 Array type to java.io.Serializable interface
//       3.3.4 Array type to another array type given the element type is a
//       widening REFERENCE conversion
bool ExprTypeResolver::isAssignableTo(const Type* lhs, const Type* rhs) const {
   // step 1
   if(*lhs == *rhs) return true;

   auto leftPrimitive = dynamic_cast<const ast::BuiltInType*>(lhs);
   auto rightPrimitive = dynamic_cast<const ast::BuiltInType*>(rhs);
   auto leftRef = dynamic_cast<const ast::ReferenceType*>(lhs);
   auto rightRef = dynamic_cast<const ast::ReferenceType*>(rhs);
   auto leftArr = dynamic_cast<const ast::ArrayType*>(lhs);
   auto rightArr = dynamic_cast<const ast::ArrayType*>(rhs);

   // step 2
   if(leftPrimitive && rightPrimitive) {
      return isWiderThan(leftPrimitive, rightPrimitive);
   }
   // Step 2.1
   if(rightPrimitive &&
      rightPrimitive->getKind() == ast::BuiltInType::Kind::NoneType) {
      return leftRef || leftArr;
   }

   if(leftRef && rightRef) {
      if(auto rightClass = dynamic_cast<ClassDecl const*>(rightRef->decl())) {
         // Step 3.1
         if(auto leftClass = dynamic_cast<ClassDecl const*>(leftRef->decl())) {
            return HC->isSuperClass(leftClass, rightClass);
         } else if(auto leftInterface =
                         dynamic_cast<InterfaceDecl const*>(leftRef->decl())) {
            return HC->isSuperInterface(leftInterface, rightClass);
         } else {
            assert(false && "Unreachable");
         }
      } else if(auto rightInterface =
                      dynamic_cast<InterfaceDecl const*>(rightRef->decl())) {
         // Step 3.2
         if(auto leftClass = dynamic_cast<ClassDecl const*>(leftRef->decl())) {
            return leftClass = NR->GetJavaLang().Object;
         } else if(auto leftInterface =
                         dynamic_cast<InterfaceDecl const*>(leftRef->decl())) {
            return HC->isSuperInterface(leftInterface, rightInterface);
         } else {
            assert(false && "Unreachable");
         }
      }
   }

   if(rightArr) {
      if(leftArr) {
         // Step 3.3.4
         auto leftElem = dynamic_cast<ReferenceType*>(leftArr->getElementType());
         auto rightElem = dynamic_cast<ReferenceType*>(rightArr->getElementType());
         return leftElem && rightElem && isAssignableTo(leftElem, rightElem);
      } else if(leftRef) {
         // Step 3.3.1
         if(leftRef->decl() == NR->GetJavaLang().Object) {
            return true;
         }
         // Step 3.3.2
         if(leftRef->decl() == NR->GetJavaLang().Cloneable) {
            return true;
         }
         // Step 3.3.3
         // Fixme(Kevin, Larry) Add method to get serializable
         // if ( leftRef->decl() == NR->GetSerializable()) {
         //    return true;
         // }
      }
   }
   return false;
}

bool ExprTypeResolver::isValidCast(const Type* exprType,
                                   const Type* castType) const {
   if(*exprType == *castType) return true;

   auto leftPrimitive = dyn_cast<ast::BuiltInType*>(exprType);
   auto rightPrimitive = dyn_cast<ast::BuiltInType*>(castType);

   auto leftRef = dyn_cast<ast::ReferenceType*>(exprType);
   auto rightRef = dyn_cast<ast::ReferenceType*>(castType);

   auto leftArr = dyn_cast<ast::ArrayType*>(exprType);
   auto rightArr = dyn_cast<ast::ArrayType*>(castType);

   if(leftPrimitive && rightPrimitive) {
      return isAssignableTo(rightPrimitive, leftPrimitive);
   } else if(leftRef && rightRef) {
      auto leftInterface = dyn_cast<ast::InterfaceDecl*>(leftRef->decl());
      auto rightInterface = dyn_cast<ast::InterfaceDecl*>(rightRef->decl());

      auto leftClass = dyn_cast<ast::ClassDecl*>(leftRef->decl());
      auto rightClass = dyn_cast<ast::ClassDecl*>(rightRef->decl());

      if(leftInterface && rightInterface) {
         return true;
      } else if(leftInterface && !rightClass->modifiers().isFinal()) {
         return true;
      } else if(rightInterface && !leftClass->modifiers().isFinal()) {
         return true;
      } else {
         return isAssignableTo(leftRef, rightRef) ||
                isAssignableTo(rightRef, leftRef);
      }
   } else if(leftArr && rightArr) {
      auto leftElem = dynamic_cast<ReferenceType*>(leftArr->getElementType());
      auto rightElem = dynamic_cast<ReferenceType*>(rightArr->getElementType());
      return leftElem && rightElem &&
             isValidCast(leftArr->getElementType(), rightArr->getElementType());
   } else {
      throw diag.ReportError(loc_) << "Invalid cast from " << exprType->toString()
                                   << " to " << castType->toString();
   }
}

Type const* ExprTypeResolver::mapValue(ExprValue& node) const {
   assert(node.isDeclResolved() && "ExprValue decl is not resolved");
   assert(node.isTypeResolved() && "ExprValue type is not resolved");
   return node.type();
}

Type const* ExprTypeResolver::evalBinaryOp(BinaryOp& op, const Type* lhs,
                                           const Type* rhs) const {
   if(op.resultType()) return op.resultType();
   switch(op.opType()) {
      case BinaryOp::OpType::Assignment: {
         if(isAssignableTo(lhs, rhs)) {
            return op.resolveResultType(lhs);
         } else {
            throw diag.ReportError(loc_)
                  << "Invalid assignment, " << lhs->toString()
                  << " is not assignable to " << rhs->toString() << " side";
         }
      }

      case BinaryOp::OpType::GreaterThan:
      case BinaryOp::OpType::GreaterThanOrEqual:
      case BinaryOp::OpType::LessThan:
      case BinaryOp::OpType::LessThanOrEqual: {
         if(lhs->isNumeric() && rhs->isNumeric()) {
            return op.resolveResultType(
                  sema.BuildBuiltInType(ast::BuiltInType::Kind::Boolean));
         } else {
            throw diag.ReportError(loc_)
                  << "Invalid types for "
                  << BinaryOp::OpType_to_string(op.opType(), "??")
                  << " operation, operands are non-numeric";
         }
      }

      case BinaryOp::OpType::Equal:
      case BinaryOp::OpType::NotEqual: {
         if(lhs->isNumeric() && rhs->isNumeric()) {
            return op.resolveResultType(
                  sema.BuildBuiltInType(ast::BuiltInType::Kind::Boolean));
         } else if(lhs->isBoolean() && rhs->isBoolean()) {
            return op.resolveResultType(
                  sema.BuildBuiltInType(ast::BuiltInType::Kind::Boolean));
         }

         auto lhsType = dynamic_cast<const ast::ReferenceType*>(lhs);
         auto rhsType = dynamic_cast<const ast::ReferenceType*>(rhs);

         if((lhs->isNull() || lhsType) && (rhs->isNull() || rhsType) &&
            (isValidCast(lhs, rhs) || isValidCast(rhs, lhs))) {
            return op.resolveResultType(
                  sema.BuildBuiltInType(ast::BuiltInType::Kind::Boolean));
         } else {
            throw diag.ReportError(loc_)
                  << "Invalid types for "
                  << BinaryOp::OpType_to_string(op.opType(), "??")
                  << " operation, operands are not of the same type";
         }
      }

      case BinaryOp::OpType::Add: {
         if(lhs->isString() || rhs->isString()) {
            return op.resolveResultType(
                  sema.BuildBuiltInType(ast::BuiltInType::Kind::String));
         } else if(lhs->isNumeric() && rhs->isNumeric()) {
            return op.resolveResultType(
                  sema.BuildBuiltInType(ast::BuiltInType::Kind::Int));
         } else {
            throw diag.ReportError(loc_)
                  << "Invalid types for arithmetic "
                  << BinaryOp::OpType_to_string(op.opType(), "??") << " operation";
         }
      }

      case BinaryOp::OpType::And:
      case BinaryOp::OpType::Or:
      case BinaryOp::OpType::BitwiseAnd:
      case BinaryOp::OpType::BitwiseOr:
      case BinaryOp::OpType::BitwiseXor: {
         if(lhs->isBoolean() && rhs->isBoolean()) {
            return op.resolveResultType(
                  sema.BuildBuiltInType(ast::BuiltInType::Kind::Boolean));
         } else {
            throw diag.ReportError(loc_)
                  << "Invalid types for "
                  << BinaryOp::OpType_to_string(op.opType(), "??")
                  << " operation, operands are non-boolean";
         }
      }

      case BinaryOp::OpType::Subtract:
      case BinaryOp::OpType::Multiply:
      case BinaryOp::OpType::Divide:
      case BinaryOp::OpType::Modulo: {
         if(lhs->isNumeric() && rhs->isNumeric()) {
            return op.resolveResultType(
                  sema.BuildBuiltInType(ast::BuiltInType::Kind::Int));
         } else {
            throw diag.ReportError(loc_)
                  << "Invalid types for "
                  << BinaryOp::OpType_to_string(op.opType(), "??")
                  << " operation, operands are non-numeric";
         }
      }

      case BinaryOp::OpType::InstanceOf: {
         auto lhsType = dynamic_cast<const ast::ReferenceType*>(lhs);
         auto rhsType = dynamic_cast<const ast::ReferenceType*>(rhs);

         if((lhs->isNull() || lhsType) && !rhs->isNull() && rhsType &&
            isValidCast(rhs, lhs)) {
            return op.resolveResultType(
                  sema.BuildBuiltInType(ast::BuiltInType::Kind::Boolean));
         } else {
            throw diag.ReportError(loc_)
                  << "Invalid types for "
                  << BinaryOp::OpType_to_string(op.opType(), "??")
                  << " operation, operands are null or reference types that can't "
                     "be casted";
         }
      }

      default:
         throw diag.ReportError(loc_) << "Invalid binary operation";
   }
}

Type const* ExprTypeResolver::evalUnaryOp(UnaryOp& op, const Type* rhs) const {
   if(op.resultType()) return op.resultType();
   switch(op.opType()) {
      case UnaryOp::OpType::Plus:
      case UnaryOp::OpType::Minus:
      case UnaryOp::OpType::BitwiseNot:
         if(rhs->isNumeric()) {
            return op.resolveResultType(
                  sema.BuildBuiltInType(ast::BuiltInType::Kind::Int));
         } else {
            throw diag.ReportError(loc_)
                  << "Invalid type for unary "
                  << UnaryOp::OpType_to_string(op.opType(), "??")
                  << " non-numeric";
         }
      case UnaryOp::OpType::Not:
         if(rhs->isBoolean()) {
            return op.resolveResultType(
                  sema.BuildBuiltInType(ast::BuiltInType::Kind::Boolean));
         } else {
            throw diag.ReportError(loc_)
                  << "Invalid type for unary not, non-boolean";
         }
      default:
         throw diag.ReportError(loc_) << "Invalid unary operation";
   }
}

Type const* ExprTypeResolver::evalMemberAccess(DotOp&, const Type*,
                                               const Type* field) const {
   return field;
}

Type const* ExprTypeResolver::evalMethodCall(MethodOp& op, const Type* method,
                                             const op_array& args) const {
   if(op.resultType()) return op.resultType();
   auto methodType = dynamic_cast<const MethodType*>(method);
   assert(methodType && "Not a method type");

   auto methodParams = methodType->getParamTypes();
   assert(methodParams.size() == args.size() &&
          "Method params and args size mismatch");

   for(size_t i = 0; i < args.size(); i++) {
      if(!isAssignableTo(methodParams[i], args[i])) {
         throw diag.ReportError(loc_) << "Invalid argument type for method call";
      }
   }

   // fixme(owen, larry) do we want to return nullptr when the type is void?
   return op.resolveResultType(methodType->getReturnType()->type);
}

Type const* ExprTypeResolver::evalNewObject(NewOp& op, const Type* object,
                                            const op_array& args) const {
   if(op.resultType()) return op.resultType();
   auto constructor = dynamic_cast<const MethodType*>(object);
   assert(constructor && "Not a method type");

   auto constructorParams = constructor->getParamTypes();
   assert(constructorParams.size() == args.size() &&
          "Constructor params and args size mismatch");

   for(size_t i = 0; i < args.size(); i++) {
      if(!isAssignableTo(constructorParams[i], args[i])) {
         throw diag.ReportError(loc_)
               << "Invalid argument type for constructor call";
      }
   }

   return op.resolveResultType(constructor->getReturnType()->type);
}

Type const* ExprTypeResolver::evalNewArray(NewArrayOp& op, const Type* array,
                                           const Type* size) const {
   if(op.resultType()) return op.resultType();
   Type* copiedType = nullptr;
   if(auto refType = dynamic_cast<const ReferenceType*>(array)) {
      copiedType = sema.BuildReferenceType(refType->decl());
   } else if(auto builtInType = dynamic_cast<const BuiltInType*>(array)) {
      copiedType = sema.BuildBuiltInType(builtInType->getKind());
   }
   assert(copiedType && "Invalid type for base type of array");
   auto arrayType = alloc.new_object<ArrayType>(alloc, copiedType, loc_);

   if(!size->isNumeric()) {
      throw diag.ReportError(loc_) << "Invalid type for array size, non-numeric";
   }

   return op.resolveResultType(arrayType);
}

Type const* ExprTypeResolver::evalArrayAccess(ArrayAccessOp& op, const Type* array,
                                              const Type* index) const {
   if(op.resultType()) return op.resultType();
   auto arrayType = dynamic_cast<const ArrayType*>(array);
   assert(arrayType && "Not an array type");

   if(!index->isNumeric()) {
      throw diag.ReportError(loc_) << "Invalid type for array index, non-numeric";
   }

   return op.resolveResultType(arrayType->getElementType());
}

Type const* ExprTypeResolver::evalCast(CastOp& op, const Type* type,
                                       const Type* value) const {
   if(op.resultType()) return op.resultType();
   if(!isValidCast(value, type)) {
      throw diag.ReportError(loc_) << "Invalid cast from " << value->toString()
                                   << " to " << type->toString();
   }

   return op.resolveResultType(type);
}

} // namespace semantic
