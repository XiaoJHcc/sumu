// SPDX-FileCopyrightText: sumu Authors
// SPDX-License-Identifier: AGPL-3.0
//
// WebView2 implementation of the embedded browser page -- see browser_view.h for the
// contract (windowed child controller, navigation + one-shot capture/cookie lookup,
// main-thread only).

#include "browser_view.h"

#include <commctrl.h> // SetWindowSubclass / RemoveWindowSubclass (comctl32)

#include <wrl/client.h>
#include <wrl/implements.h> // Microsoft::WRL::Callback (COM callback → lambda adapter)

#include "WebView2.h"

#include <cstdlib>
#include <cstdio>
#include <filesystem>

using Microsoft::WRL::ComPtr;

namespace {

// ---- lambda -> COM event handler -----------------------------------------------------------
// WebView2 hands out raw COM interfaces for every async completion / event. WRL's Callback<>
// adapter is not available in every Windows SDK's wrl headers (it is absent from the one this
// project builds against), so the adapter is 40 lines here: deduce the handler interface's
// Invoke() signature from its member pointer and forward it into a lambda.
template <typename M>
struct InvokeSignature;
template <typename C, typename... A>
struct InvokeSignature<HRESULT(STDMETHODCALLTYPE C::*)(A...)> {
    using type = HRESULT(A...);
};

template <typename Interface, typename Fn, typename Signature>
class LambdaHandler;

template <typename Interface, typename Fn, typename... A>
class LambdaHandler<Interface, Fn, HRESULT(A...)> : public Interface {
public:
    explicit LambdaHandler(Fn fn) : fn_(std::move(fn)) {}

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** out) override {
        if (!out) return E_POINTER;
        if (riid == __uuidof(IUnknown) || riid == __uuidof(Interface)) {
            *out = static_cast<Interface*>(this);
            AddRef();
            return S_OK;
        }
        *out = nullptr;
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override {
        return static_cast<ULONG>(InterlockedIncrement(&refs_));
    }
    ULONG STDMETHODCALLTYPE Release() override {
        const ULONG left = static_cast<ULONG>(InterlockedDecrement(&refs_));
        if (left == 0) delete this;
        return left;
    }
    HRESULT STDMETHODCALLTYPE Invoke(A... args) override { return fn_(args...); }

private:
    Fn fn_;
    LONG refs_ = 1;
};

template <typename Interface, typename Fn>
ComPtr<Interface> MakeHandler(Fn fn){
    using Signature = typename InvokeSignature<decltype(&Interface::Invoke)>::type;
    ComPtr<Interface> handler;
    handler.Attach(new LambdaHandler<Interface, Fn, Signature>(std::move(fn)));
    return handler;
}

} // namespace

namespace {

constexpr wchar_t kUserDataSubdir[] = L"sumu\\WebView2";

// Environment user-data folder: %LOCALAPPDATA%\sumu\WebView2 (never the repo, never a
// relative path -- WebView2 would otherwise drop its profile next to the process CWD).
// %TEMP% is a last-resort fallback for a stripped environment; both are created if absent.
std::wstring user_data_folder(){
    wchar_t buf[MAX_PATH * 2] = {};
    DWORD n = GetEnvironmentVariableW(L"LOCALAPPDATA", buf,
        static_cast<DWORD>(sizeof(buf) / sizeof(buf[0])));
    std::wstring base;
    if (n > 0 && n < sizeof(buf) / sizeof(buf[0])) base.assign(buf, n);
    if (base.empty()) {
        wchar_t tmp[MAX_PATH] = {};
        DWORD tn = GetTempPathW(static_cast<DWORD>(sizeof(tmp) / sizeof(tmp[0])), tmp);
        if (tn > 0) base.assign(tmp, tn);
    }
    if (base.empty()) return std::wstring();
    std::wstring full = base;
    if (full.back() != L'\\') full.push_back(L'\\');
    full += kUserDataSubdir;
    std::error_code ec;
    std::filesystem::create_directories(std::filesystem::path(full), ec);
    return full;
}

} // namespace

