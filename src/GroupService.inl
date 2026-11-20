// Service(VersusService.cpp)에서 include 되는 Group 방 클라이언트 로직.
// 서버의 /groups/* API를 호출해 Group 방 상태(GroupRoomInfo)와 진행 중인 전투(GroupBattle)를
// 로컬 state()에 반영하고, 방/전투 폴링·진행도 보고·방장 액션을 제공한다.
// Group UI는 1v1 UI를 재사용하므로 syncGroupRoom()이 state().room으로 변환해 준다.
namespace {
// 폴링 주기(초). 로비 / 시작 카운트다운·로딩 중 / 전투 중 / 방 전체 갱신 / 카운트다운 종료 직전.
constexpr float kGroupLobbyPoll = 1.5f;
constexpr float kGroupLaunchPoll = 1.f;
constexpr float kGroupBattlePoll = 1.f;
constexpr float kGroupRoomRefresh = 5.f;
constexpr float kGroupCountdownEndPoll = .4f;

// 현재 Group 방에 들어와 있는지.
bool groupActive() {
    return state().group.has_value();
}

// 서버의 battle JSON을 GroupBattle로 변환한다.
// 같은 전투(id)를 이미 들고 있으면 좌석별 진행 상태/DQ/로드/탈락 플래그를 이어받아
// 폴링 사이에 값이 초기화되지 않게 한다.
GroupBattle parseGroupBattle(Json const& value, GroupBattle const* previous) {
    GroupBattle battle;
    battle.id = stringAt(value, "id");
    battle.requestedAt = intAt(value, "requestedAt");
    battle.releasedAt = intAt(value, "releasedAt");
    battle.finishedAt = intAt(value, "finishedAt");
    battle.winnerUid = stringAt(value, "winnerUid");
    battle.draw = boolAt(value, "draw");
    battle.level = parseLevel(value["level"]);
    battle.rules = parseRules(value["rules"]);
    battle.timeLimitMs = intAt(value, "timeLimitMs");
    std::unordered_set<std::string> loaded, out;
    if (value["loaded"].isArray())
        for (auto const& uid : value["loaded"].asArray().unwrap()) loaded.insert(uid.asString().unwrapOr(""));
    if (value["out"].isArray())
        for (auto const& uid : value["out"].asArray().unwrap()) out.insert(uid.asString().unwrapOr(""));
    if (value["profiles"].isArray())
        for (auto const& entry : value["profiles"].asArray().unwrap()) {
            GroupSeat seat;
            seat.profile = parseProfile(entry);
            seat.state.runNumber = 1;
            seat.state.inAttempt = true;
            seat.loaded = loaded.contains(seat.profile.uid);
            seat.out = out.contains(seat.profile.uid);
            battle.seats.push_back(std::move(seat));
        }
    if (value["ranking"].isArray())
        for (auto const& index : value["ranking"].asArray().unwrap())
            battle.ranking.push_back(static_cast<int>(index.asInt().unwrapOr(-1)));
    if (value["first"].isArray())
        for (auto const& index : value["first"].asArray().unwrap())
            battle.first.push_back(static_cast<int>(index.asInt().unwrapOr(-1)));
    // 이전 스냅샷과 같은 전투이고 좌석 수가 같을 때만 상태를 승계한다.
    if (previous && previous->id == battle.id && previous->seats.size() == battle.seats.size()) {
        battle.statesFinal = previous->statesFinal;
        if (battle.timeLimitMs <= 0) battle.timeLimitMs = previous->timeLimitMs;
        for (size_t i = 0; i < battle.seats.size(); ++i) {
            battle.seats[i].state = previous->seats[i].state;
            battle.seats[i].clearedAt = previous->seats[i].clearedAt;
            battle.seats[i].disqualified = previous->seats[i].disqualified;
            battle.seats[i].loaded = battle.seats[i].loaded || previous->seats[i].loaded;
            battle.seats[i].out = battle.seats[i].out || previous->seats[i].out;
        }
    }
    return battle;
}

// 전투 스냅샷을 GroupBattle에 반영한다. 좌석별 상태는 p[i] 배열 한 줄이며 순서는
// [사용 시도, 현재%, 최고%, run 번호, flags, 유저 코인, 일시정지 누적 ms, 정지 시작 시각, 정지 seq,
//  (1.0.8) 시간제의 마지막 판, 클리어한 서버 시각].
// flags 비트: 1 시도 중, 2 클리어, 4 포기, 8 관전, 16 일시정지, 32 DQ, 64 로드 완료, 128 연습 모드.
void applyGroupRows(GroupBattle& battle, Json const& snapshot) {
    battle.releasedAt = intAt(snapshot, "releasedAt");
    if (auto const limit = intAt(snapshot, "timeLimitMs"); limit > 0) battle.timeLimitMs = limit;
    battle.finishedAt = intAt(snapshot, "finishedAt");
    battle.winnerUid = stringAt(snapshot, "winnerUid");
    battle.draw = boolAt(snapshot, "draw");
    battle.ranking.clear();
    if (snapshot["ranking"].isArray())
        for (auto const& index : snapshot["ranking"].asArray().unwrap())
            battle.ranking.push_back(static_cast<int>(index.asInt().unwrapOr(-1)));
    battle.first.clear();
    if (snapshot["first"].isArray())
        for (auto const& index : snapshot["first"].asArray().unwrap())
            battle.first.push_back(static_cast<int>(index.asInt().unwrapOr(-1)));
    if (!snapshot["p"].isArray()) return;
    auto const& rows = snapshot["p"].asArray().unwrap();
    for (size_t i = 0; i < rows.size() && i < battle.seats.size(); ++i) {
        auto const& row = rows[i];
        if (!row.isArray() || row.size() < 9) continue;
        auto at = [&row](size_t index) { return row[index].asInt().unwrapOr(0); };
        auto& seat = battle.seats[i];
        auto& player = seat.state;
        auto const flags = at(4);
        player.attemptsUsed = static_cast<int>(at(0));
        player.currentPercent = static_cast<int>(std::clamp<int64_t>(at(1), 0, 100));
        player.bestPercent = static_cast<int>(std::clamp<int64_t>(at(2), 0, 100));
        player.runNumber = static_cast<int>(at(3));
        player.inAttempt = flags & 1;
        player.cleared = flags & 2;
        player.forfeited = flags & 4;
        player.spectating = flags & 8;
        player.paused = flags & 16;
        seat.disqualified = flags & 32;
        seat.loaded = flags & 64;
        player.userCoins = static_cast<int>(std::clamp<int64_t>(at(5), 0, 3));
        player.pauseUsedMs = at(6);
        player.pausedAt = at(7);
        player.pauseSeq = at(8);
        player.practicing = flags & 128;
        player.lastRun = row.size() > 9 ? static_cast<int>(std::clamp<int64_t>(at(9), 0, 1000000)) : 0;
        seat.clearedAt = row.size() > 10 ? at(10) : 0;
    }
    // 종료 시각이 있으면 이 스냅샷이 최종 상태다 (이후 추가 폴링 불필요).
    battle.statesFinal = battle.finishedAt > 0;
}

// Group 방 상태를 1v1 화면이 쓰는 RoomInfo(state().room)로 투영한다.
// 내가 전투 좌석에 있고 로드 전 포기한 상태가 아니면 launch/battle 정보도 채운다.
void syncGroupRoom() {
    auto& s = state();
    if (!s.group) return;
    auto const& group = *s.group;
    auto const& uid = s.profile.uid;
    RoomInfo room;
    room.id = group.id;
    room.name = group.name;
    room.privateRoom = group.privateRoom;
    if (auto const* host = group.member(group.hostUid))
        room.host = host->profile;
    else {
        room.host.uid = group.hostUid;
        room.host.name = "Host";
    }
    auto const* mine = group.member(uid);
    room.guest = mine ? mine->profile : s.profile;
    room.level = group.level;
    room.rules = group.rules;
    room.emote = group.emote;
    room.mapSelection = group.mapSelection;
    room.mapDraw = group.mapDraw;
    room.debug = group.debug;
    room.mapPool = group.mapPool;
    room.hostReady = room.guestReady = mine && mine->ready;
    // 전투 중이면 내 좌석 기준으로 level/rules/launch/battle을 덮어쓴다.
    if (group.battle) {
        auto const& battle = *group.battle;
        int const seat = battle.seatOf(uid);
        if (seat >= 0 && !(battle.seats[seat].state.forfeited && !battle.seats[seat].loaded)) {
            auto const& own = battle.seats[seat];
            room.level = battle.level;
            room.rules = battle.rules;
            room.started = battle.finishedAt == 0;
            LaunchInfo launch;
            launch.id = battle.id;
            launch.requestedAt = battle.requestedAt;
            launch.releasedAt = battle.releasedAt;
            launch.hostLoaded = launch.guestLoaded = own.loaded;
            room.launch = std::move(launch);
            BattleInfo info;
            info.id = battle.id;
            info.winnerUid = battle.winnerUid;
            info.draw = battle.draw;
            info.finishedAt = battle.finishedAt;
            info.timeLimitMs = battle.timeLimitMs;
            info.hostReturned = info.guestReturned = true;
            info.host = info.guest = own.state;
            room.battle = std::move(info);
        }
    }
    s.room = std::move(room);
    s.lastContact = Clock::now();
}

// 방에서 제거된 사유(kicked/banned/closed)에 맞는 안내 문구.
std::string groupRemovedNotice(std::string const& reason) {
    if (reason == "kicked") return "You were removed from the room by the host.";
    if (reason == "banned") return "You were banned from this room by the host.";
    if (reason == "closed") return "This room has closed.";
    return "Your room connection expired.";
}

// 응답에 removed 필드가 있으면 방에서 쫓겨났거나 방이 닫힌 것으로 보고 연결을 끊는다.
// 단, 전투 응답에서 온 "battle" 사유는 연결을 유지하고 방 전체 갱신만 앞당긴다.
bool groupRemoved(Json const& body, bool battleReply = false) {
    if (!body.isObject() || !body.contains("removed")) return false;
    auto const reason = boolAt(body, "banned") ? std::string("banned") : stringAt(body, "removed");
    if (reason == "battle" && battleReply) {
        state().groupRoomTime = kGroupRoomRefresh;
        return true;
    }
    disconnected(groupRemovedNotice(reason));
    return true;
}

// emote 목록 파싱. nonce가 없는 항목은 버린다.
std::vector<EmoteInfo> parseGroupEmotes(Json const& list) {
    std::vector<EmoteInfo> emotes;
    if (list.isArray())
        for (auto const& source : list.asArray().unwrap()) {
            if (!source.isObject()) continue;
            EmoteInfo emote;
            emote.uid = stringAt(source, "uid");
            emote.kind = stringAt(source, "kind");
            emote.text = stringAt(source, "text");
            emote.at = intAt(source, "at");
            emote.nonce = stringAt(source, "nonce");
            if (!emote.nonce.empty()) emotes.push_back(std::move(emote));
        }
    return emotes;
}

// lite 응답 반영: 준비/다운로드 진행/최고 기록/이모트만 멤버 인덱스 기준으로 갱신한다.
// 멤버 목록 자체는 바뀌지 않았다는 전제이다.
bool adoptGroupLite(Json const& body) {
    auto& s = state();
    if (!s.group) return false;
    auto& group = *s.group;
    std::unordered_set<int64_t> ready;
    if (body["ready"].isArray())
        for (auto const& index : body["ready"].asArray().unwrap()) ready.insert(index.asInt().unwrapOr(-1));
    for (size_t i = 0; i < group.members.size(); ++i) group.members[i].ready = ready.contains(static_cast<int64_t>(i));
    if (body["downloads"].isArray()) {
        auto const& downloads = body["downloads"].asArray().unwrap();
        for (size_t i = 0; i < group.members.size() && i < downloads.size(); ++i)
            group.members[i].download =
                static_cast<int>(std::clamp<int64_t>(downloads[i].asInt().unwrapOr(-1), -1, 100));
    }
    if (body["bests"].isArray()) {
        auto const& bests = body["bests"].asArray().unwrap();
        for (size_t i = 0; i < group.members.size() && i < bests.size(); ++i)
            group.members[i].best = static_cast<int>(std::clamp<int64_t>(bests[i].asInt().unwrapOr(-1), -1, 100));
    }
    group.emotes = parseGroupEmotes(body["emotes"]);
    if (group.emotes.empty())
        group.emote.reset();
    else
        group.emote = group.emotes.back();
    syncGroupRoom();
    return true;
}

// 방 전체 응답(GroupRoomInfo)을 반영한다. lite 응답은 adoptGroupLite로 넘긴다.
// 방이 바뀌었거나 전투가 바뀌면 ETag 캐시를 비워 다음 요청이 304로 오지 않게 한다.
bool adoptGroup(Json const& body) {
    auto& s = state();
    if (groupRemoved(body) || !body.isObject()) return false;
    if (boolAt(body, "lite")) return adoptGroupLite(body);
    GroupRoomInfo group;
    group.id = stringAt(body, "id");
    if (group.id.empty()) return false;
    group.name = stringAt(body, "name");
    group.hostUid = stringAt(body, "hostUid");
    group.privateRoom = boolAt(body, "private");
    group.maxPlayers =
        static_cast<int>(std::clamp<int64_t>(intAt(body, "max", kGroupMaxPlayers), kGroupMinPlayers, kGroupMaxPlayers));
    if (body["members"].isArray())
        for (auto const& entry : body["members"].asArray().unwrap()) {
            GroupMember member;
            member.profile = parseProfile(entry["profile"]);
            member.ready = boolAt(entry, "ready");
            member.wins = static_cast<int>(intAt(entry, "wins"));
            member.download = entry.contains("download")
                                  ? static_cast<int>(std::clamp<int64_t>(intAt(entry, "download"), -1, 100))
                                  : -1;
            member.best =
                entry.contains("best") ? static_cast<int>(std::clamp<int64_t>(intAt(entry, "best"), -1, 100)) : -1;
            if (!member.profile.uid.empty()) group.members.push_back(std::move(member));
        }
    if (body["level"].isObject()) group.level = parseLevel(body["level"]);
    group.rules = parseRules(body["rules"]);
    group.mapSelection = parseMapSelection(body["mapSelection"]);
    group.mapDraw = parseMapDraw(body["mapDraw"]);
    group.startAt = std::max<int64_t>(0, intAt(body, "startAt"));
    group.debug = boolAt(body, "debug");
    group.mapPool = parseMapPool(body["mapPool"]);
    group.emotes = parseGroupEmotes(body["emotes"]);
    if (!group.emotes.empty()) group.emote = group.emotes.back();
    // 같은 방의 이전 전투 정보를 넘겨 좌석 상태를 승계한다.
    GroupBattle const* previous = s.group && s.group->id == group.id && s.group->battle ? &*s.group->battle : nullptr;
    if (body["battle"].isObject()) group.battle = parseGroupBattle(body["battle"], previous);
    if (!s.group || s.group->id != group.id) {
        s.groupEtag.clear();
        s.groupBattleEtag.clear();
    } else if (!group.battle || !previous || previous->id != group.battle->id)
        s.groupBattleEtag.clear();
    s.group = std::move(group);
    syncGroupRoom();
    return true;
}

// 전투 전용 응답 반영. 전투 id가 다르면 방 전체를 다시 받도록 갱신을 앞당긴다.
// 진행 중이던 전투가 끝난 순간에도 방 전체를 갱신한다.
bool adoptGroupBattle(Json const& body) {
    auto& s = state();
    if (groupRemoved(body, true) || !s.group || !s.group->battle) return false;
    if (stringAt(body, "id") != s.group->battle->id) {
        s.groupRoomTime = kGroupRoomRefresh;
        return false;
    }
    bool const wasRunning = s.group->battle->finishedAt == 0;
    applyGroupRows(*s.group->battle, body);
    if (wasRunning && s.group->battle->finishedAt > 0) s.groupRoomTime = kGroupRoomRefresh;
    syncGroupRoom();
    return true;
}

// 방 액션(PUT groups/<id>/<action>)을 보낸다.
// 다른 쓰기/인증이 진행 중이면 deferRoomAction으로 미뤄 두고, 응답은 방 전체 상태로
// 반영한다. 요청 중 방 세션(epoch)이 바뀌면 결과를 버린다.
void groupAction(std::string action, Json body, Done callback) {
    auto& s = state();
    if (!s.group) {
        callback(false, "Room is unavailable.");
        return;
    }
    if (s.writing || s.authenticating) {
        deferRoomAction([action = std::move(action), body = std::move(body)](
                            Done done) mutable { groupAction(std::move(action), std::move(body), std::move(done)); },
                        std::move(callback));
        return;
    }
    auto const epoch = s.epoch;
    s.writing = true;
    ++s.revision;
    request(
        "PUT", "groups/" + s.group->id + "/" + action, std::move(body),
        [epoch, callback = std::move(callback)](web::WebResponse response) mutable {
            auto& s = state();
            if (epoch != s.epoch) {
                callback(false, "Room session changed.");
                return;
            }
            s.writing = false;
            auto parsed = response.json();
            if (!response.ok() || !parsed) {
                callback(false, networkError(response));
                return;
            }
            if (auto etag = response.header("ETag")) s.groupEtag = *etag;
            if (!adoptGroup(parsed.unwrap())) {
                if (!s.group)
                    callback(true, {});
                else
                    callback(false, "Invalid room response. Please retry.");
                return;
            }
            callback(true, {});
        },
        {}, false, false, false, {}, {}, s.groupEtag);
}

// 내 맵 다운로드 진행률(10% 단위)과 로컬 최고 기록을 서버에 알린다.
// 값이 바뀔 때만 보내며 최소 0.5초(전송 후 2초) 간격을 둔다.
void reportGroupDownload() {
    auto& s = state();
    if (!s.group || s.writing || s.polling || Clock::now() < s.nextGroupDownloadReport) return;
    s.nextGroupDownloadReport = Clock::now() + std::chrono::milliseconds(500);
    auto const& group = *s.group;
    if (group.level.id <= 0 || group.running()) return;
    auto const* mine = group.member(s.profile.uid);
    if (!mine) return;
    auto const progress = mapDownloadProgress(group.level.id);
    int const percent = progress.complete() ? 100 : std::clamp(progress.percent / 10 * 10, 0, 90);
    int const best = mapLocalBest(group.level.id);
    if (mine->download == percent && mine->best == best) return;
    s.nextGroupDownloadReport = Clock::now() + std::chrono::seconds(2);
    auto body = Json::object();
    body["levelId"] = group.level.id;
    body["percent"] = percent;
    body["best"] = best;
    groupAction("download", std::move(body), [](bool, std::string) {});
}

// 방 전체 상태를 GET으로 받는다. ETag를 보내 변화가 없으면 304로 받는다.
// revision이 달라졌으면(그 사이 쓰기가 있었음) 오래된 응답으로 보고 버린다.
void pollGroup() {
    auto& s = state();
    if (!s.group || s.polling || s.writing) return;
    s.polling = true;
    s.groupRoomTime = 0.f;
    s.pollTime = 0.f;
    auto const epoch = s.epoch;
    auto const revision = s.revision;
    request(
        "GET", "groups/" + s.group->id, {},
        [epoch, revision](web::WebResponse response) {
            auto& s = state();
            if (epoch != s.epoch) return;
            s.polling = false;
            if (revision != s.revision) return;
            if (response.code() == 304) {
                s.lastContact = Clock::now();
                return;
            }
            auto parsed = response.json();
            if (!response.ok() || !parsed) return;
            if (auto etag = response.header("ETag")) s.groupEtag = *etag;
            adoptGroup(parsed.unwrap());
        },
        {}, false, false, false, {}, {}, s.groupEtag);
}

// 진행 중 전투의 좌석 상태만 가볍게 폴링한다. 방 폴링과 같은 ETag/revision 규칙을 쓴다.
void pollGroupBattle() {
    auto& s = state();
    if (!s.group || !s.group->battle || s.polling || s.writing) return;
    s.polling = true;
    s.pollTime = 0.f;
    auto const epoch = s.epoch;
    auto const revision = s.revision;
    request(
        "GET", "groups/" + s.group->id + "/battle", {},
        [epoch, revision](web::WebResponse response) {
            auto& s = state();
            if (epoch != s.epoch) return;
            s.polling = false;
            if (revision != s.revision) return;
            if (response.code() == 304) {
                s.lastContact = Clock::now();
                return;
            }
            auto parsed = response.json();
            if (!response.ok() || !parsed) return;
            if (auto etag = response.header("ETag")) s.groupBattleEtag = *etag;
            adoptGroupBattle(parsed.unwrap());
        },
        {}, false, false, false, {}, {}, s.groupBattleEtag);
}

// 내 전투 진행도를 서버에 보고한다. 위치/카메라 좌표는 Group에서는 보내지 않는다.
// 다른 요청이 진행 중이면 실패로 돌려보내 호출 측이 다음 틱에 다시 시도하게 한다.
void reportGroupBattle(BattlePlayerState progress, Done callback) {
    auto& s = state();
    if (!s.group || !s.group->battle || s.group->battle->releasedAt <= 0) {
        callback(false, "The battle is no longer active.");
        return;
    }
    if (s.writing || s.polling || s.authenticating || Service::get().busy()) {
        callback(false, "Room update in progress.");
        return;
    }
    if (s.group->battle->finishedAt > 0) {
        callback(true, {});
        return;
    }
    progress.x = progress.y = progress.cameraX = progress.cameraY = 0.;
    progress.updatedAt = 0;
    auto body = Json::object();
    body["battle"] = s.group->battle->id;
    body["state"] = battlePlayerJson(progress);
    auto const epoch = s.epoch;
    s.writing = true;
    ++s.revision;
    request("PUT", "groups/" + s.group->id + "/report", std::move(body),
            [epoch, callback = std::move(callback)](web::WebResponse response) mutable {
                auto& s = state();
                if (epoch != s.epoch) {
                    callback(false, "Room session changed.");
                    return;
                }
                s.writing = false;
                s.pollTime = 0.f;
                auto parsed = response.json();
                if (!response.ok() || !parsed) {
                    callback(false, networkError(response));
                    return;
                }
                if (auto etag = response.header("ETag")) s.groupBattleEtag = *etag;
                adoptGroupBattle(parsed.unwrap());
                s.lastBattleReport = Clock::now();
                callback(true, {});
            });
}

// Group 방의 매 프레임 처리. 상황별로 어떤 폴링을 할지 고른다.
// 마지막 응답 후 60초가 지나면 연결이 끊긴 것으로 본다.
void groupTick(float dt) {
    auto& s = state();
    s.pollTime += dt;
    s.groupRoomTime += dt;
    if (Clock::now() - s.lastContact > std::chrono::seconds(60)) {
        disconnected("Connection lost. Please rejoin the room.");
        return;
    }
    // 서버 연결이 끊겼으면 2초 간격으로 재접속만 시도한다.
    if (!Service::get().connected()) {
        if (!s.authenticating && s.pollTime >= 2.f) {
            s.pollTime = 0;
            Service::get().connect([](bool, std::string) {});
        }
        return;
    }
    if (s.writing || s.polling || (Service::get().busy() && !s.searchingMap)) return;
    auto const& group = *s.group;
    // 포기(cancel) 예약이 있으면 전투가 끝날 때까지 2초 간격으로 재전송한다.
    if (!s.groupForfeitBattle.empty()) {
        bool const current = group.battle && group.battle->id == s.groupForfeitBattle && group.battle->finishedAt == 0;
        if (!current)
            s.groupForfeitBattle.clear();
        else if (Clock::now() >= s.nextGroupForfeit) {
            s.nextGroupForfeit = Clock::now() + std::chrono::seconds(2);
            auto body = Json::object();
            body["battle"] = s.groupForfeitBattle;
            groupAction("cancel", std::move(body), [battle = s.groupForfeitBattle](bool ok, std::string) {
                if (ok && state().groupForfeitBattle == battle) state().groupForfeitBattle.clear();
            });
            return;
        }
    }
    // playing: 내가 뛰는 중 / loading: 전원 로드 대기 / finalMissing: 종료됐지만 최종 상태를 아직 못 받음.
    int const seat = group.battle ? group.battle->seatOf(s.profile.uid) : -1;
    bool const playing = seat >= 0 && group.battle->finishedAt == 0 && group.battle->releasedAt > 0 &&
                         !group.battle->seats[seat].state.forfeited;
    bool const loading = seat >= 0 && group.battle->finishedAt == 0 && group.battle->releasedAt == 0;
    bool const finalMissing = seat >= 0 && group.battle->finishedAt > 0 && !group.battle->statesFinal;
    // 플레이 중에는 5초마다 방 전체, 그 외에는 1초마다 전투 상태만 폴링한다 (보고 직후 900ms는 건너뜀).
    if (playing) {
        if (s.groupRoomTime >= kGroupRoomRefresh)
            pollGroup();
        else if (s.pollTime >= kGroupBattlePoll && Clock::now() - s.lastBattleReport >= std::chrono::milliseconds(900))
            pollGroupBattle();
    } else if (finalMissing) {
        if (s.pollTime >= kGroupBattlePoll) pollGroupBattle();
    }
    // 관전 중이거나 포기한 좌석도 같은 방식으로 전투를 따라간다.
    else if (s.groupSpectating && group.battle && group.battle->finishedAt == 0 && group.battle->releasedAt > 0 &&
             (seat < 0 || group.battle->seats[seat].state.forfeited)) {
        if (s.groupRoomTime >= kGroupRoomRefresh)
            pollGroup();
        else if (s.pollTime >= kGroupBattlePoll)
            pollGroupBattle();
    }
    // 로비: 평소 1.5초, 시작 카운트다운 중 1초, 카운트다운이 끝나면 0.4초 간격.
    // 폴링할 일이 없으면 다운로드 진행률을 보고한다.
    else if (s.groupRoomTime >= (loading                    ? kGroupLaunchPoll
                                 : group.startAt <= 0       ? kGroupLobbyPoll
                                 : nowMs() >= group.startAt ? kGroupCountdownEndPoll
                                                            : kGroupLaunchPoll))
        pollGroup();
    else if (!loading)
        reportGroupDownload();
}

// 방에 새로 들어갈 때 이전 방 상태와 폴링 상태를 모두 초기화하고 응답을 반영한다.
void enterGroup(Json const& body, std::string etag) {
    auto& s = state();
    ++s.epoch;
    s.group.reset();
    s.room.reset();
    s.groupEtag = std::move(etag);
    s.groupBattleEtag.clear();
    s.groupForfeitBattle.clear();
    s.groupSpectating = false;
    s.pollTime = 0.f;
    s.groupRoomTime = 0.f;
    s.heartbeatTime = 0.f;
    s.lastContact = Clock::now();
    adoptGroup(body);
}
}

