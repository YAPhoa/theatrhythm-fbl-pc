// SPDX-License-Identifier: GPL-2.0-or-later
// theatrhythm-setup.exe: sets up the release folder it sits in (tools/package.py) from the player's
// Theatrhythm Final Bar Line dump, so the game starts from theatrhythm-fbl.exe or a desktop shortcut.
//
// With no arguments it opens a window: the game folder (the dump), the save folder, an optional extra
// DLC folder, whether to skip the opening movie (settings.ini skip_opening), Install and Exit; after an
// install it offers a desktop shortcut. Run it again to change a folder, or after unpacking a newer
// release over the folder.
// With arguments it installs from the command line:
//
//     theatrhythm-setup.exe --game <dump or eboot.bin> [--save <folder>] [--copy-saves]
//                           [--dlc <folder>|--no-dlc] [--skip-opening|--play-opening]
//                           [--shortcut <folder>|desktop]

#include "setup.h"

#include <windows.h>
#include <commctrl.h>
#include <shellapi.h>
#include <shobjidl.h>

#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace fs = std::filesystem;

namespace {

constexpr wchar_t kTitle[] = L"Theatrhythm Final Bar Line setup";

// The old save folder to copy from when the save folder changes and only the old one has saves.
std::optional<fs::path> saves_to_move(const fs::path& root, const fs::path& save) {
    auto old = setup::remembered_paths(root);
    if (!old || setup::same_path(old->save, save) || !setup::has_saves(old->save) || setup::has_saves(save))
        return std::nullopt;
    return old->save;
}

// --- command line -------------------------------------------------------------------------------

// Writes to the standard handles a parent gave us, or else to the console of the shell we were started from.
class Console {
public:
    Console() {
        out_ = GetStdHandle(STD_OUTPUT_HANDLE);
        err_ = GetStdHandle(STD_ERROR_HANDLE);
        if ((!out_ || !err_) && AttachConsole(ATTACH_PARENT_PROCESS)) {
            HANDLE con = CreateFileW(L"CONOUT$", GENERIC_READ | GENERIC_WRITE, FILE_SHARE_WRITE, nullptr,
                                     OPEN_EXISTING, 0, nullptr);
            if (!out_)
                out_ = con;
            if (!err_)
                err_ = con;
        }
    }
    void out(const std::wstring& s) const { write(out_, s + L"\n"); }
    void err(const std::wstring& s) const { write(err_, s + L"\n"); }

private:
    static void write(HANDLE h, const std::wstring& s) {
        if (!h || h == INVALID_HANDLE_VALUE)
            return;
        DWORD mode, n;
        if (GetConsoleMode(h, &mode)) {
            WriteConsoleW(h, s.data(), static_cast<DWORD>(s.size()), &n, nullptr);
            return;
        }
        int len = WideCharToMultiByte(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0, nullptr, nullptr);
        std::string u(len, '\0');
        WideCharToMultiByte(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), u.data(), len, nullptr, nullptr);
        WriteFile(h, u.data(), static_cast<DWORD>(u.size()), &n, nullptr);
    }
    HANDLE out_ = nullptr;
    HANDLE err_ = nullptr;
};

constexpr wchar_t kUsage[] =
    L"usage: theatrhythm-setup.exe --game <dump or eboot.bin> [--save <folder>] [--copy-saves]\n"
    L"                             [--dlc <folder>|--no-dlc] [--skip-opening|--play-opening]\n"
    L"                             [--shortcut <folder>|desktop]\n"
    L"  --game        the decrypted game dump (eboot.bin, sce_module, sce_sys, Media); only ever read\n"
    L"  --save        where your saves go (default: the one paths.ini names, else save\\ here)\n"
    L"  --copy-saves  when --save changes, copy the saves from the old save folder\n"
    L"  --dlc         one more folder of DLC to read, e.g. shadPS4's addcont (default: the one\n"
    L"                paths.ini names); --no-dlc reads only the save folder's addcont\n"
    L"  --skip-opening, --play-opening\n"
    L"                start at the title screen without the opening movie, or play it again\n"
    L"                (settings.ini skip_opening; default: leave it as it is)\n"
    L"  --shortcut    also write the game's shortcut into this folder, or onto the Desktop\n"
    L"Sets up the folder this exe is in. With no arguments, opens the setup window.";

