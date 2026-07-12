//! Empty aggregation crate: pulls the noiser rlibs into a single staticlib so
//! their `#[no_mangle]` C FFI symbols (`noiser_vm_*`, `noiser_compile*`) are
//! available to the standalone C++ test binary.

pub use noiser_compiler as _compiler;
pub use noiser_vm as _vm;
