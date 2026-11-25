# Versus 모드 빌드 설정 (Geode SDK의 setup_geode_mod 사용).
# 필요: 환경 변수 GEODE_SDK, C++23 컴파일러.
cmake_minimum_required(VERSION 3.21 FATAL_ERROR)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_CXX_VISIBILITY_PRESET hidden)

project(Versus VERSION 1.1.0 LANGUAGES C CXX)

# Windows 로컬 빌드는 성공하면 현재 Geode 프로필에 바로 설치한다.
if(WIN32)
    set(GEODE_DONT_INSTALL_MODS OFF CACHE BOOL
        "Install Versus after a local Windows build"
    )
endif()

# MSVC: 미리 컴파일된 헤더 생성 중 충돌을 피하려고 끈다.
if(MSVC)
    set(GEODE_DISABLE_PRECOMPILED_HEADERS ON CACHE BOOL "" FORCE)
endif()

# src/ 아래의 .cpp가 모드 본체를 이룬다. src/compat/의 GD 버전별 폴더(gd2081, gd2091)와 eclipse/는
# Geode SDK를 붙인 뒤 GD 버전과 Eclipse 패키지에 맞는 것만 넣는다(아래).
file(GLOB_RECURSE SOURCES CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp"
)
list(FILTER SOURCES EXCLUDE REGEX "/src/compat/(gd[0-9]+|eclipse)/")
add_library(${PROJECT_NAME} SHARED ${SOURCES})
if(APPLE)
    # SystemText.cpp가 CoreText로 시스템 글꼴 텍스트를 그린다.
    target_link_libraries(${PROJECT_NAME} "-framework CoreText" "-framework CoreGraphics"
        "-framework CoreFoundation")
endif()
if(MSVC)
    # /MP: 소스를 병렬로 컴파일. 크래시 로그의 RVA 해석용 .map 파일도 남긴다.
    target_compile_options(${PROJECT_NAME} PRIVATE /Z7 /FS /MP)
    target_link_options(${PROJECT_NAME} PRIVATE
        "/MAP:${CMAKE_CURRENT_BINARY_DIR}/$<CONFIG>/tipp7.versus.map")
endif()
if(WIN32)
    # PE 헤더의 시각 칸에 링크한 시각 대신 내용의 해시를 넣는다(/Brepro).
    # 같은 소스·같은 도구로 다시 빌드하면 바이트까지 같은 DLL이 나와야 GitHub 릴리스를 재현해 확인할 수 있다.
    target_link_options(${PROJECT_NAME} PRIVATE "LINKER:/Brepro")
endif()

# GEODE_SDK 환경 변수로 SDK 위치를 찾는다.
set(GEODE_SDK_PATH "$ENV{GEODE_SDK}")
if(NOT GEODE_SDK_PATH)
    message(FATAL_ERROR
        "Unable to find the Geode SDK. Set GEODE_SDK to the SDK directory."
    )
endif()
file(TO_CMAKE_PATH "${GEODE_SDK_PATH}" GEODE_SDK_PATH)
if(NOT EXISTS "${GEODE_SDK_PATH}/CMakeLists.txt")
    message(FATAL_ERROR
        "GEODE_SDK does not point to a valid Geode SDK: ${GEODE_SDK_PATH}"
    )
endif()
message(STATUS "Found Geode: ${GEODE_SDK_PATH}")

add_subdirectory("${GEODE_SDK_PATH}" "${CMAKE_CURRENT_BINARY_DIR}/geode")

# GD 2.2081(Geode 5)과 GD 2.2091(Geode 6)을 같은 소스로 빌드한다. 두 버전에서 다른 부분은 src/compat/에만 있고
# (src/compat/Compat.hpp), Geode SDK가 정한 GD 버전에 맞는 폴더 하나를 넣는다.
if(GEODE_GD_VERSION STREQUAL "2.2081")
    set(VERSUS_COMPAT gd2081)
elseif(GEODE_GD_VERSION STREQUAL "2.2091")
    set(VERSUS_COMPAT gd2091)
else()
    message(FATAL_ERROR
        "Versus builds for GD 2.2081 (Geode 5) and GD 2.2091 (Geode 6); this Geode SDK is for GD ${GEODE_GD_VERSION}"
    )