// 방 목록(GET groups)을 받아 GroupSummary로 변환한다. level이 없는 항목도 허용한다.
std::optional<GroupRoomInfo> const& Service::group() const {
    return state().group;
}
bool Service::inGroup() const {
    return state().group.has_value();
}
void Service::fetchGroups(std::function<void(std::vector<GroupSummary>, std::string)> callback) {
    connect([callback = std::move(callback)](bool ok, std::string error) mutable {
        if (!ok) {
            callback({}, std::move(error));
            return;
        }
        request("GET", "groups", {}, [callback = std::move(callback)](web::WebResponse response) mutable {
            auto parsed = response.json();
            if (!response.ok() || !parsed) {
                callback({}, networkError(response));
                return;
            }
            std::vector<GroupSummary> rooms;
            if (parsed.unwrap().isArray())
                for (auto const& entry : parsed.unwrap().asArray().unwrap()) {
                    if (!entry.isObject() || !entry["host"].isObject()) continue;
                    GroupSummary room;
                    room.id = stringAt(entry, "id");
                    room.name = stringAt(entry, "name");
                    room.privateRoom = boolAt(entry, "private");
                    room.started = boolAt(entry, "started");
                    room.host = parseProfile(entry["host"]);
                    room.count = static_cast<int>(intAt(entry, "count"));
                    room.maxPlayers = static_cast<int>(intAt(entry, "max", kGroupMaxPlayers));
                    if (entry["level"].isObject()) room.level = parseLevel(entry["level"]);
                    room.rules = parseRules(entry["rules"]);
                    room.random = boolAt(entry, "random");
                    room.debug = boolAt(entry, "debug");
                    if (!room.id.empty()) rooms.push_back(std::move(room));
                }
            callback(std::move(rooms), {});
        });
    });
}
// 방 관전용 조회(GET groups/<id>/watch). 이미 아는 전투가 있으면 ETag를 보내 304면 변경 없음으로 처리한다.
// 전투가 없고 알던 것도 없으면 실패로 돌려준다.
void Service::watchGroup(std::string roomId, GroupBattle known, std::string etag,
                         std::function<void(GroupWatch)> callback) {
    connect([roomId = std::move(roomId), known = std::move(known), etag = std::move(etag),
             callback = std::move(callback)](bool ok, std::string) mutable {
        if (!ok) {
            callback({});
            return;
        }
        std::string const sent = known.id.empty() ? std::string() : etag;
        request(
            "GET", "groups/" + roomId + "/watch", {},
            [known = std::move(known), sent, callback = std::move(callback)](web::WebResponse response) mutable {
                GroupWatch result;
                if (response.code() == 304) {
                    result.ok = true;
                    result.etag = sent;
                    callback(std::move(result));
                    return;
                }
                auto parsed = response.json();
                if (!response.ok() || !parsed || !parsed.unwrap().isObject()) {
                    callback(std::move(result));
                    return;
                }
                auto const& body = parsed.unwrap();
                result.ok = true;
                if (auto tag = response.header("ETag")) result.etag = *tag;
                if (body.contains("removed")) {
                    result.closed = true;
                    callback(std::move(result));
                    return;
                }
                if (body["battle"].isObject()) {
                    result.battle = parseGroupBattle(body["battle"], known.id.empty() ? nullptr : &known);
                    result.roomName = stringAt(body["room"], "name");
                } else if (!known.id.empty()) {
                    result.battle = std::move(known);
                } else {
                    result.ok = false;
                    callback(std::move(result));
                    return;
                }
                if (body["snapshot"].isObject()) applyGroupRows(result.battle, body["snapshot"]);
                result.changed = true;
                callback(std::move(result));
            },
            {}, false, false, false, {}, {}, sent);
    });
}
// Group 방을 만든다. 이미 방에 있거나 요청 중이면 거부하고, 디버그 방은 디버그 모드에서만 허용한다.
// 응답에 members가 있어야 성공으로 보고 enterGroup으로 상태를 초기화한다.
void Service::createGroup(std::string pin, int maxPlayers, bool allowHacks, Done callback, bool debug) {
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
    maxPlayers = std::clamp(maxPlayers, kGroupMinPlayers, kGroupMaxPlayers);
    connect([pin = std::move(pin), maxPlayers, allowHacks, debug,
             callback = std::move(callback)](bool ok, std::string error) mutable {
        if (!ok) {
            callback(false, std::move(error));
            return;
        }
        auto& s = state();
        if (s.writing || s.room) {
            callback(false, "Already joining a room.");
            return;
        }
        auto body = Json::object();
        body["profile"] = profileJson(s.profile);
        body["pin"] = pin;
        body["maxPlayers"] = maxPlayers;
        body["allowHacks"] = allowHacks;
        if (debug) body["debug"] = true;
        s.writing = true;
        request("PUT", "groups/create", std::move(body),
                [callback = std::move(callback)](web::WebResponse response) mutable {
                    auto& s = state();
                    s.writing = false;
                    auto parsed = response.json();
                    if (!response.ok() || !parsed || !parsed.unwrap().isObject() ||
                        !parsed.unwrap().contains("members")) {
                        callback(false, networkError(response));
                        return;
                    }
                    auto etag = response.header("ETag");
                    enterGroup(parsed.unwrap(), etag ? std::string(*etag) : std::string());
                    callback(s.group.has_value(),
                             s.group ? std::string() : std::string("Invalid room response. Please retry."));
                });
    });
}
// Group 방에 참가한다 (비공개 방은 pin 필요). 흐름은 createGroup과 같다.
void Service::joinGroup(std::string id, std::string pin, Done callback) {
    if (busy() || state().room) {
        callback(false, "Leave your current room first.");
        return;
    }
    connect(
        [id = std::move(id), pin = std::move(pin), callback = std::move(callback)](bool ok, std::string error) mutable {
            if (!ok) {
                callback(false, std::move(error));
                return;
            }
            auto& s = state();
            if (s.writing || s.room) {
                callback(false, "Already joining a room.");
                return;
            }
            auto body = Json::object();
            body["profile"] = profileJson(s.profile);
            body["pin"] = pin;
            s.writing = true;
            request("PUT", "groups/" + id + "/join", std::move(body),
                    [callback = std::move(callback)](web::WebResponse response) mutable {
                        auto& s = state();
                        s.writing = false;
                        auto parsed = response.json();
                        if (!response.ok() || !parsed || !parsed.unwrap().isObject() ||
                            !parsed.unwrap().contains("members")) {
                            callback(false, networkError(response));
                            return;
                        }
                        auto etag = response.header("ETag");
                        enterGroup(parsed.unwrap(), etag ? std::string(*etag) : std::string());
                        callback(s.group.has_value(),
                                 s.group ? std::string() : std::string("Invalid room response. Please retry."));
                    });
        });
}
// 방장 전용 액션들. 호스트가 아니면 요청 없이 바로 실패를 돌려준다.
// kick은 ban=false일 때만 body에 ban:false를 실어 보낸다.
void Service::kickGroupMember(std::string uid, bool ban, Done callback) {
    if (!inGroup() || !isHost()) {
        callback(false, "Only the host can remove players.");
        return;
    }
    auto body = Json::object();
    body["uid"] = std::move(uid);
    if (!ban) body["ban"] = false;
    groupAction("kick", std::move(body), std::move(callback));
}
void Service::handOverGroup(std::string uid, Done callback) {
    if (!inGroup() || !isHost()) {
        callback(false, "Only the host can hand over the room.");
        return;
    }
    auto body = Json::object();
    body["uid"] = std::move(uid);
    groupAction("host", std::move(body), std::move(callback));
}
// 방 규칙 변경. 시도 횟수 1~99, 목표 % 1~100, 인원은 허용 범위로 보정하고 속도/모드/시간제 시간은 검증한다.
void Service::configureGroup(int attempts, int maxPlayers, int speed, bool autoLives, int mode, bool practice,
                             int targetPercent, GameRules const& time, Done callback) {
    if (!inGroup() || !isHost()) {
        callback(false, "Only the host can change the room.");
        return;
    }
    if (!validRuleSpeed(speed)) {
        callback(false, "Invalid game rules.");
        return;
    }
    GameRules check = time;
    check.mode = mode;
    check.practice = practice;
    if (mode < 0 || mode > 2 || !validTimeRules(check)) {
        callback(false, "Invalid game rules.");
        return;
    }
    auto body = Json::object();
    body["attempts"] = std::clamp(attempts, 1, 99);
    body["mode"] = mode;
    body["practice"] = practice;
    // 1.0.8: 시간제의 시간(Fixed / Map %) - 다른 모드에서도 방이 기억한다. 시간제를 쓴 적 없는 방(기본값)에는
    // 보내지 않는다(서버가 처음 받는 값을 바뀐 규칙으로 보고 모두의 준비를 풀기 때문).
    if (mode == 2 || time.timeType != 0 || time.timeSeconds != kTimeDefaultSeconds ||
        time.timePercent != kTimeDefaultPercent) {
        body["timeType"] = time.timeType;
        body["timeSeconds"] = time.timeSeconds;
        body["timePercent"] = time.timePercent;
    }
    body["targetPercent"] = std::clamp(targetPercent, 1, 100);
    body["maxPlayers"] = std::clamp(maxPlayers, kGroupMinPlayers, kGroupMaxPlayers);
    body["speed"] = speed;
    body["autoLives"] = autoLives;
    groupAction("settings", std::move(body), std::move(callback));
}
void Service::stopGroupStart(Done callback) {
    if (!inGroup() || !isHost()) {
        callback(false, "Only the host can stop the countdown.");
        return;
    }
    groupAction("stop", Json::object(), std::move(callback));
}
