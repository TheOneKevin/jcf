#include <ast/AST.h>
#include <ast/Expr.h>

#include <cctype>
#include <ostream>
#include <memory_resource>

#include "ast/ExprNode.h"

namespace ast {

std::ostream& Expr::print(std::ostream& os, int indentation) const {
   if(indentation >= 0) {
      os << AstNode::indent(indentation);
      os << "Expr {\n";
   }
   os << AstNode::indent(indentation + 1);
   char state = '(';
   for(const auto op : rpn_ops.nodes()) {
      if(indentation >= 0) {
         std::ostringstream oss;
         op->print(oss);
         char cur = (oss.str().begin()[0] == '(') ? '(' : '.';
         if(cur == state) {
            os << oss.str() << " ";
         } else {
            os << "\n" << AstNode::indent(indentation + 1) << oss.str() << " ";
         }
         state = cur;
      } else {
         op->print(os);
         os << "\n";
      }
   }
   if(indentation >= 0) {
      os << "\n" << AstNode::indent(indentation) << "}\n";
   }
   return os;
}

int Expr::printDotNode(DotPrinter& dp) const {
   std::ostringstream os;
   for(const auto op : rpn_ops.nodes()) {
      op->print(os);
      if(os.str().ends_with(")")) {
         os << "\n";
      } else {
         os << " ";
      }
   }
   dp.sanitize(os.str());
   return -1;
}

void Expr::dump() const { this->print(std::cerr, 0); }

/* ===--------------------------------------------------------------------=== */
// ExprNode Subclasses
/* ===--------------------------------------------------------------------=== */

void ExprNode::dump() const { this->print(std::cerr) << "\n"; }

void ExprNodeList::dump() const { print(std::cerr); }

std::ostream& ExprNodeList::print(std::ostream& os) const {
   for(auto node : nodes()) {
      node->print(os);
      os << " ";
   }
   os << "\n";
   return os;
}

void ExprNodeList::check_invariants() const {
   assert((!tail_ || tail_->next() == nullptr) &&
          "Tail node should not have a next node");
   assert(((head_ != nullptr) == (tail_ != nullptr)) &&
          "Head is null if and only if tail is null");
   assert(((head_ == nullptr) == (size_ == 0)) &&
          "Size should be 0 if and only if head is null");
}

namespace exprnode {

std::ostream& MemberName::print(std::ostream& os) const {
   return os << "(MemberName " << name_ << ")";
}

std::ostream& MethodName::print(std::ostream& os) const {
   return os << "(MethodName " << name() << ")";
}

std::ostream& ThisNode::print(std::ostream& os) const { return os << "(THIS)"; }

std::ostream& TypeNode::print(std::ostream& os) const {
   os << "(Type ";
   if(type())
      type()->print(os);
   else
      os << "unresolved ❌";
   return os << ")";
}

static uint8_t parseChar(std::string_view value) {
   // Consume ' first, next value is either \ or a character
   // If it is a character, just return that character
   if(value.at(1) != '\\') return (uint8_t)value.at(1);
   // Here, we have an escape sequence, let's first handle the octal case
   if(isdigit(value.at(2))) {
      // We have an octal escape sequence of 1 to 3 digits
      uint8_t octal[3] = {0, 0, 0};
      // 1 digit '\0
      octal[0] = value.at(2) - '0';
      // 2 digits '\00
      if(value.length() >= 4) octal[1] = value.at(3) - '0';
      // 3 digits '\000
      if(value.length() >= 5) octal[2] = value.at(4) - '0';
      // Must consume ' then return the character
      return (uint8_t)((octal[0] << 6) | (octal[1] << 3) | octal[2]);
   }
   // Here, we have a non-octal escape sequence
   switch(value.at(2)) {
      case 'n':
         return '\n';
      case 't':
         return '\t';
      case 'r':
         return '\r';
      case 'b':
         return '\b';
      case 'f':
         return '\f';
      case '\\':
         return '\\';
      case '\'':
         return '\'';
      case '\"':
         return '\"';
      default:
         assert(false && "Invalid escape sequence");
   }
}

static void unescapeString(std::string_view in, std::pmr::string& out) {
   // FIXME(kevin): String literals are broken for now :)
   for(size_t i = 1; i < in.length(); i++) {
      char c = in.at(i);
      if(c == '\"') {
         break;
      } else {
         out.push_back(c);
      }
   }
}

LiteralNode::LiteralNode(BumpAllocator& alloc, parsetree::Literal const* node,
                         ast::BuiltInType* type, SourceRange loc)
      : ExprValue{loc, static_cast<ast::Type*>(type)} {
   auto str = node->get_value();

   // 1. Check if the type is numeric
   if(type->isNumeric()) {
      uint32_t value = 0;
      if(type->getKind() == ast::BuiltInType::Kind::Char) {
         value = parseChar(str);
      } else {
         // Convert the string to an integer
         try {
            if(node->isNegative())
               value = std::stoi("-" + std::string(str));
            else
               value = std::stoi(std::string(str));
         } catch(std::invalid_argument& e) {
            assert(false && "Invalid integer literal");
         }
      }
      value_ = value;
   }
   // 2. Otherwise, check if the type is boolean
   else if(type->isBoolean()) {
      if(str == "true") {
         value_ = 1U;
      } else if(str == "false") {
         value_ = 0U;
      } else {
         assert(false && "Invalid boolean literal");
      }
   }
   // 3. Otherwise, its a string
   else if(type->isString()) {
      // Unescape the string
      value_ = std::pmr::string{alloc};
      unescapeString(str, std::get<std::pmr::string>(value_));
   }
   // 4. Maybe it's a NoneType (i.e., NULL)
   else if(type->getKind() == ast::BuiltInType::Kind::NoneType) {
      value_ = 0U;
   }
   // 5. Otherwise, it's an invalid type
   else {
      assert(false && "Invalid type for literal node");
   }
}

std::ostream& LiteralNode::print(std::ostream& os) const {
   // TODO(kevin): re-implement this
   os << "(Literal ";
   builtinType()->print(os) << ")";
   return os;
}

ast::BuiltInType const* LiteralNode::builtinType() const {
   return cast<ast::BuiltInType>(type());
}

std::ostream& MemberAccess::print(std::ostream& os) const {
   return os << "MemberAccess";
}

std::ostream& MethodInvocation::print(std::ostream& os) const {
   return os << "MethodInvocation(" << nargs() - 1 << ")";
}

std::ostream& ClassInstanceCreation::print(std::ostream& os) const {
   return os << "(ClassInstanceCreation args: " << std::to_string(nargs()) << ")";
}

std::ostream& ArrayInstanceCreation::print(std::ostream& os) const {
   return os << "ArrayInstanceCreation";
}

std::ostream& ArrayAccess::print(std::ostream& os) const {
   return os << "ArrayAccess";
}

std::ostream& Cast::print(std::ostream& os) const { return os << "Cast"; }

std::ostream& UnaryOp::print(std::ostream& os) const {
   return os << OpType_to_string(type, "(Unknown unary op)");
}

std::ostream& BinaryOp::print(std::ostream& os) const {
   return os << OpType_to_string(type, "(Unknown binary op)");
}

} // namespace exprnode

} // namespace ast