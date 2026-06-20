#include "telnet_shell.h"

#include "SD_FTP_Server/src/SD_FTP_Server.h"
#include "appconfig.h"
#include "disk.h"
#include "emu_control.h"
#include "fifo.h"
#include "platform.h"
#include "rh11.h"
#include "rk11.h"
#include "rl11.h"
#include "telnet.h"

#include <Arduino.h>
#include <SD_MMC.h>
#include "esp_attr.h"
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

static constexpr size_t SHELL_LINE_MAX = 255;
static constexpr size_t SHELL_QUEUE_DEPTH = 4;
static constexpr size_t SHELL_OUTPUT_BYTES = 8192;
static constexpr size_t SHELL_PATH_MAX = 128;

static volatile bool g_active = false;
static char g_input_line[SHELL_LINE_MAX + 1];
static size_t g_input_len = 0;
static char g_commands[SHELL_QUEUE_DEPTH][SHELL_LINE_MAX + 1];
static volatile uint8_t g_command_head = 0;
static volatile uint8_t g_command_tail = 0;
EXT_RAM_BSS_ATTR static uint8_t g_output_storage[SHELL_OUTPUT_BYTES];
EXT_RAM_BSS_ATTR static uint8_t g_file_buffer[4096];
static Fifo g_output;
static bool g_initialized = false;
static char g_cwd[SHELL_PATH_MAX] = "/";

static void output_char(uint8_t value) {
  g_output.push(value);
  if (value == 255) g_output.push(value);
}

static bool output_char_wait(uint8_t value) {
  uint32_t started = millis();
  while (!g_output.push(value)) {
    if (!g_active || millis() - started >= 2000) return false;
    delay(1);
  }
  if (value == 255) {
    started = millis();
    while (!g_output.push(value)) {
      if (!g_active || millis() - started >= 2000) return false;
      delay(1);
    }
  }
  return true;
}

static void output_text(const char* text) {
  if (!text) return;
  while (*text) output_char((uint8_t)*text++);
}

static void output_printf(const char* format, ...) {
  char buffer[384];
  va_list args;
  va_start(args, format);
  vsnprintf(buffer, sizeof(buffer), format, args);
  va_end(args);
  output_text(buffer);
}

static void prompt() {
  output_printf("vpdp:%s> ", g_cwd);
}

static bool queue_command(const char* command) {
  uint8_t next = (uint8_t)((g_command_head + 1) % SHELL_QUEUE_DEPTH);
  if (next == g_command_tail) return false;
  strncpy(g_commands[g_command_head], command, SHELL_LINE_MAX);
  g_commands[g_command_head][SHELL_LINE_MAX] = 0;
  g_command_head = next;
  return true;
}

static bool pop_command(char* command, size_t size) {
  if (g_command_head == g_command_tail) return false;
  strncpy(command, g_commands[g_command_tail], size - 1);
  command[size - 1] = 0;
  g_command_tail = (uint8_t)((g_command_tail + 1) % SHELL_QUEUE_DEPTH);
  return true;
}

void telnet_shell_init() {
  if (g_initialized) return;
  g_output.init(g_output_storage, SHELL_OUTPUT_BYTES);
  g_initialized = true;
}

void telnet_shell_enter() {
  telnet_shell_init();
  g_input_len = 0;
  g_input_line[0] = 0;
  g_command_tail = g_command_head;
  g_output.clear();
  strcpy(g_cwd, "/");
  g_active = true;
  LOG("telnet shell: entered by %s", telnet_client_ip());
}

void telnet_shell_disconnect() {
  g_active = false;
  g_input_len = 0;
  g_command_tail = g_command_head;
  if (g_initialized) g_output.clear();
}

bool telnet_shell_active() {
  return g_active;
}

bool telnet_shell_backspace() {
  if (!g_active || g_input_len == 0) return false;
  g_input_line[--g_input_len] = 0;
  return true;
}

