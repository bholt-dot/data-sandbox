#include <doctest/doctest.h>

#include "expanse/content.hpp"
#include "expanse/scenario.hpp"
#include "expanse/shell.hpp"
#include "viewer/viewer_link.hpp"

#include <atomic>
#include <memory>
#include <thread>
#include <vector>

namespace {

std::shared_ptr<const expanse::Content> game_content() {
    static const std::shared_ptr<const expanse::Content> content = [] {
        sim::Diagnostics diags;
        std::shared_ptr<const expanse::Content> c = expanse::Content::load(BELTER_DATA_DIR, diags);
        if (!c) {
            FAIL(diags.to_string());
        }
        return c;
    }();
    return content;
}

std::shared_ptr<const viewer::ViewSnapshot> snapshot_at(std::int64_t seconds) {
    auto snap = std::make_shared<viewer::ViewSnapshot>();
    snap->content = game_content();
    snap->time = sim::Time{seconds};
    return snap;
}

} // namespace

TEST_CASE("ViewerLink starts empty and open") {
    const viewer::ViewerLink link;
    CHECK(link.latest() == nullptr);
    CHECK(link.generation() == 0);
    CHECK_FALSE(link.close_requested());
    CHECK_FALSE(link.viewer_closed());
}

TEST_CASE("ViewerLink keeps the latest snapshot and ignores null") {
    viewer::ViewerLink link;
    link.publish(snapshot_at(1));
    auto first = link.latest();
    link.publish(snapshot_at(2));
    link.publish(nullptr);
    REQUIRE(link.latest() != nullptr);
    CHECK(link.latest()->time.seconds == 2);
    CHECK(link.generation() == 2);
    // A reader's snapshot stays valid after it is superseded.
    CHECK(first->time.seconds == 1);
}

TEST_CASE("ViewerLink close signals are independent") {
    viewer::ViewerLink link;
    link.request_close();
    CHECK(link.close_requested());
    CHECK_FALSE(link.viewer_closed());
    link.notify_viewer_closed();
    CHECK(link.viewer_closed());
}

TEST_CASE("ViewerLink hands snapshots from a publisher thread to a reader thread") {
    viewer::ViewerLink link;
    constexpr std::int64_t count = 2000;

    std::atomic<bool> reader_ok{true};
    std::thread reader([&] {
        std::int64_t last_seen = -1;
        while (!link.close_requested()) {
            const auto snap = link.latest();
            if (!snap) {
                continue;
            }
            // Snapshots arrive in publish order (some skipped) and are intact.
            if (snap->time.seconds < last_seen || snap->content == nullptr) {
                reader_ok = false;
            }
            last_seen = snap->time.seconds;
        }
        const auto final_snap = link.latest();
        if (!final_snap || final_snap->time.seconds != count - 1) {
            reader_ok = false;
        }
        link.notify_viewer_closed();
    });

    for (std::int64_t i = 0; i < count; ++i) {
        link.publish(snapshot_at(i));
    }
    link.request_close();
    reader.join();

    CHECK(reader_ok.load());
    CHECK(link.viewer_closed());
    CHECK(link.generation() == static_cast<std::uint64_t>(count));
}

TEST_CASE("make_snapshot copies the world so later changes do not leak into it") {
    expanse::Session session;
    session.content = game_content();
    const auto empty = viewer::make_snapshot(session);
    CHECK_FALSE(empty->world.has_value());
    CHECK(empty->content == session.content);
    CHECK_FALSE(empty->hints.plot_preview.has_value());

    const std::string scenario = session.content->table<expanse::ScenarioDef>().keys().front();
    session.world = expanse::new_game(*session.content, scenario, 7);
    const auto snap = viewer::make_snapshot(session, {.focus_ship = std::nullopt, .plot_preview = std::nullopt});
    REQUIRE(snap->world.has_value());
    CHECK(snap->time == session.world->now());
    const std::uint64_t hash = expanse::world_hash(*snap->world);
    CHECK(hash == expanse::world_hash(*session.world));

    session.world->messages.push_back({.time = {}, .kind = expanse::MessageKind::info, .urgent = false, .text = "x"});
    CHECK(expanse::world_hash(*snap->world) == hash);
}
