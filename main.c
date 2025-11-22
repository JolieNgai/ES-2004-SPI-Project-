// main.c - Single file: flash driver + FIMG backup/restore + CSV benchmark

#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <math.h>
#include <stdlib.h>

#include "pico/stdlib.h"
#include "hardware/spi.h"

#include "ff.h"
#include "diskio.h"
#include "spi.h"
#include "sd_card.h"

// =====================================================
// ===============  HARDWARE PIN CONFIG  ================
//
//  External SPI Flash on SPI0
//      GP2  = SCK
//      GP3  = MOSI
//      GP4  = MISO
//      GP5  = CS
//
//  SD Card on SPI1 (Maker Pi Pico)
//      GP10 = SCK
//      GP11 = MOSI
//      GP12 = MISO
//      GP13 = CS
// =====================================================

// -------- SPI0: external flash --------
#define FLASH_SPI_PORT   spi0
#define FLASH_PIN_SCK    2
#define FLASH_PIN_MOSI   3
#define FLASH_PIN_MISO   4
#define FLASH_PIN_CS     5

#define FLASH_SPI_HZ     (1u * 1000u * 1000u) // 1 MHz (safe), 10 MHz (faster)

// -------- SPI1: SD card (inlined hw_config.c) --------
static spi_t sd_spi = {
    .hw_inst            = spi1,
    .miso_gpio          = 12,
    .mosi_gpio          = 11,
    .sck_gpio           = 10,
    .baud_rate          = 10 * 1000 * 1000,   // can lower to 400k if needed
    .set_drive_strength = false,
};

static sd_card_t sd = {
    .pcName             = "0:",
    .spi                = &sd_spi,
    .ss_gpio            = 13,        // CS
    .use_card_detect    = false,     // no CD switch on Maker Pi Pico
    .card_detect_gpio   = 0,
    .card_detected_true = 1,
    .set_drive_strength = false,
};

// Required by FatFs_SPI library
size_t spi_get_num(void)            { return 1; }
spi_t *spi_get_by_num(size_t n)     { return (n == 0) ? &sd_spi : NULL; }
size_t sd_get_num(void)             { return 1; }
sd_card_t *sd_get_by_num(size_t n)  { return (n == 0) ? &sd : NULL; }

// =====================================================
// ===============  FLASH DUT (JEDEC DRIVER) ============
// =====================================================

typedef struct {
    uint8_t manuf_id;
    uint8_t mem_type;
    uint8_t capacity_id;
} jedec_info_t;

// Basic opcodes
#define CMD_WREN        0x06
#define CMD_WRDI        0x04
#define CMD_RDSR1       0x05
#define CMD_RDSR2       0x35
#define CMD_WRSR        0x01
#define CMD_READ        0x03
#define CMD_PP          0x02
#define CMD_SE_4K       0x20
#define CMD_BE_64K      0xD8
#define CMD_CHIP_ERASE  0xC7
#define CMD_JEDEC_ID    0x9F
#define CMD_SFDP        0x5A
#define CMD_RSTEN       0x66
#define CMD_RST         0x99
#define CMD_ULBPR       0x98      
#define CMD_RESUME      0x7A      

#define FLASH_PAGE_SIZE   256
#define FLASH_SECTOR_SIZE 4096

static inline void flash_cs_low(void) {
    asm volatile("nop; nop; nop;");
    gpio_put(FLASH_PIN_CS, 0);
    asm volatile("nop; nop; nop;");
}

static inline void flash_cs_high(void) {
    asm volatile("nop; nop; nop;");
    gpio_put(FLASH_PIN_CS, 1);
    asm volatile("nop; nop; nop;");
}

static inline void flash_cmd1(uint8_t cmd) {
    flash_cs_low();
    spi_write_blocking(FLASH_SPI_PORT, &cmd, 1);
    flash_cs_high();
}

static inline void flash_wren(void) {
    flash_cmd1(CMD_WREN);
}

static uint8_t flash_read_sr1(void) {
    uint8_t tx[2] = { CMD_RDSR1, 0x00 }, rx[2] = {0};
    flash_cs_low();
    spi_write_read_blocking(FLASH_SPI_PORT, tx, rx, 2);
    flash_cs_high();
    return rx[1];
}

static uint8_t flash_read_sr2(void) {
    uint8_t tx[2] = { CMD_RDSR2, 0x00 }, rx[2] = {0};
    flash_cs_low();
    spi_write_read_blocking(FLASH_SPI_PORT, tx, rx, 2);
    flash_cs_high();
    return rx[1];
}

static bool flash_wait_busy_timeout(uint32_t timeout_ms) {
    uint32_t t0 = to_ms_since_boot(get_absolute_time());
    while (true) {
        if ((flash_read_sr1() & 0x01) == 0) return true; // WIP=0
        if (to_ms_since_boot(get_absolute_time()) - t0 > timeout_ms) return false;
    }
}

static void flash_soft_reset(void) {
    flash_cmd1(CMD_RSTEN);
    sleep_ms(1);
    flash_cmd1(CMD_RST);
    sleep_ms(10);
}

static void flash_resume(void) {
    flash_cmd1(CMD_RESUME);
}

// Clear protection: try ULBPR then clear BP bits via WRSR
static void flash_global_unprotect(void) {
    flash_cmd1(CMD_ULBPR);
    sleep_ms(1);

    // Then explicitly clear SR1/SR2 BP bits
    flash_wren();
    uint8_t wr[3] = { CMD_WRSR, 0x00, 0x00 }; // SR1=0, SR2=0
    flash_cs_low();
    spi_write_blocking(FLASH_SPI_PORT, wr, 3);
    flash_cs_high();
    (void)flash_wait_busy_timeout(200);

    (void)flash_read_sr1();
    (void)flash_read_sr2();
}

