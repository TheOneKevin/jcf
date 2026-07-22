#pragma once

#include <stdint.h>
#include <utils/Assert.h>

#include <span>

#include "tir/Context.h"
#include "utils/Utils.h"

namespace tir {

class Value;

/**
 * @brief Type is the base class for all types in the TIR. It is immutable
 * once created and is uniqued within a Context. The uniquing means that
 * two types in the same context are equal if and only if they have the same
 * address in memory.
 */
class Type {
public:
   static Type* getVoidTy(Context const& ctx) { return ctx.pimpl().voidType; }
   static Type* getPointerTy(Context const& ctx) {
      return ctx.pimpl().pointerType;
   }
   static Type* getLabelTy(Context const& ctx) { return ctx.pimpl().labelType; }
   static Type* getInt1Ty(Context& ctx);
   static Type* getInt8Ty(Context& ctx);
   static Type* getInt16Ty(Context& ctx);
   static Type* getInt32Ty(Context& ctx);

public:
   virtual bool isIntegerType() const { return false; }
   virtual bool isFunctionType() const { return false; }
   virtual bool isArrayType() const { return false; }
   virtual bool isStructType() const { return false; }
   virtual bool isBooleanType() const { return false; }
   bool isVoidType() const { return ctx_ && (this == getVoidTy(*ctx_)); }
   bool isPointerType() const { return ctx_ && (this == getPointerTy(*ctx_)); }
   bool isLabelType() const { return ctx_ && (this == getLabelTy(*ctx_)); }
   std::ostream& print(std::ostream& os) const;
   // Get the size of the type in bits
   virtual uint32_t getSizeInBits() const {
      assert(false && "Type does not have a size");
   }
   // Does this type even have a size?
   virtual bool isSizeBounded() const { return false; }
   void dump() const;

protected:
   struct ChildTypeArray {
      Type** array;
      uint32_t size;
   };

protected:
   friend class Context;
   explicit Type(Context* ctx) : Type{0, {}, ctx} {}
   Type(uint32_t data, ChildTypeArray subtypes, Context* ctx)
         : ctx_{ctx}, data_{data}, subtypes_{subtypes} {}
   /**
    * @brief Gets the data associated with this type.
    */
   uint32_t getData() const { return data_; }
   /**
    * @brief Gets the array of child types associated with this type.
    */
   ChildTypeArray getChildTypes() const { return subtypes_; }
   /**
    * @brief Gets the context in which this type is uniqued.
    */
   Context const& ctx() const { return *ctx_; }

private:
   Context const* ctx_ = nullptr;
   const uint32_t data_;
   const ChildTypeArray subtypes_;
};
std::ostream& operator<<(std::ostream& os, const Type& type);

/**
 * @brief The type of a pointer to an opaque type.
 */
class OpaquePointerType final : public Type {
   friend class Context;

private:
   OpaquePointerType(Context* ctx) : Type{ctx} {}

public:
   uint32_t getSizeInBits() const override {
      return ctx().TI().getPointerSizeInBits();
   }
   bool isSizeBounded() const override { return true; }
};

/**
 * @brief
 */
class IntegerType final : public Type {
private:
   IntegerType(Context& ctx, uint32_t bitwidth) : Type{bitwidth, {}, &ctx} {}

public:
   /**
    * @brief Gets an IntegerType with the specified bitwidth. If the type does
    * not exist within the context, a new IntegerType is created and added to
    * the context. Otherwise, the unique IntegerType is returned.
    *
    * @param ctx The context in which this type is uniqued.
    * @param bitwidth The bitwidth of the integer type.
    * @return IntegerType* The unique integer type with the specified bitwidth.
    */
   static IntegerType* get(Context& ctx, uint32_t bitwidth) {
      // First, search ctx for existing IntegerType with bitwidth.
      for(auto* type : ctx.pimpl().integerTypes) {
         if(type->getData() == bitwidth) {
            return type;
         }
      }
      // If not found, create a new IntegerType with bitwidth.
      void* buf =
            ctx.alloc().allocate_bytes(sizeof(IntegerType), alignof(IntegerType));
      auto* type = new(buf) IntegerType{ctx, bitwidth};
      ctx.pimpl().integerTypes.push_back(type);
      return type;
   }

public:
   bool isIntegerType() const override { return true; }
   bool isBooleanType() const override { return getData() == 1; }
   uint32_t getBitWidth() const { return getData(); }
   uint32_t getSizeInBits() const override { return getBitWidth(); }
   bool isSizeBounded() const override { return true; }
   uint64_t getMask() const {
      // Shifting a 64-bit value by 64 is undefined behaviour (on AArch64 the
      // shift amount is taken mod 64, so `1 << 64` yields 1 and the mask would
      // wrongly be 0, zeroing every i64 constant). Handle the full-width case
      // explicitly instead.
      auto bits = getBitWidth();
      return bits >= 64 ? ~0ULL : ((1ULL << bits) - 1);
   }
};

/**
 * @brief
 */
class FunctionType final : public Type {
private:
   FunctionType(Context& ctx, Type** types, uint32_t numTypes)
         : Type{0, Type::ChildTypeArray{types, numTypes}, &ctx} {}

public:
   static FunctionType* get(Context& ctx, Type* returnTy,
                            utils::range_ref<Type*> types) {
      // Grab the array size
      uint32_t size = 1 + types.size();
      // First, search ctx for existing FunctionType with types.
      for(auto* type : ctx.pimpl().functionTypes) {
         auto data = type->getChildTypes();
         if(size != data.size) continue;
         assert(data.size > 0 && "FunctionType has no return type");
         bool typesEqual = returnTy == data.array[0];
         uint32_t i = 1;
         types.for_each([&](Type* ty) {
            if(data.array[i++] != ty) typesEqual = false;
         });
         if(typesEqual) return type;
      }
      // If not found, create a new FunctionType with types.
      void* buf = ctx.alloc().allocate_bytes(sizeof(FunctionType),
                                             alignof(FunctionType));
      // Types are stored after the FunctionType object in memory.
      void* buf2 =
            ctx.alloc().allocate_bytes(size * sizeof(Type*), alignof(Type*));
      auto* typesBuf = static_cast<Type**>(buf2);
      // Copy the types into the buffer.
      {
         uint32_t i = 1;
         typesBuf[0] = returnTy;
         types.for_each([&](Type* ty) { typesBuf[i++] = ty; });
      }
      // Create the FunctionType object.
      auto* type = new(buf) FunctionType{ctx, typesBuf, size};
      ctx.pimpl().functionTypes.push_back(type);
      return type;
   }

public:
   bool isFunctionType() const override { return true; }
   Type* getReturnType() const { return getChildTypes().array[0]; }
   auto getParamTypes() const {
      auto data = getChildTypes();
      return std::span{data.array + 1, data.size - 1};
   }
   auto numParams() const { return getChildTypes().size - 1; }
   auto getParamType(int index) const { return getChildTypes().array[index + 1]; }
   uint32_t getSizeInBits() const override {
      assert(false && "Function type does not have a size");
   }
};

/**
 * @brief
 */
class ArrayType : public Type {
private:
   ArrayType(Context& ctx, Type** elementType, uint32_t numElements)
         : Type{numElements, Type::ChildTypeArray{elementType, 1}, &ctx} {}

public:
   static ArrayType* get(Context& ctx, Type* elementType, uint32_t numElements) {
      // First, search ctx for existing ArrayType with elementType and numElements.
      for(auto* type : ctx.pimpl().arrayTypes) {
         auto data = type->getChildTypes();
         if(data.size == 1 && data.array[0] == elementType &&
            type->getData() == numElements) {
            return type;
         }
      }
      // If not found, create a new ArrayType with elementType and numElements.
      void* buf =
            ctx.alloc().allocate_bytes(sizeof(ArrayType), alignof(ArrayType));
      void* buf2 = ctx.alloc().allocate_bytes(sizeof(Type*), alignof(Type*));
      auto* typeBuf = static_cast<Type**>(buf2);
      typeBuf[0] = elementType;
      auto* type = new(buf) ArrayType{ctx, typeBuf, numElements};
      ctx.pimpl().arrayTypes.push_back(type);
      return type;
   }

public:
   bool isArrayType() const override { return true; }
   Type* getElementType() const { return getChildTypes().array[0]; }
   uint32_t getLength() const { return getData(); }
   uint32_t getSizeInBits() const override {
      assert(isSizeBounded() && "Array type must have a size");
      return getLength() * getElementType()->getSizeInBits();
   }
   bool isSizeBounded() const override { return getLength() != 0; }
};

/**
 * @brief
 */
class StructType final : public Type {
private:
   StructType(Context& ctx, Type** elementTypes, uint32_t numElements)
         : Type{numElements,
                Type::ChildTypeArray{elementTypes, numElements},
                &ctx} {}

public:
   static StructType* get(Context& ctx, utils::range_ref<Type*> elementTypes) {
      uint32_t size = elementTypes.size();
      // Make sure elementTypes are valid
      elementTypes.for_each([](Type* ty) {
         assert(ty && "StructType cannot have null element type");
         assert((!ty->isArrayType() || cast<ArrayType>(ty)->isSizeBounded()) &&
                "StructType element must be a bounded array type");
      });
      // First, search ctx for existing StructType with elementTypes.
      for(auto* type : ctx.pimpl().structTypes) {
         auto data = type->getChildTypes();
         if(data.size == size) {
            bool typesEqual = true;
            uint32_t i = 0;
            elementTypes.for_each([&](Type* ty) {
               if(data.array[i++] != ty) typesEqual = false;
            });
            if(typesEqual) return type;
         }
      }
      // If not found, create a new StructType with elementTypes.
      void* buf =
            ctx.alloc().allocate_bytes(sizeof(StructType), alignof(StructType));
      void* buf2 =
            ctx.alloc().allocate_bytes(size * sizeof(Type*), alignof(Type*));
      auto* typeBuf = static_cast<Type**>(buf2);
      {
         uint32_t i = 0;
         elementTypes.for_each([&](Type* ty) { typeBuf[i++] = ty; });
      }
      auto* type = new(buf) StructType{ctx, typeBuf, size};
      ctx.pimpl().structTypes.push_back(type);
      return type;
   }

public:
   bool isStructType() const override { return true; }
   auto getElements() const {
      auto data = getChildTypes();
      return std::span{data.array, data.size};
   }
   uint32_t numElements() const { return getChildTypes().size; }
   std::ostream& printDetail(std::ostream& os) const;
   uint32_t getSizeInBits() const override {
      uint32_t size = 0;
      for(auto* ty : getElements()) {
         size += ty->getSizeInBits();
      }
      return size;
   }
   bool isSizeBounded() const override { return true; }
   Type* getIndexedType(utils::range_ref<Value*> indices);
   Type* getTypeAtIndex(uint32_t index) { return getChildTypes().array[index]; }
   uint32_t getTypeOffsetAtIndex(uint32_t index) {
      uint32_t offset = 0;
      for(uint32_t i = 0; i < index; ++i) {
         offset += getTypeAtIndex(i)->getSizeInBits();
      }
      return offset;
   }
};

} // namespace tir
