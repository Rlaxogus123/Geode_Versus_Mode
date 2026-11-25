// GD 2.2081 + Geode 5 빌드의 타입과 작은 변환. compat/gd2091/GameTypes.hpp에 같은 이름이 있고,
// CMakeLists.txt가 GD 버전에 맞는 쪽을 include 경로에 넣는다. 기능 코드는 compat/Compat.hpp로 쓴다.
#pragma once

#include <Geode/c++stl/gdstdlib.hpp>
#include <Geode/utils/string.hpp>

#include <chrono>
#include <filesystem>
#include <string>

namespace versus::compat {
// GD 함수 시그니처의 문자열 / 배열. GD 2.2081에서는 GD의 STL 타입이다(Android에서는 std와 다른 타입).
using String = ::gd::string;
template <class T> using Vector = ::gd::vector<T>;

// Geode가 주고받는 경로(Mod, dirs, utils::file). Geode 5는 std::filesystem::path를 그대로 쓴다.
using GeodePath = std::filesystem::path;
inline std::filesystem::path stdPath(std::filesystem::path const& path) {
    return path;
}
inline std::filesystem::path geodePath(std::filesystem::path const& path) {
    return path;
}
// 로그 / 표시용 UTF-8 문자열.
inline std::string pathText(std::filesystem::path const& path) {
    return geode::utils::string::pathToString(path);
}

// 웹 요청 제한 시간.
inline std::chrono::seconds requestTimeout(int seconds) {
    return std::chrono::seconds(seconds);
}

// 마우스 휠 처리(scrollWheel)의 반환 타입. Geode 5에서는 반환값이 없다.
using ScrollResult = void;
inline void scrolled(bool) {}
}

// PlayLayer::init 훅의 매개변수와, 원래 함수로 넘길 인자.
#define VERSUS_PLAY_INIT_PARAMS GJGameLevel* level, bool useReplay, bool dontCreateObjects
#define VERSUS_PLAY_INIT_ARGS level, useReplay, dontCreateObjects
