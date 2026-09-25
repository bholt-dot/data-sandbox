#include "expanse/content.hpp"

#include "expanse/calendar.hpp"
#include "expanse/units.hpp"
#include "simcore/hash.hpp"

#include <cmath>
#include <format>

namespace expanse {

namespace {

std::string positive(double v) { return v > 0.0 ? std::string{} : "must be > 0"; }

} // namespace

void CommodityDef::describe(sim::Schema<CommodityDef>& s) {
    s.field("name", &CommodityDef::name).non_empty();
    s.field("category", &CommodityDef::category);
    s.field("base_price", &CommodityDef::base_price).min(Credits{1});
}

void OrbitDef::describe(sim::Schema<OrbitDef>& s) {
    s.field("a_au", &OrbitDef::a_au).check(positive);
    s.field("a_km", &OrbitDef::a_km).check(positive);
    s.optional("e", &OrbitDef::e).check([](double e) {
        return e >= 0.0 && e < 1.0 ? std::string{} : "must be in [0, 1) (only elliptic orbits)";
    });
    s.optional("i_deg", &OrbitDef::i_deg).range(-180.0, 180.0);
    s.optional("node_deg", &OrbitDef::node_deg).range(-360.0, 360.0);
    s.optional("peri_deg", &OrbitDef::peri_deg).range(-360.0, 360.0);
    s.optional("mean_anomaly_deg", &OrbitDef::mean_anomaly_deg).range(-360.0, 360.0);
    s.check([](const OrbitDef& o) {
        return o.a_au.has_value() != o.a_km.has_value()
                   ? std::string{}
                   : "orbit needs exactly one of 'a_au' or 'a_km'";
    });
}

void BodyDef::describe(sim::Schema<BodyDef>& s) {
    s.field("name", &BodyDef::name).non_empty();
    s.field("kind", &BodyDef::kind);
    s.field("parent", &BodyDef::parent);
    s.optional("gm_km3_s2", &BodyDef::gm_km3_s2).min(0.0);
    s.field("radius_km", &BodyDef::radius_km).min(0.0);
    s.field("orbit", &BodyDef::orbit);
    s.check([](const BodyDef& b) {
        const bool star = b.kind == BodyKind::star;
        if (star && (b.parent || b.orbit)) {
            return std::string{"a star has no 'parent' or 'orbit'"};
        }
        if (!star && (!b.parent || !b.orbit)) {
            return std::string{"non-star bodies need both 'parent' and 'orbit'"};
        }
        return std::string{};
    });
}

void MarketEntryDef::describe(sim::Schema<MarketEntryDef>& s) {
    s.field("commodity", &MarketEntryDef::commodity);
    s.optional("production", &MarketEntryDef::production).min(0.0);
    s.optional("consumption", &MarketEntryDef::consumption).min(0.0);
    s.optional("stock", &MarketEntryDef::stock).min(0.0);
}

void StationDef::describe(sim::Schema<StationDef>& s) {
    s.field("name", &StationDef::name).non_empty();
    s.field("body", &StationDef::body);
    s.field("orbit", &StationDef::orbit);
    s.field("faction", &StationDef::faction);
    s.optional("population", &StationDef::population);
    s.optional("docking_fee", &StationDef::docking_fee).min(Credits{0});
    s.optional("market", &StationDef::market);
}

void ShipClassDef::describe(sim::Schema<ShipClassDef>& s) {
    s.field("name", &ShipClassDef::name).non_empty();
    s.field("dry_mass_t", &ShipClassDef::dry_mass_t).check(positive);
    s.field("cargo_capacity_t", &ShipClassDef::cargo_capacity_t).min(0.0);
    s.field("reaction_mass_capacity_t", &ShipClassDef::reaction_mass_capacity_t).check(positive);
    s.field("exhaust_velocity_km_s", &ShipClassDef::exhaust_velocity_km_s).check(positive);
    s.field("max_accel_g", &ShipClassDef::max_accel_g).check(positive);
    s.field("crew_berths", &ShipClassDef::crew_berths).min(std::uint8_t{1});
    s.field("price", &ShipClassDef::price).min(Credits{0});
}

void ScenarioDef::describe(sim::Schema<ScenarioDef>& s) {
    s.field("name", &ScenarioDef::name).non_empty();
    s.optional("description", &ScenarioDef::description);
    s.field("start_date", &ScenarioDef::start_date).check([](const std::string& d) {
        return calendar::parse_date(d) ? std::string{} : "expected a real date as YYYY-MM-DD";
    });
    s.field("start_station", &ScenarioDef::start_station);
    s.field("ship_class", &ScenarioDef::ship_class);
    s.field("ship_name", &ScenarioDef::ship_name).non_empty();
    s.field("company_name", &ScenarioDef::company_name).non_empty();
    s.field("cash", &ScenarioDef::cash);
    s.optional("reaction_mass_fraction", &ScenarioDef::reaction_mass_fraction).range(0.0, 1.0);
    s.optional("hull_condition", &ScenarioDef::hull_condition).range(0.0, 1.0);
    s.optional("loan_principal", &ScenarioDef::loan_principal).min(Credits{0});
    s.optional("loan_weekly_payment", &ScenarioDef::loan_weekly_payment).min(Credits{0});
    s.optional("loan_missed_payment_limit", &ScenarioDef::loan_missed_payment_limit)
        .min(std::uint8_t{1});
    s.optional("provision_days", &ScenarioDef::provision_days).min(0.0);
}

void CrewOriginDef::describe(sim::Schema<CrewOriginDef>& s) {
    s.field("name", &CrewOriginDef::name).non_empty();
    s.field("faction", &CrewOriginDef::faction);
    s.optional("wage_multiplier", &CrewOriginDef::wage_multiplier).check(positive);
    s.field("given_names", &CrewOriginDef::given_names).non_empty();
    s.field("family_names", &CrewOriginDef::family_names).non_empty();
    s.optional("backgrounds", &CrewOriginDef::backgrounds);
}

Content::Content() {
    defs_.define<CommodityDef>("commodity");
    defs_.define<BodyDef>("body");
    defs_.define<StationDef>("station");
    defs_.define<ShipClassDef>("ship_class");
    defs_.define<ScenarioDef>("scenario");
    defs_.define<CrewOriginDef>("crew_origin");
}

std::unique_ptr<Content> Content::load(const std::filesystem::path& dir, sim::Diagnostics& diags) {
    std::vector<sim::DataSource> sources = sim::read_data_directory(dir, diags);
    if (!diags.ok()) {
        return nullptr;
    }
    return load(sources, diags);
}

std::unique_ptr<Content> Content::load(std::span<const sim::DataSource> sources,
                                       sim::Diagnostics& diags) {
    std::unique_ptr<Content> c{new Content()};
    diags.append(c->defs_.load(sources));
    if (!diags.ok()) {
        return nullptr;
    }
    sim::Hasher h;
    for (const sim::DataSource& src : sources) {
        sim::encode(h, src.name);
        sim::encode(h, src.text);
    }
    c->fingerprint_ = h.digest();
    if (!c->build(diags)) {
        return nullptr;
    }
    return c;
}

namespace {

orbit::Elements to_elements(const OrbitDef& o, double parent_gm_km3_s2) {
    orbit::Elements el;
    el.semi_major_axis = o.a_au ? units::au(*o.a_au) : units::km(*o.a_km);
    el.eccentricity = o.e;
    el.inclination = units::deg(o.i_deg);
    el.lon_ascending_node = units::deg(o.node_deg);
    el.arg_periapsis = units::deg(o.peri_deg);
    el.mean_anomaly_at_epoch = units::deg(o.mean_anomaly_deg);
    el.epoch = calendar::j2000;
    el.mu = parent_gm_km3_s2 * 1e9;
    return el;
}

} // namespace

bool Content::build(sim::Diagnostics& diags) {
    const auto& bodies = table<BodyDef>();
    const auto& stations = table<StationDef>();
    const std::size_t errors_before = diags.size();

    for (auto [id, row] : bodies) {
        (void)row;
        if (id.index >= bodies.size()) {
            throw std::logic_error("expanse::Content: definition ids are not dense");
        }
    }

    // Bodies in parent-before-child order (OrbitSystem requirement); detects cycles.
    enum class Mark : std::uint8_t { none, visiting, done };
    std::vector<Mark> mark(bodies.size(), Mark::none);
    body_orbit_.assign(bodies.size(), orbit::no_body);
    std::size_t stars = 0;

    auto add_body = [&](auto& self, sim::DefId<BodyDef> id) -> bool {
        const std::size_t i = index_of(id);
        if (mark[i] == Mark::done) {
            return body_orbit_[i].valid();
        }
        const BodyDef& b = bodies[id];
        if (mark[i] == Mark::visiting) {
            diags.error(bodies.source(id), std::format("body '{}': parent chain forms a cycle", bodies.key(id)));
            return false;
        }
        mark[i] = Mark::visiting;
        bool ok = true;
        if (b.kind == BodyKind::star) {
            body_orbit_[i] = orbits_.add_fixed();
            ++stars;
        } else {
            const sim::DefId<BodyDef> parent = *b.parent;
            ok = self(self, parent);
            const BodyDef& p = bodies[parent];
            if (ok && p.gm_km3_s2 <= 0.0) {
                diags.error(bodies.source(id),
                            std::format("body '{}' orbits '{}', which has no 'gm_km3_s2'",
                                        bodies.key(id), bodies.key(parent)));
                ok = false;
            }
            if (ok) {
                body_orbit_[i] = orbits_.add_orbiting(orbit_of(parent), to_elements(*b.orbit, p.gm_km3_s2));
            }
        }
        mark[i] = Mark::done;
        return ok;
    };
    for (auto [id, row] : bodies) {
        (void)row;
        add_body(add_body, id);
    }
    if (stars != 1) {
        diags.error({}, std::format("content must define exactly one star body (found {})", stars));
    }
    if (diags.size() != errors_before) {
        return false;
    }

    station_orbit_.assign(stations.size(), orbit::no_body);
    for (auto [id, st] : stations) {
        const BodyDef& host = bodies[st.body];
        if (st.orbit) {
            if (host.gm_km3_s2 <= 0.0) {
                diags.error(stations.source(id),
                            std::format("station '{}' orbits '{}', which has no 'gm_km3_s2'",
                                        stations.key(id), table<BodyDef>().key(st.body)));
                continue;
            }
            station_orbit_[index_of(id)] = orbits_.add_orbiting(orbit_of(st.body), to_elements(*st.orbit, host.gm_km3_s2));
        } else {
            station_orbit_[index_of(id)] = orbits_.add_fixed({}, orbit_of(st.body));
        }
        std::vector<bool> seen(table<CommodityDef>().size(), false);
        for (const MarketEntryDef& m : st.market) {
            if (seen[index_of(m.commodity)]) {
                diags.error(stations.source(id),
                            std::format("station '{}': commodity '{}' listed twice in market",
                                        stations.key(id), table<CommodityDef>().key(m.commodity)));
            }
            seen[index_of(m.commodity)] = true;
        }
    }
    for (auto [id, sc] : table<ScenarioDef>()) {
        if (sc.cash < 0 && sc.loan_principal == 0) {
            diags.error(table<ScenarioDef>().source(id),
                        std::format("scenario '{}': negative cash without a loan", table<ScenarioDef>().key(id)));
        }
    }
    return diags.size() == errors_before;
}

} // namespace expanse
