"""Versus 서버의 대결 판정 규칙.

이 파일은 Versus 서버(비공개)에서 실제로 쓰는 함수들을 그대로 옮긴 것이다. 게임이 보내는 대결 진행 보고를
서버가 어떻게 검사하고, 승패를 어떻게 정하고, 게임의 안티치트가 보낸 감지 결과를 어떻게 다루는지가 전부
여기에 있다. 서버는 이 규칙으로 판정만 하고, 치트 감지 자체는 게임 안(src/ExternalCheatMonitor.cpp 등)에서
한다.

서버의 나머지(계정, 방, 매칭, 랭크 점수, 데이터베이스, 운영 도구)는 공개하지 않는다. 그쪽 코드와 맞닿는
부분은 outage_seen 하나뿐이며, 그 함수는 아래에 설명해 둔 대로 단순하게 두었다.

읽는 법:
- validate_battle_player / validate_battle: 보고의 형식 검사. 범위를 벗어나거나 앞뒤가 안 맞는 값은 거부된다.
- validate_battle_transition: 보고가 이전 상태에서 올 수 있는 변화인지 검사한다(최고 %는 줄지 않고, 목숨은
  한도를 넘지 않고, 확정된 결과는 바뀌지 않고, 안티치트 감지는 지울 수 없다). 그 뒤 resolve_battle로 결과를
  정하고, 게임이 보낸 결과 필드는 무시하고 서버가 정한 값으로 덮어쓴다.
- resolve_battle: 승패 판정. 기권/안티치트 > 일시정지 한도 > (퍼센트 모드) 목표 도달 순서 > (목숨 모드) 클리어
  횟수·목숨·최고 %·유저코인.
- disconnected_side / seat_stale: 한쪽의 소식이 BATTLE_STALE_MS 동안 끊기면 그쪽의 기권으로 처리한다.
"""
from __future__ import annotations

import copy
import json
import marshal
import math
import re
from typing import Any

HEX_ID = re.compile(r"^[0-9a-f]{32}$")

# 한 경기에서 한 사람이 쓸 수 있는 일시정지 시간. 저장된 값은 예전 한도(30초)까지 유효하다.
PAUSE_BUDGET_MS = 20_000
PAUSE_VALUE_MAX_MS = 30_000

# 대결이 아직 그 사람을 기다리는 동안 방 신호도 진행 보고도 이만큼 없으면 연결 끊김(기권)으로 본다.
BATTLE_STALE_MS = 30_000

# 게임의 안티치트가 보내는 감지 코드. 빈 문자열은 감지 없음.
INTEGRITY_CODES = {"", "eclipse:enabled",
                   "eclipse:attempt", "qol:enabled", "megahack:enabled", "megahack:attempt",
                   "prism:enabled", "mod:enabled"}
EXTERNAL_INTEGRITY_CODES = {"eclipse:enabled", "eclipse:attempt", "qol:enabled",
                            "megahack:enabled", "megahack:attempt", "prism:enabled", "mod:enabled"}

# 켜져 있기만 해도 치트로 보는 Geode 모드("mod:enabled", 값은 모드 ID). Percentage Toggle은 지금 게임이
# 보내지 않지만(대결 중에는 Versus가 그 모드를 멈춘다) 예전 보고를 위해 남겨 둔다.
BANNED_MOD_FEATURES = {"mythicalrev.noclip-at", "tobyadd.gdh", "zsa.percentage-toggle", "zilko.xdbot",
                       "peony.silicate", "decks.echo"}