namespace browser {

std::wstring to_wide(const std::string& s){
    if (s.empty()) return std::wstring();
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
    if (n <= 0) return std::wstring();
    std::wstring w(static_cast<size_t>(n - 1), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, w.data(), n);
    return w;
}

std::string to_utf8(const std::wstring& w){
    if (w.empty()) return std::string();
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, nullptr, 0, nullptr, nullptr);
    if (n <= 0) return std::string();
    std::string s(static_cast<size_t>(n - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, s.data(), n, nullptr, nullptr);
    return s;
}

} // namespace browser

// Every WebView2 COM pointer + event token lives here so browser_view.h stays free of the
// SDK headers (see its header comment).
struct BrowserView::Impl {
    ComPtr<ICoreWebView2Environment> env;
    ComPtr<ICoreWebView2Controller> controller;
    ComPtr<ICoreWebView2> webview;
    // ICoreWebView2_2 carries the CookieManager (the manual native-play button's replay jar).
    ComPtr<ICoreWebView2_2> webview2;
    // ICoreWebView2_8 (IsMuted) is queried once; older Evergreen runtimes leave it null and the
    // page simply cannot be muted (everything else keeps working).
    ComPtr<ICoreWebView2_8> webview8;
    EventRegistrationToken tok_source{};
    EventRegistrationToken tok_nav_starting{};
    EventRegistrationToken tok_nav_completed{};
};

BrowserView::BrowserView()
    : alive_(std::make_shared<std::atomic<bool>>(true)), impl_(std::make_unique<Impl>()) {}

BrowserView::~BrowserView(){ shutdown(); }

void BrowserView::fail(const char* what){
    if (failed_) return;
    failed_ = true;
    error_ = what ? what : "browser unavailable";
    controller_ready_ = false;
    fprintf(stderr, "[sumu] browser unavailable: %s\n", error_.c_str());
}

void BrowserView::ensure_started(HWND parent){
    if (started_ || failed_) return;
    started_ = true;
    parent_ = parent;
    // Dev/verification hook (same spirit as Player's SUMU_UI_SCALE): pretend the runtime is
    // missing so the page's browser_unavailable degradation is testable on a machine that does
    // have WebView2 installed.
    if (const char* off = getenv("SUMU_WEBVIEW2_OFF"); off && off[0] && off[0] != '0') {
        fail("disabled by SUMU_WEBVIEW2_OFF");
        return;
    }
    if (!parent) { fail("no parent window"); return; }

    // WebView2 needs an STA on the thread that owns (and pumps) the controller. S_FALSE means
    // "already initialized in this mode" -- still balanced by the matching CoUninitialize.
    // RPC_E_CHANGED_MODE means this thread is already in another apartment model: a
    // controller cannot be created there and uninitializing someone else's apartment would
    // corrupt the host, so the page just reports itself unavailable.
    HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (hr == S_OK || hr == S_FALSE) {
        com_initialized_ = true;
    } else if (hr == RPC_E_CHANGED_MODE) {
        fail("COM apartment mismatch (thread already initialized)");
        return;
    } else if (FAILED(hr)) {
        fail("CoInitializeEx failed");
        return;
    }

    const std::wstring data_dir = user_data_folder();
    const std::shared_ptr<std::atomic<bool>> alive = alive_;
    hr = CreateCoreWebView2EnvironmentWithOptions(nullptr,
        data_dir.empty() ? nullptr : data_dir.c_str(), nullptr,
        MakeHandler<ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler>(
            [this, alive, parent](HRESULT result, ICoreWebView2Environment* env) -> HRESULT {
                if (!alive->load()) return S_OK;
                if (result != S_OK || !env) {
                    fail("WebView2 Runtime missing or environment creation failed");
                    return S_OK;
                }
                impl_->env = env;
                HRESULT cr = impl_->env->CreateCoreWebView2Controller(parent,
                    MakeHandler<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler>(
                        [this, alive](HRESULT res, ICoreWebView2Controller* controller) -> HRESULT {
                            if (!alive->load()) return S_OK;
                            if (res != S_OK || !controller) {
                                fail("WebView2 controller creation failed");
                                return S_OK;
                            }
                            impl_->controller = controller;
                            if (FAILED(impl_->controller->get_CoreWebView2(&impl_->webview)) ||
                                !impl_->webview) {
                                fail("WebView2 CoreWebView2 unavailable");
                                return S_OK;
                            }
                            configure_webview();
                            controller_ready_ = true;
                            apply_bounds();
                            return S_OK;
                        }).Get());
                if (FAILED(cr)) fail("CreateCoreWebView2Controller failed to start");
                return S_OK;
            }).Get());
    if (FAILED(hr)) fail("CreateCoreWebView2EnvironmentWithOptions failed");
}

