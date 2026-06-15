#include "usb_update_msc.h"

#include <dirent.h>
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#include "driver/sdmmc_host.h"
#include "esp_check.h"
#include "esp_err.h"
#include "esp_idf_version.h"
#include "esp_log.h"
#include "esp_vfs_fat.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdmmc_cmd.h"
#include "sdkconfig.h"
#include "tinyusb.h"
#include "tinyusb_default_config.h"
#include "tusb.h"

#if SOC_SDMMC_IO_POWER_EXTERNAL
#include "sd_pwr_ctrl_by_on_chip_ldo.h"
#endif

#define MOUNT_POINT "/sdcard"
#define SECTOR_SIZE 512
#define PARTITION_START 2048
#define RESERVED_SECTORS 32
#define FAT_COUNT 2
#define SECTORS_PER_CLUSTER 64
#define CLUSTER_SIZE (SECTOR_SIZE * SECTORS_PER_CLUSTER)
#define ROOT_CLUSTER 2
#define MAX_FILES 64
#define MAX_NAME 256
#define SCSI_CMD_SYNC_CACHE_10 0x35

#if CONFIG_ESP_HOSTED_SDIO_HOST_INTERFACE && (ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(6, 0, 0))
#define HOSTED_DOES_SDMMC_HOST_INIT 1
#else
#define HOSTED_DOES_SDMMC_HOST_INIT 0
#endif

typedef struct {
    char name[MAX_NAME];
    char path[MAX_NAME + 32];
    uint64_t size;
    uint32_t first_cluster;
    uint32_t cluster_count;
    char short_name[11];
    uint8_t short_checksum;
} file_entry_t;

static const char *TAG = "usb_update";
static sdmmc_card_t *s_card;
#if SOC_SDMMC_IO_POWER_EXTERNAL
static sd_pwr_ctrl_handle_t s_pwr_ctrl_handle;
#endif
static bool s_sd_mounted;
static bool s_usb_active;
static bool s_ejected;
static uc_usb_mode_t s_mode = UC_USB_MODE_OFF;
static file_entry_t s_files[MAX_FILES];
static size_t s_file_count;
static uint32_t s_data_cluster_count = 1;
static uint32_t s_fat_sectors = 1;
static uint32_t s_volume_sectors;
static uint32_t s_block_count;

const char *uc_usb_mode_name(uc_usb_mode_t mode) {
    switch (mode) {
    case UC_USB_MODE_M0:
        return "M0";
    case UC_USB_MODE_M1:
        return "M1";
    default:
        return "OFF";
    }
}

bool uc_usb_update_active(void) {
    return s_usb_active;
}

static const char *mode_folder(uc_usb_mode_t mode) {
    switch (mode) {
    case UC_USB_MODE_M0:
        return MOUNT_POINT "/M0";
    case UC_USB_MODE_M1:
        return MOUNT_POINT "/M1";
    default:
        return "";
    }
}

static uint32_t ceil_div_u64(uint64_t value, uint32_t divisor) {
    return (uint32_t)((value + divisor - 1) / divisor);
}

static void write_le16(uint8_t *p, uint16_t v) {
    p[0] = (uint8_t)(v & 0xff);
    p[1] = (uint8_t)(v >> 8);
}

static void write_le32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v & 0xff);
    p[1] = (uint8_t)((v >> 8) & 0xff);
    p[2] = (uint8_t)((v >> 16) & 0xff);
    p[3] = (uint8_t)((v >> 24) & 0xff);
}

static uint8_t lfn_checksum(const char short_name[11]) {
    uint8_t sum = 0;
    for (uint8_t i = 0; i < 11; ++i) {
        sum = (uint8_t)(((sum & 1) ? 0x80 : 0) + (sum >> 1) + (uint8_t)short_name[i]);
    }
    return sum;
}

static char upper_ascii(char c) {
    return (c >= 'a' && c <= 'z') ? (char)(c - ('a' - 'A')) : c;
}

static void make_short_name(size_t index, const char *name, char out[11]) {
    memset(out, ' ', 11);
    snprintf(out, 9, "UC%06u", (unsigned)(index + 1));
    for (uint8_t i = 0; i < 8 && out[i] != '\0'; ++i) {
        out[i] = upper_ascii(out[i]);
    }

    const char *dot = strrchr(name, '.');
    const char *ext = dot ? dot + 1 : "";
    if (strcasecmp(ext, "gz") == 0) {
        memcpy(out + 8, "TGZ", 3);
    } else if (strcasecmp(ext, "json") == 0) {
        memcpy(out + 8, "JSN", 3);
    } else {
        for (uint8_t i = 0; i < 3 && ext[i] != '\0'; ++i) {
            out[8 + i] = upper_ascii(ext[i]);
        }
    }
}