# 각 핵 메뉴에서 치트로 보는 기능. GD 기본(1초)보다 느린 리스폰 시간도 치트다(Click On Steps는 아님).
# 지금 게임이 보내지 않는 이름도 예전 보고를 위해 남겨 둔다.
QOL_CHEAT_FEATURES = {
    "custom-respawn-time",
    "accurate-spike-hitboxes", "autoclicker", "auto-coins", "coin-tracers", "force-plat",
    "gravity-arrow", "hitbox-multiplier", "instant", "instant-reverse", "jump-hack",
    "noclip", "no-invisible-objects", "no-reverse", "show-hitboxes", "show-player",
    "show-trajectory", "tps-bypass", "speedhack",
    "show-layout", "layout-mode",
}
ECLIPSE_CHEAT_FEATURES = {
    "player.respawndelay.toggle", "global.clickonsteps", "global.tpsbypass.toggle", "level.autopickupcoins", "level.autopracticemode",
    "level.forceplatformer", "level.hitbox_multiplier", "level.practicecoins",
    "level.randomseed", "level.showhitboxes", "level.zeropercentpractice",
    "player.autoclick", "player.clicktp", "player.framestepper", "player.jumphack",
    "player.noclip", "player.noplatformersquish", "player.showtrajectory", "bot.playback",
    "global.speedhack.toggle", "bot.state",
    "level.showlayout", "level.layoutmode",
}
MEGAHACK_CHEAT_FEATURES = {
    "LEVEL/RESPAWN_TIME/HACK_RESPAWN_TIME",
    "LEVEL/AUTO_CLICKER/HACK_AUTO_CLICKER", "LEVEL/FRAME_STEPPER/HACK_FRAME_STEPPER",
    "LEVEL/HACK_CBF_GMVAR_COS", "LEVEL/HACK_FORCE_PLATFORMER",
    "LEVEL/HACK_INSTANT_COMPLETE", "LEVEL/HACK_JUMP_HACK",
    "LEVEL/HITBOX_MULTIPLIER/HACK_HITBOX_MULTIPLIER", "LEVEL/NOCLIP/HACK_NOCLIP",
    "LEVEL/NO_COLLISION/HACK_NO_COLLISION", "LEVEL/SHOW_HITBOXES/HACK_SHOW_HITBOXES",
    "LEVEL/SHOW_LAYOUT/HACK_SHOW_LAYOUT", "LEVEL/SHOW_TRAJECTORY/HACK_SHOW_TRAJECTORY",
    "REPLAY/PLAYBACK/HACK_REPLAY_PLAYBACK", "SPEEDHACK/HACK_SPEEDHACK_CLASSIC",
    "SPEEDHACK/LAYOUT/HACK_SPEEDHACK",
}
PRISM_CHEAT_FEATURES = {
    "Noclip", "Instant Complete", "No Spikes", "No Hitbox", "No Solids",
    "No Rotate", "Hide Player", "Freeze Player", "Jump Hack", "Layout Mode",
    "Show Hidden Objects", "Hide Level", "Change Gravity", "Force Platformer Mode",
    "No Mirror Transition", "Instant Mirror Portal", "Show Hitboxes", "No Shaders",
    "Playback", "Disable Camera Effects", "Auto Clicker", "Speedhack",
}
INTEGRITY_FEATURES = {
    "qol:enabled": QOL_CHEAT_FEATURES,
    "eclipse:enabled": ECLIPSE_CHEAT_FEATURES,
    "eclipse:attempt": ECLIPSE_CHEAT_FEATURES,
    "megahack:enabled": MEGAHACK_CHEAT_FEATURES,
    "megahack:attempt": MEGAHACK_CHEAT_FEATURES,
    "prism:enabled": PRISM_CHEAT_FEATURES,
    "mod:enabled": BANNED_MOD_FEATURES,
}

# 진행 보고 한 건이 가진 필드. "tps"(게임이 실제로 돌린 물리 스텝/초, GD는 240, 0 = 아직 측정 전)는 있어도
# 되고 없어도 된다.
PLAYER_KEYS = {
    "attemptsUsed", "bestPercent", "currentPercent", "runNumber", "inAttempt",
    "cleared", "forfeited", "spectating", "paused", "pausedAt", "pauseUsedMs", "pauseSeq", "pauseChangedAt", "x", "y",
    "cameraX", "cameraY", "updatedAt", "userCoins", "integrityCode", "integrityFeatures",
}
PLAYER_KEYS_TPS = PLAYER_KEYS | {"tps"}
TPS_MAX = 100_000


class Denied(Exception):
    pass


def is_int(value: Any) -> bool:
    return isinstance(value, int) and not isinstance(value, bool)


def finite_number(value: Any) -> bool:
    return isinstance(value, (int, float)) and not isinstance(value, bool) and math.isfinite(float(value))


def require(condition: bool, message: str = "Permission denied") -> None:
    if not condition:
        raise Denied(message)


# 감지 코드와 기능 목록(";"로 구분)이 서로 맞는지. 코드가 비어 있으면 기능도 없어야 하고, 코드가 있으면 그 메뉴의
# 알려진 기능 이름만, 겹치지 않게, 최소 하나는 있어야 한다.
def validate_integrity_features(code: Any, encoded: Any) -> None:
    require(isinstance(code, str) and code in INTEGRITY_CODES)
    require(isinstance(encoded, str) and len(encoded) <= 4096)
    allowed = INTEGRITY_FEATURES.get(code)
    if allowed is None:
        require(encoded == "", "Empty anti-cheat code cannot contain features")
        return
    values = encoded.split(";") if encoded else []
    require(0 < len(values) <= 48 and len(values) == len(set(values)),
            "A named banned feature is required")
    require(all(value in allowed for value in values), "Unknown anti-cheat feature")


def clone(value: Any) -> Any:
    """JSON 트리를 깊은 복사한다. dict/list/str/int/float/bool/None은 marshal로 빠르게, 그 외는 deepcopy."""
    try:
        return marshal.loads(marshal.dumps(value))
    except ValueError:
        return copy.deepcopy(value)


