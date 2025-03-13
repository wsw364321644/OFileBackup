NewTargetSource()
AddSourceFolder(INCLUDE RECURSE PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/public")
AddSourceFolder(RECURSE "${CMAKE_CURRENT_SOURCE_DIR}/private")
source_group( TREE ${PROJECT_SOURCE_DIR}  FILES ${SourceFiles})

function(configure_library TARGET_NAME)
    set_target_properties(${TARGET_NAME} PROPERTIES FOLDER "OFileBackup")
    target_compile_features(${TARGET_NAME} PRIVATE cxx_std_23)
    set_target_properties(${TARGET_NAME} PROPERTIES CXX_STANDARD_REQUIRED ON)
    set_target_properties(${TARGET_NAME} PROPERTIES POSITION_INDEPENDENT_CODE ON)
    
    target_link_libraries(${TARGET_NAME} PUBLIC simple_utilpp_header)
    target_link_libraries(${TARGET_NAME} PRIVATE simple_utilpp_a)
    target_link_libraries(${TARGET_NAME} PRIVATE rollinghashcpp)
    target_link_libraries(${TARGET_NAME} PRIVATE xxHash::xxhash)
    target_link_libraries(${TARGET_NAME} PRIVATE MbedTLS::mbedcrypto)
    target_link_libraries(${TARGET_NAME} PRIVATE zstd::libzstd_static)
    target_link_libraries(${TARGET_NAME} PRIVATE nlohmann_json::nlohmann_json)
    target_link_libraries(${TARGET_NAME} PRIVATE Boost::headers)

    AddTargetInclude(${TARGET_NAME})
    
    install(TARGETS ${TARGET_NAME}
        EXPORT ${TARGET_NAME}Targets
        LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
        ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
        RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
        PUBLIC_HEADER DESTINATION include
    )
    
    install(EXPORT ${TARGET_NAME}Targets
        FILE ${TARGET_NAME}Targets.cmake
        NAMESPACE OFileBackup::
        DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/${TARGET_NAME}
    )
endfunction()

set( TARGET_NAME libfilebackup)
add_library (${TARGET_NAME} SHARED  ${SourceFiles} )
target_compile_definitions(${TARGET_NAME} PRIVATE -DLIB_FILEBACKUP_API_EXPORTS)
configure_library(${TARGET_NAME})

set( TARGET_NAME libfilebackup_a)
add_library (${TARGET_NAME} STATIC  ${SourceFiles} )
target_compile_definitions(${TARGET_NAME} PUBLIC -DLIB_FILEBACKUP_API_NODLL)
configure_library(${TARGET_NAME})