cmake_minimum_required(VERSION 3.25)
project(KytyTrackerReplay LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

get_filename_component(KYTY_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)
get_filename_component(ANYPS5_DIR "${KYTY_DIR}/../.." ABSOLUTE)
set(TRACKER_RS_DIR "${KYTY_DIR}/tracker_rs")

find_program(KYTY_CARGO NAMES cargo HINTS "$ENV{CARGO_HOME}/bin" "$ENV{HOME}/.cargo/bin" REQUIRED)
set(trackerRustTargetDir "${CMAKE_BINARY_DIR}/cargo")
set(trackerRustArchive "${trackerRustTargetDir}/release/libkyty_tracker_rs.a")
file(GLOB trackerRustSources CONFIGURE_DEPENDS "${TRACKER_RS_DIR}/src/*.rs")
add_custom_command(
    OUTPUT "${trackerRustArchive}"
    COMMAND ${CMAKE_COMMAND} -E env "CARGO_TARGET_DIR=${trackerRustTargetDir}" "RUSTFLAGS=-C target-cpu=x86-64-v3 -C force-frame-pointers=yes"
            "${KYTY_CARGO}" build --release --offline --lib --manifest-path "${TRACKER_RS_DIR}/Cargo.toml"
    DEPENDS ${trackerRustSources} "${TRACKER_RS_DIR}/Cargo.toml"
    COMMENT "Building the Rust memory tracker"
    VERBATIM)
add_custom_target(tracker_rs_archive DEPENDS "${trackerRustArchive}")

set(trackerFlags -march=x86-64-v3 -ffp-contract=off -fno-rtti -fno-exceptions -fno-strict-aliasing -fno-omit-frame-pointer -g -O3)
set(trackerIncludes
    "${CMAKE_CURRENT_SOURCE_DIR}/include"
    "${KYTY_DIR}"
    "${ANYPS5_DIR}/core/libs/prx/libSceAgcDriver/KytyShim/include"
    "${ANYPS5_DIR}/core/libs"
    "${ANYPS5_DIR}/3rdparty/fmt/include")

foreach(flavour IN ITEMS cpp rs)
    set(target tracker_replay_${flavour})
    add_executable(${target}
        "${CMAKE_CURRENT_SOURCE_DIR}/replay.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/stubs.cpp"
        "${KYTY_DIR}/graphics/host_gpu/pageManager.cpp"
        "${KYTY_DIR}/graphics/host_gpu/memoryTracker.cpp")
    target_include_directories(${target} BEFORE PRIVATE ${trackerIncludes})
    target_compile_definitions(${target} PRIVATE KYTY_TRACKER_REPLAY=1 ANYPS5_KYTY=1 FMT_HEADER_ONLY=1)
    target_compile_options(${target} PRIVATE ${trackerFlags})
    target_link_libraries(${target} PRIVATE pthread ${CMAKE_DL_LIBS})
endforeach()
target_sources(tracker_replay_rs PRIVATE "${KYTY_DIR}/graphics/host_gpu/trackerRustHost.cpp")
target_compile_definitions(tracker_replay_rs PRIVATE ANYPS5_KYTY_RUST_TRACKER=1)
target_include_directories(tracker_replay_rs PRIVATE "${TRACKER_RS_DIR}/include")
add_dependencies(tracker_replay_rs tracker_rs_archive)
target_link_libraries(tracker_replay_rs PRIVATE "${trackerRustArchive}")

add_executable(tracker_compare "${CMAKE_CURRENT_SOURCE_DIR}/compare.cpp")
target_compile_options(tracker_compare PRIVATE -O2 -g)
