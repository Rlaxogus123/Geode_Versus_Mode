// Season 2 랭크 매치 연출 모음 (VersusUI.cpp에 #include되는 조각): 매치 발견 인트로(Season2Intro),
// lounge로 돌아오는 낙하 연출(Season2Return), 맵 밴 보드(Season2BanBoard).
// s2match: 연출 공통 상수 / 이징 / 도우미 (칼 쥐는 위치 비율, 칼 길이, 양쪽 방 사이 간격 등).
// sideOf는 서버가 준 side가 없으면 uid 사전순으로 -1(왼쪽) / 1(오른쪽)을 정한다.
namespace s2match {
constexpr float kSwordGrip = .134f;
constexpr float kSwordLength = 34.f * 1.2f;
constexpr float kRoomGap = 60.f;
inline float expoOut(float t) {
    t = std::clamp(t, 0.f, 1.f);
    return t >= 1.f ? 1.f : 1.f - std::pow(2.f, -10.f * t);
}
inline float sineOut(float t) {
    return std::sin(std::clamp(t, 0.f, 1.f) * 1.5707963f);
}
inline std::set<std::string>& introduced() {
    static std::set<std::string> ids;
    return ids;
}
inline bool& fromBattle() {
    static bool value = false;
    return value;
}
inline std::string& arriving() {
    static std::string id;
    return id;
}
inline int sideOf(RankedMatch const& match) {
    if (match.side != 0) return match.side;
    return Service::get().profile().uid < match.opponent.player.uid ? -1 : 1;
}
}

// 듀얼 / 인트로용 칼 노드. s2-sword.png가 없으면 다각형으로 직접 그린 칼로 대신한다 (length는 전체 길이).
CCNodeRGBA* season2Sword(float length) {
    auto* holder = CCNodeRGBA::create();
    holder->setCascadeOpacityEnabled(true);
    holder->setCascadeColorEnabled(true);
    if (auto* sprite = art::artSprite("s2-sword.png", length)) {
        sprite->setAnchorPoint({.5f, s2match::kSwordGrip});
        holder->addChild(sprite);
        return holder;
    }
    auto* draw = CCDrawNode::create();
    float const u = length / 288.f;
    auto at = [u](float x, float y) { return CCPoint{(x - 48.f) * u, (236.f - y) * u}; };
    ccColor4F const ink{.11f, .06f, .19f, 1.f};
    CCPoint blade[] = {at(48, 8), at(64, 42), at(64, 198), at(32, 198), at(32, 42)};
    draw->drawPolygon(blade, 5, {.84f, .89f, .94f, 1.f}, 6.f * u, ink);
    CCPoint grip[] = {at(38, 214), at(58, 214), at(58, 258), at(38, 258)};
    draw->drawPolygon(grip, 4, {.48f, .26f, .13f, 1.f}, 6.f * u, ink);
    CCPoint guard[] = {at(8, 194), at(88, 194), at(88, 218), at(8, 218)};
    draw->drawPolygon(guard, 4, {1.f, .8f, .2f, 1.f}, 6.f * u, ink);
    draw->drawDot(at(48, 267), 21.f * u, ink);
    draw->drawDot(at(48, 267), 15.f * u, {1.f, .8f, .2f, 1.f});
    holder->addChild(draw);
    return holder;
}

// "MATCH FOUND!" 인트로 연출. lounge 안의 내 아이콘에게 느낌표가 뜨고 칼을 뽑는 사이,
// 상대 아이콘이 화면을 가로질러 돌진해 칼이 부딪친다 (불꽃, 번개, 충격파, 화면 흔들림).
// 이어서 화면이 어두워진 사이 내 아이콘을 매치 방 위치로 옮기고, 좌우로 갈라 세운 뒤 끝낸다.
// 각 단계의 시각(초)은 아래 상수이며 update()가 경과 시간으로 진행한다.
class Season2Intro final : public CCNode {
public:
    static constexpr float kZoomTime = .9f, kMarkHide = 1.05f;
    static constexpr float kSwordAt = .12f, kSwordTime = .7f;
    static constexpr float kDashAt = 1.3f, kDashTime = .36f;
    static constexpr float kShake = .3f, kFlash = .4f, kKick = .3f;
    static constexpr float kGripX = -17.f, kDrawFrom = -180.f;
    static constexpr float kHold = -10.f, kStruck = -113.f, kStrikeTime = .08f / 1.5f, kGhostEvery = .022f;
    static constexpr float kPushTime = .3f, kPushDist = 7.f, kDustEvery = .035f;
    static constexpr float kTitleOut = 2.2f;
    static constexpr float kDarkAt = 2.45f, kDarkTime = .28f;
    static constexpr float kLightAt = 3.15f, kLightTime = .35f;
    static constexpr float kSideAt = 3.65f, kSideTime = .3f, kEnd = 4.1f;

private:
    // m_host: lounge 호스트, m_rival: 돌진하는 상대 프로필, m_side: 방에서 내가 설 쪽(-1 왼쪽 / 1 오른쪽),
    // m_stage: 효과를 얹는 노드, m_done: 끝났을 때 콜백(방에 옮겨졌는지 전달), m_leave: 어두워진 시점에
    // 화면을 떠나야 할 때 대신 실행할 콜백.
    Weak<CCNode> m_host;
    PlayerProfile m_rival;
    int m_side = -1;
    float m_time = 0.f, m_shake = 0.f, m_kick = -1.f, m_strike = -1.f, m_ghostClock = 0.f;
    float m_push = -1.f, m_pushed = 0.f, m_pushHost = 0.f, m_dustClock = 0.f;
    bool m_hit = false, m_moved = false, m_lit = false, m_finished = false, m_dashed = false, m_away = false;
    std::optional<CCPoint> m_room;
    Ref<CCNode> m_stage;
    CCNodeRGBA* m_sword = nullptr;
    CCNode* m_mark = nullptr;
    CCNode* m_dasher = nullptr;
    CCLayerColor* m_dark = nullptr;
    CCLabelBMFont* m_title = nullptr;
    CCNode* m_block = nullptr;
    std::optional<CCPoint> m_blockAt;
    std::function<void(bool)> m_done;
    std::function<void()> m_leave;

