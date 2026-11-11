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

#ifndef BB_SPARSE_BLOSSOM_PROFILE_PROFILE_IO_H
#define BB_SPARSE_BLOSSOM_PROFILE_PROFILE_IO_H

#include <filesystem>
#include <fstream>
#include <ostream>
#include <string>
#include <string_view>

namespace beyond_blossom::profile {

std::string csv_quote(std::string_view text);
void write_json_string(std::ostream &out, std::string_view text);
void ensure_output_dir(const std::filesystem::path &dir);
std::ofstream open_output_file(const std::filesystem::path &path);

}  // namespace beyond_blossom::profile

#endif  // BB_SPARSE_BLOSSOM_PROFILE_PROFILE_IO_H
