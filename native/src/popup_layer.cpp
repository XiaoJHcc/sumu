// SPDX-FileCopyrightText: sumu Authors
// SPDX-License-Identifier: AGPL-3.0
//
// Popup layer implementation -- see popup_layer.h for the design (owned top-level window above
// the WebView2 child, shared ImGui context, draw lists routed out of the main frame, rendered by
// the present thread with a zero sync interval).

#include "popup_layer.h"

#include <dwmapi.h>
#include <windowsx.h> // GET_X_LPARAM / GET_WHEEL_DELTA_WPARAM

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

#include "imgui_impl_dx11.h"
#include "imgui_internal.h" // FindWindowByName / ImGuiWindow geometry / ImDrawList::_OwnerName
#include "ui/theme.h"       // backbuffer clear color (the popup never draws its own paper)

// Win11 DWM corner preference (the SDK may predate this value; matches dwmapi.h since 10.0.22000
// and the constants already used by player.h for the main window).
#ifndef DWMWA_WINDOW_CORNER_PREFERENCE
#define DWMWA_WINDOW_CORNER_PREFERENCE 33
#endif
#ifndef DWMWCP_ROUNDSMALL
#define DWMWCP_ROUNDSMALL 3
#endif

namespace {

constexpr char kPopupClassName[] = "Sumu_Popup_WndClass";
constexpr char kTooltipPrefix[] = "##Tooltip_";

// True when `dl`'s owner window is `target` itself or a child window rooted in it.
bool draw_list_belongs_to(ImDrawList* dl, ImGuiWindow* target)
{
    if (!dl || !target || !dl->_OwnerName) return false;
    ImGuiWindow* owner = ImGui::FindWindowByName(dl->_OwnerName);
    if (!owner) return false;
    return owner == target || owner->RootWindow == target;
}

// FNV-1a over everything that changes the pixels: vertex positions/UVs/colors (a hover wash keeps
// the vertex count but changes colors), indices, and each command's clip rect/texture/range.
constexpr uint64_t kFnvOffset = 1469598103934665603ull;

uint64_t fnv1a(uint64_t h, const void* data, size_t size)
{
    const unsigned char* p = static_cast<const unsigned char*>(data);
    for (size_t i = 0; i < size; ++i) {
        h ^= p[i];
        h *= 1099511628211ull;
    }
    return h;
}

uint64_t hash_draw_list(const ImDrawList* dl, uint64_t h)
{
    h = fnv1a(h, dl->VtxBuffer.Data, static_cast<size_t>(dl->VtxBuffer.size_in_bytes()));
    h = fnv1a(h, dl->IdxBuffer.Data, static_cast<size_t>(dl->IdxBuffer.size_in_bytes()));
    for (const ImDrawCmd& cmd : dl->CmdBuffer) {
        h = fnv1a(h, &cmd.ClipRect, sizeof(cmd.ClipRect));
        h = fnv1a(h, &cmd.TexRef, sizeof(cmd.TexRef));
        h = fnv1a(h, &cmd.VtxOffset, sizeof(cmd.VtxOffset));
        h = fnv1a(h, &cmd.IdxOffset, sizeof(cmd.IdxOffset));
        h = fnv1a(h, &cmd.ElemCount, sizeof(cmd.ElemCount));
    }
    return h;
}

} // namespace

PopupLayer::PopupLayer() = default;

PopupLayer::~PopupLayer()
{
    // Only reached without a live present thread (Player owns these and releases them right
    // after joining). release() is idempotent, so an explicit close() plus this is fine.
    release();
}

void PopupLayer::attach(HWND owner)
{
    owner_ = owner;
}

