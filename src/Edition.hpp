// 에디션(멤버십 여부)에 따라 달라지는 닉네임, 오라 표시 함수 선언.
// 구현은 Edition.cpp. 이 빌드는 멤버십 에디션이 아니다.
#pragma once
#include "Cosmetics.hpp"
#include <algorithm>
#include <array>
#include <string>
#include <string_view>

namespace cocos2d {
class CCLabelBMFont;
class CCNode;
}
class SimplePlayer;

namespace versus {
// 이 빌드가 멤버십 에디션인지 여부.
constexpr bool isMembershipEdition() {
    return false;
}
// 빌드 종류를 나타내는 마커 문자열 (로그에 남긴다).
constexpr char const* editionMarker() {
    return "versus-edition:unified";
}
// 기본 그라데이션에서 position(0~1) 위치의 닉네임 색.
constexpr std::array<unsigned char, 3> nicknameColor(float position) {
    return gradientColor(position, {});
}
// 전적 목록에서 승자 쪽에 멤버십 표시를 보여줄지 판단한다.
// 상세 보기일 때만, 이긴 쪽의 멤버십 여부를 따른다.
constexpr bool historyWinnerMembership(std::string_view result, bool detailed, bool selfMembership,
                                       bool opponentMembership) {
    return detailed && ((result == "win" && selfMembership) || (result == "loss" && opponentMembership));
}
// 꾸미기 색 적용을 지원하는 BMFont 라벨을 만든다.
cocos2d::CCLabelBMFont* createCosmeticLabel(char const* text, char const* font);
// 닉네임 라벨에 글꼴과(멤버십이면) 그라데이션 색을 적용한다.
void styleNickname(cocos2d::CCLabelBMFont* label, bool membership, Cosmetics const& cosmetics = {});
// 닉네임 글꼴 번호의 .fnt 파일명. 기본(0)이거나 파일이 없으면 nullptr.
char const* nicknameFontFile(int font);
// 글꼴 번호의 표시 이름.
std::string nicknameFontName(int font);
// 아이콘에 선택한 오라 효과(이미지 + 파티클)를 붙인다.
void addMembershipAura(cocos2d::CCNode* icon, bool membership, Cosmetics const& cosmetics = {});
// 1.0.7: 프로필의 두 색과 Glow를 SimplePlayer에 적용한다(color1/color2/glow는 RGB 정수; glow < 0 = Glow 꺼짐).
// 모든 플레이어 아이콘(방·매치 판넬·HUD·결과·LIVE·순위·광장·관전)이 이 한 곳을 거친다 — 전에는 자리마다
// 흰 외곽선을 켜거나 끄거나 해서 GD의 Glow 설정이 반영되지 않았다.
void applyIconLook(SimplePlayer* icon, int color1, int color2, int glow);
}
