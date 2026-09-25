#include <doctest/doctest.h>

#include <cmath>
#include <cstdint>
#include <format>
#include <limits>
#include <sstream>
#include <string>
#include <vector>

#include "simcore/command.hpp"
#include "simcore/command_bus.hpp"
#include "simcore/repl.hpp"
#include "simcore/serialize.hpp"

using namespace sim;

namespace {

std::vector<std::string> texts(const std::vector<Token>& tokens) {
    std::vector<std::string> out;
    for (const Token& t : tokens) {
        out.push_back(t.text);
    }
    return out;
}

std::string error_of(auto&& fn) {
    try {
        fn();
    } catch (const CommandError& e) {
        return e.what();
    }
    return "<no error>";
}

// A tiny stand-in for a game: a clock, a cash balance and a station list for completion.
struct World {
    std::int64_t now = 0;
    std::int64_t cash = 100;
    std::vector<std::string> stations{"Ceres Station", "Tycho", "Eros"};
};

void register_world(CommandBus<World>& bus, const World& world) {
    bus.add_query({.name = "status", .summary = "Show clock and cash"},
                  [](const World& w, const Invocation&, Doc& out) {
                      out << std::format("t={} cash={}\n", w.now, w.cash);
                  });
    bus.add_action({.name = "advance",
                    .aliases = {"adv"},
                    .summary = "Advance simulation time",
                    .positionals = {{.name = "span",
                                     .type = ArgType::duration,
                                     .help = "how far to advance"}}},
                   [](World& w, const Invocation& inv, Doc&) {
                       w.now += inv.get<Duration>("span").seconds;
                   });
    bus.add_action(
        {.name = "buy",
         .summary = "Buy a commodity",
         .positionals = {{.name = "item", .help = "commodity", .choices = {"water", "ice", "fuel"}},
                         {.name = "qty", .type = ArgType::integer, .help = "units", .default_value = "1"}},
         .options = {{.name = "at",
                      .help = "station",
                      .completer = [&world] { return world.stations; }},
                     {.name = "max-price", .type = ArgType::number, .help = "limit per unit"},
                     {.name = "burn", .type = ArgType::acceleration, .help = "transit burn"},
                     {.name = "dry-run", .type = ArgType::flag, .help = "only show the cost"}}},
        [](World& w, const Invocation& inv, Doc& out) {
            const std::int64_t cost = inv.get<std::int64_t>("qty") * 10;
            if (inv.flag("dry-run")) {
                out << std::format("would cost {}\n", cost);
                return;
            }
            if (cost > w.cash) {
                throw CommandError("not enough cash");
            }
            w.cash -= cost;
        });
}

} // namespace

