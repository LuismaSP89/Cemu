# Wii Remotes over Bluetooth passthrough (dedicated USB adapter)

Based on the Wii Remote work in [GamesExObscura/CemuX](https://github.com/GamesExObscura/CemuX) (MPL-2.0).
Windows x64 only for the USB passthrough part.

## What it adds

- Real Wii Remote, MotionPlus and Nunchuk/Classic Controller with hotplug: turning a remote on or off
  reaches the game as a real connect/disconnect (WPAD callbacks), together with the extension type and
  the battery level.
- Automatic placement: a new remote takes the first free controller slot (2..8) without touching
  existing profiles. Optionally, "Automatically configure connected controllers" in Input settings
  switches slots between Pro Controller and Wii Remote depending on what is connected.
- Wii Remote speaker (WPADControlSpeaker / WPADSendStreamData) with a configurable volume.
- Bluetooth passthrough: Cemu talks HCI directly to a dedicated USB adapter (WinUSB through libusb),
  the way the console does. Pairing with the red SYNC button, reconnection with A, bonds kept across
  sessions (encrypted with the Windows user's DPAPI), remote power-off handled.

Without passthrough everything above also works with a Wii Remote paired to the Windows Bluetooth
stack (hidapi), but the passthrough avoids the usual problems of that stack.

## Adapter setup

1. Plug in a classic (BR/EDR) USB Bluetooth adapter that will be dedicated to Cemu. Known to work:
   the ASUS USB-BT600 (VID:PID `0b05:1d70`, the default) and the Wii / Wii U internal Bluetooth module
   wired to USB (`057e:0305`), the same hardware Dolphin's Bluetooth passthrough uses.
2. Find its VID:PID in Device Manager (Properties > Details > Hardware Ids).
3. With [Zadig](https://zadig.akeo.ie/) install the WinUSB driver for that adapter only (not for the
   Bluetooth adapter you use for everything else).
4. In Cemu, Options > Input settings:
   - "Adapter USB ID..." and type the VID:PID (`0b05:1d70` format).
   - Tick "USB Bluetooth passthrough for Wii Remotes". The status line below should say "Adapter ready".
5. Press SYNC (the red button under the battery cover) on each new Wii Remote. Already paired remotes
   reconnect with A. Controller slots are assigned automatically.
   If a remote does not show up (typical of third-party remotes, which answer short scans badly), press
   "Pair Wii Remote (SYNC)" in Input settings and, while the 30 s search runs, SYNC on the remote.
   Some third-party remotes never answer a search and instead call the adapter themselves when a
   button is pressed; those pair by just pressing A (or 1+2), ideally while no search is running.
   If a paired remote rejects its saved key when you press A, Cemu keeps the key (third-party remotes
   sometimes fail transiently and the next A works); it only forgets it once that remote shows up again
   in a SYNC search, and then pairs it from scratch.
6. Paired USB remotes keep their number ("Controller 1", "Controller 2"...) in the order they were first
   paired, even if they reconnect in a different order. Keep each Wii Remote in a single Cemu slot: the
   same "Controller N" in two slots makes the game see it twice and its LEDs show the last channel.
7. When a game says "press HOME to add a Wii Remote" (Mario Party 10, for example), press HOME on that
   Wii Remote: Cemu does not emulate the HOME menu, so the press is turned into a disconnect/reconnect
   event, which is what the game waits for.

For the speaker through a DolphinBar/HID or the USB adapter, enable "Speaker compatibility (3 kHz)" in
the Wii Remote settings before starting the game (lower audio quality on the remote).

Setting `CEMU_USB_BLUETOOTH_TRACE` to an absolute file path writes a session trace of the USB host
(HCI commands/events, inquiry classes, rejected device names; never addresses, keys or HID payloads).

## Where the code is

- `src/input/api/Wiimote/` — rewritten provider (hotplug, MotionPlus, speaker, battery) and `usb/`
  (HCI/L2CAP host over libusb, bonds encrypted with DPAPI).
- `src/Cafe/OS/libs/padscore/` — WPAD/KPAD implementation: connect and extension callbacks, MotionPlus
  (KPADEnableMpls and friends, `MplsProcessing.h`), speaker, sync, battery.
- `src/input/InputManager.*` — automatic Wii Remote placement and the optional Pro/Wiimote mode.
- `src/gui/wxgui/input/` — passthrough checkbox, adapter ID, pairing button and status line, volume and
  speaker compatibility.
- `src/config/CemuConfig.*` — `wiimote_usb_bluetooth`, `wiimote_usb_adapter`, `automatic_controller_setup`.
