// 词光 Word Atlas —— TauriCPP 桌面外壳（C++ / Win32 / WebView2）
//
// 前端是 ../web 里的 React 应用（Vite 构建）。构建产物只有外壳：
// index.html、assets/*.js|css、品牌图与音效，编译期嵌入 exe 资源段，
// 运行时由 VirtualFS 从内存提供，不落地任何临时文件。
//
// 7,298 个词的插画 / 缩略图 / 英美发音 / 词条分块共 600MB+，放在 exe
// 同目录的 atlas/ 下，由框架按需读盘（支持 Range，音频可拖动播放），
// 因此 exe 只有几 MB，启动快、内存占用低。
#include "tauricpp/app.hpp"

#include <Windows.h>
#include <shellapi.h>

using nlohmann::json;

namespace {

// 界面语言为中文，窗口标题保留中英双写，任务栏更好认
constexpr const char* kAppTitle = "词光 Word Atlas";

/// UTF-8 -> UTF-16，用于把网页里的链接交给 ShellExecuteW
std::wstring ToWide(const std::string& text) {
    if (text.empty()) return {};
    const int len = MultiByteToWideChar(CP_UTF8, 0, text.c_str(), -1, nullptr, 0);
    if (len <= 0) return {};
    std::wstring wide(static_cast<size_t>(len - 1), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.c_str(), -1, wide.data(), len);
    return wide;
}

bool IsHttpUrl(const std::string& url) {
    return url.rfind("https://", 0) == 0 || url.rfind("http://", 0) == 0;
}

}  // namespace

int WINAPI WinMain(HINSTANCE, HINSTANCE, LPSTR, int) {
    tauricpp::App::Config config;
    config.window_config.title = kAppTitle;
    config.window_config.width = 1280;
    config.window_config.height = 860;
    config.window_config.center = true;
    config.window_config.resizable = true;
    config.window_config.devtools = true;                 // F12 打开 DevTools
    config.window_config.bg_color = RGB(13, 17, 32);      // #0D1120，与前端主题同色，消除白屏
    config.window_config.start_url = "https://tauricpp.app/index.html";
    config.window_config.data_dir = "";                   // 空 = 外置数据就在 exe 同目录（atlas/）

    tauricpp::App app(config);
    auto& bridge = app.GetBridge();

    // 前端可据此识别"桌面版"并降级（例如跳过 Service Worker、隐藏"安装到桌面"之类入口）
    bridge.RegisterCommand("app.info", [](const json&) -> json {
        return json{
            {"name", "word-atlas"},
            {"shell", "tauricpp"},
            {"platform", "windows"},
            {"desktop", true},
        };
    });

    // 关于页里的 GitHub 链接等，统一交给系统默认浏览器
    bridge.RegisterCommand("app.open_external", [](const json& args) -> json {
        const std::string url = args.value("url", "");
        if (!IsHttpUrl(url)) {
            return json{{"success", false}, {"reason", "only http(s) urls are allowed"}};
        }
        ShellExecuteW(nullptr, L"open", ToWide(url).c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        return json{{"success", true}};
    });

    bridge.RegisterCommand("app.quit", [&app](const json&) -> json {
        app.GetWindow().Close();
        return json{{"success", true}};
    });

    return app.Run();
}
