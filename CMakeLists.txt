cmake_minimum_required(VERSION 3.5)

project(bpf_tools VERSION 0.1 LANGUAGES CXX)

set(CMAKE_INCLUDE_CURRENT_DIR ON)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(CMAKE_AUTOUIC ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)

find_package(QT NAMES Qt6 Qt5 COMPONENTS Core REQUIRED)
find_package(Qt${QT_VERSION_MAJOR} COMPONENTS Core REQUIRED)

include(FetchContent)
FetchContent_Declare(
  googletest
  URL https://github.com/google/googletest/archive/03597a01ee50ed33e9dfd640b249b4be3799d395.zip
)
# For Windows: Prevent overriding the parent project's compiler/linker settings
set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(googletest)

enable_testing()

set(PROJECT_SOURCES
    bpffilter.cpp
    bpffilter.h
    bpfdistapp.cpp
    customprocess.cpp
    customprocess.h
    distcustomerapp.cpp
    distproxyapp.cpp
    distworkerapp.cpp
    distproxyserver.cpp
    distproxyserver.h
    functionprofiler.cpp
    functionprofiler.h
    main.cpp

    unittests/distproxytests.cpp
    unittests/distproxytests.h
)

add_executable(bpf_tools ${PROJECT_SOURCES})

# Tell cmake where to find BpfObject module
set(BPFTOOLS_REPO_PATH "${CMAKE_CURRENT_SOURCE_DIR}/libbpf-bootstrap/")
set(BPF_CMAKE_TOOLS_PATH ${BPFTOOLS_REPO_PATH}/tools/cmake)
list(APPEND CMAKE_MODULE_PATH ${BPF_CMAKE_TOOLS_PATH})
set(BPFOBJECT_BPFTOOL_EXE ${BPFTOOLS_REPO_PATH}/build/bpftool/bootstrap/bpftool)
set(BPFOBJECT_VMLINUX_H ${BPFTOOLS_REPO_PATH}/vmlinux/vmlinux.h)
set(LIBBPF_INCLUDE_DIRS ${BPFTOOLS_REPO_PATH}/build/libbpf)
set(LIBBPF_LIBRARIES ${BPFTOOLS_REPO_PATH}/build/libbpf/libbpf.a)
find_package(BpfObject REQUIRED)

bpf_object(bootstrap efilters/bootstrap.bpf.c)
target_link_libraries(bpf_tools PRIVATE bootstrap_skel)

target_include_directories(bpf_tools PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../)
target_link_directories(bpf_tools PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../build-commonpp-Desktop-Debug)

target_include_directories(bpf_tools PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/ld_preload/)
target_include_directories(bpf_tools PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/rpclib/include)
target_link_directories(bpf_tools PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/rpclib/build)

target_link_libraries(bpf_tools PRIVATE Qt${QT_VERSION_MAJOR}::Core)
target_link_libraries(bpf_tools PRIVATE bpf elf z procps rpc GTest::gtest_main commonpp)

include(GoogleTest)
gtest_discover_tests(bpf_tools)
