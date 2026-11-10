// 배틀 HUD 구현. 플레이어 카드(Card): 아이콘, 이름, 칭호, 현재 퍼센트, 남은 목숨 또는 시도/목표,
// 최고 기록, 사망/리롤 로그, 핵 표시. 카드 투명도 적용(자식/DrawNode/파티클), 배치 설정의 저장과 계산,
// 그룹 HUD의 선두 선정과 순위 헤더를 포함한다.
#include "BattleHUD.hpp"
#include "BattleProgress.hpp"
#include "Edition.hpp"
#include "VectorArt.hpp"
#include <Geode/binding/GameManager.hpp>
#include <Geode/binding/SimplePlayer.hpp>
#include <algorithm>
#include <array>
#include <cmath>
#include <initializer_list>
#include <string>
#include <vector>

using namespace geode::prelude;
namespace versus {
namespace {
// 하트 아이콘. 스프라이트가 있으면 그것을 쓰고 없으면 하트 곡선 폴리곤으로 그린다.
CCNodeRGBA* lifeHeart(float size) {
    auto* holder = CCNodeRGBA::create();
    auto* shape = CCDrawNode::create();
    holder->setCascadeOpacityEnabled(true);
    holder->setContentSize({size, size});
    holder->setAnchorPoint({.5f, .5f});
    holder->ignoreAnchorPointForPosition(false);
    if (auto* sprite = art::artSprite("heart-life.png", size * 1.12f)) {
        sprite->setPosition({size * .5f, size * .5f});
        holder->addChild(sprite);
        return holder;
    }
    constexpr size_t pointsCount = 64;
    constexpr float pi = 3.14159265358979323846f;
    std::array<CCPoint, pointsCount> points;
    float const scale = size / 36.f;
    for (size_t index = 0; index < pointsCount; ++index) {
        float angle = 2.f * pi * static_cast<float>(index) / static_cast<float>(pointsCount);
        float sine = std::sin(angle);
        float x = 16.f * sine * sine * sine;
        float y =
            13.f * std::cos(angle) - 5.f * std::cos(2.f * angle) - 2.f * std::cos(3.f * angle) - std::cos(4.f * angle);
        points[index] = CCPoint{size * .5f + x * scale, size * .5f + (y + 2.5f) * scale};
    }
    shape->drawPolygon(points.data(), static_cast<unsigned>(points.size()), {1.f, .12f, .2f, 1.f}, 1.25f,
                       {.48f, .02f, .06f, 1.f});
    holder->addChild(shape);
    return holder;
}

// 문구가 바뀐 경우에만 설정하고 폭에 맞춰 줄인다. 바뀌었으면 true.
bool text(CCLabelBMFont* label, std::string const& value, float width, float scale) {
    if (value == label->getString()) return false;
    label->setString(value.c_str());
    label->limitLabelWidth(width, scale, .14f);
    return true;
}

// 플레이어 한 명의 카드. 라벨들과 애니메이션/투명도 상태를 들고 있다.
struct Card {
    PlayerProfile profile;
    CCNode* root = nullptr;
    CCLabelBMFont* current = nullptr;
    CCLabelBMFont* progress = nullptr;
    CCLabelBMFont* detail = nullptr;
    CCNodeRGBA* heart = nullptr;
    CCNode* log = nullptr;
    CCLabelBMFont* hacks = nullptr;
    float textWidth = 0.f;
    float edge = 0.f;
    float progressScale = 1.f;
    float detailScale = 1.f;
    float currentScale = 1.f;
    float placedWidth = -1.f;
    int lastRemaining = 0;
    int lastPercent = 0;
    int lastAttempts = 0;
    int lastRunPercent = 0;
    bool mirrored = false;
    bool lives = false;
    bool initialized = false;
    GLubyte alpha = 255, shownAlpha = 255;
    // 파티클 원래 알파 값(투명도 적용 후 복원 계산용).
    struct ParticleAlpha {
        Ref<CCParticleSystem> system;
        float start = 1.f, startVar = 0.f, end = 0.f, endVar = 0.f;
    };
    std::vector<ParticleAlpha> particles;
    bool particlesKept = false;
    // CCDrawNode가 그린 정점의 원래 색(투명도 적용 후 복원 계산용).
    struct DrawAlpha {
        Ref<CCDrawNode> node;
        std::vector<ccColor4B> colours;
        GLubyte shown = 255;
    };
    std::vector<DrawAlpha> draws;

