// Copyright 2022 PyMatching Contributors
// Modified for Beyond Blossom, 2026.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "beyond_blossom/profile/profile_io.h"

#include <stdexcept>

namespace beyond_blossom::profile {
namespace {

void set_permissions_or_throw(const std::filesystem::path &path, std::filesystem::perms perms) {
    std::error_code err;
    std::filesystem::permissions(path, perms, std::filesystem::perm_options::replace, err);
    if (err) {
        throw std::invalid_argument("Failed to set permissions on output path: " + path.string() + ": " + err.message());
    }
}

}  // namespace

std::string csv_quote(std::string_view text) {
    std::string result;
    result.reserve(text.size() + 2);
    result.push_back('"');
    for (char c : text) {
        if (c == '"') {
            result += "\"\"";
        } else {
            result.push_back(c);
        }
    }
    result.push_back('"');
    return result;
}

void write_json_string(std::ostream &out, std::string_view text) {
    out << '"';
    for (char c : text) {
        switch (c) {
            case '"':
                out << "\\\"";
                break;
            case '\\':
                out << "\\\\";
                break;
            case '\n':
                out << "\\n";
                break;
            case '\r':
                out << "\\r";
                break;
            case '\t':
                out << "\\t";
                break;
            default:
                out << c;
                break;
        }
    }
    out << '"';
}

void ensure_output_dir(const std::filesystem::path &dir) {
    std::error_code err;
    std::filesystem::create_directories(dir, err);
    if (err) {
        throw std::invalid_argument("Failed to create output directory: " + dir.string() + ": " + err.message());
    }
    if (!std::filesystem::is_directory(dir)) {
        throw std::invalid_argument("Output path is not a directory: " + dir.string());
    }
    set_permissions_or_throw(
        dir,
        std::filesystem::perms::owner_read | std::filesystem::perms::owner_write |
            std::filesystem::perms::owner_exec | std::filesystem::perms::group_read |
            std::filesystem::perms::group_exec | std::filesystem::perms::others_read |
            std::filesystem::perms::others_exec);
}

std::ofstream open_output_file(const std::filesystem::path &path) {
    std::ofstream out(path);
    if (!out) {
        throw std::invalid_argument("Failed to open output file: " + path.string());
    }
    set_permissions_or_throw(
        path,
        std::filesystem::perms::owner_read | std::filesystem::perms::owner_write |
            std::filesystem::perms::group_read | std::filesystem::perms::others_read);
    return out;
}

}  // namespace beyond_blossom::profile
