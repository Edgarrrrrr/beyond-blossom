cmake_minimum_required(VERSION 3.18...3.25)
project(BeyondBlossom VERSION 0.1.0 LANGUAGES CXX)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

include_directories(src)
set(CMAKE_CXX_STANDARD 20 CACHE STRING "C++ version selection")
set(CMAKE_CXX_STANDARD_REQUIRED ON)
option(BB_ENABLE_NVTX "Enable NVTX ranges in sparse_blossom_profile" OFF)
option(BB_PREDECODE_SIMD "Use AVX2 or ARM NEON one-hop dual limits when available" ON)
option(BB_PREDECODE_CAP_CACHE "Reuse saturated finite one-hop radius limits" OFF)
option(BB_PREDECODE_CAP_3X128 "Use three independent 128-bit groups for AVX2 one-hop dual limits" OFF)
option(BB_PREDECODE_NEIGHBORHOOD_SIMD "Use AVX2 packed one-hop active-neighbor checks" OFF)
option(BB_PREDECODE_ENABLED "Run one-hop predecode before structured MWPM" ON)
option(BB_STRUCTURED_PROFILE_COUNTERS "Collect structured logical operation counters" OFF)
option(BB_STRUCTURED_PROFILE_TIMERS "Time structured per-shot phase boundaries" OFF)
option(BB_STRUCTURED_PROFILE_TIMELINE "Record structured phase intervals" OFF)
option(BB_SANITIZERS "Instrument the complete library and its callers with ASan/UBSan" OFF)
option(BB_TEST_COVERAGE "Instrument the complete library and tests for coverage" OFF)
option(BB_COMPONENT_OWNER_AUDIT "Audit borrowed seed storage and component owners" OFF)
option(BB_BUILD_PROFILE_TOOLS "Build the separate instrumented profiling tools" OFF)
option(BB_BUILD_PYTHON "Build the Python extension when pybind11 is available" ON)
option(BB_REQUIRE_OPENMP "Require OpenMP for the parallel decoder strategies" ON)
option(BB_SHARED_IMPORT_PLAN "Enable the experimental shared import plan" OFF)
set(BB_SHARED_COMPONENT_PARALLELISM "2" CACHE STRING "Number of independent X/Z component teams")
set(BB_PREDECODE_STRATEGY "0" CACHE STRING "One-hop predecode strategy")
set(BB_M4_SPATIAL_COMPONENT_THRESHOLD "64" CACHE STRING "Positive Apple ARM Spatial component fallback threshold")
if(NOT BB_M4_SPATIAL_COMPONENT_THRESHOLD MATCHES "^[1-9][0-9]*$")
    message(FATAL_ERROR "BB_M4_SPATIAL_COMPONENT_THRESHOLD must be a positive integer")
elseif(BB_M4_SPATIAL_COMPONENT_THRESHOLD GREATER 4294967295)
    message(FATAL_ERROR "BB_M4_SPATIAL_COMPONENT_THRESHOLD must fit uint32_t")
endif()
set(BB_TAILRESET_PREDECODE_MODE "0" CACHE STRING "AE predecode ablation: 0 degree, 1 skip, 2 coordinate")
if(NOT BB_TAILRESET_PREDECODE_MODE MATCHES "^[012]$")
    message(FATAL_ERROR "BB_TAILRESET_PREDECODE_MODE must be 0, 1, or 2")
