# TRI-VIEW Foveated Rendering for RE2, RE4 and RE7 VR

A fork of [REFramework](https://github.com/praydog/REFramework), based on Talemann's RE4VR build, that adds a new VR rendering mode for **Resident Evil 2**, **Resident Evil 4** and **Resident Evil 7**: **TRI-VIEW Foveated Rendering**.

> [!IMPORTANT]
> This fork works **only with OpenVR (SteamVR)**. OpenXR is not supported.

---

## Why this fork

This started as a personal project. I wanted to play RE2, RE4 and RE7 on PS VR2 as well as possible:

- at SteamVR's recommended resolution for PS VR2 (3400×3468)
- at a steady 90 fps
- with as few compromises as possible
- with no shadows, bushes or objects that show up in only one eye, and no brightness difference between the eyes
- with as few visual artifacts as possible. Some are still visible when you look at your arms up close with the foveated rendering set to Fixed.
- with a clean interface

To get there, I added a new rendering mode. It is the only rendering mode in this fork.

### What is TRI-VIEW?

TRI-VIEW is like **quad views** foveated rendering, but it uses **3 views instead of 4**:

- **One wide view, shared by both eyes:** the area outside the middle, drawn at lower quality.
- **Two small sharp views, one per eye:** the area you look at, drawn at full quality.

Drawing one view fewer saves both CPU and GPU time. The sharp area can stay in the middle of the lens (**Fixed foveated rendering**) or follow your eyes (**eye-tracked foveated rendering**).

### A word of warning

This was meant to stay personal, so the whole project is heavily tuned for **my setup**:

- Headset: PS VR2
- CPU: Ryzen 7 7800X3D
- GPU: Radeon RX 7900 XTX

Some of the default options may perform worse on other hardware, or even stop the rendering mode from working. I honestly don't know, because I could only test it on my own PC. I'm publishing it anyway because it may still be useful to someone.

### How it was made

I have decent programming basics, but I always stayed away from anything with a GUI (apart from a 2D game I started decades ago and will never finish, and a few other small things). **All the code was written by Claude (Anthropic's AI).** My part was:

- coming up with the new rendering mode
- running thousands of tests
- collecting the dumps and logs Claude needed

Claude is great at writing code, but much less good at solving problems or coming up with new ideas unless you point it the right way.

---

## Features

- **TRI-VIEW Foveated Rendering:** fixed, or following your eyes with eye tracking.
- **A new anti-aliasing:** a supersampling TAA made for VR, for both the sharp area and the area around it.
- **Automatic GPU relief:** when the GPU needs more than a frame time you choose, the anti-aliasing is lowered a little, step by step, and goes back up when there is room again.
- **Steady image:** fixes for flickering outlines, flickering lights, menu ghosting and trails behind the HUD.
- **RE7 main menu fix:** look down to see the tape recorder and pick options, DLCs and extras comfortably. When you select something, the tiny booklet on your right grows and moves in front of your eyes.
- **RE4 crafting menu:** the distance of the 3D item shown in front of your eyes is adjustable from the mod menu.
- **Shaders built into the DLL:** no waiting for shaders to compile on the first start.
- Lots of smaller fixes.

---

## Requirements

- **Windows**, or **Linux** with Proton (see [Linux / Proton](#linux--proton)).
- **SteamVR**, with any headset that works in SteamVR.
- **DirectX 12 version of the game.** For RE2 and RE7 that is the current Steam version, not the older DirectX 11 ("non-RT") version.
- The VR gameplay mod for your game (see step 1 below).

Tested only on PS VR2 with an AMD RX 7900 XTX. Other headsets and GPUs (NVIDIA included) should work, but they have not been tested.

---

## Installation

1. **Install the VR gameplay overhaul mod for your game first:**
   - RE2: **RE2VR Mod Reloaded** – *(link)*
   - RE4: **RE4VR** by Talemann – *(link)*
   - RE7: **RE7VR Reloaded** – *(link)*

2. **Back up the original files.** Go to the game folder (the one with the Resident Evil `.exe`) and rename these files, so you can restore them if something goes wrong:
   - `dinput8.dll` → `dinput8.dll.orig`
   - `openvr_api.dll` → `openvr_api.dll.orig`
   - `openxr_loader.dll` → `openxr_loader.dll.orig`

3. **Download TRI-VIEW** for your Resident Evil game from the *(link)* page.

4. **Unzip it** and copy `dinput8.dll` and `openvr_api.dll` into the game folder, next to the `.exe`.

5. **Start the game** and open the mod menu (see below). TRI-VIEW is under **RENDERING → TRI-VIEW FOVEATED RENDERING**.

> [!NOTE]
> On the **first start**, the mod resets REFramework's settings (`re2_fw_config.txt`) to its own defaults. This happens only once. After that, your changes are kept. In RE4, only the values on the MOD OPTIONS page are reset. Your RE4VR data (holsters, offsets and so on) is not touched.

### Uninstall

Delete the two TRI-VIEW files and rename the `.orig` files back to their original names.

---

## Compatibility with the gameplay mods

- **RE4VR (Talemann):** TRI-VIEW is a fork of Talemann's REFramework, so RE4VR works without any limitation.
- **RE7VR Reloaded:** it comes with its own settings plugin that overwrites REFramework's configuration file at every start. That sets DLSS, which crashes the game on this build. TRI-VIEW recognizes the plugin and stops it from changing the configuration, and keeps all of the mod's other features working.
- **RE2VR Mod Reloaded:** it also ships with an REFramework, but the mod itself is not built into it, so it works with TRI-VIEW's `dinput8.dll`.
- **RE2VR Embraced:** from what I've read, this newer RE2 mod builds part of itself into REFramework, so it is probably not fully compatible with TRI-VIEW. If you want to use it, talk to its author about making the two work together.

---

## Opening the mod menu

Press **left trigger + the button below** at the same time to open or close the menu:

| Controller | Button |
|---|---|
| PS VR2 Sense | left Triangle |
| Meta / Oculus Touch (Quest, Rift S) | Y |
| Valve Index | left B |
| HTC Vive wands | left Menu button |
| Steam Frame | right Y |

On the keyboard, **Insert** always opens and closes the menu. If you changed your controller bindings in SteamVR, the buttons may be different.

---

## Eye tracking

TRI-VIEW reads your gaze through SteamVR. It works with **any headset whose driver passes eye tracking to SteamVR**. If no gaze is available, it falls back to Fixed foveated rendering by itself.

How to get eye tracking into SteamVR:

| Headset | What you need |
|---|---|
| **PS VR2** | [PlayStation VR2 Toolkit](https://github.com/BnuuySolutions/PSVR2Toolkit) (Sony's PC app has no eye tracking) |
| **Steam Frame** | Turn on eye tracking in the headset, and enable *Share eye tracking data to other apps on this PC* in Steam Link |
| **Quest Pro, Galaxy XR, Vive Focus Vision** | Enable eye tracking sharing in Steam Link, or use Virtual Desktop |
| **Varjo** | Varjo-EyeTracker-SteamVR |
| **Other headsets** | Check whether their software, or Virtual Desktop, sends eye tracking to SteamVR |

Notes:
- Use an up-to-date SteamVR.
- OpenComposite and xrizer may block eye tracking.

---

## Configuration

The mod comes already configured. Depending on your headset and PC, try these options **before** lowering the resolution in SteamVR. All of them are under **RENDERING → TRI-VIEW FOVEATED RENDERING**. Hover over an option to see what it does.

### Main options

| Option | Default | What to do |
|---|---|---|
| **Sharp area position** | Fixed | Choose **Follows your eyes (eye tracking)** if your headset has eye tracking (see [Eye tracking](#eye-tracking)). |
| **Sharp area size** | 0.25 | How much of the view is sharp (0.20–0.60). **Lower = faster**, but the image gets softer sooner away from the middle. With eye tracking you can usually go smaller than in Fixed mode. |
| **Sharp area upscaling** | 1.0 | 1.0 draws the sharp area at full resolution (best image). **Higher = faster but softer**, up to 1.6. |
| **Sharp area anti-aliasing** | 2x | **1x** is lighter, and a little softer when you move. |
| **Outside area anti-aliasing** | Sharp (2x) | **Light** is a good balance. **Lightest** is the cheapest and softest. |
| **Lower the anti-aliasing when the GPU can't keep up** | On, 89% | When the GPU needs more than this share of each frame's time, the anti-aliasing is lowered step by step, then goes back up. The share follows your headset's refresh rate by itself; the slider also shows the milliseconds (9.9 ms at 90 Hz, 7.4 ms at 120 Hz). **Lower** = steps in sooner (smoother, softer); **higher** = later (sharper, may stutter). |

### Advanced Options

Most of these are tuned for my setup. If something looks wrong or runs badly on your hardware, try these first:

| Option | Try this if… |
|---|---|
| **Faster image output to the headset** | Wrong colors or a black image in the headset → turn it **off**. |
| **Faster frame start for the mods** | Stutters or problems with other mods → turn it **off**. |
| **Small game window (faster)** | You record or stream the PC window → turn it **off**. |
| **Show the left eye in the desktop window** | You don't need the image on the PC screen → turn it **off** to save a little GPU time. |
| **Even out light and fog with the rest of the image** | Something flickers in the sharp area → raise the *smoothness*, or turn it off. |
| **Eye tracking offset / smoothing / dead zone** | With eye tracking, the sharp area is not exactly where you look, or it moves too much. |

**Sync Mode** (in the VR settings) is set to **Early**. If you get stutters, try **Late**.

The **Status** line at the bottom of the page tells you whether TRI-VIEW is running. If it cannot start, a message says why.

### CPU limited?

In some cases you can be limited by the **CPU** rather than the GPU, especially in **RE4** with a less powerful CPU. RE4 VR is heavy on the CPU, and scenes with many enemies are the worst. On a Ryzen 7 7800X3D there is still CPU time to spare.

- **How to tell:** the frame rate drops while the GPU is not fully used.
- **What doesn't help:** the options that save GPU time (sharp area size, upscaling, anti-aliasing, the automatic GPU relief) help little or not at all in that case.
- **What can help:**
  - Keep **Faster frame start for the mods** and **Skip a double interface update** on (the default).
  - Lower the game's own CPU-heavy graphics settings.
  - If nothing else works, lower the refresh rate of the headset (for example 120 → 90 Hz).

---

## Linux / Proton

Copy the files as described above, then add this to the game's launch options in Steam:

```
WINEDLLOVERRIDES="dinput8.dll=n,b" %command%
```

PS VR2 on Linux needs **Ignition**, from the same developers as PlayStation VR2 Toolkit. See their [Linux support](https://github.com/BnuuySolutions/PSVR2Toolkit/wiki/Linux-support) page.

---

## Known issues

- In **Fixed** mode, some artifacts are visible when you look at your arms up close.
- Tested only on PS VR2 with an AMD RX 7900 XTX. Other setups may need the changes described in [Configuration](#configuration).
- In heavy scenes, especially in RE4 with many enemies, a slower CPU can limit the frame rate (see [CPU limited?](#cpu-limited)).

---

## Credits

Heartfelt thanks to:

- **[praydog](https://github.com/praydog)** and all the **REFramework contributors**, for REFramework and its VR support, without which none of this would exist.
- **[cursey](https://github.com/cursey)**, for Kanan and kananlib, which REFramework is built on.
- **PureDark**, for his work on REFramework's VR (his fork, with AFW and the upscaler integration, is the base of Talemann's RE4VR build).
- **Talemann**, for the RE4VR mod, the mod menu GUI and all the work he did on REFramework, which this fork is built on.
- **Andyalpa**, for the **RE2VR Mod Reloaded** and **RE7VR Reloaded** gameplay mods.
- **Bnuuy Solutions**, for PlayStation VR2 Toolkit, which brings PS VR2 eye tracking to SteamVR.
- **Claude (Anthropic)**, which wrote the code of this fork.

Third-party code used by this fork:

- **AMD FidelityFX CAS** (MIT): the sharpening idea.
- **SMAA** by Jorge Jimenez et al. (MIT).
- **Dear ImGui**, **OpenVR** (Valve), **OpenXR SDK** (Khronos), **spdlog**, **nlohmann/json**, **sol2**, **Lua**, **glm**, **safetyhook**, **MinHook**, **DirectX-Headers** and **DirectXTK12** (Microsoft), and the other libraries that come with REFramework.

---

## License

Released under the MIT License, like REFramework. See [LICENSE](LICENSE).