def canonical(value: Any) -> str:
    return json.dumps(value, ensure_ascii=False, separators=(",", ":"), sort_keys=True)


def same(left: Any, right: Any) -> bool:
    return canonical(left) == canonical(right)


_PLAYER_INT_KEYS = ("attemptsUsed", "bestPercent", "currentPercent", "runNumber", "pausedAt",
                    "pauseUsedMs", "updatedAt", "userCoins")
_PLAYER_BOOL_KEYS = ("inAttempt", "cleared", "forfeited", "spectating", "paused")
_PLAYER_POSITION_KEYS = ("x", "y", "cameraX", "cameraY")


# 한 자리의 진행 보고 형식 검사. 매초 들어오는 경로라 require() 대신 바로 검사한다(검사 순서와 문구는 같다).
def validate_battle_player(value: Any) -> None:
    if not (isinstance(value, dict) and (value.keys() == PLAYER_KEYS or value.keys() == PLAYER_KEYS_TPS)):
        raise Denied("Permission denied")
    if "tps" in value and not (is_int(value["tps"]) and 0 <= value["tps"] <= TPS_MAX):
        raise Denied("Permission denied")
    for key in _PLAYER_INT_KEYS:
        item = value[key]
        if not isinstance(item, int) or isinstance(item, bool):
            raise Denied("Permission denied")
    if not (0 <= value["attemptsUsed"] <= 1_000_000
            and 0 <= value["bestPercent"] <= 100 and 0 <= value["currentPercent"] <= value["bestPercent"]
            and 0 <= value["runNumber"] <= 1_000_000
            and 0 <= value["userCoins"] <= 3):
        raise Denied("Permission denied")
    for key in _PLAYER_BOOL_KEYS:
        if not isinstance(value[key], bool):
            raise Denied("Permission denied")
    validate_integrity_features(value["integrityCode"], value["integrityFeatures"])
    pause_seq = value["pauseSeq"]
    changed = value["pauseChangedAt"]
    # 안티치트 감지가 있으면 기권 상태여야 하고, 클리어는 100%여야 하고, 유저코인은 클리어했을 때만 센다.
    if not ((not value["integrityCode"] or value["forfeited"])
            and (not value["cleared"] or value["bestPercent"] == 100)
            and (value["cleared"] or value["userCoins"] == 0)
            and value["paused"] == (value["pausedAt"] > 0)
            and 0 <= value["pauseUsedMs"] <= PAUSE_VALUE_MAX_MS
            and is_int(pause_seq) and 0 <= pause_seq <= 1_000_000
            and is_int(changed) and changed >= 0):
        raise Denied("Permission denied")
    for key in _PLAYER_POSITION_KEYS:
        item = value[key]
        if not (finite_number(item) and -100_000_000 <= item <= 100_000_000):
            raise Denied("Permission denied")


# 대결 전체(양쪽 자리와 결과 필드)의 형식 검사. firstUid / activeUid는 예전 모드의 흔적으로 값만 확인한다.
def validate_battle(value: Any, room: dict[str, Any]) -> None:
    required = {"id", "firstUid", "activeUid", "firstReachedUid", "opponentRunAtFirst", "winnerUid", "draw", "finishedAt", "host", "guest"}
    require(isinstance(value, dict) and required <= set(value) <=
            required | {"hostReturned", "guestReturned", "hostReroll", "guestReroll", "rerolled"})
    # 각 자리의 마지막 Reroll 투표 시각(서버 시각, 0 = 없음)과 둘 다 리롤한 대결 표시.
    for key in ("hostReroll", "guestReroll"):
        require(is_int(value.get(key, 0)) and value.get(key, 0) >= 0)
    require(isinstance(value.get("rerolled", False), bool))
    require(isinstance(value["id"], str) and HEX_ID.fullmatch(value["id"]) is not None)
    participants = {room["host"]["uid"], room["guest"]["uid"]}
    require(value["firstUid"] in participants)
    require(value["activeUid"] in participants | {""})
    require(value["firstReachedUid"] in participants | {""})
    require(is_int(value["opponentRunAtFirst"]) and 0 <= value["opponentRunAtFirst"] <= 1_000_000)
    require(value["winnerUid"] in participants | {""})
    require(isinstance(value["draw"], bool) and is_int(value["finishedAt"]) and value["finishedAt"] >= 0)
    validate_battle_player(value["host"])
    validate_battle_player(value["guest"])
    require(isinstance(value.get("hostReturned", False), bool) and isinstance(value.get("guestReturned", False), bool))
    if value["finishedAt"] == 0:
        require(value["winnerUid"] == "" and not value["draw"])
    else:
        require((value["draw"] and value["winnerUid"] == "") or (not value["draw"] and value["winnerUid"] in participants))


