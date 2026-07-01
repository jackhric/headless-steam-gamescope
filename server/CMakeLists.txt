cmake_minimum_required(VERSION 3.20)
project(steam_stream_rtpmoonlight LANGUAGES C CXX)

# Standalone build of Wolf's rtpmoonlightpay_video / rtpmoonlightpay_audio elements
# as a single loadable GStreamer plugin. Build this AGAINST Wolf's /usr/local
# GStreamer 1.26.x (set PKG_CONFIG_PATH) so the ABI matches the runtime image.

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_C_STANDARD 11)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

set(SRC   ${CMAKE_CURRENT_SOURCE_DIR}/src)
set(MP    ${SRC}/moonlight-protocol)
set(TESTS ${CMAKE_CURRENT_SOURCE_DIR}/tests)

find_package(PkgConfig REQUIRED)
pkg_check_modules(GST REQUIRED IMPORTED_TARGET
        gstreamer-1.0
        gstreamer-base-1.0)

find_package(OpenSSL REQUIRED)
find_package(Threads REQUIRED)

# Wolf links Boost.Log dynamically; logger.hpp pulls it in transitively.
set(Boost_USE_STATIC_LIBS OFF)
find_package(Boost REQUIRED COMPONENTS log log_setup thread system)

# ---- nanors (Reed-Solomon FEC) ----------------------------------------------
add_library(nanors STATIC ${MP}/nanors/rswrapper.c)
target_include_directories(nanors PUBLIC
        ${MP}/nanors
        ${MP}/nanors-upstream
        ${MP}/nanors-upstream/deps/obl)
set_source_files_properties(${MP}/nanors/rswrapper.c
        PROPERTIES COMPILE_FLAGS "-ftree-vectorize -funroll-loops")
target_compile_options(nanors PRIVATE -w)

