#include "module_pins.hpp"

#include <algorithm>

#if defined(__SANITIZE_ADDRESS__)
#define VIEWER_ASAN 1
#elif defined(__has_feature)
#if __has_feature(address_sanitizer)
#define VIEWER_ASAN 1
#endif
#endif

#if VIEWER_ASAN && defined(__linux__)
#include <dlfcn.h>
#include <link.h>
#endif

namespace viewer {

namespace {

std::vector<std::string> loaded_objects() {
    std::vector<std::string> out;
#if VIEWER_ASAN && defined(__linux__)
    dl_iterate_phdr(
        [](dl_phdr_info* info, std::size_t, void* data) {
            if (info->dlpi_name != nullptr && info->dlpi_name[0] != '\0') {
                static_cast<std::vector<std::string>*>(data)->emplace_back(info->dlpi_name);
            }
            return 0;
        },
        &out);
    std::sort(out.begin(), out.end());
#endif
    return out;
}

} // namespace

ModulePins::ModulePins() : baseline_(loaded_objects()) {}

void ModulePins::pin_new() {
#if VIEWER_ASAN && defined(__linux__)
    for (const std::string& name : loaded_objects()) {
        if (std::binary_search(baseline_.begin(), baseline_.end(), name) ||
            std::find(pinned_.begin(), pinned_.end(), name) != pinned_.end()) {
            continue;
        }
        // RTLD_NOLOAD: only touch objects already mapped; RTLD_NODELETE: never unmap them. The
        // extra reference is deliberately never released.
        if (dlopen(name.c_str(), RTLD_NOW | RTLD_NOLOAD | RTLD_NODELETE) != nullptr) {
            pinned_.push_back(name);
        }
    }
#endif
}

} // namespace viewer
