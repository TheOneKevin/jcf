#include "codegen/CGExpr.h"

#include <utility>

#include "ast/Decl.h"
#include "ast/Type.h"
#include "codegen/CodeGen.h"
#include "semantic/NameResolver.h"
#include "tir/Constant.h"
#include "tir/IRBuilder.h"
#include "tir/Instructions.h"
#include "tir/Type.h"
#include "utils/Utils.h"

using namespace tir;
namespace ex = ast::exprnode;
using T = codegen::CGExprEvaluator::T;

/* ===--------------------------------------------------------------------=== */
// Conversion functions in T::
/* ===--------------------------------------------------------------------=== */

tir::Value* T::asRValue(tir::IRBuilder& builder) const {
   assert(kind_ == Kind::L || kind_ == Kind::R);
   auto [_, type, value] = std::get<TirWrapped>(data_);
   if(kind_ == Kind::L) {
      // assert(!type->isPointerType());
      return builder.createLoadInstr(type, value);
   } else {
      return value;
   }
}

tir::Value* T::asLValue() const {
   assert(kind_ == Kind::L);
   return std::get<TirWrapped>(data_).value;
}

tir::Value* T::asFn() const {
   assert(kind_ == Kind::StaticFn || kind_ == Kind::MemberFn);
   return std::get<TirWrapped>(data_).value;
}

ast::Type const* T::astType() const {
   if(kind_ == Kind::L || kind_ == Kind::R) {
      return std::get<TirWrapped>(data_).astType;
   } else if(kind_ == Kind::AstType) {
      return std::get<ast::Type const*>(data_);
   }
   assert(false);
}

ast::Decl const* T::asDecl() const {
   assert(kind_ == Kind::AstDecl);
   return std::get<ast::Decl const*>(data_);
}

tir::Type* T::irType() const {
   assert(kind_ == Kind::L || kind_ == Kind::R);
   return std::get<TirWrapped>(data_).type;
}

bool T::validate(CodeGenerator& cg) const {
   // 1. Check the values are not empty
   switch(kind_) {
      case Kind::L:
      case Kind::R:
         assert(irType() != nullptr);
         assert(std::get<TirWrapped>(data_).value != nullptr);
         break;
      case Kind::StaticFn:
      case Kind::MemberFn:
         assert(std::get<TirWrapped>(data_).value != nullptr);
         break;
      case Kind::AstType:
         assert(std::get<ast::Type const*>(data_) != nullptr);
         break;
      case Kind::AstDecl:
         assert(std::get<ast::Decl const*>(data_) != nullptr);
         break;
   }
   // 2. If the kind is an L/R value, check the AST type agrees
   if(kind_ == Kind::R || kind_ == Kind::L) {
      auto type = std::get<TirWrapped>(data_).type;
      auto astTy = std::get<TirWrapped>(data_).astType;
      assert(type == cg.emitType(astTy));
   }
   return true;
}

/* ===--------------------------------------------------------------------=== */
// CodeGenerator expression evaluator, helper functions first
/* ===--------------------------------------------------------------------=== */

namespace codegen {
T CGExprEvaluator::castIntegerType(ast::Type const* aTy, tir::Type* ty,
                                   T value) const {
   using CastOp = ICastInst::CastOp;
   auto srcAstTy = cast<ast::BuiltInType>(value.astType());
   auto dstBits = cast<tir::IntegerType>(ty)->getBitWidth();
   auto srcBits = srcAstTy->typeSizeBits();
   auto isSrcSigned = srcAstTy->getKind() != ast::BuiltInType::Kind::Char;
   auto isNarrowing = dstBits < srcBits;
   auto isWidening = dstBits > srcBits;
   Instruction* castInst = nullptr;
   // Narrowing == truncation
   if(isNarrowing) {
      castInst = cg.builder.createICastInstr(
            CastOp::Trunc, value.asRValue(cg.builder), ty);
   }
   // Widening == sign extension, if the source is signed
   else if(isWidening && isSrcSigned) {
      castInst = cg.builder.createICastInstr(
            CastOp::SExt, value.asRValue(cg.builder), ty);
   }
   // Widening == zero extension, if the source is unsigned
   else if(isWidening && !isSrcSigned) {
      castInst = cg.builder.createICastInstr(
            CastOp::ZExt, value.asRValue(cg.builder), ty);
   }
   // Identity cast
   else {
      return value;
   }
   return T::R(aTy, castInst);
}
} // namespace codegen

