cmake_minimum_required(VERSION 3.21)
project(TMS_SimpleEquipmentManager VERSION 1.0.10 LANGUAGES CXX)
if(NOT MSVC OR NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
  message(FATAL_ERROR "SEM requires MSVC x64")
endif()
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreadedDLL")
set(CMAKE_MAP_IMPORTED_CONFIG_DEBUG Release)
set(CMAKE_MAP_IMPORTED_CONFIG_RELWITHDEBINFO Release)
find_package(CommonLibSSE CONFIG REQUIRED)
find_package(nlohmann_json CONFIG REQUIRED)
set(plugin_metadata [=[
#include "SEMpch.h"
SKSEPluginInfo(
    .Version = REL::Version{ 1, 0, 10, 0 },
    .Name = "TMS_SimpleEquipmentManager"sv,
    .Author = "cashboxs"sv,
    .StructCompatibility = SKSE::StructCompatibility::Independent,
    .RuntimeCompatibility = SKSE::VersionIndependence::AddressLibrary,
    .MinimumSKSEVersion = REL::Version{ 0, 0, 0, 0 }
)
]=])
file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/SEMmetadata.cpp" "${plugin_metadata}")
add_library(${PROJECT_NAME} SHARED src/SEMmain.cpp "${CMAKE_CURRENT_BINARY_DIR}/SEMmetadata.cpp")
target_link_libraries(${PROJECT_NAME} PRIVATE CommonLibSSE::CommonLibSSE nlohmann_json::nlohmann_json)
target_compile_definitions(${PROJECT_NAME} PRIVATE __CMAKE_COMMONLIBSSE_PLUGIN=1 _UNICODE UNICODE NOMINMAX WIN32_LEAN_AND_MEAN _ITERATOR_DEBUG_LEVEL=0)
target_compile_features(${PROJECT_NAME} PRIVATE cxx_std_23)
target_precompile_headers(${PROJECT_NAME} PRIVATE src/SEMpch.h)
target_include_directories(${PROJECT_NAME} PRIVATE src)
if(MSVC)
  target_compile_options(${PROJECT_NAME} PRIVATE /W4 /permissive- /Zc:preprocessor /utf-8)
endif()
if(DEFINED ENV{SKYRIM_MODS_FOLDER})
  set(DEPLOY_PLUGIN_DIR "$ENV{SKYRIM_MODS_FOLDER}/TMS_SimpleEquipmentManager/SKSE/Plugins")
  add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory "${DEPLOY_PLUGIN_DIR}"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "$<TARGET_FILE:${PROJECT_NAME}>" "${DEPLOY_PLUGIN_DIR}/$<TARGET_FILE_NAME:${PROJECT_NAME}>" VERBATIM)
endif()
