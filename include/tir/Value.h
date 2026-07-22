#pragma once

#include <utils/Assert.h>

#include <optional>
#include <ostream>
#include <string_view>
#include <memory_resource>

#include "tir/Context.h"
#include "utils/User.h"

namespace tir {

class Value;
class User;

/**
 * @brief The base class for all values (i.e., nodes) in the TIR.
 */
class Value : public utils::GraphNode<User> {
   using UseEqual = utils::details::UseEqual<User>;

public:
   Value(Context& ctx, Type* type)
         : utils::GraphNode<User>{ctx.alloc()},
           ctx_{ctx},
           type_{type},
           name_{std::nullopt},
           valueID_{ctx.getNextValueID()} {}
   tir::Context& ctx() { return ctx_; }
   tir::Context const& ctx() const { return ctx_; }
   Type* type() const { return type_; }
   std::string_view name() const { return name_.value(); }
   std::string_view unique_name() const {
      std::string ret = "%";
      if(name_) ret += name_.value() + ".";
      ret += valueID_;
      std::string_view stringViewRet = ret;
      return stringViewRet;
   }
   auto nameOpt() const { return name_.value(); }
   void setName(std::string_view name) {
      name_ = std::pmr::string{name, ctx_.alloc()};
   }
   std::ostream& printName(std::ostream& os) const {
      if(name_) os << name_.value() << ".";
      os << valueID_;
      return os;
   }
   virtual std::ostream& print(std::ostream&) const = 0;
   void dump() const;
   virtual ~Value() = default;
   virtual bool isFunction() const { return false; }
   virtual bool isFunctionArg() const { return false; }
   virtual bool isBasicBlock() const { return false; }
   virtual bool isInstruction() const { return false; }
   virtual bool isConstant() const { return false; }
   virtual bool isUser() const { return false; }

private:
   tir::Context& ctx_;
   tir::Type* const type_;
   std::optional<std::pmr::string> name_;
   unsigned valueID_;
};

/**
 * @brief A user of values in the TIR.
 */
class User : public utils::GraphNodeUser<User>, public Value {
   friend class Value;

public:
   User(Context& ctx, Type* type)
         : utils::GraphNodeUser<User>{ctx.alloc()}, Value{ctx, type} {}
   Value* getChild(unsigned idx) const {
      return static_cast<Value*>(getRawChild(idx));
   }
   bool isUser() const override { return true; }
};

std::ostream& operator<<(std::ostream& os, const Value& val);

} // namespace tir
