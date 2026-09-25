#include "simcore/defs.hpp"

#include "toml_document.hpp"

#include <algorithm>
#include <fstream>
#include <map>
#include <sstream>
#include <system_error>

namespace sim {

// --- ReadContext ---

std::string ReadContext::path() const {
    std::string out;
    for (const std::string& seg : path_) {
        if (!out.empty() && seg.front() != '[') {
            out += '.';
        }
        out += seg;
    }
    return out;
}

std::string ReadContext::path_with(std::string_view key) const {
    std::string out = path();
    if (!out.empty()) {
        out += '.';
    }
    out += key;
    return out;
}

std::string ReadContext::prefix() const {
    if (path_.empty()) {
        return subject_;
    }
    return std::format("{}: field '{}'", subject_, path());
}

void ReadContext::error(const SourceLoc& where, std::string_view message) {
    diags_.error(where, std::format("{}: {}", prefix(), message));
}

void ReadContext::error_in_subject(const SourceLoc& where, std::string_view message) {
    diags_.error(where, std::format("{}: {}", subject_, message));
}

void ReadContext::type_error(DataNode got, std::string_view expected) {
    error(got.loc(), std::format("expected {}, got {}", expected, got.describe()));
}

void ReadContext::defer_ref(std::type_index target, std::string key, SourceLoc where, RefWriter write) {
    pending_.push_back({target, std::move(key), std::move(where), prefix(), std::move(write)});
}

// --- directory reading ---

std::vector<DataSource> read_data_directory(const std::filesystem::path& dir, Diagnostics& diags) {
    namespace fs = std::filesystem;
    std::vector<DataSource> out;
    std::error_code ec;
    if (!fs::is_directory(dir, ec)) {
        diags.error({dir.generic_string()}, "data directory does not exist or is not a directory");
        return out;
    }
    std::vector<fs::path> files;
    for (fs::recursive_directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
        if (it->is_regular_file(ec) && it->path().extension() == ".toml") {
            files.push_back(it->path());
        }
    }
    if (ec) {
        diags.error({dir.generic_string()}, "cannot list data directory: " + ec.message());
        return out;
    }
    // Directory iteration order is filesystem-dependent; sort for deterministic load order.
    std::sort(files.begin(), files.end(), [&](const fs::path& a, const fs::path& b) {
        return a.lexically_relative(dir).generic_string() < b.lexically_relative(dir).generic_string();
    });
    for (const fs::path& p : files) {
        std::ifstream in(p, std::ios::binary);
        std::ostringstream text;
        text << in.rdbuf();
        if (!in) {
            diags.error({p.generic_string()}, "cannot read file");
            continue;
        }
        out.push_back({p.generic_string(), std::move(text).str()});
    }
    return out;
}

// --- DefRegistry ---

namespace {

bool is_valid_key(std::string_view key) {
    if (key.empty() || key.front() < 'a' || key.front() > 'z') {
        return false;
    }
    return std::all_of(key.begin(), key.end(), [](char c) {
        return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_';
    });
}

constexpr std::string_view key_rule = "lowercase letters, digits and '_', starting with a letter";

SourceLoc best_loc(DataNode node, const SourceLoc& fallback) {
    SourceLoc loc = node.loc();
    return loc.line != 0 ? loc : fallback;
}

} // namespace

void DefRegistry::check_new_type(const std::string& section, std::type_index type) const {
    if (!is_valid_key(section)) {
        throw std::logic_error("sim::DefRegistry: invalid section name '" + section + "'");
    }
    for (const auto& t : types_) {
        if (t->section() == section) {
            throw std::logic_error("sim::DefRegistry: section '" + section + "' defined twice");
        }
        if (t->type() == type) {
            throw std::logic_error("sim::DefRegistry: type already defined as '" + t->section() + "'");
        }
    }
}

detail::DefTypeBase& DefRegistry::find_type(std::type_index type) const {
    for (const auto& t : types_) {
        if (t->type() == type) {
            return *t;
        }
    }
    throw std::logic_error(std::string("sim::DefRegistry: type not defined: ") + type.name());
}

Diagnostics DefRegistry::load(std::span<const DataSource> sources) {
    Diagnostics diags;
    ReadContext ctx(diags);

    std::vector<std::string_view> sections;
    for (const auto& t : types_) {
        t->begin();
        sections.emplace_back(t->section());
    }
    auto type_index_of = [&](std::string_view section) -> std::optional<std::size_t> {
        for (std::size_t i = 0; i < types_.size(); ++i) {
            if (types_[i]->section() == section) {
                return i;
            }
        }
        return std::nullopt;
    };

    // Pass 1: parse and read every row. Documents are dropped after each file: rows copy what
    // they need and deferred references keep their key and location.
    std::vector<std::map<std::string, SourceLoc, std::less<>>> first_seen(types_.size());
    for (const DataSource& src : sources) {
        auto doc = detail::TomlDocument::parse(src.name, src.text, diags);
        if (!doc) {
            continue;
        }
        doc->root().for_each([&](std::string_view section, const SourceLoc& section_loc, DataNode defs) {
            const auto ti = type_index_of(section);
            if (!ti) {
                diags.error(section_loc, std::format("unknown definition type '{}'{}", section,
                                                     did_you_mean(section, sections)));
                return;
            }
            if (!defs.is_table()) {
                diags.error(best_loc(defs, section_loc),
                            std::format("'{}' must contain definitions written as [{}.<key>], got {}",
                                        section, section, defs.describe()));
                return;
            }
            defs.for_each([&](std::string_view key, const SourceLoc& key_loc, DataNode def) {
                const SourceLoc where = best_loc(def, key_loc);
                if (!is_valid_key(key)) {
                    diags.error(key_loc, std::format("invalid {} key '{}': keys use {}", section, key, key_rule));
                    return;
                }
                if (!def.is_table()) {
                    diags.error(where, std::format("{} '{}' must be a table, got {}", section, key,
                                                   def.describe()));
                    return;
                }
                auto [it, inserted] = first_seen[*ti].try_emplace(std::string(key), where);
                if (!inserted) {
                    // Hook for mod overrides: a layered load (base game, then mods) would replace
                    // or patch the earlier row here instead of rejecting it.
                    diags.error(where, std::format("duplicate {} '{}' (first defined at {})", section,
                                                   key, it->second.to_string()));
                    return;
                }
                ctx.set_subject(std::format("{} '{}'", section, key));
                types_[*ti]->stage(std::string(key), where, def, ctx);
            });
        });
    }

    // Pass 2: fix ids (key order), then resolve references now that every key is known.
    for (const auto& t : types_) {
        t->assign_ids();
    }
    for (ReadContext::PendingRef& ref : ctx.pending_refs()) {
        const auto it = std::find_if(types_.begin(), types_.end(),
                                     [&](const auto& t) { return t->type() == ref.target; });
        if (it == types_.end()) {
            diags.error(ref.where, std::format("{}: refers to a definition type that is not registered "
                                               "(DefRegistry::define was not called for it)",
                                               ref.prefix));
            continue;
        }
        const detail::DefTypeBase& target = **it;
        if (const auto id = target.find_staged(ref.key)) {
            ref.write(id->first, id->second);
            continue;
        }
        const std::span<const std::string> keys = target.staged_keys();
        const std::vector<std::string_view> candidates(keys.begin(), keys.end());
        diags.error(ref.where, std::format("{}: unknown {} '{}'{}", ref.prefix, target.section(), ref.key,
                                           did_you_mean(ref.key, candidates)));
    }

    for (const auto& t : types_) {
        if (diags.ok()) {
            t->commit();
        } else {
            t->discard();
        }
    }

    std::vector<std::string> order;
    order.reserve(sources.size());
    for (const DataSource& src : sources) {
        order.push_back(src.name);
    }
    diags.sort(order);
    return diags;
}

Diagnostics DefRegistry::load_directory(const std::filesystem::path& dir) {
    Diagnostics diags;
    const std::vector<DataSource> sources = read_data_directory(dir, diags);
    if (!diags.ok()) {
        return diags;
    }
    return load(sources);
}

} // namespace sim
