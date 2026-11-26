cmake_minimum_required(VERSION 3.21)
project(RagBot LANGUAGES C CXX)

option(BUILD_GUI "Build the Qt interface" OFF)
option(WITH_LOCAL_LLM "Build the local generation provider" OFF)
if(BUILD_GUI)
    message(FATAL_ERROR "The Qt interface is pending.")
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

set(RAG_NLOHMANN_HEADER "" CACHE FILEPATH "Offline copy of the official nlohmann/json 3.12.0 single header")
if(RAG_NLOHMANN_HEADER)
    file(SHA256 "${RAG_NLOHMANN_HEADER}" nlohmann_header_hash)
    if(NOT nlohmann_header_hash STREQUAL "aaf127c04cb31c406e5b04a63f1ae89369fccde6d8fa7cdda1ed4f32dfc5de63")
        message(FATAL_ERROR "RAG_NLOHMANN_HEADER must match the official 3.12.0 json.hpp.")
    endif()
    set(nlohmann_include "${CMAKE_CURRENT_BINARY_DIR}/nlohmann-offline")
    configure_file("${RAG_NLOHMANN_HEADER}" "${nlohmann_include}/nlohmann/json.hpp" COPYONLY)
else()
    FetchContent_Declare(nlohmann_sources
        URL "https://github.com/nlohmann/json/releases/download/v3.12.0/json.tar.xz"
        URL_HASH SHA256=42f6e95cad6ec532fd372391373363b62a14af6d771056dbfc86160e6dfff7aa
        TLS_VERIFY ON
        TIMEOUT 60
        SOURCE_SUBDIR unused
    )
    FetchContent_MakeAvailable(nlohmann_sources)
    set(nlohmann_include "${nlohmann_sources_SOURCE_DIR}/include")
endif()
add_library(rag_json INTERFACE)
target_include_directories(rag_json SYSTEM INTERFACE "${nlohmann_include}")

add_library(rag_domain INTERFACE)
target_include_directories(rag_domain INTERFACE include)
target_compile_features(rag_domain INTERFACE cxx_std_20)

add_library(rag_application src/application/services.cpp)
target_link_libraries(rag_application PUBLIC rag_domain)

add_library(rag_infrastructure
    src/infrastructure/block_slices.cpp
    src/infrastructure/local_embeddings.cpp
    src/infrastructure/local_llama_provider.cpp
    src/infrastructure/generation_json.cpp
    src/infrastructure/cache_key.cpp
    src/infrastructure/txt_reader.cpp
    src/infrastructure/pdf_reader.cpp
    src/infrastructure/docx_reader.cpp
    src/infrastructure/local_file.cpp
    src/infrastructure/keyword_retriever.cpp
    src/infrastructure/remote_api_provider.cpp
    src/infrastructure/sqlite_knowledge_store.cpp
    src/infrastructure/sqlite_search.cpp
    src/infrastructure/utf8.cpp
    src/infrastructure/windows_openai.cpp
)
target_link_libraries(rag_infrastructure PUBLIC rag_application
    PRIVATE rag_sqlite rag_pdfium rag_miniz rag_pugixml rag_json bcrypt winhttp advapi32)

set(RAG_LLAMA_ARCHIVE "" CACHE FILEPATH "Local llama.cpp b6500 source archive for CPU inference")
if(WITH_LOCAL_LLM AND NOT RAG_LLAMA_ARCHIVE)
    message(FATAL_ERROR "WITH_LOCAL_LLM requires the pinned local RAG_LLAMA_ARCHIVE (b6500).")
