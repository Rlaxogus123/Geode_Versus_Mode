// 설치된 .geode 패키지를 안전하게 교체하는 트랜잭션(PackageTransaction).
// 교체 전 백업을 만들고, 저널(android-install.job)에 이전/새 해시를 기록하며,
// 새 버전이 정상 부팅되면 확인 영수증을 남기고 문제가 있으면 백업으로 복구한다.
// 이름과 달리 Android 외 플랫폼에서도 쓰인다(Windows는 파일 교체 재시도 포함).
#pragma once
#include "../compat/Files.hpp"
#include "UpdateCore.hpp"
#include <chrono>
#include <optional>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <cerrno>
#include <fcntl.h>
#include <unistd.h>
#endif

namespace versus::update {
// 교체 대상(m_target)을 임시 파일(m_next)로 준비한 뒤 이름을 바꿔 교체하는 트랜잭션 객체.
// Journal은 교체 전 해시, 교체 후 해시, 버전, 채널을 담는다.
class PackageTransaction {
    fs::path m_root, m_target, m_next;
    struct Journal {
        std::string oldHash, newHash, version, channel;
    };
    // 파일 또는 디렉터리를 디스크에 강제 반영(fsync / FlushFileBuffers)한다. 실패는 무시한다.
    static void sync(fs::path const& path, bool directory = false) {
#ifndef _WIN32
        int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC | (directory ? O_DIRECTORY : 0));
        if (fd < 0) return;
        (void)::fsync(fd);
        ::close(fd);
#else
        if (directory) return;
        HANDLE fd = CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                nullptr, OPEN_EXISTING, 0, nullptr);
        if (fd == INVALID_HANDLE_VALUE) return;
        (void)FlushFileBuffers(fd);
        CloseHandle(fd);
#endif
    }
#ifdef _WIN32
    // Windows에서 다른 프로세스가 잡고 있어 잠시 후 다시 시도할 만한 오류인지.
    static bool transient(DWORD error) {
        return error == ERROR_SHARING_VIOLATION || error == ERROR_LOCK_VIOLATION || error == ERROR_ACCESS_DENIED;
    }
