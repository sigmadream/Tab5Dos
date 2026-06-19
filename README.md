# TabDOS for M5Stack Tab5

This ESP-IDF example boots a FreeDOS/MS-DOS image from the Tab5 microSD card,
renders PC text and graphics output on the LCD, and forwards USB Host keyboard
reports into the emulator.

## SD card layout

Place boot images under:

```text
/sdcard/dos/
```

Recommended names, in priority order:

```text
fd0.img
a.img
hd20_DOSPROG.img
floppy_freedos.img
freedos.img
hd0.img
```

The first matching `.img` or `.ima` file is selected. Images named `hd*.img`
or larger than 2.88 MB are opened as BIOS hard disk `80h`; smaller images are
opened as floppy `00h`.

## Display orientation

The PC text80 frame is rendered as 640×400, rotated 90° counter-clockwise, and
scaled to 720×1152 on the Tab5 LCD. This uses the full LCD width while preserving
text proportions, leaving only small black margins at the top and bottom.

## Build and flash

```bash
cd examples/Tab5/DOS
idf.py set-target esp32p4
idf.py build flash monitor
```

Optional hardware write smoke automation can be compiled in for one-off
validation. It injects DOS keystrokes after boot and is disabled unless the
environment variable is present during CMake configure:

```bash
TABDOS_AUTOTEST_KEYS=1 idf.py reconfigure build flash monitor
```

## DOS shell prompt note

If a FreeDOS/MS-DOS image stops at `Bad or missing command interpreter` and
`Enter the full shell command line:`, first confirm the boot log says a hard-disk
image was opened as BIOS drive `80`. If the image itself still asks, enter a full
line such as `C:\COMMAND.COM C:\ /P /E:256`, then fix the image's
`CONFIG.SYS`/startup files with the same full shell path and switches.

## Runtime smoke

1. Insert the SD card with `/sdcard/dos/fd0.img` or another bootable image.
2. Connect a USB keyboard to the Tab5 USB-A Host port.
3. Boot the app and wait for the DOS prompt.
4. Type `dir` and press Enter.
5. Create a small file, reboot, and verify that the file remains in the image.

## Monitor evidence

The app emits serial milestones while running:

```text
before PcMachine init heap: internal/8bit=<bytes> psram=<bytes>
after PcMachine init heap: internal/8bit=<bytes> psram=<bytes>
Boot image: /sdcard/dos/<image>.img
USB boot keyboard connected
USB HID keyboard uses report protocol; accepting 8-byte boot-compatible reports
first USB keyboard input report received
TEXT80 milestone: DOS prompt detected
TEXT80 milestone: DIR listing detected
DISK milestone: write count=<n> drive=00 lba=<sector> sectors=<count>
TEXT80 milestone: DOS-created file detected
```

Use these logs with the LCD output to confirm prompt, USB keyboard input, and
write-persistence smoke progress on real hardware. Runtime log is also appended
to `/sdcard/dos/tabdos.log` and flushed with `fsync()` after each line.

Current scope includes text/VGA/Hercules display output, floppy/HDD image I/O,
USB boot or 8-byte boot-compatible HID keyboard input, touch-to-mouse/menu
fallback, and disk write-back.
