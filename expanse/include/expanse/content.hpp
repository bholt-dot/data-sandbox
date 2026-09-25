#pragma once

// Static game content: definitions loaded from data/*.toml. Content is immutable during play and is
// never saved; the World (world.hpp) refers to it by DefId. A save records content_fingerprint() so
// loading against different data can be detected.

#include "expanse/orbit.hpp"
#include "simcore/defs.hpp"

#include <array>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace expanse {

using Credits = std::int64_t;

// ---- Commodities ------------------------------------------------------------------------------

enum class CommodityCategory : std::uint8_t { volatiles, food, industrial, medical, luxury };

constexpr auto enum_names(CommodityCategory) {
    using C = CommodityCategory;
    return std::array{std::pair{std::string_view{"volatiles"}, C::volatiles},
                      std::pair{std::string_view{"food"}, C::food},
                      std::pair{std::string_view{"industrial"}, C::industrial},
                      std::pair{std::string_view{"medical"}, C::medical},
                      std::pair{std::string_view{"luxury"}, C::luxury}};
}

struct CommodityDef {
    std::string name;
    CommodityCategory category = CommodityCategory::industrial;
    Credits base_price = 0; // per tonne

    static void describe(sim::Schema<CommodityDef>& s);
};

// ---- Bodies and orbits ------------------------------------------------------------------------

enum class BodyKind : std::uint8_t { star, planet, dwarf_planet, moon, asteroid };

constexpr auto enum_names(BodyKind) {
    using K = BodyKind;
    return std::array{std::pair{std::string_view{"star"}, K::star},
                      std::pair{std::string_view{"planet"}, K::planet},
                      std::pair{std::string_view{"dwarf_planet"}, K::dwarf_planet},
                      std::pair{std::string_view{"moon"}, K::moon},
                      std::pair{std::string_view{"asteroid"}, K::asteroid}};
}

// Keplerian elements relative to the parent body, at epoch J2000. Exactly one of a_au / a_km.
struct OrbitDef {
    std::optional<double> a_au;
    std::optional<double> a_km;
    double e = 0.0;
    double i_deg = 0.0;
    double node_deg = 0.0;          // longitude of ascending node
    double peri_deg = 0.0;          // argument of periapsis
    double mean_anomaly_deg = 0.0;  // at J2000

    static void describe(sim::Schema<OrbitDef>& s);
};

struct BodyDef {
    std::string name;
    BodyKind kind = BodyKind::asteroid;
    std::optional<sim::DefId<BodyDef>> parent; // none only for the star
    double gm_km3_s2 = 0.0;                     // needed if anything orbits this body
    double radius_km = 0.0;
    std::optional<OrbitDef> orbit;              // none only for the star

    static void describe(sim::Schema<BodyDef>& s);
};

// ---- Stations ---------------------------------------------------------------------------------

enum class Faction : std::uint8_t { earth, mars, belt, independent };

constexpr auto enum_names(Faction) {
    using F = Faction;
    return std::array{std::pair{std::string_view{"earth"}, F::earth},
                      std::pair{std::string_view{"mars"}, F::mars},
                      std::pair{std::string_view{"belt"}, F::belt},
                      std::pair{std::string_view{"independent"}, F::independent}};
}

struct MarketEntryDef {
    sim::DefId<CommodityDef> commodity;
    double production = 0.0;  // t/day
    double consumption = 0.0; // t/day
    double stock = 0.0;       // t at scenario start

    static void describe(sim::Schema<MarketEntryDef>& s);
};

struct StationDef {
    std::string name;
    sim::DefId<BodyDef> body;
    std::optional<OrbitDef> orbit; // relative to `body`; none = on/at the body
    Faction faction = Faction::independent;
    std::uint32_t population = 0;
    Credits docking_fee = 0; // per day docked
    std::vector<MarketEntryDef> market;

    static void describe(sim::Schema<StationDef>& s);
};

// ---- Ships ------------------------------------------------------------------------------------

struct ShipClassDef {
    std::string name;
    double dry_mass_t = 0.0;
    double cargo_capacity_t = 0.0;
    double reaction_mass_capacity_t = 0.0;
    double exhaust_velocity_km_s = 0.0;
    double max_accel_g = 0.0; // drive limit at dry mass; crew tolerance is separate
    std::uint8_t crew_berths = 1;
    Credits price = 0;

    static void describe(sim::Schema<ShipClassDef>& s);
};

// ---- Scenarios --------------------------------------------------------------------------------

struct ScenarioDef {
    std::string name;
    std::string description;
    std::string start_date; // YYYY-MM-DD
    sim::DefId<StationDef> start_station;
    sim::DefId<ShipClassDef> ship_class;
    std::string ship_name;
    std::string company_name;
    Credits cash = 0;
    double reaction_mass_fraction = 1.0; // of tank capacity
    double hull_condition = 1.0;         // 0..1
    Credits loan_principal = 0;
    Credits loan_weekly_payment = 0;
    std::uint8_t loan_missed_payment_limit = 3;

    static void describe(sim::Schema<ScenarioDef>& s);
};

// ---- Content ----------------------------------------------------------------------------------

class Content {
public:
    // Loads and validates all definitions; on failure returns nullptr and fills `diags`.
    static std::unique_ptr<Content> load(const std::filesystem::path& dir, sim::Diagnostics& diags);
    static std::unique_ptr<Content> load(std::span<const sim::DataSource> sources,
                                         sim::Diagnostics& diags);

    template <typename Row>
    const sim::DefTable<Row>& table() const {
        return defs_.get<Row>();
    }
    template <typename Row>
    sim::DefId<Row> find(std::string_view key) const {
        return defs_.find<Row>(key);
    }

    const orbit::OrbitSystem& orbits() const { return orbits_; }
    orbit::BodyId orbit_of(sim::DefId<BodyDef> body) const { return body_orbit_[body.index]; }
    orbit::BodyId orbit_of(sim::DefId<StationDef> station) const { return station_orbit_[station.index]; }

    // Hash of every data source (names + text) in load order.
    std::uint64_t fingerprint() const { return fingerprint_; }

private:
    Content();
    bool build(sim::Diagnostics& diags);

    sim::DefRegistry defs_;
    orbit::OrbitSystem orbits_;
    std::vector<orbit::BodyId> body_orbit_;    // by BodyDef dense index
    std::vector<orbit::BodyId> station_orbit_; // by StationDef dense index
    std::uint64_t fingerprint_ = 0;
};

// Dense index of a definition id (ids are dense and assigned in key order).
template <typename Row>
constexpr std::size_t index_of(sim::DefId<Row> id) {
    return id.index;
}

} // namespace expanse
