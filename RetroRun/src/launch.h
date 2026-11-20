#pragma once
#include "inf.h"

/* Starting the program a disk's autorun.inf names, as AutoRun did when the disk went in. */

/* The entry that names the program: shellexecute, which may name a document to open with its program, or else
   open, which names a program. entry gets it as it stands, command line and all. None when UseAutoPlay=1 asks
   for AutoPlay instead of AutoRun, which autoplay then tells. */
bool AutorunEntry(const Pairs &keys, std::wstring *entry, bool *autoplay);

/* An entry's command line, as the file to start and its parameters. A quoted name runs to its quote. Otherwise,
   as CreateProcess reads a program that is not quoted, the words up to each space in turn are tried until they
   name a file of the disk, as they stand or as a program with .exe added; and if none does, the first word is
   the file, which ShellExecuteEx then looks for itself, as it does a program of Windows'. A name with no drive
   is of the disk's root ("A:\"), even after a backslash; a name with a drive, or of a share, stands as it is.
   "start document", through which disks of Windows 95's time had Windows' START program open a document, opens
   the document itself, as Windows has had no START program since, unless the disk brings its own. */
void SplitCommand(const std::wstring &root, const std::wstring &entry, std::wstring *file, std::wstring *params);

/* Whether a running program cancels AutoRun for the disk: the window in front, which AutoRun asked with the
   QueryCancelAutoPlay message, or one of the programs registered with AutoPlay to be asked, through the running
   object table (IQueryCancelAutoPlay), other than the one registered under the class own. */
bool AutoRunCancelled(wchar_t letter, const std::wstring &label, DWORD serial, const CLSID &own);

/* Whether Windows' own AutoPlay starts a disc's autorun.inf program in the drive itself: AutoPlay is on, no
   policy keeps it from running autorun.inf programs there, and the choice for software and games is "Install or
   run program from your media". */
bool AutoPlayRunsDiscs(wchar_t letter);

/* Starts the file with its parameters, in the disk's root, as ShellExecuteEx does: a program is run, asking for
   administrator rights if it needs them, and a document is opened with its program; Windows shows its own
   messages if it cannot. Returns the error, or ERROR_SUCCESS. */
DWORD StartFile(const std::wstring &root, const std::wstring &file, const std::wstring &params);
