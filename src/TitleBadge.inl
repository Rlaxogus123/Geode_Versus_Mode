// 칭호 배지(titleBadge)를 만드는 코드: 칭호 이름과 아이콘이 들어간 작은 판(plate)에
// 등급별 장식, 효과별 애니메이션, 개발자/운영자 전용 판을 얹는다.
// 다른 소스 파일에 포함(include)되어 쓰이는 조각이다.
namespace title_art {
// 색 변환 헬퍼와 칭호 아이콘 생성, 단색 사각형 추가 함수.
inline ccColor3B rgb(uint32_t value) {
    return ccc3((value >> 16) & 255, (value >> 8) & 255, value & 255);
}
inline ccColor3B mix(ccColor3B a, ccColor3B b, float t) {
    return ccc3(a.r + (b.r - a.r) * t, a.g + (b.g - a.g) * t, a.b + (b.b - a.b) * t);
}
inline ccColor4F f4(ccColor3B c, float a = 1.f) {
    return {c.r / 255.f, c.g / 255.f, c.b / 255.f, a};
}

CCNode* icon(TitleIcon kind, ccColor3B tint) {
    static_assert(static_cast<int>(TitleIcon::Target) == static_cast<int>(art::Glyph::Target));
    return art::glyph(static_cast<art::Glyph>(kind), tint);
}

void rect(CCNode* parent, float x, float y, float w, float h, ccColor3B c, GLubyte a, int z = 0) {
    auto* layer = CCLayerColor::create(ccc4(c.r, c.g, c.b, a), w, h);
    layer->setPosition({x, y});
    parent->addChild(layer, z);
}
}

