NewTargetSource()
AddSourceFolder(INCLUDE RECURSE PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/public")
AddSourceFolder(RECURSE "${CMAKE_CURRENT_SOURCE_DIR}/private")
source_group(TREE ${PROJECT_SOURCE_DIR} FILES ${SourceFiles})

function(configure_library TARGET_NAME)
    set_target_properties(${TARGET_NAME} PROPERTIES FOLDER "OFileBackup")
    target_compile_features(${TARGET_NAME} PRIVATE cxx_std_20)
    set_target_properties(${TARGET_NAME} PROPERTIES CXX_STANDARD_REQUIRED ON)
    set_target_properties(${TARGET_NAME} PROPERTIES POSITION_INDEPENDENT_CODE ON)

    AddTargetInclude(${TARGET_NAME})

    add_library(${PROJECT_NAME}::${TARGET_NAME} ALIAS ${TARGET_NAME})

    if(NOT UTILPP_DISABLE_INSTALL)
        AddTargetInstall(${TARGET_NAME} ${PROJECT_NAME})
    endif()
endfunction()

set(TARGET_NAME libfilebackup_cexport)
add_library(${TARGET_NAME} SHARED ${SourceFiles})
target_link_libraries(${TARGET_NAME} PRIVATE OFileBackup::libfilebackup)
target_compile_definitions(${TARGET_NAME} PRIVATE -DLIB_FILEBACKUP_CEXPORT_API_EXPORTS)
configure_library(${TARGET_NAME})

set(TARGET_NAME libfilebackup_cexport_a)
add_library(${TARGET_NAME} STATIC ${SourceFiles})
target_link_libraries(${TARGET_NAME} PRIVATE OFileBackup::libfilebackup_a)
target_compile_definitions(${TARGET_NAME} PUBLIC -DLIB_FILEBACKUP_CEXPORT_API_NODLL)
configure_library(${TARGET_NAME})