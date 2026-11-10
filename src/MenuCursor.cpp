// Versus 메뉴에서 커서가 사라지거나 잠기지 않게 하는 구현.
// Windows에서는 CCEGLView 훅으로 커서 숨김/잠금 요청을 막고, macOS에서는 PlatformToolbox를
// 쓴다.
// 커서 필요 여부는 MenuCursorScope의 활성 개수로 판단한다.
#include "MenuCursor.hpp"
#include "MenuCursorScope.hpp"
#include <Geode/Geode.hpp>

#ifdef GEODE_IS_WINDOWS
#include <Geode/modify/CCEGLView.hpp>
#include <Windows.h>
#include <eclipse.eclipse-menu/include/config.hpp>
#endif

using namespace geode::prelude;

namespace {
// 커서를 다시 보이게 하고 잠금 상태를 맞춘다.
// Windows는 OS 커서 표시 카운터가 숨김 상태이고 이 프로세스가 포그라운드일 때 ShowCursor를
// 최대 8번 호출해 되살린다.
void reassertCursor(bool locked) {
#ifdef GEODE_IS_WINDOWS
    auto* view = CCDirector::sharedDirector()->getOpenGLView();
    if (!view) return;
    view->showCursor(true);
    view->toggleLockCursor(locked);
    CURSORINFO info{};
    info.cbSize = sizeof(info);
    if (::GetCursorInfo(&info) && !(info.flags & CURSOR_SHOWING)) {
        DWORD owner = 0;
        if (HWND foreground = ::GetForegroundWindow()) ::GetWindowThreadProcessId(foreground, &owner);
        if (owner == ::GetCurrentProcessId())
            for (int i = 0; i < 8 && ::ShowCursor(TRUE) < 0; ++i) {}
    }
#elif defined(GEODE_IS_MACOS)
    PlatformToolbox::showCursor();
    PlatformToolbox::toggleLockCursor(locked);
#else
    (void)locked;
#endif
}
// 메뉴 커서가 필요한 상태(Scope 활성)일 때 커서를 보이고 잠금 해제 상태로 유지한다.
// Windows 전용.
void enforceMenuCursor() {
#ifdef GEODE_IS_WINDOWS
    if (!versus::cursor::Scope::required()) return;
    reassertCursor(false);
#endif
}

// 씬에 들어가 있는 동안 Scope를 활성화해 두는 보이지 않는 노드.
class MenuCursorGuard final : public CCNode {
    versus::cursor::Scope m_scope;

public:
    void onEnter() override {
        m_scope.enter();
        CCNode::onEnter();
        enforceMenuCursor();
    }
    void onExit() override {
        m_scope.leave();
        CCNode::onExit();
    }
};
}

// owner에 가드 노드를 붙인다(Windows 전용). 이미 있으면 중복 생성하지 않는다.
void versus::keepMenuCursorVisible(CCNode* owner) {
#ifdef GEODE_IS_WINDOWS
    if (!owner || owner->getChildByID("menu-cursor-guard"_spr)) return;
    auto* guard = new MenuCursorGuard;
    if (!guard->init()) {
        delete guard;
        return;
    }
    guard->autorelease();
    guard->setID("menu-cursor-guard"_spr);
    owner->addChild(guard);
#endif
}

// 게임 설정 변수 0024: 커서 표시 여부.
bool versus::cursor::gameShowsCursor() {
    auto* manager = GameManager::sharedState();
    return manager && manager->getGameVariable("0024");
}
// 게임 설정 변수 0128: 커서 잠금 여부.
bool versus::cursor::gameLocksCursor() {
    auto* manager = GameManager::sharedState();
    return manager && manager->getGameVariable("0128");
}
void versus::cursor::show(bool unlock) {
    reassertCursor(!unlock && gameLocksCursor());
}
void versus::cursor::relock(bool unlock) {
#if defined(GEODE_IS_WINDOWS) || defined(GEODE_IS_MACOS)
    PlatformToolbox::toggleLockCursor(!unlock && gameLocksCursor());
#else
    (void)unlock;
#endif
}
void versus::cursor::hideForPlay() {
#if defined(GEODE_IS_WINDOWS) || defined(GEODE_IS_MACOS)
    if (Scope::required() || gameShowsCursor()) return;
    PlatformToolbox::hideCursor();
    if (gameLocksCursor()) PlatformToolbox::toggleLockCursor(true);
#endif
}

