#include "core/system/usb_msc.h"

#include <string.h>

#include "core/hardware/sd_card.h"

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "sdmmc_cmd.h"
#include "tinyusb.h"
#include "tinyusb_default_config.h"
#include "tinyusb_msc.h"
#include "tusb.h"

static const char *TAG = "usb_msc";

/* SD 卡一律按 512 字节块暴露给电脑（MSC 的标准块大小，也是 SD 的物理扇区） */
#define USB_MSC_BLOCK_SIZE   512

/* ------------------------------------------------------------------ */
/* USB 设备描述符                                                       */
/* ------------------------------------------------------------------ */

/* 端点号：MSC 只需要一对 Bulk IN/OUT。0x81/0x01 是设备模式下最保险的取值
   （控制端点 0x00/0x80 之外的第一对）。 */
#define EPNUM_MSC_OUT   0x01
#define EPNUM_MSC_IN    0x81

#define TUSB_DESC_TOTAL_LEN  (TUD_CONFIG_DESC_LEN + TUD_MSC_DESC_LEN)

static const tusb_desc_device_t s_device_desc = {
    .bLength            = sizeof(tusb_desc_device_t),
    .bDescriptorType    = TUSB_DESC_DEVICE,
    .bcdUSB             = 0x0200,
    .bDeviceClass       = TUSB_CLASS_MISC,
    .bDeviceSubClass    = MISC_SUBCLASS_COMMON,
    .bDeviceProtocol    = MISC_PROTOCOL_IAD,
    .bMaxPacketSize0    = CFG_TUD_ENDPOINT0_SIZE,
    .idVendor           = 0x303A,   /* Espressif VID */
    .idProduct          = 0x4002,   /* 与官方 MSC 示例一致的 PID（只做区分用） */
    .bcdDevice          = 0x0100,
    .iManufacturer      = 0x01,
    .iProduct           = 0x02,
    .iSerialNumber      = 0x03,
    .bNumConfigurations = 0x01,
};

/* 全速配置：EP 大小 64（FS Bulk 上限） */
static const uint8_t s_config_fs[] = {
    TUD_CONFIG_DESCRIPTOR(1, 1, 0, TUSB_DESC_TOTAL_LEN,
                          TUSB_DESC_CONFIG_ATT_REMOTE_WAKEUP, 100),
    TUD_MSC_DESCRIPTOR(0, 4, EPNUM_MSC_OUT, EPNUM_MSC_IN, 64),
};

#if (TUD_OPT_HIGH_SPEED)
/* 高速配置：EP 大小 512（HS Bulk 上限）。S31 的 USB OTG 是 HS（内置 UTMI PHY），
   走这条描述符时传输效率高一个数量级。 */
static const uint8_t s_config_hs[] = {
    TUD_CONFIG_DESCRIPTOR(1, 1, 0, TUSB_DESC_TOTAL_LEN,
                          TUSB_DESC_CONFIG_ATT_REMOTE_WAKEUP, 100),
    TUD_MSC_DESCRIPTOR(0, 4, EPNUM_MSC_OUT, EPNUM_MSC_IN, 512),
};

static const tusb_desc_device_qualifier_t s_device_qualifier = {
    .bLength            = sizeof(tusb_desc_device_qualifier_t),
    .bDescriptorType    = TUSB_DESC_DEVICE_QUALIFIER,
    .bcdUSB             = 0x0200,
    .bDeviceClass       = TUSB_CLASS_MISC,
    .bDeviceSubClass    = MISC_SUBCLASS_COMMON,
    .bDeviceProtocol    = MISC_PROTOCOL_IAD,
    .bMaxPacketSize0    = CFG_TUD_ENDPOINT0_SIZE,
    .bNumConfigurations = 0x01,
    .bReserved          = 0,
};
#endif

/* 字符串描述符：0 = 语言，1 = 厂商，2 = 产品，3 = 序列号，4 = MSC 接口 */
static char const *s_string_desc[] = {
    (const char[]){ 0x09, 0x04 },   /* 0: English (0x0409) */
    "FakePodNano",
    "FakePodNano SD Card",
    "FPNANOPRO0001",
    "SD Card",
};

/* ------------------------------------------------------------------ */
/* 状态                                                                */
/* ------------------------------------------------------------------ */

static bool s_active = false;                 /* 已成功进入 USB 传歌模式 */
static uint64_t s_capacity_bytes = 0;         /* SD 卡容量（字节） */
static tinyusb_msc_storage_handle_t s_storage = NULL;

