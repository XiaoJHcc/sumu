// SPDX-FileCopyrightText: sumu Authors
// SPDX-License-Identifier: AGPL-3.0
//
// Embedded browser page (WebView2) -- the transport behind the URL page's address bar.
//
// Scope: this class is deliberately NOT a browser. It creates one windowed WebView2 child
// controller on the Player window, navigates where the address bar points, and reports back
// (a) the page URL the engine is on and (b) the newest media URL the page itself requested,
// with that request's Referer/User-Agent so the native FFmpeg open can replay the same
// credentials. It never injects script, never exposes a host object, file or command bridge
// and never serves or rewrites a response -- only read-only navigation + request observation.
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
// empty result, and a generation token retires answers whose page/candidate is gone.
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

namespace browser {

// True when `url` is an http(s) URL whose PATH component (query string and fragment
// stripped) ends in a container/playlist extension sumu's decoder opens directly:
// mp4/m3u8/mpd/webm/mov/mkv. A query string never hides the extension ("…/v.mp4?t=1" is
// media), and DASH/HLS segment naming (m4s/ts/…) is deliberately not in the list -- those
// are fragments of a playlist, not something to hand to avformat_open_input on their own.
bool looks_like_media_url(const std::string& url);

std::wstring to_wide(const std::string& s);
std::string to_utf8(const std::wstring& w);

} // namespace browser

// Newest media URL a page requested, plus the request context the native open has to
// replay to get the same bytes (hotlink-protected / token-guarded pages reject a bare URL).
struct BrowserMediaCandidate {
    std::string url;
    std::string referer;
    std::string user_agent;

    bool valid() const { return !url.empty(); }
};

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

    // Client-area rectangle (physical pixels, parent-relative) the page view covers, and its
    // visibility. Both are cheap no-ops when nothing changed, so the owner may call them
    // every frame straight from its layout pass.
    void set_bounds(int x, int y, int width, int height);
    void set_visible(bool visible);

    // Latest URL the page view reported (SourceChanged / NavigationCompleted), UTF-8.
    const std::string& page_url() const { return page_url_; }

    const BrowserMediaCandidate& candidate() const { return candidate_; }

    // Drops the media candidate (a new page is loading, or the page is being left). Also drops
    // any cookie answer/lookup that belonged to the old candidate -- otherwise a stale jar
    // could be handed to the next open.
    void clear_candidate();

    // Asks the CookieManager for the cookies that apply to `url` (async). The result is
    // handed back by take_cookies(); a navigation, a new candidate, cancel_cookies() or
    // shutdown() discards a pending request, so a late answer can never be mistaken for the
    // current page's. Every failure outlet still publishes a (possibly empty) result, so a
    // caller waiting on take_cookies() can never hang.
    void request_cookies(const std::string& url);
    // Drops a pending/late cookie lookup without touching the candidate: used when another
    // action (a direct media link, leaving the page) takes over, so the newest action wins.
    void cancel_cookies();
    // True while a request_cookies() lookup is in flight (dropped by cancel/navigation).
    bool cookie_pending() const { return cookie_pending_; }
    // True once a request_cookies() result is available: fills `url`/`cookies` and consumes
    // it. `cookies` is the "name=value; name2=value2" request-header form.
    bool take_cookies(std::string& url, std::string& cookies);

    // True while the page view was asked to mute itself (it is muted whenever it is hidden, so
    // a closed/loading/parked page never keeps its own audio playing over the video).
    bool muted() const { return muted_; }

    // Synchronous teardown (controller Close + COM uninit). Idempotent; must run before the
    // parent HWND is destroyed.
    void shutdown();

private:
    void fail(const char* what);
    void configure_webview();
    void apply_bounds();
    void record_candidate(const std::string& url, const std::string& referer,
                          const std::string& user_agent);
    void on_navigation_started();
    void on_source_maybe_changed();
    // Publishes a cookie lookup result (possibly empty) into the take_cookies() slot and
    // clears the in-flight state. Every request_cookies() exit path funnels through here.
    void publish_cookies(const std::string& url, std::string jar);

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
    BrowserMediaCandidate candidate_;

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

    // Shared with every late COM callback: false after shutdown(), so a callback that lands
    // after teardown returns without touching this object.
    std::shared_ptr<std::atomic<bool>> alive_;

    struct Impl;
    std::unique_ptr<Impl> impl_;
};
