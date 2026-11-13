// 신고 / 밴 / 관리자 도구 UI 모음. Service 구현에 포함되는 파일이다.
// - 플레이어 쪽: BanNoticePopup(밴 안내, 이의 신청), ReportPopup(신고), MyReportsPopup(내 신고 결과)
// - 관리자 쪽: BanEditPopup(밴 설정/수정), AdminTargetPopup(신고 대상 상세), AdminRankedPopup(레이팅·도지 조정),
//   DodgeLogPopup, AppealDetailPopup/AppealsPopup(이의 신청), AdminPanelPopup(탭형 관리 화면),
//   BroadcastSendPopup(전체 공지 전송)
// - 알림창: showRatingRefunds, showReportResults
// 중간(968번째 줄 부근)에서 Staff.inl 을 포함한다.
//
// 신고 사유 (서버 id, 표시 이름) 목록
constexpr std::array<std::pair<char const*, char const*>, 4> kReportReasons = {
    {{"speedhack", "Speedhack"}, {"noclip", "Noclip"}, {"macro", "Bot / Macro"}, {"other", "Other"}}};
// 밴 기간 선택지 (표시 이름, 기간 ms). 0 은 영구 밴이다. 마지막 항목이 영구.
constexpr std::array<std::pair<char const*, int64_t>, 10> kBanDurations = {{{"1 hour", 3'600'000LL},
                                                                            {"6 hours", 21'600'000LL},
                                                                            {"1 day", 86'400'000LL},
                                                                            {"3 days", 259'200'000LL},
                                                                            {"7 days", 604'800'000LL},
                                                                            {"14 days", 1'209'600'000LL},
                                                                            {"30 days", 2'592'000'000LL},
                                                                            {"90 days", 7'776'000'000LL},
                                                                            {"1 year", 31'536'000'000LL},
                                                                            {"Permanent", 0LL}}};
// 경고/밴 상태를 나타내는 붉은 색
constexpr ccColor3B kAlert = {255, 104, 92};

// FLAlertLayer 의 색 태그로 해석되지 않도록 < > 를 ( ) 로 바꾼다.
std::string plainAlertText(std::string text) {
    for (auto& c : text)
        if (c == '<' || c == '>') c = c == '<' ? '(' : ')';
    return text;
}
// 신고 사유 id 를 표시 이름으로 바꾼다.
std::string reportReasonName(std::string const& id) {
    for (auto const& [key, name] : kReportReasons)
        if (id == key) return name;
    return id.empty() ? std::string("Other") : id;
}
// 밴 종료 문구: 영구이거나, 종료 시각과 남은 시간(서버 시간 기준).
std::string banEndText(BanInfo const& ban) {
    if (ban.permanent) return "Permanent";
    return fmt::format("Until {}  ({} left)", localDateTime(ban.until),
                       spanText(ban.until - Service::get().serverNow()));
}
// 선택 가능한 칩 버튼의 모양을 그린다. 선택되면 초록, 아니면 회색 배경.
void paintChoice(CCMenuItemSpriteExtra* item, char const* text, bool selected, float width = 74.f) {
    auto* sprite = ButtonSprite::create(text, static_cast<int>(width), true, "goldFont.fnt",
                                        selected ? "GJ_button_01.png" : "GJ_button_04.png", 28.f, .62f);
    sprite->setScale(.72f);
    item->setSprite(sprite);
}
// 오른쪽 기준으로 정렬되는 라벨
CCLabelBMFont* rightLabel(CCNode* parent, std::string const& text, CCPoint position, float scale, float maxWidth,
                          ccColor3B color, char const* font = "chatFont.fnt") {
    auto* value = label(parent, text, position, scale, maxWidth, color, false, font);
    value->setAnchorPoint({1.f, .5f});
    return value;
}

// Staff.inl 이후에 정의되는 이의 신청 작성창 열기 함수(BanNoticePopup 이 사용)
void openBanAppealWriter(std::function<void()> sent);

// 밴 안내 팝업. 사유와 종료 시각, 남은 시간을 보여 주고 이의 신청(Appeal)의 진행 상태와
// 남은 횟수에 따라 Appeal 버튼을 활성화한다.
class BanNoticePopup final : public ::versus::DeferredPopup {
    CCLabelBMFont* m_appealLine = nullptr;
    CCMenuItemSpriteExtra* m_appeal = nullptr;
    CCMenuItemSpriteExtra* m_ok = nullptr;
    // 이의 신청 상태 문구와 Appeal 버튼 상태를 갱신한다.
    // 대기 중/거절됨/신청 가능/횟수 소진 상태에 따라 안내 문구가 달라진다.
    void showAppeal() {
        auto const& status = Service::get().ownAppeal();
        m_appeal->setVisible(status.has_value());
        m_ok->setPositionX(status ? 115.f : 175.f);
        if (!status) {
            m_appealLine->setString("");
            return;
        }
        auto const now = Service::get().serverNow();
        std::string line;
        ccColor3B color = kMuted;
        auto const& latest = status->latest;
        if (latest && latest->state == "open") {
            line = fmt::format("Appeal sent {} - waiting for the staff.", localDateTime(latest->createdAt));
            color = kIce;
        } else if (latest && latest->state == "rejected") {
            line = "Appeal rejected" + (latest->answer.empty() ? std::string(".") : ": " + latest->answer);
            if (status->canAppeal)
                line += fmt::format("  You can appeal again ({} left).", status->left);
            else if (status->nextAt > now)
                line += "  Next appeal: " + localDateTime(status->nextAt) + ".";
            else if (status->left <= 0)
                line += "  No appeals left for this ban.";
            color = ccc3(255, 160, 140);
        } else if (status->canAppeal) {
            line = fmt::format("Think this ban is wrong? Send an appeal ({} per ban).", status->left);
        } else {
            line = "No appeals left for this ban.";
        }
        m_appealLine->setString(line.c_str());
        m_appealLine->setColor(color);
        m_appealLine->limitLabelWidth(326.f, .45f, .2f);
        enabled(m_appeal, status->canAppeal);
    }
    bool init(BanInfo const& ban) {
        if (!Popup::init(350.f, 230.f, "GJ_square02.png")) return false;
        setTitle("Banned", "bigFont.fnt", .7f);
        m_title->setColor(kAlert);
        label(m_mainLayer, "You are banned from Versus matches.", {175.f, 176.f}, .62f, 320.f, ccWHITE, false,
              "chatFont.fnt");
        if (!ban.reason.empty())
            label(m_mainLayer, "Reason: " + ban.reason, {175.f, 151.f}, .5f, 320.f, kIce, false, "chatFont.fnt");
        label(m_mainLayer,
              ban.permanent ? std::string("This ban is permanent.") : fmt::format("Ends {}", localDateTime(ban.until)),
              {175.f, 122.f}, .5f, 320.f, ccc3(255, 221, 120), false, "goldFont.fnt");
        if (!ban.permanent)
            label(m_mainLayer, spanText(ban.until - Service::get().serverNow()) + " left", {175.f, 101.f}, .5f, 300.f,
                  kIce, false, "chatFont.fnt");
        label(m_mainLayer, "Ranked and public rooms open again when it ends.", {175.f, 80.f}, .45f, 320.f, kMuted,
              false, "chatFont.fnt");
        m_appealLine = label(m_mainLayer, "", {175.f, 58.f}, .45f, 326.f, kMuted, false, "chatFont.fnt");
        m_ok = button(m_buttonMenu, this, menu_selector(BanNoticePopup::onOk), "OK", {175.f, 28.f}, .6f);
        m_appeal = button(m_buttonMenu, this, menu_selector(BanNoticePopup::onAppeal), "Appeal", {235.f, 28.f}, .6f,
                          "GJ_button_02.png");
        m_appeal->setID("ban-appeal-button"_spr);
        showAppeal();
        return true;
    }
    void onOk(CCObject*) { onClose(nullptr); }
    void onAppeal(CCObject*) {
        auto const& status = Service::get().ownAppeal();
        if (!status || !status->canAppeal) return;
        openBanAppealWriter([self = Weak<BanNoticePopup>(this)] {
            if (auto owner = self.lock()) owner->showAppeal();
        });
    }

public:
    static BanNoticePopup* create(BanInfo const& ban) {
        auto* result = new BanNoticePopup;
        if (result->init(ban)) {
            result->autorelease();
            return result;
        }
        delete result;
        return nullptr;
    }
};

// 신고 금지(Report Ban) 상태 안내. 신고만 막히고 경기 참여에는 영향이 없다.
void showReportBanNotice(BanInfo const& ban) {
    std::string text = ban.permanent
                           ? std::string("You can no longer report players.")
                           : fmt::format("You can't report players until <cy>{}</c> ({} left).",
                                         localDateTime(ban.until), spanText(ban.until - Service::get().serverNow()));
    if (!ban.reason.empty()) text += "\nReason: " + plainAlertText(ban.reason);
    text += "\n<cg>Matches are not affected.</c>";
    FLAlertLayer::create(nullptr, "Reporting Blocked", text.c_str(), "OK", nullptr, 360.f)->show();
}

// 신고 팝업. 사유 4가지 중 하나를 고르고 선택 입력(맵, 퍼센트 등 상세)을 적어 서버에 보낸다.
// 전송 중에는 닫을 수 없다. 이미 신고한 상대면 서버가 기존 신고를 갱신한다("updated").
class ReportPopup final : public ::versus::DeferredPopup {
    PlayerProfile m_target;
    int m_reason = -1;
    std::array<CCMenuItemSpriteExtra*, 4> m_reasons{};
    TextInput* m_detail = nullptr;
    CCLabelBMFont* m_status = nullptr;
    void onExit() override {
        if (m_detail) m_detail->defocus();
        Popup::onExit();
    }

    CCMenuItemSpriteExtra* m_send = nullptr;
    bool m_busy = false;
    bool init(PlayerProfile target) {
        if (!Popup::init(370.f, 250.f, "GJ_square02.png")) return false;
        m_target = std::move(target);
        setTitle("Report Player", "bigFont.fnt", .6f);
        styleNickname(label(m_mainLayer, m_target.name.empty() ? std::string("Player") : m_target.name, {185.f, 203.f},
                            .5f, 300.f),
                      m_target.membership, m_target.cosmetics);
        label(m_mainLayer, "What did you see?", {185.f, 181.f}, .5f, 320.f, kIce, false, "chatFont.fnt");
        for (int i = 0; i < 4; ++i) {
            auto* item = button(m_buttonMenu, this, menu_selector(ReportPopup::onReason), "", {52.f + i * 89.f, 155.f});
            item->setTag(i);
            m_reasons[i] = item;
            paintChoice(item, kReportReasons[i].second, false, 80.f);
        }
        m_detail = TextInput::create(320.f, "Details (optional): map, %, what happened");
        m_detail->setCommonFilter(CommonFilter::Any);
        m_detail->setMaxCharCount(160);
        m_detail->setPosition({185.f, 113.f});
        styleInput(m_detail);
        m_mainLayer->addChild(m_detail);
        m_status = label(m_mainLayer, "The admin checks this player's server match log.", {185.f, 79.f}, .45f, 330.f,
                         kMuted, false, "chatFont.fnt");
        m_send = button(m_buttonMenu, this, menu_selector(ReportPopup::onSend), "Send Report", {185.f, 34.f}, .6f,
                        "GJ_button_06.png");
        enabled(m_send, false);
        return true;
    }
    void say(std::string const& text, ccColor3B color) {
        m_status->setString(text.c_str());
        m_status->setColor(color);
        m_status->limitLabelWidth(330.f, .45f, .2f);
    }
    // 사유를 고르면 Send Report 버튼이 활성화된다.
    void onReason(CCObject* sender) {
        if (m_busy) return;
        m_reason = std::clamp(sender->getTag(), 0, 3);
        for (int i = 0; i < 4; ++i) paintChoice(m_reasons[i], kReportReasons[i].second, i == m_reason, 80.f);
        enabled(m_send, true);
    }
    // 신고를 전송한다. 실패하면 메시지를 보여 주고 다시 시도하게 한다.
    void onSend(CCObject*) {
        if (m_busy || m_reason < 0) return;
        m_detail->defocus();
        m_busy = true;
        enabled(m_send, false);
        say("Sending...", kIce);
        Service::get().reportPlayer(m_target, kReportReasons[m_reason].first, std::string(m_detail->getString()),
                                    [self = Weak<ReportPopup>(this)](bool ok, std::string detail) {
                                        auto owner = self.lock();
                                        if (!owner) return;
                                        owner->m_busy = false;
                                        if (!ok) {
                                            enabled(owner->m_send, true);
                                            owner->say(detail.empty() ? "Report failed. Try again." : detail, kAlert);
                                            return;
                                        }
                                        Notification::create(detail == "updated" ? "Report updated" : "Report sent",
                                                             NotificationIcon::Success)
                                            ->show();
                                        owner->onClose(nullptr);
                                    });
    }
    void onClose(CCObject* sender) override {
        if (!m_busy) Popup::onClose(sender);
    }

public:
    static ReportPopup* create(PlayerProfile const& target) {
        auto* result = new ReportPopup;
        if (result->init(target)) {
            result->autorelease();
            return result;
        }
        delete result;
        return nullptr;
    }
};

// 밴 설정/수정 팝업. 새 밴, 기존 밴 수정, 신고 금지(report=true) 세 가지 모드로 쓴다.
// 기간은 kBanDurations 를 < > 로 고르고 사유는 필수다. 일반 밴에서는 이 플레이어의 랭크 경기를
// 취소하는 옵션(없음 / 전체 / 최근 N개)도 있다. 기존 밴 수정 시 현재 남은 기간 이상인 가장 가까운 선택지로 시작한다.
class BanEditPopup final : public ::versus::DeferredPopup {
    std::string m_uid, m_name;
    int m_accountId = 0;
    bool m_report = false;
    std::optional<BanInfo> m_current;
    std::function<void()> m_changed;
    int m_choice = 4;
    CCLabelBMFont* m_duration = nullptr;
    CCLabelBMFont* m_ends = nullptr;
    CCLabelBMFont* m_status = nullptr;
    TextInput* m_reason = nullptr;
    CCMenuItemSpriteExtra* m_apply = nullptr;
    void onExit() override {
        if (m_reason) m_reason->defocus();
        if (m_count) m_count->defocus();
        Popup::onExit();
    }

    CCMenuItemSpriteExtra* m_lift = nullptr;
    bool m_busy = false;
    // 랭크 경기 취소 옵션: 안 함, 전부, 최근 N개
    enum Cancel { kCancelNone = 0, kCancelAll = 1, kCancelLatest = 2 };
    int m_cancel = kCancelNone;
    std::array<CCMenuItemSpriteExtra*, 3> m_cancelChips{};
    TextInput* m_count = nullptr;
    bool init(std::string uid, std::string name, int accountId, std::optional<BanInfo> current, std::string reason,
              std::function<void()> changed, bool report) {
        float const lift = report ? 0.f : 40.f;
        if (!Popup::init(380.f, 270.f + lift, "GJ_square02.png")) return false;
        m_uid = std::move(uid);
        m_name = std::move(name);
        m_accountId = accountId;
        m_report = report;
        m_current = std::move(current);
        m_changed = std::move(changed);
        setTitle(m_report    ? (m_current ? "Edit Report Ban" : "Report Ban")
                 : m_current ? "Edit Ban"
                             : "Ban Player",
                 "bigFont.fnt", .62f);
        label(m_mainLayer, m_name.empty() ? std::string("Player") : m_name, {190.f, 225.f + lift}, .5f, 320.f);
        if (m_current) {
            label(m_mainLayer, "Now: " + banEndText(*m_current), {190.f, 204.f + lift}, .45f, 340.f, kAlert, false,
                  "chatFont.fnt");
            m_choice = kBanDurations.size() - 1;
            if (!m_current->permanent) {
                auto const left = m_current->until - Service::get().serverNow();
                for (size_t i = 0; i + 1 < kBanDurations.size(); ++i)
                    if (kBanDurations[i].second >= left) {
                        m_choice = static_cast<int>(i);
                        break;
                    }
            }
        }
        label(m_mainLayer,
              m_current  ? "New length (from now)"
              : m_report ? "Report ban length"
                         : "Ban length",
              {190.f, 181.f + lift}, .45f, 300.f, kMuted, false, "chatFont.fnt");
        button(m_buttonMenu, this, menu_selector(BanEditPopup::onShorter), "<", {98.f, 155.f + lift}, .55f);
        button(m_buttonMenu, this, menu_selector(BanEditPopup::onLonger), ">", {282.f, 155.f + lift}, .55f);
        m_duration = label(m_mainLayer, "", {190.f, 155.f + lift}, .7f, 140.f, ccWHITE, false, "goldFont.fnt");
        m_ends = label(m_mainLayer, "", {190.f, 131.f + lift}, .45f, 340.f, kIce, false, "chatFont.fnt");
        m_reason = TextInput::create(330.f, "Reason (required; shown to the player)");
        m_reason->setCommonFilter(CommonFilter::Any);
        m_reason->setMaxCharCount(160);
        m_reason->setString(m_current && !m_current->reason.empty() ? m_current->reason : reason);
        m_reason->setPosition({190.f, 99.f + lift});
        styleInput(m_reason);
        m_mainLayer->addChild(m_reason);
        if (!m_report) {
            label(m_mainLayer, "Cancel their Ranked matches", {190.f, 113.f}, .42f, 300.f, kMuted, false,
                  "chatFont.fnt");
            for (int i = 0; i < 3; ++i) {
                m_cancelChips[i] = button(m_buttonMenu, this, menu_selector(BanEditPopup::onCancelChoice), "",
                                          {80.f + i * 80.f, 90.f});
                m_cancelChips[i]->setTag(i);
            }
            m_count = TextInput::create(60.f, "N");
            m_count->setCommonFilter(CommonFilter::Int);
            m_count->setMaxCharCount(4);
            m_count->setString("10");
            m_count->setPosition({322.f, 90.f});
            styleInput(m_count);
            m_mainLayer->addChild(m_count);
        }
        m_status = label(m_mainLayer,
                         m_report ? "Can still play; reporting is refused (also on this GD account)."
                                  : "Bans also block this GD account on other installs.",
                         {190.f, 67.f}, .42f, 340.f, kMuted, false, "chatFont.fnt");
        m_apply = button(m_buttonMenu, this, menu_selector(BanEditPopup::onApply),
                         m_current  ? "Save"
                         : m_report ? "Report Ban"
                                    : "Ban",
                         {m_current ? 130.f : 190.f, 30.f}, .6f, "GJ_button_06.png");
        if (m_current)
            m_lift = button(m_buttonMenu, this, menu_selector(BanEditPopup::onLift), m_report ? "Lift" : "Unban",
                            {250.f, 30.f}, .6f);
        refresh();
        return true;
    }
    // 선택된 기간과 종료 시각, 경기 취소 칩 표시를 갱신한다.
    void refresh() {
        if (!m_report) {
            static constexpr char const* names[] = {"None", "All", "Latest"};
            for (int i = 0; i < 3; ++i) paintChoice(m_cancelChips[i], names[i], i == m_cancel, 70.f);
            m_count->setVisible(m_cancel == kCancelLatest);
        }
        m_duration->setString(kBanDurations[m_choice].first);
        m_duration->limitLabelWidth(140.f, .7f, .3f);
        auto const length = kBanDurations[m_choice].second;
        m_ends->setString(length ? fmt::format("Ends {}", localDateTime(Service::get().serverNow() + length)).c_str()
                                 : "Never ends (permanent)");
        m_ends->setColor(length ? kIce : kAlert);
    }
    void say(std::string const& text) {
        m_status->setString(text.c_str());
        m_status->setColor(kAlert);
        m_status->limitLabelWidth(340.f, .42f, .2f);
    }
    void busy(bool value) {
        m_busy = value;
        enabled(m_apply, !value);
        if (m_lift) enabled(m_lift, !value);
    }
    void onCancelChoice(CCObject* sender) {
        if (m_busy) return;
        m_cancel = sender->getTag();
        refresh();
    }
    // 서버에 보낼 경기 취소 값: 0 은 취소 안 함, kBanCancelAll 은 전부, 양수는 최근 N개.
    // N 입력이 잘못되면 -2 를 반환한다.
    int cancelChoice() const {
        if (m_cancel == kCancelNone) return 0;
        if (m_cancel == kCancelAll) return Service::kBanCancelAll;
        auto const text = std::string(m_count->getString());
        int const count = text.empty() || text.size() > 4 ? 0 : std::atoi(text.c_str());
        return count >= 1 ? count : -2;
    }
    void onShorter(CCObject*) {
        if (!m_busy && m_choice > 0) {
            --m_choice;
            refresh();
        }
    }
    void onLonger(CCObject*) {
        if (!m_busy && m_choice + 1 < static_cast<int>(kBanDurations.size())) {
            ++m_choice;
            refresh();
        }
    }
    std::string reasonText() const {
        auto text = std::string(m_reason->getString());
        return text.find_first_not_of(' ') == std::string::npos ? std::string() : text;
    }
    // 적용 버튼. 사유를 검사한 뒤, 일반 밴이면 서버에서 취소될 경기 수를 미리 조회해
    // 영향 범위(되돌릴 수 없음)를 확인창으로 보여 주고, 확인되면 send() 로 보낸다. 신고 금지는 바로 보낸다.
    void onApply(CCObject*) {
        if (m_busy) return;
        m_reason->defocus();
        if (reasonText().empty()) {
            say(m_report ? "Write the reason for the report ban." : "Write the reason for the ban.");
            return;
        }
        if (m_report) {
            send();
            return;
        }
        if (cancelChoice() == -2) {
            say("Enter how many of the latest matches to cancel.");
            return;
        }
        busy(true);
        Service::get().fetchBanPreview(
            m_uid, [self = Weak<BanEditPopup>(this)](std::optional<BanPreview> preview, std::string detail) {
                auto owner = self.lock();
                if (!owner) return;
                owner->busy(false);
                if (!preview) {
                    owner->say(detail.empty() ? "Could not check the player's matches. Try again." : detail);
                    return;
                }
                auto const name = plainAlertText(owner->m_name.empty() ? std::string("this player") : owner->m_name);
                int const choice = owner->cancelChoice();
                int const cancels = choice == Service::kBanCancelAll ? std::min(preview->matches, preview->cancelMax)
                                                                     : std::min(preview->matches, std::max(0, choice));
                std::string body;
                if (cancels > 0)
                    body = fmt::format(
                        "{} <cy>{}</c> also <cr>{}</c>.\n"
                        "Each opponent gets back the rating lost there, and what <cy>{}</c> gained is taken away.\n"
                        "<cr>This cannot be undone.</c>",
                        owner->m_current ? "Saving the ban of" : "Banning", name,
                        choice == Service::kBanCancelAll
                            ? fmt::format("cancels all {} Ranked match{}", cancels, cancels == 1 ? "" : "es")
                            : fmt::format("cancels their latest {} Ranked match{}", cancels, cancels == 1 ? "" : "es"),
                        name);
                else if (choice != 0)
                    body = fmt::format("{} <cy>{}</c>?\nNo Ranked matches of theirs are on record to cancel.",
                                       owner->m_current ? "Save the ban of" : "Ban", name);
                else
                    body = fmt::format("{} <cy>{}</c>?\nTheir Ranked matches stay as they are ({} on record).",
                                       owner->m_current ? "Save the ban of" : "Ban", name, preview->matches);
                createQuickPopup(owner->m_current ? "Save Ban?" : "Ban Player?", body.c_str(), "Cancel",
                                 owner->m_current ? "Save" : "Ban", [self](auto*, bool yes) {
                                     if (auto owner = self.lock(); yes && owner && !owner->m_busy) owner->send();
                                 });
            });
    }
    // 밴(또는 신고 금지)을 서버에 저장한다. 성공하면 알림을 띄우고 changed 콜백 후 닫는다.
    void send() {
        busy(true);
        auto finish = [self = Weak<BanEditPopup>(this)](bool ok, int canceled, std::string detail) {
            auto owner = self.lock();
            if (!owner) return;
            owner->busy(false);
            if (!ok) {
                owner->say(detail.empty() ? "Ban failed. Try again." : detail);
                return;
            }
            auto text = std::string(owner->m_report    ? (owner->m_current ? "Report ban updated" : "Reporting blocked")
                                    : owner->m_current ? "Ban updated"
                                                       : "Player banned");
            if (canceled > 0) text += fmt::format(" - {} Ranked match{} canceled", canceled, canceled == 1 ? "" : "es");
            Notification::create(text, NotificationIcon::Success)->show();
            if (owner->m_changed) owner->m_changed();
            owner->onClose(nullptr);
        };
        if (m_report)
            Service::get().setReportBan(m_uid, m_name, m_accountId, kBanDurations[m_choice].second, reasonText(),
                                        [finish](std::optional<BanInfo> ban, std::string detail) {
                                            finish(ban.has_value(), 0, std::move(detail));
                                        });
        else
            Service::get().setBan(m_uid, m_name, m_accountId, kBanDurations[m_choice].second, reasonText(),
                                  cancelChoice(),
                                  [finish](std::optional<BanInfo> ban, int canceled, std::string detail) {
                                      finish(ban.has_value(), canceled, std::move(detail));
                                  });
    }
    // 밴/신고 금지 해제. 확인창을 거쳐 서버에 해제를 요청한다.
    void onLift(CCObject*) {
        if (m_busy) return;
        createQuickPopup(m_report ? "Lift Report Ban?" : "Unban?",
                         (m_report ? fmt::format("Let <cy>{}</c> report players again now?", m_name)
                                   : fmt::format("End the ban of <cy>{}</c> now?", m_name))
                             .c_str(),
                         "Cancel", m_report ? "Lift" : "Unban", [self = Weak<BanEditPopup>(this)](auto*, bool yes) {
                             auto owner = self.lock();
                             if (!yes || !owner || owner->m_busy) return;
                             owner->busy(true);
                             auto done = [self](bool ok, std::string detail) {
                                 auto owner = self.lock();
                                 if (!owner) return;
                                 owner->busy(false);
                                 if (!ok) {
                                     owner->say(detail.empty() ? "Unban failed. Try again." : detail);
                                     return;
                                 }
                                 Notification::create(owner->m_report ? "Report ban lifted" : "Ban lifted",
                                                      NotificationIcon::Success)
                                     ->show();
                                 if (owner->m_changed) owner->m_changed();
                                 owner->onClose(nullptr);
                             };
                             if (owner->m_report)
                                 Service::get().liftReportBan(owner->m_uid, std::move(done));
                             else
                                 Service::get().liftBan(owner->m_uid, std::move(done));
                         });
    }
    void onClose(CCObject* sender) override {
        if (!m_busy) Popup::onClose(sender);
    }

public:
    static BanEditPopup* create(std::string uid, std::string name, int accountId, std::optional<BanInfo> current,
                                std::string reason, std::function<void()> changed, bool report = false) {
        auto* result = new BanEditPopup;
        if (result->init(std::move(uid), std::move(name), accountId, std::move(current), std::move(reason),
                         std::move(changed), report)) {
            result->autorelease();
            return result;
        }
        delete result;
        return nullptr;
    }
};

// uid 와 이름만 있는 모더레이션용 프로필을 만든다(프로필 창을 열기 위함).
PlayerProfile moderationProfile(std::string uid, std::string name, int accountId) {
    PlayerProfile profile;
    profile.uid = std::move(uid);
    profile.name = name.empty() ? std::string("Player") : std::move(name);
    profile.accountId = accountId;
    return profile;
}

// UTF-8 글자 수(연속 바이트를 제외한 시작 바이트 수)를 센다.
size_t utf8Length(std::string const& text) {
    return static_cast<size_t>(
        std::count_if(text.begin(), text.end(), [](char c) { return (static_cast<unsigned char>(c) & 0xC0) != 0x80; }));
}
// 전체 공지(Broadcast) 작성 팝업. 최대 kMax 글자이며 미리보기를 보며 쓴다.
// 한글 등은 클립보드 붙여넣기로 입력하고, 전송 전에 한 번 더 확인한다.
class BroadcastSendPopup final : public ::versus::DeferredPopup {
    static constexpr size_t kMax = 200;
    TextInput* m_input = nullptr;
    CCSprite* m_preview = nullptr;
    CCLabelBMFont* m_count = nullptr;
    CCMenuItemSpriteExtra* m_send = nullptr;
    std::string m_text;
    bool m_sending = false;
    void onExit() override {
        if (m_input) m_input->defocus();
        Popup::onExit();
    }
    bool init() {
        if (!Popup::init(420.f, 270.f, "GJ_square02.png")) return false;
        setTitle("Broadcast", "goldFont.fnt", .7f);
        setID("versus-broadcast-send"_spr);
        label(m_mainLayer,
              "Every player sees it at the top of the screen (not while playing a level), then in the inbox.",
              {210.f, 226.f}, .45f, 390.f, kIce, false, "chatFont.fnt");
        m_input = TextInput::create(300.f / .7f, "Type a message (English)");
        m_input->setCommonFilter(CommonFilter::Any);
        m_input->setMaxCharCount(static_cast<int>(kMax));
        m_input->setScale(.7f);
        m_input->setPosition({176.f, 196.f});
        styleInput(m_input);
        m_input->setCallback([self = Weak<BroadcastSendPopup>(this)](std::string const& text) {
            if (auto owner = self.lock()) {
                owner->m_text = text;
                owner->refresh();
            }
        });
        m_mainLayer->addChild(m_input, 5);
        button(m_buttonMenu, this, menu_selector(BroadcastSendPopup::onPaste), "Paste", {370.f, 196.f}, .5f,
               "GJ_button_04.png")
            ->setID("broadcast-paste"_spr);
        label(m_mainLayer, "Korean or other languages: type in any app, copy it, then tap Paste.", {210.f, 172.f}, .42f,
              390.f, kMuted, false, "chatFont.fnt");
        auto* frame = panel(m_mainLayer, {20.f, 66.f}, {380.f, 92.f}, false);
        (void)frame;
        m_preview = createSystemText("", 14.f, 360.f, kCCTextAlignmentCenter);
        m_preview->setPosition({210.f, 112.f});
        m_mainLayer->addChild(m_preview, 4);
        m_count = label(m_mainLayer, "", {390.f, 56.f}, .4f, 90.f, kMuted, false, "chatFont.fnt");
        m_count->setAnchorPoint({1.f, .5f});
        button(m_buttonMenu, this, menu_selector(BroadcastSendPopup::onCancel), "Cancel", {140.f, 28.f}, .6f,
               "GJ_button_06.png");
        m_send = button(m_buttonMenu, this, menu_selector(BroadcastSendPopup::onSend), "Send", {280.f, 28.f}, .6f);
        m_send->setID("broadcast-send"_spr);
        refresh();
        return true;
    }
    // 앞뒤 공백/개행을 제거한다.
    static std::string trimmed(std::string text) {
        auto const first = text.find_first_not_of(" \t\r\n");
        if (first == std::string::npos) return {};
        auto const last = text.find_last_not_of(" \t\r\n");
        return text.substr(first, last - first + 1);
    }
    // 미리보기와 글자 수를 갱신하고, 비었거나 길이를 넘기거나 전송 중이면 Send 를 비활성화한다.
    void refresh() {
        auto const text = trimmed(m_text);
        auto const length = utf8Length(text);
        setSystemText(m_preview, text.empty() ? std::string("(preview)") : text);
        m_preview->setColor(text.empty() ? ccc3(140, 150, 175) : ccWHITE);
        m_count->setString(fmt::format("{} / {}", length, kMax).c_str());
        m_count->setColor(length > kMax ? ccc3(255, 104, 92) : kMuted);
        enabled(m_send, !text.empty() && length <= kMax && !m_sending);
    }
    void onPaste(CCObject*) {
        if (m_sending) return;
        auto text = trimmed(clipboard::read());
        if (text.empty()) {
            Notification::create("The clipboard is empty", NotificationIcon::Warning)->show();
            return;
        }
        m_input->defocus();
        m_input->setString("");
        m_text = std::move(text);
        refresh();
    }
    void onCancel(CCObject*) { onClose(nullptr); }
    // 확인창을 거쳐 전체 공지를 전송한다.
    void onSend(CCObject*) {
        auto const text = trimmed(m_text);
        if (m_sending || text.empty() || utf8Length(text) > kMax) return;
        m_input->defocus();
        createQuickPopup("Send Broadcast?", "Every player will see this message now.", "Back", "Send",
                         [self = Weak<BroadcastSendPopup>(this), text](auto*, bool yes) {
                             auto owner = self.lock();
                             if (!yes || !owner || owner->m_sending) return;
                             owner->m_sending = true;
                             owner->refresh();
                             Service::get().sendBroadcast(text, [self](bool ok, std::string detail) {
                                 auto owner = self.lock();
                                 if (!owner) return;
                                 owner->m_sending = false;
                                 if (!ok) {
                                     owner->refresh();
                                     error(detail);
                                     return;
                                 }
                                 Notification::create("Broadcast sent", NotificationIcon::Success)->show();
                                 owner->onClose(nullptr);
                             });
                         });
    }

public:
    static BroadcastSendPopup* create() {
        auto* result = new BroadcastSendPopup;
        if (result->init()) {
            result->autorelease();
            return result;
        }
        delete result;
        return nullptr;
    }
};

// 신고를 "밴 없음"으로 종결할 때 신고자에게 보낼 메모(선택)를 쓰는 팝업.
class ReportNotePopup final : public ::versus::DeferredPopup {
    TextInput* m_note = nullptr;
    std::function<void(std::string)> m_send;
    void onExit() override {
        if (m_note) m_note->defocus();
        Popup::onExit();
    }
    bool init(std::string const& name, int reports, std::function<void(std::string)> send) {
        if (!Popup::init(380.f, 220.f, "GJ_square02.png")) return false;
        m_send = std::move(send);
        setTitle("Checked - No Ban", "bigFont.fnt", .55f);
        label(m_mainLayer, fmt::format("{} report{} on {}", reports, reports == 1 ? "" : "s", name), {190.f, 172.f},
              .5f, 340.f, kIce, false, "chatFont.fnt");
        label(m_mainLayer, "Message to the reporter(s) (optional):", {190.f, 150.f}, .45f, 340.f, kMuted, false,
              "chatFont.fnt");
        m_note = TextInput::create(330.f, "e.g. The replay shows no speedhack.");
        m_note->setCommonFilter(CommonFilter::Any);
        m_note->setMaxCharCount(200);
        m_note->setPosition({190.f, 117.f});
        styleInput(m_note);
        m_mainLayer->addChild(m_note);
        label(m_mainLayer, "The reports move to the log; each reporter is told the result.", {190.f, 84.f}, .4f, 350.f,
              kMuted, false, "chatFont.fnt");
        button(m_buttonMenu, this, menu_selector(ReportNotePopup::onSend), "Close Case", {190.f, 36.f}, .6f,
               "GJ_button_04.png")
            ->setID("report-note-send"_spr);
        return true;
    }
    void onSend(CCObject*) {
        m_note->defocus();
        auto send = std::move(m_send);
        std::string note(m_note->getString());
        onClose(nullptr);
        if (send) send(std::move(note));
    }

public:
    static ReportNotePopup* create(std::string const& name, int reports, std::function<void(std::string)> send) {
        auto* result = new ReportNotePopup;
        if (result->init(name, reports, std::move(send))) {
            result->autorelease();
            return result;
        }
        delete result;
        return nullptr;
    }
};

// 신고된 플레이어 한 명의 상세 팝업(신고 목록 또는 처리된 사례 로그 모드).
// 신고자별 사유/상세를 최대 5건 보여 주고, 프로필, 매치 기록, 밴 설정, 신고 종결(Checked) 버튼을 둔다.
class AdminTargetPopup final : public ::versus::DeferredPopup {
    ReportedPlayer m_target;
    std::function<void()> m_changed;
    CCMenuItemSpriteExtra* m_dismiss = nullptr;
    bool m_busy = false;
    bool m_log = false;
    bool init(ReportedPlayer target, std::function<void()> changed, bool log) {
        if (!Popup::init(420.f, 290.f, "GJ_square02.png")) return false;
        m_target = std::move(target);
        m_changed = std::move(changed);
        m_log = log;
        setTitle(m_log ? "Report Log" : "Player Reports", "bigFont.fnt", .6f);
        label(m_mainLayer, m_target.name, {210.f, 247.f}, .5f, 360.f);
        label(m_mainLayer,
              fmt::format("{} {}report{}   |   GD account {}", m_target.count, m_log ? "" : "open ",
                          m_target.count == 1 ? "" : "s",
                          m_target.accountId > 0 ? std::to_string(m_target.accountId) : std::string("none")),
              {210.f, 228.f}, .45f, 380.f, kMuted, false, "chatFont.fnt");
        if (m_log) {
            bool const banned = m_target.state == "banned";
            label(m_mainLayer,
                  fmt::format("{}{}  -  {}", m_target.outcome,
                              banned && m_target.banUntil > 0 ? " (until " + localDateTime(m_target.banUntil) + ")"
                                                              : std::string(),
                              localDateTime(m_target.handledAt)),
                  {210.f, 212.f}, .42f, 380.f, banned ? kAlert : ccc3(130, 255, 165), false, "chatFont.fnt");
            if (!m_target.note.empty())
                label(m_mainLayer, "Note to reporters: " + m_target.note, {210.f, 199.f}, .4f, 390.f,
                      ccc3(255, 221, 120), false, "chatFont.fnt");
        } else if (m_target.ban)
            label(m_mainLayer, "BANNED - " + banEndText(*m_target.ban), {210.f, 212.f}, .42f, 380.f, kAlert, false,
                  "chatFont.fnt");
        size_t const shown = std::min<size_t>(m_target.reports.size(), 5);
        auto* rows = menu(m_mainLayer);
        for (size_t i = 0; i < shown; ++i) {
            auto const& report = m_target.reports[i];
            float const y = 181.f - static_cast<float>(i) * 29.f;
            auto* row = panel(m_mainLayer, {15.f, y - 13.f}, {390.f, 26.f}, false);
            auto* hit = CCSprite::create();
            hit->setContentSize({390.f, 26.f});
            auto* open = CCMenuItemSpriteExtra::create(hit, this, menu_selector(AdminTargetPopup::onReporter));
            open->setTag(static_cast<int>(i));
            open->setPosition({210.f, y});
            open->m_scaleMultiplier = 1.02f;
            open->setID(fmt::format("report-reporter-{}", i));
            rows->addChild(open);
            auto* who =
                label(row,
                      fmt::format("{}: {}", report.reporterName.empty() ? std::string("Player") : report.reporterName,
                                  reportReasonName(report.reason)),
                      {8.f, 18.f}, .45f, 250.f, ccc3(150, 220, 255), true, "chatFont.fnt");
            if (report.reporterReportBanned)
                label(row, "REPORT BANNED", {who->getPositionX() + who->getScaledContentSize().width + 6.f, 18.f}, .28f,
                      90.f, kAlert, true, "bigFont.fnt");
            rightLabel(row, agoText(Service::get().serverNow() - report.createdAt), {382.f, 18.f}, .4f, 90.f, kMuted);
            label(row, report.detail.empty() ? std::string("(no details)") : report.detail, {8.f, 7.f}, .4f, 372.f,
                  report.detail.empty() ? kMuted : kIce, true, "chatFont.fnt");
        }
        if (m_target.count > static_cast<int>(shown))
            label(m_mainLayer, fmt::format("+{} older", m_target.count - static_cast<int>(shown)), {210.f, 44.f}, .4f,
                  200.f, kMuted, false, "chatFont.fnt");
        float const step = m_log ? 120.f : 95.f, first = m_log ? 90.f : 68.f;
        button(m_buttonMenu, this, menu_selector(AdminTargetPopup::onProfile), "Profile", {first, 26.f}, .5f);
        button(m_buttonMenu, this, menu_selector(AdminTargetPopup::onMatches), "Matches", {first + step, 26.f}, .5f);
        button(m_buttonMenu, this, menu_selector(AdminTargetPopup::onBan), m_target.ban ? "Edit Ban" : "Ban",
               {first + 2.f * step, 26.f}, .5f, "GJ_button_06.png");
        if (!m_log)
            m_dismiss = button(m_buttonMenu, this, menu_selector(AdminTargetPopup::onDismiss), "Checked", {355.f, 26.f},
                               .5f, "GJ_button_04.png");
        return true;
    }
    PlayerProfile profile() const { return moderationProfile(m_target.uid, m_target.name, m_target.accountId); }
    void onProfile(CCObject*) { showPlayer(profile()); }
    void onReporter(CCObject* sender) {
        int const index = sender->getTag();
        if (index < 0 || index >= static_cast<int>(m_target.reports.size())) return;
        auto const& report = m_target.reports[index];
        if (report.reporterUid.empty()) return;
        showPlayer(moderationProfile(report.reporterUid, report.reporterName, report.reporterAccountId));
    }
    void onMatches(CCObject*) {
        if (auto* popup = HistoryPopup::create(profile())) popup->show();
    }
    void onBan(CCObject*) {
        auto reason = m_target.reports.empty() ? std::string() : reportReasonName(m_target.reports.front().reason);
        if (auto* popup = BanEditPopup::create(m_target.uid, m_target.name, m_target.accountId, m_target.ban, reason,
                                               [self = Weak<AdminTargetPopup>(this)] {
                                                   if (auto owner = self.lock()) {
                                                       if (owner->m_changed) owner->m_changed();
                                                       owner->onClose(nullptr);
                                                   }
                                               }))
            popup->show();
    }
    // 신고 종결. 메모 입력창을 거쳐 서버에 처리하고, 성공하면 목록을 갱신하고 닫는다.
    void onDismiss(CCObject*) {
        if (m_busy) return;
        if (auto* popup = ReportNotePopup::create(m_target.name, m_target.count,
                                                  [self = Weak<AdminTargetPopup>(this)](std::string note) {
                                                      if (auto owner = self.lock()) owner->dismiss(std::move(note));
                                                  }))
            popup->show();
    }
    void dismiss(std::string note) {
        if (m_busy) return;
        m_busy = true;
        enabled(m_dismiss, false);
        Service::get().dismissReports(
            m_target.uid, std::move(note), [self = Weak<AdminTargetPopup>(this)](bool ok, std::string detail) {
                auto owner = self.lock();
                if (!owner) return;
                owner->m_busy = false;
                enabled(owner->m_dismiss, true);
                if (!ok) {
                    error(detail);
                    return;
                }
                Notification::create("Checked - moved to the log", NotificationIcon::Success)->show();
                if (owner->m_changed) owner->m_changed();
                owner->onClose(nullptr);
            });
    }

public:
    static AdminTargetPopup* create(ReportedPlayer target, std::function<void()> changed, bool log = false) {
        auto* result = new AdminTargetPopup;
        if (result->init(std::move(target), std::move(changed), log)) {
            result->autorelease();
            return result;
        }
        delete result;
        return nullptr;
    }
};

// 도지(dodge) 원인 코드를 사람이 읽는 문구로 바꾼다.
std::string dodgeCauseText(DodgeLogEntry const& entry) {
    if (entry.cause == "left") return "left the match";
    if (entry.cause == "disconnected") return "disconnected";
    if (entry.cause == "sweep") return "stopped answering";
    if (entry.cause == "download") return "map download failed (opponent had it)";
    if (entry.cause == "admin") return "stack set by an admin";
    return entry.cause.empty() ? std::string("dodge") : entry.cause;
}
// 도지 로그 팝업. entries 를 주면 그 목록을 그대로 보여 주고(특정 플레이어용),
// 없으면 서버에서 전체 로그를 받아 온다. 한 페이지에 kPage 건씩 보여 주며 플레이어 이름을 누르면 프로필로 이동한다.
class DodgeLogPopup final : public ::versus::DeferredPopup {
    static constexpr int kPage = 5;
    std::vector<DodgeLogEntry> m_entries;
    std::string m_title;
    bool m_loading = false;
    int m_page = 0;
    CCNode* m_rows = nullptr;
    CCLabelBMFont* m_status = nullptr;
    CCLabelBMFont* m_pageLabel = nullptr;
    CCMenuItemSpriteExtra* m_previous = nullptr;
    CCMenuItemSpriteExtra* m_next = nullptr;
    bool init(std::optional<std::vector<DodgeLogEntry>> entries, std::string title) {
        if (!Popup::init(440.f, 280.f, "GJ_square02.png")) return false;
        m_title = std::move(title);
        setTitle(m_title.empty() ? "Dodge Log" : m_title.c_str(), "bigFont.fnt", .58f);
        m_rows = CCNode::create();
        m_mainLayer->addChild(m_rows);
        m_status = label(m_mainLayer, "Loading...", {220.f, 140.f}, .5f, 380.f, kIce, false, "chatFont.fnt");
        m_previous = button(m_buttonMenu, this, menu_selector(DodgeLogPopup::onPrevious), "<", {170.f, 22.f}, .45f);
        m_next = button(m_buttonMenu, this, menu_selector(DodgeLogPopup::onNext), ">", {270.f, 22.f}, .45f);
        m_pageLabel = label(m_mainLayer, "1 / 1", {220.f, 22.f}, .33f, 70.f, kIce);
        if (entries) {
            m_entries = std::move(*entries);
            render();
        } else {
            iconButton(m_buttonMenu, this, menu_selector(DodgeLogPopup::onRefresh), "GJ_updateBtn_001.png",
                       {408.f, 24.f}, 28.f);
            load();
        }
        return true;
    }
    void load() {
        if (m_loading) return;
        m_loading = true;
        m_status->setVisible(true);
        m_status->setString("Loading...");
        Service::get().fetchDodgeLog(
            [self = Weak<DodgeLogPopup>(this)](std::vector<DodgeLogEntry> entries, std::string detail) {
                auto owner = self.lock();
                if (!owner) return;
                owner->m_loading = false;
                owner->m_entries = std::move(entries);
                owner->render();
                if (!detail.empty()) {
                    owner->m_status->setVisible(true);
                    owner->m_status->setString(detail.c_str());
                }
            });
    }
    void render() {
        m_rows->removeAllChildrenWithCleanup(true);
        int const count = static_cast<int>(m_entries.size());
        int const pages = std::max(1, (count + kPage - 1) / kPage);
        m_page = std::clamp(m_page, 0, pages - 1);
        m_pageLabel->setString(fmt::format("{} / {}", m_page + 1, pages).c_str());
        enabled(m_previous, m_page > 0);
        enabled(m_next, m_page + 1 < pages);
        m_status->setVisible(count == 0);
        m_status->setString("No dodges recorded (kept 90 days).");
        auto* actions = menu(m_rows);
        auto const now = Service::get().serverNow();
        for (int row = 0; row < kPage; ++row) {
            int const index = m_page * kPage + row;
            if (index >= count) break;
            auto const& entry = m_entries[index];
            float const y = 214.f - row * 38.f;
            auto* cell = panel(m_rows, {14.f, y - 17.f}, {412.f, 34.f}, false);
            bool const admin = entry.cause == "admin";
            auto* who = CCLabelBMFont::create(entry.name.empty() ? "Player" : entry.name.c_str(), "bigFont.fnt");
            who->setScale(std::min(.36f, 130.f / std::max(1.f, who->getContentSize().width)));
            auto* whoItem = CCMenuItemSpriteExtra::create(who, this, menu_selector(DodgeLogPopup::onPlayer));
            whoItem->setAnchorPoint({0.f, .5f});
            whoItem->setPosition({22.f, y + 7.f});
            whoItem->setTag(index * 2);
            actions->addChild(whoItem);
            float const after = 22.f + who->getScaledContentSize().width + 6.f;
            if (!admin && !entry.opponentUid.empty()) {
                label(m_rows, "vs", {after + 6.f, y + 7.f}, .32f, 20.f, kMuted, false, "chatFont.fnt");
                auto* rival = CCLabelBMFont::create(entry.opponentName.empty() ? "Player" : entry.opponentName.c_str(),
                                                    "bigFont.fnt");
                rival->setScale(std::min(.32f, 120.f / std::max(1.f, rival->getContentSize().width)));
                rival->setColor(ccc3(255, 200, 120));
                auto* rivalItem = CCMenuItemSpriteExtra::create(rival, this, menu_selector(DodgeLogPopup::onPlayer));
                rivalItem->setAnchorPoint({0.f, .5f});
                rivalItem->setPosition({after + 16.f, y + 7.f});
                rivalItem->setTag(index * 2 + 1);
                actions->addChild(rivalItem);
            }
            rightLabel(cell, fmt::format("{}  ({})", localDateTime(entry.at), agoText(now - entry.at)), {404.f, 24.f},
                       .36f, 170.f, kMuted);
            std::string line;
            if (admin)
                line = fmt::format("Stack set to {}{}", entry.stackAfter,
                                   entry.reason.empty() ? std::string() : "  |  " + entry.reason);
            else {
                line = entry.map ? fmt::format("Map: {} (#{}){}",
                                               entry.map->name.empty() ? std::string("?") : entry.map->name,
                                               entry.map->id, entry.mapShown ? "" : " - still in the roulette")
                                 : std::string("Before the map was chosen");
                line += fmt::format("  |  {}  |  stack {}", dodgeCauseText(entry), entry.stackAfter);
                if (entry.penalized) line += fmt::format("  |  PENALTY {:+} R", entry.ratingDelta);
            }
            label(cell, line, {8.f, 9.f}, .38f, 396.f,
                  admin             ? ccc3(255, 221, 120)
                  : entry.penalized ? kAlert
                                    : kIce,
                  true, "chatFont.fnt");
        }
    }
    void onPlayer(CCObject* sender) {
        int const tag = sender->getTag();
        int const index = tag / 2;
        if (index < 0 || index >= static_cast<int>(m_entries.size())) return;
        auto const& entry = m_entries[index];
        if (tag % 2 == 0)
            showPlayer(moderationProfile(entry.uid, entry.name, 0));
        else if (!entry.opponentUid.empty())
            showPlayer(moderationProfile(entry.opponentUid, entry.opponentName, 0));
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
    void onRefresh(CCObject*) { load(); }

public:
    static DodgeLogPopup* create(std::optional<std::vector<DodgeLogEntry>> entries = std::nullopt,
                                 std::string title = {}) {
        auto* result = new DodgeLogPopup;
        if (result->init(std::move(entries), std::move(title))) {
            result->autorelease();
            return result;
        }
        delete result;
        return nullptr;
    }
};

// 플레이어 한 명의 랭크 기록을 관리자가 조정하는 팝업.
// 레이팅을 +/- 로 바꾸고(사유 필수, 플레이어에게 알림), 도지 스택을 지정하며 필요하면 도지 제한을 해제한다.
// 서버에서 받은 기록이 있어야 동작하고, 최근 조정 이력 한 건을 보여 준다.
class AdminRankedPopup final : public ::versus::DeferredPopup {
    PlayerProfile m_profile;
    std::optional<AdminRankedRecord> m_record;
    bool m_busy = false;
    int m_stack = 0;
    bool m_lift = false;
    CCLabelBMFont* m_summary = nullptr;
    CCLabelBMFont* m_dodgeLine = nullptr;
    CCLabelBMFont* m_stackValue = nullptr;
    CCLabelBMFont* m_lastAction = nullptr;
    CCLabelBMFont* m_status = nullptr;
    CCMenuItemToggler* m_liftToggle = nullptr;
    CCLabelBMFont* m_liftLabel = nullptr;
    TextInput* m_delta = nullptr;
    TextInput* m_reason = nullptr;
    std::vector<CCMenuItemSpriteExtra*> m_actions;
    void onExit() override {
        if (m_delta) m_delta->defocus();
        if (m_reason) m_reason->defocus();
        Popup::onExit();
    }
    bool init(PlayerProfile profile) {
        if (!Popup::init(400.f, 290.f, "GJ_square02.png")) return false;
        m_profile = std::move(profile);
        setTitle("Ranked Admin", "bigFont.fnt", .6f);
        label(m_mainLayer, m_profile.name.empty() ? std::string("Player") : m_profile.name, {200.f, 248.f}, .5f, 340.f);
        m_summary = label(m_mainLayer, "Loading...", {200.f, 230.f}, .42f, 370.f, kIce, false, "chatFont.fnt");
        m_dodgeLine = label(m_mainLayer, "", {200.f, 215.f}, .4f, 370.f, kMuted, false, "chatFont.fnt");
        label(m_mainLayer, "Rating change", {72.f, 190.f}, .42f, 120.f, ccWHITE, false, "chatFont.fnt");
        m_delta = TextInput::create(110.f, "+25 / -25");
        m_delta->setCommonFilter(CommonFilter::Float);
        m_delta->setMaxCharCount(7);
        m_delta->setPosition({72.f, 166.f});
        styleInput(m_delta);
        m_mainLayer->addChild(m_delta);
        m_actions.push_back(button(m_buttonMenu, this, menu_selector(AdminRankedPopup::onRating), "Change Rating",
                                   {72.f, 136.f}, .45f, "GJ_button_01.png"));
        label(m_mainLayer, "Dodge stack", {272.f, 190.f}, .42f, 120.f, ccWHITE, false, "chatFont.fnt");
        m_actions.push_back(
            button(m_buttonMenu, this, menu_selector(AdminRankedPopup::onLess), "<", {236.f, 166.f}, .45f));
        m_actions.push_back(
            button(m_buttonMenu, this, menu_selector(AdminRankedPopup::onMore), ">", {308.f, 166.f}, .45f));
        m_stackValue = label(m_mainLayer, "-", {272.f, 166.f}, .55f, 40.f, ccWHITE, false, "goldFont.fnt");
        m_liftToggle = CCMenuItemToggler::createWithStandardSprites(this, menu_selector(AdminRankedPopup::onLift), .5f);
        m_liftToggle->setPosition({340.f, 166.f});
        m_buttonMenu->addChild(m_liftToggle);
        m_liftLabel = label(m_mainLayer, "lift", {358.f, 166.f}, .34f, 40.f, kMuted, true, "chatFont.fnt");
        m_actions.push_back(button(m_buttonMenu, this, menu_selector(AdminRankedPopup::onStack), "Set Stack",
                                   {272.f, 136.f}, .45f, "GJ_button_02.png"));
        m_reason = TextInput::create(350.f, "Reason (required; the player sees it for a rating change)");
        m_reason->setCommonFilter(CommonFilter::Any);
        m_reason->setMaxCharCount(160);
        m_reason->setPosition({200.f, 104.f});
        styleInput(m_reason);
        m_mainLayer->addChild(m_reason);
        m_lastAction = label(m_mainLayer, "", {200.f, 76.f}, .36f, 370.f, ccc3(255, 221, 120), false, "chatFont.fnt");
        m_status = label(m_mainLayer, "", {200.f, 60.f}, .4f, 370.f, kAlert, false, "chatFont.fnt");
        button(m_buttonMenu, this, menu_selector(AdminRankedPopup::onDodgeLog), "Dodge Log", {140.f, 28.f}, .5f,
               "GJ_button_04.png");
        button(m_buttonMenu, this, menu_selector(AdminRankedPopup::onProfile), "Profile", {260.f, 28.f}, .5f);
        busy(true);
        Service::get().fetchAdminRanked(
            m_profile.uid,
            [self = Weak<AdminRankedPopup>(this)](std::optional<AdminRankedRecord> record, std::string detail) {
                if (auto owner = self.lock()) owner->loaded(std::move(record), std::move(detail));
            });
        return true;
    }
    // 서버가 돌려준 랭크 기록을 화면에 반영한다(레이팅, 경기 수, 도지 현황, 제한 상태, 마지막 조정).
    void loaded(std::optional<AdminRankedRecord> record, std::string detail) {
        busy(false);
        if (!record) {
            say(detail.empty() ? "Could not load the Ranked record." : detail);
            if (!m_record) busy(true);
            return;
        }
        m_record = std::move(record);
        auto const& r = *m_record;
        m_summary->setString(fmt::format("R{:.1f}  |  {} matches ({}W {}L)  |  placements {}/5", r.rating, r.matches,
                                         r.wins, r.losses, std::min(5, r.placements))
                                 .c_str());
        m_summary->limitLabelWidth(370.f, .42f, .2f);
        auto const now = Service::get().serverNow();
        bool const restricted = r.dodge.blockedUntil > now;
        m_dodgeLine->setString(
            fmt::format("Dodges {}/{}  |  {} total, {} penalties{}", r.dodge.stack, r.dodge.limit, r.dodges,
                        r.dodge.penalties,
                        restricted ? fmt::format("  |  restricted until {}", localDateTime(r.dodge.blockedUntil))
                                   : std::string())
                .c_str());
        m_dodgeLine->setColor(restricted ? kAlert : kMuted);
        m_dodgeLine->limitLabelWidth(370.f, .4f, .2f);
        m_stack = std::clamp(r.dodge.stack, 0, std::max(0, r.dodge.limit - 1));
        m_lift = restricted;
        m_liftToggle->toggle(m_lift);
        m_liftToggle->setVisible(restricted);
        m_liftLabel->setVisible(restricted);
        refreshStack();
        if (!r.actions.empty()) {
            auto const& a = r.actions.front();
            m_lastAction->setString(
                (a.kind == "rating" ? fmt::format("Last: rating {:+.1f} (R{:.1f} > R{:.1f}) by {}, {} - {}", a.amount,
                                                  a.before, a.after, a.adminName, agoText(now - a.at), a.reason)
                                    : fmt::format("Last: dodge stack {:.0f} > {:.0f} by {}, {} - {}", a.before, a.after,
                                                  a.adminName, agoText(now - a.at), a.reason))
                    .c_str());
            m_lastAction->limitLabelWidth(370.f, .36f, .18f);
        }
    }
    void refreshStack() {
        int const limit = m_record ? m_record->dodge.limit : 3;
        m_stackValue->setString(fmt::format("{}/{}", m_stack, limit).c_str());
        m_stackValue->limitLabelWidth(40.f, .55f, .3f);
    }
    void say(std::string const& text) {
        m_status->setString(text.c_str());
        m_status->limitLabelWidth(370.f, .4f, .2f);
    }
    void busy(bool value) {
        m_busy = value;
        for (auto* item : m_actions) enabled(item, !value);
    }
    std::string reason() const {
        auto text = std::string(m_reason->getString());
        auto first = text.find_first_not_of(' ');
        return first == std::string::npos ? std::string() : text;
    }
    // 조정 요청을 보낸다. 처리 중에는 버튼을 막고, 성공하면 갱신된 기록으로 화면을 바꾼다.
    void apply(std::function<void(Service::AdminRankedCallback)> send, std::string done) {
        if (m_busy || !m_record) return;
        if (m_delta) m_delta->defocus();
        m_reason->defocus();
        busy(true);
        say("");
        send([self = Weak<AdminRankedPopup>(this), done = std::move(done)](std::optional<AdminRankedRecord> record,
                                                                           std::string detail) {
            auto owner = self.lock();
            if (!owner) return;
            if (!record) {
                owner->busy(false);
                owner->say(detail.empty() ? "Failed. Try again." : detail);
                return;
            }
            Notification::create(done, NotificationIcon::Success)->show();
            owner->loaded(std::move(record), {});
        });
    }
    // 레이팅 변경. 사유를 확인하고 입력값이 0 이 아니며 절댓값 1000 이하인지 검사한 뒤 확인창을 띄운다.
    void onRating(CCObject*) {
        if (reason().empty()) {
            say("Write a reason first.");
            return;
        }
        auto const text = std::string(m_delta->getString());
        char* end = nullptr;
        double const delta = std::strtod(text.c_str(), &end);
        if (text.empty() || !end || *end != '\0' || !std::isfinite(delta) || delta == 0. || std::abs(delta) > 1000.) {
            say("Enter a change between -1000 and +1000 (for example 25 or -25).");
            return;
        }
        createQuickPopup(
            "Change Rating?",
            fmt::format("<cy>{}</c>: rating <c{}>{:+.1f}</c>\nThe player is told, with the reason.",
                        plainAlertText(m_profile.name), delta > 0 ? 'g' : 'r', delta)
                .c_str(),
            "Cancel", "Change", [self = Weak<AdminRankedPopup>(this), delta](auto*, bool yes) {
                auto owner = self.lock();
                if (!yes || !owner) return;
                owner->apply(
                    [uid = owner->m_profile.uid, delta, reason = owner->reason()](Service::AdminRankedCallback done) {
                        Service::get().adjustRating(uid, delta, reason, std::move(done));
                    },
                    "Rating changed");
            });
    }
    void onLess(CCObject*) {
        if (!m_busy && m_stack > 0) {
            --m_stack;
            refreshStack();
        }
    }
    void onMore(CCObject*) {
        int const limit = m_record ? m_record->dodge.limit : 3;
        if (!m_busy && m_stack + 1 < limit) {
            ++m_stack;
            refreshStack();
        }
    }
    void onLift(CCObject*) { m_lift = !m_lift; }
    void onStack(CCObject*) {
        if (reason().empty()) {
            say("Write a reason first.");
            return;
        }
        apply(
            [uid = m_profile.uid, stack = m_stack, reason = reason(),
             lift = m_lift](Service::AdminRankedCallback done) {
                Service::get().setDodgeStack(uid, stack, reason, lift, std::move(done));
            },
            "Dodge stack set");
    }
    void onDodgeLog(CCObject*) {
        if (!m_record) return;
        if (auto* popup = DodgeLogPopup::create(m_record->dodgeLog, fmt::format("Dodges - {}", m_record->name)))
            popup->show();
    }
    void onProfile(CCObject*) { showPlayer(m_profile); }
    void onClose(CCObject* sender) override {
        if (!m_busy || !m_record) Popup::onClose(sender);
    }

public:
    static AdminRankedPopup* create(PlayerProfile profile) {
        auto* result = new AdminRankedPopup;
        if (result->init(std::move(profile))) {
            result->autorelease();
            return result;
        }
        delete result;
        return nullptr;
    }
};

// 스태프 메일/관리 UI(StaffWritePopup, StaffPopup 등)를 여기서 포함한다.
#include "Staff.inl"

// 밴 이의 신청서 작성창을 연다. 전송에 성공하면 sent 콜백을 호출한다.
void openBanAppealWriter(std::function<void()> sent) {
    auto* writer = StaffWritePopup::create(
        true, "Why should this ban end? Write it in English - every staff member can read it.", "",
        [sent = std::move(sent)](std::string, std::string text, Done done) {
            Service::get().sendBanAppeal(std::move(text), [done = std::move(done), sent](bool ok, std::string detail) {
                done(ok, std::move(detail));
                if (ok && sent) sent();
            });
        },
        "Ban Appeal");
    if (writer) writer->show();
}

// 이의 신청 한 건의 상세 팝업. 열려 있는(open) 신청이면 답변을 적고 Reject / Lift Ban 으로 처리하며,
// 이미 처리됐으면 결과만 보여 준다. 밴이 이미 끝났다면 Lift Ban 은 막는다.
class AppealDetailPopup final : public ::versus::DeferredPopup {
    static constexpr float W = 440.f, H = 300.f;
    BanAppeal m_appeal;
    Service::AppealsCallback m_changed;
    TextInput* m_answer = nullptr;
    CCLabelBMFont* m_status = nullptr;
    std::vector<CCMenuItemSpriteExtra*> m_actions;
    bool m_busy = false;
    void onExit() override {
        if (m_answer) m_answer->defocus();
        Popup::onExit();
    }
    bool init(BanAppeal appeal, Service::AppealsCallback changed) {
        if (!Popup::init(W, H, "GJ_square02.png")) return false;
        m_appeal = std::move(appeal);
        m_changed = std::move(changed);
        setTitle("Ban Appeal", "goldFont.fnt", .7f);
        auto const now = Service::get().serverNow();
        auto* head = NineSlice::create("square02b_001.png");
        head->setContentSize({W - 32.f, 44.f});
        head->setPosition({W / 2.f, H - 68.f});
        head->setColor({20, 30, 58});
        head->setOpacity(230);
        m_mainLayer->addChild(head);
        auto* menuHead = CCMenu::create();
        menuHead->setPosition({0.f, 0.f});
        m_mainLayer->addChild(menuHead, 2);
        auto* who = CCLabelBMFont::create(m_appeal.name.empty() ? "Player" : m_appeal.name.c_str(), "bigFont.fnt");
        who->setScale(std::min(.42f, 200.f / std::max(1.f, who->getContentSize().width)));
        auto* whoItem = CCMenuItemSpriteExtra::create(who, this, menu_selector(AppealDetailPopup::onPlayer));
        whoItem->setAnchorPoint({0.f, .5f});
        whoItem->setPosition({26.f, H - 58.f});
        menuHead->addChild(whoItem);
        rightLabel(m_mainLayer,
                   fmt::format("{}  ({})", localDateTime(m_appeal.createdAt), agoText(now - m_appeal.createdAt)),
                   {W - 26.f, H - 58.f}, .38f, 200.f, kMuted);
        std::string ban =
            "Ban: " + (m_appeal.banReason.empty() ? std::string("(no reason)") : m_appeal.banReason) + "  |  " +
            (m_appeal.banPermanent ? std::string("permanent") : "until " + localDateTime(m_appeal.banUntil));
        if (!m_appeal.banActive) ban += "  |  no longer running";
        label(m_mainLayer, ban, {26.f, H - 78.f}, .38f, W - 52.f, m_appeal.banActive ? kIce : kMuted, true,
              "chatFont.fnt");
        bool const open = m_appeal.state == "open";
        float const textBottom = open ? 104.f : 92.f;
        systemTextBox(m_mainLayer, m_appeal.text, {16.f, textBottom}, {W - 32.f, H - 96.f - textBottom}, 13.f, ccWHITE);
        if (open) {
            m_answer = TextInput::create(300.f / .7f, "Answer to the player (optional, English)");
            m_answer->setCommonFilter(CommonFilter::Any);
            m_answer->setMaxCharCount(300);
            m_answer->setScale(.7f);
            m_answer->setPosition({W / 2.f, 80.f});
            styleInput(m_answer);
            m_mainLayer->addChild(m_answer, 5);
            m_actions.push_back(button(m_buttonMenu, this, menu_selector(AppealDetailPopup::onReject), "Reject",
                                       {W / 2.f - 80.f, 30.f}, .55f, "GJ_button_06.png"));
            m_actions.push_back(button(m_buttonMenu, this, menu_selector(AppealDetailPopup::onAccept), "Lift Ban",
                                       {W / 2.f + 80.f, 30.f}, .55f, "GJ_button_01.png"));
            if (!m_appeal.banActive) enabled(m_actions.back(), false);
        } else {
            std::string result = m_appeal.state == "accepted" ? "Accepted - the ban was lifted" : "Rejected";
            if (!m_appeal.answeredBy.empty()) result += " by " + m_appeal.answeredBy;
            result += "  |  " + localDateTime(m_appeal.answeredAt);
            label(m_mainLayer, result, {W / 2.f, 76.f}, .42f, W - 50.f,
                  m_appeal.state == "accepted" ? ccc3(150, 255, 175) : ccc3(255, 160, 140), false, "chatFont.fnt");
            if (!m_appeal.answer.empty())
                label(m_mainLayer, "Answer: " + m_appeal.answer, {W / 2.f, 56.f}, .4f, W - 50.f, kIce, false,
                      "chatFont.fnt");
            button(m_buttonMenu, this, menu_selector(AppealDetailPopup::onDone), "OK", {W / 2.f, 26.f}, .55f);
        }
        m_status = label(m_mainLayer, "", {W / 2.f, 56.f}, .4f, W - 50.f, kAlert, false, "chatFont.fnt");
        return true;
    }
    // 신청에 답한다(accept 면 밴 해제). 성공하면 갱신된 신청 목록을 changed 콜백으로 넘기고 닫는다.
    void answer(bool accept) {
        if (m_busy || !m_answer) return;
        m_answer->defocus();
        m_busy = true;
        for (auto* item : m_actions) enabled(item, false);
        m_status->setString("Sending...");
        m_status->setColor(kIce);
        Service::get().answerBanAppeal(
            m_appeal.id, accept, std::string(m_answer->getString()),
            [self = Weak<AppealDetailPopup>(this)](std::vector<BanAppeal> appeals, int open, std::string error) {
                auto owner = self.lock();
                if (!owner) return;
                owner->m_busy = false;
                if (!error.empty()) {
                    owner->m_status->setString(error.c_str());
                    owner->m_status->setColor(kAlert);
                    owner->m_status->limitLabelWidth(W - 50.f, .4f, .2f);
                    for (auto* item : owner->m_actions) enabled(item, true);
                    return;
                }
                if (owner->m_changed) owner->m_changed(std::move(appeals), open, {});
                owner->onClose(nullptr);
            });
    }
    void onAccept(CCObject*) { answer(true); }
    void onReject(CCObject*) { answer(false); }
    void onDone(CCObject*) { onClose(nullptr); }
    void onPlayer(CCObject*) { showPlayer(moderationProfile(m_appeal.uid, m_appeal.name, m_appeal.accountId)); }
    void onClose(CCObject* sender) override {
        if (!m_busy) Popup::onClose(sender);
    }

public:
    static AppealDetailPopup* create(BanAppeal appeal, Service::AppealsCallback changed) {
        auto* result = new AppealDetailPopup;
        if (result->init(std::move(appeal), std::move(changed))) {
            result->autorelease();
            return result;
        }
        delete result;
        return nullptr;
    }
};

// 이의 신청 목록(Appeal Box) 팝업. 한 페이지에 kPage 건씩 보여 주며, 대기 중인 건수는 open 콜백으로 알린다.
class AppealsPopup final : public ::versus::DeferredPopup {
    static constexpr int kPage = 5;
    std::vector<BanAppeal> m_appeals;
    std::function<void(int)> m_open;
    bool m_loading = false;
    int m_page = 0;
    CCNode* m_rows = nullptr;
    CCLabelBMFont* m_status = nullptr;
    CCLabelBMFont* m_pageLabel = nullptr;
    CCMenuItemSpriteExtra* m_previous = nullptr;
    CCMenuItemSpriteExtra* m_next = nullptr;
    bool init(std::function<void(int)> open) {
        if (!Popup::init(440.f, 280.f, "GJ_square02.png")) return false;
        m_open = std::move(open);
        setTitle("Appeal Box", "bigFont.fnt", .58f);
        m_rows = CCNode::create();
        m_mainLayer->addChild(m_rows);
        m_status = label(m_mainLayer, "Loading...", {220.f, 140.f}, .5f, 380.f, kIce, false, "chatFont.fnt");
        m_previous = button(m_buttonMenu, this, menu_selector(AppealsPopup::onPrevious), "<", {170.f, 22.f}, .45f);
        m_next = button(m_buttonMenu, this, menu_selector(AppealsPopup::onNext), ">", {270.f, 22.f}, .45f);
        m_pageLabel = label(m_mainLayer, "1 / 1", {220.f, 22.f}, .33f, 70.f, kIce);
        iconButton(m_buttonMenu, this, menu_selector(AppealsPopup::onRefresh), "GJ_updateBtn_001.png", {408.f, 24.f},
                   28.f);
        load();
        return true;
    }
    void take(std::vector<BanAppeal> appeals, int open, std::string const& error) {
        m_loading = false;
        if (error.empty()) {
            m_appeals = std::move(appeals);
            if (m_open) m_open(open);
        }
        render();
        if (!error.empty()) {
            m_status->setVisible(true);
            m_status->setString(error.c_str());
        }
    }
    void load() {
        if (m_loading) return;
        m_loading = true;
        m_status->setVisible(true);
        m_status->setString("Loading...");
        Service::get().fetchBanAppeals(
            [self = Weak<AppealsPopup>(this)](std::vector<BanAppeal> appeals, int open, std::string error) {
                if (auto owner = self.lock()) owner->take(std::move(appeals), open, error);
            });
    }
    void render() {
        m_rows->removeAllChildrenWithCleanup(true);
        int const count = static_cast<int>(m_appeals.size());
        int const pages = std::max(1, (count + kPage - 1) / kPage);
        m_page = std::clamp(m_page, 0, pages - 1);
        m_pageLabel->setString(fmt::format("{} / {}", m_page + 1, pages).c_str());
        enabled(m_previous, m_page > 0);
        enabled(m_next, m_page + 1 < pages);
        m_status->setVisible(count == 0);
        m_status->setString("No appeals.");
        auto* actions = menu(m_rows);
        auto const now = Service::get().serverNow();
        for (int row = 0; row < kPage; ++row) {
            int const index = m_page * kPage + row;
            if (index >= count) break;
            auto const& appeal = m_appeals[index];
            float const y = 214.f - row * 38.f;
            panel(m_rows, {14.f, y - 17.f}, {412.f, 34.f}, false);
            bool const open = appeal.state == "open";
            label(m_rows, appeal.name.empty() ? std::string("Player") : appeal.name, {22.f, y + 7.f}, .34f, 150.f,
                  ccWHITE, true, "bigFont.fnt");
            auto const [chip, chipColor] = open ? std::pair{std::string("OPEN"), ccc3(255, 221, 120)}
                                           : appeal.state == "accepted"
                                               ? std::pair{std::string("LIFTED"), ccc3(150, 255, 175)}
                                               : std::pair{std::string("REJECTED"), ccc3(255, 160, 140)};
            label(m_rows, chip, {180.f, y + 7.f}, .3f, 70.f, chipColor, true, "goldFont.fnt");
            rightLabel(m_rows,
                       fmt::format("{}  ({})", localDateTime(appeal.createdAt), agoText(now - appeal.createdAt)),
                       {360.f, y + 7.f}, .34f, 150.f, kMuted);
            std::string preview = appeal.text;
            for (auto& c : preview)
                if (c == '\n' || c == '\r') c = ' ';
            if (preview.size() > 90) preview = preview.substr(0, 87) + "...";
            label(m_rows,
                  "Ban: " + (appeal.banReason.empty() ? std::string("-") : appeal.banReason) + "  |  " + preview,
                  {22.f, y - 8.f}, .34f, 330.f, kIce, true, "chatFont.fnt");
            auto* view = button(actions, this, menu_selector(AppealsPopup::onView), "View", {396.f, y}, .4f,
                                open ? "GJ_button_02.png" : "GJ_button_04.png");
            view->setTag(index);
        }
    }
    void onView(CCObject* sender) {
        int const index = sender->getTag();
        if (index < 0 || index >= static_cast<int>(m_appeals.size())) return;
        if (auto *popup = AppealDetailPopup::create(
                m_appeals[index],
                [self = Weak<AppealsPopup>(this)](std::vector<BanAppeal> appeals, int open, std::string error) {
                    if (auto owner = self.lock()) owner->take(std::move(appeals), open, error);
                }))
            popup->show();
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
    void onRefresh(CCObject*) { load(); }

public:
    static AppealsPopup* create(std::function<void(int)> open) {
        auto* result = new AppealsPopup;
        if (result->init(std::move(open))) {
            result->autorelease();
            return result;
        }
        delete result;
        return nullptr;
    }
};

// 관리자 메인 화면(Moderation). 탭: Reports(미처리 신고), Bans, R.Bans(신고 금지), Cases(처리된 신고), Mod Log.
// Bans 계열 탭은 이름/UID/GD 계정 ID 로 검색할 수 있다. 상단 버튼으로 Staff(제작자 계정 전용), Mail, Appeals, Rules 를
// 열고, 하단에서 Dodges, Broadcast(제작자 계정 전용)를 연다. 제작자 계정이 아닌 스태프가 열면 규칙 안내가 자동으로 뜬다.
class AdminPanelPopup final : public ::versus::DeferredPopup {
    static constexpr int kPage = 5;
    enum Tab { kReports = 0, kBans = 1, kLog = 2, kReportBans = 3, kModLog = 4 };
    static constexpr std::array<int, 5> kTabOrder = {kReports, kBans, kReportBans, kLog, kModLog};
    std::function<void()> m_changed;
    int m_tab = kReports;
    // m_generation: 새로 고침 때마다 올려 이전 요청의 늦은 응답을 버리기 위한 값
    int m_page = 0, m_generation = 0;
    std::array<bool, 5> m_loaded{};
    std::string m_error;
    std::vector<ReportedPlayer> m_reports, m_log;
    std::vector<BanInfo> m_bans, m_reportBans;
    std::vector<ModLogEntry> m_modLog;
    CCNode* m_rows = nullptr;
    CCLabelBMFont* m_status = nullptr;
    CCLabelBMFont* m_pageLabel = nullptr;
    CCMenuItemSpriteExtra* m_previous = nullptr;
    CCMenuItemSpriteExtra* m_next = nullptr;
    std::array<CCMenuItemSpriteExtra*, 5> m_tabs{};
    CCMenuItemSpriteExtra* m_mail = nullptr;
    CCMenuItemSpriteExtra* m_appeals = nullptr;
    static constexpr float kSearchY = 217.f, kBanRowTop = 190.f, kBanRowPitch = 33.f;
    TextInput* m_search = nullptr;
    std::string m_query;
    std::vector<int> m_shown;
    CCLabelBMFont* m_found = nullptr;
    void onExit() override {
        if (m_search) m_search->defocus();
        Popup::onExit();
    }
    // 검색용으로 소문자로 바꾼다.
    static std::string foldedText(std::string text) {
        std::transform(text.begin(), text.end(), text.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        return text;
    }
    // 밴 목록 항목이 현재 검색어(이름, UID, GD 계정 ID)와 맞는지 확인
    bool banMatches(BanInfo const& ban) const {
        if (m_query.empty()) return true;
        return foldedText(ban.name).find(m_query) != std::string::npos ||
               foldedText(ban.uid).find(m_query) != std::string::npos ||
               (ban.accountId > 0 && std::to_string(ban.accountId).find(m_query) != std::string::npos);
    }
    bool init(std::function<void()> changed) {
        if (!Popup::init(440.f, 290.f, "GJ_square02.png")) return false;
        m_changed = std::move(changed);
        setTitle("Moderation", "bigFont.fnt", .62f);
        for (int i = 0; i < 5; ++i) {
            int const tab = kTabOrder[i];
            m_tabs[tab] =
                button(m_buttonMenu, this, menu_selector(AdminPanelPopup::onTab), "", {56.f + i * 82.f, 241.f});
            m_tabs[tab]->setTag(tab);
        }
        bool const owner = Service::get().isOwner();
        if (owner)
            button(m_buttonMenu, this, menu_selector(AdminPanelPopup::onStaff), "Staff", {340.f, 268.f}, .4f,
                   "GJ_button_02.png")
                ->setID("admin-staff-button"_spr);
        m_mail = button(m_buttonMenu, this, menu_selector(AdminPanelPopup::onMail), "Mail", {400.f, 268.f}, .4f,
                        "GJ_button_04.png");
        m_mail->setID("admin-mail-button"_spr);
        refreshMail();
        m_rows = CCNode::create();
        m_mainLayer->addChild(m_rows);
        m_search = TextInput::create(220.f, "Search name, UID or GD account ID");
        m_search->setCommonFilter(CommonFilter::Any);
        m_search->setMaxCharCount(40);
        m_search->setScale(.6f);
        m_search->setWidth(220.f / .6f);
        m_search->setPosition({136.f, kSearchY});
        styleInput(m_search);
        m_search->setCallback([self = Weak<AdminPanelPopup>(this)](std::string const& text) {
            auto owner = self.lock();
            if (!owner) return;
            auto query = foldedText(std::string(text));
            query.erase(0, query.find_first_not_of(' '));
            while (!query.empty() && query.back() == ' ') query.pop_back();
            if (query == owner->m_query) return;
            owner->m_query = std::move(query);
            owner->m_page = 0;
            owner->render();
        });
        m_mainLayer->addChild(m_search, 4);
        m_found = label(m_mainLayer, "", {256.f, kSearchY}, .4f, 160.f, kMuted, true, "chatFont.fnt");
        m_status = label(m_mainLayer, "Loading...", {220.f, 140.f}, .5f, 380.f, kIce, false, "chatFont.fnt");
        m_previous = button(m_buttonMenu, this, menu_selector(AdminPanelPopup::onPrevious), "<", {170.f, 24.f}, .45f);
        m_next = button(m_buttonMenu, this, menu_selector(AdminPanelPopup::onNext), ">", {270.f, 24.f}, .45f);
        m_pageLabel = label(m_mainLayer, "1 / 1", {220.f, 24.f}, .33f, 70.f, kIce);
        iconButton(m_buttonMenu, this, menu_selector(AdminPanelPopup::onRefresh), "GJ_updateBtn_001.png", {408.f, 26.f},
                   28.f);
        button(m_buttonMenu, this, menu_selector(AdminPanelPopup::onDodges), "Dodges", {345.f, 26.f}, .45f,
               "GJ_button_04.png")
            ->setID("admin-dodge-log-button"_spr);
        if (owner)
            button(m_buttonMenu, this, menu_selector(AdminPanelPopup::onBroadcast), "Broadcast", {50.f, 26.f}, .42f,
                   "GJ_button_02.png")
                ->setID("admin-broadcast-button"_spr);
        button(m_buttonMenu, this, menu_selector(AdminPanelPopup::onRules), "Rules", {owner ? 118.f : 50.f, 26.f}, .42f,
               "GJ_button_05.png")
            ->setID("admin-rules-button"_spr);
        m_appeals = button(m_buttonMenu, this, menu_selector(AdminPanelPopup::onAppeals), "Appeals", {46.f, 268.f},
                           .36f, "GJ_button_04.png");
        m_appeals->setID("admin-appeals-button"_spr);
        Service::get().fetchAdminStatus([self = Weak<AdminPanelPopup>(this)](AdminStatus status, std::string) {
            if (auto owner = self.lock()) owner->appealsWaiting(status.appeals);
        });
        load();
        if (!owner) {
            Loader::get()->queueInMainThread([self = Weak<AdminPanelPopup>(this)] {
                if (auto owner = self.lock(); owner && owner->getParent())
                    if (auto* rules = StaffRulesPopup::create()) rules->show();
            });
        }
        return true;
    }
    void onRules(CCObject*) {
        if (auto* rules = StaffRulesPopup::create()) rules->show();
    }
    // 대기 중인 이의 신청 수를 Appeals 버튼 문구와 색에 반영한다.
    void appealsWaiting(int open) {
        if (!m_appeals) return;
        auto* sprite = ButtonSprite::create(open > 0 ? fmt::format("Appeals ({})", open).c_str() : "Appeals",
                                            "goldFont.fnt", open > 0 ? "GJ_button_01.png" : "GJ_button_04.png");
        sprite->setScale(.36f);
        m_appeals->setNormalImage(sprite);
        m_appeals->updateSprite();
    }
    void onAppeals(CCObject*) {
        if (auto* popup = AppealsPopup::create([self = Weak<AdminPanelPopup>(this)](int open) {
                if (auto owner = self.lock()) owner->appealsWaiting(open);
            }))
            popup->show();
    }
    // 탭 하나의 데이터가 도착했음을 표시하고 화면을 다시 그린다.
    void loaded(int tab, std::string const& detail) {
        m_loaded[tab] = true;
        if (!detail.empty()) m_error = detail;
        render();
    }
    // 모든 탭의 데이터를 서버에서 다시 받는다. generation 이 다르면(그사이 새로 고침됨) 응답을 무시한다.
    void load() {
        int const generation = ++m_generation;
        m_loaded = {};
        m_error.clear();
        render();
        Service::get().fetchReports(
            [self = Weak<AdminPanelPopup>(this), generation](std::vector<ReportedPlayer> rows, std::string detail) {
                auto owner = self.lock();
                if (!owner || owner->m_generation != generation) return;
                owner->m_reports = std::move(rows);
                owner->loaded(kReports, detail);
            });
        Service::get().fetchBans(
            [self = Weak<AdminPanelPopup>(this), generation](std::vector<BanInfo> rows, std::string detail) {
                auto owner = self.lock();
                if (!owner || owner->m_generation != generation) return;
                owner->m_bans = std::move(rows);
                owner->loaded(kBans, detail);
            });
        Service::get().fetchReportLog(
            [self = Weak<AdminPanelPopup>(this), generation](std::vector<ReportedPlayer> rows, std::string detail) {
                auto owner = self.lock();
                if (!owner || owner->m_generation != generation) return;
                owner->m_log = std::move(rows);
                owner->loaded(kLog, detail);
            });
        Service::get().fetchReportBans(
            [self = Weak<AdminPanelPopup>(this), generation](std::vector<BanInfo> rows, std::string detail) {
                auto owner = self.lock();
                if (!owner || owner->m_generation != generation) return;
                owner->m_reportBans = std::move(rows);
                owner->loaded(kReportBans, detail);
            });
        Service::get().fetchModLog(
            [self = Weak<AdminPanelPopup>(this), generation](std::vector<ModLogEntry> rows, std::string detail) {
                auto owner = self.lock();
                if (!owner || owner->m_generation != generation) return;
                owner->m_modLog = std::move(rows);
                owner->loaded(kModLog, detail);
            });
    }
    // Mail 버튼에 읽지 않은 메일 수를 반영한다(제작자 계정: 수신함, 그 외: 받은 답장).
    void refreshMail() {
        Service::get().fetchAdminStatus([self = Weak<AdminPanelPopup>(this)](AdminStatus status, std::string error) {
            auto owner = self.lock();
            if (!owner || !error.empty() || !owner->m_mail) return;
            bool const isOwner = status.role == "owner";
            int const count = isOwner ? status.inbox : status.replies;
            auto text = std::string("Mail");
            if (count > 0) text += fmt::format(" {}", count);
            auto* sprite =
                ButtonSprite::create(text.c_str(), "goldFont.fnt", count > 0 ? "GJ_button_06.png" : "GJ_button_04.png");
            sprite->setScale(.4f);
            owner->m_mail->setSprite(sprite);
        });
    }
    void onStaff(CCObject*) {
        if (auto* popup = StaffPopup::create()) popup->show();
    }
    void onMail(CCObject*) {
        if (auto* popup = StaffMessagesPopup::create(Service::get().isOwner(), [self = Weak<AdminPanelPopup>(this)] {
                if (auto owner = self.lock()) {
                    owner->refreshMail();
                    if (owner->m_changed) owner->m_changed();
                }
            }))
            popup->show();
    }
    // 데이터를 다시 받고 상위 화면에도 변경을 알린다.
    void reload() {
        load();
        if (m_changed) m_changed();
    }
    // 현재 탭의 목록을 다시 그린다. 탭 이름에는 건수를 붙이고, Bans 계열 탭에는 검색 필터를 적용하며,
    // 한 페이지 kPage 건씩 탭 종류별 행 모양(Mod Log / 신고 / 밴)을 그린다.
    void render() {
        m_rows->removeAllChildrenWithCleanup(true);
        paintChoice(m_tabs[kReports],
                    m_loaded[kReports] ? fmt::format("Reports ({})", m_reports.size()).c_str() : "Reports",
                    m_tab == kReports, 78.f);
        paintChoice(m_tabs[kBans], m_loaded[kBans] ? fmt::format("Bans ({})", m_bans.size()).c_str() : "Bans",
                    m_tab == kBans, 78.f);
        paintChoice(m_tabs[kLog], "Cases", m_tab == kLog, 78.f);
        paintChoice(m_tabs[kReportBans],
                    m_loaded[kReportBans] ? fmt::format("R.Bans ({})", m_reportBans.size()).c_str() : "R.Bans",
                    m_tab == kReportBans, 78.f);
        paintChoice(m_tabs[kModLog], "Mod Log", m_tab == kModLog, 78.f);
        bool const banTab = m_tab == kBans || m_tab == kReportBans;
        auto const& banList = m_tab == kReportBans ? m_reportBans : m_bans;
        if (m_search) {
            m_search->setVisible(banTab);
            m_search->setEnabled(banTab);
            if (!banTab) m_search->defocus();
        }
        m_shown.clear();
        if (banTab)
            for (int i = 0; i < static_cast<int>(banList.size()); ++i)
                if (banMatches(banList[i])) m_shown.push_back(i);
        if (m_found) {
            m_found->setVisible(banTab && m_loaded[m_tab] && !banList.empty());
            m_found->setString((m_query.empty() ? fmt::format("{} active", banList.size())
                                                : fmt::format("{} of {} found", m_shown.size(), banList.size()))
                                   .c_str());
            m_found->limitLabelWidth(160.f, .4f, .2f);
        }
        int const count = static_cast<int>(banTab             ? m_shown.size()
                                           : m_tab == kLog    ? m_log.size()
                                           : m_tab == kModLog ? m_modLog.size()
                                                              : m_reports.size());
        int const pages = std::max(1, (count + kPage - 1) / kPage);
        m_page = std::clamp(m_page, 0, pages - 1);
        m_pageLabel->setString(fmt::format("{} / {}", m_page + 1, pages).c_str());
        enabled(m_previous, m_page > 0);
        enabled(m_next, m_page + 1 < pages);
        m_status->setVisible(!m_loaded[m_tab] || count == 0);
        m_status->setString(!m_loaded[m_tab]             ? "Loading..."
                            : !m_error.empty()           ? m_error.c_str()
                            : banTab && !banList.empty() ? (m_tab == kBans ? "No active ban matches the search"
                                                                           : "No active report ban matches the search")
                            : m_tab == kBans             ? "No active bans"
                            : m_tab == kReportBans       ? "No active report bans"
                            : m_tab == kLog              ? "No handled reports yet"
                            : m_tab == kModLog           ? "No staff actions logged yet"
                                                         : "No open reports");
        m_status->limitLabelWidth(380.f, .5f, .2f);
        if (!m_loaded[m_tab]) return;
        auto* actions = menu(m_rows);
        auto const now = Service::get().serverNow();
        for (int row = 0; row < kPage; ++row) {
            int const index = m_page * kPage + row;
            if (index >= count) break;
            float const y = banTab ? kBanRowTop - row * kBanRowPitch : 203.f - row * 35.f;
            auto* cell = panel(m_rows, {16.f, y - 15.f}, {408.f, 30.f}, false);
            if (m_tab == kModLog) {
                auto const& entry = m_modLog[index];
                auto* hit = CCSprite::create();
                hit->setContentSize({408.f, 30.f});
                auto* open = CCMenuItemSpriteExtra::create(hit, this, menu_selector(AdminPanelPopup::onModLog));
                open->setTag(index);
                open->setPosition({220.f, y});
                open->m_scaleMultiplier = 1.02f;
                actions->addChild(open);
                auto* who = label(cell, entry.actorName.empty() ? std::string("Admin") : entry.actorName, {8.f, 20.f},
                                  .34f, 110.f, staffRoleColor(entry.role), true);
                float x = who->getPositionX() + who->getScaledContentSize().width + 5.f;
                auto* verb = label(cell, modLogVerb(entry), {x, 20.f}, .4f, 160.f, kMuted, true, "chatFont.fnt");
                x += verb->getScaledContentSize().width + 5.f;
                if (!entry.target.empty() || entry.levelId > 0)
                    label(cell, entry.targetName.empty() ? std::string("Player") : entry.targetName, {x, 20.f}, .32f,
                          std::max(20.f, 330.f - x), ccWHITE, true);
                rightLabel(cell, agoText(now - entry.at), {400.f, 20.f}, .36f, 70.f, kMuted);
                auto line = modLogDetail(entry);
                auto const& reason = entry.action == "broadcast" ? entry.message : entry.reason;
                if (!reason.empty())
                    line += (line.empty() ? "" : "  |  ") + std::string(asciiOnly(reason) ? reason : "(tap to read)");
                label(cell, line.empty() ? std::string("-") : line, {8.f, 8.f}, .36f, 392.f,
                      entry.action == "ban" || entry.action == "ban_edit" ? kAlert : kIce, true, "chatFont.fnt");
            } else if (m_tab == kReports || m_tab == kLog) {
                auto const& target = m_tab == kLog ? m_log[index] : m_reports[index];
                label(cell, target.name, {8.f, 20.f}, .4f, 200.f, ccWHITE, true);
                std::string reason =
                    target.reports.empty() ? std::string() : reportReasonName(target.reports.front().reason);
                if (m_tab == kReports) {
                    label(cell,
                          fmt::format("{} report{}  |  {}  |  {}", target.count, target.count == 1 ? "" : "s", reason,
                                      agoText(now - target.latestAt)),
                          {8.f, 8.f}, .4f, 270.f, kIce, true, "chatFont.fnt");
                    if (target.ban)
                        rightLabel(cell, "BANNED", {340.f, 15.f}, .45f, 70.f, kAlert, "bigFont.fnt")->setScale(.32f);
                } else {
                    bool const banned = target.state == "banned";
                    label(cell,
                          fmt::format("{}  |  {} report{}  |  {}  |  {}", target.outcome, target.count,
                                      target.count == 1 ? "" : "s", reason, agoText(now - target.handledAt)),
                          {8.f, 8.f}, .4f, 300.f, banned ? kAlert : ccc3(130, 255, 165), true, "chatFont.fnt");
                }
                auto* open = button(actions, this, menu_selector(AdminPanelPopup::onOpen),
                                    m_tab == kLog ? "View" : "Open", {392.f, y}, .45f);
                open->setTag(index);
            } else {
                int const source = m_shown[static_cast<size_t>(index)];
                auto const& ban = banList[static_cast<size_t>(source)];
                auto* nameLabel = createCosmeticLabel(ban.name.c_str(), "bigFont.fnt");
                nameLabel->setScale(std::min(.4f, 190.f / std::max(1.f, nameLabel->getContentSize().width)));
                auto* name =
                    CCMenuItemSpriteExtra::create(nameLabel, this, menu_selector(AdminPanelPopup::onBanProfile));
                name->setAnchorPoint({0.f, .5f});
                name->setPosition({24.f, y + 5.f});
                name->setTag(source);
                actions->addChild(name);
                label(cell, banEndText(ban) + (ban.reason.empty() ? std::string() : "  |  " + ban.reason), {8.f, 8.f},
                      .4f, 280.f, ban.permanent ? kAlert : kIce, true, "chatFont.fnt");
                auto* edit = button(actions, this, menu_selector(AdminPanelPopup::onEdit), "Edit", {330.f, y}, .42f);
                edit->setTag(source);
                auto* lift = button(actions, this, menu_selector(AdminPanelPopup::onUnban),
                                    m_tab == kReportBans ? "Lift" : "Unban", {390.f, y}, .42f, "GJ_button_04.png");
                lift->setTag(source);
            }
        }
    }
    // Mod Log 행을 누르면 항목 상세 팝업을 연다.
    void onModLog(CCObject* sender) {
        int const index = sender->getTag();
        if (index < 0 || index >= static_cast<int>(m_modLog.size())) return;
        if (auto* popup = ModLogEntryPopup::create(m_modLog[index])) popup->show();
    }
    void onTab(CCObject* sender) {
        int const tab = std::clamp(sender->getTag(), 0, 4);
        if (tab == m_tab) return;
        m_tab = tab;
        m_page = 0;
        render();
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
    void onRefresh(CCObject*) { load(); }
    void onBroadcast(CCObject*) {
        if (auto* popup = BroadcastSendPopup::create()) popup->show();
    }
    void onDodges(CCObject*) {
        if (auto* popup = DodgeLogPopup::create()) popup->show();
    }
    // 신고 대상 상세 팝업을 연다. 처리 후에는 목록을 다시 받는다.
    void onOpen(CCObject* sender) {
        int const index = sender->getTag();
        auto const& list = m_tab == kLog ? m_log : m_reports;
        if (index < 0 || index >= static_cast<int>(list.size())) return;
        if (auto* popup = AdminTargetPopup::create(
                list[index],
                [self = Weak<AdminPanelPopup>(this)] {
                    if (auto owner = self.lock()) owner->reload();
                },
                m_tab == kLog))
            popup->show();
    }
    // 현재 탭에 해당하는 밴 목록(일반 / 신고 금지)
    std::vector<BanInfo> const& shownBans() const { return m_tab == kReportBans ? m_reportBans : m_bans; }
    void onBanProfile(CCObject* sender) {
        int const index = sender->getTag();
        if (index < 0 || index >= static_cast<int>(shownBans().size())) return;
        auto const& ban = shownBans()[index];
        showPlayer(moderationProfile(ban.uid, ban.name, ban.accountId));
    }
    // 밴 수정 팝업을 연다.
    void onEdit(CCObject* sender) {
        int const index = sender->getTag();
        if (index < 0 || index >= static_cast<int>(shownBans().size())) return;
        auto const& ban = shownBans()[index];
        if (auto* popup = BanEditPopup::create(
                ban.uid, ban.name, ban.accountId, ban, ban.reason,
                [self = Weak<AdminPanelPopup>(this)] {
                    if (auto owner = self.lock()) owner->reload();
                },
                m_tab == kReportBans))
            popup->show();
    }
    // 확인창을 거쳐 밴 또는 신고 금지를 해제한다.
    void onUnban(CCObject* sender) {
        int const index = sender->getTag();
        if (index < 0 || index >= static_cast<int>(shownBans().size())) return;
        auto const ban = shownBans()[index];
        bool const report = m_tab == kReportBans;
        createQuickPopup(report ? "Lift Report Ban?" : "Unban?",
                         (report ? fmt::format("Let <cy>{}</c> report players again now?", ban.name)
                                 : fmt::format("End the ban of <cy>{}</c> now?", ban.name))
                             .c_str(),
                         "Cancel", report ? "Lift" : "Unban",
                         [self = Weak<AdminPanelPopup>(this), uid = ban.uid, report](auto*, bool yes) {
                             if (!yes) return;
                             auto done = [self, report](bool ok, std::string detail) {
                                 if (!ok) {
                                     error(detail);
                                     return;
                                 }
                                 Notification::create(report ? "Report ban lifted" : "Ban lifted",
                                                      NotificationIcon::Success)
                                     ->show();
                                 if (auto owner = self.lock()) owner->reload();
                             };
                             if (report)
                                 Service::get().liftReportBan(uid, std::move(done));
                             else
                                 Service::get().liftBan(uid, std::move(done));
                         });
    }

public:
    static AdminPanelPopup* create(std::function<void()> changed) {
        auto* result = new AdminPanelPopup;
        if (result->init(std::move(changed))) {
            result->autorelease();
            return result;
        }
        delete result;
        return nullptr;
    }
};

// 내 신고 목록 팝업. 최근 30일간 내가 신고한 플레이어와 처리 결과(대기, 밴, 밴 없음, 철회)를 보여 주고
// 대기 중인 신고는 철회(Withdraw)할 수 있다.
class MyReportsPopup final : public ::versus::DeferredPopup {
    static constexpr int kPage = 5;
    std::vector<MyReport> m_reports;
    int m_page = 0;
    bool m_loaded = false, m_busy = false;
    std::string m_error;
    CCNode* m_rows = nullptr;
    CCLabelBMFont* m_status = nullptr;
    CCLabelBMFont* m_pageLabel = nullptr;
    CCMenuItemSpriteExtra *m_previous = nullptr, *m_next = nullptr;
    bool init() override {
        if (!Popup::init(440.f, 290.f, "GJ_square02.png")) return false;
        setTitle("My Reports", "bigFont.fnt", .62f);
        label(m_mainLayer, "Players you reported and the admin's answers. Results stay here for 30 days.",
              {220.f, 246.f}, .42f, 400.f, kMuted, false, "chatFont.fnt");
        m_rows = CCNode::create();
        m_mainLayer->addChild(m_rows);
        m_status = label(m_mainLayer, "Loading...", {220.f, 140.f}, .5f, 380.f, kIce, false, "chatFont.fnt");
        m_previous = button(m_buttonMenu, this, menu_selector(MyReportsPopup::onPrevious), "<", {170.f, 24.f}, .45f);
        m_next = button(m_buttonMenu, this, menu_selector(MyReportsPopup::onNext), ">", {270.f, 24.f}, .45f);
        m_pageLabel = label(m_mainLayer, "1 / 1", {220.f, 24.f}, .33f, 70.f, kIce);
        iconButton(m_buttonMenu, this, menu_selector(MyReportsPopup::onRefresh), "GJ_updateBtn_001.png", {408.f, 26.f},
                   28.f);
        load();
        return true;
    }
    void received(std::vector<MyReport> reports, std::string detail) {
        m_busy = false;
        m_loaded = true;
        m_error = std::move(detail);
        if (m_error.empty()) m_reports = std::move(reports);
        render();
    }
    void load() {
        m_loaded = false;
        render();
        Service::get().fetchMyReports(
            [self = Weak<MyReportsPopup>(this)](std::vector<MyReport> reports, std::string detail) {
                if (auto owner = self.lock()) owner->received(std::move(reports), std::move(detail));
            });
    }
    // 신고 상태를 표시 문구와 색으로 바꾼다.
    static std::pair<std::string, ccColor3B> stateText(MyReport const& report) {
        if (report.state == "open") return {"OPEN - waiting for the admin", ccc3(255, 221, 120)};
        if (report.state == "banned") {
            auto text = report.outcome.empty() ? std::string("Banned") : report.outcome;
            if (report.banUntil > 0) text += " (until " + localDateTime(report.banUntil) + ")";
            return {text, kAlert};
        }
        if (report.state == "withdrawn") return {"Withdrawn", kMuted};
        return {"Checked - no ban", ccc3(130, 255, 165)};
    }
    void render() {
        m_rows->removeAllChildrenWithCleanup(true);
        int const count = static_cast<int>(m_reports.size());
        int const pages = std::max(1, (count + kPage - 1) / kPage);
        m_page = std::clamp(m_page, 0, pages - 1);
        m_pageLabel->setString(fmt::format("{} / {}", m_page + 1, pages).c_str());
        enabled(m_previous, m_page > 0);
        enabled(m_next, m_page + 1 < pages);
        m_status->setVisible(!m_loaded || !m_error.empty() || count == 0);
        m_status->setString(!m_loaded          ? "Loading..."
                            : !m_error.empty() ? m_error.c_str()
                                               : "You have not reported anyone in the last 30 days.");
        m_status->limitLabelWidth(380.f, .5f, .2f);
        if (!m_loaded || !m_error.empty()) return;
        auto* actions = menu(m_rows);
        auto const now = Service::get().serverNow();
        for (int row = 0; row < kPage; ++row) {
            int const index = m_page * kPage + row;
            if (index >= count) break;
            auto const& report = m_reports[index];
            float const y = 211.f - row * 37.f;
            auto* cell = panel(m_rows, {16.f, y - 17.f}, {408.f, 34.f}, false);
            label(cell, report.targetName.empty() ? std::string("Player") : report.targetName, {8.f, 25.f}, .36f, 150.f,
                  ccWHITE, true);
            rightLabel(cell, reportReasonName(report.reason) + "  |  " + agoText(now - report.createdAt), {400.f, 25.f},
                       .4f, 200.f, kMuted);
            auto const [text, color] = stateText(report);
            std::string line = text;
            if (!report.note.empty())
                line += "  |  Admin: " + report.note;
            else if (report.state == "open" && !report.detail.empty())
                line += "  |  " + report.detail;
            label(cell, line, {8.f, 10.f}, .4f, report.state == "open" ? 320.f : 392.f, color, true, "chatFont.fnt");
            if (report.state == "open") {
                auto* withdraw = button(actions, this, menu_selector(MyReportsPopup::onWithdraw), "Withdraw",
                                        {388.f, y - 7.f}, .32f, "GJ_button_04.png");
                withdraw->setTag(index);
                enabled(withdraw, !m_busy);
            }
        }
    }
    // 확인창을 거쳐 대기 중인 신고를 철회하고, 서버가 돌려준 목록으로 갱신한다.
    void onWithdraw(CCObject* sender) {
        int const index = sender->getTag();
        if (m_busy || index < 0 || index >= static_cast<int>(m_reports.size())) return;
        auto const report = m_reports[index];
        createQuickPopup(
            "Withdraw Report",
            fmt::format("Withdraw your report on <cy>{}</c>?",
                        plainAlertText(report.targetName.empty() ? std::string("this player") : report.targetName))
                .c_str(),
            "Keep", "Withdraw", [self = Weak<MyReportsPopup>(this), id = report.id](auto*, bool yes) {
                auto owner = self.lock();
                if (!yes || !owner || owner->m_busy) return;
                owner->m_busy = true;
                owner->render();
                Service::get().withdrawReport(id, [self](std::vector<MyReport> reports, std::string detail) {
                    auto owner = self.lock();
                    if (!owner) return;
                    if (!detail.empty()) {
                        owner->m_busy = false;
                        owner->render();
                        error(detail);
                        return;
                    }
                    Notification::create("Report withdrawn", NotificationIcon::Success)->show();
                    owner->received(std::move(reports), {});
                });
            });
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
    CREATE_FUNC(MyReportsPopup);
};

// 레이팅 환급/조정 알림창. 관리자가 직접 조정한 레이팅과, 취소된 경기나 도지 페널티 해제로 돌려받은 레이팅을
// 나눠서 합계와 최대 4~5건의 내역, 사유와 함께 보여 준다.
void showRatingRefunds(std::vector<RatingRefund> const& refunds) {
    if (refunds.empty()) return;
    std::vector<RatingRefund> adjustments, given;
    for (auto const& refund : refunds) (refund.opponent == "Rating adjustment" ? adjustments : given).push_back(refund);
    if (!adjustments.empty()) {
        double change = 0.;
        for (auto const& entry : adjustments) change += entry.amount;
        std::string text =
            fmt::format("An administrator changed your rating: <c{}>{:+.1f}</c>.", change >= 0. ? 'g' : 'r', change);
        size_t const shown = std::min<size_t>(adjustments.size(), 4);
        for (size_t i = 0; i < shown; ++i) {
            auto const& entry = adjustments[i];
            text += fmt::format("\n<c{}>{:+.1f}</c>  {}", entry.amount >= 0. ? 'g' : 'r', entry.amount,
                                entry.reason.empty() ? std::string() : plainAlertText(entry.reason));
        }
        if (adjustments.size() > shown) text += fmt::format("\n+{} more", adjustments.size() - shown);
        FLAlertLayer::create(nullptr, "Rating Changed", text, "OK", nullptr, 380.f)->show();
    }
    if (given.empty()) return;
    auto const& refunds_ = given;
    double total = 0.;
    for (auto const& refund : refunds_) total += refund.amount;
    std::string body = fmt::format("<cg>+{:.1f}</c> rating was given back to you.\n", total);
    size_t const shown = std::min<size_t>(refunds_.size(), 5);
    bool sameReason = true;
    for (auto const& refund : refunds_) sameReason = sameReason && refund.reason == refunds_.front().reason;
    for (size_t i = 0; i < shown; ++i) {
        auto const& refund = refunds_[i];
        auto const when = refund.playedAt > 0 ? " (" + localDateTime(refund.playedAt) + ")" : std::string();
        if (refund.opponent == "Dodge penalty")
            body += fmt::format("\n<cg>+{:.1f}</c>  <cy>dodge penalty</c> removed{}", refund.amount, when);
        else
            body += fmt::format("\n<cg>+{:.1f}</c>  vs <cy>{}</c>{}", refund.amount,
                                plainAlertText(refund.opponent.empty() ? std::string("an opponent") : refund.opponent),
                                when);
        if (!sameReason && !refund.reason.empty()) body += "\n<cy>Reason:</c> " + plainAlertText(refund.reason);
    }
    if (refunds_.size() > shown) body += fmt::format("\n+{} more", refunds_.size() - shown);
    if (sameReason && !refunds_.front().reason.empty())
        body += "\n\n<cy>Reason:</c> " + plainAlertText(refunds_.front().reason);
    FLAlertLayer::create(nullptr, "Rating Restored", body, "OK", nullptr, 380.f)->show();
}

// 내 신고의 처리 결과 알림창(밴 되었는지, 밴 없음인지, 관리자 메모). 최대 4건까지 보여 준다.
void showReportResults(std::vector<ReportNotice> const& notices) {
    if (notices.empty()) return;
    std::string body;
    size_t const shown = std::min<size_t>(notices.size(), 4);
    for (size_t i = 0; i < shown; ++i) {
        auto const& notice = notices[i];
        auto const name = notice.targetName.empty() ? std::string("A player") : notice.targetName;
        if (!body.empty()) body += "\n\n";
        if (notice.state == "banned") {
            auto outcome = notice.outcome;
            if (!outcome.empty()) outcome[0] = static_cast<char>(std::tolower(static_cast<unsigned char>(outcome[0])));
            body +=
                fmt::format("<cy>{}</c>, whom you reported, was <cr>{}</c>{}.", name, outcome,
                            notice.banUntil > 0 ? " (until " + localDateTime(notice.banUntil) + ")" : std::string());
        } else
            body += fmt::format("Your report on <cy>{}</c> was checked. No ban this time.", name);
        if (!notice.note.empty()) body += "\n<cy>Admin:</c> " + plainAlertText(notice.note);
    }
    if (notices.size() > shown) body += fmt::format("\n\n+{} more", notices.size() - shown);
    FLAlertLayer::create(nullptr, "Report Result", body, "OK", nullptr, 360.f)->show();
}
