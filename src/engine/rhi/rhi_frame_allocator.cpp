#include "engine/rhi/rhi_frame_allocator.hpp"

#include <algorithm>
#include <cstring>

#include <spdlog/spdlog.h>

namespace badlands::rhi {
namespace {

bool IsPowerOfTwo(uint64_t v) { return v != 0 && (v & (v - 1)) == 0; }

// Subtraction, never `(v + a - 1) & ~(a - 1)` on an unchecked v: the add can
// wrap and produce an offset *below* where it started. Callers pass sizes that
// come from user data.
std::optional<uint64_t> AlignUp(uint64_t v, uint64_t a) {
  const uint64_t rem = v & (a - 1);
  if (rem == 0) return v;
  const uint64_t pad = a - rem;
  if (v > UINT64_MAX - pad) return std::nullopt;
  return v + pad;
}

}  // namespace

std::unique_ptr<FrameAllocator> FrameAllocator::Create(
    IRhiDevice& device, const FrameAllocatorDesc& desc) {
  if (desc.block_size == 0) {
    spdlog::error("rhi: frame allocator '{}' needs a non-zero block_size",
                  desc.label);
    return nullptr;
  }
  if (desc.max_bytes_per_frame < desc.block_size) {
    spdlog::error(
        "rhi: frame allocator '{}' has max_bytes_per_frame {} below its "
        "block_size {} -- the first block would already exceed the cap",
        desc.label, desc.max_bytes_per_frame, desc.block_size);
    return nullptr;
  }

  const uint32_t slot_count = std::max(1u, device.FramesInFlight());
  std::vector<Slot> slots(slot_count);

  auto allocator = std::unique_ptr<FrameAllocator>(
      new FrameAllocator(device, desc, std::move(slots)));

  // One block per slot up front, so the steady state never allocates.
  for (size_t i = 0; i < slot_count; ++i) {
    auto block = allocator->MakeBlock(desc.block_size, i, 0);
    if (!block) return nullptr;  // MakeBlock logged why
    allocator->slots_[i].blocks.push_back(std::move(block));
  }
  return allocator;
}

FrameAllocator::FrameAllocator(IRhiDevice& device,
                               const FrameAllocatorDesc& desc,
                               std::vector<Slot> slots)
    : device_(device), desc_(desc), slots_(std::move(slots)) {
  min_alignment_ = device_.MinBufferOffsetAlignment();
  if (!IsPowerOfTwo(min_alignment_)) {
    spdlog::error(
        "rhi: backend reported a non-power-of-two buffer offset alignment "
        "({}); falling back to 256",
        min_alignment_);
    min_alignment_ = 256;
  }
}

BufferPtr FrameAllocator::MakeBlock(uint64_t size, size_t slot, size_t index) {
  auto buf = device_.CreateBuffer(
      {.size = size,
       .usage = desc_.usage,
       .label = desc_.label + ".slot" + std::to_string(slot) + "." +
                std::to_string(index)});
  if (!buf) {
    spdlog::error("rhi: frame allocator '{}' could not create a {}-byte block",
                  desc_.label, size);
  }
  return buf;
}

void FrameAllocator::BeginFrame(uint64_t frame) {
  current_slot_ = size_t(frame % slots_.size());
  Slot& s = slots_[current_slot_];
  // Safe without any check: IRhiDevice::BeginFrame blocked until the frame
  // that previously owned this slot retired, so nothing on the GPU can still
  // be reading these bytes.
  s.block_index = 0;
  s.cursor = 0;
  s.bytes_used = 0;
}

std::optional<FrameAlloc> FrameAllocator::Allocate(uint64_t size,
                                                   uint64_t alignment) {
  if (size == 0) {
    // A caller wanting nothing can allocate nothing, and a zero-length binding
    // means nothing to any backend. Refusing is clearer than handing back a
    // valid-looking slice of no bytes.
    spdlog::error("rhi: frame allocator '{}' asked for a zero-byte allocation",
                  desc_.label);
    return std::nullopt;
  }
  const uint64_t align = alignment ? alignment : min_alignment_;
  if (!IsPowerOfTwo(align)) {
    spdlog::error(
        "rhi: frame allocator '{}' asked for alignment {}, which is not a "
        "power of two",
        desc_.label, align);
    return std::nullopt;
  }
  if (align < min_alignment_) {
    spdlog::error(
        "rhi: frame allocator '{}' asked for alignment {}, below the backend's "
        "minimum of {} -- the binding would be rejected at draw time",
        desc_.label, align, min_alignment_);
    return std::nullopt;
  }

  Slot& s = slots_[current_slot_];
  const auto aligned = AlignUp(s.cursor, align);
  if (!aligned) {
    spdlog::error("rhi: frame allocator '{}' overflowed aligning offset {}",
                  desc_.label, s.cursor);
    return std::nullopt;
  }

  uint64_t offset = *aligned;
  const uint64_t block_capacity = s.blocks[s.block_index]->GetSize();

  // Does it fit in the block being bumped?
  if (size > block_capacity || offset > block_capacity - size) {
    // Grow. The cap is what separates "the ring is sized a little small",
    // which is a warning, from "something is leaking per frame", which is not.
    const uint64_t needed = std::max(size, desc_.block_size);
    // `max - needed` underflows when a single request is larger than the whole
    // cap, and the wrapped compare then PASSES -- the exact hazard rule 8
    // exists for, reproduced here in new code. The first clause is what stops
    // the subtraction from wrapping.
    if (needed > desc_.max_bytes_per_frame ||
        s.bytes_used > desc_.max_bytes_per_frame - needed) {
      spdlog::error(
          "rhi: frame allocator '{}' refused {} bytes: this frame has already "
          "used {} of its {}-byte cap",
          desc_.label, size, s.bytes_used, desc_.max_bytes_per_frame);
      return std::nullopt;
    }

    // The index advances only once a usable block EXISTS. Incrementing first
    // and then failing to create the block left block_index one past the end,
    // and the next Allocate in the same frame -- the contract says a refused
    // allocation skips one draw and carries on -- indexed the vector out of
    // bounds and called a virtual through the garbage it read.
    const size_t next_index = s.block_index + 1;
    if (next_index >= s.blocks.size()) {
      auto block = MakeBlock(needed, current_slot_, next_index);
      if (!block) return std::nullopt;
      s.blocks.push_back(std::move(block));
      // Warn once: repeating it every frame would bury the signal it is.
      if (!warned_about_growth_) {
        warned_about_growth_ = true;
        spdlog::warn(
            "rhi: frame allocator '{}' grew past its {}-byte block for a {}-"
            "byte request -- the ring is undersized for this workload",
            desc_.label, desc_.block_size, size);
      }
    } else if (s.blocks[next_index]->GetSize() < size) {
      // A recycled extra block that is too small for this request.
      auto block = MakeBlock(needed, current_slot_, next_index);
      if (!block) return std::nullopt;
      s.blocks[next_index] = std::move(block);
    }
    s.block_index = next_index;
    offset = 0;
  }

  s.cursor = offset + size;
  s.bytes_used += size;
  return FrameAlloc{.buffer = s.blocks[s.block_index].get(),
                    .offset = offset,
                    .size = size};
}

std::optional<FrameAlloc> FrameAllocator::Write(std::span<const uint8_t> data,
                                                uint64_t alignment) {
  auto alloc = Allocate(data.size(), alignment);
  if (!alloc) return std::nullopt;  // Allocate logged why
  alloc->buffer->Write(alloc->offset, data);
  return alloc;
}

uint64_t FrameAllocator::BytesUsedThisFrame() const {
  return slots_[current_slot_].bytes_used;
}

size_t FrameAllocator::BlocksThisFrame() const {
  return slots_[current_slot_].block_index + 1;
}

}  // namespace badlands::rhi
