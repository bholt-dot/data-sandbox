---
# repo-pl7w
title: Initial solar system data set
status: completed
type: task
priority: normal
created_at: 2026-09-24T23:46:47Z
updated_at: 2026-09-25T00:46:33Z
parent: repo-bc5e
---

Sun, Earth/Luna, Mars, Ceres, Vesta, Pallas, a Jovian moon (Ganymede), a Saturnian moon (Titan) + ~10 stations (Tycho, Ceres Station, Eros...).



## Summary of Changes

**Bodies (30)**, all Keplerian at J2000 in J2000 ecliptic axes:
- `data/bodies.toml`: Sun; Mercury, Venus, Earth, Mars, Jupiter, Saturn, Uranus, Neptune.
- `data/moons.toml`: Luna; Phobos, Deimos; Io, Europa, Ganymede, Callisto; Enceladus, Rhea,
  Titan, Phoebe; Miranda, Titania, Oberon.
- `data/asteroids.toml`: Ceres, Vesta, Pallas, Juno, Hygiea, Psyche, Eros (moved Ceres/Vesta/
  Pallas here from bodies.toml; their placeholder mean anomalies are now real values).

**Stations (16)**: the 5 starter stations in `data/stations.toml` are unchanged. New ones:
`stations_inner.toml` (earth_orbital: GEO in Earth's equatorial plane; luna_yards),
`stations_belt.toml` (eros_station, hygiea_outpost, psyche_foundry),
`stations_outer.toml` (ganymede_agridomes as the outer system's food source, europa_iceworks,
callisto_yards (Mars), titan_port, enceladus_wellhead, titania_relay). Markets use only existing
commodities: the Belt exports water/ore and imports food/medical; the inner system exports
food/medical/parts.

**Tests**: `expanse/tests/dataset_tests.cpp`: heliocentric ranges at 2350-01-01 against published
perihelion/aphelion; every body stays between its periapsis and apoapsis; planets within e of a;
moons inside 0.5 R_Hill of their parent and clear of its surface; planet periods within 0.5%;
Galilean and other regular moon periods within 1%; stations finite and near their host; market
numbers in a sane range; starter stations and the Secondhand start at Ceres Station still present.

**Method and sources** (JPL/SSD, Wikipedia and NSSDC were blocked by the network proxy; GitHub raw
and PyPI were reachable):
- Planets: Standish, "Keplerian Elements for Approximate Positions of the Major Planets" (JPL),
  Table 1, converted as peri = varpi - node, M = L - varpi. Existing Earth/Mars/Jupiter values
  checked and kept. Cross-check: VSOP87 (astronomy-engine 2.1.19) at J2000 agrees to within 0.15 deg.
- Asteroids: MPC osculating elements as shipped by Stellarium (stellarium-0-10-5 `ssystem.ini`,
  epoch 2007-04-10; master `ssystem_minor.ini`, epoch 2025-11-21). M carried to J2000 as
  M - n (epoch - J2000). The two sources agree to within 1-4 deg of J2000 position for Ceres/Pallas/
  Juno/Vesta; the 2007 set is used where available.
- Moons: analytic theories sampled over one orbit from J2000 and fitted (plane, first-order
  ellipse, M chosen to reproduce the J2000 direction). Galilean moons from astronomy-engine (E5);
  Mars, Saturn and Uranus moons from PyEphem 4.2.1 / libastro (TASS 1.7, GUST86), light-time
  corrected. The frame convention was checked against astronomy-engine, and the two agree on
  Galilean mean longitudes to 0.2 deg. a from JPL mean elements / IAU values; fitted e agrees with
  JPL mean e to ~0.001. Phoebe: Horizons osculating elements (via Stellarium) rotated from Saturn's
  equator (IAU 2015 pole) to the ecliptic.
- GM/radii: JPL SSD physical parameters (DE440, MAR097, JUP310, SAT441), Dawn (Ceres, Vesta),
  NEAR (Eros); the rest are G x published mass, marked approximate in the TOML.
- Best-practice check: moon stability limits (Hamilton & Burns 1992; Domingos et al. 2006:
  ~0.49 R_H prograde, ~0.93 R_H retrograde, R_H at pericentre) set the Hill test threshold.

**Approximations** (noted in the TOML): no element rates or precession (positions in 2350 are
plausible, not ephemeris-grade); moon orbits are fixed ellipses in their planet's J2000 equatorial
plane; asteroid phases are good to a few degrees; Phoebe's phase is uncertain by tens of degrees;
Luna's period is 0.5% long because orbits use the parent's GM only (not parent + body).

**Deferred**: use GM_parent + GM_body for body orbits in `content.cpp` (fixes Luna's 0.5% and the
planets' 0.05%); Triton and the Neptune system; Trojans/Hildas; areostationary Mars Highport would
belong in Mars' equatorial plane (i ~ 26.7 deg ecliptic). Left unchanged because other beans own
that station.