endif()
option(BB_TAILRESET_SYNDROME_STATS "Collect AE TailReset residual syndromes; separate untimed build" OFF)
list(APPEND BB_COMPILE_DEFINITIONS

    BB_M4_SPATIAL_COMPONENT_THRESHOLD=${BB_M4_SPATIAL_COMPONENT_THRESHOLD}
    BB_TAILRESET_PREDECODE_MODE=${BB_TAILRESET_PREDECODE_MODE}
    BB_TAILRESET_SYNDROME_STATS=$<BOOL:${BB_TAILRESET_SYNDROME_STATS}>
    BB_PREDECODE_SIMD=$<BOOL:${BB_PREDECODE_SIMD}>
    BB_PREDECODE_CAP_CACHE=$<BOOL:${BB_PREDECODE_CAP_CACHE}>
    BB_PREDECODE_CAP_3X128=$<BOOL:${BB_PREDECODE_CAP_3X128}>
    BB_PREDECODE_NEIGHBORHOOD_SIMD=$<BOOL:${BB_PREDECODE_NEIGHBORHOOD_SIMD}>
    BB_PREDECODE_ENABLED=$<BOOL:${BB_PREDECODE_ENABLED}>
    BB_STRUCTURED_PROFILE_COUNTERS=$<BOOL:${BB_STRUCTURED_PROFILE_COUNTERS}>
    BB_STRUCTURED_PROFILE_TIMERS=$<BOOL:${BB_STRUCTURED_PROFILE_TIMERS}>
    BB_STRUCTURED_PROFILE_TIMELINE=$<BOOL:${BB_STRUCTURED_PROFILE_TIMELINE}>
    BB_COMPONENT_OWNER_AUDIT=$<BOOL:${BB_COMPONENT_OWNER_AUDIT}>
    BB_SHARED_COMPONENT_PARALLELISM=${BB_SHARED_COMPONENT_PARALLELISM}
    BB_SHARED_IMPORT_PLAN=$<BOOL:${BB_SHARED_IMPORT_PLAN}>
    BB_PREDECODE_STRATEGY=${BB_PREDECODE_STRATEGY})
option(BB_BUILD_TESTS "Build C++ tests" ON)
set(BB_STRUCTURED_STORAGE_DIRECTIONS "12" CACHE STRING "Structured edge storage slots (12 or 16)")
set_property(CACHE BB_STRUCTURED_STORAGE_DIRECTIONS PROPERTY STRINGS 12 16)
if (NOT BB_STRUCTURED_STORAGE_DIRECTIONS MATCHES "^(12|16)$")
    message(FATAL_ERROR "BB_STRUCTURED_STORAGE_DIRECTIONS must be 12 or 16")
endif ()
list(APPEND BB_COMPILE_DEFINITIONS
BB_STRUCTURED_STORAGE_DIRECTIONS=${BB_STRUCTURED_STORAGE_DIRECTIONS})
set(NVTX_INCLUDE_DIR "" CACHE PATH "Optional include directory containing nvtx3/nvToolsExt.h")
if(BB_REQUIRE_OPENMP)
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
         # Intrinsic wrappers inherit the translation unit's target features.
         # Pin the baseline so a toolchain default such as POPCNT cannot leak
         # into callbacks that deliberately disable that independent feature.
         set(ARCH_OPT "-O3" "-march=x86-64" "-mno-avx2")
    else ()
         set(ARCH_OPT "-O3")
    endif ()
else ()
    set(ARCH_OPT "-O2")
    # https://stackoverflow.com/a/8591946 (RTC1 incompatible with O2 flag)
    STRING (REGEX REPLACE "/RTC(su|[1su])" "" CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS}")
endif ()

add_library(beyond_blossom_build_options INTERFACE)
target_compile_definitions(beyond_blossom_build_options INTERFACE ${BB_COMPILE_DEFINITIONS})
target_compile_options(beyond_blossom_build_options INTERFACE ${ARCH_OPT})
if(NOT MSVC)
    target_link_options(beyond_blossom_build_options INTERFACE -pthread)
endif()
if(BB_SANITIZERS)
    if(MSVC)
        message(FATAL_ERROR "The ASan/UBSan configuration requires GCC or Clang")
    endif()
    target_compile_options(beyond_blossom_build_options INTERFACE
        -O1 -fsanitize=address,undefined -fno-omit-frame-pointer -g)
    target_link_options(beyond_blossom_build_options INTERFACE -fsanitize=address,undefined)
endif()
if(BB_TEST_COVERAGE)
    if(MSVC)
        message(FATAL_ERROR "Coverage instrumentation requires GCC or Clang")
    endif()
    target_compile_options(beyond_blossom_build_options INTERFACE --coverage)
    target_link_options(beyond_blossom_build_options INTERFACE --coverage)
