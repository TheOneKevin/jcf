#include "semantic/Semantic.h"

#include <set>
#include <string>
#include <memory_resource>

#include "ast/AstNode.h"
#include "diagnostics/Location.h"
#include "parsetree/ParseTree.h"

namespace ast {

using std::string;

Semantic::Semantic(BumpAllocator& alloc, diagnostics::DiagnosticEngine& diag)
      : alloc{alloc}, diag{diag} {
   // Preallocate java.lang.Object type
   {
      auto ty = BuildUnresolvedType(SourceRange{});
      ty->addIdentifier("java");
      ty->addIdentifier("lang");
      ty->addIdentifier("Object");
      ty->lock();
      objectType_ = ty;
   }
}

/* ===--------------------------------------------------------------------=== */
// ast/Type.h
/* ===--------------------------------------------------------------------=== */

UnresolvedType* Semantic::BuildUnresolvedType(SourceRange loc) {
   return alloc.new_object<UnresolvedType>(alloc, loc);
}

ArrayType* Semantic::BuildArrayType(Type* elementType, SourceRange loc) {
   return alloc.new_object<ArrayType>(alloc, elementType, loc);
}

BuiltInType* Semantic::BuildBuiltInType(parsetree::BasicType::Type type,
                                        SourceRange loc) {
   return alloc.new_object<BuiltInType>(type, loc);
}

BuiltInType* Semantic::BuildBuiltInType(parsetree::Literal::Type type) {
   return alloc.new_object<BuiltInType>(type);
}

BuiltInType* Semantic::BuildBuiltInType(ast::BuiltInType::Kind type) {
   return alloc.new_object<BuiltInType>(type, SourceRange{});
}

ReferenceType* Semantic::BuildReferenceType(Decl const* decl) {
   return alloc.new_object<ReferenceType>(decl, SourceRange{});
}

/* ===--------------------------------------------------------------------=== */
// ast/Decl.h
/* ===--------------------------------------------------------------------=== */

VarDecl* Semantic::BuildVarDecl(Type* type, SourceRange loc, string_view name,
                                ScopeID const* scope, Expr* init, bool isArg) {
   auto decl = alloc.new_object<VarDecl>(alloc, loc, type, name, init, scope, isArg);
   if(!AddLexicalLocal(decl)) {
      diag.ReportError(loc)
            << "local variable \"" << name << "\" already declared in this scope"
            << loc << "local declared here"
            << lexicalLocalScope[std::string{name.data()}]->location()
            << "previous declaration here";
   }
   return decl;
}

FieldDecl* Semantic::BuildFieldDecl(Modifiers modifiers, SourceRange loc,
                                    Type* type, string_view name, Expr* init,
                                    bool allowFinal) {
   if(!allowFinal && modifiers.isFinal()) {
      diag.ReportError(modifiers.getLocation(Modifiers::Type::Final))
            << "field declaration cannot be final.";
   }
   if(modifiers.isAbstract()) {
      diag.ReportError(modifiers.getLocation(Modifiers::Type::Abstract))
            << "field declaration cannot be abstract.";
   }
   if(modifiers.isNative()) {
      diag.ReportError(modifiers.getLocation(Modifiers::Type::Native))
            << "field declaration cannot be native.";
   }
   if(modifiers.isPublic() && modifiers.isProtected()) {
      diag.ReportError(modifiers.getLocation(Modifiers::Type::Public))
            << "field cannot be both public and protected.";
   }
   if(!modifiers.isPublic() && !modifiers.isProtected()) {
      diag.ReportError(loc) << "field must have a visibility modifier.";
   }
   return alloc.new_object<FieldDecl>(
         alloc, loc, modifiers, type, name, init, NextFieldScopeID());
}

/* ===--------------------------------------------------------------------=== */
// ast/DeclContext.h
/* ===--------------------------------------------------------------------=== */

LinkingUnit* Semantic::BuildLinkingUnit(
      array_ref<CompilationUnit*> compilationUnits) {
   std::pmr::set<std::string_view> names;

   for(auto cu : compilationUnits) {
      if(!cu->body()) continue;
      std::string_view name = cu->bodyAsDecl()->getCanonicalName();

      if(names.count(name) > 0) {
         diag.ReportError(cu->location())
               << "No two classes or interfaces can have the same canonical name";
      }

      names.insert(name);
   }

   // Build the java.lang package
   auto javaLangPackage = BuildUnresolvedType(SourceRange{});
   javaLangPackage->addIdentifier("java");
   javaLangPackage->addIdentifier("lang");
   std::pmr::vector<ImportDeclaration> imports;
   compilationUnits.push_back(
         BuildCompilationUnit(javaLangPackage, imports, SourceRange(), nullptr));

   return alloc.new_object<LinkingUnit>(alloc, compilationUnits);
}

CompilationUnit* Semantic::BuildCompilationUnit(
      ReferenceType* package, array_ref<ImportDeclaration> imports,
      SourceRange loc, DeclContext* body) {
   std::pmr::set<std::pmr::string> names;
   std::pmr::set<std::pmr::string> fullImportNames;
   for(auto import : imports) {
      if(import.isOnDemand) continue;
      std::pmr::string name(import.simpleName());
      std::pmr::string fullName(import.type->toString());
      // Check that no two single-type-import declarations clash with each other
      // We allow duplicate if they refer to the same type
      if(names.count(name) > 0 && fullImportNames.count(fullName) == 0) {
         diag.ReportError(loc)
               << "No two single-type-import declarations clash with each other.";
      }
      names.insert(name);
      fullImportNames.insert(fullName);
   }

   // cf. JLS 7.5.2, we must import java.lang.*
   auto javaLang = BuildUnresolvedType(SourceRange{});
   javaLang->addIdentifier("java");
   javaLang->addIdentifier("lang");
   ImportDeclaration javaLangImport{javaLang, true};
   imports.push_back(javaLangImport);

   // Create the AST node
   return alloc.new_object<CompilationUnit>(alloc, package, imports, loc, body);
}

ClassDecl* Semantic::BuildClassDecl(Modifiers modifiers, SourceRange loc,
                                    string_view name, ReferenceType* superClass,
                                    array_ref<ReferenceType*> interfaces,
                                    array_ref<Decl*> classBodyDecls) {
   // Check that the modifiers are valid for a class
   if(modifiers.isAbstract() && modifiers.isFinal())
      diag.ReportError(loc) << "class cannot be both abstract and final";
   if(!modifiers.isPublic())
      diag.ReportError(loc) << "class must have a visibility modifier";
   // Create the AST node
   auto node = alloc.new_object<ClassDecl>(alloc,
                                           modifiers,
                                           loc,
                                           name,
                                           superClass,
                                           objectType_,
                                           interfaces,
                                           classBodyDecls);
   // Check if the class has at least one constructor
   if(node->constructors().size() == 0)
      diag.ReportError(loc) << "class must have at least one constructor";
   // Check if the construct name is the same as the class name
   for(auto constructor : node->constructors()) {
      if(loc.isValid() /* is decl synthetic? */ && constructor->name() != name) {
         diag.ReportError(constructor->location())
               << "constructor name must be the same as the class name"
               << constructor->location() << "constructor here" << loc
               << "must match class declared here";
      }
   }
   // Check if multiple fields have the same name
   for(auto field : node->fields()) {
      for(auto other : node->fields()) {
         if(field != other && field->name() == other->name()) {
            diag.ReportError(field->location())
                  << "field \"" << field->name() << "\" is already declared"
                  << field->location() << "field here" << other->location()
                  << "previous declaration here";
         }
      }
   }
   // Check that interfaces cannot be repeated in implements clause
   for(auto interface : node->interfaces()) {
      for(auto other : node->interfaces()) {
         if(interface != other && interface->toString() == other->toString()) {
            diag.ReportError(loc)
                  << "interface \"" << interface->toString()
                  << "\" is already implemented" << interface->location()
                  << "interface here" << other->location()
                  << "previous implementation here";
         }
      }
   }
   return node;
}

InterfaceDecl* Semantic::BuildInterfaceDecl(Modifiers modifiers, SourceRange loc,
                                            string_view name,
                                            array_ref<ReferenceType*> extends,
                                            array_ref<Decl*> interfaceBodyDecls) {
   // Check that the modifiers are valid for an interface
   if(modifiers.isFinal())
      diag.ReportError(modifiers.getLocation(Modifiers::Type::Final))
            << "interface cannot be final";
   if(!modifiers.isPublic())
      diag.ReportError(loc) << "interface must have a visibility modifier";
   // Verify the methods are abstract
   for(auto decl : interfaceBodyDecls) {
      if(auto methodDecl = dyn_cast<MethodDecl*>(decl)) {
         if(!methodDecl->modifiers().isAbstract()) {
            diag.ReportError(methodDecl->location())
                  << "interface method must be abstract";
         }
      }
   }
   // Check that interfaces cannot be repeated in extends clause
   for(auto interface : extends) {
      for(auto other : extends) {
         if(interface != other && interface->toString() == other->toString()) {
            diag.ReportError(loc)
                  << "interface \"" << interface->toString()
                  << "\" is already implemented" << interface->location()
                  << "interface here" << other->location()
                  << "previous implementation here";
         }
      }
   }
   // Create the AST node
   return alloc.new_object<InterfaceDecl>(
         alloc, modifiers, loc, name, extends, objectType_, interfaceBodyDecls);
}

MethodDecl* Semantic::BuildMethodDecl(Modifiers modifiers, SourceRange loc,
                                      string_view name, Type* returnType,
                                      array_ref<VarDecl*> parameters,
                                      bool isConstructor, Stmt* body) {
   // Check modifiers
   if((body == nullptr) != (modifiers.isAbstract() || modifiers.isNative())) {
      diag.ReportError(loc) << "method has a body if and only if it is "
                               "neither abstract nor native.";
   }
   if(modifiers.isAbstract() &&
      (modifiers.isFinal() || modifiers.isStatic() || modifiers.isNative())) {
      diag.ReportError(loc) << "an abstract method cannot be static, final, "
                               "or native.";
   }
   if(modifiers.isStatic() && modifiers.isFinal()) {
      diag.ReportError(modifiers.getLocation(Modifiers::Type::Final))
            << "static method cannot be final.";
   }
   if(modifiers.isNative()) {
      if(!modifiers.isStatic()) {
         diag.ReportError(loc) << "native method must be static.";
      }
      if(auto ty = dyn_cast<BuiltInType*>(returnType)) {
         if(ty->getKind() != BuiltInType::Kind::Int) {
            diag.ReportError(loc) << "native method must have return type int.";
         }
      }
      if(parameters.size() != 1) {
         diag.ReportError(loc) << "native method must have exactly one "
                                  "parameter of type int.";
      } else if(auto ty = dyn_cast<BuiltInType*>(parameters[0]->type())) {
         if(ty->getKind() != BuiltInType::Kind::Int) {
            diag.ReportError(loc) << "native method must have exactly one "
                                     "parameter of type int.";
         }
      }
   }
   if(modifiers.isPublic() && modifiers.isProtected()) {
      diag.ReportError(modifiers.getLocation(Modifiers::Type::Public))
            << "method cannot be both public and protected.";
   }
   if(!modifiers.isPublic() && !modifiers.isProtected()) {
      diag.ReportError(loc) << "method must have a visibility modifier.";
   }
   // Create the AST node
   return alloc.new_object<MethodDecl>(
         alloc, modifiers, loc, name, returnType, parameters, isConstructor, body);
}

/* ===-----------------------------------------------------------------=== */
// ast/Stmt.h
/* ===-----------------------------------------------------------------=== */

BlockStatement* Semantic::BuildBlockStatement(array_ref<Stmt*> stmts) {
   return alloc.new_object<BlockStatement>(alloc, stmts);
}

DeclStmt* Semantic::BuildDeclStmt(VarDecl* decl) {
   return alloc.new_object<DeclStmt>(decl);
}

ExprStmt* Semantic::BuildExprStmt(Expr* expr) {
   return alloc.new_object<ExprStmt>(expr);
}

IfStmt* Semantic::BuildIfStmt(Expr* condition, Stmt* thenStmt, Stmt* elseStmt) {
   return alloc.new_object<IfStmt>(condition, thenStmt, elseStmt);
}

WhileStmt* Semantic::BuildWhileStmt(Expr* condition, Stmt* body) {
   return alloc.new_object<WhileStmt>(condition, body);
}

ForStmt* Semantic::BuildForStmt(Stmt* init, Expr* condition, Stmt* update,
                                Stmt* body) {
   return alloc.new_object<ForStmt>(init, condition, update, body);
}

ReturnStmt* Semantic::BuildReturnStmt(SourceRange loc, Expr* expr) {
   return alloc.new_object<ReturnStmt>(loc, expr);
}

} // namespace ast
