// Windows에서 업데이트 후 부팅 확인 영수증을 파일로 남기는 코드.
// 업데이터가 이 영수증이 있으면 새 버전이 정상 시작된 것으로 보고 롤백하지 않는다.
#pragma once
#include "UpdateCore.hpp"

#include <fstream>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

namespace versus::update {
// 파일에 텍스트를 쓰고 디스크에 반영(flush)까지 한다. 실패하면 예외.
inline void writeWindowsText(fs::path const& path, std::string const& text) {
    regularOrAbsent(path);
    HANDLE handle = CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    require(handle != INVALID_HANDLE_VALUE, "Cannot open update confirmation");
    struct Close {
        HANDLE h;
        ~Close() { CloseHandle(h); }
    } close{handle};
    DWORD written = 0;
    require(text.size() <= MAXDWORD &&
                WriteFile(handle, text.data(), static_cast<DWORD>(text.size()), &written, nullptr) &&
                written == text.size(),
            "Cannot write update confirmation");
    require(SetEndOfFile(handle) && FlushFileBuffers(handle), "Cannot flush update confirmation");
}

// 임시 파일에 쓴 뒤 이름을 바꿔 원자적으로 교체한다.
inline void writeWindowsAtomicText(fs::path const& path, std::string const& text) {
    regularOrAbsent(path);
    auto pending = path;
    pending += ".tmp";
    writeWindowsText(pending, text);
    require(MoveFileExW(pending.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH),
            "Cannot replace update confirmation");
}

// nonce별 영수증 파일 경로. nonce는 SHA-256 형식이어야 한다.
inline fs::path windowsBootReceipt(fs::path const& root, std::string const& nonce) {
    require(digestValid(nonce), "Invalid update confirmation identity");
    return root / ("boot-confirmed-" + nonce + ".txt");
}

// nonce별 영수증과 공용 boot-confirmed.txt를 모두 쓴다.
inline void writeWindowsBootReceipt(fs::path const& root, std::string const& nonce) {
    writeWindowsAtomicText(windowsBootReceipt(root, nonce), nonce);
    writeWindowsText(root / "boot-confirmed.txt", nonce);
}

// 두 영수증 중 하나라도 nonce와 같은 내용이면 부팅이 확인된 것으로 본다.
inline bool windowsBootConfirmed(fs::path const& root, std::string const& nonce) {
    auto matches = [&](fs::path const& path) {
        regularOrAbsent(path);
        std::ifstream input(path);
        std::string value;
        std::getline(input, value);
        return value == nonce;
    };
    return matches(windowsBootReceipt(root, nonce)) || matches(root / "boot-confirmed.txt");
}
}
#endif
