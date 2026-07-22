#pragma once

#include <algorithm>
#include <cstdint>
#include <ostream>
#include <tuple>
#include <type_traits>
#include <vector>

#include "mc/InstSelectNode.h"
#include "utils/Generator.h"

/* ===--------------------------------------------------------------------=== */
// mc::details forward declarations and concepts
/* ===--------------------------------------------------------------------=== */

namespace target {
class TargetDesc;
}

namespace mc {
struct MatchOptions;
}

namespace mc::details {

struct Operand;

// Concept to check if T is an MCOperand
template <typename T>
concept IsMCOperand = std::is_same_v<T, Operand>;

/**
 * @brief Concept to check if T is a TargetDef. Specifically, we require these
 * fields to be present in T:
 *
 *    enum PatternType
 *    enum PatternVariantType
 *    enum FragmentType
 *    enum VariantType
 *    enum RegClass
 *    int MaxStates
 *    int MaxOperands
 *    int MaxPatternsPerDef
 *    std::string_view GetPatternName(PatternType ty)
 *    std::string_view GetFragmentName(FragmentType ty)
 *    std::string_view GetRegClassName(RegClass ty)
 *
 * @tparam T
 */
template <typename T>
concept IsTargetDesc = requires {
   std::is_enum_v<typename T::PatternType>;
   std::is_enum_v<typename T::PatternVariantType>;
   std::is_enum_v<typename T::FragmentType>;
   std::is_enum_v<typename T::VariantType>;
   std::is_enum_v<typename T::RegClass>;
   { T::MaxStates } -> std::same_as<int const&>;
   { T::MaxOperands } -> std::same_as<int const&>;
   { T::MaxPatternsPerDef } -> std::same_as<int const&>;
   {
      T::GetPatternName(std::declval<typename T::PatternType>())
   } -> std::same_as<std::string_view>;
   {
      T::GetFragmentName(std::declval<typename T::FragmentType>())
   } -> std::same_as<std::string_view>;
   {
      T::GetRegClassName(std::declval<typename T::RegClass>())
   } -> std::same_as<std::string_view>;
   std::is_base_of_v<target::TargetDesc, T>;
};

class PatBase;
class PatDefBase;
class PatFragBase;
template <typename TD>
   requires IsTargetDesc<TD>
class PatDef;

} // namespace mc::details

namespace mc {
template <typename TD>
   requires details::IsTargetDesc<TD>
class PatternBuilderContext;
} // namespace mc

namespace mc::details {
template <typename T, typename TD>
concept IsPatternBuilder = requires {
   { T::GetAllPatterns() } -> utils::tuple_like;
   { T::GetAllFragments() } -> utils::tuple_like;
   {
      T::ComparePattern(std::declval<mc::details::PatDefBase const*>(),
                        std::declval<mc::details::PatDefBase const*>())
   } -> std::same_as<bool>;
   {
      T::MatchFragment(std::declval<mc::details::PatFragBase const&>(),
                       std::declval<mc::MatchOptions&>(),
                       std::declval<unsigned>())
   } -> std::same_as<bool>;
   std::is_base_of_v<PatternBuilderContext<TD>, T>;
};
} // namespace mc::details

