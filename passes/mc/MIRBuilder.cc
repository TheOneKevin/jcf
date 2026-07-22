#include <vector>
#include "mc/InstSelectNode.h"
#include "mc/MCFunction.h"
#include "../IRPasses.h"
#include "tir/Constant.h"
#include "utils/BumpAllocator.h"
#include "utils/PassManager.h"

class MIRBuilder;
using std::string_view;
using utils::Pass;
using utils::PassManager;
using namespace mc;
using DAG = MIRBuilder;
using namespace tir;
using ISN = InstSelectNode;
using T = ISN::Type;

class MIRBuilder : public Pass {
public:
   MIRBuilder(PassManager& PM) noexcept : Pass(PM) {}
   string_view Name() const override { return "mirbuilder"; }
   string_view Desc() const override { return "Build MIR from TIR"; }
   void Init() override {
      dumpDot = PM().GetExistingOption("--debug-mc")->count();
   }
   void Run() override {
      auto& CU = GetPass<passes::IRContext>().CU();
      for(auto* F : CU.functions()) {
         if(!F->hasBody()) continue;
         buildMCFunction(F);
         if(dumpDot) {
            std::ofstream out{std::string{F->name()} + ".dag.dot"};
            MCF->printDot(out);
         }
      }
   }

private:
   void ComputeDependencies() override {
      AddDependency(GetPass<passes::IRContext>());
   }

private:
   // Build the DAG for a given TIR function
   void buildMCFunction(tir::Function const* F);
   auto& alloc() {
      if(!alloc_) alloc_ = &GetPass<passes::IRContext>().Alloc();
      return *alloc_;
   }

private:
   // Main DAG building instruction to translate TIR -> instruction node
   InstSelectNode* buildInst(tir::Instruction*);
   // Build (if not exist) or get (if exist) a virtual register node
   InstSelectNode* buildVReg(tir::Instruction*);
   // Find an already built instruction or constant or (etc.)
   InstSelectNode* findValue(tir::Value*);
   // Create a condition code leaf node
   InstSelectNode* buildCC(tir::Instruction::Predicate);
   // Allocate (if not exist) or get (if exist) a virtual register index
   // corresponding to the given value
   int findOrAllocVirtReg(tir::Value* v);
   // Allocate (if not exist) or get (if exist) a stack slot index chunk
   // corresponding to the given alloca instruction
   InstSelectNode::StackSlot findOrAllocStackSlot(tir::AllocaInst* alloca);
   // Create a chain edge from this instruction to the previous instruction
   // if possible (returns false when impossible)
   bool tryChainToPrev(tir::Instruction*, InstSelectNode*);
   // Create a chain to previous, and if not, then to the entry
   void chainToPrevOrEntry(tir::Instruction*, InstSelectNode*);
   // Creates a chain if the instruction requires it (i.e., dependencies)
   void createChainIfNeeded(tir::Instruction*, InstSelectNode*);

private:
   bool dumpDot;
   BumpAllocator* alloc_ = nullptr;
   MCFunction* MCF;
   tir::BasicBlock* curbb;
   // Maps TIR instruction -> node, this is not cleared per BB
   std::unordered_map<tir::Value*, InstSelectNode*> instMap;
   std::unordered_map<tir::Value*, int> vregMap;
   std::unordered_map<tir::AllocaInst*, InstSelectNode::StackSlot> allocaMap;
   std::unordered_map<tir::BasicBlock*, InstSelectNode*> bbMap;
   int highestVregIdx = 0;
   int highestStackSlotIdx = 0;
};
REGISTER_PASS(MIRBuilder);

/* ===--------------------------------------------------------------------=== */
// Function to actually build the DAG for a given TIR function
/* ===--------------------------------------------------------------------=== */

