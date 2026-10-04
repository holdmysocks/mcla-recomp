// PS5 port milestones P2 and P3, one step per build.
//
// The first version of this test ran everything in one title and took the
// whole console down, leaving no log (docs/ps5-port-plan.md). This version is
// built once per step (-DMCLA_STEP=n). Each build does one new thing, and every
// line is written to standard output and flushed BEFORE the operation it
// announces. Run as a payload through the ELF loader, standard output is the
// loader's socket, so the lines are on the PC before anything can go wrong.
//
//   0  nothing: the runtime's static initialisation runs, then main returns
//   1  Memory::Initialize only: the 4.5 GiB arena and its views
//   2  step 1, then allocate, fill and release in each guest virtual heap
//   3  step 1, then the physical heap: mirrors, protect, unprotect, release
//   4  step 1, then the system heap and a 256 MiB commit
//   5  no arena. One fault, resolved by unprotecting the page. The handler
//      writes the registers back unchanged
//   6  no arena. One fault, resolved by skipping the instruction: the handler
//      writes a changed instruction pointer back
//   7  no arena. One fault, resolved by emulating a load: the handler writes a
//      changed register and instruction pointer back
//
// Steps 5 to 7 use the runtime's PS5 fault handler without the arena, so a
// problem there is not confused with a memory-layout problem.

#include <sys/mman.h>
#include <unistd.h>

#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>

#include <rex/exception_handler.h>
#include <rex/memory.h>
#include <rex/system/xmemory.h>

#ifndef MCLA_STEP
#error "build with -DMCLA_STEP=0..7"
#endif

#include <signal.h>
#include <ucontext.h>

// --- Early crash reporter -------------------------------------------------------
//
// Step 5 as first built printed nothing at all: the process died while the
// runtime's static objects were being constructed, before main. This installs
// plain signal handlers from a constructor that runs ahead of those, and on a
// crash prints the signal, the fault address and the instruction pointer as an
// offset from this image, then exits. It only reads the context (at the
// position measured by probe 2) and never returns into it.

extern "C" char __executable_start[] __attribute__((weak));

namespace {

void EarlyWrite(const char* text) {
  (void)!write(1, text, std::strlen(text));
}

void EarlyHex(const char* label, uint64_t value) {
  char buffer[96];
  static const char digits[] = "0123456789abcdef";
  size_t n = 0;
  while (*label) buffer[n++] = *label++;
  buffer[n++] = '0';
  buffer[n++] = 'x';
  for (int shift = 60; shift >= 0; shift -= 4) buffer[n++] = digits[(value >> shift) & 0xF];
  buffer[n++] = '\n';
  (void)!write(1, buffer, n);
}

int EarlyAnchor() { return 0; }

void EarlyCrash(int signal_number, siginfo_t* info, void* context) {
  EarlyWrite("CRASH before or during the test\n");
  EarlyHex("  signal ", static_cast<uint64_t>(signal_number));
  EarlyHex("  fault address ", reinterpret_cast<uint64_t>(info->si_addr));
  // The machine context is 0x40 bytes into the signal context (probe 2),
  // whatever the SDK's ucontext_t says.
  auto* actual = reinterpret_cast<mcontext_t*>(static_cast<uint8_t*>(context) + 0x40);
  const uint64_t rip = static_cast<uint64_t>(actual->mc_rip);
  EarlyHex("  instruction pointer ", rip);
  EarlyHex("  address of EarlyAnchor ", reinterpret_cast<uint64_t>(&EarlyAnchor));
  EarlyHex("  rsp ", static_cast<uint64_t>(actual->mc_rsp));
  // A few return addresses from the stack, for a rough backtrace.
  const uint64_t* stack = reinterpret_cast<const uint64_t*>(actual->mc_rsp);
  const uint64_t anchor = reinterpret_cast<uint64_t>(&EarlyAnchor);
  int printed = 0;
  for (int i = 0; i < 512 && printed < 12; ++i) {
    const uint64_t word = stack[i];
    if (word > anchor - 0x4000000 && word < anchor + 0x4000000) {
      EarlyHex("  stack code pointer ", word);
      ++printed;
    }
  }
  _exit(100 + signal_number);
}

__attribute__((constructor(101))) void InstallEarlyCrashReporter() {
  EarlyWrite("early constructor: installing the crash reporter\n");
  struct sigaction action;
  std::memset(&action, 0, sizeof action);
  action.sa_sigaction = EarlyCrash;
  action.sa_flags = SA_SIGINFO;
  sigemptyset(&action.sa_mask);
  for (int signal_number : {SIGSEGV, SIGBUS, SIGILL, SIGABRT, SIGFPE}) {
    sigaction(signal_number, &action, nullptr);
  }
}

}  // namespace