    // 제목 라벨(타이틀 그라디언트, 튀어나오는 애니메이션)을 만들고 lounge를 연출 모드로 바꾼 뒤 효과음을 낸다.
    bool init(CCNode* host, PlayerProfile rival, int side, std::function<void(bool)> done, std::string const& title) {
        if (!CCNode::init()) return false;
        setID("season2-match-intro"_spr);
        m_host = host;
        m_rival = std::move(rival);
        m_side = side < 0 ? -1 : 1;
        m_done = std::move(done);
        if (auto* effects = host ? lounge::effects(host) : nullptr) {
            m_stage = CCNode::create();
            effects->addChild(m_stage);
        }
        if (host) lounge::cinematic(host, true);
        auto const win = CCDirector::sharedDirector()->getWinSize();
        m_title = label(this, title, {win.width / 2.f, win.height * .8f}, 1.f, win.width - 60.f, ccWHITE, false,
                        "gjFont46.fnt");
        rankedTitleGradient(m_title);
        float const scale = m_title->getScale() * .95f;
        m_title->setScale(.1f);
        m_title->runAction(CCEaseBackOut::create(CCScaleTo::create(.3f, scale)));
        m_title->setZOrder(10);
        audio::play(audio::Cue::MatchFound);
        scheduleUpdate();
        return true;
    }
    // 카메라 / 연출 모드 / 고정을 원래대로 돌리고 효과 노드를 제거한다.
    void restore() {
        if (auto host = m_host.lock()) {
            lounge::camera(host.data(), 1.f, {}, 0.f, {});
            lounge::cinematic(host.data(), false);
            lounge::hold(host.data(), false);
        }
        if (m_stage) {
            m_stage->removeFromParent();
            m_stage = nullptr;
        }
        m_sword = nullptr;
        m_mark = nullptr;
        m_dasher = nullptr;
        m_block = nullptr;
    }
    // 내 아이콘 발밑에 파란 투명 블록(밀려나지 않게 버티는 연출용 장치)을 그린다.
    static constexpr float kFeet = 15.f;
    void makeBlock(CCNode* host, CCPoint spot) {
        constexpr float kSize = 30.f;
        CCPoint top{spot.x, spot.y - kFeet};
        lounge::Pose pose;
        if (lounge::pose(host, pose)) {
            m_blockAt = CCPoint{pose.x, pose.y - kFeet};
            lounge::toHost(host, *m_blockAt, top);
        }
        m_block = CCNode::create();
        m_block->setID("season2-intro-block"_spr);
        m_block->setPosition(top);
        m_stage->addChild(m_block, 0);
        auto* body = CCDrawNode::create();
        CCPoint square[] = {{-kSize / 2.f, -kSize}, {kSize / 2.f, -kSize}, {kSize / 2.f, 0.f}, {-kSize / 2.f, 0.f}};
        body->drawPolygon(square, 4, {.24f, .56f, 1.f, .3f}, 1.1f, {.62f, .86f, 1.f, .95f});
        CCPoint inner[] = {{-kSize / 2.f + 4.f, -kSize + 4.f},
                           {kSize / 2.f - 4.f, -kSize + 4.f},
                           {kSize / 2.f - 4.f, -4.f},
                           {-kSize / 2.f + 4.f, -4.f}};
        body->drawPolygon(inner, 4, {.4f, .7f, 1.f, .12f}, .6f, {.62f, .86f, 1.f, .45f});
        body->drawSegment({-kSize / 2.f + 1.f, -.6f}, {kSize / 2.f - 1.f, -.6f}, .9f, {1.f, 1.f, 1.f, .95f});
        m_block->addChild(body, 1);
        if (auto* glow = loadedFrameSprite("particle_159_001.png", "GJ_ParticleSheet.plist")) {
            glow->setBlendFunc({GL_SRC_ALPHA, GL_ONE});
            glow->setColor(ccc3(110, 190, 255));
            glow->setOpacity(150);
            glow->setPosition({0.f, -5.f});
            float const size = std::max(1.f, glow->getContentSize().width);
            glow->setScaleX(kSize * 2.f / size);
            glow->setScaleY(kSize * .8f / size);
            m_block->addChild(glow, 0);
            glow->runAction(CCRepeatForever::create(
                CCSequence::create(CCFadeTo::create(.5f, 95), CCFadeTo::create(.5f, 150), nullptr)));
        }
        if (auto* ring = CCCircleWave::create(4.f, 26.f, .3f, false, true)) {
            ring->m_color = ccc3(150, 210, 255);
            ring->setPosition({0.f, -2.f});
            m_block->addChild(ring, 2);
        }
        m_block->setScaleX(.3f);
        m_block->runAction(CCEaseBackOut::create(CCScaleTo::create(.16f, 1.f, 1.f)));
    }
    // 어두운 동안 화면을 떠나야 하면(m_leave) 즉시 그 콜백을 실행하고 인트로를 끝낸다.
    void leave() {
        m_finished = true;
        unscheduleUpdate();
        if (m_dark) m_dark->setOpacity(255);
        if (m_title) {
            m_title->removeFromParent();
            m_title = nullptr;
        }
        auto leave = std::move(m_leave);
        if (leave) leave();
    }
    // 칼이 맞닿는 지점의 좌표를 계산한다.
    static CCPoint contactOf(CCPoint spot) {
        float const reach = s2match::kSwordLength * (1.f - s2match::kSwordGrip) * .55f, angle = kStruck * .0174533f;
        return spot + CCPoint{kGripX + std::sin(angle) * reach, std::cos(angle) * reach};
    }
    // 충돌 지점에서 튀는 불꽃 막대와 중앙 번쩍임.
    void sparks(CCPoint contact) {
        ccColor3B const hot[] = {ccc3(255, 252, 230), ccc3(255, 226, 120), ccc3(255, 176, 60), ccc3(255, 140, 40)};
        for (int i = 0; i < 26; ++i) {
            float const angle = (i < 20 ? fx::rnd(15.f, 165.f) : fx::rnd(200.f, 340.f)) * .0174533f;
            float const reach = fx::rnd(14.f, 38.f), life = fx::rnd(.28f, .5f);
            auto const tint = hot[fx::pick(4)];
            auto* spark =
                CCLayerColor::create(ccc4(tint.r, tint.g, tint.b, 255), fx::rnd(4.f, 9.f), fx::rnd(.9f, 1.6f));
            spark->ignoreAnchorPointForPosition(false);
            spark->setAnchorPoint({0.f, .5f});
            spark->setBlendFunc({GL_SRC_ALPHA, GL_ONE});
            spark->setPosition(contact);
            spark->setRotation(-angle / .0174533f);
            m_stage->addChild(spark, 6);
            CCPoint const out{std::cos(angle) * reach, std::sin(angle) * reach};
            ccBezierConfig arc;
            arc.controlPoint_1 = out * .55f;
            arc.controlPoint_2 = out * .95f;
            arc.endPosition = out + CCPoint{0.f, -fx::rnd(8.f, 18.f)};
            spark->runAction(CCSequence::create(
                CCSpawn::create(
                    CCEaseSineOut::create(CCBezierBy::create(life, arc)), CCScaleTo::create(life, .35f, 1.f),
                    CCSequence::create(CCDelayTime::create(life * .35f), CCFadeOut::create(life * .65f), nullptr),
                    nullptr),
                CCRemoveSelf::create(), nullptr));
        }
        if (auto* glow = loadedFrameSprite("particle_159_001.png", "GJ_ParticleSheet.plist")) {
            glow->setBlendFunc({GL_SRC_ALPHA, GL_ONE});
            glow->setColor(ccc3(255, 245, 210));
            glow->setPosition(contact);
            float const size = std::max(1.f, glow->getContentSize().width);
            glow->setScale(6.f / size);
            m_stage->addChild(glow, 7);
            glow->runAction(
                CCSequence::create(CCSpawn::create(CCEaseExponentialOut::create(CCScaleTo::create(.16f, 20.f / size)),
                                                   CCFadeOut::create(.2f), nullptr),
                                   CCRemoveSelf::create(), nullptr));
        }
    }
    // 번개 연출 도우미: 번쩍이는 원, 한 줄기 번개 세그먼트(굵기 3겹), 그리고 충돌 지점에서
    // 방사형으로 갈라지는 번개 다발과 그 가지.
    CCSprite* boltGlow(CCNode* parent, CCPoint at, float size, ccColor3B tint, GLubyte opacity) {
        auto* glow = loadedFrameSprite("particle_159_001.png", "GJ_ParticleSheet.plist");
        if (!glow) return nullptr;
        glow->setBlendFunc({GL_SRC_ALPHA, GL_ONE});
        glow->setColor(tint);
        glow->setOpacity(opacity);
        glow->setPosition(at);
        glow->setScale(size / std::max(1.f, glow->getContentSize().width));
        parent->addChild(glow);
        return glow;
    }
    static void boltSegment(CCDrawNode* bolts, CCPoint a, CCPoint b, float fade) {
        bolts->drawSegment(a, b, 5.f * fade, {.16f, .4f, 1.f, .2f * fade});
        bolts->drawSegment(a, b, 2.4f * fade, {.35f, .62f, 1.f, .5f * fade});
        bolts->drawSegment(a, b, .8f * fade, {1.f, 1.f, 1.f, fade});
    }
    void lightning(CCPoint contact) {
        auto* bolts = CCDrawNode::create();
        bolts->setBlendFunc({GL_SRC_ALPHA, GL_ONE});
        ccColor3B const blue = ccc3(70, 140, 255), pale = ccc3(150, 200, 255);
        int const count = 5;
        for (int b = 0; b < count; ++b) {
            float const heading = (b * 360.f / count + fx::rnd(-25.f, 25.f) + 45.f) * .0174533f;
            float const length = fx::rnd(32.f, 56.f);
            CCPoint const along{std::cos(heading), std::sin(heading)}, across{-along.y, along.x};
            std::vector<CCPoint> points{contact};
            int const steps = 7;
            for (int s = 1; s <= steps; ++s) {
                float const d = length * s / steps,
                            jag = s == steps ? 0.f : fx::rnd(-5.f, 5.f) * (1.f - .5f * s / steps);
                points.push_back(contact + along * d + across * jag);
            }
            for (size_t s = 1; s < points.size(); ++s)
                boltSegment(bolts, points[s - 1], points[s],
                            1.f - .6f * static_cast<float>(s) / static_cast<float>(points.size()));
            boltGlow(bolts, points[3], fx::rnd(14.f, 20.f), blue, 150);
            boltGlow(bolts, points.back(), fx::rnd(9.f, 13.f), pale, 170);
            if (b % 2 == 0) {
                CCPoint from = points[3];
                float const fork = heading + fx::rnd(.5f, .9f) * (fx::rnd(0.f, 1.f) < .5f ? -1.f : 1.f);
                CCPoint const way{std::cos(fork), std::sin(fork)};
                for (int s = 1; s <= 3; ++s) {
                    CCPoint const to = from + way * 6.f + CCPoint{-way.y, way.x} * fx::rnd(-3.f, 3.f);
                    boltSegment(bolts, from, to, .65f);
                    from = to;
                }
            }
        }
        boltGlow(bolts, contact, 54.f, blue, 210);
        boltGlow(bolts, contact, 26.f, ccc3(255, 255, 255), 230);
        m_stage->addChild(bolts, 8);
        if (auto* bloom = boltGlow(m_stage, contact, 40.f, pale, 220)) {
            bloom->setZOrder(7);
            float const scale = bloom->getScale();
            bloom->runAction(
                CCSequence::create(CCSpawn::create(CCEaseExponentialOut::create(CCScaleTo::create(.3f, scale * 2.f)),
                                                   CCFadeTo::create(.32f, 0), nullptr),
                                   CCRemoveSelf::create(), nullptr));
        }
        bolts->runAction(CCSequence::create(CCDelayTime::create(.05f), CCHide::create(), CCDelayTime::create(.035f),
                                            CCShow::create(), CCDelayTime::create(.07f), CCRemoveSelf::create(),
                                            nullptr));
    }
    // 칼이 부딪치는 순간: 효과음, 불꽃, 번개, 화면 흔들림, 칼 반동, 밀림 시작, 충격파, 흰 번쩍임을 한꺼번에 낸다.
    void clash(CCPoint spot) {
        m_hit = true;
        audio::play(audio::Cue::SwordImpact);
        audio::fx("fx_ching");
        CCPoint const contact = contactOf(spot);
        sparks(contact);
        lightning(contact);
        m_shake = kShake;
        m_kick = 0.f;
        m_push = 0.f;
        m_dustClock = kDustEvery;
        ripple(contact);
        auto* flash = CCLayerColor::create(ccc4(255, 255, 255, 128));
        addChild(flash, 20);
        flash->runAction(CCSequence::create(CCFadeTo::create(kFlash, 0), CCRemoveSelf::create(), nullptr));
    }
    // 돌진하는 상대의 잔상 하나를 남긴다.
    void ghost(CCPoint at) {
        auto* manager = GameManager::sharedState();
        int const count = manager ? std::max(1, manager->countForType(IconType::Cube)) : 1;
        auto* copy = SimplePlayer::create(std::clamp(m_rival.icon, 1, count));
        if (!copy) return;
        applyIconLook(copy, m_rival.color1, m_rival.color2, m_rival.glow);
        copy->setPosition(at);
        copy->setOpacity(140);
        m_stage->addChild(copy, 2);
        copy->runAction(CCSequence::create(CCFadeTo::create(.26f, 0), CCRemoveSelf::create(), nullptr));
    }
    // 충돌 지점에서 퍼지는 원형 파동 3개 (일부는 지연 시작).
    void ripple(CCPoint contact) {
        struct Wave {
            float from, to, time, delay;
            ccColor3B tint;
        };
        Wave const waves[] = {{4.f, 34.f, .3f, 0.f, ccc3(255, 255, 255)},
                              {6.f, 58.f, .45f, 0.f, ccc3(90, 160, 255)},
                              {8.f, 84.f, .6f, .07f, ccc3(150, 200, 255)}};
        for (auto const& wave : waves) {
            auto add = [stage = m_stage.data(), wave, contact] {
                if (auto* ring = CCCircleWave::create(wave.from, wave.to, wave.time, false, true)) {
                    ring->m_color = wave.tint;
                    ring->setPosition(contact);
                    stage->addChild(ring, 7);
                }
            };
            if (wave.delay <= 0.f)
                add();
            else
                m_stage->runAction(
                    CCSequence::create(CCDelayTime::create(wave.delay), CallFuncExt::create(add), nullptr));
        }
    }
    // 충돌 직후 내 아이콘이 뒤로 밀리는 연출 (lounge::shift로 실제 이동 + 먼지 / 불꽃).
    void push(float dt, CCPoint spot, CCNode* host) {
        m_push += dt;
        float const k = std::clamp(m_push / kPushTime, 0.f, 1.f);
        float const want = kPushDist * s2match::expoOut(k) + std::sin(m_push * 70.f) * 1.5f * (1.f - k);
        if (std::abs(want - m_pushed) > .01f) {
            m_pushHost += lounge::shift(host, want - m_pushed);
            m_pushed = want;
        }
        if (k >= 1.f) return;
        for (m_dustClock += dt; m_dustClock >= kDustEvery; m_dustClock -= kDustEvery) scrape(spot, 1.f - k);
    }
    void scrape(CCPoint spot, float strength) {
        CCPoint const feet = spot + CCPoint{fx::rnd(-8.f, 2.f), -14.f};
        if (auto* dust = loadedFrameSprite("particle_159_001.png", "GJ_ParticleSheet.plist")) {
            dust->setColor(ccc3(214, 200, 182));
            dust->setOpacity(static_cast<GLubyte>(150.f * strength + 40.f));
            dust->setPosition(feet);
            float const size = std::max(1.f, dust->getContentSize().width), start = fx::rnd(4.f, 7.f) / size;
            dust->setScale(start);
            m_stage->addChild(dust, 1);
            float const life = fx::rnd(.32f, .48f);
            dust->runAction(CCSequence::create(
                CCSpawn::create(CCEaseSineOut::create(CCMoveBy::create(life, {-fx::rnd(9.f, 18.f), fx::rnd(2.f, 7.f)})),
                                CCScaleTo::create(life, start * fx::rnd(1.7f, 2.3f)), CCFadeTo::create(life, 0),
                                nullptr),
                CCRemoveSelf::create(), nullptr));
        }
        if (fx::rnd(0.f, 1.f) > strength) return;
        ccColor3B const hot[] = {ccc3(255, 240, 200), ccc3(255, 200, 110), ccc3(255, 160, 70)};
        auto const tint = hot[fx::pick(3)];
        auto* spark = CCLayerColor::create(ccc4(tint.r, tint.g, tint.b, 255), fx::rnd(2.5f, 4.f), .8f);
        spark->ignoreAnchorPointForPosition(false);
        spark->setAnchorPoint({1.f, .5f});
        spark->setBlendFunc({GL_SRC_ALPHA, GL_ONE});
        spark->setPosition(feet + CCPoint{0.f, 1.f});
        spark->setRotation(-fx::rnd(0.f, 18.f));
        m_stage->addChild(spark, 6);
        float const life = fx::rnd(.18f, .3f);
        spark->runAction(CCSequence::create(
            CCSpawn::create(
                CCEaseSineOut::create(CCMoveBy::create(life, {-fx::rnd(14.f, 26.f), fx::rnd(1.f, 5.f)})),
                CCSequence::create(CCDelayTime::create(life * .4f), CCFadeTo::create(life * .6f, 0), nullptr), nullptr),
            CCRemoveSelf::create(), nullptr));
    }
    // 시각 t에 따른 장면 진행: 느낌표 -> 칼 뽑기 -> 상대 돌진 -> 칼이 맞기 직전 내려치기 -> 충돌.
    // spot은 내 아이콘 위치(호스트 좌표).
    void stage(float t, float dt, CCPoint spot, CCNode* host) {
        auto const win = CCDirector::sharedDirector()->getWinSize();
        if (!m_mark && t < kMarkHide) {
            CCNode* mark = loadedFrameSprite("exMark_001.png", "GJ_GameSheet03.plist");
            if (mark)
                mark->setScale(17.f / std::max(1.f, mark->getContentSize().height));
            else
                mark = label(m_stage, "!", {}, .9f, 0.f, ccc3(255, 70, 70), false, "goldFont.fnt");
            if (!mark->getParent()) m_stage->addChild(mark, 4);
            float const scale = mark->getScale();
            mark->setScale(0.f);
            mark->runAction(CCEaseBackOut::create(CCScaleTo::create(.18f, scale)));
            m_mark = mark;
            audio::playBuiltin("counter003.ogg");
        }
        if (m_mark) {
            m_mark->setPosition(spot + CCPoint{-16.f, 30.f});
            if (t >= kMarkHide) {
                m_mark->runAction(CCSequence::create(CCFadeOut::create(.15f), CCRemoveSelf::create(), nullptr));
                m_mark = nullptr;
            }
        }
        if (t >= kSwordAt) {
            if (!m_sword) {
                m_sword = season2Sword(s2match::kSwordLength);
                m_stage->addChild(m_sword, 2);
                audio::fx("fx_unsheathe");
            }
            float const e = s2match::expoOut((t - kSwordAt) / kSwordTime);
            float rotation = kDrawFrom + (kHold - kDrawFrom) * e;
            CCPoint at = spot + CCPoint{kGripX, 0.f} * e;
            if (m_strike >= 0.f) {
                m_strike += dt;
                float const k = std::clamp(m_strike / kStrikeTime, 0.f, 1.f);
                rotation = kHold + (kStruck - kHold) * k * k;
                if (k >= 1.f && !m_hit) clash(spot);
            }
            if (m_kick >= 0.f) {
                m_kick += dt;
                float const k = m_kick < kKick ? std::sin(3.1415927f * m_kick / kKick) : 0.f;
                rotation += 16.f * k;
                at.x += 2.f * k;
            }
            m_sword->setRotation(rotation);
            m_sword->setPosition(at);
            m_sword->setOpacity(static_cast<GLubyte>(255.f * std::min(1.f, e * 1.4f)));
        }
        if (m_push >= 0.f) push(dt, spot, host);
        if (t >= kDashAt && !m_dashed) {
            if (!m_dasher) {
                m_dasher = CCNode::create();
                player(m_dasher, m_rival, {}, 1.f);
                auto* sword = season2Sword(s2match::kSwordLength);
                sword->setRotation(78.f);
                sword->setPosition({10.f, -3.f});
                m_dasher->addChild(sword, 1);
                m_stage->addChild(m_dasher, 3);
                audio::play(audio::Cue::SwordFly);
            }
            float const p = std::clamp((t - kDashAt) / kDashTime, 0.f, 1.f);
            float const from = host->convertToNodeSpace({-50.f, 0.f}).x,
                        to = host->convertToNodeSpace({win.width + 50.f, 0.f}).x;
            float const x = from + (to - from) * p;
            m_dasher->setPosition({x, spot.y});
            m_ghostClock += dt;
            for (; m_ghostClock >= kGhostEvery; m_ghostClock -= kGhostEvery) ghost({x, spot.y});
            float const speed = (to - from) / kDashTime;
            float const meet = contactOf(spot).x - s2match::kSwordLength * .75f;
            if (m_strike < 0.f && !m_hit && x >= meet - speed * kStrikeTime) m_strike = 0.f;
            if (!m_hit && x >= spot.x) clash(spot);
            if (p >= 1.f) {
                m_dasher->removeFromParent();
                m_dasher = nullptr;
                m_dashed = true;
            }
        }
    }
    // 어두워진 시점에 연출 모드를 다시 켜고 내 아이콘을 매치 방 기준점으로 텔레포트한 뒤 방 그룹을 스폰한다.
    void intoRoom() {
        m_moved = true;
        auto host = m_host.lock();
        restore();
        if (!host) return;
        lounge::cinematic(host.data(), true);
        CCPoint room;
        if (lounge::roomAnchor(host.data(), room)) {
            m_room = room;
            lounge::teleport(host.data(), room);
            lounge::spawnRoomGroup(host.data());
        }
    }
    // 방에 옮겨진 뒤 내 아이콘을 m_side 쪽으로 kRoomGap만큼 부드럽게 이동시킨다.
    void step(float t) {
        auto host = m_host.lock();
        if (!host || !m_room) return;
        float const e = s2match::sineOut((t - kSideAt) / kSideTime);
        lounge::Pose pose;
        float const y = lounge::pose(host.data(), pose) ? pose.y : m_room->y;
        lounge::teleport(host.data(), {m_room->x + m_side * s2match::kRoomGap * e, y});
    }
    // 인트로를 끝낸다: 정리 후 숨기고, 다음 틱에 노드를 제거하며 done(방에 옮겨졌는지)을 호출한다.
    void finish() {
        if (m_finished) return;
        m_finished = true;
        unscheduleUpdate();
        restore();
        setVisible(false);
        auto done = std::move(m_done);
        bool const placed = m_room.has_value();
        Loader::get()->queueInMainThread([self = Ref<CCNode>(this)] { self->removeFromParent(); });
        if (done) done(placed);
    }
    // leaveAtDark: 화면이 어두운 시점에 실행할 콜백 지정. lit / moved / placed: 진행 단계 조회. abort: 즉시 중단.
public:
    static Season2Intro* create(CCNode* host, PlayerProfile rival, int side, std::function<void(bool)> done,
                                std::string const& title = "MATCH FOUND!") {
        auto* node = new Season2Intro;
        if (node->init(host, std::move(rival), side, std::move(done), title)) {
            node->autorelease();
            return node;
        }
        delete node;
        return nullptr;
    }
    void leaveAtDark(std::function<void()> leave) { m_leave = std::move(leave); }
    bool lit() const { return m_lit; }
    bool moved() const { return m_moved; }
    bool placed() const { return m_room.has_value(); }
    void abort() {
        if (m_finished) return;
        m_finished = true;
        unscheduleUpdate();
        restore();
        removeFromParent();
    }
    // 화면에서 빠졌다가 돌아오면 연출 모드를 다시 켜고 어두운 단계 이후로 건너뛴다.
    void onExit() override {
        m_away = true;
        restore();
        CCNode::onExit();
    }
    void onEnter() override {
        CCNode::onEnter();
        if (!m_away || m_finished) return;
        m_away = false;
        if (auto host = m_host.lock()) lounge::cinematic(host.data(), true);
        m_time = std::max(m_time, kDarkAt + kDarkTime);
    }
    // 매 프레임: 블록 설치, 줌 / 카메라 / 흔들림 적용, 단계별 stage(), 제목 제거, 암전 오버레이(어두워짐 -> 방 이동 ->
    // 밝아짐), 방 안에서 좌우 배치, kEnd에 종료.
    void update(float dt) override {
        if (m_finished) return;
        dt = std::clamp(dt, 0.f, .1f);
        m_time += dt;
        float const t = m_time;
        auto host = m_host.lock();
        CCPoint spot;
        bool const open = host && lounge::spot(host.data(), spot);
        if (open && m_stage && !m_block && !m_moved) {
            lounge::hold(host.data(), true);
            makeBlock(host.data(), spot);
        }
        if (m_block && m_blockAt && host && !m_moved) {
            CCPoint at;
            if (lounge::toHost(host.data(), *m_blockAt, at)) m_block->setPosition(at);
        }
        if (host && !m_moved) {
            float const e = s2match::expoOut(t / kZoomTime);
            CCPoint shake;
            if (m_shake > 0.f) {
                m_shake = std::max(0.f, m_shake - dt);
                float const k = m_shake / kShake;
                shake = CCPoint{fx::rnd(-7.f, 7.f) * k, fx::rnd(-5.f, 5.f) * k};
            }
            auto const win = CCDirector::sharedDirector()->getWinSize();
            lounge::camera(host.data(), 1.f + e, {win.width / 2.f + m_pushHost * (1.f + e), win.height * .42f}, e,
                           shake);
        }
        if (open && m_stage && !m_moved) stage(t, dt, spot, host.data());
        if (m_title && t >= kTitleOut) {
            m_title->runAction(CCSequence::create(CCFadeOut::create(.25f), CCRemoveSelf::create(), nullptr));
            m_title = nullptr;
        }
        if (t >= kDarkAt && !m_dark) {
            m_dark = CCLayerColor::create(ccc4(0, 0, 0, 0));
            addChild(m_dark, 30);
        }
        if (m_dark) {
            float const black = t < kLightAt ? std::clamp((t - kDarkAt) / kDarkTime, 0.f, 1.f)
                                             : 1.f - std::clamp((t - kLightAt) / kLightTime, 0.f, 1.f);
            m_dark->setOpacity(static_cast<GLubyte>(255.f * black));
        }
        if (t >= kDarkAt + kDarkTime && !m_moved) {
            if (m_leave) {
                leave();
                return;
            }
            intoRoom();
        }
        if (t >= kLightAt) m_lit = true;
        if (t >= kSideAt) step(t);
        if (t >= kEnd) finish();
    }
};

