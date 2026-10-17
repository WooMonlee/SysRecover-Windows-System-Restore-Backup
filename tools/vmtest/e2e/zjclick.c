/* zjclick.c - e2e automation helper for the S5 leg (leftover-dialog DELETE).
 * Native console exe (no PowerShell / no Add-Type): works on lite Windows
 * images where Add-Type cannot find System.dll.
 *
 * default mode: claim -> launch GUI -> click SysRecoverUI.Confirm right button
 *               -> dismiss #32770 -> wait for C:\~old to disappear
 * --probe <class> : list top-level windows of that class, exit 0
 *
 * exit codes: 0 done, 2 env/exe missing, 3 dialog never appeared, 4 ~old left
 */
#include <windows.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>

#define CLAIM_PATH "C:\\zjgui\\auto-b.claimed"
#define GUI_PATH   "C:\\Program Files\\SysRecover\\SysRecoverUI.exe"
#define OLD_DIR    "C:\\~old"
#define CFM_CLASS  "SysRecoverUI.Confirm"
#define MAIN_CLASS "SysRecoverUI"
#define DLG_CLASS  "#32770"
#define CLICK_X 340
#define CLICK_Y 161

static HANDLE g_log[4];
static int    g_nlog = 0;
static DWORD  g_t0;

static void logf(const char *fmt, ...)
{
    char buf[1024];
    char line[1200];
    va_list ap;
    SYSTEMTIME st;
    int n, i;
    DWORD el = (GetTickCount() - g_t0) / 1000;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    GetLocalTime(&st);
    n = snprintf(line, sizeof(line), "[%02d:%02d:%02d t=%lus] %s\r\n",
                 st.wHour, st.wMinute, st.wSecond, (unsigned long)el, buf);
    for (i = 0; i < g_nlog; i++) {
        DWORD w = 0;
        if (g_log[i] == INVALID_HANDLE_VALUE) continue;
        WriteFile(g_log[i], line, (DWORD)n, &w, NULL);
        FlushFileBuffers(g_log[i]);
    }
    fputs(line, stdout);
    fflush(stdout);
}

