// In-game settings menu.
//
// Adds submenus to the Settings tab of the game's own pause menu, with rows
// bound to this project's options. The rows are objects of the game's own
// option classes (checkbox and value rows), so they look and behave like the
// rest of the menu.
//
// Ported from LARecomp's src/mc_engine/pause_menu.cpp with its author's
// permission; the reverse engineering of the menu system is theirs. See
// config/settings_menu.toml for what each hook address is. Left out: their
// carbon-fibre, cutscene, language and native-renderer tabs.

#include "generated/default/mcla_init.h"

#include "mcla_app.h"

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>

#include <rex/cvar.h>
#include <rex/filesystem.h>
#include <rex/logging.h>
#include <rex/ppc.h>
#include <rex/runtime.h>
#include <rex/system/function_dispatcher.h>
#include <rex/ui/windowed_app_context.h>

REXCVAR_DEFINE_BOOL(mcla_motion_blur, true, "MCLA", "Motion blur");
REXCVAR_DEFINE_BOOL(mcla_depth_of_field, true, "MCLA", "Depth of field blur");
REXCVAR_DEFINE_BOOL(mcla_settings_menu, true, "MCLA",
                    "Add the settings submenus to the pause menu");

namespace {

// --- Guest addresses ---------------------------------------------------------

constexpr uint32_t kStringTableGlobal = 0x8286D7FC;
constexpr uint32_t kMenuSystemGlobal = 0x8286D804;
constexpr uint32_t kAllocStateFn = 0x8268D578;
constexpr uint32_t kUIObjectCtorFn = 0x8263AD60;
constexpr uint32_t kUIMenuVtable = 0x82020654;
constexpr uint32_t kGuestMallocFn = 0x82130528;
constexpr uint32_t kStackPushFn = 0x8268F6A8;
constexpr uint32_t kStackPopFn = 0x8268DFF0;
constexpr uint32_t kIsButtonClickedFn = 0x82661508;
constexpr uint32_t kFindMovieFn = 0x821F9FB8;
constexpr uint32_t kSetFlashIntFn = 0x825EE0E0;
constexpr uint32_t kGetFlashObjFn = 0x825ED480;
constexpr uint32_t kSetFlashStrFn = 0x827227B8;

constexpr uint32_t kOptionRowCtorFn = 0x826349F0;  // (obj, nameKey, values, wrap)
constexpr uint32_t kLabelRowCtorFn = 0x82633D40;   // (obj, nameKey, childCap)
constexpr uint32_t kSetRowLabelFn = 0x8263B860;    // (obj, nameKey)
constexpr uint32_t kStrListVtable = 0x82091D04;    // {vtable, char** keys, count, stride}
constexpr uint32_t kToggleRowVtable = 0x8209788C;

constexpr uint32_t kStr_PAUSEMOVIE = 0x8201D028;
constexpr uint32_t kStr_menu = 0x82030A34;
constexpr uint32_t kStr_tabs_count = 0x8208D4DC;
constexpr uint32_t kStr_name = 0x8200CC24;

// Row object layout.
constexpr uint32_t kRowObjectSize = 240;
constexpr uint32_t kRowChecked = 208;    // u8, checkbox rows
constexpr uint32_t kRowHasBox = 209;     // u8
constexpr uint32_t kRowEnabled = 212;
constexpr uint32_t kRowProperty1 = 216;
constexpr uint32_t kRowSelect = 208;     // value rows: current value slot

// List view layout.
constexpr uint32_t kListRows = 176;         // row pointer array
constexpr uint32_t kListCountCap = 180;     // count:u16 then capacity:u16
constexpr uint32_t kListSelect = 184;
constexpr uint32_t kListTableSource = 192;
constexpr uint32_t kListStateSource = 196;
constexpr uint32_t kListScroll = 256;

// --- Guest memory helpers ----------------------------------------------------

uint8_t* Membase() {
  auto* runtime = rex::Runtime::instance();
  return runtime ? runtime->virtual_membase() : nullptr;
}

uint32_t Read32(uint32_t ea) {
  const uint8_t* p = Membase() + ea;
  return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3];
}

uint16_t Read16(uint32_t ea) {
  const uint8_t* p = Membase() + ea;
  return static_cast<uint16_t>((p[0] << 8) | p[1]);
}

void Write32(uint32_t ea, uint32_t value) {
  uint8_t* p = Membase() + ea;
  p[0] = uint8_t(value >> 24);
  p[1] = uint8_t(value >> 16);
  p[2] = uint8_t(value >> 8);
  p[3] = uint8_t(value);
}