// Page-view settings: no context menus / devtools / status bar / web-message bridge. The
// page keeps its own error page for failed navigations (WebView2 default) -- the owner adds
// no chrome of its own beyond the address bar.
void BrowserView::configure_webview(){
    if (FAILED(impl_->webview.As(&impl_->webview8))) impl_->webview8.Reset();
    if (FAILED(impl_->webview.As(&impl_->webview2))) impl_->webview2.Reset();
    ComPtr<ICoreWebView2Settings> settings;
    if (SUCCEEDED(impl_->webview->get_Settings(&settings)) && settings) {
        settings->put_AreDefaultContextMenusEnabled(FALSE);
        settings->put_AreDevToolsEnabled(FALSE);
        settings->put_IsStatusBarEnabled(FALSE);
        settings->put_IsWebMessageEnabled(FALSE);
        settings->put_AreHostObjectsAllowed(FALSE);
        settings->put_IsZoomControlEnabled(FALSE);
    }

    const std::shared_ptr<std::atomic<bool>> alive = alive_;
    impl_->webview->add_NavigationStarting(
        MakeHandler<ICoreWebView2NavigationStartingEventHandler>(
            [this, alive](ICoreWebView2*, ICoreWebView2NavigationStartingEventArgs*) -> HRESULT {
                if (alive->load()) on_navigation_started();
                return S_OK;
            }).Get(), &impl_->tok_nav_starting);
    impl_->webview->add_SourceChanged(
        MakeHandler<ICoreWebView2SourceChangedEventHandler>(
            [this, alive](ICoreWebView2*, ICoreWebView2SourceChangedEventArgs*) -> HRESULT {
                if (alive->load()) on_source_maybe_changed();
                return S_OK;
            }).Get(), &impl_->tok_source);
    impl_->webview->add_NavigationCompleted(
        MakeHandler<ICoreWebView2NavigationCompletedEventHandler>(
            [this, alive](ICoreWebView2*, ICoreWebView2NavigationCompletedEventArgs*) -> HRESULT {
                if (alive->load()) on_source_maybe_changed();
                return S_OK;
            }).Get(), &impl_->tok_nav_completed);
}

void BrowserView::on_navigation_started(){
    // A new document: a snapshot taken for the old page must not back the settings overlay any
    // more.
    ++snapshot_gen_;
    snapshot_pending_ = false;
    snapshot_state_ = Snapshot::None;
    snapshot_png_.clear();
}

void BrowserView::on_source_maybe_changed(){
    if (!impl_->webview) return;
    LPWSTR raw = nullptr;
    if (FAILED(impl_->webview->get_Source(&raw)) || !raw) return;
    std::string url = browser::to_utf8(std::wstring(raw));
    CoTaskMemFree(raw);
    if (url.empty() || url == page_url_) return;
    page_url_ = std::move(url);
}

void BrowserView::navigate(const std::string& url){
    if (!controller_ready_ || !impl_->webview || url.empty()) return;
    impl_->webview->Navigate(browser::to_wide(url).c_str());
}

void BrowserView::go_back(){
    if (controller_ready_ && impl_->webview) impl_->webview->GoBack();
}

void BrowserView::go_forward(){
    if (controller_ready_ && impl_->webview) impl_->webview->GoForward();
}

void BrowserView::reload(){
    if (controller_ready_ && impl_->webview) impl_->webview->Reload();
}

bool BrowserView::can_go_back() const {
    if (!controller_ready_ || !impl_->webview) return false;
    BOOL can = FALSE;
    return SUCCEEDED(impl_->webview->get_CanGoBack(&can)) && can;
}

bool BrowserView::can_go_forward() const {
    if (!controller_ready_ || !impl_->webview) return false;
    BOOL can = FALSE;
    return SUCCEEDED(impl_->webview->get_CanGoForward(&can)) && can;
}