namespace mc::details {

/* ===--------------------------------------------------------------------=== */
// Detail base classes: Operand, PatBase, PatDefBase, PatFragBase
/* ===--------------------------------------------------------------------=== */

/**
 * @brief Defines an MC instruction pattern's operand and an instruction in the
 * pattern matching bytecode tape.
 */
struct [[gnu::packed]] Operand final {
   enum class Type {
      None,
      Immediate,
      Register,
      Fragment,
      Push,
      Pop,
      CheckNodeType,
      CheckOperandType
   };
   Type type : 8;
   uint32_t data : 24;
   constexpr Operand() : type{Type::None}, data{} {}
   constexpr Operand(Type type, uint32_t data)
         : type{type}, data{data & 0x00FF'FFFF} {}
   constexpr bool operator==(Operand const& other) const {
      return other.type == type && other.data == data;
   }
};

// Abstract base class for all MC patterns
class PatBase {
public:
   virtual utils::Generator<Operand const*> bytecode() const = 0;
   virtual ~PatBase() = default;
   // Does this pattern match the given node?
   bool matches(MatchOptions) const;
   // Print the pattern
   std::ostream& print(std::ostream&, int indent = 0) const;
   // Dump the pattern
   void dump() const;
};

// Abstract base class for all MC pattern definitions
class PatDefBase {
public:
   // Dump the pattern
   void dump() const;
   // Print the pattern
   std::ostream& print(std::ostream&, int indent = 0) const;
   /**
    * @brief Adjust the index so that they are contiguous with the pattern
    * fragments. For ex, if we have an input array [reg, frag, reg] where
    * frag has 3 inputs itself, adjustOperandIndex(2) will return 4. Setting
    * idx to the number of inputs will return the total number of operands.
    *
    * @param index The input index to adjust
    * @param TD The target description
    * @return unsigned The adjusted index
    */
   unsigned adjustOperandIndex(unsigned index, target::TargetDesc const& TD) const;
   /**
    * @brief Get the input operand where the index has been adjusted to be
    * contiguous with the pattern fragments. This means, the pattern fragment
    * inputs are expanded into the input array and can be indexed directly.
    *
    * @param idx The adjusted index
    * @param TD The target description
    * @return MCOperand The input operand
    */
   Operand getInputAdjusted(unsigned idx, target::TargetDesc const& TD) const;

public:
   virtual Operand getInput(unsigned idx) const = 0;
   virtual Operand getOutput(unsigned idx) const = 0;
   virtual unsigned numInputs() const = 0;
   virtual unsigned numOutputs() const = 0;
   virtual unsigned maxTapeLength() const = 0;
   virtual utils::Generator<PatBase const*> patterns() const = 0;
   virtual NodeKind getDAGKind() const = 0;
   virtual std::string_view name() const = 0;
   virtual ~PatDefBase() = default;
};

// Abstract base class for all MC pattern fragments
class PatFragBase {
public:
   virtual unsigned kind() const = 0;
   virtual Operand getInput(unsigned idx) const = 0;
   virtual unsigned numInputs() const = 0;
   virtual std::string_view name() const = 0;
   virtual ~PatFragBase() = default;
};

/* ===--------------------------------------------------------------------=== */
// Detail implementation: Pat<TD>
/* ===--------------------------------------------------------------------=== */

// Defines the rule to match an MC pattern
template <typename TD>
   requires IsTargetDesc<TD>
class Pat final : public PatBase {
public:
   unsigned N;
   Operand tape[TD::MaxStates];

public:
   // Empty pattern (matches nothing)
   constexpr Pat() : N{0} {}
   // 1st base case is an ISel node type
   constexpr Pat(mc::NodeKind op) : N{1} {
      tape[0] = Operand{Operand::Type::CheckNodeType, static_cast<uint16_t>(op)};
   }
   // 2nd base case is an MCPatternDef operand index
   constexpr Pat(uint16_t idx) : N{1} {
      tape[0] = Operand{Operand::Type::CheckOperandType, idx};
   }
   // Recursively construct the DAG pattern to match
   constexpr Pat(std::initializer_list<Pat> children) {
      // Populate the tape
      unsigned i = 0;
      for(auto child : children) {
         // Push if child.N > 1
         if(child.N > 1) tape[i++] = Operand{Operand::Type::Push, 0};
         // Add the tape contents of the children
         for(unsigned j = 0; j < child.N; j++) {
            if(i >= TD::MaxStates)
               throw "MCPattern tape is out of space! Maybe increase MaxStates?";
            tape[i++] = child.tape[j];
         }
         // Pop if child.N > 1
         if(child.N > 1) tape[i++] = Operand{Operand::Type::Pop, 0};
      }
      // Update the number of elements in our tape
      N = i;
   }
   // Grab the bytecode from the tape
   utils::Generator<Operand const*> bytecode() const override {
      for(unsigned i = 0; i < N; i++) {
         co_yield &tape[i];
      }
   }
};

/* ===--------------------------------------------------------------------=== */
// Detail helper class: PatOpList
/* ===--------------------------------------------------------------------=== */

// Defines MC pattern input
template <typename TD, bool isInput>
   requires IsTargetDesc<TD>
struct PatOpList final {
   unsigned size = 0;
   std::array<Operand, TD::MaxOperands> ops;
   constexpr PatOpList(std::initializer_list<Operand> list) {
      for(const auto op : list) {
         if(size >= TD::MaxOperands)
            throw "MCPatternDef operand array is out of space! Maybe increase "
                  "MaxOperands?";
         this->ops[size++] = op;
      }
   }
};

/* ===--------------------------------------------------------------------=== */
// Detail implementation: PatDef<TD>
/* ===--------------------------------------------------------------------=== */

// Defines the MC pattern, but not the rule for matching the pattern
// TODO(kevin): Validate the DAG pattern matchings
template <typename TD>
   requires IsTargetDesc<TD>
class PatDef final : public PatDefBase {
private:
   TD::PatternType type_;
   TD::VariantType var_;
   std::array<Operand, TD::MaxOperands> inputs;
   std::array<Operand, TD::MaxOperands> outputs;
   std::array<Pat<TD>, TD::MaxPatternsPerDef> patterns_;
   unsigned ninputs = 0;
   unsigned noutputs = 0;
   unsigned npatterns = 0;
   unsigned maxTapeLength_ = 0;

public:
   // Construct a pattern definition with a given instruction type
   constexpr PatDef(TD::PatternType type, TD::VariantType var)
         : type_{type}, var_{var} {}
   // Add a pattern to the definition
   consteval PatDef<TD> operator<<(Pat<TD> const& pat) const {
      auto P = PatDef{*this};
      if(pat.N) {
         if(P.npatterns >= TD::MaxPatternsPerDef)
            throw "Too many patterns! Increase MaxPatternsPerDef?";
         P.patterns_[P.npatterns++] = pat;
         P.maxTapeLength_ = maxTapeLength_ > pat.N ? maxTapeLength_ : pat.N;
      }
      return P;
   }
   // Add an input or output operand list to the pattern
   template <bool isInput>
   consteval PatDef<TD> operator<<(PatOpList<TD, isInput> const& list) const {
      auto P = PatDef{*this};
      if(isInput) {
         P.inputs = list.ops;
         P.ninputs = list.size;
      } else {
         P.outputs = list.ops;
         P.noutputs = list.size;
      }
      return P;
   }
   // Grab the patterns
   utils::Generator<PatBase const*> patterns() const override {
      for(unsigned i = 0; i < npatterns; i++) {
         co_yield &patterns_[i];
      }
   }
   // Get the pattern name
   std::string_view name() const override { return TD::GetPatternName(type_); }
   // Get the DAG kind for this pattern
   NodeKind getDAGKind() const override {
      return static_cast<NodeKind>(patterns_[0].tape[0].data);
   }
   // Coneval version of getDAGKind
   consteval NodeKind constEvalGetDAGKind() const {
      return static_cast<NodeKind>(patterns_[0].tape[0].data);
   }
   // Get the nth input node
   Operand getInput(unsigned idx) const override {
      assert(idx < ninputs);
      return inputs[idx];
   }
   // Get the nth output node
   Operand getOutput(unsigned idx) const override {
      assert(idx < noutputs);
      return outputs[idx];
   }
   unsigned numInputs() const override { return ninputs; }
   unsigned numOutputs() const override { return noutputs; }
   unsigned maxTapeLength() const override { return maxTapeLength_; }
};

/* ===--------------------------------------------------------------------=== */
// Detail implementation: MCPatFrag<TD>
/* ===--------------------------------------------------------------------=== */

template <typename TD>
   requires IsTargetDesc<TD>
class MCPatFrag : public PatFragBase {
private:
   TD::FragmentType type_;
   std::array<Operand, TD::MaxOperands> inputs;
   unsigned ninputs = 0;

public:
   consteval MCPatFrag() : type_(TD::FragmentType::LAST_MEMBER) {}
   consteval MCPatFrag(TD::FragmentType type) : type_{type} {}
   // Add an input or output operand list to the pattern
   consteval MCPatFrag<TD> operator<<(PatOpList<TD, true> const& list) const {
      auto P = MCPatFrag{*this};
      P.inputs = list.ops;
      P.ninputs = list.size;
      return P;
   }
   // Get the fragment name
   std::string_view name() const override { return TD::GetFragmentName(type_); }
   // Get the nth input node
   Operand getInput(unsigned idx) const override {
      assert(idx < ninputs);
      return inputs[idx];
   }
   // Get the number of inputs
   unsigned numInputs() const override { return ninputs; }
   // Get the underlying TD kind
   unsigned kind() const override { return static_cast<unsigned>(type_); }
   // Consteval version of kind
   consteval TD::FragmentType constEvalKind() const { return type_; }
};

} // namespace mc::details

/* ===--------------------------------------------------------------------=== */
// MC pattern matching public API
/* ===--------------------------------------------------------------------=== */

namespace mc {

/// @brief
using PatternDef = mc::details::PatDefBase;
/// @brief
using Pattern = mc::details::PatBase;
/// @brief
using PatternFragment = mc::details::PatFragBase;

/**
 * @brief
 *
 */
class PatternProviderBase {
public:
   /// @brief Gets the pattern list for a given instruction type
   virtual utils::Generator<PatternDef const*> getPatternFor(
         NodeKind Kind) const = 0;
   /// @brief Iterates over all the patterns in the target
   virtual utils::Generator<PatternDef const*> patterns() const = 0;
   /// @brief Gets the fragment for a given fragment type
   virtual PatternFragment const& getFragment(unsigned Kind) const = 0;
   /// @brief ?
   virtual bool matchFragment(PatternFragment const& Frag, MatchOptions& MO,
                              unsigned OpIdx) const = 0;
   /// @brief ?
   void dumpPatterns() const;
   /// @brief ?
   virtual std::ostream& printPatterns(std::ostream& os) const = 0;
   /// @brief Virtual destructor
   virtual ~PatternProviderBase() = default;
};

template <typename TD>
   requires mc::details::IsTargetDesc<TD>
class PatternBuilderContext {
public:
   using define = mc::details::PatDef<TD>;
   using fragment = mc::details::MCPatFrag<TD>;
   using pattern = mc::details::Pat<TD>;
   using inputs = mc::details::PatOpList<TD, true>;
   using outputs = mc::details::PatOpList<TD, false>;

protected:
   // Shorthand to create a new imm MC operand
   static consteval auto imm(uint8_t size) {
      using mc::details::Operand;
      return Operand{Operand::Type::Immediate, size};
   }
   // Shorthand to create a new register MC operand
   static consteval auto reg(TD::RegClass T) {
      using mc::details::Operand;
      return Operand{Operand::Type::Register, static_cast<uint8_t>(T)};
   }
   // Shorthand to create a new pattern fragment MC operand
   static consteval auto frag(TD::FragmentType T) {
      using mc::details::Operand;
      return Operand{Operand::Type::Fragment, static_cast<uint8_t>(T)};
   }
};

/**
 * @brief
 *
 * @tparam TD
 */
template <typename TD, class PP>
   requires mc::details::IsTargetDesc<TD> && mc::details::IsPatternBuilder<PP, TD>
class PatternProvider final : public PatternProviderBase {
public:
   using define = mc::details::PatDef<TD>;
   using fragment = mc::details::MCPatFrag<TD>;
   using pattern = mc::details::Pat<TD>;

private:
   using FragArrayT =
         std::array<fragment, static_cast<size_t>(TD::FragmentType::LAST_MEMBER)>;

