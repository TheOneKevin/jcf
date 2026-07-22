#include "semantic/DataflowAnalysis.h"

#include <algorithm>
#include <iostream>
#include <unordered_map>
#include <variant>
#include <set>
#include <memory_resource>

#include "ast/Decl.h"
#include "diagnostics/Location.h"

namespace semantic {

using DFA = DataflowAnalysis;
void DFA::Check() const {
   for(auto cu : lu->compliationUnits()) {
      if(auto classDecl = dyn_cast_or_null<ast::ClassDecl>(cu->body())) {
         for(auto method : classDecl->methods()) {
            if(method->body()) {
               auto cfg = cfgBuilder->build(method->body());
               // cfg->printDot(std::cout);
               ReachabilityCheck(cfg);
               LiveVariableAnalysis(cfg);

               if(method->returnTy().type) {
                  FiniteLengthReturn(cfg);
               }
            }
         }
         for(auto method : classDecl->constructors()) {
            if(method->body()) {
               auto cfg = cfgBuilder->build(method->body());
               // cfg->printDot(std::cout);
               ReachabilityCheck(cfg);
               LiveVariableAnalysis(cfg);
            }
         }
      }
   }
}

void DFA::FiniteLengthReturn(const CFGNode* node) const {
   if(node->hasBeenVisited()) {
      return;
   }

   node->setVisited(true);

   if(node->isReturnNode()) {
      return;
   }

   if(node->getChildren().begin() == node->getChildren().end()) {
      if(std::holds_alternative<CFGNode::EmptyExpr>(node->getData())) {
         diag.ReportError(SourceRange{}) << " Missing return statement here.";
      } else {
         diag.ReportError(node->location().value())
               << " Missing return statement here.";
      }

      return;
   }

   for(auto child : node->getChildren()) {
      FiniteLengthReturn(child);
   }
}

void DFA::ReachabilityCheck(const CFGNode* node) const {
   std::unordered_map<const CFGNode*, bool> out;
   std::pmr::set<const CFGNode*> worklist{alloc};
   getAllNodes(node, worklist);
   for(auto n : worklist) {
      out[n] = true;
   }
   std::pmr::set<const CFGNode*> next{alloc};
   ReachabilityCheckHelper(out, worklist, next);
   for(auto n : worklist) {
      bool reachable = n == node;
      for(auto parent : n->getParents()) {
         if(out[parent]) {
            reachable = true;
            break;
         }
      }
      if(!reachable) {
         if(n->location()) {
            diag.ReportError(n->location().value()) << "Unreachable statement";
         } else {
            diag.ReportError(node->location().value()) << "Unreachable statement";
         }
      }
   }
}

void DFA::LiveVariableAnalysis(const CFGNode* node) const {
   std::unordered_map<const CFGNode*, std::pmr::set<const ast::VarDecl*>> in;
   std::pmr::set<const CFGNode*> worklist{alloc};
   getAllNodes(node, worklist);
   for(auto n : worklist) {
      in[n] = std::pmr::set<const ast::VarDecl*>{alloc};
   }
   std::pmr::set<const CFGNode*> next{alloc};
   LiveVariableAnalysisHelper(in, worklist, next);
   for(auto n : worklist) {
      const ast::VarDecl* varDecl = nullptr;
      if(std::holds_alternative<const ast::Expr*>(n->getData())) {
         auto expr = std::get<const ast::Expr*>(n->getData());
         if(auto assignOp =
                  dyn_cast_or_null<const ast::exprnode::BinaryOp>(expr->tail())) {
            if(assignOp->opType() == ast::exprnode::BinaryOp::OpType::Assignment) {
               varDecl = assignOp->getVarAssigned();
            }
         }
      }
      if(varDecl) {
         bool isLive = false;
         for(auto child : n->getChildren()) {
            if(in[child].count(varDecl) > 0) {
               isLive = true;
               break;
            }
         }
         if(!isLive) {
            if(n->location()) {
               diag.ReportWarning(n->location().value())
                     << "Dead assignment: " << varDecl->name();
            } else {
               diag.ReportWarning(n->location().value())
                     << "Dead assignment: " << varDecl->name();
            }
         }
      }
   }
}

void DFA::LiveVariableAnalysisHelper(
      std::unordered_map<const CFGNode*, std::pmr::set<const ast::VarDecl*>>& in,
      std::pmr::set<const CFGNode*>& cur,
      std::pmr::set<const CFGNode*>& next) const {
   if(cur.empty()) return;
   for(auto n : cur) {
      std::pmr::set<const ast::VarDecl*> newIn{alloc};
      for(auto child : n->getChildren()) {
         for(auto v : in[child]) {
            newIn.insert(v);
         }
      }
      if(std::holds_alternative<const ast::Expr*>(n->getData())) {
         auto expr = std::get<const ast::Expr*>(n->getData());
         if(auto assignOp =
                  dyn_cast_or_null<const ast::exprnode::BinaryOp>(expr->tail())) {
            if(assignOp->opType() == ast::exprnode::BinaryOp::OpType::Assignment) {
               int times = getLiveVariables(expr, newIn, assignOp->getVarAssigned());
               if (times == 1) {
                  newIn.erase(assignOp->getVarAssigned());
               }
            } else {
               getLiveVariables(expr, newIn);
            }
         } else {
            getLiveVariables(expr, newIn);
         }
      } else if(std::holds_alternative<const ast::VarDecl*>(n->getData())) {
         auto varDecl = std::get<const ast::VarDecl*>(n->getData());
         int times = getLiveVariables(varDecl->init(), newIn, varDecl);
         if(times > 0) {
            diag.ReportError(varDecl->location()) << "Variable " << varDecl->name()
                                                  << " is used in its initializor";
         }
         newIn.erase(varDecl);
      }
      if(newIn != in[n]) {
         in[n] = newIn;
         for(auto parent : n->getParents()) {
            next.insert(parent);
         }
      }
   }
   std::pmr::set<const CFGNode*> next_next{alloc};
   LiveVariableAnalysisHelper(in, next, next_next);
}

int DFA::getLiveVariables(const ast::Expr* expr,
                          std::pmr::set<const ast::VarDecl*>& live_vars,
                          const ast::VarDecl* decl) const {
   int times = 0;
   for(auto node : expr->nodes()) {
      if(auto exprValue = dyn_cast_or_null<const ast::exprnode::ExprValue>(node)) {
         if(auto varDecl =
                  dyn_cast_or_null<const ast::VarDecl>(exprValue->decl())) {
            if(decl == varDecl) {
               times++;
            }
            live_vars.insert(varDecl);
         }
      }
   }
   return times;
}

void DFA::ReachabilityCheckHelper(std::unordered_map<const CFGNode*, bool>& out,
                                  std::pmr::set<const CFGNode*>& cur,
                                  std::pmr::set<const CFGNode*>& next) const {
   if(cur.empty()) return;
   for(auto n : cur) {
      bool newOut = false;
      if(n->isStart() && !n->isReturnNode()) {
         newOut = true;
      } else if(!n->isReturnNode()) {
         for(auto child : n->getParents()) {
            if(out[child]) { // if it is reachable from a parent, it is reachable
               newOut = true;
               break;
            }
         }
      }
      if(newOut != out[n]) {
         out[n] = newOut;
         for(auto child : n->getChildren()) {
            next.insert(child);
         }
      }
   }
   std::pmr::set<const CFGNode*> next_next{alloc};
   ReachabilityCheckHelper(out, next, next_next);
}

void DFA::getAllNodes(const CFGNode* node,
                      std::pmr::set<const CFGNode*>& list) const {
   if(list.count(node) > 0) {
      return;
   }
   list.insert(node);
   for(auto child : node->getChildren()) {
      getAllNodes(child, list);
   }
}

} // namespace semantic