#include "tauricpp/window.hpp"
#include "tauricpp/bridge.hpp"
#include "tauricpp/virtual_fs.hpp"

#include <WebView2.h>
#include <wrl.h>
#include <shlwapi.h>
#include <shellscalingapi.h>
#include <versionhelpers.h>
#include <ole2.h>
#include <shellapi.h>
#include <shlobj.h>
#include <KnownFolders.h>
#include <cstdint>
#include <queue>
#include <mutex>

#pragma comment(lib, "shlwapi.lib")
#pragma comment(lib, "shcore.lib")   // 用于 GetDpiForMonitor / SetProcessDpiAwarenessContext
#pragma comment(lib, "ole32.lib")

using namespace Microsoft::WRL;

// 自定义窗口消息 - 用于投递JS到UI线程执行
static const UINT WM_TAURICP_EXECUTE_JS = WM_APP + 1;

// ============================================================================
// FileDropTarget - 实现 IDropTarget 接口，拦截文件拖拽
// ============================================================================
class FileDropTarget : public IDropTarget {
public:
    FileDropTarget(HWND hwnd) : hwnd_(hwnd), refCount_(1) {}

    // IUnknown
    ULONG STDMETHODCALLTYPE AddRef() override { return InterlockedIncrement(&refCount_); }
    ULONG STDMETHODCALLTYPE Release() override {
        LONG c = InterlockedDecrement(&refCount_);
        if (c == 0) delete this;
        return c;
    }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override {
        if (riid == IID_IUnknown || riid == IID_IDropTarget) { *ppv = this; AddRef(); return S_OK; }
        *ppv = nullptr; return E_NOINTERFACE;
    }

