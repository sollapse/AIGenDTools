#include "drives.h"
#include <winioctl.h>
#include <string.h>
#include <wchar.h>
#include <wctype.h>

HANDLE
OpenDevice(const wchar_t letter)
{
    wchar_t path[] = L"\\\\.\\A:";

    path[4] = letter;
    return CreateFileW(path, 0, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
}

std::wstring
DeviceOf(const wchar_t letter)
{
    wchar_t name[]           = L"A:";
    wchar_t target[MAX_PATH] = {};

    name[0] = letter;
    return (QueryDosDeviceW(name, target, MAX_PATH) != 0) ? target : L"";
}

/* A removable drive whose device is of the floppy class. */
static bool
IsFloppyDevice(const wchar_t letter, const std::wstring &device)
{
    wchar_t root[] = L"A:\\";

    root[0] = letter;
    return (device.compare(0, 14, L"\\Device\\Floppy") == 0) && (GetDriveTypeW(root) == DRIVE_REMOVABLE);
}

/* Appends one of the device descriptor's strings, without its padding, inside it as well. */
static void
AppendField(std::wstring &model, const BYTE *buf, const DWORD got, const DWORD offset)
{
    const char *s;
    size_t      n;

    if ((offset == 0) || (offset >= got))
        return;
    s = reinterpret_cast<const char *>(buf + offset);
    n = strnlen(s, got - offset);
    while ((n > 0) && (*s == ' ')) {
        s++;
        n--;
    }
    while ((n > 0) && (s[n - 1] == ' '))
        n--;
    if (n == 0)
        return;

    if (!model.empty())
        model += L' ';
    for (size_t i = 0; i < n; i++)
        if ((s[i] != ' ') || (model.back() != L' '))
            model += static_cast<wchar_t>(static_cast<unsigned char>(s[i]));
}

/* The device's descriptor: its vendor and product, and its bus. Returns whether the device has one. */
static bool
Descriptor(const wchar_t letter, std::wstring *model, STORAGE_BUS_TYPE *bus)
{
    const HANDLE           h = OpenDevice(letter);
    STORAGE_PROPERTY_QUERY query;
    BYTE                   buf[1024];
    DWORD                  got      = 0;
    bool                   describe = false;

    model->clear();
    *bus = BusTypeUnknown;
    if (h == INVALID_HANDLE_VALUE)
        return false;
    ZeroMemory(&query, sizeof(query));
    query.PropertyId = StorageDeviceProperty;
    query.QueryType  = PropertyStandardQuery;
    ZeroMemory(buf, sizeof(buf));
    if (DeviceIoControl(h, IOCTL_STORAGE_QUERY_PROPERTY, &query, sizeof(query), buf, sizeof(buf), &got, nullptr) &&
        (got >= sizeof(STORAGE_DEVICE_DESCRIPTOR))) {
        const STORAGE_DEVICE_DESCRIPTOR *d = reinterpret_cast<const STORAGE_DEVICE_DESCRIPTOR *>(buf);

        describe = true;
        *bus     = d->BusType;
        AppendField(*model, buf, got, d->VendorIdOffset);
        AppendField(*model, buf, got, d->ProductIdOffset);
    }
    CloseHandle(h);
    return describe;
}

bool
DescribesItself(const wchar_t letter, std::wstring *model)
{
    STORAGE_BUS_TYPE bus;
    const bool       describe = Descriptor(letter, model, &bus);

    if (model->empty()) {
        const std::wstring device = DeviceOf(letter);

        *model = device.empty() ? L"Floppy drive" : device;
    }
    return describe;
}

DriveKind
FloppyKind(const std::wstring &model)
{
    static const wchar_t *const kNames[] = { L"LS-120", L"LS120", L"LS-240", L"LS240", L"SUPERDISK" };
    std::wstring                upper    = model;

    for (auto &c : upper)
        c = static_cast<wchar_t>(towupper(c));
    for (const wchar_t *n : kNames)
        if (upper.find(n) != std::wstring::npos)
            return KindSuperDisk;
    return KindFloppy;
}

/* A drive that is not a floppy drive, by its type and its device's bus. tells gets whether its disk can change
   and the device describes itself, so that the class driver's check answers whether a disk is in: a hard disk
   has no other disk to tell of. */
static DriveKind
OtherKind(const wchar_t letter, const std::wstring &device, std::wstring *model, bool *tells)
{
    wchar_t          root[] = L"A:\\";
    STORAGE_BUS_TYPE bus    = BusTypeUnknown;

    model->clear();
    root[0] = letter;
    /* A substituted drive is a folder of another drive; a floppy drive is followed on its own. */
    if (device.empty() || (device.compare(0, 4, L"\\??\\") == 0) || (device.compare(0, 14, L"\\Device\\Floppy") == 0))
        return KindNone;
    const UINT type = GetDriveTypeW(root);
    if ((type != DRIVE_REMOVABLE) && (type != DRIVE_CDROM) && (type != DRIVE_FIXED))
        return KindNone;

    const bool described = Descriptor(letter, model, &bus);
    const bool card      = described && ((bus == BusTypeSd) || (bus == BusTypeMmc));
    if (model->empty())
        *model = device;
    *tells = described && (type != DRIVE_FIXED);
    if (type == DRIVE_CDROM)
        return KindOptical;
    if (type == DRIVE_REMOVABLE)
        return card ? KindCard : (described && (bus == BusTypeUsb)) ? KindUsb : KindRemovable;
    /* A fixed disk counts only on a bus whose disks are plugged in and out. */
    if (card)
        return KindCard;
    return (described && ((bus == BusTypeUsb) || (bus == BusType1394))) ? KindExternal : KindNone;
}

DriveKind
KindOf(const wchar_t letter, const std::wstring &device, std::wstring *model, bool *tells)
{
    *tells = false;
    if (IsFloppyDevice(letter, device)) {
        *tells = DescribesItself(letter, model);
        return FloppyKind(*model);
    }
    return OtherKind(letter, device, model, tells);
}
