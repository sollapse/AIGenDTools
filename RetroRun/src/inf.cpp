#include "inf.h"
#include <string.h>
#include <wchar.h>

namespace {

constexpr DWORD kInfMax = 64u << 10; /* An autorun.inf is read up to this size. */

} /* namespace */

bool
ReadSmallFile(const std::wstring &path, const DWORD max, std::vector<char> &data)
{
    const HANDLE  h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                                  FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    LARGE_INTEGER size;
    DWORD         got   = 0;
    DWORD         error = ERROR_SUCCESS;
    bool          ok;

    if (h == INVALID_HANDLE_VALUE)
        return false;
    ok = GetFileSizeEx(h, &size) && (size.QuadPart >= 0);
    if (!ok)
        error = GetLastError();
    else if (size.QuadPart > static_cast<LONGLONG>(max)) {
        ok    = false;
        error = ERROR_FILE_TOO_LARGE;
    } else {
        data.resize(static_cast<size_t>(size.QuadPart));
        ok = data.empty() ||
             (ReadFile(h, data.data(), static_cast<DWORD>(data.size()), &got, nullptr) && (got == data.size()));
        if (!ok)
            error = (GetLastError() != ERROR_SUCCESS) ? GetLastError() : ERROR_HANDLE_EOF;
    }
    CloseHandle(h);
    if (!ok)
        SetLastError(error);
    return ok;
}

std::wstring
Decode(const std::vector<char> &data)
{
    const unsigned char *b    = reinterpret_cast<const unsigned char *>(data.data());
    const size_t         n    = data.size();
    UINT                 cp   = CP_ACP;
    size_t               skip = 0;

    if ((n >= 2) && (b[0] == 0xff) && (b[1] == 0xfe)) {
        std::wstring s((n - 2) / 2, L'\0');

        memcpy(s.data(), b + 2, s.size() * sizeof(wchar_t));
        return s;
    }
    if ((n >= 3) && (b[0] == 0xef) && (b[1] == 0xbb) && (b[2] == 0xbf)) {
        cp   = CP_UTF8;
        skip = 3;
    }
    const int len = MultiByteToWideChar(cp, 0, data.data() + skip, static_cast<int>(n - skip), nullptr, 0);
    std::wstring s(static_cast<size_t>((len > 0) ? len : 0), L'\0');
    if (len > 0)
        (void) MultiByteToWideChar(cp, 0, data.data() + skip, static_cast<int>(n - skip), s.data(), len);
    return s;
}

std::wstring
Trim(const std::wstring &s)
{
    const size_t a = s.find_first_not_of(L" \t");

    if (a == std::wstring::npos)
        return L"";
    return s.substr(a, s.find_last_not_of(L" \t") - a + 1);
}

std::wstring
Unquote(const std::wstring &s)
{
    if ((s.size() >= 2) && (s.front() == L'"') && (s.back() == L'"'))
        return s.substr(1, s.size() - 2);
    return s;
}

bool
IniSection(const std::wstring &text, const wchar_t *section, Pairs &out)
{
    bool   in    = false;
    bool   found = false;
    size_t pos   = 0;

    while (pos < text.size()) {
        size_t end = text.find_first_of(L"\r\n", pos);

        if (end == std::wstring::npos)
            end = text.size();
        const std::wstring line = Trim(text.substr(pos, end - pos));
        pos                     = end + 1;
        if (line.empty() || (line[0] == L';'))
            continue;
        if (line[0] == L'[') {
            const size_t close = line.find(L']');

            in    = _wcsicmp(Trim(line.substr(1, ((close == std::wstring::npos) ? line.size() : close) - 1)).c_str(),
                             section) == 0;
            found = found || in;
            continue;
        }
        const size_t eq = line.find(L'=');
        if (in && (eq != std::wstring::npos))
            out.emplace_back(Trim(line.substr(0, eq)), Trim(line.substr(eq + 1)));
    }
    return found;
}

std::wstring
Value(const Pairs &pairs, const wchar_t *key)
{
    for (const auto &p : pairs)
        if (_wcsicmp(p.first.c_str(), key) == 0)
            return p.second;
    return L"";
}

uint64_t
Hash(const std::vector<char> &data)
{
    uint64_t h = 1469598103934665603ull;

    for (const char c : data) {
        h ^= static_cast<unsigned char>(c);
        h *= 1099511628211ull;
    }
    return h;
}

const wchar_t *
ArchitectureSection()
{
    SYSTEM_INFO info;

    /* The system's own architecture, which a 32-bit program on 64-bit Windows does not share. */
    GetNativeSystemInfo(&info);
    return (info.wProcessorArchitecture == PROCESSOR_ARCHITECTURE_AMD64) ? L"autorun.amd64" : nullptr;
}

Pairs
AutorunKeys(const std::wstring &text, const wchar_t *arch)
{
    Pairs keys;

    if ((arch == nullptr) || !IniSection(text, arch, keys))
        (void) IniSection(text, L"autorun", keys);
    return keys;
}

bool
ReadAutorun(const std::wstring &root, Pairs &keys, uint64_t *hash, DWORD *error)
{
    std::vector<char> data;

    keys.clear();
    if (!ReadSmallFile(root + L"autorun.inf", kInfMax, data)) {
        if (error != nullptr)
            *error = GetLastError();
        return false;
    }
    if (hash != nullptr)
        *hash = Hash(data);
    keys = AutorunKeys(Decode(data), ArchitectureSection());
    return true;
}
