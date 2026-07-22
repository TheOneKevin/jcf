#pragma once

#include <ranges>
#include <string_view>
#include <variant>

#include "tir/BasicBlock.h"
#include "tir/Context.h"
#include "tir/Type.h"
#include "tir/Value.h"
#include "utils/EnumMacros.h"
#include "utils/Generator.h"
#include "utils/Utils.h"

namespace tir {

class Function;
class CompilationUnit;

/* ===--------------------------------------------------------------------=== */
// Instruction base class
/* ===--------------------------------------------------------------------=== */

/**
 * @brief Base class for all instructions in the TIR. Instructions are
 * also Values, but they are not constants. Instructions are stored as a linked
 * list, possibly belonging to a BasicBlock, or not. Instructions are also
 * Users, meaning they can have other Values as operands (children).
 */
class Instruction : public User {
   friend class BasicBlock;
   // Public enum definitions //////////////////////////////////////////////////
public:
#define BINOP_KINDS(F) \
   F(None)             \
   F(Add)              \
   F(Sub)              \
   F(Mul)              \
   F(Div)              \
   F(Rem)              \
   F(And)              \
   F(Or)               \
   F(Xor)
   DECLARE_ENUM(BinOp, BINOP_KINDS)
protected:
   DECLARE_STRING_TABLE(BinOp, binop_strtab_, BINOP_KINDS)
#undef BINOP_KINDS

public:
#define PREDICATE_KINDS(F) \
   F(EQ)                   \
   F(NE)                   \
   F(LT)                   \
   F(GT)                   \
   F(LE)                   \
   F(GE)
   DECLARE_ENUM(Predicate, PREDICATE_KINDS)
protected:
   DECLARE_STRING_TABLE(Predicate, predicate_strtab_, PREDICATE_KINDS)
#undef PREDICATE_KINDS

public:
#define CAST_KINDS(F) \
   F(Trunc)           \
   F(ZExt)            \
   F(SExt)
   DECLARE_ENUM(CastOp, CAST_KINDS)
protected:
   DECLARE_STRING_TABLE(CastOp, castop_strtab_, CAST_KINDS)
#undef CAST_KINDS

public:
#define INTRINSIC_KINDS(F)                       \
   F(malloc, jcf.malloc)                         \
   F(exception, jcf.exception)                   \
   F(check_array_bounds, jcf.check.array_bounds) \
   F(check_null, jcf.check.null)

   DECLARE_ENUM_2(IntrinsicKind, INTRINSIC_KINDS)
protected:
   DECLARE_STRING_TABLE_2(IntrinsicKind, intrinsic_strtab_, INTRINSIC_KINDS)
#undef INTRINSIC_KINDS

   // Protected data type and getter ///////////////////////////////////////////
protected:
   using DataType = std::variant<BinOp, Predicate, CastOp, Type*>;
   template <typename T>
   constexpr T const& get() const {
      return std::get<T>(data_);
   }

