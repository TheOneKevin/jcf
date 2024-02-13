#include <list>

#include "ParseTreeVisitor.h"

namespace parsetree {

using pty = Node::Type;
using ptv = ParseTreeVisitor;

ast::Expr* ptv::visitExpression(Node* node) {
   std::list<ast::ExprNode> ops = visitExpr(node);
   sem.BuildExpr(ops);
}

std::list<ast::ExprNode> ptv::visitStatementExpression(Node* node) {
   check_node_type(node, pty::StatementExpression);
   check_num_children(node, 1, 1);
}

static ast::BinaryOp convertToBinaryOp(Operator::Type type) {
   using oty = Operator::Type;
   switch(type) {
      case oty::Assign:
         return ast::BinaryOp(ast::BinaryOp::OpType::Assignment);
      case oty::Or:
         return ast::BinaryOp(ast::BinaryOp::OpType::Or);
      case oty::And:
         return ast::BinaryOp(ast::BinaryOp::OpType::And);
      case oty::BitwiseOr:
         return ast::BinaryOp(ast::BinaryOp::OpType::BitwiseOr);
      case oty::BitwiseXor:
         return ast::BinaryOp(ast::BinaryOp::OpType::BitwiseXor);
      case oty::BitwiseAnd:
         return ast::BinaryOp(ast::BinaryOp::OpType::BitwiseAnd);
      case oty::Equal:
         return ast::BinaryOp(ast::BinaryOp::OpType::Equal);
      case oty::NotEqual:
         return ast::BinaryOp(ast::BinaryOp::OpType::NotEqual);
      case oty::LessThan:
         return ast::BinaryOp(ast::BinaryOp::OpType::LessThan);
         break;
      case oty::LessThanOrEqual:
         return ast::BinaryOp(ast::BinaryOp::OpType::LessThanOrEqual);
      case oty::GreaterThan:
         return ast::BinaryOp(ast::BinaryOp::OpType::GreaterThan);
      case oty::GreaterThanOrEqual:
         return ast::BinaryOp(ast::BinaryOp::OpType::GreaterThanOrEqual);
      case oty::InstanceOf:
         return ast::BinaryOp(ast::BinaryOp::OpType::InstanceOf);
      case oty::Add:
         return ast::BinaryOp(ast::BinaryOp::OpType::Add);
      case oty::Subtract:
         return ast::BinaryOp(ast::BinaryOp::OpType::Subtract);
      case oty::Multiply:
         return ast::BinaryOp(ast::BinaryOp::OpType::Multiply);
      case oty::Divide:
         return ast::BinaryOp(ast::BinaryOp::OpType::Divide);
      case oty::Modulo:
         return ast::BinaryOp(ast::BinaryOp::OpType::Modulo);
   }
}

std::list<ast::ExprNode> ptv::visitExpr(parsetree::Node* node) {
   check_node_type(node, pty::Expression);
   check_num_children(node, 1, 3);
   if(node->num_children() == 1) {
      return visitExpr(node->child(0));
   } else if(node->num_children() == 2) {
      // unary expression, add later
   } else if(node->num_children() == 3) {
      // binary expression
      std::list<ast::ExprNode> ops;
      auto left = visitExprChild(node->child(0));
      auto right = visitExprChild(node->child(2));
      ops.splice(ops.end(), left);
      ops.splice(ops.end(), right);
      if(auto op = dynamic_cast<parsetree::Operator*>(node->child(1))) {
         ops.push_back(convertToBinaryOp(op->get_type()));
         return ops;
      } else {
         unreachable();
      }
   }
   unreachable();
}

// expression can have different types of children, so we need to visit them
// possible nodes: expression, literal, THIS, qualifiedIdentifier,
// methodInvocation, TypeNotBasic
//                  arrayAccess, fieldAccess, castExpression,
//                  ArrayCreationExpression ClassInstanceCreationExpression
std::list<ast::ExprNode> ptv::visitExprChild(Node* node) {
   if(node->get_node_type() == pty::Expression) {
      return visitExpr(node);
   }
   if(node->get_node_type() == pty::Literal) {
      // return visitLiteral(node);
   }
   if (node->get_node_type() == pty::Type) {
      
   }
   if(node->get_node_type() == pty::Identifier) {
      std::string name = visitIdentifier(node);
      if (name == "this") {
         return std::list<ast::ExprNode>({ast::ThisNode()});
      }
      return std::list<ast::ExprNode>({ast::MemberName(name)});
   }
   if(node->get_node_type() == pty::QualifiedIdentifier) {
      return visitQualifiedIdentifierInExpr(node);
   }
   if (node->get_node_type() == pty::ArrayCastType) {
      return visitQualifiedIdentifierInExpr(node->child(0));
   }
   if(node->get_node_type() == pty::MethodInvocation) {
      return visitMethodInvocation(node);
   }
   if(node->get_node_type() == pty::ArrayAccess) {
      return visitArrayAccess(node);
   }
   if(node->get_node_type() == pty::FieldAccess) {
      return visitFieldAccess(node);
   }
   if(node->get_node_type() == pty::CastExpression) {
      return visitCastExpression(node);
   }
   if(node->get_node_type() == pty::ArrayCreationExpression) {
      return visitArrayCreation(node);
   }
   if(node->get_node_type() == pty::ClassInstanceCreationExpression) {
      return visitClassCreation(node);
   }
   unreachable();
}

std::list<ast::ExprNode> ptv::visitQualifiedIdentifierInExpr(Node* node) {
   check_node_type(node, pty::QualifiedIdentifier);
   check_num_children(node, 1, 2);
   std::list<ast::ExprNode> ops;
   if(node->num_children() == 1) {
      ops.push_back(ast::MemberName(visitIdentifier(node->child(1))));
   } else if(node->num_children() == 2) {
      ops = visitQualifiedIdentifierInExpr(node->child(0));
      ops.push_back(ast::MemberName(visitIdentifier(node->child(1))));
      ops.push_back(ast::MemberAccess());
   }
   return ops;
}

std::list<ast::ExprNode> ptv::visitMethodInvocation(Node* node) {
   check_node_type(node, pty::MethodInvocation);
   check_num_children(node, 2, 3);
   std::list<ast::ExprNode> ops;
   if(node->num_children() == 2) {
      ops.splice(ops.end(), visitQualifiedIdentifierInExpr(node->child(0)));

      std::list<ast::ExprNode> args;
      visitArgumentList(node->child(1), args);
      ops.splice(ops.end(), args);

      ops.push_back(ast::MethodInvocation(args.size() + 1));
      return ops;
   } else if(node->num_children() == 3) {
      ops.splice(ops.end(), visitExprChild(node->child(0)));
      ops.push_back(ast::MemberName(visitIdentifier(node->child(1))));
      ops.push_back(ast::MemberAccess());

      std::list<ast::ExprNode> args;
      visitArgumentList(node->child(2), args);
      ops.splice(ops.end(), args);

      ops.push_back(ast::MethodInvocation(args.size() + 1));
      return ops;
   }
   unreachable();
}

std::list<ast::ExprNode> ptv::visitFieldAccess(Node* node) {
   check_node_type(node, pty::FieldAccess);
   check_num_children(node, 2, 2);
   std::list<ast::ExprNode> ops;
   ops.splice(ops.end(), visitExprChild(node->child(0)));
   ops.push_back(ast::MemberName(visitIdentifier(node->child(1))));
   ops.push_back(ast::MemberAccess());
   return ops;
}

std::list<ast::ExprNode> ptv::visitClassCreation(Node* node) {
   check_node_type(node, pty::ClassInstanceCreationExpression);
   check_num_children(node, 2, 2);
   std::list<ast::ExprNode> ops;
   ops.push_back(ast::MemberName(visitIdentifier(node->child(0))));
   std::list<ast::ExprNode> args;
   visitArgumentList(node->child(1), args);
   ops.splice(ops.end(), args);
   ops.push_back(ast::ClassInstanceCreation(args.size() + 1));
   return ops;
}

std::list<ast::ExprNode> ptv::visitArrayAccess(Node* node) {
   check_node_type(node, pty::ArrayAccess);
   check_num_children(node, 2, 2);
   std::list<ast::ExprNode> ops;
   ops.splice(ops.end(), visitExprChild(node->child(0)));
   ops.splice(ops.end(), visitExpr(node->child(1)));
   ops.push_back(ast::ArrayAccess());
   return ops;
}

std::list<ast::ExprNode> ptv::visitCastExpression(Node* node) {
   check_node_type(node, pty::CastExpression);
   check_num_children(node, 2, 3);
   std::list<ast::ExprNode> ops;
   auto type = visitExprChild(node->child(0));
   ops.splice(ops.end(), type);
   if (node->num_children() == 3 && node->child(1) != nullptr) {
      ops.push_back(ast::ArrayTypeNode());
   }
   auto expr = visitExprChild(node->child(1));
   ops.splice(ops.end(), type);
   ops.push_back(ast::Cast());
   return ops;
}

void ptv::visitArgumentList(Node* node, std::list<ast::ExprNode>& ops) {
   if(node == nullptr) return;
   check_node_type(node, pty::ArgumentList);
   check_num_children(node, 1, 2);
   if(node->num_children() == 1) {
      ops.splice(ops.end(), visitExpr(node->child(0)));
   } else if(node->num_children() == 2) {
      visitArgumentList(node->child(0), ops);
      ops.splice(ops.end(), visitExpr(node->child(1)));
   }
   return;
}

} // namespace parsetree
