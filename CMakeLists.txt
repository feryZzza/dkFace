cmake_minimum_required(VERSION 3.22.1)
project(main)

# 使用 C++11，满足 thread、mutex、atomic 等基础并发库需求。
set(CMAKE_CXX_STANDARD 11)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
find_package(Threads REQUIRED)
find_package(OpenCV REQUIRED)

# 开启 CTest，方便后续补充自动化测试。
include(CTest)
enable_testing()

# include/ 存放公共头文件，src/ 下的 cpp 文件自动加入构建。
include_directories(${PROJECT_SOURCE_DIR}/include/)
include_directories(${OpenCV_INCLUDE_DIRS})
set(INCLUDE_DIR ${CMAKE_SOURCE_DIR}/include)
file(GLOB SOURCES_DIR CONFIGURE_DEPENDS "src/*.cpp")

add_executable(main main.cpp ${SOURCES_DIR})
target_link_libraries(main PRIVATE Threads::Threads ${OpenCV_LIBS})

# CPack 配置保留默认值，后续可用于打包发布。
set(CPACK_PROJECT_NAME ${PROJECT_NAME})
set(CPACK_PROJECT_VERSION ${PROJECT_VERSION})
include(CPack)