void Write8(uint32_t ea, uint8_t value) {
  Membase()[ea] = value;
}

template <typename... Args>
uint32_t CallGuest(uint32_t function_address, Args... args) {
  auto* runtime = rex::Runtime::instance();
  PPCFunc* function = runtime ? runtime->function_dispatcher()->GetFunction(function_address)
                              : nullptr;
  if (!function) {
    REXLOG_WARN("settings menu: guest function 0x{:08X} is not available", function_address);
    return 0;
  }
  return rex::ppc::GuestToHostFunction<uint32_t>(function, static_cast<uint32_t>(args)...);
}

uint32_t AllocGuestString(const char* text) {
  const size_t size = std::strlen(text) + 1;
  uint32_t buffer = CallGuest(kGuestMallocFn, uint32_t(size));
  if (buffer) {
    std::memcpy(Membase() + buffer, text, size);
  }
  return buffer;
}

// --- The game's string table -------------------------------------------------

// The game's string hash (sub_821C9790).
uint32_t HashString(const char* text) {
  uint32_t hash = 0;
  const bool quoted = (*text == '"');
  if (quoted) {
    ++text;
  }
  while (*text) {
    char c = *text;
    if (quoted && c == '"') {
      break;
    }
    ++text;
    uint8_t ch = static_cast<uint8_t>(c);
    if (ch >= 'A' && ch <= 'Z') {
      ch += 32;
    } else if (ch == '\\') {
      ch = '/';
    }
    uint32_t v = ch + hash;
    hash = ((1025u * v) >> 6) ^ (1025u * v);
  }
  return 32769u * (((9u * hash) >> 11) ^ (9u * hash));
}

uint32_t HashMapLookup(uint32_t map, uint32_t hash) {
  uint32_t buckets = Read32(map);
  uint16_t bucket_count = Read16(map + 4);
  if (!bucket_count || !buckets) {
    return 0;
  }
  for (uint32_t node = Read32(buckets + 4 * (hash % bucket_count)); node; node = Read32(node + 8)) {
    if (Read32(node) == hash) {
      return Read32(node + 4);
    }
  }
  return 0;
}

bool HashMapInsert(uint32_t map, uint32_t hash, uint32_t value) {
  uint32_t buckets = Read32(map);
  uint16_t bucket_count = Read16(map + 4);
  if (!bucket_count || !buckets) {
    return false;
  }
  uint32_t node = CallGuest(kGuestMallocFn, 12u);
  if (!node) {
    return false;
  }
  const uint32_t slot = buckets + 4 * (hash % bucket_count);
  Write32(node + 0, hash);
  Write32(node + 4, value);
  Write32(node + 8, Read32(slot));
  Write32(slot, node);
  return true;
}

// Sets the display text for a string-table key, creating the entry if needed.
// Text buffers are 132 bytes.
void SetStringTableText(const char* key, const char* text) {
  uint32_t table = Read32(kStringTableGlobal);
  if (!table) {
    return;
  }
  size_t length = std::strlen(text);
  if (length > 131) {
    length = 131;
  }
  const uint32_t hash = HashString(key);
  uint32_t buffer = HashMapLookup(table + 16, hash);
  if (!buffer) {
    buffer = CallGuest(kGuestMallocFn, 132u);
    if (!buffer || !HashMapInsert(table + 16, hash, buffer)) {
      return;
    }
  }
  std::memcpy(Membase() + buffer, text, length);
  Membase()[buffer + length] = 0;
}

// --- The game's menu state tree ----------------------------------------------

void DetachMenuItem(uint32_t child) {
  uint32_t parent = Read32(child + 32);
  if (!parent) {
    return;
  }
  uint32_t previous = Read32(child + 40);
  uint32_t next = Read32(child + 36);
  if (previous) {
    Write32(previous + 36, next);
  } else {
    Write32(parent + 44, next);
  }
  if (next) {
    Write32(next + 40, previous);
  }
  Write32(child + 32, 0);
  Write32(child + 36, 0);
  Write32(child + 40, 0);
}

// Reimplements sub_8268CC80.
void AppendMenuItem(uint32_t parent, uint32_t child) {
  if (!parent || !child || Read32(child + 32) != 0) {
    return;
  }
  uint32_t head = Read32(parent + 44);
  if (!head) {
    Write32(parent + 44, child);
    Write32(child + 40, 0);
  } else {
    uint32_t last = head;
    for (uint32_t next; (next = Read32(last + 36)) != 0; last = next) {
    }
    Write32(last + 36, child);
    Write32(child + 40, last);
  }
  Write32(child + 32, parent);
  Write32(child + 36, 0);
}

