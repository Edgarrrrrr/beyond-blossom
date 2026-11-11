// Copyright 2022 PyMatching Contributors
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

#include "pymatching/sparse_blossom/driver/namespaced_main.h"

#include "gtest/gtest.h"

struct RaiiTempNamedFile {
    int descriptor;
    std::string path;
    RaiiTempNamedFile();
    ~RaiiTempNamedFile();
};

RaiiTempNamedFile::RaiiTempNamedFile() {
    char tmp_stdin_filename[] = "/tmp/stim_test_named_file_XXXXXX";
    descriptor = mkstemp(tmp_stdin_filename);
    if (descriptor == -1) {
        throw std::runtime_error("Failed to create temporary file.");
    }
    path = tmp_stdin_filename;
}

RaiiTempNamedFile::~RaiiTempNamedFile() {
    if (!path.empty()) {
        remove(path.data());
        path = "";
    }
}

std::string result_of_running_main(const std::vector<std::string> args, const std::string &input) {
    std::vector<const char *> argv;
    argv.push_back("TEST_PROCESS");
    for (const auto &a : args) {
        argv.push_back(a.c_str());
    }
    RaiiTempNamedFile inp;
    RaiiTempNamedFile out;
    argv.push_back("--in");
    argv.push_back(inp.path.c_str());
    argv.push_back("--out");
    argv.push_back(out.path.c_str());
    FILE *f = fopen(inp.path.c_str(), "w");
    if (f == nullptr || fwrite(input.data(), 1, input.size(), f) != input.size()) {
        throw std::invalid_argument("Failed to write input.");
    }
    fclose(f);
    if (pm::main((int)argv.size(), argv.data()) != EXIT_SUCCESS) {
        throw std::invalid_argument("Returned not EXIT_SUCCESS");
    }
    f = fopen(out.path.c_str(), "r");
    if (f == nullptr) {
        throw std::invalid_argument("Failed to read output.");
    }
    std::string s;
    while (true) {
        int i = getc(f);
        if (i == EOF) {
            break;
        }
        s.push_back((char)i);
    }
    return s;
}

TEST(Main, predict) {
    RaiiTempNamedFile dem;
    FILE *f = fopen(dem.path.c_str(), "w");
    fprintf(f, "%s", R"DEM(
        error(0.1) D0 L0
        error(0.1) D0 D1 L1
        error(0.1) D1 L2
    )DEM");
    fclose(f);
    auto stdout = result_of_running_main(
        {"predict", "--dem", dem.path, "--in_format", "dets", "--out_format", "dets"},
        R"stdin(shot
shot D0
shot D1
shot D0 D1)stdin");
    ASSERT_EQ(stdout, R"stdout(shot
shot L0
shot L2
shot L1
)stdout");
}

TEST(Main, count_mistakes) {
    RaiiTempNamedFile dem;
    FILE *f = fopen(dem.path.c_str(), "w");
    fprintf(f, "%s", R"DEM(
        error(0.1) D0 L0
        error(0.1) D0 D1 L1
        error(0.1) D1 L2
    )DEM");
    fclose(f);
    auto stdout_text = result_of_running_main(
        {
            "count_mistakes",
            "--dem",
            dem.path,
            "--in_format",
            "dets",
            "--in_includes_appended_observables",
        },
        R"stdin(shot L0
shot D0 L0
shot D1 L2
shot D0 D1 L1)stdin");
    ASSERT_EQ(stdout_text, "1 / 4\n");
}

TEST(Main, predict_default_upstream_formats_and_empty_single_odd_streams) {
    RaiiTempNamedFile dem;
    FILE* f = fopen(dem.path.c_str(), "w");
    fprintf(f, "error(0.1) D0 L0\n");
    fclose(f);
    // No strategy/grid options: preserve the original b8-input/01-output CLI.
    const std::vector<std::string> defaults{"predict", "--dem", dem.path};
    EXPECT_EQ(result_of_running_main(defaults, ""), "");
    EXPECT_EQ(result_of_running_main(defaults, std::string("\x01", 1)), "1\n");
    EXPECT_EQ(result_of_running_main(defaults, std::string("\x00\x01\x01", 3)), "0\n1\n1\n");
    EXPECT_EQ(result_of_running_main(
        {"predict", "--dem", dem.path, "--in_format", "01", "--out_format", "b8"}, "0\n1\n1\n"),
        std::string("\x00\x01\x01", 3));
    EXPECT_EQ(result_of_running_main(
        {"predict", "--dem", dem.path, "--in_format", "dets", "--out_format", "dets"}, "shot D0\n"),
        "shot L0\n");
}

