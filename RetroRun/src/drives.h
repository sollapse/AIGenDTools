#pragma once
#include <windows.h>
#include <string>

/* The kinds of drives the tray menu chooses among, in its order. */
enum DriveKind : unsigned {
    KindFloppy,    /* a floppy drive, a floppy controller's or a USB one */
    KindSuperDisk, /* a SuperDisk drive, LS-120 or LS-240 */
    KindUsb,       /* a USB drive of removable disks: a flash drive, a card reader, a Zip drive */
    KindCard,      /* a reader of memory cards on the SD or MMC bus */
    KindRemovable, /* another drive of removable disks, as an ATAPI Zip drive or a magneto-optical one */
    KindOptical,   /* a CD, DVD or Blu-ray drive, or a mounted disc image */
    KindExternal,  /* a hard disk on USB or FireWire */
    KindCount,
    KindNone = KindCount
};

/* A floppy drive, a floppy controller's, a USB one or a SuperDisk drive, which Explorer reads nothing from. */
inline bool
IsFloppyKind(const DriveKind kind)
{
    return (kind == KindFloppy) || (kind == KindSuperDisk);
}

/* The device a drive letter names, as \Device\HarddiskVolume3, or nothing. */
std::wstring DeviceOf(wchar_t letter);

/* The kind of the letter's drive, whose device is device (as DeviceOf gives it), or KindNone for a drive the menu
   does not list: an internal hard disk, a network drive, a substituted one. A floppy drive is one Explorer treats
   as one: a removable drive whose device is of the floppy class, which the floppy class drivers name
   \Device\Floppy<n> (flpydisk for a floppy controller's drives, sfloppy for ATAPI and USB ones, SuperDisk drives
   among them); any other is sorted by its type and its device's bus. model gets the device's vendor and product,
   or else its device name; tells, whether the class driver's check can tell whether a disk is in: for a floppy
   drive, whether it describes itself (DescribesItself); for any other, whether it does and its disk can change,
   which a hard disk's cannot. Nothing is
   read from a disk. */
DriveKind KindOf(wchar_t letter, const std::wstring &device, std::wstring *model, bool *tells);

/* Whether the drive's device describes itself, as an ATAPI or USB drive does. Such a drive answers the class
   driver's check for a disk from its own state; a floppy controller's drive says nothing of itself, and can only
   tell whether its disk was changed by running its motor. model gets the drive's vendor and product, or else
   its device name. */
bool DescribesItself(wchar_t letter, std::wstring *model);

/* A floppy drive's kind, by its model: a SuperDisk drive names itself LS-120 or LS-240. */
DriveKind FloppyKind(const std::wstring &model);

/* A handle on the drive's device with no access: the open goes to the device itself, not to a file system on it,
   so it mounts nothing, reads nothing and keeps no one from locking the volume. Enough for the class driver's
   check and for the device's notifications. */
HANDLE OpenDevice(wchar_t letter);