static CmpInst::Predicate getPredicate(ex::BinaryOp::OpType op) {
   using OpType = ex::BinaryOp::OpType;
   switch(op) {
      case OpType::GreaterThan:
         return CmpInst::Predicate::GT;
      case OpType::GreaterThanOrEqual:
         return CmpInst::Predicate::GE;
      case OpType::LessThan:
         return CmpInst::Predicate::LT;
      case OpType::LessThanOrEqual:
         return CmpInst::Predicate::LE;
      case OpType::Equal:
         return CmpInst::Predicate::EQ;
      case OpType::NotEqual:
         return CmpInst::Predicate::NE;
      default:
         assert(false);
   }
   std::unreachable();
}

static Instruction::BinOp getBinOp(ex::BinaryOp::OpType op) {
   using OpType = ex::BinaryOp::OpType;
   switch(op) {
      case OpType::BitwiseAnd:
         return Instruction::BinOp::And;
      case OpType::BitwiseOr:
         return Instruction::BinOp::Or;
      case OpType::BitwiseXor:
         return Instruction::BinOp::Xor;
      case OpType::Add:
         return Instruction::BinOp::Add;
      case OpType::Subtract:
         return Instruction::BinOp::Sub;
      case OpType::Multiply:
         return Instruction::BinOp::Mul;
      case OpType::Divide:
         return Instruction::BinOp::Div;
      case OpType::Modulo:
         return Instruction::BinOp::Rem;
      default:
         assert(false);
   }
   std::unreachable();
}

static auto findArrayField(semantic::NameResolver& nr) {
   for(auto field : nr.GetArrayPrototype()->fields()) {
      if(field->name() == "length") {
         return field;
      }
   }
   assert(false && "Array prototype field not found");
}

/* ===--------------------------------------------------------------------=== */
// Emit specific expressions
/* ===--------------------------------------------------------------------=== */