bool PopupLayer::ensure_window()
{
    if (hwnd_) return true;
    if (!owner_) return false;

    static const bool class_ok = []() {
        WNDCLASSEXA wc{};
        wc.cbSize = sizeof(wc);
        wc.lpfnWndProc = &PopupLayer::wnd_proc;
        wc.hInstance = GetModuleHandleA(nullptr);
        wc.hCursor = LoadCursorA(nullptr, IDC_ARROW);
        wc.lpszClassName = kPopupClassName;
        if (RegisterClassExA(&wc)) return true;
        return GetLastError() == ERROR_CLASS_ALREADY_EXISTS;
    }();
    if (!class_ok) return false;

    // WS_POPUP with `owner_` as its parent creates an OWNED window: it always stacks above the
    // owner, and therefore above the owner's child windows (the WebView2 page view). The
    // extended styles keep it out of activation, the taskbar and Alt+Tab; it is created hidden
    // and only ever shown through ShowWindow(SW_SHOWNOACTIVATE).
    hwnd_ = CreateWindowExA(WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW, kPopupClassName, "sumu",
        WS_POPUP, 0, 0, 1, 1, owner_, nullptr, GetModuleHandleA(nullptr), nullptr);
    if (!hwnd_) {
        fprintf(stderr, "[sumu] popup window create failed (err=%lu)\n", GetLastError());
        return false;
    }
    SetWindowLongPtrA(hwnd_, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(this));
    // Win11 rounded corners (best effort; older systems reject the attribute).
    int corner = DWMWCP_ROUNDSMALL;
    DwmSetWindowAttribute(hwnd_, DWMWA_WINDOW_CORNER_PREFERENCE, &corner, sizeof(corner));
    return true;
}

void PopupLayer::route(ImDrawData* dd, const char* window_name)
{
    routed_ = false;
    ImGuiWindow* target = nullptr;
    if (dd && window_name && window_name[0]) {
        target = ImGui::FindWindowByName(window_name);
        // A window that exists but produced no geometry this frame was not drawn (hidden, or
        // skipped by ImGui) -- nothing to route.
        if (target && (!target->DrawList || target->DrawList->CmdBuffer.Size == 0)) target = nullptr;
    }
    if (!target) {
        wanted_.store(false, std::memory_order_relaxed);
        return;
    }

    auto snap = std::make_unique<UiDrawSnapshot>();
    snap->content_hash = kFnvOffset;
    for (int i = 0; i < dd->CmdLists.Size;) {
        ImDrawList* dl = dd->CmdLists[i];
        if (!draw_list_belongs_to(dl, target)) {
            ++i;
            continue;
        }
        snap->total_vtx += dl->VtxBuffer.Size;
        snap->total_idx += dl->IdxBuffer.Size;
        snap->content_hash = hash_draw_list(dl, snap->content_hash);
        snap->cmd_lists.push_back(dl->CloneOutput());
        // The main frame no longer owns the routed geometry: keep its totals honest and drop the
        // list so the main swapchain never draws it a second time.
        dd->TotalVtxCount -= dl->VtxBuffer.Size;
        dd->TotalIdxCount -= dl->IdxBuffer.Size;
        dd->CmdLists.erase(dd->CmdLists.Data + i);
    }
    if (snap->cmd_lists.empty()) {
        wanted_.store(false, std::memory_order_relaxed);
        return;
    }

    // Placement rect, snapped to whole pixels: the swapchain is created at exactly this size, so
    // a fractional display_pos/display_size would smear the whole surface by a fraction of a
    // pixel. The draw coordinates inside the lists are absolute main-client coordinates and are
    // unaffected by the snap.
    const int x0 = static_cast<int>(std::floor(target->Pos.x));
    const int y0 = static_cast<int>(std::floor(target->Pos.y));
    const int x1 = static_cast<int>(std::ceil(target->Pos.x + target->Size.x));
    const int y1 = static_cast<int>(std::ceil(target->Pos.y + target->Size.y));
    if (x1 - x0 < 1 || y1 - y0 < 1) {
        wanted_.store(false, std::memory_order_relaxed);
        return;
    }
    snap->display_pos = ImVec2(static_cast<float>(x0), static_cast<float>(y0));
    snap->display_size = ImVec2(static_cast<float>(x1 - x0), static_cast<float>(y1 - y0));
    snap->framebuffer_scale = dd->FramebufferScale;

    client_rect_[0] = x0;
    client_rect_[1] = y0;
    client_rect_[2] = x1 - x0;
    client_rect_[3] = y1 - y0;
    {
        std::lock_guard<std::mutex> lk(mutex_);
        pending_ = std::move(snap);
    }
    routed_ = true;
    wanted_.store(true, std::memory_order_relaxed);
}

