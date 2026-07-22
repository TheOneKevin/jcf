#pragma once

#include <string_view>
#include <unordered_map>
#include <memory_resource>

#include "tir/Constant.h"
#include "tir/Context.h"
#include "tir/Instructions.h"
#include "utils/Generator.h"

namespace tir {

class CompilationUnit {
   friend void RegisterAllIntrinsics(CompilationUnit& cu);

public:
   CompilationUnit(Context& ctx);
   CompilationUnit(const CompilationUnit&) = delete;
   CompilationUnit(CompilationUnit&&) = delete;
   CompilationUnit& operator=(const CompilationUnit&) = delete;
   CompilationUnit& operator=(CompilationUnit&&) = delete;

   /**
    * @brief Create a new function with the given type and name.
    *
    * @param type The type of the function
    * @param name The name of the function
    * @return Function* The function, or nullptr if it already exists
    */
   Function* CreateFunction(FunctionType* type, const std::string_view name) {
      if(findFunction(std::string{name})) return nullptr;
      auto* buf = ctx_.alloc().allocate_bytes(sizeof(Function), alignof(Function));
      auto* func = new(buf) Function{ctx_, this, type, name};
      globals_.emplace(name, func);
      return func;
   }

   /**
    * @brief Create a new global variable with the given type and name.
    *
    * @param type
    * @param name
    * @return GlobalVariable*
    */
   GlobalVariable* CreateGlobalVariable(Type* type, const std::string_view name) {
      if(findGlobalVariable(std::string{name})) return nullptr;
      auto* buf = ctx_.alloc().allocate_bytes(sizeof(GlobalVariable),
                                              alignof(GlobalVariable));
      auto* gv = new(buf) GlobalVariable{ctx_, type};
      globals_.emplace(name, gv);
      gv->setName(name);
      return gv;
   }

   /**
    * @brief Get the function with the given name.
    *
    * @param name The name of the function
    * @return Function* The function, or nullptr if it does not exist
    */
   Function* findFunction(const std::string_view name) {
      auto it = globals_.find(std::string{name});
      if(it == globals_.end()) return nullptr;
      auto go = it->second;
      return dyn_cast<Function>(go);
   }

   /**
    * @brief Find the global variable with the given name.
    *
    * @param name The name of the global variable
    * @return GlobalVariable* The global variable, or nullptr if it does not exist
    */
   GlobalVariable* findGlobalVariable(const std::string_view name) {
      auto it = globals_.find(std::string{name});
      if(it == globals_.end()) return nullptr;
      auto go = it->second;
      return dyn_cast<GlobalVariable>(go);
   }

   // Print the compilation unit to the given output stream.
   std::ostream& print(std::ostream& os) const;
   void dump() const;

   /// @brief Filters through the global objects and yields just the functions
   utils::Generator<Function*> functions() const {
      for(auto& [name, func] : globals_) {
         if(auto* f = dyn_cast<Function>(func)) co_yield f;
      }
   }

   /// @brief Yields all global objects in the compilation unit
   utils::Generator<GlobalObject*> global_objects() const {
      for(auto& [name, go] : globals_) co_yield go;
   }

   /// @brief Yields all global objects in the compilation unit
   utils::Generator<std::pair<std::string_view, GlobalObject*>> global_objects_kv()
         const {
      for(auto& [name, go] : globals_) co_yield std::pair{name, go};
   }

   /// @brief Get the context associated with this compilation unit
   auto& ctx() { return ctx_; }
   auto const& ctx() const { return ctx_; }

   /// @brief Filters through the global objects and yields just the variables
   utils::Generator<GlobalVariable*> global_variables() const {
      for(auto& [name, go] : globals_) {
         if(auto* gv = dyn_cast<GlobalVariable>(go)) co_yield gv;
      }
   }

   /// @brief Remove the global object with the given name
   void removeGlobalObject(std::string const& name) { globals_.erase(name); }

public:
   Function* getIntrinsic(Instruction::IntrinsicKind kind) {
      auto it = intrinsics_.find(kind);
      assert(it != intrinsics_.end());
      return it->second;
   }

private:
   void CreateIntrinsic(Instruction::IntrinsicKind kind, FunctionType* type) {
      auto* buf = ctx_.alloc().allocate_bytes(sizeof(Function), alignof(Function));
      auto name = Instruction::getIntrinsicName(kind);
      auto* func = new(buf) Function{ctx_, this, type, name};
      func->setAttrs(Function::Attrs{.intrinsic = true});
      intrinsics_.emplace(kind, func);
   }

private:
   Context& ctx_;
   std::pmr::unordered_map<std::string, GlobalObject*> globals_;
   std::pmr::unordered_map<Instruction::IntrinsicKind, Function*> intrinsics_;
};

} // namespace tir
