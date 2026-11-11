cmake_minimum_required(VERSION 3.18...3.25)
project(PyMatching2)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

include_directories(src)
set(CMAKE_CXX_STANDARD 20 CACHE STRING "C++ version selection")
set(CMAKE_CXX_STANDARD_REQUIRED ON)
option(PYMATCHING_ENABLE_NVTX "Enable NVTX ranges in sparse_blossom_profile" OFF)
option(PYMATCHING_PREDECODE_SIMD "Use AVX2 or ARM NEON one-hop dual limits when available" OFF)
option(PYMATCHING_PREDECODE_CAP_CACHE "Reuse saturated finite one-hop radius limits" OFF)
option(PYMATCHING_PREDECODE_CAP_3X128 "Use three independent 128-bit groups for AVX2 one-hop dual limits" OFF)
option(PYMATCHING_PREDECODE_NEIGHBORHOOD_SIMD "Use AVX2 packed one-hop active-neighbor checks" OFF)
option(PYMATCHING_PREDECODE_ENABLED "Run one-hop predecode before structured MWPM" ON)
option(PYMATCHING_STRUCTURED_PROFILE_COUNTERS "Collect structured logical operation counters" OFF)
option(PYMATCHING_STRUCTURED_PROFILE_TIMERS "Time structured per-shot phase boundaries" OFF)
option(PYMATCHING_STRUCTURED_PROFILE_TIMELINE "Record structured phase intervals" OFF)
option(PYMATCHING_SANITIZERS "Instrument the complete library and its callers with ASan/UBSan" OFF)
option(PYMATCHING_TEST_COVERAGE "Instrument the complete library and tests for coverage" OFF)
option(PYMATCHING_COMPONENT_OWNER_AUDIT "Audit borrowed seed storage and component owners" OFF)
option(PYMATCHING_BUILD_PROFILE_TOOLS "Build the separate instrumented profiling tools" ON)
option(PYMATCHING_BUILD_PYTHON "Build the Python extension when pybind11 is available" ON)
option(PYMATCHING_REQUIRE_OPENMP "Require OpenMP for the parallel decoder strategies" ON)
option(PYMATCHING_SHARED_IMPORT_PLAN "Enable the experimental shared import plan" OFF)
set(PYMATCHING_SHARED_COMPONENT_PARALLELISM "2" CACHE STRING "Number of independent X/Z component teams")
set(PYMATCHING_PREDECODE_STRATEGY "0" CACHE STRING "One-hop predecode strategy")
set(PYMATCHING_M4_SPATIAL_COMPONENT_THRESHOLD "64" CACHE STRING "Positive Apple ARM Spatial component fallback threshold")
if(NOT PYMATCHING_M4_SPATIAL_COMPONENT_THRESHOLD MATCHES "^[1-9][0-9]*$")
    message(FATAL_ERROR "PYMATCHING_M4_SPATIAL_COMPONENT_THRESHOLD must be a positive integer")
elseif(PYMATCHING_M4_SPATIAL_COMPONENT_THRESHOLD GREATER 4294967295)
    message(FATAL_ERROR "PYMATCHING_M4_SPATIAL_COMPONENT_THRESHOLD must fit uint32_t")
endif()
set(PYMATCHING_TAILRESET_PREDECODE_MODE "0" CACHE STRING "AE predecode ablation: 0 degree, 1 skip, 2 coordinate")
if(NOT PYMATCHING_TAILRESET_PREDECODE_MODE MATCHES "^[012]$")
    message(FATAL_ERROR "PYMATCHING_TAILRESET_PREDECODE_MODE must be 0, 1, or 2")
