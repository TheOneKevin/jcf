#pragma once

#include <unordered_map>
#include <variant>
#include <vector>
#include <memory_resource>

#include "ast/AstNode.h"
#include "ast/Decl.h"
#include "ast/DeclContext.h"
#include "ast/Stmt.h"
#include "diagnostics/Location.h"
#include "semantic/ConstantTypeResolver.h"
#include "semantic/Semantic.h"
#include "utils/BumpAllocator.h"
#include "utils/DotPrinter.h"

namespace semantic {

using utils::DotPrinter;
class CFGNode {
public:
   struct EmptyExpr {};

private:
   friend class CFGBuilder;
   std::pmr::vector<CFGNode*> children;
   std::pmr::vector<CFGNode*> parents;
   std::variant<const ast::Expr*, const ast::VarDecl*, EmptyExpr> data;
   bool isReturn;
   bool start;
   bool isInfinite;
   mutable bool isVisited;

public:
   CFGNode(BumpAllocator& alloc,
           std::variant<const ast::Expr*, const ast::VarDecl*, EmptyExpr> data,
           bool isReturn = false)
         : children{alloc}, parents{alloc}, data{data}, isReturn{isReturn} {
      start = false;
      isInfinite = false;
      isVisited = false;
   }

   std::ostream& printDot(std::ostream& os) const {
      std::pmr::unordered_map<const CFGNode*, int> visited;
      DotPrinter dp{os};
      dp.startGraph();
      dp.print("compound=true;");
      printDotNode(dp, visited);
      dp.endGraph();
      return os;
   }

   utils::Generator<const CFGNode*> getChildren() const {
      for(auto child : children) {
         co_yield child;
      }
   }

   utils::Generator<const CFGNode*> getParents() const {
      for(auto parent : parents) {
         co_yield parent;
      }
   }

   std::optional<SourceRange> location() const {
      if(std::holds_alternative<const ast::Expr*>(data)) {
         return std::get<const ast::Expr*>(data)->location();
      } else if(std::holds_alternative<const ast::VarDecl*>(data)) {
         return std::get<const ast::VarDecl*>(data)->location();
      } else {
         return std::nullopt;
      }
   }
   std::variant<const ast::Expr*, const ast::VarDecl*, EmptyExpr> getData() const {
      return data;
   }
   bool isReturnNode() const { return isReturn; }
   bool isStart() const { return start; }
   bool isInfiniteLoop() const { return isInfinite; }
   bool hasBeenVisited() const { return isVisited; }
   void setVisited(bool val) const { isVisited = val; }

private:
   int printDotNode(utils::DotPrinter& dp,
                    std::pmr::unordered_map<const CFGNode*, int>& visited) const {
      if(visited.count(this) > 0) {
         return visited[this];
      }
      int id = dp.id();
      dp.startTLabel(id);
      if(start) {
         dp.printTableSingleRow("Start", {"bgcolor", "lightblue"});
      }
      if(isReturn) {
         dp.printTableSingleRow("Return Statement", {"bgcolor", "lightblue"});
      }
      if(std::holds_alternative<const ast::Expr*>(data)) {
         std::ostringstream expr;
         std::get<const ast::Expr*>(data)->print(expr, -1);
         dp.printTableSingleRow("Expr", {"bgcolor", "lightblue"});
         dp.printTableDoubleRow(
               "expr", expr.str(), {"port", "condition"}, {"balign", "left"});
      } else if(std::holds_alternative<const ast::VarDecl*>(data)) {
         auto name = std::get<const ast::VarDecl*>(data)->name();
         dp.printTableSingleRow("VarDecl", {"bgcolor", "lightblue"});
         dp.printTableDoubleRow(
               "expr", name, {"port", "condition"}, {"balign", "left"});
      } else {
         dp.printTableSingleRow("Empty Expr", {"bgcolor", "lightblue"});
      }
      dp.endTLabel();
      visited[this] = id;
      for(auto child : children) {
         dp.printConnection(id, child->printDotNode(dp, visited));
      }
      return id;
   }
};

class CFGBuilder {
   struct CFGInfo {
      CFGNode* head;
      std::pmr::vector<CFGNode*> leafs;
      // Constructor for 1 leaf
      CFGInfo(BumpAllocator& alloc, CFGNode* head, CFGNode* firstLeaf)
            : head{head}, leafs{alloc} {
         leafs.push_back(firstLeaf);
      }
      // Constructor for no leaves
      CFGInfo(BumpAllocator& alloc, CFGNode* head) : head{head}, leafs{alloc} {}
   };

private:
   CFGInfo buildIteratively(const ast::Stmt* stmt, CFGNode* parent = nullptr);
   CFGInfo buildForStmt(const ast::ForStmt* forStmt);
   CFGInfo buildIfStmt(const ast::IfStmt* ifStmt);
   CFGInfo buildDeclStmt(const ast::DeclStmt* declStmt);
   CFGInfo buildExprStmt(const ast::ExprStmt* exprStmt);
   CFGInfo buildReturnStmt(const ast::ReturnStmt* returnStmt);
   CFGInfo buildWhileStmt(const ast::WhileStmt* whileStmt);
   CFGInfo buildBlockStmt(const ast::BlockStatement* blockStmt);

   void connectCFGNode(CFGNode* parent, CFGNode* child);
   void connectLeafsToChild(CFGInfo parent, CFGNode* child);

public:
   CFGBuilder(diagnostics::DiagnosticEngine& diag,
              ConstantTypeResolver* constTypeResolver, BumpAllocator& alloc,
              ast::Semantic& sema)
         : diag{diag},
           alloc{alloc},
           sema{sema},
           constTypeResolver{constTypeResolver} {}
   CFGNode* build(const ast::Stmt* stmt) {
      CFGInfo info = buildIteratively(stmt);
      info.head->start = true;
      return info.head;
   }

private:
   diagnostics::DiagnosticEngine& diag;
   SourceRange loc_;
   BumpAllocator& alloc;
   ast::Semantic& sema;
   ConstantTypeResolver* constTypeResolver;
};
} // namespace semantic