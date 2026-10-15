# Security policies in the generated amalgamation.
#
# The generator decides this: with UA_ENABLE_ENCRYPTION=MBEDTLS the produced
# header defines UA_ENABLE_ENCRYPTION and UA_ENABLE_ENCRYPTION_MBEDTLS, and
# without it the same lines are #undef. The crypto plugin sources are
# concatenated either way (open62541 CMakeLists:1188 includes them whenever
# UA_ENABLE_AMALGAMATION is on), so the difference on disk is about 29 KB of
# source and two defines - but those defines are what compile the policies in.
#
# -DOPCUA_ENABLE_ENCRYPTION=ON at configure time. The two settings get separate
# amalgamation directories: the generation step is skipped when the output
# already exists, so sharing one directory would silently serve whichever was
# generated first.
option(OPCUA_ENABLE_ENCRYPTION
       "Generate the open62541 amalgamation with mbedTLS security policies" OFF)

if(OPCUA_ENABLE_ENCRYPTION)
    set(_ua_amalg_suffix "-enc")
else()
    set(_ua_amalg_suffix "")
endif()

set(OPEN62541_WIZNET_AMALGAMATION_DIR
    "${CMAKE_BINARY_DIR}/open62541_pico_amalgamation${_ua_amalg_suffix}"
    CACHE PATH "Directory containing generated open62541.c/open62541.h" FORCE)

set(OPEN62541_WIZNET_C "${OPEN62541_WIZNET_AMALGAMATION_DIR}/open62541.c")
set(OPEN62541_WIZNET_H "${OPEN62541_WIZNET_AMALGAMATION_DIR}/open62541.h")
set(OPEN62541_WIZNET_PATCHED_C
    "${CMAKE_CURRENT_BINARY_DIR}/open62541_wiznet_minimal_ns0.c")

if(NOT EXISTS "${OPEN62541_WIZNET_C}" OR NOT EXISTS "${OPEN62541_WIZNET_H}")
    message(STATUS "open62541 amalgamation not found — generating automatically...")

    set(_ua_enc_args "")
    if(OPCUA_ENABLE_ENCRYPTION)
        # find_package(MbedTLS) insists on three library files before it will
        # report success, even though the amalgamation target only concatenates
        # sources and links nothing. The real libraries are built later in this
        # same tree and do not exist yet at configure time, so empty stand-ins
        # are created purely to satisfy the search. Nothing reads them.
        set(_mbed_stub "${CMAKE_BINARY_DIR}/amalgamation_mbedtls_stub")
        file(MAKE_DIRECTORY "${_mbed_stub}")
        foreach(_lib mbedtls mbedx509 mbedcrypto)
            if(NOT EXISTS "${_mbed_stub}/lib${_lib}.a")
                file(TOUCH "${_mbed_stub}/lib${_lib}.a")
            endif()
        endforeach()

        list(APPEND _ua_enc_args
            -DUA_ENABLE_ENCRYPTION=MBEDTLS
            -DMBEDTLS_INCLUDE_DIRS=${CMAKE_SOURCE_DIR}/libraries/mbedtls/include
            -DMBEDTLS_LIBRARY=${_mbed_stub}/libmbedtls.a
            -DMBEDX509_LIBRARY=${_mbed_stub}/libmbedx509.a
            -DMBEDCRYPTO_LIBRARY=${_mbed_stub}/libmbedcrypto.a
        )
        message(STATUS "open62541 amalgamation: security policies ENABLED (mbedTLS)")
    endif()

    execute_process(
        COMMAND ${CMAKE_COMMAND}
            -S ${CMAKE_SOURCE_DIR}/libraries/open62541
            -B ${OPEN62541_WIZNET_AMALGAMATION_DIR}
            -DUA_ARCHITECTURE=none
            -DUA_ENABLE_AMALGAMATION=ON
            -DUA_NAMESPACE_ZERO=MINIMAL
            -DUA_ENABLE_METHODCALLS=ON
            -DUA_ENABLE_SUBSCRIPTIONS=ON
            -DUA_ENABLE_SUBSCRIPTIONS_EVENTS=OFF
            -DUA_ENABLE_HISTORIZING=OFF
            -DUA_ENABLE_PUBSUB=OFF
            -DUA_ENABLE_DISCOVERY=OFF
            -DUA_ENABLE_NODEMANAGEMENT=ON
            -DUA_MULTITHREADING=0
            -DUA_BUILD_EXAMPLES=OFF
            -DUA_BUILD_UNIT_TESTS=OFF
            ${_ua_enc_args}
        RESULT_VARIABLE _cfg_result
    )
    if(NOT _cfg_result EQUAL 0)
        message(FATAL_ERROR "open62541 amalgamation configure failed (exit ${_cfg_result})")
    endif()

    execute_process(
        COMMAND ${CMAKE_COMMAND}
            --build ${OPEN62541_WIZNET_AMALGAMATION_DIR}
            --target open62541-amalgamation
            --config Release
        RESULT_VARIABLE _build_result
    )
    if(NOT _build_result EQUAL 0)
        message(FATAL_ERROR "open62541 amalgamation build failed (exit ${_build_result})")
    endif()

    message(STATUS "open62541 amalgamation generated successfully.")