endif()
option(PYMATCHING_TAILRESET_SYNDROME_STATS "Collect AE TailReset residual syndromes; separate untimed build" OFF)
add_definitions(
    -DPM_M4_SPATIAL_COMPONENT_THRESHOLD=${PYMATCHING_M4_SPATIAL_COMPONENT_THRESHOLD}
    -DPM_TAILRESET_PREDECODE_MODE=${PYMATCHING_TAILRESET_PREDECODE_MODE}
    -DPM_TAILRESET_SYNDROME_STATS=$<BOOL:${PYMATCHING_TAILRESET_SYNDROME_STATS}>
    -DPM_PREDECODE_SIMD=$<BOOL:${PYMATCHING_PREDECODE_SIMD}>
    -DPM_PREDECODE_CAP_CACHE=$<BOOL:${PYMATCHING_PREDECODE_CAP_CACHE}>
    -DPM_PREDECODE_CAP_3X128=$<BOOL:${PYMATCHING_PREDECODE_CAP_3X128}>
    -DPM_PREDECODE_NEIGHBORHOOD_SIMD=$<BOOL:${PYMATCHING_PREDECODE_NEIGHBORHOOD_SIMD}>
    -DPM_PREDECODE_ENABLED=$<BOOL:${PYMATCHING_PREDECODE_ENABLED}>
    -DPM_STRUCTURED_PROFILE_COUNTERS=$<BOOL:${PYMATCHING_STRUCTURED_PROFILE_COUNTERS}>
    -DPM_STRUCTURED_PROFILE_TIMERS=$<BOOL:${PYMATCHING_STRUCTURED_PROFILE_TIMERS}>
    -DPM_STRUCTURED_PROFILE_TIMELINE=$<BOOL:${PYMATCHING_STRUCTURED_PROFILE_TIMELINE}>
    -DPM_COMPONENT_OWNER_AUDIT=$<BOOL:${PYMATCHING_COMPONENT_OWNER_AUDIT}>
    -DPM_SHARED_COMPONENT_PARALLELISM=${PYMATCHING_SHARED_COMPONENT_PARALLELISM}
    -DPM_SHARED_IMPORT_PLAN=$<BOOL:${PYMATCHING_SHARED_IMPORT_PLAN}>
    -DPM_PREDECODE_STRATEGY=${PYMATCHING_PREDECODE_STRATEGY})
option(PYMATCHING_BUILD_TESTS "Build C++ tests" ON)
set(PYMATCHING_STRUCTURED_STORAGE_DIRECTIONS "12" CACHE STRING "Structured edge storage slots (12 or 16)")
set_property(CACHE PYMATCHING_STRUCTURED_STORAGE_DIRECTIONS PROPERTY STRINGS 12 16)
if (NOT PYMATCHING_STRUCTURED_STORAGE_DIRECTIONS MATCHES "^(12|16)$")
    message(FATAL_ERROR "PYMATCHING_STRUCTURED_STORAGE_DIRECTIONS must be 12 or 16")
endif ()
add_definitions(-DPM_STRUCTURED_STORAGE_DIRECTIONS=${PYMATCHING_STRUCTURED_STORAGE_DIRECTIONS})
set(NVTX_INCLUDE_DIR "" CACHE PATH "Optional include directory containing nvtx3/nvToolsExt.h")
if(PYMATCHING_REQUIRE_OPENMP)
    find_package(OpenMP REQUIRED COMPONENTS CXX)
else()
    find_package(OpenMP QUIET COMPONENTS CXX)
endif()
function(enable_openmp_for_target target_name)
    if (OpenMP_CXX_FOUND)
        if (TARGET OpenMP::OpenMP_CXX)
            target_link_libraries(${target_name} PUBLIC OpenMP::OpenMP_CXX)
        else ()
            target_compile_options(${target_name} PRIVATE ${OpenMP_CXX_FLAGS})
            target_link_libraries(${target_name} PUBLIC ${OpenMP_CXX_FLAGS})
        endif ()
    endif ()
endfunction()
set(SIMD_WIDTH 128)
if (NOT(MSVC))
    if (CMAKE_SYSTEM_PROCESSOR MATCHES x86_64)
         set(ARCH_OPT "-O3" "-mno-avx2")
    else ()
         set(ARCH_OPT "-O3")
    endif ()