bool telnet_shell_input(uint8_t c) {
  if (!g_active) return false;
  if (c == '\r' || c == '\n') {
    g_input_line[g_input_len] = 0;
    if (!queue_command(g_input_line))
      LOGE("telnet shell: command queue full");
    g_input_len = 0;
    g_input_line[0] = 0;
    return false;
  }
  if (c < 0x20 || c > 0x7e || g_input_len >= SHELL_LINE_MAX) return false;
  g_input_line[g_input_len++] = (char)c;
  g_input_line[g_input_len] = 0;
  return true;
}

size_t telnet_shell_output_peek(const uint8_t** data) {
  if (!g_initialized) {
    *data = nullptr;
    return 0;
  }
  return g_output.peek(data);
}

void telnet_shell_output_consume(size_t bytes) {
  g_output.consume(bytes);
}

static bool mounted_path(const char* path) {
  for (int slot = 0; slot < DRIVE_COUNT; slot++)
    if (disk_is_mounted(slot) && !strcasecmp(path, disk_path(slot))) return true;
  return false;
}

static bool normalize_path(const char* input, char* output, size_t size) {
  if (!input || !*input || !output || size < 2) return false;
  char combined[256];
  int written;
  if (input[0] == '/')
    written = snprintf(combined, sizeof(combined), "%s", input);
  else if (!strcmp(g_cwd, "/"))
    written = snprintf(combined, sizeof(combined), "/%s", input);
  else
    written = snprintf(combined, sizeof(combined), "%s/%s", g_cwd, input);
  if (written < 0 || (size_t)written >= sizeof(combined)) return false;

  char working[256];
  strncpy(working, combined, sizeof(working) - 1);
  working[sizeof(working) - 1] = 0;
  const char* parts[32];
  size_t count = 0;
  char* save = nullptr;
  for (char* part = strtok_r(working, "/", &save);
       part;
       part = strtok_r(nullptr, "/", &save)) {
    if (!strcmp(part, ".") || !*part) continue;
    if (!strcmp(part, "..")) {
      if (count) count--;
      continue;
    }
    if (strchr(part, '\\') || strchr(part, ':') || strchr(part, ';'))
      return false;
    if (count >= sizeof(parts) / sizeof(parts[0])) return false;
    parts[count++] = part;
  }

  size_t used = 0;
  output[used++] = '/';
  for (size_t i = 0; i < count; i++) {
    size_t length = strlen(parts[i]);
    if (used + length + (i + 1 < count ? 1 : 0) >= size) return false;
    memcpy(output + used, parts[i], length);
    used += length;
    if (i + 1 < count) output[used++] = '/';
  }
  output[used] = 0;
  return true;
}

static const char* basename_of(const char* path) {
  const char* slash = strrchr(path, '/');
  return slash ? slash + 1 : path;
}

static int split_words(char* line, char* words[], int maximum) {
  int count = 0;
  char* cursor = line;
  while (*cursor && count < maximum) {
    while (*cursor == ' ' || *cursor == '\t') cursor++;
    if (!*cursor) break;
    char quote = 0;
    if (*cursor == '"' || *cursor == '\'') quote = *cursor++;
    words[count++] = cursor;
    if (quote) {
      while (*cursor && *cursor != quote) cursor++;
    } else {
      while (*cursor && *cursor != ' ' && *cursor != '\t') cursor++;
    }
    if (*cursor) *cursor++ = 0;
  }
  return count;
}

static void command_help() {
  output_text(
      "File commands:\r\n"
      "  pwd                         show current SD directory\r\n"
      "  cd <path>                   change current directory\r\n"
      "  ls [path]                   list a file or directory\r\n"
      "  cat <path>                  display the first 100 lines\r\n"
      "  rm <path>                   remove a file\r\n"
      "  mv <source> <destination>   rename or move a file\r\n"
      "  cp <source> <destination>   copy a file\r\n"
      "Emulator commands:\r\n"
      "  drives                      show mounted disk images\r\n"
      "  mount <unit> <path> [ro]    mount RL0-RL3, RK0, or RP0\r\n"
      "  dismount <unit>             dismount a drive\r\n"
      "  create <rk|rl01|rl02> <path> create an empty disk image\r\n"
      "  reboot                      cold reboot the PDP-11\r\n"
      "  exit                        reconnect Telnet to the PDP console\r\n");
}

