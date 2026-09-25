#include "expanse/shell.hpp"

#include "expanse/calendar.hpp"
#include "expanse/crew.hpp"
#include "expanse/economy.hpp"
#include "expanse/finance.hpp"
#include "expanse/scenario.hpp"
#include "expanse/ships.hpp"
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
    if (w.game_over) {
        out << std::format("\n*** GAME OVER: {} ***\n", w.game_over->reason);
    }
}

// The world, for commands that only make sense while the company is still in business.
World& require_playing(Session& s) {
    World& w = require_world(s);
    if (w.game_over) {
        throw CommandError(std::format("game over: {} ('new' starts again)", w.game_over->reason));
    }
    return w;
}

ShipId player_ship(const World& w) {
    for (auto [id, ship] : w.ships) {
        if (ship.owner == w.player) {
            return id;
        }
    }
    throw CommandError("you have no ship");
}

StationId station_arg(const Session& s, const Invocation& inv, std::string_view name = "station") {
    const auto& key = inv.get<std::string>(name);
    const StationId id = s.content->find<StationDef>(key);
    if (!id) {
        throw CommandError(std::format("no station '{}' (see 'stations')", key));
    }
    return id;
}

// Shared by plot and go: --accel, --maxdv and --within select the course.
CoursePreview course_for(const Session& s, const Invocation& inv) {
    const World& w = require_world(s);
    const ShipId ship = player_ship(w);
    const StationId dest = station_arg(s, inv);
    const double accel = inv.get<sim::Acceleration>("accel").gees();
    if (inv.has("within")) {
        return plot_cheapest_within(*s.content, w, ship, dest, accel, inv.get<sim::Duration>("within"));
    }
    CourseOptions opts{accel, std::numeric_limits<double>::infinity()};
    if (inv.has("maxdv")) {
        opts.max_delta_v_km_s = inv.get<double>("maxdv");
    }
    return plot_course(*s.content, w, ship, dest, opts);
}

void print_preview(const Content& c, const CoursePreview& p, std::ostream& out) {
    const auto& stations = c.table<StationDef>();
    out << std::format("Course {} -> {}\n", stations[p.origin].name, stations[p.destination].name);
    out << std::format("  depart {}  arrive {}  ({})\n", calendar::format_datetime(p.departure),
                       calendar::format_datetime(p.arrival), sim::format_duration(p.duration));
    out << std::format("  {:.2f} AU at {:.2f} g{}, flip at {}, peak {:.0f} km/s\n", p.distance_au, p.accel_g,
                       p.accel_limited ? " (drive-limited)" : "", calendar::format_datetime(p.flip),
                       p.peak_speed_km_s);
    out << std::format("  dv {:.0f} km/s (incl. {:.0f} velocity match)\n", p.delta_v_km_s, p.match_delta_v_km_s);
    out << std::format("  reaction mass {:.1f} t of {:.1f} t aboard -> {:.1f} t left ({:.0f}% of tank)\n",
                       p.reaction_mass_needed_t, p.reaction_mass_aboard_t, p.reaction_mass_after_t,
                       p.tank_after_pct);
    out << std::format("  hull wear ~{:.1f}%, light-lag {}\n", p.hull_wear_pct,
                       sim::format_duration(p.light_lag));
    out << (p.feasible() ? "  GO\n" : std::format("  NO GO: {}\n", p.reason));
}

const std::vector<sim::ArgSpec> course_options = {
    {.name = "accel", .type = sim::ArgType::acceleration, .help = "burn acceleration",
     .default_value = "0.3g"},
    {.name = "maxdv", .type = sim::ArgType::number, .help = "dv budget in km/s (coast to save mass)"},
    {.name = "within", .type = sim::ArgType::duration,
     .help = "cheapest course arriving within this time"},
};

CommodityId commodity_arg(const Session& s, const Invocation& inv) {
    const auto& key = inv.get<std::string>("commodity");
    const CommodityId id = s.content->find<CommodityDef>(key);
    if (!id) {
        throw CommandError(std::format("no commodity '{}'", key));
    }
    return id;
}

StationId docked_station(const World& w) {
    const auto* d = std::get_if<Docked>(&w.ships.at(player_ship(w)).location);
    if (d == nullptr) {
        throw CommandError("you're not docked");
    }
    return d->station;
}

// Crew are addressed by their table slot ("#12"), shown by the `crew` command.
CrewId crew_arg(const World& w, const Invocation& inv) {
    const auto slot = inv.get<std::int64_t>("id");
    for (auto [id, member] : w.crew) {
        (void)member;
        if (static_cast<std::int64_t>(id.index) == slot) {
            return id;
        }
    }
    throw CommandError(std::format("nobody with id #{} (see 'crew')", slot));
}