// 매치가 끝나 lounge로 돌아올 때의 낙하 연출. 내 아이콘이 구름(또는 Matrix 모드에서는 금속 발판) 위에 앉았다가
// 뛰어올라 아래로 떨어지며 배경이 위로 스크롤되고, lounge의 내 아이콘 위치를 찾아 착지한 뒤 서서히 사라진다.
// Look::Sky는 하늘 / 구름, Look::Matrix는 어두운 배경 / 글자 비 / 스캔 라인이다 (demon 컵 맵일 때).
class Season2Return final : public CCNode {
public:
    // 연출 단계: Sit(앉아 대기) -> Hop(뛰어오름) -> Fall(낙하) -> Land(착지 접근) -> Fade(사라짐).
    // kSit* 대기, kHop / kGravity / kTerminal 점프와 낙하 물리(화면 유닛/초), kFall* 낙하 시간 범위(초),
    // kSteady*: 목표 위치가 그만큼 가만히 있으면 착지로 판단, kAbsent: lounge가 없을 때 포기하는 시간.
    enum class Look { Sky, Matrix };
    static constexpr float kSit = .6f, kSitMax = 2.5f;
    static constexpr float kHop = 300.f, kGravity = 1400.f, kTerminal = 720.f;
    static constexpr float kFallMin = 1.f, kFallMax = 6.f;
    static constexpr float kSteady = .3f, kSteadyMove = 40.f;
    static constexpr float kAbove = 130.f;
    static constexpr float kSitHeight = .3f, kHoldLow = .2f, kHoldHigh = .4f;
    static constexpr float kFade = .8f, kIconOut = .45f;
    static constexpr float kHalf = 15.f;
    static constexpr float kAbsent = 1.5f;
    static constexpr int kSquashTag = 0x5232;

private:
    enum class Phase { Sit, Hop, Fall, Land, Fade };
    // 배경 요소들: 구름(깊이별 스크롤 속도), 스트릭 선, 글자 비(Matrix).
    struct Cloud {
        CCNodeRGBA* sprite;
        float baseY, depth, drift;
        GLubyte alpha;
    };
    struct Streak {
        CCLayerColor* line;
        float speed;
    };
    struct Rain {
        CCLabelBMFont* column;
        float baseY, depth, speed, fall;
        GLubyte alpha;
        int length;
    };
    Weak<CCNode> m_host;
    Look m_look = Look::Sky;
    Phase m_phase = Phase::Sit;
    float m_clock = 0.f, m_fade = kFade;
    bool m_held = false, m_finished = false;
    CCSize m_win;
    CCLayerGradient* m_sky = nullptr;
    CCSprite* m_sun = nullptr;
    float m_sunBase = 0.f;
    GLubyte m_sunAlpha = 150;
    CCNode* m_back = nullptr;
    CCNode* m_front = nullptr;
    CCNode* m_body = nullptr;
    SimplePlayer* m_icon = nullptr;
    CCNodeRGBA* m_perch = nullptr;
    float m_perchY = 0.f, m_perchDrop = 0.f;
    std::vector<Cloud> m_clouds;
    std::vector<Streak> m_streaks;
    std::vector<Rain> m_rain;
    std::vector<CCLayerColor*> m_shades;
    CCLayerColor* m_scan = nullptr;
    float m_glitch = 0.f, m_scanY = 0.f;
    float m_scroll = 0.f, m_camera = 0.f;
    float m_velocity = 0.f;
    CCPoint m_at;
    float m_sitY = 0.f, m_spin = 0.f, m_scale = 1.f;
    std::optional<CCPoint> m_target;
    float m_targetScale = 1.f, m_targetSpin = 0.f;
    bool m_left = false;
    std::optional<CCPoint> m_seen;
    float m_steady = 0.f;
    CCPoint m_from;
    float m_landTime = .3f, m_fallSpeed = 0.f;
    float m_absent = 0.f;
    float m_fadeIn = 0.f;
    bool m_covered = true;
    std::function<void()> m_onCovered;

    // Matrix 모드의 글자 색과 글자 집합.
    static constexpr ccColor3B kGlyphTail = {10, 70, 30}, kGlyphRed = {40, 225, 90}, kMatrixHead = {255, 255, 255};
    static constexpr ccColor3B kMatrixLine = {198, 200, 208};
    static constexpr char kGlyphs[] = "0123456789ABCDEFXZ<>/|=+*#";