void MIRBuilder::buildMCFunction(tir::Function const* F) {
   using namespace mc;
   using ISN = mc::InstSelectNode;
   auto const& TD = GetPass<passes::IRContext>().TD();
   // Allocate the MCFunction to store the DAGs
   void* buf = alloc().allocate_bytes(sizeof(MCFunction), alignof(MCFunction));
   MCF = new(buf) MCFunction{alloc(), F->ctx().TI(), TD};
   // Build dummy DAG nodes for each basic block
   for(auto* bb : F->reversePostOrder()) {
      auto* const node = InstSelectNode::CreateLeaf(alloc(), MCF, NodeKind::Entry);
      bbMap[bb] = node;
      MCF->addSubgraph(node);
   }
   // Build the DAG for each basic block
   for(auto* bb : F->body()) {
      curbb = bb;
      ISN* entry = nullptr;
      for(auto* inst : *bb) {
         entry = buildInst(inst);
      }
      assert(entry && "Basic block has no terminator");
      bbMap[bb]->addChild(entry);
   }
   // For each of the vregs, add a LoadToReg node
   for(auto [v, idx] : vregMap) {
      auto instnode = instMap[v];
      auto vreg = ISN::CreateLeaf(alloc(),
                                  MCF,
                                  NodeKind::Register,
                                  ISN::Type{v->type()->getSizeInBits()},
                                  ISN::VReg{idx});
      auto node =
            ISN::Create(alloc(), MCF, T{}, NodeKind::LoadToReg, {vreg, instnode});
      // Find the graph corresponding to instnode
      auto parbb = cast<tir::Instruction>(v)->parent();
      cast<ISN>(bbMap[parbb])->addChild(node);
   }
   // Now we rearrange the children of entry so they are children of the
   // first branch instruction instead
   for(auto& mbb : MCF->subgraphs()) {
      auto* bb = mbb.root;
      // First, find the branch of the BB
      InstSelectNode* branch = nullptr;
      for(auto* child : bb->childNodes()) {
         if(child->kind() == NodeKind::BR || child->kind() == NodeKind::BR_CC ||
            child->kind() == NodeKind::RETURN ||
            child->kind() == NodeKind::UNREACHABLE) {
            branch = child;
            break;
         }
      }
      assert(branch);
      // Next, move all the children of the entry node to the branch node
      for(auto* child : bb->childNodes())
         if(child != branch) branch->addChild(child);
      // Nuke all chains from bb
      bb->clearChains();
      // Re-add the branch node to the bb
      bb->addChild(branch);
      // This step is purely cosmetic:
      //   Let's clean up the chains of BR by removing the chained nodes that
      //   already have users (and so don't need to be chained to BR).
      for(unsigned i = branch->numChildren(); i > branch->arity(); i--) {
         auto* child = branch->getChild(i - 1);
         if(child->numUsers() > 1) {
            branch->removeChild(i - 1);
         }
      }
   }
   GetPass<passes::IRContext>().AddMIRFunction(F, MCF);
}

/* ===--------------------------------------------------------------------=== */
// DAG building helper functions (to allocate things + caching nodes)
/* ===--------------------------------------------------------------------=== */

int DAG::findOrAllocVirtReg(tir::Value* v) {
   auto it = vregMap.find(v);
   if(it != vregMap.end()) {
      return it->second;
   }
   return vregMap[v] = ++highestVregIdx;
}

ISN::StackSlot DAG::findOrAllocStackSlot(tir::AllocaInst* alloca) {
   auto it = allocaMap.find(alloca);
   if(it != allocaMap.end()) {
      return it->second;
   }
   auto& TI = MCF->TI();
   int idx = ++highestStackSlotIdx;
   int bytes = (alloca->allocatedType()->getSizeInBits() + 1) / 8;
   int slots = (bytes + TI.getStackAlignment() - 1) / TI.getStackAlignment();
   return allocaMap[alloca] = ISN::StackSlot{static_cast<uint16_t>(idx),
                                             static_cast<uint16_t>(slots)};
}

InstSelectNode* DAG::buildVReg(tir::Instruction* v) {
   int vreg = findOrAllocVirtReg(v);
   auto* const node = ISN::CreateLeaf(alloc(),
                                      MCF,
                                      NodeKind::Register,
                                      ISN::Type{v->type()->getSizeInBits()},
                                      ISN::VReg{vreg});
   return node;
}

InstSelectNode* DAG::buildCC(tir::Instruction::Predicate pred) {
   return ISN::CreateLeaf(alloc(), MCF, NodeKind::Predicate, ISN::Type{0}, pred);
}