int run_cli(const std::vector<std::wstring>& args) {
    Console con;
    std::wstring game, save, shortcut, dlc;
    bool copy_saves = false, no_dlc = false;
    std::optional<bool> skip_opening;
    for (size_t i = 0; i < args.size(); ++i) {
        const std::wstring& a = args[i];
        std::wstring* value = a == L"--game"       ? &game
                              : a == L"--save"     ? &save
                              : a == L"--shortcut" ? &shortcut
                              : a == L"--dlc"      ? &dlc
                                                   : nullptr;
        if (value && i + 1 < args.size()) {
            *value = args[++i];
        } else if (a == L"--copy-saves") {
            copy_saves = true;
        } else if (a == L"--skip-opening" || a == L"--play-opening") {
            skip_opening = a == L"--skip-opening";
        } else if (a == L"--no-dlc") {
            no_dlc = true;
        } else {
            con.err(a == L"--help" || a == L"-h" ? kUsage : std::wstring(kUsage) + L"\n\nunexpected: " + a);
            return 2;
        }
    }
    if (game.empty()) {
        con.err(std::wstring(kUsage) + L"\n\n--game is required");
        return 2;
    }
    try {
        setup::Options opt;
        opt.root = setup::exe_folder();
        opt.game = game;
        auto old = setup::remembered_paths(opt.root);
        opt.save = !save.empty() ? fs::path(save) : old ? old->save : setup::default_save();
        opt.dlc = no_dlc ? fs::path() : !dlc.empty() ? fs::path(dlc) : old ? old->dlc : fs::path();
        if (copy_saves)
            opt.copy_saves_from = saves_to_move(opt.root, opt.save);
        opt.skip_opening = skip_opening;
        fs::path exe = setup::install(opt, [&](const std::wstring& line) { con.out(line); });
        if (!shortcut.empty()) {
            fs::path lnk = (shortcut == L"desktop" ? setup::desktop_folder() : fs::path(shortcut)) / setup::kShortcutName;
            setup::create_shortcut(exe, lnk);
            con.out(L"shortcut " + lnk.wstring());
        }
        con.out(L"\nDone. Run " + exe.wstring());
        return 0;
    } catch (const setup::Error& e) {
        con.err(e.message);
        return 1;
    }
}

// --- window -------------------------------------------------------------------------------------

enum : int {
    kGameEdit = 101,
    kGameBrowse,
    kSaveEdit,
    kSaveBrowse,
    kDlcEdit,
    kDlcBrowse,
    kLog,
    kPlay,
    kSkipOpening,
};
enum : UINT {
    kMsgLog = WM_APP + 1, // lParam: std::wstring* line
    kMsgDone,             // wParam: 1 ok; lParam: std::wstring* exe path or error
};

struct Ui {
    HWND wnd = nullptr;
    HWND game = nullptr, save = nullptr, dlc = nullptr, skip_opening = nullptr, log = nullptr, play = nullptr;
    std::vector<HWND> inputs; // disabled while installing (Play Game too, and until the folder is set up)
    HFONT font = nullptr;
    UINT dpi = 96;
    bool busy = false;
    fs::path root;
};
Ui ui;

int px(int v) {
    return MulDiv(v, static_cast<int>(ui.dpi), 96);
}

std::wstring text_of(HWND h) {
    std::wstring s(static_cast<size_t>(GetWindowTextLengthW(h)), L'\0');
    GetWindowTextW(h, s.data(), static_cast<int>(s.size()) + 1);
    size_t a = s.find_first_not_of(L" \t\""), b = s.find_last_not_of(L" \t\"");
    return a == std::wstring::npos ? L"" : s.substr(a, b - a + 1);
}

void append_log(const std::wstring& line) {
    int end = GetWindowTextLengthW(ui.log);
    SendMessageW(ui.log, EM_SETSEL, end, end);
    SendMessageW(ui.log, EM_REPLACESEL, FALSE, reinterpret_cast<LPARAM>((line + L"\r\n").c_str()));
}

void set_busy(bool busy) {
    ui.busy = busy;
    for (HWND h : ui.inputs)
        EnableWindow(h, !busy);
    EnableWindow(ui.play, !busy && setup::ready(ui.root));
}