   // Constructors and public member functions /////////////////////////////////
public:
   Instruction(Context& ctx, tir::Type* resultTy)
         : Instruction{ctx, resultTy, BinOp::None} {}
   Instruction(Context& ctx, tir::Type* resultTy, DataType data)
         : User{ctx, resultTy},
           next_{nullptr},
           prev_{nullptr},
           data_{data},
           parent_{nullptr} {}
   Instruction(const Instruction&) = delete;
   Instruction(Instruction&&) = delete;
   Instruction& operator=(const Instruction&) = delete;
   Instruction& operator=(Instruction&&) = delete;
   // Returns true if this instruction is a terminator instruction
   virtual bool isTerminator() const { return false; }
   // Inserts this before the given instruction, setting the parent of this
   void insertBefore(Instruction* inst) {
      assert(!isDestroyed() && "Instruction is already destroyed");
      prev_ = inst->prev_;
      next_ = inst;
      if(prev_) {
         prev_->next_ = this;
      }
      inst->prev_ = this;
      parent_ = inst->parent_;
      // If this is the first instruction in the BB, update the BB's first pointer
      if(!prev_ && parent_) {
         parent_->first_ = this;
      }
   }
   // Inserts this after the given instruction, setting the parent of this
   void insertAfter(Instruction* inst) {
      assert(!isDestroyed() && "Instruction is already destroyed");
      next_ = inst->next_;
      prev_ = inst;
      if(next_) {
         next_->prev_ = this;
      }
      inst->next_ = this;
      parent_ = inst->parent_;
      // If this is the last instruction in the BB, update the BB's last pointer
      if(!next_ && parent_) {
         parent_->last_ = this;
      }
   }
   // Gets the parent BB of this instruction, or nullptr if it has no parent
   auto* parent() const { return parent_; }
   // Gets the next instruction in the BB, or nullptr if this is the last
   Instruction* next() const { return next_; }
   // Gets the previous instruction in the BB, or nullptr if this is the first
   Instruction* prev() const { return prev_; }
   // Gets the iterator to the next instruction in the BB
   auto nextIter() {
      return BasicBlock::iterator{
            next_ ? next_ : this, this->parent_, !next_, false};
   }
   // Gets the iterator to the previous instruction in the BB
   auto prevIter() {
      return BasicBlock::iterator{
            prev_ ? prev_ : this, this->parent_, false, !prev_};
   }
   // Gets the current iterator to this instruction
   auto iter() { return BasicBlock::iterator{this, this->parent_, false, false}; }
   /**
    * @brief Removes this instruction from its parent BB if it exists. Also
    * will unlink this instruction from the list, re-linking the previous and
    * next instructions.
    */
   void eraseFromParent(bool keep = false) {
      assert(!isDestroyed() && "Instruction is already destroyed");
      if(prev_) {
         prev_->next_ = next_;
      }
      if(next_) {
         next_->prev_ = prev_;
      }
      if(parent_) {
         if(parent_->first_ == this) {
            parent_->first_ = next_;
         }
         if(parent_->last_ == this) {
            parent_->last_ = prev_;
         }
      }
      // Destroy all references to the parent BB
      next_ = prev_ = nullptr;
      if(!keep) destroy();
   }
   // Sets the parent BB of this instruction
   void setParent(BasicBlock* parent) { parent_ = parent; }
   // Is this Value an Instruction?
   bool isInstruction() const override { return true; }
   // Gets the name of the intrinsic kind
   static inline std::string_view getIntrinsicName(IntrinsicKind kind) {
      return IntrinsicKind_to_string(kind, "??");
   }
   // Does this instruction have side effects (other than users and control flow)?
   virtual bool hasSideEffects() const { return false; }

