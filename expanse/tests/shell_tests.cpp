#include <doctest/doctest.h>

#include "expanse/shell.hpp"

#include <memory>
#include <sstream>
#include <string>

using namespace expanse;

namespace {

struct Shell {
    std::shared_ptr<const Content> content;
    Session session;
    ShellBus bus;

    Shell() {
        sim::Diagnostics diags;
        content = Content::load(EXPANSE_DATA_DIR, diags);
        REQUIRE(content);
        session.content = content;
        register_game_commands(bus, content);
    }

    // Runs one line; returns its output (errors included).
    std::string run(std::string_view line) {
        std::ostringstream out;
        const sim::LineResult r = bus.execute_line(line, session, out);
        if (!r.ok()) {
            out << "error: " << r.error << "\n";
        }
        return out.str();
    }
};

bool contains(const std::string& haystack, std::string_view needle) {
    return haystack.find(needle) != std::string::npos;
}

} // namespace

TEST_CASE("shell commands need a game") {
    Shell sh;
    CHECK(contains(sh.run("status"), "no game in progress"));
    CHECK(contains(sh.run("new secondhand --seed 3"), "Secondhand"));
    CHECK(contains(sh.run("status"), "1,850 cr"));
    CHECK(contains(sh.run("station ceres_station"), "Water ice"));
    CHECK(contains(sh.run("station nowhere"), "no station"));
}

TEST_CASE("shell plot and go fly the previewed course") {
    Shell sh;
    sh.run("new secondhand --seed 3");
    const std::string plot = sh.run("plot vesta_dock --within 25d");
    CHECK(contains(plot, "GO"));
    CHECK(contains(sh.run("go vesta_dock --within 25d"), "Underway"));
    CHECK(contains(sh.run("status"), "underway to Vesta Dock"));
    CHECK(contains(sh.run("go tycho_station"), "error"));
    sh.run("advance 30d --force");
    // Repossessed on the way (no income): further actions are refused.
    CHECK(sh.session.world->game_over.has_value());
    CHECK(contains(sh.run("pay 100"), "game over"));
    // Time still runs: the belt goes on without you.
    CHECK_FALSE(contains(sh.run("advance 1d"), "error"));
}

TEST_CASE("shell session replays to the same hash") {
    Shell a;
    for (const char* line : {"new secondhand --seed 9", "go vesta_dock --within 20d", "pay 500", "advance 5d"}) {
        a.run(line);
    }
    std::ostringstream script;
    a.bus.write_script(script);

    Shell b;
    std::istringstream lines(script.str());
    for (std::string line; std::getline(lines, line);) {
        b.run(line);
    }
    CHECK(world_hash(*a.session.world) == world_hash(*b.session.world));
}