// Public DUT-style API
static bool flash_dut_init(void) {
    spi_init(FLASH_SPI_PORT, FLASH_SPI_HZ);
    gpio_set_function(FLASH_PIN_MISO, GPIO_FUNC_SPI);
    gpio_set_function(FLASH_PIN_MOSI, GPIO_FUNC_SPI);
    gpio_set_function(FLASH_PIN_SCK,  GPIO_FUNC_SPI);
    gpio_init(FLASH_PIN_CS);
    gpio_set_dir(FLASH_PIN_CS, GPIO_OUT);
    gpio_put(FLASH_PIN_CS, 1);

    flash_soft_reset();
    flash_global_unprotect();
    return true;
}

static bool flash_dut_read_jedec(jedec_info_t *id) {
    if (!id) return false;

    uint8_t cmd = CMD_JEDEC_ID;
    uint8_t rx[3] = {0};

    flash_cs_low();
    spi_write_blocking(FLASH_SPI_PORT, &cmd, 1);
    spi_read_blocking(FLASH_SPI_PORT, 0, rx, 3);
    flash_cs_high();

    id->manuf_id    = rx[0];
    id->mem_type    = rx[1];
    id->capacity_id = rx[2];
    return true;
}

static bool flash_dut_probe_capacity_sfdp(uint32_t *out_bytes) {
    if (out_bytes) *out_bytes = 0;
    return false;
}

static bool flash_dut_read(uint32_t addr, uint8_t *buf, size_t len) {
    if (!buf || !len) return false;
    uint8_t hdr[4] = { CMD_READ,
                       (uint8_t)(addr >> 16),
                       (uint8_t)(addr >> 8),
                       (uint8_t) addr };
    flash_cs_low();
    spi_write_blocking(FLASH_SPI_PORT, hdr, 4);
    spi_read_blocking(FLASH_SPI_PORT, 0, buf, len);
    flash_cs_high();
    return true;
}

// len: 1..256, caller handles page boundaries
static bool flash_dut_program_page(uint32_t addr, const uint8_t *data, size_t len) {
    if (!data || !len || len > FLASH_PAGE_SIZE) return false;

    flash_wren();
    uint8_t hdr[4] = { CMD_PP,
                       (uint8_t)(addr >> 16),
                       (uint8_t)(addr >> 8),
                       (uint8_t) addr };
    flash_cs_low();
    spi_write_blocking(FLASH_SPI_PORT, hdr, 4);
    spi_write_blocking(FLASH_SPI_PORT, data, len);
    flash_cs_high();
    return flash_wait_busy_timeout(10 * 1000);    // 10s worst-case, usually <<1s
}

// Robust 4K erase with retry
static bool flash_dut_erase_4k(uint32_t addr) {
    uint8_t cmd[4] = { CMD_SE_4K,
                       (uint8_t)(addr >> 16),
                       (uint8_t)(addr >> 8),
                       (uint8_t) addr };

    flash_global_unprotect();

    flash_wren();
    flash_cs_low();
    spi_write_blocking(FLASH_SPI_PORT, cmd, 4);
    flash_cs_high();
    if (!flash_wait_busy_timeout(2000)) {
        uint8_t sr1 = flash_read_sr1();
        uint8_t sr2 = flash_read_sr2();
        printf("Timeout erasing 0x%08x (SR1=0x%02x SR2=0x%02x)\n", addr, sr1, sr2);

        flash_resume();
        flash_soft_reset();
        flash_global_unprotect();

        flash_wren();
        flash_cs_low();
        spi_write_blocking(FLASH_SPI_PORT, cmd, 4);
        flash_cs_high();
        if (!flash_wait_busy_timeout(3000)) {
            sr1 = flash_read_sr1();
            sr2 = flash_read_sr2();
            printf("Timeout erasing 0x%08x (SR1=0x%02x SR2=0x%02x) after retry\n",
                   addr, sr1, sr2);
            return false;
        }
    }
    return true;
}

// map JEDEC capacity code (0x10..0x20) → bytes
static uint32_t flash_calculate_capacity(uint8_t capacity_code) {
    if (capacity_code < 0x10 || capacity_code > 0x20) return 0;
    uint32_t bits = 1u << capacity_code;
    return bits / 8u;
}

// =====================================================
// ===============  FIMG BACKUP / RESTORE ===============
// =====================================================

typedef struct {
    char     magic[8];      // "FIMGv1\0"
    uint8_t  jedec[3];      // manuf, type, capacity_id
    uint8_t  reserved;
    uint32_t flash_size;    // bytes
    uint32_t chunk_size;    // e.g., 4096
    uint32_t image_size;    // bytes of image (usually == flash_size)
    uint32_t crc32_all;     // CRC-32 of the image data (no header)
} __attribute__((packed)) flashimg_hdr_t;

#define DUMP_FOLDER        "FLASHIMG"
#define CHUNK_BYTES        4096u

// mount SD (once) via FatFs_SPI + our hw_config
static bool fs_mount_once(void) {
    static bool mounted = false;
    if (mounted) return true;

    sd_card_t *pSD = sd_get_by_num(0);
    if (!pSD) {
        printf("sd_get_by_num(0) failed\n");
        return false;
    }

    FRESULT fr = f_mount(&pSD->fatfs, pSD->pcName, 1);
    if (fr != FR_OK) {
        printf("f_mount failed: %d\n", fr);
        return false;
    }
    mounted = true;
    return true;
}

