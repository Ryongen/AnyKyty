find_program(KYTY_CARGO NAMES cargo HINTS "$ENV{CARGO_HOME}/bin" "$ENV{HOME}/.cargo/bin")
if(NOT KYTY_CARGO)
    message(FATAL_ERROR "KYTY_SELF_MARKING builds core/kyty/selfmark_rs with cargo, which was not found: install Rust (https://rustup.rs), or configure AnyPS5 with -DANYPS5_KYTY_SELF_MARKING=OFF")
endif()
set(kytySelfMarkTargetDir "${CMAKE_BINARY_DIR}/selfmark_rs")
set(kytySelfMarkArgs build --release --offline --lib --manifest-path "${KYTY_DIR}/selfmark_rs/Cargo.toml")
if(WIN32)
    set(KYTY_SELF_MARKING_TRIPLE "x86_64-pc-windows-gnu" CACHE STRING "Rust target of the self-marking crate on Windows")
    list(APPEND kytySelfMarkArgs --target "${KYTY_SELF_MARKING_TRIPLE}")
    set(KYTY_SELF_MARKING_ARCHIVE "${kytySelfMarkTargetDir}/${KYTY_SELF_MARKING_TRIPLE}/release/libkyty_selfmark_rs.a")
else()
    set(KYTY_SELF_MARKING_ARCHIVE "${kytySelfMarkTargetDir}/release/libkyty_selfmark_rs.a")
endif()
file(GLOB kytySelfMarkSources CONFIGURE_DEPENDS "${KYTY_DIR}/selfmark_rs/src/*.rs")
add_custom_command(
    OUTPUT "${KYTY_SELF_MARKING_ARCHIVE}"
    COMMAND ${CMAKE_COMMAND} -E env "CARGO_TARGET_DIR=${kytySelfMarkTargetDir}" "RUSTFLAGS=-C force-frame-pointers=yes" "${KYTY_CARGO}" ${kytySelfMarkArgs}
    DEPENDS ${kytySelfMarkSources} "${KYTY_DIR}/selfmark_rs/Cargo.toml" "${KYTY_DIR}/selfmark_rs/Cargo.lock"
    COMMENT "Building the write site patcher (core/kyty/selfmark_rs) with cargo"
    VERBATIM
)
add_custom_target(kyty_selfmark_rs DEPENDS "${KYTY_SELF_MARKING_ARCHIVE}")
add_dependencies(kyty_gpu kyty_selfmark_rs)
target_compile_definitions(kyty_gpu PUBLIC KYTY_SELF_MARKING=1)
target_include_directories(kyty_gpu PUBLIC "${KYTY_DIR}/selfmark_rs/include")
target_link_libraries(kyty_gpu PUBLIC "${KYTY_SELF_MARKING_ARCHIVE}")
