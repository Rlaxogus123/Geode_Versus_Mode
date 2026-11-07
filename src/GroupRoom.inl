// Group Room(최대 75명 그룹 방) 화면과 관련 팝업 모음.
// 구성: 규칙 설정 GroupRulesPopup, 결과 GroupResultPopup, 관전 GroupSpectatePopup,
// 강퇴/차단 선택 RemovePlayerPopup, 방 화면 본체 GroupRoomLayer. 방 상태는 Service::get().group()에서 읽고,
// 변경은 Service 호출로 서버에 보낸다.
//
// 방장이 그룹 방 규칙(모드, 연습, 생명/목표 %, 최대 인원, 속도)을 바꾸는 팝업.
// m_mode 0은 Attempts(생명), 1은 Percent(목표 %). 요청 중(m_pending)에는 입력을 막는다.
class GroupRulesPopup final : public ::versus::DeferredPopup {
    int m_attempts = 3, m_max = kGroupMaxPlayers, m_minimum = kGroupMinPlayers, m_speed = 100;
    int m_mode = 0, m_target = 40;
    bool m_practice = false;
    bool m_pending = false;
    std::string m_roomID;
    LevelInfo m_level;
    bool m_random = false;
    bool m_autoLives = false;
    CCMenuItemToggler* m_auto = nullptr;
    CCLabelBMFont* m_autoLabel = nullptr;
    CCLabelBMFont* m_autoHint = nullptr;
    CCLabelBMFont* m_speedLabel = nullptr;
    CCMenuItemSpriteExtra* m_modeButton = nullptr;
    CCMenuItemSpriteExtra* m_practiceButton = nullptr;
    CCLabelBMFont* m_valueCaption = nullptr;
    CCLabelBMFont* m_unlimited = nullptr;
    CCLabelBMFont* m_rulesHint = nullptr;
    TextInput* m_livesInput = nullptr;
    TextInput* m_maxInput = nullptr;
    CCMenuItemSpriteExtra* m_apply = nullptr;
    std::vector<CCMenuItemSpriteExtra*> m_steps;

    // lives(): 생명제 모드, unlimited(): 연습 모드라 시도 제한 없음.
    bool lives() const { return m_mode == 0 && !m_practice; }
    bool unlimited() const { return m_mode == 0 && m_practice; }
    static void caption(CCMenuItemSpriteExtra* item, char const* text, char const* background) {
        auto* sprite = ButtonSprite::create(text, "goldFont.fnt", background);
        sprite->setScale(.42f);
        item->setSprite(sprite);
    }
    void onExit() override {
        if (m_livesInput) m_livesInput->defocus();
        if (m_maxInput) m_maxInput->defocus();
        Popup::onExit();
    }
    // 요청 중 / 자동 생명 / 연습 여부에 따라 버튼과 입력창의 활성 상태를 정한다.
    void refresh() {
        bool const autoOn = m_autoLives && lives();
        for (auto* item : m_steps) {
            bool const value = item->getTag() < 10;
            enabled(item, !m_pending && !(value && (autoOn || unlimited())));
            if (value) item->setVisible(!unlimited());
        }
        m_auto->setEnabled(!m_pending);
        enabled(m_apply, !m_pending);
        enabled(m_modeButton, !m_pending);
        enabled(m_practiceButton, !m_pending);
        m_livesInput->setEnabled(!m_pending && !autoOn && !unlimited());
        m_livesInput->getBGSprite()->setOpacity(autoOn ? 105 : 230);
        m_maxInput->setEnabled(!m_pending);
    }
    // 현재 값(모드, 연습, 생명/목표, 최대 인원, 속도)을 버튼 문구와 입력창, 안내 문구에 반영한다.
    // 자동 생명이 켜져 있으면 레벨에서 계산한 생명 수를 쓴다.
    void showValues() {
        caption(m_modeButton, m_mode == 1 ? "Mode: Percent" : "Mode: Attempts", "GJ_button_04.png");
        caption(m_practiceButton, m_practice ? "Practice: ON" : "Practice: OFF",
                m_practice ? "GJ_button_01.png" : "GJ_button_04.png");
        m_valueCaption->setString(m_mode == 1 ? "TARGET % (1-100)" : "LIVES");
        if (m_autoLives && lives())
            if (auto const value = autoLives(m_level, m_random)) m_attempts = value->lives;
        m_livesInput->setString(std::to_string(m_mode == 1 ? m_target : m_attempts).c_str());
        m_livesInput->setVisible(!unlimited());
        m_unlimited->setVisible(unlimited());
        m_maxInput->setString(std::to_string(m_max).c_str());
        m_speedLabel->setString(speedText(m_speed).c_str());
        m_speedLabel->setColor(m_speed == 100 ? ccWHITE : ccc3(255, 120, 200));
        for (CCNode* node :
             {static_cast<CCNode*>(m_auto), static_cast<CCNode*>(m_autoLabel), static_cast<CCNode*>(m_autoHint)})
            node->setVisible(lives());
        m_autoHint->setString(autoLivesLine(m_autoLives, m_level, m_random, "lives").c_str());
        m_autoHint->limitLabelWidth(260.f, .3f, .1f);
        m_autoHint->setColor(m_autoLives ? ccc3(150, 255, 175) : kMuted);
        m_rulesHint->setString(m_mode == 1  ? "The first to reach the target % wins."
                               : m_practice ? "Practice, no attempt limit: the fewest failed runs to a clear wins."
                                            : "Best % wins, then fewer failed runs, then coins.");
        m_rulesHint->limitLabelWidth(260.f, .5f, .1f);
    }
    // 맵이 아직 추첨 전(풀에서 굴려야 하거나 랜덤 추첨이 확정되지 않음)인지.
    static bool toRoll(GroupRoomInfo const& room) {
        if (room.mapPool) return poolRolls(*room.mapPool) && room.level.id <= 0;
        return room.mapSelection.random && (!room.mapDraw || !room.mapDraw->settled);
    }
    // 입력창에 직접 친 값이 유효하면 내부 값으로 옮긴다.
    void keepTyped() {
        int const typed = typedNumber(m_livesInput);
        if (m_mode == 1) {
            if (typed >= 1 && typed <= 100) m_target = typed;
        } else if (!unlimited() && !m_autoLives && typed >= 1 && typed <= 99)
            m_attempts = typed;
        m_livesInput->defocus();
    }
    // 자동 생명 토글. 켜기 전에 방의 현재 레벨 정보를 다시 읽는다.
    void onAuto(CCObject*) {
        if (m_pending) return;
        if (auto const& room = Service::get().group(); room && room->id == m_roomID) {
            m_level = room->level;
            m_random = toRoll(*room);
        }
        keepTyped();
        m_autoLives = !m_auto->isToggled();
        showValues();
        refresh();
    }
    void onMode(CCObject*) {
        if (m_pending) return;
        keepTyped();
        m_mode = 1 - m_mode;
        showValues();
        refresh();
    }
    void onPractice(CCObject*) {
        if (m_pending) return;
        keepTyped();
        m_practice = !m_practice;
        showValues();
        refresh();
    }
    // +/- 버튼 처리. tag 십의 자리가 대상(0 생명/목표, 1 최대 인원, 2 속도), 일의 자리가 증감량이다.
    void step(CCObject* sender) {
        if (m_pending) return;
        int const tag = sender->getTag();
        int const delta = tag % 10 == 1 ? -1 : tag % 10 == 2 ? 1 : tag % 10 == 3 ? -5 : 5;
        if (tag < 10 && (unlimited() || (m_autoLives && lives()))) return;
        if (tag < 10 && m_mode == 1)
            m_target = std::clamp(m_target + delta, 1, 100);
        else if (tag < 10)
            m_attempts = std::clamp(m_attempts + delta, 1, 99);
        else if (tag < 20)
            m_max = std::clamp(m_max + delta, m_minimum, kGroupMaxPlayers);
        else
            m_speed = std::clamp(m_speed + (delta == 1 || delta == -1 ? 25 * delta : 20 * delta), 50, 300);
        showValues();
    }
    // 입력값을 검증하고(목표 1-100, 생명 1-99, 인원 범위) 서버에 규칙을 보낸다. 성공하면 닫는다.
    void onApply(CCObject*) {
        if (m_pending) return;
        int const typed = typedNumber(m_livesInput), size = typedNumber(m_maxInput);
        if (m_mode == 1) {
            if (typed < 1 || typed > 100) {
                error("Enter a target of <cy>1-100</c>%.");
                return;
            }
            m_target = typed;
        } else if (lives()) {
            int const count = m_autoLives ? m_attempts : typed;
            if (count < 1 || count > 99) {
                error("Enter <cy>1-99</c> lives.");
                return;
            }
            m_attempts = count;
        }
        if (size < m_minimum || size > kGroupMaxPlayers) {
            error(fmt::format("Enter <cy>{}-{}</c> players.", m_minimum, kGroupMaxPlayers));
            return;
        }
        m_max = size;
        m_livesInput->defocus();
        m_maxInput->defocus();
        m_pending = true;
        refresh();
        Service::get().configureGroup(m_attempts, m_max, m_speed, m_autoLives, m_mode, m_practice, m_target,
                                      [self = Weak<GroupRulesPopup>(this)](bool ok, std::string detail) {
                                          auto owner = self.lock();
                                          if (!owner) return;
                                          owner->m_pending = false;
                                          owner->refresh();
                                          if (!ok) {
                                              error(detail);
                                              return;
                                          }
                                          owner->onClose(nullptr);
                                      });
    }
    // 방의 현재 규칙으로 값을 채우고 위젯을 배치한다.
    bool init(GroupRoomInfo const& room) {
        if (!Popup::init(300.f, 336.f, "GJ_square02.png")) return false;
        geode::addSideArt(m_mainLayer, SideArt::All, SideArtStyle::PopupBlue);
        setTitle("Group Rules", "bigFont.fnt", .62f);
        m_attempts = std::clamp(room.rules.attempts, 1, 99);
        m_mode = room.rules.mode == 1 ? 1 : 0;
        m_practice = room.rules.practice;
        m_target = std::clamp(room.rules.targetPercent, 1, 100);
        m_speed = validRuleSpeed(room.rules.speed) ? room.rules.speed : 100;
        m_max = room.maxPlayers;
        m_minimum = std::max<int>(kGroupMinPlayers, static_cast<int>(room.members.size()));
        m_roomID = room.id;
        m_level = room.level;
        m_random = toRoll(room);
        m_autoLives = room.rules.autoLives;
        m_modeButton =
            button(m_buttonMenu, this, menu_selector(GroupRulesPopup::onMode), "Mode: Attempts", {92.f, 270.f}, .42f);
        m_modeButton->setID("group-rules-mode"_spr);
        m_practiceButton = button(m_buttonMenu, this, menu_selector(GroupRulesPopup::onPractice), "Practice: OFF",
                                  {210.f, 270.f}, .42f);
        m_practiceButton->setID("group-rules-practice"_spr);
        auto row = [this](char const* caption, float y, int base, TextInput*& value, int initial, size_t digits,
                          std::function<void(int)> typed) {
            auto* title = label(m_mainLayer, caption, {150.f, y + 24.f}, .34f, 220.f, kIce);
            value = numberInput(m_mainLayer, {150.f, y}, initial, digits, 48.f, std::move(typed));
            static constexpr std::array<std::pair<char const*, float>, 4> kButtons{
                {{"-5", 62.f}, {"<", 104.f}, {">", 196.f}, {"+5", 238.f}}};
            static constexpr std::array<int, 4> kTags{3, 1, 2, 4};
            for (size_t i = 0; i < kButtons.size(); ++i) {
                auto* item = button(m_buttonMenu, this, menu_selector(GroupRulesPopup::step), kButtons[i].first,
                                    {kButtons[i].second, y}, .5f, "GJ_button_04.png");
                item->setTag(base + kTags[i]);
                m_steps.push_back(item);
            }
            return title;
        };
        auto self = Weak<GroupRulesPopup>(this);
        m_valueCaption =
            row("LIVES", 212.f, 0, m_livesInput, m_mode == 1 ? m_target : m_attempts, 3, [self](int value) {
                auto owner = self.lock();
                if (!owner) return;
                if (owner->m_mode == 1) {
                    if (value >= 1 && value <= 100) owner->m_target = value;
                } else if (owner->lives() && !owner->m_autoLives && value >= 1 && value <= 99)
                    owner->m_attempts = value;
            });
        m_unlimited = label(m_mainLayer, "No attempt limit", {150.f, 212.f}, .45f, 200.f, kIce);
        row(fmt::format("MAX PLAYERS ({}-{})", m_minimum, kGroupMaxPlayers).c_str(), 152.f, 10, m_maxInput, m_max, 2,
            [self](int value) {
                if (auto owner = self.lock(); owner && value >= owner->m_minimum && value <= kGroupMaxPlayers)
                    owner->m_max = value;
            });
        m_maxInput->setID("group-max-players-input"_spr);
        m_auto = CCMenuItemToggler::createWithStandardSprites(this, menu_selector(GroupRulesPopup::onAuto), .42f);
        m_auto->setID("group-auto-lives"_spr);
        m_auto->setPosition({194.f, 236.f});
        m_auto->toggle(m_autoLives);
        m_buttonMenu->addChild(m_auto);
        m_autoLabel = label(m_mainLayer, "AUTO", {205.f, 236.f}, .3f, 40.f, kIce, true);
        m_autoHint = label(m_mainLayer, "", {150.f, 191.f}, .3f, 260.f, kMuted, false, "chatFont.fnt");
        label(m_mainLayer, "SPEED (0.5x-3x)", {150.f, 116.f}, .34f, 220.f, kIce);
        m_speedLabel = label(m_mainLayer, speedText(m_speed), {150.f, 92.f}, .5f, 70.f);
        m_speedLabel->setID("group-speed-value"_spr);
        static constexpr std::array<std::pair<char const*, float>, 4> kSpeedButtons{
            {{"-1x", 62.f}, {"<", 104.f}, {">", 196.f}, {"+1x", 238.f}}};
        static constexpr std::array<int, 4> kSpeedTags{23, 21, 22, 24};
        for (size_t i = 0; i < kSpeedButtons.size(); ++i) {
            auto* item = button(m_buttonMenu, this, menu_selector(GroupRulesPopup::step), kSpeedButtons[i].first,
                                {kSpeedButtons[i].second, 92.f}, .5f, "GJ_button_04.png");
            item->setTag(kSpeedTags[i]);
            m_steps.push_back(item);
        }
        m_rulesHint = label(m_mainLayer, "", {150.f, 60.f}, .5f, 260.f, kMuted, false, "chatFont.fnt");
        m_apply = button(m_buttonMenu, this, menu_selector(GroupRulesPopup::onApply), "Apply", {150.f, 28.f}, .6f);
        showValues();
        refresh();
        return true;
    }
    void onClose(CCObject* sender) override {
        if (!m_pending) Popup::onClose(sender);
    }

public:
    static GroupRulesPopup* create(GroupRoomInfo const& room) {
        auto* result = new GroupRulesPopup;
        if (result->init(room)) {
            result->autorelease();
            return result;
        }
        delete result;
        return nullptr;
    }
};

