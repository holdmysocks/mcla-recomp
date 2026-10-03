# Function recovery decisions

Tool: ReXGlue SDK v0.10.0 (`f5337cdc`), `rexglue codegen mcla_manifest.toml`.
Input: `game/default.xex` from the USA/Europe Complete Edition disc (title 545407F8, version 0.0.0.8).

## Result (2026-10-03)

- Analysis passes with 8 manual function-entry hints in `config/mcla.toml`.
- 245 generated source files, 124.5 MB, 29,763 function definitions by grep of the generated code.
- Codegen takes about 11 s.

## Hints and why

All 8 hints fix the same failure: the analyser reports `UnresolvedCall ... target not in any function` for a plain `b` whose target is not inside any discovered function. These are tail-call targets that no `bl` references, so the scan never sees them as entries.

| Pass | Address | Branch site |
|---|---|---|
| 1 | 0x8220BF08 | 0x8220DAF4 |
| 1 | 0x8220C018 | 0x8220DA7C |
| 1 | 0x822B88C8 | 0x82203F90 |
| 1 | 0x822C98B8 | 0x822C9E04 |
| 1 | 0x823F32E8 | 0x823FB7F4 |
| 1 | 0x823FD718 | 0x823FDB24 |
| 1 | 0x824B0DE8 | 0x824AF4D0 |
| 2 | 0x822C9948 | 0x822C9E14 |

Pass 2 only appeared after pass 1 was applied. The hints give an entry address only; sizes are left to the analyser. None of these functions has been inspected by hand yet.

## Open items

- **20 `Unexpected float16_4 pack instruction` warnings**, all in 0x8243C67C to 0x8243D158. One tight cluster, so probably one or two functions (a vertex or colour packing routine). The recompiler emits something for them but flags it as an unexpected form. Not yet checked for correctness; candidates for the first differential tests.
- Indirect-call targets that are only reachable through function pointers or vtables are not validated by the analyser. They will show up at run time as dispatch failures. The other Vulkan MCLA project found five such addresses (0x822C9DC8, 0x822C9DD8, 0x82554060, 0x82554080, 0x822C9828); we have not added them and will only do so once our own runs hit them.
- The function count here (29,763) is lower than the 30,028 and 30,205 other projects report. The difference is unexplained; they may count imports or thunks, or carry more hints.

## Update, later on 2026-10-03: first boot to title screen

Hint totals when the game first reached the title screen:

| File | Hints | Source |
|---|---|---|
| `config/mcla.toml` | 10 | 8 analyser-reported tail-call targets, 2 early run-time stops (0x827A7FD0, 0x827A8220) |
| `config/data_referenced.toml` | 30 | `--mcla_scan_code_pointers`: of 79,270 code pointers found in the image's data, 30 pointed at entries the recompiler had not emitted |
| `config/runtime_discovered.toml` | 3 | `scripts/recover_loop.ps1`: 0x8249CBF0, 0x8249CC00, 0x822C9DD8 |

The runtime registers 30,041 functions with these hints. Only boot and the title screen have been exercised, so more pointer-only functions should be expected in menus and gameplay; `recover_loop.ps1` cannot reach those without input.

## Update: first play session (2026-10-03, user playing with a controller)

Five more run-time discoveries, all appended to `config/runtime_discovered.toml` (now 8 entries, 48 hints overall):

| Address | When it was hit |
|---|---|
| 0x82554080 | First run with a real audio output device (not reached with the silent driver) |
| 0x82264760, 0x82264770 | About 2 minutes into play |
| 0x82262320 | About 3.5 minutes into play |
| 0x822C9FE8 | About 4.5 minutes into play |

After these the game ran for 7.7 minutes and was closed normally (`play-20261003-151730.log`, exit code 0, no errors logged). One GPU warning appeared once: a texture fetch constant with an "invalid" type; the SDK has a `gpu_allow_invalid_fetch_constants` option for it. What was on screen at each point is not in the logs.