endif()

include(FetchContent)
if (BB_BUILD_TESTS)
    set(INSTALL_GTEST OFF CACHE BOOL "Do not install private test dependencies")
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
    if(BB_SANITIZERS)
        # libc++ inline container annotations must agree across all linked
        # translation units, including the GoogleTest support libraries.
        foreach(test_support IN ITEMS gtest gtest_main gmock gmock_main)
            target_link_libraries(${test_support} PRIVATE "$<BUILD_INTERFACE:beyond_blossom_build_options>")
        endforeach()
    endif()
endif ()

FetchContent_Declare(stim
        GIT_REPOSITORY https://github.com/quantumlib/Stim.git 
        GIT_TAG 1320ad7eac7de34d2e9c70daa44fbc6d84174450)
FetchContent_GetProperties(stim)
if(NOT stim_POPULATED)
    FetchContent_Populate(stim)
    # Only the dependency library belongs to the normal build/install graph.
    add_subdirectory("${stim_SOURCE_DIR}" "${stim_BINARY_DIR}" EXCLUDE_FROM_ALL)
endif()
target_link_libraries(libstim PRIVATE beyond_blossom_build_options)
if (NOT (MSVC))
    target_compile_options(libstim PRIVATE -fno-strict-aliasing -fPIC ${ARCH_OPT})
else ()
    target_compile_options(libstim PRIVATE -fPIC ${ARCH_OPT})
endif ()

set(SOURCE_FILES_NO_MAIN
        src/beyond_blossom/structured/decoder.cc
        src/beyond_blossom/unstructured/predecode/one_hop_predecoder.cc
        src/beyond_blossom/driver/namespaced_main.cc
        src/beyond_blossom/driver/mwpm_decoding.cc
        src/beyond_blossom/unstructured/flooder/graph.cc
        src/beyond_blossom/unstructured/flooder/detector_node.cc
        src/beyond_blossom/unstructured/interop/compressed_edge.cc
        src/beyond_blossom/unstructured/flooder/graph_fill_region.cc
        src/beyond_blossom/unstructured/flooder/match.cc
        src/beyond_blossom/unstructured/flooder/graph_flooder.cc
        src/beyond_blossom/unstructured/matcher/alternating_tree.cc
        src/beyond_blossom/unstructured/matcher/mwpm.cc
        src/beyond_blossom/unstructured/interop/region_edge.cc
        src/beyond_blossom/unstructured/interop/mwpm_event.cc
        src/beyond_blossom/unstructured/tracker/flood_check_event.cc
        src/beyond_blossom/unstructured/diagram/animation_main.cc
        src/beyond_blossom/unstructured/diagram/mwpm_diagram.cc
        src/beyond_blossom/unstructured/search/search_graph.cc
        src/beyond_blossom/unstructured/search/search_detector_node.cc
        src/beyond_blossom/unstructured/search/search_flooder.cc
        src/beyond_blossom/driver/user_graph.cc
        src/beyond_blossom/rand/rand_gen.cc
        )

# Independent structured backend; backend storage and hot paths remain separate.
list(APPEND SOURCE_FILES_NO_MAIN
        src/beyond_blossom/driver/backend_user_graph.cc
        src/beyond_blossom/driver/decoder_strategy.cc
        src/beyond_blossom/structured/diagram/mwpm_diagram.cc
        src/beyond_blossom/structured/driver/component_decoder.cc
        src/beyond_blossom/structured/driver/shared_component_decoder.cc
        src/beyond_blossom/structured/driver/mwpm_decoding.cc
        src/beyond_blossom/structured/predecode/one_hop_predecoder.cc
        src/beyond_blossom/structured/flooder/detector_node.cc
        src/beyond_blossom/structured/flooder/graph.cc
        src/beyond_blossom/structured/flooder/graph_fill_region.cc
        src/beyond_blossom/structured/flooder/graph_flooder.cc
        src/beyond_blossom/structured/flooder/match.cc
        src/beyond_blossom/structured/flooder_matcher_interop/compressed_edge.cc
        src/beyond_blossom/structured/flooder_matcher_interop/mwpm_event.cc
        src/beyond_blossom/structured/flooder_matcher_interop/region_edge.cc
        src/beyond_blossom/structured/grid/static_grid.cc
        src/beyond_blossom/structured/matcher/alternating_tree.cc
        src/beyond_blossom/structured/matcher/mwpm.cc
        src/beyond_blossom/structured/search/search_detector_node.cc
        src/beyond_blossom/structured/search/search_flooder.cc
        src/beyond_blossom/structured/search/search_graph.cc
        src/beyond_blossom/structured/tracker/flood_check_event.cc
)