uint32_t MenuRootNode() {
  uint32_t root = Read32(kMenuSystemGlobal);
  return root ? Read32(root + 52) : 0;
}

uint32_t MenuEngine() {
  uint32_t node = MenuRootNode();
  return node ? node + 4 : 0;
}

void SetStateName(uint32_t state, const char* name) {
  if (uint32_t buffer = AllocGuestString(name)) {
    Write32(state + 20, buffer);
  }
}

// A plain menu state, as the engine's own factory makes them.
uint32_t CreateMenuState(const char* name) {
  uint32_t engine = MenuEngine();
  if (!engine) {
    return 0;
  }
  uint32_t state = CallGuest(kAllocStateFn, engine);
  if (!state) {
    return 0;
  }
  const uint32_t index = Read32(engine + 80) - 1;
  HashMapInsert(engine + 88, HashString(name), index);
  SetStateName(state, name);
  Write32(state + 16, 0x42);
  for (uint32_t offset = 24; offset <= 44; offset += 4) {
    Write32(state + offset, 0);
  }
  return state;
}

// A "UIMenu" state: what the class factory (sub_82222080) builds for one.
uint32_t CreateUIMenuState(const char* name) {
  uint32_t engine = MenuEngine();
  if (!engine) {
    return 0;
  }
  uint32_t state = CallGuest(kGuestMallocFn, 56u);
  if (!state) {
    return 0;
  }
  CallGuest(kUIObjectCtorFn, state);
  Write32(state, kUIMenuVtable);
  uint32_t array = Read32(engine + 76);
  uint32_t count = Read32(engine + 80);
  if (!array) {
    return 0;
  }
  Write32(array + 4 * count, state);
  Write32(engine + 80, count + 1);
  HashMapInsert(engine + 88, HashString(name), count);
  SetStateName(state, name);
  return state;
}

bool MenuStackPush(uint32_t state) {
  uint32_t node = MenuRootNode();
  if (!node) {
    return false;
  }
  CallGuest(kStackPushFn, node + 776, node + 4, state, 0u);
  return true;
}

void MenuStackPop() {
  if (uint32_t node = MenuRootNode()) {
    CallGuest(kStackPopFn, node + 776, node + 4, 1u);
  }
}

// --- Options -----------------------------------------------------------------

std::string OptionGet(const char* name) {
  return rex::cvar::GetFlagByName(name);
}

void OptionSet(const char* name, const char* value) {
  REXLOG_INFO("settings menu: {} = {}", name, value);
  rex::cvar::SetFlagByName(name, value);
}

enum class ItemKind { kToggle, kStringChoice, kNumberChoice };

struct ItemDef {
  const char* key;    // menu state name and string-table key; must be unique
  ItemKind kind;
  const char* option;
  const char* label;
  const char* const* values = nullptr;        // kStringChoice: option values
  const char* const* value_labels = nullptr;  // kStringChoice: display text
  const double* numbers = nullptr;            // kNumberChoice
  int value_count = 0;
  const char* number_format = nullptr;
  const char* zero_label = nullptr;  // kNumberChoice: shown instead of 0
};

// Display text must stay within A-Z, 0-9, space, '+', '.', and parentheses.
// A value's text is also the name of the Flash element created for it, and
// characters such as '/' are path separators there (LARecomp found that the
// hard way: it crashes UI setup).
constexpr double kFpsTargets[] = {30.0, 60.0, 120.0, 0.0};
constexpr const char* kPromptValues[] = {"auto", "xbox", "playstation"};
constexpr const char* kPromptLabels[] = {"AUTO", "XBOX", "PLAYSTATION"};

const ItemDef kDisplayItems[] = {
    {"PM_MxFps", ItemKind::kNumberChoice, "mcla_fps", "FPS TARGET", nullptr, nullptr, kFpsTargets,
     4, "%.0f", "UNCAPPED"},
    {"PM_MxVsync", ItemKind::kToggle, "vsync", "VSYNC"},
    {"PM_MxFullscreen", ItemKind::kToggle, "fullscreen", "FULLSCREEN"},
    {"PM_MxMotionBlur", ItemKind::kToggle, "mcla_motion_blur", "MOTION BLUR"},
    {"PM_MxDof", ItemKind::kToggle, "mcla_depth_of_field", "DEPTH OF FIELD"},
};

const ItemDef kPerformanceItems[] = {
    {"PM_MxSingleTile", ItemKind::kToggle, "mcla_single_tile", "SINGLE TILE RENDERING"},
    {"PM_MxFenceYield", ItemKind::kToggle, "mcla_fence_yield", "GPU WAIT YIELD"},
};