// ---- non-client resize forwarding (best effort) -------------------------------------------
// See the declaration's comment in browser_view.h: only our-process windows can be subclassed
// and the OS hit-tests the deepest (cross-process) one, so this cannot make the page view's
// borders draggable by itself. Player::poll_page_view_border() is the half that does.
namespace {

constexpr UINT_PTR kChildSubclassId = 0x53554D55; // 'SUMU'

bool is_border_hit(LRESULT hit){
    switch (hit) {
    case HTLEFT: case HTRIGHT: case HTTOP: case HTBOTTOM:
    case HTTOPLEFT: case HTTOPRIGHT: case HTBOTTOMLEFT: case HTBOTTOMRIGHT:
        return true;
    default:
        return false;
    }
}

} // namespace

LRESULT CALLBACK BrowserView::child_subclass_proc(HWND hwnd, UINT msg, WPARAM wparam,
                                                  LPARAM lparam, UINT_PTR id, DWORD_PTR ref){
    (void)id;
    HWND parent = reinterpret_cast<HWND>(ref);
    switch (msg) {
    case WM_NCHITTEST: {
        // Ask the parent window the same question it answers for its own chrome: the point
        // (screen coords) lands on one of its resize bands -> the page view adopts that code,
        // so dragging the border works even though the child window covers it.
        const LRESULT hit = SendMessageW(parent, WM_NCHITTEST, wparam, lparam);
        if (is_border_hit(hit)) return hit;
        break;
    }
    case WM_NCLBUTTONDOWN:
    case WM_NCLBUTTONUP:
    case WM_NCLBUTTONDBLCLK:
        // Only these three carry a non-client hit code, and they are only sent here because our
        // WM_NCHITTEST above adopted one -- hand them to the parent so DefWindowProc starts
        // (or ends) its native sizing/move loop on the toplevel window.
        return SendMessageW(parent, msg, wparam, lparam);
    default:
        break;
    }
    return DefSubclassProc(hwnd, msg, wparam, lparam);
}

void BrowserView::install_child_subclasses(){
    if (!parent_ || !IsWindow(parent_)) return;
    struct Ctx {
        HWND parent;
        std::vector<HWND>* list;
    } ctx{ parent_, &subclassed_ };
    EnumChildWindows(parent_, [](HWND child, LPARAM lp) -> BOOL {
        Ctx* c = reinterpret_cast<Ctx*>(lp);
        if (child == c->parent) return TRUE;
        for (HWND known : *c->list)
            if (known == child) return TRUE; // already subclassed (idempotent)
        if (SetWindowSubclass(child, &BrowserView::child_subclass_proc, kChildSubclassId,
                reinterpret_cast<DWORD_PTR>(c->parent)))
            c->list->push_back(child);
        return TRUE;
    }, reinterpret_cast<LPARAM>(&ctx));
}

void BrowserView::remove_child_subclasses(){
    for (HWND child : subclassed_)
        if (IsWindow(child)) RemoveWindowSubclass(child, &BrowserView::child_subclass_proc, kChildSubclassId);
    subclassed_.clear();
}

void BrowserView::keep_child_subclasses(){
    if (controller_ready_) install_child_subclasses();
}

void BrowserView::set_bounds(int x, int y, int width, int height){
    if (width < 0) width = 0;
    if (height < 0) height = 0;
    if (bounds_valid_ && x == bounds_[0] && y == bounds_[1] &&
        width == bounds_[2] && height == bounds_[3])
        return;
    bounds_[0] = x; bounds_[1] = y; bounds_[2] = width; bounds_[3] = height;
    bounds_valid_ = true;
    apply_bounds();
}

void BrowserView::set_visible(bool visible){
    if (visible_ == visible) return;
    visible_ = visible;
    apply_bounds();
}

void BrowserView::apply_bounds(){
    if (!controller_ready_ || !impl_->controller) return;
    RECT rc{ bounds_[0], bounds_[1], bounds_[0] + bounds_[2], bounds_[1] + bounds_[3] };
    impl_->controller->put_Bounds(rc);
    impl_->controller->put_IsVisible(visible_ ? TRUE : FALSE);
    // A hidden page view (loading card up, page closed, window parked) must not keep playing
    // its own audio over the video / over a native open started from it. Mute follows
    // visibility; unmuting is part of showing the page again. Older runtimes without
    // ICoreWebView2_8 skip this (nothing else depends on it).
    if (impl_->webview8 &&
        SUCCEEDED(impl_->webview8->put_IsMuted(visible_ ? FALSE : TRUE)))
        muted_ = !visible_;
    // WebView2 builds its child window tree lazily (and may add windows later), so the resize
    // forwarding subclass is (re)installed here -- every call is idempotent per HWND.
    install_child_subclasses();
}

