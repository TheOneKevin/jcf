#pragma once

#include <vector>
#include "tir/BasicBlock.h"
#include "tir/CompilationUnit.h"
#include "tir/Constant.h"
#include "tir/Context.h"
#include "tir/Instructions.h"

namespace tir {

class IRBuilder {
public:
   IRBuilder(Context& ctx) : ctx_{ctx}, insertPoint_{} {}

   /**
    * @brief Set the insertion point to the given basic block. This will insert
    * new instructions after the given instruction iterator.
    *
    * @param it The iterator to set the insertion point to
    */
   void setInsertPoint(BasicBlock::iterator it) { insertPoint_ = it; }

   /**
    * @brief Set the insertion point to the given instruction. This will insert
    * new instructions after the given instruction.
    *
    * @param instr The instruction to set the insertion point to
    */
   void setInsertPoint(Instruction* instr) { setInsertPoint(instr->iter()); }

   /**
    * @brief Set the insertion point to the beginning of the given basic block.
    *
    * @param bb The basic block to set the insertion point to
    */
   void setInsertPoint(BasicBlock* bb) { insertPoint_ = bb->begin(); }

   BasicBlock* currentBlock() { return insertPoint_.getBB(); }

public:
   /**
    * @brief Create a new basic block within the given function. This does
    * not change the insertion point, unlike the other create functions.
    *
    * @param parent The parent function of the basic block
    * @return BasicBlock* The new (empty) basic block
    */
   BasicBlock* createBasicBlock(Function* parent) {
      return BasicBlock::Create(ctx_, parent);
   }

   /**
    * @brief Create a binary instruction with the given operation and
    * left-hand and right-hand operands (as values).
    *
    * @param op The binary operation to perform
    * @param lhs The LHS value
    * @param rhs The RHS value
    * @return BinaryInst* A pointer to the instruction
    */
   Instruction* createBinaryInstr(Instruction::BinOp op, Value* lhs, Value* rhs) {
      return insert(BinaryInst::Create(ctx_, op, lhs, rhs));
   }

   /**
    * @brief Create a call instruction with the given callee and arguments.
    *
    * @param callee The function to call
    * @param args The arguments to pass to the function
    * @return CallInst* A pointer to the instruction
    */
   Instruction* createCallInstr(Value* callee, utils::range_ref<Value*> args) {
      return insert(CallInst::Create(ctx_, callee, args));
   }

   /**
    * @brief Create an indirect call through a function pointer.
    *
    * @param retTy The result type of the call (cannot be derived from the
    *              callee, which is an untyped `ptr`)
    * @param calleePtr The function pointer to call
    * @param args The arguments to pass
    * @return Instruction* A pointer to the instruction
    */
   Instruction* createCallInstr(Type* retTy, Value* calleePtr,
                                utils::range_ref<Value*> args) {
      return insert(CallInst::CreateIndirect(ctx_, retTy, calleePtr, args));
   }

   /**
    * @brief Create a call to an intrinsic function.
    *
    * @param kind The intrinsic kind
    * @param args The arguments to pass to the intrinsic
    * @return Instruction* A pointer to the instruction
    */
   Instruction* createIntrinsicCallInstr(Instruction::IntrinsicKind kind,
                                         utils::range_ref<Value*> args) {
      auto* cu = currentBlock()->parent()->parent();
      return createCallInstr(cu->getIntrinsic(kind), args);
   }

   /**
    * @brief Create a load instruction with the given type and pointer.
    * The type is the type of the value to load and will dictate the size of
    * the load operation. The ptr is the pointer to load from.
    *
    * @param type The type of the value to load
    * @param ptr The pointer to load from
    * @return LoadInst* A pointer to the instruction
    */
   Instruction* createLoadInstr(Type* type, Value* ptr) {
      return insert(LoadInst::Create(ctx_, type, ptr));
   }

