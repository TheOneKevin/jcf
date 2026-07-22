#include "codegen/CGExpr.h"

#include <utility>
#include <vector>

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
using II = Instruction::IntrinsicKind;

/* ===--------------------------------------------------------------------=== */
// Conversion functions in T::
/* ===--------------------------------------------------------------------=== */

tir::Value* T::asRValue(tir::IRBuilder& builder) const {
   assert(kind_ == Kind::L || kind_ == Kind::R);
   auto [_, type, value] = std::get<TirWrapped>(data_);
   if(kind_ == Kind::L) {
      assert(!type->isPointerType() || isAstTypeReference(astType()));
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
   return std::get<FnWrapped>(data_).fn;
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
   if(kind_ == Kind::AstDecl)
      return std::get<ast::Decl const*>(data_);
   else if(kind_ == Kind::StaticFn || kind_ == Kind::MemberFn)
      return std::get<FnWrapped>(data_).decl;
   assert(false);
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
         assert(std::get<FnWrapped>(data_).fn != nullptr);
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

void T::dump() const {
   switch(kind_) {
      case Kind::L:
         std::cout << "L-value: ";
         std::get<TirWrapped>(data_).value->dump();
         break;
      case Kind::R:
         std::cout << "R-value: ";
         std::get<TirWrapped>(data_).value->dump();
         break;
      case Kind::StaticFn:
         std::cout << "Static function: ";
         asFn()->dump();
         break;
      case Kind::MemberFn:
         std::cout << "Member function: ";
         asFn()->dump();
         break;
      case Kind::AstType:
         std::cout << "AST type: ";
         astType()->dump();
         break;
      case Kind::AstDecl:
         std::cout << "AST decl: ";
         asDecl()->dump();
         break;
   }
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
   if(auto methodName = dyn_cast<ex::MethodName>(node)) {
      auto* methodDecl = cast<ast::MethodDecl>(methodName->decl());
      auto kind = methodDecl->modifiers().isStatic() ? T::Kind::StaticFn
                                                     : T::Kind::MemberFn;
      auto fn = cg.gvMap[methodDecl];
      // For an unqualified instance-method call the receiver is the implicit
      // `this`; a qualified call (obj.m()) overrides this in evalMemberAccess.
      // In a static context cg.curThis_ is null (the call must be qualified).
      tir::Value* refThis = (kind == T::Kind::MemberFn) ? cg.curThis_ : nullptr;
      return T::Fn(kind, methodDecl, fn, refThis);
   } else if(auto memberName = dyn_cast<ex::MemberName>(node)) {
      auto irTy = cg.emitType(cast<ast::TypedDecl>(memberName->decl())->type());
      // 1. If it's a field decl, handle the static and non-static cases
      if(auto* fieldDecl = dyn_cast<ast::FieldDecl>(memberName->decl())) {
         // a) If it's static, then grab the GV
         if(fieldDecl->modifiers().isStatic()) {
            auto GV = cg.gvMap[fieldDecl];
            return T::L(aTy, irTy, GV);
         }
         // b) Otherwise we need to wrap it to resolve in MemberAccess
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
      // "this" is the object pointer (arg 0). It is an r-value: using `this`
      // yields the pointer itself, not a load from it.
      return T::R(aTy, cg.curThis_);
   } else if(auto literal = dyn_cast<ex::LiteralNode>(node)) {
      if(literal->builtinType()->isNumeric()) {
         auto bits = static_cast<uint8_t>(literal->builtinType()->typeSizeBits());
         auto val = literal->getAsInt();
         return T::R(aTy, Constant::CreateInt(ctx, bits, val));
      } else if(literal->builtinType()->isBoolean()) {
         return T::R(aTy, Constant::CreateBool(ctx, literal->getAsInt()));
      } else if(literal->builtinType()->isString()) {
         return emitStringLiteral(literal->getAsString(), aTy);
      } else {
         // Null type
         return T::R(aTy, Constant::CreateNullPointer(ctx));
      }
   } else if(auto type = dyn_cast<ex::TypeNode>(node)) {
      // For `new C(...)` the resolver stores the chosen constructor on the type
      // node; hand it to evalNewObject as a function wrapper. Other uses of a
      // type node (casts, array element types) carry only the AST type.
      if(auto* ctor = dyn_cast_or_null<ast::MethodDecl>(type->decl())) {
         if(ctor->isConstructor())
            return T::Fn(T::Kind::StaticFn, ctor, cg.gvMap[ctor]);
      }
      return T{aTy};
   }
   std::unreachable();
}

T CGExprEvaluator::materialize(T v) const {
   if(v.kind() != T::Kind::AstDecl) return v;
   auto* fieldDecl = dyn_cast<ast::FieldDecl>(v.asDecl());
   if(!fieldDecl || fieldDecl->modifiers().isStatic()) return v;
   // A bare instance field is an implicit `this.field` access.
   return emitFieldAccess(cg.curThis_, fieldDecl, fieldDecl->type());
}

T CGExprEvaluator::emitFieldAccess(tir::Value* objPtr,
                                   ast::FieldDecl const* field,
                                   ast::Type const* resultAstTy) const {
   // Field slots are stable across the hierarchy (Phase 1b), so the declaring
   // class's struct type yields the correct offset for any receiver whose
   // runtime class derives from it.
   auto* cls = cast<ast::ClassDecl>(field->parent());
   auto* structTy = cg.typeMap[cls];
   auto idx = static_cast<unsigned>(cg.fieldIndexMap.at(field));
   auto* gep = cg.builder.createGEPInstr(objPtr, structTy, {idx});
   return T::L(resultAstTy, cg.emitType(field->type()), gep);
}

tir::Value* CGExprEvaluator::toRValue(T v) const {
   return materialize(v).asRValue(cg.builder);
}

tir::Value* CGExprEvaluator::allocObject(ast::ClassDecl const* cls) const {
   auto* structTy = cg.typeMap[cls];
   auto szBytes = (structTy->getSizeInBits() + 1) / 8;
   auto* objPtr = cg.builder.createIntrinsicCallInstr(
         II::malloc, {Constant::CreateInt32(ctx, szBytes)});
   // Store the class's vtable pointer into field 0 so virtual dispatch works.
   if(cg.vtableMap.count(cls)) {
      auto* vtGep = cg.builder.createGEPInstr(objPtr, structTy, {0u});
      cg.builder.createStoreInstr(cg.vtableMap[cls], vtGep);
   }
   return objPtr;
}

T CGExprEvaluator::emitStringLiteral(std::string_view utf8,
                                     ast::Type const* aTy) const {
   // Build a char[] holding the literal's code units, then wrap it in a
   // java.lang.String via its String(char[]) constructor.
   auto* charTy = Type::getInt16Ty(ctx);
   auto n = static_cast<uint32_t>(utf8.size());
   // 1. Allocate and fill the backing char buffer.
   auto* dataPtr = cg.builder.createIntrinsicCallInstr(
         II::malloc, {Constant::CreateInt32(ctx, n * 2)});
   auto* charArrTy = ArrayType::get(ctx, charTy, 0);
   for(uint32_t i = 0; i < n; ++i) {
      auto* elem = cg.builder.createGEPInstr(dataPtr, charArrTy, {i});
      cg.builder.createStoreInstr(
            Constant::CreateInt(ctx, 16, static_cast<uint8_t>(utf8[i])), elem);
   }
   // 2. Allocate the array struct { i32 length; ptr data }.
   auto szBytes = (cg.arrayType_->getSizeInBits() + 1) / 8;
   auto* arrStruct = cg.builder.createIntrinsicCallInstr(
         II::malloc, {Constant::CreateInt32(ctx, szBytes)});
   cg.emitSetArrayPtr(arrStruct, dataPtr);
   cg.emitSetArraySz(arrStruct, Constant::CreateInt32(ctx, n));
   // 3. Construct a String from the char[].
   auto* stringCls = cg.nr.GetJavaLang().String;
   ast::MethodDecl const* ctor = nullptr;
   for(auto* c : stringCls->constructors()) {
      if(c->parameters().size() == 1 &&
         c->parameters().front()->type()->isArray()) {
         ctor = c;
         break;
      }
   }
   assert(ctor && "java.lang.String is missing a String(char[]) constructor");
   auto* objPtr = allocObject(stringCls);
   std::vector<tir::Value*> ctorArgs{objPtr, arrStruct};
   cg.builder.createCallInstr(cast<tir::Function>(cg.gvMap[ctor]), ctorArgs);
   return T::R(aTy, objPtr);
}

T CGExprEvaluator::evalBinaryOp(ex::BinaryOp& op, T lhs, T rhs) const {
   lhs = materialize(lhs);
   rhs = materialize(rhs);
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
   rhs = materialize(rhs);
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
   // The receiver may itself be a bare instance field (e.g. `field.x`).
   lhs = materialize(lhs);
   auto obj = lhs.asRValue(cg.builder);
   auto decl = field.asDecl();
   // Special case: "field" is actually a function. Bind the receiver as `this`.
   if(field.kind() == T::Kind::MemberFn) {
      return T::Fn(T::Kind::MemberFn, decl, field.asFn(), obj);
   }
   // Special case: array.length
   else if(decl == findArrayField(cg.nr)) {
      auto arrSzGep = cg.builder.createGEPInstr(obj, cg.arrayType(), {0});
      auto arrSz = cg.builder.createLoadInstr(Type::getInt32Ty(ctx), arrSzGep);
      return T::R(aTy, arrSz);
   }
   // Instance field access: obj.field. Static fields are resolved directly to
   // their global in mapValue and never reach here.
   else {
      auto* fieldDecl = cast<ast::FieldDecl>(decl);
      return emitFieldAccess(obj, fieldDecl, aTy);
   }
}

T CGExprEvaluator::evalMethodCall(ex::MethodInvocation& op, T method,
                                  const op_array& args) const {
   auto aTy = op.resultType();
   auto* methodDecl = cast<ast::MethodDecl>(method.asDecl());
   std::vector<Value*> argValues;
   // If this is a member function, push back an extra "this"
   tir::Value* recv = nullptr;
   if(method.kind() == T::Kind::MemberFn) {
      assert(method.thisRef());
      recv = method.thisRef();
      argValues.push_back(recv);
   } else {
      assert(method.kind() == T::Kind::StaticFn);
   }
   // Now we can push back the arguments
   for(auto& arg : args) {
      argValues.push_back(materialize(arg).asRValue(cg.builder));
   }
   // Virtual dispatch: instance methods that were assigned a vtable slot
   // (public/protected, non-final overridables) are dispatched through the
   // receiver's vtable. Static, private and constructor calls bind directly.
   if(recv && cg.vtableIndexMap.count(methodDecl)) {
      // 1. Load the vtable pointer from field 0 of the object. Field 0 is the
      //    vtable pointer in every object layout, so the declaring class's
      //    struct type gives the right (zero) offset for any subclass receiver.
      auto* objStructTy = cg.typeMap[cast<ast::ClassDecl>(methodDecl->parent())];
      auto* vptrGep = cg.builder.createGEPInstr(recv, objStructTy, {0u});
      auto* vptr = cg.builder.createLoadInstr(Type::getPointerTy(ctx), vptrGep);
      // 2. Load the function pointer from the method's vtable slot.
      auto slot = static_cast<unsigned>(cg.vtableIndexMap[methodDecl]);
      auto* slotGep =
            cg.builder.createGEPInstr(vptr, cg.vtableTypeUniform_, {slot});
      auto* fnPtr = cg.builder.createLoadInstr(Type::getPointerTy(ctx), slotGep);
      // 3. Call indirectly through the loaded function pointer.
      auto* retTy = cg.emitType(methodDecl->returnTy().type);
      auto* callVal = cg.builder.createCallInstr(retTy, fnPtr, argValues);
      return T::R(aTy, callVal);
   }
   auto callVal = cg.builder.createCallInstr(method.asFn(), argValues);
   return T::R(aTy, callVal);
}

T CGExprEvaluator::evalNewObject(ex::ClassInstanceCreation& op, T object,
                                 const op_array& args) const {
   auto aTy = op.resultType();
   auto* cls = cast<ast::ClassDecl>(aTy->getAsDecl());
   // Allocate the object and set its vtable pointer.
   auto* objPtr = allocObject(cls);
   // Call the constructor (threaded through `object` by mapValue) with `this`.
   std::vector<Value*> callArgs;
   callArgs.push_back(objPtr);
   for(auto& arg : args)
      callArgs.push_back(materialize(arg).asRValue(cg.builder));
   cg.builder.createCallInstr(object.asFn(), callArgs);
   return T::R(aTy, objPtr);
}

T CGExprEvaluator::evalNewArray(ex::ArrayInstanceCreation& op, T type,
                                T size) const {
   size = materialize(size);
   // This is the AST type of the array elements
   auto aTy = op.resultType();
   // This is the type of the array elements
   auto T = cg.emitType(cast<ast::ArrayType>(type.astType())->getElementType());
   // Get the number of elements in the array
   auto N = castIntegerType(nullptr, Type::getInt32Ty(ctx), size)
                  .asRValue(cg.builder);
   // Size in bytes = N * sizeof(T)
   auto totalSz = cg.builder.createBinaryInstr(
         Instruction::BinOp::Mul,
         N,
         Constant::CreateInt32(ctx, (T->getSizeInBits() + 1) / 8));
   // T[] arr = new T[N * sizeof(T)];
   auto arrPtr = cg.builder.createIntrinsicCallInstr(II::malloc, {totalSz});
   // Array* arrStructPtr = (Array*) malloc(sizeof(Array))
   auto arrStructPtr = cg.builder.createIntrinsicCallInstr(
         II::malloc,
         {Constant::CreateInt32(ctx, (cg.arrayType_->getSizeInBits() + 1) / 8)});
   // arrStructPtr->ptr = arrPtr
   cg.emitSetArrayPtr(arrStructPtr, arrPtr);
   // arrStructPtr->sz = N
   cg.emitSetArraySz(arrStructPtr, N);
   // NOTE: We can just return an R-value without an alloca (LV) because we
   // don't support ternary expressions, so no PHI nodes are needed either.
   return T::R(aTy, arrStructPtr);
}

T CGExprEvaluator::evalArrayAccess(ex::ArrayAccess& op, T array, T index) const {
   array = materialize(array);
   index = materialize(index);
   // Build and check null pointer access
   auto arrStructPtr = array.asRValue(cg.builder);
   cg.builder.createIntrinsicCallInstr(II::check_null, {arrStructPtr});
   // Build and assert idxVal is i32 or less and promote if necessary
   auto idxVal = index.asRValue(cg.builder);
   assert(idxVal->type()->isIntegerType());
   const auto ptrWidth = static_cast<uint32_t>(ctx.TI().getPointerSizeInBits());
   assert(idxVal->type()->getSizeInBits() <= ptrWidth);
   if(idxVal->type()->getSizeInBits() < ptrWidth) {
      idxVal = cg.builder.createICastInstr(
            ICastInst::CastOp::ZExt, idxVal, IntegerType::get(ctx, ptrWidth));
   }
   // Check for out-of-bounds access
   cg.builder.createIntrinsicCallInstr(II::check_array_bounds,
                                       {arrStructPtr, idxVal});
   // Build the array access itself
   auto elemAstTy = op.resultType();
   auto elemTy = cg.emitType(elemAstTy);
   auto arrPtr = cg.emitGetArrayPtr(arrStructPtr);
   auto elemPtr = cg.builder.createGEPInstr(
         arrPtr, ArrayType::get(ctx, elemTy, 0), {idxVal});
   return T::L(elemAstTy, elemTy, elemPtr);
}

T CGExprEvaluator::evalCast(ex::Cast& op, T type, T value) const {
   value = materialize(value);
   auto aTy = op.resultType();
   auto castType = type.astType();
   if(castType->isNumeric()) {
      // Convert either promotion or narrowing
      return castIntegerType(aTy, cg.emitType(castType), value);
   } else if(castType->isBoolean()) {
      // Booleans must be identity conversion
      return value;
   } else {
      // Reference, array and string casts are pointer-preserving at the IR
      // level (Joos runtime cast checks are not emitted here). Re-wrap the
      // pointer with the target type.
      return T::R(aTy, value.asRValue(cg.builder));
   }
}

} // namespace codegen

/* ===--------------------------------------------------------------------=== */
// CodeGenerator emit router
/* ===--------------------------------------------------------------------=== */

namespace codegen {

Value* CodeGenerator::emitExpr(ast::Expr const* expr) {
   CGExprEvaluator eval{*this};
   T result = eval.EvaluateList(expr->list());
   return eval.toRValue(result);
}

} // namespace codegen
