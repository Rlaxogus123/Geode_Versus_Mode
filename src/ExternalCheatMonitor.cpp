// 대전 중 다른 모드(Eclipse, Mega Hack, QOLMod, Prism Menu)와 금지 모드를 검사해
// 치트가 켜져 있는지 판정하는 모듈. 각 모드의 저장 설정, 런타임 API, 훅에서 관측한
// 흔적을 합쳐 ExternalCheatStatus로 돌려준다. 플랫폼별(Windows/Android/macOS/iOS) 처리가
// 섞여 있다.
#include "ExternalCheatMonitor.hpp"
#include "IntegrityCodes.hpp"
#include "IntegrityFeatures.hpp"
#include "MegaHackRules.hpp"
#include "PrismRules.hpp"
#include "VersusService.hpp"
#include "compat/Compat.hpp"
#include "compat/Eclipse.hpp"

#include <Geode/Geode.hpp>
#include <Geode/binding/PlayLayer.hpp>
#include <Geode/utils/file.hpp>

#include <array>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <optional>
#include <unordered_map>

#ifdef GEODE_IS_WINDOWS
#include <Windows.h>
#endif
#if defined(GEODE_IS_ANDROID) || defined(GEODE_IS_MACOS) || defined(GEODE_IS_IOS)
#include <cxxabi.h>
#include <cstdlib>
#include <cstring>
#endif
#ifdef GEODE_IS_ANDROID
#include <link.h>
#include <fstream>
#elif defined(GEODE_IS_MACOS) || defined(GEODE_IS_IOS)
#include <mach-o/dyld.h>
#include <mach-o/loader.h>
#include <mach-o/nlist.h>
#endif

using namespace geode::prelude;

