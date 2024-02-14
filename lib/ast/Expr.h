#pragma once

#include <list>

#include "ast/AstNode.h"
#include "utils/EnumMacros.h"

namespace ast {

class ExprNode;
class Expr : public AstNode {
   std::list<ExprNode> rpn_ops;

public:
   
};

class ExprNode {
public:
   virtual std::string toString() const { return "ExprNode"; }
   virtual ~ExprNode() = default;
};

class MemberName : public ExprNode {
   std::string name;

public:
   MemberName(std::string name) : name{name} {}
};

class ExprOp : public ExprNode {
protected:
   ExprOp(int num_args) : num_args{num_args} {}

private:
   int num_args;
};

class MemberAccess : public ExprOp {
public:
   MemberAccess() : ExprOp(1) {}
};

class MethodInvocation : public ExprOp {
public:
   MethodInvocation(int num_args) : ExprOp(num_args) {}
};

class ClassInstanceCreation : public ExprOp {
public:
   ClassInstanceCreation(int num_args) : ExprOp(num_args) {}
};

class ArrayAccess : public ExprOp {
public:
   ArrayAccess() : ExprOp(2) {}
};

class UnaryOp : public ExprOp {
#define UNARY_OP_TYPE_LIST(F) \
   /* Leaf nodes */           \
   F(Not)                     \
   F(BitwiseNot)              \
   F(Plus)                    \
   F(Minus)
public:
   DECLARE_ENUM(OpType, UNARY_OP_TYPE_LIST)
private:
   DECLARE_STRING_TABLE(OpType, type_strings, UNARY_OP_TYPE_LIST)
#undef UNARY_OP_TYPE_LIST

private:
   OpType type;

public:
   UnaryOp(OpType type) : ExprOp(1), type{type} {}
};

class BinaryOp : public ExprOp {
#define BINARY_OP_TYPE_LIST(F) \
   F(Assignment)               \
   F(GreaterThan)              \
   F(LessThan)                 \
   F(Equal)                    \
   F(LessThanOrEqual)          \
   F(GreaterThanOrEqual)       \
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
private:
   DECLARE_STRING_TABLE(OpType, type_strings, BINARY_OP_TYPE_LIST)
#undef BINARY_OP_TYPE_LIST

private:
   OpType type;

public:
   BinaryOp(OpType type) : ExprOp(2), type{type} {}
};

} // namespace ast