   // Private data members /////////////////////////////////////////////////////
private:
   Instruction* next_;
   Instruction* prev_;
   const DataType data_;
   BasicBlock* parent_;
};

void RegisterAllIntrinsics(CompilationUnit& cu);

/* ===--------------------------------------------------------------------=== */
// Terminal instructions
/* ===--------------------------------------------------------------------=== */

/**
 * @brief Conditional branch instruction. This instruction is a terminator
 * and branches to one of two basic blocks based on the condition. The
 * condition value must be an i1 type.
 */
class BranchInst final : public Instruction {
private:
   BranchInst(Context& ctx, Value* cond, BasicBlock* trueBB, BasicBlock* falseBB);

public:
   static BranchInst* Create(Context& ctx, Value* cond, BasicBlock* trueBB,
                             BasicBlock* falseBB) {
      auto buf =
            ctx.alloc().allocate_bytes(sizeof(BranchInst), alignof(BranchInst));
      return new(buf) BranchInst{ctx, cond, trueBB, falseBB};
   }

public:
   bool isTerminator() const override { return true; }
   BasicBlock* getSuccessor(unsigned idx) const;
   std::ostream& print(std::ostream& os) const override;
   void replaceSuccessor(unsigned idx, BasicBlock* newBB) {
      assert(idx < 2 && "Index out of bounds");
      replaceChild(idx + 1, newBB);
   }
   Value* getCondition() const { return getChild(0); }
};

/**
 * @brief Return instruction. This instruction is a terminator and returns
 * either a value or nothing (void).
 */
class ReturnInst final : public Instruction {
private:
   ReturnInst(Context& ctx, Value* ret);

public:
   static ReturnInst* Create(Context& ctx, Value* ret) {
      auto buf =
            ctx.alloc().allocate_bytes(sizeof(ReturnInst), alignof(ReturnInst));
      return new(buf) ReturnInst{ctx, ret};
   }

public:
   bool isReturnVoid() const { return numChildren() == 0; }
   bool isTerminator() const override { return true; }
   std::ostream& print(std::ostream& os) const override;
};

/* ===--------------------------------------------------------------------=== */
// Memory instructions
/* ===--------------------------------------------------------------------=== */

/**
 * @brief Store instruction. This instruction stores a value to a pointer.
 */
class StoreInst final : public Instruction {
private:
   StoreInst(Context& ctx, Value* val, Value* ptr);

public:
   static StoreInst* Create(Context& ctx, Value* val, Value* ptr) {
      auto buf = ctx.alloc().allocate_bytes(sizeof(StoreInst), alignof(StoreInst));
      return new(buf) StoreInst{ctx, val, ptr};
   }

public:
   std::ostream& print(std::ostream& os) const override;
   bool hasSideEffects() const override { return true; }
};

/**
 * @brief Load instruction. This instruction loads a value from a pointer.
 * The size of the value loaded is determined by the type of the load instr,
 * not by the type of the pointer. Pointer types are opaque.
 */
class LoadInst final : public Instruction {
private:
   LoadInst(Context& ctx, Type* type, Value* ptr);

public:
   static LoadInst* Create(Context& ctx, Type* type, Value* ptr) {
      auto buf = ctx.alloc().allocate_bytes(sizeof(LoadInst), alignof(LoadInst));
      return new(buf) LoadInst{ctx, type, ptr};
   }

public:
   std::ostream& print(std::ostream& os) const override;
};

/* ===--------------------------------------------------------------------=== */
// Function call instruction
/* ===--------------------------------------------------------------------=== */

/**
 * @brief Call instruction. This instruction calls a function with the given
 * arguments. It returns the result of the function call.
 */
class CallInst final : public Instruction {
private:
   CallInst(Context& ctx, Value* callee, utils::range_ref<Value*> args);
   // Indirect form: the callee is an arbitrary value (e.g. a loaded function
   // pointer) whose type is not a FunctionType, so the result type must be
   // supplied explicitly rather than derived from the callee.
   CallInst(Context& ctx, Type* retTy, Value* calleePtr,
            utils::range_ref<Value*> args);

public:
   static CallInst* Create(Context& ctx, Value* callee,
                           utils::range_ref<Value*> args) {
      auto buf = ctx.alloc().allocate_bytes(sizeof(CallInst), alignof(CallInst));
      return new(buf) CallInst{ctx, callee, args};
   }
   // Create an indirect call through `calleePtr` (a `ptr` value) returning
   // `retTy`. Used for virtual dispatch through vtable slots.
   static CallInst* CreateIndirect(Context& ctx, Type* retTy, Value* calleePtr,
                                   utils::range_ref<Value*> args) {
      auto buf = ctx.alloc().allocate_bytes(sizeof(CallInst), alignof(CallInst));
      return new(buf) CallInst{ctx, retTy, calleePtr, args};
   }

public:
   std::ostream& print(std::ostream& os) const override;
   bool isTerminator() const override;
   // The callee as a Function, or null when this is an indirect call.
   Function* getCallee() const;
   // The callee operand as a raw value (a Function for direct calls, or the
   // called pointer for indirect calls). Always non-null.
   Value* getCalleeValue() const { return getChild(0); }
   bool isIndirect() const { return getCallee() == nullptr; }
   utils::Generator<Value*> args() const;
   auto nargs() const { return numChildren() - 1; }
   bool hasSideEffects() const override { return true; }
};

/* ===--------------------------------------------------------------------=== */
// Arithmetic and logic operators
/* ===--------------------------------------------------------------------=== */

/**
 * @brief Binary instruction. This instruction performs a binary operation
 * on two values. The type of the result is the same as the type of the
 * two operands (which must be the same type also).
 */
class BinaryInst final : public Instruction {
private:
   BinaryInst(Context& ctx, BinOp binop, Value* lhs, Value* rhs);

public:
   static BinaryInst* Create(Context& ctx, BinOp binop, Value* lhs, Value* rhs) {
      auto buf =
            ctx.alloc().allocate_bytes(sizeof(BinaryInst), alignof(BinaryInst));
      return new(buf) BinaryInst{ctx, binop, lhs, rhs};
   }

public:
   std::ostream& print(std::ostream& os) const override;
   BinOp binop() const { return get<BinOp>(); }
};

/**
 * @brief Compare instruction. This instruction compares two values (of the same
 * type) and returns a boolean value (i1) based on the comparison.
 */
class CmpInst final : public Instruction {
private:
   CmpInst(Context& ctx, Predicate pred, Value* lhs, Value* rhs);

public:
   static CmpInst* Create(Context& ctx, Predicate pred, Value* lhs, Value* rhs) {
      auto buf = ctx.alloc().allocate_bytes(sizeof(CmpInst), alignof(CmpInst));
      return new(buf) CmpInst{ctx, pred, lhs, rhs};
   }

public:
   std::ostream& print(std::ostream& os) const override;
   Predicate predicate() const { return get<Predicate>(); }
};

/**
 * @brief Integer cast instruction. This instruction can either truncate,
 * zero-extend, or sign-extend an integer value to a different integer type.
 */
class ICastInst final : public Instruction {
private:
   ICastInst(Context& ctx, CastOp op, Value* val, Type* destTy);

public:
   static ICastInst* Create(Context& ctx, CastOp op, Value* val, Type* destTy) {
      auto buf = ctx.alloc().allocate_bytes(sizeof(ICastInst), alignof(ICastInst));
      return new(buf) ICastInst{ctx, op, val, destTy};
   }

public:
   std::ostream& print(std::ostream& os) const override;
   CastOp castop() const { return get<CastOp>(); }
};

/* ===--------------------------------------------------------------------=== */
// Alloca instruction
/* ===--------------------------------------------------------------------=== */

/**
 * @brief Alloca instruction. This instruction allocates memory on the stack.
 * This is equivalent to the TIR TEMP() node.
 */
class AllocaInst final : public Instruction {
private:
   AllocaInst(Context& ctx, Type* type);

public:
   static AllocaInst* Create(Context& ctx, Type* type) {
      auto buf =
            ctx.alloc().allocate_bytes(sizeof(AllocaInst), alignof(AllocaInst));
      return new(buf) AllocaInst{ctx, type};
   }

public:
   std::ostream& print(std::ostream& os) const override;
   Type* allocatedType() const { return get<Type*>(); }
};

/* ===--------------------------------------------------------------------=== */
// GetElementPtr instruction
/* ===--------------------------------------------------------------------=== */

class GetElementPtrInst final : public Instruction {
private:
   GetElementPtrInst(Context& ctx, Value* ptr, Type* structTy,
                     utils::range_ref<Value*> indices);

public:
   static GetElementPtrInst* Create(Context& ctx, Value* ptr, Type* ty,
                                    utils::range_ref<Value*> indices) {
      auto buf = ctx.alloc().allocate_bytes(sizeof(GetElementPtrInst),
                                            alignof(GetElementPtrInst));
      return new(buf) GetElementPtrInst{ctx, ptr, ty, indices};
   }

public:
   std::ostream& print(std::ostream& os) const override;
   auto getContainedType() const { return get<Type*>(); }
   auto indices() const {
      return children() |
             std::views::transform([](auto* v) { return cast<Value>(v); }) |
             std::views::drop(1);
   }
   Value* getPointerOperand() const { return getChild(0); }
};

/* ===--------------------------------------------------------------------=== */
// PhiNode instruction
/* ===--------------------------------------------------------------------=== */

class PhiNode final : public Instruction {
private:
   PhiNode(Context& ctx, Type* type, utils::range_ref<Value*> values,
           utils::range_ref<BasicBlock*> preds);

public:
   static PhiNode* Create(Context& ctx, Type* type,
                          utils::range_ref<Value*> values,
                          utils::range_ref<BasicBlock*> preds) {
      auto buf = ctx.alloc().allocate_bytes(sizeof(PhiNode), alignof(PhiNode));
      return new(buf) PhiNode{ctx, type, values, preds};
   }

   struct IncomingValue {
      Value* value;
      BasicBlock* pred;
   };

public:
   std::ostream& print(std::ostream& os) const override;
   void replaceOrAddOperand(BasicBlock* pred, Value* val);
   utils::Generator<IncomingValue> incomingValues() const;
};

} // namespace tir
