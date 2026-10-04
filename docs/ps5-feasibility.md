# PS5 feasibility

Console: PS5 Pro, firmware 13.42, OnionHEN v0.0.14 (per the user), elfldr v0.26 on port 9021.
Toolchain: ps5-payload-sdk v0.43, clang 18.1.3, built in WSL Ubuntu 24.04.

> This page records the first probes, from before the port existed. They were built with a plain ps5-payload-sdk on Ubuntu; the probe sources (`ps5/probes/memprobe`, `ps5/probes/sysprobe`), `ps5/compile_probe.sh` and the Ubuntu toolchain script were removed once the port moved to the Arch Linux build in `ps5/make_ps5.sh`, and are in the git history (last present at commit `5c29e7d`). The measurements stand; to build for PS5 now, see `ps5-build-guide.md`.

Only what has been run on the console is recorded as fact here.

## Probe 1: guest memory layout (`ps5/probes/memprobe`), 2026-10-03

Run as a payload through the ELF loader. Three versions were sent; v3 results:

| Test | Result | Detail |
|---|---|---|
| Page size | 16384 | Not 4096. Guest pages are 4 KiB and 64 KiB |
| Online cores | 16 | |
| Process layout | | code 0x200000000, stack 0x7eeffbcd4, heap 0x880009ba0 |
| `shm_open(SHM_ANON)` + `ftruncate` 4.5 GiB | PASS | |
| Anonymous `PROT_NONE` reservation, 4.5 GiB or 4 GiB | **FAIL** | `ENOMEM`, with or without an address hint |
| Anonymous `PROT_NONE` reservation, 2 GiB and 1 GiB | PASS | |
| `MAP_GUARD` reservation, 4.5 GiB | **FAIL** | `EBADF` |
| Whole shared object mapped `PROT_NONE`, `MAP_SHARED` (4.5 GiB) | PASS | Kernel placed it at 0x20001c000 |
| Two `MAP_FIXED` shared views inside that range | PASS | |
| Views alias the same pages | PASS | |
| Touch (commit) 512 MiB | PASS | |
| `sigaction` SIGSEGV/SIGBUS | PASS | |
| `mprotect` to read-only, write, handler unprotects, execution resumes | PASS | fault address reported correctly |
| Protection is per view (alias stays writable) | PASS | |
| protect + fault + unprotect round trip | 105 us | measured over 2000 iterations |
| Anonymous RWX page | PASS | Not needed, recorded for reference |
| `nanosleep(1 ms)` | 1.10 ms average | clock resolution 81 ns |

### What this means

**The guest memory model is buildable on PS5**, with three required changes to the runtime's POSIX memory layer:

1. **Reserve through the shared object, not anonymously.** Map the whole 4.5 GiB object `PROT_NONE` to claim address space, then place views over it with `MAP_FIXED`. Anonymous reservations above about 2 GiB are refused.
2. **The arena base must be chosen by the kernel.** The runtime's default bases (virtual 0x100000000, physical 0x200000000) cannot be used: payload code is loaded at 0x200000000. Probe v1 forced a mapping there with `MAP_FIXED | MAP_EXCL` and the process died without output, so `MAP_EXCL` must not be relied on to protect existing mappings. Generated code takes the base as a parameter, so a different base is possible in principle; whether the runtime has other hard-coded assumptions is not yet checked.
3. **16 KiB host pages.** The SDK already has a 16 KiB path for Apple Silicon, so this is expected to work, but it has not been built for PS5.

### Concerns raised by the probe

- **Write-watch cost.** 105 us per fault round trip is slow. The Linux/Vulkan MCLA project found write-watch faults were a major CPU cost even on Linux. At 30 FPS the whole frame budget is 33,333 us, so about 300 faults per frame would consume it. How many the game triggers per frame has to be measured on PC first. This is now the top performance risk.
- **Fault context layout.** The handler read `uc_mcontext.mc_rip` as 0x7eeffbbd0, which is a stack address, not a code address (code is at 0x2000xxxxx). The SDK's `ucontext_t` layout probably does not match what this firmware delivers. The runtime's handler needs the faulting instruction pointer and registers to emulate MMIO accesses, so the real layout must be established before porting the exception handler.
- **Memory budget.** Only 512 MiB was committed. The PC run uses about 1.4 GiB at the title screen. The limit for a payload process, and whether a homebrew app gets a larger one, is untested.
- **Payload vs app.** All of this ran as a payload. Limits may differ for an installed homebrew app.

