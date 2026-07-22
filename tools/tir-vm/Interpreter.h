#pragma once

// ===--------------------------------------------------------------------=== //
// tir-vm: a tree-walking interpreter for the compiler's in-memory TIR.
//
// The x86 backend (instruction selection) is broken, so instead of lowering
// TIR to machine code we execute it directly. This interpreter walks the SSA
// IR block-by-block; its per-instruction dispatch mirrors
// passes/mc/MIRBuilder.cc::buildInst, which is the authoritative reference for
// TIR semantics.
//
// Memory model: a single flat, byte-addressed store. VM "pointers" are byte
// offsets (uint64_t) into it. To stay consistent with the front-end (which
// bakes byte sizes into its jcf.malloc arguments as `(bits+1)/8`), we size
// everything in bytes using that same formula, and read struct field offsets
// as `getTypeOffsetAtIndex(i) / 8` (the getter returns bits). This makes GEP
// strides land exactly inside the buffers the front-end allocated.
// ===--------------------------------------------------------------------=== //

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <iostream>
#include <string>
#include <unordered_map>
#include <vector>

#include "tir/BasicBlock.h"
#include "tir/CompilationUnit.h"
#include "tir/Constant.h"
#include "tir/Instructions.h"
#include "tir/Type.h"
#include "tir/Value.h"
#include "utils/Utils.h"

namespace tirvm {

/// @brief Print a fatal message to stderr and terminate the process with the
/// given exit code. Used both for Joos runtime traps (exit 13, matching
/// runtime.s's __exception) and for internal VM errors (exit 70). std::exit
/// flushes stdio, so any buffered program output is written first.
[[noreturn]] inline void vmAbort(std::string const& msg, int code) {
   std::fflush(stdout);
   std::cerr << "tir-vm: " << msg << std::endl;
   std::exit(code);
}

/* ===--------------------------------------------------------------------=== */
// Runtime value: a raw payload plus its TIR type (so we know the bit width).
/* ===--------------------------------------------------------------------=== */

struct RTValue {
   // For integers: the value, zero-extended and masked to `type`'s bit width.
   // For pointers: a byte offset into Memory (0 == null).
   uint64_t bits = 0;
   tir::Type* type = nullptr;
};

/* ===--------------------------------------------------------------------=== */
// Flat byte-addressed memory
/* ===--------------------------------------------------------------------=== */

class Memory {
public:
   // Reserve [0, 16) so that offset 0 is an always-invalid null pointer.
   Memory() : brk_{16} { buf_.resize(64, 0); }

   /// @brief Bump-allocate `n` bytes (aligned) and return the byte offset. The
   /// storage is zero-initialized. Offsets remain valid across growth because
   /// they are integer indices, not raw pointers.
   uint64_t allocate(uint64_t n, uint64_t align = 8) {
      brk_ = (brk_ + align - 1) & ~(align - 1);
      uint64_t p = brk_;
      brk_ += (n ? n : 1);
      if(brk_ > buf_.size()) buf_.resize(std::max<uint64_t>(buf_.size() * 2 + 64, brk_), 0);
      return p;
   }

   /// @brief Load `nb` bytes little-endian, zero-extended into a uint64_t.
   uint64_t loadN(uint64_t addr, unsigned nb) {
      check(addr, nb);
      uint64_t v = 0;
      for(unsigned i = 0; i < nb; ++i) v |= uint64_t(buf_[addr + i]) << (8 * i);
      return v;
   }

   /// @brief Store the low `nb` bytes of `v` little-endian.
   void storeN(uint64_t addr, uint64_t v, unsigned nb) {
      check(addr, nb);
      for(unsigned i = 0; i < nb; ++i) buf_[addr + i] = uint8_t(v >> (8 * i));
   }

private:
   void check(uint64_t addr, unsigned nb) {
      if(addr == 0 || addr + nb > buf_.size())
         vmAbort("invalid memory access at offset " + std::to_string(addr), 70);
   }

