string(RANDOM LENGTH 16 ALPHABET 0123456789abcdef suffix)
set(test_dir "${BUILD_DIR}/cli-sqlite-${suffix}")
file(MAKE_DIRECTORY "${test_dir}")
file(COPY "${DEMO_DIR}/contract.txt" "${DEMO_DIR}/decisions.txt" DESTINATION "${test_dir}")
set(database "${test_dir}/library.sqlite")
set(contract "${test_dir}/contract.txt")
set(decisions "${test_dir}/decisions.txt")
set(question "Когда выполняется резервное копирование?")

execute_process(
    COMMAND "${CLI}" --db "${database}" "${contract}" "${decisions}" "${question}"
    RESULT_VARIABLE first_status OUTPUT_VARIABLE first_output ERROR_VARIABLE first_error
    ENCODING UTF8
)
file(REMOVE "${contract}" "${decisions}")
execute_process(
    COMMAND "${CLI}" --db "${database}" "${question}"
    RESULT_VARIABLE second_status OUTPUT_VARIABLE second_output ERROR_VARIABLE second_error
    ENCODING UTF8
)
file(REMOVE_RECURSE "${test_dir}")

if(NOT first_status STREQUAL "0" OR NOT second_status STREQUAL "0")
    message(FATAL_ERROR "CLI exits: ${first_status}, ${second_status}\n${first_error}${second_error}")
endif()
if(NOT first_output STREQUAL second_output OR
   NOT first_output MATCHES "Резервное копирование выполняется каждую пятницу[.]" OR
   NOT first_output MATCHES "decisions[.]txt, строка 4 [(]ID документа 2[)]")
    message(FATAL_ERROR "Expected the same stored decisions.txt citation twice:\n${first_output}\n${second_output}")
endif()
