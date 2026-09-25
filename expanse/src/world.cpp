#include "expanse/world.hpp"

#include "simcore/hash.hpp"
#include "simcore/snapshot.hpp"

#include <format>

namespace expanse {

sim::Rng& World::rng(const std::string& key) {
    auto it = rngs.find(key);
    if (it == rngs.end()) {
        it = rngs.emplace(key, sim::rng_for(seed, key)).first;
    }
    return it->second;
}

Vec3 ship_position(const Content& content, const Ship& ship, sim::Time t) {
    if (const auto* docked = std::get_if<Docked>(&ship.location)) {
        return content.orbits().world_position(content.orbit_of(docked->station), t);
    }
    const auto& u = std::get<Underway>(ship.location);
    const Vec3 delta = u.end - u.start;
    const double len = length(delta);
    if (len <= 0.0) {
        return u.start;
    }
    const double along = transit::state_along(u.profile, (t - u.departure).to_seconds_f()).distance;
    return u.start + delta * (along / len);
}

Vec3 ship_position(const Content& content, const World& world, const Ship& ship) {
    return ship_position(content, ship, world.now());
}

double cargo_mass_t(const Ship& ship) {
    double total = 0.0;
    for (const CargoLot& lot : ship.cargo) {
        total += lot.tonnes;
    }
    return total;
}

std::vector<std::uint8_t> save_world(const World& world) {
    return sim::save_snapshot(world_schema_version, world);
}

World load_world(std::span<const std::uint8_t> bytes, const Content& content) {
    World w;
    sim::load_snapshot(bytes, world_schema_version, w);
    if (w.content_fingerprint != content.fingerprint()) {
        throw sim::SerializeError(std::format(
            "save was made with different game data (fingerprint {:016x}, loaded data {:016x})",
            w.content_fingerprint, content.fingerprint()));
    }
    if (w.stations.size() != content.table<StationDef>().size()) {
        throw sim::SerializeError("save does not match the loaded station definitions");
    }
    const std::size_t commodities = content.table<CommodityDef>().size();
    for (const StationState& st : w.stations) {
        if (st.stock.size() != commodities || st.unmet.size() != commodities ||
            st.disrupted_days.size() != commodities) {
            throw sim::SerializeError("save does not match the loaded commodity definitions");
        }
    }
    return w;
}

std::uint64_t world_hash(const World& world) { return sim::hash_state(world); }

} // namespace expanse
