# What RSA costs on this board.
#
# No network, no OPC UA stack, nothing but mbedTLS and a timer - so the numbers
# are the arithmetic alone, with nothing else competing for the core. A result
# measured inside the server would be the server's scheduling as much as the
# cipher.

# mbedTLS pulls in enough include directories that the compile command passes
# the 8191-character limit cmd.exe imposes on anything make launches. gcc then
# receives a truncated command line and reports "no input files", which reads
# like a missing source rather than a line-length problem. Response files move
# the include list into a file and leave the command short.
set(CMAKE_C_USE_RESPONSE_FILE_FOR_INCLUDES 1)
set(CMAKE_C_USE_RESPONSE_FILE_FOR_OBJECTS 1)
set(CMAKE_C_USE_RESPONSE_FILE_FOR_LIBRARIES 1)

set(TARGET_NAME crypto_bench)

add_executable(${TARGET_NAME}
        crypto_bench.c
        bench_keys.c
        )

# Stamp the image with where it came from, so the serial output identifies
# itself rather than relying on whoever flashed it to remember.
execute_process(
        COMMAND git rev-parse --short HEAD
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
        OUTPUT_VARIABLE BENCH_GIT_HASH
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET
        )
if(NOT BENCH_GIT_HASH)
    set(BENCH_GIT_HASH "no-git")
endif()
string(TIMESTAMP BENCH_BUILD_UTC "%Y-%m-%dT%H:%M:%SZ" UTC)

target_compile_definitions(${TARGET_NAME} PRIVATE
        BENCH_GIT="${BENCH_GIT_HASH}"
        BENCH_BUILT="${BENCH_BUILD_UTC}"
        )

# The RP2040 half of the matrix has no hardware RNG, and wiz_entropy.h stops the
# build on such a part unless the caller accepts the ring-oscillator source
# explicitly. Here that is the right call: the point of measuring on the RP2040
# is to find out what that source looks like.
target_compile_definitions(${TARGET_NAME} PRIVATE
        WIZ_ENTROPY_ALLOW_NO_TRNG=1
        )

target_link_libraries(${TARGET_NAME}
        pico_stdlib
        mbedcrypto
        mbedx509
        WIZ_ENTROPY_FILES
        )

# Run the whole image from RAM instead of executing in place from flash.
#
# The ECC rows move by up to 35% when unrelated code is added, while the RSA
# rows move by 0.06%. The standing explanation is the XIP cache: RSA's inner
# loop is small enough to stay resident, and the curve code is scattered, so its
# hit rate depends on where the linker happened to put it. If that is right,
# copying the image to RAM should make the curve timings both faster and stable,
# because there is no cache to miss. If it is wrong, nothing much changes and
# the explanation needs replacing.
#
# -DBENCH_RAM=1 at configure time. It needs the linker script, not just a
# define, which is what pico_set_binary_type does.
if(BENCH_RAM)
    pico_set_binary_type(${TARGET_NAME} copy_to_ram)
    message(STATUS "BENCH_RAM = 1 (crypto_bench runs from RAM)")
endif()

pico_enable_stdio_usb(${TARGET_NAME} 1)
pico_enable_stdio_uart(${TARGET_NAME} 0)

pico_add_extra_outputs(${TARGET_NAME})
