// SPDX-FileCopyrightText: sumu Authors
// SPDX-License-Identifier: AGPL-3.0
#include "player.h"
#include "ui/theme.h"

namespace {

// Directory containing this module (sumu_core.pyd in dev, sumu.exe when frozen). Used to
// locate sumu.ico for the window/taskbar icon without baking an ICO resource into the pyd.
std::string module_dir()
{
    char path[MAX_PATH] = {};
    HMODULE hm = nullptr;
    if (!GetModuleHandleExA(
            GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            reinterpret_cast<LPCSTR>(&module_dir), &hm))
        return {};
    if (!GetModuleFileNameA(hm, path, MAX_PATH)) return {};
    std::string s(path);
    size_t pos = s.find_last_of("/\\");
    return (pos == std::string::npos) ? std::string() : s.substr(0, pos);
}

// M3: WndProc's DEFINITION now lives in a reopened instance of this same anonymous namespace,
// placed after class Player's closing brace -- it needs a COMPLETE Player type to route input
// to a specific instance (GWLP_USERDATA -> Player*, see create_window()) and to call its
// record_toggle_play()/record_seek()/ui_ready()/set_quit() methods. This forward declaration is
// all create_window()'s WNDCLASSEXA::lpfnWndProc assignment needs. Unnamed namespaces reopened
// later in the same translation unit are the SAME namespace, so this declaration and that
// definition refer to the same entity.
//
// M-C1: quit used to be a file-level global (bool g_quit), silently shared across every Player
// instance in the process -- two Players, one accidentally quitting the other. It is now a
// per-instance std::atomic<bool> quit_ member (see Player::set_quit()/should_quit()), reached
// here via the routed `self` (GWLP_USERDATA), same as every other per-instance WndProc action.
LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);

} // namespace

void Player::create_window(int width_hint, int height_hint, bool maximized){
    // Process DPI awareness (once per process, before any window is created). Resolved
    // dynamically via GetProcAddress rather than calling SetProcessDpiAwarenessContext
    // directly so this doesn't depend on the exact Windows SDK version targeted by the
    // build. A FALSE return just means something else (e.g. the app manifest) already set
    // it -- not an error, so the result is ignored.
#ifndef DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2
#define DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2 ((HANDLE)-4)
#endif
    static bool once = []() {
        HMODULE user32 = GetModuleHandleW(L"user32.dll");
        if (user32) {
            using SetProcessDpiAwarenessContextFn = BOOL(WINAPI*)(HANDLE);
            auto fn = reinterpret_cast<SetProcessDpiAwarenessContextFn>(
                GetProcAddress(user32, "SetProcessDpiAwarenessContext"));
            if (fn) fn(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
        }
        return true;
    }();
    (void)once;

    HINSTANCE hinst = GetModuleHandle(nullptr);
    WNDCLASSEXA wc{};
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hinst;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.lpszClassName = "Sumu_Player_WndClass";

    // Window/taskbar icon.
    // Frozen: PyInstaller icon= embeds RT_GROUP_ICON as resource id 1 in sumu.exe --
    // LoadIcon(GetModuleHandle(nullptr), MAKEINTRESOURCE(1)) picks it up with no loose file.
    // Dev: the pyd has no PE icon resource; fall back to sumu.ico next to the pyd
    // (native/build.bat POST_BUILD copies assets/generated/sumu.ico there).
    {
        HINSTANCE hmod = GetModuleHandle(nullptr); // frozen: sumu.exe; dev: python.exe
        HICON icon_big = LoadIconA(hmod, MAKEINTRESOURCEA(1));
        HICON icon_sm = (HICON)LoadImageA(hmod, MAKEINTRESOURCEA(1), IMAGE_ICON,
            GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), 0);
        if (!icon_big || !icon_sm) {
            std::string dir = module_dir(); // dir of this pyd
            if (!dir.empty()) {
                std::string ico = dir + "\\sumu.ico";
                if (!icon_big)
                    icon_big = (HICON)LoadImageA(nullptr, ico.c_str(), IMAGE_ICON, 0, 0,
                        LR_LOADFROMFILE | LR_DEFAULTSIZE | LR_SHARED);
                if (!icon_sm)
                    icon_sm = (HICON)LoadImageA(nullptr, ico.c_str(), IMAGE_ICON,
                        GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON),
                        LR_LOADFROMFILE | LR_SHARED);
            }
        }
        if (icon_big) wc.hIcon = icon_big;
        if (icon_sm) wc.hIconSm = icon_sm;
        else if (icon_big) wc.hIconSm = icon_big;
    }




    RegisterClassExA(&wc);


    // width_hint/height_hint are 96-DPI LOGICAL units: the startup window lands on the
    // primary monitor (CW_USEDEFAULT), so scale the hints to physical pixels by the system
    // DPI -- otherwise a 150%/200% display gets a window 1.5x/2x smaller than intended.
    // Cross-monitor moves after creation are already covered by WM_DPICHANGED's
    // system-suggested rect.
    const int startup_dpi = (int)GetDpiForSystem();
    RECT rc{ 0, 0, std::max(1, MulDiv(width_hint, startup_dpi, 96)),
                  std::max(1, MulDiv(height_hint, startup_dpi, 96)) };
    AdjustWindowRect(&rc, WS_OVERLAPPEDWINDOW, FALSE);
    hwnd_ = CreateWindowExA(0, wc.lpszClassName, "sumu",
        WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT,
        rc.right - rc.left, rc.bottom - rc.top, nullptr, nullptr, hinst, nullptr);
    if (!hwnd_) throw std::runtime_error("CreateWindowEx failed");
    // Route WndProc (a free function, required by WNDCLASSEXA::lpfnWndProc) back to this
    // specific Player instance -- see the reopened-namespace WndProc definition after
    // class Player's closing brace.
    SetWindowLongPtrA(hwnd_, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(this));
    DragAcceptFiles(hwnd_, TRUE); // M-C2: enables WM_DROPFILES (see WndProc)
    // Win11 DWM chrome (rounded corners + drop shadow) before first paint -- see apply_dwm_chrome().
    apply_dwm_chrome(/*fullscreen=*/false);
    ShowWindow(hwnd_, maximized ? SW_SHOWMAXIMIZED : SW_SHOW);
    UpdateWindow(hwnd_);
    // Force one WM_NCCALCSIZE/WM_SIZE recalculation now that GWLP_USERDATA/WndProc's
    // borderless logic is live, so the window immediately reflects the borderless client
    // area rather than waiting for the next user-driven resize. Harmlessly a no-op on the
    // resulting on_resize() call: swapchain_ doesn't exist yet at this point in open(), so
    // on_resize()'s `if (!swapchain_) return;` early-out absorbs it.
    SetWindowPos(hwnd_, nullptr, 0, 0, 0, 0,
        SWP_FRAMECHANGED | SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);

    RECT client_rc;
    GetClientRect(hwnd_, &client_rc);
    win_width_ = static_cast<UINT>(std::max<LONG>(1, client_rc.right - client_rc.left));
    win_height_ = static_cast<UINT>(std::max<LONG>(1, client_rc.bottom - client_rc.top));
}