else ()
    set(ARCH_OPT "-O2")
    # https://stackoverflow.com/a/8591946 (RTC1 incompatible with O2 flag)
    STRING (REGEX REPLACE "/RTC(su|[1su])" "" CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS}")
endif ()

add_library(pymatching_build_options INTERFACE)
target_compile_options(pymatching_build_options INTERFACE ${ARCH_OPT})
if(NOT MSVC)
    target_link_options(pymatching_build_options INTERFACE -pthread)
endif()
if(PYMATCHING_SANITIZERS)
    if(MSVC)
        message(FATAL_ERROR "The ASan/UBSan configuration requires GCC or Clang")
    endif()
    target_compile_options(pymatching_build_options INTERFACE
        -fsanitize=address,undefined -fno-omit-frame-pointer -g)
    target_link_options(pymatching_build_options INTERFACE -fsanitize=address,undefined)
endif()
if(PYMATCHING_TEST_COVERAGE)
    if(MSVC)
        message(FATAL_ERROR "Coverage instrumentation requires GCC or Clang")
    endif()
    target_compile_options(pymatching_build_options INTERFACE --coverage)
    target_link_options(pymatching_build_options INTERFACE --coverage)
endif()

include(FetchContent)
if (PYMATCHING_BUILD_TESTS)
    FetchContent_Declare(
            googletest
            GIT_REPOSITORY https://github.com/google/googletest.git
            GIT_TAG 58d77fa8070e8cec2dc1ed015d66b454c8d78850
    )

    FetchContent_MakeAvailable(googletest)
    if (TARGET gtest_main AND NOT TARGET GTest::gtest_main)
        add_library(GTest::gtest_main ALIAS gtest_main)
    endif ()
    if (TARGET gmock_main AND NOT TARGET GTest::gmock_main)
        add_library(GTest::gmock_main ALIAS gmock_main)
    endif ()
    if(PYMATCHING_SANITIZERS)
        # libc++ inline container annotations must agree across all linked
        # translation units, including the GoogleTest support libraries.
        foreach(test_support IN ITEMS gtest gtest_main gmock gmock_main)
            target_link_libraries(${test_support} PRIVATE "$<BUILD_INTERFACE:pymatching_build_options>")
        endforeach()
    endif()
endif ()

FetchContent_Declare(stim
        GIT_REPOSITORY https://github.com/quantumlib/Stim.git 
        GIT_TAG 1320ad7eac7de34d2e9c70daa44fbc6d84174450)
FetchContent_MakeAvailable(stim)
target_link_libraries(libstim PRIVATE pymatching_build_options)
if (NOT (MSVC))
    target_compile_options(libstim PRIVATE -fno-strict-aliasing -fPIC ${ARCH_OPT})
else ()
    target_compile_options(libstim PRIVATE -fPIC ${ARCH_OPT})
endif ()

set(SOURCE_FILES_NO_MAIN
        src/pymatching/sparse_blossom/driver/namespaced_main.cc
        src/pymatching/sparse_blossom/driver/mwpm_decoding.cc
        src/pymatching/sparse_blossom/flooder/graph.cc
        src/pymatching/sparse_blossom/flooder/detector_node.cc
        src/pymatching/sparse_blossom/flooder_matcher_interop/compressed_edge.cc
        src/pymatching/sparse_blossom/flooder/graph_fill_region.cc
        src/pymatching/sparse_blossom/flooder/match.cc
        src/pymatching/sparse_blossom/flooder/graph_flooder.cc
        src/pymatching/sparse_blossom/matcher/alternating_tree.cc
        src/pymatching/sparse_blossom/matcher/mwpm.cc
        src/pymatching/sparse_blossom/flooder_matcher_interop/region_edge.cc
        src/pymatching/sparse_blossom/flooder_matcher_interop/mwpm_event.cc
        src/pymatching/sparse_blossom/tracker/flood_check_event.cc
        src/pymatching/sparse_blossom/diagram/animation_main.cc
        src/pymatching/sparse_blossom/diagram/mwpm_diagram.cc
        src/pymatching/sparse_blossom/search/search_graph.cc
        src/pymatching/sparse_blossom/search/search_detector_node.cc
        src/pymatching/sparse_blossom/search/search_flooder.cc
        src/pymatching/sparse_blossom/driver/user_graph.cc
        src/pymatching/rand/rand_gen.cc
        )