    // 배경 조각 도우미: 구름 스프라이트, 색 막대(CCLayerColor, 원래 알파를 tag에 저장해 페이드),
    // 빛 스프라이트, 그리고 Matrix 모드의 유리판 모양 조각(pane).
    static CCSprite* cloud(int index, float width, ccColor3B tint, GLubyte alpha) {
        static constexpr char const* frames[] = {"d_cloud_01_001.png", "d_cloud_02_001.png", "d_cloud_03_001.png",
                                                 "d_cloud_04_001.png", "d_cloud_05_001.png"};
        auto* sprite = loadedFrameSprite(frames[index % 5], "GJ_GameSheet.plist");
        if (!sprite) sprite = loadedFrameSprite("particle_159_001.png", "GJ_ParticleSheet.plist");
        if (!sprite) return nullptr;
        sprite->setScale(width / std::max(1.f, sprite->getContentSize().width));
        sprite->setColor(tint);
        sprite->setOpacity(alpha);
        return sprite;
    }
    static CCLayerColor* bar(CCNode* parent, ccColor4B colour, CCPoint at, CCSize size) {
        auto* layer = CCLayerColor::create(colour, size.width, size.height);
        layer->setPosition(at);
        layer->setTag(colour.a);
        parent->addChild(layer);
        return layer;
    }
    static void fadeBars(CCNode* node, float k) {
        for (auto* child : CCArrayExt<CCNode*>(node->getChildren()))
            if (auto* layer = typeinfo_cast<CCLayerColor*>(child); layer && layer->getTag() >= 0)
                layer->setOpacity(static_cast<GLubyte>(std::clamp(layer->getTag() * k, 0.f, 255.f)));
    }
    static CCSprite* glow(ccColor3B tint, float width, float height, GLubyte alpha) {
        auto* sprite = loadedFrameSprite("particle_159_001.png", "GJ_ParticleSheet.plist");
        if (!sprite) return nullptr;
        sprite->setBlendFunc({GL_SRC_ALPHA, GL_ONE});
        sprite->setColor(tint);
        sprite->setOpacity(alpha);
        sprite->setScaleX(width / std::max(1.f, sprite->getContentSize().width));
        sprite->setScaleY(height / std::max(1.f, sprite->getContentSize().height));
        return sprite;
    }
    static CCNodeRGBA* pane(float width, ccColor3B tint) {
        float const height = width * fx::rnd(.42f, .7f);
        auto* node = CCNodeRGBA::create();
        node->setCascadeOpacityEnabled(true);
        node->setContentSize({width, height});
        node->setAnchorPoint({.5f, .5f});
        node->ignoreAnchorPointForPosition(false);
        bar(node, ccc4(tint.r, tint.g, tint.b, 26), {0.f, 0.f}, {width, height});
        bar(node, ccc4(tint.r, tint.g, tint.b, 150), {0.f, height - 1.f}, {width, 1.f});
        bar(node, ccc4(tint.r, tint.g, tint.b, 70), {0.f, 0.f}, {width, 1.f});
        bar(node, ccc4(tint.r, tint.g, tint.b, 90), {0.f, height - 7.f}, {width * fx::rnd(.25f, .45f), 4.f});
        for (int line = 0; line < 3; ++line)
            bar(node, ccc4(tint.r, tint.g, tint.b, 55), {width * .08f, height * (.18f + .2f * line)},
                {width * fx::rnd(.3f, .8f), 2.f});
        return node;
    }
    // 모드에 맞는 배경 조각(Sky: 구름, Matrix: 유리판)을 만들어 위치를 잡는다.
    CCNodeRGBA* piece(int index, float width, ccColor3B tint, GLubyte alpha) {
        if (m_look == Look::Matrix) {
            auto* node = pane(width, tint);
            node->setOpacity(alpha);
            fadeBars(node, alpha / 255.f);
            return node;
        }
        auto* sprite = cloud(index, width, tint, alpha);
        if (sprite) sprite->setFlipX(fx::pick(2) == 1);
        return sprite;
    }
    void addCloud(CCNode* layer, int index, float depth, float width, ccColor3B tint, GLubyte alpha, float y) {
        auto* node = piece(index, width, tint, alpha);
        if (!node) return;
        node->setPosition({fx::rnd(-40.f, m_win.width + 40.f), y});
        layer->addChild(node);
        m_clouds.push_back({node, y, depth, fx::rnd(-6.f, 6.f) * depth, alpha});
    }
    // 글자 비 한 줄의 글자를 무작위로 바꾸고 아래(꼬리)에서 위(머리)로 색을 입힌다.
    static void glyphs(Rain& rain) {
        std::string text;
        for (int i = 0; i < rain.length; ++i) {
            if (i) text += '\n';
            text += kGlyphs[fx::pick(static_cast<int>(sizeof(kGlyphs) - 1))];
        }
        rain.column->setString(text.c_str());
        int index = 0;
        auto* children = rain.column->getChildren();
        if (!children) return;
        int const count = static_cast<int>(children->count());
        auto const mix = [](ccColor3B from, ccColor3B to, float t) {
            auto const one = [t](GLubyte a, GLubyte b) {
                return static_cast<GLubyte>(a + (b - a) * std::clamp(t, 0.f, 1.f));
            };
            return ccc3(one(from.r, to.r), one(from.g, to.g), one(from.b, to.b));
        };
        for (auto* glyph : CCArrayExt<CCSprite*>(children)) {
            float const k = count > 1 ? static_cast<float>(index) / (count - 1) : 1.f;
            glyph->setColor(index == count - 1 ? kMatrixHead
                            : k < .65f         ? mix(kGlyphTail, kGlyphRed, k / .65f)
                                               : mix(kGlyphRed, ccc3(228, 255, 234), (k - .65f) / .35f));
            ++index;
        }
    }
    void addRain(float depth, float y) {
        auto* column = CCLabelBMFont::create("0", "chatFont.fnt");
        if (!column) return;
        column->setScale(.32f + .38f * depth);
        column->setAnchorPoint({.5f, 0.f});
        column->setPosition({fx::rnd(8.f, m_win.width - 8.f), y});
        m_back->addChild(column, depth > .8f ? 30 : 5);
        Rain rain{column,
                  y,
                  depth,
                  fx::rnd(40.f, 110.f) * (.6f + depth),
                  0.f,
                  static_cast<GLubyte>(110.f + 145.f * std::clamp((depth - .3f) / .8f, 0.f, 1.f)),
                  6 + fx::pick(10)};
        glyphs(rain);
        column->setOpacity(rain.alpha);
        m_rain.push_back(rain);
    }
    // 하늘 배경(그라디언트, 해, 구름 층, 앉을 구름)과 Matrix 배경(어두운 그라디언트, 글자 비, 금속 발판,
    // 스캔 라인)을 만든다.
    void buildSky() {
        m_sky = CCLayerGradient::create(ccc4(156, 210, 255, 255), ccc4(52, 118, 222, 255), {0.f, 1.f});
        m_sky->setContentSize(m_win);
        addChild(m_sky, 0);
        m_sunBase = m_win.height * .9f;
        if ((m_sun = loadedFrameSprite("particle_159_001.png", "GJ_ParticleSheet.plist"))) {
            m_sun->setBlendFunc({GL_SRC_ALPHA, GL_ONE});
            m_sun->setColor(ccc3(255, 248, 214));
            m_sun->setOpacity(m_sunAlpha);
            m_sun->setScale(m_win.height * .9f / std::max(1.f, m_sun->getContentSize().width));
            m_sun->setPosition({m_win.width * .82f, m_sunBase});
            addChild(m_sun, 1);
        }
        m_back = CCNode::create();
        addChild(m_back, 2);
        m_front = CCNode::create();
        addChild(m_front, 6);
        for (int i = 0; i < 12; ++i) {
            float const depth = fx::rnd(.3f, 1.1f), k = (depth - .3f) / .8f;
            addCloud(m_back, i, depth, 60.f + depth * 120.f,
                     ccc3(static_cast<GLubyte>(205.f + 50.f * k), static_cast<GLubyte>(228.f + 27.f * k), 255),
                     static_cast<GLubyte>(120.f + 90.f * k), fx::rnd(-m_win.height * .9f, m_win.height * 1.05f));
        }
        for (int i = 0; i < 3; ++i)
            addCloud(m_front, i + 2, fx::rnd(1.6f, 2.1f), fx::rnd(260.f, 340.f), ccWHITE, 130,
                     -fx::rnd(120.f, 120.f + m_win.height));
        if (auto* perch = cloud(0, 8.8f * kHalf, ccWHITE, 255)) {
            m_perch = perch;
            m_perchDrop = .22f * perch->getContentSize().height * perch->getScale();
        }
    }
    void buildMatrix() {
        m_sky = CCLayerGradient::create(ccc4(13, 13, 16, 255), ccc4(2, 2, 3, 255), {0.f, 1.f});
        m_sky->setContentSize(m_win);
        addChild(m_sky, 0);
        m_sunBase = -m_win.height * .2f;
        m_sunAlpha = 50;
        if ((m_sun = glow(ccc3(160, 160, 170), m_win.width * 1.3f, m_win.height * .9f, m_sunAlpha))) {
            m_sun->setPosition({m_win.width / 2.f, m_sunBase});
            addChild(m_sun, 1);
        }
        m_back = CCNode::create();
        addChild(m_back, 2);
        m_front = CCNode::create();
        addChild(m_front, 6);
        for (int i = 0; i < 9; ++i) {
            float const depth = fx::rnd(.3f, 1.1f), k = (depth - .3f) / .8f;
            GLubyte const grey = static_cast<GLubyte>(110.f + 80.f * k);
            addCloud(m_back, i, depth, 50.f + depth * 90.f, ccc3(grey, grey, static_cast<GLubyte>(grey + 8)),
                     static_cast<GLubyte>(120.f + 110.f * k), fx::rnd(-m_win.height * .9f, m_win.height * 1.05f));
        }
        for (int i = 0; i < 2; ++i)
            addCloud(m_front, i, fx::rnd(1.6f, 2.1f), fx::rnd(220.f, 300.f), kMatrixLine, 150,
                     -fx::rnd(120.f, 120.f + m_win.height));
        int const columns = std::clamp(static_cast<int>(m_win.width / 16.f), 20, 36);
        for (int i = 0; i < columns; ++i) addRain(fx::rnd(.3f, 1.1f), fx::rnd(-m_win.height * .2f, m_win.height));
        auto* perch = CCNodeRGBA::create();
        perch->setCascadeOpacityEnabled(true);
        float const width = 8.8f * kHalf, height = 10.f;
        perch->setContentSize({width, height});
        perch->setAnchorPoint({.5f, 1.f});
        perch->ignoreAnchorPointForPosition(false);
        if (auto* under = glow(ccc3(170, 170, 180), width * 1.7f, 46.f, 110)) {
            under->setPosition({width / 2.f, -6.f});
            perch->addChild(under, -1);
        }
        bar(perch, ccc4(34, 34, 38, 220), {0.f, 0.f}, {width, height});
        bar(perch, ccc4(kMatrixLine.r, kMatrixLine.g, kMatrixLine.b, 255), {0.f, height - 2.f}, {width, 2.f});
        for (int pin = 0; pin < 5; ++pin)
            bar(perch, ccc4(kMatrixLine.r, kMatrixLine.g, kMatrixLine.b, 90), {width * (.1f + .2f * pin), 1.f},
                {2.f, height - 4.f});
        m_perch = perch;
        m_perchDrop = 0.f;
        auto* top = CCLayerGradient::create(ccc4(0, 0, 0, 0), ccc4(0, 0, 0, 170), {0.f, 1.f});
        top->setContentSize({m_win.width, m_win.height * .3f});
        top->setPosition({0.f, m_win.height * .7f});
        addChild(top, 7);
        auto* low = CCLayerGradient::create(ccc4(0, 0, 0, 170), ccc4(0, 0, 0, 0), {0.f, 1.f});
        low->setContentSize({m_win.width, m_win.height * .3f});
        addChild(low, 7);
        m_scan = CCLayerColor::create(ccc4(kMatrixLine.r, kMatrixLine.g, kMatrixLine.b, 34), m_win.width, 2.f);
        addChild(m_scan, 7);
        m_scanY = m_win.height;
        m_shades = {top, low, m_scan};
    }
    // 배경과 내 아이콘을 만들고 lounge를 연출 모드로 둔다. fadeIn > 0이면 배경이 서서히 나타나
    // 다 덮은 뒤(covered) 콜백을 부른다 (그 사이에 화면 전환 등을 할 수 있다).
    bool init(CCNode* host, Look look, float fadeIn, std::function<void()> covered) {
        if (!CCNode::init() || !host) return false;
        setID("season2-return"_spr);
        m_host = host;
        m_look = look;
        m_win = CCDirector::sharedDirector()->getWinSize();
        setContentSize(m_win);
        if (look == Look::Matrix)
            buildMatrix();
        else
            buildSky();
        for (int i = 0; i < 10; ++i) {
            ccColor3B const tint = look == Look::Matrix ? kMatrixLine : ccWHITE;
            auto* line =
                CCLayerColor::create(ccc4(tint.r, tint.g, tint.b, 255), fx::rnd(1.f, 2.f), fx::rnd(24.f, 64.f));
            line->setOpacity(0);
            line->setPosition({fx::rnd(0.f, m_win.width), fx::rnd(-m_win.height, m_win.height)});
            m_front->addChild(line, 1);
            m_streaks.push_back({line, fx::rnd(1.3f, 2.f)});
        }
        m_sitY = m_win.height * kSitHeight;
        m_at = CCPoint{m_win.width / 2.f, m_sitY};
        if (m_perch) {
            m_perchY = m_sitY - m_perchDrop;
            m_perch->setPosition({m_at.x, m_perchY});
            m_back->addChild(m_perch, 50);
        }
        m_body = CCNode::create();
        addChild(m_body, 4);
        m_icon = player(m_body, Service::get().profile(), {0.f, kHalf}, 1.f);
        if (m_icon) cascadeOpacity(m_icon);
        m_fadeIn = std::max(0.f, fadeIn);
        m_onCovered = std::move(covered);
        m_covered = m_fadeIn <= 0.f;
        if (!m_covered) {
            fadeTo(0.f);
            if (m_icon) m_icon->setOpacity(0);
        }
        lounge::cinematic(host, true);
        m_held = true;
        place();
        scheduleUpdate();
        if (m_covered && m_onCovered) std::exchange(m_onCovered, nullptr)();
        return true;
    }
    // lounge 안의 내 아이콘의 현재 위치 / 크기 / 회전 / 방향을 이 노드 좌표로 구한다.
    // 화면 밖이거나 이동 대기 중이면 실패.
    bool look(CCPoint& feet, float& scale, float& spin, bool& left) {
        auto host = m_host.lock();
        if (!host) return false;
        lounge::Pose pose;
        CCPoint going, centre, side;
        if (!lounge::pose(host.data(), pose) || lounge::pendingMove(host.data(), going) ||
            !lounge::spot(host.data(), centre) || !lounge::toHost(host.data(), {pose.x + 2.f * kHalf, pose.y}, side))
            return false;
        CCPoint const c = convertToNodeSpace(host->convertToWorldSpace(centre));
        CCPoint const s = convertToNodeSpace(host->convertToWorldSpace(side));
        float const size = ccpDistance(c, s) / (2.f * kHalf);
        if (!std::isfinite(size) || size < .25f || size > 4.f || !std::isfinite(c.x) || !std::isfinite(c.y) ||
            c.x < -20.f || c.x > m_win.width + 20.f || c.y < -20.f || c.y > m_win.height + 20.f)
            return false;
        feet = c - CCPoint{0.f, kHalf * size};
        scale = size;
        spin = pose.rotation;
        left = pose.left;
        return true;
    }
    // 낙하 중 목표(lounge의 내 아이콘)를 관찰하고 가만히 있는 시간을 잰다. follow는 마지막 단계에서 그 위치에 겹친다.
    void watch(float dt) {
        CCPoint feet;
        float scale = 1.f, spin = 0.f;
        bool left = false;
        if (!look(feet, scale, spin, left)) {
            m_steady = 0.f;
            m_seen.reset();
            return;
        }
        if (m_seen && ccpDistance(feet, *m_seen) <= kSteadyMove * dt + .5f)
            m_steady += dt;
        else
            m_steady = 0.f;
        m_seen = feet;
        m_target = feet;
        m_targetScale = scale;
        m_targetSpin = spin;
        m_left = left;
    }
    void follow() {
        CCPoint feet;
        float scale = 1.f, spin = 0.f;
        bool left = false;
        if (!look(feet, scale, spin, left)) return;
        m_at = feet;
        m_scale = scale;
        m_spin = spin;
        if (m_icon) m_icon->setScaleX(left ? -1.f : 1.f);
    }
    // built: lounge 레벨이 만들어졌는지. landing: 착지할 위치 (목표 없으면 화면 하단 중앙 근처). turnTo: 최단 회전각.
    bool built() const {
        auto host = m_host.lock();
        return !host || lounge::builds(host.data()) > 0;
    }
    CCPoint landing() const { return m_target ? *m_target : CCPoint{m_win.width / 2.f, m_win.height * .18f}; }
    float turnTo(float to) const {
        float turn = std::fmod(to - m_spin, 360.f);
        if (turn > 180.f) turn -= 360.f;
        if (turn <= -180.f) turn += 360.f;
        return turn == -180.f ? 180.f : turn;
    }
    // hop: 뛰어오르며 늘어나는 연출. land: 착지 접근 단계 시작. touchdown: 착지 시 찌그러짐 + 먼지(Sky) 또는 픽셀(Matrix).
    void hop() {
        m_phase = Phase::Hop;
        m_clock = 0.f;
        m_velocity = kHop;
        if (m_body) {
            m_body->stopAllActions();
            auto* stretch = CCSequence::create(CCScaleTo::create(.05f, m_scale * .9f, m_scale * 1.14f),
                                               CCEaseSineOut::create(CCScaleTo::create(.2f, m_scale)), nullptr);
            stretch->setTag(kSquashTag);
            m_body->runAction(stretch);
        }
    }
    void land() {
        m_phase = Phase::Land;
        m_clock = 0.f;
        m_from = m_at;
        m_fallSpeed = m_camera;
        float const drop = std::abs(m_from.y - landing().y);
        m_landTime = std::clamp(2.f * drop / std::max(200.f, m_camera), .22f, .6f);
        if (m_icon) m_icon->setScaleX(m_left ? -1.f : 1.f);
    }
    void touchdown() {
        m_at = landing();
        if (m_target) {
            m_spin = m_targetSpin;
            m_scale = m_targetScale;
        } else
            m_spin = std::round(m_spin / 90.f) * 90.f;
        m_camera = 0.f;
        m_phase = Phase::Fade;
        m_clock = 0.f;
        if (m_body) {
            m_body->stopAllActions();
            m_body->setScale(m_scale);
            auto* squash = CCSequence::create(CCScaleTo::create(.06f, m_scale * 1.24f, m_scale * .74f),
                                              CCEaseBackOut::create(CCScaleTo::create(.22f, m_scale)), nullptr);
            squash->setTag(kSquashTag);
            m_body->runAction(squash);
        }
        if (m_look == Look::Matrix)
            pixels();
        else
            dust();
        letGo();
    }
    // 착지 효과: 구름 먼지(Sky) / 사각 픽셀과 충격선(Matrix).
    void dust() {
        for (int i = 0; i < 8; ++i) {
            auto* puff = cloud(i, fx::rnd(10.f, 18.f) * m_scale, ccWHITE, 230);
            if (!puff) continue;
            float const side = i % 2 ? 1.f : -1.f, life = fx::rnd(.35f, .55f), size = puff->getScale();
            puff->setPosition(m_at + CCPoint{side * fx::rnd(4.f, 10.f) * m_scale, fx::rnd(1.f, 4.f)});
            m_front->addChild(puff, 3);
            puff->runAction(CCSequence::create(
                CCSpawn::create(CCEaseSineOut::create(
                                    CCMoveBy::create(life, {side * fx::rnd(16.f, 34.f) * m_scale, fx::rnd(3.f, 10.f)})),
                                CCScaleTo::create(life, size * 1.8f), CCFadeOut::create(life), nullptr),
                CCRemoveSelf::create(), nullptr));
        }
    }
    void pixels() {
        for (int i = 0; i < 12; ++i) {
            float const size = fx::rnd(2.f, 4.f) * m_scale, side = i % 2 ? 1.f : -1.f, life = fx::rnd(.3f, .55f);
            auto* bit = CCLayerColor::create(ccc4(kMatrixLine.r, kMatrixLine.g, kMatrixLine.b, 255), size, size);
            bit->ignoreAnchorPointForPosition(false);
            bit->setAnchorPoint({.5f, .5f});
            bit->setPosition(m_at + CCPoint{side * fx::rnd(2.f, 9.f) * m_scale, fx::rnd(1.f, 5.f)});
            m_front->addChild(bit, 3);
            bit->runAction(CCSequence::create(
                CCSpawn::create(CCEaseSineOut::create(CCMoveBy::create(
                                    life, {side * fx::rnd(14.f, 40.f) * m_scale, fx::rnd(4.f, 22.f) * m_scale})),
                                CCFadeOut::create(life), nullptr),
                CCRemoveSelf::create(), nullptr));
        }
        auto* shock = CCLayerColor::create(ccc4(kMatrixHead.r, kMatrixHead.g, kMatrixHead.b, 220), 40.f * m_scale, 2.f);
        shock->ignoreAnchorPointForPosition(false);
        shock->setAnchorPoint({.5f, .5f});
        shock->setPosition(m_at + CCPoint{0.f, 1.f});
        shock->setScaleX(.2f);
        m_front->addChild(shock, 2);
        shock->runAction(
            CCSequence::create(CCSpawn::create(CCEaseExponentialOut::create(CCScaleTo::create(.5f, 3.2f, 1.f)),
                                               CCFadeOut::create(.5f), nullptr),
                               CCRemoveSelf::create(), nullptr));
    }
    // 구름을 낙하 속도에 맞춰 스크롤하고 화면 위로 지나가면 아래쪽으로 재배치한다. 해는 천천히 따라온다.
    void scrollClouds(float dt, bool recycle) {
        for (auto& cloud : m_clouds) {
            auto* sprite = cloud.sprite;
            float const halfW = sprite->getContentSize().width * sprite->getScale() / 2.f;
            float const halfH = sprite->getContentSize().height * sprite->getScale() / 2.f;
            float x = sprite->getPositionX() + cloud.drift * dt;
            if (x < -halfW - 40.f)
                x += m_win.width + 2.f * halfW + 80.f;
            else if (x > m_win.width + halfW + 40.f)
                x -= m_win.width + 2.f * halfW + 80.f;
            float y = cloud.baseY + m_scroll * cloud.depth;
            if (recycle && y - halfH > m_win.height + 10.f) {
                y = -halfH - fx::rnd(10.f, 160.f);
                cloud.baseY = y - m_scroll * cloud.depth;
                x = fx::rnd(-40.f, m_win.width + 40.f);
            }
            sprite->setPosition({x, y});
        }
        if (m_perch) m_perch->setPositionY(m_perchY + m_scroll);
        if (m_sun) m_sun->setPositionY(m_sunBase + m_scroll * .06f);
    }
    // 글자 비를 스크롤 / 낙하시키고 가끔 한 줄을 바꾼다 (글리치). 스캔 라인을 아래로 흘린다.
    void rain(float dt, bool recycle) {
        for (auto& rain : m_rain) {
            rain.fall += rain.speed * dt;
            auto* column = rain.column;
            float const height = column->getContentSize().height * column->getScaleY();
            float y = rain.baseY + m_scroll * rain.depth - rain.fall;
            bool const up = m_camera * rain.depth > rain.speed;
            if (recycle && (up ? y > m_win.height + 10.f : y + height < -10.f)) {
                y = up ? -height - fx::rnd(0.f, 120.f) : m_win.height + fx::rnd(0.f, 80.f);
                rain.baseY = y - m_scroll * rain.depth + rain.fall;
                column->setPositionX(fx::rnd(8.f, m_win.width - 8.f));
                glyphs(rain);
            }
            column->setPositionY(y);
        }
        m_glitch -= dt;
        if (m_glitch <= 0.f && !m_rain.empty()) {
            m_glitch = .07f;
            glyphs(m_rain[static_cast<size_t>(fx::pick(static_cast<int>(m_rain.size())))]);
        }
        if (m_scan) {
            m_scanY -= dt * m_win.height / 2.6f;
            if (m_scanY < -4.f) m_scanY = m_win.height + 40.f;
            m_scan->setPositionY(m_scanY);
        }
    }
    // 빠르게 떨어질 때만 보이는 속도감 선.
    void streaks(float dt) {
        float const k = std::clamp((m_camera - 260.f) / 420.f, 0.f, 1.f);
        for (auto& streak : m_streaks) {
            auto* line = streak.line;
            float y = line->getPositionY() + m_camera * streak.speed * dt;
            if (y > m_win.height) {
                y = -line->getContentSize().height - fx::rnd(0.f, m_win.height * .6f);
                line->setPositionX(fx::rnd(0.f, m_win.width));
            }
            line->setPositionY(y);
            line->setOpacity(static_cast<GLubyte>(110.f * k));
        }
    }
    // 배경 전체의 불투명도를 k(0~1)에 맞춘다.
    void fadeTo(float k) {
        auto const of = [k](float base) { return static_cast<GLubyte>(std::clamp(base * k, 0.f, 255.f)); };
        m_sky->setOpacity(of(255.f));
        if (m_sun) m_sun->setOpacity(of(m_sunAlpha));
        if (m_perch) {
            m_perch->setOpacity(of(255.f));
            if (m_look == Look::Matrix) fadeBars(m_perch, k);
        }
        for (auto& cloud : m_clouds) {
            cloud.sprite->setOpacity(of(cloud.alpha));
            if (m_look == Look::Matrix) fadeBars(cloud.sprite, k * cloud.alpha / 255.f);
        }
        for (auto& rain : m_rain) rain.column->setOpacity(of(rain.alpha));
        for (auto* shade : m_shades) shade->setOpacity(of(shade == m_scan ? 34.f : 255.f));
    }
    // 아이콘을 현재 위치 / 크기 / 회전에 놓는다. letGo: 잡고 있던 lounge 연출 모드를 해제한다.
    void place() {
        if (!m_body) return;
        m_body->setPosition(m_at);
        if (!m_body->getActionByTag(kSquashTag)) m_body->setScale(m_scale);
        if (m_icon) m_icon->setRotation(m_spin);
    }
    void letGo() {
        if (!m_held) return;
        m_held = false;
        if (auto host = m_host.lock()) lounge::cinematic(host.data(), false);
    }
    // 연출을 끝내고 다음 틱에 노드를 제거한다. cover: 배경이 완전히 덮이면 covered 콜백을 호출한다.
    void finish() {
        if (m_finished) return;
        m_finished = true;
        unscheduleUpdate();
        letGo();
        setVisible(false);
        Loader::get()->queueInMainThread([self = Ref<CCNode>(this)] { self->removeFromParent(); });
    }
    void cover() {
        m_covered = true;
        m_clock = 0.f;
        m_target.reset();
        m_seen.reset();
        m_steady = 0.f;
        fadeTo(1.f);
        if (m_icon) m_icon->setOpacity(255);
        if (m_onCovered) std::exchange(m_onCovered, nullptr)();
    }

public:
    static Season2Return* create(CCNode* host, Look look = Look::Sky, float fadeIn = 0.f,
                                 std::function<void()> covered = nullptr) {
        auto* node = new Season2Return;
        if (node->init(host, look, fadeIn, std::move(covered))) {
            node->autorelease();
            return node;
        }
        delete node;
        return nullptr;
    }
    // covering: 아직 화면을 덮고 있는지. skip: 즉시 짧은 페이드로 마무리한다.
    bool covering() const { return !m_finished && m_phase != Phase::Fade; }
    void skip() {
        if (m_finished) return;
        if (!m_covered && m_onCovered) std::exchange(m_onCovered, nullptr)();
        m_covered = true;
        letGo();
        if (m_body) {
            m_body->removeFromParent();
            m_body = nullptr;
            m_icon = nullptr;
        }
        if (m_phase != Phase::Fade) {
            m_phase = Phase::Fade;
            m_clock = 0.f;
            m_fade = .25f;
        }
        m_camera = 0.f;
    }
    void onExit() override {
        finish();
        CCNode::onExit();
    }
    // 매 프레임: (덮는 중이면 페이드 인) 단계별 처리. Sit에서 위아래로 흔들리다 레벨이 만들어지면 Hop,
    // Hop에서 포물선 후 Fall, Fall에서 낙하 속도를 올리며 목표 위치로 서서히 접근하다
    // 목표가 잠잠하거나 시간이 되면 Land, Land에서 착지 보간 후 touchdown, Fade에서 페이드 아웃.
    void update(float dt) override {
        if (m_finished) return;
        dt = std::clamp(dt, 0.f, .05f);
        m_clock += dt;
        if (!m_covered) {
            float const k = std::clamp(m_clock / m_fadeIn, 0.f, 1.f);
            fadeTo(k);
            if (m_icon) m_icon->setOpacity(static_cast<GLubyte>(255.f * k));
            if (k >= 1.f) cover();
            rain(dt, true);
            place();
            return;
        }
        if (m_phase == Phase::Fade)
            follow();
        else
            watch(dt);
        m_absent = lounge::active() ? 0.f : m_absent + dt;
        switch (m_phase) {
        case Phase::Sit: {
            float const bob = std::sin(m_clock * 2.4f) * 2.f;
            m_at.y = m_sitY + bob;
            m_perchY = m_sitY - m_perchDrop + bob;
            if (m_clock >= kSit && (built() || m_absent >= kAbsent || m_clock >= kSitMax)) hop();
            break;
        }
        case Phase::Hop: {
            m_velocity -= kGravity * dt;
            m_at.y += m_velocity * dt;
            m_spin = 180.f * std::clamp(m_clock / (2.f * kHop / kGravity), 0.f, 1.f);
            if (m_velocity < 0.f && m_at.y <= m_sitY) {
                m_at.y = m_sitY;
                m_camera = -m_velocity;
                m_phase = Phase::Fall;
                m_clock = 0.f;
            }
            break;
        }
        case Phase::Fall: {
            m_camera = std::min(kTerminal, m_camera + kGravity * dt);
            m_scroll += m_camera * dt;
            float const ease = std::min(1.f, dt * 2.2f);
            if (m_target) {
                float const hold =
                    std::max(m_target->y + 40.f,
                             std::clamp(m_target->y + kAbove, m_win.height * kHoldLow, m_win.height * kHoldHigh));
                m_at.x += (m_target->x - m_at.x) * ease;
                m_at.y += (hold - m_at.y) * ease;
                m_scale += (m_targetScale - m_scale) * ease;
            }
            float const turned = m_target ? m_targetSpin : std::round(m_spin / 90.f) * 90.f;
            m_spin += turnTo(turned) * std::min(1.f, dt * 4.f);
            if ((m_clock >= kFallMin && (m_steady >= kSteady || m_absent >= kAbsent)) || m_clock >= kFallMax) land();
            break;
        }
        case Phase::Land: {
            float const u = std::clamp(m_clock / m_landTime, 0.f, 1.f);
            CCPoint const to = landing();
            m_at.x = m_from.x + (to.x - m_from.x) * (u * u * (3.f - 2.f * u));
            m_at.y = m_from.y + (to.y - m_from.y) * u * u;
            m_camera = m_fallSpeed * (1.f - u);
            m_scroll += m_camera * dt;
            m_scale += (m_targetScale - m_scale) * std::min(1.f, dt * 8.f);
            m_spin += turnTo(m_target ? m_targetSpin : std::round(m_spin / 90.f) * 90.f) * std::min(1.f, dt * 10.f);
            if (u >= 1.f) touchdown();
            break;
        }
        case Phase::Fade: {
            float const k = std::clamp(m_clock / m_fade, 0.f, 1.f);
            fadeTo(1.f - k);
            float const icon = 1.f - std::clamp((k - (1.f - kIconOut)) / kIconOut, 0.f, 1.f);
            if (m_icon) m_icon->setOpacity(static_cast<GLubyte>(255.f * icon));
            if (k >= 1.f) {
                finish();
                return;
            }
            break;
        }
        }
        scrollClouds(dt, m_phase != Phase::Fade);
        rain(dt, m_phase != Phase::Fade);
        streaks(dt);
        place();
    }
};
// demon 컵 맵(lounge)이면 Matrix, 아니면 Sky 모습을 쓴다.
inline Season2Return::Look season2ReturnLook() {
    return lounge::mapLevel(Service::get().rankedCup()) == lounge::kDemonLoungeLevelID ? Season2Return::Look::Matrix
                                                                                       : Season2Return::Look::Sky;
}

