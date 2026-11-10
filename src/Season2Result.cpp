// Season 2 매치 결과 연출 화면 (ResultLayer). 두 플레이어의 최고 진행도 게이지,
// 승패 발표, 승자의 execution 연출, 마지막 MATCH RESULT 카드(AGAIN / CLOSE)를 보여 준다.
// randomBotModel()로 가짜 결과를 만들어 연출을 시험하는 용도(Result Test)로 쓰인다.
#include "Season2Result.hpp"
#include "Particles.hpp"
#include "Cosmetics.hpp"
#include "Edition.hpp"
#include "ExecutionEffects.hpp"
#include "KeyDefer.hpp"
#include "MapRecord.hpp"
#include "MenuCursor.hpp"
#include "VersusAudio.hpp"
#include <Geode/binding/CCCircleWave.hpp>
#include <Geode/binding/FMODAudioEngine.hpp>
#include <Geode/binding/GameManager.hpp>
#include <Geode/binding/SimplePlayer.hpp>
#include <algorithm>
#include <cmath>
#include <random>

using namespace geode::prelude;
namespace versus::s2result {
namespace {
// 연출 타임라인(초): 게이지 채우기(kProgressTime) -> 승패 발표(kDecisionTime)
// -> execution 연출(kExecutionTime) -> 결과 카드 확인 대기(kConfirmTime).
// kRevealScale은 앞 두 단계의 재생 속도를 늘리는 배율이다.
constexpr float kRevealScale = 1.3f;
constexpr float kProgressTime = 1.4f * kRevealScale;
constexpr float kDecisionTime = .875f * kRevealScale;
constexpr float kSummaryTime = kProgressTime + kDecisionTime;
constexpr float kExecutionTime = 4.2f;
constexpr float kConfirmTime = 20.f;
constexpr float kConfirmStart = kSummaryTime + kExecutionTime;
// kPlayerX: 화면 중앙에서 각 플레이어까지의 가로 거리, kGaugeWidth: 게이지 폭.
// kOverPriority: 이 레이어가 다른 UI보다 먼저 터치를 받도록 하는 터치 우선순위.
constexpr float kPlayerX = 95.f, kGaugeWidth = 145.f;
constexpr int kOverPriority = -500;

// 난수 생성기와 [low, high] 범위 정수 선택 도우미.
std::mt19937& rng() {
    static std::mt19937 value{std::random_device{}()};
    return value;
}
int pick(int low, int high) {
    return std::uniform_int_distribution<int>(low, std::max(low, high))(rng());
}
// 0xRRGGBB 정수 <-> ccColor3B 변환.
ccColor3B colour(int rgb) {
    return ccc3((rgb >> 16) & 255, (rgb >> 8) & 255, rgb & 255);
}
int packed(ccColor3B value) {
    return value.r << 16 | value.g << 8 | value.b;
}

// 프로필의 아이콘 / 색 / 멤버십 오라를 적용한 SimplePlayer를 만든다 (아이콘 번호는 보유 범위로 제한).
SimplePlayer* icon(PlayerProfile const& profile, float scale) {
    auto* manager = GameManager::sharedState();
    int const count = manager ? std::max(1, manager->countForType(IconType::Cube)) : 1;
    auto* result = SimplePlayer::create(std::clamp(profile.icon, 1, count));
    applyIconLook(result, profile.color1, profile.color2, profile.glow);
    addMembershipAura(result, profile.membership, profile.cosmetics);
    result->setScale(scale);
    return result;
}

// preset 파티클을 at 위치에서 사방으로 한 번 터뜨린다. 끝나면 스스로 제거된다.
void burst(CCNode* parent, CCPoint at, char const* preset, ccColor3B tint, unsigned count, float duration, float life,
           float speed, float size) {
    auto* particles = versus::particleSystem(preset);
    if (!particles) return;
    particles->setTotalParticles(count);
    particles->setEmitterMode(kCCParticleModeGravity);
    particles->setPositionType(kCCPositionTypeGrouped);
    particles->setSourcePosition({0.f, 0.f});
    particles->setPosition(at);
    particles->setPosVar({4.f, 4.f});
    particles->setDuration(duration);
    particles->setLife(life);
    particles->setLifeVar(life * .25f);
    particles->setEmissionRate(static_cast<float>(count) / duration);
    particles->setAngle(90.f);
    particles->setAngleVar(180.f);
    particles->setSpeed(speed);
    particles->setSpeedVar(speed * .3f);
    particles->setGravity({0.f, -35.f});
    particles->setStartSize(size);
    particles->setStartSizeVar(size * .3f);
    particles->setEndSize(0.f);
    particles->setEndSizeVar(0.f);
    particles->setStartColor({tint.r / 255.f, tint.g / 255.f, tint.b / 255.f, 1.f});
    particles->setStartColorVar({.08f, .08f, .08f, 0.f});
    particles->setEndColor({tint.r / 255.f, tint.g / 255.f, tint.b / 255.f, 0.f});
    particles->setEndColorVar({0.f, 0.f, 0.f, 0.f});
    particles->setBlendAdditive(true);
    particles->setAutoRemoveOnFinish(true);
    parent->addChild(particles, 16);
    particles->resetSystem();
}
// 승자 아이콘 위치에 원형 파동과 폭발 / 반짝이 파티클을 낸다 (승자 색을 밝게 섞어 사용).
void winnerEffect(CCNode* parent, CCPoint at, PlayerProfile const& player) {
    auto tint = colour(player.color1);
    tint = ccc3((tint.r + 255) / 2, (tint.g + 255) / 2, (tint.b + 255) / 2);
    if (auto* wave = CCCircleWave::create(8.f, 66.f, .4f, false, true)) {
        wave->m_color = tint;
        wave->setPosition(at);
        parent->addChild(wave, 15);
    }
    burst(parent, at, "explodeEffect.plist", tint, 52, .06f, .5f, 105.f, 5.f);
    burst(parent, at, "glitterEffectIcon.plist", ccc3(255, 228, 135), 20, .16f, .65f, 45.f, 10.f);
}

// 결과 화면 레이어. update()가 경과 시간으로 단계를 진행한다:
// 게이지 -> 승패 발표 -> execution 연출 -> 결과 카드(확인 대기) -> 자동 닫힘.
class ResultLayer final : public CCLayer {
    Model m_model;
    std::function<void()> m_closed;
    std::function<Model()> m_again;
    CCSize m_window;
    float m_elapsed = 0.f, m_shownLeft = 0.f, m_shownRight = 0.f, m_timerWidth = 0.f;
    bool m_revealed = false, m_executed = false, m_confirming = false, m_closing = false;
    CCNodeRGBA* m_progress = nullptr;
    CCNodeRGBA* m_decision = nullptr;
    CCNodeRGBA* m_actors = nullptr;
    CCNodeRGBA* m_popup = nullptr;
    CCLayerColor* m_curtain = nullptr;
    CCLayerColor* m_leftGauge = nullptr;
    CCLayerColor* m_rightGauge = nullptr;
    CCLayerColor* m_timer = nullptr;
    CCLabelBMFont* m_leftPercent = nullptr;
    CCLabelBMFont* m_rightPercent = nullptr;
    CCLabelBMFont* m_footer = nullptr;
    CCMenu* m_menu = nullptr;

