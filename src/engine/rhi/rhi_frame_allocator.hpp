#pragma once

// Transient per-frame allocation: a bump pointer over one block per frame
// slot, recycled when that slot's frame retires.
//
// WHY THIS RATHER THAN A BUFFER PER USE. Per-frame data -- the frame uniform
// block, debug primitives, anything that changes every tick -- cannot simply be
// rewritten in place once frames overlap: the CPU would be writing frame N
// while the GPU is still reading frames N-1 and N-2 out of the same bytes.
// Creating a buffer per frame instead would fragment and add API overhead,
// which is exactly what the RHI plan's allocator marker warned about. A ring
// avoids both.
//
// WHY IT IS WRITTEN RATHER THAN VENDORED. OffsetAllocator is the right library
// for a general GPU heap and is the recorded choice for when bindless needs
// one. It is the wrong tool here: a bump pointer reset on retirement is a few
// dozen lines, and every general-purpose GPU allocator (VMA, D3D12MA) couples
// its linear-allocation mode to its own API. Importing one would be more code,
// not less.
//
// RECYCLING IS SAFE BY CONSTRUCTION, not by checking: a slot is reset in
// BeginFrame, and IRhiDevice::BeginFrame has already blocked until the frame
// that previously owned that slot retired.
//
// ONE BUFFER, PARTITIONED BY SLOT -- not one buffer per slot. Binding tables
// are immutable and hold a BUFFER, so a table built once can only follow the
// frame if the buffer stays the same and just the offset moves. An allocator
// handing out a different buffer each frame cannot back a per-frame binding at
// all, which is the whole reason it exists. Growth blocks past the primary are
// separate buffers and cannot be bound into an already-built table; that is
// the exceptional path, and the warning says the ring is undersized.

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "engine/rhi/rhi_device.hpp"
#include "engine/rhi/rhi_resources.hpp"

namespace badlands::rhi {

// A slice of a transient buffer, valid for the CURRENT frame only. Bind it
// with a dynamic offset; do not store it across frames.
struct FrameAlloc {
  IBuffer* buffer = nullptr;
  uint64_t offset = 0;
  uint64_t size = 0;

  explicit operator bool() const { return buffer != nullptr; }
};

struct FrameAllocatorDesc {
  // Bytes per frame slot before the allocator has to grow.
  uint64_t block_size = 256 * 1024;
  // Hard ceiling per frame. Growth past `block_size` warns; growth past this
  // refuses, because an unbounded ring hides a leak rather than reporting one.
  uint64_t max_bytes_per_frame = 16 * 1024 * 1024;
  BufferUsage usage = BufferUsage::Uniform | BufferUsage::Storage;
  std::string label = "frame_alloc";
};

class FrameAllocator {
 public:
  // Returns null (after logging) if the blocks cannot be created.
  static std::unique_ptr<FrameAllocator> Create(IRhiDevice& device,
                                                const FrameAllocatorDesc& desc);

  // Recycles the slot `frame` maps to. Call once per frame, after
  // IRhiDevice::BeginFrame -- which is what makes the recycle safe.
  void BeginFrame(uint64_t frame);

  // A slice of at least `size` bytes, aligned to `alignment` (0 means the
  // device minimum). Returns nullopt, after logging, if the request cannot be
  // met -- callers must not treat a failed allocation as an empty one.
  std::optional<FrameAlloc> Allocate(uint64_t size, uint64_t alignment = 0);

  // Convenience: allocate and copy in one step.
  std::optional<FrameAlloc> Write(std::span<const uint8_t> data,
                                  uint64_t alignment = 0);

  // The buffer every ordinary allocation comes from. Stable for the life of
  // the allocator, so a binding table can name it once.
  IBuffer* PrimaryBuffer() const;
  uint64_t BytesUsedThisFrame() const;
  // Blocks backing the current frame. More than one means the ring grew, which
  // is a sizing signal rather than an error.
  size_t BlocksThisFrame() const;

 private:
  struct Slot {
    // blocks[0] is always the shared primary; later entries are this slot's
    // own growth buffers.
    std::vector<BufferPtr> blocks;
    uint64_t base_offset = 0;  // this slot's window into the primary
    size_t block_index = 0;    // block currently being bumped
    uint64_t cursor = 0;       // offset within the current block's window
    uint64_t bytes_used = 0;   // across every block this frame
  };

  FrameAllocator(IRhiDevice& device, const FrameAllocatorDesc& desc,
                 std::vector<Slot> slots);

  BufferPtr MakeBlock(uint64_t size, size_t slot, size_t index);

  IRhiDevice& device_;
  FrameAllocatorDesc desc_;
  uint64_t min_alignment_ = 256;
  std::vector<Slot> slots_;
  size_t current_slot_ = 0;
  bool warned_about_growth_ = false;
};

}  // namespace badlands::rhi
