cmake_minimum_required(VERSION 3.21)
project(RagBot LANGUAGES CXX)

option(BUILD_GUI "Build the Qt interface" OFF)
option(WITH_LOCAL_LLM "Build the local generation provider" OFF)
if(BUILD_GUI OR WITH_LOCAL_LLM)
    message(FATAL_ERROR "This prototype supports the CLI with MockProvider only.")
endif()

set(CMAKE_CXX_EXTENSIONS OFF)
if(MSVC)
    add_compile_options(/utf-8 /W4)
endif()

add_library(rag_domain INTERFACE)
target_include_directories(rag_domain INTERFACE include)
target_compile_features(rag_domain INTERFACE cxx_std_20)

add_library(rag_application src/application/services.cpp)
target_link_libraries(rag_application PUBLIC rag_domain)

add_library(rag_infrastructure
    src/infrastructure/txt_reader.cpp
    src/infrastructure/keyword_retriever.cpp
    src/infrastructure/utf8.cpp
)
target_link_libraries(rag_infrastructure PUBLIC rag_application)

add_executable(rag_cli apps/main.cpp src/presentation/cli.cpp)
target_link_libraries(rag_cli PRIVATE rag_application rag_infrastructure)

include(CTest)
if(BUILD_TESTING)
    add_executable(rag_tests tests/prototype_tests.cpp)
    target_link_libraries(rag_tests PRIVATE rag_infrastructure)
    foreach(case IN ITEMS answer empty_file empty_question no_match
                          unknown_citation altered_quote missing_file encodings)
        add_test(NAME ${case}
                 COMMAND rag_tests ${case} "${CMAKE_CURRENT_SOURCE_DIR}/data/demo/contract.txt"
                         "${CMAKE_CURRENT_BINARY_DIR}")
    endforeach()
endif()