void Player::create_device_and_swapchain(){
    D3D_FEATURE_LEVEL levels[] = { D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0 };
    D3D_FEATURE_LEVEL got{};
    HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0,
        levels, ARRAYSIZE(levels), D3D11_SDK_VERSION, &device_, &got, &context_);
    check_hr(hr, "D3D11CreateDevice");

    // Present thread, decode thread, and the AI push thread (called from Python) all
    // issue calls on this same immediate context concurrently. Kept as defense-in-depth
    // alongside the explicit d3d_mutex_ (see file header) -- not a substitute for it.
    {
        ComPtr<ID3D11Multithread> mt;
        check_hr(device_.As(&mt), "QueryInterface(ID3D11Multithread)");
        mt->SetMultithreadProtected(TRUE);
    }

    ComPtr<IDXGIDevice> dxgi_device;
    device_.As(&dxgi_device);
    dxgi_device->GetAdapter(&adapter_);
    ComPtr<IDXGIFactory2> factory;
    adapter_->GetParent(IID_PPV_ARGS(&factory));

    DXGI_SWAP_CHAIN_DESC1 scd{};
    scd.Width = win_width_;
    scd.Height = win_height_;
    scd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    scd.SampleDesc.Count = 1;
    scd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    scd.BufferCount = 2;
    scd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    scd.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
    scd.Scaling = DXGI_SCALING_STRETCH;

    hr = factory->CreateSwapChainForHwnd(device_.Get(), hwnd_, &scd, nullptr, nullptr, &swapchain_);
    check_hr(hr, "CreateSwapChainForHwnd");
    factory->MakeWindowAssociation(hwnd_, DXGI_MWA_NO_ALT_ENTER);

    ComPtr<ID3D11Texture2D> backbuffer;
    swapchain_->GetBuffer(0, IID_PPV_ARGS(&backbuffer));
    hr = device_->CreateRenderTargetView(backbuffer.Get(), nullptr, &backbuffer_rtv_);
    check_hr(hr, "CreateRenderTargetView");
}