    // IDropTarget
    HRESULT STDMETHODCALLTYPE DragEnter(IDataObject* pDataObj, DWORD grfKeyState, POINTL pt, DWORD* pdwEffect) override {
        dataObj_ = pDataObj;
        *pdwEffect = DROPEFFECT_COPY;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE DragOver(DWORD grfKeyState, POINTL pt, DWORD* pdwEffect) override {
        *pdwEffect = DROPEFFECT_COPY;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE DragLeave() override {
        dataObj_ = nullptr;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE Drop(IDataObject* pDataObj, DWORD grfKeyState, POINTL pt, DWORD* pdwEffect) override {
        *pdwEffect = DROPEFFECT_NONE;
        // 检查是否包含 CF_HDROP 格式（文件拖拽）
        FORMATETC fmt = { CF_HDROP, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL };
        STGMEDIUM stg;
        if (SUCCEEDED(pDataObj->GetData(&fmt, &stg))) {
            HDROP hDrop = static_cast<HDROP>(GlobalLock(stg.hGlobal));
            if (hDrop) {
                UINT fileCount = DragQueryFileW(hDrop, 0xFFFFFFFF, nullptr, 0);
                if (fileCount > 0) {
                    wchar_t pathW[MAX_PATH] = {};
                    DragQueryFileW(hDrop, 0, pathW, MAX_PATH);
                    // 宽字符转UTF-8
                    int len = WideCharToMultiByte(CP_UTF8, 0, pathW, -1, nullptr, 0, nullptr, nullptr);
                    std::string utf8Path(len, 0);
                    WideCharToMultiByte(CP_UTF8, 0, pathW, -1, &utf8Path[0], len, nullptr, nullptr);
                    if (!utf8Path.empty() && utf8Path.back() == '\0') utf8Path.pop_back();

                    // 获取鼠标坐标（相对于窗口客户区）
                    POINT cursorPos;
                    GetCursorPos(&cursorPos);
                    ScreenToClient(hwnd_, &cursorPos);

                    // 发送事件给前端（包含鼠标X坐标，用于判断左/右）
                    nlohmann::json pathJson;
                    pathJson["path"] = utf8Path;
                    pathJson["x"] = cursorPos.x;
                    pathJson["y"] = cursorPos.y;
                    tauricpp::Bridge::Instance().Emit("file-dropped", pathJson);
                }
                GlobalUnlock(stg.hGlobal);
            }
            ReleaseStgMedium(&stg);
            *pdwEffect = DROPEFFECT_COPY;
        }
        dataObj_ = nullptr;
        return S_OK;
    }

private:
    HWND hwnd_;
    LONG refCount_;
    ComPtr<IDataObject> dataObj_;
};


// ============================================================================
// DPI 辅助：Per-Monitor V2 感知（运行时兜底，即便 manifest 缺失也能正常渲染）
// ============================================================================
static void EnsurePerMonitorDpiAwareV2() {
    // 优先使用 Win10+ 的 PerMonitorV2：系统自动缩放非客户区 + 子窗口 DPI 通知
    typedef BOOL(WINAPI* PSetProcessDpiAwarenessContext)(DPI_AWARENESS_CONTEXT);
    auto setContext = reinterpret_cast<PSetProcessDpiAwarenessContext>(
        ::GetProcAddress(::GetModuleHandleW(L"user32.dll"), "SetProcessDpiAwarenessContext"));
    if (setContext) {
        // DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2 = -4
        if (setContext(reinterpret_cast<DPI_AWARENESS_CONTEXT>(-4))) {
            return;
        }
    }
    // Win8.1 兜底：PROCESS_PER_MONITOR_DPI_AWARE = 2
    typedef HRESULT(WINAPI* PSetProcessDpiAwareness)(PROCESS_DPI_AWARENESS);
    auto setAwareness = reinterpret_cast<PSetProcessDpiAwareness>(
        ::GetProcAddress(::GetModuleHandleW(L"shcore.dll"), "SetProcessDpiAwareness"));
    if (setAwareness) {
        setAwareness(PROCESS_PER_MONITOR_DPI_AWARE);
    }
}

namespace tauricpp {

// ============================================================================
// EventTokens::Impl - 持有WebView2事件注册令牌
// ============================================================================
struct Window::EventTokens::Impl {
    EventRegistrationToken webResourceRequestedToken = {};
    EventRegistrationToken webMessageReceivedToken = {};
    EventRegistrationToken navigationCompletedToken = {};
    EventRegistrationToken sourceChangedToken = {};
    EventRegistrationToken focusChangedToken = {};
    EventRegistrationToken newWindowRequestedToken = {};
};

Window::EventTokens::EventTokens() : impl(std::make_unique<Impl>()) {}
Window::EventTokens::~EventTokens() = default;

// ============================================================================
// 构造/析构
// ============================================================================
Window::Window(const Config& config) : config_(config) {}

Window::~Window() {
    shutting_down_ = true;

    // 清理文件拖拽目标
    if (fileDropTarget_) {
        auto* target = static_cast<FileDropTarget*>(fileDropTarget_);
        if (hwnd_) {
            EnumChildWindows(hwnd_, [](HWND child, LPARAM) -> BOOL {
                RevokeDragDrop(child);
                return TRUE;
            }, 0);
            RevokeDragDrop(hwnd_);
        }
        target->Release();
        fileDropTarget_ = nullptr;
    }

    // 清理JS队列
    {
        std::lock_guard<std::mutex> lock(jsQueueMutex_);
        while (!jsQueue_.empty()) jsQueue_.pop();
    }

    // 注销WebView2事件处理器
    if (webview_) {
        ComPtr<ICoreWebView2_2> webview2;
        if (SUCCEEDED(webview_->QueryInterface(IID_PPV_ARGS(&webview2)))) {
            webview2->remove_WebResourceRequested(event_tokens_.impl->webResourceRequestedToken);
            webview2->remove_WebMessageReceived(event_tokens_.impl->webMessageReceivedToken);
            webview_->remove_NewWindowRequested(event_tokens_.impl->newWindowRequestedToken);
        }
        webview_->remove_NavigationCompleted(event_tokens_.impl->navigationCompletedToken);
    }

    if (controller_) {
        controller_->Release();
        controller_ = nullptr;
    }
    if (webview_) {
        webview_->Release();
        webview_ = nullptr;
    }
    if (env_) {
        env_->Release();
        env_ = nullptr;
    }
}

// ============================================================================
// 辅助函数 - 正确的UTF-8 <-> Wide转换
// ============================================================================
static std::wstring Utf8ToWide(const std::string& str) {
    if (str.empty()) return {};
    int len = MultiByteToWideChar(CP_UTF8, 0, str.c_str(), -1, nullptr, 0);
    std::wstring result(len - 1, 0);
    MultiByteToWideChar(CP_UTF8, 0, str.c_str(), -1, result.data(), len);
    return result;
}

static std::string WideToUtf8(const std::wstring& str) {
    if (str.empty()) return {};
    int len = WideCharToMultiByte(CP_UTF8, 0, str.c_str(), -1, nullptr, 0, nullptr, nullptr);
    std::string result(len - 1, 0);
    WideCharToMultiByte(CP_UTF8, 0, str.c_str(), -1, result.data(), len, nullptr, nullptr);
    return result;
}

// ============================================================================
// WebView2 Runtime 可用性检测
// ============================================================================
static bool IsWebView2RuntimeAvailable(std::wstring* outVersion = nullptr) {
    // 查询系统是否安装了 Microsoft Edge WebView2 Runtime
    // 返回 S_OK 且 versionInfo 非空表示可用
    LPWSTR versionInfo = nullptr;
    HRESULT hr = GetAvailableCoreWebView2BrowserVersionString(nullptr, &versionInfo);
    bool available = SUCCEEDED(hr) && versionInfo != nullptr;
    if (available && outVersion) {
        *outVersion = versionInfo;
    }
    if (versionInfo) {
        CoTaskMemFree(versionInfo);
    }
    return available;
}

// 友好的提示对话框：当机器上缺少 WebView2 Runtime 时显示
static void ShowWebView2MissingMessage(HWND hwnd) {
    std::wstring title = L"缺少 WebView2 运行时";
    std::wstring msg =
        L"无法启动应用：当前系统未检测到 Microsoft Edge WebView2 Runtime。\n\n"
        L"TauriCPP 依赖 WebView2 来渲染界面。请按以下步骤安装后重试：\n\n"
        L"1. 访问官方下载页面：\n"
        L"   https://go.microsoft.com/fwlink/p/?LinkId=2124703\n"
        L"2. 下载并运行 “Evergreen Bootstrapper（常青版引导程序）”。\n"
        L"3. 安装完成后重新启动本程序。\n\n"
        L"提示：Windows 10/11 通常已预装；若被卸载或禁用，则需手动安装。";

    // Windows 7 不支持常青版，需安装最后一个支持的固定版本
    if (IsWindows7OrGreater() && !IsWindows8OrGreater()) {
        msg +=
            L"\n注意：当前为 Windows 7 系统，常青版 Bootstrapper 已不再支持。\n"
            L"请安装最后一个支持 Win7 的固定版本：\n"
            L"  WebView2 Runtime 109.0.1518.140\n"
            L"  下载地址：\n"
            L"  https://developer.microsoft.com/zh-cn/microsoft-edge/webview2/\n"
            L"  (选择 “Fixed Version”，版本号 109.0.1518.140)";
    }

    MessageBoxW(hwnd, msg.c_str(), title.c_str(), MB_OK | MB_ICONWARNING);
}

// ============================================================================
// 创建Win32原生窗口
// ============================================================================
bool Window::CreateNativeWindow() {
    EnsurePerMonitorDpiAwareV2();

    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = WndProc;
    wc.hInstance = GetModuleHandle(nullptr);
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    // 标题栏 / 任务栏图标取自 exe 内嵌图标资源（.rc 中的 ID 1）
    wc.hIcon = static_cast<HICON>(
        LoadImageW(wc.hInstance, MAKEINTRESOURCEW(1), IMAGE_ICON, 0, 0, LR_DEFAULTSIZE | LR_SHARED));
    wc.hIconSm = wc.hIcon;
    // 使用配置的背景色作为窗口类背景，消除白色闪烁
    wc.hbrBackground = CreateSolidBrush(config_.bg_color);
    wc.lpszClassName = L"TauriCPPWindowClass";

    static bool registered = false;
    if (!registered) {
        RegisterClassExW(&wc);
        registered = true;
    }

    DWORD style = WS_OVERLAPPEDWINDOW;
    if (!config_.resizable) {
        style &= ~(WS_THICKFRAME | WS_MAXIMIZEBOX);
    }

    // 根据窗口目标显示位置的 DPI 将"设计尺寸"换算为实际物理像素尺寸
    // 设计尺寸约定：以 96 DPI (100%) 为基准（即 CSS 逻辑像素）
    UINT targetDpi = 96;
    {
        // 创建一个隐藏临时窗口以获得准确的 WMDPICHANGED 前的屏幕DPI；
        // 更简单：直接用主显示器的 DPI，窗口创建后系统会再次发送 WM_DPICHANGED。
        typedef UINT(WINAPI* PGetDpiForSystem)();
        auto getDpiForSystem = reinterpret_cast<PGetDpiForSystem>(
            ::GetProcAddress(::GetModuleHandleW(L"user32.dll"), "GetDpiForSystem"));
        if (getDpiForSystem) {
            targetDpi = getDpiForSystem();
        }
    }
    const int baseDpi = 96;
    int scaledWidth = MulDiv(config_.width, targetDpi, baseDpi);
    int scaledHeight = MulDiv(config_.height, targetDpi, baseDpi);

    // 注意：AdjustWindowRect 非 DPI 感知，使用 DPI 感知版本（Win10 1607+），
    // 若不可用则退回 AdjustWindowRect，DPI 差异由 WM_DPICHANGED 补偿。
    typedef BOOL(WINAPI* PAdjustWindowRectExForDpi)(LPRECT, DWORD, BOOL, DWORD, UINT);
    auto adjustForDpi = reinterpret_cast<PAdjustWindowRectExForDpi>(
        ::GetProcAddress(::GetModuleHandleW(L"user32.dll"), "AdjustWindowRectExForDpi"));
    DWORD exStyle = config_.always_on_top ? WS_EX_TOPMOST : 0;

    auto nonClientSize = [&]() -> SIZE {
        RECT probe = { 0, 0, 0, 0 };
        if (adjustForDpi) {
            adjustForDpi(&probe, style, FALSE, exStyle, targetDpi);
        } else {
            AdjustWindowRectEx(&probe, style, FALSE, exStyle);
        }
        return { probe.right - probe.left, probe.bottom - probe.top };
    };

    // 限制到显示器“工作区”（排除任务栏），避免窗口比屏幕还高导致
    // 标题栏被顶出屏幕、最大化按钮点不到的问题。
    RECT work = { 0, 0, GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN) };
    if (::SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0) &&
        work.right > work.left && work.bottom > work.top) {
        SIZE nc = nonClientSize();
        int maxClientW = (work.right - work.left) - nc.cx - 24;
        int maxClientH = (work.bottom - work.top) - nc.cy - 24;
        if (maxClientW < 320) maxClientW = 320;
        if (maxClientH < 240) maxClientH = 240;
        if (scaledWidth > maxClientW) scaledWidth = maxClientW;
        if (scaledHeight > maxClientH) scaledHeight = maxClientH;
    }

    RECT rect = { 0, 0, scaledWidth, scaledHeight };
    if (adjustForDpi) {
        adjustForDpi(&rect, style, FALSE, exStyle, targetDpi);
    } else {
        AdjustWindowRectEx(&rect, style, FALSE, exStyle);
    }

    int x = CW_USEDEFAULT, y = CW_USEDEFAULT;
    if (config_.center) {
        int areaW = work.right - work.left;
        int areaH = work.bottom - work.top;
        int winW = rect.right - rect.left;
        int winH = rect.bottom - rect.top;
        x = work.left + (areaW - winW) / 2;
        y = work.top + (areaH - winH) / 2;
        if (x < work.left) x = work.left;
        if (y < work.top) y = work.top;
    }

    // 关键：创建时不显示窗口（不传WS_VISIBLE），等WebView2导航完成后再显示
    hwnd_ = CreateWindowExW(
        exStyle,
        wc.lpszClassName,
        Utf8ToWide(config_.title).c_str(),
        style, x, y,
        rect.right - rect.left, rect.bottom - rect.top,
        nullptr, nullptr, wc.hInstance, this
    );

    return hwnd_ != nullptr;
}

// ============================================================================
// 窗口过程
// ============================================================================
LRESULT CALLBACK Window::WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    Window* self = nullptr;

    if (msg == WM_NCCREATE) {
        auto cs = reinterpret_cast<CREATESTRUCT*>(lParam);
        self = reinterpret_cast<Window*>(cs->lpCreateParams);
        SetWindowLongPtr(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    } else {
        self = reinterpret_cast<Window*>(GetWindowLongPtr(hwnd, GWLP_USERDATA));
    }

    switch (msg) {
    case WM_SIZE: {
        if (self && self->controller_) {
            RECT bounds;
            GetClientRect(hwnd, &bounds);
            self->controller_->put_Bounds(bounds);
        }
        if (self && self->on_resize_ && wParam != SIZE_MINIMIZED) {
            RECT clientRect;
            GetClientRect(hwnd, &clientRect);
            self->on_resize_(clientRect.right, clientRect.bottom);
        }
        if (self) {
            if (wParam == SIZE_MINIMIZED && self->on_minimize_) {
                self->on_minimize_();
            }
            if (wParam == SIZE_MAXIMIZED && self->on_maximize_) {
                self->on_maximize_();
            }
        }
        return 0;
    }

    case WM_DPICHANGED: {
        // DPI 变化（拖到不同缩放比例的屏幕）：重新调整窗口尺寸 + WebView2 bounds
        // 确保 WebView2 始终以物理像素渲染，避免位图拉伸导致模糊
        if (self) {
            UINT newDpi = LOWORD(wParam);
            RECT* suggested = reinterpret_cast<RECT*>(lParam);
            if (suggested) {
                SetWindowPos(hwnd, nullptr,
                    suggested->left, suggested->top,
                    suggested->right - suggested->left,
                    suggested->bottom - suggested->top,
                    SWP_NOZORDER | SWP_NOACTIVATE);
            }
            if (self->controller_) {
                RECT bounds;
                GetClientRect(hwnd, &bounds);
                self->controller_->put_Bounds(bounds);
            }
            (void)newDpi;
        }
        return 0;
    }

    case WM_ACTIVATE:
        if (self && self->on_focus_ && LOWORD(wParam) != WA_INACTIVE) {
            self->on_focus_();
        }
        return 0;

    case WM_TAURICP_EXECUTE_JS:
        if (self) {
            self->ExecuteJsFromQueue();
        }
        return 0;

    case WM_CLOSE:
        if (self && self->on_close_) {
            if (!self->on_close_()) {
                // 用户回调拒绝关闭
                return 0;
            }
        }
        break;

    case WM_DESTROY:
        if (self) {
            self->shutting_down_ = true;
        }
        PostQuitMessage(0);
        return 0;

    case WM_KEYDOWN:
        // F12 切换 DevTools
        if (self && wParam == VK_F12 && self->config_.devtools) {
            self->ToggleDevTools();
        }
        return 0;
    }

    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

// ============================================================================
// 设置WebView2默认背景色 - 消除白屏核心方案
// ============================================================================
void Window::SetWebViewBackgroundColor() {
    if (!controller_) return;

    // 使用 ICoreWebView2Controller2 设置默认背景色
    // 这样 WebView2 在页面加载前就显示指定颜色而非白色
    ComPtr<ICoreWebView2Controller2> controller2;
    if (SUCCEEDED(controller_->QueryInterface(IID_PPV_ARGS(&controller2)))) {
        COREWEBVIEW2_COLOR bgColor;
        bgColor.A = 255;
        bgColor.R = GetRValue(config_.bg_color);
        bgColor.G = GetGValue(config_.bg_color);
        bgColor.B = GetBValue(config_.bg_color);
        controller2->put_DefaultBackgroundColor(bgColor);
    }
}

// ============================================================================
// 初始化WebView2
// ============================================================================
bool Window::InitWebView() {
    // 用户数据（localStorage / IndexedDB / 缓存）放在 %LOCALAPPDATA%\TauriCPP 下，
    // 避免 %TEMP% 被系统清理后学习进度丢失
    WCHAR exePath[MAX_PATH];
    GetModuleFileNameW(nullptr, exePath, MAX_PATH);
    WCHAR* exeName = wcsrchr(exePath, L'\\');
    exeName = exeName ? exeName + 1 : exePath;

    std::wstring userDataFolder;
    PWSTR localAppData = nullptr;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, KF_FLAG_CREATE, nullptr, &localAppData)) &&
        localAppData) {
        userDataFolder = std::wstring(localAppData) + L"\\TauriCPP\\" + exeName;
        CoTaskMemFree(localAppData);
    }
    if (userDataFolder.empty()) {
        WCHAR tempDir[MAX_PATH];
        GetTempPathW(MAX_PATH, tempDir);
        userDataFolder = std::wstring(tempDir) + L"tauricpp_" + exeName;
    }
    SHCreateDirectoryExW(nullptr, userDataFolder.c_str(), nullptr);