void PopupLayer::sync()
{
    if (!routed_) {
        hide();
        return;
    }
    if (!ensure_window()) {
        wanted_.store(false, std::memory_order_relaxed);
        return;
    }
    const int w = client_rect_[2];
    const int h = client_rect_[3];
    if (w < 1 || h < 1) {
        hide();
        return;
    }

    POINT origin{ static_cast<LONG>(client_rect_[0]), static_cast<LONG>(client_rect_[1]) };
    if (!ClientToScreen(owner_, &origin)) {
        hide();
        return;
    }
    if (origin.x != placed_[0] || origin.y != placed_[1] || w != placed_[2] || h != placed_[3]) {
        SetWindowPos(hwnd_, nullptr, origin.x, origin.y, w, h,
            SWP_NOACTIVATE | SWP_NOZORDER | SWP_NOOWNERZORDER);
        placed_[0] = origin.x;
        placed_[1] = origin.y;
        placed_[2] = w;
        placed_[3] = h;
    }

    // Show only after the present thread has rendered a frame at THIS size: a swapchain that has
    // never presented would composite as an empty window for one frame.
    if (!shown_) {
        const uint64_t gen = rendered_gen_.load(std::memory_order_relaxed);
        if (gen != 0 && gen != shown_gen_ &&
            rendered_w_.load(std::memory_order_relaxed) == w &&
            rendered_h_.load(std::memory_order_relaxed) == h) {
            ShowWindow(hwnd_, SW_SHOWNOACTIVATE);
            shown_ = true;
            shown_gen_ = gen;
            visible_.store(true, std::memory_order_relaxed);
        }
    }
}

void PopupLayer::hide()
{
    routed_ = false;
    wanted_.store(false, std::memory_order_relaxed);
    visible_.store(false, std::memory_order_relaxed);
    {
        std::lock_guard<std::mutex> lk(mutex_);
        pending_.reset();
    }
    if (hwnd_ && shown_) {
        ShowWindow(hwnd_, SW_HIDE);
        shown_ = false;
    }
}

void PopupLayer::release()
{
    hide();
    {
        std::lock_guard<std::mutex> lk(mutex_);
        pending_.reset();
        active_.reset(); // freed through ImGui's allocator, so this must precede ui_shutdown()
    }
    release_swapchain();
    factory_.Reset();
    if (hwnd_) {
        SetWindowLongPtrA(hwnd_, GWLP_USERDATA, 0);
        DestroyWindow(hwnd_);
        hwnd_ = nullptr;
    }
    owner_ = nullptr;
    placed_[0] = placed_[1] = placed_[2] = placed_[3] = 0;
    client_rect_[0] = client_rect_[1] = client_rect_[2] = client_rect_[3] = 0;
    shown_gen_ = 0;
}

void PopupLayer::client_rect(int out[4]) const
{
    for (int i = 0; i < 4; ++i) out[i] = routed_ ? client_rect_[i] : 0;
}

void PopupLayer::screen_rect(int out[4]) const
{
    for (int i = 0; i < 4; ++i) out[i] = shown_ ? placed_[i] : 0;
}

// ---- present thread -------------------------------------------------------------------------

void PopupLayer::release_swapchain()
{
    rtv_.Reset();
    swapchain_.Reset();
    sc_w_ = sc_h_ = 0;
    rendered_valid_ = false;
}