# 이 자리의 플레이가 끝났는지: 기권, 클리어, 또는 (목숨 모드) 목숨을 다 씀.
def battle_terminal(player: dict[str, Any], rules: dict[str, Any]) -> bool:
    return bool(player["forfeited"] or player["cleared"] or (
        rules["mode"] == 0 and not rules["practice"] and player["attemptsUsed"] >= rules["attempts"]
    ))


# 목숨 모드의 조기 승리. attemptsUsed는 실패한 판 수다. 클리어한 쪽보다 상대가 이미 더 많이 실패했으면 상대는
# 따라잡을 수 없고, 상대가 목숨을 다 썼는데 내가 더 멀리 갔고 남은 목숨이 있으면 내가 이긴다.
def early_attempt_win(candidate: dict[str, Any], exhausted: dict[str, Any], rules: dict[str, Any]) -> bool:
    if rules["mode"] != 0 or rules["practice"] or candidate["forfeited"] or exhausted["forfeited"]:
        return False
    if candidate["cleared"] and exhausted["attemptsUsed"] > candidate["attemptsUsed"]:
        return True
    if exhausted["attemptsUsed"] < rules["attempts"] or candidate["bestPercent"] <= exhausted["bestPercent"]:
        return False
    spent = candidate["attemptsUsed"] + (1 if candidate["inAttempt"] else 0)
    return spent < exhausted["attemptsUsed"]


def out_of_lives(player: dict[str, Any], rules: dict[str, Any]) -> bool:
    return not player["cleared"] and not player["forfeited"] and player["attemptsUsed"] >= rules["attempts"]


# 목숨 모드: 목숨이 없는 쪽은 상대가 이미 그 최고 %를 넘었을 때만 바로 진다(진행 중인 판은 지금 %로 센다).
# 아니면 상대가 그 %를 넘거나 상대도 목숨을 다 쓸 때까지 계속한다.
def lives_exhausted(host: dict[str, Any], guest: dict[str, Any], rules: dict[str, Any]) -> bool:
    if rules["mode"] != 0 or rules["practice"]:
        return False
    return ((out_of_lives(host, rules) and live_score(guest)[0] > host["bestPercent"]) or
            (out_of_lives(guest, rules) and live_score(host)[0] > guest["bestPercent"]))


# 점수 (도달 %, -실패 횟수, 유저코인): 큰 쪽이 이긴다. 진행 중인 판은 지금 %로 센다.
def live_score(player: dict[str, Any]) -> tuple[int, int, int]:
    reached = max(player["bestPercent"], player["currentPercent"] if player["inAttempt"] else 0)
    if player["cleared"]:
        reached = 100
    return (reached, -player["attemptsUsed"], player["userCoins"])


def pause_expired(player: dict[str, Any], stamp: int) -> bool:
    elapsed = max(0, stamp - player["pausedAt"]) if player["paused"] and player["pausedAt"] > 0 else 0
    return player.get("pauseUsedMs", 0) + elapsed >= PAUSE_BUDGET_MS


# 플레이로 결과가 이미 확정된 자리(클리어, 목숨 소진, 목표 % 도달). 그 뒤의 일시정지 초과, 나가기, 연결 끊김은
# 이 자리를 패배로 바꾸지 못한다.
def final_by_play(player: dict[str, Any], rules: dict[str, Any]) -> bool:
    if player["forfeited"]:
        return False
    if player["cleared"]:
        return True
    if rules["mode"] == 0 and not rules["practice"] and player["attemptsUsed"] >= rules["attempts"]:
        return True
    return rules["mode"] == 1 and race_reached(player) >= rules["targetPercent"]


# 이 자리가 간 가장 먼 곳. 죽음과 다음 판 시작이 한 보고에 같이 오면 currentPercent는 0이지만 최고 %는 남아 있다.
def race_reached(player: dict[str, Any]) -> int:
    return 100 if player["cleared"] else max(player["bestPercent"], player["currentPercent"])


def pause_lost(player: dict[str, Any], rules: dict[str, Any], stamp: int) -> bool:
    return pause_expired(player, stamp) and not final_by_play(player, rules)


# 실제 서버에서는 마지막으로 본 시각을 서버 장애가 끝난 시각 아래로 내려가지 않게 보정한다(서버가 멈춘 동안은
# 아무도 연결 끊김이 되지 않도록). 그 부분은 운영 코드라 여기서는 그대로 돌려준다.
def outage_seen(seen: int, stamp: int) -> int:
    return seen


