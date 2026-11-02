# Pico Neo 3 Pro Eye: Tobii gaze + EyeTrackVR eyelids (VRCFaceTracking)

**Setting this up? Follow [INSTALL.md](INSTALL.md).** This file explains how it works.
MIT licensed, see [LICENSE](LICENSE). Not affiliated with Pico, Tobii or EyeTrackVR.

This is the hybrid version of [pico_eye_stream](https://github.com/ivyyork5224) (gaze only). It keeps the
headset's own Tobii gaze, which is accurate and works while PC streaming is in front, and takes the eyelids from the
**EyeTrackVR** app reading the headset's infrared eye cameras. Pico itself only reports eyes as open or closed;
ETVR gives smooth, per-eye lids (half-closed, slow blinks).

```
headset: eyestream (Tobii gaze, ~90 Hz) --Wi-Fi UDP 7777--------------------------------> PC: VRCFT module
headset: eyecam (IR eye images, MJPEG :8080/:8081) --> PC: EyeTrackVR app --OSC UDP 9010--> "Pico Neo 3 Eye + ETVR lids" --> VRChat
```

- `eyestream` runs as a plain adb shell program. It talks to Pico's eye tracking system service
  (`pxreyetrackingservice`) through the public library `libeyetrackingclient.pxr.so`. No APK, no root.
- `eyecam` reads the infrared eye images from Pico's debug image dump (`debug.pxr.et.tobii.dumpimg`, 416×200 8-bit,
  both eyes side by side). It deletes each dump file immediately, serves each eye as MJPEG on ports 8080 / 8081, and
  switches the dump off on exit. It only encodes the eyes someone is watching, and lowers its frame rate (90 down to
  30 fps) when the headset's load average goes above 9, so it gives way to head tracking.
- The VRCFT module uses the Tobii gaze for both eyes and ETVR's `/avatar/parameters/v2/EyeLidLeft` and
  `EyeLidRight` for openness (an ETVR value of 0.8 or more counts as fully open). If ETVR stops sending for half a
  second, it falls back to Pico's own open/closed blinks (debounced and eased), so the eyes never freeze.

## Files

| path | what |
|---|---|
| `INSTALL.md` | install guide |
| `Start Eye Tracking + Cameras.cmd` | start gaze tracking and the eye camera streams (runs `start_eyestream.ps1 -Cameras`) |
| `Start Eye Tracking.cmd` | gaze only (eyelids then fall back to Pico's open/closed) |
| `Stop Eye Tracking.cmd` | stop both |
| `Show Eye Camera Addresses.cmd` | camera URLs with Copy buttons (`show_eye_addresses.ps1`) |
| `Install VRCFT Module.cmd` | install / update the VRCFT module (`install_vrcft_module.ps1`) |
| `Set Up EyeTrackVR.cmd` | points EyeTrackVR at the cameras and the module's port (`setup_etvr.ps1`) |
| `find_adb.ps1` | finds adb.exe for the scripts |
| `voice_device.txt` | optional: part of an audio device name for the spoken messages (e.g. `Stealth 600`) |
| `headset/eyestream`, `headset/eyestream.c` | gaze sender for the headset (prebuilt arm64 + source) |
| `headset/eyecam`, `headset/eyecam.c` | eye camera MJPEG server for the headset (prebuilt + source) |
| `vrcft/dist/PicoNeo3EyeEtvrModule.dll`, `vrcft/*.cs*` | VRCFT 6 module (prebuilt + source) |
| `pc/osc_sniff.py` | prints the OSC addresses and value ranges arriving on a port (check what ETVR sends) |
| `pc/watch_headset.ps1` | records the headset's camera / tracking errors and load, for chasing head tracking drops |
| `pc/pico_eye_receiver.py` | standalone gaze receiver: live readout, CSV logging (don't run with VRCFT; both use UDP 7777) |
| `probe/eyeprobe.c` | raw dump tool used to map the data layout |
| `third_party/stb_image_write.h` | public-domain JPEG encoder used by eyecam |

## Gaze packet (UDP 7777, ASCII)

`PEYE,seq,time_s,ok,comb_status,l_status,r_status,gx,gy,gz,px,py,pz,l_open,r_open`
The gaze direction `g*` and origin `p*` are in Unity axes (+x right, +y up, +z forward). Openness is 0 or 1.

## Tobii data layout (`GetEyeTrackingData_2_0`, 0xC8 bytes)

C API in `libeyetrackingclient.pxr.so`: `CreateClient()`, `StartEyeTracking(c)`, `StopEyeTracking(c)`,
`GetEyeTrackingData_2_0(c, long, int, out*)`. It returns 0 ok, -1 not connected, -2 shared memory failed, -3 no data,
and -7 from Start while the headset sleeps.

| offset | field |
|---|---|
| 0x00 / 0x04 / 0x08 | left / right / combined status bitmasks (0x34 / 0x34 / 0x43 when tracking) |
| 0x24 | combined gaze origin xyz (m) |
| 0x48 | combined gaze direction xyz, unit, **x right, y up, -z forward** |
| 0x54 / 0x58 | left / right openness (only 0 or 1) |
| 0x64 / 0x70 | left / right position guide xy |
| 0x7c | foveated gaze direction xyz |
| 0x8c | convergence distance |
| 0xa8 | int64 timestamp |
| 0xb0 / 0xb8 | left / right pupil position xy (camera image) |

## Known issues

- **ETVR doesn't recover when a camera stream drops** (eyecam restarted, Wi-Fi hiccup). It keeps logging
  `Capture source ... not found` and reconnects dozens of times a second, which also loads the headset.
  Restart the EyeTrackVR app.
- **Pico's eye service can get stuck** if the sender is restarted while an old copy is still stopping, or after
  hours of image dumping. It reports "started" but gives no new frames. The start script guards against double
  starts and detects this; the fix is a headset reboot.
- **Head tracking loss** was seen once with the cameras running. The head tracking cameras share the camera system
  with the eye cameras. Cause unconfirmed; `pc/watch_headset.ps1` records the evidence.

Note this was made by AI
