// Season 2 컵 선택 화면과 티어 안내 팝업 (VersusUI.cpp에 #include되는 조각).
// 앞부분은 컵(standard / demon)별 이름, 이미지, 색 같은 표시 도우미와 맵 풀 / 길이 / 생명 문구 생성기,
// Season2TiersPopup은 TIERS / LIVES / R+ POINTS 세 페이지로 규칙을 보여 주는 팝업,
// Season2Layer는 두 컵 카드(티어, 레이팅, 접속자 수, PLAY)를 보여 주는 화면이다.
// 컵 ID를 표시 이름으로 바꾼다. 컵별 티어 이미지 / 왕관 / 패널 색 / 창 프레임은 아래 도우미들이 정한다.
std::string season2CupName(std::string const& cup) {
    return cup == "demon" ? "Demon Cup" : cup == "standard" ? "Standard Cup" : cup;
}
std::string season2CupArt(std::string const& cup) {
    return cup == "demon" ? "Extreme Demon" : "Insane";
}
char const* season2CupCrown(std::string const& cup) {
    return cup == "demon" ? "gj_weeklyCrown_001.png" : "gj_dailyCrown_001.png";
}
ccColor3B season2CupPanel(std::string const& cup) {
    return cup == "demon" ? ccc3(84, 84, 84) : ccc3(153, 85, 51);
}
char const* season2CupWindow(std::string const& cup) {
    return cup == "demon" ? "GJ_square05.png" : "GJ_square01.png";
}
// 왕관 이미지를 카드 위로 얼마나 튀어나오게 놓아야 하는지(높이)를 계산한다. sink는 카드에 잠기는 비율, drop은 추가로 내리는 값.
float season2CrownAbove(char const* frame, float width, float sink, float drop) {
    auto* crown = loadedFrameSprite(frame, "GJ_GameSheet04.plist");
    if (!crown) return 0.f;
    auto const size = crown->getContentSize();
    return std::max(0.f, width * size.height / std::max(1.f, size.width) * (1.f - sink) - drop);
}
// 왕관 스프라이트에서 흰 테두리가 끝나는 지점을 아래에서부터의 높이 비율로 구한다.
// 프레임을 렌더 텍스처에 그려 픽셀을 훑고, 실패하면 GD 기본값(kGDSeam)을 쓴다. 결과는 프레임별로 캐시한다.
float season2CrownSeam(char const* frame) {
    constexpr float kGDSeam = .307f;
    static std::unordered_map<std::string, float> found;
    if (auto known = found.find(frame); known != found.end()) return known->second;
    float seam = kGDSeam;
    auto* crown = loadedFrameSprite(frame, "GJ_GameSheet04.plist");
    auto const size = crown ? crown->getContentSize() : CCSize{};
    if (crown && size.width >= 8.f && size.height >= 8.f && size.width <= 512.f && size.height <= 512.f) {
        auto* target =
            CCRenderTexture::create(static_cast<int>(std::ceil(size.width)), static_cast<int>(std::ceil(size.height)),
                                    kCCTexture2DPixelFormat_RGBA8888);
        if (target) {
            crown->setAnchorPoint({0.f, 0.f});
            crown->setPosition({0.f, 0.f});
            target->beginWithClear(0.f, 0.f, 0.f, 0.f);
            crown->visit();
            target->end();
            if (auto* image = target->newCCImage(true)) {
                int const w = image->getWidth(), h = image->getHeight();
                auto const* data = image->getData();
                auto pixel = [data, w](int x, int y) { return data + 4 * (static_cast<std::size_t>(y) * w + x); };
                auto whiteEdge = [&](int y, bool left) {
                    for (int k = 0; k < w; ++k) {
                        auto const* p = pixel(left ? k : w - 1 - k, y);
                        if (p[3] >= 128)
                            return p[0] * 255 >= 200 * p[3] && p[1] * 255 >= 200 * p[3] && p[2] * 255 >= 200 * p[3];
                    }
                    return false;
                };
                if (data && w > 0 && h > 0 && image->getBitsPerComponent() == 8) {
                    for (int y = h - 1; y >= 0; --y) {
                        if (!whiteEdge(y, true) && !whiteEdge(y, false)) continue;
                        float const factor = std::max(1.f, CCDirector::sharedDirector()->getContentScaleFactor());
                        float const fraction = ((h - 1 - y) + .5f) / factor / size.height;
                        if (fraction > .05f && fraction < .8f) seam = fraction;
                        break;
                    }
                }
                image->release();
            }
        }
    }
    log::info("Versus: Season 2 crown {}: its white outline ends {:.3f} of its height up", frame, seam);
    found.emplace(frame, seam);
    return seam;
}
char const* season2CupTrophy(std::string const& cup) {
    return cup == "demon" ? "rankIcon_top500_001.png" : "rankIcon_top10_001.png";
}
// 컵 이름 글자에 가로 방향 색 변화를 입힌다: 앞 60%는 기본색, 이후 끝색으로 섞는다.
void season2CupColours(CCLabelBMFont* text, std::string const& cup) {
    if (!text || !text->getChildren()) return;
    bool const demon = cup == "demon";
    ccColor3B const main = demon ? ccc3(206, 210, 222) : ccc3(255, 214, 58);
    ccColor3B const end = demon ? ccc3(64, 156, 255) : ccc3(255, 64, 48);
    float const width = std::max(1.f, text->getContentSize().width);
    for (auto* child : CCArrayExt<CCNode*>(text->getChildren()))
        if (auto* glyph = typeinfo_cast<CCSprite*>(child)) {
            float const t = std::clamp(child->getPositionX() / width, 0.f, 1.f);
            glyph->setColor(t < .6f ? main : mixColor(main, end, (t - .6f) / .4f));
        }
}
// 맵 풀 비트마스크를 문구로 바꾼다. 하위 8비트는 별 2~9, 상위 비트(8~12)는 Demon 난이도(Easy~Extreme).
// 연속된 별은 "a-b Stars"로 묶는다.
std::string season2PoolText(int mask) {
    std::vector<int> stars;
    for (int bit = 0; bit < 8; ++bit)
        if (mask & (1 << bit)) stars.push_back(bit + 2);
    std::string text;
    if (!stars.empty()) {
        bool const run = stars.back() - stars.front() + 1 == static_cast<int>(stars.size());
        if (stars.size() == 1)
            text = fmt::format("{} Stars", stars.front());
        else if (run)
            text = fmt::format("{}-{} Stars", stars.front(), stars.back());
        else {
            for (size_t i = 0; i < stars.size(); ++i) text += (i ? ", " : "") + std::to_string(stars[i]);
            text += " Stars";
        }
    }
    static constexpr char const* demons[] = {"Easy", "Medium", "Hard", "Insane", "Extreme"};
    std::string demon;
    for (int bit = 0; bit < 5; ++bit)
        if (mask & (1 << (bit + 8))) demon += (demon.empty() ? "" : " / ") + std::string(demons[bit]);
    if (!demon.empty()) text += (text.empty() ? "" : " + ") + demon + " Demon";
    return text.empty() ? std::string("-") : text;
}
// 허용 맵 길이(0=Tiny ~ 4=XL) 문구. 비었거나 전부면 "Any length", 연속이면 범위로 묶는다.
std::string season2LengthText(std::vector<int> const& lengths) {
    static constexpr char const* names[] = {"Tiny", "Short", "Medium", "Long", "XL"};
    std::vector<int> valid;
    for (int length : lengths)
        if (length >= 0 && length <= 4) valid.push_back(length);
    std::sort(valid.begin(), valid.end());
    if (valid.empty() || valid.size() == 5) return "Any length";
    if (valid.back() - valid.front() + 1 == static_cast<int>(valid.size()))
        return valid.size() == 1 ? std::string(names[valid.front()])
                                 : fmt::format("{}-{}", names[valid.front()], names[valid.back()]);
    std::string text;
    for (size_t i = 0; i < valid.size(); ++i) text += (i ? ", " : "") + std::string(names[valid[i]]);
    return text;
}
// 허용 맵 등급(Rated ~ Mythic) 문구. 비었거나 전부면 "Any rating".
std::string season2FeatureText(std::vector<int> const& features) {
    static constexpr char const* names[] = {"Rated", "Featured", "Epic", "Legendary", "Mythic"};
    std::vector<int> valid;
    for (int feature : features)
        if (feature >= 0 && feature <= 4) valid.push_back(feature);
    std::sort(valid.begin(), valid.end());
    if (valid.empty() || valid.size() == 5) return "Any rating";
    std::string text;
    for (size_t i = 0; i < valid.size(); ++i) text += (i ? " / " : "") + std::string(names[valid[i]]);
    return text;
}
// 생명 수 문구. 길이별 비율(byLength, 100이면 기본)이 다른 허용 길이는 괄호 안에 반올림한 값으로 덧붙인다.
std::string season2LivesText(int lives, std::vector<int> const& byLength, std::vector<int> const& lengths) {
    static constexpr char const* names[] = {"Tiny", "Short", "Medium", "Long", "XL"};
    std::string text = fmt::format("{} {}", lives, lives == 1 ? "life" : "lives"), extra;
    for (int length = 0; length < 5 && length < static_cast<int>(byLength.size()); ++length) {
        if (byLength[length] == 100) continue;
        if (!lengths.empty() && std::find(lengths.begin(), lengths.end(), length) == lengths.end()) continue;
        int const value = std::max(1, static_cast<int>(std::floor(lives * byLength[length] / 100.0 + .5)));
        extra += fmt::format("{}{} {}", extra.empty() ? "" : ", ", names[length], value);
    }
    return extra.empty() ? text : text + " (" + extra + ")";
}
// 티어의 맵 조건 문구: GDDL 티어 범위가 있으면 그것, 아니면 맵 풀 마스크.
std::string season2MapText(Season2TierRow const& tier) {
    if (tier.gddlLow >= 0)
        return tier.gddlLow == tier.gddlHigh ? fmt::format("GDDL tier {}", tier.gddlLow)
                                             : fmt::format("GDDL tiers {}-{}", tier.gddlLow, tier.gddlHigh);
    return season2PoolText(tier.mask);
}
// 현재 레이팅 기준 다음 티어까지 남은 점수 문구. 이미 최상위면 안내 문구.
std::string season2NextTier(Season2Tiers const& tiers, double rating) {
    int const shown = static_cast<int>(std::lround(rating));
    Season2TierRow const* next = nullptr;
    for (auto const& tier : tiers.tiers)
        if (tier.minRating > shown && (!next || tier.minRating < next->minRating)) next = &tier;
    if (!next) return "You are in the top tier";
    return fmt::format("{} more to {} (R{}+)", next->minRating - shown, season2TierName(next->name), next->minRating);
}

