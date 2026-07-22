// ===--------------------------------------------------------------------=== //
// tir-vm: run a Joos1W program by interpreting the compiler's in-memory TIR.
//
// This mirrors jcc1's front-end driver (tools/jcc1/main.cc) to obtain a fully
// built tir::CompilationUnit, then hands it to the tree-walking Interpreter
// (Interpreter.h) instead of the (broken) x86 backend. The entry function's
// i32 return value becomes the process exit code.
// ===--------------------------------------------------------------------=== //

#include <filesystem>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

#include "AllPasses.h"
#include "diagnostics/Diagnostics.h"
#include "passes/IRPasses.h"
#include "third-party/CLI11.h"
#include "tir/CompilationUnit.h"
#include "tir/Constant.h"
#include "utils/PassManager.h"

#include "Interpreter.h"

namespace {

void printDiagnostics(diagnostics::DiagnosticEngine& diag) {
   for(auto const& err : diag.errors()) {
      err.emit(std::cerr);
      std::cerr << std::endl;
   }
   for(auto const& warn : diag.warnings()) {
      warn.emit(std::cerr);
      std::cerr << std::endl;
   }
}

// Selects the program entry: the function with a body that codegen tagged
// `external` (i.e. named `main`/`test`) and is not a native declaration.
tir::Function* findEntry(tir::CompilationUnit& CU, std::string const& wanted) {
   if(!wanted.empty()) {
      auto* f = CU.findFunction(wanted);
      if(!f)
         std::cerr << "tir-vm: no function named '" << wanted << "'" << std::endl;
      return f;
   }
   std::vector<tir::Function*> candidates;
   for(auto* f : CU.functions()) {
      if(f->hasBody() && static_cast<bool>(f->attrs().external) &&
         !f->name().starts_with("NATIVE")) {
         candidates.push_back(f);
      }
   }
   if(candidates.empty()) {
      std::cerr << "tir-vm: no entry point found (expected a static int "
                   "main/test). Use --entry <mangled-name>."
                << std::endl;
      return nullptr;
   }
   if(candidates.size() > 1) {
      std::cerr << "tir-vm: multiple entry candidates; pick one with --entry:"
                << std::endl;
      for(auto* f : candidates) std::cerr << "  " << f->name() << std::endl;
      return nullptr;
   }
   return candidates.front();
}

} // namespace

int main(int argc, char** argv) {
   std::string optStdlibPath = "jdk/";
   std::string optEntry = "";
   bool optFreestanding = false;
   bool optDisableHeapReuse = false;

   CLI::App app{"Joos1W TIR virtual machine", "tir-vm"};
   utils::PassManager PM{app};
   SourceManager SM{};

   // clang-format off
   app.add_option("--stdlib", optStdlibPath, "Path to the standard library to compile against")
      ->capture_default_str();
   app.add_option("--entry", optEntry, "Mangled name of the entry function to run (default: auto-detect)");
   app.allow_extras();
   // Pass-specific global options that some passes look up in Init(); registering
   // them (as jcc1 does) keeps GetExistingOption() from throwing.
   app.add_flag("--print-dot", "If a printing pass is run, print any trees in DOT format");
   app.add_option("--print-output", "If a printing pass is run, write the output here");
   app.add_flag("--print-split", "If a printing pass is run, split the output into multiple files");
   app.add_flag("--print-ignore-std", "If a printing pass is run, ignore the standard library");
   app.add_flag("--enable-filename-check", "Check if the file name matches the class name");
   app.add_flag("--enable-dfa-check", "Check if the DFA is correct");
   app.add_flag("--disable-heap-reuse", optDisableHeapReuse, "Do not reuse heap memory between passes");
   app.add_flag("--freestanding", optFreestanding, "Do not include the standard library");
   app.add_flag("--debug-mc", "Dump each function's machine code DAG to .dot files");
   // clang-format on

   // Register the front-end and (skipped) optimization/backend passes. We need
   // BuildOptPasses for the IRContext pass, which owns the CompilationUnit.
   BuildFrontEndPasses(PM);
   BuildOptPasses(PM);

   CLI11_PARSE(app, argc, argv);
   if(optDisableHeapReuse) PM.SetHeapReuse(false);

   // Collect the user's .java source files.
   auto files = app.remaining();
   if(files.empty()) {
      std::cerr << "tir-vm: no input files" << std::endl;
      return 1;
   }
   for(auto const& path : files) {
      if(!std::filesystem::exists(path)) {
         std::cerr << "tir-vm: file does not exist: " << path << std::endl;
         return 1;
      }
      SM.addFile(path);
   }

   // Add the bundled standard library (needed for java.io.OutputStream, etc).
   if(!optFreestanding) {
      namespace fs = std::filesystem;
      if(!fs::exists(optStdlibPath)) {
         std::cerr << "tir-vm: stdlib path does not exist: " << optStdlibPath
                   << " (pass --stdlib <dir> or --freestanding)" << std::endl;
         return 1;
      }
      for(auto it = fs::recursive_directory_iterator{optStdlibPath};
          it != fs::recursive_directory_iterator{};
          ++it) {
         if(it->is_regular_file() && it->path().extension() == ".java")
            SM.addFile(it->path().string());
      }
   }

   // Build the per-file parse + AST-build pipeline (mirrors jcc1).
   {
      using utils::Pass;
      Pass* prev = nullptr;
      for(auto file : SM.files()) {
         auto* parser = &NewJoos1WParserPass(PM, file, prev);
         prev = &NewAstBuilderPass(PM, parser);
      }
   }

   // Run only what's needed to produce TIR: semantic checks + codegen.
   PM.EnablePass("dfa");
   PM.EnablePass("codegen-tir");

   PM.Init();
   if(!PM.Run()) {
      std::cerr << "tir-vm: front-end failed";
      if(auto const* lr = PM.LastRun()) std::cerr << " at pass: " << lr->Desc();
      std::cerr << std::endl;
      printDiagnostics(PM.Diag());
      return 42;
   }

   // The CompilationUnit lives inside the IRContext pass; PM (and thus the CU's
   // bump-allocator heap) must stay alive for the whole interpretation below.
   tir::CompilationUnit& CU = PM.FindPass<passes::IRContext>().CU();

   if(std::getenv("TIRVM_DUMPIR")) CU.print(std::cerr);

   tir::Function* entry = findEntry(CU, optEntry);
   if(!entry) return 2;

   tirvm::Interpreter vm{CU};
   // Populate vtables and run static initializers before entering the program.
   vm.runInitializers();
   std::vector<tirvm::RTValue> args;
   for(unsigned i = 0; i < entry->numParams(); ++i) {
      auto* pty = entry->getParamType(i);
      if(pty->isPointerType()) {
         // Reference/array params (e.g. main's String[] args) get a valid empty
         // array object { i32 length = 0; ptr data = null } rather than null, so
         // programs that read args.length don't trap.
         uint64_t hdr = vm.memory().allocate(16);
         vm.memory().storeN(hdr, 0, 4); // length = 0
         args.push_back({hdr, pty});
      } else {
         args.push_back({0, pty});
      }
   }

   tirvm::RTValue ret = vm.execFunction(entry, args);

   int code = 0;
   if(ret.type && !ret.type->isVoidType())
      code = static_cast<int>(static_cast<int32_t>(ret.bits & 0xFFFFFFFFu));
   std::cerr << "tir-vm: '" << entry->name() << "' returned " << code << std::endl;
   return code & 0xFF; // process exit status is a single byte
}