bool versus::cursor::eclipseLocksCursor() {
#ifdef GEODE_IS_WINDOWS
    return Loader::get()->isModLoaded("eclipse.eclipse-menu") &&
           eclipse::config::getInternal<bool>("global.lockcursor", false);
#else
    return false;
#endif
}

void versus::cursor::warnEclipseLockCursor(bool again) {
    static bool s_warned = false;
    if (!eclipseLocksCursor()) {
        s_warned = false;
        return;
    }
    if (s_warned && !again) return;
    s_warned = true;
    auto* alert = FLAlertLayer::create(
        nullptr, "Lock Cursor",
        "<cy>Eclipse</c>'s <cy>Lock Cursor</c> keeps the mouse in the middle of the window on this screen.\n"
        "Turn it off: Eclipse (<cg>Tab</c>) > <cg>Global</c> > <cg>Lock Cursor</c>.",
        "OK", nullptr, 380.f);
    if (!alert) return;
    // 창을 올려 OK 버튼이 화면 가운데(Eclipse가 커서를 붙잡아 두는 자리)에 오게 한다: 클릭 한 번이면 닫힌다
    // (Esc도 된다). 창 위쪽이 화면 밖으로 나가지 않을 만큼만 올린다.
    auto* item = alert->m_button1 ? alert->m_button1->getParent() : nullptr;
    auto* panel = alert->m_mainLayer ? alert->m_mainLayer->getChildByType<CCScale9Sprite>(0) : nullptr;
    if (item && item->getParent() && panel) {
        auto const win = CCDirector::sharedDirector()->getWinSize();
        auto const button = item->getParent()->convertToWorldSpace(item->getPosition());
        auto const box = panel->boundingBox();
        float const top = alert->m_mainLayer->convertToWorldSpace({box.getMidX(), box.getMaxY()}).y;
        float const lift = std::max(0.f, std::min(win.height / 2.f - button.y, win.height - 6.f - top));
        alert->m_mainLayer->setPositionY(alert->m_mainLayer->getPositionY() + lift);
        if (alert->m_buttonMenu && alert->m_buttonMenu->getParent() != alert->m_mainLayer)
            alert->m_buttonMenu->setPositionY(alert->m_buttonMenu->getPositionY() + lift);
    }
    alert->show();
}

// owner에 붙인 가드 노드를 제거한다.
void versus::releaseMenuCursor(CCNode* owner) {
#ifdef GEODE_IS_WINDOWS
    if (auto* guard = owner ? owner->getChildByID("menu-cursor-guard"_spr) : nullptr) guard->removeFromParent();
#endif
}

#ifdef GEODE_IS_WINDOWS
// Windows 전용 훅. 메뉴 커서가 필요하면 showCursor(false) 요청을 무시하고,
// pollEvents/swapBuffers 앞뒤로 커서를 다시 확인하며, 잠금 요청은 해제로 바꾼다.
namespace {
void showMenuCursor(CCEGLView* view, bool visible) {
    view->showCursor(versus::cursor::Scope::required() ? true : visible);
}
}
$execute {
    auto hook =
        Mod::get()->hook(reinterpret_cast<void*>(addresser::getNonVirtual(Resolve<bool>::func(&CCEGLView::showCursor))),
                         &showMenuCursor, "CCEGLView::showCursor (Versus menus)");
    if (hook.isErr()) log::warn("Menu cursor hook unavailable: {}", hook.unwrapErr());
}
class $modify(VersusMenuCursorView, CCEGLView){void pollEvents(){enforceMenuCursor();
CCEGLView::pollEvents();
enforceMenuCursor();
}
void swapBuffers() {
    enforceMenuCursor();
    CCEGLView::swapBuffers();
    enforceMenuCursor();
}
void toggleLockCursor(bool locked) {
    CCEGLView::toggleLockCursor(versus::cursor::Scope::required() ? false : locked);
}
}
;
#endif