endif()
file(GLOB VERSUS_COMPAT_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/src/compat/${VERSUS_COMPAT}/*.cpp")
target_sources(${PROJECT_NAME} PRIVATE ${VERSUS_COMPAT_SOURCES})
target_include_directories(${PROJECT_NAME} PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src/compat/${VERSUS_COMPAT}")
message(STATUS "Versus: GD ${GEODE_GD_VERSION} (src/compat/${VERSUS_COMPAT})")

# mod.json의 Geode 버전, GD 버전, Argon 의존성은 GD 버전마다 다르다. Geode CLI가 mod.json을 읽으므로
# (GEODE_DISABLE_CLI_CALLS가 아니면) 이 SDK와 맞는지 먼저 본다. 바꾸는 방법: tools/geode_package.py modjson.
if(NOT GEODE_DISABLE_CLI_CALLS)
    file(READ "${CMAKE_CURRENT_SOURCE_DIR}/mod.json" VERSUS_MOD_JSON)
    string(JSON VERSUS_MOD_GD ERROR_VARIABLE VERSUS_MOD_GD_ERROR GET "${VERSUS_MOD_JSON}" gd win)
    if(NOT VERSUS_MOD_GD STREQUAL GEODE_GD_VERSION)
        message(FATAL_ERROR
            "mod.json is for GD ${VERSUS_MOD_GD}, but this Geode SDK is for GD ${GEODE_GD_VERSION}. "
            "Switch mod.json first: python tools/geode_package.py modjson ${GEODE_GD_VERSION}"
        )
    endif()
endif()

setup_geode_mod(${PROJECT_NAME})

# Argon(GD 계정 인증 라이브러리, MIT, github.com/GlobedGD/argon) 연동(src/compat/gd*/Argon.cpp).
# GD 2.209부터 Argon은 Geode 모드(dankmeme.argon)라서 GD 2.2091 빌드는 mod.json 의존성으로 받고, 헤더와 라이브러리는
# Geode가 빌드 폴더의 geode-deps로 연결한다. GD 2.2081 빌드는 C++ 라이브러리 1.5를 CPM으로 받으며,
# -DVERSUS_ARGON=OFF 로 끄고 빌드할 수 있다.
option(VERSUS_ARGON "Link Argon for the account verification test (GD 2.2081)" ON)
if(VERSUS_COMPAT STREQUAL "gd2091")
    target_compile_definitions(${PROJECT_NAME} PRIVATE VERSUS_ARGON=1)
else()
    if(VERSUS_ARGON)
        # 오프라인으로 구성된 빌드 폴더에서도 Argon만은 한 번 내려받도록 허용한다.
        set(FETCHCONTENT_FULLY_DISCONNECTED OFF)
        CPMAddPackage("gh:GlobedGD/argon@1.5.1")
        unset(FETCHCONTENT_FULLY_DISCONNECTED)
    endif()
    if(VERSUS_ARGON AND TARGET argon)
        target_link_libraries(${PROJECT_NAME} argon)
        target_compile_definitions(${PROJECT_NAME} PRIVATE VERSUS_ARGON=1)
    else()
        if(VERSUS_ARGON)
            message(WARNING "Argon could not be fetched: building without the Argon test button")
        endif()
        target_compile_definitions(${PROJECT_NAME} PRIVATE VERSUS_ARGON=0)
    endif()
endif()

