# disc-remuxer-gui: built by mk/gui.mk (./build.sh), which passes where the
# engine library, its headers and our FFmpeg are.
cmake_minimum_required(VERSION 3.19)
project(disc-remuxer-gui LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)

find_package(Qt6 REQUIRED COMPONENTS Widgets)
find_package(PkgConfig REQUIRED)

# our FFmpeg only (static, with every library under it)
set(ENV{PKG_CONFIG_PATH} "${FFMPEG_PKGCONFIG}")
set(ENV{PKG_CONFIG_LIBDIR} "${FFMPEG_PKGCONFIG}")
pkg_check_modules(FF REQUIRED libavformat libavcodec libswresample libavutil)

add_executable(disc-remuxer-gui
    main.cpp
    MainWindow.cpp MainWindow.h
    DiscFinder.cpp DiscFinder.h
    EngineWorker.cpp EngineWorker.h
    DetailsPanel.cpp DetailsPanel.h
    Console.cpp Console.h
    QueueTree.cpp QueueTree.h
    PrefsDialog.cpp PrefsDialog.h)

target_include_directories(disc-remuxer-gui PRIVATE ${ENGINE_INCLUDE} ${SRC_INCLUDE} ${FFMPEG_PREFIX}/include)
target_compile_definitions(disc-remuxer-gui PRIVATE DISC_REMUXER_VERSION="${DISC_REMUXER_VERSION}")
target_compile_options(disc-remuxer-gui PRIVATE -Wall -Wextra -Wno-unused-parameter)
target_link_libraries(disc-remuxer-gui PRIVATE Qt6::Widgets ${ENGINE_LIB} ${ANALYZE_LIB} ${FF_STATIC_LDFLAGS} ${DISC_LIB} ${TIMELINE_LIB} ${IO_LIB} ${MUX_LIB} ${PARSE_LIB} ${BASE_LIB} pthread
                      ${COV_LDFLAGS})