// 그룹 배틀 결과를 전체 화면으로 보여 주는 팝업. kGroupResultSeconds가 지나면 자동으로 닫힌다.
class GroupResultPopup final : public ::versus::DeferredPopup {
    CCNode* m_result = nullptr;
    float m_left = kGroupResultSeconds;
    bool init(GroupBattle const& battle) {
        auto const window = CCDirector::sharedDirector()->getWinSize();
        if (!Popup::init(window.width, window.height, "square02b_001.png")) return false;
        m_bgSprite->setOpacity(0);
        m_closeBtn->setVisible(false);
        m_result = createGroupResult(battle, Service::get().profile().uid, "Close", this,
                                     menu_selector(GroupResultPopup::onDone));
        m_mainLayer->addChild(m_result, 10);
        handleTouchPriority(this);
        updateGroupResult(m_result, m_left, kGroupResultSeconds);
        scheduleUpdate();
        return true;
    }
    void onDone(CCObject*) { onClose(nullptr); }

public:
    void update(float dt) override {
        m_left -= dt;
        updateGroupResult(m_result, m_left, kGroupResultSeconds);
        if (m_left <= 0.f) {
            unscheduleUpdate();
            onClose(nullptr);
        }
    }
    static GroupResultPopup* create(GroupBattle const& battle) {
        auto* result = new GroupResultPopup;
        if (result->init(battle)) {
            result->autorelease();
            return result;
        }
        delete result;
        return nullptr;
    }
};

// 방의 맵을 추첨으로 정해야 하는지(아직 레벨이 없거나 추첨이 확정되지 않음).
inline bool groupNeedsRoll(GroupRoomInfo const& room) {
    if (room.mapPool) return poolRolls(*room.mapPool) && room.level.id <= 0;
    return room.mapSelection.random && (!room.mapDraw || !room.mapDraw->settled);
}
// 맵 추첨 룰렛이 진행 중인지.
inline bool groupDrawing(GroupRoomInfo const& room) {
    return room.mapDraw && !room.mapDraw->settled;
}
// 방장이 맵을 다시 추첨할 수 있는지(랜덤 선택이고, 추첨이 끝났으며, 배틀 중이 아닐 때).
inline bool groupCanReroll(GroupRoomInfo const& room) {
    return room.mapSelection.random && room.mapDraw && room.mapDraw->settled && !room.running();
}

// 관전 버튼용 눈 아이콘.
inline CCNode* spectateEye(float height) {
    if (auto* eye = art::artSprite("spectate-eye.png", height)) return eye;
    auto* frame = loadedFrameSprite("hideBtn_001.png", "GJ_GameSheet03.plist");
    if (!frame) return CCNode::create();
    frame->setScale(height / std::max(1.f, frame->getContentSize().height));
    return frame;
}

// 그룹 배틀 관전 팝업. 순위, 상태, 진행도 막대, 최고 %, 남은 생명을 표로 보여 주고
// 플레이 중인 참가자의 눈 버튼으로 그 사람을 관전한다. 세 가지 모드로 쓴다:
// 내 방의 배틀 목록(create), 배틀 중 관전 화면 위(inBattle), 다른 방을 지켜보기(watch).
class GroupSpectatePopup final : public ::versus::DeferredPopup {
    static constexpr float W = 440.f, H = 290.f, ListW = W - 24.f, ListH = 196.f, RowH = 30.f;
    static constexpr float BarX = 176.f, BarW = 130.f;
    static constexpr float BestX = BarX + BarW + 24.f, HeartX = ListW - 60.f, LivesX = ListW - 42.f,
                           EyeX = ListW - 22.f;
    struct Row {
        CCNode* node = nullptr;
        CCLabelBMFont *rank = nullptr, *status = nullptr, *best = nullptr, *now = nullptr, *lives = nullptr;
        CCNode* heart = nullptr;
        CCLayerColor *fill = nullptr, *marker = nullptr;
        CCMenuItemSpriteExtra* eye = nullptr;
        CCLabelBMFont* live = nullptr;
    };
    ScrollLayer* m_scroll = nullptr;
    CCLabelBMFont *m_headline = nullptr, *m_detail = nullptr;
    CCLabelBMFont* m_livesHeader = nullptr;
    std::string m_battle, m_shown;
    std::vector<Row> m_rows;
    float m_tick = 1.f;
    CCMenuItemSpriteExtra* m_play = nullptr;
    bool m_inBattle = false;
    CCLabelBMFont* m_playProgress = nullptr;
    int64_t m_loading = 0;
    std::string m_watchRoom, m_watchName, m_watchEtag;
    std::optional<GroupBattle> m_watched;
    bool m_watchBusy = false, m_watchClosed = false;
    float m_watchWait = 0.f;
    bool watching() const { return !m_watchRoom.empty(); }
    std::string m_pendingWatch, m_pendingName;
    // 지금 관전 중인 플레이어 uid.
    static std::string spectated() { return freeplay::active() ? freeplay::spectatedUid() : battle::spectatedUid(); }
    // 관전을 시작할 수 있는 상태인지(배틀이 풀려 시작됐고 끝나지 않았으며 다른 관전이 열리는 중이 아님).
    bool canWatch() const {
        if (watching()) {
            auto const* shown = shownBattle();
            if (!shown || m_watchClosed || shown->finishedAt > 0 || shown->releasedAt == 0 || freeplay::opening())
                return false;
            if (freeplay::active()) return freeplay::liveRoom() == m_watchRoom && freeplay::canSpectate();
            return true;
        }
        if (m_inBattle) return battle::canSpectate() || freeplay::canSpectate();
        auto const& group = Service::get().group();
        return group && group->running() && group->battle->releasedAt > 0 && m_loading == 0 && !freeplay::active();
    }
    // 눈 버튼: 모드에 따라 라이브 관전을 열거나, 배틀 중이면 대상만 바꾸거나, 맵을 받은 뒤 시작한다.
    void onEye(CCObject* sender) {
        auto const* shown = shownBattle();
        int const index = sender ? sender->getTag() : -1;
        if (!shown || index < 0 || index >= static_cast<int>(shown->seats.size()) || !canWatch()) return;
        auto const& seat = shown->seats[index];
        std::string const name = seat.profile.name.empty() ? std::string("Player") : seat.profile.name;
        if (watching()) {
            if (freeplay::active()) {
                if (freeplay::spectate(seat.profile.uid, name)) onClose(nullptr);
                return;
            }
            std::string why;
            if (freeplay::openLive(*shown, m_watchRoom, m_watchName, seat.profile.uid, name, why))
                onClose(nullptr);
            else if (!why.empty())
                error(why);
            return;
        }
        if (m_inBattle) {
            bool const ok = freeplay::active() ? freeplay::spectate(seat.profile.uid, name)
                                               : battle::spectatePlayer(seat.profile.uid, name);
            if (ok) onClose(nullptr);
            return;
        }
        m_pendingWatch = seat.profile.uid;
        m_pendingName = name;
        onPlay(nullptr);
    }
    // 표시할 배틀: watch 모드면 받아 온 것, 아니면 내 그룹 방의 배틀.
    GroupBattle const* shownBattle() const {
        if (watching()) return m_watched ? &*m_watched : nullptr;
        auto const& group = Service::get().group();
        return group && group->battle ? &*group->battle : nullptr;
    }
    // 다른 방을 지켜볼 때 서버에서 배틀 상태를 주기적으로 받아 온다(끝났으면 더 느리게, 실패하면 3초 후 재시도).
    void pollWatch() {
        m_watchBusy = true;
        m_watchWait = m_watched && m_watched->finishedAt > 0 ? 3.f : 1.f;
        Service::get().watchGroup(m_watchRoom, m_watched ? *m_watched : GroupBattle{}, m_watchEtag,
                                  [self = Weak<GroupSpectatePopup>(this)](GroupWatch watch) {
                                      auto owner = self.lock();
                                      if (!owner) return;
                                      owner->m_watchBusy = false;
                                      if (!watch.ok) {
                                          owner->m_watchWait = 3.f;
                                          return;
                                      }
                                      if (!watch.etag.empty()) owner->m_watchEtag = watch.etag;
                                      if (watch.closed) {
                                          owner->m_watchClosed = true;
                                          owner->m_tick = 1.f;
                                          return;
                                      }
                                      if (!watch.roomName.empty()) owner->m_watchName = watch.roomName;
                                      if (watch.changed) {
                                          owner->m_watched = std::move(watch.battle);
                                          owner->m_tick = 1.f;
                                      }
                                  });
    }

