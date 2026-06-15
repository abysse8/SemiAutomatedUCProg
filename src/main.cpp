#include <Arduino.h>
#include <Preferences.h>
#include <SdFat.h>
#include <USB.h>
#include <USBMSC.h>

#ifndef UC_SD_CS
#define UC_SD_CS 10
#endif

#ifndef UC_MODE_BUTTON
#define UC_MODE_BUTTON 0
#endif

#ifndef UC_STATUS_LED
#define UC_STATUS_LED -1
#endif

namespace {

constexpr uint16_t kSectorSize = 512;
constexpr uint32_t kPartitionStart = 2048;
constexpr uint32_t kReservedSectors = 32;
constexpr uint32_t kFatCount = 2;
constexpr uint32_t kSectorsPerCluster = 64;
constexpr uint32_t kClusterSize = kSectorSize * kSectorsPerCluster;
constexpr uint32_t kRootCluster = 2;
constexpr size_t kMaxFiles = 64;
constexpr size_t kMaxName = 96;

enum class Mode : uint8_t {
  Off = 0,
  M0 = 1,
  M1 = 2,
};

struct FileEntry {
  char name[kMaxName];
  char path[kMaxName + 8];
  uint64_t size = 0;
  uint32_t firstCluster = 0;
  uint32_t clusterCount = 0;
  char shortName[11];
  uint8_t shortChecksum = 0;
};

SdFat sd;
USBMSC msc;
Preferences prefs;

Mode currentMode = Mode::Off;
FileEntry files[kMaxFiles];
size_t fileCount = 0;

uint32_t dataClusterCount = 1;
uint32_t fatSectors = 1;
uint32_t volumeSectors = 0;
uint32_t blockCount = 0;
bool sdReady = false;
bool mscStarted = false;

const char *modeName(Mode mode) {
  switch (mode) {
    case Mode::M0: return "M0";
    case Mode::M1: return "M1";
    default: return "OFF";
  }
}

const char *modeFolder(Mode mode) {
  switch (mode) {
    case Mode::M0: return "/M0";
    case Mode::M1: return "/M1";
    default: return "";
  }
}

Mode parseMode(const String &text) {
  String normalized = text;
  normalized.trim();
  normalized.toUpperCase();
  if (normalized == "M0") return Mode::M0;
  if (normalized == "M1") return Mode::M1;
  return Mode::Off;
}

Mode nextMode(Mode mode) {
  if (mode == Mode::Off) return Mode::M0;
  if (mode == Mode::M0) return Mode::M1;
  return Mode::Off;
}

void saveMode(Mode mode) {
  prefs.begin("ucprog", false);
  prefs.putUChar("mode", static_cast<uint8_t>(mode));
  prefs.end();
}

Mode loadMode() {
  prefs.begin("ucprog", true);
  const uint8_t raw = prefs.getUChar("mode", static_cast<uint8_t>(Mode::Off));
  prefs.end();
  if (raw == static_cast<uint8_t>(Mode::M0)) return Mode::M0;
  if (raw == static_cast<uint8_t>(Mode::M1)) return Mode::M1;
  return Mode::Off;
}

void setLed(bool on) {
#if UC_STATUS_LED >= 0
  digitalWrite(UC_STATUS_LED, on ? HIGH : LOW);
#else
  (void)on;
#endif
}

void blink(uint8_t count) {
  for (uint8_t i = 0; i < count; ++i) {
    setLed(true);
    delay(120);
    setLed(false);
    delay(120);
  }
}

uint8_t lfnChecksum(const char shortName[11]) {
  uint8_t sum = 0;
  for (uint8_t i = 0; i < 11; ++i) {
    sum = ((sum & 1) ? 0x80 : 0) + (sum >> 1) + static_cast<uint8_t>(shortName[i]);
  }
  return sum;
}

void makeShortName(size_t index, const char *name, char out[11]) {
  memset(out, ' ', 11);
  snprintf(out, 9, "UC%06u", static_cast<unsigned>(index + 1));
  for (uint8_t i = 0; i < 8 && out[i] != '\0'; ++i) {
    out[i] = toupper(out[i]);
  }

  const char *dot = strrchr(name, '.');
  const char *ext = dot ? dot + 1 : "";
  if (strcasecmp(ext, "gz") == 0) {
    memcpy(out + 8, "TGZ", 3);
  } else if (strcasecmp(ext, "json") == 0) {
    memcpy(out + 8, "JSN", 3);
  } else {
    for (uint8_t i = 0; i < 3 && ext[i] != '\0'; ++i) {
      out[8 + i] = toupper(ext[i]);
    }
  }
}

uint32_t ceilDiv(uint64_t value, uint32_t divisor) {
  return static_cast<uint32_t>((value + divisor - 1) / divisor);
}

bool scanSelectedFolder(Mode mode) {
  fileCount = 0;
  dataClusterCount = 1;

  const char *folder = modeFolder(mode);
  FsFile dir;
  if (!dir.open(folder, O_RDONLY)) {
    Serial.printf("Cannot open selected folder: %s\r\n", folder);
    return false;
  }

  FsFile file;
  while (file.openNext(&dir, O_RDONLY) && fileCount < kMaxFiles) {
    if (!file.isDir() && !file.isHidden()) {
      FileEntry &entry = files[fileCount];
      memset(&entry, 0, sizeof(entry));
      file.getName(entry.name, sizeof(entry.name));
      snprintf(entry.path, sizeof(entry.path), "%s/%s", folder, entry.name);
      entry.size = file.fileSize();
      entry.clusterCount = max<uint32_t>(1, ceilDiv(entry.size, kClusterSize));
      entry.firstCluster = kRootCluster + dataClusterCount;
      makeShortName(fileCount, entry.name, entry.shortName);
      entry.shortChecksum = lfnChecksum(entry.shortName);
      dataClusterCount += entry.clusterCount;
      ++fileCount;
    }
    file.close();
  }
  dir.close();

  const uint32_t fatEntries = kRootCluster + dataClusterCount;
  fatSectors = ceilDiv(static_cast<uint64_t>(fatEntries) * 4, kSectorSize);
  volumeSectors = kReservedSectors + (kFatCount * fatSectors) + (dataClusterCount * kSectorsPerCluster);
  blockCount = kPartitionStart + volumeSectors;

  Serial.printf("Mode %s exposes %u files, %lu sectors\r\n",
                modeName(mode),
                static_cast<unsigned>(fileCount),
                static_cast<unsigned long>(blockCount));
  return fileCount > 0;
}

void writeLe16(uint8_t *p, uint16_t v) {
  p[0] = v & 0xff;
  p[1] = v >> 8;
}

void writeLe32(uint8_t *p, uint32_t v) {
  p[0] = v & 0xff;
  p[1] = (v >> 8) & 0xff;
  p[2] = (v >> 16) & 0xff;
  p[3] = (v >> 24) & 0xff;
}

void fillMbr(uint8_t *sector) {
  memset(sector, 0, kSectorSize);
  sector[446] = 0x00;
  sector[450] = 0x0c;
  writeLe32(sector + 454, kPartitionStart);
  writeLe32(sector + 458, volumeSectors);
  sector[510] = 0x55;
  sector[511] = 0xaa;
}

void fillBootSector(uint8_t *sector) {
  memset(sector, 0, kSectorSize);
  sector[0] = 0xeb;
  sector[1] = 0x58;
  sector[2] = 0x90;
  memcpy(sector + 3, "MSDOS5.0", 8);
  writeLe16(sector + 11, kSectorSize);
  sector[13] = kSectorsPerCluster;
  writeLe16(sector + 14, kReservedSectors);
  sector[16] = kFatCount;
  writeLe16(sector + 17, 0);
  writeLe16(sector + 19, 0);
  sector[21] = 0xf8;
  writeLe16(sector + 22, 0);
  writeLe16(sector + 24, 63);
  writeLe16(sector + 26, 255);
  writeLe32(sector + 28, kPartitionStart);
  writeLe32(sector + 32, volumeSectors);
  writeLe32(sector + 36, fatSectors);
  writeLe16(sector + 40, 0);
  writeLe16(sector + 42, 0);
  writeLe32(sector + 44, kRootCluster);
  writeLe16(sector + 48, 1);
  writeLe16(sector + 50, 6);
  sector[64] = 0x80;
  sector[66] = 0x29;
  writeLe32(sector + 67, 0x20260615);
  memcpy(sector + 71, "UCUPDATE   ", 11);
  memcpy(sector + 82, "FAT32   ", 8);
  sector[510] = 0x55;
  sector[511] = 0xaa;
}

void fillFsInfo(uint8_t *sector) {
  memset(sector, 0, kSectorSize);
  writeLe32(sector + 0, 0x41615252);
  writeLe32(sector + 484, 0x61417272);
  writeLe32(sector + 488, 0xffffffff);
  writeLe32(sector + 492, 0xffffffff);
  writeLe32(sector + 508, 0xaa550000);
}

uint32_t fatEntryForCluster(uint32_t cluster) {
  if (cluster == 0) return 0x0ffffff8;
  if (cluster == 1) return 0xffffffff;
  if (cluster == kRootCluster) return 0x0fffffff;

  for (size_t i = 0; i < fileCount; ++i) {
    const FileEntry &entry = files[i];
    if (cluster >= entry.firstCluster && cluster < entry.firstCluster + entry.clusterCount) {
      const uint32_t offset = cluster - entry.firstCluster;
      if (offset + 1 >= entry.clusterCount) return 0x0fffffff;
      return cluster + 1;
    }
  }
  return 0;
}

void fillFatSector(uint8_t *sector, uint32_t fatRelativeSector) {
  memset(sector, 0, kSectorSize);
  const uint32_t firstEntry = (fatRelativeSector * kSectorSize) / 4;
  for (uint32_t i = 0; i < kSectorSize / 4; ++i) {
    writeLe32(sector + (i * 4), fatEntryForCluster(firstEntry + i));
  }
}

void writeUtf16Slot(uint8_t *entry, uint8_t offset, uint16_t value) {
  writeLe16(entry + offset, value);
}

void fillLfnEntry(uint8_t *entry, const char *name, uint8_t lfnIndex, uint8_t lfnCount, uint8_t checksum) {
  memset(entry, 0xff, 32);
  entry[0] = lfnIndex;
  if (lfnIndex == lfnCount) entry[0] |= 0x40;
  entry[11] = 0x0f;
  entry[12] = 0x00;
  entry[13] = checksum;
  writeLe16(entry + 26, 0);

  const int start = (lfnIndex - 1) * 13;
  const uint8_t slots[13] = {1, 3, 5, 7, 9, 14, 16, 18, 20, 22, 24, 28, 30};
  const size_t len = strlen(name);
  for (uint8_t i = 0; i < 13; ++i) {
    const int nameIndex = start + i;
    if (nameIndex < static_cast<int>(len)) {
      writeUtf16Slot(entry, slots[i], static_cast<uint8_t>(name[nameIndex]));
    } else if (nameIndex == static_cast<int>(len)) {
      writeUtf16Slot(entry, slots[i], 0x0000);
    } else {
      writeUtf16Slot(entry, slots[i], 0xffff);
    }
  }
}

void fillShortDirEntry(uint8_t *entry, const FileEntry &file) {
  memset(entry, 0, 32);
  memcpy(entry, file.shortName, 11);
  entry[11] = 0x20;
  writeLe16(entry + 20, (file.firstCluster >> 16) & 0xffff);
  writeLe16(entry + 26, file.firstCluster & 0xffff);
  writeLe32(entry + 28, static_cast<uint32_t>(file.size));
}

void fillRootDirectoryCluster(uint8_t *buffer, uint32_t sectorInCluster) {
  memset(buffer, 0, kSectorSize);
  const uint32_t firstEntry = (sectorInCluster * kSectorSize) / 32;
  uint32_t virtualEntry = 0;

  for (size_t i = 0; i < fileCount; ++i) {
    const FileEntry &file = files[i];
    const uint8_t lfnCount = ceilDiv(strlen(file.name), 13);
    for (int lfn = lfnCount; lfn >= 1; --lfn) {
      if (virtualEntry >= firstEntry && virtualEntry < firstEntry + 16) {
        fillLfnEntry(buffer + ((virtualEntry - firstEntry) * 32), file.name, lfn, lfnCount, file.shortChecksum);
      }
      ++virtualEntry;
    }
    if (virtualEntry >= firstEntry && virtualEntry < firstEntry + 16) {
      fillShortDirEntry(buffer + ((virtualEntry - firstEntry) * 32), file);
    }
    ++virtualEntry;
  }
}

bool readFileCluster(uint8_t *buffer, uint32_t cluster, uint32_t sectorInCluster) {
  memset(buffer, 0, kSectorSize);
  for (size_t i = 0; i < fileCount; ++i) {
    const FileEntry &entry = files[i];
    if (cluster >= entry.firstCluster && cluster < entry.firstCluster + entry.clusterCount) {
      const uint64_t offset = (static_cast<uint64_t>(cluster - entry.firstCluster) * kClusterSize) +
                              (static_cast<uint64_t>(sectorInCluster) * kSectorSize);
      if (offset >= entry.size) return true;

      FsFile f;
      if (!f.open(entry.path, O_RDONLY)) return false;
      f.seekSet(offset);
      const size_t toRead = min<uint64_t>(kSectorSize, entry.size - offset);
      const int bytes = f.read(buffer, toRead);
      f.close();
      return bytes >= 0;
    }
  }
  return true;
}

bool readVirtualSector(uint32_t lba, uint8_t *buffer) {
  if (lba == 0) {
    fillMbr(buffer);
    return true;
  }
  if (lba < kPartitionStart || lba >= blockCount) {
    memset(buffer, 0, kSectorSize);
    return true;
  }

  const uint32_t rel = lba - kPartitionStart;
  if (rel == 0 || rel == 6) {
    fillBootSector(buffer);
    return true;
  }
  if (rel == 1 || rel == 7) {
    fillFsInfo(buffer);
    return true;
  }
  if (rel < kReservedSectors) {
    memset(buffer, 0, kSectorSize);
    return true;
  }

  const uint32_t fat0 = kReservedSectors;
  const uint32_t fat1 = fat0 + fatSectors;
  const uint32_t dataStart = fat1 + fatSectors;
  if (rel >= fat0 && rel < fat0 + fatSectors) {
    fillFatSector(buffer, rel - fat0);
    return true;
  }
  if (rel >= fat1 && rel < fat1 + fatSectors) {
    fillFatSector(buffer, rel - fat1);
    return true;
  }
  if (rel >= dataStart) {
    const uint32_t dataRel = rel - dataStart;
    const uint32_t cluster = kRootCluster + (dataRel / kSectorsPerCluster);
    const uint32_t sectorInCluster = dataRel % kSectorsPerCluster;
    if (cluster == kRootCluster) {
      fillRootDirectoryCluster(buffer, sectorInCluster);
      return true;
    }
    return readFileCluster(buffer, cluster, sectorInCluster);
  }

  memset(buffer, 0, kSectorSize);
  return true;
}

int32_t onMscRead(uint32_t lba, uint32_t offset, void *buffer, uint32_t bufsize) {
  uint8_t sector[kSectorSize];
  uint8_t *out = static_cast<uint8_t *>(buffer);
  uint32_t remaining = bufsize;
  uint32_t currentLba = lba;
  uint32_t currentOffset = offset;

  while (remaining > 0) {
    if (!readVirtualSector(currentLba, sector)) return -1;
    const uint32_t chunk = min<uint32_t>(remaining, kSectorSize - currentOffset);
    memcpy(out, sector + currentOffset, chunk);
    out += chunk;
    remaining -= chunk;
    ++currentLba;
    currentOffset = 0;
  }
  return bufsize;
}

int32_t onMscWrite(uint32_t, uint32_t, uint8_t *, uint32_t) {
  return -1;
}

bool onMscStartStop(uint8_t, bool, bool) {
  return true;
}

void startMassStorage() {
  if (currentMode == Mode::Off) {
    Serial.println("Mode OFF: no USB mass storage exposed.");
    return;
  }
  if (!sdReady || !scanSelectedFolder(currentMode)) {
    Serial.println("Selected update set is unavailable; staying OFF.");
    currentMode = Mode::Off;
    return;
  }

  msc.vendorID("ABYSSE8");
  msc.productID(currentMode == Mode::M0 ? "UC-M0" : "UC-M1");
  msc.productRevision("1.0");
  msc.onRead(onMscRead);
  msc.onWrite(onMscWrite);
  msc.onStartStop(onMscStartStop);
  msc.mediaPresent(true);
  msc.begin(blockCount, kSectorSize);
  USB.begin();
  mscStarted = true;
  Serial.printf("USB mass storage active in %s mode.\r\n", modeName(currentMode));
}

void printStatus() {
  Serial.println();
  Serial.println("SemiAutomatedUCProg");
  Serial.printf("Mode: %s\r\n", modeName(currentMode));
  Serial.printf("SD: %s\r\n", sdReady ? "ready" : "not ready");
  Serial.printf("MSC: %s\r\n", mscStarted ? "active" : "not exposed");
  Serial.println("Commands: m0, m1, off, status, reboot");
}

void handleSerialCommand(const String &command) {
  String c = command;
  c.trim();
  c.toLowerCase();
  if (c == "m0" || c == "m1" || c == "off") {
    const Mode newMode = parseMode(c);
    saveMode(newMode);
    Serial.printf("Saved mode %s. Rebooting...\r\n", modeName(newMode));
    delay(250);
    ESP.restart();
  } else if (c == "status") {
    printStatus();
  } else if (c == "reboot") {
    ESP.restart();
  } else if (c.length()) {
    Serial.println("Unknown command. Use: m0, m1, off, status, reboot");
  }
}

}  // namespace

void setup() {
#if UC_STATUS_LED >= 0
  pinMode(UC_STATUS_LED, OUTPUT);
  setLed(false);
#endif
  pinMode(UC_MODE_BUTTON, INPUT_PULLUP);

  Serial.begin(115200);
  delay(1500);

  currentMode = loadMode();
  if (digitalRead(UC_MODE_BUTTON) == LOW) {
    currentMode = nextMode(currentMode);
    saveMode(currentMode);
    blink(static_cast<uint8_t>(currentMode) + 1);
    delay(800);
  }

  sdReady = sd.begin(SdSpiConfig(UC_SD_CS, DEDICATED_SPI, SD_SCK_MHZ(25)));
  if (!sdReady) {
    Serial.println("SD init failed. Check CS pin/wiring.");
  }

  printStatus();
  startMassStorage();
}

void loop() {
  static String line;
  while (Serial.available()) {
    const char ch = static_cast<char>(Serial.read());
    if (ch == '\r' || ch == '\n') {
      handleSerialCommand(line);
      line = "";
    } else {
      line += ch;
    }
  }
}