TEST(Main, predict_default_upstream_more_than_64_observables) {
    RaiiTempNamedFile dem;
    FILE* f = fopen(dem.path.c_str(), "w");
    fprintf(f, "error(0.1) D0 L74\n");
    fclose(f);
    const std::string shots = "shot\nshot D0\nshot D0\n";
    EXPECT_EQ(result_of_running_main(
        {"predict", "--dem", dem.path, "--in_format", "dets", "--out_format", "dets"}, shots),
        "shot\nshot L74\nshot L74\n");
    const auto zero = std::string(75, '0') + '\n';
    const auto one = std::string(74, '0') + "1\n";
    EXPECT_EQ(result_of_running_main(
        {"predict", "--dem", dem.path, "--in_format", "dets"}, shots), zero + one + one);
}

TEST(Main, count_mistakes_default_upstream_formats_and_stream_lengths) {
    RaiiTempNamedFile dem;
    FILE* f = fopen(dem.path.c_str(), "w");
    fprintf(f, "error(0.1) D0 L0\n");
    fclose(f);
    const std::vector<std::string> defaults{
        "count_mistakes", "--dem", dem.path, "--in_includes_appended_observables"};
    EXPECT_EQ(result_of_running_main(defaults, ""), "0 / 0\n");
    EXPECT_EQ(result_of_running_main(defaults, "11\n"), "0 / 1\n");
    EXPECT_EQ(result_of_running_main(defaults, "00\n11\n10\n"), "1 / 3\n");
    EXPECT_EQ(result_of_running_main(
        {"count_mistakes", "--dem", dem.path, "--in_includes_appended_observables", "--in_format", "b8"},
        std::string("\x00\x03\x01", 3)), "1 / 3\n");
}

TEST(Main, upstream_correlation_option_changes_the_decoded_correction) {
    RaiiTempNamedFile dem;
    FILE* f = fopen(dem.path.c_str(), "w");
    // Matching D0-D1 makes D2-D3 cheaper on the second pass. Its observable
    // differs from the two individually cheaper boundary edges of pass one.
    fprintf(f, "%s", R"DEM(
        error(0.1) D0 D1 L0
        error(0.01) D2 D3 L1
        error(0.05) D0 D1 L0 ^ D2 D3 L1
        error(0.25) D2 L2
        error(0.25) D3
    )DEM");
    fclose(f);
    const std::string shots = "shot\nshot D0 D1 D2 D3\nshot D0 D1\n";
    std::vector<std::string> predict{
        "predict", "--dem", dem.path, "--in_format", "dets", "--out_format", "dets"};
    EXPECT_EQ(result_of_running_main(predict, shots), "shot\nshot L0 L2\nshot L0\n");
    predict.push_back("--enable_correlations");
    EXPECT_EQ(result_of_running_main(predict, shots), "shot\nshot L0 L1\nshot L0\n");
    const std::string labelled = "shot D0 D1 D2 D3 L0 L1\n";
    std::vector<std::string> count{
        "count_mistakes", "--dem", dem.path, "--in_format", "dets", "--in_includes_appended_observables"};
    EXPECT_EQ(result_of_running_main(count, labelled), "1 / 1\n");
    count.push_back("--enable_correlations");
    EXPECT_EQ(result_of_running_main(count, labelled), "0 / 1\n");
}

TEST(Main, decoder_options_reject_missing_or_invalid_geometry_and_cpu_maps) {
    RaiiTempNamedFile dem;
    FILE* f = fopen(dem.path.c_str(), "w");
    fprintf(f, "%s", R"DEM(
        detector(0, 0, 0) D0
        detector(0, 0, 0) D1
        detector(2, 0, 0) D2
        detector(2, 0, 0) D3
        error(0.1) D0 D2 L0
        error(0.1) D1 D3 L1
        error(0.05) D0 L0
        error(0.05) D1 L1
    )DEM");
    fclose(f);
    EXPECT_THROW(result_of_running_main(
        {"predict", "--dem", dem.path, "--decoder-strategy", "predecode"}, ""), std::invalid_argument);
    EXPECT_THROW(result_of_running_main(
        {"predict", "--dem", dem.path, "--grid-distance", "3"}, ""), std::invalid_argument);
    EXPECT_THROW(result_of_running_main(
        {"predict", "--dem", dem.path, "--decoder-strategy", "predecode", "--grid-distance", "3",
         "--grid-rounds", "3", "--grid-geometry", "triangle"}, ""), std::invalid_argument);
    EXPECT_THROW(result_of_running_main(
        {"predict", "--dem", dem.path, "--decoder-strategy", "predecode", "--grid-distance", "3",
         "--grid-rounds", "0", "--grid-geometry", "unrotated"}, ""), std::invalid_argument);
    const std::vector<std::string> component{
        "predict", "--dem", dem.path, "--decoder-strategy", "component", "--grid-distance", "3",
        "--grid-rounds", "3", "--grid-geometry", "unrotated", "--grid-simd", "scalar"};
    EXPECT_THROW(result_of_running_main(component, ""), std::invalid_argument);
    for (const auto& cpus : {"0", "0,0", "-1,2", "0,"}) {
        auto arguments = component;
        arguments.insert(arguments.end(), {"--decoder-cpus", cpus});
        EXPECT_THROW(result_of_running_main(arguments, ""), std::invalid_argument);
    }
}