static void command_ls(const char* argument) {
  char path[SHELL_PATH_MAX];
  if (!normalize_path(argument && *argument ? argument : g_cwd,
                      path, sizeof(path))) {
    output_text("error: invalid path\r\n");
    return;
  }
  SD_FTP_StorageGuard guard;
  File entry = SD_MMC.open(path, "r");
  if (!entry) {
    output_printf("error: cannot open %s\r\n", path);
    return;
  }
  if (!entry.isDirectory()) {
    output_printf("%10lu  %s\r\n", (unsigned long)entry.size(),
                  basename_of(path));
    entry.close();
    return;
  }
  File child;
  while ((child = entry.openNextFile())) {
    const char* name = basename_of(child.name());
    if (child.isDirectory())
      output_printf("     <DIR>  %s/\r\n", name);
    else
      output_printf("%10lu  %s\r\n", (unsigned long)child.size(), name);
    child.close();
  }
  entry.close();
}

static void command_cat(const char* argument) {
  if (!argument) {
    output_text("usage: cat <path>\r\n");
    return;
  }
  char path[SHELL_PATH_MAX];
  if (!normalize_path(argument, path, sizeof(path))) {
    output_text("error: invalid path\r\n");
    return;
  }
  SD_FTP_StorageGuard guard;
  File file = SD_MMC.open(path, "r");
  if (!file || file.isDirectory()) {
    output_printf("error: cannot read file: %s\r\n", path);
    if (file) file.close();
    return;
  }

  unsigned scan_lines = 0;
  bool scan_previous_cr = false;
  bool binary = false;
  while (file.available() && scan_lines < 100) {
    int value = file.read();
    if (value < 0) break;
    uint8_t ch = (uint8_t)value;
    if (ch == '\r') {
      scan_lines++;
      scan_previous_cr = true;
    } else if (ch == '\n') {
      if (!scan_previous_cr) scan_lines++;
      scan_previous_cr = false;
    } else {
      scan_previous_cr = false;
      if (ch != '\t' && (ch < 0x20 || ch > 0x7e)) {
        binary = true;
        break;
      }
    }
  }
  if (binary) {
    file.close();
    output_text("error: file is binary\r\n");
    return;
  }
  if (!file.seek(0)) {
    file.close();
    output_printf("error: cannot rewind file: %s\r\n", path);
    return;
  }

  unsigned lines = 0;
  bool previous_cr = false;
  bool output_ok = true;
  while (file.available() && lines < 100 && output_ok) {
    int value = file.read();
    if (value < 0) break;
    uint8_t ch = (uint8_t)value;
    if (ch == '\r') {
      output_ok = output_char_wait('\r') && output_char_wait('\n');
      lines++;
      previous_cr = true;
    } else if (ch == '\n') {
      if (!previous_cr) {
        output_ok = output_char_wait('\r') && output_char_wait('\n');
        lines++;
      }
      previous_cr = false;
    } else {
      previous_cr = false;
      output_ok = output_char_wait(ch);
    }
  }
  file.close();
  if (output_ok && lines >= 100)
    output_text("[output limited to 100 lines]\r\n");
}

static void command_cd(const char* argument) {
  if (!argument) {
    output_text("usage: cd <path>\r\n");
    return;
  }
  char path[SHELL_PATH_MAX];
  if (!normalize_path(argument, path, sizeof(path))) {
    output_text("error: invalid path\r\n");
    return;
  }
  SD_FTP_StorageGuard guard;
  File directory = SD_MMC.open(path, "r");
  if (!directory || !directory.isDirectory()) {
    output_printf("error: not a directory: %s\r\n", path);
    if (directory) directory.close();
    return;
  }
  directory.close();
  strcpy(g_cwd, path);
}

