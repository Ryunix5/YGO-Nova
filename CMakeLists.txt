# 3.26 required for `cmake -E copy_directory_if_different` (incremental asset
# copy — see the POST_BUILD step below).
cmake_minimum_required(VERSION 3.26)
project(EdoProPlus VERSION 1.0.4 LANGUAGES CXX C)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# ─── Find dependencies (via vcpkg toolchain) ─────────────────────────────────
find_package(SDL2       CONFIG REQUIRED)
find_package(unofficial-sqlite3 CONFIG REQUIRED)
find_package(unofficial-lua CONFIG REQUIRED)

# ─── ocgcore sources ─────────────────────────────────────────────────────────
set(OCGCORE_DIR ${CMAKE_SOURCE_DIR}/ocgcore)
set(OCGCORE_SOURCES
    ${OCGCORE_DIR}/card.cpp
    ${OCGCORE_DIR}/duel.cpp
    ${OCGCORE_DIR}/effect.cpp
    ${OCGCORE_DIR}/field.cpp
    ${OCGCORE_DIR}/interpreter.cpp
    ${OCGCORE_DIR}/libcard.cpp
    ${OCGCORE_DIR}/libdebug.cpp
    ${OCGCORE_DIR}/libduel.cpp
    ${OCGCORE_DIR}/libeffect.cpp
    ${OCGCORE_DIR}/libgroup.cpp
    ${OCGCORE_DIR}/ocgapi.cpp
    ${OCGCORE_DIR}/operations.cpp
    ${OCGCORE_DIR}/playerop.cpp
    ${OCGCORE_DIR}/processor.cpp
    ${OCGCORE_DIR}/processor_visit.cpp
    ${OCGCORE_DIR}/scriptlib.cpp
)

# ─── Dear ImGui sources (vendored) ───────────────────────────────────────────
set(IMGUI_DIR ${CMAKE_SOURCE_DIR}/vendor/imgui)
set(IMGUI_SOURCES
    ${IMGUI_DIR}/imgui.cpp
    ${IMGUI_DIR}/imgui_demo.cpp
    ${IMGUI_DIR}/imgui_draw.cpp
    ${IMGUI_DIR}/imgui_tables.cpp
    ${IMGUI_DIR}/imgui_widgets.cpp
    ${IMGUI_DIR}/backends/imgui_impl_sdl2.cpp
    ${IMGUI_DIR}/backends/imgui_impl_opengl3.cpp
)

# ─── Game sources ─────────────────────────────────────────────────────────────
set(GAME_SOURCES
    src/main.cpp
    src/Game.cpp
    src/AssetPaths.cpp
    src/DuelManager.cpp
    src/Renderer.cpp
    src/UI.cpp
    src/UIStyle.cpp
    src/AudioManager.cpp
    src/CardDB.cpp
    src/SnapshotManager.cpp
    src/NetSession.cpp
    src/ImageFetcher.cpp
    src/UpdateChecker.cpp
    src/CrashHandler.cpp
    src/DiscordRPC.cpp
    src/ScriptFetcher.cpp
)

# ─── Executable ───────────────────────────────────────────────────────────────
add_executable(EdoProPlus
    ${GAME_SOURCES}
    ${OCGCORE_SOURCES}
    ${IMGUI_SOURCES}
)

# ─── App identity: produced exe name + Windows icon ──────────────────────────
# Ship the binary as "YGONova.exe" (the CMake target keeps its internal name).
set_target_properties(EdoProPlus PROPERTIES OUTPUT_NAME "YGONova")
# Embed the taskbar / Explorer icon when the .ico is present. It lives next to
# app_icon.rc in installer/ so the resource compiler resolves the relative path.
# Generate the .ico from the logo with: python tools/make_icon.py
if(WIN32 AND EXISTS "${CMAKE_SOURCE_DIR}/installer/YGONova.ico")
    target_sources(EdoProPlus PRIVATE "${CMAKE_SOURCE_DIR}/installer/app_icon.rc")
endif()

# ─── App version (surfaced in the window title + main menu) ──────────────────
# EDOPRO_DEFAULT_RELAY bakes a default online relay host into the build so a
# public release lands players on your server with no typing (blank = localhost,
# the dev default). Set it at configure time: -DEDOPRO_DEFAULT_RELAY=host.
set(EDOPRO_DEFAULT_RELAY "" CACHE STRING
    "Default online relay host baked into the app (blank = localhost)")
