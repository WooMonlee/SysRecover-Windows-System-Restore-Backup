#!/bin/sh
# setup-swap.sh - First-boot setup: format sda2 as NTFS, populate ~new + old files.
# This runs instead of zjrestore-lite.sh for the setup boot.
# Invoked by init when zjsetup=1 is on the kernel command line.

say() { echo "SETUP: $*" >&2; }

say "swap drill setup: formatting sda2 as NTFS"

# Load ntfs modules
for m in nls_cp437 nls_iso8859-1 nls_utf8 ntfs; do
    modprobe "$m" 2>/dev/null
done
modprobe ntfs3 2>/dev/null

# Format sda2
if ! mkntfs -F -L ZJSYS /dev/sda2 2>&1; then
    say "mkntfs failed"
    exit 1
fi
say "mkntfs done"

# Mount
mkdir -p /tmp/zjsetup
if mount -t ntfs3 /dev/sda2 /tmp/zjsetup 2>/dev/null; then
    say "mounted via ntfs3"
elif mount -t ntfs-3g /dev/sda2 /tmp/zjsetup 2>/dev/null; then
    say "mounted via ntfs-3g"
else
    say "mount failed"
    exit 1
fi

M=/tmp/zjsetup

# Old system files at root
mkdir -p "$M/Windows/System32"
echo "OLD_KERNEL" > "$M/Windows/System32/ntoskrnl.exe"
echo "OLD_BOOTMGR" > "$M/bootmgr"
echo "old user data" > "$M/old_userfile.txt"

# ~new: new system
mkdir -p "$M/~new/Windows/System32"
echo "NEW_KERNEL" > "$M/~new/Windows/System32/ntoskrnl.exe"
echo "NEW_BOOTMGR" > "$M/~new/bootmgr"
echo "new user data" > "$M/~new/new_userfile.txt"

# .zj-done marker
echo "1024 $(date +%s)" > "$M/~new/.zj-done"

sync
umount "$M"
say "setup done: old system + ~new + .zj-done on /dev/sda2"

# Update menu.lst on sda1 to remove zjsetup=1 (so next boot runs the actual restore)
say "updating menu.lst to remove zjsetup=1"
mkdir -p /tmp/zjboot
if mount -t vfat /dev/sda1 /tmp/zjboot 2>/dev/null; then
    if [ -f /tmp/zjboot/menu.lst ]; then
        sed -i 's/ zjsetup=1//g' /tmp/zjboot/menu.lst
        sync
        say "menu.lst updated"
    fi
    umount /tmp/zjboot
fi

say "SETUP COMPLETE"