// 칭호 등급(tier)에 따라 판 가장자리에 장식을 더한다.
// tier 2: 모서리 장식, 3: 안쪽 테두리와 훑고 지나가는 빛, 4: 양쪽 보석과 반짝임,
// 5: 윗면 광택과 테두리를 도는 빛. animated가 false면 움직이는 요소는 제외한다.
void tierOrnaments(CCNode* root, int title, float w, float h, ccColor3B base, bool animated) {
    using namespace title_art;
    int const tier = titleTier(title);
    if (tier < 2) return;
    auto const gold = ccc3(255, 214, 110);
    auto const metal = mix(base, gold, tier >= 4 ? .8f : .55f);
    auto const bright = art::f4(art::light(metal, .45f));
    auto* deco = CCDrawNode::create();
    root->addChild(deco, 3);
    float const arm = tier >= 4 ? 3.2f : 2.4f, in = 1.6f;
    for (int cx = 0; cx < 2; ++cx)
        for (int cy = 0; cy < 2; ++cy) {
            float const x = cx ? w - in : in, y = cy ? h - in : in;
            float const sx = cx ? -1.f : 1.f, sy = cy ? -1.f : 1.f;
            deco->drawSegment({x, y}, {x + sx * arm, y}, .38f, bright);
            deco->drawSegment({x, y}, {x, y + sy * arm}, .38f, bright);
        }
    if (tier >= 3) {
        art::stroke(deco, {{2.4f, 2.4f}, {w - 2.4f, 2.4f}, {w - 2.4f, h - 2.4f}, {2.4f, h - 2.4f}}, .22f,
                    art::f4(metal, .85f));
        if (animated) {
            auto* sweep = CCLayerColor::create(ccc4(255, 255, 255, 60), 2.5f, h - 3.f);
            sweep->setBlendFunc({GL_SRC_ALPHA, GL_ONE});
            sweep->setPosition({1.5f, 1.5f});
            root->addChild(sweep, 2);
            sweep->runAction(CCRepeatForever::create(CCSequence::create(CCDelayTime::create(1.6f + .3f * (title % 4)),
                                                                        CCMoveTo::create(.7f, {w - 4.f, 1.5f}),
                                                                        versus::placeAction({1.5f, 1.5f}), nullptr)));
        }
    }
    if (tier >= 4) {
        for (float x : {3.4f, w - 3.4f}) {
            if (auto* gem = art::layeredArt("plate-gem", art::light(base, .25f), 6.2f)) {
                gem->setPosition({x, h / 2.f});
                root->addChild(gem, 3);
                continue;
            }
            art::Path gem{{x, h / 2.f + 2.2f}, {x + 1.6f, h / 2.f}, {x, h / 2.f - 2.2f}, {x - 1.6f, h / 2.f}};
            art::shape(deco, gem, art::f4(art::light(base, .25f)), .3f);
            deco->drawDot({x - .4f, h / 2.f + .6f}, .45f, {1, 1, 1, .9f});
        }
        if (animated)
            for (int i = 0; i < 2; ++i) {
                auto* twinkle = art::sparkle(ccc3(255, 245, 200));
                twinkle->setScale(0.f);
                twinkle->setPosition({i ? w - 6.f : 6.f, i ? 3.2f : h - 3.2f});
                root->addChild(twinkle, 4);
                twinkle->runAction(CCRepeatForever::create(CCSequence::create(
                    CCDelayTime::create(.6f + i * .9f), CCEaseBackOut::create(CCScaleTo::create(.2f, .5f)),
                    CCScaleTo::create(.25f, 0.f), CCDelayTime::create(1.1f), nullptr)));
            }
    }
    if (tier >= 5) {
        auto* sheen = CCLayerGradient::create(ccc4(255, 255, 255, 70), ccc4(255, 255, 255, 0));
        sheen->setContentSize({w - 2.f, h / 2.f});
        sheen->setPosition({1.f, h / 2.f});
        sheen->setBlendFunc({GL_SRC_ALPHA, GL_ONE});
        root->addChild(sheen, 1);
        if (animated) {
            auto* rim = CCNodeRGBA::create();
            rim->setCascadeColorEnabled(true);
            rect(rim, 2.f, 2.f, w - 4.f, .6f, ccWHITE, 230);
            rect(rim, 2.f, h - 2.6f, w - 4.f, .6f, ccWHITE, 230);
            rect(rim, 2.f, 2.f, .6f, h - 4.f, ccWHITE, 230);
            rect(rim, w - 2.6f, 2.f, .6f, h - 4.f, ccWHITE, 230);
            rim->setColor(gold);
            root->addChild(rim, 3);
            rim->runAction(CCRepeatForever::create(CCSequence::create(
                CCTintTo::create(.8f, 255, 150, 180), CCTintTo::create(.8f, 150, 225, 255),
                CCTintTo::create(.8f, 180, 255, 180), CCTintTo::create(.8f, 255, 214, 110), nullptr)));
            auto* spark = CCDrawNode::create();
            spark->drawDot({0, 0}, 1.4f, {1.f, .9f, .6f, .45f});
            spark->drawDot({0, 0}, .6f, {1, 1, 1, 1});
            spark->setBlendFunc({GL_SRC_ALPHA, GL_ONE});
            spark->setPosition({2.3f, 2.3f});
            root->addChild(spark, 5);
            float const pace = 2.6f / (w + h);
            spark->runAction(CCRepeatForever::create(
                CCSequence::create(CCMoveTo::create((w - 4.6f) * pace, {w - 2.3f, 2.3f}),
                                   CCMoveTo::create((h - 4.6f) * pace, {w - 2.3f, h - 2.3f}),
                                   CCMoveTo::create((w - 4.6f) * pace, {2.3f, h - 2.3f}),
                                   CCMoveTo::create((h - 4.6f) * pace, {2.3f, 2.3f}), nullptr)));
        }
    }
}

