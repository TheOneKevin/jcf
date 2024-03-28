#include "tir/Instructions.h"

#include "tir/Constant.h"
#include "tir/TIR.h"

namespace tir {

static std::ostream& printNameOrConst(std::ostream& os, Value* val) {
   if(auto* ci = dyn_cast<ConstantInt>(val)) {
      os << *ci;
   } else {
      if(!val->type()->isLabelType()) os << *val->type() << " ";
      val->printName(os);
   }
   return os;
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
}

std::ostream& BranchInst::print(std::ostream& os) const {
   os << "br ";
   printNameOrConst(os, getChild(0)) << ", ";
   printNameOrConst(os, getChild(1)) << ", ";
   printNameOrConst(os, getChild(2));
   return os;
}

/* ===--------------------------------------------------------------------=== */
// ReturnInst implementation
/* ===--------------------------------------------------------------------=== */

ReturnInst::ReturnInst(Context& ctx, Value* ret)
      : Instruction{ctx, ret ? ret->type() : Type::getVoidTy(ctx)} {
   if(ret) addChild(ret);
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
}

std::ostream& StoreInst::print(std::ostream& os) const {
   os << "store ";
   printNameOrConst(os, getChild(0)) << ", ";
   printNameOrConst(os, getChild(1));
   return os;
}

/* ===--------------------------------------------------------------------=== */
// LoadInst implementation
/* ===--------------------------------------------------------------------=== */

LoadInst::LoadInst(Context& ctx, Type* type, Value* ptr) : Instruction{ctx, type} {
   addChild(ptr);
}

std::ostream& LoadInst::print(std::ostream& os) const {
   printName(os) << " = ";
   os << "load " << *type() << ", ";
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
}

std::ostream& CallInst::print(std::ostream& os) const {
   if(!type()->isVoidType())
      printName(os) << " = ";
   os << "call " << getChild(0)->name() << "(";
   for(unsigned i = 1; i < numChildren(); ++i) {
      printNameOrConst(os, getChild(i));
      if(i != numChildren() - 1) os << ", ";
   }
   os << ")";
   if(isTerminator())
      os << " noreturn";
   return os;
}

Function* CallInst::getCallee() const {
   return cast<Function>(getChild(0));
}

bool CallInst::isTerminator() const {
   return getCallee()->isNoReturn();
}

/* ===--------------------------------------------------------------------=== */
// BinaryInst implementation
/* ===--------------------------------------------------------------------=== */

BinaryInst::BinaryInst(Context& ctx, BinOp binop, Value* lhs, Value* rhs)
      : Instruction{ctx, lhs->type(), binop} {
   addChild(lhs);
   addChild(rhs);
}

std::ostream& BinaryInst::print(std::ostream& os) const {
   printName(os) << " = ";
   const char* name = BinOp_to_string(binop(), "unknown");
   // Print name lower case
   for(unsigned i = 0; name[i] != '\0'; ++i)
      os << static_cast<char>(tolower(name[i]));
   // Print operands
   os << " " << *type() << ", ";
   printNameOrConst(os, getChild(0)) << ", ";
   printNameOrConst(os, getChild(1));
   return os;
}

/* ===--------------------------------------------------------------------=== */
// CmpInst implementation
/* ===--------------------------------------------------------------------=== */

CmpInst::CmpInst(Context& ctx, Predicate pred, Value* lhs, Value* rhs)
      : Instruction{ctx, Type::getInt1Ty(ctx)}, pred_(pred) {
   addChild(lhs);
   addChild(rhs);
}

std::ostream& CmpInst::print(std::ostream& os) const {
   printName(os) << " = cmp ";
   const char* name = Predicate_to_string(pred_, "unknown");
   // Print name lower case
   for(unsigned i = 0; name[i] != '\0'; ++i)
      os << static_cast<char>(tolower(name[i]));
   // Print operands
   os << " " << *type() << " ";
   printNameOrConst(os, getChild(0)) << ", ";
   printNameOrConst(os, getChild(1));
   return os;
}

/* ===--------------------------------------------------------------------=== */
// AllocaInst implementation
/* ===--------------------------------------------------------------------=== */

AllocaInst::AllocaInst(Context& ctx, Type* type)
      : Instruction{ctx, Type::getPointerTy(ctx)}, allocType_{type} {}

std::ostream& AllocaInst::print(std::ostream& os) const {
   return printName(os) << " = "
                        << "alloca " << *allocType_;
}

/* ===--------------------------------------------------------------------=== */
// ICastInst implementation
/* ===--------------------------------------------------------------------=== */

ICastInst::ICastInst(Context& ctx, CastOp op, Value* val, Type* destTy)
      : Instruction{ctx, destTy}, castop_(op) {
   addChild(val);
}

std::ostream& ICastInst::print(std::ostream& os) const {
   printName(os) << " = ";
   os << "icast ";
   const char* name = CastOp_to_string(castop_, "unknown");
   // Print name lower case
   for(unsigned i = 0; name[i] != '\0'; ++i)
      os << static_cast<char>(tolower(name[i]));
   os << " ";
   printNameOrConst(os, getChild(0)) << " to " << *type() << "";
   return os;
}

/* ===--------------------------------------------------------------------=== */
// GetElementPtrInst implementation
/* ===--------------------------------------------------------------------=== */

GetElementPtrInst::GetElementPtrInst(Context& ctx, Value* ptr,
                                     StructType* structTy,
                                     utils::range_ref<Value*> indices)
      : Instruction{ctx, Type::getPointerTy(ctx)}, structTy_{structTy} {
   addChild(ptr);
   indices.for_each([this](auto* idx) { addChild(idx); });
}

std::ostream& GetElementPtrInst::print(std::ostream& os) const {
   printName(os) << " = ";
   os << "getelementptr " << *structTy_ << ", ";
   printNameOrConst(os, getChild(0));
   for(unsigned i = 1; i < numChildren(); ++i) {
      os << ", ";
      printNameOrConst(os, getChild(i));
   }
   return os;
}

} // namespace tir
