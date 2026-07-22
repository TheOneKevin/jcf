#include "tir/Instructions.h"

#include "tir/BasicBlock.h"
#include "tir/Constant.h"
#include "tir/TIR.h"

namespace tir {

static std::ostream& printNameOrConst(std::ostream& os, Value const* val) {
   if(val->isConstant()) {
      os << *val;
   } else {
      if(val->type()->isLabelType())
         os << "^";
      else
         os << "%";
      val->printName(os);
      if(!val->type()->isLabelType()) os << ":" << *val->type();
   }
   return os;
}

static std::ostream& printSelf(std::ostream& os, Value const* val) {
   os << "%";
   val->printName(os);
   return os;
}

/* ===--------------------------------------------------------------------=== */
// Register all intrinsic instructions
/* ===--------------------------------------------------------------------=== */

void RegisterAllIntrinsics(CompilationUnit& CU) {
   using I = Instruction::IntrinsicKind;
   using FTy = FunctionType;
   using T = Type;
   auto& ctx = CU.ctx();
   auto PtrTy = T::getPointerTy(ctx);
   auto VoidTy = T::getVoidTy(ctx);
   auto i32Ty = T::getInt32Ty(ctx);

   // @jcf.malloc(i32) -> ptr*
   CU.CreateIntrinsic(I::malloc, FTy::get(ctx, PtrTy, {i32Ty}));
   // @jcf.exception() -> void
   CU.CreateIntrinsic(I::exception, FTy::get(ctx, VoidTy, {}));
   // @jcf.check.array_bounds(ptr*, i32) -> void
   CU.CreateIntrinsic(I::check_array_bounds,
                      FTy::get(ctx, VoidTy, {PtrTy, i32Ty}));
   // @jcf.check.null(ptr*) -> void
   CU.CreateIntrinsic(I::check_null, FTy::get(ctx, VoidTy, {PtrTy}));
}

/* ===--------------------------------------------------------------------=== */
// BranchInst implementation
/* ===--------------------------------------------------------------------=== */

BranchInst::BranchInst(Context& ctx, Value* cond, BasicBlock* trueBB,
                       BasicBlock* falseBB)
      : Instruction{ctx, Type::getVoidTy(ctx)} {
   addChild(cond);
   addChild(trueBB);
   addChild(falseBB);
   static_assert(sizeof(BranchInst) == sizeof(Instruction));
}

std::ostream& BranchInst::print(std::ostream& os) const {
   os << "br ";
   printNameOrConst(os, getChild(0)) << ", ";
   printNameOrConst(os, getChild(1)) << ", ";
   printNameOrConst(os, getChild(2));
   return os;
}

BasicBlock* BranchInst::getSuccessor(unsigned idx) const {
   assert(idx < 2 && "BranchInst::getSuccessor index out of bounds");
   return cast<BasicBlock>(getChild(idx + 1));
}

/* ===--------------------------------------------------------------------=== */
// ReturnInst implementation
/* ===--------------------------------------------------------------------=== */

ReturnInst::ReturnInst(Context& ctx, Value* ret)
      : Instruction{ctx, ret ? ret->type() : Type::getVoidTy(ctx)} {
   if(ret) addChild(ret);
   static_assert(sizeof(ReturnInst) == sizeof(Instruction));
}

std::ostream& ReturnInst::print(std::ostream& os) const {
   os << "ret";
   if(numChildren() > 0) {
      os << " ";
      printNameOrConst(os, getChild(0));
   }
   return os;
}

/* ===--------------------------------------------------------------------=== */
// StoreInst implementation
/* ===--------------------------------------------------------------------=== */

StoreInst::StoreInst(Context& ctx, Value* val, Value* ptr)
      : Instruction{ctx, Type::getVoidTy(ctx)} {
   addChild(val);
   addChild(ptr);
   static_assert(sizeof(StoreInst) == sizeof(Instruction));
}

std::ostream& StoreInst::print(std::ostream& os) const {
   os << "store ";
   printNameOrConst(os, getChild(0)) << " to ";
   printNameOrConst(os, getChild(1));
   return os;
}

/* ===--------------------------------------------------------------------=== */
// LoadInst implementation
/* ===--------------------------------------------------------------------=== */

LoadInst::LoadInst(Context& ctx, Type* type, Value* ptr) : Instruction{ctx, type} {
   addChild(ptr);
   static_assert(sizeof(LoadInst) == sizeof(Instruction));
}

std::ostream& LoadInst::print(std::ostream& os) const {
   printSelf(os, this) << " = ";
   os << "load:" << *type() << " ";
   printNameOrConst(os, getChild(0));
   return os;
}

/* ===--------------------------------------------------------------------=== */
// CallInst implementation
/* ===--------------------------------------------------------------------=== */

CallInst::CallInst(Context& ctx, Value* callee, utils::range_ref<Value*> args)
      : Instruction{ctx, cast<FunctionType>(callee->type())->getReturnType()} {
   addChild(callee);
   args.for_each([this](Value* arg) { addChild(arg); });
   static_assert(sizeof(CallInst) == sizeof(Instruction));
}

CallInst::CallInst(Context& ctx, Type* retTy, Value* calleePtr,
                   utils::range_ref<Value*> args)
      : Instruction{ctx, retTy} {
   addChild(calleePtr);
   args.for_each([this](Value* arg) { addChild(arg); });
}

std::ostream& CallInst::print(std::ostream& os) const {
   if(!type()->isVoidType()) printSelf(os, this) << " = ";
   os << "call";
   if(!type()->isVoidType()) os << ":" << *type();
   os << " ";
   // Direct calls name the callee (@fn); indirect calls print the callee value.
   if(auto* callee = getCallee())
      os << "@" << callee->name();
   else
      printNameOrConst(os, getChild(0));
   os << "(";
   for(unsigned i = 1; i < numChildren(); ++i) {
      printNameOrConst(os, getChild(i));
      if(i != numChildren() - 1) os << ", ";
   }
   os << ")";
   if(isTerminator()) os << " noreturn";
   return os;
}

Function* CallInst::getCallee() const { return dyn_cast<Function>(getChild(0)); }

utils::Generator<Value*> CallInst::args() const {
   for(unsigned i = 1; i < numChildren(); ++i) {
      co_yield getChild(i);
   }
}

bool CallInst::isTerminator() const {
   auto* callee = getCallee();
   return callee && callee->attrs().noreturn;
}

/* ===--------------------------------------------------------------------=== */
// BinaryInst implementation
/* ===--------------------------------------------------------------------=== */

BinaryInst::BinaryInst(Context& ctx, BinOp binop, Value* lhs, Value* rhs)
      : Instruction{ctx, lhs->type(), binop} {
   addChild(lhs);
   addChild(rhs);
   static_assert(sizeof(BinaryInst) == sizeof(Instruction));
}

std::ostream& BinaryInst::print(std::ostream& os) const {
   printSelf(os, this) << " = ";
   const char* name = BinOp_to_string(binop(), "unknown");
   // Print name lower case
   for(unsigned i = 0; name[i] != '\0'; ++i)
      os << static_cast<char>(tolower(name[i]));
   // Print operands
   os << ":" << *type() << " ";
   printNameOrConst(os, getChild(0)) << ", ";
   printNameOrConst(os, getChild(1));
   return os;
}

/* ===--------------------------------------------------------------------=== */
// CmpInst implementation
/* ===--------------------------------------------------------------------=== */

CmpInst::CmpInst(Context& ctx, Predicate pred, Value* lhs, Value* rhs)
      : Instruction{ctx, Type::getInt1Ty(ctx), pred} {
   addChild(lhs);
   addChild(rhs);
   static_assert(sizeof(CmpInst) == sizeof(Instruction));
}

std::ostream& CmpInst::print(std::ostream& os) const {
   printSelf(os, this) << " = cmp:" << *type() << " ";
   auto pred = get<Predicate>();
   const char* name = Predicate_to_string(pred, "unknown");
   // Print name lower case
   for(unsigned i = 0; name[i] != '\0'; ++i)
      os << static_cast<char>(tolower(name[i]));
   // Print operands
   os << " ";
   printNameOrConst(os, getChild(0)) << ", ";
   printNameOrConst(os, getChild(1));
   return os;
}

/* ===--------------------------------------------------------------------=== */
// AllocaInst implementation
/* ===--------------------------------------------------------------------=== */

AllocaInst::AllocaInst(Context& ctx, Type* type)
      : Instruction{ctx, Type::getPointerTy(ctx), type} {
   static_assert(sizeof(AllocaInst) == sizeof(Instruction));
}

std::ostream& AllocaInst::print(std::ostream& os) const {
   auto allocType_ = get<Type*>();
   return printSelf(os, this) << " = " << "alloca type " << *allocType_;
}

/* ===--------------------------------------------------------------------=== */
// ICastInst implementation
/* ===--------------------------------------------------------------------=== */

ICastInst::ICastInst(Context& ctx, CastOp op, Value* val, Type* destTy)
      : Instruction{ctx, destTy, op} {
   addChild(val);
   static_assert(sizeof(ICastInst) == sizeof(Instruction));
}

std::ostream& ICastInst::print(std::ostream& os) const {
   printSelf(os, this) << " = ";
   os << "icast ";
   const char* name = CastOp_to_string(castop(), "unknown");
   // Print name lower case
   for(unsigned i = 0; name[i] != '\0'; ++i)
      os << static_cast<char>(tolower(name[i]));
   os << " ";
   printNameOrConst(os, getChild(0)) << " to type " << *type();
   return os;
}

/* ===--------------------------------------------------------------------=== */
// GetElementPtrInst implementation
/* ===--------------------------------------------------------------------=== */

GetElementPtrInst::GetElementPtrInst(Context& ctx, Value* ptr, Type* ty,
                                     utils::range_ref<Value*> indices)
      : Instruction{ctx, Type::getPointerTy(ctx), ty} {
   assert((dyn_cast<StructType>(ty) || dyn_cast<ArrayType>(ty)) &&
          "Type must be a struct or array type");
   /*assert((ptr->type() == Type::getPointerTy(ctx)) &&
          "Pointer passed to GEP must be a pointer");*/
   addChild(ptr);
   indices.for_each([this, &ctx](auto* idx) {
      assert((idx->type()->getSizeInBits() ==
              Type::getPointerTy(ctx)->getSizeInBits()) &&
             "Index must be compatible with a pointer type");
      addChild(idx);
   });
   static_assert(sizeof(GetElementPtrInst) == sizeof(Instruction));
}

std::ostream& GetElementPtrInst::print(std::ostream& os) const {
   printSelf(os, this) << " = ";
   os << "getelementptr type " << *getContainedType() << ", ";
   printNameOrConst(os, getChild(0));
   for(unsigned i = 1; i < numChildren(); ++i) {
      os << ", ";
      printNameOrConst(os, getChild(i));
   }
   return os;
}

/* ===--------------------------------------------------------------------=== */
// PhiNode implementation
/* ===--------------------------------------------------------------------=== */

PhiNode::PhiNode(Context& ctx, Type* type, utils::range_ref<Value*> values,
                 utils::range_ref<BasicBlock*> preds)
      : Instruction{ctx, type} {
   assert(values.size() == preds.size() && "PhiNode values and preds mismatch");
   unsigned i = 0;
   values.for_each([this, &i](Value* val) {
      addChild(val, i);
      i += 2;
   });
   i = 1;
   preds.for_each([this, &i](BasicBlock* bb) {
      addChild(bb, i);
      i += 2;
   });
}

std::ostream& PhiNode::print(std::ostream& os) const {
   printSelf(os, this) << " = ";
   os << "phi " << *type() << ", ";
   for(unsigned i = 0; i < numChildren(); i += 2) {
      printNameOrConst(os, getChild(i)) << " [";
      printNameOrConst(os, getChild(i + 1)) << "]";
      if(i != numChildren() - 2) os << ", ";
   }
   return os;
}

void PhiNode::replaceOrAddOperand(BasicBlock* pred, Value* val) {
   // Try to find the predecessor and replace the value
   for(unsigned i = 1; i < numChildren(); i += 2) {
      if(getChild(i) == pred) {
         replaceChild(i - 1, val);
         return;
      }
   }
   // Otherwise, add the new value and predecessor
   addChild(val);
   addChild(pred);
}

utils::Generator<PhiNode::IncomingValue> PhiNode::incomingValues() const {
   for(unsigned i = 0; i < numChildren(); i += 2) {
      co_yield {getChild(i), cast<BasicBlock>(getChild(i + 1))};
   }
}

} // namespace tir