// Gilded(금색)와 Diamond(푸른 별빛) 칭호 전용 판. 바탕 그라데이션과 광택을 깔고
// 글자, 테두리, 후광 색을 정한 뒤 animated이면 빛 띠와 반짝임 애니메이션을 돌린다.
void luxePlate(CCNode* root, TitleEffect effect, float w, float h, CCNodeRGBA* border, CCLabelBMFont* text,
               CCLayerColor* glow, bool animated) {
    using namespace title_art;
    bool const gold = effect == TitleEffect::Gilded;
    auto* foil = gold ? CCLayerGradient::create(ccc4(214, 162, 52, 250), ccc4(96, 60, 12, 250))
                      : CCLayerGradient::create(ccc4(44, 62, 156, 250), ccc4(8, 12, 46, 250));
    foil->setContentSize({w, h});
    root->addChild(foil, -1);
    auto* sheen = CCLayerGradient::create(ccc4(255, 255, 255, gold ? 95 : 55), ccc4(255, 255, 255, 0));
    sheen->setContentSize({w - 2.f, h * .45f});
    sheen->setPosition({1.f, h * .55f - 1.f});
    sheen->setBlendFunc({GL_SRC_ALPHA, GL_ONE});
    root->addChild(sheen, 1);
    text->setColor(gold ? ccc3(255, 240, 175) : ccc3(235, 248, 255));
    border->setColor(gold ? ccc3(255, 214, 96) : ccc3(225, 240, 255));
    glow->setColor(gold ? ccc3(255, 186, 52) : ccc3(130, 190, 255));
    glow->setOpacity(gold ? 70 : 55);
    std::array<CCNodeRGBA*, 2> stars{};
    if (!gold) {
        for (int layer = 0; layer < 2; ++layer) {
            stars[layer] = CCNodeRGBA::create();
            stars[layer]->setCascadeOpacityEnabled(true);
            for (int i = 0; i < 5; ++i) {
                float const fx = std::fmod(.13f + i * .21f + layer * .1f, .96f) + .02f;
                float const fy = std::fmod(.27f + i * .37f + layer * .19f, .8f) + .1f;
                float const size = layer ? .7f : .95f;
                auto* dot = CCLayerColor::create(ccc4(220, 236, 255, layer ? 190 : 235), size, size);
                dot->setBlendFunc({GL_SRC_ALPHA, GL_ONE});
                dot->setPosition({w * fx - size / 2.f, h * fy - size / 2.f});
                stars[layer]->addChild(dot);
            }
            root->addChild(stars[layer], 0);
        }
    }
    if (!animated) return;
    auto loop = [](CCNode* node, CCActionInterval* a, CCActionInterval* b) {
        node->runAction(CCRepeatForever::create(CCSequence::create(a, b, nullptr)));
    };
    if (gold) {
        auto* band = CCNodeRGBA::create();
        band->setCascadeOpacityEnabled(true);
        auto* left = CCLayerGradient::create(ccc4(255, 246, 200, 0), ccc4(255, 246, 200, 150), {1.f, 0.f});
        auto* right = CCLayerGradient::create(ccc4(255, 246, 200, 150), ccc4(255, 246, 200, 0), {1.f, 0.f});
        for (auto* half : {left, right}) {
            half->setContentSize({4.f, h - 2.f});
            half->setBlendFunc({GL_SRC_ALPHA, GL_ONE});
            band->addChild(half);
        }
        right->setPosition({4.f, 0.f});
        band->setPosition({1.f, 1.f});
        band->setVisible(false);
        root->addChild(band, 2);
        band->runAction(CCRepeatForever::create(
            CCSequence::create(CCDelayTime::create(.9f), versus::placeAction({1.f, 1.f}), CCShow::create(),
                               CCMoveTo::create(.9f, {w - 9.f, 1.f}), CCHide::create(), nullptr)));
        loop(text, CCTintTo::create(.9f, 255, 206, 84), CCTintTo::create(.9f, 255, 246, 196));
        loop(border, CCTintTo::create(.7f, 255, 244, 180), CCTintTo::create(.7f, 214, 150, 40));
        loop(glow, CCFadeTo::create(.8f, 130), CCFadeTo::create(.8f, 35));
        for (int i = 0; i < 4; ++i) {
            auto* spark = art::sparkle(ccc3(255, 238, 170));
            spark->setScale(0.f);
            spark->setPosition({w * (.24f + .2f * i), i % 2 ? h - 3.6f : 3.6f});
            root->addChild(spark, 4);
            spark->runAction(CCRepeatForever::create(CCSequence::create(
                CCDelayTime::create(.25f + i * .4f), CCEaseBackOut::create(CCScaleTo::create(.18f, .5f)),
                CCScaleTo::create(.3f, 0.f), CCDelayTime::create(2.f - i * .4f), nullptr)));
        }
        return;
    }
    for (int layer = 0; layer < 2; ++layer)
        stars[layer]->runAction(CCRepeatForever::create(CCSequence::create(
            CCDelayTime::create(layer * .7f), CCFadeTo::create(.7f, 50), CCFadeTo::create(.7f, 255), nullptr)));
    auto* prism = CCLayerColor::create(ccc4(255, 255, 255, 120), 3.f, h - 2.f);
    prism->setBlendFunc({GL_SRC_ALPHA, GL_ONE});
    prism->setPosition({1.f, 1.f});
    prism->setVisible(false);
    root->addChild(prism, 2);
    prism->runAction(CCRepeatForever::create(
        CCSequence::create(CCDelayTime::create(1.3f), versus::placeAction({1.f, 1.f}), CCShow::create(),
                           CCMoveTo::create(1.1f, {w - 4.f, 1.f}), CCHide::create(), nullptr)));
    prism->runAction(CCRepeatForever::create(
        CCSequence::create(CCTintTo::create(.5f, 255, 150, 210), CCTintTo::create(.5f, 150, 230, 255),
                           CCTintTo::create(.5f, 200, 255, 170), CCTintTo::create(.5f, 255, 235, 150), nullptr)));
    loop(text, CCTintTo::create(.8f, 190, 230, 255), CCTintTo::create(.8f, 245, 250, 255));
    border->runAction(CCRepeatForever::create(CCSequence::create(CCTintTo::create(.7f, 160, 210, 255),
                                                                 CCTintTo::create(.7f, 225, 185, 255),
                                                                 CCTintTo::create(.7f, 235, 248, 255), nullptr)));
    loop(glow, CCFadeTo::create(1.f, 110), CCFadeTo::create(1.f, 30));
    auto* streak = CCLayerGradient::create(ccc4(255, 255, 255, 0), ccc4(255, 255, 255, 230), {1.f, 0.f});
    streak->setContentSize({7.f, .8f});
    streak->setBlendFunc({GL_SRC_ALPHA, GL_ONE});
    streak->setVisible(false);
    root->addChild(streak, 3);
    streak->runAction(CCRepeatForever::create(
        CCSequence::create(CCDelayTime::create(2.4f), versus::placeAction({w * .25f, h - 3.2f}), CCShow::create(),
                           CCMoveTo::create(.45f, {w - 10.f, h - 5.2f}), CCHide::create(), nullptr)));
}