   std::vector<uint8_t> buf_;
   uint64_t brk_;
};

/* ===--------------------------------------------------------------------=== */
// Interpreter
/* ===--------------------------------------------------------------------=== */

class Interpreter {
   /// @brief Per-call activation record.
   struct Frame {
      tir::Function* fn;
      // Bindings for instruction results, alloca addresses, and arguments.
      std::unordered_map<tir::Value*, RTValue> regs;
      // Predecessor block of the current block, for resolving phi nodes.
      tir::BasicBlock* prev = nullptr;
   };

public:
   using NativeFn =
         std::function<uint64_t(Interpreter&, std::vector<RTValue> const&)>;

   explicit Interpreter(tir::CompilationUnit& cu) : cu_{cu} {
      registerNatives();
      trace_ = std::getenv("TIRVM_TRACE") != nullptr;
   }

   Memory& memory() { return mem_; }

   /// @brief Run the synthesized initializers before the program entry: first
   /// every jcf.vtable.ctor.* (which populate each class's vtable with function
   /// pointers), then jcf.static.init (which runs static field initializers,
   /// e.g. System.out = new PrintStream()). These are not `external`, so the
   /// entry auto-detector never mistakes them for main/test.
   void runInitializers() {
      for(tir::Function* fn : cu_.functions()) {
         if(fn->hasBody() && fn->name().starts_with("jcf.vtable.ctor."))
            execFunction(fn, {});
      }
      if(auto* si = cu_.findFunction("jcf.static.init"))
         if(si->hasBody()) execFunction(si, {});
   }

   /// @brief Execute a function with the given argument values and return its
   /// result (an RTValue whose `bits` is meaningful only for non-void returns).
   RTValue execFunction(tir::Function* fn, std::vector<RTValue> const& args) {
      if(++depth_ > kMaxDepth)
         vmAbort("call stack exhausted (recursion too deep)", 70);
      tir::BasicBlock* bb = fn->getEntryBlock();
      if(!bb)
         vmAbort("cannot execute function without a body: " +
                       std::string(fn->name()),
                 70);

      Frame F;
      F.fn = fn;
      for(unsigned i = 0; i < fn->numParams(); ++i)
         F.regs[fn->arg(i)] =
               i < args.size() ? args[i] : RTValue{0, fn->getParamType(i)};

      for(;;) {
         // (a) Resolve leading phi nodes from the incoming edge (parallel copy).
         //     None are present without mem2reg, but handle them defensively.
         std::vector<std::pair<tir::PhiNode*, RTValue>> phiTmp;
         for(tir::PhiNode* phi : bb->phis()) {
            for(auto iv : phi->incomingValues()) {
               if(iv.pred == F.prev) {
                  phiTmp.push_back({phi, eval(iv.value, F)});
                  break;
               }
            }
         }
         for(auto& [p, val] : phiTmp) F.regs[p] = val;

         // (b) Run the block body until we hit a terminator.
         tir::BasicBlock* next = nullptr;
         bool returned = false;
         RTValue retVal;
         for(tir::Instruction* inst : *bb) {
            if(trace_) {
               std::cerr << "    | ";
               inst->print(std::cerr);
               std::cerr << std::endl;
            }
            if(dyn_cast<tir::PhiNode>(inst)) continue;

            if(auto* ret = dyn_cast<tir::ReturnInst>(inst)) {
               retVal = ret->isReturnVoid() ? RTValue{0, ret->type()}
                                            : eval(ret->getChild(0), F);
               returned = true;
               break;
            }
            if(auto* br = dyn_cast<tir::BranchInst>(inst)) {
               auto* s0 = br->getSuccessor(0);
               auto* s1 = br->getSuccessor(1);
               if(s0 == s1) {
                  next = s0; // unconditional; don't evaluate the condition
               } else {
                  RTValue c = eval(br->getCondition(), F);
                  next = maskTo(c.bits, c.type->getSizeInBits()) ? s0 : s1;
               }
               break;
            }
            if(auto* call = dyn_cast<tir::CallInst>(inst)) {
               RTValue r = doCall(call, F);
               if(!inst->type()->isVoidType()) F.regs[inst] = r;
               continue;
            }

            RTValue r = execInst(inst, F);
            if(!inst->type()->isVoidType()) F.regs[inst] = r;
         }

         if(returned) {
            --depth_;
            return retVal;
         }
         if(!next) vmAbort("basic block ended without a terminator", 70);
         F.prev = bb;
         bb = next;
      }
   }

private:
   /* --- integer helpers --- */

