cmake_minimum_required(VERSION 3.21)

string(RANDOM LENGTH 16 ALPHABET 0123456789abcdef suffix)
set(work "${BUILD_DIR}/cli-docx-${suffix}")
file(MAKE_DIRECTORY "${work}")
set(demo "${DEMO_DIR}/backup-register.docx")
set(database "${work}/library.sqlite")
set(body_quote "Контрольный журнал Вега закрывается по пятницам.")
set(cell_quote "Снимок Тритон хранится четырнадцать дней.")
set(word "http://schemas.openxmlformats.org/wordprocessingml/2006/main")
set(package_rels "http://schemas.openxmlformats.org/package/2006/relationships")
set(office_rels "http://schemas.openxmlformats.org/officeDocument/2006/relationships")

function(run result)
    execute_process(COMMAND "${CLI}" ${ARGN}
        RESULT_VARIABLE status OUTPUT_VARIABLE output ERROR_VARIABLE errors
        OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_STRIP_TRAILING_WHITESPACE ENCODING UTF8)
    foreach(field IN ITEMS status output errors)
        set(${result}_${field} "${${field}}" PARENT_SCOPE)
    endforeach()
endfunction()

function(require_citation result quote source)
    set(expected "Найденный фрагмент (тестовый провайдер)\n«${quote}»\nИсточник: ${source}")
    string(REPLACE "\r\n" "\n" actual "${${result}_output}")
    if(NOT actual STREQUAL expected)
        message(FATAL_ERROR "Expected [${expected}], received [${actual}]\n${${result}_errors}")
    endif()
endfunction()

