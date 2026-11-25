// GD 2.2091 + Geode 6 빌드의 타입과 작은 변환. compat/gd2081/GameTypes.hpp에 같은 이름이 있고,
// CMakeLists.txt가 GD 버전에 맞는 쪽을 include 경로에 넣는다. 기능 코드는 compat/Compat.hpp로 쓴다.
#pragma once

#include <asp/fs/std.hpp>
#include <asp/time/Duration.hpp>

#include <filesystem>
#include <string>
#include <vector>

namespace versus::compat {
// GD 함수 시그니처의 문자열 / 배열. Geode 6부터 std 타입이다.
using String = std::string;
template <class T> using Vector = std::vector<T>;

// Geode가 주고받는 경로(Mod, dirs, utils::file). Geode 6부터 asp::fs::Path(UTF-8 문자열)다.
// Versus 안의 파일 처리는 std::filesystem을 쓰고, Geode와 주고받는 곳에서만 바꾼다.
using GeodePath = asp::fs::Path;
// 변환에 실패하면 빈 경로.
inline std::filesystem::path stdPath(asp::fs::PathView path) {
    return path.toStdPath().unwrapOrDefault();
}
inline asp::fs::Path geodePath(std::filesystem::path const& path) {
    return asp::fs::Path::fromStdPath(path);
}
// 로그 / 표시용 UTF-8 문자열.
inline std::string pathText(std::filesystem::path const& path) {
    return geodePath(path).string();
}

// 웹 요청 제한 시간.
inline asp::Duration requestTimeout(int seconds) {
    return asp::Duration::fromSecs(seconds);
}

// 마우스 휠 처리(scrollWheel)의 반환 타입. Geode 6에서는 처리했는지를 돌려준다.
using ScrollResult = bool;
inline bool scrolled(bool handled) {
    return handled;
}
}

// PlayLayer::init 훅의 매개변수와, 원래 함수로 넘길 인자.
#define VERSUS_PLAY_INIT_PARAMS GJGameLevel* level, bool dontCreateObjects
#define VERSUS_PLAY_INIT_ARGS level, dontCreateObjects
