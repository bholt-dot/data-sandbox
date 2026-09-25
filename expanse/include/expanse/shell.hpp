#pragma once

// Shell commands for the game. The Session is the command context: static content plus the
// current world (if a game has been started). Commands that change the world are bus actions,
// so a session's log replays as a script (same build + data ⇒ same world hash).

#include "expanse/content.hpp"
#include "expanse/world.hpp"
#include "simcore/command_bus.hpp"

#include <memory>
#include <optional>

namespace expanse {

struct Session {
    std::shared_ptr<const Content> content;
    std::optional<World> world;
    std::size_t messages_seen = 0; // journal index already shown to the player
};

using ShellBus = sim::CommandBus<Session>;

// `content` is used for tab-completion candidates (station and scenario keys).
void register_game_commands(ShellBus& bus, std::shared_ptr<const Content> content);

} // namespace expanse