static void command_rm(const char* argument) {
  if (!argument) {
    output_text("usage: rm <path>\r\n");
    return;
  }
  char path[SHELL_PATH_MAX];
  if (!normalize_path(argument, path, sizeof(path))) {
    output_text("error: invalid path\r\n");
    return;
  }
  if (mounted_path(path)) {
    output_text("error: file is mounted by the emulator\r\n");
    return;
  }
  SD_FTP_StorageGuard guard;
  File file = SD_MMC.open(path, "r");
  if (!file) {
    output_printf("error: file not found: %s\r\n", path);
    return;
  }
  bool directory = file.isDirectory();
  file.close();
  if (directory) {
    output_text("error: rm removes files only\r\n");
    return;
  }
  output_printf(SD_MMC.remove(path) ? "removed %s\r\n"
                                    : "error: remove failed: %s\r\n", path);
}

static void command_mv(const char* source_arg, const char* destination_arg) {
  char source[SHELL_PATH_MAX], destination[SHELL_PATH_MAX];
  if (!source_arg || !destination_arg ||
      !normalize_path(source_arg, source, sizeof(source)) ||
      !normalize_path(destination_arg, destination, sizeof(destination))) {
    output_text("usage: mv <source> <destination>\r\n");
    return;
  }
  if (mounted_path(source) || mounted_path(destination)) {
    output_text("error: source or destination is mounted\r\n");
    return;
  }
  SD_FTP_StorageGuard guard;
  if (!SD_MMC.exists(source)) {
    output_printf("error: file not found: %s\r\n", source);
    return;
  }
  if (SD_MMC.exists(destination)) {
    output_printf("error: destination exists: %s\r\n", destination);
    return;
  }
  output_printf(SD_MMC.rename(source, destination) ? "moved %s -> %s\r\n"
                                                   : "error: move failed\r\n",
                source, destination);
}

static void command_cp(const char* source_arg, const char* destination_arg) {
  char source[SHELL_PATH_MAX], destination[SHELL_PATH_MAX];
  if (!source_arg || !destination_arg ||
      !normalize_path(source_arg, source, sizeof(source)) ||
      !normalize_path(destination_arg, destination, sizeof(destination))) {
    output_text("usage: cp <source> <destination>\r\n");
    return;
  }
  if (mounted_path(source) || mounted_path(destination)) {
    output_text("error: source or destination is mounted\r\n");
    return;
  }
  SD_FTP_StorageGuard guard;
  if (SD_MMC.exists(destination)) {
    output_printf("error: destination exists: %s\r\n", destination);
    return;
  }
  File source_file = SD_MMC.open(source, "r");
  if (!source_file || source_file.isDirectory()) {
    output_printf("error: cannot read file: %s\r\n", source);
    if (source_file) source_file.close();
    return;
  }
  File destination_file = SD_MMC.open(destination, "w");
  if (!destination_file) {
    source_file.close();
    output_printf("error: cannot create: %s\r\n", destination);
    return;
  }
  bool ok = true;
  while (source_file.available()) {
    size_t count = source_file.read(g_file_buffer, sizeof(g_file_buffer));
    if (!count) break;
    if (destination_file.write(g_file_buffer, count) != count) {
      ok = false;
      break;
    }
  }
  destination_file.flush();
  destination_file.close();
  source_file.close();
  if (!ok) {
    SD_MMC.remove(destination);
    output_text("error: copy failed; partial destination removed\r\n");
  } else {
    output_printf("copied %s -> %s\r\n", source, destination);
  }
}

static const char* slot_label(int slot) {
  if (slot == DRIVE_RP0) return "RP0";
  if (slot == DRIVE_A)
    return cfg.boot_kind == AppConfig::BK_RK ? "RK0" : "RL0";
  if (slot == DRIVE_B) return "RL1";
  if (slot == DRIVE_C) return "RL2";
  return "RL3";
}

