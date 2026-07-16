// Hierarchical CPU scope profiler — see profiler.hpp. Ported from sampo's
// src/core/profiler.cpp (SAMPO_PROFILING -> BADLANDS_PROFILING).
#ifdef BADLANDS_PROFILING

#include "core/profiler.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <mutex>
#include <string>
#include <vector>

namespace profiler {

// --- Section registry (global, thread-safe) ---

struct SectionRegistry {
  std::mutex mutex;
  std::vector<std::string> names;

  uint32_t Register(const char* name) {
    std::lock_guard<std::mutex> lock(mutex);
    uint32_t id = static_cast<uint32_t>(names.size());
    names.emplace_back(name);
    return id;
  }

  const std::string& Name(uint32_t id) {
    std::lock_guard<std::mutex> lock(mutex);
    return names[id];
  }
};

static SectionRegistry& GetRegistry() {
  static SectionRegistry registry;
  return registry;
}

uint32_t RegisterSection(const char* name) {
  return GetRegistry().Register(name);
}

// --- Ring buffer (thread-local) ---

static constexpr uint32_t kFlagExit = 1;
static constexpr size_t kDefaultBufferSize = 65536;
static constexpr size_t kMaxBufferSize = 1048576;

struct ProfileEvent {
  uint32_t section_id;
  uint32_t flags;
  uint64_t timestamp_ns;
};

// --- Aggregated tree node ---

struct TreeNode {
  uint32_t section_id = 0;
  uint64_t call_count = 0;
  uint64_t total_ns = 0;
  uint64_t max_ns = 0;
  uint64_t self_ns = 0;
  std::vector<TreeNode> children;

  TreeNode* FindOrAddChild(uint32_t id) {
    for (auto& c : children) {
      if (c.section_id == id) return &c;
    }
    children.push_back({});
    children.back().section_id = id;
    return &children.back();
  }
};

struct ThreadBuffer {
  std::vector<ProfileEvent> events;
  size_t write_pos = 0;
  size_t capacity = kDefaultBufferSize;
  size_t total_events = 0;  // cumulative across flushes
  TreeNode root;            // persistent aggregated tree across flushes

  ThreadBuffer() { events.resize(capacity); }

  void RecordEvent(uint32_t id, uint32_t flags) {
    if (write_pos >= capacity) {
      FlushToTree();
    }
    auto now = std::chrono::steady_clock::now();
    uint64_t ts = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            now.time_since_epoch())
            .count());
    events[write_pos] = {id, flags, ts};
    ++write_pos;
  }

  void FlushToTree() {
    ReplayIntoTree(events.data(), write_pos, root);
    total_events += write_pos;
    write_pos = 0;

    // Grow buffer for next cycle (up to max)
    if (capacity < kMaxBufferSize) {
      capacity = std::min(capacity * 2, kMaxBufferSize);
      events.resize(capacity);
    }
  }

  static void ReplayIntoTree(const ProfileEvent* evts, size_t count,
                             TreeNode& root) {
    struct StackEntry {
      TreeNode* node;
      uint64_t enter_ts;
    };
    std::vector<StackEntry> stack;
    stack.push_back({&root, 0});

    for (size_t i = 0; i < count; ++i) {
      const auto& e = evts[i];
      if (!(e.flags & kFlagExit)) {
        // Enter
        TreeNode* parent = stack.back().node;
        TreeNode* child = parent->FindOrAddChild(e.section_id);
        stack.push_back({child, e.timestamp_ns});
      } else {
        // Exit — pop stack
        if (stack.size() <= 1) continue;  // orphaned exit, skip
        auto& top = stack.back();
        uint64_t elapsed = e.timestamp_ns - top.enter_ts;
        top.node->call_count++;
        top.node->total_ns += elapsed;
        top.node->max_ns = std::max(top.node->max_ns, elapsed);
        stack.pop_back();
      }
    }
  }

  void ComputeSelfTime(TreeNode& node) {
    uint64_t children_total = 0;
    for (auto& c : node.children) {
      ComputeSelfTime(c);
      children_total += c.total_ns;
    }
    node.self_ns = (node.total_ns > children_total)
                       ? (node.total_ns - children_total)
                       : 0;
  }
};

// --- Thread registry ---

static std::mutex& GetThreadsMutex() {
  static std::mutex m;
  return m;
}

static std::vector<ThreadBuffer*>& GetThreadBuffers() {
  static std::vector<ThreadBuffer*> buffers;
  return buffers;
}

static size_t g_initial_buffer_size = kDefaultBufferSize;

static ThreadBuffer& GetThreadBuffer() {
  thread_local ThreadBuffer* buffer = nullptr;
  if (!buffer) {
    buffer = new ThreadBuffer();
    buffer->capacity = g_initial_buffer_size;
    buffer->events.resize(buffer->capacity);
    std::lock_guard<std::mutex> lock(GetThreadsMutex());
    GetThreadBuffers().push_back(buffer);
  }
  return *buffer;
}

// --- ScopeGuard ---

ScopeGuard::ScopeGuard(uint32_t section_id) : id(section_id) {
  GetThreadBuffer().RecordEvent(id, 0);
}

ScopeGuard::~ScopeGuard() { GetThreadBuffer().RecordEvent(id, kFlagExit); }

// --- Merge trees ---