void Player::create_shader_pipeline(){
    ComPtr<ID3DBlob> vs_blob, ps_nv12_blob, ps_ai_blob, err_blob;
    HRESULT hr = D3DCompile(kShaderSrc, strlen(kShaderSrc), "present.hlsl", nullptr, nullptr,
        "VSMain", "vs_5_0", 0, 0, &vs_blob, &err_blob);
    if (FAILED(hr)) throw std::runtime_error("VS compile failed: " +
        std::string(err_blob ? (const char*)err_blob->GetBufferPointer() : "?"));

    hr = D3DCompile(kShaderSrc, strlen(kShaderSrc), "present.hlsl", nullptr, nullptr,
        "PSMain_NV12", "ps_5_0", 0, 0, &ps_nv12_blob, &err_blob);
    if (FAILED(hr)) throw std::runtime_error("PS(NV12) compile failed: " +
        std::string(err_blob ? (const char*)err_blob->GetBufferPointer() : "?"));

    hr = D3DCompile(kShaderSrc, strlen(kShaderSrc), "present.hlsl", nullptr, nullptr,
        "PSMain_AI", "ps_5_0", 0, 0, &ps_ai_blob, &err_blob);
    if (FAILED(hr)) throw std::runtime_error("PS(AI) compile failed: " +
        std::string(err_blob ? (const char*)err_blob->GetBufferPointer() : "?"));

    check_hr(device_->CreateVertexShader(vs_blob->GetBufferPointer(), vs_blob->GetBufferSize(), nullptr, &vs_),
        "CreateVertexShader");
    check_hr(device_->CreatePixelShader(ps_nv12_blob->GetBufferPointer(), ps_nv12_blob->GetBufferSize(), nullptr, &ps_nv12_),
        "CreatePixelShader(NV12)");
    check_hr(device_->CreatePixelShader(ps_ai_blob->GetBufferPointer(), ps_ai_blob->GetBufferSize(), nullptr, &ps_ai_),
        "CreatePixelShader(AI)");

    D3D11_SAMPLER_DESC sd{};
    sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sd.ComparisonFunc = D3D11_COMPARISON_NEVER;
    sd.MaxLOD = D3D11_FLOAT32_MAX;
    check_hr(device_->CreateSamplerState(&sd, &sampler_), "CreateSamplerState");

    struct SliceCB { UINT arraySlice; UINT pad[3]; };
    D3D11_BUFFER_DESC cbd{};
    cbd.ByteWidth = sizeof(SliceCB);
    cbd.Usage = D3D11_USAGE_DYNAMIC;
    cbd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    cbd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    check_hr(device_->CreateBuffer(&cbd, nullptr, &slice_cb_), "CreateBuffer(slice_cb_)");
}

