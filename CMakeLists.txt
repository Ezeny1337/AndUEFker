cmake_minimum_required(VERSION 3.22)

project(AndUEFker VERSION 0.1.0 LANGUAGES CXX)

if(NOT ANDROID)
    message(FATAL_ERROR "AndUEFker currently supports Android only")
endif()

if(NOT CMAKE_ANDROID_ARCH_ABI STREQUAL "arm64-v8a")
    message(FATAL_ERROR "AndUEFker currently supports arm64-v8a only")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

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
    src/memory/RemoteMemorySource.cpp
    src/module/ModuleCatalog.cpp
    src/reflection/ReflectionReader.cpp
    src/ue/EngineSchema.cpp
    src/ue/EngineVersion.cpp
    src/ue/NameStoreReader.cpp
    src/ue/ObjectModelReader.cpp
    src/ue/ObjectStoreReader.cpp
    src/ue/SchemaCatalog.cpp
    src/ue/SchemaResolver.cpp
)

target_include_directories(anduefker_core
    PUBLIC
        "${CMAKE_CURRENT_SOURCE_DIR}/include"
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
