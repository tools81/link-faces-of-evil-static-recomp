/* Portable install support.

   Everything lives next to the executable: linkfoe.ini (disc image location
   and update settings) plus the saves\ folder.  Nothing is written to
   AppData or the registry.

   linkfoe.ini:
     [game]   rom=<path to the .cue or .bin>   (relative paths are relative
                                                to the program folder)
     [update] mode=ask|auto|off                (default ask)
              repo=<owner>/<name>              (GitHub repository to check)
              skip=<tag>                       (a release the user declined)

   Update check: on start the latest GitHub release of the repository is
   read; if its tag (vX.Y.Z) is newer than this build and it has a
   "*-portable.zip" asset, the zip is downloaded to update\ and a small
   PowerShell script replaces the program files once this process has
   exited, keeping saves\, rom\ and linkfoe.ini, then restarts the game. */
#include "runtime.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef LINKFOE_VERSION
#define LINKFOE_VERSION "0.0.0"
#endif
#ifndef LINKFOE_UPDATE_REPO
#define LINKFOE_UPDATE_REPO ""
#endif

const char *portable_version(void) { return LINKFOE_VERSION; }

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <commdlg.h>
#include <winhttp.h>

#define APP_TITLE "Link: The Faces of Evil"

static char base_dir[512] = ".", ini_path[600];

void portable_init(const char *exe_dir) {
    snprintf(base_dir, sizeof base_dir, "%s", exe_dir);
    snprintf(ini_path, sizeof ini_path, "%s\\linkfoe.ini", exe_dir);
}

static void ini_get(const char *sec, const char *key, const char *def, char *out, DWORD n) {
    GetPrivateProfileStringA(sec, key, def, out, n, ini_path);
}

static void ini_set(const char *sec, const char *key, const char *val) {
    WritePrivateProfileStringA(sec, key, val, ini_path);
}

/* ---------------------------------------------------------------- disc image */
int portable_rom_path(char *out, size_t n) {
    char v[1024];
    ini_get("game", "rom", "", v, sizeof v);
    if (!v[0]) return -1;
    int absolute = (v[0] && v[1] == ':') || v[0] == '\\' || v[0] == '/';
    if (absolute) snprintf(out, n, "%s", v);
    else snprintf(out, n, "%s\\%s", base_dir, v);
    return 0;
}

void portable_save_rom(const char *path) {
    size_t L = strlen(base_dir);
    if (!_strnicmp(path, base_dir, L) && (path[L] == '\\' || path[L] == '/'))
        path += L + 1;                 /* inside the program folder: keep it movable */
    ini_set("game", "rom", path);
}

int portable_pick_rom(char *out, size_t n) {
    char file[1024] = "";
    OPENFILENAMEA ofn;
    memset(&ofn, 0, sizeof ofn);
    ofn.lStructSize = sizeof ofn;
    ofn.lpstrFilter = "CD-i disc image (*.cue, *.bin)\0*.cue;*.bin\0All files\0*.*\0";
    ofn.lpstrFile = file;
    ofn.nMaxFile = sizeof file;
    ofn.lpstrInitialDir = base_dir;
    ofn.lpstrTitle = "Locate the Link: The Faces of Evil disc image (.cue or .bin)";
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR | OFN_HIDEREADONLY;
    if (!GetOpenFileNameA(&ofn)) return -1;
    snprintf(out, n, "%s", file);
    return 0;
}

