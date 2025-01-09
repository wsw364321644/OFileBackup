NewTargetSource()
AddSourceFolder(INCLUDE RECURSE PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/public")
AddSourceFolder(RECURSE "${CMAKE_CURRENT_SOURCE_DIR}/private")
source_group( TREE ${PROJECT_SOURCE_DIR}  FILES ${SourceFiles})

function(configure_library TARGET_NAME)
    set_target_properties(${TARGET_NAME} PROPERTIES FOLDER "OFileBackup")
    target_compile_features(${TARGET_NAME} PRIVATE cxx_std_20)
    set_target_properties(${TARGET_NAME} PROPERTIES CXX_STANDARD_REQUIRED ON)
    set_target_properties(${TARGET_NAME} PROPERTIES POSITION_INDEPENDENT_CODE ON)
    
    target_link_libraries(${TARGET_NAME} PRIVATE libfilebackup)
    target_link_libraries(${TARGET_NAME} PRIVATE task_manager_a)
    target_link_libraries(${TARGET_NAME} PRIVATE cxxopts::cxxopts)


    AddTargetInclude(${TARGET_NAME})
    
    install(TARGETS ${TARGET_NAME}
        EXPORT ${TARGET_NAME}Targets
        LIBRARY DESTINATION lib
        ARCHIVE DESTINATION lib
        RUNTIME DESTINATION bin
        PUBLIC_HEADER DESTINATION include
    )
    
    install(EXPORT ${TARGET_NAME}Targets
        FILE ${TARGET_NAME}Targets.cmake
        NAMESPACE UTILPP::
        DESTINATION lib/cmake/${TARGET_NAME}
    )
endfunction()

set( TARGET_NAME ofilebackup)
add_executable (${TARGET_NAME}  ${SourceFiles} )
configure_library(${TARGET_NAME})

