#include "expanse/shell.hpp"

#include "expanse/calendar.hpp"
#include "expanse/scenario.hpp"
#include "expanse/simulation.hpp"
#include "expanse/units.hpp"

#include <algorithm>
#include <format>
#include <fstream>
#include <iterator>

namespace expanse {

namespace {

using sim::CommandError;
using sim::Invocation;

const World& require_world(const Session& s) {
    if (!s.world) {
        throw CommandError("no game in progress; start one with 'new'");
    }
    return *s.world;
}

World& require_world(Session& s) {
    return const_cast<World&>(require_world(std::as_const(s)));
}

std::string money(Credits c) {
    // 1234567 -> "1,234,567 cr"
    std::string digits = std::to_string(c < 0 ? -c : c);
    for (std::ptrdiff_t i = static_cast<std::ptrdiff_t>(digits.size()) - 3; i > 0; i -= 3) {
        digits.insert(static_cast<std::size_t>(i), ",");
    }
    return std::format("{}{} cr", c < 0 ? "-" : "", digits);
}

std::string_view kind_label(MessageKind k) {
    switch (k) {
    case MessageKind::info: return "info";
    case MessageKind::ship: return "ship";
    case MessageKind::market: return "market";
    case MessageKind::crew: return "crew";
    case MessageKind::finance: return "finance";
    case MessageKind::warning: return "WARN";
    }
    return "?";
}

void print_message(const Message& m, std::ostream& out) {
    out << std::format("  [{}] {:<7} {}{}\n", calendar::format_datetime(m.time), kind_label(m.kind),
                       m.urgent ? "! " : "", m.text);
}

// Shows journal entries the player hasn't seen yet.
void flush_messages(Session& s, std::ostream& out) {
    const World& w = require_world(s);
    for (; s.messages_seen < w.messages.size(); ++s.messages_seen) {
        print_message(w.messages[s.messages_seen], out);
    }
}

template <typename Row>
sim::Completer keys_of(std::shared_ptr<const Content> content) {
    return [content] {
        std::vector<std::string> out;
        for (const auto& k : content->table<Row>().keys()) {
            out.emplace_back(k);
        }
        return out;
    };
}

std::string location_text(const Content& c, const World& w, const Ship& ship) {
    if (const auto* d = std::get_if<Docked>(&ship.location)) {
        return "docked at " + c.table<StationDef>()[d->station].name;
    }
    const auto& u = std::get<Underway>(ship.location);
    return std::format("underway to {}, arriving {} (in {})", c.table<StationDef>()[u.destination].name,
                       calendar::format_datetime(u.arrival), sim::format_duration(u.arrival - w.now()));
}

void advance_and_report(Session& s, sim::Time until, bool stop_on_urgent, std::ostream& out) {
    World& w = require_world(s);
    const AdvanceReport r = advance_to(*s.content, w, until, stop_on_urgent);
    flush_messages(s, out);
    out << std::format("{} — {}\n", calendar::format_datetime(w.now()),
                       r.stopped_early ? "stopped: something needs your attention" : "done");
}

} // namespace

void register_game_commands(ShellBus& bus, std::shared_ptr<const Content> content) {
    // --- game lifecycle ---

    bus.add_action(
        {.name = "new",
         .summary = "Start a new game",
         .details = "Starts a scenario from the loaded game data. The seed decides everything "
                    "random, so the same seed and commands replay identically.",
         .positionals = {{.name = "scenario", .help = "scenario key", .required = false,
                          .default_value = "secondhand",
                          .completer = keys_of<ScenarioDef>(content)}},
         .options = {{.name = "seed", .type = sim::ArgType::integer, .help = "world seed",
                      .default_value = "1"}}},
        [](Session& s, const Invocation& inv, std::ostream& out) {
            const auto& key = inv.get<std::string>("scenario");
            if (!s.content->find<ScenarioDef>(key)) {
                throw CommandError(std::format("unknown scenario '{}'", key));
            }
            s.world = new_game(*s.content, key, static_cast<std::uint64_t>(inv.get<std::int64_t>("seed")));
            s.messages_seen = 0;
            const ScenarioDef& sc = s.content->table<ScenarioDef>()[s.content->find<ScenarioDef>(key)];
            out << std::format("== {} ==\n{}\n\n", sc.name, sc.description);
            out << std::format("{} — type 'status' to look around.\n",
                               calendar::format_datetime(s.world->now()));
        });

    bus.add_query({.name = "save",
                   .summary = "Save the game to a file",
                   .positionals = {{.name = "path", .help = "file to write"}}},
                  [](const Session& s, const Invocation& inv, std::ostream& out) {
                      const World& w = require_world(s);
                      const auto& path = inv.get<std::string>("path");
                      const auto bytes = save_world(w);
                      std::ofstream f(path, std::ios::binary);
                      f.write(reinterpret_cast<const char*>(bytes.data()),
                              static_cast<std::streamsize>(bytes.size()));
                      if (!f.flush()) {
                          throw CommandError(std::format("failed writing '{}'", path));
                      }
                      out << std::format("saved {} bytes to {} (state hash {:016x})\n", bytes.size(),
                                         path, world_hash(w));
                  });

    bus.add_action({.name = "load",
                    .summary = "Load a saved game",
                    .details = "Replays of sessions that use 'load' depend on the save file.",
                    .positionals = {{.name = "path", .help = "file to read"}}},
                   [](Session& s, const Invocation& inv, std::ostream& out) {
                       const auto& path = inv.get<std::string>("path");
                       std::ifstream f(path, std::ios::binary);
                       if (!f) {
                           throw CommandError(std::format("cannot open '{}'", path));
                       }
                       const std::vector<std::uint8_t> bytes{std::istreambuf_iterator<char>(f), {}};
                       try {
                           s.world = load_world(bytes, *s.content);
                       } catch (const sim::SerializeError& e) {
                           throw CommandError(std::format("cannot load '{}': {}", path, e.what()));
                       }
                       s.messages_seen = s.world->messages.size();
                       out << std::format("loaded {} — {}\n", path, calendar::format_datetime(s.world->now()));
                   });

    bus.add_query({.name = "hash", .summary = "Print the world state hash (for replay checks)"},
                  [](const Session& s, const Invocation&, std::ostream& out) {
                      out << std::format("{:016x}\n", world_hash(require_world(s)));
                  });

    // --- time ---

    bus.add_query({.name = "date", .summary = "Show the current date and time"},
                  [](const Session& s, const Invocation&, std::ostream& out) {
                      out << calendar::format_datetime(require_world(s).now()) << "\n";
                  });

    bus.add_action({.name = "advance",
                    .aliases = {"adv"},
                    .summary = "Let time pass (stops early if something needs your attention)",
                    .positionals = {{.name = "span", .type = sim::ArgType::duration,
                                     .help = "how long, e.g. 6h, 3d, 2w"}},
                    .options = {{.name = "force", .type = sim::ArgType::flag,
                                 .help = "don't stop for urgent events"}}},
                   [](Session& s, const Invocation& inv, std::ostream& out) {
                       const auto span = inv.get<sim::Duration>("span");
                       if (span.seconds <= 0) {
                           throw CommandError("span must be positive");
                       }
                       const World& w = require_world(s);
                       advance_and_report(s, w.now() + span, !inv.get<bool>("force"), out);
                   });

    bus.add_action({.name = "wait",
                    .summary = "Let time pass until something needs your attention",
                    .options = {{.name = "max", .type = sim::ArgType::duration,
                                 .help = "give up after this long", .default_value = "30d"}}},
                   [](Session& s, const Invocation& inv, std::ostream& out) {
                       const World& w = require_world(s);
                       advance_and_report(s, w.now() + inv.get<sim::Duration>("max"), true, out);
                   });

    // --- looking around ---

    bus.add_query({.name = "messages",
                   .aliases = {"msgs"},
                   .summary = "Show recent messages",
                   .options = {{.name = "last", .type = sim::ArgType::integer,
                                .help = "how many", .default_value = "10"}}},
                  [](const Session& s, const Invocation& inv, std::ostream& out) {
                      const World& w = require_world(s);
                      const auto n = static_cast<std::size_t>(std::max<std::int64_t>(0, inv.get<std::int64_t>("last")));
                      const std::size_t first = w.messages.size() > n ? w.messages.size() - n : 0;
                      if (first == w.messages.size()) {
                          out << "  (no messages)\n";
                      }
                      for (std::size_t i = first; i < w.messages.size(); ++i) {
                          print_message(w.messages[i], out);
                      }
                  });

    bus.add_query(
        {.name = "status", .aliases = {"st"}, .summary = "Your company, ships and debts"},
        [](const Session& s, const Invocation&, std::ostream& out) {
            const World& w = require_world(s);
            const Content& c = *s.content;
            const Company& me = w.companies.at(w.player);
            out << std::format("{} — {}\n", me.name, calendar::format_datetime(w.now()));
            out << std::format("  cash      {}\n", money(me.cash));
            for (auto [id, loan] : w.loans) {
                (void)id;
                if (loan.borrower != w.player) {
                    continue;
                }
                out << std::format("  loan      {} owed to {}, {} due {}{}\n", money(loan.balance),
                                   c.table<StationDef>()[loan.lender].name, money(loan.weekly_payment),
                                   calendar::format_date(loan.next_due),
                                   loan.missed_payments ? std::format(" ({} missed!)", loan.missed_payments) : "");
            }
            for (auto [id, ship] : w.ships) {
                (void)id;
                if (ship.owner != w.player) {
                    continue;
                }
                const ShipClassDef& cls = c.table<ShipClassDef>()[ship.ship_class];
                out << std::format("  ship      {} ({})\n", ship.name, cls.name);
                out << std::format("            {}\n", location_text(c, w, ship));
                out << std::format("            reaction mass {:.0f}/{:.0f} t ({:.0f}%), hull {:.0f}%\n",
                                   ship.reaction_mass_t, cls.reaction_mass_capacity_t,
                                   100.0 * ship.reaction_mass_t / cls.reaction_mass_capacity_t,
                                   100.0 * ship.hull_condition);
                out << std::format("            cargo {:.0f}/{:.0f} t", cargo_mass_t(ship), cls.cargo_capacity_t);
                for (const CargoLot& lot : ship.cargo) {
                    out << std::format(", {:.0f} t {}", lot.tonnes, c.table<CommodityDef>()[lot.commodity].name);
                }
                out << "\n";
                std::size_t aboard = 0;
                for (auto [cid, member] : w.crew) {
                    (void)cid;
                    aboard += member.ship == id ? 1u : 0u;
                }
                out << std::format("            crew {}/{}\n", aboard, cls.crew_berths);
            }
        });

    bus.add_query(
        {.name = "stations",
         .summary = "List stations with distance and light-lag from your ship"},
        [](const Session& s, const Invocation&, std::ostream& out) {
            const World& w = require_world(s);
            const Content& c = *s.content;
            std::optional<Vec3> here;
            for (auto [id, ship] : w.ships) {
                (void)id;
                if (ship.owner == w.player) {
                    here = ship_position(c, w, ship);
                    break;
                }
            }
            out << std::format("  {:<20} {:<22} {:<12} {:>9} {:>10}\n", "key", "name", "faction", "dist AU",
                               "light-lag");
            for (auto [id, st] : c.table<StationDef>()) {
                const Vec3 p = c.orbits().world_position(c.orbit_of(id), w.now());
                const double d = here ? distance(*here, p) : 0.0;
                const auto lag = sim::seconds(static_cast<std::int64_t>(d / 299'792'458.0));
                constexpr std::array factions{"earth", "mars", "belt", "independent"};
                out << std::format("  {:<20} {:<22} {:<12} {:>9.2f} {:>10}\n",
                                   c.table<StationDef>().key(id), st.name,
                                   factions[static_cast<std::size_t>(st.faction)], units::to_au(d),
                                   sim::format_duration(lag));
            }
        });

    bus.add_query(
        {.name = "station",
         .summary = "Describe a station and its stockpiles",
         .positionals = {{.name = "key", .help = "station key (see 'stations')",
                          .completer = keys_of<StationDef>(content)}}},
        [](const Session& s, const Invocation& inv, std::ostream& out) {
            const World& w = require_world(s);
            const Content& c = *s.content;
            const auto id = c.find<StationDef>(inv.get<std::string>("key"));
            if (!id) {
                throw CommandError(std::format("no station '{}'", inv.get<std::string>("key")));
            }
            const StationDef& st = c.table<StationDef>()[id];
            out << std::format("{} — pop. {}, docking {}/day\n", st.name, st.population, money(st.docking_fee));
            const StationState& state = w.stations[index_of(id)];
            for (const MarketEntryDef& m : st.market) {
                out << std::format("  {:<18} stock {:>9.0f} t   +{:.0f}/-{:.0f} t/day\n",
                                   c.table<CommodityDef>()[m.commodity].name,
                                   state.stock[index_of(m.commodity)], m.production, m.consumption);
            }
        });
}

} // namespace expanse