namespace versus::integrity {
namespace {

// 아래 thread_local 깊이 카운터와 관측 시각은 각 모드의 훅이 begin/end 쌍으로 갱신한다.
// 훅 호출이 중첩되어도 가장 바깥 범위에서만 상태를 초기화하기 위한 값이다.
// Prism 쪽 관측 기록은 PlayLayer 소유자와 마지막 관측 시각을 함께 들고 있다.
using RuntimeClock = std::chrono::steady_clock;
thread_local unsigned s_prismProcessDepth = 0;
thread_local unsigned s_nativeProcessDepth = 0;
thread_local unsigned s_prismInitDepth = 0;
thread_local bool s_prismInitBaseReturned = false;
thread_local unsigned s_prismSchedulerDepth = 0;
thread_local cocos2d::CCScheduler* s_prismScheduler = nullptr;
thread_local double s_prismSchedulerInput = 0.;
PlayLayer* s_prismPlaybackOwner = nullptr;
RuntimeClock::time_point s_prismPlaybackObserved{};
PlayLayer* s_prismInstantOwner = nullptr;
RuntimeClock::time_point s_prismInstantObserved{};
RuntimeClock::time_point s_prismSpeedObserved{};
thread_local unsigned s_megaHackPostDepth = 0;
thread_local bool s_megaHackPostBaseReturned = false;
PlayLayer* s_megaHackInstantOwner = nullptr;
RuntimeClock::time_point s_megaHackInstantObserved{};
// Mega Hack의 Instant Complete 기능 ID.
constexpr std::string_view kMegaHackInstantComplete = "LEVEL/HACK_INSTANT_COMPLETE";
// Mega Hack은 GD가 종료될 때 설정을 저장하므로, 저장 파일 기준 판정 시 안내로 덧붙이는
// 문구.
constexpr char const* kMegaHackSavedHint =
    "Mega Hack saves its settings when GD closes: after turning the hack off, restart GD.";

// 감지된 기능 키 목록을 인코딩해 ExternalCheatStatus로 만든다. 키가 없으면 미감지(빈
// 상태).
ExternalCheatStatus statusFor(std::string_view code, std::string_view source, std::vector<std::string> const& keys) {
    auto encoded = encodeFeatures(code, keys);
    if (encoded.empty()) return {};
    return {true, std::string(code), std::string(source), detectionDetail(code, encoded), std::move(encoded)};
}

// QOLMod 기능이 현재 지점에서 실제로 켜져 있는지 계산한다.
// 플레이 중이면 퍼센트 구간(enableranges) 설정을 반영하고, 그 외에는 기본 토글 값을 쓴다.
bool qolEffective(Mod* mod, std::string_view id) {
    bool enabled = mod->getSavedValue<bool>(fmt::format("{}_enabled", id));
    auto* play = PlayLayer::get();
    if (!play || !play->m_started || play->m_isPlatformer || (play->m_player1 && play->m_player1->m_isDead) ||
        (play->m_player2 && play->m_player2->m_isDead))
        return enabled;

    auto ranges = mod->getSavedValue<matjson::Value>(fmt::format("{}_enableranges", id));
    if (!ranges.isArray()) return enabled;
    double const percent = play->getCurrentPercent();
    for (auto const& range : ranges.asArray().unwrap()) {
        if (!range.isObject()) continue;
        double const minimum = range["min"].asDouble().unwrapOr(0.);
        double const maximum = range["max"].asDouble().unwrapOr(0.);
        if (percent >= minimum && percent <= maximum) return range["enable"].asBool().unwrapOr(enabled);
    }
    return enabled;
}

// QOLMod 저장 설정을 읽어 치트 기능 사용 여부를 판정한다.
// Speedhack과 custom respawn time은 값(배속, 리스폰 시간)까지 확인해야 하므로 따로
// 처리한다.
ExternalCheatStatus qolStatus() {
    auto* mod = Loader::get()->getLoadedMod("thesillydoggo.qolmod");
    if (!mod) return {};
    std::vector<std::string> detected;

    bool const disabled = mod->getSavedValue<bool>("safe-mode/disable-cheats-ui_enabled");
    if (!disabled) {
        for (auto const& cheat : kQolCheatFeatures) {
            if (cheat.key != "speedhack" && cheat.key != "custom-respawn-time" && qolEffective(mod, cheat.key))
                detected.emplace_back(cheat.key);
        }
    }

    if (qolEffective(mod, "custom-respawn-time")) {
        matjson::Value const& saved = mod->getSaveContainer();
        for (auto const* key : {"instant-restart/normal_value", "instant-restart/practice_value"}) {
            if (!saved.contains(key)) continue;
            auto const& value = saved[key];
            double const seconds = value.isString()
                                       ? utils::numFromString<double>(value.asString().unwrapOr("")).unwrapOr(0.)
                                       : value.asDouble().unwrapOr(0.);
            if (respawnSlowerThanDefault(seconds)) {
                detected.emplace_back("custom-respawn-time");
                break;
            }
        }
    }

    if (mod->getSavedValue<bool>("speedhack/enabled_enabled")) {
        auto const text = mod->getSavedValue<std::string>("speedhack-top_value");
        auto value = utils::numFromString<double>(text).unwrapOr(1.);
        if (std::isfinite(value) && std::abs(value - 1.) > .0001) detected.emplace_back("speedhack");
    }
    return statusFor(kQolEnabled, "QOLMod", detected);
}

// Eclipse 메뉴 설정을 읽어 판정한다. 불리언 기능 목록에 더해 Speedhack(배속 != 1),
// bot 상태, 기본보다 느린 respawn delay를 추가로 확인한다.
// 이 빌드가 Eclipse 설정을 읽을 수 없으면(compat/Eclipse.hpp) 무엇이 켜졌는지 알 수 없으므로, Eclipse가 로드돼
// 있기만 해도 kEclipseUnchecked로 감지한다. 확인 못 한 Eclipse를 대결에 들이지 않으려는 것이다.
ExternalCheatStatus eclipseStatus(bool includeAttemptHistory) {
    (void)includeAttemptHistory;
    if (!Loader::get()->isModLoaded("eclipse.eclipse-menu")) return {};
    if (!versus::compat::eclipseReadable()) {
        static bool s_logged = false;
        if (!std::exchange(s_logged, true))
            log::warn("Versus: this build can't read Eclipse's settings; a loaded Eclipse counts as a banned feature");
        return statusFor(kEclipseEnabled, "Eclipse", {std::string(kEclipseUnchecked)});
    }
    auto detected =
        activeEclipseBooleanFeatures([](std::string_view key) { return versus::compat::eclipseBool(key, false); });
    if (versus::compat::eclipseBool("global.speedhack.toggle", false)) {
        double const value = versus::compat::eclipseDouble("global.speedhack", 1.);
        if (std::isfinite(value) && std::abs(value - 1.) > .0001) detected.emplace_back("global.speedhack.toggle");
    }
    if (versus::compat::eclipseInt("bot.state", 0) != 0) detected.emplace_back("bot.state");
    if (versus::compat::eclipseBool("player.respawndelay.toggle", false) &&
        respawnSlowerThanDefault(versus::compat::eclipseDouble("player.respawndelay", 1.)))
        detected.emplace_back("player.respawndelay.toggle");
    return statusFor(kEclipseEnabled, "Eclipse", detected);
}

ExternalCheatStatus megaHackProfileStatus(Mod* mod);

// Mega Hack 저장 파일(hackpro.json)은 수정 시각/크기가 바뀔 때만 다시 파싱하고,
// 그 외에는 이전 결과를 재사용한다. 폴링이 잦아서 쓰는 캐시.
ExternalCheatStatus cachedMegaHackProfileStatus(Mod* mod) {
    static std::filesystem::path cachedPath;
    static std::filesystem::file_time_type cachedTime{};
    static std::uintmax_t cachedSize = 0;
    static bool cachedValid = false;
    static ExternalCheatStatus cachedStatus;
    auto const path = versus::compat::stdPath(mod->getSaveDir() / "v9" / "hackpro.json");
    std::error_code timeError, sizeError;
    auto const time = std::filesystem::last_write_time(path, timeError);
    auto const size = std::filesystem::file_size(path, sizeError);
    if (timeError || sizeError) {
        cachedValid = false;
        return megaHackProfileStatus(mod);
    }
    if (!cachedValid || path != cachedPath || time != cachedTime || size != cachedSize) {
        cachedStatus = megaHackProfileStatus(mod);
        cachedPath = path;
        cachedTime = time;
        cachedSize = size;
        cachedValid = true;
    }
    return cachedStatus;
}

// hackpro.json에서 현재 PROFILE의 활성 기능 목록과 V_DECIMAL 수치를 읽어 판정한다.
// 런타임 API를 쓸 수 없을 때의 대체 경로.
ExternalCheatStatus megaHackProfileStatus(Mod* mod) {
    auto document = utils::file::readJson(mod->getSaveDir() / "v9" / "hackpro.json");
    if (!document) return {};
    auto const& root = document.unwrap();
    auto const profile = root["PROFILE"].asString().unwrapOr("");
    auto const enabled = root["PROFILES"][profile];
    if (!enabled.isArray()) return {};
    auto const& entries = enabled.asArray().unwrap();
    auto detected = activeMegaHackGameplayFeatures(
        [&entries](std::string_view id) {
            for (auto const& entry : entries)
                if (entry.asString().unwrapOr("") == id) return true;
            return false;
        },
        [&root](std::string_view key, double fallback) {
            return root["V_DECIMAL"][std::string(key)].asDouble().unwrapOr(fallback);
        });
    return statusFor(kMegaHackEnabled, "Mega Hack", detected);
}

// Mega Hack이 내보내는 런타임 함수 두 개(기능 켜짐 여부, 숫자 설정값).
// 둘 다 찾았을 때만 유효하다.
struct MegaHackRuntimeApi {
    using IsHackEnabled = bool (*)(std::string const&);
    using GetDecimal = double (*)(std::string const&);
    IsHackEnabled isHackEnabled = nullptr;
    GetDecimal getDecimal = nullptr;

