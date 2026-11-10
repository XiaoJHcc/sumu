// SPDX-FileCopyrightText: sumu Authors
// SPDX-License-Identifier: AGPL-3.0
//
// WebView2 implementation of the embedded browser page -- see browser_view.h for the
// contract (windowed child controller, read-only navigation/observation, main-thread only).

#include "browser_view.h"

#include <wrl/client.h>
#include <wrl/implements.h> // Microsoft::WRL::Callback (COM callback → lambda adapter)

#include "WebView2.h"

#include <algorithm>
#include <cctype>
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

std::string header_of(ICoreWebView2WebResourceRequest* request, LPCWSTR name){
    if (!request) return std::string();
    ComPtr<ICoreWebView2HttpRequestHeaders> headers;
    if (FAILED(request->get_Headers(&headers)) || !headers) return std::string();
    LPWSTR value = nullptr;
    if (FAILED(headers->GetHeader(name, &value)) || !value) return std::string();
    std::string out = browser::to_utf8(std::wstring(value));
    CoTaskMemFree(value);
    return out;
}

std::string uri_of(ICoreWebView2WebResourceRequest* request){
    if (!request) return std::string();
    LPWSTR uri = nullptr;
    if (FAILED(request->get_Uri(&uri)) || !uri) return std::string();
    std::string out = browser::to_utf8(std::wstring(uri));
    CoTaskMemFree(uri);
    return out;
}

bool has_media_extension(const std::string& path){
    static const char* const kExt[] = { ".mp4", ".m3u8", ".mpd", ".webm", ".mov", ".mkv" };
    const size_t dot = path.find_last_of('.');
    if (dot == std::string::npos) return false;
    std::string ext = path.substr(dot);
    for (char& c : ext)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    for (const char* e : kExt)
        if (ext == e) return true;
    return false;
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

bool looks_like_media_url(const std::string& url){
    // Scheme: http(s) only -- the decoder's own network profile keys off the same prefixes.
    auto starts_with_ci = [&](const char* prefix) {
        const size_t n = std::char_traits<char>::length(prefix);
        if (url.size() < n) return false;
        for (size_t i = 0; i < n; ++i)
            if (std::tolower(static_cast<unsigned char>(url[i])) !=
                std::tolower(static_cast<unsigned char>(prefix[i])))
                return false;
        return true;
    };
    std::string::size_type host_start = 0;
    if (starts_with_ci("http://")) host_start = 7;
    else if (starts_with_ci("https://")) host_start = 8;
    else return false;
    // Path only: a query string / fragment must not affect extension detection
    // ("/stream.mp4?token=…" is a direct file; "/watch?f=movie.mp4" is a page).
    std::string path = url.substr(host_start);
    const size_t cut = path.find_first_of("?#");
    if (cut != std::string::npos) path.resize(cut);
    // Skip the authority: the extension check must look at the path, not the host
    // ("https://cdn.mp4.example.com/watch" is not a media file).
    const size_t slash = path.find('/');
    if (slash == std::string::npos) return false;
    return has_media_extension(path.substr(slash));
}

} // namespace browser

