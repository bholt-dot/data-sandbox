#include <doctest/doctest.h>

#include "simcore/defs.hpp"

#include <algorithm>
#include <array>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

using namespace sim;

namespace {

enum class BodyKind { rocky, icy, gas_giant };

constexpr auto enum_names(BodyKind) {
    return std::array{
        std::pair{std::string_view{"rocky"}, BodyKind::rocky},
        std::pair{std::string_view{"icy"}, BodyKind::icy},
        std::pair{std::string_view{"gas_giant"}, BodyKind::gas_giant},
    };
}

struct Commodity {
    std::string name;
    double base_price = 0.0;
    int volume = 1;
    bool legal = true;

    static void describe(Schema<Commodity>& s) {
        s.field("name", &Commodity::name).non_empty();
        s.field("base_price", &Commodity::base_price).range(0.0, 1e6);
        s.optional("volume", &Commodity::volume).min(1);
        s.optional("legal", &Commodity::legal);
    }
};

struct Body {
    std::string name;
    BodyKind kind = BodyKind::rocky;
    double radius_km = 0.0;
    std::optional<DefId<Body>> parent;

    static void describe(Schema<Body>& s) {
        s.field("name", &Body::name);
        s.optional("kind", &Body::kind, BodyKind::rocky);
        s.field("radius_km", &Body::radius_km).min(0.0);
        s.field("parent", &Body::parent);
    }
};

struct Stock {
    DefId<Commodity> commodity;
    std::uint32_t amount = 0;

    static void describe(Schema<Stock>& s) {
        s.field("commodity", &Stock::commodity);
        s.field("amount", &Stock::amount).max(1'000'000);
    }
};

struct Station {
    std::string name;
    DefId<Body> body;
    std::vector<Stock> stock;
    std::vector<DefId<Commodity>> banned;
    std::vector<std::string> tags;
    int min_docks = 1;
    int max_docks = 1;

    static void describe(Schema<Station>& s) {
        s.field("name", &Station::name);
        s.field("body", &Station::body);
        s.optional("stock", &Station::stock);
        s.optional("banned", &Station::banned);
        s.optional("tags", &Station::tags);
        s.optional("min_docks", &Station::min_docks, 1);
        s.optional("max_docks", &Station::max_docks, 1);
        s.check([](const Station& st) {
            return st.min_docks > st.max_docks ? std::string("min_docks must not exceed max_docks")
                                               : std::string{};
        });
    }
};

DefRegistry make_registry() {
    DefRegistry defs;
    defs.define<Commodity>("commodity");
    defs.define<Body>("body");
    defs.define<Station>("station");
    return defs;
}

Diagnostics load(DefRegistry& defs, const std::vector<DataSource>& sources) {
    return defs.load(sources);
}

// A tiny example of what game content looks like. Stations come before the bodies and
// commodities they reference to show that load order doesn't matter.
constexpr std::string_view example_toml = R"toml(
[station.ceres_station]
name = "Ceres Station"
body = "ceres"
banned = ["stims"]
tags = ["port", "belt"]
max_docks = 12

[[station.ceres_station.stock]]
commodity = "water"
amount = 5000

[[station.ceres_station.stock]]
commodity = "stims"
amount = 3

[body.sol]
name = "Sol"
kind = "gas_giant"
radius_km = 696000

[body.ceres]
name = "Ceres"
kind = "icy"
radius_km = 473.0
parent = "sol"

[commodity.water]
name = "Water"
base_price = 2.5
volume = 1

[commodity.stims]
name = "Stims"
base_price = 180
legal = false
)toml";

} // namespace

