# Dio — first-time setup

Getting a brand-new CoreS3 from box to talking. About 20 minutes.

For the full reference — every setting, the karaoke format, the landmines — see
[README.md](README.md).

---

## Before you start: Dio is a body, not a brain

**Dio does not work on her own.** She records your voice, sends it to the
[**Ph3b3**](https://github.com/astroson111/ph3b3) server for transcription and
reasoning, and plays back the reply. No Ph3b3 server on your network means no
speech, no answers — just a face.

Get Ph3b3 running first and note three things:

| You need | Looks like | Where to find it |
|---|---|---|
| Server IP | `192.168.0.16` | the machine running Ph3b3 |
| Port | `7331` | the default |
| Login | user + password | what you set during Ph3b3's first-run setup |

Check it answers before you touch the robot — **note the `https://`, it is not
optional** (plain `http://` returns nothing and looks like a firewall block):

```bash
curl -k -u <user>:<pass> https://192.168.0.16:7331/health
```

## What you need

**Hardware**
- M5Stack CoreS3
- Stack-Chan servo base (X/Y neck)
- USB-C cable, **and a real power source** — see [Power](#power-read-this) below
- Optional: FAT32 microSD for karaoke, Si12T head-pat sensor

**Software**
- [`arduino-cli`](https://arduino.github.io/arduino-cli/) with the M5Stack ESP32 core
- `esptool`

---

## 1. Your credentials

```bash
cp Ph3b3-Chan/secrets.example.h Ph3b3-Chan/secrets.h
```

Edit it with your server details. This file is gitignored — it never gets
committed, and you should never put real credentials anywhere else:

```c
#define SC_PH3B3_HOST "192.168.0.16"
#define SC_PH3B3_PORT 7331
#define SC_PH3B3_USER "your-user"
#define SC_PH3B3_PASS "your-pass"
```

These are a **first-boot seed only**. Once she boots, the live values live in
her flash and are edited from her setup portal — so you don't reflash to move
her to a new server.

## 2. Build

```bash
arduino-cli compile --fqbn m5stack:esp32:m5stack_cores3 Ph3b3-Chan
```

A correct build is **~1.49 MB**. If you get roughly 1.0 MB, something built the
wrong target — see [Never PlatformIO](README.md#landmines-dont-trip).

## 3. Flash

**Confirm which device you're writing to first.** Dio's USB serial is silent and
her splash reads "Ph3b3", so she is easy to confuse with an Iris combadge — both
are ESP32-S3 and both appear as `/dev/ttyACM*`. The MAC is the only reliable
tell:

```bash
esptool --port /dev/ttyACM0 read-mac
```

Then flash. These offsets write **only** the app — your saved WiFi and server
settings survive:

```bash
esptool --chip esp32s3 --port /dev/ttyACM0 --baud 921600 write-flash \
  0xe000  ~/.arduino15/packages/m5stack/hardware/esp32/<ver>/tools/partitions/boot_app0.bin \
  0x10000 Ph3b3-Chan/build/Ph3b3-Chan.ino.bin
```

## 4. First boot — get her on WiFi

She homes her servos, then finds no saved network and opens her setup portal.
Her screen will read:

```
        WiFi Setup

  On your phone, join WiFi:
        Dio-Setup

   then open in a browser:
       192.168.4.1
```

On your phone: join **`Dio-Setup`**, open **`192.168.4.1`**, and fill in your
WiFi name and password plus the Ph3b3 host and port. Leave the device key blank
to use what you put in `secrets.h`. Save, and she reboots and joins.

The boot splash names the network she landed on. That's your confirmation.

> The setup network is open, so your WiFi password crosses it in the clear
> during that brief window. Local only, and it closes as soon as she joins.

## 5. Say something

Tap her face. The LED ring goes purple — she's listening. Talk, then wait. She
transcribes, thinks, and speaks the reply with captions tracking her voice.

Recording ends 12 seconds after the tap, or early if you tap again.

**Mode drawer:** tap the crescent in the top-left corner and swipe right —
**Talk · Network · Karaoke · Settings**. A right-to-left swipe backs out of any
full-screen mode.

## 6. Add your other networks

She holds **three** networks and, on every boot, tries them **top to bottom**,
keeping the first that answers.

**Settings → WiFi** shows a list of three slots:
- **Tap** a slot → type the network name → type the password → saved
- **Hold** a slot ~1 second → cleared (it fills red as you hold; lift early to cancel)

Put the network you're usually on in **slot 1**, and somewhere you visit
occasionally in **slot 3** — the bottom slot is only reached when the ones above
it aren't there. Empty slots are skipped, so it's fine to fill only slot 3.

The row shows the network she's *actually* on, not just "connected".

> A network with **no password can't be saved** — that's deliberate. A blank
> password writes an unjoinable entry that she'd retry forever.

---

## Power (read this)

Most confusing first-day problems are power, not software. A computer's USB port
often can't supply what she needs:

- **Her head droops or jerks at boot** — the tilt servo can't lift against
  gravity on a current-limited port. Pan turns fine. Not a bug, and don't try to
  fix it by increasing torque; that draws more current and makes it worse.
- **She sits on "connecting wifi..." forever** — the WiFi join draws a current
  spike that browns her out, and she resets straight back into the same screen.
  It looks like a hang; she's actually rebooting in a loop.

Use her **battery** or a **real wall charger**. A USB cable to your computer is
fine for flashing, just not as her only supply.

## If something's wrong

| What you see | What it is |
|---|---|
| Nothing on serial | Normal. USB-Serial-JTAG is silent on this unit — **her screen is the instrument.** |
| Splash lists networks under "no answer from:" | She tried every saved network and none replied. The setup portal opens next. |
| Face works, but she never answers you | She's on WiFi but can't reach Ph3b3. Check the host/port in her portal, and that you used `https://`. |
| Status word `denied` | Wrong username/password for the server. |
| Status word `no route` | Right credentials, can't reach the host — wrong IP, or the server isn't running. |
| Karaoke card won't mount | Card is exFAT. The ESP32 can't read it; reformat as **FAT32**. |
