// 자체 업데이트의 핵심 로직(UI, 플랫폼과 무관): 매니페스트 검증, 다운로드 URL 허용 규칙,
// 이어받기가 되는 다운로더, 기존 패키지를 재사용하는 패치 계획(PatchPlan)과 검증.
// 예외는 require/requirePackage로 던지고, 해시 불일치는 VerifyError로 구분한다.
#pragma once
#include "Sha256.hpp"
#include <algorithm>
#include <charconv>
#include <functional>
#include <optional>
#include <regex>
#include <string_view>
#include <vector>

namespace versus::update {
namespace fs = std::filesystem;
// 공개 저장소의 GitHub Release 주소. 매니페스트와 패키지는 여기서만 받는다.
// 릴리스는 GitHub Actions가 공개된 소스 코드로 빌드해 올린다(.github/workflows/build.yml).
inline constexpr char RELEASES[] = "https://github.com/Rlaxogus123/Geode_Versus_Mode/releases";
// 버전 X.Y.Z 릴리스의 매니페스트 주소. 1.0.7: "어느 버전을 받을지"는 Versus 서버가 정한다(release.json의
// version = 서버가 받아 주는 버전). 전에는 GitHub의 "latest" 릴리스를 받았는데, 릴리스가 서버 전환보다 먼저
// 올라가면 게임이 새 버전으로 바뀐 뒤 서버에 거부당해(426) 업데이트 안내가 끝없이 반복됐다.
inline std::string manifestUrl(std::string const& version) {
    return std::string(RELEASES) + "/download/v" + version + "/manifest.json";
}
// 버전 X.Y.Z 릴리스의 통합 패키지 주소.
inline std::string packageUrl(std::string const& version) {
    return std::string(RELEASES) + "/download/v" + version + "/tipp7.versus.geode";
}
// 패키지 최대 크기(128MB)와 한 번에 요청하는 조각 크기(1MB).
inline constexpr uint64_t MAX_SIZE = 128 * 1024 * 1024, CHUNK_SIZE = 1024 * 1024;
// Android에서 자동 설치를 시도할지 여부.
inline constexpr bool ANDROID_AUTO_INSTALL = true;
// 조건이 거짓이면 예외를 던진다. 아래 예외 타입: PackageError(패키지 문제),
// VerifyError(크기/해시 검증 실패), PatchError(패치 실패 -> 전체 다운로드로 대체).
inline void require(bool ok, char const* error) {
    if (!ok) throw std::runtime_error(error);
}
struct PackageError : std::runtime_error {
    using std::runtime_error::runtime_error;
};
inline void requirePackage(bool ok, char const* error) {
    if (!ok) throw PackageError(error);
}
struct VerifyError : PackageError {
    using PackageError::PackageError;
};
struct PatchError : std::runtime_error {
    using std::runtime_error::runtime_error;
};
// 채널은 public 또는 member만 유효하다(member는 예전 설치 기록을 읽을 때만 쓰인다).
inline bool channelValid(std::string const& channel) {
    return channel == "public" || channel == "member";
}
inline bool sameRelease(std::string_view installed, std::string_view available) {
    return installed == available;
}
// 버전은 "숫자.숫자.숫자" 형식(각 9자리 이하, 앞자리 0 불가).
inline bool versionValid(std::string const& version) {
    static std::regex const pattern(R"((0|[1-9][0-9]{0,8})\.(0|[1-9][0-9]{0,8})\.(0|[1-9][0-9]{0,8}))");
    return std::regex_match(version, pattern);
}
// SHA-256 소문자 16진수 64자인지 확인한다.
inline bool digestValid(std::string const& hash) {
    return hash.size() == 64 && std::all_of(hash.begin(), hash.end(),
                                            [](char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); });
}
// 패키지 URL은 public 채널의, 그 버전 GitHub 릴리스의 통합 패키지 주소와 정확히 같아야 한다.
inline bool allowedUrl(std::string const& url, std::string const& channel, std::string const& version) {
    return channel == "public" && url == packageUrl(version);
}
// 지원하는 플랫폼 이름인지.
inline bool platformValid(std::string_view platform) {
    return platform == "windows" || platform == "android" || platform == "macos" || platform == "ios";
}
// 패키지 파일의 서버 상대 경로. 플랫폼이 비어 있으면 통합(unified) 패키지.
inline std::string packageSuffix(std::string const& version, std::string const& platform) {
    return "releases/" + version + "/tipp7.versus." + (platform.empty() ? std::string() : platform + ".") + "geode";
}
// 서버가 알려주는 업데이트 정보. 플랫폼별 패키지(url/sha256/size)와 통합
// 패키지(unified*)를 함께 가질 수 있다.
// identity()는 이어받기 중인 파일이 같은 릴리스의 것인지 구분하는 키.
struct Manifest {
    std::string version, url, sha256, channel;
    uint64_t size = 0;
    bool mandatory = true;
    std::string platform, unifiedUrl, unifiedSha256;
    uint64_t unifiedSize = 0;
    std::string identity() const { return channel + ":" + version + ":" + sha256 + ":" + std::to_string(size); }
    // 플랫폼별 정보를 통합 패키지 정보로 바꾼 사본.
    Manifest unified() const {
        Manifest m = *this;
        if (!platform.empty()) {
            m.url = unifiedUrl;
            m.sha256 = unifiedSha256;
            m.size = unifiedSize;
        }
        m.platform.clear();
        m.unifiedUrl.clear();
        m.unifiedSha256.clear();
        m.unifiedSize = 0;
        return m;
    }
};
// 매니페스트 필드를 검증한다: 버전, 해시, 크기 범위, 플랫폼, 허용된 URL.
class ManifestVerifier {
public:
    virtual ~ManifestVerifier() = default;
    virtual void validate(Manifest const& m) const {
        require(versionValid(m.version), "Invalid semantic version");
        require(digestValid(m.sha256), "Invalid SHA-256 manifest");
        require(m.size >= 1024 && m.size <= MAX_SIZE, "Invalid download size");
        // GitHub 릴리스는 통합 패키지 하나만 제공한다.
        require(m.platform.empty(), "Invalid platform package");
        require(allowedUrl(m.url, m.channel, m.version), "Untrusted download URL");
    }
};
// 패치 계획의 조각 하나. kind: 'c' = 설치된 패키지에서 복사(baseOffset부터),
// 'l' = 계획에 담긴 바이트(bytes), 'f' = 서버에서 받아야 하는 구간.
struct PatchSegment {
    char kind = 0;
    uint64_t length = 0, baseOffset = 0;
    std::vector<uint8_t> bytes;
};
// 설치된 패키지(base)를 재사용해 새 패키지를 만드는 계획. download()는 서버에서 받아야
// 하는 총 바이트 수.
struct PatchPlan {
    fs::path base;
    uint64_t baseSize = 0;
    std::vector<PatchSegment> segments;
    uint64_t downloadedBefore(uint64_t offset) const {
        uint64_t start = 0, total = 0;
        for (auto const& segment : segments) {
            if (start >= offset) break;
            if (segment.kind == 'f') total += std::min(segment.length, offset - start);
            start += segment.length;
        }
        return total;
    }
    uint64_t download() const { return downloadedBefore(UINT64_MAX); }
};
// 소문자 16진수 문자열을 바이트로 바꾼다. 형식이 틀리면 nullopt.
inline std::optional<std::vector<uint8_t>> hexBytes(std::string_view text) {
    if (text.empty() || text.size() % 2) return std::nullopt;
    auto digit = [](char c) -> int {
        return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1;
    };
    std::vector<uint8_t> bytes;
    bytes.reserve(text.size() / 2);
    for (size_t i = 0; i < text.size(); i += 2) {
        int high = digit(text[i]), low = digit(text[i + 1]);
        if (high < 0 || low < 0) return std::nullopt;
        bytes.push_back(static_cast<uint8_t>(high * 16 + low));
    }
    return bytes;
}
// 패치 계획이 안전한지 검증한다: 조각 수와 길이 한도, 복사 구간이 기존 패키지 안인지,
// 전체 길이가 매니페스트 크기와 정확히 같은지.
inline void validatePlan(PatchPlan const& plan, Manifest const& m) {
    require(!plan.segments.empty() && plan.segments.size() <= 8192, "Invalid patch plan");
    require(plan.baseSize >= 1024 && plan.baseSize <= MAX_SIZE, "Invalid patch base");
    uint64_t total = 0;
    for (auto const& segment : plan.segments) {
        require(segment.length > 0 && segment.length <= MAX_SIZE, "Invalid patch segment");
        if (segment.kind == 'c')
            require(segment.baseOffset <= plan.baseSize && segment.length <= plan.baseSize - segment.baseOffset,
                    "Patch copy outside the installed package");
        else if (segment.kind == 'l')
            require(segment.bytes.size() == segment.length, "Invalid patch bytes");
        else
            require(segment.kind == 'f', "Invalid patch segment");
        total += segment.length;
        require(total <= m.size, "Patch plan longer than the package");
    }
    require(total == m.size, "Patch plan does not cover the package");
}
// 경로가 없거나 일반 파일일 때만 통과한다(심볼릭 링크 등 거부).
inline void regularOrAbsent(fs::path const& path) {
    auto status = fs::symlink_status(path);
    require(status.type() == fs::file_type::not_found || status.type() == fs::file_type::regular,
            "Unsafe update file type");
}
// 파일 크기와 SHA-256이 매니페스트와 같은지 확인한다.
inline void verifyFile(fs::path const& path, Manifest const& manifest) {
    regularOrAbsent(path);
    if (!fs::is_regular_file(path) || fs::file_size(path) != manifest.size) throw VerifyError("Download size mismatch");
    if (hashFile(path) != manifest.sha256) throw VerifyError("SHA-256 mismatch");
}
// 다운로드 응답: HTTP 코드, Content-Range, 본문.
struct DownloadResponse {
    int code = 0;
    std::string contentRange;
    std::vector<uint8_t> data;
};
// 이어받기와 패치를 지원하는 다운로더. .part는 받는 중, .ready는 검증이 끝난 파일이다.
class UpdateDownloader {
    fs::path directory;
    void clear(fs::path const& file) {
        regularOrAbsent(file);
        fs::remove(file);
    }

public:
    explicit UpdateDownloader(fs::path root) : directory(std::move(root)) {}
    fs::path part() const { return directory / "tipp7.versus.geode.part"; }
    fs::path ready() const { return directory / "tipp7.versus.geode.ready"; }
    // 다운로드 준비. 매니페스트가 바뀌었으면 이전 파일을 지우고, 검증된 .ready가 있으면
    // 재사용한다.
    // 이어받을 위치(바이트)를 돌려주며 디스크 여유 공간을 확인한다.
    uint64_t prepare(Manifest const& m) {
        fs::create_directories(directory);
        require(!fs::is_symlink(directory), "Unsafe update directory");
        auto marker = directory / "download.identity";
        regularOrAbsent(marker);
        std::ifstream in(marker);
        std::string identity;
        std::getline(in, identity);
        in.close();
        if (identity != m.identity()) {
            clear(part());
            clear(ready());
            std::ofstream out(marker, std::ios::trunc);
            out << m.identity();
            out.flush();
            require(bool(out), "Cannot save download identity");
        }
        regularOrAbsent(ready());
        regularOrAbsent(part());
        if (fs::exists(ready())) {
            try {
                verifyFile(ready(), m);
                return m.size;
            } catch (std::exception const&) {
                clear(ready());
            }
        }
        auto size = fs::exists(part()) ? fs::file_size(part()) : 0;
        if (size > m.size) {
            clear(part());
            size = 0;
        }
        auto space = fs::space(directory);
        require(space.available >= m.size - size + 1024 * 1024, "Not enough disk space");
        return size;
    }
    // 패키지를 받는다. get(시작, 끝)이 범위 요청을 수행하며 1MB 조각 단위로 .part에 이어 쓴다.
    // 패치 계획이 있으면 복사/내장 구간은 로컬에서 채우고 'f' 구간만 서버에서 받는다.
    // 계획이 맞지 않거나 패치 결과가 틀리면 처음부터 전체를 받는 방식으로 되돌린다.
    fs::path download(Manifest const& m, std::function<DownloadResponse(uint64_t, uint64_t)> const& get,
                      std::function<void(uint64_t)> const& progress, std::function<bool()> const& canceled,
                      std::function<void()> const& verifying, PatchPlan const* plan = nullptr,
                      std::function<void(uint64_t)> const& unpatched = {}) {
        auto offset = prepare(m);
        progress(offset);
        if (fs::exists(ready())) {
            verifying();
            verifyFile(ready(), m);
            return ready();
        }
        bool const offered = plan != nullptr;
        // 계획이 검증에 실패하거나 기존 패키지 크기가 다르거나 열 수 없으면 패치를 쓰지 않는다.
        if (plan) {
            try {
                validatePlan(*plan, m);
            } catch (std::exception const&) {
                plan = nullptr;
            }
        }
        std::ifstream base;
        if (plan) {
            std::error_code error;
            if (fs::file_size(plan->base, error) != plan->baseSize || error)
                plan = nullptr;
            else {
                base.open(plan->base, std::ios::binary);
                if (!base) plan = nullptr;
            }
        }
        if (offered && !plan && unpatched) unpatched(offset);
        // 받을 위치(offset)가 속한 계획 조각을 찾아 로컬 조각이면 파일에서 복사해 채우고, 아니면
        // 범위 요청을 보낸다.
        while (true) {
            try {
                while (offset < m.size) {
                    require(!canceled(), "Download paused");
                    PatchSegment const* segment = nullptr;
                    uint64_t start = 0;
                    if (plan) {
                        for (auto const& candidate : plan->segments) {
                            if (offset < start + candidate.length) {
                                segment = &candidate;
                                break;
                            }
                            start += candidate.length;
                        }
                        if (!segment) throw PatchError("Patch plan ended early");
                    }
                    if (segment && segment->kind != 'f') {
                        auto const into = offset - start;
                        auto const count = std::min<uint64_t>(segment->length - into, CHUNK_SIZE);
                        std::vector<uint8_t> bytes(count);
                        if (segment->kind == 'c') {
                            base.clear();
                            base.seekg(static_cast<std::streamoff>(segment->baseOffset + into));
                            base.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(count));
                            if (!base || static_cast<uint64_t>(base.gcount()) != count)
                                throw PatchError("Cannot read the installed package");
                        } else
                            std::copy_n(segment->bytes.begin() + static_cast<std::ptrdiff_t>(into), count,
                                        bytes.begin());
                        regularOrAbsent(part());
                        std::ofstream out(part(), std::ios::binary | std::ios::app);
                        out.write(reinterpret_cast<char const*>(bytes.data()), bytes.size());
                        out.flush();
                        require(bool(out), "Cannot write download; check disk space");
                        out.close();
                        offset = fs::file_size(part());
                        progress(offset);
                        continue;
                    }
                    auto const limit = segment ? start + segment->length : m.size;
                    auto end = std::min(offset + CHUNK_SIZE, limit) - 1;
                    auto response = get(offset, end);
                    require(!canceled(), "Download paused");
                    // 200 응답은 전체 파일이어야 한다. 206은 Content-Range와 길이가 요청과 정확히 일치해야
                    // 한다.
                    bool full = response.code == 200;
                    if (full) {
                        require(response.data.size() == m.size, "Server ignored Range with an incomplete file");
                    } else {
                        if (response.code == 404 || response.code == 410)
                            throw PackageError("Download interrupted; retry to resume");
                        require(response.code == 206, "Download interrupted; retry to resume");
                        // Content-Range를 읽을 수 있으면 요청과 정확히 같아야 한다. 읽지 못해도 길이는 아래에서
                        // 확인하고, 다 받은 파일은 SHA-256으로 검증한다.
                        require(response.contentRange.empty() ||
                                    response.contentRange == "bytes " + std::to_string(offset) + "-" +
                                                                 std::to_string(end) + "/" + std::to_string(m.size),
                                "Invalid Content-Range");
                        require(response.data.size() == end - offset + 1, "Incomplete download chunk; retry to resume");
                    }
                    regularOrAbsent(part());
                    std::ofstream out(part(), std::ios::binary | (full ? std::ios::trunc : std::ios::app));
                    out.write(reinterpret_cast<char const*>(response.data.data()), response.data.size());
                    out.flush();
                    require(bool(out), "Cannot write download; check disk space");
                    out.close();
                    offset = fs::file_size(part());
                    progress(offset);
                }
                // 다 받으면 크기와 해시를 검증한다. 패치로 만든 파일이 틀리면 PatchError로 전체 받기에
                // 되돌아간다.
                verifying();
                try {
                    verifyFile(part(), m);
                } catch (PackageError const&) {
                    clear(part());
                    if (plan) throw PatchError("Patched package does not match");
                    throw;
                }
                break;
            } catch (PatchError const&) {
                clear(part());
                plan = nullptr;
                offset = 0;
                if (unpatched) unpatched(0);
                progress(offset);
            }
        }
        // 일시정지 요청이 없으면 .part를 .ready로 바꾼다.
        require(!canceled(), "Download paused");
        clear(ready());
        fs::rename(part(), ready());
        return ready();
    }
};
}
