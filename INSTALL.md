# Pico Neo 3 Pro Eye: Tobii gaze + EyeTrackVR eyelids, install guide

Gets the Pico Neo 3 Pro Eye's eye tracking into VRChat through **VRCFaceTracking**, with
**gaze from the headset's own Tobii tracker** and **smooth eyelids from the EyeTrackVR app**, which reads the
headset's infrared eye cameras. It keeps working while the headset streams PC VR. No root and no headset app.

```
headset: eyestream (gaze) --Wi-Fi UDP 7777----------------------------------> PC: VRCFT module
headset: eyecam (eye images :8080/:8081) --> PC: EyeTrackVR --OSC UDP 9010--> "Pico Neo 3 Eye + ETVR lids" --> VRChat
```

**What you get:** gaze at about 90 Hz, and per-eye eyelids (blinks, winks, half-closed). If EyeTrackVR stops,
the eyelids fall back to Pico's open/closed blinks.
**What you don't get:** separate gaze for each eye (both eyes use the combined gaze).

If you only want gaze and blinks, use the simpler gaze-only version (pico_eye_stream) instead.

---

## What you need

### Hardware
- Pico **Neo 3 Pro Eye** (the "Eye" model; tested on system 5.11.3, Android 10), with **eye tracking switched on** in its settings
- Windows 10/11 PC on the **same network** as the headset (PC wired, headset on Wi-Fi of the same router is fine)
- A USB-C **data** cable (some cables only charge)

### Software: required

| Software | Where | Notes |
|---|---|---|
| **Android SDK Platform-Tools** (adb) | https://developer.android.com/tools/releases/platform-tools | Unzip to `C:\platform-tools`. Talks to the headset. |
| **VRCFaceTracking 6** | Steam (search "VRCFaceTracking") | **Steam version only.** The Microsoft Store version (5.x) crashes on this module. |
| **.NET 10 Desktop Runtime** | https://dotnet.microsoft.com/download/dotnet/10.0 | Only if VRCFT won't start; it needs this runtime. |
| **EyeTrackVR app** | https://github.com/EyeTrackVR/EyeTrackApp/releases | Tested: 0.3.0 beta 10. Turns the eye images into eyelid values. |
| **SteamVR** | Steam | |
| **PC streaming for the headset**, one of: | | PC and headset versions must match. |
| • **PICO Business Streaming** | Pico enterprise downloads | Tested: PC 2.0.0 with headset app 2.0.0 (Wi-Fi). |
| • **PICO Link** | Pico enterprise downloads | DisplayPort cable mode. Tested pair: PC 1.2.9.1 with headset 1.2.9. |
| **VRChat** + an avatar with VRCFT eye parameters | Steam | |
| **Windows PowerShell 5.1** | Built into Windows | Runs the `.cmd` launchers. Nothing to install. |

On the headset nothing gets installed from a store. The two small programs (`eyestream`, `eyecam`) are
copied over adb by the start script.

### Software: optional

| Software | Needed for |
|---|---|
| **Python 3** | `pc\osc_sniff.py` (see what EyeTrackVR sends), `pc\pico_eye_receiver.py` (gaze readout without VRCFT) |

### Software: only to rebuild from source