# Eclipse(선택 의존성)의 설정 읽기(src/compat/Eclipse.hpp). setup_geode_mod가 geode-deps에 받아 둔 Eclipse 패키지가
# 이 SDK와 같은 Geode 메이저 버전용일 때만 그 헤더로 읽는다(src/compat/eclipse/Readable.cpp). Geode CLI는 Geode 버전과
# 상관없이 mod.json 범위에 맞는 최신판을 받으므로, 예를 들어 Geode 6용 Eclipse가 아직 없으면 Geode 5용 패키지가 오고
# 그 헤더는 Geode 6에서 컴파일되지 않는다. 그때는 Unreadable.cpp가 들어가고, Versus는 Eclipse가 로드돼 있기만 해도
# 대결에서 금지 기능으로 본다. 새 Eclipse API가 맞지 않아 빌드가 깨지면 -DVERSUS_USE_ECLIPSE_API=OFF로 끌 수 있다.
# Geode CLI는 같은 메이저 버전(v1.x) 안에서 가장 최근에 올라온 Eclipse를 받으므로, Eclipse가 Geode 6용을 v1.x로 내면
# GD 2.2081 빌드에도 그 패키지가 와서 Eclipse를 읽지 못하게 된다. 그런 빌드가 조용히 나가지 않도록, 읽을 수 있어야 하는
# 빌드(GitHub Actions의 GD 2.2081 빌드)는 -DVERSUS_REQUIRE_ECLIPSE_API=ON으로 구성하고, 그때 읽지 못하면 멈춘다.
option(VERSUS_USE_ECLIPSE_API "Read Eclipse's settings when its package is for this Geode version" ON)
option(VERSUS_REQUIRE_ECLIPSE_API "Stop when this build can't read Eclipse's settings" OFF)
set(VERSUS_ECLIPSE_SOURCE Unreadable.cpp)
set(VERSUS_ECLIPSE_DEP "${CMAKE_CURRENT_BINARY_DIR}/geode-deps/eclipse.eclipse-menu")
if(VERSUS_USE_ECLIPSE_API AND EXISTS "${VERSUS_ECLIPSE_DEP}/mod.json")
    file(READ "${VERSUS_ECLIPSE_DEP}/mod.json" VERSUS_ECLIPSE_JSON)
    string(JSON VERSUS_ECLIPSE_GEODE ERROR_VARIABLE VERSUS_ECLIPSE_ERROR GET "${VERSUS_ECLIPSE_JSON}" geode)
    string(REGEX MATCH "^v?([0-9]+)" VERSUS_ECLIPSE_MATCH "${VERSUS_ECLIPSE_GEODE}")
    set(VERSUS_ECLIPSE_MAJOR "${CMAKE_MATCH_1}")
    string(REGEX MATCH "^([0-9]+)" VERSUS_GEODE_MATCH "${GEODE_VERSION}")
    set(VERSUS_GEODE_MAJOR "${CMAKE_MATCH_1}")
    # Geode CLI 4는 Geode 6용 모드를 묶을 때 include/를 벗기기도 한다.
    set(VERSUS_ECLIPSE_HEADER "")
    if(NOT VERSUS_ECLIPSE_ERROR AND VERSUS_ECLIPSE_MAJOR STREQUAL VERSUS_GEODE_MAJOR)
        if(EXISTS "${VERSUS_ECLIPSE_DEP}/config.hpp")
            set(VERSUS_ECLIPSE_HEADER "eclipse.eclipse-menu/config.hpp")
        elseif(EXISTS "${VERSUS_ECLIPSE_DEP}/include/config.hpp")
            set(VERSUS_ECLIPSE_HEADER "eclipse.eclipse-menu/include/config.hpp")
        endif()
    endif()
    if(VERSUS_ECLIPSE_HEADER)
        set(VERSUS_ECLIPSE_SOURCE Readable.cpp)
        file(CONFIGURE OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/versus-eclipse/VersusEclipseConfig.hpp"
            CONTENT "#pragma once\n#include <${VERSUS_ECLIPSE_HEADER}>\n")
        target_include_directories(${PROJECT_NAME} PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/versus-eclipse")
    endif()
    message(STATUS "Versus: Eclipse package for Geode ${VERSUS_ECLIPSE_GEODE} -> src/compat/eclipse/${VERSUS_ECLIPSE_SOURCE}")
else()
    message(STATUS "Versus: no Eclipse package or VERSUS_USE_ECLIPSE_API=OFF -> src/compat/eclipse/${VERSUS_ECLIPSE_SOURCE}")
endif()
if(VERSUS_REQUIRE_ECLIPSE_API AND NOT VERSUS_ECLIPSE_SOURCE STREQUAL "Readable.cpp")
    message(FATAL_ERROR
        "Versus: this build can't read Eclipse's settings (geode-deps/eclipse.eclipse-menu is missing or not for "
        "Geode ${GEODE_VERSION}). Released as it is, a loaded Eclipse would count as a banned feature for every player "
        "of this GD version. Pin an Eclipse made for this Geode version before releasing."
    )
endif()
target_sources(${PROJECT_NAME} PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src/compat/eclipse/${VERSUS_ECLIPSE_SOURCE}")
