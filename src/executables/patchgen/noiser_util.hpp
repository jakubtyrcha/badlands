#pragma once

#include <noiser.hpp>

#include <pthread.h>

#include <expected>
#include <memory>
#include <optional>
#include <string>

namespace badlands::mapgen {

// Compile a noiser source string on a dedicated big-stack thread.
//
// The noiser compiler recurses deeply and assumes a ~64 MiB stack; an ordinary
// thread stack overflows it. This mirrors game/src/brain.cpp::compile_big_stack.
inline std::expected<std::shared_ptr<sampo::noiser::NoiserProgram>,
                     sampo::noiser::CompileError>
compile_big_stack(const std::string& source) {
  using sampo::noiser::CompileError;
  using sampo::noiser::NoiserProgram;
  using Result = std::expected<std::shared_ptr<NoiserProgram>, CompileError>;

  struct Job {
    const std::string* source;
    std::optional<Result> result;
  } job{&source, std::nullopt};

  pthread_attr_t attr;
  pthread_attr_init(&attr);
  pthread_attr_setstacksize(&attr, 64u << 20);
  pthread_t thread;
  auto entry = [](void* raw) -> void* {
    auto* j = static_cast<Job*>(raw);
    j->result = NoiserProgram::Compile(*j->source);
    return nullptr;
  };
  if (pthread_create(&thread, &attr, entry, &job) != 0) {
    pthread_attr_destroy(&attr);
    return std::unexpected(
        CompileError{.message = "failed to spawn compile thread"});
  }
  pthread_join(thread, nullptr);
  pthread_attr_destroy(&attr);
  return std::move(*job.result);
}

// Format a noiser CompileError for logging (message + source location).
inline std::string format_compile_error(
    const sampo::noiser::CompileError& err) {
  std::string out = err.message;
  if (err.line > 0) {
    out += " at " + (err.module.empty() ? std::string("main") : err.module) +
           ":" + std::to_string(err.line) + ":" + std::to_string(err.column);
  }
  return out;
}

}  // namespace badlands::mapgen