// ---- ImGui overlay (M2: real thread split -- main-thread NewFrame/build/Render/Clone,
// present-thread RenderDrawData-only; see ui_tick()/ui_render_drawdata() below) ---------
//
// init/shutdown still run on the constructor/close() thread (no present thread running
// yet at ui_init(), already joined at ui_shutdown()).
//
// Backend/context-touch audit for the main-thread-NewFrame vs. present-thread-
// RenderDrawData concurrency question (done by reading this vendored ImGui 1.92.8 tree,
// not assumed): ImGui_ImplDX11_NewFrame() (backends/imgui_impl_dx11.cpp) only calls
// ImGui_ImplDX11_CreateDeviceObjects() if `!bd->pVertexShader` -- since that prewarm call
// right below already created it here in ui_init() (present thread not started yet, no
// contention for THIS one-time call), ImGui_ImplDX11_NewFrame() on the main thread in
// ui_tick() is a pure no-op w.r.t. the D3D11 device/context every frame after that.
// ImGui_ImplWin32_NewFrame() only touches Win32/IO state, never D3D11. ImGui::NewFrame()/
// ImGui::Render()/CloneOutput() are pure CPU-side ImGui-internal state (including the font
// atlas's lazy CPU-side (re)build via UpdateTexturesNewFrame -> ImFontAtlasUpdateNewFrame,
// which only sets an ImTextureData's status to WantCreate/WantUpdates -- it does not touch
// ID3D11Device/Context). The ONLY ImGui call in this whole vendored tree that issues D3D11
// device/context calls -- including the actual font-texture upload that WantCreate defers
// to -- is ImGui_ImplDX11_RenderDrawData() (confirmed by reading its body: it maps/unmaps
// the vertex/index buffers, iterates draw_data->Textures calling
// ImGui_ImplDX11_UpdateTexture() for any non-OK status, and issues the Draw calls), and
// that call is made ONLY from ui_render_drawdata() on the present thread, inside
// d3d_mutex_, in both M1 and M2. So there is no cross-thread D3D11 context race between
// the main thread's ui_tick() and the present thread's ui_render_drawdata() -- confirmed
// from source, not assumed from the ImGui docs' general multi-threading caveat.
void Player::ui_init(){
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    // macOS-dark design-system theme (ui/theme.*) -- replaces StyleColorsDark + the old
    // ad-hoc FrameRounding tweak. Runs BEFORE the ui_style_base_ snapshot below so
    // apply_ui_dpi()'s ScaleAllSizes rebuild always restarts from these metrics
    // (FrameRounding kRadiusControl=6px etc. are 96-DPI bases it multiplies).
    ui::theme::apply_theme(ImGui::GetStyle());

    // CJK glyph support -- without this, non-ASCII UI (zh/ja labels, CJK filenames from
    // pick_open_file()) renders as tofu. MUST run before ImGui_ImplDX11_Init()/
    // CreateDeviceObjects(): the font atlas has to be configured before the font texture
    // is built. System fonts only (never bundled). Glyph ranges: merge Simplified Chinese
    // common + Japanese so zh-CN / ja / mixed filenames all cover without knowing the UI
    // language at ui_init() time (language is pushed later via set_ui_strings). Ranges
    // vector must outlive AddFontFromFileTTF until the atlas is built -- kept as a
    // Player member. SizePixels is kFontSizeBase (unscaled @ 96 DPI); FontScaleDpi
    // multiplies it at runtime (see apply_ui_dpi()). Secondary copy uses PushFont(NULL,
    // kFontSizeSm) -- same face, one step smaller (ImGui 1.92 dynamic sizing).
    ImFontGlyphRangesBuilder ranges_builder;
    ranges_builder.AddRanges(io.Fonts->GetGlyphRangesChineseSimplifiedCommon());
    ranges_builder.AddRanges(io.Fonts->GetGlyphRangesJapanese());
    ranges_builder.BuildRanges(&font_glyph_ranges_);
    static const char* kCjkFontCandidates[] = {
        "C:\\Windows\\Fonts\\msyh.ttc",      // Microsoft YaHei (zh-CN systems)
        "C:\\Windows\\Fonts\\YuGothM.ttc",   // Yu Gothic Medium (ja systems)
        "C:\\Windows\\Fonts\\meiryo.ttc",
        "C:\\Windows\\Fonts\\msgothic.ttc",
        "C:\\Windows\\Fonts\\simhei.ttf",
        "C:\\Windows\\Fonts\\simsun.ttc",
        "C:\\Windows\\Fonts\\msyhl.ttc",
    };
    bool cjk_font_loaded = false;
    for (const char* font_path : kCjkFontCandidates) {
        std::ifstream probe(font_path, std::ios::binary);
        if (!probe.good()) continue;
        probe.close();
        if (io.Fonts->AddFontFromFileTTF(font_path, kFontSizeBase, nullptr,
                font_glyph_ranges_.Data)) {
            fprintf(stderr, "[sumu] CJK font loaded: %s\n", font_path);
            cjk_font_loaded = true;
            break;
        }
    }
    if (!cjk_font_loaded) {
        // No AddFont* call succeeded -- ImGui auto-adds its built-in ASCII-only default
        // font the first time the atlas is built (Fonts.empty() path), so this is a safe,
        // non-crashing fallback, just without CJK glyph coverage.
        fprintf(stderr, "[sumu] warning: no CJK font found among candidates -- falling back "
            "to the default ASCII-only ImGui font, CJK text will render as tofu boxes\n");
    }

    ImGui_ImplWin32_Init(hwnd_);
    ImGui_ImplDX11_Init(device_.Get(), context_.Get());
    ImGui_ImplDX11_CreateDeviceObjects();
    create_logo_texture(); // first-screen logo SRV; must follow device objects
    ui::icons::init(device_.Get()); // lucide atlas SRV for IconButton glyphs; same lifetime
    // Capture the post-apply_theme metrics once so every apply_ui_dpi() call can
    // rebuild from a clean base (ScaleAllSizes is lossy -- see its ImGui docs). Must run
    // AFTER apply_theme and BEFORE the first apply_ui_dpi below.
    ui_style_base_ = ImGui::GetStyle();
    apply_ui_dpi(ImGui_ImplWin32_GetDpiScaleForHwnd(hwnd_));
    ui_ready_ = true;
}

// Upload the embedded 256x256 RGBA logo (assets/generated/sumu-logo-256.rgba, baked by
// native/cmake/embed_binary.cmake) into a D3D11 texture + SRV for ImGui::AddImage on the
// open-prompt overlay. Device-layer lifetime: built once in ui_init(), released in
// ui_shutdown(). Best-effort -- a failed create just leaves logo_srv_ null and the
// overlay skips the image (prompt still works).
void Player::create_logo_texture(){
    D3D11_TEXTURE2D_DESC td{};
    td.Width = static_cast<UINT>(kLogoW);
    td.Height = static_cast<UINT>(kLogoH);
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_IMMUTABLE;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    D3D11_SUBRESOURCE_DATA init{};
    init.pSysMem = kLogoRgba;
    init.SysMemPitch = static_cast<UINT>(kLogoW * 4);
    HRESULT hr = device_->CreateTexture2D(&td, &init, &logo_tex_);
    if (FAILED(hr)) {
        fprintf(stderr, "[sumu] logo texture create failed hr=0x%08lx -- first-screen logo disabled\n",
            static_cast<unsigned long>(hr));
        return;
    }
    hr = device_->CreateShaderResourceView(logo_tex_.Get(), nullptr, &logo_srv_);
    if (FAILED(hr)) {
        fprintf(stderr, "[sumu] logo SRV create failed hr=0x%08lx -- first-screen logo disabled\n",
            static_cast<unsigned long>(hr));
        logo_tex_.Reset();
    }
}