const ItemDef kControlItems[] = {
    {"PM_MxPrompts", ItemKind::kStringChoice, "mcla_button_prompts", "BUTTON PROMPTS",
     kPromptValues, kPromptLabels, nullptr, 3},
    {"PM_MxSkipIntro", ItemKind::kToggle, "mcla_skip_intro", "SKIP INTRO (NO LOGO)"},
};

struct MenuDef {
  const char* button_key;  // button state name and its string-table key
  const char* label;       // button label and submenu title
  const char* menu_key;    // submenu state name
  const ItemDef* items;
  int item_count;
};

const MenuDef kMenus[] = {
    {"PM_MxTabDisplay", "DISPLAY", "MxDisplayMenu", kDisplayItems,
     int(sizeof(kDisplayItems) / sizeof(kDisplayItems[0]))},
    {"PM_MxTabPerf", "PERFORMANCE", "MxPerfMenu", kPerformanceItems,
     int(sizeof(kPerformanceItems) / sizeof(kPerformanceItems[0]))},
    {"PM_MxTabControls", "CONTROLS", "MxControlsMenu", kControlItems,
     int(sizeof(kControlItems) / sizeof(kControlItems[0]))},
};
constexpr int kMenuCount = int(sizeof(kMenus) / sizeof(kMenus[0]));
constexpr int kMaxRows = 16;

int ItemValueCount(const ItemDef& item) {
  return item.kind == ItemKind::kToggle ? 2 : item.value_count;
}

std::string ItemValueLabel(const ItemDef& item, int index) {
  switch (item.kind) {
    case ItemKind::kToggle:
      return index ? "ON" : "OFF";
    case ItemKind::kStringChoice:
      return item.value_labels[index];
    case ItemKind::kNumberChoice: {
      if (item.zero_label && item.numbers[index] == 0.0) {
        return item.zero_label;
      }
      char buffer[48];
      std::snprintf(buffer, sizeof buffer, item.number_format, item.numbers[index]);
      return buffer;
    }
  }
  return "";
}

int ItemCurrentIndex(const ItemDef& item) {
  const std::string value = OptionGet(item.option);
  switch (item.kind) {
    case ItemKind::kToggle:
      return value == "true" ? 1 : 0;
    case ItemKind::kStringChoice:
      for (int i = 0; i < item.value_count; ++i) {
        if (value == item.values[i]) {
          return i;
        }
      }
      return 0;
    case ItemKind::kNumberChoice: {
      const double number = std::strtod(value.c_str(), nullptr);
      int best = 0;
      double best_distance = -1.0;
      for (int i = 0; i < item.value_count; ++i) {
        const double distance = number > item.numbers[i] ? number - item.numbers[i]
                                                         : item.numbers[i] - number;
        if (best_distance < 0.0 || distance < best_distance) {
          best_distance = distance;
          best = i;
        }
      }
      return best;
    }
  }
  return 0;
}

std::atomic<bool> g_settings_dirty{false};

void ItemSetIndex(const ItemDef& item, int index) {
  if (index < 0 || index >= ItemValueCount(item)) {
    return;
  }
  switch (item.kind) {
    case ItemKind::kToggle:
      OptionSet(item.option, index ? "true" : "false");
      break;
    case ItemKind::kStringChoice:
      OptionSet(item.option, item.values[index]);
      break;
    case ItemKind::kNumberChoice: {
      char buffer[32];
      std::snprintf(buffer, sizeof buffer, "%g", item.numbers[index]);
      OptionSet(item.option, buffer);
      break;
    }
  }
  g_settings_dirty.store(true, std::memory_order_relaxed);
}

// --- Rows --------------------------------------------------------------------

// A value row shows three slots (previous, current, next) with the selection
// on the middle one. The game's row class writes one Flash element per value
// without checking how many the row clip has, and three is what its own
// numeric rows use, so the window never exceeds three however long the real
// list is. After every step the window is re-centred, which also makes a step
// readable: a selection that is no longer on slot 1 is the direction pressed.
constexpr int kRowWindow = 3;

struct MenuRows {
  uint32_t array = 0;
  uint32_t row[kMaxRows] = {};
  int last_index[kMaxRows] = {};
  int count = 0;
};

MenuRows g_rows[kMenuCount];
std::atomic<uint32_t> g_menu_button_key[kMenuCount] = {};
std::atomic<uint32_t> g_menu_state[kMenuCount] = {};
std::atomic<uint32_t> g_menu_title[kMenuCount] = {};
std::atomic<uint32_t> g_quit_button_key{0};
std::atomic<int> g_active_menu{-1};
std::atomic<bool> g_menus_created{false};

