/*
 * RetroRun, a tray program for removable disks. It does three things, each for the kinds of drives chosen in its
 * tray menu.
 *
 * 1. It shows disks' own names and icons in File Explorer where Explorer leaves them out. Explorer draws a floppy
 *    drive as "Floppy Disk Drive" with the stock icon, whatever disk is in it, because it reads nothing from a
 *    floppy: no autorun.inf, not even the volume label. RetroRun reads the disk instead and hands Explorer what it
 *    found through the drive letter's per-user key,
 *    HKCU\Software\Classes\Applications\Explorer.exe\Drives\<letter>, whose DefaultLabel and DefaultIcon Explorer
 *    shows for that letter. The key needs no administrator rights.
 *    - The name is the autorun.inf's label, or else the volume label.
 *    - The icon is the autorun.inf's icon, or else the first icon file in the disk's root, or else Explorer's own.
 *    The menu turns each of these sources on and off; at first, floppy and SuperDisk drives are chosen. Explorer
 *    reads other drives itself, and what it finds wins over the key: the volume label over DefaultLabel, the
 *    autorun.inf's icon and label over both. There the key only fills a gap, such as the first icon file of a
 *    disc whose autorun.inf names no icon.
 *
 * 2. It starts the program that a disk's autorun.inf names as the disk goes in, as AutoRun once did. Windows now
 *    does that only for CDs and DVDs, through AutoPlay. The program is the shellexecute entry, which may name a
 *    document, or else the open entry, from the disk's root; a file with UseAutoPlay=1 asks for AutoPlay instead
 *    and starts nothing. A disk is skipped when Shift is held as it goes in, when another program cancels AutoRun
 *    (the window in front, or one registered with AutoPlay), and when AutoPlay is set to start a disc's program
 *    itself and is not turned off for the drive (see 3). A disk that is already in when RetroRun starts, or when
 *    its kind is chosen, is not started. A floppy controller's drive, though, tells of no disk until Windows
 *    mounts it, so a disk there is started when it is first read.
 *
 * 3. It keeps Windows' AutoPlay from acting on the drives whose native AutoPlay handling the menu turns off.
 *    While RetroRun runs, it registers with AutoPlay to be asked first (an IQueryCancelAutoPlay object in the
 *    running object table, whose class the per-user CancelAutoplay key lists) and answers that AutoPlay is not to
 *    act: it neither asks what to do, nor does anything, nor starts a disc's program. Explorer still reads the
 *    disks itself, which no per-user setting changes.
 *
 * Drives are found by their devices, whatever their letters, and followed as they come and go: those of the kinds
 * the menu has something to do for. How a disk is noticed depends on the drive:
 * - A drive that describes itself, as SuperDisk, CD and other ATAPI and USB drives do, can tell from its own
 *   state whether a disk is in. While "Look for disks" is chosen for its kind, it is asked every two seconds,
 *   until it reports its disks by itself.
 * - A floppy controller's drive can tell only by running its motor, so its disk is read when Windows mounts it,
 *   as when someone opens the drive, or when the user asks.
 * - A hard disk's disk never changes, so there is nothing to look for.
 * - Drives other than floppy drives also tell of their disks through Windows' device broadcasts.
 * A drive whose volume another program has locked is left alone until the program lets it go, also when the
 * program took the lock before the drive was followed, as one does that had the drive when it came: no
 * notification tells of that lock, but the volume refuses to be read. Disks are read on a worker thread, so a slow
 * drive never holds up the tray icon.
 */
#include "drives.h"
#include "inf.h"
#include "launch.h"
#include <winioctl.h>
#include <dbt.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <shlwapi.h>
#include <windowsx.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <wchar.h>
#include <wctype.h>
#include <deque>
#include <string>
#include <utility>
#include <vector>
#include <initguid.h>
#include <ioevent.h>

namespace {

const wchar_t kClass[]       = L"RetroRunWindow";
const wchar_t kMutex[]       = L"Local\\RetroRun";
const wchar_t kSettings[]    = L"Software\\RetroRun";
const wchar_t kDrives[]      = L"Software\\Classes\\Applications\\Explorer.exe\\Drives";
const wchar_t kRun[]         = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
const wchar_t kCancelList[]  = L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\AutoplayHandlers\\CancelAutoplay\\CLSID";
/* The Run value's name, and the value that marks a letter's key as this program's. */
const wchar_t kName[]        = L"RetroRun";
/* The class under which the program registers with AutoPlay, and its name in the CancelAutoplay key. */
const CLSID   kCancelClass   = { 0x6956C4D3, 0x3AE2, 0x48CA, { 0xB6, 0xF1, 0xA4, 0x38, 0x8C, 0x71, 0xEF, 0x93 } };
const wchar_t kCancelValue[] = L"6956C4D3-3AE2-48CA-B6F1-A4388C71EF93";

constexpr UINT   kTrayMessage   = WM_APP + 1;
constexpr UINT   kResultMessage = WM_APP + 2;
constexpr UINT   kTrayId        = 1;
constexpr UINT   kTimerId       = 1;
constexpr DWORD  kIconMax       = 8u << 20;  /* An icon file is read up to this size. */
constexpr DWORD  kLogMax        = 512u << 10;
constexpr int    kScanEvery     = 5;         /* Drives are looked for every fifth check, besides the broadcasts. */
constexpr size_t kLabelMax      = 64;
/* A mount is answered once its burst has settled: Windows mounts an empty drive's volume several times in a row
   after it is let go, and a read of the drive is itself a mount. Mounts just after a read are its own. */
constexpr UINT      kMountTimer  = 100; /* plus the drive's letter's index */
constexpr UINT      kMountSettle = 400;
constexpr ULONGLONG kAfterRead   = 1500;
/* Explorer's navigation pane takes a drive's entry out as its volume is dismounted or mounted, and builds it
   again, at times from what it read before the key changed; a notification meanwhile finds no entry to update.
   So a drive that is asked whether a disk is in is notified again 1 and 3 s after its key changes. Each
   notification has Explorer open the drive: not after a volume event, whose notifications would mount the
   volume again and so loop, not for a controller's drive, whose motor would run, and not later, when a larger
   disk may have spun down and keep the drive busy for its spin-up. */
constexpr UINT kRefreshTimer  = 200; /* plus the drive's letter's index */
constexpr UINT kRefreshGaps[] = { 1000, 2000 };
/* Windows may broadcast a volume's arrival before its letter is there, and AutoPlay may ask of a drive before the
   broadcast: the drives are looked for again a second later. */
constexpr UINT kScanTimer = 300;
constexpr UINT kScanAgain = 1000;
/* A disc is reported as it goes in, and can be read once it has spun up: its autorun.inf is tried every half
   second for ten seconds. A disk whose arrival is reported twice is launched once. */
constexpr int       kLaunchTries = 20;
constexpr DWORD     kLaunchWait  = 500;
constexpr ULONGLONG kLaunchAgain = 2000;
constexpr DWORD     kAllKinds    = (1u << KindCount) - 1;

/* The kinds of drives, as the tray menu lists them. */
const wchar_t *const kKindNames[KindCount] = { L"Floppy disk drives", L"SuperDisk drives", L"USB drives",
                                               L"Memory cards", L"Other removable disks", L"CD and DVD drives",
                                               L"External hard drives" };

enum : UINT {
    IDM_READ = 1000,
    IDM_AUTORUN_ICON,
    IDM_AUTORUN_LABEL,
    IDM_FILE_ICON,
    IDM_VOLUME_LABEL,
    IDM_STARTUP,
    IDM_EXIT,
    /* The submenus of kinds of drives: each kind plus its number, all of them plus KindCount. */
    IDM_DISPLAY = 1100,
    IDM_LAUNCH  = 1200,
    IDM_NATIVE  = 1300,
    IDM_LOOK    = 1400
};

struct Settings {
    bool  autorunIcon  = true;
    bool  autorunLabel = true;
    bool  fileIcon     = true; /* the first icon file in the disk's root, without an autorun.inf icon */
    bool  volumeLabel  = true; /* the volume label, without an autorun.inf label */
    DWORD checkMs     = 2000;
    bool  refreshAll  = false; /* tell Explorer that any icon may have changed, not only the drive's */
    /* The kinds of drives, a bit each: whose disks' names and icons are shown, whose autorun.inf programs are
       started, whose native handling by Windows' AutoPlay is turned off, and whose drives that describe
       themselves are asked whether a disk is in. */
    DWORD display     = (1u << KindFloppy) | (1u << KindSuperDisk);
    DWORD launch      = 0;
    DWORD native      = 0;
    DWORD look        = kAllKinds;
};

struct Drive {
    wchar_t      letter = 0;
    std::wstring device; /* \Device\Floppy<n>, \Device\CdRom<n>, \Device\HarddiskVolume<n> */
    std::wstring model;
    bool         tells    = false; /* a floppy drive that answers the class driver's check from its own state */
    HANDLE       handle   = INVALID_HANDLE_VALUE;
    HDEVNOTIFY   notify   = nullptr;
    bool         locked   = false; /* another program has locked the volume */
    bool         held     = false; /* the volume refused a read: another program holds it, whose lock went unseen */
    bool         events   = false; /* the drive reported a disk coming or going itself, so it is no longer asked */
    bool         reports  = false; /* Windows' broadcasts tell of its disks coming and going, as of any drive's
                                      but a floppy drive's */
    bool         foreign  = false; /* the letter's key is someone else's, so the drive is left alone */
    int          media    = -1;    /* -1 not known, 0 no disk, 1 a disk */
    bool         counted  = false;
    ULONG        changes  = 0;     /* the class driver's media change count */
    bool         checking = false;
    bool         reading  = false;
    ULONGLONG    quietUntil = 0;   /* mounts until then are the last read's own */
    size_t       refreshes  = 0;   /* the delayed notifications already sent, of kRefreshGaps */
    unsigned     gen      = 0;     /* bumped when the drive's state starts again: results of before are dropped */
    DriveKind    kind     = KindFloppy;