InstSelectNode* DAG::findValue(tir::Value* v) {
   if(v->isBasicBlock()) {
      auto& subgraph = bbMap[cast<BasicBlock>(v)];
      auto node = ISN::CreateLeaf(alloc(), MCF, NodeKind::BasicBlock);
      node->addChild(subgraph);
      return node;
   } else if(v->isInstruction()) {
      auto* instr = cast<Instruction>(v);
      // If this is an alloca, use a stack slot
      if(auto* alloca = dyn_cast<AllocaInst>(v)) {
         return ISN::CreateLeaf(
               alloc(),
               MCF,
               NodeKind::FrameIndex,
               ISN::Type{(uint32_t)MCF->TI().getPointerSizeInBits()},
               findOrAllocStackSlot(alloca));
      }
      // If the instruction is declared in another bb, use a vreg
      // Otherwise, grab the emitted instruction
      return instr->parent() != curbb
                   ? buildVReg(instr)
                   : (assert(instMap.contains(instr) &&
                             "Instruction does not dominate all uses"),
                      instMap[instr]);
   } else if(v->isFunction()) {
      return ISN::CreateLeaf(
            alloc(),
            MCF,
            NodeKind::GlobalAddress,
            ISN::Type{Type::getPointerTy(curbb->ctx())->getSizeInBits()},
            cast<GlobalObject>(v));
   } else if(v->isFunctionArg()) {
      auto arg = cast<Argument>(v);
      int idx = arg->index();
      return ISN::CreateLeaf(alloc(),
                             MCF,
                             NodeKind::Argument,
                             ISN::Type{arg->type()->getSizeInBits()},
                             ISN::VReg{idx});
   } else if(v->isConstant()) {
      auto c = cast<Constant>(v);
      if(c->isNumeric()) {
         auto CI = cast<ConstantInt>(c);
         auto bits = CI->type()->getSizeInBits();
         return ISN::CreateImm(alloc(), MCF, bits, CI->zextValue());
      } else if(c->isGlobalVariable()) {
         return ISN::CreateLeaf(
               alloc(),
               MCF,
               NodeKind::GlobalAddress,
               ISN::Type{Type::getPointerTy(curbb->ctx())->getSizeInBits()},
               cast<GlobalObject>(c));
      } else if(c->isNullPointer()) {
         return ISN::CreateImm(alloc(), MCF, MCF->TI().getPointerSizeInBits(), 0);
      } else if(c->isUndef()) {
         return ISN::CreateImm(alloc(), MCF, c->type()->getSizeInBits(), 0);
      }
   }
   assert(false && "Unknown value type");
   std::unreachable();
}

bool DAG::tryChainToPrev(tir::Instruction* inst, InstSelectNode* node) {
   // Check if the dependency to the previou node even exists
   auto dep = inst->prev();
   if(!dep) return false;
   // If dep is a user of inst, then don't chain
   for(auto* user : dep->users())
      if(user == inst) return false;
   // Otherwise, add a chain edge to the dep
   node->addChild(findValue(dep));
   return true;
}

void DAG::chainToPrevOrEntry(tir::Instruction* inst, InstSelectNode* node) {
   // Can a chain be created?
   if(tryChainToPrev(inst, node)) return;
   // If no chain, then chain the node to the bb entry node
   bbMap[curbb]->addChild(node);
}

void DAG::createChainIfNeeded(tir::Instruction* inst, InstSelectNode* node) {
   /**
    * Instructions with side effects must execute in-order
    * 1. Loads must wait for the previous instruction to finish
    * 2. Stores, calls and any side-effect instructions must be executed before
    *    the next instruction starts.
    */
   if(dyn_cast<LoadInst>(inst)) {
      chainToPrevOrEntry(inst, node);
   } else if(inst->prev() && inst->prev()->hasSideEffects()) {
      chainToPrevOrEntry(inst, node);
   }
}

/* ===--------------------------------------------------------------------=== */
// Main DAG building function to translate TIR instruction -> DAG nodes
/* ===--------------------------------------------------------------------=== */

