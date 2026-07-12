// Link stubs for the noiser JIT (Cranelift) C ABI.
//
// noiser.cpp references every noiser_jit_* symbol unconditionally (no #ifdef
// guard), but the JIT implementation lives in the noiser-wasm crate, which
// drags in Binaryen. Badlands only ever uses NoiserBackend::kVM, so these
// stubs satisfy the linker and are never called at runtime.

#include "noiser_jit_ffi.h"

extern "C" {

NoiserJitProgram* noiser_jit_compile(const char*, char** out_error) {
  if (out_error) *out_error = nullptr;
  return nullptr;
}

NoiserJitProgram* noiser_jit_compile_with_modules(const char*, const char*,
                                                  NoiserJitResolverFn, void*,
                                                  char** out_error) {
  if (out_error) *out_error = nullptr;
  return nullptr;
}

void noiser_jit_free_program(NoiserJitProgram*) {}

bool noiser_jit_is_generator(const NoiserJitProgram*) { return false; }

void noiser_jit_bind_host_thunk(NoiserJitProgram*, uint16_t,
                                NoiserJitHostThunkFn, void*, uint8_t,
                                uint8_t) {}

void noiser_jit_set_uniform_f32(NoiserJitProgram*, uint16_t, float) {}
void noiser_jit_set_uniform_i32(NoiserJitProgram*, uint16_t, int32_t) {}
void noiser_jit_set_uniform_vec2(NoiserJitProgram*, uint16_t, float, float) {}
void noiser_jit_set_uniform_vec3(NoiserJitProgram*, uint16_t, float, float,
                                 float) {}
void noiser_jit_set_uniform_vec4(NoiserJitProgram*, uint16_t, float, float,
                                 float, float) {}

NoiserJitStatus noiser_jit_execute(const NoiserJitProgram*, int32_t, int32_t,
                                   int32_t, int32_t, int32_t, int32_t,
                                   uint32_t*, uint32_t* out_result_len,
                                   char** out_error) {
  if (out_result_len) *out_result_len = 0;
  if (out_error) *out_error = nullptr;
  return kNoiserJitError;
}

bool noiser_jit_execute_fast_f32(const NoiserJitProgram*, int32_t, int32_t,
                                 int32_t, int32_t, int32_t, int32_t, float*) {
  return false;
}

NoiserJitGenState* noiser_jit_prepare_generator(NoiserJitProgram*, int32_t,
                                                int32_t, int32_t, int32_t,
                                                int32_t, int32_t) {
  return nullptr;
}

NoiserJitStatus noiser_jit_resume_generator(NoiserJitGenState*, uint32_t*,
                                            uint32_t* out_result_len,
                                            char** out_error) {
  if (out_result_len) *out_result_len = 0;
  if (out_error) *out_error = nullptr;
  return kNoiserJitError;
}

void noiser_jit_free_generator_state(NoiserJitGenState*) {}

void noiser_jit_reset_generator(NoiserJitGenState*, int32_t, int32_t, int32_t,
                                int32_t, int32_t, int32_t) {}

uint32_t noiser_jit_get_host_function_count(const NoiserJitProgram*) {
  return 0;
}

bool noiser_jit_get_host_function_info(const NoiserJitProgram*, uint32_t,
                                       NoiserJitHostFnInfo*) {
  return false;
}

uint32_t noiser_jit_get_uniform_count(const NoiserJitProgram*) { return 0; }

bool noiser_jit_get_uniform_info(const NoiserJitProgram*, uint32_t,
                                 NoiserJitUniformInfo*) {
  return false;
}

bool noiser_jit_get_return_type(const NoiserJitProgram*, const uint8_t**,
                                uint32_t* out_len) {
  if (out_len) *out_len = 0;
  return false;
}

void noiser_jit_free_string(char*) {}

}  // extern "C"
