#include "line_editor.hpp"

#include <cstdio>
#include <iostream>
#include <optional>
#include <string>
#include <utility>

#ifdef _WIN32
#include <io.h>
#define SIM_ISATTY _isatty
#define SIM_FILENO _fileno
#else
#include <unistd.h>
#define SIM_ISATTY isatty
#define SIM_FILENO fileno
#endif

#if SIM_HAVE_ISOCLINE
#include <isocline.h>
#endif

namespace sim {

bool is_interactive_terminal() {
    return SIM_ISATTY(SIM_FILENO(stdin)) != 0 && SIM_ISATTY(SIM_FILENO(stdout)) != 0;
}

#if SIM_HAVE_ISOCLINE

namespace {

// isocline keeps global state, so at most one reader should be alive at a time.
class IsoclineReader final : public LineReader {
public:
    explicit IsoclineReader(CompleteFn complete) : complete_(std::move(complete)) {
        ic_set_prompt_marker("", "");
        ic_enable_multiline(false); // one command per line, as in scripts
        // Inline hints assume a candidate extends the typed bytes verbatim, which does not hold
        // for case-insensitive or quoted candidates; the tab menu handles those correctly.
        ic_enable_hint(false);
        ic_set_history(nullptr, -1); // in-memory history
        ic_set_default_completer(&IsoclineReader::on_complete, this);
    }

    ~IsoclineReader() override { ic_set_default_completer(nullptr, nullptr); }

    IsoclineReader(const IsoclineReader&) = delete;
    IsoclineReader& operator=(const IsoclineReader&) = delete;

    std::optional<std::string> read_line(std::string_view prompt) override {
        const std::string p(prompt);
        char* raw = ic_readline(p.c_str());
        if (raw == nullptr) {
            return std::nullopt; // Ctrl-D / Ctrl-C
        }
        std::string line(raw);
        ic_free(raw);
        return line;
    }

private:
    static void on_complete(ic_completion_env_t* cenv, const char* prefix) {
        auto* self = static_cast<IsoclineReader*>(ic_completion_arg(cenv));
        if (self == nullptr || !self->complete_) {
            return;
        }
        const std::string_view before(prefix);
        const Completion c = self->complete_(before);
        const auto delete_before = static_cast<long>(before.size() - c.replace_from);
        for (const std::string& candidate : c.candidates) {
            if (!ic_add_completion_prim(cenv, candidate.c_str(), nullptr, nullptr, delete_before, 0)) {
                break;
            }
        }
    }

    CompleteFn complete_;
};

} // namespace

std::unique_ptr<LineReader> make_terminal_reader(CompleteFn complete) {
    if (!is_interactive_terminal()) {
        return nullptr;
    }
    return std::make_unique<IsoclineReader>(std::move(complete));
}

#else

namespace {

// Without a line editor: plain getline, but still prompt since a person is typing.
class PromptingReader final : public LineReader {
public:
    std::optional<std::string> read_line(std::string_view prompt) override {
        std::cout << prompt << std::flush;
        std::string line;
        if (!std::getline(std::cin, line)) {
            return std::nullopt;
        }
        return line;
    }
};

} // namespace

std::unique_ptr<LineReader> make_terminal_reader(CompleteFn) {
    if (!is_interactive_terminal()) {
        return nullptr;
    }
    return std::make_unique<PromptingReader>();
}

#endif

} // namespace sim
