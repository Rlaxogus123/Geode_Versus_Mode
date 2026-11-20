// 1v1 배틀의 진행 규칙을 계산하는 순수 함수 모음(일시 정지 예산, 시도 소모, 종료/승리 판정, 관전 좌석).
// BattlePlayerState와 GameRules만 받아 판단하므로 네트워크나 PlayLayer에 의존하지 않는다.
// GameRules.mode 0은 시도 횟수(attempts) 방식, 1은 목표 퍼센트(targetPercent) 레이스 방식, 2는 시간제(Time)다.
#pragma once
#include "VersusService.hpp"
#include <algorithm>
#include <string_view>
#include <tuple>

namespace versus::battle {
// 한 배틀에서 플레이어당 쓸 수 있는 일시 정지 총 시간(ms).
inline constexpr int64_t PAUSE_BUDGET_MS = 20000;
// 서버가 받아 적는 일시 정지 사용량의 상한(ms, 서버 PAUSE_VALUE_MAX_MS).
inline constexpr int64_t PAUSE_VALUE_MAX_MS = 30000;
// 시도와 시도 사이(죽은 뒤 다음 시도 시작 전)에만 일시 정지를 허용한다.
inline bool canPauseBetweenAttempts(BattlePlayerState const& player, bool nativeDead) {
    return nativeDead && !player.inAttempt && !player.cleared && !player.forfeited && !player.spectating;
}
// GD 기본 일시 정지가 실제 일시 정지 메뉴와 함께 열려 있는지.
inline bool nativePauseActive(bool nativePaused, bool pauseMenuOpen) {
    return nativePaused && pauseMenuOpen;
}
// 남은 일시 정지 예산(ms). 지금 진행 중인 일시 정지 경과 시간도 뺀다.
inline int64_t pauseRemaining(BattlePlayerState const& player, int64_t now) {
    auto elapsed = player.paused && player.pausedAt > 0 ? std::max<int64_t>(0, now - player.pausedAt) : 0;
    return std::max<int64_t>(0, PAUSE_BUDGET_MS - player.pauseUsedMs - elapsed);
}
// 진행 중이던 일시 정지 시간을 사용량에 반영하고 일시 정지 상태를 해제한다.
inline void consumePause(BattlePlayerState& player, int64_t now) {
    player.pauseUsedMs = PAUSE_BUDGET_MS - pauseRemaining(player, now);
    player.paused = false;
    player.pausedAt = 0;
}
// 진행 보고를 보낼 시점인지: 타이머가 끝났고, 이미 보고 중이 아니며, 변경분이 있고, 전송이 가능할 때.
inline bool progressReportDue(float remaining, bool reporting, bool dirty, bool transportReady) {
    return remaining <= 0.f && !reporting && dirty && transportReady;
}
// 서버가 준 배틀 보고(report)를 현재 로컬 상태에 반영해도 되는지 판단한다.
// 시작 전이거나 launch/battle ID가 expectedID와 다르거나, 출발 전 또는 이미 끝난 배틀이면 true.
inline bool adoptBattleReport(bool started, std::string_view launchID, std::string_view battleID, int64_t releasedAt,
                              int64_t finishedAt, std::string_view expectedID) {
    return !started || launchID != expectedID || battleID != expectedID || releasedAt <= 0 || finishedAt > 0;
}

// 더 이상 시도할 수 없는 상태: 포기했거나 클리어했거나, 시도 모드에서 시도를 모두 썼거나, 시간제에서 마지막 판을
// 다 했음.
inline bool terminal(BattlePlayerState const& player, GameRules const& rules) {
    return player.forfeited || player.cleared ||
           (rules.mode == 0 && !rules.practice && player.attemptsUsed >= rules.attempts) ||
           (rules.mode == 2 && timeLastRunPlayed(player));
}
// 시도 모드에서 상대의 시도가 모두 소진되어 결과가 정해졌는지(조기 승리) 판단한다.
// candidate가 클리어했는데 상대가 더 많이 시도했거나, 상대가 소진했고 candidate가 더 멀리 갔으며
// 남은 시도로도 뒤집을 수 없을 때 true.
inline bool earlyAttemptWin(BattlePlayerState const& candidate, BattlePlayerState const& exhausted,
                            GameRules const& rules) {
    if (rules.mode != 0 || rules.practice || candidate.forfeited || exhausted.forfeited) return false;
    if (candidate.cleared && exhausted.attemptsUsed > candidate.attemptsUsed) return true;
    if (exhausted.attemptsUsed < rules.attempts || candidate.bestPercent <= exhausted.bestPercent) return false;
    int const spent = candidate.attemptsUsed + (candidate.inAttempt ? 1 : 0);
    return spent < exhausted.attemptsUsed;
}
// 실시간 점수 비교용 튜플: (도달 퍼센트, 적게 쓴 시도 우선, 유저 코인). 클리어면 100%.
inline std::tuple<int, int, int> liveScore(BattlePlayerState const& player) {
    int reached = std::max(player.bestPercent, player.inAttempt ? player.currentPercent : 0);
    if (player.cleared) reached = 100;
    return {reached, -player.attemptsUsed, player.userCoins};
}
// 클리어/포기 없이 시도를 모두 쓴 상태.
inline bool outOfLives(BattlePlayerState const& player, GameRules const& rules) {
    return !player.cleared && !player.forfeited && player.attemptsUsed >= rules.attempts;
}
// 한쪽의 시도가 소진됐고 상대가 이미 그 최고 퍼센트를 넘어서 승부가 난 경우.
inline bool livesExhausted(BattlePlayerState const& host, BattlePlayerState const& guest, GameRules const& rules) {
    if (rules.mode != 0 || rules.practice) return false;
    return (outOfLives(host, rules) && std::get<0>(liveScore(guest)) > host.bestPercent) ||
           (outOfLives(guest, rules) && std::get<0>(liveScore(host)) > guest.bestPercent);
}
// 진행 중이던 시도를 퍼센트와 함께 마감한다. 시도 횟수와 최고 퍼센트를 갱신하고, 종료 상태면 관전 상태로 전환한다.
inline bool finishAttempt(BattlePlayerState& player, GameRules const& rules, int percent) {
    if (!player.inAttempt || terminal(player, rules)) return false;
    player.currentPercent = std::clamp(percent, 0, 100);
    player.bestPercent = std::max(player.bestPercent, player.currentPercent);
    ++player.attemptsUsed;
    player.inAttempt = false;
    player.spectating = terminal(player, rules);
    return true;
}
// 서버가 확인한 진행과 로컬 진행을 합친다. 각 값은 큰 쪽을 따르고, 종료 상태면 관전으로 둔다.
inline void reconcileProgress(BattlePlayerState& local, BattlePlayerState const& confirmed, GameRules const& rules) {
    local.attemptsUsed = std::max(local.attemptsUsed, confirmed.attemptsUsed);
    local.bestPercent = std::max(local.bestPercent, confirmed.bestPercent);
    local.userCoins = std::max(local.userCoins, confirmed.userCoins);
    local.cleared = local.cleared || confirmed.cleared;
    local.forfeited = local.forfeited || confirmed.forfeited;
    if (local.lastRun == 0) local.lastRun = confirmed.lastRun;  // 시간제: 서버가 정한 마지막 판
    if (terminal(local, rules)) {
        local.inAttempt = false;
        local.spectating = true;
    }
}
// 결과가 확정되어 더 플레이할 수 없는지(레이스 모드는 목표 퍼센트 도달 포함). 포기한 경우는 제외.
inline bool resultLocked(BattlePlayerState const& player, GameRules const& rules) {
    return !player.forfeited &&
           (terminal(player, rules) || (rules.mode == 1 && player.bestPercent >= rules.targetPercent));
}
// 레이스 모드에서 도달한 퍼센트(클리어면 100).
inline int raceReached(BattlePlayerState const& player) {
    return player.cleared ? 100 : std::max(player.bestPercent, player.currentPercent);
}
// 포기하지 않았고 플레이 결과만으로 끝난 상태(클리어, 시도 소진, 레이스 목표 도달).
inline bool finalByPlay(BattlePlayerState const& player, GameRules const& rules) {
    if (player.forfeited) return false;
    if (player.cleared) return true;
    if (rules.mode == 2) return timeLastRunPlayed(player);
    if (rules.mode == 0 && !rules.practice && player.attemptsUsed >= rules.attempts) return true;
    return rules.mode == 1 && raceReached(player) >= rules.targetPercent;
}
// 플레이 결과 기준으로 끝난 상태(terminal 또는 레이스 목표 도달). 포기 여부는 따지지 않는다.
inline bool resultFinal(BattlePlayerState const& player, GameRules const& rules) {
    return terminal(player, rules) || (rules.mode == 1 && raceReached(player) >= rules.targetPercent);
}
// 연습(practice) 규칙의 방은 관전할 수 없다.
inline bool watchableRules(GameRules const& rules) {
    return !rules.practice;
}
// 결과가 아직 확정되지 않았는데 로컬 플레이어가 더 못 뛰는 상태이면 관전 플레이로 본다.
inline bool spectatorPlay(GameRules const& rules, BattlePlayerState const& local, bool resultKnown) {
    return !resultKnown && resultLocked(local, rules);
}

// 관전 테스트 기능을 서버가 지원하는 마지막 서버 버전 기준 문자열.
inline constexpr char const* kSpectateTestLastServer = "v0.8.2";
// 무제한 시도로 취급할 때 쓰는 큰 시도 횟수.
inline constexpr int kEndlessAttempts = 1'000'000;
// 관전 좌석: 호스트/게스트 중 누가 관전 상태인지.
enum class SpectatorSeat { None, Host, Guest };
// 시도를 모두 쓴 쪽을 관전 좌석으로 정한다. 둘 다 소진이면 시도를 덜 쓴 쪽, 같으면 spectating 쪽.
inline SpectatorSeat spectatorSeat(BattlePlayerState const& host, BattlePlayerState const& guest,
                                   GameRules const& rules) {
    auto out = [&rules](BattlePlayerState const& player) {
        return !player.cleared && !player.forfeited && player.attemptsUsed >= rules.attempts;
    };
    bool const hostOut = out(host), guestOut = out(guest);
    if (hostOut != guestOut) return hostOut ? SpectatorSeat::Host : SpectatorSeat::Guest;
    if (!hostOut) return SpectatorSeat::None;
    if (host.attemptsUsed != guest.attemptsUsed)
        return host.attemptsUsed < guest.attemptsUsed ? SpectatorSeat::Host : SpectatorSeat::Guest;
    if (host.spectating != guest.spectating) return host.spectating ? SpectatorSeat::Host : SpectatorSeat::Guest;
    return SpectatorSeat::None;
}
// 관전하는 상대 좌석의 규칙에는 시도 횟수를 사실상 무제한으로 바꿔 돌려준다.
inline GameRules seatRules(GameRules rules, bool hostSeat, SpectatorSeat spectator) {
    if (spectator == (hostSeat ? SpectatorSeat::Guest : SpectatorSeat::Host)) rules.attempts = kEndlessAttempts;
    return rules;
}
}