    HRESULT hr = CreateCoreWebView2EnvironmentWithOptions(
        nullptr, userDataFolder.c_str(), nullptr,
        Callback<ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler>(
            [this](HRESULT result, ICoreWebView2Environment* env) -> HRESULT {
                if (FAILED(result)) return result;

                env_ = env;
                env_->AddRef();

                env->CreateCoreWebView2Controller(hwnd_,
                    Callback<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler>(
                         [this](HRESULT result, ICoreWebView2Controller* controller) -> HRESULT {
                             if (FAILED(result)) return result;

                            controller_ = controller;
                            controller_->AddRef();

                            controller_->get_CoreWebView2(&webview_);
                            if (webview_) {
                                webview_->AddRef();
                            }

                            // 设置WebView填满窗口
                            RECT bounds;
                            GetClientRect(hwnd_, &bounds);
                            controller_->put_Bounds(bounds);

                            // ★ 用自定义 IDropTarget 替换 WebView2 子窗口的拖拽处理
                            OleInitialize(nullptr);
                            {
                                auto* dropTarget = new FileDropTarget(hwnd_);
                                fileDropTarget_ = dropTarget;
                                // 撤销所有子窗口的 OLE 拖拽，并注册我们自己的
                                struct EnumData { IDropTarget* target; };
                                EnumData data{ dropTarget };
                                EnumChildWindows(hwnd_, [](HWND child, LPARAM lParam) -> BOOL {
                                    RevokeDragDrop(child);
                                    auto* d = reinterpret_cast<EnumData*>(lParam);
                                    RegisterDragDrop(child, d->target);
                                    return TRUE;
                                }, reinterpret_cast<LPARAM>(&data));
                            }

                            // ★ 关键：设置WebView2默认背景色，消除白屏闪烁
                            SetWebViewBackgroundColor();

                            // 设置资源拦截（虚拟主机名 -> 内嵌外壳 + 外置媒体）
                            SetupResourceInterception();

                            // 设置通信桥接
                            SetupBridge();
                            // 外部链接（target="_blank" / window.open）交给系统默认浏览器，
                            // 避免弹出没有地址栏的 WebView2 子窗口
                            webview_->add_NewWindowRequested(
                                Callback<ICoreWebView2NewWindowRequestedEventHandler>(
                                    [](ICoreWebView2*, ICoreWebView2NewWindowRequestedEventArgs* args) -> HRESULT {
                                        LPWSTR uriW = nullptr;
                                        args->get_Uri(&uriW);
                                        if (uriW) {
                                            ShellExecuteW(nullptr, L"open", uriW, nullptr, nullptr, SW_SHOWNORMAL);
                                            CoTaskMemFree(uriW);
                                        }
                                        args->put_Handled(TRUE);
                                        return S_OK;
                                    }
                                ).Get(),
                                &event_tokens_.impl->newWindowRequestedToken
                            );

                            // 导航到起始URL
                            webview_->Navigate(Utf8ToWide(config_.start_url).c_str());

                            // ★ 导航完成后刷新WebView布局
                            webview_->add_NavigationCompleted(
                                Callback<ICoreWebView2NavigationCompletedEventHandler>(
                                    [this](ICoreWebView2*, ICoreWebView2NavigationCompletedEventArgs*) -> HRESULT {
                                        // 导航完成后重新设置bounds确保WebView正确布局
                                        if (controller_) {
                                            RECT bounds;
                                            GetClientRect(hwnd_, &bounds);
                                            controller_->put_Bounds(bounds);
                                        }
                                        // ★ 启动焦点：把键盘焦点移入 WebView，
                                        // 否则首键总是被外层 Win32 窗口吃掉（要先用鼠标点一下）
                                        if (controller_) {
                                            controller_->MoveFocus(
                                                COREWEBVIEW2_MOVE_FOCUS_REASON_PROGRAMMATIC);
                                        }
                                        return S_OK;
                                    }
                                ).Get(),
                                &event_tokens_.impl->navigationCompletedToken
                            );

                            webview_ready_ = true;

                            return S_OK;
                        }
                    ).Get()
                );
                return S_OK;
            }
        ).Get()
    );

