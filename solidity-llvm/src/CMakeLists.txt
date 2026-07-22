add_library(solc_core STATIC
    analyzer/AccessControlDetector.cpp
    analyzer/AnalysisPrep.cpp
    analyzer/IRTrace.cpp
    analyzer/PatternDetectors.cpp
    analyzer/ReentrancyDetector.cpp
    ast/AST.cpp
    codegen/AstJsonLowering.cpp
    codegen/CodeGen.cpp
    codegen/EvmIntrinsics.cpp
    diag/Diagnostic.cpp
    driver/Driver.cpp
    driver/EtherscanFetch.cpp
    driver/SolcRunner.cpp
    klee/KleeLowering.cpp
    lexer/Lexer.cpp
    parser/Parser.cpp
    report/ConsoleReporter.cpp
    report/ContextPackWriter.cpp
    report/InvariantGen.cpp
    report/JsonReporter.cpp
    report/PocGenerator.cpp
    report/SarifReporter.cpp
    sema/Sema.cpp
    sema/Inheritance.cpp
    sema/Symbol.cpp
)
target_include_directories(solc_core
    PUBLIC ${CMAKE_SOURCE_DIR}/include
    PUBLIC ${LLVM_INCLUDE_DIRS}
)
target_compile_definitions(solc_core PUBLIC ${LLVM_DEFINITIONS})

llvm_map_components_to_libnames(llvm_libs core support irreader bitwriter analysis passes transformutils)
target_link_libraries(solc_core PUBLIC ${llvm_libs})

add_executable(solidity-llvm main.cpp)
target_link_libraries(solidity-llvm PRIVATE solc_core cxxopts::cxxopts)