// 개발자/운영자 전용 판의 크기 상수(판 높이, 양쪽 끝 장식 폭, 글자 높이, 보석 위치 등)와
// 판에 쓰는 이미지 이름 및 색 세트.
namespace maker {
constexpr float kPlateH = 13.f, kCapW = 7.f, kGemX = kCapW * .44f;
constexpr float kTextH = 9.4f, kTextPad = .064f;
constexpr float kGemClear = 5.8f;
constexpr float kCrestH = 6.f, kCrestBar = .233f, kHaloH = 2.33f, kHaloY = 3.1f;
// 전용 판에 쓰는 이미지(글자, 광택, 양끝, 가운데, 장식)와 별/보석 색 세트.
struct PlateArt {
    char const *text, *shine, *cap, *mid, *crest;
    bool halo;
    ccColor3B starAlt, popGem, popTop;
};
constexpr PlateArt kMakerArt{"tipp7-maker-text.png", "tipp7-maker-shine.png", "tipp7-maker-cap.png",
                             "tipp7-maker-mid.png",  "tipp7-maker-crest.png", true,
                             {176, 222, 255},        {214, 240, 255},         {255, 240, 196}};
constexpr PlateArt kAdminArt{"tipp7-admin-text.png", "tipp7-admin-shine.png", "tipp7-admin-cap.png",
                             "tipp7-admin-mid.png",  "tipp7-admin-crest.png", false,
                             {150, 200, 255},        {200, 230, 255},         {220, 236, 255}};
}