function(make_package)
    file(MAKE_DIRECTORY "${work}/package/_rels" "${work}/package/word/_rels")
    file(WRITE "${work}/package/[Content_Types].xml"
        "<Types xmlns='http://schemas.openxmlformats.org/package/2006/content-types'/>")
    file(WRITE "${work}/package/_rels/.rels"
        "<RelationshipList:Relationships xmlns:RelationshipList='${package_rels}'>
           <RelationshipList:Relationship Id='main' Type='${office_rels}/officeDocument' Target='/word/main.xml'/>
         </RelationshipList:Relationships>")
endfunction()

function(pack output)
    execute_process(COMMAND "${CMAKE_COMMAND}" -E tar cf "${output}" --format=zip
            "[Content_Types].xml" "_rels" "word"
        WORKING_DIRECTORY "${work}/package" RESULT_VARIABLE status)
    if(NOT status STREQUAL "0")
        message(FATAL_ERROR "Could not create DOCX fixture")
    endif()
endfunction()

if(CASE STREQUAL "mixed_persistence")
    file(COPY "${DEMO_DIR}/contract.txt" "${DEMO_DIR}/backup-protocol.pdf" "${demo}" DESTINATION "${work}")
    set(files "${work}/contract.txt" "${work}/backup-protocol.pdf" "${work}/backup-register.docx")
    set(questions "срок приёмки" "Orion snapshot" "Вега" "Тритон")
    set(quotes "Срок приёмки результата составляет десять рабочих дней."
               "Orion snapshot runs every Friday." "${body_quote}" "${cell_quote}")
    set(sources "contract.txt, строка 4 (ID документа 1)"
                "backup-protocol.pdf, страница 2 (ID документа 2)"
                "backup-register.docx, часть word/document.xml, абзац 2 (ID документа 3)"
                "backup-register.docx, часть word/document.xml, таблица 1, строка 2, ячейка 2 (ID документа 3)")
    foreach(index RANGE 0 3)
        list(GET questions ${index} question)
        list(GET quotes ${index} quote)
        list(GET sources ${index} source)
        run(before_${index} --db "${database}" ${files} "${question}")
        if(NOT before_${index}_status STREQUAL "0" OR NOT before_${index}_errors STREQUAL "")
            message(FATAL_ERROR "Import failed.\n${before_${index}_errors}")
        endif()
        require_citation(before_${index} "${quote}" "${source}")
    endforeach()
    file(REMOVE ${files})
    foreach(index RANGE 0 3)
        list(GET questions ${index} question)
        run(after --db "${database}" "${question}")
        if(NOT after_status STREQUAL "0" OR NOT after_errors STREQUAL "" OR
           NOT after_output STREQUAL "Ответ из кэша.\n${before_${index}_output}")
            message(FATAL_ERROR "Citation changed after reopening.\n${after_output}\n${after_errors}")
        endif()
    endforeach()
elseif(CASE STREQUAL "parts")
    set(questions "Лира" "Феникс" "Сириус")
    set(parts "header1" "footer1" "footnotes")
    set(quotes "Контур Лира" "Отметка Феникс" " Примечание Сириус относится к журналу.")
    foreach(index RANGE 0 2)
        list(GET questions ${index} question)
        list(GET parts ${index} part)
        list(GET quotes ${index} quote)
        run(answer "${demo}" "${question}")
        if(NOT answer_status STREQUAL "0" OR NOT answer_errors STREQUAL "")
            message(FATAL_ERROR "Could not read ${part}.\n${answer_errors}")
        endif()
        require_citation(answer "${quote}" "backup-register.docx, часть word/${part}.xml, абзац 1 (ID документа 1)")
    endforeach()
elseif(CASE STREQUAL "errors")
    make_package()
    file(WRITE "${work}/package/word/main.xml"
        "<w:document xmlns:w='${word}'><w:body><w:p><w:r><w:t>Canaryonly.</w:t></w:r></w:p></w:body></w:document>")
    file(WRITE "${work}/package/word/_rels/main.xml.rels"
        "<Relationships xmlns='${package_rels}'>
           <Relationship Id='header' Type='${office_rels}/header' Target='header.xml'/>
         </Relationships>")
    file(WRITE "${work}/package/word/header.xml" "<w:hdr xmlns:w='${word}'><w:p></w:hdr>")
    file(WRITE "${work}/bad-zip.docx" "This is no ZIP archive")
    pack("${work}/bad-header.docx")

    run(old --db "${database}" "${DEMO_DIR}/contract.txt" "срок приёмки")
    if(NOT old_status STREQUAL "0")
        message(FATAL_ERROR "Could not save the existing TXT.\n${old_errors}")
    endif()
    run(batch --db "${database}" "${work}/bad-zip.docx" "${work}/bad-header.docx"
        "${DEMO_DIR}/backup-protocol.pdf" "Orion snapshot")
    if(NOT batch_status STREQUAL "1" OR NOT batch_errors MATCHES "bad-zip[.]docx: DOCX повреждён" OR
       NOT batch_errors MATCHES "bad-header[.]docx: DOCX повреждён")
        message(FATAL_ERROR "Expected separate errors for bad ZIP and bad XML.\n${batch_errors}")
    endif()
    require_citation(batch "Orion snapshot runs every Friday." "backup-protocol.pdf, страница 2 (ID документа 2)")
    run(previous --db "${database}" "срок приёмки")
    run(canary --db "${database}" "Canaryonly")
    if(NOT previous_status STREQUAL "0" OR NOT previous_output STREQUAL old_output OR
       NOT canary_status STREQUAL "0" OR NOT canary_output MATCHES "не найдено подтверждения")
        message(FATAL_ERROR "A failed DOCX import changed the existing library.")
    endif()
elseif(CASE STREQUAL "limits")
    make_package()
    string(REPEAT "x" 8388609 oversized)
    file(WRITE "${work}/package/word/main.xml"
        "<w:document xmlns:w='${word}'><w:body><w:p><w:r><w:t>${oversized}</w:t></w:r></w:p></w:body></w:document>")
    pack("${work}/large-xml.docx")
    string(REPEAT "<w:ins>" 70 open)
    string(REPEAT "</w:ins>" 70 close)
    file(WRITE "${work}/package/word/main.xml"
        "<w:document xmlns:w='${word}'><w:body>${open}${close}</w:body></w:document>")
    pack("${work}/deep-xml.docx")
    run(result "${work}/large-xml.docx" "${work}/deep-xml.docx" "sample")
    if(NOT result_status STREQUAL "1" OR NOT result_output STREQUAL "" OR
       NOT result_errors MATCHES "large-xml[.]docx: DOCX превышает" OR
       NOT result_errors MATCHES "deep-xml[.]docx: DOCX превышает")
        message(FATAL_ERROR "Expected DOCX structure limits.\n${result_output}\n${result_errors}")
    endif()
elseif(CASE STREQUAL "visible_text")
    make_package()
    file(WRITE "${work}/package/word/main.xml"
        "<q:document xmlns:q='${word}'><q:body>
           <q:p><q:r><q:t>Visible only.</q:t></q:r>
             <q:del><q:r><q:delText>Deletedcanary.</q:delText></q:r></q:del>
             <q:moveFrom><q:r><q:t>Movedcanary.</q:t></q:r></q:moveFrom>
           </q:p><q:altChunk/><q:tbl>
             <q:tr><q:trPr><q:del/></q:trPr><q:tc><q:p><q:r><q:t>Rowcanary.</q:t></q:r></q:p></q:tc></q:tr>
             <q:tr><q:tc><q:tcPr><q:cellDel/></q:tcPr><q:p><q:r><q:t>Cellcanary.</q:t></q:r></q:p></q:tc></q:tr>
           </q:tbl>
         </q:body></q:document>")
    file(WRITE "${work}/package/word/_rels/main.xml.rels"
        "<Relationships xmlns='${package_rels}'>
           <Relationship Id='external' Type='${office_rels}/header' TargetMode='External' Target='external.xml'/>
         </Relationships>")
    file(WRITE "${work}/package/word/external.xml"
        "<w:hdr xmlns:w='${word}'><w:p><w:r><w:t>Externalcanary.</w:t></w:r></w:p></w:hdr>")
    pack("${work}/visible.docx")
    run(visible "${work}/visible.docx" "Visible")
    if(NOT visible_status STREQUAL "0" OR NOT visible_errors MATCHES "DOCX извлечён частично")
        message(FATAL_ERROR "Expected usable text and a warning.\n${visible_errors}")
    endif()
    require_citation(visible "Visible only." "visible.docx, часть word/main.xml, абзац 1 (ID документа 1)")
    run(hidden "${work}/visible.docx" "Deletedcanary Movedcanary Rowcanary Cellcanary Externalcanary")
    if(NOT hidden_status STREQUAL "0" OR NOT hidden_output MATCHES "не найдено подтверждения")
        message(FATAL_ERROR "Deleted, moved or external text was indexed.\n${hidden_output}")
    endif()
else()
    message(FATAL_ERROR "Unknown DOCX test case ${CASE}")
endif()

file(REMOVE_RECURSE "${work}")