#if HOSTED_DOES_SDMMC_HOST_INIT
static esp_err_t sdmmc_host_init_dummy(void) {
    return ESP_OK;
}

static esp_err_t sdmmc_host_deinit_dummy(void) {
    return ESP_OK;
}
#endif

static esp_err_t mount_sd_card(void) {
    if (s_sd_mounted) {
        return ESP_OK;
    }

    esp_vfs_fat_sdmmc_mount_config_t mount_config = {
        .format_if_mount_failed = false,
        .max_files = MAX_FILES + 4,
        .allocation_unit_size = 16 * 1024,
    };

    sdmmc_host_t host = SDMMC_HOST_DEFAULT();
    host.slot = SDMMC_HOST_SLOT_0;
#if HOSTED_DOES_SDMMC_HOST_INIT
    host.init = sdmmc_host_init_dummy;
    host.deinit = sdmmc_host_deinit_dummy;
#endif

#if SOC_SDMMC_IO_POWER_EXTERNAL
    sd_pwr_ctrl_ldo_config_t ldo_config = {
        .ldo_chan_id = 4,
    };
    ESP_RETURN_ON_ERROR(sd_pwr_ctrl_new_on_chip_ldo(&ldo_config, &s_pwr_ctrl_handle), TAG, "SD LDO setup failed");
    host.pwr_ctrl_handle = s_pwr_ctrl_handle;
#endif

    sdmmc_slot_config_t slot_config = SDMMC_SLOT_CONFIG_DEFAULT();
    slot_config.width = 4;
#ifdef CONFIG_SOC_SDMMC_USE_GPIO_MATRIX
    slot_config.clk = 43;
    slot_config.cmd = 44;
    slot_config.d0 = 39;
    slot_config.d1 = 40;
    slot_config.d2 = 41;
    slot_config.d3 = 42;
#endif
    slot_config.flags |= SDMMC_SLOT_FLAG_INTERNAL_PULLUP;

    ESP_LOGI(TAG, "Mounting SD card on SDMMC slot 0");
    esp_err_t ret = esp_vfs_fat_sdmmc_mount(MOUNT_POINT, &host, &slot_config, &mount_config, &s_card);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "SD mount failed: %s", esp_err_to_name(ret));
        return ret;
    }
    s_sd_mounted = true;
    sdmmc_card_print_info(stdout, s_card);
    return ESP_OK;
}

static bool scan_selected_folder(uc_usb_mode_t mode) {
    s_file_count = 0;
    s_data_cluster_count = 1;

    const char *folder = mode_folder(mode);
    DIR *dir = opendir(folder);
    if (!dir) {
        ESP_LOGE(TAG, "Cannot open selected folder: %s errno=%d", folder, errno);
        return false;
    }

    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL && s_file_count < MAX_FILES) {
        if (entry->d_name[0] == '.') {
            continue;
        }

        file_entry_t *file = &s_files[s_file_count];
        memset(file, 0, sizeof(*file));
        strlcpy(file->name, entry->d_name, sizeof(file->name));
        snprintf(file->path, sizeof(file->path), "%s/%s", folder, entry->d_name);

        struct stat st;
        if (stat(file->path, &st) != 0 || S_ISDIR(st.st_mode)) {
            continue;
        }

        file->size = (uint64_t)st.st_size;
        file->cluster_count = (uint32_t)((file->size + CLUSTER_SIZE - 1) / CLUSTER_SIZE);
        if (file->cluster_count == 0) {
            file->cluster_count = 1;
        }
        file->first_cluster = ROOT_CLUSTER + s_data_cluster_count;
        make_short_name(s_file_count, file->name, file->short_name);
        file->short_checksum = lfn_checksum(file->short_name);
        s_data_cluster_count += file->cluster_count;
        ++s_file_count;
    }
    closedir(dir);

    const uint32_t fat_entries = ROOT_CLUSTER + s_data_cluster_count;
    s_fat_sectors = ceil_div_u64((uint64_t)fat_entries * 4, SECTOR_SIZE);
    s_volume_sectors = RESERVED_SECTORS + (FAT_COUNT * s_fat_sectors) + (s_data_cluster_count * SECTORS_PER_CLUSTER);
    s_block_count = PARTITION_START + s_volume_sectors;

    ESP_LOGI(TAG, "Mode %s exposes %u files, %lu sectors",
             uc_usb_mode_name(mode), (unsigned)s_file_count, (unsigned long)s_block_count);
    return s_file_count > 0;
}

