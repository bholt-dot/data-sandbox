#include "info.hpp"

#include "expanse/calendar.hpp"
#include "expanse/contracts.hpp"
#include "expanse/economy.hpp"
#include "expanse/finance.hpp"
#include "expanse/ships.hpp"

#include <algorithm>
#include <cmath>
#include <format>
#include <map>
#include <utility>

namespace viewer {

namespace {

using expanse::units::au_m;
using Kind = InfoLine::Kind;

std::string_view faction_name(expanse::Faction faction) {
    switch (faction) {
    case expanse::Faction::earth:
        return "Earth";
    case expanse::Faction::mars:
        return "Mars";
    case expanse::Faction::belt:
        return "Belt";
    case expanse::Faction::independent:
        break;
    }
    return "Independent";
}

expanse::StationId station_id(const expanse::Content& content, std::uint32_t index) {
    return content.table<expanse::StationDef>().table().handle_at(index);
}

expanse::ShipId ship_id(ObjectRef ref) {
    expanse::ShipId id;
    id.index = ref.index;
    id.generation = ref.generation;
    return id;
}

struct Builder {
    InfoPanel panel;

    void add(Kind kind, std::vector<std::string> cells, Tone tone = Tone::normal) {
        panel.lines.push_back({kind, std::move(cells), tone});
    }
    void pair(std::string key, std::string value, Tone tone = Tone::normal) {
        add(Kind::pair, {std::move(key), std::move(value)}, tone);
    }
};

std::string percent(double fraction_pct) { return std::format("{:.0f}%", fraction_pct); }

std::string tonnes(double t) {
    if (t > 0.0 && t < 0.01) {
        return "< 0.01 t";
    }
    if (t > 0.0 && t < 1.0) {
        return std::format("{:.2f} t", t);
    }
    if (t > 0.0 && t < 10.0) {
        return std::format("{:.1f} t", t);
    }
    return group_digits(std::llround(t)) + " t";
}

// "0.12 AU · 1m 0s light" from the reference ship, or nothing when there is no other end.
void distance_from_ship(Builder& b, const Scene& scene, const SceneObject& object) {
    const SceneObject* ship = reference_ship(scene);
    if (ship == nullptr || ship->ref == object.ref) {
        return;
    }
    const double d = expanse::distance(ship->position, object.position);
    if (d < 1.0e6) {
        b.pair("From your ship", "here", Tone::good);
        return;
    }
    b.pair("From your ship", std::format("{} · {} light", format_distance(d), format_light_lag(expanse::light_lag(d))));
}

void body_info(Builder& b, const ViewSnapshot& snap, const Scene& scene, const SceneObject& object) {
    const expanse::Content& content = *snap.content;
    distance_from_ship(b, scene, object);
    if (object.cls != ObjectClass::star) {
        b.pair("From the Sun", format_distance(expanse::length(object.position)));
    }
    b.pair("Radius", group_digits(std::llround(object.radius_m / 1000.0)) + " km");
    std::string ports;
    for (auto [id, station] : content.table<expanse::StationDef>()) {
        if (station.body.index == object.ref.index) {
            ports += ports.empty() ? station.name : ", " + station.name;
        }
    }
    if (!ports.empty() && object.cls != ObjectClass::star) {
        b.pair("Stations", ports);
    }
}

void market_table(Builder& b, const ViewSnapshot& snap, expanse::StationId station) {
    const expanse::Content& content = *snap.content;
    const auto& commodities = content.table<expanse::CommodityDef>();
    struct Row {
        double deviation;
        std::string key;
        expanse::economy::MarketQuote quote;
    };
    std::vector<Row> rows;
    for (const expanse::MarketEntryDef& entry : content.table<expanse::StationDef>()[station].market) {
        const auto q = expanse::economy::quote(content, *snap.world, station, entry.commodity);
        const double base = static_cast<double>(commodities[entry.commodity].base_price);
        if (!q || base <= 0.0 || q->mid <= 0.0) {
            continue;
        }
        rows.push_back({std::abs(std::log(q->mid / base)), commodities.key(entry.commodity), *q});
    }
    if (rows.empty()) {
        b.add(Kind::text, {"No market"}, Tone::dim);
        return;
    }
    // Furthest from the base price first: the bargains and the shortages are what a trader
    // looks for. Key order breaks ties, so the table never depends on anything but the data.
    std::sort(rows.begin(), rows.end(), [](const Row& x, const Row& y) {
        return x.deviation != y.deviation ? x.deviation > y.deviation : x.key < y.key;
    });
    b.add(Kind::section, {"Market"});
    b.add(Kind::table, {"Commodity", "Stock", "Ask", "Bid"}, Tone::dim);
    const std::size_t shown = std::min(rows.size(), info_market_rows);
    for (std::size_t i = 0; i < shown; ++i) {
        const auto& q = rows[i].quote;
        const double base = static_cast<double>(commodities[q.commodity].base_price);
        Tone tone = Tone::normal;
        if (q.stock < 0.5) {
            tone = Tone::bad; // sold out
        } else if (q.mid < 0.8 * base) {
            tone = Tone::good; // cheap here
        } else if (q.mid > 1.25 * base) {
            tone = Tone::warning; // dear here: sells well
        }
        b.add(Kind::table,
              {commodities[q.commodity].name, tonnes(q.stock), group_digits(std::llround(q.ask)),
               group_digits(std::llround(q.bid))},
              tone);
    }
    if (rows.size() > shown) {
        b.add(Kind::text, {std::format("+{} more (prices in cr/t)", rows.size() - shown)}, Tone::dim);
    } else {
        b.add(Kind::text, {"prices in cr/t"}, Tone::dim);
    }
}

void station_info(Builder& b, const ViewSnapshot& snap, const Scene& scene, const SceneObject& object) {
    const expanse::Content& content = *snap.content;
    const expanse::StationId id = station_id(content, object.ref.index);
    const expanse::StationDef& def = content.table<expanse::StationDef>()[id];
    distance_from_ship(b, scene, object);
    b.pair("Population", group_digits(def.population));
    if (!snap.world) {
        b.pair("Docking", expanse::format_credits(def.docking_fee) + " / day");
        return;
    }
    const expanse::World& world = *snap.world;
    const expanse::Credits tab = expanse::dock_tab(world, world.player, id);
    if (tab > 0) {
        b.pair("Docking", std::format("{} / day · tab {}", expanse::format_credits(def.docking_fee),
                                      expanse::format_credits(tab)),
               Tone::warning);
    } else {
        b.pair("Docking", expanse::format_credits(def.docking_fee) + " / day");
    }
    const std::size_t offers = expanse::contracts::board_at(world, id).size();
    if (expanse::contracts::board_capacity(def.population) > 0) {
        b.pair("Job board", offers == 1 ? "1 offer" : std::format("{} offers", offers), offers > 0 ? Tone::good : Tone::dim);
    }
    int to_here = 0;
    int from_here = 0;
    for (const expanse::ContractId cid : expanse::contracts::held_by(world, world.player)) {
        const expanse::Contract& c = world.contracts.at(cid);
        to_here += c.destination == id ? 1 : 0;
        from_here += c.origin == id ? 1 : 0;
    }
    if (to_here + from_here > 0) {
        std::string s;
        if (to_here > 0) {
            s = std::format("{} to here", to_here);
        }
        if (from_here > 0) {
            s += std::format("{}{} from here", s.empty() ? "" : ", ", from_here);
        }
        b.pair("Your contracts", s, Tone::good);
    } else {
        b.pair("Your contracts", "none here", Tone::dim);
    }
    market_table(b, snap, id);
}

void ship_info(Builder& b, const ViewSnapshot& snap, const Scene& scene, const SceneObject& object) {
    if (!snap.world) {
        return;
    }
    const expanse::Content& content = *snap.content;
    const expanse::World& world = *snap.world;
    const expanse::ShipId id = ship_id(object.ref);
    const expanse::Ship* ship = world.ships.get(id);
    const expanse::ShipStatus st = expanse::ship_status(content, world, id);
    if (ship == nullptr || !st.valid) {
        return;
    }
    distance_from_ship(b, scene, object);
    b.pair("Status", st.location);
    if (st.eta) {
        b.pair("ETA", std::format("{} (in {})", expanse::calendar::format_datetime(*st.eta),
                                  expanse::format_trip(st.time_remaining)));
        b.pair("Speed", std::format("{} km/s · {} to go", group_digits(std::llround(st.speed_km_s)),
                                    format_distance(st.distance_to_go_au * au_m)));
    }
    b.pair("Reaction mass", std::format("{} · {}", percent(st.reaction_mass_pct), tonnes(st.reaction_mass_t)),
           st.reaction_mass_pct < 25.0 ? Tone::warning : Tone::normal);
    b.pair("Hull", percent(st.hull_pct), st.hull_pct < expanse::hull_warning_level * 100.0 ? Tone::bad : Tone::normal);
    b.pair("Cargo", std::format("{} / {}", tonnes(st.cargo_t), tonnes(st.cargo_capacity_t)));
    if (!object.player) {
        return;
    }
    if (const expanse::Company* company = world.companies.get(world.player)) {
        b.pair("Cash", expanse::format_credits(company->cash));
    }

    // Own cargo by commodity (lots of one good merge), largest first.
    const auto& commodities = content.table<expanse::CommodityDef>();
    std::map<std::uint32_t, double> by_commodity;
    for (const expanse::CargoLot& lot : ship->cargo) {
        by_commodity[lot.commodity.index] += lot.tonnes;
    }
    std::vector<std::pair<double, std::uint32_t>> lots;
    for (const auto& [index, t] : by_commodity) {
        lots.emplace_back(t, index);
    }
    std::sort(lots.begin(), lots.end(), [](const auto& x, const auto& y) {
        return x.first != y.first ? x.first > y.first : x.second < y.second;
    });
    if (!lots.empty()) {
        b.add(Kind::section, {"Hold"});
        for (const auto& [t, index] : lots) {
            b.add(Kind::table, {commodities[commodities.table().handle_at(index)].name, tonnes(t)});
        }
    }

    std::vector<const expanse::Contract*> jobs;
    for (const expanse::ContractId cid : expanse::contracts::held_by(world, world.player)) {
        const expanse::Contract& c = world.contracts.at(cid);
        if (c.ship == id) {
            jobs.push_back(&c);
        }
    }
    if (!jobs.empty()) {
        b.add(Kind::section, {"Contracts aboard"});
        for (const expanse::Contract* c : jobs) {
            const Tone tone = c->deadline < world.now() ? Tone::bad
                              : c->deadline < world.now() + sim::days(1) ? Tone::warning
                                                                         : Tone::normal;
            b.add(Kind::text,
                  {std::format("{} · due {}", expanse::contracts::describe(content, *c),
                               expanse::calendar::format_datetime(c->deadline))},
                  tone);
        }
    }
    if (const std::uint32_t pax = expanse::contracts::passengers_aboard(world, id); pax > 0) {
        b.pair("Passengers", std::to_string(pax));
    }
}

} // namespace

const SceneObject* reference_ship(const Scene& scene) {
    if (scene.focus_ship) {
        if (const SceneObject* o = scene.find(*scene.focus_ship)) {
            return o;
        }
    }
    for (const SceneObject& o : scene.objects) {
        if (o.cls == ObjectClass::ship && o.player) {
            return &o;
        }
    }
    return nullptr;
}

std::string describe_object(const ViewSnapshot& snapshot, const Scene& scene, const SceneObject& object) {
    const SceneObject* host = object.host ? scene.find(*object.host) : nullptr;
    switch (object.cls) {
    case ObjectClass::star:
        return "Star";
    case ObjectClass::planet:
        return "Planet";
    case ObjectClass::dwarf_planet:
        return "Dwarf planet";
    case ObjectClass::moon:
        return host != nullptr ? "Moon of " + host->name : "Moon";
    case ObjectClass::asteroid:
        return "Asteroid";
    case ObjectClass::station: {
        const auto& def = snapshot.content->table<expanse::StationDef>()[station_id(*snapshot.content, object.ref.index)];
        const std::string kind = std::format("{} station", faction_name(def.faction));
        return host != nullptr ? std::format("{} at {}", kind, host->name) : kind + " in solar orbit";
    }
    case ObjectClass::ship:
        break;
    }
    if (object.player) {
        return "Your ship";
    }
    if (snapshot.world) {
        if (const expanse::Ship* ship = snapshot.world->ships.get(ship_id(object.ref))) {
            if (const expanse::Company* owner = snapshot.world->companies.get(ship->owner)) {
                return "Ship of " + owner->name;
            }
        }
    }
    return "Ship";
}

InfoPanel object_info(const ViewSnapshot& snapshot, const Scene& scene, ObjectRef ref) {
    Builder b;
    const SceneObject* object = scene.find(ref);
    if (object == nullptr) {
        return {};
    }
    b.add(Kind::title, {object->name});
    std::string subtitle = describe_object(snapshot, scene, *object);
    if (object->cls == ObjectClass::ship && snapshot.world) {
        if (const expanse::Ship* ship = snapshot.world->ships.get(ship_id(ref))) {
            subtitle += " · " + snapshot.content->table<expanse::ShipClassDef>()[ship->ship_class].name;
        }
    }
    b.add(Kind::subtitle, {std::move(subtitle)}, Tone::dim);
    switch (ref.kind) {
    case ObjectKind::body:
        body_info(b, snapshot, scene, *object);
        break;
    case ObjectKind::station:
        station_info(b, snapshot, scene, *object);
        break;
    case ObjectKind::ship:
        ship_info(b, snapshot, scene, *object);
        break;
    }
    return std::move(b.panel);
}

Tooltip tooltip(const ViewSnapshot& snapshot, const Scene& scene, ObjectRef ref) {
    const SceneObject* object = scene.find(ref);
    if (object == nullptr) {
        return {};
    }
    Tooltip t{object->name, describe_object(snapshot, scene, *object)};
    if (const SceneObject* ship = reference_ship(scene); ship != nullptr && ship->ref != ref) {
        const double d = expanse::distance(ship->position, object->position);
        t.detail += d < 1.0e6 ? std::string(" · your ship is here") : " · " + format_distance(d);
    }
    return t;
}

std::string group_digits(std::int64_t value) {
    std::string digits = std::to_string(value < 0 ? -value : value);
    for (std::size_t i = digits.size(); i > 3; i -= 3) {
        digits.insert(i - 3, ",");
    }
    return value < 0 ? "-" + digits : digits;
}

std::string format_distance(double metres) {
    const double au = metres / au_m;
    if (au >= 10.0) {
        return std::format("{:.1f} AU", au);
    }
    if (au >= 0.1) {
        return std::format("{:.2f} AU", au);
    }
    if (au >= 0.01) {
        return std::format("{:.3f} AU", au);
    }
    const double km = metres / 1000.0;
    if (km >= 10'000.0) { // three significant figures
        const double step = std::pow(10.0, std::floor(std::log10(km)) - 2.0);
        return group_digits(std::llround(std::round(km / step) * step)) + " km";
    }
    return group_digits(std::llround(km)) + " km";
}

std::string format_light_lag(sim::Duration lag) {
    const std::int64_t s = lag.seconds;
    if (s < 1) {
        return "< 1 s";
    }
    if (s < 60) {
        return std::format("{} s", s);
    }
    if (s < 3600) {
        return std::format("{}m {}s", s / 60, s % 60);
    }
    return std::format("{}h {}m", s / 3600, (s % 3600) / 60);
}

std::string course_label(const SceneCourse& course) {
    std::string label = course.destination.empty() ? "" : course.destination + " · ";
    label += std::format("arr {} · Δv {} km/s", expanse::calendar::format_datetime(course.arrival),
                         group_digits(std::llround(course.delta_v_km_s)));
    if (!course.feasible) {
        label += " · not feasible";
    }
    return label;
}

ScaleBar choose_scale_bar(double metres_per_px, double max_px) {
    const double max_m = metres_per_px * max_px;
    const bool in_au = max_m >= 0.01 * au_m;
    const double unit_m = in_au ? au_m : 1000.0;
    const double max_units = std::max(max_m / unit_m, 1e-9);
    const double exponent = std::floor(std::log10(max_units));
    const double decade = std::pow(10.0, exponent);
    double units = decade;
    for (const double step : {2.0, 5.0}) {
        if (step * decade <= max_units * (1.0 + 1e-9)) {
            units = step * decade;
        }
    }
    // Round away representation noise (5 * 0.01 -> 0.05) before printing the shortest form.
    const double digits = std::max(0.0, -exponent);
    const double scale = std::pow(10.0, digits);
    units = std::round(units * scale) / scale;
    ScaleBar bar;
    bar.length_m = units * unit_m;
    bar.length_px = bar.length_m / metres_per_px;
    bar.label = in_au ? std::format("{} AU", units) : group_digits(std::llround(units)) + " km";
    if (!in_au && units < 1.0) {
        bar.label = std::format("{} km", units);
    }
    return bar;
}

} // namespace viewer
