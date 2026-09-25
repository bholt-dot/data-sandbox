#include "tui.hpp"

#include <algorithm>
#include <format>
#include <utility>

#include <ftxui/component/app.hpp>
#include <ftxui/component/mouse.hpp>
#include <ftxui/screen/terminal.hpp>

#include "doc_view.hpp"

namespace belter {

using namespace ftxui;
using sim::Style;
using sim::styled;

namespace {

constexpr std::size_t max_pinned = 3;
constexpr std::size_t wheel_rows = 3;

std::string common_prefix(const std::vector<std::string>& words) {
    std::string prefix = words.empty() ? std::string() : words.front();
    for (const std::string& w : words) {
        std::size_t n = 0;
        while (n < prefix.size() && n < w.size() && prefix[n] == w[n]) {
            ++n;
        }
        prefix.resize(n);
    }
    return prefix;
}

std::vector<sim::Line> wrap(const sim::Line& line, std::size_t width) {
    sim::Doc doc;
    doc << line;
    return sim::layout(doc, width);
}

// Packs banner segments into rows no wider than `width`, keeping each segment whole.
std::vector<sim::Line> pack(const std::vector<sim::Line>& segments, std::size_t width) {
    std::vector<sim::Line> rows(1);
    std::size_t used = 0;
    for (const sim::Line& seg : segments) {
        const std::size_t w = seg.width();
        if (used > 0 && used + 3 + w > width) {
            rows.emplace_back();
            used = 0;
        }
        if (used > 0) {
            rows.back().append(styled(Style::dim, " │ "));
            used += 3;
        }
        for (const sim::Span& s : seg.spans) {
            rows.back().append(s);
        }
        used += w;
    }
    return rows;
}

std::size_t to_size(int v) { return static_cast<std::size_t>(std::max(v, 0)); }

} // namespace

Tui::Tui(expanse::ShellBus& bus, expanse::Session& session, TuiOptions options)
    : bus_(bus), session_(session), options_(options) {
    InputOption opt;
    opt.content = &input_;
    opt.cursor_position = &cursor_;
    opt.multiline = false;
    opt.placeholder = "type a command; 'help' lists them";
    opt.transform = [](InputState state) {
        return state.is_placeholder ? state.element | dim : state.element;
    };
    input_component_ = Input(opt);
    root_ = CatchEvent(Renderer(input_component_,
                                [this] {
                                    const Dimensions size = Terminal::Size();
                                    return view(size.dimx, size.dimy);
                                }),
                       [this](Event e) { return on_event(std::move(e)); });

    sim::Doc hello;
    hello << styled(Style::heading, "BELTER") << " — a hard-SF trading sim. '" << styled(Style::key, "new")
          << "' starts a game, '" << styled(Style::key, "help") << "' lists commands.\n";
    push(std::move(hello));
}

int Tui::run() {
    App app = App::Fullscreen();
    app.ForceHandleCtrlC(false); // Ctrl+C reaches on_event and ends the loop normally
    exit_ = app.ExitLoopClosure();
    app.Loop(root_);
    exit_ = nullptr;
    return 0;
}

void Tui::exit_loop() {
    quit_ = true;
    if (exit_) {
        exit_();
    }
}

void Tui::execute(std::string_view line) {
    const std::string text(line);
    if (text.find_first_not_of(" \t") == std::string::npos) {
        return;
    }
    if (history_.empty() || history_.back() != text) {
        history_.push_back(text);
    }
    history_pos_ = history_.size();
    draft_.clear();

    sim::Doc doc;
    doc << styled(Style::dim, "> ") << styled(Style::emphasis, text) << "\n";
    const std::size_t logged = bus_.log().size();
    const sim::LineResult r = bus_.execute_line(text, session_, doc);
    if (r.status == sim::LineResult::Status::error) {
        doc << styled(Style::bad, "error:") << " " << r.error << "\n";
    }
    // A new or loaded world has its own journal: nothing in it is acknowledged yet.
    if (bus_.log().size() > logged) {
        const std::string& cmd = bus_.log().back().invocation.command;
        if (cmd == "new" || cmd == "load") {
            acknowledged_ = 0;
        }
    }
    push(std::move(doc));
    scroll_ = 0;
    if (r.status == sim::LineResult::Status::quit) {
        exit_loop();
    }
}

bool Tui::on_event(Event e) {
    if (e == Event::CtrlC || (e == Event::CtrlD && input_.empty())) {
        exit_loop();
        return true;
    }
    if (e == Event::Return) {
        const std::string line = std::exchange(input_, {});
        cursor_ = 0;
        candidates_.clear();
        execute(line);
        return true;
    }
    if (e == Event::Tab) {
        complete();
        return true;
    }
    if (e == Event::ArrowUp || e == Event::ArrowDown) {
        history_step(e == Event::ArrowUp ? -1 : 1);
        return true;
    }
    const std::size_t page = to_size(std::max(page_ - 1, 1));
    if (e == Event::PageUp) {
        scroll_ += page;
        return true;
    }
    if (e == Event::PageDown) {
        scroll_ -= std::min(scroll_, page);
        return true;
    }
    if (e == Event::Escape) {
        if (!input_.empty()) {
            input_.clear();
            cursor_ = 0;
        } else if (session_.world) {
            acknowledged_ = session_.world->messages.size();
        }
        candidates_.clear();
        return true;
    }
    if (e.is_mouse()) {
        const Mouse m = e.mouse();
        if (m.button == Mouse::WheelUp) {
            scroll_ += wheel_rows;
            return true;
        }
        if (m.button == Mouse::WheelDown) {
            scroll_ -= std::min(scroll_, wheel_rows);
            return true;
        }
        return false;
    }
    candidates_.clear(); // any edit makes the last completion list stale
    return false;
}

void Tui::history_step(int delta) {
    if (history_.empty()) {
        return;
    }
    if (history_pos_ == history_.size()) {
        draft_ = input_;
    }
    if (delta < 0 && history_pos_ > 0) {
        --history_pos_;
    } else if (delta > 0 && history_pos_ < history_.size()) {
        ++history_pos_;
    } else {
        return;
    }
    input_ = history_pos_ == history_.size() ? draft_ : history_[history_pos_];
    cursor_ = static_cast<int>(input_.size());
}

void Tui::complete() {
    const std::size_t cur = std::min(to_size(cursor_), input_.size());
    const std::string before = input_.substr(0, cur);
    const sim::Completion c = bus_.registry().complete(before);
    candidates_.clear();
    if (c.candidates.empty()) {
        return;
    }
    const std::size_t typed = cur - c.replace_from;
    std::string replacement;
    if (c.candidates.size() == 1) {
        replacement = c.candidates.front() + " ";
    } else {
        candidates_ = c.candidates;
        replacement = common_prefix(c.candidates);
        if (replacement.size() <= typed) {
            return; // matches are case-insensitive: never shorten what was typed
        }
    }
    input_.replace(c.replace_from, typed, replacement);
    cursor_ = static_cast<int>(c.replace_from + replacement.size());
}

void Tui::push(sim::Doc doc) {
    entries_.push_back(std::move(doc));
    if (entries_.size() > options_.max_entries) {
        entries_.erase(entries_.begin(), entries_.begin() + static_cast<std::ptrdiff_t>(options_.max_entries / 10 + 1));
        rows_width_ = 0; // relayout on the next frame
        return;
    }
    if (rows_width_ > 0) {
        for (sim::Line& row : sim::layout(entries_.back(), rows_width_)) {
            rows_.push_back(std::move(row));
        }
    }
}

void Tui::relayout(std::size_t width) {
    rows_.clear();
    for (const sim::Doc& doc : entries_) {
        for (sim::Line& row : sim::layout(doc, width)) {
            rows_.push_back(std::move(row));
        }
    }
    rows_width_ = width;
}

std::vector<std::size_t> Tui::pinned() const {
    std::vector<std::size_t> out;
    if (!session_.world) {
        return out;
    }
    const auto& msgs = session_.world->messages;
    const sim::Time now = session_.world->now();
    for (std::size_t i = msgs.size(); i > acknowledged_ && out.size() < max_pinned; --i) {
        const expanse::Message& m = msgs[i - 1];
        if (m.time + options_.pin_for <= now) {
            break; // the journal is in time order
        }
        if (m.urgent) {
            out.push_back(i - 1);
        }
    }
    return out;
}

std::vector<sim::Line> Tui::banner(int width) const {
    std::vector<sim::Line> segments;
    if (const auto b = expanse::status_banner(session_)) {
        segments = expanse::banner_segments(*b);
    } else {
        segments.emplace_back(std::vector<sim::Span>{styled(Style::heading, "BELTER"),
                                                     styled(Style::dim, " no game in progress")});
    }
    return pack(segments, to_size(width - 2));
}

Element Tui::journal(int width, int height) const {
    const auto w = to_size(width);
    const auto h = to_size(height);
    Elements out;
    std::vector<std::size_t> pins = pinned();
    sim::Line title(styled(Style::heading, "Journal"));
    if (!pins.empty()) {
        title.append(styled(Style::dim, "  Esc: acknowledge"));
    }
    out.push_back(sim::tui::row(title, options_.color));
    std::size_t used = 1;
    if (!session_.world) {
        out.push_back(text("(no game)") | dim);
        return vbox(std::move(out)) | size(WIDTH, EQUAL, width) | size(HEIGHT, EQUAL, height);
    }
    const auto& msgs = session_.world->messages;
    for (const std::size_t i : pins) {
        for (const sim::Line& r : wrap(expanse::journal_line(msgs[i], true), w)) {
            if (used + 2 < h) {
                out.push_back(sim::tui::row(r, options_.color));
                ++used;
            }
        }
    }
    if (!pins.empty()) {
        out.push_back(separatorLight());
        ++used;
    }
    // The latest entries, newest at the bottom.
    std::vector<sim::Line> recent;
    for (std::size_t i = msgs.size(); i > 0; --i) {
        if (std::find(pins.begin(), pins.end(), i - 1) != pins.end()) {
            continue;
        }
        std::vector<sim::Line> rows = wrap(expanse::journal_line(msgs[i - 1], true), w);
        if (used + recent.size() + rows.size() > h) {
            break;
        }
        recent.insert(recent.begin(), rows.begin(), rows.end());
    }
    out.push_back(filler());
    for (Element& e : sim::tui::rows(recent, options_.color)) {
        out.push_back(std::move(e));
    }
    return vbox(std::move(out)) | size(WIDTH, EQUAL, width) | size(HEIGHT, EQUAL, height);
}

Element Tui::output(int width, int height) {
    const auto w = to_size(width);
    const auto h = to_size(height);
    if (w != rows_width_) {
        relayout(w);
    }
    page_ = height;
    scroll_ = std::min(scroll_, rows_.size() > h ? rows_.size() - h : 0);
    const std::size_t end = rows_.size() - scroll_;
    const std::size_t begin = end > h ? end - h : 0;
    const std::vector<sim::Line> visible(rows_.begin() + static_cast<std::ptrdiff_t>(begin),
                                         rows_.begin() + static_cast<std::ptrdiff_t>(end));
    return vbox(sim::tui::rows(visible, options_.color)) | size(WIDTH, EQUAL, width) |
           size(HEIGHT, EQUAL, height);
}

Element Tui::hint(int width) const {
    sim::Line line;
    if (!candidates_.empty()) {
        for (const std::string& c : candidates_) {
            if (line.width() + c.size() + 2 > to_size(width)) {
                line.append(styled(Style::dim, "…"));
                break;
            }
            line.append(styled(Style::key, c));
            line.append({"  ", Style::plain});
        }
    } else if (scroll_ > 0) {
        line.append(styled(Style::warning, std::format("-- {} lines below; PgDn to return --", scroll_)));
    } else {
        line.append(styled(Style::dim,
                           "Tab complete · Up/Down history · PgUp/PgDn scroll · Esc clear/ack · Ctrl+C quit"));
    }
    return sim::tui::row(line, options_.color);
}

Element Tui::view(int width, int height) {
    width = std::max(width, 20);
    height = std::max(height, 8);
    Elements top;
    for (const sim::Line& r : banner(width)) {
        top.push_back(hbox({text(" "), sim::tui::row(r, options_.color)}));
    }
    const int middle = std::max(height - static_cast<int>(top.size()) - 4, 1);

    Element body;
    if (width >= options_.wide_min_width) {
        const int jw = std::clamp(width / 3, 30, 56);
        const int ow = width - jw - 1;
        body = hbox({output(ow, middle), separator(), journal(jw, middle)});
    } else {
        // No room for the journal: urgent entries sit above the output instead.
        Elements rows;
        const auto limit = to_size(middle / 3);
        if (session_.world) {
            for (const std::size_t i : pinned()) {
                const auto wrapped = wrap(expanse::journal_line(session_.world->messages[i], true), to_size(width));
                for (std::size_t r = 0; r < std::min<std::size_t>(wrapped.size(), 2) && rows.size() < limit; ++r) {
                    rows.push_back(sim::tui::row(wrapped[r], options_.color));
                }
            }
        }
        if (!rows.empty()) {
            rows.push_back(separatorLight());
        }
        const int strip = static_cast<int>(rows.size());
        rows.push_back(output(width, std::max(middle - strip, 1)));
        body = vbox(std::move(rows));
    }

    return vbox({
        vbox(std::move(top)),
        separator(),
        body,
        separator(),
        hbox({text("> ") | bold | sim::tui::decorator(Style::key, options_.color), input_component_->Render() | flex}),
        hint(width),
    });
}

} // namespace belter
