// Versus 서버 클라이언트(Service)의 구현.
// 인증/세션, 서버 시계 보정, 1v1 방 / Ranked / Season 2 / Plaza / Live / 신고·제재·스태프 도구의
// HTTP 요청과 응답 파싱, 매 프레임 tick() 폴링을 담당한다.
// 내부 상태는 익명 namespace의 State 하나(state())에 모여 있고, Group 방 로직은
// GroupService.inl, Live 목록 UI는 LiveMatches.inl이 이 파일에 include 되어 쓰인다.
#include "VersusService.hpp"
#include "FirebaseConfig.hpp"
#include "DebugMode.hpp"
#include "RoomMembership.hpp"
#include "RoomPin.hpp"
#include "BattleProgress.hpp"
#include "RandomMapPolicy.hpp"
#include "RandomMapSearch.hpp"
#include "MapPool.hpp"
#include "RoomScore.hpp"
#include "MapCache.hpp"
#include "Edition.hpp"
#include "Emotes.hpp"
#include "CosmeticsJson.hpp"
#include "IntegrityCodes.hpp"
#include "IntegrityFeatures.hpp"
#include "ExternalCheatMonitor.hpp"
#include "ReplayFormat.hpp"
#include "Season2Lounge.hpp"
#include "RankedConfig.hpp"
#if VERSUS_ARGON
#include <argon/argon.hpp>
#endif

#include <Geode/Geode.hpp>
#include <Geode/binding/GameManager.hpp>
#include <Geode/binding/GJAccountManager.hpp>
#include <Geode/binding/PlayLayer.hpp>
#include <Geode/utils/web.hpp>
#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <ctime>
#include <deque>
#include <iomanip>
#include <memory>
#include <limits>
#include <random>
#include <set>
#include <sstream>
#include <unordered_set>
#include <utility>
#include <tuple>
#include <thread>

using namespace geode::prelude;