    /* For the autorun.inf programs: whether the next disk read went in while the program watched the drive, so
       that it is launched; whether Shift was held as that read was started; and the disk read last, by its serial
       number, autorun.inf and volume label, or nothing while there is none. */
    bool         armed    = false;
    bool         shift    = false;
    std::wstring identity;

    /* What the last read of the disk found: its labels, and its icons, as cached icon files' locations. */
    bool         read = false;
    std::wstring volumeLabel;
    std::wstring autorunLabel;
    std::wstring autorunIcon;
    std::wstring fileIcon;

    /* What the letter's key gives Explorer. */
    std::wstring shownLabel;
    std::wstring shownIcon;
};

/* What a letter's drive was found to be, kept while the letter names the same device. */
struct Seen {
    bool         known = false;
    std::wstring device;
    std::wstring model;
    DriveKind    kind  = KindNone;
    bool         tells = false; /* a floppy drive's: whether it describes itself */
};

struct Job {
    enum Kind { Check, Read, Quit } kind;
    wchar_t  letter;
    unsigned gen;
};

struct Result {
    Job::Kind    kind    = Job::Check;
    wchar_t      letter  = 0;
    unsigned     gen     = 0;
    DWORD        error   = ERROR_SUCCESS;
    bool         counted = false;
    ULONG        changes = 0;
    bool         disk    = false;
    DWORD        serial  = 0;
    uint64_t     infHash = 0; /* of the autorun.inf, or 0 */
    std::wstring volumeLabel;
    std::wstring autorunLabel;
    std::wstring autorunIcon;
    std::wstring fileIcon;
};

HWND               g_hwnd;
HICON              g_trayIcon;
UINT               g_taskbarCreated;
Settings           g_set;
std::vector<Drive> g_drives;
Seen               g_seen[26];
ULONGLONG          g_launched[26]; /* when each letter's disk was last launched */
DWORD              g_cancelCookie; /* the running object table's for the AutoPlay registration, or 0 */
int                g_ticks;
std::wstring       g_iconDir;
std::wstring       g_logPath;
CRITICAL_SECTION   g_jobLock;
std::deque<Job>    g_jobs;
HANDLE             g_jobEvent;
HANDLE             g_worker;

void
Log(const wchar_t *fmt, ...)
{
    wchar_t    msg[1024];
    wchar_t    line[1100];
    char       utf8[4400];
    SYSTEMTIME t;
    va_list    ap;
    DWORD      wrote = 0;

    if (g_logPath.empty())
        return;

    va_start(ap, fmt);
    _vsnwprintf_s(msg, _countof(msg), _TRUNCATE, fmt, ap);
    va_end(ap);
    GetLocalTime(&t);
    const int n   = swprintf_s(line, L"%04u-%02u-%02u %02u:%02u:%02u.%03u %ls\r\n", static_cast<unsigned>(t.wYear),
                               static_cast<unsigned>(t.wMonth), static_cast<unsigned>(t.wDay),
                               static_cast<unsigned>(t.wHour), static_cast<unsigned>(t.wMinute),
                               static_cast<unsigned>(t.wSecond), static_cast<unsigned>(t.wMilliseconds), msg);
    const int len = WideCharToMultiByte(CP_UTF8, 0, line, (n > 0) ? n : 0, utf8, sizeof(utf8), nullptr, nullptr);

    /* Appended a line at a time, from either thread. */
    const HANDLE f = CreateFileW(g_logPath.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                                 OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE)
        return;
    (void) WriteFile(f, utf8, static_cast<DWORD>((len > 0) ? len : 0), &wrote, nullptr);
    CloseHandle(f);
}

/* Settings. */

DWORD
ReadSetting(const wchar_t *name, const DWORD def)
{
    DWORD v    = 0;
    DWORD size = sizeof(v);

    return (RegGetValueW(HKEY_CURRENT_USER, kSettings, name, RRF_RT_REG_DWORD, nullptr, &v, &size) == ERROR_SUCCESS) ? v :
                                                                                                                       def;
}

void
WriteSetting(const wchar_t *name, const DWORD v)
{
    (void) RegSetKeyValueW(HKEY_CURRENT_USER, kSettings, name, REG_DWORD, &v, sizeof(v));
}

void
LoadSettings()
{
    g_set.autorunIcon  = ReadSetting(L"AutorunIcon", 1) != 0;
    g_set.autorunLabel = ReadSetting(L"AutorunLabel", 1) != 0;
    g_set.fileIcon    = ReadSetting(L"FileIcon", 1) != 0;
    g_set.volumeLabel = ReadSetting(L"VolumeLabel", 1) != 0;
    g_set.checkMs     = ReadSetting(L"CheckMs", 2000);
    g_set.refreshAll  = ReadSetting(L"RefreshAll", 0) != 0;
    g_set.display     = ReadSetting(L"Display", g_set.display) & kAllKinds;
    g_set.launch      = ReadSetting(L"Launch", 0) & kAllKinds;
    g_set.native      = ReadSetting(L"Native", 0) & kAllKinds;
    g_set.look        = ReadSetting(L"Look", kAllKinds) & kAllKinds;
    if (g_set.checkMs < 500)
        g_set.checkMs = 500;
}

bool
StartsWithWindows()
{
    return RegGetValueW(HKEY_CURRENT_USER, kRun, kName, RRF_RT_REG_SZ, nullptr, nullptr, nullptr) == ERROR_SUCCESS;
}

void
SetStartsWithWindows(const bool on)
{
    wchar_t exe[2 * MAX_PATH];

    if (!on) {
        (void) RegDeleteKeyValueW(HKEY_CURRENT_USER, kRun, kName);
        return;
    }
    exe[0]        = L'"';
    const DWORD n = GetModuleFileNameW(nullptr, exe + 1, MAX_PATH);
    if ((n == 0) || (n >= MAX_PATH))
        return;
    wcscat_s(exe, L"\"");
    (void) RegSetKeyValueW(HKEY_CURRENT_USER, kRun, kName, REG_SZ, exe,
                           static_cast<DWORD>((wcslen(exe) + 1) * sizeof(wchar_t)));
}

/* The program's data: the log, and the icons copied from the disks, under %LOCALAPPDATA%\RetroRun. The icons
   of before are cleared, since no key points at them any more. */
void
InitData()
{
    PWSTR            base = nullptr;
    WIN32_FIND_DATAW f;

    if (FAILED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &base)))
        return;
    const std::wstring dir = std::wstring(base) + L"\\RetroRun";
    CoTaskMemFree(base);
    g_iconDir = dir + L"\\Icons";
    (void) CreateDirectoryW(dir.c_str(), nullptr);
    (void) CreateDirectoryW(g_iconDir.c_str(), nullptr);

    const std::wstring log = dir + L"\\RetroRun.log";
    WIN32_FILE_ATTRIBUTE_DATA a;
    if (GetFileAttributesExW(log.c_str(), GetFileExInfoStandard, &a) && ((a.nFileSizeHigh != 0) || (a.nFileSizeLow > kLogMax)))
        (void) DeleteFileW(log.c_str());
    g_logPath = log;

    const HANDLE h = FindFirstFileW((g_iconDir + L"\\*").c_str(), &f);
    if (h == INVALID_HANDLE_VALUE)
        return;
    do {
        if (!(f.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY))
            (void) DeleteFileW((g_iconDir + L"\\" + f.cFileName).c_str());
    } while (FindNextFileW(h, &f));
    FindClose(h);
}

/* Reading a disk, on the worker thread. */