namespace {

int g_passed = 0;
int g_failed = 0;

void Line(const char* format, ...) {
  char text[512];
  va_list args;
  va_start(args, format);
  std::vsnprintf(text, sizeof text, format, args);
  va_end(args);
  std::printf("%s\n", text);
  std::fflush(stdout);
}

// Printed before an operation, so the last "NEXT" line names what was running
// if the output stops.
#define NEXT(...) Line("NEXT " __VA_ARGS__)

void Check(bool ok, const char* what) {
  Line("%s %s", ok ? "PASS" : "FAIL", what);
  (ok ? g_passed : g_failed)++;
}

bool PatternRoundTrip(uint8_t* p, const uint8_t* q, size_t size, uint32_t seed) {
  for (size_t i = 0; i < size; i += 4) {
    const uint32_t value = seed + static_cast<uint32_t>(i) * 2654435761u;
    std::memcpy(p + i, &value, 4);
  }
  for (size_t i = 0; i < size; i += 4) {
    const uint32_t value = seed + static_cast<uint32_t>(i) * 2654435761u;
    uint32_t read;
    std::memcpy(&read, q + i, 4);
    if (read != value) {
      return false;
    }
  }
  return true;
}

using namespace rex::memory;
constexpr uint32_t kReserveCommit = kMemoryAllocationReserve | kMemoryAllocationCommit;
constexpr uint32_t kReadWrite = kMemoryProtectRead | kMemoryProtectWrite;
constexpr uint32_t kSize = 0x100000;  // 1 MiB

[[maybe_unused]] void TestVirtualHeaps(Memory& memory) {
  for (uint32_t heap_base : {0x00010000u, 0x40000000u, 0x80000000u, 0x90000000u}) {
    char what[112];
    NEXT("look up the heap at 0x%08X", heap_base);
    BaseHeap* heap = memory.LookupHeap(heap_base);
    std::snprintf(what, sizeof what, "virtual heap at 0x%08X exists", heap_base);
    Check(heap != nullptr, what);
    if (!heap) {
      continue;
    }
    uint32_t address = 0;
    NEXT("allocate 1 MiB in heap 0x%08X (page 0x%X)", heap_base, heap->page_size());
    const bool allocated = heap->Alloc(kSize, heap->page_size(), kReserveCommit, kReadWrite, false, &address);
    std::snprintf(what, sizeof what, "virtual heap 0x%08X: allocate 1 MiB at 0x%08X", heap_base, address);
    Check(allocated, what);
    if (!allocated) {
      continue;
    }
    uint8_t* host = memory.TranslateVirtual(address);
    NEXT("fill and read 1 MiB at host %p", static_cast<void*>(host));
    std::snprintf(what, sizeof what, "virtual heap 0x%08X: 1 MiB reads back", heap_base);
    Check(PatternRoundTrip(host, host, kSize, heap_base), what);
    NEXT("release 0x%08X", address);
    std::snprintf(what, sizeof what, "virtual heap 0x%08X: release", heap_base);
    Check(heap->Release(address), what);
  }
}

[[maybe_unused]] void TestPhysicalHeap(Memory& memory) {
  NEXT("look up the physical heap at 0xA0000000");
  BaseHeap* heap = memory.LookupHeap(0xA0000000);
  Check(heap != nullptr, "physical heap at 0xA0000000 exists");
  if (!heap) {
    return;
  }
  uint32_t address = 0;
  NEXT("allocate 1 MiB in the physical heap (page 0x%X)", heap->page_size());
  const bool allocated = heap->Alloc(kSize, heap->page_size(), kReserveCommit, kReadWrite, false, &address);
  char what[128];
  std::snprintf(what, sizeof what, "physical heap 0xA0000000: allocate 1 MiB at 0x%08X", address);
  Check(allocated, what);
  if (!allocated) {
    return;
  }
  const uint32_t physical = address & 0x1FFFFFFF;
  uint8_t* via_a = memory.TranslateVirtual(address);
  uint8_t* via_physical = memory.TranslatePhysical(address);
  uint8_t* via_c = memory.TranslateVirtual(0xC0000000u + physical);
  Line("physical 0x%08X: via 0xA.. %p, raw physical %p, via 0xC.. %p", physical, static_cast<void*>(via_a),
       static_cast<void*>(via_physical), static_cast<void*>(via_c));

  NEXT("write and read through the 0xA0000000 view");
  Check(PatternRoundTrip(via_a, via_a, kSize, 0xA0A0A0A0u), "physical: write and read through 0xA0000000");
  NEXT("write through 0xA0000000, read through the raw physical view");
  Check(PatternRoundTrip(via_a, via_physical, kSize, 0x1F1F1F1Fu),
        "physical: written through 0xA0000000, read through the raw physical view");
  NEXT("write through the raw view, read through 0xC0000000");
  Check(PatternRoundTrip(via_physical, via_c, kSize, 0xC0C0C0C0u),
        "physical: written through the raw view, read through 0xC0000000");

  if (physical >= 0x1000) {
    const uint32_t guest_e = 0xE0000000u + physical - 0x1000u;
    BaseHeap* heap_e = memory.LookupHeap(guest_e);
    uint8_t* via_e = memory.TranslateVirtual(guest_e);
    Line("0xE view: guest 0x%08X -> host %p, heap offset 0x%X", guest_e, static_cast<void*>(via_e),
         heap_e ? heap_e->host_address_offset() : 0u);
    NEXT("compare the 0xE0000000 view with the raw physical view");
    Check(via_e == via_physical || std::memcmp(via_e, via_physical, kSize) == 0,
          "physical: 0xE0000000 view (4 KiB pages, +0x1000) reads the same memory");
  }

  NEXT("protect 64 KiB read-only through the heap");
  Check(heap->Protect(address, 0x10000, kMemoryProtectRead), "physical: protect 64 KiB read-only");
  NEXT("read from the read-only range (no write)");
  volatile uint8_t sink = via_a[0];
  (void)sink;
  Check(true, "physical: a read-only page is still readable");
  NEXT("protect back to read-write");
  Check(heap->Protect(address, 0x10000, kReadWrite), "physical: protect back to read-write");
  NEXT("write after unprotect");
  via_a[0] = 0x5A;
  Check(via_physical[0] == 0x5A, "physical: writable again after unprotect");
  NEXT("release the physical allocation");
  Check(heap->Release(address), "physical: release");
}

[[maybe_unused]] void TestSystemHeapAndLargeCommit(Memory& memory) {
  NEXT("SystemHeapAlloc(16 KiB)");
  const uint32_t block = memory.SystemHeapAlloc(0x4000);
  char what[96];
  std::snprintf(what, sizeof what, "system heap: allocate 16 KiB at 0x%08X", block);
  Check(block != 0, what);
  if (block) {
    uint8_t* host = memory.TranslateVirtual(block);
    NEXT("fill and read the system heap block");
    Check(PatternRoundTrip(host, host, 0x4000, 0x5E5E5E5Eu), "system heap: reads back");
    NEXT("SystemHeapFree");
    memory.SystemHeapFree(block);
    Check(true, "system heap: free");
  }
  if (BaseHeap* heap = memory.LookupHeap(0x40000000)) {
    uint32_t address = 0;
    constexpr uint32_t kLarge = 256u << 20;
    NEXT("allocate 256 MiB in the 0x40000000 heap");
    const bool allocated = heap->Alloc(kLarge, heap->page_size(), kReserveCommit, kReadWrite, false, &address);
    Check(allocated, "virtual heap 0x40000000: allocate 256 MiB");
    if (allocated) {
      uint8_t* host = memory.TranslateVirtual(address);
      for (uint32_t chunk = 0; chunk < kLarge; chunk += 32u << 20) {
        NEXT("touch 32 MiB at offset %u MiB", chunk >> 20);
        for (uint32_t offset = chunk; offset < chunk + (32u << 20); offset += 0x1000) {
          host[offset] = static_cast<uint8_t>(offset >> 12);
        }
      }
      bool intact = true;
      for (uint32_t offset = 0; offset < kLarge; offset += 0x1000) {
        intact &= host[offset] == static_cast<uint8_t>(offset >> 12);
      }
      Check(intact, "virtual heap 0x40000000: 256 MiB touched and intact");
      NEXT("release 256 MiB");
      Check(heap->Release(address), "virtual heap 0x40000000: release 256 MiB");
    }
  }
}

// --- Fault handler ------------------------------------------------------------

enum class FaultMode { kUnprotect, kSkip, kEmulateLoad };

struct FaultState {
  FaultMode mode = FaultMode::kUnprotect;
  uint8_t* page = nullptr;
  size_t page_size = 0;
  int count = 0;
  uint64_t address = 0;
  uint64_t pc = 0;
  bool was_write = false;
};
FaultState g_fault;

// Both instructions are three bytes long, which the handler relies on to skip
// them: `movb $7,(%rdi)` is C6 07 07 and `movq (%rdi),%rax` is 48 8B 07.
constexpr uint64_t kFaultInstructionLength = 3;

__attribute__((noinline)) void StoreByte(volatile uint8_t* p) {
  __asm__ volatile("movb $7, (%%rdi)" : : "D"(p) : "memory");
}

__attribute__((noinline)) uint64_t LoadQword(const void* p) {
  uint64_t result;
  __asm__ volatile("movq (%%rdi), %%rax" : "=a"(result) : "D"(p) : "memory");
  return result;
}

bool PcInside(uint64_t pc, void* function) {
  const uint64_t start = reinterpret_cast<uint64_t>(function);
  return pc >= start && pc < start + 64;
}

// The handler refuses to resume anywhere it cannot account for. If the
// instruction pointer it was given is not inside the function that faulted,
// the context layout is wrong, and writing into it would hand the kernel a
// corrupted context. In that case it exits the process from the handler.
bool FaultHandler(rex::arch::Exception* ex, void*) {
  const uint64_t address = ex->fault_address();
  const uint64_t page = reinterpret_cast<uint64_t>(g_fault.page);
  if (address < page || address >= page + g_fault.page_size) {
    return false;
  }
  if (++g_fault.count > 3) {
    _exit(3);
  }
  g_fault.address = address;
  g_fault.pc = ex->pc();
  g_fault.was_write =
      ex->access_violation_operation() == rex::arch::Exception::AccessViolationOperation::kWrite;
  void* expected = g_fault.mode == FaultMode::kEmulateLoad ? reinterpret_cast<void*>(&LoadQword)
                                                           : reinterpret_cast<void*>(&StoreByte);
  if (!PcInside(g_fault.pc, expected)) {
    // Async-signal-safe output only.
    static const char message[] = "ABORT handler: instruction pointer is not in the faulting function; "
                                  "context layout is wrong, exiting without writing it back\n";
    (void)!write(1, message, sizeof message - 1);
    _exit(4);
  }
  switch (g_fault.mode) {
    case FaultMode::kUnprotect:
      mprotect(g_fault.page, g_fault.page_size, PROT_READ | PROT_WRITE);
      return true;
    case FaultMode::kSkip:
      ex->set_resume_pc(ex->pc() + kFaultInstructionLength);
      return true;
    case FaultMode::kEmulateLoad:
      ex->ModifyIntRegister(0) = 0x1122334455667788ull;  // rax
      ex->set_resume_pc(ex->pc() + kFaultInstructionLength);
      return true;
  }
  return false;
}

[[maybe_unused]] void TestFault(FaultMode mode) {
  g_fault.page_size = rex::memory::page_size();
  NEXT("map one anonymous page for the fault test");
  g_fault.page = static_cast<uint8_t*>(
      mmap(nullptr, g_fault.page_size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0));
  Check(g_fault.page != MAP_FAILED, "fault: a page to test on");
  if (g_fault.page == MAP_FAILED) {
    return;
  }
  Line("page %p, StoreByte at %p, LoadQword at %p", static_cast<void*>(g_fault.page),
       reinterpret_cast<void*>(&StoreByte), reinterpret_cast<void*>(&LoadQword));
  NEXT("install the runtime's fault handler");
  rex::arch::ExceptionHandler::Install(FaultHandler, nullptr);
  g_fault.mode = mode;
  g_fault.count = 0;

  if (mode == FaultMode::kUnprotect) {
    NEXT("mprotect the page read-only");
    mprotect(g_fault.page, g_fault.page_size, PROT_READ);
    NEXT("write to the read-only page; the handler unprotects it and writes the registers back unchanged");
    StoreByte(g_fault.page + 16);
    Line("after fault: count %d address %p pc %p write %d", g_fault.count,
         reinterpret_cast<void*>(g_fault.address), reinterpret_cast<void*>(g_fault.pc), int(g_fault.was_write));
    Check(g_fault.count == 1, "fault: taken exactly once");
    Check(g_fault.address == reinterpret_cast<uint64_t>(g_fault.page) + 16, "fault: fault address is right");
    Check(g_fault.was_write, "fault: reported as a write");
    Check(g_fault.page[16] == 7, "fault: the write completed after the handler returned");
  } else if (mode == FaultMode::kSkip) {
    NEXT("mprotect the page read-only");
    mprotect(g_fault.page, g_fault.page_size, PROT_READ);
    NEXT("write to the read-only page; the handler writes back an instruction pointer 3 bytes on");
    StoreByte(g_fault.page + 16);
    Line("after fault: count %d pc %p", g_fault.count, reinterpret_cast<void*>(g_fault.pc));
    Check(g_fault.count == 1, "fault: taken exactly once");
    Check(g_fault.page[16] == 0, "fault: the write was skipped, so the changed instruction pointer was applied");
  } else {
    NEXT("mprotect the page inaccessible");
    mprotect(g_fault.page, g_fault.page_size, PROT_NONE);
    NEXT("read the inaccessible page; the handler writes back rax and the instruction pointer");
    const uint64_t loaded = LoadQword(g_fault.page + 32);
    Line("after fault: count %d value 0x%016llx write %d", g_fault.count,
         static_cast<unsigned long long>(loaded), int(g_fault.was_write));
    Check(g_fault.count == 1, "fault: taken exactly once");
    Check(!g_fault.was_write, "fault: reported as a read");
    Check(loaded == 0x1122334455667788ull, "fault: the register value written by the handler was applied");
  }

  NEXT("uninstall the handler and unmap the page");
  rex::arch::ExceptionHandler::Uninstall(FaultHandler, nullptr);
  munmap(g_fault.page, g_fault.page_size);
}

}  // namespace