    // 카드 UI를 만든다. mirror이면 오른쪽 카드로 좌우 반전한다.
    // 배경 그라데이션, 아이콘, 이름, 칭호, 퍼센트/목숨 라벨, 로그 영역을 구성한다.
    void create(CCNode* parent, PlayerProfile const& player, GameRules const& rules, CCPoint position, float width,
                bool mirror) {
        constexpr float height = 66.f;
        mirrored = mirror;
        profile = player;
        root = CCNode::create();
        root->setPosition(position);
        root->setContentSize({width, height});
        parent->addChild(root);

        auto* background =
            CCLayerGradient::create(ccc4(255, 255, 255, 100), ccc4(255, 255, 255, 0), {mirrored ? -1.f : 1.f, -1.f});
        background->setContentSize({width, height});
        background->setID("status-glow"_spr);
        root->addChild(background);

        float const iconX = mirrored ? width - 25.f : 25.f;

        auto* manager = GameManager::sharedState();
        int const count = manager ? std::max(1, manager->countForType(IconType::Cube)) : 1;
        auto* icon = SimplePlayer::create(std::clamp(player.icon, 1, count));
        applyIconLook(icon, player.color1, player.color2, player.glow);
        addMembershipAura(icon, player.membership, player.cosmetics);
        icon->setScale(.82f);
        icon->setFlipX(mirrored);
        icon->setPosition({iconX, 34.f});
        root->addChild(icon);

        float const anchor = mirrored ? 1.f : 0.f;
        edge = mirrored ? width - 49.f : 49.f;
        textWidth = width - 58.f;
        auto makeLabel = [this, anchor](char const* value, CCPoint at, ccColor3B color) {
            auto* label = createCosmeticLabel(value, "bigFont.fnt");
            label->setAnchorPoint({anchor, .5f});
            label->setPosition(at);
            label->setColor(color);
            root->addChild(label);
            return label;
        };
        auto* name = makeLabel(player.name.c_str(), {edge, 57.f}, ccWHITE);
        name->limitLabelWidth(textWidth, .32f, .14f);
        styleNickname(name, player.membership, player.cosmetics);
        if (auto* badge = createTitleBadge(root, player.cosmetics.title, {0.f, 44.f}, .72f, textWidth, true)) {
            float const badgeWidth = badge->getContentSize().width * badge->getScale();
            badge->setPositionX(mirrored ? edge - badgeWidth / 2.f : edge + badgeWidth / 2.f);
        }

        // 시도 모드(연습 아님)에서만 하트와 남은 목숨을 표시한다.
        lives = rules.mode == 0 && !rules.practice;
        current = makeLabel("", {edge, 26.f}, ccWHITE);
        heart = lifeHeart(13.f);
        heart->setVisible(lives);
        root->addChild(heart);
        progress = makeLabel("", {edge, 26.f}, ccc3(181, 235, 255));
        detail = makeLabel("", {edge, 8.f}, ccc3(132, 200, 235));
        log = CCNode::create();
        log->setPosition({mirrored ? width - 8.f : 8.f, -5.f});
        root->addChild(log);
    }

    // 핵 사용 정보 줄을 만들거나 갱신한다. 줄이 생기면 로그를 그 아래로 내린다.
    void setHacks(std::string const& value) {
        if (!root) return;
        if (!hacks) {
            hacks = createCosmeticLabel("", "chatFont.fnt");
            hacks->setAnchorPoint({mirrored ? 1.f : 0.f, .5f});
            hacks->setPosition({mirrored ? root->getContentSize().width - 8.f : 8.f, -4.f});
            root->addChild(hacks);
            if (log) log->setPositionY(-15.f);
        }
        auto const text = "HACKS  " + value;
        if (text == hacks->getString()) return;
        hacks->setString(text.c_str());
        hacks->setColor(value == "No hacks" ? ccc3(150, 200, 160) : ccc3(255, 170, 90));
        hacks->limitLabelWidth(root->getContentSize().width - 12.f, .42f, .2f);
    }
    // 현재 퍼센트 라벨 폭에 맞춰 하트와 진행 라벨 위치를 옮긴다.
    void placeLives() {
        float const width = current->getContentSize().width * currentScale;
        if (std::abs(width - placedWidth) < .01f) return;
        placedWidth = width;
        float const dir = mirrored ? -1.f : 1.f;
        float x = edge + dir * (width + 5.f);
        if (lives) {
            heart->setPosition({x + dir * 6.5f, 26.f});
            x += dir * 15.f;
        }
        progress->setPositionX(x);
    }