// Starts the game as a double-click would (its own folder, no console), then closes the setup.
void play() {
    fs::path exe = ui.root / L"theatrhythm-fbl.exe";
    auto started = reinterpret_cast<INT_PTR>(
        ShellExecuteW(ui.wnd, L"open", exe.c_str(), nullptr, ui.root.c_str(), SW_SHOWNORMAL));
    if (started <= 32) {
        MessageBoxW(ui.wnd, (L"Cannot start " + exe.wstring()).c_str(), kTitle, MB_OK | MB_ICONERROR);
        return;
    }
    DestroyWindow(ui.wnd);
}

// The shell's folder picker, starting in the folder the field names when it exists.
void browse(HWND edit, const wchar_t* title) {
    IFileOpenDialog* dlg = nullptr;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dlg))))
        return;
    DWORD opts = 0;
    dlg->GetOptions(&opts);
    dlg->SetTitle(title);
    dlg->SetOptions(opts | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM);
    std::wstring current = text_of(edit);
    if (!current.empty()) {
        std::error_code ec;
        fs::path start = setup::game_folder(current);
        IShellItem* item = nullptr;
        if (fs::is_directory(start, ec) &&
            SUCCEEDED(SHCreateItemFromParsingName(start.c_str(), nullptr, IID_PPV_ARGS(&item)))) {
            dlg->SetFolder(item);
            item->Release();
        }
    }
    IShellItem* result = nullptr;
    if (SUCCEEDED(dlg->Show(ui.wnd)) && SUCCEEDED(dlg->GetResult(&result))) {
        PWSTR path = nullptr;
        if (SUCCEEDED(result->GetDisplayName(SIGDN_FILESYSPATH, &path))) {
            SetWindowTextW(edit, path);
            CoTaskMemFree(path);
        }
        result->Release();
    }
    dlg->Release();
}

void start_install() {
    if (ui.busy)
        return;
    std::wstring game = text_of(ui.game), save = text_of(ui.save), dlc = text_of(ui.dlc);
    if (game.empty()) {
        MessageBoxW(ui.wnd, L"Choose the game folder (your game dump).", kTitle, MB_OK | MB_ICONWARNING);
        return;
    }
    if (save.empty()) {
        MessageBoxW(ui.wnd, L"Choose where to keep your saves.", kTitle, MB_OK | MB_ICONWARNING);
        return;
    }
    setup::Options opt;
    opt.root = ui.root;
    opt.game = game;
    opt.save = save;
    opt.dlc = dlc;
    opt.skip_opening = SendMessageW(ui.skip_opening, BM_GETCHECK, 0, 0) == BST_CHECKED;
    try {
        if (auto old = saves_to_move(opt.root, opt.save)) {
            std::wstring ask = L"Copy your saves to the new save folder?\n\nFrom: " + old->wstring() + L"\nTo: " + save;
            int answer = MessageBoxW(ui.wnd, ask.c_str(), kTitle, MB_YESNOCANCEL | MB_ICONQUESTION);
            if (answer == IDCANCEL)
                return;
            if (answer == IDYES)
                opt.copy_saves_from = old;
        }
    } catch (const setup::Error& e) {
        MessageBoxW(ui.wnd, e.message.c_str(), kTitle, MB_OK | MB_ICONERROR);
        return;
    }
    SetWindowTextW(ui.log, L"");
    set_busy(true);
    std::thread([opt, wnd = ui.wnd] {
        auto log = [wnd](const std::wstring& line) {
            PostMessageW(wnd, kMsgLog, 0, reinterpret_cast<LPARAM>(new std::wstring(line)));
        };
        try {
            fs::path exe = setup::install(opt, log);
            PostMessageW(wnd, kMsgDone, 1, reinterpret_cast<LPARAM>(new std::wstring(exe.wstring())));
        } catch (const setup::Error& e) {
            PostMessageW(wnd, kMsgDone, 0, reinterpret_cast<LPARAM>(new std::wstring(e.message)));
        }
    }).detach();
}

