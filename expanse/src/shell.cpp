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
#include <cmath>
#include <format>
#include <fstream>
#include <iterator>

namespace expanse {

namespace {

using sim::Align;
using sim::CommandError;
using sim::Doc;
using sim::Invocation;
using sim::Style;
using sim::styled;

const World& require_world(const Session& s) {
    if (!s.world) {
        throw CommandError("no game in progress; start one with 'new'");
    }
    return *s.world;
}

World& require_world(Session& s) {
    return const_cast<World&>(require_world(std::as_const(s)));
}

// --- styled fragments ---

sim::Span money(Credits c) { return styled(Style::money, format_credits(c)); }

// An amount that is good news when positive (income, profit) and bad news when negative.
sim::Span money_delta(Credits c) {
    return styled(c > 0 ? Style::positive : c < 0 ? Style::negative : Style::plain, format_credits(c));
}

sim::Span key(std::string text) { return styled(Style::key, std::move(text)); }

// A percentage of something that runs out: warning below `warn`, negative below `critical`.
sim::Span level(double pct, double warn, double critical) {
    const Style st = pct < critical ? Style::negative : pct < warn ? Style::warning : Style::plain;
    return styled(st, std::format("{:.0f}%", pct));
}

// Shorter than sim::format_duration: the two leading units ("6d 23h", "4h 12m", "35m").
std::string short_duration(sim::Duration d) {
    const std::int64_t s = std::max<std::int64_t>(0, d.seconds);
    const std::int64_t days = s / 86400;
    const std::int64_t hours = (s % 86400) / 3600;
    const std::int64_t minutes = (s % 3600) / 60;
    if (days > 0) return std::format("{}d {}h", days, hours);
    if (hours > 0) return std::format("{}h {}m", hours, minutes);
    return std::format("{}m", minutes);
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

// Shows journal entries the player hasn't seen yet.
void flush_messages(Session& s, Doc& out) {
    const World& w = require_world(s);
    for (; s.messages_seen < w.messages.size(); ++s.messages_seen) {
        out << journal_line(w.messages[s.messages_seen]) << "\n";
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

void advance_and_report(Session& s, sim::Time until, bool stop_on_urgent, Doc& out) {
    World& w = require_world(s);
    s.last_plot.reset(); // a plotted course departs "now"; once time moves it is stale
    const AdvanceReport r = advance_to(*s.content, w, until, stop_on_urgent);
    flush_messages(s, out);
    out << calendar::format_datetime(w.now()) << " — "
        << (r.stopped_early ? styled(Style::warning, "stopped: something needs your attention")
                            : styled(Style::good, "done"))
        << "\n";
    if (w.game_over) {
        out << "\n" << styled(Style::urgent, std::format("*** GAME OVER: {} ***", w.game_over->reason)) << "\n";
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

std::optional<ShipId> find_player_ship(const World& w) {
    for (auto [id, ship] : w.ships) {
        if (ship.owner == w.player) {
            return id;
        }
    }
    return std::nullopt;
}

ShipId player_ship(const World& w) {
    if (const auto id = find_player_ship(w)) {
        return *id;
    }
    throw CommandError("you have no ship");
}

StationId station_arg(const Session& s, const Invocation& inv, std::string_view name = "station") {
    const auto& k = inv.get<std::string>(name);
    const StationId id = s.content->find<StationDef>(k);
    if (!id) {
        throw CommandError(std::format("no station '{}' (see 'stations')", k));
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

void print_preview(const Content& c, const CoursePreview& p, Doc& out) {
    const auto& stations = c.table<StationDef>();
    out.heading(std::format("Course {} -> {}", stations[p.origin].name, stations[p.destination].name));
    out << std::format("  depart {}  arrive {}  ({})\n", calendar::format_datetime(p.departure),
                       calendar::format_datetime(p.arrival), sim::format_duration(p.duration));
    out << std::format("  {:.2f} AU at {:.2f} g{}, flip at {}, peak {:.0f} km/s\n", p.distance_au, p.accel_g,
                       p.accel_limited ? " (drive-limited)" : "", calendar::format_datetime(p.flip),
                       p.peak_speed_km_s);
    out << std::format("  dv {:.0f} km/s (incl. {:.0f} velocity match)\n", p.delta_v_km_s, p.match_delta_v_km_s);
    out << std::format("  reaction mass {:.1f} t of {:.1f} t aboard -> {:.1f} t left (", p.reaction_mass_needed_t,
                       p.reaction_mass_aboard_t, p.reaction_mass_after_t)
        << level(p.tank_after_pct, 20.0, 5.0) << " of tank)\n";
    out << std::format("  hull wear ~{:.1f}%, light-lag {}\n", p.hull_wear_pct, sim::format_duration(p.light_lag));
    if (p.feasible()) {
        out << "  " << styled(Style::good, "GO") << "\n";
    } else {
        out << "  " << styled(Style::bad, "NO GO:") << " " << p.reason << "\n";
    }
}

const std::vector<sim::ArgSpec> course_options = {
    {.name = "accel", .type = sim::ArgType::acceleration, .help = "burn acceleration",
     .default_value = "0.3g"},
    {.name = "maxdv", .type = sim::ArgType::number, .help = "dv budget in km/s (coast to save mass)"},
    {.name = "within", .type = sim::ArgType::duration,
     .help = "cheapest course arriving within this time"},
};

CommodityId commodity_arg(const Session& s, const Invocation& inv) {
    const auto& k = inv.get<std::string>("commodity");
    const CommodityId id = s.content->find<CommodityDef>(k);
    if (!id) {
        throw CommandError(std::format("no commodity '{}'", k));
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

void print_trade(Session& s, const economy::TradeResult& r, Doc& out) {
    if (!r.ok()) {
        throw CommandError(r.reason);
    }
    s.messages_seen = s.world->messages.size(); // the reason repeats the journal line
    out << r.reason << "\n";
    out << "cash now " << money(s.world->companies.at(s.world->player).cash) << "\n";
}

void crew_table(const World& w, const std::vector<CrewId>& ids, Doc& out) {
    sim::TextTable& t = out.table({{"id"}, {"name"}, {"role"}, {"skill", Align::right}, {"wage/wk", Align::right},
                               {"morale", Align::right}, {"background"}});
    for (const CrewId id : ids) {
        const CrewMember& m = w.crew.at(id);
        t.row({key(std::format("#{}", id.index)), m.name, std::string(crew::role_name(m.role)),
               std::format("{}", m.skill), money(m.wage_per_week), level(100.0 * m.morale, 40.0, 20.0),
               styled(Style::dim, m.background)});
    }
}

} // namespace

sim::Line journal_line(const Message& m, bool compact) {
    sim::Line line;
    if (compact) {
        line.append(styled(Style::dim, calendar::format_datetime(m.time).substr(5) + " "));
    } else {
        line.append({"  ", Style::plain});
        line.append(styled(Style::dim, std::format("[{}]", calendar::format_datetime(m.time))));
        line.append({std::format(" {:<7} ", kind_label(m.kind)),
                     m.kind == MessageKind::warning ? Style::warning : Style::plain});
    }
    if (m.urgent) {
        line.append(styled(Style::urgent, "! " + m.text));
    } else {
        line.append(styled(m.kind == MessageKind::warning ? Style::warning : Style::plain, m.text));
    }
    return line;
}

std::optional<StatusBanner> status_banner(const Session& s) {
    if (!s.world) {
        return std::nullopt;
    }
    const World& w = *s.world;
    const Content& c = *s.content;
    const Company& me = w.companies.at(w.player);
    StatusBanner b;
    b.company = me.name;
    b.now = w.now();
    b.cash = me.cash;
    for (auto [id, loan] : w.loans) {
        (void)id;
        if (loan.borrower != w.player || loan.balance <= 0 || (b.loan && b.loan->due <= loan.next_due)) {
            continue;
        }
        b.loan = StatusBanner::LoanStatus{
            .lender = c.table<StationDef>()[loan.lender].name,
            .balance = loan.balance,
            .instalment = instalment_outstanding(loan),
            .due = loan.next_due,
            .until_due = loan.next_due - w.now(),
            .missed = loan.missed_payments,
            .missed_limit = loan.missed_payment_limit,
        };
    }
    if (const auto ship_id = find_player_ship(w)) {
        const Ship& ship = w.ships.at(*ship_id);
        const ShipClassDef& cls = c.table<ShipClassDef>()[ship.ship_class];
        StatusBanner::ShipStatus st;
        st.name = ship.name;
        if (const auto* d = std::get_if<Docked>(&ship.location)) {
            st.docked_at = c.table<StationDef>()[d->station].name;
        } else {
            const auto& u = std::get<Underway>(ship.location);
            st.destination = c.table<StationDef>()[u.destination].name;
            st.arrival = u.arrival;
            st.remaining = u.arrival - w.now();
        }
        st.reaction_mass_pct = 100.0 * ship.reaction_mass_t / cls.reaction_mass_capacity_t;
        st.hull_pct = 100.0 * ship.hull_condition;
        st.crew = crew::aboard(w, *ship_id).size();
        st.berths = cls.crew_berths;
        b.ship = std::move(st);
    }
    if (w.game_over) {
        b.game_over = w.game_over->reason;
    }
    return b;
}

std::vector<sim::Line> banner_segments(const StatusBanner& b) {
    auto label = [](std::string text) { return styled(Style::dim, std::move(text) + " "); };
    std::vector<sim::Line> out;
    out.emplace_back(styled(Style::emphasis, calendar::format_datetime(b.now)));
    out.push_back(std::vector<sim::Span>{label("Cash"), money_delta(b.cash)});
    if (b.loan) {
        const auto& l = *b.loan;
        const bool short_of_cash = b.cash < l.instalment;
        sim::Line seg(std::vector<sim::Span>{
            label("Loan"), styled(short_of_cash ? Style::negative : Style::money, format_credits(l.instalment)),
            {" due " + calendar::format_datetime(l.due).substr(5, 5), Style::plain},
            styled(l.until_due < sim::days(2) ? Style::warning : Style::dim,
                   std::format(" in {}", short_duration(l.until_due)))});
        if (l.missed > 0) {
            seg.append({" ", Style::plain});
            seg.append(styled(Style::urgent, std::format("{}/{} missed", l.missed, l.missed_limit)));
        }
        out.push_back(std::move(seg));
    }
    if (b.ship) {
        const auto& s = *b.ship;
        if (s.docked_at) {
            out.push_back(std::vector<sim::Span>{label(s.name), {"docked at " + *s.docked_at, Style::plain}});
        } else {
            out.push_back(std::vector<sim::Span>{
                label(s.name), {"-> " + s.destination.value_or("?"), Style::plain},
                styled(Style::dim, std::format(" ETA {} in {}", calendar::format_datetime(s.arrival).substr(5),
                                               short_duration(s.remaining)))});
        }
        out.push_back(std::vector<sim::Span>{label("RM"), level(s.reaction_mass_pct, 25.0, 10.0)});
        out.push_back(std::vector<sim::Span>{label("Hull"), level(s.hull_pct, 50.0, 25.0)});
        out.push_back(std::vector<sim::Span>{
            label("Crew"), {std::format("{}/{}", s.crew, s.berths), Style::plain}});
    }
    if (b.game_over) {
        out.emplace_back(styled(Style::urgent, "GAME OVER: " + *b.game_over));
    }
    return out;
}

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
        [](Session& s, const Invocation& inv, Doc& out) {
            const auto& k = inv.get<std::string>("scenario");
            if (!s.content->find<ScenarioDef>(k)) {
                throw CommandError(std::format("unknown scenario '{}'", k));
            }
            s.last_plot.reset();
            s.world = new_game(*s.content, k, static_cast<std::uint64_t>(inv.get<std::int64_t>("seed")));
            s.messages_seen = 0;
            const ScenarioDef& sc = s.content->table<ScenarioDef>()[s.content->find<ScenarioDef>(k)];
            out.heading(std::format("== {} ==", sc.name));
            out << sc.description << "\n\n";
            out << calendar::format_datetime(s.world->now()) << " — type '" << key("status")
                << "' to look around.\n";
        });

    bus.add_query({.name = "save",
                   .summary = "Save the game to a file",
                   .positionals = {{.name = "path", .help = "file to write"}}},
                  [](const Session& s, const Invocation& inv, Doc& out) {
                      const World& w = require_world(s);
                      const auto& path = inv.get<std::string>("path");
                      const auto bytes = save_world(w);
                      std::ofstream f(path, std::ios::binary);
                      f.write(reinterpret_cast<const char*>(bytes.data()),
                              static_cast<std::streamsize>(bytes.size()));
                      if (!f.flush()) {
                          throw CommandError(std::format("failed writing '{}'", path));
                      }
                      out << std::format("saved {} bytes to {} ", bytes.size(), path)
                          << styled(Style::dim, std::format("(state hash {:016x})", world_hash(w))) << "\n";
                  });

    bus.add_action({.name = "load",
                    .summary = "Load a saved game",
                    .details = "Replays of sessions that use 'load' depend on the save file.",
                    .positionals = {{.name = "path", .help = "file to read"}}},
                   [](Session& s, const Invocation& inv, Doc& out) {
                       const auto& path = inv.get<std::string>("path");
                       std::ifstream f(path, std::ios::binary);
                       if (!f) {
                           throw CommandError(std::format("cannot open '{}'", path));
                       }
                       const std::vector<std::uint8_t> bytes{std::istreambuf_iterator<char>(f), {}};
                       try {
                           s.world = load_world(bytes, *s.content);
                           s.last_plot.reset();
                       } catch (const sim::SerializeError& e) {
                           throw CommandError(std::format("cannot load '{}': {}", path, e.what()));
                       }
                       s.messages_seen = s.world->messages.size();
                       out << std::format("loaded {} — {}\n", path, calendar::format_datetime(s.world->now()));
                   });

    bus.add_query({.name = "hash", .summary = "Print the world state hash (for replay checks)"},
                  [](const Session& s, const Invocation&, Doc& out) {
                      out << std::format("{:016x}\n", world_hash(require_world(s)));
                  });

    // --- time ---

    bus.add_query({.name = "date", .summary = "Show the current date and time"},
                  [](const Session& s, const Invocation&, Doc& out) {
                      out << calendar::format_datetime(require_world(s).now()) << "\n";
                  });

    bus.add_action({.name = "advance",
                    .aliases = {"adv"},
                    .summary = "Let time pass (stops early if something needs your attention)",
                    .positionals = {{.name = "span", .type = sim::ArgType::duration,
                                     .help = "how long, e.g. 6h, 3d, 2w"}},
                    .options = {{.name = "force", .type = sim::ArgType::flag,
                                 .help = "don't stop for urgent events"}}},
                   [](Session& s, const Invocation& inv, Doc& out) {
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
                   [](Session& s, const Invocation& inv, Doc& out) {
                       const World& w = require_world(s);
                       advance_and_report(s, w.now() + inv.get<sim::Duration>("max"), true, out);
                   });

    // --- looking around ---

    bus.add_query({.name = "messages",
                   .aliases = {"msgs"},
                   .summary = "Show recent messages",
                   .options = {{.name = "last", .type = sim::ArgType::integer,
                                .help = "how many", .default_value = "10"}}},
                  [](const Session& s, const Invocation& inv, Doc& out) {
                      const World& w = require_world(s);
                      const auto n = static_cast<std::size_t>(std::max<std::int64_t>(0, inv.get<std::int64_t>("last")));
                      const std::size_t first = w.messages.size() > n ? w.messages.size() - n : 0;
                      if (first == w.messages.size()) {
                          out << "  " << styled(Style::dim, "(no messages)") << "\n";
                      }
                      for (std::size_t i = first; i < w.messages.size(); ++i) {
                          out << journal_line(w.messages[i]) << "\n";
                      }
                  });

    bus.add_query(
        {.name = "status", .aliases = {"st"}, .summary = "Your company, ships and debts"},
        [](const Session& s, const Invocation&, Doc& out) {
            const World& w = require_world(s);
            const Content& c = *s.content;
            const Company& me = w.companies.at(w.player);
            out.heading(std::format("{} — {}", me.name, calendar::format_datetime(w.now())));
            sim::TextTable& t = out.table({{}, {}});
            t.row({"cash", money_delta(me.cash)});
            for (auto [id, loan] : w.loans) {
                (void)id;
                if (loan.borrower != w.player) {
                    continue;
                }
                sim::Line text(std::vector<sim::Span>{
                    money(loan.balance),
                    {std::format(" owed to {}, ", c.table<StationDef>()[loan.lender].name), Style::plain},
                    money(instalment_outstanding(loan)),
                    {std::format(" due {}", calendar::format_date(loan.next_due)), Style::plain}});
                if (interest_only(loan)) {
                    text.append({std::format(" (interest only; {} from {})", format_credits(loan.weekly_payment),
                                             calendar::format_date(first_regular_due(loan))),
                                 Style::dim});
                }
                if (loan.missed_payments > 0) {
                    text.append({" ", Style::plain});
                    text.append(styled(Style::urgent, std::format("({} missed!)", loan.missed_payments)));
                }
                t.row({"loan", std::move(text)});
            }
            for (auto [id, ship] : w.ships) {
                if (ship.owner != w.player) {
                    continue;
                }
                const ShipClassDef& cls = c.table<ShipClassDef>()[ship.ship_class];
                t.row({"ship", sim::Line(std::vector<sim::Span>{styled(Style::emphasis, ship.name),
                                                                {" (" + cls.name + ")", Style::plain}})});
                t.row({"", location_text(c, w, ship)});
                const double rm_pct = 100.0 * ship.reaction_mass_t / cls.reaction_mass_capacity_t;
                t.row({"", sim::Line(std::vector<sim::Span>{
                               {std::format("reaction mass {:.0f}/{:.0f} t (", ship.reaction_mass_t,
                                            cls.reaction_mass_capacity_t),
                                Style::plain},
                               level(rm_pct, 25.0, 10.0),
                               {"), hull ", Style::plain},
                               level(100.0 * ship.hull_condition, 50.0, 25.0)})});
                std::string cargo = std::format("cargo {:.0f}/{:.0f} t", cargo_mass_t(ship), cls.cargo_capacity_t);
                for (const CargoLot& lot : ship.cargo) {
                    const std::string& what = c.table<CommodityDef>()[lot.commodity].name;
                    cargo += lot.tonnes < 10.0 ? std::format(", {:.2f} t {}", lot.tonnes, what)
                                               : std::format(", {:.0f} t {}", lot.tonnes, what);
                }
                t.row({"", cargo});
                t.row({"", std::format("crew {}/{}", crew::aboard(w, id).size(), cls.crew_berths)});
            }
        });

    bus.add_query(
        {.name = "stations",
         .summary = "List stations with distance and light-lag from your ship"},
        [](const Session& s, const Invocation&, Doc& out) {
            const World& w = require_world(s);
            const Content& c = *s.content;
            std::optional<Vec3> here;
            if (const auto ship = find_player_ship(w)) {
                here = ship_position(c, w, w.ships.at(*ship));
            }
            sim::TextTable& t = out.table({{"key"}, {"name"}, {"faction"}, {"dist AU", Align::right},
                                       {"light-lag", Align::right}});
            for (auto [id, st] : c.table<StationDef>()) {
                const Vec3 p = c.orbits().world_position(c.orbit_of(id), w.now());
                const double d = here ? distance(*here, p) : 0.0;
                const auto lag = sim::seconds(static_cast<std::int64_t>(d / 299'792'458.0));
                constexpr std::array factions{"earth", "mars", "belt", "independent"};
                t.row({key(std::string(c.table<StationDef>().key(id))), st.name,
                       factions[static_cast<std::size_t>(st.faction)], std::format("{:.2f}", units::to_au(d)),
                       sim::format_duration(lag)});
            }
        });

    bus.add_query(
        {.name = "station",
         .summary = "Describe a station and its stockpiles",
         .positionals = {{.name = "key", .help = "station key (see 'stations')",
                          .completer = keys_of<StationDef>(content)}}},
        [](const Session& s, const Invocation& inv, Doc& out) {
            const World& w = require_world(s);
            const Content& c = *s.content;
            const auto id = c.find<StationDef>(inv.get<std::string>("key"));
            if (!id) {
                throw CommandError(std::format("no station '{}'", inv.get<std::string>("key")));
            }
            const StationDef& st = c.table<StationDef>()[id];
            out.heading(std::format("{} — pop. {}, docking {}/day", st.name, st.population,
                                    format_credits(st.docking_fee)));
            const StationState& state = w.stations[index_of(id)];
            sim::TextTable& t = out.table({{"commodity"}, {"stock", Align::right}, {"made/day", Align::right},
                                       {"used/day", Align::right}});
            for (const MarketEntryDef& m : st.market) {
                t.row({c.table<CommodityDef>()[m.commodity].name,
                       std::format("{:.0f} t", state.stock[index_of(m.commodity)]),
                       std::format("+{:.0f} t", m.production), std::format("-{:.0f} t", m.consumption)});
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
                  [](const Session& s, const Invocation& inv, Doc& out) {
                      CoursePreview p = course_for(s, inv);
                      print_preview(*s.content, p, out);
                      if (p.feasible()) { // an infeasible preview has no course to draw
                          s.last_plot = std::move(p);
                      } else {
                          s.last_plot.reset();
                      }
                  });

    bus.add_query({.name = "routes",
                   .summary = "Fastest course to every station at an acceleration",
                   .options = {course_options[0]}},
                  [](const Session& s, const Invocation& inv, Doc& out) {
                      const World& w = require_world(s);
                      const auto all = plot_all_destinations(
                          *s.content, w, player_ship(w), {inv.get<sim::Acceleration>("accel").gees()});
                      sim::TextTable& t = out.table({{"destination"}, {"AU", Align::right}, {"time", Align::right},
                                                 {"rmass t", Align::right}, {"tank left", Align::right}, {""}});
                      for (const CoursePreview& p : all) {
                          const bool go = p.feasible();
                          const std::string& name = s.content->table<StationDef>()[p.destination].name;
                          t.row({go ? sim::Line(name) : sim::Line(styled(Style::dim, name)),
                                 std::format("{:.2f}", p.distance_au), sim::format_duration(p.duration),
                                 std::format("{:.1f}", p.reaction_mass_needed_t), level(p.tank_after_pct, 20.0, 5.0),
                                 go ? sim::Line() : sim::Line(styled(Style::bad, p.reason))});
                      }
                  });

    bus.add_action({.name = "go",
                    .summary = "Undock and fly to a station (same options as plot)",
                    .positionals = {{.name = "station", .help = "destination key",
                                     .completer = keys_of<StationDef>(content)}},
                    .options = course_options},
                   [](Session& s, const Invocation& inv, Doc& out) {
                       World& w = require_playing(s);
                       const CoursePreview p = course_for(s, inv);
                       if (!p.feasible()) {
                           throw CommandError(p.reason);
                       }
                       // Fly exactly the previewed course: cap dv at the plan's figure.
                       s.last_plot.reset();
                       const DepartResult r = depart(*s.content, w, player_ship(w), p.destination,
                                                     {p.accel_g, p.delta_v_km_s * (1.0 + 1e-9)});
                       if (r.status != CourseStatus::ok) {
                           throw CommandError(r.reason);
                       }
                       flush_messages(s, out);
                       out << styled(Style::good, "Underway.")
                           << std::format(" ETA {} ({}). '", calendar::format_datetime(r.preview.arrival),
                                          sim::format_duration(r.preview.duration))
                           << key("wait") << "' to fly.\n";
                   });

    // --- money ---

    bus.add_query({.name = "books",
                   .summary = "Income and expenses over recent days",
                   .options = {{.name = "days", .type = sim::ArgType::integer, .help = "period",
                                .default_value = "30"},
                               {.name = "entries", .type = sim::ArgType::integer,
                                .help = "recent ledger lines to show", .default_value = "10"}}},
                  [](const Session& s, const Invocation& inv, Doc& out) {
                      const World& w = require_world(s);
                      const sim::Time to = w.now() + sim::seconds(1);
                      const sim::Time from = to - sim::days(std::max<std::int64_t>(1, inv.get<std::int64_t>("days")));
                      const LedgerSummary sum = summarize_ledger(w, w.player, from, to);
                      out.heading(std::format("Books since {}: opening {}, closing {}", calendar::format_date(from),
                                              format_credits(sum.opening_balance),
                                              format_credits(sum.closing_balance)));
                      sim::TextTable cats{.columns = {{"category"}, {"income", Align::right}, {"expenses", Align::right}}};
                      for (const auto& [name, cat] : enum_names(LedgerCategory{})) {
                          if (sum.income_of(cat) != 0 || sum.expense_of(cat) != 0) {
                              cats.row({std::string(name), money_delta(sum.income_of(cat)),
                                        money_delta(-sum.expense_of(cat))});
                          }
                      }
                      if (!cats.rows.empty()) {
                          out.table(cats.columns).rows = std::move(cats.rows);
                      }
                      out << "  net " << money_delta(sum.net()) << "\n";
                      std::vector<const LedgerEntry*> mine;
                      for (const LedgerEntry& e : ledger_between(w, from, to)) {
                          if (e.company == w.player) {
                              mine.push_back(&e);
                          }
                      }
                      const auto n = static_cast<std::size_t>(std::max<std::int64_t>(0, inv.get<std::int64_t>("entries")));
                      if (!mine.empty() && n > 0) {
                          sim::TextTable& t = out.table({{"date"}, {"amount", Align::right}, {"category"}, {"entry"}});
                          for (std::size_t i = mine.size() > n ? mine.size() - n : 0; i < mine.size(); ++i) {
                              const LedgerEntry& e = *mine[i];
                              t.row({styled(Style::dim, calendar::format_date(e.time)), money_delta(e.amount),
                                     std::string(to_string(e.category)), e.description});
                          }
                      }
                      const Credits tabs = total_dock_tabs(w, w.player);
                      if (tabs > 0) {
                          out << "  " << styled(Style::warning, "owed to dockmasters: " + format_credits(tabs)) << "\n";
                      }
                  });

    bus.add_action({.name = "pay",
                    .summary = "Pay toward your loan now (counts toward the next instalment)",
                    .positionals = {{.name = "amount", .type = sim::ArgType::integer, .help = "credits"}}},
                   [](Session& s, const Invocation& inv, Doc& out) {
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
                   [](Session& s, const Invocation&, Doc& out) {
                       World& w = require_playing(s);
                       const Ship& ship = w.ships.at(player_ship(w));
                       const auto* docked = std::get_if<Docked>(&ship.location);
                       if (docked == nullptr) {
                           throw CommandError("you're not docked");
                       }
                       const Credits paid = pay_dock_tab(*s.content, w, w.player, docked->station);
                       flush_messages(s, out);
                       out << "paid " << money(paid) << "; still owed here: "
                           << money(dock_tab(w, w.player, docked->station)) << "\n";
                   });

    // --- trade ---

    bus.add_query({.name = "market",
                   .summary = "Prices at your dock (or another station, as last known)",
                   .positionals = {{.name = "station", .help = "station key", .required = false,
                                    .completer = keys_of<StationDef>(content)}}},
                  [](const Session& s, const Invocation& inv, Doc& out) {
                      const World& w = require_world(s);
                      const StationId st = inv.has("station") ? station_arg(s, inv) : docked_station(w);
                      const StationDef& def = s.content->table<StationDef>()[st];
                      out.heading(def.name + " market");
                      sim::TextTable& t = out.table({{"key"}, {"commodity"}, {"stock", Align::right},
                                                 {"normal", Align::right}, {"buy at", Align::right},
                                                 {"sell at", Align::right}, {""}});
                      for (const MarketEntryDef& m : def.market) {
                          const auto q = economy::quote(*s.content, w, st, m.commodity);
                          // Scarce goods are dear, glutted goods cheap: flag both for traders.
                          const Style stock_style = q->stock < 0.5 * q->target   ? Style::negative
                                                    : q->stock > 1.5 * q->target ? Style::positive
                                                                                 : Style::plain;
                          t.row({key(std::string(s.content->table<CommodityDef>().key(m.commodity))),
                                 s.content->table<CommodityDef>()[m.commodity].name,
                                 styled(stock_style, std::format("{:.0f} t", q->stock)),
                                 std::format("{:.0f} t", q->target), money(static_cast<Credits>(std::lround(q->ask))),
                                 money(static_cast<Credits>(std::lround(q->bid))),
                                 q->disrupted_days ? sim::Line(styled(Style::warning, "supply disrupted"))
                                                   : sim::Line()});
                      }
                  });

    const sim::ArgSpec commodity_spec{.name = "commodity", .help = "commodity key",
                                      .completer = keys_of<CommodityDef>(content)};

    bus.add_action({.name = "buy",
                    .summary = "Buy cargo at your dock",
                    .positionals = {commodity_spec,
                                    {.name = "tonnes", .type = sim::ArgType::number, .help = "amount"}}},
                   [](Session& s, const Invocation& inv, Doc& out) {
                       World& w = require_playing(s);
                       print_trade(s, economy::buy(*s.content, w, player_ship(w), commodity_arg(s, inv),
                                                   inv.get<double>("tonnes")), out);
                   });

    bus.add_action({.name = "sell",
                    .summary = "Sell cargo at your dock",
                    .positionals = {commodity_spec,
                                    {.name = "tonnes", .type = sim::ArgType::number,
                                     .help = "amount (default: all aboard)", .required = false}}},
                   [](Session& s, const Invocation& inv, Doc& out) {
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
                           out << "profit on cost " << money_delta(r.profit) << "\n";
                       }
                   });

    bus.add_action({.name = "refuel",
                    .summary = "Buy water as reaction mass (default: fill what you can afford)",
                    .positionals = {{.name = "tonnes", .type = sim::ArgType::number, .help = "amount",
                                     .required = false}}},
                   [](Session& s, const Invocation& inv, Doc& out) {
                       World& w = require_playing(s);
                       const std::optional<double> t =
                           inv.has("tonnes") ? std::optional{inv.get<double>("tonnes")} : std::nullopt;
                       print_trade(s, economy::refuel(*s.content, w, player_ship(w), t), out);
                   });

    // --- crew ---

    bus.add_query({.name = "crew", .summary = "Who's aboard, supplies, and who's looking for work here"},
                  [](const Session& s, const Invocation&, Doc& out) {
                      const World& w = require_world(s);
                      const Content& c = *s.content;
                      const ShipId ship = player_ship(w);
                      out.heading("Aboard:");
                      crew_table(w, crew::aboard(w, ship), out);
                      const Ship& sh = w.ships.at(ship);
                      const crew::Provisions have = crew::stores(c, sh);
                      out << "  payroll " << money(crew::weekly_payroll(w, ship))
                          << std::format("/week; stores: water {:.2f} t, food {:.2f} t, oxygen {:.2f} t\n",
                                         have.water_t, have.food_t, have.oxygen_t);
                      if (const auto* d = std::get_if<Docked>(&sh.location)) {
                          out.heading(std::format("Looking for work at {}:", c.table<StationDef>()[d->station].name));
                          const auto pool = crew::pool_at(w, d->station);
                          if (pool.empty()) {
                              out << "  " << styled(Style::dim, "(nobody)") << "\n";
                          } else {
                              crew_table(w, pool, out);
                          }
                      }
                  });

    bus.add_action({.name = "hire",
                    .summary = "Sign on someone from the dock (pays a week's wage up front)",
                    .positionals = {{.name = "id", .type = sim::ArgType::integer, .help = "#id from 'crew'"}}},
                   [](Session& s, const Invocation& inv, Doc& out) {
                       World& w = require_playing(s);
                       const CrewId id = crew_arg(w, inv);
                       const crew::HireResult r = crew::hire(*s.content, w, player_ship(w), id);
                       if (r.status != crew::HireStatus::ok) {
                           throw CommandError(r.reason);
                       }
                       flush_messages(s, out);
                       out << styled(Style::emphasis, w.crew.at(id).name) << " signs on.\n";
                   });

    bus.add_action({.name = "fire",
                    .summary = "Put a crew member ashore at this dock",
                    .positionals = {{.name = "id", .type = sim::ArgType::integer, .help = "#id from 'crew'"}}},
                   [](Session& s, const Invocation& inv, Doc& out) {
                       World& w = require_playing(s);
                       const CrewId id = crew_arg(w, inv);
                       const std::string name = w.crew.at(id).name;
                       const crew::FireResult r = crew::fire(w, id);
                       if (r.status != crew::FireStatus::ok) {
                           throw CommandError(r.reason);
                       }
                       flush_messages(s, out);
                       out << styled(Style::emphasis, name) << " goes ashore.\n";
                   });
}

} // namespace expanse
