#pragma once
#include <cstdint>

#if __has_include(<swift/bridging>)
#include <swift/bridging>
#else
#define SWIFT_IMMORTAL_REFERENCE
#endif

namespace sq {

// One app-lifetime instance; Swift imports this as a reference type.
class SWIFT_IMMORTAL_REFERENCE Editor {
public:
    static Editor* create();
    int32_t ping() const;   // M0 interop smoke test; replaced by real API in later milestones
};

} // namespace sq