/* An icon on the disk, "file[,index]" as an autorun.inf names one: a file of the disk itself, copied to the cache
   under a name its contents give, as Explorer keeps icons by their file and index and so must never meet one
   disk's icon under another's name. Returns its location for DefaultIcon, or nothing if the disk has no such
   icon. */
std::wstring
CacheIcon(const wchar_t letter, const std::wstring &spec)
{
    static const wchar_t *const kTypes[] = { L".ico", L".exe", L".dll", L".icl", L".cpl", L".ocx" };
    std::wstring                file     = spec;
    int                         index    = 0;
    const size_t                comma    = spec.rfind(L',');
    wchar_t                     root[]   = L"A:\\";
    wchar_t                     name[40];
    std::vector<char>           data;
    bool                        known = false;

    if (comma != std::wstring::npos) {
        const std::wstring num = Trim(spec.substr(comma + 1));
        wchar_t           *end = nullptr;
        const long         v   = wcstol(num.c_str(), &end, 10);

        if (!num.empty() && (end != nullptr) && (*end == 0)) {
            index = static_cast<int>(v);
            file  = spec.substr(0, comma);
        }
    }
    file = Unquote(Trim(file));
    for (auto &c : file)
        if (c == L'/')
            c = L'\\';
    while (!file.empty() && (file[0] == L'\\'))
        file.erase(0, 1);
    if (file.empty() || (file.find(L':') != std::wstring::npos) || (file.find(L"..") != std::wstring::npos)) {
        Log(L"%lc: the icon '%ls' is not a file of the disk", letter, spec.c_str());
        return L"";
    }

    std::wstring ext = PathFindExtensionW(file.c_str());
    for (auto &c : ext)
        c = static_cast<wchar_t>(towlower(c));
    for (const wchar_t *t : kTypes)
        known = known || (ext == t);
    if (!known) {
        Log(L"%lc: the icon '%ls' is not in an icon file", letter, spec.c_str());
        return L"";
    }

    root[0] = letter;
    if (!ReadSmallFile(std::wstring(root) + file, kIconMax, data)) {
        Log(L"%lc: the icon file '%ls' cannot be read (error %lu)", letter, file.c_str(), GetLastError());
        return L"";
    }

    swprintf_s(name, L"%016llx", static_cast<unsigned long long>(Hash(data)));
    const std::wstring path = g_iconDir + L"\\" + name + ext;
    if (GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES) {
        const std::wstring part  = path + L".part";
        const HANDLE       h     = CreateFileW(part.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                                               FILE_ATTRIBUTE_NORMAL, nullptr);
        DWORD              wrote = 0;

        if (h == INVALID_HANDLE_VALUE)
            return L"";
        const bool ok = data.empty() ||
                        (WriteFile(h, data.data(), static_cast<DWORD>(data.size()), &wrote, nullptr) &&
                         (wrote == data.size()));
        CloseHandle(h);
        if (!ok || !MoveFileExW(part.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING)) {
            (void) DeleteFileW(part.c_str());
            return L"";
        }
    }

    /* Explorer is to find an icon there; a negative index names one by its resource ID. */
    const UINT n = ExtractIconExW(path.c_str(), -1, nullptr, nullptr, 0);
    if ((n == 0) || ((index >= 0) && (static_cast<UINT>(index) >= n))) {
        Log(L"%lc: '%ls' has no icon %d", letter, file.c_str(), index);
        return L"";
    }
    return path + L"," + std::to_wstring(index);
}

/* The first icon file in the disk's root, in the directory's own order. A pattern of "*.ico" also finds a longer
   extension through its short name, so the name is checked. */
std::wstring
FirstIconFile(const wchar_t letter)
{
    wchar_t          pattern[] = L"A:\\*.ico";
    WIN32_FIND_DATAW f;
    std::wstring     found;

    pattern[0]     = letter;
    const HANDLE h = FindFirstFileExW(pattern, FindExInfoBasic, &f, FindExSearchNameMatch, nullptr, 0);
    if (h == INVALID_HANDLE_VALUE)
        return L"";
    do {
        if (!(f.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && (_wcsicmp(PathFindExtensionW(f.cFileName), L".ico") == 0))
            found = f.cFileName;
    } while (found.empty() && FindNextFileW(h, &f));
    FindClose(h);
    return found;
}

void
DoCheck(Result &r)
{
    const HANDLE h     = OpenDevice(r.letter);
    ULONG        count = 0;
    DWORD        got   = 0;

    if (h == INVALID_HANDLE_VALUE) {
        r.error = GetLastError();
        return;
    }
    if (DeviceIoControl(h, IOCTL_STORAGE_CHECK_VERIFY2, nullptr, 0, &count, sizeof(count), &got, nullptr)) {
        r.counted = (got >= sizeof(count));
        r.changes = count;
    } else
        r.error = GetLastError();
    CloseHandle(h);
}

/* The disk's volume label, its autorun.inf's label and icon, and its first icon file, whatever the settings, so
   that a setting changed later needs no new read; and its serial number and the autorun.inf's hash, which tell
   it from another disk. */
void
DoRead(Result &r)
{
    wchar_t root[]              = L"A:\\";
    wchar_t label[MAX_PATH + 1] = {};
    Pairs   keys;

    root[0] = r.letter;
    if (!GetVolumeInformationW(root, label, MAX_PATH + 1, &r.serial, nullptr, nullptr, nullptr, 0)) {
        r.error = GetLastError();
        return;
    }
    r.disk        = true;
    r.volumeLabel = label;

    if (ReadAutorun(root, keys, &r.infHash, nullptr)) {
        r.autorunLabel          = Unquote(Value(keys, L"label")).substr(0, kLabelMax);
        const std::wstring icon = Value(keys, L"icon");
        if (!icon.empty())
            r.autorunIcon = CacheIcon(r.letter, icon);
    }

    const std::wstring first = FirstIconFile(r.letter);
    if (!first.empty())
        r.fileIcon = CacheIcon(r.letter, first);
}

DWORD WINAPI
Worker(void *)
{
    for (;;) {
        Job  job = { Job::Quit, 0, 0 };
        bool any;

        EnterCriticalSection(&g_jobLock);
        any = !g_jobs.empty();
        if (any) {
            job = g_jobs.front();
            g_jobs.pop_front();
        }
        LeaveCriticalSection(&g_jobLock);
        if (!any) {
            (void) WaitForSingleObject(g_jobEvent, INFINITE);
            continue;
        }
        if (job.kind == Job::Quit)
            return 0;

        Result *r = new Result();
        r->kind   = job.kind;
        r->letter = job.letter;
        r->gen    = job.gen;
        if (job.kind == Job::Check)
            DoCheck(*r);
        else
            DoRead(*r);
        if (!PostMessageW(g_hwnd, kResultMessage, 0, reinterpret_cast<LPARAM>(r)))
            delete r;
    }
}

void
Queue(const Job::Kind kind, const wchar_t letter, const unsigned gen)
{
    EnterCriticalSection(&g_jobLock);
    g_jobs.push_back({ kind, letter, gen });
    LeaveCriticalSection(&g_jobLock);
    (void) SetEvent(g_jobEvent);
}

/* What Explorer shows, on the window's thread. */

std::wstring
KeyOf(const wchar_t letter)
{
    std::wstring k = kDrives;

    k += L'\\';
    k += letter;
    return k;
}

void
Refresh(const wchar_t letter)
{
    wchar_t root[] = L"A:\\";

    root[0] = letter;
    SHChangeNotify(SHCNE_UPDATEITEM, SHCNF_PATHW | SHCNF_FLUSHNOWAIT, root, nullptr);
    if (g_set.refreshAll)
        SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST | SHCNF_FLUSHNOWAIT, nullptr, nullptr);
}

/* The delayed notifications, from the first again. */
void
Settle(Drive &d)
{
    d.refreshes = 0;
    (void) SetTimer(g_hwnd, kRefreshTimer + static_cast<UINT>(d.letter - L'A'), kRefreshGaps[0], nullptr);
}

/* A subkey's default value, or no subkey, which leaves Explorer its own. */
void
SetDefault(HKEY key, const wchar_t *sub, const std::wstring &value)
{
    if (value.empty())
        (void) RegDeleteTreeW(key, sub);
    else
        (void) RegSetKeyValueW(key, sub, nullptr, REG_SZ, value.c_str(),
                               static_cast<DWORD>((value.size() + 1) * sizeof(wchar_t)));
}

/* Whether the drive is asked whether a disk is in: the menu looks for disks in drives of its kind, it describes
   itself, and it has not reported a disk coming or going itself. */
bool
Asked(const Drive &d)
{
    return (g_set.look & (1u << d.kind)) && d.tells && !d.events;
}

