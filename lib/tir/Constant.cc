#include "tir/Constant.h"

#include <iostream>

#include "tir/BasicBlock.h"

namespace tir {

ConstantInt* Constant::CreateBool(Context& ctx, bool value) {
   return ConstantInt::Create(ctx, Type::getInt1Ty(ctx), value);
}

ConstantInt* Constant::CreateInt32(Context& ctx, uint32_t value) {
   return ConstantInt::Create(ctx, Type::getInt32Ty(ctx), value);
}

ConstantNullPointer* Constant::CreateNullPointer(Context& ctx) {
   return ConstantNullPointer::Create(ctx);
}

std::ostream& ConstantNullPointer::print(std::ostream& os) const {
   os << "ptr* null";
   return os;
}

std::ostream& ConstantInt::print(std::ostream& os) const {
   os << *type() << " " << value();
   return os;
}

std::ostream& GlobalVariable::print(std::ostream& os) const { return os; }

Argument::Argument(Function* parent, Type* type, unsigned index)
      : Value{parent->ctx(), type}, parent_{parent}, index_{index} {
   setName("arg");
}

std::ostream& Argument::print(std::ostream& os) const {
   os << *type() << " ";
   printName(os);
   return os;
}

std::ostream& Function::print(std::ostream& os) const {
   os << "function ";
   if(!hasBody()) os << "external ";
   os << *getReturnType() << " " << "@" << name() << "(";
   bool isFirst = true;
   for(auto* arg : args()) {
      if(!isFirst) os << ", ";
      isFirst = false;
      arg->print(os);
   }
   os << ")";
   if(hasBody()) {
      os << " {\n";
      for(auto* bb : body()) {
         bb->print(os) << "\n";
      }
      os << "}\n";
   }
   return os;
}

} // namespace tir
