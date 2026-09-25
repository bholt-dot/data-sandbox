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
        sim::Doc out;
        const sim::LineResult r = bus.execute_line(line, session, out);
        if (!r.ok()) {
            out << "error: " << r.error << "\n";
        }
        return sim::to_text(out);
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

TEST_CASE("shell trade and crew commands") {
    Shell sh;
    sh.run("new secondhand --seed 5");
    CHECK(contains(sh.run("market"), "Water ice"));
    CHECK(contains(sh.run("crew"), "Looking for work"));
    CHECK(contains(sh.run("buy water 5"), "bought 5.0 t"));
    CHECK(contains(sh.run("sell water"), "sold 5.0 t"));
    CHECK(contains(sh.run("buy unobtainium 5"), "no commodity"));
    CHECK(contains(sh.run("refuel 1"), "reaction mass"));
    CHECK(contains(sh.run("hire 999999"), "nobody with id"));
    CHECK(contains(sh.run("books"), "trade"));
}

namespace {

// All spans of one style in a command's output, in order.
std::vector<std::string> spans_of(const sim::Doc& doc, sim::Style style) {
    std::vector<std::string> out;
    for (const sim::Line& row : sim::layout(doc)) {
        for (const sim::Span& s : row.spans) {
            if (s.style == style) {
                out.push_back(s.text);
            }
        }
    }
    return out;
}

} // namespace

TEST_CASE("shell output is styled and tabular") {
    Shell sh;
    sh.run("new secondhand --seed 3");
    sim::Doc market;
    REQUIRE(sh.bus.execute_line("market", sh.session, market).ok());
    CHECK(std::holds_alternative<sim::TextTable>(market.blocks().back()));
    CHECK(spans_of(market, sim::Style::key).front() == "water");

    sim::Doc plot;
    REQUIRE(sh.bus.execute_line("plot vesta_dock --within 25d", sh.session, plot).ok());
    CHECK(spans_of(plot, sim::Style::good) == std::vector<std::string>{"GO"});

    // A missed instalment is urgent everywhere it shows.
    sim::Doc advance;
    sh.bus.execute_line("advance 8d --force", sh.session, advance);
    CHECK_FALSE(spans_of(advance, sim::Style::urgent).empty());
    sim::Doc status;
    sh.bus.execute_line("status", sh.session, status);
    CHECK(spans_of(status, sim::Style::urgent) == std::vector<std::string>{"(1 missed!)"});
}

TEST_CASE("status banner summarizes the player's position") {
    Shell sh;
    CHECK_FALSE(status_banner(sh.session).has_value());
    sh.run("new secondhand --seed 3");
    auto b = status_banner(sh.session);
    REQUIRE(b.has_value());
    CHECK(b->cash == 1850);
    REQUIRE(b->loan.has_value());
    CHECK(b->loan->instalment == 4200);
    CHECK(b->loan->until_due == sim::days(7));
    CHECK(b->loan->missed == 0);
    REQUIRE(b->ship.has_value());
    CHECK(b->ship->docked_at == "Ceres Station");
    CHECK(b->ship->crew == 1);
    CHECK(b->ship->berths == 4);
    CHECK(b->ship->reaction_mass_pct == doctest::Approx(35.0));
    CHECK(b->ship->hull_pct == doctest::Approx(62.0));

    std::string text;
    for (const sim::Line& seg : banner_segments(*b)) {
        text += seg.text() + " | ";
    }
    CHECK(text == "2350-03-14 00:00 | Cash 1,850 cr | Loan 4,200 cr due 03-21 in 7d 0h | "
                  "Dustkicker docked at Ceres Station | RM 35% | Hull 62% | Crew 1/4 | ");

    sh.run("pay 1000");
    sh.run("go hollow_nail");
    b = status_banner(sh.session);
    CHECK(b->loan->instalment == 3200);
    CHECK_FALSE(b->ship->docked_at.has_value());
    CHECK(b->ship->destination == "Hollow Nail");
    CHECK(b->ship->remaining > sim::Duration{});
}

TEST_CASE("journal lines mark urgent entries") {
    const Message m{sim::Time{} + sim::days(72), MessageKind::finance, true, "Missed a payment"};
    CHECK(journal_line(m).text() == "  [2350-03-14 00:00] finance ! Missed a payment");
    CHECK(journal_line(m, true).text() == "03-14 00:00 ! Missed a payment");
    CHECK(journal_line(m).spans.back().style == sim::Style::urgent);
}