namespace versus {
namespace {
using Json = matjson::Value;
using Clock = std::chrono::steady_clock;
// 서버에 아직 기록하지 못한 시도(attempt) 하나. 실패하면 대기열에 남겨 두고 다시 보낸다.
struct PendingAttempt {
    std::string match, room, uid;
    int run, percent;
    Clock::time_point queued = Clock::now();
    int rejected = 0;
};
// 업로드 대기 중인 리플레이. tries는 재시도 횟수, next는 다음 시도 가능 시각.
struct PendingReplay {
    ReplayUpload upload;
    int tries = 0;
    Clock::time_point next = Clock::now();
    Clock::time_point queued = Clock::now();
};
// 서비스 전체의 내부 상태 (프로세스 전체에서 하나만 사용).
struct State {
    bool initialized = false;
    bool updatePending = false;
    bool membershipActive = false, membershipChecking = false;
    Clock::time_point membershipCheckedAt{};
    // 진행 중인 요청 표시. 인증/쓰기/폴링은 서로 겹치지 않도록 이 플래그로 직렬화한다.
    bool authenticating = false;
    bool writing = false;
    bool maintenanceWriting = false;
    bool searchingMap = false;
    bool polling = false;
    bool dispatchingAction = false;
    float pollTime = 0;
    float heartbeatTime = 0;
    // epoch: 방 세션이 바뀔 때마다 증가해 이전 세션의 늦은 응답을 버린다.
    // revision: 쓰기마다 증가해, 그 전에 시작한 폴링 응답을 버린다.
    uint64_t epoch = 0;
    uint64_t revision = 0;
    // 서버 시각 - 로컬 시각 (ms). nowMs()가 이 값을 더해 서버 기준 시각을 만든다.
    int64_t serverOffset = 0;
    bool preciseServerClock = false;
    bool dateClock = false;
    int64_t bestClockRoundTrip = std::numeric_limits<int64_t>::max();
    Clock::time_point clockMeasuredAt;
    std::string token;
    std::string refreshToken;
    std::string apiKey;
    std::string session;
    std::string sessionBase;
    int64_t sessionExpiresMs = 0;
    int syncedAccount = 0;
    std::string syncedUid;
    int syncRounds = 0;
    std::string argonState;
    bool argonBackgroundTried = false;
    bool argonNotice = false;
    std::vector<AccountChoice> accountChoices;
    std::string notice;
    // 폴링 응답의 ETag. 바뀐 게 없으면 304로 받아 파싱을 건너뛴다.
    std::string roomPollEtag;
    std::string battlePollEtag;
    PlayerProfile profile;
    std::optional<RoomInfo> room;
    // ID 토큰을 쓰지 않을 시각(만료)과, 그 전에 뒤에서 미리 갱신을 시작할 시각.
    Clock::time_point expires, refreshAt;
    bool tokenRefreshing = false;
    // 서버에서 마지막으로 응답을 받은 시각 (연결 끊김 판단에 사용).
    Clock::time_point lastContact = Clock::now();
    Clock::time_point nextHistoryAttempt = Clock::now();
    bool historyWriting = false, attemptWriting = false;
    std::string historyError;
    std::string rankedMatch;
    bool rankedLdm = false;
    uint64_t rankedRequestSerial = 0, rankedAppliedSerial = 0;
    std::optional<RankedState> lastRankedState;
    std::optional<Json> rankedStateRaw;
    std::string rankedStateEtag;
    std::vector<Json> rosterEntries;
    std::string rosterTag;
    int64_t broadcastLatest = 0, broadcastLoaded = 0, broadcastHandled = 0;
    int onlineCount = -1;
    int season = kSeasonDefault;
    int64_t seasonEndsAt = kSeasonEndsAtDefault;
    bool broadcastFetching = false;
    Clock::time_point nextBroadcastFetch;
    std::vector<BroadcastInfo> broadcasts;
    std::optional<BroadcastInfo> broadcastPending;
    std::function<bool(BroadcastInfo const&)> broadcastPresenter;
    Clock::time_point broadcastHeard = Clock::now();
    std::optional<MaintenanceInfo> maintenance;
    Clock::time_point maintenanceSince, maintenanceProbe;
    int64_t maintenanceBanner = -1;
    bool maintenanceSeen = false;
    bool maintenanceOver = false;
    std::function<bool(MaintenanceInfo const&)> maintenancePresenter;
    std::string returningMatch;
    Clock::time_point returnRequestedAt, nextReturnAttempt;
    Clock::time_point nextReturnCancel;
    std::optional<BattlePlayerState> pendingForfeit;
    std::string pendingForfeitMatch;
    Clock::time_point nextForfeitAttempt, nextGhostReset;
    Clock::time_point nextAttemptWrite;
    Clock::time_point nextRecordsConnect;
    Clock::time_point nextDrawAttempt;
    Clock::time_point nextDownloadReport;
    Clock::time_point lastBattleReport{};
    // 서버가 내 자리를 마지막으로 확인한 시각(진행 보고 또는 heartbeat 성공).
    Clock::time_point lastSeatAck{};
    Clock::time_point battleReportWantedAt{};
    Clock::time_point nextPauseSettle{};
    std::deque<PendingAttempt> attempts;
    std::deque<PendingReplay> replays;
    bool replayWriting = false;
    std::unordered_set<std::string> recordedMatches;
    std::optional<BanInfo> ownBan;
    std::optional<AppealStatus> ownAppeal;
    std::optional<BanInfo> ownReportBan;
    bool admin = false;
    std::string staffRole;
    std::string rankedCup;
    bool season2Access = false;
    std::unordered_set<std::string> moderators;
    Clock::time_point staffFetchedAt{};
    bool staffFetching = false, staffKnown = false;
    Clock::time_point liveWatchedAt{};
    std::map<int64_t, std::pair<Clock::time_point, MapVotes>> mapVotes;
    std::set<int64_t> mapVotesLoading;
    std::vector<std::pair<std::vector<int64_t>, MapVotesCallback>> mapVoteWaiters;
    std::vector<Done> connectCallbacks;
    // 인증/쓰기 중이라 미뤄 둔 방 액션 큐.
    std::deque<std::function<void()>> pendingActions;
    // Group 방 상태 (GroupService.inl 참고).
    std::optional<GroupRoomInfo> group;
    std::string groupEtag, groupBattleEtag;
    float groupRoomTime = 0.f;
    std::string groupForfeitBattle;
    Clock::time_point nextGroupForfeit;
    Clock::time_point nextGroupDownloadReport;
    std::string groupResultShown;
    bool groupSpectating = false;
    bool botBattle = false;
    std::vector<std::pair<float, int>> botRuns;
    size_t botRun = 0;
    float botTime = 0.f, botWait = 0.f;
    bool botRunning = false;
    std::string botResult;
    bool localRoom = false;
    Json localBody;
};
// State 싱글톤. 종료 시 소멸자 순서 문제를 피하려고 해제하지 않는다.
State& state() {
    static auto* value = new State;
    return *value;
}
// 로컬 시스템 시각 (ms).
int64_t systemNowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch())
        .count();
}
// 서버 기준 현재 시각 (ms).
int64_t nowMs() {
    return systemNowMs() + state().serverOffset;
}
// 응답의 HTTP Date 헤더로 서버-로컬 시계 오프셋을 보정한다.
// Date는 초 단위라 실제 서버 시각은 [at, at+1초) 범위이다. 요청을 보낸 시각(sentAt)으로
// 오프셋의 상한을 구해, 더 정확한 하한 쪽으로 오프셋을 점점 좁혀 간다.
void syncServerClock(web::WebResponse const& response, int64_t sentAt = 0) {
    if (state().preciseServerClock) return;
    auto date = response.header("Date");
    if (!date) return;
    std::tm time{};
    std::istringstream input{std::string(*date)};
    input.imbue(std::locale::classic());
    input >> std::get_time(&time, "%a, %d %b %Y %H:%M:%S GMT");
    if (input.fail()) return;
#ifdef GEODE_IS_WINDOWS
    auto seconds = _mkgmtime(&time);
#else
    auto seconds = timegm(&time);
#endif
    if (seconds <= 0) return;
    auto& s = state();
    int64_t const now = systemNowMs(), at = static_cast<int64_t>(seconds) * 1000;
    int64_t const low = at - now, high = at + 1000 - (sentAt > 0 ? sentAt : now - 10'000);
    if (!s.dateClock || s.serverOffset > high)
        s.serverOffset = low;
    else
        s.serverOffset = std::max(s.serverOffset, low);
    s.dateClock = true;
}
// application/x-www-form-urlencoded용 퍼센트 인코딩.
std::string formEncode(std::string const& value) {
    constexpr char hex[] = "0123456789ABCDEF";
    std::string encoded;
    for (unsigned char c : value) {
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_' ||
            c == '.' || c == '~') {
            encoded += static_cast<char>(c);
        } else {
            encoded += '%';
            encoded += hex[c >> 4];
            encoded += hex[c & 15];
        }
    }
    return encoded;
}
// 서버 시각으로 채워지는 타임스탬프 자리표시 값 ({".sv": "timestamp"}).
Json timestamp() {
    auto value = Json::object();
    value[".sv"] = "timestamp";
    return value;
}
// JSON 필드 읽기 도우미들. 키가 없거나 타입이 다르면 기본값을 돌려준다.
std::string stringAt(Json const& value, char const* key, std::string fallback = {}) {
    return value[key].asString().unwrapOr(std::move(fallback));
}
// 티어 이름 읽기. 서버가 보내는 옛 이름 "Grandpa Demon"(뒤에 '@...'가 붙는 형태 포함)은
// "G.O.A.T"로 바꿔서 쓴다.
std::string tierAt(Json const& value, char const* key, std::string fallback = {}) {
    auto text = stringAt(value, key, std::move(fallback));
    constexpr std::string_view old = "Grandpa Demon";
    if (std::string_view(text).substr(0, old.size()) == old && (text.size() == old.size() || text[old.size()] == '@'))
        text.replace(0, old.size(), "G.O.A.T");
    return text;
}
int64_t intAt(Json const& value, char const* key, int64_t fallback = 0) {
    return value[key].asInt().unwrapOr(fallback);
}
bool boolAt(Json const& value, char const* key) {
    return value[key].asBool().unwrapOr(false);
}
// ccColor3B를 0xRRGGBB 정수로 묶는다.
int pack(ccColor3B color) {
    return (color.r << 16) | (color.g << 8) | color.b;
}
// GD 계정/아이콘/색상과 저장된 꾸미기(cosmetics)에서 내 프로필을 다시 만든다.
void refreshLocalProfile() {
    auto& profile = state().profile;
    auto* manager = GameManager::sharedState();
    auto* account = GJAccountManager::sharedState();
    profile.name = account && !account->m_username.empty() ? std::string(account->m_username) : "Player";
    profile.accountId = account ? account->m_accountID : 0;
    profile.icon = manager ? std::max(1, manager->getPlayerFrame()) : 1;
    profile.membership = state().membershipActive;
    profile.hasCosmetics = true;
    profile.cosmetics = cosmeticsForEdition(
        parseCosmetics(Mod::get()->getSavedValue<Json>(cosmeticsSaveKey(profile.membership), Json::object()),
                       defaultCosmetics(profile.membership)),
        profile.membership);
    profile.color1 = manager ? pack(manager->colorForIdx(manager->getPlayerColor())) : 0x007BFF;
    profile.color2 = manager ? pack(manager->colorForIdx(manager->getPlayerColor2())) : 0xFFFFFF;
    // 1.0.7: Glow가 켜져 있으면 그 색(GD의 글로우 색 번호). 모든 플레이어 아이콘이 이 값을 따라 그려진다.
    profile.glow = manager && manager->getPlayerGlow() ? pack(manager->colorForIdx(manager->getPlayerGlowColor())) : -1;
}
// Firebase API 키. 모드 설정값이 있으면 그것을, 없으면 내장 값을 쓴다.
std::string configuredApiKey() {
    auto key = Mod::get()->getSettingValue<std::string>("firebase-api-key");
    return key.empty() ? firebase_config::WEB_API_KEY : key;
}
// 프로필 / 규칙 / 전투 상태 / 레벨의 JSON 변환(서버 전송용)과 파싱(서버 응답용) 함수들.
Json profileJson(PlayerProfile const& profile) {
    auto value = Json::object();
    value["uid"] = profile.uid;
    value["name"] = profile.name;
    value["accountId"] = profile.accountId;
    value["icon"] = profile.icon;
    value["color1"] = profile.color1;
    value["color2"] = profile.color2;
    if (profile.glow >= 0) value["glow"] = profile.glow;  // 1.0.7
    value["winRate"] = profile.winRate;
    value["recentGames"] = profile.recentGames;
    if (profile.membership) value["membership"] = true;
    if (profile.hasCosmetics) value["cosmetics"] = cosmeticsJson(profile.cosmetics);
    return value;
}
PlayerProfile parseProfile(Json const& value) {
    PlayerProfile result;
    result.uid = stringAt(value, "uid");
    result.name = stringAt(value, "name", "Player");
    result.accountId = static_cast<int>(intAt(value, "accountId"));
    result.icon = static_cast<int>(intAt(value, "icon", 1));
    result.color1 = static_cast<int>(intAt(value, "color1", 0x007BFF));
    result.color2 = static_cast<int>(intAt(value, "color2", 0xFFFFFF));
    result.glow = value["glow"].isNumber() ? static_cast<int>(std::clamp<int64_t>(intAt(value, "glow"), 0, 0xFFFFFF)) : -1;
    result.winRate = value["winRate"].asDouble().unwrapOr(0.);
    result.recentGames = static_cast<int>(intAt(value, "recentGames"));
    result.membership = boolAt(value, "membership");
    result.hasCosmetics = value["cosmetics"].isObject();
    result.cosmetics =
        cosmeticsForEdition(parseCosmetics(value["cosmetics"], defaultCosmetics(result.membership)), result.membership);
    return result;
}
GameRules parseRules(Json const& value) {
    GameRules result;
    result.mode = static_cast<int>(intAt(value, "mode"));
    result.attempts = static_cast<int>(intAt(value, "attempts", 3));
    result.targetPercent = static_cast<int>(intAt(value, "targetPercent", 40));
    result.practice = boolAt(value, "practice");
    result.allowHacks = boolAt(value, "allowHacks");
    result.speed = static_cast<int>(intAt(value, "speed", 100));
    if (!validRuleSpeed(result.speed)) result.speed = 100;
    result.autoLives = boolAt(value, "autoLives");
    return result;
}
// 기본값이 아닌 옵션(allowHacks, speed, autoLives)만 JSON에 넣는다.
Json rulesJson(GameRules const& rules) {
    auto value = Json::object();
    value["mode"] = rules.mode;
    value["attempts"] = rules.attempts;
    value["targetPercent"] = rules.targetPercent;
    value["sequence"] = false;
    value["practice"] = rules.practice;
    if (rules.allowHacks) value["allowHacks"] = true;
    if (rules.speed != 100) value["speed"] = rules.speed;
    if (rules.autoLives) value["autoLives"] = true;
    return value;
}
BattlePlayerState parseBattlePlayer(Json const& value) {
    BattlePlayerState result;
    result.attemptsUsed = static_cast<int>(intAt(value, "attemptsUsed"));
    result.bestPercent = static_cast<int>(intAt(value, "bestPercent"));
    result.currentPercent = static_cast<int>(intAt(value, "currentPercent"));
    result.runNumber = static_cast<int>(intAt(value, "runNumber"));
    result.inAttempt = boolAt(value, "inAttempt");
    result.cleared = boolAt(value, "cleared");
    result.userCoins = static_cast<int>(intAt(value, "userCoins"));
    result.forfeited = boolAt(value, "forfeited");
    result.spectating = boolAt(value, "spectating");
    result.paused = boolAt(value, "paused");
    result.pausedAt = intAt(value, "pausedAt");
    result.pauseUsedMs = intAt(value, "pauseUsedMs");
    result.pauseSeq = intAt(value, "pauseSeq");
    result.pauseChangedAt = intAt(value, "pauseChangedAt");
    result.x = value["x"].asDouble().unwrapOr(0.);
    result.y = value["y"].asDouble().unwrapOr(0.);
    result.cameraX = value["cameraX"].asDouble().unwrapOr(0.);
    result.cameraY = value["cameraY"].asDouble().unwrapOr(0.);
    result.updatedAt = intAt(value, "updatedAt");
    result.integrityCode = stringAt(value, "integrityCode");
    result.integrityFeatures = stringAt(value, "integrityFeatures");
    result.tps = static_cast<int>(std::clamp<int64_t>(intAt(value, "tps"), 0, 100000));
    return result;
}
BattleInfo parseBattle(Json const& source) {
    BattleInfo battle;
    battle.id = stringAt(source, "id");
    battle.firstReachedUid = stringAt(source, "firstReachedUid");
    battle.opponentRunAtFirst = static_cast<int>(intAt(source, "opponentRunAtFirst"));
    battle.winnerUid = stringAt(source, "winnerUid");
    battle.draw = boolAt(source, "draw");
    battle.hostReturned = boolAt(source, "hostReturned");
    battle.guestReturned = boolAt(source, "guestReturned");
    battle.hostReroll = intAt(source, "hostReroll");
    battle.guestReroll = intAt(source, "guestReroll");
    battle.rerolled = boolAt(source, "rerolled");
    battle.finishedAt = intAt(source, "finishedAt");
    battle.host = parseBattlePlayer(source["host"]);
    battle.guest = parseBattlePlayer(source["guest"]);
    return battle;
}
// 전투 상태를 서버에 올릴 JSON으로 만든다. tps는 측정된 경우에만 포함한다.
Json battlePlayerJson(BattlePlayerState const& player) {
    auto value = Json::object();
    value["attemptsUsed"] = player.attemptsUsed;
    value["bestPercent"] = player.bestPercent;
    value["currentPercent"] = player.currentPercent;
    value["runNumber"] = player.runNumber;
    value["inAttempt"] = player.inAttempt;
    value["cleared"] = player.cleared;
    value["userCoins"] = player.userCoins;
    value["forfeited"] = player.forfeited;
    value["spectating"] = player.spectating;
    value["paused"] = player.paused;
    value["pausedAt"] = player.pausedAt;
    value["pauseUsedMs"] = player.pauseUsedMs;
    value["pauseSeq"] = player.pauseSeq;
    value["pauseChangedAt"] = player.pauseChangedAt;
    value["x"] = player.x;
    value["y"] = player.y;
    value["cameraX"] = player.cameraX;
    value["cameraY"] = player.cameraY;
    value["updatedAt"] = player.updatedAt;
    value["integrityCode"] = player.integrityCode;
    value["integrityFeatures"] = player.integrityFeatures;
    if (player.tps > 0) value["tps"] = player.tps;
    return value;
}
// 전투 시작 직후의 초기 플레이어 상태 (run 1, 시도 중).
Json initialBattlePlayer() {
    BattlePlayerState player;
    player.runNumber = 1;
    player.inAttempt = true;
    return battlePlayerJson(player);
}
// 이 플레이어가 더 이상 플레이할 수 없는 상태인지 (포기 / 클리어 / 목숨 소진).
bool battleTerminal(Json const& player, GameRules const& rules) {
    if (boolAt(player, "forfeited") || boolAt(player, "cleared")) return true;
    return rules.mode == 0 && !rules.practice && intAt(player, "attemptsUsed") >= rules.attempts;
}
// 방 JSON(body)의 전투가 끝났는지 판정해, 끝났으면 winnerUid / draw / finishedAt을 채운다.
// actorUid는 이번 갱신을 일으킨 쪽이다. 판정 순서:
// 포기 -> 일시정지 시간 소진 -> 모드별 규칙(목표 % / 목숨).
void resolveBattle(Json& body, std::string const& actorUid) {
    auto& battle = body["battle"];
    if (!battle.isObject() || intAt(battle, "finishedAt") > 0) return;
    auto const rules = parseRules(std::as_const(body)["rules"]);
    auto const hostUid = stringAt(std::as_const(body)["host"], "uid");
    auto const guestUid = stringAt(std::as_const(body)["guest"], "uid");
    auto const host = battle["host"];
    auto const guest = battle["guest"];
    std::string winner;
    bool draw = false;
    if (boolAt(host, "forfeited"))
        winner = guestUid;
    else if (boolAt(guest, "forfeited"))
        winner = hostUid;
    else if (auto const state = parseBattlePlayer(host);
             battle::pauseRemaining(state, nowMs()) == 0 && !battle::finalByPlay(state, rules))
        winner = guestUid;
    else if (auto const state = parseBattlePlayer(guest);
             battle::pauseRemaining(state, nowMs()) == 0 && !battle::finalByPlay(state, rules))
        winner = hostUid;
    // 목표 % 모드: 먼저 목표에 도달한 쪽(firstReachedUid)을 기록한다. 상대가 시도 중이 아니면
    // 바로 승리, 시도 중이면 같은 run 안에서 상대도 도달하는지 지켜본다(도달하면 무승부,
    // 그 run이 끝나도록 못 하면 먼저 도달한 쪽 승리).
    else if (rules.mode == 1) {
        auto const threshold = rules.targetPercent;
        if (stringAt(battle, "firstReachedUid").empty()) {
            bool const actorHost = actorUid == hostUid;
            for (bool sideHost : {!actorHost, actorHost}) {
                if (battle::raceReached(parseBattlePlayer(sideHost ? host : guest)) < threshold) continue;
                auto const& rival = sideHost ? guest : host;
                auto const firstUid = sideHost ? hostUid : guestUid;
                battle["firstReachedUid"] = firstUid;
                battle["opponentRunAtFirst"] = boolAt(rival, "inAttempt") ? intAt(rival, "runNumber") : 0;
                if (!boolAt(rival, "inAttempt")) winner = firstUid;
                break;
            }
        }
        auto const first = stringAt(battle, "firstReachedUid");
        if (!first.empty() && winner.empty()) {
            auto const& other = first == hostUid ? guest : host;
            auto const replyRun = intAt(battle, "opponentRunAtFirst");
            if (replyRun > 0 && battle::raceReached(parseBattlePlayer(other)) >= threshold)
                draw = true;
            else if (replyRun == 0 || !boolAt(other, "inAttempt") || intAt(other, "runNumber") > replyRun)
                winner = first;
        }
    }
    // 목숨 모드: 둘 다 클리어하면 시도 횟수 -> 유저 코인 순으로 비교하고 같으면 무승부.
    // 그 외에는 조기 승리 조건 또는 양쪽 종료 시 점수(liveScore)로 판정한다.
    else {
        bool const hostDone = battleTerminal(host, rules);
        bool const guestDone = battleTerminal(guest, rules);
        auto const hostState = parseBattlePlayer(host);
        auto const guestState = parseBattlePlayer(guest);
        if (hostState.cleared && guestState.cleared) {
            if (hostState.attemptsUsed != guestState.attemptsUsed)
                winner = hostState.attemptsUsed < guestState.attemptsUsed ? hostUid : guestUid;
            else if (hostState.userCoins != guestState.userCoins)
                winner = hostState.userCoins > guestState.userCoins ? hostUid : guestUid;
            else
                draw = true;
        } else if (battle::earlyAttemptWin(hostState, guestState, rules))
            winner = hostUid;
        else if (battle::earlyAttemptWin(guestState, hostState, rules))
            winner = guestUid;
        else if ((rules.practice && boolAt(host, "cleared") && boolAt(guest, "cleared")) ||
                 (!rules.practice && hostDone && guestDone) || battle::livesExhausted(hostState, guestState, rules)) {
            auto const hostScore = battle::liveScore(hostState);
            auto const guestScore = battle::liveScore(guestState);
            if (hostScore == guestScore)
                draw = true;
            else
                winner = hostScore > guestScore ? hostUid : guestUid;
        }
    }
    // 결과가 정해졌을 때만 종료 정보를 기록한다.
    if (draw || !winner.empty()) {
        battle["draw"] = draw;
        battle["winnerUid"] = winner;
        battle["finishedAt"] = timestamp();
    }
}
// 레벨 JSON 파싱. featureState는 0~4, 유저 코인은 0~3, length는 0~5로 보정(없으면 -1).
LevelInfo parseLevel(Json const& value) {
    return {intAt(value, "id"),
            stringAt(value, "name"),
            static_cast<int>(intAt(value, "difficulty")),
            static_cast<int>(intAt(value, "stars")),
            boolAt(value, "demon"),
            boolAt(value, "autoLevel"),
            boolAt(value, "platformer"),
            stringAt(value, "creator"),
            static_cast<int>(std::clamp<int64_t>(intAt(value, "featureState"), 0, 4)),
            static_cast<int>(std::clamp<int64_t>(intAt(value, "userCoins"), 0, 3)),
            value.contains("length") ? static_cast<int>(std::clamp<int64_t>(intAt(value, "length"), 0, 5)) : -1};
}
// 레벨을 JSON으로 만든다. 오토 레벨은 difficulty를 -1로 보낸다.
Json levelJson(LevelInfo const& level) {
    auto value = Json::object();
    value["id"] = level.id;
    value["name"] = level.name;
    value["difficulty"] = level.autoLevel ? -1 : level.difficulty;
    value["stars"] = level.stars;
    value["demon"] = level.demon;
    value["autoLevel"] = level.autoLevel;
    value["platformer"] = level.platformer;
    value["creator"] = level.creator;
    value["featureState"] = level.featureState;
    value["userCoins"] = level.userCoins;
    if (level.length >= 0) value["length"] = level.length;
    return value;
}
}
// 맵의 별/난이도/길이로 자동 목숨 수(1~99)를 계산한다.
int autoRoomLives(LevelInfo const& level) {
    int const face = !level.demon && level.autoLevel ? -1 : level.difficulty;
    return std::clamp(ranked::livesFor(ranked::baseLives(level.stars, level.demon, face), level.length), 1, 99);
}
namespace {
// 두 JSON 값이 구조적으로 같은지.
bool sameJson(Json const& left, Json const& right) {
    if (left.isObject() != right.isObject() || left.isArray() != right.isArray()) return false;
    if (left.isObject()) {
        if (left.size() != right.size()) return false;
        for (auto const& member : left) {
            auto const& key = member.getKey();
            if (!key || !right.contains(*key) || !sameJson(member, right[*key])) return false;
        }
        return true;
    }
    if (left.isArray()) {
        if (left.size() != right.size()) return false;
        for (size_t i = 0; i < left.size(); ++i)
            if (!sameJson(left[i], right[i])) return false;
        return true;
    }
    return left == right;
}
// 규칙이 autoLives(목숨제, 연습 아님)이면 맵 정보로 attempts를 다시 계산해 body에 반영한다.
void applyAutoLives(Json& body) {
    auto const& rules = std::as_const(body)["rules"];
    auto const& level = std::as_const(body)["level"];
    if (!rules.isObject() || !boolAt(rules, "autoLives") || intAt(rules, "mode") != 0 || boolAt(rules, "practice"))
        return;
    if (!level.isObject() || !level.contains("stars") || !level.contains("difficulty")) return;
    body["rules"]["attempts"] = autoRoomLives(parseLevel(level));
}
// Map Pool이 랜덤 추첨 방식인지 (random이고 맵이 2개 이상).
bool poolRolls(Json const& pool) {
    return boolAt(pool, "random") && pool["maps"].isArray() && pool["maps"].asArray().unwrap().size() >= 2;
}
// 방 JSON의 level / mapDraw를 Map Pool 내용과 맞춘다.
// 현재 맵이 풀에 있으면 그대로 두고, 없으면 추첨 방식/빈 풀일 때는 level을 비우고,
// 그렇지 않으면 풀의 첫 맵으로 바꾼다. 맵이 바뀌면 양쪽 준비 상태를 해제한다.
void followMapPool(Json& body) {
    if (!std::as_const(body)["mapPool"].isObject() || !std::as_const(body)["mapPool"]["maps"].isArray()) return;
    bool const rolls = poolRolls(std::as_const(body)["mapPool"]);
    Json const maps = std::as_const(body)["mapPool"]["maps"];
    Json const current = std::as_const(body)["level"];
    if (!rolls) body.erase("mapDraw");
    int64_t const currentId = current.isObject() ? intAt(current, "id") : 0;
    bool const rolled = std::as_const(body)["mapDraw"].isObject();
    auto const& entries = maps.asArray().unwrap();
    for (auto const& entry : entries)
        if (currentId > 0 && intAt(entry["level"], "id") == currentId && (!rolls || rolled)) return;
    body.erase("mapDraw");
    if (entries.empty() || rolls) {
        if (current.isObject()) {
            body.erase("level");
            body["hostReady"] = false;
            body["guestReady"] = false;
        }
        return;
    }
    Json const chosen = entries[0]["level"];
    bool const changed = !sameJson(current, chosen);
    body["level"] = chosen;
    applyAutoLives(body);
    if (changed) {
        body["hostReady"] = false;
        body["guestReady"] = false;
    }
}
// 전투가 끝났을 때 방금 플레이한 맵을 Map Pool에서 빼고, 다음 level을 풀 기준으로 다시 정한다.
void dropPlayedPoolMap(Json& body) {
    auto const& pool = std::as_const(body)["mapPool"];
    auto const& battle = std::as_const(body)["battle"];
    if (!pool.isObject() || !pool["maps"].isArray() || !battle.isObject() || intAt(battle, "finishedAt") <= 0) return;
    auto const played = intAt(std::as_const(body)["level"], "id");
    auto maps = Json::array();
    for (auto const& entry : pool["maps"].asArray().unwrap())
        if (intAt(entry["level"], "id") != played) maps.push(entry);
    body["mapPool"]["maps"] = maps;
    auto const current = intAt(std::as_const(body)["level"], "id");
    for (auto const& entry : maps.asArray().unwrap())
        if (current > 0 && intAt(entry["level"], "id") == current) return;
    body.erase("mapDraw");
    bool const rolls = poolRolls(std::as_const(body)["mapPool"]);
    if (maps.asArray().unwrap().empty() || rolls) {
        body.erase("level");
        return;
    }
    Json const first = maps.asArray().unwrap().front()["level"];
    body["level"] = first;
    applyAutoLives(body);
}
// 맵 선택 조건 파싱 (옛 형식의 difficulty는 mask로 변환). 범위를 벗어난 값은 보정한다.
MapSelection parseMapSelection(Json const& value) {
    MapSelection result{boolAt(value, "random"),
                        value.contains("mask") ? static_cast<int>(intAt(value, "mask"))
                                               : legacySelectionMask(static_cast<int>(intAt(value, "difficulty"))),
                        boolAt(value, "platformer")};
    result.lengths = static_cast<int>(std::clamp<int64_t>(intAt(value, "lengths", kLengthsAll), 1, kLengthsAll));
    result.rated = !value.contains("rated") || boolAt(value, "rated");
    result.twoPlayer = static_cast<int>(std::clamp<int64_t>(intAt(value, "twoPlayer"), 0, 2));
    result.minLikes = static_cast<int>(std::clamp<int64_t>(intAt(value, "minLikes"), 0, kLikesMax));
    return result;
}
// 기본값과 다른 조건만 JSON에 넣는다.
Json mapSelectionJson(MapSelection const& selection, bool platformer = true) {
    auto config = Json::object();
    config["random"] = selection.random;
    config["mask"] = selection.mask;
    if (platformer) config["platformer"] = selection.platformer;
    if (selection.lengths != kLengthsAll) config["lengths"] = selection.lengths;
    if (!selection.rated) config["rated"] = false;
    if (selection.twoPlayer) config["twoPlayer"] = selection.twoPlayer;
    if (selection.minLikes) config["minLikes"] = selection.minLikes;
    return config;
}
// 맵 추첨 파싱. selected는 숫자 또는 한 글자 숫자 문자열, levels는 배열 또는 "0"~"9" 키의
// 객체를 모두 받는다. 후보가 2~10개이고 선택 인덱스가 유효해야 결과를 돌려준다.
std::optional<MapDraw> parseMapDraw(Json const& source) {
    if (!source.isObject()) return std::nullopt;
    MapDraw draw;
    draw.id = stringAt(source, "id");
    draw.at = intAt(source, "at");
    if (source["selected"].isNumber())
        draw.selected = static_cast<int>(std::clamp<int64_t>(intAt(source, "selected", -1), -1, 9));
    else {
        auto const selected = stringAt(source, "selected", "0");
        draw.selected = selected.size() == 1 && selected[0] >= '0' && selected[0] <= '9' ? selected[0] - '0' : -1;
    }
    draw.settled = boolAt(source, "settled");
    auto const& levels = source["levels"];
    if (levels.isArray())
        for (auto const& level : levels.asArray().unwrap()) draw.levels.push_back(parseLevel(level));
    else if (levels.isObject())
        for (int i = 0; i < 10; ++i) {
            auto const key = std::to_string(i);
            if (!levels.contains(key)) break;
            draw.levels.push_back(parseLevel(levels[key]));
        }
    if (draw.levels.size() >= 2 && draw.levels.size() <= 10 && draw.selected >= 0 &&
        draw.selected < static_cast<int>(draw.levels.size()) && !draw.id.empty())
        return draw;
    return std::nullopt;
}
// Map Pool 설정 / 풀 파싱과 JSON 변환.
MapPoolSettings parsePoolSettings(Json const& value) {
    MapPoolSettings result;
    result.perPlayer = static_cast<int>(std::clamp<int64_t>(intAt(value, "perPlayer", 3), 1, kPoolPerPlayerMax));
    result.rated = boolAt(value, "rated");
    result.mask = static_cast<int>(std::clamp<int64_t>(intAt(value, "mask", kPoolMaskAll), 1, kPoolMaskAll));
    result.twoPlayer = static_cast<int>(std::clamp<int64_t>(intAt(value, "twoPlayer"), 0, 2));
    result.random = boolAt(value, "random");
    result.lengths = static_cast<int>(std::clamp<int64_t>(intAt(value, "lengths", kLengthsAll), 1, kLengthsAll));
    result.minLikes = static_cast<int>(std::clamp<int64_t>(intAt(value, "minLikes"), 0, kLikesMax));
    result.filter = !value.contains("filter") || boolAt(value, "filter");
    return result;
}
Json poolSettingsJson(MapPoolSettings const& settings) {
    auto value = Json::object();
    value["perPlayer"] = settings.perPlayer;
    value["rated"] = settings.rated;
    value["mask"] = settings.mask;
    value["twoPlayer"] = settings.twoPlayer;
    value["random"] = settings.random;
    value["lengths"] = settings.lengths;
    value["minLikes"] = settings.minLikes;
    value["filter"] = settings.filter;
    return value;
}
std::optional<MapPool> parseMapPool(Json const& value) {
    if (!value.isObject()) return std::nullopt;
    MapPool pool;
    pool.settings = parsePoolSettings(value);
    if (value["permit"].isArray())
        for (auto const& item : value["permit"].asArray().unwrap())
            if (item.isString()) pool.permit.push_back(item.asString().unwrap());
    if (value["maps"].isArray())
        for (auto const& item : value["maps"].asArray().unwrap()) {
            MapPoolEntry entry;
            entry.level = parseLevel(item["level"]);
            entry.twoPlayer = boolAt(item, "twoPlayer");
            entry.by = stringAt(item, "by");
            entry.name = stringAt(item, "name");
            if (entry.level.id > 0) pool.maps.push_back(std::move(entry));
        }
    return pool;
}
// 1v1 방 JSON을 RoomInfo로 변환한다 (다운로드 진행, 점수, 추첨, 이모트, launch, battle, hacks 포함).
RoomInfo parseRoom(std::string id, Json const& value) {
    RoomInfo result;
    result.id = std::move(id);
    result.name = stringAt(value, "name");
    result.privateRoom = boolAt(value, "privateRoom");
    result.host = parseProfile(value["host"]);
    if (hasRoomGuest(value)) result.guest = parseProfile(value["guest"]);
    result.level = parseLevel(value["level"]);
    auto download = [](Json const& item) {
        RoomInfo::DownloadInfo result;
        result.uid = stringAt(item, "uid");
        result.levelId = intAt(item, "levelId");
        result.updatedAt = intAt(item, "updatedAt");
        result.percent = static_cast<int>(intAt(item, "percent"));
        result.mapReady = boolAt(item, "mapReady");
        result.songsDone = static_cast<int>(intAt(item, "songsDone"));
        result.songsTotal = static_cast<int>(intAt(item, "songsTotal"));
        result.soundsDone = static_cast<int>(intAt(item, "soundsDone"));
        result.soundsTotal = static_cast<int>(intAt(item, "soundsTotal"));
        result.best =
            item.contains("best") ? static_cast<int>(std::clamp<int64_t>(intAt(item, "best", -1), -1, 100)) : -1;
        return result;
    };
    // hacks: 플레이어 uid별로 감지된 무결성 코드/기능 목록.
    if (value["hacks"].isObject())
        for (auto const& entry : value["hacks"]) {
            auto& list = result.hacks[entry.getKey().value_or("")];
            if (entry.isArray())
                for (auto const& item : entry.asArray().unwrap())
                    if (item.isObject()) list.emplace_back(stringAt(item, "code"), stringAt(item, "features"));
        }
    result.hostDownload = download(value["downloads"]["host"]);
    result.guestDownload = download(value["downloads"]["guest"]);
    result.mapSelection = parseMapSelection(value["mapSelection"]);
    result.hostWins = static_cast<int>(intAt(value["score"], "host"));
    result.guestWins = static_cast<int>(intAt(value["score"], "guest"));
    result.scoredMatch = stringAt(value["score"], "lastMatch");
    result.mapDraw = parseMapDraw(value["mapDraw"]);
    result.rules = parseRules(value["rules"]);
    result.hostReady = boolAt(value, "hostReady");
    result.guestReady = boolAt(value, "guestReady");
    result.hostEmoteAt = intAt(value, "hostEmoteAt");
    result.guestEmoteAt = intAt(value, "guestEmoteAt");
    if (value["emote"].isObject()) {
        EmoteInfo emote;
        emote.uid = stringAt(value["emote"], "uid");
        emote.kind = stringAt(value["emote"], "kind");
        emote.text = stringAt(value["emote"], "text");
        emote.at = intAt(value["emote"], "at");
        emote.nonce = stringAt(value["emote"], "nonce");
        result.emote = std::move(emote);
    }
    result.started = boolAt(value, "started");
    result.updatedAt = intAt(value, "updatedAt");
    if (value["launch"].isObject()) {
        LaunchInfo launch;
        launch.id = stringAt(value["launch"], "id");
        launch.requestedAt = intAt(value["launch"], "requestedAt");
        launch.releasedAt = intAt(value["launch"], "releasedAt");
        launch.hostLoaded = boolAt(value["launch"], "hostLoaded");
        launch.guestLoaded = boolAt(value["launch"], "guestLoaded");
        result.launch = std::move(launch);
    }
    if (value["battle"].isObject()) result.battle = parseBattle(value["battle"]);
    result.debug = boolAt(value, "debug");
    result.mapPool = parseMapPool(value["mapPool"]);
    return result;
}
// Ranked 프로필 파싱. 필드가 없으면 기본 레이팅 1500 / RD 350 / 변동성 0.06을 쓴다.
RankedProfile parseRankedProfile(Json const& value) {
    RankedProfile result;
    result.player = parseProfile(value["profile"]);
    result.rating = value["rating"].asDouble().unwrapOr(1500.);
    result.ratingDeviation = value["ratingDeviation"].asDouble().unwrapOr(350.);
    result.volatility = value["volatility"].asDouble().unwrapOr(.06);
    result.matches = static_cast<int>(intAt(value, "rankedMatches"));
    result.wins = static_cast<int>(intAt(value, "rankedWins"));
    result.losses = static_cast<int>(intAt(value, "rankedLosses"));
    result.placementCompleted = static_cast<int>(intAt(value, "placementMatchesCompleted"));
    result.placementRequired = static_cast<int>(intAt(value, "placementMatchesRequired", 0));
    result.globalRank = static_cast<int>(intAt(value, "globalRank"));
    result.population = static_cast<int>(intAt(value, "population"));
    result.percentile = value["percentile"].asDouble().unwrapOr(100.);
    result.tier = tierAt(value, "tier", "Insane");
    result.tierName = tierAt(value, "tierName");
    result.placed = value.contains("cup") ? boolAt(value, "placed") : true;
    result.allowDuel = value["allowDuel"].asBool().unwrapOr(true);
    result.duelAnonymous = value["duelAnonymous"].asBool().unwrapOr(true);
    result.searching = value["searching"].asBool().unwrapOr(true);
    result.winStreak = static_cast<int>(intAt(value, "winStreak"));
    if (value["emote"].isObject()) {
        result.emoteKind = stringAt(value["emote"], "kind");
        result.emoteAt = intAt(value["emote"], "at");
    }
    result.banned = boolAt(value, "banned");
    if (value["ban"].isObject()) {
        result.banPermanent = boolAt(value["ban"], "permanent");
        result.banUntil = intAt(value["ban"], "until");
        result.banReason = stringAt(value["ban"], "reason");
    }
    return result;
}
// Ranked 경기 결과 파싱. 레이팅/티어/순위/백분위의 이전·이후 값, 연승 보너스, 패배 감면 등.
// (mapBest / opponentMapBest가 모두 있을 때만 mapKnown)
RankedResult parseRankedResult(Json const& value) {
    RankedResult result;
    result.matchId = stringAt(value, "matchId");
    result.winnerUid = stringAt(value, "winnerUid");
    result.result = stringAt(value, "result");
    result.disqualifiedUid = stringAt(value, "disqualifiedUid");
    result.integrityCode = stringAt(value, "integrityCode");
    result.integrityFeatures = stringAt(value, "integrityFeatures");
    result.rematchDeclined = stringAt(value, "rematchDeclined");  // 1.0.7
    result.draw = boolAt(value, "draw");
    result.oldRating = value["oldRating"].asDouble().unwrapOr(1500.);
    result.newRating = value["newRating"].asDouble().unwrapOr(result.oldRating);
    result.oldTier = tierAt(value, "oldTier", "Easy");
    result.newTier = tierAt(value, "newTier", result.oldTier);
    result.oldTierName = tierAt(value, "oldTierName");
    result.newTierName = tierAt(value, "newTierName");
    result.oldGlobalRank = static_cast<int>(intAt(value, "oldGlobalRank"));
    result.newGlobalRank = static_cast<int>(intAt(value, "newGlobalRank"));
    result.oldPercentile = value["oldPercentile"].asDouble().unwrapOr(100.);
    result.newPercentile = value["newPercentile"].asDouble().unwrapOr(100.);
    result.placementBefore = static_cast<int>(intAt(value, "placementBefore"));
    result.placementAfter = static_cast<int>(intAt(value, "placementAfter"));
    result.newPopulation = static_cast<int>(intAt(value, "newPopulation"));
    result.placementComplete = boolAt(value, "placementComplete");
    result.winStreak = static_cast<int>(intAt(value, "winStreak"));
    result.streakBonus = value["streakBonus"].asDouble().unwrapOr(0.);
    result.mapKnown = value.contains("mapBest") && value.contains("opponentMapBest");
    if (result.mapKnown) {
        result.mapBest = static_cast<int>(std::clamp<int64_t>(intAt(value, "mapBest", -1), -1, 100));
        result.opponentMapBest = static_cast<int>(std::clamp<int64_t>(intAt(value, "opponentMapBest", -1), -1, 100));
    }
    result.lossReduction = std::clamp(value["lossReduction"].asDouble().unwrapOr(0.), 0., 1.);
    result.fullLoss = static_cast<int>(std::clamp<int64_t>(intAt(value, "fullLoss"), -10000, 0));
    result.duel = boolAt(value, "duel");
    return result;
}
// 듀얼 요청 파싱. id가 없으면 없는 것으로 본다.
std::optional<RankedDuelRequest> parseDuelRequest(Json const& value) {
    if (!value.isObject()) return std::nullopt;
    RankedDuelRequest request;
    request.id = stringAt(value, "id");
    request.state = stringAt(value, "state");
    request.createdAt = intAt(value, "createdAt");
    request.expiresAt = intAt(value, "expiresAt");
    request.decidedAt = intAt(value, "decidedAt");
    request.anonymous = boolAt(value, "anonymous");
    if (value["player"].isObject()) request.player = parseRankedProfile(value["player"]);
    request.cup = stringAt(value, "cup");
    request.cupName = stringAt(value, "cupName");
    if (value.contains("tier")) request.tier = tierAt(value, "tier", "Unranked");
    request.tierName = stringAt(value, "tierName");
    request.reason = stringAt(value, "reason");
    if (request.id.empty()) return std::nullopt;
    return request;
}
// 회피(dodge) 상태 파싱. 필드가 없으면 기본 규칙값을 쓰고, 객체가 있을 때만 known을 true로 한다.
RankedDodge parseDodge(Json const& source) {
    RankedDodge dodge;
    if (!source.isObject()) return dodge;
    auto number = [&](char const* key, int fallback) { return static_cast<int>(intAt(source, key, fallback)); };
    dodge.known = true;
    dodge.stack = number("stack", 0);
    dodge.limit = std::max(1, number("limit", 3));
    dodge.afterMap = number("afterMap", 2);
    dodge.cleanMaps = number("cleanMaps", 0);
    dodge.clearMaps = std::max(1, number("clearMaps", 5));
    dodge.blockMinutes = number("blockMinutes", 20);
    dodge.downloadFree = number("downloadFree", 2);
    dodge.downloadWeight = number("downloadWeight", 2);
    dodge.penaltyLosses = number("penaltyLosses", 2);
    dodge.penalties = number("penalties", 0);
    dodge.penaltyDelta = number("penaltyDelta", 0);
    dodge.blockedUntil = intAt(source, "blockedUntil");
    dodge.penaltyAt = intAt(source, "penaltyAt");
    dodge.season2 = intAt(source, "season", 1) == 2;
    dodge.penaltyCup = stringAt(source, "penaltyCup");
    return dodge;
}
// ranked/state 응답을 RankedState로 변환한다 (내 프로필, 대기열, 듀얼 요청, 진행 중인 매치).
// 응답 크기/범위를 믿지 않고 목록 길이와 숫자 범위를 제한해 읽는다.
RankedState parseRankedState(Json const& value) {
    RankedState result;
    result.profile = parseRankedProfile(value["profile"]);
    result.searchingPlayers = static_cast<int>(intAt(value, "searchingPlayers"));
    result.playingPlayers = static_cast<int>(intAt(value, "playingPlayers"));
    result.queuedAt = intAt(value, "queuedAt");
    result.blockedUntil = intAt(value, "blockedUntil");
    result.cancelStrikes = static_cast<int>(intAt(value, "cancelStrikes"));
    result.dodge = parseDodge(value["dodge"]);
    result.searchRange = static_cast<int>(intAt(value, "searchRange", 100));
    result.searchScale = static_cast<int>(std::clamp<int64_t>(intAt(value, "searchScale", 100), 100, 400));
    if (value["searchCancel"].isObject()) {
        result.searchCancelOutcome = stringAt(value["searchCancel"], "outcome");
        result.searchCancelsLeft = static_cast<int>(intAt(value["searchCancel"], "left", 0));
    }
    if (value["rematchGone"].isObject()) {  // 1.0.7
        result.rematchGoneMatch = stringAt(value["rematchGone"], "matchId");
        result.rematchGoneText = stringAt(value["rematchGone"], "reason");
    }
    result.allowDuel = value["allowDuel"].asBool().unwrapOr(true);
    result.duelIncoming = parseDuelRequest(value["duelIncoming"]);
    result.duelOutgoing = parseDuelRequest(value["duelOutgoing"]);
    if (value["searchers"].isArray())
        for (auto const& entry : value["searchers"].asArray().unwrap())
            result.searchers.push_back(parseRankedProfile(entry));
    if (value["match"].isObject()) {
        auto const& source = value["match"];
        RankedMatch match;
        match.id = stringAt(source, "id");
        match.roomId = stringAt(source, "roomId");
        match.state = stringAt(source, "state");
        match.cancelReason = stringAt(source, "cancelReason");
        match.drawId = stringAt(source, "drawId");
        match.rerollReason = stringAt(source, "rerollReason");
        if (source["failedMaps"].isArray())
            for (auto const& id : source["failedMaps"].asArray().unwrap())
                match.failedMaps.push_back(id.asInt().unwrapOr(0));
        if (source["playedMaps"].isArray())
            for (auto const& id : source["playedMaps"].asArray().unwrap())
                match.playedMaps.push_back(id.asInt().unwrapOr(0));
        match.rematchRound = static_cast<int>(intAt(source, "rematchRound"));
        if (source["downloadFail"].isObject()) {
            auto const& fail = source["downloadFail"];
            RankedDownloadFail value;
            value.count = static_cast<int>(intAt(fail, "count"));
            value.free = static_cast<int>(intAt(fail, "free", 2));
            value.weight = static_cast<int>(intAt(fail, "weight", 2));
            value.stack = static_cast<int>(intAt(fail, "stack"));
            value.resetAt = intAt(fail, "resetAt");
            value.dodged = boolAt(fail, "dodged");
            value.penalized = boolAt(fail, "penalized");
            value.noDodge = boolAt(fail, "noDodge");
            value.reason = stringAt(fail, "reason");
            match.downloadFail = std::move(value);
        }
        match.createdAt = intAt(source, "createdAt");
        match.selectedAt = intAt(source, "selectedAt");
        match.mapMask = static_cast<int>(intAt(source, "mapMask", 1));
        // mapBand: 이번 매치의 맵 범위(티어/순위/레이팅 구간)와 방 색상(main/sub, RGB 3개).
        // 색상은 둘 다 올바를 때만 쓴다.
        if (source["mapBand"].isObject()) {
            auto const& value = source["mapBand"];
            auto optional = [&](char const* key) -> std::optional<int> {
                if (!value[key].isNumber()) return std::nullopt;
                return static_cast<int>(value[key].asInt().unwrapOr(0));
            };
            RankedMapBand band;
            band.tier = tierAt(value, "tier");
            band.reason = stringAt(value, "reason");
            band.tierName = tierAt(value, "tierName");
            band.mask = static_cast<int>(intAt(value, "mask", match.mapMask));
            band.topFrom = optional("topFrom");
            band.topTo = optional("topTo");
            band.ratingMin = optional("ratingMin");
            band.ratingMax = optional("ratingMax");
            band.population = static_cast<int>(intAt(value, "population"));
            if (value["room"].isObject()) {
                auto colour = [&](char const* key) -> std::optional<uint32_t> {
                    auto const& rgb = value["room"][key];
                    if (!rgb.isArray() || rgb.size() != 3) return std::nullopt;
                    uint32_t packed = 0;
                    for (size_t i = 0; i < 3; ++i) {
                        if (!rgb[i].isNumber()) return std::nullopt;
                        auto const part = rgb[i].asInt().unwrapOr(-1);
                        if (part < 0 || part > 255) return std::nullopt;
                        packed = packed << 8 | static_cast<uint32_t>(part);
                    }
                    return packed;
                };
                band.roomMain = colour("main");
                band.roomSub = colour("sub");
                if (!band.roomMain || !band.roomSub) band.roomMain = band.roomSub = std::nullopt;
            }
            match.mapBand = std::move(band);
        }
        match.maxAttempts = static_cast<int>(intAt(source, "maxAttempts", 3));
        match.candidateProvider = boolAt(source, "candidateProvider");
        match.ownReady = boolAt(source, "ownReady");
        match.opponentReady = boolAt(source, "opponentReady");
        match.ownLdm = boolAt(source, "ownLdm");
        match.opponentLdm = boolAt(source, "opponentLdm");
        match.duel = boolAt(source, "duel");
        match.startAt = intAt(source, "startAt");
        match.ownReroll = boolAt(source, "ownReroll");
        match.opponentReroll = boolAt(source, "opponentReroll");
        match.quickReroll = boolAt(source, "quickReroll");
        match.rerolled = boolAt(source, "rerolled");
        match.voteRerolls = static_cast<int>(intAt(source, "voteRerolls"));
        // side: 라운지 화면에서 내 위치(left = -1, right = 1).
        if (auto const side = stringAt(source, "side"); side == "left" || side == "right")
            match.side = side == "left" ? -1 : 1;
        // ban: Season 2 맵 밴 단계 (종료 시각, 제한 시간(초), 각자 밴한 맵 id).
        if (source["ban"].isObject()) {
            auto const& value = source["ban"];
            RankedMatch::Bans bans;
            bans.endsAt = intAt(value, "endsAt");
            bans.seconds = static_cast<int>(std::clamp<int64_t>(intAt(value, "seconds", 20), 1, 120));
            bans.own = std::max<int64_t>(0, intAt(value, "own"));
            bans.opponent = std::max<int64_t>(0, intAt(value, "opponent"));
            bans.ownRandom = boolAt(value, "ownRandom");
            bans.opponentRandom = boolAt(value, "opponentRandom");
            match.bans = bans;
        }
        match.opponent = parseRankedProfile(source["opponent"]);
        if (source["candidates"].isArray())
            for (auto const& candidate : source["candidates"].asArray().unwrap())
                match.candidates.push_back(parseLevel(candidate));
        if (source["selectedMap"].isObject()) match.selectedMap = parseLevel(source["selectedMap"]);
        if (source["result"].isObject()) match.result = parseRankedResult(source["result"]);
        // mapRules: 맵 조건 (길이 / 기능 코드 0~4, 최소 좋아요, 최대 길이(초), 2P 제외, GDDL 범위).
        if (source["mapRules"].isObject()) {
            auto const& rules = source["mapRules"];
            auto codes = [&](char const* key, std::vector<int>& into) {
                if (rules[key].isArray())
                    for (auto const& item : rules[key].asArray().unwrap()) {
                        int const value = static_cast<int>(item.asInt().unwrapOr(-1));
                        if (value >= 0 && value <= 4) into.push_back(value);
                    }
            };
            codes("lengths", match.mapLengths);
            codes("features", match.mapFeatures);
            match.minLikes =
                static_cast<int>(std::clamp<int64_t>(rules["minLikes"].asInt().unwrapOr(0), 0, 100'000'000));
            match.maxSeconds =
                static_cast<int>(std::clamp<int64_t>(rules["maxSeconds"].asInt().unwrapOr(0), 0, 86'400));
            match.noTwoPlayer = rules["noTwoPlayer"].asBool().unwrapOr(false);
            if (rules["gddl"].isArray()) {
                auto const& range = rules["gddl"].asArray().unwrap();
                if (range.size() == 2) {
                    match.gddlLow = static_cast<int>(range.front().asInt().unwrapOr(-1));
                    match.gddlHigh = static_cast<int>(range.back().asInt().unwrapOr(-1));
                }
            }
        }
        // gddlTiers / gddlPool / gddlCheck: GDDL 티어 정보. 맵 id 범위, 티어 1~39, 항목 수 상한을
        // 두어 비정상 응답을 걸러 낸다. gddlCheck는 key가 비었거나 너무 길면 버린다.
        if (source["gddlTiers"].isObject())
            for (auto const& entry : source["gddlTiers"]) {
                auto const key = entry.getKey().value_or("");
                int64_t id = 0;
                bool digits = !key.empty() && key.size() <= 10;
                for (char c : key) digits = digits && c >= '0' && c <= '9';
                if (digits) id = std::stoll(key);
                int const tier = static_cast<int>(entry.asInt().unwrapOr(0));
                if (id > 0 && id <= 2'147'483'647 && tier >= 1 && tier <= 39 && match.gddlTiers.size() < 8)
                    match.gddlTiers[id] = tier;
            }
        if (source["gddlPool"].isArray())
            for (auto const& id : source["gddlPool"].asArray().unwrap()) {
                int64_t const value = id.asInt().unwrapOr(0);
                if (value > 0 && value <= 2'147'483'647 && match.gddlPool.size() < 100)
                    match.gddlPool.push_back(static_cast<int>(value));
            }
        if (auto const& check = source["gddlCheck"]; check.isObject() && check["ids"].isArray()) {
            match.gddlCheckKey = stringAt(check, "key");
            for (auto const& id : check["ids"].asArray().unwrap()) {
                int64_t const value = id.asInt().unwrapOr(0);
                if (value > 0 && value <= 2'147'483'647 && match.gddlCheck.size() < 200)
                    match.gddlCheck.push_back(static_cast<int>(value));
            }
            if (match.gddlCheckKey.empty() || match.gddlCheckKey.size() > 4096) match.gddlCheck.clear();
        }
        result.match = std::move(match);
    }
    result.season = static_cast<int>(intAt(value, "season", 1));
    result.cup = stringAt(value, "cup");
    result.ratingMax = static_cast<int>(intAt(value, "ratingMax"));
    result.matchCup = stringAt(value, "matchCup");
    return result;
}
// 방 비밀번호 형식 검사.
bool validPin(std::string const& pin) {
    return validRoomPin(pin);
}
// 무작위 32자리 16진수 id.
std::string randomId() {
    std::random_device random;
    std::string result;
    constexpr char digits[] = "0123456789abcdef";
    for (int index = 0; index < 32; ++index) result += digits[random() & 15];
    return result;
}
// 요청 URL: 서버 주소 / 디버그 API 루트 / 경로.json
std::string endpoint(std::string const& path) {
    return fmt::format("{}/{}/{}.json", firebase_config::DATABASE_URL, debug::apiRoot(), path);
}
// 점검 안내 문구 (접두사 + 업데이트 중/재시작 중 + 남은 시간).
std::string maintenanceText(MaintenanceInfo const& info) {
    return fmt::format("{}{} {}", kMaintenancePrefix,
                       info.startedAt > 0 ? "Versus is being updated." : "The Versus server is restarting.",
                       maintenanceTimeLeft(info, nowMs()));
}
// 점검 상태가 이 시간 동안 유지되면 업데이트 중으로 보지 않는다.
constexpr auto kMaintenanceWait = std::chrono::minutes(10);
// 응답 헤더로 서버 점검 상태를 갱신한다.
// X-Versus-Maintenance가 "down"이면 본문에서 시작/예상 종료/버전을 읽고, 그 외에는
// "시작ms,예상종료ms,버전" 형식으로 읽는다. 헤더가 없을 때 응답이 없거나(0, 502~504)
// 점검 중이던 서버가 정상 응답하면 각각 down 표시 / 점검 해제로 처리한다.
void noteMaintenance(web::WebResponse const& response) {
    auto& s = state();
    int const code = response.code();
    if (code < 0) return;
    auto adopt = [&](MaintenanceInfo info) {
        if (!s.maintenance) s.maintenanceSince = Clock::now();
        if (info.startedAt <= 0 && s.maintenance && s.maintenance->startedAt > 0) {
            info.startedAt = s.maintenance->startedAt;
            info.expectedEndAt = s.maintenance->expectedEndAt;
            info.version = s.maintenance->version;
        }
        if (s.maintenance && s.maintenance->startedAt != info.startedAt) s.maintenanceBanner = -1;
        s.maintenance = std::move(info);
    };
    auto header = response.header("X-Versus-Maintenance");
    if (header && *header == "down") {
        MaintenanceInfo info;
        info.down = true;
        if (auto parsed = response.json(); parsed && parsed.unwrap().isObject()) {
            auto const& value = parsed.unwrap();
            if (boolAt(value, "active")) {
                info.startedAt = intAt(value, "startedAt");
                info.expectedEndAt = intAt(value, "expectedEndAt");
                info.version = stringAt(value, "version");
            }
        }
        adopt(std::move(info));
        return;
    }
    if (header) {
        MaintenanceInfo info;
        std::string const text(*header);
        auto const first = text.find(','), second = first == std::string::npos ? first : text.find(',', first + 1);
        if (second != std::string::npos) {
            info.startedAt = std::strtoll(text.substr(0, first).c_str(), nullptr, 10);
            info.expectedEndAt = std::strtoll(text.substr(first + 1, second - first - 1).c_str(), nullptr, 10);
            info.version = text.substr(second + 1);
        }
        if (info.startedAt > 0 && info.expectedEndAt >= info.startedAt) adopt(std::move(info));
        return;
    }
    bool const noAnswer = code == 0 || code == 502 || code == 503 || code == 504;
    if (noAnswer) {
        if (s.maintenance) s.maintenance->down = true;
        return;
    }
    if (s.maintenance) {
        s.maintenance.reset();
        s.lastContact = Clock::now();
        s.maintenanceBanner = -1;
        if (std::exchange(s.maintenanceSeen, false)) s.maintenanceOver = true;
    }
}
// 점검 중이면 요청 실패 사유를 점검 안내로 바꾼다. 내 버전이 낡아서 온 426과 일반 4xx 오류는 점검으로 보지 않는다.
std::optional<std::string> maintenanceError(web::WebResponse const& response) {
    auto& s = state();
    if (!s.maintenance) return std::nullopt;
    int const code = response.code();
    auto latest = response.header("X-Versus-Latest");
    if (code == 426 && latest && *latest != debug::wireVersion()) return std::nullopt;
    if (code != 0 && code != 426 && code < 500) return std::nullopt;
    s.maintenanceSeen = true;
    return maintenanceText(*s.maintenance);
}
// 실패한 응답을 사용자에게 보여 줄 문구로 바꾼다 (점검 / 버전 / 권한 / 412 충돌 / 네트워크).
std::string networkError(web::WebResponse const& response) {
    if (auto maintenance = maintenanceError(response)) return *maintenance;
    if (response.code() == 426) {
        auto latest = response.header("X-Versus-Latest");
        if (latest && *latest == debug::wireVersion())
            return "Versus is finishing an update. New matches open again in a moment.";
        return "Versus is out of date. Install the latest version to continue.";
    }
    if (response.code() == 401 || response.code() == 403) {
        auto body = response.json();
        auto const error = body ? stringAt(body.unwrap(), "error") : std::string();
        constexpr std::string_view prefix = "Permission denied: ";
        if (error.starts_with(prefix) && error.size() > prefix.size() &&
            error.substr(prefix.size()) != "Permission denied")
            return error.substr(prefix.size());
        if (body && stringAt(body.unwrap(), "error").starts_with("Permission denied"))
            return "Firebase database rules denied this request.";
        return "Firebase authentication failed. Reconnect to Versus.";
    }
    if (response.code() == 412) return "Room changed. Please try again.";
    return "Unable to reach Versus. Check your connection and retry.";
}
using Response = std::function<void(web::WebResponse)>;
// 서버 세션 토큰이 만료되기 이 시간(ms) 전부터는 쓰지 않는다.
constexpr int64_t kSessionMarginMs = 15'000;
bool sessionUsable() {
    auto const& s = state();
    return !s.session.empty() && !s.token.empty() && s.sessionBase == s.token &&
           s.sessionExpiresMs - nowMs() > kSessionMarginMs;
}
void forgetSession() {
    auto& s = state();
    s.session.clear();
    s.sessionBase.clear();
    s.sessionExpiresMs = 0;
}
// 서버가 X-Versus-Session 헤더로 발급한 세션 토큰을 검사하고 저장한다.
// 형식은 "vs1.<uid>.<만료(초)>.<서명>"이며, 현재 토큰과 내 uid에 맞는 것만 받는다.
void adoptSession(std::string_view header, std::string const& base) {
    auto& s = state();
    if (base.empty() || base != s.token || header.size() < 10 || header.size() > 200 || !header.starts_with("vs1."))
        return;
    for (char c : header)
        if (!std::isalnum(static_cast<unsigned char>(c)) && c != '_' && c != '-' && c != '.') return;
    auto const last = header.rfind('.');
    auto const middle =
        last == std::string_view::npos || last <= 4 ? std::string_view::npos : header.rfind('.', last - 1);
    if (middle == std::string_view::npos || middle <= 4 || last - middle < 2 || last - middle > 12) return;
    if (header.substr(4, middle - 4) != s.profile.uid) return;
    int64_t expires = 0;
    for (char c : header.substr(middle + 1, last - middle - 1)) {
        if (c < '0' || c > '9') return;
        expires = expires * 10 + (c - '0');
    }
    if (expires <= 0) return;
    s.session = std::string(header);
    s.sessionBase = base;
    s.sessionExpiresMs = expires * 1000;
}
// 서버 요청의 공통 진입점. 인증 헤더(세션 토큰이 있으면 세션, 없으면 로그인 토큰 + 세션 요청),
// if-match / If-None-Match(ETag), 범위 조회 파라미터를 붙여 비동기로 보낸다.
// 세션으로 보낸 요청이 401이면 세션을 버리고 로그인 토큰으로 한 번 다시 보낸다.
// 응답 헤더에서는 서버 시계, 점검, 최신 버전, 방송 id, 관전 여부, 접속자 수, 시즌,
// 멤버십 정보를 읽어 상태에 반영한 뒤 콜백을 호출한다.
void request(std::string method, std::string path, Json body, Response callback, std::string etag = {},
             bool wantEtag = false, bool roomList = false, bool history = false, std::string firstRun = {},
             std::string action = {}, std::string ifNoneEtag = {}, int timeoutSeconds = 8, bool idToken = false,
             std::string room = {}) {
    auto request = web::WebRequest();
    request.timeout(std::chrono::seconds(timeoutSeconds));
    request.header("Cache-Control", "no-cache");
    std::string const base = state().token;
    std::string const session = !idToken && sessionUsable() ? state().session : std::string();
    request.header("Authorization", "Bearer " + (session.empty() ? base : session));
    if (session.empty() && !base.empty()) request.header("X-Versus-Session", "1");
    struct Again {
        std::string method, path;
        Json body;
        std::string etag;
        bool wantEtag, roomList, history;
        std::string firstRun, action, ifNoneEtag;
        int timeoutSeconds;
        std::string room;
    };
    std::shared_ptr<Again> again;
    if (!session.empty())
        again = std::make_shared<Again>(Again{method, path, body, etag, wantEtag, roomList, history, firstRun, action,
                                              ifNoneEtag, timeoutSeconds, room});
    if (!room.empty()) request.header("X-Versus-Room", std::move(room));
    request.header("X-Versus-Version", debug::wireVersion());
    if (debug::active()) request.header("X-Versus-Debug", "1");
    if (wantEtag) request.header("X-Firebase-ETag", "true");
    if (!etag.empty()) request.header("if-match", std::move(etag));
    if (!ifNoneEtag.empty()) request.header("If-None-Match", std::move(ifNoneEtag));
    if (method == "GET" && path == "ranked/state")
        request.header("X-Versus-Roster", state().rosterTag.empty() ? std::string("-") : state().rosterTag);
    if (!action.empty()) request.header("X-Versus-Action", std::move(action));
    if (roomList) {
        request.param("orderBy", "\"updatedAt\"");
        request.param("limitToLast", firebase_config::ROOM_LIMIT);
        request.param("startAt", nowMs() - firebase_config::PRESENCE_TIMEOUT_MS);
    }
    if (history) {
        request.param("orderBy", "\"playedAt\"");
        request.param("limitToLast", 10);
    }
    if (!firstRun.empty()) {
        request.param("orderBy", "\"run\"");
        request.param("startAt", std::stoi(firstRun));
        request.param("endAt", std::stoi(firstRun) + 9);
        request.param("limitToFirst", 10);
    }
    if (method != "GET") {
        request.header("Content-Type", "application/json");
        request.bodyString(body.dump());
    }
    // 방 PUT 응답의 updatedAt(서버 타임스탬프)은 정밀한 시계 측정에 쓴다.
    bool const clockSample =
        method == "PUT" && path.starts_with("rooms/") && !path.ends_with("/battle/player") && body.isObject();
    auto const sentAt = Clock::now();
    auto const sentSystem = systemNowMs();
    auto const url = endpoint(path);
    async::spawn(request.send(std::move(method), url), [callback = std::move(callback), clockSample, sentAt, sentSystem,
                                                        path = std::move(path), base, session,
                                                        again = std::move(again)](web::WebResponse response) mutable {
        // 세션 만료(401) 처리: 같은 요청을 로그인 토큰으로 한 번만 다시 보낸다.
        if (again && response.code() == 401) {
            if (state().session == session) forgetSession();
            if (state().token != base) {
                callback(std::move(response));
                return;
            }
            auto& a = *again;
            versus::request(std::move(a.method), std::move(a.path), std::move(a.body), std::move(callback),
                            std::move(a.etag), a.wantEtag, a.roomList, a.history, std::move(a.firstRun),
                            std::move(a.action), std::move(a.ifNoneEtag), a.timeoutSeconds, true, std::move(a.room));
            return;
        }
        if (auto issued = response.header("X-Versus-Session")) adoptSession(*issued, base);
        syncServerClock(response, sentSystem);
        noteMaintenance(response);
        auto latest = response.header("X-Versus-Latest");
        if (latest) debug::noteLatest(*latest);
        auto draining = response.header("X-Versus-Draining");
        if (auto broadcast = response.header("X-Versus-Broadcast")) {
            auto const id = std::strtoll(broadcast->c_str(), nullptr, 10);
            if (id > state().broadcastLatest) state().broadcastLatest = id;
            state().broadcastHeard = Clock::now();
        }
        if (auto watched = response.header("X-Versus-Watched"); watched && *watched == "1")
            state().liveWatchedAt = Clock::now();
        if (auto online = response.header("X-Versus-Online")) {
            auto const count = std::strtol(online->c_str(), nullptr, 10);
            if (count >= 0) state().onlineCount = static_cast<int>(std::min<long>(count, 9'999'999));
        }
        if (auto season = response.header("X-Versus-Season")) {
            std::string const text(*season);
            auto const comma = text.find(',');
            if (comma != std::string::npos) {
                auto const number = std::strtol(text.substr(0, comma).c_str(), nullptr, 10);
                auto const endsAt = std::strtoll(text.substr(comma + 1).c_str(), nullptr, 10);
                if (number > 0 && number < 1000 && endsAt > 0) {
                    state().season = static_cast<int>(number);
                    state().seasonEndsAt = endsAt;
                }
            }
        }
        if (auto member = response.header("X-Versus-Membership")) {
            bool active = *member == "1";
            if (active != state().membershipActive) {
                state().membershipActive = active;
                if (!state().room) refreshLocalProfile();
            }
        }
        // 서버가 더 새 버전을 알리거나(426, X-Versus-Latest) 종료 중(Draining)이면 업데이트 대기 상태로 두고,
        // 내 버전이 최신이라고 확인되면 해제한다.
        if (response.code() == 426 || (latest && *latest != debug::wireVersion()) || (draining && *draining == "1"))
            state().updatePending = true;
        else if (latest && *latest == debug::wireVersion() && response.code() > 0 && response.code() < 500)
            state().updatePending = false;
        if (response.code() == 401 || response.code() == 403)
            log::warn("Versus Firebase request denied at {} (HTTP {}): {}", path, response.code(),
                      networkError(response));
        // 왕복 시간의 절반을 보정해 서버 시계 오프셋을 정한다. 왕복이 가장 짧았던 측정이거나
        // 5분이 지난 경우에만 갱신한다.
        if (clockSample && response.ok()) {
            auto parsed = response.json();
            auto const serverStamp = parsed ? intAt(parsed.unwrap(), "updatedAt") : 0;
            if (serverStamp > 0) {
                auto const roundTrip =
                    std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - sentAt).count();
                auto& s = state();
                if (roundTrip <= s.bestClockRoundTrip || Clock::now() - s.clockMeasuredAt > std::chrono::minutes(5)) {
                    s.serverOffset = serverStamp + roundTrip / 2 - systemNowMs();
                    s.preciseServerClock = true;
                    s.bestClockRoundTrip = roundTrip;
                    s.clockMeasuredAt = Clock::now();
                }
            }
        }
        callback(std::move(response));
    });
}
// 방 / Group 연결 상태를 모두 버리고 안내 문구(notice)를 남긴다.
// Ranked 전투가 끝난 직후라면 안내는 생략한다.
void disconnected(std::string notice) {
    auto& s = state();
    if (!s.rankedMatch.empty() && s.room && s.room->battle && s.room->battle->finishedAt > 0) notice.clear();
    ++s.epoch;
    s.room.reset();
    s.polling = false;
    s.writing = false;
    s.maintenanceWriting = false;
    s.roomPollEtag.clear();
    s.battlePollEtag.clear();
    s.searchingMap = false;
    s.group.reset();
    s.groupEtag.clear();
    s.groupBattleEtag.clear();
    s.groupForfeitBattle.clear();
    s.localRoom = false;
    s.localBody = nullptr;
    s.notice = std::move(notice);
}
// 서버에서 받은 방 JSON을 현재 방으로 반영한다 (방이 바뀌면 ETag 초기화).
void adoptRoom(std::string const& id, Json const& value) {
    auto& s = state();
    if (!s.room || s.room->id != id) {
        s.roomPollEtag.clear();
        s.battlePollEtag.clear();
    }
    s.room = parseRoom(id, value);
    s.lastContact = Clock::now();
}
// 전투 응답을 현재 방에 반영한다. 방 id / 전투 id가 맞지 않으면 무시한다.
bool adoptBattle(std::string const& roomId, std::string const& matchId, Json const& value) {
    auto& s = state();
    if (!s.room || s.room->id != roomId || !value.isObject()) return false;
    auto battle = parseBattle(value);
    if (battle.id.empty() || (!matchId.empty() && battle.id != matchId)) return false;
    if (!s.room->battle || s.room->battle->id != battle.id) s.battlePollEtag.clear();
    s.room->battle = std::move(battle);
    s.lastContact = Clock::now();
    return true;
}
// Ranked 상태 응답을 반영한다. 응답에 매치와 방 정보가 있으면 그 방을 현재 방으로 채택하고
// (방이 바뀌면 epoch를 올려 이전 요청 결과를 버린다), 매치가 사라지면 Ranked 방을 정리한다.
// 봇 전투 중에는 상태를 건드리지 않는다.
RankedState adoptRankedState(Json const& value) {
    auto parsed = parseRankedState(value);
    auto& s = state();
    ++s.revision;
    if (s.botBattle) return parsed;
    if (parsed.match) {
        s.rankedMatch = parsed.match->id;
        s.rankedLdm = parsed.match->ownLdm;
        auto const& roomValue = value["match"]["room"];
        if (roomValue.isObject() && !parsed.match->roomId.empty()) {
            if (!s.room || s.room->id != parsed.match->roomId) {
                ++s.epoch;
                s.writing = s.maintenanceWriting = s.polling = s.searchingMap = false;
                s.returningMatch.clear();
                s.heartbeatTime = 0.f;
            }
            adoptRoom(parsed.match->roomId, roomValue);
            s.pollTime = 0.f;
        } else if (s.room && s.room->id == parsed.match->roomId) {
            ++s.epoch;
            s.room.reset();
            s.polling = false;
            s.writing = false;
            s.roomPollEtag.clear();
            s.battlePollEtag.clear();
            s.maintenanceWriting = s.searchingMap = false;
        }
    } else if (!s.rankedMatch.empty()) {
        if (s.room && s.room->id.starts_with("ranked-")) {
            ++s.epoch;
            s.room.reset();
            s.polling = false;
            s.writing = false;
            s.roomPollEtag.clear();
            s.battlePollEtag.clear();
            s.maintenanceWriting = s.searchingMap = false;
        }
        s.rankedMatch.clear();
        s.rankedLdm = false;
    }
    return parsed;
}

// 인증/쓰기 중이라 지금 보낼 수 없는 방 액션을 큐에 넣는다. 방 세션이 바뀌면 실패로 돌려준다.
void deferRoomAction(std::function<void(Done)> action, Done callback) {
    auto const epoch = state().epoch;
    state().pendingActions.push_back([epoch, action = std::move(action), callback = std::move(callback)]() mutable {
        if (epoch != state().epoch) {
            callback(false, "Room session changed.");
            return;
        }
        action(std::move(callback));
    });
}

// 방 갱신이 실패했을 때, 서버의 방을 다시 읽어 내가 아직 호스트/게스트로 남아 있고
// 호스트 접속 신호가 유효하면 성공으로 간주한다.
void recoverMembership(std::string id, uint64_t epoch, bool host, std::string error, Done callback) {
    request(
        "GET", "rooms/" + id, {},
        [id, epoch, host, error = std::move(error), callback = std::move(callback)](web::WebResponse response) mutable {
            auto& s = state();
            if (epoch != s.epoch) {
                callback(false, "Room session changed.");
                return;
            }
            auto parsed = response.json();
            if (response.ok() && parsed && parsed.unwrap().isObject()) {
                auto const& body = parsed.unwrap();
                if (stringAt(std::as_const(body)[host ? "host" : "guest"], "uid") == s.profile.uid &&
                    nowMs() - intAt(body, "hostSeen") <= firebase_config::PRESENCE_TIMEOUT_MS) {
                    adoptRoom(id, body);
                    callback(true, {});
                    return;
                }
            }
            callback(false, std::move(error));
        });
}

// 방 JSON을 직접 고치는 함수. 오류 문구를 돌려주며 빈 문자열이면 성공이다.
using Mutator = std::function<std::string(Json&)>;
bool localRoomWrite(std::string const& id, uint64_t epoch, Mutator const& change, Done& callback);
// Mutator가 "바꿀 것이 없다"는 뜻으로 돌려주는 특수 값.
constexpr char const* ROOM_UNCHANGED = "\x1froom-unchanged";
// 방 JSON을 읽고(ETag 포함) Mutator로 고친 뒤 if-match와 함께 PUT하는 낙관적 갱신.
// 그 사이 상대가 먼저 바꿔 412가 나오면 최대 attempts번까지 다시 읽어서 시도한다.
void mutateRoom(std::string id, uint64_t epoch, std::string action, Mutator change, Done callback, int attempts = 3) {
    if (localRoomWrite(id, epoch, change, callback)) return;
    if (!Service::get().connected()) {
        Service::get().connect([id = std::move(id), epoch, action = std::move(action), change = std::move(change),
                                callback = std::move(callback), attempts](bool ok, std::string error) mutable {
            if (epoch != state().epoch) {
                callback(false, "Room session changed.");
                return;
            }
            if (!ok) {
                callback(false, std::move(error));
                return;
            }
            mutateRoom(std::move(id), epoch, std::move(action), std::move(change), std::move(callback), attempts);
        });
        return;
    }
    ++state().revision;
    request(
        "GET", "rooms/" + id, {},
        [id, epoch, action = std::move(action), change = std::move(change), callback = std::move(callback),
         attempts](web::WebResponse response) mutable {
            auto& s = state();
            if (epoch != s.epoch) {
                callback(false, "Room session changed.");
                return;
            }
            auto parsed = response.json();
            auto etag = response.header("ETag");
            if (!response.ok() || !parsed || !etag) {
                callback(false, networkError(response));
                return;
            }
            auto body = parsed.unwrap();
            if (!body.isObject()) {
                callback(false, "The host left the room.");
                return;
            }
            auto error = change(body);
            if (error == ROOM_UNCHANGED) {
                adoptRoom(id, body);
                callback(true, {});
                return;
            }
            if (!error.empty()) {
                callback(false, std::move(error));
                return;
            }
            if (!body.isNull()) body["updatedAt"] = timestamp();
            request(
                "PUT", "rooms/" + id, body,
                [id, epoch, action, change = std::move(change), callback = std::move(callback),
                 attempts](web::WebResponse result) mutable {
                    if (epoch != state().epoch) {
                        callback(false, "Room session changed.");
                        return;
                    }
                    if (result.code() == 412 && attempts > 1) {
                        mutateRoom(id, epoch, action, std::move(change), std::move(callback), attempts - 1);
                        return;
                    }
                    if (!result.ok()) {
                        callback(false, networkError(result));
                        return;
                    }
                    auto parsed = result.json();
                    if (!parsed) {
                        callback(false, "Invalid room response. Please retry.");
                        return;
                    }
                    if (parsed.unwrap().isObject()) adoptRoom(id, parsed.unwrap());
                    callback(true, {});
                },
                std::string(*etag), false, false, false, {}, std::string(action));
        },
        {}, true);
}
// 방 상태를 GET으로 받아 반영한다 (ETag가 같으면 304).
// 호스트가 방을 지웠거나, 호스트 접속 신호가 끊겼거나, 내가 더 이상 방의 멤버가 아니면
// (밴 / 강퇴 / 만료) 안내 문구와 함께 방을 떠난 상태로 만든다.
void pollRoom() {
    auto& s = state();
    if (!s.room || s.polling || s.writing) return;
    if (s.localRoom) {
        s.pollTime = 0.f;
        return;
    }
    s.polling = true;
    s.pollTime = 0;
    auto const id = s.room->id;
    auto const epoch = s.epoch;
    auto const revision = s.revision;
    auto const etag = s.roomPollEtag;
    request(
        "GET", "rooms/" + id, {},
        [id, epoch, revision](web::WebResponse response) {
            auto& s = state();
            if (epoch != s.epoch) return;
            s.polling = false;
            s.pollTime = 0.f;
            if (s.writing || revision != s.revision) return;
            if (response.code() == 304) {
                s.lastContact = Clock::now();
                return;
            }
            auto parsed = response.json();
            if (!response.ok() || !parsed) return;
            auto const body = parsed.unwrap();
            if (body.isNull()) {
                disconnected("The host left the room.");
                return;
            }
            if (!body.isObject()) return;
            if (stringAt(std::as_const(body)["host"], "uid") != s.profile.uid) {
                if (s.rankedMatch.empty() && nowMs() - intAt(body, "hostSeen") > firebase_config::PRESENCE_TIMEOUT_MS) {
                    disconnected("The host disconnected.");
                    return;
                }
                if (stringAt(std::as_const(body)["guest"], "uid") != s.profile.uid) {
                    bool banned = false;
                    if (std::as_const(body)["banned"].isArray())
                        for (auto const& item : std::as_const(body)["banned"].asArray().unwrap())
                            if (item.isString() && item.asString().unwrap() == s.profile.uid) banned = true;
                    disconnected(banned ? "You were banned from this room by the host."
                                 : stringAt(body, "lastKickUid") == s.profile.uid
                                     ? "You were removed from the room by the host."
                                     : "Your room connection expired.");
                    return;
                }
            }
            adoptRoom(id, body);
            if (auto fresh = response.header("ETag")) s.roomPollEtag = *fresh;
        },
        {}, false, false, false, {}, {}, std::move(etag));
}
// 전투 상태만 GET으로 가볍게 폴링한다. 응답이 현재 전투와 맞지 않으면 방 전체를 다시 받는다.
void pollBattle() {
    auto& s = state();
    if (!s.room || !s.room->battle || s.polling || s.writing) return;
    if (s.localRoom) {
        s.pollTime = 0.f;
        return;
    }
    s.polling = true;
    s.pollTime = 0.f;
    auto const roomId = s.room->id;
    auto const matchId = s.room->battle->id;
    auto const epoch = s.epoch;
    auto const revision = s.revision;
    auto const etag = s.battlePollEtag;
    request(
        "GET", "rooms/" + roomId + "/battle", {},
        [roomId, matchId, epoch, revision](web::WebResponse response) {
            auto& s = state();
            if (epoch != s.epoch) return;
            s.polling = false;
            s.pollTime = 0.f;
            if (s.writing || revision != s.revision) return;
            if (response.code() == 304) {
                s.lastContact = Clock::now();
                return;
            }
            auto parsed = response.json();
            if (!response.ok() || !parsed) return;
            if (adoptBattle(roomId, matchId, parsed.unwrap())) {
                if (auto fresh = response.header("ETag")) s.battlePollEtag = *fresh;
            } else {
                pollRoom();
            }
        },
        {}, false, false, false, {}, {}, std::move(etag));
}
// 내 맵/음악/효과음 다운로드 진행률과 로컬 최고 기록을 방 JSON의 downloads에 올린다.
// 2초 간격이며 값이 그대로면 10초까지는 다시 보내지 않고, 전투가 시작되면 멈춘다.
void reportDownload() {
    auto& s = state();
    if (!s.room || s.writing || s.polling || s.authenticating ||
        (s.room->started && (!s.room->launch || s.room->launch->releasedAt > 0)) || s.room->level.id <= 0 ||
        Clock::now() < s.nextDownloadReport)
        return;
    s.nextDownloadReport = Clock::now() + std::chrono::seconds(2);
    auto const levelID = s.room->level.id;
    auto const progress = mapDownloadProgress(levelID, s.room->id.starts_with("ranked-"));
    auto const best = mapLocalBest(levelID);
    bool const host = s.room->host.uid == s.profile.uid;
    auto const& own = host ? s.room->hostDownload : s.room->guestDownload;
    if (own.uid == s.profile.uid && own.levelId == levelID && own.percent == progress.percent && own.best == best &&
        own.mapReady == progress.mapReady && own.songsDone == progress.songsDone &&
        own.songsTotal == progress.songsTotal && own.soundsDone == progress.soundsDone &&
        own.soundsTotal == progress.soundsTotal && (progress.complete() || nowMs() - own.updatedAt < 10000))
        return;
    auto const epoch = s.epoch;
    s.writing = s.maintenanceWriting = true;
    mutateRoom(
        s.room->id, epoch, "download",
        [levelID, progress, best](Json& body) -> std::string {
            auto const uid = state().profile.uid;
            bool const host = stringAt(std::as_const(body)["host"], "uid") == uid;
            if (!host && stringAt(std::as_const(body)["guest"], "uid") != uid) return "Your room connection expired.";
            if ((boolAt(body, "started") && intAt(std::as_const(body)["launch"], "releasedAt") > 0) ||
                intAt(std::as_const(body)["level"], "id") != levelID)
                return ROOM_UNCHANGED;
            auto value = Json::object();
            value["uid"] = uid;
            value["levelId"] = levelID;
            value["percent"] = progress.percent;
            value["mapReady"] = progress.mapReady;
            value["songsDone"] = progress.songsDone;
            value["songsTotal"] = progress.songsTotal;
            value["soundsDone"] = progress.soundsDone;
            value["soundsTotal"] = progress.soundsTotal;
            value["best"] = best;
            value["updatedAt"] = timestamp();
            body["downloads"][host ? "host" : "guest"] = value;
            return {};
        },
        [epoch](bool ok, std::string detail) {
            if (epoch != state().epoch) return;
            state().writing = state().maintenanceWriting = false;
            if (!ok) {
                state().nextDownloadReport = Clock::now() + std::chrono::seconds(10);
                log::warn("Versus download progress sync: {}", detail);
            }
        });
}
// 방 존재 신호(heartbeat). 호스트는 hostSeen, 게스트는 guestSeen을 서버 시각으로 갱신한다.
// 호스트는 끊긴 게스트를 정리하고, 게스트는 호스트 신호가 끊겼으면 방을 떠난다 (Ranked 방 제외).
// 핵을 허용한 방에서는 내가 감지한 외부 치트 정보(최대 8개)를 hacks에 함께 올린다.
void heartbeat() {
    auto& s = state();
    if (!s.room || s.writing || s.polling) return;
    s.writing = true;
    s.maintenanceWriting = true;
    s.heartbeatTime = 0;
    auto const id = s.room->id;
    auto const epoch = s.epoch;
    mutateRoom(
        id, epoch, "heartbeat",
        [](Json& body) -> std::string {
            auto const uid = state().profile.uid;
            if (stringAt(std::as_const(body)["host"], "uid") == uid) {
                body["hostSeen"] = timestamp();
                if (state().rankedMatch.empty() &&
                    roomGuestExpired(body, nowMs(), firebase_config::PRESENCE_TIMEOUT_MS)) {
                    dropPlayedPoolMap(body);
                    removeRoomGuest(body);
                }
            } else if (stringAt(std::as_const(body)["guest"], "uid") == uid) {
                if (state().rankedMatch.empty() &&
                    nowMs() - intAt(body, "hostSeen") > firebase_config::PRESENCE_TIMEOUT_MS)
                    return "The host disconnected.";
                body["guestSeen"] = timestamp();
            } else
                return "Your room connection expired.";
            if (!state().rankedMatch.empty() || !boolAt(std::as_const(body)["rules"], "allowHacks")) return {};
            auto hacks = Json::object();
            for (char const* side : {"host", "guest"}) {
                auto const participant = stringAt(std::as_const(body)[side], "uid");
                if (participant.empty()) continue;
                if (participant == uid) {
                    auto list = Json::array();
                    for (auto const& status : integrity::activeExternalCheats(false)) {
                        if (!status.detected || !integrity::externalCode(status.code) ||
                            !integrity::validFeatureDetails(status.code, status.features) || list.size() >= 8)
                            continue;
                        auto item = Json::object();
                        item["code"] = status.code;
                        item["features"] = status.features;
                        list.push(std::move(item));
                    }
                    hacks[participant] = std::move(list);
                } else if (std::as_const(body)["hacks"].contains(participant)) {
                    hacks[participant] = std::as_const(body)["hacks"][participant];
                }
            }
            body["hacks"] = std::move(hacks);
            return {};
        },
        [epoch](bool success, std::string error) {
            if (epoch != state().epoch) return;
            state().writing = false;
            state().maintenanceWriting = false;
            if (success) state().lastSeatAck = Clock::now();
            auto const& room = state().room;
            bool const live = room && room->started && room->battle && room->battle->finishedAt == 0 && room->launch &&
                              room->launch->releasedAt > 0;
            if (!success && !live) state().heartbeatTime = std::max(state().heartbeatTime, 8.f);
            if (!success && (error == "The host left the room." || error == "The host disconnected." ||
                             error == "Your room connection expired."))
                disconnected(std::move(error));
        });
}
// 씬에 붙지 않고 스케줄러에 등록되어 매 프레임 Service::tick을 호출하는 노드.
class ServiceTicker final : public CCNode {
public:
    void update(float dt) override { Service::get().tick(dt); }
};
// 인증 절차를 끝내고 대기 중이던 connect 콜백들을 모두 호출한다.
void finishAuthentication(bool success, std::string error) {
    auto& s = state();
    s.authenticating = false;
    auto callbacks = std::move(s.connectCallbacks);
    s.connectCallbacks.clear();
    for (auto& callback : callbacks)
        if (callback) callback(success, error);
}
void authenticate(std::string key, bool refresh);
constexpr char const* kGdLoginRequired = "Log in to a Geometry Dash account to play Versus.";
// 서버에 저장된 계정 꾸미기를 받아 로컬 저장값과 프로필에 반영한다 (방에 있을 때는 건너뜀).
void adoptAccountCosmetics(Json const& value) {
    auto& s = state();
    if (!value.isObject() || s.room) return;
    auto cosmetics = cosmeticsForEdition(parseCosmetics(value, s.profile.cosmetics), s.profile.membership);
    Mod::get()->setSavedValue(cosmeticsSaveKey(s.profile.membership), cosmeticsJson(cosmetics));
    s.profile.cosmetics = cosmetics;
    s.profile.hasCosmetics = true;
}
void uploadAccountCosmetics() {
    request("PUT", "account/cosmetics", cosmeticsJson(state().profile.cosmetics), [](web::WebResponse) {});
}
void syncAccount(std::string argonToken = {}, std::string argonError = {}, bool argonFresh = false);
// Argon 토큰을 기다리는 최대 시간(초).
constexpr int kArgonWaitSeconds = 15;
// Argon 토큰을 받아 콜백으로 넘긴다. 토큰 요청이 15초 안에 끝나지 않으면 "timeout"으로 한 번만 호출한다.
// Argon이 없는 빌드에서는 즉시 오류를 돌려준다.
void withArgonToken(int account, bool fresh, std::function<void(std::string, std::string)> done) {
#if VERSUS_ARGON
    if (fresh) argon::clearToken(account);
    auto pending = std::make_shared<std::function<void(std::string, std::string)>>(std::move(done));
    auto once = [pending](std::string token, std::string error) {
        if (!*pending) return;
        auto callback = std::exchange(*pending, nullptr);
        callback(std::move(token), std::move(error));
    };
    async::spawn(argon::startAuth(), [once](Result<std::string> result) {
        if (result.isErr())
            once({}, result.unwrapErr().substr(0, 180));
        else
            once(std::move(result).unwrap(), {});
    });
    std::thread([once] {
        std::this_thread::sleep_for(std::chrono::seconds(kArgonWaitSeconds));
        Loader::get()->queueInMainThread([once] { once({}, "timeout"); });
    }).detach();
#else
    (void)account;
    (void)fresh;
    done({}, "Argon is not in this build");
#endif
}
// 계정 동기화 이후 백그라운드에서 Argon으로 GD 계정을 검증한다.
// 서버가 retry를 요청하면 새 토큰으로 한 번 더 시도한다.
void verifyArgonInBackground(int account, std::string name, bool fresh = false) {
#if VERSUS_ARGON
    auto& s = state();
    if (!fresh && s.argonBackgroundTried) return;
    s.argonBackgroundTried = true;
    s.argonState = "checking";
    withArgonToken(account, fresh, [account, name, fresh](std::string token, std::string error) {
        auto& s = state();
        if (s.syncedAccount != account) return;
        if (token.empty()) {
            log::info("Versus: Argon check skipped ({})", error);
            s.argonState.clear();
            return;
        }
        auto body = Json::object();
        body["accountId"] = account;
        body["name"] = name;
        body["argonToken"] = token;
        if (fresh) body["argonFresh"] = true;
        request("PUT", "account/argon", std::move(body), [account, name, fresh](web::WebResponse response) {
            auto& s = state();
            auto parsed = response.json();
            if (!response.ok() || !parsed || !parsed.unwrap().isObject()) {
                s.argonState.clear();
                return;
            }
            auto const value = parsed.unwrap();
            if (boolAt(value, "retry") && !fresh && s.syncedAccount == account) {
                verifyArgonInBackground(account, name, true);
                return;
            }
            s.argonState = stringAt(value, "argon");
        });
    });
#else
    (void)account;
    (void)name;
    (void)fresh;
#endif
}
// 로그인한 신원(refresh token, uid)을 모드 저장소에 저장한다.
void saveIdentity(std::string const& refreshToken, std::string const& uid) {
    auto* mod = Mod::get();
    mod->setSavedValue("firebase-refresh-token", refreshToken);
    mod->setSavedValue("firebase-uid", uid);
    (void)mod->saveData();
}
void forgetSavedRefreshToken() {
    Mod::get()->setSavedValue("firebase-refresh-token", std::string());
}
// Firebase ID 토큰은 발급 후 60분 유효하다. 50분까지만 쓰고(만료), 40분부터는 계속 쓰면서 뒤에서 미리
// 갱신한다(refreshTokenInBackground). 그래서 대결 중에 만료가 와서 통신이 멈추는 일이 없다.
constexpr auto kTokenUsable = std::chrono::minutes(50);
constexpr auto kTokenRefreshAfter = std::chrono::minutes(40);
void markTokenFresh() {
    auto& s = state();
    s.expires = Clock::now() + kTokenUsable;
    s.refreshAt = Clock::now() + kTokenRefreshAfter;
}
// 서버가 준 커스텀 토큰으로 Firebase에 로그인해 이 GD 계정의 기존 Versus 신원으로 전환한다.
void signInAs(std::string customToken, std::string uid) {
    if (customToken.empty() || uid.empty()) {
        state().token.clear();
        finishAuthentication(false, "Account sync failed. Reopen Versus to retry.");
        return;
    }
    auto request = web::WebRequest();
    request.timeout(std::chrono::seconds(10));
    request.param("key", state().apiKey);
    auto body = Json::object();
    body["token"] = std::move(customToken);
    body["returnSecureToken"] = true;
    request.header("Content-Type", "application/json");
    request.bodyString(body.dump());
    async::spawn(request.post("https://identitytoolkit.googleapis.com/v1/accounts:signInWithCustomToken"),
                 [uid = std::move(uid)](web::WebResponse response) {
                     syncServerClock(response);
                     auto parsed = response.json();
                     auto& s = state();
                     auto const token = response.ok() && parsed ? stringAt(parsed.unwrap(), "idToken") : std::string();
                     auto const refresh =
                         response.ok() && parsed ? stringAt(parsed.unwrap(), "refreshToken") : std::string();
                     if (token.empty() || refresh.empty()) {
                         s.token.clear();
                         finishAuthentication(false, "Account sign-in failed. Reopen Versus to retry.");
                         return;
                     }
                     s.profile.uid = uid;
                     s.token = token;
                     s.refreshToken = refresh;
                     markTokenFresh();
                     saveIdentity(refresh, uid);
                     log::info("Versus: this device now plays as its GD account's Versus identity");
                     Notification::create(fmt::format("Versus progress loaded for {}", s.profile.name),
                                          NotificationIcon::Success, 2.5f)
                         ->show();
                     syncAccount();
                 });
}
// GD 계정과 Versus 신원을 서버와 맞춘다 (account/sync). 서버의 action에 따라 진행한다:
// verify(Argon 검증 후 재호출), unverified, switch(다른 신원으로 로그인), new(새 익명 신원),
// choose(여러 Ranked 기록 중 선택), ok. 7회를 넘게 반복하면 실패로 처리한다.
void syncAccount(std::string argonToken, std::string argonError, bool argonFresh) {
    auto& s = state();
    if (!s.room) refreshLocalProfile();
    int const account = s.profile.accountId;
    if (account <= 0) {
        s.token.clear();
        s.syncRounds = 0;
        finishAuthentication(false, kGdLoginRequired);
        return;
    }
    if (s.syncedAccount == account && s.syncedUid == s.profile.uid) {
        finishAuthentication(true, {});
        return;
    }
    if (++s.syncRounds > 7) {
        s.syncRounds = 0;
        s.token.clear();
        finishAuthentication(false, "Account sync failed. Reopen Versus to retry.");
        return;
    }
    auto body = Json::object();
    body["accountId"] = account;
    body["name"] = s.profile.name;
    body["argon"] = true;
    if (!argonToken.empty())
        body["argonToken"] = std::move(argonToken);
    else if (!argonError.empty())
        body["argonError"] = std::move(argonError);
    if (argonFresh) body["argonFresh"] = true;
    request(
        "PUT", "account/sync", std::move(body),
        [account](web::WebResponse response) {
            auto& s = state();
            if (response.code() == 426) {
                log::warn("Versus account sync skipped: this build is outdated");
                s.syncedAccount = account;
                s.syncedUid = s.profile.uid;
                s.syncRounds = 0;
                finishAuthentication(true, {});
                return;
            }
            auto parsed = response.json();
            if (!response.ok() || !parsed || !parsed.unwrap().isObject()) {
                s.token.clear();
                s.syncRounds = 0;
                finishAuthentication(false, networkError(response));
                return;
            }
            auto const value = parsed.unwrap();
            auto const action = stringAt(value, "action");
            if (action == "verify") {
                bool const fresh = boolAt(value, "fresh");
                s.argonState = "checking";
                withArgonToken(account, fresh, [account, fresh](std::string token, std::string error) {
                    if (state().profile.accountId != account) {
                        syncAccount();
                        return;
                    }
                    if (token.empty()) {
                        log::warn("Versus: no Argon token ({})", error);
                        syncAccount({}, error.empty() ? std::string("no token") : error, fresh);
                        return;
                    }
                    syncAccount(std::move(token), {}, fresh);
                });
                return;
            }
            if (action == "unverified") {
                s.argonState = "failed";
                log::warn("Versus: Argon did not confirm GD account {}: {}", account, stringAt(value, "error"));
                Notification::create("Argon could not verify your GD account", NotificationIcon::Error, 4.f)->show();
                if (boolAt(value, "drop")) {
                    s.refreshToken.clear();
                    forgetSavedRefreshToken();
                    authenticate(s.apiKey, false);
                    return;
                }
                s.syncedAccount = account;
                s.syncedUid = s.profile.uid;
                s.syncRounds = 0;
                finishAuthentication(true, {});
                return;
            }
            if (value["argon"].isString()) s.argonState = stringAt(value, "argon");
            if (action == "switch") {
                signInAs(stringAt(value, "token"), stringAt(value, "uid"));
                return;
            }
            if (action == "new") {
                s.refreshToken.clear();
                authenticate(s.apiKey, false);
                return;
            }
            s.accountChoices.clear();
            if (action == "choose") {
                if (value["choices"].isArray())
                    for (auto const& entry : value["choices"].asArray().unwrap()) {
                        if (!entry.isObject()) continue;
                        AccountChoice choice;
                        choice.rank = parseRankedProfile(entry);
                        choice.uid = stringAt(entry, "uid");
                        choice.lastPlayedAt = intAt(entry, "lastRankedPlayedAt");
                        choice.thisDevice = boolAt(entry, "thisDevice");
                        if (!choice.uid.empty()) s.accountChoices.push_back(std::move(choice));
                    }
                log::info("Versus: this GD account has {} Ranked records to choose from", s.accountChoices.size());
            } else if (action == "ok") {
                if (value["cosmetics"].isObject())
                    adoptAccountCosmetics(value["cosmetics"]);
                else
                    uploadAccountCosmetics();
            } else {
                auto const error = stringAt(value, "error");
                log::warn("Versus account sync unavailable: {}", error);
                if (s.argonState == "checking") s.argonState.clear();
                if (stringAt(value, "argon") == "unlinked") {
                    s.argonState = "unlinked";
                    s.argonNotice = true;
                }
            }
            s.syncedAccount = account;
            s.syncedUid = s.profile.uid;
            s.syncRounds = 0;
            finishAuthentication(true, {});
            if (action == "ok" && s.argonState.empty()) verifyArgonInBackground(account, s.profile.name);
        },
        {}, false, false, false, {}, {}, {}, 10);
}
// Firebase 익명 가입 또는 refresh token으로 ID 토큰을 받은 뒤(50분 유효로 취급) 계정 동기화를 시작한다.
void authenticate(std::string key, bool refresh) {
    auto request = web::WebRequest();
    request.timeout(std::chrono::seconds(10));
    request.param("key", std::move(key));
    auto body = Json::object();
    std::string url;
    if (refresh) {
        url = "https://securetoken.googleapis.com/v1/token";
        request.header("Content-Type", "application/x-www-form-urlencoded");
        request.bodyString("grant_type=refresh_token&refresh_token=" + formEncode(state().refreshToken));
    } else {
        url = "https://identitytoolkit.googleapis.com/v1/accounts:signUp";
        body["returnSecureToken"] = true;
        request.header("Content-Type", "application/json");
        request.bodyString(body.dump());
    }
    async::spawn(request.post(url), [refresh](web::WebResponse response) {
        syncServerClock(response);
        auto parsed = response.json();
        if (!response.ok() || !parsed) {
            finishAuthentication(false,
                                 "Firebase sign-in failed. Check the Web API key and enable Anonymous authentication.");
            return;
        }
        auto const body = parsed.unwrap();
        auto& s = state();
        auto const uid = stringAt(body, refresh ? "user_id" : "localId");
        auto const token = stringAt(body, refresh ? "id_token" : "idToken");
        auto const refreshToken = stringAt(body, refresh ? "refresh_token" : "refreshToken");
        if (uid.empty() || token.empty() || refreshToken.empty()) {
            finishAuthentication(false, "Invalid Firebase sign-in response.");
            return;
        }
        s.profile.uid = uid;
        s.token = token;
        s.refreshToken = refreshToken;
        markTokenFresh();
        saveIdentity(refreshToken, uid);
        syncAccount();
    });
}
// 토큰이 아직 쓸 수 있을 때(갱신 시각 ~ 만료 전) 뒤에서 새 ID 토큰을 받는다. connect와 달리
// authenticating을 켜지 않으므로, 받는 동안에도 기존 토큰(과 서버 세션)으로 보고·폴링·heartbeat가 계속된다.
// 실패하면 1분 뒤 다시 시도하고, 만료까지 끝내 못 받으면 예전처럼 connect가 갱신한다.
void refreshTokenInBackground() {
    auto& s = state();
    if (s.tokenRefreshing || s.authenticating || s.refreshToken.empty() || s.token.empty() ||
        Clock::now() < s.refreshAt || Clock::now() >= s.expires)
        return;
    auto key = s.apiKey.empty() ? configuredApiKey() : s.apiKey;
    if (key.empty()) return;
    s.tokenRefreshing = true;
    s.refreshAt = Clock::now() + std::chrono::minutes(1);
    auto request = web::WebRequest();
    request.timeout(std::chrono::seconds(10));
    request.param("key", std::move(key));
    request.header("Content-Type", "application/x-www-form-urlencoded");
    request.bodyString("grant_type=refresh_token&refresh_token=" + formEncode(s.refreshToken));
    async::spawn(request.post("https://securetoken.googleapis.com/v1/token"),
                 [before = s.token, uid = s.profile.uid](web::WebResponse response) {
                     auto& s = state();
                     s.tokenRefreshing = false;
                     auto parsed = response.json();
                     std::string token, refresh, owner;
                     if (response.ok() && parsed && parsed.unwrap().isObject()) {
                         auto const body = parsed.unwrap();
                         token = stringAt(body, "id_token");
                         refresh = stringAt(body, "refresh_token");
                         owner = stringAt(body, "user_id");
                     }
                     if (token.empty() || refresh.empty() || owner != uid) {
                         log::warn("Versus: sign-in refresh failed (HTTP {}); still signed in, retrying in a minute",
                                   response.code());
                         return;
                     }
                     // 그 사이 다른 로그인(계정 전환, 만료 후 재로그인)이 토큰을 바꿨으면 이 결과는 버린다.
                     if (s.token != before || s.profile.uid != uid) return;
                     s.token = token;
                     // 지금 서버 세션은 그대로 쓴다(서버가 정한 만료까지). 다음 세션은 새 토큰으로 받는다.
                     if (s.sessionBase == before) s.sessionBase = token;
                     markTokenFresh();
                     if (refresh != s.refreshToken) {
                         s.refreshToken = refresh;
                         saveIdentity(refresh, uid);
                     }
                 });
}
// Ranked 대기 중인 플레이어 목록(searchers)을 복원한다. 서버가 전체 목록 또는 변경분(rosterDelta:
// drop/put)을 보내며, rosterTag가 이어지지 않으면 목록을 비워 다음 응답에서 전체를 받는다.
Json materializeRoster(Json body) {
    auto& s = state();
    auto const tag = stringAt(body, "rosterTag");
    auto uidOf = [](Json const& entry) { return stringAt(entry["profile"], "uid"); };
    if (body["searchers"].isArray()) {
        s.rosterEntries = body["searchers"].asArray().unwrap();
        s.rosterTag = tag;
        return body;
    }
    if (body["rosterDelta"].isObject()) {
        auto const& delta = body["rosterDelta"];
        if (stringAt(delta, "from") != s.rosterTag) {
            s.rosterEntries.clear();
            s.rosterTag.clear();
        } else {
            std::unordered_set<std::string> drop;
            if (delta["drop"].isArray())
                for (auto const& uid : delta["drop"].asArray().unwrap()) drop.insert(uid.asString().unwrapOr(""));
            std::erase_if(s.rosterEntries, [&](Json const& entry) { return drop.contains(uidOf(entry)); });
            if (delta["put"].isArray())
                for (auto const& entry : delta["put"].asArray().unwrap()) {
                    auto const uid = uidOf(entry);
                    auto found = std::find_if(s.rosterEntries.begin(), s.rosterEntries.end(),
                                              [&](Json const& known) { return uidOf(known) == uid; });
                    if (found != s.rosterEntries.end())
                        *found = entry;
                    else
                        s.rosterEntries.push_back(entry);
                }
            s.rosterTag = tag;
        }
        body.erase("rosterDelta");
    } else if (!tag.empty() && tag != s.rosterTag) {
        s.rosterEntries.clear();
        s.rosterTag.clear();
    }
    auto list = Json::array();
    for (auto const& entry : s.rosterEntries) list.push(entry);
    body["searchers"] = std::move(list);
    return body;
}
// Ranked 경로: 컵(Season 2)이 선택되어 있으면 ranked2/<컵>/..., 아니면 ranked/...
std::string rankedPath(std::string const& sub) {
    auto const& cup = state().rankedCup;
    return cup.empty() ? "ranked/" + sub : "ranked2/" + cup + "/" + sub;
}
// Ranked 요청 공통 처리. 상태 폴링은 ETag로 304를 받으면 저장해 둔 응답을 재사용하고,
// 더 늦게 보낸 요청의 결과가 이미 반영됐으면(serial) 오래된 응답은 최신 상태로 대체한다.
// 요청 도중 컵이 바뀌면 결과를 버린다.
void rankedRequest(std::string method, std::string path, Json body, RankedStateCallback callback) {
    auto const serial = ++state().rankedRequestSerial;
    auto cup = state().rankedCup;
    Service::get().connect([method = std::move(method), path = std::move(path), body = std::move(body),
                            callback = std::move(callback), serial, cup](bool ok, std::string detail) mutable {
        if (!ok) {
            callback(std::nullopt, std::move(detail));
            return;
        }
        bool const statePoll = method == "GET" && path == "ranked/state";
        auto& s = state();
        std::string ifNone = statePoll && s.rankedStateRaw ? s.rankedStateEtag : std::string();
        request(
            std::move(method), std::move(path), std::move(body),
            [callback = std::move(callback), serial, statePoll, cup](web::WebResponse response) mutable {
                auto& s = state();
                if (s.rankedCup != cup) {
                    callback(std::nullopt, "The Ranked cup changed.");
                    return;
                }
                if (serial < s.rankedAppliedSerial && s.lastRankedState) {
                    callback(*s.lastRankedState, {});
                    return;
                }
                if (statePoll && response.code() == 304 && s.rankedStateRaw) {
                    s.rankedAppliedSerial = serial;
                    s.rankedStateRaw = materializeRoster(*s.rankedStateRaw);
                    s.lastRankedState = adoptRankedState(*s.rankedStateRaw);
                    callback(*s.lastRankedState, {});
                    return;
                }
                auto parsed = response.json();
                if (!response.ok() || !parsed || !parsed.unwrap().isObject()) {
                    callback(std::nullopt, networkError(response));
                    return;
                }
                auto const full = materializeRoster(parsed.unwrap());
                if (statePoll) {
                    auto etag = response.header("ETag");
                    s.rankedStateEtag = etag ? std::string(*etag) : std::string();
                    if (etag)
                        s.rankedStateRaw = full;
                    else
                        s.rankedStateRaw.reset();
                }
                s.rankedAppliedSerial = serial;
                s.lastRankedState = adoptRankedState(full);
                callback(*s.lastRankedState, {});
            },
            {}, false, false, false, {}, {}, std::move(ifNone));
    });
}
}

// 점검 문구 판별과 남은 시간 문구(10초 단위 올림 / 분 단위).
bool isMaintenanceMessage(std::string const& message) {
    return message.starts_with(kMaintenancePrefix);
}
std::string maintenanceTimeLeft(MaintenanceInfo const& info, int64_t serverNow) {
    if (info.expectedEndAt <= 0) return "Back shortly.";
    auto const left = info.expectedEndAt - serverNow;
    if (left <= 0) return "Almost done.";
    auto const seconds = (left + 999) / 1000;
    if (seconds <= 50) return fmt::format("Back in about {} s.", std::max<int64_t>(10, (seconds + 9) / 10 * 10));
    return fmt::format("Back in about {} min.", (seconds + 59) / 60);
}

// 시즌 종료까지 남은 시간 문구 (일 hh:mm:ss).
std::string seasonTimeLeft(int season, int64_t endsAt, int64_t serverNow) {
    auto const left = endsAt - serverNow;
    if (left <= 0) return fmt::format("SEASON {} HAS ENDED", season);
    auto const seconds = left / 1000;
    auto const days = seconds / 86400, hours = seconds / 3600 % 24, minutes = seconds / 60 % 60;
    return fmt::format("SEASON {} ENDS IN {} {} {:02}:{:02}:{:02}", season, days, days == 1 ? "Day" : "Days", hours,
                       minutes, seconds % 60);
}

// Group 방 로직, 디버그용 봇 전투, 디버그 방은 별도 파일로 나누어 이 번역 단위에 포함한다.
#include "GroupService.inl"
#include "DebugBot.inl"
#include "DebugRoom.inl"

Service& Service::get() {
    static Service instance;
    return instance;
}
// 저장된 신원과 API 키를 읽고, 매 프레임 tick을 호출하는 ticker를 스케줄러에 등록한다 (한 번만).
void Service::initialize() {
    auto& s = state();
    if (s.initialized) return;
    s.initialized = true;
    s.profile.uid = Mod::get()->getSavedValue<std::string>("firebase-uid", "");
    s.refreshToken = Mod::get()->getSavedValue<std::string>("firebase-refresh-token", "");
    s.apiKey = configuredApiKey();
    refreshLocalProfile();
    auto ticker = new ServiceTicker();
    if (ticker->init()) {
        CCDirector::sharedDirector()->getScheduler()->scheduleUpdateForTarget(ticker, 0, false);
        ticker->release();
    } else
        delete ticker;
}
// 서버 연결 보장. 이미 연결돼 있으면 바로 성공, 아니면 콜백을 쌓아 두고 인증을 한 번만 시작한다.
// GD 로그인이 없거나 API 키가 비어 있으면 실패.
void Service::connect(Done callback) {
    initialize();
    auto& s = state();
    if (!s.room) refreshLocalProfile();
    auto key = configuredApiKey();
    if (key != s.apiKey && !s.authenticating && !s.room) {
        s.apiKey = key;
        s.token.clear();
    }
    if (s.profile.accountId <= 0) {
        callback(false, kGdLoginRequired);
        return;
    }
    if (connected() && !s.room && s.syncedAccount != s.profile.accountId) s.token.clear();
    if (connected()) {
        callback(true, {});
        return;
    }
    if (key.empty()) {
        callback(false, "Firebase setup required: enter this project's Web API key in the Versus settings.");
        return;
    }
    s.connectCallbacks.push_back(std::move(callback));
    if (s.authenticating) return;
    s.authenticating = true;
    authenticate(std::move(key), !s.refreshToken.empty());
}
// 토큰이 있고 만료 전이면 연결된 것으로 본다.
bool Service::connected() const {
    return !state().token.empty() && Clock::now() < state().expires;
}
// 멤버십 상태 조회 / 코드 등록. 동시에 하나만 진행하며 성공하면 로컬 프로필을 갱신한다.
void Service::refreshMembership(Done callback) {
    connect([callback = std::move(callback)](bool ok, std::string error) mutable {
        if (!ok) {
            callback(false, std::move(error));
            return;
        }
        if (state().membershipChecking) {
            callback(false, "Membership check already in progress.");
            return;
        }
        state().membershipChecking = true;
        request("GET", "membership", {}, [callback = std::move(callback)](web::WebResponse response) mutable {
            auto& s = state();
            s.membershipChecking = false;
            s.membershipCheckedAt = Clock::now();
            auto value = response.json();
            bool ok = response.ok() && value && value.unwrap()["active"].isBool();
            s.membershipActive = ok && boolAt(value.unwrap(), "active");
            if (!s.room) refreshLocalProfile();
            callback(ok, ok ? std::string{} : networkError(response));
        });
    });
}
void Service::redeemMembership(std::string code, Done callback) {
    if (!safeToUpdate()) {
        callback(false, "Finish your match before entering a code.");
        return;
    }
    if (code.empty() || code.size() > 128) {
        callback(false, "Enter a membership code (up to 128 characters).");
        return;
    }
    connect([code = std::move(code), callback = std::move(callback)](bool ok, std::string error) mutable {
        if (!ok) {
            callback(false, std::move(error));
            return;
        }
        if (state().membershipChecking) {
            callback(false, "Membership check already in progress.");
            return;
        }
        state().membershipChecking = true;
        auto body = Json::object();
        body["code"] = std::move(code);
        request("PUT", "membership", std::move(body),
                [callback = std::move(callback)](web::WebResponse response) mutable {
                    auto& s = state();
                    s.membershipChecking = false;
                    s.membershipCheckedAt = Clock::now();
                    auto value = response.json();
                    bool ok = response.ok() && value && boolAt(value.unwrap(), "active");
                    if (ok) {
                        s.membershipActive = true;
                        if (!s.room) refreshLocalProfile();
                    }
                    callback(ok, ok ? std::string{} : networkError(response));
                });
    });
}
// 상태 조회 도우미들. recordsPending은 서버에 아직 못 보낸 기록/액션이 있는지,
// safeToUpdate는 그것이 없고 전투 중도 아닌지를 뜻한다 (업데이트 가능 여부).
bool Service::updatePending() const {
    return state().updatePending;
}
RankedState const* Service::rankedState() const {
    auto const& s = state();
    return s.lastRankedState ? &*s.lastRankedState : nullptr;
}
bool Service::battleInProgress() const {
    auto const& s = state();
    return s.room && s.room->started && !(s.room->battle && s.room->battle->finishedAt > 0);
}
bool Service::recordsPending() const {
    auto const& s = state();
    return s.writing || s.historyWriting || s.attemptWriting || !s.attempts.empty() || !s.pendingActions.empty() ||
           !s.returningMatch.empty();
}
bool Service::safeToUpdate() const {
    auto const& s = state();
    if (recordsPending()) return false;
    return !(s.room && s.room->started);
}
PlayerProfile const& Service::profile() const {
    return state().profile;
}
bool Service::saveCosmetics(Cosmetics const& cosmetics) {
    if (!canUseCosmetics(cosmetics, Service::get().profile().membership) || state().room) return false;
    Mod::get()->setSavedValue(cosmeticsSaveKey(Service::get().profile().membership), cosmeticsJson(cosmetics));
    state().profile.cosmetics = cosmetics;
    state().profile.hasCosmetics = true;
    if (connected() && state().syncedAccount > 0) uploadAccountCosmetics();
    return true;
}
std::optional<RoomInfo> const& Service::room() const {
    return state().room;
}
bool Service::isHost() const {
    return state().room && state().room->host.uid == state().profile.uid;
}
// 요청이 진행 중이라 새 방 조작을 시작하면 안 되는 상태인지 (heartbeat 같은 유지용 쓰기는 제외).
bool Service::busy() const {
    return state().searchingMap || (state().writing && !state().maintenanceWriting) ||
           (!state().pendingActions.empty() && !state().dispatchingAction) || state().authenticating;
}
int64_t Service::serverNow() const {
    return nowMs();
}
// 내 다운로드 정보가 서버에 이 맵의 최신 값으로 반영됐는지 (방이 없거나 로컬/봇 전투면 true).
bool Service::downloadReported(int64_t levelID) const {
    auto const& s = state();
    if (!s.room || s.localRoom || s.botBattle || s.room->level.id != levelID) return true;
    bool const host = s.room->host.uid == s.profile.uid;
    auto const& own = host ? s.room->hostDownload : s.room->guestDownload;
    return own.uid == s.profile.uid && own.levelId == levelID && own.best == mapLocalBest(levelID);
}
void Service::reportDownloadNow() {
    state().nextDownloadReport = {};
}
bool Service::battleReportAvailable() const {
    return !state().writing && !state().polling && !busy();
}
std::chrono::steady_clock::time_point Service::lastSeatAck() const {
    return state().lastSeatAck;
}
void Service::wantBattleReport(bool wanted) {
    state().battleReportWantedAt = wanted ? Clock::now() : Clock::time_point{};
}
// 공개 방 목록. 최근 호스트 신호가 있는 방만 남기고 최근 갱신 순으로 정렬한다.
void Service::fetchRooms(RoomListCallback callback) {
    connect([callback = std::move(callback)](bool ok, std::string error) mutable {
        if (!ok) {
            callback({}, std::move(error));
            return;
        }
        request(
            "GET", "rooms", {},
            [callback = std::move(callback)](web::WebResponse response) mutable {
                auto parsed = response.json();
                if (!response.ok() || !parsed) {
                    callback({}, networkError(response));
                    return;
                }
                std::vector<RoomInfo> rooms;
                if (parsed.unwrap().isObject())
                    for (auto const& entry : parsed.unwrap()) {
                        if (!entry.isObject() ||
                            nowMs() - intAt(entry, "hostSeen") > firebase_config::PRESENCE_TIMEOUT_MS)
                            continue;
                        auto room = parseRoom(entry.getKey().value_or(""), entry);
                        if (room.id.empty() || room.host.uid.empty()) continue;
                        rooms.push_back(std::move(room));
                    }
                std::sort(rooms.begin(), rooms.end(),
                          [](auto const& a, auto const& b) { return a.updatedAt > b.updatedAt; });
                callback(std::move(rooms), {});
            },
            {}, false, true);
    });
}
void Service::fetchHistory(HistoryCallback callback) {
    fetchPlayerHistory({}, std::move(callback));
}
// 전투 기록을 받는다 (uid가 비면 내 기록). 최신순으로 정렬해 일반 100건, 관리자 500건까지만 남긴다.
// 내 기록이면 최근 10경기로 승률(recentGames, winRate)을 갱신한다.
// Ranked 기록(roomId가 ranked-로 시작)에는 레이팅 변화 정보가 포함된다.
void Service::fetchPlayerHistory(std::string uid, HistoryCallback callback) {
    connect([uid = std::move(uid), callback = std::move(callback)](bool ok, std::string error) mutable {
        if (!ok) {
            callback({}, std::move(error));
            return;
        }
        if (uid.empty()) uid = state().profile.uid;
        bool const own = uid == state().profile.uid;
        request(
            "GET", "history/" + uid, {},
            [own, callback = std::move(callback)](web::WebResponse response) mutable {
                auto parsed = response.json();
                if (!response.ok() || !parsed) {
                    callback({}, networkError(response));
                    return;
                }
                std::vector<MatchRecord> records;
                if (parsed.unwrap().isObject())
                    for (auto const& entry : parsed.unwrap()) {
                        MatchRecord record;
                        record.id = entry.getKey().value_or("");
                        record.opponentName = stringAt(entry, "opponentName");
                        record.levelName = stringAt(entry, "levelName");
                        record.winnerName = stringAt(entry, "winnerName");
                        record.result = stringAt(entry, "result");
                        record.playedAt = intAt(entry, "playedAt");
                        record.levelId = intAt(entry, "levelId");
                        record.roomId = stringAt(entry, "roomId");
                        record.ranked = record.roomId.starts_with("ranked-");
                        if (record.ranked && entry["ranked"].isObject()) {
                            auto const& info = entry["ranked"];
                            record.duel = boolAt(info, "duel");
                            record.ratingKnown = info.contains("ratingChange") && info["ratingChange"].isNumber();
                            record.ratingChange =
                                static_cast<int>(std::clamp<int64_t>(intAt(info, "ratingChange"), -10000, 10000));
                            record.newRating =
                                static_cast<int>(std::clamp<int64_t>(intAt(info, "newRating"), 0, 100000));
                            record.canceled = boolAt(info, "canceled");
                        }
                        record.detailed = entry["self"].isObject() && entry["opponent"].isObject();
                        if (record.detailed) {
                            record.self = parseProfile(entry["self"]);
                            record.opponent = parseProfile(entry["opponent"]);
                            record.selfStats = parseBattlePlayer(entry["selfStats"]);
                            record.opponentStats = parseBattlePlayer(entry["opponentStats"]);
                            record.rules = parseRules(entry["rules"]);
                            auto before = [&entry](char const* key) {
                                return entry.contains(key) && entry[key].isNumber()
                                           ? static_cast<int>(std::clamp<int64_t>(intAt(entry, key, -2), -2, 100))
                                           : -2;
                            };
                            record.selfBefore = before("selfBefore");
                            record.opponentBefore = before("opponentBefore");
                        }
                        records.push_back(std::move(record));
                    }
                std::sort(records.begin(), records.end(),
                          [](auto const& a, auto const& b) { return a.playedAt > b.playedAt; });
                auto const limit = Service::get().isAdmin() ? kAdminHistoryLimit : kHistoryLimit;
                if (records.size() > limit) records.resize(limit);
                if (own) {
                    auto const recent = std::min<size_t>(records.size(), 10);
                    auto& profile = state().profile;
                    profile.recentGames = static_cast<int>(recent);
                    auto wins = std::count_if(records.begin(), records.begin() + recent,
                                              [](auto const& r) { return r.result == "win"; });
                    profile.winRate = recent == 0 ? 0. : 100. * wins / recent;
                }
                callback(std::move(records), {});
            },
            {}, false, false, true);
    });
}
// 경기의 시도 기록(matchAttempts)을 내 것과 상대 것 두 번 요청해 모아서 한 번에 돌려준다.
// firstRun부터 10개 run만 요청한다 (한쪽이라도 실패하면 오류 문자열을 함께 돌려줌).
void Service::fetchAttempts(MatchRecord match, int firstRun, AttemptsCallback callback) {
    struct Result {
        std::vector<AttemptRecord> own, other;
        std::string error;
        int pending = 2;
        AttemptsCallback done;
    };
    auto result = std::make_shared<Result>();
    result->done = std::move(callback);
    for (bool own : {true, false}) {
        auto uid = own ? match.self.uid : match.opponent.uid;
        request(
            "GET", "matchAttempts/" + match.id + "/" + uid, {},
            [result, own](web::WebResponse response) {
                auto parsed = response.json();
                if (!response.ok() || !parsed)
                    result->error = networkError(response);
                else if (parsed.unwrap().isObject())
                    for (auto const& value : parsed.unwrap()) {
                        AttemptRecord record;
                        record.run = static_cast<int>(intAt(value, "run"));
                        record.percent = static_cast<int>(std::clamp<int64_t>(intAt(value, "percent"), 0, 100));
                        record.ms = value.contains("ms") ? intAt(value, "ms", -1) : -1;
                        record.fast = boolAt(value, "fast");
                        record.length = static_cast<int>(std::clamp<int64_t>(intAt(value, "length", -1), -1, 5));
                        record.outcome = stringAt(value, "outcome");
                        (own ? result->own : result->other).push_back(std::move(record));
                    }
                if (--result->pending == 0)
                    result->done(std::move(result->own), std::move(result->other), std::move(result->error));
            },
            {}, false, false, false, fmt::format("{}", std::clamp(firstRun, 1, 1000000)));
    }
}
// 현재 1v1 전투의 시도 하나(run, 최고 %)를 서버 전송 대기열에 넣는다.
// Group / 봇 / 로컬 방에서는 기록하지 않으며 같은 run은 한 번만 넣는다.
void Service::recordAttempt(int run, int percent) {
    auto& s = state();
    if (s.group) return;
    if (s.botBattle) return;
    if (s.localRoom) return;
    if (!s.room || !s.room->battle || run <= 0 || run > 1000000) return;
    auto const& id = s.room->battle->id;
    for (auto const& entry : s.attempts)
        if (entry.match == id && entry.run == run) return;
    s.attempts.push_back({id, s.room->id, s.profile.uid, run, std::clamp(percent, 0, 100)});
}
// 결과 화면을 확인했음을 알린다. 종료된 일반 방 전투면 방으로 복귀하는 절차(returningMatch)를
// 시작하고, Ranked 매치는 안내만 지운다.
void Service::acknowledgeResult(Done callback) {
    auto& s = state();
    if (s.group) {
        callback(true, {});
        return;
    }
    if (!s.room || !s.room->battle) {
        callback(true, {});
        return;
    }
    if (s.room->battle->finishedAt <= 0) {
        callback(false, "Match is still active.");
        return;
    }
    if (isRankedMatch()) {
        s.notice.clear();
        callback(true, {});
        return;
    }
    if (s.returningMatch != s.room->battle->id) {
        s.returningMatch = s.room->battle->id;
        s.returnRequestedAt = Clock::now();
        s.nextReturnAttempt = Clock::now();
    }
    callback(true, {});
}
// 1v1 방을 만든다. 비밀번호가 있으면 roomSecrets에 먼저 기록한 뒤 방 본문을 PUT한다.
// 방 생성 응답이 실패해도 서버에 실제로 만들어졌을 수 있어 recoverMembership으로 확인하고,
// 최종 실패 시 roomSecrets를 지운다. (name 인자는 쓰지 않고 호스트 이름으로 방 이름을 정함)
void Service::createRoom(std::string, std::string pin, Done callback, bool allowHacks, bool debug) {
    if (busy() || state().room) {
        callback(false, "Leave your current room first.");
        return;
    }
    if (!pin.empty() && !validPin(pin)) {
        callback(false, "Use a numeric password (1-32 digits).");
        return;
    }
    if (debug && !debug::active()) {
        callback(false, "Debug rooms are for debug mode only.");
        return;
    }
    connect(
        [pin = std::move(pin), callback = std::move(callback), allowHacks, debug](bool ok, std::string error) mutable {
            if (!ok) {
                callback(false, std::move(error));
                return;
            }
            auto& s = state();
            if (s.writing || s.room) {
                callback(false, "Already joining a room.");
                return;
            }
            auto name = debug ? "Debug (" + s.profile.name + ")" : s.profile.name + "'s Room";
            s.writing = true;
            auto const id = randomId();
            auto const epoch = ++s.epoch;
            auto secret = Json::object();
            secret["hostUid"] = s.profile.uid;
            secret["pin"] = pin;
            request(
                "PUT", "roomSecrets/" + id, secret,
                [id, epoch, name = std::move(name), privateRoom = !pin.empty(), allowHacks, debug,
                 callback = std::move(callback)](web::WebResponse response) mutable {
                    if (!response.ok()) {
                        state().writing = false;
                        callback(false, networkError(response));
                        return;
                    }
                    auto body = Json::object();
                    body["name"] = name;
                    body["privateRoom"] = privateRoom;
                    if (debug) {
                        body["debug"] = true;
                        auto pool = poolSettingsJson(MapPoolSettings{});
                        pool["permit"] = Json::array();
                        pool["maps"] = Json::array();
                        body["mapPool"] = std::move(pool);
                    }
                    body["host"] = profileJson(state().profile);
                    body["hostSeen"] = timestamp();
                    body["updatedAt"] = timestamp();
                    body["started"] = false;
                    GameRules rules;
                    rules.allowHacks = allowHacks;
                    body["rules"] = rulesJson(rules);
                    body["hostReady"] = false;
                    body["guestReady"] = false;
                    body["hostEmoteAt"] = 0;
                    body["guestEmoteAt"] = 0;
                    resetRoomScore(body);
                    request(
                        "PUT", "rooms/" + id, body,
                        [id, epoch, callback = std::move(callback)](web::WebResponse result) mutable {
                            auto& s = state();
                            if (epoch != s.epoch) {
                                callback(false, "Room session changed.");
                                return;
                            }
                            auto finish = [id, epoch, callback = std::move(callback)](bool success,
                                                                                      std::string error) mutable {
                                if (epoch != state().epoch) {
                                    callback(false, "Room session changed.");
                                    return;
                                }
                                state().writing = false;
                                state().pollTime = 0;
                                state().heartbeatTime = 0;
                                if (!success) request("DELETE", "roomSecrets/" + id, {}, [](auto) {});
                                callback(success, std::move(error));
                            };
                            auto parsed = result.json();
                            if (!result.ok() || !parsed || !parsed.unwrap().isObject()) {
                                recoverMembership(id, epoch, true, networkError(result), std::move(finish));
                                return;
                            }
                            adoptRoom(id, parsed.unwrap());
                            finish(true, {});
                        },
                        "null_etag");
                },
                "null_etag");
        });
}
// 1v1 방에 게스트로 참가한다. 비공개 방은 먼저 admissions에 비밀번호를 제출해 통과해야 한다.
// 이후 mutateRoom으로 guest 자리를 채운다 (밴 목록 / 이미 찬 방 / 호스트 끊김 검사).
void Service::joinRoom(RoomInfo room, std::string pin, Done callback) {
    if (busy() || state().room) {
        callback(false, "Leave your current room first.");
        return;
    }
    if (room.privateRoom && !validPin(pin)) {
        callback(false, "Enter the numeric password (1-32 digits).");
        return;
    }
    connect([room = std::move(room), pin = std::move(pin), callback = std::move(callback)](bool ok,
                                                                                           std::string error) mutable {
        if (!ok) {
            callback(false, std::move(error));
            return;
        }
        auto& s = state();
        if (s.writing || s.room) {
            callback(false, "Already joining a room.");
            return;
        }
        s.writing = true;
        auto const epoch = ++s.epoch;
        auto join = [id = room.id, epoch, callback = std::move(callback)](bool admitted, std::string error) mutable {
            if (!admitted) {
                state().writing = false;
                callback(false, std::move(error));
                return;
            }
            mutateRoom(
                id, epoch, "join",
                [](Json& body) -> std::string {
                    auto const uid = state().profile.uid;
                    if (auto error = roomJoinError(body, uid, nowMs(), firebase_config::PRESENCE_TIMEOUT_MS);
                        !error.empty())
                        return error;
                    if (std::as_const(body)["banned"].isArray())
                        for (auto const& item : std::as_const(body)["banned"].asArray().unwrap())
                            if (item.isString() && item.asString().unwrap() == uid)
                                return "You were banned from this room by the host.";
                    if (stringAt(std::as_const(body)["guest"], "uid") == uid) {
                        body["guestSeen"] = timestamp();
                        return {};
                    }
                    body["guest"] = profileJson(state().profile);
                    body["guestSeen"] = timestamp();
                    clearGuestDownloads(body);
                    resetRoomScore(body);
                    body["guestReady"] = false;
                    return {};
                },
                [id, epoch, callback = std::move(callback)](bool success, std::string error) mutable {
                    auto finish = [epoch, callback = std::move(callback)](bool recovered, std::string error) mutable {
                        if (epoch != state().epoch) {
                            callback(false, "Room session changed.");
                            return;
                        }
                        state().writing = false;
                        state().pollTime = 0;
                        state().heartbeatTime = 0;
                        callback(recovered, std::move(error));
                    };
                    if (!success && epoch == state().epoch) {
                        recoverMembership(id, epoch, false, std::move(error), std::move(finish));
                        return;
                    }
                    finish(success, std::move(error));
                });
        };
        if (!room.privateRoom) {
            join(true, {});
            return;
        }
        auto proof = Json::object();
        proof["pin"] = pin;
        request("PUT", "admissions/" + room.id + "/" + s.profile.uid, proof,
                [join = std::move(join)](web::WebResponse response) mutable {
                    join(response.ok(), response.code() == 401 || response.code() == 403
                                            ? "Incorrect password or room closed."
                                            : networkError(response));
                });
    });
}
// 방 나가기. Group이면 leave를 보내고 즉시 로컬 상태를 정리한다.
// 1v1에서 호스트는 방 자체를 삭제하고(admissions, roomSecrets도 정리), 게스트는 자리만 비운다.
void Service::leaveRoom(Done callback) {
    auto& s = state();
    if (s.group) {
        if (s.writing) {
            callback(false, "Please wait for the room update.");
            return;
        }
        auto const epoch = s.epoch;
        s.writing = true;
        request("PUT", "groups/" + s.group->id + "/leave", Json::object(), [epoch](web::WebResponse) {
            if (epoch == state().epoch) state().writing = false;
        });
        disconnected({});
        callback(true, {});
        return;
    }
    if (s.maintenanceWriting) {
        deferRoomAction([](Done done) { Service::get().leaveRoom(std::move(done)); }, std::move(callback));
        return;
    }
    if (!s.room) {
        callback(true, {});
        return;
    }
    if (s.writing) {
        callback(false, "Please wait for the room update.");
        return;
    }
    auto const id = s.room->id;
    auto const host = isHost();
    auto const epoch = s.epoch;
    s.writing = true;
    mutateRoom(
        id, epoch, "leave",
        [host](Json& body) -> std::string {
            auto const uid = state().profile.uid;
            if (host && stringAt(std::as_const(body)["host"], "uid") == uid)
                body = nullptr;
            else if (!host && stringAt(std::as_const(body)["guest"], "uid") == uid) {
                dropPlayedPoolMap(body);
                removeRoomGuest(body);
            } else
                return "Your room connection expired.";
            return {};
        },
        [id, epoch, host, local = s.localRoom, callback = std::move(callback)](bool success,
                                                                               std::string error) mutable {
            if (epoch != state().epoch) {
                callback(false, "Room session changed.");
                return;
            }
            state().writing = false;
            bool const away = !success && isMaintenanceMessage(error);
            if (!success && !away && error != "The host left the room." && error != "Your room connection expired.") {
                callback(false, std::move(error));
                return;
            }
            disconnected({});
            if (host && !local) {
                request("DELETE", "admissions/" + id, {},
                        [id](auto) { request("DELETE", "roomSecrets/" + id, {}, [](auto) {}); });
            }
            callback(true, {});
        });
}
// 호스트가 대기실의 게스트를 내보낸다 (ban이면 방 밴 목록에 추가, 최대 kRoomBannedMax명 유지).
// Group이면 kickGroupMember로 넘긴다.
void Service::kickGuest(std::string uid, Done callback, bool ban) {
    if (inGroup()) {
        kickGroupMember(std::move(uid), ban, std::move(callback));
        return;
    }
    if (state().localRoom) {
        callback(false, "The Debug Room's stand-in stays.");
        return;
    }
    if (!isHost() || busy() || !state().room->guest || state().room->started) {
        callback(false, "Only the host can remove a challenger in the waiting room.");
        return;
    }
    auto const epoch = state().epoch;
    state().writing = true;
    mutateRoom(
        state().room->id, epoch, ban ? "ban" : "kick",
        [uid, ban](Json& body) -> std::string {
            if (stringAt(std::as_const(body)["host"], "uid") != state().profile.uid || boolAt(body, "started"))
                return "The room has changed. Try again in the waiting room.";
            if (stringAt(std::as_const(body)["guest"], "uid") != uid) return "The challenger has changed.";
            body["lastKickUid"] = uid;
            if (ban) {
                auto banned = Json::array();
                std::vector<std::string> kept;
                if (std::as_const(body)["banned"].isArray())
                    for (auto const& item : std::as_const(body)["banned"].asArray().unwrap())
                        if (item.isString() && item.asString().unwrap() != uid)
                            kept.push_back(item.asString().unwrap());
                kept.push_back(uid);
                size_t const from = kept.size() > kRoomBannedMax ? kept.size() - kRoomBannedMax : 0;
                for (size_t i = from; i < kept.size(); ++i) banned.push(kept[i]);
                body["banned"] = std::move(banned);
            }
            dropPlayedPoolMap(body);
            removeRoomGuest(body);
            return {};
        },
        [epoch, callback = std::move(callback)](bool ok, std::string detail) mutable {
            if (epoch == state().epoch) state().writing = false;
            callback(ok, std::move(detail));
        });
}
// 호스트가 맵을 고른다. 맵이 바뀌면 양쪽 준비를 해제하고 자동 목숨 규칙을 다시 적용한다.
// 플랫포머 맵은 거부한다.
void Service::selectLevel(LevelInfo level, Done callback) {
    if (inGroup()) {
        if (!isHost() || level.id <= 0) {
            callback(false, "Only the host can select a level.");
            return;
        }
        if (level.platformer || level.length == 5) {
            callback(false, "Versus supports Classic maps only.");
            return;
        }
        auto body = Json::object();
        body["level"] = levelJson(level);
        groupAction("settings", std::move(body), std::move(callback));
        return;
    }
    if (state().maintenanceWriting) {
        deferRoomAction([level = std::move(level)](
                            Done done) mutable { Service::get().selectLevel(std::move(level), std::move(done)); },
                        std::move(callback));
        return;
    }
    if (!isHost() || busy() || level.id <= 0) {
        callback(false, "Only the host can select a level.");
        return;
    }
    auto const epoch = state().epoch;
    state().writing = true;
    mutateRoom(
        state().room->id, epoch, "select-level",
        [level = std::move(level)](Json& body) -> std::string {
            if (stringAt(std::as_const(body)["host"], "uid") != state().profile.uid)
                return "Only the host can select a level.";
            if (boolAt(body, "started")) return "The match has already started.";
            if (boolAt(std::as_const(body)["mapSelection"], "random")) return "Switch to Map Select first.";
            if (level.platformer) return "Versus supports Classic maps only.";
            auto value = levelJson(level);
            if (!sameJson(std::as_const(body)["level"], value)) {
                body["hostReady"] = false;
                body["guestReady"] = false;
            }
            body["level"] = value;
            applyAutoLives(body);
            return {};
        },
        [epoch, callback = std::move(callback)](bool ok, std::string error) mutable {
            if (epoch == state().epoch) state().writing = false;
            callback(ok, std::move(error));
        });
}
// 랜덤 맵 조건(MapSelection) 변경. 룰렛이 돌아가는 중이면 거부하고, 바뀌면 맵/추첨/Map Pool을 비우고
// 양쪽 준비를 해제한다.
void Service::configureMapSelection(MapSelection selection, Done callback) {
    if (inGroup()) {
        if (!isHost() || !validSelection(selection)) {
            callback(false, "Only the host can change map selection.");
            return;
        }
        if (selection.platformer) {
            callback(false, "Versus supports Classic maps only.");
            return;
        }
        auto body = Json::object();
        body["mapSelection"] = mapSelectionJson(selection, false);
        groupAction("settings", std::move(body), std::move(callback));
        return;
    }
    if (state().maintenanceWriting) {
        deferRoomAction([selection](Done done) { Service::get().configureMapSelection(selection, std::move(done)); },
                        std::move(callback));
        return;
    }
    if (busy() || !isHost() || !validSelection(selection)) {
        callback(false, "Only the host can change map selection.");
        return;
    }
    auto const epoch = state().epoch;
    state().writing = true;
    mutateRoom(
        state().room->id, epoch, "configure-map",
        [selection](Json& body) -> std::string {
            if (stringAt(std::as_const(body)["host"], "uid") != state().profile.uid || boolAt(body, "started"))
                return "Room changed.";
            if (std::as_const(body)["mapDraw"].isObject() && !boolAt(std::as_const(body)["mapDraw"], "settled"))
                return "Wait for the roulette.";
            bool const pool = std::as_const(body)["mapPool"].isObject();
            if (!pool && parseMapSelection(std::as_const(body)["mapSelection"]) == selection) return ROOM_UNCHANGED;
            if (selection.platformer) return "Versus supports Classic maps only.";
            body["mapSelection"] = mapSelectionJson(selection);
            body.erase("mapDraw");
            body.erase("level");
            body.erase("mapPool");
            body["hostReady"] = false;
            body["guestReady"] = false;
            return {};
        },
        [epoch, callback = std::move(callback)](bool ok, std::string detail) mutable {
            if (epoch == state().epoch) state().writing = false;
            callback(ok, std::move(detail));
        });
}
namespace {
// Map Pool 변경 공통 처리: 방 쓰기 직렬화(유지 보수 중이면 미룸)와 완료 후 폴링 앞당김.
void poolWrite(char const* action, Mutator change, Done callback) {
    auto& s = state();
    if (s.maintenanceWriting) {
        deferRoomAction([action, change = std::move(change)](
                            Done done) mutable { poolWrite(action, std::move(change), std::move(done)); },
                        std::move(callback));
        return;
    }
    if (Service::get().busy() || !s.room) {
        callback(false, "The room is busy. Try again.");
        return;
    }
    auto const epoch = s.epoch;
    s.writing = true;
    mutateRoom(s.room->id, epoch, action, std::move(change),
               [epoch, callback = std::move(callback)](bool ok, std::string detail) mutable {
                   if (epoch == state().epoch) {
                       state().writing = false;
                       state().pollTime = 1.f;
                   }
                   callback(ok, std::move(detail));
               });
}
bool hasPool(Json const& body) {
    return std::as_const(body)["mapPool"].isObject() && std::as_const(body)["mapPool"]["maps"].isArray();
}
}
// Map Pool 설정 변경. 새 조건에 맞지 않는 기존 맵은 풀에서 빼고, 추첨 방식이 바뀌면
// 현재 추첨/맵을 초기화한다.
void Service::configureMapPool(MapPoolSettings settings, Done callback) {
    if (!validPoolSettings(settings)) {
        callback(false, "Invalid Map Pool settings.");
        return;
    }
    if (inGroup()) {
        if (!isHost()) {
            callback(false, "Only the host can change the Map Pool.");
            return;
        }
        auto body = Json::object();
        body["mapPool"] = poolSettingsJson(settings);
        groupAction("settings", std::move(body), std::move(callback));
        return;
    }
    if (!isHost() || !state().room) {
        callback(false, "Only the host can change the Map Pool.");
        return;
    }
    poolWrite(
        "configure-pool",
        [settings](Json& body) -> std::string {
            if (stringAt(std::as_const(body)["host"], "uid") != state().profile.uid || boolAt(body, "started"))
                return "Room changed.";
            if (std::as_const(body)["mapDraw"].isObject() && !boolAt(std::as_const(body)["mapDraw"], "settled"))
                return "Wait for the roulette.";
            auto const selection = parseMapSelection(std::as_const(body)["mapSelection"]);
            auto const old = parseMapPool(std::as_const(body)["mapPool"]);
            if (old && old->settings == settings && !selection.random) return ROOM_UNCHANGED;
            auto pool = poolSettingsJson(settings);
            auto permit = Json::array();
            auto maps = Json::array();
            auto const& previous = std::as_const(body)["mapPool"];
            if (previous.isObject()) {
                if (previous["permit"].isArray())
                    for (auto const& uid : previous["permit"].asArray().unwrap()) permit.push(uid);
                if (previous["maps"].isArray())
                    for (auto const& entry : previous["maps"].asArray().unwrap())
                        if (poolLevelFits(parseLevel(entry["level"]), boolAt(entry, "twoPlayer"), settings))
                            maps.push(entry);
            }
            pool["permit"] = std::move(permit);
            pool["maps"] = std::move(maps);
            auto config = Json::object();
            config["random"] = false;
            config["mask"] = std::max(1, selection.mask);
            config["platformer"] = false;
            body["mapSelection"] = std::move(config);
            bool const reordered = old && old->settings.random != settings.random;
            if (!old || reordered) body.erase("mapDraw");
            if (reordered) body.erase("level");
            body["mapPool"] = std::move(pool);
            followMapPool(body);
            body["hostReady"] = false;
            body["guestReady"] = false;
            return {};
        },
        std::move(callback));
}
// Map Pool에 맵 추가. 추가 권한(호스트 또는 허가된 게스트), 조건 일치, 중복, 1인당 한도,
// 풀 크기 상한을 검사한다.
void Service::addToMapPool(LevelInfo level, bool twoPlayer, Done callback) {
    if (level.id <= 0 || level.platformer || level.length == 5) {
        callback(false, "Versus supports Classic maps only.");
        return;
    }
    if (inGroup()) {
        auto body = Json::object();
        body["level"] = levelJson(level);
        body["twoPlayer"] = twoPlayer;
        groupAction("pool-add", std::move(body), std::move(callback));
        return;
    }
    poolWrite(
        "pool-add",
        [level = std::move(level), twoPlayer](Json& body) -> std::string {
            if (!hasPool(body) || boolAt(body, "started")) return "The room changed.";
            if (std::as_const(body)["mapDraw"].isObject() && !boolAt(std::as_const(body)["mapDraw"], "settled"))
                return "Wait for the roulette.";
            auto const uid = state().profile.uid;
            bool const host = stringAt(std::as_const(body)["host"], "uid") == uid;
            if (!host && stringAt(std::as_const(body)["guest"], "uid") != uid) return "Your room connection expired.";
            auto const pool = *parseMapPool(std::as_const(body)["mapPool"]);
            auto const hostUid = stringAt(std::as_const(body)["host"], "uid");
            if (!pool.permitted(uid, hostUid)) return "The host has not let you add maps.";
            if (auto why = poolMisfit(level, twoPlayer, pool.settings); !why.empty()) return why;
            for (auto const& entry : pool.maps)
                if (entry.level.id == level.id) return "This map is already in the Map Pool.";
            if (pool.addedBy(uid) >= pool.settings.perPlayer)
                return fmt::format("You can add {} map{} at most.", pool.settings.perPlayer,
                                   pool.settings.perPlayer == 1 ? "" : "s");
            if (pool.maps.size() >= kPoolMapsMax) return "The Map Pool is full.";
            auto entry = Json::object();
            entry["level"] = levelJson(level);
            entry["twoPlayer"] = twoPlayer;
            entry["by"] = uid;
            entry["name"] = stringAt(std::as_const(body)[host ? "host" : "guest"], "name");
            body["mapPool"]["maps"].push(std::move(entry));
            followMapPool(body);
            return {};
        },
        std::move(callback));
}
// Map Pool에서 맵 제거. 호스트이거나 그 맵을 추가한 본인만 가능하다.
void Service::removeFromMapPool(int64_t levelId, Done callback) {
    if (inGroup()) {
        auto body = Json::object();
        body["id"] = levelId;
        groupAction("pool-remove", std::move(body), std::move(callback));
        return;
    }
    poolWrite(
        "pool-remove",
        [levelId](Json& body) -> std::string {
            if (!hasPool(body) || boolAt(body, "started")) return "The room changed.";
            if (std::as_const(body)["mapDraw"].isObject() && !boolAt(std::as_const(body)["mapDraw"], "settled"))
                return "Wait for the roulette.";
            auto const uid = state().profile.uid;
            bool const host = stringAt(std::as_const(body)["host"], "uid") == uid;
            auto maps = Json::array();
            bool found = false;
            for (auto const& entry : std::as_const(body)["mapPool"]["maps"].asArray().unwrap()) {
                if (!found && intAt(entry["level"], "id") == levelId) {
                    if (!host && stringAt(entry, "by") != uid)
                        return "Only the host or the player who added it can remove it.";
                    found = true;
                    continue;
                }
                maps.push(entry);
            }
            if (!found) return ROOM_UNCHANGED;
            body["mapPool"]["maps"] = std::move(maps);
            followMapPool(body);
            return {};
        },
        std::move(callback));
}
// 호스트가 게스트에게 Map Pool 추가 권한을 주거나 회수한다.
void Service::permitMapPool(std::string uid, bool allowed, Done callback) {
    if (inGroup()) {
        auto body = Json::object();
        if (uid.empty())
            body["all"] = allowed;
        else {
            body["uid"] = uid;
            body["allowed"] = allowed;
        }
        groupAction("pool-permit", std::move(body), std::move(callback));
        return;
    }
    if (!isHost()) {
        callback(false, "Only the host can change who adds maps.");
        return;
    }
    poolWrite(
        "pool-permit",
        [uid = std::move(uid), allowed](Json& body) -> std::string {
            if (!hasPool(body) || stringAt(std::as_const(body)["host"], "uid") != state().profile.uid)
                return "The room changed.";
            auto const guest = stringAt(std::as_const(body)["guest"], "uid");
            if (guest.empty() || (!uid.empty() && uid != guest)) return ROOM_UNCHANGED;
            auto permit = Json::array();
            if (allowed) permit.push(guest);
            body["mapPool"]["permit"] = std::move(permit);
            return {};
        },
        std::move(callback));
}
// 맵 룰렛 다시 돌리기(호스트). 아직 가능한 상태이고 추첨 id가 같을 때만 허용한다.
void Service::rerollMap(std::string drawId, Done callback) {
    if (inGroup()) {
        if (!isHost()) {
            callback(false, "Only the host can re-roll.");
            return;
        }
        groupAction("reroll", Json::object(), std::move(callback));
        return;
    }
    if (state().maintenanceWriting) {
        deferRoomAction([drawId](Done done) { Service::get().rerollMap(drawId, std::move(done)); },
                        std::move(callback));
        return;
    }
    if (busy() || !isHost()) {
        callback(false, "Only the host can re-roll.");
        return;
    }
    auto const epoch = state().epoch;
    state().writing = true;
    mutateRoom(
        state().room->id, epoch, "reroll-map",
        [drawId](Json& body) -> std::string {
            auto const room = parseRoom("", body);
            if (room.host.uid != state().profile.uid || !canRerollMap(room) || room.mapDraw->id != drawId)
                return "The map or match changed. Re-roll is no longer available.";
            body.erase("mapDraw");
            body.erase("level");
            return {};
        },
        [epoch, callback = std::move(callback)](bool ok, std::string detail) mutable {
            if (epoch == state().epoch) {
                state().writing = false;
                state().pollTime = 2.f;
            }
            callback(ok, std::move(detail));
        });
}
// 룰렛이 끝나면 결과를 확정한다. 연출 시간(DRAW_END_MS)이 지난 뒤에만 가능하며,
// Map Pool이면 뽑힌 맵을 풀의 맨 앞으로 올리고, 아니면 조건에 맞는지 다시 확인해 level에 기록한다.
void Service::finishMapDraw(std::string id, Done callback) {
    if (inGroup()) {
        callback(false, "Group rooms play a chosen map.");
        return;
    }
    if (state().writing || busy() || !isHost()) {
        callback(false, "Room is busy.");
        return;
    }
    auto const epoch = state().epoch;
    state().writing = true;
    mutateRoom(
        state().room->id, epoch, "finish-map-draw",
        [id](Json& body) -> std::string {
            auto room = parseRoom("", body);
            if (!room.mapDraw || room.mapDraw->id != id || boolAt(body, "started")) return "Roulette changed.";
            if (room.mapDraw->settled) return ROOM_UNCHANGED;
            if (nowMs() < room.mapDraw->at + DRAW_END_MS) return "Roulette is still spinning.";
            if (room.mapPool) {
                if (!room.guest) return "Room changed.";
                auto const& draw = std::as_const(body)["mapDraw"];
                auto const key = std::to_string(room.mapDraw->selected);
                Json const chosen = draw["levels"].isArray()
                                        ? draw["levels"][static_cast<size_t>(room.mapDraw->selected)]
                                        : draw["levels"][key];
                int64_t const id = intAt(chosen, "id");
                auto top = Json::array(), rest = Json::array();
                for (auto const& entry : std::as_const(body)["mapPool"]["maps"].asArray().unwrap())
                    (intAt(entry["level"], "id") == id ? top : rest).push(entry);
                if (top.asArray().unwrap().empty()) return "The Map Pool changed.";
                for (auto const& entry : rest.asArray().unwrap()) top.push(entry);
                body["mapPool"]["maps"] = top;
                body["level"] = chosen;
                body["mapDraw"]["settled"] = true;
                applyAutoLives(body);
                body["hostReady"] = false;
                body["guestReady"] = false;
                return {};
            }
            auto const& level = room.mapDraw->levels[room.mapDraw->selected];
            if (!room.guest || !matchesRandomMap(level, room.mapSelection)) return "Room/filter changed.";
            body["level"] = levelJson(level);
            body["mapDraw"]["settled"] = true;
            applyAutoLives(body);
            body["hostReady"] = false;
            body["guestReady"] = false;
            return {};
        },
        [epoch, callback = std::move(callback)](bool ok, std::string detail) mutable {
            if (epoch == state().epoch) state().writing = false;
            callback(ok, std::move(detail));
        });
}
// 게임 규칙 변경(호스트). 값 범위를 검사하고 자동 목숨이면 맵에 맞춰 다시 계산한다.
// Percent 모드는 플랫포머 맵에서 쓸 수 없다. 규칙이 실제로 바뀌면 준비를 해제한다.
void Service::configureRules(GameRules rules, Done callback) {
    if (inGroup()) {
        if (!isHost()) {
            callback(false, "Only the host can change game rules.");
            return;
        }
        if (rules.attempts < 1 || rules.attempts > 99) {
            callback(false, "Invalid game rules.");
            return;
        }
        auto body = Json::object();
        body["attempts"] = rules.attempts;
        groupAction("settings", std::move(body), std::move(callback));
        return;
    }
    if (state().writing || state().authenticating) {
        deferRoomAction([rules](Done done) { Service::get().configureRules(rules, std::move(done)); },
                        std::move(callback));
        return;
    }
    if (!isHost() || busy()) {
        callback(false, "Only the host can change game rules.");
        return;
    }
    if ((rules.mode != 0 && rules.mode != 1) || rules.attempts < 1 || rules.attempts > 99 || rules.targetPercent < 1 ||
        rules.targetPercent > 100 || !validRuleSpeed(rules.speed)) {
        callback(false, "Invalid game rules.");
        return;
    }
    auto const epoch = state().epoch;
    state().writing = true;
    mutateRoom(
        state().room->id, epoch, "configure-rules",
        [rules](Json& body) -> std::string {
            if (stringAt(std::as_const(body)["host"], "uid") != state().profile.uid)
                return "Only the host can change game rules.";
            if (boolAt(body, "started")) return "The match has already started.";
            if (std::as_const(body)["mapDraw"].isObject() && !boolAt(std::as_const(body)["mapDraw"], "settled"))
                return "Wait for the roulette.";
            auto newRules = rulesJson(rules);
            {
                Json probe = body;
                probe["rules"] = newRules;
                applyAutoLives(probe);
                newRules = std::as_const(probe)["rules"];
            }
            auto const selection = parseMapSelection(std::as_const(body)["mapSelection"]);
            if (rules.mode == 1 &&
                (boolAt(std::as_const(body)["level"], "platformer") || (selection.random && selection.platformer)))
                return "Platformer maps do not support Percent mode. Choose Attempts mode first.";
            if (!sameJson(std::as_const(body)["rules"], newRules)) {
                body["rules"] = newRules;
                body["hostReady"] = false;
                body["guestReady"] = false;
            }
            body["hostSeen"] = timestamp();
            return {};
        },
        [epoch, callback = std::move(callback)](bool ok, std::string error) mutable {
            if (epoch == state().epoch) state().writing = false;
            callback(ok, std::move(error));
        });
}
// 준비 상태 변경. 준비 시점에 본 맵/규칙/맵 선택이 서버의 현재 값과 다르면 거부해서
// 상대(호스트)가 바꾼 설정에 동의 없이 준비되지 않게 한다.
void Service::setReady(bool ready, Done callback) {
    if (inGroup()) {
        auto body = Json::object();
        body["ready"] = ready;
        groupAction("ready", std::move(body), std::move(callback));
        return;
    }
    if (state().writing || state().authenticating) {
        auto const expectedLevel = state().room ? state().room->level : LevelInfo{};
        auto const expectedRules = state().room ? state().room->rules : GameRules{};
        auto const expectedSelection = state().room ? state().room->mapSelection : MapSelection{};
        deferRoomAction(
            [ready, expectedLevel, expectedRules, expectedSelection](Done done) {
                auto const& current = state().room;
                if (ready && (!current || current->level != expectedLevel || current->rules != expectedRules ||
                              current->mapSelection != expectedSelection)) {
                    done(false, "The map or rules changed. Check them and ready again.");
                    return;
                }
                Service::get().setReady(ready, std::move(done));
            },
            std::move(callback));
        return;
    }
    if (!state().room || busy()) {
        callback(false, "Room is unavailable.");
        return;
    }
    auto const epoch = state().epoch;
    auto const expectedLevel = state().room->level;
    auto const expectedRules = state().room->rules;
    auto const expectedSelection = state().room->mapSelection;
    state().writing = true;
    mutateRoom(
        state().room->id, epoch, "ready",
        [ready, expectedLevel, expectedRules, expectedSelection](Json& body) -> std::string {
            auto const uid = state().profile.uid;
            bool const host = stringAt(std::as_const(body)["host"], "uid") == uid;
            if (!host && stringAt(std::as_const(body)["guest"], "uid") != uid) return "Your room connection expired.";
            if (boolAt(body, "started")) return "The match has already started.";
            auto const current = parseRoom("", body);
            if (drawingMap(current)) return "Wait for the roulette.";
            if (ready &&
                ((!needsRandomDraw(current) && intAt(std::as_const(body)["level"], "id") <= 0) ||
                 current.mapSelection != expectedSelection || current.level.platformer != expectedLevel.platformer ||
                 current.level.creator != expectedLevel.creator ||
                 current.level.featureState != expectedLevel.featureState ||
                 intAt(std::as_const(body)["level"], "id") != expectedLevel.id ||
                 stringAt(std::as_const(body)["level"], "name") != expectedLevel.name ||
                 intAt(std::as_const(body)["level"], "stars") != expectedLevel.stars ||
                 intAt(std::as_const(body)["level"], "difficulty") != expectedLevel.difficulty ||
                 boolAt(std::as_const(body)["level"], "demon") != expectedLevel.demon ||
                 boolAt(std::as_const(body)["level"], "autoLevel") != expectedLevel.autoLevel ||
                 intAt(std::as_const(body)["level"], "userCoins") != expectedLevel.userCoins ||
                 parseRules(std::as_const(body)["rules"]) != expectedRules))
                return "The map or rules changed. Check them and ready again.";
            body[host ? "hostReady" : "guestReady"] = ready;
            body[host ? "hostSeen" : "guestSeen"] = timestamp();
            return {};
        },
        [epoch, callback = std::move(callback)](bool ok, std::string error) mutable {
            if (epoch == state().epoch) state().writing = false;
            callback(ok, std::move(error));
        });
}
// 빠른 채팅은 정해진 문구 목록만 허용한다.
static bool validChatText(std::string const& text) {
    static std::unordered_set<std::string> const allowed = {
        "Hello!",       "Good Luck!", "Have Fun!", "Pro",  "Respect", "Let's go!", "glhf!",
        "I have to go", "Bye!",       "One more?", "Yes!", "No!",     "GG"};
    return allowed.contains(text);
}
// 이모트 / 빠른 채팅 전송. 멤버 전용 이모트는 멤버십이 필요하다.
// 1v1은 방 JSON의 emote에 nonce와 함께 기록하며 쿨다운(EMOTE_COOLDOWN_MS)을 둔다.
void sendRoomSignal(std::string kind, std::string text, Done callback) {
    auto const* emote = emotes::find(kind);
    if (kind != "chat" && !emote) {
        callback(false, "Unknown room signal.");
        return;
    }
    if (kind == "chat" && !validChatText(text)) {
        callback(false, "Unknown quick-chat message.");
        return;
    }
    if (emote && emote->member && !Service::get().profile().membership) {
        callback(false, "Enter a valid membership code to use this emote.");
        return;
    }
    if (groupActive()) {
        auto body = Json::object();
        body["kind"] = kind;
        if (kind == "chat") body["text"] = text;
        groupAction("emote", std::move(body), std::move(callback));
        return;
    }
    if (state().writing || state().authenticating) {
        deferRoomAction([kind = std::move(kind), text = std::move(text)](
                            Done done) mutable { sendRoomSignal(std::move(kind), std::move(text), std::move(done)); },
                        std::move(callback));
        return;
    }
    if (!state().room || !state().room->guest || Service::get().busy()) {
        callback(false, "Wait for the other player.");
        return;
    }
    auto const epoch = state().epoch;
    state().writing = true;
    mutateRoom(
        state().room->id, epoch, "emote",
        [kind = std::move(kind), text = std::move(text), nonce = randomId()](Json& body) -> std::string {
            auto const uid = state().profile.uid;
            bool const host = stringAt(std::as_const(body)["host"], "uid") == uid;
            if (!std::as_const(body)["guest"].isObject() ||
                (!host && stringAt(std::as_const(body)["guest"], "uid") != uid))
                return "Wait for the other player.";
            auto const timeKey = host ? "hostEmoteAt" : "guestEmoteAt";
            if (nowMs() - intAt(body, timeKey) < firebase_config::EMOTE_COOLDOWN_MS)
                return "Wait a moment between emotes.";
            auto emote = Json::object();
            emote["uid"] = uid;
            emote["kind"] = kind;
            if (kind == "chat") emote["text"] = text;
            emote["at"] = timestamp();
            emote["nonce"] = nonce;
            body["emote"] = std::move(emote);
            body[timeKey] = timestamp();
            return {};
        },
        [epoch, callback = std::move(callback)](bool ok, std::string error) mutable {
            if (epoch == state().epoch) state().writing = false;
            callback(ok, std::move(error));
        });
}
// 전투 시작. Group은 서버 액션(draw / start)으로 처리하고, 랜덤 맵 조건이면 먼저 후보를
// 검색해 추첨(draw)을 요청한다.
// 1v1에서 Map Pool 추첨이 필요하면 호스트가 후보 최대 10개와 당첨 맵을 뽑아 mapDraw로 기록한다.
void Service::startMatch(Done callback) {
    if (inGroup()) {
        if (!isHost()) {
            callback(false, "Only the host can start the battle.");
            return;
        }
        auto const& group = *state().group;
        if (group.mapPool && poolRolls(*group.mapPool) && group.level.id <= 0) {
            if (group.mapDraw && !group.mapDraw->settled) {
                callback(false, "Wait for the roulette.");
                return;
            }
            auto body = Json::object();
            body["pool"] = true;
            groupAction("draw", std::move(body), std::move(callback));
            return;
        }
        if (group.mapSelection.random && (!group.mapDraw || !group.mapDraw->settled)) {
            if (group.mapDraw) {
                callback(false, "Wait for the roulette.");
                return;
            }
            auto const epoch = state().epoch;
            auto const groupId = group.id;
            state().searchingMap = true;
            findRandomMaps(group.mapSelection,
                           [epoch, groupId, rated = group.mapSelection.rated,
                            callback = std::move(callback)](std::vector<LevelInfo> levels, std::string error) mutable {
                               state().searchingMap = false;
                               if (epoch != state().epoch || !state().group || state().group->id != groupId) {
                                   callback(false, "Room changed.");
                                   return;
                               }
                               if (!error.empty() || levels.size() < MIN_RANDOM_CANDIDATES) {
                                   callback(false, !error.empty() ? std::move(error)
                                                   : rated        ? "No rated maps found."
                                                                  : "No maps found.");
                                   return;
                               }
                               if (levels.size() > 10) levels.resize(10);
                               auto list = Json::array();
                               for (auto const& level : levels) list.push(levelJson(level));
                               auto body = Json::object();
                               body["levels"] = std::move(list);
                               groupAction("draw", std::move(body), std::move(callback));
                           });
            return;
        }
        groupAction("start", Json::object(), std::move(callback));
        return;
    }
    if (state().maintenanceWriting) {
        deferRoomAction([](Done done) { Service::get().startMatch(std::move(done)); }, std::move(callback));
        return;
    }
    if (!isHost() || busy()) {
        callback(false, "Only the host can start the match.");
        return;
    }
    if (state().room->mapPool && needsRandomDraw(*state().room)) {
        auto const expected = *state().room;
        if (drawingMap(expected) || !expected.guest || !expected.hostReady || !expected.guestReady) {
            callback(false, "Both players must be ready for the roulette.");
            return;
        }
        auto const epoch = state().epoch;
        deferRoomAction(
            [expected, epoch](Done done) {
                state().writing = true;
                mutateRoom(
                    expected.id, epoch, "pool-draw",
                    [expected](Json& body) -> std::string {
                        auto const current = parseRoom(expected.id, body);
                        if (current.started || current.host.uid != state().profile.uid || !current.guest ||
                            current.guest->uid != expected.guest->uid || current.mapDraw || current.level.id > 0 ||
                            !current.mapPool || !poolRolls(*current.mapPool) ||
                            nowMs() - intAt(body, "guestSeen") > firebase_config::PRESENCE_TIMEOUT_MS ||
                            !current.hostReady || !current.guestReady)
                            return "Room, readiness or the Map Pool changed. Try again.";
                        Json const maps = std::as_const(body)["mapPool"]["maps"];
                        auto const& entries = maps.asArray().unwrap();
                        std::mt19937 random{std::random_device{}()};
                        std::vector<size_t> others;
                        size_t const chosen = std::uniform_int_distribution<size_t>(0, entries.size() - 1)(random);
                        for (size_t i = 0; i < entries.size(); ++i)
                            if (i != chosen) others.push_back(i);
                        std::shuffle(others.begin(), others.end(), random);
                        others.resize(std::min<size_t>(others.size(), 9));
                        others.push_back(chosen);
                        std::sort(others.begin(), others.end());
                        auto draw = Json::object();
                        draw["id"] = randomId();
                        draw["at"] = timestamp();
                        draw["settled"] = false;
                        draw["levels"] = Json::array();
                        for (size_t i = 0; i < others.size(); ++i) {
                            draw["levels"].push(entries[others[i]]["level"]);
                            if (others[i] == chosen) draw["selected"] = std::to_string(i);
                        }
                        body["mapDraw"] = std::move(draw);
                        body.erase("level");
                        body["hostReady"] = false;
                        body["guestReady"] = false;
                        body["hostSeen"] = timestamp();
                        return {};
                    },
                    [epoch, done = std::move(done)](bool ok, std::string detail) mutable {
                        if (epoch == state().epoch) {
                            state().writing = false;
                            state().pollTime = 2.f;
                        }
                        done(ok, std::move(detail));
                    });
            },
            std::move(callback));
        return;
    }
    // 랜덤 맵 방: 조건에 맞는 후보를 검색해 룰렛(mapDraw)을 방에 게시한다.
    // 게시 시점에 방/준비/조건/게스트 접속이 그대로일 때만 반영하고, 반영하면 양쪽 준비를 해제한다.
    if (needsRandomDraw(*state().room)) {
        auto const expected = *state().room;
        if (drawingMap(expected) || !expected.guest || !expected.hostReady || !expected.guestReady) {
            callback(false, "Both players must be ready for the roulette.");
            return;
        }
        auto const epoch = state().epoch;
        state().searchingMap = true;
        findRandomMaps(expected.mapSelection, [epoch, expected, callback = std::move(callback)](
                                                  std::vector<LevelInfo> levels, std::string error) mutable {
            if (epoch != state().epoch) {
                callback(false, "Room changed.");
                return;
            }
            state().searchingMap = false;
            if (!error.empty() || levels.size() < MIN_RANDOM_CANDIDATES) {
                callback(false, !error.empty()                ? std::move(error)
                                : expected.mapSelection.rated ? "No rated maps found."
                                                              : "No maps found.");
                return;
            }
            std::mt19937 random{std::random_device{}()};
            auto const selected = std::uniform_int_distribution<int>(0, static_cast<int>(levels.size()) - 1)(random);
            auto draw = Json::object();
            draw["id"] = randomId();
            draw["at"] = timestamp();
            draw["selected"] = std::to_string(selected);
            draw["settled"] = false;
            draw["levels"] = Json::array();
            for (auto const& level : levels) draw["levels"].push(levelJson(level));
            deferRoomAction(
                [expected, draw, epoch](Done done) {
                    state().writing = true;
                    mutateRoom(
                        expected.id, epoch, "publish-map-draw",
                        [expected, draw](Json& body) -> std::string {
                            auto current = parseRoom(expected.id, body);
                            if (current.started || current.host.uid != state().profile.uid || !current.guest ||
                                current.guest->uid != expected.guest->uid ||
                                current.mapSelection != expected.mapSelection || current.rules != expected.rules ||
                                current.mapDraw ||
                                nowMs() - intAt(body, "guestSeen") > firebase_config::PRESENCE_TIMEOUT_MS ||
                                !current.hostReady || !current.guestReady)
                                return "Room, readiness or filters changed. Try again.";
                            body["mapDraw"] = draw;
                            body.erase("level");
                            body["hostReady"] = false;
                            body["guestReady"] = false;
                            body["hostSeen"] = timestamp();
                            return {};
                        },
                        [epoch, done = std::move(done)](bool ok, std::string detail) mutable {
                            if (epoch == state().epoch) {
                                state().writing = false;
                                state().pollTime = 2.f;
                            }
                            done(ok, std::move(detail));
                        });
                },
                std::move(callback));
        });
        return;
    }
    // 일반 시작: 조건(게스트 접속, 양쪽 준비, 맵, 룰렛 완료, 규칙 범위)을 검사한 뒤
    // launch(양쪽 로드 대기)와 초기 battle 상태를 방에 기록하고 started를 켠다. id는 launchId로 통일.
    auto const epoch = state().epoch;
    state().writing = true;
    mutateRoom(
        state().room->id, epoch, "start-match",
        [launchId = randomId()](Json& body) -> std::string {
            if (stringAt(std::as_const(body)["host"], "uid") != state().profile.uid)
                return "Only the host can start the match.";
            if (boolAt(body, "started")) return "The match has already started.";
            if (!std::as_const(body)["guest"].isObject() ||
                nowMs() - intAt(body, "guestSeen") > firebase_config::PRESENCE_TIMEOUT_MS)
                return "Wait for another player.";
            if (!boolAt(body, "hostReady") || !boolAt(body, "guestReady")) return "Both players must be ready.";
            if (intAt(std::as_const(body)["level"], "id") <= 0) return "Choose a level first.";
            auto const room = parseRoom("", body);
            if (needsRandomDraw(room)) return "Complete the roulette first.";
            auto const rules = parseRules(std::as_const(body)["rules"]);
            if ((rules.mode != 0 && rules.mode != 1) || rules.attempts < 1 || rules.attempts > 99 ||
                rules.targetPercent < 1 || rules.targetPercent > 100)
                return "Invalid game rules. Apply the game rules again.";
            auto launch = Json::object();
            launch["id"] = launchId;
            launch["requestedAt"] = timestamp();
            launch["hostLoaded"] = false;
            launch["guestLoaded"] = false;
            launch["releasedAt"] = 0;
            auto battle = Json::object();
            auto const hostUid = stringAt(std::as_const(body)["host"], "uid");
            battle["id"] = launchId;
            battle["firstUid"] = hostUid;
            battle["activeUid"] = hostUid;
            battle["firstReachedUid"] = "";
            battle["opponentRunAtFirst"] = 0;
            battle["winnerUid"] = "";
            battle["draw"] = false;
            battle["finishedAt"] = 0;
            battle["host"] = initialBattlePlayer();
            battle["guest"] = initialBattlePlayer();
            body["battle"] = std::move(battle);
            body["launch"] = std::move(launch);
            body["started"] = true;
            body["hostSeen"] = timestamp();
            return {};
        },
        [epoch, callback = std::move(callback)](bool ok, std::string error) mutable {
            if (epoch == state().epoch) {
                state().writing = false;
                state().pollTime = 2.f;
            }
            callback(ok, std::move(error));
        });
}
// 내 쪽 맵 로딩이 끝났음을 알린다. 양쪽이 모두 로드하면 releasedAt이 찍혀 동시에 시작한다.
// 요청 후 60초가 지나면 로딩 시간 초과로 거부한다.
void Service::markLoaded(std::string launchId, Done callback) {
    if (state().botBattle) {
        auto& s = state();
        if (!s.room || !s.room->launch || s.room->launch->id != launchId) {
            callback(false, "The match was cancelled.");
            return;
        }
        s.room->launch->hostLoaded = s.room->launch->guestLoaded = true;
        if (s.room->launch->releasedAt <= 0) s.room->launch->releasedAt = nowMs();
        callback(true, {});
        return;
    }
    if (inGroup()) {
        auto body = Json::object();
        body["battle"] = std::move(launchId);
        groupAction("loaded", std::move(body), std::move(callback));
        return;
    }
    if (state().writing || state().authenticating) {
        deferRoomAction([launchId = std::move(launchId)](
                            Done done) mutable { Service::get().markLoaded(std::move(launchId), std::move(done)); },
                        std::move(callback));
        return;
    }
    if (!state().room || busy()) {
        callback(false, "Your room connection expired.");
        return;
    }
    auto const epoch = state().epoch;
    state().writing = true;
    mutateRoom(
        state().room->id, epoch, "loaded",
        [launchId = std::move(launchId)](Json& body) -> std::string {
            auto& launch = body["launch"];
            if (!boolAt(body, "started") || stringAt(launch, "id") != launchId ||
                stringAt(std::as_const(body)["battle"], "id") != launchId)
                return "The match was cancelled.";
            auto const uid = state().profile.uid;
            bool const host = stringAt(std::as_const(body)["host"], "uid") == uid;
            if (!host && stringAt(std::as_const(body)["guest"], "uid") != uid) return "Your room connection expired.";
            auto const key = host ? "hostLoaded" : "guestLoaded";
            if (intAt(launch, "releasedAt") > 0) return boolAt(launch, key) ? "" : "The match has already started.";
            if (nowMs() >= intAt(launch, "requestedAt") + 60000) return "Loading timed out after 60 seconds.";
            launch[key] = true;
            if (boolAt(launch, "hostLoaded") && boolAt(launch, "guestLoaded")) launch["releasedAt"] = timestamp();
            body[host ? "hostSeen" : "guestSeen"] = timestamp();
            return {};
        },
        [epoch, callback = std::move(callback)](bool ok, std::string error) mutable {
            if (epoch == state().epoch) {
                state().writing = false;
                state().pollTime = 2.f;
            }
            callback(ok, std::move(error));
        });
}
// 시작 대기/전투를 취소하고 방을 대기실 상태로 되돌린다. 이미 끝난 전투면 먼저 방 점수를 확정하고,
// 랜덤 맵 / Map Pool이면 플레이한 맵을 비우거나 풀에서 뺀다.
void Service::cancelLaunch(std::string launchId, Done callback) {
    if (state().botBattle) {
        endBotBattle();
        callback(true, {});
        return;
    }
    if (inGroup()) {
        auto body = Json::object();
        body["battle"] = std::move(launchId);
        groupAction("cancel", std::move(body), std::move(callback));
        return;
    }
    if (state().writing || state().authenticating) {
        deferRoomAction([launchId = std::move(launchId)](
                            Done done) mutable { Service::get().cancelLaunch(std::move(launchId), std::move(done)); },
                        std::move(callback));
        return;
    }
    if (!state().room || !state().room->launch) {
        callback(true, {});
        return;
    }
    if (state().room->launch->id != launchId) {
        callback(false, "Match session changed.");
        return;
    }
    auto const epoch = state().epoch;
    state().writing = true;
    mutateRoom(
        state().room->id, epoch, "cancel-launch",
        [launchId = std::move(launchId)](Json& body) -> std::string {
            auto const uid = state().profile.uid;
            if (stringAt(std::as_const(body)["host"], "uid") != uid &&
                stringAt(std::as_const(body)["guest"], "uid") != uid)
                return "Your room connection expired.";
            if (!boolAt(body, "started") && !std::as_const(body)["launch"].isObject() &&
                !std::as_const(body)["battle"].isObject())
                return ROOM_UNCHANGED;
            if (stringAt(std::as_const(body)["launch"], "id") != launchId ||
                stringAt(std::as_const(body)["battle"], "id") != launchId)
                return "Match session changed.";
            bool const finished = intAt(std::as_const(body)["battle"], "finishedAt") > 0;
            if (finished) settleRoomScore(body);
            body["started"] = false;
            body["hostReady"] = false;
            body["guestReady"] = false;
            body["launch"] = nullptr;
            body["battle"] = nullptr;
            if (finished && boolAt(std::as_const(body)["mapSelection"], "random")) {
                body.erase("mapDraw");
                body.erase("level");
            }
            if (finished && hasPool(body)) {
                auto const played = intAt(std::as_const(body)["level"], "id");
                auto maps = Json::array();
                for (auto const& entry : std::as_const(body)["mapPool"]["maps"].asArray().unwrap())
                    if (intAt(entry["level"], "id") != played) maps.push(entry);
                body["mapPool"]["maps"] = std::move(maps);
                followMapPool(body);
            }
            return {};
        },
        [epoch, callback = std::move(callback)](bool ok, std::string error) mutable {
            if (epoch == state().epoch) {
                state().writing = false;
                state().pollTime = 2.f;
            }
            callback(ok, std::move(error));
        });
}
// 포기(forfeit)를 예약한다. 실제 전송은 tick에서 성공할 때까지 재시도하며,
// 봇 / 로컬 방에서는 바로 로컬 처리한다.
void Service::queueForfeit(BattlePlayerState progress) {
    auto& s = state();
    if (s.group) {
        if (s.group->battle && s.group->battle->finishedAt == 0) {
            s.groupForfeitBattle = s.group->battle->id;
            s.nextGroupForfeit = Clock::now();
        }
        return;
    }
    if (!s.room || !s.room->battle || s.room->battle->finishedAt > 0) return;
    progress.forfeited = true;
    progress.inAttempt = false;
    progress.spectating = true;
    progress.paused = false;
    progress.pausedAt = 0;
    if (s.botBattle) {
        botReport(std::move(progress));
        return;
    }
    if (s.localRoom) {
        soloReport(std::move(progress));
        return;
    }
    s.pendingForfeit = progress;
    s.pendingForfeitMatch = s.room->battle->id;
    s.nextForfeitAttempt = Clock::now();
}
// 전투 진행도를 서버에 보고한다. 보내기 전에 값을 검증하고 단조 증가 값(시도 횟수, 최고 %,
// 클리어, 코인, 포기, 일시정지 사용량)은 서버 값 아래로 내려가지 않게 보정한다.
// 무결성 코드는 서버가 알던 값과 달라지거나 포기가 아닌데 설정되면 거부한다.
// 위치/카메라는 상대가 관전 중일 때만 보내고 유한한 범위로 제한한다.
void Service::reportBattle(BattlePlayerState progress, bool sendPosition, Done callback) {
    if (inGroup()) {
        reportGroupBattle(std::move(progress), std::move(callback));
        return;
    }
    if (state().botBattle) {
        botReport(std::move(progress));
        callback(true, {});
        return;
    }
    if (state().localRoom) {
        soloReport(std::move(progress));
        callback(true, {});
        return;
    }
    if (!state().room || !state().room->battle || !state().room->launch) {
        callback(false, "The battle is no longer active.");
        return;
    }
    if (state().writing || state().polling || state().authenticating || busy()) {
        callback(false, "Room update in progress.");
        return;
    }
    auto const room = *state().room;
    auto const epoch = state().epoch;
    auto const matchId = room.battle->id;
    auto const uid = state().profile.uid;
    bool const host = room.host.uid == uid;
    if ((!host && (!room.guest || room.guest->uid != uid)) || !room.started || room.launch->id != matchId ||
        room.launch->releasedAt <= 0) {
        callback(false, "The battle is no longer active.");
        return;
    }
    auto const& current = host ? room.battle->host : room.battle->guest;
    auto const& other = host ? room.battle->guest : room.battle->host;
    auto const& rules = room.rules;
    if (room.battle->finishedAt > 0) {
        callback(true, {});
        return;
    }
    if (progress.attemptsUsed < current.attemptsUsed || progress.runNumber < current.runNumber ||
        progress.attemptsUsed < 0 || progress.runNumber < 0 ||
        progress.attemptsUsed > (rules.mode == 0 && !rules.practice ? rules.attempts : 1000000) ||
        progress.runNumber > 1000000 || progress.currentPercent < 0 || progress.currentPercent > 100 ||
        progress.bestPercent < 0 || progress.bestPercent > 100 || progress.userCoins < 0 || progress.userCoins > 3 ||
        (!progress.cleared && progress.userCoins != 0) || (progress.cleared && progress.bestPercent < 100)) {
        callback(false, "Invalid battle progress.");
        return;
    }
    if (!integrity::validCode(progress.integrityCode) ||
        !integrity::validFeatureDetails(progress.integrityCode, progress.integrityFeatures) ||
        (!progress.integrityCode.empty() && !progress.forfeited) ||
        (!current.integrityCode.empty() && (progress.integrityCode != current.integrityCode ||
                                            progress.integrityFeatures != current.integrityFeatures))) {
        callback(false, "Invalid anti-cheat result.");
        return;
    }
    progress.bestPercent = std::max(progress.bestPercent, current.bestPercent);
    progress.cleared = current.cleared || progress.cleared;
    progress.userCoins = std::max(progress.userCoins, current.userCoins);
    progress.forfeited = current.forfeited || progress.forfeited;
    progress.pauseUsedMs =
        std::clamp<int64_t>(std::max(progress.pauseUsedMs, current.pauseUsedMs), 0, battle::PAUSE_BUDGET_MS);
    if (!sendPosition || progress.spectating || !other.spectating) {
        progress.x = current.x;
        progress.y = current.y;
        progress.cameraX = current.cameraX;
        progress.cameraY = current.cameraY;
    } else {
        auto const finiteBounded = [](double value) {
            return std::isfinite(value) ? std::clamp(value, -100000000., 100000000.) : 0.;
        };
        progress.x = finiteBounded(progress.x);
        progress.y = finiteBounded(progress.y);
        progress.cameraX = finiteBounded(progress.cameraX);
        progress.cameraY = finiteBounded(progress.cameraY);
    }
    progress.updatedAt = 0;
    state().writing = true;
    state().battleReportWantedAt = {};
    ++state().revision;
    request(
        "PUT", "rooms/" + room.id + "/battle/player", battlePlayerJson(progress),
        [roomId = room.id, matchId, epoch, callback = std::move(callback)](web::WebResponse response) mutable {
            auto& s = state();
            if (epoch != s.epoch) {
                callback(false, "Room session changed.");
                return;
            }
            s.writing = false;
            s.pollTime = 0.f;
            if (!response.ok()) {
                callback(false, networkError(response));
                return;
            }
            auto parsed = response.json();
            if (!parsed || !adoptBattle(roomId, matchId, parsed.unwrap())) {
                callback(false, "Invalid battle response. Please retry.");
                return;
            }
            if (auto fresh = response.header("ETag")) s.battlePollEtag = *fresh;
            s.lastBattleReport = s.lastSeatAck = Clock::now();
            callback(true, {});
        },
        {}, false, false, false, {}, "battle-report");
}
// Ranked 상태 조회.
void Service::fetchRankedState(RankedStateCallback callback) {
    rankedRequest("GET", rankedPath("state"), {}, std::move(callback));
}
// 서버의 최신 릴리스 버전 / 패치 노트(영어, 한국어)를 받는다.
void Service::fetchRelease(std::function<void(std::string, std::string)> callback) {
    request("GET", "release", {}, [callback = std::move(callback)](web::WebResponse response) {
        auto value = response.json();
        callback(response.ok() && value ? stringAt(value.unwrap(), "version") : "",
                 response.ok() ? "" : networkError(response));
    });
}
void Service::fetchPatchNotes(std::function<void(std::string, std::string, std::string)> callback) {
    request("GET", "patchnotes", {}, [callback = std::move(callback)](web::WebResponse response) {
        auto value = response.json();
        if (!response.ok() || !value) {
            callback("", "", networkError(response));
            return;
        }
        callback(stringAt(value.unwrap(), "en"), stringAt(value.unwrap(), "ko"), "");
    });
}
// Live 목록 / 상세의 매치 항목 파싱 (플레이어는 최대 2명).
static LiveMatch parseLiveMatch(Json const& entry) {
    LiveMatch match;
    match.roomId = stringAt(entry, "roomId");
    match.name = stringAt(entry, "name");
    match.battleId = stringAt(entry, "battleId");
    match.avgRank = static_cast<int>(std::clamp<int64_t>(intAt(entry, "avgRank"), 0, 100'000'000));
    match.ranked = boolAt(entry, "ranked");
    match.level = parseLevel(entry["level"]);
    match.rules = parseRules(entry["rules"]);
    match.releasedAt = intAt(entry, "releasedAt");
    match.scoreHost = static_cast<int>(intAt(entry["score"], "host"));
    match.scoreGuest = static_cast<int>(intAt(entry["score"], "guest"));
    if (entry["players"].isArray()) {
        size_t index = 0;
        for (auto const& player : entry["players"].asArray().unwrap()) {
            if (index >= 2) break;
            auto& slot = match.players[index++];
            slot.profile = parseProfile(player["profile"]);
            slot.state = parseBattlePlayer(player["state"]);
            slot.tier = tierAt(player, "tier");
            slot.globalRank = static_cast<int>(intAt(player, "globalRank"));
        }
    }
    return match;
}
namespace {
// 맵 투표 캐시 유효 시간과 한 번에 조회하는 최대 맵 수.
constexpr auto kMapVotesFresh = std::chrono::seconds(15);
constexpr size_t kMapVotesMax = 12;
MapVotes parseMapVotes(Json const& value) {
    MapVotes votes;
    votes.up = static_cast<int>(std::clamp<int64_t>(intAt(value, "up"), 0, 1'000'000'000));
    votes.down = static_cast<int>(std::clamp<int64_t>(intAt(value, "down"), 0, 1'000'000'000));
    votes.mine = static_cast<int>(std::clamp<int64_t>(intAt(value, "mine"), -1, 1));
    return votes;
}
// 맵 투표 조회를 기다리던 호출들 중 필요한 항목이 모두 도착(또는 실패)한 것부터 응답한다.
void answerMapVoteWaiters(std::vector<int64_t> const& failed, std::string const& error) {
    auto& s = state();
    auto waiters = std::move(s.mapVoteWaiters);
    s.mapVoteWaiters.clear();
    for (auto& [ids, callback] : waiters) {
        bool ready = true, broken = false;
        std::map<int64_t, MapVotes> found;
        for (auto id : ids) {
            auto kept = s.mapVotes.find(id);
            if (kept != s.mapVotes.end())
                found[id] = kept->second.second;
            else if (s.mapVotesLoading.count(id))
                ready = false;
            else if (std::find(failed.begin(), failed.end(), id) != failed.end())
                broken = true;
        }
        if (!ready && !broken) {
            s.mapVoteWaiters.emplace_back(std::move(ids), std::move(callback));
            continue;
        }
        callback(std::move(found), broken ? error : std::string());
    }
}
}
// 맵 투표 수(좋아요/싫어요/내 투표)를 한 번에 조회한다. 캐시가 신선하면 요청 없이 응답하고,
// 이미 조회 중인 id는 기다리기만 해서 같은 요청이 겹치지 않게 한다. 캐시는 512개를 넘으면 오래된 것을 정리한다.
void Service::fetchMapVotes(std::vector<int64_t> ids, MapVotesCallback callback) {
    auto& s = state();
    std::vector<int64_t> wanted;
    auto const now = Clock::now();
    for (auto id : ids) {
        if (id <= 0 || id > std::numeric_limits<int>::max() ||
            std::find(wanted.begin(), wanted.end(), id) != wanted.end())
            continue;
        if (wanted.size() >= kMapVotesMax) break;
        wanted.push_back(id);
    }
    if (wanted.empty()) {
        callback({}, {});
        return;
    }
    std::vector<int64_t> stale;
    for (auto id : wanted) {
        auto kept = s.mapVotes.find(id);
        if ((kept == s.mapVotes.end() || now - kept->second.first > kMapVotesFresh) && !s.mapVotesLoading.count(id))
            stale.push_back(id);
    }
    bool const waiting =
        std::any_of(wanted.begin(), wanted.end(), [&s](int64_t id) { return s.mapVotesLoading.count(id); });
    if (stale.empty() && !waiting) {
        std::map<int64_t, MapVotes> found;
        for (auto id : wanted) found[id] = s.mapVotes[id].second;
        callback(std::move(found), {});
        return;
    }
    s.mapVoteWaiters.emplace_back(wanted, std::move(callback));
    if (stale.empty()) return;
    for (auto id : stale) s.mapVotesLoading.insert(id);
    connect([stale](bool ok, std::string detail) {
        if (!ok) {
            for (auto id : stale) state().mapVotesLoading.erase(id);
            answerMapVoteWaiters(stale, detail.empty() ? std::string("Not signed in.") : detail);
            return;
        }
        auto body = Json::object();
        auto list = Json::array();
        for (auto id : stale) list.push(id);
        body["ids"] = std::move(list);
        request("PUT", "maps/votes", std::move(body), [stale](web::WebResponse response) {
            auto& s = state();
            for (auto id : stale) s.mapVotesLoading.erase(id);
            auto value = response.json();
            if (!response.ok() || !value || !value.unwrap().isObject()) {
                answerMapVoteWaiters(stale, networkError(response));
                return;
            }
            auto const now = Clock::now();
            for (auto id : stale) {
                auto const& entry = value.unwrap()[std::to_string(id)];
                s.mapVotes[id] = {now, entry.isObject() ? parseMapVotes(entry) : MapVotes{}};
            }
            if (s.mapVotes.size() > 512) {
                std::vector<std::pair<Clock::time_point, int64_t>> ages;
                for (auto const& [id, kept] : s.mapVotes) ages.emplace_back(kept.first, id);
                std::sort(ages.begin(), ages.end());
                for (size_t i = 0; i + 256 < ages.size(); ++i) s.mapVotes.erase(ages[i].second);
            }
            answerMapVoteWaiters({}, {});
        });
    });
}
std::optional<MapVotes> Service::knownMapVotes(int64_t id) const {
    auto const& kept = state().mapVotes;
    auto found = kept.find(id);
    if (found == kept.end()) return std::nullopt;
    return found->second.second;
}
// 맵에 투표한다 (-1 싫어요, 0 취소, 1 좋아요). 이름은 64자로 자르고 제어 문자를 공백으로 바꾼다.
void Service::voteMap(int64_t id, std::string name, int vote,
                      std::function<void(std::optional<MapVotes>, std::string)> callback) {
    if (id <= 0 || id > std::numeric_limits<int>::max() || vote < -1 || vote > 1) {
        callback(std::nullopt, "Invalid map.");
        return;
    }
    if (name.size() > 64) name.resize(64);
    for (auto& c : name)
        if (static_cast<unsigned char>(c) < 0x20 || c == 0x7f) c = ' ';
    connect([id, vote, name = std::move(name), callback = std::move(callback)](bool ok, std::string detail) mutable {
        if (!ok) {
            callback(std::nullopt, detail.empty() ? std::string("Not signed in.") : std::move(detail));
            return;
        }
        auto body = Json::object();
        body["vote"] = vote;
        body["name"] = std::move(name);
        request("PUT", fmt::format("maps/{}/vote", id), std::move(body),
                [id, callback = std::move(callback)](web::WebResponse response) mutable {
                    auto value = response.json();
                    if (!response.ok() || !value || !value.unwrap().isObject()) {
                        callback(std::nullopt, networkError(response));
                        return;
                    }
                    auto votes = parseMapVotes(value.unwrap());
                    state().mapVotes[id] = {Clock::now(), votes};
                    callback(votes, {});
                });
    });
}
// 플레이어가 투표한 맵 목록 (최대 300건).
void Service::fetchPlayerMapVotes(std::string uid,
                                  std::function<void(std::vector<MapVoteRecord>, std::string)> callback) {
    connect([uid = std::move(uid), callback = std::move(callback)](bool ok, std::string detail) mutable {
        if (!ok) {
            callback({}, std::move(detail));
            return;
        }
        request("GET", "players/" + uid + "/map-votes", {},
                [callback = std::move(callback)](web::WebResponse response) mutable {
                    auto value = response.json();
                    if (!response.ok() || !value || !value.unwrap().isObject()) {
                        callback({}, networkError(response));
                        return;
                    }
                    std::vector<MapVoteRecord> rows;
                    if (auto const& list = value.unwrap()["votes"]; list.isArray())
                        for (auto const& entry : list.asArray().unwrap()) {
                            if (!entry.isObject() || rows.size() >= 300) continue;
                            MapVoteRecord row;
                            row.levelId = intAt(entry, "levelId");
                            row.name = stringAt(entry, "name");
                            row.vote = static_cast<int>(std::clamp<int64_t>(intAt(entry, "vote"), -1, 1));
                            row.updatedAt = intAt(entry, "updatedAt");
                            if (row.levelId > 0 && row.vote != 0) rows.push_back(std::move(row));
                        }
                    callback(std::move(rows), {});
                });
    });
}
// 한 방의 Live 전투 상세를 받는다 (방이 사라졌으면 removed).
void Service::fetchLiveBattle(std::string roomId, std::function<void(LiveBattleView)> callback) {
    connect([roomId = std::move(roomId), callback = std::move(callback)](bool ok, std::string) mutable {
        if (!ok) {
            callback({});
            return;
        }
        request("GET", "live/" + roomId, {}, [callback = std::move(callback)](web::WebResponse response) mutable {
            LiveBattleView view;
            auto value = response.json();
            if (!response.ok() || !value || !value.unwrap().isObject()) {
                callback(std::move(view));
                return;
            }
            auto const& root = value.unwrap();
            view.ok = true;
            if (boolAt(root, "removed")) {
                view.removed = true;
                callback(std::move(view));
                return;
            }
            view.match = parseLiveMatch(root);
            view.finishedAt = intAt(root, "finishedAt");
            view.winnerUid = stringAt(root, "winnerUid");
            view.draw = boolAt(root, "draw");
            callback(std::move(view));
        });
    });
}
// Live 목록을 받는다. 1v1 매치 최대 60개, Group 전투 최대 30개이며
// Group 항목에는 상위 2명과 참가자 이름 목록(최대 80)이 포함된다.
void Service::fetchLiveMatches(std::function<void(LiveList, std::string)> callback) {
    connect([callback = std::move(callback)](bool ok, std::string detail) mutable {
        if (!ok) {
            callback({}, std::move(detail));
            return;
        }
        request("GET", "live", {}, [callback = std::move(callback)](web::WebResponse response) mutable {
            auto value = response.json();
            if (!response.ok() || !value || !value.unwrap().isObject()) {
                callback({}, networkError(response));
                return;
            }
            auto const& root = value.unwrap();
            LiveList list;
            auto& matches = list.matches;
            if (root["matches"].isArray())
                for (auto const& entry : root["matches"].asArray().unwrap()) {
                    if (!entry.isObject() || matches.size() >= 60) continue;
                    matches.push_back(parseLiveMatch(entry));
                }
            if (root["groups"].isArray())
                for (auto const& entry : root["groups"].asArray().unwrap()) {
                    if (!entry.isObject() || list.groups.size() >= 30) continue;
                    LiveGroup group;
                    group.roomId = stringAt(entry, "roomId");
                    group.name = stringAt(entry, "name");
                    group.battleId = stringAt(entry, "battleId");
                    group.avgRank = static_cast<int>(std::clamp<int64_t>(intAt(entry, "avgRank"), 0, 100'000'000));
                    group.level = parseLevel(entry["level"]);
                    group.rules = parseRules(entry["rules"]);
                    group.releasedAt = intAt(entry, "releasedAt");
                    group.count = static_cast<int>(intAt(entry, "count"));
                    group.alive = static_cast<int>(intAt(entry, "alive"));
                    if (entry["top"].isArray())
                        for (auto const& player : entry["top"].asArray().unwrap()) {
                            if (!player.isObject() || group.top.size() >= 2) continue;
                            LivePlayer slot;
                            slot.profile = parseProfile(player["profile"]);
                            slot.state = parseBattlePlayer(player["state"]);
                            group.top.push_back(std::move(slot));
                        }
                    if (entry["names"].isArray())
                        for (auto const& name : entry["names"].asArray().unwrap())
                            if (group.names.size() < 80) group.names.push_back(name.asString().unwrapOr(""));
                    if (!group.roomId.empty()) list.groups.push_back(std::move(group));
                }
            list.count = static_cast<int>(intAt(root, "count"));
            callback(std::move(list), {});
        });
    });
}
// Ranked 리더보드 (최대 100명).
void Service::fetchRankedLeaderboard(std::function<void(std::vector<RankedProfile>, std::string)> callback) {
    connect([callback = std::move(callback)](bool ok, std::string detail) mutable {
        if (!ok) {
            callback({}, std::move(detail));
            return;
        }
        request("GET", rankedPath("leaderboard"), {},
                [callback = std::move(callback)](web::WebResponse response) mutable {
                    auto value = response.json();
                    if (!response.ok() || !value || !value.unwrap().isArray()) {
                        callback({}, networkError(response));
                        return;
                    }
                    std::vector<RankedProfile> rows;
                    for (auto const& row : value.unwrap().asArray().unwrap()) {
                        if (rows.size() >= 100) break;
                        rows.push_back(parseRankedProfile(row));
                    }
                    callback(std::move(rows), {});
                });
    });
}
// 내 티어보다 한 단계 위 티어 중 레이팅 기준이 있는 첫 티어와, 거기까지 필요한 점수를 계산한다.
std::optional<NextRankedTier> nextRankedTier(RankedTiers const& tiers, std::string const& tier, double rating) {
    auto const mine = std::find_if(tiers.tiers.begin(), tiers.tiers.end(),
                                   [&](RankedTierCutoff const& row) { return row.tier == tier; });
    if (mine == tiers.tiers.end()) return std::nullopt;
    for (auto it = mine; it != tiers.tiers.begin();) {
        --it;
        if (!it->minRating) continue;
        int const shown = static_cast<int>(std::lround(rating));
        return NextRankedTier{it->tier, *it->minRating, std::max(1, *it->minRating - shown + 1)};
    }
    return std::nullopt;
}
// 티어 구간 표 조회. 30초 동안 캐시하며 fresh면 항상 새로 받는다.
void Service::fetchRankedTiers(std::function<void(std::optional<RankedTiers>, std::string)> callback, bool fresh) {
    static std::optional<RankedTiers> cached;
    static int64_t cachedAt = 0;
    if (!fresh && cached && systemNowMs() - cachedAt < 30'000) {
        callback(cached, {});
        return;
    }
    connect([callback = std::move(callback)](bool ok, std::string detail) mutable {
        if (!ok) {
            callback(std::nullopt, std::move(detail));
            return;
        }
        request("GET", "ranked/tiers", {}, [callback = std::move(callback)](web::WebResponse response) mutable {
            auto value = response.json();
            if (!response.ok() || !value || !value.unwrap().isObject()) {
                callback(std::nullopt, networkError(response));
                return;
            }
            auto const& root = value.unwrap();
            RankedTiers result;
            result.population = static_cast<int>(intAt(root, "population"));
            if (root["tiers"].isArray())
                for (auto const& row : root["tiers"].asArray().unwrap()) {
                    if (result.tiers.size() >= 16 || !row.isObject()) break;
                    RankedTierCutoff tier;
                    tier.tier = tierAt(row, "tier");
                    tier.topPercent = static_cast<int>(intAt(row, "topPercent"));
                    if (row["minRating"].isNumber()) tier.minRating = static_cast<int>(intAt(row, "minRating"));
                    if (row["maxRating"].isNumber()) tier.maxRating = static_cast<int>(intAt(row, "maxRating"));
                    if (!tier.tier.empty()) result.tiers.push_back(std::move(tier));
                }
            cached = result;
            cachedAt = systemNowMs();
            callback(std::move(result), {});
        });
    });
}
// 이름으로 Ranked 플레이어 검색 (최대 30명).
void Service::searchRankedPlayers(std::string query,
                                  std::function<void(std::vector<RankedProfile>, std::string)> callback) {
    connect([query = std::move(query), callback = std::move(callback)](bool ok, std::string detail) mutable {
        if (!ok) {
            callback({}, std::move(detail));
            return;
        }
        auto body = Json::object();
        body["query"] = std::move(query);
        request("PUT", rankedPath("search"), std::move(body),
                [callback = std::move(callback)](web::WebResponse response) mutable {
                    auto value = response.json();
                    if (!response.ok() || !value || !value.unwrap().isArray()) {
                        callback({}, networkError(response));
                        return;
                    }
                    std::vector<RankedProfile> rows;
                    for (auto const& row : value.unwrap().asArray().unwrap()) {
                        if (rows.size() >= 30) break;
                        rows.push_back(parseRankedProfile(row));
                    }
                    callback(std::move(rows), {});
                });
    });
}
// Ranked 프로필 조회. 컵이 비어 있으면 Season 1, 있으면 해당 Season 2 컵의 프로필이다.
void Service::fetchRankedProfile(std::string uid,
                                 std::function<void(std::optional<RankedProfile>, std::string)> callback) {
    fetchRankedProfileIn(state().rankedCup, std::move(uid), std::move(callback));
}
void Service::fetchRankedProfileIn(std::string cup, std::string uid,
                                   std::function<void(std::optional<RankedProfile>, std::string)> callback) {
    connect([cup = std::move(cup), uid = std::move(uid), callback = std::move(callback)](bool ok,
                                                                                         std::string detail) mutable {
        if (!ok) {
            callback(std::nullopt, std::move(detail));
            return;
        }
        if (uid.empty()) uid = state().profile.uid;
        request("GET", (cup.empty() ? "ranked/profile/" : "ranked2/" + cup + "/profile/") + uid, {},
                [callback = std::move(callback)](web::WebResponse response) mutable {
                    auto value = response.json();
                    if (!response.ok() || !value || !value.unwrap().isObject()) {
                        callback(std::nullopt, networkError(response));
                        return;
                    }
                    callback(parseRankedProfile(value.unwrap()), {});
                });
    });
}
// 업적 달성률 캐시. 10분 동안 재사용하고 조회 중에는 요청을 합쳐 모든 대기자에게 한 번에 응답한다.
namespace {
struct AchievementRatesCache {
    std::optional<AchievementRates> value;
    std::chrono::steady_clock::time_point at{};
    bool loading = false;
    std::vector<std::function<void(std::optional<AchievementRates>)>> waiters;
};
AchievementRatesCache& achievementRatesCache() {
    static auto* value = new AchievementRatesCache;
    return *value;
}
void answerAchievementRates(std::optional<AchievementRates> fresh) {
    auto& cache = achievementRatesCache();
    cache.loading = false;
    if (fresh) {
        cache.value = std::move(fresh);
        cache.at = std::chrono::steady_clock::now();
    }
    auto waiters = std::move(cache.waiters);
    cache.waiters.clear();
    for (auto& waiter : waiters) waiter(cache.value);
}
}

void Service::fetchAchievementRates(std::function<void(std::optional<AchievementRates>)> callback) {
    auto& cache = achievementRatesCache();
    if (cache.value && std::chrono::steady_clock::now() - cache.at < std::chrono::minutes(10)) {
        callback(cache.value);
        return;
    }
    cache.waiters.push_back(std::move(callback));
    if (cache.loading) return;
    cache.loading = true;
    connect([](bool ok, std::string) {
        if (!ok) {
            answerAchievementRates(std::nullopt);
            return;
        }
        request("GET", "achievement-rates", {}, [](web::WebResponse response) {
            auto parsed = response.json();
            if (!response.ok() || !parsed || !parsed.unwrap().isObject()) {
                answerAchievementRates(std::nullopt);
                return;
            }
            auto const& value = parsed.unwrap();
            AchievementRates rates;
            rates.players = static_cast<int>(std::clamp<int64_t>(intAt(value, "players"), 0, 1'000'000'000));
            if (value["unlocked"].isObject())
                for (auto const& entry : value["unlocked"]) {
                    auto const key = entry.getKey();
                    if (!key || key->empty() || key->size() > 64) continue;
                    rates.unlocked[*key] =
                        static_cast<int>(std::clamp<int64_t>(entry.asInt().unwrapOr(0), 0, 1'000'000'000));
                }
            if (rates.players <= 0) {
                answerAchievementRates(std::nullopt);
                return;
            }
            answerAchievementRates(std::move(rates));
        });
    });
}

// 플레이어의 업적 진행과 보유 칭호를 받는다. 서버의 업적 id를 칭호 표(kTitles)와 맞춰 항목을 채운다.
void Service::fetchAchievements(std::string uid,
                                std::function<void(std::optional<AchievementState>, std::string)> callback) {
    connect([uid = std::move(uid), callback = std::move(callback)](bool ok, std::string detail) mutable {
        if (!ok) {
            callback(std::nullopt, std::move(detail));
            return;
        }
        if (uid.empty()) uid = state().profile.uid;
        request(
            "GET", "achievements/" + uid, {}, [uid, callback = std::move(callback)](web::WebResponse response) mutable {
                auto parsed = response.json();
                if (!response.ok() || !parsed || !parsed.unwrap().isObject()) {
                    callback(std::nullopt, networkError(response));
                    return;
                }
                auto const& value = parsed.unwrap();
                AchievementState result;
                result.uid = uid;
                if (value["achievements"].isArray())
                    for (auto const& entry : value["achievements"].asArray().unwrap()) {
                        auto const id = stringAt(entry, "id");
                        for (size_t i = 1; i < kTitles.size(); ++i)
                            if (!kTitles[i].achievement.empty() && kTitles[i].achievement == id) {
                                int const index = achievementOfTitle(static_cast<int>(i));
                                if (index < 0 || index >= kAllAchievementCount) continue;
                                if (index >= kAchievementCount) result.season2 = true;
                                auto& slot = result.entries[index];
                                slot.goal = std::max(1, static_cast<int>(intAt(entry, "goal")));
                                slot.progress = std::clamp(static_cast<int>(intAt(entry, "progress")), 0, slot.goal);
                                slot.unlockedAt = intAt(entry, "unlockedAt");
                            }
                    }
                result.titles = {0};
                if (value["titles"].isArray())
                    for (auto const& title : value["titles"].asArray().unwrap()) {
                        int const index =
                            static_cast<int>(std::clamp<int64_t>(title.asInt().unwrapOr(-1), -1, 2'000'000));
                        if (index > 0 && index < kTitleCount)
                            result.titles.push_back(index);
                        else if (isS1FinalTitle(index)) {
                            result.titles.push_back(index);
                            result.s1Final = index;
                        }
                    }
                if (uid == state().profile.uid && debug::creatorAccount() && !result.ownsTitle(kDeveloperTitle))
                    result.titles.push_back(kDeveloperTitle);
                result.unlocked =
                    static_cast<int>(std::count_if(result.titles.begin(), result.titles.end(),
                                                   [](int title) { return achievementOfTitle(title) >= 0; }));
                result.games = static_cast<int>(intAt(value["stats"], "games"));
                result.wins = static_cast<int>(intAt(value["stats"], "wins"));
                callback(std::move(result), {});
            });
    });
}
// 이모트 / 빠른 채팅 전송 (sendRoomSignal로 공통 처리).
void Service::sendEmote(std::string kind, Done callback) {
    sendRoomSignal(std::move(kind), {}, std::move(callback));
}
void Service::sendChat(std::string text, Done callback) {
    sendRoomSignal("chat", std::move(text), std::move(callback));
}
// Ranked 대기열 참여. 다른 일반 방에 있으면 거부하고, 최신 프로필을 함께 보낸다.
void Service::joinRankedQueue(RankedStateCallback callback) {
    auto& s = state();
    if (s.room && !s.room->id.starts_with("ranked-")) {
        callback(std::nullopt, "Leave your current room before entering Ranked Match.");
        return;
    }
    refreshLocalProfile();
    auto body = Json::object();
    body["profile"] = profileJson(s.profile);
    rankedRequest("PUT", rankedPath("queue"), std::move(body), std::move(callback));
}
// Ranked 듀얼(친선 도전) 관련 요청들. Season 2에서는 컵 경로(rankedPath)와 전용 엔드포인트를 쓴다.
void Service::challengeRanked(std::string targetUid, bool anonymous, RankedStateCallback callback) {
    if (!state().rankedCup.empty()) {
        callback(std::nullopt, "Duels are not part of Season 2 yet.");
        return;
    }
    refreshLocalProfile();
    auto body = Json::object();
    body["target"] = std::move(targetUid);
    body["anonymous"] = anonymous;
    body["profile"] = profileJson(state().profile);
    rankedRequest("PUT", "ranked/challenge", std::move(body), std::move(callback));
}
void Service::respondDuel(std::string requestId, bool accept, RankedStateCallback callback) {
    auto body = Json::object();
    body["id"] = std::move(requestId);
    body["accept"] = accept;
    if (!state().rankedCup.empty()) {
        if (accept) {
            refreshLocalProfile();
            body["profile"] = profileJson(state().profile);
        }
        rankedRequest("PUT", rankedPath("duel/respond"), std::move(body), std::move(callback));
        return;
    }
    rankedRequest("PUT", "ranked/duel/respond", std::move(body), std::move(callback));
}
void Service::cancelDuel(std::string requestId, RankedStateCallback callback) {
    auto body = Json::object();
    body["id"] = std::move(requestId);
    rankedRequest("PUT", state().rankedCup.empty() ? std::string("ranked/duel/cancel") : rankedPath("duel/cancel"),
                  std::move(body), std::move(callback));
}
void Service::challengeSeason2(std::string targetUid, std::string cup, std::string tierName,
                               RankedStateCallback callback) {
    if (state().rankedCup.empty()) {
        callback(std::nullopt, "Open a Season 2 cup first.");
        return;
    }
    refreshLocalProfile();
    auto body = Json::object();
    body["target"] = std::move(targetUid);
    body["cup"] = std::move(cup);
    body["tier"] = std::move(tierName);
    body["profile"] = profileJson(state().profile);
    rankedRequest("PUT", rankedPath("duel/request"), std::move(body), std::move(callback));
}
void Service::setRankedDuels(bool enabled, RankedStateCallback callback) {
    if (!state().rankedCup.empty()) {
        callback(std::nullopt, "Duels are not part of Season 2 yet.");
        return;
    }
    auto body = Json::object();
    body["enabled"] = enabled;
    rankedRequest("PUT", "ranked/duels", std::move(body), std::move(callback));
}
void Service::sendRankedLobbyEmote(std::string kind, RankedStateCallback callback) {
    auto body = Json::object();
    body["kind"] = std::move(kind);
    rankedRequest("PUT", rankedPath("emote"), std::move(body), std::move(callback));
}
// 제재 / 이의 제기(appeal) 응답 파싱 도우미.
BanAppeal parseAppeal(Json const& value) {
    BanAppeal appeal;
    appeal.id = intAt(value, "id");
    appeal.createdAt = intAt(value, "createdAt");
    appeal.answeredAt = intAt(value, "answeredAt");
    appeal.state = stringAt(value, "state");
    appeal.text = stringAt(value, "text");
    appeal.answer = stringAt(value, "answer");
    appeal.uid = stringAt(value, "uid");
    appeal.name = stringAt(value, "name");
    appeal.banReason = stringAt(value, "banReason");
    appeal.answeredBy = stringAt(value, "answeredBy");
    appeal.accountId = static_cast<int>(intAt(value, "accountId"));
    appeal.banUntil = intAt(value, "banUntil");
    appeal.banPermanent = boolAt(value, "banPermanent");
    appeal.banActive = boolAt(value, "banActive");
    return appeal;
}
AppealStatus parseAppealStatus(Json const& value) {
    AppealStatus status;
    if (value["latest"].isObject()) status.latest = parseAppeal(value["latest"]);
    status.left = static_cast<int>(intAt(value, "left"));
    status.nextAt = intAt(value, "nextAt");
    status.canAppeal = boolAt(value, "canAppeal");
    return status;
}
BanInfo parseBan(Json const& value) {
    BanInfo ban;
    ban.uid = stringAt(value, "uid");
    ban.name = stringAt(value, "name");
    ban.reason = stringAt(value, "reason");
    ban.accountId = static_cast<int>(intAt(value, "accountId"));
    ban.permanent = boolAt(value, "permanent");
    ban.until = intAt(value, "until");
    ban.createdAt = intAt(value, "createdAt");
    ban.remainingMs = intAt(value, "remainingMs");
    return ban;
}
// 연결을 보장한 뒤 JSON 객체 응답을 돌려주는 요청 (제재, 스태프 도구가 공통으로 사용).
void moderationRequest(std::string method, std::string path, Json body,
                       std::function<void(std::optional<Json>, std::string)> done) {
    Service::get().connect([method = std::move(method), path = std::move(path), body = std::move(body),
                            done = std::move(done)](bool ok, std::string detail) mutable {
        if (!ok) {
            done(std::nullopt, std::move(detail));
            return;
        }
        request(std::move(method), std::move(path), std::move(body),
                [done = std::move(done)](web::WebResponse response) {
                    auto parsed = response.json();
                    if (!response.ok() || !parsed || !parsed.unwrap().isObject()) {
                        done(std::nullopt, networkError(response));
                        return;
                    }
                    done(parsed.unwrap(), {});
                });
    });
}
// 내 제재 상태를 조회한다 (계정 id 기준). 제재 / 신고 제재 / 이의 제기 상태를 상태에 저장한다.
void Service::fetchOwnBan(std::function<void(bool, std::optional<BanInfo>, std::string)> callback) {
    auto body = Json::object();
    body["accountId"] = state().profile.accountId;
    moderationRequest(
        "PUT", "ban", std::move(body), [callback = std::move(callback)](std::optional<Json> value, std::string error) {
            if (!value) {
                callback(false, std::nullopt, std::move(error));
                return;
            }
            auto& s = state();
            s.ownBan = boolAt(*value, "banned") && (*value)["ban"].isObject() ? std::optional(parseBan((*value)["ban"]))
                                                                              : std::nullopt;
            s.ownReportBan = boolAt(*value, "reportBanned") && (*value)["reportBan"].isObject()
                                 ? std::optional(parseBan((*value)["reportBan"]))
                                 : std::nullopt;
            s.ownAppeal = s.ownBan && (*value)["appeal"].isObject()
                              ? std::optional(parseAppealStatus((*value)["appeal"]))
                              : std::nullopt;
            callback(true, s.ownBan, {});
        });
}
std::optional<BanInfo> const& Service::ownBan() const {
    return state().ownBan;
}
std::optional<AppealStatus> const& Service::ownAppeal() const {
    return state().ownAppeal;
}
void Service::sendBanAppeal(std::string text, std::function<void(bool, std::string)> done) {
    auto body = Json::object();
    body["accountId"] = state().profile.accountId;
    body["name"] = state().profile.name.substr(0, 32);
    body["text"] = std::move(text);
    moderationRequest(
        "PUT", "ban/appeal", std::move(body), [done = std::move(done)](std::optional<Json> value, std::string error) {
            if (!value) {
                done(false, error.empty() ? std::string("The appeal could not be sent.") : std::move(error));
                return;
            }
            if ((*value)["appeal"].isObject()) state().ownAppeal = parseAppealStatus((*value)["appeal"]);
            done(true, {});
        });
}
// 이의 제기 목록 응답을 최대 300건까지 변환해 콜백에 넘긴다 (open은 처리 대기 수).
void answerAppeals(Service::AppealsCallback const& callback, std::optional<Json> const& value, std::string error) {
    std::vector<BanAppeal> appeals;
    if (!value) {
        callback(std::move(appeals), 0, error.empty() ? std::string("The appeal box could not be loaded.") : error);
        return;
    }
    if ((*value)["appeals"].isArray())
        for (auto const& entry : (*value)["appeals"].asArray().unwrap())
            if (entry.isObject() && appeals.size() < 300) appeals.push_back(parseAppeal(entry));
    callback(std::move(appeals), static_cast<int>(intAt(*value, "open")), {});
}
void Service::fetchBanAppeals(AppealsCallback callback) {
    moderationRequest("GET", "admin/appeals", {},
                      [callback = std::move(callback)](std::optional<Json> value, std::string error) {
                          answerAppeals(callback, value, std::move(error));
                      });
}
void Service::answerBanAppeal(int64_t id, bool accept, std::string answer, AppealsCallback callback) {
    auto body = Json::object();
    body["action"] = accept ? "accept" : "reject";
    body["answer"] = std::move(answer);
    moderationRequest("PUT", fmt::format("admin/appeals/{}", id), std::move(body),
                      [callback = std::move(callback)](std::optional<Json> value, std::string error) {
                          answerAppeals(callback, value, std::move(error));
                      });
}
std::optional<BanInfo> const& Service::ownReportBan() const {
    return state().ownReportBan;
}
// 관리자 상태 조회. 권한 여부, 처리 대기 수, 역할을 저장한다 (역할이 비면 owner로 간주).
void Service::fetchAdminStatus(std::function<void(AdminStatus, std::string)> callback) {
    moderationRequest(
        "GET", "admin/status", {}, [callback = std::move(callback)](std::optional<Json> value, std::string error) {
            AdminStatus status;
            if (value) {
                status.admin = boolAt(*value, "admin");
                status.reports = static_cast<int>(intAt(*value, "reports"));
                status.bans = static_cast<int>(intAt(*value, "bans"));
                status.reportBans = static_cast<int>(intAt(*value, "reportBans"));
                status.appeals = static_cast<int>(intAt(*value, "appeals"));
                status.role = stringAt(*value, "role");
                status.inbox = static_cast<int>(intAt(*value, "inbox"));
                status.replies = static_cast<int>(intAt(*value, "replies"));
                status.season2 = boolAt(*value, "season2");
                state().season2Access = status.season2;
                state().admin = status.admin;
                state().staffRole =
                    status.admin ? (status.role.empty() ? std::string("owner") : status.role) : std::string();
            }
            callback(status, std::move(error));
        });
}
bool Service::isAdmin() const {
    return state().admin;
}
// Season 2 컵 선택 변경. 컵이 바뀌면 이전 컵의 Ranked 상태 캐시와 대기자 목록을 모두 버리고
// 이전 요청의 응답이 반영되지 않게 serial을 올린다.
void Service::setRankedCup(std::string cup) {
    auto& s = state();
    if (s.rankedCup == cup) return;
    s.rankedCup = std::move(cup);
    s.lastRankedState.reset();
    s.rankedStateRaw.reset();
    s.rankedStateEtag.clear();
    s.rosterEntries.clear();
    s.rosterTag.clear();
    s.rankedAppliedSerial = ++s.rankedRequestSerial;
}
std::string const& Service::rankedCup() const {
    return state().rankedCup;
}
// Season 2 접근 권한: 서버가 허용했거나 현재 시즌이 2 이상.
bool Service::season2Access() const {
    return state().season2Access || state().season >= 2;
}
// Season 2 컵 목록 (최대 4개)과 내 프로필 / 접속자 수.
void Service::fetchSeason2Home(std::function<void(std::vector<Season2Cup>, std::string)> callback) {
    connect([callback = std::move(callback)](bool ok, std::string detail) mutable {
        if (!ok) {
            callback({}, std::move(detail));
            return;
        }
        request("GET", "ranked2/home", {}, [callback = std::move(callback)](web::WebResponse response) mutable {
            auto value = response.json();
            if (!response.ok() || !value || !value.unwrap()["cups"].isArray()) {
                callback({}, networkError(response));
                return;
            }
            std::vector<Season2Cup> cups;
            for (auto const& row : value.unwrap()["cups"].asArray().unwrap()) {
                if (!row.isObject() || cups.size() >= 4) continue;
                cups.push_back({stringAt(row, "id"), stringAt(row, "name"), parseRankedProfile(row["profile"]),
                                static_cast<int>(intAt(row, "online", -1)),
                                static_cast<int>(std::max<int64_t>(0, intAt(row, "mapBans")))});
            }
            callback(std::move(cups), {});
        });
    });
}
namespace {
// Season 2 티어 한 줄 파싱 (레이팅 구간, 순위 구간, 맵 조건 mask / 길이 / 기능 / GDDL 범위, 목숨).
Season2TierRow parseSeason2Tier(Json const& row) {
    Season2TierRow tier;
    tier.tier = tierAt(row, "tier", "Unranked");
    tier.name = tierAt(row, "tierName", tier.tier);
    tier.minRating = static_cast<int>(intAt(row, "minRating"));
    tier.maxRating = static_cast<int>(intAt(row, "maxRating"));
    tier.topRank = static_cast<int>(intAt(row, "topRank"));
    tier.topPercent = row["topPercent"].asDouble().unwrapOr(0.);
    auto const& map = row["map"].isObject() ? row["map"] : row;
    tier.mask = static_cast<int>(intAt(map, "mask"));
    tier.lives = static_cast<int>(intAt(map, "lives"));
    if (map["lengths"].isArray())
        for (auto const& length : map["lengths"].asArray().unwrap())
            tier.lengths.push_back(static_cast<int>(length.asInt().unwrapOr(0)));
    if (map["features"].isArray())
        for (auto const& feature : map["features"].asArray().unwrap())
            tier.features.push_back(static_cast<int>(feature.asInt().unwrapOr(0)));
    if (map["gddl"].isArray()) {
        auto const& range = map["gddl"].asArray().unwrap();
        if (range.size() == 2) {
            tier.gddlLow = static_cast<int>(range.front().asInt().unwrapOr(-1));
            tier.gddlHigh = static_cast<int>(range.back().asInt().unwrapOr(-1));
        }
    }
    return tier;
}
}
// Season 2 컵의 티어 표와 점수 규칙 표(points)를 받는다. 목록 길이와 값 범위를 제한해 읽고,
// 점수 표는 행마다 win/loss 길이가 diffs와 맞을 때만 사용한다.
void Service::fetchSeason2Tiers(std::string cup,
                                std::function<void(std::optional<Season2Tiers>, std::string)> callback) {
    connect([cup = std::move(cup), callback = std::move(callback)](bool ok, std::string detail) mutable {
        if (!ok) {
            callback(std::nullopt, std::move(detail));
            return;
        }
        request("GET", "ranked2/" + cup + "/tiers", {},
                [callback = std::move(callback)](web::WebResponse response) mutable {
                    auto value = response.json();
                    if (!response.ok() || !value || !value.unwrap().isObject()) {
                        callback(std::nullopt, networkError(response));
                        return;
                    }
                    auto const& root = value.unwrap();
                    Season2Tiers tiers;
                    tiers.cup = stringAt(root, "cup");
                    tiers.cupName = stringAt(root, "cupName");
                    tiers.population = static_cast<int>(intAt(root, "population"));
                    tiers.placementGames = static_cast<int>(intAt(root, "placementGames", 5));
                    tiers.ratingMin = static_cast<int>(intAt(root, "ratingMin"));
                    tiers.ratingMax = static_cast<int>(intAt(root, "ratingMax", 20000));
                    if (root["livesByLength"].isArray())
                        for (auto const& value : root["livesByLength"].asArray().unwrap())
                            if (tiers.livesByLength.size() < 5)
                                tiers.livesByLength.push_back(static_cast<int>(value.asInt().unwrapOr(100)));
                    if (root["placement"].isObject()) {
                        tiers.placement = parseSeason2Tier(root["placement"]);
                        tiers.placement.tier = "Unranked";
                        tiers.placement.name = "Placement";
                    }
                    if (root["tiers"].isArray())
                        for (auto const& row : root["tiers"].asArray().unwrap())
                            if (row.isObject() && tiers.tiers.size() < 30) tiers.tiers.push_back(parseSeason2Tier(row));
                    if (auto const& points = root["points"];
                        points.isObject() && points["diffs"].isArray() && points["rows"].isArray()) {
                        Season2Points table;
                        auto ints = [](Json const& list, size_t limit) {
                            std::vector<int> out;
                            if (list.isArray())
                                for (auto const& value : list.asArray().unwrap())
                                    if (out.size() < limit)
                                        out.push_back(static_cast<int>(
                                            std::clamp<int64_t>(value.asInt().unwrapOr(0), -1'000'000, 1'000'000)));
                            return out;
                        };
                        table.diffs = ints(points["diffs"], 9);
                        for (auto const& row : points["rows"].asArray().unwrap()) {
                            if (!row.isObject() || table.rows.size() >= 12) continue;
                            Season2PointsRow entry;
                            entry.from = static_cast<int>(intAt(row, "from"));
                            entry.below = row["below"].isNumber() ? static_cast<int>(intAt(row, "below")) : -1;
                            entry.k = static_cast<int>(intAt(row, "k"));
                            entry.win = ints(row["win"], 9);
                            entry.loss = ints(row["loss"], 9);
                            if (entry.win.size() == table.diffs.size() && entry.loss.size() == table.diffs.size())
                                table.rows.push_back(std::move(entry));
                        }
                        table.placementGames = static_cast<int>(intAt(points, "placementGames"));
                        table.placementK = static_cast<int>(intAt(points, "placementK"));
                        table.newPlayerMatches = static_cast<int>(intAt(points, "newPlayerMatches"));
                        table.newPlayerK = static_cast<int>(intAt(points, "newPlayerK"));
                        if (points["streak"].isArray())
                            for (auto const& pair : points["streak"].asArray().unwrap()) {
                                auto values = ints(pair, 2);
                                if (values.size() == 2 && table.streak.size() < 8)
                                    table.streak.emplace_back(values[0], values[1]);
                            }
                        table.mapLossMax = points["mapLossMax"].asDouble().unwrapOr(0.);
                        table.upsetLossScale = points["upsetLossScale"].asDouble().unwrapOr(1.);
                        if (!table.diffs.empty() && !table.rows.empty()) tiers.points = std::move(table);
                    }
                    callback(std::move(tiers), {});
                });
    });
}
// Plaza(라운지) 동기화. 내 이동 샘플(x, y, 회전, 방향)과 이모트/채팅, 필요하면 프로필을 보내고
// 다른 걷는 플레이어들의 변경분을 받는다. 서버가 알려 주는 전송 주기(every)와 샘플 수(max)는
// 범위를 제한해 쓴다. 컵을 벗어났거나 컵이 바뀌면 응답을 버린다.
void Service::exchangePlaza(PlazaUpdate update, std::function<void(std::optional<PlazaAnswer>, std::string)> callback) {
    auto& s = state();
    if (s.rankedCup.empty() || s.token.empty()) {
        callback(std::nullopt, {});
        return;
    }
    auto body = Json::object();
    body["since"] = update.since;
    if (!update.samples.empty()) {
        body["k"] = update.first;
        auto list = Json::array();
        for (auto const& sample : update.samples) {
            auto row = Json::array();
            row.push(sample.x);
            row.push(sample.y);
            row.push(sample.rotation);
            row.push(sample.left ? 1 : 0);
            list.push(std::move(row));
        }
        body["s"] = std::move(list);
    }
    if (!update.eventKind.empty()) {
        auto event = Json::object();
        event["kind"] = update.eventKind;
        if (update.eventKind == "chat") event["text"] = update.eventText;
        body["e"] = std::move(event);
    }
    if (update.withProfile) {
        refreshLocalProfile();
        body["profile"] = profileJson(s.profile);
    }
    auto const cup = s.rankedCup;
    request(
        "PUT", rankedPath("plaza"), std::move(body),
        [cup, callback = std::move(callback)](web::WebResponse response) mutable {
            auto value = response.json();
            if (state().rankedCup != cup) {
                callback(std::nullopt, {});
                return;
            }
            if (!response.ok() || !value || !value.unwrap().isObject()) {
                callback(std::nullopt, networkError(response));
                return;
            }
            auto const& root = value.unwrap();
            PlazaAnswer answer;
            answer.epoch = intAt(root, "epoch");
            answer.tick = std::max<int64_t>(0, intAt(root, "tick"));
            answer.everyMs = static_cast<int>(std::clamp<int64_t>(intAt(root, "every", 100), 100, 5'000));
            answer.maxSamples = static_cast<int>(std::clamp<int64_t>(intAt(root, "max", 10), 1, 40));
            answer.needProfile = boolAt(root, "need");
            if (root["ids"].isArray())
                for (auto const& id : root["ids"].asArray().unwrap())
                    if (answer.ids.size() < 64) answer.ids.push_back(static_cast<int>(id.asInt().unwrapOr(0)));
            if (root["p"].isArray())
                for (auto const& row : root["p"].asArray().unwrap()) {
                    if (!row.isObject() || answer.changes.size() >= 64) continue;
                    PlazaWalker walker;
                    walker.id = static_cast<int>(intAt(row, "id"));
                    if (walker.id <= 0) continue;
                    if (row["i"].isObject()) walker.info = parseRankedProfile(row["i"]);
                    if (row["s"].isArray()) {
                        walker.first = intAt(row, "k", -1);
                        for (auto const& sample : row["s"].asArray().unwrap()) {
                            if (!sample.isArray() || walker.samples.size() >= 40) continue;
                            auto const& values = sample.asArray().unwrap();
                            if (values.size() != 4) continue;
                            walker.samples.push_back({static_cast<int>(values[0].asInt().unwrapOr(0)),
                                                      static_cast<int>(values[1].asInt().unwrapOr(0)),
                                                      static_cast<int>(values[2].asInt().unwrapOr(0)),
                                                      values[3].asInt().unwrapOr(0) != 0});
                        }
                        if (walker.first < 0) walker.samples.clear();
                    }
                    if (row["e"].isArray()) {
                        auto const& event = row["e"].asArray().unwrap();
                        if (event.size() == 3) {
                            walker.eventNonce = event[0].asInt().unwrapOr(0);
                            walker.eventKind = event[1].asString().unwrapOr("");
                            walker.eventText = event[2].asString().unwrapOr("");
                        }
                    }
                    answer.changes.push_back(std::move(walker));
                }
            callback(std::move(answer), {});
        },
        {}, false, false, false, {}, {}, {}, 4);
}
// Live 관전 데이터 교환. 내 쪽에서는 send(리플레이 조각)를 올리고, 관전 중이면 watch로
// 지정한 run / 위치부터의 조각(parts)을 받는다. 403 / 404는 관전 거부(kLiveRefused)로 돌려준다.
// 응답의 watched 값으로 "누군가 나를 보고 있는지"(liveWatchedAt)를 갱신한다.
void Service::exchangeLive(LiveTarget const& target, std::optional<LiveSend> send, std::optional<LiveWatch> watch,
                           std::function<void(std::optional<LiveAnswer>, std::string)> callback) {
    auto& s = state();
    if (s.token.empty() || target.empty()) {
        callback(std::nullopt, {});
        return;
    }
    auto body = Json::object();
    body["room"] = target.roomId;
    body["battle"] = target.battleId;
    if (target.group) body["group"] = true;
    if (send) {
        auto part = Json::object();
        part["run"] = send->run;
        part["index"] = send->index;
        part["data"] = std::move(send->data);
        body["send"] = std::move(part);
    }
    if (watch) {
        auto wanted = Json::object();
        wanted["run"] = watch->run;
        wanted["from"] = watch->from;
        if (!target.of.empty()) wanted["of"] = target.of;
        body["watch"] = std::move(wanted);
    }
    request(
        "PUT", "spectate", std::move(body),
        [callback = std::move(callback)](web::WebResponse response) mutable {
            auto value = response.json();
            if (!response.ok() || !value || !value.unwrap().isObject()) {
                callback(std::nullopt, response.code() == 403 || response.code() == 404 ? std::string(kLiveRefused)
                                                                                        : networkError(response));
                return;
            }
            auto const& root = value.unwrap();
            LiveAnswer answer;
            answer.run = static_cast<int>(std::clamp<int64_t>(intAt(root, "run"), 0, 1'000'000));
            answer.next = static_cast<int>(std::clamp<int64_t>(intAt(root, "next"), 0, 1'000'000));
            answer.watchedMe = root["watched"].asBool().unwrapOr(false);
            if (answer.watchedMe)
                state().liveWatchedAt = Clock::now();
            else if (root["watched"].isBool())
                state().liveWatchedAt = {};
            if (root["watch"].isObject()) {
                auto const& watched = root["watch"];
                answer.watched = true;
                answer.watchRun = static_cast<int>(std::clamp<int64_t>(intAt(watched, "run"), 0, 1'000'000));
                answer.watchFrom = static_cast<int>(std::clamp<int64_t>(intAt(watched, "from"), 0, 1'000'000));
                answer.watchNext = static_cast<int>(std::clamp<int64_t>(intAt(watched, "next"), 0, 1'000'000));
                answer.latest = static_cast<int>(std::clamp<int64_t>(intAt(watched, "latest"), 0, 1'000'000));
                if (watched["parts"].isArray())
                    for (auto const& part : watched["parts"].asArray().unwrap())
                        if (answer.parts.size() < 4096) answer.parts.push_back(part.asString().unwrapOr(""));
            }
            callback(std::move(answer), {});
        },
        {}, false, false, false, {}, {}, {}, 6, false, target.group ? std::string() : target.roomId);
}
// 최근 7초 안에 누군가 나를 관전한 것으로 확인됐는지.
bool Service::liveWatched() const {
    auto const at = state().liveWatchedAt;
    return at != Clock::time_point{} && Clock::now() - at < std::chrono::seconds(7);
}
void Service::forgetLiveWatched() {
    state().liveWatchedAt = {};
}
// 스태프 역할 / 목록 조회. 서버의 moderators 목록을 받아 보관하고, 내가 관리자인지
// 판단해 디버그 모드 쪽에 알린다 (5분 간격으로만 다시 받음).
std::string const& Service::staffRole() const {
    return state().staffRole;
}
bool Service::isOwner() const {
    return state().admin && state().staffRole == "owner";
}
bool Service::isModerator(std::string const& uid) const {
    return !uid.empty() && state().moderators.contains(uid);
}
bool takeStaffList(web::WebResponse const& response, bool& manager) {
    auto& s = state();
    auto parsed = response.json();
    if (!response.ok() || !parsed || !parsed.unwrap()["moderators"].isArray()) return false;
    std::unordered_set<std::string> moderators;
    for (auto const& uid : parsed.unwrap()["moderators"].asArray().unwrap())
        if (auto text = uid.asString().unwrapOr(""); !text.empty()) moderators.insert(std::move(text));
    s.moderators = std::move(moderators);
    s.staffKnown = true;
    manager = !s.profile.uid.empty() && s.moderators.contains(s.profile.uid);
    if (s.profile.accountId > 0 && s.syncedAccount == s.profile.accountId && s.syncedUid == s.profile.uid)
        debug::noteStaff(s.profile.accountId, manager);
    return true;
}
void refreshStaffList() {
    auto& s = state();
    if (s.staffFetching || !Service::get().connected() || versus::lounge::realPlayLayer()) return;
    if (s.staffFetchedAt != Clock::time_point{} && Clock::now() - s.staffFetchedAt < std::chrono::minutes(5)) return;
    s.staffFetching = true;
    s.staffFetchedAt = Clock::now();
    request("GET", "staff", {}, [](web::WebResponse response) {
        auto& s = state();
        s.staffFetching = false;
        bool manager = false;
        if (!takeStaffList(response, manager)) s.staffFetchedAt = Clock::now() - std::chrono::minutes(4);
    });
}
void Service::checkStaffMember(std::function<void(bool)> done) {
    connect([done = std::move(done)](bool ok, std::string) mutable {
        if (!ok) {
            done(false);
            return;
        }
        auto& s = state();
        s.staffFetchedAt = Clock::now();
        request("GET", "staff", {}, [done = std::move(done)](web::WebResponse response) mutable {
            bool manager = false;
            if (!takeStaffList(response, manager)) state().staffFetchedAt = Clock::now() - std::chrono::minutes(4);
            log::info("Versus: staff list for a build newer than the server: {}",
                      manager ? "a manager (debug mode)" : "not a manager");
            done(manager);
        });
    });
}
// 관리 로그 항목 파싱 (detail 안의 기간, 환급 / 취소 건수, 레이팅 변화 등).
ModLogEntry parseModLogEntry(Json const& entry) {
    ModLogEntry result;
    result.id = intAt(entry, "id");
    result.at = intAt(entry, "at");
    result.actor = stringAt(entry, "actor");
    result.actorName = stringAt(entry, "actorName");
    result.role = stringAt(entry, "role");
    result.action = stringAt(entry, "action");
    result.target = stringAt(entry, "target");
    result.targetName = stringAt(entry, "targetName");
    result.reason = stringAt(entry, "reason");
    auto const& detail = entry["detail"];
    if (detail.isObject()) {
        result.durationMs = intAt(detail, "durationMs", -1);
        result.until = intAt(detail, "until");
        result.permanent = boolAt(detail, "permanent");
        result.lifted = boolAt(detail, "lifted");
        result.reports = static_cast<int>(intAt(detail, "reports"));
        result.canceled = static_cast<int>(intAt(detail, "canceled"));
        result.refunded = static_cast<int>(intAt(detail, "refunded"));
        result.from = static_cast<int>(intAt(detail, "from"));
        result.to = static_cast<int>(intAt(detail, "to"));
        result.amount = detail["amount"].asDouble().unwrapOr(0.);
        result.before = detail["before"].asDouble().unwrapOr(0.);
        result.after = detail["after"].asDouble().unwrapOr(0.);
        result.removed = detail["removed"].asDouble().unwrapOr(0.);
        result.message = stringAt(detail, "message");
        result.levelId = intAt(detail, "levelId");
        result.creator = stringAt(detail, "creator");
        result.cup = stringAt(detail, "cup");
    }
    return result;
}
// 스태프 목록 / 메시지 / 우편함 API. 목록 응답으로 로컬 moderators 집합도 갱신한다.
void Service::fetchModLog(std::function<void(std::vector<ModLogEntry>, std::string)> callback) {
    moderationRequest("GET", "admin/log", {},
                      [callback = std::move(callback)](std::optional<Json> value, std::string error) {
                          std::vector<ModLogEntry> entries;
                          if (!value) {
                              callback(std::move(entries), std::move(error));
                              return;
                          }
                          if ((*value)["entries"].isArray())
                              for (auto const& entry : (*value)["entries"].asArray().unwrap())
                                  if (entry.isObject()) entries.push_back(parseModLogEntry(entry));
                          callback(std::move(entries), {});
                      });
}
void staffRequest(std::string method, std::string path, Json body, Service::StaffCallback callback) {
    moderationRequest(std::move(method), std::move(path), std::move(body),
                      [callback = std::move(callback)](std::optional<Json> value, std::string error) {
                          std::vector<StaffMember> staff;
                          if (!value) {
                              callback(std::move(staff),
                                       error.empty() ? std::string("Staff list unavailable.") : std::move(error));
                              return;
                          }
                          if ((*value)["staff"].isArray())
                              for (auto const& entry : (*value)["staff"].asArray().unwrap()) {
                                  if (!entry.isObject()) continue;
                                  StaffMember member;
                                  member.uid = stringAt(entry, "uid");
                                  member.name = stringAt(entry, "name");
                                  member.addedBy = stringAt(entry, "addedBy");
                                  member.addedByName = stringAt(entry, "addedByName");
                                  member.accountId = static_cast<int>(intAt(entry, "accountId"));
                                  member.addedAt = intAt(entry, "addedAt");
                                  if (!member.uid.empty()) staff.push_back(std::move(member));
                              }
                          auto& s = state();
                          s.moderators.clear();
                          for (auto const& member : staff) s.moderators.insert(member.uid);
                          callback(std::move(staff), {});
                      });
}
void Service::fetchStaff(StaffCallback callback) {
    staffRequest("GET", "admin/staff", {}, std::move(callback));
}
void Service::addStaff(std::string uid, StaffCallback callback) {
    staffRequest("PUT", "admin/staff/" + uid, Json::object(), std::move(callback));
}
void Service::removeStaff(std::string uid, StaffCallback callback) {
    staffRequest("DELETE", "admin/staff/" + uid, Json::object(), std::move(callback));
}
// 1.0.8: 컵별 맵 밴 API. 응답의 maps 배열(최대 600개), limit, staff 를 콜백에 넘긴다.
void staffMapRequest(std::string method, std::string path, Json body, Service::StaffMapBansCallback callback) {
    moderationRequest(std::move(method), std::move(path), std::move(body),
                      [callback = std::move(callback)](std::optional<Json> value, std::string error) {
                          if (!value) {
                              callback(std::nullopt, error.empty() ? std::string("The map bans could not be loaded.")
                                                                   : std::move(error));
                              return;
                          }
                          StaffMapBanList list;
                          auto& maps = list.maps;
                          if ((*value)["maps"].isArray())
                              for (auto const& entry : (*value)["maps"].asArray().unwrap()) {
                                  if (!entry.isObject() || maps.size() >= 600) continue;
                                  StaffMapBan map;
                                  map.levelId = intAt(entry, "levelId");
                                  map.at = intAt(entry, "at");
                                  map.name = stringAt(entry, "name");
                                  map.creator = stringAt(entry, "creator");
                                  map.reason = stringAt(entry, "reason");
                                  map.by = stringAt(entry, "by");
                                  map.byName = stringAt(entry, "byName");
                                  if (map.levelId > 0) maps.push_back(std::move(map));
                              }
                          list.limit = static_cast<int>(intAt(*value, "limit"));
                          list.staff = boolAt(*value, "staff");
                          callback(std::move(list), {});
                      });
}
void Service::fetchStaffMapBans(std::string cup, StaffMapBansCallback callback) {
    staffMapRequest("GET", fmt::format("ranked2/{}/map-bans", cup), {}, std::move(callback));
}
void Service::banStaffMap(std::string cup, LevelInfo const& level, std::string reason, StaffMapBansCallback callback) {
    auto body = Json::object();
    body["name"] = level.name.substr(0, 64);
    body["creator"] = level.creator.substr(0, 32);
    body["reason"] = reason.substr(0, 200);
    staffMapRequest("PUT", fmt::format("admin/maps/{}/{}", cup, level.id), std::move(body), std::move(callback));
}
void Service::unbanStaffMap(std::string cup, int64_t levelId, StaffMapBansCallback callback) {
    staffMapRequest("DELETE", fmt::format("admin/maps/{}/{}", cup, levelId), Json::object(), std::move(callback));
}
void Service::findStaffCandidates(std::string name,
                                  std::function<void(std::vector<StaffCandidate>, std::string)> callback) {
    auto body = Json::object();
    body["name"] = name.substr(0, 32);
    moderationRequest("PUT", "admin/staff/find", std::move(body),
                      [callback = std::move(callback)](std::optional<Json> value, std::string error) {
                          std::vector<StaffCandidate> players;
                          if (!value) {
                              callback(std::move(players), std::move(error));
                              return;
                          }
                          if ((*value)["players"].isArray())
                              for (auto const& entry : (*value)["players"].asArray().unwrap()) {
                                  if (!entry.isObject()) continue;
                                  StaffCandidate candidate;
                                  candidate.uid = stringAt(entry, "uid");
                                  candidate.name = stringAt(entry, "name");
                                  candidate.role = stringAt(entry, "role");
                                  candidate.accountId = static_cast<int>(intAt(entry, "accountId"));
                                  if (!candidate.uid.empty()) players.push_back(std::move(candidate));
                              }
                          callback(std::move(players), {});
                      });
}
void staffMessagesRequest(std::string method, std::string path, Json body, Service::StaffMessagesCallback callback) {
    moderationRequest(std::move(method), std::move(path), std::move(body),
                      [callback = std::move(callback)](std::optional<Json> value, std::string error) {
                          std::vector<StaffMessage> messages;
                          if (!value) {
                              callback(std::move(messages), 0,
                                       error.empty() ? std::string("Messages unavailable.") : std::move(error));
                              return;
                          }
                          if ((*value)["messages"].isArray())
                              for (auto const& entry : (*value)["messages"].asArray().unwrap()) {
                                  if (!entry.isObject()) continue;
                                  StaffMessage message;
                                  message.id = intAt(entry, "id");
                                  message.createdAt = intAt(entry, "createdAt");
                                  message.readAt = intAt(entry, "readAt");
                                  message.repliedAt = intAt(entry, "repliedAt");
                                  message.sender = stringAt(entry, "sender");
                                  message.senderName = stringAt(entry, "senderName");
                                  message.kind = stringAt(entry, "kind");
                                  message.body = stringAt(entry, "body");
                                  message.reply = stringAt(entry, "reply");
                                  message.read = boolAt(entry, "read");
                                  message.replySeen = boolAt(entry, "replySeen");
                                  if (message.id > 0) messages.push_back(std::move(message));
                              }
                          callback(std::move(messages), static_cast<int>(intAt(*value, "unread")), {});
                      });
}
void Service::fetchStaffMessages(StaffMessagesCallback callback) {
    staffMessagesRequest("GET", "admin/messages", {}, std::move(callback));
}
void Service::sendStaffMessage(std::string kind, std::string body, StaffMessagesCallback callback) {
    auto value = Json::object();
    value["kind"] = std::move(kind);
    value["body"] = std::move(body);
    staffMessagesRequest("PUT", "admin/messages", std::move(value), std::move(callback));
}
void Service::acknowledgeStaffReplies(std::vector<int64_t> ids) {
    if (ids.empty()) return;
    if (ids.size() > 100) ids.resize(100);
    auto list = Json::array();
    for (auto id : ids) list.push(id);
    auto body = Json::object();
    body["ids"] = std::move(list);
    moderationRequest("PUT", "admin/messages/seen", std::move(body), [](std::optional<Json>, std::string) {});
}
void Service::fetchStaffInbox(StaffMessagesCallback callback) {
    staffMessagesRequest("GET", "admin/inbox", {}, std::move(callback));
}
void Service::readStaffMessage(int64_t id, StaffMessagesCallback callback) {
    staffMessagesRequest("PUT", fmt::format("admin/inbox/{}/read", id), Json::object(), std::move(callback));
}
void Service::deleteStaffMail(std::vector<int64_t> ids, StaffMessagesCallback callback) {
    if (ids.size() > 100) ids.resize(100);
    auto list = Json::array();
    for (auto id : ids) list.push(id);
    auto body = Json::object();
    body["ids"] = std::move(list);
    staffMessagesRequest("PUT", isOwner() ? "admin/inbox/delete" : "admin/messages/delete", std::move(body),
                         std::move(callback));
}
void Service::messageStaffMember(std::string uid, std::string text, StaffMessagesCallback callback) {
    auto body = Json::object();
    body["body"] = std::move(text);
    staffMessagesRequest("PUT", "admin/staff/" + uid + "/message", std::move(body), std::move(callback));
}
void Service::replyStaffMessage(int64_t id, std::string reply, StaffMessagesCallback callback) {
    auto body = Json::object();
    body["reply"] = std::move(reply);
    staffMessagesRequest("PUT", fmt::format("admin/inbox/{}/reply", id), std::move(body), std::move(callback));
}
// 플레이어의 제재 이력 (사유별 횟수와 최근 제재 최대 20건, 현재 제재 여부).
void Service::fetchBanHistory(std::string uid, std::function<void(std::optional<BanHistory>, std::string)> callback) {
    moderationRequest("GET", "admin/players/" + uid + "/ban-history", {},
                      [callback = std::move(callback)](std::optional<Json> value, std::string error) {
                          if (!value) {
                              callback(std::nullopt, std::move(error));
                              return;
                          }
                          BanHistory history;
                          history.total = static_cast<int>(intAt(*value, "total"));
                          history.reportBans = static_cast<int>(intAt(*value, "reportBans"));
                          if ((*value)["reasons"].isArray())
                              for (auto const& entry : (*value)["reasons"].asArray().unwrap()) {
                                  if (!entry.isObject() || history.reasons.size() >= 50) continue;
                                  BanHistory::Reason reason;
                                  reason.reason = stringAt(entry, "reason");
                                  reason.count = static_cast<int>(intAt(entry, "count"));
                                  reason.lastAt = intAt(entry, "lastAt");
                                  reason.lastDurationMs = intAt(entry, "lastDurationMs");
                                  reason.lastPermanent = boolAt(entry, "lastPermanent");
                                  history.reasons.push_back(std::move(reason));
                              }
                          if ((*value)["bans"].isArray())
                              for (auto const& entry : (*value)["bans"].asArray().unwrap()) {
                                  if (!entry.isObject() || history.bans.size() >= 20) continue;
                                  BanHistory::Entry ban;
                                  ban.reason = stringAt(entry, "reason");
                                  ban.at = intAt(entry, "at");
                                  ban.durationMs = intAt(entry, "durationMs");
                                  ban.permanent = boolAt(entry, "permanent");
                                  history.bans.push_back(std::move(ban));
                              }
                          auto const& current = (*value)["current"];
                          if (current.isObject()) {
                              history.banned = true;
                              history.permanent = boolAt(current, "permanent");
                              history.until = intAt(current, "until");
                          }
                          callback(std::move(history), {});
                      });
}
// 제재를 걸 때 취소 / 영향을 받는 경기 수 미리보기.
void Service::fetchBanPreview(std::string uid, std::function<void(std::optional<BanPreview>, std::string)> callback) {
    moderationRequest("GET", "admin/bans/" + uid + "/preview", {},
                      [callback = std::move(callback)](std::optional<Json> value, std::string error) {
                          if (!value) {
                              callback(std::nullopt, std::move(error));
                              return;
                          }
                          BanPreview preview;
                          preview.matches = static_cast<int>(intAt(*value, "matches"));
                          preview.opponents = static_cast<int>(intAt(*value, "opponents"));
                          preview.retentionDays = static_cast<int>(intAt(*value, "retentionDays", 7));
                          preview.cancelMax = static_cast<int>(intAt(*value, "cancelMax", 2000));
                          preview.banned = boolAt(*value, "banned");
                          callback(preview, {});
                      });
}
// 듀얼 요청 무시(차단). Season 1은 ignore, Season 2는 컵 경로의 block을 쓴다.
void Service::ignoreDuel(std::string requestId, int minutes, RankedStateCallback callback) {
    auto body = Json::object();
    body["id"] = std::move(requestId);
    body["minutes"] = std::max(0, minutes);
    rankedRequest("PUT", state().rankedCup.empty() ? std::string("ranked/duel/ignore") : rankedPath("duel/block"),
                  std::move(body), std::move(callback));
}
// 듀얼 설정 / 차단 목록 응답 파싱과 요청 공통 처리. Season 2 여부에 따라 경로가 달라진다.
DuelBlockList parseDuelBlocks(Json const& value) {
    DuelBlockList result;
    auto const& settings = value["settings"];
    result.settings.enabled = settings["enabled"].asBool().unwrapOr(true);
    result.settings.anonymous = settings["anonymous"].asBool().unwrapOr(true);
    if (value["rating"].isNumber()) result.rating = static_cast<int>(intAt(value, "rating"));
    result.limit = static_cast<int>(intAt(value, "limit", 300));
    result.gap = static_cast<int>(intAt(value, "gap", 500));
    if (value["blocks"].isArray())
        for (auto const& entry : value["blocks"].asArray().unwrap()) {
            DuelBlock block;
            block.id = stringAt(entry, "id");
            block.anonymous = boolAt(entry, "anonymous");
            block.createdAt = intAt(entry, "createdAt");
            block.until = intAt(entry, "until");
            if (entry["profile"].isObject()) block.player = parseProfile(entry["profile"]);
            if (!block.id.empty()) result.blocks.push_back(std::move(block));
        }
    return result;
}
void duelBlocksRequest(std::string method, std::string path, Json body, DuelBlocksCallback callback) {
    moderationRequest(std::move(method), std::move(path), std::move(body),
                      [callback = std::move(callback)](std::optional<Json> value, std::string error) {
                          if (!value) {
                              callback(std::nullopt, std::move(error));
                              return;
                          }
                          callback(parseDuelBlocks(*value), {});
                      });
}
bool season2Duels() {
    return !state().rankedCup.empty();
}
void Service::fetchDuelBlocks(DuelBlocksCallback callback) {
    duelBlocksRequest("GET", season2Duels() ? rankedPath("duel/blocks") : std::string("ranked/duel/blocks"), {},
                      std::move(callback));
}
void Service::editDuelBlock(std::string blockId, int minutes, DuelBlocksCallback callback) {
    auto body = Json::object();
    body["id"] = std::move(blockId);
    body["minutes"] = std::max(0, minutes);
    duelBlocksRequest("PUT", season2Duels() ? rankedPath("duel/blocks") : std::string("ranked/duel/blocks"),
                      std::move(body), std::move(callback));
}
void Service::removeDuelBlock(std::string blockId, DuelBlocksCallback callback) {
    if (season2Duels()) {
        duelBlocksRequest("DELETE", rankedPath(blockId.empty() ? std::string("duel/blocks") : "duel/blocks/" + blockId),
                          {}, std::move(callback));
        return;
    }
    duelBlocksRequest("DELETE", blockId.empty() ? std::string("ranked/duel/blocks") : "ranked/duel/blocks/" + blockId,
                      {}, std::move(callback));
}
void Service::setDuelSettings(DuelSettings settings, DuelBlocksCallback callback) {
    auto body = Json::object();
    body["enabled"] = settings.enabled;
    body["anonymous"] = settings.anonymous;
    duelBlocksRequest("PUT", season2Duels() ? rankedPath("duel/settings") : std::string("ranked/duel/settings"),
                      std::move(body), std::move(callback));
}
// Season 2 듀얼 상대 검색 / 최근 상대 응답 파싱 (최대 8명).
std::vector<Season2DuelPlayer> parseSeason2DuelPlayers(Json const& value) {
    std::vector<Season2DuelPlayer> players;
    if (value["players"].isArray())
        for (auto const& entry : value["players"].asArray().unwrap()) {
            if (!entry.isObject() || players.size() >= 8) continue;
            Season2DuelPlayer found;
            found.uid = stringAt(entry, "uid");
            found.name = stringAt(entry, "name");
            found.status = stringAt(entry, "status");
            found.reason = stringAt(entry, "reason");
            found.available = boolAt(entry, "available");
            found.lastDuel = intAt(entry, "lastDuel");
            if (entry["profile"].isObject()) found.player = parseProfile(entry["profile"]);
            if (!found.uid.empty()) players.push_back(std::move(found));
        }
    return players;
}
// Season 2 듀얼 상대 이름 검색 / 최근 듀얼 상대(최대 5명). 컵이 열려 있어야 한다.
void Service::searchSeason2Duel(std::string name,
                                std::function<void(std::vector<Season2DuelPlayer>, std::string)> callback) {
    if (state().rankedCup.empty()) {
        callback({}, "Open a Season 2 cup first.");
        return;
    }
    auto body = Json::object();
    body["name"] = std::move(name);
    moderationRequest("PUT", rankedPath("duel/search"), std::move(body),
                      [callback = std::move(callback)](std::optional<Json> value, std::string error) {
                          if (!value) {
                              callback({}, error.empty() ? std::string("The search failed.") : std::move(error));
                              return;
                          }
                          callback(parseSeason2DuelPlayers(*value), {});
                      });
}
void Service::recentSeason2Duels(std::function<void(std::vector<Season2DuelPlayer>, std::string)> callback) {
    if (state().rankedCup.empty()) {
        callback({}, "Open a Season 2 cup first.");
        return;
    }
    moderationRequest("GET", rankedPath("duel/recent"), {},
                      [callback = std::move(callback)](std::optional<Json> value, std::string error) {
                          if (!value) {
                              callback({},
                                       error.empty() ? std::string("Could not load your duels.") : std::move(error));
                              return;
                          }
                          auto players = parseSeason2DuelPlayers(*value);
                          if (players.size() > 5) players.resize(5);
                          callback(std::move(players), {});
                      });
}
// 다른 컵에서 진행 중인 매치만 살짝 확인한다 (상태는 반영하지 않고 match 부분만 돌려줌).
void Service::peekSeason2Match(std::string cup, std::function<void(std::optional<RankedMatch>, std::string)> callback) {
    if (cup.empty()) {
        callback(std::nullopt, "No cup.");
        return;
    }
    connect([cup = std::move(cup), callback = std::move(callback)](bool ok, std::string detail) mutable {
        if (!ok) {
            callback(std::nullopt, std::move(detail));
            return;
        }
        request("GET", "ranked2/" + cup + "/state", {},
                [callback = std::move(callback)](web::WebResponse response) mutable {
                    auto value = response.json();
                    if (!response.ok() || !value || !value.unwrap().isObject()) {
                        callback(std::nullopt, networkError(response));
                        return;
                    }
                    auto parsed = parseRankedState(value.unwrap());
                    callback(std::move(parsed.match), {});
                });
    });
}
// 리플레이 업로드를 대기열에 넣는다 (최대 4개, 넘으면 가장 오래된 것을 버림).
// 봇 / 디버그 방 경기와 같은 경기의 중복은 제외한다. 실제 전송은 tick에서 한다.
void Service::uploadReplay(ReplayUpload upload) {
    auto& s = state();
    if (upload.matchId.empty() || upload.data.empty()) return;
    if (s.botBattle || upload.matchId.starts_with("debug-bot-")) return;
    if (upload.matchId.starts_with(kDebugRoomMatch)) return;
    for (auto const& entry : s.replays)
        if (entry.upload.matchId == upload.matchId) return;
    if (upload.roomId.empty() && s.room && s.room->battle && s.room->battle->id == upload.matchId)
        upload.roomId = s.room->id;
    if (s.replays.size() >= 4) s.replays.pop_front();
    s.replays.push_back({std::move(upload)});
}
// 경기의 리플레이 목록 / 리플레이 파일 하나를 받는다 (파일은 60초 제한).
void Service::fetchReplayList(std::string matchId,
                              std::function<void(std::vector<ReplaySummary>, std::string)> callback) {
    moderationRequest("GET", "replays/" + matchId, {},
                      [callback = std::move(callback)](std::optional<Json> value, std::string error) {
                          std::vector<ReplaySummary> list;
                          if (value && (*value)["replays"].isArray())
                              for (auto const& entry : (*value)["replays"]) {
                                  ReplaySummary item;
                                  item.uid = stringAt(entry, "uid");
                                  item.side = stringAt(entry, "side");
                                  item.end = stringAt(entry, "end");
                                  item.run = static_cast<int>(intAt(entry, "run"));
                                  item.percent = static_cast<int>(intAt(entry, "percent"));
                                  item.steps = intAt(entry, "steps");
                                  item.bytes = intAt(entry, "bytes");
                                  item.at = intAt(entry, "at");
                                  if (!item.uid.empty()) list.push_back(std::move(item));
                              }
                          callback(std::move(list), std::move(error));
                      });
}
void Service::fetchReplay(std::string matchId, std::string uid,
                          std::function<void(std::optional<ReplayFile>, std::string)> callback) {
    connect([matchId = std::move(matchId), uid = std::move(uid),
             callback = std::move(callback)](bool ok, std::string detail) mutable {
        if (!ok) {
            callback(std::nullopt, std::move(detail));
            return;
        }
        request(
            "GET", "replays/" + matchId + "/" + uid, {},
            [callback = std::move(callback)](web::WebResponse response) {
                if (response.code() == 404) {
                    callback(std::nullopt, "This replay is no longer available.");
                    return;
                }
                auto parsed = response.json();
                if (!response.ok() || !parsed || !parsed.unwrap().isObject()) {
                    callback(std::nullopt, networkError(response));
                    return;
                }
                auto const& value = parsed.unwrap();
                ReplayFile file;
                file.uid = stringAt(value, "uid");
                file.name = stringAt(value, "name");
                file.levelName = stringAt(value, "levelName");
                file.end = stringAt(value, "end");
                file.data = stringAt(value, "data");
                file.run = static_cast<int>(intAt(value, "run"));
                file.percent = static_cast<int>(intAt(value, "percent"));
                file.serverBest = static_cast<int>(intAt(value, "serverBest", -1));
                file.coins = static_cast<int>(std::clamp<int64_t>(intAt(value, "coins", -1), -1, 3));
                file.serverCoins = static_cast<int>(std::clamp<int64_t>(intAt(value, "serverCoins", -1), -1, 3));
                file.level = intAt(value, "level");
                file.at = intAt(value, "createdAt");
                if (file.data.empty()) {
                    callback(std::nullopt, "The replay data is missing.");
                    return;
                }
                callback(std::move(file), {});
            },
            {}, false, false, false, {}, {}, {}, 60);
    });
}
// 플레이어 신고. 자기 자신은 신고할 수 없고 상세 내용은 160자, 이름은 32자로 자른다.
void Service::reportPlayer(PlayerProfile const& target, std::string reason, std::string detail, Done callback) {
    if (target.uid.empty() || target.uid == state().profile.uid) {
        callback(false, "You cannot report this player.");
        return;
    }
    auto body = Json::object();
    body["reason"] = std::move(reason);
    body["detail"] = detail.substr(0, 160);
    auto who = Json::object();
    who["name"] = target.name.substr(0, 32);
    who["accountId"] = target.accountId;
    auto me = Json::object();
    me["name"] = state().profile.name.substr(0, 32);
    me["accountId"] = state().profile.accountId;
    body["target"] = std::move(who);
    body["from"] = std::move(me);
    moderationRequest("PUT", "reports/" + target.uid, std::move(body),
                      [callback = std::move(callback)](std::optional<Json> value, std::string error) {
                          if (!value) {
                              callback(false, std::move(error));
                              return;
                          }
                          callback(true, boolAt(*value, "updated") ? "updated" : "");
                      });
}
// 신고 대상 목록 파싱 (대상별 신고 내역과 처리 결과 포함).
std::vector<ReportedPlayer> parseReportedPlayers(Json const& list) {
    std::vector<ReportedPlayer> targets;
    if (list.isArray())
        for (auto const& entry : list.asArray().unwrap()) {
            if (!entry.isObject()) continue;
            ReportedPlayer target;
            target.uid = stringAt(entry, "uid");
            target.name = stringAt(entry, "name");
            target.accountId = static_cast<int>(intAt(entry, "accountId"));
            target.count = static_cast<int>(intAt(entry, "count"));
            target.latestAt = intAt(entry, "latestAt");
            if (entry["ban"].isObject()) target.ban = parseBan(entry["ban"]);
            target.state = stringAt(entry, "state");
            target.outcome = stringAt(entry, "outcome");
            target.handledBy = stringAt(entry, "handledBy");
            target.handledAt = intAt(entry, "handledAt");
            target.banUntil = intAt(entry, "banUntil", -1);
            target.note = stringAt(entry, "note");
            if (entry["reports"].isArray())
                for (auto const& item : entry["reports"].asArray().unwrap()) {
                    if (!item.isObject()) continue;
                    PlayerReport report;
                    report.id = intAt(item, "id");
                    report.createdAt = intAt(item, "createdAt");
                    report.reporterUid = stringAt(item, "reporterUid");
                    report.reporterName = stringAt(item, "reporterName");
                    report.reporterAccountId = static_cast<int>(intAt(item, "reporterAccountId"));
                    report.reporterReportBanned = boolAt(item, "reporterReportBanned");
                    report.reason = stringAt(item, "reason");
                    report.detail = stringAt(item, "detail");
                    report.matchId = stringAt(item, "matchId");
                    target.reports.push_back(std::move(report));
                }
            if (!target.uid.empty()) targets.push_back(std::move(target));
        }
    return targets;
}
// 신고 처리 도구: 접수된 신고 목록 / 처리 기록, 내 신고에 대한 처리 알림 조회 및 확인.
void Service::fetchReports(std::function<void(std::vector<ReportedPlayer>, std::string)> callback) {
    moderationRequest("GET", "admin/reports", {},
                      [callback = std::move(callback)](std::optional<Json> value, std::string error) {
                          if (!value) {
                              callback({}, std::move(error));
                              return;
                          }
                          callback(parseReportedPlayers((*value)["targets"]), {});
                      });
}
void Service::fetchReportLog(std::function<void(std::vector<ReportedPlayer>, std::string)> callback) {
    moderationRequest("GET", "admin/reports/log", {},
                      [callback = std::move(callback)](std::optional<Json> value, std::string error) {
                          if (!value) {
                              callback({}, std::move(error));
                              return;
                          }
                          callback(parseReportedPlayers((*value)["entries"]), {});
                      });
}
void Service::fetchReportNotices(std::function<void(std::vector<ReportNotice>, std::string)> callback) {
    moderationRequest("GET", "reports/notices", {},
                      [callback = std::move(callback)](std::optional<Json> value, std::string error) {
                          std::vector<ReportNotice> notices;
                          if (!value) {
                              callback(std::move(notices), std::move(error));
                              return;
                          }
                          if ((*value)["notices"].isArray())
                              for (auto const& entry : (*value)["notices"].asArray().unwrap()) {
                                  if (!entry.isObject()) continue;
                                  ReportNotice notice;
                                  notice.id = intAt(entry, "id");
                                  notice.handledAt = intAt(entry, "handledAt");
                                  notice.banUntil = intAt(entry, "banUntil", -1);
                                  notice.targetName = stringAt(entry, "targetName");
                                  notice.state = stringAt(entry, "state");
                                  notice.outcome = stringAt(entry, "outcome");
                                  notice.note = stringAt(entry, "note");
                                  if (notice.id > 0) notices.push_back(std::move(notice));
                              }
                          callback(std::move(notices), {});
                      });
}
void Service::acknowledgeReportNotices(std::vector<int64_t> ids) {
    if (ids.empty()) return;
    if (ids.size() > 50) ids.resize(50);
    auto list = Json::array();
    for (auto id : ids) list.push(id);
    auto body = Json::object();
    body["ids"] = std::move(list);
    moderationRequest("PUT", "reports/notices", std::move(body), [](std::optional<Json>, std::string) {});
}
// 레이팅 환급 목록 조회 / 확인 (경기 취소 등으로 돌려받은 레이팅).
void Service::fetchRatingRefunds(std::function<void(std::vector<RatingRefund>, std::string)> callback) {
    moderationRequest("GET", "refunds", {},
                      [callback = std::move(callback)](std::optional<Json> value, std::string error) {
                          std::vector<RatingRefund> refunds;
                          if (!value) {
                              callback(std::move(refunds), std::move(error));
                              return;
                          }
                          if ((*value)["refunds"].isArray())
                              for (auto const& entry : (*value)["refunds"].asArray().unwrap()) {
                                  if (!entry.isObject()) continue;
                                  RatingRefund refund;
                                  refund.id = intAt(entry, "id");
                                  refund.playedAt = intAt(entry, "playedAt");
                                  refund.createdAt = intAt(entry, "createdAt");
                                  refund.amount = entry["amount"].asDouble().unwrapOr(0.);
                                  refund.opponent = stringAt(entry, "opponent");
                                  refund.reason = stringAt(entry, "reason");
                                  if (refund.id > 0) refunds.push_back(std::move(refund));
                              }
                          callback(std::move(refunds), {});
                      });
}
// 회피(dodge) 로그 항목 파싱.
DodgeLogEntry parseDodgeLogEntry(Json const& entry) {
    DodgeLogEntry result;
    result.id = intAt(entry, "id");
    result.at = intAt(entry, "at");
    result.uid = stringAt(entry, "uid");
    result.name = stringAt(entry, "name");
    result.opponentUid = stringAt(entry, "opponentUid");
    result.opponentName = stringAt(entry, "opponentName");
    result.cause = stringAt(entry, "cause");
    result.state = stringAt(entry, "state");
    result.reason = stringAt(entry, "reason");
    result.cup = stringAt(entry, "cup");
    result.cupName = stringAt(entry, "cupName");
    result.duel = boolAt(entry, "duel");
    result.weight = static_cast<int>(intAt(entry, "weight"));
    result.stackAfter = static_cast<int>(intAt(entry, "stackAfter"));
    result.ratingDelta = static_cast<int>(intAt(entry, "ratingDelta"));
    result.penalized = boolAt(entry, "penalized");
    result.mapShown = boolAt(entry, "mapShown");
    if (entry["map"].isObject() && intAt(entry["map"], "id") > 0) {
        LevelInfo level;
        level.id = intAt(entry["map"], "id");
        level.name = stringAt(entry["map"], "name");
        result.map = std::move(level);
    }
    return result;
}
// 관리자용 Ranked 기록 파싱 (레이팅, 전적, 회피 상태, 조정 이력). uid가 없으면 실패.
std::optional<AdminRankedRecord> parseAdminRanked(Json const& value) {
    if (!value.isObject() || stringAt(value, "uid").empty()) return std::nullopt;
    AdminRankedRecord record;
    record.uid = stringAt(value, "uid");
    record.name = stringAt(value, "name");
    record.rating = value["rating"].asDouble().unwrapOr(0.);
    record.matches = static_cast<int>(intAt(value, "matches"));
    record.wins = static_cast<int>(intAt(value, "wins"));
    record.losses = static_cast<int>(intAt(value, "losses"));
    record.placements = static_cast<int>(intAt(value, "placements"));
    record.dodge = parseDodge(value["dodge"]);
    record.dodges = static_cast<int>(intAt(value["dodge"], "dodges"));
    if (value["actions"].isArray())
        for (auto const& entry : value["actions"].asArray().unwrap()) {
            if (!entry.isObject()) continue;
            AdminAction action;
            action.kind = stringAt(entry, "kind");
            action.reason = stringAt(entry, "reason");
            action.adminName = stringAt(entry, "adminName");
            action.amount = entry["amount"].asDouble().unwrapOr(0.);
            action.before = entry["before"].asDouble().unwrapOr(0.);
            action.after = entry["after"].asDouble().unwrapOr(0.);
            action.at = intAt(entry, "at");
            record.actions.push_back(std::move(action));
        }
    if (value["dodges"].isArray())
        for (auto const& entry : value["dodges"].asArray().unwrap())
            if (entry.isObject()) record.dodgeLog.push_back(parseDodgeLogEntry(entry));
    return record;
}
// 회피 로그: 관리자용 전체 목록 / Season 2에서 내 로그(최대 100건).
void Service::fetchDodgeLog(std::function<void(std::vector<DodgeLogEntry>, std::string)> callback) {
    moderationRequest("GET", "admin/dodges", {},
                      [callback = std::move(callback)](std::optional<Json> value, std::string error) {
                          std::vector<DodgeLogEntry> entries;
                          if (!value) {
                              callback(std::move(entries), std::move(error));
                              return;
                          }
                          if ((*value)["dodges"].isArray())
                              for (auto const& entry : (*value)["dodges"].asArray().unwrap())
                                  if (entry.isObject()) entries.push_back(parseDodgeLogEntry(entry));
                          callback(std::move(entries), {});
                      });
}
void Service::fetchSeason2Dodges(std::function<void(std::optional<std::vector<DodgeLogEntry>>, std::string)> callback) {
    connect([callback = std::move(callback)](bool ok, std::string detail) mutable {
        if (!ok) {
            callback(std::nullopt, std::move(detail));
            return;
        }
        request("GET", "ranked2/dodges", {}, [callback = std::move(callback)](web::WebResponse response) mutable {
            auto value = response.json();
            if (!response.ok() || !value || !value.unwrap()["dodges"].isArray()) {
                callback(std::nullopt, networkError(response));
                return;
            }
            std::vector<DodgeLogEntry> entries;
            for (auto const& entry : value.unwrap()["dodges"].asArray().unwrap())
                if (entry.isObject() && entries.size() < 100) entries.push_back(parseDodgeLogEntry(entry));
            callback(std::move(entries), {});
        });
    });
}
// 관리자의 Ranked 조정 요청 공통 처리: 레이팅 증감과 회피 스택 설정.
void adminRankedRequest(std::string method, std::string path, Json body, Service::AdminRankedCallback callback) {
    moderationRequest(std::move(method), std::move(path), std::move(body),
                      [callback = std::move(callback)](std::optional<Json> value, std::string error) {
                          if (!value) {
                              callback(std::nullopt, std::move(error));
                              return;
                          }
                          auto record = parseAdminRanked(*value);
                          callback(std::move(record), record ? std::string() : std::string("Invalid server answer."));
                      });
}
void Service::fetchAdminRanked(std::string uid, AdminRankedCallback callback) {
    adminRankedRequest("GET", "admin/ranked/" + uid, {}, std::move(callback));
}
void Service::adjustRating(std::string uid, double delta, std::string reason, AdminRankedCallback callback) {
    auto body = Json::object();
    body["delta"] = delta;
    body["reason"] = std::move(reason);
    adminRankedRequest("PUT", "admin/ranked/" + uid + "/rating", std::move(body), std::move(callback));
}
void Service::setDodgeStack(std::string uid, int stack, std::string reason, bool lift, AdminRankedCallback callback) {
    auto body = Json::object();
    body["stack"] = stack;
    body["reason"] = std::move(reason);
    body["lift"] = lift;
    adminRankedRequest("PUT", "admin/ranked/" + uid + "/dodge", std::move(body), std::move(callback));
}
// 계정 선택: 서버가 여러 Ranked 기록 중 선택을 요구했을 때 고른 uid를 알리고 인증 상태를 초기화해 다시 동기화한다.
std::vector<AccountChoice> const& Service::accountChoices() const {
    return state().accountChoices;
}
void Service::chooseAccount(std::string uid, Done callback) {
    auto& s = state();
    auto body = Json::object();
    body["accountId"] = s.profile.accountId;
    body["name"] = s.profile.name;
    body["uid"] = std::move(uid);
    moderationRequest("PUT", "account/choose", std::move(body),
                      [callback = std::move(callback)](std::optional<Json> value, std::string error) {
                          if (!value) {
                              callback(false, std::move(error));
                              return;
                          }
                          auto& s = state();
                          s.accountChoices.clear();
                          s.syncedAccount = 0;
                          s.syncedUid.clear();
                          s.syncRounds = 0;
                          if (!s.room) s.token.clear();
                          callback(true, {});
                      });
}
void Service::acknowledgeRatingRefunds(std::vector<int64_t> ids) {
    if (ids.empty()) return;
    if (ids.size() > 50) ids.resize(50);
    auto list = Json::array();
    for (auto id : ids) list.push(id);
    auto body = Json::object();
    body["ids"] = std::move(list);
    moderationRequest("PUT", "refunds", std::move(body), [](std::optional<Json>, std::string) {});
}
// 관리자: 선택한 Ranked 경기들을 취소하고 레이팅을 환급한다. 사유는 160자까지.
void Service::cancelRankedMatches(std::string target, std::vector<std::string> roomIds, std::string reason,
                                  std::function<void(std::optional<MatchCancelResult>, std::string)> callback) {
    auto rooms = Json::array();
    for (auto& room : roomIds) rooms.push(std::move(room));
    auto body = Json::object();
    body["target"] = std::move(target);
    body["rooms"] = std::move(rooms);
    if (reason.size() > 160) reason.resize(160);
    body["reason"] = std::move(reason);
    moderationRequest("PUT", "admin/matches/cancel", std::move(body),
                      [callback = std::move(callback)](std::optional<Json> value, std::string error) {
                          if (!value) {
                              callback(std::nullopt, std::move(error));
                              return;
                          }
                          MatchCancelResult result;
                          result.canceled = static_cast<int>(intAt(*value, "canceled"));
                          result.skipped = static_cast<int>(intAt(*value, "skipped"));
                          result.removed = (*value)["removed"].asDouble().unwrapOr(0.);
                          if ((*value)["refunds"].isArray())
                              for (auto const& entry : (*value)["refunds"].asArray().unwrap())
                                  if (entry.isObject())
                                      result.refunds.emplace_back(stringAt(entry, "name"),
                                                                  entry["amount"].asDouble().unwrapOr(0.));
                          callback(std::move(result), {});
                      });
}
// 신고 기각(메모 200자까지) / 내 신고 조회와 철회.
void Service::dismissReports(std::string uid, std::string note, Done callback) {
    auto body = Json::object();
    if (note.size() > 200) note.resize(200);
    if (!note.empty()) body["note"] = std::move(note);
    moderationRequest("PUT", "admin/reports/" + uid + "/dismiss", std::move(body),
                      [callback = std::move(callback)](std::optional<Json> value, std::string error) {
                          callback(value.has_value(), std::move(error));
                      });
}
namespace {
std::vector<MyReport> parseMyReports(Json const& value) {
    std::vector<MyReport> reports;
    if (value["reports"].isArray())
        for (auto const& entry : value["reports"].asArray().unwrap()) {
            if (!entry.isObject()) continue;
            MyReport report;
            report.id = intAt(entry, "id");
            report.createdAt = intAt(entry, "createdAt");
            report.handledAt = intAt(entry, "handledAt");
            report.banUntil = intAt(entry, "banUntil", -1);
            report.targetUid = stringAt(entry, "targetUid");
            report.targetName = stringAt(entry, "targetName");
            report.reason = stringAt(entry, "reason");
            report.detail = stringAt(entry, "detail");
            report.state = stringAt(entry, "state");
            report.outcome = stringAt(entry, "outcome");
            report.note = stringAt(entry, "note");
            if (report.id > 0) reports.push_back(std::move(report));
        }
    return reports;
}
}
void Service::fetchMyReports(MyReportsCallback callback) {
    moderationRequest("GET", "reports/mine", {},
                      [callback = std::move(callback)](std::optional<Json> value, std::string error) {
                          if (!value) {
                              callback({}, std::move(error));
                              return;
                          }
                          callback(parseMyReports(*value), {});
                      });
}
void Service::withdrawReport(int64_t id, MyReportsCallback callback) {
    moderationRequest("DELETE", fmt::format("reports/mine/{}", id), {},
                      [callback = std::move(callback)](std::optional<Json> value, std::string error) {
                          if (!value) {
                              callback({}, error.empty() ? std::string("The report could not be withdrawn.")
                                                         : std::move(error));
                              return;
                          }
                          callback(parseMyReports(*value), {});
                      });
}
// 제재 목록 조회, 제재 / 신고 제재 설정 (기간 ms, 사유 160자, 이름 32자 제한), 해제.
void fetchBanList(char const* path, std::function<void(std::vector<BanInfo>, std::string)> callback) {
    moderationRequest("GET", path, {}, [callback = std::move(callback)](std::optional<Json> value, std::string error) {
        std::vector<BanInfo> bans;
        if (!value) {
            callback(std::move(bans), std::move(error));
            return;
        }
        if ((*value)["bans"].isArray())
            for (auto const& entry : (*value)["bans"].asArray().unwrap())
                if (entry.isObject()) bans.push_back(parseBan(entry));
        callback(std::move(bans), {});
    });
}
void Service::fetchBans(std::function<void(std::vector<BanInfo>, std::string)> callback) {
    fetchBanList("admin/bans", std::move(callback));
}
void Service::fetchReportBans(std::function<void(std::vector<BanInfo>, std::string)> callback) {
    fetchBanList("admin/report-bans", std::move(callback));
}
void putBan(std::string const& path, std::string name, int accountId, int64_t durationMs, std::string reason,
            BanResultCallback callback, std::optional<int> cancelMatches = std::nullopt) {
    auto body = Json::object();
    if (cancelMatches) body["cancelMatches"] = *cancelMatches;
    body["durationMs"] = std::max<int64_t>(0, durationMs);
    body["reason"] = reason.substr(0, 160);
    body["name"] = name.substr(0, 32);
    body["accountId"] = std::max(0, accountId);
    moderationRequest(
        "PUT", path, std::move(body), [callback = std::move(callback)](std::optional<Json> value, std::string error) {
            if (!value) {
                callback(std::nullopt, 0, std::move(error));
                return;
            }
            int const canceled =
                (*value)["canceled"].isObject() ? static_cast<int>(intAt((*value)["canceled"], "canceled")) : 0;
            callback((*value)["ban"].isObject() ? std::optional(parseBan((*value)["ban"])) : std::nullopt, canceled,
                     {});
        });
}
// 관리자: Argon 토큰 검증 결과를 서버에 시험해 본다.
void Service::argonTest(std::string token, int accountId, std::string name,
                        std::function<void(std::optional<ArgonCheck>, std::string)> callback) {
    auto body = Json::object();
    body["token"] = std::move(token);
    body["accountId"] = accountId;
    body["name"] = name.substr(0, 32);
    moderationRequest("PUT", "admin/argon-test", std::move(body),
                      [callback = std::move(callback)](std::optional<Json> value, std::string error) {
                          if (!value) {
                              callback(std::nullopt, std::move(error));
                              return;
                          }
                          ArgonCheck check;
                          check.reachable = boolAt(*value, "reachable");
                          check.valid = boolAt(*value, "valid");
                          check.validWeak = boolAt(*value, "validWeak");
                          check.status = static_cast<int>(intAt(*value, "status"));
                          check.ms = static_cast<int>(intAt(*value, "ms"));
                          check.username = stringAt(*value, "username");
                          check.cause = stringAt(*value, "cause");
                          check.error = stringAt(*value, "error");
                          callback(check, {});
                      });
}
void Service::setBan(std::string uid, std::string name, int accountId, int64_t durationMs, std::string reason,
                     int cancelMatches, BanResultCallback callback) {
    putBan("admin/bans/" + uid, std::move(name), accountId, durationMs, std::move(reason), std::move(callback),
           cancelMatches);
}
void Service::setReportBan(std::string uid, std::string name, int accountId, int64_t durationMs, std::string reason,
                           BanCallback callback) {
    putBan("admin/report-bans/" + uid, std::move(name), accountId, durationMs, std::move(reason),
           [callback = std::move(callback)](std::optional<BanInfo> ban, int, std::string error) {
               callback(std::move(ban), std::move(error));
           });
}
void Service::liftBan(std::string uid, Done callback) {
    moderationRequest("DELETE", "admin/bans/" + uid, Json::object(),
                      [callback = std::move(callback)](std::optional<Json> value, std::string error) {
                          callback(value.has_value(), std::move(error));
                      });
}
void Service::liftReportBan(std::string uid, Done callback) {
    moderationRequest("DELETE", "admin/report-bans/" + uid, Json::object(),
                      [callback = std::move(callback)](std::optional<Json> value, std::string error) {
                          callback(value.has_value(), std::move(error));
                      });
}
// Ranked 대기열/세션 취소. 봇 전투면 로컬에서 끝낸다. searchCancel이면 검색 취소로 기록한다.
void Service::cancelRanked(RankedStateCallback callback, bool searchCancel, std::string seen, bool declineRematch) {
    if (state().botBattle) {
        endBotBattle();
        callback(state().lastRankedState ? state().lastRankedState : std::optional<RankedState>(RankedState{}), {});
        return;
    }
    auto body = Json::object();
    if (searchCancel) {
        body["searchCancel"] = true;
        body["seen"] = std::move(seen);
    }
    if (declineRematch) body["declineRematch"] = true;  // 1.0.7: 서버 _s2_decline_rematch
    rankedRequest("DELETE", rankedPath("session"), std::move(body), std::move(callback));
}
// Ranked 매치 진행 API들: 무결성 위반 실격, 후보 맵 제출, 에셋 준비 완료, LDM 설정, 맵 재추첨 요청/투표,
// Season 2 밴과 플레이한 맵 보고, 결과 확인.
void Service::disqualifyRanked(std::string matchId, std::string integrityCode, std::string integrityFeatures,
                               RankedStateCallback callback) {
    if (state().botBattle) {
        callback(std::nullopt, "Bot battle.");
        return;
    }
    auto body = Json::object();
    body["integrityCode"] = std::move(integrityCode);
    body["integrityFeatures"] = std::move(integrityFeatures);
    rankedRequest("PUT", rankedPath("disqualify/" + std::move(matchId)), std::move(body), std::move(callback));
}
void Service::submitRankedCandidates(std::string matchId, std::vector<LevelInfo> candidates,
                                     RankedStateCallback callback) {
    auto body = Json::object();
    body["candidates"] = Json::array();
    for (auto const& candidate : candidates) body["candidates"].push(levelJson(candidate));
    rankedRequest("PUT", rankedPath("candidates/" + std::move(matchId)), std::move(body), std::move(callback));
}
void Service::rankedAssetsReady(std::string matchId, std::string drawId, double seconds, RankedStateCallback callback,
                                bool twoPlayer, bool noSong) {
    auto body = Json::object();
    body["drawId"] = std::move(drawId);
    if (seconds > 0.) body["seconds"] = seconds;
    if (twoPlayer) body["twoPlayer"] = true;
    if (noSong && !state().rankedCup.empty()) body["noSong"] = true;
    rankedRequest("PUT", rankedPath("ready/" + std::move(matchId)), std::move(body), std::move(callback));
}
void Service::setRankedLdm(std::string matchId, bool enabled, RankedStateCallback callback) {
    auto body = Json::object();
    body["enabled"] = enabled;
    rankedRequest("PUT", rankedPath("ldm/" + std::move(matchId)), std::move(body), std::move(callback));
}
bool Service::rankedLdm() const {
    return isRankedMatch() && state().rankedLdm;
}
void Service::rerollRankedMap(std::string matchId, std::string drawId, std::string reason, std::string kind,
                              RankedStateCallback callback) {
    auto body = Json::object();
    body["drawId"] = std::move(drawId);
    body["reason"] = reason.substr(0, 240);
    body["kind"] = kind == "map" ? "map" : "network";
    rankedRequest("PUT", rankedPath("reroll/" + std::move(matchId)), std::move(body), std::move(callback));
}
void Service::voteRankedReroll(std::string matchId, std::string drawId, RankedStateCallback callback) {
    if (state().botBattle) {
        callback(std::nullopt, "No reroll in a bot battle.");
        return;
    }
    auto body = Json::object();
    if (!drawId.empty()) body["drawId"] = std::move(drawId);
    rankedRequest("PUT",
                  state().rankedCup.empty() ? "ranked/vote-reroll/" + std::move(matchId)
                                            : rankedPath("vote-reroll/" + std::move(matchId)),
                  std::move(body), std::move(callback));
}
void Service::banSeason2Map(std::string matchId, int64_t levelId, RankedStateCallback callback) {
    if (state().rankedCup.empty()) {
        callback(std::nullopt, "Map bans are Season 2's.");
        return;
    }
    auto body = Json::object();
    body["level"] = levelId;
    rankedRequest("PUT", rankedPath("ban/" + std::move(matchId)), std::move(body), std::move(callback));
}
void Service::reportSeason2Played(std::string matchId, std::string key, std::vector<int> ids,
                                  std::function<void(bool)> done) {
    if (state().rankedCup.empty() || matchId.empty() || key.empty()) {
        if (done) done(false);
        return;
    }
    auto body = Json::object();
    body["key"] = std::move(key);
    auto list = Json::array();
    for (int const id : ids) list.push(id);
    body["ids"] = std::move(list);
    connect([path = rankedPath("played/" + std::move(matchId)), body = std::move(body),
             done = std::move(done)](bool ok, std::string) mutable {
        if (!ok) {
            if (done) done(false);
            return;
        }
        request("PUT", std::move(path), std::move(body), [done = std::move(done)](web::WebResponse response) {
            if (done) done(response.ok());
        });
    });
}
void Service::acknowledgeRanked(std::string matchId, Done callback) {
    rankedRequest("PUT", rankedPath("ack/" + std::move(matchId)), Json::object(),
                  [callback = std::move(callback)](std::optional<RankedState> value, std::string detail) mutable {
                      callback(value.has_value(), std::move(detail));
                  });
}
// 안티치트를 강제하는지: Ranked는 항상, 핵 허용 일반 방에서만 면제.
bool Service::antiCheatEnforced() const {
    if (isRankedMatch()) return true;
    auto const& s = state();
    return !(s.room && !s.room->id.starts_with("ranked-") && s.room->rules.allowHacks);
}
// 현재 방이 Ranked 매치인지.
bool Service::isRankedMatch() const {
    auto const& s = state();
    if (s.rankedMatch.empty()) return false;
    if (!s.room) return true;
    return s.room->id.starts_with("ranked-") && (!s.room->battle || s.room->battle->id == s.rankedMatch);
}
// 방송 공지 목록을 받는다 (5초 간격 제한). 가장 새 공지가 아직 처리하지 않은 것이고
// kBroadcastLiveMs 이내에 만들어졌으면 화면 표시 대기(broadcastPending)에 올린다.
void fetchBroadcastList(Done callback) {
    auto& s = state();
    if (s.broadcastFetching) {
        if (callback) callback(false, "Loading...");
        return;
    }
    s.broadcastFetching = true;
    s.nextBroadcastFetch = Clock::now() + std::chrono::seconds(5);
    request(
        "GET", "broadcasts", {},
        [callback = std::move(callback)](web::WebResponse response) mutable {
            auto& s = state();
            s.broadcastFetching = false;
            auto parsed = response.json();
            if (!response.ok() || !parsed || !parsed.unwrap().isObject()) {
                if (callback) callback(false, networkError(response));
                return;
            }
            auto const& value = parsed.unwrap();
            std::vector<BroadcastInfo> list;
            if (value["broadcasts"].isArray())
                for (auto const& entry : value["broadcasts"].asArray().unwrap()) {
                    BroadcastInfo info;
                    info.id = intAt(entry, "id");
                    info.createdAt = intAt(entry, "createdAt");
                    info.sender = stringAt(entry, "sender");
                    info.message = stringAt(entry, "message");
                    if (info.id > 0 && !info.message.empty()) list.push_back(std::move(info));
                }
            std::sort(list.begin(), list.end(), [](auto const& a, auto const& b) { return a.id > b.id; });
            int64_t newest = std::max<int64_t>(intAt(value, "latest"), list.empty() ? 0 : list.front().id);
            s.broadcastLoaded = std::max(s.broadcastLoaded, newest);
            s.broadcastLatest = std::max(s.broadcastLatest, newest);
            if (!list.empty() && list.front().id > s.broadcastHandled) {
                auto const& fresh = list.front();
                if (nowMs() - fresh.createdAt < kBroadcastLiveMs && !versus::lounge::realPlayLayer())
                    s.broadcastPending = fresh;
                s.broadcastHandled = fresh.id;
            }
            s.broadcasts = std::move(list);
            if (callback) callback(true, {});
        },
        {}, false, false, false, {}, {}, {}, 10);
}
// 새 공지가 있으면 목록을 다시 받고, 표시 대기 중인 공지는 presenter가 받아 줄 때까지 시도한다.
void tickBroadcasts() {
    auto& s = state();
    if (s.broadcastLatest > s.broadcastLoaded && !s.broadcastFetching && Service::get().connected() &&
        Clock::now() >= s.nextBroadcastFetch)
        fetchBroadcastList({});
    if (!s.broadcastPending) return;
    if (versus::lounge::realPlayLayer() || nowMs() - s.broadcastPending->createdAt >= kBroadcastLiveMs) {
        s.broadcastPending.reset();
        return;
    }
    if (s.broadcastPresenter && s.broadcastPresenter(*s.broadcastPending)) s.broadcastPending.reset();
}
// 방송, 접속자 수, 시즌, Argon 상태 조회 및 읽음 처리 (읽은 공지 id는 모드 저장소에 저장).
std::vector<BroadcastInfo> const& Service::broadcasts() const {
    return state().broadcasts;
}
int Service::onlineCount() const {
    return state().onlineCount;
}
int Service::season() const {
    return state().season;
}
int64_t Service::seasonEndsAt() const {
    return state().seasonEndsAt;
}
bool Service::takeArgonNotice() {
    return std::exchange(state().argonNotice, false);
}
std::string Service::argonState() const {
#if VERSUS_ARGON
    return state().argonState;
#else
    return "off";
#endif
}
int Service::unreadBroadcasts() const {
    auto const seen = Mod::get()->getSavedValue<int64_t>("broadcast-seen", 0);
    return static_cast<int>(std::count_if(state().broadcasts.begin(), state().broadcasts.end(),
                                          [seen](BroadcastInfo const& info) { return info.id > seen; }));
}
void Service::markBroadcastsRead() {
    auto const& list = state().broadcasts;
    if (list.empty()) return;
    auto const newest = list.front().id;
    if (newest <= Mod::get()->getSavedValue<int64_t>("broadcast-seen", 0)) return;
    Mod::get()->setSavedValue<int64_t>("broadcast-seen", newest);
    (void)Mod::get()->saveData();
}
void Service::refreshBroadcasts(Done callback) {
    connect([callback = std::move(callback)](bool ok, std::string error) mutable {
        if (!ok) {
            callback(false, std::move(error));
            return;
        }
        fetchBroadcastList(std::move(callback));
    });
}
void Service::sendBroadcast(std::string message, Done callback) {
    auto body = Json::object();
    body["message"] = std::move(message);
    moderationRequest("PUT", "admin/broadcast", std::move(body),
                      [callback = std::move(callback)](std::optional<Json> value, std::string error) {
                          if (!value) {
                              callback(false, std::move(error));
                              return;
                          }
                          auto& s = state();
                          s.broadcastLatest = std::max(s.broadcastLatest, intAt(*value, "id"));
                          s.nextBroadcastFetch = Clock::now();
                          callback(true, {});
                      });
}
void Service::setBroadcastPresenter(std::function<bool(BroadcastInfo const&)> presenter) {
    state().broadcastPresenter = std::move(presenter);
}
// 점검 상태 조회 / presenter 등록.
std::optional<MaintenanceInfo> const& Service::maintenance() const {
    return state().maintenance;
}
std::string Service::maintenanceMessage() const {
    auto& s = state();
    if (!s.maintenance) return {};
    s.maintenanceSeen = true;
    return maintenanceText(*s.maintenance);
}
void Service::setMaintenancePresenter(std::function<bool(MaintenanceInfo const&)> presenter) {
    state().maintenancePresenter = std::move(presenter);
}
// 점검 상태 주기 처리: 30분이 지나면 상태를 버리고, 서버가 내려가 있으면 5초마다 가볍게 확인하며,
// 점검 배너를 한 번 띄운다. 점검이 끝났으면 복구 알림을 보낸다.
void tickMaintenance() {
    auto& s = state();
    if (s.maintenance) {
        if (Clock::now() - s.maintenanceSince > std::chrono::minutes(30)) {
            s.maintenance.reset();
            s.maintenanceBanner = -1;
            s.maintenanceSeen = false;
        } else {
            if (s.maintenance->down && Clock::now() >= s.maintenanceProbe) {
                s.maintenanceProbe = Clock::now() + std::chrono::seconds(5);
                request("GET", "release", {}, [](web::WebResponse) {});
            }
            if (s.maintenanceBanner != s.maintenance->startedAt && !versus::lounge::realPlayLayer() &&
                s.maintenancePresenter && s.maintenancePresenter(*s.maintenance)) {
                s.maintenanceBanner = s.maintenance->startedAt;
                s.maintenanceSeen = true;
            }
        }
    }
    if (s.maintenanceOver && !versus::lounge::realPlayLayer()) {
        s.maintenanceOver = false;
        Notification::create("Versus is back online", NotificationIcon::Success, 3.f)->show();
    }
}
// 방송 헤더가 40초 이상 오지 않으면 가벼운 요청으로 최신 방송 id를 갱신한다.
void Service::keepBroadcastsFresh() {
    auto& s = state();
    if (!connected() || s.broadcastFetching || versus::lounge::realPlayLayer() ||
        Clock::now() - s.broadcastHeard < std::chrono::seconds(40))
        return;
    s.broadcastHeard = Clock::now();
    request("GET", "broadcasts/latest", {}, [](web::WebResponse) {});
}
// 매 프레임 처리. 순서대로:
// 공지 / 점검 / 스태프 목록 갱신, 미뤄 둔 방 액션 1개 실행, 시도 기록(attempt) 업로드,
// 리플레이 업로드, 그 다음 Group / 봇 / 1v1 방별 폴링과 하트비트.
void Service::tick(float dt) {
    auto& s = state();
    tickBroadcasts();
    tickMaintenance();
    refreshStaffList();
    refreshTokenInBackground();
    if (!s.writing && !s.authenticating && !s.pendingActions.empty()) {
        auto next = std::move(s.pendingActions.front());
        s.pendingActions.pop_front();
        s.dispatchingAction = true;
        next();
        s.dispatchingAction = false;
    }
    // 2분 넘게 못 보낸 시도 기록은 버리고, 연결이 끊겼으면 10초 간격으로 재접속을 시도한다.
    // 서버가 영구 거부(4xx)하는 기록은 4번 거부되면 버린다.
    while (!s.attemptWriting && !s.attempts.empty() &&
           Clock::now() - s.attempts.front().queued > std::chrono::minutes(2))
        s.attempts.pop_front();
    if (!s.attemptWriting && !s.attempts.empty() && !connected() && !s.authenticating &&
        Clock::now() >= s.nextRecordsConnect) {
        s.nextRecordsConnect = Clock::now() + std::chrono::seconds(10);
        connect([](bool, std::string) {});
    }
    if (!s.attemptWriting && !s.attempts.empty() && connected() && Clock::now() >= s.nextAttemptWrite &&
        Clock::now() - s.attempts.front().queued >= std::chrono::milliseconds(1500)) {
        auto entry = s.attempts.front();
        {
            s.attemptWriting = true;
            auto data = Json::object();
            data["roomId"] = entry.room;
            data["run"] = entry.run;
            data["percent"] = entry.percent;
            data["endedAt"] = timestamp();
            request("PUT", "matchAttempts/" + entry.match + "/" + entry.uid + fmt::format("/r{}", entry.run), data,
                    [](web::WebResponse response) {
                        auto& s = state();
                        s.attemptWriting = false;
                        int const code = response.code();
                        bool const refused = code >= 400 && code < 500 && code != 408 && code != 429;
                        if (!s.attempts.empty()) {
                            if (response.ok())
                                s.attempts.pop_front();
                            else if (refused && ++s.attempts.front().rejected >= 4) {
                                log::warn("Versus attempt record r{} dropped: {}", s.attempts.front().run,
                                          networkError(response));
                                s.attempts.pop_front();
                            }
                        }
                        s.nextAttemptWrite = Clock::now() + (response.ok() ? std::chrono::milliseconds(0)
                                                                           : std::chrono::milliseconds(1500));
                    });
        }
    }
    // 리플레이 업로드: 실패하면 시도 횟수 제곱에 비례해 (최대 60초) 늦춰 재시도하고,
    // 서버가 거부하거나 30분이 지나면 포기한다.
    if (!s.replayWriting && !s.replays.empty() && !connected() && !s.authenticating &&
        Clock::now() >= s.replays.front().next && Clock::now() >= s.nextRecordsConnect) {
        s.nextRecordsConnect = Clock::now() + std::chrono::seconds(10);
        connect([](bool, std::string) {});
    }
    if (!s.replayWriting && !s.replays.empty() && connected() && Clock::now() >= s.replays.front().next) {
        auto& entry = s.replays.front();
        ++entry.tries;
        s.replayWriting = true;
        auto const& upload = entry.upload;
        auto body = Json::object();
        body["format"] = static_cast<int>(replay::kFormat);
        body["run"] = upload.run;
        body["percent"] = upload.percent;
        body["end"] = upload.end;
        body["steps"] = upload.steps;
        body["inputs"] = upload.inputs;
        body["level"] = upload.level;
        body["data"] = upload.data;
        if (upload.coins >= 0) body["coins"] = upload.coins;
        request(
            "PUT", "replays/" + upload.matchId, std::move(body),
            [](web::WebResponse response) {
                auto& s = state();
                s.replayWriting = false;
                if (s.replays.empty()) return;
                auto& entry = s.replays.front();
                bool const rejected =
                    response.code() >= 400 && response.code() < 500 && response.code() != 408 && response.code() != 429;
                bool const expired = Clock::now() - entry.queued > std::chrono::minutes(30);
                if (response.ok() || rejected || expired) {
                    if (!response.ok())
                        log::warn("Versus replay upload {} (match {}, attempt {}, {} chars): {}",
                                  rejected ? "rejected" : "gave up", entry.upload.matchId, entry.upload.run,
                                  entry.upload.data.size(), networkError(response));
                    else
                        log::info("Versus replay uploaded (match {}, attempt {})", entry.upload.matchId,
                                  entry.upload.run);
                    s.replays.pop_front();
                    return;
                }
                entry.next = Clock::now() + std::chrono::seconds(std::min(60, 5 * entry.tries * entry.tries));
            },
            {}, false, false, false, {}, {}, {}, 60, false, upload.roomId);
    }
    // Group 방이면 groupTick, 봇 전투면 botTick이 전담한다.
    if (s.group) {
        dt = std::clamp(dt, 0.f, 1.f);
        groupTick(dt);
        return;
    }
    if (s.botBattle) {
        botTick(std::clamp(dt, 0.f, 1.f));
        return;
    }
    if (!s.room || !s.room->battle || s.room->battle->id != s.returningMatch) s.returningMatch.clear();
    if (!s.room) return;
    // 1v1 방 처리. 마지막 응답 후 60초가 지나면 연결 끊김 처리 (점검 중이면 유예).
    dt = std::clamp(dt, 0.f, 1.f);
    s.pollTime += dt;
    s.heartbeatTime += dt;
    if (s.localRoom) s.lastContact = Clock::now();
    bool const updating = s.maintenance && Clock::now() - s.maintenanceSince < kMaintenanceWait;
    if (!updating && Clock::now() - s.lastContact > std::chrono::seconds(60)) {
        disconnected("Connection lost. Please rejoin the room.");
        return;
    }
    if (!connected() && !s.localRoom) {
        if (!s.authenticating && s.pollTime >= 2.f) {
            s.pollTime = 0;
            connect([](bool, std::string) {});
        }
        return;
    }
    // 호스트는 룰렛 연출이 끝나면 결과를 확정한다 (실패 시 2초 뒤 재시도).
    if (isHost() && drawingMap(*s.room) && !s.writing && !busy() && !s.polling &&
        nowMs() >= s.room->mapDraw->at + DRAW_END_MS && Clock::now() >= s.nextDrawAttempt) {
        s.nextDrawAttempt = Clock::now() + std::chrono::seconds(2);
        finishMapDraw(s.room->mapDraw->id, [](bool ok, std::string detail) {
            if (!ok) log::warn("Versus roulette finalization: {}", detail);
        });
        return;
    }
    // 전투가 끝났으면 내 경기 기록(history)을 서버에 한 번 저장한다.
    // 412(이미 기록됨)이면 같은 방의 기록인지 확인해 저장된 것으로 처리한다.
    if (!s.localRoom && s.room->guest && s.room->battle && s.room->battle->finishedAt > 0 && !s.historyWriting &&
        !s.recordedMatches.contains(s.room->battle->id) && Clock::now() >= s.nextHistoryAttempt) {
        auto const match = *s.room->battle;
        auto const room = *s.room;
        auto const host = isHost();
        auto history = Json::object();
        history["roomId"] = room.id;
        history["opponentName"] = host && room.guest ? room.guest->name : room.host.name;
        history["levelName"] = room.level.name;
        history["winnerName"] = match.draw                         ? "Draw"
                                : match.winnerUid == room.host.uid ? room.host.name
                                : room.guest                       ? room.guest->name
                                                                   : "Player";
        history["result"] = match.draw ? "draw" : match.winnerUid == s.profile.uid ? "win" : "loss";
        history["playedAt"] = timestamp();
        history["levelId"] = room.level.id;
        history["self"] = profileJson(host ? room.host : *room.guest);
        history["opponent"] = profileJson(host ? *room.guest : room.host);
        history["rules"] = rulesJson(room.rules);
        history["selfStats"] = battlePlayerJson(host ? match.host : match.guest);
        history["opponentStats"] = battlePlayerJson(host ? match.guest : match.host);
        s.historyWriting = true;
        request(
            "PUT", "history/" + s.profile.uid + "/" + match.id, history,
            [id = match.id, roomId = room.id, uid = s.profile.uid](web::WebResponse response) {
                auto& s = state();
                s.historyWriting = false;
                s.nextHistoryAttempt = Clock::now() + std::chrono::seconds(4);
                if (response.ok()) {
                    s.recordedMatches.insert(id);
                    s.historyError.clear();
                } else
                    s.historyError = response.code() == 403 || response.code() == 401
                                         ? "Update Firebase rules to save match details."
                                         : "Retrying match history save...";
                if (!response.ok() && response.code() == 412) {
                    s.historyWriting = true;
                    request("GET", "history/" + uid + "/" + id, {}, [id, roomId](web::WebResponse check) {
                        auto parsed = check.json();
                        auto& s = state();
                        s.historyWriting = false;
                        if (check.ok() && parsed && stringAt(parsed.unwrap(), "roomId") == roomId &&
                            intAt(parsed.unwrap(), "playedAt") > 0) {
                            s.recordedMatches.insert(id);
                            s.historyError.clear();
                        }
                    });
                }
            },
            "null_etag");
    }
    // 결과 화면을 확인한 뒤 방 JSON에 내 쪽 복귀 표시(hostReturned / guestReturned)를 남긴다.
    // 기록이 저장됐거나 8초가 지나면 보낸다.
    if (!s.returningMatch.empty() && !s.writing && !s.polling && !busy() && Clock::now() >= s.nextReturnAttempt &&
        (s.recordedMatches.contains(s.returningMatch) ||
         Clock::now() - s.returnRequestedAt >= std::chrono::seconds(8))) {
        auto const epoch = s.epoch;
        auto const match = s.returningMatch;
        s.writing = true;
        mutateRoom(
            s.room->id, epoch, "return-result",
            [match](Json& body) -> std::string {
                auto& battle = body["battle"];
                if (!boolAt(body, "started") && !battle.isObject()) return ROOM_UNCHANGED;
                if (stringAt(battle, "id") != match) return "Match session changed.";
                if (intAt(battle, "finishedAt") <= 0) return "Match is still active.";
                auto const uid = state().profile.uid;
                bool host = stringAt(std::as_const(body)["host"], "uid") == uid;
                if (!host && stringAt(std::as_const(body)["guest"], "uid") != uid)
                    return "Your room connection expired.";
                auto const flag = host ? "hostReturned" : "guestReturned";
                if (boolAt(battle, flag)) return ROOM_UNCHANGED;
                battle[flag] = true;
                body[host ? "hostSeen" : "guestSeen"] = timestamp();
                return {};
            },
            [epoch, match](bool ok, std::string error) {
                auto& s = state();
                if (epoch != s.epoch) return;
                s.writing = false;
                s.nextReturnAttempt = Clock::now() + std::chrono::seconds(1);
                if (ok && s.returningMatch == match) s.returningMatch.clear();
                if (!ok) log::warn("Versus result return will retry: {}", error);
            });
    }
    // 양쪽이 모두 복귀했으면 전투를 취소해 대기실로 되돌린다.
    // 내 쪽만 복귀했고 종료 후 25초가 지나도록 상대가 복귀하지 않으면 (Ranked 제외) 같은 방식으로 정리한다.
    if (!s.writing && !s.polling && !busy() && s.room->battle && s.room->battle->finishedAt > 0 &&
        s.room->battle->hostReturned && s.room->battle->guestReturned) {
        if (Clock::now() >= s.nextReturnCancel) {
            s.nextReturnCancel = Clock::now() + std::chrono::seconds(1);
            cancelLaunch(s.room->battle->id, [](bool, std::string) {});
        }
    } else if (!s.writing && !s.polling && !busy() && s.rankedMatch.empty() && !s.room->id.starts_with("ranked-") &&
               s.room->battle && s.room->battle->finishedAt > 0 && Clock::now() >= s.nextGhostReset &&
               (isHost() ? s.room->battle->hostReturned : s.room->battle->guestReturned) &&
               serverNow() - s.room->battle->finishedAt >= 25'000) {
        s.nextGhostReset = Clock::now() + std::chrono::seconds(3);
        cancelLaunch(s.room->battle->id, [](bool, std::string) {});
    }
    // 예약된 포기가 있으면 같은 전투가 진행 중인 동안 2초 간격으로 보고한다.
    if (s.pendingForfeit) {
        bool const same =
            s.room->battle && s.room->battle->id == s.pendingForfeitMatch && s.room->battle->finishedAt == 0;
        if (!same) {
            s.pendingForfeit.reset();
            s.pendingForfeitMatch.clear();
        } else if (!s.writing && !s.polling && !busy() && Clock::now() >= s.nextForfeitAttempt) {
            s.nextForfeitAttempt = Clock::now() + std::chrono::seconds(2);
            reportBattle(*s.pendingForfeit, false, [match = s.pendingForfeitMatch](bool ok, std::string) {
                auto& state = versus::state();
                if (ok && state.pendingForfeitMatch == match) {
                    state.pendingForfeit.reset();
                    state.pendingForfeitMatch.clear();
                }
            });
        }
    }
    // 상대의 일시정지 시간이 소진됐으면 내 상태를 보고해 서버가 승패를 판정하게 한다.
    if (!s.writing && !s.polling && s.pollTime < .5f && s.heartbeatTime < 10.f && s.room->battle &&
        s.room->battle->finishedAt == 0 && s.room->launch && s.room->launch->releasedAt > 0 &&
        Clock::now() >= s.nextPauseSettle) {
        auto const& other = isHost() ? s.room->battle->guest : s.room->battle->host;
        if (other.paused && battle::pauseRemaining(other, nowMs()) == 0) {
            s.nextPauseSettle = Clock::now() + std::chrono::seconds(1);
            auto own = isHost() ? s.room->battle->host : s.room->battle->guest;
            reportBattle(own, false, [](bool, std::string) {});
        }
    }
    // 주기 선택: 보고 요청이 있으면 대기, 10초마다 heartbeat(전투 보고가 최근 8초 안에
    // 있었으면 heartbeat를 대신함), 전투 중이면 1초마다 전투 폴링, 그 외에는 방 폴링과 다운로드 보고.
    bool const activeBattle = s.room->started && s.room->battle && s.room->launch && s.room->launch->releasedAt > 0;
    bool const reportsKeepSeat = activeBattle && s.room->battle->finishedAt == 0 && !s.room->rules.allowHacks &&
                                 Clock::now() - s.lastBattleReport < std::chrono::seconds(8);
    if (s.heartbeatTime >= 10.f && reportsKeepSeat) s.heartbeatTime = 0.f;
    bool const reportWanted = activeBattle && s.room->battle->finishedAt == 0 &&
                              s.battleReportWantedAt != Clock::time_point{} &&
                              Clock::now() - s.battleReportWantedAt < std::chrono::seconds(3) && !s.searchingMap &&
                              s.pendingActions.empty() && !s.authenticating;
    if (reportWanted) {
    } else if (s.heartbeatTime >= 10.f)
        heartbeat();
    else if (activeBattle) {
        if (s.pollTime >= 1.f) pollBattle();
    } else if (s.pollTime >= firebase_config::ROOM_POLL_INTERVAL_SECONDS)
        pollRoom();
    else
        reportDownload();
}
// 화면에 보여 줄 안내 문구를 한 번 꺼내고 비운다. 아래는 Group 결과 표시 / 관전 상태 접근자.
std::string Service::takeNotice() {
    return std::exchange(state().notice, {});
}
void Service::markGroupResultShown(std::string battleId) {
    state().groupResultShown = std::move(battleId);
}
void Service::setGroupSpectating(bool on) {
    state().groupSpectating = on;
    state().pollTime = kGroupBattlePoll;
}
bool Service::groupSpectating() const {
    return state().groupSpectating;
}
bool Service::groupResultShown(std::string const& battleId) const {
    return state().groupResultShown == battleId;
}
}
