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

#include "pybind11/pybind11.h"
#include "beyond_blossom/bindings/structured.pybind.h"
#include "beyond_blossom/rand/rand_gen.pybind.h"
#include "beyond_blossom/driver/namespaced_main.h"
#include "beyond_blossom/driver/user_graph.pybind.h"

namespace py = pybind11;
using namespace py::literals;

int beyond_blossom_main(const std::vector<std::string> &args) {
    std::vector<const char *> argv;
    argv.push_back("beyond_blossom.main");
    for (const auto &arg : args) {
        argv.push_back(arg.data());
    }
    return beyond_blossom::main(argv.size(), argv.data());
}

PYBIND11_MODULE(_cpp_beyond_blossom, m) {
    beyond_blossom::bindings::bind_structured_decoder(m);
    auto matching_graph = beyond_blossom::bindings::pybind_user_graph(m);
    beyond_blossom::bindings::pybind_user_graph_methods(m, matching_graph);
    beyond_blossom::bindings::pybind_rand_gen_methods(m);
    m.def("main", &beyond_blossom_main, pybind11::kw_only(), pybind11::arg("command_line_args"), R"pbdoc(
Runs the command line tool version of beyond_blossom with the given arguments.
)pbdoc");
}