# PS5 feasibility

Console: PS5 Pro, firmware 13.42, OnionHEN v0.0.14 (per the user), elfldr v0.26 on port 9021.
Toolchain: ps5-payload-sdk v0.43, clang 18.1.3, built in WSL Ubuntu 24.04.

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
