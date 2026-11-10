// 매치메이킹 대기 중 보여 주는 미니 러너 게임 레이어. 아바타가 달리며 가시와 박쥐 몬스터를 점프로 피한다.
// 거리 기반 점수를 올리고 300점마다 배경 색을 바꾸며, 터치/Space/Up/W로 점프하고 Esc로 닫는다.
#include "SpectatorRunner.hpp"
#include "KeyDefer.hpp"
#include "RunnerPhysics.hpp"
#include "BattleHUD.hpp"
#include "Edition.hpp"
#include "VersusAudio.hpp"
#include "MenuCursor.hpp"
#include "SpriteFrames.hpp"
#include <Geode/binding/SimplePlayer.hpp>
#include <Geode/binding/GameManager.hpp>
#include <cmath>
#include <random>
using namespace geode::prelude;
namespace versus {
namespace {
// 러너의 바닥 높이와 아바타 중심에서 바닥까지의 거리(충돌 계산용).
constexpr float kRunnerGround = 20.f;
constexpr float kRunnerAvatarHalf = 13.5f;
// 날아다니는 장애물(박쥐 몬스터): GJBeast 스프라이트 조각을 모아 만들고 날개 조각은 계속 흔든다.
// 스프라이트 프레임을 못 불러오면 nullptr.
CCNode* batMonster() {
    if (!loadedFrame("GJBeast04_01_001.png", "FireSheet_01.plist")) return nullptr;
    auto* holder = CCNode::create();
    struct Part {
        char const* frame;
        CCPoint at;
        float angle;
        ccColor3B color;
    };
    Part parts[] = {
        {"GJBeast04_02_001.png", {-.5f, -9.6f}, -33.7f, ccBLACK},
        {"GJBeast04_01_001.png", {-5.5f, 2.6f}, -20.4f, ccBLACK},
        {"GJBeast04_03_001.png", {-12.8f, -1.f}, 0.f, ccWHITE},
        {"GJBeast04_04_001.png", {17.5f, 18.f}, 0.f, ccBLACK},
    };
    for (int i = 0; i < 4; ++i) {
        auto* frame = cachedFrame(parts[i].frame);
        if (!frame) continue;
        auto* sprite = CCSprite::createWithSpriteFrame(frame);
        if (!sprite) continue;
        sprite->setPosition(parts[i].at * .82f);
        sprite->setScale(.82f);
        sprite->setRotation(parts[i].angle);
        sprite->setColor(parts[i].color);
        holder->addChild(sprite, i);
        if (i == 0 || i == 3)
            sprite->runAction(CCRepeatForever::create(CCSequence::create(
                CCEaseSineInOut::create(CCRotateTo::create(.24f, parts[i].angle + (i == 3 ? 24.f : 10.f))),
                CCEaseSineInOut::create(CCRotateTo::create(.24f, parts[i].angle)), nullptr)));
    }
    return holder;
}
}
// 러너 상태 보관소.
struct SpectatorRunner::Impl {
    // 스크롤하는 배경 줄: 타일 목록, 타일 폭, 스크롤 오프셋, 속도 배율(factor).
    struct Strip {
        std::vector<CCSprite*> tiles;
        float width, offset = 0.f, factor;
    };
    // 장애물: 노드, 위치, 충돌 반폭/반높이, 비행 여부.
    struct Obstacle {
        CCNode* node;
        float x, y, halfWidth, halfHeight;
        bool flying;
    };
    std::vector<Strip> strips;
    std::vector<Obstacle> obstacles;
    CCNode* field = nullptr;
    SimplePlayer* avatar = nullptr;
    CCLabelBMFont *scoreLabel = nullptr, *hint = nullptr;
    CCLabelBMFont *title = nullptr, *footer = nullptr;
    CCScale9Sprite* panel = nullptr;
    CCMenu* controls = nullptr;
    CCRect playBounds;
    std::function<void()> chooseGame, closed;
    RunnerJump jump;
    RunnerMilestones milestones;
    double distance = 0.;
    float width = 0.f, spawn = 1.4f, time = 0.f, deathTime = 0.f;
    bool dead = false;
    bool matchmaking = false;
    bool stopped = false;
    int best = 0, displayedScore = -1;
    int touchID = -1;
    std::mt19937 random{std::random_device{}()};
    // 점프 입력. 죽은 상태면 0.5초 지난 뒤 장애물을 지우고 초기화하여 다시 시작한다.
    void press() {
        if (stopped) return;
        if (dead) {
            if (deathTime < .5f) return;
            for (auto& obstacle : obstacles) obstacle.node->removeFromParent();
            obstacles.clear();
            jump = {};
            distance = 0.f;
            spawn = 1.4f;
            dead = false;
            avatar->setOpacity(255);
            hint->setString("Tap / Space to jump - hold for a longer jump");
        }
        jump.press();
    }
    // 점수 단계(stage)에 맞는 배경색을 적용한다. animate면 0.65초 동안 색을 서서히 바꾼다.
    // 앞쪽 줄(factor 1)은 밝게 섞어 구분한다.
    void theme(int stage, bool animate) {
        static constexpr ccColor3B backgrounds[] = {{32, 104, 190}, {107, 52, 177}, {19, 130, 111},
                                                    {168, 65, 46},  {51, 65, 135},  {151, 65, 124}};
        auto color = backgrounds[static_cast<size_t>(std::max(0, stage)) % std::size(backgrounds)];
        for (auto& strip : strips) {
            auto target =
                strip.factor < 1.f ? color : ccc3((color.r + 255) / 2, (color.g + 255) / 2, (color.b + 255) / 2);
            for (auto* tile : strip.tiles) {
                tile->stopActionByTag(300);
                if (animate) {
                    auto* tint = CCTintTo::create(.65f, target.r, target.g, target.b);
                    tint->setTag(300);
                    tile->runAction(tint);
                } else
                    tile->setColor(target);
            }
        }
        panel->setColor(ccc3(color.r / 3, color.g / 3, color.b / 3));
    }
};
SpectatorRunner::SpectatorRunner() : m(std::make_unique<Impl>()) {}
SpectatorRunner::~SpectatorRunner() = default;
// 패널, 라벨, 배경/바닥 줄, 아바타를 만든다. 창이 너무 작으면 생성하지 않는다(false).
bool SpectatorRunner::init(PlayerProfile const& profile) {
    if (!CCLayer::init()) return false;
    setID("spectator-runner"_spr);
    auto window = CCDirector::sharedDirector()->getWinSize();
    if (!std::isfinite(window.width) || !std::isfinite(window.height) || window.width < 200.f || window.height < 150.f)
        return false;
    float width = std::min(510.f, window.width - 20.f), height = std::min(275.f, window.height - 20.f);
    auto origin = CCPoint{(window.width - width) / 2.f, (window.height - height) / 2.f};
    auto* shade = CCLayerColor::create(ccc4(3, 13, 35, 205));
    addChild(shade);
    auto* panel = CCScale9Sprite::create("square02b_001.png");
    if (!panel) return false;
    m->panel = panel;
    panel->setColor(ccc3(10, 45, 92));
    panel->setContentSize({width, height});
    panel->setPosition(origin + CCPoint{width / 2.f, height / 2.f});
    addChild(panel);
    // 크기가 최대 폭에 맞게 줄어드는 라벨 생성 헬퍼.
    auto text = [this](std::string value, CCPoint p, float scale, float maxWidth) {
        auto* label = createCosmeticLabel(value.c_str(), "bigFont.fnt");
        label->setPosition(p);
        label->setScale(std::min(scale, maxWidth / std::max(1.f, label->getContentSize().width)));
        addChild(label, 5);
        return label;
    };
    m->title = text("MATCHMAKING RUNNER", origin + CCPoint{width / 2.f, height - 15.f}, .75f, width - 20.f);
    m->scoreLabel = text("Runner 0  /  Best 0", origin + CCPoint{width - 15.f, 51.f}, .5f, width - 25.f);
    m->scoreLabel->setAnchorPoint({1.f, .5f});
    m->hint =
        text("Tap / Space to jump - hold for a longer jump", origin + CCPoint{width / 2.f, 30.f}, .55f, width - 20.f);
    m->footer =
        text("Searching continues in the background...", origin + CCPoint{width / 2.f, 13.f}, .6f, width - 20.f);
    m->footer->setColor(ccc3(146, 220, 255));
    // 플레이 영역은 클리핑 노드로 잘라 패널 밖으로 그려지지 않게 한다. 터치는 playBounds 안에서만 점프로 처리한다.
    float fieldHeight = height - 91.f;
    m->width = width - 20.f;
    m->playBounds = CCRect{origin.x, origin.y, width, height - 49.f};
    auto* stencil = CCLayerColor::create(ccc4(255, 255, 255, 255), m->width, fieldHeight);
    auto* clip = CCClippingNode::create(stencil);
    clip->setPosition(origin + CCPoint{10.f, 46.f});
    addChild(clip, 1);
    m->field = clip;
    // 텍스처를 타일로 반복 배치할 줄(strip)을 만든다. 화면 폭을 덮도록 타일 수(2~32)를 정한다.
    auto strip = [&](char const* file, float h, ccColor3B color, float factor) {
        Impl::Strip result;
        result.factor = factor;
        auto* texture = CCTextureCache::sharedTextureCache()->addImage(file, false);
        if (!texture) return;
        auto size = texture->getContentSize();
        if (!std::isfinite(size.width) || !std::isfinite(size.height) || size.width <= 0.f || size.height <= 0.f)
            return;
        result.width = size.width * h / size.height;
        if (!std::isfinite(result.width) || result.width < 1.f) return;
        int count = std::clamp(static_cast<int>(std::ceil(m->width / result.width)) + 2, 2, 32);
        result.tiles.reserve(count);
        for (int i = 0; i < count; ++i) {
            auto* tile = CCSprite::createWithTexture(texture);
            tile->setAnchorPoint({0, 0});
            tile->setScale(h / tile->getContentSize().height);
            tile->setColor(color);
            clip->addChild(tile);
            result.tiles.push_back(tile);
        }
        m->strips.push_back(std::move(result));
    };
    strip("game_bg_01_001.png", fieldHeight, ccc3(32, 104, 190), .13f);
    strip("groundSquare_01_001.png", kRunnerGround, ccc3(75, 190, 245), 1.f);
    auto* groundEdge = CCLayerColor::create(ccc4(153, 230, 255, 255), m->width, 1.f);
    groundEdge->setPosition({0.f, kRunnerGround - .5f});
    clip->addChild(groundEdge, 2);
    // 아바타는 프로필의 아이콘/색/오라를 그대로 사용한다.
    m->avatar = SimplePlayer::create(
        std::clamp(profile.icon, 1, std::max(1, GameManager::sharedState()->countForType(IconType::Cube))));
    if (!m->avatar) return false;
    applyIconLook(m->avatar, profile.color1, profile.color2, profile.glow);
    addMembershipAura(m->avatar, profile.membership, profile.cosmetics);
    m->avatar->setScale(.9f);
    clip->addChild(m->avatar, 3);
    m->obstacles.reserve(5);
    m->theme(0, false);
    setTouchEnabled(true);
    setKeyboardEnabled(true);
    setKeypadEnabled(true);
    scheduleUpdate();
    update(0.f);
    keepMenuCursorVisible(this);
    return true;
}
SpectatorRunner* SpectatorRunner::create(PlayerProfile const& profile) {
    auto* result = new SpectatorRunner;
    if (result->init(profile)) {
        result->autorelease();
        return result;
    }
    delete result;
    return nullptr;
}
// 매치메이킹 모드: 닫기 버튼과(콜백이 있으면) 게임 전환 버튼을 우상단에 단다. 한 번만 적용된다.
void SpectatorRunner::setMatchmakingMode(std::function<void()> switchGame, std::function<void()> closed) {
    if (m->matchmaking) return;
    m->matchmaking = true;
    m->chooseGame = std::move(switchGame);
    m->closed = std::move(closed);
    m->title->setString("MATCHMAKING RUNNER");
    m->footer->setString("Searching continues in the background...");
    auto window = CCDirector::sharedDirector()->getWinSize();
    auto* controls = CCMenu::create();
    m->controls = controls;
    controls->setPosition({0.f, 0.f});
    addChild(controls, 20);
    auto* sprite = CCSprite::createWithSpriteFrameName("GJ_closeBtn_001.png");
    if (sprite) {
        sprite->setScale(.65f);
        auto* item = CCMenuItemSpriteExtra::create(sprite, this, menu_selector(SpectatorRunner::closeMatchmaking));
        item->setPosition({window.width - 18.f, window.height - 18.f});
        controls->addChild(item);
    }
    if (m->chooseGame) {
        auto* switchSprite = CCSprite::createWithSpriteFrameName("GJ_gpgBtn_001.png");
        if (switchSprite) {
            switchSprite->setScale(28.f / std::max(1.f, switchSprite->getContentSize().height));
            auto* game = CCMenuItemSpriteExtra::create(switchSprite, this, menu_selector(SpectatorRunner::switchGame));
            game->setID("runner-switch-game"_spr);
            game->setPosition({window.width - 53.f, window.height - 18.f});
            controls->addChild(game);
        }
    }
    if (isRunning()) controls->setHandlerPriority(-501);
}
void SpectatorRunner::closeMatchmaking(CCObject*) {
    if (m->matchmaking) dismiss();
}
// 게임 전환 버튼: 콜백을 먼저 꺼내 두고 닫은 뒤 호출한다.
void SpectatorRunner::switchGame(CCObject*) {
    if (!m->matchmaking || m->stopped) return;
    Ref<SpectatorRunner> keepAlive(this);
    auto choose = std::move(m->chooseGame);
    dismiss();
    if (choose) choose();
}
// 입력, 스케줄, 액션을 모두 끄고 콜백을 비운다. 여러 번 호출해도 한 번만 동작한다.
void SpectatorRunner::stopRunner() {
    if (m->stopped) return;
    m->stopped = true;
    m->jump.release();
    m->touchID = -1;
    setTouchEnabled(false);
    setKeyboardEnabled(false);
    setKeypadEnabled(false);
    unscheduleAllSelectors();
    stopAllActions();
    m->chooseGame = {};
    m->closed = {};
    cleanup();
}
// 러너를 정지하고 부모에서 제거한 뒤 closed 콜백을 호출한다.
void SpectatorRunner::dismiss() {
    if (m->stopped) return;
    Ref<SpectatorRunner> keepAlive(this);
    auto closed = std::move(m->closed);
    stopRunner();
    removeFromParentAndCleanup(true);
    if (closed) closed();
}
void SpectatorRunner::onEnter() {
    CCLayer::onEnter();
    if (m->controls) m->controls->setHandlerPriority(-501);
}
void SpectatorRunner::onExit() {
    stopRunner();
    CCLayer::onExit();
}
// 다른 UI보다 먼저 터치를 받도록 높은 우선순위(-500)로 등록한다.
void SpectatorRunner::registerWithTouchDispatcher() {
    CCDirector::sharedDirector()->getTouchDispatcher()->addTargetedDelegate(this, -500, true);
}
bool SpectatorRunner::ccTouchBegan(CCTouch* touch, CCEvent*) {
    if (m->stopped || !isVisible()) return false;
    if (m->touchID < 0 && m->playBounds.containsPoint(touch->getLocation())) {
        m->touchID = touch->getID();
        m->press();
    }
    return true;
}
void SpectatorRunner::ccTouchEnded(CCTouch* touch, CCEvent*) {
    if (touch->getID() == m->touchID) {
        m->touchID = -1;
        m->jump.release();
    }
}
void SpectatorRunner::ccTouchCancelled(CCTouch* touch, CCEvent* event) {
    ccTouchEnded(touch, event);
}
void SpectatorRunner::keyDown(enumKeyCodes key, double) {
    if (m->stopped) return;
    if (key == KEY_Escape) {
        keyBackClicked();
        return;
    }
    if (isVisible() && (key == KEY_Space || key == KEY_Up || key == KEY_W)) m->press();
}
void SpectatorRunner::keyUp(enumKeyCodes key, double) {
    if (key == KEY_Space || key == KEY_Up || key == KEY_W) m->jump.release();
}
void SpectatorRunner::keyBackClicked() {
    if (m->matchmaking && !m->stopped) versus::deferBack(this, [this] { closeMatchmaking(nullptr); });
}
// 매 프레임: 점프 물리 적분(최대 1/120초 단위), 속도는 거리에 따라 175에서 최대 300까지 증가,
// 장애물 생성(1.05~1.64초 간격, 최대 5개, 350 이후 1/3 확률로 비행형), 충돌 판정, 배경 스크롤, 점수 표시.
void SpectatorRunner::update(float dt) {
    if (m->stopped || !isVisible()) {
        m->jump.release();
        return;
    }
    dt = runnerFrameDelta(dt);
    m->time = std::fmod(m->time + dt, 60.f);
    if (m->dead) m->deathTime += dt;
    float speed = static_cast<float>(std::min(300., 175. + m->distance * .025));
    if (!m->dead) {
        for (float remaining = dt; remaining > 0.f;) {
            float step = std::min(remaining, 1.f / 120.f);
            m->jump.step(step);
            remaining -= step;
        }
        m->distance = std::min(99999990., m->distance + dt * speed);
        m->spawn -= dt;
        if (m->spawn <= 0.f && m->obstacles.size() < 5) {
            bool flying = m->distance > 350.f && m->random() % 3 == 0;
            CCNode* node = flying ? static_cast<CCNode*>(batMonster())
                                  : static_cast<CCNode*>(CCSprite::createWithSpriteFrameName("spike_01_001.png"));
            if (node) {
                node->setScale(flying ? .92f : .75f);
                m->field->addChild(node, 2);
                float y = flying ? (m->random() % 2 ? 66.f : 94.f)
                                 : kRunnerGround + node->getScaledContentSize().height / 2.f;
                m->obstacles.push_back({node, m->width + 30.f, y, flying ? 20.f : 8.f, flying ? 9.f : 10.f, flying});
            }
            m->spawn = 1.05f + (m->random() % 60) / 100.f;
        }
        for (auto& obstacle : m->obstacles) {
            obstacle.x -= dt * speed;
            float y = obstacle.y + (obstacle.flying ? std::sin(m->time * 6.f) * 3.f : 0.f);
            obstacle.node->setPosition({obstacle.x, y});
            // 충돌 판정: 아바타 위치(x=55) 근처의 장애물 박스와 겹치면 사망 처리하고 최고 점수를 갱신한다.
            if (std::abs(obstacle.x - 55.f) < obstacle.halfWidth + 7.f &&
                std::abs(y - (kRunnerGround + kRunnerAvatarHalf + m->jump.height)) < obstacle.halfHeight + 7.f) {
                m->dead = true;
                m->deathTime = 0.f;
                m->jump.release();
                m->avatar->setOpacity(140);
                m->best = std::max(m->best, static_cast<int>(m->distance / 10.f));
                m->displayedScore = -1;
                m->hint->setString("Crashed! Tap / Space to retry");
            }
        }
        std::erase_if(m->obstacles, [](auto const& obstacle) {
            if (obstacle.x > -30.f) return false;
            obstacle.node->removeFromParent();
            return true;
        });
        for (auto& strip : m->strips) {
            strip.offset = std::fmod(strip.offset + dt * speed * strip.factor, strip.width);
            for (size_t i = 0; i < strip.tiles.size(); ++i)
                strip.tiles[i]->setPosition({i * strip.width - strip.offset, 0.f});
        }
    }
    m->avatar->setPosition({55.f, kRunnerGround + kRunnerAvatarHalf + m->jump.height});
    // 점수는 이동 거리/10. 300점 구간이 올라가면 테마를 바꾸고 효과음을 낸다.
    int score = static_cast<int>(m->distance / 10.f);
    int oldStage = m->milestones.stage;
    bool reached = m->milestones.update(score);
    if (m->milestones.stage != oldStage) m->theme(m->milestones.stage, reached);
    if (reached) audio::play(audio::Cue::RunnerMilestone);
    if (score != m->displayedScore) {
        m->displayedScore = score;
        m->scoreLabel->setString(fmt::format("Runner {}  /  Best {}", score, m->best).c_str());
    }
}
}