// 컵의 티어 표를 서버에서 받아 세 페이지(TIERS / LIVES / R+ POINTS)로 보여 주는 팝업.
// 좌우 화살표로 페이지를 넘기고, R+ POINTS에서는 WIN / LOSS 탭으로 표를 바꾼다.
// 내 프로필(self)이 있으면 내 티어 행을 강조하고 하단에 요약 한 줄을 보여 준다.
class Season2TiersPopup final : public ::versus::DeferredPopup {
    // 레이아웃 상수 (팝업 크기, 목록 영역, 행 높이, 제목 판, 화살표 위치, 열 x 좌표).
    static constexpr float W = 440.f, H = 262.f, ListW = 416.f, ListH = 180.f, ListY = 52.f, RowH = 46.f;
    static constexpr float LifeRowH = 34.f, kTitleGap = 17.f, kArrowGap = 26.f, kArrowH = 34.f;
    static constexpr float kDrop = 12.f, kPlateH = 30.f, kPlatePad = 26.f;
    static constexpr GLubyte kPlateOpacity = 175;
    static constexpr int kPages = 3;
    static constexpr float LifeX[5] = {226.f, 270.f, 314.f, 358.f, 402.f};
    static constexpr float PointsLeft = 14.f, PointsFirst = 122.f, PointsRight = 426.f;
    std::string m_cup;
    std::optional<RankedProfile> m_self;
    std::optional<Season2Tiers> m_tiers;
    std::string m_error;
    bool m_loaded = false;
    int m_page = 0;
    bool m_win = true;
    CCNode* m_view = nullptr;
    CCLabelBMFont* m_title = nullptr;
    CCNode* m_titlePlate = nullptr;
    CCMenuItemSpriteExtra *m_winTab = nullptr, *m_lossTab = nullptr;
    // 창, 제목 판, 페이지 화살표, WIN / LOSS 탭을 만들고 티어 표를 요청한다 (응답이 오면 render).
    bool init(std::string cup, std::optional<RankedProfile> self) {
        if (!Popup::init(W, H, "GJ_square02.png")) return false;
        m_cup = std::move(cup);
        m_self = std::move(self);
        setID("season2-tiers-window"_spr);
        m_mainLayer->setPositionY(m_mainLayer->getPositionY() - kDrop);
        auto* plate = NineSlice::create("square02b_001.png");
        plate->setColor(ccc3(0, 0, 0));
        plate->setOpacity(kPlateOpacity);
        plate->setPosition({W / 2.f, H + kTitleGap});
        plate->setID("season2-tiers-title-plate"_spr);
        m_mainLayer->addChild(plate, 4);
        m_titlePlate = plate;
        m_title = label(m_mainLayer, "", {W / 2.f, H + kTitleGap}, .55f, W - 40.f, ccWHITE, false, "bigFont.fnt");
        m_title->setZOrder(5);
        for (int side = -1; side <= 1; side += 2) {
            auto* arrow = loadedFrameSprite("GJ_arrow_03_001.png", "GJ_GameSheet03.plist");
            if (!arrow) break;
            arrow->setScale(kArrowH / std::max(1.f, arrow->getContentSize().height));
            arrow->setFlipX(side > 0);
            auto* item = CCMenuItemSpriteExtra::create(arrow, this,
                                                       side < 0 ? menu_selector(Season2TiersPopup::onPrevious)
                                                                : menu_selector(Season2TiersPopup::onNext));
            item->setPosition({side < 0 ? -kArrowGap : W + kArrowGap, H / 2.f});
            item->setID(side < 0 ? "season2-tiers-previous"_spr : "season2-tiers-next"_spr);
            m_buttonMenu->addChild(item);
        }
        m_winTab = button(m_buttonMenu, this, menu_selector(Season2TiersPopup::onWin), "WIN",
                          {W / 2.f - 38.f, H - 20.f}, .45f);
        m_lossTab = button(m_buttonMenu, this, menu_selector(Season2TiersPopup::onLoss), "LOSS",
                           {W / 2.f + 38.f, H - 20.f}, .45f, "GJ_button_06.png");
        cascadeOpacity(m_winTab);
        cascadeOpacity(m_lossTab);
        render();
        Service::get().fetchSeason2Tiers(
            m_cup, [self = Weak<Season2TiersPopup>(this)](std::optional<Season2Tiers> tiers, std::string error) {
                auto owner = self.lock();
                if (!owner) return;
                owner->m_loaded = true;
                owner->m_tiers = std::move(tiers);
                owner->m_error = std::move(error);
                owner->render();
            });
        return true;
    }
    void onPrevious(CCObject*) {
        m_page = (m_page + kPages - 1) % kPages;
        render();
    }
    void onNext(CCObject*) {
        m_page = (m_page + 1) % kPages;
        render();
    }
    void onWin(CCObject*) {
        if (!m_win) {
            m_win = true;
            render();
        }
    }
    void onLoss(CCObject*) {
        if (m_win) {
            m_win = false;
            render();
        }
    }
    // 현재 페이지에 맞는 제목 / 탭을 갱신하고 내용을 다시 만든다. 표를 아직 못 받았으면 로딩 / 오류 문구.
    void render() {
        static constexpr char const* titles[kPages] = {"TIERS", "LIVES", "R+ POINTS"};
        m_title->setString(fmt::format("{} {}", season2CupName(m_cup), titles[m_page]).c_str());
        m_title->limitLabelWidth(W - 40.f, .55f, .2f);
        m_titlePlate->setContentSize({m_title->getScaledContentSize().width + kPlatePad, kPlateH});
        bool const tabs = m_page == 2 && m_tiers && m_tiers->points;
        m_winTab->setVisible(tabs);
        m_lossTab->setVisible(tabs);
        m_winTab->setOpacity(m_win ? 255 : 110);
        m_lossTab->setOpacity(m_win ? 110 : 255);
        if (m_view) m_view->removeFromParent();
        m_view = CCNode::create();
        m_mainLayer->addChild(m_view, 2);
        if (!m_tiers) {
            label(m_view,
                  !m_loaded         ? "LOADING..."
                  : m_error.empty() ? "Could not load the tiers."
                                    : m_error,
                  {W / 2.f, H / 2.f}, .4f, ListW - 20.f, kIce);
            return;
        }
        if (m_page == 0)
            tiersPage();
        else if (m_page == 1)
            livesPage();
        else
            pointsPage();
    }
    // placed: 배치 경기를 마쳤는지. mine: 이 티어 행이 내 티어인지.
    bool placed() const { return m_self && m_self->placed; }
    bool mine(Season2TierRow const& tier) const { return placed() && m_self->tierName == tier.name; }
    // 스크롤 목록 공통 틀. 티어 행을 위에서부터(최상위 티어 먼저) 만들고 맨 아래에 Placement 행을 붙인다.
    // 내 티어 행이 보이도록 스크롤 위치를 맞춘다.
    void list(float rowH, std::function<void(CCNode*, float, Season2TierRow const&, bool, bool)> const& row) {
        auto* paper = NineSlice::create("square02b_001.png");
        paper->setContentSize({ListW, ListH + 4.f});
        paper->setPosition({W / 2.f, ListY + ListH / 2.f});
        paper->setColor({12, 22, 44});
        paper->setOpacity(200);
        m_view->addChild(paper);
        auto* scroll = ScrollLayer::create({ListW, ListH});
        scroll->setPosition({12.f, ListY});
        m_view->addChild(scroll);
        auto* content = scroll->m_contentLayer;
        size_t const count = m_tiers->tiers.size() + 1;
        float const height = std::max(ListH, count * rowH + 4.f);
        content->setContentSize({ListW, height});
        float myY = -1.f;
        for (size_t i = 0; i < m_tiers->tiers.size(); ++i) {
            float const y = height - 2.f - rowH * (i + 1);
            bool const own = mine(m_tiers->tiers[i]);
            if (own) myY = y;
            row(content, y, m_tiers->tiers[i], false, own);
        }
        row(content, height - 2.f - rowH * count, m_tiers->placement, true, m_self && !placed());
        scroll->scrollToTop();
        if (myY >= 0.f) content->setPositionY(std::clamp(ListH - (myY + rowH * 1.5f), ListH - height, 0.f));
    }
    // 행 배경 패널 (내 행이면 강조색)과 티어 이름 표시.
    CCNode* rowBack(CCNode* content, float y, float rowH, bool own) {
        auto* back = panel(content, {4.f, y + 2.f}, {ListW - 8.f, rowH - 4.f}, false);
        if (own)
            if (auto* fill = back->getChildByType<NineSlice*>(0)) {
                fill->setColor(ccc3(90, 70, 18));
                fill->setOpacity(235);
            }
        return back;
    }
    void tierName(CCNode* back, CCPoint at, Season2TierRow const& tier, bool placement, float scale, float width) {
        auto* name = Season2TierText::create();
        name->setPosition(at);
        back->addChild(name);
        name->set(placement ? std::string("Placement") : tier.name, placement ? std::string("Unranked") : tier.tier,
                  scale, width, false);
    }
    // 하단의 "YOU: ..." 요약 한 줄.
    void you(std::string const& text) {
        auto* line = label(m_view, text, {W / 2.f, 34.f}, .3f, W - 30.f, ccc3(255, 214, 60), false, "bigFont.fnt");
        line->limitLabelWidth(W - 30.f, .3f, .15f);
    }
    // TIERS 페이지: 티어별 이미지, 이름, 최소 레이팅 (상위 N위 / N% 조건 포함), 맵 조건,
    // 길이 / 등급 / 생명 제한을 한 행에 보여 준다.
    void tiersPage() {
        label(m_view, "TIER", {112.f, H - 18.f}, .28f, 120.f, kIce);
        label(m_view, "RATING", {214.f, H - 18.f}, .28f, 80.f, kIce);
        label(m_view, "MAPS", {340.f, H - 18.f}, .28f, 150.f, kIce);
        list(RowH, [this](CCNode* content, float y, Season2TierRow const& tier, bool placement, bool own) {
            auto* back = rowBack(content, y, RowH, own);
            float const mid = (RowH - 4.f) / 2.f;
            season2Face(back, placement ? std::string("Unranked") : tier.tier, {30.f, mid}, .55f);
            tierName(back, {108.f, mid + (own ? 5.f : 0.f)}, tier, placement, .42f, 104.f);
            if (own) label(back, "YOU", {108.f, mid - 11.f}, .4f, 40.f, ccc3(255, 214, 60), false, "chatFont.fnt");
            std::string rating = placement ? fmt::format("first {} games", m_tiers->placementGames)
                                           : fmt::format("R{}+", tier.minRating);
            std::string limit = tier.topRank > 0       ? fmt::format("top {}", tier.topRank)
                                : tier.topPercent > 0. ? fmt::format("top {:.4g}%", tier.topPercent)
                                                       : std::string();
            label(back, rating, {206.f, mid + (limit.empty() ? 0.f : 6.f)}, .32f, 84.f, ccWHITE, false, "bigFont.fnt");
            if (!limit.empty())
                label(back, limit, {206.f, mid - 8.f}, .26f, 84.f, ccc3(255, 190, 110), false, "bigFont.fnt");
            label(back, season2MapText(tier), {332.f, mid + 7.f}, .52f, 150.f, ccWHITE, false, "chatFont.fnt");
            std::string extra;
            auto add = [&](std::string const& part) { extra += (extra.empty() ? "" : "  |  ") + part; };
            if (auto const lengths = season2LengthText(tier.lengths); lengths != "Any length") add(lengths);
            if (auto const ratings = season2FeatureText(tier.features); ratings != "Any rating") add(ratings);
            if (tier.lives > 0) add(season2LivesText(tier.lives, m_tiers->livesByLength, tier.lengths));
            label(back, extra, {332.f, mid - 8.f}, .45f, 150.f, kMuted, false, "chatFont.fnt");
        });
        std::string line =
            !m_self     ? std::string()
            : !placed() ? fmt::format("YOU: placement {} / {}", m_self->placementCompleted, m_tiers->placementGames)
                        : fmt::format("YOU: {}  R{:.0f}   |   {}",
                                      m_self->tierName.empty() ? m_self->tier : season2TierName(m_self->tierName),
                                      m_self->rating, season2NextTier(*m_tiers, m_self->rating));
        you(line);
        label(m_view, "Each cup has its own rating and tiers. The higher your tier, the harder the maps.",
              {W / 2.f, 17.f}, .42f, W - 30.f, kIce, false, "chatFont.fnt");
    }
    // 해당 티어가 그 맵 길이에서 가지는 생명 수. 허용되지 않는 길이면 0.
    // 길이별 비율을 반올림해 1~99로 제한한다.
    int livesOn(Season2TierRow const& tier, int length) const {
        if (tier.lives <= 0) return 0;
        if (!tier.lengths.empty() && std::find(tier.lengths.begin(), tier.lengths.end(), length) == tier.lengths.end())
            return 0;
        int const percent =
            length < static_cast<int>(m_tiers->livesByLength.size()) ? m_tiers->livesByLength[length] : 100;
        return std::clamp(static_cast<int>(std::floor(tier.lives * percent / 100.0 + .5)), 1, 99);
    }
    // LIVES 페이지의 열 묶음: Tiny~Medium은 한 열, Long과 XL은 각각 한 열.
    struct LifeColumn {
        int first, last;
        float x, w;
        char const* name;
        ccColor3B tint;
    };
    static constexpr LifeColumn kLifeColumns[3] = {
        {0, 2, (LifeX[0] + LifeX[2]) / 2.f, LifeX[2] - LifeX[0] + 40.f, "TINY - MEDIUM", {150, 225, 120}},
        {3, 3, LifeX[3], 40.f, "LONG", {255, 150, 80}},
        {4, 4, LifeX[4], 40.f, "XL", {255, 96, 96}},
    };
    // 값 목록에서 0과 중복을 제거해 "a / b" 형태로 잇는다. 모두 없으면 "-".
    static std::string joined(std::vector<int> values, char const* suffix) {
        values.erase(std::remove(values.begin(), values.end(), 0), values.end());
        values.erase(std::unique(values.begin(), values.end()), values.end());
        if (values.empty()) return "-";
        std::string text;
        for (size_t i = 0; i < values.size(); ++i) text += (i ? " / " : "") + std::to_string(values[i]);
        return text + suffix;
    }
    int lengthPercent(int length) const {
        return length < static_cast<int>(m_tiers->livesByLength.size()) ? m_tiers->livesByLength[length] : 100;
    }
    // LIVES 페이지: 열마다 길이별 생명 비율(%)을 머리글로 보여 주고, 행마다 티어별 생명 수를 보여 준다.
    void livesPage() {
        auto* heart = createLifeHeart(12.f);
        heart->setPosition({40.f, H - 18.f});
        m_view->addChild(heart, 3);
        label(m_view, "TIER", {112.f, H - 18.f}, .28f, 120.f, kIce);
        for (auto const& column : kLifeColumns) {
            std::vector<int> percents;
            for (int length = column.first; length <= column.last; ++length) percents.push_back(lengthPercent(length));
            auto* chip = NineSlice::create("square02b_001.png");
            chip->setContentSize({column.w, 22.f});
            chip->setPosition({column.x, H - 16.f});
            chip->setColor(column.tint);
            chip->setOpacity(70);
            m_view->addChild(chip, 1);
            label(m_view, column.name, {column.x, H - 12.f}, .2f, column.w - 4.f, ccWHITE, false, "bigFont.fnt")
                ->setZOrder(2);
            label(m_view, joined(percents, "%"), {column.x, H - 21.f}, .34f, column.w - 4.f, column.tint, false,
                  "chatFont.fnt")
                ->setZOrder(2);
        }
        list(LifeRowH, [this](CCNode* content, float y, Season2TierRow const& tier, bool placement, bool own) {
            auto* back = rowBack(content, y, LifeRowH, own);
            float const mid = (LifeRowH - 4.f) / 2.f;
            season2Face(back, placement ? std::string("Unranked") : tier.tier, {24.f, mid}, .4f);
            tierName(back, {108.f, mid}, tier, placement, .36f, 120.f);
            if (own) label(back, "YOU", {184.f, mid}, .36f, 30.f, ccc3(255, 214, 60), false, "chatFont.fnt");
            for (auto const& column : kLifeColumns) {
                std::vector<int> lives;
                for (int length = column.first; length <= column.last; ++length) lives.push_back(livesOn(tier, length));
                auto const text = joined(lives, "");
                bool const any = text != "-";
                float const x = column.x - 12.f;
                auto* pill = NineSlice::create("square02b_001.png");
                pill->setContentSize({column.w - 2.f, LifeRowH - 10.f});
                pill->setPosition({x, mid});
                pill->setColor(any ? ccc3(255, 120, 150) : ccc3(80, 80, 90));
                pill->setOpacity(any ? 60 : 30);
                back->addChild(pill, 2);
                label(back, text, {x, mid}, .34f, column.w - 6.f, any ? ccWHITE : kMuted, false, "bigFont.fnt")
                    ->setZOrder(3);
            }
        });
        std::string line;
        if (m_self)
            for (auto const& tier : m_tiers->tiers)
                if (mine(tier)) {
                    line = fmt::format("YOU: {}  |  {}", season2TierName(tier.name),
                                       season2LivesText(tier.lives, m_tiers->livesByLength, tier.lengths));
                    break;
                }
        if (m_self && !placed())
            line =
                fmt::format("YOU: placement  |  {}", season2LivesText(m_tiers->placement.lives, m_tiers->livesByLength,
                                                                      m_tiers->placement.lengths));
        you(line);
        label(m_view,
              "Your tier sets your lives. Longer maps give fewer (the % above). \"-\": the tier has no such maps.",
              {W / 2.f, 17.f}, .42f, W - 30.f, kIce, false, "chatFont.fnt");
    }
    // R+ POINTS 표의 행 이름 (레이팅 구간): "N+", "Below N", "a-b".
    static std::string bandName(Season2PointsRow const& row) {
        if (row.below < 0) return fmt::format("{}+", row.from);
        if (row.from <= 0) return fmt::format("Below {}", row.below);
        return fmt::format("{}-{}", row.from, row.below - 1);
    }
    // 상대 레이팅 차이 열의 x 좌표.
    float pointsX(size_t column) const {
        auto const& diffs = m_tiers->points->diffs;
        float const pitch = (PointsRight - PointsFirst) / std::max<size_t>(1, diffs.size());
        return PointsFirst + pitch * (column + .5f);
    }
    // R+ POINTS 페이지: 내 레이팅 구간(행)과 상대와의 레이팅 차이(열)에 따른 승 / 패 점수 표 (최대 6행).
    // WIN 탭은 연승 보너스, LOSS 탭은 맵 경험에 따른 손실 감소를 아래에 설명하고,
    // 배치 경기와 신규 플레이어 구간의 K 값을 안내한다. 서버에 표가 없으면 안내 문구만 보인다.
    void pointsPage() {
        if (!m_tiers->points) {
            label(m_view, "This server does not have the points table yet.", {W / 2.f, H / 2.f}, .4f, ListW - 20.f,
                  kIce);
            return;
        }
        auto const& points = *m_tiers->points;
        ccColor3B const good = ccc3(130, 255, 165), bad = ccc3(255, 160, 160);
        label(m_view, m_win ? "Points for a WIN" : "Points for a LOSS", {(PointsLeft + PointsFirst) / 2.f, 216.f}, .28f,
              104.f, m_win ? good : bad);
        label(m_view, "OPPONENT'S RATING vs YOURS", {(PointsFirst + PointsRight) / 2.f, 216.f}, .26f,
              PointsRight - PointsFirst, kIce);
        label(m_view, "YOUR RATING", {(PointsLeft + PointsFirst) / 2.f, 202.f}, .28f, 104.f, kIce);
        for (size_t column = 0; column < points.diffs.size(); ++column) {
            int const diff = points.diffs[column];
            label(m_view, diff == 0 ? std::string("EVEN") : fmt::format("{:+}", diff), {pointsX(column), 202.f}, .28f,
                  40.f, diff < 0 ? kMuted : ccWHITE);
        }
        int own = -1;
        if (placed())
            for (size_t row = 0; row < points.rows.size(); ++row)
                if (m_self->rating >= points.rows[row].from &&
                    (points.rows[row].below < 0 || m_self->rating < points.rows[row].below))
                    own = static_cast<int>(row);
        size_t const shown = std::min<size_t>(points.rows.size(), 6);
        for (size_t row = 0; row < shown; ++row) {
            float const y = 185.f - 18.f * row;
            bool const mineRow = static_cast<int>(row) == own;
            auto* bg = panel(m_view, {PointsLeft, y - 9.f}, {PointsRight - PointsLeft, 18.f}, false);
            if (mineRow)
                if (auto* slice = bg->getChildByType<NineSlice*>(0)) {
                    slice->setColor(ccc3(40, 110, 70));
                    slice->setOpacity(220);
                }
            auto const& values = m_win ? points.rows[row].win : points.rows[row].loss;
            label(m_view, bandName(points.rows[row]), {(PointsLeft + PointsFirst) / 2.f + (mineRow ? 8.f : 0.f), y},
                  .48f, 92.f, mineRow ? ccc3(255, 230, 120) : ccWHITE, false, "chatFont.fnt");
            if (mineRow) label(m_view, "YOU", {20.f, y}, .2f, 24.f, ccc3(255, 230, 120), true);
            for (size_t column = 0; column < values.size(); ++column)
                label(m_view, fmt::format("{:+}", values[column]), {pointsX(column), y}, .52f, 42.f, m_win ? good : bad,
                      false, "chatFont.fnt");
        }
        auto* note = panel(m_view, {PointsLeft, 64.f}, {PointsRight - PointsLeft, 20.f}, false);
        std::string chips;
        if (m_win) {
            for (auto const& [wins, bonus] : points.streak) {
                if (bonus <= 0) continue;
                if (!chips.empty()) chips += "    ";
                chips += fmt::format("{} in a row +{}", wins, bonus);
            }
            label(note, "WIN STREAK", {8.f, 10.f}, .26f, 80.f, ccc3(255, 205, 126), true);
            label(note, chips, {(PointsRight - PointsLeft + 92.f) / 2.f, 10.f}, .44f, PointsRight - PointsLeft - 100.f,
                  good, false, "chatFont.fnt");
        } else {
            label(note, "MAP EXPERIENCE", {8.f, 10.f}, .26f, 90.f, ccc3(120, 228, 255), true);
            label(note,
                  fmt::format("a lower best % loses less: up to -{:.0f}% at a 100% gap", points.mapLossMax * 100.),
                  {(PointsRight - PointsLeft + 100.f) / 2.f, 10.f}, .44f, PointsRight - PointsLeft - 110.f,
                  ccc3(120, 228, 255), false, "chatFont.fnt");
        }
        label(m_view,
              fmt::format("Placement ({} games): K {}.  The next {} matches: at least K {}.  A draw: no change.",
                          points.placementGames, points.placementK, points.newPlayerMatches, points.newPlayerK),
              {W / 2.f, 50.f}, .4f, W - 30.f, kIce, false, "chatFont.fnt");
        std::string line;
        if (m_self && !placed())
            line = fmt::format("YOU: placement {} / {} (K {})", m_self->placementCompleted, points.placementGames,
                               points.placementK);
        else if (own >= 0) {
            auto const even = std::find(points.diffs.begin(), points.diffs.end(), 0);
            if (even != points.diffs.end()) {
                auto const column = static_cast<size_t>(even - points.diffs.begin());
                line = fmt::format("YOU: R{:.0f}. Vs an equal opponent: a win {:+}, a loss {:+}", m_self->rating,
                                   points.rows[own].win[column], points.rows[own].loss[column]);
            }
        }
        you(line);
        label(m_view, "Points follow your rating in this cup; tiers follow the ladder. Duels: no rating.",
              {W / 2.f, 17.f}, .4f, W - 30.f, kMuted, false, "chatFont.fnt");
    }
    // cup: 보여 줄 컵 ID, self: 내 프로필 (없으면 강조 없이 표만 표시).
public:
    static Season2TiersPopup* create(std::string cup, std::optional<RankedProfile> self = std::nullopt) {
        auto* result = new Season2TiersPopup;
        if (result->init(std::move(cup), std::move(self))) {
            result->autorelease();
            return result;
        }
        delete result;
        return nullptr;
    }
};

