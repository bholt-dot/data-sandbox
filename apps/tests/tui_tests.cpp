#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <memory>
#include <string>
#include <string_view>

#include <ftxui/component/event.hpp>
#include <ftxui/dom/elements.hpp>
#include <ftxui/screen/screen.hpp>

#include "doc_view.hpp"
#include "expanse/content.hpp"
#include "expanse/finance.hpp"
#include "expanse/shell.hpp"
#include "tui.hpp"

namespace {

struct Game {
    std::shared_ptr<const expanse::Content> content;
    expanse::Session session;
    expanse::ShellBus bus;

    Game() {
        sim::Diagnostics diags;
        content = expanse::Content::load(BELTER_TEST_DATA_DIR, diags);
        REQUIRE(content);
        session.content = content;
        expanse::register_game_commands(bus, content);
    }
};

// Renders one frame and returns its characters.
std::string frame(belter::Tui& tui, int width, int height) {
    ftxui::Screen screen = ftxui::Screen::Create(ftxui::Dimension::Fixed(width), ftxui::Dimension::Fixed(height));
    ftxui::Render(screen, tui.view(width, height));
    return sim::tui::plain_text(screen);
}

bool contains(const std::string& haystack, std::string_view needle) {
    return haystack.find(needle) != std::string::npos;
}

void type(belter::Tui& tui, std::string_view text) {
    for (const char c : text) {
        tui.component()->OnEvent(ftxui::Event::Character(c));
    }
}

} // namespace

TEST_CASE("tui frame shows the status banner output and journal") {
    Game g;
    belter::Tui tui(g.bus, g.session);
    CHECK(contains(frame(tui, 120, 30), "no game in progress"));

    tui.execute("new secondhand --seed 3");
    tui.execute("go hollow_nail");
    const std::string f = frame(tui, 120, 30);
    MESSAGE(f); // a text snapshot of the frame, shown with -s
    for (const char* s : {"2350-03-14 00:00", "Cash 1,850 cr", "Loan 360 cr due 03-21", "Dustkicker -> Hollow Nail",
                          "RM 30%", "Hull 62%", "Crew 1/4", "Journal", "> go hollow_nail", "Underway."}) {
        CHECK_MESSAGE(contains(f, s), s);
    }
}

TEST_CASE("tui narrow terminals collapse the journal") {
    Game g;
    belter::Tui tui(g.bus, g.session);
    tui.execute("new secondhand --seed 3");
    const std::string f = frame(tui, 70, 20);
    CHECK_FALSE(contains(f, "Journal"));
    CHECK(contains(f, "Cash 1,850 cr"));
    // The banner wraps rather than clipping.
    CHECK(contains(f, "Crew 1/4"));
}

TEST_CASE("tui pins urgent journal entries until acknowledged") {
    Game g;
    belter::Tui tui(g.bus, g.session);
    tui.execute("new secondhand --seed 3");
    expanse::World& w = *g.session.world;
    REQUIRE(expanse::transact(w, w.player, -w.companies.at(w.player).cash, expanse::LedgerCategory::other, "drank it"));
    tui.execute("advance 8d --force"); // broke: the first instalment is missed
    std::string f = frame(tui, 120, 30);
    CHECK(contains(f, "1/3 missed"));
    CHECK(contains(f, "Esc: acknowledge"));

    tui.on_event(ftxui::Event::Escape);
    f = frame(tui, 120, 30);
    CHECK_FALSE(contains(f, "Esc: acknowledge"));
    CHECK(contains(f, "1/3 missed")); // the banner still tells the truth
}

TEST_CASE("tui input completes and recalls history") {
    Game g;
    belter::Tui tui(g.bus, g.session);
    type(tui, "stati");
    tui.on_event(ftxui::Event::Tab);
    CHECK(tui.input() == "station");
    CHECK(contains(frame(tui, 120, 30), "stations"));

    tui.on_event(ftxui::Event::Escape);
    CHECK(tui.input().empty());

    type(tui, "new --seed 2");
    tui.on_event(ftxui::Event::Return);
    CHECK(g.session.world.has_value());
    CHECK(tui.input().empty());
    tui.on_event(ftxui::Event::ArrowUp);
    CHECK(tui.input() == "new --seed 2");
    tui.on_event(ftxui::Event::ArrowDown);
    CHECK(tui.input().empty());
}

TEST_CASE("tui scrollback pages and quit ends the loop") {
    Game g;
    belter::Tui tui(g.bus, g.session);
    for (int i = 0; i < 5; ++i) {
        tui.execute("help");
    }
    CHECK_FALSE(contains(frame(tui, 120, 30), "lines below"));
    tui.on_event(ftxui::Event::PageUp);
    CHECK(contains(frame(tui, 120, 30), "lines below"));
    tui.on_event(ftxui::Event::PageDown);
    CHECK_FALSE(contains(frame(tui, 120, 30), "lines below"));

    CHECK_FALSE(tui.quit_requested());
    tui.execute("quit");
    CHECK(tui.quit_requested());
}