    // 새 로그 줄을 넣기 전에 기존 줄을 한 칸씩 아래로 내리고 3번째 이후는 지운다.
    void pushLog() {
        auto* children = log->getChildren();
        std::vector<CCNode*> lines;
        if (children)
            for (auto* child : CCArrayExt<CCNode*>(children)) lines.push_back(child);
        for (auto* line : lines) {
            if (line->getTag() >= 2) {
                line->removeFromParent();
                continue;
            }
            line->setTag(line->getTag() + 1);
            line->setPositionY(-9.f * line->getTag());
        }
    }
    // 리롤 알림 줄(아이콘 + 회전하는 리플레이 아이콘 + REROLL)을 4초 동안 표시한다.
    void addReroll() {
        if (!log) return;
        pushLog();
        float const dir = mirrored ? -1.f : 1.f;
        auto* line = CCNodeRGBA::create();
        line->setCascadeOpacityEnabled(true);
        auto* manager = GameManager::sharedState();
        int const count = manager ? std::max(1, manager->countForType(IconType::Cube)) : 1;
        auto* icon = SimplePlayer::create(std::clamp(profile.icon, 1, count));
        applyIconLook(icon, profile.color1, profile.color2, profile.glow);
        icon->setCascadeOpacityEnabled(true);
        icon->setScale(.32f);
        icon->setFlipX(mirrored);
        icon->setPosition({dir * 5.f, 0.f});
        line->addChild(icon);
        float x = 15.f;
        if (auto* retry = loadedFrameSprite("GJ_replayBtn_001.png", "GJ_GameSheet03.plist")) {
            retry->setScale(12.f / std::max(1.f, retry->getContentSize().height));
            retry->setPosition({dir * (x + 6.f), 0.f});
            line->addChild(retry);
            retry->runAction(CCEaseSineOut::create(CCRotateBy::create(.6f, -360.f)));
            x += 15.f;
        }
        auto* text = createCosmeticLabel("REROLL", "bigFont.fnt");
        text->setAnchorPoint({mirrored ? 1.f : 0.f, .5f});
        text->setScale(.22f);
        text->setColor(ccc3(255, 205, 120));
        text->setPosition({dir * x, 0.f});
        line->addChild(text);
        line->setOpacity(0);
        line->setTag(0);
        line->setPositionX(mirrored ? 6.f : -6.f);
        log->addChild(line);
        line->runAction(
            CCSequence::create(CCSpawn::create(CCFadeIn::create(.15f), CCMoveTo::create(.15f, {0.f, 0.f}), nullptr),
                               CCDelayTime::create(4.f), CCFadeOut::create(.5f), CCRemoveSelf::create(), nullptr));
    }

    // "DIED AT n%" 줄을 2.4초 동안 표시한다.
    void addDeath(int percent) {
        if (!log) return;
        pushLog();
        auto* text =
            createCosmeticLabel(fmt::format("DIED AT {}%", std::clamp(percent, 0, 100)).c_str(), "bigFont.fnt");
        text->setAnchorPoint({mirrored ? 1.f : 0.f, .5f});
        text->setScale(.24f);
        text->setColor(ccc3(255, 120, 132));
        text->setOpacity(0);
        text->setTag(0);
        text->setPositionX(mirrored ? 6.f : -6.f);
        log->addChild(text);
        text->runAction(
            CCSequence::create(CCSpawn::create(CCFadeIn::create(.15f), CCMoveTo::create(.15f, {0.f, 0.f}), nullptr),
                               CCDelayTime::create(2.4f), CCFadeOut::create(.5f), CCRemoveSelf::create(), nullptr));
    }

    // 상태로 카드를 갱신한다. 현재 퍼센트, 목숨/시도/목표, 상태(FORFEITED, PAUSED, CLEAR! 등)를 보여 주고,
    // 값이 바뀐 순간 라벨을 잠깐 키우며 시도가 늘면 사망 로그를 남긴다.
    void update(BattlePlayerState const& state, GameRules const& rules) {
        int const remaining = std::max(0, rules.attempts - state.attemptsUsed);
        bool const endless = lives && rules.attempts >= battle::kEndlessAttempts;
        if (text(current, fmt::format("{}%", std::clamp(state.currentPercent, 0, 100)), textWidth * .5f, .48f))
            currentScale = current->getScale();
        std::string value;
        if (endless)
            value = "INF";
        else if (lives)
            value = std::to_string(remaining);
        else if (rules.mode == 1)
            value = fmt::format("TARGET {}%", rules.targetPercent);
        else
            value = fmt::format("ATT {}", std::max(1, state.attemptsUsed + (state.inAttempt ? 1 : 0)));
        bool const progressChanged = text(progress, value, textWidth * .5f - (lives ? 15.f : 0.f), lives ? .4f : .3f);
        if (progressChanged) progressScale = progress->getScale();
        progress->setColor(lives && remaining <= 1 ? ccc3(255, 163, 178) : ccc3(183, 238, 255));
        if (GLubyte const want = lives && remaining == 0 ? 90 : 255; heart->getOpacity() != want) {
            heart->setOpacity(want);
            heart->updateDisplayedOpacity(alpha);
        }
        placeLives();

        std::string info;
        ccColor3B tint = ccc3(136, 207, 243);
        if (state.forfeited) {
            info = "FORFEITED";
            tint = ccc3(255, 146, 162);
        } else if (state.paused) {
            info = "PAUSED";
            tint = ccc3(255, 216, 135);
        } else if (state.cleared) {
            info = "CLEAR!";
            tint = ccc3(138, 255, 193);
        } else if (state.spectating)
            info =
                fmt::format("{} | BEST {}%", battle::resultLocked(state, rules) ? "LOCKED" : "WAIT", state.bestPercent);
        else if (rules.mode == 1)
            info = fmt::format("TARGET {}%", rules.targetPercent);
        else
            info = fmt::format("BEST {}%", state.bestPercent);
        bool const detailChanged = text(detail, info, textWidth, .3f);
        if (detailChanged) detailScale = detail->getScale();
        detail->setColor(tint);
        if (initialized) {
            if (lives && !endless && remaining != lastRemaining) progress->setScale(progressScale * 1.75f);
            if (state.currentPercent != lastPercent)
                current->setScale(std::max(current->getScale(), currentScale * 1.18f));
            if (state.attemptsUsed > lastAttempts && !state.cleared && !state.forfeited)
                addDeath(state.inAttempt ? lastRunPercent : state.currentPercent);
        }
        if (state.inAttempt) lastRunPercent = state.currentPercent;
        lastRemaining = remaining;
        lastPercent = state.currentPercent;
        lastAttempts = state.attemptsUsed;
        initialized = true;
    }