void
SetKey(Drive &d, const std::wstring &label, const std::wstring &icon)
{
    const std::wstring sub = KeyOf(d.letter);

    if (d.foreign || ((label == d.shownLabel) && (icon == d.shownIcon)))
        return;

    if (label.empty() && icon.empty())
        (void) RegDeleteTreeW(HKEY_CURRENT_USER, sub.c_str());
    else {
        HKEY        key = nullptr;
        const DWORD one = 1;

        if (RegCreateKeyExW(HKEY_CURRENT_USER, sub.c_str(), 0, nullptr, 0, KEY_ALL_ACCESS, nullptr, &key, nullptr) !=
            ERROR_SUCCESS)
            return;
        (void) RegSetValueExW(key, kName, 0, REG_DWORD, reinterpret_cast<const BYTE *>(&one), sizeof(one));
        SetDefault(key, L"DefaultLabel", label);
        SetDefault(key, L"DefaultIcon", icon);
        RegCloseKey(key);
    }
    d.shownLabel = label;
    d.shownIcon  = icon;
    Log(L"%lc: Explorer shows the name '%ls' and the icon '%ls'", d.letter,
        label.empty() ? L"(its own)" : label.c_str(), icon.empty() ? L"(its own)" : icon.c_str());
    Refresh(d.letter);
    if (IsFloppyKind(d.kind) && Asked(d))
        Settle(d);
}

/* The name and icon the settings give the disk: the autorun.inf's label, or else the volume label; the
   autorun.inf's icon, or else the first icon file's, or else Explorer's own; nothing while there is no disk, or
   for a kind of drives the menu does not show disks of. While another program wants the volume locked, or holds
   it, nothing changes, so that Explorer, answering the notification of a change, does not open the volume under
   the lock; the change waits for the lock to end. */
void
Show(Drive &d)
{
    std::wstring label;
    std::wstring icon;

    if (d.locked || d.held)
        return;
    if (d.read && (d.media == 1) && (g_set.display & (1u << d.kind))) {
        if (g_set.autorunLabel && !d.autorunLabel.empty())
            label = d.autorunLabel;
        else if (g_set.volumeLabel)
            label = d.volumeLabel;
        if (g_set.autorunIcon && !d.autorunIcon.empty())
            icon = d.autorunIcon;
        else if (g_set.fileIcon)
            icon = d.fileIcon;
    }
    SetKey(d, label, icon);
}

void
ShowAll()
{
    for (auto &d : g_drives)
        Show(d);
}

/* Launching the disks' autorun.inf programs. */

struct Launch {
    wchar_t   letter;
    DriveKind kind;
    bool      native; /* the kind's native handling is off, and Windows' AutoPlay cancelled for it */
};

bool
ShiftDown()
{
    return (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0;
}

void
LaunchDisk(const Launch &l)
{
    wchar_t      r[]                 = L"A:\\";
    wchar_t      label[MAX_PATH + 1] = {};
    DWORD        serial              = 0;
    DWORD        error               = ERROR_SUCCESS;
    bool         autoplay            = false;
    Pairs        keys;
    std::wstring entry;
    std::wstring file;
    std::wstring params;

    r[0]                    = l.letter;
    const std::wstring root = r;
    for (int tries = 1; !ReadAutorun(root, keys, nullptr, &error); tries++) {
        if ((error == ERROR_NOT_READY) && (tries < kLaunchTries)) {
            Sleep(kLaunchWait);
            continue;
        }
        if (error == ERROR_NOT_READY)
            Log(L"%lc: no disk to launch", l.letter);
        else if ((error == ERROR_FILE_NOT_FOUND) || (error == ERROR_PATH_NOT_FOUND))
            Log(L"%lc: no autorun.inf to launch", l.letter);
        else
            Log(L"%lc: the autorun.inf cannot be read (error %lu)", l.letter, error);
        return;
    }
    if (!AutorunEntry(keys, &entry, &autoplay)) {
        if (autoplay)
            Log(L"%lc: the autorun.inf asks for AutoPlay instead (UseAutoPlay=1), so nothing is launched", l.letter);
        else
            Log(L"%lc: the autorun.inf names no program", l.letter);
        return;
    }
    if ((l.kind == KindOptical) && !l.native && AutoPlayRunsDiscs(l.letter)) {
        Log(L"%lc: Windows' AutoPlay starts the disc's program itself", l.letter);
        return;
    }
    (void) GetVolumeInformationW(root.c_str(), label, MAX_PATH + 1, &serial, nullptr, nullptr, nullptr, 0);
    if (AutoRunCancelled(l.letter, label, serial, kCancelClass)) {
        Log(L"%lc: a program that is running cancelled AutoRun", l.letter);
        return;
    }
    SplitCommand(root, entry, &file, &params);
    error = StartFile(root, file, params);
    if (error == ERROR_SUCCESS)
        Log(L"%lc: started '%ls' with '%ls', as the autorun.inf names it: '%ls'", l.letter, file.c_str(),
            params.c_str(), entry.c_str());
    else
        Log(L"%lc: '%ls', as the autorun.inf names it ('%ls'), did not start (error %lu)", l.letter, file.c_str(),
            entry.c_str(), error);
}

/* Each disk is launched on a thread of its own: a disc may take seconds to spin up, and a program may need
   administrator rights, or Windows show why it cannot start, which waits for the user. */
DWORD WINAPI
Launcher(void *p)
{
    const Launch *const l   = static_cast<const Launch *>(p);
    const HRESULT       com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);

    LaunchDisk(*l);
    delete l;
    if (SUCCEEDED(com))
        CoUninitialize();
    return 0;
}

/* A disk that went in while the program watched: its autorun.inf's program is started if the menu has chosen
   the drive's kind, unless Shift was held as the disk went in, as with AutoRun. */
void
Inserted(const wchar_t letter, const DriveKind kind, const bool shift)
{
    ULONGLONG      &last = g_launched[letter - L'A'];
    const ULONGLONG now  = GetTickCount64();

    if ((kind >= KindCount) || !(g_set.launch & (1u << kind)))
        return;
    if (shift) {
        Log(L"%lc: Shift was held as the disk went in, so it is not launched", letter);
        return;
    }
    if ((last != 0) && ((now - last) < kLaunchAgain))
        return;
    last = now;

    Launch *const l = new Launch{ letter, kind, (g_set.native & (1u << kind)) != 0 };
    const HANDLE  t = CreateThread(nullptr, 0, Launcher, l, 0, nullptr);
    if (t == nullptr) {
        Log(L"%lc: no thread to launch the disk (error %lu)", letter, GetLastError());
        delete l;
        return;
    }
    CloseHandle(t);
}

/* A disk went out of the drive, or the drive went: the next disk there is launched however soon it comes. */
void
Removed(const wchar_t letter)
{
    if ((letter >= L'A') && (letter <= L'Z'))
        g_launched[letter - L'A'] = 0;
}

/* The drives. */

Drive *
FindDrive(const wchar_t letter)
{
    for (auto &d : g_drives)
        if (d.letter == letter)
            return &d;
    return nullptr;
}

void
Register(Drive &d)
{
    DEV_BROADCAST_HANDLE f = {};

    d.handle = OpenDevice(d.letter);
    if (d.handle == INVALID_HANDLE_VALUE)
        return;
    f.dbch_size       = sizeof(f);
    f.dbch_devicetype = DBT_DEVTYP_HANDLE;
    f.dbch_handle     = d.handle;
    d.notify          = RegisterDeviceNotificationW(g_hwnd, &f, DEVICE_NOTIFY_WINDOW_HANDLE);
    if (d.notify == nullptr)
        Log(L"%lc: no notifications for the drive (error %lu)", d.letter, GetLastError());
}

void
Unregister(Drive &d)
{
    if (d.notify != nullptr) {
        (void) UnregisterDeviceNotification(d.notify);
        d.notify = nullptr;
    }
    if (d.handle != INVALID_HANDLE_VALUE) {
        CloseHandle(d.handle);
        d.handle = INVALID_HANDLE_VALUE;
    }
}

void StartRead(Drive &d);
bool Watched(const Drive &d);

/* A drive the menu has something to do for. Unless quiet, it came while the program ran, and brought its disk
   with it, as a USB drive does. */
void
AddDrive(const wchar_t letter, const Seen &s, const bool quiet)
{
    Drive              d;
    HKEY               key = nullptr;
    const std::wstring sub = KeyOf(letter);

    d.letter = letter;
    d.device = s.device;
    d.model  = s.model;
    d.kind   = s.kind;
    d.tells   = s.tells;
    d.reports = !IsFloppyKind(s.kind);
    d.armed   = !quiet;

    /* A key this program left, if it ended without taking it back, is taken back now; anyone else's is kept. */
    if (RegOpenKeyExW(HKEY_CURRENT_USER, sub.c_str(), 0, KEY_QUERY_VALUE, &key) == ERROR_SUCCESS) {
        const bool mine = RegQueryValueExW(key, kName, nullptr, nullptr, nullptr, nullptr) == ERROR_SUCCESS;

        RegCloseKey(key);
        if (mine) {
            (void) RegDeleteTreeW(HKEY_CURRENT_USER, sub.c_str());
            Refresh(letter);
        } else {
            d.foreign = true;
            Log(L"%lc: HKCU\\%ls is not this program's, so the drive keeps it", letter, sub.c_str());
        }
    }

    g_drives.push_back(d);
    Register(g_drives.back());
    Log(L"%lc: %ls (%ls), among %ls: %ls", letter, d.model.c_str(), d.device.c_str(), kKindNames[d.kind],
        Asked(d)  ? L"asked whether a disk is in" :
        d.reports ? L"Windows tells of its disks" :
                    L"its disk is read when Windows mounts it");
    /* A floppy drive's disk is read once it is asked or mounted; any other's now. */
    if (d.reports)
        StartRead(g_drives.back());
}

