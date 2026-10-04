# Building and installing the PS5 version

One command takes your own disc image of the game to a title installed on a jailbroken PS5. This page is the whole procedure, written so that you can follow it yourself or hand it to an AI coding agent.

Nothing of the game is in this repository, and no ready-made PS5 executable can be offered: the executable contains the game's own code, recompiled, so everyone builds theirs from their own copy. What the build produces is for you, not for sharing.

## What you need

| | |
|---|---|
| The game | Your own disc image (`.iso`) of **Midnight Club: Los Angeles Complete Edition, USA/Europe, Xbox 360** (title id `545407F8`). This is the release the project was made with and the only one it is known to work with: the fixes are tied to addresses in that executable. |
| A console | A jailbroken PS5 that can run homebrew titles. Tested on two consoles: a PS5 Pro on firmware 13.42, where the development and all measurements were done, and a PS5 Slim on firmware 12.70, installed from scratch with this procedure and reported to run well. |
| On the console | An FTP server payload (the build uploads over FTP; port 2121 by default) and a homebrew mounter that puts folders under `/data/homebrew` on the home screen (ShadowMountPlus is what was used). |
| A PC | Windows 10/11 with WSL2, or a PC running Arch Linux. x86-64, about 30 GB of free disk, 16 GB of memory or more. |
| Time | About 45 minutes the first time on a 16-core PC (the Vulkan driver about 20, everything else about 25), plus the upload of 6 GB of game data. Later builds take a minute or two. |

The build host is **Arch Linux, run as root**. That is what the PS5 Vulkan driver project builds on, and the script installs packages with `pacman`. On Windows that means an Arch distribution under WSL2, which is a few commands; other distributions are not supported by this script.

## Windows: set up WSL2 with Arch Linux

In PowerShell, as administrator:

```powershell
wsl --install --no-distribution
```

Restart if asked, then:

```powershell
wsl --update
wsl --install -d archlinux
```

Open it as root:

```powershell
wsl -d archlinux -u root
```

Everything from here on is typed in that Linux shell. Your Windows drives are under `/mnt`: `C:\Games\mcla.iso` is `/mnt/c/Games/mcla.iso`.

If you already run Arch Linux natively, open a root shell and continue.

## Build and install

```bash
pacman -Sy --noconfirm git
cd /root
git clone https://github.com/holdmysocks/mcla-recomp.git
cd mcla-recomp
bash ps5/make_ps5.sh --iso /mnt/c/path/to/your.iso --console 192.168.1.50
```

Use your console's address for `--console`. Leave `--console` out to build without uploading; the script then prints what to copy where.

Keep the repository inside the Linux file system (under `/root`), not under `/mnt/c`: building from a Windows drive is several times slower, and Windows line endings break shell scripts.

What the script does, in order. Each step is skipped when its result already exists, so after a failure or an update you run the same command again and it continues:

