set(OPEN62541_WIZNET_AMALGAMATION_DIR
    "${CMAKE_BINARY_DIR}/open62541_pico_amalgamation"
    CACHE PATH "Directory containing generated open62541.c/open62541.h")

set(OPEN62541_WIZNET_C "${OPEN62541_WIZNET_AMALGAMATION_DIR}/open62541.c")
set(OPEN62541_WIZNET_H "${OPEN62541_WIZNET_AMALGAMATION_DIR}/open62541.h")
set(OPEN62541_WIZNET_PATCHED_C
    "${CMAKE_CURRENT_BINARY_DIR}/open62541_wiznet_minimal_ns0.c")

if(NOT EXISTS "${OPEN62541_WIZNET_C}" OR NOT EXISTS "${OPEN62541_WIZNET_H}")
    message(STATUS "open62541 amalgamation not found — generating automatically...")

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
)

target_link_libraries(OPEN62541_WIZNET PUBLIC
    pico_stdlib
    ETHERNET_FILES
    IOLIBRARY_FILES
)
