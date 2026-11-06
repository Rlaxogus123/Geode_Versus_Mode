// 클라이언트가 서버와 통신할 때 쓰는 접속 설정과 주기, 쿨다운 상수 모음.
#pragma once
#include <string_view>

namespace versus::firebase_config {
// 서버 주소, 웹 API 키, 데이터 루트 경로, 클라이언트 버전 문자열.
inline constexpr auto DATABASE_URL = "https://54.180.43.196";
inline constexpr auto WEB_API_KEY = "AIzaSyD1yoiFEhGa0oFm6XCzJ9DgRXs92UIvzpY";
inline constexpr auto ROOT = "versus-v1";
inline constexpr auto CLIENT_VERSION = "v1.0.6";
// 패치 노트를 자동으로 보여줄지 여부.
inline constexpr bool AUTO_PATCH_NOTES = std::string_view(CLIENT_VERSION) != "v0.5.32";
// 방 목록 최대 개수.
inline constexpr int ROOM_LIMIT = 200;
// 접속 상태(presence) 만료 시간(ms).
inline constexpr int PRESENCE_TIMEOUT_MS = 45'000;
// 방 상태 폴링 간격(초).
inline constexpr float ROOM_POLL_INTERVAL_SECONDS = .5f;
// 이모트 쿨다운(ms)과 그 값을 초로 환산한 것.
inline constexpr int EMOTE_COOLDOWN_MS = 334;
inline constexpr float EMOTE_COOLDOWN_SECONDS = EMOTE_COOLDOWN_MS / 1000.f;
// 방 목록 새로고침 버튼의 쿨다운(초).
inline constexpr float ROOM_LIST_REFRESH_COOLDOWN_SECONDS = 3.f;
}