# ---- wolf crypto -------------------------------------------------------------
file(GLOB CRYPTO_SRCS ${MP}/crypto/src/*.cpp)
add_library(wolf_crypto STATIC ${CRYPTO_SRCS})
target_include_directories(wolf_crypto PUBLIC ${MP}/crypto)
target_link_libraries(wolf_crypto PUBLIC OpenSSL::SSL OpenSSL::Crypto)

# ---- the plugin --------------------------------------------------------------
add_library(gstrtpmoonlight SHARED
        ${SRC}/gst-plugin/gstrtpmoonlightpay_video.cpp
        ${SRC}/gst-plugin/gstrtpmoonlightpay_audio.cpp
        ${SRC}/gst-plugin/plugin.cpp)

target_include_directories(gstrtpmoonlight PRIVATE
        ${SRC}            # <gst-plugin/...> and <helpers/...>
        ${MP}             # <moonlight/...>
        ${MP}/crypto)     # <crypto/...>

# fmt header-only, exactly as Wolf uses it (fmt::fmt-header-only)
target_compile_definitions(gstrtpmoonlight PRIVATE FMT_HEADER_ONLY=1 BOOST_LOG_DYN_LINK=1)

target_link_libraries(gstrtpmoonlight PRIVATE
        PkgConfig::GST
        nanors
        wolf_crypto
        Boost::log Boost::log_setup Boost::thread Boost::system
        Threads::Threads)

# The GStreamer loader derives the entry-point symbol from the filename:
# libgst<NAME>.so -> gst_plugin_<NAME>_get_desc. NAME must match the plugin name
# given to GST_PLUGIN_DEFINE (rtpmoonlightpay), so the file must be
# libgstrtpmoonlightpay.so.
set_target_properties(gstrtpmoonlight PROPERTIES
        OUTPUT_NAME gstrtpmoonlightpay
        PREFIX "lib")

# =============================================================================
# M2: steam-stream-server -- the Moonlight host control plane (HTTP/HTTPS REST +
# PIN pairing) reusing wolf_crypto. Built only when -DBUILD_SERVER=ON.
# =============================================================================
option(BUILD_SERVER "Build the steam-stream Moonlight host server (M2)" OFF)

if(BUILD_SERVER)
    # Header-only Simple-Web-Server (same fork/commit Wolf uses); path from Docker.
    set(SIMPLE_WEB_SERVER_DIR "$ENV{SIMPLE_WEB_SERVER_DIR}" CACHE PATH "Simple-Web-Server include dir")
    if(NOT SIMPLE_WEB_SERVER_DIR)
        message(FATAL_ERROR "Set SIMPLE_WEB_SERVER_DIR to the Simple-Web-Server checkout")
    endif()

    # Header-only cpp-peglib (single header peglib.h) -- needed by the copied
    # moonlight-protocol/rtsp/parser.hpp. Extracted offline (build-server.sh) the same
    # way as Simple-Web-Server, so no agent-initiated git clone.
    set(PEGLIB_DIR "$ENV{PEGLIB_DIR}" CACHE PATH "cpp-peglib include dir (contains peglib.h)")
    if(NOT PEGLIB_DIR)
        message(FATAL_ERROR "Set PEGLIB_DIR to the directory containing peglib.h")
    endif()

    # M4: GStreamer App (appsink) for the live media send path, and the Moonlight ENet
    # fork (vendored offline, like the deps above) for the control channel.
    pkg_check_modules(GSTAPP REQUIRED IMPORTED_TARGET gstreamer-app-1.0)

    set(ENET_DIR "$ENV{ENET_DIR}" CACHE PATH "Moonlight ENet fork source dir (enet-src)")
    if(NOT ENET_DIR)
        message(FATAL_ERROR "Set ENET_DIR to the extracted enet-src checkout")
    endif()
    add_subdirectory(${ENET_DIR} ${CMAKE_BINARY_DIR}/enet-build)

    set(SERVER_PROTO ${SRC}/server/moonlight_proto.cpp)

    add_executable(steam-stream-server
            ${SRC}/server/main.cpp
            ${SRC}/server/servers.cpp
            ${SRC}/server/state.cpp
            ${SRC}/server/https_server.cpp
            ${SRC}/server/rtsp_server.cpp
            ${SRC}/server/media.cpp
            ${SRC}/server/launcher.cpp
            ${SRC}/server/control.cpp
            ${SRC}/server/uinput.cpp
            ${SERVER_PROTO})

    target_include_directories(steam-stream-server PRIVATE
            ${SRC}                      # <server/...> and <helpers/...>
            ${MP}                       # <rtsp/parser.hpp>
            ${MP}/crypto                # <crypto/...>
            ${SIMPLE_WEB_SERVER_DIR}    # <server_http.hpp>
            ${PEGLIB_DIR})              # <peglib.h>

    target_compile_definitions(steam-stream-server PRIVATE FMT_HEADER_ONLY=1 BOOST_LOG_DYN_LINK=1)

    target_link_libraries(steam-stream-server PRIVATE
            wolf_crypto
            enet
            PkgConfig::GST PkgConfig::GSTAPP
            OpenSSL::SSL OpenSSL::Crypto
            Boost::log Boost::log_setup Boost::thread Boost::system
            Threads::Threads)

    # =========================================================================
    # Test suite (server/tests/). UNIT tests are pure (no GPU/client/sockets) and
    # are registered with ctest under the label "unit"; the uinput integration
    # binary is labelled "integration". The shell integration tiers (REST->RTSP,
    # media pipeline) are driven by tests/run-tests.sh in a GPU container.
    # No test framework: each binary uses plain asserts + a PASS/FAIL line.
    # =========================================================================
    enable_testing()

    # -- M2: known-answer pairing crypto test --
    add_executable(test_pairing
            ${TESTS}/unit/test_pairing.cpp
            ${SERVER_PROTO})
    target_include_directories(test_pairing PRIVATE ${SRC} ${MP}/crypto)
    target_compile_definitions(test_pairing PRIVATE FMT_HEADER_ONLY=1)
    target_link_libraries(test_pairing PRIVATE wolf_crypto OpenSSL::SSL OpenSSL::Crypto Threads::Threads)

    # -- M3: known-answer RTSP verb test (verbs + SDP + AES-format proof). Header-only
    #    handlers, so no rtsp_server.cpp needed here. --
    add_executable(test_rtsp ${TESTS}/unit/test_rtsp.cpp)
    target_include_directories(test_rtsp PRIVATE ${SRC} ${MP} ${MP}/crypto ${PEGLIB_DIR})
    target_compile_definitions(test_rtsp PRIVATE FMT_HEADER_ONLY=1 BOOST_LOG_DYN_LINK=1)
    target_link_libraries(test_rtsp PRIVATE
            wolf_crypto OpenSSL::SSL OpenSSL::Crypto
            Boost::log Boost::log_setup Boost::thread Boost::system Threads::Threads)

    # -- M4: control-channel crypto KAT (no GPU/ENet/uinput needed). --
    add_executable(test_control ${TESTS}/unit/test_control.cpp)
    target_include_directories(test_control PRIVATE ${SRC} ${MP} ${MP}/crypto)
    target_compile_definitions(test_control PRIVATE FMT_HEADER_ONLY=1 BOOST_LOG_DYN_LINK=1)
    target_link_libraries(test_control PRIVATE
            wolf_crypto OpenSSL::SSL OpenSSL::Crypto
            Boost::log Boost::log_setup Boost::thread Boost::system Threads::Threads)

    # -- M10: per-session AES key rotation on resume (control decrypt re-keys cleanly). --
    add_executable(test_resume_key ${TESTS}/unit/test_resume_key.cpp)
    target_include_directories(test_resume_key PRIVATE ${SRC} ${MP} ${MP}/crypto)
    target_compile_definitions(test_resume_key PRIVATE FMT_HEADER_ONLY=1 BOOST_LOG_DYN_LINK=1)
    target_link_libraries(test_resume_key PRIVATE
            wolf_crypto OpenSSL::SSL OpenSSL::Crypto
            Boost::log Boost::log_setup Boost::thread Boost::system Threads::Threads)

    # -- M1/M3/M4: FEC (nanors Reed-Solomon) encode->decode roundtrip. --
    add_executable(test_fec ${TESTS}/unit/test_fec.cpp)
    target_include_directories(test_fec PRIVATE ${MP})
    target_link_libraries(test_fec PRIVATE nanors)

    # -- M4/M1: AES roundtrip (control GCM + audio CBC paths). --
    add_executable(test_aes ${TESTS}/unit/test_aes.cpp)
    target_include_directories(test_aes PRIVATE ${MP}/crypto)
    target_link_libraries(test_aes PRIVATE wolf_crypto OpenSSL::SSL OpenSSL::Crypto)

    # -- M2: GameStream serverinfo/applist/launch XML correctness. --
    add_executable(test_xml ${TESTS}/unit/test_xml.cpp ${SERVER_PROTO})
    target_include_directories(test_xml PRIVATE ${SRC} ${MP}/crypto)
    target_compile_definitions(test_xml PRIVATE FMT_HEADER_ONLY=1)
    target_link_libraries(test_xml PRIVATE
            wolf_crypto OpenSSL::SSL OpenSSL::Crypto
            Boost::thread Boost::system Threads::Threads)

    # -- M2/M3: session AES-key-from-rikey/rikeyid (+ pairing key) format assertion. --
    add_executable(test_session_key ${TESTS}/unit/test_session_key.cpp ${SERVER_PROTO})
    target_include_directories(test_session_key PRIVATE ${SRC} ${MP}/crypto)
    target_compile_definitions(test_session_key PRIVATE FMT_HEADER_ONLY=1)
    target_link_libraries(test_session_key PRIVATE
            wolf_crypto OpenSSL::SSL OpenSSL::Crypto
            Boost::thread Boost::system Threads::Threads)

    # -- M4: uinput device-creation smoke test (needs --device /dev/uinput at runtime). --
    add_executable(test_uinput ${TESTS}/integration/test_uinput.cpp ${SRC}/server/uinput.cpp)
    target_include_directories(test_uinput PRIVATE ${SRC})
    target_compile_definitions(test_uinput PRIVATE FMT_HEADER_ONLY=1 BOOST_LOG_DYN_LINK=1)
    target_link_libraries(test_uinput PRIVATE
            Boost::log Boost::log_setup Boost::thread Boost::system Threads::Threads)

    # ---- ctest registration ----
    # UNIT tier: pure, runs anywhere (CPU-only, no devices). `ctest -L unit`.
    foreach(t test_pairing test_rtsp test_control test_resume_key test_fec test_aes test_xml test_session_key)
        add_test(NAME ${t} COMMAND ${t})
        set_tests_properties(${t} PROPERTIES LABELS "unit")
    endforeach()

    # INTEGRATION tier: needs real devices. `ctest -L integration` (run in a GPU +
    # /dev/uinput container by tests/run-tests.sh). test_uinput needs /dev/uinput.
    add_test(NAME test_uinput COMMAND test_uinput)
    set_tests_properties(test_uinput PROPERTIES LABELS "integration")
endif()