// Season 2 메인 화면. 컵 카드 두 장(standard, demon)에 티어 이미지, 레이팅 / 배치 진행,
// 순위, 접속자 수, PLAY 버튼을 보여 준다. 서버에서 10초마다 정보를 다시 받는다.
// 맵 / 오디오 다운로드 진행 상황도 하단에 표시한다 (lounge 모듈에서 가져옴).
// 1.0.8: 컵 카드의 맵 밴 창. 이 컵의 랭크에 나오지 않는 맵 목록을 모든 플레이어가 보고, 스태프는 레벨 ID 로
// GD 에서 레벨을 찾아 사유와 함께 밴하거나(이미 밴이면 사유 변경) 다시 넣을(Put Back) 수 있다. 사유와 밴한 스태프는
// 스태프에게만 보인다(서버가 staff 일 때만 보냄). 스태프의 모든 변경은 모더레이션 로그에 남는다.
class Season2MapBansPopup final : public ::versus::DeferredPopup {
    static constexpr float W = 440.f, H = 290.f;
    static constexpr int kRows = 4, kFoundRows = 3, kPlayerRows = 5;
    std::string m_cup;
    std::function<void()> m_changed;
    std::vector<StaffMapBan> m_maps;
    int m_limit = 0, m_page = 0;
    bool m_staff = false, m_loaded = false, m_busy = false, m_looking = false;
    std::string m_error, m_lookupError;
    std::optional<LevelInfo> m_found;
    TextInput* m_id = nullptr;
    TextInput* m_reason = nullptr;
    CCNode* m_rows = nullptr;
    CCNode* m_foundNode = nullptr;
    CCLabelBMFont* m_info = nullptr;
    CCLabelBMFont* m_status = nullptr;
    CCLabelBMFont* m_count = nullptr;
    CCLabelBMFont* m_pageLabel = nullptr;
    CCMenuItemSpriteExtra *m_previous = nullptr, *m_next = nullptr, *m_find = nullptr, *m_ban = nullptr;
    void onExit() override {
        if (m_id) m_id->defocus();
        if (m_reason) m_reason->defocus();
        Popup::onExit();
    }
    bool init(std::string cup, std::function<void()> changed) {
        if (!Popup::init(W, H, "GJ_square02.png")) return false;
        m_cup = std::move(cup);
        m_changed = std::move(changed);
        m_staff = Service::get().isAdmin();
        setTitle(fmt::format("{} - Banned Maps", season2CupName(m_cup)).c_str(), "goldFont.fnt", .65f);
        setID("versus-season2-map-bans"_spr);
        m_info = label(m_mainLayer, "", {W / 2.f, 248.f}, .4f, 400.f, kMuted, false, "chatFont.fnt");
        m_id = TextInput::create(130.f, "Level ID");
        m_id->setCommonFilter(CommonFilter::Uint);
        m_id->setMaxCharCount(10);
        m_id->setPosition({87.f, 222.f});
        styleInput(m_id);
        m_mainLayer->addChild(m_id);
        m_find = button(m_buttonMenu, this, menu_selector(Season2MapBansPopup::onFind), "Find", {190.f, 222.f}, .5f,
                        "GJ_button_02.png");
        m_count = rightLabel(m_mainLayer, "", {420.f, 222.f}, .4f, 150.f, kIce);
        m_foundNode = CCNode::create();
        m_mainLayer->addChild(m_foundNode);
        // 사유 입력은 찾은 레벨이 있을 때만 보인다(다시 그릴 때 입력한 글이 지워지지 않게 한 번만 만든다).
        m_reason = TextInput::create(300.f, "Reason (required; staff only)");
        m_reason->setCommonFilter(CommonFilter::Any);
        m_reason->setMaxCharCount(160);
        m_reason->setPosition({174.f, 164.f});
        styleInput(m_reason);
        m_mainLayer->addChild(m_reason, 3);
        m_ban = button(m_buttonMenu, this, menu_selector(Season2MapBansPopup::onBan), "Ban", {378.f, 164.f}, .5f,
                       "GJ_button_06.png");
        m_rows = CCNode::create();
        m_mainLayer->addChild(m_rows);
        m_status = label(m_mainLayer, "Loading...", {W / 2.f, 120.f}, .5f, 380.f, kIce, false, "chatFont.fnt");
        m_previous =
            button(m_buttonMenu, this, menu_selector(Season2MapBansPopup::onPrevious), "<", {170.f, 24.f}, .45f);
        m_next = button(m_buttonMenu, this, menu_selector(Season2MapBansPopup::onNext), ">", {270.f, 24.f}, .45f);
        m_pageLabel = label(m_mainLayer, "1 / 1", {W / 2.f, 24.f}, .33f, 70.f, kIce);
        iconButton(m_buttonMenu, this, menu_selector(Season2MapBansPopup::onRefresh), "GJ_updateBtn_001.png",
                   {408.f, 26.f}, 28.f);
        load();
        return true;
    }
    void received(std::optional<StaffMapBanList> list, std::string error) {
        m_loaded = true;
        m_busy = false;
        m_error = std::move(error);
        if (list) {
            m_error.clear();
            m_maps = std::move(list->maps);
            m_limit = list->limit;
            m_staff = list->staff;
        }
        render();
    }
    void load() {
        m_loaded = false;
        render();
        Service::get().fetchStaffMapBans(
            m_cup, [self = Weak<Season2MapBansPopup>(this)](std::optional<StaffMapBanList> list, std::string error) {
                if (auto owner = self.lock()) owner->received(std::move(list), std::move(error));
            });
    }
    StaffMapBan const* banned(int64_t id) const {
        for (auto const& map : m_maps)
            if (map.levelId == id) return &map;
        return nullptr;
    }
    static std::string mapName(std::string const& name, int64_t id) {
        return name.empty() ? fmt::format("Level {}", id) : name;
    }
    // 사유는 한글일 수 있어 ASCII 가 아니면 시스템 글꼴로 그린다.
    static void reasonText(CCNode* parent, std::string const& text, CCPoint at, float maxWidth, ccColor3B color) {
        auto shown = previewLine(text.empty() ? std::string("No reason") : text, 60);
        if (asciiOnly(shown)) {
            label(parent, shown, at, .36f, maxWidth, color, true, "chatFont.fnt");
            return;
        }
        auto* system = createSystemText(shown, 11.f, 0.f, kCCTextAlignmentLeft);
        system->setColor(color);
        system->setAnchorPoint({0.f, .5f});
        if (system->getContentSize().width > maxWidth) system->setScale(maxWidth / system->getContentSize().width);
        system->setPosition(at);
        parent->addChild(system);
    }
    // 스태프: 찾은 레벨 카드, 사유 입력/밴 버튼, 목록. 플레이어: 목록만(한 페이지에 더 많이).
    void render() {
        m_rows->removeAllChildrenWithCleanup(true);
        m_foundNode->removeAllChildrenWithCleanup(true);
        int const count = static_cast<int>(m_maps.size());
        m_info->setString(m_staff ? "Maps here never come up in this cup's Ranked matches until put back. "
                                    "Every change is logged."
                                  : "These maps never come up in this cup's Ranked matches.");
        m_info->limitLabelWidth(400.f, .4f, .2f);
        m_id->setVisible(m_staff);
        m_find->setVisible(m_staff);
        m_count->setString(!m_loaded                ? ""
                           : m_staff && m_limit > 0 ? fmt::format("{} / {} banned", count, m_limit).c_str()
                                                    : fmt::format("{} banned", count).c_str());
        m_count->setPosition(m_staff ? CCPoint{420.f, 222.f} : CCPoint{150.f, 24.f});  // (플레이어: 아래 줄 왼쪽)
        enabled(m_find, !m_busy && !m_looking);
        bool const showFound = m_staff && (m_looking || m_found.has_value());
        m_reason->setVisible(m_staff && m_found.has_value());
        m_ban->setVisible(m_staff && m_found.has_value());
        if (showFound) {
            auto* cell = panel(m_foundNode, {16.f, 182.f}, {408.f, 30.f}, false);
            if (m_looking)
                label(cell, "Looking the level up on GD...", {204.f, 15.f}, .42f, 380.f, kIce, false, "chatFont.fnt");
            else {
                auto const& level = *m_found;
                float x = 8.f;
                if (m_lookupError.empty())
                    if (auto* face = mapDifficulty(level)) {
                        face->setScale(22.f / std::max(1.f, face->getContentSize().height));
                        face->setPosition({20.f, 15.f});
                        cell->addChild(face);
                        x = 36.f;
                    }
                bool const already = banned(level.id) != nullptr;
                label(cell, mapName(level.name, level.id), {x, 21.f}, .34f, already ? 200.f : 360.f - x, ccWHITE, true);
                label(cell,
                      m_lookupError.empty()
                          ? fmt::format("by {}  |  ID {}", level.creator.empty() ? "?" : level.creator, level.id)
                          : fmt::format("ID {}  |  {} You can still ban it by its ID.", level.id, m_lookupError),
                      {x, 8.f}, .34f, 370.f - x, kMuted, true, "chatFont.fnt");
                if (auto const* ban = banned(level.id))
                    rightLabel(cell, "Banned by " + (ban->byName.empty() ? std::string("Admin") : ban->byName),
                               {400.f, 21.f}, .36f, 140.f, ccc3(255, 130, 115));
            }
            if (m_found) {
                auto* sprite = ButtonSprite::create(banned(m_found->id) ? "Save" : "Ban", "goldFont.fnt",
                                                    "GJ_button_06.png");
                sprite->setScale(.5f);
                m_ban->setNormalImage(sprite);
                m_ban->updateSprite();
                enabled(m_ban, !m_busy);
            }
        }
        int const perPage = !m_staff ? kPlayerRows : showFound ? kFoundRows : kRows;
        float const listTop = !m_staff ? 240.f : showFound ? 148.f : 206.f;
        int const pages = std::max(1, (count + perPage - 1) / perPage);
        m_page = std::clamp(m_page, 0, pages - 1);
        m_pageLabel->setString(fmt::format("{} / {}", m_page + 1, pages).c_str());
        enabled(m_previous, m_page > 0);
        enabled(m_next, m_page + 1 < pages);
        m_status->setVisible(!m_loaded || !m_error.empty() || count == 0);
        m_status->setString(!m_loaded          ? "Loading..."
                            : !m_error.empty() ? m_error.c_str()
                            : m_staff          ? "No map is banned in this cup. Find a level by its ID to ban it."
                                               : "No map is banned in this cup.");
        m_status->setPositionY(listTop - 60.f);
        m_status->limitLabelWidth(380.f, .5f, .2f);
        if (!m_loaded || !m_error.empty()) return;
        auto* rows = menu(m_rows);
        auto const now = Service::get().serverNow();
        for (int row = 0; row < perPage; ++row) {
            int const index = m_page * perPage + row;
            if (index >= count) break;
            auto const& map = m_maps[index];
            float const y = listTop - 16.f - row * 34.f;
            auto* cell = panel(m_rows, {16.f, y - 15.f}, {408.f, 30.f}, false);
            auto* nameLabel = createCosmeticLabel(mapName(map.name, map.levelId).c_str(), "bigFont.fnt");
            nameLabel->setScale(std::min(.34f, (m_staff ? 170.f : 250.f) / std::max(1.f, nameLabel->getContentSize().width)));
            auto* name = CCMenuItemSpriteExtra::create(nameLabel, this, menu_selector(Season2MapBansPopup::onDetail));
            name->setAnchorPoint({0.f, .5f});
            name->setPosition({24.f, y + 6.f});
            name->setTag(index);
            rows->addChild(name);
            label(cell, fmt::format("ID {}", map.levelId), {16.f + nameLabel->getScaledContentSize().width, 21.f},
                  .34f, 80.f, kMuted, true, "chatFont.fnt");
            if (m_staff) {
                reasonText(cell, map.reason, {8.f, 8.f}, 210.f, kIce);
                rightLabel(cell,
                           fmt::format("{}  |  {}", map.byName.empty() ? std::string("Admin") : map.byName,
                                       agoText(now - map.at)),
                           {330.f, 8.f}, .32f, 105.f, kMuted);
                auto* back = button(rows, this, menu_selector(Season2MapBansPopup::onPutBack), "Put Back", {386.f, y},
                                    .36f, "GJ_button_01.png");
                back->setTag(index);
                enabled(back, !m_busy);
            } else {
                label(cell, map.creator.empty() ? std::string("by ?") : "by " + map.creator, {8.f, 8.f}, .34f, 250.f,
                      kIce, true, "chatFont.fnt");
                rightLabel(cell, "Banned " + agoText(now - map.at), {400.f, 15.f}, .36f, 130.f, kMuted);
            }
        }
    }
    // 입력한 ID 의 레벨을 GD 에서 찾는다. 못 찾아도 ID 로 밴할 수 있게 카드를 보여 준다.
    void onFind(CCObject*) {
        if (!m_staff || m_busy || m_looking) return;
        m_id->defocus();
        std::string const text(m_id->getString());
        int64_t id = 0;
        auto const parsed = std::from_chars(text.data(), text.data() + text.size(), id);
        if (text.empty() || parsed.ec != std::errc() || parsed.ptr != text.data() + text.size() || id <= 0 ||
            id > 2'147'483'647) {
            Notification::create("Enter a level ID", NotificationIcon::Warning)->show();
            return;
        }
        m_looking = true;
        m_found.reset();
        m_lookupError.clear();
        render();
        lookupLevel(id, [self = Weak<Season2MapBansPopup>(this), id](std::optional<LevelInfo> level, std::string error) {
            auto owner = self.lock();
            if (!owner) return;
            owner->m_looking = false;
            if (level) {
                owner->m_found = std::move(level);
            } else {
                LevelInfo unknown;
                unknown.id = id;
                owner->m_found = unknown;
                owner->m_lookupError = error.empty() ? std::string("Not found on GD's servers.") : std::move(error);
            }
            if (auto const* ban = owner->banned(id); ban && owner->m_reason->getString().empty())
                owner->m_reason->setString(ban->reason);
            owner->render();
        });
    }
    static void error_(std::string const& text) { error(text); }
    // 서버가 돌려준 새 목록을 반영하고, 컵 카드의 버튼 표시를 갱신하게 한다.
    void changed(std::optional<StaffMapBanList> list, std::string error, char const* done) {
        if (!list) {
            m_busy = false;
            render();
            error_(error);
            return;
        }
        Notification::create(done, NotificationIcon::Success)->show();
        received(std::move(list), {});
        if (m_changed) m_changed();
    }
    // 사유를 확인하고 확인창을 거쳐 밴(이미 밴된 맵이면 사유 변경)한다.
    void onBan(CCObject*) {
        if (!m_staff || m_busy || !m_found) return;
        m_reason->defocus();
        std::string reason(m_reason->getString());
        if (reason.find_first_not_of(' ') == std::string::npos) {
            Notification::create("Write the reason for the ban", NotificationIcon::Warning)->show();
            return;
        }
        auto const level = *m_found;
        bool const change = banned(level.id) != nullptr;
        createQuickPopup(
            change ? "Change the Ban?" : "Ban This Map?",
            change ? fmt::format("Save the new reason for <cy>{}</c>?", plainAlertText(mapName(level.name, level.id)))
                   : fmt::format("<cy>{}</c> (ID {}) will not come up in <cg>{}</c> Ranked matches until a staff "
                                 "member puts it back.",
                                 plainAlertText(mapName(level.name, level.id)), level.id, season2CupName(m_cup)),
            "Cancel", change ? "Save" : "Ban",
            [self = Weak<Season2MapBansPopup>(this), level, reason, change](auto*, bool yes) {
                auto owner = self.lock();
                if (!yes || !owner || owner->m_busy) return;
                owner->m_busy = true;
                owner->render();
                Service::get().banStaffMap(
                    owner->m_cup, level, reason, [self, change](std::optional<StaffMapBanList> list, std::string error) {
                        auto owner = self.lock();
                        if (!owner) return;
                        if (list) {
                            owner->m_found.reset();
                            owner->m_lookupError.clear();
                            owner->m_reason->setString("");
                            owner->m_id->setString("");
                            owner->m_page = 0;
                        }
                        owner->changed(std::move(list), std::move(error), change ? "Ban changed" : "Map banned");
                    });
            });
    }
    // 확인창을 거쳐 맵을 다시 이 컵의 랭크에 넣는다.
    void onPutBack(CCObject* sender) {
        int const index = sender->getTag();
        if (!m_staff || m_busy || index < 0 || index >= static_cast<int>(m_maps.size())) return;
        auto const map = m_maps[index];
        createQuickPopup(
            "Put Back in Ranked?",
            fmt::format("<cy>{}</c> (ID {}) can come up in <cg>{}</c> Ranked matches again.",
                        plainAlertText(mapName(map.name, map.levelId)), map.levelId, season2CupName(m_cup)),
            "Keep Banned", "Put Back", [self = Weak<Season2MapBansPopup>(this), id = map.levelId](auto*, bool yes) {
                auto owner = self.lock();
                if (!yes || !owner || owner->m_busy) return;
                owner->m_busy = true;
                owner->render();
                Service::get().unbanStaffMap(owner->m_cup, id,
                                             [self](std::optional<StaffMapBanList> list, std::string error) {
                                                 if (auto owner = self.lock())
                                                     owner->changed(std::move(list), std::move(error), "Map put back");
                                             });
            });
    }
    // 밴 항목의 자세한 정보(스태프는 전체 사유와 밴한 스태프까지).
    void onDetail(CCObject* sender) {
        int const index = sender->getTag();
        if (index < 0 || index >= static_cast<int>(m_maps.size())) return;
        auto const& map = m_maps[index];
        auto text = fmt::format("ID <cy>{}</c>{}\nBanned {}", map.levelId,
                                map.creator.empty() ? "" : "  |  by " + plainAlertText(map.creator),
                                localDateTime(map.at));
        if (m_staff)
            text += fmt::format(" by <cb>{}</c>\n\n{}",
                                plainAlertText(map.byName.empty() ? std::string("Admin") : map.byName),
                                plainAlertText(map.reason.empty() ? std::string("No reason") : map.reason));
        FLAlertLayer::create(nullptr, mapName(map.name, map.levelId).c_str(), text.c_str(), "OK", nullptr, 380.f)
            ->show();
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
    void onRefresh(CCObject*) {
        if (!m_busy) load();
    }

public:
    static Season2MapBansPopup* create(std::string cup, std::function<void()> changed) {
        auto* result = new Season2MapBansPopup;
        if (result->init(std::move(cup), std::move(changed))) {
            result->autorelease();
            return result;
        }
        delete result;
        return nullptr;
    }
};

class Season2Layer final : public SceneLayer {
    // 컵 카드 하나에 속한 노드 모음 (face: 티어 이미지, status: 레이팅 또는 배치 진행 문구, online: 접속자 수 등).
    struct Card {
        std::string cup, art;
        CCNode* root = nullptr;
        CCNode* face = nullptr;
        CCLabelBMFont* status = nullptr;
        Season2TierText* tier = nullptr;
        CCLabelBMFont* online = nullptr;
        CCDrawNode* dot = nullptr;
        CCMenuItemSpriteExtra* play = nullptr;
        CCMenuItemSpriteExtra* bans = nullptr;  // 1.0.8: 맵 밴 목록 (스태프 항상, 플레이어는 밴된 맵이 있을 때)
        CCLabelBMFont* rank = nullptr;
    };
    std::array<Card, 2> m_cards;
    bool m_loaded = false;
    float m_refresh = 0.f;
    CCLabelBMFont* m_downloads = nullptr;
    CCMenuItemSpriteExtra* m_retry = nullptr;
    std::string m_downloadsText;

