// Live 매치 목록 팝업(Ranked / Public / Group 진행 중 전투)과 그 보조 노드.
// 3초마다 서버의 라이브 목록을 다시 받아 행을 그리고, 행의 눈 아이콘으로 실시간 관전을,
// 맵 아이콘/재생 버튼으로 해당 맵 열기·플레이를 연결한다. 다른 .cpp에서 include 되어 쓰인다.
//
// LevelInfoLayer가 열린 뒤 전환이 끝나면 자동으로 Play를 눌러 주는 보조 노드.
// 0.35초 이후 페이지가 준비되면 onPlay를 한 번 호출하고, 12초가 지나도록 준비되지
// 않으면 포기한다.
class LiveAutoPlay final : public CCNode {
    Weak<LevelInfoLayer> m_page;
    float m_waited = 0.f;

public:
    static LiveAutoPlay* create(LevelInfoLayer* page) {
        auto* node = new LiveAutoPlay();
        node->init();
        node->autorelease();
        node->m_page = page;
        node->scheduleUpdate();
        return node;
    }
    void update(float dt) override {
        m_waited += dt;
        auto page = m_page.lock();
        if (!page || m_waited > 12.f) {
            unscheduleUpdate();
            return;
        }
        if (m_waited < .35f || !page->m_enterTransitionFinished || page->m_isBusy || !page->m_level ||
            page->m_level->m_levelString.empty())
            return;
        unscheduleUpdate();
        page->onPlay(nullptr);
    }
};

// 진행 중인 전투 목록 팝업. 목록은 "All / Ranked / Public / Group" 탭과 플레이어 이름 검색,
// 평균 랭크 정렬을 지원한다. 행 안의 각 터치 영역은 보이지 않는 CCNode의 tag로 구분하며
// 아래 tag 상수가 종류별 시작값(+행 인덱스)이다.
class LiveMatchesPopup final : public ::versus::DeferredPopup {
    static constexpr float kWidth = 440.f, kRowHeight = 66.f, kListHeight = 196.f;
    static constexpr int kMapTag = 100000, kPlayTag = 200000;
    static constexpr int kWatchTag = 300000, kGroupMapTag = 400000, kGroupPlayTag = 500000;
    static constexpr int kEyeTag = 600000, kGroupEyeTag = 700000;
    static constexpr int kPlayerEyeTag = 800000, kLeaderEyeTag = 900000;
    static constexpr float kEyeSize = 22.f, kPlayerEyeSize = 17.f;
    // 정렬 모드: 0 기본(서버 순서), 1 랭크 높은 순, 2 랭크 낮은 순. 설정에 저장된다.
    int m_sort = 0;
    CCMenuItemSpriteExtra* m_sortButton = nullptr;
    static constexpr std::array<char const*, 3> kSortNames = {"Sort: Default", "Sort: Rank High", "Sort: Rank Low"};
    // Group 관전용 조회가 진행 중인 동안 중복 클릭을 막는 플래그.
    bool m_eyeLoading = false;
    static constexpr int kTabs = 4;
    // 플레이를 위해 맵을 받는 중인 레벨 id (0이면 없음).
    int64_t m_preparing = 0;
    CCLabelBMFont* m_playStatus = nullptr;
    TapScrollLayer* m_scroll = nullptr;
    CCLabelBMFont* m_status = nullptr;
    CCLabelBMFont* m_count = nullptr;
    std::vector<LiveMatch> m_matches;
    std::vector<LiveGroup> m_groups;
    float m_refresh = 0.f;
    int m_filter = 0;
    std::array<CCMenuItemSpriteExtra*, kTabs> m_tabs{};
    bool m_loading = false;
    TextInput* m_search = nullptr;
    std::string m_query;
    // 닫힐 때 검색창 포커스를 해제한다.
    void onExit() override {
        if (m_search) m_search->defocus();
        Popup::onExit();
    }

