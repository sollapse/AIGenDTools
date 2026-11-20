/*
 * Tests of how RetroRun reads an autorun.inf and finds the program it names: the sections and their values,
 * which entry names the program, how a command line is split into its file and parameters on a disk, and the
 * kinds of floppy drives by their models. Nothing is started; a temporary folder stands in for a disk's root.
 */
#include "../src/drives.h"
#include "../src/inf.h"
#include "../src/launch.h"
#include <stdio.h>
#include <wchar.h>

namespace {

int g_checks;
int g_failures;

void
Check(const bool ok, const wchar_t *what, const std::wstring &got = L"")
{
    g_checks++;
    if (!ok) {
        g_failures++;
        wprintf(L"FAIL: %ls: got '%ls'\n", what, got.c_str());
    }
}

/* The keys as x64 Windows reads them. */
Pairs
Keys(const wchar_t *text)
{
    return AutorunKeys(text, L"autorun.amd64");
}

bool
WriteBytes(const std::wstring &path, const void *data, const DWORD size)
{
    const HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    DWORD        wrote = 0;

    if (h == INVALID_HANDLE_VALUE)
        return false;
    const bool ok = (size == 0) || (WriteFile(h, data, size, &wrote, nullptr) && (wrote == size));
    CloseHandle(h);
    return ok;
}

/* The entry split on the disk, against the file (matched without case, as the disk's names are) and the
   parameters it should give. */
void
Split(const std::wstring &root, const std::wstring &entry, const std::wstring &file, const std::wstring &params)
{
    std::wstring f;
    std::wstring p;

    SplitCommand(root, entry, &f, &p);
    Check((_wcsicmp(f.c_str(), file.c_str()) == 0) && (p == params), entry.c_str(), f + L" | " + p);
}

} /* namespace */

