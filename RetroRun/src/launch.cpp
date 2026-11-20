#include "launch.h"
#include <shellapi.h>
#include <shlobj.h>
#include <shlwapi.h>
#include <shobjidl.h>
#include <wchar.h>

namespace {

const wchar_t kAutoplay[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\AutoplayHandlers";
const wchar_t kCancel[]   = L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\AutoplayHandlers\\CancelAutoplay\\CLSID";
const wchar_t kPolicies[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Policies\\Explorer";

/* A path of the command line: one with a drive, or of a share, as it is; any other of the disk's root. */
std::wstring
OfDisk(const std::wstring &root, const std::wstring &path)
{
    size_t skip = 0;

    if ((path.size() >= 2) && ((path[1] == L':') || ((path[0] == L'\\') && (path[1] == L'\\'))))
        return path;
    while ((skip < path.size()) && ((path[skip] == L'\\') || (path[skip] == L'/')))
        skip++;
    return root + path.substr(skip);
}

bool
IsFile(const std::wstring &path)
{
    const DWORD a = GetFileAttributesW(path.c_str());

    return (a != INVALID_FILE_ATTRIBUTES) && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

/* The name as a file of the disk, as it stands or as a program with .exe added, or nothing if the disk has
   neither. */
std::wstring
FileOfDisk(const std::wstring &root, const std::wstring &name)
{
    if (name.empty())
        return L"";
    const std::wstring path = OfDisk(root, name);
    if (IsFile(path))
        return path;
    if ((*PathFindExtensionW(name.c_str()) == L'\0') && IsFile(path + L".exe"))
        return path + L".exe";
    return L"";
}

DWORD
Policy(const HKEY hive, const wchar_t *name)
{
    DWORD v    = 0;
    DWORD size = sizeof(v);

    return (RegGetValueW(hive, kPolicies, name, RRF_RT_DWORD, nullptr, &v, &size) == ERROR_SUCCESS) ? v : 0;
}

/* The classes of the programs registered with AutoPlay to be asked whether it may run, from the machine's
   CancelAutoplay key and the user's, whose values are named by the classes' IDs; and the shell's own class,
   which programs registered under before. */
void
CancelClasses(std::vector<CLSID> &ids)
{
    ids.push_back(__uuidof(QueryCancelAutoPlay));
    for (const HKEY hive : { HKEY_LOCAL_MACHINE, HKEY_CURRENT_USER }) {
        HKEY key = nullptr;

        if (RegOpenKeyExW(hive, kCancel, 0, KEY_QUERY_VALUE, &key) != ERROR_SUCCESS)
            continue;
        for (DWORD i = 0;; i++) {
            wchar_t     name[80];
            DWORD       n = _countof(name);
            CLSID       id;
            const LONG  s = RegEnumValueW(key, i, name, &n, nullptr, nullptr, nullptr, nullptr);

            if (s == ERROR_NO_MORE_ITEMS)
                break;
            if ((s != ERROR_SUCCESS) || (n == 0))
                continue;
            const std::wstring text = (name[0] == L'{') ? std::wstring(name) : (L"{" + std::wstring(name) + L"}");
            if (SUCCEEDED(CLSIDFromString(text.c_str(), &id)))
                ids.push_back(id);
        }
        RegCloseKey(key);
    }
}

} /* namespace */

bool
AutorunEntry(const Pairs &keys, std::wstring *entry, bool *autoplay)
{
    *autoplay = Trim(Unquote(Value(keys, L"UseAutoPlay"))) == L"1";
    *entry    = Value(keys, L"shellexecute");
    if (entry->empty())
        *entry = Value(keys, L"open");
    return !*autoplay && !entry->empty();
}

void
SplitCommand(const std::wstring &root, const std::wstring &entry, std::wstring *file, std::wstring *params)
{
    const std::wstring e = Trim(entry);

    file->clear();
    params->clear();
    if (e.empty())
        return;

    /* "start document": Windows 95, 98 and Me had a START program, through which disks of their time opened a
       document with its program, as shellexecute does since; Windows has had no such program since, so unless
       the disk brings its own, what follows is split instead, its switches (as /m or /w) dropped. */
    const size_t       space = e.find_first_of(L" \t");
    const std::wstring first = e.substr(0, space);
    if ((space != std::wstring::npos) &&
        ((_wcsicmp(first.c_str(), L"start") == 0) || (_wcsicmp(first.c_str(), L"start.exe") == 0)) &&
        FileOfDisk(root, first).empty()) {
        std::wstring rest = Trim(e.substr(space));

        while (!rest.empty() && (rest[0] == L'/')) {
            const size_t next = rest.find_first_of(L" \t");

            rest = (next == std::wstring::npos) ? L"" : Trim(rest.substr(next));
        }
        SplitCommand(root, rest, file, params);
        return;
    }

    if (e[0] == L'"') {
        const size_t       close = e.find(L'"', 1);
        const std::wstring name  = e.substr(1, ((close == std::wstring::npos) ? e.size() : close) - 1);
        const std::wstring found = FileOfDisk(root, name);

        *file = found.empty() ? name : found;
        if (close != std::wstring::npos)
            *params = Trim(e.substr(close + 1));
        return;
    }

    for (size_t end = e.find_first_of(L" \t");; end = e.find_first_of(L" \t", end + 1)) {
        const std::wstring found = FileOfDisk(root, e.substr(0, end));

        if (!found.empty()) {
            *file = found;
            if (end != std::wstring::npos)
                *params = Trim(e.substr(end));
            return;
        }
        if (end == std::wstring::npos)
            break;
    }
    *file = first;
    if (space != std::wstring::npos)
        *params = Trim(e.substr(space));
}

bool
AutoRunCancelled(const wchar_t letter, const std::wstring &label, const DWORD serial, const CLSID &own)
{
    const UINT           query  = RegisterWindowMessageW(L"QueryCancelAutoPlay");
    const HWND           front  = GetForegroundWindow();
    wchar_t              root[] = L"A:\\";
    DWORD_PTR            answer = 0;
    std::vector<CLSID>   ids;
    IRunningObjectTable *rot    = nullptr;
    bool                 cancel = false;

    root[0] = letter;
    if ((query != 0) && (front != nullptr) &&
        SendMessageTimeoutW(front, query, static_cast<WPARAM>(letter - L'A'), ARCONTENT_AUTORUNINF, SMTO_ABORTIFHUNG,
                            2000, &answer) &&
        (answer != 0))
        return true;

    CancelClasses(ids);
    if (FAILED(GetRunningObjectTable(0, &rot)))
        return false;
    for (const CLSID &id : ids) {
        IMoniker             *moniker = nullptr;
        IUnknown             *object  = nullptr;
        IQueryCancelAutoPlay *asked   = nullptr;

        if ((id == own) || FAILED(CreateClassMoniker(id, &moniker)))
            continue;
        if (rot->GetObject(moniker, &object) == S_OK) {
            if (SUCCEEDED(object->QueryInterface(IID_PPV_ARGS(&asked)))) {
                cancel = asked->AllowAutoPlay(root, ARCONTENT_AUTORUNINF, label.c_str(), serial) == S_FALSE;
                asked->Release();
            }
            object->Release();
        }
        moniker->Release();
        if (cancel)
            break;
    }
    rot->Release();
    return cancel;
}

bool
AutoPlayRunsDiscs(const wchar_t letter)
{
    static const wchar_t *const kChoices[] = { L"\\UserChosenExecuteHandlers\\AutorunINFLegacyArrival",
                                               L"\\EventHandlersDefaultSelection\\AutorunINFLegacyArrival" };
    DWORD   off  = 0;
    DWORD   size = sizeof(off);
    wchar_t handler[64];

    /* The policies that keep Windows from running any autorun.inf program, a disc's, or the drive's. */
    for (const HKEY hive : { HKEY_LOCAL_MACHINE, HKEY_CURRENT_USER })
        if ((Policy(hive, L"NoAutorun") == 1) || (Policy(hive, L"NoDriveTypeAutoRun") & 0x20) ||
            (Policy(hive, L"NoDriveAutoRun") & (1u << (letter - L'A'))))
            return false;
    if ((RegGetValueW(HKEY_CURRENT_USER, kAutoplay, L"DisableAutoplay", RRF_RT_REG_DWORD, nullptr, &off, &size) ==
         ERROR_SUCCESS) &&
        (off != 0))
        return false;
    for (const wchar_t *choice : kChoices) {
        size = sizeof(handler);
        if (RegGetValueW(HKEY_CURRENT_USER, (std::wstring(kAutoplay) + choice).c_str(), nullptr, RRF_RT_REG_SZ, nullptr,
                         handler, &size) == ERROR_SUCCESS)
            return _wcsicmp(handler, L"MSAutoRun") == 0;
    }
    return false;
}

DWORD
StartFile(const std::wstring &root, const std::wstring &file, const std::wstring &params)
{
    SHELLEXECUTEINFOW sei = {};

    if (file.empty())
        return ERROR_FILE_NOT_FOUND;
    sei.cbSize       = sizeof(sei);
    sei.fMask        = SEE_MASK_NOASYNC;
    sei.lpFile       = file.c_str();
    sei.lpParameters = params.empty() ? nullptr : params.c_str();
    sei.lpDirectory  = root.c_str();
    sei.nShow        = SW_SHOWNORMAL;
    if (ShellExecuteExW(&sei))
        return ERROR_SUCCESS;
    const DWORD error = GetLastError();
    return (error != ERROR_SUCCESS) ? error : ERROR_GEN_FAILURE;
}
