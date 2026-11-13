// 스태프(관리자) 기능의 UI 모음.
// - 역할 이름/색, 모더레이션 로그 항목의 문구 변환
// - ModLogEntryPopup: 로그 항목 상세, StaffWritePopup: 스태프 메일 작성창
// - StaffMessagePopup / StaffMessagesPopup / StaffRecipientPopup: 제작자 계정과 서브 관리자 사이의 메일함
// - StaffPopup: 서브 관리자 임명/해임, StaffRulesPopup: 스태프 규칙 안내, BanHistoryPopup: 밴 이력
// - StaffMapsPopup: 랭크에 나오지 않게 뺀 맵(레벨) 목록과 밴/해제 (1.0.8)
// Service 구현에 포함되는 파일이며 서버 통신은 Service::get() 을 통해 한다.
// 역할 색상 상수: 제작자 계정은 금색, 서브 관리자는 파란색.
constexpr ccColor3B kOwnerGold = {255, 214, 110};
constexpr ccColor3B kModBlue = {110, 190, 255};

// 서버가 주는 role 문자열을 화면에 표시할 이름으로 바꾼다.
std::string staffRoleName(std::string const& role) {
    if (role == "owner") return "Tipp7";
    if (role == "moderator") return "Sub-admin";
    if (role == "system") return "System";
    return "Admin";
}
// 역할별 표시 색. 그 외 역할은 회색.
ccColor3B staffRoleColor(std::string const& role) {
    if (role == "owner") return kOwnerGold;
    if (role == "moderator") return kModBlue;
    return kMuted;
}
// 밴 기간(ms)을 문구로 바꾼다. 0 은 영구 밴이고, 표에 없는 값은 일반 기간 문구로 표시한다.
std::string banLengthText(int64_t durationMs) {
    if (durationMs == 0) return "Permanent";
    for (auto const& [name, length] : kBanDurations)
        if (length == durationMs) return name;
    return durationMs > 0 ? spanText(durationMs) : std::string();
}
// 모더레이션 로그 한 줄에 쓰는 동사구("~가 ○○를 밴함" 형태의 서술부)
std::string modLogVerb(ModLogEntry const& entry) {
    auto const& a = entry.action;
    if (a == "ban") return "banned";
    if (a == "ban_edit") return "changed the ban of";
    if (a == "unban") return "unbanned";
    if (a == "report_ban") return "report-banned";
    if (a == "report_ban_edit") return "changed the report ban of";
    if (a == "report_unban") return "lifted the report ban of";
    if (a == "dismiss") return "checked the reports on";
    if (a == "rating") return "changed the rating of";
    if (a == "dodge") return "set the dodge stack of";
    if (a == "cancel_matches") return "canceled Ranked matches of";
    if (a == "broadcast") return "sent a broadcast";
    if (a == "staff_add") return "made a sub-admin:";
    if (a == "staff_remove") return "removed the sub-admin";
    if (a == "appeal_accept") return "accepted the ban appeal of";
    if (a == "appeal_reject") return "rejected the ban appeal of";
    if (a == "map_ban") return "banned the Ranked map";
    if (a == "map_ban_edit") return "changed the map ban of";
    if (a == "map_unban") return "put back the Ranked map";
    return a.empty() ? std::string("did") : a;
}
// 로그 항목 상세 팝업의 제목
std::string modLogTitle(ModLogEntry const& entry) {
    auto const& a = entry.action;
    if (a == "ban") return "Ban";
    if (a == "ban_edit") return "Ban Changed";
    if (a == "unban") return "Unban";
    if (a == "report_ban") return "Report Ban";
    if (a == "report_ban_edit") return "Report Ban Changed";
    if (a == "report_unban") return "Report Ban Lifted";
    if (a == "dismiss") return "Checked - No Ban";
    if (a == "rating") return "Rating Change";
    if (a == "dodge") return "Dodge Stack";
    if (a == "cancel_matches") return "Matches Canceled";
    if (a == "broadcast") return "Broadcast";
    if (a == "staff_add") return "Sub-admin Added";
    if (a == "staff_remove") return "Sub-admin Removed";
    if (a == "appeal_accept") return "Appeal Accepted";
    if (a == "appeal_reject") return "Appeal Rejected";
    if (a == "map_ban") return "Map Banned";
    if (a == "map_ban_edit") return "Map Ban Changed";
    if (a == "map_unban") return "Map Put Back";
    return "Staff Action";
}
// 로그 항목의 상세 한 줄: 밴 기간과 만료 시각, 취소/환불된 랭크 매치 수, 처리된 신고 수,
// 레이팅 변동, 도지 스택 변화 등 action 별로 다른 내용을 만든다.
std::string modLogDetail(ModLogEntry const& entry) {
    auto const& a = entry.action;
    std::string line;
    if (a == "ban" || a == "ban_edit" || a == "report_ban" || a == "report_ban_edit") {
        line = banLengthText(entry.permanent ? 0 : entry.durationMs);
        if (!entry.permanent && entry.until > 0) line += " (until " + localDateTime(entry.until) + ")";
        if (entry.canceled > 0)
            line += fmt::format("  |  {} Ranked match{} canceled, {} opponent{} refunded", entry.canceled,
                                entry.canceled == 1 ? "" : "es", entry.refunded, entry.refunded == 1 ? "" : "s");
        if (entry.reports > 0)
            line += fmt::format("  |  {} report{} closed", entry.reports, entry.reports == 1 ? "" : "s");
    } else if (a == "rating")
        line = fmt::format("{:+.1f}  (R{:.1f} > R{:.1f})", entry.amount, entry.before, entry.after);
    else if (a == "dodge")
        line = fmt::format("Stack {} > {}{}", entry.from, entry.to, entry.lifted ? ", restriction lifted" : "");
    else if (a == "cancel_matches")
        line = fmt::format("{} match{} canceled, {} opponent{} refunded", entry.canceled,
                           entry.canceled == 1 ? "" : "es", entry.refunded, entry.refunded == 1 ? "" : "s");
    else if (a == "dismiss")
        line = fmt::format("{} report{}, no ban", entry.reports, entry.reports == 1 ? "" : "s");
    else if (entry.levelId > 0)
        line = fmt::format("Level ID {}{}", entry.levelId, entry.creator.empty() ? "" : "  |  by " + entry.creator);
    return line;
}
// 첫 줄만 chars 글자(UTF-8 글자 수 기준)까지 잘라 미리보기를 만든다. 잘렸으면 "..." 를 붙인다.
std::string previewLine(std::string const& text, size_t chars) {
    auto line = text.substr(0, text.find('\n'));
    size_t count = 0, cut = line.size();
    for (size_t i = 0; i < line.size(); ++i) {
        if ((static_cast<unsigned char>(line[i]) & 0xC0) == 0x80) continue;
        if (count == chars) {
            cut = i;
            break;
        }
        ++count;
    }
    bool const more = cut < line.size() || line.size() < text.size();
    line.resize(cut);
    return more ? line + "..." : line;
}
// ASCII 문자만 있는지 확인(미리보기 글자 수와 폰트 폭 판단용)
bool asciiOnly(std::string const& text) {
    return std::all_of(text.begin(), text.end(), [](char c) { return static_cast<unsigned char>(c) < 0x80; });
}
// 시스템 폰트 텍스트를 스크롤 가능한 어두운 상자에 담아 parent 에 붙이고 반환한다.
ScrollLayer* systemTextBox(CCNode* parent, std::string const& text, CCPoint origin, CCSize size, float fontSize,
                           ccColor3B color) {
    auto* frame = NineSlice::create("square02b_001.png");
    frame->setContentSize(size);
    frame->setPosition(origin + size / 2.f);
    frame->setColor({8, 18, 44});
    frame->setOpacity(200);
    parent->addChild(frame);
    auto* scroll = ScrollLayer::create(size);
    scroll->setPosition(origin);
    parent->addChild(scroll, 2);
    auto* body =
        createSystemText(text.empty() ? std::string(" ") : text, fontSize, size.width - 16.f, kCCTextAlignmentLeft);
    body->setColor(color);
    body->setAnchorPoint({0.f, 1.f});
    float const height = std::max(size.height, body->getContentSize().height + 12.f);
    scroll->m_contentLayer->setContentSize({size.width, height});
    body->setPosition({8.f, height - 6.f});
    scroll->m_contentLayer->addChild(body);
    scroll->scrollToTop();
    return scroll;
}

// 모더레이션 로그 항목 상세 팝업. 실행자, 대상, 시각, 상세 내용과 사유(방송 메시지/신고자 메모 포함)를 보여 주고
// 대상 플레이어/스태프 프로필로 이동하는 버튼을 둔다.
class ModLogEntryPopup final : public ::versus::DeferredPopup {
    ModLogEntry m_entry;
    bool init(ModLogEntry entry) {
        if (!Popup::init(400.f, 250.f, "GJ_square02.png")) return false;
        m_entry = std::move(entry);
        setTitle(modLogTitle(m_entry).c_str(), "bigFont.fnt", .55f);
        auto const now = Service::get().serverNow();
        label(m_mainLayer,
              fmt::format("By {}  ({})", m_entry.actorName.empty() ? std::string("Admin") : m_entry.actorName,
                          staffRoleName(m_entry.role)),
              {200.f, 206.f}, .5f, 360.f, staffRoleColor(m_entry.role), false, "chatFont.fnt");
        if (!m_entry.target.empty())
            label(m_mainLayer, "Player: " + (m_entry.targetName.empty() ? std::string("Player") : m_entry.targetName),
                  {200.f, 187.f}, .5f, 360.f, ccWHITE, false, "chatFont.fnt");
        else if (m_entry.levelId > 0)
            label(m_mainLayer, "Map: " + m_entry.targetName, {200.f, 187.f}, .5f, 360.f, ccWHITE, false,
                  "chatFont.fnt");
        label(m_mainLayer, fmt::format("{}  ({})", localDateTime(m_entry.at), agoText(now - m_entry.at)),
              {200.f, 169.f}, .42f, 360.f, kMuted, false, "chatFont.fnt");
        auto const detail = modLogDetail(m_entry);
        if (!detail.empty())
            label(m_mainLayer, detail, {200.f, 151.f}, .42f, 370.f, ccc3(255, 221, 120), false, "chatFont.fnt");
        auto const text = m_entry.action == "broadcast" ? m_entry.message : m_entry.reason;
        label(m_mainLayer,
              m_entry.action == "broadcast" ? "Message"
              : m_entry.action == "dismiss" ? "Note to the reporters"
                                            : "Reason",
              {24.f, 133.f}, .4f, 200.f, kIce, true, "chatFont.fnt");
        systemTextBox(m_mainLayer, text.empty() ? std::string("(none)") : text, {20.f, 52.f}, {360.f, 72.f}, 13.f,
                      text.empty() ? ccc3(140, 150, 175) : ccWHITE);
        if (!m_entry.target.empty())
            button(m_buttonMenu, this, menu_selector(ModLogEntryPopup::onTarget), "Player", {140.f, 26.f}, .5f);
        if (!m_entry.actor.empty() && m_entry.actor != "system")
            button(m_buttonMenu, this, menu_selector(ModLogEntryPopup::onActor), "Staff", {260.f, 26.f}, .5f,
                   "GJ_button_04.png");
        return true;
    }
    void onTarget(CCObject*) { showPlayer(moderationProfile(m_entry.target, m_entry.targetName, 0)); }
    void onActor(CCObject*) { showPlayer(moderationProfile(m_entry.actor, m_entry.actorName, 0)); }

public:
    static ModLogEntryPopup* create(ModLogEntry entry) {
        auto* result = new ModLogEntryPopup;
        if (result->init(std::move(entry))) {
            result->autorelease();
            return result;
        }
        delete result;
        return nullptr;
    }
};