std::atomic<uint32_t> g_pause_tab{0};
std::atomic<uint32_t> g_settings_state{0};
std::atomic<uint32_t> g_last_list{0};
std::atomic<uint32_t> g_pause_list{0};

// The pause list's own row sources, saved while one of our submenus borrows it.
struct SavedList {
  uint32_t rows, count_cap, table, state, select, scroll;
};
SavedList g_saved_list{};

void WindowKey(const ItemDef& item, int slot, char* out, size_t size) {
  std::snprintf(out, size, "%s_w%d", item.key, slot);
}

void CentreRowWindow(MenuRows& rows, int i, const ItemDef& item, int index) {
  const int count = ItemValueCount(item);
  for (int slot = 0; slot < kRowWindow; ++slot) {
    char key[96];
    WindowKey(item, slot, key, sizeof key);
    const int value = ((index + slot - 1) % count + count) % count;
    SetStringTableText(key, ItemValueLabel(item, value).c_str());
  }
  rows.last_index[i] = index;
  Write32(rows.row[i] + kRowSelect, 1);
}

uint32_t BuildValueSet(const ItemDef& item) {
  uint32_t keys = CallGuest(kGuestMallocFn, uint32_t(4 * kRowWindow));
  if (!keys) {
    return 0;
  }
  for (int slot = 0; slot < kRowWindow; ++slot) {
    char key[96];
    WindowKey(item, slot, key, sizeof key);
    SetStringTableText(key, "");
    uint32_t key_string = AllocGuestString(key);
    if (!key_string) {
      return 0;
    }
    Write32(keys + uint32_t(4 * slot), key_string);
  }
  uint32_t set = CallGuest(kGuestMallocFn, 16u);
  if (!set) {
    return 0;
  }
  Write32(set + 0, kStrListVtable);
  Write32(set + 4, keys);
  Write32(set + 8, uint32_t(kRowWindow));
  Write32(set + 12, 1);
  return set;
}

bool BuildRows(int menu) {
  MenuRows& rows = g_rows[menu];
  if (rows.array) {
    return true;
  }
  const MenuDef& def = kMenus[menu];
  if (def.item_count > kMaxRows) {
    return false;
  }
  uint32_t array = CallGuest(kGuestMallocFn, uint32_t(4 * def.item_count));
  if (!array) {
    return false;
  }
  for (int i = 0; i < def.item_count; ++i) {
    const ItemDef& item = def.items[i];
    // The row constructor resolves its label through the string table once,
    // so the text has to exist first.
    SetStringTableText(item.key, item.label);
    uint32_t name = AllocGuestString(item.key);
    uint32_t row = CallGuest(kGuestMallocFn, kRowObjectSize);
    if (!name || !row) {
      return false;
    }
    std::memset(Membase() + row, 0, kRowObjectSize);

    if (item.kind == ItemKind::kToggle) {
      // The checkbox class has no constructor of its own: the label row's
      // constructor runs and the checkbox vtable is written over it.
      CallGuest(kLabelRowCtorFn, row, name, 0u);
      Write32(row, kToggleRowVtable);
      Write8(row + kRowChecked, uint8_t(ItemCurrentIndex(item)));
      Write8(row + kRowHasBox, 1);
      Write32(row + kRowEnabled, 1);
      Write32(row + kRowProperty1, 0xFFFFFFFFu);
      rows.row[i] = row;
      rows.last_index[i] = ItemCurrentIndex(item);
    } else {
      uint32_t set = BuildValueSet(item);
      if (!set) {
        return false;
      }
      CallGuest(kOptionRowCtorFn, row, name, set, 1u);
      rows.row[i] = row;
      CentreRowWindow(rows, i, item, ItemCurrentIndex(item));
    }
    Write32(array + uint32_t(4 * i), row);
  }
  rows.count = def.item_count;
  rows.array = array;
  REXLOG_INFO("settings menu: '{}' built with {} rows", def.label, rows.count);
  return true;
}

// Points the list at our rows. The renderer only reaches the row array when
// the two higher-priority sources are null, so those are cleared.
void InstallRows(int menu, uint32_t list) {
  MenuRows& rows = g_rows[menu];
  if (!list || !rows.array || rows.count <= 0) {
    return;
  }
  const uint32_t count_cap = (uint32_t(rows.count) << 16) | uint32_t(rows.count);
  Write32(list + kListStateSource, 0);
  Write32(list + kListTableSource, 0);
  Write32(list + kListRows, rows.array);
  Write32(list + kListCountCap, count_cap);
}

