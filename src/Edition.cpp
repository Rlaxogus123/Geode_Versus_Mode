// 닉네임 글꼴/그라데이션 색 적용과 아이콘 오라(이미지 + 파티클 효과)를 구현한다.
// 선언은 Edition.hpp, 선택값 규칙은 Cosmetics.hpp.
#include "Edition.hpp"
#include "Particles.hpp"
#include "AuraSprites.hpp"
#include "SpriteFrames.hpp"
#include <Geode/Geode.hpp>
#include <limits>

using namespace geode::prelude;
namespace versus {
namespace {
// 오라 이미지를 아이콘 뒤에 붙이고, spec에 따라 회전하거나 투명도(205~255)를 반복해
// 깜빡이게 한다.
// 파일이 없으면 경고만 남기고 건너뛴다.
void addAuraImage(CCNode* icon, AuraSpriteSpec const& spec) {
    if (icon->getChildByID("membership-aura-image"_spr)) return;
    auto const resource = Mod::get()->expandSpriteName(spec.file);
    auto* art = versus::fileSprite(resource.c_str());
    if (!art) {
        log::warn("[Cosmetics] Missing aura image: {}", spec.file);
        return;
    }
    auto const size = art->getContentSize();
    auto const scale = auraSpriteScale(spec, size.width, size.height);
    if (scale <= 0.f) return;
    art->setID("membership-aura-image"_spr);
    art->setAnchorPoint({.5f, .5f});
    art->setPosition({0.f, 0.f});
    art->setScale(scale);
    icon->addChild(art, -2);
    if (spec.rotate) {
        art->runAction(CCRepeatForever::create(CCRotateBy::create(spec.period, 360.f)));
    } else {
        art->runAction(CCRepeatForever::create(
            CCSequence::create(CCFadeTo::create(spec.period, 205), CCFadeTo::create(spec.period, 255), nullptr)));
    }
}
}
// 1.0.7: 두 색과 Glow를 한 번에 입힌다. glow는 Glow가 켜진 플레이어의 글로우 색(RGB), 꺼져 있으면 -1.
void applyIconLook(SimplePlayer* icon, int color1, int color2, int glow) {
    if (!icon) return;
    auto const rgb = [](int value) { return ccc3((value >> 16) & 255, (value >> 8) & 255, value & 255); };
    icon->setColors(rgb(color1), rgb(color2));
    if (glow >= 0)
        icon->setGlowOutline(rgb(glow));
    else
        icon->disableGlowOutline();
}
// 아이콘에 오라를 붙인다. 사용 권한이 없거나 None(8)이면 아무것도 하지 않는다.
// 이미지가 있는 오라는 이미지를 먼저 붙이고, 이어서 공통으로 파티클을 붙인다.
void addMembershipAura(CCNode* icon, bool membership, Cosmetics const& cosmetics) {
    if (!icon || !canUseAura(cosmetics.aura, membership)) return;
    auto const choice = cosmetics.aura;
    if (choice == 8) return;
    auto const* imageSpec = auraSpriteSpec(choice);
    if (imageSpec) addAuraImage(icon, *imageSpec);
    if (icon->getChildByID("membership-aura"_spr)) return;
    // 오라별 파티클 사양: 프레임, 입자 수, 시작/끝 색(RGB), 수명(초), 크기, 퍼짐(spread),
    // 속도, 중력, 반지름(0보다 크면 원형 회전 모드), 초당 회전각.
    struct Preset {
        char const* frame;
        int count;
        int first, last;
        float life, size, spread, speed, gravity, radius, rotation;
    };
    // 오라 ID 순서(kAuraNames)와 같은 순서의 사양 표.
    static constexpr Preset presets[] = {
        {"particle_02_001.png", 18, 0x4DFF73, 0xFFFFFF, 1.1f, 10.f, 0.f, 0.f, 0.f, 24.f, 35.f},
        {"particle_01_001.png", 28, 0xFF5A16, 0xFFD650, 1.f, 13.f, 24.f, 24.f, 16.f, 0.f, 0.f},
        {"particle_07_001.png", 24, 0xB07AFF, 0x62EFFF, 1.2f, 7.f, 0.f, 0.f, 0.f, 29.f, 65.f},
        {"particle_04_001.png", 22, 0xB5FFFF, 0x429AFF, 1.1f, 8.f, 25.f, 9.f, -7.f, 0.f, 0.f},
        {"particle_07_001.png", 26, 0xDDF8FF, 0x5A65FF, .65f, 10.f, 0.f, 0.f, 0.f, 25.f, 220.f},
        {"particle_02_001.png", 28, 0xFFF078, 0xFF961F, 1.f, 12.f, 0.f, 0.f, 0.f, 26.f, -45.f},
        {"particle_01_001.png", 28, 0x9B3AFF, 0xE49AFF, 1.3f, 12.f, 0.f, 0.f, 0.f, 29.f, -100.f},
        {"particle_07_001.png", 20, 0xFFEAB0, 0xB6C7FF, 1.4f, 9.f, 26.f, 12.f, -10.f, 0.f, 0.f},
        {},
        {"particle_07_001.png", 22, 0xFFE18A, 0xFFFFFF, .9f, 6.f, 26.f, 7.f, 4.f, 0.f, 0.f},
        {"particle_07_001.png", 20, 0xAAA7FF, 0xE4F2FF, 1.25f, 6.f, 0.f, 0.f, 0.f, 28.f, -30.f},
        {"particle_01_001.png", 28, 0xFF5828, 0xFFECA0, .7f, 9.f, 25.f, 16.f, 10.f, 0.f, 0.f},
        {"particle_04_001.png", 22, 0x8AF7FF, 0xA5AEFF, 1.f, 6.f, 0.f, 0.f, 0.f, 30.f, 40.f},
        {"particle_07_001.png", 24, 0xFFE9AC, 0x9EEFFF, 1.3f, 6.f, 0.f, 0.f, 0.f, 30.f, 25.f},
        {"particle_166_001.png", 22, 0xFF9AE6, 0x8AF4FF, 1.3f, 9.f, 24.f, 14.f, 8.f, 0.f, 0.f},
        {"particle_158_001.png", 16, 0x6BFFD8, 0x9C7BFF, 1.5f, 14.f, 0.f, 0.f, 0.f, 27.f, 75.f},
        {"particle_168_001.png", 22, 0xFFF2B0, 0x9ED8FF, 1.f, 8.f, 0.f, 0.f, 0.f, 30.f, -40.f},
        {"particle_83_001.png", 24, 0x7DFFB0, 0xFFD36B, .9f, 7.f, 25.f, 15.f, 10.f, 0.f, 0.f}};
    static_assert(std::size(presets) == kAuraCount);
    // GD 파티클 시트의 프레임을 쓰고, 없으면 square.png 텍스처로 대체한다.
    // 둘 다 없으면 경고 후 포기한다.
    auto const& spec = presets[choice];
    auto* frame = versus::loadedFrame(spec.frame, "GJ_ParticleSheet.plist");
    auto* fallback = frame ? nullptr : CCTextureCache::sharedTextureCache()->addImage("square.png", false);
    if (!frame && !fallback) {
        log::warn("[Cosmetics] No usable GD particle texture for aura {}", choice);
        return;
    }
    auto* aura = versus::particleSystem(static_cast<unsigned>(spec.count));
    if (!aura) return;
    if (frame)
        aura->setDisplayFrame(frame);
    else
        aura->setTexture(fallback);
    aura->setID("membership-aura"_spr);
    aura->setOpacity(255);
    aura->toggleUniformColorMode(false);
    aura->setPositionType(kCCPositionTypeGrouped);
    aura->setEmitterMode(spec.radius > 0.f ? kCCParticleModeRadius : kCCParticleModeGravity);
    aura->setSourcePosition({0.f, 0.f});
    aura->setPosition({0.f, 0.f});
    aura->setPosVar({spec.spread, imageSpec ? spec.spread * .35f : spec.spread});
    aura->setDuration(-1.f);
    aura->setLife(spec.life);
    aura->setLifeVar(.15f);
    aura->setEmissionRate(spec.count / spec.life);
    aura->setAngle(90.f);
    aura->setAngleVar(spec.radius > 0.f ? 180.f : imageSpec ? 15.f : 40.f);
    if (spec.radius > 0.f) {
        aura->setStartRadius(spec.radius);
        aura->setStartRadiusVar(imageSpec ? 1.5f : 2.f);
        aura->setEndRadius(choice == 6 ? 21.f : spec.radius);
        aura->setEndRadiusVar(0.f);
        aura->setRotatePerSecond(spec.rotation);
        aura->setRotatePerSecondVar(10.f);
    } else {
        aura->setSpeed(spec.speed);
        aura->setSpeedVar(imageSpec ? 3.f : 4.f);
        aura->setGravity({0.f, spec.gravity});
        aura->setRadialAccel(0.f);
        aura->setRadialAccelVar(0.f);
        aura->setTangentialAccel(0.f);
        aura->setTangentialAccelVar(0.f);
    }
    aura->setStartSize(spec.size);
    aura->setStartSizeVar(imageSpec ? 1.5f : 2.f);
    aura->setEndSize(2.f);
    aura->setEndSizeVar(0.f);
    auto rgba = [](int color, float alpha) {
        return ccColor4F{((color >> 16) & 255) / 255.f, ((color >> 8) & 255) / 255.f, (color & 255) / 255.f, alpha};
    };
    aura->setStartColor(rgba(spec.first, 1.f));
    aura->setStartColorVar({0.f, 0.f, 0.f, 0.f});
    aura->setEndColor(rgba(spec.last, 0.f));
    aura->setEndColorVar({0.f, 0.f, 0.f, 0.f});
    aura->setBlendAdditive(true);
    aura->setAutoRemoveOnFinish(false);
    icon->addChild(aura, -1);
    aura->resetSystem();
    aura->setVisible(true);
    // 처음 보일 때 빈 상태가 아니도록 시뮬레이션을 12스텝(1/30초) 미리 돌려 둔다.
    for (int step = 0; step < 12; ++step) aura->update(1.f / 30.f);
    if (!aura->getTexture() || aura->getParticleCount() == 0) {
        log::warn("[Cosmetics] Aura {} did not initialize: texture={}, particles={}", choice,
                  aura->getTexture() != nullptr, aura->getParticleCount());
    }
}
namespace {
// 닉네임 글자(glyph)마다 그라데이션 색을 입힌다. 라벨 전체 경계 기준으로 위치를 계산하고,
// 글자 네 꼭짓점 색을 따로 정해 글자 안에서도 색이 이어지게 한다.
void tintNickname(CCLabelBMFont* label, Cosmetics const& cosmetics) {
    if (!label || !label->getChildren()) return;
    label->setColor(ccWHITE);
    auto const value = sanitizeCosmetics(cosmetics);
    auto const axis = gradientAxis(value.angle);
    float left = std::numeric_limits<float>::max(), right = -left, bottom = left, top = -left;
    for (auto* glyph : CCArrayExt<CCSprite*>(label->getChildren())) {
        if (!glyph->isVisible()) continue;
        auto const bounds = glyph->boundingBox();
        left = std::min(left, bounds.getMinX());
        right = std::max(right, bounds.getMaxX());
        bottom = std::min(bottom, bounds.getMinY());
        top = std::max(top, bounds.getMaxY());
    }
    for (auto* glyph : CCArrayExt<CCSprite*>(label->getChildren())) {
        if (!glyph->isVisible()) continue;
        auto const bounds = glyph->boundingBox();
        auto at = [&](float x, float y) {
            return gradientColor(gradientPosition(x - left, y - bottom, right - left, top - bottom, axis), value);
        };
        auto color = at(bounds.getMidX(), bounds.getMidY());
        glyph->setColor(ccc3(color[0], color[1], color[2]));
        auto vertex = [&](auto& corner, float x, float y) {
            auto rgb = at(x, y);
            auto const opacity = glyph->getDisplayedOpacity();
            float const factor = glyph->isOpacityModifyRGB() ? opacity / 255.f : 1.f;
            corner.colors = {static_cast<GLubyte>(rgb[0] * factor), static_cast<GLubyte>(rgb[1] * factor),
                             static_cast<GLubyte>(rgb[2] * factor), opacity};
        };
        vertex(glyph->m_sQuad.bl, bounds.getMinX(), bounds.getMinY());
        vertex(glyph->m_sQuad.br, bounds.getMaxX(), bounds.getMinY());
        vertex(glyph->m_sQuad.tl, bounds.getMinX(), bounds.getMaxY());
        vertex(glyph->m_sQuad.tr, bounds.getMaxX(), bounds.getMaxY());
        glyph->setDirty(true);
    }
}
}
namespace {
// 문자열, 색, 투명도가 바뀔 때마다 그라데이션을 다시 입히는 BMFont 라벨.
// m_applying은 refresh 중 재진입을 막는다.
class CosmeticLabel final : public CCLabelBMFont {
    Cosmetics m_cosmetics;
    bool m_styled = false, m_applying = false;
    void refresh() {
        if (!m_styled || m_applying) return;
        m_applying = true;
        tintNickname(this, m_cosmetics);
        m_applying = false;
    }

public:
    void configure(Cosmetics const& value) {
        m_cosmetics = value;
        m_styled = true;
        refresh();
    }
    void updateDisplayedOpacity(GLubyte opacity) override {
        CCLabelBMFont::updateDisplayedOpacity(opacity);
        refresh();
    }
    void updateDisplayedColor(ccColor3B const& color) override {
        CCLabelBMFont::updateDisplayedColor(color);
        refresh();
    }
    void setString(char const* text) override {
        CCLabelBMFont::setString(text);
        refresh();
    }
    void setString(char const* text, bool update) override {
        CCLabelBMFont::setString(text, update);
        if (update) refresh();
    }
};
}
// CosmeticLabel을 생성한다.
CCLabelBMFont* createCosmeticLabel(char const* text, char const* font) {
    auto* label = new CosmeticLabel;
    if (label->initWithString(text, font)) {
        label->autorelease();
        return label;
    }
    delete label;
    return nullptr;
}
// 닉네임 글꼴 번호에 해당하는 gjFont<번호>.fnt가 실제로 있는지 확인하고 결과를 캐시한다.
// 기본(0)이거나 범위 밖, 파일 없음이면 nullptr.
char const* nicknameFontFile(int font) {
    static std::array<std::string, kNicknameFontCount> files;
    static std::array<signed char, kNicknameFontCount> present{};
    if (font <= 0 || font >= kNicknameFontCount) return nullptr;
    if (present[font] == 0) {
        files[font] = fmt::format("gjFont{:02}.fnt", font);
        auto* utils = CCFileUtils::sharedFileUtils();
        auto const path = std::string(utils->fullPathForFilename(files[font].c_str(), false));
        present[font] = !path.empty() && utils->isFileExist(path) ? 1 : -1;
    }
    return present[font] > 0 ? files[font].c_str() : nullptr;
}
// 글꼴 번호의 표시 이름.
std::string nicknameFontName(int font) {
    return font <= 0 ? std::string("Default") : fmt::format("Font {}", font);
}
namespace {
// 글꼴을 바꾸기 전 라벨의 원래 fnt 파일, 배율, 크기를 userObject로 기억해 두는 객체.
class FontMemory final : public CCObject {
public:
    std::string file;
    float scaleX = 1.f, scaleY = 1.f;
    CCSize size;
};
constexpr char const* kFontMemory = "tipp7.versus/nickname-font";
// 닉네임 글꼴을 적용한다. 원래 글꼴을 처음 한 번 기억해 두고, 글꼴 없음이면 복원한다.
// 바꾼 뒤에는 원래 라벨 박스 안에 맞도록 배율을 다시 계산한다.
void applyNicknameFont(CCLabelBMFont* label, int font) {
    auto* memory = typeinfo_cast<FontMemory*>(label->getUserObject(kFontMemory));
    char const* file = nicknameFontFile(font);
    if (!file) {
        if (!memory) return;
        label->setFntFile(memory->file.c_str());
        label->setScaleX(memory->scaleX);
        label->setScaleY(memory->scaleY);
        return;
    }
    if (!memory) {
        auto* fresh = new FontMemory();
        fresh->autorelease();
        auto const* original = label->getFntFile();
        if (!original || !*original) return;
        fresh->file = original;
        fresh->scaleX = label->getScaleX();
        fresh->scaleY = label->getScaleY();
        fresh->size = label->getContentSize();
        label->setUserObject(kFontMemory, fresh);
        memory = fresh;
    }
    float const boxWidth = memory->size.width * memory->scaleX, boxHeight = memory->size.height * memory->scaleY;
    label->setFntFile(file);
    auto const size = label->getContentSize();
    if (size.width <= 0.f || size.height <= 0.f || boxWidth <= 0.f || boxHeight <= 0.f) return;
    label->setScale(std::min(boxWidth / size.width, boxHeight / size.height));
}
}
// 글꼴은 모든 에디션에서, 그라데이션 색은 멤버십일 때만 적용한다.
void styleNickname(CCLabelBMFont* label, bool membership, Cosmetics const& cosmetics) {
    if (!label) return;
    applyNicknameFont(label, cosmetics.font);
    if (!membership) return;
    if (auto* own = typeinfo_cast<CosmeticLabel*>(label))
        own->configure(cosmetics);
    else
        tintNickname(label, cosmetics);
}
}