## Not yet probed

- Vulkan driver availability and feature set on this firmware (probe 2).
- Compiling ReXGlue and the generated code with the PS5 toolchain (probe 3). The SDK has clang 18; ReXGlue is C++23.
- Thread creation limits (the game runs about 77 host threads on PC), priorities, affinity.
- File I/O throughput from `/data`.
- Audio output and controller input.

## Reference: what the Vulkan backend used on PC (2026-10-03)

The game reached the title screen on the Vulkan backend on this PC (RTX 4080, run `20261003-103756`, `--mcla_gpu_backend=vulkan`). That run is the checklist for the PS5 driver probe.

Device extensions enabled: `VK_KHR_swapchain`, `VK_EXT_custom_border_color`, `VK_EXT_fragment_shader_interlock`, `VK_EXT_memory_budget`, `VK_EXT_non_seamless_cube_map`, `VK_EXT_robustness2`.

Core features enabled include `robustBufferAccess`, `fullDrawIndexUint32`, `independentBlend`, `geometryShader`, `tessellationShader`, `sampleRateShading`; the full list is in the run log.

Which of these are required rather than opportunistic is not yet known. The backend has a framebuffer path (`render_target_path_vulkan = "fbo"`) that should not need fragment shader interlock; that path has not been tried here. The next PC step is to turn optional features off one at a time and see what still renders, so the PS5 requirement list is a minimum, not a maximum.

## Probe 2: signal context, threads, memory budget (`ps5/probes/sysprobe`), 2026-10-03

Run as a payload. It finished and reported "done, cleaned up".

| Test | Result |
|---|---|
| 100 threads created and joined | PASS |
| Commit 2048 MiB of shared memory in 256 MiB steps | PASS, all eight steps |
| Fault address in `siginfo` | Correct |
| Register layout in the signal context | **Shifted by 0x30 from the SDK header** |

Signal context detail. Marker values were loaded into registers before a deliberate fault and located in the context the handler received:

| Register | SDK header offset | Actual offset |
|---|---|---|
| rdi | 0x18 | 0x48 |
| rbx | (not printed) | 0x80 |
| r12 | 0x70 | 0xA0 |
| r13 | | 0xA8 |
| r14 | | 0xB0 |
| r15 | | 0xB8 |
| rip | 0xB0 | 0xE0 |

Every register checked sits exactly 0x30 bytes later than `ucontext_t` in ps5-payload-sdk v0.43 says. This explains probe 1 reading a stack address as the instruction pointer. The runtime's exception handler must read the machine context at `(char *)uc + 0x30 + offsetof(...)`, or use a corrected structure, on this firmware. Whether the offset is the same on other firmware versions is unknown.

What this settles:

- **Memory budget is not a blocker at the size the PC build uses.** 2 GiB committed without complaint; the PC build uses about 1.4 to 1.6 GiB. The upper limit was not searched for. The 4.5 GiB reservation plus a 2 GiB texture cache has not been tried together.
- **Thread count is not a blocker.** The PC build runs about 80 threads.
- **Fault handling is usable** once the 0x30 offset is applied.

## Probe 3: compiling with the PS5 toolchain (`ps5/compile_probe.sh`), 2026-10-03

Syntax-only check (`-fsyntax-only`, C++23, prospero-clang++ 18.1.3 from ps5-payload-sdk v0.43). Nothing was linked, so this says nothing yet about missing libraries or symbols.

| Set | Result |
|---|---|
| Generated game code | **121 of 121 source files pass** |
| ReXGlue runtime, portable and POSIX sources | 195 of 254 pass |

The generated code needed three small SDK header changes first (`patches/rexglue-ps5-platform.patch`): a `REX_PLATFORM_PS5` definition, and extending two existing macOS fallbacks to PS5 because the SDK's libc++ lacks floating-point `std::from_chars` and `std::chrono::clock_cast`. It also needed a PS5 entry in the dynamic-library name table.

The 59 runtime failures fall into four groups:

| Group | Files | Nature |
|---|---|---|
| Include paths the probe script did not supply (renderdoc, glslang SPIR-V headers, generated `rex/version.h`, and so on) | about 40 | Probe artefact, not a porting problem. A real CMake build supplies these |
| `static_assert` "This file is POSIX-only" keyed on Linux or macOS | 8 | One-line condition change per file |
| Linux-only names: `fseeko64`, `ftello64`, `ftruncate64`, `mmap64`, `stat64`, `CLOCK_MONOTONIC_RAW`, `SYS_gettid`, `pthread_getname_np` | 5 | FreeBSD has the plain 64-bit-clean equivalents |
| libc++ gaps: `std::jthread`, `std::stop_token` in `timer_queue.cpp` | 1 | Needs a small replacement |

Also seen: the window surface code includes X11/XCB headers, and the Vulkan loader, RenderDoc and SPIRV-Tools are opened by library name at run time. PS5 needs its own surface and a statically linked driver, as expected.

Verdict: **no compiler-level blocker.** The toolchain's age (clang 18, older libc++) costs a handful of fallbacks, not a redesign. The real porting work is the platform layer (memory, exception handler, threads, surface, audio, input) and linking against a PS5 Vulkan driver.

## Probe 4: the Vulkan driver, 2026-10-03

Driver: mihawk-99's PS5_Vulkan, the Mesa 26.2 RADV port (`main` at the time, RADV pinned at `0b2d6d1a61d9`). GPL-3.0-or-later. No tagged releases.

Build, recorded in `ps5/build_ps5_vulkan_driver.sh`:

- **On Arch Linux (WSL, clang/LLD 23.1.1): all nine steps succeed**, producing the driver archive (`libvulkan_radeon.ps5.a`, 272 MB) and the author's smoke-test title `PPSA99014` (42 MB app folder).
- On Ubuntu 24.04 (LLVM 18) the same steps build the shader compiler, runtime and RADV, but both link stages fail: an undefined `ps5_fp_ieee` in the base app, and the packaging tool rejecting a weak undefined `radv_EnumeratePhysicalDevices`. The driver's documentation names Arch as the supported host; this is why.
- The build order and the sibling repositories (`PS5_PayloadSDK`, `PS5_Mesa`, `ps5-opengl` at tag `v0.3.0`) are not documented upstream. One upstream source download (zlib.net) was unreachable from here; the script substitutes zlib's own release archive, checked against the hash Mesa's build expects.

A second driver, mpereiraesaa's ps5-vulkan, could not be built: it requires a companion repository (`logging_server`) that is not public.

The smoke test creates a device and runs a buffer fill, a buffer copy, a compute dispatch and a triangle draw, each read back and compared, plus further checks. It writes its results to klog and to `radv-smoke.txt` in its own app folder, which FTP can read. It reports the device name and Vulkan version but not a full feature list; a feature dump needs a small title of our own linked the same way.

### Result on the console

The smoke-test title was uploaded to `/data/homebrew/PPSA99014` (seven files, sizes verified), launched by the user from the home screen, ran to completion and exited. Its results file, read back over FTP:

**102 checks passed, 0 failed.**

| Area | What passed |
|---|---|
| Device | `PlayStation 5 GPU (RADV NAVI21), Vulkan 1.4.354, radv Mesa 26.2.0`; instance and device creation |
| Shader compilation | SPIR-V compiled to pipelines on the console for compute, vertex/fragment, tessellation, geometry, mesh and task stages |
| Basics | Buffer fill and copy read back exactly; compute dispatch; clear and triangle draw read back texel for texel |
| Geometry shaders | 15+ pipeline variants, strips of 16 to 128 vertices, indexed, indirect and indirect-count draws, primitive restart |
| Tessellation | From coordinates, control points, with varyings, levels 2 to 9 |
| Memory | The GPU reads what the CPU just wrote without a flush, and the reverse; 16 MiB read in 0.90 ms and written in 1.05 ms through a mapping; **a shader writes into the title's own anonymous memory through its address** |
| Display | `VK_KHR_display` and `VK_KHR_swapchain`; one 3840x2160 display at 59.94 Hz; 3 to 5 swapchain images; 60 frames presented with FIFO pacing matching the refresh; swapchain replacement |
| Other | Ray-tracing acceleration structures build; mesh and task shaders run |