   static uint64_t maskTo(uint64_t x, unsigned bits) {
      return bits >= 64 ? x : (x & ((1ULL << bits) - 1));
   }
   // Interpret the low `bits` of x as a two's-complement signed value.
   static int64_t sext(uint64_t x, unsigned bits) {
      if(bits == 0 || bits >= 64) return static_cast<int64_t>(x);
      x = maskTo(x, bits);
      uint64_t sign = 1ULL << (bits - 1);
      return static_cast<int64_t>((x ^ sign) - sign);
   }
   // Byte size of a sized type, matching the front-end's `(bits+1)/8`.
   static uint32_t sizeBytes(tir::Type* t) {
      // A function used as a value is a function pointer (pointer-sized). Its
      // FunctionType has no bit width, so handle it before querying the size.
      if(dyn_cast<tir::FunctionType>(t)) return 8;
      uint32_t bits = t->getSizeInBits();
      uint32_t bytes = (bits + 1) / 8;
      return bytes ? bytes : 1;
   }

   /* --- value resolution --- */

   RTValue eval(tir::Value* v, Frame& F) {
      if(auto* ci = dyn_cast<tir::ConstantInt>(v))
         return {ci->zextValue(), v->type()};
      // A function used as a value (e.g. stored into a vtable slot) resolves to
      // its synthetic address; without this it would read as null.
      if(auto* fn = dyn_cast<tir::Function>(v))
         return {functionAddr(fn), v->type()};
      if(v->isConstant()) {
         if(auto* gv = dyn_cast<tir::GlobalVariable>(v))
            return {globalAddr(gv), v->type()};
         // null pointer and undef both read as 0.
         return {0, v->type()};
      }
      // Instruction results and function arguments live in the frame.
      auto it = F.regs.find(v);
      if(it == F.regs.end())
         vmAbort("use of value before definition", 70);
      return it->second;
   }

   uint64_t globalAddr(tir::GlobalVariable* gv) {
      auto [it, inserted] = globals_.try_emplace(gv, 0);
      if(inserted) {
         tir::Type* t = gv->type();
         it->second = mem_.allocate(t->isSizeBounded() ? sizeBytes(t) : 8);
      }
      return it->second;
   }

   // Assign each Function a unique synthetic "address" drawn from a reserved
   // high band that is disjoint from the heap, so that stray dereferences of a
   // function pointer trap instead of aliasing real data. Used to represent
   // functions as values in vtable slots and to resolve indirect calls.
   uint64_t functionAddr(tir::Function* fn) {
      auto [it, inserted] = funcAddr_.try_emplace(fn, 0);
      if(inserted) {
         it->second = nextFuncAddr_++;
         addrToFunc_[it->second] = fn;
      }
      return it->second;
   }

   /* --- non-terminator, non-call, non-phi instructions --- */

   RTValue execInst(tir::Instruction* inst, Frame& F) {
      if(auto* a = dyn_cast<tir::AllocaInst>(inst))
         return {mem_.allocate(sizeBytes(a->allocatedType())), inst->type()};

      if(auto* ld = dyn_cast<tir::LoadInst>(inst)) {
         uint64_t addr = eval(ld->getChild(0), F).bits;
         return {mem_.loadN(addr, sizeBytes(inst->type())), inst->type()};
      }
      if(auto* st = dyn_cast<tir::StoreInst>(inst)) {
         RTValue val = eval(st->getChild(0), F);
         uint64_t addr = eval(st->getChild(1), F).bits;
         mem_.storeN(addr, val.bits, sizeBytes(st->getChild(0)->type()));
         return {0, inst->type()};
      }
      if(auto* bin = dyn_cast<tir::BinaryInst>(inst))
         return execBinary(bin, F);
      if(auto* cmp = dyn_cast<tir::CmpInst>(inst))
         return execCmp(cmp, F);
      if(auto* c = dyn_cast<tir::ICastInst>(inst))
         return execCast(c, F);
      if(auto* gep = dyn_cast<tir::GetElementPtrInst>(inst))
         return execGEP(gep, F);

      vmAbort("unsupported instruction in interpreter", 70);
   }