void
RemoveDrive(const size_t i)
{
    Drive &d = g_drives[i];

    Log(L"%lc: no longer followed", d.letter);
    SetKey(d, L"", L"");
    Unregister(d);
    KillTimer(g_hwnd, kMountTimer + static_cast<UINT>(d.letter - L'A'));
    KillTimer(g_hwnd, kRefreshTimer + static_cast<UINT>(d.letter - L'A'));
    g_drives.erase(g_drives.begin() + static_cast<ptrdiff_t>(i));
}

/* What the letter's drive is, sorted once for each device the letter names. */
const Seen &
SeenOf(const wchar_t letter)
{
    Seen              &s      = g_seen[letter - L'A'];
    const std::wstring device = DeviceOf(letter);

    if (!s.known || (device != s.device)) {
        s        = Seen();
        s.known  = true;
        s.device = device;
        s.kind   = device.empty() ? KindNone : KindOf(letter, device, &s.model, &s.tells);
    }
    return s;
}

/* Whether the drives of a kind are followed: the menu shows their disks or launches them. */
bool
Followed(const DriveKind kind)
{
    return (kind < KindCount) && (((g_set.display | g_set.launch) & (1u << kind)) != 0);
}

/* The drives there are now, by their devices: one that went, whose letter another device took, or whose kind
   the menu no longer has anything to do for, is dropped; one that came, or whose kind the menu now has
   something to do for, is added. Unless quiet, one that came brings its disk, as a USB drive does; at the start,
   and as the menu chooses kinds, the disks in the drives are not launched. */
void
Scan(const bool quiet = false)
{
    const DWORD mask = GetLogicalDrives();

    for (int i = 0; i < 26; i++)
        if (mask & (1u << i))
            (void) SeenOf(static_cast<wchar_t>(L'A' + i));
        else
            g_seen[i] = Seen();

    for (size_t i = g_drives.size(); i-- > 0;) {
        const Seen &s = g_seen[g_drives[i].letter - L'A'];

        if (!s.known || (s.device != g_drives[i].device) || !Followed(s.kind))
            RemoveDrive(i);
    }
    for (int i = 0; i < 26; i++) {
        const wchar_t letter = static_cast<wchar_t>(L'A' + i);

        if (g_seen[i].known && Followed(g_seen[i].kind) && (FindDrive(letter) == nullptr))
            AddDrive(letter, g_seen[i], quiet);
    }
}

/* Shift held as a disk goes in skips its autorun.inf, as with AutoRun: a read is started as near to that as the
   program sees a disk. */
void
StartRead(Drive &d)
{
    if (d.reading || d.locked)
        return;
    d.reading = true;
    d.shift   = ShiftDown();
    Queue(Job::Read, d.letter, d.gen);
}

void
Tick()
{
    if ((++g_ticks % kScanEvery) == 0)
        Scan();
    for (auto &d : g_drives) {
        /* A handle given up for a removal that did not happen is taken again. */
        if (d.handle == INVALID_HANDLE_VALUE)
            Register(d);
        if (d.locked || d.checking || d.reading)
            continue;
        /* A volume that another program holds is not asked, as a check reaches the drive and may take its news of
           a disk change from that program, but read again: the read is refused without reaching the disk until
           the program lets the volume go. A drive that nothing watches waits for its next mount. */
        if (d.held) {
            if (Watched(d))
                StartRead(d);
            continue;
        }
        if (!Asked(d))
            continue;
        d.checking = true;
        Queue(Job::Check, d.letter, d.gen);
    }
}

/* Whether the program learns of the drive's disks coming and going: it asks the drive, the drive reports them, or
   Windows' broadcasts tell of them. */
bool
Watched(const Drive &d)
{
    return Asked(d) || d.events || d.reports;
}

/* The disk a read found, as far as a read tells disks apart: by its serial number, autorun.inf and label. */
std::wstring
Identity(const Result &r)
{
    wchar_t id[40];

    swprintf_s(id, L"%08lx %016llx ", r.serial, static_cast<unsigned long long>(r.infHash));
    return id + r.volumeLabel;
}

/* Whether the volume refused to be opened because another program holds it. */
bool
Refused(const DWORD error)
{
    return (error == ERROR_ACCESS_DENIED) || (error == ERROR_SHARING_VIOLATION);
}

/* The volume refused to be opened: another program holds it, which locked it before the drive was followed, as a
   program does that had the drive when it came, so that no notification told of the lock. The drive is left alone
   as under a lock, its key kept as it is, until the program lets the volume go, as a notification or a read that
   is let through tells. */
void
Hold(Drive &d, const DWORD error)
{
    if (!d.held)
        Log(L"%lc: another program holds the volume (error %lu)", d.letter, error);
    d.held = true;
}

void
OnResult(Result *r)
{
    Drive *d      = FindDrive(r->letter);
    bool   launch = false;

    if (d != nullptr) {
        if (r->kind == Job::Check)
            d->checking = false;
        else
            d->reading = false;
        /* The drive started again meanwhile. */
        if (r->gen != d->gen)
            d = nullptr;
    }
    if (d == nullptr) {
        delete r;
        return;
    }

    if (r->kind == Job::Check) {
        switch (r->error) {
            case ERROR_SUCCESS: {
                const bool moved = d->counted && r->counted && (r->changes != d->changes);

                if (r->counted) {
                    d->changes = r->changes;
                    d->counted = true;
                }
                /* Another disk took the disk's place since the last check. */
                if (moved)
                    d->armed = true;
                if ((d->media != 1) || moved)
                    StartRead(*d);
                break;
            }
            case ERROR_MEDIA_CHANGED:
                if (d->media != -1)
                    d->armed = true;
                StartRead(*d);
                break;
            case ERROR_NOT_READY:
            case ERROR_NO_MEDIA_IN_DRIVE:
                /* The next disk goes in while the program watches. */
                d->armed = true;
                d->identity.clear();
                Removed(d->letter);
                if (d->media != 0) {
                    Log(L"%lc: no disk", d->letter);
                    d->media = 0;
                    d->read  = false;
                    Show(*d);
                }
                break;
            case ERROR_ACCESS_DENIED:
            case ERROR_SHARING_VIOLATION:
                Hold(*d, r->error);
                break;
            case ERROR_INVALID_FUNCTION:
            case ERROR_NOT_SUPPORTED:
                d->tells = false;
                Log(L"%lc: the drive does not answer the check, so %ls", d->letter,
                    d->reports ? L"Windows' broadcasts tell of its disks" : L"its disk is read when Windows mounts it");
                break;
            default:
                Log(L"%lc: the check failed (error %lu)", d->letter, r->error);
                break;
        }
    } else {
        /* A refused read mounts nothing, so the mounts after it are not its own. */
        if (!r->disk && Refused(r->error)) {
            Hold(*d, r->error);
            delete r;
            return;
        }
        d->quietUntil = GetTickCount64() + kAfterRead;
        if (d->held) {
            /* The program let the volume go. The disk went in for it, and is not launched; the class driver's
               count of changes, which may have moved meanwhile, is taken afresh. */
            Log(L"%lc: the volume is free again", d->letter);
            d->held    = false;
            d->armed   = false;
            d->counted = false;
        }
        if (r->disk) {
            const std::wstring identity = Identity(*r);

            /* The disk is launched if it went in while the program watched the drive, or, in a drive that tells
               of no disk until Windows mounts it, if it is another than the disk read last. */
            launch          = d->armed || (!Watched(*d) && (identity != d->identity));
            d->armed        = false;
            d->identity     = identity;
            d->media        = 1;
            d->read         = true;
            d->volumeLabel  = r->volumeLabel;
            d->autorunLabel = r->autorunLabel;
            d->autorunIcon  = r->autorunIcon;
            d->fileIcon     = r->fileIcon;
            Log(L"%lc: a disk labelled '%ls'; autorun.inf label '%ls', icon '%ls'; first icon file '%ls'", d->letter,
                r->volumeLabel.c_str(), r->autorunLabel.c_str(), r->autorunIcon.c_str(), r->fileIcon.c_str());
        } else {
            /* Nothing to show: no disk; a disk that Windows cannot read; or, as an empty drive's volume answers
               just after it was let go, a parameter error, which says neither. */
            d->read = false;
            switch (r->error) {
                case ERROR_NOT_READY:
                case ERROR_NO_MEDIA_IN_DRIVE:
                    d->media = 0;
                    d->armed = true;
                    d->identity.clear();
                    Removed(d->letter);
                    break;
                case ERROR_INVALID_PARAMETER:
                    d->media = -1;
                    break;
                default:
                    d->media = 1;
                    d->armed = false;
                    d->identity.clear();
                    break;
            }
            Log(L"%lc: no disk to read (error %lu)", d->letter, r->error);
        }
        Show(*d);
        /* Not while another program wants the volume, which the program would have to read again. */
        if (launch && !d->locked)
            Inserted(d->letter, d->kind, d->shift);
    }
    delete r;
}