static void MergeTree(TreeNode& dst, const TreeNode& src) {
  dst.call_count += src.call_count;
  dst.total_ns += src.total_ns;
  dst.max_ns = std::max(dst.max_ns, src.max_ns);
  for (const auto& sc : src.children) {
    TreeNode* dc = dst.FindOrAddChild(sc.section_id);
    MergeTree(*dc, sc);
  }
}

// --- Output formatting ---

static double NsToMs(uint64_t ns) { return static_cast<double>(ns) / 1e6; }

static void PrintNode(std::ostream& out, const TreeNode& node,
                      uint64_t parent_total_ns, const std::string& prefix,
                      bool is_last, SectionRegistry& reg) {
  std::string connector = is_last ? "\xe2\x94\x94\xe2\x94\x80 "    // └─
                                  : "\xe2\x94\x9c\xe2\x94\x80 ";   // ├─
  std::string child_prefix =
      prefix + (is_last ? "   " : "\xe2\x94\x82  ");                // │

  double pct =
      (parent_total_ns > 0)
          ? (100.0 * static_cast<double>(node.total_ns) /
             static_cast<double>(parent_total_ns))
          : 0.0;
  double avg_ms =
      (node.call_count > 0) ? NsToMs(node.total_ns) / node.call_count : 0.0;

  out << prefix << connector;
  out << reg.Name(node.section_id);
  out << " [" << std::fixed;
  out.precision(1);
  out << pct << "%]: ";
  out << node.call_count << " calls, ";
  out << NsToMs(node.total_ns) << "ms, ";
  out << avg_ms << "ms avg, ";
  out << NsToMs(node.max_ns) << "ms max";

  if (!node.children.empty() && node.total_ns > 0) {
    double self_pct = 100.0 * static_cast<double>(node.self_ns) /
                      static_cast<double>(node.total_ns);
    out << " (self: " << self_pct << "%)";
  }
  out << "\n";

  // Sort children by total_ns descending (work on a copy of indices)
  std::vector<size_t> order(node.children.size());
  for (size_t i = 0; i < order.size(); ++i) order[i] = i;
  std::sort(order.begin(), order.end(), [&](size_t a, size_t b) {
    return node.children[a].total_ns > node.children[b].total_ns;
  });

  for (size_t i = 0; i < order.size(); ++i) {
    PrintNode(out, node.children[order[i]], node.total_ns, child_prefix,
              i == order.size() - 1, reg);
  }
}

static void PrintTree(std::ostream& out, const TreeNode& root,
                      size_t thread_count, size_t total_events,
                      SectionRegistry& reg) {
  out << "=== Profile (" << thread_count << " thread"
      << (thread_count != 1 ? "s" : "") << ", " << total_events
      << " events) ===\n";

  // Sort top-level children by total_ns descending
  std::vector<size_t> order(root.children.size());
  for (size_t i = 0; i < order.size(); ++i) order[i] = i;
  std::sort(order.begin(), order.end(), [&](size_t a, size_t b) {
    return root.children[a].total_ns > root.children[b].total_ns;
  });

  for (size_t i = 0; i < order.size(); ++i) {
    const auto& child = root.children[order[i]];
    double avg_ms = (child.call_count > 0)
                        ? NsToMs(child.total_ns) / child.call_count
                        : 0.0;

    out << std::fixed;
    out.precision(1);
    out << reg.Name(child.section_id) << ": " << child.call_count << " calls, "
        << NsToMs(child.total_ns) << "ms total, " << avg_ms << "ms avg, "
        << NsToMs(child.max_ns) << "ms max\n";

    // Sort this node's children
    std::vector<size_t> child_order(child.children.size());
    for (size_t j = 0; j < child_order.size(); ++j) child_order[j] = j;
    std::sort(child_order.begin(), child_order.end(), [&](size_t a, size_t b) {
      return child.children[a].total_ns > child.children[b].total_ns;
    });

    for (size_t j = 0; j < child_order.size(); ++j) {
      PrintNode(out, child.children[child_order[j]], child.total_ns, "",
                j == child_order.size() - 1, reg);
    }
  }
}

// --- Public API ---

void Report(std::ostream& out) {
  std::lock_guard<std::mutex> lock(GetThreadsMutex());
  auto& buffers = GetThreadBuffers();

  TreeNode merged;
  size_t total_events = 0;

  for (auto* buf : buffers) {
    // Flush remaining events
    buf->FlushToTree();
    buf->ComputeSelfTime(buf->root);
    total_events += buf->total_events;
    MergeTree(merged, buf->root);
  }

  // Compute self time on merged tree
  ThreadBuffer dummy;
  dummy.ComputeSelfTime(merged);

  PrintTree(out, merged, buffers.size(), total_events, GetRegistry());

  // Reset all buffers
  for (auto* buf : buffers) {
    buf->root = TreeNode{};
    buf->write_pos = 0;
    buf->total_events = 0;
  }
}

void ReportToStderr() { Report(std::cerr); }

void Reset() {
  std::lock_guard<std::mutex> lock(GetThreadsMutex());
  for (auto* buf : GetThreadBuffers()) {
    buf->root = TreeNode{};
    buf->write_pos = 0;
  }
}

void SetBufferSize(size_t events_per_thread) {
  // Must be power of 2 (round up)
  size_t s = 1;
  while (s < events_per_thread) s <<= 1;
  g_initial_buffer_size = std::min(s, kMaxBufferSize);
}

}  // namespace profiler

#endif  // BADLANDS_PROFILING