    // 异步初始化，不等待（WebView2回调在消息循环中分发）
    return SUCCEEDED(hr);
}

namespace {

// 外置数据根目录：配置优先，否则取 exe 所在目录
// 页面外壳（html/js/css）在 exe 资源段里，大体积媒体（插画/音频）从这里读
std::wstring ResolveDataRoot(const std::string& configured) {
    if (!configured.empty()) {
        WCHAR absPath[MAX_PATH];
        if (GetFullPathNameW(Utf8ToWide(configured).c_str(), MAX_PATH, absPath, nullptr)) {
            return absPath;
        }
    }
    WCHAR exePath[MAX_PATH];
    GetModuleFileNameW(nullptr, exePath, MAX_PATH);
    WCHAR* lastSlash = wcsrchr(exePath, L'\\');
    if (lastSlash) *lastSlash = L'\0';
    return exePath;
}

// 把 URL 路径解析为数据目录下的真实文件；拒绝目录穿越与非法字符
bool ResolveDataFile(const std::wstring& root, const std::string& vpath,
                     std::wstring& outPath, ULONGLONG& outSize) {
    if (root.empty() || vpath.size() < 2 || vpath[0] != '/') return false;
    if (vpath.find("..") != std::string::npos) return false;
    if (vpath.find(':') != std::string::npos) return false;

    std::wstring rel = Utf8ToWide(vpath);
    for (auto& ch : rel) {
        if (ch == L'/') ch = L'\\';
    }

    const std::wstring full = root + rel;
    WIN32_FILE_ATTRIBUTE_DATA info{};
    if (!GetFileAttributesExW(full.c_str(), GetFileExInfoStandard, &info)) return false;
    if (info.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_DEVICE)) return false;