/* A mount whose burst has settled. A drive that is asked whether a disk is in is asked now, which mounts
   nothing; any other has its disk read, as a mount is all it tells of a disk, and so has a volume that another
   program holds, which the mount may show let go. */
void
OnMountSettled(Drive &d)
{
    if (d.locked)
        return;
    if (Asked(d) && !d.held) {
        if (!d.checking && !d.reading) {
            d.checking = true;
            Queue(Job::Check, d.letter, d.gen);
        }
    } else
        StartRead(d);
}

/* A disk went into the drive, or out of it, as the drive reports, or Windows' broadcasts tell. */
void
MediaArrived(Drive &d)
{
    d.armed = true;
    StartRead(d);
}

void
MediaRemoved(Drive &d)
{
    d.media = 0;
    d.read  = false;
    d.armed = true;
    d.identity.clear();
    Removed(d.letter);
    Show(d);
}

void
OnVolumeEvent(Drive &d, const GUID &g)
{
    if (g == GUID_IO_VOLUME_LOCK) {
        /* Another program wants the volume, to eject the disk or to use the drive itself: the drive is left alone
           until it is let go, neither asked nor read, and its key kept as it is. */
        if (!d.locked)
            Log(L"%lc: another program wants the volume locked", d.letter);
        d.locked = true;
    } else if (g == GUID_IO_VOLUME_LOCK_FAILED) {
        /* Nothing happened to the volume: what a read found meanwhile is shown now. */
        if (d.locked) {
            Log(L"%lc: the lock was refused", d.letter);
            d.locked = false;
            Show(d);
        }
    } else if (g == GUID_IO_VOLUME_UNLOCK) {
        /* Whatever the program did, the disk may be another now: the drive is asked again, a controller's drive
           is read when its next disk is mounted, and a drive that reports its disks, which nothing asks, is read
           now. A disk that went in meanwhile went in for that program, and is not launched. So it goes even when
           no notification told of the lock, which the program took before the drive was followed. */
        Log(L"%lc: the volume is free again", d.letter);
        d.locked  = false;
        d.held    = false;
        d.counted = false;
        d.gen++;
        d.media = -1;
        d.armed = false;
        if (!IsFloppyKind(d.kind))
            StartRead(d);
    } else if (g == GUID_IO_VOLUME_MOUNT) {
        if (d.reading || (GetTickCount64() < d.quietUntil))
            return;
        /* Each mount of a burst sets the timer again, so the burst is answered once. */
        Log(L"%lc: a disk was mounted", d.letter);
        (void) SetTimer(g_hwnd, kMountTimer + static_cast<UINT>(d.letter - L'A'), kMountSettle, nullptr);
    } else if (g == GUID_IO_VOLUME_DISMOUNT) {
        Log(L"%lc: the disk was dismounted", d.letter);
        /* Only a check, or the next mount, can tell whether the disk is still in. */
        if (!d.tells && !d.locked && !d.held) {
            d.media = -1;
            d.read  = false;
            Show(d);
        }
    } else if (g == GUID_IO_MEDIA_ARRIVAL) {
        if (!d.events)
            Log(L"%lc: the drive reports its disks itself, so it is no longer asked", d.letter);
        d.events = true;
        MediaArrived(d);
    } else if (g == GUID_IO_MEDIA_REMOVAL) {
        d.events = true;
        MediaRemoved(d);
    } else {
        wchar_t s[64] = {};

        (void) StringFromGUID2(g, s, 64);
        Log(L"%lc: event %ls", d.letter, s);
    }
}

Drive *
DriveOfNotification(const DEV_BROADCAST_HDR *h)
{
    if ((h == nullptr) || (h->dbch_devicetype != DBT_DEVTYP_HANDLE))
        return nullptr;
    const DEV_BROADCAST_HANDLE *b = reinterpret_cast<const DEV_BROADCAST_HANDLE *>(h);
    for (auto &d : g_drives)
        if ((d.notify != nullptr) && (d.notify == b->dbch_hdevnotify))
            return &d;
    return nullptr;
}

void
OnDeviceChange(const WPARAM event, const LPARAM lp)
{
    const DEV_BROADCAST_HDR *h = reinterpret_cast<const DEV_BROADCAST_HDR *>(lp);
    Drive                   *d = DriveOfNotification(h);

    switch (event) {
        case DBT_DEVICEARRIVAL:
        case DBT_DEVICEREMOVECOMPLETE:
            /* A drive letter came or went, or a disk went in or out of a drive; the volume broadcasts reach
               top-level windows. A floppy drive's disks are followed on their own. */
            if ((h != nullptr) && (h->dbch_devicetype == DBT_DEVTYP_VOLUME)) {
                const DEV_BROADCAST_VOLUME *v = reinterpret_cast<const DEV_BROADCAST_VOLUME *>(h);

                if (event == DBT_DEVICEREMOVECOMPLETE)
                    for (int i = 0; i < 26; i++)
                        if (v->dbcv_unitmask & (1u << i))
                            Removed(static_cast<wchar_t>(L'A' + i));
                Scan();
                (void) SetTimer(g_hwnd, kScanTimer, kScanAgain, nullptr);
                if (v->dbcv_flags & DBTF_MEDIA)
                    for (int i = 0; i < 26; i++) {
                        Drive *m = FindDrive(static_cast<wchar_t>(L'A' + i));

                        if (!(v->dbcv_unitmask & (1u << i)) || (m == nullptr) || IsFloppyKind(m->kind))
                            continue;
                        if (event == DBT_DEVICEARRIVAL)
                            MediaArrived(*m);
                        else
                            MediaRemoved(*m);
                    }
            } else if (d != nullptr) {
                Unregister(*d);
                Scan();
            }
            break;
        case DBT_DEVICEQUERYREMOVE:
        case DBT_DEVICEREMOVEPENDING:
            /* The drive is being removed, which its handle must not hold up. */
            if (d != nullptr)
                Unregister(*d);
            break;
        case DBT_CUSTOMEVENT:
            if (d != nullptr)
                OnVolumeEvent(*d, reinterpret_cast<const DEV_BROADCAST_HANDLE *>(h)->dbch_eventguid);
            break;
        default:
            break;
    }
}

void
Cleanup()
{
    for (auto &d : g_drives) {
        SetKey(d, L"", L"");
        Unregister(d);
    }
    g_drives.clear();
}

/* Windows' AutoPlay, cancelled for the kinds of drives whose native handling the menu turns off. AutoPlay asks
   before it acts on a disk; COM brings the question to the window's thread, which keeps the program's state. */
class AutoPlayCancel : public IQueryCancelAutoPlay {
public:
    STDMETHODIMP QueryInterface(REFIID iid, void **pv) override
    {
        if ((iid == IID_IUnknown) || (iid == __uuidof(IQueryCancelAutoPlay))) {
            *pv = static_cast<IQueryCancelAutoPlay *>(this);
            return S_OK;
        }
        *pv = nullptr;
        return E_NOINTERFACE;
    }
    /* The one object lives as long as the program: references count for nothing. */
    STDMETHODIMP_(ULONG) AddRef() override { return 2; }
    STDMETHODIMP_(ULONG) Release() override { return 1; }

    STDMETHODIMP AllowAutoPlay(LPCWSTR path, DWORD content, LPCWSTR, DWORD) override
    {
        if ((path == nullptr) || !iswalpha(path[0]) || (path[1] != L':'))
            return S_OK;
        const wchar_t letter = static_cast<wchar_t>(towupper(path[0]));
        const Seen   &s      = SeenOf(letter);
        /* A drive the broadcasts have not brought yet, when its kind is followed. */
        if (Followed(s.kind) && (FindDrive(letter) == nullptr))
            (void) SetTimer(g_hwnd, kScanTimer, 100, nullptr);
        if ((s.kind >= KindCount) || !(g_set.native & (1u << s.kind)))
            return S_OK;
        Log(L"%lc: Windows' AutoPlay cancelled (content 0x%08lx), as its native handling is off for %ls", letter,
            content, kKindNames[s.kind]);
        return S_FALSE;
    }
};