// ---- CRC32 (poly 0xEDB88320) ----
static uint32_t crc32_update(uint32_t c, const uint8_t *b, size_t n) {
    c = ~c;
    for (size_t i = 0; i < n; ++i) {
        c ^= b[i];
        for (int k = 0; k < 8; ++k)
            c = (c >> 1) ^ (0xEDB88320u & -(int)(c & 1));
    }
    return ~c;
}

// CRC32 over live flash (streamed)
static int crc32_over_flash(uint32_t total_bytes,
                            uint32_t chunk_bytes,
                            uint32_t *out_crc) {
    if (!out_crc || chunk_bytes == 0) return -1;

    uint8_t *buf = (uint8_t*)malloc(chunk_bytes);
    if (!buf) { printf("OOM.\n"); return -2; }

    uint32_t crc = 0, addr = 0, remain = total_bytes;
    while (remain) {
        uint32_t n = (remain > chunk_bytes) ? chunk_bytes : remain;
        if (!flash_dut_read(addr, buf, n)) {
            free(buf);
            printf("Flash read failed @0x%08x\n", addr);
            return -3;
        }
        crc = crc32_update(crc, buf, n);
        addr   += n;
        remain -= n;
        if ((addr & 0xFFFF) == 0)
            printf("CRC %u / %u KiB\r", addr/1024, total_bytes/1024);
    }
    printf("\n");
    free(buf);
    *out_crc = crc;
    return 0;
}

// ensure /FLASHIMG exists
static void ensure_folder(void) {
    FILINFO i;
    if (f_stat(DUMP_FOLDER, &i) != FR_OK) {
        f_mkdir(DUMP_FOLDER);
    }
}

// timestamp label (simple: ms since boot)
static void fmt_time(char *out, size_t n) {
    uint32_t ms = to_ms_since_boot(get_absolute_time());
    snprintf(out, n, "t%010u", ms);
}

// list .fimg files
static int list_flash_images(void) {
    if (!fs_mount_once()) {
        printf("SD not mounted.\n");
        return -1;
    }
    ensure_folder();

    DIR d; FILINFO f;
    int count = 0;
    if (f_opendir(&d, DUMP_FOLDER) != FR_OK) {
        printf("Open dir failed.\n");
        return -1;
    }

    while (f_readdir(&d, &f) == FR_OK && f.fname[0]) {
        if (strstr(f.fname, ".fimg")) {
            printf("%s/%s\n", DUMP_FOLDER, f.fname);
            count++;
        }
    }
    f_closedir(&d);
    if (!count) printf("(no images found)\n");
    return count;
}

// backup entire flash into /FLASHIMG/<stamp>_<jedec>.fimg
static int backup_flash_to_sd(void) {
    if (!fs_mount_once()) {
        printf("SD mount failed.\n");
        return -1;
    }
    if (!flash_dut_init()) {
        printf("Flash init failed.\n");
        return -2;
    }

    jedec_info_t id;
    if (!flash_dut_read_jedec(&id)) {
        printf("JEDEC read failed.\n");
        return -3;
    }

    uint32_t flash_sz = 16u * 1024u * 1024u;   // default 16MB
    uint32_t sfdp = 0;
    if (flash_dut_probe_capacity_sfdp(&sfdp) && sfdp) {
        flash_sz = sfdp;
    } else {
        uint32_t by_jedec = flash_calculate_capacity(id.capacity_id);
        if (by_jedec) flash_sz = by_jedec;
    }

    ensure_folder();
    char stamp[32]; fmt_time(stamp, sizeof(stamp));

    char name[128];
    snprintf(name, sizeof(name), "%s/%s_%02x%02x%02x.fimg",
             DUMP_FOLDER, stamp, id.manuf_id, id.mem_type, id.capacity_id);

    FIL fp; UINT bw = 0;
    if (f_open(&fp, name, FA_CREATE_ALWAYS | FA_WRITE) != FR_OK) {
        printf("Open %s failed\n", name);
        return -4;
    }

    flashimg_hdr_t h = {0};
    memcpy(h.magic, "FIMGv1\0", 8);
    h.jedec[0]    = id.manuf_id;
    h.jedec[1]    = id.mem_type;
    h.jedec[2]    = id.capacity_id;
    h.flash_size  = flash_sz;
    h.chunk_size  = CHUNK_BYTES;
    h.image_size  = flash_sz;
    h.crc32_all   = 0;   // will backfill later

    if (f_write(&fp, &h, sizeof(h), &bw) != FR_OK || bw != sizeof(h)) {
        f_close(&fp);
        printf("Header write failed.\n");
        return -5;
    }

    uint8_t *buf = (uint8_t*)malloc(CHUNK_BYTES);
    if (!buf) {
        f_close(&fp);
        printf("OOM.\n");
        return -6;
    }

    uint32_t crc = 0, addr = 0, total = flash_sz;
    while (total) {
        uint32_t n = (total > CHUNK_BYTES) ? CHUNK_BYTES : total;
        if (!flash_dut_read(addr, buf, n)) {
            free(buf); f_close(&fp);
            printf("Flash read failed @0x%08x\n", addr);
            return -7;
        }
        crc = crc32_update(crc, buf, n);
        if (f_write(&fp, buf, n, &bw) != FR_OK || bw != n) {
            free(buf); f_close(&fp);
            printf("SD write failed.\n");
            return -8;
        }
        addr  += n;
        total -= n;

        if ((addr & 0xFFFF) == 0)
            printf("Backup %u / %u KiB\r", addr/1024, flash_sz/1024);
    }
    printf("\n");

    // write CRC trailer
    if (f_write(&fp, &crc, sizeof(crc), &bw) != FR_OK || bw != sizeof(crc)) {
        free(buf); f_close(&fp);
        printf("CRC write failed.\n");
        return -9;
    }

    // backfill header CRC at offset 0
    h.crc32_all = crc;
    f_lseek(&fp, 0);
    f_write(&fp, &h, sizeof(h), &bw);

    f_close(&fp);
    free(buf);
    printf("Backup OK: %s (size=%u, crc=0x%08x)\n", name, flash_sz, crc);
    return 0;
}

