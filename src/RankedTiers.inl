// Ranked 팝업 두 개: 난이도별 생명 수 표(RankedLivesPopup)와 티어 목록(RankedTiersPopup).
// 티어 컷오프는 서버에서 받아 와 표시한다.
//
// 난이도(2별 ~ Extreme Demon)와 맵 길이 열(TINY-MEDIUM / LONG / XL / XL 3분 이상)에 따른
// 생명 수를 ranked::livesFor로 계산해 보여 주는 팝업. 데몬 행은 위쪽 줄과 구분해 그린다.
class RankedLivesPopup final : public ::versus::DeferredPopup {
    static constexpr float W = 440.f, H = 300.f, RowH = 15.5f, GroupGap = 7.f;
    static constexpr float ColName = 54.f, ColValue[4] = {204.f, 270.f, 332.f, 394.f}, PillW = 50.f;
    // 행별 난이도 얼굴 아이콘 번호와 난이도 이름(ranked::MASK_LIVES와 같은 순서).
    static constexpr int Faces[13] = {1, 2, 3, 3, 4, 4, 5, 5, 7, 8, 6, 9, 10};
    static constexpr char const* Names[13] = {
        "Easy",   "Normal",     "Hard",         "Hard",       "Harder",       "Harder",       "Insane",
        "Insane", "Easy Demon", "Medium Demon", "Hard Demon", "Insane Demon", "Extreme Demon"};
    static ccColor3B columnColor(int column) {
        constexpr ccColor3B colors[4] = {{120, 220, 140}, {255, 214, 90}, {255, 150, 80}, {255, 96, 96}};
        return colors[column];
    }
    static ccColor3B nameColor(size_t row) {
        constexpr ccColor3B colors[13] = {{110, 200, 255}, {110, 230, 120}, {255, 220, 90},  {255, 220, 90},
                                          {255, 160, 70},  {255, 160, 70},  {215, 130, 255}, {215, 130, 255},
                                          {255, 140, 120}, {255, 120, 120}, {255, 100, 100}, {255, 90, 140},
                                          {255, 80, 80}};
        return colors[row];
    }
    // 난이도 얼굴 아이콘을 그린다.
    void face(CCNode* parent, int id, CCPoint at) {
        auto name = fmt::format("diffIcon_{:02}_btn_001.png", id);
        auto* sprite = loadedFrameSprite(name.c_str(), "GJ_GameSheet03.plist");
        if (!sprite) return;
        sprite->setScale(14.f / std::max(1.f, sprite->getContentSize().height));
        sprite->setPosition(at);
        parent->addChild(sprite, 3);
    }
    // 생명 수 값을 열 색으로 칠한 알약 모양으로 그린다.
    void pill(CCNode* parent, int value, int column, CCPoint at) {
        auto* back = NineSlice::create("square02b_001.png");
        back->setContentSize({PillW, RowH - 3.f});
        back->setPosition(at);
        auto const tint = columnColor(column);
        back->setColor(tint);
        back->setOpacity(46);
        parent->addChild(back, 2);
        label(parent, std::to_string(value), at, .34f, PillW - 6.f, tint, false, "bigFont.fnt")->setZOrder(3);
    }
    // 헤더(길이 열과 감소율), 난이도 행, 데몬 구분선을 배치한다.
    bool init() override {
        if (!Popup::init(W, H, "GJ_square02.png")) return false;
        setTitle("RANKED LIVES", "bigFont.fnt", .6f);
        setID("versus-ranked-lives"_spr);
        float const headY = H - 48.f;
        auto* heart = createLifeHeart(12.f);
        heart->setPosition({ColName - 18.f, headY});
        m_mainLayer->addChild(heart, 3);
        label(m_mainLayer, "DIFFICULTY", {ColName - 8.f, headY}, .3f, 120.f, kIce, true, "bigFont.fnt");
        char const* heads[4] = {"TINY-MEDIUM", "LONG", "XL", "XL 3 MIN+"};
        char const* cuts[4] = {"full", "-30%", "-50%", "-70%"};
        for (int c = 0; c < 4; ++c) {
            auto* chip = NineSlice::create("square02b_001.png");
            chip->setContentSize({PillW + 12.f, 24.f});
            chip->setPosition({ColValue[c], headY - 1.f});
            chip->setColor(columnColor(c));
            chip->setOpacity(70);
            m_mainLayer->addChild(chip, 1);
            label(m_mainLayer, heads[c], {ColValue[c], headY + 4.f}, .24f, PillW + 6.f, ccWHITE, false, "bigFont.fnt")
                ->setZOrder(2);
            label(m_mainLayer, cuts[c], {ColValue[c], headY - 6.f}, .36f, PillW, columnColor(c), false, "chatFont.fnt")
                ->setZOrder(2);
        }
        auto const count = ranked::MASK_LIVES.size();
        float const top = headY - 17.f;
        float const bottom = top - RowH * count - GroupGap;
        auto* paper = NineSlice::create("square02b_001.png");
        paper->setContentSize({W - 24.f, top - bottom + 8.f});
        paper->setPosition({W / 2.f, (top + bottom) / 2.f});
        paper->setColor({10, 18, 38});
        paper->setOpacity(215);
        m_mainLayer->addChild(paper);
        for (size_t i = 0; i < count; ++i) {
            bool const demon = i >= 8;
            float const y = top - RowH * (i + .5f) - (demon ? GroupGap : 0.f);
            auto* stripe = CCLayerColor::create(
                demon ? ccc4(255, 60, 60, i % 2 ? 18 : 30) : ccc4(255, 255, 255, i % 2 ? 6 : 16), W - 32.f, RowH - 1.f);
            stripe->setPosition({16.f, y - (RowH - 1.f) / 2.f});
            m_mainLayer->addChild(stripe, 1);
            face(m_mainLayer, Faces[i], {ColName - 18.f, y});
            auto* name =
                label(m_mainLayer, Names[i], {ColName - 8.f, y}, .5f, 100.f, nameColor(i), true, "chatFont.fnt");
            name->setZOrder(3);
            if (!demon)
                starRating(m_mainLayer, std::to_string(i + 2),
                           {ColName - 8.f + name->getScaledContentSize().width + 14.f, y}, .3f)
                    ->setZOrder(3);
            int const base = ranked::MASK_LIVES[i];
            int const values[] = {ranked::livesFor(base, 2), ranked::livesFor(base, 3), ranked::livesFor(base, 4),
                                  ranked::livesFor(base, 4, true)};
            for (int c = 0; c < 4; ++c) pill(m_mainLayer, values[c], c, {ColValue[c], y});
        }
        auto* rule = CCLayerColor::create(ccc4(255, 120, 120, 90), W - 40.f, 1.f);
        rule->setPosition({20.f, top - RowH * 8.f - GroupGap / 2.f});
        m_mainLayer->addChild(rule, 2);
        return true;
    }

public:
    CREATE_FUNC(RankedLivesPopup);
};

