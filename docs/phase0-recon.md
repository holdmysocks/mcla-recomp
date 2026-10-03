# Phase 0: recon and feasibility

Date: 2026-10-03. Status: report only, no project code written yet.

Every claim below is tagged with how it is known:

- **[measured]** read from the user's own disc image with `recon.py` (XDVDFS walk + XEX2 header parse, no decryption)
- **[source]** stated by a third-party project's README/docs; not reproduced here
- **[unverified]** inference or open question

## 1. Headline findings

1. **MCLA has already been statically recompiled for PC by at least three projects**, all on the ReXGlue SDK. One (LARecomp) reports menus, free roam, races and saves working on Windows/D3D12. Writing a new recompiler and a new Xbox 360 runtime from scratch would duplicate work that exists under a permissive licence.
2. **Hardware-accelerated Vulkan exists for PS5 homebrew** (Mesa-derived drivers on top of AGC). So a GPU path for the PS5 target plausibly exists. It is GPL-3.0 and non-conformant.
3. **Nobody appears to have ported ReXGlue's runtime to PS5.** That port is the part of this project that is actually new.

Recommended reshaping of the plan is in section 7.

## 2. Prior art

| Project | What it is | Licence | Reusable for us |
|---|---|---|---|
| [XenonRecomp](https://github.com/hedge-dev/XenonRecomp) | PPC to C++ recompiler (hedge-dev, 2025) | MIT [unverified, check before use] | Recompiler only; no runtime. Superseded for our purposes by ReXGlue. |
| XenosRecomp | Xenos shader microcode to HLSL, ahead of time | MIT [unverified] | Only useful with a hand-written native renderer (the UnleashedRecomp approach). Not needed if we keep Xenia's GPU emulation. |
| UnleashedRecomp | Sonic Unleashed port: XenonRecomp + bespoke runtime and renderer | GPL-3.0 [unverified] | Reference for the "native renderer" approach. Game-specific. |
| Xenia | Xbox 360 emulator: kernel HLE, Xenos command processor, XMA, VFS | BSD-3 | Foundation of ReXGlue. Also the reference for a differential-test harness. |
| [ReXGlue SDK](https://github.com/rexglue/rexglue-sdk) | Recompiler + Xenia-derived runtime (kernel HLE, GPU, audio, input), TOML config, mid-asm hooks | BSD-3 (Tom Clay; Xenia contributors) [source] | **Everything in Phases 1 to 4.** Windows and Linux, x86-64 and ARM64, Vulkan/D3D12/Metal. Nightly 0.10.0.24 on 2026-10-02; API still unstable. |
| [LARecomp](https://github.com/mzzvxm/LARecomp) | MCLA Complete Edition on ReXGlue 0.10.0, Windows, D3D12 | **No licence file** [source] | Reference reading only. Without a licence its config (24,927 function hints, 112 hooks) and source cannot be copied into our repo. |
| [Heus-Sueh/midnight-club-la-recomp](https://github.com/Heus-Sueh/midnight-club-la-recomp) | MCLA on ReXGlue 0.10.0 + SDL3 + Vulkan, Linux and Windows | No licence stated [source] | Evidence that the Vulkan path works for this game: 30,028 functions, 1280x720, 18 to 30 FPS on Linux, CPU-side draw translation named as bottleneck. |
| [CrownParkComputing/Xbox360-Native-Ports](https://github.com/CrownParkComputing/Xbox360-Native-Ports) | Binary launchers, MCLA v1 (2026-09-16), Vulkan, Linux and Windows | Not checked | Binary only. Confirms title 545407F8 and 30,205 functions. |
| [zarif98/midnightclub](https://github.com/zarif98/midnightclub) | Earlier MCLA attempt, abandoned in favour of LARecomp | Not checked | None. |

PS5 side:

| Project | What it is | Licence | Notes |
|---|---|---|---|
| [ps5-payload-dev/sdk](https://github.com/ps5-payload-dev/sdk) | Toolchain: clang 18 / lld 18, libc, pthreads, sockets, stubs for libkernel, libSceVideoOut, libSceAudioOut, libScePad | GPLv3+ | Host must be Linux or macOS. Firmware range not documented in README. |
| [mihawk-99/PS5_Vulkan](https://github.com/mihawk-99/PS5_Vulkan) | Mesa RADV port (reports Vulkan 1.4) plus older `ps5vk` (1.1) | GPL-3.0-or-later | SPIR-V compiled on console (NIR, ACO, AGC). Full CTS run on console with 0 failures on second run [source]. Static link only. Needs etaHEN. Runs vkQuake, Dolphin, PPSSPP. |
| [mpereiraesaa/ps5-vulkan](https://github.com/mpereiraesaa/ps5-vulkan) | Separate experimental Vulkan 1.3 | GPL-3.0-or-later | MSAA 2x/4x, geometry, tessellation, BC textures, dual-source blend, fragment stores. 1080p VideoOut. |
| [ps5-opengl](https://github.com/OpenAGC/ps5-opengl) | Mesa/Gallium OpenGL 4.6 | Not checked | Not needed; Xenia has no GL backend. |
| PS5SX2 | PCSX2 port using PS5_Vulkan (Sept 2026) | n/a | Proof that a large emulator with a Vulkan renderer ships on this stack. Requires firmware 13.60 or below [source]. |

## 3. The executable

All [measured] from `/default.xex` on the user's disc (USA/Europe Complete Edition).

| Field | Value |
|---|---|
| Format | XEX2, module flags 0x1 (title) |
| Original PE name | `mc4_xenon_final.exe` |
| Title ID / media ID | 545407F8 / 5940C9DB |
| Version / base version | 0.0.0.8 / 0.0.0.8, disc 1 of 1 |
| Link timestamp | 1247869399 (2009-07-17 UTC) |
| File size | 9,252,864 bytes, PE data at file offset 0x3000 |
| Encryption | 1 = normal (AES-128-CBC, session key wrapped by the retail key) |
| Compression | 1 = basic (zero-run blocks, 2 blocks). **Not LZX.** |
| Image base / size | 0x82000000 / 0x9E0000 (9.9 MiB), 64 KiB pages, 158 page descriptors |
| Entry point | 0x821322B8 |
| Default stack | 0x40000 |
| TLS | 64 slots, template at 0x8294A800, 0x94 bytes |
| Embedded resource | `545407F8` at 0x82950000, 0x8A724 bytes (XDBF: title strings, achievements) |
| Region / media flags | 0xFFFFFFFF (region free) / 0x4 (DVD) |

Section layout from page descriptors:

| Range | Kind | Size |
|---|---|---|
| 0x82000000 to 0x8212FFFF | read-only data | 1.2 MiB |
| 0x82130000 to 0x827CFFFF | code | 6.6 MiB |
| 0x827D0000 to 0x8294FFFF | data | 1.5 MiB |
| 0x82950000 to 0x829DFFFF | read-only (resources) | 0.6 MiB |

Imports: only two libraries, `xboxkrnl.exe` (313 records) and `xam.xex` (190 records), both 2.0.7371.0. Function imports use two records each, so these are upper bounds: at most 313 kernel and 190 XAM entries. Ordinals live inside the encrypted image, so the named list is **[unverified]** until the image is loaded by the recompiler.

Statically linked XDK libraries (XDK build 6995): `XAPILIB`, `XBOXKRNL`, `LIBCMT`, `XAUD`, `XMP`, `D3D9`, `XGRAPHC`, `XONLINE`, `XHV`, `XFFB`.

What that tells us:

- `D3D9` and `XGRAPHC` are statically linked. The game writes Xenos command buffers itself through inlined D3D code; there is no D3D import to hook. GPU work has to be done at command-buffer level (Xenia's approach) or by hooking the statically linked D3D functions by address (UnleashedRecomp's approach).
- `XAUD` is the legacy XAudio, not XAudio2 and not XACT. Audio goes through the kernel XMA context and audio-driver imports. No third-party audio middleware is visible in the header.
- `XHV` (voice) and `XONLINE` are present: Live code paths exist and need stubbing.
- `XFFB` is force feedback (wheels).
- 6.6 MiB of code is roughly 1.7 million PPC instructions. Other projects report 30,028 to 30,205 functions [source].

### How the user-side extractor gets the image

1. Walk XDVDFS in the user's ISO (game partition at 0xFD90000, root dir sector 1783935) and copy out `default.xex`, the four `.rpf` archives and the `.bik` movies. The Phase 0 script already does the walk and pulled `default.xex` this way.
2. Decrypt: unwrap the per-file session key from the security info with the retail XEX key, AES-CBC decrypt the payload, expand the basic-compression blocks. ReXGlue's loader does this in memory at codegen and at run time, so no decrypted PE has to be written to disk or shipped.
3. The retail XEX key is a well-known constant present in Xenia and ReXGlue source. Whether our own repo may carry it, or must rely on the upstream SDK's copy, is a decision for the user (section 8).

Disc content has no title update. Whether one exists and is needed is **[unverified]**.

## 4. Disc contents and engine

[measured] 15 files, 6.12 GiB:

| File | Size | Notes |
|---|---|---|
| `xarchive_cache.rpf` | 2.13 GB | Main game data |
| `xarchive_audio.rpf` | 1.62 GB | Audio |
| `xarchive_audlo.rpf` | 1.46 GB | Audio (second bank) |
| `xarchive_music.rpf` | 0.78 GB | Radio music |
| `intro720.bik`, `intro576_16x9.bik`, `intro576_4x3.bik` | 124 MB each | Bink video |
| `attract720.bik` and two 576 variants | 63 MB each | Bink video |
| `default.xex` | 8.8 MB | |
| `nxeart`, `$SystemUpdate/*` | | Dashboard art and system update, not needed |

Engine characterisation:

- Rockstar San Diego RAGE, same generation as GTA IV. Archives are RPF3 [source: GTAMods wiki, rpf-archive-rs]. Tools that read them: OpenIV, RPFTool, rpf-archive-rs.
- Movies are Bink 1. Bink is not in the XEX static library table (that table lists XDK libraries only), so the decoder is linked into the game image and gets recompiled with everything else. Nothing to reimplement.
- Resource types inside the archives (textures, drawables, scripts) are **[unverified]**; I have not opened the RPFs. The recompiled game parses them itself, so this only matters for modding or a native renderer.
- Threading: about 40 guest threads in steady state [source: LARecomp notes], with dedicated render, XMA decode and audio worker threads.
- Timing: the engine has a frame timer object and two fixed-timestep paths; both need patching to run above 30 FPS [source: LARecomp notes].
- Renderer: 1280x720, roughly 70 draw calls per frame by LARecomp's count [source]. EDRAM tiling behaviour is in play (LARecomp reports a 1.5x gain from forcing single-tile rendering).

## 5. Risk list

| # | Risk | Evidence | Severity | Mitigation |
|---|---|---|---|---|
| 1 | Indirect calls, jump tables, function boundaries | Three projects got to 30k functions with ReXGlue's analysis; LARecomp still needed about 25k hints | Medium, solved in principle | Use ReXGlue's analyser; build our own hint file from our own analysis runs |
| 2 | Self-modifying code | None reported; image is a single static PE with no code pages marked writable [unverified beyond page flags] | Low | ReXGlue has a `writable_code_segments` feature if needed |
| 3 | VMX128 | RAGE uses it heavily for math. Heus-Sueh reports 20 "unexpected float16_4 pack" codegen warnings | Medium | Differential tests of vector ops against Xenia's interpreter |
| 4 | Kernel/XAM surface | At most 313 + 190 imports; Xenia HLE covers enough for three projects to reach gameplay | Low on PC | Reuse. On PS5 the HLE's host dependencies must be ported (risk 8) |
| 5 | GPU: inlined D3D writing raw Xenos command buffers, EDRAM, resolves, tiling | Static `D3D9`; known unresolved dithered-alpha artifact on D3D12 in LARecomp | Medium | Keep Xenia's command processor; Vulkan backend is the one PS5 can use |
| 6 | Memory model | Xenia maps a 4 GiB guest address space with several aliased views of 512 MiB physical memory at a fixed host base | **High on PS5** | Must confirm PS5 homebrew can reserve >4 GiB of address space and create shared, aliased mappings. Unverified. |
| 7 | PS5 Vulkan driver completeness | Xenia's Vulkan backend wants geometry shaders, multiple render targets, MSAA, BC formats, and optionally fragment-shader interlock. RADV port claims most; `ps5vk` lacks MRT. Drivers are non-conformant | **High** | Run Xenia's Vulkan backend feature checks on the console early, before any other PS5 work |
| 8 | Runtime host dependencies on PS5 | ReXGlue expects Windows or Linux, SDL, a normal libc, threads with priorities/affinity, high-resolution timers, memory-mapped files. PS5 is FreeBSD-derived with Sony libraries | **High**, and it is the main body of new work | Platform layer port: threads, memory, timers, file I/O, audio out (libSceAudioOut), pad (libScePad), VideoOut via the Vulkan driver's swapchain |
| 9 | Toolchain mismatch | PS5 SDK ships clang 18; Heus-Sueh's project asks for clang 20+; ReXGlue requires SSE4.1 (PS5 Zen 2 has AVX2, fine) | Medium | Test-compile ReXGlue and a sample of generated code with prospero-clang 18 first |
| 10 | Performance on PS5 | Linux/Vulkan build reports 18 to 30 FPS on PC with CPU-side draw translation as bottleneck; LARecomp says dense areas are bound by guest AI and physics. PS5 cores are Zen 2 at about 3.5 GHz | **High** | Measure on PC Vulkan first. If PC Vulkan cannot hold 30, PS5 will not either |
| 11 | Licensing | PS5 SDK and both Vulkan drivers are GPL-3; drivers are statically linked. ReXGlue is BSD-3 (compatible). LARecomp has no licence | Medium | Our repo would have to be GPL-3-compatible for the PS5 build. Do not copy LARecomp material |
| 12 | Upstream churn | ReXGlue is at 0.10 nightlies with breaking API changes, and has a known vblank-timer overflow bug that collapses frame rate | Medium | Pin a version; carry patches in a fork |
| 13 | Dev environment | Host has no WSL distribution, no CMake, clang or Ninja on PATH [measured]. PS5 SDK needs Linux or macOS | Low | Install a WSL distribution and the toolchain |

## 6. What could not be verified in Phase 0

- Named import list, PE section names, jump-table count, VMX128 instruction density. All need the image decrypted in memory by the recompiler.
- Contents of the RPF archives.
- Whether a title update is needed, and whether the Complete Edition DLC is inside `xarchive_cache.rpf` or expected as separate content packages.
- Everything about the PS5 side on this specific console: firmware, etaHEN version, whether the Vulkan drivers run on it, memory-mapping limits.
- None of the third-party status claims were reproduced. Nothing has been built or run.

## 7. Recommended plan change

The original brief has us write a recompiler (Phase 1), a kernel HLE (Phase 2) and a Xenos GPU layer (Phase 3). All three exist in ReXGlue under BSD-3 and are already proven on this exact game. Recommendation:

- **Phases 1 to 4 become "stand up our own MCLA project on a pinned ReXGlue fork".** Our own manifest and hint file, generated from our own analysis; Vulkan backend from day one, since that is the only backend PS5 can use. Milestones stay the same: boots, menu, race.
- **Differential harness** is still worth building, scoped to what is new: generated code compiled with the PS5 toolchain versus the PC build.
- **Phase 5 is the real project**: a PS5 platform layer for the ReXGlue runtime, linked against a PS5 Vulkan driver. It starts with three go/no-go probes on the console, in this order: (a) 4 GiB guest memory mapping with aliased views, (b) Xenia Vulkan backend device-feature check against the RADV port, (c) test-compile of the generated code with prospero-clang.

Writing everything from scratch remains possible but would be many months of work to reach a point others have already reached, with no benefit to the PS5 goal.

## 8. Decisions needed from the user

1. Build on ReXGlue (recommended) or write from scratch as originally specified?
2. Licence: accept GPL-3 for the project, which the PS5 Vulkan drivers and payload SDK effectively require?
3. LARecomp has no licence. Treat it as read-only reference and derive our own hints and hooks (recommended), or ask its author for permission?
4. XEX retail key: rely on the copy inside the upstream SDK and never place it in our repo (recommended), or something else?
5. PS5 firmware version, etaHEN (or other HEN) version, and whether running homebrew apps (not just payload ELFs) already works on the console.
6. Install a WSL distribution on this PC for the PS5 toolchain and Linux builds?
