#pragma once

#include <forward_list>
#include <string_view>
#include <memory_resource>

#include "tir/Instructions.h"
#include "tir/Type.h"
#include "tir/Value.h"
#include "utils/BitField.h"
#include "utils/Generator.h"
#include "utils/Utils.h"

namespace tir {

class BasicBlock;
class CompilationUnit;
class Function;
class ConstantInt;

/**
 * @brief
 */
class Constant : public User {
protected:
   Constant(Context& ctx, Type* type) : User{ctx, type} {}

public:
   static ConstantInt* CreateInt(Context& ctx, uint8_t bits, uint32_t value);
   static ConstantInt* CreateBool(Context& ctx, bool value) {
      return CreateInt(ctx, 1, value ? 1 : 0);
   }
   static ConstantInt* CreateInt32(Context& ctx, uint32_t value) {
      return CreateInt(ctx, 32, value);
   }
   static ConstantNullPointer* CreateNullPointer(Context& ctx);
   bool isConstant() const override { return true; }
   virtual bool isNumeric() const { return false; }
   virtual bool isGlobalVariable() const { return false; }
   virtual bool isNullPointer() const { return false; }
   virtual bool isBoolean() const { return false; }
   virtual bool isUndef() const { return false; }
};

/**
 * @brief
 */
class ConstantInt final : public Constant {
private:
   ConstantInt(Context& ctx, Type* type, uint64_t value)
         : Constant{ctx, type}, value_{value} {}

public:
   static ConstantInt* Create(Context& ctx, Type* type, uint64_t value) {
      assert(type->isIntegerType() && "Type must be an integer type");
      auto* buf =
            ctx.alloc().allocate_bytes(sizeof(ConstantInt), alignof(ConstantInt));
      return new(buf) ConstantInt{ctx, type, value};
   }
   static ConstantInt* AllOnes(Context& ctx, Type* type) {
      return Create(ctx, type, ~0ULL);
   }
   static ConstantInt* Zero(Context& ctx, Type* type) {
      return Create(ctx, type, 0);
   }

public:
   std::ostream& print(std::ostream& os) const override;
   uint64_t zextValue() const {
      return value_ & cast<IntegerType>(type())->getMask();
   }
   uint64_t sextValue() const {
      auto mask = cast<IntegerType>(type())->getMask();
      return (value_ & mask) |
             ((value_ & (1ULL << (type()->getSizeInBits() - 1))) ? ~mask : 0);
   }
   bool isNumeric() const override { return true; }
   bool isBoolean() const override { return type()->isBooleanType(); }

private:
   uint64_t value_;
};

/**
 * @brief
 */
class ConstantNullPointer final : public Constant {
private:
   friend class Context;
   ConstantNullPointer(Context& ctx, Type* ty) : Constant{ctx, ty} {}

public:
   static ConstantNullPointer* Create(Context& ctx) {
      return ctx.pimpl().nullPointer;
   }

public:
   std::ostream& print(std::ostream& os) const override;
   bool isNullPointer() const override { return true; }
};

/**
 * @brief
 */
class Undef final : public Constant {
private:
   Undef(Context& ctx, Type* ty) : Constant{ctx, ty} {}

public:
   static Undef* Create(Context& ctx, Type* ty) {
      auto* buf = ctx.alloc().allocate_bytes(sizeof(Undef), alignof(Undef));
      return new(buf) Undef{ctx, ty};
   }

public:
   std::ostream& print(std::ostream& os) const override;
   bool isUndef() const override { return true; }
};

/**
 * @brief
 */
class GlobalObject : public Constant {
protected:
   GlobalObject(Context& ctx, Type* type) : Constant{ctx, type} {}

public:
   virtual bool isExternalLinkage() const = 0;
};

/**
 * @brief
 */
class GlobalVariable final : public GlobalObject {
   friend class CompilationUnit;

private:
   GlobalVariable(Context& ctx, Type* type) : GlobalObject{ctx, type} {}

public:
   std::ostream& print(std::ostream& os) const override;
   bool isExternalLinkage() const override { return false; }
   bool isGlobalVariable() const override { return true; }
   void setInitializer(Function* init) { initializer_ = init; }
   Function* initializer() const { return initializer_; }

private:
   Function* initializer_ = nullptr;
};

/**
 * @brief
 */
class Argument final : public Value {
   friend class Function;

private:
   Argument(Function* parent, Type* type, unsigned index);

public:
   std::ostream& print(std::ostream& os) const override;
   auto* parent() const { return parent_; }
   auto index() const { return index_; }
   bool isFunctionArg() const override { return true; }

private:
   Function* parent_;
   unsigned index_;
};

/**
 * @brief
 */
class Function final : public GlobalObject {
   friend class CompilationUnit;
   friend class BasicBlock;

private:
   Function(Context& ctx, CompilationUnit* parent, FunctionType* type,
            const std::string_view name)
         : GlobalObject{ctx, type}, body_{ctx.alloc()}, parent_{parent} {
      // Set name of the function
      setName(name);
      // Build the argument list
      for(unsigned i = 0; i < type->numParams(); ++i) {
         auto* buf =
               ctx.alloc().allocate_bytes(sizeof(Argument), alignof(Argument));
         auto* arg = new(buf) Argument{this, type->getParamType(i), i};
         addChild(arg);
      }
   }

public:
   union Attrs {
      using T = uint8_t;
      utils::BitField<T, 0, 8 * sizeof(T)> all{0};
      utils::BitField<T, 0, 1> noreturn;
      utils::BitField<T, 1, 1> external;
      utils::BitField<T, 2, 1> intrinsic;
   };

public:
   std::ostream& print(std::ostream& os) const override;
   auto* parent() const { return parent_; }
   auto args() const {
      return std::views::transform(
            children(), [](auto* val) { return static_cast<Argument*>(val); });
   }
   auto numParams() const { return cast<FunctionType>(type())->numParams(); }
   auto getParamType(int index) const {
      return cast<FunctionType>(type())->getParamType(index);
   }
   auto getReturnType() const {
      return cast<FunctionType>(type())->getReturnType();
   }
   bool hasBody() const { return !body_.empty(); }
   /// @brief Gets the entry BB of the function or nullptr if it doesn't exist.
   BasicBlock* getEntryBlock() const { return entryBB_; }
   /// @brief Create a new basic block and add it to the function.
   auto body() const { return std::views::all(body_); }
   void removeBlock(BasicBlock* block);
   bool isFunction() const override { return true; }