// choose the newest .fimg from /FLASHIMG
static int choose_latest_image(char *out, size_t n) {
    DIR d; FILINFO f;
    WORD best_date = 0, best_time = 0;
    char best_path[128] = {0};

    if (f_opendir(&d, DUMP_FOLDER) != FR_OK) return -1;

    while (f_readdir(&d, &f) == FR_OK && f.fname[0]) {
        if (!strstr(f.fname, ".fimg")) continue;

        bool newer = false;
        if (f.fdate || f.ftime) {
            if (f.fdate > best_date ||
               (f.fdate == best_date && f.ftime > best_time)) {
                newer = true;
            }
        } else {
            if (!best_path[0] || strcmp(f.fname, best_path) > 0) {
                newer = true;
            }
        }

        if (newer) {
            best_date = f.fdate;
            best_time = f.ftime;
            snprintf(best_path, sizeof(best_path),
                     "%s/%s", DUMP_FOLDER, f.fname);
        }
    }
    f_closedir(&d);

    if (!best_path[0]) return -1;
    snprintf(out, n, "%s", best_path);
    return 0;
}

// restore from .fimg and print final CRC(file) vs CRC(flash)
// name == NULL or "" → auto-pick latest
static int restore_flash_from_sd(const char *name) {
    if (!fs_mount_once()) {
        printf("SD mount failed.\n");
        return -1;
    }
    if (!flash_dut_init()) {
        printf("Flash init failed.\n");
        return -2;
    }

    char path[128];
    if (!name || !*name) {
        if (choose_latest_image(path, sizeof(path)) != 0) {
            printf("No .fimg found.\n");
            return -3;
        }
        name = path;
    }
    printf("Restoring from %s\n", name);

    FIL fp;
    UINT br = 0;
    if (f_open(&fp, name, FA_READ) != FR_OK) {
        printf("Open failed\n");
        return -4;
    }

    // ----- Read and validate header -----
    flashimg_hdr_t h = {0};
    if (f_read(&fp, &h, sizeof(h), &br) != FR_OK || br != sizeof(h) ||
        memcmp(h.magic, "FIMGv1\0", 8) != 0) {
        f_close(&fp);
        printf("Bad header.\n");
        return -5;
    }

    if (h.image_size == 0 || h.chunk_size == 0) {
        f_close(&fp);
        printf("Bad sizes in header: image_size=%u chunk_size=%u\n",
               h.image_size, h.chunk_size);
        return -6;
    }

    // ----- Recompute CRC over image data and compare with header & trailer -----
    uint32_t chunk_bytes = h.chunk_size;
    uint8_t *buf = (uint8_t*)malloc(chunk_bytes);
    if (!buf) {
        f_close(&fp);
        printf("OOM.\n");
        return -7;
    }

    uint32_t crc_calc   = 0;
    uint32_t remain     = h.image_size;
    uint32_t data_start = sizeof(h);

    f_lseek(&fp, data_start);

    while (remain) {
        uint32_t n = (remain > chunk_bytes) ? chunk_bytes : remain;
        if (f_read(&fp, buf, n, &br) != FR_OK || br != n) {
            free(buf);
            f_close(&fp);
            printf("Read fail while computing image CRC.\n");
            return -8;
        }
        crc_calc = crc32_update(crc_calc, buf, n);
        remain  -= n;
    }

    uint32_t crc_file_trailer = 0;
    if (f_read(&fp, &crc_file_trailer, sizeof(crc_file_trailer), &br) != FR_OK ||
        br != sizeof(crc_file_trailer)) {
        free(buf);
        f_close(&fp);
        printf("CRC trailer read fail.\n");
        return -9;
    }

    if (crc_calc != crc_file_trailer || crc_calc != h.crc32_all) {
        printf("CRC mismatch in image (header/trailer vs recompute)\n");
        printf("  header   : 0x%08x\n", h.crc32_all);
        printf("  trailer  : 0x%08x\n", crc_file_trailer);
        printf("  recompute: 0x%08x\n", crc_calc);
        free(buf);
        f_close(&fp);
        return -10;
    }
    printf("Image CRC OK: 0x%08x\n", crc_calc);

    // ----- Erase flash (up to flash_size from header) -----
    printf("Erasing sectors...\n");
    for (uint32_t a = 0; a < h.flash_size; a += FLASH_SECTOR_SIZE) {
        if (!flash_dut_erase_4k(a)) {
            free(buf);
            f_close(&fp);
            printf("Erase fail @0x%08x\n", a);
            return -11;
        }
        if ((a & 0xFFFF) == 0) {
            printf("Erased %u / %u KiB\r",
                   (a + FLASH_SECTOR_SIZE) / 1024, h.flash_size / 1024);
        }
    }
    printf("\nProgramming...\n");

    // ----- Program flash from image -----
    uint32_t addr   = 0;
    remain          = h.image_size;
    f_lseek(&fp, data_start);   // go back to start of data

    while (remain) {
        uint32_t n = (remain > chunk_bytes) ? chunk_bytes : remain;
        if (f_read(&fp, buf, n, &br) != FR_OK || br != n) {
            free(buf);
            f_close(&fp);
            printf("Read fail during programming.\n");
            return -12;
        }

        // Split into page-sized writes
        uint32_t off = 0;
        while (off < n) {
            uint32_t page_off = (addr + off) & (FLASH_PAGE_SIZE - 1);
            uint32_t room     = FLASH_PAGE_SIZE - page_off;
            uint32_t w        = (n - off > room) ? room : (n - off);

            if (!flash_dut_program_page(addr + off, buf + off, w)) {
                free(buf);
                f_close(&fp);
                printf("Prog fail @0x%08x\n", addr + off);
                return -13;
            }
            off += w;
        }

        addr   += n;
        remain -= n;

        if ((addr & 0xFFFF) == 0) {
            printf("Wrote %u / %u KiB\r", addr / 1024, h.flash_size / 1024);
        }
    }
    printf("\nProgramming done.\n");

    // ----- Final CRC over live flash -----
    uint32_t crc_flash = 0;
    int crc_rc = crc32_over_flash(h.image_size, h.chunk_size, &crc_flash);
    if (crc_rc != 0) {
        free(buf);
        f_close(&fp);
        printf("Final CRC over flash failed (rc=%d)\n", crc_rc);
        return -14;
    }

    printf("CRC(file)=0x%08x  CRC(flash)=0x%08x\n", h.crc32_all, crc_flash);

    free(buf);
    f_close(&fp);

    if (crc_flash != h.crc32_all) {
        printf("WARNING: CRC mismatch between file and flash.\n");
        return -15;
    }

    printf("Restore OK: flash matches image.\n");
    return 0;
}