    outPath = full;
    outSize = (static_cast<ULONGLONG>(info.nFileSizeHigh) << 32) | info.nFileSizeLow;
    return true;
}

// 读取 [start, end] 区间；单次响应有上限，避免异常请求把内存吃满
bool ReadFileRange(const std::wstring& path, ULONGLONG start, ULONGLONG end, std::vector<uint8_t>& out) {
    if (end < start) return false;
    const ULONGLONG length = end - start + 1;
    if (length > 64ull * 1024 * 1024) return false;

    HANDLE hFile = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ,
                               nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (hFile == INVALID_HANDLE_VALUE) return false;

    LARGE_INTEGER offset{};
    offset.QuadPart = static_cast<LONGLONG>(start);
    if (!SetFilePointerEx(hFile, offset, nullptr, FILE_BEGIN)) {
        CloseHandle(hFile);
        return false;
    }

    out.resize(static_cast<size_t>(length));
    DWORD read = 0;
    const BOOL ok = ReadFile(hFile, out.data(), static_cast<DWORD>(length), &read, nullptr);
    CloseHandle(hFile);
    if (!ok) return false;
    out.resize(read);
    return true;
}

// 仅识别 "bytes=start-end" / "bytes=start-"；无法解析时按整文件返回
void ParseRangeHeader(const std::wstring& header, ULONGLONG size,
                      ULONGLONG& start, ULONGLONG& end, bool& partial) {
    partial = false;
    start = 0;
    end = size > 0 ? size - 1 : 0;
    if (size == 0) return;

    const std::wstring prefix = L"bytes=";
    if (header.compare(0, prefix.size(), prefix) != 0) return;

    const std::wstring spec = header.substr(prefix.size());
    const auto dash = spec.find(L'-');
    if (dash == std::wstring::npos) return;

    const std::wstring first = spec.substr(0, dash);
    const std::wstring last = spec.substr(dash + 1);
    const ULONGLONG s = first.empty() ? 0 : wcstoull(first.c_str(), nullptr, 10);
    if (s >= size) {
        // 起点越界：交给调用方回 416
        start = s;
        end = s;
        partial = true;
        return;
    }

    ULONGLONG e = last.empty() ? size - 1 : wcstoull(last.c_str(), nullptr, 10);
    if (e >= size) e = size - 1;
    if (e < s) return;

    start = s;
    end = e;
    partial = true;
}