void Player::ui_shutdown(){
    if (!ui_ready_) return;
    ui_ready_ = false;
    logo_srv_.Reset();
    logo_tex_.Reset();
    ui::icons::shutdown();
    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();
}

// ---- present thread (M2): swap in the latest published snapshot, render it -------------
//
// Called from draw_and_present(), after the video Draw(3,0) and before Present(1,0), while
// already holding d3d_mutex_. Never calls NewFrame/Render/CloneOutput -- only renders an
// already-published snapshot. Tolerates ui_pending_/ui_active_ both being empty (present
// started before the first ui_tick()) by returning without drawing anything.
void Player::ui_render_drawdata(){
    if (!ui_ready_) return;
    {
        std::lock_guard<std::mutex> lk(ui_mutex_);
        if (ui_pending_) ui_active_ = std::move(ui_pending_); // O(1) pointer move only
    }
    if (!ui_active_ || ui_active_->cmd_lists.empty()) return; // nothing published yet

    ImDrawData dd{};
    dd.Valid = true;
    dd.CmdListsCount = ui_active_->cmd_lists.Size;
    dd.TotalVtxCount = ui_active_->total_vtx;
    dd.TotalIdxCount = ui_active_->total_idx;
    dd.DisplayPos = ui_active_->display_pos;
    dd.DisplaySize = ui_active_->display_size;
    dd.FramebufferScale = ui_active_->framebuffer_scale;
    dd.Textures = &ImGui::GetPlatformIO().Textures; // font-texture WantCreate/WantUpdates
                                                      // list lives on the (single, shared)
                                                      // ImGuiContext, not per-snapshot.
    dd.CmdLists.swap(ui_active_->cmd_lists); // O(1) borrow, no per-frame allocation
    ImGui_ImplDX11_RenderDrawData(&dd);
    dd.CmdLists.swap(ui_active_->cmd_lists); // give ownership back to the snapshot
}

// Win11 DWM window chrome for the borderless (hollow-NC) client: system rounded corners
// (~8px at 96 DPI via DWMWCP_ROUND) and a drop shadow. WS_THICKFRAME alone usually keeps
// the shadow, but WM_NCCALCSIZE hollowing can drop it -- a 1px frame extension re-arms DWM
// composition without turning the whole client into glass. Fullscreen disables both.
void Player::apply_dwm_chrome(bool fullscreen){
    if (!hwnd_) return;

    // DWMWCP_ROUND = Win11 default window rounding; DONOTROUND for edge-to-edge fullscreen.
    // Harmless no-op on Win10 (DwmSetWindowAttribute returns E_INVALIDARG for unknown attrs).
    int corner = fullscreen ? DWMWCP_DONOTROUND : DWMWCP_ROUND;
    DwmSetWindowAttribute(hwnd_, DWMWA_WINDOW_CORNER_PREFERENCE, &corner, sizeof(corner));

    // System-default thin border color (matches accent / dark-mode theme).
    COLORREF border = DWMWA_COLOR_DEFAULT;
    DwmSetWindowAttribute(hwnd_, DWMWA_BORDER_COLOR, &border, sizeof(border));

    if (fullscreen) {
        MARGINS m = {0, 0, 0, 0};
        DwmExtendFrameIntoClientArea(hwnd_, &m);
    } else {
        // 1px on each side is enough to keep the DWM drop shadow while the client remains
        // fully opaque under the flip-model swapchain (AlphaMode = IGNORE).
        MARGINS m = {1, 1, 1, 1};
        DwmExtendFrameIntoClientArea(hwnd_, &m);
    }
}