   RTValue execBinary(tir::BinaryInst* bin, Frame& F) {
      using B = tir::Instruction::BinOp;
      unsigned w = bin->type()->getSizeInBits();
      uint64_t a = eval(bin->getChild(0), F).bits;
      uint64_t b = eval(bin->getChild(1), F).bits;
      uint64_t r = 0;
      switch(bin->binop()) {
         case B::Add: r = a + b; break;
         case B::Sub: r = a - b; break;
         case B::Mul: r = a * b; break;
         case B::And: r = a & b; break;
         case B::Or: r = a | b; break;
         case B::Xor: r = a ^ b; break;
         case B::Div: {
            int64_t l = sext(a, w), rr = sext(b, w);
            if(rr == 0) vmAbort("division by zero", 13);
            r = static_cast<uint64_t>(l / rr);
            break;
         }
         case B::Rem: {
            int64_t l = sext(a, w), rr = sext(b, w);
            if(rr == 0) vmAbort("division by zero", 13);
            r = static_cast<uint64_t>(l % rr);
            break;
         }
         default: vmAbort("unsupported binary operator", 70);
      }
      return {maskTo(r, w), bin->type()};
   }

   RTValue execCmp(tir::CmpInst* cmp, Frame& F) {
      using P = tir::Instruction::Predicate;
      unsigned w = cmp->getChild(0)->type()->getSizeInBits();
      uint64_t a = eval(cmp->getChild(0), F).bits;
      uint64_t b = eval(cmp->getChild(1), F).bits;
      bool res = false;
      switch(cmp->predicate()) {
         case P::EQ: res = maskTo(a, w) == maskTo(b, w); break;
         case P::NE: res = maskTo(a, w) != maskTo(b, w); break;
         case P::LT: res = sext(a, w) < sext(b, w); break;
         case P::GT: res = sext(a, w) > sext(b, w); break;
         case P::LE: res = sext(a, w) <= sext(b, w); break;
         case P::GE: res = sext(a, w) >= sext(b, w); break;
         default: vmAbort("unsupported compare predicate", 70);
      }
      return {res ? 1ULL : 0ULL, cmp->type()};
   }

   RTValue execCast(tir::ICastInst* c, Frame& F) {
      using C = tir::Instruction::CastOp;
      unsigned sw = c->getChild(0)->type()->getSizeInBits();
      unsigned dw = c->type()->getSizeInBits();
      uint64_t x = eval(c->getChild(0), F).bits;
      uint64_t r = 0;
      switch(c->castop()) {
         case C::Trunc: r = maskTo(x, dw); break;
         case C::ZExt: r = maskTo(x, sw); break;
         case C::SExt: r = maskTo(static_cast<uint64_t>(sext(x, sw)), dw); break;
         default: vmAbort("unsupported cast operator", 70);
      }
      return {maskTo(r, dw), c->type()};
   }

   RTValue execGEP(tir::GetElementPtrInst* gep, Frame& F) {
      uint64_t base = eval(gep->getPointerOperand(), F).bits;
      tir::Type* t = gep->getContainedType();
      for(tir::Value* idx : gep->indices()) {
         if(auto* st = dyn_cast<tir::StructType>(t)) {
            // Struct indices must be constant.
            uint32_t k = static_cast<uint32_t>(cast<tir::ConstantInt>(idx)->zextValue());
            base += st->getTypeOffsetAtIndex(k) / 8;
            t = st->getTypeAtIndex(k);
         } else if(auto* at = dyn_cast<tir::ArrayType>(t)) {
            // Array index may be dynamic; stride is the element's byte size.
            // (Never ArrayType::getSizeInBits(): the element GEP uses a
            //  length-0 array type, whose getSizeInBits() asserts.)
            uint64_t i = eval(idx, F).bits;
            base += i * sizeBytes(at->getElementType());
            t = at->getElementType();
         } else {
            vmAbort("getelementptr into non-aggregate type", 70);
         }
      }
      return {base, gep->type()};
   }

   /* --- calls: intrinsics, natives, and ordinary functions --- */

