#pragma once

// Registry of StatPipeline ids and entity-key kinds used by the game (World::stats).
// Each system owns a block of ids so parallel work never collides; add new blocks below.

#include "expanse/world.hpp"
#include "simcore/stats.hpp"

#include <cstdint>

namespace expanse {

// EntityKey::kind values. Kind 0 is reserved for sim::global_scope.
namespace entity_kind {
inline constexpr std::uint32_t ship = 1;
inline constexpr std::uint32_t crew_member = 2;
// Modifier *source* for everything a ship's crew complement contributes; crew.cpp replaces all
// modifiers from this source whenever the complement changes (hire, fire, quit).
inline constexpr std::uint32_t crew_complement = 3;
} // namespace entity_kind

inline sim::EntityKey ship_key(ShipId id) { return sim::entity_key(entity_kind::ship, id); }
inline sim::EntityKey crew_key(CrewId id) { return sim::entity_key(entity_kind::crew_member, id); }
inline sim::EntityKey crew_complement_key(ShipId id) {
    return sim::entity_key(entity_kind::crew_complement, id);
}

namespace stat {

// ---- Crew-driven ship multipliers: ids 100-199 (crew.cpp) --------------------------------------
// All targets are ship_key(ship). Every one is a dimensionless multiplier with a global base of
// 1.0 (set by crew::start_new_game), so a ship without relevant crew reads exactly 1.0 and callers
// just multiply: e.g. ship ops computes
//     sustained_accel_g = crew_base_tolerance_g * world.stats.get(ship_key(id), stat::accel_tolerance)
// Only the best crew member in each role contributes (one person has the helm), using
// `increased` modifiers scaled by skill/100 — see crew::skill_effect().

// Pilot: sustained-acceleration tolerance the crew can fly at (better burn/couch management).
inline constexpr sim::StatId accel_tolerance{100};
// Pilot: reaction mass used for a given delta-v plan (trim, fewer correction burns).
inline constexpr sim::StatId reaction_mass_use{101};
// Engineer: hull/subsystem wear rate.
inline constexpr sim::StatId hull_wear{102};
// Engineer: water and oxygen make-up drawn from stores (recycler losses and leaks; read by crew.cpp).
inline constexpr sim::StatId life_support_use{103};
// Medic: health lost to deprivation (read by crew.cpp).
inline constexpr sim::StatId health_loss{104};

} // namespace stat

} // namespace expanse