// 用内存缓冲构造响应
HRESULT ReplyWithBody(ICoreWebView2Environment* env,
                      ICoreWebView2WebResourceRequestedEventArgs* args,
                      int status, const wchar_t* reason, const std::wstring& headers,
                      const void* data, size_t size) {
    if (!env) return E_POINTER;

    HGLOBAL hGlobal = GlobalAlloc(GMEM_MOVEABLE, size > 0 ? size : 1);
    if (!hGlobal) return E_OUTOFMEMORY;
    void* pData = GlobalLock(hGlobal);
    if (!pData) return E_OUTOFMEMORY;
    if (size > 0 && data) {
        memcpy(pData, data, size);
    } else {
        *static_cast<uint8_t*>(pData) = 0;
    }
    GlobalUnlock(hGlobal);

    ComPtr<IStream> stream;
    if (FAILED(CreateStreamOnHGlobal(hGlobal, TRUE, &stream))) return E_FAIL;

    ComPtr<ICoreWebView2WebResourceResponse> response;
    const HRESULT hr = env->CreateWebResourceResponse(stream.Get(), status, reason, headers.c_str(), &response);
    if (FAILED(hr)) return hr;
    return args->put_Response(response.Get());
}

}  // namespace

// ============================================================================
// 设置资源请求拦截：外壳来自 exe 资源段，大体积媒体来自外置数据目录
// ============================================================================
void Window::SetupResourceInterception() {
    if (!webview_) return;

    // 外置数据根目录：exe 内嵌页面外壳，大体积媒体放在这里
    dataRoot_ = ResolveDataRoot(config_.data_dir);

    // 注意：不要用 SetVirtualHostNameToFolderMapping 把 tauricpp.app 映射到目录。
    // 映射后的请求由 WebView2 直接从磁盘提供，不会经过 WebResourceRequested，
    // 内嵌在 exe 里的外壳就无法被命中。这里只做拦截，由处理函数自己作答：
    // 外壳来自 exe 资源段（内存），atlas 媒体来自外置目录（支持 Range）。

    // 拦截所有 tauricpp.app 请求，从内存VirtualFS提供内容
    ComPtr<ICoreWebView2_2> webview2;
    if (SUCCEEDED(webview_->QueryInterface(IID_PPV_ARGS(&webview2)))) {
        webview2->AddWebResourceRequestedFilter(L"https://tauricpp.app/*", COREWEBVIEW2_WEB_RESOURCE_CONTEXT_ALL);

        webview2->add_WebResourceRequested(
            Callback<ICoreWebView2WebResourceRequestedEventHandler>(
                [this](ICoreWebView2* sender, ICoreWebView2WebResourceRequestedEventArgs* args) -> HRESULT {
                    ComPtr<ICoreWebView2WebResourceRequest> request;
                    args->get_Request(&request);

                    LPWSTR uriW = nullptr;
                    request->get_Uri(&uriW);
                    std::string uri = WideToUtf8(uriW);
                    CoTaskMemFree(uriW);

                    // 解析虚拟路径：https://tauricpp.app/path -> /path
                    const std::string scheme = "https://tauricpp.app";
                    std::string vpath;
                    if (uri.find(scheme) == 0) {
                        vpath = uri.substr(scheme.size());
                        auto qpos = vpath.find('?');
                        if (qpos != std::string::npos) vpath = vpath.substr(0, qpos);
                        auto hpos = vpath.find('#');
                        if (hpos != std::string::npos) vpath = vpath.substr(0, hpos);
                        if (vpath.empty() || vpath[0] != '/') vpath = "/" + vpath;
                    } else {
                        return S_OK;
                    }

                    // 1) 页面外壳等小文件：直接来自 exe 内嵌的 VirtualFS
                    const VirtualFS::VFile* memory = VirtualFS::Instance().FindFile(vpath);
                    if (memory) {
                        const std::wstring headers = L"Content-Type: " + Utf8ToWide(memory->mime_type) + L"\r\n"
                                                   + L"Access-Control-Allow-Origin: *\r\n"
                                                   + L"Cache-Control: no-cache";
                        return ReplyWithBody(env_, args, 200, L"OK", headers,
                                             memory->data.data(), memory->data.size());
                    }

                    // 2) 外置数据目录（插画/音频等大文件）：按需读盘，支持 Range
                    //    音频播放依赖 206 分段响应，这里显式实现
                    std::wstring diskPath;
                    ULONGLONG diskSize = 0;
                    if (ResolveDataFile(dataRoot_, vpath, diskPath, diskSize)) {
                        ULONGLONG start = 0;
                        ULONGLONG end = 0;
                        bool partial = false;

                        ComPtr<ICoreWebView2HttpRequestHeaders> inHeaders;
                        if (SUCCEEDED(request->get_Headers(&inHeaders)) && inHeaders) {
                            LPWSTR rangeW = nullptr;
                            if (SUCCEEDED(inHeaders->GetHeader(L"Range", &rangeW)) && rangeW) {
                                ParseRangeHeader(rangeW, diskSize, start, end, partial);
                                CoTaskMemFree(rangeW);
                            }
                        }

                        std::wstring headers = L"Content-Type: " + Utf8ToWide(VirtualFS::InferMimeType(vpath)) + L"\r\n"
                                             + L"Access-Control-Allow-Origin: *\r\n"
                                             + L"Accept-Ranges: bytes\r\n"
                                             + L"Cache-Control: max-age=3600";

                        if (partial && start >= diskSize) {
                            headers += L"\r\nContent-Range: bytes */" + std::to_wstring(diskSize);
                            return ReplyWithBody(env_, args, 416, L"Range Not Satisfiable", headers, nullptr, 0);
                        }
                        if (!partial) {
                            start = 0;
                            end = diskSize > 0 ? diskSize - 1 : 0;
                        }

                        std::vector<uint8_t> body;
                        if (!ReadFileRange(diskPath, start, end, body)) {
                            return ReplyWithBody(env_, args, 500, L"Internal Server Error",
                                                 L"Content-Type: text/plain", "", 0);
                        }
                        if (partial) {
                            headers += L"\r\nContent-Range: bytes " + std::to_wstring(start) + L"-" +
                                       std::to_wstring(end) + L"/" + std::to_wstring(diskSize);
                        }
                        return ReplyWithBody(env_, args, partial ? 206 : 200,
                                             partial ? L"Partial Content" : L"OK",
                                             headers, body.data(), body.size());
                    }

                    // 3) 单页应用回退：没有扩展名的路径返回 index.html
                    const auto slash = vpath.rfind('/');
                    const bool hasExt = vpath.find('.', slash == std::string::npos ? 0 : slash) != std::string::npos;
                    if (!hasExt) {
                        const VirtualFS::VFile* index = VirtualFS::Instance().FindFile("/index.html");
                        if (index) {
                            const std::wstring headers = L"Content-Type: text/html\r\n"
                                                       + std::wstring(L"Access-Control-Allow-Origin: *\r\n"
                                                                      L"Cache-Control: no-cache");
                            return ReplyWithBody(env_, args, 200, L"OK", headers,
                                                 index->data.data(), index->data.size());
                        }
                    }

                    // 4) 其余直接 404
                    return ReplyWithBody(env_, args, 404, L"Not Found",
                                         L"Content-Type: text/plain\r\nAccess-Control-Allow-Origin: *",
                                         "", 0);
                }
            ).Get(),
            &event_tokens_.impl->webResourceRequestedToken
        );
    }
}