void finish_install(bool ok, const std::wstring& value) {
    set_busy(false);
    if (!ok) {
        append_log(L"Install failed.");
        MessageBoxW(ui.wnd, value.c_str(), kTitle, MB_OK | MB_ICONERROR);
        return;
    }
    append_log(L"Done. Press Play Game to start.");
    if (MessageBoxW(ui.wnd, L"Done!\n\nAdd a shortcut to the game on your desktop?", kTitle,
                    MB_YESNO | MB_ICONQUESTION) == IDYES) {
        try {
            fs::path lnk = setup::desktop_folder() / setup::kShortcutName;
            setup::create_shortcut(value, lnk);
            append_log(L"Shortcut " + lnk.wstring());
        } catch (const setup::Error& e) {
            MessageBoxW(ui.wnd, e.message.c_str(), kTitle, MB_OK | MB_ICONERROR);
        }
    }
    SetFocus(ui.play); // Enter now starts the game
}

HWND add(const wchar_t* cls, const wchar_t* text, DWORD style, int x, int y, int w, int h, int id, DWORD ex = 0) {
    HWND c = CreateWindowExW(ex, cls, text, WS_CHILD | WS_VISIBLE | style, px(x), px(y), px(w), px(h), ui.wnd,
                             reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), GetModuleHandleW(nullptr), nullptr);
    SendMessageW(c, WM_SETFONT, reinterpret_cast<WPARAM>(ui.font), FALSE);
    return c;
}

// Layout in 96-dpi units; the client area is kWidth x kHeight.
constexpr int kWidth = 600, kHeight = 414, kMargin = 12, kButton = 90;

void create_controls() {
    const int edit_w = kWidth - 2 * kMargin - kButton - 6, right = kWidth - kMargin - kButton;
    add(WC_STATICW, L"Game folder (your game dump):", 0, kMargin, 12, edit_w, 18, 0);
    ui.game = add(WC_EDITW, L"", WS_TABSTOP | ES_AUTOHSCROLL, kMargin, 32, edit_w, 24, kGameEdit, WS_EX_CLIENTEDGE);
    HWND game_browse = add(WC_BUTTONW, L"Browse...", WS_TABSTOP, right, 31, kButton, 26, kGameBrowse);
    add(WC_STATICW, L"Save folder:", 0, kMargin, 68, edit_w, 18, 0);
    ui.save = add(WC_EDITW, L"", WS_TABSTOP | ES_AUTOHSCROLL, kMargin, 88, edit_w, 24, kSaveEdit, WS_EX_CLIENTEDGE);
    HWND save_browse = add(WC_BUTTONW, L"Browse...", WS_TABSTOP, right, 87, kButton, 26, kSaveBrowse);
    add(WC_STATICW, L"Extra DLC folder (optional, e.g. shadPS4's addcont; empty for none):", 0, kMargin, 124, edit_w, 18, 0);
    ui.dlc = add(WC_EDITW, L"", WS_TABSTOP | ES_AUTOHSCROLL, kMargin, 144, edit_w, 24, kDlcEdit, WS_EX_CLIENTEDGE);
    HWND dlc_browse = add(WC_BUTTONW, L"Browse...", WS_TABSTOP, right, 143, kButton, 26, kDlcBrowse);
    ui.skip_opening = add(WC_BUTTONW, L"Skip the opening movie (set skip_opening=0 in settings.ini to bring it back)",
                          WS_TABSTOP | BS_AUTOCHECKBOX, kMargin, 180, kWidth - 2 * kMargin, 22, kSkipOpening);
    ui.log = add(WC_EDITW, L"", ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL | WS_VSCROLL, kMargin, 210,
                 kWidth - 2 * kMargin, 150, kLog, WS_EX_CLIENTEDGE);
    ui.play = add(WC_BUTTONW, L"Play Game", WS_TABSTOP, right - 2 * (kButton + 6), 374, kButton, 28, kPlay);
    HWND install = add(WC_BUTTONW, L"Install", WS_TABSTOP | BS_DEFPUSHBUTTON, right - kButton - 6, 374, kButton, 28, IDOK);
    HWND exit = add(WC_BUTTONW, L"Exit", WS_TABSTOP, right, 374, kButton, 28, IDCANCEL);
    ui.inputs = {ui.game, game_browse, ui.save, save_browse, ui.dlc, dlc_browse, ui.skip_opening, install, exit};
    set_busy(false);

    // Start from what the last install chose (paths.ini here, else the per-user copy), else an empty
    // game folder and save\ here.
    std::optional<setup::Paths> paths;
    try {
        paths = setup::remembered_paths(ui.root);
    } catch (const setup::Error&) {
    }
    fs::path game = paths ? paths->game : fs::path();
    fs::path save = paths ? paths->save : setup::default_save();
    SetWindowTextW(ui.game, game.c_str());
    SetWindowTextW(ui.save, save.c_str());
    if (paths)
        SetWindowTextW(ui.dlc, paths->dlc.c_str());
    // What settings.ini says now; Install writes the box's state, so unticking it brings the movie back.
    SendMessageW(ui.skip_opening, BM_SETCHECK, setup::skip_opening(ui.root, save) ? BST_CHECKED : BST_UNCHECKED, 0);
    append_log(setup::ready(ui.root) ? L"The game is set up. Press Play Game, or pick new folders and press Install."
               : paths              ? L"This version isn't set up yet. Press Install."
                                    : L"Pick the folders, then press Install.");
}

