// 칭호(title)와 업적(achievement) 데이터 표.
// kTitles의 인덱스가 곧 칭호 ID이고, 업적 번호와의 변환 함수, 칭호 묶음(kSeries),
// 등급(tier) 계산을 함께 둔다. 칭호 ID 배치:
// 0 기본, 1~30 기존 업적, 31~34 멤버십, 35~74 나머지 업적, 75 개발자, 76 운영자, 77~98
// Season 2.
#pragma once
#include <array>
#include <cstdint>
#include <string_view>

namespace versus {
// 칭호 이름에 적용하는 시각 효과 종류와 앞에 붙는 아이콘 종류.
enum class TitleEffect : uint8_t {
    Plain,
    Pulse,
    Shimmer,
    Sparkle,
    Flame,
    Rainbow,
    Electric,
    Aurora,
    Royal,
    Shadow,
    Frost,
    Gilded,
    Diamond,
    Sky,
    Staff
};
enum class TitleIcon : uint8_t {
    Shield,
    Star,
    Sword,
    Crown,
    Flame,
    Bolt,
    Gem,
    Skull,
    Heart,
    Trophy,
    Moon,
    Coin,
    Clock,
    Wing,
    People,
    Home,
    Target
};

// 칭호 하나의 정의: 표시 이름, 업적 ID(없으면 빈 문자열), 달성 조건 문구, 효과, 아이콘,
// 색(0xRRGGBB).
struct TitleStyle {
    std::string_view name;
    std::string_view achievement;
    std::string_view requirement;
    TitleEffect effect;
    TitleIcon icon;
    uint32_t color;
};

// ID 구간 상수: 기존 업적 수, 멤버십 칭호 위치/개수, 업적 총수,
// 개발자/운영자 칭호 ID, Season 2 칭호 시작 ID와 개수, Season 1 랭크 칭호 시작 ID.
inline constexpr int kLegacyAchievementCount = 30;
inline constexpr int kFirstMembershipTitle = 31, kMembershipTitleCount = 4;
inline constexpr int kAchievementCount = 70;
inline constexpr int kDeveloperTitle = 75;
inline constexpr int kAdminTitle = 76;
inline constexpr int kFirstSeason2Title = 77, kSeason2AchievementCount = 22;
inline constexpr int kFirstS1RankTitle = 65;
inline constexpr int kAllAchievementCount = kAchievementCount + kSeason2AchievementCount;
// 칭호 정의 표. 순서가 칭호 ID와 같다.
inline constexpr std::array<TitleStyle, 99> kTitles{{
    {"Challenger", "", "Default title", TitleEffect::Plain, TitleIcon::Shield, 0x3A6EA5},
    {"Newcomer", "first-steps", "Play 1 Versus match", TitleEffect::Plain, TitleIcon::Star, 0x4C8C6A},
    {"Regular", "regular", "Play 10 matches", TitleEffect::Pulse, TitleIcon::Clock, 0x4F7FB8},
    {"Veteran", "veteran", "Play 50 matches", TitleEffect::Shimmer, TitleIcon::Shield, 0x6B6FCF},
    {"Versus Legend", "versus-legend", "Play 200 matches", TitleEffect::Aurora, TitleIcon::Crown, 0x7A4FC9},
    {"First Blood", "first-blood", "Win 1 match", TitleEffect::Pulse, TitleIcon::Sword, 0xA8373F},
    {"Rising Rival", "rising-rival", "Win 10 matches", TitleEffect::Shimmer, TitleIcon::Target, 0xB5552E},
    {"Champion", "champion", "Win 50 matches", TitleEffect::Royal, TitleIcon::Trophy, 0xB8892B},
    {"Conqueror", "conqueror", "Win 150 matches", TitleEffect::Royal, TitleIcon::Crown, 0xC2702A},
    {"On Fire", "on-fire", "Win 3 matches in a row", TitleEffect::Flame, TitleIcon::Flame, 0xC4471F},
    {"Unstoppable", "unstoppable", "Win 7 matches in a row", TitleEffect::Flame, TitleIcon::Bolt, 0xD8331C},
    {"Ranked Rookie", "ranked-rookie", "Play 1 Ranked match", TitleEffect::Plain, TitleIcon::Target, 0x2F7E92},
    {"Ladder Climber", "ladder-climber", "Play 25 Ranked matches", TitleEffect::Pulse, TitleIcon::Wing, 0x2D8F9C},
    {"Ranked Victor", "ranked-victor", "Win 10 Ranked matches", TitleEffect::Shimmer, TitleIcon::Trophy, 0x2E9A7A},
    {"Warlord", "warlord", "Win 50 Ranked matches", TitleEffect::Shadow, TitleIcon::Skull, 0x5B2A6E},
    {"Contender", "contender", "Reach 1600 rating", TitleEffect::Electric, TitleIcon::Bolt, 0x2E6FD0},
    {"Elite", "elite", "Reach 1800 rating", TitleEffect::Frost, TitleIcon::Gem, 0x3AA0D8},
    {"Grandmaster", "grandmaster", "Reach 2000 rating", TitleEffect::Rainbow, TitleIcon::Crown, 0x9B3FD0},
    {"Top 100", "top-100", "Reach global rank 100", TitleEffect::Royal, TitleIcon::Star, 0xA87A22},
    {"Top 10", "top-10", "Reach global rank 10", TitleEffect::Rainbow, TitleIcon::Trophy, 0xC9A23A},
    {"Finisher", "finisher", "Clear the map in 10 matches", TitleEffect::Pulse, TitleIcon::Target, 0x3D8B4F},
    {"Completionist", "completionist", "Clear the map in 50 matches", TitleEffect::Aurora, TitleIcon::Gem, 0x2F9E6E},
    {"First Try", "first-try", "Win by clearing with no deaths", TitleEffect::Sparkle, TitleIcon::Star, 0xC8A63A},
    {"Demon Slayer", "demon-slayer", "Win on a Demon level", TitleEffect::Shadow, TitleIcon::Skull, 0x8C1F2E},
    {"Extreme Executioner", "extreme-executioner", "Win on an Insane or Extreme Demon", TitleEffect::Flame,
     TitleIcon::Sword, 0x7A1020},
    {"Coin Collector", "coin-collector", "Collect 30 user coins in clears", TitleEffect::Sparkle, TitleIcon::Coin,
     0xB08A1E},
    {"Socialite", "socialite", "Face 10 different opponents", TitleEffect::Pulse, TitleIcon::People, 0xB0457A},
    {"Daily Duelist", "daily-duelist", "Play on 7 different days", TitleEffect::Shimmer, TitleIcon::Clock, 0x4A6FB0},
    {"Marathoner", "marathoner", "Play on 30 different days", TitleEffect::Aurora, TitleIcon::Heart, 0xC0406A},
    {"Stalemate", "stalemate", "Finish a match in a draw", TitleEffect::Frost, TitleIcon::Moon, 0x5A6A8A},
    {"Host", "host", "Host 10 Public Room matches", TitleEffect::Plain, TitleIcon::Home, 0x3F7F6F},
    {"Tipp7 Member", "", "Tipp7 Membership", TitleEffect::Gilded, TitleIcon::Crown, 0xC9982E},
    {"Starborn", "", "Tipp7 Membership", TitleEffect::Diamond, TitleIcon::Star, 0x3A55C8},
    {"Moonlit", "", "Tipp7 Membership", TitleEffect::Frost, TitleIcon::Moon, 0x3A4F8F},
    {"Aurora Walker", "", "Tipp7 Membership", TitleEffect::Aurora, TitleIcon::Wing, 0x2A7FA8},
    {"Eternal Duelist", "eternal-duelist", "Play 500 matches", TitleEffect::Rainbow, TitleIcon::Crown, 0x8A3FD8},
    {"Overlord", "overlord", "Win 300 matches", TitleEffect::Rainbow, TitleIcon::Crown, 0xD08A20},
    {"Rampage", "rampage", "Win 5 matches in a row", TitleEffect::Flame, TitleIcon::Flame, 0xD0581C},
    {"Godlike", "godlike", "Win 10 matches in a row", TitleEffect::Electric, TitleIcon::Bolt, 0xE0401A},
    {"Ladder Veteran", "ladder-veteran", "Play 100 Ranked matches", TitleEffect::Shimmer, TitleIcon::Wing, 0x2A86A8},
    {"Ladder Legend", "ladder-legend", "Play 250 Ranked matches", TitleEffect::Aurora, TitleIcon::Wing, 0x1F7FB8},
    {"Ranked Debut", "ranked-debut", "Win 1 Ranked match", TitleEffect::Plain, TitleIcon::Trophy, 0x2E8A70},
    {"Ranked Duelist", "ranked-duelist", "Win 25 Ranked matches", TitleEffect::Pulse, TitleIcon::Sword, 0x4A3A8E},
    {"Ranked Overlord", "ranked-overlord", "Win 100 Ranked matches", TitleEffect::Rainbow, TitleIcon::Skull, 0x6A1E8A},
    {"Mythic", "mythic", "Reach 2200 rating", TitleEffect::Aurora, TitleIcon::Gem, 0xB040D0},
    {"Number One", "number-one", "Reach global rank 1", TitleEffect::Rainbow, TitleIcon::Crown, 0xE0B030},
    {"Perfectionist", "perfectionist", "Clear the map in 100 matches", TitleEffect::Royal, TitleIcon::Target, 0x2F8F7E},
    {"Map Master", "map-master", "Clear the map in 250 matches", TitleEffect::Rainbow, TitleIcon::Gem, 0x22A07A},
    {"Coin Finder", "coin-finder", "Collect 10 user coins in clears", TitleEffect::Plain, TitleIcon::Coin, 0x9A7A26},
    {"Coin Hoarder", "coin-hoarder", "Collect 50 user coins in clears", TitleEffect::Royal, TitleIcon::Coin, 0xC09A1E},
    {"Treasure King", "treasure-king", "Collect 100 user coins in clears", TitleEffect::Rainbow, TitleIcon::Crown,
     0xD8AE20},
    {"Networker", "networker", "Face 25 different opponents", TitleEffect::Shimmer, TitleIcon::People, 0xA0407E},
    {"Celebrity", "celebrity", "Face 50 different opponents", TitleEffect::Sparkle, TitleIcon::Star, 0xC03C8A},
    {"Lifer", "lifer", "Play on 100 different days", TitleEffect::Rainbow, TitleIcon::Heart, 0xD04A70},
    {"Party Host", "party-host", "Host 25 Public Room matches", TitleEffect::Pulse, TitleIcon::Home, 0x3A8F78},
    {"Grand Host", "grand-host", "Host 50 Public Room matches", TitleEffect::Royal, TitleIcon::Home, 0x2E9F84},
    {"Clean Sweep", "clean-sweep", "Win 5 clears with no deaths", TitleEffect::Shimmer, TitleIcon::Star, 0xC0A040},
    {"Untouchable", "untouchable", "Win 15 clears with no deaths", TitleEffect::Rainbow, TitleIcon::Shield, 0xD8B850},
    {"Demon Hunter", "demon-hunter", "Win on 10 Demon levels", TitleEffect::Flame, TitleIcon::Skull, 0x9A1E30},
    {"Demon Lord", "demon-lord", "Win on 25 Demon levels", TitleEffect::Shadow, TitleIcon::Crown, 0x7A0E28},
    {"Extreme Master", "extreme-master", "Win on 5 Insane or Extreme Demons", TitleEffect::Flame, TitleIcon::Sword,
     0x8A0E1A},
    {"Mirror Match", "mirror-match", "Finish 5 matches in a draw", TitleEffect::Frost, TitleIcon::Moon, 0x56709A},
    {"Extreme Legend", "extreme-legend", "Win on 15 Insane or Extreme Demons", TitleEffect::Rainbow, TitleIcon::Flame,
     0xA01020},
    {"Equilibrium", "equilibrium", "Finish 10 matches in a draw", TitleEffect::Aurora, TitleIcon::Moon, 0x4A80B0},
    {"Met the Maker", "met-tipp7", "Meet Tipp7 in any room or match", TitleEffect::Rainbow, TitleIcon::Crown, 0xD0408A},
    {"S1 Easy", "s1-easy", "Reach Easy tier in Season 1", TitleEffect::Plain, TitleIcon::Star, 0x3C8CD8},
    {"S1 Normal", "s1-normal", "Reach Normal tier in Season 1", TitleEffect::Plain, TitleIcon::Star, 0x3FAE5A},
    {"S1 Hard", "s1-hard", "Reach Hard tier in Season 1", TitleEffect::Pulse, TitleIcon::Star, 0xC9A227},
    {"S1 Harder", "s1-harder", "Reach Harder tier in Season 1", TitleEffect::Shimmer, TitleIcon::Bolt, 0xD8652A},
    {"S1 Insane", "s1-insane", "Reach Insane tier in Season 1", TitleEffect::Sparkle, TitleIcon::Gem, 0xC23FA0},
    {"S1 Easy Demon", "s1-easy-demon", "Reach Easy Demon tier in Season 1", TitleEffect::Flame, TitleIcon::Skull,
     0x9C3A9E},
    {"S1 Medium Demon", "s1-medium-demon", "Reach Medium Demon tier in Season 1", TitleEffect::Electric,
     TitleIcon::Skull, 0x6A3FC0},
    {"S1 Hard Demon", "s1-hard-demon", "Reach Hard Demon tier in Season 1", TitleEffect::Shadow, TitleIcon::Skull,
     0xB0282E},
    {"S1 Insane Demon", "s1-insane-demon", "Reach Insane Demon tier in Season 1", TitleEffect::Royal, TitleIcon::Flame,
     0xC0391B},
    {"S1 Extreme Demon", "s1-extreme-demon", "Reach Extreme Demon tier in Season 1", TitleEffect::Rainbow,
     TitleIcon::Crown, 0x8E1020},
    {"The Maker", "", "Versus developer", TitleEffect::Sky, TitleIcon::Wing, 0x7CCBFF},
    {"Admin", "", "Versus staff", TitleEffect::Staff, TitleIcon::Shield, 0x4C8DFF},
    {"S2 Easy", "s2-easy", "Reach Easy in the Standard Cup (Season 2)", TitleEffect::Plain, TitleIcon::Star, 0x3C8CD8},
    {"S2 Normal", "s2-normal", "Reach Normal in the Standard Cup (Season 2)", TitleEffect::Pulse, TitleIcon::Star,
     0x3FAE5A},
    {"S2 Hard", "s2-hard", "Reach Hard in the Standard Cup (Season 2)", TitleEffect::Shimmer, TitleIcon::Star,
     0xC9A227},
    {"S2 Harder", "s2-harder", "Reach Harder in the Standard Cup (Season 2)", TitleEffect::Sparkle, TitleIcon::Bolt,
     0xD8652A},
    {"S2 Insane", "s2-insane", "Reach Insane in the Standard Cup (Season 2)", TitleEffect::Rainbow, TitleIcon::Gem,
     0xC23FA0},
    {"S2 Easy Demon", "s2-easy-demon", "Reach Easy Demon in the Demon Cup (Season 2)", TitleEffect::Flame,
     TitleIcon::Skull, 0x9C3A9E},
    {"S2 Medium Demon", "s2-medium-demon", "Reach Medium Demon in the Demon Cup (Season 2)", TitleEffect::Electric,
     TitleIcon::Skull, 0x6A3FC0},
    {"S2 Hard Demon", "s2-hard-demon", "Reach Hard Demon in the Demon Cup (Season 2)", TitleEffect::Shadow,
     TitleIcon::Skull, 0xB0282E},
    {"S2 Insane Demon", "s2-insane-demon", "Reach Insane Demon in the Demon Cup (Season 2)", TitleEffect::Royal,
     TitleIcon::Flame, 0xC0391B},
    {"S2 Extreme Demon", "s2-extreme-demon", "Reach Extreme Demon in the Demon Cup (Season 2)", TitleEffect::Aurora,
     TitleIcon::Crown, 0x8E1020},
    {"S2 G.O.A.T", "s2-grandpa-demon", "Reach G.O.A.T in the Demon Cup (Season 2)", TitleEffect::Rainbow,
     TitleIcon::Crown, 0xE0B030},
    {"Veto", "s2-veto", "Ban 10 maps in Season 2", TitleEffect::Pulse, TitleIcon::Target, 0x5A7FB0},
    {"Gatekeeper", "s2-gatekeeper", "Ban 50 maps in Season 2", TitleEffect::Royal, TitleIcon::Shield, 0x3A5FA0},
    {"Cup Contender", "s2-cup-contender", "Win 10 Standard Cup matches", TitleEffect::Shimmer, TitleIcon::Trophy,
     0xC2702A},
    {"Cup Champion", "s2-cup-champion", "Win 50 Standard Cup matches", TitleEffect::Royal, TitleIcon::Trophy, 0xD08A20},
    {"Demon Duelist", "s2-demon-duelist", "Win 10 Demon Cup matches", TitleEffect::Shadow, TitleIcon::Skull, 0x7A1E3A},
    {"Demon Champion", "s2-demon-champion", "Win 50 Demon Cup matches", TitleEffect::Rainbow, TitleIcon::Skull,
     0x9A1030},
    {"Heat Wave", "s2-heat-wave", "Win 5 Season 2 matches in a row", TitleEffect::Flame, TitleIcon::Flame, 0xD0581C},
    {"Inferno", "s2-inferno", "Win 10 Season 2 matches in a row", TitleEffect::Electric, TitleIcon::Bolt, 0xE0401A},
    {"Giant Slayer", "s2-giant-slayer", "Beat a player rated 2000+ higher in Season 2", TitleEffect::Sparkle,
     TitleIcon::Sword, 0xB040D0},
    {"Placed", "s2-placed", "Finish the placement games of a Season 2 cup", TitleEffect::Plain, TitleIcon::Target,
     0x3F8F6F},
    {"Double Cup", "s2-double-cup", "Finish the placement games of both Season 2 cups", TitleEffect::Aurora,
     TitleIcon::Crown, 0x2A86A8},
}};
// 칭호 ID가 특수 칭호(개발자/운영자)인지, 멤버십/Season 2 구간인지 판별한다.
constexpr bool isDeveloperTitle(int title) {
    return title == kDeveloperTitle;
}
constexpr bool isAdminTitle(int title) {
    return title == kAdminTitle;
}
constexpr bool isMembershipTitle(int title) {
    return title >= kFirstMembershipTitle && title < kFirstMembershipTitle + kMembershipTitleCount;
}
constexpr bool isSeason2Title(int title) {
    return title >= kFirstSeason2Title && title < kFirstSeason2Title + kSeason2AchievementCount;
}
// 칭호 ID에서 업적 번호(0부터)를 구한다. 업적과 연결되지 않은 칭호는 -1.
constexpr int achievementOfTitle(int title) {
    if (isSeason2Title(title)) return kAchievementCount + (title - kFirstSeason2Title);
    if (title <= 0 || isMembershipTitle(title) || title >= kDeveloperTitle) return -1;
    return title <= kLegacyAchievementCount ? title - 1 : title - 1 - kMembershipTitleCount;
}
// 업적 번호에서 칭호 ID를 구한다 (achievementOfTitle의 역변환).
constexpr int titleOfAchievement(int index) {
    if (index >= kAchievementCount) return kFirstSeason2Title + (index - kAchievementCount);
    return index < kLegacyAchievementCount ? index + 1 : index + 1 + kMembershipTitleCount;
}
// 표 구조가 위 변환 함수와 맞는지 컴파일 시점에 확인한다.
static_assert(titleOfAchievement(kAchievementCount - 1) == kDeveloperTitle - 1);
static_assert(kAdminTitle == kDeveloperTitle + 1 && kFirstSeason2Title == kAdminTitle + 1);
static_assert(static_cast<int>(kTitles.size()) == kFirstSeason2Title + kSeason2AchievementCount);
static_assert(kTitles[kFirstS1RankTitle].achievement == std::string_view("s1-easy") &&
              kTitles[kFirstS1RankTitle + 9].achievement == std::string_view("s1-extreme-demon"));
static_assert(achievementOfTitle(titleOfAchievement(kAllAchievementCount - 1)) == kAllAchievementCount - 1);

// 같은 계열의 칭호를 단계 순서로 묶은 것. titles의 앞 count개만 유효하다.
struct TitleSeries {
    std::string_view name;
    std::array<int, 10> titles;
    int count;
};
// 칭호 계열 목록 (선택 화면의 그룹 구성에 쓰인다).
inline constexpr std::array<TitleSeries, 27> kSeries{{
    {"Matches Played", {1, 2, 3, 4, 35}, 5},
    {"Victories", {5, 6, 7, 8, 36}, 5},
    {"Win Streak", {9, 37, 10, 38, 0}, 4},
    {"Ranked Matches", {11, 12, 39, 40, 0}, 4},
    {"Ranked Wins", {41, 13, 42, 14, 43}, 5},
    {"Rating", {15, 16, 17, 44, 0}, 4},
    {"Global Rank", {18, 19, 45, 0, 0}, 3},
    {"Map Clears", {20, 21, 46, 47, 0}, 4},
    {"User Coins", {48, 25, 49, 50, 0}, 4},
    {"Rivals", {26, 51, 52, 0, 0}, 3},
    {"Active Days", {27, 28, 53, 0, 0}, 3},
    {"Hosting", {30, 54, 55, 0, 0}, 3},
    {"Flawless Wins", {22, 56, 57, 0, 0}, 3},
    {"Demon Wins", {23, 58, 59, 0, 0}, 3},
    {"Extreme Wins", {24, 60, 62, 0, 0}, 3},
    {"Draws", {29, 61, 63, 0, 0}, 3},
    {"Membership", {31, 32, 33, 34, 0}, 4},
    {"Special", {64, 0, 0, 0, 0}, 1},
    {"Season 1 Rank", {65, 66, 67, 68, 69, 70, 71, 72, 73, 74}, 10},
    {"Season 2 Standard Cup", {77, 78, 79, 80, 81}, 5},
    {"Season 2 Demon Cup", {82, 83, 84, 85, 86, 87}, 6},
    {"Season 2 Map Bans", {88, 89}, 2},
    {"Standard Cup Wins", {90, 91}, 2},
    {"Demon Cup Wins", {92, 93}, 2},
    {"Season 2 Win Streak", {94, 95}, 2},
    {"Season 2 Upset", {96}, 1},
    {"Season 2 Placement", {97, 98}, 2},
}};
// kSeries에서 Season 2 계열이 시작하는 인덱스.
inline constexpr int kFirstSeason2Series = 19;
// Season 1/2 랭크 칭호가 표시할 티어 얼굴 번호(1~10)를 돌려준다. 해당하지 않으면 0.
constexpr int seasonRankFace(int title) {
    constexpr int faces[10] = {1, 2, 3, 4, 5, 7, 8, 6, 9, 10};
    if (title >= 65 && title <= 74) return faces[title - 65];
    constexpr int season2[11] = {1, 2, 3, 4, 5, 7, 8, 6, 9, 10, 10};
    return title >= kFirstSeason2Title && title < kFirstSeason2Title + 11 ? season2[title - kFirstSeason2Title] : 0;
}
// Season 2 최상위(G.O.A.T) 칭호 ID.
inline constexpr int kSeason2GoatTitle = kFirstSeason2Title + 10;
// 칭호의 등급(1~4 등)을 계산한다. 계열 안에서의 위치에 따라 올라가며,
// 멤버십 칭호와 Special 칭호는 4로 고정한다.
constexpr int titleTier(int title) {
    for (auto const& series : kSeries) {
        if (series.name == "Membership") continue;
        if (series.name == "Special" && series.titles[0] == title) return 4;
        for (int i = 0; i < series.count; ++i)
            if (series.titles[i] == title)
                return series.count <= 1 ? 1 : 1 + (i * 4 + (series.count - 1) / 2) / (series.count - 1);
    }
    return isMembershipTitle(title) ? 4 : 1;
}

// 업적 창의 분류 탭. 순서가 화면의 왼쪽→오른쪽 순서다.
enum class AchievementGroup : uint8_t { Season1, Season2, Versus };
inline constexpr int kAchievementGroupCount = 3;
inline constexpr std::array<std::string_view, kAchievementGroupCount> kAchievementGroupNames{"Season 1", "Season 2",
                                                                                             "Versus"};
// 칭호 계열이 들어가는 탭.
// Season 1: 시즌 1 랭크 경기로만 오르던 계열(랭크 경기 수·랭크 승리·레이팅·시즌 1 티어). 시즌 1이 끝나 더 오르지 않는다.
// Season 2: Season 2 계열 전부.
// Versus: 나머지 - 모든 경기에서 오르는 계열, 멤버십, Special, 그리고 시즌 2 순위로도 얻는 Global Rank.
constexpr AchievementGroup seriesGroup(int series) {
    if (series >= kFirstSeason2Series) return AchievementGroup::Season2;
    if (series < 0) return AchievementGroup::Versus;
    auto const name = kSeries[series].name;
    return name == "Ranked Matches" || name == "Ranked Wins" || name == "Rating" || name == "Season 1 Rank"
               ? AchievementGroup::Season1
               : AchievementGroup::Versus;
}
// 칭호가 속한 계열의 탭. 계열에 없는 칭호(기본·개발자·운영자)는 Versus.
constexpr AchievementGroup titleSeriesGroup(int title) {
    for (int s = 0; s < static_cast<int>(kSeries.size()); ++s)
        for (int i = 0; i < kSeries[s].count; ++i)
            if (kSeries[s].titles[i] == title) return seriesGroup(s);
    return AchievementGroup::Versus;
}
// 탭에 들어가는 업적 수(멤버십 칭호는 업적이 아니라 빼고 센다).
constexpr int groupAchievementCount(AchievementGroup group) {
    int total = 0;
    for (int s = 0; s < static_cast<int>(kSeries.size()); ++s)
        if (seriesGroup(s) == group)
            for (int i = 0; i < kSeries[s].count; ++i) total += achievementOfTitle(kSeries[s].titles[i]) >= 0;
    return total;
}
// 모든 업적의 칭호가 계열 표에 꼭 한 번씩 있는지(탭별 개수의 합이 전체와 맞으려면 필요하다).
constexpr bool everyAchievementInOneSeries() {
    std::array<int, kTitles.size()> seen{};
    for (auto const& series : kSeries)
        for (int i = 0; i < series.count; ++i) ++seen[series.titles[i]];
    for (int index = 0; index < kAllAchievementCount; ++index)
        if (seen[titleOfAchievement(index)] != 1) return false;
    return true;
}
static_assert(everyAchievementInOneSeries());
static_assert(groupAchievementCount(AchievementGroup::Season1) + groupAchievementCount(AchievementGroup::Versus) ==
              kAchievementCount);
static_assert(groupAchievementCount(AchievementGroup::Season2) == kSeason2AchievementCount);
static_assert(groupAchievementCount(AchievementGroup::Season1) == 23 &&
              titleSeriesGroup(74) == AchievementGroup::Season1 && titleSeriesGroup(45) == AchievementGroup::Versus &&
              titleSeriesGroup(kDeveloperTitle) == AchievementGroup::Versus);

// 업적 진행도: 현재 값, 목표 값, 달성 시각(미달성이면 0).
struct AchievementProgress {
    int progress = 0, goal = 1;
    int64_t unlockedAt = 0;
};
}