# EDOPRO_UPDATE_REPO ("owner/name") enables the in-app update check against that
# GitHub repo's latest release. Blank = update check disabled.
set(EDOPRO_UPDATE_REPO "" CACHE STRING
    "GitHub owner/repo for in-app update checks (blank = disabled)")
# Card-art image host. Blank = built-in default (public ygoprodeck CDN). Point
# these at a self-hosted mirror to serve art in regions where the public CDN is
# blocked: -DEDOPRO_IMAGE_CDN_HOST=images.myserver.com -DEDOPRO_IMAGE_CDN_PATH=/cards/
set(EDOPRO_IMAGE_CDN_HOST "" CACHE STRING
    "Card-art image host (blank = public ygoprodeck CDN)")
set(EDOPRO_IMAGE_CDN_PATH "" CACHE STRING
    "Card-art image path prefix, e.g. /images/cards/ (blank = default)")
target_compile_definitions(EdoProPlus PRIVATE
    EDOPROPLUS_VERSION="${PROJECT_VERSION}"
    EDOPRO_DEFAULT_RELAY="${EDOPRO_DEFAULT_RELAY}"
    EDOPRO_UPDATE_REPO="${EDOPRO_UPDATE_REPO}")
if(EDOPRO_IMAGE_CDN_HOST)
    target_compile_definitions(EdoProPlus PRIVATE
        EDOPRO_IMAGE_CDN_HOST="${EDOPRO_IMAGE_CDN_HOST}")
endif()
if(EDOPRO_IMAGE_CDN_PATH)
    target_compile_definitions(EdoProPlus PRIVATE
        EDOPRO_IMAGE_CDN_PATH="${EDOPRO_IMAGE_CDN_PATH}")
endif()

# ─── Include dirs ─────────────────────────────────────────────────────────────
target_include_directories(EdoProPlus PRIVATE
    src/
    ${OCGCORE_DIR}
    ${IMGUI_DIR}
    ${IMGUI_DIR}/backends
)

# ─── Link libraries ───────────────────────────────────────────────────────────
find_package(OpenGL REQUIRED)

target_link_libraries(EdoProPlus PRIVATE
    $<IF:$<TARGET_EXISTS:SDL2::SDL2>,SDL2::SDL2,SDL2::SDL2-static>
    SDL2::SDL2main
    unofficial::sqlite3::sqlite3
    unofficial::lua::lua-cpp
    OpenGL::GL
)

# Multiplayer needs platform sockets; card art + update check need WinHTTP;
# opening the release page in a browser needs Shell32 (ShellExecute).
if(WIN32)
    target_link_libraries(EdoProPlus PRIVATE Ws2_32 winhttp Shell32)
else()
    find_package(Threads REQUIRED)
    target_link_libraries(EdoProPlus PRIVATE Threads::Threads)
endif()

# ─── Compiler flags ───────────────────────────────────────────────────────────
if(MSVC)
    target_compile_options(EdoProPlus PRIVATE /W3 /utf-8)
else()
    target_compile_options(EdoProPlus PRIVATE -Wall -Wextra)
endif()

# ─── Copy assets next to the exe after build (one-time) ──────────────────────
# IMPORTANT: the assets tree holds ~9k card images. Copying — or even
# content-comparing — it on every build added ~80s per incremental build (the
# "copying assets then it hangs" symptom). cards.cdb + images are effectively
# static, so sync_assets.cmake copies them ONCE (when the output copy is
# missing) and skips thereafter. Delete the output assets folder (or clean
# build) to force a refresh.
add_custom_command(TARGET EdoProPlus POST_BUILD
    COMMAND ${CMAKE_COMMAND}
        -DEDOPP_SRC=${CMAKE_SOURCE_DIR}/assets
        -DEDOPP_DST=$<TARGET_FILE_DIR:EdoProPlus>/assets
        -P ${CMAKE_SOURCE_DIR}/tools/sync_assets.cmake
    COMMENT "Syncing assets to output directory (one-time)"
)

# Copy SDL2 DLL on Windows if it is a shared library
if(WIN32)
    if(TARGET SDL2::SDL2)
        get_target_property(SDL2_TYPE SDL2::SDL2 TYPE)
        if(SDL2_TYPE STREQUAL "SHARED_LIBRARY")
            add_custom_command(TARGET EdoProPlus POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    $<TARGET_FILE:SDL2::SDL2>
                    $<TARGET_FILE_DIR:EdoProPlus>
                COMMENT "Copying SDL2.dll"
            )
        endif()
    endif()
endif()
