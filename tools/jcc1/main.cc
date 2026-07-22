#include <unistd.h>

#include <filesystem>
#include <iostream>
#include <iterator>
#include <sstream>
#include <string>
#include <unordered_set>

#include "AllPasses.h"
#include "diagnostics/Diagnostics.h"
#include "passes/IRPasses.h"
#include "third-party/CLI11.h"
#include "utils/PassManager.h"

enum class InputMode { File, Stdin };
void pretty_print_errors(SourceManager& SM, diagnostics::DiagnosticEngine& diag);

int main(int argc, char** argv) {
   InputMode optInputMode = InputMode::Stdin;
   std::string optStdlibPath = "/u/cs444/pub/stdlib/6.1/";
   bool optSplit = false;
   bool optCompile = false;
   bool optFreestanding = false;
   bool optDisableHeapReuse = false;
   bool optCodeGen = false;
   int verboseLevel = 0;
   std::string optOutputFile = "";
   std::string optPipeline = "";

   // Create the pass manager and source manager
   CLI::App app{"Joos1W Compiler Frontend", "jcc1"};
   utils::PassManager PM{app};
   SourceManager SM{};

   // clang-format off
   // Build the jcc1-specific command line options
   app.add_flag("-x,--split", optSplit, "Split the input file whose contents are delimited\nby \"---\" into multiple compilation units");
   auto optVerboseLevel = app.add_flag("-v", "Set the verbosity level")
      ->expected(0, 1)
      ->check(CLI::Range(0, 3));
   app.add_flag("-c", optCompile, "Compile-only, running all the front-end passes.");
   app.add_flag("-s", optCodeGen, "Run the front-end passes, then generate IR code. Implies -c.");
   app.add_option("-o", optOutputFile, "Output the generated code to this file.");
   app.add_option("--stdlib", optStdlibPath, "The path to the standard library to use for compilation")
      ->check(CLI::ExistingDirectory)
      ->expected(0, 1)
      ->capture_default_str();
   app.allow_extras();
   // Build the pass-specific global command line options
   app.add_flag("--print-dot", "If a printing pass is run, print any trees in DOT format");
   app.add_option("--print-output", "If a printing pass is run, will write the output to this file or directory");
   app.add_flag("--print-split", "If a printing pass is run, split the output into multiple files");
   app.add_flag("--print-ignore-std", "If a printing pass is run, ignore the standard library");
   app.add_flag("--enable-filename-check", "Check if the file name matches the class name");
   app.add_flag("--enable-dfa-check", "Check if the DFA is correct");
   app.add_flag("--disable-heap-reuse", optDisableHeapReuse, "Do not reuse heap memory between passes (for debugging heap GC issues)");
   app.add_flag("--freestanding", optFreestanding, "Do not include the standard library in the compilation");
   app.add_flag("--debug-mc", "Dump each function's machine code DAG to .dot files for debugging");
   // clang-format on

   // Build the front-end and optimization passes
   BuildFrontEndPasses(PM);
   BuildOptPasses(PM);
   
   // Add the pipeline option and print the pass names
   {
      std::ostringstream ss;
      ss << "The pipeline string to run. Below is a list of the passes.\n";
      ss << "  Front end passes:\n";
      for(auto const* pass : PM.Passes()) {
         auto name = pass->Name();
         auto desc = pass->Desc();
         if(name.empty()) continue;
         // Pad name with spaces to align the descriptions
         ss << "    " << name;
         for(size_t i = name.size(); i < 15; ++i) ss << " ";
         ss << desc << "\n";
      }
      /*ss << "  Optimization passes:\n";
      for(auto [name, desc] : PM.PassNames()) {
         // Pad name with spaces to align the descriptions
         ss << "    " << name;
         for(size_t i = name.size(); i < 15; ++i) ss << " ";
         ss << desc << "\n";
      }*/
      ss << "You can specify a comma separated list of passes to run.\n";
      ss << "Optimization passes are run in-order and may be specified more than "
            "once.\n";
      ss << "Frontend passes are always run first, and run only once.";
      app.add_option("-p,--pipeline", optPipeline, ss.str());
   }

   // Parse the command line options
   CLI11_PARSE(app, argc, argv);

   // Disable heap reuse if requested
   if(optDisableHeapReuse) PM.SetHeapReuse(false);

   // Validate the command line options
   {
      auto split = app.count("--print-split");
      auto output = app.get_option("--print-output")->reduced_results();
      // If print-split is set, the print-output must be set
      if(split && output.empty()) {
         std::cerr << "Error: --print-split requires --print-output" << std::endl;
         return 1;
      }
      // If print-split is set, the print-dot must be set
      if(split && !app.count("--print-dot")) {
         std::cerr << "Error: --print-split requires --print-dot" << std::endl;
         return 1;
      }
      // If -c is not set, -o must be set too
      if(!optCompile && optOutputFile.empty()) {
         std::cerr << "Error: missing option -o." << std::endl;
         return 1;
      }
      // If -c is set and -o is set, print a warning
      if(optCompile && !optOutputFile.empty()) {
         std::cerr << "Warning: -c is set, but -o is also set. Ignoring -o."
                   << std::endl;
      }
   }

   // Set the verbosity of the diagnostic engine
   for(auto r : optVerboseLevel->results()) {
      if(r == "true") {
         verboseLevel = 1;
      } else {
         try {
            verboseLevel = std::stoi(r);
         } catch(std::invalid_argument const& e) {
            std::cerr << "Invalid verbosity level: " << r << std::endl;
            return 1;
         }
      }
      PM.Diag().setVerbose(verboseLevel);
   }

   // Ensure the remaining arguments are all valid paths
   auto files{app.remaining()};
   for(auto const& path : files) {
      if(access(path.c_str(), F_OK) == -1) {
         std::cerr << "File " << path << " does not exist" << std::endl;
         return 1;
      }
      optInputMode = InputMode::File;
   }

   // Read the input into the source manager (either from file or stdin)
   if(optInputMode == InputMode::File) {
      for(auto const& path : files) {
         SM.addFile(path);
      }
   } else {
      // Read std cin input until EOF
      std::string buffer_{std::istreambuf_iterator<char>(std::cin),
                          std::istreambuf_iterator<char>()};
      SM.emplaceBuffer();
      // If we choose to split the input or not
      if(optSplit) {
         for(auto it = buffer_.begin(); it != buffer_.end(); ++it) {
            // If we see "---" in the input, split the input into multiple bufs
            if(*it == '-' && *(it + 1) == '-' && *(it + 2) == '-') {
               SM.emplaceBuffer(), it += 2;
            } else {
               SM.currentBuffer().push_back(*it);
            }
         }
      } else {
         SM.currentBuffer().insert(
               SM.currentBuffer().end(), buffer_.begin(), buffer_.end());
      }
   }

   // Add the standard library to the source manager by recursively searching
   // for .java files in the stdlib path
   if(!optFreestanding) {
      namespace fs = std::filesystem;
      fs::recursive_directory_iterator it{optStdlibPath};
      fs::recursive_directory_iterator end;
      for(; it != end; ++it) {
         if(it->is_regular_file() && it->path().extension() == ".java") {
            SM.addFile(it->path().string());
         }
      }
   }

   // Parse the pipeline string by splitting by ","
   std::vector<std::string> optPassNames;
   std::unordered_set<std::string> fePasses;
   {
      std::istringstream ss{optPipeline};
      std::string name;
      while(std::getline(ss, name, ',')) {
         if(name.empty()) continue;
         if(!PM.HasPass(name)) {
            std::cerr << "Error: Unknown pass " << name << std::endl;
            return 1;
         }
         auto& pass = PM.FindPass(name);
         switch(static_cast<PassTag>(pass.Tag())) {
            case PassTag::FrontendPass:
               fePasses.insert(name);
               break;
            case PassTag::BasicBlockPass:
            case PassTag::FunctionPass:
            case PassTag::CompilationUnitPass:
               optPassNames.push_back(name);
               break;
            default:
               std::cerr << "Error: Unknown pass " << name << std::endl;
               return 1;
         }
      }
      // Print the enabled passes
      if(!fePasses.empty()) {
         if(verboseLevel > 0) std::cerr << "Enabled front-end passes (unordered):";
         for(auto const& pass : fePasses) {
            if(verboseLevel > 0) std::cerr << " " << pass;
            PM.EnablePass(pass);
         }
         if(verboseLevel > 0) std::cerr << std::endl;
      }
      if(!optPassNames.empty()) {
         if(verboseLevel > 0)
            std::cerr << "Enabled optimization passes (in order):";
         for(auto const& pass : optPassNames)
            if(verboseLevel > 0) std::cerr << " " << pass;
         if(verboseLevel > 0) std::cerr << std::endl;
      }
   }

   // Build the front end pipeline now that we have the files
   {
      using utils::Pass;
      Pass* p2 = nullptr;
      for(auto file : SM.files()) {
         auto* p1 = &NewJoos1WParserPass(PM, file, p2);
         p2 = &NewAstBuilderPass(PM, p1);
      }
   }

   // Enable the default front-end pass to run
   if(fePasses.empty()) {
      PM.EnablePass("dfa");
   }

   // If we want to codegen, enable the codegen pass
   if(optCodeGen || !optCompile) {
      PM.EnablePass("codegen-tir");
   }

   // Run the front end passes
   PM.Init();
   if(!PM.Run()) {
      std::cerr << "Error running pass: " << PM.LastRun()->Desc() << std::endl;
      if(PM.Diag().hasErrors()) {
         pretty_print_errors(SM, PM.Diag());
         return 42;
      } else if(PM.Diag().hasWarnings()) {
         pretty_print_errors(SM, PM.Diag());
         return 43;
      }
   }

   // If we only "-c" is set and "-s" is not
   if(!optCodeGen && optCompile) {
      return 0;
   }

   // Run the middle-end pipeline now and add the IR context pass
   for(auto const& name : optPassNames) {
      PM.Reset();
      PM.EnablePass(name);
      PM.Run();
   }

   // Dump the generated code to the output file and exit when "-s" is set
   if(optCodeGen) {
      std::ofstream out{optOutputFile};
      PM.FindPass<passes::IRContext>().CU().print(out);
      out.close();
      return 0;
   }

   if(verboseLevel > 0) {
      std::cerr << "*** Running backend machine-code pipeline... ***" << std::endl;
   }

   if(verboseLevel >= 3) {
      // TD.patternProvider().dumpPatterns();
   }

   // PM.Reset();
   // PM.EnablePass("isched");
   // PM.Run();

   return 0;
}
