#pragma once

#include <string>
#include <vector>

namespace viewer {

// Keeps GPU driver libraries loaded until exit in AddressSanitizer builds.
//
// The Vulkan loader dlopen()s the installable client driver (Mesa radv/anv/lavapipe, NVIDIA...)
// and dlclose()s it at shutdown. Drivers keep process-lifetime allocations reachable only from
// their own globals; once the library is unmapped, LeakSanitizer (which runs at exit) sees those
// blocks as unreachable and reports them as leaks from "<unknown module>". Pinning the libraries
// loaded while the viewer starts (RTLD_NODELETE) keeps them mapped, so the report stays empty
// without switching leak detection off for our own code. A no-op in non-ASan builds.
class ModulePins {
public:
    ModulePins(); // records the shared objects loaded now
    void pin_new(); // pins every shared object loaded since construction (idempotent)

private:
    std::vector<std::string> baseline_;
    std::vector<std::string> pinned_;
};

} // namespace viewer
