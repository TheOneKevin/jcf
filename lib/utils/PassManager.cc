#include "utils/PassManager.h"

#include <algorithm>
#include <memory>
#include <queue>
#include <unordered_set>
#include <vector>

#include "third-party/CLI11.h"
#include "utils/BumpAllocator.h"
#include "utils/Error.h"

namespace utils {

/* ===--------------------------------------------------------------------=== */
// Static helper functions & Default pass dispatcher
/* ===--------------------------------------------------------------------=== */

namespace {

/**
 * @brief Get the single name of a command line option
 * @param name The option name to get the single name of (ie., "-o,--option")
 * @return std::string The single name (ie., "option")
 */
std::string GetSingleName(std::string_view name) {
   std::vector<std::string> s, l;
   std::string p;
   std::tie(s, l, p) =
         CLI::detail::get_names(CLI::detail::split_names(std::string{name}));
   if(!l.empty()) return l[0];
   if(!s.empty()) return s[0];
   if(!p.empty()) return p;
   return std::string{name};
}

inline unsigned sat_sub(unsigned& a, unsigned b) {
   if(a < b)
      a = 0;
   else
      a -= b;
   return a;
}

class DefaultDispatcher final : public PassDispatcher {
public:
   std::string_view Name() override { return "Default Dispatcher"; }
   bool CanDispatch(Pass&) override { return true; }
   Generator<void*> Iterate(PassManager&) override { co_yield nullptr; }
};

DefaultDispatcher DefaultDispatcherInstance{};

} // namespace

/* ===--------------------------------------------------------------------=== */
// Pass
/* ===--------------------------------------------------------------------=== */

void Pass::AddDependency(Pass& pass) { PM().addDependency(*this, pass); }

CustomBufferResource* Pass::NewHeap(Pass::Lifetime lifetime) {
   // Check the pass is running
   if(state != State::Running) {
      throw FatalError("Pass requesting a heap is not running");
   }
   // Grab or create a free heap
   auto& heap = PM().findHeapFor(this, lifetime);
   // Re/initialize the heap
   heap.owner = this;
   heap.lifetime = lifetime;
   heap.refcount = ShouldPreserve() ? 2 : 1;
   heap.get()->reset();
   return heap.get();
}

BumpAllocator& Pass::NewAlloc(Pass::Lifetime lifetime) {
   // Check the pass is running
   if(state != State::Running) {
      throw FatalError("Pass requesting an allocator is not running");
   }
   // Create the allocator if not exists
   auto* heap = NewHeap(lifetime);
   return allocs_.emplace_back(heap);
}

Pass& Pass::GetPass(std::string_view name) { return PM().FindPass(name); }

/* ===--------------------------------------------------------------------=== */
// PassManager helpers
/* ===--------------------------------------------------------------------=== */

CLI::Option* PassManager::FindOption(std::string_view name) {
   std::string test_name = "--" + GetSingleName(name);
   if(test_name.size() == 3) test_name.erase(0, 1);
   return app_.get_option_no_throw(test_name);
}

CLI::Option* PassManager::GetExistingOption(std::string name) {
   auto opt = FindOption(name);
   if(opt == nullptr)
      throw FatalError("Pass requested nonexistent option: " + name);
   return opt;
}

void PassManager::addDependency(Pass& pass, Pass& depends) {
   if(state_ != State::Uninitialized) {
      throw FatalError("Cannot add dependencies after initialization. " +
                       std::string{pass.Name()} + " depends on " +
                       std::string{depends.Name()});
   }
   depGraph_[&depends].transpose.push_back(&pass);
   depGraph_[&pass].forward.push_back(&depends);
}

void PassManager::validate() const {
   for(int i = 0; static_cast<unsigned>(i) < passes_.size(); i++) {
      assert(passes_[i]->topoIdx == i);
   }
}

PassManager::HeapResource& PassManager::findHeapFor(Pass* owner,
                                                    Pass::Lifetime lifetime) {
   using Lifetime = Pass::Lifetime;
   for(auto& heap : heaps_) {
      if(!reuseHeaps_) [[unlikely]]
         break;
      /**
       * Conditions for reusing a resource:
       * 1) The resource is free (i.e., refcount is 0)
       * 2) The resource is temporary/managed and we are requesting a
       *    temporary/managed resource from the same pass.
       */
      bool canReuse =
            /* 1) */
            (heap.refcount == 0) ||
            /* 2) */
            ((lifetime == Lifetime::Temporary || lifetime == Lifetime::Managed) &&
             heap.lifetime == lifetime && heap.owner == owner);
      if(canReuse) {
         if(Diag().Verbose(2))
            Diag().ReportDebug() << "[HH] Reusing heap " << heap.id;
         return heap;
      }
   }
   // 3. Otherwise, create the resource and return it
   auto& heap = heaps_.emplace_back(std::make_unique<CustomBufferResource>());
   if(Diag().Verbose(2)) Diag().ReportDebug() << "[HH] Creating heap " << heap.id;
   return heap;
}

void PassManager::runPassLifeCycle(Pass& pass, int left, int right,
                                   bool lastIter) {
   if(pass.state != Pass::State::Initialized) {
      throw FatalError("Pass " + std::string{pass.Name()} +
                       " is not in the initialized state");
   }

   // 1. Run the pass
   if(Diag().Verbose()) {
      Diag().ReportDebug() << "[=>] Running pass #" << pass.topoIdx << " \""
                           << pass.Name() << "\": " << pass.Desc();
   }
   pass.state = Pass::State::Running;
   pass.Run();
   pass.state = Pass::State::Valid;
   assert((validate(), true));

   // 2. Persist any of the pass's resources and free self-resources
   pass.allocs_.clear();
   for(auto& heap : heaps_) {
      if(heap.owner != &pass) continue;
      // 2a. Check if anyone wants to acquire the current pass's resources
      for(auto pred : depGraph_[&pass].transpose) {
         // lastIter == we should acquire inter-chunk dependencies
         if(!lastIter && (pred->topoIdx < left || pred->topoIdx > right)) continue;
         heap.refcount++;
      }
      // 2b. Free the self-refs and temporary resources
      if(heap.lifetime == Pass::Lifetime::Managed) {
         if(sat_sub(heap.refcount, 1) == 0)
            heap.owner->GC();
      } else {
         heap.refcount = 0;
      }
   }

   // 3. Free any of the pass's (non self-ref) resources
   for(auto succ : depGraph_[&pass].forward) {
      // lastIter == we should release inter-chunk dependencies
      if(!lastIter && (succ->topoIdx < left || succ->topoIdx > right)) continue;
      for(auto& heap : heaps_) {
         if(heap.owner != succ) continue;
         if(heap.lifetime == Pass::Lifetime::Managed) {
            if(sat_sub(heap.refcount, 1) == 0)
               heap.owner->GC();
         }
      }
   }

   // 4. Print the heaps-in-use
   if(Diag().Verbose(2)) {
      for(auto& heap : heaps_) {
         if(heap.refcount > 0) {
            Diag().ReportDebug()
                  << "[--] Heap " << heap.id << " in use by "
                  << heap.owner->topoIdx << " (refcount: " << heap.refcount << ")";
         } else {
            Diag().ReportDebug() << "[--] Heap " << heap.id << " free";
         }
      }
   }
}

/* ===--------------------------------------------------------------------=== */
// PassManager
/* ===--------------------------------------------------------------------=== */

void PassManager::Reset() {
   lastRun_ = nullptr;
   for(auto& pass : passes_) {
      pass->state = Pass::State::Initialized;
      pass->enabled = false;
      pass->allocs_.clear();
   }
   for(auto& heap : heaps_) heap.refcount = 0;
   state_ = State::Initialized;
}

void PassManager::Init() {
   assert(state_ == State::Uninitialized && "PassManager already initialized");
   std::vector<Pass*> S;
   std::unordered_map<Pass*, unsigned> passDeps;
   std::unordered_set<Pass*> visited;
   S.reserve(passes_.size());
   // 1a. Build the adjacency list of passes
   depGraph_.clear();
   for(auto& pass : passes_) {
      pass->ComputeDependencies();
      pass->Init();
      pass->state = Pass::State::Initialized;
   }
   // 1b. Perform several init steps per pass here
   for(auto& pass : passes_) {
      // For topological sorting
      passDeps[pass.get()] = depGraph_[pass.get()].forward.size();
      if(passDeps[pass.get()] == 0) S.push_back(pass.get());
      // Find the right dispatcher for this pass
      pass->dispatcher = &DefaultDispatcherInstance;
      for(auto& dispatcher : dispatchers_) {
         if(dispatcher->CanDispatch(*pass)) {
            pass->dispatcher = dispatcher.get();
            break;
         }
      }
   }
   // 2. Run a topological sort on the dependency graph
   unsigned PassesAdded = 0;
   while(!S.empty()) {
      auto* n = *S.begin();
      S.erase(S.begin());
      n->topoIdx = PassesAdded++;
      if(!visited.contains(n)) {
         for(auto* m : depGraph_[n].transpose)
            if(--passDeps[m] == 0) S.push_back(m);
         visited.insert(n);
      }
   }
   // 3. Check for cycles
   if(PassesAdded++ != passes_.size())
      throw FatalError("Cyclic pass dependency detected");
   // 4. Sort the passes by topological order
   std::sort(passes_.begin(),
             passes_.end(),
             [](const std::unique_ptr<Pass>& a, const std::unique_ptr<Pass>& b) {
                return a->topoIdx < b->topoIdx;
             });
   // 6. Print the passes in topological order
   if(Diag().Verbose()) {
      Diag().ReportDebug() << "Passes sorted by topological order:";
      for(auto& pass : passes_) {
         Diag().ReportDebug()
               << "  " << pass->topoIdx << ": " << pass->Name() << " ("
               << pass->Desc() << ") Dispatcher: " << pass->dispatcher->Name();
      }
   }
   state_ = State::Initialized;
   assert((validate(), true));
}

bool PassManager::Run() {
   assert(state_ == State::Initialized && "PassManager not initialized");
   state_ = State::Running;

   // Propagate enabled state
   {
      std::queue<Pass*> Q;
      for(auto& pass : passes_)
         if(pass->enabled) Q.push(pass.get());
      while(!Q.empty()) {
         auto* pass = Q.front();
         Q.pop();
         for(auto* dep : depGraph_[pass].forward) {
            dep->enabled = true;
            Q.push(dep);
         }
      }
   }

   // Chunkify the passes by their dispatcher. Store the chunk
   // indices in passChunks_
   {
      passChunks_.clear();
      PassDispatcher* curDispatcher = nullptr;
      unsigned left = 0;
      while(true) {
         // First, move the left pointer to find an enabled pass
         while(left < passes_.size() && !passes_[left]->enabled) left++;
         curDispatcher = passes_[left]->dispatcher;
         // Then, move the right pointer to find the end of the chunk,
         // only considering enabled passes with the same dispatcher and
         // skipping disabled passes
         unsigned right = left;
         while(right < passes_.size() &&
               ((passes_[right]->dispatcher == curDispatcher &&
                 passes_[right]->enabled) ||
                !passes_[right]->enabled))
            right++;
         // Now we have a chunk, so store it
         passChunks_.emplace_back(left, right - 1, curDispatcher);
         left = right;
         if(right == passes_.size()) break;
      }
      if(Diag().Verbose()) {
         Diag().ReportDebug()
               << "Passes chunked into " << passChunks_.size() << " chunks";
         for(auto [left, right, dispatcher] : passChunks_) {
            Diag().ReportDebug() << "  [" << left << ", " << right
                                 << "] dispatched by: " << dispatcher->Name();
         }
      }
   }

   // Run the passes
   for(auto [left, right, dispatcher] : passChunks_) {
      auto leftIt = passes_.begin() + left;
      auto rightIt = passes_.begin() + right;
      auto iterated = dispatcher->Iterate(*this);
      auto dIt = iterated.begin();
      if(Diag().Verbose()) {
         Diag().ReportDebug() << "Running chunk [" << left << ", " << right
                              << "] dispatched by: " << dispatcher->Name();
      }
      /**
       * Take care to handle the INTER-chunk resource dependencies.
       * The INTRA-chunk dependencies are handled by runPassLifeCycle.
       *
       * At the beginning of iterating this chunk, we assume (inductively)
       * that the current chunk has acquired all resources it needs.
       */
      while(dIt != iterated.end()) {
         ++dIt;
         /**
          * In the last iteration over this chunk, we should:
          * 1) Release resources the current chunk has acquired
          * 2) Allow the next chunk(s) to acquire resources in the current chunk
          */
         for(auto it = leftIt; it != rightIt; it++) {
            auto& pass = *it->get();
            if(!pass.enabled) continue;
            runPassLifeCycle(pass, left, right, dIt == iterated.end());
            if(Diag().hasErrors()) return false;
         }
         for(auto it = leftIt; it != rightIt; it++)
            it->get()->state = Pass::State::Initialized;
      }
   }
   state_ = State::Cleanup;
   return true;
}

PassManager::~PassManager() {
   // Make sure we free the passes BEFORE we free the heaps because the
   // allocs_ array holds on to the heap for just a bit longer.
   passes_.clear();
   heaps_.clear();
}

} // namespace utils
