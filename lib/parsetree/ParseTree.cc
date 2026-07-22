#include "parsetree/ParseTree.h"

#include <algorithm>
#include <iostream>
#include <string>

#include "utils/DotPrinter.h"

namespace parsetree {

bool Literal::isValid() const {
   if(type != Type::Integer) return true;
   errno = 0;
   char* endptr{};
   const long long int x = std::strtol(value.c_str(), &endptr, 10);
   const long long int kIntMax = 2147483647ULL;
   if(errno == ERANGE || *endptr != '\0') return false;
   if(x > kIntMax && !isNegative_) return false;
   if(x > kIntMax + 1 && isNegative_) return false;
   return true;
}

// Leaf node printers //////////////////////////////////////////////////////////

std::string Operator::to_string() const {
   using T = Operator::Type;
   switch(type) {
      case T::Assign:
         return "=";
      case T::GreaterThan:
         return ">";
      case T::LessThan:
         return "<";
      case T::Not:
         return "!";
      case T::Equal:
         return "==";
      case T::LessThanOrEqual:
         return "<=";
      case T::GreaterThanOrEqual:
         return ">=";
      case T::NotEqual:
         return "!=";
      case T::And:
         return "&&";
      case T::Or:
         return "||";
      case T::BitwiseAnd:
         return "&";
      case T::BitwiseOr:
         return "|";
      case T::BitwiseXor:
         return "^";
      case T::BitwiseNot:
         return "~";
      case T::Add:
         return "+";
      case T::Subtract:
         return "-";
      case T::Multiply:
         return "*";
      case T::Divide:
         return "/";
      case T::Modulo:
         return "%";
      case T::Plus:
         return "+";
      case T::Minus:
         return "-";
      case T::InstanceOf:
         return "instanceof";
   }
   return "";
}

std::ostream& Identifier::print(std::ostream& os) const {
   os << "(Id " << name << ")";
   return os;
}

std::ostream& Literal::print(std::ostream& os) const {
   std::string formattedValue{value};
   std::replace(formattedValue.begin(), formattedValue.end(), '\"', ' ');
   os << "(Literal " << Type_to_string(type, "??") << " " << formattedValue << ")";
   return os;
}

std::ostream& Modifier::print(std::ostream& os) const {
   os << "(Modifier " << Type_to_string(modty, "??") << ")";
   return os;
}

std::ostream& BasicType::print(std::ostream& os) const {
   os << "(BasicType " << Type_to_string(type, "??") << ")";
   return os;
}

void Identifier::printDotNode(DotPrinter& dp) const {
   dp.printTableDoubleRow("Id", name);
}

void Literal::printDotNode(DotPrinter& dp) const {
   dp.printTableDoubleRow("Literal", value);
}

void Modifier::printDotNode(DotPrinter& dp) const {
   dp.printTableDoubleRow("Modifier", Type_to_string(modty, "??"));
}

void BasicType::printDotNode(DotPrinter& dp) const {
   dp.printTableDoubleRow("BasicType", Type_to_string(type, "??"));
}

void Operator::printDotNode(DotPrinter& dp) const {
   dp.printTableDoubleRow("Op", to_string());
}

// Node printers ///////////////////////////////////////////////////////////////

std::ostream& Node::print(std::ostream& os) const {
   os << "(";
   os << Type_to_string(type, "Unknown");
   for(size_t i = 0; i < num_args; ++i) {
      os << " ";
      if(args[i] == nullptr) {
         os << "ε";
      } else {
         args[i]->print(os);
      }
   }
   os << ")";
   return os;
}

void Node::printDotNode(DotPrinter& dp) const {
   dp.printTableSingleRow(Type_to_string(this->type, "Unknown"));
   dp.printTableSingleRow(this->loc.toString());
}

int Node::printDotRecursive(DotPrinter& dp, const Node& node) const {
   const int id = dp.id();
   // Print the label first
   if(node.is_marked()) {
      // clang-format off
      dp.startTLabel(id, {
         "color", "red",
      }, "5");
      // clang-format on
   } else if(node.get_node_type() == Node::Type::Poison) {
      // clang-format off
      dp.startTLabel(id, {
         "style", "filled",
         "fillcolor", "red",
         "fontcolor", "white"
      }, "5");
      // clang-format on
   } else if(node.num_children() == 0) {
      // clang-format off
      dp.startTLabel(id, {
         "style", "filled",
         "fillcolor", "lightblue"
      }, "5");
      // clang-format on
   } else {
      dp.startTLabel(id, {}, "5");
   }
   node.printDotNode(dp);
   dp.endTLabel();

   for(size_t i = 0; i < node.num_children(); i++) {
      const Node* child = node.child(i);
      int child_id = 0;
      if(child != nullptr) {
         child_id = printDotRecursive(dp, *child);
      } else {
         child_id = dp.id();
         // clang-format off
         dp.printLabel(child_id, "ε", {
            "shape", "rect",
            "style", "filled",
            "fillcolor", "lightgrey",
            "width", "0.25",
            "height", "0.25",
            "fontsize", "20"
         });
         // clang-format on
      }
      dp.printConnection(id, child_id);
   }
   return id;
}

std::ostream& Node::printDot(std::ostream& os) const {
   DotPrinter dp{os, "35"};
   dp.startGraph();
   printDotRecursive(dp, *this);
   dp.endGraph();
   return os;
}

std::ostream& operator<<(std::ostream& os, Node const& n) { return n.print(os); }

} // namespace parsetree
