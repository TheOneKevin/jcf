#include "semantic/NameResolver.h"

#include <cassert>
#include <iostream>
#include <string>
#include <string_view>
#include <unordered_map>
#include <variant>
#include <vector>

#include "ast/AST.h"
#include "ast/AstNode.h"
#include "ast/DeclContext.h"
#include "ast/Type.h"
#include "diagnostics/Location.h"

using std::string_view;
using std::pmr::string;
using std::pmr::unordered_map;
using std::pmr::vector;

namespace semantic {

static const std::pmr::string UNNAMED_PACKAGE{""};

void NameResolver::buildSymbolTable() {
   rootPkg_ = alloc.new_object<Pkg>(alloc);
   // Add the unnamed package to the root package.
   rootPkg_->children[UNNAMED_PACKAGE] = alloc.new_object<Pkg>(alloc);
   // Grab the package type from the compilation unit.
   for(auto cu : lu_->compliationUnits()) {
      // Grab the CU's package and mark it as immutable
      auto pkg = dynamic_cast<ast::UnresolvedType*>(cu->package());
      assert(pkg && "Package should be an unresolved type");
      pkg->lock();
      // Traverse the package name to find the leaf package.
      Pkg* subPkg = rootPkg_;
      for(auto const& id : pkg->parts()) {
         // If the subpackage name is not in the symbol table, add it
         // and continue to the next one.
         if(subPkg->children.find(id) == subPkg->children.end()) {
            auto newpkg = alloc.new_object<Pkg>(alloc, string_view{id});
            subPkg->children[id] = newpkg;
            subPkg = newpkg;
            continue;
         }
         // If the subpackage does not hold a package, then it must be a
         // a decl with the same name as the package. This is an error.
         // cf. JLS 6.4.1.
         Pkg::Child const& child = subPkg->children[id];
         if(std::holds_alternative<ast::Decl*>(child)) {
            auto decl = std::get<ast::Decl*>(child);
            assert(decl && "Package node holds empty decl");
            diag.ReportError(cu->location())
                  << "subpackage name cannot be the same as a declaration: " << id;
            continue;
         }
         // Otherwise, we can traverse into the next subpackage.
         subPkg = std::get<Pkg*>(child);
      }
      if(cu->isDefaultPackage()) {
         subPkg = std::get<Pkg*>(rootPkg_->children[UNNAMED_PACKAGE]);
      }
      // If the CU has no body, then we can skip to the next CU.
      if(!cu->body()) continue;
      // Check that the declaration is unique, cf. JLS 6.4.1.
      if(subPkg->children.find(cu->bodyAsDecl()->name().data()) !=
         subPkg->children.end()) {
         diag.ReportError(cu->bodyAsDecl()->location())
               << "declaration name is not unique in the subpackage.";
      }
      // Now add the CU's declaration to the subpackage.
      subPkg->children[cu->bodyAsDecl()->name().data()] = cu->bodyAsDecl();
   }
}

void NameResolver::BeginContext(ast::CompilationUnit* cu) {
   using ast::UnresolvedType, ast::Decl;

   // Set the current compilation unit and clear the imports map
   cu_ = cu;
   importsMap_.clear();
   auto curPkg = dynamic_cast<UnresolvedType*>(cu->package());
   assert(curPkg && "Package should be an unresolved type");

   // We can populate the imports map by order of shadowing cf. JLS 6.3.1.
   //   1. Package declarations, does not shadow anything.
   //   2. Import-on-demand declarations, never causes any declaration to be
   //      shadowed (even with other IOD), but maybe shadow other packages.
   //   3. All declarations in the same package (different CUs).
   //   4. Single-type-import declarations, shadows any other decl in all CUs
   //      under the same package. It also shadows any import-on-demand decls.
   //   4. All declarations in the current CU.
   // We should also note the scope of types under the same package declarations
   // cf. JLS 6.3 is visible across all CUs in the same package.

   // 1. Import-on-demand declarations. Populate this first so we can check
   //    if two import-on-demand declarations shadow each other.
   for(auto const& imp : cu->imports()) {
      if(!imp.isOnDemand) continue;
      // First, resolve the subpackage subtree from the symbol table.
      auto subPkg = resolveImport(static_cast<UnresolvedType const*>(imp.type));
      // No value means an error has been reported, skip this import.
      if(!subPkg) continue;
      if(!std::holds_alternative<Pkg*>(subPkg.value())) {
         diag.ReportError(imp.location())
               << "failed to resolve import-on-demand as subpackage is a "
                  "declaration: \""
               << imp.simpleName() << "\"";
         continue;
      }
      // Grab the package as a Pkg*.
      auto pkg = std::get<Pkg*>(subPkg.value());
      // Second, add all the Decl from the subpackage to the imports map.
      // We only add declarations, not subpackages. cf. JLS 7.5:
      //
      //    > A type-import-on-demand declaration (§7.5.2) imports all the
      //    > accessible types of a named type or package as needed.
      //
      for(auto& kv : pkg->children) {
         if(!std::holds_alternative<Decl*>(kv.second)) continue;
         auto decl = std::get<Decl*>(kv.second);
         if(importsMap_.find(kv.first) != importsMap_.end()) {
            auto imported = importsMap_[kv.first];
            if(std::holds_alternative<Decl*>(imported) &&
               std::get<Decl*>(imported) == decl)
               continue; // Same declaration, no error
            // FIXME(kevin): The import-on-demand shadows another
            // import-on-demand, so we mark the declaration as a null pointer.
            importsMap_[kv.first] = static_cast<Decl*>(nullptr);
            continue;
         }
         importsMap_[kv.first] = Pkg::Child{decl};
      }
   }
   // 2. Package declarations. We can ignore any duplicate names as they are
   //    shadowed by the import-on-demand declarations.
   for(auto& kv : rootPkg_->children) {
      if(!std::holds_alternative<Pkg*>(kv.second))
         continue; // We only care about subpackages
      if(importsMap_.find(kv.first) != importsMap_.end())
         continue; // This package is shadowed by an import-on-demand
      importsMap_[kv.first] = Pkg::Child{std::get<Pkg*>(kv.second)};
   }
   // 3. All declarations in the same package (different CUs). We can shadow
   //    any declarations already existing.
   {
      auto curTree = resolveImport(curPkg);
      assert(curTree && "Current package should exist!");
      assert(std::holds_alternative<Pkg*>(curTree.value()));
      for(auto& kv : std::get<Pkg*>(curTree.value())->children)
         if(std::holds_alternative<Decl*>(kv.second))
            importsMap_[kv.first] = Pkg::Child{std::get<Decl*>(kv.second)};
   }
   // 4. Single-type-import declarations. This may also shadow anything existing.
   for(auto const& imp : cu->imports()) {
      if(imp.isOnDemand) continue;
      // First, resolve the subpackage subtree from the symbol table.
      auto subPkg = resolveImport(static_cast<UnresolvedType const*>(imp.type));
      if(!subPkg) continue;
      if(!std::holds_alternative<Decl*>(subPkg.value())) {
         diag.ReportError(imp.location())
               << "failed to resolve single-type-import as a declaration: \""
               << imp.simpleName() << "\"";
         continue;
      }
      // Second, add the Decl from the subpackage to the imports map.
      auto decl = std::get<Decl*>(subPkg.value());
      auto cuDecl = cu->bodyAsDecl();
      // If the single-type-import name is the same as the class name, then it
      // shadows the class name. This is an error.
      if((decl->name() == cuDecl->name()) && (decl != cuDecl)) {
         diag.ReportError(cu->location()) << "single-type-import is the same "
                                             "as the class/interface name: "
                                          << decl->name();
         continue;
      }
      importsMap_[imp.simpleName()] = decl;
   }
   // 5. All declarations in the current CU. This may also shadow anything.
   if(cu->body())
      importsMap_[cu_->bodyAsDecl()->name().data()] = cu_->bodyAsDecl();
}

NameResolver::ChildOpt NameResolver::resolveImport(
      ast::UnresolvedType const* t) const {
   assert(t && "Type should not be null");
   assert(!t->isResolved() && "Type should not be resolved");
   if(t->parts().empty()) {
      return rootPkg_->children[UNNAMED_PACKAGE];
   }
   Pkg::Child subPkg = rootPkg_;
   for(auto const& id : t->parts()) {
      // If the subpackage is a declaration, then the import is invalid.
      if(std::holds_alternative<ast::Decl*>(subPkg)) {
         diag.ReportError(t->location())
               << "failed to resolve import as subpackage is a declaration: \""
               << id << "\"";
         return std::nullopt;
      }
      // Now that we know it's not a decl, get it as a package.
      auto pkg = std::get<Pkg*>(subPkg);
      // If the subpackage does not exist, then the import is invalid.
      if(pkg->children.find(id) == pkg->children.end()) {
         diag.ReportError(t->location())
               << "failed to resolve import as subpackage does not exist: \"" << id
               << "\"";
         return std::nullopt;
      }
      // Get the next subpackage.
      subPkg = pkg->children[id];
   }
   // At the end, we either have a decl or a subpackage.
   return subPkg;
}

void NameResolver::ResolveType(ast::UnresolvedType* ty) {
   // assert(type && "Type should not be null");
   if(!ty) return;
   assert(!ty->isResolved() && "Type should not be resolved");
   Pkg::Child subTy;
   auto it = ty->parts().begin();
   // Resolve the first level of the type against importMaps_
   if(auto it2 = importsMap_.find(*it); it2 != importsMap_.end()) {
      subTy = it2->second;
   } else {
      diag.ReportError(ty->location())
            << "failed to resolve type as subpackage does not exist: \"" << *it
            << "\"";
      return;
   }
   // Now resolve the remainder of the type against subTy
   it = std::next(it);
   for(; it != ty->parts().end(); it++) {
      // If the subpackage is a declaration, then the import is invalid.
      if(std::holds_alternative<ast::Decl*>(subTy)) {
         diag.ReportError(ty->location())
               << "failed to resolve type as subpackage is a declaration: \""
               << *it << "\"";
         return;
      }
      // Now that we know it's not a decl, get it as a package.
      auto pkg = std::get<Pkg*>(subTy);
      // If the subpackage does not exist, then the import is invalid.
      if(pkg->children.find(*it) == pkg->children.end()) {
         diag.ReportError(ty->location())
               << "failed to resolve type as subpackage does not exist: \"" << *it
               << "\"";
         return;
      }
      // Get the next subpackage.
      subTy = pkg->children[*it];
   }
   // The final type should be a declaration.
   if(!std::holds_alternative<ast::Decl*>(subTy)) {
      diag.ReportError(ty->location())
            << "failed to resolve type, is not a declaration: \"" << ty->toString()
            << "\"";
      return;
   }
   // Now we can create a reference type to the declaration.
   ty->resolveInternal(std::get<ast::Decl*>(subTy));
   // After, the type should be resolved
   assert(ty->isResolved() && "Type should be resolved");
}

void NameResolver::Resolve() {
   for(auto cu : lu_->compliationUnits()) {
      BeginContext(cu);
      if(!cu->body()) continue;
      if(auto ast = dynamic_cast<ast::ClassDecl*>(cu->body())) {
         resolveClass(ast);
      } else if(auto ast = dynamic_cast<ast::InterfaceDecl*>(cu->body())) {
         resolveInterface(ast);
      } else {
         assert(false && "Unimplemented");
      }
   }
}

void NameResolver::resolveMethod(ast::MethodDecl* decl) {
   for(auto local : decl->mut_locals()) local->mut_type()->resolve(*this);
   if(decl->mut_returnType()) decl->mut_returnType()->resolve(*this);
}

void NameResolver::resolveInterface(ast::InterfaceDecl* decl) {
   for(auto ty : decl->mut_extends()) ty->resolve(*this);
   for(auto method : decl->mut_methods()) resolveMethod(method);
}

void NameResolver::resolveClass(ast::ClassDecl* decl) {
   for(auto ty : decl->mut_interfaces()) ty->resolve(*this);
   for(auto super : decl->mut_superClasses())
      if(super) super->resolve(*this);
   for(auto field : decl->mut_fields()) field->mut_type()->resolve(*this);
   for(auto method : decl->mut_methods()) resolveMethod(method);
}

std::ostream& NameResolver::Pkg::print(std::ostream& os, int indent) const {
   for(auto const& [name, child] : children) {
      for(int i = 0; i < indent; i++) os << "  ";
      if(std::holds_alternative<ast::Decl*>(child)) {
         os << name << std::endl;
      } else {
         os << name << " ->" << std::endl;
         std::get<Pkg*>(child)->print(os, indent + 1);
      }
   }
   return os;
}

void NameResolver::Pkg::dump() const { print(std::cout, 0); }

} // namespace semantic