void BrowserView::cancel_cookies(){
    // Bumping the generation retires answers that are already in the browser's queue: their
    // handler no longer matches and returns without publishing anything.
    ++cookie_gen_;
    cookie_pending_ = false;
    cookie_url_.clear();
    cookie_done_ = false;
    cookie_result_url_.clear();
    cookie_result_.clear();
}

void BrowserView::publish_cookies(const std::string& url, std::string jar){
    cookie_pending_ = false;
    cookie_url_.clear();
    cookie_done_ = true;
    cookie_result_url_ = url;
    cookie_result_ = std::move(jar);
}

void BrowserView::request_cookies(const std::string& url){
    // Every exit path below either publishes a real answer or publishes an EMPTY one: the
    // caller polls take_cookies() to finish a native open, so a lookup that never answers
    // would leave its loading state (and the play button) spinning forever. An empty jar just
    // means "no cookies to replay" -- the real open then succeeds or reports its own failure.
    const uint64_t gen = ++cookie_gen_;
    cookie_pending_ = true;
    cookie_url_ = url;
    cookie_done_ = false; // a previous answer nobody took belongs to the previous request
    cookie_result_url_.clear();
    cookie_result_.clear();

    if (!controller_ready_ || !impl_->webview || url.empty()) {
        publish_cookies(url, std::string());
        return;
    }
    if (!impl_->webview2) {
        publish_cookies(url, std::string());
        return;
    }
    ComPtr<ICoreWebView2CookieManager> manager;
    if (FAILED(impl_->webview2->get_CookieManager(&manager)) || !manager) {
        publish_cookies(url, std::string());
        return;
    }

    const std::shared_ptr<std::atomic<bool>> alive = alive_;
    HRESULT hr = manager->GetCookies(browser::to_wide(url).c_str(),
        MakeHandler<ICoreWebView2GetCookiesCompletedHandler>(
            [this, alive, url, gen](HRESULT result, ICoreWebView2CookieList* list) -> HRESULT {
                if (!alive->load()) return S_OK;
                // Stale answer (page left / a newer request was issued since): the generation no
                // longer matches, so drop it silently instead of publishing it as the current
                // URL's cookies.
                if (gen != cookie_gen_) return S_OK;
                std::string jar;
                UINT count = 0;
                if (result == S_OK && list) list->get_Count(&count);
                for (UINT i = 0; i < count; ++i) {
                    ComPtr<ICoreWebView2Cookie> cookie;
                    if (FAILED(list->GetValueAtIndex(i, &cookie)) || !cookie) continue;
                    LPWSTR name = nullptr, value = nullptr;
                    if (FAILED(cookie->get_Name(&name)) || !name) continue;
                    if (FAILED(cookie->get_Value(&value)) || !value) {
                        CoTaskMemFree(name);
                        continue;
                    }
                    if (!jar.empty()) jar += "; ";
                    jar += browser::to_utf8(std::wstring(name));
                    jar += '=';
                    jar += browser::to_utf8(std::wstring(value));
                    CoTaskMemFree(name);
                    CoTaskMemFree(value);
                }
                // A failed GetCookies (result != S_OK) publishes the empty jar it accumulated
                // -- same "no cookies to replay" outcome as a page that sets none.
                publish_cookies(url, std::move(jar));
                return S_OK;
            }).Get());
    if (FAILED(hr)) publish_cookies(url, std::string());
}

bool BrowserView::take_cookies(std::string& url, std::string& cookies){
    if (!cookie_done_) return false;
    cookie_done_ = false;
    url = std::move(cookie_result_url_);
    cookies = std::move(cookie_result_);
    cookie_result_url_.clear();
    cookie_result_.clear();
    return true;
}