AutoPlayCancel g_cancel;

/* The registration with AutoPlay, while any kind of drives has its native handling off: the object, in the
   running object table under the program's class, and the class, in the per-user CancelAutoplay key, whose
   classes AutoPlay asks. */
void
RegisterCancel(const bool on)
{
    IRunningObjectTable *rot = nullptr;

    if ((on == (g_cancelCookie != 0)) || FAILED(GetRunningObjectTable(0, &rot)))
        return;
    if (on) {
        IMoniker *moniker = nullptr;

        if (SUCCEEDED(CreateClassMoniker(kCancelClass, &moniker))) {
            const HRESULT hr = rot->Register(ROTFLAGS_REGISTRATIONKEEPSALIVE, &g_cancel, moniker, &g_cancelCookie);

            if (FAILED(hr)) {
                g_cancelCookie = 0;
                Log(L"AutoPlay cannot ask the program (error 0x%08lx)", static_cast<unsigned long>(hr));
            }
            moniker->Release();
        }
        if (g_cancelCookie != 0)
            (void) RegSetKeyValueW(HKEY_CURRENT_USER, kCancelList, kCancelValue, REG_SZ, L"", sizeof(wchar_t));
    } else {
        HKEY  key    = nullptr;
        DWORD values = 0;

        (void) rot->Revoke(g_cancelCookie);
        g_cancelCookie = 0;
        (void) RegDeleteKeyValueW(HKEY_CURRENT_USER, kCancelList, kCancelValue);
        /* The keys it made, if nothing else is listed. */
        if (RegOpenKeyExW(HKEY_CURRENT_USER, kCancelList, 0, KEY_QUERY_VALUE, &key) == ERROR_SUCCESS) {
            (void) RegQueryInfoKeyW(key, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, &values, nullptr,
                                    nullptr, nullptr, nullptr);
            RegCloseKey(key);
            if (values == 0) {
                const std::wstring list   = kCancelList;
                const std::wstring parent = list.substr(0, list.rfind(L'\\'));
                DWORD              subs   = 1;

                (void) RegDeleteKeyW(HKEY_CURRENT_USER, kCancelList);
                if (RegOpenKeyExW(HKEY_CURRENT_USER, parent.c_str(), 0, KEY_QUERY_VALUE, &key) == ERROR_SUCCESS) {
                    (void) RegQueryInfoKeyW(key, nullptr, nullptr, nullptr, &subs, nullptr, nullptr, &values, nullptr,
                                            nullptr, nullptr, nullptr);
                    RegCloseKey(key);
                    if ((subs == 0) && (values == 0))
                        (void) RegDeleteKeyW(HKEY_CURRENT_USER, parent.c_str());
                }
            }
        }
    }
    rot->Release();
}

/* The tray icon and its menu. */

void
AddTray()
{
    NOTIFYICONDATAW n = {};

    n.cbSize           = sizeof(n);
    n.hWnd             = g_hwnd;
    n.uID              = kTrayId;
    n.uFlags           = NIF_MESSAGE | NIF_ICON | NIF_TIP | NIF_SHOWTIP;
    n.uCallbackMessage = kTrayMessage;
    n.hIcon            = g_trayIcon;
    wcscpy_s(n.szTip, L"RetroRun");
    (void) Shell_NotifyIconW(NIM_ADD, &n);
    n.uVersion = NOTIFYICON_VERSION_4;
    (void) Shell_NotifyIconW(NIM_SETVERSION, &n);
}

void
RemoveTray()
{
    NOTIFYICONDATAW n = {};

    n.cbSize = sizeof(n);
    n.hWnd   = g_hwnd;
    n.uID    = kTrayId;
    (void) Shell_NotifyIconW(NIM_DELETE, &n);
}

/* Menu text: an ampersand is shown as one. */
std::wstring
MenuText(const std::wstring &s)
{
    std::wstring t;

    for (const wchar_t c : s) {
        if (c == L'&')
            t += L'&';
        t += c;
    }
    return t;
}

std::wstring
Status(const Drive &d)
{
    std::wstring s(1, d.letter);

    s += L":  " + d.model + L": ";
    if (d.foreign)
        s += L"keeps its own name and icon";
    else if (d.locked || d.held)
        s += L"in use by another program";
    else if (d.media == 0)
        s += L"no disk";
    else if ((d.media == 1) && d.read)
        s += d.volumeLabel.empty() ? L"a disk without a label" : (L"disk " + d.volumeLabel);
    else if (d.media == 1)
        s += L"a disk Windows cannot read";
    else if (d.reading)
        s += L"reading";
    else
        s += Watched(d) ? L"looking for a disk" : L"read when a disk is opened";
    return s;
}

/* A kind of drives, with the letters of those there are now, as "SuperDisk drives (A:)". */
std::wstring
KindText(const DriveKind kind)
{
    std::wstring letters;

    for (int i = 0; i < 26; i++)
        if (g_seen[i].known && (g_seen[i].kind == kind))
            letters += std::wstring(letters.empty() ? L"" : L", ") + static_cast<wchar_t>(L'A' + i) + L':';
    return letters.empty() ? std::wstring(kKindNames[kind]) : (std::wstring(kKindNames[kind]) + L" (" + letters + L")");
}

void
Toggle(bool &setting, const wchar_t *name)
{
    setting = !setting;
    WriteSetting(name, setting);
    ShowAll();
}

/* The kinds of drives of one of the submenus, as its item index chose: a kind's own, or all of them, which turns
   them all off when they are all on already. */
void
SetKinds(DWORD &kinds, const wchar_t *name, const wchar_t *what, const UINT index)
{
    std::wstring list;

    if (index >= KindCount)
        kinds = (kinds == kAllKinds) ? 0 : kAllKinds;
    else
        kinds ^= 1u << index;
    WriteSetting(name, kinds);
    for (unsigned k = 0; k < KindCount; k++)
        if (kinds & (1u << k))
            list += std::wstring(list.empty() ? L"" : L", ") + kKindNames[k];
    Log(L"%ls: %ls", what, list.empty() ? L"no drives" : list.c_str());

    /* The drives that are followed now, and what they show; the disks in them are not launched. */
    Scan(true);
    ShowAll();
    RegisterCancel(g_set.native != 0);
}

void
OnCommand(const UINT cmd)
{
    switch (cmd) {
        case IDM_READ:
            for (auto &d : g_drives)
                StartRead(d);
            break;
        case IDM_AUTORUN_ICON:
            Toggle(g_set.autorunIcon, L"AutorunIcon");
            break;
        case IDM_AUTORUN_LABEL:
            Toggle(g_set.autorunLabel, L"AutorunLabel");
            break;
        case IDM_FILE_ICON:
            Toggle(g_set.fileIcon, L"FileIcon");
            break;
        case IDM_VOLUME_LABEL:
            Toggle(g_set.volumeLabel, L"VolumeLabel");
            break;
        case IDM_STARTUP:
            SetStartsWithWindows(!StartsWithWindows());
            break;
        case IDM_EXIT:
            DestroyWindow(g_hwnd);
            break;
        default:
            if ((cmd >= IDM_DISPLAY) && (cmd <= (IDM_DISPLAY + KindCount)))
                SetKinds(g_set.display, L"Display", L"names and icons are shown for", cmd - IDM_DISPLAY);
            else if ((cmd >= IDM_LAUNCH) && (cmd <= (IDM_LAUNCH + KindCount)))
                SetKinds(g_set.launch, L"Launch", L"autorun.inf programs are launched from", cmd - IDM_LAUNCH);
            else if ((cmd >= IDM_NATIVE) && (cmd <= (IDM_NATIVE + KindCount)))
                SetKinds(g_set.native, L"Native", L"native AutoPlay handling is off for", cmd - IDM_NATIVE);
            else if ((cmd >= IDM_LOOK) && (cmd <= (IDM_LOOK + KindCount)))
                SetKinds(g_set.look, L"Look", L"disks are looked for in", cmd - IDM_LOOK);
            break;
    }
}

/* A submenu of the kinds of drives, each with the letters of the drives there are, ticked as kinds has them,
   under the item text; base is its first command. */