    void onExit() override {
        if (!watching() && !m_inBattle) Service::get().setGroupSpectating(false);
        Popup::onExit();
    }
    // 좌석 상태를 표시 문구와 색으로 바꾼다.
    static std::string statusOf(GroupSeat const& seat, GameRules const& rules, ccColor3B& color) {
        auto const& state = seat.state;
        if (seat.disqualified) {
            color = ccc3(255, 90, 90);
            return "DISQUALIFIED";
        }
        if (state.cleared) {
            color = ccc3(120, 255, 150);
            return "CLEARED";
        }
        if (state.forfeited || seat.out) {
            color = ccc3(150, 155, 170);
            return "OUT";
        }
        if (battle::resultFinal(state, rules)) {
            color = ccc3(255, 200, 110);
            return rules.mode == 1 ? "TARGET REACHED" : "NO LIVES LEFT";
        }
        if (state.paused) {
            color = ccc3(255, 230, 90);
            return "PAUSED";
        }
        if (state.inAttempt) {
            color = ccc3(110, 220, 255);
            return "PLAYING";
        }
        color = ccc3(190, 200, 220);
        return "RESTARTING";
    }
    // 표시 순서. 서버 ranking이 유효하면 그대로 쓰고, 아니면 탈락 여부, 클리어, 최고 %, 현재 % 순으로 정렬한다.
    static std::vector<int> order(GroupBattle const& battle) {
        std::vector<int> result;
        if (battle.ranking.size() == battle.seats.size()) {
            for (int index : battle.ranking)
                if (index >= 0 && index < static_cast<int>(battle.seats.size())) result.push_back(index);
            if (result.size() == battle.seats.size()) return result;
            result.clear();
        }
        for (int i = 0; i < static_cast<int>(battle.seats.size()); ++i) result.push_back(i);
        std::stable_sort(result.begin(), result.end(), [&battle](int a, int b) {
            auto const& x = battle.seats[a];
            auto const& y = battle.seats[b];
            bool const xOut = x.state.forfeited || x.out || x.disqualified;
            bool const yOut = y.state.forfeited || y.out || y.disqualified;
            if (xOut != yOut) return !xOut;
            if (x.state.cleared != y.state.cleared) return x.state.cleared;
            if (x.state.bestPercent != y.state.bestPercent) return x.state.bestPercent > y.state.bestPercent;
            return x.state.currentPercent > y.state.currentPercent;
        });
        return result;
    }
    // 좌석마다 한 줄(순위, 아이콘, 이름, 상태, 진행도 막대, 최고 %, 생명, 관전 버튼)을 만든다.
    void build(GroupBattle const& battle) {
        m_battle = battle.id;
        m_shown.clear();
        m_rows.clear();
        auto* content = m_scroll->m_contentLayer;
        content->removeAllChildrenWithCleanup(true);
        float const height = std::max(ListH, RowH * battle.seats.size() + 4.f);
        content->setContentSize({ListW, height});
        for (auto const& seat : battle.seats) {
            Row row;
            row.node = CCNode::create();
            row.node->setContentSize({ListW - 8.f, RowH - 3.f});
            content->addChild(row.node);
            auto* back = NineSlice::create("square02b_001.png");
            back->setContentSize({ListW - 8.f, RowH - 3.f});
            back->setAnchorPoint({0.f, 0.f});
            back->setColor({0, 0, 0});
            back->setOpacity(seat.profile.uid == Service::get().profile().uid ? 130 : 70);
            row.node->addChild(back);
            float const mid = (RowH - 3.f) / 2.f;
            row.rank = label(row.node, "#", {16.f, mid}, .3f, 26.f, ccc3(255, 214, 90), false, "bigFont.fnt");
            player(row.node, seat.profile, {42.f, mid}, .42f);
            auto* name = label(row.node, seat.profile.name.empty() ? "Player" : seat.profile.name, {58.f, mid + 5.f},
                               .3f, 108.f, ccWHITE, true, "bigFont.fnt");
            styleNickname(name, seat.profile.membership, seat.profile.cosmetics);
            row.status = label(row.node, "", {58.f, mid - 7.f}, .36f, 108.f, kIce, true, "chatFont.fnt");
            auto* track = CCLayerColor::create(ccc4(255, 255, 255, 30), BarW, 7.f);
            track->setPosition({BarX, mid - 1.f});
            row.node->addChild(track);
            row.fill = CCLayerColor::create(ccc4(255, 255, 255, 255), BarW, 7.f);
            row.fill->setPosition({BarX, mid - 1.f});
            row.fill->setScaleX(0.f);
            row.fill->ignoreAnchorPointForPosition(false);
            row.fill->setAnchorPoint({0.f, 0.f});
            row.node->addChild(row.fill);
            row.marker = CCLayerColor::create(ccc4(255, 255, 255, 230), 2.f, 11.f);
            row.marker->setPosition({BarX, mid - 3.f});
            row.node->addChild(row.marker);
            row.now = label(row.node, "", {BarX, mid - 8.f}, .34f, BarW, kMuted, true, "chatFont.fnt");
            row.best = label(row.node, "", {BestX, mid + 2.f}, .34f, 44.f, ccWHITE, false, "bigFont.fnt");
            row.heart = createLifeHeart(10.f);
            row.heart->setPosition({HeartX, mid});
            row.node->addChild(row.heart);
            row.lives = label(row.node, "", {LivesX, mid}, .32f, 26.f, ccc3(255, 150, 170), false, "bigFont.fnt");
            auto* actions = CCMenu::create();
            actions->setPosition({0.f, 0.f});
            actions->setContentSize(row.node->getContentSize());
            row.node->addChild(actions, 2);
            auto* eyeSprite =
                CircleButtonSprite::create(spectateEye(16.f), CircleBaseColor::Gray, CircleBaseSize::Tiny);
            eyeSprite->setScale(.82f);
            row.eye = CCMenuItemSpriteExtra::create(eyeSprite, this, menu_selector(GroupSpectatePopup::onEye));
            row.eye->setTag(static_cast<int>(m_rows.size()));
            row.eye->setPosition({EyeX, mid});
            row.eye->setID("group-spectate-eye"_spr);
            row.eye->setVisible(false);
            actions->addChild(row.eye);
            row.live = label(row.node, "LIVE", {EyeX, mid}, .3f, 30.f, ccc3(255, 80, 80), false, "bigFont.fnt");
            row.live->setVisible(false);
            m_rows.push_back(row);
        }
        m_scroll->scrollToTop();
    }
    // 배틀 상태를 행에 반영한다. 바뀐 값의 키 문자열이 같으면 건너뛰어 불필요한 갱신을 줄인다.
    void refresh() {
        auto const* shown = shownBattle();
        if (!shown) {
            if (!watching()) {
                if (isRunning()) onClose(nullptr);
                return;
            }
            m_headline->setString(m_watchClosed ? "THIS BATTLE IS NO LONGER LIVE" : "LOADING...");
            m_headline->setColor(kIce);
            m_headline->limitLabelWidth(W - 40.f, .42f, .2f);
            return;
        }
        auto const& battle = *shown;
        if (battle.id != m_battle || m_rows.size() != battle.seats.size()) build(battle);
        std::string key = fmt::format("{}:{}:{}:{}:{}:{}", battle.releasedAt, battle.finishedAt, battle.winnerUid,
                                      m_watchClosed, spectated(), canWatch());
        for (auto const& seat : battle.seats)
            key += fmt::format("|{},{},{},{}{}{}{}{}{}", seat.state.bestPercent, seat.state.currentPercent,
                               seat.state.attemptsUsed, seat.state.cleared, seat.state.forfeited, seat.state.paused,
                               seat.state.inAttempt, seat.disqualified, seat.out);
        if (key == m_shown) return;
        m_shown = std::move(key);
        auto const& rules = battle.rules;
        int playing = 0;
        for (auto const& seat : battle.seats) playing += battle::resultFinal(seat.state, rules) || seat.out ? 0 : 1;
        if (battle.finishedAt > 0) {
            int const winner = battle.seatOf(battle.winnerUid);
            m_headline->setString(
                battle.draw || winner < 0
                    ? "BATTLE OVER - DRAW"
                    : fmt::format("BATTLE OVER - {} WINS", battle.seats[winner].profile.name).c_str());
            m_headline->setColor(ccc3(255, 214, 90));
        } else if (m_watchClosed) {
            m_headline->setString("ROOM CLOSED");
            m_headline->setColor(kIce);
        } else {
            m_headline->setString(battle.releasedAt > 0 ? "LIVE" : "LOADING...");
            m_headline->setColor(battle.releasedAt > 0 ? ccc3(255, 90, 90) : kIce);
        }
        m_headline->limitLabelWidth(W - 40.f, .42f, .2f);
        m_detail->setString(fmt::format("{}{}  |  {} of {} still playing{}",
                                        watching() && !m_watchName.empty() ? m_watchName + "  |  " : std::string(),
                                        battle.level.name, playing, battle.seats.size(),
                                        (rules.mode == 1  ? fmt::format("  |  first to {}%", rules.targetPercent)
                                         : rules.practice ? std::string("  |  practice, fewest failed runs")
                                                          : fmt::format("  |  {} lives", rules.attempts)) +
                                            (rules.speed != 100 ? "  |  " + speedText(rules.speed) : std::string()))
                                .c_str());
        m_detail->limitLabelWidth(W - 40.f, .42f, .2f);
        if (m_livesHeader) m_livesHeader->setString(rules.mode == 0 && !rules.practice ? "LIVES" : "FAILS");
        auto const ranks = order(battle);
        float const height = m_scroll->m_contentLayer->getContentSize().height;
        for (size_t place = 0; place < ranks.size(); ++place) {
            auto const& seat = battle.seats[ranks[place]];
            auto& row = m_rows[ranks[place]];
            row.node->setPosition({4.f, height - 2.f - RowH * (place + 1)});
            row.rank->setString(fmt::format("{}", place + 1).c_str());
            ccColor3B color;
            row.status->setString(statusOf(seat, rules, color).c_str());
            row.status->setColor(color);
            float const best = seat.state.cleared ? 100.f : static_cast<float>(seat.state.bestPercent);
            row.fill->setScaleX(best / 100.f);
            row.fill->setColor(seat.state.cleared ? ccc3(110, 240, 140)
                               : place == 0       ? ccc3(255, 205, 80)
                                                  : ccc3(120, 190, 255));
            row.marker->setPositionX(BarX + BarW * seat.state.currentPercent / 100.f - 1.f);
            row.marker->setVisible(seat.state.inAttempt && !seat.state.cleared);
            row.best->setString(fmt::format("{}%", seat.state.cleared ? 100 : seat.state.bestPercent).c_str());
            row.now->setString(
                seat.state.inAttempt && !seat.state.cleared
                    ? fmt::format("now {}%  |  run {}", seat.state.currentPercent, seat.state.runNumber).c_str()
                    : fmt::format("run {}", seat.state.runNumber).c_str());
            bool const lives = rules.mode == 0 && !rules.practice;
            row.lives->setString(
                std::to_string(lives ? std::max(0, rules.attempts - seat.state.attemptsUsed) : seat.state.attemptsUsed)
                    .c_str());
            row.heart->setVisible(lives);
            bool const playingNow = battle.finishedAt == 0 && battle.releasedAt > 0 && !seat.out &&
                                    !seat.disqualified && !battle::resultFinal(seat.state, rules) &&
                                    battle::watchableRules(rules) && seat.profile.uid != Service::get().profile().uid;
            bool const watched = !seat.profile.uid.empty() && seat.profile.uid == spectated();
            row.eye->setVisible(playingNow && canWatch() && !watched);
            row.live->setVisible(watched);
        }
        auto* content = m_scroll->m_contentLayer;
        content->setPosition(content->getPosition());
    }
    // 재생 버튼 상태(맵 다운로드 진행률, 배틀이 풀린 뒤에만 활성).
    void updatePlay() {
        if (!m_play || watching() || m_inBattle) return;
        auto const& group = Service::get().group();
        bool const live = group && group->running() && group->battle->releasedAt > 0;
        m_playProgress->setString(m_loading > 0 ? fmt::format("{}%", mapDownloadProgress(m_loading).percent).c_str()
                                                : "");
        enabled(m_play, live && m_loading == 0);
    }
    // 관전 시작: 배틀 맵이 캐시에 없으면 받고, 준비되면 play로 넘어간다.
    void onPlay(CCObject* sender) {
        if (sender) {
            m_pendingWatch.clear();
            m_pendingName.clear();
        }
        auto const& group = Service::get().group();
        if (m_loading > 0 || !group || !group->running() || group->battle->releasedAt == 0) return;
        int64_t const level = group->battle->level.id;
        if (level <= 0) return;
        m_loading = level;
        updatePlay();
        ensureMapCached(level, [self = Weak<GroupSpectatePopup>(this), level,
                                battle = group->battle->id](bool ok, std::string detail) {
            auto owner = self.lock();
            if (!owner) return;
            owner->m_loading = 0;
            owner->updatePlay();
            if (!ok) {
                error(detail.empty() ? "The map could not be downloaded." : detail);
                return;
            }
            owner->play(level, battle);
        });
    }
    // 배틀이 그대로면 팝업을 닫고 freeplay로 관전용 플레이를 시작한다.
    void play(int64_t id, std::string const& battle) {
        auto const& group = Service::get().group();
        if (!group || !group->running() || group->battle->id != battle) return;
        Ref<GJGameLevel> level(cachedLevel(id));
        if (!level) {
            error("The map could not be loaded.");
            return;
        }
        std::string const battleID = battle;
        auto const watch = m_pendingWatch, name = m_pendingName;
        onClose(nullptr);
        if (!freeplay::start(level.data(), battleID, watch, name)) error("The map could not be opened.");
    }
    // 0.25초마다 표와 재생 버튼을 갱신한다. 배틀 중 관전 모드에서 관전 화면이 사라지면 닫는다.
    void update(float dt) override {
        if (m_inBattle) {
            auto* layer = PlayLayer::get();
            if (!layer || !(battle::groupSpectatorPause(layer) || freeplay::owns(layer))) {
                onClose(nullptr);
                return;
            }
        }
        m_tick += std::clamp(dt, 0.f, .25f);
        if (watching()) {
            m_watchWait -= std::clamp(dt, 0.f, .25f);
            if (m_watchWait <= 0.f && !m_watchBusy && !m_watchClosed) pollWatch();
        }
        if (m_tick < .25f) return;
        m_tick = 0.f;
        refresh();
        updatePlay();
    }
    bool init() override {
        if (!Popup::init(W, H, "GJ_square02.png")) return false;
        setTitle("SPECTATOR MODE", "goldFont.fnt", .75f);
        setID("versus-group-spectate"_spr);
        m_headline =
            label(m_mainLayer, "", {W / 2.f, H - 44.f}, .42f, W - 40.f, ccc3(255, 90, 90), false, "bigFont.fnt");
        m_detail = label(m_mainLayer, "", {W / 2.f, H - 59.f}, .42f, W - 40.f, kIce, false, "chatFont.fnt");
        label(m_mainLayer, "BEST", {12.f + BestX, H - 72.f}, .24f, 44.f, kMuted);
        label(m_mainLayer, "PROGRESS", {BarX + 12.f + BarW / 2.f, H - 72.f}, .24f, BarW, kMuted);
        m_livesHeader = label(m_mainLayer, "LIVES", {12.f + (HeartX + LivesX) / 2.f, H - 72.f}, .24f, 44.f, kMuted);
        label(m_mainLayer, "WATCH", {12.f + EyeX, H - 72.f}, .24f, 40.f, kMuted);
        auto* paper = NineSlice::create("square02b_001.png");
        paper->setContentSize({ListW, ListH + 4.f});
        paper->setPosition({W / 2.f, 14.f + ListH / 2.f});
        paper->setColor({12, 22, 44});
        paper->setOpacity(200);
        m_mainLayer->addChild(paper);
        m_scroll = ScrollLayer::create({ListW, ListH});
        m_scroll->setPosition({12.f, 14.f});
        m_mainLayer->addChild(m_scroll);
        m_play = iconButton(m_buttonMenu, this, menu_selector(GroupSpectatePopup::onPlay), "GJ_playBtn2_001.png",
                            {W - 28.f, H - 24.f}, 30.f);
        m_play->setID("group-spectate-play"_spr);
        m_playProgress =
            label(m_mainLayer, "", {W - 48.f, H - 24.f}, .32f, 50.f, ccc3(140, 255, 170), false, "bigFont.fnt");
        m_playProgress->setAnchorPoint({1.f, .5f});
        if (watching() || m_inBattle)
            m_play->setVisible(false);
        else
            Service::get().setGroupSpectating(true);
        refresh();
        updatePlay();
        scheduleUpdate();
        return true;
    }

public:
    CREATE_FUNC(GroupSpectatePopup);
    static GroupSpectatePopup* inBattle() {
        auto* popup = new GroupSpectatePopup();
        popup->m_inBattle = true;
        if (popup->init()) {
            popup->autorelease();
            return popup;
        }
        delete popup;
        return nullptr;
    }
    static GroupSpectatePopup* watch(std::string roomId, std::string name) {
        auto* popup = new GroupSpectatePopup();
        popup->m_watchRoom = std::move(roomId);
        popup->m_watchName = std::move(name);
        if (popup->init()) {
            popup->autorelease();
            return popup;
        }
        delete popup;
        return nullptr;
    }
};

