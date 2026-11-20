// 디버그 방(Debug Room): 서버 없이 혼자 방 흐름을 시험하는 로컬 전용 방.
// 서버에 쓰는 대신 메모리의 localBody 를 갱신하고, 서버가 하던 일(타임스탬프 채우기,
// 게스트·다운로드·launch 처리)을 soloFixup 이 흉내 낸다. Service 구현에 포함되는 파일.
namespace {
// 혼자 테스트할 때 게스트 자리에 넣는 가짜 플레이어의 uid
constexpr char const* kSoloUid = "debug-solo";

// 게스트 자리를 채울 가짜 프로필
PlayerProfile soloStandIn() {
    PlayerProfile solo;
    solo.uid = kSoloUid;
    solo.name = "Solo Test";
    solo.icon = 1;
    solo.color1 = 0x7D7D7D;
    solo.color2 = 0xFFFFFF;
    return solo;
}

// 값이 서버 타임스탬프 자리표시자(.sv)면 현재 시각 now 로 바꾼다.
void stampAt(Json& parent, char const* key, int64_t now) {
    auto const& value = std::as_const(parent)[key];
    if (value.isObject() && value.contains(".sv")) parent[key] = now;
}
// 방 본문에서 서버 타임스탬프가 들어갈 모든 필드를 현재 시각으로 치환한다.
void stampServerValues(Json& body, int64_t now) {
    for (auto key : {"updatedAt", "hostSeen", "guestSeen", "hostEmoteAt", "guestEmoteAt"}) stampAt(body, key, now);
    if (std::as_const(body)["launch"].isObject())
        for (auto key : {"requestedAt", "releasedAt"}) stampAt(body["launch"], key, now);
    if (std::as_const(body)["battle"].isObject()) stampAt(body["battle"], "finishedAt", now);
    if (std::as_const(body)["mapDraw"].isObject()) stampAt(body["mapDraw"], "at", now);
    if (std::as_const(body)["emote"].isObject()) stampAt(body["emote"], "at", now);
    if (std::as_const(body)["downloads"].isObject())
        for (auto side : {"host", "guest"})
            if (std::as_const(body)["downloads"][side].isObject()) stampAt(body["downloads"][side], "updatedAt", now);
}

// 서버 대신 방 본문을 보정한다: 가짜 게스트와 준비 상태를 넣고, 레벨 다운로드를 완료로 표시하며,
// launch/battle ID 에 디버그 접두사를 붙이고 게스트 로딩·복귀를 자동 처리한다.
void soloFixup(Json& body) {
    auto& s = state();
    auto const now = nowMs();
    stampServerValues(body, now);
    body["guest"] = profileJson(soloStandIn());
    body["guestSeen"] = now;
    body["guestReady"] = true;
    if (auto const level = intAt(std::as_const(body)["level"], "id"); level > 0) {
        auto done = Json::object();
        done["uid"] = kSoloUid;
        done["levelId"] = level;
        done["percent"] = 100;
        done["mapReady"] = true;
        done["songsDone"] = 0;
        done["songsTotal"] = 0;
        done["soundsDone"] = 0;
        done["soundsTotal"] = 0;
        done["updatedAt"] = now;
        if (!std::as_const(body)["downloads"].isObject()) body["downloads"] = Json::object();
        body["downloads"]["guest"] = std::move(done);
    }
    if (std::as_const(body)["launch"].isObject() && std::as_const(body)["battle"].isObject()) {
        auto const id = stringAt(std::as_const(body)["launch"], "id");
        if (!id.empty() && !id.starts_with(kDebugRoomMatch) && stringAt(std::as_const(body)["battle"], "id") == id) {
            body["launch"]["id"] = kDebugRoomMatch + id;
            body["battle"]["id"] = kDebugRoomMatch + id;
        }
        auto& launch = body["launch"];
        launch["guestLoaded"] = true;
        if (boolAt(launch, "hostLoaded") && intAt(launch, "releasedAt") <= 0) launch["releasedAt"] = now;
    }
    if (std::as_const(body)["battle"].isObject() && intAt(std::as_const(body)["battle"], "finishedAt") > 0) {
        body["battle"]["guestReturned"] = true;
        s.recordedMatches.insert(stringAt(std::as_const(body)["battle"], "id"));
    }
}

// 혼자 하는 배틀의 종료 조건을 판정한다. 호스트가 규칙상 끝났으면 호스트 승리로 마감한다.
void soloResolve(Json& body) {
    if (!std::as_const(body)["battle"].isObject()) return;
    auto& battle = body["battle"];
    if (intAt(battle, "finishedAt") > 0) return;
    auto const rules = parseRules(std::as_const(body)["rules"]);
    auto const me = parseBattlePlayer(std::as_const(battle)["host"]);
    bool const ended = me.forfeited || battle::terminal(me, rules) ||
                       (rules.mode == 1 && battle::raceReached(me) >= rules.targetPercent) ||
                       (rules.mode != 2 && battle::pauseRemaining(me, nowMs()) == 0 &&
                        !battle::finalByPlay(me, rules));  // (시간제는 일시정지 한도가 없다)
    if (!ended) return;
    battle["draw"] = false;
    battle["winnerUid"] = stringAt(std::as_const(body)["host"], "uid");
    battle["finishedAt"] = nowMs();
}

// 호스트의 진행 상황을 로컬 방에 반영한다. 시도 횟수, 최고 퍼센트, 클리어 등은 줄어들지 않게
// 기존 값과 합치고, 일시정지 사용량은 예산 안으로 제한한 뒤 종료 여부를 판정한다.
void soloReport(BattlePlayerState progress) {
    auto& s = state();
    if (!s.localRoom || !s.room || !std::as_const(s.localBody)["battle"].isObject()) return;
    if (intAt(std::as_const(s.localBody)["battle"], "finishedAt") > 0) return;
    auto const current = parseBattlePlayer(std::as_const(s.localBody)["battle"]["host"]);
    progress.attemptsUsed = std::max(progress.attemptsUsed, current.attemptsUsed);
    progress.runNumber = std::max(progress.runNumber, current.runNumber);
    progress.bestPercent = std::clamp(std::max(progress.bestPercent, current.bestPercent), 0, 100);
    progress.currentPercent = std::clamp(progress.currentPercent, 0, 100);
    progress.cleared = current.cleared || progress.cleared;
    progress.forfeited = current.forfeited || progress.forfeited;
    progress.userCoins = std::max(progress.userCoins, current.userCoins);
    progress.pauseUsedMs =
        std::clamp<int64_t>(std::max(progress.pauseUsedMs, current.pauseUsedMs), 0, battle::PAUSE_BUDGET_MS);
    progress.updatedAt = nowMs();
    s.localBody["battle"]["host"] = battlePlayerJson(progress);
    soloResolve(s.localBody);
    soloFixup(s.localBody);
    adoptRoom(s.room->id, s.localBody);
    s.lastBattleReport = Clock::now();
}

// 디버그 방 ID 에 대한 방 쓰기를 서버로 보내지 않고 로컬에서 처리한다.
// 디버그 방이 아니면 false(호출자가 서버 경로로 진행). 방이 닫혔거나 세션 epoch 가
// 다르면 실패 콜백, 변경이 없으면 현재 본문을 그대로 채택한다.
bool localRoomWrite(std::string const& id, uint64_t epoch, Mutator const& change, Done& callback) {
    auto& s = state();
    if (!id.starts_with("debug-room-")) return false;
    if (!s.localRoom || !s.room || s.room->id != id) {
        callback(false, "The Debug Room was closed.");
        return true;
    }
    if (epoch != s.epoch) {
        callback(false, "Room session changed.");
        return true;
    }
    ++s.revision;
    auto body = s.localBody;
    body["guestSeen"] = nowMs();
    auto error = change(body);
    if (error == ROOM_UNCHANGED) {
        adoptRoom(id, s.localBody);
        callback(true, {});
        return true;
    }
    if (!error.empty()) {
        callback(false, std::move(error));
        return true;
    }
    if (body.isNull()) {
        s.localBody = std::move(body);
        callback(true, {});
        return true;
    }
    body["updatedAt"] = nowMs();
    soloFixup(body);
    s.localBody = body;
    adoptRoom(id, body);
    callback(true, {});
    return true;
}
}

// 현재 디버그 방에 있는지 여부
bool Service::debugRoom() const {
    return state().localRoom;
}
