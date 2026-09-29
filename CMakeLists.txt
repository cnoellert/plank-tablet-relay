cmake_minimum_required(VERSION 3.20)
project(plank_tablet_relay LANGUAGES C CXX)

enable_testing()
add_library(plank_relay_protocol STATIC src/protocol.c src/session.c)
target_include_directories(plank_relay_protocol PUBLIC src)
target_compile_features(plank_relay_protocol PUBLIC c_std_11)

add_executable(protocol_test tests/protocol_test.c)
target_link_libraries(protocol_test PRIVATE plank_relay_protocol)
add_test(NAME protocol_test COMMAND protocol_test)

add_executable(session_test tests/session_test.c)
target_link_libraries(session_test PRIVATE plank_relay_protocol)
add_test(NAME session_test COMMAND session_test)

add_executable(wacom_identity_test tests/wacom_identity_test.cpp)
target_compile_features(wacom_identity_test PRIVATE cxx_std_17)
add_test(NAME wacom_identity_test COMMAND wacom_identity_test)

if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    add_library(plank_relay_pad STATIC src/pad.c)
    target_include_directories(plank_relay_pad PUBLIC src)
    target_compile_features(plank_relay_pad PUBLIC c_std_11)

    add_executable(pad_test tests/pad_test.c)
    target_link_libraries(pad_test PRIVATE plank_relay_pad)
    add_test(NAME pad_test COMMAND pad_test)

    add_executable(pad_probe tools/pad_probe.c)
    target_link_libraries(pad_probe PRIVATE plank_relay_pad)

    # A vendored source change must be made in the Client and pinned again.
    set(PLANK_CLIENT_VENDOR "${CMAKE_CURRENT_SOURCE_DIR}/vendor/plank-client")
    file(SHA256 "${PLANK_CLIENT_VENDOR}/linuxrawwacom.cpp" WORKER_CPP_SHA256)
    file(SHA256 "${PLANK_CLIENT_VENDOR}/linuxrawwacom.h" WORKER_H_SHA256)
    file(SHA256 "${PLANK_CLIENT_VENDOR}/wacomidentity.h" WORKER_IDENTITY_SHA256)
    file(SHA256 "${PLANK_CLIENT_VENDOR}/plank.h" PLANK_H_SHA256)
    if(NOT WORKER_CPP_SHA256 STREQUAL
            "654e2301f29779854d746185a5fb67230dd5d70942628a2c2b7f98193475804b" OR
       NOT WORKER_H_SHA256 STREQUAL
            "7f6c9880c2a91f80b1100246c8692e47b39a0973f460e10909ed8d20ccf916e4" OR
       NOT WORKER_IDENTITY_SHA256 STREQUAL
            "e339de432d646892ad1787741bfc3a61715dd5df239affe1ff90fec2bb3e93c4" OR
       NOT PLANK_H_SHA256 STREQUAL
            "b6a197da614ec5becc812b73d1d8e0ac5861cea22805f8443690b71ee16c8392")
        message(FATAL_ERROR "Vendored PLANK Client worker differs from its pinned snapshot")
    endif()

    find_package(PkgConfig REQUIRED)
    pkg_check_modules(UDEV REQUIRED IMPORTED_TARGET libudev)
    pkg_check_modules(SODIUM QUIET IMPORTED_TARGET "libsodium>=1.0.19")
    find_package(Threads REQUIRED)
    add_library(plank_wacom_worker STATIC "${PLANK_CLIENT_VENDOR}/linuxrawwacom.cpp")
    target_include_directories(plank_wacom_worker PUBLIC "${PLANK_CLIENT_VENDOR}")
    target_compile_features(plank_wacom_worker PUBLIC cxx_std_17)
    target_link_libraries(plank_wacom_worker PUBLIC PkgConfig::UDEV Threads::Threads)

    add_library(plank_relay_worker_bridge STATIC src/worker_bridge.cpp)
    target_include_directories(plank_relay_worker_bridge PUBLIC src)
    target_compile_features(plank_relay_worker_bridge PUBLIC cxx_std_17)
    target_link_libraries(plank_relay_worker_bridge PUBLIC plank_wacom_worker plank_relay_protocol)

    add_executable(worker_bridge_test tests/worker_bridge_test.cpp)
    target_link_libraries(worker_bridge_test PRIVATE plank_relay_worker_bridge)
    add_test(NAME worker_bridge_test COMMAND worker_bridge_test)

    if(SODIUM_FOUND)
        add_library(plank_ble_lab SHARED
            src/ble_lab.c src/protocol.c src/session.c src/link.c src/noise.c
            src/cpace.c src/confirm.c src/identity.c src/pair_budget.c
            src/pairing.c src/pair_wire.c)
        target_include_directories(plank_ble_lab PUBLIC src)
        target_compile_features(plank_ble_lab PRIVATE c_std_11)
        target_link_libraries(plank_ble_lab PRIVATE PkgConfig::SODIUM)

        add_library(plank_relay_noise STATIC src/noise.c)
        target_include_directories(plank_relay_noise PUBLIC src)
        target_compile_features(plank_relay_noise PUBLIC c_std_11)
        target_link_libraries(plank_relay_noise PUBLIC PkgConfig::SODIUM)

        add_executable(noise_vector_test tests/noise_vector_test.c src/noise.c)
        target_include_directories(noise_vector_test PRIVATE src)
        target_compile_definitions(noise_vector_test PRIVATE PLTR_NOISE_TESTING=1)
        target_link_libraries(noise_vector_test PRIVATE PkgConfig::SODIUM)
        add_test(NAME noise_vector_test COMMAND noise_vector_test)

        add_library(plank_relay_cpace STATIC src/cpace.c)
        target_include_directories(plank_relay_cpace PUBLIC src)
        target_compile_features(plank_relay_cpace PUBLIC c_std_11)
        target_link_libraries(plank_relay_cpace PUBLIC PkgConfig::SODIUM)

        add_executable(cpace_vector_test tests/cpace_vector_test.c src/cpace.c)
        target_include_directories(cpace_vector_test PRIVATE src)
        target_compile_definitions(cpace_vector_test PRIVATE PLTR_CPACE_TESTING=1)
        target_link_libraries(cpace_vector_test PRIVATE PkgConfig::SODIUM)
        add_test(NAME cpace_vector_test COMMAND cpace_vector_test)

        add_library(plank_relay_confirm STATIC src/confirm.c)
        target_include_directories(plank_relay_confirm PUBLIC src)
        target_compile_features(plank_relay_confirm PUBLIC c_std_11)
        target_link_libraries(plank_relay_confirm PUBLIC PkgConfig::SODIUM)

        add_executable(confirm_test tests/confirm_test.c)
        target_include_directories(confirm_test PRIVATE src)
        target_link_libraries(confirm_test PRIVATE plank_relay_confirm)
        add_test(NAME confirm_test COMMAND confirm_test)

        add_library(plank_relay_link STATIC src/link.c)
        target_include_directories(plank_relay_link PUBLIC src)
        target_compile_features(plank_relay_link PUBLIC c_std_11)
        target_link_libraries(plank_relay_link PUBLIC plank_relay_protocol plank_relay_noise PkgConfig::SODIUM)

        add_library(plank_relay_client_link STATIC src/client_link.c)
        target_include_directories(plank_relay_client_link PUBLIC src)
        target_compile_features(plank_relay_client_link PUBLIC c_std_11)
        target_link_libraries(plank_relay_client_link PUBLIC plank_relay_link)

        add_library(plank_relay_client_pair STATIC src/client_pair.c)
        target_include_directories(plank_relay_client_pair PUBLIC src)
        target_compile_features(plank_relay_client_pair PUBLIC c_std_11)
        target_link_libraries(plank_relay_client_pair PUBLIC
            plank_relay_cpace plank_relay_confirm plank_relay_noise plank_relay_protocol)

        add_executable(pair_e2e_test tests/pair_e2e_test.c)
        target_link_libraries(pair_e2e_test PRIVATE
            plank_relay_client_pair plank_relay_pairing)
        add_test(NAME pair_e2e_test COMMAND pair_e2e_test)

        add_executable(client_link_test tests/client_link_test.c)
        target_link_libraries(client_link_test PRIVATE plank_relay_client_link)
        add_test(NAME client_link_test COMMAND client_link_test)

        add_executable(ble_lab_test tests/ble_lab_test.c)
        target_link_libraries(ble_lab_test PRIVATE plank_ble_lab
            plank_relay_client_pair plank_relay_client_link)
        add_test(NAME ble_lab_test COMMAND ble_lab_test)
        add_executable(button_pair_test tests/button_pair_test.c)
        target_link_libraries(button_pair_test PRIVATE plank_ble_lab plank_relay_client_pair plank_relay_client_link)
        add_test(NAME button_pair_test COMMAND button_pair_test)
        find_package(Python3 COMPONENTS Interpreter REQUIRED)
        add_test(NAME ble_transport_test COMMAND "${Python3_EXECUTABLE}"
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/ble_transport_test.py")
        add_test(NAME ble_controller_test COMMAND "${Python3_EXECUTABLE}"
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/ble_controller_test.py")
        add_test(NAME ble_service_test COMMAND "${Python3_EXECUTABLE}"
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/ble_service_test.py")

        add_executable(link_test tests/link_test.c)
        target_link_libraries(link_test PRIVATE plank_relay_link)
        add_test(NAME link_test COMMAND link_test)

        add_library(plank_relay_identity STATIC src/identity.c)
        target_include_directories(plank_relay_identity PUBLIC src)
        target_compile_features(plank_relay_identity PUBLIC c_std_11)
        target_link_libraries(plank_relay_identity PUBLIC plank_relay_noise PkgConfig::SODIUM)

        add_library(plank_relay_pair_budget STATIC src/pair_budget.c)
        target_include_directories(plank_relay_pair_budget PUBLIC src)
        target_compile_features(plank_relay_pair_budget PUBLIC c_std_11)
        target_link_libraries(plank_relay_pair_budget PUBLIC plank_relay_identity)

        add_executable(pair_budget_test tests/pair_budget_test.c)
        target_link_libraries(pair_budget_test PRIVATE plank_relay_pair_budget)
        add_test(NAME pair_budget_test COMMAND pair_budget_test)

        target_link_libraries(link_test PRIVATE plank_relay_identity)

        add_executable(identity_test tests/identity_test.c)
        target_link_libraries(identity_test PRIVATE plank_relay_identity)
        add_test(NAME identity_test COMMAND identity_test)

        add_library(plank_relay_pairing STATIC src/pairing.c src/pair_wire.c)
        target_include_directories(plank_relay_pairing PUBLIC src)
        target_compile_features(plank_relay_pairing PUBLIC c_std_11)
        target_link_libraries(plank_relay_pairing PUBLIC plank_relay_identity plank_relay_cpace plank_relay_confirm plank_relay_protocol)

        add_executable(pairing_test tests/pairing_test.c)
        target_link_libraries(pairing_test PRIVATE plank_relay_pairing)
        add_test(NAME pairing_test COMMAND pairing_test)

        add_executable(pair_wire_test tests/pair_wire_test.c)
        target_link_libraries(pair_wire_test PRIVATE plank_relay_pairing)
        add_test(NAME pair_wire_test COMMAND pair_wire_test)

        add_library(plank_relay_session_dispatcher STATIC src/session_dispatcher.cpp)
        target_include_directories(plank_relay_session_dispatcher PUBLIC src)
        target_compile_features(plank_relay_session_dispatcher PUBLIC cxx_std_17)
        target_link_libraries(plank_relay_session_dispatcher PUBLIC plank_relay_worker_bridge plank_relay_link)

        add_executable(session_dispatcher_test tests/session_dispatcher_test.cpp)
        target_link_libraries(session_dispatcher_test PRIVATE plank_relay_session_dispatcher)
        add_test(NAME session_dispatcher_test COMMAND session_dispatcher_test)

        add_library(plank_relay_tcp_session STATIC
            src/stream_session.cpp src/tcp_session.cpp)
        target_include_directories(plank_relay_tcp_session PUBLIC src)
        target_compile_features(plank_relay_tcp_session PUBLIC cxx_std_17)
        target_link_libraries(plank_relay_tcp_session PUBLIC
            plank_relay_session_dispatcher plank_relay_identity)

        add_library(plank_relay_tcp_pair_session STATIC src/tcp_pair_session.cpp)
        target_include_directories(plank_relay_tcp_pair_session PUBLIC src)
        target_compile_features(plank_relay_tcp_pair_session PUBLIC cxx_std_17)
        target_link_libraries(plank_relay_tcp_pair_session PUBLIC
            plank_relay_pairing plank_relay_pad)

        add_executable(tcp_pair_session_test tests/tcp_pair_session_test.cpp)
        target_link_libraries(tcp_pair_session_test PRIVATE
            plank_relay_tcp_pair_session plank_relay_client_pair)
        add_test(NAME tcp_pair_session_test COMMAND tcp_pair_session_test)

        add_executable(plank-tablet-relay src/main.cpp)
        target_link_libraries(plank-tablet-relay PRIVATE
            plank_relay_tcp_session plank_relay_tcp_pair_session plank_relay_pair_budget)

        add_executable(tcp_session_test tests/tcp_session_test.cpp)
        target_link_libraries(tcp_session_test PRIVATE plank_relay_tcp_session)
        add_test(NAME tcp_session_test COMMAND tcp_session_test)
    else()
        message(STATUS "Noise tests disabled: libsodium 1.0.19 or newer not found")
    endif()
endif()