static void fill_mbr(uint8_t *sector) {
    memset(sector, 0, SECTOR_SIZE);
    sector[446] = 0x00;
    sector[450] = 0x0c;
    write_le32(sector + 454, PARTITION_START);
    write_le32(sector + 458, s_volume_sectors);
    sector[510] = 0x55;
    sector[511] = 0xaa;
}

static void fill_boot_sector(uint8_t *sector) {
    memset(sector, 0, SECTOR_SIZE);
    sector[0] = 0xeb;
    sector[1] = 0x58;
    sector[2] = 0x90;
    memcpy(sector + 3, "MSDOS5.0", 8);
    write_le16(sector + 11, SECTOR_SIZE);
    sector[13] = SECTORS_PER_CLUSTER;
    write_le16(sector + 14, RESERVED_SECTORS);
    sector[16] = FAT_COUNT;
    sector[21] = 0xf8;
    write_le16(sector + 24, 63);
    write_le16(sector + 26, 255);
    write_le32(sector + 28, PARTITION_START);
    write_le32(sector + 32, s_volume_sectors);
    write_le32(sector + 36, s_fat_sectors);
    write_le32(sector + 44, ROOT_CLUSTER);
    write_le16(sector + 48, 1);
    write_le16(sector + 50, 6);
    sector[64] = 0x80;
    sector[66] = 0x29;
    write_le32(sector + 67, 0x20260615);
    memcpy(sector + 71, "UCUPDATE   ", 11);
    memcpy(sector + 82, "FAT32   ", 8);
    sector[510] = 0x55;
    sector[511] = 0xaa;
}

static void fill_fs_info(uint8_t *sector) {
    memset(sector, 0, SECTOR_SIZE);
    write_le32(sector + 0, 0x41615252);
    write_le32(sector + 484, 0x61417272);
    write_le32(sector + 488, 0xffffffff);
    write_le32(sector + 492, 0xffffffff);
    write_le32(sector + 508, 0xaa550000);
}

static uint32_t fat_entry_for_cluster(uint32_t cluster) {
    if (cluster == 0) return 0x0ffffff8;
    if (cluster == 1) return 0xffffffff;
    if (cluster == ROOT_CLUSTER) return 0x0fffffff;

    for (size_t i = 0; i < s_file_count; ++i) {
        const file_entry_t *entry = &s_files[i];
        if (cluster >= entry->first_cluster && cluster < entry->first_cluster + entry->cluster_count) {
            const uint32_t offset = cluster - entry->first_cluster;
            return (offset + 1 >= entry->cluster_count) ? 0x0fffffff : cluster + 1;
        }
    }
    return 0;
}

static void fill_fat_sector(uint8_t *sector, uint32_t fat_relative_sector) {
    memset(sector, 0, SECTOR_SIZE);
    const uint32_t first_entry = (fat_relative_sector * SECTOR_SIZE) / 4;
    for (uint32_t i = 0; i < SECTOR_SIZE / 4; ++i) {
        write_le32(sector + (i * 4), fat_entry_for_cluster(first_entry + i));
    }
}

static void fill_lfn_entry(uint8_t *entry, const char *name, uint8_t lfn_index, uint8_t lfn_count, uint8_t checksum) {
    memset(entry, 0xff, 32);
    entry[0] = lfn_index;
    if (lfn_index == lfn_count) entry[0] |= 0x40;
    entry[11] = 0x0f;
    entry[13] = checksum;
    write_le16(entry + 26, 0);

    const int start = (lfn_index - 1) * 13;
    const uint8_t slots[13] = {1, 3, 5, 7, 9, 14, 16, 18, 20, 22, 24, 28, 30};
    const size_t len = strlen(name);
    for (uint8_t i = 0; i < 13; ++i) {
        const int name_index = start + i;
        uint16_t value = 0xffff;
        if (name_index < (int)len) {
            value = (uint8_t)name[name_index];
        } else if (name_index == (int)len) {
            value = 0;
        }
        write_le16(entry + slots[i], value);
    }
}