// 참가자를 내보낼 때 Kick(다시 입장 가능)과 Ban(방이 열려 있는 동안 불가) 중 고르는 팝업.
class RemovePlayerPopup final : public ::versus::DeferredPopup {
    std::function<void(bool)> m_choose;
    bool init(std::string const& name, std::function<void(bool)> choose) {
        if (!Popup::init(300.f, 170.f, "GJ_square02.png")) return false;
        m_choose = std::move(choose);
        setTitle("Remove Player", "bigFont.fnt", .55f);
        setID("group-remove-player"_spr);
        label(m_mainLayer, name, {150.f, 122.f}, .5f, 260.f, ccWHITE);
        label(m_mainLayer, "Kick: they can join again.", {150.f, 96.f}, .62f, 270.f, kIce, false, "chatFont.fnt");
        label(m_mainLayer, "Ban: they can't come back while this room is open.", {150.f, 80.f}, .62f, 270.f, kIce,
              false, "chatFont.fnt");
        button(m_buttonMenu, this, menu_selector(RemovePlayerPopup::onKick), "Kick", {62.f, 36.f}, .6f,
               "GJ_button_04.png")
            ->setID("group-remove-kick"_spr);
        button(m_buttonMenu, this, menu_selector(RemovePlayerPopup::onBan), "Ban", {150.f, 36.f}, .6f,
               "GJ_button_06.png")
            ->setID("group-remove-ban"_spr);
        button(m_buttonMenu, this, menu_selector(RemovePlayerPopup::onCancel), "Cancel", {238.f, 36.f}, .6f,
               "GJ_button_01.png");
        return true;
    }
    void pick(bool ban) {
        auto choose = std::move(m_choose);
        m_choose = nullptr;
        onClose(nullptr);
        if (choose) choose(ban);
    }
    void onKick(CCObject*) { pick(false); }
    void onBan(CCObject*) { pick(true); }
    void onCancel(CCObject*) {
        m_choose = nullptr;
        onClose(nullptr);
    }

public:
    static RemovePlayerPopup* create(std::string const& name, std::function<void(bool)> choose) {
        auto* popup = new RemovePlayerPopup;
        if (popup->init(name, std::move(choose))) {
            popup->autorelease();
            return popup;
        }
        delete popup;
        return nullptr;
    }
};

// 그룹 방 본체 화면. 왼쪽에 참가자 목록(검색, 정렬, 준비 안 한 사람만 보기), 오른쪽에
// 맵 정보 또는 Map Pool, 하단에 준비/시작 버튼과 이모트 줄이 있다.
// 상태 변화는 시그니처 문자열로 비교해 바뀌었을 때만 다시 그린다.
class GroupRoomLayer : public SceneLayer {
    CCNode* m_frame = nullptr;
    CCNode* m_listPanel = nullptr;
    CCNode* m_dynamic = nullptr;
    CCNode* m_rows = nullptr;
    TapScrollLayer* m_scroll = nullptr;
    TextInput* m_search = nullptr;
    CCLabelBMFont* m_status = nullptr;
    CCLabelBMFont* m_empty = nullptr;
    CCMenuItemSpriteExtra* m_sortButton = nullptr;
    CCMenuItemSpriteExtra* m_filterButton = nullptr;
    CCSize m_rowsView;
    float m_width = 0.f, m_height = 0.f, m_listWidth = 0.f;
    std::vector<std::string> m_rowUids;
    std::vector<std::string> m_emoteSet;
    bool m_sortWins = false, m_notReadyOnly = false;
    float m_filterDelay = -1.f;
    int64_t m_mapID = 0;
    bool m_cached = false, m_downloading = false, m_readyAfterDownload = false;
    uint64_t m_downloadGeneration = 0;
    std::string m_cacheError;
    float m_progressElapsed = 0.f;
    CCLabelBMFont* m_downloadLabel = nullptr;
    std::string m_downloadKey;
    bool m_pending = false, m_sendingEmote = false, m_renderQueued = false, m_leaveAsked = false;
    float m_emoteCooldown = 0.f, m_elapsed = 1.f;
    std::string m_signature, m_rowSignature;
    std::unordered_set<std::string> m_seenEmotes;
    // 참가자 줄 위에 잠깐 띄우는 이모트/채팅 말풍선 정보와 표시 시간.
    struct RowEmote {
        std::string kind, text;
        float until = 0.f;
    };
    static constexpr float kRowEmoteSeconds = 2.8f;
    std::unordered_map<std::string, RowEmote> m_rowEmotes;
    std::vector<std::pair<float, float>> m_rowBubbleSpan;
    float m_clock = 0.f;
    AvatarCache m_rowIcons;
    std::string m_shownDraw;
    std::string m_lastHost;
    CCLabelBMFont* m_countdownLabel = nullptr;
    int m_countdownShown = -1;

    // 나갈 때 검색창 포커스를 푼다.
    void onExit() override {
        if (m_search) m_search->defocus();
        SceneLayer::onExit();
    }
    static void fit(CCMenuItemSpriteExtra* item, float width) {
        if (item->getContentSize().width <= width) return;
        auto* sprite = static_cast<ButtonSprite*>(item->getNormalImage());
        sprite->setScale(sprite->getScale() * width / item->getContentSize().width);
        item->updateSprite();
    }
    static constexpr float kPermitColumn = 24.f, kKickColumn = 28.f;
    CCMenuItemSpriteExtra* m_permitAll = nullptr;
    static constexpr float kRowY = 80.f, kRowGap = 6.f, kGearSize = 22.f;
    static constexpr float kBarBottom = 12.f, kBarHeight = 26.f, kBarY = kBarBottom + kBarHeight / 2.f;
    static constexpr float kBannerY = 50.f, kMapBottom = 94.f;
    void onSpectate(CCObject*) {
        if (auto const& room = Service::get().group(); !room || !room->running()) return;
        if (auto* popup = GroupSpectatePopup::create()) popup->show();
    }
    std::string const& self() const { return Service::get().profile().uid; }
    bool host() const { return Service::get().isHost(); }
    static bool stillPlaying(GroupRoomInfo const& room, std::string const& uid) {
        if (!room.running()) return false;
        int const seat = room.battle->seatOf(uid);
        return seat >= 0 && !battle::resultFinal(room.battle->seats[seat].state, room.battle->rules);
    }
    bool playingNow(GroupRoomInfo const& room) const { return stillPlaying(room, self()); }

    // 프레임, 참가자 목록 패널, 정렬/필터 버튼, 검색 입력, 스크롤 영역을 만든다.
    // 이미 있는 이모트는 새로 뜬 것으로 착각하지 않게 기록해 둔다.
    bool init() override {
        if (!initScene("Group Room")) return false;
        addAudioButton(false);
        setID("versus-group-room"_spr);
        if (auto const& room = Service::get().group())
            for (auto const& emote : room->emotes) m_seenEmotes.insert(emote.nonce);
        m_width = std::min(550.f, m_window.width - 44.f);
        m_height = std::min(250.f, m_window.height - 65.f);
        m_frame = panel(this, {(m_window.width - m_width) / 2.f, (m_window.height - m_height) / 2.f - 8.f},
                        {m_width, m_height});
        for (auto* id : {"side-art-bottom-left", "side-art-bottom-right"})
            if (auto* art = m_frame->getChildByID(id)) art->setVisible(false);
        m_listWidth = m_width * (m_width >= 480.f ? .54f : .58f);
        bool const debugRoom = m_listWidth - 172.f - kPermitColumn >= 30.f;
        float const listHeight = m_height - 90.f;
        m_listPanel = panel(m_frame, {10.f, 44.f}, {m_listWidth, listHeight}, false);
        auto* tools = menu(m_listPanel);
        m_sortButton = button(tools, this, menu_selector(GroupRoomLayer::onSort), "Sort: Default",
                              {44.f, listHeight - 13.f}, .34f, "GJ_button_04.png");
        fit(m_sortButton, 76.f);
        m_filterButton = button(tools, this, menu_selector(GroupRoomLayer::onFilter), "Not Ready",
                                {124.f, listHeight - 13.f}, .34f, "GJ_button_04.png");
        fit(m_filterButton, 70.f);
        float const reserve = debugRoom ? kPermitColumn : 0.f;
        float const searchWidth = std::max(30.f, m_listWidth - 172.f - reserve);
        m_search = TextInput::create(searchWidth / .62f, "Search");
        m_search->setCommonFilter(CommonFilter::Any);
        m_search->setMaxCharCount(20);
        m_search->setScale(.62f);
        m_search->setPosition({m_listWidth - 8.f - reserve - searchWidth / 2.f, listHeight - 13.f});
        if (debugRoom) {
            m_permitAll =
                CCMenuItemSpriteExtra::create(permitSprite(false), this, menu_selector(GroupRoomLayer::onPermitAll));
            m_permitAll->setID("group-pool-permit-all"_spr);
            m_permitAll->setPosition({m_listWidth - 6.f - kPermitColumn / 2.f, listHeight - 13.f});
            tools->addChild(m_permitAll);
        }
        styleInput(m_search);
        m_search->setCallback([self = Weak<GroupRoomLayer>(this)](std::string const&) {
            if (auto owner = self.lock()) owner->m_filterDelay = .12f;
        });
        m_listPanel->addChild(m_search, 6);
        if (m_listWidth - 172.f - reserve < 30.f) {
            m_search->setVisible(false);
            m_search->setEnabled(false);
        }
        m_rowsView = CCSize(m_listWidth - 12.f, listHeight - 30.f);
        m_scroll = TapScrollLayer::createAt(m_rowsView, [self = Weak<GroupRoomLayer>(this)](CCNode* row, CCPoint at) {
            if (auto owner = self.lock()) owner->tapRow(row->getTag(), at);
        });
        m_scroll->setPosition({6.f, 4.f});
        m_listPanel->addChild(m_scroll);
        m_rows = m_scroll->m_contentLayer;
        m_empty = label(m_listPanel, "", {m_listWidth / 2.f, listHeight / 2.f - 12.f}, .38f, m_listWidth - 30.f, kIce);
        m_dynamic = CCNode::create();
        m_frame->addChild(m_dynamic, 4);
        m_status =
            label(this, "", {m_window.width / 2.f, 10.f}, .52f, m_window.width - 40.f, kIce, false, "chatFont.fnt");
        styleToolbar();
        render();
        return true;
    }
    void styleToolbar() {
        auto tint = [](CCMenuItemSpriteExtra* item, bool on) {
            static_cast<ButtonSprite*>(item->getNormalImage())
                ->m_BGSprite->setColor(on ? ccc3(100, 255, 125) : ccc3(255, 255, 255));
        };
        auto* sort = static_cast<ButtonSprite*>(m_sortButton->getNormalImage());
        sort->setString(m_sortWins ? "Sort: Wins" : "Sort: Default");
        m_sortButton->updateSprite();
        fit(m_sortButton, 76.f);
        tint(m_sortButton, m_sortWins);
        tint(m_filterButton, m_notReadyOnly);
    }
    void onSort(CCObject*) {
        m_sortWins = !m_sortWins;
        styleToolbar();
        renderRows(true);
    }
    void onFilter(CCObject*) {
        m_notReadyOnly = !m_notReadyOnly;
        styleToolbar();
        renderRows(true);
    }

