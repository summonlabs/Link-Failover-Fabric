// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// Internal filesystem helpers. Not part of the installed interface.
#pragma once

#include "lff/outcome.hpp"

#include <cstdint>
#include <filesystem>
#include <vector>

namespace lff::detail {

// Creates every missing component of the directory path.
Status ensure_directory(const std::filesystem::path& path);

bool path_exists(const std::filesystem::path& path) noexcept;

// Returns the size of an existing regular file, or Outcome::NotFound.
Result<std::uint64_t> file_size(const std::filesystem::path& path);

// Reads a whole file. Refuses to allocate more than max_bytes: a file that
// claims to be larger is rejected before any buffer is created.
Result<std::vector<std::uint8_t>> read_file(const std::filesystem::path& path,
                                            std::uint64_t max_bytes);

// Reads at most max_bytes from the start of a file. Used to validate a header
// without materialising the whole artefact.
Result<std::vector<std::uint8_t>> read_prefix(const std::filesystem::path& path,
                                              std::uint64_t max_bytes);

// Writes to a sibling temporary file, forces it to the medium, then renames it
// over the target so that a crash can never leave a half-written target.
Status write_file_atomic(const std::filesystem::path& path, const std::vector<std::uint8_t>& data);

// Truncates a file to the given size. Used only to discard a recovered torn tail.
Status truncate_file(const std::filesystem::path& path, std::uint64_t size);

Status remove_file(const std::filesystem::path& path) noexcept;

}  // namespace lff::detail
