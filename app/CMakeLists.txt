cmake_minimum_required(VERSION 3.24)
project(omarchy-mail VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

find_package(Qt6 6.11 REQUIRED COMPONENTS Core Gui Widgets Qml Quick Network DBus Concurrent Test)
qt_standard_project_setup(REQUIRES 6.11)

add_library(mailcore STATIC)
target_include_directories(mailcore PUBLIC src)
target_compile_definitions(mailcore PUBLIC QT_NO_KEYWORDS)
target_link_libraries(mailcore PUBLIC Qt6::Core Qt6::Gui Qt6::Qml Qt6::Quick Qt6::Network Qt6::DBus Qt6::Concurrent)

add_executable(omarchy-mail src/main.cpp)
target_link_libraries(omarchy-mail PRIVATE mailcore)
install(TARGETS omarchy-mail RUNTIME DESTINATION bin)

enable_testing()
set(OM_TEST_HOME "${CMAKE_BINARY_DIR}/test-home")
function(om_test name)
    add_executable(${name} tests/${name}.cpp)
    target_link_libraries(${name} PRIVATE mailcore Qt6::Test)
    if(TARGET omtest)
        target_link_libraries(${name} PRIVATE omtest)
    endif()
    target_compile_definitions(${name} PRIVATE OM_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
    add_test(NAME ${name} COMMAND ${name})
    set_tests_properties(${name} PROPERTIES ENVIRONMENT_MODIFICATION
        "QT_QPA_PLATFORM=set:offscreen;QT_QPA_OFFSCREEN_NO_GLX=set:1;QT_QUICK_BACKEND=set:software;QT_QPA_PLATFORMTHEME=unset:;HYPRLAND_INSTANCE_SIGNATURE=unset:;QT_IM_MODULE=unset:;LC_ALL=unset:;LC_MESSAGES=unset:;LC_TIME=unset:;LANG=set:en_US.UTF-8;XDG_RUNTIME_DIR=set:${OM_TEST_HOME}/run;XDG_CACHE_HOME=set:${OM_TEST_HOME}/cache;XDG_STATE_HOME=set:${OM_TEST_HOME}/state;XDG_CONFIG_HOME=set:${OM_TEST_HOME}/config;XDG_DATA_HOME=set:${OM_TEST_HOME}/data")
endfunction()

# ---- sources and tests: every task appends its own lines below ----
target_sources(mailcore PRIVATE src/Theme.h src/Theme.cpp src/I18n.h src/I18n.cpp src/Format.h src/Format.cpp)
qt_add_resources(mailcore "i18n" PREFIX "/" FILES i18n/ru.tsv)
om_test(tst_theme)
om_test(tst_i18n)
om_test(tst_format)
target_sources(mailcore PRIVATE src/Client.h src/Client.cpp)
add_library(omtest STATIC tests/FakeMaild.h tests/FakeMaild.cpp)
target_link_libraries(omtest PUBLIC mailcore Qt6::Test)
om_test(tst_client)
