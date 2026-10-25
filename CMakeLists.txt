# SPDX-FileCopyrightText: 2026 Miryu Gaming <dev@miryugaming.org>
# SPDX-License-Identifier: GPL-3.0-or-later

cmake_minimum_required(VERSION 3.16)

project(miryu-duo VERSION 45.0.0 LANGUAGES CXX)

set(QT_MIN_VERSION "6.6.0")
set(KF6_MIN_VERSION "6.0.0")
set(KWIN_MIN_VERSION "6.0.0")

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)

find_package(ECM 5.240.0 REQUIRED NO_MODULE)
set(CMAKE_MODULE_PATH ${ECM_MODULE_PATH} ${ECM_KDE_MODULE_DIR} ${CMAKE_MODULE_PATH})

include(KDEInstallDirs)
include(KDECMakeSettings)
include(KDECompilerSettings NO_POLICY_SCOPE)
include(FeatureSummary)

find_package(Qt6 ${QT_MIN_VERSION} REQUIRED COMPONENTS Core Gui Widgets)
find_package(KF6 ${KF6_MIN_VERSION} REQUIRED COMPONENTS I18n)
find_package(KWin ${KWIN_MIN_VERSION} REQUIRED COMPONENTS kwineffects)

# 不同 KWin 版本导出的目标名略有差异，这里取实际存在的那个
if(TARGET KWin::kwin)
    set(MIRYUDUO_KWIN_TARGET KWin::kwin)
elseif(TARGET KWin::kwineffects)
    set(MIRYUDUO_KWIN_TARGET KWin::kwineffects)
else()
    message(FATAL_ERROR "未找到 KWin 特效库目标（KWin::kwin / KWin::kwineffects），请安装 KWin 6 开发文件")
endif()

# ---- 探测 KWin 特效 API 版本 ----
# KWin 6.6 起特效 API 大改：paintScreen() 返回 bool，区域类型由 QRegion 换成 Region，
# Output 换成 LogicalOutput，prePaintScreen() 不再接收 presentTime。
# 这里直接读取已安装的 effect.h 来判断，避免新旧版本各写死一套。
get_target_property(_miryuDuo_kwinIncludeDirs ${MIRYUDUO_KWIN_TARGET}
                    INTERFACE_INCLUDE_DIRECTORIES)
set(_miryuDuo_effectHeader "")
foreach(_dir IN LISTS _miryuDuo_kwinIncludeDirs)
    if(EXISTS "${_dir}/effect/effect.h")
        set(_miryuDuo_effectHeader "${_dir}/effect/effect.h")
        break()
    endif()
endforeach()
if(NOT _miryuDuo_effectHeader)
    # 目标属性里拿不到时（例如全是 BUILD_INTERFACE 生成表达式），退回系统路径搜索
    find_path(_miryuDuo_kwinIncludeDir NAMES effect/effect.h
              PATH_SUFFIXES kwin
              PATHS ${KDE_INSTALL_INCLUDEDIR})
    if(_miryuDuo_kwinIncludeDir)
        set(_miryuDuo_effectHeader "${_miryuDuo_kwinIncludeDir}/effect/effect.h")
    endif()
endif()

set(MIRYUDUO_NEW_KWIN_API 1)
if(_miryuDuo_effectHeader)
    file(STRINGS "${_miryuDuo_effectHeader}" _miryuDuo_newApiMark
         REGEX "const Region &deviceRegion")
    if(NOT _miryuDuo_newApiMark)
        set(MIRYUDUO_NEW_KWIN_API 0)
    endif()
    message(STATUS "Miryu Duo: 使用 ${_miryuDuo_effectHeader}，新版特效 API = ${MIRYUDUO_NEW_KWIN_API}")
else()
    message(WARNING "Miryu Duo: 未找到 effect/effect.h，默认按新版特效 API 编译")
endif()

set(miryuDuoSources
    src/main.cpp
    src/miryuduo.cpp
    src/miryuduo.h
    src/metadata.json
    src/shaders.qrc
)

add_library(org.miryugaming.duo MODULE ${miryuDuoSources})

target_link_libraries(org.miryugaming.duo PRIVATE
    ${MIRYUDUO_KWIN_TARGET}
    Qt6::Core
    Qt6::Gui
    KF6::I18n
)

target_compile_definitions(org.miryugaming.duo PRIVATE
    TRANSLATION_DOMAIN="miryu-duo"
    MIRYUDUO_NEW_KWIN_API=${MIRYUDUO_NEW_KWIN_API}
)

# KWin 从 <qt插件目录>/kwin/effects/plugins 加载二进制特效插件
install(TARGETS org.miryugaming.duo
        DESTINATION ${KDE_INSTALL_PLUGINDIR}/kwin/effects/plugins)

# ---- 翻译 ----
# 不用 ki18n_install()：它生成 .mo 的时机和安装路径受 KF6I18n 版本影响，
# 在 rpmbuild 里常常什么都没装出来，导致 %find_lang 报 “No translations found”。
# 这里自己调用 msgfmt，域名固定为 miryu-duo，路径固定为
# <localedir>/<lang>/LC_MESSAGES/miryu-duo.mo，%find_lang 必然能找到。
find_package(Gettext REQUIRED)

set(miryuDuoMoFiles "")
file(GLOB _miryuDuoPoFiles "${CMAKE_CURRENT_SOURCE_DIR}/po/*.po")
foreach(_poFile IN LISTS _miryuDuoPoFiles)
    get_filename_component(_lang "${_poFile}" NAME_WE)
    set(_moFile "${CMAKE_CURRENT_BINARY_DIR}/locale/${_lang}/LC_MESSAGES/miryu-duo.mo")

    add_custom_command(
        OUTPUT "${_moFile}"
        COMMAND ${CMAKE_COMMAND} -E make_directory
                "${CMAKE_CURRENT_BINARY_DIR}/locale/${_lang}/LC_MESSAGES"
        COMMAND ${GETTEXT_MSGFMT_EXECUTABLE} -c -o "${_moFile}" "${_poFile}"
        DEPENDS "${_poFile}"
        COMMENT "编译翻译 ${_lang}"
        VERBATIM)

    install(FILES "${_moFile}"
            DESTINATION ${KDE_INSTALL_LOCALEDIR}/${_lang}/LC_MESSAGES)

    list(APPEND miryuDuoMoFiles "${_moFile}")
endforeach()

add_custom_target(miryuDuoTranslations ALL DEPENDS ${miryuDuoMoFiles})

feature_summary(WHAT ALL)