set(TEST_FILES
        src/beyond_blossom/core/matching_result.test.cc
        src/beyond_blossom/core/header_odr_a.test.cc
        src/beyond_blossom/core/header_odr_b.test.cc
        src/beyond_blossom/unstructured/predecode/one_hop_predecoder.test.cc
        src/beyond_blossom/driver/decoder_strategy.test.cc
        src/beyond_blossom/structured/driver/predecode_binding.test.cc
        src/beyond_blossom/structured/driver/shared_component_decoder.test.cc
        src/beyond_blossom/structured/driver/serial_seed_storage.test.cc
        src/beyond_blossom/structured/flooder/region_pool.test.cc
        src/beyond_blossom/structured/matcher/serial_import.test.cc
        src/beyond_blossom/structured/matcher/active_trees.test.cc
        src/beyond_blossom/structured/grid/static_grid.test.cc
        src/beyond_blossom/structured/predecode/one_hop_predecoder.test.cc
        src/beyond_blossom/structured/predecode/neighborhood_kernels.test.cc
        src/beyond_blossom/structured/predecode/structured_profile.test.cc
        src/beyond_blossom/structured/predecode/one_hop_cap.test.cc
        src/beyond_blossom/structured/predecode/one_hop_sparse_cap.test.cc
        src/beyond_blossom/driver/namespaced_main.test.cc
        src/beyond_blossom/driver/mwpm_decoding.test.cc
        src/beyond_blossom/core/interop/varying.test.cc
        src/beyond_blossom/unstructured/flooder/graph.test.cc
        src/beyond_blossom/unstructured/flooder/detector_node.test.cc
        src/beyond_blossom/unstructured/interop/compressed_edge.test.cc
        src/beyond_blossom/unstructured/flooder/graph_fill_region.test.cc
        src/beyond_blossom/unstructured/flooder/match.test.cc
        src/beyond_blossom/unstructured/flooder/graph_flooder.test.cc
        src/beyond_blossom/unstructured/matcher/alternating_tree.test.cc
        src/beyond_blossom/unstructured/matcher/mwpm.test.cc
        src/beyond_blossom/unstructured/tracker/flood_check_event.test.cc
        src/beyond_blossom/unstructured/tracker/radix_heap_queue.test.cc
        src/beyond_blossom/unstructured/interop/mwpm_event.test.cc
        src/beyond_blossom/unstructured/tracker/queued_event_tracker.test.cc
        src/beyond_blossom/core/tracker/cyclic.test.cc
        src/beyond_blossom/unstructured/diagram/mwpm_diagram.test.cc
        src/beyond_blossom/unstructured/search/search_graph.test.cc
        src/beyond_blossom/unstructured/search/search_flooder.test.cc
        src/beyond_blossom/driver/user_graph.test.cc
        )

set(PERF_FILES
        src/beyond_blossom/perf/main.perf.cc
        src/beyond_blossom/perf/util.perf.cc
        src/beyond_blossom/driver/mwpm_decoding.perf.cc
        src/beyond_blossom/driver/user_graph.perf.cc
        src/beyond_blossom/core/interop/varying.perf.cc
        src/beyond_blossom/unstructured/tracker/radix_heap_queue.perf.cc
        )

set(PROFILE_CORE_FILES
        src/beyond_blossom/profile/profile_io.cc
        src/beyond_blossom/profile/profile_schema.cc
        src/beyond_blossom/profile/profile_stats.cc
        )

set(PROFILE_RUNNER_FILES
        src/beyond_blossom/profile/profile_runner.cc
        )