int
wmain()
{
    /* Sections and values. */
    {
        const Pairs p = Keys(L"; a comment\r\n[AutoRun]\r\nopen = setup.exe /s \r\nlabel=\"My Disk\"\r\n"
                             L"icon=\"a b.ico\",1\r\n[Other]\r\nopen=other.exe\r\n");

        Check(Value(p, L"open") == L"setup.exe /s", L"a value is trimmed", Value(p, L"open"));
        Check(Value(p, L"OPEN") == L"setup.exe /s", L"keys are matched without regard to case", Value(p, L"OPEN"));
        Check(Unquote(Value(p, L"label")) == L"My Disk", L"a label loses its quotes", Value(p, L"label"));
        Check(Value(p, L"icon") == L"\"a b.ico\",1", L"a value keeps its quotes", Value(p, L"icon"));
    }
    {
        const Pairs p = Keys(L"[autorun]\nopen=x86.exe\n[autorun.amd64]\nopen=x64.exe\n");

        Check(Value(p, L"open") == L"x64.exe", L"the architecture's section is read", Value(p, L"open"));
    }
    {
        const Pairs p = Keys(L"[autorun.amd64]\nicon=x.ico\n[autorun]\nopen=x86.exe\n");

        Check(Value(p, L"open").empty(), L"the architecture's section stands alone", Value(p, L"open"));
    }
    {
        const Pairs p = AutorunKeys(L"[autorun]\nopen=x86.exe\n[autorun.amd64]\nopen=x64.exe\n", nullptr);

        Check(Value(p, L"open") == L"x86.exe", L"without an architecture's section, [autorun] is read",
              Value(p, L"open"));
    }
    {
        static const unsigned char utf8[]  = { 0xef, 0xbb, 0xbf, '[', 'a', ']', '\n', 'l', '=', 0xc3, 0xa9 };
        static const unsigned char utf16[] = { 0xff, 0xfe, 'l', 0, '=', 0, 0xe9, 0 };

        Check(Decode(std::vector<char>(utf8, utf8 + sizeof(utf8))) == L"[a]\nl=\x00e9", L"UTF-8 with its mark",
              Decode(std::vector<char>(utf8, utf8 + sizeof(utf8))));
        Check(Decode(std::vector<char>(utf16, utf16 + sizeof(utf16))) == L"l=\x00e9", L"UTF-16 with its mark",
              Decode(std::vector<char>(utf16, utf16 + sizeof(utf16))));
    }

    /* The entry that names the program. */
    {
        std::wstring e;
        bool         autoplay = false;

        Check(AutorunEntry(Keys(L"[autorun]\nopen=a.exe\nshellexecute=b.htm\n"), &e, &autoplay) && (e == L"b.htm"),
              L"shellexecute before open", e);
        Check(AutorunEntry(Keys(L"[autorun]\nopen=\"a b.exe\" /x\n"), &e, &autoplay) && (e == L"\"a b.exe\" /x"),
              L"open as it stands", e);
        Check(!AutorunEntry(Keys(L"[autorun]\nopen=a.exe\nUseAutoPlay=1\n"), &e, &autoplay) && autoplay,
              L"UseAutoPlay=1 launches nothing", e);
        Check(AutorunEntry(Keys(L"[autorun]\nopen=a.exe\nuseautoplay=0\n"), &e, &autoplay) && !autoplay,
              L"UseAutoPlay=0 launches", e);
        Check(!AutorunEntry(Keys(L"[autorun]\nicon=a.ico\nlabel=x\n"), &e, &autoplay) && !autoplay,
              L"an autorun.inf without a program", e);
    }

    /* Command lines on a disk, and the disk's autorun.inf. */
    {
        static const char inf[] = "[autorun]\r\nopen=setup.exe /s\r\n";
        wchar_t           tmp[MAX_PATH];
        Pairs             keys;
        uint64_t          hash  = 0;
        DWORD             error = ERROR_SUCCESS;

        (void) GetTempPathW(MAX_PATH, tmp);
        const std::wstring root = std::wstring(tmp) + L"RetroRun-launch-test\\";
        (void) CreateDirectoryW(root.c_str(), nullptr);
        (void) CreateDirectoryW((root + L"sub").c_str(), nullptr);
        (void) CreateDirectoryW((root + L"tools").c_str(), nullptr);
        for (const wchar_t *f : { L"SETUP.EXE", L"My Setup.exe", L"sub\\INSTALL.EXE", L"README.TXT" })
            Check(WriteBytes(root + f, "", 0), L"a file of the test's disk", f);

        Split(root, L"setup.exe /s", root + L"SETUP.EXE", L"/s");
        Split(root, L"setup /s", root + L"SETUP.EXE", L"/s");
        Split(root, L"My Setup.exe -x  y", root + L"My Setup.exe", L"-x  y");
        Split(root, L"\"My Setup.exe\" \"a b\"", root + L"My Setup.exe", L"\"a b\"");
        Split(root, L"sub\\install.exe", root + L"sub\\INSTALL.EXE", L"");
        Split(root, L"\\sub\\install", root + L"sub\\INSTALL.EXE", L"");
        Split(root, L"readme.txt", root + L"README.TXT", L"");
        Split(root, L"notepad.exe readme.txt", L"notepad.exe", L"readme.txt");
        Split(root, L"tools x", L"tools", L"x");
        Split(root, L"\"not here.exe\" x", L"not here.exe", L"x");
        Split(root, root + L"SETUP.EXE /q", root + L"SETUP.EXE", L"/q");
        Split(root, L"  setup.exe  ", root + L"SETUP.EXE", L"");
        Split(root, L"", L"", L"");

        /* Windows 95's "start document". */
        Split(root, L"start readme.txt", root + L"README.TXT", L"");
        Split(root, L"START /m /w setup.exe /s", root + L"SETUP.EXE", L"/s");
        Split(root, L"start \"My Setup.exe\" x", root + L"My Setup.exe", L"x");
        Split(root, L"start.exe index.htm", L"index.htm", L"");
        Split(root, L"start", L"start", L"");
        Check(WriteBytes(root + L"START.EXE", "", 0), L"the disk's own START program");
        Split(root, L"start readme.txt", root + L"START.EXE", L"readme.txt");
        (void) DeleteFileW((root + L"START.EXE").c_str());

        Check(!ReadAutorun(root, keys, &hash, &error) && (error == ERROR_FILE_NOT_FOUND),
              L"a disk without an autorun.inf", std::to_wstring(error));
        Check(WriteBytes(root + L"AUTORUN.INF", inf, sizeof(inf) - 1), L"the test's autorun.inf");
        Check(ReadAutorun(root, keys, &hash, &error) && (Value(keys, L"open") == L"setup.exe /s") && (hash != 0),
              L"the disk's autorun.inf", Value(keys, L"open"));

        for (const wchar_t *f : { L"SETUP.EXE", L"My Setup.exe", L"sub\\INSTALL.EXE", L"README.TXT", L"AUTORUN.INF" })
            (void) DeleteFileW((root + f).c_str());
        (void) RemoveDirectoryW((root + L"sub").c_str());
        (void) RemoveDirectoryW((root + L"tools").c_str());
        (void) RemoveDirectoryW(root.c_str());
    }

    /* The kinds of floppy drives, by their models. */
    Check(FloppyKind(L"MATSHITA LS-120 VER5 00") == KindSuperDisk, L"an LS-120 drive is a SuperDisk drive");
    Check(FloppyKind(L"MATSHITA LS-240 COSM 04") == KindSuperDisk, L"an LS-240 drive is a SuperDisk drive");
    Check(FloppyKind(L"Imation SuperDisk USB 120") == KindSuperDisk, L"a SuperDisk drive by name");
    Check(FloppyKind(L"TEAC FD-05PUW") == KindFloppy, L"a USB floppy drive");
    Check(FloppyKind(L"\\Device\\Floppy0") == KindFloppy, L"a floppy controller's drive");

    wprintf(L"%d of %d checks passed\n", g_checks - g_failures, g_checks);
    return (g_failures == 0) ? 0 : 1;
}