endif()
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
    target_sources(rag_infrastructure PRIVATE src/infrastructure/llama_runtime.cpp)
    target_compile_definitions(rag_infrastructure PRIVATE RAG_LLAMA_EMBEDDINGS)
    if(WITH_LOCAL_LLM)
        target_compile_definitions(rag_infrastructure PRIVATE RAG_LOCAL_GENERATION)
    endif()
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
                          migration_v2 migration_v3 migration_v4 migration_v5 migration_v6 migration_v7 migration_v8 policy_revisions
                          cache_history cache_access_errors hybrid_search rrf long_blocks split_positions whole_terms)
        add_test(NAME sqlite_${case}
                 COMMAND rag_sqlite_tests ${case} "${CMAKE_CURRENT_SOURCE_DIR}/data/demo"
                         "${CMAKE_CURRENT_BINARY_DIR}")
    endforeach()

    add_executable(rag_openai_tests tests/openai_tests.cpp)
    target_link_libraries(rag_openai_tests PRIVATE rag_infrastructure rag_json)
    foreach(case IN ITEMS request errors budget)
        add_test(NAME openai_${case} COMMAND rag_openai_tests ${case})
    endforeach()
    foreach(case IN ITEMS restart policy_race)
        add_test(NAME openai_${case}
                 COMMAND "${CMAKE_COMMAND}" "-DTESTER=$<TARGET_FILE:rag_openai_tests>"
                         "-DBUILD_DIR=${CMAKE_CURRENT_BINARY_DIR}" "-DCASE=${case}"
                         -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/openai_database.cmake")
    endforeach()
    add_test(NAME cli_openai_policy
             COMMAND "${CMAKE_COMMAND}" "-DCLI=$<TARGET_FILE:rag_cli>"
                     "-DDEMO_DIR=${CMAKE_CURRENT_SOURCE_DIR}/data/demo" "-DBUILD_DIR=${CMAKE_CURRENT_BINARY_DIR}"
                     -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/cli_openai_policy.cmake")

    set(RAG_TEST_EMBEDDING_MODEL "" CACHE FILEPATH "Local Qwen GGUF for the optional integration test")
    set(RAG_TEST_GENERATION_MODEL "" CACHE FILEPATH "Local Qwen3-4B Q4_K_M for offline generation tests")
    add_executable(rag_local_cli_probe apps/main.cpp src/presentation/cli.cpp tests/local_cli_probe.cpp)
    target_link_libraries(rag_local_cli_probe PRIVATE rag_infrastructure)
    add_dependencies(rag_local_cli_probe rag_cli)
    set(local_cli_cases files)
    if(WITH_LOCAL_LLM AND RAG_TEST_GENERATION_MODEL AND RAG_TEST_EMBEDDING_MODEL)
        list(APPEND local_cli_cases offline)
        add_executable(rag_local_tests tests/local_llama_tests.cpp)
        target_link_libraries(rag_local_tests PRIVATE rag_infrastructure)
        add_test(NAME local_real_generation COMMAND rag_local_tests "${CMAKE_CURRENT_SOURCE_DIR}/data/demo"
                 "${RAG_TEST_EMBEDDING_MODEL}" "${RAG_TEST_GENERATION_MODEL}")
    endif()
    foreach(case IN LISTS local_cli_cases)
        add_test(NAME cli_local_${case}
                 COMMAND "${CMAKE_COMMAND}" "-DCLI=$<TARGET_FILE:rag_local_cli_probe>"
                         "-DCASE=${case}" "-DGENERATOR=${WITH_LOCAL_LLM}"
                         "-DMODEL=${RAG_TEST_GENERATION_MODEL}" "-DEMBEDDINGS=${RAG_TEST_EMBEDDING_MODEL}"
                         "-DDEMO_DIR=${CMAKE_CURRENT_SOURCE_DIR}/data/demo" "-DBUILD_DIR=${CMAKE_CURRENT_BINARY_DIR}"
                         -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/cli_local.cmake")
    endforeach()
    if(RAG_TEST_EMBEDDING_MODEL AND RAG_LLAMA_ARCHIVE)
        add_test(NAME sqlite_real_chunks COMMAND rag_sqlite_tests real_chunks
                 "${CMAKE_CURRENT_SOURCE_DIR}/data/demo" "${CMAKE_CURRENT_BINARY_DIR}" "${RAG_TEST_EMBEDDING_MODEL}")
    endif()

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