# 이 자리에서 방 신호도 진행 보고도 BATTLE_STALE_MS 동안 없었는지.
def seat_stale(room: dict[str, Any], side: str, stamp: int) -> bool:
    released = room.get("launch", {}).get("releasedAt", 0)
    if not is_int(released) or released <= 0:
        return False
    seen = outage_seen(max(room.get(f"{side}Seen", 0) or 0, room["battle"][side].get("updatedAt", 0) or 0,
                           released), stamp)
    return stamp - seen >= BATTLE_STALE_MS


# 관전 테스트에서 쓰는 "무제한" 목숨(보고가 셀 수 있는 최대).
ENDLESS_ATTEMPTS = 1_000_000


# 관전 테스트(예전 시즌 2 실험, 지금은 꺼져 있음)의 규칙이 적용되는 단계인지: 한쪽만 목숨을 다 쓰고 다른 쪽은
# 계속 플레이하는 상태.
def spectate_phase(battle: dict[str, Any], rules: dict[str, Any]) -> bool:
    def out(player: dict[str, Any]) -> bool:
        return not player["cleared"] and player["attemptsUsed"] >= rules["attempts"]
    host, guest = battle["host"], battle["guest"]
    host_out, guest_out = out(host), out(guest)
    if host_out != guest_out:
        return True
    if not host_out:
        return False
    return host["attemptsUsed"] != guest["attemptsUsed"] or bool(host["spectating"]) != bool(guest["spectating"])


# 관전 테스트에서 먼저 목숨을 다 쓴 자리(관전자). 둘 다 썼으면 한도를 넘은 쪽은 무제한 목숨이었던 쪽이고,
# 한도에서 같으면 관전 중이라고 한 쪽, 둘 다면 없음(동시에 끝남).
def spectator_side(battle: dict[str, Any], rules: dict[str, Any]) -> str | None:
    def out(player: dict[str, Any]) -> bool:
        return not player["cleared"] and not player["forfeited"] and player["attemptsUsed"] >= rules["attempts"]
    host, guest = battle["host"], battle["guest"]
    host_out, guest_out = out(host), out(guest)
    if host_out != guest_out:
        return "host" if host_out else "guest"
    if not host_out:
        return None
    if host["attemptsUsed"] != guest["attemptsUsed"]:
        return "host" if host["attemptsUsed"] < guest["attemptsUsed"] else "guest"
    if bool(host["spectating"]) != bool(guest["spectating"]):
        return "host" if host["spectating"] else "guest"
    return None


# 한 자리가 따르는 규칙. 관전 테스트에서 상대가 관전자가 되면 이 자리는 목숨이 무제한이다.
def seat_rules(room: dict[str, Any], side: str, spectate: bool) -> dict[str, Any]:
    rules = room["rules"]
    other = "guest" if side == "host" else "host"
    if spectate and spectator_side(room["battle"], rules) == other:
        endless = dict(rules)
        endless["attempts"] = ENDLESS_ATTEMPTS
        return endless
    return rules


# 연결 끊김으로 진 자리: 대결이 아직 그 자리를 기다리는데 소식이 끊겼고, 상대는 아직 연결돼 있을 때.
def disconnected_side(room: dict[str, Any], stamp: int, spectate: bool = False) -> str | None:
    battle = room["battle"]
    rules = room["rules"]
    for side, other in (("host", "guest"), ("guest", "host")):
        player = battle[side]
        if player["forfeited"] or battle_terminal(player, seat_rules(room, side, spectate)):
            continue
        if rules["mode"] == 1 and player["bestPercent"] >= rules["targetPercent"]:
            continue
        if seat_stale(room, side, stamp) and not seat_stale(room, other, stamp):
            return side
    return None


# 동점(둘 다 같은 횟수로 클리어, 또는 같은 %·같은 횟수)일 때 유저코인으로 가르는지. 모든 방에서 그렇다.
def coins_decide(room_id: str) -> bool:
    del room_id
    return True