    explicit operator bool() const { return isHackEnabled && getDecimal; }
};

// 내보내진 심볼 이름이 Mega Hack의 isHackEnabled / getConfigVar<double> 시그니처
// (인자가 std::string const& 하나)인지 demangle해서 확인한다. 1 = isHackEnabled, 2 =
// getConfigVar, 0 = 해당 없음.
#if defined(GEODE_IS_ANDROID) || defined(GEODE_IS_MACOS) || defined(GEODE_IS_IOS)
int megaHackApiSymbol(char const* mangled) {
    if (!std::strstr(mangled, "hackpro") || !std::strstr(mangled, "extensions")) return 0;
    if (!std::strstr(mangled, "isHackEnabled") && !std::strstr(mangled, "getConfigVar")) return 0;
    int status = 0;
    char* text = abi::__cxa_demangle(mangled, nullptr, nullptr, &status);
    if (status != 0 || !text) return 0;
    std::string_view const name(text);
    bool const stringArg = name.find("(std::") != std::string_view::npos &&
                           name.find("basic_string<char") != std::string_view::npos && name.ends_with("> const&)") &&
                           name.find("const&, ") == std::string_view::npos;
    int kind = 0;
    if (stringArg && name.starts_with("hackpro::extensions::isHackEnabled("))
        kind = 1;
    else if (stringArg && name.starts_with("double hackpro::extensions::getConfigVar<double>("))
        kind = 2;
    std::free(text);
    return kind;
}
// 찾은 심볼 주소를 아직 비어 있는 함수 포인터에만 기록한다.
void noteMegaHackSymbol(MegaHackRuntimeApi& api, int kind, void* address) {
    if (kind == 1 && !api.isHackEnabled)
        api.isHackEnabled = reinterpret_cast<MegaHackRuntimeApi::IsHackEnabled>(address);
    if (kind == 2 && !api.getDecimal) api.getDecimal = reinterpret_cast<MegaHackRuntimeApi::GetDecimal>(address);
}
#endif

// Android: dl_iterate_phdr로 로드된 megahack 라이브러리를 찾고, ELF 심볼
// 테이블(DYNSYM/SYMTAB)을
// 파일에서 직접 읽어 API 주소(로드 기준 주소 + st_value)를 얻는다. 크기 상한을 두고 잘못된
// 파일은 무시한다.
#ifdef GEODE_IS_ANDROID
struct LoadedObject {
    std::string path;
    uintptr_t base = 0;
};
int findMegaHackObject(dl_phdr_info* info, size_t, void* data) {
    if (!info->dlpi_name || !std::strstr(info->dlpi_name, "megahack")) return 0;
    auto* object = static_cast<LoadedObject*>(data);
    object->path = info->dlpi_name;
    object->base = info->dlpi_addr;
    return 1;
}
MegaHackRuntimeApi scanMegaHackApi() {
    LoadedObject object;
    dl_iterate_phdr(findMegaHackObject, &object);
    if (object.path.empty()) return {};
    std::ifstream file(object.path, std::ios::binary);
    if (!file) return {};
    auto read = [&file](uint64_t offset, void* into, size_t size) {
        file.seekg(static_cast<std::streamoff>(offset));
        file.read(static_cast<char*>(into), static_cast<std::streamsize>(size));
        return static_cast<size_t>(file.gcount()) == size;
    };
    using Ehdr = ElfW(Ehdr);
    using Shdr = ElfW(Shdr);
    using Sym = ElfW(Sym);
    Ehdr header{};
    if (!read(0, &header, sizeof(header)) || std::memcmp(header.e_ident, ELFMAG, SELFMAG) != 0 || header.e_shoff == 0 ||
        header.e_shentsize != sizeof(Shdr) || header.e_shnum == 0 || header.e_shnum > 4096)
        return {};
    std::vector<Shdr> sections(header.e_shnum);
    if (!read(header.e_shoff, sections.data(), sections.size() * sizeof(Shdr))) return {};
    MegaHackRuntimeApi api;
    for (auto const& table : sections) {
        if ((table.sh_type != SHT_DYNSYM && table.sh_type != SHT_SYMTAB) || table.sh_entsize != sizeof(Sym) ||
            table.sh_link >= sections.size() || table.sh_size > (64u << 20))
            continue;
        auto const& strings = sections[table.sh_link];
        if (strings.sh_size == 0 || strings.sh_size > (64u << 20)) continue;
        std::vector<Sym> symbols(table.sh_size / sizeof(Sym));
        std::vector<char> names(strings.sh_size + 1, '\0');
        if (!read(table.sh_offset, symbols.data(), symbols.size() * sizeof(Sym)) ||
            !read(strings.sh_offset, names.data(), strings.sh_size))
            continue;
        for (auto const& symbol : symbols) {
            if ((symbol.st_info & 0xf) != STT_FUNC || symbol.st_value == 0 || symbol.st_shndx == SHN_UNDEF ||
                symbol.st_name >= strings.sh_size)
                continue;
            int const kind = megaHackApiSymbol(names.data() + symbol.st_name);
            if (kind) noteMegaHackSymbol(api, kind, reinterpret_cast<void*>(object.base + symbol.st_value));
        }
        if (api) break;
    }
    return api;
}
// macOS/iOS: 로드된 dyld 이미지에서 megahack을 찾아 Mach-O 심볼 테이블(LC_SYMTAB)을
// 읽는다.
// 주소는 slide를 더해 실제 주소로 바꾼다.
#elif defined(GEODE_IS_MACOS) || defined(GEODE_IS_IOS)
MegaHackRuntimeApi scanMegaHackApi() {
    for (uint32_t i = 0, count = _dyld_image_count(); i < count; ++i) {
        char const* image = _dyld_get_image_name(i);
        if (!image || !std::strstr(image, "megahack")) continue;
        auto const* header = reinterpret_cast<mach_header_64 const*>(_dyld_get_image_header(i));
        if (!header || header->magic != MH_MAGIC_64) continue;
        intptr_t const slide = _dyld_get_image_vmaddr_slide(i);
        symtab_command const* symtab = nullptr;
        segment_command_64 const* linkedit = nullptr;
        auto const* command = reinterpret_cast<load_command const*>(header + 1);
        for (uint32_t c = 0; c < header->ncmds; ++c) {
            if (command->cmd == LC_SYMTAB)
                symtab = reinterpret_cast<symtab_command const*>(command);
            else if (command->cmd == LC_SEGMENT_64 &&
                     std::strncmp(reinterpret_cast<segment_command_64 const*>(command)->segname, "__LINKEDIT", 16) == 0)
                linkedit = reinterpret_cast<segment_command_64 const*>(command);
            command = reinterpret_cast<load_command const*>(reinterpret_cast<char const*>(command) + command->cmdsize);
        }
        if (!symtab || !linkedit) continue;
        auto const base = static_cast<uintptr_t>(slide) + static_cast<uintptr_t>(linkedit->vmaddr - linkedit->fileoff);
        auto const* symbols = reinterpret_cast<nlist_64 const*>(base + symtab->symoff);
        auto const* strings = reinterpret_cast<char const*>(base + symtab->stroff);
        MegaHackRuntimeApi api;
        for (uint32_t s = 0; s < symtab->nsyms; ++s) {
            auto const& symbol = symbols[s];
            if ((symbol.n_type & N_STAB) || (symbol.n_type & N_TYPE) != N_SECT || symbol.n_value == 0 ||
                symbol.n_un.n_strx >= symtab->strsize)
                continue;
            char const* name = strings + symbol.n_un.n_strx;
            if (*name == '_') ++name;
            int const kind = megaHackApiSymbol(name);
            if (kind) noteMegaHackSymbol(api, kind, reinterpret_cast<void*>(symbol.n_value + slide));
        }
        if (api) return api;
    }
    return {};
}
#endif

// Mega Hack 런타임 API를 해석한다. Windows는 DLL의 export 이름으로 GetProcAddress,
// 그 외 플랫폼은 최초 한 번만 심볼을 스캔해서 결과를 캐시한다.
MegaHackRuntimeApi megaHackRuntimeApi(Mod* mod) {
#ifdef GEODE_IS_WINDOWS
    auto const path = versus::compat::stdPath(mod->getBinaryPath());
    HMODULE module = GetModuleHandleW(path.c_str());
    if (!module) module = GetModuleHandleW(path.filename().c_str());
    if (!module) module = GetModuleHandleW(L"absolllute.megahack.dll");
    if (!module) return {};
    return {
        reinterpret_cast<MegaHackRuntimeApi::IsHackEnabled>(GetProcAddress(
            module,
            "?isHackEnabled@extensions@hackpro@@YA_NAEBV?$basic_string@DU?$char_traits@D@std@@V?$allocator@D@2@@std@@@Z")),
        reinterpret_cast<MegaHackRuntimeApi::GetDecimal>(GetProcAddress(
            module,
            "??$getConfigVar@N@extensions@hackpro@@YANAEBV?$basic_string@DU?$char_traits@D@std@@V?$allocator@D@2@@std@@@Z")),
    };
#elif defined(GEODE_IS_ANDROID) || defined(GEODE_IS_MACOS) || defined(GEODE_IS_IOS)
    (void)mod;
    static bool scanned = false;
    static MegaHackRuntimeApi found;
    if (!scanned) {
        scanned = true;
        found = scanMegaHackApi();
        log::info("Versus: Mega Hack live settings {}", found ? "found (read live)" : "not exported (saved profile)");
    }
    return found;
#else
    (void)mod;
    return {};
#endif
}

// Mega Hack 런타임 API로 현재 설정을 바로 읽어 판정한다. API를 못 찾았거나 호출 중 예외가
// 나면 nullopt.
// API를 못 찾은 경우 재탐색은 10초 간격으로만 한다.
std::optional<ExternalCheatStatus> megaHackRuntimeStatus(Mod* mod) {
    static Mod* resolvedFor = nullptr;
    static MegaHackRuntimeApi resolved;
    static RuntimeClock::time_point nextLookup{};
    if (resolvedFor != mod || (!resolved && RuntimeClock::now() >= nextLookup)) {
        resolved = megaHackRuntimeApi(mod);
        resolvedFor = mod;
        nextLookup = RuntimeClock::now() + std::chrono::seconds(10);
    }
    auto const api = resolved;
    if (!api) return std::nullopt;
    try {
        auto detected =
            activeMegaHackGameplayFeatures([&api](std::string_view id) { return api.isHackEnabled(std::string(id)); },
                                           [&api](std::string_view key, double fallback) {
                                               auto const value = api.getDecimal(std::string(key));
                                               return std::isfinite(value) ? value : fallback;
                                           });
        return statusFor(kMegaHackEnabled, "Mega Hack", detected);
    } catch (...) {
        return std::nullopt;
    }
}

// 훅에서 Instant Complete 호출이 관측됐다면(현재 PlayLayer 기준) 그 기능을 결과에
// 추가한다.
ExternalCheatStatus withObservedMegaHackInstant(ExternalCheatStatus status) {
    if (!s_megaHackInstantOwner || s_megaHackInstantOwner != PlayLayer::get() ||
        s_megaHackInstantObserved == RuntimeClock::time_point{})
        return status;
    auto features = status.detected ? decodeFeatures(status.code, status.features) : std::vector<std::string>{};
    if (std::find(features.begin(), features.end(), kMegaHackInstantComplete) != features.end()) return status;
    features.emplace_back(kMegaHackInstantComplete);
    return statusFor(kMegaHackEnabled, "Mega Hack", features);
}

// Mega Hack 판정 진입점. 런타임 API 결과를 우선 쓰고, 없으면 저장된 프로필 파일로
// 대체한다.
// 프로필 기반(Windows 외)일 때는 재시작 안내 문구를 detail에 덧붙인다.
ExternalCheatStatus megaHackStatus(bool includeAttemptHistory) {
    auto* mod = Loader::get()->getLoadedMod("absolllute.megahack");
    if (!mod) return {};
    (void)includeAttemptHistory;
    if (auto live = megaHackRuntimeStatus(mod)) return withObservedMegaHackInstant(std::move(*live));
    auto status = withObservedMegaHackInstant(cachedMegaHackProfileStatus(mod));
#ifndef GEODE_IS_WINDOWS
    if (status.detected) status.detail += "\n" + std::string(kMegaHackSavedHint);
#endif
    return status;
}

// Prism 저장 값 배열({name, value} 객체들)에서 이름이 id인 값을 찾는다.
matjson::Value prismValue(matjson::Value const& values, std::string_view id) {
    if (!values.isArray()) return {};
    for (auto const& entry : values.asArray().unwrap()) {
        if (!entry.isObject() || entry["name"].asString().unwrapOr("") != id) continue;
        return entry["value"];
    }
    return {};
}

// Prism Menu 저장 설정과 훅에서 관측한 흔적(Playback, Instant Complete, Speedhack)을 합쳐
// 판정한다.
// 관측 흔적은 Playback 2초, Speedhack 0.75초 이내일 때만 유효하고, Instant Complete는 같은
// PlayLayer 동안 유지한다.
ExternalCheatStatus prismStatus() {
    auto* mod = Loader::get()->getLoadedMod("firee.prism");
    if (!mod) return {};

    matjson::Value const& container = mod->getSaveContainer();
    matjson::Value const& values = container["values"];
    std::unordered_map<std::string, matjson::Value const*> byName;
    if (values.isArray()) {
        for (auto const& entry : values.asArray().unwrap()) {
            if (!entry.isObject()) continue;
            byName.try_emplace(entry["name"].asString().unwrapOr(""), &entry["value"]);
        }
    }
    static matjson::Value const missing;
    auto lookup = [&byName](std::string_view id) -> matjson::Value const& {
        auto found = byName.find(std::string(id));
        return found == byName.end() ? missing : *found->second;
    };
    auto detected = activePrismGameplayFeatures(
        [&lookup](std::string_view id) { return lookup(id).asBool().unwrapOr(false); },
        [&lookup](std::string_view id, double fallback) { return lookup(id).asDouble().unwrapOr(fallback); });
    if (s_prismPlaybackOwner == PlayLayer::get() && s_prismPlaybackObserved != RuntimeClock::time_point{} &&
        RuntimeClock::now() - s_prismPlaybackObserved <= std::chrono::seconds(2) &&
        std::find(detected.begin(), detected.end(), "Playback") == detected.end())
        detected.emplace_back("Playback");
    if (s_prismInstantOwner == PlayLayer::get() && s_prismInstantObserved != RuntimeClock::time_point{} &&
        std::find(detected.begin(), detected.end(), "Instant Complete") == detected.end())
        detected.emplace_back("Instant Complete");
    bool const playbackEnabled = prismValue(values, "Playback").asBool().unwrapOr(false);
    if (!playbackEnabled && s_prismSpeedObserved != RuntimeClock::time_point{} &&
        RuntimeClock::now() - s_prismSpeedObserved <= std::chrono::milliseconds(750) &&
        std::find(detected.begin(), detected.end(), "Speedhack") == detected.end())
        detected.emplace_back("Speedhack");
    return statusFor(kPrismEnabled, "Prism Menu", detected);
}

}

// Prism이 입력을 처리하는 구간과 게임 자체 입력 처리 구간을 표시하는 begin/end 쌍.
// Prism 구간 안이면서 게임 본래 구간이 아닐 때 들어온 입력을 Prism이 주입한 것으로 본다.
void beginPrismProcessScope() {
    ++s_prismProcessDepth;
}

void endPrismProcessScope() {
    if (s_prismProcessDepth) --s_prismProcessDepth;
}

void beginNativeProcessScope() {
    ++s_nativeProcessDepth;
}

void endNativeProcessScope() {
    if (s_nativeProcessDepth) --s_nativeProcessDepth;
}

// Prism이 주입한 입력(Playback)이 관측됐는지 확인하고, 맞으면 시각을 기록하며 감지 상태를
// 돌려준다.
// 안티치트가 적용 중이고 PlayLayer가 있을 때만 동작한다.
ExternalCheatStatus observePrismInjectedInput() {
    if (!s_prismProcessDepth || s_nativeProcessDepth || !Loader::get()->isModLoaded("firee.prism") ||
        !Service::get().antiCheatEnforced() || !PlayLayer::get())
        return {};
    s_prismPlaybackOwner = PlayLayer::get();
    s_prismPlaybackObserved = RuntimeClock::now();
    return statusFor(kPrismEnabled, "Prism Menu", {"Playback"});
}

// PlayLayer 초기화 구간 표시. 가장 바깥 begin에서 Instant Complete 관측 기록을 지운다.
// base 초기화가 끝난 뒤(BaseReturned)에 일어난 완료 호출만 관측으로 인정한다.
void beginPrismInitScope() {
    if (!s_prismInitDepth++) {
        s_prismInitBaseReturned = false;
        s_prismInstantOwner = nullptr;
        s_prismInstantObserved = {};
        s_megaHackInstantOwner = nullptr;
        s_megaHackInstantObserved = {};
    }
}

void markPrismInitBaseReturned() {
    if (s_prismInitDepth) s_prismInitBaseReturned = true;
}

void endPrismInitScope() {
    if (s_prismInitDepth && !--s_prismInitDepth) s_prismInitBaseReturned = false;
}

// Mega Hack의 postUpdate 구간 표시. 구조는 위의 초기화 구간과 같다.
void beginMegaHackPostUpdateScope() {
    if (!s_megaHackPostDepth++) s_megaHackPostBaseReturned = false;
}

void markMegaHackPostUpdateBaseReturned() {
    if (s_megaHackPostDepth) s_megaHackPostBaseReturned = true;
}

void endMegaHackPostUpdateScope() {
    if (s_megaHackPostDepth && !--s_megaHackPostDepth) s_megaHackPostBaseReturned = false;
}

// postUpdate의 base 호출이 끝난 뒤 Mega Hack이 완료 처리를 한 경우를 기록한다.
void observeMegaHackInstantComplete(PlayLayer* layer) {
    if (!s_megaHackPostDepth || !s_megaHackPostBaseReturned || !layer ||
        !Loader::get()->isModLoaded("absolllute.megahack"))
        return;
    s_megaHackInstantOwner = layer;
    s_megaHackInstantObserved = RuntimeClock::now();
}

// PlayLayer 초기화의 base 호출이 끝난 뒤 Prism이 완료 처리를 한 경우를 기록한다.
void observePrismInstantComplete(PlayLayer* layer) {
    if (!s_prismInitDepth || !s_prismInitBaseReturned || !layer || !Loader::get()->isModLoaded("firee.prism")) return;
    s_prismInstantOwner = layer;
    s_prismInstantObserved = RuntimeClock::now();
}

// Prism의 scheduler 훅 구간: 가장 바깥 begin에서 원래 dt를 저장해 둔다.
// end는 같은 scheduler일 때만 저장값을 초기화한다.
void beginPrismSchedulerScope(cocos2d::CCScheduler* scheduler, double dt) {
    if (!s_prismSchedulerDepth++) {
        s_prismScheduler = scheduler;
        s_prismSchedulerInput = dt;
    }
}

void endPrismSchedulerScope(cocos2d::CCScheduler* scheduler) {
    if (!s_prismSchedulerDepth) return;
    if (scheduler != s_prismScheduler) {
        --s_prismSchedulerDepth;
        return;
    }
    if (!--s_prismSchedulerDepth) {
        s_prismScheduler = nullptr;
        s_prismSchedulerInput = 0.;
    }
}

// 다른 모드 훅을 지난 뒤의 dt를 원래 dt와 비교해 배속이 바뀌었는지 본다.
// Playback 중이면 제외하고, 어떤 모드의 Speedhack인지 설정으로 귀속시키며
// 찾지 못하면 Prism Speedhack으로 처리한다.
ExternalCheatStatus observePrismSchedulerDelta(cocos2d::CCScheduler* scheduler, double dt) {
    if (!s_prismSchedulerDepth || scheduler != s_prismScheduler || !Loader::get()->isModLoaded("firee.prism") ||
        !prismSchedulerDeltaIndicatesSpeed(s_prismSchedulerInput, dt))
        return {};

    auto* prism = Loader::get()->getLoadedMod("firee.prism");
    auto const values = prism ? prism->getSaveContainer()["values"] : matjson::Value{};
    if (prismValue(values, "Playback").asBool().unwrapOr(false)) return {};

    auto status = attributeRuntimeSpeedhack();
    if (!status.detected) status = statusFor(kPrismEnabled, "Prism Menu", {"Speedhack"});
    if (status.code == kPrismEnabled) s_prismSpeedObserved = RuntimeClock::now();
    return status;
}

// 배속 변경이 관측됐을 때 원인이 되는 모드를 Eclipse, Mega Hack, QOLMod, Prism 순으로 찾아
// 상태를 돌려준다.
ExternalCheatStatus attributeRuntimeSpeedhack() {
    auto hasFeature = [](ExternalCheatStatus const& status, std::string_view key) {
        auto const features = decodeFeatures(status.code, status.features);
        return std::find(features.begin(), features.end(), key) != features.end();
    };
    auto eclipse = eclipseStatus(false);
    if (hasFeature(eclipse, "global.speedhack.toggle"))
        return statusFor(kEclipseEnabled, "Eclipse", {"global.speedhack.toggle"});

    auto megaHack = megaHackStatus(false);
    for (auto const key :
         {std::string_view("SPEEDHACK/HACK_SPEEDHACK_CLASSIC"), std::string_view("SPEEDHACK/LAYOUT/HACK_SPEEDHACK")}) {
        if (hasFeature(megaHack, key)) return statusFor(kMegaHackEnabled, "Mega Hack", {std::string(key)});
    }

    auto qol = qolStatus();
    if (hasFeature(qol, "speedhack")) return statusFor(kQolEnabled, "QOLMod", {"speedhack"});

    auto* prism = Loader::get()->getLoadedMod("firee.prism");
    if (!prism) return {};
    if (prismValue(prism->getSaveContainer()["values"], "Playback").asBool().unwrapOr(false)) return {};
    if (s_prismSpeedObserved != RuntimeClock::time_point{} &&
        RuntimeClock::now() - s_prismSpeedObserved <= std::chrono::milliseconds(750))
        return statusFor(kPrismEnabled, "Prism Menu", {"Speedhack"});
    return {};
}

// 허용되지 않은 모드(kBannedMods)가 로드되어 있으면 감지로 처리한다.
namespace {
ExternalCheatStatus bannedModStatus() {
    std::vector<std::string> loaded;
    for (auto const& mod : kBannedMods)
        if (Loader::get()->isModLoaded(std::string(mod.key))) loaded.emplace_back(mod.key);
    return loaded.empty() ? ExternalCheatStatus{} : statusFor(kBannedModEnabled, "Unauthorized mod", loaded);
}
}

// 감지된 외부 치트 중 첫 번째만 반환한다. 없으면 빈 상태.
ExternalCheatStatus activeExternalCheat(bool includeAttemptHistory) {
    auto statuses = activeExternalCheats(includeAttemptHistory);
    return statuses.empty() ? ExternalCheatStatus{} : std::move(statuses.front());
}

// 모든 모드 검사를 실행하고 감지된 항목만 모아 반환한다.
std::vector<ExternalCheatStatus> activeExternalCheats(bool includeAttemptHistory) {
    std::vector<ExternalCheatStatus> statuses;
    auto add = [&statuses](ExternalCheatStatus status) {
        if (status.detected) statuses.emplace_back(std::move(status));
    };
    add(eclipseStatus(includeAttemptHistory));
    add(megaHackStatus(includeAttemptHistory));
    add(qolStatus());
    add(prismStatus());
    add(bannedModStatus());
    return statuses;
}

}