/* ---------------------------------------------------------------- update check */
static char *http_get(const char *url, size_t maxlen, size_t *outlen, int recv_timeout_ms) {
    wchar_t wurl[2048], host[256], path[2048];
    if (!MultiByteToWideChar(CP_UTF8, 0, url, -1, wurl, 2048)) return NULL;
    URL_COMPONENTS uc;
    memset(&uc, 0, sizeof uc);
    uc.dwStructSize = sizeof uc;
    uc.lpszHostName = host; uc.dwHostNameLength = 256;
    uc.lpszUrlPath = path; uc.dwUrlPathLength = 2048;   /* includes the query string */
    if (!WinHttpCrackUrl(wurl, 0, 0, &uc)) return NULL;

    char *body = NULL;
    size_t len = 0;
    int ok = 0;
    HINTERNET s = WinHttpOpen(L"linkfoe-recomp-updater/" LINKFOE_VERSION, WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                              WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    HINTERNET c = s ? WinHttpConnect(s, host, uc.nPort, 0) : NULL;
    HINTERNET r = c ? WinHttpOpenRequest(c, L"GET", path, NULL, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                                         uc.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0) : NULL;
    if (r) {
        WinHttpSetTimeouts(s, 3000, 3000, 3000, recv_timeout_ms);
        DWORD st = 0, sz = sizeof st;
        if (WinHttpSendRequest(r, L"Accept: application/vnd.github+json, application/octet-stream\r\n", (DWORD)-1L,
                               WINHTTP_NO_REQUEST_DATA, 0, 0, 0) &&
            WinHttpReceiveResponse(r, NULL) &&
            WinHttpQueryHeaders(r, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX,
                                &st, &sz, WINHTTP_NO_HEADER_INDEX) && st == 200) {
            ok = 1;
            for (;;) {
                DWORD avail = 0, got = 0;
                if (!WinHttpQueryDataAvailable(r, &avail)) { ok = 0; break; }
                if (!avail) break;
                if (len + avail > maxlen) { ok = 0; break; }
                char *nb = (char *)realloc(body, len + avail + 1);
                if (!nb) { ok = 0; break; }
                body = nb;
                if (!WinHttpReadData(r, body + len, avail, &got)) { ok = 0; break; }
                len += got;
            }
        }
    }
    if (r) WinHttpCloseHandle(r);
    if (c) WinHttpCloseHandle(c);
    if (s) WinHttpCloseHandle(s);
    if (!ok || !body) { free(body); return NULL; }
    body[len] = 0;
    if (outlen) *outlen = len;
    return body;
}

/* the string value following "key": in a JSON text, starting at p */
static const char *json_str(const char *p, const char *key, char *out, size_t n) {
    char pat[64];
    snprintf(pat, sizeof pat, "\"%s\"", key);
    const char *k = strstr(p, pat);
    if (!k) return NULL;
    const char *q = strchr(k + strlen(pat), '"');
    if (!q) return NULL;
    size_t i = 0;
    for (q++; *q && *q != '"' && i + 1 < n; q++) {
        if (*q == '\\' && q[1]) q++;
        out[i++] = *q;
    }
    out[i] = 0;
    return q;
}

static void parse_ver(const char *s, int v[3]) {
    v[0] = v[1] = v[2] = 0;
    while (*s && (*s < '0' || *s > '9')) s++;
    sscanf(s, "%d.%d.%d", &v[0], &v[1], &v[2]);
}

static int newer(const char *tag, const char *cur) {
    int a[3], b[3];
    parse_ver(tag, a);
    parse_ver(cur, b);
    for (int i = 0; i < 3; i++) if (a[i] != b[i]) return a[i] > b[i];
    return 0;
}

static const char *update_script =
    "param([string]$Zip, [string]$Dir, [int]$ProcessId, [string]$Relaunch)\r\n"
    "$ErrorActionPreference = 'Stop'\r\n"
    "try {\r\n"
    "  try { Wait-Process -Id $ProcessId -Timeout 30 } catch {}\r\n"
    "  $staged = Join-Path $Dir 'update\\staged'\r\n"
    "  if (Test-Path -LiteralPath $staged) { Remove-Item -LiteralPath $staged -Recurse -Force }\r\n"
    "  Expand-Archive -LiteralPath $Zip -DestinationPath $staged -Force\r\n"
    "  $exe = Get-ChildItem -LiteralPath $staged -Recurse -Filter 'linkfoe.exe' | Select-Object -First 1\r\n"
    "  if (-not $exe) { throw 'The update package does not contain linkfoe.exe.' }\r\n"
    "  $keep = @('saves', 'rom', 'linkfoe.ini', 'update')\r\n"
    "  Get-ChildItem -LiteralPath $exe.DirectoryName | Where-Object { $keep -notcontains $_.Name } | ForEach-Object {\r\n"
    "    Copy-Item -LiteralPath $_.FullName -Destination $Dir -Recurse -Force\r\n"
    "  }\r\n"
    "} catch {\r\n"
    "  Add-Type -AssemblyName PresentationFramework\r\n"
    "  [System.Windows.MessageBox]::Show(\"The update could not be installed:`n`n\" + $_.Exception.Message, "
    "'" APP_TITLE "') | Out-Null\r\n"
    "}\r\n"
    "Remove-Item -LiteralPath (Join-Path $Dir 'update') -Recurse -Force -ErrorAction SilentlyContinue\r\n"
    "Start-Process -FilePath $Relaunch -WorkingDirectory $Dir\r\n";

static int write_file(const char *path, const void *p, size_t n) {
    FILE *f = fopen(path, "wb");
    int ok = f && fwrite(p, 1, n, f) == n;
    if (f && fclose(f)) ok = 0;
    return ok;
}

static void install_update(const char *tag, const char *zip_url) {
    char dir[600], zip[700], ps1[700], exe[600], cmd[3000];
    size_t n = 0;
    snprintf(dir, sizeof dir, "%s\\update", base_dir);
    CreateDirectoryA(dir, NULL);
    snprintf(zip, sizeof zip, "%s\\package.zip", dir);
    snprintf(ps1, sizeof ps1, "%s\\apply_update.ps1", dir);
    char *data = http_get(zip_url, 256u << 20, &n, 60000);
    if (!data || !write_file(zip, data, n) || !write_file(ps1, update_script, strlen(update_script))) {
        free(data);
        RemoveDirectoryA(dir);
        char m[900];
        snprintf(m, sizeof m, "The update to %s could not be downloaded or saved in\n%s\n\n"
                 "The game will start normally.", tag, base_dir);
        MessageBoxA(NULL, m, APP_TITLE, MB_ICONWARNING);
        return;
    }
    free(data);
    GetModuleFileNameA(NULL, exe, sizeof exe);
    snprintf(cmd, sizeof cmd,
             "powershell.exe -NoProfile -ExecutionPolicy Bypass -File \"%s\" -Zip \"%s\" -Dir \"%s\" "
             "-ProcessId %lu -Relaunch \"%s\"",
             ps1, zip, base_dir, (unsigned long)GetCurrentProcessId(), exe);
    STARTUPINFOA si;
    PROCESS_INFORMATION pi;
    memset(&si, 0, sizeof si);
    si.cb = sizeof si;
    if (!CreateProcessA(NULL, cmd, NULL, NULL, FALSE, CREATE_NO_WINDOW, NULL, base_dir, &si, &pi)) {
        MessageBoxA(NULL, "The updater could not be started (PowerShell is required).\n\nThe game will start normally.",
                    APP_TITLE, MB_ICONWARNING);
        return;
    }
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    LOG("update %s downloaded; restarting into the updater", tag);
    exit(0);
}

void portable_check_update(void) {
    char mode[16], repo[200], skip[64], url[400], tag[64] = "", zip_url[1024] = "";
    ini_get("update", "mode", "ask", mode, sizeof mode);
    ini_get("update", "repo", LINKFOE_UPDATE_REPO, repo, sizeof repo);
    ini_get("update", "skip", "", skip, sizeof skip);
    if (!_stricmp(mode, "off") || !repo[0]) return;

    snprintf(url, sizeof url, "https://api.github.com/repos/%s/releases/latest", repo);
    char *json = http_get(url, 4u << 20, NULL, 5000);
    if (!json) { LOG("update check: no release information (offline or no releases)"); return; }
    if (json_str(json, "tag_name", tag, sizeof tag)) {
        const char *p = json;
        char u[1024];
        while ((p = json_str(p, "browser_download_url", u, sizeof u)) != NULL) {
            size_t L = strlen(u);
            if (L > 13 && !_stricmp(u + L - 13, "-portable.zip")) { snprintf(zip_url, sizeof zip_url, "%s", u); break; }
        }
    }
    free(json);
    LOG("update check: this build v%s, latest release %s", LINKFOE_VERSION, tag[0] ? tag : "?");
    if (!tag[0] || !zip_url[0] || !newer(tag, LINKFOE_VERSION) || !_stricmp(tag, skip)) return;

    if (_stricmp(mode, "auto")) {
        char m[600];
        snprintf(m, sizeof m,
                 "A new version is available.\n\n"
                 "    Installed:  v%s\n    Available:  %s\n\n"
                 "Download and install it now? Your saves, settings and disc image are kept.\n\n"
                 "Yes: update now     No: not now     Cancel: skip this version",
                 LINKFOE_VERSION, tag);
        int a = MessageBoxA(NULL, m, APP_TITLE " - update", MB_YESNOCANCEL | MB_ICONINFORMATION);
        if (a == IDCANCEL) ini_set("update", "skip", tag);
        if (a != IDYES) return;
    }
    install_update(tag, zip_url);
}

#else
void portable_init(const char *exe_dir) { (void)exe_dir; }
int portable_rom_path(char *out, size_t n) { (void)out; (void)n; return -1; }
void portable_save_rom(const char *path) { (void)path; }
int portable_pick_rom(char *out, size_t n) { (void)out; (void)n; return -1; }
void portable_check_update(void) {}
#endif