1. Installs the packages it needs.
2. Builds the PS5 toolchain and the Vulkan driver ([mihawk-99/PS5_Vulkan](https://github.com/mihawk-99/PS5_Vulkan), GPL-3, with its sibling repositories) into `/root/ps5vk`.
3. Clones the ReXGlue SDK v0.10.0 into `/root/mcla/rexglue-sdk` and applies this project's patches.
4. Builds the recompiler for your PC.
5. Extracts the game files from your disc image into `game/`, and recompiles the game's code into `generated/`.
6. Builds the runtime for PS5.
7. Makes the title's tile and backgrounds from the dashboard art on your disc.
8. Compiles the game for PS5 and packages the title into `out/ps5-title/PPSA99779`.
9. Uploads the game data to `/data/mcla/game` and the title to `/data/homebrew/PPSA99779` on the console, checking every file's size.

Logs of every step are in `/root/mcla/logs`; when a step fails the script prints the end of its log and the log's path.

### Options

| Option | |
|---|---|
| `--iso PATH` | Your disc image. Not needed again once `game/` is extracted. |
| `--console IP` | Upload to the console. |
| `--ftp-port N` | The console's FTP port (default 2121). |
| `--title-id ID` | The title's id on the console (default `PPSA99779`). |
| `--tile IMAGE` | Your own picture for the home-screen tile; any common format, resized to 512x512. See "Your own tile and theme". |
| `--theme FILE.at9` | Your own home-screen theme, already in ATRAC9. |
| `--art-dir DIR` | Your own art in the console's formats (`icon0.png` 512x512, `pic0.dds`, `pic1.dds`, and optionally a theme `snd0.at9`) instead of the art made from the disc. |
| `--test-build` | Keep the test scaffolding: the title waits for `ps5/title_log_client.py` to connect and logs over the network. For development. |
| `--jobs N` | Parallel compile jobs. |

## Your own tile and theme (optional)

By default the tile and backgrounds are made from the dashboard art on your own disc, and the title has the driver project's default sound rather than a theme of its own. This repository ships no artwork or music: the game's art and music belong to Rockstar Games and Take-Two, and are not this project's to distribute. You can add your own at build time; these are the ones the maintainer uses.

**Tile.** The official cover art, for example [this image](https://assets-prd.ignimgs.com/2022/04/17/midnightcomplete-1650239367276.jpg). Save it and pass it with `--tile`:

```bash
bash ps5/make_ps5.sh --tile /mnt/c/Users/you/Downloads/midnightcomplete.jpg --console 192.168.1.50
```

**Theme.** The console only takes a theme as an ATRAC9 file, which needs Sony's encoder, so this is a step you do yourself on Windows:

1. Get the audio you want as a file. The maintainer uses the game's main theme, as heard in [this video](https://www.youtube.com/watch?v=VE8Xv4CCs8k).
2. Turn it down first. The console plays a theme as it is, and music mastered for listening is far too loud there: the first attempt measured -9 LUFS against the -28 LUFS a title is expected to have. With ffmpeg, which the build installs in the Linux shell:

   ```bash
   ffmpeg -i /mnt/c/path/to/theme-source.m4a -af loudnorm=I=-28:TP=-2 -ar 48000 -ac 2 /mnt/c/path/to/theme-quiet.wav
   ```

3. Convert `theme-quiet.wav` to ATRAC9 with a converter such as [AT9-AT3 Converter](https://github.com/BMK-Studio/AT9-AT3_Converter/releases): choose **AT9**, **PS4**, bitrate **72**. The result should be 48 kHz stereo.
4. Pass the `.at9` file with `--theme`:

   ```bash
   bash ps5/make_ps5.sh --tile /mnt/c/.../midnightcomplete.jpg --theme /mnt/c/.../theme-quiet.at9 --console 192.168.1.50
   ```

The console keeps its own copy of a title's tile and sound from when the title was registered, so a change may not show until the title is registered again.

## On the console

1. Start your FTP server payload before running the script with `--console`.
2. After the upload, let your homebrew mounter pick up `/data/homebrew/PPSA99779` (with ShadowMountPlus that happens when it scans, at the latest after restarting it or the console). "Midnight Club: Los Angeles" then appears on the home screen.
3. Start it. The first start takes a little longer; later ones load the graphics pipelines saved from earlier sessions.

The console keeps its own copy of a title's name, tile and sound from the moment the title is registered, so new art may not show until the title is registered again.

Where things are on the console:

| Path | |
|---|---|
| `/data/homebrew/PPSA99779` | The title |
| `/data/mcla/game` | The game data, unmodified, from your disc |
| `/data/mcla/mcla.toml` | Settings saved by the in-game menu |
| `/data/mcla/cache` | Saved shaders and pipelines |
| `/data/mcla/mcla-play.log` | Warnings, errors and a crash report from the last start |

## In the game

Pause, and the menu has three extra entries: DISPLAY (frame-rate target 30 or 60, motion blur, depth of field), PERFORMANCE and CONTROLS (button prompts, and whether the intro plays at normal speed, fast, or is skipped). Settings are saved when you leave the menu.

What to expect, measured on a PS5 Pro: a steady 30 frames a second in play; the zoomed-out map view runs at 20 to 25; a 60 target averages about 43 and is uneven, so 30 is the better setting. Sound is stereo.

## Updating

```bash
cd /root/mcla-recomp
git pull
bash ps5/make_ps5.sh --console 192.168.1.50
```

Only what changed is rebuilt, and only the title is uploaded again; game data already on the console is left alone.

If an update changes the SDK patch (`patches/`), remove the SDK checkout so that it is cloned and patched afresh: `rm -rf /root/mcla/rexglue-sdk /root/mcla/build-ps5 /root/mcla/build-host`.

## If something goes wrong

| Symptom | What to do |
|---|---|
| `this script needs Arch Linux` | You are in another distribution. Install Arch under WSL2 as above. |
| Step 2 fails | The log is `/root/ps5vk-arch.log`. It downloads from GitHub and other hosts; a failed download is the usual cause, and running the script again continues. |
| `SDK patch does not apply` | The SDK checkout is not a clean v0.10.0. Remove `/root/mcla/rexglue-sdk` and run again. |
| `game/default.xex is missing` or extraction fails | The image is not the supported release, or not a plain disc image. The extractor reads XDVDFS images (`.iso`), including the usual redump layouts. |
| `no FTP server at ...` | Start the FTP payload on the console, check the address and `--ftp-port`. |
| The game stops with "Call to invalid or unregistered function at guest address ..." in `mcla-play.log` | The recompiler missed a function that is only reached through a pointer. Open an issue with that line; the fix is one entry in `config/runtime_discovered.toml`. |
| A bar with the frame rate, temperatures and memory is drawn over the game | That is not the game or this project: it is your homebrew enabler's own monitor overlay. With OnionHEN, set `enabled=false` (or just `show_fps=false`) under `[overlay]` in `/data/OnionHEN/config.ini`, or turn it off in its settings. |
| The game closes with nothing in the log | Fetch `/data/mcla/mcla-play.log` over FTP before starting the game again (it is replaced at every start) and open an issue with it, your console model and firmware. |

## For an AI agent doing this for someone

- Ask the user for two things before starting: the path to their disc image and the console's IP address. Do not look for or download a disc image; the user must supply their own.
- Check the host first: `cat /etc/os-release` must say Arch Linux and `id -u` must be 0. On Windows, drive the Linux side with `wsl -d archlinux -u root -- <command>`.
- The whole job is the one command in "Build and install". It is safe to run again; it resumes at the first step whose result is missing. Expect it to run for most of an hour the first time with little output: each step logs to `/root/mcla/logs/<step>.log`, and step 2 to `/root/ps5vk-arch.log`.
- A step failed if the script prints `FAILED: <step>`; read that step's log, fix the cause, run the same command again. Do not edit generated files or the SDK checkout by hand.
- Writing to the console happens only in step 9 and only with `--console`. It uploads to `/data/mcla/game` and `/data/homebrew/<title id>` and nowhere else, and deletes nothing. Confirm with the user before running with `--console` if they have not asked for the upload.
- Never commit or share `game/`, `generated/`, `out/` or anything built from them.
