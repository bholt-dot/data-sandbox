#include <doctest/doctest.h>

#include <cstdlib>
#include <optional>
#include <sstream>
#include <string>

#include "simcore/doc.hpp"

using namespace sim;

namespace {

std::string ansi_text(const Doc& doc, Ansi ansi) {
    std::ostringstream out;
    render(doc, out, ansi);
    return out.str();
}

std::string rows_text(const std::vector<Line>& rows) {
    std::ostringstream out;
    render(rows, out);
    return out.str();
}

// Sets an environment variable for the lifetime of the guard (nullptr unsets it).
class EnvGuard {
public:
    EnvGuard(const char* name, const char* value) : name_(name) {
        if (const char* old = std::getenv(name)) {
            old_ = old;
        }
        set(value);
    }
    ~EnvGuard() { set(old_ ? old_->c_str() : nullptr); }
    EnvGuard(const EnvGuard&) = delete;
    EnvGuard& operator=(const EnvGuard&) = delete;

private:
    void set(const char* value) {
        if (value == nullptr) {
            ::unsetenv(name_);
        } else {
            ::setenv(name_, value, 1);
        }
    }
    const char* name_;
    std::optional<std::string> old_;
};

} // namespace

TEST_CASE("doc plain text keeps text and line breaks") {
    Doc out;
    out << "cash " << styled(Style::money, "1,850 cr") << "\n\n" << "last line without newline";
    CHECK(to_text(out) == "cash 1,850 cr\n\nlast line without newline\n");
    REQUIRE(out.blocks().size() == 3);
    const Line& first = std::get<Line>(out.blocks()[0]);
    CHECK(first.spans == std::vector<Span>{{"cash ", Style::plain}, {"1,850 cr", Style::money}});

    Doc h;
    h.heading("== Title ==");
    h << "body\n";
    CHECK(to_text(h) == "== Title ==\nbody\n");
    CHECK(to_text(Doc()).empty());
}

TEST_CASE("doc spans of one style merge") {
    Line l;
    l.append({"a", Style::key});
    l.append({"b", Style::key});
    l.append({"", Style::bad});
    l.append({"c", Style::plain});
    CHECK(l.spans == std::vector<Span>{{"ab", Style::key}, {"c", Style::plain}});
    CHECK(l.width() == 3);
    CHECK(display_width("Hauling — 2350") == 14);
}

TEST_CASE("doc tables size columns to their contents") {
    Doc out;
    out << "before\n";
    TextTable& t = out.table({{"key"}, {"stock", Align::right}, {"note"}});
    t.row({"water", "30000 t", ""});
    t.row({styled(Style::key, "medical"), "40 t", styled(Style::warning, "short")});
    out << "after\n";
    CHECK(to_text(out) ==
          "before\n"
          "  key        stock  note\n"
          "  water    30000 t\n"
          "  medical     40 t  short\n"
          "after\n");
    CHECK_THROWS_AS(t.row({"1", "2", "3", "4"}), std::logic_error);
}

TEST_CASE("doc tables without headers and with missing cells") {
    Doc out;
    TextTable& t = out.table({{}, {}});
    t.indent = 4;
    t.gap = 1;
    t.row({"a", "first"});
    t.row({"long"});
    CHECK(to_text(out) == "    a    first\n    long\n");
}

TEST_CASE("doc layout wraps long lines with a hanging indent") {
    Doc out;
    out << "  one two three four five six\n";
    const auto rows = layout(out, 14);
    CHECK(rows_text(rows) == "  one two\n  three four\n  five six\n");

    Doc word;
    word << "abcdefghij\n";
    CHECK(rows_text(layout(word, 4)) == "abcd\nefgh\nij\n");

    // Unlimited width never wraps.
    CHECK(rows_text(layout(out, 0)) == "  one two three four five six\n");
}

TEST_CASE("doc layout wraps the last table column") {
    Doc out;
    TextTable& t = out.table({{"n"}, {"why"}});
    t.row({"1", "needs more reaction mass than aboard"});
    CHECK(rows_text(layout(out, 20)) ==
          "  n  why\n"
          "  1  needs more\n"
          "     reaction mass\n"
          "     than aboard\n");
}

TEST_CASE("doc ansi rendering wraps styled spans in SGR codes") {
    Doc out;
    out << "cash " << styled(Style::money, "5 cr") << " " << styled(Style::urgent, "! repossession") << "\n";
    CHECK(ansi_text(out, Ansi::none) == "cash 5 cr ! repossession\n");
    CHECK(ansi_text(out, Ansi::color) ==
          "cash \x1b[33m5 cr\x1b[0m \x1b[1;37;41m! repossession\x1b[0m\n");
    // Without colour, money is plain and urgent is bold inverse.
    CHECK(ansi_text(out, Ansi::mono) == "cash 5 cr \x1b[1;7m! repossession\x1b[0m\n");
    CHECK(sgr(Style::plain, Ansi::color).empty());
    CHECK(sgr(Style::good, Ansi::color) == "1;32");
    CHECK(sgr(Style::bad, Ansi::none).empty());
}

TEST_CASE("doc terminal detection respects NO_COLOR and TERM") {
    EnvGuard term("TERM", "xterm-256color");
    {
        EnvGuard nc("NO_COLOR", nullptr);
        CHECK(ansi_for_terminal(true) == Ansi::color);
        CHECK(ansi_for_terminal(false) == Ansi::none);
        CHECK_FALSE(no_color_requested());
    }
    {
        EnvGuard nc("NO_COLOR", "1");
        CHECK(ansi_for_terminal(true) == Ansi::mono);
        CHECK(no_color_requested());
    }
    {
        EnvGuard nc("NO_COLOR", ""); // present but empty: ignored, per no-color.org
        CHECK(ansi_for_terminal(true) == Ansi::color);
    }
    {
        EnvGuard nc("NO_COLOR", nullptr);
        EnvGuard dumb("TERM", "dumb");
        CHECK(ansi_for_terminal(true) == Ansi::none);
    }
}
