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
    // ICoreWebView2_2 carries the CookieManager (the manual native-play button's replay jar) and
    // WebResourceResponseReceived (the media handover's one look at page traffic).
    ComPtr<ICoreWebView2_2> webview2;
    // ICoreWebView2_4 (DownloadStarting) catches a navigation the browser downloads instead of
    // displaying it; null on an older Evergreen runtime means such a navigation just downloads.
    ComPtr<ICoreWebView2_4> webview4;
    // ICoreWebView2_8 (IsMuted) is queried once; older Evergreen runtimes leave it null and the
    // page simply cannot be muted (everything else keeps working).
    ComPtr<ICoreWebView2_8> webview8;
    EventRegistrationToken tok_source{};
    EventRegistrationToken tok_nav_starting{};
    EventRegistrationToken tok_nav_completed{};
    EventRegistrationToken tok_title{};
    EventRegistrationToken tok_response{};
    EventRegistrationToken tok_download{};
    EventRegistrationToken tok_new_window{};
    EventRegistrationToken tok_web_request{};
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

// ---- media handover: is this navigation's document the video itself? -----------------------
//
// The page view is the transport, so a navigation can land on media instead of a page. These
// helpers answer that from the response alone -- a plain page whose URL happens to end in .mp4
// must never be taken over -- and read the replay context out of the document request.
namespace {

std::string ascii_lower(std::string s){
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

// A navigation a web page is allowed to cause in this view (its new-window requests included).
bool is_http_url(const std::string& url){
    const std::string head = ascii_lower(url.substr(0, 8));
    return head.compare(0, 7, "http://") == 0 || head.compare(0, 8, "https://") == 0;
}

// "scheme://authority", lowercased; empty when `url` has none.
std::string url_origin(const std::string& url){
    const size_t scheme = url.find("://");
    if (scheme == std::string::npos) return std::string();
    const size_t end = url.find_first_of("/?#", scheme + 3);
    return ascii_lower(url.substr(0, end == std::string::npos ? url.size() : end));
}

// The Referer a clicked link to `target` would carry from `opener`: the opener's own URL when both
// share an origin, its bare origin otherwise (the browser's default policy) -- another site never
// learns the opener's path or query.
std::string referrer_from(const std::string& opener, const std::string& target){
    if (!is_http_url(opener) || !is_http_url(target)) return std::string();
    const std::string origin = url_origin(opener);
    if (origin.empty()) return std::string();
    const std::string target_origin = url_origin(target);
    if (origin == target_origin) return opener.substr(0, opener.find('#'));
    // Cross-site: the bare origin only, and nothing at all on an https -> http downgrade
    // (strict-origin-when-cross-origin, the browser's default).
    if (origin.compare(0, 6, "https:") == 0 && target_origin.compare(0, 6, "https:") != 0)
        return std::string();
    return origin;
}

// `url` made absolute against the page it came from (a window request may carry a path only).
std::string absolute_url(const std::string& base, const std::string& url){
    if (url.empty() || is_http_url(url)) return url;
    // Another scheme (about:, blob:, mailto:): not ours to resolve -- the caller rejects it.
    const size_t colon = url.find(':');
    if (colon != std::string::npos && url.find('/') > colon) return url;
    const std::string origin = url_origin(base);
    if (origin.empty()) return std::string();
    if (url.compare(0, 2, "//") == 0) return origin.substr(0, origin.find("://") + 1) + url;
    if (url[0] == '/') return origin + url;
    const size_t end = base.find_first_of("?#");
    const std::string path = base.substr(0, end == std::string::npos ? base.size() : end);
    return path.substr(0, path.rfind('/') + 1) + url;
}

// A COM out-string (LPWSTR + CoTaskMemFree) as UTF-8; empty on failure.
template <typename Getter>
std::string take_com_string(Getter get){
    LPWSTR raw = nullptr;
    if (FAILED(get(&raw)) || !raw) return std::string();
    std::string out = browser::to_utf8(std::wstring(raw));
    CoTaskMemFree(raw);
    return out;
}

std::string request_header(ICoreWebView2HttpRequestHeaders* headers, const wchar_t* name){
    if (!headers) return std::string();
    return take_com_string([&](LPWSTR* out) { return headers->GetHeader(name, out); });
}

std::string response_header(ICoreWebView2HttpResponseHeaders* headers, const wchar_t* name){
    if (!headers) return std::string();
    return take_com_string([&](LPWSTR* out) { return headers->GetHeader(name, out); });
}

// Content-Type without its parameters, lowercased ("video/mp4; codecs=..." -> "video/mp4").
std::string mime_type(const std::string& raw){
    const std::string value = raw.substr(0, raw.find(';'));
    const size_t first = value.find_first_not_of(" \t");
    if (first == std::string::npos) return std::string();
    return ascii_lower(value.substr(first, value.find_last_not_of(" \t") - first + 1));
}

// Media by its own type: anything video/* plus the playlist/manifest types FFmpeg demuxes
// (HLS, DASH, MSS). A page type (text/html, application/json, ...) never qualifies.
bool is_media_mime(const std::string& mime){
    if (mime.size() >= 6 && mime.compare(0, 6, "video/") == 0) return true;
    static const char* const kPlaylists[] = {
        "application/vnd.apple.mpegurl", "application/x-mpegurl", "application/mpegurl",
        "audio/mpegurl", "application/dash+xml", "application/vnd.ms-sstr+xml",
    };
    for (const char* playlist : kPlaylists)
        if (mime == playlist) return true;
    return false;
}

// Types a server hands out when it does not know (or does not care about) the file: these -- and
// only these -- may fall back to the URL's own extension.
bool is_generic_mime(const std::string& mime){
    return mime.empty() || mime == "application/octet-stream" ||
        mime == "binary/octet-stream" || mime == "application/binary" ||
        mime == "application/unknown" || mime == "unknown/unknown" ||
        mime == "application/x-unknown";
}

// URL reduced to what both sides of a navigation agree on: the fragment is dropped (it never
// reaches the server, but a link may well carry one -- ".../clip.mp4#t=30") and percent-escape
// hex digits are upper-cased, so two spellings of the same address compare equal.
std::string url_identity(const std::string& url){
    std::string s = url.substr(0, url.find('#'));
    for (size_t i = 0; i + 2 < s.size(); ++i) {
        if (s[i] != '%') continue;
        s[i + 1] = static_cast<char>(std::toupper(static_cast<unsigned char>(s[i + 1])));
        s[i + 2] = static_cast<char>(std::toupper(static_cast<unsigned char>(s[i + 2])));
        i += 2;
    }
    return s;
}

// True when `a` and `b` name the same resource (see url_identity).
bool same_url(const std::string& a, const std::string& b){
    return !a.empty() && !b.empty() && url_identity(a) == url_identity(b);
}

// Path of `url` (no scheme/authority, no query, no fragment), lowercased.
std::string url_path_lower(const std::string& url){
    size_t start = 0;
    const size_t scheme = url.find("://");
    if (scheme != std::string::npos) {
        const size_t authority_end = url.find('/', scheme + 3);
        start = authority_end == std::string::npos ? url.size() : authority_end;
    }
    size_t end = url.find_first_of("?#", start);
    if (end == std::string::npos) end = url.size();
    return ascii_lower(url.substr(start, end - start));
}

// Unambiguous video/stream extension in the URL's path (query string and fragment ignored).
bool has_video_extension(const std::string& url){
    const std::string path = url_path_lower(url);
    const size_t dot = path.rfind('.');
    if (dot == std::string::npos) return false;
    const size_t slash = path.rfind('/');
    if (slash != std::string::npos && dot < slash) return false;
    const std::string ext = path.substr(dot);
    static const char* const kExts[] = {
        ".mp4", ".m4v", ".mkv", ".webm", ".mov", ".flv", ".avi", ".ts", ".m2ts", ".mts",
        ".mpg", ".mpeg", ".wmv", ".3gp", ".ogv", ".f4v", ".m3u8", ".mpd",
    };
    for (const char* candidate : kExts)
        if (ext == candidate) return true;
    return false;
}

} // namespace

// Page-view settings: no context menus / devtools / status bar / web-message bridge. The
// page keeps its own error page for failed navigations (WebView2 default) -- the owner adds
// no chrome of its own beyond the address bar.
void BrowserView::configure_webview(){
    if (FAILED(impl_->webview.As(&impl_->webview8))) impl_->webview8.Reset();
    if (FAILED(impl_->webview.As(&impl_->webview2))) impl_->webview2.Reset();
    if (FAILED(impl_->webview.As(&impl_->webview4))) impl_->webview4.Reset();
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
            [this, alive](ICoreWebView2*, ICoreWebView2NavigationStartingEventArgs* args) -> HRESULT {
                if (!alive->load() || !args) return S_OK;
                const std::string url =
                    take_com_string([&](LPWSTR* out) { return args->get_Uri(out); });
                UINT64 nav_id = 0;
                args->get_NavigationId(&nav_id);
                BOOL redirected = FALSE;
                args->get_IsRedirected(&redirected);
                on_navigation_started(url, nav_id, redirected != FALSE);
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
    // Document title: the address bar's favorites list uses it as the entry's label (the URL
    // is the fallback). Recorded here, never acted upon.
    impl_->webview->add_DocumentTitleChanged(
        MakeHandler<ICoreWebView2DocumentTitleChangedEventHandler>(
            [this, alive](ICoreWebView2* sender, IUnknown*) -> HRESULT {
                if (!alive->load() || !sender) return S_OK;
                page_title_ = take_com_string([&](LPWSTR* out) { return sender->get_DocumentTitle(out); });
                return S_OK;
            }).Get(), &impl_->tok_title);

    // Media handover (see browser_view.h). Both handlers only RECORD a candidate: the owner decides
    // and stops/opens from its own poll, so nothing here re-enters WebView2 or blocks it.
    if (impl_->webview2) {
        impl_->webview2->add_WebResourceResponseReceived(
            MakeHandler<ICoreWebView2WebResourceResponseReceivedEventHandler>(
                [this, alive](ICoreWebView2*,
                              ICoreWebView2WebResourceResponseReceivedEventArgs* args) -> HRESULT {
                    if (!alive->load() || !args) return S_OK;
                    // Cheap gate first: no navigation is asking for a handover (or one was already
                    // made), so nothing here is worth a single string allocation.
                    if (!media_handover_possible()) return S_OK;
                    ComPtr<ICoreWebView2WebResourceRequest> request;
                    ComPtr<ICoreWebView2WebResourceResponseView> response;
                    if (FAILED(args->get_Request(&request)) || !request) return S_OK;
                    const std::string uri =
                        take_com_string([&](LPWSTR* out) { return request->get_Uri(out); });
                    // Any other URI is a subresource of the page (<video>, fetch, iframe, an ad).
                    if (!same_url(uri, nav_url_)) return S_OK;
                    if (FAILED(args->get_Response(&response)) || !response) return S_OK;
                    int status = 0;
                    if (FAILED(response->get_StatusCode(&status))) return S_OK;
                    const std::string method =
                        take_com_string([&](LPWSTR* out) { return request->get_Method(out); });
                    std::string content_type;
                    ComPtr<ICoreWebView2HttpResponseHeaders> resp_headers;
                    if (SUCCEEDED(response->get_Headers(&resp_headers)) && resp_headers)
                        content_type = response_header(resp_headers.Get(), L"Content-Type");
                    std::string referer, user_agent;
                    ComPtr<ICoreWebView2HttpRequestHeaders> req_headers;
                    if (SUCCEEDED(request->get_Headers(&req_headers)) && req_headers) {
                        referer = request_header(req_headers.Get(), L"Referer");
                        user_agent = request_header(req_headers.Get(), L"User-Agent");
                    }
                    on_document_response(uri, method, status, content_type, referer, user_agent);
                    return S_OK;
                }).Get(), &impl_->tok_response);
    }
    if (impl_->webview4) {
        impl_->webview4->add_DownloadStarting(
            MakeHandler<ICoreWebView2DownloadStartingEventHandler>(
                [this, alive](ICoreWebView2*, ICoreWebView2DownloadStartingEventArgs* args) -> HRESULT {
                    if (!alive->load() || !args) return S_OK;
                    ComPtr<ICoreWebView2DownloadOperation> operation;
                    if (FAILED(args->get_DownloadOperation(&operation)) || !operation) return S_OK;
                    const std::string uri =
                        take_com_string([&](LPWSTR* out) { return operation->get_Uri(out); });
                    const std::string content_type =
                        take_com_string([&](LPWSTR* out) { return operation->get_MimeType(out); });
                    if (!on_download_started(uri, content_type)) return S_OK;
                    // Cancelled while the download is still only announced: no file is written and no
                    // save prompt appears. Both properties must be set before this returns.
                    args->put_Cancel(TRUE);
                    args->put_Handled(TRUE);
                    return S_OK;
                }).Get(), &impl_->tok_download);
    }

    // New-window requests (target=_blank / window.open) become navigations of THIS view: the
    // runtime must never open a window of its own, and the navigation itself is left to the owner's
    // poll. Plus the one-shot filter that puts a click's Referer back on such a navigation.
    impl_->webview->add_NewWindowRequested(
        MakeHandler<ICoreWebView2NewWindowRequestedEventHandler>(
            [this, alive](ICoreWebView2*, ICoreWebView2NewWindowRequestedEventArgs* args) -> HRESULT {
                if (!alive->load() || !args) return S_OK;
                args->put_Handled(TRUE);
                const std::string url = absolute_url(page_url_,
                    take_com_string([&](LPWSTR* out) { return args->get_Uri(out); }));
                if (!is_http_url(url)) return S_OK;
                popup_pending_ = true;
                popup_url_ = url;
                return S_OK;
            }).Get(), &impl_->tok_new_window);
    impl_->webview->add_WebResourceRequested(
        MakeHandler<ICoreWebView2WebResourceRequestedEventHandler>(
            [this, alive](ICoreWebView2*, ICoreWebView2WebResourceRequestedEventArgs* args) -> HRESULT {
                if (!alive->load() || !args || !web_filter_active_) return S_OK;
                ComPtr<ICoreWebView2WebResourceRequest> request;
                if (FAILED(args->get_Request(&request)) || !request) return S_OK;
                const std::string uri =
                    take_com_string([&](LPWSTR* out) { return request->get_Uri(out); });
                if (!same_url(uri, web_filter_uri_)) return S_OK;
                ComPtr<ICoreWebView2HttpRequestHeaders> headers;
                if (SUCCEEDED(request->get_Headers(&headers)) && headers)
                    headers->SetHeader(L"Referer", browser::to_wide(web_filter_referer_).c_str());
                // One shot. The filter itself stays registered (removing it here would call back
                // into the control from inside a callback); the next popup or shutdown drops it.
                web_filter_active_ = false;
                return S_OK;
            }).Get(), &impl_->tok_web_request);
}

void BrowserView::on_navigation_started(const std::string& url, uint64_t nav_id, bool redirected){
    // A new document: a snapshot taken for the old page must not back the settings overlay any
    // more.
    ++snapshot_gen_;
    snapshot_pending_ = false;
    snapshot_state_ = Snapshot::None;
    snapshot_png_.clear();

    // Top-level navigation bookkeeping for the media handover: a redirect fires again with the
    // same id (it only moves `nav_url_` forward), so anything else is a different navigation --
    // the candidate nobody took belongs to the one the user left.
    const bool same_navigation = nav_id_valid_ && (redirected || nav_id == nav_id_);
    if (!same_navigation) {
        nav_id_ = nav_id;
        nav_id_valid_ = true;
        ++nav_serial_;
        cancel_media_navigation();
        nav_handed_id_ = 0;
        nav_handed_valid_ = false;
        nav_doc_seen_ = false;
        // A window request made by the page being left must not jump the view later.
        popup_pending_ = false;
        popup_url_.clear();
        nav_page_url_before_ = page_url_;
    } else {
        nav_id_ = nav_id;
        // A redirect hop: the document to judge is the one its target returns.
        nav_doc_seen_ = false;
    }
    nav_url_ = url;
}

bool BrowserView::media_handover_possible() const {
    // One handover per navigation, and never while one waits for the owner: the media element
    // re-requests the same URL for every range it plays.
    if (!nav_id_valid_ || nav_url_.empty()) return false;
    if (nav_handed_valid_ && nav_handed_id_ == nav_id_) return false;
    return !media_done_;
}

void BrowserView::on_document_response(const std::string& uri, const std::string& method,
                                       int status, const std::string& content_type,
                                       const std::string& referer,
                                       const std::string& user_agent){
    // Only this navigation's own document (`nav_url_`, redirects included): subresources are a
    // different URI and never looked at.
    if (!same_url(uri, nav_url_)) return;
    // Judged once: a download of the same navigation must not re-litigate a rejected response.
    if (nav_doc_seen_) return;
    nav_doc_seen_ = true;
    if (!media_handover_possible()) return;
    // Only a GET can be replayed honestly (a POST body is not available), and a redirect hop or an
    // error page is not the media itself.
    if (ascii_lower(method) != "get") return;
    if (status != 200 && status != 206) return;
    // The type decides: media types by themselves, a generic/absent type only with a video path.
    const std::string mime = mime_type(content_type);
    if (!is_media_mime(mime) && !(is_generic_mime(mime) && has_video_extension(nav_url_))) return;
    MediaNavigation nav;
    // The navigation's own URL, not the request's: query and percent-escapes as asked for.
    nav.url = nav_url_;
    nav.referer = !referer.empty() ? referer
        : (same_url(nav_page_url_before_, nav_url_) ? std::string() : nav_page_url_before_);
    nav.user_agent = user_agent;
    nav.nav_serial = nav_serial_;
    publish_media_navigation(std::move(nav));
}

bool BrowserView::on_download_started(const std::string& uri, const std::string& content_type){
    // Only the navigation this page view just started keeps the browser's download behaviour: a
    // right-click "save as" or a link to an archive must not be taken over.
    if (!nav_id_valid_ || !same_url(uri, nav_url_)) return false;
    // Its document response was already judged (accepted -> handed over below, or rejected:
    // 4xx / a page type) -- a rejected navigation keeps its ordinary download.
    if (!media_handover_possible()) {
        if (media_done_) media_result_.download = true; // no load left to stop
        return true;
    }
    if (nav_doc_seen_) return false;
    // No response was reported for this navigation: the download's own type is the only signal.
    const std::string mime = mime_type(content_type);
    if (!is_media_mime(mime) && !(is_generic_mime(mime) && has_video_extension(nav_url_))) return false;
    MediaNavigation nav;
    nav.url = nav_url_;
    nav.referer = same_url(nav_page_url_before_, nav_url_) ? std::string() : nav_page_url_before_;
    nav.nav_serial = nav_serial_;
    nav.download = true;
    publish_media_navigation(std::move(nav));
    return true;
}

void BrowserView::publish_media_navigation(MediaNavigation nav){
    nav_handed_id_ = nav_id_;
    nav_handed_valid_ = true;
    media_done_ = true;
    media_result_ = std::move(nav);
}

bool BrowserView::take_media_navigation(MediaNavigation& out){
    if (!media_done_) return false;
    media_done_ = false;
    out = std::move(media_result_);
    media_result_ = MediaNavigation{};
    return true;
}

void BrowserView::cancel_media_navigation(){
    media_done_ = false;
    media_result_ = MediaNavigation{};
}

void BrowserView::stop_media_navigation(){
    // Stop() re-enters WebView2, so the owner calls this from its poll, never from a callback.
    if (!controller_ready_ || !impl_->webview) return;
    impl_->webview->Stop();
}

bool BrowserView::take_popup_navigation(std::string& url){
    if (!popup_pending_) return false;
    popup_pending_ = false;
    url = std::move(popup_url_);
    popup_url_.clear();
    return true;
}

void BrowserView::navigate_popup(const std::string& url){
    if (!controller_ready_ || !impl_->webview || url.empty()) return;
    // A host Navigate() carries no Referer at all, so the click's is written onto this one
    // document request by a narrow URI + DOCUMENT filter (see configure_webview()).
    clear_web_filter();
    const std::string referer = referrer_from(page_url_, url);
    if (!referer.empty()) {
        web_filter_uri_ = url;
        web_filter_referer_ = referer;
        if (SUCCEEDED(impl_->webview->AddWebResourceRequestedFilter(
                browser::to_wide(url).c_str(), COREWEBVIEW2_WEB_RESOURCE_CONTEXT_DOCUMENT)))
            web_filter_active_ = true;
    }
    impl_->webview->Navigate(browser::to_wide(url).c_str());
}

void BrowserView::clear_web_filter(){
    if (!web_filter_active_ && web_filter_uri_.empty()) return;
    web_filter_active_ = false;
    if (impl_->webview && !web_filter_uri_.empty())
        impl_->webview->RemoveWebResourceRequestedFilter(browser::to_wide(web_filter_uri_).c_str(),
            COREWEBVIEW2_WEB_RESOURCE_CONTEXT_DOCUMENT);
    web_filter_uri_.clear();
    web_filter_referer_.clear();
}

void BrowserView::on_source_maybe_changed(){
    if (!impl_->webview) return;
    LPWSTR raw = nullptr;
    if (FAILED(impl_->webview->get_Source(&raw)) || !raw) return;
    std::string url = browser::to_utf8(std::wstring(raw));
    CoTaskMemFree(raw);
    if (url.empty() || url == page_url_) return;
    page_url_ = std::move(url);
    // A new document is loading: its predecessor's title must not be offered as this page's
    // label. DocumentTitleChanged fills it again as soon as the page sets one.
    page_title_.clear();
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
    cancel_media_navigation(); // nor a media handover nobody took
    popup_pending_ = false;
    popup_url_.clear();
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
            impl_->webview->remove_DocumentTitleChanged(impl_->tok_title);
            impl_->webview->remove_NewWindowRequested(impl_->tok_new_window);
            impl_->webview->remove_WebResourceRequested(impl_->tok_web_request);
            clear_web_filter();
            if (impl_->webview2) impl_->webview2->remove_WebResourceResponseReceived(impl_->tok_response);
            if (impl_->webview4) impl_->webview4->remove_DownloadStarting(impl_->tok_download);
            impl_->webview.Reset();
        }
        impl_->webview8.Reset();
        impl_->webview4.Reset();
        impl_->webview2.Reset();
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