void print_trade(Session& s, const economy::TradeResult& r, std::ostream& out) {
    if (!r.ok()) {
        throw CommandError(r.reason);
    }
    s.messages_seen = s.world->messages.size(); // the reason repeats the journal line
    out << r.reason << "\n";
    out << std::format("cash now {}\n", format_credits(s.world->companies.at(s.world->player).cash));
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
                    const std::string& what = c.table<CommodityDef>()[lot.commodity].name;
                    out << (lot.tonnes < 10.0 ? std::format(", {:.2f} t {}", lot.tonnes, what)
                                              : std::format(", {:.0f} t {}", lot.tonnes, what));
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
    // --- flying ---

    bus.add_query({.name = "plot",
                   .summary = "Preview a course to a station",
                   .details = "Defaults to the fastest flip-and-burn at 0.3 g. --maxdv caps the dv "
                              "(the ship coasts mid-course); --within finds the cheapest course "
                              "arriving in time.",
                   .positionals = {{.name = "station", .help = "destination key",
                                    .completer = keys_of<StationDef>(content)}},
                   .options = course_options},
                  [](const Session& s, const Invocation& inv, std::ostream& out) {
                      print_preview(*s.content, course_for(s, inv), out);
                  });

    bus.add_query({.name = "routes",
                   .summary = "Fastest course to every station at an acceleration",
                   .options = {course_options[0]}},
                  [](const Session& s, const Invocation& inv, std::ostream& out) {
                      const World& w = require_world(s);
                      const auto all = plot_all_destinations(
                          *s.content, w, player_ship(w), {inv.get<sim::Acceleration>("accel").gees()});
                      out << std::format("  {:<22} {:>7} {:>11} {:>9} {:>9}  {}\n", "destination", "AU",
                                         "time", "rmass t", "tank left", "");
                      for (const CoursePreview& p : all) {
                          out << std::format("  {:<22} {:>7.2f} {:>11} {:>9.1f} {:>8.0f}%  {}\n",
                                             s.content->table<StationDef>()[p.destination].name,
                                             p.distance_au, sim::format_duration(p.duration),
                                             p.reaction_mass_needed_t, p.tank_after_pct,
                                             p.feasible() ? "" : p.reason);
                      }
                  });

    bus.add_action({.name = "go",
                    .summary = "Undock and fly to a station (same options as plot)",
                    .positionals = {{.name = "station", .help = "destination key",
                                     .completer = keys_of<StationDef>(content)}},
                    .options = course_options},
                   [](Session& s, const Invocation& inv, std::ostream& out) {
                       World& w = require_playing(s);
                       const CoursePreview p = course_for(s, inv);
                       if (!p.feasible()) {
                           throw CommandError(p.reason);
                       }
                       // Fly exactly the previewed course: cap dv at the plan's figure.
                       const DepartResult r = depart(*s.content, w, player_ship(w), p.destination,
                                                     {p.accel_g, p.delta_v_km_s * (1.0 + 1e-9)});
                       if (r.status != CourseStatus::ok) {
                           throw CommandError(r.reason);
                       }
                       flush_messages(s, out);
                       out << std::format("Underway. ETA {} ({}). 'wait' to fly.\n",
                                          calendar::format_datetime(r.preview.arrival),
                                          sim::format_duration(r.preview.duration));
                   });

    // --- money ---

    bus.add_query({.name = "books",
                   .summary = "Income and expenses over recent days",
                   .options = {{.name = "days", .type = sim::ArgType::integer, .help = "period",
                                .default_value = "30"},
                               {.name = "entries", .type = sim::ArgType::integer,
                                .help = "recent ledger lines to show", .default_value = "10"}}},
                  [](const Session& s, const Invocation& inv, std::ostream& out) {
                      const World& w = require_world(s);
                      const sim::Time to = w.now() + sim::seconds(1);
                      const sim::Time from = to - sim::days(std::max<std::int64_t>(1, inv.get<std::int64_t>("days")));
                      const LedgerSummary sum = summarize_ledger(w, w.player, from, to);
                      out << std::format("Books since {}: opening {}, closing {}\n",
                                         calendar::format_date(from), format_credits(sum.opening_balance),
                                         format_credits(sum.closing_balance));
                      for (const auto& [name, cat] : enum_names(LedgerCategory{})) {
                          if (sum.income_of(cat) != 0 || sum.expense_of(cat) != 0) {
                              out << std::format("  {:<10} +{:>12}  -{:>12}\n", name,
                                                 format_credits(sum.income_of(cat)),
                                                 format_credits(sum.expense_of(cat)));
                          }
                      }
                      out << std::format("  net {}\n", format_credits(sum.net()));
                      std::vector<const LedgerEntry*> mine;
                      for (const LedgerEntry& e : ledger_between(w, from, to)) {
                          if (e.company == w.player) {
                              mine.push_back(&e);
                          }
                      }
                      const auto n = static_cast<std::size_t>(std::max<std::int64_t>(0, inv.get<std::int64_t>("entries")));
                      for (std::size_t i = mine.size() > n ? mine.size() - n : 0; i < mine.size(); ++i) {
                          const LedgerEntry& e = *mine[i];
                          out << std::format("  {}  {:>12}  {:<8} {}\n", calendar::format_date(e.time),
                                             format_credits(e.amount), to_string(e.category), e.description);
                      }
                      const Credits tabs = total_dock_tabs(w, w.player);
                      if (tabs > 0) {
                          out << std::format("  owed to dockmasters: {}\n", format_credits(tabs));
                      }
                  });

    bus.add_action({.name = "pay",
                    .summary = "Pay toward your loan now (counts toward the next instalment)",
                    .positionals = {{.name = "amount", .type = sim::ArgType::integer, .help = "credits"}}},
                   [](Session& s, const Invocation& inv, std::ostream& out) {
                       World& w = require_playing(s);
                       for (auto [id, loan] : w.loans) {
                           if (loan.borrower == w.player) {
                               const PaymentResult r = pay_loan(*s.content, w, id, inv.get<std::int64_t>("amount"));
                               if (!r.ok) {
                                   throw CommandError(r.message);
                               }
                               flush_messages(s, out);
                               out << r.message << "\n";
                               return;
                           }
                       }
                       throw CommandError("you have no loans");
                   });

    bus.add_action({.name = "paytab", .summary = "Settle what you owe the dockmaster here"},
                   [](Session& s, const Invocation&, std::ostream& out) {
                       World& w = require_playing(s);
                       const Ship& ship = w.ships.at(player_ship(w));
                       const auto* docked = std::get_if<Docked>(&ship.location);
                       if (docked == nullptr) {
                           throw CommandError("you're not docked");
                       }
                       const Credits paid = pay_dock_tab(*s.content, w, w.player, docked->station);
                       flush_messages(s, out);
                       out << std::format("paid {}; still owed here: {}\n", format_credits(paid),
                                          format_credits(dock_tab(w, w.player, docked->station)));
                   });
    // --- trade ---

    bus.add_query({.name = "market",
                   .summary = "Prices at your dock (or another station, as last known)",
                   .positionals = {{.name = "station", .help = "station key", .required = false,
                                    .completer = keys_of<StationDef>(content)}}},
                  [](const Session& s, const Invocation& inv, std::ostream& out) {
                      const World& w = require_world(s);
                      const StationId st = inv.has("station") ? station_arg(s, inv) : docked_station(w);
                      const StationDef& def = s.content->table<StationDef>()[st];
                      out << std::format("{} market                 stock      normal     buy at    sell at\n", def.name);
                      for (const MarketEntryDef& m : def.market) {
                          const auto q = economy::quote(*s.content, w, st, m.commodity);
                          out << std::format("  {:<12} {:<14} {:>8.0f} t {:>8.0f} t {:>7.0f} cr {:>7.0f} cr{}\n",
                                             s.content->table<CommodityDef>().key(m.commodity),
                                             s.content->table<CommodityDef>()[m.commodity].name, q->stock,
                                             q->target, q->ask, q->bid,
                                             q->disrupted_days ? "  (supply disrupted)" : "");
                      }
                  });

    const sim::ArgSpec commodity_spec{.name = "commodity", .help = "commodity key",
                                      .completer = keys_of<CommodityDef>(content)};

    bus.add_action({.name = "buy",
                    .summary = "Buy cargo at your dock",
                    .positionals = {commodity_spec,
                                    {.name = "tonnes", .type = sim::ArgType::number, .help = "amount"}}},
                   [](Session& s, const Invocation& inv, std::ostream& out) {
                       World& w = require_playing(s);
                       print_trade(s, economy::buy(*s.content, w, player_ship(w), commodity_arg(s, inv),
                                                   inv.get<double>("tonnes")), out);
                   });

    bus.add_action({.name = "sell",
                    .summary = "Sell cargo at your dock",
                    .positionals = {commodity_spec,
                                    {.name = "tonnes", .type = sim::ArgType::number,
                                     .help = "amount (default: all aboard)", .required = false}}},
                   [](Session& s, const Invocation& inv, std::ostream& out) {
                       World& w = require_playing(s);
                       const ShipId ship = player_ship(w);
                       const CommodityId k = commodity_arg(s, inv);
                       double tonnes = 0.0;
                       if (inv.has("tonnes")) {
                           tonnes = inv.get<double>("tonnes");
                       } else {
                           for (const CargoLot& lot : w.ships.at(ship).cargo) {
                               tonnes += lot.commodity == k ? lot.tonnes : 0.0;
                           }
                       }
                       const economy::TradeResult r = economy::sell(*s.content, w, ship, k, tonnes);
                       print_trade(s, r, out);
                       if (r.ok()) {
                           out << std::format("profit on cost {}\n", format_credits(r.profit));
                       }
                   });

    bus.add_action({.name = "refuel",
                    .summary = "Buy water as reaction mass (default: fill what you can afford)",
                    .positionals = {{.name = "tonnes", .type = sim::ArgType::number, .help = "amount",
                                     .required = false}}},
                   [](Session& s, const Invocation& inv, std::ostream& out) {
                       World& w = require_playing(s);
                       const std::optional<double> t =
                           inv.has("tonnes") ? std::optional{inv.get<double>("tonnes")} : std::nullopt;
                       print_trade(s, economy::refuel(*s.content, w, player_ship(w), t), out);
                   });

    // --- crew ---

    bus.add_query({.name = "crew", .summary = "Who's aboard, supplies, and who's looking for work here"},
                  [](const Session& s, const Invocation&, std::ostream& out) {
                      const World& w = require_world(s);
                      const Content& c = *s.content;
                      const ShipId ship = player_ship(w);
                      auto row = [&](CrewId id) {
                          const CrewMember& m = w.crew.at(id);
                          out << std::format("  #{:<4} {:<24} {:<9} skill {:>3}  {:>6}/wk  morale {:>3.0f}%  {}\n",
                                             id.index, m.name, crew::role_name(m.role), m.skill,
                                             format_credits(m.wage_per_week), 100.0 * m.morale, m.background);
                      };
                      out << "Aboard:\n";
                      for (const CrewId id : crew::aboard(w, ship)) {
                          row(id);
                      }
                      const Ship& sh = w.ships.at(ship);
                      const crew::Provisions have = crew::stores(c, sh);
                      out << std::format("  payroll {}/week; stores: water {:.2f} t, food {:.2f} t, oxygen {:.2f} t\n",
                                         format_credits(crew::weekly_payroll(w, ship)), have.water_t,
                                         have.food_t, have.oxygen_t);
                      if (const auto* d = std::get_if<Docked>(&sh.location)) {
                          out << std::format("Looking for work at {}:\n", c.table<StationDef>()[d->station].name);
                          for (const CrewId id : crew::pool_at(w, d->station)) {
                              row(id);
                          }
                      }
                  });

    bus.add_action({.name = "hire",
                    .summary = "Sign on someone from the dock (pays a week's wage up front)",
                    .positionals = {{.name = "id", .type = sim::ArgType::integer, .help = "#id from 'crew'"}}},
                   [](Session& s, const Invocation& inv, std::ostream& out) {
                       World& w = require_playing(s);
                       const CrewId id = crew_arg(w, inv);
                       const crew::HireResult r = crew::hire(*s.content, w, player_ship(w), id);
                       if (r.status != crew::HireStatus::ok) {
                           throw CommandError(r.reason);
                       }
                       flush_messages(s, out);
                       out << std::format("{} signs on.\n", w.crew.at(id).name);
                   });

    bus.add_action({.name = "fire",
                    .summary = "Put a crew member ashore at this dock",
                    .positionals = {{.name = "id", .type = sim::ArgType::integer, .help = "#id from 'crew'"}}},
                   [](Session& s, const Invocation& inv, std::ostream& out) {
                       World& w = require_playing(s);
                       const CrewId id = crew_arg(w, inv);
                       const std::string name = w.crew.at(id).name;
                       const crew::FireResult r = crew::fire(w, id);
                       if (r.status != crew::FireStatus::ok) {
                           throw CommandError(r.reason);
                       }
                       flush_messages(s, out);
                       out << std::format("{} goes ashore.\n", name);
                   });
}

} // namespace expanse
