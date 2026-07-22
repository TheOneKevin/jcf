#pragma once

#include <string>
#include <string_view>
#include <variant>
#include <memory_resource>

#include "ast/AstNode.h"
#include "ast/Decl.h"
#include "diagnostics/Location.h"
#include "utils/BumpAllocator.h"
#include "utils/EnumMacros.h"
#include "utils/Generator.h"

namespace ast {
class ExprNodeList;

template <typename T>
class ExprEvaluator;

/* ===--------------------------------------------------------------------=== */
// ExprNode and ExprNodeList
/* ===--------------------------------------------------------------------=== */

class ExprNode {
public:
   explicit ExprNode(SourceRange loc) : next_{nullptr}, loc_{loc} {}
   virtual std::ostream& print(std::ostream& os) const { return os << "ExprNode"; }
   virtual ~ExprNode() = default;

public:
   void setNext(ExprNode* new_next_) {
      assert(!locked_ && "Attempt to mutate locked node");
      next_ = new_next_;
   }
   const ExprNode* next() const { return next_; }
   ExprNode* mut_next() const { return next_; }
   void dump() const;
   SourceRange location() const { return loc_; }

private:
   template <typename T>
   friend class ExprEvaluator;

   void const_lock() const { locked_ = true; }
   void const_unlock() const { locked_ = false; }

private:
   // The next node is mutable because it can be modified on-the-fly during
   // evaluation. That being said, it's the responsibility of the evaluator
   // to ensure the correct order of unlocking nodes.
   ExprNode* next_;

   // The lock for the previous node.
   mutable bool locked_ = false;

   SourceRange loc_;
};

/// @brief A list of ExprNodes* that can be iterated and concatenated
class ExprNodeList final {
public:
   explicit ExprNodeList(ExprNode* node) : head_{node}, tail_{node}, size_{1} {
      node->setNext(nullptr);
   }
   ExprNodeList() : head_{nullptr}, tail_{nullptr}, size_{0} {}
   bool isBracketed = false;

   /**
    * @brief Pushes a node to the back of the list
    * @param node The node to push
    */
   void push_back(ExprNode* node) {
      if(node == nullptr) return;
      if(head_ == nullptr) {
         head_ = node;
         tail_ = node;
      } else {
         tail_->setNext(node);
         tail_ = node;
      }
      node->setNext(nullptr);
      size_ += 1;
      check_invariants();
   }

   /**
    * @brief Concatenates another list to the end of this list.
    * @param other The list to concatenate (rvalue reference)
    */
   void concat(ExprNodeList&& other) {
      if(other.size_ == 0) return;
      if(head_ == nullptr) {
         head_ = other.head_;
         tail_ = other.tail_;
      } else {
         tail_->setNext(other.head_);
         tail_ = other.tail_;
      }
      size_ += other.size_;
      check_invariants();
   }

   /**
    * @brief Concatenates another list to the end of this list. The other list
    * will be invalidated and empty after this operation.
    * @param other The list to concatenate
    */
   void concat(ExprNodeList& other) {
      concat(std::move(other));
      other.head_ = nullptr;
      other.tail_ = nullptr;
      other.size_ = 0;
   }

   /// @brief Returns the number of nodes in the list
   [[nodiscard]] auto size() const { return size_; }

   /// @brief Returns a generator that yields each node in the list
   utils::Generator<ExprNode const*> nodes() const {
      size_t i = 0;
      for(auto const* node = head_; i < size_; node = node->next(), i++) {
         co_yield node;
      }
   }

   /// @brief Non-const version of nodes()
   utils::Generator<ExprNode*> mut_nodes() const {
      size_t i = 0;
      for(auto node = head_; i < size_; node = node->mut_next(), i++) {
         co_yield node;
      }
   }

   ExprNode* mut_head() const { return head_; }
   ExprNode const* tail() const { return tail_; }

   void dump() const;
   std::ostream& print(std::ostream&) const;

private:
   void check_invariants() const;

private:
   ExprNode* head_;
   ExprNode* tail_;
   size_t size_;
};

} // namespace ast

