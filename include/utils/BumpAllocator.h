#pragma once

#include <cstring>
#include <memory_resource>
#include <vector>

using BumpAllocator = std::pmr::polymorphic_allocator<std::byte>;

namespace utils {

class CustomBufferResource : public std::pmr::memory_resource {
public:
   CustomBufferResource() : CustomBufferResource{128 * sizeof(void*)} {}
   CustomBufferResource(size_t size);

   void* do_allocate(std::size_t bytes, std::size_t alignment) override;

   inline void reset() {
      alloc_top_ = 0;
      cur_buf_ = buffers_.begin();
#ifdef DEBUG
      clear_all_buffers();
#endif
   }

   void destroy() {
      invalid = true;
      clear_all_buffers();
   }

   void clear_all_buffers();

   ~CustomBufferResource();

   void do_deallocate(void* p, std::size_t bytes, std::size_t alignment) override {
      // No-op
      (void)p;
      (void)bytes;
      (void)alignment;
   }

   bool do_is_equal(
         const std::pmr::memory_resource& other) const noexcept override {
      // No-op
      (void)other;
      return false;
   }

private:
   static constexpr float growth_factor = 1.5;
   struct Buffer {
      size_t size;
      void* buf;
   };
   void* alloc_top_;
   size_t avail_;
   std::vector<Buffer>::iterator cur_buf_;
   std::vector<Buffer> buffers_;
   bool invalid = false;
};

} // namespace utils
