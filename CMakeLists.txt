cmake_minimum_required(VERSION 3.22)

project(AndUEFker VERSION 0.1.0 LANGUAGES CXX)

if(NOT ANDROID)
    message(FATAL_ERROR "AndUEFker currently supports Android only")
endif()

if(NOT CMAKE_ANDROID_ARCH_ABI MATCHES "^(arm64-v8a|armeabi-v7a)$")
    message(FATAL_ERROR "AndUEFker currently supports arm64-v8a and armeabi-v7a only")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(ANDUEFKER_PRODUCER_COMMIT "unknown")
set(ANDUEFKER_PRODUCER_WORKTREE "unknown")
set(ANDUEFKER_PRODUCER_IDENTITY_STATUS "git-not-found")
set(ANDUEFKER_PRODUCER_IDENTITY_QUERY_RESULT "not-run")
set(ANDUEFKER_PRODUCER_WORKTREE_STATUS "not-queried")
set(ANDUEFKER_PRODUCER_IDENTITY_FAILURE_REASON "git-not-found")
set(ANDUEFKER_PRODUCER_WORKTREE_FAILURE_REASON "not-queried")
# Persist only known categories, never Git stderr (which can contain local paths).
function(anduefker_git_failure_reason result error output)
    if("${result}" STREQUAL "0")
        set(reason "none")
    elseif(NOT "${result}" MATCHES "^-?[0-9]+$")
        set(reason "launch-failed")
    elseif("${error}" MATCHES "detected dubious ownership|unsafe repository")
        set(reason "unsafe-repository-ownership")
    elseif("${error}" MATCHES "not a git repository")
        set(reason "not-a-git-repository")
    elseif("${error}" MATCHES "Permission denied|Access is denied")
        set(reason "permission-denied")
    elseif("${error}" MATCHES "unknown revision|Needed a single revision|ambiguous argument")
        set(reason "revision-unavailable")
    else()
        set(reason "unclassified-git-error")
    endif()
    set(${output} "${reason}" PARENT_SCOPE)
endfunction()
find_package(Git QUIET)
if(Git_FOUND)
    set(ANDUEFKER_PRODUCER_IDENTITY_STATUS "worktree-query-failed")
    execute_process(COMMAND "${GIT_EXECUTABLE}" rev-parse --is-inside-work-tree
        WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
        RESULT_VARIABLE ANDUEFKER_GIT_WORKTREE_RESULT
        OUTPUT_VARIABLE ANDUEFKER_GIT_INSIDE_WORKTREE OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_VARIABLE ANDUEFKER_GIT_ERROR)
    anduefker_git_failure_reason("${ANDUEFKER_GIT_WORKTREE_RESULT}" "${ANDUEFKER_GIT_ERROR}"
        ANDUEFKER_PRODUCER_IDENTITY_FAILURE_REASON)
    set(ANDUEFKER_PRODUCER_IDENTITY_QUERY_RESULT "${ANDUEFKER_GIT_WORKTREE_RESULT}")
    if(ANDUEFKER_GIT_WORKTREE_RESULT STREQUAL "0" AND ANDUEFKER_GIT_INSIDE_WORKTREE STREQUAL "true")
        set(ANDUEFKER_PRODUCER_IDENTITY_STATUS "revision-query-failed")
        execute_process(COMMAND "${GIT_EXECUTABLE}" rev-parse HEAD
            WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
            RESULT_VARIABLE ANDUEFKER_GIT_RESULT
            OUTPUT_VARIABLE ANDUEFKER_GIT_COMMIT OUTPUT_STRIP_TRAILING_WHITESPACE
            ERROR_VARIABLE ANDUEFKER_GIT_ERROR)
        anduefker_git_failure_reason("${ANDUEFKER_GIT_RESULT}" "${ANDUEFKER_GIT_ERROR}"
            ANDUEFKER_PRODUCER_IDENTITY_FAILURE_REASON)
        set(ANDUEFKER_PRODUCER_IDENTITY_QUERY_RESULT "${ANDUEFKER_GIT_RESULT}")
        if(ANDUEFKER_GIT_RESULT STREQUAL "0" AND ANDUEFKER_GIT_COMMIT MATCHES "^[0-9a-fA-F]+$")
            set(ANDUEFKER_PRODUCER_COMMIT "${ANDUEFKER_GIT_COMMIT}")
            set(ANDUEFKER_PRODUCER_IDENTITY_STATUS "ok")
            set(ANDUEFKER_PRODUCER_WORKTREE_STATUS "status-query-failed")
            execute_process(COMMAND "${GIT_EXECUTABLE}" status --porcelain --untracked-files=normal
                WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
                RESULT_VARIABLE ANDUEFKER_GIT_STATUS_RESULT
                OUTPUT_VARIABLE ANDUEFKER_GIT_STATUS OUTPUT_STRIP_TRAILING_WHITESPACE
                ERROR_VARIABLE ANDUEFKER_GIT_ERROR)
            anduefker_git_failure_reason("${ANDUEFKER_GIT_STATUS_RESULT}" "${ANDUEFKER_GIT_ERROR}"
                ANDUEFKER_PRODUCER_WORKTREE_FAILURE_REASON)
            if(ANDUEFKER_GIT_STATUS_RESULT STREQUAL "0")
                set(ANDUEFKER_PRODUCER_WORKTREE_STATUS "ok")
                if(ANDUEFKER_GIT_STATUS STREQUAL "")
                    set(ANDUEFKER_PRODUCER_WORKTREE "clean")
                else()
                    set(ANDUEFKER_PRODUCER_WORKTREE "dirty")
                endif()
            endif()
        elseif(ANDUEFKER_GIT_RESULT STREQUAL "0")
            set(ANDUEFKER_PRODUCER_IDENTITY_STATUS "invalid-revision-output")
            set(ANDUEFKER_PRODUCER_IDENTITY_FAILURE_REASON "invalid-revision-output")
        endif()
    elseif(ANDUEFKER_GIT_WORKTREE_RESULT STREQUAL "0")
        set(ANDUEFKER_PRODUCER_IDENTITY_STATUS "not-a-worktree")
        set(ANDUEFKER_PRODUCER_IDENTITY_FAILURE_REASON "not-a-worktree")
    endif()