// Borderless fill-monitor fullscreen toggle -- explicitly NOT exclusive fullscreen (no mode
// change, no Alt+Enter path; WM_SYSKEYDOWN already swallows Alt+Enter, see WndProc).
//
// Maximized <-> fullscreen must not leave WS_MAXIMIZE set while the window fills the monitor:
// WM_NCCALCSIZE insets a maximized client by the frame thickness, which shows up as a visible
// margin/border around the video. Save/restore via WINDOWPLACEMENT so a pre-FS maximize comes
// back correctly, and always clear WS_MAXIMIZE for the FS style.
void Player::toggle_fullscreen(){
    if (!fullscreen_.load(std::memory_order_relaxed)) {
        windowed_placement_ = WINDOWPLACEMENT{};
        windowed_placement_.length = sizeof(windowed_placement_);
        GetWindowPlacement(hwnd_, &windowed_placement_);
        windowed_style_ = GetWindowLongA(hwnd_, GWL_STYLE);

        HMONITOR mon = MonitorFromWindow(hwnd_, MONITOR_DEFAULTTONEAREST);
        MONITORINFO mi{ sizeof(mi) };
        GetMonitorInfo(mon, &mi);

        // Flip the flag BEFORE SetWindowPos: SetWindowPos drives WM_SIZE synchronously, which
        // now calls ui_tick() (see WndProc) -- so the very first UI rebuild at the fullscreen
        // size already sees fullscreen_==true and lays out the auto-hiding bar / full-screen
        // video (fit_viewport()) correctly, with no one-frame transient.
        fullscreen_.store(true, std::memory_order_relaxed);
        apply_dwm_chrome(/*fullscreen=*/true);
        // Strip caption/frame AND WS_MAXIMIZE so IsZoomed is false for the whole FS session
        // (otherwise WM_NCCALCSIZE's maximized inset carves a frame-sized border into the
        // monitor-sized window).
        SetWindowLongA(hwnd_, GWL_STYLE, windowed_style_ & ~(WS_CAPTION | WS_THICKFRAME |
            WS_MINIMIZEBOX | WS_MAXIMIZEBOX | WS_SYSMENU | WS_MAXIMIZE));
        SetWindowPos(hwnd_, HWND_TOP, mi.rcMonitor.left, mi.rcMonitor.top,
            mi.rcMonitor.right - mi.rcMonitor.left, mi.rcMonitor.bottom - mi.rcMonitor.top,
            SWP_FRAMECHANGED | SWP_NOOWNERZORDER);
    } else {
        fullscreen_.store(false, std::memory_order_relaxed); // before placement restore, see above
        apply_dwm_chrome(/*fullscreen=*/false);
        SetWindowLongA(hwnd_, GWL_STYLE, windowed_style_);
        // Restores both normal rect and maximized showCmd without workspace/screen confusion
        // that a raw SetWindowPos(rcNormalPosition) would hit.
        windowed_placement_.length = sizeof(windowed_placement_);
        SetWindowPlacement(hwnd_, &windowed_placement_);
        SetWindowPos(hwnd_, nullptr, 0, 0, 0, 0,
            SWP_FRAMECHANGED | SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOOWNERZORDER);
    }
}


namespace {

// WndProc's full DEFINITION (declared above, before class Player -- see that declaration's
// header comment for why it had to move here: it needs a complete Player type). This reopened
// block is the SAME unnamed namespace as the one earlier in this translation unit.
//
// ImGui_ImplWin32_WndProcHandler is given first crack at every message (confirmed by reading
// backends/imgui_impl_win32.cpp: for every keyboard/mouse message it always returns 0/never
// consumes, except a couple of WM_IME_* paths that return 1 -- so this "return true" idiom is
// standard boilerplate here, not an observed short-circuit of our own switch below).
LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    Player* self = reinterpret_cast<Player*>(GetWindowLongPtrA(hwnd, GWLP_USERDATA));
    if (self && self->ui_ready()) {
        if (ImGui_ImplWin32_WndProcHandler(hwnd, msg, wp, lp))
            return true;
    }

