#pragma once

// Internal: an owned, parsed TOML document. Keeps toml++ out of every header.

#include "simcore/data_node.hpp"

#include <memory>
#include <optional>
#include <string_view>

namespace sim::detail {

class TomlDocument {
public:
    // Parses `text`; `name` is recorded in every node's source location. On a syntax error the
    // error is reported to `diags` and nullopt is returned (toml++ stops at the first one).
    static std::optional<TomlDocument> parse(std::string_view name, std::string_view text,
                                             Diagnostics& diags);

    TomlDocument(TomlDocument&&) noexcept;
    TomlDocument& operator=(TomlDocument&&) noexcept;
    ~TomlDocument();

    // Valid for the lifetime of this document; moving the document keeps it valid.
    DataNode root() const;

private:
    struct Impl;
    explicit TomlDocument(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> impl_;
};

} // namespace sim::detail
