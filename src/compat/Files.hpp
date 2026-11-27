// 파일 읽기 / 쓰기. std::ifstream / std::ofstream 대신 C 파일 함수만 쓴다.
// GD 2.2091 Android(32비트)에서 파일 스트림이 GD의 libc++(libcocos2dcpp.so)과 맞지 않아 스트림을 지울 때
// 튕겼다(~basic_ios → ~ios_base). 플랫폼마다 다른 곳(Windows의 와이드 경로)은 이 파일에만 둔다.
#pragma once

#include <Geode/platform/cplatform.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace versus::files {
struct Closer {
    void operator()(std::FILE* file) const {
        if (file) std::fclose(file);
    }
};
using File = std::unique_ptr<std::FILE, Closer>;

// mode는 fopen과 같다("rb", "wb", "ab"). 열지 못하면 nullptr.
inline File open(std::filesystem::path const& path, char const* mode) {
#ifdef GEODE_IS_WINDOWS
    std::wstring const wide(mode, mode + std::strlen(mode));
    return File(_wfopen(path.c_str(), wide.c_str()));
#else
    return File(std::fopen(path.c_str(), mode));
#endif
}

// 파일 전체. 없거나 읽지 못하면 nullopt.
inline std::optional<std::string> read(std::filesystem::path const& path) {
    auto file = open(path, "rb");
    if (!file) return std::nullopt;
    std::string text;
    char buffer[65536];
    while (true) {
        auto const count = std::fread(buffer, 1, sizeof(buffer), file.get());
        text.append(buffer, count);
        if (count < sizeof(buffer)) break;
    }
    if (std::ferror(file.get())) return std::nullopt;
    return text;
}

// 파일을 data로 바꾸거나(append가 아니면) 끝에 붙인다. 다 쓰고 닫혔으면 true.
inline bool write(std::filesystem::path const& path, std::string_view data, bool append = false) {
    auto file = open(path, append ? "ab" : "wb");
    if (!file) return false;
    bool ok = data.empty() || std::fwrite(data.data(), 1, data.size(), file.get()) == data.size();
    ok = std::fflush(file.get()) == 0 && ok;
    return std::fclose(file.release()) == 0 && ok;
}

// offset부터 size바이트를 정확히 읽었으면 true.
inline bool readAt(std::FILE* file, std::uint64_t offset, void* into, std::size_t size) {
    if (!file) return false;
#ifdef GEODE_IS_WINDOWS
    if (_fseeki64(file, static_cast<long long>(offset), SEEK_SET) != 0) return false;
#else
    if (fseeko(file, static_cast<off_t>(offset), SEEK_SET) != 0) return false;
#endif
    return std::fread(into, 1, size, file) == size;
}

// std::getline처럼 '\n'으로 나눈 줄들('\r'은 그대로 둔다). 마지막 줄 뒤의 빈 줄은 넣지 않는다.
inline std::vector<std::string> lines(std::string_view text) {
    std::vector<std::string> result;
    std::size_t start = 0;
    while (start < text.size()) {
        auto const end = text.find('\n', start);
        if (end == std::string_view::npos) {
            result.emplace_back(text.substr(start));
            break;
        }
        result.emplace_back(text.substr(start, end - start));
        start = end + 1;
    }
    return result;
}
}