# Independent structured backend; original generic translation units stay unchanged.
list(APPEND SOURCE_FILES_NO_MAIN
        src/pymatching/sparse_blossom/driver/backend_user_graph.cc
        src/pymatching/sparse_blossom/driver/decoder_strategy.cc
        src/pymatching/sparse_blossom/structured/diagram/mwpm_diagram.cc
        src/pymatching/sparse_blossom/structured/driver/component_decoder.cc
        src/pymatching/sparse_blossom/structured/driver/shared_component_decoder.cc
        src/pymatching/sparse_blossom/structured/driver/mwpm_decoding.cc
        src/pymatching/sparse_blossom/structured/predecode/one_hop_predecoder.cc
        src/pymatching/sparse_blossom/structured/flooder/detector_node.cc
        src/pymatching/sparse_blossom/structured/flooder/graph.cc
        src/pymatching/sparse_blossom/structured/flooder/graph_fill_region.cc
        src/pymatching/sparse_blossom/structured/flooder/graph_flooder.cc
        src/pymatching/sparse_blossom/structured/flooder/match.cc
        src/pymatching/sparse_blossom/structured/flooder_matcher_interop/compressed_edge.cc
        src/pymatching/sparse_blossom/structured/flooder_matcher_interop/mwpm_event.cc
        src/pymatching/sparse_blossom/structured/flooder_matcher_interop/region_edge.cc
        src/pymatching/sparse_blossom/structured/grid/static_grid.cc
        src/pymatching/sparse_blossom/structured/matcher/alternating_tree.cc
        src/pymatching/sparse_blossom/structured/matcher/mwpm.cc
        src/pymatching/sparse_blossom/structured/search/search_detector_node.cc
        src/pymatching/sparse_blossom/structured/search/search_flooder.cc
        src/pymatching/sparse_blossom/structured/search/search_graph.cc
        src/pymatching/sparse_blossom/structured/tracker/flood_check_event.cc
)

set(TEST_FILES
        src/pymatching/sparse_blossom/driver/decoder_strategy.test.cc
        src/pymatching/sparse_blossom/structured/driver/predecode_binding.test.cc
        src/pymatching/sparse_blossom/structured/driver/shared_component_decoder.test.cc
        src/pymatching/sparse_blossom/structured/driver/serial_seed_storage.test.cc
        src/pymatching/sparse_blossom/structured/flooder/region_pool.test.cc
        src/pymatching/sparse_blossom/structured/matcher/serial_import.test.cc
        src/pymatching/sparse_blossom/structured/matcher/active_trees.test.cc
        src/pymatching/sparse_blossom/grid/static_grid.test.cc
        src/pymatching/sparse_blossom/structured/predecode/one_hop_predecoder.test.cc
        src/pymatching/sparse_blossom/structured/predecode/neighborhood_kernels.test.cc
        src/pymatching/sparse_blossom/structured/predecode/structured_profile.test.cc
        src/pymatching/sparse_blossom/structured/predecode/one_hop_cap.test.cc
        src/pymatching/sparse_blossom/structured/predecode/one_hop_sparse_cap.test.cc
        src/pymatching/sparse_blossom/driver/namespaced_main.test.cc
        src/pymatching/sparse_blossom/driver/mwpm_decoding.test.cc
        src/pymatching/sparse_blossom/flooder_matcher_interop/varying.test.cc
        src/pymatching/sparse_blossom/flooder/graph.test.cc
        src/pymatching/sparse_blossom/flooder/detector_node.test.cc
        src/pymatching/sparse_blossom/flooder_matcher_interop/compressed_edge.test.cc
        src/pymatching/sparse_blossom/flooder/graph_fill_region.test.cc
        src/pymatching/sparse_blossom/flooder/match.test.cc
        src/pymatching/sparse_blossom/flooder/graph_flooder.test.cc
        src/pymatching/sparse_blossom/matcher/alternating_tree.test.cc
        src/pymatching/sparse_blossom/matcher/mwpm.test.cc
        src/pymatching/sparse_blossom/tracker/flood_check_event.test.cc
        src/pymatching/sparse_blossom/tracker/radix_heap_queue.test.cc
        src/pymatching/sparse_blossom/flooder_matcher_interop/mwpm_event.test.cc
        src/pymatching/sparse_blossom/tracker/queued_event_tracker.test.cc
        src/pymatching/sparse_blossom/tracker/cyclic.test.cc
        src/pymatching/sparse_blossom/diagram/mwpm_diagram.test.cc
        src/pymatching/sparse_blossom/search/search_graph.test.cc
        src/pymatching/sparse_blossom/search/search_flooder.test.cc
        src/pymatching/sparse_blossom/driver/user_graph.test.cc
        )