namespace codegen {

T CGExprEvaluator::mapValue(ex::ExprValue& node) const {
   auto aTy = node.type();
   if(auto memberName = dyn_cast<ex::MemberName>(node)) {
      auto irTy = cg.emitType(cast<ast::TypedDecl>(memberName->decl())->type());
      // 1. If it's a field decl, handle the static and non-static cases
      if(auto* fieldDecl = dyn_cast<ast::FieldDecl>(memberName->decl())) {
         // 1. If it's static, then grab the GV
         if(fieldDecl->modifiers().isStatic()) {
            auto GV = cg.gvMap[fieldDecl];
            return T::L(aTy, irTy, GV);
         }
         // 2. Otherwise we need to wrap it to resolve in MemberAccess
         else {
            return T{fieldDecl};
         }
      }
      // 2. If it's a local (var) decl, grab the alloca inst
      else {
         auto* localDecl = cast<ast::VarDecl>(memberName->decl());
         return T::L(aTy, irTy, cg.valueMap[localDecl]);
      }
   } else if(auto thisNode = dyn_cast<ex::ThisNode>(node)) {
      // "this" will be the first argument of the function
      return T::L(aTy, cg.emitType(aTy), curFn.args().front());
   } else if(auto methodName = dyn_cast<ex::MethodName>(node)) {
      auto* methodDecl = cast<ast::MethodDecl>(methodName->decl());
      // 1. If it's static, then grab the GV
      if(methodDecl->modifiers().isStatic()) {
         auto fn = cg.gvMap[methodDecl];
         return T::Fn(T::Kind::StaticFn, fn);
      }
      // 2. Otherwise... it's complicated
      else {
         assert(false && "Not implemented yet");
      }
   } else if(auto literal = dyn_cast<ex::LiteralNode>(node)) {
      if(literal->builtinType()->isNumeric()) {
         return T::R(aTy, Constant::CreateInt32(ctx, literal->getAsInt()));
      } else if(literal->builtinType()->isBoolean()) {
         return T::R(aTy, Constant::CreateBool(ctx, literal->getAsInt()));
      } else if(literal->builtinType()->isString()) {
         // TODO: String type
         return T::L(aTy, Type::getPointerTy(ctx), Constant::CreateNullPointer(ctx));
      } else {
         // Null type
         return T::R(aTy, Constant::CreateNullPointer(ctx));
      }
   } else if(auto type = dyn_cast<ex::TypeNode>(node)) {
      return T{aTy};
   }
   std::unreachable();
}

T CGExprEvaluator::evalBinaryOp(ex::BinaryOp& op, T lhs, T rhs) const {
   using OpType = ex::BinaryOp::OpType;
   auto aTy = op.resultType();
   switch(op.opType()) {
      // Assignment expression //
      case OpType::Assignment:
         cg.builder.createStoreInstr(rhs.asRValue(cg.builder), lhs.asLValue());
         return lhs;

      // Comparison expressions //
      case OpType::GreaterThan:
      case OpType::GreaterThanOrEqual:
      case OpType::LessThan:
      case OpType::LessThanOrEqual:
      case OpType::Equal:
      case OpType::NotEqual: {
         auto inst = cg.builder.createCmpInstr(getPredicate(op.opType()),
                                               lhs.asRValue(cg.builder),
                                               rhs.asRValue(cg.builder));
         return T::R(aTy, inst);
      }

      // Short circuit expressions //
      case OpType::And: {
         /*
            curBB:
               %v0 = i1 eval(lhs)
               store i1 %v0, %tmp
               br i1 %v0, bb1, bb2
            bb1:
               %v1 = i1 eval(rhs)
               store i1 %v1, %tmp
               br bb2
            bb2:
               %tmp as lvalue
         */
         auto tmp = cg.curFn->createAlloca(Type::getInt1Ty(ctx));
         auto bb1 = cg.builder.createBasicBlock(&curFn);
         auto bb2 = cg.builder.createBasicBlock(&curFn);
         bb1->setName("and.true");
         bb2->setName("and.false");
         auto v0 = lhs.asRValue(cg.builder);
         cg.builder.createStoreInstr(v0, tmp);
         cg.builder.createBranchInstr(v0, bb1, bb2);
         cg.builder.setInsertPoint(bb1);
         auto v1 = rhs.asRValue(cg.builder);
         cg.builder.createStoreInstr(v1, tmp);
         cg.builder.createBranchInstr(bb2);
         cg.builder.setInsertPoint(bb2);
         return T::L(aTy, Type::getInt1Ty(ctx), tmp);
      }
      case OpType::Or: {
         /*
            curBB:
               v0 = i1 eval(lhs)
               store i1 %v0, %tmp
               br i1 %v0, bb2, bb1
            bb1:
               v1 = i1 eval(rhs)
               store i1 %v1, %tmp
               br bb2
            bb2:
               %tmp as lvalue
         */
         auto tmp = cg.curFn->createAlloca(Type::getInt1Ty(ctx));
         auto bb1 = cg.builder.createBasicBlock(&curFn);
         auto bb2 = cg.builder.createBasicBlock(&curFn);
         bb1->setName("or.true");
         bb2->setName("or.false");
         auto v0 = lhs.asRValue(cg.builder);
         cg.builder.createStoreInstr(v0, tmp);
         cg.builder.createBranchInstr(v0, bb2, bb1);
         cg.builder.setInsertPoint(bb1);
         auto v1 = rhs.asRValue(cg.builder);
         cg.builder.createStoreInstr(v1, tmp);
         cg.builder.createBranchInstr(bb2);
         cg.builder.setInsertPoint(bb2);
         return T::L(aTy, Type::getInt1Ty(ctx), tmp);
      }

      // Arithmetic expressions //
      case OpType::BitwiseAnd:
      case OpType::BitwiseOr:
      case OpType::BitwiseXor:
      case OpType::Add:
      case OpType::Subtract:
      case OpType::Multiply:
      case OpType::Divide:
      case OpType::Modulo: {
         // 1. Promote the operands to i32
         auto lhsP = castIntegerType(aTy, Type::getInt32Ty(ctx), lhs);
         auto rhsP = castIntegerType(aTy, Type::getInt32Ty(ctx), rhs);
         // 2. Compute in i32
         auto res = cg.builder.createBinaryInstr(getBinOp(op.opType()),
                                                 lhsP.asRValue(cg.builder),
                                                 rhsP.asRValue(cg.builder));
         // 3. Narrow back to aTy implicitly
         auto emittedTy = cg.emitType(aTy);
         assert(res->type() == emittedTy);
         return castIntegerType(aTy, emittedTy, T::R(aTy, res));
      }

      // Instance of expression //
      case OpType::InstanceOf: {
         // TODO(kevin): Implement
         return T::R(aTy, Constant::CreateBool(ctx, false));
      }

      default:
         break;
   }
   std::unreachable();
}

T CGExprEvaluator::evalUnaryOp(ex::UnaryOp& op, T rhs) const {
   using BinOp = Instruction::BinOp;
   using OpType = ex::UnaryOp::OpType;
   auto aTy = op.resultType();
   auto value = rhs.asRValue(cg.builder);
   auto ty = value->type();
   switch(op.opType()) {
      case OpType::Not:
      case OpType::BitwiseNot: {
         // We actually don't need to promote the operand here
         auto allOnes = ConstantInt::AllOnes(ctx, ty);
         auto instr = cg.builder.createBinaryInstr(BinOp::Xor, value, allOnes);
         return T::R(aTy, instr);
      }
      case OpType::Plus: {
         // Do nothing for unary plus
         return rhs;
      }
      case OpType::Minus: {
         // We also don't need to promote the operand here either
         auto instr = cg.builder.createBinaryInstr(
               BinOp::Sub, ConstantInt::Zero(ctx, ty), value);
         return T::R(aTy, instr);
      }
      default:
         break;
   }
   std::unreachable();
}

T CGExprEvaluator::evalMemberAccess(ex::MemberAccess& op, T lhs, T field) const {
   auto aTy = op.resultType();
   auto obj = lhs.asRValue(cg.builder);
   auto decl = field.asDecl();
   // Special case: array.length
   if(decl == findArrayField(cg.nr)) {
      auto arrTy = cast<StructType>(lhs.irType());
      auto arrSzGep = cg.builder.createGEPInstr(obj, arrTy, {Constant::CreateInt32(ctx, 0)});
      auto arrSz = cg.builder.createLoadInstr(Type::getInt32Ty(ctx), arrSzGep);
      return T::R(aTy, arrSz);
   }
   assert(false);
}

T CGExprEvaluator::evalMethodCall(ex::MethodInvocation& op, T method,
                                  const op_array& args) const {
   auto aTy = op.resultType();
   // Check if the method is static or not
   if(method.kind() == T::Kind::StaticFn) {
      std::vector<Value*> argValues;
      for(auto& arg : args) {
         argValues.push_back(arg.asRValue(cg.builder));
      }
      auto callVal = cg.builder.createCallInstr(method.asFn(), argValues);
      return T::R(aTy, callVal);
   }
   // TODO: Implement logic for member function call
   assert(false);
}

T CGExprEvaluator::evalNewObject(ex::ClassInstanceCreation& op, T object,
                                 const op_array& args) const {
   (void) op;
   (void) object;
   (void) args;
   // TODO: Implement this
   return T::L(op.resultType(), Type::getPointerTy(ctx), Constant::CreateNullPointer(ctx));
}

T CGExprEvaluator::evalNewArray(ex::ArrayInstanceCreation& op, T type,
                                T size) const {
   auto aTy = op.resultType();
   auto arrTy = cast<StructType>(cg.emitType(aTy));
   auto elemTy = cg.emitType(type.astType());
   auto arrLength = castIntegerType(nullptr, Type::getInt32Ty(ctx), size)
                          .asRValue(cg.builder);
   auto totalSz = cg.builder.createBinaryInstr(
         Instruction::BinOp::Mul,
         arrLength,
         Constant::CreateInt32(ctx, elemTy->getSizeInBits() / 8));
   auto arrPtr = cg.builder.createCallInstr(cu.builtinMalloc(), {totalSz});
   auto alloca = curFn.createAlloca(cg.emitType(op.resultType()));
   auto gepSz =
         cg.builder.createGEPInstr(arrPtr, arrTy, {Constant::CreateInt32(ctx, 0)});
   auto gepPtr =
         cg.builder.createGEPInstr(arrPtr, arrTy, {Constant::CreateInt32(ctx, 1)});
   cg.builder.createStoreInstr(arrLength, gepSz);
   cg.builder.createStoreInstr(arrPtr, gepPtr);
   auto loadArr = cg.builder.createLoadInstr(arrTy, arrPtr);
   cg.builder.createStoreInstr(loadArr, alloca);
   totalSz->setName("arr.sz");
   arrPtr->setName("arr.ptr");
   alloca->setName("arr.alloca");
   gepSz->setName("arr.gep.sz");
   gepPtr->setName("arr.gep.ptr");
   return T::L(aTy, arrTy, alloca);
}

T CGExprEvaluator::evalArrayAccess(ex::ArrayAccess& op, T array, T index) const {
   auto arrAlloca = array.asLValue();
   auto elemAstTy = op.resultType();
   auto arrTy = cast<StructType>(array.irType());
   auto arrSzGep = cg.builder.createGEPInstr(
         arrAlloca, arrTy, {Constant::CreateInt32(ctx, 0)});
   auto arrPtrGep = cg.builder.createGEPInstr(
         arrAlloca, arrTy, {Constant::CreateInt32(ctx, 1)});
   auto arrSz = cg.builder.createLoadInstr(Type::getInt32Ty(ctx), arrSzGep);
   arrSz->setName("arr.sz");
   auto arrPtr = cg.builder.createLoadInstr(Type::getPointerTy(ctx), arrPtrGep);
   arrPtr->setName("arr.ptr");
   auto idxVal = index.asRValue(cg.builder);
   auto lengthValid =
         cg.builder.createCmpInstr(CmpInst::Predicate::LT, idxVal, arrSz);
   auto bb1 = cg.builder.createBasicBlock(&curFn);
   bb1->setName("array.oob");
   auto bb2 = cg.builder.createBasicBlock(&curFn);
   bb2->setName("array.inbounds");
   cg.builder.createBranchInstr(lengthValid, bb2, bb1);
   cg.builder.setInsertPoint(bb1);
   cg.builder.createCallInstr(cu.builtinException(), {});
   cg.builder.createBranchInstr(bb2);
   cg.builder.setInsertPoint(bb2);
   auto elemPtr = cg.builder.createGEPInstr(arrPtr, arrTy, {idxVal});
   return T::L(elemAstTy, cg.emitType(elemAstTy), elemPtr);
}

T CGExprEvaluator::evalCast(ex::Cast& op, T type, T value) const {
   auto aTy = op.resultType();
   auto castType = type.astType();
   if(castType->isNumeric()) {
      // Convert either promotion or narrowing
      return castIntegerType(aTy, cg.emitType(castType), value);
   } else if(castType->isBoolean()) {
      // Booleans must be identity conversion
      return value;
   } else if(castType->isString()) {
   } else if(castType->isArray()) {
   } else {
   }
   assert(false);
}

} // namespace codegen

/* ===--------------------------------------------------------------------=== */
// CodeGenerator emit router
/* ===--------------------------------------------------------------------=== */

namespace codegen {

Value* CodeGenerator::emitExpr(ast::Expr const* expr) {
   CGExprEvaluator eval{*this};
   T result = eval.EvaluateList(expr->list());
   return result.asRValue(builder);
}

} // namespace codegen