// 밴 보드 상수와 도우미: 카드 크기 / 배치, 선택 연출 시간, 숫자 축약 표기(count), GDDL 티어 색 보간(gddlColor),
// GD 버전 번호의 표기(gdVersion).
namespace s2ban {
constexpr float kCardH = 40.f, kCardGap = 4.f, kHeaderH = 30.f, kTagW = 56.f, kTagH = 19.f;
constexpr float kDateScale = .28f * 1.2f, kRowBottom = 3.f;
constexpr int kMaxCards = 5;
constexpr float kCardScale = .8f, kSlotH = kCardH * kCardScale, kSlotGap = 3.f;
constexpr float kBoardH = kHeaderH + kMaxCards * kSlotH + (kMaxCards - 1) * kSlotGap + 4.f;
constexpr float kRevealSeconds = 1.8f;
constexpr int kSpinRounds = 3;
constexpr float kSpinFirst = .06f, kSpinLast = .34f;
constexpr float kAppear = .3f, kAppearStep = .08f, kAppearTime = .22f;
inline std::string count(int value) {
    if (value >= 1'000'000) return fmt::format("{:.1f}M", value / 1'000'000.);
    if (value >= 10'000) return fmt::format("{}K", value / 1'000);
    if (value >= 1'000) return fmt::format("{:.1f}K", value / 1'000.);
    return std::to_string(value);
}
inline ccColor3B gddlColor(int tier) {
    struct Stop {
        int tier;
        float r, g, b;
    };
    constexpr Stop stops[] = {
        {1, 178, 110, 255}, {11, 212, 66, 214}, {20, 232, 36, 48}, {30, 150, 12, 18}, {39, 66, 0, 4}};
    tier = std::clamp(tier, 1, 39);
    for (size_t i = 1; i < std::size(stops); ++i) {
        auto const& a = stops[i - 1];
        auto const& b = stops[i];
        if (tier > b.tier) continue;
        float const k = static_cast<float>(tier - a.tier) / static_cast<float>(b.tier - a.tier);
        auto mix = [k](float from, float to) { return static_cast<GLubyte>(std::lround(from + (to - from) * k)); };
        return ccc3(mix(a.r, b.r), mix(a.g, b.g), mix(a.b, b.b));
    }
    return ccc3(66, 0, 4);
}
inline constexpr float kGddlBoxW = 24.f, kGddlBoxH = 16.f;
inline std::string gdVersion(int version) {
    if (version >= 18) return fmt::format("GD {}.{}", version / 10, version % 10);
    if (version == 11) return "GD 1.8";
    if (version == 10) return "GD 1.7";
    if (version >= 1 && version <= 7) return fmt::format("GD 1.{}", version - 1);
    return {};
}
}