Not reported by the driver on this console: `VK_KHR_fragment_shader_barycentric` and `VK_KHR_fragment_shading_rate` (their checks were skipped, not failed). Neither is used by the Xenia-derived backend.

Files the run left on the console, all inside `/data/homebrew/PPSA99014`: `radv-smoke.txt` (7.5 KB) and a `radv-shader-cache` folder.

### What this settles

- **A usable GPU path exists on this console.** Hardware-accelerated Vulkan 1.4 runs on the PS5 Pro at firmware 13.42, with on-console shader compilation and presentation to the display.
- **The two features the Xenia Vulkan backend leans on most are present and exercised:** geometry shaders and swapchain presentation.
- **GPU access to title memory by address works.** The emulated GPU reads guest memory directly, so this matters; it has to be confirmed at the scale of the 512 MiB guest physical range.

### What it does not settle

- The full feature and extension list against the PC checklist above (`fragmentStoresAndAtomics`, `independentBlend`, `sampleRateShading`, MSAA sample counts, BC formats, `VK_EXT_fragment_shader_interlock`, and so on). The smoke test does not print it. A small title of our own, linked the same way, is the next step.
- Performance: draw-call throughput and shader compile time on the console.
- Whether the driver's memory use fits alongside the game's 4.5 GiB reservation.

What this means for the port: the PS5 build of this project will need an Arch (or equally current LLVM) host for at least the final link, and the driver is linked statically as one large archive.

## Probe 5: Vulkan feature dump (`ps5/probes/vkinfo`), 2026-10-03

Our own title, "MCLA Vulkan Info" (PPSA99777), linked against the driver the same way as its smoke test (`ps5/probes/vkinfo/build.sh`). It queries the physical device and writes the result to its own folder; it creates no logical device and submits no GPU work. Uploaded to `/data/homebrew/PPSA99777`, launched by the user, ran and exited. Raw output: `ps5/results/mcla-vkinfo-ps5pro-13.42-2026-10-03.txt`.

**Every extension and feature the PC Vulkan run used is present on the console.**

| Item from the PC checklist | On PS5 |
|---|---|
| `VK_KHR_swapchain` | yes |
| `VK_EXT_custom_border_color` | yes |
| `VK_EXT_fragment_shader_interlock` (sample and pixel interlock) | yes |
| `VK_EXT_memory_budget` | yes |
| `VK_EXT_non_seamless_cube_map` | yes |
| `VK_EXT_robustness2` (including null descriptors) | yes |
| `robustBufferAccess`, `fullDrawIndexUint32`, `independentBlend`, `geometryShader`, `tessellationShader`, `sampleRateShading` | all yes |
| `fragmentStoresAndAtomics`, `dualSrcBlend`, `depthClamp`, `samplerAnisotropy` (16x) | all yes |
| `textureCompressionBC` | yes: BC1 to BC7 sampled with linear filtering |
| MSAA | 1, 2, 4 and 8 samples for colour, depth and stencil |
| Colour attachments | 8 |

Also reported: 219 device extensions, including `VK_EXT_external_memory_host` (importing existing host memory). Device: `PlayStation 5 GPU (RADV NAVI21)`, Vulkan 1.4.354, Mesa 26.2.0. Conformance version 0.0.0.0, meaning the driver does not claim conformance.

Memory as the driver reports it: a 4096 MiB host-visible heap and an 8192 MiB device-local heap. Queue families: one graphics/compute/transfer queue and four compute/transfer queues.

Differences from a desktop GPU that the backend has to handle:

- **`D24_UNORM_S8_UINT` and `X8_D24_UNORM_PACK32` are not supported.** Depth is available as `D16_UNORM`, `D32_SFLOAT` and `D32_SFLOAT_S8_UINT`. This is normal for AMD hardware and Xenia's Vulkan render-target code already has a 32-bit float depth path for it, but it is the path that will be used, and the Xbox 360's 24-bit depth has to be converted.
- `shaderResourceResidency`, ETC2 and ASTC are not available. None are needed.

### Verdict on the GPU path

There is no missing capability. The Xenia-derived Vulkan backend's requirements, as observed on PC, are all met by this driver on this console. What remains unknown is behaviour under real load: draw throughput, shader compile stalls, driver bugs outside what its test suite covers, and memory use next to the game's 4.5 GiB reservation.
