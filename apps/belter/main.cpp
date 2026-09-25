#include <cstdint>
#include <format>
#include <fstream>
#include <iostream>
#include <memory>
#include <variant>

#include "expanse/game.hpp"
#include "line_editor.hpp"
#include "simcore/command_bus.hpp"
#include "simcore/repl.hpp"
#include "simcore/scheduler.hpp"
#include "simcore/time.hpp"

namespace {

// Placeholder world proving the command plumbing; real game state and commands come later.
struct DailyTick {};
using Payload = std::variant<DailyTick>;

struct World {
    sim::Scheduler<Payload> scheduler;
    std::int64_t daily_ticks = 0;

    World() { scheduler.add_periodic(sim::days(1), DailyTick{}); }
};

using Bus = sim::CommandBus<World>;

void register_commands(Bus& bus) {
    bus.add_query({.name = "date", .summary = "Show the current simulation time"},
                  [](const World& w, const sim::Invocation&, std::ostream& out) {
                      const sim::Duration elapsed = w.scheduler.now() - sim::Time{};
                      out << std::format("T+{} (day {}), {} daily ticks\n",
                                         sim::format_duration(elapsed),
                                         elapsed.seconds / 86400 + 1, w.daily_ticks);
                  });

    bus.add_action({.name = "advance",
                    .aliases = {"adv"},
                    .summary = "Advance simulation time",
                    .positionals = {{.name = "span",
                                     .type = sim::ArgType::duration,
                                     .help = "how far to advance, e.g. 30d, 6h, 90m"}}},
                   [](World& w, const sim::Invocation& inv, std::ostream& out) {
                       const auto span = inv.get<sim::Duration>("span");
                       if (span.seconds <= 0) {
                           throw sim::CommandError("span must be positive");
                       }
                       const auto r = w.scheduler.advance_by(
                           span, [&w](auto&, const sim::Occurrence<Payload>& occ) {
                               if (std::holds_alternative<DailyTick>(occ.payload)) {
                                   ++w.daily_ticks;
                               }
                           });
                       out << std::format("advanced {} ({} daily ticks)\n", sim::format_duration(span),
                                          r.periodic_fired);
                   });

    bus.add_query({.name = "save-script",
                   .summary = "Write this session's state-changing commands to a replay script",
                   .positionals = {{.name = "path", .help = "file to write"}}},
                  [&bus](const World&, const sim::Invocation& inv, std::ostream& out) {
                      const auto& path = inv.get<std::string>("path");
                      std::ofstream file(path);
                      if (!file) {
                          throw sim::CommandError(std::format("cannot open '{}' for writing", path));
                      }
                      file << "# " << expanse::game_name() << " session replay\n";
                      bus.write_script(file);
                      if (!file.flush()) {
                          throw sim::CommandError(std::format("failed writing '{}'", path));
                      }
                      out << std::format("wrote {} commands to {}\n", bus.log().size(), path);
                  });
}

} // namespace

int main() {
    World world;
    Bus bus;
    register_commands(bus);

    std::unique_ptr<sim::LineReader> terminal = sim::make_terminal_reader(
        [&bus](std::string_view line) { return bus.registry().complete(line); });
    // Piped input: echo each line so the transcript reads like an interactive session.
    sim::StreamLineReader piped(std::cin, {}, &std::cout);
    sim::LineReader& reader = terminal ? *terminal : piped;

    if (terminal) {
        std::cout << std::format("{} - type 'help' for commands, Tab to complete.\n",
                                 expanse::game_name());
    }
    return sim::run_repl(bus, world, reader, std::cout);
}
