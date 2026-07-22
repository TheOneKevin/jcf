#include "CompilerPasses.h"

#include <memory>
#include <string_view>
#include <memory_resource>

#include "ast/AST.h"
#include "diagnostics/Diagnostics.h"
#include "diagnostics/Location.h"
#include "diagnostics/SourceManager.h"
#include "grammar/Joos1WGrammar.h"
#include "parsetree/ParseTreeVisitor.h"
#include "semantic/NameResolver.h"
#include "semantic/Semantic.h"
#include "third-party/CLI11.h"
#include "utils/PassManager.h"
#include "utils/Utils.h"

using std::string_view;
using utils::Pass;
using utils::PassManager;

namespace passes::joos1 {

/* ===--------------------------------------------------------------------=== */
// Parser
/* ===--------------------------------------------------------------------=== */

void Parser::Run() {
   // Print the file being parsed if verbose
   if(PM().Diag().Verbose()) {
      auto os = PM().Diag().ReportDebug();
      os << "Parsing file ";
      SourceManager::print(os.get(), file_);
   }
   // Check for non-ASCII characters
   checkNonAscii(SourceManager::getBuffer(file_));
   // Parse the file
   auto& alloc = NewAlloc(Lifetime::Managed);
   Joos1WParser parser{file_, alloc, &PM().Diag()};
   int result = parser.parse(tree_);
   // If no parse tree was generated, report error if not already reported
   if((result != 0 || !tree_) && !PM().Diag().hasErrors())
      PM().Diag().ReportError(SourceRange{file_}) << "failed to parse file";
   if(result != 0 || !tree_) return;
   // If the parse tree is poisoned, report error
   if(tree_->is_poisoned()) {
      PM().Diag().ReportError(SourceRange{file_}) << "parse tree is poisoned";
      return;
   }
   // If the parse tree has invalid literal types, report error
   if(!isLiteralTypeValid(tree_)) {
      PM().Diag().ReportError(SourceRange{file_})
            << "invalid literal types in parse tree";
      return;
   }
}

bool Parser::isLiteralTypeValid(parsetree::Node* node) {
   if(node == nullptr) return true;
   if(node->get_node_type() == parsetree::Node::Type::Literal)
      return static_cast<parsetree::Literal*>(node)->isValid();
   for(size_t i = 0; i < node->num_children(); i++) {
      if(!isLiteralTypeValid(node->child(i))) return false;
   }
   return true;
}

void Parser::checkNonAscii(std::string_view str) {
   for(unsigned i = 0; i < str.length(); i++) {
      if(static_cast<unsigned char>(str[i]) > 127) {
         PM().Diag().ReportError(SourceRange{file_})
               << "non-ASCII character in file";
         return;
      }
   }
}

/* ===--------------------------------------------------------------------=== */
// AstContextPass
/* ===--------------------------------------------------------------------=== */

void AstContext::Run() {
   sema = std::make_unique<ast::Semantic>(NewAlloc(Lifetime::Managed), PM().Diag());
}

/* ===--------------------------------------------------------------------=== */
// AstBuilderPass
/* ===--------------------------------------------------------------------=== */

/// @brief Prints the parse tree back to the parent node
/// @param node Node to trace back to the parent
static inline void trace_node(parsetree::Node const* node, std::ostream& os) {
   if(node->parent() != nullptr) {
      trace_node(node->parent(), os);
      os << " -> ";
   }
   os << node->type_string() << std::endl;
}

/// @brief Marks a node and all its parents
/// @param node The node to mark
static inline void mark_node(parsetree::Node* node) {
   if(!node) return;
   mark_node(node->parent());
   node->mark();
}

AstBuilder::AstBuilder(PassManager& PM, Parser& dep) noexcept
      : Pass(PM), dep{dep} {}

void AstBuilder::Init() {
   optCheckName = PM().GetExistingOption("--enable-filename-check");
}

void AstBuilder::Run() {
   // Get the parse tree and the semantic analysis
   auto& sema = GetPass<AstContext>().Sema();
   auto* PT = dep.Tree();
   // Create a new heap just for creating the AST
   auto& alloc = NewAlloc(Lifetime::Temporary);
   parsetree::ParseTreeVisitor visitor{sema, alloc};
   // Store the result in the pass
   try {
      cu_ = visitor.visitCompilationUnit(PT);
   } catch(const parsetree::ParseTreeException& e) {
      PM().Diag().ReportError(SourceRange{}) << "ParseTreeException occured";
      std::cerr << "ParseTreeException: " << e.what() << " in file ";
      SourceManager::print(std::cerr, dep.File());
      std::cerr << std::endl;
      std::cerr << "Parse tree trace:" << std::endl;
      trace_node(e.get_where(), std::cerr);
      return;
   }
   if(cu_ == nullptr && !PM().Diag().hasErrors()) {
      PM().Diag().ReportError(PT->location()) << "failed to build AST";
      return;
   }
   // Check if the file name matches the class name
   bool shouldCheck = optCheckName && optCheckName->as<bool>();
   std::pmr::string fileName{SourceManager::getFileName(dep.File()), alloc};
   if(!fileName.empty() && shouldCheck) {
      auto cuBody = cu_->bodyAsDecl();
      // Grab the file without the path and the extension
      fileName = fileName.substr(0, fileName.find_last_of('.'));
      fileName = fileName.substr(fileName.find_last_of('/') + 1);
      if(cuBody->name() != fileName) {
         PM().Diag().ReportError(cuBody->location())
               << "class/interface name does not match file name: "
               << cuBody->name() << " != " << fileName;
         return;
      }
   }
}

/* ===--------------------------------------------------------------------=== */
// Linker
/* ===--------------------------------------------------------------------=== */

void Linker::Run() {
   std::pmr::vector<ast::CompilationUnit*> cus{};
   // Get the semantic analysis
   auto& sema = GetPass<AstContext>().Sema();
   // Create the linking unit
   for(auto* pass : GetPasses<AstBuilder>())
      cus.push_back(pass->CompilationUnit());
   lu_ = sema.BuildLinkingUnit(cus);
}

/* ===--------------------------------------------------------------------=== */
// PrintAST
/* ===--------------------------------------------------------------------=== */

class PrintAST final : public Pass {
public:
   PrintAST(PassManager& PM) noexcept : Pass(PM) {}
   string_view Name() const override { return "print-ast"; }
   string_view Desc() const override { return "Print AST"; }

