// UpdateManager 구현: 업데이트 서버에서 매니페스트를 확인하고, 패키지를 받아 검증한 뒤
// 플랫폼 설치기로 설치/재시작하는 흐름. 네트워크 작업은 별도 스레드에서 하고 상태는
// mutex로 갱신한다.
#include "UpdateManager.hpp"
#include "../Edition.hpp"
#include "../DebugMode.hpp"
#include "../FirebaseConfig.hpp"
#include <Geode/binding/GJAccountManager.hpp>
#include "UpdateIntegration.hpp"
#include <Geode/Geode.hpp>
#include <Geode/utils/file.hpp>
#include <Geode/utils/web.hpp>
#include <Geode/utils/string.hpp>
#include <thread>
#include <atomic>
#include <chrono>
#include <memory>
#ifdef GEODE_IS_WINDOWS
#include <windows.h>
#endif

using namespace geode::prelude;
namespace versus::update {
namespace {
// 공통 요청 설정: 20초 타임아웃, 인증서 검증, 압축 없음, 캐시 방지.
// GitHub 릴리스 파일은 GitHub의 다운로드 서버로 리다이렉트되므로 리다이렉트를 따라간다.
// 받은 파일은 매니페스트의 크기와 SHA-256으로 검증한다.
web::WebRequest request() {
    auto req = web::WebRequest();
    req.timeout(std::chrono::seconds(20));
    req.certVerification(true);
    req.followRedirects(true);
    req.acceptEncoding("identity");
    req.header("Cache-Control", "no-cache");
    return req;
}
// 일시적 오류(응답 없음, 408, 429, 5xx)면 최대 2번 더 시도한다(대기 0.7초, 1.4초).
// 취소 요청이 있으면 재시도하지 않는다.
template <class Send> web::WebResponse withRetry(Send&& send, std::atomic<bool> const* cancel = nullptr) {
    for (int attempt = 0;; ++attempt) {
        auto response = send();
        int const code = response.code();
        bool const transient = code <= 0 || code == 408 || code == 429 || code >= 500;
        if (!transient || attempt >= 2 || (cancel && cancel->load())) return response;
        std::this_thread::sleep_for(std::chrono::milliseconds(700 * (attempt + 1)));
    }
}
// 응답이 200이고 16KB 이하의 JSON 객체인지 확인해 반환한다.
matjson::Value json(web::WebResponse const& response) {
    require(response.code() == 200, "Update server unavailable. Retry; your installed version is unchanged.");
    require(response.data().size() <= 16384, "Update manifest is too large");
    auto parsed = response.json();
    require(parsed.isOk() && parsed.unwrap().isObject(), "Invalid update manifest");
    return parsed.unwrap();
}
// JSON 객체에서 문자열 필드를 꺼낸다. 없으면 예외.
std::string string(matjson::Value const& value, char const* key) {
    auto field = value.get(key);
    require(field.isOk() && field.unwrap().isString(), "Missing manifest field");
    return field.unwrap().asString().unwrap();
}
// 1.0.7: Versus 서버가 받아 주는 릴리스(release.json의 version, "v1.0.7" 꼴 → "1.0.7"). 서버에 닿지 못하면
// (점검 중 503, 네트워크) 빈 문자열: 그때는 확인 없이 Versus로 들어가고, 서버가 다시 답하면 그 화면에서
// 업데이트 안내(X-Versus-Latest / 426)가 뜬다.
std::string fetchServerRelease() {
    auto const url = fmt::format("{}/{}/release.json", firebase_config::DATABASE_URL, debug::apiRoot());
    auto response = withRetry([&] {
        auto req = request();
        req.timeout(std::chrono::seconds(8));
        return req.getSync(url);
    });
    if (response.code() != 200 || response.data().size() > 4096) return {};
    auto parsed = response.json();
    if (!parsed.isOk() || !parsed.unwrap().isObject()) return {};
    auto field = parsed.unwrap().get("version");
    if (!field.isOk() || !field.unwrap().isString()) return {};
    auto version = field.unwrap().asString().unwrap();
    if (!version.empty() && version.front() == 'v') version.erase(0, 1);
    return versionValid(version) ? version : std::string();
}
// 서버가 정한 버전의 GitHub 릴리스 매니페스트를 받아 검증한다.
// modId, 채널(public), 스키마(1), 크기 범위, 정책 필드, 패키지 주소, 그리고 그 버전인지 확인한다.
Manifest fetchManifest(std::string const& version) {
    auto response = withRetry([&] { return request().getSync(manifestUrl(version)); });
    require(response.code() != 404,
            fmt::format("The server runs Versus v{}, but that release is not on GitHub yet. Retry later.", version)
                .c_str());
    auto value = json(response);
    std::string const channel = "public";
    require(string(value, "modId") == "tipp7.versus" && string(value, "channel") == channel,
            "Manifest identity mismatch");
    auto schema = value.get("schema"), size = value.get("size"), mandatory = value.get("mandatory");
    require(schema.isOk() && schema.unwrap().asInt().unwrapOr(0) == 1, "Unsupported update schema");
    require(size.isOk() && size.unwrap().isNumber(), "Missing download size");
    auto number = size.unwrap().asDouble().unwrapOr(0);
    require(number >= 1024 && number <= MAX_SIZE && std::floor(number) == number, "Invalid download size");
    require(mandatory.isOk() && mandatory.unwrap().isBool(), "Invalid update policy");
    Manifest m{string(value, "version"),      string(value, "url"),
               string(value, "sha256"),       channel,
               static_cast<uint64_t>(number), mandatory.unwrap().asBool().unwrap()};
    require(m.version == version, "Manifest version mismatch");
    ManifestVerifier().validate(m);
    return m;
}
}
// 싱글턴. 프로세스 끝까지 유지하기 위해 해제하지 않는다.
UpdateManager& UpdateManager::get() {
    static auto* instance = new UpdateManager;
    return *instance;
}
UpdateSnapshot UpdateManager::snapshot() {
    std::lock_guard lock(m_mutex);
    return m_snapshot;
}
void UpdateManager::state(UpdateState value, std::string message) {
    std::lock_guard lock(m_mutex);
    m_snapshot.state = value;
    m_snapshot.message = std::move(message);
}
void UpdateManager::fail(std::string message) {
    std::lock_guard lock(m_mutex);
    m_snapshot.state = UpdateState::Error;
    m_snapshot.message = std::move(message);
    m_snapshot.busy = false;
}
// 업데이트 확인을 시작한다. 이미 진행 중이면 무시한다.
// 설치된 버전/채널과 제작자 계정 여부를 기록한 뒤 인증을 요청한다.
void UpdateManager::check() {
    {
        std::lock_guard lock(m_mutex);
        if (m_snapshot.busy) return;
        m_snapshot.busy = true;
        m_cancel = false;
        m_snapshot.state = UpdateState::Checking;
        m_snapshot.newerThanServer = false;
        m_snapshot.message = "Checking server version...";
    }
    try {
        auto* mod = Mod::get();
        m_context.directory = mod->getSaveDir() / "updater";
        regularOrAbsent(mod->getPackagePath());
        auto* account = GJAccountManager::sharedState();
        m_creator = account && (debug::debugAccount(std::string(account->m_username), account->m_accountID) ||
                                debug::listedAccount(mod->getSavedValue<std::string>(debug::kStaffAccountsKey, ""),
                                                     account->m_accountID));
        m_context.package = fs::weakly_canonical(mod->getPackagePath());
        m_context.modsDirectory = dirs::getModsDir();
        {
            std::lock_guard lock(m_mutex);
            m_snapshot.installedVersion = mod->getVersion().toNonVString();
            m_snapshot.installedChannel = isMembershipEdition() ? "member" : "public";
        }
        authorize();
    } catch (std::exception const& error) {
        fail(error.what());
    }
}
// 백그라운드 스레드에서 performCheck를 실행한다 (GitHub 릴리스 확인에는 로그인이 필요 없다).
void UpdateManager::authorize() {
    if (m_cancel) {
        fail("Update check canceled");
        return;
    }
    try {
        std::thread([this] {
            try {
                performCheck();
            } catch (std::exception const& error) {
                fail(error.what());
            }
        }).detach();
    } catch (std::exception const& error) {
        fail(error.what());
    }
}
// 1.0.7: 서버가 받아 주는 버전을 먼저 묻고, 설치된 버전과 비교해 상태를 정한다.
// 같으면 GitHub를 보지 않고 Current(바로 입장). 서버 버전이 더 새로우면 그 버전의 매니페스트를 받아
// UpdateAvailable. 서버 버전이 더 낮으면 오류(제작자 계정의 디버그 빌드는 예외). 서버에 닿지 못하면 Current로
// 두어 Versus 화면이 점검 안내를 보여 주게 한다.
void UpdateManager::performCheck() {
    auto const server = fetchServerRelease();
    require(!m_cancel, "Update check canceled");
    auto old = snapshot();
    if (server.empty()) {
        std::lock_guard lock(m_mutex);
        m_snapshot.manifest = Manifest{};
        m_snapshot.busy = false;
        m_snapshot.state = UpdateState::Current;
        m_snapshot.message = "Versus server not reachable; continuing";
        return;
    }
    auto latest = VersionInfo::parse(server), current = VersionInfo::parse(old.installedVersion);
    require(latest.isOk() && current.isOk(), "Invalid release version");
    bool const debugBuild = m_creator && latest.unwrap() < current.unwrap();
    {
        std::lock_guard lock(m_mutex);
        m_snapshot.newerThanServer = latest.unwrap() < current.unwrap();
    }
    require(debugBuild || current.unwrap() <= latest.unwrap(),
            "Server release is older than this installation. Please retry later.");
    if (debugBuild || sameRelease(old.installedVersion, server)) {
        std::lock_guard lock(m_mutex);
        m_snapshot.manifest = Manifest{};
        m_snapshot.manifest.version = server;
        m_snapshot.busy = false;
        m_snapshot.state = UpdateState::Current;
        m_snapshot.message = debugBuild ? "Debug build (newer than the server)" : "Up to date";
        return;
    }
    auto manifest = fetchManifest(server);
    require(!m_cancel, "Update check canceled");
    std::lock_guard lock(m_mutex);
    // 플랫폼 패키지가 쓸 수 없다고 판명된 버전은 통합 패키지로 바꾼다.
    if (!manifest.platform.empty() && manifest.version == m_unifiedOnly) manifest = manifest.unified();
    m_snapshot.manifest = std::move(manifest);
    m_snapshot.downloaded = 0;
    m_snapshot.bytesPerSecond = 0;
    m_snapshot.busy = false;
    m_snapshot.transferTotal = 0;
    m_snapshot.transferDone = 0;
    bool same = debugBuild || sameRelease(old.installedVersion, m_snapshot.manifest.version);
    m_snapshot.state = same ? UpdateState::Current : UpdateState::UpdateAvailable;
    m_snapshot.message = debugBuild ? "Debug build (newer than the server)"
                         : same     ? "Up to date"
                                    : "Update required to enter Versus";
}
// 받은 파일을 검증한다: 크기와 해시, Geode 패키지 형식, 모드 ID와 버전, 플랫폼 바이너리
// 포함 여부,
// 요구하는 Geode 로더 버전.
void UpdateManager::validateArchive(fs::path const& path, Manifest const& manifest) {
    verifyFile(path, manifest);
    auto meta = ModMetadata::createFromGeodeFile(path);
    requirePackage(!meta.hasErrors(), "Downloaded file is not a valid Geode package");
    requirePackage(meta.getID() == "tipp7.versus" && meta.getVersion().toNonVString() == manifest.version,
                   "Package version mismatch");
    auto zip = file::Unzip::create(path);
    requirePackage(zip.isOk(), "Cannot open downloaded package");
    requirePackage(zip.unwrap().hasEntry(std::string(meta.getBinaryName())),
                   "This update does not support your platform");
    auto* loader = Loader::get()->getLoadedMod("geode.loader");
    require(loader && loader->getVersion() >= meta.getGeodeVersion(),
            "Update Geode first; this package requires a newer loader");
}
// 설치된 패키지를 재사용하는 패치 계획을 서버에서 받는다. 설치된 패키지의 해시(캐시)를
// 키로 요청하며,
// 실패하거나 계획이 없으면 nullopt를 반환해 전체 다운로드로 진행한다.
std::optional<PatchPlan> UpdateManager::loadPatch(Manifest const& manifest) {
    try {
        auto const base = m_context.package;
        std::error_code error;
        if (fs::symlink_status(base, error).type() != fs::file_type::regular || error) return std::nullopt;
        auto const size = fs::file_size(base, error);
        if (error || size < 1024 || size > MAX_SIZE) return std::nullopt;
        auto const stamp = fs::last_write_time(base, error);
        if (error) return std::nullopt;
        if (m_baseDigest.empty() || m_baseSize != size || m_baseTime != stamp) {
            m_baseDigest.clear();
            auto digest = hashFile(base);
            m_baseDigest = std::move(digest);
            m_baseSize = size;
            m_baseTime = stamp;
        }
        auto const url = patchUrl(manifest, m_baseDigest);
        if (url.empty() || m_cancel) return std::nullopt;
        auto send = [&] {
            auto req = request();
            req.timeout(std::chrono::seconds(6));
            return req.getSync(url);
        };
        auto response = send();
        int const code = response.code();
        if ((code <= 0 || code == 408 || code == 429 || code >= 500) && !m_cancel) {
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
            response = send();
        }
        if (response.code() != 200 || response.data().size() > MAX_PLAN_SIZE) return std::nullopt;
        auto parsed = response.json();
        if (!parsed.isOk()) return std::nullopt;
        auto plan = parsePatchPlan(parsed.unwrap(), manifest, base, size, m_baseDigest);
        if (plan)
            log::info("Versus update: {} {} patched from the installed package ({} of {} bytes downloaded)",
                      manifest.version, manifest.platform, plan->download(), manifest.size);
        return plan;
    } catch (std::exception const& error) {
        log::info("Versus update: no patch plan ({})", error.what());
        return std::nullopt;
    }
}
// 패키지를 받는다. 이어받기 위치와 패치 계획을 정하고, 진행률(바이트, 속도)을 갱신하며
// 1MB 범위 요청으로 내려받는다. 패치가 실패하면 진행률 기준을 전체 크기로 되돌린다.
fs::path UpdateManager::fetchPackage(Manifest const& manifest) {
    UpdateDownloader downloader(m_context.directory);
    uint64_t const baseline = downloader.prepare(manifest);
    std::shared_ptr<PatchPlan const> plan;
    if (!manifest.platform.empty() && baseline < manifest.size) {
        if (auto loaded = loadPatch(manifest)) plan = std::make_shared<PatchPlan const>(std::move(*loaded));
    }
    auto patched = std::make_shared<std::atomic<bool>>(plan != nullptr);
    auto first = std::make_shared<std::atomic<uint64_t>>(plan ? plan->downloadedBefore(baseline)
                                                              : std::min(baseline, manifest.size));
    auto const started = std::chrono::steady_clock::now();
    auto progress = [this, started, first, patched, plan, manifest](uint64_t bytes) {
        bytes = std::min(bytes, manifest.size);
        bool const viaPlan = patched->load() && plan;
        auto const total = viaPlan ? plan->download() : manifest.size;
        auto const done = std::min(viaPlan ? plan->downloadedBefore(bytes) : bytes, total);
        auto const from = first->load();
        double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
        std::lock_guard lock(m_mutex);
        m_snapshot.downloaded = bytes;
        m_snapshot.transferTotal = total;
        m_snapshot.transferDone = done;
        m_snapshot.bytesPerSecond = seconds > .1 && done > from ? (done - from) / seconds : 0;
        if (m_snapshot.state == UpdateState::Verifying && bytes < manifest.size) {
            m_snapshot.state = UpdateState::Downloading;
            m_snapshot.message = "Downloading...";
        }
    };
    return downloader.download(
        manifest,
        [&](uint64_t start, uint64_t end) {
            auto response = withRetry(
                [&] {
                    auto req = request();
                    req.downloadRange({start, end});
                    req.onProgress([=](web::WebProgress const& p) {
                        progress(start + std::min<uint64_t>(p.downloaded(), end - start + 1));
                    });
                    return req.getSync(manifest.url);
                },
                &m_cancel);
            DownloadResponse result;
            result.code = response.code();
            // 헤더 이름은 대소문자를 구분하지 않는다(HTTP/2는 소문자로 온다). 리다이렉트를 따라가면
            // 응답 헤더가 여러 단계 쌓이므로 마지막(실제 파일 서버) 값을 쓴다.
            for (char const* name : {"Content-Range", "content-range", "CONTENT-RANGE"}) {
                if (auto all = response.getAllHeadersNamed(name); all && !all->empty()) {
                    result.contentRange = all->back();
                    break;
                }
            }
            result.data = std::move(response).data();
            return result;
        },
        progress, [this] { return m_cancel.load(); },
        [this] { state(UpdateState::Verifying, "Verifying SHA-256 and package..."); }, plan.get(),
        [patched, first](uint64_t from) {
            patched->store(false);
            first->store(from);
        });
}
// 다운로드를 시작한다(UpdateAvailable일 때만). 백그라운드에서 받고 검증한다.
// 플랫폼 패키지가 검증에 실패하면 한 번 더 받고, 그래도 쓸 수 없으면 통합 패키지로 바꿔
// 다시 받는다.
void UpdateManager::download() {
    Manifest manifest;
    {
        std::lock_guard lock(m_mutex);
        if (m_snapshot.busy || m_snapshot.state != UpdateState::UpdateAvailable) return;
        m_snapshot.busy = true;
        m_cancel = false;
        m_snapshot.state = UpdateState::Downloading;
        m_snapshot.message = "Downloading...";
        manifest = m_snapshot.manifest;
    }
    try {
        std::thread([this, manifest]() mutable {
            try {
                fs::path ready;
                auto fetch = [&] {
                    ready = fetchPackage(manifest);
                    validateArchive(ready, manifest);
                };
                try {
                    try {
                        fetch();
                    } catch (VerifyError const& error) {
                        if (manifest.platform.empty()) throw;
                        log::warn("Versus update: {} package did not verify ({}); downloading it again",
                                  manifest.platform, error.what());
                        fetch();
                    }
                } catch (PackageError const& error) {
                    if (manifest.platform.empty()) throw;
                    log::warn("Versus update: {} package unusable ({}); downloading the full package",
                              manifest.platform, error.what());
                    manifest = manifest.unified();
                    {
                        std::lock_guard lock(m_mutex);
                        m_unifiedOnly = manifest.version;
                        m_snapshot.manifest = manifest;
                        m_snapshot.downloaded = 0;
                        m_snapshot.transferTotal = 0;
                        m_snapshot.transferDone = 0;
                        m_snapshot.bytesPerSecond = 0;
                        m_snapshot.state = UpdateState::Downloading;
                        m_snapshot.message = "Downloading...";
                    }
                    fetch();
                }
                // 자동 설치가 안 되는 플랫폼은 받은 파일을 수동 설치용 이름으로 복사해 둔다.
                auto automatic = platformInstaller()->automatic();
                if (!automatic) {
                    auto manual =
                        m_context.directory / ("tipp7.versus-" + manifest.version + "-" + manifest.channel + ".geode");
                    regularOrAbsent(manual);
                    fs::copy_file(ready, manual, fs::copy_options::overwrite_existing);
                    verifyFile(manual, manifest);
                    ready = manual;
                }
                std::lock_guard lock(m_mutex);
                m_snapshot.busy = false;
                m_snapshot.downloaded = manifest.size;
                m_snapshot.downloadedPath = utils::string::pathToString(ready);
                m_snapshot.state = automatic ? UpdateState::ReadyToInstall : UpdateState::VerifiedDownloaded;
                m_snapshot.message =
                    automatic ? "최신버전 다운로드 완료!\n업데이트 적용을 위해 Geometry Dash를 재시작해야 합니다."
                              : "최신버전 다운로드 완료!\n이 실행 환경에서는 자동 설치가 지원되지 않습니다.";
            } catch (std::exception const& error) {
                fail(error.what());
            }
        }).detach();
    } catch (std::exception const& error) {
        fail(error.what());
    }
}
// 설치한다. 대전 중이면 거부한다. 아직 보내지 못한 기록이 있으면 최대 8초 기다렸다가
// (그래도 남아 있으면 그대로) 진행한다. 설치 직전에 패키지를 다시 검증하고,
// 설치기의 prepare 후 메인 스레드에서 재시작한다.
void UpdateManager::install() {
    if (matchInProgress()) {
        {
            std::lock_guard lock(m_mutex);
            m_installAfterRecords = false;
        }
        fail("Finish the current match first. Your result and rating are saved before updating.");
        return;
    }
    Manifest manifest;
    {
        std::lock_guard lock(m_mutex);
        bool const resuming = m_installAfterRecords && m_snapshot.state == UpdateState::Installing;
        if (m_snapshot.busy || (m_snapshot.state != UpdateState::ReadyToInstall && !resuming) ||
            !platformInstaller()->automatic()) {
            m_installAfterRecords = false;
            return;
        }
        if (recordsPending()) {
            auto const now = std::chrono::steady_clock::now();
            if (!m_installAfterRecords) {
                m_installAfterRecords = true;
                m_recordsSince = now;
            }
            if (now - m_recordsSince < std::chrono::seconds(8)) {
                m_snapshot.state = UpdateState::Installing;
                m_snapshot.message = "Saving your match records before updating...";
                return;
            }
            log::warn("Versus update: match records still pending after 8 s; installing anyway");
        }
        m_installAfterRecords = false;
        m_snapshot.busy = true;
        m_cancel = false;
        m_snapshot.state = UpdateState::Installing;
        m_snapshot.message = "Installing the update...";
        manifest = m_snapshot.manifest;
    }
    try {
        std::thread([this, manifest] {
            try {
                m_context.manifest = manifest;
                validateArchive(m_context.directory / "tipp7.versus.geode.ready", manifest);
                platformInstaller()->prepare(m_context);
                log::info("Versus update {} installed; restarting", manifest.version);
#ifdef GEODE_IS_ANDROID
                state(
                    UpdateState::Restarting,
                    "업데이트 설치 완료! Geometry Dash를 다시 시작합니다.\n자동으로 다시 열리지 않으면 게임을 직접 다시 실행하세요.");
#elif defined(GEODE_IS_IOS)
                state(UpdateState::Restarting,
                      "업데이트 설치 완료! Geode 런처가 게임을 다시 시작합니다.\n"
                      "자동으로 다시 열리지 않으면 게임을 완전히 종료한 뒤 Geode 런처에서 다시 실행하세요.");
#else
                state(
                    UpdateState::Restarting,
                    canRestart()
                        ? "업데이트 설치 완료! Geometry Dash를 다시 시작합니다."
                        : "업데이트 설치 완료! Geometry Dash가 종료됩니다.\n게임을 다시 실행하면 새 버전이 적용됩니다.");
#endif
                queueInMainThread([] { platformInstaller()->restart(); });
            } catch (std::exception const& error) {
                log::warn("Versus update install failed: {}", error.what());
                fail(std::string("업데이트 적용 실패: ") + error.what() +
                     "\n현재 버전은 그대로입니다. Retry를 눌러 다시 시도하세요.");
            }
        }).detach();
    } catch (std::exception const& error) {
        fail(error.what());
    }
}
// 기록 전송을 기다리는 중인지.
bool UpdateManager::waitingForRecords() {
    std::lock_guard lock(m_mutex);
    return m_installAfterRecords;
}
// 진행 중인 다운로드를 일시정지(취소)한다.
void UpdateManager::pause() {
    m_cancel = true;
}
// Android/iOS에서 재시작이 일어나지 않을 때 상태를 ReadyToInstall로 되돌려 다시 시도하게
// 한다.
void UpdateManager::restartTimeout() {
#if defined(GEODE_IS_ANDROID) || defined(GEODE_IS_IOS)
    std::lock_guard lock(m_mutex);
    if (m_snapshot.state != UpdateState::Restarting) return;
    m_snapshot.state = UpdateState::ReadyToInstall;
    m_snapshot.busy = false;
    m_snapshot.message = "If the launcher did not restart, retry Restart or close and reopen Geometry Dash"
#ifdef GEODE_IS_IOS
                         " from the Geode launcher"
#endif
                         ".\nThe verified package and recovery backup are saved.";
#endif
}
}