// 스태프 메일 작성 팝업. 아이디어/질문(제작자 계정에게 보낼 때) 또는 답장/지정 수신자 메시지를 쓴다.
// 입력은 최대 kMax 글자이며, 한글 등은 클립보드 붙여넣기로 입력한다. 전송은 submit 콜백에 맡긴다.
class StaffWritePopup final : public ::versus::DeferredPopup {
    static constexpr size_t kMax = 1000;
    // 전송 콜백: (종류, 본문, 완료 콜백). 종류는 "idea"/"question".
    using Submit = std::function<void(std::string kind, std::string text, Done done)>;
    bool m_answer = false;
    std::string m_title;
    int m_kind = -1;
    std::array<CCMenuItemSpriteExtra*, 2> m_kinds{};
    TextInput* m_input = nullptr;
    ScrollLayer* m_scroll = nullptr;
    CCSprite* m_preview = nullptr;
    CCLabelBMFont* m_count = nullptr;
    CCLabelBMFont* m_status = nullptr;
    CCMenuItemSpriteExtra* m_send = nullptr;
    std::string m_text;
    bool m_sending = false;
    Submit m_submit;
    // 팝업이 사라질 때 입력창 포커스를 해제한다.
    void onExit() override {
        if (m_input) m_input->defocus();
        Popup::onExit();
    }
    // UI 구성. answer(또는 제목 지정) 모드에서는 종류 선택 없이 본문만 쓴다.
    bool init(bool answer, std::string context, std::string initial, Submit submit, std::string title) {
        if (!Popup::init(420.f, 290.f, "GJ_square02.png")) return false;
        m_answer = answer || !title.empty();
        m_submit = std::move(submit);
        m_text = std::move(initial);
        m_title = std::move(title);
        setTitle(!m_title.empty() ? m_title : m_answer ? "Answer" : "Message Tipp7", "goldFont.fnt", .7f);
        label(m_mainLayer, context, {210.f, 246.f}, .44f, 390.f, kIce, false, "chatFont.fnt");
        float inputY = 224.f;
        if (!m_answer) {
            static constexpr std::array<char const*, 2> names = {"Idea", "Question"};
            for (int i = 0; i < 2; ++i) {
                m_kinds[i] =
                    button(m_buttonMenu, this, menu_selector(StaffWritePopup::onKind), "", {160.f + i * 100.f, 224.f});
                m_kinds[i]->setTag(i);
                paintChoice(m_kinds[i], names[i], false, 90.f);
            }
            inputY = 196.f;
        }
        m_input = TextInput::create(300.f / .7f, m_answer && m_title.empty() ? "Type your answer (English)"
                                                                             : "Type a message (English)");
        m_input->setCommonFilter(CommonFilter::Any);
        m_input->setMaxCharCount(static_cast<int>(kMax));
        m_input->setScale(.7f);
        m_input->setPosition({176.f, inputY});
        styleInput(m_input);
        m_input->setCallback([self = Weak<StaffWritePopup>(this)](std::string const& text) {
            if (auto owner = self.lock()) {
                owner->m_text = text;
                owner->refresh();
            }
        });
        m_mainLayer->addChild(m_input, 5);
        button(m_buttonMenu, this, menu_selector(StaffWritePopup::onPaste), "Paste", {370.f, inputY}, .5f,
               "GJ_button_04.png");
        label(m_mainLayer, "Korean or other languages: type in any app, copy it, then tap Paste.",
              {210.f, inputY - 22.f}, .4f, 390.f, kMuted, false, "chatFont.fnt");
        float const top = inputY - 34.f, bottom = 56.f;
        auto* frame = NineSlice::create("square02b_001.png");
        frame->setContentSize({380.f, top - bottom});
        frame->setPosition({210.f, (top + bottom) / 2.f});
        frame->setColor({8, 18, 44});
        frame->setOpacity(200);
        m_mainLayer->addChild(frame);
        m_scroll = ScrollLayer::create({380.f, top - bottom});
        m_scroll->setPosition({20.f, bottom});
        m_mainLayer->addChild(m_scroll, 4);
        m_preview = createSystemText(" ", 13.f, 364.f, kCCTextAlignmentLeft);
        m_preview->setAnchorPoint({0.f, 1.f});
        m_scroll->m_contentLayer->addChild(m_preview);
        m_count = label(m_mainLayer, "", {398.f, 46.f}, .38f, 90.f, kMuted, false, "chatFont.fnt");
        m_count->setAnchorPoint({1.f, .5f});
        m_status = label(m_mainLayer, "", {24.f, 46.f}, .38f, 280.f, kAlert, true, "chatFont.fnt");
        button(m_buttonMenu, this, menu_selector(StaffWritePopup::onCancel), "Cancel", {140.f, 24.f}, .6f,
               "GJ_button_06.png");
        m_send = button(m_buttonMenu, this, menu_selector(StaffWritePopup::onSend), "Send", {280.f, 24.f}, .6f);
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
    // 미리보기, 글자 수, 전송 버튼 활성 상태를 갱신한다. 비었거나 길이 초과, 전송 중, 종류 미선택이면 전송 불가.
    void refresh() {
        auto const text = trimmed(m_text);
        auto const length = utf8Length(text);
        setSystemText(m_preview, text.empty() ? std::string("(preview)") : text);
        m_preview->setColor(text.empty() ? ccc3(140, 150, 175) : ccWHITE);
        auto const size = m_scroll->getContentSize();
        float const height = std::max(size.height, m_preview->getContentSize().height + 12.f);
        m_scroll->m_contentLayer->setContentSize({size.width, height});
        m_preview->setPosition({8.f, height - 6.f});
        m_count->setString(fmt::format("{} / {}", length, kMax).c_str());
        m_count->setColor(length > kMax ? kAlert : kMuted);
        enabled(m_send, !text.empty() && length <= kMax && !m_sending && (m_answer || m_kind >= 0));
    }
    void say(std::string const& text) {
        m_status->setString(text.c_str());
        m_status->limitLabelWidth(280.f, .38f, .2f);
    }
    void onKind(CCObject* sender) {
        if (m_sending) return;
        m_kind = std::clamp(sender->getTag(), 0, 1);
        static constexpr std::array<char const*, 2> names = {"Idea", "Question"};
        for (int i = 0; i < 2; ++i) paintChoice(m_kinds[i], names[i], i == m_kind, 90.f);
        refresh();
    }
    // 클립보드 내용으로 본문을 바꾼다.
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
    // 전송한다. 전송 중에는 중복 전송과 팝업 닫기를 막고, 실패하면 오류를 상태줄에 보여 준다.
    void onSend(CCObject*) {
        auto const text = trimmed(m_text);
        if (m_sending || text.empty() || utf8Length(text) > kMax || (!m_answer && m_kind < 0) || !m_submit) return;
        m_input->defocus();
        m_sending = true;
        say("");
        refresh();
        m_submit(m_kind == 1 ? "question" : "idea", text,
                 [self = Weak<StaffWritePopup>(this)](bool ok, std::string detail) {
                     auto owner = self.lock();
                     if (!owner) return;
                     owner->m_sending = false;
                     if (!ok) {
                         owner->refresh();
                         owner->say(detail.empty() ? "Sending failed. Try again." : detail);
                         return;
                     }
                     Notification::create(!owner->m_title.empty() ? "Message sent"
                                          : owner->m_answer       ? "Answer sent"
                                                                  : "Message sent to Tipp7",
                                          NotificationIcon::Success)
                         ->show();
                     owner->onClose(nullptr);
                 });
    }
    // 전송 중에는 닫히지 않는다.
    void onClose(CCObject* sender) override {
        if (!m_sending) Popup::onClose(sender);
    }

public:
    static StaffWritePopup* create(bool answer, std::string context, std::string initial, Submit submit,
                                   std::string title = {}) {
        auto* result = new StaffWritePopup;
        if (result->init(answer, std::move(context), std::move(initial), std::move(submit), std::move(title))) {
            result->autorelease();
            return result;
        }
        delete result;
        return nullptr;
    }
};

// 스태프 메일 UI 공용 도우미와 색상
namespace staffmail {
constexpr ccColor3B kIdea = {95, 210, 185};
constexpr ccColor3B kQuestion = {175, 150, 255};
constexpr ccColor3B kAnswer = {130, 230, 160};
constexpr ccColor3B kNew = {130, 255, 165};
constexpr ccColor3B kPreview = {175, 188, 212};

// 제작자 계정이 보낸 메시지인지 여부
bool fromTipp7(StaffMessage const& message) {
    return message.kind == "tipp7";
}
// 새 항목(NEW) 여부. 제작자 쪽은 읽지 않은 받은 메시지, 서브 관리자 쪽은 확인하지 않은 답장/메시지.
bool fresh(StaffMessage const& message, bool owner) {
    return owner ? !fromTipp7(message) && !message.read
                 : (fromTipp7(message) || !message.reply.empty()) && !message.replySeen;
}
// 삭제할 수 있는 상태인지. 제작자 쪽은 읽은 메시지, 서브 관리자 쪽은 답장 전이거나 답장을 확인한 메시지.
bool deletable(StaffMessage const& message, bool owner) {
    return owner ? fromTipp7(message) || message.read : message.repliedAt == 0 || message.replySeen;
}
// 메시지 종류 칩의 문구와 색
std::pair<std::string, ccColor3B> kindChip(StaffMessage const& message, bool answerRow) {
    if (fromTipp7(message)) return {"MESSAGE", kOwnerGold};
    if (answerRow) return {"ANSWER", kAnswer};
    return message.kind == "question" ? std::pair<std::string, ccColor3B>{"QUESTION", kQuestion}
                                      : std::pair<std::string, ccColor3B>{"IDEA", kIdea};
}
// 작은 색상 칩(배경+글자)을 그리고 너비를 반환한다.
float chip(CCNode* parent, std::string const& text, ccColor3B color, CCPoint left) {
    auto* value = CCLabelBMFont::create(text.c_str(), "bigFont.fnt");
    value->setScale(.22f);
    value->setColor(color);
    auto const size = value->getScaledContentSize();
    float const width = size.width + 10.f;
    auto* back = CCLayerColor::create(ccc4(color.r, color.g, color.b, 48), width, 12.f);
    back->setPosition({left.x, left.y - 6.f});
    parent->addChild(back);
    value->setPosition({left.x + width / 2.f, left.y});
    parent->addChild(value);
    return width;
}
// 버튼들을 centre 를 기준으로 가로 가운데 정렬한다.
void centreRow(std::vector<CCMenuItemSpriteExtra*> const& items, float centre, float y, float gap = 10.f) {
    float total = -gap;
    for (auto* item : items) total += item->getScaledContentSize().width + gap;
    float x = centre - total / 2.f;
    for (auto* item : items) {
        float const width = item->getScaledContentSize().width;
        item->setPosition({x + width / 2.f, y});
        x += width + gap;
    }
}
}

// 메시지 하나를 보여 주는 팝업. owner 가 true 면 제작자 계정 시점이다.
// 열면 읽음/확인 처리를 서버에 알리고, 복사/답변/삭제 버튼을 둔다.
class StaffMessagePopup final : public ::versus::DeferredPopup {
    static constexpr float W = 440.f, H = 320.f;
    StaffMessage m_message;
    bool m_owner = false;
    bool m_busy = false;
    std::function<void()> m_changed;
    std::function<void(std::vector<StaffMessage>)> m_deleted;
    bool init(StaffMessage message, bool owner, std::function<void()> changed,
              std::function<void(std::vector<StaffMessage>)> deleted) {
        if (!Popup::init(W, H, "GJ_square02.png")) return false;
        m_message = std::move(message);
        m_owner = owner;
        m_changed = std::move(changed);
        m_deleted = std::move(deleted);
        bool const tipp7 = staffmail::fromTipp7(m_message);
        setTitle(tipp7 ? "Message" : m_message.kind == "question" ? "Question" : "Idea", "goldFont.fnt", .7f);
        setID("versus-staff-message"_spr);
        auto* head = NineSlice::create("square02b_001.png");
        head->setContentSize({W - 32.f, 30.f});
        head->setPosition({W / 2.f, H - 62.f});
        head->setColor({20, 30, 58});
        head->setOpacity(230);
        m_mainLayer->addChild(head);
        ccColor3B const other = m_owner ? kModBlue : kOwnerGold;
        std::string const who =
            m_owner ? (tipp7 ? "To " : "From ") + m_message.senderName : std::string(tipp7 ? "From Tipp7" : "To Tipp7");
        auto* name = label(m_mainLayer, who, {28.f, H - 62.f}, .36f, 190.f, other, true, "bigFont.fnt");
        auto const [chipText, chipColor] = staffmail::kindChip(m_message, false);
        staffmail::chip(m_mainLayer, chipText, chipColor, {28.f + name->getScaledContentSize().width + 8.f, H - 62.f});
        auto const now = Service::get().serverNow();
        rightLabel(m_mainLayer,
                   fmt::format("{}  ({})", localDateTime(m_message.createdAt), agoText(now - m_message.createdAt)),
                   {W - 26.f, H - 62.f}, .4f, 190.f, kMuted);
        float const bodyTop = H - 84.f;
        if (tipp7) {
            systemTextBox(m_mainLayer, m_message.body, {16.f, 52.f}, {W - 32.f, bodyTop - 52.f}, 13.f,
                          ccc3(255, 238, 200));
            if (m_owner)
                rightLabel(m_mainLayer, m_message.replySeen ? "Seen" : "Not seen yet", {W - 22.f, 42.f}, .36f, 120.f,
                           m_message.replySeen ? staffmail::kAnswer : kMuted);
        } else {
            systemTextBox(m_mainLayer, m_message.body, {16.f, 156.f}, {W - 32.f, bodyTop - 156.f}, 13.f, ccWHITE);
            if (m_message.reply.empty()) {
                auto* waiting = CCLayerColor::create(ccc4(255, 255, 255, 10), W - 32.f, 80.f);
                waiting->setPosition({16.f, 52.f});
                m_mainLayer->addChild(waiting);
                label(m_mainLayer, m_owner ? "Not answered yet." : "Tipp7 has not answered yet.", {W / 2.f, 92.f}, .45f,
                      W - 60.f, kMuted, false, "chatFont.fnt");
            } else {
                auto* answer =
                    label(m_mainLayer, "TIPP7'S ANSWER", {20.f, 142.f}, .28f, 160.f, kOwnerGold, true, "bigFont.fnt");
                label(m_mainLayer,
                      fmt::format("{}{}", localDateTime(m_message.repliedAt),
                                  m_owner ? (m_message.replySeen ? "  |  Seen" : "  |  Not seen yet") : std::string()),
                      {answer->getPositionX() + answer->getScaledContentSize().width + 8.f, 142.f}, .38f, 220.f, kMuted,
                      true, "chatFont.fnt");
                systemTextBox(m_mainLayer, m_message.reply, {16.f, 52.f}, {W - 32.f, 80.f}, 13.f, ccc3(255, 238, 200));
            }
        }
        std::vector<CCMenuItemSpriteExtra*> actions;
        actions.push_back(
            button(m_buttonMenu, this, menu_selector(StaffMessagePopup::onCopy), "Copy", {}, .48f, "GJ_button_04.png"));
        actions.back()->setID("staff-message-copy"_spr);
        if (!m_message.reply.empty())
            actions.push_back(button(m_buttonMenu, this, menu_selector(StaffMessagePopup::onCopyAnswer), "Copy Answer",
                                     {}, .48f, "GJ_button_04.png"));
        if (m_owner && !tipp7) {
            actions.push_back(button(m_buttonMenu, this, menu_selector(StaffMessagePopup::onAnswer),
                                     m_message.reply.empty() ? "Answer" : "Edit Answer", {}, .48f, "GJ_button_01.png"));
            actions.back()->setID("staff-message-answer"_spr);
        }
        // 제작자 시점에서 서브 관리자의 메시지를 열면 읽음 처리한다.
        if (m_owner && !m_message.read && !tipp7) {
            m_message.read = true;
            Service::get().readStaffMessage(m_message.id,
                                            [changed = m_changed](std::vector<StaffMessage>, int, std::string error) {
                                                if (error.empty() && changed) changed();
                                            });
        }
        // 서브 관리자 시점에서 답장이나 제작자의 메시지를 처음 열면 확인(seen) 처리를 서버에 알린다.
        if (!m_owner && (tipp7 || !m_message.reply.empty()) && !m_message.replySeen) {
            m_message.replySeen = true;
            Service::get().acknowledgeStaffReplies({m_message.id});
            if (m_changed) m_changed();
        }
        // 삭제 가능한 상태일 때만 Delete 버튼을 둔다.
        if (staffmail::deletable(m_message, m_owner)) {
            actions.push_back(button(m_buttonMenu, this, menu_selector(StaffMessagePopup::onDelete), "Delete", {}, .48f,
                                     "GJ_button_06.png"));
            actions.back()->setID("staff-message-delete"_spr);
        }
        staffmail::centreRow(actions, W / 2.f, 26.f);
        return true;
    }
    void onCopy(CCObject*) {
        clipboard::write(m_message.body);
        Notification::create("Message copied", NotificationIcon::Success)->show();
    }
    void onCopyAnswer(CCObject*) {
        clipboard::write(m_message.reply);
        Notification::create("Answer copied", NotificationIcon::Success)->show();
    }
    // 답변 작성창을 연다. 성공하면 목록을 갱신하고 이 팝업을 닫는다.
    void onAnswer(CCObject*) {
        auto const id = m_message.id;
        if (auto *popup = StaffWritePopup::create(
                true, "To " + m_message.senderName + " - only they see it.", m_message.reply,
                [self = Weak<StaffMessagePopup>(this), id](std::string, std::string text, Done done) {
                    Service::get().replyStaffMessage(
                        id, std::move(text),
                        [self, done = std::move(done)](std::vector<StaffMessage>, int, std::string error) {
                            done(error.empty(), error);
                            if (!error.empty()) return;
                            if (auto owner = self.lock()) {
                                if (owner->m_changed) owner->m_changed();
                                owner->onClose(nullptr);
                            }
                        });
                }))
            popup->show();
    }
    // 삭제 확인 후 자신의 메일함에서만 삭제한다(상대방 쪽에는 그대로 남음).
    // 서버가 아직 반영하지 않아 목록에 남아 있으면 다시 시도하라고 알린다.
    void onDelete(CCObject*) {
        if (m_busy) return;
        auto const other = m_owner ? m_message.senderName : std::string("Tipp7");
        createQuickPopup(
            "Delete Message?",
            fmt::format("It is removed from <cy>your</c> mail only. {} still has it.", plainAlertText(other)), "Cancel",
            "Delete", [self = Weak<StaffMessagePopup>(this)](auto*, bool yes) {
                auto owner = self.lock();
                if (!yes || !owner || owner->m_busy) return;
                owner->m_busy = true;
                auto const id = owner->m_message.id;
                Service::get().deleteStaffMail(
                    {id}, [self, id](std::vector<StaffMessage> messages, int, std::string error) {
                        auto owner = self.lock();
                        if (!owner) return;
                        owner->m_busy = false;
                        if (!error.empty()) {
                            ::versus::error(error);
                            return;
                        }
                        bool const kept = std::any_of(messages.begin(), messages.end(),
                                                      [id](StaffMessage const& message) { return message.id == id; });
                        if (kept) {
                            Notification::create("Not deleted yet - try again in a moment", NotificationIcon::Warning)
                                ->show();
                            return;
                        }
                        Notification::create("Deleted", NotificationIcon::Success)->show();
                        if (owner->m_deleted) owner->m_deleted(std::move(messages));
                        owner->onClose(nullptr);
                    });
            });
    }

public:
    static StaffMessagePopup* create(StaffMessage message, bool owner, std::function<void()> changed,
                                     std::function<void(std::vector<StaffMessage>)> deleted = {}) {
        auto* result = new StaffMessagePopup;
        if (result->init(std::move(message), owner, std::move(changed), std::move(deleted))) {
            result->autorelease();
            return result;
        }
        delete result;
        return nullptr;
    }
};

// 제작자 계정이 메시지를 보낼 서브 관리자를 고르는 팝업. 목록을 서버에서 받아 탭하면 작성창을 연다.
class StaffRecipientPopup final : public ::versus::DeferredPopup {
    static constexpr float W = 320.f, H = 260.f, ListW = W - 32.f, ListH = 170.f, RowH = 34.f;
    std::vector<StaffMember> m_staff;
    std::function<void()> m_sent;
    TapScrollLayer* m_list = nullptr;
    CCLabelBMFont* m_status = nullptr;
    bool init(std::function<void()> sent) {
        if (!Popup::init(W, H, "GJ_square02.png")) return false;
        m_sent = std::move(sent);
        setTitle("Write To", "goldFont.fnt", .7f);
        auto* paper = NineSlice::create("square02b_001.png");
        paper->setContentSize({ListW + 8.f, ListH + 8.f});
        paper->setPosition({W / 2.f, 40.f + ListH / 2.f});
        paper->setColor({10, 16, 34});
        paper->setOpacity(220);
        m_mainLayer->addChild(paper);
        m_list = TapScrollLayer::create({ListW, ListH}, [self = Weak<StaffRecipientPopup>(this)](CCNode* row) {
            if (auto owner = self.lock()) owner->write(row->getTag());
        });
        m_list->setPosition({(W - ListW) / 2.f, 40.f});
        m_mainLayer->addChild(m_list, 2);
        m_status = label(m_mainLayer, "Loading...", {W / 2.f, 40.f + ListH / 2.f}, .5f, ListW - 20.f, kMuted, false,
                         "chatFont.fnt");
        m_status->setZOrder(3);
        label(m_mainLayer, "Only that sub-admin sees it.", {W / 2.f, 22.f}, .4f, W - 30.f, kMuted, false,
              "chatFont.fnt");
        Service::get().fetchStaff(
            [self = Weak<StaffRecipientPopup>(this)](std::vector<StaffMember> staff, std::string error) {
                if (auto owner = self.lock()) owner->received(std::move(staff), std::move(error));
            });
        return true;
    }
    void received(std::vector<StaffMember> staff, std::string error) {
        m_staff = std::move(staff);
        m_status->setVisible(!error.empty() || m_staff.empty());
        m_status->setString(!error.empty() ? error.c_str() : "No sub-admins yet.");
        m_status->limitLabelWidth(ListW - 20.f, .5f, .2f);
        auto* content = m_list->m_contentLayer;
        content->removeAllChildrenWithCleanup(true);
        float const height = std::max(ListH, m_staff.size() * (RowH + 5.f) + 5.f);
        content->setContentSize({ListW, height});
        for (size_t i = 0; i < m_staff.size(); ++i) {
            float const w = ListW - 8.f;
            auto* row = CCNode::create();
            row->setContentSize({w, RowH});
            row->setAnchorPoint({.5f, .5f});
            row->ignoreAnchorPointForPosition(false);
            auto* card = NineSlice::create("square02b_001.png");
            card->setContentSize({w, RowH});
            card->setPosition({w / 2.f, RowH / 2.f});
            card->setColor({22, 31, 58});
            card->setOpacity(230);
            row->addChild(card);
            auto* bar = CCLayerColor::create(ccc4(kModBlue.r, kModBlue.g, kModBlue.b, 200), 3.f, RowH - 10.f);
            bar->setPosition({6.f, 5.f});
            row->addChild(bar);
            label(row, m_staff[i].name, {16.f, RowH / 2.f}, .36f, w - 90.f, kModBlue, true, "bigFont.fnt");
            rightLabel(row, "Write  >", {w - 10.f, RowH / 2.f}, .42f, 70.f, kIce);
            row->setTag(static_cast<int>(i));
            row->setPosition({ListW / 2.f, height - 5.f - RowH / 2.f - i * (RowH + 5.f)});
            content->addChild(row);
        }
        m_list->scrollToTop();
    }
    // 고른 서브 관리자에게 보낼 메시지 작성창을 연다. 전송에 성공하면 sent 콜백을 부르고 닫는다.
    void write(int index) {
        if (index < 0 || index >= static_cast<int>(m_staff.size())) return;
        auto const member = m_staff[index];
        if (auto *popup = StaffWritePopup::create(
                true, "To " + member.name + " - only they see it (their desk shows it as new).", "",
                [self = Weak<StaffRecipientPopup>(this), uid = member.uid](std::string, std::string text, Done done) {
                    Service::get().messageStaffMember(
                        uid, std::move(text),
                        [self, done = std::move(done)](std::vector<StaffMessage>, int, std::string error) {
                            done(error.empty(), error);
                            if (!error.empty()) return;
                            if (auto owner = self.lock()) {
                                if (owner->m_sent) owner->m_sent();
                                owner->onClose(nullptr);
                            }
                        });
                },
                "Message " + member.name))
            popup->show();
    }

public:
    static StaffRecipientPopup* create(std::function<void()> sent) {
        auto* result = new StaffRecipientPopup;
        if (result->init(std::move(sent))) {
            result->autorelease();
            return result;
        }
        delete result;
        return nullptr;
    }
};

// 스태프 메일함 팝업. Inbox / Sent 두 탭이며, 제작자 계정(owner)과 서브 관리자의 시점에 따라
// 어떤 메시지가 어느 탭에 들어가는지가 달라진다. 목록은 서버에서 받아 새로 고친다.
class StaffMessagesPopup final : public ::versus::DeferredPopup {
    static constexpr float W = 460.f, H = 300.f, ListW = W - 28.f, ListH = 196.f, RowH = 46.f, Gap = 6.f;
    bool m_owner = false;
    int m_tab = 0;
    std::vector<StaffMessage> m_messages;
    std::vector<int> m_shown;
    bool m_loaded = false;
    std::string m_error;
    std::function<void()> m_changed;
    TapScrollLayer* m_list = nullptr;
    CCLabelBMFont* m_status = nullptr;
    CCSprite* m_emptyIcon = nullptr;
    std::array<CCMenuItemSpriteExtra*, 2> m_tabs{};
    // Inbox 에 넣을 메시지인지(제작자: 서브 관리자가 보낸 것, 서브 관리자: 제작자의 메시지나 답장이 있는 것)
    bool inInbox(StaffMessage const& m) const {
        return m_owner ? !staffmail::fromTipp7(m) : staffmail::fromTipp7(m) || !m.reply.empty();
    }
    // Sent 에 넣을 메시지인지(위의 반대 방향)
    bool inSent(StaffMessage const& m) const { return m_owner ? staffmail::fromTipp7(m) : !staffmail::fromTipp7(m); }
    bool init(bool owner, std::function<void()> changed) {
        if (!Popup::init(W, H, "GJ_square02.png")) return false;
        m_owner = owner;
        m_changed = std::move(changed);
        setTitle("Staff Mail", "goldFont.fnt", .75f);
        setID("versus-staff-mail"_spr);
        for (int i = 0; i < 2; ++i) {
            m_tabs[i] =
                button(m_buttonMenu, this, menu_selector(StaffMessagesPopup::onTab), "", {64.f + i * 98.f, H - 52.f});
            m_tabs[i]->setTag(i);
        }
        button(m_buttonMenu, this, menu_selector(StaffMessagesPopup::onNew), m_owner ? "Write" : "Write to Tipp7",
               {W - (m_owner ? 84.f : 112.f), H - 52.f}, .48f, "GJ_button_02.png")
            ->setID("staff-new-message"_spr);
        iconButton(m_buttonMenu, this, menu_selector(StaffMessagesPopup::onRefresh), "GJ_updateBtn_001.png",
                   {W - 30.f, H - 52.f}, 24.f);
        auto* paper = NineSlice::create("square02b_001.png");
        paper->setContentSize({ListW + 8.f, ListH + 8.f});
        paper->setPosition({W / 2.f, 34.f + ListH / 2.f});
        paper->setColor({10, 16, 34});
        paper->setOpacity(220);
        m_mainLayer->addChild(paper);
        m_list = TapScrollLayer::create({ListW, ListH}, [self = Weak<StaffMessagesPopup>(this)](CCNode* row) {
            if (auto owner = self.lock()) owner->open(row->getTag());
        });
        m_list->setPosition({(W - ListW) / 2.f, 34.f});
        m_mainLayer->addChild(m_list, 2);
        if (loadedFrame("accountBtn_messages_001.png", "GJ_GameSheet03.plist")) {
            m_emptyIcon = CCSprite::createWithSpriteFrameName("accountBtn_messages_001.png");
            m_emptyIcon->setOpacity(70);
            m_emptyIcon->setScale(.9f);
            m_emptyIcon->setPosition({W / 2.f, 34.f + ListH / 2.f + 18.f});
            m_mainLayer->addChild(m_emptyIcon, 3);
        }
        m_status = label(m_mainLayer, "Loading...", {W / 2.f, 34.f + ListH / 2.f - 16.f}, .48f, ListW - 30.f, kMuted,
                         false, "chatFont.fnt");
        m_status->setZOrder(3);
        label(m_mainLayer,
              m_owner ? "Each conversation is only between you and that sub-admin."
                      : "Only you and Tipp7 see these messages.",
              {W / 2.f, 18.f}, .38f, W - 40.f, kMuted, false, "chatFont.fnt");
        render();
        load();
        return true;
    }
    // 서버 응답을 반영한다. 오류가 있으면 기존 목록은 유지한다.
    void received(std::vector<StaffMessage> messages, std::string error) {
        m_loaded = true;
        m_error = std::move(error);
        if (m_error.empty()) m_messages = std::move(messages);
        render();
    }
    // 시점에 맞는 API(제작자: 전체 수신함, 서브 관리자: 자신의 메시지)로 목록을 다시 받는다.
    void load() {
        m_loaded = false;
        render();
        auto done = [self = Weak<StaffMessagesPopup>(this)](std::vector<StaffMessage> messages, int,
                                                            std::string error) {
            if (auto owner = self.lock()) owner->received(std::move(messages), std::move(error));
        };
        if (m_owner)
            Service::get().fetchStaffInbox(std::move(done));
        else
            Service::get().fetchStaffMessages(std::move(done));
    }
    void changed() {
        load();
        if (m_changed) m_changed();
    }
    // 목록의 한 행을 만든다: 보낸/받는 사람, 종류 칩, 경과 시간, 본문(또는 답장) 미리보기, 상태 문구.
    CCNode* row(StaffMessage const& m, int64_t now) const {
        float const w = ListW - 8.f;
        bool const isNew = staffmail::fresh(m, m_owner), tipp7 = staffmail::fromTipp7(m);
        bool const answerRow = !m_owner && m_tab == 0 && !tipp7;
        auto* node = CCNode::create();
        node->setContentSize({w, RowH});
        node->setAnchorPoint({.5f, .5f});
        node->ignoreAnchorPointForPosition(false);
        auto* card = NineSlice::create("square02b_001.png");
        card->setContentSize({w, RowH});
        card->setPosition({w / 2.f, RowH / 2.f});
        card->setColor(isNew ? ccc3(34, 52, 98) : ccc3(22, 31, 58));
        card->setOpacity(isNew ? 245 : 225);
        node->addChild(card);
        ccColor3B const other = m_owner ? kModBlue : kOwnerGold;
        auto* bar = CCLayerColor::create(ccc4(other.r, other.g, other.b, isNew ? 255 : 110), 3.f, RowH - 12.f);
        bar->setPosition({6.f, 6.f});
        node->addChild(bar);
        std::string const who =
            m_owner ? (tipp7 ? "To " + m.senderName : m.senderName) : std::string(m_tab == 1 ? "To Tipp7" : "Tipp7");
        float const top = RowH - 14.f;
        auto* name = label(node, who, {16.f, top}, .33f, 150.f, other, true, "bigFont.fnt");
        auto const [chipText, chipColor] = staffmail::kindChip(m, answerRow);
        staffmail::chip(node, chipText, chipColor, {16.f + name->getScaledContentSize().width + 7.f, top});
        rightLabel(node, agoText(now - (answerRow ? m.repliedAt : m.createdAt)), {w - 10.f, top}, .38f, 110.f, kMuted);
        auto const& text = answerRow ? m.reply : m.body;
        auto* preview =
            createSystemText(previewLine(text, asciiOnly(text) ? 72 : 34), 11.5f, 0.f, kCCTextAlignmentLeft);
        preview->setColor(isNew ? ccWHITE : staffmail::kPreview);
        preview->setAnchorPoint({0.f, .5f});
        preview->setPosition({16.f, 13.f});
        float const room = w - 16.f - 86.f;
        if (preview->getContentSize().width > room) preview->setScale(room / preview->getContentSize().width);
        node->addChild(preview);
        std::string state;
        if (m_owner)
            state = tipp7 ? (m.replySeen ? "Seen" : "Sent") : isNew ? "NEW" : m.reply.empty() ? "Read" : "Answered";
        else if (m_tab == 0)
            state = isNew ? "NEW" : "Read";
        else
            state = m.reply.empty() ? "Waiting" : isNew ? "NEW ANSWER" : "Answered";
        rightLabel(node, state, {w - 10.f, 13.f}, .38f, 90.f,
                   isNew                ? staffmail::kNew
                   : state == "Waiting" ? kIce
                                        : kMuted);
        return node;
    }
    // 탭 이름(새 항목 수 포함)과 현재 탭의 목록을 다시 그린다. 비어 있으면 상태 문구를 보여 준다.
    void render() {
        int counts[2] = {0, 0};
        for (auto const& m : m_messages)
            if (staffmail::fresh(m, m_owner)) {
                if (inInbox(m))
                    ++counts[0];
                else if (inSent(m))
                    ++counts[1];
            }
        for (int i = 0; i < 2; ++i) {
            std::string const text = (i ? std::string("Sent") : std::string("Inbox")) +
                                     (counts[i] > 0 ? fmt::format("  {}", counts[i]) : std::string());
            paintChoice(m_tabs[i], text.c_str(), m_tab == i, 88.f);
        }
        auto* content = m_list->m_contentLayer;
        content->removeAllChildrenWithCleanup(true);
        m_shown.clear();
        if (m_loaded && m_error.empty())
            for (int i = 0; i < static_cast<int>(m_messages.size()); ++i)
                if (m_tab == 0 ? inInbox(m_messages[i]) : inSent(m_messages[i])) m_shown.push_back(i);
        bool const empty = m_shown.empty();
        m_status->setVisible(empty);
        if (m_emptyIcon) m_emptyIcon->setVisible(empty && m_loaded && m_error.empty());
        m_status->setString(
            !m_loaded          ? "Loading..."
            : !m_error.empty() ? m_error.c_str()
            : m_tab == 0       ? (m_owner ? "No messages from your sub-admins." : "No messages from Tipp7 yet.")
                         : (m_owner ? "You have not written to anyone yet." : "Nothing sent yet. Tap Write to Tipp7."));
        m_status->limitLabelWidth(ListW - 30.f, .48f, .2f);
        float const height = std::max(ListH, m_shown.size() * (RowH + Gap) + Gap);
        content->setContentSize({ListW, height});
        auto const now = Service::get().serverNow();
        for (size_t r = 0; r < m_shown.size(); ++r) {
            auto* node = row(m_messages[m_shown[r]], now);
            node->setTag(m_shown[r]);
            node->setPosition({ListW / 2.f, height - Gap - RowH / 2.f - r * (RowH + Gap)});
            content->addChild(node);
        }
        m_list->scrollToTop();
    }
    // 메시지 팝업을 열고, 변경/삭제 시 이 목록을 갱신한다.
    void open(int index) {
        if (index < 0 || index >= static_cast<int>(m_messages.size())) return;
        if (auto* popup = StaffMessagePopup::create(
                m_messages[index], m_owner,
                [self = Weak<StaffMessagesPopup>(this)] {
                    if (auto owner = self.lock()) owner->changed();
                },
                [self = Weak<StaffMessagesPopup>(this)](std::vector<StaffMessage> messages) {
                    if (auto owner = self.lock()) {
                        owner->received(std::move(messages), {});
                        if (owner->m_changed) owner->m_changed();
                    }
                }))
            popup->show();
    }
    void onTab(CCObject* sender) {
        int const tab = std::clamp(sender->getTag(), 0, 1);
        if (tab == m_tab) return;
        m_tab = tab;
        render();
    }
    // 새 메시지 작성. 제작자는 수신자 선택부터, 서브 관리자는 제작자 계정에게 보내는 작성창을 연다.
    void onNew(CCObject*) {
        if (m_owner) {
            if (auto* popup = StaffRecipientPopup::create([self = Weak<StaffMessagesPopup>(this)] {
                    if (auto owner = self.lock()) {
                        owner->m_tab = 1;
                        owner->changed();
                    }
                }))
                popup->show();
            return;
        }
        if (auto *popup = StaffWritePopup::create(
                false, "Ideas or questions for Tipp7. Only Tipp7 reads them.", "",
                [self = Weak<StaffMessagesPopup>(this)](std::string kind, std::string text, Done done) {
                    Service::get().sendStaffMessage(
                        std::move(kind), std::move(text),
                        [self, done = std::move(done)](std::vector<StaffMessage> messages, int, std::string error) {
                            done(error.empty(), error);
                            if (auto owner = self.lock(); owner && error.empty()) {
                                owner->m_tab = 1;
                                owner->received(std::move(messages), {});
                            }
                        });
                }))
            popup->show();
    }
    void onRefresh(CCObject*) { load(); }

public:
    static StaffMessagesPopup* create(bool owner, std::function<void()> changed = {}) {
        auto* result = new StaffMessagesPopup;
        if (result->init(owner, std::move(changed))) {
            result->autorelease();
            return result;
        }
        delete result;
        return nullptr;
    }
};

// 서브 관리자 관리 팝업. GD 이름으로 후보를 검색해 임명하고, 목록에서 메시지/해임/프로필 보기를 할 수 있다.
// 한 페이지에 kPage 명씩 보여 준다. 제작자 계정 전용.
class StaffPopup final : public ::versus::DeferredPopup {
    static constexpr int kPage = 4;
    std::vector<StaffMember> m_staff;
    std::vector<StaffCandidate> m_found;
    int m_page = 0;
    bool m_loaded = false, m_busy = false;
    std::string m_error;
    TextInput* m_name = nullptr;
    CCNode* m_rows = nullptr;
    CCNode* m_results = nullptr;
    CCLabelBMFont* m_status = nullptr;
    CCLabelBMFont* m_count = nullptr;
    CCLabelBMFont* m_pageLabel = nullptr;
    CCMenuItemSpriteExtra *m_previous = nullptr, *m_next = nullptr, *m_find = nullptr;
    void onExit() override {
        if (m_name) m_name->defocus();
        Popup::onExit();
    }
    bool init() override {
        if (!Popup::init(440.f, 290.f, "GJ_square02.png")) return false;
        setTitle("Staff", "goldFont.fnt", .75f);
        label(m_mainLayer, "Sub-admins use the moderation desk like you (not on staff). Only you appoint them.",
              {220.f, 248.f}, .4f, 400.f, kMuted, false, "chatFont.fnt");
        m_name = TextInput::create(190.f, "GD name");
        m_name->setCommonFilter(CommonFilter::Any);
        m_name->setMaxCharCount(32);
        m_name->setPosition({150.f, 222.f});
        styleInput(m_name);
        m_mainLayer->addChild(m_name);
        m_find = button(m_buttonMenu, this, menu_selector(StaffPopup::onFind), "Find", {280.f, 222.f}, .5f,
                        "GJ_button_02.png");
        m_count = rightLabel(m_mainLayer, "", {420.f, 222.f}, .4f, 90.f, kIce);
        m_results = CCNode::create();
        m_mainLayer->addChild(m_results);
        m_rows = CCNode::create();
        m_mainLayer->addChild(m_rows);
        m_status = label(m_mainLayer, "Loading...", {220.f, 120.f}, .5f, 380.f, kIce, false, "chatFont.fnt");
        m_previous = button(m_buttonMenu, this, menu_selector(StaffPopup::onPrevious), "<", {170.f, 24.f}, .45f);
        m_next = button(m_buttonMenu, this, menu_selector(StaffPopup::onNext), ">", {270.f, 24.f}, .45f);
        m_pageLabel = label(m_mainLayer, "1 / 1", {220.f, 24.f}, .33f, 70.f, kIce);
        iconButton(m_buttonMenu, this, menu_selector(StaffPopup::onRefresh), "GJ_updateBtn_001.png", {408.f, 26.f},
                   28.f);
        load();
        return true;
    }
    // 서버에서 받은 서브 관리자 목록을 반영한다.
    void received(std::vector<StaffMember> staff, std::string error) {
        m_loaded = true;
        m_busy = false;
        m_error = std::move(error);
        if (m_error.empty()) m_staff = std::move(staff);
        render();
    }
    void load() {
        m_loaded = false;
        render();
        Service::get().fetchStaff([self = Weak<StaffPopup>(this)](std::vector<StaffMember> staff, std::string error) {
            if (auto owner = self.lock()) owner->received(std::move(staff), std::move(error));
        });
    }
    // 검색 결과(최대 2개)와 서브 관리자 목록 페이지를 다시 그린다. 검색 결과 수에 따라 목록 시작 위치가 달라진다.
    void render() {
        m_rows->removeAllChildrenWithCleanup(true);
        m_results->removeAllChildrenWithCleanup(true);
        int const count = static_cast<int>(m_staff.size());
        m_count->setString(m_loaded ? fmt::format("{} sub-admin{}", count, count == 1 ? "" : "s").c_str() : "");
        int const pages = std::max(1, (count + kPage - 1) / kPage);
        m_page = std::clamp(m_page, 0, pages - 1);
        m_pageLabel->setString(fmt::format("{} / {}", m_page + 1, pages).c_str());
        enabled(m_previous, m_page > 0);
        enabled(m_next, m_page + 1 < pages);
        enabled(m_find, !m_busy);
        auto* actions = menu(m_results);
        for (size_t i = 0; i < std::min<size_t>(m_found.size(), 2); ++i) {
            auto const& found = m_found[i];
            float const y = 194.f - static_cast<float>(i) * 24.f;
            auto* cell = panel(m_results, {16.f, y - 11.f}, {408.f, 22.f}, false);
            label(cell, found.name, {8.f, 11.f}, .36f, 200.f, ccWHITE, true);
            if (found.role.empty()) {
                auto* add = button(actions, this, menu_selector(StaffPopup::onAdd), "Make Sub-admin", {360.f, y}, .38f,
                                   "GJ_button_01.png");
                add->setTag(static_cast<int>(i));
                enabled(add, !m_busy);
            } else
                rightLabel(cell, found.role == "owner" ? "Administrator" : "Already a sub-admin", {400.f, 11.f}, .4f,
                           160.f, staffRoleColor(found.role));
        }
        float const listTop = m_found.empty() ? 186.f : 186.f - std::min<float>(m_found.size(), 2.f) * 24.f;
        m_status->setVisible(!m_loaded || !m_error.empty() || count == 0);
        m_status->setString(!m_loaded          ? "Loading..."
                            : !m_error.empty() ? m_error.c_str()
                                               : "No sub-admins yet. Find a player by GD name to appoint one.");
        m_status->setPositionY(listTop - 50.f);
        m_status->limitLabelWidth(380.f, .5f, .2f);
        if (!m_loaded || !m_error.empty()) return;
        auto* rows = menu(m_rows);
        int const shown =
            m_found.empty() ? kPage : std::max(2, kPage - static_cast<int>(std::min<size_t>(m_found.size(), 2)));
        for (int row = 0; row < shown; ++row) {
            int const index = m_page * kPage + row;
            if (index >= count) break;
            auto const& member = m_staff[index];
            float const y = listTop - 16.f - row * 34.f;
            auto* cell = panel(m_rows, {16.f, y - 15.f}, {408.f, 30.f}, false);
            auto* nameLabel = createCosmeticLabel(member.name.c_str(), "bigFont.fnt");
            nameLabel->setScale(std::min(.4f, 190.f / std::max(1.f, nameLabel->getContentSize().width)));
            nameLabel->setColor(kModBlue);
            auto* name = CCMenuItemSpriteExtra::create(nameLabel, this, menu_selector(StaffPopup::onProfile));
            name->setAnchorPoint({0.f, .5f});
            name->setPosition({24.f, y + 5.f});
            name->setTag(index);
            rows->addChild(name);
            label(cell,
                  fmt::format("Sub-admin since {}  |  by {}", localDateTime(member.addedAt).substr(0, 10),
                              member.addedByName.empty() ? std::string("Tipp7") : member.addedByName),
                  {8.f, 8.f}, .38f, 300.f, kMuted, true, "chatFont.fnt");
            auto* remove =
                button(rows, this, menu_selector(StaffPopup::onRemove), "Remove", {388.f, y}, .4f, "GJ_button_06.png");
            remove->setTag(index);
            enabled(remove, !m_busy);
            auto* write = button(rows, this, menu_selector(StaffPopup::onMessage), "Message", {324.f, y}, .4f,
                                 "GJ_button_02.png");
            write->setTag(index);
            write->setID(fmt::format("staff-message-{}", index));
            enabled(write, !m_busy);
        }
    }
    // 입력한 GD 이름으로 Versus 플레이어를 검색한다. 결과가 없으면 알린다.
    void onFind(CCObject*) {
        if (m_busy) return;
        m_name->defocus();
        std::string name(m_name->getString());
        if (name.find_first_not_of(' ') == std::string::npos) {
            Notification::create("Enter a GD name", NotificationIcon::Warning)->show();
            return;
        }
        m_busy = true;
        render();
        Service::get().findStaffCandidates(
            name, [self = Weak<StaffPopup>(this)](std::vector<StaffCandidate> found, std::string error) {
                auto owner = self.lock();
                if (!owner) return;
                owner->m_busy = false;
                if (!error.empty()) {
                    owner->render();
                    error_(error);
                    return;
                }
                owner->m_found = std::move(found);
                if (owner->m_found.empty())
                    Notification::create("No Versus player with that GD name", NotificationIcon::Warning)->show();
                owner->render();
            });
    }
    static void error_(std::string const& text) { error(text); }
    // 임명 확인창을 거쳐 서브 관리자로 추가한다. 성공하면 서버가 돌려준 목록으로 갱신한다.
    void onAdd(CCObject* sender) {
        int const index = sender->getTag();
        if (m_busy || index < 0 || index >= static_cast<int>(m_found.size())) return;
        auto const found = m_found[index];
        createQuickPopup(
            "Make Sub-admin?",
            fmt::format(
                "<cy>{}</c> becomes a sub-admin: the moderation desk (reports, bans, "
                "rating and dodge changes), the <cb>Admin</c> title and a blue wrench by the name. Every action is logged.",
                plainAlertText(found.name)),
            "Cancel", "Appoint", [self = Weak<StaffPopup>(this), uid = found.uid](auto*, bool yes) {
                auto owner = self.lock();
                if (!yes || !owner || owner->m_busy) return;
                owner->m_busy = true;
                owner->render();
                Service::get().addStaff(uid, [self](std::vector<StaffMember> staff, std::string error) {
                    auto owner = self.lock();
                    if (!owner) return;
                    if (!error.empty()) {
                        owner->m_busy = false;
                        owner->render();
                        error_(error);
                        return;
                    }
                    Notification::create("Sub-admin added", NotificationIcon::Success)->show();
                    owner->m_found.clear();
                    owner->received(std::move(staff), {});
                });
            });
    }
    // 해당 서브 관리자에게 보낼 메시지 작성창을 연다.
    void onMessage(CCObject* sender) {
        int const index = sender->getTag();
        if (m_busy || index < 0 || index >= static_cast<int>(m_staff.size())) return;
        auto const member = m_staff[index];
        if (auto *popup = StaffWritePopup::create(
                true, "To " + member.name + " - only they see it (their desk shows it as new).", "",
                [uid = member.uid](std::string, std::string text, Done done) {
                    Service::get().messageStaffMember(
                        uid, std::move(text),
                        [done = std::move(done)](std::vector<StaffMessage>, int, std::string error) {
                            done(error.empty(), error);
                        });
                },
                "Message " + member.name))
            popup->show();
    }
    // 해임 확인창을 거쳐 서브 관리자를 제거한다.
    void onRemove(CCObject* sender) {
        int const index = sender->getTag();
        if (m_busy || index < 0 || index >= static_cast<int>(m_staff.size())) return;
        auto const member = m_staff[index];
        createQuickPopup(
            "Remove Sub-admin?",
            fmt::format("<cy>{}</c> loses the moderation desk and the Admin title now.", plainAlertText(member.name)),
            "Keep", "Remove", [self = Weak<StaffPopup>(this), uid = member.uid](auto*, bool yes) {
                auto owner = self.lock();
                if (!yes || !owner || owner->m_busy) return;
                owner->m_busy = true;
                owner->render();
                Service::get().removeStaff(uid, [self](std::vector<StaffMember> staff, std::string error) {
                    auto owner = self.lock();
                    if (!owner) return;
                    if (!error.empty()) {
                        owner->m_busy = false;
                        owner->render();
                        error_(error);
                        return;
                    }
                    Notification::create("Sub-admin removed", NotificationIcon::Success)->show();
                    owner->received(std::move(staff), {});
                });
            });
    }
    // 서브 관리자의 프로필을 연다.
    void onProfile(CCObject* sender) {
        int const index = sender->getTag();
        if (index < 0 || index >= static_cast<int>(m_staff.size())) return;
        auto const& member = m_staff[index];
        showPlayer(moderationProfile(member.uid, member.name, member.accountId));
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
    CREATE_FUNC(StaffPopup);
};

// 스태프 규칙 안내 팝업. 밴 처리 전 주의사항을 영어/한국어로 보여 준다.
// 줄 앞 표식으로 서식을 정한다: "# " 제목, "!! " 경고 상자, "* " 항목, "- " 세부 항목, 뒤에 "!" 가 붙으면 강조.
class StaffRulesPopup final : public ::versus::DeferredPopup {
    static constexpr float W = 420.f, H = 310.f, PaperW = 396.f, PaperH = 236.f, PaperY = 42.f;
    // 영어 본문
    static constexpr char const* kEnglish =
        "# Before you ban a player\n"
        "!! Breaking these rules can cost you your staff role immediately!\n"
        "*! Never ban on a hasty judgment.\n"
        "- On maps with random triggers the replay may not play back correctly and the route can change a lot, so a run can look like noclip.\n"
        "- If something looks wrong, open the map and play it yourself to check.\n"
        "*! When you play a replay, use the same TPS as the recording.\n"
        "- If the recording and the playback run at a different TPS, a tiny timing difference can make a run look like noclip.\n"
        "* Ban length\n"
        "-! Short: 7 days. Usual: 30 days. Serious: 1 year to permanent.\n"
        "-! TPS Bypass: raise the ban each time - 1 day, then 7 days, then 30 days, and longer after that.\n"
        "- The player's profile shows how often and why they were banned before (staff only).\n"
        "*! A wrong ban can hurt a player, even emotionally. Always act carefully.\n"
        "* Hacking with macros or bots has been very rare so far.\n"
        "* Many reports are only about losing, plain complaints or AFK.\n"
        "- You can just dismiss and ignore those reports.";
    // 한국어 본문(UTF-8 바이트 이스케이프로 적혀 있음)
    static constexpr char const* kKorean =
        "# \xEC\x9C\xA0\xEC\xA0\x80 \xEB\xB0\xB4 \xEC\xB2\x98\xEB\xA6\xAC\xED\x95\xA0 \xEB\x95\x8C \xEC\xA3\xBC\xEC\x9D\x98\xEC\x82\xAC\xED\x95\xAD\n"
        "!! \xEC\xA7\x80\xED\x82\xA4\xEC\xA7\x80 \xEC\x95\x8A\xEC\x9C\xBC\xEB\xA9\xB4 \xEC\x8A\xA4\xED\x83\x9C\xED\x94\x84 \xEC\x9E\x90\xEA\xB2\xA9\xEC\x9D\x84 \xEC\xA6\x89\xEC\x8B\x9C \xEB\xB0\x95\xED\x83\x88\xEB\x8B\xB9\xED\x95\xA0 \xEC\x88\x98 \xEC\x9E\x88\xEC\x8A\xB5\xEB\x8B\x88\xEB\x8B\xA4!\n"
        "*! \xEC\x84\xA3\xEB\xB6\x80\xEB\xA5\xB8 \xED\x8C\x90\xEB\x8B\xA8\xEC\x9C\xBC\xEB\xA1\x9C \xEB\xB0\xB4\xED\x95\x98\xEC\xA7\x80 \xEB\xA7\x88\xEC\x84\xB8\xEC\x9A\x94.\n"
        "- \xEB\x9E\x9C\xEB\x8D\xA4 \xED\x8A\xB8\xEB\xA6\xAC\xEA\xB1\xB0\xEA\xB0\x80 \xEB\x93\xA4\xEC\x96\xB4\xEA\xB0\x84 \xEB\xA7\xB5\xEC\x97\x90\xEC\x84\x9C\xEB\x8A\x94 \xEB\xA6\xAC\xED\x94\x8C\xEB\xA0\x88\xEC\x9D\xB4\xEA\xB0\x80 \xEC\xA0\x9C\xEB\x8C\x80\xEB\xA1\x9C \xEC\x9E\xAC\xEC\x83\x9D\xEB\x90\x98\xEC\xA7\x80 \xEC\x95\x8A\xEC\x9D\x84 \xEC\x88\x98 \xEC\x9E\x88\xEA\xB3\xA0, \xEB\x8F\x99\xEC\x84\xA0\xEC\x9D\xB4 \xED\x81\xAC\xEA\xB2\x8C \xEB\xB0\x94\xEB\x80\x8C\xEC\x96\xB4 \xEB\x85\xB8\xED\x81\xB4\xEB\xA6\xBD\xEC\xB2\x98\xEB\x9F\xBC \xEB\xB3\xB4\xEC\x9D\xBC \xEC\x88\x98 \xEC\x9E\x88\xEC\x8A\xB5\xEB\x8B\x88\xEB\x8B\xA4.\n"
        "- \xEC\x9D\xB4\xEC\x83\x81\xED\x95\x98\xEB\x8B\xA4 \xEC\x8B\xB6\xEC\x9C\xBC\xEB\xA9\xB4 \xEC\xA7\x81\xEC\xA0\x91 \xEB\xA7\xB5\xEC\x97\x90 \xEB\x93\xA4\xEC\x96\xB4\xEA\xB0\x80\xEC\x84\x9C \xED\x94\x8C\xEB\xA0\x88\xEC\x9D\xB4\xED\x95\x98\xEB\xA9\xB0 \xEA\xB2\x80\xEC\xA6\x9D\xED\x95\x98\xEC\x84\xB8\xEC\x9A\x94.\n"
        "*! \xEB\xA6\xAC\xED\x94\x8C\xEB\xA0\x88\xEC\x9D\xB4 \xEC\x9E\xAC\xEC\x83\x9D \xEC\x8B\x9C TPS \xED\x99\x98\xEA\xB2\xBD\xEC\x9D\x84 \xEB\x8F\x99\xEC\x9D\xBC\xED\x95\x98\xEA\xB2\x8C \xEB\xA7\x9E\xEC\xB6\xB0\xEC\xA3\xBC\xEC\x84\xB8\xEC\x9A\x94.\n"
        "- \xEB\x85\xB9\xED\x99\x94\xEB\x90\x9C TPS \xED\x99\x98\xEA\xB2\xBD\xEA\xB3\xBC \xEC\x9E\xAC\xEC\x83\x9D TPS \xED\x99\x98\xEA\xB2\xBD\xEC\x9D\xB4 \xEB\x8B\xA4\xEB\xA5\xBC \xEA\xB2\xBD\xEC\x9A\xB0, \xEC\xB0\xB0\xEB\x82\x98\xEC\x9D\x98 \xEC\x88\x9C\xEA\xB0\x84\xEC\x9D\x98 \xEC\xB0\xA8\xEC\x9D\xB4\xEB\xA1\x9C \xEB\x85\xB8\xED\x81\xB4\xEB\xA6\xBD\xEC\x9C\xBC\xEB\xA1\x9C \xEC\x98\xA4\xEC\x9D\xB8\xEB\xB0\x9B\xEB\x8A\x94 \xEC\x83\x81\xED\x99\xA9\xEC\x9D\xB4 \xEB\xA7\x8C\xEB\x93\xA4\xEC\x96\xB4\xEC\xA7\x88 \xEC\x88\x98 \xEC\x9E\x88\xEC\x8A\xB5\xEB\x8B\x88\xEB\x8B\xA4.\n"
        "* \xEB\xB0\xB4 \xEA\xB8\xB0\xED\x95\x9C\n"
        "-! \xEC\xA7\xA7\xEA\xB2\x8C\xEB\x8A\x94 7\xEC\x9D\xBC, \xED\x8F\x89\xEA\xB7\xA0 30\xEC\x9D\xBC, \xEC\xA0\x95\xEB\x8F\x84\xEC\x97\x90 \xEB\x94\xB0\xEB\x9D\xBC 1\xEB\x85\x84~\xEB\xAC\xB4\xEA\xB8\xB0\xED\x95\x9C.\n"
        "-! TPS Bypass: \xEA\xB1\xB8\xEB\xA6\xB4 \xEB\x95\x8C\xEB\xA7\x88\xEB\x8B\xA4 \xEB\x8A\x98\xEB\xA0\xA4\xEA\xB0\x80\xEB\xA9\xB0 \xEB\xB0\xB4 - 1\xEC\x9D\xBC, \xEB\x8B\xA4\xEC\x9D\x8C 7\xEC\x9D\xBC, \xEB\x8B\xA4\xEC\x9D\x8C 30\xEC\x9D\xBC, \xEA\xB7\xB8 \xEB\x92\xA4\xEB\xA1\x9C\xEB\x8A\x94 \xEB\x8D\x94 \xEA\xB8\xB8\xEA\xB2\x8C.\n"
        "- \xEC\x9C\xA0\xEC\xA0\x80 \xED\x94\x84\xEB\xA1\x9C\xED\x95\x84\xEC\x97\x90\xEC\x84\x9C \xEC\x98\x88\xEC\xA0\x84\xEC\x97\x90 \xEC\x96\xB4\xEB\x96\xA4 \xEC\x9D\xB4\xEC\x9C\xA0\xEB\xA1\x9C \xEB\xAA\x87 \xEB\xB2\x88 \xEB\xB0\xB4\xEB\x90\x90\xEB\x8A\x94\xEC\xA7\x80 \xEB\xB3\xBC \xEC\x88\x98 \xEC\x9E\x88\xEC\x8A\xB5\xEB\x8B\x88\xEB\x8B\xA4(\xEC\x8A\xA4\xED\x83\x9C\xED\x94\x84\xEB\xA7\x8C).\n"
        "*! \xEC\x98\xA4\xEC\x9D\xB8\xEB\x90\x9C \xEB\xB0\xB4\xEC\x9D\x80 \xED\x95\x9C \xEC\x9C\xA0\xEC\xA0\x80\xEC\x97\x90\xEA\xB2\x8C \xEC\xA0\x95\xEC\x8B\xA0\xEC\xA0\x81\xEC\x9D\xB8 \xED\x94\xBC\xED\x95\xB4\xEA\xB9\x8C\xEC\xA7\x80 \xEC\xA4\x84 \xEC\x88\x98 \xEC\x9E\x88\xEC\x8A\xB5\xEB\x8B\x88\xEB\x8B\xA4. \xED\x95\xAD\xEC\x83\x81 \xEC\x8B\xA0\xEC\xA4\x91\xED\x95\x98\xEA\xB2\x8C \xED\x96\x89\xEB\x8F\x99\xED\x95\x98\xEC\x84\xB8\xEC\x9A\x94.\n"
        "* \xEB\xA7\xA4\xED\x81\xAC\xEB\xA1\x9C/\xEB\xB4\x87\xEC\x9D\x84 \xEC\x9D\xB4\xEC\x9A\xA9\xED\x95\x9C \xED\x95\xB5 \xEC\x82\xAC\xEC\x9A\xA9\xEC\x9D\x80 \xEC\xA7\x80\xEA\xB8\x88\xEA\xB9\x8C\xEC\xA7\x80 \xEA\xB1\xB0\xEC\x9D\x98 \xEC\x97\x86\xEC\x97\x88\xEC\x8A\xB5\xEB\x8B\x88\xEB\x8B\xA4.\n"
        "* \xEB\x8B\xA8\xEC\x88\x9C\xED\x9E\x88 \xEC\xA1\x8C\xEB\x8B\xA4\xEB\x8A\x94 \xEC\x9D\xB4\xEC\x9C\xA0\xEC\x9D\x98 \xEC\x8B\xA0\xEA\xB3\xA0, \xEB\x8B\xA8\xEC\x88\x9C \xEB\xB6\x88\xED\x8F\x89, AFK \xEA\xB4\x80\xEB\xA0\xA8 \xEC\x8B\xA0\xEA\xB3\xA0\xEA\xB0\x80 \xEC\x9E\x90\xEC\xA3\xBC \xEB\x93\xA4\xEC\x96\xB4\xEC\x98\xB5\xEB\x8B\x88\xEB\x8B\xA4.\n"
        "- \xEC\x9D\xB4\xEB\x9F\xB0 \xEC\x8B\xA0\xEA\xB3\xA0\xEB\x8A\x94 \xEA\xB7\xB8\xEB\x83\xA5 \xEC\xB2\xA0\xED\x9A\x8C(Dismiss)\xED\x95\x98\xEA\xB3\xA0 \xEB\xAC\xB4\xEC\x8B\x9C\xED\x95\x98\xEC\x85\x94\xEB\x8F\x84 \xEB\x90\xA9\xEB\x8B\x88\xEB\x8B\xA4.";
    ScrollLayer* m_scroll = nullptr;
    CCMenuItemSpriteExtra *m_english = nullptr, *m_korean = nullptr;
    bool m_isKorean = true;
    // 줄이 길어 영역을 넘으면 글자 크기를 한 단계씩 줄여 가며(1.0 > 0.92 > 0.86) 다시 배치한다.
    // 선택되지 않은 언어 버튼은 반투명하게 표시한다.
    void render() {
        float height = 0.f;
        for (float scale : {1.f, .92f, .86f}) {
            height = layout(scale);
            if (height <= PaperH) break;
        }
        m_scroll->m_contentLayer->setContentSize({PaperW, std::max(PaperH, height)});
        m_scroll->scrollToTop();
        for (auto* item : {m_english, m_korean}) cascadeOpacity(item);
        m_english->setOpacity(m_isKorean ? 130 : 255);
        m_korean->setOpacity(m_isKorean ? 255 : 130);
    }
    // 현재 언어의 본문을 줄 단위로 파싱해 scale 배율로 배치하고, 필요한 전체 높이를 반환한다.
    float layout(float scale) {
        m_scroll->m_contentLayer->removeAllChildren();
        std::string_view const text = m_isKorean ? kKorean : kEnglish;
        float offset = 10.f * scale;
        std::vector<std::pair<CCNode*, float>> rows;
        auto place = [&](CCNode* node, float x, float y, int z = 0) {
            node->setAnchorPoint({0.f, 1.f});
            node->setPositionX(x);
            rows.emplace_back(node, y);
            m_scroll->m_contentLayer->addChild(node, z);
        };
        ccColor3B const strongColor = ccc3(255, 214, 60), body = ccc3(230, 234, 242);
        for (size_t start = 0; start < text.size();) {
            auto end = text.find('\n', start);
            std::string line(text.substr(start, end == std::string_view::npos ? std::string_view::npos : end - start));
            start = end == std::string_view::npos ? text.size() : end + 1;
            if (line.starts_with("# ")) {
                auto* title = createSystemText(line.substr(2), 17.f * scale, PaperW - 28.f, kCCTextAlignmentCenter,
                                               TextWeight::Bold);
                title->setColor(ccc3(255, 190, 105));
                title->setAnchorPoint({.5f, 1.f});
                title->setPositionX(PaperW / 2.f);
                rows.emplace_back(title, offset);
                m_scroll->m_contentLayer->addChild(title);
                offset += title->getContentSize().height + 7.f * scale;
                continue;
            }
            if (line.starts_with("!! ")) {
                auto* warning = createSystemText(line.substr(3), 14.f * scale, PaperW - 40.f, kCCTextAlignmentCenter,
                                                 TextWeight::Bold);
                warning->setColor(ccc3(255, 214, 60));
                warning->setAnchorPoint({.5f, 1.f});
                warning->setPositionX(PaperW / 2.f);
                rows.emplace_back(warning, offset + 5.f * scale);
                m_scroll->m_contentLayer->addChild(warning);
                auto* box = NineSlice::create("square02b_001.png");
                box->setColor({95, 70, 12});
                box->setOpacity(210);
                box->setContentSize({PaperW - 16.f, warning->getContentSize().height + 12.f * scale});
                place(box, 8.f, offset, -1);
                offset += warning->getContentSize().height + 18.f * scale;
                continue;
            }
            bool const point = line.starts_with("* ") || line.starts_with("*! ");
            bool const detail = line.starts_with("- ") || line.starts_with("-! ");
            bool const strong = line.starts_with("*! ") || line.starts_with("-! ");
            line.erase(0, strong ? 3 : (point || detail) ? 2 : 0);
            float const size = (point ? 14.f : 13.f) * scale;
            float const x = detail ? 46.f : 32.f;
            if (point) {
                auto* bullet = createSystemText("\xE2\x80\xA2", size, 0.f, kCCTextAlignmentLeft, TextWeight::Bold);
                bullet->setColor(strong ? strongColor : ccc3(255, 190, 105));
                place(bullet, 18.f, offset);
            } else if (detail) {
                auto* dash = createSystemText("-", size, 0.f, kCCTextAlignmentLeft);
                dash->setColor(kMuted);
                place(dash, 34.f, offset);
            }
            auto* words = createSystemText(line, size, PaperW - x - 14.f, kCCTextAlignmentLeft,
                                           strong || point ? TextWeight::Bold : TextWeight::Regular);
            words->setColor(strong ? strongColor : point ? ccWHITE : body);
            place(words, x, offset);
            offset += words->getContentSize().height + (point ? 4.f : 3.f) * scale;
        }
        float const needed = offset + 8.f * scale, height = std::max(PaperH, needed);
        for (auto const& [row, y] : rows) row->setPositionY(height - y);
        return needed;
    }
    // 언어를 바꾸고 선택을 저장한다(패치 노트 언어 설정과 같은 저장값을 공유).
    void language(bool korean) {
        m_isKorean = korean;
        Mod::get()->setSavedValue("patch-notes-korean", korean);
        render();
    }
    void english(CCObject*) { language(false); }
    void korean(CCObject*) { language(true); }
    bool init() override {
        if (!Popup::init(W, H, "GJ_square02.png")) return false;
        setTitle("Staff Rules", "bigFont.fnt", .62f);
        m_title->setColor(ccc3(255, 214, 60));
        auto* paper = NineSlice::create("square02b_001.png");
        paper->setContentSize({PaperW, PaperH});
        paper->setPosition({W / 2.f, PaperY + PaperH / 2.f});
        paper->setColor({18, 23, 34});
        paper->setOpacity(225);
        m_mainLayer->addChild(paper);
        m_scroll = ScrollLayer::create({PaperW, PaperH});
        m_scroll->setPosition({(W - PaperW) / 2.f, PaperY});
        m_mainLayer->addChild(m_scroll);
        m_english = button(m_buttonMenu, this, menu_selector(StaffRulesPopup::english), "English", {58.f, 24.f}, .42f);
        m_korean = button(m_buttonMenu, this, menu_selector(StaffRulesPopup::korean), "Korean", {132.f, 24.f}, .42f);
        button(m_buttonMenu, this, menu_selector(StaffRulesPopup::onClose), "I understand", {W - 76.f, 24.f}, .5f,
               "GJ_button_01.png")
            ->setID("staff-rules-ok"_spr);
        m_isKorean = Mod::get()->getSavedValue<bool>("patch-notes-korean", true);
        render();
        return true;
    }

public:
    CREATE_FUNC(StaffRulesPopup);
};

// 플레이어의 밴 이력 팝업(스태프용). 최근 1년 밴 횟수, 현재 밴 상태, 사유별 횟수와 최근 밴 목록을 보여 준다.
class BanHistoryPopup final : public ::versus::DeferredPopup {
    static constexpr float W = 400.f, H = 280.f, ListW = W - 32.f, ListH = 176.f;
    std::string m_name;
    BanHistory m_history;
    bool init(std::string name, BanHistory history) {
        if (!Popup::init(W, H, "GJ_square02.png")) return false;
        m_name = std::move(name);
        m_history = std::move(history);
        setTitle("Ban History", "goldFont.fnt", .7f);
        setID("versus-ban-history"_spr);
        auto const& h = m_history;
        label(m_mainLayer,
              fmt::format("{}  |  {} ban{} in the last year{}", m_name, h.total, h.total == 1 ? "" : "s",
                          h.reportBans > 0
                              ? fmt::format("  |  {} report ban{}", h.reportBans, h.reportBans == 1 ? "" : "s")
                              : std::string()),
              {W / 2.f, H - 50.f}, .45f, W - 30.f, kIce, false, "chatFont.fnt");
        label(m_mainLayer,
              h.banned
                  ? (h.permanent ? std::string("Banned now: permanent") : "Banned now: until " + localDateTime(h.until))
                  : std::string("Not banned now"),
              {W / 2.f, H - 63.f}, .42f, W - 30.f, h.banned ? ccc3(255, 120, 105) : ccc3(130, 230, 160), false,
              "chatFont.fnt");
        auto* paper = NineSlice::create("square02b_001.png");
        paper->setContentSize({ListW, ListH});
        paper->setPosition({W / 2.f, 26.f + ListH / 2.f + 8.f});
        paper->setColor({12, 20, 40});
        paper->setOpacity(215);
        m_mainLayer->addChild(paper);
        auto* scroll = TapScrollLayer::create({ListW, ListH}, {});
        scroll->setPosition({(W - ListW) / 2.f, 34.f});
        m_mainLayer->addChild(scroll, 2);
        std::vector<std::pair<CCNode*, float>> rows;
        float y = 8.f;
        auto heading = [&](char const* text) {
            auto* node = label(scroll->m_contentLayer, text, {12.f, 0.f}, .32f, ListW - 24.f, ccc3(255, 190, 105), true,
                               "bigFont.fnt");
            rows.emplace_back(node, y + 7.f);
            y += 20.f;
        };
        auto lineAt = [&](std::string const& reason, std::string const& right, std::string const& under,
                          ccColor3B tone) {
            auto* card = CCLayerColor::create(ccc4(255, 255, 255, 12), ListW - 16.f, under.empty() ? 22.f : 32.f);
            card->ignoreAnchorPointForPosition(false);
            card->setAnchorPoint({0.f, 1.f});
            card->setPositionX(8.f);
            scroll->m_contentLayer->addChild(card);
            rows.emplace_back(card, y);
            auto* text = createSystemText(reason, 12.5f, 0.f, kCCTextAlignmentLeft, TextWeight::Bold);
            text->setColor(ccWHITE);
            if (text->getContentSize().width > ListW - 120.f)
                text->setScale((ListW - 120.f) / text->getContentSize().width);
            text->setAnchorPoint({0.f, .5f});
            text->setPositionX(18.f);
            scroll->m_contentLayer->addChild(text, 1);
            rows.emplace_back(text, y + 11.f);
            auto* value =
                label(scroll->m_contentLayer, right, {ListW - 18.f, 0.f}, .5f, 100.f, tone, false, "chatFont.fnt");
            value->setAnchorPoint({1.f, .5f});
            rows.emplace_back(value, y + 11.f);
            if (!under.empty()) {
                auto* note =
                    label(scroll->m_contentLayer, under, {18.f, 0.f}, .36f, ListW - 40.f, kMuted, true, "chatFont.fnt");
                rows.emplace_back(note, y + 24.f);
            }
            y += (under.empty() ? 22.f : 32.f) + 4.f;
        };
        // 영구 밴이면 "Permanent", 아니면 기간 문구
        auto length = [](int64_t durationMs, bool permanent) {
            return permanent ? std::string("Permanent") : banLengthText(durationMs);
        };
        if (h.total == 0) {
            auto* none = label(scroll->m_contentLayer, "No bans on record.", {ListW / 2.f, 0.f}, .5f, ListW - 30.f,
                               kMuted, false, "chatFont.fnt");
            rows.emplace_back(none, ListH / 2.f);
        } else {
            heading("BY REASON");
            for (auto const& reason : h.reasons)
                lineAt(reason.reason, fmt::format("x{}", reason.count),
                       fmt::format("Last: {}  |  {}", localDateTime(reason.lastAt).substr(0, 10),
                                   length(reason.lastDurationMs, reason.lastPermanent)),
                       reason.count >= 2 ? ccc3(255, 150, 110) : ccc3(255, 214, 110));
            y += 6.f;
            heading("LATEST BANS");
            for (auto const& ban : h.bans)
                lineAt(ban.reason.empty() ? std::string("No reason") : ban.reason,
                       length(ban.durationMs, ban.permanent), localDateTime(ban.at), kIce);
        }
        float const height = std::max(ListH, y + 6.f);
        scroll->m_contentLayer->setContentSize({ListW, height});
        for (auto const& [node, top] : rows) node->setPositionY(height - top);
        scroll->scrollToTop();
        button(m_buttonMenu, this, menu_selector(BanHistoryPopup::onClose), "OK", {W / 2.f, 20.f}, .5f);
        return true;
    }

public:
    static BanHistoryPopup* create(std::string name, BanHistory history) {
        auto* result = new BanHistoryPopup;
        if (result->init(std::move(name), std::move(history))) {
            result->autorelease();
            return result;
        }
        delete result;
        return nullptr;
    }
};

// 1.0.8: 랭크 맵 밴 창(스태프 전원). 레벨 ID 로 GD 에서 레벨을 찾아 사유와 함께 밴하면 Season 2 의 어느 컵 검색에도
// 나오지 않는다. 목록에서 사유/밴한 스태프를 보고 다시 넣을(Put Back) 수 있다. 모든 변경은 모더레이션 로그에 남는다.
class StaffMapsPopup final : public ::versus::DeferredPopup {
    static constexpr float W = 440.f, H = 290.f;
    static constexpr int kRows = 4, kFoundRows = 3;
    std::vector<StaffMapBan> m_maps;
    int m_limit = 0, m_page = 0;
    bool m_loaded = false, m_busy = false, m_looking = false;
    std::string m_error, m_lookupError;
    std::optional<LevelInfo> m_found;
    TextInput* m_id = nullptr;
    TextInput* m_reason = nullptr;
    CCNode* m_rows = nullptr;
    CCNode* m_foundNode = nullptr;
    CCLabelBMFont* m_status = nullptr;
    CCLabelBMFont* m_count = nullptr;
    CCLabelBMFont* m_pageLabel = nullptr;
    CCMenuItemSpriteExtra *m_previous = nullptr, *m_next = nullptr, *m_find = nullptr, *m_ban = nullptr;
    void onExit() override {
        if (m_id) m_id->defocus();
        if (m_reason) m_reason->defocus();
        Popup::onExit();
    }
    bool init() override {
        if (!Popup::init(W, H, "GJ_square02.png")) return false;
        setTitle("Ranked Map Bans", "goldFont.fnt", .7f);
        setID("versus-staff-map-bans"_spr);
        label(m_mainLayer, "A banned map never comes up in Ranked (either cup) until put back. Every change is logged.",
              {W / 2.f, 248.f}, .4f, 400.f, kMuted, false, "chatFont.fnt");
        m_id = TextInput::create(130.f, "Level ID");
        m_id->setCommonFilter(CommonFilter::Uint);
        m_id->setMaxCharCount(10);
        m_id->setPosition({87.f, 222.f});
        styleInput(m_id);
        m_mainLayer->addChild(m_id);
        m_find = button(m_buttonMenu, this, menu_selector(StaffMapsPopup::onFind), "Find", {190.f, 222.f}, .5f,
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
        m_ban = button(m_buttonMenu, this, menu_selector(StaffMapsPopup::onBan), "Ban", {378.f, 164.f}, .5f,
                       "GJ_button_06.png");
        m_rows = CCNode::create();
        m_mainLayer->addChild(m_rows);
        m_status = label(m_mainLayer, "Loading...", {W / 2.f, 120.f}, .5f, 380.f, kIce, false, "chatFont.fnt");
        m_previous = button(m_buttonMenu, this, menu_selector(StaffMapsPopup::onPrevious), "<", {170.f, 24.f}, .45f);
        m_next = button(m_buttonMenu, this, menu_selector(StaffMapsPopup::onNext), ">", {270.f, 24.f}, .45f);
        m_pageLabel = label(m_mainLayer, "1 / 1", {W / 2.f, 24.f}, .33f, 70.f, kIce);
        iconButton(m_buttonMenu, this, menu_selector(StaffMapsPopup::onRefresh), "GJ_updateBtn_001.png", {408.f, 26.f},
                   28.f);
        load();
        return true;
    }
    void received(std::vector<StaffMapBan> maps, int limit, std::string error) {
        m_loaded = true;
        m_busy = false;
        m_error = std::move(error);
        if (m_error.empty()) {
            m_maps = std::move(maps);
            if (limit > 0) m_limit = limit;
        }
        render();
    }
    void load() {
        m_loaded = false;
        render();
        Service::get().fetchStaffMapBans(
            [self = Weak<StaffMapsPopup>(this)](std::vector<StaffMapBan> maps, int limit, std::string error) {
                if (auto owner = self.lock()) owner->received(std::move(maps), limit, std::move(error));
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
    static CCNode* reasonText(CCNode* parent, std::string const& text, CCPoint at, float maxWidth, ccColor3B color) {
        auto shown = previewLine(text.empty() ? std::string("No reason") : text, 60);
        CCNode* node = nullptr;
        if (asciiOnly(shown))
            node = label(parent, shown, at, .36f, maxWidth, color, true, "chatFont.fnt");
        else {
            auto* system = createSystemText(shown, 11.f, 0.f, kCCTextAlignmentLeft);
            system->setColor(color);
            system->setAnchorPoint({0.f, .5f});
            if (system->getContentSize().width > maxWidth) system->setScale(maxWidth / system->getContentSize().width);
            system->setPosition(at);
            parent->addChild(system);
            node = system;
        }
        return node;
    }
    // 찾은 레벨 카드, 사유 입력/밴 버튼, 밴 목록 한 페이지를 다시 그린다.
    void render() {
        m_rows->removeAllChildrenWithCleanup(true);
        m_foundNode->removeAllChildrenWithCleanup(true);
        int const count = static_cast<int>(m_maps.size());
        m_count->setString(!m_loaded ? ""
                           : m_limit > 0 ? fmt::format("{} / {} banned", count, m_limit).c_str()
                                         : fmt::format("{} banned", count).c_str());
        enabled(m_find, !m_busy && !m_looking);
        bool const showFound = m_looking || m_found.has_value();
        m_reason->setVisible(m_found.has_value());
        m_ban->setVisible(m_found.has_value());
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
        int const perPage = showFound ? kFoundRows : kRows;
        float const listTop = showFound ? 148.f : 206.f;
        int const pages = std::max(1, (count + perPage - 1) / perPage);
        m_page = std::clamp(m_page, 0, pages - 1);
        m_pageLabel->setString(fmt::format("{} / {}", m_page + 1, pages).c_str());
        enabled(m_previous, m_page > 0);
        enabled(m_next, m_page + 1 < pages);
        m_status->setVisible(!m_loaded || !m_error.empty() || count == 0);
        m_status->setString(!m_loaded          ? "Loading..."
                            : !m_error.empty() ? m_error.c_str()
                                               : "No map is banned. Find a level by its ID to ban it.");
        m_status->setPositionY(listTop - 50.f);
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
            nameLabel->setScale(std::min(.34f, 170.f / std::max(1.f, nameLabel->getContentSize().width)));
            auto* name = CCMenuItemSpriteExtra::create(nameLabel, this, menu_selector(StaffMapsPopup::onDetail));
            name->setAnchorPoint({0.f, .5f});
            name->setPosition({24.f, y + 6.f});
            name->setTag(index);
            rows->addChild(name);
            label(cell, fmt::format("ID {}", map.levelId), {16.f + nameLabel->getScaledContentSize().width, 21.f},
                  .34f, 80.f, kMuted, true, "chatFont.fnt");
            reasonText(cell, map.reason, {8.f, 8.f}, 210.f, kIce);
            rightLabel(cell,
                       fmt::format("{}  |  {}", map.byName.empty() ? std::string("Admin") : map.byName,
                                   agoText(now - map.at)),
                       {330.f, 8.f}, .32f, 105.f, kMuted);
            auto* back = button(rows, this, menu_selector(StaffMapsPopup::onPutBack), "Put Back", {386.f, y}, .36f,
                                "GJ_button_01.png");
            back->setTag(index);
            enabled(back, !m_busy);
        }
    }
    // 입력한 ID 의 레벨을 GD 에서 찾는다. 못 찾아도 ID 로 밴할 수 있게 카드를 보여 준다.
    void onFind(CCObject*) {
        if (m_busy || m_looking) return;
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
        lookupLevel(id, [self = Weak<StaffMapsPopup>(this), id](std::optional<LevelInfo> level, std::string error) {
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
    // 사유를 확인하고 확인창을 거쳐 밴(이미 밴된 맵이면 사유 변경)한다.
    void onBan(CCObject*) {
        if (m_busy || !m_found) return;
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
                   : fmt::format("<cy>{}</c> (ID {}) will not come up in Ranked, in either cup, until a staff member "
                                 "puts it back.",
                                 plainAlertText(mapName(level.name, level.id)), level.id),
            "Cancel", change ? "Save" : "Ban",
            [self = Weak<StaffMapsPopup>(this), level, reason, change](auto*, bool yes) {
                auto owner = self.lock();
                if (!yes || !owner || owner->m_busy) return;
                owner->m_busy = true;
                owner->render();
                Service::get().banStaffMap(
                    level, reason, [self, change](std::vector<StaffMapBan> maps, int limit, std::string error) {
                        auto owner = self.lock();
                        if (!owner) return;
                        if (!error.empty()) {
                            owner->m_busy = false;
                            owner->render();
                            error_(error);
                            return;
                        }
                        Notification::create(change ? "Ban changed" : "Map banned from Ranked",
                                             NotificationIcon::Success)
                            ->show();
                        owner->m_found.reset();
                        owner->m_lookupError.clear();
                        owner->m_reason->setString("");
                        owner->m_id->setString("");
                        owner->m_page = 0;
                        owner->received(std::move(maps), limit, {});
                    });
            });
    }
    // 확인창을 거쳐 맵을 다시 랭크에 넣는다.
    void onPutBack(CCObject* sender) {
        int const index = sender->getTag();
        if (m_busy || index < 0 || index >= static_cast<int>(m_maps.size())) return;
        auto const map = m_maps[index];
        createQuickPopup("Put Back in Ranked?",
                         fmt::format("<cy>{}</c> (ID {}) can come up in Ranked again.",
                                     plainAlertText(mapName(map.name, map.levelId)), map.levelId),
                         "Keep Banned", "Put Back", [self = Weak<StaffMapsPopup>(this), id = map.levelId](auto*, bool yes) {
                             auto owner = self.lock();
                             if (!yes || !owner || owner->m_busy) return;
                             owner->m_busy = true;
                             owner->render();
                             Service::get().unbanStaffMap(
                                 id, [self](std::vector<StaffMapBan> maps, int limit, std::string error) {
                                     auto owner = self.lock();
                                     if (!owner) return;
                                     if (!error.empty()) {
                                         owner->m_busy = false;
                                         owner->render();
                                         error_(error);
                                         return;
                                     }
                                     Notification::create("Map put back", NotificationIcon::Success)->show();
                                     owner->received(std::move(maps), limit, {});
                                 });
                         });
    }
    // 밴 항목의 전체 사유와 밴한 스태프/시각을 보여 준다.
    void onDetail(CCObject* sender) {
        int const index = sender->getTag();
        if (index < 0 || index >= static_cast<int>(m_maps.size())) return;
        auto const& map = m_maps[index];
        FLAlertLayer::create(
            nullptr, mapName(map.name, map.levelId).c_str(),
            fmt::format("ID <cy>{}</c>{}\nBanned by <cb>{}</c>, {}\n\n{}", map.levelId,
                        map.creator.empty() ? "" : "  |  by " + plainAlertText(map.creator),
                        plainAlertText(map.byName.empty() ? std::string("Admin") : map.byName), localDateTime(map.at),
                        plainAlertText(map.reason.empty() ? std::string("No reason") : map.reason))
                .c_str(),
            "OK", nullptr, 380.f)
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
    CREATE_FUNC(StaffMapsPopup);
};