// =====================================================
// ===============  CSV PARSING & MATCHING ==============
// =====================================================
//structure of chip entry (variables matches csv file)
typedef struct {
    char dev_name[32];
    uint8_t manf_id;
    uint8_t device_id[2];
    float read_time_us;      // us
    float write_time_ms;     // ms
    float write_time_ms_max; // ms
    float erase_time_ms;     // ms
    float erase_time_ms_max; // ms
} ChipEntry;
//Set max amount of chips to load from database to 1000 (Can change if needed)
#define MAX_CHIPS   1000 
#define MAX_MATCHES 10

static ChipEntry chip_data[MAX_CHIPS];
static int chip_count = 0;

/**
 * Parse one CSV line into ChipEntry. Reading from csv file and saving to local.
 * Returns 1 on success, 0 on failure (e.g. N/A fields, wrong format).
 */
static int parse_chip_line(const char *line, ChipEntry *chip) {
    memset(chip, 0, sizeof(*chip));

    int n = sscanf(line,
        "%31[^,],0x%hhx,0x%hhx,0x%hhx,%f,%f,%f,%f,%f",
        chip->dev_name,
        &chip->manf_id,
        &chip->device_id[0],
        &chip->device_id[1],
        &chip->read_time_us,
        &chip->write_time_ms,
        &chip->write_time_ms_max,
        &chip->erase_time_ms,
        &chip->erase_time_ms_max
    );

    if (n != 9) { // if there is more or less than 9 fields (more/less than intended) system will not save that line.
        return 0;
    }

    return 1;
}

// ---------- Matching & Scoring (lower score = better) ----------
static float rel2(float a, float b) {
    const float eps = 1e-6f;
    if (b == 0.0f) return 0.0f;
    float r = (a - b) / (fabsf(b) + eps);
    return r * r;
}

static float score_entry(const ChipEntry* db,
                         uint8_t obs_manf,
                         uint8_t obs_dev0,
                         uint8_t obs_dev1,
                         double  obs_read_us,
                         double  obs_prog_ms,
                         double  obs_erase_ms)
{
    const float W_ID_MATCH_BONUS   = -1.5f;
    const float W_ID_PARTIAL_BONUS = -0.6f;
    const float W_READ  = 1.0f;
    const float W_PROG  = 0.8f;
    const float W_ERASE = 0.6f;

    float s = 0.0f;

    if (db->manf_id == obs_manf &&
        db->device_id[0] == obs_dev0 &&
        db->device_id[1] == obs_dev1) {
        s += W_ID_MATCH_BONUS;
    } else if (db->manf_id == obs_manf) {
        s += W_ID_PARTIAL_BONUS;
    }

    const double db_read_us  = db->read_time_us;
    const double db_prog_ms  = db->write_time_ms;
    const double db_erase_ms = db->erase_time_ms;

    s += W_READ  * rel2((float)obs_read_us,  (float)db_read_us);
    s += W_PROG  * rel2((float)obs_prog_ms,  (float)db_prog_ms);
    s += W_ERASE * rel2((float)obs_erase_ms, (float)db_erase_ms);

    return s;
}

typedef struct {
    int index;
    float score;
} RankItem;