   RTValue doCall(tir::CallInst* call, Frame& F) {
      // Direct calls name a Function; indirect (virtual) calls carry a loaded
      // function pointer as the callee operand, which we resolve back to its
      // Function via the reserved-address map.
      tir::Function* callee = call->getCallee();
      if(!callee) {
         uint64_t addr = eval(call->getCalleeValue(), F).bits;
         auto it = addrToFunc_.find(addr);
         if(it == addrToFunc_.end())
            vmAbort("indirect call through invalid function pointer", 70);
         callee = it->second;
      }
      std::vector<RTValue> args;
      unsigned n = static_cast<unsigned>(call->nargs());
      args.reserve(n);
      for(unsigned i = 0; i < n; ++i)
         args.push_back(eval(call->getChild(1 + i), F)); // child 0 is the callee

      if(trace_) {
         std::cerr << "    > call " << callee->name() << " args=[";
         for(unsigned i = 0; i < n; ++i)
            std::cerr << (i ? ", " : "") << args[i].bits;
         std::cerr << "]" << std::endl;
      }

      using K = tir::Instruction::IntrinsicKind;
      if(callee == cu_.getIntrinsic(K::malloc)) {
         // The size is an i32. Guard against a negative or implausibly large
         // request (e.g. from `new T[negative]`) so we trap as a Joos runtime
         // error instead of throwing std::bad_alloc out of the allocator.
         int64_t sz = sext(args.at(0).bits, 32);
         if(sz < 0 || sz > (int64_t{1} << 30))
            vmAbort("invalid allocation size", 13);
         return {mem_.allocate(static_cast<uint64_t>(sz)),
                 callee->getReturnType()};
      }
      if(callee == cu_.getIntrinsic(K::exception)) {
         vmAbort("uncaught exception (jcf.exception)", 13);
      }
      if(callee == cu_.getIntrinsic(K::check_null)) {
         if(args.at(0).bits == 0) vmAbort("null pointer dereference", 13);
         return {0, callee->getReturnType()};
      }
      if(callee == cu_.getIntrinsic(K::check_array_bounds)) {
         uint64_t hdr = args.at(0).bits;
         int64_t idx = sext(args.at(1).bits, 32);
         int64_t len = static_cast<int32_t>(mem_.loadN(hdr, 4)); // length @ offset 0
         if(idx < 0 || idx >= len) vmAbort("array index out of bounds", 13);
         return {0, callee->getReturnType()};
      }

      if(callee->name().starts_with("NATIVE")) {
         auto it = natives_.find(std::string(callee->name()));
         if(it == natives_.end())
            vmAbort("unimplemented native symbol: " + std::string(callee->name()),
                    70);
         return {it->second(*this, args), callee->getReturnType()};
      }

      if(callee->hasBody()) return execFunction(callee, args);
      vmAbort("call to declared-only function: " + std::string(callee->name()), 70);
   }

   /* --- native symbol registry (the "JDK" symbols the runtime provides) --- */

   void registerNatives() {
      // java.io.OutputStream.nativeWrite(int b): write the low byte to stdout,
      // return 0. This single symbol backs every print/write path in the
      // stdlib (PrintStream extends OutputStream).
      natives_["NATIVEjava.io.OutputStream.nativeWrite"] =
            [](Interpreter&, std::vector<RTValue> const& a) -> uint64_t {
         std::putchar(static_cast<int>(a.at(0).bits & 0xFF));
         return 0;
      };
   }

   static constexpr unsigned kMaxDepth = 4096;

   tir::CompilationUnit& cu_;
   Memory mem_;
   std::unordered_map<tir::GlobalVariable*, uint64_t> globals_;
   std::unordered_map<std::string, NativeFn> natives_;
   // Function <-> synthetic address maps for functions-as-values / indirect
   // calls. The band starts at 4 GiB, far above any realistic heap offset.
   std::unordered_map<tir::Function*, uint64_t> funcAddr_;
   std::unordered_map<uint64_t, tir::Function*> addrToFunc_;
   uint64_t nextFuncAddr_ = 0x1'0000'0000ULL;
   unsigned depth_ = 0;
   bool trace_ = false;
};

} // namespace tirvm
