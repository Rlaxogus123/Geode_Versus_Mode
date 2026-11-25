// 방에서 맵을 고르기 위해 GD 기본 LevelSearchLayer / LevelBrowserLayer / LevelCell을 후킹한다.
// 선택 모드에서는 레벨 셀을 누르면 레벨을 방에 전달하고 방 화면으로 돌아가며,
// 방이 바뀌거나 시작되면 자동으로 빠져나온다. describeLevel도 여기서 제공한다.
#include "LevelSelector.hpp"
#include "MapPool.hpp"
#include "VersusUI.hpp"
#include "compat/GameHooks.hpp"

#include <Geode/Geode.hpp>
#include <Geode/binding/GJGameLevel.hpp>
#include <Geode/modify/LevelBrowserLayer.hpp>
#include <Geode/modify/LevelCell.hpp>
#include <Geode/modify/LevelSearchLayer.hpp>

#include <algorithm>
#include <utility>

using namespace geode::prelude;

namespace {
// 현재 선택 세션 상태. generation으로 이전 세션의 화면과 구분하고,
// stackDepth는 시작 시 장면 스택 깊이(돌아올 때 복원), pool이면 Map Pool 조건 모드.
struct PickerState {
    bool active = false;
    bool closing = false;
    int generation = 0;
    unsigned int stackDepth = 0;
    std::string roomID;
    std::function<void(versus::LevelInfo)> selected;
    bool pool = false;
    versus::MapPoolSettings conditions;
    std::function<void(versus::LevelInfo, bool)> poolSelected;
};

PickerState s_picker;

// 선택 세션을 시작한 방이 아직 같은 방인지.
bool sameRoom() {
    auto const& room = versus::Service::get().room();
    return room && room->id == s_picker.roomID;
}

// 이 노드가 현재 선택 세션의 화면에 속하는지. 부모를 거슬러 올라가며 마커(generation 태그)를 확인한다.
bool ownsNode(CCNode* node) {
    if (!s_picker.active) return false;
    for (; node; node = node->getParent()) {
        if (auto* marker = node->getChildByID("native-level-picker"_spr)) {
            return marker->getTag() == s_picker.generation;
        }
    }
    return false;
}

// 검색 화면들을 닫아 장면 스택을 시작 시 깊이로 되돌린다.
void unwindSearch(unsigned int stackDepth) {
    auto* director = CCDirector::sharedDirector();
    auto* stack = director->m_pobScenesStack;
    if (stackDepth > 0 && stack && stack->count() > stackDepth) {
        director->popToSceneStackLevel(static_cast<int>(stackDepth));
    }
}

// 선택(또는 취소)을 마치고 방 화면으로 돌아간다. 다음 프레임에 처리하며,
// 방이 사라졌으면 로비로, 그렇지 않으면 선택 결과를 콜백으로 넘긴 뒤 방을 연다.
// 일반 선택은 호스트이고 방이 시작 전일 때만, 풀 선택은 시작 전일 때만 전달한다.
void returnToRoom(std::optional<versus::LevelInfo> level = std::nullopt, bool twoPlayer = false) {
    if (!s_picker.active || s_picker.closing) return;
    s_picker.closing = true;
    auto generation = s_picker.generation;
    Loader::get()->queueInMainThread([generation, level = std::move(level), twoPlayer]() mutable {
        if (!s_picker.active || s_picker.generation != generation) return;
        auto roomID = s_picker.roomID;
        auto onSelected = std::move(s_picker.selected);
        auto onPool = std::move(s_picker.poolSelected);
        bool const pool = s_picker.pool;
        versus::cancelLevelSearch();

        auto& service = versus::Service::get();
        auto const& room = service.room();
        if (!room || room->id != roomID) {
            auto notice = service.takeNotice();
            versus::showLobby(notice.empty() ? "This room is no longer available." : notice);
            return;
        }

        if (level && pool && !room->started && onPool)
            onPool(std::move(*level), twoPlayer);
        else if (level && !pool && service.isHost() && !room->started && onSelected) {
            onSelected(std::move(*level));
        }
        versus::showRoom();
    });
}

// 선택 화면에 붙어 0.25초마다 방 상태를 확인하는 감시 노드. 방이 바뀌었거나 권한을 잃었거나
// 시작되었으면 방으로 돌려보낸다.
class PickerWatch final : public CCNode {
public:
    static PickerWatch* create() {
        auto* watch = new PickerWatch;
        if (!watch->init()) {
            delete watch;
            return nullptr;
        }
        watch->autorelease();
        watch->setID("native-level-picker"_spr);
        watch->setTag(s_picker.generation);
        watch->schedule(schedule_selector(PickerWatch::checkRoom), .25f);
        return watch;
    }