set(PERF_FILES
        src/pymatching/perf/main.perf.cc
        src/pymatching/perf/util.perf.cc
        src/pymatching/sparse_blossom/driver/mwpm_decoding.perf.cc
        src/pymatching/sparse_blossom/driver/user_graph.perf.cc
        src/pymatching/sparse_blossom/flooder_matcher_interop/varying.perf.cc
        src/pymatching/sparse_blossom/tracker/radix_heap_queue.perf.cc
        )

set(PROFILE_CORE_FILES
        src/pymatching/sparse_blossom/profile/profile_io.cc
        src/pymatching/sparse_blossom/profile/profile_schema.cc
        src/pymatching/sparse_blossom/profile/profile_stats.cc
        )

set(PROFILE_RUNNER_FILES
        src/pymatching/sparse_blossom/profile/profile_runner.cc
        )

set(PYTHON_API_FILES
        src/pymatching/sparse_blossom/driver/user_graph.pybind.cc
        src/pymatching/rand/rand_gen.pybind.cc
        src/pymatching/pymatching.pybind.cc
        )

# Every normal caller links this library. Strategy selection binds one of the
# shared implementations in this library; no alternate source trees are built.
add_library(libpymatching ${SOURCE_FILES_NO_MAIN})
set_target_properties(libpymatching PROPERTIES PREFIX "" POSITION_INDEPENDENT_CODE ON)
target_include_directories(libpymatching PUBLIC src)
target_compile_definitions(libpymatching PUBLIC PYMATCHING_ENABLE_PROFILE=0)
target_link_libraries(libpymatching PUBLIC libstim pymatching_build_options)
enable_openmp_for_target(libpymatching)
install(TARGETS libpymatching LIBRARY DESTINATION)
install(DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/src/" DESTINATION "include"
    FILES_MATCHING PATTERN "*.h" PATTERN "*.inl")

add_executable(pymatching src/main.cc)
target_link_libraries(pymatching libpymatching)
install(TARGETS pymatching RUNTIME DESTINATION bin)