static void print_match_summary(const ChipEntry* db,
                                uint8_t manf, uint8_t dev0, uint8_t dev1,
                                double read_us, double prog_ms, double erase_ms,
                                const RankItem* r)
{
    const ChipEntry* best = &db[r->index];
    printf("\n=== Most likely chip ===\n");
    printf("Name: %s\n", best->dev_name);
    printf("DB JEDEC: 0x%02X 0x%02X 0x%02X\n",
           best->manf_id, best->device_id[0], best->device_id[1]);
    printf("Obs JEDEC:0x%02X 0x%02X 0x%02X\n", manf, dev0, dev1);
    printf("Score: %.4f (lower is better)\n", r->score);

    double db_read_us  = best->read_time_us;
    double db_prog_ms  = best->write_time_ms;
    double db_erase_ms = best->erase_time_ms;

    double rd_diff = (read_us  - db_read_us)  / (db_read_us  == 0 ? 1 : db_read_us)  * 100.0;
    double pr_diff = (prog_ms  - db_prog_ms)  / (db_prog_ms  == 0 ? 1 : db_prog_ms)  * 100.0;
    double er_diff = (erase_ms - db_erase_ms) / (db_erase_ms == 0 ? 1 : db_erase_ms) * 100.0;

    printf("Compare timings (obs vs DB):\n");
    printf("  READ : %.2f us vs %.2f us  (%+.1f%%)\n",
           read_us,  db_read_us,  rd_diff);
    printf("  PROG : %.2f ms vs %.2f ms  (%+.1f%%)\n",
           prog_ms,  db_prog_ms,  pr_diff);
    printf("  ERASE: %.2f ms vs %.2f ms  (%+.1f%%)\n",
           erase_ms, db_erase_ms, er_diff);
}

// ====================== BENCHMARK + CSV WORKFLOW ======================

