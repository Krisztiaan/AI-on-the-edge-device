# This file was automatically generated for projects
# without default 'CMakeLists.txt' file.

##################################################################
##################################################################
execute_process(COMMAND git log --pretty=format:'%h' -n 1
                WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
                OUTPUT_VARIABLE GIT_REV
                ERROR_QUIET)

string(TIMESTAMP BUILD_TIME "%Y-%m-%d %H:%M")

# Check whether we got any revision (which isn't
# always the case, e.g. when someone downloaded a zip
# file from Github instead of a checkout)
if ("${GIT_REV}" STREQUAL "")
    set(GIT_REV "N/A")
    set(GIT_DIFF "")
    set(GIT_TAG "N/A")
    set(GIT_BRANCH "N/A")
else()
    execute_process(
        COMMAND bash -c "git diff --quiet --exit-code || echo +"
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
        OUTPUT_VARIABLE GIT_DIFF)
    execute_process(
        COMMAND git describe --exact-match --tags
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
        OUTPUT_VARIABLE GIT_TAG ERROR_QUIET)
    execute_process(
        COMMAND git rev-parse --abbrev-ref HEAD
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
        OUTPUT_VARIABLE GIT_BRANCH)

    string(STRIP "${GIT_REV}" GIT_REV)
    string(SUBSTRING "${GIT_REV}" 1 7 GIT_REV)
    string(STRIP "${GIT_DIFF}" GIT_DIFF)
    string(STRIP "${GIT_TAG}" GIT_TAG)
    string(STRIP "${GIT_BRANCH}" GIT_BRANCH)
endif()

set(VERSION "const char* GIT_REV=\"${GIT_REV}${GIT_DIFF}\";
const char* GIT_TAG=\"${GIT_TAG}\";
const char* GIT_BRANCH=\"${GIT_BRANCH}\";
const char* BUILD_TIME=\"${BUILD_TIME}\";")

if ("${GIT_TAG}" STREQUAL "") # Tag not set, show branch
    set(VERSION_HTML "Development-Branch: ${GIT_BRANCH} (Commit: ${GIT_REV}${GIT_DIFF})\n${GIT_REV}")
else() # Tag is set, ignore branch
    set(VERSION_HTML "Release: ${GIT_TAG} (Commit: ${GIT_REV}${GIT_DIFF})\n${GIT_REV}")
endif()

if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/version.cpp)
    file(READ ${CMAKE_CURRENT_SOURCE_DIR}/version.cpp VERSION_)
else()
    set(VERSION_ "")
endif()

if (NOT "${VERSION}" STREQUAL "${VERSION_}")
    file(WRITE ${CMAKE_CURRENT_SOURCE_DIR}/version.cpp "${VERSION}")
endif()
#######################################################################
#######################################################################

FILE(GLOB_RECURSE app_sources
    ${CMAKE_SOURCE_DIR}/main/*.c
    ${CMAKE_SOURCE_DIR}/main/*.cpp
    ${CMAKE_SOURCE_DIR}/main/*.cc
)

# idf_component_register(SRCS ${app_sources})

file(GLOB ui_embed_files RELATIVE "${CMAKE_CURRENT_SOURCE_DIR}" "ui/*")

find_package(Python3 QUIET COMPONENTS Interpreter)
if (Python3_Interpreter_FOUND)
    set(PYTHON_EXECUTABLE ${Python3_EXECUTABLE})
else()
    find_program(PYTHON_EXECUTABLE NAMES python3 python)
endif()
if (NOT PYTHON_EXECUTABLE)
    message(FATAL_ERROR "Python is required to gzip embedded UI assets")
endif()

# PlatformIO's ESP-IDF integration may try to compile the generated
# `<name>.S` sources before the IDF "embed files" custom commands run.
# Pre-generate the `.S` files during CMake configure so they exist.
set(embed_script "${IDF_PATH}/tools/cmake/scripts/data_file_embed_asm.cmake")

set(ui_gz_dir "${CMAKE_BINARY_DIR}/ui_gz")
file(MAKE_DIRECTORY "${ui_gz_dir}")

set(ui_embed_files_gz "")
foreach(ui_file IN LISTS ui_embed_files)
    get_filename_component(ui_name "${ui_file}" NAME) # e.g. "common.js"
    set(ui_in "${CMAKE_CURRENT_SOURCE_DIR}/${ui_file}")

    set(ui_gz "${ui_gz_dir}/${ui_name}.gz")
    execute_process(
        COMMAND ${PYTHON_EXECUTABLE} -c
                "import gzip, pathlib, sys; data = pathlib.Path(sys.argv[1]).read_bytes(); out = pathlib.Path(sys.argv[2]); out.parent.mkdir(parents=True, exist_ok=True); f = gzip.GzipFile(filename='', mode='wb', fileobj=out.open('wb'), mtime=0); f.write(data); f.close()"
                ${ui_in} ${ui_gz}
        RESULT_VARIABLE gzip_res
    )
    if (NOT gzip_res EQUAL 0)
        message(FATAL_ERROR "Failed to gzip embedded asset: ${ui_gz} from ${ui_in}")
    endif()

    list(APPEND ui_embed_files_gz "${ui_gz}")

    set(ui_out "${CMAKE_BINARY_DIR}/${ui_name}.gz.S")
    execute_process(
        COMMAND "${CMAKE_COMMAND}"
                -D DATA_FILE=${ui_gz}
                -D SOURCE_FILE=${ui_out}
                -D FILE_TYPE=BINARY
                -P "${embed_script}"
        RESULT_VARIABLE embed_res
    )
    if (NOT embed_res EQUAL 0)
        message(FATAL_ERROR "Failed to generate embedded asset: ${ui_out} from ${ui_in}")
    endif()
endforeach()

idf_component_register(
    SRCS ${app_sources}
    INCLUDE_DIRS "."
    REQUIRES esp_psram
    EMBED_FILES ${ui_embed_files_gz}
)