    void checkRoom(float) {
        if (!s_picker.active || getTag() != s_picker.generation) return;
        CCNode* scene = this;
        while (scene->getParent()) scene = scene->getParent();
        if (scene != CCDirector::sharedDirector()->getRunningScene()) return;
        if (!sameRoom() || (!s_picker.pool && !versus::Service::get().isHost()) ||
            versus::Service::get().room()->started) {
            returnToRoom();
        }
    }
};

// 검색/브라우저 화면에 감시 노드를 붙이고, 풀 모드면 하단에 풀 조건 문구를 표시한다.
void markPicker(CCNode* layer) {
    if (!s_picker.active) return;
    if (auto* watch = PickerWatch::create()) layer->addChild(watch);
    if (!s_picker.pool) return;
    auto const window = CCDirector::sharedDirector()->getWinSize();
    auto* text =
        CCLabelBMFont::create(("Map Pool: " + versus::poolConditionText(s_picker.conditions)).c_str(), "chatFont.fnt");
    text->setID("map-pool-conditions"_spr);
    text->setColor(ccc3(255, 225, 120));
    text->limitLabelWidth(window.width - 160.f, .6f, .3f);
    auto* plate = CCScale9Sprite::create("square02_small.png");
    plate->setOpacity(150);
    plate->setColor(ccBLACK);
    plate->setContentSize({text->getScaledContentSize().width + 16.f, text->getScaledContentSize().height + 8.f});
    plate->setPosition({window.width / 2.f, 16.f});
    plate->setID("map-pool-conditions-plate"_spr);
    text->setPosition({window.width / 2.f, 16.f});
    layer->addChild(plate, 100);
    layer->addChild(text, 101);
}

}
namespace versus {
// GJGameLevel을 LevelInfo로 변환한다. 피처 상태는 0 없음/1 Featured/2 Epic/3 Legendary/4 Mythic,
// 난이도는 오토, 데몬 종류, 일반 난이도(0이면 N/A)로 구분한다.
LevelInfo describeLevel(GJGameLevel* level) {
    versus::LevelInfo info;
    info.id = level->m_levelID.value();
    info.name = std::string(level->m_levelName);
    info.creator = std::string(level->m_creatorName);
    if (info.creator.size() > 32) info.creator.resize(32);
    info.stars = level->m_stars.value();
    info.featureState = level->m_isEpic >= 3     ? 4
                        : level->m_isEpic == 2   ? 3
                        : level->m_isEpic == 1   ? 2
                        : level->m_featured >= 1 ? 1
                                                 : 0;
    info.userCoins = std::clamp(level->m_coins, 0, 3);
    info.length = std::clamp(level->m_levelLength, 0, 5);
    info.demon = level->m_demon.value() != 0;
    info.autoLevel = level->m_autoLevel;
    info.platformer = level->isPlatformer();
    if (info.autoLevel) {
        info.difficulty = static_cast<int>(GJDifficulty::Auto);
    } else if (info.demon) {
        switch (level->m_demonDifficulty) {
        case 3: info.difficulty = static_cast<int>(GJDifficulty::DemonEasy); break;
        case 4: info.difficulty = static_cast<int>(GJDifficulty::DemonMedium); break;
        case 5: info.difficulty = static_cast<int>(GJDifficulty::DemonInsane); break;
        case 6: info.difficulty = static_cast<int>(GJDifficulty::DemonExtreme); break;
        default: info.difficulty = static_cast<int>(GJDifficulty::Demon); break;
        }
    } else {
        int difficulty = std::clamp(level->getAverageDifficulty(), 0, 5);
        info.difficulty = difficulty == 0 ? static_cast<int>(GJDifficulty::NA) : difficulty;
    }
    return info;
}
}