   /**
    * @brief Create a store instruction with the given value and pointer.
    * The value is the value to store and the ptr is the pointer to store to.
    *
    * @param val The value to store
    * @param ptr The pointer to store to
    * @return StoreInst* A pointer to the instruction
    */
   Instruction* createStoreInstr(Value* val, Value* ptr) {
      return insert(StoreInst::Create(ctx_, val, ptr));
   }

   /**
    * @brief Create a return instruction with the given value.
    *
    * @param val The value to return
    * @return ReturnInst* A pointer to the instruction
    */
   Instruction* createReturnInstr(Value* val) {
      return insert(ReturnInst::Create(ctx_, val));
   }

   /**
    * @brief Create a return instruction with no value (void).
    *
    * @return ReturnInst* A pointer to the instruction
    */
   Instruction* createReturnInstr() {
      return insert(ReturnInst::Create(ctx_, nullptr));
   }

   /**
    * @brief Creates an unconditional branch instruction to the given target
    * basic block.
    *
    * @param target The target basic block
    * @return BranchInst* A pointer to the instruction
    */
   Instruction* createBranchInstr(BasicBlock* target) {
      return createBranchInstr(Constant::CreateBool(ctx_, true), target, target);
   }

   /**
    * @brief Create a conditional branch instruction with the given condition
    * and true and false target basic blocks.
    *
    * @param cond The condition to branch on
    * @param true_target The target basic block if the condition is true
    * @param false_target The target basic block if the condition is false
    * @return BranchInst* A pointer to the instruction
    */
   Instruction* createBranchInstr(Value* cond, BasicBlock* true_target,
                                  BasicBlock* false_target) {
      return insert(BranchInst::Create(ctx_, cond, true_target, false_target));
   }

   /**
    * @brief Create a comparison instruction with the given predicate and
    * left-hand and right-hand operands.
    *
    * @param pred The comparison predicate
    * @param lhs The LHS value
    * @param rhs The RHS value
    * @return CmpInst* A pointer to the instruction
    */
   Instruction* createCmpInstr(CmpInst::Predicate pred, Value* lhs, Value* rhs) {
      return insert(CmpInst::Create(ctx_, pred, lhs, rhs));
   }

   /**
    * @brief Create the generic trunc/zext/sext instruction with the given
    * operation, value, and destination type.
    *
    * @param op The cast operation to perform
    * @param val The value to cast
    * @param destTy The destination type
    * @return Instruction* The created instruction
    */
   Instruction* createICastInstr(ICastInst::CastOp op, Value* val, Type* destTy) {
      return insert(ICastInst::Create(ctx_, op, val, destTy));
   }

   Instruction* createGEPInstr(Value* ptr, Type* t,
                               utils::range_ref<Value*> indices) {
      return insert(GetElementPtrInst::Create(ctx_, ptr, t, indices));
   }

   Instruction* createGEPInstr(Value* ptr, Type* t,
                               utils::range_ref<unsigned> indices) {
      auto& ctx = ptr->ctx();
      const auto ptrWidth = ctx.TI().getPointerSizeInBits();
      // TODO(kevin): one day we can do indices.map<Value(>)
      std::vector<Value*> values;
      indices.for_each([&values, &ctx, &ptrWidth](auto idx) {
         values.push_back(
               ConstantInt::Create(ctx, IntegerType::get(ctx, ptrWidth), idx));
      });
      return insert(GetElementPtrInst::Create(ctx_, ptr, t, values));
   }

private:
   Instruction* insert(Instruction* instr) {
      assert(insertPoint_.getBB() && "No insertion point set");
      if(insertPoint_.isAfterLast()) {
         insertPoint_.getBB()->appendAfterEnd(instr);
      } else if(insertPoint_.isBeforeFirst()) {
         insertPoint_.getBB()->insertBeforeBegin(instr);
      } else {
         instr->insertAfter(*insertPoint_);
      }
      insertPoint_ = instr->iter();
      return instr;
   }

private:
   tir::Context& ctx_;
   BasicBlock::iterator insertPoint_;
};

} // namespace tir