TEST_CASE("defs: example data loads with references resolved") {
    DefRegistry defs = make_registry();
    const Diagnostics d = load(defs, {{"example.toml", std::string(example_toml)}});
    REQUIRE(d.ok());

    const auto& commodities = defs.get<Commodity>();
    const auto& bodies = defs.get<Body>();
    const auto& stations = defs.get<Station>();
    CHECK(commodities.size() == 2);
    CHECK(bodies.size() == 2);
    CHECK(stations.size() == 1);

    const DefId<Commodity> water = defs.find<Commodity>("water");
    const DefId<Commodity> stims = defs.find<Commodity>("stims");
    REQUIRE(water);
    CHECK(commodities[water].name == "Water");
    CHECK(commodities[water].base_price == doctest::Approx(2.5));
    CHECK(commodities[stims].base_price == doctest::Approx(180.0)); // integer accepted for double
    CHECK_FALSE(commodities[stims].legal);
    CHECK(commodities[water].legal); // optional, default kept
    CHECK(commodities.key(water) == "water");
    CHECK(commodities.source(water).file == "example.toml");
    CHECK(commodities.source(water).line == 28);

    const DefId<Body> ceres = defs.find<Body>("ceres");
    const DefId<Body> sol = defs.find<Body>("sol");
    CHECK(bodies[ceres].kind == BodyKind::icy);
    CHECK(bodies[sol].kind == BodyKind::gas_giant);
    REQUIRE(bodies[ceres].parent.has_value());
    CHECK(*bodies[ceres].parent == sol);
    CHECK_FALSE(bodies[sol].parent.has_value());

    const Station& st = stations[defs.find<Station>("ceres_station")];
    CHECK(st.body == ceres);
    REQUIRE(st.stock.size() == 2);
    CHECK(st.stock[0].commodity == water);
    CHECK(st.stock[0].amount == 5000);
    CHECK(st.stock[1].commodity == stims);
    CHECK(st.banned == std::vector<DefId<Commodity>>{stims});
    CHECK(st.tags == std::vector<std::string>{"port", "belt"});
    CHECK(st.min_docks == 1);
    CHECK(st.max_docks == 12);

    CHECK(defs.find<Commodity>("nope").is_null());
}

TEST_CASE("defs: ids are assigned in key order regardless of file layout") {
    const std::string a = "[commodity.zinc]\nname = \"Zinc\"\nbase_price = 1\n"
                          "[commodity.argon]\nname = \"Argon\"\nbase_price = 1\n";
    const std::string b = "[commodity.mercury]\nname = \"Mercury\"\nbase_price = 1\n";

    DefRegistry one = make_registry();
    DefRegistry two = make_registry();
    REQUIRE(load(one, {{"a.toml", a}, {"b.toml", b}}).ok());
    REQUIRE(load(two, {{"b.toml", b}, {"a.toml", a}}).ok());

    for (const DefRegistry* defs : {&one, &two}) {
        const auto& t = defs->get<Commodity>();
        CHECK(t.find("argon").index == 0);
        CHECK(t.find("mercury").index == 1);
        CHECK(t.find("zinc").index == 2);
        std::vector<std::string> order;
        for (auto [id, row] : t) {
            order.push_back(t.key(id));
        }
        CHECK(order == std::vector<std::string>{"argon", "mercury", "zinc"});
    }
    CHECK(one.find<Commodity>("zinc") == two.find<Commodity>("zinc"));
}

TEST_CASE("defs: missing required field reports location and expected type") {
    DefRegistry defs = make_registry();
    const Diagnostics d = load(defs, {{"c.toml", "\n[commodity.water]\nname = \"Water\"\n"}});
    REQUIRE(d.size() == 1);
    CHECK(d.items()[0].where.file == "c.toml");
    CHECK(d.items()[0].where.line == 2);
    CHECK(d.items()[0].message == "commodity 'water': missing required field 'base_price' (number)");
    CHECK(d.items()[0].to_string() ==
          "c.toml:2:1: error: commodity 'water': missing required field 'base_price' (number)");
    CHECK(defs.get<Commodity>().empty()); // failed loads leave the registry untouched
}

TEST_CASE("defs: wrong type reports what was expected and what was found") {
    DefRegistry defs = make_registry();
    const Diagnostics d =
        load(defs, {{"c.toml", "[commodity.water]\nname = \"Water\"\nbase_price = \"cheap\"\n"}});
    REQUIRE(d.size() == 1);
    CHECK(d.items()[0].where.line == 3);
    CHECK(d.items()[0].where.column == 14);
    CHECK(d.items()[0].message ==
          "commodity 'water': field 'base_price': expected number, got string \"cheap\"");
}