// 맵 밴 보드. 후보 맵 카드(3~5장)를 보여 주고 제한 시간 안에 한 장을 두 번 눌러 밴한다
// (첫 번째 탭은 BAN? 확인, 두 번째가 전송). 시간이 지나면 서버가 무작위로 밴한다.
// 서로의 밴이 끝나고 선택된 맵이 정해지면 남은 후보들을 룰렛처럼 돌려 결과를 보여 준다.
class Season2BanBoard final : public CCNode {
public:
    using Ban = std::function<void(int64_t)>;

private:
    // 카드 하나의 노드 모음(배경, 태그, 글로우, 맵 정보 라벨)과 현재 표시 상태.
    struct Card {
        LevelInfo level;
        CCMenuItemSpriteExtra* item = nullptr;
        NineSlice* back = nullptr;
        CCLayerColor* edge = nullptr;
        CCLayerColor* shade = nullptr;
        CCDrawNode* cross = nullptr;
        CCLayerColor* glow = nullptr;
        NineSlice* tag = nullptr;
        CCLabelBMFont* tagText = nullptr;
        CCLabelBMFont* who = nullptr;
        CCLabelBMFont* date = nullptr;
        CCLabelBMFont* likes = nullptr;
        CCPoint likesAt;
        CCLabelBMFont* length = nullptr;
        CCNode* lengthClock = nullptr;
        CCPoint lengthAt;
        float lengthWidth = 66.f;
        std::string details, look;
    };
    // m_bans: 서버가 준 밴 현황(내 밴 / 상대 밴 / 종료 시각), m_armed: 확인 대기 중인 카드,
    // m_sending: 전송 중인 카드, m_chosen: 최종 선택된 맵, m_spin: 룰렛 단계별 (시각, 맵) 일정.
    std::vector<Card> m_cards;
    std::map<int64_t, int> m_gddl;
    std::string m_match, m_opponent;
    RankedMatch::Bans m_bans;
    int64_t m_armed = 0, m_sending = 0, m_chosen = 0;
    float m_age = 0.f, m_revealAge = 0.f, m_detailsTimer = 0.f, m_errorAge = 0.f;
    int m_lastSecond = -1;
    std::string m_error;
    Ban m_ban;
    ccColor3B m_plate;
    float m_cardW = 0.f, m_left = 0.f, m_artW = 0.f;
    std::vector<std::pair<float, int64_t>> m_spin;
    float m_spinEnd = 0.f;
    int64_t m_spinAt = 0;
    bool spinning() const { return m_chosen && m_revealAge < m_spinEnd; }
    bool m_cardsIn = false;
    CCLabelBMFont* m_title = nullptr;
    CCLabelBMFont* m_hint = nullptr;
    CCLabelBMFont* m_clock = nullptr;
    CCNode* m_clockIcon = nullptr;
    CCMenu* m_menu = nullptr;