set(PYTHON_API_FILES
        src/beyond_blossom/bindings/structured.pybind.cc
        src/beyond_blossom/driver/user_graph.pybind.cc
        src/beyond_blossom/rand/rand_gen.pybind.cc
        src/beyond_blossom/bindings/module.cc
        )

# Every normal caller links this library. Strategy selection binds one of the
# shared implementations in this library; no alternate source trees are built.
add_library(libbeyond_blossom ${SOURCE_FILES_NO_MAIN})
target_compile_features(libbeyond_blossom PUBLIC cxx_std_20)
set_target_properties(libbeyond_blossom PROPERTIES PREFIX "" POSITION_INDEPENDENT_CODE ON)
target_include_directories(libbeyond_blossom PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
add_library(BeyondBlossom::decoder ALIAS libbeyond_blossom)
set_target_properties(libbeyond_blossom PROPERTIES EXPORT_NAME decoder)
target_compile_definitions(libbeyond_blossom PUBLIC BB_ENABLE_PROFILE=0)
target_link_libraries(libbeyond_blossom PUBLIC libstim beyond_blossom_build_options)
enable_openmp_for_target(libbeyond_blossom)
# Export the exact build definitions with the native library. Inline headers
# must use the same layout and instrumentation settings as the compiled core.
set_target_properties(libstim PROPERTIES EXPORT_NAME stim
    INTERFACE_INCLUDE_DIRECTORIES "$<BUILD_INTERFACE:${stim_SOURCE_DIR}/src>;$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>")
set_target_properties(beyond_blossom_build_options PROPERTIES EXPORT_NAME build_options)
install(TARGETS libbeyond_blossom libstim beyond_blossom_build_options
    EXPORT BeyondBlossomTargets
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(FILES LICENSE NOTICE DESTINATION ${CMAKE_INSTALL_DATADIR}/licenses/BeyondBlossom)
install(FILES "${stim_SOURCE_DIR}/LICENSE"
    DESTINATION ${CMAKE_INSTALL_DATADIR}/licenses/BeyondBlossom RENAME Stim.txt)
install(DIRECTORY "${stim_SOURCE_DIR}/src/" DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}"
    FILES_MATCHING PATTERN "*.h" PATTERN "*.inl" PATTERN "*.test.h" EXCLUDE)
install(EXPORT BeyondBlossomTargets NAMESPACE BeyondBlossom::
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/BeyondBlossom)
configure_package_config_file(cmake/BeyondBlossomConfig.cmake.in
    "${CMAKE_CURRENT_BINARY_DIR}/BeyondBlossomConfig.cmake"
    INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/BeyondBlossom)
write_basic_package_version_file("${CMAKE_CURRENT_BINARY_DIR}/BeyondBlossomConfigVersion.cmake"
    VERSION ${PROJECT_VERSION} COMPATIBILITY SameMajorVersion)
install(FILES "${CMAKE_CURRENT_BINARY_DIR}/BeyondBlossomConfig.cmake"
    "${CMAKE_CURRENT_BINARY_DIR}/BeyondBlossomConfigVersion.cmake"
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/BeyondBlossom)
install(DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/src/" DESTINATION "include"
    FILES_MATCHING PATTERN "*.h" PATTERN "*.inl" PATTERN "*.test.h" EXCLUDE)

add_executable(beyond_blossom src/main.cc)
target_link_libraries(beyond_blossom libbeyond_blossom)
install(TARGETS beyond_blossom RUNTIME DESTINATION bin)

if(BB_BUILD_TESTS)
    enable_testing()
    set(PROFILE_TEST_FILES
        src/beyond_blossom/profile/profile_schema.test.cc
        src/beyond_blossom/profile/profile_stats.test.cc)
    add_executable(beyond_blossom_tests ${TEST_FILES})
    target_link_libraries(beyond_blossom_tests libbeyond_blossom GTest::gtest_main GTest::gmock_main)
    # These helpers require active profile sessions, while normal core tests
    # exercise the exact uninstrumented library used by CLI/Python callers.
    # No decoder implementation is linked into the helper test executable.
    add_executable(beyond_blossom_profile_tests ${PROFILE_CORE_FILES} ${PROFILE_TEST_FILES})
    target_include_directories(beyond_blossom_profile_tests PRIVATE src)
    target_compile_definitions(beyond_blossom_profile_tests PRIVATE BB_ENABLE_PROFILE=1)
    target_link_libraries(beyond_blossom_profile_tests PRIVATE libstim beyond_blossom_build_options
        GTest::gtest_main GTest::gmock_main)
    add_executable(decoder_strategy_differential tests/decoder_strategy_differential.cc)
    target_link_libraries(decoder_strategy_differential libbeyond_blossom)
    include(GoogleTest)
    gtest_discover_tests(beyond_blossom_tests DISCOVERY_TIMEOUT 60
        WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}")
    gtest_discover_tests(beyond_blossom_profile_tests DISCOVERY_TIMEOUT 60 TEST_PREFIX "profile."
        WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}")
endif()

option(BB_BUILD_BENCHMARKS "Build native microbenchmarks" OFF)
if(BB_BUILD_BENCHMARKS)
    add_executable(beyond_blossom_perf ${PERF_FILES})
    target_link_libraries(beyond_blossom_perf libbeyond_blossom)
endif()

if(BB_BUILD_PROFILE_TOOLS)
    # Diagnostic instrumentation changes inline code and decoder operations.
    # Keep that configuration separate while using the same core source list.
    add_library(beyond_blossom_profile_core STATIC EXCLUDE_FROM_ALL ${SOURCE_FILES_NO_MAIN})
    set_target_properties(beyond_blossom_profile_core PROPERTIES POSITION_INDEPENDENT_CODE ON)
    target_include_directories(beyond_blossom_profile_core PUBLIC src)
    target_compile_definitions(beyond_blossom_profile_core PUBLIC BB_ENABLE_PROFILE=1)
    target_link_libraries(beyond_blossom_profile_core PUBLIC libstim beyond_blossom_build_options)
    enable_openmp_for_target(beyond_blossom_profile_core)
    if(BB_ENABLE_NVTX)
        target_compile_definitions(beyond_blossom_profile_core PUBLIC BB_ENABLE_NVTX=1)
        if(NVTX_INCLUDE_DIR)
            target_include_directories(beyond_blossom_profile_core PUBLIC "${NVTX_INCLUDE_DIR}")
        endif()
    endif()
    add_executable(sparse_blossom_profile ${PROFILE_CORE_FILES} ${PROFILE_RUNNER_FILES}
        src/beyond_blossom/profile/profile_main.cc)
    target_link_libraries(sparse_blossom_profile beyond_blossom_profile_core)
    add_executable(sparse_blossom_profile_baseline ${PROFILE_CORE_FILES} ${PROFILE_RUNNER_FILES}
        src/beyond_blossom/profile/profile_main.cc)
    target_link_libraries(sparse_blossom_profile_baseline libbeyond_blossom)
endif()

set(BB_PYBIND11_SOURCE_DIR "${CMAKE_CURRENT_LIST_DIR}/pybind11" CACHE PATH "pybind11 source directory")
if(BB_BUILD_PYTHON)
    if(EXISTS "${BB_PYBIND11_SOURCE_DIR}/CMakeLists.txt")
        add_subdirectory("${BB_PYBIND11_SOURCE_DIR}" "${CMAKE_CURRENT_BINARY_DIR}/pybind11")
    else()
        # Also accept an installed pybind11 package; no download is performed.
        find_package(pybind11 CONFIG QUIET)
    endif()
    if(COMMAND pybind11_add_module)
        pybind11_add_module(_cpp_beyond_blossom NO_EXTRAS ${PYTHON_API_FILES})
        target_link_libraries(_cpp_beyond_blossom PRIVATE libbeyond_blossom)
    else()
        message(FATAL_ERROR "Cannot build _cpp_beyond_blossom: install pybind11 or provide BB_PYBIND11_SOURCE_DIR or pybind11_DIR")
    endif()
endif()