endif()

file(READ "${OPEN62541_WIZNET_C}" OPEN62541_WIZNET_SOURCE)
set(OPEN62541_WIZNET_PATCHED_SOURCE "${OPEN62541_WIZNET_SOURCE}")
string(REPLACE
[=[#ifdef UA_GENERATED_NAMESPACE_ZERO
    /* Standard configuration: generate NS0 nodes at runtime */
    res = initNS0(server);
#else
    /* NONE configuration: NS0 pre-loaded by external nodestore (e.g., ROM).
     * Only connect data sources for dynamic values like ServerTime, ServerStatus, etc. */
    res = initNS0_dataSources(server);
#endif]=]
[=[#ifdef UA_GENERATED_NAMESPACE_ZERO
    /* Standard configuration: generate NS0 nodes at runtime */
    res = initNS0(server);
#else
    /* WIZnet MINIMAL build: create open62541's built-in minimal NS0.
     * This keeps Root/Objects/Server available without REDUCED generated nodes. */
    res = initNS0(server);
#endif]=]
    OPEN62541_WIZNET_PATCHED_SOURCE
    "${OPEN62541_WIZNET_PATCHED_SOURCE}")

if(OPEN62541_WIZNET_PATCHED_SOURCE STREQUAL OPEN62541_WIZNET_SOURCE)
    message(FATAL_ERROR
        "Failed to patch open62541 minimal NS0 startup path. "
        "Check generated open62541.c layout.")
endif()

file(WRITE "${OPEN62541_WIZNET_PATCHED_C}"
     "${OPEN62541_WIZNET_PATCHED_SOURCE}")

add_library(OPEN62541_WIZNET STATIC
    ${OPEN62541_WIZNET_PATCHED_C}
    open62541_wiznet_eventloop.c
)

target_include_directories(OPEN62541_WIZNET PUBLIC
    ${OPEN62541_WIZNET_AMALGAMATION_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}
)

target_compile_definitions(OPEN62541_WIZNET PUBLIC
    UA_ARCHITECTURE_LWIP
    UA_LOGLEVEL=300
    # 이벤트루프 진단. 멀티소켓 브링업(2026-09-03)에 썼다.
    # 소켓 배정·수신 크기·연결 개폐를 [el] 접두어로 찍는다.
    # 문제 추적이 필요하면 1 로 올린다.
    WIZ_EL_DEBUG=0
)

target_link_libraries(OPEN62541_WIZNET PUBLIC
    pico_stdlib
    ETHERNET_FILES
    IOLIBRARY_FILES
)

# With the policies compiled in, the amalgamation calls into mbedTLS directly.
#
# WIZ_ENTROPY_FILES is deliberately NOT linked here. It is an INTERFACE library
# that hands its source to whoever links it, so linking it both here and in the
# executable would compile wiz_entropy.c twice into the same image. The
# executable is the one that links it.
if(OPCUA_ENABLE_ENCRYPTION)
    target_link_libraries(OPEN62541_WIZNET PUBLIC
        mbedcrypto
        mbedx509
    )
endif()
