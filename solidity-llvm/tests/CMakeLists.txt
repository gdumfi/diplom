add_executable(unit_tests
    test_smoke.cpp
    test_lexer.cpp
    test_parser.cpp
    test_sema.cpp
    test_codegen.cpp
    test_analyzer.cpp
    test_access_control.cpp
    test_pattern_detectors.cpp
    test_reporter.cpp
    test_klee.cpp
    test_inheritance.cpp
)
target_compile_definitions(unit_tests PRIVATE
    SOLC_TESTS_DIR="${CMAKE_CURRENT_SOURCE_DIR}"
)
target_link_libraries(unit_tests PRIVATE
    solc_core
    GTest::gtest_main
)

include(GoogleTest)
gtest_discover_tests(unit_tests)