   void Init() override {
      optDot = PM().GetExistingOption("--print-dot");
      optOutput = PM().GetExistingOption("--print-output");
      optSplit = PM().GetExistingOption("--print-split");
      optIgnoreStd = PM().GetExistingOption("--print-ignore-std");
   }

   void Run() override {
      // 1a. Grab the AST node
      auto& pass = GetPass<Linker>();
      auto* node = pass.LinkingUnit();
      if(!node) return;
      // 1b. Create a new heap to build a new AST if we're ignoring stdlib
      auto& alloc = NewAlloc(Lifetime::Temporary);
      ast::Semantic newSema{alloc, PM().Diag()};
      if(optIgnoreStd && optIgnoreStd->count()) {
         std::pmr::vector<ast::CompilationUnit*> cus{alloc};
         for(auto* cu : node->compliationUnits())
            if(!cu->isStdLib()) cus.push_back(cu);
         node = newSema.BuildLinkingUnit(cus);
      }
      // 2. Interpret the command line options
      bool printDot = optDot && optDot->count();
      bool printSplit = optSplit && optSplit->count();
      std::string outputPath = optOutput ? optOutput->as<std::string>() : "";
      // 3. Print the AST depending on the options
      if(!printSplit) {
         // 1. Now we try to open the output file if exists
         std::ofstream file{};
         if(!outputPath.empty()) {
            file = std::ofstream{outputPath};
            if(!file.is_open()) {
               PM().Diag().ReportError(SourceRange{})
                     << "failed to open output file " << outputPath;
               return;
            }
         }
         // 2. Set the output stream
         std::ostream& os = file.is_open() ? file : std::cout;
         // 3. Now we can print the AST
         if(printDot)
            node->printDot(os);
         else
            node->print(os);
      } else {
         namespace fs = std::filesystem;
         // 1. Create the output directory if it doesn't exist
         fs::create_directories(outputPath);
         // 2. Create a new file for each compilation unit
         for(auto const& cu : node->compliationUnits()) {
            if(!cu->body() || !cu->bodyAsDecl()->hasCanonicalName()) continue;
            std::string canonName{cu->bodyAsDecl()->getCanonicalName()};
            auto filepath = fs::path{outputPath} / fs::path{canonName + ".dot"};
            std::ofstream file{filepath};
            if(!file.is_open()) {
               PM().Diag().ReportError(SourceRange{})
                     << "failed to open output file " << filepath.string();
               return;
            }
            // 3. Set the output stream
            std::ostream& os = file;
            // 4. Now we can print the AST
            cu->printDot(os);
         }
      }
   }

private:
   void ComputeDependencies() override {
      AddDependency(GetPass<Linker>());
      AddDependency(GetPass<AstContext>());
   }
   CLI::Option *optDot, *optOutput, *optSplit, *optIgnoreStd;
};

} // namespace joos1

/* ===--------------------------------------------------------------------=== */
// Register the passes
/* ===--------------------------------------------------------------------=== */

REGISTER_PASS_NS(passes::joos1, AstContext);
REGISTER_PASS_NS(passes::joos1, Linker);
REGISTER_PASS_NS(passes::joos1, PrintAST);

Pass& NewJoos1WParserPass(PassManager& PM, SourceFile file, Pass* prev) {
   return PM.AddPass<passes::joos1::Parser>(file, prev);
}

Pass& NewAstBuilderPass(PassManager& PM, Pass* depends) {
   // "depends" must be a Parser
   auto* p = cast<passes::joos1::Parser*>(depends);
   return PM.AddPass<passes::joos1::AstBuilder>(*p);
}
