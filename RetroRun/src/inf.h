#pragma once
#include <windows.h>
#include <stdint.h>
#include <string>
#include <utility>
#include <vector>

/* A disk's autorun.inf, read as the shell reads one. */

using Pairs = std::vector<std::pair<std::wstring, std::wstring>>;

/* The file's bytes, if it is no larger than max. Otherwise false, and GetLastError tells why. */
bool ReadSmallFile(const std::wstring &path, DWORD max, std::vector<char> &data);

/* An autorun.inf's text: UTF-16 or UTF-8 with its byte order mark, else the ANSI code page. */
std::wstring Decode(const std::vector<char> &data);

std::wstring Trim(const std::wstring &s);

/* The string without the quotes around it, if it has a pair, as a value of an .ini file is read. */
std::wstring Unquote(const std::wstring &s);

/* The key=value lines of [section]: a value runs to the end of its line, and is kept as it stands, quotes and
   all, since a command line's quotes tell its program from its parameters. Returns whether the section is there
   at all. */
bool IniSection(const std::wstring &text, const wchar_t *section, Pairs &out);

/* The value of the key's first line, or nothing. Keys are matched without regard to case. */
std::wstring Value(const Pairs &pairs, const wchar_t *key);

/* The bytes' FNV-1a hash. */
uint64_t Hash(const std::vector<char> &data);

/* The autorun.inf section of Windows' own architecture, which the shell reads first: "autorun.amd64" on x64
   Windows, whatever this program's own architecture, and nullptr where the shell reads only [autorun]. */
const wchar_t *ArchitectureSection();

/* The keys of an autorun.inf's text: those of the section arch names, which stands alone when the file has one,
   or else of [autorun]. arch may be nullptr. */
Pairs AutorunKeys(const std::wstring &text, const wchar_t *arch);

/* The keys of the autorun.inf in root ("A:\"), as AutorunKeys reads them for Windows' own architecture. hash, if
   given, gets the file's hash. If the file cannot be read, returns false, and error, if given, gets the reason. */
bool ReadAutorun(const std::wstring &root, Pairs &keys, uint64_t *hash, DWORD *error);
