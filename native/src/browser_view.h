// SPDX-FileCopyrightText: sumu Authors
// SPDX-License-Identifier: AGPL-3.0
//
// Embedded browser page (WebView2) -- the transport behind the URL page's address bar.
//
// Scope: this class is deliberately NOT a browser. It creates one windowed WebView2 child
// controller on the Player window, navigates where the address bar points, and reports back the
// page URL the engine is on plus a PNG of the page (settings overlay backdrop) and the Cookie
// jar for a URL (so a manually started native open can replay the page's credentials). It never
// observes or rewrites page traffic, never injects script, and never exposes a host object, file
// or command bridge -- only navigation, one-shot capture and one-shot cookie lookup.
//
// WebView2 headers stay in browser_view.cpp: Player holds this by forward-declared
// unique_ptr (player.h), so the SDK's ~60k-line COM header never leaks into the present
// kernel's translation units.
//
// Threading: every method, every COM callback and the whole controller lifetime belong to
// the UI thread that owns the Player window and its message pump (Player::pump_messages()).
// WebView2 marshals its async callbacks to that thread, so no lock is needed -- and the
// cookie result is POLLED by the owner (take_cookies()) rather than pushed through a
// callback, which keeps the "晚到回调访问已销毁 Player" hazard out of the picture entirely.
// The polled slot always ends up filled: every request_cookies() failure outlet publishes an
// empty result, and a generation token retires answers whose URL is no longer the one asked for.
//
// Runtime dependency: the system Evergreen WebView2 Runtime (no fixed runtime is shipped).
// When it is missing or the controller cannot be created, failed() turns true and the
// owner draws its "browser unavailable" page -- Player construction, shutdown and the
// native direct-link open path never depend on this object.

#pragma once

#include <windows.h>

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace browser {

std::wstring to_wide(const std::string& s);
std::string to_utf8(const std::wstring& w);

} // namespace browser

class BrowserView {
public:
    BrowserView();
    ~BrowserView();

    BrowserView(const BrowserView&) = delete;
    BrowserView& operator=(const BrowserView&) = delete;

    // Starts (async) the Evergreen environment + controller for `parent`. Idempotent; the
    // completion callbacks arrive through the parent's message pump.
    void ensure_started(HWND parent);

    bool ready() const { return controller_ready_; }
    bool failed() const { return failed_; }
    const std::string& error() const { return error_; }

    // Navigates the page view. Ignored before ready() / after failure.
    void navigate(const std::string& url);

    // Browser history: the address bar's back/forward/reload. All are no-ops before ready().
    // CanGoBack/CanGoForward report the engine's live state (with the reload/navigation in
    // flight they may lag one tick -- WebView2 exposes them as plain properties).
    void go_back();
    void go_forward();
    void reload();
    bool can_go_back() const;
    bool can_go_forward() const;

    // Client-area rectangle (physical pixels, parent-relative) the page view covers, and its
    // visibility. Both are cheap no-ops when nothing changed, so the owner may call them
    // every frame straight from its layout pass.
    void set_bounds(int x, int y, int width, int height);
    void set_visible(bool visible);

    // Latest URL the page view reported (SourceChanged / NavigationCompleted), UTF-8.
    const std::string& page_url() const { return page_url_; }

    // Page snapshot for the settings overlay (see docs/native_core.md): an async
    // CapturePreview() into memory, POLLED like the cookie result so no callback ever touches
    // the owner. Ready gives PNG bytes; Failed means the caller should fall back to the plain
    // "hide the page view" overlay. A newer request, a navigation or shutdown() retires an
    // older one.
    enum class Snapshot { None, Ready, Failed };
    void request_snapshot();
    Snapshot take_snapshot(std::vector<uint8_t>& png);

    // Asks the CookieManager for the cookies that apply to `url` (async) -- the manual
    // native-play button's replay context. The result is handed back by take_cookies(); a
    // newer request, cancel_cookies() or shutdown() discards a pending one, so a late answer
    // can never be mistaken for the current one. Every failure outlet still publishes a
    // (possibly empty) result, so a caller waiting on take_cookies() can never hang.
    void request_cookies(const std::string& url);
    // Drops a pending/late cookie lookup: used when another action (a new play click, leaving
    // the page) takes over, so the newest action wins.
    void cancel_cookies();
    // True while a request_cookies() lookup is in flight (dropped by cancel/navigation).
    bool cookie_pending() const { return cookie_pending_; }
    // True once a request_cookies() result is available: fills `url`/`cookies` and consumes
    // it. `cookies` is the "name=value; name2=value2" request-header form.
    bool take_cookies(std::string& url, std::string& cookies);

