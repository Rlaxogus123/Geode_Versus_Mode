// Geode 6 경로(asp::fs::Path)와 std::filesystem::path 사이 변환.
// Geode 6부터 Mod/dirs/utils::file의 경로가 asp::fs::Path(UTF-8 문자열)다. Versus 안의 파일 처리는
// std::filesystem을 그대로 쓰고, Geode와 주고받는 곳에서만 바꾼다.
#pragma once

#include <asp/fs/std.hpp>

#include <filesystem>
#include <string>

namespace versus {
// Geode 경로 -> std 경로 (변환에 실패하면 빈 경로).
inline std::filesystem::path stdPath(asp::fs::PathView path) {
    return path.toStdPath().unwrapOrDefault();
}
// std 경로 -> Geode 경로.
inline asp::fs::Path geodePath(std::filesystem::path const& path) {
    return asp::fs::Path::fromStdPath(path);
}
// 로그·표시용 UTF-8 문자열 (Geode 5의 utils::string::pathToString 자리).
inline std::string pathText(std::filesystem::path const& path) {
    return geodePath(path).string();
}
}