TEST_CASE("tokenize splits on whitespace and handles quotes") {
    CHECK(texts(tokenize("  dock   Tycho  ")) == std::vector<std::string>{"dock", "Tycho"});
    CHECK(texts(tokenize("dock \"Ceres Station\" 'a \"b\"'")) ==
          std::vector<std::string>{"dock", "Ceres Station", "a \"b\""});
    CHECK(texts(tokenize(R"(say "quote \" and \\ slash")")) ==
          std::vector<std::string>{"say", R"(quote " and \ slash)"});
    // Adjacent quoted and bare parts join into one word, as in a POSIX shell.
    CHECK(texts(tokenize(R"(x pre"mid dle"post)")) == std::vector<std::string>{"x", "premid dlepost"});
    CHECK(texts(tokenize("empty \"\"")) == std::vector<std::string>{"empty", ""});
    CHECK(tokenize("dock \"Ceres Station\"")[1].column == 5);
}

TEST_CASE("tokenize drops comments only at word starts") {
    CHECK(texts(tokenize("# whole line")).empty());
    CHECK(texts(tokenize("advance 3d # three days")) == std::vector<std::string>{"advance", "3d"});
    CHECK(texts(tokenize("tag a#b \"#quoted\"")) == std::vector<std::string>{"tag", "a#b", "#quoted"});
}

TEST_CASE("tokenize marks option syntax only when unquoted") {
    const auto t = tokenize("cmd --qty=3 \"--literal\" -5 --");
    CHECK(t[1].option_syntax);
    CHECK_FALSE(t[2].option_syntax);
    CHECK_FALSE(t[3].option_syntax);
    CHECK(t[4].option_syntax);
}

TEST_CASE("tokenize reports unterminated quotes with a column") {
    CHECK(error_of([] { tokenize("dock \"Ceres"); }) == "unterminated quote starting at column 6");
    CHECK(error_of([] { tokenize("say 'oops"); }) == "unterminated quote starting at column 5");
}

TEST_CASE("quote_token round-trips through tokenize") {
    for (std::string s : {"plain", "", "two words", "--flag", "#hash", "a\"b", "back\\slash", "it's"}) {
        const auto t = tokenize("x " + quote_token(s));
        REQUIRE(t.size() == 2);
        CHECK(t[1].text == s);
        CHECK_FALSE(t[1].option_syntax);
    }
    CHECK(quote_token("plain") == "plain");
    CHECK_THROWS_AS(quote_token("line\nbreak"), std::invalid_argument);
}

TEST_CASE("durations parse with units and exact fractions") {
    auto d = [](std::string_view s) { return std::get<Duration>(parse_value(ArgType::duration, s)); };
    CHECK(d("30d") == days(30));
    CHECK(d("6h") == hours(6));
    CHECK(d("90m") == minutes(90));
    CHECK(d("45s") == seconds(45));
    CHECK(d("2w") == days(14));
    CHECK(d("1d12h") == hours(36));
    CHECK(d("1.5h") == minutes(90));
    CHECK(d("0.25d") == hours(6));
    CHECK(d("-3d") == days(-3));
    CHECK(d("0s") == Duration{});
}

TEST_CASE("durations reject malformed input with a helpful message") {
    auto err = [](std::string_view s) { return error_of([&] { parse_value(ArgType::duration, s); }); };
    CHECK(err("abc") == "expected duration like 30d, 6h or 90m, got 'abc'");
    CHECK(err("30") == "expected duration like 30d, 6h or 90m, got '30' (missing unit: w, d, h, m or s)");
    CHECK(err("3y") == "expected duration like 30d, 6h or 90m, got '3y' (unknown unit 'y')");
    CHECK(err("1.5s") == "expected duration like 30d, 6h or 90m, got '1.5s' (not a whole number of seconds)");
    CHECK(err("") == "expected duration like 30d, 6h or 90m, got ''");
    CHECK(err("d") == "expected duration like 30d, 6h or 90m, got 'd'");
    CHECK(err("99999999999999999999d") == "duration out of range: '99999999999999999999d'");
    CHECK(err("999999999999999w") == "duration out of range: '999999999999999w'");
}

TEST_CASE("accelerations parse in g and metres per second squared") {
    auto a = [](std::string_view s) { return std::get<Acceleration>(parse_value(ArgType::acceleration, s)); };
    CHECK(a("0.3g").mps2 == doctest::Approx(0.3 * standard_gravity_mps2));
    CHECK(a("1g").gees() == doctest::Approx(1.0));
    CHECK(a("2.5m/s2").mps2 == 2.5);
    CHECK(a("2.5m/s^2").mps2 == 2.5);
    CHECK(a("0g").mps2 == 0.0);
    auto err = [](std::string_view s) { return error_of([&] { parse_value(ArgType::acceleration, s); }); };
    CHECK(err("fast") == "expected acceleration like 0.3g or 2.5m/s2, got 'fast'");
    CHECK(err("0.3") == "expected acceleration like 0.3g or 2.5m/s2, got '0.3' (missing unit: g or m/s2)");
    CHECK(err("3km") == "expected acceleration like 0.3g or 2.5m/s2, got '3km' (unknown unit 'km')");
    CHECK(err("-1g") == "acceleration must not be negative, got '-1g'");
}

TEST_CASE("numbers integers and flags parse strictly") {
    CHECK(std::get<std::int64_t>(parse_value(ArgType::integer, "-42")) == -42);
    CHECK(std::get<double>(parse_value(ArgType::number, "0.25")) == 0.25);
    CHECK(std::get<double>(parse_value(ArgType::number, "1e6")) == 1e6);
    CHECK(std::get<bool>(parse_value(ArgType::flag, "off")) == false);
    CHECK(error_of([] { parse_value(ArgType::integer, "4x"); }) == "expected integer, got '4x'");
    CHECK(error_of([] { parse_value(ArgType::integer, "1.5"); }) == "expected integer, got '1.5'");
    CHECK(error_of([] { parse_value(ArgType::integer, "99999999999999999999"); }) ==
          "integer out of range: '99999999999999999999'");
    CHECK(error_of([] { parse_value(ArgType::number, "inf"); }) == "expected number like 0.25 or 12, got 'inf'");
    CHECK(error_of([] { parse_value(ArgType::number, "nan"); }) == "expected number like 0.25 or 12, got 'nan'");
    CHECK(error_of([] { parse_value(ArgType::flag, "maybe"); }) == "expected true or false, got 'maybe'");
}

TEST_CASE("format_value is canonical and parses back to the same value") {
    CHECK(format_value(days(3)) == "3d");
    CHECK(format_value(minutes(90)) == "1h30m");
    CHECK(format_value(Duration{90061}) == "1d1h1m1s");
    CHECK(format_value(Duration{}) == "0s");
    CHECK(format_value(days(-2)) == "-2d");
    CHECK(format_value(std::get<Acceleration>(parse_value(ArgType::acceleration, "0.3g"))) == "0.3g");
    CHECK(format_value(std::string("Ceres Station")) == "\"Ceres Station\"");

    const std::vector<std::pair<ArgType, ArgValue>> values{
        {ArgType::integer, std::int64_t{-9'000'000'000'000}},
        {ArgType::number, 0.1},
        {ArgType::number, 1e300},
        {ArgType::number, -0.0},
        {ArgType::number, 2.0 / 3.0},
        {ArgType::string, std::string("say \"hi\" #1")},
        {ArgType::duration, Duration{123456789}},
        {ArgType::duration, Duration{std::numeric_limits<std::int64_t>::min() + 1}},
        {ArgType::acceleration, Acceleration{0.3 * standard_gravity_mps2}},
        {ArgType::acceleration, Acceleration{1.0 / 3.0}},
        {ArgType::acceleration, Acceleration{12.345678901234567}},
        {ArgType::flag, true},
    };
    for (const auto& [type, value] : values) {
        const std::string text = format_value(value);
        const auto tokens = tokenize(text);
        REQUIRE(tokens.size() == 1);
        CHECK(parse_value(type, tokens[0].text) == value);
    }
}

TEST_CASE("registry binds positionals options defaults and flags") {
    World world;
    CommandBus<World> bus;
    register_world(bus, world);
    const CommandRegistry& reg = bus.registry();

    const Invocation inv = *reg.parse("buy water 5 --at \"Ceres Station\" --max-price=12.5 --dry-run --burn 0.3g");
    CHECK(inv.command == "buy");
    CHECK(inv.get<std::string>("item") == "water");
    CHECK(inv.get<std::int64_t>("qty") == 5);
    CHECK(inv.get<std::string>("at") == "Ceres Station");
    CHECK(inv.get<double>("max-price") == 12.5);
    CHECK(inv.get<Acceleration>("burn").gees() == doctest::Approx(0.3));
    CHECK(inv.flag("dry-run"));

    // Defaults are filled in; flags are always present; other omitted options are absent.
    const Invocation minimal = *reg.parse("buy ice");
    CHECK(minimal.get<std::int64_t>("qty") == 1);
    CHECK(minimal.has("dry-run"));
    CHECK_FALSE(minimal.flag("dry-run"));
    CHECK_FALSE(minimal.has("at"));
    CHECK(minimal.get_or<std::string>("at", "here") == "here");

    // Aliases resolve to the canonical name.
    CHECK(reg.parse("adv 3d")->command == "advance");
    // Blank and comment-only lines are not commands.
    CHECK_FALSE(reg.parse("   ").has_value());
    CHECK_FALSE(reg.parse("# nothing").has_value());
    CHECK_THROWS_AS(inv.get<std::int64_t>("item"), std::logic_error);
}

TEST_CASE("registry reports argument errors clearly") {
    World world;
    CommandBus<World> bus;
    register_world(bus, world);
    const CommandRegistry& reg = bus.registry();
    auto err = [&](std::string_view line) { return error_of([&] { reg.parse(line); }); };

    CHECK(err("advance") == "missing argument <span>; usage: advance <span>");
    CHECK(err("advance abc") == "<span>: expected duration like 30d, 6h or 90m, got 'abc'");
    CHECK(err("advance 3d 4d") == "too many arguments for 'advance' (unexpected '4d'); usage: advance <span>");
    CHECK(err("buy gold") == "<item>: expected one of water, ice, fuel; got 'gold'");
    CHECK(err("buy water --max-price") ==
          "option '--max-price' needs a number value; usage: buy <item> [<qty>] [--at <string>] "
          "[--max-price <number>] [--burn <acceleration>] [--dry-run]");
    CHECK(err("buy water --max-price --dry-run").starts_with("option '--max-price' needs a number value"));
    CHECK(err("buy water --max-prise 3") == "unknown option '--max-prise' for 'buy'; did you mean '--max-price'?");
    CHECK(err("buy water --dry-run --dry-run") == "option '--dry-run' given more than once");
    CHECK(err("buy water --qty=x").starts_with("unknown option '--qty' for 'buy'; usage: buy <item> [<qty>]"));
    CHECK(err("buy water --burn=2") == "--burn: expected acceleration like 0.3g or 2.5m/s2, got '2' (missing unit: g or m/s2)");
    CHECK(err("advance \"--oops\"") == "<span>: expected duration like 30d, 6h or 90m, got '--oops'");
    // After a lone "--", option-looking words are positionals.
    CHECK(err("advance -- --5d") == "<span>: expected duration like 30d, 6h or 90m, got '--5d'");
}

TEST_CASE("a rest argument collects the remaining words canonically") {
    CommandRegistry reg;
    reg.add({.name = "find",
             .positionals = {{.name = "table"}, {.name = "filter", .required = false, .rest = true}},
             .options = {{.name = "limit", .type = ArgType::integer}}});

    const Invocation inv = *reg.parse("find ships where rmass  < 0.2 --limit 5 'Ceres Station'");
    CHECK(inv.get<std::string>("filter") == "where rmass < 0.2 \"Ceres Station\"");
    CHECK(texts(tokenize(inv.get<std::string>("filter"))) ==
          std::vector<std::string>{"where", "rmass", "<", "0.2", "Ceres Station"});
    CHECK(inv.get<std::int64_t>("limit") == 5);
    CHECK(reg.format(inv) == "find ships where rmass < 0.2 \"Ceres Station\" --limit 5");
    CHECK(*reg.parse(reg.format(inv)) == inv);
    CHECK(reg.usage(reg.at("find")) == "find <table> [<filter>...] [--limit <integer>]");
    CHECK_FALSE(reg.parse("find ships")->has("filter"));

    Invocation odd = inv;
    odd.args[1].value = std::string("not  canonical");
    CHECK_THROWS_AS(reg.format(odd), std::invalid_argument);
    CHECK_THROWS_AS(reg.add({.name = "bad", .positionals = {{.name = "a", .rest = true}, {.name = "b"}}}),
                    std::invalid_argument);
}

TEST_CASE("unknown commands get did-you-mean suggestions") {
    World world;
    CommandBus<World> bus;
    register_world(bus, world);
    const CommandRegistry& reg = bus.registry();

    CHECK(error_of([&] { reg.parse("advnce 3d"); }) == "unknown command 'advnce'; did you mean 'advance'?");
    CHECK(error_of([&] { reg.parse("stauts"); }) == "unknown command 'stauts'; did you mean 'status'?");
    CHECK(error_of([&] { reg.parse("xyzzy"); }) == "unknown command 'xyzzy'; type 'help' for a list");
    CHECK(error_of([&] { reg.parse("--help"); }).starts_with("unknown command '--help'"));
    // Prefixes are suggested (but never silently executed).
    CHECK(reg.suggest("h") == std::vector<std::string>{"help", "history"});
    CHECK(reg.suggest("ADV") == std::vector<std::string>{"adv", "advance"});
    CHECK(edit_distance("kitten", "sitting") == 3);
    CHECK(edit_distance("ab", "ba") == 1);
}

TEST_CASE("registration rejects malformed specs") {
    CommandRegistry reg;
    reg.add({.name = "go", .aliases = {"g"}});
    CHECK_THROWS_AS(reg.add({.name = "go"}), std::invalid_argument);
    CHECK_THROWS_AS(reg.add({.name = "other", .aliases = {"g"}}), std::invalid_argument);
    CHECK_THROWS_AS(reg.add({.name = "two words"}), std::invalid_argument);
    CHECK_THROWS_AS(reg.add({.name = "a", .positionals = {{.name = "x", .required = false}, {.name = "y"}}}),
                    std::invalid_argument);
    CHECK_THROWS_AS(reg.add({.name = "b", .positionals = {{.name = "f", .type = ArgType::flag}}}),
                    std::invalid_argument);
    CHECK_THROWS_AS(reg.add({.name = "c", .positionals = {{.name = "d", .type = ArgType::duration,
                                                           .default_value = "soon"}}}),
                    std::invalid_argument);
    CHECK_THROWS_AS(reg.add({.name = "d", .positionals = {{.name = "x"}}, .options = {{.name = "x"}}}),
                    std::invalid_argument);
    CHECK(reg.find("g") == reg.find("go"));
}

TEST_CASE("help lists commands and describes one command") {
    World world;
    CommandBus<World> bus;
    register_world(bus, world);
    Doc out;

    CHECK(bus.execute_line("help", world, out).status == LineResult::Status::ok);
    const std::string overview = to_text(out);
    CHECK(overview.find("Commands:\n") == 0);
    CHECK(overview.find("  advance  Advance simulation time (alias: adv)\n") != std::string::npos);
    CHECK(overview.find("  quit     Leave the shell (alias: exit)\n") != std::string::npos);
    CHECK(overview.find("Type 'help <command>' for details.") != std::string::npos);
    // Sorted by name.
    CHECK(overview.find("advance") < overview.find("buy"));

    out = Doc();
    CHECK(bus.execute_line("help adv", world, out).ok());
    CHECK(to_text(out) ==
          "usage: advance <span>\n"
          "  Advance simulation time\n"
          "aliases: adv\n"
          "Changes the simulation; recorded in the session log for replay.\n"
          "arguments:\n"
          "  <span>  duration      how far to advance\n");

    out = Doc();
    CHECK(bus.execute_line("help buy", world, out).ok());
    CHECK(to_text(out).find("  <qty>        integer       units (default: 1)\n") != std::string::npos);
    CHECK(to_text(out).find("  <item>       string        commodity [water|ice|fuel]\n") != std::string::npos);
    CHECK(to_text(out).find("  --dry-run    flag          only show the cost\n") != std::string::npos);

    const LineResult bad = bus.execute_line("help advnce", world, out);
    CHECK(bad.status == LineResult::Status::error);
    CHECK(bad.error == "unknown command 'advnce'; did you mean 'advance'?");
}

TEST_CASE("actions are queued and applied in order while queries run immediately") {
    World world;
    CommandBus<World> bus;
    register_world(bus, world);
    Doc out;

    CHECK(bus.submit_line("advance 1d", world, out).ok());
    CHECK(bus.submit_line("buy water 2", world, out).ok());
    CHECK(bus.pending() == 2);
    CHECK(world.now == 0); // nothing applied yet

    CHECK(bus.submit_line("status", world, out).ok());
    CHECK(to_text(out) == "t=0 cash=100\n");

    const auto applied = bus.apply_pending(world, out);
    CHECK(applied.applied == 2);
    CHECK(applied.errors.empty());
    CHECK(world.now == 86400);
    CHECK(world.cash == 80);
    CHECK(bus.log().size() == 2);
    CHECK(bus.pending() == 0);

    // Queries and parse errors never reach the log; failed actions do, with their error.
    CHECK(bus.execute_line("status", world, out).ok());
    CHECK(bus.execute_line("advnce 3d", world, out).status == LineResult::Status::error);
    const LineResult poor = bus.execute_line("buy fuel 50", world, out);
    CHECK(poor.status == LineResult::Status::error);
    CHECK(poor.error == "not enough cash");
    REQUIRE(bus.log().size() == 3);
    CHECK(bus.log().back().error == "not enough cash");

    CHECK_THROWS_AS(bus.submit(Invocation{"status", {}}), std::invalid_argument);
}

TEST_CASE("command log round-trips through a script") {
    World world;
    CommandBus<World> bus;
    register_world(bus, world);
    Doc out;
    for (const char* line : {"advance 1.5d", "buy water", "buy ice 3 --at \"Ceres Station\" --burn 0.3g",
                             "status", "adv 90m  # comment", "buy fuel 99", "buy water 1 --max-price 0.1 --dry-run"}) {
        bus.execute_line(line, world, out);
    }
    REQUIRE(bus.log().size() == 6);

    std::ostringstream script;
    bus.write_script(script);
    CHECK(script.str() ==
          "advance 1d12h\n"
          "buy water 1\n"
          "buy ice 3 --at \"Ceres Station\" --burn 0.3g\n"
          "advance 1h30m\n"
          "buy fuel 99  # failed: not enough cash\n"
          "buy water 1 --max-price 0.1 --dry-run\n");

    const std::vector<Invocation> reparsed = parse_script(bus.registry(), script.str());
    REQUIRE(reparsed.size() == bus.log().size());
    for (std::size_t i = 0; i < reparsed.size(); ++i) {
        CHECK(reparsed[i] == bus.log()[i].invocation);
        CHECK(bus.registry().format(reparsed[i]) == bus.registry().format(bus.log()[i].invocation));
    }

    // Replaying the script into a fresh world reproduces the state and the log.
    World replay_world;
    CommandBus<World> replay_bus;
    register_world(replay_bus, replay_world);
    std::istringstream in(script.str());
    StreamLineReader reader(in, "replay.txt");
    std::ostringstream replay_out;
    CHECK(run_repl(replay_bus, replay_world, reader, replay_out) == 0);
    CHECK(replay_world.now == world.now);
    CHECK(replay_world.cash == world.cash);
    CHECK(replay_bus.log() == bus.log());
    CHECK(replay_out.str() == "replay.txt:5: error: not enough cash\nwould cost 10\n");
}

TEST_CASE("command log entries serialize for snapshots") {
    World world;
    CommandBus<World> bus;
    register_world(bus, world);
    Doc out;
    bus.execute_line("buy ice 2 --at Tycho --burn 1.5m/s2 --max-price 3.25", world, out);
    bus.execute_line("advance 6h", world, out);

    Writer w;
    encode(w, bus.log());
    Reader r(w.bytes());
    const auto decoded = decode_as<std::vector<LoggedCommand>>(r);
    r.expect_end();
    CHECK(decoded == bus.log());
}

TEST_CASE("parse_script reports the failing line") {
    CommandBus<int> bus;
    CHECK(error_of([&] { parse_script(bus.registry(), "help\n\n# c\nhlep\n"); }) ==
          "line 4: unknown command 'hlep'; did you mean 'help'?");
}

TEST_CASE("repl output is captured and stops on error in batch mode") {
    World world;
    CommandBus<World> bus;
    register_world(bus, world);

    std::istringstream in("status\nadvance 2h\n\nstatus\nbogus\nstatus\n");
    StreamLineReader reader(in, "s.txt");
    std::ostringstream out;
    CHECK(run_repl(bus, world, reader, out, {.stop_on_error = true}) == 1);
    CHECK(out.str() ==
          "t=0 cash=100\n"
          "t=7200 cash=100\n"
          "s.txt:5: error: unknown command 'bogus'; type 'help' for a list\n");

    // Interactive-style: errors are reported and the loop continues until quit.
    std::istringstream in2("bogus\nstatus\nquit\nstatus\n");
    StreamLineReader echoing(in2, {}, nullptr);
    std::ostringstream out2;
    CHECK(run_repl(bus, world, echoing, out2) == 0);
    CHECK(out2.str() == "error: unknown command 'bogus'; type 'help' for a list\nt=7200 cash=100\n");
    CHECK(bus.quit_requested());
}

TEST_CASE("stream reader echoes lines for transcripts") {
    std::istringstream in("one\r\ntwo\n");
    std::ostringstream echo;
    StreamLineReader reader(in, "f", &echo);
    CHECK(reader.read_line("> ") == "one");
    CHECK(reader.location() == "f:1");
    CHECK(reader.read_line("> ") == "two");
    CHECK_FALSE(reader.read_line("> ").has_value());
    CHECK(echo.str() == "> one\n> two\n");
}

TEST_CASE("completion offers commands options and argument values") {
    World world;
    CommandBus<World> bus;
    register_world(bus, world);
    const CommandRegistry& reg = bus.registry();

    Completion c = reg.complete("ad");
    CHECK(c.replace_from == 0);
    CHECK(c.candidates == std::vector<std::string>{"adv", "advance"});

    c = reg.complete("buy ");
    CHECK(c.replace_from == 4);
    CHECK(c.candidates == std::vector<std::string>{"fuel", "ice", "water"});

    c = reg.complete("buy water 3 --m");
    CHECK(c.replace_from == 12);
    CHECK(c.candidates == std::vector<std::string>{"--max-price"});

    c = reg.complete("buy water --at ce");
    CHECK(c.replace_from == 15);
    CHECK(c.candidates == std::vector<std::string>{"\"Ceres Station\""});

    c = reg.complete("buy water --at \"Ceres S");
    CHECK(c.replace_from == 15);
    CHECK(c.candidates == std::vector<std::string>{"\"Ceres Station\""});

    c = reg.complete("buy water --at=T");
    CHECK(c.replace_from == 15);
    CHECK(c.candidates == std::vector<std::string>{"Tycho"});

    c = reg.complete("help st");
    CHECK(c.candidates == std::vector<std::string>{"status"});

    CHECK(reg.complete("nosuch ").candidates.empty());
    CHECK(reg.complete("advance 3d # ").candidates.empty());
}

TEST_CASE("after line observer sees every non empty line") {
    World world;
    CommandBus<World> bus;
    register_world(bus, world);
    std::vector<std::pair<std::int64_t, bool>> seen; // (world time, ok) after each line
    bus.on_after_line([&](const World& w, const LineResult& r) { seen.emplace_back(w.now, r.ok()); });

    std::istringstream in("status\n\nadvance 2h\nbogus\n");
    StreamLineReader reader(in);
    std::ostringstream out;
    run_repl(bus, world, reader, out);
    REQUIRE(seen.size() == 3); // the blank line is skipped
    CHECK(seen[0] == std::pair<std::int64_t, bool>{0, true});
    CHECK(seen[1] == std::pair<std::int64_t, bool>{7200, true}); // after the action applied
    CHECK(seen[2] == std::pair<std::int64_t, bool>{7200, false});
}
