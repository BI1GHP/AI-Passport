// main/app_store.c
#include "app_store.h"

#include "esp_log.h"
#include "esp_vfs_fat.h"
#include "wear_levelling.h"

#include <errno.h>
#include <stdio.h>
#include <sys/stat.h>

static const char *TAG = "app_store";
static wl_handle_t s_wl = WL_INVALID_HANDLE;

esp_err_t app_store_init(void) {
    esp_vfs_fat_mount_config_t cfg = {
        .format_if_mount_failed = true,
        .max_files = 4,
        .allocation_unit_size = 0,
        .disk_status_check_enable = false,
    };
    esp_err_t e = esp_vfs_fat_spiflash_mount_rw_wl(APP_STORE_BASE, "storage", &cfg, &s_wl);
    if (e != ESP_OK) {
        ESP_LOGE(TAG, "FAT 挂载失败: %s", esp_err_to_name(e));
        return e;
    }
    static const char *DIRS[] = { "/badge/rec", "/badge/rem" };
    for (size_t i = 0; i < sizeof(DIRS) / sizeof(DIRS[0]); i++) {
        if (mkdir(DIRS[i], 0775) != 0 && errno != EEXIST) {
            ESP_LOGW(TAG, "创建目录失败 %s (errno=%d)", DIRS[i], errno);
        }
    }
    ESP_LOGI(TAG, "FAT 已挂载于 %s", APP_STORE_BASE);
    return ESP_OK;
}

bool app_store_ready(void) {
    return s_wl != WL_INVALID_HANDLE;
}

void app_store_path(char *buf, size_t size, const char *rel) {
    snprintf(buf, size, "%s/%s", APP_STORE_BASE, rel);
}