   /**
    * @brief Create an alloca instruction for the given type. This will be
    * at the start of the entry BB of the function. If no entry BB exists,
    * this will fail.
    *
    * @param type The type of the alloca instruction.
    * @return AllocaInst* The alloca instruction created.
    */
   AllocaInst* createAlloca(Type* type) {
      assert(getEntryBlock());
      auto* inst = AllocaInst::Create(ctx(), type);
      getEntryBlock()->insertBeforeBegin(inst);
      return inst;
   }
   // Set all function attributes that is true
   void setAttrs(Attrs attrs) {
      attrs_.all =
            static_cast<Attrs::T>(attrs_.all) | static_cast<Attrs::T>(attrs.all);
   }
   // Clear all function attributes that is true
   void clearAttrs(Attrs attrs) {
      attrs_.all =
            static_cast<Attrs::T>(attrs_.all) & ~static_cast<Attrs::T>(attrs.all);
   }
   // Get all function attributes
   inline Attrs attrs() const { return attrs_; }
   // Override the external linkage for global objects
   bool isExternalLinkage() const override { return attrs_.external; }
   // Print the function in DOT format
   void printDot(std::ostream& os) const;
   // Get the reverse post order of the basic blocks
   utils::Generator<BasicBlock*> reversePostOrder() const;
   // Iterates through the set of allocas
   utils::Generator<AllocaInst*> allocas() const {
      if(entryBB_) {
         for(auto* inst : *entryBB_) {
            if(auto* alloca = dyn_cast<AllocaInst>(inst)) {
               co_yield alloca;
            }
         }
      }
   }
   // Gets the n-th argument of the function
   Argument* arg(unsigned index) const {
      return static_cast<Argument*>(getChild(index));
   }

private:
   void addBlock(BasicBlock* block) {
      if(!entryBB_) entryBB_ = block;
      body_.push_front(block);
   }

private:
   std::pmr::forward_list<BasicBlock*> body_;
   BasicBlock* entryBB_ = nullptr;
   tir::CompilationUnit* parent_;
   Attrs attrs_{.all{0}};
};

} // namespace tir