// 티어 목록 팝업. 각 티어의 얼굴/오라/이름/순위 구간과 현재 컷오프 레이팅을 보여 주고,
// 내 티어 행은 강조하며 그 위치로 스크롤한다.
class RankedTiersPopup final : public ::versus::DeferredPopup {
    std::optional<RankedProfile> m_self;
    std::vector<CCLabelBMFont*> m_cutoffs;
    CCLabelBMFont* m_mine = nullptr;
    bool init(std::optional<RankedProfile> self) {
        if (!Popup::init(420.f, 300.f, "GJ_square02.png")) return false;
        m_self = std::move(self);
        setTitle("RANK TIERS", "bigFont.fnt", .6f);

        label(m_mainLayer, "TIER", {150.f, 254.f}, .3f, 128.f, kIce);
        label(m_mainLayer, "RANK BAND", {290.f, 254.f}, .3f, 110.f, kIce);
        label(m_mainLayer, "CUTOFF", {370.f, 254.f}, .3f, 64.f, kIce);

        // 티어 행 목록. 컷오프 라벨은 서버 응답 전까지 "..."로 둔다.
        constexpr float rowH = 44.f;
        auto* paper = NineSlice::create("square02b_001.png");
        paper->setContentSize({396.f, 190.f});
        paper->setPosition({210.f, 150.f});
        paper->setColor({12, 22, 44});
        paper->setOpacity(200);
        m_mainLayer->addChild(paper);
        auto* scroll = versus::compat::scrollLayer({396.f, 186.f});
        scroll->setPosition({12.f, 57.f});
        m_mainLayer->addChild(scroll);
        auto const count = ranked::TIER_DISPLAY.size();
        float const height = std::max(186.f, count * rowH + 4.f);
        scroll->m_contentLayer->setContentSize({396.f, height});
        bool const hasTier = m_self && m_self->placed && !m_self->banned;
        float myRowY = -1.f;
        for (size_t i = 0; i < count; ++i) {
            auto const& tier = ranked::TIER_DISPLAY[i];
            float const y = height - 2.f - rowH * (i + 1);
            auto* row = panel(scroll->m_contentLayer, {4.f, y + 2.f}, {388.f, rowH - 4.f}, false);
            row->setID(fmt::format("rank-tier-row-{}", i));
            bool const mine = hasTier && m_self->tier == tier.name;
            if (mine) {
                if (auto* back = row->getChildByType<NineSlice*>(0)) {
                    back->setColor(ccc3(90, 70, 18));
                    back->setOpacity(235);
                }
                myRowY = y;
            }
            float const mid = (rowH - 4.f) / 2.f;
            rankedGlow(row, std::string(tier.name), {34.f, mid}, {50.f, 26.f});
            rankedAura(row, std::string(tier.name), {34.f, mid});
            rankedFace(row, std::string(tier.name), {34.f, mid}, .6f);
            auto* name = label(row, std::string(tier.name), {140.f, mid}, .42f, 160.f);
            rankedGradient(name, std::string(tier.name));
            if (mine) label(row, "YOU", {140.f, mid - 13.f}, .42f, 40.f, ccc3(255, 214, 60), false, "chatFont.fnt");
            label(row, std::string(tier.rankBand), {280.f, mid}, .62f, 100.f, ccWHITE, false, "chatFont.fnt");
            m_cutoffs.push_back(label(row, "...", {362.f, mid}, .6f, 56.f, ccc3(255, 210, 96), false, "chatFont.fnt"));
        }
        scroll->scrollToTop();
        // 내 티어 행이 보이도록 스크롤 위치를 맞춘다.
        if (myRowY >= 0.f)
            scroll->m_contentLayer->setPositionY(std::clamp(186.f - (myRowY + rowH * 1.5f), 186.f - height, 0.f));

        m_mine = label(m_mainLayer,
                       hasTier ? fmt::format("YOU: {}  R{:.0f}", m_self->tier, m_self->rating)
                               : std::string("Finish your placement matches to get a tier."),
                       {210.f, 40.f}, .5f, 395.f, ccc3(255, 214, 60), false, "chatFont.fnt");
        label(m_mainLayer, "Rating decides leaderboard order. Tier cutoffs move with the live ladder.", {210.f, 22.f},
              .45f, 395.f, kIce, false, "chatFont.fnt");
        // 실시간 컷오프 요청. 팝업이 이미 닫혔으면 무시한다.
        Service::get().fetchRankedTiers(
            [self = Weak<RankedTiersPopup>(this)](std::optional<RankedTiers> tiers, std::string) {
                if (auto owner = self.lock()) owner->applyTiers(tiers);
            });
        return true;
    }
    // 받은 컷오프를 각 행에 표시한다(최하위 티어는 ANY, 값이 없으면 "-"). 배치를 마친 유저에게는
    // 다음 티어까지 남은 점수를 하단에 보여 준다.
    void applyTiers(std::optional<RankedTiers> const& tiers) {
        for (size_t i = 0; i < m_cutoffs.size(); ++i) {
            std::string text = "LIVE";
            if (tiers) {
                auto const name = std::string(ranked::TIER_DISPLAY[i].name);
                auto const row = std::find_if(tiers->tiers.begin(), tiers->tiers.end(),
                                              [&](RankedTierCutoff const& value) { return value.tier == name; });
                text = i + 1 == m_cutoffs.size()                      ? "ANY"
                       : row == tiers->tiers.end() || !row->minRating ? "-"
                                                                      : fmt::format("R{}+", *row->minRating + 1);
            }
            m_cutoffs[i]->setString(text.c_str());
            m_cutoffs[i]->limitLabelWidth(56.f, .6f, .3f);
        }
        if (!tiers || !m_self || !m_self->placed || m_self->banned) return;
        auto const next = nextRankedTier(*tiers, m_self->tier, m_self->rating);
        auto const text = next ? fmt::format("YOU: {}  R{:.0f}   |   {} more to {} (R{}+)", m_self->tier,
                                             m_self->rating, next->points, next->tier, next->cutoff + 1)
                               : fmt::format("YOU: {}  R{:.0f}   |   The top tier", m_self->tier, m_self->rating);
        m_mine->setString(text.c_str());
        m_mine->limitLabelWidth(395.f, .5f, .25f);
    }

public:
    static RankedTiersPopup* create(std::optional<RankedProfile> self = std::nullopt) {
        auto* result = new RankedTiersPopup;
        if (result->init(std::move(self))) {
            result->autorelease();
            return result;
        }
        delete result;
        return nullptr;
    }
};
