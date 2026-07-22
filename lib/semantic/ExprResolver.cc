#include "semantic/ExprResolver.h"

#include <functional>
#include <string_view>
#include <utility>
#include <variant>
#include <memory_resource>

#include "ast/AST.h"
#include "ast/AstNode.h"
#include "ast/Decl.h"
#include "ast/DeclContext.h"
#include "ast/ExprNode.h"
#include "ast/Type.h"
#include "diagnostics/Location.h"
#include "semantic/HierarchyChecker.h"
#include "semantic/NameResolver.h"
#include "utils/Generator.h"
#include "utils/Utils.h"

namespace semantic {

namespace ex = ast::exprnode;
using ETy = internal::ExprResolverTy;
using PrevTy = internal::ExprNameWrapper::PrevTy;
using ER = ExprResolver;
using internal::ExprNameWrapper;
using Pkg = semantic::NameResolver::Pkg;

/* ===--------------------------------------------------------------------=== */
// Static helper functions
/* ===--------------------------------------------------------------------=== */

static ast::DeclContext const* GetTypeAsDecl(ast::Type const* type,
                                             NameResolver const& NR) {
   if(auto refty = dyn_cast<ast::ReferenceType>(type)) {
      return cast<ast::DeclContext>(refty->decl());
   } else if(type->isString()) {
      return NR.GetJavaLang().String;
   } else if(type->isArray()) {
      return NR.GetArrayPrototype();
   }
   return nullptr;
}

/* ===--------------------------------------------------------------------=== */
// Functions to resolve names and chains of names
/* ===--------------------------------------------------------------------=== */

const ast::Decl* ER::lookupDecl(ast::DeclContext const* ctx,
                                std::function<bool(ast::Decl const*)> cond) const {
   const ast::Decl* ret = nullptr;
   auto classDecl = dyn_cast<ast::ClassDecl>(ctx);
   if(classDecl && ctx != NR->GetArrayPrototype()) {
      for(auto decl : HC->getInheritedMembers(classDecl)) {
         if(cond(decl)) {
            if(ret) return nullptr; // Ambiguous, cannot resolve
            ret = decl;
         }
      }
      return ret;
   } else {
      // Search for the unique local variable
      for(auto decl : ctx->decls())
         if(cond(decl)) return decl;
      return nullptr;
   }
   // Context is probably CU
   return nullptr;
}

const ast::Decl* ER::lookupNamedDecl(ast::DeclContext const* ctx,
                                     std::string_view name) const {
   auto cond = [name, this](ast::Decl const* d) {
      auto td = dyn_cast<ast::TypedDecl>(d);
      if(!td) return false;
      bool scopeVisible = true;
      bool sameName = d->name() == name;
      bool sameContext = d->parent() == this->lctx_;
      // Ignore scoping rules for fields
      bool checkScope = dyn_cast<ast::VarDecl>(d) && this->lscope_;
      // Scoping is only meaningful inside methods
      if(sameContext && checkScope)
         scopeVisible = this->lscope_->canView(td->scope());
      // Check access modifiers for fields
      bool canAccess = true;
      if(auto fieldDecl = dyn_cast<ast::FieldDecl>(d)) {
         canAccess = isAccessible(fieldDecl->modifiers(), fieldDecl->parent());
      }
      return sameName && scopeVisible && canAccess;
   };
   return lookupDecl(ctx, cond);
}

bool ER::tryReclassifyDecl(ExprNameWrapper& data,
                           ast::DeclContext const* ctx) const {
   // Search in this context
   if(auto decl = lookupNamedDecl(ctx, data.node->name())) {
      if(auto varDecl = dynamic_cast<ast::VarDecl const*>(decl)) {
         data.reclassify(
               ExprNameWrapper::Type::ExpressionName, varDecl, varDecl->type());
         return true;
      } else if(auto fieldDecl = dynamic_cast<ast::FieldDecl const*>(decl)) {
         data.reclassify(ExprNameWrapper::Type::ExpressionName,
                         fieldDecl,
                         fieldDecl->type());
         return true;
      }
   }
   // If not found in this context, search in parent context
   // If the parent context does not exist, return the original data
   auto ctxAsDecl = dynamic_cast<ast::Decl const*>(ctx);
   if(!ctxAsDecl) return false;
   auto parentCtx = dynamic_cast<ast::DeclContext const*>(ctxAsDecl->parent());
   if(!parentCtx) return false;
   return tryReclassifyDecl(data, parentCtx);
}

bool ER::tryReclassifyImport(ExprNameWrapper& data,
                             NameResolver::ConstImportOpt import) const {
   if(!import.has_value()) {
      throw diag.ReportError(loc_) << "cannot resolve name: " << data.node->name();
   }
   if(std::holds_alternative<const ast::Decl*>(import.value())) {
      auto decl = std::get<const ast::Decl*>(import.value());
      // If declaration is null, then there is an import-on-demand conflict
      if(!decl) {
         throw diag.ReportError(loc_) << "ambiguous import-on-demand conflict";
      }
      data.reclassify(
            ExprNameWrapper::Type::TypeName, decl, Sema->BuildReferenceType(decl));
      return true;
   } else if(std::holds_alternative<const Pkg*>(import.value())) {
      auto pkg = std::get<const Pkg*>(import.value());
      assert(pkg && "expected non-null package here");
      data.reclassify(ExprNameWrapper::Type::PackageName, pkg);
      return true;
   }
   return false;
}

ExprNameWrapper* ER::reclassifySingleAmbiguousName(ExprNameWrapper* data) const {
   // JLS 6.5.2 Reclassification of Contextually Ambiguous Names

   assert(data->type() == ExprNameWrapper::Type::SingleAmbiguousName &&
          "Expected SingleAmbiguousName here");
   auto copy = alloc.new_object<ExprNameWrapper>(*data);

   /**
    * 1. If the Identifier appears within the scope (§6.3) of a local variable
    *    declaration (§14.4) or parameter declaration (§8.4.1, §8.8.1, §14.19)
    *    or field declaration (§8.3) with that name, then the AmbiguousName is
    *    reclassified as an ExpressionName.
    * 2. Otherwise, if a type of that name is declared in the compilation unit
    *    (§7.3) containing the Identifier, either by a single-type-import
    *    declaration (§7.5.1) or by a top level class (§8) or interface type
    *    declaration (§9), then the AmbiguousName is reclassified as a TypeName.
    * 3. Otherwise, if a type of that name is declared in another compilation unit
    *    (§7.3) of the package (§7.1) of the compilation unit containing the
    *    Identifier, then the AmbiguousName is reclassified as a TypeName.
    * 4. Otherwise, if a type of that name is declared by exactly one
    *    type-import-on-demand declaration (§7.5.2) of the compilation unit
    *    containing the Identifier, then the AmbiguousName is reclassified as a
    *    TypeName.
    * 5. Otherwise, if a type of that name is declared by more than one
    *    type-import-on-demand declaration of the compilation unit containing the
    *    Identifier, then a compile-time error results.
    * 6. Otherwise, the AmbiguousName is reclassified as a PackageName. A later
    *    step determines whether or not a package of that name actually exists.
    */

   // Criteria 1 and part of 2 (checks body of CU)
   if(tryReclassifyDecl(*copy, lctx_)) return copy;

   // Criteria 2 through 6 are handled by the NameResolver
   if(tryReclassifyImport(*copy, NR->GetImport(cu_, data->node->name())))
      return copy;

   // If all else fails, we probably hit criteria 6
   throw diag.ReportError(loc_)
         << "Unknown error when attempting to resolve import type";
}

void ER::resolveFieldAccess(ExprNameWrapper* access) const {
   // First, verify invariants if access is a field access
   access->verifyInvariants(ExprNameWrapper::Type::SingleAmbiguousName);
   if(auto p = access->prevIfWrapper())
      p->verifyInvariants(ExprNameWrapper::Type::ExpressionName);
   // Next, fetch the type or declaration
   auto name = access->node->name();
   auto typeOrDecl = access->prevAsDecl(*TR, *NR, loc_);
   ast::DeclContext const* refTy = nullptr;
   if(access->prevIfWrapper()) {
      // If the previous node is a wrapper, then we resolve the type
      auto decl = typeOrDecl;
      auto typeddecl = dyn_cast<ast::TypedDecl>(decl);
      if(!typeddecl) {
         throw diag.ReportError(loc_)
               << "field access \"" << name
               << "\" to non-typed declaration: " << decl->name();
      }
      auto type = typeddecl->type();
      if(!type) {
         throw diag.ReportError(loc_)
               << "field access \"" << name
               << "\" to void-typed declaration: " << decl->name();
      }
      refTy = GetTypeAsDecl(type, *NR);
      if(!refTy) {
         throw diag.ReportError(loc_)
               << "field access \"" << name << "\" to non-class type: "
               << (decl->hasCanonicalName() ? decl->getCanonicalName()
                                            : decl->name());
      }
   } else {
      refTy = typeOrDecl->asDeclContext();
      assert(refTy && "Expected non-null type here");
   }
   // Now we check if "name" is a field of "decl"
   auto field = lookupNamedDecl(refTy, name);
   if(!field) {
      throw diag.ReportError(loc_)
            << "cannot access field: " << name << access->node->location()
            << "inaccessible member";
   }
   // Field must be either a FieldDecl or a MethodDecl
   assert(dyn_cast<ast::FieldDecl>(field) || dyn_cast<ast::MethodDecl>(field));
   // Now we can reclassify the access node
   ast::Type const* fieldty = nullptr;
   if(auto x = dyn_cast<ast::FieldDecl>(field)) fieldty = x->type();
   access->reclassify(ExprNameWrapper::Type::ExpressionName, field, fieldty);
}

void ER::resolveTypeAccess(internal::ExprNameWrapper* access) const {
   // First, verify invariants if access is a type access
   access->verifyInvariants(ExprNameWrapper::Type::SingleAmbiguousName);
   if(auto p = access->prevIfWrapper())
      p->verifyInvariants(ExprNameWrapper::Type::TypeName);
   // Next, fetch the type or declaration
   auto name = access->node->name();
   auto typeOrDecl = access->prevAsDecl(*TR, *NR, loc_);
   // We note this must be a class type or we have a type error
   auto type = dynamic_cast<ast::ClassDecl const*>(typeOrDecl);
   if(!type) {
      throw diag.ReportError(loc_)
            << "static member access \"" << name
            << "\" to non-class type: " << typeOrDecl->name();
   }
   // Now we check if "name" is a field of "decl".
   auto field = lookupNamedDecl(type, name);
   if(!field) {
      throw diag.ReportError(loc_)
            << "static member access to undeclared field: " << name;
   }
   // With the additional constraint that the field must be static
   ast::Modifiers mods;
   if(auto fieldDecl = dyn_cast<ast::FieldDecl>(field)) {
      mods = fieldDecl->modifiers();
   } else if(auto methodDecl = dyn_cast<ast::MethodDecl>(field)) {
      mods = methodDecl->modifiers();
   } else {
      assert(false && "Field must be either a FieldDecl or a MethodDecl");
   }
   if(!mods.isStatic()) {
      throw diag.ReportError(loc_)
            << "attempted to access non-static member: "
            << (field->hasCanonicalName() ? field->getCanonicalName()
                                          : field->name());
   }
   // We've reached here which means the field access is valid
   ast::Type const* fieldty = nullptr;
   if(auto x = dyn_cast<ast::FieldDecl>(field)) fieldty = x->type();
   access->reclassify(ExprNameWrapper::Type::ExpressionName, field, fieldty);
   access->setPrev(std::nullopt);
}

void ER::resolvePackageAccess(internal::ExprNameWrapper* access) const {
   // First, verify invariants if access is a package access
   auto prev = access->prevAsWrapper();
   assert(prev && "Expected non-null previous node");
   access->verifyInvariants(ExprNameWrapper::Type::SingleAmbiguousName);
   prev->verifyInvariants(ExprNameWrapper::Type::PackageName);
   // Now we can get the "pkg" and "name" to resolve against
   auto pkg = std::get<Pkg const*>(prev->resolution());
   auto name = access->node->name();
   assert(pkg && "Expected non-null package here");
   // Now we check if "name" is a package of "pkg"
   auto subpkg = pkg->lookup(name, alloc);
   if(!subpkg.has_value()) {
      throw diag.ReportError(loc_)
            << "package access to undeclared member: " << name;
   }
   // Now we can reclassify the access node depending on what we found
   if(std::holds_alternative<ast::Decl const*>(subpkg.value())) {
      auto pkgdecl = std::get<ast::Decl const*>(subpkg.value());
      access->reclassify(ExprNameWrapper::Type::TypeName,
                         pkgdecl,
                         Sema->BuildReferenceType(pkgdecl));
   } else {
      access->reclassify(ExprNameWrapper::Type::PackageName,
                         std::get<Pkg const*>(subpkg.value()));
   }
   access->setPrev(std::nullopt);
}

/* ===--------------------------------------------------------------------=== */
// ExprNameWrapper methods and ResolveExprNode reduction functions
/* ===--------------------------------------------------------------------=== */

void ExprNameWrapper::dump() const {
   std::cerr << "\n**** ExprNameWrapper reverse list dump ****\n";
   dump(0);
}

void ExprNameWrapper::dump(int indent) const {
   // Print the indent
   for(int i = 0; i < indent; i++) std::cerr << "  ";
   // Print the current type and node
   std::cerr << "{ " << Type_to_string(type_, "??") << ", ";
   node->print(std::cerr) << " }\n";
   // If the previous is nullopt
   if(!prev_.has_value()) {
      for(int i = 0; i < indent; i++) std::cerr << "  ";
      std::cerr << "  - prev: nullopt\n";
   }
   // If the previous node is a wrapper, print it
   else if(auto p = prevIfWrapper()) {
      p->dump(indent + 1);
   }
   // If the previous node is a list, print it
   else {
      std::get<ast::ExprNodeList>(prev_.value()).print(std::cerr);
   }
}

void ExprNameWrapper::verifyInvariants(ExprNameWrapper::Type expectedTy) const {
   assert(type_ == expectedTy && "Expected type does not match actual type");
   // TODO(kevin): Add more invariants here
}

ast::Decl const* ExprNameWrapper::prevAsDecl(ExprTypeResolver& TR,
                                             NameResolver& NR,
                                             SourceRange loc) const {
   // Simple case, the previous node is wrapped so we know the decl
   if(auto p = prevIfWrapper()) return std::get<ast::Decl const*>(p->resolution());
   // Complex case, the previous node is an expression list
   ast::ExprNodeList prevList = std::get<ast::ExprNodeList>(prev_.value());
   auto type = TR.EvalList(prevList, loc);
   return cast<ast::Decl>(GetTypeAsDecl(type, NR));
}

ast::ExprNodeList ER::recursiveReduce(ExprNameWrapper* node) const {
   if(node->type() != ExprNameWrapper::Type::ExpressionName) {
      throw diag.ReportError(loc_) << "expected an expression name here, got: \""
                                   << node->type_string() << "\" instead";
   }
   node->verifyInvariants(ExprNameWrapper::Type::ExpressionName);
   auto decl = std::get<ast::Decl const*>(node->resolution());
   auto* expr = node->node;

   // Base case, no more nodes to reduce. Return singleton list.
   if(!node->prev().has_value() /* Either no more previous */ ||
      (node->prevIfWrapper() /* Or the previous node is irreducible */ &&
       node->prevAsWrapper()->type() != ExprNameWrapper::Type::ExpressionName)) {
      expr->resolveDeclAndType(decl, node->typeResolution());
      return ast::ExprNodeList{expr};
   }

   // Otherwise, chain the previous node to the current node
   ast::ExprNodeList list{};
   if(auto prev = node->prevIfWrapper()) {
      list = recursiveReduce(prev);
   } else {
      list = std::get<ast::ExprNodeList>(node->prev().value());
   }
   expr->resolveDeclAndType(decl, node->typeResolution());
   list.push_back(expr);
   assert(node->op && "Expected non-null operator here");
   list.push_back(node->op);
   return list;
}

ast::ExprNodeList ER::resolveExprNode(const ETy node) const {
   ExprNameWrapper* Q = nullptr;
   // 1. If node is an ExprNode, then resolution is only needed for names
   // 2. If the node is a list, then no resolution is needed
   // 3. Otherwise, get the wrapped node and build resolution
   if(std::holds_alternative<ast::ExprNode*>(node)) {
      auto expr = std::get<ast::ExprNode*>(node);
      auto thisNode = dyn_cast<ex::ThisNode*>(expr);
      auto name = dynamic_cast<ex::MemberName*>(expr);
      if(thisNode) {
         thisNode->resolveDeclAndType(
               cast<ast::Decl>(cu_->body()),
               Sema->BuildReferenceType(cast<ast::Decl>(cu_->body())));
      }
      if(!name) return ast::ExprNodeList{expr};
      Q = resolveSingleName(name);
   } else if(std::holds_alternative<ast::ExprNodeList>(node)) {
      return std::get<ast::ExprNodeList>(node);
   } else {
      Q = std::get<ExprNameWrapper*>(node);
   }
   // Now we can reduce the expression to a list of nodes
   return recursiveReduce(Q);
}

bool ER::isMethodMoreSpecific(ast::MethodDecl const* a,
                              ast::MethodDecl const* b) const {
   // Let a be declared in T with parameter types Ti
   // Let b be declared in U with parameter types Ui
   // Then a > b when for all i, Ti > Ui and T > U
   // Where two types A > B when A converts to B
   auto T = alloc.new_object<ast::ReferenceType>(cast<ast::Decl>(a->parent()),
                                                 SourceRange{});
   auto U = alloc.new_object<ast::ReferenceType>(cast<ast::Decl>(b->parent()),
                                                 SourceRange{});
   if(!TR->isAssignableTo(T, U)) return false;
   assert(a->parameters().size() == b->parameters().size());
   for(size_t i = 0; i < a->parameters().size(); i++) {
      auto Ti = a->parameters()[i]->type();
      auto Ui = b->parameters()[i]->type();
      if(!TR->isAssignableTo(Ti, Ui)) return false;
   }
   return true;
}

bool ER::areParameterTypesApplicable(ast::MethodDecl const* decl,
                                     const ty_array& argtys) const {
   bool valid = true;
   for(size_t i = 0; i < argtys.size(); i++) {
      auto ty1 = argtys[i];
      auto ty2 = decl->parameters()[i]->type();
      valid &= TR->isAssignableTo(ty1, ty2);
   }
   return valid;
}

utils::Generator<ast::MethodDecl const*> ER::getInheritedMethods(
      ast::DeclContext const* ctx) const {
   bool isArrayType = ctx == NR->GetArrayPrototype();
   if(isArrayType) {
      for(auto method : ctx->decls())
         if(auto decl = dyn_cast<ast::MethodDecl>(method)) co_yield decl;
   } else {
      for(auto method : HC->getInheritedMethods(cast<ast::Decl>(ctx)))
         co_yield method;
   }
   // JLS 9.2: Interfaces should also inherit java.lang.Object methods
   if(dyn_cast<ast::InterfaceDecl>(ctx)) {
      for(auto method : NR->GetJavaLang().Object->decls())
         if(auto decl = dyn_cast<ast::MethodDecl>(method)) co_yield decl;
   }
}

ast::MethodDecl const* ER::resolveMethodOverload(ast::DeclContext const* ctx,
                                                 std::string_view name,
                                                 const ty_array& argtys,
                                                 bool isCtor) const {
   // Set the name to the constructor name if isCtor is true
   if(isCtor) name = ctx->asDecl()->name();
   // 15.12.2.1 Find Methods that are Applicable and Accessible
   std::pmr::vector<ast::MethodDecl const*> candidates{alloc};
   if(isCtor) {
      auto cu = cast<ast::CompilationUnit>(cast<ast::Decl>(ctx)->parent());
      // Only grab the constructors of this type
      for(auto decl : ctx->decls()) {
         auto ctor = dynamic_cast<ast::MethodDecl const*>(decl);
         if(!ctor) continue;
         if(!ctor->isConstructor()) continue;
         if(ctor->parameters().size() != argtys.size()) continue;
         if(!isAccessible(ctor->modifiers(), ctor->parent())) continue;
         // If the ctor is private, it must be in the same PACKAGE
         if(ctor->modifiers().isProtected()) {
            if(cu->getPackageName() != cu_->getPackageName()) {
               throw diag.ReportError(loc_)
                     << "attempted to access protected constructor from different "
                        "package";
            }
         }
         if(areParameterTypesApplicable(ctor, argtys)) candidates.push_back(ctor);
      }
   } else {
      // Search the current class and all superclasses
      // 1. Parameters number match
      // 2. Parameters are convertible
      // 3. Method is accessible
      for(auto decl : getInheritedMethods(ctx)) {
         if(!decl) continue;
         if(decl->parameters().size() != argtys.size()) continue;
         if(decl->name() != name) continue;
         if(!areParameterTypesApplicable(decl, argtys)) continue;
         if(!isAccessible(decl->modifiers(), decl->parent())) continue;
         candidates.push_back(decl);
      }
   }
   if(candidates.size() == 0) {
      if(isCtor) {
         throw diag.ReportError(loc_) << "no method found for " << name;
      } else {
         throw diag.ReportError(loc_) << "no constructor found for " << name;
      }
   }
   if(candidates.size() == 1) return candidates[0];

   // 15.12.2.2 Choose the Most Specific Method
   ast::MethodDecl const* mostSpecific = nullptr;
   std::pmr::vector<ast::MethodDecl const*> maxSpecific{alloc};
   // Grab the (not necessarily unique) minimum
   for(auto cur : candidates) {
      if(!mostSpecific) {
         mostSpecific = cur;
         continue;
      }
      // cur < minimum?
      if(isMethodMoreSpecific(cur, mostSpecific)) {
         mostSpecific = cur;
      }
   }
   // Now grab all the maximally specific methods
   for(auto cur : candidates) {
      if(cur == mostSpecific) {
         maxSpecific.push_back(cur);
      } else if(isMethodMoreSpecific(cur, mostSpecific) &&
                isMethodMoreSpecific(mostSpecific, cur)) {
         maxSpecific.push_back(cur);
      }
   }
   // If there's only one maximally specific method, return it
   if(maxSpecific.size() == 1) return maxSpecific[0];
   // FIXME(kevin): There are more conditions i.e., abstract...
   // Otherwise, we have an ambiguity error
   for(auto cur : maxSpecific) cur->printSignature(std::cerr) << "\n";
   throw diag.ReportError(loc_) << "ambiguous method found for name: " << name;
}

/* ===--------------------------------------------------------------------=== */
// Begin the evaluation of the expression nodes
/* ===--------------------------------------------------------------------=== */

ETy ER::mapValue(ExprValue& node) const { return &node; }

ETy ER::evalMemberAccess(DotOp& op, const ETy lhs, const ETy id) const {
   // Resolves expr of form: Q . Id
   //    Where Q is either a simple identifier or a qualified
   //    identifier. If the LHS is an ExprNode, then it is simple and we need to
   //    reclassify it. Otherwise, we can continue to resolve recursively.
   PrevTy Q = nullptr;
   if(std::holds_alternative<ast::ExprNode*>(lhs)) {
      auto node = std::get<ast::ExprNode*>(lhs);
      if(auto simpleName = dyn_cast<ex::MemberName>(node)) {
         Q = resolveSingleName(simpleName);
      } else if(auto thisNode = dyn_cast<ex::ThisNode>(node)) {
         Q = resolveExprNode(thisNode);
      } else if(auto lit = dyn_cast<ex::LiteralNode>(node)) {
         if(!lit->builtinType()->isString()) {
            throw diag.ReportError(loc_)
                  << "attempted to access field on non-string literal"
                  << node->location() << "is a non-string literal";
         }
         Q = ast::ExprNodeList{lit};
      } else {
         assert(false && "Malformed node. Expected MemberName here.");
      }
   } else if(std::holds_alternative<ExprNameWrapper*>(lhs)) {
      Q = std::get<ExprNameWrapper*>(lhs);
   } else {
      Q = std::get<ast::ExprNodeList>(lhs);
   }
   // The LHS must be an ExpressionName, TypeName or PackageName
   auto* P = std::get_if<ExprNameWrapper*>(&Q);
   if(P) {
      assert(((*P)->type() == ExprNameWrapper::Type::ExpressionName ||
              (*P)->type() == ExprNameWrapper::Type::TypeName ||
              (*P)->type() == ExprNameWrapper::Type::PackageName) &&
             "Malformed node. Expected ExpressionName, TypeName or PackageName "
             "here.");
   }
   // Grab the Id as an expr node
   auto exprNode = std::get<ast::ExprNode*>(id);
   // Special case: If "Id" in Q . Id is a method name, then defer resolution
   if(auto methodNode = dyn_cast<ex::MethodName*>(exprNode)) {
      auto newQ = alloc.new_object<ExprNameWrapper>(
            ExprNameWrapper::Type::MethodName, methodNode, &op);
      newQ->setPrev(Q);
      return newQ;
   }
   // Now grab the id and cast it to the appropriate type
   auto fieldNode = dyn_cast<ex::MemberName*>(exprNode);
   assert(fieldNode && "Malformed node. Expected MemberName here.");
   // Allocate a new node as the member access to represent "Id" in Lhs . Id
   auto newQ = alloc.new_object<ExprNameWrapper>(
         ExprNameWrapper::Type::SingleAmbiguousName, fieldNode, &op);
   newQ->setPrev(Q);
   // And we can build the reduced expression now
   // 1. If the previous node is a wrapper, then newQ can be anything
   // 2. If the previous is a list, then newQ must be ExpressionName
   //    FIXME: Is this true? What about (Class).Field?
   if(P) {
      switch((*P)->type()) {
         case ExprNameWrapper::Type::ExpressionName:
            resolveFieldAccess(newQ);
            break;
         case ExprNameWrapper::Type::TypeName:
            resolveTypeAccess(newQ);
            break;
         case ExprNameWrapper::Type::PackageName:
            resolvePackageAccess(newQ);
            break;
         default:
            std::unreachable();
      }
   } else {
      resolveFieldAccess(newQ);
   }
   return newQ;
}

ETy ER::evalBinaryOp(BinaryOp& op, const ETy lhs, const ETy rhs) const {
   ast::ExprNodeList list{};
   list.concat(resolveExprNode(lhs));
   list.concat(resolveExprNode(rhs));
   list.push_back(&op);
   return list;
}

ETy ER::evalUnaryOp(UnaryOp& op, const ETy rhs) const {
   ast::ExprNodeList list{};
   list.concat(resolveExprNode(rhs));
   list.push_back(&op);
   return list;
}

ast::DeclContext const* ER::getMethodParent(ExprNameWrapper* Q) const {
   Q->verifyInvariants(ExprNameWrapper::Type::MethodName);
   // If there's no previous, use the current context
   if(!Q->prev().has_value()) return cu_->body();
   auto declOrType = Q->prevAsDecl(*TR, *NR, loc_);
   auto ty = dynamic_cast<ast::DeclContext const*>(declOrType);
   if(Q->prevIfWrapper() && !ty) {
      auto typedDecl = dynamic_cast<ast::TypedDecl const*>(declOrType);
      if(!typedDecl) {
         throw diag.ReportError(loc_)
               << "method call to non-typed declaration: " << declOrType->name();
      }
      auto type = typedDecl->type();
      if(!type) {
         throw diag.ReportError(loc_)
               << "method call to void-typed declaration: " << declOrType->name();
      }
      ty = GetTypeAsDecl(type, *NR);
      if(!ty) {
         throw diag.ReportError(loc_)
               << "method call to non-reference type: " << declOrType->name();
      }
   }
   assert(ty && "Expected non-null type here");
   return ty;
}

bool ER::isAccessible(ast::Modifiers mod, ast::DeclContext const* parent) const {
   // 6.6.1 Determining Accessibility
   if(mod.isPublic()) return true;
   // 6.6.2 Details on protected Access
   // If current is a child of parent, then it is accessible
   auto* targetClass = cast<ast::ClassDecl>(parent);
   if(auto* curClass = dyn_cast<ast::ClassDecl>(cu_->bodyAsDecl())) {
      if(HC->isSuperClass(targetClass, curClass)) return true;
   }
   // If they are in the same package, access is also allowed
   if(auto other_cu = dyn_cast<ast::CompilationUnit>(targetClass->parent())) {
      if(cu_->getPackageName() == other_cu->getPackageName()) return true;
   }
   return false;
}

ETy ER::evalMethodCall(MethodOp& op, const ETy method,
                       const op_array& args) const {
   // Q is the incompletely resolved method name
   ExprNameWrapper* Q = nullptr;
   // Single name method can never be static, and static methods can
   // never be single name methods!
   bool isSingleNameMethod = false;

   // 1. It could be a raw ExprNode, in which case is something like Fun()
   // 2. Or it could be a wrapped ExprNameWrapper, in which case is something
   //    like org.pkg.Class.Fun()
   // 3. Or it could be a list of nodes, in which case is something like
   //    (1 + 2 + 3)()
   if(std::holds_alternative<ast::ExprNode*>(method)) {
      auto expr = std::get<ast::ExprNode*>(method);
      auto name = dynamic_cast<ex::MethodName*>(expr);
      assert(name && "Malformed node. Expected MethodName here.");
      Q = resolveSingleName(name);
      isSingleNameMethod = true;
   } else if(std::holds_alternative<ExprNameWrapper*>(method)) {
      Q = std::get<ExprNameWrapper*>(method);
   } else {
      throw diag.ReportError(loc_) << "malformed method call expression";
   }

   // Resolve the array of arguments
   ty_array argtys{alloc};
   ast::ExprNodeList arglist{};
   for(auto& arg : args | std::views::reverse) {
      auto tmplist = resolveExprNode(arg);
      argtys.push_back(TR->EvalList(tmplist, loc_));
      arglist.concat(tmplist);
   }

   // Begin resolution of the method call
   auto ctx = getMethodParent(Q);
   auto methodDecl = resolveMethodOverload(ctx, Q->node->name(), argtys, false);

   // Check if the method call is legal
   if(!isAccessible(methodDecl->modifiers(), methodDecl->parent())) {
      throw diag.ReportError(loc_)
            << "method call to non-accessible method: " << methodDecl->name();
   }

   // Check static method call
   if(isSingleNameMethod && methodDecl->modifiers().isStatic()) {
      throw diag.ReportError(loc_)
            << "attempted to call static method using single name: "
            << methodDecl->name();
   }

   // Is the previous a type name?
   if(Q->prev().has_value() && Q->prevIfWrapper() &&
      Q->prevAsWrapper()->type() == ExprNameWrapper::Type::TypeName) {
      // If so, then we are calling a static method, so we should check
      if(!methodDecl->modifiers().isStatic()) {
         throw diag.ReportError(loc_)
               << "attempted to call non-static method: " << methodDecl->name();
      }
   }

   // Reclassify the Q node to be an ExpressionName
   Q->reclassify(ExprNameWrapper::Type::ExpressionName, methodDecl, nullptr);

   // Once Q has been resolved, we can build the expression list
   ast::ExprNodeList list{};
   list.concat(recursiveReduce(Q));
   list.concat(arglist);
   list.push_back(&op);
   return list;
}

ETy ER::evalNewObject(NewOp& op, const ETy object, const op_array& args) const {
   // Get the type we are instantiating
   ex::TypeNode* expr = nullptr;
   if(std::holds_alternative<ast::ExprNode*>(object)) {
      expr = cast<ex::TypeNode>(std::get<ast::ExprNode*>(object));
   } else {
      assert(false && "Grammar wrong, object must be an atomic ExprNode*");
   }

   // Resolve the array of arguments
   ty_array argtys{alloc};
   ast::ExprNodeList arglist{};
   for(auto& arg : args | std::views::reverse) {
      auto tmplist = resolveExprNode(arg);
      argtys.push_back(TR->EvalList(tmplist, loc_));
      arglist.concat(tmplist);
   }

   // Check if the type is abstract
   auto ctx = cast<ast::DeclContext>(expr->type()->getAsDecl());
   if(auto classDecl = dyn_cast<ast::ClassDecl>(ctx)) {
      if(classDecl->modifiers().isAbstract()) {
         throw diag.ReportError(loc_)
               << "attempted to instantiate abstract class: "
               << (classDecl->hasCanonicalName() ? classDecl->getCanonicalName()
                                                 : classDecl->name());
      }
   }

   // Begin resolution of the method call
   auto methodDecl = resolveMethodOverload(ctx, "", argtys, true);
   expr->overrideDecl(methodDecl);

   // Once op has been resolved, we can build the expression list
   ast::ExprNodeList list{};
   list.push_back(expr);
   list.concat(arglist);
   list.push_back(&op);
   return list;
}

ETy ER::evalNewArray(NewArrayOp& op, const ETy type, const ETy size) const {
   ast::ExprNodeList list{};
   if(std::holds_alternative<ast::ExprNode*>(type)) {
      auto expr = cast<ex::TypeNode>(std::get<ast::ExprNode*>(type));
      list.push_back(expr);
   } else {
      assert(false && "Grammar wrong, type must be an atomic ExprNode*");
   }
   list.concat(resolveExprNode(size));
   list.push_back(&op);
   return list;
}

ETy ER::evalArrayAccess(ArrayAccessOp& op, const ETy array,
                        const ETy index) const {
   ast::ExprNodeList list{};
   list.concat(resolveExprNode(array));
   list.concat(resolveExprNode(index));
   list.push_back(&op);
   return list;
}

ETy ER::evalCast(CastOp& op, const ETy type, const ETy value) const {
   ast::ExprNodeList list{};
   if(std::holds_alternative<ast::ExprNode*>(type)) {
      auto expr = cast<ex::TypeNode>(std::get<ast::ExprNode*>(type));
      list.push_back(expr);
   } else {
      assert(false && "Grammar wrong, type must be an atomic ExprNode*");
   }
   list.concat(resolveExprNode(value));
   list.push_back(&op);
   return list;
}

bool ER::validate(ETy const& value) const {
   if(auto wrapper = std::get_if<ExprNameWrapper*>(&value)) {
      // TODO(kevin)
   } else if(auto list = std::get_if<ast::ExprNodeList>(&value)) {
      if(list->size() == 0) return false;
      if(list->size() == 1) return true;
      return dyn_cast<ex::ExprOp>(list->tail()) != nullptr;
   } else if(auto expr = std::get_if<ast::ExprNode*>(&value)) {
      // TODO(kevin)
   }
   return true;
}

} // namespace semantic
