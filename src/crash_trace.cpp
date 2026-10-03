// Guest crash tracer.
//
// When recompiled code faults on the guest null page, log the host call stack
// with each frame mapped back to the guest function it belongs to. Needs no
// debug symbols: host function addresses come from the dispatch table.

#include "generated/default/mcla_init.h"

#include "mcla_app.h"

#include <algorithm>
#include <cstdint>
#include <utility>
#include <vector>

#include <rex/logging.h>
#include <rex/runtime.h>
#include <rex/system/function_dispatcher.h>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace {

constexpr uint32_t kCodeBegin = 0x82130000;
constexpr uint32_t kCodeEnd = 0x827CD054;
constexpr uintptr_t kGuestBase = 0x100000000ull;
constexpr uintptr_t kNullPageSize = 0x10000;
// Largest plausible distance from a function's entry to an address inside it.
constexpr uintptr_t kMaxFunctionSpan = 0x80000;

std::vector<std::pair<uintptr_t, uint32_t>> g_host_to_guest;

uint32_t GuestFunctionFor(uintptr_t host_pc, uintptr_t* offset) {
  auto it = std::upper_bound(g_host_to_guest.begin(), g_host_to_guest.end(),
                             std::make_pair(host_pc, UINT32_MAX));
  if (it == g_host_to_guest.begin()) {
    return 0;
  }
  --it;
  if (host_pc - it->first > kMaxFunctionSpan) {
    return 0;
  }
  *offset = host_pc - it->first;
  return it->second;
}

#ifdef _WIN32
LONG CALLBACK OnException(EXCEPTION_POINTERS* info) {
  static LONG reported = 0;
  const EXCEPTION_RECORD* record = info->ExceptionRecord;
  if (record->ExceptionCode != EXCEPTION_ACCESS_VIOLATION || record->NumberParameters < 2) {
    return EXCEPTION_CONTINUE_SEARCH;
  }
  uintptr_t fault = record->ExceptionInformation[1];
  if (fault < kGuestBase || fault >= kGuestBase + kNullPageSize) {
    return EXCEPTION_CONTINUE_SEARCH;
  }
  if (InterlockedExchange(&reported, 1)) {
    return EXCEPTION_CONTINUE_SEARCH;
  }

  REXLOG_ERROR("guest null-page {} at guest 0x{:08X}; host stack (innermost first):",
               record->ExceptionInformation[0] ? "write" : "read",
               static_cast<uint32_t>(fault - kGuestBase));
  CONTEXT context = *info->ContextRecord;
  for (int frame = 0; frame < 32 && context.Rip; ++frame) {
    uintptr_t offset = 0;
    if (uint32_t guest = GuestFunctionFor(context.Rip, &offset)) {
      REXLOG_ERROR("  #{:<2} sub_{:08X} +0x{:X} (host)", frame, guest, offset);
    } else {
      REXLOG_ERROR("  #{:<2} host 0x{:016X}", frame, static_cast<uint64_t>(context.Rip));
    }
    DWORD64 image_base = 0;
    PRUNTIME_FUNCTION function = RtlLookupFunctionEntry(context.Rip, &image_base, nullptr);
    if (!function) {
      // Leaf function: return address is at the top of the stack.
      context.Rip = *reinterpret_cast<DWORD64*>(context.Rsp);
      context.Rsp += 8;
      continue;
    }
    void* handler_data = nullptr;
    DWORD64 establisher = 0;
    RtlVirtualUnwind(UNW_FLAG_NHANDLER, image_base, context.Rip, function, &context,
                     &handler_data, &establisher, nullptr);
  }
  return EXCEPTION_CONTINUE_SEARCH;
}
#endif

}  // namespace

void MclaApp::InstallCrashTrace() {
  auto* dispatcher = runtime()->function_dispatcher();
  g_host_to_guest.clear();
  for (uint32_t address = kCodeBegin; address < kCodeEnd; address += 4) {
    if (auto* function = dispatcher->GetFunction(address)) {
      g_host_to_guest.emplace_back(reinterpret_cast<uintptr_t>(function), address);
    }
  }
  std::sort(g_host_to_guest.begin(), g_host_to_guest.end());
  REXLOG_INFO("crash trace: {} guest functions mapped", g_host_to_guest.size());
#ifdef _WIN32
  AddVectoredExceptionHandler(1, OnException);
#endif
}