// Brings every row in line with the current option values.
void SyncRowsFromOptions(int menu) {
  MenuRows& rows = g_rows[menu];
  const MenuDef& def = kMenus[menu];
  for (int i = 0; i < rows.count; ++i) {
    const ItemDef& item = def.items[i];
    const int index = ItemCurrentIndex(item);
    if (item.kind == ItemKind::kToggle) {
      rows.last_index[i] = index;
      Write8(rows.row[i] + kRowChecked, uint8_t(index));
    } else {
      CentreRowWindow(rows, i, item, index);
    }
  }
}

// Left and right are handled inside the row, which then re-renders the list.
// The render is where the move is noticed and written to the option.
void PollRows(int menu) {
  MenuRows& rows = g_rows[menu];
  const MenuDef& def = kMenus[menu];
  for (int i = 0; i < rows.count; ++i) {
    const ItemDef& item = def.items[i];
    const int count = ItemValueCount(item);
    if (item.kind == ItemKind::kToggle) {
      const int checked = ItemCurrentIndex(item);
      if (checked != rows.last_index[i]) {
        rows.last_index[i] = checked;
        Write8(rows.row[i] + kRowChecked, uint8_t(checked));
      }
      continue;
    }
    const int slot = int(Read32(rows.row[i] + kRowSelect));
    if (slot != 1) {
      const int direction = slot < 1 ? -1 : +1;
      const int index = ((rows.last_index[i] + direction) % count + count) % count;
      ItemSetIndex(item, index);
      CentreRowWindow(rows, i, item, index);
    }
  }
}

// --- Pause list control ------------------------------------------------------

uint32_t PauseMovieContext() {
  uint32_t root = Read32(kMenuSystemGlobal);
  if (!root) {
    return 0;
  }
  uint32_t movie = CallGuest(kFindMovieFn, root, kStr_PAUSEMOVIE);
  return movie ? Read32(movie + 56) : 0;
}

void RefreshPauseList() {
  uint32_t list = g_pause_list.load(std::memory_order_relaxed);
  if (!list) {
    return;
  }
  uint32_t vtable = Read32(list);
  uint32_t function = vtable ? Read32(vtable + 176) : 0;
  if (function) {
    CallGuest(function, list);
  }
}

void ShowSubmenu() {
  const int menu = g_active_menu.load(std::memory_order_relaxed);
  uint32_t list = g_pause_list.load(std::memory_order_relaxed);
  if (menu < 0 || !list) {
    return;
  }
  InstallRows(menu, list);
  RefreshPauseList();

  uint32_t context = PauseMovieContext();
  if (!context) {
    return;
  }
  // Hide the tab strip so the movie presents a submenu, as the game's own
  // time-of-day menu does.
  CallGuest(kSetFlashIntFn, context, kStr_tabs_count, 0u);

  uint32_t title = g_menu_title[menu].load(std::memory_order_relaxed);
  if (!title) {
    title = AllocGuestString(kMenus[menu].label);
    g_menu_title[menu].store(title, std::memory_order_relaxed);
  }
  uint32_t menu_object = CallGuest(kGetFlashObjFn, context, HashString("menu"), kStr_menu);
  if (menu_object && title) {
    CallGuest(kSetFlashStrFn, menu_object, kStr_name, title);
  }
}

void RestorePauseList() {
  uint32_t list = g_pause_list.load(std::memory_order_relaxed);
  if (!list) {
    return;
  }
  Write32(list + kListRows, g_saved_list.rows);
  Write32(list + kListCountCap, g_saved_list.count_cap);
  Write32(list + kListTableSource, g_saved_list.table);
  Write32(list + kListStateSource, g_saved_list.state);
  Write32(list + kListSelect, g_saved_list.select);
  Write32(list + kListScroll, g_saved_list.scroll);
  RefreshPauseList();
}

void SaveSettingsIfChanged() {
  if (!g_settings_dirty.exchange(false, std::memory_order_relaxed)) {
    return;
  }
  // The file the runtime loads at start: <executable folder>/mcla.toml.
  const auto path = rex::filesystem::GetExecutableFolder() / "mcla.toml";
  rex::cvar::SaveConfig(path);
  REXLOG_INFO("settings menu: saved to {}", path.string());
}