// Every WebView2 COM pointer + event token lives here so browser_view.h stays free of the
// SDK headers (see its header comment).
struct BrowserView::Impl {
    ComPtr<ICoreWebView2Environment> env;
    ComPtr<ICoreWebView2Controller> controller;
    ComPtr<ICoreWebView2> webview;
    // ICoreWebView2_8 (IsMuted) is queried once; older Evergreen runtimes leave it null and the
    // page simply cannot be muted (everything else keeps working).
    ComPtr<ICoreWebView2_8> webview8;
    EventRegistrationToken tok_source{};
    EventRegistrationToken tok_nav_starting{};
    EventRegistrationToken tok_nav_completed{};
    EventRegistrationToken tok_web_resource{};
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
    // Observe (never modify) every request so the page's own media URL + Referer/User-Agent
    // can be replayed by the native open. Reading the request is all this handler does --
    // no response, header or body is touched.
    impl_->webview->AddWebResourceRequestedFilter(L"*", COREWEBVIEW2_WEB_RESOURCE_CONTEXT_ALL);
    impl_->webview->add_WebResourceRequested(
        MakeHandler<ICoreWebView2WebResourceRequestedEventHandler>(
            [this, alive](ICoreWebView2*, ICoreWebView2WebResourceRequestedEventArgs* args) -> HRESULT {
                if (!alive->load() || !args) return S_OK;
                ComPtr<ICoreWebView2WebResourceRequest> request;
                if (FAILED(args->get_Request(&request)) || !request) return S_OK;
                const std::string url = uri_of(request.Get());
                if (!browser::looks_like_media_url(url)) return S_OK;
                std::string referer = header_of(request.Get(), L"Referer");
                const std::string user_agent = header_of(request.Get(), L"User-Agent");
                if (referer.empty()) referer = page_url_;
                record_candidate(url, referer, user_agent);
                return S_OK;
            }).Get(), &impl_->tok_web_resource);
}

void BrowserView::on_navigation_started(){
    // A new document: the previous page's media is no longer what the address bar means.
    clear_candidate();
}

void BrowserView::on_source_maybe_changed(){
    if (!impl_->webview) return;
    LPWSTR raw = nullptr;
    if (FAILED(impl_->webview->get_Source(&raw)) || !raw) return;
    std::string url = browser::to_utf8(std::wstring(raw));
    CoTaskMemFree(raw);
    if (url.empty() || url == page_url_) return;
    page_url_ = std::move(url);
    clear_candidate(); // SPA/history navigation can skip NavigationStarting
}

void BrowserView::navigate(const std::string& url){
    if (!controller_ready_ || !impl_->webview || url.empty()) return;
    clear_candidate();
    impl_->webview->Navigate(browser::to_wide(url).c_str());
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
}

void BrowserView::record_candidate(const std::string& url, const std::string& referer,
                                   const std::string& user_agent){
    if (candidate_.url == url && candidate_.referer == referer) return;
    candidate_.url = url;
    candidate_.referer = referer;
    candidate_.user_agent = user_agent;
    // Any media switch invalidates the whole cookie handoff -- both the lookup still in flight
    // AND an answer nobody consumed yet: both belong to the previous URL.
    cancel_cookies();
}

void BrowserView::clear_candidate(){
    candidate_ = BrowserMediaCandidate{};
    cancel_cookies();
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
    ComPtr<ICoreWebView2_2> webview2;
    if (FAILED(impl_->webview.As(&webview2)) || !webview2) {
        publish_cookies(url, std::string());
        return;
    }
    ComPtr<ICoreWebView2CookieManager> manager;
    if (FAILED(webview2->get_CookieManager(&manager)) || !manager) {
        publish_cookies(url, std::string());
        return;
    }

    const std::shared_ptr<std::atomic<bool>> alive = alive_;
    HRESULT hr = manager->GetCookies(browser::to_wide(url).c_str(),
        MakeHandler<ICoreWebView2GetCookiesCompletedHandler>(
            [this, alive, url, gen](HRESULT result, ICoreWebView2CookieList* list) -> HRESULT {
                if (!alive->load()) return S_OK;
                // Stale answer (page left / navigated / candidate switched / a newer request
                // was issued since): the generation no longer matches, so drop it silently
                // instead of publishing it as the current page's cookies.
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

void BrowserView::shutdown(){
    if (!alive_) return;
    alive_->store(false);
    cancel_cookies(); // no late answer may be published after teardown
    if (impl_) {
        if (impl_->webview) {
            impl_->webview->remove_SourceChanged(impl_->tok_source);
            impl_->webview->remove_NavigationStarting(impl_->tok_nav_starting);
            impl_->webview->remove_NavigationCompleted(impl_->tok_nav_completed);
            impl_->webview->remove_WebResourceRequested(impl_->tok_web_resource);
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
