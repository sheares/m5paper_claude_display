#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/spi_master.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "it8951.h"

#define TAG "it8951"

// Pin definitions (M5Paper)
#define EPD_MOSI  12
#define EPD_MISO  13
#define EPD_SCK   14
#define EPD_CS    15
#define EPD_BUSY  27   // HRDY: HIGH = ready, LOW = busy
#define EPD_PWR   23   // EPD power enable (M5EPD_EPD_PWR_EN_PIN), active HIGH
#define EXT_PWR   5    // External peripherals power enable (M5EPD_EXT_PWR_EN_PIN), active HIGH
#define SD_CS     4    // SD card CS — shares SPI bus with IT8951; must be held HIGH

// SPI preamble codes
#define PRE_CMD   0x6000
#define PRE_WR    0x0000
#define PRE_RD    0x1000

// IT8951 commands (from IT8951_Defines.h)
#define CMD_SYS_RUN       0x0001
#define CMD_SLEEP         0x0003
#define CMD_REG_RD        0x0010
#define CMD_REG_WR        0x0011
#define CMD_LD_IMG        0x0020
#define CMD_LD_IMG_AREA   0x0021
#define CMD_LD_IMG_END    0x0022
#define CMD_DPY_AREA      0x0034   // simple refresh (5 args)
#define CMD_DPY_BUF_AREA  0x0037   // refresh with explicit buf addr (7 args)
#define CMD_GET_DEV_INFO  0x0302
#define CMD_VCOM          0x0039

// Register addresses
#define REG_I80CPCR   0x0004   // pack pixel control
#define REG_LISAR     0x0208   // load image start addr (low)
#define REG_LISAR_H   0x020A   // load image start addr (high)
#define REG_LUTAFSR   0x1224   // LUT engine status (0 = all idle)

// Pixel packing: 4bpp big-endian, 4 pixels per 16-bit word
#define LDIMG_BPP_4   2        // IT8951_4BPP
#define LDIMG_ROT_0   0
#define LDIMG_B_END   1        // big-endian byte order (matches our SPI output)

static spi_device_handle_t s_spi;

// ── CS control (manual GPIO) ──────────────────────────────────────────────────

static inline void cs_low(void)  { gpio_set_level(EPD_CS, 0); }
static inline void cs_high(void) { gpio_set_level(EPD_CS, 1); }

// ── low-level SPI ────────────────────────────────────────────────────────────

static inline void wait_ready(void)
{
    // HRDY: HIGH = ready, LOW = busy — wait until HIGH
    int tries = 0;
    while (!gpio_get_level(EPD_BUSY) && tries++ < 5000) {
        vTaskDelay(1);
    }
}

static void spi_write16(uint16_t word)
{
    spi_transaction_t t = {
        .length = 16,
        .flags  = SPI_TRANS_USE_TXDATA,
    };
    t.tx_data[0] = (word >> 8) & 0xFF;
    t.tx_data[1] =  word       & 0xFF;
    spi_device_polling_transmit(s_spi, &t);
}

// Send 32 bits as one atomic transaction (matches M5EPD's write32)
static void spi_write32(uint16_t hi, uint16_t lo)
{
    spi_transaction_t t = {
        .length = 32,
        .flags  = SPI_TRANS_USE_TXDATA,
    };
    t.tx_data[0] = (hi >> 8) & 0xFF;
    t.tx_data[1] =  hi       & 0xFF;
    t.tx_data[2] = (lo >> 8) & 0xFF;
    t.tx_data[3] =  lo       & 0xFF;
    spi_device_polling_transmit(s_spi, &t);
}

static uint16_t spi_read16(void)
{
    spi_transaction_t t = {
        .length    = 16,
        .rxlength  = 16,
        .flags     = SPI_TRANS_USE_RXDATA | SPI_TRANS_USE_TXDATA,
    };
    t.tx_data[0] = 0;
    t.tx_data[1] = 0;
    spi_device_polling_transmit(s_spi, &t);
    return ((uint16_t)t.rx_data[0] << 8) | t.rx_data[1];
}

// IT8951 write command: CS LOW → PRE_CMD → wait → cmd → CS HIGH
// CS stays asserted across preamble+data (matching M5EPD WriteCommand)
static void write_cmd(uint16_t cmd)
{
    wait_ready();
    cs_low();
    spi_write16(PRE_CMD);
    wait_ready();
    spi_write16(cmd);
    cs_high();
}

// IT8951 write single data word: CS LOW → PRE_WR → wait → data → CS HIGH
static void write_data(uint16_t data)
{
    wait_ready();
    cs_low();
    spi_write16(PRE_WR);
    wait_ready();
    spi_write16(data);
    cs_high();
}

// IT8951 read single data word: CS LOW → PRE_RD → wait → dummy → data → CS HIGH
static uint16_t read_data(void)
{
    wait_ready();
    cs_low();
    spi_write16(PRE_RD);
    wait_ready();
    spi_read16();           // dummy word
    uint16_t val = spi_read16();
    cs_high();
    return val;
}

static void write_reg(uint16_t addr, uint16_t val)
{
    write_cmd(CMD_REG_WR);
    write_data(addr);
    write_data(val);
}

