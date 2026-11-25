// 플랫폼 설치기 구현. 받아 둔 .geode 패키지를 mods 폴더에 교체 설치하고 게임을 재시작한다.
// 설치 트랜잭션은 AndroidPackageTransaction.hpp(PackageTransaction)가, Windows 부팅 확인은
// WindowsUpdateReceipt.hpp가 맡는다.
#include "UpdateInstaller.hpp"
#include "AndroidPackageTransaction.hpp"
#include "WindowsUpdateReceipt.hpp"
#include "../Edition.hpp"
#include "../PathCompat.hpp"
#include <Geode/Geode.hpp>
#include <Geode/utils/file.hpp>
#include <Geode/utils/string.hpp>
#include <thread>
#include <chrono>
#include <fstream>
#ifdef GEODE_IS_WINDOWS
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif
using namespace geode::prelude;
namespace versus::update {
namespace {
// Geode가 풀어 둔 모드 리소스 캐시가 갱신되도록 modified-at 표지를 바꿔 둔다.
void invalidateExtraction() {
    std::error_code error;
    auto path = versus::stdPath(dirs::getModRuntimeDir() / Mod::get()->getID() / "modified-at");
    if (!fs::exists(path, error)) return;
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out << "versus-update";
}
// Patchless 로더에서 모드 바이너리를 따로 교체하기 위한 도우미.
// stage는 패키지에서 바이너리를 임시 이름으로 꺼내고, commit은 이름을 바꿔 교체하고,
// discard는 임시 파일을 지운다.
class PatchlessBinary {
    fs::path m_final, m_next;

public:
    PatchlessBinary() {
        std::string name(Mod::get()->getMetadata().getBinaryName());
        require(!name.empty() && name.find_first_of("/\\") == std::string::npos, "Unknown platform binary");
        m_final = versus::stdPath(dirs::getModRuntimeDir() / "binaries" / name);
        m_next = m_final;
        m_next += ".versus-next";
    }
    bool present() const {
        std::error_code error;
        return fs::is_regular_file(m_final, error);
    }
    void stage(fs::path const& package) {
        regularOrAbsent(m_final);
        regularOrAbsent(m_next);
        auto opened = file::Unzip::create(versus::geodePath(package));
        require(opened.isOk(), "Cannot open the update package");
        auto& zip = opened.unwrap();
        auto const name = versus::pathText(m_final.filename());
        require(zip.hasEntry(name), "This update does not support your platform");
        auto written = zip.extractTo(name, versus::geodePath(m_next));
        if (!written) throw std::runtime_error("Cannot extract the update binary: " + written.unwrapErr());
        std::error_code error;
        require(fs::is_regular_file(m_next, error) && fs::file_size(m_next, error) > 0,
                "Update binary extraction failed");
    }
    void commit() {
        regularOrAbsent(m_final);
        fs::rename(m_next, m_final);
    }
    void discard() noexcept {
        std::error_code error;
        fs::remove(m_next, error);
    }
};
// 패키지 교체 방식의 설치기. Windows는 항상 자동 설치, 그 외 플랫폼은 ANDROID_AUTO_INSTALL
// 설정을 따른다.
class PackageUpdateInstaller final : public UpdateInstaller {
public:
    bool automatic() const override {
#ifdef GEODE_IS_WINDOWS
        return true;
#else
        return ANDROID_AUTO_INSTALL;
#endif
    }
    // 트랜잭션으로 패키지를 교체한다. 이전 설치가 미완료였는지(settlePrevious)에 따라 새로
    // 준비하거나 건너뛴다.
    // Patchless 로더에서는 바이너리도 함께 교체하고, 실패하면 임시 바이너리를 지운다.
    void prepare(InstallContext const& c) override {
        require(automatic(), "This launcher requires manual Geode installation");
        PackageTransaction transaction(c.directory, c.package, c.modsDirectory);
        auto version = Mod::get()->getVersion().toNonVString();
        auto const ready = c.directory / "tipp7.versus.geode.ready";
        bool const settled = transaction.settlePrevious(version, c.manifest.sha256);
        if (Loader::get()->isPatchless()) {
            PatchlessBinary binary;
            if (!settled) {
                binary.stage(ready);
                try {
                    transaction.prepare(ready, c.manifest);
                } catch (...) {
                    binary.discard();
                    throw;
                }
                binary.commit();
            } else if (!binary.present()) {
                binary.stage(c.package);
                binary.commit();
            }
        } else if (!settled)
            transaction.prepare(ready, c.manifest);
        invalidateExtraction();
    }
    // 게임을 재시작한다. Windows는 업데이터가 있으면 재시작, 없으면 종료한다.
    // 그 외 플랫폼에서 재시작이 일어나지 않을 경우를 대비해 6초 뒤 종료한다(iOS 제외).
    void restart() override {
#ifdef GEODE_IS_WINDOWS
        if (canRestart())
            utils::game::restart(true);
        else
            utils::game::exit(true);
#elif defined(GEODE_IS_IOS)
        utils::game::restart(true);
#else
        utils::game::restart(true);
        std::thread([] {
            std::this_thread::sleep_for(std::chrono::seconds(6));
            queueInMainThread([] { utils::game::exit(false); });
        }).detach();
#endif
    }
};
}
// 재시작 가능 여부. Windows는 GeodeUpdater.exe가 게임 폴더에 있을 때만 가능하다.
bool canRestart() {
#ifdef GEODE_IS_WINDOWS
    std::error_code error;
    return fs::is_regular_file(versus::stdPath(dirs::getGameDir() / "GeodeUpdater.exe"), error);
#else
    return true;
#endif
}
// 현재 플랫폼의 설치기.
std::unique_ptr<UpdateInstaller> platformInstaller() {
    return std::make_unique<PackageUpdateInstaller>();
}
// Android 설치 작업이 남아 있으면 한 번만 정리(reconcile)한다. 실패하면 작업 기록을
// 지운다.
namespace {
void reconcilePackageTransaction() {
    static bool attempted = false;
    if (attempted) return;
    attempted = true;
    try {
        auto* mod = Mod::get();
        auto root = versus::stdPath(mod->getSaveDir() / "updater");
        if (!fs::exists(root / "android-install.job")) return;
        auto target = versus::stdPath(mod->getPackagePath()), mods = versus::stdPath(dirs::getModsDir());
        auto version = mod->getVersion().toNonVString();
        if (PackageTransaction(root, target, mods).reconcile(version)) {
            invalidateExtraction();
            log::warn("Versus update installed but not loaded yet; it loads on the next launch");
        }
    } catch (std::exception const& error) {
        log::warn("Android update reconcile failed: {}", error.what());
        std::error_code ignored;
        auto root = versus::stdPath(Mod::get()->getSaveDir() / "updater");
        fs::remove(root / "android-install.job", ignored);
        fs::remove(root / "android-boot-confirmed.txt", ignored);
    }
}
}
// 업데이트 후 첫 부팅이 정상적으로 시작되었음을 기록한다.
// Windows에서는 install.job의 내용(형식, 버전, 채널, 패키지 경로, 작업 폴더)이 현재 설치와
// 일치할 때만 부팅 확인 영수증을 쓰며, 이후 크래시로 롤백되지 않게 한다.
void confirmUpdateBoot() {
    reconcilePackageTransaction();
#ifdef GEODE_IS_WINDOWS
    static std::string confirmedNonce;
    try {
        auto root = versus::stdPath(Mod::get()->getSaveDir() / "updater");
        auto path = root / "install.job";
        if (!fs::is_regular_file(path) || fs::file_size(path) > 16384) return;
        std::ifstream in(path);
        std::string line;
        std::vector<std::string> rows;
        while (std::getline(in, line)) {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            rows.push_back(line);
        }
        if (rows.size() != 12 || rows[0] != "VERSUS-UPDATE-1" || !digestValid(rows[8])) return;
        if (rows[3] != Mod::get()->getVersion().toNonVString()) return;
        if (rows[5] != "member" && rows[5] != "public") return;
        if (rows[8] == confirmedNonce) return;
        if (fs::weakly_canonical(fs::u8path(rows[1])) != fs::weakly_canonical(versus::stdPath(Mod::get()->getPackagePath()))) return;
        if (fs::weakly_canonical(fs::u8path(rows[11])) != fs::weakly_canonical(root)) return;
        writeWindowsBootReceipt(root, rows[8]);
        confirmedNonce = rows[8];
        log::info("Updater startup confirmed; later gameplay crashes will not roll back this update");
    } catch (std::exception const&) {
        log::warn("Updater startup receipt unavailable");
    }
#endif
}
}
