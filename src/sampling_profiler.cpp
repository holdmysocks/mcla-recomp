// Built-in sampling profiler (development aid, Windows only for now).
//
// --mcla_profile=<file> starts a thread that waits --mcla_profile_delay
// seconds, then for --mcla_profile_seconds samples the instruction pointer of
// every other thread about once a millisecond. Samples inside recompiled game
// code are attributed to their guest function through the dispatch table;
// everything else is written as module + offset for scripts/profile_report.py
// to symbolise.

#include "generated/default/mcla_init.h"

#include "mcla_app.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <rex/cvar.h>
#include <rex/logging.h>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <tlhelp32.h>
#endif

REXCVAR_DEFINE_STRING(mcla_profile, "", "MCLA", "Write a sampling profile to this file");
REXCVAR_DEFINE_INT32(mcla_profile_delay, 40, "MCLA", "Seconds to wait before sampling")
    .range(0, 3600);
REXCVAR_DEFINE_INT32(mcla_profile_seconds, 15, "MCLA", "Seconds to sample for").range(1, 600);

// Defined in crash_trace.cpp.
uint32_t MclaGuestFunctionForHostPc(uintptr_t host_pc);

#ifdef _WIN32
namespace {

struct Sample {
  uint32_t thread_id;
  uint64_t pc;
};

std::vector<uint32_t> OtherThreadIds() {
  std::vector<uint32_t> ids;
  const DWORD pid = GetCurrentProcessId();
  const DWORD self = GetCurrentThreadId();
  HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
  if (snapshot == INVALID_HANDLE_VALUE) {
    return ids;
  }
  THREADENTRY32 entry{};
  entry.dwSize = sizeof entry;
  if (Thread32First(snapshot, &entry)) {
    do {
      if (entry.th32OwnerProcessID == pid && entry.th32ThreadID != self) {
        ids.push_back(entry.th32ThreadID);
      }
    } while (Thread32Next(snapshot, &entry));
  }
  CloseHandle(snapshot);
  return ids;
}

void RunProfile(std::string path, int delay_s, int seconds) {
  Sleep(static_cast<DWORD>(delay_s) * 1000);

  std::vector<std::pair<uint32_t, HANDLE>> threads;
  for (uint32_t id : OtherThreadIds()) {
    HANDLE h = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION,
                          FALSE, id);
    if (h) {
      threads.emplace_back(id, h);
    }
  }

  // Nothing may allocate or log between SuspendThread and ResumeThread: the
  // suspended thread could be holding the heap or logger lock.
  std::vector<Sample> samples;
  samples.resize(static_cast<size_t>(seconds) * 1100 * threads.size());
  size_t count = 0;
  const ULONGLONG end = GetTickCount64() + static_cast<ULONGLONG>(seconds) * 1000;
  uint64_t rounds = 0;
  while (GetTickCount64() < end && count + threads.size() <= samples.size()) {
    for (auto& [id, handle] : threads) {
      if (SuspendThread(handle) == static_cast<DWORD>(-1)) {
        continue;
      }
      CONTEXT context;
      context.ContextFlags = CONTEXT_CONTROL;
      if (GetThreadContext(handle, &context)) {
        samples[count++] = {id, context.Rip};
      }
      ResumeThread(handle);
    }
    ++rounds;
    Sleep(1);
  }
  for (auto& [id, handle] : threads) {
    CloseHandle(handle);
  }

  // Aggregate: (thread, location) -> hits. A location is a guest function,
  // or module+offset.
  std::map<std::pair<uint32_t, std::string>, uint32_t> hits;
  char buffer[MAX_PATH + 32];
  for (size_t i = 0; i < count; ++i) {
    const uint64_t pc = samples[i].pc;
    std::string where;
    if (uint32_t guest = MclaGuestFunctionForHostPc(static_cast<uintptr_t>(pc))) {
      std::snprintf(buffer, sizeof buffer, "guest sub_%08X", guest);
      where = buffer;
    } else {
      HMODULE module = nullptr;
      if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                 GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                             reinterpret_cast<LPCSTR>(pc), &module) &&
          module) {
        char name[MAX_PATH];
        GetModuleFileNameA(module, name, sizeof name);
        const char* base = std::strrchr(name, '\\');
        std::snprintf(buffer, sizeof buffer, "module %s 0x%llX", base ? base + 1 : name,
                      static_cast<unsigned long long>(pc - reinterpret_cast<uint64_t>(module)));
      } else {
        std::snprintf(buffer, sizeof buffer, "unknown 0x%llX", static_cast<unsigned long long>(pc));
      }
      where = buffer;
    }
    ++hits[{samples[i].thread_id, where}];
  }

  if (FILE* f = std::fopen(path.c_str(), "w")) {
    std::fprintf(f, "# rounds=%llu seconds=%d threads=%zu samples=%zu\n",
                 static_cast<unsigned long long>(rounds), seconds, threads.size(), count);
    for (const auto& [key, n] : hits) {
      std::fprintf(f, "%u\t%u\t%s\n", key.first, n, key.second.c_str());
    }
    std::fclose(f);
  }
  REXLOG_INFO("profile: {} samples over {} rounds written to {}", count, rounds, path);
}

}  // namespace
#endif

void MclaApp::StartProfilerIfRequested() {
#ifdef _WIN32
  std::string path = REXCVAR_GET(mcla_profile);
  if (path.empty()) {
    return;
  }
  std::thread(RunProfile, path, static_cast<int>(REXCVAR_GET(mcla_profile_delay)),
              static_cast<int>(REXCVAR_GET(mcla_profile_seconds)))
      .detach();
#endif
}
