// Versus 모드의 메인 UI 구현 파일. 로비/방 목록/방 상세/랭크/히스토리/프로필 등
// 대부분의 팝업과 레이어(VersusLayer 계열)와, 그 아래에서 쓰는 UI 헬퍼를 담고 있다.
// 파일 앞부분은 익명 namespace 안의 공용 헬퍼(label, panel, button, TapScrollLayer 등)이고,
// 뒤쪽에서 이를 조합해 화면을 만든다. 서버 통신은 VersusService, 전투 진행은
// BattleSession/MatchLaunch 쪽에 맡기고 여기서는 표시와 입력 처리만 한다.
#include "VersusUI.hpp"
#include "DebugMode.hpp"
#include "KeyDefer.hpp"
#include "DeferredPopup.hpp"
#include "SystemText.hpp"
#include "Particles.hpp"
#include <random>
#include <unordered_map>
#include <unordered_set>
#include "ExecutionEffects.hpp"
#include "VersusService.hpp"
#include "FirebaseConfig.hpp"
#include "LevelSelector.hpp"
#include "MatchLaunch.hpp"
#include "GroupFreePlay.hpp"
#include "Replay.hpp"
#include "WeakRefs.hpp"
#include "MapCache.hpp"
#include "BattleLevelData.hpp"
#include "RoomControls.hpp"
#include "RoomPin.hpp"
#include "VersusAudio.hpp"
#include "RandomMapPolicy.hpp"
#include "RandomMapSearch.hpp"
#include "MapPool.hpp"
#include "RankedConfig.hpp"
#include "BattleHUD.hpp"
#include "ExternalCheatMonitor.hpp"
#include "IntegrityCodes.hpp"
#include "IntegrityFeatures.hpp"
#include "SpectatorRunner.hpp"
#include "Edition.hpp"
#include "MenuCursor.hpp"
#include "Minesweeper.hpp"
#include "update/UpdateManager.hpp"
#include "VectorArt.hpp"
#include "Emotes.hpp"
#include "GroupResult.hpp"
#include "BattleProgress.hpp"
#include "BattleSession.hpp"
#include "PortableActions.hpp"
#include "Season2Lounge.hpp"
#include "GoatFace.hpp"
#include "EditorPlaza.hpp"
#include "Season2Result.hpp"

#include <Geode/Geode.hpp>
#include <Geode/binding/CreatorLayer.hpp>
#include <Geode/binding/LevelInfoLayer.hpp>
#include <Geode/binding/GJGameLevel.hpp>
#include <Geode/binding/GameManager.hpp>
#include <Geode/binding/MusicDownloadManager.hpp>
#include <Geode/binding/SongInfoObject.hpp>
#include <Geode/binding/GJDifficultySprite.hpp>
#include <Geode/binding/SimplePlayer.hpp>
#include <Geode/binding/ProfilePage.hpp>
#include <Geode/ui/General.hpp>
#include <Geode/ui/Popup.hpp>
#include <Geode/ui/ScrollLayer.hpp>
#include <Geode/ui/TextInput.hpp>
#include <Geode/ui/ColorPickPopup.hpp>
#include <Geode/ui/BasedButtonSprite.hpp>
#include <Geode/ui/OverlayManager.hpp>
#include <Geode/binding/Slider.hpp>
#include <Geode/binding/GJAccountManager.hpp>
#include <Geode/binding/CCCircleWave.hpp>
#if VERSUS_ARGON
#include <argon/argon.hpp>
#endif

#include <algorithm>
#include <deque>
#include <set>
#include <array>
#include <cctype>
#include <charconv>
#include <cmath>
#include <chrono>
#include <ctime>
#include <limits>
#include <tuple>
#include <utility>

using namespace geode::prelude;

namespace versus {
namespace {

// 한 페이지에 보여 주는 방 수 / 히스토리 항목 수.
constexpr int kRoomPageSize = 4;
constexpr int kHistoryPageSize = 5;
// UI 공통 색상 (밝은 얼음색 / 흐린 보조색).
constexpr ccColor3B kIce = {155, 225, 255};
constexpr ccColor3B kMuted = {150, 185, 225};
// 퀵챗 프리셋 문구 (방 안에서 버튼 하나로 보낼 수 있는 고정 메시지).
constexpr std::array<char const*, 12> kQuickChat = {"Hello!",    "Good Luck!", "Have Fun!",    "Pro",
                                                    "Respect",   "Let's go!",  "I have to go", "Bye!",
                                                    "One more?", "Yes!",       "No!",          "GG"};

// 0xRRGGBB 정수를 ccColor3B로 변환한다.
ccColor3B unpackColor(int color) {
    return ccc3((color >> 16) & 255, (color >> 8) & 255, color & 255);
}

// parent에 BMFont 라벨을 붙여 반환한다. maxWidth > 0이면 그 폭을 넘지 않도록 scale을 줄이고,
// left가 true면 왼쪽 기준 정렬(anchor)로 둔다.
CCLabelBMFont* label(CCNode* parent, std::string const& text, CCPoint position, float scale = .45f,
                     float maxWidth = 0.f, ccColor3B color = {255, 255, 255}, bool left = false,
                     char const* font = "bigFont.fnt") {
    auto* value = createCosmeticLabel(text.c_str(), font);
    value->setPosition(position);
    value->setScale(scale);
    value->setColor(color);
    if (left) value->setAnchorPoint({0.f, .5f});
    if (maxWidth > 0.f && value->getContentSize().width * scale > maxWidth) {
        value->setScale(maxWidth / value->getContentSize().width);
    }
    parent->addChild(value);
    return value;
}

// 레벨 난이도(오토 포함)에 맞는 GJDifficultySprite를 만들고 feature 상태(0~4)를 반영한다.
GJDifficultySprite* mapDifficulty(LevelInfo const& level) {
    auto* face =
        GJDifficultySprite::create(level.autoLevel ? -1 : std::clamp(level.difficulty, 0, 10), GJDifficultyName::Short);
    if (face) face->updateFeatureState(static_cast<GJFeatureState>(std::clamp(level.featureState, 0, 4)));
    return face;
}

// 별 개수 텍스트 + 작은 별 아이콘을 가로로 묶은 노드를 만든다. maxWidth를 넘으면 전체를 축소한다.
CCNode* starRating(CCNode* parent, std::string const& stars, CCPoint position, float scale = .34f,
                   float maxWidth = 0.f) {
    auto* group = CCNode::create();
    group->setAnchorPoint({.5f, .5f});
    group->setPosition(position);
    auto* number = label(group, stars, {}, scale, 0.f, ccWHITE, false, "goldFont.fnt");
    auto const textSize = number->getScaledContentSize();
    auto* star = CCSprite::createWithSpriteFrameName("star_small01_001.png");
    float const iconHeight = std::max(7.f, textSize.height * .85f);
    float width = textSize.width;
    float height = textSize.height;
    if (star) {
        star->setScale(iconHeight / std::max(1.f, star->getContentSize().height));
        auto const iconSize = star->getScaledContentSize();
        width += 2.f + iconSize.width;
        height = std::max(height, iconSize.height);
        star->setPosition({textSize.width + 2.f + iconSize.width / 2.f, height / 2.f});
        group->addChild(star);
    }
    number->setPosition({textSize.width / 2.f, height / 2.f});
    group->setContentSize({width, height});
    if (maxWidth > 0.f && width > maxWidth) group->setScale(maxWidth / width);
    parent->addChild(group);
    return group;
}

// 유저 코인 개수(1~3)를 코인 아이콘과 숫자로 표시한다. 코인이 없으면 아무것도 그리지 않는다.
void mapCoinCount(CCNode* parent, LevelInfo const& level, CCPoint position, float scale = .32f) {
    if (level.userCoins <= 0) return;
    auto* group = CCNode::create();
    group->setID("map-user-coin-count"_spr);
    group->setPosition(position);
    parent->addChild(group);
    auto* coin = CCSprite::createWithSpriteFrameName("usercoin_small01_001.png");
    if (coin) {
        coin->setScale(14.f * scale / .32f / std::max(1.f, coin->getContentSize().height));
        coin->setPosition({-9.f, 0.f});
        group->addChild(coin);
    }
    label(group, std::to_string(std::clamp(level.userCoins, 0, 3)), {7.f, 0.f}, scale, 16.f);
}

// 필터 알약(pill) 하나: 텍스트, 선택적 아이콘 프레임 이름, 켜짐 여부.
struct FilterPill {
    std::string text;
    char const* icon;
    bool on;
};
// 알약들을 center 기준으로 한 줄에 배치한다. 전체 폭이 width를 넘으면 텍스트 scale만 줄여
// (최소 .12) 맞추고, 꺼진 알약은 어둡게 표시한다.
void filterPills(CCNode* parent, std::vector<FilterPill> const& pills, CCPoint center, float width, float height,
                 float scale, float gap = 3.f) {
    if (pills.empty()) return;
    constexpr float kPad = 7.f;
    std::vector<float> textW, iconW;
    float fixed = gap * static_cast<float>(pills.size() - 1), text = 0.f;
    for (auto const& pill : pills) {
        float const t =
            pill.text.empty() ? 0.f : createCosmeticLabel(pill.text.c_str(), "bigFont.fnt")->getContentSize().width;
        float i = 0.f;
        if (pill.icon)
            if (auto* icon = loadedFrameSprite(pill.icon))
                i = icon->getContentSize().width * (height - 6.f) / std::max(1.f, icon->getContentSize().height) +
                    (t > 0.f ? 3.f : 0.f);
        textW.push_back(t);
        iconW.push_back(i);
        fixed += i + kPad;
        text += t;
    }
    float const s = std::clamp(text > 0.f ? (width - fixed) / text : scale, .12f, scale);
    float const total = fixed + text * s;
    float x = center.x - total / 2.f;
    for (size_t k = 0; k < pills.size(); ++k) {
        float const w = textW[k] * s + iconW[k] + kPad;
        auto* plate = NineSlice::create("square02b_001.png");
        plate->setContentSize({w, height});
        plate->setPosition({x + w / 2.f, center.y});
        plate->setColor(pills[k].on ? ccc3(38, 118, 205) : ccc3(10, 26, 62));
        plate->setOpacity(pills[k].on ? 235 : 205);
        parent->addChild(plate);
        float cursor = x + kPad / 2.f;
        if (pills[k].icon)
            if (auto* icon = loadedFrameSprite(pills[k].icon)) {
                icon->setScale((height - 6.f) / std::max(1.f, icon->getContentSize().height));
                float const iw = icon->getScaledContentSize().width;
                icon->setPosition({cursor + iw / 2.f, center.y});
                if (!pills[k].on) icon->setColor(ccc3(95, 95, 95));
                parent->addChild(icon);
                cursor += iw + 3.f;
            }
        if (!pills[k].text.empty())
            label(parent, pills[k].text, {cursor + textW[k] * s / 2.f, center.y + .5f}, s, 0.f,
                  pills[k].on ? ccWHITE : ccc3(105, 115, 135));
        x += w + gap;
    }
}
// 랜덤 맵 필터(난이도 구성/길이/레이티드/2P/최소 좋아요)를 요약해 보여 주는 카드를 그린다.
// stacked이면 세로로 쌓은 배치, 아니면 하단 한 줄 배치를 쓴다.
void randomFilterCard(CCNode* parent, MapSelection const& selection, CCRect area, bool stacked) {
    float const top = area.getMaxY(), x0 = area.getMinX(), w = area.size.width, cx = area.getMidX();
    auto const entries = randomFilterPreview(selection);
    int const count = std::max(1, static_cast<int>(entries.size()));
    float const cell = std::min(48.f, w / count), faceH = stacked ? 24.f : 30.f;
    for (int i = 0; i < static_cast<int>(entries.size()); ++i) {
        float const x = cx + (i - (count - 1) / 2.f) * cell;
        if (auto* face = GJDifficultySprite::create(CATEGORY_FACES[entries[i].category], GJDifficultyName::Short)) {
            auto const size = face->getContentSize();
            face->setScale(
                std::min({.8f, (cell - 4.f) / std::max(1.f, size.width), faceH / std::max(1.f, size.height)}));
            face->setPosition({x, top - faceH / 2.f - 1.f});
            face->setID(fmt::format("random-filter-face-{}", entries[i].category));
            parent->addChild(face);
        }
        starRating(parent, entries[i].stars, {x, top - faceH - 7.f}, .26f, cell - 2.f)
            ->setID(fmt::format("random-filter-stars-{}", entries[i].category));
    }
    // 길이 알약 목록 생성. initials가 true면 T/S/M/L/XL 약어를 쓰고, 마지막에 레이티드 별 알약을 붙인다.
    auto lengthPills = [&](bool initials) {
        static constexpr char const* names[] = {"Tiny", "Short", "Medium", "Long", "XL"};
        static constexpr char const* letters[] = {"T", "S", "M", "L", "XL"};
        std::vector<FilterPill> pills;
        for (int length = 0; length <= 4; ++length)
            pills.push_back({(initials ? letters : names)[length], nullptr, (selection.lengths & (1 << length)) != 0});
        pills.push_back({"", "GJ_starsIcon_001.png", selection.rated});
        return pills;
    };
    auto lengths = lengthPills(false);
    std::string likes = selection.minLikes <= 0          ? std::string("Any")
                        : selection.minLikes % 1000 == 0 ? fmt::format("{}K+", selection.minLikes / 1000)
                                                         : fmt::format("{}+", selection.minLikes);
    std::vector<FilterPill> others{{selection.twoPlayer == 1   ? "No 2P"
                                    : selection.twoPlayer == 2 ? "2P only"
                                                               : "2P: Any",
                                    nullptr, selection.twoPlayer != 0},
                                   {likes, "GJ_likesIcon_001.png", selection.minLikes > 0}};
    auto box = [&](CCPoint center, CCSize size) {
        auto* plate = NineSlice::create("square02b_001.png");
        plate->setContentSize(size);
        plate->setPosition(center);
        plate->setColor(ccc3(0, 0, 0));
        plate->setOpacity(80);
        parent->addChild(plate);
    };
    // 시계 아이콘 + 길이 알약 행. 이름 전체가 들어갈 공간이 부족하면 약어 버전으로 바꾼다.
    auto clockRow = [&](CCPoint center, float width, float height) {
        float const clockW = height - 6.f;
        if (auto* clock = loadedFrameSprite("GJ_timeIcon_001.png")) {
            clock->setScale(clockW / std::max(1.f, clock->getContentSize().height));
            clock->setPosition({center.x - width / 2.f + 4.f + clockW / 2.f, center.y});
            parent->addChild(clock);
        }
        float const room = width - clockW - 12.f, rowH = height - 6.f;
        float names = 0.f;
        for (auto const& pill : lengths)
            if (!pill.text.empty())
                names += createCosmeticLabel(pill.text.c_str(), "bigFont.fnt")->getContentSize().width;
        float const fixed = 2.f * 5.f + 7.f * 6.f + (rowH - 6.f);
        std::vector<FilterPill> const row = (room - fixed) / std::max(1.f, names) < .18f ? lengthPills(true) : lengths;
        filterPills(parent, row, {center.x + (clockW + 4.f) / 2.f, center.y}, room, rowH, .3f, 2.f);
    };
    if (stacked) {
        float const boxH = 20.f;
        CCPoint const first{cx, top - faceH - 30.f}, second{cx, top - faceH - 30.f - boxH - 4.f};
        box(first, {w, boxH});
        clockRow(first, w, boxH);
        box(second, {w, boxH});
        filterPills(parent, others, second, w - 10.f, boxH - 6.f, .3f, 6.f);
    } else {
        float const boxH = 26.f, left = (w - 6.f) * .64f, right = w - 6.f - left, y = area.getMinY() + boxH / 2.f;
        CCPoint const first{x0 + left / 2.f, y}, second{x0 + left + 6.f + right / 2.f, y};
        box(first, {left, boxH});
        clockRow(first, left, boxH);
        box(second, {right, boxH});
        filterPills(parent, others, second, right - 8.f, boxH - 8.f, .3f, 4.f);
    }
}

// 배경 9-slice가 있는 패널 노드를 만든다. decorated면 GJ_square02 + 팝업 사이드 아트,
// 아니면 어두운 반투명 파란 판으로 그린다.
CCNode* panel(CCNode* parent, CCPoint position, CCSize size, bool decorated = true) {
    auto* node = CCNode::create();
    node->setPosition(position);
    node->setContentSize(size);
    auto* bg = NineSlice::create(decorated ? "GJ_square02.png" : "square02b_001.png");
    bg->setContentSize(size);
    bg->setPosition(size / 2.f);
    if (!decorated) {
        bg->setColor(ccc3(6, 30, 80));
        bg->setOpacity(170);
    }
    node->addChild(bg, -1);
    parent->addChild(node);
    if (decorated) geode::addSideArt(node, SideArt::All, SideArtStyle::PopupBlue);
    return node;
}

// parent 위에 (0,0) 기준의 CCMenu를 만들어 붙인다.
CCMenu* menu(CCNode* parent) {
    auto* value = CCMenu::create();
    value->setPosition({0.f, 0.f});
    parent->addChild(value, 5);
    return value;
}

// 스크롤 레이어에 "탭" 판정을 더한 클래스. 터치가 kTapSlop 이내로만 움직였을 때
// 눌린 행(row)을 찾아 콜백(onTap 또는 터치 좌표를 함께 받는 onTapAt)을 호출한다.
class TapScrollLayer final : public ScrollLayer {
    std::function<void(CCNode*)> m_onTap;
    std::function<void(CCNode*, CCPoint)> m_onTapAt;
    CCPoint m_start{};
    bool m_tracking = false;
    // 탭으로 인정하는 최대 이동 거리(화면 좌표).
    static constexpr float kTapSlop = 9.f;
    explicit TapScrollLayer(CCRect const& rect) : ScrollLayer(rect, true, true) {}

public:
    static TapScrollLayer* create(CCSize const& size, std::function<void(CCNode*)> onTap) {
        auto* result = new TapScrollLayer({0.f, 0.f, size.width, size.height});
        result->m_onTap = std::move(onTap);
        result->autorelease();
        return result;
    }
    static TapScrollLayer* createAt(CCSize const& size, std::function<void(CCNode*, CCPoint)> onTap) {
        auto* result = new TapScrollLayer({0.f, 0.f, size.width, size.height});
        result->m_onTapAt = std::move(onTap);
        result->autorelease();
        return result;
    }
    bool ccTouchBegan(CCTouch* touch, CCEvent* event) override {
        bool const taken = ScrollLayer::ccTouchBegan(touch, event);
        auto const local = convertToNodeSpace(touch->getLocation());
        m_tracking = taken && CCRect(0.f, 0.f, getContentSize().width, getContentSize().height).containsPoint(local);
        m_start = touch->getLocation();
        return taken;
    }
    void ccTouchMoved(CCTouch* touch, CCEvent* event) override {
        ScrollLayer::ccTouchMoved(touch, event);
        if (m_tracking && ccpDistance(touch->getLocation(), m_start) > kTapSlop) m_tracking = false;
    }
    void ccTouchCancelled(CCTouch* touch, CCEvent* event) override {
        m_tracking = false;
        ScrollLayer::ccTouchCancelled(touch, event);
    }
    // 화면 밖에 있는 행은 숨겨서 그리는 비용을 줄인다 (content layer 오프셋 기준 가시성 계산).
    void visit() override {
        if (m_contentLayer) {
            float const offset = m_contentLayer->getPositionY(), view = getContentSize().height;
            for (auto* row : CCArrayExt<CCNode*>(m_contentLayer->getChildren())) {
                auto const box = row->boundingBox();
                row->setVisible(box.getMaxY() + offset > 0.f && box.getMinY() + offset < view);
            }
        }
        ScrollLayer::visit();
    }
    // 탭이면 눌린 행에 짧은 확대 애니메이션(tag 77)을 주고 콜백을 호출한다. tag가 음수인 행은 제외.
    void ccTouchEnded(CCTouch* touch, CCEvent* event) override {
        ScrollLayer::ccTouchEnded(touch, event);
        bool const tap = m_tracking && ccpDistance(touch->getLocation(), m_start) <= kTapSlop;
        m_tracking = false;
        if (!tap || (!m_onTap && !m_onTapAt)) return;
        auto const point = m_contentLayer->convertToNodeSpace(touch->getLocation());
        for (auto* row : CCArrayExt<CCNode*>(m_contentLayer->getChildren())) {
            if (row->getTag() < 0 || !row->isVisible() || !row->boundingBox().containsPoint(point)) continue;
            row->stopActionByTag(77);
            auto* pulse = CCSequence::create(CCScaleTo::create(.06f, 1.025f), CCScaleTo::create(.08f, 1.f), nullptr);
            pulse->setTag(77);
            row->runAction(pulse);
            if (m_onTapAt) {
                auto callback = m_onTapAt;
                callback(row, row->convertToNodeSpace(touch->getLocation()));
                return;
            }
            auto callback = m_onTap;
            callback(row);
            return;
        }
    }
};

// BMFont에 없는 한글은 기존 언어 설정에 맞춰 시스템 글꼴로 표시한다.
void localizeSkipMusic(ButtonSprite* sprite, std::string const& caption) {
    if (caption != "Skip Music" || !Mod::get()->getSavedValue<bool>("patch-notes-korean", true)) return;
    sprite->m_label->setVisible(false);
    auto* text = createSystemText("\xEC\x9D\x8C\xEC\x95\x85 \xEA\xB1\xB4\xEB\x84\x88\xEB\x9B\xB0\xEA\xB8\xB0", 20.f,
                                  0.f, kCCTextAlignmentCenter, TextWeight::Bold);
    text->setScale(
        std::min(1.f, (sprite->getContentSize().width - 14.f) / std::max(1.f, text->getContentSize().width)));
    text->setPosition(sprite->getContentSize() / 2.f);
    sprite->addChild(text, 5);
}

// goldFont 텍스트 버튼(ButtonSprite)을 만들어 parent 메뉴에 추가한다.
CCMenuItemSpriteExtra* button(CCMenu* parent, CCObject* target, SEL_MenuHandler action, std::string const& text,
                              CCPoint position, float scale = .58f, char const* background = "GJ_button_01.png") {
    auto* sprite = ButtonSprite::create(text.c_str(), "goldFont.fnt", background);
    localizeSkipMusic(sprite, text);
    sprite->setScale(scale);
    auto* item = CCMenuItemSpriteExtra::create(sprite, target, action);
    item->setPosition(position);
    parent->addChild(item);
    return item;
}

// GJ_GameSheet03 등에서 프레임을 찾아 만든 아이콘 버튼. 프레임이 없으면 info 아이콘 -> 빈 스프라이트 순으로 대체.
CCMenuItemSpriteExtra* iconButton(CCMenu* parent, CCObject* target, SEL_MenuHandler action, char const* frame,
                                  CCPoint position, float height) {
    auto* sprite = loadedFrameSprite(frame, "GJ_GameSheet03.plist");
    if (!sprite) sprite = loadedFrameSprite("GJ_infoIcon_001.png");
    if (!sprite) sprite = CCSprite::create();
    sprite->setScale(height / std::max(1.f, sprite->getContentSize().height));
    auto* item = CCMenuItemSpriteExtra::create(sprite, target, action);
    item->setPosition(position);
    parent->addChild(item);
    return item;
}

// 실제로 그려지는 폭: 메뉴 아이템이면 안의 스프라이트 boundingBox(스케일 포함)까지 본다.
float drawnWidth(CCNode* node) {
    if (!node) return 0.f;
    float width = node->getScaledContentSize().width;
    if (auto* item = typeinfo_cast<CCMenuItemSprite*>(node))
        if (auto* image = item->getNormalImage())
            width = std::max(width, image->boundingBox().size.width * item->getScale());
    return width;
}

// 화면 폭에 비례한 버튼 사이 간격(569 폭 기준 약 7).
float rowGap(float windowWidth) {
    return std::max(4.f, windowWidth * .012f);
}

// 노드들을 오른쪽 끝 right에서 왼쪽으로 실제 폭과 gap만큼 띄워 한 줄로 놓는다(nodes[0]이 가장 오른쪽).
// 보이지 않는 노드는 자리를 차지하지 않는다. 줄이 maxWidth를 넘으면 전체를 같은 비율로 줄인다.
void layoutRowRight(std::vector<CCNode*> const& nodes, float right, float y, float gap, float maxWidth) {
    float total = 0.f;
    int shown = 0;
    for (auto* node : nodes)
        if (node && node->isVisible()) {
            total += drawnWidth(node);
            ++shown;
        }
    if (shown == 0) return;
    total += gap * (shown - 1);
    float const fit = maxWidth > 0.f && total > maxWidth ? maxWidth / total : 1.f;
    float x = right;
    for (auto* node : nodes) {
        if (!node || !node->isVisible()) continue;
        if (fit < 1.f) node->setScale(node->getScale() * fit);
        float const width = drawnWidth(node);
        node->setPosition({x - width / 2.f, y});
        x -= width + gap * fit;
    }
}

// 노드 트리 전체에 cascade opacity를 켠다.
void cascadeOpacity(CCNode* node) {
    if (auto* rgba = typeinfo_cast<CCRGBAProtocol*>(node)) rgba->setCascadeOpacityEnabled(true);
    for (auto* child : CCArrayExt<CCNode*>(node->getChildren())) cascadeOpacity(child);
}

// 노드 트리에서 RGBA 노드를 찾아 opacity를 적용한다 (미리보기용 반투명 표시).
void previewOpacity(CCNode* node, GLubyte opacity) {
    if (auto* rgba = typeinfo_cast<CCRGBAProtocol*>(node)) {
        cascadeOpacity(node);
        rgba->setOpacity(opacity);
        return;
    }
    for (auto* child : CCArrayExt<CCNode*>(node->getChildren())) previewOpacity(child, opacity);
}

// 버튼 활성/비활성 전환. 비활성이면 opacity를 95로 낮춰 흐리게 보이게 한다.
void enabled(CCMenuItemSpriteExtra* item, bool value) {
    item->setEnabled(value);
    cascadeOpacity(item);
    item->setOpacity(value ? 255 : 95);
}

// TextInput 배경을 UI 공통 파란색으로 맞춘다.
void styleInput(TextInput* input) {
    input->getBGSprite()->setColor(ccc3(7, 31, 86));
    input->getBGSprite()->setOpacity(230);
}

// 숫자(Uint)만 받는 작은 TextInput. 입력 문자열을 정수로 파싱해 typed 콜백에 넘기며,
// 비었거나 숫자가 아니면 -1을 넘긴다.
TextInput* numberInput(CCNode* parent, CCPoint at, int value, size_t digits, float width,
                       std::function<void(int)> typed) {
    auto* input = TextInput::create(width / .6f, "#");
    input->setCommonFilter(CommonFilter::Uint);
    input->setMaxCharCount(digits);
    input->setScale(.6f);
    input->setPosition(at);
    input->setString(std::to_string(value).c_str());
    styleInput(input);
    input->setCallback([typed = std::move(typed)](std::string const& text) {
        int parsed = -1;
        if (!text.empty()) {
            auto const [end, problem] = std::from_chars(text.data(), text.data() + text.size(), parsed);
            if (problem != std::errc() || end != text.data() + text.size()) parsed = -1;
        }
        typed(parsed);
    });
    parent->addChild(input, 5);
    return input;
}
// numberInput과 같은 규칙으로 현재 입력값을 정수로 읽는다 (잘못된 값이면 -1).
int typedNumber(TextInput* input) {
    std::string const text = input->getString();
    int value = -1;
    if (!text.empty()) {
        auto const [end, problem] = std::from_chars(text.data(), text.data() + text.size(), value);
        if (problem != std::errc() || end != text.data() + text.size()) value = -1;
    }
    return value;
}

// 아바타 캐시 키. 닉네임/아이콘/색/멤버십/치장 항목 중 하나라도 바뀌면 키가 달라져 다시 만들게 된다.
std::string avatarKey(PlayerProfile const& profile) {
    auto const& c = profile.cosmetics;
    return fmt::format("{}|{}|{}|{}|{}|{}|{}|{}|{}|{}|{}|{}", profile.name, profile.icon, profile.color1,
                       profile.color2, profile.membership, c.start, c.end, c.angle, c.aura, c.execution, c.title,
                       c.font);
}
// 유저별 아바타 노드를 재사용하기 위한 캐시. put()으로 이번 갱신에서 쓴 노드를 모아 두고
// keep()에서 교체하며, detach()는 부모에서 떼어 내 다음 화면에 다시 붙일 수 있게 한다.
class AvatarCache {
    std::unordered_map<std::string, std::pair<std::string, Ref<CCNode>>> m_kept, m_next;

public:
    void detach() {
        for (auto& [uid, entry] : m_kept)
            if (entry.second && entry.second->getParent()) entry.second->removeFromParentAndCleanup(false);
    }
    CCNode* take(std::string const& uid, std::string const& key) {
        auto found = m_kept.find(uid);
        if (found == m_kept.end() || found->second.first != key) return nullptr;
        return found->second.second;
    }
    void put(std::string const& uid, std::string key, CCNode* node) {
        m_next[uid] = {std::move(key), Ref<CCNode>(node)};
    }
    void keep() {
        m_kept = std::move(m_next);
        m_next.clear();
    }
};

// 프로필의 아이콘/색/멤버십 오라를 적용한 SimplePlayer를 만든다. 아이콘 번호는 보유 개수로 제한.
SimplePlayer* player(CCNode* parent, PlayerProfile const& profile, CCPoint point, float scale) {
    auto* manager = GameManager::sharedState();
    int const count = manager ? std::max(1, manager->countForType(IconType::Cube)) : 1;
    auto* icon = SimplePlayer::create(std::clamp(profile.icon, 1, count));
    icon->setColors(unpackColor(profile.color1), unpackColor(profile.color2));
    icon->disableGlowOutline();
    addMembershipAura(icon, profile.membership, profile.cosmetics);
    icon->setPosition(point);
    icon->setScale(scale);
    parent->addChild(icon);
    return icon;
}

// 승률 문자열. 최근 경기 기록이 없으면 "--".
std::string rate(PlayerProfile const& profile) {
    if (!profile.recentGames) return "--";
    return fmt::format("{:.0f}%", profile.winRate);
}

// 오류 알림. 점검 공지 처리(showMaintenanceNotice)가 먼저 가로채지 않으면 FLAlertLayer로 띄운다.
void error(std::string const& message) {
    if (showMaintenanceNotice(message)) return;
    FLAlertLayer::create("Versus", message.empty() ? "Please try again." : message.c_str(), "OK")->show();
}

// 방 PIN 형식 검사 (RoomPin 모듈에 위임).
bool validPin(std::string const& pin) {
    return validRoomPin(pin);
}

// ASCII 소문자 변환.
std::string lower(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return value;
}

// 속도 퍼센트(100=1x)를 "1x", "1.5x", "1.25x" 형태로 표시한다.
std::string speedText(int percent) {
    if (percent % 100 == 0) return fmt::format("{}x", percent / 100);
    if (percent % 10 == 0) return fmt::format("{:.1f}x", percent / 100.0);
    return fmt::format("{:.2f}x", percent / 100.0);
}
// 방 규칙을 한 줄 요약 문자열로 만든다 (모드, 연습, 핵 허용, 속도).
std::string rulesText(GameRules const& rules) {
    std::string result = rules.mode == 0
                             ? (rules.practice ? "Fewest Attempts" : fmt::format("{} Attempts", rules.attempts))
                             : fmt::format("First to {}%", rules.targetPercent);
    if (rules.practice) result += " / Practice";
    if (rules.allowHacks) result += " / Hacks OK";
    if (rules.speed != 100) result += " / Speed " + speedText(rules.speed);
    return result;
}

// 방 참가자의 READY 표시. 준비되면 체크 표시, 아직이면 색이 순환하고 맥동하는 애니메이션을 준다.
void readyBadge(CCNode* parent, CCPoint position, bool ready) {
    auto* badge = CCNode::create();
    badge->setPosition(position);
    parent->addChild(badge);
    auto* text = label(badge, ready ? "READY" : "NOT READY", {ready ? 8.f : 0.f, 0.f}, .27f, 100.f,
                       ready ? ccc3(150, 255, 175) : kIce);
    if (ready) {
        auto* check = CCDrawNode::create();
        check->drawSegment({-32.f, -1.f}, {-28.f, -5.f}, 1.7f, {0.55f, 1.f, .68f, 1.f});
        check->drawSegment({-28.f, -5.f}, {-20.f, 5.f}, 1.7f, {0.55f, 1.f, .68f, 1.f});
        badge->addChild(check);
    } else {
        text->runAction(CCRepeatForever::create(
            CCSequence::create(CCTintTo::create(.55f, 255, 145, 165), CCTintTo::create(.55f, 255, 225, 135),
                               CCTintTo::create(.55f, 135, 255, 180), CCTintTo::create(.55f, 130, 210, 255),
                               CCTintTo::create(.55f, 205, 155, 255), nullptr)));
        badge->runAction(
            CCRepeatForever::create(CCSequence::create(CCEaseSineInOut::create(CCScaleTo::create(.8f, 1.06f)),
                                                       CCEaseSineInOut::create(CCScaleTo::create(.8f, 1.f)), nullptr)));
    }
}

// 이모트 벡터 아트와 이펙트 구현(인라인 include).
#include "EmoteArt.inl"

#include "EmoteEffects.inl"
// 이모트 이펙트를 약간(.16초) 뒤에 재생한다. 도중에 parent가 사라져도 안전하도록 Ref로 잡는다.
void emoteBurst(CCNode* parent, CCPoint at, std::string const& kind, float scale = 1.f) {
    if (!parent) return;
    Ref<CCNode> keep(parent);
    parent->runAction(CCSequence::create(
        CCDelayTime::create(.16f),
        CallFuncExt::create([keep, at, kind, scale] { emoteEffect(keep.data(), at, kind, scale); }), nullptr));
}

// 이모트 아이콘 노드. 등록된 이미지가 있으면 그것을, 멤버 전용이면 memberEmoteArt를,
// 그 외 기본 이모트는 CCDrawNode로 직접 그린 벡터 도형을 쓴다.
CCNode* emoteArt(std::string const& kind) {
    if (emotes::find(kind))
        if (auto* sprite = art::artSprite("emote-" + kind + ".png", 34.f)) {
            auto* holder = art::artHolder(34.f);
            sprite->setPosition({17.f, 17.f});
            holder->addChild(sprite);
            return holder;
        }
    if (auto const* emote = emotes::find(kind); emote && emote->member && kind != "money") return memberEmoteArt(kind);
    auto* art = CCDrawNode::create();
    ccColor4F const ink = {.12f, .18f, .27f, 1.f};
    if (kind == "money") {
        CCPoint note[] = {{-15, -10}, {15, -10}, {15, 10}, {-15, 10}};
        art->drawPolygon(note, 4, {.2f, .78f, .38f, 1.f}, 1.3f, ink);
        CCPoint border[] = {{-11, -6}, {11, -6}, {11, 6}, {-11, 6}};
        art->drawPolygon(border, 4, {.5f, 1.f, .66f, 1.f}, .7f, ink);
        label(art, "$", {0, 0}, .63f, 15.f, ccWHITE, false, "chatFont.fnt");
    } else if (kind == "fire") {
        CCPoint flame[] = {{0, 14}, {4, 5}, {8, 9}, {12, -2}, {8, -11}, {0, -14}, {-9, -10}, {-12, -1}, {-4, 8}};
        art->drawPolygon(flame, 9, {1.f, .39f, .16f, 1.f}, 1.f, ink);
        CCPoint center[] = {{0, 4}, {5, -4}, {2, -10}, {-4, -9}, {-5, -4}};
        art->drawPolygon(center, 5, {1.f, .88f, .27f, 1.f}, 0.f, ink);
    } else if (kind == "like") {
        CCPoint thumb[] = {{-4, -10}, {8, -10}, {12, 2}, {7, 6}, {1, 6}, {2, 14}, {-2, 15}, {-6, 4}};
        art->drawPolygon(thumb, 8, {1.f, .83f, .42f, 1.f}, 1.f, ink);
        CCPoint cuff[] = {{-13, -11}, {-6, -11}, {-6, 4}, {-13, 4}};
        art->drawPolygon(cuff, 4, {.25f, .58f, 1.f, 1.f}, 1.f, ink);
    } else if (kind == "dislike") {
        CCPoint thumb[] = {{4, 10}, {-8, 10}, {-12, -2}, {-7, -6}, {-1, -6}, {-2, -14}, {2, -15}, {6, -4}};
        art->drawPolygon(thumb, 8, {1.f, .83f, .42f, 1.f}, 1.f, ink);
        CCPoint cuff[] = {{13, 11}, {6, 11}, {6, -4}, {13, -4}};
        art->drawPolygon(cuff, 4, {.25f, .58f, 1.f, 1.f}, 1.f, ink);
    } else {
        bool const angry = kind == "angry";
        bool const sad = kind == "sad";
        art->drawDot({0, 0}, 14.f, ink);
        art->drawDot({0, 0}, 12.8f,
                     angry ? ccColor4F{1.f, .44f, .32f, 1.f}
                     : sad ? ccColor4F{.38f, .72f, 1.f, 1.f}
                           : ccColor4F{1.f, .84f, .29f, 1.f});
        art->drawDot({-4.5f, 3.f}, 1.6f, ink);
        art->drawDot({4.5f, 3.f}, 1.6f, ink);
        if (angry) {
            art->drawSegment({-9, 9}, {-3, 6}, 1.1f, ink);
            art->drawSegment({3, 6}, {9, 9}, 1.1f, ink);
            art->drawSegment({-5, -6}, {0, -4}, 1.2f, ink);
            art->drawSegment({0, -4}, {5, -6}, 1.2f, ink);
        } else if (sad) {
            art->drawSegment({-6, -7}, {-3, -4}, 1.2f, ink);
            art->drawSegment({-3, -4}, {3, -4}, 1.2f, ink);
            art->drawSegment({3, -4}, {6, -7}, 1.2f, ink);
            art->drawDot({-7.f, -1.f}, 1.7f, {.35f, .9f, 1.f, 1.f});
            art->drawDot({7.f, -1.f}, 1.7f, {.35f, .9f, 1.f, 1.f});
        } else {
            art->drawSegment({-6, -3}, {-3, -6}, 1.2f, ink);
            art->drawSegment({-3, -6}, {3, -6}, 1.2f, ink);
            art->drawSegment({3, -6}, {6, -3}, 1.2f, ink);
        }
    }
    return art;
}

// 배경 위로 타일이 천천히 흘러가는 움직이는 파란 배경. 타일 폭만큼 이동하면 위치를 되감아 반복한다.
class MovingBackground : public CCNode {
    CCNode* m_tiles = nullptr;
    CCSprite* m_bg = nullptr;
    float m_tileWidth = 0.f;

public:
    void tint(ccColor3B base, ccColor3B tiles, GLubyte tileOpacity) {
        if (m_bg) m_bg->setColor(base);
        if (m_tiles)
            for (auto* tile : CCArrayExt<CCSprite*>(m_tiles->getChildren())) {
                tile->setColor(tiles);
                tile->setOpacity(tileOpacity);
            }
    }
    bool init() override {
        if (!CCNode::init()) return false;
        auto const window = CCDirector::sharedDirector()->getWinSize();
        auto* bg = geode::createLayerBG();
        bg->setColor(ccc3(0, 56, 145));
        m_bg = bg;
        addChild(bg);
        auto* first = CCSprite::create("game_bg_02_001.png");
        if (!first || first->getContentSize().height <= 0.f) return true;
        m_tiles = CCNode::create();
        addChild(m_tiles);
        float const scale = window.height / first->getContentSize().height;
        m_tileWidth = first->getContentSize().width * scale;
        if (m_tileWidth <= 0.f) return true;
        int const count = static_cast<int>(std::ceil(window.width / m_tileWidth)) + 2;
        for (int i = 0; i < count; ++i) {
            auto* tile = i ? CCSprite::create("game_bg_02_001.png") : first;
            if (!tile) continue;
            tile->setScale(scale);
            tile->setColor(ccc3(65, 125, 255));
            tile->setOpacity(85);
            tile->setPosition({(i + .5f) * m_tileWidth, window.height / 2.f});
            m_tiles->addChild(tile);
        }
        scheduleUpdate();
        return true;
    }
    void update(float dt) override {
        if (!m_tiles || m_tileWidth <= 0.f) return;
        float x = m_tiles->getPositionX() - 18.f * std::clamp(dt, 0.f, .1f);
        if (x <= -m_tileWidth) x += m_tileWidth;
        m_tiles->setPositionX(x);
    }
    CREATE_FUNC(MovingBackground);
};

// 곡 ID에서 "제목 - 아티스트" 문자열을 얻는다. 0 이하는 기본 곡, 정보가 없으면 "Song N".
std::string songTitle(int songID) {
    if (songID <= 0) return "Versus (default)";
    if (auto* music = MusicDownloadManager::sharedState())
        if (auto* info = music->getSongInfoObject(songID); info && !std::string(info->m_songName).empty())
            return fmt::format("{} - {}", std::string(info->m_songName), std::string(info->m_artistName));
    return fmt::format("Song {}", songID);
}

// 로비 배경곡을 고르는 팝업. 다운로드된 곡 + 알려진 곡을 모아 검색/정렬해서 보여 준다.
class LobbySongPopup final : public ::versus::DeferredPopup {
    static constexpr float kWidth = 380.f, kHeight = 290.f, kRow = 30.f;
    std::function<void()> m_changed;
    TapScrollLayer* m_scroll = nullptr;
    TextInput* m_search = nullptr;
    CCLabelBMFont* m_status = nullptr;
    void onExit() override {
        if (m_search) m_search->defocus();
        Popup::onExit();
    }
    struct Song {
        int id;
        std::string title;
        bool downloaded;
    };
    std::vector<Song> m_all;
    std::vector<Song> m_shown;
    std::string m_query;
    int m_downloading = 0;
    float m_waited = 0.f;

    static std::string folded(std::string text) {
        std::transform(text.begin(), text.end(), text.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        return text;
    }
    // MusicDownloadManager의 다운로드 목록과 곡 정보 딕셔너리에서 곡을 중복 없이 모은다.
    // 다운로드된 곡이 먼저, 그다음 제목순이며 맨 앞은 기본 곡(0).
    void collect() {
        m_all.clear();
        auto* music = MusicDownloadManager::sharedState();
        std::vector<Song> found;
        std::set<int> seen;
        auto take = [&](SongInfoObject* info) {
            if (!info || info->m_songID <= 0 || !seen.insert(info->m_songID).second) return;
            found.push_back({info->m_songID, songTitle(info->m_songID), music->isSongDownloaded(info->m_songID)});
        };
        if (music) {
            if (auto* songs = music->getDownloadedSongs())
                for (auto* object : CCArrayExt<CCObject*>(songs)) take(typeinfo_cast<SongInfoObject*>(object));
            if (auto* known = music->m_songObjects) {
                CCDictElement* element = nullptr;
                CCDictElement* next = nullptr;
                HASH_ITER(hh, known->m_pElements, element, next)
                take(typeinfo_cast<SongInfoObject*>(element->getObject()));
            }
        }
        std::sort(found.begin(), found.end(), [](Song const& a, Song const& b) {
            return a.downloaded != b.downloaded ? a.downloaded : a.title < b.title;
        });
        m_all.push_back({0, songTitle(0), true});
        for (auto& song : found) m_all.push_back(std::move(song));
    }
    // 검색어(m_query)로 곡 목록을 걸러 m_shown을 만든다. 숫자만 있으면 곡 ID 접두사 일치,
    // 아니면 제목 부분 문자열 일치. 숫자 검색에서 목록에 없는 ID는 "다운로드" 항목으로 맨 앞에 추가하고
    // 결과는 최대 120개로 자른다.
    void filter() {
        m_shown.clear();
        bool const numeric = !m_query.empty() && std::all_of(m_query.begin(), m_query.end(),
                                                             [](unsigned char c) { return std::isdigit(c); });
        for (auto const& song : m_all) {
            if (m_query.empty()) {
                m_shown.push_back(song);
                continue;
            }
            if (song.id == 0) continue;
            bool const hit = numeric ? std::to_string(song.id).starts_with(m_query)
                                     : folded(song.title).find(m_query) != std::string::npos;
            if (hit) m_shown.push_back(song);
        }
        if (numeric && m_query.size() <= 9) {
            int id = 0;
            std::from_chars(m_query.data(), m_query.data() + m_query.size(), id);
            if (id > 0 && std::none_of(m_shown.begin(), m_shown.end(), [id](Song const& s) { return s.id == id; }))
                m_shown.insert(m_shown.begin(), {id, fmt::format("Song {}  (tap to download)", id), false});
        }
        if (m_shown.size() > 120) m_shown.resize(120);
        fill();
    }
    // m_shown을 스크롤 목록으로 다시 그린다. 현재 선택된 곡은 초록색, 행 tag는 m_shown 인덱스.
    void fill() {
        if (m_scroll) m_scroll->removeFromParent();
        CCSize const view{kWidth - 30.f, 168.f};
        m_scroll = TapScrollLayer::create(view, [self = Weak<LobbySongPopup>(this)](CCNode* row) {
            if (auto owner = self.lock()) owner->choose(row->getTag());
        });
        m_scroll->setPosition({15.f, 50.f});
        m_mainLayer->addChild(m_scroll, 2);
        int const current = audio::rankedMusicSong();
        float const height = std::max(view.height, m_shown.size() * (kRow + 3.f));
        m_scroll->m_contentLayer->setContentSize({view.width, height});
        float y = height;
        for (size_t i = 0; i < m_shown.size(); ++i) {
            auto const& song = m_shown[i];
            y -= kRow + 3.f;
            auto* row = CCNode::create();
            row->setContentSize({view.width, kRow});
            row->setPosition({0.f, y});
            row->setTag(static_cast<int>(i));
            bool const selected = song.id == current;
            auto* bg = NineSlice::create("square02b_001.png");
            bg->setContentSize({view.width, kRow});
            bg->setPosition({view.width / 2.f, kRow / 2.f});
            bg->setColor(selected ? ccc3(40, 110, 70) : ccc3(10, 32, 70));
            bg->setOpacity(220);
            row->addChild(bg);
            label(row, song.title, {10.f, kRow / 2.f}, .34f, view.width - 110.f,
                  selected ? ccc3(170, 255, 200) : ccWHITE, true, "bigFont.fnt");
            auto tag =
                song.id <= 0 ? std::string("BUILT-IN") : fmt::format("{}  {}", song.downloaded ? "" : "DL", song.id);
            label(row, tag, {view.width - 8.f, kRow / 2.f}, .26f, 96.f, song.downloaded ? kMuted : ccc3(255, 190, 110),
                  false, "bigFont.fnt")
                ->setAnchorPoint({1.f, .5f});
            m_scroll->m_contentLayer->addChild(row);
        }
        m_scroll->scrollToTop();
    }
    void status(std::string const& text) {
        m_status->setString(text.c_str());
        m_status->limitLabelWidth(kWidth - 30.f, .3f, .18f);
    }
    // 행을 눌렀을 때: 아직 받지 않은 곡이면 다운로드를 시작하고, 받은 곡이면 바로 적용한다.
    void choose(int index) {
        if (index < 0 || index >= static_cast<int>(m_shown.size())) return;
        int const id = m_shown[index].id;
        auto* music = MusicDownloadManager::sharedState();
        if (id > 0 && music && !music->isSongDownloaded(id)) {
            startDownload(id);
            return;
        }
        apply(id);
    }
    // 선택한 곡 ID를 로비 음악 설정으로 저장하고 목록/상태 표시를 갱신한다.
    void apply(int id) {
        audio::setRankedMusicSong(id);
        if (auto* mod = Mod::get()) (void)mod->saveData();
        status(fmt::format("Lobby music: {}", songTitle(id)));
        collect();
        filter();
        if (m_changed) m_changed();
    }
    // 곡 정보를 요청하고 다운로드를 시작한다. 한 번에 하나만 받는다(m_downloading).
    void startDownload(int id) {
        auto* music = MusicDownloadManager::sharedState();
        if (!music || m_downloading) return;
        m_downloading = id;
        m_waited = 0.f;
        music->getSongInfo(id, true);
        music->downloadSong(id);
        status(fmt::format("Downloading song {}...", id));
    }
    // 다운로드 대기 중 매 프레임 확인: 완료되면 적용하고, 45초가 지나도 안 받아지면 실패로 표시한다.
    void update(float dt) override {
        if (m_downloading <= 0) return;
        m_waited += dt;
        auto* music = MusicDownloadManager::sharedState();
        if (music && music->isSongDownloaded(m_downloading)) {
            apply(std::exchange(m_downloading, 0));
            return;
        }
        if (m_waited >= 45.f) {
            status(fmt::format("Song {} could not be downloaded.", m_downloading));
            m_downloading = 0;
        }
    }
    // 검색 입력 콜백은 공백을 다듬고 소문자로 바꾼 뒤 바뀐 경우에만 filter()를 다시 호출한다.
    bool init(std::function<void()> changed) {
        if (!Popup::init(kWidth, kHeight, "GJ_square02.png")) return false;
        m_changed = std::move(changed);
        setTitle("Lobby Music", "goldFont.fnt", .7f);
        m_search = TextInput::create(kWidth - 40.f, "Search by song name or song ID");
        m_search->setCommonFilter(CommonFilter::Any);
        m_search->setMaxCharCount(40);
        m_search->setScale(.75f);
        m_search->setWidth((kWidth - 40.f) / .75f);
        m_search->setPosition({kWidth / 2.f, kHeight - 50.f});
        styleInput(m_search);
        m_search->setCallback([self = Weak<LobbySongPopup>(this)](std::string const& text) {
            auto owner = self.lock();
            if (!owner) return;
            auto query = folded(std::string(text));
            query.erase(0, query.find_first_not_of(' '));
            while (!query.empty() && query.back() == ' ') query.pop_back();
            if (query == owner->m_query) return;
            owner->m_query = std::move(query);
            owner->filter();
        });
        m_mainLayer->addChild(m_search, 3);
        m_status = label(m_mainLayer, "", {kWidth / 2.f, 30.f}, .3f, kWidth - 30.f, ccc3(255, 214, 120), false,
                         "chatFont.fnt");
        label(m_mainLayer, "Downloaded songs first  -  DL = tap to download", {kWidth / 2.f, 14.f}, .28f, kWidth - 30.f,
              kMuted, false, "chatFont.fnt");
        status(fmt::format("Lobby music: {}", songTitle(audio::rankedMusicSong())));
        collect();
        filter();
        scheduleUpdate();
        return true;
    }

public:
    static LobbySongPopup* create(std::function<void()> changed) {
        auto* result = new LobbySongPopup;
        if (result->init(std::move(changed))) {
            result->autorelease();
            return result;
        }
        delete result;
        return nullptr;
    }
};

// 아래에서 쓰는 함수의 전방 선언 (정의는 파일 뒤쪽).
void showSettings(bool ranked);

void showLiveMatches();
std::string localDateTime(int64_t ms);

// 시스템 폰트 이름 반환 (SystemText 모듈 위임).
char const* systemFont() {
    return systemFontName();
}

// 서버 방송(BroadcastInfo)을 화면 위쪽에서 내려오는 배너로 7초간 보여 준다.
// 씬이 없거나 씬 전환 중이면 false를 반환한다.
bool showBroadcastBanner(BroadcastInfo const& info) {
    auto* director = CCDirector::sharedDirector();
    auto* scene = director->getRunningScene();
    if (!scene || typeinfo_cast<CCTransitionScene*>(scene)) return false;
    auto const win = director->getWinSize();
    float const width = std::min(470.f, win.width - 40.f);
    auto* text = createSystemText(info.message, 15.f, width - 40.f, kCCTextAlignmentCenter);
    text->setColor(ccWHITE);
    float const height = std::max(62.f, text->getContentSize().height + 42.f);
    auto* banner = CCNodeRGBA::create();
    banner->setCascadeOpacityEnabled(true);
    banner->setContentSize({width, height});
    banner->setAnchorPoint({.5f, 1.f});
    banner->setID("versus-broadcast-banner"_spr);
    auto* glow = NineSlice::create("square02b_001.png");
    glow->setContentSize({width + 6.f, height + 6.f});
    glow->setColor(ccc3(255, 196, 70));
    glow->setOpacity(235);
    glow->setPosition({width / 2.f, height / 2.f});
    banner->addChild(glow, 0);
    auto* bg = NineSlice::create("square02b_001.png");
    bg->setContentSize({width, height});
    bg->setColor(ccc3(14, 20, 52));
    bg->setOpacity(245);
    bg->setPosition({width / 2.f, height / 2.f});
    banner->addChild(bg, 1);
    auto* title = CCLabelBMFont::create("BROADCAST", "goldFont.fnt");
    title->setScale(.55f);
    title->setPosition({width / 2.f, height - 14.f});
    banner->addChild(title, 2);
    text->setPosition({width / 2.f, (height - 24.f) / 2.f + 2.f});
    banner->addChild(text, 2);
    banner->setPosition({win.width / 2.f, win.height + height + 4.f});
    scene->addChild(banner, 10'000);
    banner->runAction(CCSequence::create(
        CCEaseBackOut::create(CCMoveTo::create(.35f, {win.width / 2.f, win.height - 6.f})), CCDelayTime::create(7.f),
        CCSpawn::create(CCFadeOut::create(.4f), CCMoveBy::create(.4f, {0.f, height}), nullptr), CCRemoveSelf::create(),
        nullptr));
    return true;
}

// 서버 시간(ms)을 로컬 시각 "HH:MM"으로 바꾼다. 서버와 로컬 시계의 차이(serverNow - system)를 보정.
std::string localClock(int64_t serverMs) {
    auto const system =
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch())
            .count();
    std::time_t const seconds = static_cast<std::time_t>((serverMs - (Service::get().serverNow() - system)) / 1000);
    std::tm local{};
#ifdef GEODE_IS_WINDOWS
    if (localtime_s(&local, &seconds) != 0) return {};
#else
    if (!localtime_r(&seconds, &local)) return {};
#endif
    char text[16] = {};
    std::strftime(text, sizeof text, "%H:%M", &local);
    return text;
}
// 점검 배너/팝업의 제목 문구. 점검 시작 시각이 없으면 서버 재시작 중으로 본다.
std::string maintenanceHeadline(MaintenanceInfo const& info) {
    if (info.startedAt <= 0) return "The Versus server is restarting.";
    return info.version.empty() ? std::string("Versus is being updated.")
                                : fmt::format("Versus is being updated to {}.", info.version);
}
// 점검 남은 시간 문구. 종료 예정 시각이 미래이면 로컬 시각을 덧붙인다.
std::string maintenanceEta(MaintenanceInfo const& info) {
    auto const now = Service::get().serverNow();
    auto text = maintenanceTimeLeft(info, now);
    if (info.expectedEndAt > now && !text.empty() && text.back() == '.') {
        auto const clock = localClock(info.expectedEndAt);
        if (!clock.empty()) {
            text.pop_back();
            text += fmt::format(" (around {}).", clock);
        }
    }
    return text;
}

// 서버 점검 배너를 8초간 표시한다. 이미 떠 있는 점검 배너는 지우고 새로 띄운다.
bool showMaintenanceBanner(MaintenanceInfo const& info) {
    auto* director = CCDirector::sharedDirector();
    auto* scene = director->getRunningScene();
    if (!scene || typeinfo_cast<CCTransitionScene*>(scene)) return false;
    if (auto* old = scene->getChildByID("versus-maintenance-banner"_spr)) old->removeFromParent();
    auto const win = director->getWinSize();
    float const width = std::min(470.f, win.width - 40.f), height = 70.f;
    auto* banner = CCNodeRGBA::create();
    banner->setCascadeOpacityEnabled(true);
    banner->setContentSize({width, height});
    banner->setAnchorPoint({.5f, 1.f});
    banner->setID("versus-maintenance-banner"_spr);
    auto* glow = NineSlice::create("square02b_001.png");
    glow->setContentSize({width + 6.f, height + 6.f});
    glow->setColor(ccc3(90, 200, 255));
    glow->setOpacity(235);
    glow->setPosition({width / 2.f, height / 2.f});
    banner->addChild(glow, 0);
    auto* bg = NineSlice::create("square02b_001.png");
    bg->setContentSize({width, height});
    bg->setColor(ccc3(14, 20, 52));
    bg->setOpacity(245);
    bg->setPosition({width / 2.f, height / 2.f});
    banner->addChild(bg, 1);
    auto* title = CCLabelBMFont::create("SERVER MAINTENANCE", "goldFont.fnt");
    title->setScale(.55f);
    title->setPosition({width / 2.f, height - 14.f});
    banner->addChild(title, 2);
    auto* headline = CCLabelBMFont::create(maintenanceHeadline(info).c_str(), "chatFont.fnt");
    headline->limitLabelWidth(width - 30.f, .7f, .3f);
    headline->setPosition({width / 2.f, height - 34.f});
    banner->addChild(headline, 2);
    auto* eta = CCLabelBMFont::create(maintenanceEta(info).c_str(), "chatFont.fnt");
    eta->limitLabelWidth(width - 30.f, .7f, .3f);
    eta->setColor(kIce);
    eta->setPosition({width / 2.f, height - 53.f});
    banner->addChild(eta, 2);
    banner->setPosition({win.width / 2.f, win.height + height + 4.f});
    scene->addChild(banner, 10'000);
    banner->runAction(CCSequence::create(
        CCEaseBackOut::create(CCMoveTo::create(.35f, {win.width / 2.f, win.height - 6.f})), CCDelayTime::create(8.f),
        CCSpawn::create(CCFadeOut::create(.4f), CCMoveBy::create(.4f, {0.f, height}), nullptr), CCRemoveSelf::create(),
        nullptr));
    return true;
}

// 서버 점검 안내 팝업. 0.5초마다 상태를 갱신하고, 점검이 끝나면 "다시 플레이 가능" 문구로 바꾸고 갱신을 멈춘다.
// s_open으로 중복 열기를 막는다.
class MaintenancePopup final : public ::versus::DeferredPopup {
    CCLabelBMFont* m_headline = nullptr;
    CCLabelBMFont* m_eta = nullptr;
    CCLabelBMFont* m_note = nullptr;
    static inline Weak<MaintenancePopup> s_open;
    bool init() {
        if (!Popup::init(340.f, 190.f, "GJ_square02.png")) return false;
        setTitle("Server Maintenance", "goldFont.fnt", .75f);
        setID("versus-maintenance-popup"_spr);
        m_headline = label(m_mainLayer, "", {170.f, 132.f}, .7f, 300.f, ccWHITE, false, "chatFont.fnt");
        m_eta = label(m_mainLayer, "", {170.f, 104.f}, .45f, 300.f, kIce);
        m_note = label(m_mainLayer, "Matches can't start until it is done. Your records and rating are safe.",
                       {170.f, 76.f}, .55f, 300.f, kMuted, false, "chatFont.fnt");
        button(m_buttonMenu, this, menu_selector(MaintenancePopup::onOK), "OK", {170.f, 32.f}, .7f);
        refresh(0.f);
        schedule(schedule_selector(MaintenancePopup::refresh), .5f);
        return true;
    }
    void setText(CCLabelBMFont* target, std::string const& text, float scale) {
        if (text == target->getString()) return;
        target->setString(text.c_str());
        target->limitLabelWidth(300.f, scale, .2f);
    }
    void refresh(float) {
        auto const& info = Service::get().maintenance();
        if (!info) {
            setText(m_headline, "The maintenance is over.", .7f);
            setText(m_eta, "Versus is back online!", .45f);
            m_eta->setColor(ccc3(120, 255, 140));
            setText(m_note, "You can play again.", .55f);
            unschedule(schedule_selector(MaintenancePopup::refresh));
            return;
        }
        setText(m_headline, maintenanceHeadline(*info), .7f);
        setText(m_eta, maintenanceEta(*info), .45f);
    }
    void onOK(CCObject*) { onClose(nullptr); }

public:
    static MaintenancePopup* create() {
        auto* result = new MaintenancePopup;
        if (result->init()) {
            result->autorelease();
            return result;
        }
        delete result;
        return nullptr;
    }
    static void open() {
        if (s_open.lock() || !Service::get().maintenance()) return;
        if (auto* popup = create()) {
            popup->show();
            s_open = popup;
        }
    }
};

// 방송 목록(인박스) 팝업. 열 때 읽음 처리하고 서버에서 목록을 다시 받아 그린다.
class BroadcastInboxPopup final : public ::versus::DeferredPopup {
    ScrollLayer* m_scroll = nullptr;
    CCLabelBMFont* m_status = nullptr;
    int64_t m_seen = 0;
    bool init() {
        if (!Popup::init(420.f, 280.f, "GJ_square02.png")) return false;
        setTitle("Broadcasts", "goldFont.fnt", .7f);
        setID("versus-broadcast-inbox-popup"_spr);
        // 마지막으로 본 방송 ID를 읽어 와, 그보다 새로운 항목을 NEW로 강조한다.
        m_seen = Mod::get()->getSavedValue<int64_t>("broadcast-seen", 0);
        m_scroll = ScrollLayer::create({392.f, 214.f});
        m_scroll->setPosition({14.f, 18.f});
        m_mainLayer->addChild(m_scroll);
        m_status = label(m_mainLayer, "Loading...", {210.f, 126.f}, .5f, 360.f, kIce, false, "chatFont.fnt");
        render();
        Service::get().markBroadcastsRead();
        Service::get().refreshBroadcasts([self = Weak<BroadcastInboxPopup>(this)](bool ok, std::string detail) {
            auto owner = self.lock();
            if (!owner) return;
            owner->render(ok ? std::string() : detail);
            Service::get().markBroadcastsRead();
        });
        return true;
    }
    // 방송 목록을 스크롤 행으로 그린다. 목록이 비어 있으면 로딩/오류/빈 상태 문구를 보여 준다.
    void render(std::string const& problem = "loading") {
        auto* content = m_scroll->m_contentLayer;
        content->removeAllChildrenWithCleanup(true);
        auto const& list = Service::get().broadcasts();
        m_status->setVisible(list.empty());
        m_status->setString(!list.empty()          ? ""
                            : problem == "loading" ? "Loading..."
                            : problem.empty()      ? "No broadcasts yet"
                                                   : problem.c_str());
        m_status->limitLabelWidth(360.f, .5f, .2f);
        float const width = 392.f;
        std::vector<std::pair<CCNode*, float>> rows;
        float offset = 4.f;
        for (auto const& info : list) {
            auto* text = createSystemText(info.message, 13.5f, width - 30.f, kCCTextAlignmentLeft);
            float const rowHeight = text->getContentSize().height + 30.f;
            auto* row = CCNode::create();
            row->setContentSize({width - 8.f, rowHeight});
            auto* bg = NineSlice::create("square02b_001.png");
            bg->setContentSize({width - 8.f, rowHeight});
            bg->setColor(info.id > m_seen ? ccc3(70, 46, 12) : ccc3(10, 32, 74));
            bg->setOpacity(225);
            bg->setPosition({(width - 8.f) / 2.f, rowHeight / 2.f});
            row->addChild(bg);
            auto* when =
                label(row, info.id > m_seen ? "NEW  " + localDateTime(info.createdAt) : localDateTime(info.createdAt),
                      {10.f, rowHeight - 11.f}, .5f, 260.f, info.id > m_seen ? ccc3(255, 210, 110) : kIce, true,
                      "chatFont.fnt");
            (void)when;
            if (!info.sender.empty())
                label(row, info.sender, {width - 18.f, rowHeight - 11.f}, .45f, 110.f, kMuted, false, "chatFont.fnt")
                    ->setAnchorPoint({1.f, .5f});
            text->setAnchorPoint({0.f, 1.f});
            text->setPosition({10.f, rowHeight - 22.f});
            row->addChild(text);
            rows.emplace_back(row, offset + rowHeight);
            content->addChild(row);
            offset += rowHeight + 5.f;
        }
        float const height = std::max(214.f, offset);
        content->setContentSize({width, height});
        for (auto const& [row, bottom] : rows) row->setPosition({4.f, height - bottom});
        m_scroll->scrollToTop();
    }

public:
    static BroadcastInboxPopup* create() {
        auto* result = new BroadcastInboxPopup;
        if (result->init()) {
            result->autorelease();
            return result;
        }
        delete result;
        return nullptr;
    }
};

// 인박스 종 아이콘을 벡터로 그려 만든다.
CCNode* inboxBellSprite() {
    auto* sprite = CCSprite::create();
    sprite->setContentSize({28.f, 28.f});
    auto* bg = NineSlice::create("square02b_001.png");
    bg->setContentSize({28.f, 28.f});
    bg->setColor(ccc3(60, 36, 110));
    bg->setOpacity(235);
    bg->setPosition({14.f, 14.f});
    sprite->addChild(bg);
    auto* bell = CCDrawNode::create();
    ccColor4F const gold{1.f, .82f, .32f, 1.f};
    bell->drawDot({14.f, 17.5f}, 5.2f, gold);
    CCPoint body[] = {{8.8f, 17.5f}, {19.2f, 17.5f}, {21.f, 9.5f}, {7.f, 9.5f}};
    bell->drawPolygon(body, 4, gold, 0.f, gold);
    CCPoint rim[] = {{5.5f, 10.5f}, {22.5f, 10.5f}, {22.5f, 8.5f}, {5.5f, 8.5f}};
    bell->drawPolygon(rim, 4, gold, 0.f, gold);
    bell->drawDot({14.f, 6.5f}, 2.f, gold);
    bell->drawDot({14.f, 23.f}, 1.4f, gold);
    sprite->addChild(bell);
    return sprite;
}

// Versus 화면들의 공통 베이스 레이어. 움직이는 배경, 뒤로가기 버튼, 제목, 디버그 배지를 만들고
// 방송/점검 배너 표시기를 등록한다. 하위 클래스는 goBack()을 구현한다.
class SceneLayer : public CCLayer {
protected:
    CCSize m_window;
    bool m_transitioning = false;
    CCMenuItemSpriteExtra* m_back = nullptr;
    bool m_updateOffered = false;
    // 앱 업데이트가 대기 중이고 안전한 시점(매치 밖)일 때 업데이트 안내 팝업을 한 번만 띄운다.
    // 랭크 탐색/맵 선정 중이었다면 그것이 중단되었음을 알린다(레이팅 변동 없음).
    void offerUpdate() {
        if (m_transitioning || m_updateOffered || !Service::get().updatePending() || !Service::get().safeToUpdate())
            return;
        CCNode* root = this;
        while (root->getParent()) root = root->getParent();
        if (root != CCDirector::sharedDirector()->getRunningScene()) return;
        m_updateOffered = true;
        auto const& ranked = Service::get().rankedState();
        bool const interrupted =
            ranked && (ranked->queuedAt > 0 ||
                       (ranked->match && ranked->match->state != "completed" && ranked->match->state != "playing"));
        createQuickPopup(
            "Versus Update",
            interrupted
                ? "A Versus update was released.\nYour Ranked search or map draw was stopped (rating unchanged).\nDownload the latest Versus from the server?"
                : "Your match is finished and its result is saved.\nDownload the latest Versus from the server?",
            "Later", "Update", [self = Weak<SceneLayer>(this)](auto*, bool yes) {
                if (auto owner = self.lock(); owner && yes && Service::get().safeToUpdate()) {
                    owner->m_transitioning = true;
                    update::showUpdateCheck();
                }
            });
    }

    // 디버그 모드일 때 왼쪽 위에 디버그 배지 텍스트를 표시한다.
    void placeDebugBadge() {
        if (auto* old = getChildByID("versus-debug-badge"_spr)) old->removeFromParent();
        if (auto const badge = debug::badgeText(); !badge.empty()) {
            auto* tag = label(this, badge, {44.f, m_window.height - 6.f}, .26f, 210.f, ccc3(255, 120, 200), false,
                              "bigFont.fnt");
            tag->setAnchorPoint({0.f, .5f});
            tag->setPositionX(44.f);
            tag->setID("versus-debug-badge"_spr);
            tag->setZOrder(200);
        }
    }
    bool initScene(std::string const& title) {
        if (!CCLayer::init()) return false;
        keepMenuCursorVisible(this);
        m_window = CCDirector::sharedDirector()->getWinSize();
        auto* background = MovingBackground::create();
        background->setID("versus-default-background"_spr);
        addChild(background, -10);
        auto* nav = menu(this);
        auto* backSprite = CCSprite::createWithSpriteFrameName("GJ_arrow_01_001.png");
        backSprite->setScale(.72f);
        m_back = CCMenuItemSpriteExtra::create(backSprite, this, menu_selector(SceneLayer::onBack));
        m_back->setPosition({23.f, m_window.height - 24.f});
        nav->addChild(m_back);
        if (!title.empty())
            label(this, title, {m_window.width / 2.f, m_window.height - 25.f}, .8f, m_window.width - 100.f);
        placeDebugBadge();
        setKeypadEnabled(true);
        Service::get().setBroadcastPresenter(showBroadcastBanner);
        Service::get().setMaintenancePresenter(showMaintenanceBanner);
        return true;
    }
    // 뒤로가기 동작 (하위 클래스가 구현).
    virtual void goBack() = 0;
    void onBack(CCObject*) { goBack(); }
    bool m_rankedAudio = false;
    // 오른쪽 위 톱니 버튼으로 사운드 설정을 연다. ranked는 설정 팝업에 넘길 모드.
    void addAudioButton(bool ranked) {
        m_rankedAudio = ranked;
        auto* nav = menu(this);
        nav->setZOrder(30);
        auto* gear = iconButton(nav, this, menu_selector(SceneLayer::onAudio), "GJ_optionsBtn_001.png",
                                {m_window.width - 24.f, m_window.height - 24.f}, 30.f);
        gear->setID("versus-sound-settings"_spr);
    }
    void onAudio(CCObject*) { showSettings(m_rankedAudio); }
    CCLabelBMFont* m_inboxCount = nullptr;
    CCNode* m_inboxBadge = nullptr;
    // 오른쪽 위에 LIVE 버튼과 방송 인박스(종) 버튼을 만든다(x: 종의 중심). 종 위의 빨간 배지에 안 읽은 개수를 표시.
    void addLiveButton(float x) {
        auto* nav = menu(this);
        nav->setZOrder(30);
        auto* sprite = ButtonSprite::create("LIVE", "bigFont.fnt", "GJ_button_06.png", .8f);
        sprite->setScale(.55f);
        auto* item = CCMenuItemSpriteExtra::create(sprite, this, menu_selector(SceneLayer::onLive));
        item->setID("versus-live-matches"_spr);
        nav->addChild(item);
        auto* bell = inboxBellSprite();
        m_inboxBadge = CCNode::create();
        auto* dot = CCDrawNode::create();
        dot->drawDot({0.f, 0.f}, 7.f, {.9f, .16f, .2f, 1.f});
        m_inboxBadge->addChild(dot);
        m_inboxCount = CCLabelBMFont::create("0", "bigFont.fnt");
        m_inboxCount->setScale(.32f);
        m_inboxBadge->addChild(m_inboxCount);
        m_inboxBadge->setPosition({25.f, 25.f});
        bell->addChild(m_inboxBadge, 5);
        auto* inbox = CCMenuItemSpriteExtra::create(bell, this, menu_selector(SceneLayer::onInbox));
        inbox->setID("versus-broadcast-inbox"_spr);
        nav->addChild(inbox);
        // x는 종의 중심: 종의 오른쪽 끝을 기준으로 종, LIVE를 실제 폭과 화면 비례 간격으로 놓는다.
        layoutRowRight({inbox, item}, x + drawnWidth(inbox) / 2.f, m_window.height - 24.f, rowGap(m_window.width),
                       0.f);
        refreshInbox(0.f);
        schedule(schedule_selector(SceneLayer::refreshInbox), .5f);
    }
    // 0.5초마다 방송을 갱신하고 안 읽은 개수(9 초과는 "9+")를 배지에 반영한다.
    void refreshInbox(float) {
        if (!m_inboxBadge) return;
        Service::get().keepBroadcastsFresh();
        int const unread = Service::get().unreadBroadcasts();
        m_inboxBadge->setVisible(unread > 0);
        if (unread > 0) m_inboxCount->setString(unread > 9 ? "9+" : std::to_string(unread).c_str());
    }
    void onInbox(CCObject*) {
        if (auto* popup = BroadcastInboxPopup::create()) popup->show();
        refreshInbox(0.f);
    }
    void onLive(CCObject*) { showLiveMatches(); }

public:
    // 뒤로가기 키는 DeferredPopup/KeyDefer 규칙에 따라 한 프레임 미뤄 처리한다.
    void keyBackClicked() override {
        deferBack(this, [this] { goBack(); });
    }
    void onEnter() override {
        CCLayer::onEnter();
#if defined(GEODE_IS_WINDOWS) || defined(GEODE_IS_MACOS)
        PlatformToolbox::toggleLockCursor(false);
        PlatformToolbox::showCursor();
#endif
        scheduleUpdate();
    }
    void onExit() override {
        unscheduleUpdate();
        CCLayer::onExit();
    }
};

// 퀵챗 프리셋(kQuickChat) 선택 팝업. 고른 문구를 콜백으로 넘기고 닫는다.
class QuickChatPopup final : public ::versus::DeferredPopup {
    std::function<void(std::string)> m_choose;
    bool init(std::function<void(std::string)> choose) {
        if (!Popup::init(390.f, 290.f, "GJ_square02.png")) return false;
        m_choose = std::move(choose);
        setTitle("Quick Chat", "bigFont.fnt", .63f);
        geode::addSideArt(m_mainLayer, SideArt::All, SideArtStyle::PopupBlue);
        for (int index = 0; index < static_cast<int>(kQuickChat.size()); ++index) {
            int const column = index % 2;
            int const row = index / 2;
            auto* card = CCSprite::create();
            card->setContentSize({170.f, 32.f});
            auto* bg = NineSlice::create("square02b_001.png");
            bg->setContentSize({170.f, 32.f});
            bg->setColor(ccc3(21, 44, 82));
            bg->setPosition({85.f, 16.f});
            card->addChild(bg);
            label(card, kQuickChat[index], {12.f, 16.f}, .44f, 147.f, ccWHITE, true, "bigFont.fnt");
            auto* item = CCMenuItemSpriteExtra::create(card, this, menu_selector(QuickChatPopup::onChoose));
            item->setPosition({105.f + column * 180.f, 239.f - row * 37.f});
            item->m_scaleMultiplier = 1.04f;
            m_buttonMenu->addChild(item);
            item->setTag(index);
        }
        return true;
    }
    void onChoose(CCObject* sender) {
        int const index = static_cast<CCNode*>(sender)->getTag();
        if (index < 0 || index >= static_cast<int>(kQuickChat.size())) return;
        auto callback = std::move(m_choose);
        Popup::onClose(nullptr);
        if (callback) callback(kQuickChat[index]);
    }

public:
    static QuickChatPopup* create(std::function<void(std::string)> choose) {
        auto* result = new QuickChatPopup;
        if (result->init(std::move(choose))) {
            result->autorelease();
            return result;
        }
        delete result;
        return nullptr;
    }
};

// 이모트 하나를 정사각 타일로 그린다. locked이면 반투명 + 자물쇠 아이콘.
CCSprite* emoteTile(std::string const& kind, float size, ccColor3B background, bool locked = false) {
    auto* sprite = CCSprite::create();
    sprite->setContentSize({size, size});
    auto* bg = NineSlice::create("square02b_001.png");
    bg->setContentSize({size, size});
    bg->setColor(background);
    bg->setPosition({size / 2.f, size / 2.f});
    sprite->addChild(bg);
    auto* art = emoteArt(kind);
    art->setScale(size / 38.f);
    art->setPosition({size / 2.f, size / 2.f});
    sprite->addChild(art);
    if (locked) {
        previewOpacity(sprite, 95);
        if (auto* lock = CCSprite::createWithSpriteFrameName("GJ_lock_001.png")) {
            lock->setScale(size * .42f / std::max(1.f, lock->getContentSize().height));
            lock->setPosition({size - 6.f, 7.f});
            sprite->addChild(lock, 5);
        }
    }
    return sprite;
}

// 이모트 선택/편집 팝업. send가 있으면 이모트를 눌러 전송하고, 없으면 퀵바 슬롯 편집 전용.
// 퀵바(emotes::kSlots개) 구성은 저장되며 멤버십 전용 이모트는 비회원에게 잠겨 있다.
class EmotePickerPopup final : public ::versus::DeferredPopup {
    static constexpr float kWidth = 420.f, kHeight = 280.f, kCell = 34.f;
    std::function<void(std::string)> m_send;
    std::function<void()> m_changed;
    std::vector<std::string> m_set;
    CCMenu* m_slots = nullptr;
    CCLabelBMFont* m_hint = nullptr;
    int m_editing = -1;
    bool m_membership = false;

    // 퀵바 슬롯을 다시 그리고 현재 모드에 맞는 안내 문구를 표시한다.
    void renderSlots() {
        m_slots->removeAllChildrenWithCleanup(true);
        for (int i = 0; i < emotes::kSlots; ++i) {
            bool const selected = i == m_editing;
            auto* tile = emoteTile(m_set[i], 30.f, selected ? ccc3(190, 120, 30) : ccc3(5, 27, 73));
            auto* item = CCMenuItemSpriteExtra::create(tile, this, menu_selector(EmotePickerPopup::onSlot));
            item->setTag(i);
            item->setPosition({kWidth / 2.f + (i - 2) * 36.f, kHeight - 62.f});
            if (selected) item->setScale(1.12f);
            m_slots->addChild(item);
        }
        m_hint->setString(m_editing >= 0 ? fmt::format("Pick an emote for slot {}{}", m_editing + 1,
                                                       m_send ? "  (tap the slot again to cancel)" : "")
                                               .c_str()
                          : m_send ? "Tap an emote to send it  -  tap a quick-bar slot to change it"
                                   : "Tap a quick-bar slot, then an emote");
        m_hint->limitLabelWidth(kWidth - 40.f, .32f, .2f);
    }
    void onSlot(CCObject* sender) {
        int const slot = sender->getTag();
        m_editing = m_editing == slot && m_send ? -1 : slot;
        renderSlots();
    }
    // 이모트 클릭: 잠긴 이모트면 안내, 슬롯 편집 중이면 그 슬롯에 지정(이미 있던 슬롯과는 서로 교환)하고
    // 저장, 아니면 이모트를 전송하고 닫는다.
    void onEmote(CCObject* sender) {
        int const index = sender->getTag();
        if (index < 0 || index >= static_cast<int>(emotes::kEmotes.size())) return;
        auto const& emote = emotes::kEmotes[index];
        std::string const id(emote.id);
        if (!emotes::usable(id, m_membership)) {
            FLAlertLayer::create("Membership Emote",
                                 fmt::format("<cy>{}</c> is part of the membership emote pack.", emote.name).c_str(),
                                 "OK")
                ->show();
            return;
        }
        if (!m_send && m_editing < 0) m_editing = 0;
        if (m_editing >= 0) {
            auto existing = std::find(m_set.begin(), m_set.end(), id);
            if (existing != m_set.end()) *existing = m_set[m_editing];
            m_set[m_editing] = id;
            emotes::saveQuickSet(m_set);
            m_editing = m_send ? -1 : (m_editing + 1) % emotes::kSlots;
            renderSlots();
            if (m_changed) m_changed();
            return;
        }
        auto send = std::move(m_send);
        Popup::onClose(nullptr);
        if (send) send(id);
    }
    // 퀵바를 기본값으로 되돌린다.
    void onReset(CCObject*) {
        emotes::resetQuickSet();
        m_set = emotes::quickSet(m_membership);
        m_editing = -1;
        renderSlots();
        if (m_changed) m_changed();
    }
    bool init(std::function<void(std::string)> send, std::function<void()> changed) {
        if (!Popup::init(kWidth, kHeight, "GJ_square02.png")) return false;
        m_send = std::move(send);
        m_changed = std::move(changed);
        m_membership = Service::get().profile().membership;
        m_set = emotes::quickSet(m_membership);
        if (!m_send) m_editing = 0;
        setTitle(m_send ? "Emotes" : "Customize Emotes", "goldFont.fnt", .75f);
        label(m_mainLayer, "QUICK BAR", {28.f, kHeight - 62.f}, .3f, 80.f, ccc3(255, 214, 120), true, "goldFont.fnt");
        m_slots = CCMenu::create();
        m_slots->setPosition({0.f, 0.f});
        m_mainLayer->addChild(m_slots, 3);
        m_hint =
            label(m_mainLayer, "", {kWidth / 2.f, kHeight - 86.f}, .32f, kWidth - 40.f, kIce, false, "chatFont.fnt");
        auto* grid = CCMenu::create();
        grid->setPosition({0.f, 0.f});
        m_mainLayer->addChild(grid, 3);
        constexpr int columns = 10;
        float const left = kWidth / 2.f - (columns - 1) * (kCell + 3.f) / 2.f;
        for (int index = 0; index < static_cast<int>(emotes::kEmotes.size()); ++index) {
            auto const& emote = emotes::kEmotes[index];
            bool const locked = emote.member && !m_membership;
            auto* tile =
                emoteTile(std::string(emote.id), kCell, emote.member ? ccc3(44, 20, 70) : ccc3(5, 27, 73), locked);
            auto* item = CCMenuItemSpriteExtra::create(tile, this, menu_selector(EmotePickerPopup::onEmote));
            item->setTag(index);
            item->setPosition(
                {left + (index % columns) * (kCell + 3.f), kHeight - 118.f - (index / columns) * (kCell + 3.f)});
            grid->addChild(item);
        }
        if (!m_membership)
            label(m_mainLayer, "Purple emotes are part of the membership pack", {kWidth / 2.f, 22.f}, .3f,
                  kWidth - 140.f, ccc3(205, 160, 255), false, "chatFont.fnt");
        button(menu(m_mainLayer), this, menu_selector(EmotePickerPopup::onReset), "Reset Bar", {kWidth - 52.f, 22.f},
               .45f, "GJ_button_04.png");
        renderSlots();
        return true;
    }

public:
    static EmotePickerPopup* create(std::function<void(std::string)> send, std::function<void()> changed) {
        auto* result = new EmotePickerPopup;
        if (result->init(std::move(send), std::move(changed))) {
            result->autorelease();
            return result;
        }
        delete result;
        return nullptr;
    }
};

// 방 PIN 입력 숫자 패드 팝업의 베이스. 입력값 m_pin, 표시/숨김 토글, 제출 중 상태를 관리하고
// 방 입장/방 만들기 팝업이 이를 상속해 제출 동작을 구현한다.
class PinPadPopup : public ::versus::DeferredPopup {
protected:
    std::string m_pin;
    bool m_reveal = false;
    bool m_pending = false;
    CCLabelBMFont* m_display = nullptr;
    CCLabelBMFont* m_status = nullptr;
    ButtonSprite* m_revealSprite = nullptr;
    CCMenuItemSpriteExtra* m_submit = nullptr;
    std::vector<CCMenuItemSpriteExtra*> m_keys;

    // 패드 UI 구성: 제목, PIN 표시창, Show 버튼, 숫자 키 12개(0~9, C=지우기 전체, <=한 칸 삭제).
    bool initPad(std::string const& title, std::string const& subtitle, std::string const& action,
                 std::string initial = {}, bool revealed = false) {
        if (!Popup::init(300.f, 300.f, "GJ_square02.png")) return false;
        geode::addSideArt(m_mainLayer, SideArt::All, SideArtStyle::PopupBlue);
        setTitle(title, "bigFont.fnt", .62f);
        label(m_mainLayer, subtitle, {150.f, 250.f}, .42f, 250.f, kIce);
        panel(m_mainLayer, {48.f, 206.f}, {170.f, 30.f}, false);
        m_display = label(m_mainLayer, "", {133.f, 221.f}, .55f, 158.f);
        m_revealSprite = ButtonSprite::create("Show", 44, true, "goldFont.fnt", "GJ_button_05.png", 26.f, .6f);
        m_revealSprite->setScale(.75f);
        auto* reveal = CCMenuItemSpriteExtra::create(m_revealSprite, this, menu_selector(PinPadPopup::onReveal));
        reveal->setPosition({247.f, 221.f});
        m_buttonMenu->addChild(reveal);
        static constexpr char const* kCaptions[12] = {"1", "2", "3", "4", "5", "6", "7", "8", "9", "C", "0", "<"};
        static constexpr int kTags[12] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 0, 11};
        for (int i = 0; i < 12; ++i) {
            auto* key = ButtonSprite::create(kCaptions[i], 34, true, "bigFont.fnt",
                                             i == 9 || i == 11 ? "GJ_button_06.png" : "GJ_button_04.png", 32.f, .75f);
            auto* item = CCMenuItemSpriteExtra::create(key, this, menu_selector(PinPadPopup::onKey));
            item->setTag(kTags[i]);
            item->setPosition({102.f + (i % 3) * 48.f, 180.f - (i / 3) * 36.f});
            m_buttonMenu->addChild(item);
            m_keys.push_back(item);
        }
        m_status = label(m_mainLayer, "", {150.f, 50.f}, .33f, 260.f, kMuted);
        m_submit = button(m_buttonMenu, this, menu_selector(PinPadPopup::onSubmit), action, {150.f, 26.f}, .62f);
        m_pin = initial.substr(0, MAX_ROOM_PIN);
        m_reveal = revealed;
        refresh();
        return true;
    }
    // PIN 표시창 갱신. 비어 있으면 안내 문구, 아니면 숫자(숨김 상태면 '*')를 보여 주고 Show/Hide 문구를 맞춘다.
    void refresh() {
        if (m_pin.empty()) {
            m_display->setString("Tap the numbers");
            m_display->setColor(kMuted);
        } else {
            m_display->setString(m_reveal ? m_pin.c_str() : std::string(m_pin.size(), '*').c_str());
            m_display->setColor(ccWHITE);
        }
        m_display->limitLabelWidth(158.f, .55f, .2f);
        m_revealSprite->setString(m_reveal ? "Hide" : "Show");
    }
    // 키 입력 처리. tag 0~9는 숫자 추가(MAX_ROOM_PIN까지), 10은 전체 지우기, 11은 한 글자 삭제.
    // 제출 중(m_pending)에는 입력을 무시한다.
    void type(int tag) {
        if (m_pending) return;
        if (tag >= 0 && tag <= 9) {
            if (m_pin.size() < MAX_ROOM_PIN) m_pin += static_cast<char>('0' + tag);
        } else if (tag == 10)
            m_pin.clear();
        else if (tag == 11 && !m_pin.empty())
            m_pin.pop_back();
        refresh();
    }
    void onKey(CCObject* sender) { type(sender->getTag()); }
    void onReveal(CCObject*) {
        m_reveal = !m_reveal;
        refresh();
    }
    // 제출 전에 PIN 형식을 검사한 뒤 하위 클래스의 submit()을 호출한다.
    void onSubmit(CCObject*) {
        if (m_pending) return;
        if (!validPin(m_pin)) {
            error("Enter <cy>1-32 digits</c>.");
            return;
        }
        submit();
    }
    // 제출 중 상태 전환: 모든 키/제출/닫기 버튼을 잠그고 상태 문구를 표시한다.
    void setPending(bool pending, std::string const& status) {
        m_pending = pending;
        enabled(m_submit, !pending);
        for (auto* key : m_keys) enabled(key, !pending);
        m_closeBtn->setEnabled(!pending);
        m_status->setString(status.c_str());
    }
    virtual void submit() = 0;
    // 키보드 입력도 숫자 패드와 같이 처리 (숫자열, 키패드, Backspace, Delete, Enter).
    void keyDown(enumKeyCodes key, double timestamp) override {
        if (key >= KEY_Zero && key <= KEY_Nine) {
            type(key - KEY_Zero);
            return;
        }
        if (key >= KEY_NumPad0 && key <= KEY_NumPad9) {
            type(key - KEY_NumPad0);
            return;
        }
        if (key == KEY_Backspace) {
            type(11);
            return;
        }
        if (key == KEY_Delete) {
            type(10);
            return;
        }
        if (key == KEY_Enter) {
            onSubmit(nullptr);
            return;
        }
        Popup::keyDown(key, timestamp);
    }
    // 요청이 진행 중일 때는 팝업을 닫지 못하게 한다.
    void onClose(CCObject* sender) override {
        if (!m_pending) Popup::onClose(sender);
    }
};

// 방 비밀번호를 정하는 패드. 확인하면 입력값을 콜백으로 넘기고 닫는다 (서버 요청은 하지 않음).
class RoomPinPopup : public PinPadPopup {
    std::function<void(std::string)> m_done;
    void submit() override {
        if (m_done) m_done(m_pin);
        onClose(nullptr);
    }

public:
    static RoomPinPopup* create(std::string current, std::function<void(std::string)> done) {
        auto* result = new RoomPinPopup;
        result->m_done = std::move(done);
        if (result->initPad("Room Password", "Choose 1-32 digits for your private room", "Set", std::move(current),
                            true)) {
            result->autorelease();
            return result;
        }
        delete result;
        return nullptr;
    }
};

// 방 만들기 팝업. Classic(1v1)/Group 탭, 공개/비공개(PIN), 핵 허용 여부, Group 최대 인원을 정하고
// 서버에 방 생성을 요청한다. 성공하면 방 화면(showRoom)으로 넘어간다.
class CreateRoomPopup : public ::versus::DeferredPopup {
    std::string m_pinValue;
    CCLabelBMFont* m_privacy = nullptr;
    CCLabelBMFont* m_pinLabel = nullptr;
    CCLabelBMFont* m_pinCaption = nullptr;
    CCNode* m_pinBox = nullptr;
    CCLabelBMFont* m_status = nullptr;
    CCSprite* m_lockSprite = nullptr;
    CCMenuItemSpriteExtra* m_submit = nullptr;
    CCMenuItemSpriteExtra* m_lock = nullptr;
    CCMenuItemSpriteExtra* m_setPin = nullptr;
    bool m_private = false;
    bool m_pending = false;
    bool m_allowHacks = false;
    CCMenuItemToggler* m_hacks = nullptr;
    bool m_group = false;
    bool m_debug = false;
    int m_maxPlayers = kGroupMaxPlayers;
    CCMenuItemSpriteExtra* m_classicTab = nullptr;
    CCMenuItemSpriteExtra* m_groupTab = nullptr;
    CCMenuItemSpriteExtra* m_fewer = nullptr;
    CCMenuItemSpriteExtra* m_more = nullptr;
    CCLabelBMFont* m_players = nullptr;
    CCLabelBMFont* m_playersCaption = nullptr;
    CCLabelBMFont* m_modeNote = nullptr;
    TextInput* m_playersInput = nullptr;
    void onExit() override {
        if (m_playersInput) m_playersInput->defocus();
        Popup::onExit();
    }

    // 현재 탭(Classic/Group)에 맞게 탭 색, 인원 표시/입력, 안내 문구, 증감 버튼 상태를 갱신한다.
    void showCategory() {
        auto tint = [](CCMenuItemSpriteExtra* item, bool selected) {
            static_cast<ButtonSprite*>(item->getNormalImage())
                ->m_BGSprite->setColor(selected ? ccc3(100, 255, 125) : ccc3(140, 140, 140));
        };
        tint(m_classicTab, !m_group);
        tint(m_groupTab, m_group);
        m_fewer->setVisible(m_group);
        m_more->setVisible(m_group);
        m_players->setVisible(!m_group);
        m_playersInput->setVisible(m_group);
        m_playersInput->setEnabled(m_group && !m_pending);
        if (!m_group) m_playersInput->defocus();
        m_playersCaption->setString(m_group ? "MAX PLAYERS" : "PLAYERS");
        m_modeNote->setString(m_group ? "Lives match - host-only emotes & chat" : "");
        enabled(m_fewer, !m_pending && m_maxPlayers > kGroupMinPlayers);
        enabled(m_more, !m_pending && m_maxPlayers < kGroupMaxPlayers);
    }
    void onClassic(CCObject*) {
        if (!m_pending) {
            m_group = false;
            showCategory();
        }
    }
    void onGroup(CCObject*) {
        if (!m_pending) {
            m_group = true;
            showCategory();
        }
    }
    // Group 최대 인원을 delta만큼 조절한다 (kGroupMinPlayers~kGroupMaxPlayers로 제한).
    void stepPlayers(int delta) {
        if (m_pending) return;
        m_maxPlayers = std::clamp(m_maxPlayers + delta, kGroupMinPlayers, kGroupMaxPlayers);
        m_playersInput->setString(std::to_string(m_maxPlayers).c_str());
        showCategory();
    }
    void onFewer(CCObject*) { stepPlayers(-1); }
    void onMore(CCObject*) { stepPlayers(1); }
    // 인원 입력창에 직접 입력한 값이 유효 범위일 때만 반영하고 증감 버튼 상태를 맞춘다.
    void typedPlayers(int value) {
        if (value >= kGroupMinPlayers && value <= kGroupMaxPlayers) m_maxPlayers = value;
        enabled(m_fewer, !m_pending && m_maxPlayers > kGroupMinPlayers);
        enabled(m_more, !m_pending && m_maxPlayers < kGroupMaxPlayers);
    }

    // 핵 허용 토글 (이 경우 안티치트 없이 각자 사용한 핵이 표시된다).
    void onHacks(CCObject*) {
        if (m_pending) return;
        m_allowHacks = !m_hacks->isToggled();
    }
    bool init() {
        if (!Popup::init(310.f, 300.f, "GJ_square02.png")) return false;
        geode::addSideArt(m_mainLayer, SideArt::All, SideArtStyle::PopupBlue);
        setTitle(m_debug ? "Create Debug Room" : "Create Room", "bigFont.fnt", .65f);
        m_classicTab =
            button(m_buttonMenu, this, menu_selector(CreateRoomPopup::onClassic), "Classic", {112.f, 250.f}, .55f);
        m_classicTab->setID("room-category-classic"_spr);
        m_groupTab = button(m_buttonMenu, this, menu_selector(CreateRoomPopup::onGroup), "Group", {200.f, 250.f}, .55f);
        m_groupTab->setID("room-category-group"_spr);
        label(m_mainLayer, "Room name", {155.f, 226.f}, .34f, 250.f, kIce);
        panel(m_mainLayer, {32.f, 188.f}, {246.f, 28.f}, false);
        auto const& profile = Service::get().profile();
        styleNickname(label(m_mainLayer, m_debug ? "Debug (" + profile.name + ")" : profile.name + "'s Room",
                            {155.f, 202.f}, .42f, 232.f),
                      profile.membership, profile.cosmetics);
        m_playersCaption = label(m_mainLayer, "PLAYERS", {244.f, 179.f}, .24f, 80.f, kMuted);
        m_players = label(m_mainLayer, "1 vs 1", {244.f, 166.f}, .4f, 70.f, ccWHITE);
        m_playersInput = numberInput(m_mainLayer, {244.f, 166.f}, m_maxPlayers, 2, 36.f,
                                     [self = Weak<CreateRoomPopup>(this)](int value) {
                                         if (auto owner = self.lock()) owner->typedPlayers(value);
                                     });
        m_playersInput->setID("group-max-players-input"_spr);
        m_fewer = button(m_buttonMenu, this, menu_selector(CreateRoomPopup::onFewer), "<", {214.f, 166.f}, .45f,
                         "GJ_button_04.png");
        m_more = button(m_buttonMenu, this, menu_selector(CreateRoomPopup::onMore), ">", {274.f, 166.f}, .45f,
                        "GJ_button_04.png");
        m_modeNote = label(m_mainLayer, "", {155.f, 108.f}, .3f, 250.f, ccc3(255, 215, 120), false, "chatFont.fnt");
        auto* hacksPanel = NineSlice::create("square02b_001.png");
        hacksPanel->setID("create-room-hacks-panel"_spr);
        hacksPanel->setContentSize({272.f, 36.f});
        hacksPanel->setPosition({155.f, 83.f});
        hacksPanel->setColor(ccc3(0, 0, 0));
        hacksPanel->setOpacity(120);
        m_mainLayer->addChild(hacksPanel);
        m_hacks = CCMenuItemToggler::createWithStandardSprites(this, menu_selector(CreateRoomPopup::onHacks), .6f);
        m_hacks->setPosition({52.f, 84.f});
        m_buttonMenu->addChild(m_hacks);
        label(m_mainLayer, "Allow hacks (Mega Hack etc.)", {72.f, 90.f}, .36f, 210.f, ccc3(255, 190, 110), true);
        label(m_mainLayer, "No anti-cheat; each player's hacks are shown", {72.f, 76.f}, .3f, 210.f, kMuted, true,
              "chatFont.fnt");

        m_lockSprite = CCSprite::createWithSpriteFrameName("GJ_lockGray_001.png");
        m_lockSprite->setScale(.7f);
        m_lock = CCMenuItemSpriteExtra::create(m_lockSprite, this, menu_selector(CreateRoomPopup::onPrivacy));
        m_lock->setPosition({68.f, 167.f});
        m_buttonMenu->addChild(m_lock);
        m_privacy = label(m_mainLayer, "Public room", {88.f, 167.f}, .4f, 100.f, kIce, true);
        m_pinCaption = label(m_mainLayer, "Password", {116.f, 148.f}, .3f, 160.f, kIce);
        m_pinBox = panel(m_mainLayer, {32.f, 114.f}, {168.f, 26.f}, false);
        m_pinLabel = label(m_mainLayer, "", {116.f, 127.f}, .4f, 156.f, ccWHITE);
        m_setPin = button(m_buttonMenu, this, menu_selector(CreateRoomPopup::onSetPin), "Set", {241.f, 127.f}, .5f);
        for (CCNode* node : std::initializer_list<CCNode*>{m_setPin, m_pinLabel, m_pinCaption, m_pinBox})
            node->setVisible(false);
        m_status = label(m_mainLayer, "", {155.f, 56.f}, .35f, 255.f, kIce);
        m_submit =
            button(m_buttonMenu, this, menu_selector(CreateRoomPopup::onCreate), "Create Room", {155.f, 31.f}, .62f);
        m_lockSprite->setOpacity(110);
        showCategory();
        return true;
    }
    // PIN 표시 라벨과 Set/Change 버튼 문구를 갱신한다.
    void showPin() {
        m_pinLabel->setString(m_pinValue.empty() ? "Not set" : m_pinValue.c_str());
        m_pinLabel->setColor(m_pinValue.empty() ? kMuted : ccWHITE);
        m_pinLabel->limitLabelWidth(156.f, .4f, .15f);
        auto* sprite = ButtonSprite::create(m_pinValue.empty() ? "Set" : "Change", "goldFont.fnt", "GJ_button_01.png");
        sprite->setScale(.5f);
        m_setPin->setNormalImage(sprite);
        m_setPin->updateSprite();
    }
    void onSetPin(CCObject*) {
        if (m_pending || !m_private) return;
        if (auto* pad = RoomPinPopup::create(m_pinValue, [self = Weak<CreateRoomPopup>(this)](std::string pin) {
                if (auto owner = self.lock()) {
                    owner->m_pinValue = std::move(pin);
                    owner->showPin();
                }
            }))
            pad->show();
    }
    // 공개/비공개 전환. 비공개로 바꿨는데 PIN이 없으면 바로 PIN 패드를 연다.
    void onPrivacy(CCObject*) {
        if (m_pending) return;
        m_private = !m_private;
        m_privacy->setString(m_private ? "Private room" : "Public room");
        m_privacy->limitLabelWidth(100.f, .4f, .2f);
        m_modeNote->setVisible(!m_private);
        m_lockSprite->setOpacity(m_private ? 255 : 110);
        m_lockSprite->setColor(m_private ? kIce : ccc3(255, 255, 255));
        for (CCNode* node : std::initializer_list<CCNode*>{m_setPin, m_pinLabel, m_pinCaption, m_pinBox})
            node->setVisible(m_private);
        showPin();
        if (m_private && m_pinValue.empty()) onSetPin(nullptr);
    }
    // 방 생성 요청. 입력을 검증(Group 인원 범위, 비공개면 PIN 형식)한 뒤 버튼을 모두 잠그고
    // Group이면 createGroup, 아니면 createRoom을 호출한다. 실패하면 버튼을 다시 풀고 오류를 보여 준다.
    void onCreate(CCObject*) {
        if (m_pending) return;
        std::string const pin = m_private ? m_pinValue : std::string();
        if (m_group) {
            int const typed = typedNumber(m_playersInput);
            if (typed < kGroupMinPlayers || typed > kGroupMaxPlayers) {
                error(fmt::format("Enter <cy>{}-{}</c> players.", kGroupMinPlayers, kGroupMaxPlayers));
                return;
            }
            m_maxPlayers = typed;
            m_playersInput->defocus();
        }
        if (m_private && !validPin(pin)) {
            error("Set a password of <cy>1-32 digits</c> first.");
            onSetPin(nullptr);
            return;
        }
        m_pending = true;
        enabled(m_submit, false);
        enabled(m_lock, false);
        enabled(m_setPin, false);
        m_closeBtn->setEnabled(false);
        m_status->setString("Creating room...");
        enabled(m_classicTab, false);
        enabled(m_groupTab, false);
        enabled(m_fewer, false);
        enabled(m_more, false);
        m_playersInput->setEnabled(false);
        auto finished = [self = Weak<CreateRoomPopup>(this)](bool success, std::string detail) {
            auto owner = self.lock();
            if (!owner) return;
            owner->m_pending = false;
            if (success) {
                owner->onClose(nullptr);
                showRoom();
                return;
            }
            enabled(owner->m_submit, true);
            enabled(owner->m_lock, true);
            enabled(owner->m_setPin, true);
            enabled(owner->m_classicTab, true);
            enabled(owner->m_groupTab, true);
            owner->showCategory();
            owner->m_closeBtn->setEnabled(true);
            owner->m_status->setString("Creation failed. Try again.");
            error(detail);
        };
        if (m_group) {
            Service::get().createGroup(pin, m_maxPlayers, m_allowHacks, std::move(finished), m_debug);
            return;
        }
        auto name = Service::get().profile().name + "'s Room";
        Service::get().createRoom(
            std::move(name), pin,
            [self = Weak<CreateRoomPopup>(this)](bool success, std::string detail) {
                auto owner = self.lock();
                if (!owner) return;
                owner->m_pending = false;
                if (success) {
                    owner->onClose(nullptr);
                    showRoom();
                    return;
                }
                enabled(owner->m_submit, true);
                enabled(owner->m_lock, true);
                enabled(owner->m_setPin, true);
                owner->m_closeBtn->setEnabled(true);
                owner->m_status->setString("Creation failed. Try again.");
                error(detail);
            },
            m_allowHacks, m_debug);
    }
    void onClose(CCObject* sender) override {
        if (!m_pending) Popup::onClose(sender);
    }

public:
    static CreateRoomPopup* create(bool debug = false) {
        auto* result = new CreateRoomPopup;
        result->m_debug = debug;
        if (result->init()) {
            result->autorelease();
            return result;
        }
        delete result;
        return nullptr;
    }
};

// 비공개 1v1 방에 PIN을 입력해 참가하는 패드.
class JoinRoomPopup : public PinPadPopup {
    RoomInfo m_room;
    void submit() override {
        setPending(true, "Joining...");
        Service::get().joinRoom(m_room, m_pin, [self = Weak<JoinRoomPopup>(this)](bool success, std::string detail) {
            auto owner = self.lock();
            if (!owner) return;
            owner->setPending(false, "");
            if (success) {
                owner->onClose(nullptr);
                showRoom();
                return;
            }
            owner->m_status->setString("Unable to join. Check the password.");
            error(detail);
        });
    }

public:
    static JoinRoomPopup* create(RoomInfo room) {
        auto* result = new JoinRoomPopup;
        result->m_room = std::move(room);
        if (result->initPad("Private Room", result->m_room.name, "Join")) {
            result->autorelease();
            return result;
        }
        delete result;
        return nullptr;
    }
};

// 비공개 Group 방에 PIN을 입력해 참가하는 패드.
class JoinGroupPopup : public PinPadPopup {
    GroupSummary m_room;
    void submit() override {
        setPending(true, "Joining...");
        Service::get().joinGroup(m_room.id, m_pin,
                                 [self = Weak<JoinGroupPopup>(this)](bool success, std::string detail) {
                                     auto owner = self.lock();
                                     if (!owner) return;
                                     owner->setPending(false, "");
                                     if (success) {
                                         owner->onClose(nullptr);
                                         showRoom();
                                         return;
                                     }
                                     owner->m_status->setString("Unable to join. Check the password.");
                                     error(detail);
                                 });
    }

public:
    static JoinGroupPopup* create(GroupSummary room) {
        auto* result = new JoinGroupPopup;
        result->m_room = std::move(room);
        if (result->initPad("Private Group Room", result->m_room.name, "Join")) {
            result->autorelease();
            return result;
        }
        delete result;
        return nullptr;
    }
};

// 칭호 배지 그리기 (인라인 include).
#include "TitleBadge.inl"

// 새로 얻은 칭호를 화면 오른쪽 아래에 순서대로 띄우는 토스트 큐. 한 번에 하나씩 보여 주고
// 끝나면 다음 항목으로 넘어간다.
namespace title_toast {
std::deque<int>& queue() {
    static std::deque<int> value;
    return value;
}
bool& showing() {
    static bool value = false;
    return value;
}
void showNext() {
    auto& pending = queue();
    if (pending.empty()) {
        showing() = false;
        return;
    }
    showing() = true;
    int const title = pending.front();
    pending.pop_front();
    auto const win = CCDirector::sharedDirector()->getWinSize();
    CCSize const size{196.f, 52.f};
    auto* toast = CCNode::create();
    toast->setContentSize(size);
    toast->setID("title-unlocked-toast"_spr);
    auto* bg = NineSlice::create("square02b_001.png");
    bg->setContentSize(size);
    bg->setPosition(size / 2.f);
    bg->setColor(ccc3(10, 18, 42));
    bg->setOpacity(240);
    toast->addChild(bg, -2);
    auto* accent = CCLayerColor::create(ccc4(255, 206, 90, 230), 3.f, size.height - 12.f);
    accent->setPosition({6.f, 6.f});
    toast->addChild(accent, -1);
    auto* glow = CCDrawNode::create();
    art::glowDot(glow, {30.f, 26.f}, 22.f, ccc3(255, 206, 90), .8f);
    glow->setBlendFunc({GL_SRC_ALPHA, GL_ONE});
    toast->addChild(glow);
    auto* trophy = art::glyph(art::Glyph::Trophy, ccc3(255, 196, 70));
    trophy->setScale(.5f);
    trophy->setPosition({30.f, 26.f});
    toast->addChild(trophy);
    trophy->runAction(
        CCSequence::create(CCDelayTime::create(.2f), CCEaseBackOut::create(CCScaleTo::create(.3f, 2.4f)), nullptr));
    label(toast, "TITLE UNLOCKED", {118.f, 38.f}, .5f, 140.f, ccc3(255, 221, 120), false, "goldFont.fnt");
    titleBadge(toast, title, {118.f, 17.f}, 1.3f, 140.f);
    toast->setPosition({win.width + 4.f, 10.f});
    OverlayManager::get()->addChild(toast, 1000);
    audio::playBuiltin("achievement_01.ogg");
    toast->runAction(CCSequence::create(
        CCEaseBackOut::create(CCMoveTo::create(.38f, {win.width - size.width - 10.f, 10.f})), CCDelayTime::create(3.2f),
        CCEaseSineIn::create(CCMoveTo::create(.3f, {win.width + 4.f, 10.f})), CallFuncExt::create([] { showNext(); }),
        CCRemoveSelf::create(), nullptr));
}
}
// 서버에서 받은 업적 상태의 칭호 목록을 기기에 저장된 "이미 아는 칭호"와 비교해 새 칭호는 토스트로 알린다.
// 내 계정 데이터만 처리하며, 처음 저장하는 경우에는 토스트 없이 기준만 기록한다.
void noteTitles(AchievementState const& state) {
    auto const& me = Service::get().profile();
    if (state.uid.empty() || state.uid != me.uid) return;
    auto const key = "known-titles-" + state.uid;
    auto* mod = Mod::get();
    bool const first = !mod->hasSavedValue(key);
    std::set<int> known;
    for (auto const& part : utils::string::split(mod->getSavedValue<std::string>(key, ""), ",")) {
        auto value = utils::numFromString<int>(part);
        if (value.isOk()) known.insert(value.unwrap());
    }
    std::vector<int> fresh;
    for (int title : state.titles)
        if (title > 0 && validTitle(title) && known.insert(title).second) fresh.push_back(title);
    if (fresh.empty() && !first) return;
    std::string saved;
    for (int title : known) saved += (saved.empty() ? "" : ",") + std::to_string(title);
    mod->setSavedValue(key, saved);
    if (first) return;
    for (int title : fresh) title_toast::queue().push_back(title);
    if (!title_toast::showing()) title_toast::showNext();
}
// 업적 상태를 서버에서 받아 칭호 알림을 확인한다. 최소 15초 간격으로만 요청한다.
void watchTitles() {
    static auto last = std::chrono::steady_clock::time_point{};
    auto const now = std::chrono::steady_clock::now();
    if (last != std::chrono::steady_clock::time_point{} && now - last < std::chrono::seconds(15)) return;
    last = now;
    Service::get().fetchAchievements({}, [](std::optional<AchievementState> state, std::string) {
        if (state) noteTitles(*state);
    });
}
void showPlayer(PlayerProfile const& profile);

// 기록 시간(ms)을 "12.3s" 또는 "1:02.3" 형태로 표시한다.
std::string runTimeText(int64_t ms) {
    if (ms < 0) return "";
    auto const tenths = (ms + 50) / 100;
    if (tenths < 600) return fmt::format("{}.{}s", tenths / 10, tenths % 10);
    return fmt::format("{}:{:02}.{}", tenths / 600, (tenths / 10) % 60, tenths % 10);
}
// epoch ms를 로컬 시간 "YYYY-MM-DD HH:MM"으로 바꾼다.
std::string localDateTime(int64_t ms) {
    std::time_t const seconds = static_cast<std::time_t>(ms / 1000);
    std::tm local{};
#ifdef GEODE_IS_WINDOWS
    if (localtime_s(&local, &seconds) != 0) return "--";
#else
    if (!localtime_r(&seconds, &local)) return "--";
#endif
    char text[32] = {};
    std::strftime(text, sizeof text, "%Y-%m-%d %H:%M", &local);
    return text;
}
// 기간(ms)을 "5m", "3h 20m", "2d 4h" 형태로 표시한다 (분은 올림, 최소 1m).
std::string spanText(int64_t ms) {
    ms = std::max<int64_t>(0, ms);
    int64_t const minutes = (ms + 59'999) / 60'000;
    if (minutes < 60) return fmt::format("{}m", std::max<int64_t>(1, minutes));
    int64_t const hours = minutes / 60;
    if (hours < 48) return fmt::format("{}h {}m", hours, minutes % 60);
    return fmt::format("{}d {}h", hours / 24, hours % 24);
}
// 경과 시간 문구 ("just now" 또는 "... ago").
std::string agoText(int64_t ms) {
    return ms < 60'000 ? std::string("just now") : spanText(ms) + " ago";
}
// 경기 시각 표시. 7일 이내면 상대 시간, 그보다 오래되면 날짜만 보여 준다.
std::string playedText(int64_t playedAt) {
    auto const age = Service::get().serverNow() - playedAt;
    return age < 7 * 86'400'000 ? agoText(age) : localDateTime(playedAt).substr(0, 10);
}
// 맵 길이 번호(0~5)를 이름으로 바꾼다.
char const* lengthName(int length) {
    static char const* const names[] = {"Tiny", "Short", "Medium", "Long", "XL", "Platformer"};
    return length >= 0 && length <= 5 ? names[length] : "";
}

// 전적 기록 종류 태그와 색. 공개 방/랭크/랭크 듀얼, 취소된 경기, 레이팅 변동을 구분한다.
std::pair<std::string, ccColor3B> matchKindTag(MatchRecord const& match) {
    if (!match.ranked) return {"PUBLIC ROOM", ccc3(150, 200, 255)};
    std::string text = match.duel ? "RANKED DUEL" : "RANKED";
    if (match.canceled) return {text + " CANCELED", ccc3(170, 170, 185)};
    if (match.ratingKnown) text += fmt::format(" {:+}", match.ratingChange);
    return {text, match.ratingKnown && match.ratingChange < 0 ? ccc3(255, 160, 140) : ccc3(255, 205, 110)};
}

// 경기가 어떤 이유로 결정됐는지 설명 문구를 만든다 (안티치트 실격, 일시정지 시간 소진, 이탈,
// 목표 퍼센트 선도달). 상세 기록이 없거나 취소/무승부면 비운다.
std::string decisionReason(MatchRecord const& match) {
    if (!match.detailed || match.canceled || match.result == "draw") return {};
    bool const won = match.result == "win";
    auto const& lost = won ? match.opponentStats : match.selfStats;
    auto const& name = (won ? match.opponent : match.self).name;
    // 일시정지 허용 시간 20초(ms). 50ms 여유로 소진 여부를 판단한다.
    constexpr int64_t kPauseBudget = 20000;
    if (!lost.integrityCode.empty()) return name + " was disqualified by the anti-cheat";
    if (lost.forfeited)
        return lost.pauseUsedMs >= kPauseBudget - 50 ? name + " ran out of pause time" : name + " left or disconnected";
    if (lost.paused || lost.pauseUsedMs >= kPauseBudget) return name + " ran out of pause time";
    if (match.rules.mode == 1) return fmt::format("first to reach {}%", match.rules.targetPercent);
    return {};
}

// 이전 기록 문구 (클리어함 / 이전 최고 N% / 첫 도전).
std::string beforeText(int best) {
    if (best >= 100) return "Cleared before";
    if (best >= 0) return fmt::format("Before {}%", best);
    if (best == -1) return "First time";
    return {};
}

// 맵 평가(좋아요/싫어요) 투표 UI (인라인 include).
#include "MapVotes.inl"

// 경기 한 건의 상세 팝업. 규칙, 양쪽 플레이어, 최고 퍼센트, 승패 사유와 시도(attempt)별 기록을
// 10개씩 페이지로 서버에서 받아 보여 주고, 리플레이/맵 보기/레벨 ID 복사를 제공한다.
class MatchDetailPopup : public ::versus::DeferredPopup {
    MatchRecord m_match;
    CCNode* m_rows = nullptr;
    CCLabelBMFont* m_pageLabel = nullptr;
    CCMenuItemSpriteExtra *m_previous = nullptr, *m_next = nullptr;
    int m_page = 0, m_generation = 0;
    std::function<void(int64_t, int)> m_onVoted;
    bool init(MatchRecord match, std::function<void(int64_t, int)> onVoted) {
        if (!Popup::init(420.f, 294.f, "GJ_square02.png")) return false;
        m_match = std::move(match);
        m_onVoted = std::move(onVoted);
        setTitle(m_match.ranked ? (m_match.duel ? "Ranked Duel" : "Ranked Match") : "Public Match", "bigFont.fnt", .6f);
        label(m_mainLayer, m_match.levelName, {210.f, 251.f}, .44f, 270.f, kIce);
        if (mapvotes::shown() && m_match.ranked && m_match.levelId > 0 && !m_match.self.uid.empty() &&
            m_match.self.uid == Service::get().profile().uid) {
            label(m_mainLayer, "RATE THIS MAP", {386.f, 266.f}, .18f, 60.f, kMuted, false, "bigFont.fnt");
            mapVoteButtons(m_mainLayer, m_match.levelId, m_match.levelName, {386.f, 248.f}, 22.f, -2,
                           [self = Weak<MatchDetailPopup>(this)](MapVotes const& votes) {
                               if (auto owner = self.lock(); owner && owner->m_onVoted)
                                   owner->m_onVoted(owner->m_match.levelId, votes.mine);
                           });
        }
        if (m_match.playedAt > 0)
            label(m_mainLayer,
                  fmt::format("Played {} ({})", localDateTime(m_match.playedAt),
                              agoText(Service::get().serverNow() - m_match.playedAt)),
                  {210.f, 238.f}, .42f, 300.f, kMuted, false, "chatFont.fnt");
        auto* view =
            button(m_buttonMenu, this, menu_selector(MatchDetailPopup::onView), "View Map", {119.f, 224.f}, .4f);
        auto* copy =
            button(m_buttonMenu, this, menu_selector(MatchDetailPopup::onCopy), "Copy ID", {299.f, 224.f}, .4f);
        enabled(view, m_match.levelId > 0);
        enabled(copy, m_match.levelId > 0);
        if (m_match.ranked && m_match.ratingKnown)
            label(m_mainLayer, fmt::format("{:+}  R{}", m_match.ratingChange, m_match.newRating), {210.f, 224.f}, .32f,
                  100.f, matchKindTag(m_match).second);
        label(m_mainLayer, m_match.detailed ? rulesText(m_match.rules) : "Details unavailable for this older match",
              {210.f, 203.f}, .55f, 380.f, kIce, false, "chatFont.fnt");
        if (m_match.detailed) {
            player(m_mainLayer, m_match.self, {88.f, 174.f}, .55f);
            player(m_mainLayer, m_match.opponent, {285.f, 174.f}, .55f);
            for (int side = 0; side < 2; ++side) {
                auto const& who = side ? m_match.opponent : m_match.self;
                auto* nameLabel = createCosmeticLabel(who.name.c_str(), "bigFont.fnt");
                nameLabel->setScale(
                    std::min(.3f, (side ? 100.f : 110.f) / std::max(1.f, nameLabel->getContentSize().width)));
                styleNickname(nameLabel, who.membership, who.cosmetics);
                auto* hit = CCMenuItemSpriteExtra::create(nameLabel, this, menu_selector(MatchDetailPopup::onName));
                hit->setTag(side);
                hit->setAnchorPoint({0.f, .5f});
                hit->setPosition({side ? 304.f : 107.f, 180.f});
                m_buttonMenu->addChild(hit);
            }
            label(m_mainLayer, fmt::format("Best {}%", m_match.selfStats.bestPercent), {107.f, 167.f}, .5f, 110.f, kIce,
                  true, "chatFont.fnt");
            label(m_mainLayer, fmt::format("Best {}%", m_match.opponentStats.bestPercent), {304.f, 167.f}, .5f, 100.f,
                  kIce, true, "chatFont.fnt");
            for (int side = 0; side < 2; ++side) {
                auto const& stats = side ? m_match.opponentStats : m_match.selfStats;
                float const x = side ? 304.f : 107.f, width = side ? 100.f : 110.f;
                auto const before = beforeText(side ? m_match.opponentBefore : m_match.selfBefore);
                if (!before.empty())
                    label(m_mainLayer, before, {x, 157.f}, .38f, width * .6f, kMuted, true, "chatFont.fnt")
                        ->setID(side ? "history-before-opponent"_spr : "history-before-self"_spr);
                if (stats.tps > 0) {
                    auto* tps =
                        label(m_mainLayer, fmt::format("{} TPS", stats.tps), {x + width * .62f, 157.f}, .38f,
                              width * .38f, stats.tps == kGameTps ? kMuted : ccc3(255, 96, 84), true, "chatFont.fnt");
                    tps->setID(side ? "history-tps-opponent"_spr : "history-tps-self"_spr);
                }
            }
        }
        auto const reason = decisionReason(m_match);
        styleNickname(label(m_mainLayer,
                            m_match.result == "draw" ? "DRAW"
                                                     : m_match.winnerName + " wins" +
                                                           (reason.empty() ? std::string() : "  (" + reason + ")"),
                            {210.f, 147.f}, .45f, 370.f, kIce, false, "chatFont.fnt"),
                      historyWinnerMembership(m_match.result, m_match.detailed, m_match.self.membership,
                                              m_match.opponent.membership),
                      (m_match.result == "win" ? m_match.self : m_match.opponent).cosmetics);
        m_rows = CCNode::create();
        m_mainLayer->addChild(m_rows);
        m_previous = button(m_buttonMenu, this, menu_selector(MatchDetailPopup::onPrevious), "<", {153.f, 21.f}, .4f);
        m_next = button(m_buttonMenu, this, menu_selector(MatchDetailPopup::onNext), ">", {267.f, 21.f}, .4f);
        m_pageLabel = label(m_mainLayer, "", {210.f, 21.f}, .3f, 75.f, kIce);
        fetch();
        if (m_match.detailed) fetchReplays();
        return true;
    }
    // 이 경기에서 받을 수 있는 리플레이 목록.
    std::vector<ReplaySummary> m_replays;
    // 양쪽 플레이어의 리플레이가 있으면 "Replay N%" 버튼을 만든다.
    void fetchReplays() {
        Service::get().fetchReplayList(
            m_match.id, [self = Weak<MatchDetailPopup>(this)](std::vector<ReplaySummary> list, std::string) {
                auto owner = self.lock();
                if (!owner) return;
                owner->m_replays = std::move(list);
                for (int side = 0; side < 2; ++side) {
                    auto const& who = side ? owner->m_match.opponent : owner->m_match.self;
                    auto found = std::find_if(owner->m_replays.begin(), owner->m_replays.end(),
                                              [&who](ReplaySummary const& entry) { return entry.uid == who.uid; });
                    if (found == owner->m_replays.end()) continue;
                    auto* item = button(owner->m_buttonMenu, owner, menu_selector(MatchDetailPopup::onReplay),
                                        fmt::format("Replay {}%", found->percent), {side ? 360.f : 60.f, 21.f}, .4f,
                                        "GJ_button_02.png");
                    item->setTag(side);
                }
            });
    }
    void onReplay(CCObject* sender) {
        auto const& who = sender->getTag() ? m_match.opponent : m_match.self;
        replay::watch({m_match.id, who.uid, who.name});
    }
    // 현재 페이지의 시도별 기록을 요청해 표로 그린다. 응답이 늦게 와도 최신 요청(generation)만 반영한다.
    // 너무 빠른 기록(a.fast)은 빨간색으로 표시하고 아래에 이유 문구를 붙인다.
    void fetch() {
        m_rows->removeAllChildrenWithCleanup(true);
        int count = std::max(m_match.selfStats.runNumber, m_match.opponentStats.runNumber);
        int pages = std::max(1, (count + 9) / 10);
        m_page = std::clamp(m_page, 0, pages - 1);
        m_pageLabel->setString(fmt::format("{} / {}", m_page + 1, pages).c_str());
        enabled(m_previous, m_match.detailed && m_page > 0);
        enabled(m_next, m_match.detailed && m_page + 1 < pages);
        if (!m_match.detailed) return;
        label(m_rows, "Loading attempts...", {210.f, 92.f}, .5f, 360.f, kIce, false, "chatFont.fnt");
        int generation = ++m_generation;
        Service::get().fetchAttempts(
            m_match, m_page * 10 + 1,
            [self = Weak<MatchDetailPopup>(this), generation](std::vector<AttemptRecord> own,
                                                              std::vector<AttemptRecord> other, std::string detail) {
                auto owner = self.lock();
                if (!owner || owner->m_generation != generation) return;
                owner->m_rows->removeAllChildrenWithCleanup(true);
                if (!detail.empty()) {
                    label(owner->m_rows, "Records unavailable. Reopen to retry.", {210.f, 92.f}, .48f, 365.f, kIce,
                          false, "chatFont.fnt");
                    return;
                }
                int flaggedLength = -1;
                auto cell = [&flaggedLength](CCNode* bg, auto const& list, int run, float x) {
                    for (auto const& a : list)
                        if (a.run == run) {
                            auto text = fmt::format("{}%", a.percent);
                            if (a.ms >= 0) text += "   " + runTimeText(a.ms);
                            auto const color = a.fast ? ccc3(255, 96, 84) : a.outcome == "end" ? kMuted : ccWHITE;
                            if (a.fast) flaggedLength = a.length;
                            label(bg, text, {x, 5.f}, .46f, 118.f, color, false, "chatFont.fnt");
                            return;
                        }
                    label(bg, "--", {x, 5.f}, .46f, 118.f, ccWHITE, false, "chatFont.fnt");
                };
                int maxRun = std::max(owner->m_match.selfStats.runNumber, owner->m_match.opponentStats.runNumber);
                for (int row = 0; row < 10; ++row) {
                    int run = owner->m_page * 10 + row + 1;
                    if (run > maxRun) break;
                    float y = 135.f - row * 10.5f;
                    auto* bg = panel(owner->m_rows, {20.f, y - 5.f}, {380.f, 10.f}, false);
                    label(bg, fmt::format("Attempt {}", run), {7.f, 5.f}, .42f, 95.f, kMuted, true, "chatFont.fnt");
                    cell(bg, own, run, 140.f);
                    cell(bg, other, run, 318.f);
                }
                if (flaggedLength >= 0) {
                    label(owner->m_rows,
                          fmt::format("Red time: faster than a{} {} map allows", flaggedLength == 4 ? "n" : "",
                                      lengthName(flaggedLength)),
                          {210.f, 7.f}, .4f, 390.f, ccc3(255, 120, 105), false, "chatFont.fnt");
                }
            });
    }
    void onPrevious(CCObject*) {
        if (m_page > 0) {
            --m_page;
            fetch();
        }
    }
    void onNext(CCObject*) {
        ++m_page;
        fetch();
    }
    void onName(CCObject* sender) { showPlayer(sender->getTag() ? m_match.opponent : m_match.self); }
    void onCopy(CCObject*) {
        clipboard::write(fmt::format("{}", m_match.levelId));
        Notification::create("Level ID copied", NotificationIcon::Success)->show();
    }
    // 레벨 정보 화면(LevelInfoLayer)으로 이동해 맵을 본다. 로비 씬은 먼저 해제한다.
    void onView(CCObject*) {
        if (m_match.levelId <= 0 || m_match.levelId > std::numeric_limits<int>::max()) return;
        auto* level = GJGameLevel::create();
        level->m_levelID = static_cast<int>(m_match.levelId);
        level->m_levelName = m_match.levelName;
        level->m_levelType = GJLevelType::Saved;
        lounge::release();
        auto* scene = LevelInfoLayer::scene(level, false);
        if (scene) CCDirector::sharedDirector()->pushScene(CCTransitionFade::create(.2f, scene));
    }

public:
    static MatchDetailPopup* create(MatchRecord match, std::function<void(int64_t, int)> onVoted = nullptr) {
        auto* result = new MatchDetailPopup;
        if (result->init(std::move(match), std::move(onVoted))) {
            result->autorelease();
            return result;
        }
        delete result;
        return nullptr;
    }
};

// 랭크 경기 취소 확인 팝업 (관리 기능). 사유 입력을 필수로 받고, 취소 시 상대는 잃은 레이팅을
// 돌려받고 해당 플레이어는 얻은 레이팅을 잃는다는 안내를 보여 준다.
class CancelMatchesPopup final : public ::versus::DeferredPopup {
    TextInput* m_reason = nullptr;
    std::function<void(std::string)> m_confirm;
    void onExit() override {
        if (m_reason) m_reason->defocus();
        Popup::onExit();
    }
    void onBack(CCObject*) { onClose(nullptr); }
    void onConfirm(CCObject*) {
        std::string reason = m_reason->getString();
        auto const first = reason.find_first_not_of(' ');
        if (first == std::string::npos) {
            error("Enter a reason. The refunded players will see it.");
            return;
        }
        m_reason->defocus();
        auto confirm = std::move(m_confirm);
        onClose(nullptr);
        if (confirm) confirm(std::move(reason));
    }
    bool init(std::string const& name, int count, std::function<void(std::string)> confirm) {
        if (!Popup::init(340.f, 210.f, "GJ_square02.png")) return false;
        m_confirm = std::move(confirm);
        setTitle("Cancel Matches", "bigFont.fnt", .6f);
        label(m_mainLayer, fmt::format("Cancel {} Ranked match{} of {}?", count, count == 1 ? "" : "es", name),
              {170.f, 166.f}, .34f, 310.f, ccWHITE, false, "bigFont.fnt");
        label(m_mainLayer, "Each opponent gets back the rating it lost there;", {170.f, 146.f}, .5f, 310.f, kIce, false,
              "chatFont.fnt");
        label(m_mainLayer, "this player loses what it gained.", {170.f, 133.f}, .5f, 310.f, kIce, false,
              "chatFont.fnt");
        label(m_mainLayer, "Reason (shown to the refunded players)", {170.f, 112.f}, .3f, 300.f, kMuted, false,
              "bigFont.fnt");
        m_reason = TextInput::create(300.f / .7f, "Reason");
        m_reason->setScale(.7f);
        m_reason->setMaxCharCount(160);
        m_reason->setCommonFilter(CommonFilter::Any);
        m_reason->setPosition({170.f, 90.f});
        m_reason->setString(fmt::format("Your opponent {} broke the rules, so the match was canceled.", name).c_str());
        styleInput(m_reason);
        m_mainLayer->addChild(m_reason);
        button(m_buttonMenu, this, menu_selector(CancelMatchesPopup::onBack), "Back", {100.f, 32.f}, .6f,
               "GJ_button_04.png");
        button(m_buttonMenu, this, menu_selector(CancelMatchesPopup::onConfirm), "Cancel Matches", {226.f, 32.f}, .6f,
               "GJ_button_06.png")
            ->setID("cancel-matches-confirm"_spr);
        return true;
    }

public:
    static CancelMatchesPopup* create(std::string const& name, int count, std::function<void(std::string)> confirm) {
        auto* result = new CancelMatchesPopup;
        if (result->init(name, count, std::move(confirm))) {
            result->autorelease();
            return result;
        }
        delete result;
        return nullptr;
    }
};

// 플레이어의 최근 경기(전적) 목록 팝업. 5건씩 페이지로 보여 주고 행을 누르면 MatchDetailPopup,
// 상대 아바타를 누르면 플레이어 정보를 연다. 관리자가 다른 플레이어의 기록을 볼 때는
// Ranked 경기를 골라 취소할 수 있는 선택 모드가 추가된다.
class HistoryPopup : public ::versus::DeferredPopup {
    CCNode* m_rows = nullptr;
    CCLabelBMFont* m_status = nullptr;
    CCLabelBMFont* m_pageLabel = nullptr;
    CCMenuItemSpriteExtra* m_previous = nullptr;
    CCMenuItemSpriteExtra* m_next = nullptr;
    CCMenuItemSpriteExtra* m_previousFast = nullptr;
    CCMenuItemSpriteExtra* m_nextFast = nullptr;
    std::vector<MatchRecord> m_matches;
    int m_page = 0;
    PlayerProfile m_owner;
    bool m_selecting = false, m_canceling = false;
    std::set<std::string> m_selected;
    CCMenuItemSpriteExtra* m_selectButton = nullptr;
    CCMenuItemSpriteExtra* m_pageButton = nullptr;
    CCMenuItemSpriteExtra* m_cancelButton = nullptr;
    // 내가 이 맵들에 남긴 평가 (levelId -> 좋아요 1 / 싫어요 -1 / 없음 0).
    std::map<int64_t, int> m_myVotes;
    // 지금 보는 기록이 내 것인지 여부.
    bool mine() const { return !m_owner.uid.empty() && m_owner.uid == Service::get().profile().uid; }
    // 현재 페이지의 Ranked 경기 중 아직 평가를 모르는 맵만 모아 서버에 한꺼번에 물어보고 결과를 표시에 반영한다.
    void askVotes() {
        if (!mine() || !mapvotes::shown()) return;
        std::vector<int64_t> ids;
        for (int row = 0; row < kHistoryPageSize; ++row) {
            size_t const index = static_cast<size_t>(m_page * kHistoryPageSize + row);
            if (index >= m_matches.size()) break;
            int64_t const id = m_matches[index].levelId;
            if (!m_matches[index].ranked) continue;
            if (id > 0 && !m_myVotes.count(id) && std::find(ids.begin(), ids.end(), id) == ids.end()) ids.push_back(id);
        }
        if (ids.empty()) return;
        Service::get().fetchMapVotes(
            ids, [self = Weak<HistoryPopup>(this), ids](std::map<int64_t, MapVotes> votes, std::string error) {
                auto owner = self.lock();
                if (!owner || !error.empty()) return;
                for (auto id : ids) {
                    auto found = votes.find(id);
                    owner->m_myVotes[id] = found != votes.end() ? found->second.mine : 0;
                }
                owner->render();
            });
    }
    // 취소할 수 있는 경기: 끝난 Ranked 경기이고 아직 취소되지 않았으며 방 ID가 있어야 한다.
    static bool cancelable(MatchRecord const& match) {
        return match.ranked && !match.canceled && !match.roomId.empty();
    }
    bool init(PlayerProfile const& profile) {
        if (!Popup::init(390.f, 270.f, "GJ_square02.png")) return false;
        m_owner = profile;
        geode::addSideArt(m_mainLayer, SideArt::All, SideArtStyle::PopupBlue);
        setTitle("Recent Matches", "bigFont.fnt", .65f);
        bool const admin =
            Service::get().isAdmin() && !profile.uid.empty() && profile.uid != Service::get().profile().uid;
        styleNickname(label(m_mainLayer, fmt::format("{} - LAST {} GAMES (7 DAYS)", profile.name, historyLimit()),
                            {admin ? 170.f : 195.f, 224.f}, .3f, admin ? 270.f : 335.f, kIce),
                      profile.membership, profile.cosmetics);
        if (admin) {
            m_selectButton = button(m_buttonMenu, this, menu_selector(HistoryPopup::onSelect), "Select", {345.f, 224.f},
                                    .4f, "GJ_button_04.png");
            m_selectButton->setID("history-select"_spr);
            m_pageButton = button(m_buttonMenu, this, menu_selector(HistoryPopup::onSelectPage), "Page", {42.f, 29.f},
                                  .4f, "GJ_button_04.png");
            m_cancelButton = button(m_buttonMenu, this, menu_selector(HistoryPopup::onCancelMatches), "Cancel",
                                    {348.f, 29.f}, .4f, "GJ_button_06.png");
            m_cancelButton->setID("history-cancel-matches"_spr);
            m_pageButton->setVisible(false);
            m_cancelButton->setVisible(false);
        }
        m_rows = CCNode::create();
        m_mainLayer->addChild(m_rows);
        m_status = label(m_mainLayer, "Loading history...", {195.f, 134.f}, .45f, 330.f, kIce);
        m_pageLabel = label(m_mainLayer, "1 / 1", {195.f, 29.f}, .33f, 90.f, kIce);
        m_previous = button(m_buttonMenu, this, menu_selector(HistoryPopup::onPrevious), "<", {139.f, 29.f}, .45f);
        m_next = button(m_buttonMenu, this, menu_selector(HistoryPopup::onNext), ">", {251.f, 29.f}, .45f);
        m_previousFast =
            button(m_buttonMenu, this, menu_selector(HistoryPopup::onPreviousFast), "<<", {101.f, 29.f}, .45f);
        m_nextFast = button(m_buttonMenu, this, menu_selector(HistoryPopup::onNextFast), ">>", {289.f, 29.f}, .45f);
        enabled(m_previous, false);
        enabled(m_next, false);
        enabled(m_previousFast, false);
        enabled(m_nextFast, false);
        load();
        return true;
    }
    // 서버에서 최근 경기 기록을 받아 historyLimit()개까지만 보관하고 그린다.
    void load() {
        Service::get().fetchPlayerHistory(
            m_owner.uid, [self = Weak<HistoryPopup>(this)](std::vector<MatchRecord> rows, std::string detail) {
                auto owner = self.lock();
                if (!owner) return;
                if (!detail.empty()) {
                    owner->m_status->setString("History unavailable");
                    error(detail);
                    return;
                }
                owner->m_matches = std::move(rows);
                if (owner->m_matches.size() > historyLimit()) owner->m_matches.resize(historyLimit());
                owner->render();
            });
    }
    // 선택 모드 버튼 상태 갱신 (Select/Done, 선택 개수가 표시되는 Cancel 버튼).
    void refreshSelectControls() {
        if (!m_selectButton) return;
        auto* sprite = static_cast<ButtonSprite*>(m_selectButton->getNormalImage());
        sprite->setString(m_selecting ? "Done" : "Select");
        m_selectButton->updateSprite();
        m_pageButton->setVisible(m_selecting);
        m_cancelButton->setVisible(m_selecting);
        auto* cancel = static_cast<ButtonSprite*>(m_cancelButton->getNormalImage());
        cancel->setString(fmt::format("Cancel ({})", m_selected.size()).c_str());
        m_cancelButton->updateSprite();
        enabled(m_cancelButton, !m_selected.empty() && !m_canceling);
    }
    void onSelect(CCObject*) {
        if (m_canceling) return;
        m_selecting = !m_selecting;
        if (!m_selecting) m_selected.clear();
        refreshSelectControls();
        render();
    }
    // 현재 페이지의 취소 가능한 경기를 모두 선택하고, 이미 모두 선택되어 있으면 모두 해제한다.
    void onSelectPage(CCObject*) {
        std::vector<std::string> rooms;
        for (int row = 0; row < kHistoryPageSize; ++row) {
            size_t const index = static_cast<size_t>(m_page * kHistoryPageSize + row);
            if (index < m_matches.size() && cancelable(m_matches[index])) rooms.push_back(m_matches[index].roomId);
        }
        bool const all = !rooms.empty() && std::all_of(rooms.begin(), rooms.end(), [this](std::string const& room) {
            return m_selected.contains(room);
        });
        for (auto const& room : rooms) {
            if (all)
                m_selected.erase(room);
            else
                m_selected.insert(room);
        }
        refreshSelectControls();
        render();
    }
    void toggle(MatchRecord const& match) {
        if (!cancelable(match)) {
            Notification::create(match.canceled ? "Already canceled" : "Only Ranked matches can be canceled",
                                 NotificationIcon::Info, 1.5f)
                ->show();
            return;
        }
        if (!m_selected.erase(match.roomId)) m_selected.insert(match.roomId);
        refreshSelectControls();
        render();
    }
    // 선택한 Ranked 경기들의 취소를 요청한다. 사유 입력 팝업을 거쳐 서버에 보내고, 결과(취소 수,
    // 환급받은 상대와 레이팅, 회수된 레이팅)를 알림창으로 요약한다. 알림 태그와 겹치지 않게 이름의 '<' '>'는 바꾼다.
    void onCancelMatches(CCObject*) {
        if (m_selected.empty() || m_canceling) return;
        std::vector<std::string> rooms(m_selected.begin(), m_selected.end());
        auto* popup = CancelMatchesPopup::create(
            m_owner.name, static_cast<int>(rooms.size()), [self = Weak<HistoryPopup>(this), rooms](std::string reason) {
                auto owner = self.lock();
                if (!owner) return;
                owner->m_canceling = true;
                owner->refreshSelectControls();
                Service::get().cancelRankedMatches(
                    owner->m_owner.uid, rooms, std::move(reason),
                    [self](std::optional<MatchCancelResult> result, std::string detail) {
                        auto owner = self.lock();
                        if (!owner) return;
                        owner->m_canceling = false;
                        if (!result) {
                            owner->refreshSelectControls();
                            error(detail.empty() ? "Canceling failed." : detail);
                            return;
                        }
                        owner->m_selected.clear();
                        owner->m_selecting = false;
                        owner->refreshSelectControls();
                        owner->load();
                        auto plain = [](std::string text) {
                            for (auto& c : text)
                                if (c == '<' || c == '>') c = c == '<' ? '(' : ')';
                            return text;
                        };
                        double total = 0.;
                        std::string lines;
                        for (size_t i = 0; i < result->refunds.size(); ++i) {
                            total += result->refunds[i].second;
                            if (i < 6)
                                lines += fmt::format("\n<cy>{}</c>  <cg>+{:.1f}</c>", plain(result->refunds[i].first),
                                                     result->refunds[i].second);
                        }
                        if (result->refunds.size() > 6) lines += fmt::format("\n+{} more", result->refunds.size() - 6);
                        auto body = fmt::format(
                            "Canceled <cy>{}</c> match{}{}.\nRefunded {} player{} (<cg>+{:.1f}</c> total).{}",
                            result->canceled, result->canceled == 1 ? "" : "es",
                            result->skipped
                                ? fmt::format(
                                      " ({} skipped: not a finished Ranked match of this player, or already canceled)",
                                      result->skipped)
                                : std::string(),
                            result->refunds.size(), result->refunds.size() == 1 ? "" : "s", total, lines);
                        if (result->removed > 0.)
                            body += fmt::format("\n\nRemoved <cr>{:.1f}</c> rating from {}.", result->removed,
                                                plain(owner->m_owner.name));
                        FLAlertLayer::create(nullptr, "Matches Canceled", body, "OK", nullptr, 360.f)->show();
                    });
            });
        if (popup) popup->show();
    }
    // 현재 페이지 행들을 그린다. Ranked는 갈색 배경, 선택 모드에서는 선택된 행을 초록색, 취소 불가 행을 흐리게
    // 표시한다. 그린 뒤에 이 페이지의 맵 평가를 서버에 확인한다.
    void render() {
        m_rows->removeAllChildrenWithCleanup(true);
        int const pages = std::max(1, (static_cast<int>(m_matches.size()) + kHistoryPageSize - 1) / kHistoryPageSize);
        m_page = std::clamp(m_page, 0, pages - 1);
        m_pageLabel->setString(fmt::format("{} / {}", m_page + 1, pages).c_str());
        enabled(m_previous, m_page > 0);
        enabled(m_next, m_page + 1 < pages);
        enabled(m_previousFast, m_page > 0);
        enabled(m_nextFast, m_page + 1 < pages);
        m_status->setVisible(m_matches.empty());
        m_status->setString("No matches in the last 7 days");
        auto* rowsMenu = menu(m_rows);
        for (int row = 0; row < kHistoryPageSize; ++row) {
            size_t const index = static_cast<size_t>(m_page * kHistoryPageSize + row);
            if (index >= m_matches.size()) break;
            auto const& match = m_matches[index];
            auto* cell = CCSprite::create();
            cell->setContentSize({352.f, 29.f});
            auto* back = panel(cell, {0.f, 0.f}, {352.f, 29.f}, false);
            if (match.ranked)
                if (auto* slice = back->getChildByType<NineSlice*>(0)) {
                    slice->setColor(ccc3(88, 52, 24));
                    slice->setOpacity(200);
                }
            if (match.detailed && !match.opponent.uid.empty()) {
                auto* face = CCNode::create();
                face->setContentSize({30.f, 29.f});
                auto* who = CCMenuItemSpriteExtra::create(face, this, menu_selector(HistoryPopup::onOpponent));
                who->setTag(static_cast<int>(index));
                who->setPosition({195.f - 176.f + 219.f, 192.5f - row * 32.f});
                rowsMenu->addChild(who);
            }
            auto* item = CCMenuItemSpriteExtra::create(cell, this, menu_selector(HistoryPopup::onDetails));
            item->setPosition({195.f, 192.5f - row * 32.f});
            item->setTag(static_cast<int>(index));
            rowsMenu->addChild(item);
            if (m_selecting) {
                bool const picked = m_selected.contains(match.roomId);
                if (auto* slice = back->getChildByType<NineSlice*>(0)) {
                    if (picked) {
                        slice->setColor(ccc3(40, 120, 70));
                        slice->setOpacity(230);
                    } else if (!cancelable(match))
                        slice->setOpacity(90);
                }
                if (picked)
                    if (auto* check = CCSprite::createWithSpriteFrameName("GJ_completesIcon_001.png")) {
                        check->setScale(13.f / std::max(1.f, check->getContentSize().height));
                        check->setPosition({343.f, 22.f});
                        cell->addChild(check, 5);
                    }
            }
            auto result = match.result;
            std::transform(result.begin(), result.end(), result.begin(),
                           [](unsigned char c) { return std::toupper(c); });
            auto const resultColor = result == "WIN"    ? ccc3(130, 255, 165)
                                     : result == "LOSS" ? ccc3(255, 160, 160)
                                                        : kIce;
            label(cell, result.empty() ? "PLAYED" : result, {8.f, 20.f}, .29f, 49.f, resultColor, true);
            if (match.playedAt > 0)
                label(cell, playedText(match.playedAt), {8.f, 8.f}, .4f, 52.f, kMuted, true, "chatFont.fnt");
            styleNickname(label(cell, "vs " + match.opponentName, {64.f, 20.f}, .32f, 134.f, {255, 255, 255}, true),
                          match.detailed && match.opponent.membership, match.opponent.cosmetics);
            if (match.detailed) player(cell, match.opponent, {219.f, 15.f}, .5f);
            label(cell, match.levelName, {64.f, 8.f}, .42f, 165.f, kIce, true, "chatFont.fnt");
            auto const kind = matchKindTag(match);
            label(cell, kind.first, {287.f, 21.f}, .24f, 100.f, kind.second);
            label(cell, "Winner", {239.f, 9.f}, .3f, 30.f, kMuted, true, "chatFont.fnt");
            styleNickname(
                label(cell, match.winnerName.empty() ? "Draw" : match.winnerName, {270.f, 9.f}, .25f, 76.f, ccWHITE,
                      true),
                historyWinnerMembership(match.result, match.detailed, match.self.membership, match.opponent.membership),
                (match.result == "win" ? match.self : match.opponent).cosmetics);
            if (!m_selecting && match.ranked)
                if (auto found = m_myVotes.find(match.levelId); found != m_myVotes.end() && found->second != 0)
                    if (auto* mark = mapvotes::icon(
                            found->second > 0 ? "GJ_likesIcon_001.png" : "GJ_dislikesIcon_001.png", 9.f)) {
                        mark->setPosition({344.f, 23.f});
                        cell->addChild(mark, 5);
                    }
        }
        askVotes();
    }
    // 상대 아바타 클릭: 선택 모드면 행 선택을 토글하고, 아니면 상대 프로필을 연다.
    void onOpponent(CCObject* sender) {
        int index = sender->getTag();
        if (index < 0 || index >= static_cast<int>(m_matches.size())) return;
        if (m_selecting) {
            toggle(m_matches[index]);
            return;
        }
        showPlayer(m_matches[index].opponent);
    }
    // 행 클릭: 선택 모드면 행 선택을 토글하고, 아니면 경기 상세 팝업을 연다 (평가가 바뀌면 목록에 반영).
    void onDetails(CCObject* sender) {
        int index = sender->getTag();
        if (index < 0 || index >= static_cast<int>(m_matches.size())) return;
        if (m_selecting) {
            toggle(m_matches[index]);
            return;
        }
        if (auto *popup =
                MatchDetailPopup::create(m_matches[index], [self = Weak<HistoryPopup>(this)](int64_t level, int vote) {
                    if (auto owner = self.lock()) {
                        owner->m_myVotes[level] = vote;
                        owner->render();
                    }
                }))
            popup->show();
    }
    // 취소 요청 중에는 닫지 못하게 한다.
    void onClose(CCObject* sender) override {
        if (!m_canceling) Popup::onClose(sender);
    }
    void onPrevious(CCObject*) {
        if (m_page > 0) {
            --m_page;
            render();
        }
    }
    void onNext(CCObject*) {
        ++m_page;
        render();
    }
    // 보관할 기록 개수와 빠른 이동 페이지 수 (관리자는 더 많이 본다).
    static size_t historyLimit() { return Service::get().isAdmin() ? kAdminHistoryLimit : kHistoryLimit; }
    static int fastStep() { return Service::get().isAdmin() ? 10 : 5; }
    void onPreviousFast(CCObject*) {
        m_page = std::max(0, m_page - fastStep());
        render();
    }
    void onNextFast(CCObject*) {
        m_page += fastStep();
        render();
    }

public:
    static HistoryPopup* create(PlayerProfile const& profile = Service::get().profile()) {
        auto* result = new HistoryPopup;
        if (result->init(profile)) {
            result->autorelease();
            return result;
        }
        delete result;
        return nullptr;
    }
};

// 시즌/랭크 UI 스타일, 랭크 아트, 관리(모더레이션) 팝업 (인라인 include).
#include "Season2Style.inl"
#include "RankedArt.inl"
#include "Moderation.inl"

// 프로필에서 고를 수 있는 시즌 항목 (cup: 빈 값=Season 1 전체, 그 외 Season 2 컵 이름).
struct ProfileSeason {
    std::string cup;
    std::string season, name;
};
// Season 2 접근 권한이 있으면 Season 2 STANDARD/DEMON 컵 항목을 목록에 추가한다.
std::vector<ProfileSeason> profileSeasons() {
    std::vector<ProfileSeason> seasons{{"", "S1", "ALL"}};
    if (Service::get().season2Access()) {
        seasons.push_back({"standard", "S2", "STANDARD"});
        seasons.push_back({"demon", "S2", "DEMON"});
    }
    return seasons;
}
// 프로필 팝업의 시즌 선택기. 항목이 둘 이상이면 좌우 화살표로 순환하며 바뀔 때 콜백을 호출한다.
class SeasonPicker final : public CCNode {
    std::vector<ProfileSeason> m_seasons;
    size_t m_index = 0;
    std::function<void(ProfileSeason const&)> m_changed;
    CCLabelBMFont* m_season = nullptr;
    CCLabelBMFont* m_name = nullptr;
    float m_width = 0.f;
    static constexpr float kSeasonScale = .3f, kNameScale = .22f, kLine = 5.f, kArrowH = 16.f, kArrowGap = 8.f;
    void show() {
        auto const& now = m_seasons[m_index];
        bool const two = !now.cup.empty();
        m_season->setString(now.season.c_str());
        m_season->setScale(kSeasonScale);
        m_season->limitLabelWidth(m_width, kSeasonScale, .1f);
        m_season->setColor(two ? ccc3(255, 205, 140) : ccc3(170, 225, 255));
        m_name->setString(now.name.c_str());
        m_name->setScale(kNameScale);
        m_name->limitLabelWidth(m_width, kNameScale, .1f);
        m_name->setColor(two ? ccc3(255, 175, 110) : kIce);
    }
    void step(int by) {
        if (m_seasons.size() < 2) return;
        m_index = (m_index + m_seasons.size() + by) % m_seasons.size();
        show();
        if (m_changed) m_changed(m_seasons[m_index]);
    }
    void onLeft(CCObject*) { step(-1); }
    void onRight(CCObject*) { step(1); }
    bool init(CCPoint at, std::vector<ProfileSeason> seasons, size_t index, float width,
              std::function<void(ProfileSeason const&)> changed) {
        if (!CCNode::init() || seasons.empty()) return false;
        m_seasons = std::move(seasons);
        m_index = std::min(index, m_seasons.size() - 1);
        m_changed = std::move(changed);
        m_width = width;
        setPosition(at);
        setID("profile-season-picker"_spr);
        m_season = label(this, "", {0.f, kLine}, kSeasonScale, width, ccWHITE, false, "bigFont.fnt");
        m_name = label(this, "", {0.f, -kLine}, kNameScale, width, kIce, false, "bigFont.fnt");
        if (m_seasons.size() > 1) {
            auto* arrows = menu(this);
            for (int side = -1; side <= 1; side += 2) {
                auto* arrow = loadedFrameSprite("navArrowBtn_001.png", "GJ_GameSheet03.plist");
                if (!arrow) break;
                arrow->setScale(kArrowH / std::max(1.f, arrow->getContentSize().height));
                arrow->setFlipX(side < 0);
                auto* hit = CCNode::create();
                hit->setContentSize({24.f, 26.f});
                arrow->setPosition({12.f, 13.f});
                hit->addChild(arrow);
                auto* item = CCMenuItemSpriteExtra::create(
                    hit, this, side < 0 ? menu_selector(SeasonPicker::onLeft) : menu_selector(SeasonPicker::onRight));
                item->setPosition({side * (width / 2.f + kArrowGap), 0.f});
                item->setID(side < 0 ? "profile-season-left"_spr : "profile-season-right"_spr);
                arrows->addChild(item);
            }
        }
        show();
        return true;
    }

public:
    static SeasonPicker* create(CCPoint at, std::vector<ProfileSeason> seasons, size_t index, float width,
                                std::function<void(ProfileSeason const&)> changed) {
        auto* node = new SeasonPicker;
        if (node->init(at, std::move(seasons), index, width, std::move(changed))) {
            node->autorelease();
            return node;
        }
        delete node;
        return nullptr;
    }
    size_t count() const { return m_seasons.size(); }
};
// 시즌에 맞는 티어 얼굴 아이콘 (Season 1 또는 Season 2).
CCSprite* profileTierFace(CCNode* parent, std::string const& cup, std::string const& tier, CCPoint at, float scale) {
    return cup.empty() ? season1Face(parent, tier, at, scale) : season2Face(parent, tier, at, scale);
}
// 티어 이름 문구. 배치 전이면 "PLACEMENT".
std::string profileTierText(std::string const& cup, RankedProfile const& rank) {
    if (!rank.placed) return "PLACEMENT";
    return cup.empty() || rank.tierName.empty() ? rank.tier : season2TierName(rank.tierName);
}
// 티어에 해당하는 그라데이션 팔레트 키.
std::string profileTierColours(std::string const& cup, std::string const& tier) {
    return cup.empty() ? season1PaletteKey(tier) : season2PaletteKey(tier);
}

// 플레이어 정보 팝업. 아이콘/닉네임/칭호, 시즌별 랭크(S1, S2 컵)와 밴 상태를 보여 주고
// 최근 경기, GD 프로필, 맵 평가, 신고 버튼을 연다. 관리자는 밴/신고 밴/Ranked Admin 도구도 쓴다.
class PlayerPopup : public ::versus::DeferredPopup {
    PlayerProfile m_profile;
    CCNode* m_identity = nullptr;
    CCLabelBMFont* m_rankText = nullptr;
    CCLabelBMFont* m_rankRating = nullptr;
    CCSprite* m_rankFace = nullptr;
    CCLabelBMFont* m_winLine = nullptr;
    Season2TierText* m_s2Tier = nullptr;
    std::string m_cup;
    // 컵별로 받아 둔 랭크 정보 (키가 빈 문자열이면 Season 1).
    std::map<std::string, RankedProfile> m_ranks;
    // 요청 중인 컵 (중복 요청 방지).
    std::set<std::string> m_loading;
    std::optional<RankedProfile> m_ban;
    // 해당 컵의 랭크 정보를 요청한다. 실패하면 현재 보는 컵이 그것일 때 "UNAVAILABLE"을 표시한다.
    // Season 1 정보에 밴이 있으면 컵과 상관없이 밴 표시로 바꾼다.
    void load(std::string const& cup) {
        if (m_ranks.count(cup) || !m_loading.insert(cup).second) return;
        Service::get().fetchRankedProfileIn(
            cup, m_profile.uid, [self = Weak<PlayerPopup>(this), cup](std::optional<RankedProfile> rank, std::string) {
                auto owner = self.lock();
                if (!owner) return;
                owner->m_loading.erase(cup);
                if (!rank) {
                    if (owner->m_cup == cup && !owner->m_ban) {
                        rankedGradientOff(owner->m_rankText);
                        owner->m_rankText->setString("UNAVAILABLE");
                        owner->m_rankText->setColor(kMuted);
                        owner->m_rankText->limitLabelWidth(kRankTextWidth, .46f, .28f);
                    }
                    return;
                }
                owner->m_ranks[cup] = *rank;
                bool const ban = cup.empty() && rank->banned;
                if (ban) owner->m_ban = *rank;
                if (owner->m_cup == cup || ban) owner->showSeason(owner->m_cup);
            });
    }
    // 선택한 시즌의 정보를 표시한다. 아직 없으면 "LOADING"을 보이고 요청을 시작한다.
    void showSeason(std::string const& cup) {
        m_cup = cup;
        if (auto found = m_ranks.find(cup); found != m_ranks.end()) {
            showRank(found->second);
            return;
        }
        if (m_ban) {
            showBan(*m_ban);
            return;
        }
        m_s2Tier->setVisible(false);
        m_rankText->setVisible(true);
        rankedGradientOff(m_rankText);
        rankedGradientOff(m_rankRating);
        m_rankText->setString("LOADING");
        m_rankText->setColor(kMuted);
        m_rankText->limitLabelWidth(kRankTextWidth, .46f, .28f);
        m_rankRating->setString("");
        load(cup);
    }
    // 랭크 정보를 그린다 (티어 얼굴, 티어명, 레이팅 또는 배치 진행도 N/필요 판수).
    // 밴 상태면 showBan으로 넘긴다.
    void showRank(RankedProfile const& rank) {
        m_identity->removeFromParent();
        m_identity = rankedNickname(m_mainLayer, m_profile, rank.globalRank, {150.f, 186.f}, .5f, 250.f);
        m_rankText->setColor(ccWHITE);
        m_rankRating->setColor(ccWHITE);
        m_winLine->setString("");
        if (rank.banned || m_ban) {
            showBan(rank.banned ? rank : *m_ban);
            return;
        }
        m_banned = false;
        m_rankFace->removeFromParent();
        m_rankFace = profileTierFace(m_mainLayer, m_cup, rank.tier, {48.f, 126.f}, .7f);
        bool const two = !m_cup.empty();
        m_rankText->setVisible(!two);
        m_s2Tier->setVisible(two);
        if (two)
            m_s2Tier->set(rank.placed && !rank.tierName.empty() ? rank.tierName : std::string("Placement"),
                          rank.placed ? rank.tier : std::string("Unranked"), .46f, kRankTextWidth, false);
        m_rankText->setString(profileTierText(m_cup, rank).c_str());
        m_rankRating->setString((rank.placed ? fmt::format("R{:.0f}", rank.rating)
                                             : fmt::format("{} / {}", rank.placementCompleted,
                                                           rank.placementRequired > 0 ? rank.placementRequired : 5))
                                    .c_str());
        m_rankText->limitLabelWidth(kRankTextWidth, .46f, .28f);
        m_rankRating->limitLabelWidth(kRankTextWidth, .43f, .3f);
        rankedGradient(m_rankText, profileTierColours(m_cup, rank.tier));
        rankedGradient(m_rankRating, profileTierColours(m_cup, rank.tier));
    }
    static constexpr float kRankTextX = 183.f, kRankTextWidth = 191.f;
    static constexpr float kReasonX = 145.f, kReasonWidth = 120.f, kRowY = 103.f, kPickerX = 246.f, kPickerW = 48.f;
    // 초기 UI 구성. 처음에 보여 줄 시즌은 Season 2 컵 중 현재 랭크 컵 쪽을 우선한다.
    bool init(PlayerProfile profile) {
        if (!Popup::init(300.f, 270.f, "GJ_square02.png")) return false;
        m_profile = std::move(profile);
        setTitle("Player", "bigFont.fnt", .6f);
        rankedPanel(m_mainLayer, {9.f, 157.f}, {282.f, 104.f}, ccc3(15, 22, 48), 160)->setZOrder(0);
        rankedPanel(m_mainLayer, {9.f, 93.f}, {282.f, 58.f}, ccc3(15, 22, 48), 160)->setZOrder(0);
        m_title->setZOrder(2);
        player(m_mainLayer, m_profile, {150.f, 216.f}, 1.05f);
        m_identity = rankedNickname(m_mainLayer, m_profile, 0, {150.f, 186.f}, .5f, 250.f);
        titleBadge(m_mainLayer, m_profile.cosmetics.title, {150.f, 167.f}, 1.1f, 240.f);
        m_rankFace = rankedFace(m_mainLayer, "Unranked", {48.f, 126.f}, .7f);
        m_rankText =
            label(m_mainLayer, "RANK", {kRankTextX, 139.f}, .46f, kRankTextWidth, ccWHITE, false, "bigFont.fnt");
        m_s2Tier = Season2TierText::create();
        m_s2Tier->setPosition({kRankTextX, 139.f});
        m_s2Tier->setVisible(false);
        m_mainLayer->addChild(m_s2Tier, 2);
        m_rankRating =
            label(m_mainLayer, "R----", {kRankTextX, 120.f}, .43f, kRankTextWidth, ccWHITE, false, "bigFont.fnt");
        m_winLine = label(m_mainLayer, "", {kReasonX, kRowY}, .36f, kReasonWidth, kIce, false, "chatFont.fnt");
        auto seasons = profileSeasons();
        size_t first = 0;
        for (size_t i = 0; i < seasons.size(); ++i)
            if (!seasons[i].cup.empty() && (first == 0 || seasons[i].cup == Service::get().rankedCup())) first = i;
        std::string const cup = seasons[first].cup;
        if (auto* picker = SeasonPicker::create({kPickerX, kRowY}, std::move(seasons), first, kPickerW,
                                                [self = Weak<PlayerPopup>(this)](ProfileSeason const& season) {
                                                    if (auto owner = self.lock()) owner->showSeason(season.cup);
                                                }))
            m_mainLayer->addChild(picker, 3);
        showSeason(cup);
        if (!cup.empty()) load({});
        // 다른 사람 프로필인지, 관리자 도구를 쓸 수 있는지에 따라 버튼 배치를 정한다.
        auto const& me = Service::get().profile();
        bool const other = !m_profile.uid.empty() && m_profile.uid != me.uid;
        bool const desk = other && Service::get().isAdmin();
        bool const rate = mapvotes::shown();
        float const left = rate ? (desk ? 35.f : other ? 46.f : 72.f) : other ? 56.f : 82.f;
        float const right = rate ? (desk ? 264.f : other ? 256.f : 228.f) : other ? 244.f : 218.f;
        float const ratings = desk ? 87.f : other ? 115.f : 150.f, captionW = rate && desk ? 50.f : 72.f;
        float const reportX = rate ? 185.f : 150.f;
        auto caption = [this, captionW](char const* text, float x) {
            label(m_mainLayer, text, {x, 28.f}, .26f, captionW, kIce, false, "bigFont.fnt");
        };
        iconButton(m_buttonMenu, this, menu_selector(PlayerPopup::onHistory), "GJ_viewListsBtn_001.png", {left, 60.f},
                   36.f);
        iconButton(m_buttonMenu, this, menu_selector(PlayerPopup::onProfile), "accountBtn_pendingRequest_001.png",
                   {right, 60.f}, 36.f);
        if (rate) {
            auto* ratingsButton = iconButton(m_buttonMenu, this, menu_selector(PlayerPopup::onMapRatings),
                                             "GJ_like2Btn_001.png", {ratings, 60.f}, 36.f);
            ratingsButton->setID("player-map-ratings"_spr);
            caption("Map Ratings", ratings);
        }
        caption("Recent Matches", left);
        caption("GD Profile", right);
        if (desk) {
            auto* report =
                button(m_buttonMenu, this, menu_selector(PlayerPopup::onReport), "Report", {150.f, 80.f}, .45f);
            report->setID("report-player-button"_spr);
            auto* ban = button(m_buttonMenu, this, menu_selector(PlayerPopup::onBan), "Ban", {150.f, 57.f}, .45f,
                               "GJ_button_06.png");
            ban->setID("admin-ban-button"_spr);
            auto* reportBan = button(m_buttonMenu, this, menu_selector(PlayerPopup::onReportBan), "Report Ban",
                                     {150.f, 35.f}, .38f, "GJ_button_04.png");
            reportBan->setID("admin-report-ban-button"_spr);
            auto* ranked = button(m_buttonMenu, this, menu_selector(PlayerPopup::onRankedAdmin), "Ranked Admin",
                                  {150.f, 13.f}, .34f, "GJ_button_02.png");
            ranked->setID("admin-ranked-button"_spr);
            Service::get().fetchBanHistory(
                m_profile.uid, [self = Weak<PlayerPopup>(this)](std::optional<BanHistory> history, std::string) {
                    if (auto owner = self.lock(); owner && history) owner->showBanHistory(std::move(*history));
                });
        } else if (!other && Service::get().isOwner() && !m_profile.uid.empty()) {
            auto* ranked =
                button(m_buttonMenu, this, menu_selector(PlayerPopup::onRankedAdmin), "Ranked Admin",
                       rate ? CCPoint{150.f, 14.f} : CCPoint{150.f, 60.f}, rate ? .34f : .38f, "GJ_button_02.png");
            ranked->setID("admin-ranked-button"_spr);
        } else if (other) {
            auto* report = iconButton(m_buttonMenu, this, menu_selector(PlayerPopup::onReport), "GJ_reportBtn_001.png",
                                      {reportX, 60.f}, 36.f);
            report->setID("report-player-button"_spr);
            caption("Report", reportX);
        }
        return true;
    }
    // 밴 상태 표시: 영구 또는 해제 시각, 사유를 보여 주고 티어 표시는 숨긴다.
    void showBan(RankedProfile const& rank) {
        m_s2Tier->setVisible(false);
        m_rankText->setVisible(true);
        rankedGradientOff(m_rankText);
        rankedGradientOff(m_rankRating);
        m_rankFace->removeFromParent();
        m_rankFace = rankedFace(m_mainLayer, "Unranked", {48.f, 126.f}, .7f);
        m_rankText->setString("BANNED");
        m_rankText->setColor(ccc3(255, 90, 80));
        m_rankText->limitLabelWidth(kRankTextWidth, .46f, .28f);
        m_rankRating->setString(rank.banPermanent || rank.banUntil <= 0
                                    ? "PERMANENT"
                                    : fmt::format("UNTIL {}", localDateTime(rank.banUntil)).c_str());
        m_rankRating->setColor(ccc3(255, 170, 160));
        m_rankRating->limitLabelWidth(kRankTextWidth, .43f, .2f);
        m_winLine->setString(rank.banReason.empty() ? "Not on the Ranked ladder"
                                                    : fmt::format("Reason: {}", rank.banReason).c_str());
        m_winLine->setColor(ccc3(255, 190, 180));
        m_winLine->limitLabelWidth(kReasonWidth, .36f, .2f);
        m_banned = true;
    }
    bool m_banned = false;
    std::optional<BanHistory> m_banHistory;
    // 관리자용 밴 기록을 받아 오면 총 횟수를 보여 주는 버튼(없으면 NO BANS)을 만든다.
    void showBanHistory(BanHistory history) {
        m_banHistory = std::move(history);
        if (m_banHistory->total == 0) {
            label(m_mainLayer, "NO BANS", {262.f, 247.f}, .3f, 60.f, ccc3(130, 230, 160), false, "bigFont.fnt");
            return;
        }
        auto* bans = button(m_buttonMenu, this, menu_selector(PlayerPopup::onBanHistory),
                            fmt::format("BANS {}", m_banHistory->total), {258.f, 247.f}, .34f, "GJ_button_06.png");
        bans->setID("admin-ban-history"_spr);
    }
    void onBanHistory(CCObject*) {
        if (!m_banHistory) return;
        if (auto* popup = BanHistoryPopup::create(m_profile.name, *m_banHistory)) popup->show();
    }
    void onRankedAdmin(CCObject*) {
        if (auto* popup = AdminRankedPopup::create(m_profile)) popup->show();
    }
    // 신고하기. 먼저 내가 신고 금지 상태인지 서버에서 확인하고, 금지면 안내를 보여 주고, 아니면 신고 팝업을 연다.
    void onReport(CCObject*) {
        if (m_reportChecking) return;
        m_reportChecking = true;
        Service::get().fetchOwnBan([self = Weak<PlayerPopup>(this)](bool ok, std::optional<BanInfo>, std::string) {
            auto owner = self.lock();
            if (!owner) return;
            owner->m_reportChecking = false;
            auto const& blocked = Service::get().ownReportBan();
            if (ok && blocked && (blocked->permanent || blocked->until > Service::get().serverNow())) {
                showReportBanNotice(*blocked);
                return;
            }
            if (auto* popup = ReportPopup::create(owner->m_profile)) popup->show();
        });
    }
    bool m_reportChecking = false;
    // 관리자: 이 플레이어의 신고 밴을 편집하는 팝업을 연다 (현재 밴이 있으면 함께 전달).
    void onReportBan(CCObject*) {
        Service::get().fetchReportBans([self = Weak<PlayerPopup>(this)](std::vector<BanInfo> bans, std::string detail) {
            auto owner = self.lock();
            if (!owner) return;
            if (!detail.empty()) {
                error(detail);
                return;
            }
            std::optional<BanInfo> current;
            for (auto& ban : bans)
                if (ban.uid == owner->m_profile.uid) current = std::move(ban);
            auto const& profile = owner->m_profile;
            if (auto* popup = BanEditPopup::create(profile.uid, profile.name, profile.accountId, current, "", {}, true))
                popup->show();
        });
    }
    // 관리자: 이 플레이어의 밴을 편집하는 팝업을 연다.
    void onBan(CCObject*) {
        Service::get().fetchBans([self = Weak<PlayerPopup>(this)](std::vector<BanInfo> bans, std::string detail) {
            auto owner = self.lock();
            if (!owner) return;
            if (!detail.empty()) {
                error(detail);
                return;
            }
            std::optional<BanInfo> current;
            for (auto& ban : bans)
                if (ban.uid == owner->m_profile.uid) current = std::move(ban);
            auto const& profile = owner->m_profile;
            if (auto* popup = BanEditPopup::create(profile.uid, profile.name, profile.accountId, current, "", {}))
                popup->show();
        });
    }
    void onHistory(CCObject*) {
        if (auto* popup = HistoryPopup::create(m_profile)) popup->show();
    }
    void onMapRatings(CCObject*) {
        if (auto* popup = MapRatingsPopup::create(m_profile)) popup->show();
    }
    // GD 프로필 페이지를 연다. GD 계정이 없는 플레이어는 오류 안내.
    void onProfile(CCObject*) {
        if (m_profile.accountId <= 0) {
            error("This player does not have a GD account profile.");
            return;
        }
        if (auto* page =
                ProfilePage::create(m_profile.accountId, m_profile.accountId == Service::get().profile().accountId))
            page->show();
    }

public:
    static PlayerPopup* create(PlayerProfile const& profile) {
        auto* result = new PlayerPopup;
        if (result->init(profile)) {
            result->autorelease();
            return result;
        }
        delete result;
        return nullptr;
    }
};

// uid가 있는 프로필에 대해서만 플레이어 팝업을 연다.
void showPlayer(PlayerProfile const& profile) {
    if (profile.uid.empty()) return;
    if (auto* popup = PlayerPopup::create(profile)) popup->show();
}

// Auto 규칙에서 맵에 맞춰 자동으로 정해지는 목숨/시도 수와 그 근거 설명.
struct AutoLives {
    int lives = 0;
    std::string basis;
};
// Auto 목숨 계산. 랜덤 맵이거나 맵이 정해지지 않았으면 값이 없다. 근거는 데몬 난이도, 별 개수,
// 난이도 얼굴, 맵 길이 순으로 만든다.
std::optional<AutoLives> autoLives(LevelInfo const& level, bool random) {
    if (random || level.id <= 0) return std::nullopt;
    int const difficulty = level.autoLevel ? -1 : level.difficulty;
    AutoLives result;
    result.lives = autoRoomLives(level);
    static constexpr char const* demons[] = {"Hard Demon", "Easy Demon", "Medium Demon", "Insane Demon",
                                             "Extreme Demon"};
    static constexpr char const* faces[] = {"Unrated", "Easy", "Normal", "Hard", "Harder", "Insane"};
    if (level.demon)
        result.basis = demons[difficulty >= 7 && difficulty <= 10 ? difficulty - 6 : 0];
    else if (level.stars >= 2 && level.stars <= 9)
        result.basis = fmt::format("{} Stars", level.stars);
    else if (difficulty < 0)
        result.basis = "Auto";
    else
        result.basis = faces[std::clamp(difficulty, 0, 5)];
    static constexpr char const* lengths[] = {"Tiny", "Short", "Medium", "Long", "XL", "Platformer"};
    if (level.length >= 0 && level.length <= 5) result.basis += fmt::format(", {}", lengths[level.length]);
    return result;
}
// Auto 규칙 안내 문구. 꺼져 있으면 일반 설명, 켜져 있으면 계산된 개수와 근거를 보여 준다.
std::string autoLivesLine(bool on, LevelInfo const& level, bool random, char const* unit) {
    if (!on) return fmt::format("Auto: the {} follow the map like Ranked's", unit);
    if (auto const value = autoLives(level, random))
        return fmt::format("Auto: {} {} - Ranked's for {}", value->lives,
                           value->lives == 1 ? (std::string_view(unit) == "lives" ? "life" : "attempt") : unit,
                           value->basis);
    return fmt::format("Auto: Ranked's {} for the {}", unit, random ? "map rolled at the start" : "map, once chosen");
}

// 규칙 팝업의 Auto 안내 문구 위치/크기 상수.
constexpr float kRulesAutoX = 97.f, kRulesAutoY = 140.f;
constexpr float kRulesAutoTextX = 108.f, kRulesAutoTextWidth = 130.f, kRulesAutoTextHeight = 17.f;
// Auto 안내 문구를 한 줄로 두거나, 공백 기준으로 두 줄로 나눠 더 크게 보일 때는 두 줄로 배치한다.
// (공간 안에서 글자 크기가 더 커지는 쪽을 선택)
void rulesAutoText(CCNode* holder, std::string const& text, ccColor3B color) {
    holder->removeAllChildrenWithCleanup(true);
    auto make = [holder, color](std::string const& line) {
        auto* value = CCLabelBMFont::create(line.c_str(), "bigFont.fnt");
        value->setAnchorPoint({0.f, .5f});
        value->setColor(color);
        holder->addChild(value);
        return value;
    };
    auto* single = make(text);
    float const lineHeight = std::max(1.f, single->getContentSize().height);
    float const one = std::min(
        {.3f, kRulesAutoTextWidth / std::max(1.f, single->getContentSize().width), kRulesAutoTextHeight / lineHeight});
    auto const space = [&text] {
        size_t best = std::string::npos;
        for (size_t i = text.find(' '); i != std::string::npos; i = text.find(' ', i + 1))
            if (best == std::string::npos || std::abs(static_cast<long>(i) - static_cast<long>(text.size() / 2)) <
                                                 std::abs(static_cast<long>(best) - static_cast<long>(text.size() / 2)))
                best = i;
        return best;
    }();
    if (space != std::string::npos) {
        auto* first = make(text.substr(0, space));
        auto* second = make(text.substr(space + 1));
        float const wide = std::max({1.f, first->getContentSize().width, second->getContentSize().width});
        float const two = std::min({.3f, kRulesAutoTextWidth / wide, kRulesAutoTextHeight / 2.f / lineHeight});
        if (two > one) {
            single->removeFromParent();
            float const step = lineHeight * two / 2.f;
            for (auto [line, y] : {std::pair{first, kRulesAutoY + step}, std::pair{second, kRulesAutoY - step}}) {
                line->setScale(two);
                line->setPosition({kRulesAutoTextX, y});
            }
            return;
        }
        first->removeFromParent();
        second->removeFromParent();
    }
    single->setScale(one);
    single->setPosition({kRulesAutoTextX, kRulesAutoY});
}

// 방 규칙(모드, 시도 횟수, 목표 퍼센트, 연습, 핵 허용, 속도 등)을 고르는 팝업.
class GameRulesPopup : public ::versus::DeferredPopup {
    // 편집 중인 규칙 사본과 대상 방 ID. 방장이 아니거나 이미 시작/맵 추첨 중이면 읽기 전용(m_readOnly)으로 열린다.
    GameRules m_rules;
    std::string m_roomID;
    TextInput* m_value = nullptr;
    CCLabelBMFont* m_valueTitle = nullptr;
    void onExit() override {
        if (m_value) m_value->defocus();
        Popup::onExit();
    }

    CCLabelBMFont* m_unlimited = nullptr;
    CCMenuItemSpriteExtra* m_mode = nullptr;
    CCMenuItemSpriteExtra* m_practice = nullptr;
    CCMenuItemSpriteExtra* m_hacks = nullptr;
    CCMenuItemSpriteExtra* m_less = nullptr;
    CCMenuItemSpriteExtra* m_more = nullptr;
    CCMenuItemSpriteExtra* m_save = nullptr;
    CCLabelBMFont* m_speedLabel = nullptr;
    CCLabelBMFont* m_speedHint = nullptr;
    CCMenuItemSpriteExtra* m_speedLess = nullptr;
    CCMenuItemSpriteExtra* m_speedMore = nullptr;
    CCMenuItemToggler* m_auto = nullptr;
    CCNode* m_autoHint = nullptr;
    NineSlice* m_rulesPanel = nullptr;
    // Auto 목숨 계산에 쓰는 방의 레벨 정보와 랜덤 맵 여부, 플랫포머 맵 여부(Percent 모드 불가).
    LevelInfo m_level;
    bool m_random = false;
    bool m_pending = false;
    bool m_readOnly = false;
    bool m_platformer = false;

    // 버튼 안의 글자/배경을 바꾼다.
    void caption(CCMenuItemSpriteExtra* item, std::string const& text, float scale = .5f,
                 char const* background = "GJ_button_04.png") {
        auto* sprite = ButtonSprite::create(text.c_str(), "goldFont.fnt", background);
        sprite->setScale(scale);
        item->setSprite(sprite);
    }
    bool init(RoomInfo const& room) {
        if (!Popup::init(330.f, 270.f, "GJ_square02.png")) return false;
        m_rules = room.rules;
        m_readOnly = !Service::get().isHost() || room.started || drawingMap(room);
        m_platformer = room.level.platformer || (room.mapSelection.random && room.mapSelection.platformer);
        m_roomID = room.id;
        m_level = room.level;
        m_random = needsRandomDraw(room);
        geode::addSideArt(m_mainLayer, SideArt::All, SideArtStyle::PopupBlue);
        setTitle("Game Rules", "bigFont.fnt", .65f);
        m_rulesPanel = NineSlice::create("square02b_001.png");
        m_rulesPanel->setID("rules-panel"_spr);
        m_rulesPanel->setContentSize({286.f, 138.f});
        m_rulesPanel->setPosition({165.f, 165.f});
        m_mainLayer->addChild(m_rulesPanel);
        auto* speedPanel = NineSlice::create("square02b_001.png");
        speedPanel->setID("rules-speed-panel"_spr);
        speedPanel->setContentSize({286.f, 46.f});
        speedPanel->setPosition({165.f, 69.f});
        speedPanel->setColor(ccc3(0, 0, 0));
        speedPanel->setOpacity(110);
        m_mainLayer->addChild(speedPanel);
        m_mode = button(m_buttonMenu, this, menu_selector(GameRulesPopup::onMode), "Attempts", {165.f, 217.f}, .62f);
        m_valueTitle = label(m_mainLayer, "Attempt limit", {165.f, 189.f}, .45f, 270.f, kIce);
        m_value = TextInput::create(96.f, "3");
        m_value->setCommonFilter(CommonFilter::Uint);
        m_value->setMaxCharCount(3);
        m_value->setPosition({165.f, 165.f});
        styleInput(m_value);
        m_mainLayer->addChild(m_value);
        m_unlimited = label(m_mainLayer, "Unlimited", {165.f, 165.f}, .48f, 150.f, kIce);
        m_less = button(m_buttonMenu, this, menu_selector(GameRulesPopup::onLess), "-", {95.f, 165.f}, .46f);
        m_more = button(m_buttonMenu, this, menu_selector(GameRulesPopup::onMore), "+", {235.f, 165.f}, .46f);
        m_auto = CCMenuItemToggler::createWithStandardSprites(this, menu_selector(GameRulesPopup::onAuto), .4f);
        m_auto->setID("rules-auto-lives"_spr);
        m_auto->setPosition({kRulesAutoX, kRulesAutoY});
        m_auto->toggle(m_rules.autoLives);
        m_buttonMenu->addChild(m_auto);
        m_autoHint = CCNode::create();
        m_autoHint->setID("rules-auto-text"_spr);
        m_mainLayer->addChild(m_autoHint);
        m_practice =
            button(m_buttonMenu, this, menu_selector(GameRulesPopup::onPractice), "Practice: OFF", {98.f, 109.f}, .5f);
        m_hacks = button(m_buttonMenu, this, menu_selector(GameRulesPopup::onHacks), "Hacks: OFF", {232.f, 109.f}, .5f);
        m_speedLabel = label(m_mainLayer, "Speed: 1x", {165.f, 80.f}, .5f, 120.f, kIce);
        m_speedLess = button(m_buttonMenu, this, menu_selector(GameRulesPopup::onSpeedLess), "-", {95.f, 80.f}, .46f);
        m_speedMore = button(m_buttonMenu, this, menu_selector(GameRulesPopup::onSpeedMore), "+", {235.f, 80.f}, .46f);
        m_speedHint = label(m_mainLayer, "Both players' map plays at this speed. Respawns stay normal.", {165.f, 59.f},
                            .32f, 276.f, kMuted, false, "chatFont.fnt");
        m_save = button(m_buttonMenu, this, menu_selector(GameRulesPopup::onSave), m_readOnly ? "OK" : "Apply",
                        {165.f, 25.f}, .62f);
        refresh();
        return true;
    }
    // 입력창 값을 현재 모드의 값(시도 횟수 1~99 또는 목표 퍼센트 1~100)으로 읽어 m_rules에 반영한다.
    // 범위를 벗어나면 false (showError면 안내도 띄움).
    bool readValue(bool showError = false) {
        auto const text = std::string(m_value->getString());
        int value = 0;
        auto parsed = std::from_chars(text.data(), text.data() + text.size(), value);
        int const maximum = m_rules.mode == 0 ? 99 : 100;
        if (parsed.ec != std::errc() || parsed.ptr != text.data() + text.size() || value < 1 || value > maximum) {
            if (showError) error(fmt::format("Enter a number from 1 to {}.", maximum));
            return false;
        }
        if (m_rules.mode == 0)
            m_rules.attempts = value;
        else
            m_rules.targetPercent = value;
        return true;
    }
    // m_rules 상태에 맞춰 모든 컨트롤을 갱신한다. 연습 모드(Attempts)는 시도 제한이 없고,
    // Auto가 켜져 있으면 맵에서 계산한 개수를 쓰며 직접 입력은 막는다.
    void refresh() {
        caption(m_mode, m_rules.mode == 0 ? "Mode: Attempts" : "Mode: Percent", .62f);
        m_rulesPanel->setColor(m_rules.mode == 0 ? ccc3(205, 210, 220) : ccc3(150, 255, 165));
        m_rulesPanel->setOpacity(m_rules.mode == 0 ? 95 : 110);
        bool const unlimited = m_rules.mode == 0 && m_rules.practice;
        bool const autoOn = m_rules.livesFollowMap();
        if (autoOn)
            if (auto const value = autoLives(m_level, m_random)) m_rules.attempts = value->lives;
        m_valueTitle->setString(unlimited           ? "No attempt limit"
                                : m_rules.mode == 0 ? "Attempt limit (1-99)"
                                                    : "Target percent (1-100)");
        m_value->setString(std::to_string(m_rules.mode == 0 ? m_rules.attempts : m_rules.targetPercent));
        m_value->setVisible(!unlimited);
        m_unlimited->setVisible(unlimited);
        caption(m_practice, m_rules.practice ? "Practice: ON" : "Practice: OFF", .5f,
                m_rules.practice ? "GJ_button_01.png" : "GJ_button_04.png");
        caption(m_hacks, m_rules.allowHacks ? "Hacks: ON" : "Hacks: OFF", .5f,
                m_rules.allowHacks ? "GJ_button_02.png" : "GJ_button_04.png");
        enabled(m_hacks, !m_pending && !m_readOnly);
        bool const valueEnabled = !(m_rules.mode == 0 && m_rules.practice) && !m_pending && !m_readOnly && !autoOn;
        m_value->setEnabled(valueEnabled);
        m_value->getBGSprite()->setOpacity(valueEnabled ? 230 : 105);
        enabled(m_less, valueEnabled);
        enabled(m_more, valueEnabled);
        m_less->setVisible(!m_readOnly && !unlimited);
        m_more->setVisible(!m_readOnly && !unlimited);
        enabled(m_mode, !m_pending && !m_readOnly);
        enabled(m_practice, !m_pending && !m_readOnly);
        m_speedLabel->setString(("Speed: " + speedText(m_rules.speed)).c_str());
        m_speedLabel->setColor(m_rules.speed == 100 ? kIce : ccc3(255, 120, 200));
        enabled(m_speedLess, !m_pending && !m_readOnly && m_rules.speed > 50);
        enabled(m_speedMore, !m_pending && !m_readOnly && m_rules.speed < 300);
        m_speedLess->setVisible(!m_readOnly);
        m_speedMore->setVisible(!m_readOnly);
        bool const autoShown = m_rules.mode == 0 && !m_rules.practice;
        m_auto->setVisible(autoShown);
        m_auto->setEnabled(!m_pending && !m_readOnly);
        m_autoHint->setVisible(autoShown);
        if (autoShown)
            rulesAutoText(m_autoHint, autoLivesLine(autoOn, m_level, m_random, "attempts"),
                          autoOn ? ccc3(150, 255, 175) : kMuted);
        if (m_readOnly) {
            m_mode->setOpacity(255);
            m_practice->setOpacity(255);
            m_hacks->setOpacity(255);
            m_less->setVisible(false);
            m_more->setVisible(false);
            m_value->getBGSprite()->setOpacity(190);
        }
    }
    // 속도를 25%씩 조절한다 (50~300%).
    void speedStep(int amount) {
        if (m_pending || m_readOnly) return;
        readValue();
        m_rules.speed = std::clamp(m_rules.speed + amount, 50, 300);
        refresh();
    }
    void onSpeedLess(CCObject*) { speedStep(-25); }
    void onSpeedMore(CCObject*) { speedStep(25); }
    // 모드 전환 (Attempts <-> Percent). 플랫포머 맵은 Percent 모드를 쓸 수 없다.
    void onMode(CCObject*) {
        if (!m_pending && !m_readOnly) {
            if (m_rules.mode == 0 && m_platformer) {
                error("<cy>Platformer</c> maps do not support <cr>Percent mode</c>. Use Attempts mode.");
                return;
            }
            readValue();
            m_rules.mode = 1 - m_rules.mode;
            refresh();
        }
    }
    void onHacks(CCObject*) {
        if (m_pending || m_readOnly) return;
        readValue();
        m_rules.allowHacks = !m_rules.allowHacks;
        refresh();
    }
    void onPractice(CCObject*) {
        if (m_pending || m_readOnly) return;
        readValue();
        m_rules.practice = !m_rules.practice;
        refresh();
    }
    void adjust(int amount) {
        if (m_pending || m_readOnly) return;
        readValue();
        if (m_rules.mode == 0)
            m_rules.attempts = std::clamp(m_rules.attempts + amount, 1, 99);
        else
            m_rules.targetPercent = std::clamp(m_rules.targetPercent + amount, 1, 100);
        refresh();
    }
    void onLess(CCObject*) { adjust(-1); }
    void onMore(CCObject*) { adjust(1); }
    // Auto 목숨 토글. 방의 최신 레벨 정보를 다시 읽어 계산에 사용한다.
    void onAuto(CCObject*) {
        if (m_pending || m_readOnly) return;
        if (auto const& room = Service::get().room(); room && room->id == m_roomID) {
            m_level = room->level;
            m_random = needsRandomDraw(*room);
        }
        readValue();
        m_value->defocus();
        m_rules.autoLives = !m_auto->isToggled();
        refresh();
    }
    // 적용: 방장이 맞고 방이 아직 시작 전인지 다시 확인한 뒤 서버에 규칙 변경을 요청한다.
    // 읽기 전용이면 그냥 닫는다.
    void onSave(CCObject*) {
        if (m_readOnly) {
            onClose(nullptr);
            return;
        }
        if (m_pending || !readValue(true)) return;
        auto const& room = Service::get().room();
        if (!room || room->id != m_roomID || !Service::get().isHost() || room->started) {
            error("Only the host can edit rules before a match.");
            return;
        }
        m_value->defocus();
        m_pending = true;
        refresh();
        enabled(m_save, false);
        m_closeBtn->setEnabled(false);
        Service::get().configureRules(m_rules, [self = Weak<GameRulesPopup>(this)](bool success, std::string detail) {
            auto owner = self.lock();
            if (!owner) return;
            owner->m_pending = false;
            if (success) {
                owner->onClose(nullptr);
                return;
            }
            owner->refresh();
            enabled(owner->m_save, true);
            owner->m_closeBtn->setEnabled(true);
            error(detail);
        });
    }
    void onClose(CCObject* sender) override {
        if (!m_pending) Popup::onClose(sender);
    }

public:
    static GameRulesPopup* create(RoomInfo const& room) {
        auto* result = new GameRulesPopup;
        if (result->init(room)) {
            result->autorelease();
            return result;
        }
        delete result;
        return nullptr;
    }
};

// 맵 선택, 맵 풀, 오라 캐러셀, 치장, 계정 선택, 진입 화면 UI (인라인 include).
#include "MapSelectionPopup.inl"
#include "MapPoolPopup.inl"
#include "AuraCarousel.inl"

#include "CosmeticsPopup.inl"
#include "AccountChoice.inl"
#include "VersusEntry.inl"

// Public Room 로비 화면. 방 목록(1v1 방과 Group 방)을 검색/갱신/참가하고, 내 프로필 요약과
// 치장(Aura/Finisher/Nickname/Titles)/전적 버튼이 있는 통계 패널을 보여 준다.
class LobbyLayer : public SceneLayer {
    CCNode* m_listPanel = nullptr;
    CCNode* m_statsPanel = nullptr;
    CCNode* m_rows = nullptr;
    TapScrollLayer* m_scroll = nullptr;
    CCSize m_rowsView;
    CCNode* m_stats = nullptr;
    TextInput* m_search = nullptr;
    CCLabelBMFont* m_status = nullptr;
    void onExit() override {
        if (m_search) m_search->defocus();
        SceneLayer::onExit();
    }
    size_t m_rowsShown = 0;
    CCLabelBMFont* m_empty = nullptr;
    CCMenuItemSpriteExtra* m_previous = nullptr;
    CCMenuItemSpriteExtra* m_next = nullptr;
    CCMenuItemSpriteExtra* m_create = nullptr;
    CCMenuItemSpriteExtra* m_retry = nullptr;
    CCMenuItemSpriteExtra* m_history = nullptr;
    CCMenuItemSpriteExtra* m_debugRoom = nullptr;
    std::vector<RoomInfo> m_rooms;
    std::vector<GroupSummary> m_groups;
    std::vector<size_t> m_filtered;
    // m_filtered에서 이 값 이상이면 Group 방 인덱스(값 - kGroupTag)를 뜻한다.
    static constexpr size_t kGroupTag = 100000;
    int m_page = 0;
    float m_listWidth = 0.f;
    float m_height = 0.f;
    // 수동 새로고침 쿨다운(초)과 검색어 입력 후 목록 갱신까지의 지연.
    float m_refreshCooldown = 0.f;
    bool m_autoRefreshed = false;
    float m_filterDelay = -1.f;
    bool m_fetching = false;
    bool m_connecting = false;
    bool m_loadingStats = false;
    bool m_mutating = false;
    std::string m_notice;
    std::string m_lastError;
    int m_statsGames = -1;
    std::string m_statsKey;
    double m_statsRate = -1;

    // 화면이 실제로 표시 중일 때만 대기 중인 안내문을 한 번 띄운다.
    void showPendingNotice() {
        if (m_notice.empty() || !isRunning()) return;
        CCNode* scene = this;
        while (scene->getParent()) scene = scene->getParent();
        if (scene != CCDirector::sharedDirector()->getRunningScene()) return;
        auto message = std::exchange(m_notice, {});
        if (showMaintenanceNotice(message)) return;
        FLAlertLayer::create("Versus", message.c_str(), "OK")->show();
    }

    // 화면 구성: 방 목록 패널(검색, 스크롤 목록, 생성/새로고침/디버그 방 버튼)과 통계 패널.
    bool init(std::string notice) {
        if (!initScene("Public Room")) return false;
        addAudioButton(false);
        addLiveButton(m_window.width - 70.f);
        setID("versus-lobby"_spr);
        watchTitles();
        m_notice = std::move(notice);
        float const totalWidth = std::min(550.f, m_window.width - 44.f);
        m_height = std::min(250.f, m_window.height - 64.f);
        m_listWidth = totalWidth * .70f - 5.f;
        float const statsWidth = totalWidth - m_listWidth - 12.f;
        float const originX = (m_window.width - totalWidth) / 2.f;
        float const originY = (m_window.height - m_height) / 2.f - 10.f;
        m_listPanel = panel(this, {originX, originY}, {m_listWidth, m_height});
        m_statsPanel = panel(this, {originX + m_listWidth + 12.f, originY}, {statsWidth, m_height});
        label(m_listPanel, "Room List", {m_listWidth / 2.f, m_height - 20.f}, .52f, m_listWidth - 30.f);
        m_search = TextInput::create(m_listWidth - 32.f, "Search room name");
        m_search->setCommonFilter(CommonFilter::Any);
        m_search->setMaxCharCount(32);
        m_search->setScale(.75f);
        m_search->setWidth((m_listWidth - 30.f) / .75f);
        m_search->setPosition({m_listWidth / 2.f, m_height - 48.f});
        styleInput(m_search);
        m_search->setCallback([self = Weak<LobbyLayer>(this)](std::string const&) {
            if (auto owner = self.lock()) owner->m_filterDelay = .12f;
        });
        m_listPanel->addChild(m_search);
        m_rowsView = CCSize(m_listWidth - 24.f, m_height - 102.f);
        m_scroll = TapScrollLayer::create(m_rowsView, [self = Weak<LobbyLayer>(this)](CCNode* row) {
            if (auto owner = self.lock()) owner->joinRoom(row->getTag());
        });
        m_scroll->setPosition({12.f, 38.f});
        m_listPanel->addChild(m_scroll);
        m_rows = m_scroll->m_contentLayer;
        m_empty = label(m_listPanel, "Connecting...", {m_listWidth / 2.f, m_height / 2.f - 5.f}, .43f,
                        m_listWidth - 45.f, kIce);
        auto* actions = menu(m_listPanel);
        m_create = button(actions, this, menu_selector(LobbyLayer::onCreate), "Create Room",
                          {m_listWidth - 102.f, 22.f}, .48f);
        m_retry = iconButton(actions, this, menu_selector(LobbyLayer::onRetry), "GJ_updateBtn_001.png",
                             {m_listWidth - 24.f, 22.f}, 28.f);
        m_debugRoom = button(actions, this, menu_selector(LobbyLayer::onDebugRoom), "Debug Room",
                             {m_listWidth - 212.f, 22.f}, .42f, "GJ_button_05.png");
        m_debugRoom->setID("debug-room-button"_spr);
        m_debugRoom->setVisible(debug::active());
        enabled(m_create, false);
        m_status = label(this, "Connecting...", {m_window.width / 2.f, 9.f}, .25f, totalWidth - 30.f, kIce);
        m_stats = CCNode::create();
        m_statsPanel->addChild(m_stats);
        auto* statsActions = menu(m_statsPanel);
        {
            char const* names[] = {"Aura", "Finisher", "Nickname", "Titles", "History"};
            SEL_MenuHandler handlers[] = {menu_selector(LobbyLayer::onAura), menu_selector(LobbyLayer::onFinisher),
                                          menu_selector(LobbyLayer::onNickname), menu_selector(LobbyLayer::onTitles),
                                          menu_selector(LobbyLayer::onHistory)};
            float const slot = std::min(40.f, (statsWidth - 16.f) / 3.f), size = std::min(30.f, slot - 8.f);
            float const rowY[] = {std::min(90.f, m_height * .36f), std::min(41.f, m_height * .164f)};
            for (int i = 0; i < 5; ++i) {
                CCNode* sprite = nullptr;
                if (i == 0)
                    sprite = CircleButtonSprite::createWithSpriteFrameName(
                        "GJ_sMagicIcon_001.png", 1.f, CircleBaseColor::Cyan, CircleBaseSize::Medium);
                else if (i == 1)
                    sprite = CircleButtonSprite::createWithSpriteFrameName(
                        "GJ_demonIcon_001.png", 1.f, CircleBaseColor::Pink, CircleBaseSize::Medium);
                else if (i == 2)
                    sprite = loadedFrameSprite("GJ_profileButton_001.png");
                else if (i == 3)
                    sprite = loadedFrameSprite("GJ_achBtn_001.png");
                else
                    sprite = loadedFrameSprite("GJ_viewListsBtn_001.png");
                if (!sprite)
                    sprite = ButtonSprite::create(names[i], 50, true, "goldFont.fnt", "GJ_button_01.png", 30.f, .6f);
                sprite->setScale(size / std::max(1.f, sprite->getContentSize().height));
                auto* item = CCMenuItemSpriteExtra::create(sprite, this, handlers[i]);
                int const row = i < 3 ? 0 : 1, column = i < 3 ? i : i - 3;
                float const x =
                    row == 0 ? statsWidth / 2.f + (column - 1.f) * slot : statsWidth / 2.f + (column - .5f) * slot;
                item->setPosition({x, rowY[row]});
                item->setID(fmt::format("lobby-stats-{}", i));
                statsActions->addChild(item);
                label(m_statsPanel, names[i], {x, rowY[row] - size / 2.f - 7.f}, .3f, slot - 2.f, ccWHITE, false,
                      "goldFont.fnt");
                if (i == 4) m_history = item;
            }
        }
        enabled(m_history, false);
        refreshStats();
        return true;
    }
    // 서버에 연결한다. 성공하면 버튼을 풀고 통계와 방 목록을 한 번 받아 온다. 실패하면 새로고침으로 재시도하게 한다.
    void connect() {
        if (m_connecting || m_fetching || m_transitioning) return;
        m_connecting = true;
        m_status->setString("Connecting...");
        m_empty->setString("Connecting...");
        enabled(m_retry, false);
        Service::get().connect([self = Weak<LobbyLayer>(this)](bool success, std::string detail) {
            auto owner = self.lock();
            if (!owner || owner->m_transitioning) return;
            owner->m_connecting = false;
            enabled(owner->m_retry, owner->m_refreshCooldown <= 0.f);
            if (!success) {
                owner->m_lastError = std::move(detail);
                owner->m_status->setString("Connection unavailable. Press Refresh to retry.");
                owner->m_empty->setString("Unable to connect");
                error(owner->m_lastError);
                return;
            }
            owner->m_lastError.clear();
            enabled(owner->m_create, true);
            enabled(owner->m_history, true);
            owner->refreshStats();
            owner->fetchStats();
            owner->m_status->setString("Connected. Press Refresh to load rooms.");
            owner->m_empty->setString("Press Refresh to load rooms");
            owner->autoRefreshOnce();
        });
    }
    // 연결 직후 방 목록을 한 번만 자동으로 받는다.
    void autoRefreshOnce() {
        if (m_autoRefreshed || !Service::get().connected()) return;
        m_autoRefreshed = true;
        m_empty->setString("Loading rooms...");
        fetch();
    }
    // 내 최근 전적을 받아 통계 패널을 갱신한다 (받는 동안 방 만들기는 잠금).
    void fetchStats() {
        m_loadingStats = true;
        enabled(m_create, false);
        Service::get().fetchHistory([self = Weak<LobbyLayer>(this)](std::vector<MatchRecord>, std::string detail) {
            auto owner = self.lock();
            if (!owner || owner->m_transitioning) return;
            owner->m_loadingStats = false;
            enabled(owner->m_create, Service::get().connected() && !owner->m_mutating);
            if (detail.empty())
                owner->refreshStats();
            else
                log::warn("Versus match history unavailable: {}", detail);
            if (!owner->m_rooms.empty()) owner->renderRooms();
        });
    }
    // 방 목록을 새로 받는다 (수동은 쿨다운 적용). 1v1 방을 먼저 받아 정렬한 뒤 이어서 Group 방도 받는다.
    // 1v1은 빈 방, 최근 갱신순; Group은 자리가 남은 방, 인원 많은 순.
    void fetch(bool manual = false) {
        if (m_fetching || m_mutating || m_transitioning || !Service::get().connected() ||
            (manual && m_refreshCooldown > 0.f))
            return;
        m_fetching = true;
        m_refreshCooldown = firebase_config::ROOM_LIST_REFRESH_COOLDOWN_SECONDS;
        m_status->setString("Refreshing rooms...");
        enabled(m_retry, false);
        Service::get().fetchRooms([self = Weak<LobbyLayer>(this)](std::vector<RoomInfo> rooms, std::string detail) {
            auto owner = self.lock();
            if (!owner || owner->m_transitioning) return;
            owner->m_fetching = false;
            enabled(owner->m_retry, owner->m_refreshCooldown <= 0.f);
            if (!detail.empty()) {
                owner->m_lastError = std::move(detail);
                owner->m_status->setString("Room refresh failed. Refresh to retry.");
                if (owner->m_rooms.empty()) owner->m_empty->setString("Rooms unavailable");
                return;
            }
            owner->m_lastError.clear();
            owner->m_rooms = std::move(rooms);
            std::sort(owner->m_rooms.begin(), owner->m_rooms.end(), [](RoomInfo const& a, RoomInfo const& b) {
                if (a.guest.has_value() != b.guest.has_value()) return !a.guest.has_value();
                if (a.updatedAt != b.updatedAt) return a.updatedAt > b.updatedAt;
                return a.id < b.id;
            });
            owner->m_fetching = true;
            Service::get().fetchGroups([self](std::vector<GroupSummary> groups, std::string detail) {
                auto owner = self.lock();
                if (!owner || owner->m_transitioning) return;
                owner->m_fetching = false;
                if (!detail.empty()) log::warn("Versus Group room list: {}", detail);
                owner->m_groups = std::move(groups);
                std::stable_sort(owner->m_groups.begin(), owner->m_groups.end(),
                                 [](GroupSummary const& a, GroupSummary const& b) {
                                     bool const openA = a.count < a.maxPlayers, openB = b.count < b.maxPlayers;
                                     if (openA != openB) return openA;
                                     return a.count > b.count;
                                 });
                owner->renderRooms();
                owner->refreshStats();
            });
        });
    }
    // Group 방 한 줄을 그린다 (보라색 테마, 인원 수/상태 배지).
    void groupRow(int row, size_t index, float rowStep, float rowHeight, float listHeight) {
        auto const& room = m_groups[index];
        bool const full = room.count >= room.maxPlayers;
        auto const accent = ccc3(205, 150, 255);
        auto const stateColor = room.started ? ccc3(255, 203, 117) : full ? kMuted : ccc3(117, 247, 184);
        auto* rowSprite = CCSprite::create();
        rowSprite->setContentSize({m_listWidth - 24.f, rowHeight});
        float const rowWidth = rowSprite->getContentSize().width;
        auto* bg = NineSlice::create("square02b_001.png");
        bg->setContentSize(rowSprite->getContentSize());
        bg->setColor(ccc3(30, 14, 62));
        bg->setOpacity(245);
        bg->setPosition(rowSprite->getContentSize() / 2.f);
        rowSprite->addChild(bg, -1);
        auto* gradient = CCLayerGradient::create(row % 2 ? ccc4(92, 52, 170, 220) : ccc4(70, 38, 140, 220),
                                                 ccc4(24, 14, 64, 170), {1.f, -.3f});
        gradient->setContentSize({rowWidth - 8.f, rowHeight - 6.f});
        gradient->setPosition({4.f, 3.f});
        rowSprite->addChild(gradient, -1);
        auto* trim = CCDrawNode::create();
        trim->drawSegment({5.f, 7.f}, {5.f, rowHeight - 7.f}, 1.4f,
                          {accent.r / 255.f, accent.g / 255.f, accent.b / 255.f, .95f});
        trim->drawDot({34.f, rowHeight / 2.f}, rowHeight * .38f, {.26f, .14f, .48f, .9f});
        rowSprite->addChild(trim);
        if (room.privateRoom) {
            auto* lock = CCSprite::createWithSpriteFrameName("GJ_lockGray_001.png");
            lock->setScale(.23f);
            lock->setColor(ccc3(255, 217, 144));
            lock->setPosition({17.f, rowHeight * .28f});
            rowSprite->addChild(lock, 2);
        }
        player(rowSprite, room.host, {34.f, rowHeight / 2.f}, .60f);
        float const infoWidth = rowWidth - 122.f;
        float const nameWidth = infoWidth * .53f;
        float const badgeWidth = infoWidth - nameWidth - 7.f;
        float const badgeX = 57.f + nameWidth + 7.f;
        styleNickname(
            label(rowSprite, room.host.name, {57.f, rowHeight * .68f}, .32f, nameWidth, {255, 255, 255}, true),
            room.host.membership, room.host.cosmetics);
        styleNickname(
            label(rowSprite, room.name, {57.f, rowHeight * .31f}, .39f, nameWidth, kIce, true, "chatFont.fnt"),
            room.host.membership, room.host.cosmetics);
        auto* modeBadge = NineSlice::create("square02b_001.png");
        modeBadge->setContentSize({badgeWidth, rowHeight - 10.f});
        modeBadge->setColor(ccc3(58, 28, 104));
        modeBadge->setPosition({badgeX + badgeWidth / 2.f, rowHeight / 2.f});
        rowSprite->addChild(modeBadge);
        label(rowSprite, "GROUP", {badgeX + badgeWidth / 2.f, rowHeight * .66f}, .28f, badgeWidth - 8.f, accent);
        auto const groupSpeed = room.rules.speed != 100 ? " / " + speedText(room.rules.speed) : std::string();
        auto const groupRule = room.rules.mode == 1  ? fmt::format("First to {}%", room.rules.targetPercent)
                               : room.rules.practice ? std::string("Practice")
                                                     : fmt::format("{} Lives", room.rules.attempts);
        label(rowSprite,
              (room.level.id > 0 ? fmt::format("{} / {}", room.level.name, groupRule)
               : room.random     ? fmt::format("Random Map / {}", groupRule)
                                 : groupRule) +
                  groupSpeed,
              {badgeX + badgeWidth / 2.f, rowHeight * .32f}, .41f, badgeWidth - 8.f, kIce, false, "chatFont.fnt");
        auto* statusBadge = NineSlice::create("square02b_001.png");
        statusBadge->setContentSize({48.f, rowHeight - 9.f});
        statusBadge->setColor(room.started ? ccc3(79, 56, 31) : full ? ccc3(24, 41, 64) : ccc3(10, 66, 61));
        statusBadge->setPosition({rowWidth - 29.f, rowHeight / 2.f});
        rowSprite->addChild(statusBadge);
        label(rowSprite, fmt::format("{} / {}", room.count, room.maxPlayers), {rowWidth - 29.f, rowHeight * .65f}, .29f,
              43.f, stateColor);
        label(rowSprite,
              full           ? "FULL"
              : room.started ? "PLAYING"
                             : "JOIN",
              {rowWidth - 29.f, rowHeight * .28f}, .22f, 43.f, stateColor);
        rowSprite->setAnchorPoint({.5f, .5f});
        rowSprite->setPosition({m_rowsView.width / 2.f, listHeight - 1.5f - rowStep / 2.f - row * rowStep});
        rowSprite->setTag(static_cast<int>(kGroupTag + index));
        m_rows->addChild(rowSprite);
        if (full) previewOpacity(rowSprite, 170);
    }
    // 검색어로 걸러 방 행을 다시 그린다. 참가 가능한 방을 먼저, 가득 찬/진행 중인 방을 뒤에 두며
    // 디버그 방은 디버그 모드에서만 보인다. 갱신 시 스크롤 위치는 가능하면 유지한다.
    void renderRooms() {
        m_rows->removeAllChildrenWithCleanup(true);
        m_filtered.clear();
        std::string const query = lower(std::string(m_search->getString()));
        auto matches = [&query](std::string const& name) {
            return query.empty() || lower(name).find(query) != std::string::npos;
        };
        for (int pass = 0; pass < 2; ++pass) {
            bool const debugRooms = debug::active();
            for (size_t i = 0; i < m_rooms.size(); ++i) {
                bool const open = !m_rooms[i].guest && !m_rooms[i].started;
                if (m_rooms[i].debug && !debugRooms) continue;
                if (open == (pass == 0) && matches(m_rooms[i].name)) m_filtered.push_back(i);
            }
            for (size_t i = 0; i < m_groups.size(); ++i) {
                bool const open = m_groups[i].count < m_groups[i].maxPlayers;
                if (m_groups[i].debug && !debugRooms) continue;
                if (open == (pass == 0) && matches(m_groups[i].name)) m_filtered.push_back(kGroupTag + i);
            }
        }
        m_empty->setVisible(m_filtered.empty());
        m_empty->setString(query.empty() ? "No rooms yet. Create one!" : "No matching rooms");
        m_status->setString(fmt::format("{} room{}", m_filtered.size(), m_filtered.size() == 1 ? "" : "s").c_str());
        float const rowStep = (m_height - 105.f) / kRoomPageSize;
        float const rowHeight = rowStep - 3.f;
        float const listHeight = std::max(m_rowsView.height, rowStep * m_filtered.size() + 3.f);
        float const scrollFromTop = m_rows->getContentSize().height + m_rows->getPositionY();
        bool const keepScroll = m_rows->getContentSize().height > 0.f && m_rowsShown > 0;
        m_rows->setContentSize({m_rowsView.width, listHeight});
        m_rowsShown = m_filtered.size();
        // 1v1 방 한 줄: 방장 아바타/닉네임/방 이름, 규칙 배지, 인원과 상태(JOIN/FULL/PLAYING/LOADING).
        for (int row = 0; row < static_cast<int>(m_filtered.size()); ++row) {
            auto const index = m_filtered[static_cast<size_t>(row)];
            if (index >= kGroupTag) {
                groupRow(row, index - kGroupTag, rowStep, rowHeight, listHeight);
                continue;
            }
            auto const& room = m_rooms[index];
            bool const full = room.guest.has_value();
            auto const accent = room.debug             ? ccc3(255, 110, 90)
                                : room.rules.mode == 1 ? ccc3(163, 221, 255)
                                                       : ccc3(80, 221, 255);
            auto const stateColor = room.started ? ccc3(255, 203, 117) : full ? kMuted : ccc3(117, 247, 184);
            auto* rowSprite = CCSprite::create();
            rowSprite->setContentSize({m_listWidth - 24.f, rowHeight});
            float const rowWidth = rowSprite->getContentSize().width;
            auto* bg = NineSlice::create("square02b_001.png");
            bg->setContentSize(rowSprite->getContentSize());
            bg->setColor(ccc3(8, 24, 59));
            bg->setOpacity(245);
            bg->setPosition(rowSprite->getContentSize() / 2.f);
            rowSprite->addChild(bg, -1);
            auto* gradient = CCLayerGradient::create(row % 2 ? ccc4(25, 109, 170, 220) : ccc4(17, 81, 151, 220),
                                                     ccc4(10, 31, 70, 170), {1.f, -.3f});
            gradient->setContentSize({rowWidth - 8.f, rowHeight - 6.f});
            gradient->setPosition({4.f, 3.f});
            rowSprite->addChild(gradient, -1);
            auto* trim = CCDrawNode::create();
            trim->drawSegment({5.f, 7.f}, {5.f, rowHeight - 7.f}, 1.4f,
                              {accent.r / 255.f, accent.g / 255.f, accent.b / 255.f, .95f});
            trim->drawSegment({10.f, rowHeight - 3.f}, {rowWidth - 10.f, rowHeight - 3.f}, .45f,
                              {.65f, .86f, 1.f, .3f});
            trim->drawDot({34.f, rowHeight / 2.f}, rowHeight * .38f, {.12f, .24f, .48f, .9f});
            rowSprite->addChild(trim);
            if (room.privateRoom) {
                auto* lock = CCSprite::createWithSpriteFrameName("GJ_lockGray_001.png");
                lock->setScale(.23f);
                lock->setColor(ccc3(255, 217, 144));
                lock->setPosition({17.f, rowHeight * .28f});
                rowSprite->addChild(lock, 2);
            }
            player(rowSprite, room.host, {34.f, rowHeight / 2.f}, .60f);
            float const infoWidth = rowWidth - 122.f;
            float const nameWidth = infoWidth * .53f;
            float const badgeWidth = infoWidth - nameWidth - 7.f;
            float const badgeX = 57.f + nameWidth + 7.f;
            styleNickname(
                label(rowSprite, room.host.name, {57.f, rowHeight * .68f}, .32f, nameWidth, {255, 255, 255}, true),
                room.host.membership, room.host.cosmetics);
            styleNickname(
                label(rowSprite, room.name, {57.f, rowHeight * .31f}, .39f, nameWidth, kIce, true, "chatFont.fnt"),
                room.host.membership, room.host.cosmetics);
            auto* modeBadge = NineSlice::create("square02b_001.png");
            modeBadge->setContentSize({badgeWidth, rowHeight - 10.f});
            modeBadge->setColor(room.rules.mode == 1 ? ccc3(23, 68, 119) : ccc3(7, 56, 85));
            modeBadge->setPosition({badgeX + badgeWidth / 2.f, rowHeight / 2.f});
            rowSprite->addChild(modeBadge);
            label(rowSprite, room.rules.mode == 1 ? "PERCENT" : "ATTEMPTS",
                  {badgeX + badgeWidth / 2.f, rowHeight * .66f}, .28f, badgeWidth - 8.f, accent);
            label(rowSprite, rulesText(room.rules), {badgeX + badgeWidth / 2.f, rowHeight * .32f}, .41f,
                  badgeWidth - 8.f, kIce, false, "chatFont.fnt");
            auto* statusBadge = NineSlice::create("square02b_001.png");
            statusBadge->setContentSize({48.f, rowHeight - 9.f});
            statusBadge->setColor(room.started ? ccc3(79, 56, 31) : full ? ccc3(24, 41, 64) : ccc3(10, 66, 61));
            statusBadge->setPosition({rowWidth - 29.f, rowHeight / 2.f});
            rowSprite->addChild(statusBadge);
            label(rowSprite, full ? "2 / 2" : "1 / 2", {rowWidth - 29.f, rowHeight * .65f}, .29f, 43.f, stateColor);
            label(rowSprite,
                  room.started ? (room.launch && !room.launch->releasedAt ? "LOADING" : "PLAYING")
                  : full       ? "FULL"
                               : "JOIN",
                  {rowWidth - 29.f, rowHeight * .28f}, .22f, 43.f, stateColor);
            rowSprite->setAnchorPoint({.5f, .5f});
            rowSprite->setPosition({m_rowsView.width / 2.f, listHeight - 1.5f - rowStep / 2.f - row * rowStep});
            rowSprite->setTag(static_cast<int>(index));
            m_rows->addChild(rowSprite);
            if (full || room.started) previewOpacity(rowSprite, 170);
        }
        if (keepScroll) {
            float const lowest = m_rowsView.height - listHeight;
            m_rows->setPositionY(std::clamp(scrollFromTop - listHeight, lowest, 0.f));
        } else
            m_scroll->scrollToTop();
    }

    // 통계 패널을 다시 그려야 하는지 비교하기 위한 프로필 키.
    std::string statsKey() const {
        auto const& p = Service::get().profile();
        return fmt::format("{}:{}:{}:{}:{}:{}:{}:{}:{}:{}:{}:{}", p.name, p.icon, p.color1, p.color2, p.membership,
                           p.cosmetics.title, p.cosmetics.font, p.cosmetics.start, p.cosmetics.end, p.cosmetics.angle,
                           p.cosmetics.aura, p.recentGames);
    }
    // 내 아바타, 닉네임, 최근 승률 등을 통계 패널에 다시 그린다.
    void refreshStats() {
        m_stats->removeAllChildrenWithCleanup(true);
        auto const& profile = Service::get().profile();
        m_statsGames = profile.recentGames;
        m_statsRate = profile.winRate;
        m_statsKey = statsKey();
        float const width = m_statsPanel->getContentSize().width;
        player(m_stats, profile, {width / 2.f, m_height - 62.f}, std::min(1.4f, width / 110.f));
        styleNickname(label(m_stats, profile.name.empty() ? "Player" : profile.name, {width / 2.f, m_height - 97.f},
                            .53f, width - 20.f),
                      profile.membership, profile.cosmetics);
        titleBadge(m_stats, profile.cosmetics.title, {width / 2.f, m_height - 117.f}, .92f, width - 14.f);
    }
    void onAura(CCObject*) {
        if (auto* popup = AuraPopup::create(statsSaved())) popup->show();
    }
    void onFinisher(CCObject*) {
        if (auto* popup = FinisherPopup::create(statsSaved())) popup->show();
    }
    void onNickname(CCObject*) {
        if (auto* popup = NicknamePopup::create(statsSaved())) popup->show();
    }
    void onTitles(CCObject*) {
        if (auto* popup = TitlesPopup::create(statsSaved())) popup->show();
    }
    // 치장 팝업에서 저장이 끝나면 통계 패널을 다시 그리는 콜백.
    std::function<void()> statsSaved() {
        return [self = Weak<LobbyLayer>(this)] {
            if (auto owner = self.lock()) owner->refreshStats();
        };
    }
    // 방 만들기 팝업을 연다. 연결 전/통계 로딩 중/다른 요청 중에는 열지 않는다.
    void onCreate(CCObject*) {
        if (!m_mutating && !m_transitioning && !m_loadingStats && Service::get().connected()) {
            m_search->defocus();
            if (auto* popup = CreateRoomPopup::create()) popup->show();
        }
    }
    void onHistory(CCObject*) {
        if (!m_mutating && !m_transitioning) {
            m_search->defocus();
            if (auto* popup = HistoryPopup::create()) popup->show();
        }
    }
    // 디버그 모드에서만 쓰는 디버그 방 만들기 팝업.
    void onDebugRoom(CCObject*) {
        if (!debug::active() || m_mutating || m_transitioning || Service::get().busy()) return;
        m_search->defocus();
        if (auto* popup = CreateRoomPopup::create(true)) popup->show();
    }
    // 새로고침 버튼: 연결되지 않았으면 다시 연결하고, 연결돼 있으면 방 목록을 수동 갱신한다.
    void onRetry(CCObject*) {
        if (m_mutating || m_transitioning) return;
        if (!Service::get().connected())
            connect();
        else {
            if (!m_lastError.empty()) error(m_lastError);
            fetch(true);
        }
    }
    void onJoin(CCObject* sender) {
        if (sender) joinRoom(sender->getTag());
    }
    // 방 목록 행 클릭. tag가 kGroupTag 이상이면 Group 방이다. 가득 찼거나 이미 시작한 방은 무시하고
    // 비공개 방은 PIN 팝업, 공개 방은 바로 참가를 요청한다. 참가 중에는 다른 조작을 잠근다.
    void joinRoom(int index) {
        if (m_mutating || m_transitioning || m_loadingStats || Service::get().busy()) return;
        if (index >= static_cast<int>(kGroupTag)) {
            joinGroup(static_cast<size_t>(index) - kGroupTag);
            return;
        }
        if (index < 0 || static_cast<size_t>(index) >= m_rooms.size()) return;
        auto const room = m_rooms[static_cast<size_t>(index)];
        if (room.guest || room.started) return;
        m_search->defocus();
        if (room.privateRoom) {
            if (auto* popup = JoinRoomPopup::create(room)) popup->show();
            return;
        }
        m_mutating = true;
        enabled(m_create, false);
        enabled(m_back, false);
        m_status->setString("Joining room...");
        Service::get().joinRoom(room, {}, [self = Weak<LobbyLayer>(this)](bool success, std::string detail) {
            auto owner = self.lock();
            if (!owner || owner->m_transitioning) return;
            owner->m_mutating = false;
            if (success) {
                owner->m_transitioning = true;
                showRoom();
                return;
            }
            enabled(owner->m_create, true);
            enabled(owner->m_back, true);
            owner->m_lastError = detail;
            owner->m_status->setString("Room changed. Press Refresh to update the list.");
            error(detail);
        });
    }
    // Group 방 참가. 구조는 joinRoom과 같다 (자리가 남은 방만, 비공개면 PIN 팝업).
    void joinGroup(size_t index) {
        if (index >= m_groups.size()) return;
        auto const room = m_groups[index];
        if (room.count >= room.maxPlayers) return;
        m_search->defocus();
        if (room.privateRoom) {
            if (auto* popup = JoinGroupPopup::create(room)) popup->show();
            return;
        }
        m_mutating = true;
        enabled(m_create, false);
        enabled(m_back, false);
        m_status->setString("Joining room...");
        Service::get().joinGroup(room.id, {}, [self = Weak<LobbyLayer>(this)](bool success, std::string detail) {
            auto owner = self.lock();
            if (!owner || owner->m_transitioning) return;
            owner->m_mutating = false;
            if (success) {
                owner->m_transitioning = true;
                showRoom();
                return;
            }
            enabled(owner->m_create, true);
            enabled(owner->m_back, true);
            owner->m_lastError = detail;
            owner->m_status->setString("Room changed. Press Refresh to update the list.");
            error(detail);
        });
    }
    // 뒤로가기: 요청이 진행 중이면 무시하고 Versus 메인 화면으로 돌아간다.
    void goBack() override {
        if (m_mutating || m_transitioning || Service::get().busy()) return;
        m_transitioning = true;
        m_search->defocus();
        showVersus();
    }

public:
    static LobbyLayer* create(std::string notice) {
        auto* result = new LobbyLayer;
        if (result->init(std::move(notice))) {
            result->autorelease();
            return result;
        }
        delete result;
        return nullptr;
    }
    // 화면에 들어오면 다음 프레임에 연결 상태를 확인하고 연결돼 있으면 통계/방 목록을 받고,
    // 아니면 연결을 시작한다.
    void onEnter() override {
        SceneLayer::onEnter();
        geode::queueInMainThread([self = Weak<LobbyLayer>(this)] {
            auto owner = self.lock();
            if (!owner || !owner->isRunning() || owner->m_transitioning) return;
            if (Service::get().connected()) {
                enabled(owner->m_create, true);
                enabled(owner->m_history, true);
                owner->fetchStats();
                owner->m_status->setString("Connected. Press Refresh to load rooms.");
                owner->m_empty->setString("Press Refresh to load rooms");
                owner->autoRefreshOnce();
            } else
                owner->connect();
        });
    }
    void onEnterTransitionDidFinish() override {
        SceneLayer::onEnterTransitionDidFinish();
        showPendingNotice();
    }
    // 매 프레임: 안내/업데이트 팝업 확인, 새로고침 쿨다운 감소, 검색 입력 후 지연(디바운스)이 끝나면
    // 목록 재렌더, 프로필이 바뀌었으면 통계 패널 갱신.
    void update(float dt) override {
        showPendingNotice();
        offerUpdate();
        if (m_debugRoom) m_debugRoom->setVisible(debug::active());
        if (m_refreshCooldown > 0.f) {
            m_refreshCooldown = std::max(0.f, m_refreshCooldown - dt);
            if (m_refreshCooldown <= 0.f && !m_fetching && !m_connecting && !m_mutating && Service::get().connected())
                enabled(m_retry, true);
        }
        if (m_transitioning || m_mutating) return;
        if (m_filterDelay >= 0.f) {
            m_filterDelay -= dt;
            if (m_filterDelay < 0.f) {
                m_page = 0;
                renderRooms();
            }
        }
        if (statsKey() != m_statsKey) refreshStats();
    }
};

// 방 규칙 요약 칩(시도/퍼센트, 연습, 핵 허용 여부, 속도)을 한 줄로 그린다. 폭에 맞게 칩 폭을 줄인다.
void ruleChips(CCNode* parent, GameRules const& rules, CCPoint center, float maxWidth) {
    struct Chip {
        std::string text;
        ccColor3B color;
    };
    std::vector<Chip> chips{
        {rules.mode == 1  ? fmt::format("FIRST TO {}%", rules.targetPercent)
         : rules.practice ? std::string("FEWEST ATTEMPTS")
                          : fmt::format("{}{} {}", rules.livesFollowMap() ? "AUTO " : "", rules.attempts,
                                        rules.attempts == 1 ? "ATTEMPT" : "ATTEMPTS"),
         ccc3(255, 215, 90)},
        {rules.practice ? "PRACTICE ON" : "PRACTICE OFF", rules.practice ? ccc3(120, 230, 255) : ccc3(170, 185, 210)},
        {rules.allowHacks ? "HACKS ALLOWED" : "ANTI-CHEAT ON",
         rules.allowHacks ? ccc3(255, 150, 70) : ccc3(135, 255, 175)},
    };
    if (rules.speed != 100) chips.push_back({"SPEED " + speedText(rules.speed), ccc3(255, 120, 200)});
    int const count = static_cast<int>(chips.size());
    float const gap = 6.f;
    float const chipWidth = std::min(118.f, (maxWidth - (count - 1) * gap) / count);
    auto* row = CCNode::create();
    row->setID("room-rules"_spr);
    row->setPosition(center);
    for (int i = 0; i < count; ++i) {
        float const x = (i - (count - 1) / 2.f) * (chipWidth + gap);
        auto* bg = NineSlice::create("square02b_001.png");
        bg->setContentSize({chipWidth, 15.f});
        bg->setColor(ccc3(0, 0, 0));
        bg->setOpacity(95);
        bg->setPosition({x, 0.f});
        row->addChild(bg);
        label(row, chips[i].text, {x, 0.f}, .27f, chipWidth - 10.f, chips[i].color, false, "bigFont.fnt");
    }
    parent->addChild(row, 6);
}
// 핵 허용이 아닌 방에서 외부 치트 프로그램이 감지되면 READY를 막고 안내를 띄운다
// (전투가 시작되면 곧바로 실격되기 때문). 막았으면 true.
bool readyBlockedByCheat(GameRules const& rules) {
    if (rules.allowHacks) return false;
    auto status = integrity::activeExternalCheat(false);
    if (!status.detected) return false;
    auto message = fmt::format("Anti-cheat is on in this room. Disable <cr>{}</c> gameplay-affecting features first: "
                               "the battle would disqualify you at once.\n{}",
                               status.source, status.detail);
    FLAlertLayer::create("Ready Blocked", message.c_str(), "OK")->show();
    return true;
}

// 방 화면 가운데 영역의 버튼 줄 항목 (노드, 폭, 아래에 붙일 캡션).
struct RoomRowItem {
    CCNode* item;
    float width;
    char const* caption;
};
constexpr float kRoomRowY = 66.f, kRoomRowCaptionY = 47.f, kRoomRowGap = 7.f;
constexpr float kRoomCardBottom = 38.f;
// 화면 폭에 따른 플레이어 카드 폭 (맵 풀 방이면 조금 더 넓다).
inline float roomCardWidth(float width, bool pool = false) {
    float const card = std::max(width * .19f, std::min(width * .245f, 90.f));
    return pool ? std::max(card, width * .23f) : card;
}
// 버튼 줄을 center 기준으로 가로 정렬한다. 전체 폭이 available을 넘으면 비율대로 축소한다.
void roomActionRow(CCNode* frame, std::vector<RoomRowItem> const& row, float centerX, float available) {
    if (row.empty()) return;
    float total = -kRoomRowGap;
    for (auto const& entry : row) total += std::max(0.f, entry.width) + kRoomRowGap;
    float const fit = total > available && total > 0.f ? available / total : 1.f;
    float x = centerX - total * fit / 2.f;
    for (auto const& entry : row) {
        float const width = std::max(0.f, entry.width) * fit;
        CCPoint const at{x + width / 2.f, kRoomRowY};
        entry.item->setPosition(at);
        if (fit < 1.f) {
            entry.item->setScale(fit);
            if (auto* item = typeinfo_cast<CCMenuItemSpriteExtra*>(entry.item)) item->m_baseScale = fit;
        }
        if (entry.caption)
            label(frame, entry.caption, {at.x, kRoomRowCaptionY}, .22f, std::max(12.f, width + 6.f), kIce);
        x += width + kRoomRowGap * fit;
    }
}

// 1v1 방 화면. 두 플레이어 카드(READY/다운로드 진행도/강퇴·밴), 가운데 맵 정보 또는 랜덤 맵 필터/맵 풀,
// 규칙 칩, 이모트 줄, 시작/준비 버튼을 그리고 서버가 보내는 방 상태가 바뀌면 다시 그린다.
class RoomLayer : public SceneLayer {
    std::string m_shownDraw;
    CCNode* m_content = nullptr;
    CCNode* m_emotes = nullptr;
    std::vector<std::string> m_emoteSet;
    CCLabelBMFont* m_status = nullptr;
    float m_elapsed = 1.f;
    float m_readyPhase = 0.f;
    NineSlice* m_readyGlow = nullptr;
    ButtonSprite* m_readySprite = nullptr;
    float m_emoteCooldown = 0.f;
    bool m_pending = false;
    bool m_sendingEmote = false;
    bool m_downloading = false, m_readyAfterDownload = false;
    bool m_cached = false;
    bool m_renderQueued = false;
    uint64_t m_downloadGeneration = 0;
    int64_t m_mapID = 0;
    std::string m_cacheError;
    std::string m_seenEmote;
    std::string m_signature;
    std::string m_returnedBattle;
    std::string m_readyRoomID;
    bool m_opponentReadyKnown = false;
    bool m_lastOpponentReady = false;
    // 각 좌석(0=나, 1=상대)의 맵 다운로드 진행 표시 위젯.
    struct DownloadWidgets {
        CCLabelBMFont* caption = nullptr;
        CCLayerColor* fill = nullptr;
        float width = 0.f;
        std::string key;
    } m_downloads[2];
    float m_progressElapsed = 0.f;

    // 좌석별 맵 다운로드 진행도를 갱신한다. 내 쪽은 로컬 상태를 직접 읽고, 상대 쪽은 서버를 통해 공유된
    // 값(같은 유저/같은 레벨일 때만 유효)을 쓴다. 25초 넘게 갱신이 없으면 "Sync delayed"로 표시한다.
    void updateDownloads(RoomInfo const& room) {
        bool const ownHost = Service::get().isHost();
        for (int i = 0; i < 2; ++i) {
            auto& widget = m_downloads[i];
            if (!widget.caption || !widget.fill) continue;
            bool const seatHost = i == 0 ? ownHost : !ownHost;
            auto const* profile = seatHost ? &room.host : (room.guest ? &*room.guest : nullptr);
            auto const& shared = seatHost ? room.hostDownload : room.guestDownload;
            bool known = i == 0 || (profile && shared.uid == profile->uid && shared.levelId == room.level.id);
            MapDownloadProgress progress;
            if (i == 0 && room.level.id > 0)
                progress = mapDownloadProgress(room.level.id);
            else if (known) {
                progress.mapReady = shared.mapReady;
                progress.percent = std::clamp(shared.percent, 0, 100);
                progress.songsDone = shared.songsDone;
                progress.songsTotal = shared.songsTotal;
                progress.soundsDone = shared.soundsDone;
                progress.soundsTotal = shared.soundsTotal;
            }
            bool const stale =
                i != 0 && known && !progress.complete() && Service::get().serverNow() - shared.updatedAt > 25000;
            auto title = room.level.id <= 0 ? std::string{}
                         : !known           ? "Syncing..."
                         : stale            ? "Sync delayed"
                                 : fmt::format("{} {}%", progress.complete() ? "Ready" : "Download", progress.percent);
            auto key = title + ":" + std::to_string(progress.percent);
            if (widget.key == key) continue;
            widget.key = std::move(key);
            widget.caption->setString(title.c_str());
            widget.caption->setScale(
                std::min(.28f, widget.width / std::max(1.f, widget.caption->getContentSize().width)));
            auto color = progress.complete() && known ? ccc3(135, 255, 175) : stale ? ccc3(255, 210, 125) : kIce;
            widget.caption->setColor(color);
            widget.fill->setColor(color);
            widget.fill->setScaleX(room.level.id > 0 && known ? progress.percent / 100.f : 0.f);
        }
    }

    // 같은 프레임에 render 요청이 여러 번 와도 메인 스레드에서 한 번만 다시 그리게 한다.
    void requestRender() {
        if (m_renderQueued || m_transitioning) return;
        m_renderQueued = true;
        Loader::get()->queueInMainThread([self = Weak<RoomLayer>(this)] {
            if (auto owner = self.lock()) {
                owner->m_renderQueued = false;
                if (!owner->m_transitioning) owner->render();
            }
        });
    }

    // READY 버튼 테두리를 RGB 사인파로 색이 순환하며 깜박이게 한다 (m_readyPhase는 update에서 증가).
    void animateReady() {
        if (!m_readyGlow || !m_readySprite) return;
        auto channel = [this](float offset) {
            return static_cast<GLubyte>(155.f + 100.f * std::sin(m_readyPhase + offset));
        };
        auto color = ccc3(channel(0.f), channel(2.0944f), channel(4.1888f));
        m_readyGlow->setColor(color);
        m_readyGlow->setOpacity(static_cast<GLubyte>(130.f + 55.f * std::sin(m_readyPhase * 2.f)));
        m_readySprite->m_BGSprite->setColor(color);
    }

    // 방의 맵을 아직 받지 않았으면 다운로드를 시작한다. 세대 번호(generation)로 맵이 바뀐 뒤 도착한
    // 이전 응답은 버린다. 받으면 전투용 에셋을 미리 로드한다.
    void beginDownload() {
        auto const& room = Service::get().room();
        if (!room || room->level.id <= 0 || m_downloading || mapCached(room->level.id)) {
            if (room && room->level.id > 0) m_cached = mapCached(room->level.id);
            return;
        }
        m_downloading = true;
        m_cacheError.clear();
        auto const levelID = room->level.id;
        auto const generation = ++m_downloadGeneration;
        ensureMapCached(levelID, [self = Weak<RoomLayer>(this), levelID, generation](bool success, std::string detail) {
            auto owner = self.lock();
            if (!owner || owner->m_mapID != levelID || owner->m_downloadGeneration != generation) return;
            owner->m_downloading = false;
            owner->m_cached = success && mapCached(levelID);
            if (owner->m_cached) preloadBattleAssets();
            owner->m_cacheError = owner->m_cached ? std::string() : std::move(detail);
            bool const ready = std::exchange(owner->m_readyAfterDownload, false);
            auto const& room = Service::get().room();
            if (ready && owner->m_cached && mapAudioSkipped(levelID) && room &&
                !(Service::get().isHost() ? room->hostReady : room->guestReady))
                owner->onReady(nullptr);
            owner->requestRender();
        });
    }

    // 방에서 온 이모트/퀵챗을 말풍선으로 띄운다. 이미 본 nonce이거나 너무 오래된(kFreshMs 초과) 것은 무시하고,
    // 보낸 사람 쪽(왼쪽/오른쪽 카드)에서 나타나게 한다.
    void showRoomEmote(RoomInfo const& room) {
        if (!room.emote || room.emote->nonce.empty() || room.emote->nonce == m_seenEmote) return;
        m_seenEmote = room.emote->nonce;
        if (Service::get().serverNow() - room.emote->at > emotes::kFreshMs) return;
        bool const fromHost = room.emote->uid == room.host.uid;
        bool const mineIsHost = Service::get().isHost();
        bool const onLeft = fromHost == mineIsHost;
        float const width = std::min(550.f, m_window.width - 44.f);
        float const height = std::min(250.f, m_window.height - 65.f);
        float const frameX = (m_window.width - width) / 2.f;
        float const frameY = (m_window.height - height) / 2.f - 8.f;
        float const cardWidth = roomCardWidth(width, room.mapPool.has_value());
        bool const chat = room.emote->kind == "chat";
        auto* bubble = CCNodeRGBA::create();
        bubble->setCascadeOpacityEnabled(true);
        bubble->setPosition(
            {frameX + (onLeft ? 12.f + cardWidth + (chat ? 35.f : -3.f) : width - cardWidth - (chat ? 35.f : 9.f)),
             frameY + height - 70.f});
        bubble->setScale(.15f);
        m_emotes->addChild(bubble);
        auto* bg = NineSlice::create("square02b_001.png");
        bg->setContentSize({chat ? 118.f : 48.f, 43.f});
        bg->setColor(ccc3(255, 255, 255));
        bg->setPosition({0.f, 0.f});
        bubble->addChild(bg);
        CCNode* animated = nullptr;
        if (chat) {
            auto* words =
                label(bubble, room.emote->text, {0.f, 1.f}, .48f, 106.f, ccc3(22, 35, 55), false, "bigFont.fnt");
            animated = words;
            auto* tail = CCDrawNode::create();
            CCPoint points[] = {
                {onLeft ? -49.f : 49.f, -18.f}, {onLeft ? -61.f : 61.f, -25.f}, {onLeft ? -43.f : 43.f, -12.f}};
            tail->drawPolygon(points, 3, {1.f, 1.f, 1.f, 1.f}, 0, {1.f, 1.f, 1.f, 1.f});
            bubble->addChild(tail, -1);
        } else {
            auto* art = emoteArt(room.emote->kind);
            art->setScale(.85f);
            bubble->addChild(art, 2);
            animated = art;
            emoteBurst(m_emotes, bubble->getPosition() + CCPoint{0.f, 8.f}, room.emote->kind);
        }
        bubble->runAction(
            CCSequence::create(CCEaseBackOut::create(CCScaleTo::create(.22f, 1.f)),
                               CCEaseSineInOut::create(CCMoveBy::create(.22f, {0.f, 8.f})), CCDelayTime::create(1.25f),
                               CCSpawn::create(CCFadeOut::create(.25f), CCScaleTo::create(.25f, .7f), nullptr),
                               CCRemoveSelf::create(), nullptr));
        if (animated && !chat)
            animated->runAction(
                CCRepeat::create(CCSequence::create(CCEaseSineInOut::create(CCRotateTo::create(.12f, -10.f)),
                                                    CCEaseSineInOut::create(CCRotateTo::create(.12f, 10.f)),
                                                    CCEaseSineInOut::create(CCRotateTo::create(.12f, 0.f)), nullptr),
                                 2));
    }

    // 씬 초기화 후 곧바로 현재 방 상태를 그린다. 이미 있던 이모트는 다시 재생하지 않도록 nonce만 기억한다.
    bool init() {
        if (!initScene("Versus Room")) return false;
        addAudioButton(false);
        setID("versus-room"_spr);
        m_content = CCNode::create();
        addChild(m_content);
        m_emotes = CCNode::create();
        addChild(m_emotes, 30);
        if (auto const& room = Service::get().room(); room && room->emote) m_seenEmote = room->emote->nonce;
        m_status =
            label(this, "", {m_window.width / 2.f, 10.f}, .52f, m_window.width - 40.f, kIce, false, "chatFont.fnt");
        render();
        return true;
    }
    // 방 상태 중 화면에 영향을 주는 값들을 문자열 하나로 합친 서명. 이전과 같으면 다시 그릴 필요가 없다.
    std::string signature(RoomInfo const& room) {
        auto profileKey = [](PlayerProfile const& profile) {
            return fmt::format("{}:{}:{}:{}:{}:{:.1f}:{}:{}:{}:{}:{}:{}:{}:{}", profile.uid, profile.name, profile.icon,
                               profile.color1, profile.color2, profile.winRate, profile.recentGames, profile.membership,
                               profile.cosmetics.start, profile.cosmetics.end, profile.cosmetics.angle,
                               profile.cosmetics.aura, profile.cosmetics.title, profile.cosmetics.font);
        };
        auto const launchKey =
            room.launch ? fmt::format("{}:{}:{}:{}:{}", room.launch->id, room.launch->requestedAt,
                                      room.launch->releasedAt, room.launch->hostLoaded, room.launch->guestLoaded)
                        : "";
        return fmt::format("{}:{}:{}:{}:{}:{}:{}:{}:{}:{}:{}:{}:{}:{}:{}:{}:{}:{}:{}:{}", room.id, room.name,
                           profileKey(room.host), room.guest ? profileKey(*room.guest) : "", room.level.id,
                           room.level.name, room.level.difficulty, room.level.stars, room.level.demon,
                           room.level.autoLevel, room.started, m_pending, Service::get().busy(), room.guestReady,
                           launchKey, room.hostReady, rulesText(room.rules), m_cached, m_downloading, m_cacheError) +
               fmt::format(":{}:{}:{}:{}:{}", room.mapSelection.random, room.mapSelection.mask,
                           room.mapSelection.platformer, room.mapDraw ? room.mapDraw->id : "",
                           room.mapDraw && room.mapDraw->settled) +
               fmt::format(":{}:{}:{}:{}", room.mapSelection.lengths, room.mapSelection.rated,
                           room.mapSelection.twoPlayer, room.mapSelection.minLikes) +
               fmt::format(":{}:{}:{}:{}:{}", room.level.creator, room.level.featureState, roomWins(room, true),
                           roomWins(room, false), room.level.userCoins) +
               (room.battle ? fmt::format(":{}:{}:{}", room.battle->finishedAt, room.battle->hostReturned,
                                          room.battle->guestReturned)
                            : "") +
               hackSignature(room) +
               (room.mapPool ? fmt::format(":pool{}:{}:{}", room.mapPool->maps.size(), room.mapPool->permit.size(),
                                           room.mapPool->settings.random)
                             : std::string());
    }
    // 방 참가자별 핵 감지 목록을 서명용 문자열로 만든다.
    static std::string hackSignature(RoomInfo const& room) {
        std::string key;
        for (auto const& [uid, list] : room.hacks) {
            key += "|" + uid;
            for (auto const& [code, features] : list) key += ":" + code + "=" + features;
        }
        return key;
    }
    // 유저의 핵 목록을 표시용 문자열로 만든다 (없으면 "No hacks").
    static std::string hackText(RoomInfo const& room, std::string const& uid) {
        auto it = room.hacks.find(uid);
        return it == room.hacks.end() ? std::string("No hacks") : integrity::hackListText(it->second);
    }
    // 플레이어 카드 하나: 승수, 아바타, READY 표시, 닉네임(누르면 프로필), 칭호, 맵 다운로드 막대.
    // 방장이면 상대 카드에 Kick/Ban 버튼이 붙는다. 상대가 없으면 "Waiting..." 자리 표시를 그린다.
    void playerCard(CCNode* parent, std::optional<PlayerProfile> const& profile, CCPoint origin, CCSize size, bool host,
                    bool ready, int wins, int seat, bool checkingResult = false) {
        auto* card = panel(parent, origin, size, false);
        auto* score = label(parent, fmt::format("Win : {}", wins),
                            {origin.x + size.width / 2.f, parent->getContentSize().height - 17.f}, .56f,
                            size.width - 24.f, ccWHITE, false, "goldFont.fnt");
        score->setID(host ? "host-room-wins"_spr : "guest-room-wins"_spr);
        if (profile) {
            float buttonsLeft = size.width;
            if (!host && Service::get().isHost() && Service::get().room() && !Service::get().room()->started &&
                !Service::get().debugRoom()) {
                auto* actions = menu(card);
                auto* kick = button(actions, this, menu_selector(RoomLayer::onKick), "Kick",
                                    {size.width - 23.f, size.height - 43.f}, .3f);
                enabled(kick, !m_pending && !Service::get().busy());
                auto* ban = button(actions, this, menu_selector(RoomLayer::onBan), "Ban",
                                   {size.width - 23.f, size.height - 58.f}, .3f, "GJ_button_06.png");
                ban->setID("room-ban-button"_spr);
                enabled(ban, !m_pending && !Service::get().busy());
                buttonsLeft =
                    size.width - 23.f - std::max(kick->getContentSize().width, ban->getContentSize().width) / 2.f;
            }
            float const iconScale = 1.3f * std::clamp(size.width / 128.f, .8f, 1.f);
            float const iconX = std::min(size.width / 2.f, buttonsLeft - 2.f - 15.5f * iconScale);
            if (checkingResult) {
                label(card, "CHECKING RESULT...", {size.width / 2.f, size.height - 46.f}, .32f, size.width - 12.f,
                      kIce);
            } else
                player(card, *profile, {iconX, size.height - 46.f}, iconScale);
            readyBadge(card, {size.width / 2.f, size.height - 73.f}, ready);
            auto* nameSprite = CCSprite::create();
            nameSprite->setContentSize({size.width - 12.f, 21.f});
            styleNickname(label(nameSprite, profile->name, nameSprite->getContentSize() / 2.f, .43f, size.width - 18.f),
                          profile->membership, profile->cosmetics);
            auto* name = CCMenuItemSpriteExtra::create(nameSprite, this, menu_selector(RoomLayer::onPlayer));
            name->setTag(host ? 0 : 1);
            name->setPosition({size.width / 2.f, size.height - 94.f});
            menu(card)->addChild(name);
            titleBadge(card, profile->cosmetics.title, {size.width / 2.f, size.height - 112.f}, .9f, size.width - 14.f);
            auto& progress = m_downloads[seat];
            progress.width = size.width - 12.f;
            progress.caption = label(card, "", {size.width / 2.f, 41.f}, .28f, progress.width, kIce);
            auto* track = CCLayerColor::create(ccc4(15, 34, 65, 255), progress.width, 4.f);
            track->setPosition({6.f, 30.f});
            card->addChild(track);
            progress.fill = CCLayerColor::create(ccc4(155, 225, 255, 255), progress.width, 4.f);
            progress.fill->setAnchorPoint({0.f, 0.f});
            progress.fill->ignoreAnchorPointForPosition(false);
            progress.fill->setPosition({6.f, 30.f});
            progress.fill->setScaleX(0.f);
            card->addChild(progress.fill);
        } else {
            auto* placeholder = CCDrawNode::create();
            placeholder->setPosition({size.width / 2.f, size.height - 48.f});
            placeholder->drawDot({0.f, 8.f}, 8.f, {.38f, .61f, .87f, .65f});
            placeholder->drawSegment({-9.f, -12.f}, {9.f, -12.f}, 8.f, {.38f, .61f, .87f, .65f});
            card->addChild(placeholder);
            label(card, "Waiting...", {size.width / 2.f, size.height - 94.f}, .40f, size.width - 18.f, kIce);
        }
    }
    // 현재 방 상태 전체를 다시 그린다. 상대의 READY 순간에는 효과음을 내고, 방의 레벨이 바뀌면 다운로드
    // 상태를 초기화한 뒤 새로 받는다. 이후 카드/규칙/맵 영역/버튼/이모트 줄을 차례로 만든다.
    void render() {
        auto const& current = Service::get().room();
        if (!current) return;
        auto room = *current;
        hideSpinningDraw(room, Service::get().serverNow());
        bool const host = Service::get().isHost();
        bool const opponentPresent = host ? room.guest.has_value() : true;
        bool const currentOpponentReady = host ? room.guestReady : room.hostReady;
        if (m_readyRoomID != room.id) {
            m_readyRoomID = room.id;
            m_opponentReadyKnown = true;
            m_lastOpponentReady = currentOpponentReady;
        } else if (m_opponentReadyKnown) {
            if (opponentPresent && !m_lastOpponentReady && currentOpponentReady && !room.started)
                audio::play(audio::Cue::ReadyLock);
            m_lastOpponentReady = currentOpponentReady;
        }
        if (m_mapID != room.level.id) {
            ++m_downloadGeneration;
            m_readyAfterDownload = false;
            m_mapID = room.level.id;
            m_cached = room.level.id > 0 && mapCached(room.level.id);
            m_downloading = false;
            m_cacheError.clear();
            if (room.level.id > 0 && !m_cached)
                beginDownload();
            else if (m_cached)
                preloadBattleAssets();
        } else if (room.level.id > 0)
            m_cached = mapCached(room.level.id);
        m_signature = signature(room);
        m_readyGlow = nullptr;
        m_readySprite = nullptr;
        for (auto& widgets : m_downloads) widgets = {};
        m_content->removeAllChildrenWithCleanup(true);
        float const width = std::min(550.f, m_window.width - 44.f);
        float const height = std::min(250.f, m_window.height - 65.f);
        auto* frame =
            panel(m_content, {(m_window.width - width) / 2.f, (m_window.height - height) / 2.f - 8.f}, {width, height});
        styleNickname(label(frame, room.name, {width / 2.f, height - 17.f}, .46f,
                            width - 2.f * roomCardWidth(width, room.mapPool.has_value()) - 30.f, kIce),
                      room.host.membership, room.host.cosmetics);
        ruleChips(frame, room.rules, {width / 2.f, height - 33.f}, width - 80.f);
        // 핵 비허용 방(랭크 방 제외)에서 지금 감지된 외부 치트 기능이 있으면 끄라는 경고를 보여 준다.
        if (!room.rules.allowHacks && !room.id.starts_with("ranked-")) {
            std::string active;
            for (auto const& status : integrity::activeExternalCheats(false))
                if (status.detected) {
                    auto summary = integrity::featureSummary(status.code, status.features);
                    if (!summary.empty()) active += (active.empty() ? "" : ", ") + summary;
                }
            if (!active.empty()) {
                auto* warning = label(frame, "Anti-cheat on - turn off: " + active, {width / 2.f, height - 48.f}, .3f,
                                      width - 2.f * roomCardWidth(width, room.mapPool.has_value()) - 40.f,
                                      ccc3(255, 92, 74), false, "chatFont.fnt");
                warning->setZOrder(7);
            }
        }
        if (room.privateRoom) {
            auto* lock = CCSprite::createWithSpriteFrameName("GJ_lockGray_001.png");
            lock->setScale(.38f);
            lock->setPosition({18.f, height - 18.f});
            frame->addChild(lock);
        }
        float const cardWidth = roomCardWidth(width, room.mapPool.has_value());
        float const cardBottom = kRoomCardBottom, cardTop = height - 45.f, cardHeight = cardTop - cardBottom;
        std::optional<PlayerProfile> mine = host ? std::optional<PlayerProfile>(room.host) : room.guest;
        std::optional<PlayerProfile> opponent = host ? room.guest : std::optional<PlayerProfile>(room.host);
        if (!mine) mine = Service::get().profile();
        bool const myReady = host ? room.hostReady : room.guestReady;
        bool const opponentReady = host ? room.guestReady : room.hostReady;
        playerCard(frame, mine, {12.f, cardBottom}, {cardWidth, cardHeight}, host, myReady, roomWins(room, host), 0);
        bool const opponentChecking = room.battle && room.battle->finishedAt > 0 &&
                                      (host ? !room.battle->guestReturned : !room.battle->hostReturned);
        playerCard(frame, opponent, {width - cardWidth - 12.f, cardBottom}, {cardWidth, cardHeight}, !host,
                   opponentReady, roomWins(room, !host), 1, opponentChecking);
        // 핵 허용 방이면 각 플레이어가 사용 중인 핵 목록을 카드 위에 표시한다.
        if (room.rules.allowHacks) {
            auto hackLine = [&](std::optional<PlayerProfile> const& who, float x) {
                if (!who) return;
                auto const text = hackText(room, who->uid);
                auto* line =
                    label(frame, text, {x, cardTop - 10.f}, .3f, cardWidth - 8.f,
                          text == "No hacks" ? ccc3(150, 200, 160) : ccc3(255, 170, 90), false, "chatFont.fnt");
                line->setZOrder(6);
            };
            hackLine(mine, 12.f + cardWidth / 2.f);
            hackLine(opponent, width - cardWidth / 2.f - 12.f);
        }
        // LDM(저화질) 토글. 값은 모드 설정에 저장되고 전투 시작 전에만 바꿀 수 있다.
        float const centerWidth = width - 2.f * cardWidth - 40.f;
        float const centerX = width / 2.f;
        auto* actions = menu(frame);
        {
            auto* ldm = CCMenuItemToggler::createWithStandardSprites(this, menu_selector(RoomLayer::onLdm), .4f);
            ldm->setID("room-ldm-toggle"_spr);
            ldm->toggle(Mod::get()->getSavedValue<bool>("room-ldm", false));
            float const boxW = ldm->getContentSize().width * .4f;
            auto* caption = label(frame, "LDM", {}, .3f, 40.f, kIce, true);
            float const total = boxW + 4.f + caption->getScaledContentSize().width;
            float const left = 12.f + cardWidth / 2.f - total / 2.f;
            ldm->setPosition({left + boxW / 2.f, cardBottom + 12.f});
            caption->setPosition({left + boxW + 4.f, cardBottom + 12.f});
            caption->setZOrder(6);
            actions->addChild(ldm);
            ldm->setEnabled(!room.started);
        }
        // 맵 풀 방: 방장이 상대에게 풀 편집 권한(permit)을 줄 수 있는 버튼.
        if (room.mapPool && host && room.guest) {
            bool const allowed = room.mapPool->permitted(room.guest->uid, room.host.uid);
            auto* permit = CCMenuItemSpriteExtra::create(mappool::permitSprite(allowed, 24.f), this,
                                                         menu_selector(RoomLayer::onPermitGuest));
            permit->setID("room-pool-permit"_spr);
            permit->setPosition({width - 12.f - cardWidth / 2.f, cardBottom + 12.f});
            actions->addChild(permit);
            enabled(permit, !m_pending && !room.started);
        }
        // 가운데 영역: 맵 풀 목록 / 랜덤 맵 필터 카드 / 선택된 맵 정보(난이도 얼굴, 이름, 제작자, 별, 코인) 중 하나.
        if (room.mapPool) {
            auto* list = MapPoolList::create(
                {centerWidth + 6.f, cardHeight},
                [self = Weak<RoomLayer>(this)] {
                    if (auto owner = self.lock()) owner->onChoose(nullptr);
                },
                [self = Weak<RoomLayer>(this)] {
                    if (auto owner = self.lock()) owner->onRules(nullptr);
                });
            list->setPosition({centerX - (centerWidth + 6.f) / 2.f, cardBottom});
            frame->addChild(list, 3);
        } else if (!room.level.id && room.mapSelection.random) {
            label(frame, "Random Map", {centerX, height - 59.f}, .6f, centerWidth - 10.f)
                ->setID("random-map-title"_spr);
            bool const stacked = centerWidth < 220.f;
            randomFilterCard(frame, room.mapSelection,
                             CCRect(centerX - centerWidth / 2.f, stacked ? 90.f : 96.f, centerWidth,
                                    (height - 70.f) - (stacked ? 90.f : 96.f)),
                             stacked);
        } else {
            if (room.level.id) {
                auto* selected = NineSlice::create("square02b_001.png");
                selected->setID("selected-map-highlight"_spr);
                selected->setContentSize({centerWidth + 6.f, 114.f});
                selected->setPosition({centerX, height - 105.f});
                selected->setColor(ccc3(255, 150, 50));
                selected->setOpacity(38);
                frame->addChild(selected);
            }
            auto* face = mapDifficulty(room.level);
            if (face) {
                face->setScale(.90f);
                face->setPosition({centerX, height - 77.f});
                frame->addChild(face);
            }
            label(frame, room.level.id ? room.level.name : "Choose a map",
                  {centerX, height - (room.level.id ? 120.f : 114.f)}, room.level.id ? .36f : .45f, centerWidth);
            if (room.level.id) {
                label(frame, room.level.creator.empty() ? "Unknown creator" : room.level.creator,
                      {centerX, height - 130.5f}, .34f, centerWidth, ccWHITE, false, "goldFont.fnt");
                starRating(frame, std::to_string(room.level.stars),
                           {centerX - (room.level.userCoins > 0 ? 24.f : 0.f), height - 106.f}, .34f,
                           centerWidth * .5f);
                mapCoinCount(frame, room.level, {centerX + 24.f, height - 106.f}, .3f);
            }
        }
        // READY 버튼. 맵이 정해졌는지, 내려받았는지, 요청 중인지에 따라 문구와 활성 상태를 정한다
        // (readyControl). 받은 맵이면 눌러야 할 때 테두리가 깜박인다.
        auto const control =
            readyControl(room.started || drawingMap(room), m_pending, host, room.level.id > 0 || needsRandomDraw(room),
                         m_cached || needsRandomDraw(room), m_downloading, myReady, mapAudioSkipped(room.level.id));
        if (!(room.mapPool && control.action == ReadyAction::ChooseMap)) {
            auto* ready =
                button(actions, this, menu_selector(RoomLayer::onReady), control.caption,
                       {12.f + cardWidth / 2.f, 24.f}, .55f, myReady ? "GJ_button_04.png" : "GJ_button_01.png");
            ready->setID("ready-button"_spr);
            if (ready->getContentSize().width > cardWidth - 6.f) {
                auto* sprite = static_cast<ButtonSprite*>(ready->getNormalImage());
                sprite->setScale(sprite->getScale() * (cardWidth - 6.f) / ready->getContentSize().width);
                ready->updateSprite();
            }
            enabled(ready, control.enabled());
            if (!myReady && m_cached && ready->isEnabled()) {
                m_readySprite = static_cast<ButtonSprite*>(ready->getNormalImage());
                m_readyGlow = NineSlice::create("square02b_001.png");
                m_readyGlow->setContentSize(ready->getContentSize() + CCSize{8.f, 7.f});
                m_readyGlow->setPosition(ready->getPosition());
                frame->addChild(m_readyGlow, 4);
                animateReady();
            }
        }
        // 방장 전용 버튼: Start/Roll(시작 조건: 상대 있음, 둘 다 READY, 맵 준비 완료 또는 랜덤 추첨),
        // Map Mode, Re-Roll. 공통으로 다운로드 버튼과 규칙(Game) 톱니 버튼이 중앙 줄에 추가된다.
        std::vector<RoomRowItem> row;
        if (host) {
            auto* start = button(actions, this, menu_selector(RoomLayer::onStart),
                                 room.battle && room.battle->finishedAt ? "Finishing"
                                 : room.started                         ? "Preparing"
                                 : needsRandomDraw(room)                ? "Roll"
                                                                        : "Start",
                                 {width - cardWidth / 2.f - 12.f, 24.f}, .57f);
            if (start->getContentSize().width > cardWidth - 6.f) {
                auto* sprite = static_cast<ButtonSprite*>(start->getNormalImage());
                sprite->setScale(sprite->getScale() * (cardWidth - 6.f) / start->getContentSize().width);
                start->updateSprite();
            }
            enabled(start, !m_pending && !Service::get().busy() && !room.started && room.guest.has_value() &&
                               !drawingMap(room) && room.hostReady && room.guestReady &&
                               ((room.level.id > 0 && m_cached) || needsRandomDraw(room)));
            if (!room.mapPool) {
                auto* choose =
                    button(actions, this, menu_selector(RoomLayer::onChoose), "Map Mode", {}, .46f, "GJ_button_04.png");
                enabled(choose, !m_pending && !Service::get().busy() && !room.started && !drawingMap(room));
                row.push_back({choose, choose->getContentSize().width, nullptr});
                if (canRerollMap(room)) {
                    auto* reroll = button(actions, this, menu_selector(RoomLayer::onReroll), "Re-Roll", {}, .40f,
                                          "GJ_button_04.png");
                    reroll->setID("reroll-button"_spr);
                    enabled(reroll, !m_pending && !Service::get().busy());
                    row.push_back({reroll, reroll->getContentSize().width, nullptr});
                }
            }
        }
        if (!room.mapPool) {
            if (room.level.id > 0 && !m_cached) {
                auto* download = button(actions, this, menu_selector(RoomLayer::onDownload),
                                        m_downloading ? "Downloading..." : "Download", {}, .40f, "GJ_button_04.png");
                enabled(download, !m_downloading && !room.started);
                row.push_back({download, download->getContentSize().width, nullptr});
            }
            auto* gear =
                iconButton(actions, this, menu_selector(RoomLayer::onRules), "GJ_optionsBtn_001.png", {}, 26.f);
            enabled(gear, !m_pending);
            gear->setID("room-rules-button"_spr);
            row.push_back({gear, gear->getContentSize().width, "Game"});
            roomActionRow(frame, row, centerX, centerWidth - 4.f);
        }

        // 퀵바 이모트 버튼. 상대가 들어와 있고 방이 시작 전일 때만 보낼 수 있다.
        m_emoteSet = emotes::quickSet(Service::get().profile().membership);
        int const emoteCount = static_cast<int>(m_emoteSet.size());
        float const pitch = std::min(30.f, (width - 2.f * cardWidth - 32.f) / static_cast<float>(emoteCount + 2));
        float const tile = std::min(29.f, pitch - 1.f);
        float const signalStart = centerX - (emoteCount + 1) * pitch / 2.f;
        bool const signalsOpen = room.guest.has_value() && !room.started;
        for (int i = 0; i < emoteCount; ++i) {
            auto* item = CCMenuItemSpriteExtra::create(emoteTile(m_emoteSet[i], tile, ccc3(5, 27, 73)), this,
                                                       menu_selector(RoomLayer::onEmote));
            item->setTag(i);
            item->setPosition({signalStart + i * pitch, 22.f});
            actions->addChild(item);
            enabled(item, signalsOpen);
        }
        {
            auto* sprite = CCSprite::create();
            sprite->setContentSize({tile, tile});
            auto* bg = NineSlice::create("square02b_001.png");
            bg->setContentSize({tile, tile});
            bg->setColor(ccc3(70, 30, 105));
            bg->setPosition({tile / 2.f, tile / 2.f});
            sprite->addChild(bg);
            auto* dots = CCDrawNode::create();
            float const step = tile * 10.f / 29.f;
            for (int i = 0; i < 4; ++i)
                dots->drawDot({tile / 2.f + ((i % 2) - .5f) * step, tile / 2.f + ((i / 2) - .5f) * step},
                              3.4f * tile / 29.f, {1.f, .85f, .4f, 1.f});
            sprite->addChild(dots);
            // 이모트 전체 목록/퀵바 편집을 여는 "더보기" 버튼 (점 4개 아이콘).
            auto* more = CCMenuItemSpriteExtra::create(sprite, this, menu_selector(RoomLayer::onEmotePicker));
            more->setID("emote-picker"_spr);
            more->setPosition({signalStart + emoteCount * pitch, 22.f});
            actions->addChild(more);
        }
        // 오른쪽 끝의 퀵챗 버튼.
        auto* chat = iconButton(actions, this, menu_selector(RoomLayer::onChat), "GJ_chatBtn_001.png",
                                {signalStart + (emoteCount + 1) * pitch, 22.f}, tile - 2.f);
        enabled(chat, signalsOpen);
        // 하단 상태 문구: 맵 캐시 오류, 결과 확인 대기, 준비 중, 맵 추첨 중 중 하나.
        std::string const status = !m_cacheError.empty()                    ? m_cacheError + " Tap Download to retry."
                                   : room.battle && room.battle->finishedAt ? "Waiting for players to return..."
                                   : room.started                           ? "Preparing..."
                                   : drawingMap(room)                       ? "Choosing map..."
                                                                            : "";
        m_status->setString(status.c_str());
        updateDownloads(room);
    }
    // Re-Roll: 방장이 현재 맵 추첨을 다시 돌린다.
    void onReroll(CCObject*) {
        auto const& room = Service::get().room();
        if (m_pending || m_transitioning || Service::get().busy() || !Service::get().isHost() || !room ||
            !canRerollMap(*room))
            return;
        auto const id = room->mapDraw->id;
        m_pending = true;
        requestRender();
        Service::get().rerollMap(id, [self = Weak<RoomLayer>(this)](bool ok, std::string detail) {
            if (auto owner = self.lock()) {
                owner->m_pending = false;
                owner->requestRender();
                if (!ok) error(detail);
            }
        });
    }
    void onRules(CCObject*) {
        auto const& room = Service::get().room();
        if (m_pending || !room) return;
        if (auto* popup = GameRulesPopup::create(*room)) popup->show();
    }
    // LDM 설정 저장. 방이 이미 시작됐으면 바꾸지 않고 화면만 되돌린다.
    void onLdm(CCObject*) {
        auto const& room = Service::get().room();
        if (room && room->started) {
            requestRender();
            return;
        }
        Mod::get()->setSavedValue("room-ldm", !Mod::get()->getSavedValue<bool>("room-ldm", false));
    }
    // 이름을 누르면 해당 플레이어 팝업. 디버그 방의 상대 자리는 가짜 자리라서 안내만 한다.
    void onPlayer(CCObject* sender) {
        auto const& room = Service::get().room();
        if (!room || m_transitioning) return;
        if (sender->getTag() != 0 && Service::get().debugRoom()) {
            Notification::create("Debug Room: this seat is a stand-in. It never plays.", NotificationIcon::Info, 2.5f)
                ->show();
            return;
        }
        auto const* profile = sender->getTag() == 0 ? &room->host : (room->guest ? &*room->guest : nullptr);
        if (profile)
            if (auto* popup = PlayerPopup::create(*profile)) popup->show();
    }
    // 맵 풀 방에서 방장이 상대의 풀 편집 허용 여부를 토글한다.
    void onPermitGuest(CCObject*) {
        auto const& room = Service::get().room();
        if (m_pending || !room || !room->mapPool || !room->guest || !Service::get().isHost() || room->started) return;
        bool const allowed = room->mapPool->permitted(room->guest->uid, room->host.uid);
        m_pending = true;
        requestRender();
        Service::get().permitMapPool(room->guest->uid, !allowed,
                                     [self = Weak<RoomLayer>(this)](bool ok, std::string detail) {
                                         if (auto owner = self.lock()) {
                                             owner->m_pending = false;
                                             owner->requestRender();
                                         }
                                         if (!ok) error(detail);
                                     });
    }
    void onKick(CCObject*) { askRemove(false); }
    void onBan(CCObject*) { askRemove(true); }
    // 방장이 상대를 내보내거나(Kick) 밴(방이 열려 있는 동안 재입장 불가)한다. 확인 팝업을 거친다.
    void askRemove(bool ban) {
        auto const& room = Service::get().room();
        if (m_pending || m_transitioning || !room || room->started || !room->guest || !Service::get().isHost()) return;
        auto const uid = room->guest->uid;
        createQuickPopup(ban ? "Ban challenger" : "Remove challenger",
                         ban ? "Ban this challenger?\nThey can't come back while this room is open."
                             : "Remove this challenger from the room?\nThey can join again.",
                         "Cancel", ban ? "Ban" : "Kick", [self = Weak<RoomLayer>(this), uid, ban](auto*, bool yes) {
                             auto owner = self.lock();
                             if (!yes || !owner || owner->m_pending || owner->m_transitioning) return;
                             owner->m_pending = true;
                             owner->requestRender();
                             Service::get().kickGuest(
                                 uid,
                                 [self](bool ok, std::string detail) {
                                     if (auto owner = self.lock()) {
                                         owner->m_pending = false;
                                         owner->requestRender();
                                         if (!ok) error(detail);
                                     }
                                 },
                                 ban);
                         });
    }
    void onDownload(CCObject*) {
        if (!m_downloading) {
            m_cacheError.clear();
            beginDownload();
            requestRender();
        }
    }
    // 퀵바의 이모트 버튼.
    void onEmote(CCObject* sender) {
        int const tag = static_cast<CCNode*>(sender)->getTag();
        if (tag < 0 || tag >= static_cast<int>(m_emoteSet.size())) return;
        sendEmoteKind(m_emoteSet[tag]);
    }
    // 이모트 선택 팝업을 열어 고른 것을 전송한다. 상대가 없거나 방이 시작됐으면 안내만 한다.
    void onEmotePicker(CCObject*) {
        if (auto* popup = EmotePickerPopup::create(
                [self = Weak<RoomLayer>(this)](std::string kind) {
                    auto owner = self.lock();
                    if (!owner) return;
                    auto const& room = Service::get().room();
                    if (!room || !room->guest || room->started) {
                        FLAlertLayer::create("Emotes", "Emotes can be sent once both players are in the room.", "OK")
                            ->show();
                        return;
                    }
                    owner->sendEmoteKind(kind);
                },
                [self = Weak<RoomLayer>(this)] {
                    if (auto owner = self.lock()) owner->requestRender();
                }))
            popup->show();
    }
    // 이모트 전송. 쿨다운 중이거나 전송 중이면 무시하며, 멤버십 전용 이모트는 비회원이 보낼 수 없다.
    void sendEmoteKind(std::string const& kind) {
        auto const& room = Service::get().room();
        if (m_sendingEmote || m_emoteCooldown > 0.f || !room || !room->guest || room->started) return;
        if (!emotes::usable(kind, Service::get().profile().membership)) return;
        m_sendingEmote = true;
        m_emoteCooldown = firebase_config::EMOTE_COOLDOWN_SECONDS;
        Service::get().sendEmote(kind, [self = Weak<RoomLayer>(this)](bool success, std::string detail) {
            auto owner = self.lock();
            if (!owner) return;
            owner->m_sendingEmote = false;
            if (!success) error(detail);
        });
    }
    // 퀵챗 문구 전송 (이모트와 같은 쿨다운을 공유).
    void sendChat(std::string text) {
        auto const& room = Service::get().room();
        if (m_sendingEmote || m_emoteCooldown > 0.f || !room || !room->guest || room->started) return;
        m_sendingEmote = true;
        m_emoteCooldown = firebase_config::EMOTE_COOLDOWN_SECONDS;
        Service::get().sendChat(std::move(text), [self = Weak<RoomLayer>(this)](bool success, std::string detail) {
            if (auto owner = self.lock()) {
                owner->m_sendingEmote = false;
                if (!success) error(detail);
            }
        });
    }
    void onChat(CCObject*) {
        auto const& room = Service::get().room();
        if (m_sendingEmote || m_emoteCooldown > 0.f || !room || !room->guest || room->started) return;
        if (auto* popup = QuickChatPopup::create([self = Weak<RoomLayer>(this)](std::string text) {
                if (auto owner = self.lock()) owner->sendChat(std::move(text));
            }))
            popup->show();
    }
    // 맵 선택 팝업 (방장만, 추첨 중이 아닐 때).
    void onChoose(CCObject*) {
        auto const& room = Service::get().room();
        if (m_pending || m_transitioning || Service::get().busy() || !room || !Service::get().isHost() || room->started)
            return;
        if (drawingMap(*room)) return;
        if (auto* popup = MapSelectionPopup::create(*room)) popup->show();
    }
    // 방 시작 요청. 방장만 가능하고 상대가 있으며 둘 다 READY, 맵이 정해졌거나 랜덤 추첨이 필요한 상태여야 한다.
    // 외부 치트가 감지되면 막는다. 실제 전투 진입은 update에서 서버 상태를 보고 처리한다.
    void onStart(CCObject*) {
        auto const& room = Service::get().room();
        if (m_pending || m_transitioning || Service::get().busy() || !room || !Service::get().isHost() ||
            room->started || !room->guest || !room->hostReady || !room->guestReady || drawingMap(*room) ||
            (!room->level.id && !needsRandomDraw(*room)))
            return;
        if (readyBlockedByCheat(room->rules)) return;
        m_pending = true;
        requestRender();
        Service::get().startMatch([self = Weak<RoomLayer>(this)](bool success, std::string detail) {
            auto owner = self.lock();
            if (!owner) return;
            owner->m_pending = false;
            owner->requestRender();
            if (!success) error(detail);
        });
    }
    // READY 버튼 동작. readyControl이 정한 현재 동작에 따라 맵 선택, 다운로드, READY/READY 취소를 수행한다.
    void onReady(CCObject*) {
        auto const& room = Service::get().room();
        if (m_pending || m_transitioning || !room || room->started || (!Service::get().isHost() && !room->guest))
            return;
        m_cached = mapCached(room->level.id);
        bool const host = Service::get().isHost();
        auto const control =
            readyControl(room->started || drawingMap(*room), m_pending, host,
                         room->level.id > 0 || needsRandomDraw(*room), m_cached || needsRandomDraw(*room),
                         m_downloading, host ? room->hostReady : room->guestReady, mapAudioSkipped(room->level.id));
        if (control.action == ReadyAction::None) return;
        if (control.action == ReadyAction::ChooseMap) {
            if (room->mapPool)
                mappool::addMap();
            else
                onChoose(nullptr);
            return;
        }
        if (control.action == ReadyAction::Download) {
            beginDownload();
            requestRender();
            return;
        }
        if (control.action == ReadyAction::SkipMusic) {
            m_readyAfterDownload = skipMapAudio(room->level.id);
            requestRender();
            return;
        }
        bool const ready = control.action == ReadyAction::Ready;
        if (ready && readyBlockedByCheat(room->rules)) return;
        m_pending = true;
        requestRender();
        Service::get().setReady(ready, [self = Weak<RoomLayer>(this), ready](bool success, std::string detail) {
            auto owner = self.lock();
            if (!owner) return;
            owner->m_pending = false;
            owner->requestRender();
            if (success) {
                if (ready)
                    audio::play(audio::Cue::ReadyLock);
                else
                    audio::playBuiltin("quitSound_01.ogg");
            } else
                error(detail);
        });
    }
    // 방 나가기 확인. 방장이 나가면 방이 모두에게 닫힌다는 것을 알린다.
    bool m_leaveAsked = false;
    void goBack() override {
        if (m_pending || m_transitioning || m_leaveAsked || Service::get().busy()) return;
        m_leaveAsked = true;
        bool const host = Service::get().isHost();
        createQuickPopup("Leave Room",
                         host ? "Leave this room?\nYou are the host: the room closes for everyone."
                              : "Leave this room?",
                         "Stay", "Leave", [self = Weak<RoomLayer>(this)](auto*, bool yes) {
                             auto owner = self.lock();
                             if (!owner) return;
                             owner->m_leaveAsked = false;
                             if (yes) owner->leaveNow();
                         });
    }
    // 서버에 방 나가기를 요청하고 성공(또는 이미 방이 없음)하면 로비로 돌아간다.
    void leaveNow() {
        if (m_pending || m_transitioning || Service::get().busy()) return;
        m_pending = true;
        enabled(m_back, false);
        m_status->setString("Leaving room...");
        Service::get().leaveRoom([self = Weak<RoomLayer>(this)](bool success, std::string detail) {
            auto owner = self.lock();
            if (!owner) return;
            owner->m_pending = false;
            if (success || !Service::get().room()) {
                owner->m_transitioning = true;
                showLobby();
                return;
            }
            enabled(owner->m_back, true);
            owner->requestRender();
            error(detail);
        });
    }

public:
    CREATE_FUNC(RoomLayer);
    // 방 상태를 서버 폴링 결과로 확인하는 루프. 매 프레임은 애니메이션/다운로드 표시만 갱신하고,
    // ROOM_POLL_INTERVAL_SECONDS마다 다음을 처리한다.
    // - 방이 사라졌으면 로비로 이동 (방장이 나간 경우 등)
    // - 전투 결과가 나왔으면 서버에 확인(acknowledge)
    // - 시작된 방의 launch 정보가 있으면 launchMatch()로 전투 진입
    // - 새 맵 추첨이 있으면 룰렛 팝업 표시
    // - 맵 캐시 상태 변화 반영, 이모트 표시, 서명이 바뀌었으면 다시 그림
    void update(float dt) override {
        if (m_transitioning) return;
        m_readyPhase = std::fmod(m_readyPhase + dt * 2.f, 6.2831853f);
        offerUpdate();
        animateReady();
        m_emoteCooldown = std::max(0.f, m_emoteCooldown - dt);
        m_progressElapsed += dt;
        if (m_progressElapsed >= .25f) {
            m_progressElapsed = 0.f;
            if (auto const& room = Service::get().room()) updateDownloads(*room);
        }
        m_elapsed += dt;
        if (m_elapsed < firebase_config::ROOM_POLL_INTERVAL_SECONDS) return;
        m_elapsed = 0.f;
        auto const& room = Service::get().room();
        if (!room && !m_pending) {
            m_transitioning = true;
            std::string notice = Service::get().takeNotice();
            if (notice.empty()) notice = "The host left. This room has closed.";
            showLobby(std::move(notice));
            return;
        }
        if (room && room->battle && room->battle->finishedAt > 0 && m_returnedBattle != room->battle->id) {
            auto id = room->battle->id;
            Service::get().acknowledgeResult([self = Weak<RoomLayer>(this), id](bool ok, std::string) {
                if (auto owner = self.lock(); owner && ok) owner->m_returnedBattle = id;
            });
        }
        if (room && room->started && room->launch && room->battle && room->battle->finishedAt == 0 && !m_pending &&
            !isMatchLaunchActive()) {
            CCNode* scene = this;
            while (scene->getParent()) scene = scene->getParent();
            if (scene == CCDirector::sharedDirector()->getRunningScene()) {
                m_transitioning = true;
                launchMatch();
                return;
            }
        }
        if (room) {
            if (room->mapDraw && room->mapDraw->id != m_shownDraw &&
                (!room->mapDraw->settled || Service::get().serverNow() < room->mapDraw->at + DRAW_END_MS)) {
                if (auto* popup = MapRoulettePopup::create(*room)) {
                    m_shownDraw = room->mapDraw->id;
                    popup->show();
                }
            }
            RoomInfo const* shown = &*room;
            RoomInfo hidden;
            if (rouletteSpinning(room->mapDraw, Service::get().serverNow()) && room->mapDraw->settled) {
                hidden = *room;
                hideSpinningDraw(hidden, Service::get().serverNow());
                shown = &hidden;
            }
            bool const cached = shown->level.id > 0 && mapCached(shown->level.id);
            if (cached != m_cached) {
                m_cached = cached;
                if (cached) {
                    m_downloading = false;
                    m_cacheError.clear();
                    preloadBattleAssets();
                }
            }
            showRoomEmote(*room);
            if (signature(*shown) != m_signature) requestRender();
        }
    }
};

// Group 방 화면 구현 (인라인 include).
#include "GroupRoom.inl"
// Ranked 입장 전에 게임플레이에 영향을 주는 외부 치트/모드가 켜져 있는지 검사한다.
// 감지되면 안내 팝업을 띄우고 true(차단)를 반환한다.
bool rankedBlockedByCheat() {
    auto status = integrity::activeExternalCheat(false);
    if (!status.detected) return false;
    auto message = fmt::format("Disable <cr>{}</c> gameplay-affecting features before entering Ranked.\n{}",
                               status.source, status.detail);
    FLAlertLayer::create("Ranked Blocked", message.c_str(), "OK")->show();
    return true;
}

// 아래 .inl 파일들은 이 번역 단위에 그대로 포함되는 화면별 구현이다.
// (Ranked 대전/리더보드/티어/점수, Season 2 광장·매치·결과·듀얼 등)
#include "DuelBlocks.inl"
#include "RankedPresentation.inl"
#include "RankedDuel.inl"
#include "RankedGuide.inl"
#include "RankedLeaderboard.inl"
#include "LiveMatches.inl"
#include "RankedTiers.inl"
#include "RankedPoints.inl"
#include "Season2.inl"
#include "Season2Plaza.inl"
#include "Season2Match.inl"
#include "Season2Result.inl"
#include "Season2Duel.inl"

// Ranked 로비 음악 팝업. 로비 음악 볼륨(최대 150%), 곡 선택, 곡 자동 다운로드 여부를
// 조절한다.
// Season 2 모드에서는 게임 음악/효과음 볼륨 슬라이더가 함께 표시된다.
class RankedMusicPopup final : public ::versus::DeferredPopup {
    Slider* m_slider = nullptr;
    CCLabelBMFont* m_value = nullptr;
    CCLabelBMFont* m_song = nullptr;
    // 현재 곡 제목: 고른 곡이 있으면 그 곡, 없고 컵 노래가 있으면 컵 노래(CUP SONG).
    void showSong() {
        int const cup = audio::cupSong(), mine = audio::rankedMusicSong();
        m_song->setString(mine <= 0 && cup > 0 ? fmt::format("{}  (CUP SONG)", songTitle(cup)).c_str()
                                               : songTitle(mine).c_str());
        m_song->limitLabelWidth(170.f, .32f, .15f);
    }
    void onSong(CCObject*) {
        if (auto* popup = LobbySongPopup::create([self = Weak<RankedMusicPopup>(this)] {
                if (auto owner = self.lock()) owner->showSong();
            }))
            popup->show();
    }
    void refresh() {
        m_value->setString(
            fmt::format("{}%", static_cast<int>(std::lround(audio::rankedMusicVolume() * 100.f))).c_str());
    }
    void onVolume(CCObject*) {
        audio::setRankedMusicVolume(m_slider->getValue() * audio::kRankedMusicMax);
        refresh();
    }
    void onMute(CCObject*) {
        audio::setRankedMusicVolume(0.f);
        m_slider->setValue(0.f);
        refresh();
    }
    CCMenuItemToggler* m_alwaysSong = nullptr;
    // 토글 상태를 다음 프레임에 설정값과 다시 맞춘다. (확인창에서 취소해도 토글이
    // 되돌아가도록)
    void syncAlwaysSong() {
        Loader::get()->queueInMainThread([self = Weak<RankedMusicPopup>(this)] {
            if (auto owner = self.lock(); owner && owner->m_alwaysSong)
                owner->m_alwaysSong->toggle(alwaysDownloadSong());
        });
    }
    // 끄려고 하면 바로 저장하지 않고 확인창을 먼저 띄운다. 켜는 것은 즉시 저장한다.
    void onAlwaysSong(CCObject*) {
        if (!alwaysDownloadSong()) {
            setAlwaysDownloadSong(true);
            syncAlwaysSong();
            return;
        }
        syncAlwaysSong();
        createQuickPopup(
            "Always Download Song",
            "If you turn this off, Ranked matches will <cr>not download the map's song and sound effects</c> "
            "automatically, and you will play the match <cy>without them</c>.\n(Only what is already on this device plays.)",
            "Keep On", "Turn Off", 340.f, [self = Weak<RankedMusicPopup>(this)](auto*, bool off) {
                if (off) setAlwaysDownloadSong(false);
                if (auto owner = self.lock()) owner->syncAlwaysSong();
            });
    }
    void songPlate(float width, float y) {
        auto* plate = NineSlice::create("square02b_001.png");
        plate->setID("ranked-song-plate"_spr);
        plate->setContentSize({width - 24.f, 24.f});
        plate->setPosition({width / 2.f, y});
        plate->setColor(ccc3(0, 0, 0));
        m_mainLayer->addChild(plate);
    }
    void alwaysSongRow(float width, float y) {
        m_alwaysSong =
            CCMenuItemToggler::createWithStandardSprites(this, menu_selector(RankedMusicPopup::onAlwaysSong), .55f);
        m_alwaysSong->setID("ranked-always-download-song"_spr);
        float const x = width / 2.f - 72.f;
        m_alwaysSong->setPosition({x, y});
        m_alwaysSong->toggle(alwaysDownloadSong());
        m_buttonMenu->addChild(m_alwaysSong);
        label(m_mainLayer, "Always Download Song", {x + 16.f, y}, .4f, 150.f, ccWHITE, true, "goldFont.fnt");
    }
    bool m_season2 = false;
    Slider* m_gameMusic = nullptr;
    Slider* m_sfx = nullptr;
    CCLabelBMFont* m_gameMusicValue = nullptr;
    CCLabelBMFont* m_sfxValue = nullptr;
    static std::string percentText(float value) {
        return fmt::format("{}%", static_cast<int>(std::lround(std::clamp(value, 0.f, 1.f) * 100.f)));
    }
    void refreshGame() {
        auto* manager = GameManager::sharedState();
        if (!manager || !m_gameMusicValue || !m_sfxValue) return;
        m_gameMusicValue->setString(percentText(manager->m_bgVolume).c_str());
        m_sfxValue->setString(percentText(manager->m_sfxVolume).c_str());
    }
    // 게임 배경음악 볼륨: GameManager 값과 FMOD 엔진에 모두 즉시 반영한다.
    void onGameMusic(CCObject*) {
        float const value = std::clamp(m_gameMusic->getValue(), 0.f, 1.f);
        if (auto* manager = GameManager::sharedState()) manager->m_bgVolume = value;
        if (auto* engine = FMODAudioEngine::sharedEngine()) engine->setBackgroundMusicVolume(value);
        refreshGame();
    }
    void onSfx(CCObject*) {
        float const value = std::clamp(m_sfx->getValue(), 0.f, 1.f);
        if (auto* manager = GameManager::sharedState()) manager->m_sfxVolume = value;
        if (auto* engine = FMODAudioEngine::sharedEngine()) engine->setEffectsVolume(value);
        refreshGame();
    }
    // 라벨 + 슬라이더 + 퍼센트 값으로 이루어진 볼륨 행을 만든다. 슬라이더를 반환한다.
    Slider* volumeRow(char const* name, float y, SEL_MenuHandler handler, float value, CCLabelBMFont*& text) {
        label(m_mainLayer, name, {26.f, y + 17.f}, .38f, 170.f, kIce, true);
        text = label(m_mainLayer, "", {264.f, y + 17.f}, .38f, 60.f, ccWHITE);
        text->setAnchorPoint({1.f, .5f});
        auto* slider = Slider::create(this, handler, .75f);
        slider->setPosition({145.f, y});
        slider->setValue(std::clamp(value, 0.f, 1.f));
        m_mainLayer->addChild(slider);
        return slider;
    }
    // Season 2용 레이아웃: 게임 음악, 효과음, Ranked 로비 음악 볼륨과 곡 선택을 한 팝업에
    // 둔다.
    bool initSeason2() {
        if (!Popup::init(290.f, 326.f, "GJ_square02.png")) return false;
        setTitle("Music", "bigFont.fnt", .55f);
        auto* manager = GameManager::sharedState();
        m_gameMusic = volumeRow("Music", 254.f, menu_selector(RankedMusicPopup::onGameMusic),
                                manager ? manager->m_bgVolume : 1.f, m_gameMusicValue);
        m_sfx = volumeRow("Sound Effects", 202.f, menu_selector(RankedMusicPopup::onSfx),
                          manager ? manager->m_sfxVolume : 1.f, m_sfxValue);
        m_slider = volumeRow("Ranked Lobby Music", 150.f, menu_selector(RankedMusicPopup::onVolume),
                             audio::rankedMusicVolume() / audio::kRankedMusicMax, m_value);
        label(m_mainLayer, "Lobby music goes up to 150%.", {145.f, 128.f}, .36f, 250.f, kIce, false, "chatFont.fnt");
        songPlate(290.f, 102.f);
        label(m_mainLayer, "SONG", {24.f, 102.f}, .32f, 40.f, ccc3(255, 214, 120), true, "goldFont.fnt");
        m_song = label(m_mainLayer, "", {62.f, 102.f}, .32f, 170.f, ccWHITE, true, "bigFont.fnt");
        showSong();
        alwaysSongRow(290.f, 68.f);
        // Standard Cup(컵 노래가 있는 컵)에서도 곡을 바꿀 수 있다. Default를 고르면 다시 컵 노래가 나온다.
        button(m_buttonMenu, this, menu_selector(RankedMusicPopup::onSong), "Change Song", {90.f, 30.f}, .58f,
               "GJ_button_04.png");
        button(m_buttonMenu, this, menu_selector(RankedMusicPopup::onMute), "Mute", {216.f, 30.f}, .58f);
        refresh();
        refreshGame();
        return true;
    }
    // 기본(Season 1) 레이아웃은 로비 음악 볼륨 하나와 곡 선택만 가진다.
    bool init() override {
        if (m_season2) return initSeason2();
        if (!Popup::init(270.f, 244.f, "GJ_square02.png")) return false;
        setTitle("Ranked Music", "bigFont.fnt", .55f);
        m_slider = Slider::create(this, menu_selector(RankedMusicPopup::onVolume), .75f);
        m_slider->setPosition({135.f, 159.f});
        m_slider->setValue(audio::rankedMusicVolume() / audio::kRankedMusicMax);
        m_mainLayer->addChild(m_slider);
        m_value = label(m_mainLayer, "", {135.f, 189.f}, .5f, 150.f, kIce);
        label(m_mainLayer, "Only affects Ranked lobby music (up to 150%).", {135.f, 130.f}, .4f, 238.f, kIce, false,
              "chatFont.fnt");
        songPlate(270.f, 100.f);
        label(m_mainLayer, "SONG", {20.f, 100.f}, .32f, 40.f, ccc3(255, 214, 120), true, "goldFont.fnt");
        m_song = label(m_mainLayer, "", {58.f, 100.f}, .32f, 170.f, ccWHITE, true, "bigFont.fnt");
        showSong();
        alwaysSongRow(270.f, 66.f);
        button(m_buttonMenu, this, menu_selector(RankedMusicPopup::onSong), "Change Song", {84.f, 30.f}, .58f,
               "GJ_button_04.png");
        button(m_buttonMenu, this, menu_selector(RankedMusicPopup::onMute), "Mute", {208.f, 30.f}, .58f);
        refresh();
        return true;
    }
    // 팝업을 닫을 때 변경된 설정을 디스크에 저장한다.
    void onExit() override {
        if (auto* mod = Mod::get()) (void)mod->saveData();
        Popup::onExit();
    }

public:
    CREATE_FUNC(RankedMusicPopup);
    // Season 2 레이아웃으로 생성하는 팩토리.
    static RankedMusicPopup* createSeason2() {
        auto* result = new RankedMusicPopup;
        result->m_season2 = true;
        if (result->init()) {
            result->autorelease();
            return result;
        }
        delete result;
        return nullptr;
    }
};

#include "SettingsPopup.inl"
#include "QueueGames.inl"

// 닷지 스택 표시 색: 0이면 초록, 한도 직전이면 주황, 그 외는 노랑.
ccColor3B dodgeColor(int stack, int limit) {
    if (stack <= 0) return ccc3(120, 235, 150);
    if (stack + 1 >= limit) return ccc3(255, 130, 90);
    return ccc3(255, 215, 110);
}
// 이탈 시 부과될 닷지 가중치. 맵이 이미 정해진 뒤의 이탈이면 afterMap, 아니면 1.
int dodgeWeight(RankedMatch const& match, RankedDodge const& dodge) {
    bool const chosen = match.state == "loading" || (match.state == "preparing" && match.selectedMap &&
                                                     Service::get().serverNow() >= match.selectedAt + DRAW_END_MS);
    return chosen ? std::max(1, dodge.afterMap) : 1;
}
// 밀리초를 올림하여 m:ss 형식의 시계 문자열로 만든다.
std::string clockText(int64_t ms) {
    auto const seconds = std::max<int64_t>(0, (ms + 999) / 1000);
    return fmt::format("{}:{:02}", seconds / 60, seconds % 60);
}

// 시스템 폰트 텍스트 한 줄을 만들고, 지정 너비보다 길면 너비에 맞춰 축소한다.
CCSprite* systemLine(std::string const& text, float size, float width, TextWeight weight = TextWeight::Regular) {
    auto* node = createSystemText(text, size, 0.f, kCCTextAlignmentLeft, weight);
    float const natural = node ? node->getContentSize().width : 0.f;
    if (node && width > 0.f && natural > width) node->setScale(width / natural);
    return node;
}
// 경과 시간을 한국어 상대 표현(방금/N분 전/N시간 전/N일 전)으로 만든다.
// (문자열은 UTF-8 이스케이프로 적혀 있다.)
std::string koreanAgo(int64_t ms) {
    int64_t const minutes = std::max<int64_t>(0, ms) / 60'000;
    if (minutes < 1) return "\xEB\xB0\xA9\xEA\xB8\x88";
    if (minutes < 60) return fmt::format("{}\xEB\xB6\x84 \xEC\xA0\x84", minutes);
    if (minutes < 48 * 60) return fmt::format("{}\xEC\x8B\x9C\xEA\xB0\x84 \xEC\xA0\x84", minutes / 60);
    return fmt::format("{}\xEC\x9D\xBC \xEC\xA0\x84", minutes / (24 * 60));
}

// 내 Season 2 닷지 기록 팝업. 서버에서 최근 90일 기록을 받아 5개씩 페이지로 보여 준다.
// 한국어/영어 표시를 지원한다.
class MyDodgeLogPopup final : public ::versus::DeferredPopup {
    static constexpr int kPage = 5;
    static constexpr float kWidth = 420.f, kHeight = 280.f, kRowWidth = 392.f;
    std::vector<DodgeLogEntry> m_entries;
    int m_limit = 3, m_page = 0;
    bool m_isKorean = true, m_loading = false;
    std::string m_error;
    CCNode* m_rows = nullptr;
    CCLabelBMFont* m_pageLabel = nullptr;
    CCMenuItemSpriteExtra *m_previous = nullptr, *m_next = nullptr;
    CCSprite* text(CCNode* parent, std::string const& value, CCPoint position, float size, float width, ccColor3B color,
                   CCPoint anchor, TextWeight weight = TextWeight::Regular) {
        auto* node = systemLine(value, size, width, weight);
        if (!node) return nullptr;
        node->setColor(color);
        node->setAnchorPoint(anchor);
        node->setPosition(position);
        parent->addChild(node);
        return node;
    }
    // 닷지 원인 코드(left/disconnected/sweep/admin)를 표시용 문자열로 바꾼다.
    std::string cause(DodgeLogEntry const& entry) const {
        bool const ko = m_isKorean;
        if (entry.cause == "left")
            return ko ? "\xEB\xA7\xA4\xEC\xB9\x98\xEB\xA5\xBC \xEB\x82\x98\xEA\xB0\x90" : "Left the match";
        if (entry.cause == "disconnected")
            return ko ? "\xEC\x97\xB0\xEA\xB2\xB0 \xEB\x81\x8A\xEA\xB9\x80" : "Disconnected";
        if (entry.cause == "sweep")
            return ko ? "\xEA\xB2\x8C\xEC\x9E\x84 \xEC\x9D\x91\xEB\x8B\xB5 \xEC\x97\x86\xEC\x9D\x8C"
                      : "Game stopped answering";
        if (entry.cause == "admin")
            return ko ? "\xEA\xB4\x80\xEB\xA6\xAC\xEC\x9E\x90\xEA\xB0\x80 \xEC\x8A\xA4\xED\x83\x9D \xEB\xB3\x80\xEA\xB2\xBD"
                      : "Stack set by an admin";
        return entry.cause.empty() ? std::string(ko ? "\xEB\x8B\xB7\xEC\xA7\x80" : "Dodge") : entry.cause;
    }
    // 닷지가 발생한 시점(맵 이름, 밴 중, 맵 탐색 중, 맵 결정 전)을 표시용 문자열로 만든다.
    std::string where(DodgeLogEntry const& entry) const {
        bool const ko = m_isKorean;
        if (entry.map && entry.mapShown) {
            auto const name = entry.map->name.empty() ? std::string("?") : entry.map->name;
            return ko ? fmt::format("\xEB\xA7\xB5: {} (#{})", name, entry.map->id)
                      : fmt::format("Map: {} (#{})", name, entry.map->id);
        }
        if (entry.state == "banning") return ko ? "\xEB\xA7\xB5 \xEB\xB0\xB4 \xEC\xA4\x91" : "During the map bans";
        if (entry.state == "map_search")
            return ko ? "\xEB\xA7\xB5 \xEC\xB0\xBE\xEB\x8A\x94 \xEC\xA4\x91" : "While the map was searched";
        return ko ? "\xEB\xA7\xB5 \xEA\xB2\xB0\xEC\xA0\x95 \xEC\xA0\x84" : "Before the map was chosen";
    }
    bool init(int limit, bool korean) {
        if (!Popup::init(kWidth, kHeight, "GJ_square02.png")) return false;
        m_limit = std::max(1, limit);
        m_isKorean = korean;
        setTitle("Dodge Log", "goldFont.fnt", .72f);
        setID("versus-my-dodge-log"_spr);
        m_rows = CCNode::create();
        m_mainLayer->addChild(m_rows, 2);
        m_previous = button(m_buttonMenu, this, menu_selector(MyDodgeLogPopup::onPrevious), "<", {150.f, 22.f}, .45f);
        m_next = button(m_buttonMenu, this, menu_selector(MyDodgeLogPopup::onNext), ">", {250.f, 22.f}, .45f);
        m_pageLabel = label(m_mainLayer, "1 / 1", {200.f, 22.f}, .33f, 60.f, kIce);
        button(m_buttonMenu, this, menu_selector(MyDodgeLogPopup::onOK), "OK", {370.f, 22.f}, .55f);
        load();
        return true;
    }
    // 기록을 서버에서 받아 온다. 응답이 오기 전에 팝업이 닫혔으면 아무것도 하지 않는다.
    void load() {
        if (m_loading) return;
        m_loading = true;
        render();
        Service::get().fetchSeason2Dodges([self = Weak<MyDodgeLogPopup>(this)](
                                              std::optional<std::vector<DodgeLogEntry>> entries, std::string detail) {
            auto owner = self.lock();
            if (!owner) return;
            owner->m_loading = false;
            owner->m_error =
                entries ? std::string() : (detail.empty() ? std::string("Could not load the log.") : detail);
            if (entries) owner->m_entries = std::move(*entries);
            owner->render();
        });
    }
    // 현재 페이지의 상태 문구(로딩/오류/빈 목록)와 기록 행을 다시 그린다.
    // 패널 한 행에는 원인·가중치·컵·듀얼 여부, 시각, 상대, 스택, 패널티가 들어간다.
    void render() {
        m_rows->removeAllChildrenWithCleanup(true);
        bool const ko = m_isKorean;
        int const count = static_cast<int>(m_entries.size());
        int const pages = std::max(1, (count + kPage - 1) / kPage);
        m_page = std::clamp(m_page, 0, pages - 1);
        m_pageLabel->setString(fmt::format("{} / {}", m_page + 1, pages).c_str());
        enabled(m_previous, m_page > 0);
        enabled(m_next, m_page + 1 < pages);
        text(
            m_rows,
            ko ? "\xEB\x82\xB4 \xEC\x8B\x9C\xEC\xA6\x8C 2 \xEB\x8B\xB7\xEC\xA7\x80 \xEA\xB8\xB0\xEB\xA1\x9D (\xEC\xB5\x9C\xEA\xB7\xBC 90\xEC\x9D\xBC, \xEC\xB5\x9C\xEC\x8B\xA0\xEC\x88\x9C)"
               : "Your Season 2 dodges (last 90 days, newest first)",
            {kWidth / 2.f, 238.f}, 11.5f, kRowWidth, kMuted, {.5f, .5f});
        std::string status;
        if (m_loading && m_entries.empty())
            status = ko ? "\xEB\xB6\x88\xEB\x9F\xAC\xEC\x98\xA4\xEB\x8A\x94 \xEC\xA4\x91..." : "Loading...";
        else if (!m_error.empty())
            status = m_error;
        else if (count == 0)
            status =
                ko ? "\xEC\xB5\x9C\xEA\xB7\xBC 90\xEC\x9D\xBC \xEB\x8F\x99\xEC\x95\x88 \xEB\x8B\xB7\xEC\xA7\x80 \xEA\xB8\xB0\xEB\xA1\x9D\xEC\x9D\xB4 \xEC\x97\x86\xEC\x8A\xB5\xEB\x8B\x88\xEB\x8B\xA4."
                   : "No dodges in the last 90 days.";
        if (!status.empty())
            text(m_rows, status, {kWidth / 2.f, 140.f}, 13.f, kRowWidth, m_error.empty() ? kIce : kAlert, {.5f, .5f});
        auto const now = Service::get().serverNow();
        for (int row = 0; row < kPage; ++row) {
            int const index = m_page * kPage + row;
            if (index >= count) break;
            auto const& entry = m_entries[index];
            float const y = 205.f - row * 37.f;
            panel(m_rows, {14.f, y - 17.f}, {kRowWidth, 34.f}, false);
            std::string head =
                ko ? fmt::format("{}  +{} \xEB\x8B\xB7\xEC\xA7\x80", cause(entry), entry.weight)
                   : fmt::format("{}  +{} dodge{}", cause(entry), entry.weight, entry.weight == 1 ? "" : "s");
            auto cup = entry.cupName.empty() ? season2CupName(entry.cup) : entry.cupName;
            if (!cup.empty()) head += "  |  " + cup;
            if (entry.duel) head += ko ? "  |  \xEB\x93\x80\xEC\x96\xBC" : "  |  Duel";
            auto const when = fmt::format("{}  ({})", localDateTime(entry.at),
                                          ko ? koreanAgo(now - entry.at) : agoText(now - entry.at));
            auto* time = text(m_rows, when, {14.f + kRowWidth - 8.f, y + 7.f}, 10.5f, 150.f, kMuted, {1.f, .5f});
            float const timeWidth = time ? time->getScaledContentSize().width : 120.f;
            text(m_rows, head, {22.f, y + 7.f}, 12.f, kRowWidth - 24.f - timeWidth, entry.penalized ? kAlert : kIce,
                 {0.f, .5f}, TextWeight::Bold);
            std::string line = entry.opponentUid.empty()
                                   ? std::string()
                                   : fmt::format("vs {}  |  ", entry.opponentName.empty() ? std::string("Player")
                                                                                          : entry.opponentName);
            line += where(entry);
            line += ko ? fmt::format("  |  \xEC\x8A\xA4\xED\x83\x9D {} / {}", entry.stackAfter, m_limit)
                       : fmt::format("  |  stack {} / {}", entry.stackAfter, m_limit);
            if (entry.penalized)
                line +=
                    ko ? fmt::format(
                             "  |  \xED\x8C\xA8\xEB\x84\x90\xED\x8B\xB0 {:+} R, \xEB\xA7\xA4\xEC\xB9\xAD \xEC\xA0\x9C\xED\x95\x9C",
                             entry.ratingDelta)
                       : fmt::format("  |  PENALTY {:+} R, matchmaking closed", entry.ratingDelta);
            text(m_rows, line, {22.f, y - 8.f}, 10.5f, kRowWidth - 16.f, entry.penalized ? ccc3(255, 160, 140) : kMuted,
                 {0.f, .5f});
        }
    }
    void onPrevious(CCObject*) {
        if (m_page > 0) {
            --m_page;
            render();
        }
    }
    void onNext(CCObject*) {
        ++m_page;
        render();
    }
    void onOK(CCObject*) { onClose(nullptr); }

public:
    static MyDodgeLogPopup* create(int limit, bool korean) {
        auto* result = new MyDodgeLogPopup;
        if (result->init(limit, korean)) {
            result->autorelease();
            return result;
        }
        delete result;
        return nullptr;
    }
};

// 닷지 규칙 안내 팝업. 현재 스택, 마지막 패널티, 규칙 목록을 보여 주고 한/영 전환을
// 지원한다.
// Season 1(Ranked)과 Season 2 규칙 문구가 서로 다르다.
class DodgeInfoPopup final : public ::versus::DeferredPopup {
    RankedDodge m_dodge;
    bool m_penalty = false, m_isKorean = true;
    CCNode* m_body = nullptr;
    CCSprite* m_restriction = nullptr;
    std::string m_restrictionText;
    CCMenuItemSpriteExtra *m_english = nullptr, *m_korean = nullptr;
    // 본문 영역에 가운데 정렬된 텍스트를 추가한다. (여러 줄 줄바꿈 허용)
    CCSprite* line(std::string const& text, CCPoint position, float size, ccColor3B color,
                   TextWeight weight = TextWeight::Regular) {
        auto* node = createSystemText(text, size, 344.f, kCCTextAlignmentCenter, weight);
        node->setColor(color);
        node->setAnchorPoint({.5f, .5f});
        node->setPosition(position);
        m_body->addChild(node);
        return node;
    }
    // 본문 영역에 한 줄짜리 텍스트를 추가한다. (너비가 넘으면 축소)
    CCSprite* single(std::string const& text, CCPoint position, float size, ccColor3B color,
                     TextWeight weight = TextWeight::Regular) {
        auto* node = systemLine(text, size, 344.f, weight);
        if (!node) return nullptr;
        node->setColor(color);
        node->setAnchorPoint({.5f, .5f});
        node->setPosition(position);
        m_body->addChild(node);
        return node;
    }
    // 서버가 내려 준 값(한도, 패널티 판수, 차단 시간 등)을 채운 규칙 문장 목록.
    // Season 2 / Season 1 과 언어별로 분기한다.
    std::vector<std::string> rules() const {
        auto const& d = m_dodge;
        if (d.season2) {
            if (m_isKorean)
                return {
                    "\xEC\xB0\xBE\xEC\x9D\x80 \xEB\xA7\xA4\xEC\xB9\x98\xEB\xA5\xBC \xEB\x8C\x80\xEA\xB2\xB0 \xEC\x8B\x9C\xEC\x9E\x91 \xEC\xA0\x84\xEC\x97\x90 \xEB\x82\x98\xEA\xB0\x80\xEB\xA9\xB4(\xEA\xB7\xB8\xEB\x95\x8C \xEC\x97\xB0\xEA\xB2\xB0\xEC\x9D\xB4 \xEB\x81\x8A\xEA\xB2\xA8\xEB\x8F\x84) = \xEB\x8B\xB7\xEC\xA7\x80 1. \xEB\x93\x80\xEC\x96\xBC\xEB\x8F\x84 \xED\x8F\xAC\xED\x95\xA8\xEB\x90\xA9\xEB\x8B\x88\xEB\x8B\xA4.",
                    fmt::format(
                        "\xEB\xA7\xB5 \xEB\xB0\xB4\xEC\x9C\xBC\xEB\xA1\x9C \xEB\xA7\xB5\xEC\x9D\xB4 \xEC\xA0\x95\xED\x95\xB4\xEC\xA7\x84 \xEB\x92\xA4\xEC\x97\x90 \xEB\x82\x98\xEA\xB0\x80\xEB\xA9\xB4 = \xEB\x8B\xB7\xEC\xA7\x80 {}.",
                        d.afterMap),
                    fmt::format(
                        "\xEB\x8B\xB7\xEC\xA7\x80 {}\xEA\xB0\x9C: \xEA\xB7\xB8 \xEB\xA7\xA4\xEC\xB9\x98 \xEC\xBB\xB5\xEC\x9D\x98 \xEB\xA0\x88\xEC\x9D\xB4\xED\x8C\x85\xEC\x9D\xB4 \xEB\x82\xB4 \xEB\xA0\x88\xEC\x9D\xB4\xED\x8C\x85\xEA\xB3\xBC \xEA\xB0\x99\xEC\x9D\x80 \xEC\x83\x81\xEB\x8C\x80\xEC\x97\x90\xEA\xB2\x8C {}\xED\x8C\x90 \xEC\xA7\x84 \xEB\xA7\x8C\xED\x81\xBC \xEB\x96\xA8\xEC\x96\xB4\xEC\xA7\x80\xEA\xB3\xA0, {}\xEB\xB6\x84 \xEB\x8F\x99\xEC\x95\x88 \xEB\x91\x90 \xEC\xBB\xB5 \xEB\xAA\xA8\xEB\x91\x90 \xEB\xA7\xA4\xEC\xB9\xAD\xEC\x9D\xB4 \xEB\xA7\x89\xED\x9E\x99\xEB\x8B\x88\xEB\x8B\xA4. \xEA\xB7\xB8 \xEB\x92\xA4 \xEC\x8A\xA4\xED\x83\x9D\xEC\x9D\x80 {}\xEB\xA1\x9C \xEB\x82\xA8\xEC\x8A\xB5\xEB\x8B\x88\xEB\x8B\xA4.",
                        d.limit, d.penaltyLosses, d.blockMinutes, std::max(0, d.limit - 1)),
                    fmt::format(
                        "\xEC\x8B\x9C\xEC\xA6\x8C 2 \xEB\xA7\xB5\xEC\x9D\x84 {}\xED\x8C\x90 \xEC\xA0\x95\xEC\x83\x81\xEC\xA0\x81\xEC\x9C\xBC\xEB\xA1\x9C \xED\x94\x8C\xEB\xA0\x88\xEC\x9D\xB4\xED\x95\x98\xEB\xA9\xB4(\xEC\x8A\xB9\xC2\xB7\xED\x8C\xA8\xC2\xB7\xEB\xAC\xB4 \xEB\xAA\xA8\xEB\x91\x90, \xEB\x93\x80\xEC\x96\xBC \xEC\xA0\x9C\xEC\x99\xB8) \xEC\x8A\xA4\xED\x83\x9D\xEC\x9D\xB4 \xEC\x82\xAC\xEB\x9D\xBC\xEC\xA7\x91\xEB\x8B\x88\xEB\x8B\xA4.",
                        d.clearMaps),
                    "\xEC\x8B\x9C\xEC\xA6\x8C 2\xEC\x9D\x98 \xEB\x8B\xB7\xEC\xA7\x80 \xEC\x8A\xA4\xED\x83\x9D\xEC\x9D\x80 \xEC\x8B\x9C\xEC\xA6\x8C 1\xEA\xB3\xBC \xEB\x94\xB0\xEB\xA1\x9C\xEC\x9E\x85\xEB\x8B\x88\xEB\x8B\xA4.",
                };
            return {
                "Leaving a found Season 2 match before its battle starts (or disconnecting then) = 1 dodge. Duels count too.",
                fmt::format("Leaving after the map bans have chosen the map = {} dodges.", d.afterMap),
                fmt::format(
                    "{} dodges: your rating in that match's cup drops as if you lost {} games to a player of your "
                    "rating, and matchmaking in both cups is closed for {} minutes. The stack then stays at {}.",
                    d.limit, d.penaltyLosses, d.blockMinutes, std::max(0, d.limit - 1)),
                fmt::format("{} Season 2 maps played normally clear the stack (wins, losses and draws; not duels).",
                            d.clearMaps),
                "Season 2 has its own dodge stack: Season 1's is separate.",
            };
        }
        if (m_isKorean)
            return {
                "\xEC\xB0\xBE\xEC\x9D\x80 \xEB\xA7\xA4\xEC\xB9\x98\xEB\xA5\xBC \xEB\x8C\x80\xEA\xB2\xB0 \xEC\x8B\x9C\xEC\x9E\x91 \xEC\xA0\x84\xEC\x97\x90 \xEB\x82\x98\xEA\xB0\x80\xEB\xA9\xB4(\xEA\xB7\xB8\xEB\x95\x8C \xEC\x97\xB0\xEA\xB2\xB0\xEC\x9D\xB4 \xEB\x81\x8A\xEA\xB2\xA8\xEB\x8F\x84) = \xEB\x8B\xB7\xEC\xA7\x80 1.",
                fmt::format(
                    "\xEB\xA3\xB0\xEB\xA0\x9B\xEC\x9D\xB4 \xEB\xA7\xB5\xEC\x9D\x84 \xEA\xB3\xA0\xEB\xA5\xB8 \xEB\x92\xA4\xEC\x97\x90 \xEB\x82\x98\xEA\xB0\x80\xEB\xA9\xB4 = \xEB\x8B\xB7\xEC\xA7\x80 {}.",
                    d.afterMap),
                fmt::format(
                    "\xEB\x8B\xB7\xEC\xA7\x80 {}\xEA\xB0\x9C: \xEB\x82\xB4 \xEB\xA0\x88\xEC\x9D\xB4\xED\x8C\x85\xEA\xB3\xBC \xEA\xB0\x99\xEC\x9D\x80 \xEC\x83\x81\xEB\x8C\x80\xEC\x97\x90\xEA\xB2\x8C {}\xED\x8C\x90 \xEC\xA7\x84 \xEB\xA7\x8C\xED\x81\xBC \xEB\xA0\x88\xEC\x9D\xB4\xED\x8C\x85\xEC\x9D\xB4 \xEB\x96\xA8\xEC\x96\xB4\xEC\xA7\x80\xEA\xB3\xA0, {}\xEB\xB6\x84 \xEB\x8F\x99\xEC\x95\x88 \xEB\xA7\xA4\xEC\xB9\xAD\xEC\x9D\xB4 \xEB\xA7\x89\xED\x9E\x99\xEB\x8B\x88\xEB\x8B\xA4. "
                    "\xEA\xB7\xB8 \xEB\x92\xA4 \xEC\x8A\xA4\xED\x83\x9D\xEC\x9D\x80 {}\xEB\xA1\x9C \xEB\x82\xA8\xEC\x8A\xB5\xEB\x8B\x88\xEB\x8B\xA4.",
                    d.limit, d.penaltyLosses, d.blockMinutes, std::max(0, d.limit - 1)),
                fmt::format(
                    "\xEB\x9E\xAD\xED\x81\xAC \xEB\xA7\xB5\xEC\x9D\x84 {}\xED\x8C\x90 \xEC\xA0\x95\xEC\x83\x81\xEC\xA0\x81\xEC\x9C\xBC\xEB\xA1\x9C \xED\x94\x8C\xEB\xA0\x88\xEC\x9D\xB4\xED\x95\x98\xEB\xA9\xB4(\xEC\x8A\xB9\xC2\xB7\xED\x8C\xA8\xC2\xB7\xEB\xAC\xB4 \xEB\xAA\xA8\xEB\x91\x90) \xEC\x8A\xA4\xED\x83\x9D\xEC\x9D\xB4 \xEC\x82\xAC\xEB\x9D\xBC\xEC\xA7\x91\xEB\x8B\x88\xEB\x8B\xA4.",
                    d.clearMaps),
                d.downloadWeight > 0
                    ? fmt::format(
                          "\xEC\x83\x81\xEB\x8C\x80\xEB\x8A\x94 \xEB\xB0\x9B\xEC\x9D\x80 \xEB\xA7\xB5\xEC\x9D\x84 \xEB\x82\xB4 \xEA\xB2\x8C\xEC\x9E\x84\xEC\x9D\xB4 \xEB\x82\xB4\xEB\xA0\xA4\xEB\xB0\x9B\xEC\xA7\x80 \xEB\xAA\xBB\xED\x95\x98\xEB\xA9\xB4 \xEB\xA7\xA4\xEC\xB9\x98\xEA\xB0\x80 \xEC\xB7\xA8\xEC\x86\x8C\xEB\x90\xA9\xEB\x8B\x88\xEB\x8B\xA4. 2\xEC\x8B\x9C\xEA\xB0\x84 \xEC\x95\x88\xEC\x97\x90 {}\xEB\xB2\x88\xEA\xB9\x8C\xEC\xA7\x80\xEB\x8A\x94 \xEA\xB2\xBD\xEA\xB3\xA0\xEB\xA7\x8C, \xEA\xB7\xB8 \xEB\x92\xA4\xEB\xA1\x9C\xEB\x8A\x94 \xEB\xA7\xA4\xEB\xB2\x88 \xEB\x8B\xB7\xEC\xA7\x80 {}.",
                          d.downloadFree, d.downloadWeight)
                    : std::string(
                          "\xEC\x83\x81\xEB\x8C\x80\xEB\x8A\x94 \xEB\xB0\x9B\xEC\x9D\x80 \xEB\xA7\xB5\xEC\x9D\x84 \xEB\x82\xB4 \xEA\xB2\x8C\xEC\x9E\x84\xEC\x9D\xB4 \xEB\x82\xB4\xEB\xA0\xA4\xEB\xB0\x9B\xEC\xA7\x80 \xEB\xAA\xBB\xED\x95\x98\xEB\xA9\xB4 \xEB\xA7\xA4\xEC\xB9\x98\xEA\xB0\x80 \xEC\xB7\xA8\xEC\x86\x8C\xEB\x90\xA9\xEB\x8B\x88\xEB\x8B\xA4(\xEB\xA0\x88\xEC\x9D\xB4\xED\x8C\x85 \xEB\xB3\x80\xEB\x8F\x99 \xEC\x97\x86\xEC\x9D\x8C). \xEB\x8B\xA4\xEC\x9A\xB4\xEB\xA1\x9C\xEB\x93\x9C \xEC\x8B\xA4\xED\x8C\xA8\xEB\x8A\x94 \xEB\x8B\xB7\xEC\xA7\x80\xEA\xB0\x80 \xEC\x95\x84\xEB\x8B\x99\xEB\x8B\x88\xEB\x8B\xA4."),
            };
        return {
            "Leaving a found match before its battle starts (or disconnecting then) = 1 dodge.",
            fmt::format("Leaving after the roulette has chosen the map = {} dodges.", d.afterMap),
            fmt::format("{} dodges: your rating drops as if you lost {} games to a player of your rating, and "
                        "matchmaking is closed for {} minutes. The stack then stays at {}.",
                        d.limit, d.penaltyLosses, d.blockMinutes, std::max(0, d.limit - 1)),
            fmt::format("{} Ranked maps played normally clear the stack (wins, losses and draws).", d.clearMaps),
            d.downloadWeight > 0
                ? fmt::format("If your game cannot download a map your opponent has, the match is canceled. "
                              "The first {} in 2 hours only warn; each later one = {} dodges.",
                              d.downloadFree, d.downloadWeight)
                : std::string(
                      "If your game cannot download a map your opponent has, the match is canceled (rating unchanged). "
                      "A download fail is never a dodge."),
        };
    }
    // 스택 요약, 마지막 패널티, 규칙 목록을 다시 그린다.
    void render() {
        m_body->removeAllChildren();
        auto const& d = m_dodge;
        bool const ko = m_isKorean;
        int const left = std::max(1, d.clearMaps - d.cleanMaps);
        single(ko ? fmt::format("\xEB\x82\xB4 \xEB\x8B\xB7\xEC\xA7\x80 \xEC\x8A\xA4\xED\x83\x9D  {} / {}", d.stack,
                                d.limit)
                  : fmt::format("YOUR DODGE STACK  {} / {}", d.stack, d.limit),
               {190.f, 238.f}, 15.f, dodgeColor(d.stack, d.limit), TextWeight::Bold);
        single(
            d.stack > 0
                ? (ko ? (d.season2
                             ? fmt::format(
                                   "\xEC\x8B\x9C\xEC\xA6\x8C 2 \xEB\xA7\xB5\xEC\x9D\x84 {}\xED\x8C\x90 \xEB\x8D\x94 \xEC\xA0\x95\xEC\x83\x81\xEC\xA0\x81\xEC\x9C\xBC\xEB\xA1\x9C \xED\x94\x8C\xEB\xA0\x88\xEC\x9D\xB4\xED\x95\x98\xEB\xA9\xB4 \xEC\x8A\xA4\xED\x83\x9D\xEC\x9D\xB4 \xEC\x82\xAC\xEB\x9D\xBC\xEC\xA7\x91\xEB\x8B\x88\xEB\x8B\xA4.",
                                   left)
                             : fmt::format(
                                   "\xEB\x9E\xAD\xED\x81\xAC \xEB\xA7\xB5\xEC\x9D\x84 {}\xED\x8C\x90 \xEB\x8D\x94 \xEC\xA0\x95\xEC\x83\x81\xEC\xA0\x81\xEC\x9C\xBC\xEB\xA1\x9C \xED\x94\x8C\xEB\xA0\x88\xEC\x9D\xB4\xED\x95\x98\xEB\xA9\xB4 \xEC\x8A\xA4\xED\x83\x9D\xEC\x9D\xB4 \xEC\x82\xAC\xEB\x9D\xBC\xEC\xA7\x91\xEB\x8B\x88\xEB\x8B\xA4.",
                                   left))
                      : fmt::format("Play {} more {} map{} normally to clear it.", left,
                                    d.season2 ? "Season 2" : "Ranked", left == 1 ? "" : "s"))
                : std::string(
                      ko ? "\xEB\x8B\xB7\xEC\xA7\x80 \xEC\x97\x86\xEC\x9D\x8C. \xEC\xB0\xBE\xEC\x9D\x80 \xEB\xA7\xA4\xEC\xB9\x98\xEB\xA5\xBC \xEB\x81\x9D\xEA\xB9\x8C\xEC\xA7\x80 \xED\x94\x8C\xEB\xA0\x88\xEC\x9D\xB4\xED\x95\x98\xEB\xA9\xB4 \xEC\x9C\xA0\xEC\xA7\x80\xEB\x90\xA9\xEB\x8B\x88\xEB\x8B\xA4."
                         : "No dodges. Play your found matches to keep it that way."),
            {190.f, 219.f}, 12.5f, kIce);
        m_restriction = line(" ", {190.f, 202.f}, 12.5f, ccc3(255, 130, 90), TextWeight::Bold);
        m_restrictionText.clear();
        m_restriction->setVisible(false);
        if (d.penalties > 0 && d.penaltyAt > 0) {
            auto const when = localDateTime(d.penaltyAt);
            auto text =
                m_penalty
                    ? (ko ? fmt::format(
                                "\xEB\x8B\xB7\xEC\xA7\x80 {}\xEA\xB0\x9C \xEB\x8F\x84\xEB\x8B\xAC: \xEB\xA0\x88\xEC\x9D\xB4\xED\x8C\x85 {:+} ({})",
                                d.limit, d.penaltyDelta, when)
                          : fmt::format("You reached {} dodges: {:+} rating ({})", d.limit, d.penaltyDelta, when))
                    : (ko ? fmt::format(
                                "\xEB\xA7\x88\xEC\xA7\x80\xEB\xA7\x89 \xED\x8C\xA8\xEB\x84\x90\xED\x8B\xB0: \xEB\xA0\x88\xEC\x9D\xB4\xED\x8C\x85 {:+} ({})",
                                d.penaltyDelta, when)
                          : fmt::format("Last penalty: {:+} rating ({})", d.penaltyDelta, when));
            if (d.season2 && !d.penaltyCup.empty()) text += " - " + season2CupName(d.penaltyCup);
            single(text, {190.f, 185.f}, m_penalty ? 13.f : 12.f, m_penalty ? ccc3(255, 110, 90) : kMuted,
                   m_penalty ? TextWeight::Bold : TextWeight::Regular);
        }
        // 규칙 목록이 정해진 높이(122)에 들어갈 때까지 글자 크기를 12부터 9까지 줄여 가며
        // 배치한다.
        auto const texts = rules();
        for (float size : {12.f, 11.f, 10.f, 9.f}) {
            std::vector<CCSprite*> made;
            float height = 0.f;
            for (auto const& text : texts) {
                auto* bullet = createSystemText("\xE2\x80\xA2", size, 0.f, kCCTextAlignmentLeft, TextWeight::Bold);
                bullet->setColor(ccc3(255, 190, 105));
                auto* words = createSystemText(text, size, 322.f, kCCTextAlignmentLeft);
                made.push_back(bullet);
                made.push_back(words);
                height += words->getContentSize().height + 3.f;
            }
            if (height > 122.f && size > 9.f) continue;
            float y = 169.f;
            for (size_t i = 0; i + 1 < made.size(); i += 2) {
                made[i]->setAnchorPoint({0.f, 1.f});
                made[i]->setPosition({22.f, y});
                m_body->addChild(made[i]);
                made[i + 1]->setAnchorPoint({0.f, 1.f});
                made[i + 1]->setPosition({34.f, y});
                m_body->addChild(made[i + 1]);
                y -= made[i + 1]->getContentSize().height + 3.f;
            }
            break;
        }
        for (auto* item : {m_english, m_korean}) cascadeOpacity(item);
        m_english->setOpacity(ko ? 130 : 255);
        m_korean->setOpacity(ko ? 255 : 130);
        refresh(0.f);
    }
    // 표시 언어를 바꾸고 저장한 뒤 다시 그린다. (패치 노트와 같은 설정 키를 공유)
    void language(bool korean) {
        m_isKorean = korean;
        Mod::get()->setSavedValue("patch-notes-korean", korean);
        render();
    }
    void english(CCObject*) { language(false); }
    void korean(CCObject*) { language(true); }
    // 스택/패널티 정보를 받아 팝업을 구성한다. Season 2일 때만 기록(Log) 버튼을 둔다.
    // 0.5초마다 refresh로 매치 제한 남은 시간을 갱신한다.
    bool init(RankedDodge dodge, bool penalty) {
        if (!Popup::init(380.f, 286.f, "GJ_square02.png")) return false;
        m_dodge = std::move(dodge);
        m_penalty = penalty;
        setTitle("Dodge Penalty", "goldFont.fnt", .75f);
        setID("versus-dodge-info"_spr);
        panel(m_mainLayer, {14.f, 44.f}, {352.f, 128.f}, false);
        m_body = CCNode::create();
        m_mainLayer->addChild(m_body, 2);
        m_english = button(m_buttonMenu, this, menu_selector(DodgeInfoPopup::english), "English", {58.f, 24.f}, .42f);
        m_korean = button(m_buttonMenu, this, menu_selector(DodgeInfoPopup::korean), "Korean", {132.f, 24.f}, .42f);
        button(m_buttonMenu, this, menu_selector(DodgeInfoPopup::onOK), "OK", {318.f, 24.f}, .6f);
        if (m_dodge.season2)
            button(m_buttonMenu, this, menu_selector(DodgeInfoPopup::onLog), "Log", {229.f, 24.f}, .42f);
        m_isKorean = Mod::get()->getSavedValue<bool>("patch-notes-korean", true);
        render();
        schedule(schedule_selector(DodgeInfoPopup::refresh), .5f);
        return true;
    }
    // 매치메이킹 제한이 걸려 있으면 남은 시간(서버 시각 기준)을 표시하고, 문구가 바뀔 때만
    // 갱신한다.
    void refresh(float) {
        if (!m_restriction) return;
        auto const left = m_dodge.blockedUntil - Service::get().serverNow();
        std::string const text =
            left <= 0 ? std::string()
            : m_isKorean
                ? "\xEB\xA7\xA4\xEC\xB9\xAD \xEC\xA0\x9C\xED\x95\x9C: " + clockText(left) + " \xEB\x82\xA8\xEC\x9D\x8C"
                : "Matchmaking closed: " + clockText(left) + " left";
        if (text == m_restrictionText) return;
        m_restrictionText = text;
        m_restriction->setVisible(!text.empty());
        if (!text.empty()) setSystemText(m_restriction, text);
    }
    void onOK(CCObject*) { onClose(nullptr); }
    void onLog(CCObject*) {
        if (auto* popup = MyDodgeLogPopup::create(m_dodge.limit, m_isKorean)) popup->show();
    }

public:
    static DodgeInfoPopup* create(RankedDodge dodge, bool penalty = false) {
        auto* result = new DodgeInfoPopup;
        if (result->init(std::move(dodge), penalty)) {
            result->autorelease();
            return result;
        }
        delete result;
        return nullptr;
    }
};

// 맵 다운로드 실패로 매치가 취소되었을 때 뜨는 안내 팝업.
// 2시간 내 실패 횟수와 닷지 처리 여부, 해결 팁을 한/영으로 보여 준다.
class DownloadFailPopup final : public ::versus::DeferredPopup {
    static constexpr float kWidth = 400.f, kHeight = 300.f, kText = 364.f;
    RankedDownloadFail m_fail;
    bool m_isKorean = true;
    CCNode* m_body = nullptr;
    CCMenuItemSpriteExtra *m_english = nullptr, *m_korean = nullptr;
    // 다운로드 실패가 닷지로 집계되었는지(noDodge/dodged/미집계/경고만)에 따른 설명 문구를
    // 만든다.
    std::string stackText() const {
        auto const& f = m_fail;
        bool const ko = m_isKorean;
        if (f.noDodge)
            return ko ? std::string(
                            "\xEB\x8B\xA4\xEC\x9A\xB4\xEB\xA1\x9C\xEB\x93\x9C \xEC\x8B\xA4\xED\x8C\xA8\xEB\x8A\x94 \xEB\x8B\xB7\xEC\xA7\x80\xEA\xB0\x80 \xEC\x95\x84\xEB\x8B\x99\xEB\x8B\x88\xEB\x8B\xA4. \xEB\x8B\xB7\xEC\xA7\x80 \xEC\x8A\xA4\xED\x83\x9D\xEC\x9D\x80 \xEA\xB7\xB8\xEB\x8C\x80\xEB\xA1\x9C\xEC\x9E\x85\xEB\x8B\x88\xEB\x8B\xA4.")
                      : std::string("A download fail is never a dodge: your dodge stack is unchanged.");
        if (f.dodged) {
            auto text =
                ko ? fmt::format(
                         "2\xEC\x8B\x9C\xEA\xB0\x84 \xEC\x95\x88\xEC\x9D\x98 \xEB\x8B\xA4\xEC\x9A\xB4\xEB\xA1\x9C\xEB\x93\x9C \xEC\x8B\xA4\xED\x8C\xA8 {}\xED\x9A\x8C\xEC\xA7\xB8: \xEB\x8B\xB7\xEC\xA7\x80 {}\xEB\xA1\x9C \xEA\xB8\xB0\xEB\xA1\x9D\xEB\x90\x98\xEC\x97\x88\xEC\x8A\xB5\xEB\x8B\x88\xEB\x8B\xA4 (\xEB\x8B\xB7\xEC\xA7\x80 \xEC\x8A\xA4\xED\x83\x9D {}).",
                         f.count, f.weight, f.stack)
                   : fmt::format("Download fail {} in 2 hours: counted as {} dodges (dodge stack {}).", f.count,
                                 f.weight, f.stack);
            if (f.penalized)
                text +=
                    ko ? std::string(
                             " \xEB\x8B\xB7\xEC\xA7\x80 \xED\x8C\xA8\xEB\x84\x90\xED\x8B\xB0\xEA\xB0\x80 \xEC\xA0\x81\xEC\x9A\xA9\xEB\x90\x98\xEC\x97\x88\xEC\x8A\xB5\xEB\x8B\x88\xEB\x8B\xA4.")
                       : std::string(" The dodge penalty was applied.");
            return text;
        }
        if (f.count <= 0)
            return ko ? std::string(
                            "\xEC\x9D\xB4\xEB\xB2\x88 \xEC\x8B\xA4\xED\x8C\xA8\xEB\x8A\x94 \xEC\xA7\x91\xEA\xB3\x84\xEB\x90\x98\xEC\xA7\x80 \xEC\x95\x8A\xEC\x95\x98\xEC\x8A\xB5\xEB\x8B\x88\xEB\x8B\xA4(\xEC\x84\x9C\xEB\xB2\x84 \xEC\xA0\x90\xEA\xB2\x80 \xEC\xA4\x91). \xEB\x8B\xB7\xEC\xA7\x80 \xEA\xB8\xB0\xEB\xA1\x9D \xEC\x97\x86\xEC\x9D\x8C.")
                      : std::string("This one was not counted (server maintenance). No dodge.");
        int const left = std::max(0, f.free - f.count);
        if (left == 0)
            return ko ? fmt::format(
                            "2\xEC\x8B\x9C\xEA\xB0\x84 \xEC\x95\x88\xEC\x9D\x98 \xEB\x8B\xA4\xEC\x9A\xB4\xEB\xA1\x9C\xEB\x93\x9C \xEC\x8B\xA4\xED\x8C\xA8 {} / {}\xED\x9A\x8C (\xEA\xB2\xBD\xEA\xB3\xA0\xEB\xA7\x8C). \xEB\x8B\xA4\xEC\x9D\x8C \xEC\x8B\xA4\xED\x8C\xA8\xEB\xB6\x80\xED\x84\xB0\xEB\x8A\x94 \xEB\xA7\xA4\xEB\xB2\x88 \xEB\x8B\xB7\xEC\xA7\x80 {}\xEB\xA1\x9C \xEA\xB8\xB0\xEB\xA1\x9D\xEB\x90\xA9\xEB\x8B\x88\xEB\x8B\xA4.",
                            f.count, f.free, f.weight)
                      : fmt::format(
                            "Download fails in 2 hours: {} / {} (warning only). From the next one, each fail counts as {} dodges.",
                            f.count, f.free, f.weight);
        return ko ? fmt::format(
                        "2\xEC\x8B\x9C\xEA\xB0\x84 \xEC\x95\x88\xEC\x9D\x98 \xEB\x8B\xA4\xEC\x9A\xB4\xEB\xA1\x9C\xEB\x93\x9C \xEC\x8B\xA4\xED\x8C\xA8 {} / {}\xED\x9A\x8C (\xEA\xB2\xBD\xEA\xB3\xA0\xEB\xA7\x8C). {}\xEB\xB2\x88 \xEB\x8D\x94 \xEC\x8B\xA4\xED\x8C\xA8\xED\x95\x98\xEB\xA9\xB4 \xEA\xB7\xB8 \xEB\x92\xA4\xEB\xA1\x9C\xEB\x8A\x94 \xEB\xA7\xA4\xEB\xB2\x88 \xEB\x8B\xB7\xEC\xA7\x80 {}\xEB\xA1\x9C \xEA\xB8\xB0\xEB\xA1\x9D\xEB\x90\xA9\xEB\x8B\x88\xEB\x8B\xA4.",
                        f.count, f.free, left, f.weight)
                  : fmt::format(
                        "Download fails in 2 hours: {} / {} (warning only). After {} more, each fail counts as {} dodges.",
                        f.count, f.free, left, f.weight);
    }
    // 연결 문제 해결 팁 목록 (인터넷, VPN/프록시, 방화벽, GD 온라인 레벨 확인).
    std::vector<std::string> tips() const {
        if (m_isKorean)
            return {
                "\xEC\x9D\xB8\xED\x84\xB0\xEB\x84\xB7 \xEC\x97\xB0\xEA\xB2\xB0\xEC\x9D\xB4 \xEC\x95\x88\xEC\xA0\x95\xEC\xA0\x81\xEC\x9D\xB8\xEC\xA7\x80 \xED\x99\x95\xEC\x9D\xB8\xED\x95\xB4 \xEC\xA3\xBC\xEC\x84\xB8\xEC\x9A\x94(\xEA\xB0\x80\xEB\x8A\xA5\xED\x95\x98\xEB\xA9\xB4 \xEC\x9C\xA0\xEC\x84\xA0 \xEC\x97\xB0\xEA\xB2\xB0, \xEA\xB0\x99\xEC\x9D\x80 \xEB\x84\xA4\xED\x8A\xB8\xEC\x9B\x8C\xED\x81\xAC\xEC\x9D\x98 \xED\x81\xB0 \xEB\x8B\xA4\xEC\x9A\xB4\xEB\xA1\x9C\xEB\x93\x9C\xEB\x8A\x94 \xEC\x9E\xA0\xEC\x8B\x9C \xEB\xA9\x88\xEC\xB6\xA4).",
                "VPN\xC2\xB7\xED\x94\x84\xEB\xA1\x9D\xEC\x8B\x9C\xEB\xA5\xBC \xEA\xBA\xBC \xEC\xA3\xBC\xEC\x84\xB8\xEC\x9A\x94. \xEC\x82\xAC\xEC\x9A\xA9 \xEC\xA4\x91\xEC\x9D\xB8 \xEB\x84\xA4\xED\x8A\xB8\xEC\x9B\x8C\xED\x81\xAC\xEA\xB0\x80 GD \xEC\x84\x9C\xEB\xB2\x84\xEB\xA5\xBC \xEB\xA7\x89\xEB\x8A\x94\xEB\x8B\xA4\xEB\xA9\xB4 GD \xEC\x84\x9C\xEB\xB2\x84\xEC\x97\x90 \xEC\xA0\x91\xEC\x86\x8D\xEB\x90\x98\xEB\x8A\x94 VPN\xEC\x9D\x84 \xEC\x82\xAC\xEC\x9A\xA9\xED\x95\xB4 \xEC\xA3\xBC\xEC\x84\xB8\xEC\x9A\x94.",
                "\xEB\xB0\xA9\xED\x99\x94\xEB\xB2\xBD\xC2\xB7\xEB\xB0\xB1\xEC\x8B\xA0 \xED\x94\x84\xEB\xA1\x9C\xEA\xB7\xB8\xEB\x9E\xA8\xEC\x97\x90\xEC\x84\x9C Geometry Dash\xEB\xA5\xBC \xED\x97\x88\xEC\x9A\xA9\xED\x95\xB4 \xEC\xA3\xBC\xEC\x84\xB8\xEC\x9A\x94.",
                "GD\xEC\x97\x90\xEC\x84\x9C \xEC\x98\xA8\xEB\x9D\xBC\xEC\x9D\xB8 \xEB\xA0\x88\xEB\xB2\xA8 \xEA\xB2\x80\xEC\x83\x89\xEA\xB3\xBC \xEB\x8B\xA4\xEC\x9A\xB4\xEB\xA1\x9C\xEB\x93\x9C\xEA\xB0\x80 \xEC\xA0\x95\xEC\x83\x81\xEC\xA0\x81\xEC\x9C\xBC\xEB\xA1\x9C \xEB\x90\x98\xEB\x8A\x94\xEC\xA7\x80 \xED\x99\x95\xEC\x9D\xB8\xED\x95\xB4 \xEC\xA3\xBC\xEC\x84\xB8\xEC\x9A\x94.",
            };
        return {
            "Check that your internet connection is stable (use a cable if you can, pause other big downloads).",
            "Turn off any VPN or proxy. If your network blocks GD's servers, use a VPN that can reach them.",
            "Allow Geometry Dash through your firewall and antivirus.",
            "Make sure you can search and download online levels in GD itself.",
        };
    }
    // 제목, 취소 안내, 실패 집계, 초기화 시각, 팁 패널을 그린다.
    // 팁은 패널 높이에 들어갈 때까지 글자 크기를 줄여 가며 배치한다.
    void render() {
        m_body->removeAllChildren();
        bool const ko = m_isKorean;
        float y = kHeight - 40.f;
        auto add = [&](std::string const& text, float size, ccColor3B color, TextWeight weight = TextWeight::Regular) {
            auto* node = createSystemText(text, size, kText, kCCTextAlignmentCenter, weight);
            node->setColor(color);
            node->setAnchorPoint({.5f, 1.f});
            node->setPosition({kWidth / 2.f, y});
            m_body->addChild(node, 2);
            y -= node->getContentSize().height + 4.f;
        };
        add(ko ? std::string(
                     "\xEB\xA7\xB5\xEC\x9D\x84 \xEB\x82\xB4\xEB\xA0\xA4\xEB\xB0\x9B\xEC\xA7\x80 \xEB\xAA\xBB\xED\x96\x88\xEC\x8A\xB5\xEB\x8B\x88\xEB\x8B\xA4")
               : std::string("Your game could not download the map"),
            15.f, ccc3(255, 130, 90), TextWeight::Bold);
        add(ko ? std::string(
                     "\xEB\xA7\xA4\xEC\xB9\x98\xEA\xB0\x80 \xEC\xB7\xA8\xEC\x86\x8C\xEB\x90\x98\xEC\x97\x88\xEC\x8A\xB5\xEB\x8B\x88\xEB\x8B\xA4(\xEB\xA0\x88\xEC\x9D\xB4\xED\x8C\x85 \xEB\xB3\x80\xEB\x8F\x99 \xEC\x97\x86\xEC\x9D\x8C). \xEC\x83\x81\xEB\x8C\x80\xEB\x8A\x94 \xEA\xB0\x99\xEC\x9D\x80 \xEB\xA7\xB5\xEC\x9D\x84 \xEB\xB0\x9B\xEC\x95\x98\xEC\x9C\xBC\xEB\xAF\x80\xEB\xA1\x9C \xEB\x82\xB4 \xEC\x97\xB0\xEA\xB2\xB0 \xEC\xAA\xBD \xEB\xAC\xB8\xEC\xA0\x9C\xEC\x9D\xBC \xEA\xB0\x80\xEB\x8A\xA5\xEC\x84\xB1\xEC\x9D\xB4 \xEB\x86\x92\xEC\x8A\xB5\xEB\x8B\x88\xEB\x8B\xA4.")
               : std::string("The match was canceled (rating unchanged). Your opponent got the same map, "
                             "so your connection is the likely cause."),
            11.f, kIce);
        add(stackText(), 12.f, m_fail.dodged ? ccc3(255, 110, 90) : ccc3(255, 200, 110), TextWeight::Bold);
        if (m_fail.resetAt > 0) {
            auto const when = localDateTime(m_fail.resetAt);
            add(ko ? fmt::format(
                         "{}\xEC\x97\x90 \xEC\x8B\xA4\xED\x8C\xA8 \xED\x9A\x9F\xEC\x88\x98\xEA\xB0\x80 0\xEC\x9C\xBC\xEB\xA1\x9C \xEC\xB4\x88\xEA\xB8\xB0\xED\x99\x94\xEB\x90\xA9\xEB\x8B\x88\xEB\x8B\xA4.",
                         when)
                   : fmt::format("The count starts over at {}.", when),
                10.5f, kMuted);
        }
        float const top = y - 2.f, bottom = 44.f;
        panel(m_body, {14.f, bottom}, {kWidth - 28.f, std::max(30.f, top - bottom)}, false);
        auto const texts = tips();
        for (float size : {10.5f, 10.f, 9.f, 8.f, 7.f}) {
            std::vector<CCSprite*> made;
            float height = 0.f;
            for (auto const& text : texts) {
                auto* bullet = createSystemText("\xE2\x80\xA2", size, 0.f, kCCTextAlignmentLeft, TextWeight::Bold);
                bullet->setColor(ccc3(255, 190, 105));
                auto* words = createSystemText(text, size, kText - 22.f, kCCTextAlignmentLeft);
                made.push_back(bullet);
                made.push_back(words);
                height += words->getContentSize().height + 3.f;
            }
            if (height > top - bottom - 6.f && size > 7.f) continue;
            float row = top - 4.f;
            for (size_t i = 0; i + 1 < made.size(); i += 2) {
                made[i]->setAnchorPoint({0.f, 1.f});
                made[i]->setPosition({24.f, row});
                m_body->addChild(made[i], 2);
                made[i + 1]->setAnchorPoint({0.f, 1.f});
                made[i + 1]->setPosition({36.f, row});
                m_body->addChild(made[i + 1], 2);
                row -= made[i + 1]->getContentSize().height + 3.f;
            }
            break;
        }
        for (auto* item : {m_english, m_korean}) cascadeOpacity(item);
        m_english->setOpacity(ko ? 130 : 255);
        m_korean->setOpacity(ko ? 255 : 130);
    }
    void language(bool korean) {
        m_isKorean = korean;
        Mod::get()->setSavedValue("patch-notes-korean", korean);
        render();
    }
    void english(CCObject*) { language(false); }
    void korean(CCObject*) { language(true); }
    bool init(RankedDownloadFail fail) {
        if (!Popup::init(kWidth, kHeight, "GJ_square02.png")) return false;
        m_fail = std::move(fail);
        setTitle("Map Download Failed", "goldFont.fnt", .75f);
        setID("versus-download-fail"_spr);
        m_body = CCNode::create();
        m_mainLayer->addChild(m_body, 2);
        m_english =
            button(m_buttonMenu, this, menu_selector(DownloadFailPopup::english), "English", {58.f, 24.f}, .42f);
        m_korean = button(m_buttonMenu, this, menu_selector(DownloadFailPopup::korean), "Korean", {132.f, 24.f}, .42f);
        button(m_buttonMenu, this, menu_selector(DownloadFailPopup::onOK), "OK", {kWidth - 62.f, 24.f}, .6f);
        m_isKorean = Mod::get()->getSavedValue<bool>("patch-notes-korean", true);
        render();
        return true;
    }
    void onOK(CCObject*) { onClose(nullptr); }

public:
    static DownloadFailPopup* create(RankedDownloadFail fail) {
        auto* result = new DownloadFailPopup;
        if (result->init(std::move(fail))) {
            result->autorelease();
            return result;
        }
        delete result;
        return nullptr;
    }
};

// Ranked에서 별/오브 획득 기능을 켤 때 보여 주는 동의 팝업.
// 대전이 실제 GD 저장 맵으로 진행되어 별/오브 합계에 영향을 줄 수 있음을 알리고,
// "이해합니다"를 누르면 ranked-rewards 설정을 저장한 뒤 콜백을 호출한다.
class RankedRewardsPopup final : public ::versus::DeferredPopup {
    std::function<void()> m_enabled;
    bool m_isKorean = true;
    CCNode* m_body = nullptr;
    CCMenuItemSpriteExtra *m_english = nullptr, *m_korean = nullptr, *m_accept = nullptr;
    // 안내 문구 4개 (세 번째는 경고라 render에서 강조 표시한다).
    std::vector<std::string> rules() const {
        if (m_isKorean)
            return {
                "Versus \xEB\x9E\xAD\xED\x81\xAC \xEA\xB2\x8C\xEC\x9E\x84\xEC\x97\x90\xEC\x84\x9C \xED\x94\x8C\xEB\xA0\x88\xEC\x9D\xB4\xED\x95\x9C \xEB\xA7\xB5\xEC\x9D\x98 \xEC\xA7\x84\xED\x96\x89\xEB\x8F\x84(\xEC\x8B\x9C\xEB\x8F\x84 \xED\x9A\x9F\xEC\x88\x98\xC2\xB7\xEC\xB5\x9C\xEA\xB3\xA0 %\xC2\xB7\xED\x81\xB4\xEB\xA6\xAC\xEC\x96\xB4)\xEC\x99\x80 \xEC\x98\xA4\xEB\xB8\x8C, \xEB\xB3\x84\xEC\x9D\x98 \xEA\xB0\x9C\xEC\x88\x98\xEA\xB0\x80 GD\xEC\x97\x90 \xEB\xB0\x98\xEC\x98\x81\xEB\x90\xA9\xEB\x8B\x88\xEB\x8B\xA4.", "\xEB\x8C\x80\xEA\xB2\xB0\xEC\x9D\x80 \xEB\xB3\xB5\xEC\x82\xAC\xEB\xB3\xB8\xEC\x9D\xB4 \xEC\x95\x84\xEB\x8B\x88\xEB\x9D\xBC GD\xEC\x97\x90 \xEC\xA0\x80\xEC\x9E\xA5\xEB\x90\x9C \xEC\xA7\x84\xEC\xA7\x9C \xEB\xA7\xB5\xEC\x9C\xBC\xEB\xA1\x9C \xEC\xA7\x84\xED\x96\x89\xEB\x90\x98\xEA\xB3\xA0, \xEA\xB8\xB0\xEB\xA1\x9D\xEA\xB3\xBC \xEB\xB3\xB4\xEC\x83\x81\xEC\x9D\x80 GD\xEA\xB0\x80 \xED\x8F\x89\xEC\x86\x8C \xED\x94\x8C\xEB\xA0\x88\xEC\x9D\xB4\xEC\xB2\x98\xEB\x9F\xBC \xEC\xA7\x81\xEC\xA0\x91 \xEC\xA0\x80\xEC\x9E\xA5\xED\x95\xA9\xEB\x8B\x88\xEB\x8B\xA4. \xEB\xB0\x9B\xEC\x9D\x80 \xEC\x98\xA4\xEB\xB8\x8C\xC2\xB7\xEB\xB3\x84\xEC\x9D\x80 \xEB\x8C\x80\xEA\xB2\xB0\xEC\x9D\xB4 \xEB\x81\x9D\xEB\x82\x9C \xEB\x92\xA4 \xED\x95\x9C \xEB\xB2\x88\xEC\x97\x90 \xEB\xB3\xB4\xEC\x97\xAC \xEB\x93\x9C\xEB\xA6\xBD\xEB\x8B\x88\xEB\x8B\xA4.", "\xEC\x9D\xB4 \xEA\xB8\xB0\xEB\x8A\xA5\xEC\x9D\x84 \xEC\x82\xAC\xEC\x9A\xA9\xED\x95\x98\xEB\xA9\xB4 \xEC\x8B\xA4\xEC\xA0\x9C\xEB\xA1\x9C GD \xEB\xA6\xAC\xEB\x8D\x94\xEB\xB3\xB4\xEB\x93\x9C\xEC\x9D\x98 \xEB\xB3\x84/\xEC\x98\xA4\xEB\xB8\x8C \xEC\x88\x98\xEC\xA7\x91\xEC\x97\x90 \xEC\x98\x81\xED\x96\xA5\xEC\x9D\xB4 \xEA\xB0\x88 \xEC\x88\x98 \xEC\x9E\x88\xEC\x9C\xBC\xEB\xAF\x80\xEB\xA1\x9C, \xEC\x9B\x90\xEC\xB9\x98 \xEC\x95\x8A\xEC\x9C\xBC\xEC\x8B\x9C\xEB\x8A\x94 \xEB\xB6\x84\xEB\x93\xA4\xEC\x9D\x80 \xEC\x9D\xB4 \xEA\xB8\xB0\xEB\x8A\xA5\xEC\x9D\x84 \xEA\xBA\xBC \xEC\xA3\xBC\xEC\x8B\x9C\xEA\xB8\xB0 \xEB\xB0\x94\xEB\x9E\x8D\xEB\x8B\x88\xEB\x8B\xA4.", "\xEC\x8B\x9C\xEC\x9E\x91 \xEC\x9C\x84\xEC\xB9\x98(Start Pos)\xEA\xB0\x80 \xEC\x9E\x88\xEB\x8A\x94 \xEB\xA7\xB5\xEA\xB3\xBC \xED\x95\xB5\xEC\x9D\xB4 \xEC\xBC\x9C\xEC\xA7\x84 \xEC\x83\x81\xED\x83\x9C\xEC\x9D\x98 \xED\x81\xB4\xEB\xA6\xAC\xEC\x96\xB4\xEB\x8A\x94 \xEB\xB0\x98\xEC\x98\x81\xEB\x90\x98\xEC\xA7\x80 \xEC\x95\x8A\xEC\x8A\xB5\xEB\x8B\x88\xEB\x8B\xA4. \xEC\x96\xB8\xEC\xA0\x9C\xEB\x93\xA0 \xEB\x81\x8C \xEC\x88\x98 \xEC\x9E\x88\xEC\x8A\xB5\xEB\x8B\x88\xEB\x8B\xA4."};
        return {
            "The progress (attempts, best %, clears), orbs and stars of the maps you play in Versus Ranked games count in GD.",
            "The battle is played on the real map saved in GD, not a copy, and GD saves the records and rewards itself, just like normal play. What you earned is shown once the battle is over.",
            "Using this can actually affect your star / orb totals on GD's leaderboards. If you do not want that, please keep this feature off.",
            "Maps with start positions and clears made with a hack on do not count. You can turn it off at any time."};
    }
    // 'I Understand' 버튼 스프라이트. 시스템 폰트로 언어별 텍스트를 얹는다.
    static CCSprite* acceptSprite(bool korean) {
        auto* sprite = ButtonSprite::create(" ", 92, true, "goldFont.fnt", "GJ_button_01.png", 30.f, .6f);
        auto* text =
            createSystemText(korean ? "\xEC\x9D\xB4\xED\x95\xB4\xED\x95\xA9\xEB\x8B\x88\xEB\x8B\xA4" : "I Understand",
                             13.f, 0.f, kCCTextAlignmentCenter, TextWeight::Bold);
        text->setAnchorPoint({.5f, .5f});
        text->setPosition({sprite->getContentSize().width / 2.f, sprite->getContentSize().height / 2.f + 1.f});
        sprite->addChild(text, 5);
        return sprite;
    }
    void render() {
        m_body->removeAllChildren();
        bool const ko = m_isKorean;
        auto* head = createSystemText(
            ko ? "\xEB\x9E\xAD\xED\x81\xAC\xEC\x97\x90\xEC\x84\x9C \xEB\xB3\x84\xEA\xB3\xBC \xEC\x98\xA4\xEB\xB8\x8C \xEB\xB0\x9B\xEA\xB8\xB0"
               : "Earn stars & orbs in Ranked",
            15.f, 0.f, kCCTextAlignmentCenter, TextWeight::Bold);
        head->setAnchorPoint({.5f, .5f});
        head->setPosition({190.f, 214.f});
        head->setColor(ccc3(255, 222, 90));
        m_body->addChild(head);
        float const half = head->getContentSize().width / 2.f;
        if (auto* star = loadedFrameSprite("GJ_starsIcon_001.png", "GJ_GameSheet03.plist")) {
            star->setScale(16.f / std::max(1.f, star->getContentSize().height));
            star->setPosition({190.f - half - 14.f, 214.f});
            m_body->addChild(star);
        }
        if (auto* orb = loadedFrameSprite("currencyOrbIcon_001.png", "GJ_GameSheet03.plist")) {
            orb->setScale(17.f / std::max(1.f, orb->getContentSize().height));
            orb->setPosition({190.f + half + 14.f, 214.f});
            m_body->addChild(orb);
        }
        auto const texts = rules();
        for (float size : {12.f, 11.f, 10.f, 9.f, 8.f}) {
            std::vector<CCSprite*> made;
            float height = 0.f;
            for (size_t i = 0; i < texts.size(); ++i) {
                auto* bullet = createSystemText("\xE2\x80\xA2", size, 0.f, kCCTextAlignmentLeft, TextWeight::Bold);
                bullet->setColor(ccc3(255, 190, 105));
                bool const warning = i == 2;
                auto* words = createSystemText(texts[i], size, 322.f, kCCTextAlignmentLeft,
                                               warning ? TextWeight::Bold : TextWeight::Regular);
                if (warning) words->setColor(ccc3(255, 140, 100));
                made.push_back(bullet);
                made.push_back(words);
                height += words->getContentSize().height + 4.f;
            }
            if (height > 150.f && size > 8.f) continue;
            float y = 197.f;
            for (size_t i = 0; i + 1 < made.size(); i += 2) {
                made[i]->setAnchorPoint({0.f, 1.f});
                made[i]->setPosition({22.f, y});
                m_body->addChild(made[i]);
                made[i + 1]->setAnchorPoint({0.f, 1.f});
                made[i + 1]->setPosition({34.f, y});
                m_body->addChild(made[i + 1]);
                y -= made[i + 1]->getContentSize().height + 4.f;
            }
            break;
        }
        m_accept->setSprite(acceptSprite(ko));
        for (auto* item : {m_english, m_korean}) cascadeOpacity(item);
        m_english->setOpacity(ko ? 130 : 255);
        m_korean->setOpacity(ko ? 255 : 130);
    }
    void language(bool korean) {
        m_isKorean = korean;
        Mod::get()->setSavedValue("patch-notes-korean", korean);
        render();
    }
    void english(CCObject*) { language(false); }
    void korean(CCObject*) { language(true); }
    void onCancel(CCObject*) { onClose(nullptr); }
    // 동의 저장 후 팝업을 닫고, 닫은 다음에 콜백을 실행한다.
    void onAccept(CCObject*) {
        Mod::get()->setSavedValue("ranked-rewards", true);
        auto done = std::move(m_enabled);
        onClose(nullptr);
        if (done) done();
    }
    bool init(std::function<void()> enabled) {
        if (!Popup::init(380.f, 262.f, "GJ_square02.png")) return false;
        m_enabled = std::move(enabled);
        setTitle("Stars & Orbs", "goldFont.fnt", .75f);
        setID("versus-ranked-rewards"_spr);
        panel(m_mainLayer, {14.f, 44.f}, {352.f, 158.f}, false);
        m_body = CCNode::create();
        m_mainLayer->addChild(m_body, 2);
        m_english =
            button(m_buttonMenu, this, menu_selector(RankedRewardsPopup::english), "English", {50.f, 24.f}, .42f);
        m_korean = button(m_buttonMenu, this, menu_selector(RankedRewardsPopup::korean), "Korean", {118.f, 24.f}, .42f);
        button(m_buttonMenu, this, menu_selector(RankedRewardsPopup::onCancel), "Cancel", {224.f, 24.f}, .5f,
               "GJ_button_04.png");
        m_accept = CCMenuItemSpriteExtra::create(acceptSprite(true), this, menu_selector(RankedRewardsPopup::onAccept));
        m_accept->setPosition({314.f, 24.f});
        m_accept->setID("ranked-rewards-accept"_spr);
        m_buttonMenu->addChild(m_accept);
        m_isKorean = Mod::get()->getSavedValue<bool>("patch-notes-korean", true);
        render();
        return true;
    }

public:
    static RankedRewardsPopup* create(std::function<void()> enabled) {
        auto* result = new RankedRewardsPopup;
        if (result->init(std::move(enabled))) {
            result->autorelease();
            return result;
        }
        delete result;
        return nullptr;
    }
};

// Ranked 메인 화면 레이어. 매치 탐색/대전 준비/결과/듀얼 요청 등 RankedState를 받아
// 화면을 갱신하는 SceneLayer로, Season 1과 Season 2 화면을 모두 담당한다.
// 티어(band) 테마 배경, 내 카드, 리더보드, 신호(signal), 다운로드 진행 표시 등을
// 가진다.
class RankedLayer final : public SceneLayer {
    CCNode* m_root = nullptr;
    CCNode* m_preview = nullptr;
    RankedBackground* m_background = nullptr;
    NineSlice* m_backdrop = nullptr;
    std::vector<NineSlice*> m_framePanels;
    // 티어(band) 테마가 바뀔 때 m_bandFrom -> m_bandTo 로 m_bandBlend(0~1)만큼 섞어
    // 전환한다.
    std::string m_band;
    RankedBandTheme m_bandFrom = rankedBandTheme(""), m_bandTo = rankedBandTheme("");
    float m_bandBlend = 1.f;
    CCNode* m_bandCard = nullptr;
    std::string m_bandCardKey;
    CCSprite* m_face = nullptr;
    CCNode* m_aura = nullptr;
    CCNode* m_titleGlow = nullptr;
    CCNode* m_ratingGlow = nullptr;
    CCNode* m_selfCard = nullptr;
    CCMenu* m_boardActions = nullptr;
    CCLabelBMFont* m_seasonLeft = nullptr;
    bool m_season2 = false;
    bool m_keepMusic = false;
    std::string m_seasonShown;
    float m_seasonTimer = 0.f, m_seasonWidth = 200.f;
    // 시즌 남은 시간 라벨 갱신. Season 1/2 화면에 따라 종료 안내 또는 남은 시간을
    // 표시한다.
    void refreshSeason() {
        if (!m_seasonLeft) return;
        auto const& service = Service::get();
        auto text =
            m_season2
                ? (service.season() >= 2 ? seasonTimeLeft(service.season(), service.seasonEndsAt(), service.serverNow())
                                         : std::string("SEASON 2"))
            : service.season() >= 2 ? std::string("SEASON 1 HAS ENDED")
                                    : seasonTimeLeft(service.season(), service.seasonEndsAt(), service.serverNow());
        if (text == m_seasonShown) return;
        m_seasonShown = text;
        m_seasonLeft->setString(text.c_str());
        m_seasonLeft->limitLabelWidth(m_seasonWidth, .28f, .16f);
    }
    static constexpr float kToolsCompactY = 88.f, kToolsIdleY = 62.f, kToolsIdleScale = 1.33f;
    float m_toolsX = 0.f;
    bool m_toolsExpanded = false;
    int m_selfRank = -1;
    CCNode* m_signals = nullptr;
    CCLabelBMFont* m_downloadText[2] = {};
    CCDrawNode* m_downloadFill[2] = {};
    float m_downloadWidth = 0.f, m_progressTimer = 0.f;
    float m_downloadBarH = 6.f, m_downloadTextScale = .4f, m_downloadTextWidth = 0.f;
    CCNode* m_downloadRow = nullptr;
    CCMenuItemSpriteExtra* m_confirm = nullptr;
    CCMenuItemSpriteExtra* m_runnerButton = nullptr;
    float m_signalCooldown = 0.f;
    bool m_sendingSignal = false;
    std::string m_seenSignal;
    Ref<SpectatorRunner> m_queueRunner;
    Ref<QueueGamePopup> m_queueGame;
    uint64_t m_queueGameGeneration = 0;
    float m_actionCenter = 0.f;
    std::string m_previewDraw, m_observedDraw, m_failedDraw, m_failureReason, m_dismissedResult;
    bool m_failureMapSide = false;
    int64_t m_observedSelection = 0;
    size_t m_notifiedRerolls = 0;
    bool m_rerollSending = false;
    CCMenuItemSpriteExtra* m_voteButton = nullptr;
    std::string m_voteText;
    bool m_voteSending = false;
    int m_countdownShown = -1;
    static constexpr int64_t kSearchDrift = 3000;
    int64_t m_searchSince = 0;
    std::chrono::steady_clock::time_point m_searchAnchor{};
    std::string m_opponentVoteSeen, m_voteRerollMatch;
    int m_voteRerollsSeen = 0;
    RankedSearchBoard* m_searchBoard = nullptr;
    Ref<RankedDuelRequestPopup> m_duelIncomingPopup;
    Ref<RankedDuelWaitPopup> m_duelWaitPopup;
    std::string m_duelIncomingSeen, m_duelIncomingFinal, m_duelOutgoingSeen, m_duelOutgoingFinal;
    // 서버 응답(RankedState)을 이 레이어로 전달하는 콜백. 전환 중이거나 레이어가
    // 사라졌으면 무시한다.
    std::function<void(RankedState)> stateSink() {
        return [self = Weak<RankedLayer>(this)](RankedState value) {
            if (auto owner = self.lock(); owner && !owner->m_transitioning) owner->receive(std::move(value));
        };
    }
    // 듀얼 요청 팝업을 정리한다. 내가 보낸 요청이 아직 최종 상태가 아니면 서버에 취소를
    // 보낸다.
    void closeDuelPopups() {
        if (m_duelIncomingPopup) m_duelIncomingPopup->dismiss();
        if (m_duelWaitPopup) {
            if (m_duelWaitPopup->getParent() && m_duelOutgoingFinal != m_duelWaitPopup->requestId())
                Service::get().cancelDuel(m_duelWaitPopup->requestId(), [](std::optional<RankedState>, std::string) {});
            m_duelWaitPopup->dismiss();
        }
        m_duelIncomingPopup = nullptr;
        m_duelWaitPopup = nullptr;
    }
    // 상태에 포함된 들어온/보낸 듀얼 요청을 처리한다.
    // 새 pending 요청이면 팝업(알림음 포함)을 열고, 최종 상태(취소/만료/거절/무효)가 되면
    // 팝업을 닫고 사유를 알린다. 이미 매치 중이면 새 팝업은 띄우지 않는다.
    void handleDuels(RankedState const& state) {
        if (auto const& request = state.duelIncoming) {
            auto popup = m_duelIncomingPopup;
            bool const open = popup && popup->requestId() == request->id;
            std::string const name = request->player ? request->player->player.name : std::string("The player");
            if (request->state == "pending") {
                if (m_duelIncomingSeen != request->id && !state.match) {
                    m_duelIncomingSeen = request->id;
                    if (popup) popup->dismiss();
                    audio::playBuiltin("highscoreGet02.ogg");
                    if (auto* next = RankedDuelRequestPopup::create(*request, stateSink())) {
                        next->show();
                        m_duelIncomingPopup = next;
                    }
                }
            } else if (m_duelIncomingFinal != request->id) {
                m_duelIncomingFinal = request->id;
                if (open) popup->dismiss();
                bool const seen = m_duelIncomingSeen == request->id;
                if (seen && request->state == "canceled")
                    Notification::create(fmt::format("{} canceled the duel request.", name), NotificationIcon::Info,
                                         2.5f)
                        ->show();
                else if (seen && open && request->state == "expired")
                    Notification::create("The duel request expired.", NotificationIcon::Info, 2.f)->show();
            }
        }
        if (auto const& request = state.duelOutgoing) {
            auto popup = m_duelWaitPopup;
            bool const open = popup && popup->requestId() == request->id;
            std::string const name = request->player ? request->player->player.name : std::string("The player");
            if (request->state == "pending") {
                if (m_duelOutgoingSeen != request->id && !state.match) {
                    m_duelOutgoingSeen = request->id;
                    if (popup) popup->dismiss();
                    if (auto* next = RankedDuelWaitPopup::create(*request, stateSink())) {
                        next->show();
                        m_duelWaitPopup = next;
                    }
                }
            } else if (m_duelOutgoingFinal != request->id) {
                m_duelOutgoingFinal = request->id;
                if (open) popup->dismiss();
                if (m_duelOutgoingSeen == request->id) {
                    std::string message;
                    if (request->state == "declined")
                        message = fmt::format("<cy>{}</c> declined your duel request.", name);
                    else if (request->state == "expired")
                        message = fmt::format("<cy>{}</c> did not answer your duel request.", name);
                    else if (request->state == "void") {
                        auto const& why = request->reason;
                        message = why == "own"         ? std::string()
                                  : why == "searching" ? fmt::format("<cy>{}</c> started searching for a match.", name)
                                  : why == "matched"   ? fmt::format("<cy>{}</c> found another match.", name)
                                  : why == "unavailable" ? fmt::format("<cy>{}</c> can't play right now.", name)
                                  : why == "gone"        ? fmt::format("<cy>{}</c> is no longer available.", name)
                                                         : fmt::format("<cy>{}</c> is no longer searching.", name);
                    }
                    if (!message.empty()) FLAlertLayer::create("Duel", message.c_str(), "OK")->show();
                }
            }
        }
    }
    CCLabelBMFont* m_delta = nullptr;
    std::string m_faceTier;
    std::string m_introMatch;
    float m_introTime = 0.f;
    float m_twoPlayerScale = 0.f;
    float m_livesX = -1.f, m_lengthX = -1.f;
    float m_livesScale = .36f, m_lengthScale = .42f, m_heartWidth = 13.f;
    // 2인 대결 UI에서 쓰는 빨간색.
    static constexpr ccColor3B kTwoPlayerRed{255, 70, 70};
    bool m_tierAnimated = false;
    // Season 2 맵 추첨 연출: kS2DrawHold 초 동안 상태를 붙잡아 두고, kS2DrawGiveUp 초가
    // 지나면 포기한다.
    static constexpr float kS2DrawHold = 3.f, kS2DrawGiveUp = 20.f;
    float m_s2DrawShown = -1.f;
    std::optional<RankedState> m_s2HeldState;
    // Season 2 결과 화면에서 추첨 연출이 끝나기를 기다리는 중인지.
    bool season2DrawWaiting() const {
        return m_season2 && m_lastResult && m_s2DrawShown >= 0.f && m_s2DrawShown < kS2DrawGiveUp;
    }
    float m_leftWidth = 0.f;
    float m_height = 0.f;
    std::string m_actionText;
    bool m_ldmSending = false;
    CCMenuItemToggler* m_ldmToggle = nullptr;
    CCLabelBMFont* m_opponentLdm = nullptr;
    uint64_t m_stateRevision = 1;
    int m_ldmShown = -1;
    // LDM 토글: 내가 아직 준비(ready)하지 않았고 전송 중이 아닐 때만 저장된 ranked-ldm
    // 값을 반전한다.
    void onLdm(CCObject*) {
        if (!m_state || !m_state->match || m_state->match->ownReady || m_readySending || m_ldmSending) return;
        bool const value = !Mod::get()->getSavedValue<bool>("ranked-ldm", false);
        Mod::get()->setSavedValue("ranked-ldm", value);
    }
    // 내/상대 LDM 표시를 갱신한다. 토글은 맵 탐색/준비 단계에서 준비 전일 때만 조작할 수
    // 있다.
    // 표시 상태가 이전과 같으면 아무것도 하지 않는다.
    void updateLdmControls() {
        if (!m_state || !m_state->match || !m_ldmToggle || !m_opponentLdm) return;
        auto const& match = *m_state->match;
        bool const own = match.ownReady ? match.ownLdm : Mod::get()->getSavedValue<bool>("ranked-ldm", false);
        bool const editable = !match.ownReady && !m_readySending && !m_ldmSending &&
                              (match.state == "map_search" || match.state == "preparing");
        int const shown = (own ? 1 : 0) | (editable ? 2 : 0) | (match.opponentLdm ? 4 : 0);
        if (shown == m_ldmShown && m_ldmToggle->isToggled() == own) return;
        m_ldmShown = shown;
        m_ldmToggle->toggle(own);
        m_ldmToggle->setEnabled(editable);
        m_opponentLdm->setString(match.opponentLdm ? "LDM: ON" : "LDM: OFF");
        m_opponentLdm->setColor(match.opponentLdm ? ccc3(120, 255, 140) : kIce);
    }
    // 왼쪽 패널의 내 카드(아이콘, 닉네임, 칭호)를 다시 만든다.
    void refreshSelfCard() {
        m_selfCard->removeAllChildren();
        auto const& p = Service::get().profile();
        player(m_selfCard, p, {28.f, 32.f}, .85f);
        m_selfRank = m_state ? m_state->profile.globalRank : 0;
        rankedNickname(m_selfCard, p, m_selfRank, {m_leftWidth / 2.f + 11.f, 41.f}, .4f, m_leftWidth - 86.f);
        titleBadge(m_selfCard, p.cosmetics.title, {m_leftWidth / 2.f + 11.f, 21.f}, .85f, m_leftWidth - 86.f);
    }
    void onHistory(CCObject*) {
        if (auto* popup = HistoryPopup::create()) popup->show();
    }
    void onLeaderboard(CCObject*) {
        if (auto* popup = RankedLeaderboardPopup::create()) popup->show();
    }
    // 티어 안내 팝업. Season 2는 현재 컵 기준 팝업을 사용한다.
    void onTiers(CCObject*) {
        std::optional<RankedProfile> self;
        if (m_state) self = m_state->profile;
        if (m_season2) {
            if (auto* popup = Season2TiersPopup::create(Service::get().rankedCup(), std::move(self))) popup->show();
            return;
        }
        if (auto* popup = RankedTiersPopup::create(std::move(self))) popup->show();
    }
    void onPoints(CCObject*) {
        if (auto* popup = RankedPointsPopup::create()) popup->show();
    }
    void onLives(CCObject*) {
        if (auto* popup = RankedLivesPopup::create()) popup->show();
    }
    void onSeason2Duel(CCObject*) {
        if (!m_season2 || m_transitioning || !m_state) return;
        if (auto* popup = Season2DuelPopup::create(stateSink(), m_state->allowDuel)) popup->show();
    }
    // Season 2 컵 전환 상태: 전환할 컵, 상대 매치 조회 중 여부, 이동 예약된 컵/매치.
    std::string m_s2CupSwitch;
    bool m_s2Peeking = false;
    std::string m_s2CupGo, m_s2CupGoMatch;
    // 다른 컵의 듀얼 매치로 넘어가기 전에 그 컵의 매치를 조회(peek)한다.
    // 새로 시작할 매치이고 로비 장면이 준비되어 있으면 소개 연출 후 이동하고,
    // 그렇지 않으면 바로 컵을 전환한다.
    void season2CrossCup(std::string const& cup) {
        if (!m_s2CupSwitch.empty() || m_transitioning) return;
        m_s2CupSwitch = cup;
        m_s2Peeking = true;
        Service::get().peekSeason2Match(cup, [self = Weak<RankedLayer>(this), cup](std::optional<RankedMatch> match,
                                                                                   std::string) {
            auto owner = self.lock();
            if (!owner) return;
            owner->m_s2Peeking = false;
            if (owner->m_transitioning) return;
            bool const fresh = match && !match->result && match->state != "canceled" &&
                               match->state != "rematch_wait" && match->state != "loading" && match->state != "playing";
            if (!fresh || !owner->m_s2Lounge || owner->m_s2Intro || owner->m_botActive || !owner->isRunning() ||
                s2match::introduced().count(match->id)) {
                owner->season2GoCup(cup, {});
                return;
            }
            owner->season2CrossIntro(*match, cup);
        });
    }
    // 컵 전환을 예약하고 가능한 시점이 되면 season2TryGo가 실행한다.
    void season2GoCup(std::string const& cup, std::string const& arriving) {
        m_s2CupGo = cup;
        m_s2CupGoMatch = arriving;
        season2TryGo();
    }
    // 레이어가 실행 중이고 현재 씬일 때만 예약된 컵으로 전환한다. 전환 중이면 예약을
    // 버린다.
    void season2TryGo() {
        if (m_s2CupGo.empty()) return;
        if (m_transitioning) {
            m_s2CupGo.clear();
            m_s2CupGoMatch.clear();
            return;
        }
        CCNode* scene = this;
        while (scene->getParent()) scene = scene->getParent();
        if (!isRunning() || scene != CCDirector::sharedDirector()->getRunningScene()) return;
        auto const cup = std::exchange(m_s2CupGo, std::string());
        season2SwitchCup(cup, std::exchange(m_s2CupGoMatch, std::string()));
    }
    // 다른 컵에서 매치가 잡혔을 때 로비에서 보여 주는 소개(MATCH FOUND!/DUEL!) 연출.
    // 화면이 어두워지는 시점에 컵을 전환한다.
    void season2CrossIntro(RankedMatch const& match, std::string const& cup) {
        if (auto cover = m_s2Return.lock()) cover->skip();
        lounge::stopEffects(m_s2Lounge);
        audio::restoreRankedMusic();
        m_s2Intro = Season2Intro::create(
            m_s2Lounge, match.opponent.player, s2match::sideOf(match),
            [self = Weak<RankedLayer>(this)](bool) {
                if (auto owner = self.lock()) owner->m_s2Intro = nullptr;
            },
            match.duel ? "DUEL!" : "MATCH FOUND!");
        if (!m_s2Intro) {
            season2GoCup(cup, {});
            return;
        }
        s2match::introduced().insert(match.id);
        m_s2Intro->leaveAtDark([self = Weak<RankedLayer>(this), cup, id = match.id] {
            if (auto owner = self.lock()) owner->season2GoCup(cup, id);
        });
        addChild(m_s2Intro, 150);
        render();
    }
    // 컵 전환 직후 도착 연출: 덮개(m_s2Arrive)를 걷는 최대 대기(kArriveMax)와
    // 상한(kArriveCap), 초 단위.
    static constexpr float kArriveMax = 4.f, kArriveCap = 10.f;
    CCLayerColor* m_s2Arrive = nullptr;
    std::string m_s2ArriveMatch;
    float m_s2ArriveTime = 0.f, m_s2ArriveRun = 0.f;
    // 도착 덮개를 걷을 조건(이동 완료, 매치 종료/취소, 시간 초과)을 매 프레임 검사하고
    // 페이드아웃한다.
    void season2Arrive(float dt) {
        if (!m_s2Arrive) return;
        m_s2ArriveTime += dt;
        lounge::Pose pose;
        if (m_s2Lounge && lounge::pose(m_s2Lounge, pose)) m_s2ArriveRun += dt;
        bool lift = m_s2ArriveRun >= kArriveMax || m_s2ArriveTime >= kArriveCap;
        if (m_state) {
            auto const& match = m_state->match;
            bool const live = match && match->id == m_s2ArriveMatch && !match->result && match->state != "canceled";
            CCPoint pending;
            if (!live || m_lastResult)
                lift = true;
            else if (m_s2Room == match->id && !lounge::pendingMove(m_s2Lounge, pending))
                lift = true;
        }
        if (!lift) return;
        auto* cover = std::exchange(m_s2Arrive, nullptr);
        cover->runAction(
            CCSequence::create(CCDelayTime::create(.25f), CCFadeOut::create(.35f), CCRemoveSelf::create(), nullptr));
    }
    // Ranked 컵을 바꾸고 화면을 다시 연다. 로비 음악은 유지한다.
    void season2SwitchCup(std::string const& cup, std::string const& arriving = {}) {
        if (m_transitioning) return;
        m_transitioning = true;
        m_keepMusic = true;
        s2match::arriving() = arriving;
        Service::get().setRankedCup(cup);
        Notification::create(fmt::format("Duel in the {}", season2CupName(cup)), NotificationIcon::Info, 2.f)->show();
        showRanked();
    }
    void onGuide(CCObject*) {
        if (auto* popup = RankedGuide::create()) popup->show();
    }
    void onSelfInfo(CCObject*) {
        if (auto* popup = PlayerPopup::create(Service::get().profile())) popup->show();
    }
    void onOpponentInfo(CCObject*) {
        if (m_state && m_state->match)
            if (auto* popup = PlayerPopup::create(m_state->match->opponent.player)) popup->show();
    }
    // 대기 중 미니게임(러너/팝업)을 닫는다. 세대 번호를 올려 늦게 도착한 콜백을
    // 무효화한다.
    void closeQueueRunner() {
        ++m_queueGameGeneration;
        auto runner = m_queueRunner;
        auto popup = m_queueGame;
        m_queueRunner = nullptr;
        m_queueGame = nullptr;
        if (runner) runner->dismiss();
        if (popup) popup->dismiss();
    }
    // 미니게임이 닫혔을 때 참조를 비우는 콜백. 세대(epoch)가 같을 때만 동작한다.
    auto queueGameClosedCallback() {
        return [self = Weak<RankedLayer>(this), epoch = m_queueGameGeneration] {
            if (auto owner = self.lock(); owner && owner->m_queueGameGeneration == epoch) {
                owner->m_queueRunner = nullptr;
                owner->m_queueGame = nullptr;
            }
        };
    }
    // 미니게임 팝업에서 러너 게임으로 전환하는 콜백. 세대가 같고 레이어가 실행 중일 때만
    // 동작한다.
    auto queueGameSwitcher() {
        return [self = Weak<RankedLayer>(this), epoch = m_queueGameGeneration] {
            if (auto owner = self.lock(); owner && owner->m_queueGameGeneration == epoch && owner->isRunning())
                owner->onQueueRunner(nullptr);
        };
    }
    // 대기 중 미니게임 선택 팝업을 연다. 그 사이 상황이 바뀌었으면(매치 생성 등) 열지
    // 않는다.
    void showQueueGamePicker(uint64_t epoch) {
        if (m_queueGameGeneration != epoch || !isRunning() || m_transitioning || !m_state || !queueGamesOpen() ||
            m_state->match || m_queueRunner || m_queueGame)
            return;
        if (auto* picker = QueueGamePicker::create([self = Weak<RankedLayer>(this), epoch](int game) {
                Loader::get()->queueInMainThread([self, epoch, game] {
                    if (auto owner = self.lock(); owner && owner->m_queueGameGeneration == epoch)
                        owner->startQueueGame(game);
                });
            })) {
            picker->setClosedCallback(queueGameClosedCallback());
            picker->m_scene = this;
            m_queueGame = picker;
            picker->show();
        }
    }
    // 매치 탐색 대기 중(Season 1은 큐 등록 후, Season 2는 결과/연출/봇 대전이 없을 때)에만
    // 미니게임을 허용한다.
    bool queueGamesOpen() const {
        return m_state && (m_season2 ? !m_lastResult && !m_s2Intro && !m_botActive : m_state->queuedAt > 0);
    }
    void onQueueRunner(CCObject*) {
        if (!isRunning() || m_transitioning || !m_state || !queueGamesOpen() || m_state->match || m_queueRunner ||
            m_queueGame)
            return;
        auto epoch = ++m_queueGameGeneration;
        Loader::get()->queueInMainThread([self = Weak<RankedLayer>(this), epoch] {
            if (auto owner = self.lock()) owner->showQueueGamePicker(epoch);
        });
    }
    // 선택한 미니게임 시작: 1이면 Minesweeper 팝업, 0이면 SpectatorRunner(매치메이킹
    // 모드).
    void startQueueGame(int game) {
        if (!isRunning() || !m_state || !queueGamesOpen() || m_state->match || m_transitioning || m_queueRunner ||
            m_queueGame)
            return;
        ++m_queueGameGeneration;
        if (game == 1) {
            if (auto* popup = QueueMinesweeper::create()) {
                popup->setClosedCallback(queueGameClosedCallback());
                popup->setGameSwitcher(queueGameSwitcher());
                popup->m_scene = this;
                m_queueGame = popup;
                popup->show();
            }
            return;
        }
        if (game != 0) return;
        if (auto* runner = SpectatorRunner::create(Service::get().profile())) {
            runner->setMatchmakingMode(queueGameSwitcher(), queueGameClosedCallback());
            m_queueRunner = runner;
            addChild(runner, 120);
        }
    }
    // 방의 이모트/채팅 신호를 미리보기 위에 말풍선으로 띄운다.
    // 같은 nonce는 한 번만, 너무 오래된 신호(kFreshMs 초과)는 무시한다.
    void showRankedSignal(RoomInfo const& room) {
        if (!room.emote || room.emote->nonce.empty() || room.emote->nonce == m_seenSignal || !m_signals) return;
        m_seenSignal = room.emote->nonce;
        if (Service::get().serverNow() - room.emote->at > emotes::kFreshMs) return;
        bool const onLeft = room.emote->uid == Service::get().profile().uid;
        bool const chat = room.emote->kind == "chat";
        auto size = m_preview->getContentSize();
        auto at = m_root->convertToWorldSpace(m_preview->getPosition() +
                                              CCPoint{size.width * (onLeft ? .25f : .75f), size.height - 20.f});
        auto* bubble = CCNodeRGBA::create();
        bubble->setCascadeOpacityEnabled(true);
        bubble->setPosition(at);
        bubble->setScale(.15f);
        m_signals->addChild(bubble);
        auto* bg = NineSlice::create("square02b_001.png");
        bg->setContentSize({chat ? 118.f : 45.f, 40.f});
        bg->setColor(ccWHITE);
        bubble->addChild(bg);
        CCNode* animated = nullptr;
        if (chat) {
            animated = label(bubble, room.emote->text, {0.f, 1.f}, .46f, 106.f, ccc3(22, 35, 55), false, "bigFont.fnt");
        } else {
            animated = emoteArt(room.emote->kind);
            animated->setScale(.78f);
            bubble->addChild(animated, 2);
            emoteBurst(m_signals, at, room.emote->kind);
        }
        bubble->runAction(
            CCSequence::create(CCEaseBackOut::create(CCScaleTo::create(.2f, 1.f)), CCDelayTime::create(1.35f),
                               CCSpawn::create(CCFadeOut::create(.24f), CCScaleTo::create(.24f, .72f), nullptr),
                               CCRemoveSelf::create(), nullptr));
        if (!chat)
            animated->runAction(
                CCRepeat::create(CCSequence::create(CCEaseSineInOut::create(CCRotateTo::create(.12f, -9.f)),
                                                    CCEaseSineInOut::create(CCRotateTo::create(.12f, 9.f)),
                                                    CCEaseSineInOut::create(CCRotateTo::create(.12f, 0.f)), nullptr),
                                 2));
    }
    // 이모트/채팅을 서버로 보낸다. 전송 중이거나 쿨다운 중이거나 매치가 끝났으면 무시한다.
    void sendRankedSignal(std::string kind, std::string text = {}) {
        auto const& room = Service::get().room();
        if (m_sendingSignal || m_signalCooldown > 0.f || !m_state || !m_state->match || m_state->match->result ||
            !room || !room->guest)
            return;
        m_sendingSignal = true;
        m_signalCooldown = firebase_config::EMOTE_COOLDOWN_SECONDS;
        auto done = [self = Weak<RankedLayer>(this)](bool ok, std::string detail) {
            if (auto owner = self.lock()) {
                owner->m_sendingSignal = false;
                if (!ok) error(detail);
            }
        };
        if (kind == "chat")
            Service::get().sendChat(std::move(text), std::move(done));
        else
            Service::get().sendEmote(std::move(kind), std::move(done));
    }
    std::vector<std::string> m_rankedEmotes;
    void onRankedEmote(CCObject* sender) {
        int const tag = static_cast<CCNode*>(sender)->getTag();
        if (tag >= 0 && tag < static_cast<int>(m_rankedEmotes.size())) sendRankedSignal(m_rankedEmotes[tag]);
    }
    void onRankedEmotePicker(CCObject*) {
        if (auto* popup = EmotePickerPopup::create(
                [self = Weak<RankedLayer>(this)](std::string kind) {
                    if (auto owner = self.lock()) owner->sendRankedSignal(std::move(kind));
                },
                [self = Weak<RankedLayer>(this)] {
                    if (auto owner = self.lock();
                        owner && owner->m_state && owner->m_state->match && !owner->m_lastResult)
                        owner->rebuildPreview(*owner->m_state->match);
                }))
            popup->show();
    }
    void onRankedChat(CCObject*) {
        if (m_sendingSignal || m_signalCooldown > 0.f) return;
        if (auto* popup = QuickChatPopup::create([self = Weak<RankedLayer>(this)](std::string text) {
                if (auto owner = self.lock()) owner->sendRankedSignal("chat", std::move(text));
            }))
            popup->show();
    }
    // 내 티어 얼굴(face), 오라, 글로우를 교체한다. animate이면 이전 얼굴이 떨어져 나가고
    // 새 얼굴이 크게 떨어지며 충격 효과와 화면 흔들림을 준다. Season 2는 효과음만
    // 처리한다.
    void displayFace(std::string const& tier, bool animate = false) {
        if (tier == m_faceTier) return;
        if (m_season2) {
            m_faceTier = tier;
            auto view = m_s2ResultView.lock();
            if (animate && !(view && view->handlesTierChange())) audio::play(audio::Cue::TierChange);
            return;
        }
        CCPoint at{m_leftWidth / 2.f, m_height - 48.f};
        if (m_face) {
            if (animate)
                m_face->runAction(CCSequence::create(CCDelayTime::create(.32f),
                                                     CCSpawn::create(CCMoveBy::create(.5f, {-64.f, -85.f}),
                                                                     CCRotateBy::create(.5f, -155.f),
                                                                     CCFadeOut::create(.5f), nullptr),
                                                     CCRemoveSelf::create(), nullptr));
            else
                m_face->removeFromParent();
        }
        m_faceTier = tier;
        if (m_aura) m_aura->removeFromParent();
        if (m_titleGlow) m_titleGlow->removeFromParent();
        if (m_ratingGlow) m_ratingGlow->removeFromParent();
        m_aura = rankedAura(m_root, tier, at);
        m_titleGlow = rankedGlow(m_root, tier, {at.x, m_height - 84.f}, {108.f, 38.f});
        m_ratingGlow = rankedGlow(m_root, tier, {at.x, m_height - 107.f}, {134.f, 33.f});
        m_face = rankedFace(m_root, tier, at, 1.35f);
        if (animate) {
            m_face->setScale(3.5f);
            m_face->setOpacity(150);
            m_face->runAction(CCSequence::create(
                CCSpawn::create(CCEaseExponentialIn::create(CCScaleTo::create(.32f, 1.35f)), CCFadeIn::create(.32f),
                                nullptr),
                CallFuncExt::create([this, at] {
                    audio::play(audio::Cue::TierChange);
                    rankedImpact(m_root, at, 36.f);
                    m_root->runAction(CCSequence::create(CCMoveBy::create(.04f, {3.f, 2.f}),
                                                         CCMoveBy::create(.06f, {-5.f, -3.f}),
                                                         CCMoveBy::create(.05f, {2.f, 1.f}), nullptr));
                }),
                CCEaseSineOut::create(CCScaleTo::create(.12f, 1.48f)), CCScaleTo::create(.12f, 1.35f), nullptr));
        }
    }
    // 새 매치의 소개 연출을 한 번만 시작한다. 종료/취소/로딩/플레이 단계의 매치는
    // 건너뛴다.
    // Season 1은 재대결 라운드가 아니면 RankedClash 연출을 띄운다.
    void beginIntro(RankedMatch const& match) {
        if (m_introMatch == match.id || match.result || match.state == "canceled" || match.state == "rematch_wait" ||
            match.state == "loading" || match.state == "playing")
            return;
        m_introMatch = match.id;
        m_introTime = ranked::MATCH_INTRO_SECONDS;
        if (m_season2) {
            season2Intro(match);
            rebuildPreview(match);
            return;
        }
        if (match.rematchRound > 0) {
            m_introTime = 0.f;
            rebuildPreview(match);
            return;
        }
        auto self = m_state->profile;
        self.player = Service::get().profile();
        if (auto* clash = RankedClash::create(self, match.opponent, match.duel)) {
            clash->setID("ranked-clash"_spr);
            addChild(clash, 100);
        }
        rebuildPreview(match);
    }
    // 미리보기 영역의 세로 배치 상수 (카드 높이, 게이지 및 게이지 텍스트 y 오프셋).
    static constexpr float kCardHeight = 42.f;
    static constexpr float kGaugeY = 48.f - 79.f, kGaugeTextY = 37.f - 79.f;
    // 매치 미리보기(양쪽 플레이어, 맵 카드, 다운로드 진행 표시 등)를 처음부터 다시 만든다.
    // Season 2는 별도 함수로 위임한다.
    void rebuildPreview(RankedMatch const& match) {
        m_preview->removeAllChildren();
        m_mapCard = nullptr;
        m_ldmToggle = nullptr;
        m_opponentLdm = nullptr;
        m_ldmShown = -1;
        if (m_season2) {
            rebuildSeason2Preview(match);
            return;
        }
        auto size = m_preview->getContentSize();
        auto* actions = menu(m_preview);
        auto self = m_state->profile;
        self.player = Service::get().profile();
        float const bottom = 13.f, top = size.height - kCardHeight - 6.f;
        float const span = std::max(60.f, top - bottom);
        float const iconY = bottom + span * .80f, nameY = bottom + span * .52f;
        float const faceY = bottom + span * .315f, tierY = bottom + span * .12f;
        float const rowY = -2.f;
        m_downloadWidth = size.width * .43f;
        m_downloadRow = CCNode::create();
        m_downloadRow->setID("ranked-download-row"_spr);
        m_preview->addChild(m_downloadRow);
        // 양쪽 플레이어(i=0 나, i=1 상대)의 아이콘/닉네임/티어/레이팅과 맵 다운로드 진행 바를
        // 배치한다.
        // Season 2에서 배치를 마친 플레이어는 컵 티어 텍스트 + 레이팅으로, 아니면 배치
        // 진행도로 표시한다.
        for (int i = 0; i < 2; ++i) {
            auto const& p = i ? match.opponent : self;
            float x = size.width * (i ? .75f : .25f);
            player(m_preview, p.player, {x, iconY}, .95f);
            rankedNickname(m_preview, p.player, p.globalRank, {x, nameY}, .35f, size.width * .43f);
            auto* face = rankedFace(m_preview, p.placed ? p.tier : std::string("Unranked"), {x, faceY}, .4f);
            if (float const tall = face->getContentSize().height; tall > 1.f) face->setScale(19.f / tall);
            if (m_season2 && p.placed) {
                auto* tierName = Season2TierText::create();
                m_preview->addChild(tierName);
                tierName->set(p.tierName.empty() ? p.tier : p.tierName, p.tier, .3f, size.width * .28f, true);
                auto* rating = label(m_preview, fmt::format("  /  R{:.0f}", p.rating), {}, .28f, size.width * .15f,
                                     ccWHITE, true, "bigFont.fnt");
                rankedGradient(rating, p.tier);
                float const start = x - (tierName->width() + rating->getScaledContentSize().width) / 2.f;
                tierName->setPosition({start, tierY});
                rating->setPosition({start + tierName->width(), tierY});
            } else
                rankedGradient(label(m_preview,
                                     p.placed ? fmt::format("{} / R{:.0f}", tierText(p.tier, p.tierName), p.rating)
                                              : fmt::format("PLACEMENT {} / {}", p.placementCompleted,
                                                            p.placementRequired > 0 ? p.placementRequired : 5),
                                     {x, tierY}, .30f, size.width * .44f, ccWHITE, false, "bigFont.fnt"),
                               p.tier);
            iconButton(actions, this,
                       i ? menu_selector(RankedLayer::onOpponentInfo) : menu_selector(RankedLayer::onSelfInfo),
                       "GJ_infoIcon_001.png", {x + (i ? 1.f : -1.f) * size.width * .18f, iconY}, 18.f);
            auto* bar = CCDrawNode::create();
            CCPoint pnts[] = {{0, 0}, {m_downloadWidth, 0}, {m_downloadWidth, 6}, {0, 6}};
            bar->drawPolygon(pnts, 4, {.08f, .09f, .12f, 1}, .5f, {.4f, .45f, .5f, 1});
            bar->setPosition({x - m_downloadWidth / 2.f, kGaugeY});
            m_downloadRow->addChild(bar);
            m_downloadFill[i] = CCDrawNode::create();
            bar->addChild(m_downloadFill[i]);
            m_downloadText[i] = label(m_downloadRow, "WAITING FOR MAP", {x, kGaugeTextY}, .32f, m_downloadWidth, kIce,
                                      false, "chatFont.fnt");
        }
        // 가운데 영역: VS, 맵 라이프(하트), 맵 길이, 2인 플레이 모드 표시. 맵 정보가 오면
        // 나중에 채운다.
        label(m_preview, "VS", {size.width / 2.f, iconY + 2.f}, .48f, 35.f, ccc3(255, 185, 93), false, "gjFont46.fnt");
        float const livesY = iconY - 20.f;
        auto* lifeHeart = createLifeHeart(11.f);
        lifeHeart->setID("ranked-map-lives-heart"_spr);
        lifeHeart->setPosition({size.width / 2.f - 8.f, livesY});
        lifeHeart->setVisible(false);
        m_preview->addChild(lifeHeart);
        auto* lives = label(m_preview, "", {size.width / 2.f + 6.f, livesY}, .36f, 40.f, ccWHITE, false, "bigFont.fnt");
        lives->setID("ranked-map-lives"_spr);
        lives->setVisible(false);
        float const lengthY = iconY - 33.f;
        if (auto* clock = CCSprite::createWithSpriteFrameName("GJ_timeIcon_001.png")) {
            clock->setScale(12.f / std::max(1.f, clock->getContentSize().height));
            clock->setPosition({size.width / 2.f - 14.f, lengthY});
            clock->setID("ranked-map-length-clock"_spr);
            m_preview->addChild(clock);
        }
        label(m_preview, "---", {size.width / 2.f + 6.f, lengthY}, .42f, 52.f, ccc3(255, 214, 120), false,
              "goldFont.fnt")
            ->setID("ranked-map-length"_spr);
        auto* twoPlayer = label(m_preview, "2-PLAYER MODE", {size.width / 2.f, lengthY - 15.f}, .3f, 92.f,
                                kTwoPlayerRed, false, "bigFont.fnt");
        twoPlayer->setID("ranked-map-two-player"_spr);
        twoPlayer->setVisible(false);
        m_twoPlayerScale = twoPlayer->getScale();
        {
            auto const text = twoPlayer->getScaledContentSize();
            auto* back = NineSlice::create("square02b_001.png");
            back->setContentSize({text.width + 14.f, text.height + 7.f});
            back->setColor(ccc3(34, 6, 12));
            back->setOpacity(225);
            back->setPosition(twoPlayer->getPosition());
            back->setID("ranked-map-two-player-back"_spr);
            back->setVisible(false);
            m_preview->addChild(back, twoPlayer->getZOrder() - 1);
        }
        // 맵 카드와 티어 밴드 카드를 새로 만든다.
        updateMapLength(match);
        m_mapCard = nullptr;
        m_cardMapId = -1;
        refreshMapCard(match);
        m_bandCard = nullptr;
        m_bandCardKey.clear();
        refreshBandCard(match);
        // 하단 행: 퀵 이모트 버튼들, 이모트 선택기, 채팅 버튼, 좌우의 LDM 토글/상대 LDM 표시.
        // 방에 상대(guest)가 없으면 신호 버튼은 비활성화한다.
        m_rankedEmotes = emotes::quickSet(Service::get().profile().membership);
        int const emoteCount = static_cast<int>(m_rankedEmotes.size());
        float const pitch = 24.f;
        float const start = size.width / 2.f - (emoteCount + 1) * pitch / 2.f;
        bool const canSignal = Service::get().room() && Service::get().room()->guest.has_value();
        for (int index = 0; index < emoteCount; ++index) {
            auto* item = CCMenuItemSpriteExtra::create(emoteTile(m_rankedEmotes[index], 22.f, ccc3(15, 22, 31)), this,
                                                       menu_selector(RankedLayer::onRankedEmote));
            item->setTag(index);
            item->setPosition({start + index * pitch, rowY});
            actions->addChild(item);
            enabled(item, canSignal);
        }
        {
            auto* sprite = CCSprite::create();
            sprite->setContentSize({22.f, 22.f});
            auto* bg = NineSlice::create("square02b_001.png");
            bg->setContentSize({22.f, 22.f});
            bg->setColor(ccc3(70, 30, 105));
            bg->setPosition({11.f, 11.f});
            sprite->addChild(bg);
            auto* dots = CCDrawNode::create();
            for (int i = 0; i < 4; ++i)
                dots->drawDot({7.f + (i % 2) * 8.f, 7.f + (i / 2) * 8.f}, 2.6f, {1.f, .85f, .4f, 1.f});
            sprite->addChild(dots);
            auto* more = CCMenuItemSpriteExtra::create(sprite, this, menu_selector(RankedLayer::onRankedEmotePicker));
            more->setPosition({start + emoteCount * pitch, rowY});
            actions->addChild(more);
            enabled(more, canSignal);
        }
        auto* chat = iconButton(actions, this, menu_selector(RankedLayer::onRankedChat), "GJ_chatBtn_001.png",
                                {start + (emoteCount + 1) * pitch, rowY}, 21.f);
        enabled(chat, canSignal);
        float const leftEdge = start - pitch / 2.f, rightEdge = start + (emoteCount + 1) * pitch + pitch / 2.f;
        float const ldmX = std::min(size.width * .13f, leftEdge / 2.f + 2.f);
        m_ldmToggle = CCMenuItemToggler::createWithStandardSprites(this, menu_selector(RankedLayer::onLdm), .45f);
        m_ldmToggle->setID("ranked-ldm-toggle"_spr);
        m_ldmToggle->setPosition({ldmX - 12.f, rowY});
        actions->addChild(m_ldmToggle);
        label(m_preview, "LDM", {ldmX + 10.f, rowY}, .25f, 30.f, kIce);
        m_opponentLdm =
            label(m_preview, "LDM: OFF", {std::max(size.width * .87f, (rightEdge + size.width) / 2.f - 2.f), rowY},
                  .25f, std::max(30.f, size.width - rightEdge - 8.f), kIce);
        updateLdmControls();
        updateDownloads();
    }
    // 맵 카드/밴드 카드가 공유하는 카드 틀(배경판, 강조선, 아이콘 자리). plain이면
    // 배경판만 만든다.
    static CCNode* infoCard(CCNode* parent, std::string const& id, CCPoint topCenter, float width, float height,
                            ccColor3B accent, ccColor3B plate, bool plain = false) {
        auto* card = CCNode::create();
        card->setID(id);
        card->setContentSize({width, height});
        card->setAnchorPoint({.5f, 1.f});
        card->ignoreAnchorPointForPosition(false);
        card->setPosition(topCenter);
        parent->addChild(card, 4);
        auto* back = NineSlice::create("square02b_001.png");
        back->setContentSize({width, height});
        back->setPosition({width / 2.f, height / 2.f});
        back->setColor(plate);
        back->setOpacity(215);
        card->addChild(back);
        if (plain) return card;
        auto* edge = CCLayerColor::create(ccc4(accent.r, accent.g, accent.b, 235), 2.f, height - 14.f);
        edge->setPosition({4.f, 7.f});
        card->addChild(edge);
        auto* slot = CCDrawNode::create();
        slot->drawDot({21.f, height / 2.f}, 15.5f, {0.f, 0.f, 0.f, .32f});
        card->addChild(slot);
        return card;
    }
    CCNode* m_mapCard = nullptr;
    int64_t m_cardMapId = -1;
    // 선택된 맵 정보 카드를 만든다. 맵이 같으면 좋아요/날짜만 갱신하고 그대로 둔다.
    // 맵이 아직 없으면 SELECTING MAP... 을 깜빡이게 표시한다.
    // Season 2는 별/코인/투표 배지를 한 줄에 배치하고 공간이 모자라면 배지를 작은 형태로
    // 바꾼다.
    void refreshMapCard(RankedMatch const& match) {
        if (!m_preview || m_lastResult) return;
        int64_t const id = match.selectedMap ? match.selectedMap->id : 0;
        if (m_mapCard && m_mapCard->getParent() == m_preview && m_cardMapId == id) {
            updateMapCardDetails();
            return;
        }
        if (m_mapCard) m_mapCard->removeFromParent();
        m_cardMapId = id;
        m_cardDetails.clear();
        auto const size = m_preview->getContentSize();
        float const width = cardWidths(size.width).second, height = kCardHeight;
        auto* card = infoCard(m_preview, "ranked-map-card"_spr, {size.width - 6.f - width / 2.f, size.height - 1.f},
                              width, height, match.selectedMap ? ccc3(255, 205, 110) : kIce,
                              m_season2 ? season2Plate(.78f) : ccc3(9, 15, 30), m_season2);
        m_mapCard = card;
        if (!match.selectedMap) {
            auto* waiting = label(card, "SELECTING MAP...", {width / 2.f + 8.f, height / 2.f + 1.f}, .3f, width - 40.f,
                                  kIce, false, "bigFont.fnt");
            waiting->runAction(CCRepeatForever::create(
                CCSequence::create(CCFadeTo::create(.7f, 110), CCFadeTo::create(.7f, 255), nullptr)));
            return;
        }
        auto const& level = *match.selectedMap;
        if (auto* face = mapDifficulty(level)) {
            auto const faceSize = face->getContentSize();
            face->setScale(std::min(28.f / std::max(1.f, faceSize.height), 28.f / std::max(1.f, faceSize.width)));
            face->setPosition({21.f, height / 2.f + 1.f});
            card->addChild(face);
        }
        float const left = 41.f, textWidth = width - left - 7.f, textX = left + textWidth / 2.f;
        auto* date =
            label(card, "", {width - 7.f, height - 6.f}, cardDateScale(), textWidth, kMuted, false, "chatFont.fnt");
        date->setAnchorPoint({1.f, .5f});
        date->setID("ranked-map-date"_spr);
        label(card, level.name.empty() ? "Unknown map" : level.name, {left, height - 14.f}, .3f, textWidth, ccWHITE,
              true, "bigFont.fnt")
            ->setID("ranked-map-title"_spr);
        bool const coins = level.userCoins > 0;
        float const row = 8.f;
        m_cardLikesCenter = -1.f;
        if (m_season2) {
            constexpr float kRowBottom = 3.f, kRight = 9.f, kLikesW = 46.f, kSlack = 12.f;
            constexpr float kBadgeH = 8.f * 1.3f * 1.2f, kCompactH = 7.f * 1.3f * 1.2f;
            auto* stars = starRating(card, std::to_string(level.stars), {0.f, row}, .3f, 40.f);
            float const starsW = stars->getScaledContentSize().width;
            stars->setPositionX(left + starsW / 2.f);
            mapvotes::sitOn(stars, kRowBottom);
            float starsEnd = left + starsW;
            if (coins) {
                mapCoinCount(card, level, {starsEnd + 18.f, row}, .27f);
                mapvotes::sitOn(card->getChildByID("map-user-coin-count"_spr), kRowBottom);
                starsEnd += 34.f;
            }
            auto* badge = mapVotesBadge(card, level.id, {width - kRight, row}, kBadgeH, 1.f, 2);
            float zoneLeft = width - kRight - (badge ? badge->reserve() : 0.f) - 4.f;
            float likesCenter = std::max((starsEnd + zoneLeft) / 2.f, starsEnd + 6.f + kLikesW / 2.f);
            if (badge && likesCenter + kLikesW / 2.f > zoneLeft + kSlack) {
                badge->removeFromParent();
                badge = mapVotesBadge(card, level.id, {width - kRight, row}, kCompactH, 1.f, 2, true);
                zoneLeft = width - kRight - (badge ? badge->reserve() : 0.f) - 4.f;
                likesCenter = std::max((starsEnd + zoneLeft) / 2.f, starsEnd + 6.f + kLikesW / 2.f);
            }
            if (badge) mapvotes::sitOn(badge, kRowBottom);
            label(card, "by " + (level.creator.empty() ? std::string("Unknown") : level.creator),
                  {left, height - 23.5f}, .26f, std::max(40.f, zoneLeft - left), ccc3(255, 214, 120), true,
                  "goldFont.fnt");
            m_cardLikesCenter = likesCenter;
            m_cardRowBottom = kRowBottom;
            m_cardLikesAt = CCPoint{m_cardLikesCenter, row};
            auto* likes = label(card, "---", {m_cardLikesAt.x + 8.f, row}, .3f, 34.f, ccWHITE, true, "bigFont.fnt");
            likes->setID("ranked-map-likes"_spr);
            updateMapCardDetails();
            return;
        }
        bool const narrow = textWidth < 150.f;
        auto* badge = mapVotesBadge(card, level.id, {width - 7.f, height - 23.5f}, narrow ? 7.f : 8.f, 1.f, 2, narrow);
        float const creatorW = textWidth - (badge ? badge->reserve() + 6.f : 0.f);
        label(card, "by " + (level.creator.empty() ? std::string("Unknown") : level.creator), {left, height - 23.5f},
              .26f, creatorW, ccc3(255, 214, 120), true, "goldFont.fnt");
        starRating(card, std::to_string(level.stars), {textX - (coins ? 34.f : 20.f), row}, .3f, 40.f);
        if (coins) mapCoinCount(card, level, {textX + 2.f, row}, .27f);
        m_cardLikesAt = CCPoint{textX + (coins ? 28.f : 14.f), row};
        auto* likes = label(card, "---", {m_cardLikesAt.x + 8.f, row}, .3f, 34.f, ccWHITE, true, "bigFont.fnt");
        likes->setID("ranked-map-likes"_spr);
        updateMapCardDetails();
    }
    std::string m_cardDetails;
    CCPoint m_cardLikesAt;
    float m_cardLikesCenter = -1.f;
    float m_cardRowBottom = 0.f;
    // 밴드 카드와 맵 카드의 너비(first, second)를 계산한다. Season 2는 맵 카드를 더 넓게
    // 둔다.
    std::pair<float, float> cardWidths(float row) const {
        float const both = row - 18.f;
        return m_season2 ? std::pair{both / 4.f * 1.4f, both - both / 4.f * 1.4f} : std::pair{both / 2.f, both / 2.f};
    }
    float cardDateScale() const { return m_season2 ? .28f * 1.2f : .28f; }
    // 숫자를 1.2K / 12K / 1.5M 식으로 줄여 표시한다.
    static std::string compactCount(int value) {
        if (value >= 1'000'000) return fmt::format("{:.1f}M", value / 1'000'000.);
        if (value >= 10'000) return fmt::format("{}K", value / 1'000);
        if (value >= 1'000) return fmt::format("{:.1f}K", value / 1'000.);
        return std::to_string(value);
    }
    // level->m_gameVersion 값을 "GD 2.2" 같은 표기로 바꾼다. 1.x 시절 값은 별도로
    // 매핑한다.
    static std::string gdVersionText(int version) {
        if (version >= 18) return fmt::format("GD {}.{}", version / 10, version % 10);
        if (version == 11) return "GD 1.8";
        if (version == 10) return "GD 1.7";
        if (version >= 1 && version <= 7) return fmt::format("GD 1.{}", version - 1);
        return {};
    }
    // 캐시된 레벨 정보로 맵 카드의 좋아요 수(싫어요가 더 많으면 음수), ID, 버전, 업로드
    // 시각 문구를 갱신한다.
    // 내용이 바뀌지 않았으면 다시 그리지 않는다.
    void updateMapCardDetails() {
        if (!m_mapCard || m_cardMapId <= 0) return;
        auto* likes = typeinfo_cast<CCLabelBMFont*>(m_mapCard->getChildByID("ranked-map-likes"_spr));
        auto* date = typeinfo_cast<CCLabelBMFont*>(m_mapCard->getChildByID("ranked-map-date"_spr));
        if (!likes || !date) return;
        std::string likeText = "---", dateText = fmt::format("ID: {}", m_cardMapId);
        bool disliked = false;
        if (auto* level = cachedLevel(m_cardMapId)) {
            int const net = level->m_likes - level->m_dislikes;
            disliked = net < 0;
            likeText = (disliked ? "-" : "") + compactCount(std::abs(net));
            std::string const version = gdVersionText(level->m_gameVersion);
            std::string const uploaded(level->m_uploadDate);
            std::string const ago = uploaded.empty() ? std::string() : uploaded + " ago";
            if (!version.empty()) dateText += "  |  " + version;
            if (!ago.empty()) dateText += "  |  " + ago;
        }
        std::string const key = likeText + "|" + dateText;
        if (key == m_cardDetails && m_mapCard->getChildByID("ranked-map-likes-icon"_spr)) return;
        m_cardDetails = key;
        if (auto* old = m_mapCard->getChildByID("ranked-map-likes-icon"_spr)) old->removeFromParent();
        char const* const frame = disliked ? "GJ_dislikesIcon_001.png" : "GJ_likesIcon_001.png";
        auto* icon =
            m_cardLikesCenter >= 0.f ? mapvotes::icon(frame, 11.f) : loadedFrameSprite(frame, "GJ_GameSheet03.plist");
        if (icon) {
            icon->setScale(11.f / std::max(1.f, icon->getContentSize().height));
            icon->setPosition(m_cardLikesAt);
            icon->setID("ranked-map-likes-icon"_spr);
            m_mapCard->addChild(icon);
            if (m_cardLikesCenter >= 0.f) {
                mapvotes::sitOn(icon, m_cardRowBottom);
                likes->setPositionY(icon->getPositionY());
            }
        }
        likes->setString(likeText.c_str());
        likes->limitLabelWidth(34.f, .3f, .15f);
        if (m_cardLikesCenter >= 0.f) {
            float const iconW = icon ? icon->getScaledContentSize().width : 0.f,
                        textW = likes->getScaledContentSize().width;
            float const start = m_cardLikesCenter - (iconW + 3.f + textW) / 2.f;
            if (icon) icon->setPositionX(start + iconW / 2.f);
            likes->setPositionX(start + iconW + 3.f);
        }
        date->setString(dateText.c_str());
        date->limitLabelWidth(m_mapCard->getContentSize().width - 48.f, cardDateScale(), .15f);
    }
    // 티어 밴드가 바뀌면 현재 색(전환 중이면 중간색)에서 새 테마로 전환을 시작하고 밴드
    // 카드를 살짝 튕긴다.
    void applyBand(std::string const& band) {
        if (band == m_band) return;
        RankedBandTheme now = m_bandTo;
        if (m_bandBlend < 1.f) {
            now.root = mixColor(m_bandFrom.root, m_bandTo.root, m_bandBlend);
            now.panel = mixColor(m_bandFrom.panel, m_bandTo.panel, m_bandBlend);
        }
        m_band = band;
        m_bandFrom = now;
        m_bandTo = rankedBandTheme(band);
        m_bandBlend = 0.f;
        if (m_background) m_background->setBand(band);
        if (!band.empty() && m_preview) {
            if (m_bandCard)
                m_bandCard->runAction(CCSequence::create(CCEaseSineOut::create(CCScaleTo::create(.12f, 1.08f)),
                                                         CCEaseSineIn::create(CCScaleTo::create(.16f, 1.f)), nullptr));
        }
    }
    // 밴드 전환 진행도 t(0~1)에 맞춰 배경판과 프레임 패널 색을 섞어 칠한다.
    void paintBand(float t) {
        if (m_backdrop) m_backdrop->setColor(mixColor(m_bandFrom.root, m_bandTo.root, t));
        auto const panel = mixColor(m_bandFrom.panel, m_bandTo.panel, t);
        for (auto* item : m_framePanels)
            if (item) item->setColor(panel);
    }
    // 맵 풀 범위 문구를 만든다 (TOP a-b%, 레이팅 범위, 첫 매치 풀 등).
    static std::string bandRangeText(RankedMatch const& match) {
        if (!match.mapBand) return "Map pool for this match";
        auto const& band = *match.mapBand;
        if (band.reason == "placement") return "First-match pool";
        std::string top;
        if (band.topTo)
            top = band.topFrom.value_or(0) <= 0 ? fmt::format("TOP {}%", *band.topTo)
                                                : fmt::format("TOP {}-{}%", *band.topFrom, *band.topTo);
        std::string rating;
        if (!band.tierName.empty() && band.ratingMin && !band.ratingMax && !band.topTo) {
            rating = fmt::format("R{}+", *band.ratingMin);
        } else if (band.ratingMin && band.ratingMax) {
            if (band.topFrom.value_or(1) <= 0)
                rating = fmt::format("R{}+", *band.ratingMin);
            else if (band.topTo.value_or(0) >= 100)
                rating = fmt::format("R{} or less", *band.ratingMax);
            else if (*band.ratingMin == *band.ratingMax)
                rating = fmt::format("R{}", *band.ratingMin);
            else
                rating = fmt::format("R{} - {}", *band.ratingMin, *band.ratingMax);
        } else if (band.topTo)
            rating = "no players yet";
        if (top.empty()) return rating;
        return rating.empty() ? top : top + "   " + rating;
    }
    // 맵 풀(티어 밴드) 카드를 만든다. 밴드·마스크·문구가 같으면 다시 만들지 않는다.
    // 선택 이유(듀얼/무승부/레이팅/첫 게임/평균 랭크), 밴드 이름, 범위, 난이도 아이콘과 별
    // 범위 칩을 보여 주고
    // 칩이 폭을 넘으면 전체를 축소한다.
    void refreshBandCard(RankedMatch const& match) {
        if (!m_preview || m_lastResult) return;
        auto const key = rankedBandKey(match);
        std::string signature = fmt::format("{}|{}|{}", key, match.mapMask, bandRangeText(match));
        if (match.mapBand) signature += "|" + match.mapBand->reason;
        if (m_bandCard && m_bandCard->getParent() == m_preview && m_bandCardKey == signature) return;
        if (m_bandCard) m_bandCard->removeFromParent();
        m_bandCardKey = signature;
        auto const size = m_preview->getContentSize();
        float const width = cardWidths(size.width).first, height = kCardHeight;
        auto const theme = rankedBandTheme(key);
        auto* card =
            infoCard(m_preview, "ranked-band-card"_spr, {6.f + width / 2.f, size.height - 1.f}, width, height,
                     theme.accent, m_season2 ? season2Plate(.78f) : mixColor(theme.panel, ccBLACK, .45f), m_season2);
        m_bandCard = card;
        std::string const faceTier = key.empty() || key == "Placement" ? std::string("Unranked") : key;
        auto* face = rankedFace(card, faceTier, {21.f, height / 2.f + (m_season2 ? 3.f : 1.f)}, .6f);
        if (float const tall = face->getContentSize().height; tall > 1.f) face->setScale(24.f / tall);
        std::string reason = "MAP BAND";
        if (match.mapBand) {
            auto const& why = match.mapBand->reason;
            reason = why == "duel"        ? "DUEL"
                     : why == "draw"      ? "DRAW +1"
                     : why == "rating"    ? "BY RATING"
                     : why == "placement" ? "FIRST GAMES"
                                          : "AVG RANK";
        }
        float const rowTop = height - 10.f, rowMid = height / 2.f, rowLow = 9.5f;
        auto* tag = m_season2 ? label(card, reason, {21.f, 4.5f}, .2f, 38.f, ccc3(255, 221, 120), false, "goldFont.fnt")
                              : label(card, reason, {width - 7.f, rowTop}, .24f, 56.f, ccc3(255, 221, 120), false,
                                      "goldFont.fnt");
        if (!m_season2) tag->setAnchorPoint({1.f, .5f});
        float const textX = 41.f;
        std::string title = key.empty() ? std::string("MAP POOL") : key == "Placement" ? std::string("PLACEMENT") : key;
        std::transform(title.begin(), title.end(), title.begin(), [](unsigned char c) { return std::toupper(c); });
        auto* name = label(card, title, {textX, rowTop}, .3f,
                           std::max(30.f, width - textX - (m_season2 ? 7.f : 10.f + tag->getScaledContentSize().width)),
                           ccWHITE, true, "bigFont.fnt");
        if (key.empty() || key == "Placement")
            name->setColor(theme.accent);
        else
            rankedGradient(name, key);
        label(card, bandRangeText(match), {textX, rowMid}, .4f, width - textX - 7.f, ccc3(215, 228, 245), true,
              "chatFont.fnt");
        auto* chips = CCNode::create();
        card->addChild(chips);
        loadedFrame("diffIcon_01_btn_001.png", "GJ_GameSheet03.plist");
        float cursor = 0.f;
        for (auto const& entry : randomFilterPreview(MapSelection{true, match.mapMask, false})) {
            auto frame = fmt::format("diffIcon_{:02}_btn_001.png", CATEGORY_FACES[entry.category]);
            if (auto* sprite = cachedFrame(frame.c_str())) {
                auto* icon = CCSprite::createWithSpriteFrame(sprite);
                icon->setScale(10.f / std::max(1.f, icon->getContentSize().height));
                icon->setPosition({cursor + icon->getScaledContentSize().width / 2.f, 0.f});
                chips->addChild(icon);
                cursor += icon->getScaledContentSize().width + 2.f;
            }
            if (entry.category < 5) {
                auto* stars = label(chips, entry.stars, {cursor, 0.f}, .34f, 0.f, ccWHITE, true, "chatFont.fnt");
                cursor += stars->getScaledContentSize().width + 1.f;
                if (auto* star = CCSprite::createWithSpriteFrameName("star_small01_001.png")) {
                    star->setScale(6.f / std::max(1.f, star->getContentSize().height));
                    star->setPosition({cursor + 3.f, .5f});
                    chips->addChild(star);
                    cursor += 7.f;
                }
            }
            cursor += 5.f;
        }
        float const room = width - textX - 7.f;
        chips->setScale(cursor > room ? room / cursor : 1.f);
        chips->setPosition({textX, rowLow});
    }
    // 맵 경험(Map Experience)에 의한 패배 감소율(lossReduction)을 방패 연출과 함께 보여
    // 준다.
    void showLossShield(RankedResult const& result, CCSize board, bool hideNumbers) {
        int const percent = static_cast<int>(std::lround(result.lossReduction * 100.));
        float const y1 = std::max(board.height * .52f - 31.f, 58.f), y2 = y1 - 15.f;
        ccColor3B const cyan = ccc3(120, 228, 255);
        auto* title = label(m_preview, fmt::format("MAP EXPERIENCE  -{}% LOSS", percent), {board.width / 2.f + 9.f, y1},
                            .36f, board.width - 70.f, cyan, false, "bigFont.fnt");
        title->setID("ranked-map-experience"_spr);
        float const titleLeft = title->getPositionX() - title->getScaledContentSize().width / 2.f;
        auto* shield = art::glyph(art::Glyph::Shield, ccc3(90, 210, 255));
        shield->setPosition({titleLeft - 11.f, y1});
        shield->setScale(0.f);
        m_preview->addChild(shield, 6);
        shield->runAction(CCSequence::create(CCDelayTime::create(.45f),
                                             CCEaseBackOut::create(CCScaleTo::create(.35f, 1.35f)), nullptr));
        for (int ring = 0; ring < 2; ++ring) {
            auto* pulse = CCDrawNode::create();
            pulse->drawDot({0.f, 0.f}, 9.f, {.45f, .85f, 1.f, .38f});
            pulse->setPosition(shield->getPosition());
            pulse->setScale(0.f);
            m_preview->addChild(pulse, 5);
            pulse->runAction(CCSequence::create(CCDelayTime::create(.6f + ring * .35f), CCScaleTo::create(0.f, .6f),
                                                CCScaleTo::create(.55f, 2.4f), CCRemoveSelf::create(), nullptr));
        }
        title->setOpacity(0);
        title->runAction(CCSequence::create(CCDelayTime::create(.5f), CCFadeIn::create(.3f), nullptr));
        // 맵 최고 기록: 값이 음수면 아직 플레이하지 않은 맵이라 NEW로 표시한다.
        auto best = [](int value) { return value < 0 ? std::string("NEW") : fmt::format("{}%", value); };
        auto* bests =
            label(m_preview, fmt::format("YOU {}  vs  {}", best(result.mapBest), best(result.opponentMapBest)),
                  {board.width / 2.f + (hideNumbers ? 0.f : 52.f), y2}, .5f, board.width * .45f, ccc3(170, 215, 235),
                  false, "chatFont.fnt");
        bests->setOpacity(0);
        bests->runAction(CCSequence::create(CCDelayTime::create(.65f), CCFadeIn::create(.3f), nullptr));
        if (hideNumbers) return;
        // 원래 감소량(취소선) -> 실제 반영된 레이팅 변화 순으로 숫자 연출을 낸다.
        // hideNumbers면 숫자는 생략한다.
        float const numbersX = board.width / 2.f - 48.f;
        auto* full = label(m_preview, fmt::format("{:+d}", result.fullLoss), {numbersX - 22.f, y2}, .42f, 50.f,
                           ccc3(150, 150, 160), false, "bigFont.fnt");
        auto* arrow = label(m_preview, ">", {numbersX, y2}, .4f, 14.f, cyan, false, "bigFont.fnt");
        auto* taken = label(m_preview, fmt::format("{:+.0f}", result.newRating - result.oldRating),
                            {numbersX + 22.f, y2}, .5f, 50.f, ccc3(255, 130, 115), false, "bigFont.fnt");
        for (auto* item : {full, arrow}) {
            item->setOpacity(0);
            item->runAction(CCSequence::create(CCDelayTime::create(.65f), CCFadeIn::create(.25f), nullptr));
        }
        float const fullWidth = full->getScaledContentSize().width;
        auto* strike = CCDrawNode::create();
        strike->drawSegment({0.f, 0.f}, {fullWidth + 4.f, 0.f}, 1.2f, {1.f, .35f, .3f, 1.f});
        strike->setPosition({full->getPositionX() - fullWidth / 2.f - 2.f, y2});
        strike->setScaleX(0.f);
        m_preview->addChild(strike, 7);
        strike->runAction(CCSequence::create(CCDelayTime::create(1.f),
                                             CCEaseSineOut::create(CCScaleTo::create(.22f, 1.f, 1.f)), nullptr));
        float const takenScale = taken->getScale();
        taken->setScale(0.f);
        taken->runAction(CCSequence::create(CCDelayTime::create(1.2f),
                                            CCEaseBackOut::create(CCScaleTo::create(.3f, takenScale)), nullptr));
    }
    // 맵 카드/밴드 카드를 갱신하고, 양쪽의 맵 다운로드 진행도(퍼센트, 맵/음악 상태)를
    // 막대에 그린다.
    // 내 쪽은 로컬 진행도, 상대 쪽은 방(room)의 다운로드 정보를 쓴다. 준비 완료면 100%로
    // 표시한다.
    void updateDownloads() {
        if (m_state && m_state->match && !m_lastResult && m_preview && m_preview->isVisible()) {
            refreshMapCard(*m_state->match);
            refreshBandCard(*m_state->match);
        }
        if (!m_state || !m_state->match || m_lastResult || !m_downloadText[0]) return;
        auto const& match = *m_state->match;
        auto const& room = Service::get().room();
        if (m_downloadRow) m_downloadRow->setVisible(downloadPhase(match));
        for (int side = 0; side < 2; ++side) {
            int percent = 0;
            std::string detail = "WAITING FOR MAP";
            if (match.selectedMap) {
                if (side == 0) {
                    auto progress = mapDownloadProgress(match.selectedMap->id, true);
                    percent = progress.percent;
                    detail = fmt::format("{}%  {}", percent,
                                         !progress.mapReady ? std::string("Map...")
                                         : progress.songsSkipped
                                             ? std::string("Music skipped")
                                             : fmt::format("Music {}/{}", progress.songsDone, progress.songsTotal));
                } else if (room && room->id == match.roomId) {
                    auto const& p = Service::get().isHost() ? room->guestDownload : room->hostDownload;
                    if (p.levelId == match.selectedMap->id) {
                        percent = p.percent;
                        detail =
                            fmt::format("{}%  {}", percent,
                                        p.mapReady ? fmt::format("Music {}/{}", p.songsDone, p.songsTotal) : "Map...");
                    } else
                        detail = "0%  Map...";
                }
                if (side == 0 ? match.ownReady : match.opponentReady) {
                    percent = 100;
                    detail = "100%  READY";
                }
            }
            m_downloadText[side]->setString(detail.c_str());
            m_downloadText[side]->limitLabelWidth(m_downloadTextWidth > 0.f ? m_downloadTextWidth : m_downloadWidth,
                                                  m_downloadTextScale, .22f);
            auto* fill = m_downloadFill[side];
            fill->clear();
            if (percent > 0) {
                float w = m_downloadWidth * std::clamp(percent, 0, 100) / 100.f, h = m_downloadBarH;
                CCPoint p[] = {{0, 0}, {w, 0}, {w, h}, {0, h}};
                fill->drawPolygon(p, 4, side ? ccColor4F{.15f, .75f, 1.f, 1} : ccColor4F{1, .6f, .18f, 1}, 0,
                                  {0, 0, 0, 0});
            }
        }
    }
    // 아래는 화면 상태 필드: 최신 RankedState, 마지막 결과, 서버 요청 진행 플래그, 결과
    // 확인(acknowledge) 재시도,
    // 무결성 검사/폴링/준비/에셋 재시도 타이머(초), 현재 phase 등.
    CCLabelBMFont* m_tier = nullptr;
    CCLabelBMFont* m_rating = nullptr;
    CCLabelBMFont* m_placement = nullptr;
    CCLabelBMFont* m_population = nullptr;
    CCLabelBMFont* m_status = nullptr;
    CCLabelBMFont* m_detail = nullptr;
    CCMenuItemSpriteExtra* m_action = nullptr;
    std::optional<RankedState> m_state;
    std::optional<RankedResult> m_lastResult;
    bool m_fetching = false;
    bool m_pending = false;
    bool m_assetsReady = false;
    bool m_assetPreparing = false;
    bool m_readySending = false;
    bool m_resultAcknowledged = false;
    bool m_ackPending = false;
    bool m_leaveAfterAck = false;
    float m_ackRetry = 0.f;
    float m_transitionStuck = 0.f;
    float m_integrityTimer = 0.f;
    float m_pollTimer = 0.f;
    float m_readyTimer = 0.f;
    float m_assetRetry = 0.f;
    float m_resultTime = 0.f;
    double m_displayRating = ranked::INITIAL_RATING;
    ranked::Phase m_phase = ranked::Phase::Idle;
    std::string m_candidateMatch;
    std::string m_assetMatch;
    std::string m_observedMatch;
    std::string m_shownDraw;
    std::string m_cancelShown;
    std::string m_notice;
    std::string m_shownIntegrityResult;
    bool m_integritySending = false;

    // Ranked 중 외부 치트가 감지되는지 검사한다. 탐색 중이면 탐색을 취소시키고 안내하며,
    // 매치가 진행 중이면 서버에 실격(disqualify) 보고를 보낸다. 큐/매치가 없으면 아무것도
    // 하지 않는다.
    void checkRankedIntegrity() {
        if (m_integritySending || m_pending || !m_state || m_botActive) return;
        auto status = integrity::activeExternalCheat(false);
        if (!status.detected) return;
        if (!m_state->match && m_state->queuedAt <= 0) return;
        m_integritySending = true;
        if (!m_state->match) {
            auto message = fmt::format(
                "<cr>{}</c>\nRanked search has been stopped.\nDisable the listed banned gameplay features before queueing.",
                status.detail);
            FLAlertLayer::create("Ranked Anti-Cheat", message.c_str(), "OK")->show();
            Service::get().cancelRanked(
                [self = Weak<RankedLayer>(this)](std::optional<RankedState> value, std::string detail) {
                    if (auto owner = self.lock()) {
                        owner->m_integritySending = false;
                        if (value)
                            owner->receive(std::move(*value));
                        else
                            owner->m_detail->setString(detail.c_str());
                    }
                },
                true);
            return;
        }
        auto const match = *m_state->match;
        if (match.result || match.state == "canceled") {
            m_integritySending = false;
            return;
        }
        Service::get().disqualifyRanked(
            match.id, status.code, status.features,
            [self = Weak<RankedLayer>(this)](std::optional<RankedState> value, std::string detail) {
                if (auto owner = self.lock()) {
                    owner->m_integritySending = false;
                    if (value)
                        owner->receive(std::move(*value));
                    else
                        owner->m_detail->setString(detail.c_str());
                }
            });
    }

    // 준비 단계에서 서버 시각 기준 대전 시작까지 남은 초(올림). 시작 시각이 없으면 0.
    static int countdownLeft(RankedMatch const& match) {
        if (match.state != "preparing" || match.startAt <= 0) return 0;
        auto const left = match.startAt - Service::get().serverNow();
        return left > 0 ? static_cast<int>((left + 999) / 1000) : 0;
    }
    void setVote(std::string const& text, bool active) {
        if (!m_voteButton) return;
        if (m_voteText != text) {
            m_voteText = text;
            auto* sprite = ButtonSprite::create(text.c_str(), "goldFont.fnt", "GJ_button_06.png");
            sprite->setScale(.55f);
            m_voteButton->setSprite(sprite);
        }
        enabled(m_voteButton, active);
    }
    // REROLL 투표 버튼 표시/활성 갱신. Season 1의 맵 탐색/준비 단계에서만 보이고,
    // 맵이 정해졌고 아직 시작 전이며 내가 투표하지 않았을 때만 누를 수 있다.
    void updateVoteButton() {
        if (!m_voteButton) return;
        auto const* match = m_state && m_state->match && !m_lastResult ? &*m_state->match : nullptr;
        bool const show = match && (match->state == "map_search" || match->state == "preparing") && !m_season2;
        m_voteButton->setVisible(show);
        if (!show) return;
        auto const& room = Service::get().room();
        bool const started = room && room->id == match->roomId && room->started;
        bool const canVote = match->state == "preparing" && match->selectedMap && !match->ownReroll && !m_voteSending &&
                             !started && (match->startAt <= 0 || countdownLeft(*match) > 0);
        setVote(match->ownReroll || match->opponentReroll ? "REROLL 1/2" : "REROLL", canVote);
    }
    // 리롤 투표를 서버로 보낸다. 봇 대전에서는 리롤이 없다.
    void onVoteReroll(CCObject*) {
        if (m_botActive) {
            Notification::create("No reroll in a bot battle.", NotificationIcon::Info, 2.f)->show();
            return;
        }
        if (!m_state || !m_state->match || m_voteSending || m_transitioning) return;
        auto const match = *m_state->match;
        if (match.state != "preparing" || !match.selectedMap || match.ownReroll) return;
        m_voteSending = true;
        updateVoteButton();
        audio::play(audio::Cue::Reroll);
        Service::get().voteRankedReroll(
            match.id, match.drawId,
            [self = Weak<RankedLayer>(this)](std::optional<RankedState> value, std::string detail) {
                auto owner = self.lock();
                if (!owner || owner->m_transitioning) return;
                owner->m_voteSending = false;
                if (value)
                    owner->receive(std::move(*value));
                else {
                    owner->render();
                    error(detail);
                }
            });
    }
    // 맵 다운로드 표시를 보여 줄 단계인지 (결과 전 맵 탐색/준비).
    static bool downloadPhase(RankedMatch const& match) {
        return !match.result && (match.state == "map_search" || match.state == "preparing");
    }
    // 선택된 맵의 길이(플레이 시간 또는 TINY~PLATFORMER), 라이프(maxAttempts), 2인 플레이
    // 표시를 갱신하고
    // 각 요소를 가운데 정렬한다. 2인 플레이 맵이면 확대 후 붉게 깜빡이는 강조 연출을 건다.
    void updateMapLength(RankedMatch const& match) {
        if (!m_preview) return;
        auto* value = static_cast<CCLabelBMFont*>(m_preview->getChildByID("ranked-map-length"_spr));
        if (!value) return;
        static constexpr char const* names[] = {"TINY", "SHORT", "MEDIUM", "LONG", "XL", "PLATFORMER"};
        int const length = match.selectedMap ? match.selectedMap->length : -1;
        double const seconds = match.selectedMap ? mapPlaySeconds(match.selectedMap->id) : -1.;
        std::string const text = seconds > 0.                 ? playTimeText(seconds)
                                 : length >= 0 && length <= 5 ? names[length]
                                                              : "---";
        if (text != value->getString()) {
            value->setString(text.c_str());
            value->limitLabelWidth(60.f, m_lengthScale, .2f);
        }
        float const middle = m_preview->getContentSize().width / 2.f;
        float center = m_lengthX >= 0.f ? m_lengthX : middle;
        float const textWidth = value->getScaledContentSize().width;
        auto* clock = m_preview->getChildByID("ranked-map-length-clock"_spr);
        float const clockWidth = clock ? clock->getScaledContentSize().width + 3.f : 0.f;
        float const left = center - (clockWidth + textWidth) / 2.f;
        if (clock) clock->setPositionX(left + clockWidth / 2.f - 1.5f);
        value->setPositionX(left + clockWidth + textWidth / 2.f);
        auto* lives = typeinfo_cast<CCLabelBMFont*>(m_preview->getChildByID("ranked-map-lives"_spr));
        auto* heart = m_preview->getChildByID("ranked-map-lives-heart"_spr);
        center = m_livesX >= 0.f ? m_livesX : middle;
        if (lives && heart) {
            bool const shown = match.selectedMap.has_value() && match.maxAttempts > 0;
            lives->setVisible(shown);
            heart->setVisible(shown);
            if (shown) {
                std::string const count = std::to_string(match.maxAttempts);
                if (count != lives->getString()) {
                    lives->setString(count.c_str());
                    lives->limitLabelWidth(40.f, m_livesScale, .2f);
                }
                float const heartWidth = m_heartWidth, livesWidth = lives->getScaledContentSize().width;
                float const start = center - (heartWidth + 3.f + livesWidth) / 2.f;
                heart->setPositionX(start + heartWidth / 2.f);
                lives->setPositionX(start + heartWidth + 3.f + livesWidth / 2.f);
            }
        }
        if (auto* twoPlayer = static_cast<CCLabelBMFont*>(m_preview->getChildByID("ranked-map-two-player"_spr))) {
            bool const shown = match.selectedMap && mapTwoPlayer(match.selectedMap->id) == 1;
            auto* back = m_preview->getChildByID("ranked-map-two-player-back"_spr);
            float const scale = m_twoPlayerScale > 0.f ? m_twoPlayerScale : twoPlayer->getScale();
            if (shown && !twoPlayer->isVisible()) {
                twoPlayer->setScale(scale * 1.6f);
                twoPlayer->runAction(CCEaseBackOut::create(CCScaleTo::create(.35f, scale)));
                twoPlayer->runAction(CCRepeatForever::create(CCSequence::create(
                    CCTintTo::create(.45f, 255, 150, 140),
                    CCTintTo::create(.45f, kTwoPlayerRed.r, kTwoPlayerRed.g, kTwoPlayerRed.b), nullptr)));
                if (back) {
                    back->setScale(1.6f);
                    back->runAction(CCEaseBackOut::create(CCScaleTo::create(.35f, 1.f)));
                }
            }
            if (!shown) {
                twoPlayer->stopAllActions();
                twoPlayer->setScale(scale);
                twoPlayer->setColor(kTwoPlayerRed);
                if (back) {
                    back->stopAllActions();
                    back->setScale(1.f);
                }
            }
            twoPlayer->setVisible(shown);
            if (back) back->setVisible(shown);
        }
    }
    // 메인 액션 버튼의 문구와 활성 상태를 바꾼다. Season 2 보드 확장 상태에서는 전용 맞춤
    // 함수를 쓴다.
    void setAction(std::string const& text, bool active) {
        if (!m_action) return;
        if (m_actionText != text) {
            m_actionText = text;
            if (m_season2 && m_s2Expanded == kS2Board) {
                m_s2ActionKey.clear();
                season2FitAction(true);
            } else {
                auto* sprite = ButtonSprite::create(text.c_str(), "goldFont.fnt", "GJ_button_01.png");
                localizeSkipMusic(sprite, text);
                sprite->setScale(.55f);
                m_action->setSprite(sprite);
                m_s2ActionKey.clear();
            }
        }
        enabled(m_action, active);
    }
    // 현재 상태(결과/큐/매치 단계)를 ranked::Phase로 변환한다.
    ranked::Phase phaseFor(RankedState const& state) const {
        if (m_lastResult) {
            if (m_resultTime >= ranked::RATING_ANIMATION_SECONDS && m_lastResult->oldTier != m_lastResult->newTier)
                return ranked::Phase::RankPromotion;
            return ranked::Phase::RankUpdate;
        }
        if (!state.match) return state.queuedAt > 0 ? ranked::Phase::Searching : ranked::Phase::Idle;
        auto const& match = *state.match;
        if (match.state == "canceled") return ranked::Phase::Canceled;
        if (match.state == "map_search" || match.state == "banning") return ranked::Phase::MatchFound;
        if (match.state == "preparing")
            return match.ownReady ? ranked::Phase::WaitingOpponentReady : ranked::Phase::AssetPreparing;
        if (match.state == "playing") return ranked::Phase::Playing;
        if (match.state == "loading") return ranked::Phase::BattleSceneLoading;
        return ranked::Phase::WaitingBattleStart;
    }
    // Ranked 화면을 떠나 홈으로 돌아간다. Season 2는 로비 음악을 유지한 채 Season 2
    // 화면으로 간다.
    void transitionHome() {
        if (m_transitioning) return;
        m_transitioning = true;
        if (m_season2) {
            m_keepMusic = true;
            showSeason2();
            return;
        }
        audio::stopRankedMusic(true);
        showVersus();
    }
    int placementGames() const {
        return m_state && m_state->profile.placementRequired > 0 ? m_state->profile.placementRequired
                                                                 : ranked::PLACEMENT_MATCH_COUNT;
    }
    static std::string tierText(std::string const& tier, std::string const& name) {
        return name.empty() ? tier : season2TierName(name);
    }
    // 서버에 결과 확인(acknowledge)을 보낸다. 실패하면 2초 뒤 재시도하고,
    // 성공 후 나가기가 예약되어 있었으면 홈으로 이동한다.
    void acknowledge(std::string const& id) {
        if (m_resultAcknowledged || m_ackPending || m_ackRetry > 0.f || id.empty()) return;
        m_ackPending = true;
        Service::get().acknowledgeRanked(id, [self = Weak<RankedLayer>(this), id](bool ok, std::string detail) {
            auto owner = self.lock();
            if (!owner) return;
            // 응답이 늦게 와서 그 사이 다른 매치를 보고 있어도 '보내는 중' 표시는 풀어야 한다
            // (안 풀면 확인을 다시 보내지 못해 결과 화면이 멈춘다).
            owner->m_ackPending = false;
            if (owner->m_observedMatch != id) return;
            owner->m_resultAcknowledged = ok;
            owner->m_ackRetry = ok ? 0.f : 2.f;
            if (!ok) {
                owner->m_status->setString(detail.c_str());
                // Season 2 결과 화면에서는 상태 줄이 숨겨져 있으므로 알림으로도 보여 준다.
                if (owner->m_season2 && !detail.empty())
                    Notification::create(detail, NotificationIcon::Warning, 2.f)->show();
            } else if (owner->m_leaveAfterAck)
                owner->transitionHome();
            else {
                owner->m_pollTimer = ranked::STATE_POLL_NOW;
                owner->render();
            }
        });
    }
    std::optional<RankedDownloadFail> m_downloadFail;
    // 장면이 현재 실행 중인 씬일 때만 대기 중인 알림(다운로드 실패 팝업 또는 점검/일반
    // 안내)을 한 번 보여 준다.
    void showNoticeWhenReady() {
        if ((m_notice.empty() && !m_downloadFail) || !isRunning()) return;
        CCNode* scene = this;
        while (scene->getParent()) scene = scene->getParent();
        if (scene != CCDirector::sharedDirector()->getRunningScene()) return;
        if (m_downloadFail) {
            auto const fail = *std::exchange(m_downloadFail, std::nullopt);
            if (auto* popup = DownloadFailPopup::create(fail)) popup->show();
            return;
        }
        auto text = std::exchange(m_notice, {});
        if (showMaintenanceNotice(text)) return;
        FLAlertLayer::create("Ranked Match", text.c_str(), "OK")->show();
    }
    // state와 마지막 결과를 바탕으로 화면 전체를 다시 그린다.
    // 티어/레이팅/배치 진행, 인원 수, 밴드 테마, 각 위젯 표시 여부, 상태/상세 문구, 액션
    // 버튼을 단계별로 정한다.
    // 배치 중이거나 레이팅 애니메이션 중에는 티어와 레이팅을 숨긴다.
    void render() {
        if (!m_state) return;
        auto const& p = m_state->profile;
        bool changing = m_lastResult && m_resultTime < ranked::RATING_ANIMATION_SECONDS;
        bool visibleRating = m_lastResult ? m_lastResult->placementAfter >= placementGames() : p.placed;
        bool revealing = m_lastResult && m_lastResult->placementComplete;
        bool hidden = !visibleRating || (revealing && changing);
        std::string tier = m_lastResult ? (changing ? m_lastResult->oldTier : m_lastResult->newTier) : p.tier;
        std::string const tierName =
            m_lastResult ? (changing ? m_lastResult->oldTierName : m_lastResult->newTierName) : p.tierName;
        if (hidden) tier = "Unranked";
        displayFace(tier);
        m_tier->setString(hidden ? "PLACEMENT" : tierText(tier, tierName).c_str());
        m_tier->limitLabelWidth(m_leftWidth - 18.f, .58f, .28f);
        m_rating->setString(hidden ? "Rating ----" : fmt::format("R{:.0f}", m_displayRating).c_str());
        m_rating->limitLabelWidth(m_leftWidth - 16.f, .53f, .25f);
        m_placement->setString((hidden ? fmt::format("{} / {}   Tier ----",
                                                     m_lastResult ? m_lastResult->placementAfter : p.placementCompleted,
                                                     std::max(1, placementGames()))
                                : p.globalRank > 0
                                    ? fmt::format("#{} / {}  |  Top {:.1f}%", p.globalRank, p.population, p.percentile)
                                    : "READY TO RANK")
                                   .c_str());
        m_placement->limitLabelWidth(m_leftWidth - 16.f, .27f, .17f);
        if (m_selfRank != p.globalRank) refreshSelfCard();
        rankedGradient(m_tier, tier);
        rankedGradient(m_rating, tier);
        if (m_season2)
            season2Badge(tier, hidden ? std::string("PLACEMENT") : tierText(tier, tierName),
                         hidden ? fmt::format("{} / {}",
                                              m_lastResult ? m_lastResult->placementAfter : p.placementCompleted,
                                              std::max(1, placementGames()))
                                : fmt::format("R{:.0f}", m_displayRating));
        if (m_s2Billboard) m_s2Billboard->setRating(hidden ? -1. : std::round(m_displayRating), tier);
        m_population->setString(
            fmt::format("SEARCHING {}    /    PLAYING {}", m_state->searchingPlayers, m_state->playingPlayers).c_str());
        bool matchPreview = m_state->match && !m_state->match->result && m_state->match->state != "canceled";
        applyBand(matchPreview && !m_lastResult ? rankedBandKey(*m_state->match) : std::string());
        m_searchBoard->setVisible(!matchPreview && !m_lastResult);
        m_boardActions->setVisible(!matchPreview && !m_lastResult && !m_season2);
        if (m_seasonLeft) m_seasonLeft->setVisible(!matchPreview && !m_lastResult);
        if (m_tools) m_tools->setVisible(!matchPreview && !m_lastResult);
        if (m_toolCaptions) m_toolCaptions->setVisible(!matchPreview && !m_lastResult);
        refreshDodgeBadge(!matchPreview && !m_lastResult);
        bool const idle = !m_lastResult && !m_state->match && m_state->queuedAt <= 0 &&
                          m_state->blockedUntil <= Service::get().serverNow();
        if (idle != m_toolsExpanded) {
            m_toolsExpanded = idle;
            m_boardActions->stopAllActions();
            m_boardActions->runAction(CCEaseSineOut::create(
                CCSpawn::create(CCMoveTo::create(.18f, {m_toolsX, idle ? kToolsIdleY : kToolsCompactY}),
                                CCScaleTo::create(.18f, idle ? kToolsIdleScale : 1.f), nullptr)));
        }
        m_preview->setVisible(matchPreview || m_lastResult.has_value());
        m_confirm->setVisible(m_lastResult.has_value() && !season2DrawWaiting());
        m_action->setVisible(!season2DrawWaiting());
        bool const queueWaiting = m_state->queuedAt > 0 && !m_state->match && !m_lastResult;
        m_runnerButton->setVisible(queueWaiting && !m_season2);
        enabled(m_runnerButton, queueWaiting && !m_season2 && !m_pending);
        enabled(m_confirm, m_resultAcknowledged && !changing && !m_pending);
        m_detail->setFntFile(m_lastResult ? "bigFont.fnt" : "chatFont.fnt");
        // 결과 화면: 승/패/무, 티어 변동(RANK UP/DOWN), 배치 완료 여부에 따른 문구와 레이팅
        // 변화.
        if (m_lastResult) {
            bool tierChanged = visibleRating && m_lastResult->oldTier != m_lastResult->newTier && !changing;
            m_status->setString(revealing        ? "PLACEMENT COMPLETE"
                                : !visibleRating ? "PLACEMENT MATCH"
                                : tierChanged
                                    ? (m_lastResult->newRating > m_lastResult->oldRating ? "RANK UP!" : "RANK DOWN")
                                : m_lastResult->result == "win"  ? "VICTORY"
                                : m_lastResult->result == "loss" ? "DEFEAT"
                                                                 : "DRAW");
            m_detail->setString(
                (!visibleRating ? fmt::format("{} / {}  |  Rating ----  Tier ----", m_lastResult->placementAfter,
                                              std::max(1, placementGames()))
                 : revealing    ? fmt::format("#{} / {}   Top {:.1f}%", m_lastResult->newGlobalRank,
                                              m_lastResult->newPopulation, m_lastResult->newPercentile)
                                : fmt::format("R{:+.0f}", m_lastResult->newRating - m_lastResult->oldRating))
                    .c_str());
            rankedGradient(m_detail, m_lastResult->newTier);
            setAction(m_resultAcknowledged ? "PLAY AGAIN" : "SAVING...",
                      !changing && !m_pending && m_resultAcknowledged);
            // 매치 단계별 문구: 취소, 재대결 대기, 밴, 맵 탐색, 준비(카운트다운, 다운로드 실패,
            // 재시도) 등.
        } else if (m_state->match) {
            auto const& match = *m_state->match;
            if (match.state == "canceled") {
                m_status->setString("MATCH CANCELLED");
                m_detail->setString("Rating unchanged");
                setAction("RETURNING...", false);
            } else if (match.state == "rematch_wait") {
                m_status->setString(match.rerolled ? "REROLL - NEW MAP" : "DRAW - REMATCH");
                m_detail->setString(match.rerolled ? "Both players chose Reroll. Loading the next map..."
                                                   : "Waiting for both players to return. Rating unchanged.");
                setAction(match.rerolled ? "NEXT MAP" : "NEXT ROUND", false);
            } else if (match.state == "banning") {
                m_status->setString("BAN A MAP");
                m_detail->setString("");
                setAction(match.bans && match.bans->own ? "WAITING..." : "BAN A MAP", false);
            } else if (match.state == "map_search") {
                updateMapLength(match);
                m_status->setString(match.quickReroll        ? "REROLL - NEW ARENA"
                                    : match.rematchRound > 0 ? "DRAW - NEW ARENA"
                                    : match.duel             ? "DUEL MATCH FOUND"
                                                             : "MATCH FOUND");
                m_detail->setString(match.quickReroll         ? "40% fewer lives on the next map. Finding your arena..."
                                    : match.duel && m_season2 ? "Duel - no rating change. Finding your arena..."
                                    : match.duel ? "Duel request - rating range ignored. Finding your arena..."
                                                 : "Finding your arena...");
                setAction("GET READY", false);
            } else if (match.state == "preparing") {
                updateMapLength(match);
                auto progress =
                    match.selectedMap ? mapDownloadProgress(match.selectedMap->id, true) : MapDownloadProgress{};
                int const left = countdownLeft(match);
                std::string const vote = match.ownReroll ? "Reroll requested - waiting for your opponent."
                                         : match.opponentReroll
                                             ? "Your opponent wants a new map - press REROLL to agree."
                                             : "";
                if (left > 0) {
                    m_status->setString(fmt::format("BATTLE STARTS IN {}", left).c_str());
                    m_detail->setString(!vote.empty() ? vote.c_str()
                                        : m_season2   ? "Both maps ready."
                                                      : "Both maps ready. Both players can REROLL for a new map.");
                    setAction(fmt::format("START IN {}", left), false);
                } else if (match.startAt > 0) {
                    m_status->setString("BATTLE STARTING");
                    m_detail->setString(match.selectedMap ? match.selectedMap->name.c_str() : "");
                    setAction("STARTING...", false);
                } else if (!m_failedDraw.empty() && m_failedDraw == match.drawId) {
                    m_status->setString(m_failureMapSide ? "MAP NOT PLAYABLE" : "MAP DOWNLOAD FAILED");
                    m_detail->setString(m_failureMapSide
                                            ? "This map cannot be played in Versus. Asking for a new map..."
                                            : fmt::format("Tried {} times. Waiting for your opponent's download...",
                                                          std::max(1, m_assetAttempts))
                                                  .c_str());
                    setAction("GET READY", false);
                } else {
                    bool const retrying = m_assetMatch == match.drawId && !m_assetsReady && m_assetAttempts > 1;
                    m_status->setString(match.ownReady ? "WAITING FOR OPPONENT" : "PREPARING MATCH");
                    m_detail->setString(
                        !vote.empty() ? vote.c_str()
                        : retrying
                            ? fmt::format("Map download failed - trying again ({}/{})...", m_assetAttempts, kAssetTries)
                                  .c_str()
                        : match.selectedMap ? fmt::format("{} / {}", match.selectedMap->name, progress.detail()).c_str()
                                            : "Selecting map...");
                    bool const skip = canSkipMusic();
                    setAction(skip ? "Skip Music" : "GET READY", skip);
                }
            } else {
                m_status->setString("BATTLE READY");
                m_detail->setString("Both players loading...");
                setAction("STARTING...", false);
            }
            // 매치 탐색 중 경과 시간 표시. 서버 시각 기준 값과 로컬 타이머 값이 kSearchDrift(3초)
            // 이상 어긋나면
            // 서버 기준으로 다시 맞춘다.
        } else if (m_state->queuedAt > 0) {
            auto const now = std::chrono::steady_clock::now();
            int64_t const server = std::max<int64_t>(0, Service::get().serverNow() - m_state->queuedAt);
            int64_t local = m_searchSince == m_state->queuedAt
                                ? std::chrono::duration_cast<std::chrono::milliseconds>(now - m_searchAnchor).count()
                                : -1;
            if (local < 0 || std::abs(local - server) > kSearchDrift) {
                m_searchSince = m_state->queuedAt;
                m_searchAnchor = now - std::chrono::milliseconds(server);
                local = server;
            }
            auto elapsed = local / 1000;
            m_status->setString("FINDING OPPONENT");
            m_detail->setString(fmt::format("{:02}:{:02}", elapsed / 60, elapsed % 60).c_str());
            setAction("CANCEL", !m_pending);
            // 매치메이킹 제한 중: 남은 시간과 사유(닷지 한도 도달 또는 매칭 후 취소 누적)를
            // 표시한다.
        } else if (m_state->blockedUntil > Service::get().serverNow()) {
            auto seconds = (m_state->blockedUntil - Service::get().serverNow() + 999) / 1000;
            m_status->setString("MATCHMAKING RESTRICTED");
            bool const dodged = m_state->dodge.known && m_state->dodge.blockedUntil > Service::get().serverNow();
            m_detail->setString(fmt::format("Remaining  {}:{:02}  |  {}", seconds / 60, seconds % 60,
                                            dodged ? fmt::format("{} dodges", m_state->dodge.limit)
                                                   : std::string("3 matched cancellations"))
                                    .c_str());
            m_detail->setColor(ccc3(255, 185, 110));
            setAction(fmt::format("WAIT {}:{:02}", seconds / 60, seconds % 60), false);
        } else {
            m_status->setString("");
            m_detail->setString("");
            setAction("FIND MATCH", !m_pending);
        }
        // 다운로드 단계에서는 상태/상세 문구 대신 다운로드 막대를 보여 준다.
        updateVoteButton();
        bool const gauges = !m_lastResult && m_state->match && downloadPhase(*m_state->match);
        m_status->setVisible(!gauges);
        m_detail->setVisible(!gauges);
        if (m_downloadRow) m_downloadRow->setVisible(gauges);
        m_status->limitLabelWidth(m_searchBoard->getContentSize().width - 12.f, .43f, .25f);
        m_detail->limitLabelWidth(m_searchBoard->getContentSize().width - 12.f, .35f, .22f);
        layoutActionRow();
        // Season 2는 현재 단계(연출/결과/밴/매치/보드)에 맞춰 레이아웃을 다시 잡는다.
        if (m_season2) {
            int const mode = m_s2Intro                 ? kS2Intro
                             : m_lastResult            ? kS2Result
                             : matchPreview && m_s2Ban ? kS2Ban
                             : matchPreview            ? kS2Match
                                                       : kS2Board;
            season2Layout(mode);
            season2Arrange(mode);
        }
    }
    // Season 1용 닷지 스택 배지(DODGE n/limit). 누르면 규칙 팝업을 연다.
    static constexpr float kDodgeWidth = 46.f, kDodgeHeight = 28.f, kDodgeGap = 10.f;
    CCMenuItemSpriteExtra* m_dodgeBadge = nullptr;
    CCLabelBMFont* m_dodgeValue = nullptr;
    int m_dodgeStackSeen = -1;
    std::optional<RankedDodge> m_dodgePenalty;
    void makeDodgeBadge(CCMenu* parent, CCPoint position) {
        auto* sprite = CCSprite::create();
        sprite->setContentSize({kDodgeWidth, kDodgeHeight});
        auto* back = NineSlice::create("square02b_001.png");
        back->setContentSize({kDodgeWidth, kDodgeHeight});
        back->setColor(ccc3(18, 26, 58));
        back->setOpacity(235);
        back->setPosition({kDodgeWidth / 2.f, kDodgeHeight / 2.f});
        sprite->addChild(back);
        auto* title = CCLabelBMFont::create("DODGE", "chatFont.fnt");
        title->setScale(.5f);
        title->setColor(kMuted);
        title->setPosition({kDodgeWidth / 2.f, kDodgeHeight - 8.f});
        sprite->addChild(title);
        m_dodgeValue = CCLabelBMFont::create("0/3", "bigFont.fnt");
        m_dodgeValue->setScale(.36f);
        m_dodgeValue->setPosition({kDodgeWidth / 2.f, 9.f});
        sprite->addChild(m_dodgeValue);
        m_dodgeBadge = CCMenuItemSpriteExtra::create(sprite, this, menu_selector(RankedLayer::onDodgeInfo));
        m_dodgeBadge->setID("ranked-dodge-stack"_spr);
        m_dodgeBadge->setPosition(position);
        m_dodgeBadge->setVisible(false);
        parent->addChild(m_dodgeBadge);
    }
    // Season 2의 닷지 표시(원형 버튼 안의 숫자).
    CircleButtonSprite* m_s2DodgeCircle = nullptr;
    CCLabelBMFont* m_s2DodgeValue = nullptr;
    void refreshSeason2Dodge() {
        if (!m_s2DodgeValue) return;
        bool const known = m_state && m_state->dodge.known;
        auto const text = known ? fmt::format("{}/{}", m_state->dodge.stack, m_state->dodge.limit) : std::string("-/-");
        if (text != m_s2DodgeValue->getString()) {
            m_s2DodgeValue->setString(text.c_str());
            if (m_s2DodgeCircle) {
                auto const top = m_s2DodgeCircle->getMaxTopSize();
                auto const size = m_s2DodgeValue->getContentSize();
                m_s2DodgeValue->setScale(
                    std::min(top.width / std::max(1.f, size.width), top.height / std::max(1.f, size.height)));
            }
        }
        m_s2DodgeValue->setColor(known ? dodgeColor(m_state->dodge.stack, m_state->dodge.limit) : kMuted);
    }
    // 닷지 배지를 갱신한다. 서버가 닷지 정보를 알려 준 경우에만 표시한다.
    void refreshDodgeBadge(bool shown) {
        refreshSeason2Dodge();
        if (!m_dodgeBadge || !m_dodgeValue) return;
        bool const visible = shown && m_state && m_state->dodge.known;
        m_dodgeBadge->setVisible(visible);
        if (!visible) return;
        auto const& dodge = m_state->dodge;
        auto const text = fmt::format("{}/{}", dodge.stack, dodge.limit);
        if (text != m_dodgeValue->getString()) m_dodgeValue->setString(text.c_str());
        m_dodgeValue->setColor(dodgeColor(dodge.stack, dodge.limit));
    }
    void onDodgeInfo(CCObject*) {
        if (!m_state || !m_state->dodge.known) return;
        if (auto* popup = DodgeInfoPopup::create(m_state->dodge)) popup->show();
    }
    // 마지막으로 확인한 패널티 시각을 저장하는 설정 키 (시즌별로 분리).
    static char const* dodgeSeenKey(RankedDodge const& dodge) {
        return dodge.season2 ? "s2-dodge-penalty-seen" : "dodge-penalty-seen";
    }
    // 새 닷지 정보를 처리한다. 아직 보지 않은 패널티가 24시간 이내면 팝업 대기열에 넣고,
    // 오래된 것은 본 것으로 기록한다. 스택만 늘었으면 알림을 띄운다.
    void noteDodge(RankedDodge const& dodge) {
        if (!dodge.known) return;
        auto const seen = Mod::get()->getSavedValue<int64_t>(dodgeSeenKey(dodge), 0);
        if (dodge.penaltyAt > 0 && dodge.penaltyAt != seen) {
            if (Service::get().serverNow() - dodge.penaltyAt < 86'400'000)
                m_dodgePenalty = dodge;
            else
                Mod::get()->setSavedValue<int64_t>(dodgeSeenKey(dodge), dodge.penaltyAt);
        } else if (m_dodgeStackSeen >= 0 && dodge.stack > m_dodgeStackSeen)
            Notification::create(fmt::format("Dodge recorded: {}/{}", dodge.stack, dodge.limit),
                                 NotificationIcon::Warning, 3.f)
                ->show();
        m_dodgeStackSeen = dodge.stack;
    }
    // 현재 씬이고 다운로드 실패 팝업이 없을 때 대기 중인 닷지 패널티 팝업을 한 번 띄우고
    // 본 것으로 저장한다.
    void showDodgePenaltyWhenReady() {
        if (!m_dodgePenalty || m_transitioning || !isRunning()) return;
        CCNode* scene = this;
        while (scene->getParent()) scene = scene->getParent();
        if (scene != CCDirector::sharedDirector()->getRunningScene()) return;
        if (m_downloadFail || scene->getChildByID("versus-download-fail"_spr)) return;
        auto const dodge = *std::exchange(m_dodgePenalty, std::nullopt);
        Mod::get()->setSavedValue<int64_t>(dodgeSeenKey(dodge), dodge.penaltyAt);
        (void)Mod::get()->saveData();
        if (auto* popup = DodgeInfoPopup::create(dodge, true)) popup->show();
    }
    // 하단 도구 버튼 크기/간격 상수와 별/오브 보상 토글 버튼.
    static constexpr float kToolSize = 28.f, kToolPitch = 34.f, kToolGap = 16.f;
    static constexpr float kRewardsWidth = 46.f, kRewardsGap = 7.f;
    CCMenuItemSpriteExtra* m_rewardsItem = nullptr;
    // 별/오브 보상이 실제로 켜져 있는지 (기능 빌드 플래그 + 사용자 설정).
    static bool rankedRewardsOn() {
        return ::versus::progress::kStarsOrbsEnabled && Mod::get()->getSavedValue<bool>("ranked-rewards", false);
    }
    static CCSprite* rewardsSprite(bool on) {
        auto* sprite = CCSprite::create();
        sprite->setContentSize({kRewardsWidth, 24.f});
        auto place = [sprite, on](char const* frame, float height, float x, bool icon) {
            auto* part = loadedFrameSprite(frame, "GJ_GameSheet03.plist");
            if (!part) return;
            part->setScale(height / std::max(1.f, part->getContentSize().height));
            part->setPosition({x, 12.f});
            if (icon && !on) {
                part->setOpacity(120);
                part->setColor(ccc3(170, 170, 170));
            }
            sprite->addChild(part);
        };
        place(on ? "GJ_checkOn_001.png" : "GJ_checkOff_001.png", 19.f, 9.5f, false);
        place("GJ_starsIcon_001.png", 13.f, 26.f, true);
        place("currencyOrbIcon_001.png", 14.f, 39.5f, true);
        return sprite;
    }
    void refreshRewards() {
        if (m_rewardsItem) m_rewardsItem->setSprite(rewardsSprite(rankedRewardsOn()));
    }
    // 켜져 있으면 바로 끄고, 꺼져 있으면 동의 팝업을 거쳐 켠다.
    void onRewards(CCObject*) {
        if (rankedRewardsOn()) {
            Mod::get()->setSavedValue("ranked-rewards", false);
            refreshRewards();
            return;
        }
        if (auto* popup = RankedRewardsPopup::create([self = Weak<RankedLayer>(this)] {
                if (auto owner = self.lock()) owner->refreshRewards();
            }))
            popup->show();
    }
    CCMenu* m_tools = nullptr;
    CCNode* m_toolCaptions = nullptr;
    static constexpr float kS1ToolsY = 23.f;
    static constexpr int kS1Tools = 3;
    static constexpr char const* kS1ToolNames[kS1Tools] = {"History", "Music", "Info"};
    static constexpr char const* kS1RatingNames[4] = {"Top", "Tier", "Life", "Points"};
    // 하단 액션 행(도구 버튼, 메인 버튼, 리롤 버튼, 러너 버튼, 닷지 배지)을 가운데 정렬로
    // 배치한다.
    void layoutActionRow() {
        if (!m_action) return;
        auto* image = m_action->getNormalImage();
        float const buttonWidth = image ? image->getScaledContentSize().width : 110.f;
        bool const dodge = m_dodgeBadge && m_dodgeBadge->isVisible();
        if (m_voteButton && m_voteButton->isVisible()) {
            auto* vote = m_voteButton->getNormalImage();
            float const voteWidth = vote ? vote->getScaledContentSize().width : 70.f;
            float const total = buttonWidth + 10.f + voteWidth;
            m_action->setPositionX(m_actionCenter - total / 2.f + buttonWidth / 2.f);
            m_voteButton->setPositionX(m_actionCenter + total / 2.f - voteWidth / 2.f);
        } else if (m_tools && m_tools->isVisible()) {
            float const rewards = m_rewardsItem ? kRewardsWidth + kRewardsGap : 0.f;
            float const toolsWidth = rewards + kToolPitch * (kS1Tools - 1) + kToolSize;
            float const badge = dodge ? kDodgeGap + kDodgeWidth : 0.f;
            float const left = m_actionCenter - (toolsWidth + kToolGap + buttonWidth + badge) / 2.f;
            m_tools->setPosition({left + rewards + kToolSize / 2.f, kS1ToolsY});
            if (m_toolCaptions) m_toolCaptions->setPosition(m_tools->getPosition());
            m_action->setPositionX(left + toolsWidth + kToolGap + buttonWidth / 2.f);
        } else {
            m_action->setPositionX(m_actionCenter - (m_lastResult ? 65.f : 0.f));
        }
        if (m_runnerButton) m_runnerButton->setPositionX(m_action->getPositionX() + buttonWidth / 2.f + 24.f);
        if (dodge) {
            float const after = m_runnerButton && m_runnerButton->isVisible()
                                    ? m_runnerButton->getPositionX() + 15.f
                                    : m_action->getPositionX() + buttonWidth / 2.f;
            m_dodgeBadge->setPosition({after + kDodgeGap + kDodgeWidth / 2.f, 20.f});
        }
    }
    std::vector<std::string> m_lobbyEmotes;
    // 로비 퀵 이모트 버튼들과 '더 보기' 버튼을 다시 만든다.
    void rebuildLobbyEmotes() {
        if (!m_boardActions) return;
        m_boardActions->removeAllChildren();
        m_lobbyEmotes = emotes::quickSet(Service::get().profile().membership);
        int const count = static_cast<int>(m_lobbyEmotes.size());
        float const pitch = 26.f, start = -(count)*pitch / 2.f;
        for (int index = 0; index < count; ++index) {
            auto* item = CCMenuItemSpriteExtra::create(emoteTile(m_lobbyEmotes[index], 23.f, ccc3(15, 22, 31)), this,
                                                       menu_selector(RankedLayer::onLobbyEmote));
            item->setTag(index);
            item->setPosition({start + index * pitch, 0.f});
            item->setID(fmt::format("ranked-lobby-emote-{}", index));
            m_boardActions->addChild(item);
        }
        auto* sprite = CCSprite::create();
        sprite->setContentSize({23.f, 23.f});
        auto* bg = NineSlice::create("square02b_001.png");
        bg->setContentSize({23.f, 23.f});
        bg->setColor(ccc3(70, 30, 105));
        bg->setPosition({11.5f, 11.5f});
        sprite->addChild(bg);
        auto* dots = CCDrawNode::create();
        for (int i = 0; i < 4; ++i)
            dots->drawDot({7.5f + (i % 2) * 8.f, 7.5f + (i / 2) * 8.f}, 2.6f, {1.f, .85f, .4f, 1.f});
        sprite->addChild(dots);
        auto* more = CCMenuItemSpriteExtra::create(sprite, this, menu_selector(RankedLayer::onLobbyEmotePicker));
        more->setPosition({start + count * pitch, 0.f});
        more->setID("ranked-lobby-emote-more"_spr);
        m_boardActions->addChild(more);
    }
    void onLobbyEmote(CCObject* sender) {
        int const tag = static_cast<CCNode*>(sender)->getTag();
        if (tag >= 0 && tag < static_cast<int>(m_lobbyEmotes.size())) sendLobbyEmote(m_lobbyEmotes[tag]);
    }
    void onLobbyEmotePicker(CCObject*) {
        if (auto* popup = EmotePickerPopup::create(
                [self = Weak<RankedLayer>(this)](std::string kind) {
                    if (auto owner = self.lock()) owner->sendLobbyEmote(std::move(kind));
                },
                [self = Weak<RankedLayer>(this)] {
                    if (auto owner = self.lock()) owner->rebuildLobbyEmotes();
                }))
            popup->show();
    }
    bool m_lobbyEmoteSending = false;
    float m_lobbyEmoteCooldown = 0.f;
    // 로비에서 이모트를 보낸다. 로컬 연출을 먼저 보여 주고 서버에 전송하며, 1.25초
    // 쿨다운을 둔다.
    void sendLobbyEmote(std::string kind) {
        if (m_lobbyEmoteSending || m_lobbyEmoteCooldown > 0.f || !m_state || m_state->match || m_lastResult) return;
        m_lobbyEmoteSending = true;
        m_lobbyEmoteCooldown = 1.25f;
        m_searchBoard->localEmote(kind);
        Service::get().sendRankedLobbyEmote(
            std::move(kind), [self = Weak<RankedLayer>(this)](std::optional<RankedState> state, std::string detail) {
                auto owner = self.lock();
                if (!owner) return;
                owner->m_lobbyEmoteSending = false;
                if (!state) {
                    if (!detail.empty()) error(detail);
                    return;
                }
                if (auto sink = owner->stateSink()) sink(std::move(*state));
            });
    }
    // Season 2 매치에서 GDDL 확인용 목록 중 내가 로컬에 기록을 가진 맵(mapLocalBest >=
    // 0)을
    // 서버에 보고한다. 같은 매치/키는 한 번만 보내고, 실패하면 다시 보낼 수 있게 키를
    // 비운다.
    std::string m_s2PlayedKey;
    void season2Played(RankedMatch const& match) {
        if (!m_season2 || match.gddlCheck.empty() || match.gddlCheckKey.empty()) return;
        auto const key = match.id + "|" + match.gddlCheckKey;
        if (m_s2PlayedKey == key) return;
        m_s2PlayedKey = key;
        std::vector<int> played;
        for (int const id : match.gddlCheck)
            if (mapLocalBest(id) >= 0) played.push_back(id);
        Service::get().reportSeason2Played(
            match.id, match.gddlCheckKey, std::move(played), [self = Weak<RankedLayer>(this), key](bool ok) {
                if (auto owner = self.lock(); owner && !ok && owner->m_s2PlayedKey == key) owner->m_s2PlayedKey.clear();
            });
    }
    // 맵 탐색 단계에서 이 클라이언트가 후보 맵 제공자일 때 무작위 후보를 찾아 서버에
    // 제출한다.
    // 실패했거나 이미 플레이한 맵은 제외한다. 후보가 모자라면 상태 문구를 바꾸고 재시도할
    // 수 있게 한다.
    void beginCandidates(RankedMatch const& match) {
        if (m_introTime > 0.f || !match.candidateProvider || match.state != "map_search" ||
            m_candidateMatch == match.id)
            return;
        m_candidateMatch = match.id;
        MapSelection filter{true, match.mapMask, false};
        auto excluded = match.failedMaps;
        excluded.insert(excluded.end(), match.playedMaps.begin(), match.playedMaps.end());
        size_t const needed = m_season2 ? 1 : MIN_RANDOM_CANDIDATES;
        if (match.gddlLow >= 0 && match.gddlPool.empty()) {
            m_status->setString("MAP SEARCH RETRYING...");
            m_detail->setString("No demon of this tier's GDDL range is available.");
            return;
        }
        RandomMapExtras extras{match.mapLengths, match.mapFeatures, match.gddlPool, m_season2 ? 1 : 0};
        extras.minLikes = match.minLikes;
        extras.maxSeconds = match.maxSeconds;
        extras.noTwoPlayer = match.noTwoPlayer;
        findRandomMaps(filter, std::move(extras),
                       [self = Weak<RankedLayer>(this), id = match.id, failed = std::move(excluded),
                        needed](std::vector<LevelInfo> levels, std::string detail) mutable {
                           auto owner = self.lock();
                           if (!owner || owner->m_transitioning || owner->m_candidateMatch != id) return;
                           std::erase_if(levels, [&](auto const& level) {
                               return std::find(failed.begin(), failed.end(), level.id) != failed.end();
                           });
                           if (detail.empty() && levels.size() < needed)
                               detail = needed == MIN_RANDOM_CANDIDATES ? "Searching for five new Classic maps..."
                                                                        : "Searching for a map...";
                           if (!detail.empty()) {
                               owner->m_candidateMatch.clear();
                               owner->m_status->setString("MAP SEARCH RETRYING...");
                               owner->m_detail->setString(detail.c_str());
                               return;
                           }
                           Service::get().submitRankedCandidates(
                               id, std::move(levels),
                               [self, id](std::optional<RankedState> state, std::string errorText) {
                                   if (auto owner = self.lock(); owner && !owner->m_transitioning) {
                                       if (state)
                                           owner->receive(std::move(*state));
                                       else {
                                           owner->m_candidateMatch.clear();
                                           owner->m_status->setString(errorText.c_str());
                                       }
                                   }
                               });
                       });
    }
    // 맵 다운로드 재시도: 최대 kAssetTries회, 맵 선택 후 kAssetRetryUntilMs(ms) 안에서만
    // 재시도한다.
    static constexpr int kAssetTries = 3;
    static constexpr int64_t kAssetRetryUntilMs = 45'000;
    int m_assetAttempts = 0;
    // 맵 자체의 문제(플랫포머, 음원 제공자 차단, 레벨 데이터 없음, 다운로드 URL 없음)로
    // 인한 실패인지 판별한다.
    static bool mapSideFailure(std::string const& detail) {
        return detail == ::versus::kPlatformerMapMessage ||
               detail.find("not allowed by the audio provider") != std::string::npos ||
               detail.find("contains no level data") != std::string::npos ||
               detail.find("has no downloadable audio URL") != std::string::npos;
    }
    // 다운로드 중인 현재 추첨에서만 음악 건너뛰기를 허용한다.
    bool canSkipMusic() const {
        if (m_pending || m_ackPending || m_transitioning || m_assetsReady || !m_assetPreparing || !m_state ||
            !m_state->match)
            return false;
        auto const& match = *m_state->match;
        return match.state == "preparing" && !match.ownReady && match.selectedMap && m_assetMatch == match.drawId &&
               !mapAudioSkipped(match.selectedMap->id);
    }
    // 선택된 맵(과 음악)을 로컬에 받아 둔다. 룰렛 연출이 끝나기 전에는 시작하지 않는다.
    // 실패 시 맵 문제가 아니고 횟수/시간이 남았으면 2초 뒤 재시도하고,
    // 그렇지 않으면 실패로 기록한 뒤 서버에 맵 리롤을 요청한다.
    void prepareAssets(RankedMatch const& match) {
        auto const& room = Service::get().room();
        if (m_introTime > 0.f || match.result || match.state != "preparing" || !match.selectedMap || !room ||
            room->id != match.roomId)
            return;
        if (!m_season2 && !match.quickReroll && room->mapDraw &&
            Service::get().serverNow() < room->mapDraw->at + DRAW_END_MS)
            return;
        if (m_rerollSending || m_failedDraw == match.drawId) return;
        if (m_assetPreparing || (m_assetMatch == match.drawId && m_assetsReady) || m_assetRetry > 0.f) return;
        if (m_assetMatch != match.drawId) m_assetAttempts = 0;
        m_assetMatch = match.drawId;
        m_assetPreparing = true;
        ++m_assetAttempts;
        ensureMapCached(
            match.selectedMap->id,
            [self = Weak<RankedLayer>(this), id = match.drawId, selectedAt = match.selectedAt](bool ok,
                                                                                               std::string detail) {
                auto owner = self.lock();
                if (!owner || owner->m_assetMatch != id || owner->m_transitioning) return;
                owner->m_assetPreparing = false;
                owner->m_assetsReady = ok;
                owner->m_readyTimer = 0.f;
                if (ok && owner->m_state && owner->m_state->match && owner->m_state->match->selectedMap &&
                    mapAudioSkipped(owner->m_state->match->selectedMap->id))
                    owner->m_readyTimer = ranked::READY_RESEND_SECONDS;
                if (ok && detail.rfind("Song ", 0) == 0) owner->m_songMissingDraw = id;
                if (ok && !detail.empty() && owner->m_audioNoticeDraw != id) {
                    owner->m_audioNoticeDraw = id;
                    log::warn("Versus Ranked map: {}", detail);
                    Notification::create(detail, NotificationIcon::Warning, 4.f)->show();
                }
                if (!ok) {
                    log::warn("Versus Ranked map download try {}/{} failed: {}", owner->m_assetAttempts, kAssetTries,
                              detail);
                    bool const mapSide = mapSideFailure(detail);
                    if (!mapSide && owner->m_assetAttempts < kAssetTries &&
                        Service::get().serverNow() - selectedAt < kAssetRetryUntilMs) {
                        owner->m_assetRetry = 2.f;
                        owner->render();
                        return;
                    }
                    owner->m_failedDraw = id;
                    owner->m_failureReason = detail;
                    owner->m_failureMapSide = mapSide;
                    owner->render();
                    owner->requestReroll();
                }
                owner->render();
            },
            rankedAudioNeed());
        render();
    }
    std::string m_audioNoticeDraw;
    std::string m_songMissingDraw;
    // 다운로드 실패한 맵의 리롤을 서버에 요청한다. 사유는 map(맵 문제) 또는 network로
    // 구분해 보낸다.
    void requestReroll() {
        if (m_rerollSending || m_failedDraw.empty() || !m_state || !m_state->match || m_assetRetry > 0.f) return;
        auto const& match = *m_state->match;
        if (match.drawId != m_failedDraw || match.state != "preparing") return;
        m_rerollSending = true;
        Service::get().rerollRankedMap(
            match.id, m_failedDraw, m_failureReason, m_failureMapSide ? "map" : "network",
            [self = Weak<RankedLayer>(this)](std::optional<RankedState> state, std::string detail) {
                if (auto owner = self.lock(); owner && !owner->m_transitioning) {
                    owner->m_rerollSending = false;
                    if (state)
                        owner->receive(std::move(*state));
                    else {
                        owner->m_assetRetry = 2.f;
                        owner->m_detail->setString(detail.c_str());
                    }
                }
            });
    }
    // 서버에서 받은 RankedState를 반영하는 중심 함수.
    // Season 2 결과 연출 중이면 새 매치 상태를 보류하고, 다른 컵의 매치면 컵 전환으로
    // 넘기며,
    // 오래된(selectedAt이 과거) 상태는 버린다. 새 매치/새 추첨이면 관련 진행 상태를 모두
    // 초기화한다.
    void receive(RankedState next) {
        if (m_botActive) return;
        if (m_season2 && m_lastResult && m_s2DrawShown >= 0.f && m_s2DrawShown < kS2DrawHold && next.match &&
            next.match->id != m_observedMatch && next.match->rematchRound > 0 && !next.match->result &&
            next.match->state != "canceled") {
            season2Played(*next.match);
            m_s2HeldState = std::move(next);
            return;
        }
        m_s2HeldState.reset();
        handleDuels(next);
        if (!m_s2CupSwitch.empty()) return;
        if (m_season2 && !next.matchCup.empty() && next.matchCup != Service::get().rankedCup()) {
            season2CrossCup(next.matchCup);
            return;
        }
        noteDodge(next.dodge);
        if (next.match || (next.queuedAt <= 0 && !m_season2)) closeQueueRunner();
        if (next.match && next.match->id == m_dismissedResult) next.match.reset();
        if (m_state && m_state->match && next.match && m_state->match->id == next.match->id &&
            next.match->selectedAt < m_state->match->selectedAt)
            return;
        bool const firstState = !m_state.has_value();
        auto const incomingMatch = next.match ? next.match->id : std::string{};
        // 확인을 못 받은 결과가 있는데 서버 상태에 그 매치가 더는 없으면, 서버는 이미 확인을
        // 받은 것이다(응답만 잃어버린 경우). 결과 화면의 버튼이 영영 잠기지 않게 확인된 것으로 둔다.
        if (m_lastResult && !m_resultAcknowledged && incomingMatch.empty()) {
            m_resultAcknowledged = true;
            m_ackPending = false;
            m_ackRetry = 0.f;
        }
        if (!incomingMatch.empty() && incomingMatch != m_observedMatch) {
            m_lastResult.reset();
            m_delta->stopAllActions();
            m_delta->setString("");
            m_s2DrawShown = -1.f;
            m_observedMatch = incomingMatch;
            m_candidateMatch.clear();
            m_assetMatch.clear();
            m_shownDraw.clear();
            m_assetsReady = false;
            m_assetPreparing = false;
            m_readySending = false;
            m_assetRetry = 0.f;
            m_readyTimer = 0.f;
            m_resultAcknowledged = false;
            m_ackPending = false;
            m_ackRetry = 0.f;
            m_observedDraw.clear();
            m_observedSelection = 0;
            m_previewDraw.clear();
            m_notifiedRerolls = 0;
            m_integritySending = false;
        }
        if (next.match && (m_observedDraw != next.match->drawId || m_observedSelection != next.match->selectedAt)) {
            auto oldLevel =
                m_state && m_state->match && m_state->match->selectedMap ? m_state->match->selectedMap->id : 0;
            m_observedDraw = next.match->drawId;
            m_observedSelection = next.match->selectedAt;
            m_assetMatch.clear();
            m_candidateMatch.clear();
            m_failedDraw.clear();
            m_failureMapSide = false;
            if (oldLevel) cancelMapPreparation(oldLevel);
            m_assetsReady = false;
            m_assetPreparing = false;
            m_readySending = false;
            m_assetRetry = 0;
            // 서버가 맵을 다시 뽑았으면(failedMaps 증가) 사유를 안내 문구로 남긴다.
            if (next.match->failedMaps.size() > m_notifiedRerolls && !next.match->rerollReason.empty()) {
                m_notifiedRerolls = next.match->failedMaps.size();
                m_notice = "Map re-roll: " + next.match->rerollReason;
            }
        }
        // 새 결과가 도착한 경우: 안티치트 실격 안내, 레이팅 애니메이션 초기화, 결과 화면 구성.
        if (next.match && next.match->result &&
            (!m_lastResult || m_lastResult->matchId != next.match->result->matchId)) {
            m_lastResult = next.match->result;
            // 안티치트로 실격 처리된 결과면 한 번만 알려 준다 (누가 실격되었는지, 감지된 기능).
            if (!m_lastResult->integrityCode.empty() && m_shownIntegrityResult != m_lastResult->matchId) {
                m_shownIntegrityResult = m_lastResult->matchId;
                bool const self = m_lastResult->disqualifiedUid == Service::get().profile().uid;
                auto message = fmt::format(
                    "{} was <cr>disqualified</c> by the {} anti-cheat.\nDetected: {}\nThe Ranked result was applied immediately.",
                    self ? "You" : "Your opponent", integrity::sourceName(m_lastResult->integrityCode),
                    integrity::featureSummary(m_lastResult->integrityCode, m_lastResult->integrityFeatures));
                FLAlertLayer::create("Anti-Cheat", message.c_str(), "OK")->show();
            }
            m_displayRating = m_lastResult->oldRating;
            m_resultTime = 0.f;
            m_tierAnimated = false;
            m_s2DrawShown =
                m_season2 && m_lastResult->result == "draw" && m_lastResult->disqualifiedUid.empty() ? 0.f : -1.f;
            audio::play(audio::Cue::Rating);
            // 매치 미리보기를 비우고 결과 표시용으로 다시 채운다. 다운로드 위젯 참조도 함께
            // 끊는다.
            m_preview->removeAllChildren();
            m_mapCard = nullptr;
            m_ldmToggle = nullptr;
            m_opponentLdm = nullptr;
            m_downloadText[0] = m_downloadText[1] = nullptr;
            m_downloadFill[0] = m_downloadFill[1] = nullptr;
            m_downloadRow = nullptr;
            if (m_season2) season2Result(*m_lastResult, next.profile.placementRequired, next.match->selectedMap);
            // Season 1 결과 화면: 제목, VICTORY/DEFEAT/DRAW, 연승 보너스, 패배 감소 방패,
            // 레이팅 변화, 다음 티어까지 남은 점수, 레이팅 변화량 숫자 연출.
            else {
                auto boardSize = m_preview->getContentSize();
                bool const reveal = m_lastResult->placementComplete;
                int const games =
                    next.profile.placementRequired > 0 ? next.profile.placementRequired : ranked::PLACEMENT_MATCH_COUNT;
                bool const provisional = m_lastResult->placementAfter < games;
                auto* resultTitle = label(m_preview,
                                          reveal        ? "PLACEMENT COMPLETE"
                                          : provisional ? "PLACEMENT"
                                                        : "RATING UPDATE",
                                          {boardSize.width / 2.f, boardSize.height - 30.f}, .56f,
                                          boardSize.width - 24.f, ccWHITE, false, "gjFont46.fnt");
                rankedTitleGradient(resultTitle);
                auto* outcome = label(m_preview,
                                      m_lastResult->result == "win"    ? "VICTORY"
                                      : m_lastResult->result == "loss" ? "DEFEAT"
                                                                       : "DRAW",
                                      {boardSize.width / 2.f, boardSize.height * .52f}, 1.45f, boardSize.width * .88f,
                                      m_lastResult->result == "loss" ? ccc3(255, 116, 99) : ccc3(255, 210, 125));
                auto finalScale = outcome->getScale();
                outcome->setScale(finalScale * .75f);
                outcome->runAction(CCEaseBackOut::create(CCScaleTo::create(.4f, finalScale)));
                if (m_lastResult->result == "win" && m_lastResult->winStreak >= 2 && m_lastResult->streakBonus > 0.) {
                    auto text = provisional || reveal ? fmt::format("WIN STREAK x{}", m_lastResult->winStreak)
                                                      : fmt::format("WIN STREAK x{}   +{:.0f} BONUS",
                                                                    m_lastResult->winStreak, m_lastResult->streakBonus);
                    auto* streak = label(m_preview, text, {boardSize.width / 2.f + 8.f, boardSize.height * .52f - 34.f},
                                         .36f, boardSize.width - 60.f, ccc3(255, 160, 60), false, "bigFont.fnt");
                    auto* flame = art::glyph(art::Glyph::Flame, ccc3(255, 120, 40));
                    flame->setScale(1.3f);
                    flame->setPosition(streak->getPosition() -
                                       CCPoint{streak->getScaledContentSize().width / 2.f + 12.f, 0.f});
                    m_preview->addChild(flame);
                    streak->setOpacity(0);
                    streak->runAction(CCSequence::create(CCDelayTime::create(.45f), CCFadeIn::create(.3f), nullptr));
                }
                bool const shielded =
                    m_lastResult->result == "loss" && m_lastResult->lossReduction > 0. && m_lastResult->fullLoss < 0;
                if (shielded) showLossShield(*m_lastResult, boardSize, provisional || reveal);
                auto summary =
                    provisional ? fmt::format("{} / {}   Rating ----", m_lastResult->placementAfter, std::max(1, games))
                    : reveal    ? fmt::format("{0} / {0}  |  REVEALING...", std::max(1, games))
                                : fmt::format("R{:.0f}  >  R{:.0f}", m_lastResult->oldRating, m_lastResult->newRating);
                auto* resultRating = label(m_preview, summary, {boardSize.width / 2.f, 28.f}, .57f,
                                           boardSize.width - 24.f, ccWHITE, false, "bigFont.fnt");
                rankedGradient(resultRating, m_lastResult->newTier);
                if (reveal) {
                    auto result = *m_lastResult;
                    resultRating->runAction(CCSequence::create(
                        CCDelayTime::create(ranked::RATING_ANIMATION_SECONDS),
                        CallFuncExt::create([target = Weak<CCLabelBMFont>(resultRating), result, boardSize] {
                            if (auto text = target.lock()) {
                                text->setString(fmt::format("{}  /  R{:.0f}",
                                                            tierText(result.newTier, result.newTierName),
                                                            result.newRating)
                                                    .c_str());
                                text->limitLabelWidth(boardSize.width - 24.f, .57f, .25f);
                                auto scale = text->getScale();
                                text->setScale(scale * .7f);
                                text->runAction(CCEaseBackOut::create(CCScaleTo::create(.4f, scale)));
                            }
                        }),
                        nullptr));
                }
                // 배치가 끝난 뒤에는 서버에서 티어표를 받아 다음 티어까지의 안내 문구를 지연 후
                // 페이드인한다.
                if (!provisional && m_season2) {
                    auto* tierLine = label(m_preview, "", {boardSize.width / 2.f, 11.f}, .5f, boardSize.width - 24.f,
                                           ccc3(255, 214, 60), false, "chatFont.fnt");
                    tierLine->setID("ranked-next-tier"_spr);
                    float const delay = reveal ? ranked::RATING_ANIMATION_SECONDS : .6f;
                    Service::get().fetchSeason2Tiers(
                        Service::get().rankedCup(),
                        [target = Weak<CCLabelBMFont>(tierLine), rating = m_lastResult->newRating, delay,
                         width = boardSize.width](std::optional<Season2Tiers> tiers, std::string) {
                            auto text = target.lock();
                            if (!text || !tiers) return;
                            text->setString(season2NextTier(*tiers, rating).c_str());
                            text->limitLabelWidth(width - 24.f, .5f, .25f);
                            text->setOpacity(0);
                            text->runAction(
                                CCSequence::create(CCDelayTime::create(delay), CCFadeIn::create(.3f), nullptr));
                        });
                } else if (!provisional) {
                    auto* tierLine = label(m_preview, "", {boardSize.width / 2.f, 11.f}, .5f, boardSize.width - 24.f,
                                           ccc3(255, 214, 60), false, "chatFont.fnt");
                    tierLine->setID("ranked-next-tier"_spr);
                    float const delay = reveal ? ranked::RATING_ANIMATION_SECONDS : .6f;
                    Service::get().fetchRankedTiers(
                        [target = Weak<CCLabelBMFont>(tierLine), result = *m_lastResult, delay,
                         width = boardSize.width](std::optional<RankedTiers> tiers, std::string) {
                            auto text = target.lock();
                            if (!text || !tiers) return;
                            auto const step = nextRankedTier(*tiers, result.newTier, result.newRating);
                            auto const value =
                                step ? fmt::format("{} more to {} (R{}+)", step->points, step->tier, step->cutoff + 1)
                                     : std::string("You are in the top tier");
                            text->setString(value.c_str());
                            text->limitLabelWidth(width - 24.f, .5f, .25f);
                            text->setOpacity(0);
                            text->runAction(
                                CCSequence::create(CCDelayTime::create(delay), CCFadeIn::create(.3f), nullptr));
                        },
                        true);
                }
                m_delta->stopAllActions();
                m_delta->setOpacity(255);
                m_delta->setPosition(m_preview->getPosition() + CCPoint{boardSize.width / 2.f, 46.f});
                m_delta->setScale(.5f);
                m_delta->setString(
                    provisional || reveal || shielded
                        ? ""
                        : fmt::format("{:+.0f}", m_lastResult->newRating - m_lastResult->oldRating).c_str());
                m_delta->setColor(m_lastResult->newRating >= m_lastResult->oldRating ? ccc3(120, 255, 175)
                                                                                     : ccc3(255, 105, 95));
                rankedGradient(m_delta, m_lastResult->newTier);
                m_delta->runAction(CCSequence::create(
                    CCSpawn::create(CCMoveBy::create(2.f, {0.f, 5.f}),
                                    CCSequence::create(CCDelayTime::create(1.2f), CCFadeOut::create(.8f), nullptr),
                                    nullptr),
                    nullptr));
            }
            // 결과를 서버에 확인(acknowledge)해 다음 상태 조회에서 같은 결과가 반복되지 않게 한다.
            acknowledge(next.match->id);
        } else if (firstState) {
            m_displayRating = next.profile.rating;
        } else if (!m_lastResult)
            m_displayRating = next.profile.rating;
        // 취소된 매치: 다운로드 실패면 전용 팝업을, 아니면 취소 사유 안내를 한 번만 예약한다.
        if (next.match && next.match->state == "canceled" && m_cancelShown != next.match->id) {
            m_cancelShown = next.match->id;
            if (next.match->downloadFail)
                m_downloadFail = next.match->downloadFail;
            else
                m_notice = next.match->cancelReason.empty() ? "Ranked match was canceled." : next.match->cancelReason;
            acknowledge(next.match->id);
        }
        if (next.match && next.match->state == "rematch_wait") acknowledge(next.match->id);
        // 상대의 리롤 요청 알림과, 양쪽이 리롤에 동의했을 때의 알림(효과음 포함).
        if (next.match && !next.match->result) {
            auto const& match = *next.match;
            auto const key = match.id + "/" + match.drawId;
            if (match.state == "preparing" && match.opponentReroll && !match.ownReroll && m_opponentVoteSeen != key) {
                m_opponentVoteSeen = key;
                audio::play(audio::Cue::Reroll);
                Notification::create("Your opponent wants a new map: press REROLL to agree.", NotificationIcon::Info,
                                     3.f)
                    ->show();
            }
            if (m_voteRerollMatch != match.id) {
                m_voteRerollMatch = match.id;
                m_voteRerollsSeen = match.voteRerolls;
            } else if (match.voteRerolls > m_voteRerollsSeen) {
                m_voteRerollsSeen = match.voteRerolls;
                audio::play(audio::Cue::Reroll);
                Notification::create("Both players chose Reroll: picking a new map.", NotificationIcon::Success, 2.5f)
                    ->show();
            }
        }
        // 매치가 없어졌거나 끝났으면 Season 2 밴 화면과 프리페치를 정리한다.
        if (!next.match || next.match->state == "canceled" || next.match->result) {
            season2EndBans();
            season2DropPrefetch(next.match && next.match->selectedMap ? next.match->selectedMap->id : 0);
        }
        // 취소/없음: 소개 연출을 중단하고 후보 탐색과 에셋 준비 상태를 초기화한다.
        if (!next.match || next.match->state == "canceled") {
            if (auto* clash = getChildByID("ranked-clash"_spr)) clash->removeFromParent();
            if (m_s2Intro) {
                if (m_s2Intro->placed()) m_s2Room = m_introMatch;
                m_s2Intro->abort();
                m_s2Intro = nullptr;
            }
            m_introTime = 0.f;
            m_candidateMatch.clear();
            m_assetMatch.clear();
            m_assetsReady = false;
        }
        // 새 상태를 적용하고 phase를 갱신한 뒤, 매치 진행에 필요한 후속 동작을 이어서
        // 실행한다:
        // 소개 연출, 미리보기, 후보 맵 제출, 룰렛 팝업(Season 1), 에셋 준비.
        m_state = std::move(next);
        ++m_stateRevision;
        m_phase = phaseFor(*m_state);
        if (m_state->match) {
            auto const& match = *m_state->match;
            season2Played(match);
            beginIntro(match);
            if (!match.result && match.state != "canceled" && m_previewDraw != match.drawId) {
                m_previewDraw = match.drawId;
                rebuildPreview(match);
            }
            beginCandidates(match);
            if (m_season2) season2Bans(match);
            auto const& room = Service::get().room();
            if (!m_season2 && m_introTime <= 0.f && room && room->id == match.roomId && room->mapDraw &&
                room->mapDraw->id != m_shownDraw && !match.quickReroll &&
                Service::get().serverNow() < room->mapDraw->at + DRAW_END_MS) {
                m_shownDraw = room->mapDraw->id;
                if (auto* popup = MapRoulettePopup::create(*room)) popup->show();
            }
            prepareAssets(match);
            // 방이 시작되어 실제 전투가 진행 중이면(launch 정보가 있고 아직 끝나지 않음) 이 씬이
            // 현재 씬일 때
            // 로비 음악을 멈추고 전투 씬으로 넘어간다.
            if (room && room->id == match.roomId && room->started && room->launch && room->battle &&
                room->battle->finishedAt == 0 && !isMatchLaunchActive() && !m_transitioning) {
                CCNode* scene = this;
                while (scene->getParent()) scene = scene->getParent();
                if (scene == CCDirector::sharedDirector()->getRunningScene()) {
                    m_transitioning = true;
                    audio::stopRankedMusic(false);
                    launchMatch();
                    return;
                }
            }
        }
        render();
    }
    // 서버에서 Ranked 상태를 조회한다. 다른 요청이 진행 중이거나 전환 중, 봇 대전 중이면
    // 건너뛴다.
    void fetch() {
        if (m_botActive) return;
        if (m_fetching || m_pending || m_ackPending || m_rerollSending || m_readySending || m_transitioning) return;
        if (m_s2BanSending) return;
        m_fetching = true;
        Service::get().fetchRankedState(
            [self = Weak<RankedLayer>(this)](std::optional<RankedState> value, std::string detail) {
                auto owner = self.lock();
                if (!owner || owner->m_transitioning) return;
                owner->m_fetching = false;
                if (value)
                    owner->receive(std::move(*value));
                else {
                    owner->m_status->setString("CONNECTION UNAVAILABLE");
                    owner->m_detail->setString(detail.c_str());
                }
            });
    }
    // 탐색/매치 취소 요청. searchCancel이면 탐색 중단이며, 서버 판정(outcome)에 따라
    // 처리한다:
    // 'kept'(이미 매치가 잡혀 유지됨)이면 경고 팝업, 'free'이면 무료 취소로 패널티 없이
    // 알림만 보낸다.
    void cancel(bool leave, bool searchCancel = false, std::string seen = {}) {
        if (m_pending) return;
        m_pending = true;
        setAction(searchCancel ? "STOPPING..." : "CANCELLING...", false);
        Service::get().cancelRanked(
            [self = Weak<RankedLayer>(this), leave, searchCancel](std::optional<RankedState> value,
                                                                  std::string detail) {
                auto owner = self.lock();
                if (!owner) return;
                owner->m_pending = false;
                if (!value) {
                    owner->render();
                    owner->m_status->setString(detail.c_str());
                    return;
                }
                auto const outcome = value->searchCancelOutcome;
                int const left = value->searchCancelsLeft;
                if (searchCancel && outcome == "kept" && value->match && !value->match->result &&
                    value->match->state != "canceled") {
                    owner->m_leaveAfterAck = false;
                    owner->receive(std::move(*value));
                    owner->warnCrossedMatch(leave, left);
                    return;
                }
                if (searchCancel && outcome == "free" && value->match) {
                    owner->m_cancelShown = value->match->id;
                    Notification::create(
                        fmt::format("A match was found as you stopped searching: canceled with no "
                                    "penalty ({} free cancel{} left; each comes back 5 min after use).",
                                    left, left == 1 ? "" : "s"),
                        NotificationIcon::Info, 3.5f)
                        ->show();
                }
                auto id = value->match ? value->match->id : std::string{};
                owner->m_leaveAfterAck = leave;
                owner->receive(std::move(*value));
                if (!id.empty())
                    owner->acknowledge(id);
                else if (leave)
                    owner->transitionHome();
            },
            searchCancel, std::move(seen));
    }
    // 5분 동안 패널티 없이 탐색을 중단할 수 있는 횟수.
    static constexpr int kSearchCancelsFree = 3;
    // 매치를 떠날 때의 경고 문구를 만든다 (전투 시작 중이면 몰수패, 그 외에는 닷지
    // 가중치와 패널티 안내).
    std::string leaveWarning() const {
        if (m_state->match->state == "loading")
            return "The battle is starting: leaving now counts as a <cr>forfeit</c> (a loss).";
        if (m_season2 && !m_state->dodge.known)
            return "Leaving now <cy>cancels this match</c> (no rating change).\n"
                   "Once the battle is starting, leaving counts as a <cr>forfeit</c>.";
        auto const& dodge = m_state->dodge;
        int const weight = dodgeWeight(*m_state->match, dodge);
        int const after = dodge.stack + weight;
        std::string text =
            fmt::format("Leaving now is <cr>{} dodge{}</c> (stack <cy>{}/{}</c> -> <cr>{}</c>).\n", weight,
                        weight == 1 ? "" : "s", dodge.stack, dodge.limit, std::min(after, dodge.limit));
        text += after >= dodge.limit
                    ? fmt::format("<cr>Penalty now:</c> rating as {} losses + {} min without matchmaking.\n",
                                  dodge.penaltyLosses, dodge.blockMinutes)
                    : fmt::format("At {}: rating as {} losses + {} min without matchmaking.\n", dodge.limit,
                                  dodge.penaltyLosses, dodge.blockMinutes);
        return text + "Cancel during synchronization counts as a <cr>forfeit</c>.";
    }
    // 탐색을 멈추는 순간 매치가 잡힌 경우, 계속 머물지 떠날지 묻는 확인창.
    void warnCrossedMatch(bool leave, int left) {
        if (m_cancelDialog || m_transitioning || !m_state || !m_state->match) return;
        m_cancelDialog = true;
        std::string text = "<cy>A match was found just as you stopped searching.</c>\n";
        if (left == 0) text += fmt::format("No free cancels left ({} per 5 minutes).\n", kSearchCancelsFree);
        text += leaveWarning();
        createQuickPopup("Match Found", text, "Stay", "Leave", 380.f,
                         [self = Weak<RankedLayer>(this), leave](auto*, bool yes) {
                             auto owner = self.lock();
                             if (!owner) return;
                             owner->m_cancelDialog = false;
                             if (yes) owner->cancel(leave);
                         });
    }
    bool m_cancelDialog = false;
    // 나가기/취소 확인. 매치가 없으면 바로 탐색 취소, 매치가 있으면 경고 확인창을 먼저
    // 띄운다.
    void confirmCancel(bool leave) {
        if (m_cancelDialog || m_pending || m_transitioning) return;
        bool const matched = m_state && m_state->match && m_state->match->state != "canceled";
        if (!matched) {
            cancel(leave, true, std::string());
            return;
        }
        m_cancelDialog = true;
        createQuickPopup("Leave Ranked?", leaveWarning(), "Stay", "Leave", 380.f,
                         [self = Weak<RankedLayer>(this), leave](auto*, bool yes) {
                             if (auto owner = self.lock()) {
                                 owner->m_cancelDialog = false;
                                 if (yes) owner->cancel(leave);
                             }
                         });
    }
    // 메인 버튼 처리: 큐/매치 중이면 취소 확인, 아니면 치트 검사 후 Ranked 큐에 참가한다.
    // 결과 화면에서 눌렀으면 그 결과를 닫고 새로 탐색한다.
    void onMatch(CCObject*) {
        if (m_pending || m_ackPending || !m_state || (m_lastResult && !m_resultAcknowledged)) return;
        if (m_state->match && m_state->match->state == "preparing") {
            if (canSkipMusic()) skipMapAudio(m_state->match->selectedMap->id);
            render();
            return;
        }
        if (season2DrawWaiting()) return;
        m_s2DrawShown = -1.f;
        if (m_state->blockedUntil > Service::get().serverNow() && !m_state->match) {
            render();
            return;
        }
        if (rankedBlockedByCheat()) return;
        if (m_state->queuedAt > 0 || (m_state->match && !m_state->match->result)) {
            confirmCancel(false);
            return;
        }
        if (m_lastResult) {
            m_displayRating = m_lastResult->newRating;
            m_dismissedResult = m_lastResult->matchId;
        }
        m_lastResult.reset();
        m_pending = true;
        setAction("SEARCHING...", false);
        Service::get().joinRankedQueue(
            [self = Weak<RankedLayer>(this)](std::optional<RankedState> value, std::string detail) {
                if (auto owner = self.lock(); owner && !owner->m_transitioning) {
                    owner->m_pending = false;
                    if (value)
                        owner->receive(std::move(*value));
                    else {
                        owner->render();
                        owner->m_status->setString(detail.c_str());
                        owner->m_pollTimer = ranked::STATE_POLL_NOW;
                    }
                }
            });
    }
    // 디버그 전용 봇 대전 상태 (debug::active()일 때만 사용). 서버 없이 가짜 상대와 매치를
    // 흉내 낸다.
    bool m_botActive = false, m_botLaunching = false;
    float m_botBanIn = -1.f;
    static inline int s_botMatches = 0;
    bool m_botBoardAsked = false;
    PlayerProfile m_botProfile;
    CCMenuItemSpriteExtra* m_botButton = nullptr;
    CCMenuItemSpriteExtra* m_returnButton = nullptr;
    // 봇 프로필을 무작위(이름, 아이콘, 색, 오라 등)로 만든다.
    static PlayerProfile makeBotProfile() {
        static std::mt19937 rng{std::random_device{}()};
        auto pick = [](int low, int high) { return std::uniform_int_distribution<int>(low, high)(rng); };
        auto* manager = GameManager::sharedState();
        auto color = [manager, &pick] {
            auto const c = manager ? manager->colorForIdx(pick(0, 40)) : ccc3(pick(0, 255), pick(0, 255), pick(0, 255));
            return (c.r << 16) | (c.g << 8) | c.b;
        };
        PlayerProfile bot;
        bot.uid = "debug-bot";
        bot.name = fmt::format("Bot{}", pick(100, 999));
        bot.icon = pick(1, 120);
        bot.color1 = color();
        bot.color2 = color();
        bot.membership = true;
        bot.hasCosmetics = true;
        bot.cosmetics = defaultCosmetics(true);
        bot.cosmetics.start = color();
        bot.cosmetics.end = color();
        bot.cosmetics.angle = pick(0, 7) * 45;
        bot.cosmetics.aura = pick(0, kAuraCount - 1);
        bot.cosmetics.execution = pick(1, kExecutionCount - 1);
        return bot;
    }
    // 디버그 전용: Season 2 로비 장면을 처음 상태로 되돌린다. 진행 중인 매치/큐가 있으면
    // 거부한다.
    void onReturn(CCObject*) {
        if (!debug::active() || !m_s2Lounge || m_transitioning || m_s2Return.lock() || m_s2Intro) return;
        if (m_botActive || (m_state && (m_state->match || m_state->queuedAt > 0)) || m_lastResult) {
            Notification::create("Finish or cancel the current Ranked match first.", NotificationIcon::Warning, 2.5f)
                ->show();
            return;
        }
        if (!lounge::active()) {
            Notification::create("The lounge is not open yet.", NotificationIcon::Warning, 2.f)->show();
            return;
        }
        auto* lounge = m_s2Lounge;
        auto* plaza = m_plaza;
        if (auto* cover = Season2Return::create(m_s2Lounge, season2ReturnLook(), .35f, [lounge, plaza] {
                lounge::restart(lounge);
                if (plaza) plaza->nudge();
            })) {
            addChild(cover, -7);
            m_s2Return = cover;
        }
    }
    // 디버그 전용: 봇 매치를 시작한다. 가짜 RankedMatch를 만들어 상태에 넣고 소개 연출을
    // 보인 뒤,
    // 방 없이 무작위 맵을 찾는다 (Season 2는 밴 단계, Season 1은 바로 맵 결정).
    void onBot(CCObject*) {
        if (!debug::active() || m_transitioning || m_botActive || !m_s2CupSwitch.empty()) return;
        if (!m_state || m_state->match || m_state->queuedAt > 0 || m_lastResult || m_pending || Service::get().room()) {
            Notification::create("Finish or cancel the current Ranked match first.", NotificationIcon::Warning, 2.5f)
                ->show();
            return;
        }
        m_botActive = true;
        m_botProfile = makeBotProfile();
        RankedMatch match;
        match.id = m_season2 ? fmt::format("debug-bot-s2-{}", ++s_botMatches) : "debug-bot";
        if (m_season2) {
            static std::mt19937 sides{std::random_device{}()};
            match.side = std::uniform_int_distribution<int>(0, 1)(sides) ? 1 : -1;
        }
        match.state = "map_search";
        match.drawId = "debug-bot-draw";
        match.createdAt = Service::get().serverNow();
        match.mapMask = 0xFF;
        match.opponentReady = true;
        match.ownLdm = Mod::get()->getSavedValue<bool>("ranked-ldm", false);
        match.opponent.player = m_botProfile;
        match.opponent.tier = m_state->profile.tier;
        match.opponent.placed = m_state->profile.placed;
        match.opponent.rating = m_state->profile.rating;
        match.opponent.placementCompleted = 5;
        auto next = *m_state;
        next.match = match;
        m_state = std::move(next);
        ++m_stateRevision;
        m_phase = phaseFor(*m_state);
        beginIntro(match);
        render();
        findRandomMapsWithoutRoom(
            MapSelection{true, match.mapMask, false},
            [self = Weak<RankedLayer>(this)] {
                auto owner = self.lock();
                return owner && owner->m_botActive && !owner->m_transitioning;
            },
            [self = Weak<RankedLayer>(this)](std::vector<LevelInfo> levels, std::string detail) {
                auto owner = self.lock();
                if (!owner || !owner->m_botActive || owner->m_transitioning) return;
                std::erase_if(levels, [](LevelInfo const& level) { return level.id <= 0 || level.platformer; });
                if (levels.empty()) {
                    owner->endBotIntro(detail.empty() ? "Bot battle: no map found. Try again."
                                                      : "Bot battle: " + detail);
                    return;
                }
                static std::mt19937 rng{std::random_device{}()};
                if (owner->m_season2) {
                    owner->botBans(std::move(levels));
                    return;
                }
                owner->botMapFound(levels[std::uniform_int_distribution<size_t>(0, levels.size() - 1)(rng)]);
            });
    }
    // 디버그 봇 대전(Season 2): 찾은 맵 중 중복 없이 kMaxCards(3)장을 골라 밴 단계 상태를
    // 만들고,
    // 봇이 3~9초 뒤에 밴하도록 타이머를 건다. 후보가 3장보다 적으면 중단한다.
    void botBans(std::vector<LevelInfo> levels) {
        if (!m_state || !m_state->match) return;
        static std::mt19937 rng{std::random_device{}()};
        std::shuffle(levels.begin(), levels.end(), rng);
        std::vector<LevelInfo> three;
        for (auto& level : levels) {
            if (std::none_of(three.begin(), three.end(), [&](LevelInfo const& kept) { return kept.id == level.id; }))
                three.push_back(std::move(level));
            if (three.size() == static_cast<size_t>(s2ban::kMaxCards)) break;
        }
        if (three.size() < 3) {
            endBotIntro("Bot battle: fewer than three maps found. Try again.");
            return;
        }
        auto& match = *m_state->match;
        match.state = "banning";
        match.candidates = std::move(three);
        match.selectedAt = Service::get().serverNow();
        RankedMatch::Bans bans;
        bans.seconds = 20;
        bans.endsAt = match.selectedAt + static_cast<int64_t>((m_introTime + 1.f + bans.seconds) * 1000.f);
        match.bans = bans;
        m_botBanIn = std::uniform_real_distribution<float>(3.f, 9.f)(rng);
        m_botBoardAsked = false;
        ++m_stateRevision;
        render();
    }
    // 봇 밴 보조 함수: 아직 밴되지 않은 후보 맵인지 검사하고, 그중 무작위로 하나를 고른다.
    static bool botBannable(RankedMatch const& match, int64_t level) {
        return level > 0 && level != match.bans->own && level != match.bans->opponent &&
               std::any_of(match.candidates.begin(), match.candidates.end(),
                           [&](LevelInfo const& map) { return map.id == level; });
    }
    static int64_t botRandomBan(RankedMatch const& match) {
        std::vector<int64_t> left;
        for (auto const& map : match.candidates)
            if (botBannable(match, map.id)) left.push_back(map.id);
        if (left.empty()) return 0;
        static std::mt19937 rng{std::random_device{}()};
        return left[std::uniform_int_distribution<size_t>(0, left.size() - 1)(rng)];
    }
    // 내가 밴한 맵을 기록한다 (이미 밴했거나 밴할 수 없는 맵이면 무시).
    void botBan(int64_t level) {
        if (!m_state || !m_state->match || m_state->match->state != "banning" || !m_state->match->bans) return;
        auto& match = *m_state->match;
        if (match.bans->own || !botBannable(match, level)) return;
        match.bans->own = level;
        ++m_stateRevision;
        botBansDone();
        render();
    }
    // 봇 밴 단계 진행: 소개가 끝나면 밴 보드를 열고, 봇이 일정 시간 뒤 밴한다.
    // 제한 시간이 끝났는데 밴하지 않은 쪽은 무작위로 밴한 것으로 처리한다.
    void botBanStage(float dt) {
        auto& match = *m_state->match;
        if (m_introTime > 0.f || m_s2Intro) return;
        if (!m_s2Ban && !m_botBoardAsked) {
            m_botBoardAsked = true;
            render();
        }
        bool changed = false;
        if (m_s2Ban && !match.bans->opponent && (m_botBanIn -= dt) <= 0.f) {
            match.bans->opponent = botRandomBan(match);
            changed = true;
        }
        if (Service::get().serverNow() >= match.bans->endsAt) {
            if (!match.bans->own) {
                match.bans->own = botRandomBan(match);
                match.bans->ownRandom = true;
                changed = true;
            }
            if (!match.bans->opponent) {
                match.bans->opponent = botRandomBan(match);
                match.bans->opponentRandom = true;
                changed = true;
            }
        }
        if (!changed) return;
        ++m_stateRevision;
        botBansDone();
        render();
    }
    // 양쪽 밴이 모두 끝나면 남은 후보 중 무작위로 맵을 확정한다.
    void botBansDone() {
        auto const& match = *m_state->match;
        if (!match.bans->own || !match.bans->opponent) return;
        std::vector<LevelInfo> left;
        for (auto const& map : match.candidates)
            if (map.id != match.bans->own && map.id != match.bans->opponent) left.push_back(map);
        if (left.empty()) return;
        static std::mt19937 rng{std::random_device{}()};
        botMapFound(left[std::uniform_int_distribution<size_t>(0, left.size() - 1)(rng)]);
    }
    // 봇 매치용 맵 확정: preparing 상태로 바꾸고 맵을 내려받은 뒤, 준비 완료와 시작
    // 시각(5초 + 남은 소개 시간 후)을 정한다.
    void botMapFound(LevelInfo level) {
        if (!m_state || !m_state->match) return;
        auto& match = *m_state->match;
        match.state = "preparing";
        match.maxAttempts = autoRoomLives(level);
        match.selectedMap = level;
        match.selectedAt = Service::get().serverNow();
        ++m_stateRevision;
        render();
        ensureMapCached(level.id, [self = Weak<RankedLayer>(this), id = level.id](bool ok, std::string detail) {
            auto owner = self.lock();
            if (!owner || !owner->m_botActive || owner->m_transitioning || !owner->m_state || !owner->m_state->match ||
                !owner->m_state->match->selectedMap || owner->m_state->match->selectedMap->id != id)
                return;
            if (!ok) {
                owner->endBotIntro("Bot battle: the map did not download. " + detail);
                return;
            }
            auto& match = *owner->m_state->match;
            match.ownReady = true;
            match.startAt = Service::get().serverNow() + 5000 + static_cast<int64_t>(owner->m_introTime * 1000.f);
            owner->render();
        });
    }
    // 봇 대전을 중단하고 화면 상태를 초기화한다. message가 있으면 안내로 남기고 다음
    // 폴링을 바로 하게 한다.
    void endBotIntro(std::string const& message) {
        if (!m_botActive) return;
        m_botActive = false;
        m_botLaunching = false;
        m_botBanIn = -1.f;
        if (m_season2) {
            if (m_s2Intro) {
                if (m_s2Intro->placed()) m_s2Room = m_introMatch;
                m_s2Intro->abort();
                m_s2Intro = nullptr;
            }
            season2EndBans();
            season2DropPrefetch();
        }
        if (m_state) m_state->match.reset();
        if (auto* clash = getChildByID("ranked-clash"_spr)) clash->removeFromParent();
        m_introTime = 0.f;
        m_preview->removeAllChildren();
        m_mapCard = nullptr;
        m_ldmToggle = nullptr;
        m_opponentLdm = nullptr;
        m_downloadText[0] = m_downloadText[1] = nullptr;
        m_downloadFill[0] = m_downloadFill[1] = nullptr;
        m_downloadRow = nullptr;
        m_previewDraw.clear();
        m_introMatch.clear();
        if (!message.empty()) m_notice = message;
        m_pollTimer = ranked::STATE_POLL_NOW;
        render();
    }
    // 디버그 버튼 표시를 갱신하고, 봇 매치가 진행 중이면 밴 단계를 돌리거나
    // 시작 시각이 되었을 때 봇 전투(startBotBattle)를 시작한다.
    void updateBot(float dt) {
        if (m_botButton)
            m_botButton->setVisible(debug::active() && !m_botActive && m_state && !m_state->match &&
                                    m_state->queuedAt <= 0 && !m_lastResult);
        if (m_returnButton)
            m_returnButton->setVisible(debug::active() && !m_botActive && m_state && !m_state->match &&
                                       m_state->queuedAt <= 0 && !m_lastResult && !m_s2Intro && !m_s2Return.lock());
        if (!m_botActive || m_botLaunching || !m_state || !m_state->match) return;
        auto& match = *m_state->match;
        if (match.state == "banning" && match.bans) {
            botBanStage(dt);
            return;
        }
        match.ownLdm = Mod::get()->getSavedValue<bool>("ranked-ldm", false);
        if (match.state != "preparing" || match.startAt <= 0 || !match.selectedMap ||
            Service::get().serverNow() < match.startAt || m_introTime > 0.f)
            return;
        std::string error;
        if (!Service::get().startBotBattle(*match.selectedMap, m_botProfile, error)) {
            endBotIntro("Bot battle: " + error);
            return;
        }
        m_botLaunching = true;
        m_transitioning = true;
        audio::stopRankedMusic(false);
        launchMatch();
    }
    // 뒤로 가기. 봇 대전/점검 중이면 바로 홈으로, 보류된 Season 2 상태가 있으면 먼저
    // 반영한다.
    // 결과 미확인이면 확인 후 나가고, 큐/매치 중이면 나가기 확인창을 띄운다.
    void goBack() override {
        if (m_pending || m_transitioning) return;
        if (m_botActive) {
            endBotIntro({});
            transitionHome();
            return;
        }
        if (Service::get().maintenance()) {
            transitionHome();
            return;
        }
        if (m_s2HeldState) {
            auto held = std::move(*m_s2HeldState);
            m_s2HeldState.reset();
            m_s2DrawShown = -1.f;
            receive(std::move(held));
            if (m_transitioning) return;
        }
        if (m_lastResult && !m_resultAcknowledged) {
            // 두 번째 뒤로 가기는 확인을 기다리지 않고 나간다 (결과는 서버에 이미 저장되어 있다).
            if (m_leaveAfterAck) {
                transitionHome();
                return;
            }
            m_leaveAfterAck = true;
            acknowledge(m_lastResult->matchId);
            return;
        }
        if (m_state && (m_state->queuedAt > 0 || (m_state->match && !m_state->match->result)))
            confirmCancel(true);
        else
            transitionHome();
    }
    // 결과 화면의 확인 버튼: 레이팅 애니메이션이 끝나고 결과가 서버에 확인된 뒤에만 결과를
    // 닫는다.
    void onConfirm(CCObject*) {
        if (!m_resultAcknowledged || m_pending || m_resultTime < ranked::RATING_ANIMATION_SECONDS) return;
        if (season2DrawWaiting()) return;
        m_s2DrawShown = -1.f;
        if (m_lastResult) {
            m_displayRating = m_lastResult->newRating;
            m_dismissedResult = m_lastResult->matchId;
        }
        m_lastResult.reset();
        if (m_state) m_state->match.reset();
        m_delta->stopAllActions();
        m_delta->setString("");
        render();
    }
    void onMusic(CCObject*) {
        if (auto* popup = RankedMusicPopup::create()) popup->show();
    }
    void onSeason2Music(CCObject*) {
        if (auto* popup = RankedMusicPopup::createSeason2()) popup->show();
    }
    // Season 2 레이아웃 상수 (왕관, 패널, 보드/행 높이, 액션 버튼 크기 등; 좌표 단위는
    // 노드 좌표).
    static constexpr float kS2Rise = 11.f;
    static constexpr float kS2CrownWidth = 54.f, kS2CrownSink = .2f, kS2CrownDrop = 4.f, kS2CrownTop = 6.f;
    static constexpr float kS2PanelLift = 7.f;
    static constexpr float kS2BackClear = 5.5f;
    static constexpr float kS2PanelBorder = .5f;
    NineSlice* m_s2Border = nullptr;
    static constexpr float kS2CrownMargin = 6.f;
    static constexpr float kS2ToolSpan = 3.f;
    float m_s2Top = 0.f;
    ccColor3B m_s2PanelColour = s2::kFrame;
    CCSprite* m_s2Crown = nullptr;
    static constexpr float kS2BoardH = 72.f + kS2Rise, kS2RowH = 34.f, kS2PanelH = 112.f + kS2Rise;
    static constexpr float kS2RowY = 20.f;
    static constexpr float kS2SideY = kS2RowY - 3.f, kS2CaptionY = -1.5f, kS2CaptionPad = 10.f;
    static constexpr float kS2ActionH = 24.3f, kS2ActionNarrow = .7f, kS2ActionY = kS2SideY - 4.f;
    std::string m_s2ActionKey;
    std::string m_s2MeasuredText;
    float m_s2NaturalW = 0.f, m_s2NaturalH = 1.f, m_s2TextW = 1.f;
    // Season 2 보드 상태에서 메인 액션 버튼을 좌우의 다른 위젯과 겹치지 않는 너비로 다시
    // 만든다.
    // 문구와 너비가 같으면 다시 만들지 않는다. 보드가 아니면 기본 버튼 모양으로 되돌린다.
    void season2FitAction(bool board) {
        if (!m_action) return;
        if (!board) {
            if (m_s2ActionKey.empty()) return;
            m_s2ActionKey.clear();
            auto* sprite = ButtonSprite::create(m_actionText.c_str(), "goldFont.fnt", "GJ_button_01.png");
            localizeSkipMusic(sprite, m_actionText);
            sprite->setScale(.55f);
            m_action->setSprite(sprite);
            enabled(m_action, m_action->isEnabled());
            return;
        }
        if (m_s2MeasuredText != m_actionText) {
            m_s2MeasuredText = m_actionText;
            auto* natural = ButtonSprite::create(m_actionText.c_str(), "goldFont.fnt", "GJ_button_01.png");
            m_s2NaturalW = natural ? natural->getContentSize().width : 110.f;
            m_s2NaturalH = natural ? std::max(1.f, natural->getContentSize().height) : 30.f;
            m_s2TextW = natural && natural->m_label ? std::max(1.f, natural->m_label->getContentSize().width) : 90.f;
        }
        float const scale = kS2ActionH / m_s2NaturalH;
        float target = m_s2NaturalW * scale * kS2ActionNarrow;
        float const width = m_root->getContentSize().width, center = width / 2.f;
        float leftEdge = 0.f, rightEdge = width;
        if (m_s2RatingMenu)
            for (auto* item : CCArrayExt<CCNode*>(m_s2RatingMenu->getChildren()))
                leftEdge = std::max(leftEdge, item->getPositionX() + item->getContentSize().width / 2.f);
        if (m_tools) rightEdge = width - 12.f - kToolSize - kToolPitch * kS2ToolSpan;
        float const extra = m_runnerButton && m_runnerButton->isVisible() ? 39.f : 0.f;
        float const room = 2.f * (std::min(center - leftEdge, rightEdge - extra - center) - 8.f);
        target = std::min(target, std::max(room, 24.f));
        float const text = std::max(.2f, std::min(scale, (target - 16.f) / m_s2TextW));
        auto key = fmt::format("{}|{:.0f}|{:.3f}", m_actionText, target, text);
        if (key == m_s2ActionKey) return;
        m_s2ActionKey = std::move(key);
        auto* sprite = ButtonSprite::create(m_actionText.c_str(), static_cast<int>(std::lround(target)), true,
                                            "goldFont.fnt", "GJ_button_01.png", kS2ActionH, text);
        localizeSkipMusic(sprite, m_actionText);
        m_action->setSprite(sprite);
        enabled(m_action, m_action->isEnabled());
    }
    // Season 2 접힘/펼침 애니메이션 상수와 상태 필드.
    static constexpr float kS2FoldTime = .28f, kS2Gap = 2.f;
    static constexpr float kS2ResultCut = s2view::kPanelCut, kS2MatchCut = 36.f;
    float m_s2Lift = 0.f, m_s2Trim = 0.f;
    float m_s2Floor = 0.f;
    float m_s2Fold = 0.f;
    float m_s2Wide = 0.f;
    float m_s2Pad = kS2CaptionPad;
    bool m_s2Folded = false;
    CCClippingNode* m_s2BoardClip = nullptr;
    CCLayerColor* m_s2BoardStencil = nullptr;
    bool m_s2BoardOpen = false;
    CCNode* m_s2RatingCaptions = nullptr;
    CCNode* m_s2ToolCaptions = nullptr;
    float m_s2Tint = 0.f;
    // 맵 밴드의 대표색/보조색을 구한다. 서버가 색을 주면 그대로, 아니면 티어 팔레트에서
    // 계산한다.
    static std::pair<ccColor3B, ccColor3B> season2BandColours(RankedMapBand const& band) {
        auto unpack = [](uint32_t packed) { return ccc3(packed >> 16 & 255, packed >> 8 & 255, packed & 255); };
        if (band.roomMain && band.roomSub) return {unpack(*band.roomMain), unpack(*band.roomSub)};
        bool const placement = band.tier.empty() || band.tier == "Placement";
        auto const palette = rankedPalette(season2PaletteKey(placement ? std::string("Unranked") : band.tier));
        ccColor3B const main = palette.empty() ? s2::kFrame : palette.front();
        ccColor3B sub = mixColor(main, ccBLACK, .45f);
        if (int const frame = rankedFrame(band.tier))
            sub = season2FeatureColor(frame);
        else if (palette.size() > 1 && !(palette[1].r == 255 && palette[1].g == 255 && palette[1].b == 255))
            sub = palette[1];
        return {main, sub};
    }
    // 현재 맵 밴드(없으면 내 티어)의 색을 dark 비율만큼 어둡게 한 카드 배경색.
    ccColor3B season2Plate(float dark) const {
        if (m_state && m_state->match && m_state->match->mapBand)
            return mixColor(season2BandColours(*m_state->match->mapBand).first, ccBLACK, dark);
        bool const placed = m_state && m_state->profile.placed;
        auto const palette = rankedPalette(placed ? m_state->profile.tier : std::string("Unranked"));
        return mixColor(palette.empty() ? s2::kFrame : palette.front(), ccBLACK, dark);
    }
    // 아래는 Season 2 화면의 위젯 참조와 레이아웃/연출 상수 (패널, 라운지, 뱃지, 이모트,
    // 밴 보드, 포커스 연출 등).
    CCNode* m_s2Rows = nullptr;
    CCNode* m_s2Compact = nullptr;
    CCNode* m_s2Full = nullptr;
    CCMenu* m_s2RatingMenu = nullptr;
    CCNode* m_s2Lounge = nullptr;
    CCNode* m_s2Badge = nullptr;
    CCNode* m_s2BadgeFace = nullptr;
    Season2TierText* m_s2BadgeTier = nullptr;
    CCLabelBMFont* m_s2BadgeRating = nullptr;
    CCNode* m_s2BadgeName = nullptr;
    CCNode* m_s2BadgeTitle = nullptr;
    CCNode* m_s2BadgeBubble = nullptr;
    std::string m_s2BadgeArt, m_s2BadgeLook;
    PlazaCrowd* m_plaza = nullptr;
    CCMenu* m_s2Emotes = nullptr;
    CCMenu* m_s2CameraMenu = nullptr;
    CCMenuItemSpriteExtra* m_s2Camera = nullptr;
    float m_s2HideX = 0.f;
    bool m_s2UiHidden = false;
    std::vector<Ref<CCNode>> m_s2Veiled;
    static constexpr float kS2Corner = 20.f, kS2CornerTile = 28.f;
    static constexpr float kS2IdleScale = .85f;
    CCPoint m_s2RootAt;
    std::vector<std::string> m_s2EmoteKinds;
    float m_s2EmoteCooldown = 0.f;
    static constexpr float kS2EmoteY = 20.f, kS2EmoteTile = 24.f, kS2EmotePitch = 29.f;
    static constexpr float kS2NameY = -21.f, kS2TierY = 29.f, kS2TitleY = 49.f, kS2BubbleY = 72.f;
    static constexpr float kS2Face = 17.f;
    static constexpr float kS2TitleScale = .92f;
    CCNode* m_s2Decor = nullptr;
    Season2Billboard* m_s2Billboard = nullptr;
    int m_s2Expanded = -1;
    static constexpr int kS2Board = 0, kS2Match = 1, kS2Result = 2, kS2Intro = 3, kS2Ban = 4;
    static constexpr float kS2BanShare = .6f, kS2BanTime = .35f, kS2FocusTime = .4f;
    static constexpr GLubyte kS2FocusNear = 70, kS2FocusFar = 235;
    static constexpr float kS2FocusSoft = 14.f;
    Season2BanBoard* m_s2Ban = nullptr;
    float m_s2BanGrow = 0.f;
    float m_s2FocusAmount = 0.f;
    bool m_s2BanSending = false;
    std::string m_s2Prefetched;
    std::set<int64_t> m_s2PrefetchIds;
    CCNode* m_s2Focus = nullptr;
    std::array<CCLayerGradient*, 6> m_s2FocusParts{};
    Season2Intro* m_s2Intro = nullptr;
    Weak<Season2Return> m_s2Return;
    std::string m_s2Room;
    std::set<std::string> m_s2Ended;
    int m_s2Builds = 0;
    // Season 2 프레임 패널(반투명 배경)을 만든다.
    static CCNode* s2Frame(CCNode* parent, CCPoint at, CCSize size) {
        auto* node = panel(parent, at, size, false);
        if (auto* back = node->getChildByType<NineSlice*>(0)) {
            back->setColor(s2::kFrame);
            back->setOpacity(s2::kFrameOpacity);
        }
        node->setZOrder(-2);
        return node;
    }
    // Season 2 Ranked 화면을 구성한다: 컵 왕관, 접힘 가능한 패널, 검색 보드, 로비 이모트,
    // 상태/상세 문구, 메인/확인/러너/리롤 버튼, 도구 버튼(기록/음악/게임/안내)과
    // 레이팅 버튼(Top/Tier/Duel/Dodge)을 만든다.
    void buildSeason2(CCNode*) {
        auto const w = m_window.width, h = m_window.height;
        float const side = std::max(
            15.f, m_back ? m_back->getPositionX() + m_back->getScaledContentSize().width / 2.f + kS2BackClear : 15.f);
        float const width = std::min(640.f, w - 2.f * side);
        m_height = h - 58.f;
        m_leftWidth = 140.f;
        m_root = CCNode::create();
        m_root->setPosition({(w - width) / 2.f, 8.f});
        m_s2RootAt = m_root->getPosition();
        m_root->setContentSize({width, m_height});
        m_root->setID("season2-ranked-root"_spr);
        addChild(m_root, 2);
        auto const cup = Service::get().rankedCup();
        m_s2Top = h - kS2CrownTop - season2CrownAbove(season2CupCrown(cup), kS2CrownWidth, kS2CrownSink, kS2CrownDrop) -
                  m_root->getPositionY() + kS2PanelLift;
        m_s2Floor = m_s2Top - kS2CrownMargin - kS2PanelH;
        m_s2Full = s2Frame(m_root, {}, {width, m_height});
        m_s2Full->setVisible(false);
        m_s2Compact = s2Frame(m_root, {0.f, m_s2Floor}, {width, kS2PanelH});
        m_s2Border = NineSlice::create("square02b_001.png");
        m_s2Border->setID("season2-panel-border"_spr);
        m_root->addChild(m_s2Border, -3);
        m_s2PanelColour = season2CupPanel(cup);
        if (auto* back = m_s2Compact->getChildByType<NineSlice*>(0)) back->setOpacity(255);
        if ((m_s2Crown = loadedFrameSprite(season2CupCrown(cup), "GJ_GameSheet04.plist"))) {
            m_s2Crown->setScale(kS2CrownWidth / std::max(1.f, m_s2Crown->getContentSize().width));
            m_s2Crown->setAnchorPoint({.5f, 0.f});
            m_s2Crown->setPosition(
                {width / 2.f, m_s2Top + kS2PanelBorder / 2.f -
                                  m_s2Crown->getScaledContentSize().height * season2CrownSeam(season2CupCrown(cup))});
            m_s2Crown->setID("season2-panel-crown"_spr);
            m_root->addChild(m_s2Crown, 5);
        }
        float const boardWidth = width - 22.f;
        m_searchBoard = RankedSearchBoard::create({boardWidth, kS2BoardH});
        m_searchBoard->useSeason2Look();
        m_s2BoardClip = CCClippingNode::create();
        m_s2BoardStencil = CCLayerColor::create(ccc4(255, 255, 255, 255), boardWidth, 0.f);
        m_s2BoardClip->setStencil(m_s2BoardStencil);
        m_root->addChild(m_s2BoardClip);
        m_searchBoard->setPosition({11.f, m_s2Floor + kS2RowH});
        m_s2BoardClip->addChild(m_searchBoard);
        m_searchBoard->onState(stateSink());
        m_boardActions = menu(m_root);
        m_boardActions->setContentSize({0.f, 0.f});
        m_toolsX = width / 2.f;
        m_boardActions->setPosition({m_toolsX, kToolsCompactY});
        rebuildLobbyEmotes();
        m_preview = CCNode::create();
        m_preview->setContentSize({boardWidth, kS2PanelH - 4.f});
        m_preview->setPosition({11.f, m_s2Floor + 2.f});
        m_root->addChild(m_preview);
        m_delta = label(m_root, "", {width / 2.f, 120.f}, .58f, 160.f, ccWHITE, false, "bigFont.fnt");
        m_delta->setZOrder(20);
        m_s2Rows = CCNode::create();
        m_s2Rows->setPosition({0.f, m_s2Floor});
        m_root->addChild(m_s2Rows, 6);
        float const center = width / 2.f;
        m_actionCenter = center;
        m_status = label(m_s2Rows, "CONNECTING...", {center, kS2RowH + kS2BoardH - 9.f}, .36f, boardWidth - 12.f,
                         ccc3(255, 214, 60));
        m_detail = label(m_s2Rows, "", {center, 40.f}, .35f, boardWidth - 12.f, kMuted, false, "chatFont.fnt");
        auto* rows = menu(m_s2Rows);
        m_action = button(rows, this, menu_selector(RankedLayer::onMatch), "CONNECTING...", {center, kS2RowY}, .55f);
        m_confirm =
            button(rows, this, menu_selector(RankedLayer::onConfirm), "CONFIRM", {center + 75.f, kS2RowY}, .55f);
        m_confirm->setVisible(false);
        m_runnerButton = iconButton(rows, this, menu_selector(RankedLayer::onQueueRunner), "GJ_gpgBtn_001.png",
                                    {center + 78.f, kS2RowY}, 30.f);
        m_runnerButton->setVisible(false);
        m_voteButton = button(rows, this, menu_selector(RankedLayer::onVoteReroll), "REROLL", {center + 80.f, kS2RowY},
                              .55f, "GJ_button_06.png");
        m_voteButton->setID("ranked-reroll-vote"_spr);
        m_voteButton->setVisible(false);
        enabled(m_action, false);
        m_tools = menu(m_s2Rows);
        m_tools->setContentSize({0.f, 0.f});
        iconButton(m_tools, this, menu_selector(RankedLayer::onSeason2Profile), "GJ_profileButton_001.png",
                   {0.f, 0.f}, kToolSize)
            ->setID("season2-profile-button"_spr);
        auto* music = iconButton(m_tools, this, menu_selector(RankedLayer::onSeason2Music), "GJ_audioOnBtn_001.png",
                                 {kToolPitch, 0.f}, kToolSize);
        music->setID("ranked-music-volume-button"_spr);
        auto* game = iconButton(m_tools, this, menu_selector(RankedLayer::onQueueRunner), "GJ_gpgBtn_001.png",
                                {kToolPitch * 2.f, 0.f}, kToolSize);
        game->setID("season2-queue-game-button"_spr);
        iconButton(m_tools, this, menu_selector(RankedLayer::onGuide), "GJ_infoIcon_001.png",
                   {kToolPitch * kS2ToolSpan, 0.f}, kToolSize);
        float const toolsX = width - 12.f - kToolSize / 2.f - kToolPitch * kS2ToolSpan;
        m_tools->setPosition({toolsX, kS2SideY});
        m_s2ToolCaptions = CCNode::create();
        m_s2Rows->addChild(m_s2ToolCaptions, 4);
        season2Caption(m_s2ToolCaptions, "Profile", toolsX);
        season2Caption(m_s2ToolCaptions, "Music", toolsX + kToolPitch);
        season2Caption(m_s2ToolCaptions, "Game", toolsX + kToolPitch * 2.f);
        season2Caption(m_s2ToolCaptions, "Info", toolsX + kToolPitch * kS2ToolSpan);
        // 왼쪽 레이팅 메뉴: 리더보드, 티어 목록, 듀얼, 닷지 버튼과 그 캡션.
        m_s2RatingMenu = menu(m_s2Rows);
        float const tile = 26.f, pitch = 31.f, first = 12.f + tile / 2.f;
        m_s2RatingCaptions = CCNode::create();
        m_s2Rows->addChild(m_s2RatingCaptions, 4);
        season2Caption(m_s2RatingCaptions, "Top", first);
        season2Caption(m_s2RatingCaptions, "Tier", first + pitch);
        season2Caption(m_s2RatingCaptions, "Duel", first + pitch * 2.f);
        season2Caption(m_s2RatingCaptions, "Dodge", first + pitch * 3.f);
        auto* leaderboardButton = iconButton(m_s2RatingMenu, this, menu_selector(RankedLayer::onLeaderboard),
                                             "levelLeaderboard_globalBtn_001.png", {first, kS2SideY}, tile);
        leaderboardButton->setID("ranked-top-100-button"_spr);
        bool const demonFace = loadedFrame("diffIcon_05_btn_001.png", "GJ_GameSheet03.plist");
        CCNode* tiersTile =
            demonFace ? CircleButtonSprite::createWithSpriteFrameName("diffIcon_05_btn_001.png", 1.f,
                                                                      CircleBaseColor::Blue, CircleBaseSize::Medium)
                      : nullptr;
        if (!tiersTile) tiersTile = CCSprite::create();
        tiersTile->setScale(tile / std::max(1.f, tiersTile->getContentSize().height));
        auto* tiersButton = CCMenuItemSpriteExtra::create(tiersTile, this, menu_selector(RankedLayer::onTiers));
        tiersButton->setPosition({first + pitch, kS2SideY});
        m_s2RatingMenu->addChild(tiersButton);
        tiersButton->setID("ranked-tier-list-button"_spr);
        CCNode* duelTile =
            CircleButtonSprite::create(season2DuelArt(20.f), CircleBaseColor::Blue, CircleBaseSize::Medium);
        if (!duelTile) duelTile = season2DuelArt(20.f);
        duelTile->setScale(tile / std::max(1.f, duelTile->getContentSize().height));
        auto* duelButton = CCMenuItemSpriteExtra::create(duelTile, this, menu_selector(RankedLayer::onSeason2Duel));
        duelButton->setPosition({first + pitch * 2.f, kS2SideY});
        m_s2RatingMenu->addChild(duelButton);
        duelButton->setID("season2-duel-button"_spr);
        // 닷지 버튼은 원형 버튼 안에 "스택/한도" 숫자를 넣는다. 눌러도 같은 DodgeInfoPopup이
        // 열린다.
        m_s2DodgeValue = CCLabelBMFont::create("0/3", "bigFont.fnt");
        m_s2DodgeCircle = CircleButtonSprite::create(m_s2DodgeValue, CircleBaseColor::Blue, CircleBaseSize::Medium);
        CCNode* dodgeTile = m_s2DodgeCircle;
        if (!dodgeTile) dodgeTile = m_s2DodgeValue;
        dodgeTile->setScale(tile / std::max(1.f, dodgeTile->getContentSize().height));
        auto* dodgeButton = CCMenuItemSpriteExtra::create(dodgeTile, this, menu_selector(RankedLayer::onDodgeInfo));
        dodgeButton->setPosition({first + pitch * 3.f, kS2SideY});
        m_s2RatingMenu->addChild(dodgeButton);
        dodgeButton->setID("season2-dodge-button"_spr);
        refreshSeason2Dodge();
        season2Corner();
        // 티어/레이팅 라벨은 숨겨 둔 채 값만 갱신에 쓰고, 실제 표시는 라운지의 뱃지와 전광판이
        // 맡는다.
        m_signals = CCNode::create();
        addChild(m_signals, 35);
        m_population = label(m_root, "", {width - 14.f, m_s2Floor + kS2RowH + kS2BoardH - 9.f}, .3f, width * .28f,
                             kMuted, false, "chatFont.fnt");
        m_population->setAnchorPoint({1.f, .5f});
        m_population->setZOrder(7);
        m_tier = label(m_root, "", {}, .5f, m_leftWidth);
        m_tier->setVisible(false);
        m_rating = label(m_root, "", {}, .42f, m_leftWidth);
        m_rating->setVisible(false);
        m_placement = label(m_root, "", {}, .26f, m_leftWidth);
        m_placement->setVisible(false);
        m_selfCard = CCNode::create();
        m_selfCard->setVisible(false);
        m_root->addChild(m_selfCard);
        refreshSelfCard();
        // 라운지(광장 3D 장면) 호스트를 만들고 입력/뒤로 가기/터치 판정 콜백을 연결한다.
        // 뱃지(티어, 레이팅, 닉네임, 칭호), 오라 장식, 군중(PlazaCrowd), 전광판, 이모트,
        // 카메라 버튼을 붙인다.
        m_s2Lounge = lounge::createHost({[self = Weak<RankedLayer>(this)] {
                                             if (auto owner = self.lock()) owner->keyBackClicked();
                                         },
                                         [self = Weak<RankedLayer>(this)] {
                                             auto owner = self.lock();
                                             return owner && owner->season2LoungeInput();
                                         },
                                         [self = Weak<RankedLayer>(this)] {
                                             auto owner = self.lock();
                                             return owner && !owner->m_transitioning;
                                         },
                                         [self = Weak<RankedLayer>(this)](float x, float y) {
                                             auto owner = self.lock();
                                             return owner && owner->season2TouchFree(x, y);
                                         }});
        if (m_s2Lounge) {
            addChild(m_s2Lounge, -8);
            m_s2Badge = CCNode::create();
            m_s2Badge->setID("season2-lounge-badge"_spr);
            m_s2BadgeFace = CCNode::create();
            m_s2Badge->addChild(m_s2BadgeFace, 1);
            m_s2BadgeTier = Season2TierText::create();
            m_s2Badge->addChild(m_s2BadgeTier);
            m_s2BadgeRating = label(m_s2Badge, "", {}, .26f, 100.f, ccWHITE, true, "bigFont.fnt");
            m_s2BadgeTier->setZOrder(2);
            m_s2BadgeRating->setZOrder(2);
            m_s2BadgeBubble = CCNode::create();
            m_s2BadgeBubble->setPosition({0.f, kS2BubbleY});
            m_s2Badge->addChild(m_s2BadgeBubble, 5);
            lounge::setBadge(m_s2Lounge, m_s2Badge);
            m_s2Decor = CCNode::create();
            m_s2Decor->setID("season2-lounge-aura"_spr);
            lounge::setDecor(m_s2Lounge, m_s2Decor);
            m_plaza = PlazaCrowd::create(m_s2Lounge);
            if (m_plaza) lounge::setCrowd(m_s2Lounge, m_plaza);
            m_s2Billboard = Season2Billboard::create();
            if (m_s2Billboard) lounge::setBillboard(m_s2Lounge, m_s2Billboard);
            buildSeason2Emotes();
            buildSeason2Camera();
        }
        // 현재 컵의 티어표를 받아 검색 보드와 전광판에 전달한다.
        Service::get().fetchSeason2Tiers(
            Service::get().rankedCup(),
            [self = Weak<RankedLayer>(this)](std::optional<Season2Tiers> tiers, std::string) {
                auto owner = self.lock();
                if (!owner || owner->m_transitioning || !tiers) return;
                owner->m_searchBoard->setSeason2Tiers(*tiers);
                if (owner->m_s2Billboard) owner->m_s2Billboard->setTiers(*tiers);
            });
    }
    // Season 2 우측 상단 버튼 막대: 설정 톱니 옆에 알림 버튼을 같은 크기로 맞춰 놓고 LIVE
    // 목록 버튼은 숨긴다.
    static constexpr float kS2BarTall = 18.f, kS2BarGap = 6.f;
    static void season2Fit(CCNode* node, float tall) {
        auto* item = typeinfo_cast<CCMenuItemSpriteExtra*>(node);
        float const height = node->getContentSize().height;
        if (!item || height <= 1.f) return;
        item->setScale(tall / height);
        item->m_baseScale = tall / height;
    }
    void season2Corner() {
        float const w = m_window.width, h = m_window.height;
        if (auto* gear = getChildByIDRecursive("versus-sound-settings"_spr))
            gear->setPosition({w - kS2Corner, h - kS2Corner});
        auto* gear = getChildByIDRecursive("versus-sound-settings"_spr);
        auto* bell = getChildByIDRecursive("versus-broadcast-inbox"_spr);
        auto* live = getChildByIDRecursive("versus-live-matches"_spr);
        float gearSize =
            gear ? std::max(gear->getContentSize().width, gear->getContentSize().height) * gear->getScale() : 0.f;
        if (gearSize <= 1.f) gearSize = 30.f;
        if (bell) {
            season2Fit(bell, gearSize);
            float const wide = bell->getContentSize().width * bell->getScale();
            bell->setPosition({w - kS2Corner - gearSize / 2.f - kS2BarGap - wide / 2.f, h - kS2Corner});
        }
        if (live) {
            live->setVisible(false);
            if (auto* item = typeinfo_cast<CCMenuItem*>(live)) item->setEnabled(false);
        }
    }
    // hideBtn 프레임이 없을 때 쓰는 카메라 아이콘을 직접 그린다.
    static CCNode* season2CameraArt(float size) {
        auto* holder = art::artHolder(size);
        auto* d = CCDrawNode::create();
        float const k = size / 20.f;
        auto at = [k](float x, float y) { return CCPoint{x * k, y * k}; };
        auto const body = art::f4(ccc3(236, 242, 252));
        art::shape(d, {at(6.f, 13.f), at(8.f, 16.6f), at(12.f, 16.6f), at(14.f, 13.f)}, body, .9f * k);
        art::shape(d, {at(2.f, 3.6f), at(18.f, 3.6f), at(18.f, 14.6f), at(2.f, 14.6f)}, body, .9f * k);
        art::disc(d, at(10.f, 9.f), 4.4f * k, art::f4(ccc3(42, 50, 74)), .7f * k);
        d->drawDot(at(10.f, 9.f), 2.7f * k, art::f4(ccc3(96, 176, 244)));
        d->drawDot(at(8.9f, 10.1f), .9f * k, art::f4(ccc3(255, 255, 255)));
        d->drawDot(at(15.6f, 12.4f), .9f * k, art::f4(ccc3(255, 196, 92)));
        holder->addChild(d);
        return holder;
    }
    // UI 숨김 상태(분홍)/표시 상태(초록)에 맞는 카메라 버튼 스프라이트.
    CCSprite* season2CameraSprite() const {
        auto const base = m_s2UiHidden ? CircleBaseColor::Pink : CircleBaseColor::Green;
        CCNode* top = loadedFrameSprite("hideBtn_001.png", "GJ_GameSheet03.plist");
        if (!top) top = season2CameraArt(20.f);
        CCSprite* sprite = CircleButtonSprite::create(top, base, CircleBaseSize::Medium);
        if (sprite) {
            auto const size = sprite->getContentSize();
            float const wide = std::max({1.f, top->getContentSize().width, top->getContentSize().height});
            top->setScale(std::min(size.width, size.height) * .62f / wide);
            top->setPosition(size / 2.f);
        }
        if (!sprite) {
            sprite = CCSprite::create();
            sprite->setContentSize({20.f, 20.f});
            auto* art = season2CameraArt(20.f);
            art->setPosition({10.f, 10.f});
            sprite->addChild(art);
        }
        sprite->setScale((kS2EmoteTile + 2.f) / std::max(1.f, sprite->getContentSize().height));
        return sprite;
    }
    void buildSeason2Camera() {
        m_s2CameraMenu = menu(this);
        m_s2CameraMenu->setZOrder(31);
        m_s2CameraMenu->setID("season2-camera-menu"_spr);
        m_s2Camera =
            CCMenuItemSpriteExtra::create(season2CameraSprite(), this, menu_selector(RankedLayer::onSeason2Camera));
        m_s2Camera->setPosition({m_window.width / 2.f + m_s2HideX, kS2EmoteY});
        m_s2Camera->setID("season2-camera"_spr);
        m_s2CameraMenu->addChild(m_s2Camera);
    }
    void onSeason2Camera(CCObject*) { season2HideUi(!m_s2UiHidden); }
    // UI 숨기기 토글: 숨길 때 보이던 자식 노드를 m_s2Veiled에 기억해 두었다가 다시 보여
    // 준다.
    void season2HideUi(bool hidden) {
        if (hidden == m_s2UiHidden) return;
        m_s2UiHidden = hidden;
        if (!hidden) {
            for (auto& node : m_s2Veiled)
                if (node && node->getParent() == this) node->setVisible(true);
            m_s2Veiled.clear();
        }
        if (m_s2Camera) m_s2Camera->setSprite(season2CameraSprite());
        season2Veil();
    }
    // 카메라 버튼은 라운지만 보일 때(매치/결과/연출/밴/봇 대전이 아닐 때)에만 표시한다.
    bool season2CameraShown() const {
        return m_s2Lounge && !m_transitioning && !(m_state && m_state->match) && !m_lastResult && !m_s2Intro &&
               !m_s2Ban && !m_botActive && m_s2Expanded != kS2Match && m_s2Expanded != kS2Result &&
               m_s2Expanded != kS2Ban;
    }
    // UI 숨김 중이면 카메라 버튼, 이모트, 라운지 등을 제외한 나머지 자식 노드를 가린다.
    void season2Veil() {
        if (!m_s2CameraMenu) return;
        bool const shown = season2CameraShown();
        if (m_s2UiHidden && !shown) season2HideUi(false);
        m_s2CameraMenu->setVisible(shown);
        m_s2CameraMenu->setEnabled(shown);
        if (!m_s2UiHidden) return;
        for (auto* child : CCArrayExt<CCNode*>(getChildren())) {
            if (child == m_s2CameraMenu || child == m_s2Emotes || child == m_s2Lounge || child == m_s2Arrive ||
                child->getZOrder() <= -7 || !child->isVisible())
                continue;
            child->setVisible(false);
            m_s2Veiled.emplace_back(child);
        }
    }
    // 버튼 아래에 붙는 작은 캡션 라벨.
    static void season2Caption(CCNode* parent, char const* text, float x, float y = kS2CaptionY) {
        auto* caption = CCLabelBMFont::create(text, "bigFont.fnt");
        caption->setPosition({x, y});
        caption->limitLabelWidth(31.f, .22f, .12f);
        caption->setID(fmt::format("season2-caption-{}", lower(text)));
        parent->addChild(caption);
    }
    // Season 2 패널의 접힘/펼침 애니메이션을 한 프레임 진행한다.
    // 보드(대기) / 매치 / 결과 / 밴 모드에 따라 패널 높이, 위치, 색(밴드 색 혼합), 클리핑
    // 영역, 투명도, 전체 스케일을
    // 목표 값으로 부드럽게 보간한다. 처음 상태를 받기 전에는 보간 없이 바로 맞춘다.
    void season2Fold(float dt) {
        if (!m_season2 || !m_s2Rows || !m_s2Compact || !m_s2BoardClip) return;
        bool const board = m_s2Expanded == kS2Board || m_s2Expanded < 0;
        bool const searching = m_state && m_state->queuedAt > 0 && !m_state->match && !m_lastResult;
        bool const notice = board && m_state && m_status->getString() && *m_status->getString();
        float const want = board && !searching && !notice ? 0.f : 1.f, pad = board ? kS2CaptionPad : 0.f;
        if (board && want > 0.f)
            m_s2BoardOpen = true;
        else if (!board)
            m_s2BoardOpen = false;
        bool const snap = !m_s2Folded || !m_state;
        float const wideWant = board ? 0.f : want;
        if (snap) {
            m_s2Folded = m_state.has_value();
            m_s2Fold = want;
            m_s2Pad = pad;
            m_s2Wide = wideWant;
        }
        float const step = std::clamp(dt, 0.f, .1f) / kS2FoldTime;
        m_s2Fold = want > m_s2Fold ? std::min(want, m_s2Fold + step) : std::max(want, m_s2Fold - step);
        m_s2Wide = wideWant > m_s2Wide ? std::min(wideWant, m_s2Wide + step) : std::max(wideWant, m_s2Wide - step);
        m_s2Pad = pad > m_s2Pad ? std::min(pad, m_s2Pad + kS2CaptionPad * step)
                                : std::max(pad, m_s2Pad - kS2CaptionPad * step);
        float const e = m_s2Fold * m_s2Fold * (3.f - 2.f * m_s2Fold);
        float const width = m_root->getContentSize().width, top = m_s2Top;
        float const liftWant = m_s2Expanded == kS2Result ? kS2ResultCut : 0.f;
        float const trimWant = m_s2Expanded == kS2Match ? kS2MatchCut : 0.f;
        if (snap) {
            m_s2Lift = liftWant;
            m_s2Trim = trimWant;
        }
        m_s2Lift = liftWant > m_s2Lift ? std::min(liftWant, m_s2Lift + kS2ResultCut * step)
                                       : std::max(liftWant, m_s2Lift - kS2ResultCut * step);
        m_s2Trim = trimWant > m_s2Trim ? std::min(trimWant, m_s2Trim + kS2MatchCut * step)
                                       : std::max(trimWant, m_s2Trim - kS2MatchCut * step);
        float const banWant = m_s2Expanded == kS2Ban ? 1.f : 0.f;
        float const banStep = std::clamp(dt, 0.f, .1f) / kS2BanTime;
        m_s2BanGrow =
            banWant > m_s2BanGrow ? std::min(banWant, m_s2BanGrow + banStep) : std::max(banWant, m_s2BanGrow - banStep);
        float const banE = m_s2BanGrow * m_s2BanGrow * (3.f - 2.f * m_s2BanGrow);
        float const banBody =
            std::max(kS2CrownMargin + 4.f + s2ban::kBoardH + kS2RowH, m_window.height * kS2BanShare - kS2CaptionPad);
        float const banExtra = std::max(0.f, banBody - (kS2PanelH + kS2CrownMargin)) * banE;
        float const body = kS2RowH + kS2Gap + (kS2PanelH + kS2CrownMargin - kS2RowH - kS2Gap) * e + banExtra;
        float const rows = top - body + m_s2Lift;
        float const floor = rows - m_s2Pad + std::min(m_s2Trim, std::max(0.f, body - m_s2Lift - kS2RowH));
        float const frame = top - floor;
        m_s2Compact->setPosition({0.f, floor});
        m_s2Compact->setContentSize({width, frame});
        float const tint = m_s2Expanded == kS2Match || m_s2Expanded == kS2Ban ? 1.f : 0.f;
        m_s2Tint = tint > m_s2Tint ? std::min(tint, m_s2Tint + step) : std::max(tint, m_s2Tint - step);
        if (auto* back = m_s2Compact->getChildByType<NineSlice*>(0)) {
            back->setContentSize({width, frame});
            back->setPosition({width / 2.f, frame / 2.f});
            back->setColor(mixColor(m_s2PanelColour, season2Plate(.5f), m_s2Tint));
        }
        if (m_s2Crown) m_s2Crown->setVisible(m_s2Compact->isVisible());
        if (m_s2Border) {
            m_s2Border->setVisible(m_s2Compact->isVisible());
            m_s2Border->setContentSize({width + 2.f * kS2PanelBorder, frame + 2.f * kS2PanelBorder});
            m_s2Border->setPosition({width / 2.f, floor + frame / 2.f});
        }
        season2Focus(dt, floor);
        m_s2Rows->setPositionY(rows);
        m_preview->setPositionY(rows + 2.f);
        float const boardTop = m_s2Floor + kS2RowH + kS2BoardH, low = rows + kS2RowH;
        float const shown = std::max(0.f, boardTop - low);
        m_s2BoardClip->setVisible(shown > .5f && m_s2BoardOpen);
        m_s2BoardStencil->setPosition({11.f, low});
        m_s2BoardStencil->setContentSize({m_searchBoard->getContentSize().width, shown});
        auto const fade = static_cast<GLubyte>(255.f * std::clamp((e - .45f) / .55f, 0.f, 1.f));
        if (board) {
            m_status->setPositionY(top - kS2CrownMargin - 15.f - rows);
            m_status->setOpacity(fade);
        } else
            m_status->setOpacity(255);
        m_population->setOpacity(fade);
        if (m_s2RatingCaptions) m_s2RatingCaptions->setVisible(m_s2RatingMenu && m_s2RatingMenu->isVisible());
        if (m_s2ToolCaptions) m_s2ToolCaptions->setVisible(m_tools && m_tools->isVisible());
        float const wide = m_s2Wide * m_s2Wide * (3.f - 2.f * m_s2Wide);
        float const scale = kS2IdleScale + (1.f - kS2IdleScale) * std::max(wide, banE);
        m_root->setScale(scale);
        m_root->setPosition(m_s2RootAt + CCPoint{width / 2.f, top} * (1.f - scale));
    }
    // 라운지 뱃지를 갱신한다: 티어 얼굴, 티어명/레이팅, 닉네임, 칭호, 멤버십 오라.
    // 외형(avatarKey)이 바뀐 경우에만 닉네임/칭호/오라를 다시 만든다.
    void season2Badge(std::string const& art, std::string const& name, std::string const& value) {
        if (!m_s2Badge) return;
        if (art != m_s2BadgeArt) {
            m_s2BadgeArt = art;
            m_s2BadgeFace->removeAllChildren();
            auto* face = season2Face(m_s2BadgeFace, art, {}, .55f);
            if (float const tall = std::max(face->getContentSize().width, face->getContentSize().height); tall > 1.f)
                face->setScale(kS2Face / tall);
        }
        auto const& me = Service::get().profile();
        if (auto look = avatarKey(me); look != m_s2BadgeLook) {
            m_s2BadgeLook = look;
            if (m_s2BadgeName) m_s2BadgeName->removeFromParent();
            if (m_s2BadgeTitle) m_s2BadgeTitle->removeFromParent();
            m_s2BadgeName = rankedNickname(m_s2Badge, me, 0, {0.f, kS2NameY}, .3f, 120.f);
            m_s2BadgeTitle = titleBadge(m_s2Badge, me.cosmetics.title, {0.f, kS2TitleY}, kS2TitleScale, 170.f);
            if (m_s2Decor) {
                m_s2Decor->removeAllChildren();
                addMembershipAura(m_s2Decor, me.membership, me.cosmetics);
            }
        }
        m_s2BadgeTier->set(name, art, .3f, 150.f, true);
        m_s2BadgeRating->setString(value.c_str());
        m_s2BadgeRating->limitLabelWidth(100.f, .26f, .14f);
        rankedGradient(m_s2BadgeRating, art);
        float const textW = std::max(m_s2BadgeTier->width(), m_s2BadgeRating->getScaledContentSize().width);
        float const gap = kS2Face + 3.f, total = gap + textW, left = -total / 2.f;
        m_s2BadgeFace->setPosition({left + kS2Face / 2.f, kS2TierY});
        m_s2BadgeTier->setPosition({left + gap, kS2TierY + (value.empty() ? 0.f : 5.f)});
        m_s2BadgeRating->setPosition({left + gap, kS2TierY - 6.f});
        m_s2BadgeRating->setVisible(!value.empty());
    }
    // 광장 하단의 이모트 줄(퀵 이모트, 더 보기, 채팅)을 다시 만들고 가운데 정렬한다.
    // 카메라(UI 숨김) 버튼 위치도 이 줄에 맞춰 정한다.
    void buildSeason2Emotes() {
        if (!m_s2Emotes) {
            m_s2Emotes = menu(this);
            m_s2Emotes->setID("season2-plaza-emotes"_spr);
            m_s2Emotes->setZOrder(3);
            m_s2Emotes->setPosition({m_window.width / 2.f, kS2EmoteY});
        }
        m_s2Emotes->removeAllChildrenWithCleanup(true);
        m_s2EmoteKinds = emotes::quickSet(Service::get().profile().membership);
        int const count = static_cast<int>(m_s2EmoteKinds.size());
        for (int index = 0; index < count; ++index) {
            auto* item = CCMenuItemSpriteExtra::create(emoteTile(m_s2EmoteKinds[index], kS2EmoteTile, ccc3(43, 22, 12)),
                                                       this, menu_selector(RankedLayer::onPlazaEmote));
            item->setTag(index);
            item->setPosition({(index - (count - 1) / 2.f) * kS2EmotePitch, 0.f});
            item->setID(fmt::format("season2-plaza-emote-{}", index));
            m_s2Emotes->addChild(item);
        }
        float const last = ((count - 1) / 2.f) * kS2EmotePitch;
        auto* sprite = CCSprite::create();
        sprite->setContentSize({kS2EmoteTile, kS2EmoteTile});
        auto* back = NineSlice::create("square02b_001.png");
        back->setContentSize({kS2EmoteTile, kS2EmoteTile});
        back->setColor(ccc3(43, 22, 12));
        back->setPosition({kS2EmoteTile / 2.f, kS2EmoteTile / 2.f});
        sprite->addChild(back);
        auto* dots = CCDrawNode::create();
        float const step = kS2EmoteTile * .34f, from = kS2EmoteTile / 2.f - step / 2.f;
        for (int i = 0; i < 4; ++i)
            dots->drawDot({from + (i % 2) * step, from + (i / 2) * step}, kS2EmoteTile * .11f, {1.f, .85f, .4f, 1.f});
        sprite->addChild(dots);
        auto* more = CCMenuItemSpriteExtra::create(sprite, this, menu_selector(RankedLayer::onPlazaEmotePicker));
        more->setPosition({last + kS2EmotePitch, 0.f});
        more->setID("season2-plaza-emote-more"_spr);
        m_s2Emotes->addChild(more);
        auto* chat = iconButton(m_s2Emotes, this, menu_selector(RankedLayer::onPlazaChat), "GJ_chatBtn_001.png",
                                {last + kS2EmotePitch * 2.f + 4.f, 0.f}, kS2EmoteTile + 2.f);
        chat->setID("season2-plaza-chat"_spr);
        float const hide = -((count - 1) / 2.f) * kS2EmotePitch - kS2EmotePitch - 4.f;
        float const left = hide - (kS2EmoteTile + 2.f) / 2.f;
        float const right = last + kS2EmotePitch * 2.f + 4.f + (kS2EmoteTile + 2.f) / 2.f;
        float const shift = -(left + right) / 2.f;
        for (auto* item : CCArrayExt<CCNode*>(m_s2Emotes->getChildren()))
            item->setPositionX(item->getPositionX() + shift);
        m_s2HideX = hide + shift;
        if (m_s2Camera) m_s2Camera->setPosition({m_window.width / 2.f + m_s2HideX, kS2EmoteY});
    }
    void onPlazaEmotePicker(CCObject*) {
        if (auto* popup = EmotePickerPopup::create(
                [self = Weak<RankedLayer>(this)](std::string kind) {
                    if (auto owner = self.lock()) owner->plazaSay(std::move(kind), {});
                },
                [self = Weak<RankedLayer>(this)] {
                    Loader::get()->queueInMainThread([self] {
                        if (auto owner = self.lock(); owner && owner->m_s2Emotes) owner->buildSeason2Emotes();
                    });
                }))
            popup->show();
    }
    // 광장에서 내 말풍선을 띄우고 군중 쪽에도 이모트/채팅을 전달한다. 1.25초 쿨다운을
    // 둔다.
    void plazaSay(std::string kind, std::string text) {
        if (!m_plaza || m_s2EmoteCooldown > 0.f) return;
        m_s2EmoteCooldown = 1.25f;
        plazaBubble(m_s2BadgeBubble, kind, text);
        m_plaza->emote(std::move(kind), std::move(text));
    }
    void onPlazaEmote(CCObject* sender) {
        int const tag = static_cast<CCNode*>(sender)->getTag();
        if (tag >= 0 && tag < static_cast<int>(m_s2EmoteKinds.size())) plazaSay(m_s2EmoteKinds[tag], {});
    }
    void onPlazaChat(CCObject*) {
        if (auto* popup = QuickChatPopup::create([self = Weak<RankedLayer>(this)](std::string text) {
                if (auto owner = self.lock()) owner->plazaSay("chat", std::move(text));
            }))
            popup->show();
    }
    // 라운지(3D 장면)가 터치를 받아도 되는 좌표인지 판정한다. UI 버튼, 패널, 이모트 줄
    // 위이면 false,
    // 빈 곳이면 광장 캐릭터 탭 처리로 넘긴다.
    bool season2TouchFree(float x, float y) {
        if (m_transitioning || m_s2Expanded == kS2Intro) return false;
        auto inside = [x, y](CCNode* node, float pad) {
            if (!node || !node->isVisible() || !node->getParent()) return false;
            auto const box = node->boundingBox();
            auto const a = node->getParent()->convertToWorldSpace(box.origin);
            auto const b =
                node->getParent()->convertToWorldSpace(box.origin + CCPoint{box.size.width, box.size.height});
            return x >= std::min(a.x, b.x) - pad && x <= std::max(a.x, b.x) + pad && y >= std::min(a.y, b.y) - pad &&
                   y <= std::max(a.y, b.y) + pad;
        };
        if (m_s2CameraMenu && m_s2CameraMenu->isVisible() && inside(m_s2Camera, 3.f)) return false;
        if (m_s2Emotes && m_s2Emotes->isVisible())
            for (auto* item : CCArrayExt<CCNode*>(m_s2Emotes->getChildren()))
                if (inside(item, 3.f)) return false;
        if (m_s2UiHidden) return season2WalkerTap(x, y);
        if (y > m_window.height - 48.f) return false;
        if (inside(m_s2Compact, 2.f)) return false;
        if (m_s2Expanded == kS2Match && m_s2Rows && m_root) {
            auto const a = m_root->convertToWorldSpace({0.f, m_s2Rows->getPositionY()});
            auto const b =
                m_root->convertToWorldSpace({m_root->getContentSize().width, m_s2Rows->getPositionY() + kS2RowH});
            if (x >= a.x && x <= b.x && y >= a.y && y <= b.y) return false;
        }
        if (inside(m_back, 3.f)) return false;
        if (m_s2Emotes && m_s2Emotes->isVisible())
            for (auto* item : CCArrayExt<CCNode*>(m_s2Emotes->getChildren()))
                if (inside(item, 3.f)) return false;
        return season2WalkerTap(x, y);
    }
    // 광장에서 다른 플레이어를 탭했으면 프로필 팝업을 다음 프레임에 연다.
    bool season2WalkerTap(float x, float y) {
        if (m_plaza && season2LoungeInput() && !lounge::onControls(m_s2Lounge, {x, y}))
            if (auto profile = m_plaza->walkerAt({x, y})) {
                Loader::get()->queueInMainThread([self = Weak<RankedLayer>(this), profile = std::move(*profile)] {
                    if (auto owner = self.lock(); owner && !owner->m_transitioning && owner->isRunning())
                        if (auto* popup = PlayerPopup::create(profile)) popup->show();
                });
                return false;
            }
        return true;
    }
    bool season2PlazaOpen() {
        lounge::Pose pose;
        return m_s2Lounge && !m_transitioning && lounge::pose(m_s2Lounge, pose);
    }
    // 라운지가 입력을 받을 수 있는 상태인지: 전환/연출/미니게임/도착 덮개/알림창 등이
    // 가리고 있으면 false.
    bool season2LoungeInput() {
        if (m_transitioning || m_s2Expanded == kS2Intro || m_s2Intro || m_queueRunner || m_queueGame) return false;
        if (m_s2Arrive) return false;
        if (auto cover = m_s2Return.lock(); cover && cover->covering()) return false;
        auto covered = [](CCNode* parent) {
            if (!parent || !parent->getChildren()) return false;
            for (auto* child : CCArrayExt<CCNode*>(parent->getChildren()))
                if (child && child->isVisible() && typeinfo_cast<FLAlertLayer*>(child)) return true;
            return false;
        };
        return !covered(CCDirector::sharedDirector()->getRunningScene()) && !covered(this) &&
               !covered(OverlayManager::get());
    }
    // 모드(보드/매치/결과/연출/밴)에 맞춰 Season 2 버튼과 위젯의 위치와 표시를 정리한다.
    void season2Arrange(int mode) {
        if (!m_s2Rows || !m_action) return;
        float const width = m_root->getContentSize().width, center = width / 2.f;
        season2FitAction(mode == kS2Board);
        m_action->setPositionY(mode == kS2Board ? kS2ActionY : kS2RowY);
        if (m_runnerButton) m_runnerButton->setPositionY(mode == kS2Board ? kS2ActionY : kS2RowY);
        m_preview->setVisible(mode == kS2Match || mode == kS2Result);
        if (mode == kS2Intro) return;
        if (mode == kS2Ban) {
            m_status->setVisible(false);
            m_detail->setVisible(false);
            if (m_downloadRow) m_downloadRow->setVisible(false);
            m_action->setPositionX(m_root->getContentSize().width / 2.f);
            return;
        }
        // 결과 모드: 무승부 재대결을 기다리는 동안에만 상태 문구를 보여 준다.
        if (mode == kS2Result) {
            bool const rematch = season2DrawWaiting();
            m_status->setVisible(rematch);
            if (rematch) {
                m_status->setString(m_s2DrawShown < kS2DrawHold ? "DRAW - REMATCH" : "WAITING FOR THE REMATCH...");
                m_status->setPosition({center, kS2RowY});
                m_status->limitLabelWidth(width * .5f, .4f, .2f);
            }
            m_detail->setVisible(false);
            return;
        }
        // 매치 모드: 상태와 상세 문구를 한 줄로 합쳐 아래쪽에 표시한다.
        if (mode == kS2Match) {
            if (m_status->isVisible()) {
                std::string line = m_status->getString();
                std::string const detail = m_detail->getString();
                if (!line.empty() && !detail.empty()) line += "  -  " + detail;
                m_status->setString(line.c_str());
                m_status->setPosition({center, kS2MatchCut + 5.f});
                m_status->limitLabelWidth(width - 40.f, .3f, .2f);
            }
            m_detail->setVisible(false);
            return;
        }
        // 보드 모드: 메인 버튼을 가운데에 두고 러너/닷지 배지/도구 버튼을 그 주변에 배치한다.
        auto* image = m_action->getNormalImage();
        float const buttonWidth = image ? image->getScaledContentSize().width : 110.f;
        m_action->setPositionX(center);
        if (m_runnerButton) m_runnerButton->setPositionX(center + buttonWidth / 2.f + 24.f);
        if (m_dodgeBadge && m_dodgeBadge->isVisible()) {
            float const after = m_runnerButton && m_runnerButton->isVisible() ? m_runnerButton->getPositionX() + 15.f
                                                                              : center + buttonWidth / 2.f;
            m_dodgeBadge->setPosition({after + kDodgeGap + kDodgeWidth / 2.f, kS2SideY});
        }
        if (m_tools) m_tools->setPosition({width - 12.f - kToolSize / 2.f - kToolPitch * kS2ToolSpan, kS2SideY});
        std::string line = m_status->getString();
        std::string const detail = m_detail->getString();
        if (!line.empty() && !detail.empty()) line += "  " + detail;
        m_status->setString(line.c_str());
        m_status->setPosition({center, m_s2Top - kS2CrownMargin - 15.f - m_s2Rows->getPositionY()});
        m_status->limitLabelWidth(width * .38f, .36f, .16f);
        m_detail->setVisible(false);
    }
    // 화면 모드(보드/매치/결과/연출/밴)가 바뀔 때 패널과 메뉴의 표시 여부를 한 번
    // 정리한다.
    void season2Layout(int mode) {
        if (!m_season2 || !m_s2Rows) return;
        if (mode == m_s2Expanded) return;
        m_s2Expanded = mode;
        bool const board = mode == kS2Board, shown = mode != kS2Intro;
        if (m_s2Ban) m_s2Ban->setVisible(mode == kS2Ban);
        m_s2Full->setVisible(false);
        m_s2Compact->setVisible(shown);
        m_s2Rows->setVisible(shown);
        if (auto* rows = m_s2Rows->getChildByType<CCMenu*>(0)) rows->setEnabled(shown);
        if (m_s2RatingMenu) {
            m_s2RatingMenu->setVisible(board);
            m_s2RatingMenu->setEnabled(board);
        }
        m_population->setVisible(board);
        if (!shown) m_searchBoard->setVisible(false);
        season2Bell(mode == kS2Board || mode == kS2Result);
    }
    // 매치가 잡힌 뒤 게임이 시작될 때까지(매치/밴/연출 화면) 알림 버튼을 숨기고,
    // 열려 있던 알림 창도 닫는다.
    void season2Bell(bool shown) {
        if (auto* bell = typeinfo_cast<CCMenuItem*>(getChildByIDRecursive("versus-broadcast-inbox"_spr))) {
            bell->setVisible(shown);
            bell->setEnabled(shown);
        }
        if (shown) return;
        if (auto* scene = CCDirector::sharedDirector()->getRunningScene())
            if (auto* popup = typeinfo_cast<::versus::DeferredPopup*>(
                    scene->getChildByIDRecursive("versus-broadcast-inbox-popup"_spr)))
                popup->closeFromCode();
    }
    // Season 2 맵 밴: banning 상태이고 후보가 3장 이상일 때 밴 보드를 만들고, 상태가
    // 바뀌면 갱신/정리한다.
    // 밴 중에는 후보 맵을 미리 내려받고(prefetch), 밴된 맵은 내려받기를 취소한다.
    void season2Bans(RankedMatch const& match) {
        bool const banning = match.state == "banning" && match.bans && match.candidates.size() >= 3 &&
                             match.candidates.size() <= static_cast<size_t>(s2ban::kMaxCards);
        if (m_s2Ban && (m_s2Ban->matchId() != match.id || (banning && !m_s2Ban->sameDraw(match)) ||
                        (!banning && !m_s2Ban->revealing() && match.state != "preparing")))
            season2EndBans();
        if (banning && !m_s2Ban && m_introTime <= 0.f && !m_s2Intro) {
            float const width = m_root->getContentSize().width - 22.f;
            m_s2Ban = Season2BanBoard::create(match, {width, s2ban::kBoardH}, season2Plate(.62f),
                                              [self = Weak<RankedLayer>(this)](int64_t level) {
                                                  if (auto owner = self.lock()) owner->season2Ban(level);
                                              });
            if (m_s2Ban) {
                m_s2Ban->setPosition({11.f, m_s2Top - kS2CrownMargin - 2.f - s2ban::kBoardH});
                m_root->addChild(m_s2Ban, 7);
                audio::fx("fx_draw", "playSound_01.ogg");
            }
        }
        if (m_s2Ban) {
            m_s2Ban->apply(match);
            if (match.state != "banning" && !m_s2Ban->revealing()) season2EndBans();
        }
        if (match.state == "banning" && match.bans) {
            std::string const draw = match.id + "|" + std::to_string(match.selectedAt);
            if (m_s2Prefetched != draw) {
                season2DropPrefetch();
                m_s2Prefetched = draw;
            }
            for (auto const& level : match.candidates) {
                bool const banned = level.id == match.bans->own || level.id == match.bans->opponent;
                if (banned && m_s2PrefetchIds.erase(level.id))
                    cancelMapPreparation(level.id);
                else if (!banned && m_s2PrefetchIds.insert(level.id).second)
                    ensureMapCached(level.id, [](bool, std::string) {}, rankedAudioNeed(), true);
            }
        } else
            season2DropPrefetch(match.selectedMap ? match.selectedMap->id : 0);
    }
    // 미리 받던 맵 중 keep을 제외한 나머지의 준비를 취소한다.
    void season2DropPrefetch(int64_t keep = 0) {
        for (auto const id : m_s2PrefetchIds)
            if (id != keep) cancelMapPreparation(id);
        m_s2PrefetchIds.clear();
        m_s2Prefetched.clear();
    }
    // 밴 보드를 제거한다 (다음 프레임에 부모에서 떼어 낸다).
    void season2EndBans() {
        if (m_s2Ban) {
            auto* board = m_s2Ban;
            m_s2Ban = nullptr;
            board->unscheduleUpdate();
            board->setVisible(false);
            Loader::get()->queueInMainThread([board = Ref<CCNode>(board)] { board->removeFromParent(); });
        }
        m_s2BanSending = false;
    }
    // 밴 모드일 때 화면 주변부를 어둡게 하는 포커스 연출. 패널 영역 밖을 그라데이션으로
    // 덮는다.
    void season2Focus(float dt, float bottom) {
        float const want = m_s2Expanded == kS2Ban ? 1.f : 0.f;
        float const step = std::clamp(dt, 0.f, .1f) / kS2FocusTime;
        m_s2FocusAmount =
            want > m_s2FocusAmount ? std::min(want, m_s2FocusAmount + step) : std::max(want, m_s2FocusAmount - step);
        if (!m_s2Focus) {
            if (m_s2FocusAmount <= 0.f) return;
            m_s2Focus = CCNode::create();
            m_s2Focus->setID("season2-ban-focus"_spr);
            addChild(m_s2Focus, 31);
            CCPoint const along[] = {{-1.f, 0.f}, {-1.f, 0.f}, {1.f, 0.f}, {1.f, 0.f}, {0.f, -1.f}, {0.f, -1.f}};
            for (size_t i = 0; i < m_s2FocusParts.size(); ++i) {
                m_s2FocusParts[i] = CCLayerGradient::create(ccc4(0, 0, 0, 0), ccc4(0, 0, 0, 0), along[i]);
                m_s2Focus->addChild(m_s2FocusParts[i]);
            }
        }
        m_s2Focus->setVisible(m_s2FocusAmount > 0.f);
        if (m_s2FocusAmount <= 0.f) return;
        float const w = m_window.width, h = m_window.height;
        auto const origin = m_root->getPosition();
        float const rootScale = m_root->getScale();
        float const x0 = std::clamp(origin.x, 0.f, w),
                    x1 = std::clamp(origin.x + m_root->getContentSize().width * rootScale, x0, w);
        float const y0 = std::clamp(origin.y + bottom * rootScale, 0.f, h);
        auto const nearAlpha = static_cast<GLubyte>(kS2FocusNear * m_s2FocusAmount);
        auto const farAlpha = static_cast<GLubyte>(kS2FocusFar * m_s2FocusAmount);
        auto side = [&](size_t first, float room, auto place) {
            float const soft = std::min(kS2FocusSoft, room * .5f);
            auto* inner = m_s2FocusParts[first];
            auto* outer = m_s2FocusParts[first + 1];
            place(inner, 0.f, soft);
            place(outer, soft, room - soft);
            inner->setStartOpacity(0);
            inner->setEndOpacity(nearAlpha);
            outer->setStartOpacity(nearAlpha);
            outer->setEndOpacity(farAlpha);
        };
        side(0, x0, [&](CCLayerGradient* part, float from, float size) {
            part->setPosition({x0 - from - size, 0.f});
            part->setContentSize({std::max(0.f, size), h});
        });
        side(2, w - x1, [&](CCLayerGradient* part, float from, float size) {
            part->setPosition({x1 + from, 0.f});
            part->setContentSize({std::max(0.f, size), h});
        });
        side(4, y0, [&](CCLayerGradient* part, float from, float size) {
            part->setPosition({0.f, y0 - from - size});
            part->setContentSize({w, std::max(0.f, size)});
        });
    }
    // 밴 선택을 서버에 보낸다. 실패하면 보드에 오류를 알리고 상태를 바로 다시 조회한다. 봇
    // 대전이면 로컬로 처리한다.
    void season2Ban(int64_t level) {
        if (m_botActive) {
            botBan(level);
            return;
        }
        if (m_s2BanSending || !m_state || !m_state->match || m_state->match->state != "banning" || m_transitioning)
            return;
        m_s2BanSending = true;
        Service::get().banSeason2Map(
            m_state->match->id, level,
            [self = Weak<RankedLayer>(this)](std::optional<RankedState> state, std::string detail) {
                auto owner = self.lock();
                if (!owner || owner->m_transitioning) return;
                owner->m_s2BanSending = false;
                if (state) {
                    owner->receive(std::move(*state));
                    return;
                }
                if (owner->m_s2Ban) owner->m_s2Ban->failed(detail);
                owner->m_pollTimer = ranked::STATE_POLL_NOW;
            });
    }
    // Season 2 매치 소개 연출을 시작한다. 이미 소개한 매치거나 라운지가 없으면 건너뛴다.
    // 재대결 라운드는 화면 깜빡임만 주고, 그 외에는 Season2Intro를 띄운다.
    void season2Intro(RankedMatch const& match) {
        if (!m_s2CupSwitch.empty()) {
            m_introTime = 0.f;
            return;
        }
        if (m_s2Intro) {
            m_s2Intro->abort();
            m_s2Intro = nullptr;
        }
        if (auto cover = m_s2Return.lock()) cover->skip();
        auto& introduced = s2match::introduced();
        if (!m_s2Lounge || introduced.count(match.id)) {
            m_introTime = 0.f;
            return;
        }
        introduced.insert(match.id);
        season2ClosePopups();
        if (match.rematchRound > 0) {
            lounge::stopEffects(m_s2Lounge);
            audio::restoreRankedMusic();
            m_introTime = 0.f;
            season2Blink();
            return;
        }
        lounge::stopEffects(m_s2Lounge);
        audio::restoreRankedMusic();
        m_s2Intro = Season2Intro::create(
            m_s2Lounge, match.opponent.player, s2match::sideOf(match),
            [self = Weak<RankedLayer>(this), id = match.id](bool placed) {
                auto owner = self.lock();
                if (!owner) return;
                owner->m_s2Intro = nullptr;
                owner->m_s2Room = placed ? id : std::string();
                if (owner->m_plaza) owner->m_plaza->nudge();
                owner->render();
            },
            match.duel ? "DUEL!" : "MATCH FOUND!");
        if (m_s2Intro)
            addChild(m_s2Intro, 150);
        else
            m_introTime = 0.f;
    }
    // 열려 있는 DeferredPopup을 모두 코드로 닫는다 (매치 연출 전에 화면을 정리).
    void season2ClosePopups() {
        std::vector<Ref<::versus::DeferredPopup>> open;
        auto collect = [&open](CCNode* parent) {
            if (!parent || !parent->getChildren()) return;
            for (auto* child : CCArrayExt<CCNode*>(parent->getChildren()))
                if (auto* popup = typeinfo_cast<::versus::DeferredPopup*>(child)) open.emplace_back(popup);
        };
        collect(CCDirector::sharedDirector()->getRunningScene());
        collect(this);
        collect(OverlayManager::get());
        for (auto& popup : open)
            if (popup->getParent()) popup->closeFromCode();
    }
    // 화면을 잠깐 검게 덮었다가 걷어 장면 전환처럼 보이게 한다.
    void season2Blink() {
        auto* dark = CCLayerColor::create(ccc4(0, 0, 0, 255));
        addChild(dark, 150);
        dark->runAction(
            CCSequence::create(CCDelayTime::create(.12f), CCFadeOut::create(.3f), CCRemoveSelf::create(), nullptr));
    }
    // 라운지 장면을 현재 매치에 맞춘다. 진행 중인 매치가 있으면 매치 방 위치로
    // 이동(teleport)하고
    // 방 색을 밴드 색으로 바꾸며, 매치가 끝났으면 광장으로 복귀(restart)시킨다.
    void season2Room() {
        if (!m_s2Lounge || !m_plaza || !m_state) return;
        auto const& current = m_state->match;
        if (current && (current->result || current->state == "canceled")) m_s2Ended.insert(current->id);
        RankedMatch const* live = current && !current->result && current->state != "canceled" && !m_lastResult &&
                                          !m_s2Ended.count(current->id)
                                      ? &*current
                                      : nullptr;
        m_plaza->setRoom(live ? live->opponent.player.uid : std::string());
        std::optional<std::pair<uint32_t, uint32_t>> colours;
        if (live && live->mapBand && (m_s2Room == live->id || (m_s2Intro && m_s2Intro->moved()))) {
            auto const [main, sub] = season2BandColours(*live->mapBand);
            auto pack = [](ccColor3B c) {
                return static_cast<uint32_t>(c.r) << 16 | static_cast<uint32_t>(c.g) << 8 | c.b;
            };
            colours = std::pair{pack(main), pack(sub)};
        }
        lounge::roomColors(m_s2Lounge, colours);
        int const builds = lounge::builds(m_s2Lounge);
        bool const rebuilt = m_s2Builds != 0 && builds != m_s2Builds;
        m_s2Builds = builds;
        if (m_s2Intro) return;
        if (rebuilt && live) m_s2Room.clear();
        if (live) {
            if (m_s2Room == live->id) return;
            lounge::Pose pose;
            CCPoint room;
            if (!lounge::pose(m_s2Lounge, pose) || !lounge::roomAnchor(m_s2Lounge, room)) return;
            if (!m_s2Room.empty()) season2Blink();
            lounge::teleport(m_s2Lounge, {room.x + s2match::sideOf(*live) * s2match::kRoomGap, room.y});
            lounge::spawnRoomGroup(m_s2Lounge);
            m_plaza->nudge();
            m_s2Room = live->id;
            return;
        }
        if (!m_s2Room.empty()) {
            season2Blink();
            lounge::restart(m_s2Lounge);
            m_plaza->nudge();
            m_s2Room.clear();
        }
    }
    void onSeason2Opponent(CCObject* sender) { onOpponentInfo(sender); }
    // 내 프로필 창을 연다 (광장 도구 줄과 매치 판넬의 Profile 버튼).
    void onSeason2Profile(CCObject*) { showPlayer(Service::get().profile()); }
    // Season 2 매치 미리보기 레이아웃의 다운로드 영역 크기 상수.
    static constexpr float kS2DownloadW = 124.f, kS2DownloadY = 25.f, kS2DownloadPlateH = 15.f;
    // Season 2 매치 미리보기를 만든다: 양쪽 다운로드 상태, 맵 라이프/길이/2인 모드 표시,
    // 맵 카드와 밴드 카드, LDM 토글, 상대 프로필 버튼.
    void rebuildSeason2Preview(RankedMatch const& match) {
        auto const size = m_preview->getContentSize();
        auto* actions = menu(m_preview);
        float const top = size.height - kCardHeight - 3.f;
        float const bottom = kS2MatchCut - 2.f;
        float const rowY = (top + bottom) / 2.f + 2.f;
        float const center = size.width / 2.f;
        float const blockW = std::clamp(center - 75.f, 70.f, kS2DownloadW);
        m_downloadWidth = blockW - 12.f;
        m_downloadBarH = 4.f;
        m_downloadTextScale = .3f;
        m_downloadTextWidth = blockW * .56f;
        m_downloadRow = CCNode::create();
        m_downloadRow->setID("ranked-download-row"_spr);
        m_preview->addChild(m_downloadRow);
        for (int side = 0; side < 2; ++side) {
            float const left = side ? size.width - blockW : 0.f, lineY = kS2DownloadY + 3.5f;
            auto* plate = NineSlice::create("square02b_001.png");
            plate->setContentSize({blockW, kS2DownloadPlateH});
            plate->setPosition({left + blockW / 2.f, kS2DownloadY});
            plate->setColor(ccBLACK);
            plate->setOpacity(150);
            m_downloadRow->addChild(plate, -1);
            label(m_downloadRow, side ? match.opponent.player.name : std::string("YOU"), {left + 6.f, lineY}, .22f,
                  blockW * .38f, side ? ccc3(150, 215, 255) : ccc3(255, 190, 110), true, "bigFont.fnt");
            auto* bar = CCDrawNode::create();
            CCPoint corners[] = {{0, 0}, {m_downloadWidth, 0}, {m_downloadWidth, m_downloadBarH}, {0, m_downloadBarH}};
            bar->drawPolygon(corners, 4, {.08f, .05f, .03f, 1}, .5f, {.55f, .4f, .28f, 1});
            bar->setPosition({left + 6.f, kS2DownloadY - 5.5f});
            m_downloadRow->addChild(bar);
            m_downloadFill[side] = CCDrawNode::create();
            bar->addChild(m_downloadFill[side]);
            m_downloadText[side] = label(m_downloadRow, "WAITING FOR MAP", {left + blockW - 6.f, lineY}, .3f,
                                         m_downloadTextWidth, kIce, false, "chatFont.fnt");
            m_downloadText[side]->setAnchorPoint({1.f, .5f});
        }
        CCSize const cardSize{124.f, 26.f};
        float const cardX = size.width - cardSize.width / 2.f - 4.f;
        m_opponentLdm = label(m_preview, "LDM: OFF", {cardX - cardSize.width / 2.f - 6.f, rowY}, .24f, 54.f, kIce);
        m_opponentLdm->setAnchorPoint({1.f, .5f});
        float const livesY = top - 9.f, lengthY = top - 24.f, twoPlayerY = bottom + 5.5f;
        m_livesX = center;
        m_lengthX = center;
        m_livesScale = .46f * 1.3f;
        m_lengthScale = .44f;
        m_heartWidth = 15.f * 1.3f;
        auto* lifeHeart = createLifeHeart(14.f * 1.3f);
        lifeHeart->setID("ranked-map-lives-heart"_spr);
        lifeHeart->setPosition({m_livesX - 8.f, livesY});
        lifeHeart->setVisible(false);
        m_preview->addChild(lifeHeart);
        auto* lives = label(m_preview, "", {m_livesX + 6.f, livesY}, m_livesScale, 40.f, ccWHITE, false, "bigFont.fnt");
        lives->setID("ranked-map-lives"_spr);
        lives->setVisible(false);
        if (auto* clock = CCSprite::createWithSpriteFrameName("GJ_timeIcon_001.png")) {
            clock->setScale(13.f / std::max(1.f, clock->getContentSize().height));
            clock->setPosition({m_lengthX - 14.f, lengthY});
            clock->setID("ranked-map-length-clock"_spr);
            m_preview->addChild(clock);
        }
        label(m_preview, "---", {m_lengthX + 6.f, lengthY}, m_lengthScale, 52.f, ccc3(255, 214, 120), false,
              "goldFont.fnt")
            ->setID("ranked-map-length"_spr);
        auto* twoPlayer = label(m_preview, "2-PLAYER MODE", {center, twoPlayerY}, .26f, size.width * .3f, kTwoPlayerRed,
                                false, "bigFont.fnt");
        twoPlayer->setID("ranked-map-two-player"_spr);
        twoPlayer->setVisible(false);
        m_twoPlayerScale = twoPlayer->getScale();
        {
            auto const text = twoPlayer->getScaledContentSize();
            auto* back = NineSlice::create("square02b_001.png");
            back->setContentSize({text.width + 12.f, text.height + 6.f});
            back->setColor(ccc3(34, 6, 12));
            back->setOpacity(225);
            back->setPosition(twoPlayer->getPosition());
            back->setID("ranked-map-two-player-back"_spr);
            back->setVisible(false);
            m_preview->addChild(back, twoPlayer->getZOrder() - 1);
        }
        updateMapLength(match);
        m_mapCard = nullptr;
        m_cardMapId = -1;
        refreshMapCard(match);
        m_bandCard = nullptr;
        m_bandCardKey.clear();
        refreshBandCard(match);
        // 왼쪽 아래: 내 프로필 버튼, 그 오른쪽에 LDM 체크박스.
        iconButton(actions, this, menu_selector(RankedLayer::onSeason2Profile), "GJ_profileButton_001.png",
                   {14.f, rowY}, 22.f)
            ->setID("season2-match-profile-button"_spr);
        label(m_preview, "Profile", {14.f, rowY - 15.f}, .2f, 30.f, ccWHITE, false, "bigFont.fnt");
        m_ldmToggle = CCMenuItemToggler::createWithStandardSprites(this, menu_selector(RankedLayer::onLdm), .45f);
        m_ldmToggle->setID("ranked-ldm-toggle"_spr);
        m_ldmToggle->setPosition({42.f, rowY});
        actions->addChild(m_ldmToggle);
        label(m_preview, "LDM", {64.f, rowY}, .25f, 30.f, kIce);
        auto* card = CCSprite::create();
        card->setContentSize(cardSize);
        auto* back = NineSlice::create("square02b_001.png");
        back->setContentSize(cardSize);
        back->setPosition(cardSize / 2.f);
        back->setColor(season2Plate(.78f));
        back->setOpacity(235);
        card->addChild(back);
        player(card, match.opponent.player, {14.f, cardSize.height / 2.f}, .5f);
        label(card, match.opponent.player.name, {28.f, cardSize.height / 2.f}, .3f, 74.f, ccWHITE, true, "bigFont.fnt");
        if (auto* info = loadedFrameSprite("GJ_infoIcon_001.png")) {
            info->setScale(15.f / std::max(1.f, info->getContentSize().height));
            info->setPosition({cardSize.width - 12.f, cardSize.height / 2.f});
            card->addChild(info);
        }
        auto* opponent = CCMenuItemSpriteExtra::create(card, this, menu_selector(RankedLayer::onSeason2Opponent));
        opponent->setID("season2-opponent-profile"_spr);
        opponent->setPosition({cardX, rowY});
        actions->addChild(opponent);
        updateLdmControls();
        updateDownloads();
    }
    // Season 2 결과 화면을 만든다. 레이팅 변화 숫자(m_delta)는 배치 중, 배치 완료, 패배
    // 감소, 듀얼일 때는 비운다.
    Weak<Season2ResultView> m_s2ResultView;
    void season2Result(RankedResult const& result, int placementRequired, std::optional<LevelInfo> const& map) {
        auto size = m_preview->getContentSize();
        size.height -= kS2ResultCut;
        auto* view = Season2ResultView::create(result, placementRequired, map, size);
        m_s2ResultView = view;
        if (!view) return;
        m_preview->addChild(view);
        bool const provisional =
            result.placementAfter < (placementRequired > 0 ? placementRequired : ranked::PLACEMENT_MATCH_COUNT);
        bool const shielded = result.result == "loss" && result.lossReduction > 0. && result.fullLoss < 0;
        m_delta->stopAllActions();
        m_delta->setOpacity(255);
        m_delta->setPosition(CCPoint{11.f, m_s2Floor + kS2ResultCut + 2.f} + view->deltaAt());
        m_delta->setScale(.45f);
        m_delta->setString(provisional || result.placementComplete || shielded || result.duel
                               ? ""
                               : fmt::format("{:+.0f}", result.newRating - result.oldRating).c_str());
        m_delta->setColor(result.newRating >= result.oldRating ? ccc3(120, 255, 175) : ccc3(255, 105, 95));
        rankedGradient(m_delta, result.newTier);
        m_delta->runAction(CCSequence::create(
            CCSpawn::create(CCMoveBy::create(2.f, {0.f, 5.f}),
                            CCSequence::create(CCDelayTime::create(1.2f), CCFadeOut::create(.8f), nullptr), nullptr),
            nullptr));
    }

public:
    // 씬 초기화. notice가 있으면 화면이 준비된 뒤 안내로 보여 준다.
    bool init(std::string notice) {
        m_season2 = Service::get().season2();
        if (!initScene("")) return false;
        addAudioButton(true);
        addLiveButton(m_window.width - 70.f);
        setID("ranked-scene"_spr);
        if (auto const& room = Service::get().room(); room && room->emote) m_seenSignal = room->emote->nonce;
        watchTitles();
        if (auto* background = getChildByID("versus-default-background"_spr)) background->removeFromParent();
        m_background = RankedBackground::create();
        addChild(m_background, -9);
        CCLabelBMFont* heading = nullptr;
        // Season 1 화면에서만 제목(RANKED MATCH S1)을 표시한다.
        if (!m_season2) {
            heading = label(this, std::string("RANKED MATCH S1"), {m_window.width / 2.f, m_window.height - 24.f}, .9f,
                            m_window.width - 100.f, ccWHITE, false, "gjFont46.fnt");
            heading->setID("ranked-heading"_spr);
            rankedTitleGradient(heading);
        }
        m_notice = std::move(notice);
        // Season 2는 buildSeason2가 전체 화면을 구성하고, 아래 else 블록은 Season 1
        // 레이아웃이다.
        // Season 1: 왼쪽 패널(티어/레이팅/배치 진행/내 카드/버튼 4개), 오른쪽 검색 보드와
        // 미리보기, 하단 액션 행.
        if (m_season2)
            buildSeason2(heading);
        else {
            float width = std::min(550.f, m_window.width - 40.f);
            m_height = std::min(260.f, m_window.height - 76.f);
            m_leftWidth = width * .32f;
            m_root = panel(this, {(m_window.width - width) / 2.f, 25.f}, {width, m_height}, false);
            if (auto* backdrop = m_root->getChildByType<NineSlice*>(0)) {
                backdrop->setColor(ccc3(112, 55, 22));
                backdrop->setOpacity(220);
                m_backdrop = backdrop;
            }
            m_framePanels.push_back(rankedPanel(m_root, {6.f, 65.f}, {m_leftWidth - 10.f, m_height - 71.f}));
            m_framePanels.push_back(rankedPanel(m_root, {6.f, 7.f}, {m_leftWidth - 10.f, 52.f}));
            m_tier = label(m_root, "INSANE", {m_leftWidth / 2.f, m_height - 84.f}, .58f, m_leftWidth - 18.f, ccWHITE,
                           false, "gjFont46.fnt");
            m_rating = label(m_root, "Rating ----", {m_leftWidth / 2.f, m_height - 107.f}, .53f, m_leftWidth - 16.f,
                             ccWHITE, false, "bigFont.fnt");
            m_tier->setZOrder(3);
            m_rating->setZOrder(3);
            m_placement = label(m_root, "", {m_leftWidth / 2.f, m_height - 126.f}, .27f, m_leftWidth - 16.f, kIce);
            m_delta = label(m_root, "", {m_leftWidth / 2.f, m_height - 122.f}, .58f, m_leftWidth - 15.f, ccWHITE, false,
                            "bigFont.fnt");
            m_delta->setZOrder(20);
            m_selfCard = CCNode::create();
            m_root->addChild(m_selfCard);
            refreshSelfCard();
            // 왼쪽 버튼 4개(Top, Tier, Life, Points)를 가로로 균등 배치하고 캡션을 단다.
            auto* actions = menu(m_root);
            float const pitch = std::min(36.f, (m_leftWidth - 44.f) / 3.f), tile = std::min(28.f, pitch - 3.f);
            auto slotX = [&](int slot) { return m_leftWidth / 2.f + pitch * (slot - 1.5f); };
            auto* leaderboardButton =
                iconButton(actions, this, menu_selector(RankedLayer::onLeaderboard),
                           "levelLeaderboard_globalBtn_001.png", {slotX(0), m_height - 157.f}, tile);
            leaderboardButton->setID("ranked-top-100-button"_spr);
            bool const demonFace = loadedFrame("diffIcon_05_btn_001.png", "GJ_GameSheet03.plist");
            CCNode* tiersTile =
                demonFace ? CircleButtonSprite::createWithSpriteFrameName("diffIcon_05_btn_001.png", 1.f,
                                                                          CircleBaseColor::Blue, CircleBaseSize::Medium)
                          : nullptr;
            if (!tiersTile) tiersTile = CCSprite::create();
            tiersTile->setScale(tile / std::max(1.f, tiersTile->getContentSize().height));
            auto* tiersButton = CCMenuItemSpriteExtra::create(tiersTile, this, menu_selector(RankedLayer::onTiers));
            tiersButton->setPosition({slotX(1), m_height - 157.f});
            actions->addChild(tiersButton);
            tiersButton->setID("ranked-tier-list-button"_spr);
            auto* heart = createLifeHeart(20.f);
            CCNode* livesTile = CircleButtonSprite::create(heart, CircleBaseColor::Pink, CircleBaseSize::Medium);
            if (!livesTile) livesTile = heart;
            livesTile->setScale(tile / std::max(1.f, livesTile->getContentSize().height));
            auto* livesButton = CCMenuItemSpriteExtra::create(livesTile, this, menu_selector(RankedLayer::onLives));
            livesButton->setPosition({slotX(2), m_height - 157.f});
            actions->addChild(livesButton);
            livesButton->setID("ranked-lives-button"_spr);
            auto* pointsText = CCLabelBMFont::create("+R", "bigFont.fnt");
            CCNode* pointsTile = CircleButtonSprite::create(pointsText, CircleBaseColor::Green, CircleBaseSize::Medium);
            if (!pointsTile) pointsTile = pointsText;
            pointsTile->setScale(tile / std::max(1.f, pointsTile->getContentSize().height));
            auto* pointsButton = CCMenuItemSpriteExtra::create(pointsTile, this, menu_selector(RankedLayer::onPoints));
            pointsButton->setPosition({slotX(3), m_height - 157.f});
            actions->addChild(pointsButton);
            pointsButton->setID("ranked-points-table-button"_spr);
            pointsButton->setVisible(!m_season2);
            for (int slot = 0; slot < 4; ++slot)
                season2Caption(m_root, kS1RatingNames[slot], slotX(slot), m_height - 157.f - tile / 2.f - 4.f);
            iconButton(actions, this, menu_selector(RankedLayer::onSelfInfo), "GJ_infoIcon_001.png",
                       {m_leftWidth - 15.f, 30.f}, 18.f);
            season2Caption(m_root, "Me", m_leftWidth - 15.f, 30.f - 9.f - 4.f);
            float boardWidth = width - m_leftWidth - 22.f;
            m_searchBoard = RankedSearchBoard::create({boardWidth, m_height - 119.f});
            m_searchBoard->setPosition({m_leftWidth + 11.f, 115.f});
            m_root->addChild(m_searchBoard);
            m_seasonWidth = boardWidth - 16.f;
            m_seasonLeft = label(m_root, "", {m_leftWidth + 11.f + boardWidth / 2.f, 106.f}, .28f, m_seasonWidth,
                                 ccc3(255, 205, 126));
            m_seasonLeft->setID("ranked-season-countdown"_spr);
            refreshSeason();
            m_searchBoard->onState(stateSink());
            // 로비 이모트와 하단 도구(History, Music, Info)와 별/오브 토글(기능이 켜진
            // 빌드에서만).
            m_boardActions = menu(m_root);
            m_boardActions->setContentSize({0.f, 0.f});
            m_toolsX = m_leftWidth + 11.f + boardWidth / 2.f;
            m_boardActions->setPosition({m_toolsX, kToolsCompactY});
            rebuildLobbyEmotes();
            m_tools = menu(m_root);
            m_tools->setContentSize({0.f, 0.f});
            iconButton(m_tools, this, menu_selector(RankedLayer::onHistory), "GJ_viewListsBtn_001.png", {0.f, 0.f},
                       kToolSize);
            auto* music = iconButton(m_tools, this, menu_selector(RankedLayer::onMusic), "GJ_audioOnBtn_001.png",
                                     {kToolPitch, 0.f}, kToolSize);
            music->setID("ranked-music-volume-button"_spr);
            iconButton(m_tools, this, menu_selector(RankedLayer::onGuide), "GJ_infoIcon_001.png",
                       {kToolPitch * 2.f, 0.f}, kToolSize);
            m_toolCaptions = CCNode::create();
            m_toolCaptions->setID("ranked-tool-captions"_spr);
            m_root->addChild(m_toolCaptions, 4);
            for (int slot = 0; slot < kS1Tools; ++slot)
                season2Caption(m_toolCaptions, kS1ToolNames[slot], kToolPitch * slot, -kToolSize / 2.f - 4.f);
            if (::versus::progress::kStarsOrbsEnabled) {
                m_rewardsItem = CCMenuItemSpriteExtra::create(rewardsSprite(rankedRewardsOn()), this,
                                                              menu_selector(RankedLayer::onRewards));
                m_rewardsItem->setPosition({-(kToolSize / 2.f + kRewardsGap + kRewardsWidth / 2.f), 0.f});
                m_rewardsItem->setID("ranked-rewards-toggle"_spr);
                m_tools->addChild(m_rewardsItem);
            }
            // 매치 미리보기 영역과 신호(말풍선) 레이어, 상태/상세 문구, 메인/확인/러너/닷지/리롤
            // 버튼.
            m_preview = CCNode::create();
            m_preview->setContentSize({boardWidth, m_height - 83.f});
            m_preview->setPosition({m_leftWidth + 11.f, 79.f});
            m_root->addChild(m_preview);
            m_signals = CCNode::create();
            addChild(m_signals, 35);
            float center = m_leftWidth + 11.f + boardWidth / 2.f;
            m_actionCenter = center;
            m_status = label(m_root, "CONNECTING...", {center, 55.f}, .43f, boardWidth - 12.f, ccc3(255, 199, 120));
            m_detail = label(m_root, "", {center, 40.f}, .35f, boardWidth - 12.f, kMuted, false, "chatFont.fnt");
            m_action =
                button(actions, this, menu_selector(RankedLayer::onMatch), "CONNECTING...", {center, 20.f}, .55f);
            m_confirm =
                button(actions, this, menu_selector(RankedLayer::onConfirm), "CONFIRM", {center + 75.f, 20.f}, .55f);
            m_confirm->setVisible(false);
            m_runnerButton = iconButton(actions, this, menu_selector(RankedLayer::onQueueRunner), "GJ_gpgBtn_001.png",
                                        {center + 78.f, 20.f}, 30.f);
            m_runnerButton->setVisible(false);
            makeDodgeBadge(actions, {center + 120.f, 20.f});
            m_voteButton = button(actions, this, menu_selector(RankedLayer::onVoteReroll), "REROLL",
                                  {center + 80.f, 20.f}, .55f, "GJ_button_06.png");
            m_voteButton->setID("ranked-reroll-vote"_spr);
            m_voteButton->setVisible(false);
            enabled(m_action, false);
            m_population =
                label(this, "", {m_window.width / 2.f, 10.f}, .32f, width - 30.f, kMuted, false, "chatFont.fnt");
        }
// 광장 봇 버튼은 데스크톱 빌드에서만 허용한다.
#ifdef GEODE_IS_DESKTOP
        constexpr bool kPlazaBot = true;
#else
        constexpr bool kPlazaBot = false;
#endif
        // 디버그 모드에서만 BOT / RETURN 버튼을 만든다 (평소에는 숨김).
        if (debug::active() && (!m_season2 || (kPlazaBot && m_s2Lounge))) {
            auto* top = menu(this);
            top->setZOrder(30);
            m_botButton = button(top, this, menu_selector(RankedLayer::onBot), "BOT", {74.f, m_window.height - 24.f},
                                 .5f, "GJ_button_05.png");
            m_botButton->setID("ranked-debug-bot-button"_spr);
            m_botButton->setVisible(false);
        }
        if (debug::active() && m_season2 && m_s2Lounge) {
            auto* top = menu(this);
            top->setZOrder(30);
            m_returnButton = button(top, this, menu_selector(RankedLayer::onReturn), "RETURN",
                                    {m_botButton ? 140.f : 74.f, m_window.height - 24.f}, .5f, "GJ_button_05.png");
            m_returnButton->setID("ranked-debug-return-button"_spr);
            m_returnButton->setVisible(false);
        }
        // 초기 티어 얼굴을 그리고 첫 상태 조회를 바로 예약한다. 라운지가 있으면 장면에 붙이고,
        // 전투에서 돌아온 경우 복귀 덮개를, 다른 컵에서 넘어온 경우 도착 덮개를 만든다.
        displayFace("Unranked");
        m_pollTimer = ranked::STATE_POLL_NOW;
        if (m_s2Lounge) {
            lounge::attachNow(m_s2Lounge);
            if (s2match::fromBattle())
                if (auto* cover = Season2Return::create(m_s2Lounge, season2ReturnLook())) {
                    addChild(cover, -7);
                    m_s2Return = cover;
                }
            if (!s2match::arriving().empty()) {
                m_s2ArriveMatch = s2match::arriving();
                m_s2Arrive = CCLayerColor::create(ccc4(0, 0, 0, 255));
                m_s2Arrive->setID("season2-arrive-cover"_spr);
                addChild(m_s2Arrive, 150);
            }
        }
        s2match::arriving().clear();
        return true;
    }
    // Ranked 로비 음악을 시작한다. Season 2 standard 컵은 전용 곡을 쓴다.
    void onEnter() override {
        SceneLayer::onEnter();
        audio::setCupSong(m_season2 && Service::get().rankedCup() == "standard" ? lounge::kStandardCupSong : 0);
        audio::startRankedMusic();
    }
    // 화면을 떠날 때 미니게임/듀얼 팝업/프리페치를 정리하고, 유지 대상이 아니면 로비
    // 음악을 멈춘다.
    void onExit() override {
        closeQueueRunner();
        closeDuelPopups();
        season2DropPrefetch(m_state && m_state->match && m_state->match->selectedMap ? m_state->match->selectedMap->id
                                                                                     : 0);
        if (!m_keepMusic) audio::stopRankedMusic(false);
        SceneLayer::onExit();
    }
    // 매 프레임 갱신. 시즌 남은 시간, 밴드 색 전환, 쿨다운, 광장, Season 2 접힘
    // 애니메이션, 봇, 무결성 검사(0.25초),
    // 다운로드 표시, 안내 팝업, 결과 레이팅 애니메이션, 카운트다운 효과음 등을 처리한다.
    void update(float dt) override {
        dt = std::clamp(dt, 0.f, .25f);
        if ((m_seasonTimer += dt) >= .25f) {
            m_seasonTimer = 0.f;
            refreshSeason();
        }
        if (m_bandBlend < 1.f) {
            m_bandBlend = std::min(1.f, m_bandBlend + std::min(dt, .1f) / .7f);
            paintBand(m_bandBlend);
        }
        m_signalCooldown = std::max(0.f, m_signalCooldown - dt);
        m_lobbyEmoteCooldown = std::max(0.f, m_lobbyEmoteCooldown - dt);
        if (m_plaza) {
            m_s2EmoteCooldown = std::max(0.f, m_s2EmoteCooldown - dt);
            bool const open = season2PlazaOpen() && !m_botActive;
            m_plaza->tick(dt, open);
            m_plaza->setHidden(m_s2Intro && !m_s2Intro->lit());
            bool const bar = open && !m_s2Intro;
            if (m_s2Emotes && m_s2Emotes->isVisible() != bar) {
                m_s2Emotes->setVisible(bar);
                m_s2Emotes->setEnabled(bar);
            }
            season2Room();
        }
        season2Arrive(dt);
        season2TryGo();
        if (!m_s2CupSwitch.empty() && !m_s2Peeking && m_s2CupGo.empty() && !m_s2Intro && !m_transitioning)
            m_s2CupSwitch.clear();
        season2Fold(dt);
        season2Veil();
        if (m_s2Ban && m_s2Ban->revealDone()) {
            season2EndBans();
            render();
        }
        updateBot(dt);
        m_integrityTimer += dt;
        if (m_integrityTimer >= .25f) {
            m_integrityTimer = 0.f;
            checkRankedIntegrity();
        }
        if (auto const& room = Service::get().room()) showRankedSignal(*room);
        m_progressTimer += dt;
        if (m_progressTimer >= .25f) {
            m_progressTimer = 0.f;
            updateDownloads();
        }
        showNoticeWhenReady();
        showDodgePenaltyWhenReady();
        if (!m_lastResult || m_resultTime >= ranked::RATING_ANIMATION_SECONDS) offerUpdate();
        m_ackRetry = std::max(0.f, m_ackRetry - dt);
        // Season 2 무승부 재대결 대기: kS2DrawHold 이후 보류된 상태를 반영하고, kS2DrawGiveUp
        // 이후에는 다시 그린다.
        if (m_s2DrawShown >= 0.f) {
            bool const waited = m_s2DrawShown >= kS2DrawGiveUp;
            m_s2DrawShown += std::clamp(dt, 0.f, .25f);
            if (m_s2HeldState && m_transitioning) m_s2HeldState.reset();
            if (m_s2HeldState && m_s2DrawShown >= kS2DrawHold) {
                auto held = std::move(*m_s2HeldState);
                m_s2HeldState.reset();
                receive(std::move(held));
            } else if (!waited && m_s2DrawShown >= kS2DrawGiveUp)
                render();
        }
        if (m_state && m_state->match &&
            (m_state->match->result || m_state->match->state == "canceled" || m_state->match->state == "rematch_wait"))
            acknowledge(m_state->match->id);
        else if (m_lastResult && !m_resultAcknowledged && m_lastResult->matchId == m_observedMatch)
            acknowledge(m_lastResult->matchId);  // 서버 상태에 매치가 없어도 결과 확인은 계속 다시 보낸다
        // 화면 전환을 시작했는데 3초가 지나도 이 화면이 그대로 실행 중이면(전환이 일어나지 않음)
        // 전환 중 표시를 풀어 버튼과 뒤로 가기가 다시 동작하게 한다.
        if (m_transitioning) {
            CCNode* root = this;
            while (root->getParent()) root = root->getParent();
            if (root == CCDirector::sharedDirector()->getRunningScene() && (m_transitionStuck += dt) >= 3.f) {
                log::warn("Versus: a screen change did not happen; the Ranked screen takes input again");
                m_transitioning = false;
                m_transitionStuck = 0.f;
                render();
            }
        } else
            m_transitionStuck = 0.f;
        if (m_state) m_searchBoard->refresh(*m_state, dt, m_stateRevision);
        if (m_introTime > 0.f) {
            m_introTime = std::max(0.f, m_introTime - dt);
            if (m_introTime <= 0.f && m_state && m_state->match) beginCandidates(*m_state->match);
        }
        // 결과 화면의 레이팅 숫자를 easing(ease-out cubic)으로 이전 값에서 새 값까지 움직인다.
        // 끝나면 티어가 바뀐 경우 티어 얼굴 전환 연출을 낸다.
        if (m_lastResult && m_resultTime < ranked::RATING_ANIMATION_SECONDS) {
            m_resultTime = std::min(ranked::RATING_ANIMATION_SECONDS, m_resultTime + dt);
            float t = m_resultTime / ranked::RATING_ANIMATION_SECONDS;
            t = 1.f - std::pow(1.f - t, 3.f);
            m_displayRating = m_lastResult->placementComplete
                                  ? m_lastResult->newRating
                                  : std::lerp(m_lastResult->oldRating, m_lastResult->newRating, t);
            if (m_resultTime >= ranked::RATING_ANIMATION_SECONDS && !m_tierAnimated) {
                m_tierAnimated = true;
                displayFace(m_lastResult->newTier, m_lastResult->newTier != m_lastResult->oldTier);
            }
            render();
        }
        // 준비 단계 카운트다운 숫자가 바뀔 때마다 효과음을 내고 다시 그린다. 0이 되면 곧바로
        // 상태를 조회한다.
        m_pollTimer += dt;
        m_readyTimer += dt;
        m_assetRetry = std::max(0.f, m_assetRetry - dt);
        requestReroll();
        if (m_state && m_state->match && !m_lastResult && m_state->match->state == "preparing" &&
            m_state->match->startAt > 0) {
            int const left = countdownLeft(*m_state->match);
            if (left != m_countdownShown) {
                if (left > 0)
                    audio::play(audio::Cue::Clock);
                else
                    m_pollTimer = ranked::STATE_POLL_NOW;
                m_countdownShown = left;
                render();
            }
        } else
            m_countdownShown = -1;
        if (m_state && (m_state->queuedAt > 0 || m_state->blockedUntil > 0) && m_progressTimer == 0.f) render();
        // 매치 중 LDM 설정을 서버에 맞추고(준비 전에만), 에셋이 준비되면 READY를 서버에
        // 알린다.
        // 준비 알림은 READY_RESEND_SECONDS 간격으로 다시 보낼 수 있고, 2인 플레이 금지/음악
        // 누락 여부도 함께 보낸다.
        if (m_state && m_state->match) {
            auto const match = *m_state->match;
            bool const desiredLdm = Mod::get()->getSavedValue<bool>("ranked-ldm", false);
            if (!m_botActive && (match.state == "map_search" || match.state == "preparing") && !match.ownReady &&
                !m_ldmSending && !m_readySending && match.ownLdm != desiredLdm && m_assetRetry <= 0.f) {
                m_ldmSending = true;
                Service::get().setRankedLdm(match.id, desiredLdm,
                                            [self = Weak<RankedLayer>(this),
                                             id = match.id](std::optional<RankedState> value, std::string detail) {
                                                if (auto owner = self.lock(); owner && !owner->m_transitioning) {
                                                    owner->m_ldmSending = false;
                                                    if (!owner->m_state || !owner->m_state->match ||
                                                        owner->m_state->match->id != id)
                                                        return;
                                                    if (value)
                                                        owner->receive(std::move(*value));
                                                    else {
                                                        owner->m_assetRetry = 2.f;
                                                        owner->m_detail->setString(detail.c_str());
                                                    }
                                                }
                                            });
            }
            updateLdmControls();
            prepareAssets(match);
            if (!m_botActive && match.state == "preparing" && m_assetsReady && !match.ownReady && !m_readySending &&
                !m_ldmSending && match.ownLdm == desiredLdm && m_readyTimer >= ranked::READY_RESEND_SECONDS) {
                m_readySending = true;
                m_readyTimer = 0.f;
                Service::get().rankedAssetsReady(
                    match.id, match.drawId, match.selectedMap ? mapPlaySeconds(match.selectedMap->id) : -1.,
                    [self = Weak<RankedLayer>(this)](std::optional<RankedState> value, std::string detail) {
                        if (auto owner = self.lock(); owner && !owner->m_transitioning) {
                            owner->m_readySending = false;
                            if (value)
                                owner->receive(std::move(*value));
                            else
                                owner->m_detail->setString(detail.c_str());
                        }
                    },
                    match.noTwoPlayer && match.selectedMap && mapTwoPlayer(match.selectedMap->id) == 1,
                    !match.drawId.empty() && m_songMissingDraw == match.drawId);
            }
        }
        // 상태 조회 주기가 되면 서버에서 상태를 가져온다.
        if (m_pollTimer >= statePollSeconds()) {
            m_pollTimer = 0.f;
            fetch();
        }
    }
    // 조회 주기: 맵 탐색/밴/준비/로딩 단계에서는 짧게, 그 외(대기)에는 길게.
    float statePollSeconds() const {
        if (m_state && m_state->match && !m_lastResult) {
            auto const& state = m_state->match->state;
            if (state == "map_search" || state == "banning" || state == "preparing" || state == "loading")
                return ranked::STATE_POLL_SECONDS;
        }
        return ranked::IDLE_STATE_POLL_SECONDS;
    }
    static RankedLayer* create(std::string notice) {
        auto* result = new RankedLayer;
        if (result->init(std::move(notice))) {
            result->autorelease();
            return result;
        }
        delete result;
        return nullptr;
    }
};

}

// 점검 메시지이면 점검 팝업을 열고 true를 반환한다.
bool showMaintenanceNotice(std::string const& message) {
    if (!isMaintenanceMessage(message)) return false;
    MaintenancePopup::open();
    return true;
}
// 서버 오류를 알린다. 점검 중이면 점검 팝업, 아니면 일반 경고창.
void alertServerError(std::string const& title, std::string const& message) {
    if (showMaintenanceNotice(message)) return;
    FLAlertLayer::create(title.c_str(), message.c_str(), "OK")->show();
}

// Ranked 룰렛 팝업을 연다.
void showRankedRoulette(RoomInfo const& room) {
    if (auto* popup = MapRoulettePopup::create(room)) popup->show();
}

// 각 화면용 씬 생성/전환 함수들: 로비, 방(그룹/일반), Ranked(Season 2에서 전투 복귀
// 여부 포함).
CCScene* lobbyScene(std::string notice) {
    auto* scene = CCScene::create();
    if (auto* layer = LobbyLayer::create(std::move(notice))) scene->addChild(layer);
    return scene;
}

CCScene* roomScene() {
    auto* scene = CCScene::create();
    if (Service::get().inGroup()) {
        if (auto* layer = GroupRoomLayer::create()) scene->addChild(layer);
    } else if (auto* layer = RoomLayer::create())
        scene->addChild(layer);
    return scene;
}

CCScene* rankedScene(std::string notice, bool fromBattle) {
    auto* scene = CCScene::create();
    s2match::fromBattle() = fromBattle;
    if (auto* layer = RankedLayer::create(std::move(notice))) scene->addChild(layer);
    s2match::fromBattle() = false;
    return scene;
}

// 로비로 페이드 전환한다.
void showLobby(std::string notice) {
    CCDirector::sharedDirector()->replaceScene(CCTransitionFade::create(.25f, lobbyScene(std::move(notice))));
}

// 그룹 대전 순위 팝업을 연다. 자유 플레이 방이 있으면 그 방을 관전하고,
// 그렇지 않으면 진행 중인 그룹 전투 순위를 보여 준다. 이미 열려 있으면 중복해서 열지
// 않는다.
void showGroupStandings() {
    if (auto const room = freeplay::liveRoom(); !room.empty()) {
        if (auto* scene = CCDirector::sharedDirector()->getRunningScene();
            scene && scene->getChildByID("versus-group-spectate"_spr))
            return;
        if (auto* popup = GroupSpectatePopup::watch(room, freeplay::liveRoomName())) popup->show();
        return;
    }
    auto const& group = Service::get().group();
    if (!group || !group->running()) return;
    if (auto* scene = CCDirector::sharedDirector()->getRunningScene();
        scene && scene->getChildByID("versus-group-spectate"_spr))
        return;
    if (auto* popup = GroupSpectatePopup::inBattle()) popup->show();
}

// 방으로 전환한다. 방이 없으면 로비로 보내고 보류 중인 안내를 함께 전달한다.
void showRoom() {
    if (!Service::get().room()) {
        showLobby(Service::get().takeNotice());
        return;
    }
    CCDirector::sharedDirector()->replaceScene(CCTransitionFade::create(.25f, roomScene()));
}
// Versus 진입 화면으로 전환한다.
void showVersus() {
    auto* scene = CCScene::create();
    if (auto* entry = VersusEntry::create()) scene->addChild(entry);
    CCDirector::sharedDirector()->replaceScene(CCTransitionFade::create(.25f, scene));
}

// Ranked / Season 2 화면으로 페이드 전환한다.
void showRanked(std::string notice) {
    CCDirector::sharedDirector()->replaceScene(CCTransitionFade::create(.25f, rankedScene(std::move(notice))));
}

void showSeason2() {
    auto* scene = CCScene::create();
    if (auto* layer = Season2Layer::create()) scene->addChild(layer);
    CCDirector::sharedDirector()->replaceScene(CCTransitionFade::create(.25f, scene));
}

// 칭호 뱃지 생성 함수를 외부에 공개하는 래퍼.
CCNode* createTitleBadge(CCNode* parent, int title, CCPoint position, float scale, float maxWidth, bool animated) {
    return titleBadge(parent, title, position, scale, maxWidth, animated);
}

}