bool PopupLayer::ensure_swapchain(ID3D11Device* device, ID3D11DeviceContext* context, int w, int h)
{
    if (!device || !context || !hwnd_) return false;
    if (swapchain_ && sc_w_ == w && sc_h_ == h) return true;

    if (!factory_) {
        Microsoft::WRL::ComPtr<IDXGIDevice> dxgi_device;
        Microsoft::WRL::ComPtr<IDXGIAdapter> adapter;
        if (FAILED(device->QueryInterface(IID_PPV_ARGS(&dxgi_device))) || !dxgi_device) return false;
        if (FAILED(dxgi_device->GetAdapter(&adapter)) || !adapter) return false;
        if (FAILED(adapter->GetParent(IID_PPV_ARGS(&factory_))) || !factory_) return false;
        factory_->MakeWindowAssociation(hwnd_, DXGI_MWA_NO_ALT_ENTER);
    }

    if (swapchain_) {
        // Same device/adapter and the same window: resize in place. All back-buffer references
        // (the RTV) must be gone before ResizeBuffers.
        rtv_.Reset();
        if (SUCCEEDED(swapchain_->ResizeBuffers(0, static_cast<UINT>(w), static_cast<UINT>(h),
                DXGI_FORMAT_UNKNOWN, 0))) {
            Microsoft::WRL::ComPtr<ID3D11Texture2D> backbuffer;
            if (SUCCEEDED(swapchain_->GetBuffer(0, IID_PPV_ARGS(&backbuffer))) && backbuffer &&
                SUCCEEDED(device->CreateRenderTargetView(backbuffer.Get(), nullptr, &rtv_))) {
                sc_w_ = w;
                sc_h_ = h;
                rendered_valid_ = false; // new surface: always render once
                return true;
            }
        }
        release_swapchain(); // fall through: build a fresh one
    }

    DXGI_SWAP_CHAIN_DESC1 scd{};
    scd.Width = static_cast<UINT>(w);
    scd.Height = static_cast<UINT>(h);
    scd.Format = DXGI_FORMAT_R8G8B8A8_UNORM; // same format as the main swapchain (ImGui target)
    scd.SampleDesc.Count = 1;
    scd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    scd.BufferCount = 2;
    scd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    scd.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
    scd.Scaling = DXGI_SCALING_STRETCH;
    HRESULT hr = factory_->CreateSwapChainForHwnd(device, hwnd_, &scd, nullptr, nullptr,
        &swapchain_);
    if (FAILED(hr)) {
        fprintf(stderr, "[sumu] popup swapchain create failed hr=0x%08lx\n",
            static_cast<unsigned long>(hr));
        swapchain_.Reset();
        return false;
    }
    Microsoft::WRL::ComPtr<ID3D11Texture2D> backbuffer;
    if (FAILED(swapchain_->GetBuffer(0, IID_PPV_ARGS(&backbuffer))) || !backbuffer ||
        FAILED(device->CreateRenderTargetView(backbuffer.Get(), nullptr, &rtv_))) {
        release_swapchain();
        return false;
    }
    sc_w_ = w;
    sc_h_ = h;
    rendered_valid_ = false;
    return true;
}

void PopupLayer::render(ID3D11Device* device, ID3D11DeviceContext* context)
{
    {
        std::lock_guard<std::mutex> lk(mutex_);
        if (pending_) active_ = std::move(pending_); // O(1) pointer move only
    }
    if (!active_ || active_->cmd_lists.empty()) return;
    if (!wanted_.load(std::memory_order_relaxed)) return;

    const int w = static_cast<int>(std::lround(active_->display_size.x));
    const int h = static_cast<int>(std::lround(active_->display_size.y));
    if (w < 1 || h < 1) return;

    // An unchanged frame that is already on screen needs neither a redraw nor a present. While
    // the window is hidden the frame is always produced, because the main thread's "may I show
    // it yet?" gate waits for a render at the current size.
    const int rx = static_cast<int>(std::lround(active_->display_pos.x));
    const int ry = static_cast<int>(std::lround(active_->display_pos.y));
    if (visible_.load(std::memory_order_relaxed) && rendered_valid_ &&
        rendered_hash_ == active_->content_hash && rendered_rect_[0] == rx &&
        rendered_rect_[1] == ry && rendered_rect_[2] == w && rendered_rect_[3] == h)
        return;

    if (!ensure_swapchain(device, context, w, h)) return;

    ID3D11RenderTargetView* rtv = rtv_.Get();
    context->OMSetRenderTargets(1, &rtv, nullptr);
    const ImVec4 bg = ui::theme::kPanelBg; // the routed window paints its own paper on top
    const float clear[4] = { bg.x, bg.y, bg.z, 1.0f };
    context->ClearRenderTargetView(rtv, clear);

    ImDrawData dd{};
    dd.Valid = true;
    dd.CmdListsCount = active_->cmd_lists.Size;
    dd.TotalVtxCount = active_->total_vtx;
    dd.TotalIdxCount = active_->total_idx;
    dd.DisplayPos = active_->display_pos;
    dd.DisplaySize = active_->display_size;
    dd.FramebufferScale = active_->framebuffer_scale;
    dd.Textures = &ImGui::GetPlatformIO().Textures; // shared context: font texture list
    dd.CmdLists.swap(active_->cmd_lists);           // O(1) borrow, no per-frame allocation
    ImGui_ImplDX11_RenderDrawData(&dd);
    dd.CmdLists.swap(active_->cmd_lists);           // give ownership back to the snapshot

    // Interval 0: never a second vblank wait in the video present loop (DESIGN I10). The popup
    // has no tearing concern (2D chrome, tiny area).
    swapchain_->Present(0, 0);

    rendered_valid_ = true;
    rendered_hash_ = active_->content_hash;
    rendered_rect_[0] = rx;
    rendered_rect_[1] = ry;
    rendered_rect_[2] = w;
    rendered_rect_[3] = h;
    rendered_w_.store(w, std::memory_order_relaxed);
    rendered_h_.store(h, std::memory_order_relaxed);
    rendered_gen_.fetch_add(1, std::memory_order_relaxed);
}