    // 같은 프레임에 여러 번 요청돼도 메인 스레드에서 한 번만 render하도록 예약한다.
    void requestRender() {
        if (m_renderQueued || m_transitioning) return;
        m_renderQueued = true;
        Loader::get()->queueInMainThread([self = Weak<GroupRoomLayer>(this)] {
            if (auto owner = self.lock()) {
                owner->m_renderQueued = false;
                if (!owner->m_transitioning) owner->render();
            }
        });
    }

    // 검색어와 필터를 적용한 참가자 목록. 정렬: 승수 정렬이 켜져 있으면 승수 순,
    // 같은 경우 나 -> 방장 -> 멤버십 -> 일반 순이다.
    std::vector<GroupMember const*> visibleMembers(GroupRoomInfo const& room) const {
        std::string const query = m_search ? lower(std::string(m_search->getString())) : std::string();
        std::vector<GroupMember const*> result;
        for (auto const& member : room.members) {
            if (m_notReadyOnly && member.ready) continue;
            if (!query.empty() && lower(member.profile.name).find(query) == std::string::npos) continue;
            result.push_back(&member);
        }
        auto const& uid = self();
        auto group = [&room, &uid](GroupMember const* member) {
            return member->profile.uid == uid            ? 0
                   : member->profile.uid == room.hostUid ? 1
                   : member->profile.membership          ? 2
                                                         : 3;
        };
        std::stable_sort(result.begin(), result.end(), [&](GroupMember const* a, GroupMember const* b) {
            if (m_sortWins && a->wins != b->wins) return a->wins > b->wins;
            return group(a) < group(b);
        });
        return result;
    }
    // 참가자 목록을 다시 그려야 하는지 비교하는 시그니처.
    std::string rowSignature(GroupRoomInfo const& room) const {
        std::string key = fmt::format("{}:{}:{}:{}:{}:{}", room.id, room.hostUid, m_sortWins, m_notReadyOnly,
                                      m_search ? std::string(m_search->getString()) : std::string(),
                                      room.running() ? room.battle->id : "");
        if (room.mapPool) {
            key += "|pool";
            for (auto const& uid : room.mapPool->permit) key += "+" + uid;
        }
        for (auto const& member : room.members) {
            auto const& p = member.profile;
            key += fmt::format("|{}:{}:{}:{}:{}:{}:{}:{}:{}:{}:{}", p.uid, p.name, p.icon, p.color1, p.color2,
                               p.membership, p.cosmetics.title, p.cosmetics.font, p.cosmetics.start, member.ready,
                               member.wins) +
                   fmt::format(":{}", room.level.id > 0 && !room.running() ? member.download : -2);
        }
        return key;
    }
    // 참가자 줄을 다시 그린다(스크롤 위치는 유지). 각 줄: 아이콘, 이름/칭호, 준비 또는 PLAYING 상태,
    // 맵 다운로드 진행도, 승수, Map Pool 허용 표시, 방장에게는 강퇴 아이콘.
    void renderRows(bool force = false) {
        auto const& current = Service::get().group();
        if (!current) return;
        auto const& room = *current;
        auto signature = rowSignature(room);
        if (!force && signature == m_rowSignature) return;
        m_rowSignature = std::move(signature);
        float const scrollFromTop = m_rows->getContentSize().height + m_rows->getPositionY();
        bool const keepScroll = !force && m_rows->getContentSize().height > 0.f && !m_rowUids.empty();
        m_rowIcons.detach();
        m_rows->removeAllChildrenWithCleanup(true);
        m_rowUids.clear();
        m_rowBubbleSpan.clear();
        std::erase_if(m_rowEmotes, [this](auto const& entry) { return entry.second.until <= m_clock; });
        auto const members = visibleMembers(room);
        m_empty->setString(members.empty() ? (m_notReadyOnly ? "Everyone is ready" : "No matching players") : "");
        float const rowStep = 27.f, rowHeight = 25.f;
        float const width = m_rowsView.width;
        float const listHeight = std::max(m_rowsView.height, rowStep * members.size() + 2.f);
        m_rows->setContentSize({width, listHeight});
        bool const canKick = host();
        bool const pool = room.mapPool.has_value();
        float const kickRight = canKick ? kKickColumn : 0.f, permitRight = pool ? kPermitColumn : 0.f;
        float const permitX = width - kPermitColumn / 2.f;
        float const kickX = width - permitRight - kKickColumn / 2.f;
        float const winX = width - kickRight - permitRight - 24.f;
        float const statusWidth = std::clamp((winX - 50.f) * .45f, 40.f, 62.f);
        float const statusX = winX - 22.f - statusWidth / 2.f;
        for (size_t i = 0; i < members.size(); ++i) {
            auto const& member = *members[i];
            auto const& profile = member.profile;
            bool const mine = profile.uid == self();
            bool const isHost = profile.uid == room.hostUid;
            bool const playing = stillPlaying(room, profile.uid);
            auto* row = CCSprite::create();
            row->setContentSize({width, rowHeight});
            auto* bg = NineSlice::create("square02b_001.png");
            bg->setContentSize({width, rowHeight});
            bg->setColor(mine     ? ccc3(66, 120, 30)
                         : isHost ? ccc3(18, 58, 112)
                         : i % 2  ? ccc3(10, 40, 88)
                                  : ccc3(8, 32, 74));
            bg->setOpacity(235);
            bg->setPosition({width / 2.f, rowHeight / 2.f});
            row->addChild(bg, -1);
            auto iconKey = avatarKey(profile);
            CCNode* icon = m_rowIcons.take(profile.uid, iconKey);
            if (icon) {
                icon->setPosition({20.f, rowHeight / 2.f});
                row->addChild(icon);
            } else
                icon = player(row, profile, {20.f, rowHeight / 2.f}, .46f);
            m_rowIcons.put(profile.uid, std::move(iconKey), icon);
            if (isHost) {
                if (auto* badge = loadedFrameSprite("modBadge_03_001.png", "GJ_GameSheet03.plist")) {
                    badge->setScale(11.f / std::max(1.f, badge->getContentSize().height));
                    badge->setPosition({7.f, rowHeight - 5.5f});
                    badge->setID("group-row-host"_spr);
                    row->addChild(badge, 4);
                }
            } else if (canKick) {
                row->addChild(handOverMark({8.5f, rowHeight - 5.5f}), 4);
            }
            constexpr float nameX = 34.f;
            float const nameWidth = std::max(30.f, statusX - statusWidth / 2.f - 4.f - nameX);
            auto* name = label(row, profile.name, {nameX, rowHeight * .68f}, .3f, nameWidth, ccWHITE, true);
            styleNickname(name, profile.membership, profile.cosmetics);
            auto* plate = titleBadge(row, profile.cosmetics.title, {0.f, rowHeight * .27f}, .62f, nameWidth, false);
            plate->setPositionX(nameX + plate->getScaledContentSize().width / 2.f);
            plate->setID("group-row-title"_spr);
            float const nameRight =
                std::max(name->getContentSize().width * name->getScaleX(), plate->getScaledContentSize().width);
            m_rowBubbleSpan.emplace_back(nameX + std::min(nameRight, nameWidth) + 9.f, winX - 26.f);
            bool const showDownload = room.level.id > 0 && !room.running() && !playing;
            float const statusY = showDownload ? rowHeight * .64f : rowHeight / 2.f;
            if (playing)
                label(row, "PLAYING", {statusX, statusY}, .26f, statusWidth, ccc3(255, 196, 110));
            else
                label(row, member.ready ? "READY" : "NOT READY", {statusX, statusY}, .26f, statusWidth,
                      member.ready ? ccc3(150, 255, 175) : ccc3(255, 150, 165));
            if (showDownload) {
                bool const done = member.download >= 100;
                label(row,
                      member.download < 0 ? std::string("MAP --")
                      : done              ? std::string("MAP READY")
                                          : fmt::format("MAP {}%", member.download),
                      {statusX, rowHeight * .26f}, .2f, statusWidth,
                      done                  ? ccc3(135, 255, 175)
                      : member.download < 0 ? kMuted
                                            : kIce);
            }
            label(row, fmt::format("WIN {}", member.wins), {winX, rowHeight / 2.f}, .34f, 40.f, ccWHITE, false,
                  "goldFont.fnt");
            if (pool) {
                bool const allowed = room.mapPool->permitted(profile.uid, room.hostUid);
                auto* box = permitSprite(allowed);
                box->setPosition({permitX, rowHeight / 2.f});
                row->addChild(box, 2);
            }
            if (canKick && !mine) {
                if (auto* remove = loadedFrameSprite("GJ_deleteIcon_001.png")) {
                    remove->setScale(15.f / std::max(1.f, remove->getContentSize().height));
                    remove->setPosition({kickX, rowHeight / 2.f});
                    if (m_pending) remove->setOpacity(110);
                    row->addChild(remove, 2);
                }
            }
            row->setAnchorPoint({.5f, .5f});
            row->setPosition({width / 2.f, listHeight - 1.f - rowStep / 2.f - i * rowStep});
            row->setTag(static_cast<int>(m_rowUids.size()));
            m_rowUids.push_back(profile.uid);
            m_rows->addChild(row);
            attachRowEmote(row, m_rowUids.size() - 1);
        }
        m_rowIcons.keep();
        if (keepScroll) {
            float const lowest = m_rowsView.height - listHeight;
            m_rows->setPositionY(std::clamp(scrollFromTop - listHeight, lowest, 0.f));
        } else
            m_scroll->scrollToTop();
    }
    // 줄 안을 눌렀을 때 위치에 따라 분기한다: 풀 허용 토글, 강퇴, 방장 넘기기, 아니면 프로필 팝업.
    void tapRow(int tag, CCPoint at) {
        auto const& room = Service::get().group();
        if (!room || m_transitioning || tag < 0 || tag >= static_cast<int>(m_rowUids.size())) return;
        auto const uid = m_rowUids[static_cast<size_t>(tag)];
        auto const* member = room->member(uid);
        if (!member) return;
        float const width = m_rowsView.width;
        float const permitRight = room->mapPool ? kPermitColumn : 0.f;
        if (room->mapPool && at.x >= width - kPermitColumn) {
            if (host() && uid != room->hostUid) permit(uid, !room->mapPool->permitted(uid, room->hostUid));
            return;
        }
        if (host() && uid != self() && at.x >= width - permitRight - kKickColumn && at.x < width - permitRight) {
            askRemove(member->profile);
            return;
        }
        if (host() && uid != self() && at.x < 18.f && at.y > 12.5f) {
            askHandOver(member->profile);
            return;
        }
        if (auto* popup = PlayerPopup::create(member->profile)) popup->show();
    }
    static CCSprite* permitSprite(bool allowed) { return mappool::permitSprite(allowed); }
    void permit(std::string const& uid, bool allowed) {
        if (m_pending) return;
        m_pending = true;
        requestRender();
        Service::get().permitMapPool(uid, allowed, [self = Weak<GroupRoomLayer>(this)](bool ok, std::string detail) {
            if (auto owner = self.lock()) {
                owner->m_pending = false;
                owner->requestRender();
            }
            if (!ok) error(detail);
        });
    }
    // 방장이 모든 참가자의 Map Pool 허용을 한꺼번에 켜거나 끈다.
    void onPermitAll(CCObject*) {
        auto const& room = Service::get().group();
        if (m_pending || !room || !room->mapPool || !host()) return;
        bool everyone = true;
        for (auto const& member : room->members)
            if (member.profile.uid != room->hostUid && !room->mapPool->permitted(member.profile.uid, room->hostUid))
                everyone = false;
        permit({}, !everyone);
    }
    void styleRowsToolbar(GroupRoomInfo const& room) {
        if (!m_permitAll) return;
        m_permitAll->setVisible(room.mapPool.has_value());
        m_permitAll->setPositionX(m_listWidth - 6.f - kPermitColumn / 2.f);
        bool everyone = room.mapPool.has_value();
        if (room.mapPool)
            for (auto const& member : room.members)
                if (member.profile.uid != room.hostUid && !room.mapPool->permitted(member.profile.uid, room.hostUid))
                    everyone = false;
        m_permitAll->setSprite(permitSprite(everyone));
        m_permitAll->setEnabled(host() && !m_pending);
    }
    // 강퇴/차단 선택 팝업을 거쳐 서버에 요청한다.
    void askRemove(PlayerProfile const& profile) {
        if (m_pending) return;
        auto const uid = profile.uid;
        auto* popup = RemovePlayerPopup::create(profile.name, [self = Weak<GroupRoomLayer>(this), uid](bool ban) {
            auto owner = self.lock();
            if (!owner || owner->m_pending || owner->m_transitioning) return;
            owner->m_pending = true;
            owner->requestRender();
            Service::get().kickGroupMember(uid, ban, [self](bool ok, std::string detail) {
                if (auto owner = self.lock()) {
                    owner->m_pending = false;
                    owner->requestRender();
                    if (!ok) error(detail);
                }
            });
        });
        if (popup) popup->show();
    }
    // 방장 넘기기 표시(방장 배지 + 화살표).
    static CCNode* handOverMark(CCPoint at) {
        auto* mark = CCNode::create();
        mark->setContentSize({17.f, 10.f});
        mark->setAnchorPoint({.5f, .5f});
        mark->setPosition(at);
        mark->setID("group-row-hand-over"_spr);
        if (auto* badge = loadedFrameSprite("modBadge_03_001.png", "GJ_GameSheet03.plist")) {
            badge->setScale(9.f / std::max(1.f, badge->getContentSize().height));
            badge->setPosition({5.5f, 5.f});
            badge->setOpacity(200);
            mark->addChild(badge);
        }
        if (auto* arrow = loadedFrameSprite("backArrowPlain_01_001.png", "GJ_GameSheet03.plist")) {
            arrow->setScale(8.f / std::max(1.f, arrow->getContentSize().height));
            arrow->setRotation(180.f);
            arrow->setPosition({13.5f, 5.f});
            mark->addChild(arrow);
        }
        return mark;
    }
    // 확인 후 해당 참가자에게 방장을 넘긴다.
    void askHandOver(PlayerProfile const& profile) {
        if (m_pending) return;
        auto const uid = profile.uid;
        createQuickPopup("Hand Over Room",
                         fmt::format("Make <cy>{}</c> the host?\nYou will be a player in the room.", profile.name),
                         "Cancel", "Make Host", [self = Weak<GroupRoomLayer>(this), uid](auto*, bool yes) {
                             auto owner = self.lock();
                             if (!yes || !owner || owner->m_pending || owner->m_transitioning) return;
                             owner->m_pending = true;
                             owner->requestRender();
                             Service::get().handOverGroup(uid, [self](bool ok, std::string detail) {
                                 if (auto owner = self.lock()) {
                                     owner->m_pending = false;
                                     owner->requestRender();
                                     if (!ok) error(detail);
                                 }
                             });
                         });
    }

