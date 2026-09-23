add_executable(geryon_tests
    test_memory_region.cpp
    test_fault_handler.cpp
    test_network_sync.cpp
    test_interprocess.cpp
    test_memnon_integration.cpp
    test_synchronization.cpp
    test_multi_client.cpp
    test_consistency.cpp
)

target_link_libraries(geryon_tests
    PRIVATE geryon
    PRIVATE gtest_main
)

include(GoogleTest)
if (NOT (CMAKE_SYSTEM_NAME STREQUAL "Windows" AND CMAKE_HOST_SYSTEM_NAME STREQUAL "Darwin"))
    gtest_discover_tests(geryon_tests)
endif()