#endif
    // from을 to로 바꿔 놓는다. Windows는 잠금 오류일 때 250ms 간격으로 최대 40번 재시도한다.
    static void replace(fs::path const& from, fs::path const& to) {
        regularOrAbsent(from);
        regularOrAbsent(to);
#ifdef _WIN32
        for (int attempt = 0;; ++attempt) {
            if (MoveFileExW(from.c_str(), to.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) return;
            auto const error = GetLastError();
            if (attempt >= 40 || !transient(error))
                throw std::runtime_error("Could not replace " + to.filename().string() + " (Windows error " +
                                         std::to_string(error) + ")");
            Sleep(250);
        }
#else
        fs::rename(from, to);
#endif
    }
    // 파일 복사. 실패하면 250ms 간격으로 최대 40번 재시도한다.
    static void copy(fs::path const& from, fs::path const& to) {
        for (int attempt = 0;; ++attempt) {
            std::error_code error;
            fs::copy_file(from, to, fs::copy_options::overwrite_existing, error);
            if (!error) return;
            if (attempt >= 40)
                throw std::runtime_error("Could not copy " + from.filename().string() + ": " + error.message());
#ifdef _WIN32
            Sleep(250);
#else
            usleep(250000);
#endif
        }
    }
    // 임시 파일에 쓰고 디스크에 반영한 뒤 이름을 바꿔 저널/영수증 파일을 원자적으로 쓴다.
    void write(fs::path const& path, std::string const& value) {
        auto temporary = path;
        temporary += ".tmp";
        regularOrAbsent(temporary);
        regularOrAbsent(path);
        require(versus::files::write(temporary, value), "Cannot save update journal");
        sync(temporary);
        replace(temporary, path);
        sync(path.parent_path(), true);
    }
    // 저널 파일을 읽고 형식(매직 문자열, 해시, 버전, 채널)을 검증한다.
    Journal journal() const {
        auto path = m_root / "android-install.job";
        regularOrAbsent(path);
        require(fs::is_regular_file(path) && fs::file_size(path) < 1024, "Missing update journal");
        auto rows = versus::files::lines(versus::files::read(path).value_or(std::string()));
        rows.resize(5);
        std::string const magic = rows[0];
        Journal j;
        j.oldHash = rows[1];
        j.newHash = rows[2];
        j.version = rows[3];
        j.channel = rows[4];
        require(magic == "VERSUS-ANDROID-1" && digestValid(j.oldHash) && digestValid(j.newHash) &&
                    versionValid(j.version) && (channelValid(j.channel) || j.channel == "any"),
                "Invalid update journal");
        return j;
    }
    // 저널과 부팅 확인 영수증을 지운다.
    void retireJournal() {
        auto marker = m_root / "android-install.job", receipt = m_root / "android-boot-confirmed.txt";
        regularOrAbsent(marker);
        regularOrAbsent(receipt);
        std::error_code error;
        fs::remove(marker, error);
        fs::remove(receipt, error);
        sync(m_root, true);
    }
    // 새 파일의 수정 시각을 기존 파일과 달라지게 만든다(변경 감지를 위해 최소 2초 차이).
    void distinctModificationTime() {
        std::error_code error;
        auto before = fs::last_write_time(m_target, error);
        if (error) return;
        fs::last_write_time(m_next, fs::file_time_type::clock::now(), error);
        if (error) return;
        auto after = fs::last_write_time(m_next, error);
        if (error) return;
        if (std::chrono::duration_cast<std::chrono::seconds>(after - before).count() == 0)
            fs::last_write_time(m_next, before + std::chrono::seconds(2), error);
    }
    // 두 경로가 같은 폴더를 가리키는지.
    static bool sameDirectory(fs::path const& a, fs::path const& b) {
        std::error_code error;
        if (fs::equivalent(a, b, error)) return true;
        auto ca = fs::weakly_canonical(a, error);
        if (error) return false;
        auto cb = fs::weakly_canonical(b, error);
        if (error) return false;
        return ca == cb;
    }
    // 여유 공간이 충분한지(확인할 수 없으면 충분하다고 본다).
    static bool enoughSpace(fs::path const& path, uintmax_t needed) {
        std::error_code error;
        auto info = fs::space(path, error);
        return error || info.available >= needed;
    }

public:
    // 작업 폴더와 설치 대상을 확인한다: 심볼릭 링크가 아니고, 대상이 mods 폴더 안의 .geode
    // 파일이어야 한다.
    PackageTransaction(fs::path root, fs::path target, fs::path mods)
        : m_root(std::move(root)), m_target(std::move(target)) {
        require(!fs::is_symlink(m_root), "Unsafe updater directory");
        fs::create_directories(m_root);
        regularOrAbsent(m_target);
        require(fs::is_regular_file(m_target) && m_target.extension() == ".geode", "Installed package is missing");
        require(sameDirectory(m_target.parent_path(), mods), "Unexpected installation directory");
        m_next = m_target;
        m_next += ".versus-next";
    }
    // 백업 파일 경로와, 그 파일이 주어진 해시와 같은지 확인하는 함수.
    fs::path backup() const { return m_root / "android-previous.geode"; }
    bool backupMatches(std::string const& hash) const {
        regularOrAbsent(backup());
        return fs::is_regular_file(backup()) && hashFile(backup()) == hash;
    }
    // 부팅 확인 영수증에 기록된 해시.
    std::string receipt() const {
        auto path = m_root / "android-boot-confirmed.txt";
        regularOrAbsent(path);
        auto rows = versus::files::lines(versus::files::read(path).value_or(std::string()));
        return rows.empty() ? std::string() : rows[0];
    }
    // 저널을 읽는다. 없으면 nullopt, 손상되었으면 지우고 nullopt.
    std::optional<Journal> readJournal() {
        auto marker = m_root / "android-install.job";
        regularOrAbsent(marker);
        if (!fs::exists(marker)) return std::nullopt;
        try {
            return journal();
        } catch (std::exception const&) {
            retireJournal();
            return std::nullopt;
        }
    }
    // 시작할 때 이전 설치 결과를 정리한다. 설치된 파일이 저널의 새 해시와 같고 현재 실행 중인
    // 버전이면
    // 정상 부팅으로 영수증을 쓴다. 같지만 아직 로드되지 않았다면 true(다음 실행에 적용됨),
    // 다르면 저널을 지운다.
    bool reconcile(std::string const& runningVersion) {
        auto j = readJournal();
        if (!j) return false;
        auto current = hashFile(m_target);
        if (current == j->newHash) {
            if (j->version == runningVersion) {
                write(m_root / "android-boot-confirmed.txt", j->newHash);
                return false;
            }
            return true;
        }
        retireJournal();
        return false;
    }
    // 새 설치 전에 이전 설치 상태를 정리한다. 이미 목표 해시로 설치되어 있으면 true(다시
    // 설치할 필요 없음).
    bool settlePrevious(std::string const& runningVersion, std::string const& targetHash) {
        auto j = readJournal();
        if (!j) return false;
        auto current = hashFile(m_target);
        if (current != j->newHash) return false;
        if (j->version == runningVersion) {
            write(m_root / "android-boot-confirmed.txt", j->newHash);
            return false;
        }
        return current == targetHash;
    }
    // 새 패키지를 설치한다: 매니페스트와 받은 파일을 검증하고 디스크 여유 공간을 확인한 뒤,
    // 현재 패키지를 백업(검증 포함)하고 새 파일을 임시로 복사해 검증한다.
    // 저널을 기록한 다음 이름을 바꿔 교체한다. 이전 설치가 부팅 확인 전이면 기존 백업을
    // 유지한다.
    void prepare(fs::path const& ready, Manifest const& manifest) {
        ManifestVerifier().validate(manifest);
        verifyFile(ready, manifest);
        auto current = hashFile(m_target);
        auto marker = m_root / "android-install.job";
        std::string baseline = current;
        bool keepBackup = false;
        if (auto j = readJournal()) {
            if (current == j->newHash && j->newHash == manifest.sha256) return;
            if (current == j->newHash && receipt() != j->newHash && backupMatches(j->oldHash)) {
                baseline = j->oldHash;
                keepBackup = true;
            } else
                retireJournal();
        }
        regularOrAbsent(m_next);
        regularOrAbsent(backup());
        auto backupNext = backup();
        backupNext += ".tmp";
        regularOrAbsent(backupNext);
        auto oldSize = fs::file_size(m_target);
        auto needed = (keepBackup ? 0 : oldSize) + manifest.size + 1024 * 1024;
        require(enoughSpace(m_root, needed) && enoughSpace(m_target.parent_path(), needed),
                "Not enough installation disk space");
        if (!keepBackup) {
            copy(m_target, backupNext);
            require(hashFile(backupNext) == current, "Recovery backup verification failed");
            sync(backupNext);
            replace(backupNext, backup());
            sync(m_root, true);
        }
        copy(ready, m_next);
        verifyFile(m_next, manifest);
        distinctModificationTime();
        sync(m_next);
        auto receiptPath = m_root / "android-boot-confirmed.txt";
        regularOrAbsent(receiptPath);
        if (fs::remove(receiptPath)) sync(m_root, true);
        write(marker, "VERSUS-ANDROID-1\n" + baseline + "\n" + manifest.sha256 + "\n" + manifest.version + "\n" +
                          manifest.channel + "\n");
        require(hashFile(m_target) == current, "Installed package changed during update");
        replace(m_next, m_target);
        sync(m_target.parent_path(), true);
    }
    // 현재 버전/채널이 저널과 일치하고 설치된 파일이 새 해시와 같으면 부팅 확인 영수증을 쓴다.
    bool confirm(std::string const& version, std::string const& channel) {
        if (!fs::exists(m_root / "android-install.job")) return false;
        auto j = journal();
        if (j.version != version || (channel != "any" && j.channel != channel) || hashFile(m_target) != j.newHash)
            return false;
        write(m_root / "android-boot-confirmed.txt", j.newHash);
        return true;
    }
    // 백업으로 되돌린다. 백업과 설치된 파일의 해시가 저널과 맞을 때만 수행하고, 이미 이전
    // 파일이면 아무것도 하지 않는다.
    void restore() {
        auto j = journal();
        regularOrAbsent(backup());
        require(hashFile(backup()) == j.oldHash, "Recovery backup is damaged");
        auto current = hashFile(m_target);
        require(current == j.newHash || current == j.oldHash, "Installed package changed; refusing rollback");
        if (current == j.oldHash) return;
        regularOrAbsent(m_next);
        copy(backup(), m_next);
        require(hashFile(m_next) == j.oldHash, "Recovery copy verification failed");
        distinctModificationTime();
        sync(m_next);
        replace(m_next, m_target);
        sync(m_target.parent_path(), true);
    }
};
using AndroidPackageTransaction = PackageTransaction;
}