// ============================================================================
// 设置通信桥接
// ============================================================================
void Window::SetupBridge() {
    if (!webview_) return;

    // 设置Bridge的JS执行回调
    Bridge::Instance().SetExecuteJsCallback([this](const std::string& js) {
        ExecuteJs(js);
    });

    // 监听来自前端的消息
    ComPtr<ICoreWebView2_2> webview2;
    if (SUCCEEDED(webview_->QueryInterface(IID_PPV_ARGS(&webview2)))) {
        webview2->add_WebMessageReceived(
            Callback<ICoreWebView2WebMessageReceivedEventHandler>(
                [](ICoreWebView2* sender, ICoreWebView2WebMessageReceivedEventArgs* args) -> HRESULT {
                    LPWSTR msgW = nullptr;
                    args->get_WebMessageAsJson(&msgW);
                    std::string msgJson = WideToUtf8(msgW);
                    CoTaskMemFree(msgW);

                    try {
                        auto parsed = nlohmann::json::parse(msgJson);
                        nlohmann::json msg;
                        if (parsed.is_string()) {
                            msg = nlohmann::json::parse(parsed.get<std::string>());
                        } else {
                            msg = parsed;
                        }

                        if (msg.contains("__tauricpp_invoke") && msg["__tauricpp_invoke"].get<bool>()) {
                            int id = msg["id"].get<int>();
                            std::string cmd = msg["cmd"].get<std::string>();
                            std::string argsStr = msg["args"].dump();

                            std::string resultJson = Bridge::Instance().HandleInvoke(cmd, argsStr);

                            // 将结果发回前端
                            nlohmann::json response;
                            response["__tauricpp_result"] = true;
                            response["id"] = id;
                            try {
                                response["result"] = nlohmann::json::parse(resultJson);
                            } catch (...) {
                                response["result"] = resultJson;
                            }

                            // Use replace error handler to avoid type_error.316 on non-UTF-8 strings
                            std::string responseStr = response.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace);
                            sender->PostWebMessageAsJson(Utf8ToWide(responseStr).c_str());
                        }
                    } catch (const std::exception&) {}

                    return S_OK;
                }
            ).Get(),
            &event_tokens_.impl->webMessageReceivedToken
        );
    }

    // 在文档创建时注入桥接JS（在页面脚本执行之前）
    ComPtr<ICoreWebView2_5> webview5;
    if (SUCCEEDED(webview_->QueryInterface(IID_PPV_ARGS(&webview5)))) {
        std::string bridgeJs = Bridge::GetBridgeJs();
        webview5->AddScriptToExecuteOnDocumentCreated(
            Utf8ToWide(bridgeJs).c_str(), nullptr
        );
    } else {
        // 回退：导航完成后注入桥接JS
        webview_->add_NavigationCompleted(
            Callback<ICoreWebView2NavigationCompletedEventHandler>(
                [](ICoreWebView2* sender, ICoreWebView2NavigationCompletedEventArgs*) -> HRESULT {
                    std::string bridgeJs = Bridge::GetBridgeJs();
                    sender->ExecuteScript(Utf8ToWide(bridgeJs).c_str(), nullptr);
                    return S_OK;
                }
            ).Get(),
            &event_tokens_.impl->sourceChangedToken  // 复用sourceChangedToken作为回退令牌
        );
    }
}

// ============================================================================
// 执行JS - 通过PostMessage投递到UI线程安全执行
// ============================================================================
void Window::ExecuteJs(const std::string& js) {
    if (shutting_down_ || !hwnd_) return;

    // 入队JS字符串
    {
        std::lock_guard<std::mutex> lock(jsQueueMutex_);
        jsQueue_.push(js);
    }
    // 通知UI线程处理
    PostMessageW(hwnd_, WM_TAURICP_EXECUTE_JS, 0, 0);
}