// Wait until LUTAFSR = 0 (all LUT engines idle) — called before DPY command
static void wait_display_ready(void)
{
    for (int i = 0; i < 300; i++) {
        write_cmd(CMD_REG_RD);
        write_data(REG_LUTAFSR);
        uint16_t status = read_data();
        if (status == 0) return;
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    ESP_LOGW(TAG, "wait_display_ready timeout");
}

// ── bulk pixel transfer ──────────────────────────────────────────────────────

// Convert 1bpp framebuffer → stream 4bpp packed words to IT8951.
// Matches M5EPD WritePartGram4bpp: each word is sent as PRE_WR(16)+data(16)
// in a single 32-bit atomic SPI transfer with CS toggled per word.
static void stream_pixels(const uint8_t *buf_1bpp, uint32_t npixels)
{
    // 4bpp: each 16-bit word = 4 pixels, [p0(15:12) p1(11:8) p2(7:4) p3(3:0)]
    // 1bpp input: 1=white, 0=black  →  4bpp: 0xF=white, 0x0=black
    const uint32_t n_words = npixels / 4;
    for (uint32_t w = 0; w < n_words; w++) {
        uint32_t bit_idx = w * 4;
        uint8_t  raw[4];
        for (int i = 0; i < 4; i++) {
            uint32_t bi = bit_idx + i;
            raw[i] = (buf_1bpp[bi / 8] >> (7 - (bi & 7))) & 1;
        }
        uint16_t data = ((raw[0] ? 0xF : 0x0) << 12) |
                        ((raw[1] ? 0xF : 0x0) <<  8) |
                        ((raw[2] ? 0xF : 0x0) <<  4) |
                         (raw[3] ? 0xF : 0x0);

        // M5EPD sends PRE_WR(0x0000) + data as one atomic 32-bit transfer
        cs_low();
        spi_write32(PRE_WR, data);
        cs_high();
    }
}

// ── public API ───────────────────────────────────────────────────────────────

esp_err_t it8951_init(it8951_info_t *info)
{
    // GPIO 4 (SD card CS) shares SPI bus — deassert FIRST to prevent bus contention.
    // GPIO 5 (EXT_PWR_EN) and GPIO 23 (EPD_PWR_EN) both required to power IT8951.
    gpio_config_t io = {
        .pin_bit_mask = (1ULL << EXT_PWR) | (1ULL << EPD_PWR) | (1ULL << SD_CS),
        .mode         = GPIO_MODE_OUTPUT,
    };
    gpio_config(&io);
    gpio_set_level(SD_CS, 1);   // deassert SD card before anything else
    gpio_set_level(EXT_PWR, 1);
    gpio_set_level(EPD_PWR, 1);

    // BUSY / HRDY input
    io.pin_bit_mask = (1ULL << EPD_BUSY);
    io.mode         = GPIO_MODE_INPUT;
    io.pull_up_en   = GPIO_PULLUP_ENABLE;
    gpio_config(&io);

    // CS as manual GPIO output, initially deasserted (HIGH)
    io.pin_bit_mask  = (1ULL << EPD_CS);
    io.mode          = GPIO_MODE_OUTPUT;
    io.pull_up_en    = GPIO_PULLUP_DISABLE;
    gpio_config(&io);
    gpio_set_level(EPD_CS, 1);

    vTaskDelay(pdMS_TO_TICKS(1000));  // IT8951 needs ~1000ms after power-on

    // SPI3_HOST (VSPI) — same host used by M5EPD Arduino library
    spi_bus_config_t bus = {
        .mosi_io_num     = EPD_MOSI,
        .miso_io_num     = EPD_MISO,
        .sclk_io_num     = EPD_SCK,
        .quadwp_io_num   = -1,
        .quadhd_io_num   = -1,
        .max_transfer_sz = 4096,
    };
    ESP_ERROR_CHECK(spi_bus_initialize(SPI3_HOST, &bus, SPI_DMA_CH_AUTO));

    // spics_io_num = -1: CS managed manually via GPIO
    spi_device_interface_config_t dev = {
        .clock_speed_hz = 10000000,   // 10 MHz, matching M5EPD
        .mode           = 0,
        .spics_io_num   = -1,
        .queue_size     = 1,
    };
    ESP_ERROR_CHECK(spi_bus_add_device(SPI3_HOST, &dev, &s_spi));

    vTaskDelay(pdMS_TO_TICKS(10));
    wait_ready();

    // M5EPD library hardcodes these (GET_DEV_INFO commented out in M5EPD_Driver.cpp)
    info->width        = 960;
    info->height       = 540;
    info->img_buf_addr = 0x001236E0UL;

    // SYS_RUN must be first command (matching M5EPD begin())
    write_cmd(CMD_SYS_RUN);

    // Enable pack-pixel mode (4px per 16-bit word)
    write_reg(REG_I80CPCR, 0x0001);

    // Set VCOM to -2.30V (M5Paper panel spec, matching M5EPD)
    write_cmd(CMD_VCOM);
    write_data(0x0001);   // sub-command: write
    write_data(2300);     // |VCOM| in mV

    ESP_LOGI(TAG, "panel %u×%u  buf=0x%08" PRIx32,
             info->width, info->height, info->img_buf_addr);

    return ESP_OK;
}

void it8951_display(const it8951_info_t *info, const uint8_t *buf, uint8_t mode)
{
    ESP_LOGI(TAG, "display start mode=%u", mode);

    // Set target image buffer address in LISAR (matching M5EPD SetTargetMemoryAddr)
    write_reg(REG_LISAR_H, (uint16_t)(info->img_buf_addr >> 16));
    write_reg(REG_LISAR,   (uint16_t)(info->img_buf_addr & 0xFFFF));

    // LD_IMG_AREA: announce image load region + pixel format
    write_cmd(CMD_LD_IMG_AREA);
    write_data((LDIMG_B_END << 8) | (LDIMG_BPP_4 << 4) | LDIMG_ROT_0);
    write_data(0);              // x
    write_data(0);              // y
    write_data(info->width);
    write_data(info->height);

    ESP_LOGI(TAG, "streaming %lu pixels...", (unsigned long)(info->width * info->height));
    stream_pixels(buf, (uint32_t)info->width * info->height);
    ESP_LOGI(TAG, "pixels done, sending LD_IMG_END");

    write_cmd(CMD_LD_IMG_END);

    // Wait for LUT engines to be idle before triggering refresh
    wait_display_ready();

    // CMD_DPY_BUF_AREA (0x0037): refresh with explicit buffer address (7 args)
    // Matches M5EPD UpdateArea → WriteArgs(IT8951_I80_CMD_DPY_BUF_AREA, args, 7)
    ESP_LOGI(TAG, "sending DPY_BUF_AREA...");
    write_cmd(CMD_DPY_BUF_AREA);
    write_data(0);                                          // x
    write_data(0);                                          // y
    write_data(info->width);
    write_data(info->height);
    write_data(mode);
    write_data((uint16_t)(info->img_buf_addr & 0xFFFF));    // addr low
    write_data((uint16_t)(info->img_buf_addr >> 16));       // addr high

    // Wait until the LUT engines finish — exits as soon as the waveform
    // completes, instead of a fixed worst-case sleep.
    wait_display_ready();
    wait_ready();
    ESP_LOGI(TAG, "display done");
}

void it8951_display_area(const it8951_info_t *info, const uint8_t *fb,
                         uint16_t x, uint16_t y,
                         uint16_t w, uint16_t h, uint8_t mode)
{
    // Align x down and w up to multiples of 4 (4bpp word boundary)
    uint16_t ax = x & ~0x3;
    uint16_t end = (uint16_t)(x + w);
    if (end > info->width) end = info->width;
    uint16_t aw = (end - ax + 3) & ~0x3;
    if (ax + aw > info->width) aw = info->width - ax;
    uint16_t ay = y;
    uint16_t ah = h;

    write_reg(REG_LISAR_H, (uint16_t)(info->img_buf_addr >> 16));
    write_reg(REG_LISAR,   (uint16_t)(info->img_buf_addr & 0xFFFF));

    write_cmd(CMD_LD_IMG_AREA);
    write_data((LDIMG_B_END << 8) | (LDIMG_BPP_4 << 4) | LDIMG_ROT_0);
    write_data(ax);
    write_data(ay);
    write_data(aw);
    write_data(ah);

    // Per-word CS toggle — IT8951 SPI mode treats each CS-low pulse as a
    // single (PRE_WR, data) transaction. Earlier burst attempts broke the
    // chip's state machine because it expects CS edges between words.
    const int stride = info->width / 8;
    for (uint16_t row = ay; row < ay + ah; row++) {
        for (uint16_t col = ax; col < ax + aw; col += 4) {
            uint16_t data = 0;
            for (int i = 0; i < 4; i++) {
                uint16_t c = col + i;
                int bit = (fb[row * stride + c / 8] >> (7 - (c & 7))) & 1;
                uint16_t nib = bit ? 0xF : 0x0;
                data |= nib << (12 - i * 4);
            }
            cs_low();
            spi_write32(PRE_WR, data);
            cs_high();
        }
    }

    write_cmd(CMD_LD_IMG_END);
    wait_display_ready();

    write_cmd(CMD_DPY_BUF_AREA);
    write_data(ax);
    write_data(ay);
    write_data(aw);
    write_data(ah);
    write_data(mode);
    write_data((uint16_t)(info->img_buf_addr & 0xFFFF));
    write_data((uint16_t)(info->img_buf_addr >> 16));

    wait_display_ready();
    wait_ready();
}

void it8951_clear(const it8951_info_t *info)
{
    const uint32_t nbytes = (PANEL_W / 8) * PANEL_H;
    uint8_t *blank = heap_caps_malloc(nbytes, MALLOC_CAP_DMA);
    if (!blank) {
        ESP_LOGE(TAG, "OOM for clear buffer");
        return;
    }
    memset(blank, 0xFF, nbytes);   // all white
    it8951_display(info, blank, IT8951_UPD_INIT);
    free(blank);
}
