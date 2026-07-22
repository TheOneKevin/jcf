#include "tir/Constant.h"

#include <iostream>
#include <queue>
#include <vector>

#include "tir/BasicBlock.h"
#include "tir/Type.h"
#include "utils/DotPrinter.h"

namespace tir {

ConstantInt* Constant::CreateInt(Context& ctx, uint8_t bits, uint32_t value) {
   return ConstantInt::Create(ctx, IntegerType::get(ctx, bits), value);
}

ConstantNullPointer* Constant::CreateNullPointer(Context& ctx) {
   return ConstantNullPointer::Create(ctx);
}

std::ostream& ConstantNullPointer::print(std::ostream& os) const {
   os << "null";
   return os;
}

std::ostream& Undef::print(std::ostream& os) const {
   os << "undef";
   return os;
}

std::ostream& ConstantInt::print(std::ostream& os) const {
   os << zextValue() << ":" << *type();
   return os;
}

std::ostream& GlobalVariable::print(std::ostream& os) const {
   os << "global " << *type() << " @" << name();
   return os;
}

Argument::Argument(Function* parent, Type* type, unsigned index)
      : Value{parent->ctx(), type}, parent_{parent}, index_{index} {
   setName("arg");
}

std::ostream& Argument::print(std::ostream& os) const {
   os << "%";
   printName(os)  << ":" << *type();
   return os;
}

std::ostream& Function::print(std::ostream& os) const {
   os << "function ";
   if(isExternalLinkage()) os << "external ";
   if(attrs().noreturn) os << "noreturn ";
   os << *getReturnType() << " "
      << "@" << name() << "(";
   bool isFirst = true;
   for(auto* arg : args()) {
      if(!isFirst) os << ", ";
      isFirst = false;
      arg->print(os);
   }
   os << ")";
   if(hasBody()) {
      os << " {\n";
      for(auto* bb : reversePostOrder()) {
         bb->print(os) << "\n";
      }
      os << "}\n";
   }
   return os;
}

void Function::printDot(std::ostream& os) const {
   utils::DotPrinter dp{os, "5"};
   dp.startGraph();
   std::unordered_map<BasicBlock*, int> bbMap;
   std::vector<BasicBlock*> terms;
   for(auto bb : body()) {
      int id = bb->printDotNode(dp);
      bbMap[bb] = id;
   }
   for(auto bb : body()) {
      terms.clear();
      for(auto succ : bb->successors()) terms.push_back(succ);
      if(terms.size() == 2) {
         dp.printConnection(bbMap[bb], ":T", bbMap[terms[0]]);
         dp.printConnection(bbMap[bb], ":F", bbMap[terms[1]]);
      } else {
         for(auto term : terms) {
            dp.printConnection(bbMap[bb], bbMap[term]);
         }
      }
   }
   dp.endGraph();
}

utils::Generator<BasicBlock*> Function::reversePostOrder() const {
   std::unordered_set<BasicBlock*> visited;
   std::queue<BasicBlock*> next;
   assert(hasBody() && getEntryBlock() && "Function has no entry block");
   next.push(getEntryBlock());
   while(!next.empty()) {
      auto* bb = next.front();
      next.pop();
      if(visited.count(bb)) continue;
      visited.insert(bb);
      co_yield bb;
      for(auto succ : bb->successors()) {
         next.push(succ);
      }
   }
}

void Function::removeBlock(BasicBlock* block) {
   assert(block->parent() == this && "Block does not belong to this function");
   assert(block != entryBB_ && "Cannot remove the entry block");
   block->releaseAllReferences();
   body_.remove(block);
}

} // namespace tir
