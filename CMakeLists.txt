cmake_minimum_required(VERSION 3.21)
project(RagBot LANGUAGES C CXX)

option(BUILD_GUI "Build the Qt interface" OFF)
option(WITH_LOCAL_LLM "Build the local generation provider" OFF)
if(BUILD_GUI OR WITH_LOCAL_LLM)
    message(FATAL_ERROR "This prototype supports the CLI with MockProvider only.")
endif()

set(CMAKE_CXX_EXTENSIONS OFF)
if(MSVC)
    add_compile_options($<$<COMPILE_LANGUAGE:CXX>:/utf-8>
                        $<$<COMPILE_LANGUAGE:CXX>:/W4>)
endif()

include(FetchContent)
if(POLICY CMP0135)
    cmake_policy(SET CMP0135 NEW)
endif()
set(RAG_SQLITE_ARCHIVE "" CACHE FILEPATH "Path to the official SQLite amalgamation ZIP")
set(sqlite_url "https://www.sqlite.org/2026/sqlite-amalgamation-3540100.zip")
if(RAG_SQLITE_ARCHIVE)
    set(sqlite_url "${RAG_SQLITE_ARCHIVE}")
endif()
FetchContent_Declare(sqlite_amalgamation
    URL "${sqlite_url}"
    URL_HASH SHA3_256=92281c86e2957f9b9305890f9ac04204caa7b677cc868dc326d942689df28e30
    TLS_VERIFY ON
    TIMEOUT 60
)
FetchContent_MakeAvailable(sqlite_amalgamation)

add_library(rag_sqlite STATIC "${sqlite_amalgamation_SOURCE_DIR}/sqlite3.c")
target_include_directories(rag_sqlite SYSTEM PUBLIC "${sqlite_amalgamation_SOURCE_DIR}")
target_compile_definitions(rag_sqlite PRIVATE SQLITE_OMIT_LOAD_EXTENSION)

add_library(rag_domain INTERFACE)
target_include_directories(rag_domain INTERFACE include)
target_compile_features(rag_domain INTERFACE cxx_std_20)

add_library(rag_application src/application/services.cpp)
target_link_libraries(rag_application PUBLIC rag_domain)

add_library(rag_infrastructure
    src/infrastructure/txt_reader.cpp
    src/infrastructure/keyword_retriever.cpp
    src/infrastructure/sqlite_knowledge_store.cpp
    src/infrastructure/utf8.cpp
)
target_link_libraries(rag_infrastructure PUBLIC rag_application PRIVATE rag_sqlite)

add_executable(rag_cli apps/main.cpp src/presentation/cli.cpp)
target_link_libraries(rag_cli PRIVATE rag_application rag_infrastructure)

include(CTest)
if(BUILD_TESTING)
    add_executable(rag_tests tests/prototype_tests.cpp)
    target_link_libraries(rag_tests PRIVATE rag_infrastructure)
    foreach(case IN ITEMS answer empty_file empty_question no_match
                          unknown_citation altered_quote missing_file encodings second_document
                          duplicate_import failed_import_preserves_library wrong_document_citation
                          store_errors)
        add_test(NAME ${case}
                 COMMAND rag_tests ${case} "${CMAKE_CURRENT_SOURCE_DIR}/data/demo/contract.txt"
                         "${CMAKE_CURRENT_BINARY_DIR}")
    endforeach()
    add_executable(rag_sqlite_tests tests/sqlite_tests.cpp)
    target_link_libraries(rag_sqlite_tests PRIVATE rag_infrastructure rag_sqlite)
    foreach(case IN ITEMS reopen duplicate rollback invalid_database)
        add_test(NAME sqlite_${case}
                 COMMAND rag_sqlite_tests ${case} "${CMAKE_CURRENT_SOURCE_DIR}/data/demo"
                         "${CMAKE_CURRENT_BINARY_DIR}")
    endforeach()

    add_test(NAME cli_single_txt
             COMMAND rag_cli "${CMAKE_CURRENT_SOURCE_DIR}/data/demo/contract.txt"
                             "Какой срок приёмки?")
    set_tests_properties(cli_single_txt PROPERTIES
        PASS_REGULAR_EXPRESSION "contract[.]txt, строка 4 [(]ID документа 1[)]")
    add_test(NAME cli_multiple_txt
             COMMAND rag_cli "${CMAKE_CURRENT_SOURCE_DIR}/data/demo/contract.txt"
                             "${CMAKE_CURRENT_SOURCE_DIR}/data/demo/decisions.txt"
                             "Резервное копирование")
    set_tests_properties(cli_multiple_txt PROPERTIES
        PASS_REGULAR_EXPRESSION "decisions[.]txt, строка 4 [(]ID документа 2[)]")
    add_test(NAME cli_sqlite_persistence
             COMMAND "${CMAKE_COMMAND}" "-DCLI=$<TARGET_FILE:rag_cli>"
                     "-DDEMO_DIR=${CMAKE_CURRENT_SOURCE_DIR}/data/demo"
                     "-DBUILD_DIR=${CMAKE_CURRENT_BINARY_DIR}"
                     -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/cli_sqlite_persistence.cmake")
endif()