    // 카드 트리 전체에 투명도를 적용한다. DrawNode는 정점 색을 직접 조정해야 한다.
    void fadeTree(CCNode* node, GLubyte inherited) {
        auto* children = node->getChildren();
        if (!children) return;
        for (auto* child : CCArrayExt<CCNode*>(children)) {
            GLubyte next = inherited;
            if (auto* rgba = typeinfo_cast<CCRGBAProtocol*>(child)) {
                rgba->updateDisplayedOpacity(inherited);
                if (auto* layer = typeinfo_cast<CCLayerColor*>(child)) layer->setColor(layer->getColor());
                if (rgba->isCascadeOpacityEnabled()) next = rgba->getDisplayedOpacity();
            }
            if (auto* draw = typeinfo_cast<CCDrawNode*>(child)) {
                draw->updateDisplayedOpacity(inherited);
                fadeDraw(draw, draw->getDisplayedOpacity());
            }
            fadeTree(child, next);
        }
    }
    // CCDrawNode의 정점 알파를 원래 색 기준으로 다시 계산해 적용한다(프리멀티플라이드 블렌드면 RGB도).
    void fadeDraw(CCDrawNode* node, GLubyte opacity) {
        auto found = std::find_if(draws.begin(), draws.end(),
                                  [node](DrawAlpha const& each) { return each.node.data() == node; });
        if (found == draws.end()) {
            draws.push_back({Ref<CCDrawNode>(node), {}, 255});
            found = draws.end() - 1;
        }
        auto const count = static_cast<size_t>(std::max<GLsizei>(0, node->m_nBufferCount));
        auto* buffer = node->m_pBuffer;
        if (!buffer && count > 0) return;
        bool changed = false;
        if (count < found->colours.size()) {
            found->colours.clear();
            found->shown = 255;
        }
        if (count > found->colours.size()) {
            for (size_t i = found->colours.size(); i < count; ++i) found->colours.push_back(buffer[i].colors);
            changed = true;
        }
        if (!changed && opacity == found->shown) return;
        bool const premultiplied = node->m_sBlendFunc.src == GL_ONE;
        float const k = opacity / 255.f;
        for (size_t i = 0; i < count; ++i) {
            auto const& drawn = found->colours[i];
            auto& colour = buffer[i].colors;
            colour.a = static_cast<GLubyte>(drawn.a * k);
            if (premultiplied) {
                colour.r = static_cast<GLubyte>(drawn.r * k);
                colour.g = static_cast<GLubyte>(drawn.g * k);
                colour.b = static_cast<GLubyte>(drawn.b * k);
            }
        }
        node->m_bDirty = true;
        found->shown = opacity;
    }
    // 카드 안의 파티클 시스템과 원래 알파를 모아 둔다.
    void keepParticles(CCNode* node) {
        auto* children = node->getChildren();
        if (!children) return;
        for (auto* child : CCArrayExt<CCNode*>(children)) {
            if (auto* system = typeinfo_cast<CCParticleSystem*>(child))
                particles.push_back({system, system->getStartColor().a, system->getStartColorVar().a,
                                     system->getEndColor().a, system->getEndColorVar().a});
            keepParticles(child);
        }
    }
    // alpha 값을 카드와 자식 노드, 파티클에 반영한다(바뀐 경우만).
    void applyAlpha() {
        if (!root || (alpha == 255 && shownAlpha == 255)) return;
        draws.erase(std::remove_if(draws.begin(), draws.end(),
                                   [](DrawAlpha const& each) { return !each.node || !each.node->getParent(); }),
                    draws.end());
        fadeTree(root, alpha);
        if (alpha != shownAlpha) {
            if (!particlesKept) {
                particlesKept = true;
                keepParticles(root);
            }
            float const k = alpha / 255.f;
            for (auto& each : particles) {
                auto* system = each.system.data();
                if (!system) continue;
                auto colour = system->getStartColor();
                colour.a = each.start * k;
                system->setStartColor(colour);
                colour = system->getStartColorVar();
                colour.a = each.startVar * k;
                system->setStartColorVar(colour);
                colour = system->getEndColor();
                colour.a = each.end * k;
                system->setEndColor(colour);
                colour = system->getEndColorVar();
                colour.a = each.endVar * k;
                system->setEndColorVar(colour);
            }
        }
        shownAlpha = alpha;
    }

