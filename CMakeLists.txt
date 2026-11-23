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
target_compile_definitions(rag_sqlite PRIVATE SQLITE_OMIT_LOAD_EXTENSION SQLITE_ENABLE_FTS5)

if(NOT MSVC OR NOT CMAKE_CXX_COMPILER_ARCHITECTURE_ID STREQUAL "x64")
    message(FATAL_ERROR "The pinned PDFium binary requires MSVC targeting Windows x64.")
endif()
set(RAG_PDFIUM_ARCHIVE "" CACHE FILEPATH "Path to PDFium 160.0.8165.0 for Windows x64")
set(pdfium_url "https://github.com/bblanchon/pdfium-binaries/releases/download/chromium%2F8165/pdfium-win-x64.tgz")
if(RAG_PDFIUM_ARCHIVE)
    set(pdfium_url "${RAG_PDFIUM_ARCHIVE}")
endif()
FetchContent_Declare(pdfium_binary
    URL "${pdfium_url}"
    URL_HASH SHA256=ca7338ae7779b8b1b82f4580305c4d762d214f8df1d54b47647fe3cc75989ded
    TLS_VERIFY ON
    TIMEOUT 60
)
FetchContent_MakeAvailable(pdfium_binary)

add_library(rag_pdfium SHARED IMPORTED)
set_target_properties(rag_pdfium PROPERTIES
    IMPORTED_LOCATION "${pdfium_binary_SOURCE_DIR}/bin/pdfium.dll"
    IMPORTED_IMPLIB "${pdfium_binary_SOURCE_DIR}/lib/pdfium.dll.lib"
    INTERFACE_INCLUDE_DIRECTORIES "${pdfium_binary_SOURCE_DIR}/include"
)

set(RAG_MINIZ_ARCHIVE "" CACHE FILEPATH "Path to miniz-3.1.2.zip")
set(RAG_PUGIXML_ARCHIVE "" CACHE FILEPATH "Path to pugixml-1.16.tar.gz")
set(miniz_url "https://github.com/richgel999/miniz/releases/download/3.1.2/miniz-3.1.2.zip")
set(pugixml_url "https://github.com/zeux/pugixml/releases/download/v1.16/pugixml-1.16.tar.gz")
if(RAG_MINIZ_ARCHIVE)
    set(miniz_url "${RAG_MINIZ_ARCHIVE}")
endif()
if(RAG_PUGIXML_ARCHIVE)
    set(pugixml_url "${RAG_PUGIXML_ARCHIVE}")
endif()
FetchContent_Declare(miniz_sources
    URL "${miniz_url}"
    URL_HASH SHA256=f0446d863f9c19926ad9483c523fdc42e42b8d4a6a431d27e09d49c79a140d9a
    TLS_VERIFY ON
    TIMEOUT 60
)
FetchContent_Declare(pugixml_sources
    URL "${pugixml_url}"
    URL_HASH SHA256=4cee1ca4aad395170f4c7a07824f3bdd41f28316c6e1e1090a1425b278ec0b4b
    TLS_VERIFY ON
    TIMEOUT 60
    SOURCE_SUBDIR unused
)
FetchContent_MakeAvailable(miniz_sources pugixml_sources)

add_library(rag_miniz STATIC "${miniz_sources_SOURCE_DIR}/miniz.c")
target_include_directories(rag_miniz SYSTEM PUBLIC "${miniz_sources_SOURCE_DIR}")
target_compile_definitions(rag_miniz PUBLIC MINIZ_NO_STDIO MINIZ_NO_ARCHIVE_WRITING_APIS)
add_library(rag_pugixml STATIC "${pugixml_sources_SOURCE_DIR}/src/pugixml.cpp")
target_include_directories(rag_pugixml SYSTEM PUBLIC "${pugixml_sources_SOURCE_DIR}/src")
target_compile_definitions(rag_pugixml PUBLIC PUGIXML_NO_XPATH)

add_library(rag_domain INTERFACE)
target_include_directories(rag_domain INTERFACE include)
target_compile_features(rag_domain INTERFACE cxx_std_20)

add_library(rag_application src/application/services.cpp)
target_link_libraries(rag_application PUBLIC rag_domain)

