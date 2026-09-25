#pragma once

// Shared fixtures for the viewer's CPU tests: the shipped game data and a new game on it.

#include <doctest/doctest.h>

#include "expanse/content.hpp"
#include "expanse/scenario.hpp"
#include "expanse/simulation.hpp"
#include "viewer/view_snapshot.hpp"

#include <memory>
#include <string>

namespace viewer::test {

inline std::shared_ptr<const expanse::Content> game_content() {
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

inline expanse::ShipId player_ship(const expanse::World& world) {
    for (auto [id, ship] : world.ships) {
        if (ship.owner == world.player) {
            return id;
        }
    }
    return {};
}

inline std::shared_ptr<const ViewSnapshot> new_game_snapshot(ViewHints hints = {}, sim::Duration after = {}) {
    const auto content = game_content();
    const std::string scenario = content->table<expanse::ScenarioDef>().keys().front();
    expanse::World world = expanse::new_game(*content, scenario, 1);
    if (after.seconds > 0) {
        expanse::advance_to(*content, world, world.now() + after, false);
    }
    return make_snapshot(content, world, std::move(hints));
}

// Hints naming the player's ship of a new game, as the shell sends them.
inline ViewHints focus_hints() {
    const auto content = game_content();
    const expanse::World world =
        expanse::new_game(*content, content->table<expanse::ScenarioDef>().keys().front(), 1);
    ViewHints hints;
    hints.focus_ship = player_ship(world);
    return hints;
}

} // namespace viewer::test
