cmake_minimum_required(VERSION 3.10)
project(WebSocketClientExample)

# 设置 C++ 标准
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# 添加编译器标志
add_definitions(-D_WEBSOCKETPP_CPP11_STL_)

# 查找必要的依赖包
find_package(Boost REQUIRED COMPONENTS log system thread)
find_package(Threads REQUIRED)

find_package(PkgConfig)
pkg_search_module(SAMPLERATE REQUIRED samplerate)

# 包含头文件路径
include_directories(${Boost_INCLUDE_DIR})
include_directories(/home/ubuntu/Desktop/workspace/cppProjects/microphone_live/websocketpp) # 确保这里路径正确
include_directories(/usr/include)  # PortAudio 头文件位置
include_directories(${PROJECT_SOURCE_DIR}/include)
include_directories(${PROJECT_SOURCE_DIR}/json/include/nlohmann)
include_directories(${PROJECT_SOURCE_DIR}/json/include)
include_directories(/usr/include/rtaudio)  # RtAudio 头文件位置
include_directories(/usr/include/ncursesw)  # ncursesw 头文件位置
include_directories(${SAMPLERATE_INCLUDE_DIRS})

# 定义源文件
set(SOURCE_FILES
    microphone_live.cpp
)

# 创建可执行文件
add_executable(microphone_live ${SOURCE_FILES} Base64.cpp)


# 链接库
target_link_libraries(microphone_live
    ${Boost_LIBRARIES}
    ${CMAKE_THREAD_LIBS_INIT}
    ${SAMPLERATE_LIBRARIES}
    "/usr/lib/x86_64-linux-gnu/libportaudio.so"  # 直接指定 PortAudio 库的位置
    "/usr/lib/x86_64-linux-gnu/librtaudio.so"  # 直接指定 RtAudio 库的位置
    "/usr/lib/x86_64-linux-gnu/libncursesw.so"  # 直接指定 ncursesw 库的位置 
    Boost::log
)