    // find: 맵 ID로 카드 찾기. secondsLeft: 남은 시간(서버 시각 기준, 올림). locked: 더 누를 수 없는 상태.
    Card* find(int64_t id) {
        for (auto& card : m_cards)
            if (card.level.id == id) return &card;
        return nullptr;
    }
    int secondsLeft() const {
        int64_t const left = m_bans.endsAt - Service::get().serverNow();
        return static_cast<int>(std::clamp<int64_t>((left + 999) / 1000, 0, m_bans.seconds));
    }
    bool locked() const { return m_chosen || m_sending || m_bans.own || secondsLeft() <= 0; }
    // 카드 하나를 만든다: 난이도 얼굴, 이름, 제작자, 별 / 코인 / 좋아요, 길이, 투표 배지, BAN 태그.
    void build(LevelInfo const& level, int index, float y) {
        Card card;
        card.level = level;
        float const w = m_artW, h = s2ban::kCardH;
        auto* art = CCSprite::create();
        art->setContentSize({w, h});
        art->setCascadeOpacityEnabled(true);
        card.back = NineSlice::create("square02b_001.png");
        card.back->setContentSize({w, h});
        card.back->setPosition({w / 2.f, h / 2.f});
        card.back->setColor(m_plate);
        card.back->setOpacity(228);
        art->addChild(card.back, 0);
        card.glow = CCLayerColor::create(ccc4(255, 210, 90, 0), w + 6.f, h + 6.f);
        card.glow->setPosition({-3.f, -3.f});
        art->addChild(card.glow, -1);
        card.edge = CCLayerColor::create(ccc4(255, 205, 110, 235), 2.f, h - 14.f);
        card.edge->setPosition({4.f, 7.f});
        art->addChild(card.edge, 1);
        auto* slot = CCDrawNode::create();
        slot->drawDot({21.f, h / 2.f}, 15.5f, {0.f, 0.f, 0.f, .32f});
        art->addChild(slot, 1);
        if (auto* face = mapDifficulty(level)) {
            auto const size = face->getContentSize();
            face->setScale(std::min(26.f / std::max(1.f, size.height), 26.f / std::max(1.f, size.width)));
            face->setPosition({21.f, h / 2.f + 1.f});
            art->addChild(face, 2);
        }
        float const left = 41.f, textW = w - left - s2ban::kTagW - 16.f;
        card.date = label(art, fmt::format("ID: {}", level.id), {w - s2ban::kTagW - 14.f, h - 6.f}, s2ban::kDateScale,
                          textW, kMuted, false, "chatFont.fnt");
        card.date->setAnchorPoint({1.f, .5f});
        label(art, level.name.empty() ? "Unknown map" : level.name, {left, h - 14.f}, .3f, textW * .62f, ccWHITE, true,
              "bigFont.fnt")
            ->setZOrder(2);
        auto* votes = mapVotesBadge(art, level.id, {0.f, 7.f}, 8.f * 1.3f, .5f, 2);
        float const zoneRight = w - s2ban::kTagW - 12.f, zoneW = std::max(84.f, votes ? votes->reserve() + 8.f : 0.f);
        if (votes) {
            votes->setPositionX(zoneRight - zoneW / 2.f);
            mapvotes::sitOn(votes, s2ban::kRowBottom);
        }
        card.lengthAt = CCPoint{zoneRight - zoneW / 2.f, h / 2.f + 3.f};
        card.lengthWidth = zoneW - 18.f;
        if (auto* clock = loadedFrameSprite("GJ_timeIcon_001.png")) {
            clock->setScale(11.f / std::max(1.f, clock->getContentSize().height));
            clock->setID("season2-ban-length-clock");
            art->addChild(clock, 2);
            card.lengthClock = clock;
        }
        card.length = label(art, "", card.lengthAt, .32f, zoneW - 18.f, ccc3(255, 214, 120), false, "goldFont.fnt");
        card.length->setID("season2-ban-length");
        card.length->setZOrder(2);
        showLength(card);
        label(art, "by " + (level.creator.empty() ? std::string("Unknown") : level.creator), {left, h - 23.f}, .26f,
              std::max(40.f, votes ? zoneRight - zoneW - 6.f - left : textW), ccc3(255, 214, 120), true, "goldFont.fnt")
            ->setZOrder(2);
        bool const coins = level.userCoins > 0;
        auto* stars = starRating(art, std::to_string(level.stars), {0.f, 7.f}, .28f, 34.f);
        float const starsW = stars->getScaledContentSize().width;
        stars->setPositionX(left + starsW / 2.f);
        mapvotes::sitOn(stars, s2ban::kRowBottom);
        float starsEnd = left + starsW;
        if (coins) {
            mapCoinCount(art, level, {starsEnd + 18.f, 7.f}, .26f);
            mapvotes::sitOn(art->getChildByID("map-user-coin-count"_spr), s2ban::kRowBottom);
            starsEnd += 34.f;
        }
        card.likesAt = CCPoint{starsEnd + 6.f + 5.5f, 7.f};
        card.likes =
            label(art, "---", {card.likesAt.x + 8.f, card.likesAt.y}, .28f, 34.f, ccWHITE, true, "bigFont.fnt");
        CCPoint const tagAt{w - 8.f - s2ban::kTagW / 2.f, h / 2.f + 4.f};
        card.tag = NineSlice::create("square02b_001.png");
        card.tag->setContentSize({s2ban::kTagW, s2ban::kTagH});
        card.tag->setPosition(tagAt);
        art->addChild(card.tag, 10);
        card.tagText = label(art, "BAN", tagAt, .34f, s2ban::kTagW - 10.f, ccWHITE, false, "bigFont.fnt");
        card.tagText->setZOrder(11);
        card.who = label(art, "", {tagAt.x, 6.f}, .2f, s2ban::kTagW + 8.f, kMuted, false, "bigFont.fnt");
        card.who->setZOrder(11);
        card.shade = CCLayerColor::create(ccc4(0, 0, 0, 0), w, h);
        art->addChild(card.shade, 8);
        card.cross = CCDrawNode::create();
        art->addChild(card.cross, 9);
        card.item = CCMenuItemSpriteExtra::create(art, this, menu_selector(Season2BanBoard::onCard));
        card.item->m_scaleMultiplier = 1.04f;
        card.item->setScale(s2ban::kCardScale);
        card.item->m_baseScale = s2ban::kCardScale;
        card.item->setTag(index);
        card.item->setPosition({m_left + m_cardW / 2.f, y});
        card.item->setID(fmt::format("season2-ban-{}", index));
        card.item->setVisible(false);
        art->setOpacity(0);
        m_menu->addChild(card.item);
        m_cards.push_back(std::move(card));
    }
    // 카드 상태(open, armed, sending, mine, theirs, chosen, spin, wait, out)에 맞춰 태그 문구 / 색,
    // 그림자, 취소선, 글로우를 바꾼다. 상태가 같으면 건너뛴다.
    void paint(Card& card) {
        int64_t const id = card.level.id;
        bool const mine = m_bans.own == id, theirs = m_bans.opponent == id, banned = mine || theirs;
        bool const chosen = m_chosen == id, armed = m_armed == id && !banned, sending = m_sending == id && !banned;
        bool const spin = spinning();
        std::string const look = banned     ? (mine ? "mine" : "theirs")
                                 : spin     ? (m_spinAt == id ? "spin" : "wait")
                                 : chosen   ? "chosen"
                                 : sending  ? "sending"
                                 : armed    ? "armed"
                                 : m_chosen ? "out"
                                            : "open";
        if (look == card.look) return;
        card.look = look;
        card.tag->stopAllActions();
        card.tag->setScale(1.f);
        card.cross->clear();
        card.shade->setOpacity(0);
        card.glow->stopAllActions();
        card.glow->setOpacity(0);
        card.back->setColor(m_plate);
        card.edge->setColor(ccc3(255, 205, 110));
        card.who->setString("");
        auto tag = [&](char const* text, ccColor3B back, ccColor3B ink) {
            card.tagText->setString(text);
            card.tagText->limitLabelWidth(s2ban::kTagW - 10.f, .34f, .18f);
            card.tagText->setColor(ink);
            card.tag->setColor(back);
            card.tag->setOpacity(240);
        };
        if (look == "open")
            tag("BAN", ccc3(120, 28, 28), ccc3(255, 205, 195));
        else if (look == "armed") {
            tag("BAN?", ccc3(230, 40, 40), ccWHITE);
            card.back->setColor(mixColor(m_plate, ccc3(170, 20, 20), .55f));
            card.edge->setColor(ccc3(255, 70, 60));
            card.tag->runAction(CCRepeatForever::create(
                CCSequence::create(CCEaseSineInOut::create(CCScaleTo::create(.3f, 1.12f)),
                                   CCEaseSineInOut::create(CCScaleTo::create(.3f, 1.f)), nullptr)));
        } else if (look == "sending")
            tag("...", ccc3(150, 30, 30), ccWHITE);
        else if (look == "mine" || look == "theirs") {
            tag("BANNED", ccc3(60, 16, 16), ccc3(255, 90, 80));
            bool const random = mine ? m_bans.ownRandom : m_bans.opponentRandom;
            card.who->setString(random ? (mine ? "YOU - RANDOM" : "RANDOM") : mine ? "YOU" : m_opponent.c_str());
            card.who->limitLabelWidth(s2ban::kTagW + 8.f, .2f, .1f);
            card.who->setColor(mine ? ccc3(255, 190, 110) : ccc3(150, 215, 255));
            card.shade->setOpacity(150);
            float const w = m_artW, h = s2ban::kCardH;
            ccColor4F const red{1.f, .22f, .2f, .85f};
            card.cross->drawSegment({10.f, 6.f}, {w - s2ban::kTagW - 20.f, h - 6.f}, 1.6f, red);
            card.cross->drawSegment({10.f, h - 6.f}, {w - s2ban::kTagW - 20.f, 6.f}, 1.6f, red);
        } else if (look == "chosen") {
            tag("PLAY", ccc3(46, 160, 50), ccWHITE);
            card.edge->setColor(ccc3(255, 225, 120));
            card.back->setColor(mixColor(m_plate, ccc3(120, 90, 20), .35f));
            card.glow->setOpacity(150);
            card.glow->runAction(CCRepeatForever::create(
                CCSequence::create(CCFadeTo::create(.45f, 70), CCFadeTo::create(.45f, 170), nullptr)));
            card.item->runAction(
                CCSequence::create(CCEaseSineOut::create(CCScaleTo::create(.16f, 1.06f * s2ban::kCardScale)),
                                   CCEaseSineIn::create(CCScaleTo::create(.22f, s2ban::kCardScale)), nullptr));
        } else if (look == "spin") {
            tag("?", ccc3(214, 160, 30), ccWHITE);
            card.edge->setColor(ccc3(255, 225, 120));
            card.back->setColor(mixColor(m_plate, ccc3(120, 90, 20), .3f));
            card.glow->setOpacity(130);
        } else if (look == "wait") {
            tag("?", ccc3(56, 56, 60), kMuted);
            card.shade->setOpacity(50);
        } else if (look == "out") {
            tag("-", ccc3(40, 40, 40), kMuted);
            card.shade->setOpacity(120);
        }
    }
    void paintAll() {
        for (auto& card : m_cards) paint(card);
    }
    // 맵 길이 문구: 플레이 시간을 알면 시간으로, 아니면 길이 등급 이름으로 표시하고 시계 아이콘과 함께 가운데 정렬한다.
    static void showLength(Card& card) {
        if (!card.length) return;
        static constexpr char const* names[] = {"TINY", "SHORT", "MEDIUM", "LONG", "XL", "PLATFORMER"};
        double const seconds = mapPlaySeconds(card.level.id);
        std::string const text = seconds > 0.                                       ? playTimeText(seconds)
                                 : card.level.length >= 0 && card.level.length <= 5 ? names[card.level.length]
                                                                                    : "---";
        if (text == card.length->getString()) return;
        card.length->setString(text.c_str());
        card.length->limitLabelWidth(card.lengthWidth, .32f, .16f);
        float const clockW = card.lengthClock ? card.lengthClock->getScaledContentSize().width + 3.f : 0.f;
        float const textW = card.length->getScaledContentSize().width, start = card.lengthAt.x - (clockW + textW) / 2.f;
        if (card.lengthClock) card.lengthClock->setPosition({start + clockW / 2.f - 1.5f, card.lengthAt.y});
        card.length->setPosition({start + clockW + textW / 2.f, card.lengthAt.y});
    }
    // GD에 캐시된 레벨 정보(버전, 업로드 시기, 좋아요 수)가 있으면 카드에 채운다.
    void details(Card& card) {
        showLength(card);
        auto* level = cachedLevel(card.level.id);
        if (!level) return;
        int const net = level->m_likes - level->m_dislikes;
        std::string text = fmt::format("ID: {}", card.level.id);
        auto const version = s2ban::gdVersion(level->m_gameVersion);
        std::string const uploaded(level->m_uploadDate);
        if (!version.empty()) text += "  |  " + version;
        if (!uploaded.empty()) text += "  |  " + uploaded + " ago";
        auto const key = text + "|" + std::to_string(net);
        if (key == card.details) return;
        card.details = key;
        card.date->setString(text.c_str());
        card.date->limitLabelWidth(m_artW - 41.f - s2ban::kTagW - 16.f, s2ban::kDateScale, .15f);
        card.likes->setString(((net < 0 ? "-" : "") + s2ban::count(std::abs(net))).c_str());
        card.likes->limitLabelWidth(34.f, .28f, .15f);
        auto* art = card.item->getNormalImage();
        if (auto* old = art->getChildByID("season2-ban-likes-icon")) old->removeFromParent();
        if (auto* icon = mapvotes::icon(net < 0 ? "GJ_dislikesIcon_001.png" : "GJ_likesIcon_001.png", 11.f)) {
            icon->setScale(11.f / std::max(1.f, icon->getContentSize().height));
            icon->setPosition(card.likesAt);
            icon->setID("season2-ban-likes-icon");
            art->addChild(icon, 2);
            mapvotes::sitOn(icon, s2ban::kRowBottom);
            card.likes->setPositionY(icon->getPositionY());
        }
    }
    // 상황에 맞는 안내 문구와 제목을 갱신한다 (룰렛 중, 선택됨, 오류, 상대 대기, 시간 초과, 확인 대기 등).
    void hint() {
        std::string text;
        ccColor3B color = kIce;
        if (spinning()) {
            text = "Rolling the maps left...";
            color = ccc3(255, 220, 110);
        } else if (m_chosen) {
            auto* card = find(m_chosen);
            text = card ? fmt::format("{} is played!", card->level.name) : std::string("The map is chosen.");
            color = ccc3(255, 220, 110);
        } else if (!m_error.empty()) {
            text = m_error;
            color = ccc3(255, 130, 110);
        } else if (m_bans.own)
            text = m_bans.opponent ? "Both maps banned."
                                   : fmt::format("{} is banning a map...",
                                                 m_opponent.empty() ? std::string("Your opponent") : m_opponent);
        else if (secondsLeft() <= 0)
            text = "Time is up - a random map is banned for you.";
        else if (m_sending)
            text = "Banning...";
        else if (m_armed)
            text = "Tap it again to ban it.";
        else
            text = m_cards.size() > 3 ? "Tap a map, then tap it again to ban it. One of the maps left is played."
                                      : "Tap a map, then tap it again to ban it. The map left is played.";
        if (text != m_hint->getString()) {
            m_hint->setString(text.c_str());
            m_hint->limitLabelWidth(m_cardW - 70.f, .5f, .25f);
        }
        m_hint->setColor(color);
        m_title->setString(spinning() ? "PICKING A MAP" : m_chosen ? "MAP CHOSEN" : "BAN A MAP");
        m_title->setColor(m_chosen ? ccc3(255, 220, 110) : ccc3(255, 110, 95));
    }
    // 카드 탭: 상대가 밴한 카드는 흔들기만 한다. 처음 누르면 확인 대기(armed), 같은 카드를 다시 누르면 ban 콜백으로 전송한다.
    void onCard(CCObject* sender) {
        int const index = sender->getTag();
        if (index < 0 || index >= static_cast<int>(m_cards.size()) || locked()) return;
        auto& card = m_cards[index];
        int64_t const id = card.level.id;
        if (m_bans.opponent == id) {
            card.item->runAction(CCSequence::create(CCMoveBy::create(.04f, {4.f, 0.f}),
                                                    CCMoveBy::create(.08f, {-8.f, 0.f}),
                                                    CCMoveBy::create(.04f, {4.f, 0.f}), nullptr));
            return;
        }
        m_error.clear();
        if (m_armed != id) {
            m_armed = id;
            audio::playBuiltin("counter003.ogg");
        } else {
            m_sending = id;
            audio::fx("fx_thud", "explode_11.ogg");
            if (m_ban) m_ban(id);
        }
        paintAll();
        hint();
    }
    // 후보가 3~5장이고 밴 정보가 있어야 만들어진다. 제목 / 안내 / 남은 시간 시계와 카드를 배치한다.
    bool init(RankedMatch const& match, CCSize size, ccColor3B plate, Ban ban) {
        if (!CCNode::init() || match.candidates.size() < 3 || match.candidates.size() > s2ban::kMaxCards || !match.bans)
            return false;
        m_match = match.id;
        m_opponent = match.opponent.player.name;
        m_ban = std::move(ban);
        m_plate = plate;
        m_gddl = match.gddlTiers;
        setID("season2-ban-board"_spr);
        setContentSize(size);
        m_cardW = std::min(size.width * .8f, 430.f);
        m_artW = m_cardW / s2ban::kCardScale;
        m_left = (size.width - m_cardW) / 2.f;
        float const top = size.height;
        m_title = label(this, "BAN A MAP", {m_left + 2.f, top - 9.f}, .42f, m_cardW * .5f, ccc3(255, 110, 95), true,
                        "bigFont.fnt");
        m_hint = label(this, "", {m_left + 2.f, top - 22.f}, .5f, m_cardW - 70.f, kIce, true, "chatFont.fnt");
        if (auto* icon = loadedFrameSprite("GJ_timeIcon_001.png")) {
            icon->setScale(14.f / std::max(1.f, icon->getContentSize().height));
            icon->setPosition({m_left + m_cardW - 36.f, top - 14.f});
            addChild(icon);
            m_clockIcon = icon;
        }
        m_clock = label(this, "", {m_left + m_cardW - 2.f, top - 14.f}, .55f, 40.f, ccWHITE, false, "bigFont.fnt");
        m_clock->setAnchorPoint({1.f, .5f});
        m_menu = CCMenu::create();
        m_menu->setPosition({0.f, 0.f});
        addChild(m_menu, 5);
        float y = top - s2ban::kHeaderH - s2ban::kSlotH / 2.f;
        for (size_t i = 0; i < match.candidates.size(); ++i) {
            build(match.candidates[i], static_cast<int>(i), y);
            y -= s2ban::kSlotH + s2ban::kSlotGap;
        }
        apply(match);
        scheduleUpdate();
        return true;
    }

public:
    // match: 서버 매치 정보, plate: 카드 배경색, ban: 밴 확정 시 호출되는 콜백(맵 ID).
    static Season2BanBoard* create(RankedMatch const& match, CCSize size, ccColor3B plate, Ban ban) {
        auto* node = new Season2BanBoard;
        if (node->init(match, size, plate, std::move(ban))) {
            node->autorelease();
            return node;
        }
        delete node;
        return nullptr;
    }
    // matchId / sameDraw: 같은 매치와 같은 후보 구성인지 확인한다. revealing / revealDone: 선택 연출 진행 여부.
    std::string const& matchId() const { return m_match; }
    bool sameDraw(RankedMatch const& match) const {
        if (match.id != m_match || match.candidates.size() != m_cards.size()) return false;
        for (size_t i = 0; i < m_cards.size(); ++i)
            if (match.candidates[i].id != m_cards[i].level.id) return false;
        return true;
    }
    bool revealing() const { return m_chosen != 0; }
    bool revealDone() const { return m_chosen != 0 && m_revealAge >= m_spinEnd + s2ban::kRevealSeconds; }
    // 서버에서 새 정보가 오면 반영한다: 밴 현황, 내 요청이 상대보다 늦은 경우의 오류 문구,
    // 전송 / 확인 상태 해제, 맵이 선택되었으면 룰렛(startSpin)을 시작한다.
    void apply(RankedMatch const& match) {
        if (match.id != m_match || !match.bans) return;
        m_bans = *match.bans;
        if (m_sending && !m_bans.own && m_bans.opponent == m_sending && match.state == "banning") {
            m_error = "Your opponent banned that map first.";
            m_errorAge = 0.f;
        }
        if (m_sending && (m_bans.own || m_bans.opponent == m_sending || match.state != "banning")) m_sending = 0;
        if (m_bans.own || (m_bans.opponent && m_armed == m_bans.opponent) || match.state != "banning") m_armed = 0;
        if (match.state != "banning" && match.selectedMap && !m_chosen && find(match.selectedMap->id)) {
            m_chosen = match.selectedMap->id;
            m_revealAge = 0.f;
            startSpin();
        }
        paintAll();
        hint();
    }
    // 밴되지 않고 남은 맵을 여러 바퀴(kSpinRounds) 돌며 점점 느려지다 선택된 맵에서 멈추는 일정을 만든다.
    void startSpin() {
        std::vector<int64_t> left;
        for (auto const& card : m_cards)
            if (card.level.id != m_bans.own && card.level.id != m_bans.opponent) left.push_back(card.level.id);
        auto const at = std::find(left.begin(), left.end(), m_chosen);
        m_spin.clear();
        m_spinEnd = 0.f;
        m_spinAt = 0;
        if (left.size() < 2 || at == left.end()) {
            audio::playBuiltin("gold01.ogg");
            return;
        }
        int const steps = s2ban::kSpinRounds * static_cast<int>(left.size()) + static_cast<int>(at - left.begin());
        float time = 0.f;
        for (int i = 0; i <= steps; ++i) {
            m_spin.push_back({time, left[static_cast<size_t>(i) % left.size()]});
            float const k = static_cast<float>(i) / static_cast<float>(steps);
            time += s2ban::kSpinFirst + (s2ban::kSpinLast - s2ban::kSpinFirst) * k * k;
        }
        m_spinEnd = time;
        m_spinAt = left.front();
    }
    // 밴 전송 실패: 상태를 풀고 3초간 오류 문구를 보여 준다.
    void failed(std::string const& text) {
        m_sending = 0;
        m_armed = 0;
        m_error = text.empty() ? std::string("That map could not be banned.") : text;
        m_errorAge = 0.f;
        paintAll();
        hint();
    }
    // 매 프레임: 룰렛 진행(켜진 카드 바꾸고 효과음), 카드 등장 애니메이션, 0.5초마다 카드 정보 갱신,
    // 오류 문구 만료, 남은 시간 시계 (5초 이하 빨강 + 시계 소리, 0초에 확인 대기 해제).
    void update(float dt) override {
        m_age += dt;
        if (m_chosen) m_revealAge += dt;
        if (m_chosen && !m_spin.empty()) {
            int64_t lit = 0;
            for (auto const& [time, id] : m_spin) {
                if (time > m_revealAge) break;
                lit = id;
            }
            if (m_revealAge >= m_spinEnd) lit = 0;
            if (lit != m_spinAt) {
                m_spinAt = lit;
                if (!lit) {
                    m_spin.clear();
                    audio::playBuiltin("gold01.ogg");
                } else
                    audio::playBuiltin("counter003.ogg");
                paintAll();
                hint();
            }
        }
        if (!m_cardsIn) {
            m_cardsIn = true;
            for (size_t i = 0; i < m_cards.size(); ++i) {
                float const t =
                    std::clamp((m_age - s2ban::kAppear - s2ban::kAppearStep * i) / s2ban::kAppearTime, 0.f, 1.f);
                float const e = 1.f - (1.f - t) * (1.f - t);
                auto* item = m_cards[i].item;
                item->setVisible(t > 0.f);
                item->setPositionX(m_left + m_cardW / 2.f - 18.f * (1.f - e));
                if (auto* art = typeinfo_cast<CCSprite*>(item->getNormalImage()))
                    art->setOpacity(static_cast<GLubyte>(255.f * t));
                m_cardsIn &= t >= 1.f;
            }
        }
        if ((m_detailsTimer -= dt) <= 0.f) {
            m_detailsTimer = .5f;
            for (auto& card : m_cards) details(card);
        }
        if (!m_error.empty() && (m_errorAge += dt) > 3.f) {
            m_error.clear();
            hint();
        }
        int const left = secondsLeft();
        bool const running = !m_chosen && !(m_bans.own && m_bans.opponent);
        m_clock->setVisible(running);
        if (m_clockIcon) m_clockIcon->setVisible(running);
        if (running && left != m_lastSecond) {
            m_lastSecond = left;
            m_clock->setString(std::to_string(left).c_str());
            m_clock->setColor(left <= 5 ? ccc3(255, 90, 80) : ccWHITE);
            if (left <= 5 && left > 0 && !m_bans.own) audio::play(audio::Cue::Clock);
            if (left == 0) {
                m_armed = 0;
                paintAll();
            }
            hint();
        }
    }
};