    // 방의 맵을 다운로드한다. generation으로 맵이 바뀐 뒤 도착한 오래된 응답을 무시한다.
    void beginDownload() {
        auto const& room = Service::get().group();
        if (!room || room->level.id <= 0 || m_downloading || mapCached(room->level.id)) {
            if (room && room->level.id > 0) m_cached = mapCached(room->level.id);
            return;
        }
        m_downloading = true;
        m_cacheError.clear();
        auto const levelID = room->level.id;
        auto const generation = ++m_downloadGeneration;
        ensureMapCached(levelID,
                        [self = Weak<GroupRoomLayer>(this), levelID, generation](bool success, std::string detail) {
                            auto owner = self.lock();
                            if (!owner || owner->m_mapID != levelID || owner->m_downloadGeneration != generation)
                                return;
                            owner->m_downloading = false;
                            owner->m_cached = success && mapCached(levelID);
                            if (owner->m_cached) preloadBattleAssets();
                            owner->m_cacheError = owner->m_cached ? std::string() : std::move(detail);
                            bool const ready = std::exchange(owner->m_readyAfterDownload, false);
                            auto const& room = Service::get().group();
                            auto const* mine = room ? room->member(owner->self()) : nullptr;
                            if (ready && owner->m_cached && mapAudioSkipped(levelID) && mine && !mine->ready)
                                owner->onReady(nullptr);
                            owner->requestRender();
                        });
    }
    // 다운로드 진행률 문구를 갱신한다.
    void updateDownload() {
        if (!m_downloadLabel || m_mapID <= 0) return;
        auto const progress = mapDownloadProgress(m_mapID);
        auto text = progress.complete() ? std::string("Map ready") : fmt::format("Download {}%", progress.percent);
        if (text == m_downloadKey) return;
        m_downloadKey = text;
        m_downloadLabel->setString(text.c_str());
        m_downloadLabel->setColor(progress.complete() ? ccc3(135, 255, 175) : kIce);
    }