static void run_main_workflow(uint8_t manf_id,
                              uint8_t mem_type,
                              uint8_t capacity_code,
                              int     topN)
{
    printf("\n--- Starting benchmark ---\n");
    const uint32_t target_addr = 0x000000;
    uint8_t page_buf[FLASH_PAGE_SIZE];
    for (int i = 0; i < FLASH_PAGE_SIZE; i++) page_buf[i] = i;

    #define ERASE_TRIALS 30
    #define PROG_TRIALS  30
    #define READ_TRIALS  100

    uint64_t start, end;
    double erase_total_us = 0, erase_min_us = 1e12, erase_max_us = 0;
    double prog_total_us  = 0, prog_min_us  = 1e12, prog_max_us  = 0;
    double read_total_us  = 0, read_min_us  = 1e12, read_max_us  = 0;

    // Erase trials
    for (int i = 0; i < ERASE_TRIALS; i++) {
        start = time_us_64();
        flash_dut_erase_4k(target_addr);
        end   = time_us_64();
        double elapsed = (double)(end - start);
        erase_total_us += elapsed;
        if (elapsed < erase_min_us) erase_min_us = elapsed;
        if (elapsed > erase_max_us) erase_max_us = elapsed;
    }

    // Program trials
    for (int i = 0; i < PROG_TRIALS; i++) {
        start = time_us_64();
        flash_dut_program_page(target_addr, page_buf, FLASH_PAGE_SIZE);
        end   = time_us_64();
        double elapsed = (double)(end - start);
        prog_total_us += elapsed;
        if (elapsed < prog_min_us) prog_min_us = elapsed;
        if (elapsed > prog_max_us) prog_max_us = elapsed;
    }

    // Read trials
    for (int i = 0; i < READ_TRIALS; i++) {
        start = time_us_64();
        flash_dut_read(target_addr, page_buf, FLASH_PAGE_SIZE);
        end   = time_us_64();
        double elapsed = (double)(end - start);
        read_total_us += elapsed;
        if (elapsed < read_min_us) read_min_us = elapsed;
        if (elapsed > read_max_us) read_max_us = elapsed;
    }

    double erase_avg_us = erase_total_us / ERASE_TRIALS;
    double prog_avg_us  = prog_total_us  / PROG_TRIALS;
    double read_avg_us  = read_total_us  / READ_TRIALS;

    double erase_min_ms = erase_min_us / 1000.0;
    double erase_max_ms = erase_max_us / 1000.0;
    double erase_avg_ms = erase_avg_us / 1000.0;

    double prog_min_ms  = prog_min_us  / 1000.0;
    double prog_max_ms  = prog_max_us  / 1000.0;
    double prog_avg_ms  = prog_avg_us  / 1000.0;

    printf("\n================ Benchmark Summary ================\n");
    printf("Operation       |    Min       |    Max       |    Avg\n");
    printf("--------------------------------------------------------\n");
    printf("Erase (ms) x%-3d |   %8.2f   |  %8.2f   |  %8.2f\n",
           ERASE_TRIALS, erase_min_ms, erase_max_ms, erase_avg_ms);
    printf("Program (ms) x%-3d |   %8.2f   |  %8.2f   |  %8.2f\n",
           PROG_TRIALS, prog_min_ms, prog_max_ms, prog_avg_ms);
    printf("Read (us) x%-3d  |   %8.2f   |  %8.2f   |  %8.2f\n",
           READ_TRIALS, read_min_us, read_max_us, read_avg_us);
    printf("========================================================\n");

    // --- Load CSV database from SD ---
    printf("\n--- Loading database from SD card ---\n");
    if (!fs_mount_once()) {
        printf("ERROR: SD card not mounted!\n");
        return;
    }

    FIL file_sd;
    FRESULT fr = f_open(&file_sd, "Embedded_datasheet.csv", FA_READ);
    if (fr != FR_OK) {
        printf("ERROR: Could not open Embedded_datasheet.csv (%d)\n", fr);
        return;
    }

    char line[128];
    #define BATCH_SIZE 25
    chip_count = 0;

    // Skip header
    f_gets(line, sizeof(line), &file_sd);

    while (true) {
        int batch_count = 0;

        while (f_gets(line, sizeof(line), &file_sd) &&
               batch_count < BATCH_SIZE &&
               chip_count + batch_count < MAX_CHIPS) {

            if (parse_chip_line(line, &chip_data[chip_count + batch_count])) {
                batch_count++;
            } else {
                printf("Skipped bad CSV line: %s", line);
            }
        }

        if (batch_count == 0) break;
        printf("\n%d entries loaded\n", batch_count); //Print every batch
        chip_count += batch_count;
    }

    printf("\nTotal entries loaded into local memory: %d\n", chip_count); //Print total chips loaded

    printf("\n--- First 5 entries in local ---\n"); //Prints first 5 entries for user to check
    for (int i = 0; i < 5 && i < chip_count; i++) {
        ChipEntry *c = &chip_data[i];

        printf("Row %d:\n", i + 1);
        printf("Name: %s\n", c->dev_name);
        printf("ManfID: 0x%02X\n", c->manf_id);
        printf("DeviceID: 0x%02X 0x%02X\n",
               c->device_id[0], c->device_id[1]);
        printf("Read_typ : %.2f us\n",  c->read_time_us);
        printf("Write(tpp): %.2f ms\n", c->write_time_ms);
        printf("Write(max): %.2f ms\n", c->write_time_ms_max);
        printf("Erase(tSE): %.2f ms\n", c->erase_time_ms);
        printf("Erase(max): %.2f ms\n\n", c->erase_time_ms_max);
    }

    f_close(&file_sd);
    printf("\nIntegration complete.\n");

    // --- Chip Identification: TOP N matches ---
    if (chip_count > 0) {
        if (topN < 1) topN = 1;
        if (topN > MAX_MATCHES) topN = MAX_MATCHES;

        const uint8_t obs_manf = manf_id;
        const uint8_t obs_dev0 = mem_type;
        const uint8_t obs_dev1 = capacity_code;

        const double obs_read_us  = read_avg_us;
        const double obs_prog_ms  = prog_avg_ms;
        const double obs_erase_ms = erase_avg_ms;

        RankItem best[MAX_MATCHES];
        for (int i = 0; i < MAX_MATCHES; i++) {
            best[i].index = -1;
            best[i].score = INFINITY;
        }

        for (int i = 0; i < chip_count; i++) {
            const ChipEntry *c = &chip_data[i];

            if (c->read_time_us <= 0.0f ||
                c->write_time_ms <= 0.0f ||
                c->erase_time_ms <= 0.0f) {
                continue;
            }

            float sc = score_entry(c,
                                   obs_manf, obs_dev0, obs_dev1,
                                   obs_read_us, obs_prog_ms, obs_erase_ms);

            for (int k = 0; k < topN; k++) {
                if (sc < best[k].score) {
                    for (int m = topN - 1; m > k; m--) {
                        best[m] = best[m - 1];
                    }
                    best[k].index = i;
                    best[k].score = sc;
                    break;
                }
            }
        }

        printf("\n================= TOP %d MATCHES FROM CSV =================\n", topN);
        printf("Observed JEDEC: 0x%02X 0x%02X 0x%02X\n",
               obs_manf, obs_dev0, obs_dev1);
        printf("Observed timings: READ=%.2f us, PROG=%.2f ms, ERASE=%.2f ms\n",
               obs_read_us, obs_prog_ms, obs_erase_ms);
        printf("==========================================================\n");

        for (int k = 0; k < topN; k++) {
            if (best[k].index < 0) continue;

            ChipEntry *c = &chip_data[best[k].index];

            double db_read_us  = c->read_time_us;
            double db_prog_ms  = c->write_time_ms;
            double db_erase_ms = c->erase_time_ms;

            double rd_diff = (obs_read_us  - db_read_us)  /
                             (db_read_us  == 0 ? 1 : db_read_us)  * 100.0;
            double pr_diff = (obs_prog_ms  - db_prog_ms)  /
                             (db_prog_ms  == 0 ? 1 : db_prog_ms)  * 100.0;
            double er_diff = (obs_erase_ms - db_erase_ms) /
                             (db_erase_ms == 0 ? 1 : db_erase_ms) * 100.0;

            printf("\n[#%d] DB Row %d: %s\n",
                   k + 1, best[k].index + 1, c->dev_name);
            printf("  JEDEC (DB):   0x%02X 0x%02X 0x%02X\n",
                   c->manf_id, c->device_id[0], c->device_id[1]);
            printf("  Score:        %.4f (lower is better)\n", best[k].score);

            printf("  DB timings:\n");
            printf("    READ_typ : %.2f us\n",  c->read_time_us);
            printf("    PROG_typ : %.2f ms\n",  c->write_time_ms);
            printf("    PROG_max : %.2f ms\n",  c->write_time_ms_max);
            printf("    ERASE_typ: %.2f ms\n",  c->erase_time_ms);
            printf("    ERASE_max: %.2f ms\n",  c->erase_time_ms_max);

            printf("  Compare vs observed:\n");
            printf("    READ  DB: %8.2f us | OBS: %8.2f us (%+6.1f%%)\n",
                   db_read_us,  obs_read_us,  rd_diff);
            printf("    PROG  DB: %8.2f ms | OBS: %8.2f ms (%+6.1f%%)\n",
                   db_prog_ms,  obs_prog_ms,  pr_diff);
            printf("    ERASE DB: %8.2f ms | OBS: %8.2f ms (%+6.1f%%)\n",
                   db_erase_ms, obs_erase_ms, er_diff);
        }

        if (best[0].index >= 0) {
            print_match_summary(chip_data,
                                obs_manf, obs_dev0, obs_dev1,
                                obs_read_us, obs_prog_ms, obs_erase_ms,
                                &best[0]);
        }
    }

    printf("\nProcess complete.\n");
}

// =====================================================
// ===============  INPUT HELPER ========================
// =====================================================

