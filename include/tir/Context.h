#pragma once

#include <memory_resource>
#include "utils/BumpAllocator.h"
#include "target/TargetInfo.h"

namespace tir {

class Type;
class ConstantNullPointer;
class FunctionType;
class ArrayType;
class IntegerType;
class StructType;

struct ContextPImpl {
public:
   ContextPImpl(BumpAllocator& alloc, Type* const pointerType,
                Type* const voidType, Type* const labelType,
                ConstantNullPointer* const nullPointer)
         : functionTypes(alloc),
           arrayTypes(alloc),
           integerTypes(alloc),
           structTypes(alloc),
           pointerType(pointerType),
           voidType(voidType),
           labelType(labelType),
           nullPointer(nullPointer) {}

public:
   std::pmr::vector<FunctionType*> functionTypes;
   std::pmr::vector<ArrayType*> arrayTypes;
   std::pmr::vector<IntegerType*> integerTypes;
   std::pmr::vector<StructType*> structTypes;
   Type* const pointerType;
   Type* const voidType;
   Type* const labelType;
   ConstantNullPointer* const nullPointer;
};

class Context {
public:
   Context(BumpAllocator& alloc, target::TargetInfo& TI);
   Context(const Context&) = delete;
   Context(Context&&) = delete;
   Context& operator=(const Context&) = delete;
   Context& operator=(Context&&) = delete;

   BumpAllocator& alloc() const { return alloc_; }
   ContextPImpl& pimpl() { return *pimpl_; }
   ContextPImpl const& pimpl() const { return *pimpl_; }
   unsigned getNextValueID() { return value_counter++; }
   auto const& TI() const { return TI_; }

private:
   BumpAllocator& alloc_;
   target::TargetInfo& TI_;
   ContextPImpl* pimpl_;
   unsigned value_counter = 0;
};

} // namespace tir