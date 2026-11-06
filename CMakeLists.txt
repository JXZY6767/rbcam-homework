cmake_minimum_required(VERSION 3.16)
project(work LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(OpenCV 4 REQUIRED COMPONENTS
        core imgproc imgcodecs highgui dnn videoio
)

add_executable(work main.cpp)
target_include_directories(work PRIVATE ${OpenCV_INCLUDE_DIRS} /opt/MVS/include)
target_link_libraries(work PRIVATE ${OpenCV_LIBS} /opt/MVS/lib/64/libMvCameraControl.so)

# 课堂内存示例：独立运行，不需要连接相机。
add_executable(mat_memory mat_memory.cpp)
target_include_directories(mat_memory PRIVATE ${OpenCV_INCLUDE_DIRS})
target_link_libraries(mat_memory PRIVATE ${OpenCV_LIBS})