// Simple blocking line input from USB serial:
// - Stops on Enter (\r or \n)
// - OR if there's been no new characters for ~500 ms after typing starts
// - Handles backspace
static void read_line_blocking(char *buf, size_t buf_len) {
    size_t   pos     = 0;
    bool     got_any = false;
    uint64_t last_us = time_us_64();

    while (1) {
        int c = getchar_timeout_us(100000);   // wait up to 100 ms

        if (c == PICO_ERROR_TIMEOUT) {
            if (got_any && (time_us_64() - last_us) > 500000) { // 500 ms idle
                putchar('\n');
                break;
            }
            continue;
        }

        last_us = time_us_64();
        got_any = true;

        if (c == '\r' || c == '\n') {
            putchar('\n');
            break;
        }

        // Backspace / delete
        if (c == 8 || c == 127) {
            if (pos > 0) {
                pos--;
                putchar('\b');
                putchar(' ');
                putchar('\b');
            }
            continue;
        }

        // Printable ASCII
        if (c >= 32 && c < 127) {
            if (pos < buf_len - 1) {
                buf[pos++] = (char)c;
                putchar(c);  // echo
            }
        }
    }

    buf[pos] = '\0';
}

// =====================================================
// ===============  MAIN + MENU =========================
// =====================================================

int main(void) {
    stdio_init_all();
    sleep_ms(2000);

    printf("\n=== SPI Flash Forensic + FIMG Backup Tool ===\n");

    // Init flash & read JEDEC
    flash_dut_init();

    jedec_info_t id;
    if (!flash_dut_read_jedec(&id)) {
        printf("ERROR: flash_dut_read_jedec() failed\n");
    }

    uint8_t manf_id       = id.manuf_id;
    uint8_t mem_type      = id.mem_type;
    uint8_t capacity_code = id.capacity_id;

    uint32_t capacity_bytes = 0;
    uint32_t sfdp_bytes     = 0;
    if (flash_dut_probe_capacity_sfdp(&sfdp_bytes) && sfdp_bytes) {
        capacity_bytes = sfdp_bytes;
    } else {
        capacity_bytes = flash_calculate_capacity(capacity_code);
    }
    if (capacity_bytes == 0) {
        capacity_bytes = 16u * 1024u * 1024u; // fallback
    }

    printf("\n--- Flash Chip Info ---\n");
    printf("Manufacturer ID: 0x%02X\n", manf_id);
    if (mem_type == 0x00 || mem_type == 0xFF)
        printf("Memory Type:     Unknown / Internal Flash\n");
    else
        printf("Memory Type:     0x%02X\n", mem_type);
    printf("Capacity Code:   0x%02X\n", capacity_code);
    printf("Approx Capacity: %.2f MB\n",
           capacity_bytes / (1024.0 * 1024.0));

    while (true) {
        printf("\n=== MAIN MENU ===\n");
        printf("  1 = Run benchmark + CSV + identification\n");
        printf("  2 = Backup SPI flash to SD  (/FLASHIMG/*.fimg)\n");
        printf("  3 = Restore SPI flash from SD (latest .fimg)\n");
        printf("  4 = Restore SPI flash from SD (choose specific file)\n");
        printf("  5 = List available flash images (.fimg)\n");
        printf("  q = Quit (idle loop)\n");
        printf("=================\n");
        printf("Select option: ");

        int ch = getchar_timeout_us(0);
        while (ch == PICO_ERROR_TIMEOUT) {
            tight_loop_contents();
            ch = getchar_timeout_us(0);
        }
        printf("%c\n", ch);   // echo

        switch (ch) {
        case '1': {
            int topN = 3;
            printf("\n[CSV MATCH] How many top matches to display? (1-10): ");
            char line[8];
            read_line_blocking(line, sizeof(line));
            if (line[0] != '\0') {
                topN = atoi(line);
            }
            if (topN < 1)           topN = 1;
            if (topN > MAX_MATCHES) topN = MAX_MATCHES;

            run_main_workflow(manf_id, mem_type, capacity_code, topN);
            break;
        }

        case '2':
            backup_flash_to_sd();
            break;

        case '3':
            // latest image
            restore_flash_from_sd(NULL);
            break;

        case '4': {
            // choose specific file
            char input[128];
            char path[160];

            printf("\n[RESTORE] Existing images:\n");
            list_flash_images();
            printf("\n[RESTORE] Enter image path or name inside %s\n", DUMP_FOLDER);
            printf("          e.g. FLASHIMG/xxx.fimg or just xxx.fimg\n");
            printf("Filename: ");
            read_line_blocking(input, sizeof(input));

            if (input[0] == '\0') {
                printf("[RESTORE] No filename entered, cancelled.\n");
                break;
            }

            // If user only typed a bare filename, prepend DUMP_FOLDER/
            if (strchr(input, '/') == NULL && strchr(input, '\\') == NULL) {
                snprintf(path, sizeof(path), "%s/%s", DUMP_FOLDER, input);
            } else {
                snprintf(path, sizeof(path), "%s", input);
            }

            printf("[RESTORE] Using image: %s\n", path);
            restore_flash_from_sd(path);
            break;
        }

        case '5':
            list_flash_images();
            break;

        case 'q':
        case 'Q':
            printf("[MENU] Entering idle mode. Press 'm' to return to main menu.\n");
            while (1) {
                tight_loop_contents();
                int ch2 = getchar_timeout_us(0);
                if (ch2 == 'm' || ch2 == 'M') {
                    printf("[MENU] Returning to main menu...\n");
                    break;
                }
            }
            break;

        default:
            printf("[MENU] Unknown option '%c'. Please choose 1–5 or q.\n", ch);
            break;
        }
    }

    return 0;   // never reached
}
