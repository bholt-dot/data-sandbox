#pragma once

#include <cmath>
#include <tuple>

namespace expanse {

// Minimal double-precision 3-vector for continuous models (orbits, transits). SI units.
struct Vec3 {
    double x = 0.0;
    double y = 0.0;
    double z = 0.0;

    constexpr bool operator==(const Vec3&) const = default;
    static constexpr auto fields(auto& self) { return std::tie(self.x, self.y, self.z); }

    constexpr Vec3 operator+(const Vec3& o) const { return {x + o.x, y + o.y, z + o.z}; }
    constexpr Vec3 operator-(const Vec3& o) const { return {x - o.x, y - o.y, z - o.z}; }
    constexpr Vec3 operator-() const { return {-x, -y, -z}; }
    constexpr Vec3 operator*(double k) const { return {x * k, y * k, z * k}; }
    constexpr Vec3 operator/(double k) const { return {x / k, y / k, z / k}; }
    constexpr Vec3& operator+=(const Vec3& o) { x += o.x; y += o.y; z += o.z; return *this; }
    constexpr Vec3& operator-=(const Vec3& o) { x -= o.x; y -= o.y; z -= o.z; return *this; }
    constexpr Vec3& operator*=(double k) { x *= k; y *= k; z *= k; return *this; }
};

constexpr Vec3 operator*(double k, const Vec3& v) { return v * k; }

constexpr double dot(const Vec3& a, const Vec3& b) { return a.x * b.x + a.y * b.y + a.z * b.z; }

constexpr Vec3 cross(const Vec3& a, const Vec3& b) {
    return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}

constexpr double length_squared(const Vec3& v) { return dot(v, v); }

inline double length(const Vec3& v) { return std::sqrt(length_squared(v)); }

inline double distance(const Vec3& a, const Vec3& b) { return length(a - b); }

inline Vec3 normalized(const Vec3& v) { return v / length(v); }

} // namespace expanse
