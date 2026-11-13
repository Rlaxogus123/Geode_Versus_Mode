# Versus 모드 빌드 설정 (Geode SDK의 setup_geode_mod 사용).
# 필요: 환경 변수 GEODE_SDK, C++23 컴파일러.
cmake_minimum_required(VERSION 3.21 FATAL_ERROR)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_CXX_VISIBILITY_PRESET hidden)

project(Versus VERSION 1.0.8 LANGUAGES C CXX)

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

# src/ 아래의 모든 .cpp가 모드 본체를 이룬다.
file(GLOB_RECURSE SOURCES CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp"
)
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
setup_geode_mod(${PROJECT_NAME})
# Argon(GD 계정 인증 라이브러리, MIT, github.com/GlobedGD/argon) 연동.
# -DVERSUS_ARGON=OFF 로 끄고 빌드할 수 있다.
option(VERSUS_ARGON "Link Argon for the account verification test" ON)
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