void
AppendKinds(const HMENU m, const wchar_t *text, const DWORD kinds, const UINT base, const wchar_t *hint)
{
    const HMENU sub = CreatePopupMenu();

    if (sub == nullptr)
        return;
    (void) AppendMenuW(sub, MF_STRING | ((kinds == kAllKinds) ? MF_CHECKED : 0), base + KindCount,
                       L"All removable disks");
    (void) AppendMenuW(sub, MF_SEPARATOR, 0, nullptr);
    for (unsigned k = 0; k < KindCount; k++)
        (void) AppendMenuW(sub, MF_STRING | ((kinds & (1u << k)) ? MF_CHECKED : 0), base + k,
                           KindText(static_cast<DriveKind>(k)).c_str());
    (void) AppendMenuW(sub, MF_SEPARATOR, 0, nullptr);
    (void) AppendMenuW(sub, MF_STRING | MF_GRAYED, 0, hint);
    (void) AppendMenuW(m, MF_POPUP | ((kinds != 0) ? MF_CHECKED : 0), reinterpret_cast<UINT_PTR>(sub), text);
}

void
ShowMenu(const POINT pt)
{
    const HMENU m = CreatePopupMenu();

    if (m == nullptr)
        return;
    if (g_drives.empty())
        (void) AppendMenuW(m, MF_STRING | MF_GRAYED, 0, L"No drives of the kinds chosen");
    for (const auto &d : g_drives)
        (void) AppendMenuW(m, MF_STRING | MF_GRAYED, 0, MenuText(Status(d)).c_str());
    (void) AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    (void) AppendMenuW(m, MF_STRING | (g_drives.empty() ? MF_GRAYED : 0), IDM_READ, L"&Read the disks now");
    (void) AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendKinds(m, L"Display names and icons", g_set.display, IDM_DISPLAY,
                L"On other drives, Explorer's own label and autorun.inf come first");
    (void) AppendMenuW(m, MF_STRING | (g_set.autorunIcon ? MF_CHECKED : 0), IDM_AUTORUN_ICON, L"Display autorun.inf icon");
    (void) AppendMenuW(m, MF_STRING | (g_set.autorunLabel ? MF_CHECKED : 0), IDM_AUTORUN_LABEL,
                       L"Display autorun.inf label");
    (void) AppendMenuW(m, MF_STRING | (g_set.fileIcon ? MF_CHECKED : 0), IDM_FILE_ICON,
                       L"Display first icon on disk (no autorun.inf)");
    (void) AppendMenuW(m, MF_STRING | (g_set.volumeLabel ? MF_CHECKED : 0), IDM_VOLUME_LABEL,
                       L"Display volume label (no autorun.inf label)");
    (void) AppendMenuW(m, MF_SEPARATOR, 0, nullptr);

    AppendKinds(m, L"Launch autorun.inf applications", g_set.launch, IDM_LAUNCH,
                L"Hold Shift as a disk goes in to skip it");
    AppendKinds(m, L"Disable native AutoPlay handling", g_set.native, IDM_NATIVE, L"While RetroRun runs");
    (void) AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendKinds(m, L"Look for disks", g_set.look, IDM_LOOK, L"Each drive is asked every 2 seconds");
    (void) AppendMenuW(m, MF_STRING | (StartsWithWindows() ? MF_CHECKED : 0), IDM_STARTUP, L"Start with Windows");
    (void) AppendMenuW(m, MF_STRING, IDM_EXIT, L"E&xit");

    /* The menu closes as it should when it is clicked away from only if the window is in front. */
    SetForegroundWindow(g_hwnd);
    const UINT cmd = static_cast<UINT>(
        TrackPopupMenuEx(m, TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_NONOTIFY, pt.x, pt.y, g_hwnd, nullptr));
    (void) PostMessageW(g_hwnd, WM_NULL, 0, 0);
    DestroyMenu(m);
    OnCommand(cmd);
}

LRESULT CALLBACK
WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    if ((g_taskbarCreated != 0) && (msg == g_taskbarCreated)) {
        /* Explorer started again: its tray is new. */
        AddTray();
        return 0;
    }

    switch (msg) {
        case WM_CREATE:
            g_hwnd = hwnd;
            AddTray();
            /* The disks in the drives now went in before the program started, and are not launched. */
            Scan(true);
            RegisterCancel(g_set.native != 0);
            Tick();
            (void) SetTimer(hwnd, kTimerId, g_set.checkMs, nullptr);
            return 0;
        case WM_TIMER:
            if ((wp >= kMountTimer) && (wp < (kMountTimer + 26))) {
                Drive *d = FindDrive(static_cast<wchar_t>(L'A' + (wp - kMountTimer)));

                KillTimer(hwnd, wp);
                if (d != nullptr)
                    OnMountSettled(*d);
            } else if ((wp >= kRefreshTimer) && (wp < (kRefreshTimer + 26))) {
                Drive *d = FindDrive(static_cast<wchar_t>(L'A' + (wp - kRefreshTimer)));

                KillTimer(hwnd, wp);
                if (d != nullptr) {
                    Refresh(d->letter);
                    if (++d->refreshes < _countof(kRefreshGaps))
                        (void) SetTimer(hwnd, wp, kRefreshGaps[d->refreshes], nullptr);
                }
            } else if (wp == kScanTimer) {
                KillTimer(hwnd, wp);
                Scan();
            } else
                Tick();
            return 0;
        case kTrayMessage:
            switch (LOWORD(lp)) {
                case WM_CONTEXTMENU:
                case NIN_SELECT:
                case NIN_KEYSELECT: {
                    const POINT pt = { GET_X_LPARAM(wp), GET_Y_LPARAM(wp) };

                    ShowMenu(pt);
                    break;
                }
                default:
                    break;
            }
            return 0;
        case kResultMessage:
            OnResult(reinterpret_cast<Result *>(lp));
            return 0;
        case WM_DEVICECHANGE:
            OnDeviceChange(wp, lp);
            return TRUE;
        case WM_ENDSESSION:
            /* The drives' usual names back before the session ends, and AutoPlay its own. */
            if (wp) {
                RegisterCancel(false);
                Cleanup();
            }
            return 0;
        case WM_DESTROY:
            KillTimer(hwnd, kTimerId);
            RegisterCancel(false);
            Cleanup();
            RemoveTray();
            Log(L"ended");
            PostQuitMessage(0);
            return 0;
        default:
            return DefWindowProcW(hwnd, msg, wp, lp);
    }
}

/* "/exit": the copy that runs is asked to end, which gives the drives their usual names back, and is waited
   for. For the install and uninstall scripts. */
int
ExitRunning()
{
    const HWND w   = FindWindowW(kClass, nullptr);
    DWORD      pid = 0;

    if (w == nullptr)
        return 0;
    (void) GetWindowThreadProcessId(w, &pid);
    const HANDLE p = OpenProcess(SYNCHRONIZE, FALSE, pid);
    (void) PostMessageW(w, WM_CLOSE, 0, 0);
    if (p != nullptr) {
        (void) WaitForSingleObject(p, 10000);
        CloseHandle(p);
    }
    return 0;
}

} /* namespace */

int WINAPI
wWinMain(HINSTANCE inst, HINSTANCE, PWSTR cmd, int)
{
    WNDCLASSEXW wc = {};
    MSG         msg;

    /* A drive without a disk fails the call instead of asking for one. */
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOOPENFILEERRORBOX);
    if ((cmd != nullptr) && (_wcsicmp(cmd, L"/exit") == 0))
        return ExitRunning();

    const HANDLE running = CreateMutexW(nullptr, FALSE, kMutex);
    if (GetLastError() == ERROR_ALREADY_EXISTS)
        return 0;
    (void) CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

    InitData();
    LoadSettings();
    Log(L"started");
    /* The program's own icon, at the size of the tray's icons on the screen's scale. */
    const UINT dpi = GetDpiForSystem();
    g_trayIcon     = static_cast<HICON>(LoadImageW(inst, MAKEINTRESOURCEW(1), IMAGE_ICON,
                                                   GetSystemMetricsForDpi(SM_CXSMICON, dpi),
                                                   GetSystemMetricsForDpi(SM_CYSMICON, dpi), LR_DEFAULTCOLOR));
    if (g_trayIcon == nullptr)
        g_trayIcon = LoadIconW(nullptr, IDI_APPLICATION);
    InitializeCriticalSection(&g_jobLock);
    g_jobEvent       = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    g_taskbarCreated = RegisterWindowMessageW(L"TaskbarCreated");

    wc.cbSize        = sizeof(wc);
    wc.lpfnWndProc   = WndProc;
    wc.hInstance     = inst;
    wc.lpszClassName = kClass;
    if (!RegisterClassExW(&wc))
        return 1;
    /* A top-level window, never shown, as only those get the volume broadcasts. */
    if (CreateWindowExW(WS_EX_TOOLWINDOW, kClass, L"RetroRun", WS_POPUP, 0, 0, 0, 0, nullptr, nullptr, inst,
                        nullptr) == nullptr)
        return 1;
    g_worker = CreateThread(nullptr, 0, Worker, nullptr, 0, nullptr);

    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    Queue(Job::Quit, 0, 0);
    if (g_worker != nullptr)
        (void) WaitForSingleObject(g_worker, 5000);
    if (running != nullptr)
        CloseHandle(running);
    return 0;
}
