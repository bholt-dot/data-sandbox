#include "viewer/viewer_link.hpp"

#include <utility>

namespace viewer {

void ViewerLink::publish(std::shared_ptr<const ViewSnapshot> snapshot) {
    if (!snapshot) {
        return;
    }
    std::shared_ptr<const ViewSnapshot> previous;
    {
        const std::lock_guard lock(mutex_);
        previous = std::exchange(latest_, std::move(snapshot));
        ++generation_;
    }
    // `previous` may hold the last reference to a large World; free it outside the lock.
}

std::shared_ptr<const ViewSnapshot> ViewerLink::latest() const {
    const std::lock_guard lock(mutex_);
    return latest_;
}

std::uint64_t ViewerLink::generation() const {
    const std::lock_guard lock(mutex_);
    return generation_;
}

} // namespace viewer
