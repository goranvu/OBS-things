cmake_minimum_required(VERSION 3.28...3.30)
include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/common/bootstrap.cmake" NO_POLICY_SCOPE)
project(${_name} VERSION ${_version} LANGUAGES C CXX)

# Native QWidget dock: frontend and Qt are required, not optional.
set(ENABLE_FRONTEND_API ON CACHE BOOL "Needs OBS frontend" FORCE)
set(ENABLE_QT ON CACHE BOOL "Needs Qt6" FORCE)
include(compilerconfig)
include(defaults)
include(helpers)

add_library(${CMAKE_PROJECT_NAME} MODULE)
find_package(libobs REQUIRED)
find_package(obs-frontend-api REQUIRED)
find_package(Qt6 REQUIRED COMPONENTS Core Widgets)

target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE
    OBS::libobs OBS::obs-frontend-api Qt6::Core Qt6::Widgets)
target_compile_features(${CMAKE_PROJECT_NAME} PRIVATE cxx_std_17)
target_sources(${CMAKE_PROJECT_NAME} PRIVATE src/plugin-main.cpp)
set_target_properties(${CMAKE_PROJECT_NAME} PROPERTIES
    AUTOMOC ON AUTOUIC ON AUTORCC ON)
set_target_properties_plugin(${CMAKE_PROJECT_NAME} PROPERTIES OUTPUT_NAME ${_name})
