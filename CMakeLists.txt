cmake_minimum_required(VERSION 3.16)
project(demcheck LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)

find_package(Qt6 COMPONENTS Core Network QUIET)
if (NOT Qt6_FOUND)
    find_package(Qt5 5.12 REQUIRED COMPONENTS Core Network)
    set(QT_LIBS Qt5::Core Qt5::Network)
else()
    set(QT_LIBS Qt6::Core Qt6::Network)
endif()

add_executable(demcheck
    main.cpp
    src/TnmClient.cpp
    src/CsvTable.cpp
    src/SiteProcessor.cpp
)
target_include_directories(demcheck PRIVATE src)
target_link_libraries(demcheck PRIVATE ${QT_LIBS})
