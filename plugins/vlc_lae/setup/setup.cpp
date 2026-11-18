// SPDX-FileCopyrightText: 2026 mtq0605
// SPDX-License-Identifier: Apache-2.0
// lae_vlc_setup.exe: installs the LAE input module for VLC 3.0 (64-bit) for the current user (no administrator rights): the module (built
// into this program as a resource) goes to %LOCALAPPDATA%\Programs\LAE\VLC, that folder is added to the user's environment variable
// VLC_PLUGIN_PATH (VLC 3 loads modules from the folders listed there; entries already there are kept), and an entry in Windows' installed
// apps list lets it be removed. Texts are Chinese when the Windows display language is Chinese.
//   lae_vlc_setup.exe              asks, then installs (or updates)
//   lae_vlc_setup.exe /uninstall   asks, then removes (the entry in the apps list runs this)
//   /quiet                         with either: no questions, no messages
#include <windows.h>
#include <shlobj.h>
#include <string>
#include <vector>
#define IDR_MODULE 101

namespace {
// texts: two standalone builds, English (the standard) and Chinese (/DLAE_LANG_ZH)
#ifdef LAE_LANG_ZH
const bool g_zh = true;
#else
const bool g_zh = false;
#endif
bool g_quiet = false;
const wchar_t* T(const wchar_t* en, const wchar_t* zh) { return g_zh ? zh : en; }
const wchar_t* kTitle = L"LAE VLC";
const wchar_t* kUninstKey = L"Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\LAE.VLC";
const wchar_t* kVersion = L"0.1.12";

std::wstring installDir() { wchar_t* p = nullptr; std::wstring d; if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &p))) d = p; CoTaskMemFree(p);
    return d + L"\\Programs\\LAE\\VLC"; }
void say(const std::wstring& s, UINT icon = MB_ICONINFORMATION) { if (!g_quiet) MessageBoxW(nullptr, s.c_str(), kTitle, MB_OK | icon); }
bool ask(const std::wstring& s) { return g_quiet || MessageBoxW(nullptr, s.c_str(), kTitle, MB_YESNO | MB_ICONQUESTION) == IDYES; }
void setStr(HKEY k, const wchar_t* name, const std::wstring& v) { RegSetValueExW(k, name, 0, REG_SZ, (const BYTE*)v.c_str(), (DWORD)((v.size() + 1) * sizeof(wchar_t))); }

// the user's VLC_PLUGIN_PATH (HKCU\Environment) as a list of folders; written back with the type it had (REG_SZ when new)
std::vector<std::wstring> readPluginPath(DWORD& type) {
    std::vector<std::wstring> out; type = REG_SZ; DWORD n = 0, t = 0;   // t: a failed read may leave anything in it
    if (RegGetValueW(HKEY_CURRENT_USER, L"Environment", L"VLC_PLUGIN_PATH", RRF_RT_REG_SZ | RRF_RT_REG_EXPAND_SZ | RRF_NOEXPAND, &t, nullptr, &n) != ERROR_SUCCESS) return out;
    std::wstring v(n / sizeof(wchar_t) + 1, L'\0');
    if (RegGetValueW(HKEY_CURRENT_USER, L"Environment", L"VLC_PLUGIN_PATH", RRF_RT_REG_SZ | RRF_RT_REG_EXPAND_SZ | RRF_NOEXPAND, &t, &v[0], &n) != ERROR_SUCCESS) return out;
    if (t == REG_EXPAND_SZ) type = REG_EXPAND_SZ;
    v.resize(wcslen(v.c_str())); size_t at = 0;
    while (at <= v.size()) { size_t e = v.find(L';', at); if (e == std::wstring::npos) e = v.size(); if (e > at) out.push_back(v.substr(at, e - at)); at = e + 1; }
    return out; }
void writePluginPath(const std::vector<std::wstring>& list, DWORD type) {
    HKEY k; if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Environment", 0, KEY_SET_VALUE, &k) != ERROR_SUCCESS) return;
    if (list.empty()) RegDeleteValueW(k, L"VLC_PLUGIN_PATH");
    else { std::wstring v; for (size_t i = 0; i < list.size(); i++) v += (i ? L";" : L"") + list[i];
        RegSetValueExW(k, L"VLC_PLUGIN_PATH", 0, type, (const BYTE*)v.c_str(), (DWORD)((v.size() + 1) * sizeof(wchar_t))); }
    RegCloseKey(k);
    DWORD_PTR r; SendMessageTimeoutW(HWND_BROADCAST, WM_SETTINGCHANGE, 0, (LPARAM)L"Environment", SMTO_ABORTIFHUNG, 3000, &r); }   // Explorer picks up the change