namespace ast::exprnode {

/* ===--------------------------------------------------------------------=== */
// ExprValue Subclasses
/* ===--------------------------------------------------------------------=== */

class ExprValue : public ExprNode {
public:
   explicit ExprValue(SourceRange loc, ast::Type const* type = nullptr)
         : ExprNode{loc}, decl_{nullptr}, type_{type} {}
   ast::Decl const* decl() const { return decl_; }
   virtual bool isDeclResolved() const { return decl_ != nullptr; }
   bool isTypeResolved() const { return type_ != nullptr; }
   void resolveDeclAndType(ast::Decl const* decl, ast::Type const* type) {
      assert(!decl_ && "Tried to resolve expression decl twice");
      decl_ = decl;
      assert(!type_ && "Tried to resolve expression type twice");
      assert((!type || type->isResolved()) &&
             "Tried to resolve expression with unresolved type");
      type_ = type;
   }
   void overrideDecl(ast::Decl const* decl) { decl_ = decl; }
   ast::Type const* type() const { return type_; }

protected:
   ast::Type const* set_type(ast::Type const* type) {
      assert(!type_ && "Tried to set type twice");
      type_ = type;
      return type;
   }

private:
   ast::Decl const* decl_;
   ast::Type const* type_;
};

class MemberName : public ExprValue {
public:
   MemberName(BumpAllocator& alloc, std::string_view name, SourceRange loc)
         : ExprValue{loc}, name_{name, alloc} {}
   std::ostream& print(std::ostream& os) const;
   std::string_view name() const { return name_; }

private:
   std::pmr::string name_;
};

class MethodName : public MemberName {
public:
   MethodName(BumpAllocator& alloc, std::string_view name, SourceRange loc)
         : MemberName{alloc, name, loc} {}
   std::ostream& print(std::ostream& os) const override;
};

class ThisNode final : public ExprValue {
public:
   ThisNode(SourceRange loc) : ExprValue{loc} {}
   std::ostream& print(std::ostream& os) const override;
};

class TypeNode final : public ExprValue {
public:
   TypeNode(Type* type, SourceRange loc) : ExprValue{loc}, unres_type_{type} {}
   void resolveUnderlyingType(TypeResolver& NR) {
      assert(unres_type_ && "Tried to resolve underlying type twice");
      // Resolve underlying type if it's not already resolved
      if(!unres_type_->isResolved()) unres_type_->resolve(NR);

      // NOTE: We cannot assume that unres_type_ is resolved as import-on-demand
      // conflicts will result in unresolved types (null), but is valid.

      set_type(unres_type_);
   }
   bool isDeclResolved() const override { return true; }
   std::ostream& print(std::ostream& os) const override;

private:
   ast::Type* unres_type_;
};

class LiteralNode final : public ExprValue {
public:
   LiteralNode(BumpAllocator&, parsetree::Literal const* node,
               ast::BuiltInType* type, SourceRange loc);
   bool isDeclResolved() const override { return true; }
   std::ostream& print(std::ostream& os) const override;
   ast::BuiltInType const* builtinType() const;
   uint32_t getAsInt() const { return std::get<uint32_t>(value_); }
   auto const& getAsString() const { return std::get<std::pmr::string>(value_); }

private:
   std::variant<uint32_t, std::pmr::string> value_;
};

/* ===--------------------------------------------------------------------=== */
// ExprOp Subclasses
/* ===--------------------------------------------------------------------=== */

class ExprOp : public ExprNode {
protected:
   ExprOp(int num_args, SourceRange loc)
         : ExprNode{loc}, num_args_{num_args}, result_type_{nullptr} {}

public:
   auto nargs() const { return num_args_; }
   std::ostream& print(std::ostream& os) const override = 0;
   ast::Type const* resolveResultType(ast::Type const* type) {
      assert(!result_type_ && "Tried to operator-resolve result type twice");
      assert((!type || type->isResolved()) &&
             "Tried to resolve operator with unresolved type");
      result_type_ = type;
      return type;
   }
   ast::Type const* resultType() const { return result_type_; }

private:
   int num_args_;
   ast::Type const* result_type_;
};

class MemberAccess final : public ExprOp {
public:
   MemberAccess() : ExprOp(2, SourceRange{}) {}
   std::ostream& print(std::ostream& os) const override;
};

class MethodInvocation final : public ExprOp {
public:
   MethodInvocation(int num_args) : ExprOp(num_args, SourceRange{}) {}
   std::ostream& print(std::ostream& os) const override;
};

class ClassInstanceCreation final : public ExprOp {
public:
   ClassInstanceCreation(int num_args) : ExprOp(num_args, SourceRange{}) {}
   std::ostream& print(std::ostream& os) const override;
};

class ArrayInstanceCreation final : public ExprOp {
public:
   ArrayInstanceCreation() : ExprOp(2, SourceRange{}) {}
   std::ostream& print(std::ostream& os) const override;
};

class ArrayAccess final : public ExprOp {
public:
   ArrayAccess() : ExprOp(2, SourceRange{}) {}
   std::ostream& print(std::ostream& os) const override;
};

class Cast final : public ExprOp {
public:
   Cast() : ExprOp(2, SourceRange{}) {}
   std::ostream& print(std::ostream& os) const override;
};

class UnaryOp final : public ExprOp {
#define UNARY_OP_TYPE_LIST(F) \
   /* Leaf nodes */           \
   F(Not)                     \
   F(BitwiseNot)              \
   F(Plus)                    \
   F(Minus)
public:
   DECLARE_ENUM(OpType, UNARY_OP_TYPE_LIST)
   DECLARE_STRING_TABLE(OpType, type_strings, UNARY_OP_TYPE_LIST)
#undef UNARY_OP_TYPE_LIST

private:
   OpType type;

public:
   UnaryOp(OpType type, SourceRange loc) : ExprOp(1, loc), type{type} {}
   std::ostream& print(std::ostream& os) const override;
   OpType opType() const { return type; }
};

class BinaryOp final : public ExprOp {
#define BINARY_OP_TYPE_LIST(F) \
   F(Assignment)               \
   F(GreaterThan)              \
   F(GreaterThanOrEqual)       \
   F(LessThan)                 \
   F(LessThanOrEqual)          \
   F(Equal)                    \
   F(NotEqual)                 \
   F(And)                      \
   F(Or)                       \
   F(BitwiseAnd)               \
   F(BitwiseOr)                \
   F(BitwiseXor)               \
   F(Add)                      \
   F(Subtract)                 \
   F(Multiply)                 \
   F(Divide)                   \
   F(Modulo)                   \
   F(InstanceOf)
public:
   DECLARE_ENUM(OpType, BINARY_OP_TYPE_LIST)
   DECLARE_STRING_TABLE(OpType, type_strings, BINARY_OP_TYPE_LIST)
#undef BINARY_OP_TYPE_LIST

private:
   OpType type;
   // the variable using assigned, only used for assignment
   const ast::VarDecl* varAssigned = nullptr;

public:
   BinaryOp(OpType type, SourceRange loc) : ExprOp(2, loc), type{type} {}
   std::ostream& print(std::ostream& os) const override;
   OpType opType() const { return type; }
   void setVarAssigned(const ast::VarDecl* var) {
      assert(!varAssigned && "Tried to set varAssigned twice");
      varAssigned = var;
   }
   const ast::VarDecl* getVarAssigned() const { return varAssigned; }
};

} // namespace ast::exprnode