static void command_drives() {
  for (int slot = 0; slot < DRIVE_COUNT; slot++) {
    if (!disk_is_mounted(slot)) {
      output_printf("%-3s  empty\r\n", slot_label(slot));
      continue;
    }
    output_printf("%-3s  %s  %lu bytes  %s\r\n",
                  slot_label(slot), disk_path(slot),
                  (unsigned long)disk_size_bytes(slot),
                  disk_is_readonly(slot) ? "read-only" : "read-write");
  }
}

static int unit_slot(const char* unit) {
  if (!unit) return -1;
  if (!strcasecmp(unit, "RP0")) return DRIVE_RP0;
  if (!strcasecmp(unit, "RK0"))
    return cfg.boot_kind == AppConfig::BK_RK ? DRIVE_A : -1;
  if (!strcasecmp(unit, "RL0") || !strcasecmp(unit, "DL0"))
    return cfg.boot_kind == AppConfig::BK_RL ? DRIVE_A : -1;
  if (cfg.boot_kind != AppConfig::BK_RL) return -1;
  if (!strcasecmp(unit, "RL1") || !strcasecmp(unit, "DL1")) return DRIVE_B;
  if (!strcasecmp(unit, "RL2") || !strcasecmp(unit, "DL2")) return DRIVE_C;
  if (!strcasecmp(unit, "RL3") || !strcasecmp(unit, "DL3")) return DRIVE_D;
  return -1;
}

static void notify_media(int slot, bool mounted) {
  if (slot == DRIVE_RP0)
    rh11::media_changed(mounted);
  else if (cfg.boot_kind == AppConfig::BK_RK)
    rk11::media_changed(slot, mounted);
  else
    rl11::media_changed(slot, mounted);
}

static void command_mount(const char* unit, const char* path_arg,
                          const char* mode) {
  int slot = unit_slot(unit);
  if (slot < 0 || !path_arg) {
    output_text("usage: mount <RL0-RL3|RK0|RP0> <path> [ro]\r\n");
    return;
  }
  char path[SHELL_PATH_MAX];
  if (!normalize_path(path_arg, path, sizeof(path))) {
    output_text("error: invalid path\r\n");
    return;
  }
  if (disk_is_mounted(slot)) {
    output_printf("error: %s is mounted; dismount it first\r\n",
                  slot_label(slot));
    return;
  }
  bool readonly = mode && (!strcasecmp(mode, "ro") ||
                           !strcasecmp(mode, "readonly"));
  if (!disk_mount_mode(slot, path, readonly)) {
    output_printf("error: mount failed: %s: %s\r\n", path,
                  disk_last_error()[0] ? disk_last_error() : "unknown error");
    return;
  }
  notify_media(slot, true);
  output_printf("mounted %s on %s (%s)\r\n", path, slot_label(slot),
                disk_is_readonly(slot) ? "read-only" : "read-write");
}

static void command_dismount(const char* unit) {
  int slot = unit_slot(unit);
  if (slot < 0) {
    output_text("usage: dismount <RL0-RL3|RK0|RP0>\r\n");
    return;
  }
  if (!disk_is_mounted(slot)) {
    output_printf("%s is already empty\r\n", slot_label(slot));
    return;
  }
  disk_dismount(slot);
  notify_media(slot, false);
  output_printf("dismounted %s\r\n", slot_label(slot));
}

