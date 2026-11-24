string(RANDOM LENGTH 16 ALPHABET 0123456789abcdef id)
set(db "${BUILD_DIR}/openai-cli-${id}.sqlite")
set(question "Какой срок приёмки?")

function(check_cli expected pattern)
    execute_process(COMMAND "${CMAKE_COMMAND}" -E env --unset=OPENAI_API_KEY "${CLI}" ${ARGN}
                    RESULT_VARIABLE status OUTPUT_VARIABLE output ERROR_VARIABLE error ENCODING UTF-8)
    if(NOT "${status}" STREQUAL "${expected}" OR NOT "${output}${error}" MATCHES "${pattern}")
        message(FATAL_ERROR "CLI expected ${expected}, got ${status}\n${output}${error}")
    endif()
endfunction()

check_cli(0 "contract[.]txt" --db "${db}" "${DEMO_DIR}/contract.txt" "${question}")
check_cli(0 "Библиотека: LocalOnly\n1 [|] contract[.]txt [|] LocalOnly" --documents "${db}")
check_cli(1 "Внешняя обработка запрещена" --db "${db}" --openai --dev-env-key "${question}")
check_cli(0 "Библиотека: AllowRemote" --policy "${db}" library allow-remote)
check_cli(1 "Внешняя обработка запрещена" --db "${db}" --openai --dev-env-key "${question}")
check_cli(0 "Документ 1: AllowRemote" --policy "${db}" document 1 allow-remote)
check_cli(1 "Ключ OpenAI отсутствует" --db "${db}" --openai --model gpt-5.4-mini-2026-03-17
                                      --dev-env-key --credential-scope cli-test "${question}")
check_cli(0 "Документ 1: LocalOnly" --policy "${db}" document 1 local-only)
check_cli(1 "Внешняя обработка запрещена" --db "${db}" --openai --dev-env-key "${question}")
file(REMOVE "${db}")
