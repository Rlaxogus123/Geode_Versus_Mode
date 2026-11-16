// 1.0.8: 결과 화면의 유저 코인 수 배지 (CoinBadge.hpp).
#include "CoinBadge.hpp"
#include "Particles.hpp"
#include <algorithm>

using namespace geode::prelude;
namespace versus {
namespace {
constexpr float kCoinScale = .45f;           // 코인 아이콘 기본 배율
constexpr unsigned kForever = 100'000;       // CCSequence 안에서 끝없이 반복하는 횟수 (RepeatForever는 못 넣음)
constexpr ccColor3B kGlow = {255, 214, 90};  // 빛 색 (금색)
}

bool coinsDecided(BattlePlayerState const& a, BattlePlayerState const& b, GameRules const& rules, bool draw) {
    constexpr int64_t kPauseBudget = 20'000;
    if (draw || rules.mode != 0 || a.userCoins == b.userCoins || a.attemptsUsed != b.attemptsUsed) return false;
    for (auto const* seat : {&a, &b})
        if (seat->forfeited || !seat->integrityCode.empty() || seat->pauseUsedMs >= kPauseBudget - 50) return false;
    if (a.cleared && b.cleared) return true;
    return !a.cleared && !b.cleared && !a.inAttempt && !b.inAttempt && a.bestPercent == b.bestPercent;
}

namespace {
std::string coinText(int coins, int total) {
    int const shown = std::clamp(coins, 0, 99);
    return total > 0 ? fmt::format("{}/{}", shown, std::min(total, 99)) : std::to_string(shown);
}
}

float coinBadgeWidth(int coins, int total) {
    auto* value = CCLabelBMFont::create(coinText(coins, total).c_str(), "goldFont.fnt");
    auto* coin = CCSprite::createWithSpriteFrameName("usercoin_small01_001.png");
    return (coin ? coin->getContentSize().width * kCoinScale : 0.f) + 3.f + value->getContentSize().width * .5f;
}

CCNode* coinBadge(int coins, int total, float scale, bool decisive, float delay, float grow) {
    auto* root = CCNode::create();
    root->setID("coin-badge"_spr);
    auto* body = CCNodeRGBA::create();
    body->setCascadeOpacityEnabled(true);
    root->addChild(body, 2);
    auto* coin = CCSprite::createWithSpriteFrameName("usercoin_small01_001.png");
    auto* value = CCLabelBMFont::create(coinText(coins, total).c_str(), "goldFont.fnt");
    value->setScale(.5f);
    float const coinWidth = coin ? coin->getContentSize().width * kCoinScale : 0.f;
    float const width = coinWidth + 3.f + value->getScaledContentSize().width;
    CCPoint const coinAt{-width / 2.f + coinWidth / 2.f, 0.f};
    if (coin) {
        coin->setScale(kCoinScale);
        coin->setPosition(coinAt);
        body->addChild(coin, 2);
    }
    value->setAnchorPoint({0.f, .5f});
    value->setPosition({-width / 2.f + coinWidth + 3.f, 1.f});
    body->addChild(value, 2);
    if (coins <= 0) body->setOpacity(150);  // (못 먹었으면 흐리게)
    root->setScale(scale);
    if (!decisive) return root;
    // 코인 수로 이긴 쪽: delay 뒤 크게 튀어나와 천천히 숨쉬고, 뒤의 금빛 코인이 맥동하며, 반짝이가 계속 튄다.
    float const big = scale * grow;
    root->runAction(CCSequence::create(
        CCDelayTime::create(delay), CCEaseBackOut::create(CCScaleTo::create(.35f, big)),
        CCRepeat::create(CCSequence::create(CCEaseSineInOut::create(CCScaleTo::create(.6f, big * 1.07f)),
                                            CCEaseSineInOut::create(CCScaleTo::create(.6f, big)), nullptr),
                         kForever),
        nullptr));
    if (coin) {
        auto* glow = CCSprite::createWithSpriteFrameName("usercoin_small01_001.png");
        glow->setPosition(coinAt);
        glow->setScale(kCoinScale * 1.7f);
        glow->setColor(kGlow);
        glow->setBlendFunc({GL_SRC_ALPHA, GL_ONE});
        glow->setOpacity(0);
        root->addChild(glow, 1);
        auto* pulse = CCSequence::create(
            CCSpawn::create(CCEaseSineInOut::create(CCFadeTo::create(.55f, 70)),
                            CCEaseSineInOut::create(CCScaleTo::create(.55f, kCoinScale * 1.45f)), nullptr),
            CCSpawn::create(CCEaseSineInOut::create(CCFadeTo::create(.55f, 190)),
                            CCEaseSineInOut::create(CCScaleTo::create(.55f, kCoinScale * 1.85f)), nullptr),
            nullptr);
        glow->runAction(CCSequence::create(CCDelayTime::create(delay), CCFadeTo::create(.25f, 170),
                                           CCRepeat::create(pulse, kForever), nullptr));
    }
    if (auto* sparkle = particleSystem("glitterEffectIcon.plist")) {
        sparkle->setEmitterMode(kCCParticleModeGravity);
        sparkle->setPositionType(kCCPositionTypeGrouped);
        sparkle->setSourcePosition({0.f, 0.f});
        sparkle->setPosition(coinAt);
        sparkle->setPosVar({6.f, 6.f});
        sparkle->setTotalParticles(24);
        sparkle->setDuration(-1.f);
        sparkle->setEmissionRate(9.f);
        sparkle->setLife(.7f);
        sparkle->setLifeVar(.2f);
        sparkle->setAngle(90.f);
        sparkle->setAngleVar(180.f);
        sparkle->setSpeed(18.f);
        sparkle->setSpeedVar(8.f);
        sparkle->setGravity({0.f, -10.f});
        sparkle->setStartSize(5.f);
        sparkle->setStartSizeVar(2.f);
        sparkle->setEndSize(0.f);
        sparkle->setEndSizeVar(0.f);
        sparkle->setStartColor({1.f, .9f, .5f, 1.f});
        sparkle->setStartColorVar({0.f, 0.f, 0.f, 0.f});
        sparkle->setEndColor({1.f, .8f, .3f, 0.f});
        sparkle->setEndColorVar({0.f, 0.f, 0.f, 0.f});
        sparkle->setBlendAdditive(true);
        root->addChild(sparkle, 3);
        sparkle->stopSystem();
        sparkle->runAction(
            CCSequence::create(CCDelayTime::create(delay), CallFuncExt::create([sparkle] { sparkle->resetSystem(); }),
                               nullptr));
    }
    return root;
}
}
