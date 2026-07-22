#pragma once
#include "ast/DeclContext.h"
#include "semantic/CFGBuilder.h"
#include "utils/BumpAllocator.h"
#include <set>
#include <memory_resource>
namespace semantic {

class DataflowAnalysis {
   using Heap = std::pmr::memory_resource;

public:
   DataflowAnalysis(diagnostics::DiagnosticEngine& diag, BumpAllocator& alloc,
                    ast::Semantic& sema, ast::LinkingUnit* lu)
         : diag{diag}, alloc{alloc}, sema{sema}, lu{lu} {}
   void init(CFGBuilder* cfgBuilder) { this->cfgBuilder = cfgBuilder; }
   void Check() const;

private:
   diagnostics::DiagnosticEngine& diag;
   CFGBuilder* cfgBuilder;
   BumpAllocator& alloc;
   ast::Semantic& sema;
   ast::LinkingUnit* lu;

   void LiveVariableAnalysis(const CFGNode* node) const;
   void LiveVariableAnalysisHelper(
         std::unordered_map<const CFGNode*, std::pmr::set<const ast::VarDecl*>>&
               in,
         std::pmr::set<const CFGNode*>& cur,
         std::pmr::set<const CFGNode*>& next) const;
   int getLiveVariables(const ast::Expr* expr,
                         std::pmr::set<const ast::VarDecl*>& live_vars, const ast::VarDecl* decl = nullptr) const;
   void FiniteLengthReturn(const CFGNode* node) const;
   void ReachabilityCheck(const CFGNode* node) const;
   void ReachabilityCheckHelper(std::unordered_map<const CFGNode*, bool>& out,
                                std::pmr::set<const CFGNode*>& cur,
                                std::pmr::set<const CFGNode*>& next) const;
   void getAllNodes(const CFGNode* node,
                    std::pmr::set<const CFGNode*>& list) const;
};

} // namespace semantic