LRESULT CALLBACK wnd_proc(HWND wnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case IDOK:
            start_install();
            return 0;
        case kPlay:
            play();
            return 0;
        case IDCANCEL:
            if (!ui.busy)
                DestroyWindow(wnd);
            return 0;
        case kGameBrowse:
            browse(ui.game, L"Select the game folder (the one with eboot.bin)");
            return 0;
        case kSaveBrowse:
            browse(ui.save, L"Select the save folder");
            return 0;
        case kDlcBrowse:
            browse(ui.dlc, L"Select a folder of DLC (e.g. shadPS4's addcont)");
            return 0;
        }
        break;
    case WM_CTLCOLORSTATIC:
        if (reinterpret_cast<HWND>(lp) == ui.log) { // the read-only log reads as a text box, not a label
            SetBkColor(reinterpret_cast<HDC>(wp), GetSysColor(COLOR_WINDOW));
            return reinterpret_cast<LRESULT>(GetSysColorBrush(COLOR_WINDOW));
        }
        break;
    case kMsgLog: {
        std::unique_ptr<std::wstring> line(reinterpret_cast<std::wstring*>(lp));
        append_log(*line);
        return 0;
    }
    case kMsgDone: {
        std::unique_ptr<std::wstring> value(reinterpret_cast<std::wstring*>(lp));
        finish_install(wp == 1, *value);
        return 0;
    }
    case WM_CLOSE:
        if (!ui.busy)
            DestroyWindow(wnd);
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(wnd, msg, wp, lp);
}

int run_window(HINSTANCE inst) {
    ui.root = setup::exe_folder();
    INITCOMMONCONTROLSEX icc = {sizeof(icc), ICC_STANDARD_CLASSES};
    InitCommonControlsEx(&icc);
    ui.dpi = GetDpiForSystem();
    NONCLIENTMETRICSW ncm = {};
    ncm.cbSize = sizeof(ncm);
    SystemParametersInfoForDpi(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0, ui.dpi);
    ui.font = CreateFontIndirectW(&ncm.lfMessageFont);

    WNDCLASSEXW wc = {sizeof(wc)};
    wc.lpfnWndProc = wnd_proc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hIcon = LoadIconW(inst, MAKEINTRESOURCEW(1));
    wc.hbrBackground = GetSysColorBrush(COLOR_BTNFACE);
    wc.lpszClassName = L"theatrhythm_setup";
    RegisterClassExW(&wc);

    const DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
    RECT r = {0, 0, px(kWidth), px(kHeight)};
    AdjustWindowRectExForDpi(&r, style, FALSE, 0, ui.dpi);
    ui.wnd = CreateWindowExW(0, wc.lpszClassName, kTitle, style, CW_USEDEFAULT, CW_USEDEFAULT, r.right - r.left,
                             r.bottom - r.top, nullptr, nullptr, inst, nullptr);
    create_controls();
    ShowWindow(ui.wnd, SW_SHOW);
    SetFocus(ui.game);

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        if (IsDialogMessageW(ui.wnd, &msg)) // Tab between controls, Enter = Install, Esc = Exit
            continue;
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    DeleteObject(ui.font);
    return 0;
}

} // namespace

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, PWSTR, int) {
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    std::vector<std::wstring> args(argv + 1, argv + argc);
    LocalFree(argv);
    if (!args.empty())
        return run_cli(args);
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    int rc = run_window(inst);
    CoUninitialize();
    return rc;
}