// ---- input ----------------------------------------------------------------------------------

void PopupLayer::feed_mouse(UINT msg, WPARAM wp, LPARAM lp)
{
    if (!owner_ || !hwnd_) return;
    ImGuiIO& io = ImGui::GetIO();
    if (msg == WM_MOUSEWHEEL) {
        // The wheel message's coordinates are SCREEN coordinates; the position itself is already
        // tracked by the mouse-move stream.
        io.AddMouseWheelEvent(0.0f, static_cast<float>(GET_WHEEL_DELTA_WPARAM(wp)) /
            static_cast<float>(WHEEL_DELTA));
        return;
    }
    POINT pt{ GET_X_LPARAM(lp), GET_Y_LPARAM(lp) }; // popup client coordinates
    if (!ClientToScreen(hwnd_, &pt)) return;
    // The routed ImGui window was built in the Player's CLIENT coordinates -- that is exactly
    // what the shared ImGui context expects, so translate into that space and nothing else.
    if (!ScreenToClient(owner_, &pt)) return;
    io.AddMousePosEvent(static_cast<float>(pt.x), static_cast<float>(pt.y));
    switch (msg) {
    case WM_LBUTTONDOWN:
    case WM_LBUTTONDBLCLK:
        if (GetCapture() != hwnd_) SetCapture(hwnd_); // keep receiving while dragged outside
        io.AddMouseButtonEvent(0, true);
        break;
    case WM_LBUTTONUP:
        io.AddMouseButtonEvent(0, false);
        if (GetCapture() == hwnd_) ReleaseCapture();
        break;
    default:
        break;
    }
}

LRESULT CALLBACK PopupLayer::wnd_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    PopupLayer* self = reinterpret_cast<PopupLayer*>(GetWindowLongPtrA(hwnd, GWLP_USERDATA));
    switch (msg) {
    // Never take activation: keyboard focus, the InputText caret and IME stay in the address
    // field of the Player window, so typing keeps working while the popup is up.
    case WM_MOUSEACTIVATE:
        return MA_NOACTIVATE;
    case WM_ACTIVATE:
    case WM_SETFOCUS:
    case WM_KILLFOCUS:
    case WM_DPICHANGED: // geometry follows the routed window every tick
        return 0;
    case WM_SETCURSOR:
        SetCursor(LoadCursorA(nullptr, IDC_ARROW));
        return TRUE;
    case WM_NCHITTEST:
        return HTCLIENT;
    case WM_ERASEBKGND:
        return 1; // the swapchain covers every pixel; no GDI erase flicker
    case WM_PAINT: {
        PAINTSTRUCT ps;
        BeginPaint(hwnd, &ps);
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_CLOSE:
        return 0; // only the owner decides when this window goes away
    case WM_MOUSEMOVE:
    case WM_LBUTTONDOWN:
    case WM_LBUTTONDBLCLK:
    case WM_LBUTTONUP:
    case WM_MOUSEWHEEL:
        if (self) self->feed_mouse(msg, wp, lp);
        return 0;
    default:
        break;
    }
    return DefWindowProcA(hwnd, msg, wp, lp);
}

// ---- introspection --------------------------------------------------------------------------

const char* PopupLayer::active_tooltip_name()
{
    ImGuiContext* ctx = ImGui::GetCurrentContext();
    if (!ctx) return nullptr;
    // ImGui::SetTooltip() creates "##Tooltip_%02d" windows on demand, so the frame's tooltip (if
    // any) only exists as a name after the build. Search from the top of the display order.
    for (int i = ctx->Windows.Size - 1; i >= 0; --i) {
        ImGuiWindow* w = ctx->Windows[i];
        if (!w || !w->Name || std::strncmp(w->Name, kTooltipPrefix, std::strlen(kTooltipPrefix)) != 0)
            continue;
        if (!w->Active || !w->DrawList || w->DrawList->CmdBuffer.Size == 0) continue;
        return w->Name;
    }
    return nullptr;
}