    PlayerProfile const& winner() const { return m_model.leftWon ? m_model.left.profile : m_model.right.profile; }
    // 투명하게 시작하는 구획 노드. 자식 불투명도가 함께 적용되어 한 번에 페이드할 수 있다.
    CCNodeRGBA* section(int z) {
        auto* node = CCNodeRGBA::create();
        node->setCascadeOpacityEnabled(true);
        node->setOpacity(0);
        addChild(node, z);
        return node;
    }
    // 화면 중앙 기준 (x, y) 위치에 라벨을 놓는다. 너비가 width를 넘으면 배율을 줄인다.
    CCLabelBMFont* text(CCNode* parent, std::string const& value, float x, float y, float width, float scale,
                        char const* font = "chatFont.fnt", int z = 5) {
        auto* label = createCosmeticLabel(value.c_str(), font);
        label->setScale(std::min(scale, width / std::max(1.f, label->getContentSize().width)));
        label->setPosition({m_window.width / 2.f + x, m_window.height / 2.f + y});
        parent->addChild(label, z);
        return label;
    }
    // 게이지 트랙(어두운 막대)을 만들고, 그 안에서 늘어날 채움 막대를 반환한다.
    CCLayerColor* gauge(float x, ccColor4B tint) {
        auto* track = CCLayerColor::create(ccc4(28, 49, 67, 255));
        track->setContentSize({kGaugeWidth, 7.f});
        track->setPosition({m_window.width / 2.f + x - kGaugeWidth / 2.f, m_window.height / 2.f - 99.f});
        m_progress->addChild(track, 5);
        auto* fill = CCLayerColor::create(tint);
        fill->setContentSize({0.f, 7.f});
        track->addChild(fill);
        return fill;
    }
    // 화면 구성. 진행도 구획(아이콘, 이름, 퍼센트, 게이지, 이전 기록), 승패 발표 구획,
    // execution 연출용 구획, 결과 카드, 남은 시간 막대를 만들고 결과 음악을 시작한다.
    bool init(Model model, std::function<void()> closed, std::function<Model()> again) {
        if (!CCLayer::init()) return false;
        m_model = std::move(model);
        m_closed = std::move(closed);
        m_again = std::move(again);
        m_window = CCDirector::sharedDirector()->getWinSize();
        setID("season2-result"_spr);
        keepMenuCursorVisible(this);
        auto const& left = m_model.left;
        auto const& right = m_model.right;
        bool const draw = m_model.draw;
        addChild(CCLayerColor::create(ccc4(5, 20, 50, 175)));
        m_progress = section(5);
        m_curtain = CCLayerColor::create(ccc4(0, 0, 0, 0));
        addChild(m_curtain, 10);
        m_decision = CCNodeRGBA::create();
        m_decision->setCascadeOpacityEnabled(true);
        m_decision->setOpacity(0);
        m_progress->addChild(m_decision, 15);
        m_actors = section(20);
        // 승패 발표: 제목 ("<이름> WINS!" / DRAW), 승패 문구, 사유(기권 등).
        auto* title = createCosmeticLabel(draw ? "DRAW" : (winner().name + " WINS!").c_str(), "bigFont.fnt");
        title->setPosition({m_window.width / 2.f, m_window.height / 2.f + 94.f});
        title->limitLabelWidth(m_window.width - 40.f, .75f, .3f);
        if (!draw) styleNickname(title, winner().membership, winner().cosmetics);
        m_decision->addChild(title, 10);
        text(m_decision,
             draw              ? "Equal result"
             : m_model.leftWon ? "You won!"
                               : "You lost",
             0.f, 65.f, m_window.width - 40.f, .95f);
        if (!draw && !m_model.reason.empty()) text(m_decision, m_model.reason, 0.f, 36.f, m_window.width - 40.f, .75f);
        // 게이지 구획의 양쪽 플레이어 아이콘, 이름, 퍼센트. 승자(무승부면 양쪽)의 퍼센트만 밝은 색.
        auto* leftIcon = icon(left.profile, 1.8f);
        auto* rightIcon = icon(right.profile, 1.8f);
        rightIcon->setFlipX(true);
        leftIcon->setPosition({m_window.width / 2.f - kPlayerX, m_window.height / 2.f - 10.f});
        rightIcon->setPosition({m_window.width / 2.f + kPlayerX, m_window.height / 2.f - 10.f});
        m_progress->addChild(leftIcon);
        m_progress->addChild(rightIcon);
        styleNickname(text(m_progress, left.profile.name, -kPlayerX, -56.f, 170.f, .5f, "bigFont.fnt"),
                      left.profile.membership, left.profile.cosmetics);
        styleNickname(text(m_progress, right.profile.name, kPlayerX, -56.f, 170.f, .5f, "bigFont.fnt"),
                      right.profile.membership, right.profile.cosmetics);
        m_leftPercent = text(m_progress, "0%", -kPlayerX, -79.f, 140.f, .5f, "bigFont.fnt");
        m_rightPercent = text(m_progress, "0%", kPlayerX, -79.f, 140.f, .5f, "bigFont.fnt");
        m_leftPercent->setColor(draw || m_model.leftWon ? ccc3(179, 237, 255) : ccc3(235, 235, 235));
        m_rightPercent->setColor(draw || !m_model.leftWon ? ccc3(179, 237, 255) : ccc3(235, 235, 235));
        text(m_progress, "VS", 0.f, -79.f, 35.f, .55f, "bigFont.fnt");
        m_leftGauge = gauge(-kPlayerX, ccc4(80, 205, 255, 255));
        m_rightGauge = gauge(kPlayerX, ccc4(255, 139, 178, 255));
        // 각 플레이어의 이전 기록(before)이 있으면 게이지 아래에 표시한다.
        for (auto [x, before] : {std::pair{-kPlayerX, left.before}, std::pair{kPlayerX, right.before}}) {
            auto [record, tint] = mapRecordText(before);
            if (!record.empty()) text(m_progress, record, x, -113.f, 170.f, .55f)->setColor(tint);
        }
        m_footer = text(this, "", 0.f, -133.f, m_window.width - 40.f, .65f);
        m_footer->setZOrder(40);
        // execution 연출 전용 아이콘 쌍. 평소엔 숨겨 두었다가 연출 단계에서 보여 준다.
        auto* a = icon(left.profile, 1.8f);
        auto* b = icon(right.profile, 1.8f);
        b->setFlipX(true);
        m_actors->setContentSize(m_window);
        m_actors->setAnchorPoint({.5f, .5f});
        m_actors->setPosition(m_window / 2.f);
        m_actors->setVisible(false);
        a->setPosition({m_window.width / 2.f - kPlayerX, m_window.height / 2.f - 10.f});
        b->setPosition({m_window.width / 2.f + kPlayerX, m_window.height / 2.f - 10.f});
        m_actors->addChild(a);
        m_actors->addChild(b);
        // 승자의 execution 선택(cosmetics, 멤버십, seed로 결정)을 승자 -> 패자 방향으로 재생한다.
        // 무승부이면 연출이 없다.
        if (!draw) {
            int const choice = resolveExecution(winner().cosmetics.execution, winner().membership, m_model.seed);
            execution::play(m_actors, m_model.leftWon ? a : b, m_model.leftWon ? b : a, choice, kSummaryTime + .55f);
        }
        buildCard();
        // 화면 아래쪽의 남은 시간 막대 (update()에서 현재 단계의 남은 비율만큼 줄어든다).
        m_timerWidth = std::min(440.f, m_window.width - 48.f);
        auto* track = CCLayerColor::create(ccc4(255, 255, 255, 48));
        track->setContentSize({m_timerWidth, 2.f});
        track->setPosition({(m_window.width - m_timerWidth) / 2.f, 8.f});
        addChild(track, 60);
        m_timer = CCLayerColor::create(ccc4(255, 255, 255, 240));
        m_timer->setContentSize({m_timerWidth, 2.f});
        m_timer->setPosition(track->getPosition());
        addChild(m_timer, 61);
        m_progress->runAction(CCFadeIn::create(.42f));
        setTouchEnabled(true);
        setKeypadEnabled(true);
        // 화면 음악을 멈추고 결과 효과음 / 결과 음악을 재생한다.
        if (auto* engine = FMODAudioEngine::sharedEngine()) engine->pauseMusic(0);
        audio::play(audio::Cue::Finish);
        audio::startResultMusic();
        scheduleUpdate();
        return true;
    }
    // 마지막 MATCH RESULT 카드 (처음엔 숨김). 승자 아이콘을 키우고 패자를 흐리게 하며,
    // AGAIN 콜백이 있을 때만 AGAIN 버튼을 CLOSE와 함께 보여 준다.
    void buildCard() {
        auto const& left = m_model.left;
        auto const& right = m_model.right;
        bool const draw = m_model.draw;
        m_popup = CCNodeRGBA::create();
        m_popup->setCascadeOpacityEnabled(true);
        m_popup->setOpacity(0);
        m_popup->setVisible(false);
        m_popup->setID("season2-result-card"_spr);
        float const panelW = std::min(390.f, m_window.width - 24.f), panelH = std::min(190.f, m_window.height - 24.f);
        auto* panel = CCLayerColor::create(ccc4(8, 29, 56, 248));
        panel->setContentSize({panelW, panelH});
        panel->setPosition({(m_window.width - panelW) / 2.f, (m_window.height - panelH) / 2.f});
        m_popup->addChild(panel);
        auto line = [this](std::string const& value, float x, float y, float width, float scale,
                           char const* font = "bigFont.fnt") {
            auto* label = createCosmeticLabel(value.c_str(), font);
            label->setPosition({m_window.width / 2.f + x, m_window.height / 2.f + y});
            label->limitLabelWidth(width, scale, .2f);
            m_popup->addChild(label, 2);
            return label;
        };
        line("MATCH RESULT", 0.f, 76.f, panelW - 24.f, .48f);
        auto* title = line(draw ? "DRAW" : winner().name + " WINS!", 0.f, 55.f, panelW - 24.f, .55f);
        if (!draw) styleNickname(title, winner().membership, winner().cosmetics);
        constexpr float playerX = 82.f;
        auto* leftIcon = icon(left.profile, !draw && m_model.leftWon ? 1.35f : 1.05f);
        auto* rightIcon = icon(right.profile, !draw && !m_model.leftWon ? 1.35f : 1.05f);
        rightIcon->setFlipX(true);
        leftIcon->setPosition({m_window.width / 2.f - playerX, m_window.height / 2.f + 10.f});
        rightIcon->setPosition({m_window.width / 2.f + playerX, m_window.height / 2.f + 10.f});
        if (!draw && !m_model.leftWon) leftIcon->setOpacity(115);
        if (!draw && m_model.leftWon) rightIcon->setOpacity(115);
        m_popup->addChild(leftIcon, 3);
        m_popup->addChild(rightIcon, 3);
        line("VS", 0.f, 8.f, 38.f, .42f);
        styleNickname(line(left.profile.name, -playerX, -25.f, 125.f, .36f), left.profile.membership,
                      left.profile.cosmetics);
        styleNickname(line(right.profile.name, playerX, -25.f, 125.f, .36f), right.profile.membership,
                      right.profile.cosmetics);
        auto* leftScore = line(fmt::format("{}%", left.best), -playerX, -44.f, 100.f, .42f);
        auto* rightScore = line(fmt::format("{}%", right.best), playerX, -44.f, 100.f, .42f);
        if (draw || m_model.leftWon) leftScore->setColor(ccc3(179, 237, 255));
        if (draw || !m_model.leftWon) rightScore->setColor(ccc3(179, 237, 255));
        for (auto [x, before] : {std::pair{-playerX, left.before}, std::pair{playerX, right.before}}) {
            auto [record, tint] = mapRecordText(before);
            if (!record.empty()) line(record, x, -58.f, 150.f, .5f, "chatFont.fnt")->setColor(tint);
        }
        m_menu = CCMenu::create();
        m_menu->setPosition({m_window.width / 2.f, m_window.height / 2.f - 78.f});
        auto add = [this](char const* label, char const* frame, SEL_MenuHandler handler, float x) {
            auto* sprite = ButtonSprite::create(label, "goldFont.fnt", frame);
            sprite->setScale(.72f);
            auto* item = CCMenuItemSpriteExtra::create(sprite, this, handler);
            item->setPositionX(x);
            m_menu->addChild(item);
        };
        if (m_again) {
            add("AGAIN", "GJ_button_02.png", menu_selector(ResultLayer::onAgain), -55.f);
            add("CLOSE", "GJ_button_01.png", menu_selector(ResultLayer::onClose), 55.f);
        } else
            add("CLOSE", "GJ_button_01.png", menu_selector(ResultLayer::onClose), 0.f);
        m_popup->addChild(m_menu, 3);
        addChild(m_popup, 50);
    }
    // 매 프레임 호출. 경과 시간 m_elapsed로 단계를 나눈다:
    // 1) kProgressTime까지 퍼센트 / 게이지를 목표값으로 수렴시킨다.
    // 2) 그 시점에 승패 발표를 띄우고(한 번), kSummaryTime 이후 execution 연출을 보여 준다.
    // 3) kConfirmStart 이후 결과 카드를 띄우고 남은 시간이 0이 되면 닫는다.
    void update(float dt) override {
        if (m_closing) return;
        m_elapsed += dt;
        float const t = m_elapsed;
        float remaining = t < kSummaryTime    ? 1.f - t / kSummaryTime
                          : t < kConfirmStart ? 1.f - (t - kSummaryTime) / kExecutionTime
                                              : 1.f - (t - kConfirmStart) / kConfirmTime;
        m_timer->setContentSize({m_timerWidth * std::clamp(remaining, 0.f, 1.f), 2.f});
        float const ease = std::clamp(-std::expm1(-8.f * std::max(0.f, dt) / kRevealScale), 0.f, 1.f);
        auto settle = [&](float& shown, int target) {
            shown = std::lerp(shown, static_cast<float>(target), ease);
            if (t >= kProgressTime || std::abs(shown - target) < .01f) shown = static_cast<float>(target);
        };
        settle(m_shownLeft, m_model.left.best);
        settle(m_shownRight, m_model.right.best);
        for (auto [label, shown] : {std::pair{m_leftPercent, m_shownLeft}, std::pair{m_rightPercent, m_shownRight}}) {
            auto const value = fmt::format("{}%", static_cast<int>(std::round(shown)));
            if (value != label->getString()) {
                label->setString(value.c_str());
                label->limitLabelWidth(140.f, .5f, .2f);
            }
        }
        m_leftGauge->setContentSize({kGaugeWidth * m_shownLeft / 100.f, 7.f});
        m_rightGauge->setContentSize({kGaugeWidth * m_shownRight / 100.f, 7.f});
        if (t < kProgressTime) return;
        if (!m_revealed) {
            m_revealed = true;
            m_curtain->runAction(CCFadeTo::create(.21f * kRevealScale, 65));
            m_decision->runAction(CCFadeIn::create(.385f * kRevealScale));
            if (!m_model.draw)
                winnerEffect(
                    this,
                    {m_window.width / 2.f + (m_model.leftWon ? -kPlayerX : kPlayerX), m_window.height / 2.f - 10.f},
                    winner());
        }
        if (t < kSummaryTime) return;
        if (t < kConfirmStart) {
            if (!m_executed) {
                m_executed = true;
                if (!m_model.draw) {
                    m_progress->runAction(CCSequence::create(CCFadeOut::create(.22f), CCHide::create(), nullptr));
                    m_actors->setVisible(true);
                    m_actors->setOpacity(0);
                    m_actors->runAction(CCSequence::create(CCDelayTime::create(.22f), CCFadeIn::create(.35f), nullptr));
                }
            }
            return;
        }
        if (!m_confirming) {
            m_confirming = true;
            m_actors->runAction(CCSequence::create(CCFadeOut::create(.28f), CCHide::create(), nullptr));
            m_progress->runAction(CCSequence::create(CCFadeOut::create(.28f), CCHide::create(), nullptr));
            m_curtain->runAction(CCFadeTo::create(.35f, 120));
            m_popup->setVisible(true);
            m_popup->runAction(CCSequence::create(CCDelayTime::create(.35f), CCFadeIn::create(.7f), nullptr));
        }
        int const seconds = std::max(0, static_cast<int>(std::ceil(kConfirmStart + kConfirmTime - t)));
        auto const footer = fmt::format("Result Test  |  closes in {}s", seconds);
        if (footer != m_footer->getString()) m_footer->setString(footer.c_str());
        if (seconds <= 0) close();
    }
    // 레이어를 닫는다. 결과 음악을 멈추고(screenMusic이면 원래 화면 음악 재개),
    // 다음 메인 스레드 틱에 노드를 제거한 뒤 closed 콜백을 부른다.
    void close(bool screenMusic = true) {
        if (m_closing) return;
        m_closing = true;
        unscheduleUpdate();
        setVisible(false);
        audio::stopResultMusic();
        if (screenMusic)
            if (auto* engine = FMODAudioEngine::sharedEngine()) engine->resumeMusic(0);
        Loader::get()->queueInMainThread([keep = Ref<ResultLayer>(this), closed = std::move(m_closed)] {
            keep->removeFromParentAndCleanup(true);
            if (closed) closed();
        });
    }
    void onClose(CCObject*) {
        if (m_confirming) close();
    }
    // AGAIN: 이 레이어를 닫고(음악은 이어서 재생) 다음 틱에 again()이 주는 새 모델로
    // 같은 부모에 결과 화면을 다시 띄운다. 부모가 이미 사라졌으면 음악만 재개하고 closed를 부른다.
    void onAgain(CCObject*) {
        if (!m_confirming || m_closing || !m_again || !getParent()) return;
        auto closed = std::move(m_closed);
        auto again = m_again;
        Ref<CCNode> parent(getParent());
        close(false);
        Loader::get()->queueInMainThread([parent, closed = std::move(closed), again = std::move(again)]() mutable {
            if (parent->isRunning()) {
                show(parent, again(), std::move(closed), std::move(again));
                return;
            }
            if (auto* engine = FMODAudioEngine::sharedEngine()) engine->resumeMusic(0);
            if (closed) closed();
        });
    }

public:
    static ResultLayer* create(Model model, std::function<void()> closed, std::function<Model()> again) {
        auto* result = new ResultLayer;
        if (result->init(std::move(model), std::move(closed), std::move(again))) {
            result->autorelease();
            return result;
        }
        delete result;
        return nullptr;
    }
    // 화면에 들어오면 다음 틱에 버튼 메뉴의 터치 우선순위를 레이어보다 높인다.
    void onEnter() override {
        CCLayer::onEnter();
        Loader::get()->queueInMainThread([self = Ref<ResultLayer>(this)] { self->raiseMenu(); });
    }
    void raiseMenu() {
        if (!m_menu || m_closing) return;
        auto* dispatcher = CCDirector::sharedDirector()->getTouchDispatcher();
        if (dispatcher && dispatcher->findHandler(m_menu)) dispatcher->setPriority(kOverPriority - 1, m_menu);
    }
    void onExit() override {
        if (!m_closing) {
            m_closing = true;
            audio::stopResultMusic();
            if (auto* engine = FMODAudioEngine::sharedEngine()) engine->resumeMusic(0);
        }
        CCLayer::onExit();
    }
    // 모든 터치를 이 레이어가 가장 먼저 받아 삼킨다 (보이는 동안).
    void registerWithTouchDispatcher() override {
        CCDirector::sharedDirector()->getTouchDispatcher()->addTargetedDelegate(this, kOverPriority, true);
    }
    bool ccTouchBegan(CCTouch*, CCEvent*) override { return isVisible(); }
    // 뒤로 가기 키: 결과 카드가 떠 있으면 닫고, 연출 중이면 카드 단계로 건너뛴다.
    void keyBackClicked() override {
        deferBack(this, [this] {
            if (m_confirming) {
                close();
                return;
            }
            m_elapsed = std::max(m_elapsed, kConfirmStart);
            m_actors->cleanup();
            m_actors->setVisible(false);
        });
    }
};
}

// 시험용 무작위 결과 모델: 상대는 가짜 봇 (무작위 아이콘 / 색 / 오라 / execution).
// 무승부 10%, 내 승리 45%, 패배 45%이며 승자 진행도가 더 높게 정해진다.
Model randomBotModel() {
    Model model;
    model.left.profile = Service::get().profile();
    if (model.left.profile.name.empty()) model.left.profile.name = "You";
    PlayerProfile bot;
    bot.uid = "season2-result-test-bot";
    bot.name = fmt::format("Bot {}", pick(100, 999));
    auto* manager = GameManager::sharedState();
    bot.icon = pick(1, manager ? std::max(1, manager->countForType(IconType::Cube)) : 1);
    int const first = pick(0, 106), second = (first + pick(1, 106)) % 107;
    bot.color1 = manager ? packed(manager->colorForIdx(first)) : pick(0, 0xFFFFFF);
    bot.color2 = manager ? packed(manager->colorForIdx(second)) : 0xFFFFFF;
    bot.membership = pick(0, 1) == 1;
    bot.cosmetics = defaultCosmetics(bot.membership);
    do bot.cosmetics.aura = pick(0, kAuraCount - 1);
    while (!canUseAura(bot.cosmetics.aura, bot.membership));
    do bot.cosmetics.execution = pick(1, kExecutionCount - 1);
    while (!canUseExecution(bot.cosmetics.execution, bot.membership));
    bot.hasCosmetics = true;
    model.right.profile = std::move(bot);
    model.lives = pick(3, 12);
    int const roll = pick(0, 99);
    model.draw = roll < 10;
    model.leftWon = !model.draw && roll < 55;
    auto& won = model.leftWon ? model.left : model.right;
    auto& lost = model.leftWon ? model.right : model.left;
    if (model.draw) {
        model.left.best = model.right.best = pick(3, 99);
    } else {
        won.best = pick(0, 2) == 0 ? 100 : pick(15, 99);
        lost.best = pick(0, won.best - 1);
    }
    for (auto* side : {&model.left, &model.right}) {
        side->attemptsUsed = side->best >= 100 ? pick(1, model.lives) : model.lives;
        int const kind = pick(0, 2);
        side->before = kind == 0 ? -1 : kind == 1 ? pick(0, 99) : 100;
    }
    if (!model.draw && pick(0, 9) == 0) model.reason = model.leftWon ? "Opponent forfeited" : "Match forfeited";
    model.seed = fmt::format("result-test-{}", pick(0, 1'000'000));
    return model;
}

// parent 위에 결과 화면을 띄우고 반환한다. closed는 닫힌 뒤 호출,
// again이 있으면 카드에 AGAIN 버튼이 생기고 누를 때마다 새 모델을 받아 다시 보여 준다.
CCNode* show(CCNode* parent, Model const& model, std::function<void()> closed, std::function<Model()> again) {
    if (!parent) return nullptr;
    auto* layer = ResultLayer::create(model, std::move(closed), std::move(again));
    if (layer) parent->addChild(layer, 1000);
    return layer;
}
}
