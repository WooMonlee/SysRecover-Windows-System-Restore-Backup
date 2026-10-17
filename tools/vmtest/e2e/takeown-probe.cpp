/* takeown-probe.cpp - host-side probe for the swapleft.cpp TakeOwnershipAndGrant
 * LocalFree(interior DACL) hypothesis.  ASCII only.
 *
 * usage: takeown-probe.exe buggy|fixed <dir>
 *
 * buggy = replicate src/app/swapleft.cpp:79-125 exactly:
 *     GetNamedSecurityInfoW(..., ppSecurityDescriptor = NULL, ..., &oldDacl, ...)
 *     ... LocalFree(oldDacl)            <-- interior pointer free
 * fixed = same but keep the security descriptor and LocalFree(sd).
 *
 * Every step is flushed to takeown-probe.log so that a fastfail-style hard
 * process death still leaves the last reached marker on disk.
 */
#include <windows.h>
#include <aclapi.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

static const wchar_t* LOGP = L"D:\\Prog\\_Project\\SysRecover\\tools\\vmtest\\e2e\\takeown-probe.log";

static void mark(const char* s) {
    FILE* f = _wfopen(LOGP, L"a");
    if (f) { fputs(s, f); fputc('\n', f); fflush(f); fclose(f); }
}

static void EnablePriv(const wchar_t* name) {
    HANDLE tok = NULL;
    if (!OpenProcessToken(GetCurrentProcess(),
                          TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &tok))
        return;
    TOKEN_PRIVILEGES tp = {0};
    tp.PrivilegeCount = 1;
    if (LookupPrivilegeValueW(NULL, name, &tp.Privileges[0].Luid)) {
        tp.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
        AdjustTokenPrivileges(tok, FALSE, &tp, sizeof(tp), NULL, NULL);
    }
    CloseHandle(tok);
}

static void m2(const char* tag, const wchar_t* path) {
    char buf[512];
    _snprintf_s(buf, sizeof(buf), _TRUNCATE, "%s: %ls", tag, path);
    mark(buf);
}

int wmain(int argc, wchar_t** argv) {
    mark("---- probe run ----");
    if (argc < 3) { mark("usage: buggy|fixed <dir>"); return 2; }
    int buggy = (wcscmp(argv[1], L"buggy") == 0);
    const wchar_t* path = argv[2];
    if (!buggy && wcscmp(argv[1], L"fixed") != 0) { mark("bad mode"); return 2; }

    m2(buggy ? "mode=buggy" : "mode=fixed", path);
    if (!CreateDirectoryW(path, NULL)) {
        DWORD e = GetLastError();
        char b[128]; _snprintf_s(b, sizeof(b), _TRUNCATE, "CreateDirectory err=%lu (exists ok)", e);
        mark(b);
    }

    EnablePriv(L"SeTakeOwnershipPrivilege");
    EnablePriv(L"SeBackupPrivilege");
    EnablePriv(L"SeRestorePrivilege");

    BYTE adminSidBuf[SECURITY_MAX_SID_SIZE] = {0};
    DWORD sidSize = sizeof(adminSidBuf);
    PSID adminSid = (PSID)adminSidBuf;
    if (!CreateWellKnownSid(WinBuiltinAdministratorsSid, NULL, adminSid, &sidSize)) {
        mark("CreateWellKnownSid FAILED"); return 3;
    }
    mark("sid ok");

    DWORD r = SetNamedSecurityInfoW((LPWSTR)path, SE_FILE_OBJECT,
                                    OWNER_SECURITY_INFORMATION, adminSid, NULL,
                                    NULL, NULL);
    { char b[128]; _snprintf_s(b, sizeof(b), _TRUNCATE, "set-owner rc=%lu", r); mark(b); }

    EXPLICIT_ACCESSW ea = {0};
    ea.grfAccessPermissions = GENERIC_ALL;
    ea.grfAccessMode = SET_ACCESS;
    ea.grfInheritance = SUB_CONTAINERS_AND_OBJECTS_INHERIT;
    ea.Trustee.TrusteeForm = TRUSTEE_IS_SID;
    ea.Trustee.TrusteeType = TRUSTEE_IS_WELL_KNOWN_GROUP;
    ea.Trustee.ptstrName = (LPWSTR)adminSid;

    if (buggy) {
        PACL oldDacl = NULL;
        DWORD gr = GetNamedSecurityInfoW((LPWSTR)path, SE_FILE_OBJECT,
                                         DACL_SECURITY_INFORMATION, NULL, NULL,
                                         &oldDacl, NULL, NULL);
        { char b[160]; _snprintf_s(b, sizeof(b), _TRUNCATE,
            "get-dacl(sd=NULL) rc=%lu oldDacl=%p", gr, (void*)oldDacl); mark(b); }

        PACL newDacl = NULL;
        DWORD sr = SetEntriesInAclW(1, &ea, oldDacl, &newDacl);
        { char b[160]; _snprintf_s(b, sizeof(b), _TRUNCATE,
            "SetEntriesInAcl rc=%lu newDacl=%p", sr, (void*)newDacl); mark(b); }

        if (newDacl) {
            DWORD r2 = SetNamedSecurityInfoW((LPWSTR)path, SE_FILE_OBJECT,
                                             DACL_SECURITY_INFORMATION, NULL, NULL,
                                             newDacl, NULL);
            { char b[128]; _snprintf_s(b, sizeof(b), _TRUNCATE, "set-dacl rc=%lu", r2); mark(b); }
            mark("before LocalFree(newDacl)");
            LocalFree(newDacl);
            mark("after  LocalFree(newDacl)");
        }
        mark("before LocalFree(oldDacl)   <-- expected death point (interior ptr)");
        if (oldDacl) LocalFree(oldDacl);
        mark("after  LocalFree(oldDacl)");
        mark("done (buggy survived)");
        return 0;
    }

    /* fixed: keep the security descriptor returned by GetNamedSecurityInfoW
     * and free *it* (the DACL pointer is interior to it). */
    PACL oldDacl = NULL;
    PSECURITY_DESCRIPTOR sd = NULL;
    DWORD gr = GetNamedSecurityInfoW((LPWSTR)path, SE_FILE_OBJECT,
                                     DACL_SECURITY_INFORMATION, NULL, NULL,
                                     &oldDacl, NULL, &sd);
    { char b[160]; _snprintf_s(b, sizeof(b), _TRUNCATE,
        "get-dacl(sd!=NULL) rc=%lu oldDacl=%p sd=%p", gr, (void*)oldDacl, sd); mark(b); }

    PACL newDacl = NULL;
    DWORD sr = SetEntriesInAclW(1, &ea, oldDacl, &newDacl);
    { char b[160]; _snprintf_s(b, sizeof(b), _TRUNCATE,
        "SetEntriesInAcl rc=%lu newDacl=%p", sr, (void*)newDacl); mark(b); }

    if (newDacl) {
        DWORD r2 = SetNamedSecurityInfoW((LPWSTR)path, SE_FILE_OBJECT,
                                         DACL_SECURITY_INFORMATION, NULL, NULL,
                                         newDacl, NULL);
        { char b[128]; _snprintf_s(b, sizeof(b), _TRUNCATE, "set-dacl rc=%lu", r2); mark(b); }
        LocalFree(newDacl);
        mark("freed newDacl");
    }
    if (sd) { LocalFree(sd); mark("freed security descriptor"); }
    mark("done (fixed)");
    return 0;
}
