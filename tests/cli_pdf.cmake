string(RANDOM LENGTH 16 ALPHABET 0123456789abcdef suffix)
set(test_dir "${BUILD_DIR}/cli-pdf-${suffix}")
file(MAKE_DIRECTORY "${test_dir}")

if(CASE STREQUAL "persistence")
    set(pdf "${test_dir}/backup-protocol.PDF")
    set(database "${test_dir}/library.sqlite")
    file(COPY_FILE "${DEMO_DIR}/backup-protocol.pdf" "${pdf}")
    execute_process(
        COMMAND "${CLI}" --db "${database}" "${DEMO_DIR}/contract.txt" "${pdf}" "Orion snapshot"
        RESULT_VARIABLE status OUTPUT_VARIABLE first ERROR_VARIABLE errors ENCODING UTF8
    )
    file(REMOVE "${pdf}")
    execute_process(
        COMMAND "${CLI}" --db "${database}" "Orion snapshot"
        RESULT_VARIABLE reopened_status OUTPUT_VARIABLE second ERROR_VARIABLE reopened_errors ENCODING UTF8
    )
    file(REMOVE_RECURSE "${test_dir}")
    if(NOT status STREQUAL "0" OR NOT reopened_status STREQUAL "0" OR
       NOT errors STREQUAL "" OR NOT reopened_errors STREQUAL "" OR NOT second STREQUAL "Ответ из кэша.\n${first}" OR
       NOT second MATCHES "Orion snapshot runs every Friday[.]" OR
       NOT second MATCHES "backup-protocol[.]PDF, страница 2 [(]ID документа 2[)]")
        message(FATAL_ERROR "Expected the same page 2 citation before and after reopening.\n${first}${errors}\n${second}${reopened_errors}")
    endif()
elseif(CASE STREQUAL "partial")
    execute_process(
        COMMAND "${CLI}" "${FIXTURES}/partial-text.pdf" "Nebula snapshot"
        RESULT_VARIABLE status OUTPUT_VARIABLE output ERROR_VARIABLE errors ENCODING UTF8
    )
    file(REMOVE_RECURSE "${test_dir}")
    if(NOT status STREQUAL "0" OR NOT errors MATCHES "страницы без текста — 1, 3[.]" OR
       NOT output MATCHES "partial-text[.]pdf, страница 2 [(]ID документа 1[)]" OR
       NOT output MATCHES "Nebula snapshot is available[.]")
        message(FATAL_ERROR "Expected two skipped pages and a citation from the text page.\n${output}${errors}")
    endif()
elseif(CASE STREQUAL "errors")
    file(WRITE "${test_dir}/broken.pdf" "%PDF-1.7\nThis document has no valid PDF structure.")
    execute_process(
        COMMAND "${CLI}" "${FIXTURES}/without-text.pdf" "${FIXTURES}/encrypted.pdf"
                "${test_dir}/broken.pdf" "${DEMO_DIR}/contract.txt" "срок приёмки"
        RESULT_VARIABLE status OUTPUT_VARIABLE output ERROR_VARIABLE errors ENCODING UTF8
    )
    file(REMOVE_RECURSE "${test_dir}")
    if(NOT status STREQUAL "1" OR
       NOT errors MATCHES "without-text[.]pdf: В PDF нет текстового слоя" OR
       NOT errors MATCHES "encrypted[.]pdf: PDF зашифрован" OR
       NOT errors MATCHES "broken[.]pdf: PDF повреждён" OR
       NOT output MATCHES "contract[.]txt, строка 4 [(]ID документа 1[)]")
        message(FATAL_ERROR "Expected three specific errors and a TXT citation from the successful import.\n${output}${errors}")
    endif()
else()
    message(FATAL_ERROR "Unknown PDF test case ${CASE}")
endif()