# 관전 테스트 단계의 판정: 조기 종료 없이, 나가거나 끊긴 쪽이 생기면 각자 간 만큼으로 정한다(안티치트 기권은
# 그대로 패배). 일시정지 초과와 양쪽 종료는 평소와 같다.
def resolve_spectate(room: dict[str, Any], stamp: int, coins: bool = True) -> None:
    battle = room["battle"]
    host_uid, guest_uid = room["host"]["uid"], room["guest"]["uid"]
    host, guest = battle["host"], battle["guest"]
    if not host["forfeited"] and not guest["forfeited"]:
        gone = disconnected_side(room, stamp, True)
        if gone:
            lost = battle[gone]
            lost["forfeited"] = True
            lost["inAttempt"] = False
            lost["spectating"] = True
            if lost["paused"]:
                lost["pauseUsedMs"] = min(PAUSE_VALUE_MAX_MS, lost["pauseUsedMs"] + max(0, stamp - lost["pausedAt"]))
                lost["paused"] = False
                lost["pausedAt"] = 0
    host_rules, guest_rules = seat_rules(room, "host", True), seat_rules(room, "guest", True)
    winner, draw = "", False

    def by_score() -> tuple[str, bool]:
        host_score, guest_score = live_score(host), live_score(guest)
        if not coins:
            host_score, guest_score = host_score[:2], guest_score[:2]
        if host_score == guest_score:
            return "", True
        return (host_uid if host_score > guest_score else guest_uid), False

    if host["forfeited"] or guest["forfeited"]:
        if host["forfeited"] and host["integrityCode"]:
            winner = guest_uid
        elif guest["forfeited"] and guest["integrityCode"]:
            winner = host_uid
        elif host["forfeited"] and pause_expired(host, stamp):
            winner = guest_uid
        elif guest["forfeited"] and pause_expired(guest, stamp):
            winner = host_uid
        else:
            winner, draw = by_score()
    elif pause_lost(host, host_rules, stamp):
        winner = guest_uid
    elif pause_lost(guest, guest_rules, stamp):
        winner = host_uid
    elif battle_terminal(host, host_rules) and battle_terminal(guest, guest_rules):
        if host["cleared"] and guest["cleared"] and host["attemptsUsed"] != guest["attemptsUsed"]:
            winner = host_uid if host["attemptsUsed"] < guest["attemptsUsed"] else guest_uid
        else:
            winner, draw = by_score()
    if draw or winner:
        battle["draw"] = draw
        battle["winnerUid"] = winner
        battle["finishedAt"] = stamp


# 승패 판정. 결과가 나면 battle의 draw / winnerUid / finishedAt을 채운다. 이미 끝난 대결은 그대로 둔다.
def resolve_battle(room: dict[str, Any], actor_uid: str, stamp: int, spectate: bool = False,
                   coins: bool = True) -> None:
    battle = room["battle"]
    if battle["finishedAt"] > 0:
        return
    if spectate and spectate_phase(battle, room["rules"]):
        resolve_spectate(room, stamp, coins)
        return
    rules = room["rules"]
    host_uid = room["host"]["uid"]
    guest_uid = room["guest"]["uid"]
    host = battle["host"]
    guest = battle["guest"]
    winner = ""
    draw = False
    gone = None if host["forfeited"] or guest["forfeited"] else disconnected_side(room, stamp)
    if gone:
        # 연결이 끊긴 자리는 기권으로 기록한다(결과 화면에도 그렇게 보인다).
        lost = battle[gone]
        lost["forfeited"] = True
        lost["inAttempt"] = False
        lost["spectating"] = True
        if lost["paused"]:
            lost["pauseUsedMs"] = min(PAUSE_VALUE_MAX_MS, lost["pauseUsedMs"] + max(0, stamp - lost["pausedAt"]))
            lost["paused"] = False
            lost["pausedAt"] = 0
    if host["forfeited"]:
        winner = guest_uid
    elif guest["forfeited"]:
        winner = host_uid
    elif pause_lost(host, rules, stamp):
        winner = guest_uid
    elif pause_lost(guest, rules, stamp):
        winner = host_uid
    elif rules["mode"] == 1:
        # 퍼센트 모드: 목표 %에 먼저 닿은 쪽. 보고한 쪽의 보고는 보고한 쪽만 움직일 수 있으므로, 이미 목표에 서
        # 있는 자리가 먼저 간 것으로 본다(그 도달이 죽음 보고 안에 묻혀 왔을 수 있다). 도달 = 최고 %, 진행 중인
        # 판, 클리어.
        threshold = rules["targetPercent"]
        if not battle["firstReachedUid"]:
            actor_side = "host" if actor_uid == host_uid else "guest"
            for side in ("guest" if actor_side == "host" else "host", actor_side):
                if race_reached(battle[side]) >= threshold:
                    rival = battle["guest" if side == "host" else "host"]
                    battle["firstReachedUid"] = host_uid if side == "host" else guest_uid
                    battle["opponentRunAtFirst"] = rival["runNumber"] if rival["inAttempt"] else 0
                    if not rival["inAttempt"]:
                        winner = battle["firstReachedUid"]
                    break
        first = battle["firstReachedUid"]
        if first and not winner:
            # 상대는 첫 도달 때 목표 아래였으므로, 지금 도달했다면 그 답판에서 한 것이다(죽음과 다음 판 시작이
            # 한 보고에 같이 와도).
            other = guest if first == host_uid else host
            reply_run = battle["opponentRunAtFirst"]
            if reply_run > 0 and race_reached(other) >= threshold:
                draw = True
            elif reply_run == 0 or not other["inAttempt"] or other["runNumber"] > reply_run:
                winner = first
    else:
        # 목숨 모드.
        host_done = battle_terminal(host, rules)
        guest_done = battle_terminal(guest, rules)
        if host["cleared"] and guest["cleared"]:
            if host["attemptsUsed"] != guest["attemptsUsed"]:
                winner = host_uid if host["attemptsUsed"] < guest["attemptsUsed"] else guest_uid
            elif coins and host["userCoins"] != guest["userCoins"]:
                winner = host_uid if host["userCoins"] > guest["userCoins"] else guest_uid
            else:
                draw = True
        elif early_attempt_win(host, guest, rules):
            winner = host_uid
        elif early_attempt_win(guest, host, rules):
            winner = guest_uid
        elif (rules["practice"] and host["cleared"] and guest["cleared"]) or (not rules["practice"] and host_done and guest_done) \
                or lives_exhausted(host, guest, rules):
            host_score = live_score(host)
            guest_score = live_score(guest)
            if not coins:
                host_score, guest_score = host_score[:2], guest_score[:2]
            if host_score == guest_score:
                draw = True
            else:
                winner = host_uid if host_score > guest_score else guest_uid
    if draw or winner:
        battle["draw"] = draw
        battle["winnerUid"] = winner
        battle["finishedAt"] = stamp