static void log_open(const char *path)
{
    HANDLE h;
    if (g_nlog >= 4) return;
    h = CreateFileA(path, FILE_APPEND_DATA, FILE_SHARE_READ, NULL,
                    OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return;
    SetFilePointer(h, 0, NULL, FILE_END);
    g_log[g_nlog++] = h;
}

static void log_open_all(void)
{
    char p[8];
    int c;
    log_open("C:\\zjgui\\auto-b.log");
    for (c = 'D'; c <= 'Z'; c++) {
        char probe[32];
        DWORD t;
        sprintf(p, "%c:\\", c);
        t = GetDriveTypeA(p);
        if (t != DRIVE_FIXED && t != DRIVE_REMOVABLE) continue;
        sprintf(probe, "%c:\\images\\c.esd", c);
        if (GetFileAttributesA(probe) == INVALID_FILE_ATTRIBUTES) continue;
        sprintf(probe, "%c:\\auto-b.log", c);
        log_open(probe);
        break;
    }
}

/* --- window enumeration ------------------------------------------------ */
typedef struct { HWND h; char cls[64]; RECT rc; } WINREC;
static WINREC g_win[256];
static int    g_nwin;

static BOOL CALLBACK enum_cb(HWND h, LPARAM lp)
{
    char cls[64] = {0};
    (void)lp;
    if (g_nwin >= 256) return FALSE;
    if (!IsWindowVisible(h)) return TRUE;
    GetClassNameA(h, cls, sizeof(cls) - 1);
    g_win[g_nwin].h = h;
    strncpy(g_win[g_nwin].cls, cls, sizeof(g_win[g_nwin].cls) - 1);
    GetWindowRect(h, &g_win[g_nwin].rc);
    g_nwin++;
    return TRUE;
}

static void scan(void)
{
    g_nwin = 0;
    EnumWindows(enum_cb, 0);
}

static HWND find_cls(const char *cls)
{
    int i;
    for (i = 0; i < g_nwin; i++)
        if (strcmp(g_win[i].cls, cls) == 0) return g_win[i].h;
    return NULL;
}

static int count_cls(const char *cls)
{
    int i, n = 0;
    for (i = 0; i < g_nwin; i++)
        if (strcmp(g_win[i].cls, cls) == 0) n++;
    return n;
}

static void click_client(HWND h, int x, int y)
{
    RECT rc;
    LPARAM lp;
    DWORD_PTR res = 0;
    GetClientRect(h, &rc);
    if (x >= rc.right)   x = rc.right - 2;
    if (y >= rc.bottom)  y = rc.bottom - 2;
    if (x < 0) x = 0;
    if (y < 0) y = 0;
    lp = MAKELPARAM(x, y);
    SendMessageTimeoutA(h, WM_MOUSEMOVE,   0, lp, SMTO_ABORTIFHUNG, 5000, &res);
    Sleep(40);
    SendMessageTimeoutA(h, WM_LBUTTONDOWN, 1, lp, SMTO_ABORTIFHUNG, 5000, &res);
    Sleep(120);
    SendMessageTimeoutA(h, WM_LBUTTONUP,   0, lp, SMTO_ABORTIFHUNG, 5000, &res);
}

static int probe_mode(const char *cls)
{
    int i;
    scan();
    printf("windows=%d\n", g_nwin);
    for (i = 0; i < g_nwin; i++) {
        int hit = (cls && cls[0]) ? (strcmp(g_win[i].cls, cls) == 0) : 1;
        char title[128] = {0};
        GetWindowTextA(g_win[i].h, title, sizeof(title) - 1);
        printf("%s%s hwnd=%p rect=%ld,%ld,%ld,%ld title=%s\n",
               hit ? "HIT " : "", g_win[i].cls, (void *)g_win[i].h,
               g_win[i].rc.left, g_win[i].rc.top,
               g_win[i].rc.right, g_win[i].rc.bottom, title);
    }
    return 0;
}

int main(int argc, char **argv)
{
    int secs = 3600;
    int no_launch = 0;
    int i;
    int clicked = 0, dismissed = 0, launched = 0, saw_confirm = 0;
    DWORD clicks[4];
    int nclicks = 0;
    DWORD last_tick = 0;
    HANDLE claim;
    DWORD dlg_deadline = 0;

    g_t0 = GetTickCount();
    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--probe") == 0 && i + 1 < argc) return probe_mode(argv[++i]);
        if (strcmp(argv[i], "--secs") == 0 && i + 1 < argc) secs = atoi(argv[++i]);
        if (strcmp(argv[i], "--no-launch") == 0) no_launch = 1;
    }

    if (!no_launch && GetFileAttributesA("C:\\zjgui") == INVALID_FILE_ATTRIBUTES) {
        printf("missing C:\\zjgui\n");
        return 2;
    }
    log_open_all();
    if (g_nlog == 0) { printf("no log target\n"); return 2; }

    if (GetFileAttributesA(GUI_PATH) == INVALID_FILE_ATTRIBUTES) {
        logf("FAIL: gui exe missing (%s)", GUI_PATH);
        return 2;
    }

    claim = CreateFileA(CLAIM_PATH, GENERIC_WRITE, 0, NULL, CREATE_NEW,
                        FILE_ATTRIBUTE_NORMAL, NULL);
    if (claim == INVALID_HANDLE_VALUE) {
        logf("claim busy (err=%lu) - another instance active; exit 0",
             (unsigned long)GetLastError());
        return 0;
    }
    logf("claim OK (%s) pid=%lu", CLAIM_PATH, (unsigned long)GetCurrentProcessId());

    for (;;) {
        DWORD el = (GetTickCount() - g_t0) / 1000;
        HWND cfm;
        scan();

        cfm = find_cls(CFM_CLASS);
        if (cfm) {
            if (!saw_confirm) {
                RECT rc;
                char title[128] = {0};
                GetClientRect(cfm, &rc);
                GetWindowTextA(cfm, title, sizeof(title) - 1);
                logf("confirm hwnd=%p client=%ldx%ld title=%s",
                     (void *)cfm, rc.right, rc.bottom, title);
                saw_confirm = 1;
            }
            if (nclicks < 4 && el >= (nclicks ? clicks[nclicks - 1] / 1000 + 20 : 0)) {
                click_client(cfm, CLICK_X, CLICK_Y);
                clicks[nclicks++] = GetTickCount();
                clicked = 1;
                logf("clicked DELETE (right btn) at client %d,%d (try %d)",
                     CLICK_X, CLICK_Y, nclicks);
            }
        }

        if (clicked && !dismissed) {
            HWND dlg = find_cls(DLG_CLASS);
            if (dlg) {
                DWORD_PTR res = 0;
                if (!dlg_deadline) dlg_deadline = GetTickCount() + 120000;
                SendMessageTimeoutA(dlg, WM_COMMAND, 1 /*IDOK*/, 0,
                                    SMTO_ABORTIFHUNG, 5000, &res);
                Sleep(300);
                if (IsWindow(dlg)) {
                    SendMessageTimeoutA(dlg, WM_CLOSE, 0, 0,
                                        SMTO_ABORTIFHUNG, 5000, &res);
                    Sleep(200);
                }
                logf("dismissed MessageBox (%s)", DLG_CLASS);
                dismissed = 1;
            } else if (dlg_deadline && GetTickCount() > dlg_deadline) {
                dismissed = 1;
                logf("no MessageBox seen within 120s - continue");
            }
        }

        if (GetFileAttributesA(OLD_DIR) == INVALID_FILE_ATTRIBUTES) {
            logf("SUCCESS: %s is gone (clicked=%d dismissed=%d)", OLD_DIR, clicked, dismissed);
            return 0;
        }

        if (!launched && !no_launch && !cfm && count_cls(MAIN_CLASS) == 0 && el < 5) {
            /* wait; GUI may still be starting */
        }
        if (!launched && !no_launch && el >= 5) {
            if (count_cls(MAIN_CLASS) == 0 && count_cls(CFM_CLASS) == 0) {
                STARTUPINFOA si;
                PROCESS_INFORMATION pi;
                ZeroMemory(&si, sizeof(si));
                si.cb = sizeof(si);
                ZeroMemory(&pi, sizeof(pi));
                launched = 1;
                if (CreateProcessA(GUI_PATH, NULL, NULL, NULL, FALSE,
                                   CREATE_NEW_PROCESS_GROUP, NULL, NULL, &si, &pi)) {
                    logf("launched gui pid=%lu", (unsigned long)pi.dwProcessId);
                    CloseHandle(pi.hThread);
                    CloseHandle(pi.hProcess);
                } else {
                    logf("FAIL: CreateProcess(%s) err=%lu", GUI_PATH,
                         (unsigned long)GetLastError());
                    return 2;
                }
            } else {
                launched = 1;
                logf("gui already running");
            }
        }

        if (launched && !saw_confirm && el > 300) {
            logf("FAIL: confirm dialog never appeared within 300s");
            return 3;
        }
        if (el >= (DWORD)secs) {
            logf("FAIL: timeout %ds (~old present=%d clicked=%d dismissed=%d)",
                 secs, GetFileAttributesA(OLD_DIR) != INVALID_FILE_ATTRIBUTES,
                 clicked, dismissed);
            return 4;
        }
        if (el >= last_tick + 30) {
            last_tick = el;
            logf("tick t=%lus windows=%d confirm=%d main=%d old=%d clicked=%d dismissed=%d",
                 (unsigned long)el, g_nwin, count_cls(CFM_CLASS),
                 count_cls(MAIN_CLASS), 1, clicked, dismissed);
        }
        Sleep(2000);
    }
}
