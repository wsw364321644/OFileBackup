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

set(OCmakeUtil_TAG e816493cd13c65f0f044446c2ab775dea99eae12)
set(UTILPP_TAG 1d72d318b3952a943b6451f0e4706578d4fa94a8)

project(OFileBackup VERSION ${VERSION})
enable_language(CXX ASM C)

if(WIN32)
  option(STATIC_CRT "Set to ON to build with static CRT on Windows (/MT)." OFF)
endif()

option(OFB_ONLY_LIB "Set to ON to only contain libfilebackup." OFF)
option(OFB_BUILD_TEST "Set to ON to contain test target." OFF)

if(CMAKE_INSTALL_PREFIX_INITIALIZED_TO_DEFAULT)
  set(CMAKE_INSTALL_PREFIX
    "${CMAKE_BINARY_DIR}/rundir"
    CACHE STRING "Directory to install after building" FORCE)
endif()

set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

if(PROJECT_IS_TOP_LEVEL)
  if(WIN32 AND STATIC_CRT)
    if(MSVC)
      set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
      add_definitions(-D_CRT_SECURE_NO_WARNINGS)
    endif()

    set(CMAKE_C_FLAGS_RELEASE "${CMAKE_C_FLAGS_RELEASE} /MT")
    set(CMAKE_C_FLAGS_DEBUG "${CMAKE_C_FLAGS_DEBUG} /MTd")
  endif()
endif()

FetchContent_Declare(
  OCmakeUtil
  GIT_REPOSITORY git@github.com:wsw364321644/OCmakeUtil.git
  GIT_TAG ${OCmakeUtil_TAG}
  GIT_SUBMODULES_RECURSE true
)
FetchContent_MakeAvailable(OCmakeUtil)
include(ProjectImport)
include(TargetImport)

if(STATIC_CRT)
  ImportProject(Boost STATIC_CRT STATIC URL "https://github.com/boostorg/boost/releases/download/boost-1.87.0/boost-1.87.0-cmake.tar.xz")
else()
  ImportProject(Boost STATIC URL "https://github.com/boostorg/boost/releases/download/boost-1.87.0/boost-1.87.0-cmake.tar.xz")
endif()
ImportProject(libuv STATIC_CRT SSH FIND TAG be6b81a352d17513c95be153afcb3148f1a451cd) # 1.47.0
ImportProject(MbedTLS STATIC STATIC_CRT SSH FIND TAG 2ca6c285a0dd3f33982dd57299012dacab1ff206) # 3.6.0
ImportProject(zstd STATIC_CRT SSH FIND TAG f0937b83d9a32cb2b59f99bbc4db717ae6e83c9b) # after v1.5.6
ImportProject(xxHash STATIC STATIC_CRT SSH FIND TAG bbb27a5efb85b92a0486cf361a8635715a53f6ba)
ImportTarget(rollinghashcpp SSH TAG 5cb883b8692f56636835697863ddb80cc8ef2311)

set(TF_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(TF_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(TF_BUILD_PROFILER ON CACHE BOOL "" FORCE)
set(TF_BUILD_BENCHMARKS OFF CACHE BOOL "" FORCE)
FetchContent_Declare(
  taskflow
  GIT_REPOSITORY git@github.com:taskflow/taskflow.git
  GIT_TAG 12f8bd4e970ab27fd3dee3bffa24b5b48b54ba39 # 3.6.0
)
FetchContent_MakeAvailable(taskflow)

set(UTILPP_WITH_STATIC_LIBUV OFF CACHE BOOL "" FORCE)
set(UTILPP_STATIC_CRT ON CACHE BOOL "" FORCE)
FetchContent_Declare(
  utilpp
  GIT_REPOSITORY git@github.com:wsw364321644/utilpp.git
  GIT_TAG ${UTILPP_TAG}
)
FetchContent_MakeAvailable(utilpp)

if(OFB_BUILD_TEST)
  FetchContent_Declare(
    Catch2
    GIT_REPOSITORY git@github.com:catchorg/Catch2.git
    GIT_TAG fa43b77429ba76c462b1898d6cd2f2d7a9416b14 # v3.7.1
  )
  FetchContent_MakeAvailable(Catch2)
endif()

if(NOT OFB_ONLY_LIB)
  FetchContent_Declare(
    cxxopts
    GIT_REPOSITORY git@github.com:jarro2783/cxxopts.git
    GIT_TAG 3bf268481da8208d171d8908e6491459de3651d7 # v3.2.0
  )
  FetchContent_MakeAvailable(cxxopts)

  add_subdirectory(src/BackupFolder)
  add_subdirectory(src/CompareManifest)
  add_subdirectory(src/RecoverFolder)
endif()

add_subdirectory(src/ofilebackup)
add_subdirectory(src/libfilebackup)
add_subdirectory(src/libfilebackup_cexport)
