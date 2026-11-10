// Versus 서버 클라이언트(Service)의 공개 헤더.
// 서버와 주고받는 데이터 구조(프로필, 맵, 규칙, 1v1 방, Group 방, Ranked/Season 2, Live 관전,
// 신고/제재/스태프 도구 등)와 이를 다루는 싱글톤 Service 클래스를 선언한다.
// 구현은 VersusService.cpp(및 GroupService.inl 등 include 파일)에 있다.
#pragma once
#include "Cosmetics.hpp"
#include "Achievements.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <utility>
#include <string>
#include <vector>

namespace versus {
// 서버에 등록된 플레이어 프로필 (uid는 서버가 발급한 식별자, accountId는 GD 계정 id).
struct PlayerProfile {
    std::string uid;
    std::string name;
    int accountId = 0;
    int icon = 1;
    int color1 = 0x007BFF;
    int color2 = 0xFFFFFF;
    // 1.0.7: GD의 Glow 설정 - 켜져 있으면 그 외곽선 색(RGB), 꺼져 있으면 -1. 서버 프로필의 선택 키 "glow"
    // (없으면 끔; 1.0.6 이하 게임은 보내지 않으므로 그 플레이어는 Glow 없이 그려진다).
    int glow = -1;
    double winRate = 0;
    int recentGames = 0;
    bool membership = false;
    Cosmetics cosmetics = defaultCosmetics(false);
    bool hasCosmetics = false;
};
// 방/매치에서 쓰는 맵(레벨) 정보. id가 0이면 아직 정해지지 않은 상태. length는 -1이면 알 수 없음.
struct LevelInfo {
    int64_t id = 0;
    std::string name;
    int difficulty = 0;
    int stars = 0;
    bool demon = false;
    bool autoLevel = false;
    bool platformer = false;
    std::string creator;
    int featureState = 0;
    int userCoins = 0;
    int length = -1;
    bool operator==(LevelInfo const&) const = default;
};
// 맵 길이 필터 비트 마스크의 전체 값(5개 비트), 좋아요 필터의 최대값.
inline constexpr int kLengthsAll = 31;
inline constexpr int kLikesMax = 1'000'000;
// 방장이 고르는 맵 선택 조건 (랜덤 여부, 난이도 마스크, 길이, 평점 여부, 2P 포함 여부, 최소 좋아요).
struct MapSelection {
    bool random = false;
    int mask = 1;
    bool platformer = false;
    int lengths = kLengthsAll;
    bool rated = true;
    int twoPlayer = 0;
    int minLikes = 0;
    bool operator==(MapSelection const&) const = default;
};
// Map Pool 관련 상수: 전체 난이도 마스크, 1인당 최대 맵 수, 풀 최대 맵 수, 방 밴 목록 최대 수.
inline constexpr int kPoolMaskAll = 8191;
inline constexpr int kPoolPerPlayerMax = 10;
inline constexpr size_t kPoolMapsMax = 50;
inline constexpr size_t kRoomBannedMax = 50;
// Map Pool 설정: 1인당 추가 가능한 맵 수와 풀에 들어올 수 있는 맵의 조건.
struct MapPoolSettings {
    int perPlayer = 3;
    bool rated = false;
    int mask = kPoolMaskAll;
    int twoPlayer = 0;
    bool random = false;
    int lengths = kLengthsAll;
    int minLikes = 0;
    bool filter = false;
    bool operator==(MapPoolSettings const&) const = default;
};
// Map Pool에 들어 있는 맵 한 개와 추가한 사람(by / name).
struct MapPoolEntry {
    LevelInfo level;
    bool twoPlayer = false;
    std::string by, name;
};
// 방의 Map Pool. permit은 방장 외에 맵을 추가할 수 있도록 허가된 uid 목록이다.
struct MapPool {
    MapPoolSettings settings;
    std::vector<std::string> permit;
    std::vector<MapPoolEntry> maps;
    bool permitted(std::string const& uid, std::string const& host) const {
        return uid == host || std::find(permit.begin(), permit.end(), uid) != permit.end();
    }
    int addedBy(std::string const& uid) const {
        return static_cast<int>(
            std::count_if(maps.begin(), maps.end(), [&](auto const& entry) { return entry.by == uid; }));
    }
};
// 맵 추첨(draw) 상태: 후보 레벨 목록과 선택된 인덱스, 확정 여부.
struct MapDraw {
    std::string id;
    int64_t at = 0;
    int selected = 0;
    bool settled = false;
    std::vector<LevelInfo> levels;
};
// 전투 시작 준비 상태. 양쪽이 맵을 불러오면 releasedAt이 채워져 동시에 시작한다 (서버 시각, ms).
struct LaunchInfo {
    std::string id;
    int64_t requestedAt = 0;
    int64_t releasedAt = 0;
    bool hostLoaded = false;
    bool guestLoaded = false;
};
// 전투 규칙. mode 0은 목숨제(attempts), 1은 목표 퍼센트제(targetPercent). speed는 % 단위(50~300, 25 간격).
struct GameRules {
    int mode = 0;
    int attempts = 3;
    int targetPercent = 40;
    bool practice = false;
    bool allowHacks = false;
    int speed = 100;
    bool autoLives = false;
    bool operator==(GameRules const&) const = default;
    // 목숨제 + 연습 아님일 때만 목숨 수가 맵 길이를 따라간다 (autoLives).
    bool livesFollowMap() const { return autoLives && mode == 0 && !practice; }
};
// 허용되는 게임 속도인지 검사.
inline bool validRuleSpeed(int speed) {
    return speed >= 50 && speed <= 300 && speed % 25 == 0;
}
// 맵 정보에서 자동 목숨 수를 계산한다.
int autoRoomLives(LevelInfo const& level);
// 방에서 보낸 이모트 한 건. nonce는 중복 표시를 막기 위한 고유값.
struct EmoteInfo {
    std::string uid;
    std::string kind;
    std::string text;
    int64_t at = 0;
    std::string nonce;
};
// 전투 중 한 플레이어의 진행 상태. 서버에 주기적으로 보고하고 상대 쪽에서 받아 표시한다.
// 위치(x, y, camera)와 updatedAt은 관전/실시간 보기에 쓰이고, integrity* 필드는 무결성 검사 결과이다.
struct BattlePlayerState {
    int attemptsUsed = 0;
    int bestPercent = 0;
    int currentPercent = 0;
    int runNumber = 0;
    bool inAttempt = false;
    bool cleared = false;
    int userCoins = 0;
    bool forfeited = false;
    bool spectating = false;
    bool paused = false;
    int64_t pausedAt = 0;
    int64_t pauseUsedMs = 0;
    int64_t pauseSeq = 0, pauseChangedAt = 0;
    double x = 0;
    double y = 0;
    double cameraX = 0;
    double cameraY = 0;
    int64_t updatedAt = 0;
    std::string integrityCode;
    std::string integrityFeatures;
    int tps = 0;
};
// 게임 기준 TPS와 기록 목록 길이 제한 (일반 / 관리자).
inline constexpr int kGameTps = 240;
inline constexpr size_t kHistoryLimit = 100;
inline constexpr size_t kAdminHistoryLimit = 500;
// 지난 경기 한 건의 기록. 상세(detailed) 정보와 Ranked 레이팅 변화 필드를 함께 담는다.
struct MatchRecord {
    std::string id, opponentName, levelName, winnerName, result;
    std::string roomId;
    int64_t playedAt = 0, levelId = 0;
    PlayerProfile self, opponent;
    GameRules rules;
    BattlePlayerState selfStats, opponentStats;
    bool detailed = false;
    bool ranked = false, duel = false, ratingKnown = false;
    int ratingChange = 0, newRating = 0;
    bool canceled = false;
    int selfBefore = -2, opponentBefore = -2;
};
// 리플레이/시도 기록 구조: 시도 한 번, 서버에 올라간 리플레이 요약, 업로드 요청, 내려받은 리플레이 파일.
struct AttemptRecord {
    int run = 0;
    int percent = 0;
    int64_t ms = -1;
    bool fast = false;
    int length = -1;
    std::string outcome;
};
struct ReplaySummary {
    std::string uid, side, end;
    int run = 0, percent = 0;
    int64_t steps = 0, bytes = 0, at = 0;
};
struct ReplayUpload {
    std::string matchId, end, data;
    int run = 0, percent = 0, coins = -1;
    int64_t steps = 0, inputs = 0, level = 0;
    std::string roomId;
};
struct ReplayFile {
    std::string uid, name, levelName, end, data;
    int run = 0, percent = 0, serverBest = -1, coins = -1, serverCoins = -1;
    int64_t level = 0, at = 0;
};
using AttemptsCallback = std::function<void(std::vector<AttemptRecord>, std::vector<AttemptRecord>, std::string)>;
// 1v1 전투의 진행/결과 상태. hostReroll / guestReroll은 맵 재추첨 투표 시각이다.
struct BattleInfo {
    std::string id;
    std::string firstReachedUid;
    int opponentRunAtFirst = 0;
    std::string winnerUid;
    bool draw = false;
    bool hostReturned = false, guestReturned = false;
    int64_t hostReroll = 0, guestReroll = 0;
    bool rerolled = false;
    int64_t finishedAt = 0;
    BattlePlayerState host;
    BattlePlayerState guest;
};
// 맵 재추첨 투표 유효 시간(ms).
inline constexpr int64_t kRerollVoteWindowMs = 20'000;
// 1v1 방의 전체 상태. Group 방도 화면 재사용을 위해 이 구조로 변환되어 쓰인다 (syncGroupRoom).
struct RoomInfo {
    std::string id;
    std::string name;
    bool privateRoom = false;
    PlayerProfile host;
    std::optional<PlayerProfile> guest;
    LevelInfo level;
    struct DownloadInfo {
        std::string uid;
        int64_t levelId = 0, updatedAt = 0;
        int percent = 0;
        bool mapReady = false;
        int songsDone = 0, songsTotal = 0, soundsDone = 0, soundsTotal = 0;
        int best = -1;
    };
    DownloadInfo hostDownload, guestDownload;
    MapSelection mapSelection;
    std::optional<MapDraw> mapDraw;
    int hostWins = 0, guestWins = 0;
    std::string scoredMatch;
    GameRules rules;
    bool hostReady = false;
    bool guestReady = false;
    std::optional<EmoteInfo> emote;
    int64_t hostEmoteAt = 0;
    int64_t guestEmoteAt = 0;
    bool started = false;
    std::optional<LaunchInfo> launch;
    std::optional<BattleInfo> battle;
    int64_t updatedAt = 0;
    std::map<std::string, std::vector<std::pair<std::string, std::string>>> hacks;
    bool debug = false;
    std::optional<MapPool> mapPool;
};
// Group 방 인원 상한/하한, 시작에 필요한 준비 인원(과반, 최소 2명), 이모트 쿨다운(초).
inline constexpr int kGroupMaxPlayers = 75;
inline constexpr int kGroupMinPlayers = 2;
inline int groupReadyNeeded(int members) {
    return std::max(kGroupMinPlayers, (members + 1) / 2);
}
inline constexpr float kGroupEmoteCooldown = 1.5f;
// Group 방 멤버. download는 맵 다운로드 진행률(%, -1이면 정보 없음), best는 그 맵의 로컬 최고 %.
struct GroupMember {
    PlayerProfile profile;
    bool ready = false;
    int wins = 0;
    int download = -1;
    int best = -1;
};
// Group 전투의 좌석 한 개: 프로필과 진행 상태, 로드 완료 / 실격(DQ) / 탈락 여부.
struct GroupSeat {
    PlayerProfile profile;
    BattlePlayerState state;
    bool loaded = false, disqualified = false;
    bool out = false;
};
// Group 전투 전체 상태. ranking / first는 좌석 인덱스 목록이며 statesFinal이면 최종 상태를 받은 것이다.
struct GroupBattle {
    std::string id;
    int64_t requestedAt = 0, releasedAt = 0, finishedAt = 0;
    std::string winnerUid;
    bool draw = false;
    LevelInfo level;
    GameRules rules;
    std::vector<GroupSeat> seats;
    std::vector<int> ranking;
    std::vector<int> first;
    bool statesFinal = false;
    int seatOf(std::string const& uid) const {
        for (size_t i = 0; i < seats.size(); ++i)
            if (seats[i].profile.uid == uid) return static_cast<int>(i);
        return -1;
    }
};
// Group 방 전체 상태 (멤버, 규칙, 현재 전투, 시작 카운트다운 시각 startAt 등).
struct GroupRoomInfo {
    std::string id, name, hostUid;
    bool privateRoom = false;
    int maxPlayers = kGroupMaxPlayers;
    std::vector<GroupMember> members;
    LevelInfo level;
    GameRules rules;
    std::optional<EmoteInfo> emote;
    std::vector<EmoteInfo> emotes;
    std::optional<GroupBattle> battle;
    MapSelection mapSelection;
    std::optional<MapDraw> mapDraw;
    int64_t startAt = 0;
    bool debug = false;
    std::optional<MapPool> mapPool;
    bool running() const { return battle && battle->finishedAt == 0; }
    GroupMember const* member(std::string const& uid) const {
        for (auto const& entry : members)
            if (entry.profile.uid == uid) return &entry;
        return nullptr;
    }
};
// Group 방 목록의 한 줄 요약.
struct GroupSummary {
    std::string id, name;
    bool privateRoom = false, started = false;
    PlayerProfile host;
    int count = 0, maxPlayers = kGroupMaxPlayers;
    LevelInfo level;
    GameRules rules;
    bool random = false;
    bool debug = false;
};
// Ranked 티어 구분: 티어별 상위 % 및 레이팅 구간, 전체 인원.
struct RankedTierCutoff {
    std::string tier;
    int topPercent = 0;
    std::optional<int> minRating, maxRating;
};
struct RankedTiers {
    int population = 0;
    std::vector<RankedTierCutoff> tiers;
};
// 다음 티어 이름, 컷오프, 필요한 점수를 계산해 돌려준다.
struct NextRankedTier {
    std::string tier;
    int cutoff = 0, points = 0;
};
std::optional<NextRankedTier> nextRankedTier(RankedTiers const& tiers, std::string const& tier, double rating);
// Ranked 프로필 (레이팅, 전적, 티어, 배치 진행도, 제재 상태 등). rating / ratingDeviation / volatility는 레이팅 시스템 값이다.
struct RankedProfile {
    PlayerProfile player;
    double rating = 1500.;
    double ratingDeviation = 350.;
    double volatility = .06;
    int matches = 0, wins = 0, losses = 0;
    int placementCompleted = 0, placementRequired = 0;
    int globalRank = 0, population = 0;
    double percentile = 100.;
    std::string tier = "Insane";
    std::string tierName;
    bool placed = true;
    bool allowDuel = true;
    bool duelAnonymous = true;
    bool searching = true;
    int winStreak = 0;
    std::string emoteKind;
    int64_t emoteAt = 0;
    bool banned = false, banPermanent = false;
    int64_t banUntil = 0;
    std::string banReason;
};
// Ranked 경기 결과 (레이팅/티어/순위 변화, 연승 보너스, 패배 감면 등).
struct RankedResult {
    std::string matchId, winnerUid, result, oldTier, newTier;
    std::string oldTierName, newTierName;
    std::string disqualifiedUid, integrityCode, integrityFeatures;
    // 1.0.7: 무승부 카드에서 뒤로 가기로 재대결을 사양한 쪽의 uid(서버 rematchDeclined). 비어 있으면 없음.
    std::string rematchDeclined;
    bool draw = false;
    double oldRating = 1500., newRating = 1500.;
    int oldGlobalRank = 0, newGlobalRank = 0;
    double oldPercentile = 100., newPercentile = 100.;
    int placementBefore = 0, placementAfter = 0;
    int newPopulation = 0;
    bool placementComplete = false;
    int winStreak = 0;
    double streakBonus = 0.;
    bool mapKnown = false;
    int mapBest = -1, opponentMapBest = -1;
    double lossReduction = 0.;
    int fullLoss = 0;
    bool duel = false;
};
// 이번 매치에서 나올 맵 범위 (티어별 마스크와 근거).
struct RankedMapBand {
    std::string tier, reason;
    std::string tierName;
    int mask = 0;
    std::optional<int> topFrom, topTo;
    std::optional<int> ratingMin, ratingMax;
    int population = 0;
    std::optional<uint32_t> roomMain, roomSub;
};
// 맵 다운로드 실패(회피) 누적 상태와 페널티 여부.
struct RankedDownloadFail {
    int count = 0, free = 2, weight = 2, stack = 0;
    int64_t resetAt = 0;
    bool dodged = false, penalized = false;
    bool noDodge = false;
    std::string reason;
};
// Ranked 매치 한 건의 상태: 후보 맵, 선택된 맵, 준비 상태, 재추첨 투표, 밴, 결과 등.
struct RankedMatch {
    std::string id, roomId, state, cancelReason, drawId, rerollReason;
    std::optional<RankedDownloadFail> downloadFail;
    std::optional<RankedMapBand> mapBand;
    std::vector<int64_t> failedMaps;
    std::vector<int64_t> playedMaps;
    int rematchRound = 0;
    int64_t createdAt = 0, selectedAt = 0;
    int mapMask = 1, maxAttempts = 3;
    bool candidateProvider = false, ownReady = false, opponentReady = false;
    bool ownLdm = false, opponentLdm = false;
    bool duel = false;
    int64_t startAt = 0;
    bool ownReroll = false, opponentReroll = false, quickReroll = false, rerolled = false;
    int voteRerolls = 0;
    RankedProfile opponent;
    std::vector<LevelInfo> candidates;
    std::optional<LevelInfo> selectedMap;
    std::optional<RankedResult> result;
    std::vector<int> mapLengths;
    std::vector<int> mapFeatures;
    int minLikes = 0, maxSeconds = 0;
    bool noTwoPlayer = false;
    std::vector<int> gddlPool;
    std::string gddlCheckKey;
    std::vector<int> gddlCheck;
    int gddlLow = -1, gddlHigh = -1;
    std::map<int64_t, int> gddlTiers;
    int side = 0;
    // Season 2 맵 밴 단계 상태 (종료 시각, 제한 시간(초), 각자 밴한 맵).
    struct Bans {
        int64_t endsAt = 0;
        int seconds = 20;
        int64_t own = 0, opponent = 0;
        bool ownRandom = false, opponentRandom = false;
    };
    std::optional<Bans> bans;
};
// Ranked 듀얼(친선 도전) 요청.
struct RankedDuelRequest {
    std::string id, state;
    int64_t createdAt = 0, expiresAt = 0, decidedAt = 0;
    std::optional<RankedProfile> player;
    bool anonymous = false;
    std::string cup, cupName, tier, tierName, reason;
};
// Season 2 듀얼 상대 검색 결과 한 명.
struct Season2DuelPlayer {
    std::string uid, name, status, reason;
    bool available = false;
    std::optional<PlayerProfile> player;
    int64_t lastDuel = 0;
};
// 매치 회피(dodge) 누적 상태: 스택, 한도, 차단 시간(분), 페널티 등.
struct RankedDodge {
    bool known = false;
    int stack = 0, limit = 3, afterMap = 2, cleanMaps = 0, clearMaps = 5;
    int blockMinutes = 20, penaltyLosses = 2, penalties = 0, penaltyDelta = 0;
    int64_t blockedUntil = 0, penaltyAt = 0;
    int downloadFree = 2, downloadWeight = 2;
    bool season2 = false;
    std::string penaltyCup;
};
// 내 Ranked 상태 전체: 프로필, 대기열 정보, 진행 중인 매치, 듀얼 요청, 시즌과 컵.
struct RankedState {
    RankedProfile profile;
    int searchingPlayers = 0, playingPlayers = 0;
    int64_t queuedAt = 0;
    int64_t blockedUntil = 0;
    int cancelStrikes = 0;
    RankedDodge dodge;
    int searchRange = 100;
    int searchScale = 100;
    std::string searchCancelOutcome;
    int searchCancelsLeft = -1;
    // 1.0.7: 상대가 무승부 카드에서 뒤로 가기로 재대결을 사양함(서버 rematchGone: 그 무승부 매치 id와 안내 문구).
    std::string rematchGoneMatch, rematchGoneText;
    bool allowDuel = true;
    std::optional<RankedDuelRequest> duelIncoming, duelOutgoing;
    std::vector<RankedProfile> searchers;
    std::optional<RankedMatch> match;
    int season = 1;
    std::string cup;
    int ratingMax = 0;
    std::string matchCup;
};
// Season 2 컵(리그) 구조: 컵 목록, 티어 표, 점수 규칙.
struct Season2Cup {
    std::string id, name;
    RankedProfile profile;
    int online = -1;
};
struct Season2TierRow {
    std::string tier, name;
    int minRating = 0, maxRating = 0, topRank = 0;
    double topPercent = 0.;
    int mask = 0, lives = 0;
    int gddlLow = -1, gddlHigh = -1;
    std::vector<int> lengths, features;
};
struct Season2PointsRow {
    int from = 0, below = 0, k = 0;
    std::vector<int> win, loss;
};
struct Season2Points {
    std::vector<int> diffs;
    std::vector<Season2PointsRow> rows;
    int placementGames = 0, placementK = 0, newPlayerMatches = 0, newPlayerK = 0;
    std::vector<std::pair<int, int>> streak;
    double mapLossMax = 0., upsetLossScale = 1.;
};
struct Season2Tiers {
    std::string cup, cupName;
    int population = 0, placementGames = 5, ratingMin = 0, ratingMax = 20000;
    std::vector<int> livesByLength;
    std::optional<Season2Points> points;
    Season2TierRow placement;
    std::vector<Season2TierRow> tiers;
};
// Plaza(라운지) 구조: 걷는 플레이어들의 위치 샘플을 주고받는 요청/응답.
struct PlazaSample {
    int x = 0, y = 0;
    int rotation = 0;
    bool left = false;
    bool operator==(PlazaSample const&) const = default;
};
struct PlazaWalker {
    int id = 0;
    std::optional<RankedProfile> info;
    int64_t first = -1;
    std::vector<PlazaSample> samples;
    int64_t eventNonce = 0;
    std::string eventKind, eventText;
};
struct PlazaAnswer {
    int64_t epoch = 0;
    int64_t tick = 0;
    int everyMs = 200;
    int maxSamples = 10;
    bool needProfile = false;
    std::vector<int> ids;
    std::vector<PlazaWalker> changes;
};
struct PlazaUpdate {
    int64_t since = 0;
    int64_t first = 0;
    std::vector<PlazaSample> samples;
    std::string eventKind, eventText;
    bool withProfile = false;
};
// Live 관전 구조: 보낼 데이터, 보고 싶은 구간, 대상(방/전투), 서버 응답, 목록 항목.
struct LiveSend {
    int run = 0, index = 0;
    std::string data;
};
struct LiveWatch {
    int run = 0, from = 0;
};
struct LiveTarget {
    std::string roomId, battleId;
    bool group = false;
    std::string of;
    bool empty() const { return roomId.empty() || battleId.empty(); }
    std::string key() const { return std::string(group ? "g|" : "r|") + roomId + "|" + battleId + "|" + of; }
};
// 관전이 거부되었음을 나타내는 오류 문자열.
inline constexpr char const* kLiveRefused = "live-refused";
struct LiveAnswer {
    int run = 0, next = 0;
    bool watchedMe = false;
    bool watched = false;
    int watchRun = 0, watchFrom = 0, watchNext = 0, latest = 0;
    std::vector<std::string> parts;
};
struct LivePlayer {
    PlayerProfile profile;
    BattlePlayerState state;
    std::string tier;
    int globalRank = 0;
};
struct LiveMatch {
    std::string roomId, name;
    std::string battleId;
    int avgRank = 0;
    bool ranked = false;
    LevelInfo level;
    GameRules rules;
    int64_t releasedAt = 0;
    int scoreHost = 0, scoreGuest = 0;
    std::array<LivePlayer, 2> players;
};
struct LiveGroup {
    std::string roomId, name;
    std::string battleId;
    int avgRank = 0;
    LevelInfo level;
    GameRules rules;
    int64_t releasedAt = 0;
    int count = 0, alive = 0;
    std::vector<LivePlayer> top;
    std::vector<std::string> names;
};
// 맵 투표(좋아요/싫어요) 결과와 내 투표, 기록.
struct MapVotes {
    int up = 0, down = 0, mine = 0;
};
struct MapVoteRecord {
    int64_t levelId = 0;
    std::string name;
    int vote = 0;
    int64_t updatedAt = 0;
};
using MapVotesCallback = std::function<void(std::map<int64_t, MapVotes>, std::string)>;
// Live 전투 보기 응답, 라이브 목록 응답, Group 관전 응답.
struct LiveBattleView {
    bool ok = false;
    bool removed = false;
    LiveMatch match;
    int64_t finishedAt = 0;
    std::string winnerUid;
    bool draw = false;
};
struct LiveList {
    std::vector<LiveMatch> matches;
    std::vector<LiveGroup> groups;
    int count = 0;
};
struct GroupWatch {
    bool ok = false;
    bool closed = false;
    bool changed = false;
    std::string etag, roomName;
    GroupBattle battle;
};
// 업적 달성률(전체 플레이어 중 해금 비율)과 개별 업적 진행/보유 칭호 상태.
struct AchievementRates {
    int players = 0;
    std::map<std::string, int> unlocked;
    float share(std::string const& id) const {
        if (players <= 0) return 0.f;
        auto const found = unlocked.find(id);
        return found == unlocked.end() ? 0.f : std::clamp(static_cast<float>(found->second) / players, 0.f, 1.f);
    }
};
struct AchievementState {
    std::string uid;
    std::array<AchievementProgress, kAllAchievementCount> entries{};
    std::vector<int> titles{0};
    int unlocked = 0, games = 0, wins = 0;
    bool season2 = false;
    int s1Final = 0;
    bool ownsTitle(int title) const { return std::find(titles.begin(), titles.end(), title) != titles.end(); }
};
// 콜백 타입 별칭 모음 (Done은 성공 여부와 오류 문자열).
using Done = std::function<void(bool, std::string)>;
using RoomListCallback = std::function<void(std::vector<RoomInfo>, std::string)>;
using HistoryCallback = std::function<void(std::vector<MatchRecord>, std::string)>;
using RankedStateCallback = std::function<void(std::optional<RankedState>, std::string)>;
// 제재/신고/관리 도구에서 쓰는 구조들: 밴, 이의 제기(appeal), 신고, 환급, 회피 로그, 관리자 조작 기록, 계정 선택.
struct BanInfo {
    std::string uid, name, reason;
    int accountId = 0;
    bool permanent = false;
    int64_t until = 0, createdAt = 0, remainingMs = 0;
};
struct BanAppeal {
    int64_t id = 0, createdAt = 0, answeredAt = 0;
    std::string state, text, answer;
    std::string uid, name, banReason, answeredBy;
    int accountId = 0;
    int64_t banUntil = 0;
    bool banPermanent = false, banActive = false;
};
struct AppealStatus {
    std::optional<BanAppeal> latest;
    int left = 0;
    int64_t nextAt = 0;
    bool canAppeal = false;
};
struct BanHistory {
    struct Reason {
        std::string reason;
        int count = 0;
        int64_t lastAt = 0, lastDurationMs = 0;
        bool lastPermanent = false;
    };
    struct Entry {
        std::string reason;
        int64_t at = 0, durationMs = 0;
        bool permanent = false;
    };
    int total = 0, reportBans = 0;
    std::vector<Reason> reasons;
    std::vector<Entry> bans;
    bool banned = false, permanent = false;
    int64_t until = 0;
};
struct PlayerReport {
    int64_t id = 0, createdAt = 0;
    std::string reporterUid, reporterName, reason, detail, matchId;
    int reporterAccountId = 0;
    bool reporterReportBanned = false;
};
struct ReportedPlayer {
    std::string uid, name;
    int accountId = 0, count = 0;
    int64_t latestAt = 0;
    std::optional<BanInfo> ban;
    std::vector<PlayerReport> reports;
    std::string state, outcome, handledBy;
    int64_t handledAt = 0, banUntil = -1;
    std::string note;
};
struct RatingRefund {
    int64_t id = 0, playedAt = 0, createdAt = 0;
    double amount = 0.;
    std::string opponent, reason;
};
struct DodgeLogEntry {
    int64_t id = 0, at = 0;
    std::string uid, name, opponentUid, opponentName, cause, state, reason;
    std::string cup, cupName;
    int weight = 0, stackAfter = 0, ratingDelta = 0;
    bool penalized = false, mapShown = false, duel = false;
    std::optional<LevelInfo> map;
};
struct AdminAction {
    std::string kind, reason, adminName;
    double amount = 0., before = 0., after = 0.;
    int64_t at = 0;
};
struct AdminRankedRecord {
    std::string uid, name;
    double rating = 0.;
    int matches = 0, wins = 0, losses = 0, placements = 0, dodges = 0;
    RankedDodge dodge;
    std::vector<AdminAction> actions;
    std::vector<DodgeLogEntry> dodgeLog;
};
struct AccountChoice {
    RankedProfile rank;
    std::string uid;
    int64_t lastPlayedAt = 0;
    bool thisDevice = false;
};
struct MatchCancelResult {
    int canceled = 0, skipped = 0;
    double removed = 0.;
    std::vector<std::pair<std::string, double>> refunds;
};
// 서버 방송 공지와 점검 정보. 방송은 kBroadcastLiveMs(ms) 동안 화면에 표시한다.
struct BroadcastInfo {
    int64_t id = 0, createdAt = 0;
    std::string sender, message;
};
inline constexpr int64_t kBroadcastLiveMs = 120'000;
struct MaintenanceInfo {
    int64_t startedAt = 0, expectedEndAt = 0;
    std::string version;
    bool down = false;
};
// 점검 안내 메시지의 고정 접두사와 판별/남은 시간 계산 함수.
inline constexpr char const* kMaintenancePrefix = "Server maintenance: ";
bool isMaintenanceMessage(std::string const& message);
std::string maintenanceTimeLeft(MaintenanceInfo const& info, int64_t serverNow);
// 기본 시즌 번호와 종료 시각(서버 시각 ms), 남은 시간 문구.
inline constexpr int kSeasonDefault = 2;
inline constexpr int64_t kSeasonEndsAtDefault = 1'798'729'200'000;
std::string seasonTimeLeft(int season, int64_t endsAt, int64_t serverNow);
// 내가 보낸 신고의 처리 결과 알림 / 내 신고 목록 항목.
struct ReportNotice {
    int64_t id = 0, handledAt = 0, banUntil = -1;
    std::string targetName, state, outcome;
    std::string note;
};
struct MyReport {
    int64_t id = 0, createdAt = 0, handledAt = 0, banUntil = -1;
    std::string targetUid, targetName, reason, detail, state, outcome, note;
};
using MyReportsCallback = std::function<void(std::vector<MyReport>, std::string)>;
// 관리자 패널용 상태 (권한, 처리 대기 수 등)와 제재 콜백 타입.
struct AdminStatus {
    bool admin = false;
    int reports = 0, bans = 0, reportBans = 0;
    int appeals = 0;
    bool season2 = false;
    std::string role;
    int inbox = 0, replies = 0;
};
using BanCallback = std::function<void(std::optional<BanInfo>, std::string)>;
using BanResultCallback = std::function<void(std::optional<BanInfo>, int, std::string)>;
// 스태프 도구 구조: 처리 로그, 스태프 목록/후보, 스태프 메시지, 제재 미리보기.
struct ModLogEntry {
    int64_t id = 0, at = 0, durationMs = -1, until = 0;
    std::string actor, actorName, role, action, target, targetName, reason, message;
    bool permanent = false, lifted = false;
    int reports = 0, canceled = 0, refunded = 0, from = 0, to = 0;
    double amount = 0., before = 0., after = 0., removed = 0.;
};
struct StaffMember {
    std::string uid, name, addedBy, addedByName;
    int accountId = 0;
    int64_t addedAt = 0;
};
struct StaffCandidate {
    std::string uid, name, role;
    int accountId = 0;
};
struct StaffMessage {
    int64_t id = 0, createdAt = 0, readAt = 0, repliedAt = 0;
    std::string sender, senderName, kind, body, reply;
    bool read = false, replySeen = false;
};
struct BanPreview {
    int matches = 0, opponents = 0, retentionDays = 7, cancelMax = 2000;
    bool banned = false;
};
// 듀얼 설정과 차단 목록.
struct DuelSettings {
    bool enabled = true, anonymous = true;
};
struct DuelBlock {
    std::string id;
    bool anonymous = false;
    int64_t createdAt = 0, until = 0;
    std::optional<PlayerProfile> player;
};
struct DuelBlockList {
    DuelSettings settings;
    std::optional<int> rating;
    int limit = 300;
    int gap = 500;
    std::vector<DuelBlock> blocks;
};
using DuelBlocksCallback = std::function<void(std::optional<DuelBlockList>, std::string)>;
// 디버그 방 매치 id 접두사.
inline constexpr char const* kDebugRoomMatch = "debug-room-";

// 서버와 통신하는 클라이언트 서비스 (싱글톤). 모든 요청은 비동기 콜백으로 결과를 돌려준다.
// 상태는 구현 파일의 내부 state()에 있으며, tick(dt)가 매 프레임 폴링/하트비트/재접속을 처리한다.
// 대부분의 메서드는 요청 전에 connect()로 인증을 보장한다.
class Service {
public:
    // 연결/인증/업데이트/멤버십 관련.
    static Service& get();
    void initialize();
    void connect(Done callback);
    void fetchRelease(std::function<void(std::string, std::string)> callback);
    void fetchPatchNotes(std::function<void(std::string, std::string, std::string)> callback);
    void refreshMembership(Done callback);
    void redeemMembership(std::string code, Done callback);
    bool connected() const;
    bool updatePending() const;
    bool safeToUpdate() const;
    bool battleInProgress() const;
    bool recordsPending() const;
    RankedState const* rankedState() const;
    PlayerProfile const& profile() const;
    bool saveCosmetics(Cosmetics const& cosmetics);
    std::optional<RoomInfo> const& room() const;
    bool isHost() const;
    bool busy() const;
    // 1v1 방, 전투 진행 보고, 기록 조회.
    bool downloadReported(int64_t levelID) const;
    void reportDownloadNow();
    bool battleReportAvailable() const;
    void wantBattleReport(bool wanted);
    // 서버가 내 자리를 마지막으로 확인한 시각(진행 보고 또는 heartbeat 성공). 서버는 이게 30초 넘게 없으면
    // 연결 끊김 패배로 본다.
    std::chrono::steady_clock::time_point lastSeatAck() const;
    void fetchRooms(RoomListCallback callback);
    void fetchHistory(HistoryCallback callback);
    void fetchPlayerHistory(std::string uid, HistoryCallback callback);
    void fetchAttempts(MatchRecord match, int firstRun, AttemptsCallback callback);
    void recordAttempt(int run, int percent);
    void acknowledgeResult(Done callback);
    void createRoom(std::string name, std::string pin, Done callback, bool allowHacks = false, bool debug = false);
    // Group 방 (생성/참가/방장 기능, Map Pool).
    std::optional<GroupRoomInfo> const& group() const;
    bool inGroup() const;
    void setGroupSpectating(bool on);
    bool groupSpectating() const;
    void fetchGroups(std::function<void(std::vector<GroupSummary>, std::string)> callback);
    void createGroup(std::string pin, int maxPlayers, bool allowHacks, Done callback, bool debug = false);
    void configureMapPool(MapPoolSettings settings, Done callback);
    void addToMapPool(LevelInfo level, bool twoPlayer, Done callback);
    void removeFromMapPool(int64_t levelId, Done callback);
    void permitMapPool(std::string uid, bool allowed, Done callback);
    void joinGroup(std::string id, std::string pin, Done callback);
    void kickGroupMember(std::string uid, bool ban, Done callback);
    void handOverGroup(std::string uid, Done callback);
    void configureGroup(int attempts, int maxPlayers, int speed, bool autoLives, int mode, bool practice,
                        int targetPercent, Done callback);
    void stopGroupStart(Done callback);
    void markGroupResultShown(std::string battleId);
    bool groupResultShown(std::string const& battleId) const;
    // 1v1 방 조작 (참가/나가기/맵·규칙/준비/시작/전투 보고).
    void joinRoom(RoomInfo room, std::string pin, Done callback);
    void leaveRoom(Done callback);
    void kickGuest(std::string uid, Done callback, bool ban = false);
    void selectLevel(LevelInfo level, Done callback);
    void configureMapSelection(MapSelection selection, Done callback);
    void finishMapDraw(std::string id, Done callback);
    void rerollMap(std::string drawId, Done callback);
    void configureRules(GameRules rules, Done callback);
    void setReady(bool ready, Done callback);
    void sendEmote(std::string kind, Done callback);
    void sendChat(std::string text, Done callback);
    void startMatch(Done callback);
    void markLoaded(std::string launchId, Done callback);
    void cancelLaunch(std::string launchId, Done callback);
    void reportBattle(BattlePlayerState progress, bool sendPosition, Done callback);
    void queueForfeit(BattlePlayerState progress);
    // Ranked / Season 2 조회와 Plaza, Live 교환.
    void fetchRankedState(RankedStateCallback callback);
    void fetchRankedProfile(std::string uid, std::function<void(std::optional<RankedProfile>, std::string)> callback);
    void fetchRankedProfileIn(std::string cup, std::string uid,
                              std::function<void(std::optional<RankedProfile>, std::string)> callback);
    void fetchAchievements(std::string uid, std::function<void(std::optional<AchievementState>, std::string)> callback);
    void fetchAchievementRates(std::function<void(std::optional<AchievementRates>)> callback);
    void fetchRankedLeaderboard(std::function<void(std::vector<RankedProfile>, std::string)> callback);
    void fetchRankedTiers(std::function<void(std::optional<RankedTiers>, std::string)> callback, bool fresh = false);
    void setRankedCup(std::string cup);
    std::string const& rankedCup() const;
    bool season2() const { return !rankedCup().empty(); }
    bool season2Access() const;
    void fetchSeason2Home(std::function<void(std::vector<Season2Cup>, std::string)> callback);
    void fetchSeason2Tiers(std::string cup, std::function<void(std::optional<Season2Tiers>, std::string)> callback);
    void fetchSeason2Dodges(std::function<void(std::optional<std::vector<DodgeLogEntry>>, std::string)> callback);
    void searchSeason2Duel(std::string name, std::function<void(std::vector<Season2DuelPlayer>, std::string)> callback);
    void recentSeason2Duels(std::function<void(std::vector<Season2DuelPlayer>, std::string)> callback);
    void peekSeason2Match(std::string cup, std::function<void(std::optional<RankedMatch>, std::string)> callback);
    void challengeSeason2(std::string targetUid, std::string cup, std::string tierName, RankedStateCallback callback);
    void exchangePlaza(PlazaUpdate update, std::function<void(std::optional<PlazaAnswer>, std::string)> callback);
    void exchangeLive(LiveTarget const& target, std::optional<LiveSend> send, std::optional<LiveWatch> watch,
                      std::function<void(std::optional<LiveAnswer>, std::string)> callback);
    bool liveWatched() const;
    void forgetLiveWatched();
    void searchRankedPlayers(std::string query, std::function<void(std::vector<RankedProfile>, std::string)> callback);
    // Ranked 대기열, 듀얼, 이모트.
    void joinRankedQueue(RankedStateCallback callback);
    // declineRematch: 무승부 카드에서 뒤로 가기(1.0.7) - 재대결을 사양한다(닷지 아님).
    void cancelRanked(RankedStateCallback callback, bool searchCancel = false, std::string seen = {},
                      bool declineRematch = false);
    void challengeRanked(std::string targetUid, bool anonymous, RankedStateCallback callback);
    void setRankedDuels(bool enabled, RankedStateCallback callback);
    void sendRankedLobbyEmote(std::string kind, RankedStateCallback callback);
    // 내 제재 및 이의 제기, 관리자 상태, 리플레이, 신고.
    void fetchOwnBan(std::function<void(bool, std::optional<BanInfo>, std::string)> callback);
    std::optional<BanInfo> const& ownBan() const;
    std::optional<AppealStatus> const& ownAppeal() const;
    void sendBanAppeal(std::string text, std::function<void(bool, std::string)> done);
    using AppealsCallback = std::function<void(std::vector<BanAppeal>, int, std::string)>;
    void fetchBanAppeals(AppealsCallback callback);
    void answerBanAppeal(int64_t id, bool accept, std::string answer, AppealsCallback callback);
    std::optional<BanInfo> const& ownReportBan() const;
    void fetchAdminStatus(std::function<void(AdminStatus, std::string)> callback);
    void uploadReplay(ReplayUpload upload);
    void fetchReplayList(std::string matchId, std::function<void(std::vector<ReplaySummary>, std::string)> callback);
    void fetchReplay(std::string matchId, std::string uid,
                     std::function<void(std::optional<ReplayFile>, std::string)> callback);
    bool isAdmin() const;
    void reportPlayer(PlayerProfile const& target, std::string reason, std::string detail, Done callback);
    void fetchReports(std::function<void(std::vector<ReportedPlayer>, std::string)> callback);
    void dismissReports(std::string uid, std::string note, Done callback);
    void fetchMyReports(MyReportsCallback callback);
    void withdrawReport(int64_t id, MyReportsCallback callback);
    void fetchReportLog(std::function<void(std::vector<ReportedPlayer>, std::string)> callback);
    void fetchReportNotices(std::function<void(std::vector<ReportNotice>, std::string)> callback);
    // 방송 공지, 점검 상태, 시즌 정보.
    std::vector<BroadcastInfo> const& broadcasts() const;
    int onlineCount() const;
    int season() const;
    int64_t seasonEndsAt() const;
    std::string argonState() const;
    bool takeArgonNotice();
    int unreadBroadcasts() const;
    void markBroadcastsRead();
    void refreshBroadcasts(Done callback);
    void sendBroadcast(std::string message, Done callback);
    void setBroadcastPresenter(std::function<bool(BroadcastInfo const&)> presenter);
    void keepBroadcastsFresh();
    std::optional<MaintenanceInfo> const& maintenance() const;
    std::string maintenanceMessage() const;
    void setMaintenancePresenter(std::function<bool(MaintenanceInfo const&)> presenter);
    void acknowledgeReportNotices(std::vector<int64_t> ids);
    // 환급/회피 로그 조회, 관리자 레이팅 조정, 계정 선택.
    void fetchRatingRefunds(std::function<void(std::vector<RatingRefund>, std::string)> callback);
    void fetchDodgeLog(std::function<void(std::vector<DodgeLogEntry>, std::string)> callback);
    using AdminRankedCallback = std::function<void(std::optional<AdminRankedRecord>, std::string)>;
    void fetchAdminRanked(std::string uid, AdminRankedCallback callback);
    void adjustRating(std::string uid, double delta, std::string reason, AdminRankedCallback callback);
    void setDodgeStack(std::string uid, int stack, std::string reason, bool lift, AdminRankedCallback callback);
    std::vector<AccountChoice> const& accountChoices() const;
    void chooseAccount(std::string uid, Done callback);
    void acknowledgeRatingRefunds(std::vector<int64_t> ids);
    void cancelRankedMatches(std::string target, std::vector<std::string> roomIds, std::string reason,
                             std::function<void(std::optional<MatchCancelResult>, std::string)> callback);
    // 제재(밴) 관리와 스태프 도구.
    void fetchBans(std::function<void(std::vector<BanInfo>, std::string)> callback);
    static constexpr int kBanCancelAll = -1;
    struct ArgonCheck {
        bool reachable = false, valid = false, validWeak = false;
        int status = 0, ms = 0;
        std::string username, cause, error;
    };
    void argonTest(std::string token, int accountId, std::string name,
                   std::function<void(std::optional<ArgonCheck>, std::string)> callback);
    void setBan(std::string uid, std::string name, int accountId, int64_t durationMs, std::string reason,
                int cancelMatches, BanResultCallback callback);
    void fetchBanPreview(std::string uid, std::function<void(std::optional<BanPreview>, std::string)> callback);
    void fetchBanHistory(std::string uid, std::function<void(std::optional<BanHistory>, std::string)> callback);
    std::string const& staffRole() const;
    bool isOwner() const;
    bool isModerator(std::string const& uid) const;
    void checkStaffMember(std::function<void(bool)> done);
    void fetchModLog(std::function<void(std::vector<ModLogEntry>, std::string)> callback);
    using StaffCallback = std::function<void(std::vector<StaffMember>, std::string)>;
    void fetchStaff(StaffCallback callback);
    void findStaffCandidates(std::string name, std::function<void(std::vector<StaffCandidate>, std::string)> callback);
    void addStaff(std::string uid, StaffCallback callback);
    void removeStaff(std::string uid, StaffCallback callback);
    using StaffMessagesCallback = std::function<void(std::vector<StaffMessage>, int, std::string)>;
    void fetchStaffMessages(StaffMessagesCallback callback);
    void sendStaffMessage(std::string kind, std::string body, StaffMessagesCallback callback);
    void acknowledgeStaffReplies(std::vector<int64_t> ids);
    void fetchStaffInbox(StaffMessagesCallback callback);
    void readStaffMessage(int64_t id, StaffMessagesCallback callback);
    void replyStaffMessage(int64_t id, std::string reply, StaffMessagesCallback callback);
    void messageStaffMember(std::string uid, std::string text, StaffMessagesCallback callback);
    void deleteStaffMail(std::vector<int64_t> ids, StaffMessagesCallback callback);
    void liftBan(std::string uid, Done callback);
    void fetchReportBans(std::function<void(std::vector<BanInfo>, std::string)> callback);
    void setReportBan(std::string uid, std::string name, int accountId, int64_t durationMs, std::string reason,
                      BanCallback callback);
    void liftReportBan(std::string uid, Done callback);
    // 듀얼 요청 응답과 차단 목록.
    void respondDuel(std::string requestId, bool accept, RankedStateCallback callback);
    void cancelDuel(std::string requestId, RankedStateCallback callback);
    void ignoreDuel(std::string requestId, int minutes, RankedStateCallback callback);
    void fetchDuelBlocks(DuelBlocksCallback callback);
    void editDuelBlock(std::string blockId, int minutes, DuelBlocksCallback callback);
    void removeDuelBlock(std::string blockId, DuelBlocksCallback callback);
    void setDuelSettings(DuelSettings settings, DuelBlocksCallback callback);
    // Live 목록, 맵 투표, Group 관전.
    void fetchLiveMatches(std::function<void(LiveList, std::string)> callback);
    void fetchLiveBattle(std::string roomId, std::function<void(LiveBattleView)> callback);
    void fetchMapVotes(std::vector<int64_t> ids, MapVotesCallback callback);
    std::optional<MapVotes> knownMapVotes(int64_t id) const;
    void voteMap(int64_t id, std::string name, int vote,
                 std::function<void(std::optional<MapVotes>, std::string)> callback);
    void fetchPlayerMapVotes(std::string uid, std::function<void(std::vector<MapVoteRecord>, std::string)> callback);
    void watchGroup(std::string roomId, GroupBattle known, std::string etag, std::function<void(GroupWatch)> callback);
    // Ranked 매치 진행: 실격, 후보 맵 제출, 에셋 준비, LDM, 재추첨, 밴, 결과 확인.
    void disqualifyRanked(std::string matchId, std::string integrityCode, std::string integrityFeatures,
                          RankedStateCallback callback);
    void submitRankedCandidates(std::string matchId, std::vector<LevelInfo> candidates, RankedStateCallback callback);
    void rankedAssetsReady(std::string matchId, std::string drawId, double seconds, RankedStateCallback callback,
                           bool twoPlayer = false, bool noSong = false);
    void setRankedLdm(std::string matchId, bool enabled, RankedStateCallback callback);
    bool rankedLdm() const;
    void rerollRankedMap(std::string matchId, std::string drawId, std::string reason, std::string kind,
                         RankedStateCallback callback);
    void voteRankedReroll(std::string matchId, std::string drawId, RankedStateCallback callback);
    void banSeason2Map(std::string matchId, int64_t levelId, RankedStateCallback callback);
    void reportSeason2Played(std::string matchId, std::string key, std::vector<int> ids,
                             std::function<void(bool)> done);
    void acknowledgeRanked(std::string matchId, Done callback);
    // 디버그용 봇 전투, 디버그 방, 안티치트 적용 여부, 서버 시각.
    bool isRankedMatch() const;
    bool botBattle() const;
    bool startBotBattle(LevelInfo level, PlayerProfile bot, std::string& error);
    void endBotBattle();
    std::string takeBotResult();
    bool debugRoom() const;
    bool antiCheatEnforced() const;
    int64_t serverNow() const;
    // 매 프레임 호출. 폴링과 하트비트 등 주기 작업을 처리한다.
    void tick(float dt);
    std::string takeNotice();
};
}