void CreateSubmenus() {
  if (g_menus_created.load(std::memory_order_relaxed)) {
    return;
  }
  uint32_t settings = g_settings_state.load(std::memory_order_relaxed);
  if (!settings) {
    return;
  }
  for (int menu = 0; menu < kMenuCount; ++menu) {
    const MenuDef& def = kMenus[menu];

    uint32_t button = CreateMenuState(def.button_key);
    if (!button) {
      return;
    }
    AppendMenuItem(settings, button);
    SetStringTableText(def.button_key, def.label);
    g_menu_button_key[menu].store(AllocGuestString(def.button_key), std::memory_order_relaxed);

    uint32_t submenu = CreateUIMenuState(def.menu_key);
    if (!submenu) {
      return;
    }
    // Give the submenu a parent pointer without listing it as a child: the
    // engine's "find my root" walk dereferences a null result for a state
    // with no parent.
    Write32(submenu + 32, settings);
    g_menu_state[menu].store(submenu, std::memory_order_relaxed);
    SetStringTableText(def.menu_key, def.label);

    // One child state per item keeps the tree the shape the engine expects,
    // although the list draws the row objects, not these.
    for (int i = 0; i < def.item_count; ++i) {
      if (uint32_t state = CreateMenuState(def.items[i].key)) {
        AppendMenuItem(submenu, state);
      }
    }
    BuildRows(menu);
  }

  constexpr const char* kQuitKey = "PM_MxQuit";
  uint32_t quit = CreateMenuState(kQuitKey);
  uint32_t quit_key = AllocGuestString(kQuitKey);
  if (quit && quit_key) {
    AppendMenuItem(settings, quit);
    SetStringTableText(kQuitKey, "QUIT GAME");
    g_quit_button_key.store(quit_key, std::memory_order_relaxed);
  }
  g_menus_created.store(true, std::memory_order_relaxed);
  REXLOG_INFO("settings menu: {} submenus added to the pause menu", kMenuCount);
}

bool ButtonClicked(uint32_t key) {
  return key && CallGuest(kIsButtonClickedFn, key, 1u) != 0;
}

// True when `list` is reachable from `node` through the widget child arrays.
// Most widgets keep children at +68; the tab strip keeps its tabs at +180.
bool NodeReachesList(uint32_t node, uint32_t list, int depth, int& budget) {
  if (!node || depth > 8 || --budget < 0) {
    return false;
  }
  for (uint32_t offset : {68u, 180u}) {
    uint32_t array = Read32(node + offset);
    uint16_t count = Read16(node + offset + 4);
    if (array < 0xB0000000 || !count || count > 64) {
      continue;
    }
    for (uint16_t i = 0; i < count; ++i) {
      uint32_t child = Read32(array + uint32_t(4 * i));
      if (child == list) {
        return true;
      }
      if (child >= 0xB0000000 && NodeReachesList(child, list, depth + 1, budget)) {
        return true;
      }
    }
  }
  return false;
}

}  // namespace

// --- Hooks -------------------------------------------------------------------

void mcla_menu_capture_continue(PPCRegister& r3) {
  if (r3.u32) {
    if (uint32_t pause_tab = Read32(r3.u32 + 32)) {
      g_pause_tab.store(pause_tab, std::memory_order_relaxed);
    }
  }
}

bool mcla_menu_enable_save(PPCRegister& r3, PPCRegister& r4) {
  if (!REXCVAR_GET(mcla_settings_menu)) {
    return false;
  }
  const uint32_t save_button = r3.u32;
  if (save_button && !g_settings_state.load(std::memory_order_relaxed)) {
    if (uint32_t parent = Read32(save_button + 32)) {
      g_settings_state.store(parent, std::memory_order_relaxed);
    }
  }
  uint32_t pause_tab = g_pause_tab.load(std::memory_order_relaxed);
  if (save_button && pause_tab) {
    DetachMenuItem(save_button);
    AppendMenuItem(pause_tab, save_button);
    SetStringTableText("Save (Dev Only)", "SAVE GAME");
  }
  CreateSubmenus();
  r4.u64 = 1;
  return true;
}