endif()
if(NOT ANDUEFKER_PRODUCER_IDENTITY_QUERY_RESULT MATCHES "^-?[0-9]+$"
   AND NOT ANDUEFKER_PRODUCER_IDENTITY_QUERY_RESULT STREQUAL "not-run")
    set(ANDUEFKER_PRODUCER_IDENTITY_QUERY_RESULT "launch-failed")
endif()
file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/generated/anduefker")
configure_file(cmake/BuildInfo.hpp.in generated/anduefker/BuildInfo.hpp @ONLY)

option(ANDUEFKER_ENABLE_THINLTO "Enable ThinLTO for native targets" ON)
option(ANDUEFKER_OPTIMIZE_FOR_SIZE "Use Clang size optimization for native targets" OFF)
option(ANDUEFKER_FORCE_NDEBUG "Define NDEBUG for all native targets" OFF)
option(ANDUEFKER_DISABLE_NDEBUG "Undefine NDEBUG for all native targets" OFF)

if(ANDUEFKER_FORCE_NDEBUG AND ANDUEFKER_DISABLE_NDEBUG)
    message(FATAL_ERROR "ANDUEFKER_FORCE_NDEBUG and ANDUEFKER_DISABLE_NDEBUG are mutually exclusive")
endif()

set(KITTY_MEMORY_DIR "${CMAKE_CURRENT_SOURCE_DIR}/third_party/KittyMemoryEx")
file(GLOB KITTY_MEMORY_SOURCES CONFIGURE_DEPENDS "${KITTY_MEMORY_DIR}/*.cpp")

add_library(kitty_memory_ex STATIC ${KITTY_MEMORY_SOURCES})
target_include_directories(kitty_memory_ex SYSTEM PUBLIC "${KITTY_MEMORY_DIR}")
target_compile_definitions(kitty_memory_ex PUBLIC kNO_KEYSTONE)
target_compile_options(kitty_memory_ex PRIVATE -fexceptions)
target_link_libraries(kitty_memory_ex PUBLIC log)