   static consteval auto grabAllFragments() {
      FragArrayT fragments;
      for(auto& frag : utils::array_from_tuple(PP::GetAllFragments()))
         fragments[static_cast<size_t>(frag.constEvalKind())] = frag;
      return fragments;
   }

   static inline std::array<std::vector<define const*>,
                            static_cast<size_t>(NodeKind::LAST_MEMBER)>
         patterns_{};

   static constexpr auto patterns1_{utils::array_from_tuple(PP::GetAllPatterns())};
   static constexpr FragArrayT fragments_{grabAllFragments()};

public:
   PatternProvider() {
      // Initialize the patterns
      for(auto& list : patterns_) list.clear();
      for(auto& def : patterns1_) {
         auto kind = static_cast<size_t>(def.getDAGKind());
         patterns_[kind].push_back(&def);
      }
      // Sort the patterns
      for(auto& list : patterns_) {
         std::sort(list.begin(), list.end(), [](auto const* a, auto const* b) {
            return PP::ComparePattern(a, b);
         });
      }
   }

public:
   utils::Generator<PatternDef const*> getPatternFor(
         NodeKind kind) const override final {
      for(auto& def : patterns_[static_cast<size_t>(kind)]) co_yield def;
   }
   utils::Generator<PatternDef const*> patterns() const override final {
      for(auto& list : patterns_)
         for(auto& def : list) co_yield def;
   }
   PatternFragment const& getFragment(unsigned kind) const override final {
      return fragments_[kind];
   }
   std::ostream& printPatterns(std::ostream& os) const override {
      unsigned frag = 0;
      for(auto& list : patterns_) {
         os << "Patterns for "
            << mc::InstSelectNode::NodeKind_to_string(
                     static_cast<mc::NodeKind>(frag++), "??")
            << ":\n";
         for(auto* def : list) {
            os << "  " << def->name() << ": ";
            for(unsigned i = 0; i < def->numInputs(); i++)
               printOperand(os, def->getInput(i)) << " ";
            os << "-> ";
            for(unsigned i = 0; i < def->numOutputs(); i++)
               printOperand(os, def->getOutput(i)) << " ";
            os << "\n";
         }
      }
      return os;
   }
   bool matchFragment(mc::PatternFragment const& Frag, mc::MatchOptions& MO,
                      unsigned OpIdx) const override {
      return PP::MatchFragment(Frag, MO, OpIdx);
   }

private:
   static std::ostream& printOperand(std::ostream& os, details::Operand op) {
      using Op = mc::details::Operand::Type;
      switch(op.type) {
         case Op::Immediate:
            os << "Imm(" << op.data << ")";
            break;
         case Op::Register:
            os << "Reg(" << TD::GetRegClassName(static_cast<TD::RegClass>(op.data))
               << ")";
            break;
         case Op::Fragment:
            os << "Frag("
               << TD::GetFragmentName(static_cast<TD::FragmentType>(op.data))
               << ")";
            break;
         default:
            os << "??";
            break;
      }
      return os;
   }
};

/**
 * @brief Struct of options passed to match a pattern
 */
struct MatchOptions final {
   target::TargetDesc const& TD;
   details::PatDefBase const* def;
   std::vector<InstSelectNode*>& operands;
   std::vector<InstSelectNode*>& nodesToDelete;
   InstSelectNode* node;
};

} // namespace mc