bool mcla_menu_click(PPCRegister& r3, PPCRegister& r31) {
  (void)r31;
  if (!g_menus_created.load(std::memory_order_relaxed)) {
    return false;
  }
  const int active = g_active_menu.load(std::memory_order_relaxed);
  if (active >= 0) {
    // Accept on a row steps its value forward. Every press inside a submenu is
    // consumed so the game never resolves it against the Settings tab's items.
    uint32_t list = g_pause_list.load(std::memory_order_relaxed);
    const uint32_t index = list ? Read32(list + kListSelect) : 0xFFFFFFFFu;
    const MenuDef& def = kMenus[active];
    if (index < uint32_t(def.item_count)) {
      const ItemDef& item = def.items[index];
      const int count = ItemValueCount(item);
      ItemSetIndex(item, (ItemCurrentIndex(item) + 1) % count);
      SyncRowsFromOptions(active);
      ShowSubmenu();
    }
    r3.u64 = 1;
    return true;
  }

  if (ButtonClicked(g_quit_button_key.load(std::memory_order_relaxed))) {
    auto* runtime = rex::Runtime::instance();
    if (auto* context = runtime ? runtime->app_context() : nullptr) {
      REXLOG_INFO("settings menu: quit requested");
      SaveSettingsIfChanged();
      context->RequestDeferredQuit();
    }
    r3.u64 = 1;
    return true;
  }

  for (int menu = 0; menu < kMenuCount; ++menu) {
    if (!ButtonClicked(g_menu_button_key[menu].load(std::memory_order_relaxed))) {
      continue;
    }
    uint32_t submenu = g_menu_state[menu].load(std::memory_order_relaxed);
    uint32_t list = g_last_list.load(std::memory_order_relaxed);
    if (!submenu || !list || !BuildRows(menu)) {
      return false;
    }
    g_pause_list.store(list, std::memory_order_relaxed);
    g_saved_list = {Read32(list + kListRows),        Read32(list + kListCountCap),
                    Read32(list + kListTableSource), Read32(list + kListStateSource),
                    Read32(list + kListSelect),      Read32(list + kListScroll)};
    Write32(list + kListSelect, 0);
    Write32(list + kListScroll, 0);

    g_active_menu.store(menu, std::memory_order_relaxed);
    SyncRowsFromOptions(menu);
    MenuStackPush(submenu);
    ShowSubmenu();
    REXLOG_INFO("settings menu: opened '{}'", kMenus[menu].label);
    r3.u64 = 1;
    return true;
  }
  return false;
}

bool mcla_menu_cancel(PPCRegister& r31) {
  (void)r31;
  if (g_active_menu.exchange(-1, std::memory_order_relaxed) < 0) {
    return false;
  }
  // Pop first, then hand the list back, in that order.
  MenuStackPop();
  RestorePauseList();
  SaveSettingsIfChanged();
  REXLOG_INFO("settings menu: closed");
  return true;
}

bool mcla_menu_populate_redirect(PPCRegister& r3) {
  const int menu = g_active_menu.load(std::memory_order_relaxed);
  uint32_t settings = g_settings_state.load(std::memory_order_relaxed);
  if (menu >= 0 && settings && r3.u32 == settings) {
    if (uint32_t submenu = g_menu_state[menu].load(std::memory_order_relaxed)) {
      r3.u64 = submenu;
    }
  }
  return false;
}

bool mcla_menu_list_populate(PPCRegister& r3) {
  const uint32_t list = r3.u32;
  if (!list) {
    return false;
  }
  g_last_list.store(list, std::memory_order_relaxed);
  const int menu = g_active_menu.load(std::memory_order_relaxed);
  if (menu >= 0 && list == g_pause_list.load(std::memory_order_relaxed)) {
    PollRows(menu);
    InstallRows(menu, list);
  }
  return false;
}

bool mcla_menu_key_dispatch(PPCRegister& r31, PPCRegister& r28, PPCRegister& r27) {
  if (g_active_menu.load(std::memory_order_relaxed) < 0) {
    return false;
  }
  const uint32_t key = r28.u32 & 0xFF;
  if (key < '3' || key > '6') {  // left is '3' or '5', right is '4' or '6'
    return false;
  }
  uint32_t list = g_pause_list.load(std::memory_order_relaxed);
  if (!list) {
    return false;
  }
  int budget = 512;
  if (!NodeReachesList(r31.u32, list, 0, budget)) {
    return false;
  }
  uint32_t vtable = Read32(list);
  uint32_t function = vtable ? Read32(vtable + 32) : 0;
  if (!function) {
    return false;
  }
  CallGuest(function, list, key, r27.u32);
  return true;
}

bool mcla_menu_null_table_guard(PPCRegister& r30) {
  return r30.u32 == 0;
}

bool mcla_menu_list_vtable_guard(PPCRegister& r31) {
  const uint32_t list = r31.u32;
  return list && Read32(list) == 0;
}

bool mcla_motion_blur_gate(PPCRegister& r3) {
  if (REXCVAR_GET(mcla_motion_blur)) {
    return false;
  }
  r3.u64 = 0;
  return true;
}

void mcla_dof_composite(PPCRegister& r3) {
  if (REXCVAR_GET(mcla_depth_of_field) || !r3.u32) {
    return;
  }
  std::memset(Membase() + r3.u32 + 0xF0, 0, 16);
}