    // 검색 비교용 소문자 변환.
    static std::string folded(std::string text) {
        std::transform(text.begin(), text.end(), text.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        return text;
    }
    // 검색어가 플레이어 이름(1v1은 두 명, Group은 참가자 이름 목록)에 포함되는지.
    bool matchesQuery(LiveMatch const& match) const {
        if (m_query.empty()) return true;
        for (auto const& player : match.players)
            if (folded(player.profile.name).find(m_query) != std::string::npos) return true;
        return false;
    }
    bool matchesQuery(LiveGroup const& group) const {
        if (m_query.empty()) return true;
        for (auto const& name : group.names)
            if (folded(name).find(m_query) != std::string::npos) return true;
        return false;
    }

    // ms를 mm:ss 문자열로 바꾼다 (음수는 0).
    static std::string clock(int64_t ms) {
        auto seconds = std::max<int64_t>(0, ms) / 1000;
        return fmt::format("{:02}:{:02}", seconds / 60, seconds % 60);
    }
    // 규칙 요약 문구: PRACTICE / TARGET n% / n LIVES, 속도가 100%가 아니면 뒤에 덧붙인다.
    static std::string ruleText(GameRules const& rules) {
        std::string const speed = rules.speed != 100 ? "  " + speedText(rules.speed) : std::string();
        if (rules.practice) return "PRACTICE" + speed;
        if (rules.mode == 1) return fmt::format("TARGET {}%", rules.targetPercent) + speed;
        return fmt::format("{} LIVES", rules.attempts) + speed;
    }
    // 플레이어 한 줄(아이콘, 랭크 아이콘/닉네임, 현재 %, 최고 %, 시도 횟수, 상태 태그)을 그린다.
    // 상태 태그는 CLEAR / FORFEIT / PAUSED / WAITING(관전) / RESPAWN 중 해당하는 것.
    void playerLine(CCNode* cell, LivePlayer const& player, GameRules const& rules, bool ranked, float y) {
        auto const& state = player.state;
        auto* manager = GameManager::sharedState();
        int const count = manager ? std::max(1, manager->countForType(IconType::Cube)) : 1;
        auto* icon = SimplePlayer::create(std::clamp(player.profile.icon, 1, count));
        applyIconLook(icon, player.profile.color1, player.profile.color2, player.profile.glow);
        icon->setScale(.42f);
        icon->setPosition({16.f, y});
        cell->addChild(icon);
        if (ranked && !player.tier.empty()) rankedFace(cell, player.tier, {36.f, y}, .3f);
        auto* name = rankedNickname(cell, player.profile, ranked ? player.globalRank : 0, {ranked ? 98.f : 88.f, y},
                                    .26f, 110.f);
        (void)name;
        bool const running = state.inAttempt && !state.cleared && !state.forfeited;
        auto* current = label(cell, fmt::format("{}%", std::clamp(state.currentPercent, 0, 100)), {190.f, y}, .4f, 52.f,
                              running ? ccWHITE : ccc3(150, 160, 180), false, "bigFont.fnt");
        (void)current;
        label(cell, fmt::format("BEST {}%", std::clamp(state.bestPercent, 0, 100)), {252.f, y}, .27f, 66.f,
              ccc3(130, 205, 245), false, "bigFont.fnt");
        int const used = state.attemptsUsed + (running ? 1 : 0);
        std::string attempts;
        if (rules.mode == 0 && !rules.practice)
            attempts = fmt::format("ATT {}/{}", std::min(used, rules.attempts), rules.attempts);
        else
            attempts = fmt::format("ATT {}", std::max(1, used));
        bool const lastLife = rules.mode == 0 && !rules.practice && rules.attempts - state.attemptsUsed <= 1;
        label(cell, attempts, {316.f, y}, .27f, 58.f, lastLife ? ccc3(255, 160, 175) : ccc3(225, 230, 240), false,
              "bigFont.fnt");
        std::string tag;
        ccColor3B tint = ccc3(255, 214, 120);
        if (state.cleared) {
            tag = "CLEAR";
            tint = ccc3(138, 255, 193);
        } else if (state.forfeited) {
            tag = "FORFEIT";
            tint = ccc3(255, 146, 162);
        } else if (state.paused)
            tag = "PAUSED";
        else if (!state.inAttempt)
            tag = state.spectating ? "WAITING" : "RESPAWN";
        if (!tag.empty()) label(cell, tag, {368.f, y}, .22f, 42.f, tint, false, "bigFont.fnt");
    }
    // 맵 이름 영역을 눌렀을 때: 해당 레벨의 정보 화면(LevelInfoLayer)을 연다.
    void openMap(int index) {
        if (index < 0 || index >= static_cast<int>(m_matches.size())) return;
        openLevel(m_matches[index].level);
    }
    void openLevel(LevelInfo const& info) {
        if (info.id <= 0 || info.id > std::numeric_limits<int>::max()) return;
        auto* level = GJGameLevel::create();
        level->m_levelID = static_cast<int>(info.id);
        level->m_levelName = info.name;
        level->m_levelType = GJLevelType::Saved;
        lounge::release();
        if (auto* scene = LevelInfoLayer::scene(level, false))
            CCDirector::sharedDirector()->pushScene(CCTransitionFade::create(.2f, scene));
    }
    void playMap(int index) {
        if (index < 0 || index >= static_cast<int>(m_matches.size())) return;
        playLevel(m_matches[index].level);
    }
    // Play 아이콘: 맵을 내려받아(캐시) LevelInfoLayer를 연 뒤 LiveAutoPlay로 자동 재생한다.
    // 플랫포머 맵이거나, 방/Ranked 대기열에 있으면 거부한다.
    void playLevel(LevelInfo const info) {
        if (m_preparing || info.id <= 0 || info.id > std::numeric_limits<int>::max()) return;
        if (info.platformer) {
            error(kPlatformerMapMessage);
            return;
        }
        auto const* ranked = Service::get().rankedState();
        if (Service::get().room() || (ranked && ranked->queuedAt > 0)) {
            error("Leave your room or the Ranked queue before playing a map from Live.");
            return;
        }
        m_preparing = info.id;
        m_playStatus->setVisible(true);
        ensureMapCached(
            info.id,
            [self = Weak<LiveMatchesPopup>(this), id = info.id](bool ok, std::string detail) {
                auto owner = self.lock();
                if (!owner || owner->m_preparing != id) return;
                owner->m_preparing = 0;
                owner->m_playStatus->setVisible(false);
                auto* level = ok ? cachedLevel(id) : nullptr;
                if (!level) {
                    error(detail.empty() ? std::string("This map could not be downloaded.") : detail);
                    return;
                }
                lounge::release();
                auto* scene = LevelInfoLayer::scene(level, false);
                auto* page = scene ? scene->getChildByType<LevelInfoLayer*>(0) : nullptr;
                if (!scene || !page) {
                    error("This map could not be opened.");
                    return;
                }
                page->addChild(LiveAutoPlay::create(page));
                CCDirector::sharedDirector()->pushScene(CCTransitionFade::create(.2f, scene));
            },
            AudioNeed::All, false, true);
    }
    // Group 행을 눌렀을 때: 순위표를 보여 주는 관전 팝업을 연다.
    void watchGroup(int index) {
        if (index < 0 || index >= static_cast<int>(m_groups.size())) return;
        auto const& group = m_groups[static_cast<size_t>(index)];
        if (auto* popup = GroupSpectatePopup::watch(group.roomId, group.name)) popup->show();
    }
    // 눈 아이콘 관전. 1v1은 이미 가진 정보로 바로, Group은 watchGroup으로 최신 전투를 받은 뒤
    // 처음으로 볼 수 있는 플레이어를 골라 실시간 관전 화면을 연다.
    void watchLive(int index, bool group) {
        std::string why, name;
        if (!group) {
            if (index < 0 || index >= static_cast<int>(m_matches.size())) return;
            auto const& match = m_matches[static_cast<size_t>(index)];
            auto const uid = freeplay::firstToWatch(freeplay::battleOf(match), name, why);
            if ((uid.empty() || !freeplay::openLive(match, uid, name, why)) && !why.empty()) error(why);
            return;
        }
        if (m_eyeLoading || index < 0 || index >= static_cast<int>(m_groups.size())) return;
        auto const& entry = m_groups[static_cast<size_t>(index)];
        m_eyeLoading = true;
        Service::get().watchGroup(
            entry.roomId, GroupBattle{}, {},
            [self = Weak<LiveMatchesPopup>(this), room = entry.roomId, roomName = entry.name](GroupWatch watch) {
                auto owner = self.lock();
                if (!owner) return;
                owner->m_eyeLoading = false;
                if (!watch.ok) {
                    error("This battle could not be loaded - try again.");
                    return;
                }
                if (watch.closed || watch.battle.finishedAt > 0) {
                    error("This battle is no longer live.");
                    return;
                }
                std::string why, name;
                auto const uid = freeplay::firstToWatch(watch.battle, name, why);
                if ((uid.empty() ||
                     !freeplay::openLive(watch.battle, room, watch.roomName.empty() ? roomName : watch.roomName, uid,
                                         name, why)) &&
                    !why.empty())
                    error(why);
            });
    }
    // 1v1 행에서 특정 플레이어의 눈 아이콘: 관전 가능 여부를 확인하고 연다.
    void watchPlayer(int index, int side) {
        std::string why;
        if (index < 0 || index >= static_cast<int>(m_matches.size()) || side < 0 || side > 1) return;
        auto const& match = m_matches[static_cast<size_t>(index)];
        auto const& player = match.players[static_cast<size_t>(side)].profile;
        if (!freeplay::canWatch(freeplay::battleOf(match), player.uid, why) ||
            !freeplay::openLive(match, player.uid, player.name.empty() ? std::string("Player") : player.name, why)) {
            if (!why.empty()) error(why);
        }
    }
    // Group 행에서 상위 플레이어(place)의 눈 아이콘: 최신 전투를 받아 그 플레이어를 관전한다.
    void watchLeader(int index, int place) {
        if (m_eyeLoading || index < 0 || index >= static_cast<int>(m_groups.size())) return;
        auto const& entry = m_groups[static_cast<size_t>(index)];
        if (place < 0 || place >= static_cast<int>(entry.top.size())) return;
        auto const uid = entry.top[static_cast<size_t>(place)].profile.uid;
        auto const name = entry.top[static_cast<size_t>(place)].profile.name;
        m_eyeLoading = true;
        Service::get().watchGroup(entry.roomId, GroupBattle{}, {},
                                  [self = Weak<LiveMatchesPopup>(this), room = entry.roomId, roomName = entry.name, uid,
                                   name](GroupWatch watch) {
                                      auto owner = self.lock();
                                      if (!owner) return;
                                      owner->m_eyeLoading = false;
                                      if (!watch.ok) {
                                          error("This battle could not be loaded - try again.");
                                          return;
                                      }
                                      if (watch.closed || watch.battle.finishedAt > 0) {
                                          error("This battle is no longer live.");
                                          return;
                                      }
                                      std::string why;
                                      if (!freeplay::canWatch(watch.battle, uid, why) ||
                                          !freeplay::openLive(watch.battle, room,
                                                              watch.roomName.empty() ? roomName : watch.roomName, uid,
                                                              name.empty() ? std::string("Player") : name, why)) {
                                          if (!why.empty()) error(why);
                                      }
                                  });
    }
    // 행 오른쪽에 눈 아이콘을 그리고, 스크롤 영역 위에 같은 위치의 터치 노드(tag)를 만든다.
    // live가 false면 회색(지금은 관전 불가)으로 표시한다.
    void addEye(CCNode* holder, float width, float bottom, int tag, bool live, float atY = kRowHeight - 12.f,
                float size = kEyeSize) {
        auto* eye = CircleButtonSprite::create(spectateEye(16.f), live ? CircleBaseColor::Green : CircleBaseColor::Gray,
                                               CircleBaseSize::Tiny);
        eye->setScale(size / std::max(1.f, eye->getContentSize().height));
        float const x = width - 4.f - size / 2.f;
        eye->setPosition({x, atY});
        holder->addChild(eye);
        auto* tap = CCNode::create();
        tap->setContentSize({size + 8.f, std::min(24.f, size + 4.f)});
        tap->setAnchorPoint({.5f, .5f});
        tap->setPosition({x, bottom + atY});
        tap->setTag(tag);
        m_scroll->m_contentLayer->addChild(tap);
    }
    // 해당 플레이어를 지금 관전할 수 있는지.
    static bool watchable(GroupBattle const& battle, std::string const& uid) {
        std::string why;
        return freeplay::canWatch(battle, uid, why);
    }
    // Group 행에 표시된 상위 플레이어들만으로 임시 GroupBattle을 만들어 관전 가능 여부 판단에 쓴다.
    static GroupBattle leadersBattle(LiveGroup const& group) {
        GroupBattle battle;
        battle.releasedAt = group.releasedAt;
        battle.rules = group.rules;
        for (auto const& player : group.top) {
            GroupSeat seat;
            seat.profile = player.profile;
            seat.state = player.state;
            battle.seats.push_back(std::move(seat));
        }
        return battle;
    }
    // 평균 랭크로 행 인덱스를 정렬한다. 랭크 정보가 없는(0) 행은 항상 뒤로 보낸다.
    void sortByRank(std::vector<size_t>& rows, bool group) const {
        if (m_sort == 0) return;
        auto rank = [this, group](size_t i) { return group ? m_groups[i].avgRank : m_matches[i].avgRank; };
        std::stable_sort(rows.begin(), rows.end(), [&](size_t a, size_t b) {
            int const x = rank(a), y = rank(b);
            if ((x > 0) != (y > 0)) return x > 0;
            return m_sort == 1 ? x < y : x > y;
        });
    }
    std::string rankNote(int avgRank) const {
        return m_sort != 0 && avgRank > 0 ? fmt::format("  |  AVG #{}", avgRank) : std::string();
    }
    void paintSort() {
        if (!m_sortButton) return;
        auto* sprite = ButtonSprite::create(kSortNames[static_cast<size_t>(std::clamp(m_sort, 0, 2))], "goldFont.fnt",
                                            m_sort == 0 ? "GJ_button_04.png" : "GJ_button_02.png");
        sprite->setScale(.36f);
        m_sortButton->setSprite(sprite);
    }
    void onSort(CCObject*) {
        m_sort = (std::clamp(m_sort, 0, 2) + 1) % 3;
        if (auto* mod = Mod::get()) mod->setSavedValue<int>("live-sort", m_sort);
        paintSort();
        fill();
        if (m_scroll) m_scroll->scrollToTop();
    }
    // 터치 노드의 tag를 종류별 시작값과 비교해 알맞은 동작으로 연결한다.
    // (가장 큰 시작값부터 확인하며, 어느 것에도 해당하지 않으면 플레이어 프로필 팝업이다.)
    void openPlayer(int tag) {
        if (tag >= kLeaderEyeTag) {
            watchLeader((tag - kLeaderEyeTag) / 2, (tag - kLeaderEyeTag) % 2);
            return;
        }
        if (tag >= kPlayerEyeTag) {
            watchPlayer((tag - kPlayerEyeTag) / 2, (tag - kPlayerEyeTag) % 2);
            return;
        }
        if (tag >= kGroupEyeTag) {
            watchLive(tag - kGroupEyeTag, true);
            return;
        }
        if (tag >= kEyeTag) {
            watchLive(tag - kEyeTag, false);
            return;
        }
        if (tag >= kGroupPlayTag) {
            if (tag - kGroupPlayTag < static_cast<int>(m_groups.size())) playLevel(m_groups[tag - kGroupPlayTag].level);
            return;
        }
        if (tag >= kGroupMapTag) {
            if (tag - kGroupMapTag < static_cast<int>(m_groups.size())) openLevel(m_groups[tag - kGroupMapTag].level);
            return;
        }
        if (tag >= kWatchTag) {
            watchGroup(tag - kWatchTag);
            return;
        }
        if (tag >= kPlayTag) {
            playMap(tag - kPlayTag);
            return;
        }
        if (tag >= kMapTag) {
            openMap(tag - kMapTag);
            return;
        }
        int const match = tag / 2, side = tag % 2;
        if (tag < 0 || match >= static_cast<int>(m_matches.size())) return;
        if (auto* popup = PlayerPopup::create(m_matches[match].players[side].profile)) popup->show();
    }
    // 목록의 섹션 제목(RANKED / PUBLIC / GROUP + 개수)과 구분선을 그린다.
    void section(float width, float& y, char const* title, size_t count, ccColor3B tint) {
        y -= 18.f;
        auto* header = CCNode::create();
        header->setTag(-1);
        header->setContentSize({width, 18.f});
        header->setPosition({0.f, y});
        m_scroll->m_contentLayer->addChild(header);
        auto* text =
            label(header, fmt::format("{}  ({})", title, count), {4.f, 9.f}, .3f, 160.f, tint, true, "goldFont.fnt");
        float const lineStart = 12.f + text->getScaledContentSize().width;
        auto* line =
            CCLayerColor::create(ccc4(tint.r, tint.g, tint.b, 150), std::max(0.f, width - lineStart - 4.f), 1.f);
        line->setPosition({lineStart, 8.5f});
        header->addChild(line);
    }
    // 목록 전체를 다시 그린다. 스크롤 위치는 이전 행이 있었으면 최대한 유지한다.
    // 탭 필터: 0 전체, 1 Ranked, 2 Public, 3 Group (Group 섹션은 전체/Group 탭에만 나온다).
    // 섹션 순서는 Ranked, Public, Group이며 각각 정렬 모드와 검색어를 적용한다.
    void fill() {
        float offset = 0.f;
        bool hadRows = false;
        if (m_scroll) {
            hadRows = m_scroll->m_contentLayer->getChildrenCount() > 0;
            offset = m_scroll->m_contentLayer->getPositionY();
            m_scroll->removeFromParent();
        }
        m_scroll =
            TapScrollLayer::create({kWidth - 24.f, kListHeight}, [self = Weak<LiveMatchesPopup>(this)](CCNode* row) {
                if (auto owner = self.lock()) owner->openPlayer(row->getTag());
            });
        m_scroll->setPosition({12.f, 34.f});
        m_mainLayer->addChild(m_scroll);
        float const width = kWidth - 24.f;
        std::vector<size_t> ranked, open, grouped;
        for (size_t i = 0; i < m_matches.size(); ++i) {
            if (m_filter == 3 || (m_matches[i].ranked ? m_filter == 2 : m_filter == 1)) continue;
            if (!matchesQuery(m_matches[i])) continue;
            (m_matches[i].ranked ? ranked : open).push_back(i);
        }
        if (m_filter == 0 || m_filter == 3)
            for (size_t i = 0; i < m_groups.size(); ++i)
                if (matchesQuery(m_groups[i])) grouped.push_back(i);
        sortByRank(ranked, false);
        sortByRank(open, false);
        sortByRank(grouped, true);
        int const sections = (ranked.empty() ? 0 : 1) + (open.empty() ? 0 : 1) + (grouped.empty() ? 0 : 1);
        float const height = std::max(kListHeight, (ranked.size() + open.size() + grouped.size()) * (kRowHeight + 4.f) +
                                                       sections * 18.f);
        m_status->setVisible(ranked.empty() && open.empty() && grouped.empty());
        m_status->setString(!m_query.empty() ? "No live match with that player."
                            : m_filter == 1  ? "No Ranked matches right now."
                            : m_filter == 2  ? "No Public matches right now."
                            : m_filter == 3  ? "No Group battles right now."
                                             : "No matches are being played right now.");
        m_status->limitLabelWidth(380.f, .4f, .2f);
        m_scroll->m_contentLayer->setContentSize({width, height});
        auto const now = Service::get().serverNow();
        float y = height;
        // 1v1 전투 행: 맵 제목/재생 버튼, 규칙/경과 시간/시리즈 점수, 두 플레이어 줄과 눈 아이콘.
        auto row = [&](size_t i) {
            auto const& match = m_matches[i];
            y -= kRowHeight + 4.f;
            float const bottom = y + 2.f;
            auto* holder = CCNode::create();
            holder->setContentSize({width, kRowHeight});
            holder->setTag(-1);
            holder->setPosition({0.f, bottom});
            m_scroll->m_contentLayer->addChild(holder);
            rankedPanel(holder, {0.f, 0.f}, {width, kRowHeight}, match.ranked ? ccc3(60, 28, 14) : ccc3(14, 28, 60),
                        200);
            if (auto* face = mapDifficulty(match.level)) {
                face->setScale(.42f);
                face->setPosition({16.f, kRowHeight - 13.f});
                holder->addChild(face);
            }
            auto* title =
                label(holder, match.level.name.empty() ? fmt::format("Level {}", match.level.id) : match.level.name,
                      {32.f, kRowHeight - 12.f}, .32f, 150.f, ccc3(255, 238, 160), true, "bigFont.fnt");
            auto* mapTap = CCNode::create();
            mapTap->setContentSize({32.f + title->getScaledContentSize().width + 8.f, 22.f});
            mapTap->setAnchorPoint({0.f, .5f});
            mapTap->setPosition({0.f, bottom + kRowHeight - 12.f});
            mapTap->setTag(kMapTag + static_cast<int>(i));
            m_scroll->m_contentLayer->addChild(mapTap);
            float const playX = 32.f + title->getScaledContentSize().width + 19.f;
            if (auto* play = playIcon()) {
                play->setPosition({playX, kRowHeight - 12.f});
                holder->addChild(play);
                auto* playTap = CCNode::create();
                playTap->setContentSize({24.f, 24.f});
                playTap->setAnchorPoint({.5f, .5f});
                playTap->setPosition({playX, bottom + kRowHeight - 12.f});
                playTap->setTag(kPlayTag + static_cast<int>(i));
                m_scroll->m_contentLayer->addChild(playTap);
            }
            auto info = fmt::format("{}  |  {}", ruleText(match.rules),
                                    match.releasedAt > 0 ? clock(now - match.releasedAt) : std::string("LOADING"));
            if (!match.ranked && (match.scoreHost || match.scoreGuest))
                info += fmt::format("  |  SERIES {}-{}", match.scoreHost, match.scoreGuest);
            info += rankNote(match.avgRank);
            label(holder, info, {width - 8.f, kRowHeight - 12.f}, .24f, 200.f,
                  match.ranked ? ccc3(255, 190, 110) : ccc3(140, 200, 255), false, "bigFont.fnt")
                ->setAnchorPoint({1.f, .5f});
            auto const shape = freeplay::battleOf(match);
            for (int side = 0; side < 2; ++side) {
                float const lineY = side == 0 ? 32.f : 12.f;
                playerLine(holder, match.players[side], match.rules, match.ranked, lineY);
                if (!match.battleId.empty())
                    addEye(holder, width, bottom, kPlayerEyeTag + static_cast<int>(i) * 2 + side,
                           watchable(shape, match.players[side].profile.uid), lineY, kPlayerEyeSize);
                auto* tap = CCNode::create();
                tap->setContentSize({width, 20.f});
                tap->setAnchorPoint({.5f, .5f});
                tap->setPosition({width / 2.f, bottom + lineY});
                tap->setTag(static_cast<int>(i) * 2 + side);
                m_scroll->m_contentLayer->addChild(tap);
            }
        };
        // Group 전투 행: 맵 제목/재생 버튼, 규칙/경과 시간/생존자 수, 상위 2명 줄, SPECTATE 버튼.
        auto groupRow = [&](size_t i) {
            auto const& group = m_groups[i];
            y -= kRowHeight + 4.f;
            float const bottom = y + 2.f;
            auto* holder = CCNode::create();
            holder->setContentSize({width, kRowHeight});
            holder->setTag(-1);
            holder->setPosition({0.f, bottom});
            m_scroll->m_contentLayer->addChild(holder);
            rankedPanel(holder, {0.f, 0.f}, {width, kRowHeight}, ccc3(36, 18, 64), 200);
            if (auto* face = mapDifficulty(group.level)) {
                face->setScale(.42f);
                face->setPosition({16.f, kRowHeight - 13.f});
                holder->addChild(face);
            }
            auto* title =
                label(holder, group.level.name.empty() ? fmt::format("Level {}", group.level.id) : group.level.name,
                      {32.f, kRowHeight - 12.f}, .32f, 116.f, ccc3(255, 238, 160), true, "bigFont.fnt");
            auto* mapTap = CCNode::create();
            mapTap->setContentSize({32.f + title->getScaledContentSize().width + 8.f, 22.f});
            mapTap->setAnchorPoint({0.f, .5f});
            mapTap->setPosition({0.f, bottom + kRowHeight - 12.f});
            mapTap->setTag(kGroupMapTag + static_cast<int>(i));
            m_scroll->m_contentLayer->addChild(mapTap);
            float const playX = 32.f + title->getScaledContentSize().width + 19.f;
            if (auto* play = playIcon()) {
                play->setPosition({playX, kRowHeight - 12.f});
                holder->addChild(play);
                auto* playTap = CCNode::create();
                playTap->setContentSize({24.f, 24.f});
                playTap->setAnchorPoint({.5f, .5f});
                playTap->setPosition({playX, bottom + kRowHeight - 12.f});
                playTap->setTag(kGroupPlayTag + static_cast<int>(i));
                m_scroll->m_contentLayer->addChild(playTap);
            }
            float const eyeRoom = kEyeSize + 6.f;
            addEye(holder, width, bottom, kGroupEyeTag + static_cast<int>(i),
                   group.releasedAt > 0 && group.alive > 0 && battle::watchableRules(group.rules));
            auto* spectate = ButtonSprite::create("SPECTATE", "goldFont.fnt", "GJ_button_02.png");
            spectate->setScale(.42f);
            float const spectateWidth = spectate->getScaledContentSize().width + eyeRoom;
            spectate->setPosition(
                {width - 6.f - spectateWidth + spectate->getScaledContentSize().width / 2.f, kRowHeight - 12.f});
            holder->addChild(spectate);
            auto info = fmt::format("{}  |  {}  |  {} / {} PLAYING", ruleText(group.rules),
                                    group.releasedAt > 0 ? clock(now - group.releasedAt) : std::string("LOADING"),
                                    group.alive, group.count) +
                        rankNote(group.avgRank);
            auto* infoLabel = label(holder, info, {width - 12.f - spectateWidth, kRowHeight - 12.f}, .24f,
                                    std::max(60.f, width - 12.f - spectateWidth - playX - 16.f), ccc3(205, 160, 255),
                                    false, "bigFont.fnt");
            infoLabel->setAnchorPoint({1.f, .5f});
            auto const leaders = leadersBattle(group);
            for (size_t place = 0; place < group.top.size() && place < 2; ++place) {
                float const lineY = place == 0 ? 32.f : 12.f;
                playerLine(holder, group.top[place], group.rules, false, lineY);
                addEye(holder, width, bottom, kLeaderEyeTag + static_cast<int>(i) * 2 + static_cast<int>(place),
                       watchable(leaders, group.top[place].profile.uid), lineY, kPlayerEyeSize);
            }
            auto* watchTap = CCNode::create();
            watchTap->setContentSize({width, kRowHeight});
            watchTap->setPosition({0.f, bottom});
            watchTap->setTag(kWatchTag + static_cast<int>(i));
            m_scroll->m_contentLayer->addChild(watchTap);
        };
        if (!ranked.empty()) {
            section(width, y, "RANKED", ranked.size(), ccc3(255, 190, 110));
            for (auto i : ranked) row(i);
        }
        if (!open.empty()) {
            section(width, y, "PUBLIC", open.size(), ccc3(140, 200, 255));
            for (auto i : open) row(i);
        }
        if (!grouped.empty()) {
            section(width, y, "GROUP", grouped.size(), ccc3(205, 160, 255));
            for (auto i : grouped) groupRow(i);
        }
        if (hadRows) {
            float const lowest = m_scroll->getContentSize().height - height;
            m_scroll->m_contentLayer->setPositionY(std::clamp(offset, lowest, 0.f));
        } else
            m_scroll->scrollToTop();
    }
    // 라이브 목록을 서버에서 받아 갱신한다. 실패하면 목록이 비어 있을 때만 오류 문구를 보여 준다.
    void load() {
        if (m_loading) return;
        m_loading = true;
        Service::get().fetchLiveMatches([self = Weak<LiveMatchesPopup>(this)](LiveList list, std::string detail) {
            auto owner = self.lock();
            if (!owner) return;
            owner->m_loading = false;
            owner->m_refresh = 3.f;
            if (!detail.empty()) {
                if (owner->m_matches.empty() && owner->m_groups.empty()) owner->m_status->setString(detail.c_str());
                owner->m_status->limitLabelWidth(380.f, .4f, .2f);
                return;
            }
            owner->m_matches = std::move(list.matches);
            owner->m_groups = std::move(list.groups);
            owner->m_count->setString(
                fmt::format("{} LIVE", list.count + static_cast<int>(owner->m_groups.size())).c_str());
            owner->fill();
        });
    }
    // 3초마다 목록을 다시 받고, 맵을 받는 중이면 진행률 문구를 갱신한다.
    void update(float dt) override {
        m_refresh -= dt;
        if (m_refresh <= 0.f && !m_loading) {
            m_refresh = 3.f;
            load();
        }
        if (m_preparing) {
            auto const progress = mapDownloadProgress(m_preparing);
            m_playStatus->setString(fmt::format("Downloading map for play... {}%", progress.percent).c_str());
            m_playStatus->limitLabelWidth(380.f, .32f, .2f);
        }
    }
    // 재생 버튼 스프라이트 (프레임이 없으면 대체 스프라이트, 둘 다 없으면 nullptr).
    static CCSprite* playIcon() {
        auto* sprite = loadedFrameSprite("GJ_playBtn2_001.png", "GJ_GameSheet03.plist");
        if (!sprite) sprite = loadedFrameSprite("GJ_playBtn_001.png");
        if (!sprite) return nullptr;
        sprite->setScale(20.f / std::max(1.f, sprite->getContentSize().height));
        return sprite;
    }
    // 팝업 레이아웃 구성: 정렬 버튼, 현재 LIVE 개수, 상태/안내 문구, 탭 4개, 검색창.
    bool init() {
        if (!Popup::init(kWidth, 290.f, "GJ_square02.png")) return false;
        setTitle("LIVE MATCHES", "goldFont.fnt", .75f);
        m_title->setZOrder(2);
        if (auto* mod = Mod::get()) m_sort = std::clamp(mod->getSavedValue<int>("live-sort", 0), 0, 2);
        m_sortButton = button(m_buttonMenu, this, menu_selector(LiveMatchesPopup::onSort), kSortNames[0],
                              {82.f, 290.f - 22.f}, .36f, "GJ_button_04.png");
        m_sortButton->setID("live-sort-button"_spr);
        paintSort();
        m_count =
            label(m_mainLayer, "", {kWidth - 40.f, 290.f - 22.f}, .3f, 60.f, ccc3(255, 110, 110), false, "bigFont.fnt");
        m_status = label(m_mainLayer, "Loading live matches...", {kWidth / 2.f, 140.f}, .4f, 380.f, kIce, false,
                         "bigFont.fnt");
        m_status->setZOrder(3);
        label(m_mainLayer,
              "Updates every 3s  |  tap a map to open it, Play to play it, a player for the profile, "
              "a Group battle for its standings, an eye to watch that player live",
              {kWidth / 2.f, 18.f}, .3f, 400.f, kMuted, false, "chatFont.fnt");
        m_playStatus =
            label(m_mainLayer, "", {kWidth / 2.f, 30.f}, .32f, 380.f, ccc3(140, 255, 170), false, "bigFont.fnt");
        m_playStatus->setZOrder(5);
        m_playStatus->setVisible(false);
        auto* tabs = menu(m_mainLayer);
        for (int i = 0; i < kTabs; ++i) {
            m_tabs[i] = button(tabs, this, menu_selector(LiveMatchesPopup::onTab), kTabNames[i],
                               {48.f + i * 64.f, 244.f}, .4f, i == 0 ? "GJ_button_01.png" : "GJ_button_04.png");
            m_tabs[i]->setTag(i);
        }
        m_search = TextInput::create(140.f, "Search player");
        m_search->setCommonFilter(CommonFilter::Any);
        m_search->setMaxCharCount(20);
        m_search->setScale(.75f);
        m_search->setWidth(140.f / .75f);
        m_search->setPosition({kWidth - 80.f, 244.f});
        styleInput(m_search);
        // 검색어를 소문자/양끝 공백 제거 후 바뀐 경우에만 목록을 다시 그린다.
        m_search->setCallback([self = Weak<LiveMatchesPopup>(this)](std::string const& text) {
            auto owner = self.lock();
            if (!owner) return;
            auto query = folded(std::string(text));
            query.erase(0, query.find_first_not_of(' '));
            while (!query.empty() && query.back() == ' ') query.pop_back();
            if (query == owner->m_query) return;
            owner->m_query = std::move(query);
            if (owner->m_scroll) {
                owner->m_scroll->removeFromParent();
                owner->m_scroll = nullptr;
            }
            owner->fill();
        });
        m_mainLayer->addChild(m_search, 4);
        load();
        scheduleUpdate();
        return true;
    }
    static constexpr char const* kTabNames[kTabs] = {"All", "Ranked", "Public", "Group"};
    // 탭 전환: 선택된 탭 버튼의 스프라이트를 바꾸고 목록을 다시 그린다.
    void onTab(CCObject* sender) {
        int const tab = sender->getTag();
        if (tab == m_filter || tab < 0 || tab >= kTabs) return;
        m_filter = tab;
        for (int i = 0; i < kTabs; ++i) {
            auto* sprite = ButtonSprite::create(kTabNames[i], "goldFont.fnt",
                                                i == m_filter ? "GJ_button_01.png" : "GJ_button_04.png");
            sprite->setScale(.4f);
            m_tabs[i]->setNormalImage(sprite);
            m_tabs[i]->updateSprite();
        }
        if (m_scroll) {
            m_scroll->removeFromParent();
            m_scroll = nullptr;
        }
        fill();
    }

public:
    static LiveMatchesPopup* create() {
        auto* result = new LiveMatchesPopup;
        if (result->init()) {
            result->autorelease();
            return result;
        }
        delete result;
        return nullptr;
    }
};

// Live 매치 팝업을 띄운다.
void showLiveMatches() {
    if (auto* popup = LiveMatchesPopup::create()) popup->show();
}