file(GLOB UE_ANALYZER_SOURCES CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/src/analyzer/Architecture/*.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/analyzer/UEAnalyzer/*.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/analyzer/UEAnalyzer/Analysis/*.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/analyzer/UEAnalyzer/Core/*.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/analyzer/UEAnalyzer/Strategies/*.cpp"
)

add_library(ue_analyzer STATIC ${UE_ANALYZER_SOURCES})
target_include_directories(ue_analyzer PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src/analyzer")
target_include_directories(ue_analyzer SYSTEM PUBLIC "${KITTY_MEMORY_DIR}")
target_compile_features(ue_analyzer PUBLIC cxx_std_20)
target_compile_options(ue_analyzer PRIVATE -Wall -Wextra -Wno-unused-parameter -fexceptions)
target_link_libraries(ue_analyzer PUBLIC kitty_memory_ex)

add_library(anduefker_core STATIC
    src/app/RuntimeSession.cpp
    src/analyzer/AnalyzerMemoryAdapter.cpp
    src/binding/BindingBuilder.cpp
    src/binding/CommonObjectCollector.cpp
    src/binding/GlobalLocator.cpp
    src/binding/LayoutDiscovery.cpp
    src/binding/LayoutProbes.cpp
    src/binding/RuntimeBinding.cpp
    src/generation/ArtifactWriter.cpp
    src/generation/CppTypeResolver.cpp
    src/generation/JsonExport.cpp
    src/memory/CaptureMemorySource.cpp
    src/memory/RemoteMemorySource.cpp
    src/module/ModuleCatalog.cpp
    src/module/ElfSymbols.cpp
    src/module/ElfSymbolQuery.cpp
    src/reflection/ReflectionReader.cpp
    src/reflection/ReflectionLayout.cpp
    src/ue/EngineSchema.cpp
    src/ue/EngineVersion.cpp
    src/ue/FunctionSemantics.cpp
    src/ue/NameStoreReader.cpp
    src/ue/ObjectModelReader.cpp
    src/ue/ObjectStoreReader.cpp
    src/ue/SchemaCatalog.cpp
    src/ue/SchemaEnum.cpp
    src/ue/SchemaField.cpp
    src/ue/SchemaFunction.cpp
    src/ue/SchemaObject.cpp
    src/ue/SchemaProperty.cpp
    src/ue/SchemaSubtypes.cpp
    src/ue/SchemaResolver.cpp
    src/ue/SchemaStruct.cpp
    src/ue/SchemaSelection.cpp
)

target_include_directories(anduefker_core
    PUBLIC
        "${CMAKE_CURRENT_SOURCE_DIR}/include"
    PRIVATE
        "${CMAKE_CURRENT_BINARY_DIR}/generated"
)
target_include_directories(anduefker_core SYSTEM PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/third_party"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/analyzer"
)

target_compile_options(anduefker_core PRIVATE
    -Wall
    -Wextra
    -Wpedantic
    -Werror
    -fexceptions
)
target_link_libraries(anduefker_core PUBLIC kitty_memory_ex ue_analyzer)

add_executable(AndUEFker src/app/main.cpp)
target_link_libraries(AndUEFker PRIVATE anduefker_core)
target_compile_options(AndUEFker PRIVATE -Wall -Wextra -Wpedantic -Werror)

set(ANDUEFKER_NATIVE_TARGETS kitty_memory_ex ue_analyzer anduefker_core AndUEFker)
foreach(target IN LISTS ANDUEFKER_NATIVE_TARGETS)
    target_compile_options(${target} PRIVATE
        -ffunction-sections
        -fdata-sections
    )
    if(ANDUEFKER_ENABLE_THINLTO)
        target_compile_options(${target} PRIVATE -flto=thin)
    endif()
    if(ANDUEFKER_OPTIMIZE_FOR_SIZE)
        target_compile_options(${target} PRIVATE -Oz)
    endif()
    if(ANDUEFKER_FORCE_NDEBUG)
        target_compile_options(${target} PRIVATE -DNDEBUG)
    elseif(ANDUEFKER_DISABLE_NDEBUG)
        target_compile_options(${target} PRIVATE -UNDEBUG)
    endif()
endforeach()

if(ANDUEFKER_ENABLE_THINLTO)
    target_link_options(AndUEFker PRIVATE -flto=thin)
endif()

# Android's lld removes unused function/data sections from the final executable.
target_link_options(AndUEFker PRIVATE -Wl,--gc-sections)
