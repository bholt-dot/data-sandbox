#pragma once

// What the viewer draws: an immutable, self-contained copy of the game state at one instant.
// Snapshots are built on the shell thread and handed to the viewer through a ViewerLink
// (viewer_link.hpp); once published they are never modified, so the viewer reads them without
// locks while the shell keeps mutating its own World.

#include "expanse/content.hpp"
#include "expanse/shell.hpp"
#include "expanse/ships.hpp"
#include "expanse/world.hpp"
#include "simcore/time.hpp"

#include <memory>
#include <optional>

namespace viewer {

// Presentation hints from the shell. None of this is game state.
struct ViewHints {
    // Ship to highlight (e.g. the one the captain is commanding). Null/absent: none.
    std::optional<expanse::ShipId> focus_ship;
    // A course the captain is considering (`plot`), drawn as a provisional line.
    std::optional<expanse::CoursePreview> plot_preview;
};

struct ViewSnapshot {
    std::shared_ptr<const expanse::Content> content; // never null
    // A full copy of the World (all of its members are value types). Absent before a game is
    // started or loaded; the viewer then shows the system at `time`.
    std::optional<expanse::World> world;
    sim::Time time{}; // == world->now() when there is a world
    ViewHints hints;
};

// Snapshot of the shell's session: copies the World, O(world size). The journal and ledger grow
// with play, so a long game makes this a few hundred KB; publish once per command, not per tick.
std::shared_ptr<const ViewSnapshot> make_snapshot(const expanse::Session& session, ViewHints hints = {});
std::shared_ptr<const ViewSnapshot> make_snapshot(std::shared_ptr<const expanse::Content> content,
                                                  const expanse::World& world, ViewHints hints = {});

} // namespace viewer
