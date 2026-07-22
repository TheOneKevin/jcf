#include "utils/BumpAllocator.h"

#include <memory>

#include <utils/Assert.h>

namespace utils {

CustomBufferResource::CustomBufferResource(size_t size) {
   buffers_.emplace_back(size, std::malloc(size));
   cur_buf_ = buffers_.begin();
   alloc_top_ = cur_buf_->buf;
   avail_ = size;
}

void* CustomBufferResource::do_allocate(std::size_t bytes, std::size_t alignment) {
   assert((!invalid) &&
          "attempt to allocate when CustomBufferResource is invalid");

   // Do not return the same pointer twice, so set min size to 1
   if(bytes == 0) bytes = 1;

   // Align the next allocation
   void* p = std::align(alignment, bytes, alloc_top_, avail_);

   // Do we have enough space in the current buffer?
   if(!p) {
      // Is there no next buffer? Allocate a new one
      if(std::next(cur_buf_) == buffers_.end()) {
         size_t new_size = cur_buf_->size * growth_factor;
         buffers_.emplace_back(new_size, std::malloc(new_size));
         cur_buf_ = std::prev(buffers_.end());
      } else {
         ++cur_buf_;
      }
      alloc_top_ = cur_buf_->buf;
      avail_ = cur_buf_->size;
      p = cur_buf_->buf;
   }

   // Update the allocation pointer and the available space
   alloc_top_ = static_cast<char*>(alloc_top_) + bytes;
   avail_ -= bytes;

   // Return the aligned pointer
   return p;
}

void CustomBufferResource::clear_all_buffers() {
   // memset the memory to 0 to catch use-after-free bugs
   for(auto& buf : buffers_) {
      std::memset(buf.buf, 0, buf.size);
   }
}

CustomBufferResource::~CustomBufferResource() {
   for(auto& buf : buffers_) {
      std::free(buf.buf);
   }
}

} // namespace utils