int main() {
  alarm(60);  // watchdog: the process ends by itself whatever happens
  Line("mcla-arena step %d starts, pid %d", MCLA_STEP, getpid());
  Line("host page size %zu", rex::memory::page_size());

#if MCLA_STEP >= 1 && MCLA_STEP <= 4
  {
  NEXT("construct rex::memory::Memory");
  Memory memory;
  NEXT("Memory::Initialize (shared object, 4.5 GiB reservation, views, heaps)");
  const bool initialized = memory.Initialize();
  Check(initialized, "Memory::Initialize");
  if (initialized) {
    Line("virtual base %p, physical base %p", static_cast<void*>(memory.virtual_membase()),
         static_cast<void*>(memory.physical_membase()));
    Check(memory.physical_membase() == memory.virtual_membase() + 0x100000000ull,
          "physical base is virtual base + 4 GiB");
#if MCLA_STEP == 2
    TestVirtualHeaps(memory);
#elif MCLA_STEP == 3
    TestPhysicalHeap(memory);
#elif MCLA_STEP == 4
    TestSystemHeapAndLargeCommit(memory);
#endif
  }
  NEXT("destroy rex::memory::Memory (unmaps the arena)");
  }
  Line("arena destroyed");
#elif MCLA_STEP == 5
  TestFault(FaultMode::kUnprotect);
#elif MCLA_STEP == 6
  TestFault(FaultMode::kSkip);
#elif MCLA_STEP == 7
  TestFault(FaultMode::kEmulateLoad);
#endif

  Line("mcla-arena step %d ends: %d passed, %d failed", MCLA_STEP, g_passed, g_failed);
  return g_failed ? 1 : 0;
}