/* ------------------------------------------------------------------ */
/* 对外接口                                                            */
/* ------------------------------------------------------------------ */

usb_msc_result_t usb_msc_start(void)
{
    if (s_active) {
        return USB_MSC_ERR_STATE;
    }

    /* ① SD 卡切到原始块设备模式（内部会先卸载 FATFS）。
          这一步之后里世界不能再访问 /sdcard。 */
    sdmmc_card_t *card = NULL;
    esp_err_t err = sd_card_raw_init(&card);
    if (err != ESP_OK || card == NULL) {
        if (err == ESP_ERR_INVALID_STATE && card != NULL) {
            /* 上一次已经切过 raw（异常路径），继续用这张卡 */
            ESP_LOGW(TAG, "SD 卡已在原始模式，继续安装 USB 栈");
        } else {
            ESP_LOGE(TAG, "SD 卡原始模式初始化失败：%s", esp_err_to_name(err));
            return USB_MSC_ERR_NO_SD;
        }
    }
    s_capacity_bytes = (uint64_t)card->csd.capacity * USB_MSC_BLOCK_SIZE;

    /* ② 注册 MSC 存储介质（挂载点直接设为 USB：不留给固件自己的 FATFS），
          再由 TinyUSB MSC 类驱动完成 SCSI READ10/WRITE10 的实现。 */
    tinyusb_msc_storage_config_t storage_cfg = {
        .mount_point = TINYUSB_MSC_STORAGE_MOUNT_USB,
        .fat_fs = {
            .base_path = NULL,          /* 用组件默认挂载点 */
            .config.max_files = 4,
            /* ⚠ 绝不自动格式化：内存卡里是用户的歌，宁可报错也不能清盘
               （mount_point=USB 时组件本来就不挂 FATFS，这里再显式兜一层） */
            .do_not_format = true,
            .format_flags = 0,
        },
    };
    storage_cfg.medium.card = card;

    err = tinyusb_msc_new_storage_sdmmc(&storage_cfg, &s_storage);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "注册 MSC 存储失败：%s", esp_err_to_name(err));
        s_storage = NULL;
        return USB_MSC_ERR_NO_SD;
    }

    /* ③ 安装 TinyUSB 设备栈：S31 的 USB OTG 由 esp_tinyusb 内部按 SoC 配置
          PHY（HS/UTMI），这里只提供描述符。 */
    tinyusb_config_t tusb_cfg = TINYUSB_DEFAULT_CONFIG();
    tusb_cfg.descriptor.device = &s_device_desc;
    tusb_cfg.descriptor.full_speed_config = s_config_fs;
    tusb_cfg.descriptor.string = s_string_desc;
    tusb_cfg.descriptor.string_count = sizeof(s_string_desc) / sizeof(s_string_desc[0]);
#if (TUD_OPT_HIGH_SPEED)
    tusb_cfg.descriptor.high_speed_config = s_config_hs;
    tusb_cfg.descriptor.qualifier = &s_device_qualifier;
#endif

    err = tinyusb_driver_install(&tusb_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "TinyUSB 安装失败：%s", esp_err_to_name(err));
        tinyusb_msc_delete_storage(s_storage);
        s_storage = NULL;
        return USB_MSC_ERR_USB;
    }

    s_active = true;
    ESP_LOGI(TAG, "USB 传歌模式已启动：%s，容量 %llu 字节",
             card->cid.name, (unsigned long long)s_capacity_bytes);
    return USB_MSC_OK;
}

bool usb_msc_is_active(void)
{
    return s_active;
}

bool usb_msc_host_mounted(void)
{
    return s_active && tud_mounted();
}

uint64_t usb_msc_capacity_bytes(void)
{
    return s_capacity_bytes;
}

const char *usb_msc_result_text(usb_msc_result_t result)
{
    switch (result) {
    case USB_MSC_OK:
        return "USB 传歌已就绪";
    case USB_MSC_ERR_STATE:
        return "USB 传歌已在运行";
    case USB_MSC_ERR_NO_SD:
        return "SD 卡不可用：请插卡后重试";
    case USB_MSC_ERR_NO_MEM:
        return "内存不足，无法启动 USB 传歌";
    case USB_MSC_ERR_USB:
        return "USB 设备栈启动失败";
    default:
        return "USB 传歌启动失败";
    }
}