    switch (msg) {
    case WM_DESTROY:
        if (self) self->set_quit();
        PostQuitMessage(0);
        return 0;
    case WM_CLOSE:
        // park_on_close: don't destroy -- hand the decision to Python via close_request
        // (it hides the window and keeps the process/models warm for a fast reopen).
        // Unset (verification scripts): legacy immediate teardown, unchanged.
        if (self && self->park_on_close()) self->record_close_request();
        else DestroyWindow(hwnd);
        return 0;
    case WM_KEYDOWN:
        if (wp == VK_ESCAPE) {
            // ESC quits regardless of ImGui's keyboard-capture state (unchanged M1/M2
            // behavior) -- an escape hatch that must never be swallowed by a focused widget.
            // Under park_on_close it takes the same close_request detour as WM_CLOSE above.
            if (self && self->park_on_close()) {
                self->record_close_request();
                return 0;
            }
            if (self) self->set_quit();
            PostQuitMessage(0);
            return 0;
        }
        if (self && self->ui_ready() && !ImGui::GetIO().WantCaptureKeyboard) {
            if (wp == VK_SPACE) {
                self->record_toggle_play();
            } else if (wp == VK_LEFT || wp == VK_RIGHT) {
                int64_t fc = self->frame_count();
                int64_t step = static_cast<int64_t>(std::llround(5.0 * self->fps()));
                int64_t target = self->current_frame() + (wp == VK_LEFT ? -step : step);
                target = (fc > 0) ? std::clamp<int64_t>(target, 0, fc - 1) : std::max<int64_t>(0, target);
                self->record_seek(target);
            } else if (self->has_audio() && (wp == VK_UP || wp == VK_DOWN)) {
                // Volume/mute (additive): pure native state, writes atomics directly (no
                // scheduler coordination needed -- see set_volume()'s header comment), so this
                // does NOT go through record_seek()/record_toggle_play()'s UiIntents channel.
                float step = (wp == VK_UP) ? 0.05f : -0.05f;
                self->set_volume(self->get_volume() + step); // set_volume() itself clamps to [0,1]
            } else if (self->has_audio() && wp == 'M') {
                // WM_KEYDOWN's wParam is a virtual-key code, not a character: VK codes for
                // letters equal the uppercase ASCII value regardless of Shift/CapsLock state,
                // so this already matches both 'm' and 'M' as typed -- no separate lowercase
                // branch needed.
                self->toggle_mute();
            } else if (wp == 'D') {
                // Phase 6 M-D: de-mosaic on/off, direct atomic flip (same discipline as the
                // volume/mute keys above -- pure present-side view state, no UiIntents needed).
                self->set_ai_enabled(!self->is_ai_enabled());
            }
        }
        return 0;
    case WM_SYSKEYDOWN:
        if (wp == VK_RETURN) return 0; // swallow Alt+Enter, no exclusive fullscreen here
        break;
    // ---- fullscreen caption drag -> windowed -------------------------------------------------------------------
    // While borderless-fullscreen the revealed top bar still reports HTCAPTION (WM_NCHITTEST),
    // so a drag would slide the monitor-sized FS window around. Instead: leave fullscreen
    // FIRST (toggle_fullscreen() restores the saved pre-FS placement synchronously -- flag,
    // style and SetWindowPos all complete inside this handler), then fall through to
    // DefWindowProc so the OS move-loop starts right here on the already-windowed window --
    // the drag continues seamlessly with no second click. If the pre-FS placement was
    // maximized, Windows' native "drag restores a maximized window" takes over from there.
    case WM_NCLBUTTONDOWN:
        if (wp == HTCAPTION && self && self->is_fullscreen()) self->exit_fullscreen();
        break;
    // ---- borderless custom-caption (see Player::point_in_caption_drag()'s header comment) --
    // WS_OVERLAPPEDWINDOW/WS_CAPTION/WS_THICKFRAME stay ON the window (unlike a from-scratch
    // popup-style borderless window) -- this pair just hollows out the non-client area so the
    // client fills the whole window, while keeping DWM's native Aero Snap / maximize animation /
    // shadow / taskbar behavior (same trick as Windows Terminal / Chromium). May fire during
    // CreateWindowEx before GWLP_USERDATA is set (self == nullptr) -- the WM_NCCALCSIZE branch
    // below deliberately doesn't need self for that reason.
    case WM_NCCALCSIZE:
        if (wp == TRUE) {
            // Maximized (windowed only): without this inset, DWM's default zoomed rect overhangs
            // the monitor by one frame thickness on each side, which then overlaps the taskbar
            // and clips content -- pulling it in by the frame+padding metrics compensates.
            // Skip while borderless-fullscreen: the window is already sized to the monitor, and
            // an IsZoomed leftover (or a maximize-while-FS click) would carve a visible margin.
            if (IsZoomed(hwnd) && !(self && self->is_fullscreen())) {
                NCCALCSIZE_PARAMS* p = reinterpret_cast<NCCALCSIZE_PARAMS*>(lp);
                UINT dpi = GetDpiForWindow(hwnd);
                int fx = GetSystemMetricsForDpi(SM_CXFRAME, dpi) + GetSystemMetricsForDpi(SM_CXPADDEDBORDER, dpi);
                int fy = GetSystemMetricsForDpi(SM_CYFRAME, dpi) + GetSystemMetricsForDpi(SM_CXPADDEDBORDER, dpi);
                p->rgrc[0].left += fx; p->rgrc[0].right -= fx;
                p->rgrc[0].top += fy; p->rgrc[0].bottom -= fy;
            }
            return 0; // non-maximized / fullscreen: client fills the entire window == borderless
        }
        break;
    case WM_NCHITTEST: {
        POINT pt = { GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
        ScreenToClient(hwnd, &pt);
        RECT rc; GetClientRect(hwnd, &rc);
        UINT dpi = GetDpiForWindow(hwnd);
        int margin = MulDiv(8, dpi, 96);
        bool can_resize = !IsZoomed(hwnd) && !(self && self->is_fullscreen());
        if (can_resize) {
            bool L = pt.x < margin, R = pt.x >= rc.right - margin;
            bool T = pt.y < margin, B = pt.y >= rc.bottom - margin;
            if (T && L) return HTTOPLEFT;
            if (T && R) return HTTOPRIGHT;
            if (B && L) return HTBOTTOMLEFT;
            if (B && R) return HTBOTTOMRIGHT;
            if (L) return HTLEFT;
            if (R) return HTRIGHT;
            if (T) return HTTOP;
            if (B) return HTBOTTOM;
        }
        if (self && self->point_in_caption_drag(pt.x, pt.y)) return HTCAPTION;
        return HTCLIENT;
    }
    case WM_SIZE:
        if (wp != SIZE_MINIMIZED) {
            UINT w = LOWORD(lp);
            UINT h = HIWORD(lp);
            if (self && w != 0 && h != 0) {
                self->on_resize(w, h);
                // Rebuild the ImGui overlay (title bar + progress bar) NOW at the new size so
                // they track the window edge in real time during a drag-resize. Without this
                // they only refreshed once the modal resize loop exited and Python's ui_tick()
                // ran again -- the video (rendered by the independent present thread) followed
                // the resize live, but the self-drawn bars lagged until mouse-up. WM_SIZE is
                // dispatched on the main thread (both from pump_messages() and from inside the
                // OS modal resize loop), which is the only thread allowed to call ui_tick(), so
                // this is thread-safe; the present thread picks up the fresh snapshot next
                // vblank. ui_tick() self-guards on ui_ready_ (no-op before ui_init()).
                self->ui_tick();
            }
        }
        return 0;
    case WM_GETMINMAXINFO: {
        // Task: manual resize must not be artificially limited. Lower the minimum track size
        // (below Windows' default for a WS_THICKFRAME window) so the user can shrink freely; the
        // floor here is just enough to keep the title-bar buttons usable. Deliberately does NOT
        // touch ptMaxSize/ptMaxTrackSize/ptMaxPosition -- those govern the maximized rect, which
        // the borderless WM_NCCALCSIZE path relies on at its default values.
        MINMAXINFO* mmi = reinterpret_cast<MINMAXINFO*>(lp);
        UINT dpi = GetDpiForWindow(hwnd);
        mmi->ptMinTrackSize.x = MulDiv(240, dpi, 96);
        mmi->ptMinTrackSize.y = MulDiv(135, dpi, 96);
        return 0;
    }
    case WM_DPICHANGED: {
        // HIWORD(wp) = new DPI (Y); recommended new window rect is in *lp (system already
        // computed it for Per-Monitor-V2). Re-apply ImGui font/style + our chrome scale, then
        // accept the suggested rect so the client area tracks the new DPI.
        if (self) {
            float scale = static_cast<float>(HIWORD(wp)) / 96.0f;
            self->apply_ui_dpi(scale);
        }
        RECT* rc = reinterpret_cast<RECT*>(lp);
        if (rc) {
            SetWindowPos(hwnd, nullptr, rc->left, rc->top,
                rc->right - rc->left, rc->bottom - rc->top,
                SWP_NOZORDER | SWP_NOACTIVATE);
        }
        return 0;
    }
    case WM_DROPFILES: {
        // M-C2: file(s) dropped on the window. In export mode every dropped file is queued for
        // export (multi-file); otherwise the first file is opened (multi-file open out of scope).
        HDROP hdrop = reinterpret_cast<HDROP>(wp);
        if (self) {
            int nfiles = static_cast<int>(DragQueryFileW(hdrop, 0xFFFFFFFF, nullptr, 0));
            for (int i = 0; i < nfiles; ++i) {
                wchar_t wide_path[MAX_PATH] = {};
                if (DragQueryFileW(hdrop, i, wide_path, ARRAYSIZE(wide_path)) <= 0) continue;
                // Same WideCharToMultiByte(CP_UTF8) conversion as pick_open_file() -- guards
                // against a CJK path silently mis-encoding through the process's ANSI codepage.
                int utf8_len = WideCharToMultiByte(CP_UTF8, 0, wide_path, -1, nullptr, 0, nullptr, nullptr);
                if (utf8_len <= 0) continue;
                std::string utf8_path(static_cast<size_t>(utf8_len - 1), '\0');
                WideCharToMultiByte(CP_UTF8, 0, wide_path, -1, utf8_path.data(), utf8_len, nullptr, nullptr);
                if (self->export_mode()) self->record_export_drop(utf8_path);
                else if (i == 0) self->record_open_path(utf8_path);
            }
        }
        DragFinish(hdrop);
        return 0;
    }
    default:
        break;
    }
    return DefWindowProc(hwnd, msg, wp, lp);
}

} // namespace