InstSelectNode* DAG::buildInst(tir::Instruction* inst) {
   // Build the current instruction
   InstSelectNode* node = nullptr;
   auto IRTy = inst->type();
   auto Ty = IRTy->isSizeBounded() ? T{IRTy->getSizeInBits()} : T{};
   // Go through the different types of instruction and emit it!
   if(auto alloca = dyn_cast<AllocaInst>(inst)) {
      return nullptr;
   } else if(auto br = dyn_cast<BranchInst>(inst)) {
      auto cond = br->getCondition();
      auto bb1 = br->getSuccessor(0);
      auto bb2 = br->getSuccessor(1);
      // If it's an unconditional branch, emit a br
      if(bb1 == bb2) {
         node = ISN::Create(alloc(), MCF, T{}, NodeKind::BR, {findValue(bb1)});
      }
      // If the condition comes from a cmp, emit a br_cc
      else if(auto cmp = dyn_cast<CmpInst>(cond)) {
         auto lhs = findValue(cmp->getChild(0));
         auto rhs = findValue(cmp->getChild(1));
         auto cc = buildCC(cmp->predicate());
         node = ISN::Create(alloc(),
                            MCF,
                            T{},
                            NodeKind::BR_CC,
                            {cc, lhs, rhs, findValue(bb1), findValue(bb2)});
      }
      // Else, emit a br_cc with a comparison against 0
      else {
         auto cc = buildCC(CmpInst::Predicate::NE);
         const auto zero =
               ISN::CreateImm(alloc(), MCF, cond->type()->getSizeInBits(), 0);
         auto lhs = findValue(cond);
         node = ISN::Create(alloc(),
                            MCF,
                            T{},
                            NodeKind::BR_CC,
                            {cc, lhs, zero, findValue(bb1), findValue(bb2)});
      }
   } else if(auto ri = dyn_cast<ReturnInst>(inst)) {
      if(!ri->isReturnVoid()) {
         node = ISN::Create(
               alloc(), MCF, T{}, NodeKind::RETURN, {findValue(ri->getChild(0))});
      } else {
         node = ISN::Create(alloc(), MCF, Ty, NodeKind::RETURN, {});
      }
   } else if(dyn_cast<StoreInst>(inst)) {
      auto src = inst->getChild(0);
      auto dst = inst->getChild(1);
      node = ISN::Create(
            alloc(), MCF, T{}, NodeKind::STORE, {findValue(src), findValue(dst)});
   } else if(dyn_cast<LoadInst>(inst)) {
      auto src = inst->getChild(0);
      node = ISN::Create(alloc(), MCF, Ty, NodeKind::LOAD, {findValue(src)});
   } else if(auto bin = dyn_cast<BinaryInst>(inst)) {
      auto op = bin->binop();
      auto lhs = findValue(bin->getChild(0));
      auto rhs = findValue(bin->getChild(1));
      auto nodeType = NodeKind::None;
      switch(op) {
         case tir::Instruction::BinOp::Add:
            nodeType = NodeKind::ADD;
            break;
         case tir::Instruction::BinOp::Sub:
            nodeType = NodeKind::SUB;
            break;
         case tir::Instruction::BinOp::Mul:
            nodeType = NodeKind::MUL;
            break;
         case tir::Instruction::BinOp::Div:
            nodeType = NodeKind::SDIV;
            break;
         case tir::Instruction::BinOp::Rem:
            nodeType = NodeKind::SREM;
            break;
         case tir::Instruction::BinOp::And:
            nodeType = NodeKind::AND;
            break;
         case tir::Instruction::BinOp::Or:
            nodeType = NodeKind::OR;
            break;
         case tir::Instruction::BinOp::Xor:
            nodeType = NodeKind::XOR;
            break;
         case tir::Instruction::BinOp::None:
         case tir::Instruction::BinOp::LAST_MEMBER:
            assert(false);
            std::unreachable();
      }
      node = ISN::Create(alloc(), MCF, Ty, nodeType, {lhs, rhs});
   } else if(auto ci = dyn_cast<CallInst>(inst)) {
      // FIXME(kevin): Calls should have a call begin + call end
      findValue(ci->getCallee());
      std::vector<InstSelectNode*> args;
      args.push_back(findValue(ci->getCallee()));
      if(ci->nargs() > 0) {
         for(auto* arg : ci->args()) args.push_back(findValue(arg));
      }
      node = ISN::Create(alloc(), MCF, Ty, NodeKind::CALL, args);
      // If the call is a terminator, add an UNREACHABLE to the end of it
      if(ci->isTerminator()) {
         auto unreachable = ISN::CreateLeaf(alloc(), MCF, NodeKind::UNREACHABLE);
         bbMap[curbb]->addChild(unreachable);
         unreachable->addChild(node);
         node = unreachable;
      }
   } else if(auto ici = dyn_cast<ICastInst>(inst)) {
      auto src = findValue(ici->getChild(0));
      auto nodeType = NodeKind::None;
      switch(ici->castop()) {
         case tir::Instruction::CastOp::Trunc:
            nodeType = NodeKind::TRUNCATE;
            break;
         case tir::Instruction::CastOp::ZExt:
            nodeType = NodeKind::ZERO_EXTEND;
            break;
         case tir::Instruction::CastOp::SExt:
            nodeType = NodeKind::SIGN_EXTEND;
            break;
         case tir::Instruction::CastOp::LAST_MEMBER:
            assert(false);
            std::unreachable();
      }
      node = ISN::Create(alloc(), MCF, Ty, nodeType, {src});
   } else if(auto gep = dyn_cast<GetElementPtrInst>(inst)) {
      auto base = findValue(gep->getPointerOperand());
      auto type = gep->getContainedType();
      auto ptrbits = tir::Type::getPointerTy(inst->ctx())->getSizeInBits();
      auto PTy = T{ptrbits};
      for(auto* idx : gep->indices()) {
         // If the type is a struct, we need to add the offset of the field
         // Whereas arrays can be indexed dynamically
         if(type->isStructType()) {
            assert(idx->isConstant() && "Struct type requires constant index");
            auto nidx = cast<ConstantInt>(idx)->zextValue();
            auto STy = cast<StructType>(type);
            auto offs = STy->getTypeOffsetAtIndex(nidx);
            auto offs_node = ISN::CreateImm(alloc(), MCF, ptrbits, offs);
            base =
                  ISN::Create(alloc(), MCF, PTy, NodeKind::ADD, {base, offs_node});
            type = STy->getTypeAtIndex(nidx);
         } else if(type->isArrayType()) {
            auto ATy = cast<ArrayType>(type);
            auto idx_node = findValue(idx);
            auto elem_sz = ATy->getElementType()->getSizeInBits();
            auto elem_sz_node = ISN::CreateImm(alloc(), MCF, ptrbits, elem_sz);
            auto offs_node = ISN::Create(
                  alloc(), MCF, PTy, NodeKind::MUL, {idx_node, elem_sz_node});
            base =
                  ISN::Create(alloc(), MCF, PTy, NodeKind::ADD, {base, offs_node});
            type = ATy->getElementType();
         } else {
            assert(false && "Unsupported GEP type");
         }
      }
      node = base;
   } else if(auto cmp = dyn_cast<CmpInst>(inst)) {
      // Emit cmp as set_cc even if it's not used
      auto lhs = findValue(cmp->getChild(0));
      auto rhs = findValue(cmp->getChild(1));
      auto cc = buildCC(cmp->predicate());
      node = ISN::Create(alloc(), MCF, Ty, NodeKind::SET_CC, {cc, lhs, rhs});
   } else if(dyn_cast<PhiNode>(inst)) {
      node = ISN::Create(alloc(), MCF, Ty, NodeKind::PHI, {});
      for(auto [bb, val] : cast<PhiNode>(inst)->incomingValues()) {
         node->addChild(findValue(val));
         node->addChild(findValue(bb));
      }
   } else {
      assert(false &&
             "Instruction selection DAG does not support this instruction");
      std::unreachable();
   }
   // Create chain if needed
   createChainIfNeeded(inst, node);
   // Store the instruction
   instMap[inst] = node;
   return node;
}
