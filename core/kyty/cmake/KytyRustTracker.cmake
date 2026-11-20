find_program(KYTY_CARGO NAMES cargo HINTS "$ENV{CARGO_HOME}/bin" "$ENV{HOME}/.cargo/bin")
if(NOT KYTY_CARGO)
    message(FATAL_ERROR "KYTY_RUST_TRACKER builds core/kyty/tracker_rs with cargo, which was not found: install Rust (https://rustup.rs), or configure AnyPS5 with -DANYPS5_KYTY_RUST_TRACKER=OFF")
endif()
set(kytyRustTargetDir "${CMAKE_BINARY_DIR}/tracker_rs")
set(kytyRustArgs build --release --offline --lib --manifest-path "${KYTY_DIR}/tracker_rs/Cargo.toml")
set(kytyRustFlags "-C target-cpu=x86-64-v3 -C force-frame-pointers=yes")
if(WIN32)
    set(KYTY_RUST_TRIPLE "x86_64-pc-windows-gnullvm" CACHE STRING "Rust target of the memory tracker crate on Windows")
    list(APPEND kytyRustArgs --target "${KYTY_RUST_TRIPLE}")
    set(KYTY_RUST_TRACKER_ARCHIVE "${kytyRustTargetDir}/${KYTY_RUST_TRIPLE}/release/libkyty_tracker_rs.a")
else()
    set(KYTY_RUST_TRACKER_ARCHIVE "${kytyRustTargetDir}/release/libkyty_tracker_rs.a")
endif()
file(GLOB kytyRustSources CONFIGURE_DEPENDS "${KYTY_DIR}/tracker_rs/src/*.rs")
add_custom_command(
    OUTPUT "${KYTY_RUST_TRACKER_ARCHIVE}"
    COMMAND ${CMAKE_COMMAND} -E env "CARGO_TARGET_DIR=${kytyRustTargetDir}" "RUSTFLAGS=${kytyRustFlags}" "${KYTY_CARGO}" ${kytyRustArgs}
    DEPENDS ${kytyRustSources} "${KYTY_DIR}/tracker_rs/Cargo.toml" "${KYTY_DIR}/tracker_rs/Cargo.lock"
    COMMENT "Building the Rust memory tracker (core/kyty/tracker_rs) with cargo"
    VERBATIM
)
add_custom_target(kyty_tracker_rs DEPENDS "${KYTY_RUST_TRACKER_ARCHIVE}")
add_dependencies(kyty_gpu kyty_tracker_rs)
target_compile_definitions(kyty_gpu PUBLIC ANYPS5_KYTY_RUST_TRACKER=1)
target_include_directories(kyty_gpu PUBLIC "${KYTY_DIR}/tracker_rs/include")
target_link_libraries(kyty_gpu PUBLIC "${KYTY_RUST_TRACKER_ARCHIVE}")