// 在UI线程处理JS执行队列
void Window::ExecuteJsFromQueue() {
    if (!webview_ || !webview_ready_ || shutting_down_) return;
    
    while (true) {
        std::string js;
        {
            std::lock_guard<std::mutex> lock(jsQueueMutex_);
            if (jsQueue_.empty()) break;
            js = jsQueue_.front();
            jsQueue_.pop();
        }
        // Log first 80 chars for debugging
        std::string jsPreview = js.substr(0, 80);
        char buf[256];
        snprintf(buf, sizeof(buf), "[TauriCPP] ExecuteJsFromQueue: (len=%zu): %s\n", js.size(), jsPreview.c_str());
        OutputDebugStringA(buf);
        webview_->ExecuteScript(Utf8ToWide(js).c_str(), nullptr);
    }
}

// ============================================================================
// 窗口操作API
// ============================================================================
void Window::SetTitle(const std::string& title) {
    if (hwnd_) {
        SetWindowTextW(hwnd_, Utf8ToWide(title).c_str());
    }
}

void Window::SetSize(int width, int height) {
    if (!hwnd_) return;
    DWORD style = GetWindowLongW(hwnd_, GWL_STYLE);
    RECT rect = { 0, 0, width, height };
    AdjustWindowRect(&rect, style, FALSE);
    SetWindowPos(hwnd_, nullptr, 0, 0, rect.right - rect.left, rect.bottom - rect.top,
                 SWP_NOMOVE | SWP_NOZORDER);
}

void Window::SetPosition(int x, int y) {
    if (hwnd_) {
        SetWindowPos(hwnd_, nullptr, x, y, 0, 0, SWP_NOSIZE | SWP_NOZORDER);
    }
}

void Window::SetAlwaysOnTop(bool on_top) {
    if (hwnd_) {
        HWND zIndex = on_top ? HWND_TOPMOST : HWND_NOTOPMOST;
        SetWindowPos(hwnd_, zIndex, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE);
    }
}

void Window::SetResizable(bool resizable) {
    if (!hwnd_) return;
    DWORD style = GetWindowLongW(hwnd_, GWL_STYLE);
    if (resizable) {
        style |= WS_THICKFRAME | WS_MAXIMIZEBOX;
    } else {
        style &= ~(WS_THICKFRAME | WS_MAXIMIZEBOX);
    }
    SetWindowLongW(hwnd_, GWL_STYLE, style);
    SetWindowPos(hwnd_, nullptr, 0, 0, 0, 0, SWP_FRAMECHANGED | SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER);
}

void Window::SetIcon(HICON icon) {
    if (hwnd_ && icon) {
        SendMessageW(hwnd_, WM_SETICON, ICON_BIG, (LPARAM)icon);
        SendMessageW(hwnd_, WM_SETICON, ICON_SMALL, (LPARAM)icon);
    }
}

void Window::Minimize() {
    if (hwnd_) ShowWindow(hwnd_, SW_MINIMIZE);
}

void Window::Maximize() {
    if (hwnd_) ShowWindow(hwnd_, SW_MAXIMIZE);
}

void Window::Restore() {
    if (hwnd_) ShowWindow(hwnd_, SW_RESTORE);
}

bool Window::IsMinimized() const {
    return hwnd_ ? IsIconic(hwnd_) : false;
}

bool Window::IsMaximized() const {
    return hwnd_ ? IsZoomed(hwnd_) : false;
}

bool Window::IsFocused() const {
    return hwnd_ ? (GetForegroundWindow() == hwnd_) : false;
}

void Window::ToggleDevTools() {
    if (!webview_) return;
    ComPtr<ICoreWebView2_3> webview3;
    if (SUCCEEDED(webview_->QueryInterface(IID_PPV_ARGS(&webview3)))) {
        ComPtr<ICoreWebView2DevToolsProtocolEventReceiver> receiver;
        // 使用 CDP 命令切换 DevTools
        static bool devtools_open = false;
        if (devtools_open) {
            webview3->CallDevToolsProtocolMethod(L"Page.close", L"{}", nullptr);
        } else {
            webview3->CallDevToolsProtocolMethod(L"Page.enable", L"{}", nullptr);
            // 打开DevTools窗口
            ComPtr<ICoreWebView2_6> webview6;
            if (SUCCEEDED(webview_->QueryInterface(IID_PPV_ARGS(&webview6)))) {
                webview6->OpenDevToolsWindow();
            }
        }
        devtools_open = !devtools_open;
    }
}

// ============================================================================
// 从队列取出并执行JS（必须在UI线程调用）
// ============================================================================
// ============================================================================
// 运行
// ============================================================================
int Window::Run() {
    if (!CreateNativeWindow()) return -1;

    // 记录主线程ID
    mainThreadId_ = GetCurrentThreadId();

    // 立即显示窗口（背景色已通过SetWebViewBackgroundColor和窗口类画刷设置为深色）
    // 这样用户立刻看到窗口，而不是等WebView2初始化
    ShowWindow(hwnd_, SW_SHOW);
    UpdateWindow(hwnd_);

    // ★ 启动焦点：让窗口成为前台并取得键盘焦点
    SetForegroundWindow(hwnd_);
    SetFocus(hwnd_);

    // 启动前检测 WebView2 Runtime 是否可用，缺失则给出友好提示
    if (!IsWebView2RuntimeAvailable()) {
        ShowWebView2MissingMessage(hwnd_);
        return -2;
    }

    if (!InitWebView()) return -1;

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    return static_cast<int>(msg.wParam);
}

void Window::Close() {
    if (hwnd_) {
        PostMessage(hwnd_, WM_CLOSE, 0, 0);
    }
}

} // namespace tauricpp
