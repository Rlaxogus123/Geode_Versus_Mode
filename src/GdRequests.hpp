// GD 레벨 검색 / 다운로드 요청을 GD 델리게이트 자리와 상관없이 요청한 쪽에 돌려주는 라우터의 선언.
// GD의 GameLevelManager는 검색 응답을 m_levelManagerDelegate 한 자리로, 다운로드 응답을 m_levelDownloadDelegate
// 한 자리로만 돌려준다. 그 자리를 다른 화면(남아 있는 레벨 목록 / 레벨 정보)이나 다른 모드가 쥐고 있으면 비길 때까지
// 기다리던 Versus의 맵 검색과 다운로드가 그대로 멈췄다. 1.0.8부터는 응답을 요청 키로 구분해 보낸 쪽에 바로 넘긴다
// (응답을 넘기는 동안만 자리를 잠깐 바꿨다가 되돌린다). 자리가 비어 있을 때는 예전처럼 요청하는 동안 쥐어 두어,
// 라우터가 응답을 못 보는 경우에도 받을 수 있게 한다 - 다른 곳이 쥐고 있으면 기다리지 않고 그대로 둔다.
#pragma once
#include <string>

class GJSearchObject;
class LevelManagerDelegate;
class LevelDownloadDelegate;

namespace versus::gdnet {
// GD 온라인 레벨 검색을 보낸다. 응답(성공 / 실패)은 target의 LevelManagerDelegate 콜백으로 온다.
// GD가 같은 키를 이미 요청 중이면(다른 곳의 요청 포함) 보내지 않고 false.
bool search(GJSearchObject* search, LevelManagerDelegate* target);
// 레벨을 내려받는다. 응답은 target의 levelDownloadFinished / levelDownloadFailed로 온다. 이미 요청 중이면 false.
bool download(int levelID, LevelDownloadDelegate* target);
// 요청을 마친 target이 쥐고 있던 GD의 델리게이트 자리를 비운다(다른 곳이 쥐었으면 그대로 둔다).
void release(LevelManagerDelegate* target);
void release(LevelDownloadDelegate* target);
// 레벨 다운로드 키("<ID>_0_0", GD의 getLevelDownloadKey와 같다).
std::string downloadKey(int levelID);
// 지금 넘기고 있는 응답의 키. 라우팅된 콜백 안에서만 값이 있고 그 밖에서는 빈 문자열.
std::string const& deliveringKey();
// 다운로드 키의 레벨 ID (키가 아니면 0).
int levelOfKey(std::string const& key);
// key의 응답을 더는 받지 않는다. 늦게 온 응답은 아무 델리게이트에도 가지 않는다
// (다운로드라면 GD는 그래도 레벨을 저장해 둔다).
void drop(std::string const& key);
// GD가 key를 요청 중인 것으로 들고 있는지.
bool active(std::string const& key);
// 응답이 오지 않는 요청을 다시 보낼 수 있게 GD의 요청 중 목록에서 key를 지운다.
void forget(std::string const& key);
}