// 개발자/운영자용 전용 판을 만든다. 글자 이미지(워드마크)가 있으면 그 폭에 맞추고,
// 없으면 칭호 이름을 BMFont로 쓴다. 판 이미지가 없으면 단색 사각형으로 대체한다.
// maxWidth를 넘으면 배율을 줄여(최소 0.45) 맞춘다. animated이면 후광, 광택, 별 반짝임을
// 추가한다.
CCNode* plateBadge(CCNode* parent, TitleStyle const& style, CCPoint position, float scale, float maxWidth,
                   bool animated, maker::PlateArt const& look) {
    using namespace title_art;
    using namespace maker;
    auto* root = CCNode::create();
    root->setID("player-title"_spr);
    CCNode* text = art::artSprite(look.text, kTextH);
    bool const wordmark = text != nullptr;
    float inkWidth = 0.f;
    if (wordmark) {
        inkWidth = text->getContentSize().width * text->getScale() - 2.f * kTextPad * kTextH;
    } else {
        auto* label = CCLabelBMFont::create(std::string(style.name).c_str(), "goldFont.fnt");
        label->setScale(8.f / std::max(1.f, label->getContentSize().height));
        inkWidth = label->getContentSize().width * label->getScale();
        text = label;
    }
    float const w = inkWidth + 2.f * kGemClear, h = kPlateH;
    float const fit = std::clamp(maxWidth / std::max(1.f, scale * w), .45f, 1.f);
    root->setContentSize({w, h});
    root->setAnchorPoint({.5f, .5f});
    root->setPosition(position);
    root->setScale(scale * fit);
    parent->addChild(root);

    auto* mid = art::artSprite(look.mid, h);
    auto* right = art::artSprite(look.cap, h);
    auto* left = art::artSprite(look.cap, h);
    if (mid && right && left) {
        mid->setScaleX((w - 2.f * kCapW + .4f) / std::max(1.f, mid->getContentSize().width));
        mid->setPosition({w / 2.f, h / 2.f});
        root->addChild(mid, 0);
        right->setAnchorPoint({1.f, .5f});
        right->setPosition({w, h / 2.f});
        root->addChild(right, 0);
        left->setFlipX(true);
        left->setAnchorPoint({0.f, .5f});
        left->setPosition({0.f, h / 2.f});
        root->addChild(left, 0);
    } else {
        root->addChild(CCLayerColor::create(ccc4(20, 52, 150, 255), w, h), 0);
        rect(root, 0, 0, w, 1, ccc3(255, 216, 108), 255, 0);
        rect(root, 0, h - 1, w, 1, ccc3(255, 216, 108), 255, 0);
        rect(root, 0, 0, 1, h, ccc3(255, 216, 108), 255, 0);
        rect(root, w - 1, 0, 1, h, ccc3(255, 216, 108), 255, 0);
    }
    text->setPosition({w / 2.f, h / 2.f});
    root->addChild(text, 2);

    if (auto* crest = art::artSprite(look.crest, kCrestH)) {
        crest->setAnchorPoint({.5f, kCrestBar});
        crest->setPosition({w / 2.f, h - .75f});
        root->addChild(crest, 3);
    }
    CCPoint const haloAt{w / 2.f, h + kHaloY};
    auto* halo = look.halo ? art::artSprite("tipp7-halo.png", kHaloH) : nullptr;
    if (halo) {
        halo->setPosition(haloAt);
        root->addChild(halo, 4);
    }
    std::vector<CCSprite*> stars;
    for (int i = 0; i < 10; ++i) {
        auto* star = art::artSprite("tipp7-maker-star.png", i % 3 ? 1.5f : 2.3f);
        if (!star) break;
        float const fx = std::fmod(.05f + i * .137f, .9f) + .05f, fy = std::fmod(.3f + i * .419f, .62f) + .22f;
        star->setColor(i % 2 ? look.starAlt : ccWHITE);
        star->setPosition({kCapW + (w - 2.f * kCapW) * fx, h * fy});
        root->addChild(star, 1);
        stars.push_back(star);
    }
    if (!animated) return root;

    if (halo) {
        halo->runAction(CCRepeatForever::create(
            CCSequence::create(CCEaseSineInOut::create(CCMoveTo::create(.9f, {haloAt.x, haloAt.y + .7f})),
                               CCEaseSineInOut::create(CCMoveTo::create(.9f, haloAt)), nullptr)));
    }
    if (auto* glint = wordmark ? art::artSprite(look.shine, kTextH) : nullptr) {
        auto* texture = glint->getTexture();
        auto const sheet = texture->getContentSize();
        float const fw = sheet.width / 4.f, fh = sheet.height / 2.f;
        std::vector<Ref<CCSpriteFrame>> frames;
        for (int i = 0; i < 8; ++i)
            if (auto* frame = CCSpriteFrame::createWithTexture(texture, CCRect((i % 4) * fw, (i / 4) * fh, fw, fh)))
                frames.emplace_back(frame);
        if (frames.size() == 8) {
            glint->setDisplayFrame(frames[0].data());
            glint->setScale(kTextH / std::max(1.f, fh));
            glint->setBlendFunc({GL_SRC_ALPHA, GL_ONE});
            glint->setPosition(text->getPosition());
            glint->setVisible(false);
            root->addChild(glint, 3);
            auto* steps = CCArray::create();
            steps->addObject(CCDelayTime::create(2.2f));
            steps->addObject(CCShow::create());
            for (auto const& frame : frames) {
                steps->addObject(CallFuncExt::create([glint, frame] { glint->setDisplayFrame(frame.data()); }));
                steps->addObject(CCDelayTime::create(.05f));
            }
            steps->addObject(CCHide::create());
            glint->runAction(CCRepeatForever::create(CCSequence::create(steps)));
        }
    }
    for (size_t i = 0; i < stars.size(); ++i) {
        float const s = stars[i]->getScale();
        stars[i]->runAction(CCRepeatForever::create(CCSequence::create(
            CCDelayTime::create(.2f + std::fmod(i * .37f, 1.1f)),
            CCEaseSineInOut::create(CCScaleTo::create(.45f, s * .25f)),
            CCEaseSineInOut::create(CCScaleTo::create(.45f, s * 1.15f)), CCScaleTo::create(.2f, s), nullptr)));
    }
    CCPoint const pops[3] = {{kGemX - .5f, h / 2.f + .9f},
                             {w - kGemX - .5f, h / 2.f + .9f},
                             look.halo ? CCPoint{w / 2.f + 2.9f, h + kHaloY + .7f} : CCPoint{w / 2.f + 2.f, h + 2.3f}};
    for (int i = 0; i < 3; ++i) {
        auto* pop = art::artSprite("tipp7-maker-star.png", 4.4f);
        if (!pop) break;
        float const s = pop->getScale();
        pop->setPosition(pops[i]);
        pop->setScale(0.f);
        pop->setColor(i == 2 ? look.popTop : look.popGem);
        root->addChild(pop, 5);
        pop->runAction(CCRepeatForever::create(
            CCSequence::create(CCDelayTime::create(.5f + i * .7f), CCEaseBackOut::create(CCScaleTo::create(.18f, s)),
                               CCRotateBy::create(.3f, 90.f), CCScaleTo::create(.18f, 0.f),
                               CCDelayTime::create(1.9f - i * .4f), nullptr)));
    }
    return root;
}
// 제작자 계정용 판과 운영자용 판. 같은 plateBadge에 서로 다른 이미지 세트를 넘긴다.
CCNode* makerBadge(CCNode* parent, TitleStyle const& style, CCPoint position, float scale, float maxWidth,
                   bool animated) {
    return plateBadge(parent, style, position, scale, maxWidth, animated, maker::kMakerArt);
}
CCNode* adminBadge(CCNode* parent, TitleStyle const& style, CCPoint position, float scale, float maxWidth,
                   bool animated) {
    return plateBadge(parent, style, position, scale, maxWidth, animated, maker::kAdminArt);
}

