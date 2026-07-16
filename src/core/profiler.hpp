#pragma once

// Lightweight hierarchical CPU scope profiler (ported from sampo's
// src/core/profiler.{hpp,cpp}). RAII PROFILE_SCOPE("name") records enter/exit
// events into a thread-local ring buffer; Report() replays them into a call
// tree and prints per-section calls / total / avg / max ms + % + self-time.
//
// Entirely compiled out unless BADLANDS_PROFILING is defined (the
// BADLANDS_SCOPE_PROFILING CMake option, ON by default) -- PROFILE_SCOPE
// becomes a no-op, so markers can be left in hot paths at zero release cost.
#include <cstdint>

#ifdef BADLANDS_PROFILING

#include <cstddef>
#include <iosfwd>

namespace profiler {

uint32_t RegisterSection(const char* name);

struct ScopeGuard {
  uint32_t id;

  explicit ScopeGuard(uint32_t section_id);
  ~ScopeGuard();

  ScopeGuard(const ScopeGuard&) = delete;
  ScopeGuard& operator=(const ScopeGuard&) = delete;
};

void Report(std::ostream& out);
void ReportToStderr();
void Reset();
void SetBufferSize(size_t events_per_thread);

}  // namespace profiler

// NOLINTBEGIN(bugprone-macro-parentheses)
#define PROFILER_CONCAT_INNER(a, b) a##b
#define PROFILER_CONCAT(a, b) PROFILER_CONCAT_INNER(a, b)
#define PROFILE_SCOPE(name)                                              \
  static uint32_t PROFILER_CONCAT(profiler_id_, __LINE__) =              \
      profiler::RegisterSection(name);                                   \
  profiler::ScopeGuard PROFILER_CONCAT(profiler_guard_, __LINE__)(       \
      PROFILER_CONCAT(profiler_id_, __LINE__))
// NOLINTEND(bugprone-macro-parentheses)

#else  // BADLANDS_PROFILING not defined

#define PROFILE_SCOPE(name) ((void)0)

namespace profiler {
inline void ReportToStderr() {}
inline void Reset() {}
}  // namespace profiler

#endif  // BADLANDS_PROFILING