static void fill_short_dir_entry(uint8_t *entry, const file_entry_t *file) {
    memset(entry, 0, 32);
    memcpy(entry, file->short_name, 11);
    entry[11] = 0x20;
    write_le16(entry + 20, (file->first_cluster >> 16) & 0xffff);
    write_le16(entry + 26, file->first_cluster & 0xffff);
    write_le32(entry + 28, (uint32_t)file->size);
}

static void fill_root_directory_cluster(uint8_t *buffer, uint32_t sector_in_cluster) {
    memset(buffer, 0, SECTOR_SIZE);
    const uint32_t first_entry = (sector_in_cluster * SECTOR_SIZE) / 32;
    uint32_t virtual_entry = 0;

    for (size_t i = 0; i < s_file_count; ++i) {
        const file_entry_t *file = &s_files[i];
        uint8_t lfn_count = (uint8_t)ceil_div_u64(strlen(file->name), 13);
        if (lfn_count == 0) lfn_count = 1;
        for (int lfn = lfn_count; lfn >= 1; --lfn) {
            if (virtual_entry >= first_entry && virtual_entry < first_entry + 16) {
                fill_lfn_entry(buffer + ((virtual_entry - first_entry) * 32), file->name, (uint8_t)lfn, lfn_count, file->short_checksum);
            }
            ++virtual_entry;
        }
        if (virtual_entry >= first_entry && virtual_entry < first_entry + 16) {
            fill_short_dir_entry(buffer + ((virtual_entry - first_entry) * 32), file);
        }
        ++virtual_entry;
    }
}

static bool read_file_cluster(uint8_t *buffer, uint32_t cluster, uint32_t sector_in_cluster) {
    memset(buffer, 0, SECTOR_SIZE);
    for (size_t i = 0; i < s_file_count; ++i) {
        const file_entry_t *entry = &s_files[i];
        if (cluster >= entry->first_cluster && cluster < entry->first_cluster + entry->cluster_count) {
            const uint64_t offset = ((uint64_t)(cluster - entry->first_cluster) * CLUSTER_SIZE) +
                                    ((uint64_t)sector_in_cluster * SECTOR_SIZE);
            if (offset >= entry->size) {
                return true;
            }

            FILE *f = fopen(entry->path, "rb");
            if (!f) return false;
            if (fseek(f, (long)offset, SEEK_SET) != 0) {
                fclose(f);
                return false;
            }
            const size_t to_read = (size_t)((entry->size - offset) > SECTOR_SIZE ? SECTOR_SIZE : (entry->size - offset));
            fread(buffer, 1, to_read, f);
            fclose(f);
            return true;
        }
    }
    return true;
}

static bool read_virtual_sector(uint32_t lba, uint8_t *buffer) {
    if (lba == 0) {
        fill_mbr(buffer);
        return true;
    }
    if (lba < PARTITION_START || lba >= s_block_count) {
        memset(buffer, 0, SECTOR_SIZE);
        return true;
    }

    const uint32_t rel = lba - PARTITION_START;
    if (rel == 0 || rel == 6) {
        fill_boot_sector(buffer);
        return true;
    }
    if (rel == 1 || rel == 7) {
        fill_fs_info(buffer);
        return true;
    }
    if (rel < RESERVED_SECTORS) {
        memset(buffer, 0, SECTOR_SIZE);
        return true;
    }

    const uint32_t fat0 = RESERVED_SECTORS;
    const uint32_t fat1 = fat0 + s_fat_sectors;
    const uint32_t data_start = fat1 + s_fat_sectors;
    if (rel >= fat0 && rel < fat0 + s_fat_sectors) {
        fill_fat_sector(buffer, rel - fat0);
        return true;
    }
    if (rel >= fat1 && rel < fat1 + s_fat_sectors) {
        fill_fat_sector(buffer, rel - fat1);
        return true;
    }
    if (rel >= data_start) {
        const uint32_t data_rel = rel - data_start;
        const uint32_t cluster = ROOT_CLUSTER + (data_rel / SECTORS_PER_CLUSTER);
        const uint32_t sector_in_cluster = data_rel % SECTORS_PER_CLUSTER;
        if (cluster == ROOT_CLUSTER) {
            fill_root_directory_cluster(buffer, sector_in_cluster);
            return true;
        }
        return read_file_cluster(buffer, cluster, sector_in_cluster);
    }

    memset(buffer, 0, SECTOR_SIZE);
    return true;
}