add_library(rag_infrastructure
    src/infrastructure/local_embeddings.cpp
    src/infrastructure/cache_key.cpp
    src/infrastructure/txt_reader.cpp
    src/infrastructure/pdf_reader.cpp
    src/infrastructure/docx_reader.cpp
    src/infrastructure/local_file.cpp
    src/infrastructure/keyword_retriever.cpp
    src/infrastructure/sqlite_knowledge_store.cpp
    src/infrastructure/sqlite_search.cpp
    src/infrastructure/utf8.cpp
)
target_link_libraries(rag_infrastructure PUBLIC rag_application
    PRIVATE rag_sqlite rag_pdfium rag_miniz rag_pugixml bcrypt)

set(RAG_LLAMA_ARCHIVE "" CACHE FILEPATH "Local llama.cpp b6500 source archive for CPU embeddings")
if(RAG_LLAMA_ARCHIVE)
    if(NOT EXISTS "${RAG_LLAMA_ARCHIVE}")
        message(FATAL_ERROR "RAG_LLAMA_ARCHIVE must point to the local b6500 source archive.")
    endif()
    set(BUILD_SHARED_LIBS OFF)
    set(LLAMA_BUILD_COMMON OFF)
    set(LLAMA_CURL OFF)
    set(GGML_NATIVE OFF)
    set(GGML_OPENMP OFF)
    FetchContent_Declare(llama_runtime URL "${RAG_LLAMA_ARCHIVE}"
        URL_HASH SHA256=f2a188b194f25178460eecda7a7619b34d0b53421588faa2e6f0287a96674f28)
    FetchContent_MakeAvailable(llama_runtime)
    target_link_libraries(rag_infrastructure PRIVATE llama)
    target_compile_definitions(rag_infrastructure PRIVATE RAG_LLAMA_EMBEDDINGS)
endif()

add_executable(rag_cli apps/main.cpp src/presentation/cli.cpp)
target_link_libraries(rag_cli PRIVATE rag_application rag_infrastructure)
add_custom_command(TARGET rag_cli POST_BUILD
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different
            "$<TARGET_FILE:rag_pdfium>" "$<TARGET_FILE_DIR:rag_cli>"
)

include(CTest)
if(BUILD_TESTING)
    add_executable(rag_tests tests/prototype_tests.cpp)
    target_link_libraries(rag_tests PRIVATE rag_infrastructure)
    foreach(case IN ITEMS answer empty_file empty_question no_match
                          unknown_citation altered_quote missing_file encodings second_document
                          duplicate_import failed_import_preserves_library wrong_document_citation
                          store_errors paged_citation provider_access)
        add_test(NAME ${case}
                 COMMAND rag_tests ${case} "${CMAKE_CURRENT_SOURCE_DIR}/data/demo/contract.txt"
                         "${CMAKE_CURRENT_BINARY_DIR}")
    endforeach()
    add_executable(rag_sqlite_tests tests/sqlite_tests.cpp)
    target_link_libraries(rag_sqlite_tests PRIVATE rag_infrastructure rag_sqlite)
    foreach(case IN ITEMS reopen duplicate rollback invalid_database migration migration_failure
                          migration_v2 migration_v3 migration_v4 migration_v5 policy_revisions cache_history cache_access_errors hybrid_search rrf)
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
    add_test(NAME cli_history
             COMMAND "${CMAKE_COMMAND}" "-DCLI=$<TARGET_FILE:rag_cli>"
                     "-DDEMO_DIR=${CMAKE_CURRENT_SOURCE_DIR}/data/demo"
                     "-DBUILD_DIR=${CMAKE_CURRENT_BINARY_DIR}"
                     -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/cli_history.cmake")
    foreach(case IN ITEMS persistence partial errors)
        add_test(NAME cli_pdf_${case}
                 COMMAND "${CMAKE_COMMAND}" "-DCLI=$<TARGET_FILE:rag_cli>"
                         "-DCASE=${case}" "-DDEMO_DIR=${CMAKE_CURRENT_SOURCE_DIR}/data/demo"
                         "-DFIXTURES=${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures"
                         "-DBUILD_DIR=${CMAKE_CURRENT_BINARY_DIR}"
                         -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/cli_pdf.cmake")
    endforeach()
    foreach(case IN ITEMS mixed_persistence parts errors limits visible_text)
        add_test(NAME cli_docx_${case}
                 COMMAND "${CMAKE_COMMAND}" "-DCLI=$<TARGET_FILE:rag_cli>"
                         "-DCASE=${case}" "-DDEMO_DIR=${CMAKE_CURRENT_SOURCE_DIR}/data/demo"
                         "-DBUILD_DIR=${CMAKE_CURRENT_BINARY_DIR}"
                         -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/cli_docx.cmake")
    endforeach()
endif()
