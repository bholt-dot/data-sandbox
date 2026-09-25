#pragma once

// Shell commands for the game. The Session is the command context: static content plus the
// current world (if a game has been started). Commands that change the world are bus actions,
// so a session's log replays as a script (same build + data ⇒ same world hash).
//
// Output is semantic (sim::Doc): the same commands render as plain text for scripts, ANSI colour
// in a terminal, or panels in the full-screen UI. The status banner and journal lines are exposed
// here as plain data / styled lines so any front end can show them.

#include "expanse/content.hpp"
#include "expanse/ships.hpp"
#include "expanse/world.hpp"
#include "simcore/command_bus.hpp"
#include "simcore/doc.hpp"

#include <cstddef>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace expanse {

struct Session {
    std::shared_ptr<const Content> content;
    std::optional<World> world;
    std::size_t messages_seen = 0; // journal index already shown to the player
    // Presentation-only: the course from the last `plot`, until the player acts on it or time
    // moves (a viewer draws it as a provisional line). Never read by the simulation.
    mutable std::optional<CoursePreview> last_plot;
};

using ShellBus = sim::CommandBus<Session>;

// `content` is used for tab-completion candidates (station and scenario keys).
void register_game_commands(ShellBus& bus, std::shared_ptr<const Content> content);

// One journal entry as the shell prints it: "  [2350-03-14 00:00] ship    text", urgent entries
// highlighted. `compact` drops the indent, the year and the kind column (for narrow panels).
sim::Line journal_line(const Message& message, bool compact = false);

// What an always-visible status bar shows, as plain data.
struct StatusBanner {
    struct LoanStatus {
        std::string lender;
        Credits balance = 0;
        Credits instalment = 0; // still due at `due` (after voluntary payments)
        sim::Time due;
        sim::Duration until_due;
        int missed = 0;
        int missed_limit = 0;
    };
    struct ShipStatus {
        std::string name;
        std::optional<std::string> docked_at; // station name
        std::optional<std::string> destination;
        sim::Time arrival;       // when underway
        sim::Duration remaining; // when underway
        double reaction_mass_pct = 0.0;
        double hull_pct = 0.0;
        std::size_t crew = 0;
        std::size_t berths = 0;
    };

    std::string company;
    sim::Time now;
    Credits cash = 0;
    std::optional<LoanStatus> loan; // the player's loan with the earliest instalment
    std::optional<ShipStatus> ship; // the player's first ship
    std::optional<std::string> game_over;
};

// nullopt when no game is in progress.
std::optional<StatusBanner> status_banner(const Session& session);

// The banner as styled segments ("Cash 1,850 cr", "RM 35%", ...), each meant to stay on one row;
// a front end packs them into as many rows as its width needs.
std::vector<sim::Line> banner_segments(const StatusBanner& banner);

} // namespace expanse