// ---- page snapshot (settings overlay backdrop) -------------------------------------------
// WebView2's windowed controller paints into its own child HWND, so ImGui cannot draw anything
// on top of the live page. The settings overlay therefore asks the engine for a PNG of the page,
// draws THAT under the panel and hides the live view -- the page session, history and URL are
// untouched, and closing the panel brings the same controller back.
void BrowserView::request_snapshot(){
    const uint64_t gen = ++snapshot_gen_;
    snapshot_state_ = Snapshot::None;
    snapshot_png_.clear();
    if (!controller_ready_ || !impl_->webview) {
        snapshot_pending_ = false;
        snapshot_state_ = Snapshot::Failed;
        return;
    }
    ComPtr<IStream> stream;
    if (FAILED(CreateStreamOnHGlobal(nullptr, TRUE, &stream)) || !stream) {
        snapshot_pending_ = false;
        snapshot_state_ = Snapshot::Failed;
        return;
    }
    snapshot_pending_ = true;
    const std::shared_ptr<std::atomic<bool>> alive = alive_;
    HRESULT hr = impl_->webview->CapturePreview(COREWEBVIEW2_CAPTURE_PREVIEW_IMAGE_FORMAT_PNG,
        stream.Get(),
        MakeHandler<ICoreWebView2CapturePreviewCompletedHandler>(
            [this, alive, stream, gen](HRESULT result) -> HRESULT {
                if (!alive->load()) return S_OK;
                if (gen != snapshot_gen_) return S_OK; // retired by a newer request/navigation
                snapshot_pending_ = false;
                if (result != S_OK) {
                    snapshot_state_ = Snapshot::Failed;
                    return S_OK;
                }
                STATSTG stat{};
                if (FAILED(stream->Stat(&stat, STATFLAG_NONAME)) || stat.cbSize.QuadPart <= 0) {
                    snapshot_state_ = Snapshot::Failed;
                    return S_OK;
                }
                LARGE_INTEGER zero{};
                if (FAILED(stream->Seek(zero, STREAM_SEEK_SET, nullptr))) {
                    snapshot_state_ = Snapshot::Failed;
                    return S_OK;
                }
                std::vector<uint8_t> png(static_cast<size_t>(stat.cbSize.QuadPart));
                ULONG read = 0;
                if (FAILED(stream->Read(png.data(), static_cast<ULONG>(png.size()), &read)) ||
                    read != png.size()) {
                    snapshot_state_ = Snapshot::Failed;
                    return S_OK;
                }
                publish_snapshot(std::move(png));
                return S_OK;
            }).Get());
    if (FAILED(hr)) {
        snapshot_pending_ = false;
        snapshot_state_ = Snapshot::Failed;
    }
}

void BrowserView::publish_snapshot(std::vector<uint8_t> png){
    if (png.empty()) {
        snapshot_state_ = Snapshot::Failed;
        return;
    }
    snapshot_png_ = std::move(png);
    snapshot_state_ = Snapshot::Ready;
}

BrowserView::Snapshot BrowserView::take_snapshot(std::vector<uint8_t>& png){
    if (snapshot_state_ == Snapshot::None) return Snapshot::None;
    const Snapshot state = snapshot_state_;
    snapshot_state_ = Snapshot::None;
    if (state == Snapshot::Ready) png = std::move(snapshot_png_);
    snapshot_png_.clear();
    return state;
}

void BrowserView::shutdown(){
    if (!alive_) return;
    alive_->store(false);
    cancel_cookies(); // no late answer may be published after teardown
    ++snapshot_gen_;
    snapshot_pending_ = false;
    snapshot_state_ = Snapshot::None;
    snapshot_png_.clear();
    remove_child_subclasses(); // before the child windows go away with the controller
    if (impl_) {
        if (impl_->webview) {
            impl_->webview->remove_SourceChanged(impl_->tok_source);
            impl_->webview->remove_NavigationStarting(impl_->tok_nav_starting);
            impl_->webview->remove_NavigationCompleted(impl_->tok_nav_completed);
            impl_->webview.Reset();
        }
        // Close() (not just Release) tears down the child window/process synchronously --
        // required before the parent HWND goes away.
        if (impl_->controller) {
            impl_->controller->Close();
            impl_->controller.Reset();
        }
        impl_->env.Reset();
    }
    controller_ready_ = false;
    if (com_initialized_) {
        CoUninitialize();
        com_initialized_ = false;
    }
}