    // 방 화면 전체를 다시 그려야 하는지 비교하는 시그니처.
    std::string signature(GroupRoomInfo const& room) const {
        auto battle = room.battle ? fmt::format("{}:{}:{}:{}", room.battle->id, room.battle->releasedAt,
                                                room.battle->finishedAt, room.battle->seats.size())
                                  : std::string();
        int ready = 0;
        for (auto const& member : room.members) ready += member.ready ? 1 : 0;
        auto const* mine = room.member(self());
        return fmt::format("{}:{}:{}:{}:{}:{}:{}:{}:{}:{}:{}:{}:{}:{}:{}", room.id, room.name, room.hostUid,
                           room.maxPlayers, room.members.size(), ready, mine && mine->ready, room.level.id,
                           room.level.name, room.rules.attempts, room.rules.allowHacks, battle, m_pending, m_cached,
                           m_downloading) +
               fmt::format(":{}", room.rules.speed) +
               fmt::format(":{}:{}:{}", m_cacheError, Service::get().busy(), room.privateRoom) +
               fmt::format(":{}:{}:{}:{}", room.mapSelection.random, room.mapSelection.mask,
                           room.mapDraw ? room.mapDraw->id : std::string(), room.mapDraw && room.mapDraw->settled) +
               fmt::format(":{}:{}:{}:{}", room.mapSelection.lengths, room.mapSelection.rated,
                           room.mapSelection.twoPlayer, room.mapSelection.minLikes) +
               fmt::format(":{}", room.startAt) +
               fmt::format(":{}:{}:{}", room.rules.mode, room.rules.practice, room.rules.targetPercent) +
               (room.mapPool ? fmt::format(":pool{}", room.mapPool->maps.size()) : std::string());
    }
    // 시작 카운트다운 남은 초(올림). 숫자가 바뀔 때 시계 소리를 낸다.
    static int countdownLeft(GroupRoomInfo const& room) {
        auto const left = room.startAt - Service::get().serverNow();
        return left > 0 ? static_cast<int>((left + 999) / 1000) : 0;
    }
    static std::string countdownText(int left) {
        return left > 0 ? fmt::format("STARTING IN {}", left) : std::string("STARTING...");
    }
    void updateCountdown() {
        auto const& room = Service::get().group();
        if (!m_countdownLabel || !room || room->startAt <= 0) {
            m_countdownShown = -1;
            return;
        }
        int const left = countdownLeft(*room);
        if (left == m_countdownShown) return;
        if (left > 0) audio::play(audio::Cue::Clock);
        m_countdownShown = left;
        m_countdownLabel->setString(countdownText(left).c_str());
    }
    // 오른쪽 영역과 하단 막대를 전부 다시 만든다: 방 이름과 인원, 규칙 칩, 맵 정보 또는 Map Pool,
    // Map Mode / Re-Roll / 규칙 버튼, 배틀 진행 배너 또는 카운트다운 배너, 다운로드 표시,
    // LDM 토글, Start / Cancel, Ready 버튼, 준비 인원, 이모트와 채팅 버튼.
    void render() {
        auto const& current = Service::get().group();
        if (!current) return;
        auto room = *current;
        hideSpinningDraw(room, Service::get().serverNow());
        if (m_mapID != room.level.id) {
            ++m_downloadGeneration;
            m_readyAfterDownload = false;
            m_mapID = room.level.id;
            m_cached = room.level.id > 0 && mapCached(room.level.id);
            m_downloading = false;
            m_cacheError.clear();
            if (room.level.id > 0 && !m_cached)
                beginDownload();
            else if (m_cached)
                preloadBattleAssets();
        } else if (room.level.id > 0)
            m_cached = mapCached(room.level.id);
        m_signature = signature(room);
        m_dynamic->removeAllChildrenWithCleanup(true);
        m_downloadLabel = nullptr;
        m_downloadKey.clear();
        m_countdownLabel = nullptr;
        bool const owner = host();
        auto const* mine = room.member(self());
        bool const myReady = mine && mine->ready;
        int readyCount = 0;
        for (auto const& member : room.members) readyCount += member.ready ? 1 : 0;
        bool const running = room.running();
        bool const seated = playingNow(room);
        int const mySeat = running ? room.battle->seatOf(self()) : -1;
        styleNickname(label(m_dynamic, room.name, {m_width / 2.f, m_height - 17.f}, .46f, m_width - 180.f, kIce), false,
                      {});
        if (room.privateRoom) {
            auto* lock = CCSprite::createWithSpriteFrameName("GJ_lockGray_001.png");
            lock->setScale(.38f);
            lock->setPosition({18.f, m_height - 18.f});
            m_dynamic->addChild(lock);
        }
        auto* count = NineSlice::create("square02b_001.png");
        count->setContentSize({64.f, 16.f});
        count->setColor(ccc3(58, 28, 104));
        count->setOpacity(220);
        count->setPosition({m_width - 46.f, m_height - 17.f});
        m_dynamic->addChild(count);
        label(m_dynamic, fmt::format("{} / {}", room.members.size(), room.maxPlayers),
              {m_width - 46.f, m_height - 17.f}, .32f, 58.f, ccc3(215, 175, 255));
        label(m_dynamic, "GROUP", {m_width - 46.f, m_height - 31.f}, .2f, 58.f, ccc3(205, 150, 255));
        ruleChips(m_dynamic, room.rules, {m_width / 2.f, m_height - 33.f}, m_width - 180.f);
        float const sideX = m_listWidth + 18.f;
        float const sideWidth = m_width - sideX - 10.f;
        float const centerX = sideX + sideWidth / 2.f;
        float const top = m_height - 46.f;
        auto* side = panel(m_dynamic, {sideX, kBarBottom}, {sideWidth, top - kBarBottom}, false);
        side->setZOrder(-1);
        auto* actions = menu(m_dynamic);
        bool const counting = room.startAt > 0 && !running;
        bool const downloadShown = room.level.id > 0 && !m_cached && !running;
        bool const poolShown = room.mapPool && !running;
        styleRowsToolbar(room);
        if (poolShown) {
            float const bottom = counting || downloadShown ? kBannerY + 16.f : kBarBottom + kBarHeight + 4.f;
            auto* list = MapPoolList::create(
                {sideWidth - 8.f, top - 4.f - bottom},
                [self = Weak<GroupRoomLayer>(this)] {
                    if (auto owner = self.lock()) owner->onChoose(nullptr);
                },
                owner ? std::function<void()>([self = Weak<GroupRoomLayer>(this)] {
                    if (auto layer = self.lock()) layer->onRules(nullptr);
                })
                      : std::function<void()>());
            list->setPosition({sideX + 4.f, bottom});
            m_dynamic->addChild(list, 3);
        } else if (room.level.id > 0) {
            if (auto* face = mapDifficulty(room.level)) {
                face->setScale(sideWidth < 160.f ? .75f : .9f);
                face->setPosition({centerX, top - 30.f});
                m_dynamic->addChild(face);
            }
            label(m_dynamic, room.level.name, {centerX, top - 61.f}, .42f, sideWidth - 12.f);
            label(m_dynamic, room.level.creator.empty() ? "Unknown creator" : room.level.creator, {centerX, top - 76.f},
                  .36f, sideWidth - 12.f, ccWHITE, false, "goldFont.fnt");
            starRating(m_dynamic, std::to_string(room.level.stars),
                       {centerX - (room.level.userCoins > 0 ? 20.f : 0.f), top - 92.f}, .34f, sideWidth * .5f);
            mapCoinCount(m_dynamic, room.level, {centerX + 20.f, top - 92.f});
        } else if (room.mapSelection.random) {
            label(m_dynamic, groupDrawing(room) ? "ROLLING A RANDOM MAP..." : "RANDOM MAP", {centerX, top - 9.f}, .4f,
                  sideWidth - 14.f, ccc3(255, 214, 120), false, "goldFont.fnt");
            randomFilterCard(m_dynamic, room.mapSelection,
                             CCRect(sideX + 6.f, kMapBottom, sideWidth - 12.f, top - 20.f - kMapBottom), true);
        } else
            label(m_dynamic, owner ? "Choose a map" : "Waiting for the host\nto choose a map",
                  {centerX, (top + kMapBottom) / 2.f}, .36f, sideWidth - 14.f, kIce);
        {
            std::vector<CCMenuItemSpriteExtra*> row;
            std::vector<std::pair<CCMenuItemSpriteExtra*, char const*>> gears;
            if (owner && !running && !poolShown) {
                auto* choose = button(actions, this, menu_selector(GroupRoomLayer::onChoose), "Map Mode", {}, .42f,
                                      "GJ_button_04.png");
                choose->setID("group-map-mode-button"_spr);
                enabled(choose, !m_pending && !Service::get().busy() && !groupDrawing(room));
                row.push_back(choose);
                if (groupCanReroll(room)) {
                    auto* again = button(actions, this, menu_selector(GroupRoomLayer::onReroll), "Re-Roll", {}, .38f,
                                         "GJ_button_04.png");
                    again->setID("group-reroll-button"_spr);
                    enabled(again, !m_pending && !Service::get().busy());
                    row.push_back(again);
                }
                auto* gear = iconButton(actions, this, menu_selector(GroupRoomLayer::onRules), "GJ_optionsBtn_001.png",
                                        {}, kGearSize);
                gear->setID("group-rules-button"_spr);
                enabled(gear, !m_pending && !groupDrawing(room));
                row.push_back(gear);
                gears.push_back({gear, "Game"});
            }
            if (!row.empty()) {
                float total = kRowGap * static_cast<float>(row.size() - 1);
                for (auto* item : row) total += item->getContentSize().width;
                float const scale = std::min(1.f, (sideWidth - 10.f) / std::max(1.f, total));
                float x = centerX - total * scale / 2.f;
                for (auto* item : row) {
                    if (scale < 1.f) {
                        auto* sprite = item->getNormalImage();
                        sprite->setScale(sprite->getScale() * scale);
                        item->updateSprite();
                    }
                    float const half = item->getContentSize().width / 2.f;
                    item->setPosition({x + half, kRowY});
                    x += 2.f * half + kRowGap * scale;
                }
            }
            for (auto [item, caption] : gears)
                label(m_dynamic, caption, {item->getPositionX(), kRowY - kGearSize / 2.f - 4.f}, .2f, 28.f, kIce)
                    ->setZOrder(2);
        }
        int const needed = groupReadyNeeded(static_cast<int>(room.members.size()));
        if (running) {
            int alive = 0;
            for (auto const& seat : room.battle->seats) alive += seat.state.forfeited ? 0 : 1;
            auto* banner = NineSlice::create("square02b_001.png");
            banner->setContentSize({sideWidth - 10.f, 24.f});
            banner->setColor(ccc3(90, 52, 16));
            banner->setOpacity(230);
            banner->setPosition({centerX, kBannerY});
            m_dynamic->addChild(banner);
            label(m_dynamic, room.battle->releasedAt ? "BATTLE IN PROGRESS" : "BATTLE LOADING",
                  {centerX, kBannerY + 7.f}, .28f, sideWidth - 18.f, ccc3(255, 205, 120));
            label(m_dynamic,
                  mySeat < 0 ? fmt::format("{} playing - ready up for the next round", alive)
                  : room.battle->seats[mySeat].state.forfeited
                      ? std::string("You left this battle - ready up for the next round")
                      : std::string("Your result is locked - waiting for the others"),
                  {centerX, kBannerY - 5.f}, .4f, sideWidth - 18.f, ccWHITE, false, "chatFont.fnt");
            if (room.battle->releasedAt > 0 && (mySeat < 0 || room.battle->seats[mySeat].state.forfeited)) {
                auto* watch = button(actions, this, menu_selector(GroupRoomLayer::onSpectate), "Spectate",
                                     {centerX, kRowY}, .45f, "GJ_button_02.png");
                watch->setID("group-spectate-button"_spr);
                fit(watch, sideWidth - 14.f);
            }
        } else if (counting) {
            auto* banner = NineSlice::create("square02b_001.png");
            banner->setContentSize({sideWidth - 10.f, 24.f});
            banner->setColor(ccc3(22, 82, 48));
            banner->setOpacity(230);
            banner->setPosition({centerX, kBannerY});
            m_dynamic->addChild(banner);
            m_countdownLabel = label(m_dynamic, countdownText(countdownLeft(room)), {centerX, kBannerY + 7.f}, .3f,
                                     sideWidth - 18.f, ccc3(255, 225, 120), false, "goldFont.fnt");
            if (m_cached || room.level.id <= 0)
                label(m_dynamic, myReady ? "You will play" : "Get ready to play", {centerX, kBannerY - 5.f}, .4f,
                      sideWidth - 18.f, myReady ? ccc3(150, 255, 175) : ccWHITE, false, "chatFont.fnt");
        }
        if (room.level.id > 0 && !m_cached && !running) {
            m_downloadLabel =
                label(m_dynamic, "", {centerX, counting ? kBannerY - 5.f : kBannerY}, .24f, sideWidth - 12.f, kIce);
            updateDownload();
        }
        bool const roll = groupNeedsRoll(room);
        auto const control = readyControl(seated || groupDrawing(room), m_pending, owner, room.level.id > 0 || roll,
                                          m_cached || roll, m_downloading, myReady, mapAudioSkipped(room.level.id));
        float barLeft = sideX + 6.f, barRight = sideX + sideWidth - 6.f;
        {
            auto* ldm = CCMenuItemToggler::createWithStandardSprites(this, menu_selector(GroupRoomLayer::onLdm), .4f);
            ldm->setID("group-ldm-toggle"_spr);
            ldm->toggle(Mod::get()->getSavedValue<bool>("room-ldm", false));
            float const boxW = ldm->getContentSize().width * .4f;
            ldm->setPosition({barLeft + boxW / 2.f, kBarY});
            ldm->setEnabled(!seated);
            actions->addChild(ldm);
            barLeft += boxW + 6.f;
            if (sideWidth >= 170.f) {
                auto* caption = label(m_dynamic, "LDM", {barLeft - 3.f, kBarY}, .28f, 30.f, kIce, true);
                barLeft += caption->getScaledContentSize().width + 3.f;
            }
        }
        if (owner && counting) {
            auto* stop = button(actions, this, menu_selector(GroupRoomLayer::onStopStart), "Cancel", {}, .5f,
                                "GJ_button_06.png");
            stop->setID("group-stop-start-button"_spr);
            fit(stop, sideWidth * .36f);
            stop->setPosition({barRight - stop->getContentSize().width / 2.f, kBarY});
            barRight -= stop->getContentSize().width + 6.f;
            enabled(stop, !m_pending);
        } else if (owner) {
            auto* start = button(actions, this, menu_selector(GroupRoomLayer::onStart),
                                 groupDrawing(room) ? "Rolling"
                                 : roll             ? "Roll"
                                                    : "Start",
                                 {}, .5f);
            start->setID("group-start-button"_spr);
            fit(start, sideWidth * .36f);
            start->setPosition({barRight - start->getContentSize().width / 2.f, kBarY});
            barRight -= start->getContentSize().width + 6.f;
            enabled(start, !m_pending && !Service::get().busy() && !running && !groupDrawing(room) &&
                               ((room.level.id > 0 && m_cached) || roll) && myReady &&
                               readyCount >= (roll ? kGroupMinPlayers : needed));
        }
        if (!(poolShown && control.action == ReadyAction::ChooseMap)) {
            auto* ready = button(actions, this, menu_selector(GroupRoomLayer::onReady), control.caption, {}, .48f,
                                 myReady ? "GJ_button_04.png" : "GJ_button_01.png");
            ready->setID("group-ready-button"_spr);
            fit(ready, std::max(30.f, barRight - barLeft));
            ready->setPosition({(barLeft + barRight) / 2.f, kBarY});
            enabled(ready, control.enabled());
        }
        float const boxWidth = std::clamp(m_listWidth - 4.f - 7.f * 25.8f, 64.f, 120.f);
        {
            auto* box = NineSlice::create("square02b_001.png");
            box->setID("group-ready-count"_spr);
            box->setContentSize({boxWidth, kBarHeight});
            box->setColor(ccc3(0, 0, 0));
            box->setOpacity(120);
            box->setPosition({10.f + boxWidth / 2.f, kBarY});
            m_dynamic->addChild(box);
            bool const enough = readyCount >= needed || readyCount == static_cast<int>(room.members.size());
            auto const text = fmt::format("READY {} / {}", readyCount, room.members.size());
            if (enough || running)
                label(m_dynamic, text, {10.f + boxWidth / 2.f, kBarY}, .36f, boxWidth - 12.f,
                      enough ? ccc3(150, 255, 175) : kIce);
            else {
                label(m_dynamic, text, {10.f + boxWidth / 2.f, kBarY + 5.f}, .32f, boxWidth - 12.f, kIce);
                label(m_dynamic, fmt::format("{} to start", needed), {10.f + boxWidth / 2.f, kBarY - 7.f}, .42f,
                      boxWidth - 12.f, kMuted, false, "chatFont.fnt");
            }
        }
        {
            m_emoteSet = emotes::quickSet(Service::get().profile().membership);
            int const emoteCount = static_cast<int>(m_emoteSet.size());
            float const areaX = 10.f + boxWidth + 4.f, areaWidth = m_listWidth - boxWidth - 4.f;
            float const pitch = areaWidth / static_cast<float>(emoteCount + 2);
            float const tile = std::min(23.f, pitch - 3.f);
            auto at = [&](int index) { return CCPoint{areaX + pitch * (index + .5f), kBarY}; };
            for (int i = 0; i < emoteCount; ++i) {
                auto* item = CCMenuItemSpriteExtra::create(emoteTile(m_emoteSet[i], tile, ccc3(5, 27, 73)), this,
                                                           menu_selector(GroupRoomLayer::onEmote));
                item->setTag(i);
                item->setPosition(at(i));
                actions->addChild(item);
            }
            auto* sprite = CCSprite::create();
            sprite->setContentSize({tile, tile});
            auto* bg = NineSlice::create("square02b_001.png");
            bg->setContentSize({tile, tile});
            bg->setColor(ccc3(70, 30, 105));
            bg->setPosition({tile / 2.f, tile / 2.f});
            sprite->addChild(bg);
            auto* dots = CCDrawNode::create();
            float const step = tile * 10.f / 27.f;
            for (int i = 0; i < 4; ++i)
                dots->drawDot({tile / 2.f + ((i % 2) - .5f) * step, tile / 2.f + ((i / 2) - .5f) * step},
                              3.2f * tile / 27.f, {1.f, .85f, .4f, 1.f});
            sprite->addChild(dots);
            auto* more = CCMenuItemSpriteExtra::create(sprite, this, menu_selector(GroupRoomLayer::onEmotePicker));
            more->setPosition(at(emoteCount));
            actions->addChild(more);
            iconButton(actions, this, menu_selector(GroupRoomLayer::onChat), "GJ_chatBtn_001.png", at(emoteCount + 1),
                       tile + 2.f);
        }
        std::string const status = !m_cacheError.empty() ? m_cacheError + " Tap Download to retry."
                                   : room.battle && room.battle->releasedAt == 0 && running ? "The battle is loading..."
                                                                                            : "";
        m_status->setString(status.c_str());
        renderRows();
    }
    // 방 상태의 새 이모트를 한 번씩만(nonce 기준) 보여 주고, 너무 오래된 것은 건너뛴다.
    void showRoomEmotes(GroupRoomInfo const& room) {
        auto const now = Service::get().serverNow();
        for (auto const& emote : room.emotes) {
            if (emote.nonce.empty() || !m_seenEmotes.insert(emote.nonce).second) continue;
            if (now - emote.at > emotes::kFreshMs) continue;
            showRoomEmote(room, emote);
        }
        if (m_seenEmotes.size() > 256) {
            m_seenEmotes.clear();
            for (auto const& emote : room.emotes) m_seenEmotes.insert(emote.nonce);
        }
    }
    void showRoomEmote(GroupRoomInfo const& room, EmoteInfo const& emote) {
        if (!room.member(emote.uid)) return;
        m_rowEmotes[emote.uid] = RowEmote{emote.kind, emote.text, m_clock + kRowEmoteSeconds};
        auto const at = std::find(m_rowUids.begin(), m_rowUids.end(), emote.uid);
        if (at == m_rowUids.end()) return;
        auto const index = static_cast<size_t>(at - m_rowUids.begin());
        if (auto* row = m_rows->getChildByTag(static_cast<int>(index))) attachRowEmote(row, index);
    }
    // 참가자 줄에 이모트/채팅 말풍선을 붙인다. 이름과 겹치지 않는 영역 안에서 폭을 정하고,
    // 새 말풍선은 커지며 나타났다가 남은 시간 뒤 사라진다.
    void attachRowEmote(CCNode* row, size_t index) {
        if (index >= m_rowUids.size() || index >= m_rowBubbleSpan.size()) return;
        auto const found = m_rowEmotes.find(m_rowUids[index]);
        if (found == m_rowEmotes.end()) return;
        float const left = found->second.until - m_clock;
        if (left <= .2f) {
            m_rowEmotes.erase(found);
            return;
        }
        if (auto* old = row->getChildByID("versus-row-emote")) old->removeFromParent();
        auto const& emote = found->second;
        bool const chat = emote.kind == "chat";
        float const rowHeight = row->getContentSize().height, height = rowHeight - 4.f;
        auto [start, limit] = m_rowBubbleSpan[index];
        auto* bubble = CCNode::create();
        bubble->setID("versus-row-emote");
        bubble->setAnchorPoint({0.f, .5f});
        row->addChild(bubble, 10);
        CCNode* content = nullptr;
        float width = height + 6.f;
        if (chat) {
            auto* text = label(bubble, emote.text, {0.f, 0.f}, .3f, 0.f, ccc3(22, 35, 55));
            text->setZOrder(2);
            width = text->getContentSize().width * .3f + 12.f;
            content = text;
        } else {
            auto* art = emoteArt(emote.kind);
            art->setScale((height - 3.f) / 34.f);
            bubble->addChild(art, 2);
            content = art;
        }
        if (start + width > limit) start = std::max(46.f, limit - width);
        width = std::min(width, limit - start);
        if (chat) {
            auto* text = static_cast<CCLabelBMFont*>(content);
            if (text->getContentSize().width * text->getScale() > width - 10.f)
                text->setScale((width - 10.f) / text->getContentSize().width);
        }
        content->setPosition({width / 2.f, height / 2.f + (chat ? .5f : 0.f)});
        bubble->setContentSize({width, height});
        bubble->setPosition({start, rowHeight / 2.f});
        auto* bg = NineSlice::create("square02b_001.png");
        bg->setContentSize({width * 2.f, height * 2.f});
        bg->setScale(.5f);
        bg->setColor(ccWHITE);
        bg->setPosition({width / 2.f, height / 2.f});
        bubble->addChild(bg, 0);
        auto* tail = CCDrawNode::create();
        CCPoint tip[] = {{1.5f, height / 2.f + 3.5f}, {1.5f, height / 2.f - 3.5f}, {-4.5f, height / 2.f}};
        tail->drawPolygon(tip, 3, {1.f, 1.f, 1.f, 1.f}, 0.f, {1.f, 1.f, 1.f, 1.f});
        bubble->addChild(tail, 1);
        bool const fresh = left > kRowEmoteSeconds - .25f;
        auto* hold = CCDelayTime::create(std::max(0.f, left - (fresh ? .2f : 0.f) - .18f));
        auto* out = CCEaseBackIn::create(CCScaleTo::create(.18f, 0.f));
        if (fresh) {
            bubble->setScale(.2f);
            bubble->runAction(CCSequence::create(CCEaseBackOut::create(CCScaleTo::create(.2f, 1.f)), hold, out,
                                                 CCRemoveSelf::create(), nullptr));
        } else
            bubble->runAction(CCSequence::create(hold, out, CCRemoveSelf::create(), nullptr));
    }