def participant_side(room: dict[str, Any], uid: str) -> str | None:
    if room.get("host", {}).get("uid") == uid:
        return "host"
    if room.get("guest", {}).get("uid") == uid:
        return "guest"
    return None


# 대결이 시작될 때 각 자리의 첫 상태.
def initial_player(playing: bool = True, spectator: bool = False) -> dict[str, Any]:
    return {
        "attemptsUsed": 0,
        "bestPercent": 0,
        "currentPercent": 0,
        "runNumber": 1 if playing else 0,
        "inAttempt": playing,
        "cleared": False,
        "userCoins": 0,
        "forfeited": False,
        "spectating": spectator,
        "paused": False,
        "pausedAt": 0,
        "pauseUsedMs": 0,
        "pauseSeq": 0,
        "pauseChangedAt": 0,
        "x": 0,
        "y": 0,
        "cameraX": 0,
        "cameraY": 0,
        "updatedAt": 0,
        "integrityCode": "",
        "integrityFeatures": "",
    }


# 결과가 확정된 자리(클리어, 목숨 소진, 목표 도달, 기권)는 관전 모드로 맵을 계속 할 수 있지만, 그 뒤 보고가
# 결과를 바꾸지는 못한다.
def result_locked(player: dict[str, Any], rules: dict[str, Any]) -> bool:
    if player["forfeited"] or player["cleared"]:
        return True
    if rules["mode"] == 0 and not rules["practice"] and player["attemptsUsed"] >= rules["attempts"]:
        return True
    return rules["mode"] == 1 and player["bestPercent"] >= rules["targetPercent"]


BATTLE_OUTCOME_KEYS = ("finishedAt", "winnerUid", "draw", "firstReachedUid", "opponentRunAtFirst")
RESULT_LOCKED_KEYS = ("attemptsUsed", "bestPercent", "currentPercent", "runNumber", "inAttempt",
                      "cleared", "userCoins")