| Software | Needed for |
|---|---|
| **Android NDK r27c** (https://developer.android.com/ndk/downloads) | `headset\eyestream.c`, `headset\eyecam.c` |
| **.NET 10 SDK** (https://dotnet.microsoft.com/download/dotnet/10.0) | `vrcft\PicoNeo3EyeEtvrModule.cs` |
| `third_party\stb_image_write.h` | Included (public domain JPEG encoder used by eyecam) |

Keep this folder together. The launchers find everything relative to themselves.

---

## Part 1: one-time setup

### 0. Unpack the folder

1. Before extracting the zip, right-click it → **Properties** → tick **Unblock** (if shown) → OK.
   Otherwise Windows may warn about the `.cmd` files every time.
2. Extract it somewhere permanent. Keep the folder together.

### 1. Android platform-tools (adb) on the PC

1. Download **SDK Platform-Tools for Windows** from Google:
   https://developer.android.com/tools/releases/platform-tools
2. Unzip it so you have `C:\platform-tools\adb.exe`.
   (A `platform-tools` folder inside this project folder works too, as does adb on your PATH.)

### 2. Developer mode on the headset

1. On the headset: **Settings → General → About**, then click **Software Version** about 7 times until developer mode turns on.
2. **Settings → Developer**: turn on **USB debugging**.
3. Run the headset's **eye tracking calibration** (Settings → eye tracking / "Eye Calibration").
   Without it the gaze data comes back invalid.

### 3. First USB connection

1. Plug the headset into the PC with the data cable.
2. In the headset, accept **"Allow USB debugging?"** and tick **Always allow from this computer**.
3. Check it on the PC:
   ```
   C:\platform-tools\adb.exe devices
   ```
   You should see a line ending in `device`. If it says `unauthorized`, accept the prompt in the headset.

### 4. Windows Firewall rule for VRCFT

VRCFT's module process has to receive UDP from the headset. Open **PowerShell as Administrator** and run:
```
New-NetFirewallRule -DisplayName "VRCFT Pico Neo 3 eye (UDP 7777)" -Direction Inbound -Program "C:\Program Files (x86)\Steam\steamapps\common\VRCFaceTracking\VRCFaceTracking.ModuleProcess.exe" -Protocol UDP -LocalPort 7777 -Profile Private -Action Allow
```
(Change the path if Steam is installed elsewhere.) Your network must be set to **Private** in Windows.
EyeTrackVR talks to the module on the same PC (127.0.0.1), so that part needs no rule.

### 5. Install the VRCFT module

1. **Close VRCFaceTracking.**
2. Double-click **`Install VRCFT Module.cmd`**. It installs **Pico Neo 3 Eye + ETVR lids** and, if the gaze-only
   "Pico Neo 3 Pro Eye" module is installed, sets it aside (both use port 7777). `-Remove` puts it back.
3. If it lists other eye modules (ETVR, Baballonia, Pico 4...), remove or disable them in VRCFT's
   **Module Registry**. VRCFT gives the eyes to whichever eye module loads first, and the ETVR module
   would take EyeTrackVR's data for itself.

### 6. Set up EyeTrackVR

1. Run **`Start Eye Tracking + Cameras.cmd`** once (Part 2) so the headset's address is known.
2. Double-click **`Set Up EyeTrackVR.cmd`** (run it as Administrator if it can't write the settings). It closes
   EyeTrackVR, backs up its settings, and sets:
   - left camera `http://<headset-ip>:8080/`, right camera `http://<headset-ip>:8081/`
   - OSC port **9010** (the module; VRChat no longer gets ETVR's data directly)
   - **VRCFT v2** output on, **pyVRCFT** off (pyVRCFT only sends one eyelid value for both eyes)
3. Start EyeTrackVR, put the headset on, and run its **calibration** for both eyes so the lids open and close fully.

### 7. Optional: desktop shortcut and voice output

- Make a shortcut on your desktop to **`Start Eye Tracking + Cameras.cmd`**.
- The start script speaks its result on the default audio output. To send it to a specific device (for
  example your headphones), put part of that device's name in `voice_device.txt`, such as `Stealth 600`.
  Delete the file to use the default.

---

## Part 2: every session

1. Turn on the headset.
   **If it was rebooted, plug in USB first**, because adb over Wi-Fi usually switches off on reboot.
2. Double-click **`Start Eye Tracking + Cameras.cmd`**.
   - It finds the headset, switches adb to Wi-Fi, starts the gaze sender and the eye cameras, and opens a window
     with the camera addresses ("no picture yet" there just means the headset is asleep).
   - It **speaks** the result. "You can unplug the USB cable" means it's running.
3. Start your PC streaming (Business Streaming / PICO Link), **EyeTrackVR**, and **VRCFaceTracking from Steam**.
4. Start VRChat. Use an avatar with eye tracking (VRCFT parameters) and check in a mirror.

The programs keep running until the headset turns off. They pause while the headset sleeps and resume when it wakes.
To stop them early, run **`Stop Eye Tracking.cmd`**.

---

## Troubleshooting

| Problem | Fix |
|---|---|
| "ADB was not found" | Install platform-tools (Part 1, step 1). |
| "Headset not found" / "Can't reach the headset over Wi-Fi" | Plug in USB and run Start again. Check the "Allow USB debugging" prompt in the headset. |
| "The headset is asking for permission" | Put the headset on and accept **Allow USB debugging**, then run Start again. |
| VRCFT shows the module but the eyes don't move | Firewall rule missing (Part 1, step 4). Or another eye module grabbed the eye slot: check VRCFT's **Output** page for `Tracking initialized via ...`. |
| Eyes move but the lids only snap open/closed | EyeTrackVR isn't sending: VRCFT's Output says `no ETVR eyelids, using Pico blinks`. Check it's running, run `Set Up EyeTrackVR.cmd` again, and check with `python pc\osc_sniff.py 9010` (VRCFT closed) that `/avatar/parameters/v2/EyeLidLeft` arrives. |
| One eye stops in EyeTrackVR, its log says `Capture source ... not found` | EyeTrackVR doesn't recover after a camera stream drops. **Restart EyeTrackVR.** |
| Eyes never look fully open / half-closed looks too closed | Rerun EyeTrackVR's calibration. The module treats an ETVR value of 0.8 as fully open (`LidOpenAt` in `vrcft\PicoNeo3EyeEtvrModule.cs`). |
| Sender log says `no new frames` while the headset is awake | Pico's eye service got stuck. Reboot the headset, plug in USB, run Start again. |
| Headset loses head tracking with the cameras running | Seen once. Before rebooting, run `pc\watch_headset.ps1` (or keep it running) and report its folder. |
| VRCFT crashes on its module page | You're running the **Microsoft Store** VRCFT 5.x. Use the Steam version. |
| PC streaming can't find the headset | Disable extra network adapters (VPNs like Hamachi, second NICs) in `ncpa.cpl`. |

To check things by hand:
```
C:\platform-tools\adb.exe shell "tail -3 /data/local/tmp/eyestream.log"
C:\platform-tools\adb.exe shell "grep fps /data/local/tmp/eyecam.log | tail -3"
```
`90 pkt/s, 2600 valid` per 30 s means gaze is tracking (`0 valid`: the headset isn't on your face).
The eyecam line shows the camera frame rate, its current limit and the headset's load.

### Reporting back (testers)

If something doesn't work, send these to whoever gave you this folder:

1. What you did and what happened (or a photo of the Start window).
2. The headset logs:
   ```
   C:\platform-tools\adb.exe shell "cat /data/local/tmp/eyestream.log; tail -50 /data/local/tmp/eyecam.log"
   ```
3. VRCFaceTracking's **Output** page (copy all), especially the lines with `Tracking initialized` and `Pico`.
4. The newest EyeTrackVR log (`C:\Program Files (x86)\EyeTrackVR\logs`).
5. Headset system version (Settings → General → About) and your streaming app and version.

---

## Uninstall

- `powershell -ExecutionPolicy Bypass -File install_vrcft_module.ps1 -Remove` (with VRCFT closed)
- EyeTrackVR: restore the backup `eyetrack_settings.json.before_pico_*` next to its settings, or set its OSC port back to 9000.
- Run `Stop Eye Tracking.cmd`. To remove the programs from the headset:
  ```
  C:\platform-tools\adb.exe shell rm /data/local/tmp/eyestream /data/local/tmp/eyecam
  ```
- Remove the firewall rule: `Remove-NetFirewallRule -DisplayName "VRCFT Pico Neo 3 eye (UDP 7777)"` (admin)

---

## Building from source

Only needed if you change the code. Prebuilt files ship in `headset\` and `vrcft\dist\`.

- **Headset programs** (Android NDK r27c):
  ```
  <ndk>\toolchains\llvm\prebuilt\windows-x86_64\bin\aarch64-linux-android29-clang.cmd -O2 -o headset\eyestream headset\eyestream.c -ldl
  <ndk>\toolchains\llvm\prebuilt\windows-x86_64\bin\aarch64-linux-android29-clang.cmd -O2 -o headset\eyecam headset\eyecam.c -lm
  ```
  The start script compares checksums and copies changed programs to the headset.
- **VRCFT module** (.NET 10 SDK, VRCFT from Steam installed):
  ```
  dotnet build vrcft\PicoNeo3EyeEtvrModule.csproj -c Release -o vrcft\dist
  ```
  Then run `Install VRCFT Module.cmd`.

How it works and the data layout are in [README.md](README.md).