if(PYMATCHING_BUILD_TESTS)
    enable_testing()
    set(PROFILE_TEST_FILES
        src/pymatching/sparse_blossom/profile/profile_schema.test.cc
        src/pymatching/sparse_blossom/profile/profile_stats.test.cc)
    add_executable(pymatching_tests ${TEST_FILES})
    target_link_libraries(pymatching_tests libpymatching GTest::gtest_main GTest::gmock_main)
    # These helpers require active profile sessions, while normal core tests
    # exercise the exact uninstrumented library used by CLI/Python callers.
    # No decoder implementation is linked into the helper test executable.
    add_executable(pymatching_profile_tests ${PROFILE_CORE_FILES} ${PROFILE_TEST_FILES})
    target_include_directories(pymatching_profile_tests PRIVATE src)
    target_compile_definitions(pymatching_profile_tests PRIVATE PYMATCHING_ENABLE_PROFILE=1)
    target_link_libraries(pymatching_profile_tests PRIVATE libstim pymatching_build_options
        GTest::gtest_main GTest::gmock_main)
    add_executable(decoder_strategy_differential tests/decoder_strategy_differential.cc)
    target_link_libraries(decoder_strategy_differential libpymatching)
    include(GoogleTest)
    gtest_discover_tests(pymatching_tests DISCOVERY_TIMEOUT 60
        WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}")
    gtest_discover_tests(pymatching_profile_tests DISCOVERY_TIMEOUT 60 TEST_PREFIX "profile."
        WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}")
endif()

add_executable(pymatching_perf ${PERF_FILES})
target_link_libraries(pymatching_perf libpymatching)

if(PYMATCHING_BUILD_PROFILE_TOOLS)
    # Diagnostic instrumentation changes inline code and decoder operations.
    # Keep that configuration separate while using the same core source list.
    add_library(pymatching_profile_core STATIC EXCLUDE_FROM_ALL ${SOURCE_FILES_NO_MAIN})
    set_target_properties(pymatching_profile_core PROPERTIES POSITION_INDEPENDENT_CODE ON)
    target_include_directories(pymatching_profile_core PUBLIC src)
    target_compile_definitions(pymatching_profile_core PUBLIC PYMATCHING_ENABLE_PROFILE=1)
    target_link_libraries(pymatching_profile_core PUBLIC libstim pymatching_build_options)
    enable_openmp_for_target(pymatching_profile_core)
    if(PYMATCHING_ENABLE_NVTX)
        target_compile_definitions(pymatching_profile_core PUBLIC PYMATCHING_ENABLE_NVTX=1)
        if(NVTX_INCLUDE_DIR)
            target_include_directories(pymatching_profile_core PUBLIC "${NVTX_INCLUDE_DIR}")
        endif()
    endif()
    add_executable(sparse_blossom_profile ${PROFILE_CORE_FILES} ${PROFILE_RUNNER_FILES}
        src/pymatching/sparse_blossom/profile/profile_main.cc)
    target_link_libraries(sparse_blossom_profile pymatching_profile_core)
    add_executable(sparse_blossom_profile_baseline ${PROFILE_CORE_FILES} ${PROFILE_RUNNER_FILES}
        src/pymatching/sparse_blossom/profile/profile_main.cc)
    target_link_libraries(sparse_blossom_profile_baseline libpymatching)
endif()

set(PYMATCHING_PYBIND11_SOURCE_DIR "${CMAKE_CURRENT_LIST_DIR}/pybind11" CACHE PATH "pybind11 source directory")
if(PYMATCHING_BUILD_PYTHON)
    if(EXISTS "${PYMATCHING_PYBIND11_SOURCE_DIR}/CMakeLists.txt")
        add_subdirectory("${PYMATCHING_PYBIND11_SOURCE_DIR}" "${CMAKE_CURRENT_BINARY_DIR}/pybind11")
    else()
        # Also accept an installed pybind11 package; no download is performed.
        find_package(pybind11 CONFIG QUIET)
    endif()
    if(COMMAND pybind11_add_module)
        pybind11_add_module(_cpp_pymatching ${PYTHON_API_FILES})
        target_link_libraries(_cpp_pymatching PRIVATE libpymatching)
    else()
        message(WARNING "Skipped _cpp_pymatching: provide PYMATCHING_PYBIND11_SOURCE_DIR, pybind11_DIR, or initialize the pybind11 submodule")
    endif()
endif()