# 진행 보고 한 건을 받아 들일지. room은 서버가 가진 지금 상태, candidate는 게임이 보낸 대결 전체다. 받아 들이면
# 서버가 정한 결과를 덮어쓴 candidate를 돌려주고, 아니면 Denied를 던진다.
def validate_battle_transition(room: dict[str, Any], candidate: dict[str, Any], uid: str, stamp: int,
                               spectate: bool = False, coins: bool = True) -> dict[str, Any]:
    side = participant_side(room, uid)
    require(side is not None and room["started"] and room.get("launch", {}).get("releasedAt", 0) > 0)
    old = room["battle"]
    require(old["id"] == candidate.get("id") and old["finishedAt"] == 0)
    validate_battle(candidate, room)
    expected = clone(old)
    current = old[side]
    proposed = candidate[side]
    rules = seat_rules(room, side, spectate)
    # 되돌릴 수 없는 것들: 실패 횟수, 판 번호, 최고 %, 유저코인, 클리어, 기권, 안티치트 감지.
    require(proposed["attemptsUsed"] >= current["attemptsUsed"] and proposed["runNumber"] >= current["runNumber"])
    if result_locked(current, rules):
        for key in RESULT_LOCKED_KEYS:
            require(proposed[key] == current[key], "Result already locked")
    max_attempts = rules["attempts"] if rules["mode"] == 0 and not rules["practice"] else 1_000_000
    require(proposed["attemptsUsed"] <= max_attempts)
    require(proposed["bestPercent"] >= current["bestPercent"])
    require(proposed["userCoins"] >= current["userCoins"])
    require(not current["cleared"] or proposed["cleared"])
    require(not current["forfeited"] or proposed["forfeited"])
    require(proposed["integrityCode"] in INTEGRITY_CODES)
    validate_integrity_features(proposed["integrityCode"], proposed["integrityFeatures"])
    require(not proposed["integrityCode"] or proposed["forfeited"])
    require(not current["integrityCode"] or proposed["integrityCode"] == current["integrityCode"],
            "Anti-cheat result cannot be changed")
    require(not current["integrityCode"] or proposed["integrityFeatures"] == current["integrityFeatures"],
            "Anti-cheat features cannot be changed")
    require(proposed["updatedAt"] == stamp)
    # 일시정지: 쓴 시간은 줄지 않고, 매 변화마다 pauseSeq가 올라가야 하며, 변화 시각은 서버 시각 근처여야 한다.
    require(proposed["pauseUsedMs"] >= current["pauseUsedMs"], "Pause budget cannot be restored")
    require(proposed["pauseSeq"] >= current["pauseSeq"], "Stale pause revision")
    if proposed["pauseSeq"] > 0:
        if proposed["pauseSeq"] == current["pauseSeq"]:
            for key in ("paused", "pausedAt", "pauseChangedAt", "pauseUsedMs"):
                require(proposed[key] == current[key], "Pause edge must advance its revision")
        else:
            changed = proposed["pauseChangedAt"]
            require(current["pauseChangedAt"] <= changed <= stamp + 1500)
            require(changed >= stamp - 30_000, "Pause edge is too old; reconnect")
            if proposed["paused"]:
                require(proposed["pausedAt"] == changed)
            else:
                require(proposed["pausedAt"] == 0)
            if current["paused"] and proposed["pauseSeq"] == current["pauseSeq"] + 1:
                elapsed = max(0,changed-current["pausedAt"])
                require(proposed["pauseUsedMs"] >= min(PAUSE_BUDGET_MS,current["pauseUsedMs"]+elapsed-1500),
                        "Pause interval cannot be erased")
    elif proposed["paused"]:
        require(proposed["pausedAt"] == current["pausedAt"] or (not current["paused"] and proposed["pausedAt"] == stamp))
    else:
        require(proposed["pausedAt"] == 0)
        if current["paused"]:
            # 일시정지 시간은 서버 시계로 전부 센다. 게임이 마감에 맞춰 풀거나 적게 적어 보내도 소용없다.
            proposed["pauseUsedMs"] = min(PAUSE_VALUE_MAX_MS, max(proposed["pauseUsedMs"],
                current["pauseUsedMs"] + max(0, stamp - current["pausedAt"])))
    server_kept = False
    if proposed["forfeited"] and not current["forfeited"] and not proposed["integrityCode"] and \
            final_by_play(current, rules) and not (spectate and spectate_phase(room["battle"], room["rules"])):
        # 플레이로 결과가 이미 확정된 자리는 그 결과를 지킨다. 안티치트 감지가 없는 기권(관전 모드에서 일시정지를
        # 열어 둔 채 한도를 넘김 등)은 적용하지 않는다.
        proposed = clone(proposed)
        proposed["forfeited"] = False
        server_kept = True
    other_side = "guest" if side == "host" else "host"
    if not room["battle"][other_side]["spectating"]:
        for key in ("x", "y", "cameraX", "cameraY"):
            require(proposed[key] == current[key])
    expected[side] = clone(proposed)
    envelope = dict(room)  # resolve_battle은 따로 복사한 battle만 바꾼다
    envelope["battle"] = expected
    resolve_battle(envelope, uid, stamp, spectate, coins)
    expected = envelope["battle"]
    # 결과는 서버만 정한다. 게임이 스스로 낸 결과(옛 빌드의 규칙, 전송 중에 지난 일시정지 마감, 놓친 목표 도달)는
    # 버리고, 나머지 보고는 서버 계산과 같아야 한다.
    for key in BATTLE_OUTCOME_KEYS:
        candidate[key] = expected[key]
    if server_kept:
        candidate[side] = clone(expected[side])
    other_side = "guest" if side == "host" else "host"
    if (candidate["finishedAt"] == 0 and expected["finishedAt"] > 0 and
            expected[other_side]["forfeited"] and not room["battle"][other_side]["forfeited"]):
        # 연결 끊김 판정은 서버의 것이다. 끊긴 자리의 값도 서버 것으로 맞춘다.
        for key in ("finishedAt", "winnerUid", "draw"):
            candidate[key] = expected[key]
        candidate[other_side] = clone(expected[other_side])
    require(same(candidate, expected), "Invalid battle result")
    validate_battle(candidate, room)
    return candidate