int install() {
    if (!ask(T(L"Install the LAE decoder for VLC 3.0 (64-bit) for the current user?", L"为当前用户安装 VLC 3.0（64 位）的 LAE 解码器？"))) return 1;
    const std::wstring dir = installDir(); SHCreateDirectoryExW(nullptr, dir.c_str(), nullptr);
    const std::wstring dll = dir + L"\\liblae_plugin.dll", self = dir + L"\\lae_vlc_setup.exe";
    HRSRC r = FindResourceW(nullptr, MAKEINTRESOURCEW(IDR_MODULE), RT_RCDATA); HGLOBAL g = r ? LoadResource(nullptr, r) : nullptr; const DWORD n = r ? SizeofResource(nullptr, r) : 0;
    if (!g || !n) { say(T(L"Internal error: the module is missing from the installer.", L"内部错误：安装程序中缺少模块。"), MB_ICONERROR); return 2; }
    HANDLE h = CreateFileW(dll.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) { say(T(L"The module is in use. Close VLC and run the installer again.", L"模块正在使用。请关闭 VLC 后重新运行安装程序。"), MB_ICONWARNING); return 3; }
    DWORD w = 0; const BOOL ok = WriteFile(h, LockResource(g), n, &w, nullptr) && w == n; CloseHandle(h); if (!ok) { say(T(L"Cannot write ", L"无法写入 ") + dll, MB_ICONERROR); return 4; }
    wchar_t me[MAX_PATH]; GetModuleFileNameW(nullptr, me, MAX_PATH); if (_wcsicmp(me, self.c_str()) != 0) CopyFileW(me, self.c_str(), FALSE);
    DWORD type; std::vector<std::wstring> list = readPluginPath(type); bool have = false; for (const auto& e : list) if (!_wcsicmp(e.c_str(), dir.c_str())) have = true;
    if (!have) { list.push_back(dir); writePluginPath(list, type); }
    HKEY k; if (RegCreateKeyExW(HKEY_CURRENT_USER, kUninstKey, 0, nullptr, 0, KEY_WRITE, nullptr, &k, nullptr) == ERROR_SUCCESS) {
        setStr(k, L"DisplayName", T(L"LAE decoder for VLC", L"LAE 解码器（VLC）")); setStr(k, L"DisplayVersion", kVersion); setStr(k, L"Publisher", L"mtq0605");
        setStr(k, L"InstallLocation", dir); setStr(k, L"UninstallString", L"\"" + self + L"\" /uninstall"); setStr(k, L"QuietUninstallString", L"\"" + self + L"\" /uninstall /quiet");
        const DWORD one = 1; RegSetValueExW(k, L"NoModify", 0, REG_DWORD, (const BYTE*)&one, 4); RegSetValueExW(k, L"NoRepair", 0, REG_DWORD, (const BYTE*)&one, 4); RegCloseKey(k); }
    say(T(L"Installed. Restart VLC if it is running.\n"
          L"Settings: Tools > Preferences > Show settings: All > Input / Codecs > Demuxers > LAE.\n\nInstalled in: ",
          L"已安装。如 VLC 正在运行，请重新启动。\n"
          L"设置：工具 > 偏好设置 > 显示设置：全部 > 输入/编解码器 > 分流器 > LAE。\n\n安装位置：") + dir);
    return 0; }

int uninstall() {
    if (!ask(T(L"Remove the LAE decoder for VLC?", L"卸载 VLC 的 LAE 解码器？"))) return 1;
    const std::wstring dir = installDir(), dll = dir + L"\\liblae_plugin.dll", self = dir + L"\\lae_vlc_setup.exe";
    if (!DeleteFileW(dll.c_str()) && GetFileAttributesW(dll.c_str()) != INVALID_FILE_ATTRIBUTES) {
        say(T(L"The module is in use. Close VLC and run the uninstaller again.", L"模块正在使用。请关闭 VLC 后重新卸载。"), MB_ICONWARNING); return 3; }
    DWORD type; std::vector<std::wstring> list = readPluginPath(type), keep; for (const auto& e : list) if (_wcsicmp(e.c_str(), dir.c_str()) != 0) keep.push_back(e);
    if (keep.size() != list.size()) writePluginPath(keep, type);
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
