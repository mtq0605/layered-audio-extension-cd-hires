// SPDX-FileCopyrightText: 2026 mtq0605
// SPDX-License-Identifier: Apache-2.0
// lae_dshow_setup.exe: installs the LAE DirectShow source filter for the current user (no administrator rights): the filter (built into
// this program as a resource) goes to %LOCALAPPDATA%\Programs\LAE\DirectShow, is registered (its own DllRegisterServer: HKCU), and an
// entry in Windows' installed apps list lets it be removed. Texts are Chinese when the Windows display language is Chinese.
//   lae_dshow_setup.exe              asks, then installs (or updates)
//   lae_dshow_setup.exe /uninstall   asks, then removes (the entry in the apps list runs this)
//   /quiet                           with either: no questions, no messages
#include <windows.h>
#include <shlobj.h>
#include <string>
#define IDR_FILTER 101

namespace {
// texts: two standalone builds, English (the standard) and Chinese (/DLAE_LANG_ZH)
#ifdef LAE_LANG_ZH
const bool g_zh = true;
#else
const bool g_zh = false;
#endif
bool g_quiet = false;
const wchar_t* T(const wchar_t* en, const wchar_t* zh) { return g_zh ? zh : en; }
const wchar_t* kTitle = L"LAE DirectShow";
const wchar_t* kUninstKey = L"Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\LAE.DirectShow";
const wchar_t* kVersion = L"0.2.11";

std::wstring installDir() { wchar_t* p = nullptr; std::wstring d; if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &p))) d = p; CoTaskMemFree(p);
    return d + L"\\Programs\\LAE\\DirectShow"; }
void say(const std::wstring& s, UINT icon = MB_ICONINFORMATION) { if (!g_quiet) MessageBoxW(nullptr, s.c_str(), kTitle, MB_OK | icon); }
bool ask(const std::wstring& s) { return g_quiet || MessageBoxW(nullptr, s.c_str(), kTitle, MB_YESNO | MB_ICONQUESTION) == IDYES; }
HRESULT callExport(const std::wstring& dll, const char* fn) { HMODULE h = LoadLibraryW(dll.c_str()); if (!h) return HRESULT_FROM_WIN32(GetLastError());
    typedef HRESULT(STDAPICALLTYPE* Fn)(); Fn f = (Fn)GetProcAddress(h, fn); const HRESULT hr = f ? f() : E_FAIL; FreeLibrary(h); return hr; }
void setStr(HKEY k, const wchar_t* name, const std::wstring& v) { RegSetValueExW(k, name, 0, REG_SZ, (const BYTE*)v.c_str(), (DWORD)((v.size() + 1) * sizeof(wchar_t))); }