// G.O.A.T 칭호(시즌 2 데몬 컵 최상위, 한 번 달성하면 G.O.A.T에서 내려와도 남는다) 배지 뒤에서 타오르는 불꽃.
// G.O.A.T 티어 이름 뒤의 것과 같은 셰이더(goat::band)로, 판 아래쪽에서 피어올라 위로 판 높이만큼 넘실댄다.
// 셰이더를 못 쓰는 기기면 불꽃 없이 간다.
void goatTitleFire(CCNode* root, float w, float h) {
    float const tall = h * 1.9f;
    if (auto* fire = goat::band({w + 10.f, tall}, ccc3(255, 150, 40), .75f)) {
        fire->setPosition({w / 2.f, tall / 2.f - 2.f});
        fire->setID("goat-title-fire"_spr);
        root->addChild(fire, -3);
    }
}

// 칭호 ID에 맞는 배지를 만들어 parent에 붙이고 반환한다.
// 시즌 1 최종 칭호는 티어 칭호의 모양에 "S1 R<레이팅>" 글자를 쓴다.
// 개발자/운영자 효과(Sky, Staff)는 전용 판으로 넘기고, 나머지는 아래에서 직접 구성한다.
CCNode* titleBadge(CCNode* parent, int title, CCPoint position, float scale = 1.f, float maxWidth = 150.f,
                   bool animated = true) {
    using namespace title_art;
    std::string const finalText = isS1FinalTitle(title) ? fmt::format("S1 R{}", s1FinalRating(title)) : std::string();
    if (isS1FinalTitle(title)) title = kFirstS1RankTitle + s1FinalTier(title) - 1;
    auto const& style = kTitles[std::clamp(title, 0, kTitleCount - 1)];
    if (style.effect == TitleEffect::Sky) return makerBadge(parent, style, position, scale, maxWidth, animated);
    if (style.effect == TitleEffect::Staff) return adminBadge(parent, style, position, scale, maxWidth, animated);
    auto const base = rgb(style.color);
    auto* root = CCNode::create();
    root->setID("player-title"_spr);
    auto* text =
        CCLabelBMFont::create(finalText.empty() ? std::string(style.name).c_str() : finalText.c_str(), "bigFont.fnt");
    float const pad = titleTier(title) >= 4 ? 4.f : 0.f;
    float const natural = text->getContentSize().width * .3f;
    float const textWidth = std::min(maxWidth / scale - 24.f - pad * 2.f, natural);
    text->setScale(textWidth / std::max(1.f, text->getContentSize().width));
    float const w = textWidth + 24.f + pad * 2.f, h = 13.f;
    root->setContentSize({w, h});
    root->setAnchorPoint({.5f, .5f});
    root->setPosition(position);
    root->setScale(scale);
    parent->addChild(root);

    auto* glow = CCLayerColor::create(ccc4(base.r, base.g, base.b, 0), w + 4.f, h + 4.f);
    glow->setPosition({-2.f, -2.f});
    glow->setBlendFunc({GL_SRC_ALPHA, GL_ONE});
    root->addChild(glow, -2);
    auto* box = CCLayerColor::create(ccc4(base.r * .45f, base.g * .45f, base.b * .45f, 235), w, h);
    root->addChild(box, -1);
    if (title == kSeason2GoatTitle && animated) goatTitleFire(root, w, h);
    auto const edge = mix(base, ccWHITE, .35f);
    auto* border = CCNodeRGBA::create();
    border->setCascadeColorEnabled(true);
    border->setCascadeOpacityEnabled(true);
    rect(border, 0, 0, w, 1, ccWHITE, 255);
    rect(border, 0, h - 1, w, 1, ccWHITE, 255);
    rect(border, 0, 0, 1, h, ccWHITE, 255);
    rect(border, w - 1, 0, 1, h, ccWHITE, 255);
    border->setColor(edge);
    root->addChild(border, 1);
    // 왼쪽 아이콘: G.O.A.T 칭호는 전용 얼굴, 랭크 칭호는 GD 난이도 얼굴 스프라이트,
    // 그 외에는 칭호의 아이콘을 쓴다.
    CCNode* glyph = nullptr;
    if (title == kSeason2GoatTitle) glyph = goat::face({13.5f, 13.5f}, false);
    if (int const face = glyph ? 0 : seasonRankFace(title)) {
        auto const frameName = fmt::format("diffIcon_{:02}_btn_001.png", face);
        if (auto* frame = loadedFrame(frameName.c_str(), "GJ_GameSheet03.plist")) {
            auto* sprite = CCSprite::createWithSpriteFrame(frame);
            sprite->setScale(13.5f / std::max(1.f, sprite->getContentSize().height));
            glyph = sprite;
        }
    }
    if (!glyph) {
        glyph = icon(style.icon, mix(base, ccWHITE, .55f));
        glyph->setScale(1.12f);
    }
    glyph->setPosition({9.f + pad, h / 2.f});
    root->addChild(glyph, 2);
    text->setPosition({15.f + pad + textWidth / 2.f, h / 2.f + .5f});
    root->addChild(text, 2);
    // 등급 장식을 얹고, Gilded/Diamond는 전용 판 함수로 마무리한다.
    tierOrnaments(root, title, w, h, base, animated);
    if (style.effect == TitleEffect::Gilded || style.effect == TitleEffect::Diamond) {
        luxePlate(root, style.effect, w, h, border, text, glow, animated);
        return root;
    }

    // animated가 false이면 효과별로 색만 정적으로 바꿔 두고 끝낸다.
    if (!animated) {
        switch (style.effect) {
        case TitleEffect::Royal:
            text->setColor(ccc3(255, 221, 120));
            border->setColor(ccc3(255, 206, 90));
            glow->setOpacity(45);
            break;
        case TitleEffect::Frost: text->setColor(ccc3(215, 245, 255)); break;
        case TitleEffect::Flame:
            glow->setColor(ccc3(255, 120, 40));
            glow->setOpacity(70);
            text->setColor(ccc3(255, 230, 170));
            break;
        case TitleEffect::Electric:
            glow->setColor(ccc3(110, 200, 255));
            glow->setOpacity(50);
            break;
        case TitleEffect::Shadow:
            box->setColor(ccc3(18, 10, 26));
            glow->setColor(ccc3(150, 60, 220));
            glow->setOpacity(60);
            break;
        case TitleEffect::Rainbow:
            text->setColor(ccc3(255, 200, 160));
            border->setColor(ccc3(200, 170, 255));
            break;
        case TitleEffect::Aurora: box->setColor(ccc3(40, 90, 120)); break;
        case TitleEffect::Pulse: glow->setOpacity(50); break;
        default: break;
        }
        return root;
    }
    // animated이면 효과별 애니메이션을 건다. loop는 두 동작을 번갈아 무한 반복한다.
    auto loop = [](CCNode* node, CCActionInterval* a, CCActionInterval* b) {
        node->runAction(CCRepeatForever::create(CCSequence::create(a, b, nullptr)));
    };
    switch (style.effect) {
    case TitleEffect::Plain: break;
    case TitleEffect::Pulse: loop(glow, CCFadeTo::create(.9f, 90), CCFadeTo::create(.9f, 10)); break;
    case TitleEffect::Shimmer:
    case TitleEffect::Royal:
    case TitleEffect::Frost: {
        auto* stripe =
            CCLayerColor::create(ccc4(255, 255, 255, style.effect == TitleEffect::Frost ? 110 : 80), 4.f, h - 2.f);
        stripe->setBlendFunc({GL_SRC_ALPHA, GL_ONE});
        stripe->setPosition({1.f, 1.f});
        root->addChild(stripe, 1);
        stripe->runAction(CCRepeatForever::create(
            CCSequence::create(CCMoveTo::create(1.1f, {w - 5.f, 1.f}), CCHide::create(), CCDelayTime::create(1.4f),
                               versus::placeAction({1.f, 1.f}), CCShow::create(), nullptr)));
        if (style.effect == TitleEffect::Royal) {
            text->setColor(ccc3(255, 221, 120));
            border->setColor(ccc3(255, 206, 90));
            loop(glow, CCFadeTo::create(1.2f, 70), CCFadeTo::create(1.2f, 15));
        }
        if (style.effect == TitleEffect::Frost) text->setColor(ccc3(215, 245, 255));
        break;
    }
    case TitleEffect::Sparkle:
        for (int i = 0; i < 3; ++i) {
            auto* spark = art::sparkle(ccc3(255, 245, 190));
            spark->setScale(.55f);
            spark->setVisible(false);
            spark->setPosition({w * (.35f + .3f * i), i % 2 ? h - 1.f : 1.f});
            root->addChild(spark, 3);
            spark->runAction(CCRepeatForever::create(
                CCSequence::create(CCDelayTime::create(.4f + i * .55f), CCShow::create(), CCDelayTime::create(.35f),
                                   CCHide::create(), CCDelayTime::create(.9f), nullptr)));
        }
        break;
    case TitleEffect::Flame:
        glow->setColor(ccc3(255, 120, 40));
        loop(glow, CCFadeTo::create(.25f, 110), CCFadeTo::create(.35f, 40));
        loop(text, CCTintTo::create(.5f, 255, 214, 120), CCTintTo::create(.5f, 255, 255, 255));
        break;
    case TitleEffect::Rainbow:
        text->runAction(CCRepeatForever::create(
            CCSequence::create(CCTintTo::create(.6f, 255, 140, 160), CCTintTo::create(.6f, 255, 225, 130),
                               CCTintTo::create(.6f, 140, 255, 180), CCTintTo::create(.6f, 130, 200, 255),
                               CCTintTo::create(.6f, 210, 150, 255), nullptr)));
        border->runAction(CCRepeatForever::create(CCSequence::create(CCTintTo::create(.6f, 130, 200, 255),
                                                                     CCTintTo::create(.6f, 255, 140, 160),
                                                                     CCTintTo::create(.6f, 255, 225, 130), nullptr)));
        break;
    case TitleEffect::Electric:
        border->runAction(CCRepeatForever::create(
            CCSequence::create(CCTintTo::create(.05f, 255, 255, 255), CCDelayTime::create(.08f),
                               CCTintTo::create(.05f, edge.r, edge.g, edge.b), CCDelayTime::create(.6f),
                               CCTintTo::create(.04f, 170, 240, 255), CCDelayTime::create(.05f),
                               CCTintTo::create(.05f, edge.r, edge.g, edge.b), CCDelayTime::create(1.1f), nullptr)));
        glow->setColor(ccc3(110, 200, 255));
        loop(glow, CCFadeTo::create(.08f, 90),
             CCSequence::create(CCFadeTo::create(.2f, 0), CCDelayTime::create(1.2f), nullptr));
        break;
    case TitleEffect::Aurora:
        box->runAction(CCRepeatForever::create(CCSequence::create(CCTintTo::create(1.4f, 30, 110, 90),
                                                                  CCTintTo::create(1.4f, 40, 70, 130),
                                                                  CCTintTo::create(1.4f, 90, 50, 120), nullptr)));
        break;
    case TitleEffect::Shadow:
        box->setColor(ccc3(18, 10, 26));
        glow->setColor(ccc3(150, 60, 220));
        loop(glow, CCFadeTo::create(1.f, 100), CCFadeTo::create(1.f, 20));
        break;
    case TitleEffect::Gilded:
    case TitleEffect::Diamond: break;
    case TitleEffect::Sky: break;
    case TitleEffect::Staff: break;
    }
    return root;
}
