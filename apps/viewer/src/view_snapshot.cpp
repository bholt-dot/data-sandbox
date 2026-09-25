#include "viewer/view_snapshot.hpp"

#include <type_traits>
#include <utility>

namespace viewer {

static_assert(std::is_copy_constructible_v<expanse::World>,
              "ViewSnapshot copies the World; keep its members value types");

std::shared_ptr<const ViewSnapshot> make_snapshot(std::shared_ptr<const expanse::Content> content,
                                                  const expanse::World& world, ViewHints hints) {
    auto snap = std::make_shared<ViewSnapshot>();
    snap->content = std::move(content);
    snap->world = world;
    snap->time = world.now();
    snap->hints = std::move(hints);
    return snap;
}

std::shared_ptr<const ViewSnapshot> make_snapshot(const expanse::Session& session, ViewHints hints) {
    if (session.world) {
        return make_snapshot(session.content, *session.world, std::move(hints));
    }
    auto snap = std::make_shared<ViewSnapshot>();
    snap->content = session.content;
    snap->hints = std::move(hints);
    return snap;
}

} // namespace viewer
