cmake_minimum_required(VERSION 3.24)
include(FetchContent)

set_property(GLOBAL PROPERTY USE_FOLDERS ON)
set(FETCHCONTENT_QUIET FALSE)
set(MY_PRODUCT_NUMBER 1)
set(MY_PRODUCT_VERSION 0)
set(MY_BUILD_NUMBER 0)
set(VERSION "${MY_PRODUCT_NUMBER}.${MY_PRODUCT_VERSION}.${MY_BUILD_NUMBER}")

if(CMAKE_SIZEOF_VOID_P EQUAL 8)
  set(_bin_suffix 64)
else()
  set(_bin_suffix 32)
endif()

set(OCmakeUtil_TAG 7cec5fa3e9ca466f7af22b51d5555b287202e179)
set(UTILPP_TAG ada546cc3793a236316bdb5d31dd51945f9afe96)

project(OFileBackup VERSION ${VERSION})
enable_language(CXX ASM C)

if(WIN32)
  option(OFB_STATIC_CRT "Set to ON to build with static CRT on Windows (/MT)." OFF)
endif()

option(OFB_ONLY_LIB "Set to ON to only contain libfilebackup." OFF)
option(OFB_DISABLE_INSTALL "Disable install config." OFF)
option(OFB_BUILD_TEST "Set to ON to contain test target." OFF)
option(OFB_GIT_SSH "Set to ON to git port 22." OFF)

if(OFB_GIT_SSH)
  set(SSH SSH)
else()
  unset(SSH)
endif()

set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

if(PROJECT_IS_TOP_LEVEL)
  if(CMAKE_INSTALL_PREFIX_INITIALIZED_TO_DEFAULT)
    set(CMAKE_INSTALL_PREFIX
      "${CMAKE_BINARY_DIR}/rundir"
      CACHE STRING "Directory to install after building" FORCE)
  endif()

  if(OFB_STATIC_CRT)
    if(MSVC)
      set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
      add_definitions(-D_CRT_SECURE_NO_WARNINGS)
    endif()

    set(CMAKE_C_FLAGS_RELEASE "${CMAKE_C_FLAGS_RELEASE} /MT")
    set(CMAKE_C_FLAGS_DEBUG "${CMAKE_C_FLAGS_DEBUG} /MTd")
    set(STATIC_CRT STATIC_CRT)
  else()
    if(MSVC)
      set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")
    endif()

    set(CMAKE_C_FLAGS_RELEASE "${CMAKE_C_FLAGS_RELEASE} /MD")
    set(CMAKE_C_FLAGS_DEBUG "${CMAKE_C_FLAGS_DEBUG} /MDd")
    unset(STATIC_CRT)
  endif()
endif()

FetchContent_Declare(
  OCmakeUtil
  GIT_REPOSITORY https://github.com/wsw364321644/OCmakeUtil.git
  GIT_TAG ${OCmakeUtil_TAG}
  GIT_SUBMODULES_RECURSE true
)
FetchContent_MakeAvailable(OCmakeUtil)
include(ProjectImport)
include(TargetImport)

ImportProject(simdjson ${STATIC_CRT} ${SSH} TAG v4.2.4)
ImportProject(absl ${STATIC_CRT} STATIC ${SSH} TAG 20260107.1)
ImportProject(libuv ${STATIC_CRT} ${SSH} FIND TAG v1.52.0)
ImportProject(zstd ${STATIC_CRT} ${SSH} FIND TAG v1.5.7)
ImportProject(xxHash STATIC ${STATIC_CRT} ${SSH} FIND TAG v0.8.3)
ImportProject(concurrentqueue ${SSH} TAG master)
ImportProject(Glob ${SSH} TAG master)
ImportProject(taskflow ${SSH} TAG v4.0.0)

set(UTILPP_WITH_STATIC_LIBUV OFF CACHE BOOL "" FORCE)
set(UTILPP_STATIC_CRT ${OFB_STATIC_CRT} CACHE BOOL "" FORCE)
set(UTILPP_GIT_SSH ${OFB_GIT_SSH} CACHE BOOL "" FORCE)
FetchContent_Declare(
  utilpp
  GIT_REPOSITORY https://github.com/wsw364321644/utilpp.git
  GIT_TAG ${UTILPP_TAG}
)
FetchContent_MakeAvailable(utilpp)

if(OFB_BUILD_TEST)
  FetchContent_Declare(
    Catch2
    GIT_REPOSITORY https://github.com/catchorg/Catch2.git
    GIT_TAG v3.13.0
  )
  FetchContent_MakeAvailable(Catch2)
endif()

if(NOT OFB_ONLY_LIB)
  ImportProject(cxxopts TAG v3.3.1)

  add_subdirectory(src/BackupFolder)
  add_subdirectory(src/CompareManifest)
  add_subdirectory(src/RecoverFolder)
endif()

add_subdirectory(src/ofilebackup)
add_subdirectory(src/libfilebackup)
add_subdirectory(src/libfilebackup_cexport)

if(NOT OFB_DISABLE_INSTALL)
  ExportFromInstall(${PROJECT_NAME})
endif()