    // 라벨 배율을 목표 배율로 서서히 되돌린다(강조 효과 감쇠).
    void animate(float dt) {
        float const amount = std::clamp(dt * 8.f, 0.f, 1.f);
        auto settle = [amount](CCLabelBMFont* label, float target) {
            if (label->getScale() == target) return;
            float const next = label->getScale() + (target - label->getScale()) * amount;
            label->setScale(std::abs(next - target) < .001f ? target : next);
        };
        settle(progress, progressScale);
        settle(detail, detailScale);
        settle(current, currentScale);
    }
};
}

// BattleHUD 내부 상태: 규칙(전체/좌석별)과 호스트/게스트 카드.
struct BattleHUD::Impl {
    GameRules rules, hostRules, guestRules;
    Card host, guest;
};

CCNodeRGBA* createLifeHeart(float size) {
    return lifeHeart(size);
}
BattleHUD::BattleHUD() : m(std::make_unique<Impl>()) {}
BattleHUD::~BattleHUD() = default;

BattleHUD* BattleHUD::create(PlayerProfile const& host, PlayerProfile const& guest, GameRules const& rules) {
    auto* node = new BattleHUD;
    if (node->init(host, guest, rules)) {
        node->autorelease();
        return node;
    }
    delete node;
    return nullptr;
}

// 모바일은 오른쪽 위에 일시 정지 버튼 자리를 비워 둔다.
#ifdef GEODE_IS_DESKTOP
constexpr float kPauseReserve = 0.f;
#else
constexpr float kPauseReserve = 40.f;
#endif

// 배치 설정 키("hud-부분-필드").
static std::string placementKey(int part, char const* field) {
    static constexpr char const* names[] = {"own", "opponent", "text"};
    return fmt::format("hud-{}-{}", names[std::clamp(part, 0, 2)], field);
}
// 저장된 배치 설정을 범위 안으로 제한해 읽는다.
BattlePlacement battlePlacement(int part) {
    auto* mod = Mod::get();
    BattlePlacement value;
    value.anchor =
        static_cast<int>(std::clamp<int64_t>(mod->getSavedValue<int64_t>(placementKey(part, "anchor"), 0), 0, 4));
    value.x = static_cast<int>(std::clamp<int64_t>(mod->getSavedValue<int64_t>(placementKey(part, "x"), 0),
                                                   -kPlacementOffsetMax, kPlacementOffsetMax));
    value.y = static_cast<int>(std::clamp<int64_t>(mod->getSavedValue<int64_t>(placementKey(part, "y"), 0),
                                                   -kPlacementOffsetMax, kPlacementOffsetMax));
    value.scale = static_cast<int>(std::clamp<int64_t>(mod->getSavedValue<int64_t>(placementKey(part, "scale"), 100),
                                                       kPlacementScaleMin, kPlacementScaleMax));
    return value;
}
// 배치 설정을 범위 안으로 제한해 저장한다.
void setBattlePlacement(int part, BattlePlacement const& value) {
    auto* mod = Mod::get();
    mod->setSavedValue<int64_t>(placementKey(part, "anchor"), std::clamp(value.anchor, 0, 4));
    mod->setSavedValue<int64_t>(placementKey(part, "x"),
                                std::clamp(value.x, -kPlacementOffsetMax, kPlacementOffsetMax));
    mod->setSavedValue<int64_t>(placementKey(part, "y"),
                                std::clamp(value.y, -kPlacementOffsetMax, kPlacementOffsetMax));
    mod->setSavedValue<int64_t>(placementKey(part, "scale"),
                                std::clamp(value.scale, kPlacementScaleMin, kPlacementScaleMax));
}
namespace {
constexpr float kCardHeight = 66.f, kCardInset = 4.f;
constexpr float kLogReserve = 22.f;
}
// 카드 원점 계산. 기본 앵커는 내 카드 왼쪽 위, 상대 카드 오른쪽 위이며 하단일 때는 로그 자리를 비운다.
// 오프셋을 더한 뒤 화면 밖으로 나가지 않게 제한한다.
CCPoint battleCardOrigin(int part, CCSize window, float width) {
    auto const place = battlePlacement(part);
    float const scale = place.scale / 100.f;
    float const w = width * scale, h = kCardHeight * scale;
    int anchor = place.anchor;
    if (anchor == kAnchorDefault) anchor = part == kPartOwn ? kAnchorTopLeft : kAnchorTopRight;
    bool const right = anchor == kAnchorTopRight || anchor == kAnchorBottomRight;
    bool const bottom = anchor == kAnchorBottomLeft || anchor == kAnchorBottomRight;
    float x = right ? window.width - kCardInset - (bottom ? 0.f : kPauseReserve) - w : kCardInset;
    float y = bottom ? kCardInset + kLogReserve * scale : window.height - kCardInset - h;
    x = std::clamp(x + place.x, 0.f, std::max(0.f, window.width - w));
    y = std::clamp(y + place.y, 0.f, std::max(0.f, window.height - h));
    return {x, y};
}
// 안내 텍스트 위치 계산(앵커별 기본 위치 + 오프셋, 크기 설정 반영).
BattleTextPlace battleTextPlace(CCSize window) {
    auto const place = battlePlacement(kPartText);
    BattleTextPlace result{{window.width / 2.f, window.height - 91.f}, {.5f, .5f}, .65f * place.scale / 100.f};
    switch (place.anchor) {
    case kAnchorTopLeft:
        result.at = CCPoint{8.f, window.height - 91.f};
        result.anchor = CCPoint{0.f, .5f};
        break;
    case kAnchorTopRight:
        result.at = CCPoint{window.width - 8.f, window.height - 91.f};
        result.anchor = CCPoint{1.f, .5f};
        break;
    case kAnchorBottomLeft:
        result.at = CCPoint{8.f, 16.f};
        result.anchor = CCPoint{0.f, .5f};
        break;
    case kAnchorBottomRight:
        result.at = CCPoint{window.width - 8.f, 16.f};
        result.anchor = CCPoint{1.f, .5f};
        break;
    default: break;
    }
    result.at.x = std::clamp(result.at.x + place.x, 0.f, window.width);
    result.at.y = std::clamp(result.at.y + place.y, 8.f, window.height - 8.f);
    return result;
}

// 창 크기에 맞춰 카드 폭을 정하고 양쪽 카드를 만든다.
bool BattleHUD::init(PlayerProfile const& host, PlayerProfile const& guest, GameRules const& rules) {
    if (!CCNode::init()) return false;
    m->rules = rules;
    m->hostRules = m->guestRules = rules;
    auto const window = CCDirector::sharedDirector()->getWinSize();
    setContentSize(window);
    setID("player-cards"_spr);
    float const width = std::min(205.f, (window.width - 116.f) / 2.f);
    m->host.create(this, host, rules, {}, width, false);
    m->guest.create(this, guest, rules, {}, width, true);
    applyLayout();
    updatePlayers({}, {});
    scheduleUpdate();
    return true;
}
// 저장된 설정으로 카드 크기와 위치를 다시 적용한다.
void BattleHUD::applyLayout() {
    auto const window = getContentSize();
    for (auto* card : {&m->host, &m->guest}) {
        if (!card->root) continue;
        int const part = card == &m->host ? kPartOwn : kPartOpponent;
        card->root->setScale(battlePlacement(part).scale / 100.f);
        card->root->setPosition(battleCardOrigin(part, window, card->root->getContentSize().width));
    }
}

void BattleHUD::updatePlayers(BattlePlayerState const& host, BattlePlayerState const& guest) {
    m->host.update(host, m->hostRules);
    m->guest.update(guest, m->guestRules);
}
void BattleHUD::setSeatRules(GameRules const& host, GameRules const& guest) {
    m->hostRules = host;
    m->guestRules = guest;
}
void BattleHUD::setCardOpacity(GLubyte left, GLubyte right) {
    m->host.alpha = left;
    m->guest.alpha = right;
    m->host.applyAlpha();
    m->guest.applyAlpha();
}
// 카드 투명도 설정 키. 값이 없으면 이전 시즌 키를 대신 읽는다.
static char const* cardOpacityKey(bool own) {
    return own ? "hud-own-opacity" : "hud-opponent-opacity";
}
int battleCardOpacity(bool own) {
    auto* mod = Mod::get();
    char const* key = cardOpacityKey(own);
    if (!mod->hasSavedValue(key)) key = own ? "s2-hud-own-opacity" : "s2-hud-opponent-opacity";
    auto const value = mod->getSavedValue<int64_t>(key, 100);
    return static_cast<int>(std::clamp<int64_t>(value, 0, 100));
}
void setBattleCardOpacity(bool own, int percent) {
    Mod::get()->setSavedValue<int64_t>(cardOpacityKey(own), std::clamp(percent, 0, 100));
}
// 퍼센트 값을 0~255 알파로 변환한다.
GLubyte battleCardAlpha(bool own) {
    return static_cast<GLubyte>(std::lround(battleCardOpacity(own) * 2.55));
}
int battleTextOpacity() {
    auto const value = Mod::get()->getSavedValue<int64_t>("hud-text-opacity", 100);
    return static_cast<int>(std::clamp<int64_t>(value, 0, 100));
}
void setBattleTextOpacity(int percent) {
    Mod::get()->setSavedValue<int64_t>("hud-text-opacity", std::clamp(percent, 0, 100));
}
GLubyte battleTextAlpha() {
    return static_cast<GLubyte>(std::lround(battleTextOpacity() * 2.55));
}
void BattleHUD::setHacks(std::string const& left, std::string const& right) {
    m->host.setHacks(left);
    m->guest.setHacks(right);
}
void BattleHUD::showReroll(bool left) {
    (left ? m->host : m->guest).addReroll();
}
// 매 프레임 라벨 애니메이션과 투명도를 갱신한다.
void BattleHUD::update(float dt) {
    m->host.animate(dt);
    m->guest.animate(dt);
    m->host.applyAlpha();
    m->guest.applyAlpha();
}

namespace {
// 그룹 순위 항목: 좌석, 도달 퍼센트, 남은 목숨, 탈락 여부, 내 자리 여부.
struct GroupStanding {
    size_t seat;
    int reached, lives;
    bool out, mine;
};
// 그룹 순위를 계산한다. 내 상태는 로컬 값을 쓰고, 탈락하지 않은 쪽 -> 도달 퍼센트 -> 남은 목숨 순으로 정렬한다.
// alive에는 아직 끝나지 않은 생존자 수를 돌려준다.
std::vector<GroupStanding> groupStandings(BattlePlayerState const& self, GroupBattle const& battle,
                                          std::string const& selfUid, int attempts, int& alive) {
    std::vector<GroupStanding> entries;
    entries.reserve(battle.seats.size());
    alive = 0;
    for (size_t i = 0; i < battle.seats.size(); ++i) {
        auto const& seat = battle.seats[i];
        bool const mine = seat.profile.uid == selfUid;
        auto const& state = mine ? self : seat.state;
        int const lives = std::max(0, attempts - state.attemptsUsed);
        bool const out = state.forfeited || (!state.cleared && lives == 0);
        if (!out && !state.cleared) ++alive;
        int const reached =
            state.cleared ? 100
                          : std::clamp(std::max(state.bestPercent, state.inAttempt ? state.currentPercent : 0), 0, 100);
        entries.push_back({i, reached, out ? 0 : lives, state.forfeited, mine});
    }
    std::stable_sort(entries.begin(), entries.end(), [](GroupStanding const& a, GroupStanding const& b) {
        if (a.out != b.out) return !a.out;
        if (a.reached != b.reached) return a.reached > b.reached;
        return a.lives > b.lives;
    });
    return entries;
}
// 순위를 1ST, 2ND 같은 서수 문구로 바꾼다.
std::string placeText(int place) {
    int const tens = place % 100;
    char const* suffix = tens >= 11 && tens <= 13 ? "TH"
                         : place % 10 == 1        ? "ST"
                         : place % 10 == 2        ? "ND"
                         : place % 10 == 3        ? "RD"
                                                  : "TH";
    return fmt::format("{}{}", place, suffix);
}
// 선두가 바뀌어도 카드를 바로 바꾸지 않는 최소 유지 시간(초).
constexpr float kLeaderSwitchSeconds = .75f;
}

// GroupHUD 내부 상태: 내 카드, 선두 카드, 헤더, 투명도, 선두 교체 쿨다운.
struct GroupHUD::Impl {
    GameRules rules;
    Card self;
    Card leader;
    std::string leaderUid;
    float switchCooldown = 0.f;
    float width = 0.f;
    CCLabelBMFont* header = nullptr;
    std::string headerKey;
    GLubyte ownAlpha = 255, otherAlpha = 255;
};

GroupHUD::GroupHUD() : m(std::make_unique<Impl>()) {}
GroupHUD::~GroupHUD() = default;

GroupHUD* GroupHUD::create(PlayerProfile const& self, GameRules const& rules) {
    auto* node = new GroupHUD;
    if (node->init(self, rules)) {
        node->autorelease();
        return node;
    }
    delete node;
    return nullptr;
}

// 내 카드와 순위 헤더를 만든다. 선두 카드는 첫 갱신 때 만든다.
bool GroupHUD::init(PlayerProfile const& self, GameRules const& rules) {
    if (!CCNode::init()) return false;
    m->rules = rules;
    auto const window = CCDirector::sharedDirector()->getWinSize();
    setContentSize(window);
    setID("group-cards"_spr);
    m->width = std::min(205.f, (window.width - 116.f) / 2.f);
    m->self.create(this, self, rules, battleCardOrigin(kPartOwn, window, m->width), m->width, false);
    m->self.root->setScale(battlePlacement(kPartOwn).scale / 100.f);
    m->header = createCosmeticLabel("", "bigFont.fnt");
    m->header->setAnchorPoint({1.f, .5f});
    m->header->setScale(.24f);
    m->header->setColor(ccc3(255, 220, 120));
    m->header->setPosition({window.width - 8.f, window.height - 77.f});
    addChild(m->header, 2);
    scheduleUpdate();
    return true;
}

// 내 카드를 갱신하고, 나를 제외한 1위를 선두 카드에 보여 준다.
// 선두가 바뀌면 쿨다운 동안은 기존 선두를 유지해 카드가 깜빡이지 않게 하고, 교체할 때 카드를 다시 만든다.
void GroupHUD::updateGroup(BattlePlayerState const& self, GroupBattle const& battle, std::string const& selfUid) {
    m->self.update(self, m->rules);
    int alive = 0;
    auto const entries = groupStandings(self, battle, selfUid, m->rules.attempts, alive);
    auto best = std::find_if(entries.begin(), entries.end(), [](GroupStanding const& e) { return !e.mine; });
    if (best == entries.end()) {
        if (m->leader.root) {
            m->leader.root->setVisible(false);
        }
        return;
    }
    auto shown = best;
    if (!m->leaderUid.empty() && battle.seats[best->seat].profile.uid != m->leaderUid && m->switchCooldown > 0.f) {
        auto current = std::find_if(entries.begin(), entries.end(), [&](GroupStanding const& e) {
            return battle.seats[e.seat].profile.uid == m->leaderUid;
        });
        if (current != entries.end()) shown = current;
    }
    auto const& seat = battle.seats[shown->seat];
    if (seat.profile.uid != m->leaderUid || !m->leader.root) {
        if (m->leader.root) m->leader.root->removeFromParent();
        m->leader = Card{};
        auto const window = getContentSize();
        auto const origin = battleCardOrigin(kPartOpponent, window, m->width);
        auto const place = battlePlacement(kPartOpponent);
        float const scale = place.scale / 100.f;
        m->leader.create(this, seat.profile, m->rules, origin, m->width, true);
        m->leader.root->setScale(scale);
        bool const topRight = place.anchor == kAnchorDefault || place.anchor == kAnchorTopRight;
        m->header->setScale(.24f * scale);
        m->header->setPosition(
            {std::clamp(origin.x + m->width * scale - 4.f + (topRight ? kPauseReserve : 0.f), 8.f, window.width - 4.f),
             std::max(6.f, origin.y - 7.f * scale)});
        if (m->leader.log) {
            m->leader.log->removeFromParent();
            m->leader.log = nullptr;
        }
        m->leader.alpha = m->otherAlpha;
        m->leaderUid = seat.profile.uid;
        m->switchCooldown = kLeaderSwitchSeconds;
    }
    m->leader.root->setVisible(true);
    m->leader.update(seat.state, m->rules);
    int const place = static_cast<int>(shown - entries.begin()) + 1;
    // 헤더: 선두의 순위와 생존자 수(ALIVE n / 전체).
    auto header = fmt::format("{}{}  -  ALIVE {} / {}", seat.disqualified ? "DQ  " : "", placeText(place), alive,
                              battle.seats.size());
    if (header != m->headerKey) {
        m->headerKey = header;
        m->header->setString(header.c_str());
    }
}

// 내 카드와 선두 카드의 투명도를 설정한다.
void GroupHUD::setCardOpacity(GLubyte own, GLubyte other) {
    m->ownAlpha = own;
    m->otherAlpha = other;
    m->self.alpha = own;
    m->leader.alpha = other;
    if (m->header) m->header->setOpacity(other);
    m->self.applyAlpha();
    m->leader.applyAlpha();
}

// 카드 애니메이션과 투명도를 갱신하고 선두 교체 쿨다운을 줄인다.
void GroupHUD::update(float dt) {
    m->self.animate(dt);
    if (m->leader.root) m->leader.animate(dt);
    m->self.applyAlpha();
    m->leader.applyAlpha();
    m->switchCooldown = std::max(0.f, m->switchCooldown - dt);
}
}