int install() {
    if (!ask(T(L"Install the LAE source filter for DirectShow players for the current user?",
               L"为当前用户安装 DirectShow 播放器用的 LAE 源滤镜？"))) return 1;
    const std::wstring dir = installDir(); SHCreateDirectoryExW(nullptr, dir.c_str(), nullptr);
    const std::wstring ax = dir + L"\\lae_source.ax", self = dir + L"\\lae_dshow_setup.exe";
    HRSRC r = FindResourceW(nullptr, MAKEINTRESOURCEW(IDR_FILTER), RT_RCDATA); HGLOBAL g = r ? LoadResource(nullptr, r) : nullptr; const DWORD n = r ? SizeofResource(nullptr, r) : 0;
    if (!g || !n) { say(T(L"Internal error: the filter is missing from the installer.", L"内部错误：安装程序中缺少滤镜。"), MB_ICONERROR); return 2; }
    callExport(ax, "DllUnregisterServer");   // an earlier version, if any
    HANDLE h = CreateFileW(ax.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) { say(T(L"The filter is in use. Close all players and run the installer again.",
                                           L"滤镜正在使用。请关闭所有播放器后重新运行安装程序。"), MB_ICONWARNING); return 3; }
    DWORD w = 0; const BOOL ok = WriteFile(h, LockResource(g), n, &w, nullptr) && w == n; CloseHandle(h); if (!ok) { say(T(L"Cannot write ", L"无法写入 ") + ax, MB_ICONERROR); return 4; }
    wchar_t me[MAX_PATH]; GetModuleFileNameW(nullptr, me, MAX_PATH); if (_wcsicmp(me, self.c_str()) != 0) CopyFileW(me, self.c_str(), FALSE);
    const HRESULT hr = callExport(ax, "DllRegisterServer"); if (FAILED(hr)) { say(T(L"Registration failed.", L"注册失败。"), MB_ICONERROR); return 5; }
    HKEY k; if (RegCreateKeyExW(HKEY_CURRENT_USER, kUninstKey, 0, nullptr, 0, KEY_WRITE, nullptr, &k, nullptr) == ERROR_SUCCESS) {
        setStr(k, L"DisplayName", T(L"LAE source filter (DirectShow)", L"LAE 源滤镜（DirectShow）")); setStr(k, L"DisplayVersion", kVersion); setStr(k, L"Publisher", L"mtq0605");
        setStr(k, L"InstallLocation", dir); setStr(k, L"UninstallString", L"\"" + self + L"\" /uninstall"); setStr(k, L"QuietUninstallString", L"\"" + self + L"\" /uninstall /quiet");
        const DWORD one = 1; RegSetValueExW(k, L"NoModify", 0, REG_DWORD, (const BYTE*)&one, 4); RegSetValueExW(k, L"NoRepair", 0, REG_DWORD, (const BYTE*)&one, 4); RegCloseKey(k); }
    say(T(L"Installed. Files with the extension .lae open with this filter. For .wav / .flac LAE files, add the filter in the player:\n"
          L"PotPlayer: Preferences (F5) > Filters > Global filter priority > Add system filter > LAE source > Force use\n"
          L"MPC-BE: Options > External Filters > Add Filter > LAE source > Prefer\n\nInstalled in: ",
          L"已安装。扩展名为 .lae 的文件用本滤镜打开。.wav / .flac 形式的 LAE 文件需在播放器中添加本滤镜：\n"
          L"PotPlayer：参数选项（F5）> 滤镜 > 全局滤镜优先权 > 添加系统滤镜 > LAE source > 强制使用\n"
          L"MPC-BE：选项 > 外部滤镜 > 添加滤镜 > LAE source > 优先\n\n安装位置：") + dir);
    return 0; }

int uninstall() {
    if (!ask(T(L"Remove the LAE source filter?", L"卸载 LAE 源滤镜？"))) return 1;
    const std::wstring dir = installDir(), ax = dir + L"\\lae_source.ax", self = dir + L"\\lae_dshow_setup.exe";
    callExport(ax, "DllUnregisterServer");
    if (!DeleteFileW(ax.c_str()) && GetFileAttributesW(ax.c_str()) != INVALID_FILE_ATTRIBUTES) {
        say(T(L"The filter is in use. Close all players and run the uninstaller again.", L"滤镜正在使用。请关闭所有播放器后重新卸载。"), MB_ICONWARNING); return 3; }
    RegDeleteTreeW(HKEY_CURRENT_USER, kUninstKey);
    // this program cannot delete itself while it runs: a short command line removes it and the empty folders after it has ended
    wchar_t me[MAX_PATH]; GetModuleFileNameW(nullptr, me, MAX_PATH);
    if (!_wcsicmp(me, self.c_str())) { std::wstring cmd = L"cmd.exe /c ping -n 3 127.0.0.1 >nul & del /f /q \"" + self + L"\" & rmdir \"" + dir + L"\" & rmdir \"" + dir.substr(0, dir.rfind(L'\\')) + L"\"";
        STARTUPINFOW si{sizeof si}; si.dwFlags = STARTF_USESHOWWINDOW; si.wShowWindow = SW_HIDE; PROCESS_INFORMATION pi{};
        if (CreateProcessW(nullptr, &cmd[0], nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) { CloseHandle(pi.hThread); CloseHandle(pi.hProcess); } }
    else { DeleteFileW(self.c_str()); RemoveDirectoryW(dir.c_str()); RemoveDirectoryW(dir.substr(0, dir.rfind(L'\\')).c_str()); }
    say(T(L"Removed. Verification results in %APPDATA%\\LAE are kept (shared with the other LAE plug-ins).",
          L"已卸载。%APPDATA%\\LAE 中的校验结果保留（其他 LAE 插件共用）。"));
    return 0; }
}  // namespace

int WINAPI wWinMain(HINSTANCE, HINSTANCE, LPWSTR cmdLine, int) {
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED); const std::wstring c = cmdLine ? cmdLine : L"";
    g_quiet = c.find(L"/quiet") != std::wstring::npos; const int r = c.find(L"/uninstall") != std::wstring::npos ? uninstall() : install();
    CoUninitialize(); return r; }