    // 방장이 Map Mode 팝업을 연다(배틀/추첨 중에는 불가).
    void onChoose(CCObject*) {
        auto const& room = Service::get().group();
        if (m_pending || m_transitioning || Service::get().busy() || !room || !host() || room->running() ||
            groupDrawing(*room))
            return;
        m_search->defocus();
        if (auto const& view = Service::get().room())
            if (auto* popup = MapSelectionPopup::create(*view)) popup->show();
    }
    // 방장이 이미 추첨된 맵을 다시 추첨하도록 서버에 요청한다.
    void onReroll(CCObject*) {
        auto const& room = Service::get().group();
        if (m_pending || m_transitioning || Service::get().busy() || !room || !host() || !groupCanReroll(*room)) return;
        m_pending = true;
        requestRender();
        Service::get().rerollMap(room->mapDraw->id,
                                 [self = Weak<GroupRoomLayer>(this)](bool success, std::string detail) {
                                     auto owner = self.lock();
                                     if (!owner) return;
                                     owner->m_pending = false;
                                     owner->requestRender();
                                     if (!success) error(detail);
                                 });
    }
    // LDM 설정을 저장한다. 배틀 중인 참가자는 바꿀 수 없다.
    void onLdm(CCObject*) {
        auto const& room = Service::get().group();
        if (room && playingNow(*room)) {
            requestRender();
            return;
        }
        Mod::get()->setSavedValue("room-ldm", !Mod::get()->getSavedValue<bool>("room-ldm", false));
    }
    void onRules(CCObject*) {
        auto const& room = Service::get().group();
        if (m_pending || !room || !host()) return;
        if (auto* popup = GroupRulesPopup::create(*room)) popup->show();
    }
    // Ready 버튼. readyControl이 정한 동작(맵 선택, 다운로드, 준비, 준비 해제)을 수행한다.
    // 준비는 부정행위 감지로 막혀 있으면 보내지 않는다.
    void onReady(CCObject*) {
        auto const& room = Service::get().group();
        if (m_pending || m_transitioning || !room) return;
        m_cached = room->level.id > 0 && mapCached(room->level.id);
        auto const* mine = room->member(self());
        bool const roll = groupNeedsRoll(*room);
        auto const control =
            readyControl(playingNow(*room) || groupDrawing(*room), m_pending, host(), room->level.id > 0 || roll,
                         m_cached || roll, m_downloading, mine && mine->ready, mapAudioSkipped(room->level.id));
        if (control.action == ReadyAction::None) return;
        if (control.action == ReadyAction::ChooseMap) {
            if (room->mapPool) {
                m_search->defocus();
                mappool::addMap();
            } else
                onChoose(nullptr);
            return;
        }
        if (control.action == ReadyAction::Download) {
            beginDownload();
            requestRender();
            return;
        }
        if (control.action == ReadyAction::SkipMusic) {
            m_readyAfterDownload = skipMapAudio(room->level.id);
            requestRender();
            return;
        }
        bool const ready = control.action == ReadyAction::Ready;
        if (ready && readyBlockedByCheat(room->rules)) return;
        m_pending = true;
        requestRender();
        Service::get().setReady(ready, [self = Weak<GroupRoomLayer>(this), ready](bool success, std::string detail) {
            auto owner = self.lock();
            if (!owner) return;
            owner->m_pending = false;
            owner->requestRender();
            if (success) {
                if (ready)
                    audio::play(audio::Cue::ReadyLock);
                else
                    audio::playBuiltin("quitSound_01.ogg");
            } else
                error(detail);
        });
    }
    // 방장이 시작(또는 룰렛 Roll)을 요청한다.
    void onStart(CCObject*) {
        auto const& room = Service::get().group();
        if (m_pending || m_transitioning || Service::get().busy() || !room || !host() || room->running()) return;
        if (readyBlockedByCheat(room->rules)) return;
        m_pending = true;
        requestRender();
        Service::get().startMatch([self = Weak<GroupRoomLayer>(this)](bool success, std::string detail) {
            auto owner = self.lock();
            if (!owner) return;
            owner->m_pending = false;
            owner->requestRender();
            if (!success) error(detail);
        });
    }
    // 방장이 진행 중인 시작 카운트다운을 취소한다.
    void onStopStart(CCObject*) {
        auto const& room = Service::get().group();
        if (m_pending || m_transitioning || !room || !host() || room->startAt <= 0) return;
        m_pending = true;
        requestRender();
        Service::get().stopGroupStart([self = Weak<GroupRoomLayer>(this)](bool success, std::string detail) {
            auto owner = self.lock();
            if (!owner) return;
            owner->m_pending = false;
            owner->requestRender();
            if (!success) error(detail);
        });
    }
    void onEmote(CCObject* sender) {
        int const tag = static_cast<CCNode*>(sender)->getTag();
        if (tag >= 0 && tag < static_cast<int>(m_emoteSet.size())) sendSignal(m_emoteSet[tag], {});
    }
    void onEmotePicker(CCObject*) {
        if (auto* popup = EmotePickerPopup::create(
                [self = Weak<GroupRoomLayer>(this)](std::string kind) {
                    if (auto owner = self.lock()) owner->sendSignal(std::move(kind), {});
                },
                [self = Weak<GroupRoomLayer>(this)] {
                    if (auto owner = self.lock()) owner->requestRender();
                }))
            popup->show();
    }
    void onChat(CCObject*) {
        if (m_sendingEmote || m_emoteCooldown > 0.f) return;
        if (auto* popup = QuickChatPopup::create([self = Weak<GroupRoomLayer>(this)](std::string text) {
                if (auto owner = self.lock()) owner->sendSignal("chat", std::move(text));
            }))
            popup->show();
    }
    // 이모트/채팅을 보낸다. 쿨다운 중이거나 쓸 수 없는 이모트면 무시한다.
    void sendSignal(std::string kind, std::string text) {
        if (m_sendingEmote || m_emoteCooldown > 0.f) return;
        if (kind != "chat" && !emotes::usable(kind, Service::get().profile().membership)) return;
        m_sendingEmote = true;
        m_emoteCooldown = std::max(firebase_config::EMOTE_COOLDOWN_SECONDS, kGroupEmoteCooldown);
        auto done = [self = Weak<GroupRoomLayer>(this)](bool success, std::string detail) {
            if (auto owner = self.lock()) {
                owner->m_sendingEmote = false;
                if (!success) error(detail);
            }
        };
        if (kind == "chat")
            Service::get().sendChat(std::move(text), std::move(done));
        else
            Service::get().sendEmote(std::move(kind), std::move(done));
    }
    // 뒤로 가기: 확인 후 방을 나간다(방장이면 다음 참가자가 방장이 된다).
    void goBack() override {
        if (m_pending || m_transitioning || m_leaveAsked || Service::get().busy()) return;
        m_leaveAsked = true;
        createQuickPopup("Leave Room",
                         host() ? "Leave this room?\nYou are the host: the next player becomes the host."
                                : "Leave this room?",
                         "Stay", "Leave", [self = Weak<GroupRoomLayer>(this)](auto*, bool yes) {
                             auto owner = self.lock();
                             if (!owner) return;
                             owner->m_leaveAsked = false;
                             if (yes) owner->leaveNow();
                         });
    }
    void leaveNow() {
        if (m_pending || m_transitioning || Service::get().busy()) return;
        m_pending = true;
        enabled(m_back, false);
        m_search->defocus();
        Service::get().leaveRoom([self = Weak<GroupRoomLayer>(this)](bool, std::string) {
            auto owner = self.lock();
            if (!owner) return;
            owner->m_pending = false;
            owner->m_transitioning = true;
            showLobby();
        });
    }
    bool sceneRunning() {
        CCNode* scene = this;
        while (scene->getParent()) scene = scene->getParent();
        return scene == CCDirector::sharedDirector()->getRunningScene();
    }

public:
    CREATE_FUNC(GroupRoomLayer);
    // 매 프레임 타이머를 갱신하고 0.25초마다 방 상태를 점검한다: 방이 닫히면 로비로 이동,
    // 내 배틀이 풀리면 launchMatch, 결과 팝업, 맵 캐시 상태, 룰렛 팝업, 방장 변경 알림, 새 이모트,
    // 시그니처가 바뀌면 화면 다시 그리기.
    void update(float dt) override {
        if (m_transitioning) return;
        offerUpdate();
        m_clock += dt;
        m_emoteCooldown = std::max(0.f, m_emoteCooldown - dt);
        if (m_filterDelay >= 0.f) {
            m_filterDelay -= dt;
            if (m_filterDelay < 0.f) renderRows(true);
        }
        m_progressElapsed += dt;
        if (m_progressElapsed >= .25f) {
            m_progressElapsed = 0.f;
            updateDownload();
        }
        updateCountdown();
        m_elapsed += dt;
        if (m_elapsed < .25f) return;
        m_elapsed = 0.f;
        auto const& room = Service::get().group();
        if (!room) {
            if (m_pending) return;
            m_transitioning = true;
            std::string notice = Service::get().takeNotice();
            if (notice.empty()) notice = "This room has closed.";
            showLobby(std::move(notice));
            return;
        }
        if (room->running() && room->battle->releasedAt == 0 && playingNow(*room) && !m_pending &&
            !isMatchLaunchActive() && sceneRunning()) {
            m_search->defocus();
            m_transitioning = true;
            launchMatch();
            return;
        }
        if (room->battle && room->battle->finishedAt > 0 && room->battle->statesFinal &&
            room->battle->seatOf(self()) >= 0 && !Service::get().groupResultShown(room->battle->id) && sceneRunning()) {
            Service::get().markGroupResultShown(room->battle->id);
            if (auto* popup = GroupResultPopup::create(*room->battle)) popup->show();
        }
        GroupRoomInfo const* shown = &*room;
        GroupRoomInfo hidden;
        if (rouletteSpinning(room->mapDraw, Service::get().serverNow()) && room->mapDraw->settled) {
            hidden = *room;
            hideSpinningDraw(hidden, Service::get().serverNow());
            shown = &hidden;
        }
        bool const cached = shown->level.id > 0 && mapCached(shown->level.id);
        if (cached != m_cached) {
            m_cached = cached;
            if (cached) {
                m_downloading = false;
                m_cacheError.clear();
                preloadBattleAssets();
            }
        }
        if (room->mapDraw && room->mapDraw->id != m_shownDraw && sceneRunning() &&
            (!room->mapDraw->settled || Service::get().serverNow() < room->mapDraw->at + DRAW_END_MS)) {
            m_shownDraw = room->mapDraw->id;
            if (auto const& view = Service::get().room())
                if (auto* popup = MapRoulettePopup::create(*view)) popup->show();
        }
        if (room->hostUid != m_lastHost) {
            if (!m_lastHost.empty() && !room->hostUid.empty()) {
                auto const* host = room->member(room->hostUid);
                auto const name = host ? host->profile.name : std::string("Someone");
                Notification::create(room->hostUid == self() ? std::string("You are now the host.")
                                                             : fmt::format("{} is now the host.", name),
                                     NotificationIcon::Info, 2.5f)
                    ->show();
            }
            m_lastHost = room->hostUid;
        }
        showRoomEmotes(*room);
        if (signature(*shown) != m_signature)
            requestRender();
        else
            renderRows();
    }
};