esp_err_t uc_usb_update_start(uc_usb_mode_t mode) {
    s_mode = mode;
    if (mode == UC_USB_MODE_OFF) {
        ESP_LOGW(TAG, "USB MSC mode OFF: no update disk exposed");
        return ESP_OK;
    }

    ESP_RETURN_ON_ERROR(mount_sd_card(), TAG, "SD card mount failed");
    ESP_RETURN_ON_FALSE(scan_selected_folder(mode), ESP_FAIL, TAG, "Selected folder has no visible files");

    tinyusb_config_t tusb_cfg = TINYUSB_DEFAULT_CONFIG();
    ESP_RETURN_ON_ERROR(tinyusb_driver_install(&tusb_cfg), TAG, "TinyUSB install failed");
    s_usb_active = true;
    s_ejected = false;
    ESP_LOGW(TAG, "USB mass storage ACTIVE in %s mode; only /%s is exposed read-only",
             uc_usb_mode_name(mode), uc_usb_mode_name(mode));
    return ESP_OK;
}

uint32_t tud_msc_inquiry2_cb(uint8_t lun, scsi_inquiry_resp_t *inquiry_resp, uint32_t bufsize) {
    (void)lun;
    (void)bufsize;
    const char vid[] = "ABYSSE8";
    const char pid[] = "UC Update";
    const char rev[] = "1.0";
    memcpy(inquiry_resp->vendor_id, vid, strlen(vid));
    memcpy(inquiry_resp->product_id, pid, strlen(pid));
    memcpy(inquiry_resp->product_rev, rev, strlen(rev));
    return sizeof(scsi_inquiry_resp_t);
}

bool tud_msc_test_unit_ready_cb(uint8_t lun) {
    if (!s_usb_active || s_ejected) {
        tud_msc_set_sense(lun, SCSI_SENSE_NOT_READY, 0x3a, 0x00);
        return false;
    }
    return true;
}

void tud_msc_capacity_cb(uint8_t lun, uint32_t *block_count, uint16_t *block_size) {
    (void)lun;
    *block_count = s_block_count;
    *block_size = SECTOR_SIZE;
}

bool tud_msc_start_stop_cb(uint8_t lun, uint8_t power_condition, bool start, bool load_eject) {
    (void)lun;
    (void)power_condition;
    if (load_eject && !start) {
        s_ejected = true;
    }
    return true;
}

bool tud_msc_is_writable_cb(uint8_t lun) {
    (void)lun;
    return false;
}

int32_t tud_msc_read10_cb(uint8_t lun, uint32_t lba, uint32_t offset, void *buffer, uint32_t bufsize) {
    (void)lun;
    uint8_t sector[SECTOR_SIZE];
    uint8_t *out = (uint8_t *)buffer;
    uint32_t remaining = bufsize;
    uint32_t current_lba = lba;
    uint32_t current_offset = offset;

    while (remaining > 0) {
        if (!read_virtual_sector(current_lba, sector)) {
            return -1;
        }
        const uint32_t chunk = remaining < (SECTOR_SIZE - current_offset) ? remaining : (SECTOR_SIZE - current_offset);
        memcpy(out, sector + current_offset, chunk);
        out += chunk;
        remaining -= chunk;
        ++current_lba;
        current_offset = 0;
    }
    return (int32_t)bufsize;
}

int32_t tud_msc_write10_cb(uint8_t lun, uint32_t lba, uint32_t offset, uint8_t *buffer, uint32_t bufsize) {
    (void)lun;
    (void)lba;
    (void)offset;
    (void)buffer;
    return (int32_t)bufsize;
}

int32_t tud_msc_scsi_cb(uint8_t lun, uint8_t const scsi_cmd[16], void *buffer, uint16_t bufsize) {
    (void)buffer;
    (void)bufsize;
    switch (scsi_cmd[0]) {
    case SCSI_CMD_PREVENT_ALLOW_MEDIUM_REMOVAL:
    case SCSI_CMD_SYNC_CACHE_10:
        return 0;
    default:
        tud_msc_set_sense(lun, SCSI_SENSE_ILLEGAL_REQUEST, 0x20, 0x00);
        return -1;
    }
}