    // True while the page view was asked to mute itself (it is muted whenever it is hidden, so
    // a closed/loading/parked page never keeps its own audio playing over the video).
    bool muted() const { return muted_; }

    // True while the page view's child window is shown (last applied visibility).
    bool visible() const { return visible_; }

    // Last applied client-area rectangle (x, y, w, h) -- for scripted assertions only.
    void bounds(int out[4]) const { for (int i = 0; i < 4; ++i) out[i] = bounds_[i]; }

    // Re-applies the resize-forwarding subclass to any child window that appeared since the
    // last call (WebView2 builds/rebuilds its window tree lazily). Idempotent and cheap; the
    // Player calls it once per UI tick while the page is up.
    void keep_child_subclasses();

    // Synchronous teardown (controller Close + COM uninit). Idempotent; must run before the
    // parent HWND is destroyed.
    void shutdown();

private:
    void fail(const char* what);
    void configure_webview();
    void apply_bounds();
    void on_navigation_started();
    void on_source_maybe_changed();
    // Publishes a cookie lookup result (possibly empty) into the take_cookies() slot and
    // clears the in-flight state. Every request_cookies() exit path funnels through here.
    void publish_cookies(const std::string& url, std::string jar);
    void publish_snapshot(std::vector<uint8_t> png);

    // ---- non-client resize forwarding (best effort, our own process only) -------------
    // Measured window chain (see docs/native_core.md):
    //   Sumu_Player_WndClass -> Chrome_WidgetWin_0 (OUR process)
    //                        -> Chrome_WidgetWin_1 / Chrome_RenderWidgetHostHWND (WebView2
    //                           browser process) -> Intermediate D3D Window (GPU process)
    // Only the windows owned by our process can be subclassed, and the OS hit-tests the
    // DEEPEST window -- which belongs to another process. So this subclassing is a best-effort
    // first line of defense for the outer window we do own (it adopts the parent's border code
    // for WM_NCHITTEST and forwards WM_NCLBUTTON*): it cannot reach the cross-process children
    // and therefore does NOT by itself make the borders draggable over the page. That half is
    // covered by Player::poll_page_view_border() (our own per-tick drag). No padding is added
    // around the page.
    void install_child_subclasses();
    void remove_child_subclasses();
    static LRESULT CALLBACK child_subclass_proc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam,
                                                UINT_PTR id, DWORD_PTR ref);

    bool started_ = false;
    bool com_initialized_ = false;
    bool controller_ready_ = false;
    bool failed_ = false;
    std::string error_;
    HWND parent_ = nullptr;

    // Child-window placement, in parent client-area physical pixels.
    int bounds_[4] = { 0, 0, 0, 0 };
    bool bounds_valid_ = false;
    bool visible_ = false;

    std::string page_url_;

    // Page snapshot handoff (see request_snapshot()): generation-guarded, polled.
    uint64_t snapshot_gen_ = 0;
    bool snapshot_pending_ = false;
    Snapshot snapshot_state_ = Snapshot::None;
    std::vector<uint8_t> snapshot_png_;

    // Cookie handoff (polled by take_cookies(), push-free): `cookie_pending_` is the in-flight
    // lookup for `cookie_url_`, the `cookie_done_` slot holds its answer. `cookie_gen_` is
    // bumped by every request/cancel so an answer that arrives after the slot moved on is
    // dropped instead of being published as current.
    uint64_t cookie_gen_ = 0;
    bool cookie_pending_ = false;
    std::string cookie_url_;
    bool cookie_done_ = false;
    std::string cookie_result_url_;
    std::string cookie_result_;
    bool muted_ = false; // last IsMuted state we successfully applied
    // Descendant HWNDs of the parent that currently carry the resize-forwarding subclass.
    std::vector<HWND> subclassed_;

    // Shared with every late COM callback: false after shutdown(), so a callback that lands
    // after teardown returns without touching this object.
    std::shared_ptr<std::atomic<bool>> alive_;

    struct Impl;
    std::unique_ptr<Impl> impl_;
};