static void command_create(const char* type, const char* path_arg) {
  uint32_t bytes = 0;
  if (type && !strcasecmp(type, "rk")) bytes = 2494464u;
  else if (type && !strcasecmp(type, "rl01")) bytes = 5242880u;
  else if (type && !strcasecmp(type, "rl02")) bytes = 10485760u;
  if (!bytes || !path_arg) {
    output_text("usage: create <rk|rl01|rl02> <path>\r\n");
    return;
  }
  char path[SHELL_PATH_MAX];
  if (!normalize_path(path_arg, path, sizeof(path))) {
    output_text("error: invalid path\r\n");
    return;
  }
  if (mounted_path(path)) {
    output_text("error: path is mounted\r\n");
    return;
  }
  output_printf("creating %s (%lu bytes)...\r\n", path, (unsigned long)bytes);
  SD_FTP_StorageGuard guard;
  if (SD_MMC.exists(path)) {
    output_printf("error: file already exists: %s\r\n", path);
    return;
  }
  File file = SD_MMC.open(path, "w");
  if (!file) {
    output_printf("error: cannot create: %s\r\n", path);
    return;
  }
  memset(g_file_buffer, 0, sizeof(g_file_buffer));
  uint32_t remaining = bytes;
  bool ok = true;
  while (remaining) {
    size_t count = remaining > sizeof(g_file_buffer)
                     ? sizeof(g_file_buffer) : remaining;
    if (file.write(g_file_buffer, count) != count) {
      ok = false;
      break;
    }
    remaining -= count;
    if ((remaining & 0x3ffffu) == 0) delay(1);
  }
  file.flush();
  file.close();
  if (!ok) {
    SD_MMC.remove(path);
    output_text("error: create failed; partial file removed\r\n");
  } else {
    output_printf("created %s\r\n", path);
  }
}

static void execute_command(char* line) {
  char* words[8];
  int count = split_words(line, words, 8);
  if (count == 0) {
    prompt();
    return;
  }
  if (!strcasecmp(words[0], "help") || !strcmp(words[0], "?"))
    command_help();
  else if (!strcasecmp(words[0], "pwd"))
    output_printf("%s\r\n", g_cwd);
  else if (!strcasecmp(words[0], "cd"))
    command_cd(count > 1 ? words[1] : nullptr);
  else if (!strcasecmp(words[0], "ls"))
    command_ls(count > 1 ? words[1] : nullptr);
  else if (!strcasecmp(words[0], "cat"))
    command_cat(count > 1 ? words[1] : nullptr);
  else if (!strcasecmp(words[0], "rm"))
    command_rm(count > 1 ? words[1] : nullptr);
  else if (!strcasecmp(words[0], "mv"))
    command_mv(count > 1 ? words[1] : nullptr,
               count > 2 ? words[2] : nullptr);
  else if (!strcasecmp(words[0], "cp"))
    command_cp(count > 1 ? words[1] : nullptr,
               count > 2 ? words[2] : nullptr);
  else if (!strcasecmp(words[0], "drives"))
    command_drives();
  else if (!strcasecmp(words[0], "mount"))
    command_mount(count > 1 ? words[1] : nullptr,
                  count > 2 ? words[2] : nullptr,
                  count > 3 ? words[3] : nullptr);
  else if (!strcasecmp(words[0], "dismount") ||
           !strcasecmp(words[0], "unmount"))
    command_dismount(count > 1 ? words[1] : nullptr);
  else if (!strcasecmp(words[0], "create"))
    command_create(count > 1 ? words[1] : nullptr,
                   count > 2 ? words[2] : nullptr);
  else if (!strcasecmp(words[0], "reboot")) {
    if (emu_control::submit("PDP;REBOOT"))
      output_text("PDP-11 cold reboot scheduled\r\n");
    else
      output_text("error: emulator command queue full\r\n");
  } else if (!strcasecmp(words[0], "exit")) {
    output_text("Returning Telnet to the PDP-11 console.\r\n");
    g_active = false;
    LOG("telnet shell: returned to PDP console");
    return;
  } else {
    output_printf("unknown command: %s (type help)\r\n", words[0]);
  }
  prompt();
}

void telnet_shell_poll() {
  if (!g_initialized) return;
  char command[SHELL_LINE_MAX + 1];
  while (g_active && pop_command(command, sizeof(command)))
    execute_command(command);
}
