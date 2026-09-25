#pragma once

#include <numbers>

// Everything internal is SI (metres, seconds, radians, m^3/s^2). These helpers exist so data and
// tests can be written in the units they are published in.
namespace expanse::units {

inline constexpr double pi = std::numbers::pi;
inline constexpr double two_pi = 2.0 * std::numbers::pi;

// IAU 2012 Resolution B2: exact.
inline constexpr double au_m = 149'597'870'700.0;
inline constexpr double km_m = 1000.0;
inline constexpr double day_s = 86'400.0;
// Julian year, as used by the J2000 element rates.
inline constexpr double julian_year_s = 365.25 * day_s;
inline constexpr double julian_century_s = 100.0 * julian_year_s;

// Heliocentric gravitational constant (IAU 2015 nominal, TDB-compatible), m^3/s^2.
inline constexpr double mu_sun = 1.32712440018e20;
// Geocentric gravitational constant (IERS/IAU), m^3/s^2.
inline constexpr double mu_earth = 3.986004418e14;

constexpr double au(double v) { return v * au_m; }
constexpr double km(double v) { return v * km_m; }
constexpr double days(double v) { return v * day_s; }
constexpr double deg(double v) { return v * (pi / 180.0); }

constexpr double to_au(double metres) { return metres / au_m; }
constexpr double to_deg(double radians) { return radians * (180.0 / pi); }

} // namespace expanse::units