TEST_CASE("defs: integer field rejects floats and values that do not fit") {
    DefRegistry defs = make_registry();
    const Diagnostics d = load(defs, {{"c.toml", R"(
[commodity.water]
name = "Water"
base_price = 1
volume = 1.5

[station.s]
name = "S"
body = "b"
stock = [{ commodity = "water", amount = -1 }]

[body.b]
name = "B"
radius_km = 1
)"}});
    CHECK(d.size() == 2);
    CHECK(d.contains("commodity 'water': field 'volume': expected integer, got float 1.5"));
    CHECK(d.contains("station 's': field 'stock[0].amount': value -1 is out of range; must be between 0 and 4294967295"));
}

TEST_CASE("defs: out of range values are rejected") {
    DefRegistry defs = make_registry();
    const Diagnostics d = load(defs, {{"c.toml", R"(
[commodity.water]
name = "Water"
base_price = -2.5
volume = 0

[body.b]
name = "B"
radius_km = nan
)"}});
    CHECK(d.size() == 3);
    CHECK(d.contains("commodity 'water': field 'base_price': must be between 0 and 1e+06, got -2.5"));
    CHECK(d.contains("commodity 'water': field 'volume': must be at least 1, got 0"));
    CHECK(d.contains("body 'b': field 'radius_km': expected a finite number"));
}

TEST_CASE("defs: unknown keys are errors with did you mean suggestions") {
    DefRegistry defs = make_registry();
    const Diagnostics d = load(defs, {{"c.toml", R"(
[commodity.water]
name = "Water"
base_prcie = 2.0
colour = "blue"

[comodity.ice]
name = "Ice"

[station.s]
name = "S"
body = "b"
stock = [{ commodity = "water", amount = 1, amonut = 2 }]
)"}});
    INFO(d.to_string());
    CHECK(d.contains("commodity 'water': unknown field 'base_prcie' (did you mean 'base_price'?)"));
    CHECK(d.contains("unknown field 'colour'"));
    CHECK_FALSE(d.contains("'colour' (did you mean"));
    CHECK(d.contains("unknown definition type 'comodity' (did you mean 'commodity'?)"));
    CHECK(d.contains("station 's': unknown field 'stock[0].amonut' (did you mean 'amount'?)"));
    // base_price is missing too (the typo'd key doesn't count).
    CHECK(d.contains("missing required field 'base_price'"));

    const auto it = std::find_if(d.begin(), d.end(), [](const Diagnostic& x) {
        return x.message.find("base_prcie") != std::string::npos;
    });
    REQUIRE(it != d.end());
    CHECK(it->where.line == 4);
    CHECK(it->where.column == 1);
}

TEST_CASE("defs: enum values are validated with suggestions") {
    DefRegistry defs = make_registry();
    const Diagnostics d =
        load(defs, {{"b.toml", "[body.b]\nname = \"B\"\nradius_km = 1\nkind = \"rokcy\"\n"}});
    REQUIRE(d.size() == 1);
    CHECK(d.items()[0].message ==
          "body 'b': field 'kind': unknown value 'rokcy'; expected one of 'rocky', 'icy', "
          "'gas_giant' (did you mean 'rocky'?)");
}

TEST_CASE("defs: unresolved references are reported with location and suggestion") {
    DefRegistry defs = make_registry();
    const Diagnostics d = load(defs, {{"s.toml", R"(
[body.ceres]
name = "Ceres"
radius_km = 473
parent = "sun"

[station.s]
name = "S"
body = "ceress"
banned = ["water", "stim"]
)"}});
    INFO(d.to_string());
    CHECK(d.size() == 4);
    CHECK(d.contains("body 'ceres': field 'parent': unknown body 'sun'"));
    CHECK(d.contains("station 's': field 'body': unknown body 'ceress' (did you mean 'ceres'?)"));
    CHECK(d.contains("station 's': field 'banned[0]': unknown commodity 'water'"));
    CHECK(d.contains("station 's': field 'banned[1]': unknown commodity 'stim'"));
    const auto it = std::find_if(d.begin(), d.end(), [](const Diagnostic& x) {
        return x.message.find("ceress") != std::string::npos;
    });
    REQUIRE(it != d.end());
    CHECK(it->where.to_string() == "s.toml:9:8");
}

TEST_CASE("defs: duplicate keys across files are errors") {
    DefRegistry defs = make_registry();
    const Diagnostics d = load(defs, {
                                         {"a.toml", "[commodity.water]\nname = \"W\"\nbase_price = 1\n"},
                                         {"b.toml", "\n[commodity.water]\nname = \"W2\"\nbase_price = 2\n"},
                                     });
    REQUIRE(d.size() == 1);
    CHECK(d.items()[0].where.to_string() == "b.toml:2:1");
    CHECK(d.items()[0].message == "duplicate commodity 'water' (first defined at a.toml:1:1)");
}

TEST_CASE("defs: syntax errors and bad keys are reported and other files still checked") {
    DefRegistry defs = make_registry();
    const Diagnostics d = load(defs, {
                                         {"a.toml", "[commodity.water\nname = 1\n"},
                                         {"b.toml", "[commodity.Water]\nname = \"W\"\nbase_price = 1\n"
                                                    "[body]\nb = 3\n"},
                                     });
    INFO(d.to_string());
    CHECK(d.size() == 3);
    CHECK(d.items()[0].where.file == "a.toml"); // sorted in load order
    CHECK(d.items()[0].message.starts_with("TOML syntax error"));
    CHECK(d.contains("invalid commodity key 'Water'"));
    CHECK(d.contains("body 'b' must be a table, got integer 3"));
}

TEST_CASE("defs: row checks run after fields read cleanly") {
    DefRegistry defs = make_registry();
    const Diagnostics d = load(defs, {{"s.toml", R"(
[body.b]
name = "B"
radius_km = 1

[station.s]
name = "S"
body = "b"
min_docks = 4
max_docks = 2
)"}});
    REQUIRE(d.size() == 1);
    CHECK(d.items()[0].message == "station 's': min_docks must not exceed max_docks");
    CHECK(d.items()[0].where.line == 6);
}

TEST_CASE("defs: a failed reload keeps the previous definitions") {
    DefRegistry defs = make_registry();
    REQUIRE(load(defs, {{"a.toml", "[commodity.water]\nname = \"W\"\nbase_price = 1\n"}}).ok());
    CHECK_FALSE(load(defs, {{"a.toml", "[commodity.ice]\nname = \"I\"\n"}}).ok());
    CHECK(defs.get<Commodity>().size() == 1);
    CHECK(defs.find<Commodity>("water"));
    REQUIRE(load(defs, {{"a.toml", "[commodity.ice]\nname = \"I\"\nbase_price = 1\n"}}).ok());
    CHECK(defs.get<Commodity>().size() == 1);
    CHECK(defs.find<Commodity>("water").is_null());
}

TEST_CASE("defs: directories load recursively in sorted path order") {
    namespace fs = std::filesystem;
    const fs::path dir = fs::temp_directory_path() / "simcore_defs_tests";
    fs::remove_all(dir);
    fs::create_directories(dir / "sub");
    auto write = [](const fs::path& p, std::string_view text) { std::ofstream(p) << text; };
    write(dir / "b.toml", "[commodity.water]\nname = \"W\"\nbase_price = 1\n");
    write(dir / "a.toml", "[commodity.water]\nname = \"W\"\nbase_price = 1\n");
    write(dir / "sub" / "c.toml", "[commodity.ice]\nname = \"I\"\nbase_price = 1\n");
    write(dir / "notes.txt", "not toml");

    Diagnostics read_diags;
    const std::vector<DataSource> sources = read_data_directory(dir, read_diags);
    CHECK(read_diags.ok());
    REQUIRE(sources.size() == 3);
    CHECK(sources[0].name.ends_with("/a.toml"));
    CHECK(sources[1].name.ends_with("/b.toml"));
    CHECK(sources[2].name.ends_with("/sub/c.toml"));

    DefRegistry defs = make_registry();
    const Diagnostics d = defs.load_directory(dir);
    REQUIRE(d.size() == 1);
    CHECK(d.items()[0].where.file.ends_with("/b.toml")); // a.toml is first, so b.toml duplicates

    CHECK_FALSE(defs.load_directory(dir / "missing").ok());
    fs::remove_all(dir);
}

TEST_CASE("defs: registry rejects duplicate types and sections") {
    DefRegistry defs;
    defs.define<Commodity>("commodity");
    CHECK_THROWS_AS(defs.define<Commodity>("goods"), std::logic_error);
    CHECK_THROWS_AS(defs.define<Body>("commodity"), std::logic_error);
    CHECK_THROWS_AS(defs.define<Body>("Bodies"), std::logic_error);
    CHECK_THROWS_AS((void)defs.get<Body>(), std::logic_error);
}

TEST_CASE("defs: references to unregistered types are reported") {
    DefRegistry defs;
    defs.define<Station>("station");
    const Diagnostics d = load(defs, {{"s.toml", "[station.s]\nname = \"S\"\nbody = \"b\"\n"}});
    REQUIRE(d.size() == 1);
    CHECK(d.contains("refers to a definition type that is not registered"));
}

TEST_CASE("defs: edit distance and suggestions") {
    CHECK(edit_distance("", "") == 0);
    CHECK(edit_distance("abc", "") == 3);
    CHECK(edit_distance("kitten", "sitting") == 3);
    CHECK(edit_distance("amount", "amonut") == 1); // transposition counts as one edit
    const std::array<std::string_view, 3> names{"base_price", "volume", "name"};
    CHECK(closest_match("volum", names) == "volume");
    CHECK(closest_match("nmae", names) == "name");
    CHECK(closest_match("Base_Price", names) == "base_price");
    CHECK_FALSE(closest_match("colour", names).has_value());
    CHECK(did_you_mean("xyz", names).empty());
}