namespace versus {
// 선택 모드가 켜져 있는지.
bool isLevelSearchActive() {
    return s_picker.active;
}

// 선택 모드를 해제하고 상태를 비운 뒤 장면 스택을 되돌린다.
void cancelLevelSearch() {
    if (!s_picker.active) return;
    auto depth = s_picker.stackDepth;
    s_picker.active = false;
    s_picker.closing = false;
    s_picker.selected = {};
    s_picker.poolSelected = {};
    s_picker.pool = false;
    s_picker.roomID.clear();
    s_picker.stackDepth = 0;
    unwindSearch(depth);
}

// Map Pool용 검색 화면을 연다. 이미 열려 있거나 방이 없거나 시작됐으면 무시한다.
void openPoolSearch(MapPoolSettings settings, std::function<void(LevelInfo, bool)> onSelected) {
    auto& service = Service::get();
    if (s_picker.active || !service.room() || service.room()->started) return;
    auto* director = CCDirector::sharedDirector();
    s_picker.active = true;
    ++s_picker.generation;
    s_picker.roomID = service.room()->id;
    s_picker.stackDepth = director->m_pobScenesStack ? director->m_pobScenesStack->count() : 1;
    s_picker.selected = {};
    s_picker.pool = true;
    s_picker.conditions = settings;
    s_picker.poolSelected = std::move(onSelected);
    auto* scene = LevelSearchLayer::scene(0);
    if (!scene) {
        cancelLevelSearch();
        FLAlertLayer::create("Versus", "Unable to open level search.", "OK")->show();
        return;
    }
    director->replaceScene(CCTransitionFade::create(.25f, scene));
}

// 호스트 전용 일반 선택 검색 화면을 연다.
void openLevelSearch(std::function<void(LevelInfo)> onSelected) {
    auto& service = Service::get();
    if (s_picker.active || !service.room() || !service.isHost() || service.room()->started) {
        return;
    }
    s_picker.pool = false;
    s_picker.poolSelected = {};
    auto* director = CCDirector::sharedDirector();
    s_picker.active = true;
    ++s_picker.generation;
    s_picker.roomID = service.room()->id;
    s_picker.stackDepth = director->m_pobScenesStack ? director->m_pobScenesStack->count() : 1;
    s_picker.selected = std::move(onSelected);

    auto* scene = LevelSearchLayer::scene(0);
    if (!scene) {
        cancelLevelSearch();
        FLAlertLayer::create("Versus", "Unable to open level search.", "OK")->show();
        return;
    }
    director->replaceScene(CCTransitionFade::create(.25f, scene));
}
}

// 선택 모드일 때 검색 화면에 감시 노드를 붙이고, 뒤로 가기(버튼, Esc / Android 뒤로 가기)를 방 복귀로 바꾼다.
// 뒤로 가기는 GD의 onBack 훅(compat/gd2081, compat/gd2091의 GameHooks.cpp)이 아래 holdLevelSearchBack으로 넘긴다.
// GD 2.2091 iOS 바인딩에는 onBack 주소가 없어서, 뒤로 가기 버튼(Node IDs의 exit-button)의 콜백도 바꿔 둔다.
class $modify(VersusNativeLevelSearch,
              LevelSearchLayer){bool init(int type){if (!LevelSearchLayer::init(type)) return false;
markPicker(this);
if (ownsNode(this))
    if (auto* back = typeinfo_cast<CCMenuItem*>(getChildByIDRecursive("exit-button")))
        back->setTarget(this, menu_selector(VersusNativeLevelSearch::onVersusBack));
return true;
}

// 바꾼 뒤로 가기 버튼. 선택이 이미 끝났으면 GD의 원래 뒤로 가기(keyBackClicked가 onBack을 부른다)로 넘긴다.
void onVersusBack(CCObject*) {
    if (!versus::hooks::holdLevelSearchBack(this)) keyBackClicked();
}
}
;

bool versus::hooks::holdLevelSearchBack(LevelSearchLayer* layer) {
    if (!ownsNode(layer)) return false;
    returnToRoom();
    return true;
}

// 레벨 브라우저 화면에도 선택 모드 표시를 붙인다.
class $modify(VersusNativeLevelBrowser,
              LevelBrowserLayer){bool init(GJSearchObject * search){if (!LevelBrowserLayer::init(search)) return false;
markPicker(this);
return true;
}
}
;

// 레벨 셀 클릭: 선택 모드가 아니면 기본 동작. 선택 모드에서는 권한/온라인 레벨/Platformer 여부를 검사하고,
// 풀 모드면 풀 조건(poolMisfit)을 검사한 뒤 레벨을 방에 넘기고 돌아간다.
class $modify(VersusNativeLevelCell,
              LevelCell){void onClick(CCObject * sender){if (!ownsNode(this)){LevelCell::onClick(sender);
return;
}
auto& service = versus::Service::get();
if (!sameRoom() || (!s_picker.pool && !service.isHost()) || service.room()->started) {
    returnToRoom();
    return;
}
if (!m_level || m_level->m_levelID.value() <= 0 || m_level->m_levelType == GJLevelType::Editor ||
    m_level->m_levelType == GJLevelType::Main) {
    FLAlertLayer::create("Versus", "Choose an online level.", "OK")->show();
    return;
}
if (m_level->isPlatformer()) {
    FLAlertLayer::create("Versus", "Versus supports Classic maps only. Choose another map.", "OK")->show();
    return;
}
if (s_picker.pool) {
    auto const level = versus::describeLevel(m_level);
    bool const twoPlayer = m_level->m_twoPlayerMode;
    if (auto const why =
            versus::poolMisfit(level, twoPlayer, s_picker.conditions, m_level->m_likes - m_level->m_dislikes);
        !why.empty()) {
        FLAlertLayer::create("Map Pool",
                             fmt::format("{}\n<cy>{}</c>", why, versus::poolConditionText(s_picker.conditions)).c_str(),
                             "OK")
            ->show();
        return;
    }
    returnToRoom(level, twoPlayer);
    return;
}
returnToRoom(versus::describeLevel(m_level));
}
}
;