    // 화면 구성: 제목, 카드 크기 / 위치 계산, 카드별 장식(창, 왕관, 트로피, 이름), 디버그 전용 RESULT TEST 버튼,
    // 다운로드 상태 줄과 RETRY 버튼. 카드 정보는 refresh()로 채운다.
    bool init() {
        if (!initScene("")) return false;
        addAudioButton(true);
        addLiveButton(m_window.width - 70.f);
        setID("ranked-season2-scene"_spr);
        if (auto* background = typeinfo_cast<MovingBackground*>(getChildByID("versus-default-background"_spr)))
            background->tint(s2::kBase, s2::kTiles, s2::kTileOpacity);
        auto const w = m_window.width, h = m_window.height;
        auto* heading =
            label(this, "RANKED SEASON 2", {w / 2.f, h - 24.f}, .9f, w - 120.f, ccWHITE, false, "gjFont46.fnt");
        heading->setID("ranked-season2-heading"_spr);
        rankedTitleGradient(heading);
        float const cardW = std::min(250.f, (w - 60.f) / 2.f);
        float const crownAbove = season2CrownAbove(season2CupCrown("standard"), kCrownWidth, kCrownSink, kCrownDrop);
        float const space = h - kHeadingRoom;
        float const cardH = std::min(kCardH, space - crownAbove - 2.f * kCardMargin);
        float const gap = std::min(24.f, w - 2.f * cardW - 30.f);
        float const left = (w - 2.f * cardW - gap) / 2.f,
                    bottom = std::max(
                        0.f, std::min((space - cardH - crownAbove) / 2.f + kRaise, space - cardH - crownAbove - 2.f));
        static constexpr char const* cups[] = {"standard", "demon"};
        for (int i = 0; i < 2; ++i) {
            auto& card = m_cards[i];
            card.cup = cups[i];
            card.root = panel(this, {left + i * (cardW + gap), bottom}, {cardW, cardH}, false);
            card.root->setID(fmt::format("season2-cup-{}", card.cup));
            if (auto* back = card.root->getChildByType<NineSlice*>(0)) back->removeFromParent();
            auto* window = NineSlice::create(season2CupWindow(card.cup));
            window->setContentSize({cardW, cardH});
            window->setPosition({cardW / 2.f, cardH / 2.f});
            window->setID("season2-cup-window");
            card.root->addChild(window, -1);
            auto* standing = NineSlice::create("square02b_001.png");
            standing->setColor(ccc3(0, 0, 0));
            standing->setOpacity(kStandingOpacity);
            standing->setContentSize({cardW - 2.f * kStandingInset, kStandingTop - kStandingBottom});
            standing->setPosition({cardW / 2.f, (kStandingTop + kStandingBottom) / 2.f});
            standing->setID("season2-cup-standing");
            card.root->addChild(standing, 1);
            card.face = CCNode::create();
            card.face->setPosition({cardW / 2.f, cardH - kFaceFromTop});
            card.root->addChild(card.face, 3);
            drawFace(card, season2CupArt(card.cup));
            if (auto* crown = loadedFrameSprite(season2CupCrown(card.cup), "GJ_GameSheet04.plist")) {
                crown->setScale(kCrownWidth / std::max(1.f, crown->getContentSize().width));
                crown->setAnchorPoint({.5f, 0.f});
                crown->setPosition(
                    {cardW / 2.f,
                     cardH - crown->getScaledContentSize().height * (kCrownSink + kCrownLower) - kCrownDrop});
                crown->setID("season2-cup-crown"_spr);
                card.root->addChild(crown, 6);
            }
            float const nameY = kNameY, trophyH = 19.f;
            auto* name = label(card.root, season2CupName(card.cup), {cardW / 2.f, nameY}, .62f,
                               cardW - 24.f - 2.f * (trophyH * 1.3f + 6.f), ccWHITE, false, "gjFont46.fnt");
            name->setZOrder(4);
            season2CupColours(name, card.cup);
            for (int side = -1; side <= 1; side += 2) {
                auto* trophy = loadedFrameSprite(season2CupTrophy(card.cup), "GJ_GameSheet03.plist");
                if (!trophy) break;
                trophy->setScale(trophyH / std::max(1.f, trophy->getContentSize().height));
                trophy->setPosition({cardW / 2.f + side * (name->getScaledContentSize().width / 2.f + 6.f +
                                                           trophy->getScaledContentSize().width / 2.f),
                                     nameY + 1.f});
                trophy->setID(fmt::format("season2-cup-trophy-{}", side < 0 ? "left" : "right"));
                card.root->addChild(trophy, 4);
            }
            card.status = label(card.root, "LOADING...", {cardW / 2.f, kStandingY}, .45f, cardW - 24.f, s2::kSoft,
                                false, "bigFont.fnt");
            card.status->setZOrder(4);
            card.tier = Season2TierText::create();
            card.tier->setPosition({cardW / 2.f, kTierY});
            card.root->addChild(card.tier, 4);
            card.rank = label(this, "", {left + i * (cardW + gap) + cardW / 2.f, bottom - kRankBelow}, .34f,
                              cardW - 16.f, kIce, false, "bigFont.fnt");
            card.rank->setID(fmt::format("season2-cup-rank-{}", card.cup));
            card.dot = CCDrawNode::create();
            card.dot->setPosition({14.f, cardH - 14.f});
            card.root->addChild(card.dot, 4);
            card.online = label(card.root, "-- ONLINE", {22.f, cardH - 14.f}, .36f, onlineWidth(cardW), kMuted, true,
                                "bigFont.fnt");
            card.online->setZOrder(4);
            auto* actions = menu(card.root);
            auto* natural = ButtonSprite::create("PLAY", "goldFont.fnt", "GJ_button_01.png");
            auto* wide =
                ButtonSprite::create("PLAY", static_cast<int>(std::lround(natural->getContentSize().width * 1.5f)),
                                     true, "goldFont.fnt", "GJ_button_01.png", natural->getContentSize().height, 1.f);
            wide->setScale(.6f);
            card.play = CCMenuItemSpriteExtra::create(wide, this, menu_selector(Season2Layer::onCup));
            card.play->setPosition({cardW / 2.f, 22.f});
            actions->addChild(card.play);
            card.play->setTag(i);
            card.play->setID(fmt::format("season2-play-{}", card.cup));
            enabled(card.play, false);
            // 1.0.8: PLAY 오른쪽 아래 구석의 돋보기 = 이 컵의 맵 밴 목록.
            card.bans = iconButton(actions, this, menu_selector(Season2Layer::onMapBans), "gj_findBtnOff_001.png",
                                   {cardW - kBansInset, 22.f}, kBansSize);
            card.bans->setTag(i);
            card.bans->setID(fmt::format("season2-map-bans-{}", card.cup));
            card.bans->setVisible(Service::get().isAdmin());
        }
        auto* tools = menu(this);
        auto* testSprite = ButtonSprite::create("RESULT TEST", "goldFont.fnt", "GJ_button_05.png");
        testSprite->setScale(.5f);
        auto* test = CCMenuItemSpriteExtra::create(testSprite, this, menu_selector(Season2Layer::onResultTest));
        test->setPosition({w / 2.f, (bottom + cardH + space) / 2.f});
        test->setID("season2-result-test"_spr);
        test->setVisible(debug::active());
        tools->addChild(test);
        float const downloadsY = std::max(8.f, (bottom - kRankBelow - 6.f) / 2.f);
        m_downloads = label(this, "", {w / 2.f, downloadsY}, .5f, 0.f, kMuted, false, "chatFont.fnt");
        m_downloads->setID("season2-downloads"_spr);
        auto* retrySprite = ButtonSprite::create("RETRY", "goldFont.fnt", "GJ_button_04.png");
        retrySprite->setScale(.42f);
        m_retry = CCMenuItemSpriteExtra::create(retrySprite, this, menu_selector(Season2Layer::onRetryDownloads));
        m_retry->setPosition({w - 40.f, downloadsY});
        m_retry->setID("season2-downloads-retry"_spr);
        tools->addChild(m_retry);
        showDownloads(0.f);
        schedule(schedule_selector(Season2Layer::showDownloads), .25f);
        showOnline(m_cards[0], -1);
        showOnline(m_cards[1], -1);
        refresh();
        schedule(schedule_selector(Season2Layer::tick), 1.f);
        return true;
    }
    // 다운로드 상태 문구 생성기. 맵은 확인 중 / 다운로드 중 / 없음 / 준비됨 상태별로,
    // 오디오는 완료 / 진행 % / 실패 / 없음으로 표시한다.
    static std::string mapText(char const* name, lounge::MapDownload const& map, bool standard) {
        using State = lounge::MapDownload::State;
        switch (map.state) {
        case State::Checking: return fmt::format("{} map: checking...", name);
        case State::Downloading: return fmt::format("{} map: downloading...", name);
        case State::Missing: return fmt::format("{} map: {}", name, standard ? "built-in copy" : "not downloaded");
        case State::Ready: break;
        }
        return fmt::format("{} map: {}", name, map.failed ? "OK (not updated)" : "OK");
    }
    static std::string audioText(char const* name, lounge::AudioDownload const& audio, bool count) {
        if (audio.total == 0) return fmt::format("{}: none", name);
        if (audio.done >= audio.total)
            return count ? fmt::format("{}: {}/{}", name, audio.done, audio.total) : fmt::format("{}: OK", name);
        if (audio.busy)
            return count ? fmt::format("{}: {}/{} ({}%)", name, audio.done, audio.total, audio.percent)
                         : fmt::format("{}: {}%", name, audio.percent);
        return count ? fmt::format("{}: {}/{} (failed)", name, audio.done, audio.total)
                     : fmt::format("{}: failed", name);
    }
    // 0.25초마다 lounge의 다운로드 상태를 읽어 한 줄로 보여 준다.
    // 모두 완료면 초록, 실패해 멈췄으면 빨강 계열, 진행 중이면 노랑. 재시도할 항목이 있을 때만 RETRY를 보인다.
    void showDownloads(float) {
        if (!m_downloads || !m_retry) return;
        auto const state = lounge::downloads();
        bool const mapsBusy = state.standard.state <= lounge::MapDownload::Downloading ||
                              state.demon.state <= lounge::MapDownload::Downloading;
        auto text =
            fmt::format("{}   {}   {}   {}", mapText("Standard", state.standard, true),
                        mapText("Demon", state.demon, false), audioText("Music", state.song, false),
                        mapsBusy && state.sfx.total == 0 ? std::string("SFX: ...") : audioText("SFX", state.sfx, true));
        bool const complete =
            !mapsBusy && !state.retry && state.song.done >= state.song.total && state.sfx.done >= state.sfx.total;
        bool const busy = mapsBusy || state.song.busy || state.sfx.busy;
        if (text != m_downloadsText) {
            m_downloadsText = text;
            m_downloads->setString(text.c_str());
            m_downloads->limitLabelWidth(m_window.width - 2.f * 90.f, .5f, .2f);
        }
        m_downloads->setColor(complete               ? ccc3(150, 255, 170)
                              : state.retry && !busy ? ccc3(255, 150, 120)
                                                     : ccc3(255, 214, 120));
        m_retry->setVisible(state.retry);
        m_retry->setEnabled(state.retry && !m_transitioning);
    }
    // RETRY 버튼: 실패한 다운로드를 다시 시작한다.
    void onRetryDownloads(CCObject*) {
        if (m_transitioning) return;
        lounge::retryDownloads();
        showDownloads(0.f);
    }
    // 카드의 티어 이미지를 art로 다시 그린다.
    void drawFace(Card& card, std::string const& art) {
        card.art = art;
        card.face->removeAllChildren();
        season2Face(card.face, art, {}, 1.45f);
    }
    // 카드 레이아웃 상수 (카드 높이, 여백, 이미지 / 이름 / 순위 위치, 왕관 크기와 잠김 정도 등).
    static constexpr float kCardTaller = 30.f, kCardH = 174.f + kCardTaller;
    static constexpr float kHeadingRoom = 38.f;
    static constexpr float kCardMargin = 8.f;
    static constexpr float kFaceFromTop = 50.f, kNameFromTop = 106.f;
    static constexpr float kStandingBottom = 79.f, kStandingTop = 117.f, kStandingInset = 8.f;
    static constexpr float kStandingY = (kStandingBottom + kStandingTop) / 2.f, kTierY = kStandingY + 8.f,
                           kRatingY = kStandingY - 9.f, kNameY = 56.f;
    static constexpr float kRaise = 8.f, kRankBelow = 8.f;
    static constexpr GLubyte kStandingOpacity = 80;
    static constexpr float kCrownWidth = 62.f;
    static constexpr float kCrownSink = .2f;
    static constexpr float kCrownDrop = 4.f;
    static constexpr float kCrownLower = .03f;
    static constexpr float kBansSize = 28.f, kBansInset = 26.f;  // 1.0.8: 맵 밴 버튼 (카드 오른쪽 아래)
    // 접속자 수 표시 (-1이면 알 수 없음 -> 회색 점, 아니면 초록 점).
    static float onlineWidth(float cardW) { return std::max(30.f, cardW / 2.f - 32.f - 22.f - 4.f); }
    void showOnline(Card& card, int count) {
        card.online->setString(count < 0 ? "-- ONLINE" : fmt::format("{} ONLINE", count).c_str());
        card.online->limitLabelWidth(onlineWidth(card.root->getContentSize().width), .36f, .2f);
        card.online->setColor(count < 0 ? kMuted : ccc3(150, 255, 170));
        card.dot->clear();
        card.dot->drawDot({0.f, 0.f}, 3.2f,
                          count < 0 ? ccColor4F{.45f, .5f, .6f, 1.f} : ccColor4F{.3f, 1.f, .45f, 1.f});
    }
    // 1초마다 호출되어 10초가 지나면 서버 정보를 다시 요청한다.
    void tick(float dt) {
        if (m_transitioning || !m_loaded) return;
        if ((m_refresh += dt) < 10.f) return;
        m_refresh = 0.f;
        refresh();
    }
    // Season 2 홈 정보(컵별 프로필, 접속자 수)를 요청하고, 화면이 아직 살아 있으면 apply로 반영한다.
    void refresh() {
        Service::get().fetchSeason2Home(
            [self = Weak<Season2Layer>(this)](std::vector<Season2Cup> cups, std::string error) {
                auto owner = self.lock();
                if (!owner || owner->m_transitioning) return;
                owner->apply(cups, error);
            });
    }
    // 서버 정보를 카드에 반영한다. 정보가 없으면 UNAVAILABLE / OFFLINE을 보이고 PLAY를 막는다
    // (이미 한 번 불러왔고 오류인 경우엔 이전 표시를 유지). 배치를 마쳤으면 티어 이름과 레이팅,
    // 아니면 배치 진행(n / N)을 보이고, 전체 순위 / 백분위, 접속자 수를 갱신한다.
    void apply(std::vector<Season2Cup> const& cups, std::string const& error) {
        for (auto& card : m_cards) {
            auto found =
                std::find_if(cups.begin(), cups.end(), [&](Season2Cup const& cup) { return cup.id == card.cup; });
            float const width = card.root->getContentSize().width - 24.f;
            if (found == cups.end()) {
                if (m_loaded && !error.empty()) continue;
                card.status->setString(error.empty() ? "UNAVAILABLE" : "OFFLINE");
                card.status->setPositionY(kStandingY);
                card.status->limitLabelWidth(width, .45f, .2f);
                card.tier->setVisible(false);
                showOnline(card, -1);
                enabled(card.play, false);
                continue;
            }
            auto const& p = found->profile;
            std::string const art = p.placed ? p.tier : std::string("Unranked");
            if (card.art != art) drawFace(card, art);
            card.tier->setVisible(p.placed);
            if (p.placed) {
                card.tier->set(p.tierName.empty() ? p.tier : p.tierName, p.tier, .5f, width * .62f, false);
                card.status->setString(fmt::format("R{:.0f}", p.rating).c_str());
                card.status->setPositionY(kRatingY);
                card.status->limitLabelWidth(width * .62f, .42f, .2f);
                rankedGradient(card.status, season2PaletteKey(p.tier));
            } else {
                card.status->setString(
                    fmt::format("PLACEMENT  {} / {}", p.placementCompleted, std::max(1, p.placementRequired)).c_str());
                card.status->setPositionY(kStandingY);
                card.status->setColor(s2::kSoft);
                card.status->limitLabelWidth(width, .45f, .2f);
            }
            if (card.rank) {
                card.rank->setString((p.globalRank > 0 ? fmt::format("#{} / {}  |  Top {:.1f}%", p.globalRank,
                                                                     p.population, p.percentile)
                                                       : fmt::format("#---- / {}", std::max(0, p.population)))
                                         .c_str());
                card.rank->limitLabelWidth(width, .34f, .16f);
            }
            showOnline(card, found->online);
            enabled(card.play, true);
            card.bans->setVisible(Service::get().isAdmin() || found->mapBans > 0);
        }
        m_loaded = true;
    }
    // 1.0.8: 컵 카드의 돋보기 - 그 컵의 맵 밴 목록. 스태프가 바꾸면 카드 정보를 다시 받아 버튼 표시를 맞춘다.
    void onMapBans(CCObject* sender) {
        int const index = sender->getTag();
        if (m_transitioning || m_resultTest || index < 0 || index > 1) return;
        if (auto* popup = Season2MapBansPopup::create(m_cards[index].cup, [self = Weak<Season2Layer>(this)] {
                if (auto owner = self.lock(); owner && !owner->m_transitioning) owner->refresh();
            }))
            popup->show();
    }
    // 디버그 전용: 가짜 결과 화면(Result Test)을 띄운다. 결과 화면이 떠 있는 동안에는 컵 선택과 뒤로 가기를 막는다.
    bool m_resultTest = false;
    void onResultTest(CCObject*) {
        if (m_transitioning || m_resultTest || !debug::active()) return;
        m_resultTest = true;
        Loader::get()->queueInMainThread([self = Weak<Season2Layer>(this)] {
            auto owner = self.lock();
            if (!owner) return;
            if (!owner->isRunning() || owner->m_transitioning) {
                owner->m_resultTest = false;
                return;
            }
            s2result::show(
                owner.data(), s2result::randomBotModel(),
                [self] {
                    if (auto layer = self.lock()) layer->m_resultTest = false;
                },
                [] { return s2result::randomBotModel(); });
        });
    }
    // PLAY: 선택한 컵을 현재 랭크 컵으로 지정하고 Ranked 화면으로 넘어간다.
    void onCup(CCObject* sender) {
        int const index = sender->getTag();
        if (m_transitioning || m_resultTest || !m_loaded || index < 0 || index > 1) return;
        m_transitioning = true;
        Service::get().setRankedCup(m_cards[index].cup);
        showRanked();
    }
    // 뒤로 가기: 컵 선택을 해제하고 랭크 음악을 멈춘다. 에디터 Plaza Test로 들어온 경우엔 원래 레벨 페이지로, 아니면 Versus 메인으로 돌아간다.
    void goBack() override {
        if (m_transitioning || m_resultTest) return;
        m_transitioning = true;
        Service::get().setRankedCup("");
        audio::stopRankedMusic(true);
        if (editorplaza::backToLevel()) return;
        showVersus();
    }

public:
    static Season2Layer* create() {
        auto* result = new Season2Layer;
        if (result->init()) {
            result->autorelease();
            return result;
        }
        delete result;
        return nullptr;
    }
    // 화면에 들어올 때 랭크 음악을 멈추고 컵 노래를 초기화하며 lounge 다운로드를 이어서 진행한다.
    void onEnter() override {
        SceneLayer::onEnter();
        audio::stopRankedMusic(true);
        audio::setCupSong(0);
        lounge::resumeDownloads();
    }
};
