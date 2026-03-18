NewTargetSource()
AddSourceFolder(INCLUDE RECURSE PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/public")
AddSourceFolder(RECURSE "${CMAKE_CURRENT_SOURCE_DIR}/private")
source_group(TREE ${PROJECT_SOURCE_DIR} FILES ${SourceFiles})

function(configure_library TARGET_NAME)
    set_target_properties(${TARGET_NAME} PROPERTIES VS_DEBUGGER_WORKING_DIRECTORY ${CMAKE_INSTALL_PREFIX}/${CMAKE_INSTALL_BINDIR})
    set_target_properties(${TARGET_NAME} PROPERTIES FOLDER "OFileBackup")
    target_compile_features(${TARGET_NAME} PRIVATE cxx_std_20)
    set_target_properties(${TARGET_NAME} PROPERTIES CXX_STANDARD_REQUIRED ON)

    target_link_libraries(${TARGET_NAME} PRIVATE cxxopts::cxxopts)
    target_link_libraries(${TARGET_NAME} PRIVATE UTILPP::simple_util_a)
    target_link_libraries(${TARGET_NAME} PRIVATE UTILPP::task_manager_a)
    target_link_libraries(${TARGET_NAME} PRIVATE OFileBackup::libfilebackup_a)
    target_link_libraries(${TARGET_NAME} PRIVATE OFileBackup::ofilebackup_a)

    AddTargetInclude(${TARGET_NAME})

    if(NOT UTILPP_DISABLE_INSTALL)
        AddTargetInstall(${TARGET_NAME} ${PROJECT_NAME})
    endif()
endfunction()

set(TARGET_NAME oBackupFolder)
add_executable(${TARGET_NAME} ${SourceFiles})
configure_library(${TARGET_NAME})
