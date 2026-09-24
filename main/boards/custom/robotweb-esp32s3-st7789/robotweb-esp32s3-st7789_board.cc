// Robotweb ESP32-S3 ST7789 Board
// ESP32-S3 N16R8 + ST7789 240x320 + ES8311 + Motor DC L298N
#include <esp_lcd_panel_vendor.h>
#include <esp_lcd_panel_io.h>
#include <esp_lcd_panel_ops.h>

#include <wifi_station.h>
#include "wifi_board.h"
#include "codecs/es8311_audio_codec.h"
#include "display/lcd_display.h"
#include "application.h"
#include "button.h"
#include "config.h"
#include "mcp_server.h"

#include <esp_log.h>
#include <driver/gpio.h>
#include <driver/ledc.h>
#include <driver/i2c_master.h>
#include <driver/spi_common.h>

#include "led/single_led.h"
#include "system_reset.h"

#define TAG "RobowebBoard"

// ===== Motor DC helper functions =====
static void motor_gpio_init() {
    uint64_t mask = (1ULL<<(int)MOTOR_IN1)|(1ULL<<(int)MOTOR_IN2)|
                    (1ULL<<(int)MOTOR_IN3)|(1ULL<<(int)MOTOR_IN4);
    gpio_config_t c = {.pin_bit_mask=mask, .mode=GPIO_MODE_OUTPUT,
        .pull_up_en=GPIO_PULLUP_DISABLE, .pull_down_en=GPIO_PULLDOWN_DISABLE,
        .intr_type=GPIO_INTR_DISABLE};
    gpio_config(&c);
    ledc_timer_config_t t = {.speed_mode=LEDC_LOW_SPEED_MODE,
        .duty_resolution=LEDC_TIMER_8_BIT, .timer_num=LEDC_TIMER_0,
        .freq_hz=1000, .clk_cfg=LEDC_AUTO_CLK};
    ledc_timer_config(&t);
    ledc_channel_config_t ca = {.gpio_num=(int)MOTOR_ENA, .speed_mode=LEDC_LOW_SPEED_MODE,
        .channel=LEDC_CHANNEL_0, .timer_sel=LEDC_TIMER_0, .duty=0, .hpoint=0};
    ledc_channel_config(&ca);
    ledc_channel_config_t cb = {.gpio_num=(int)MOTOR_ENB, .speed_mode=LEDC_LOW_SPEED_MODE,
        .channel=LEDC_CHANNEL_1, .timer_sel=LEDC_TIMER_0, .duty=0, .hpoint=0};
    ledc_channel_config(&cb);
    gpio_set_level(MOTOR_IN1,0); gpio_set_level(MOTOR_IN2,0);
    gpio_set_level(MOTOR_IN3,0); gpio_set_level(MOTOR_IN4,0);
    ESP_LOGI(TAG, "Motor DC OK (IN1=%d IN2=%d IN3=%d IN4=%d ENA=%d ENB=%d)",
        (int)MOTOR_IN1,(int)MOTOR_IN2,(int)MOTOR_IN3,(int)MOTOR_IN4,(int)MOTOR_ENA,(int)MOTOR_ENB);
}

static void motor_speed(uint8_t a, uint8_t b) {
    ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, a);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0);
    ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_1, b);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_1);
}
static void motor_forward(int s)  { gpio_set_level(MOTOR_IN1,1); gpio_set_level(MOTOR_IN2,0); gpio_set_level(MOTOR_IN3,1); gpio_set_level(MOTOR_IN4,0); motor_speed(s,s); ESP_LOGI(TAG,"Fwd %d",s); }
static void motor_backward(int s) { gpio_set_level(MOTOR_IN1,0); gpio_set_level(MOTOR_IN2,1); gpio_set_level(MOTOR_IN3,0); gpio_set_level(MOTOR_IN4,1); motor_speed(s,s); ESP_LOGI(TAG,"Bwd %d",s); }
static void motor_left(int s)     { gpio_set_level(MOTOR_IN1,0); gpio_set_level(MOTOR_IN2,1); gpio_set_level(MOTOR_IN3,1); gpio_set_level(MOTOR_IN4,0); motor_speed(s,s); }
static void motor_right(int s)    { gpio_set_level(MOTOR_IN1,1); gpio_set_level(MOTOR_IN2,0); gpio_set_level(MOTOR_IN3,0); gpio_set_level(MOTOR_IN4,1); motor_speed(s,s); }
static void motor_stop()          { gpio_set_level(MOTOR_IN1,0); gpio_set_level(MOTOR_IN2,0); gpio_set_level(MOTOR_IN3,0); gpio_set_level(MOTOR_IN4,0); motor_speed(0,0); ESP_LOGI(TAG,"Stop"); }

// ===== Board class =====
class RobowebESP32S3ST7789 : public WifiBoard {
private:
    Button boot_button_;
    LcdDisplay* display_;
    i2c_master_bus_handle_t i2c_bus_;

    void InitializeI2c() {
        i2c_master_bus_config_t cfg = {
            .i2c_port = AUDIO_CODEC_I2C_NUM,
            .sda_io_num = AUDIO_CODEC_I2C_SDA_PIN,
            .scl_io_num = AUDIO_CODEC_I2C_SCL_PIN,
            .clk_source = I2C_CLK_SRC_DEFAULT,
            .glitch_ignore_cnt = 7,
            .intr_priority = 0,
            .trans_queue_depth = 0,
            .flags = { .enable_internal_pullup = 1 },
        };
        ESP_ERROR_CHECK(i2c_new_master_bus(&cfg, &i2c_bus_));
    }

    void InitializeSpi() {
        spi_bus_config_t buscfg = {};
        buscfg.mosi_io_num = DISPLAY_MOSI_PIN;
        buscfg.miso_io_num = GPIO_NUM_NC;
        buscfg.sclk_io_num = DISPLAY_SCK_PIN;
        buscfg.quadwp_io_num = GPIO_NUM_NC;
        buscfg.quadhd_io_num = GPIO_NUM_NC;
        buscfg.max_transfer_sz = DISPLAY_WIDTH * DISPLAY_HEIGHT * sizeof(uint16_t);
        ESP_ERROR_CHECK(spi_bus_initialize(LCD_SPI_HOST, &buscfg, SPI_DMA_CH_AUTO));
    }

    void InitializeLcdDisplay() {
        esp_lcd_panel_io_handle_t panel_io = nullptr;
        esp_lcd_panel_handle_t panel = nullptr;
        esp_lcd_panel_io_spi_config_t io_cfg = {};
        io_cfg.cs_gpio_num = DISPLAY_CS_PIN;
        io_cfg.dc_gpio_num = DISPLAY_DC_PIN;
        io_cfg.spi_mode = DISPLAY_SPI_MODE;
        io_cfg.pclk_hz = DISPLAY_SPI_SCLK_HZ;
        io_cfg.trans_queue_depth = 10;
        io_cfg.lcd_cmd_bits = 8;
        io_cfg.lcd_param_bits = 8;
        ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi(LCD_SPI_HOST, &io_cfg, &panel_io));
        esp_lcd_panel_dev_config_t panel_cfg = {};
        panel_cfg.reset_gpio_num = DISPLAY_RST_PIN;
        panel_cfg.rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB;
        panel_cfg.bits_per_pixel = 16;
        ESP_ERROR_CHECK(esp_lcd_new_panel_st7789(panel_io, &panel_cfg, &panel));
        esp_lcd_panel_reset(panel);
        esp_lcd_panel_init(panel);
        esp_lcd_panel_invert_color(panel, DISPLAY_INVERT_COLOR);
        esp_lcd_panel_swap_xy(panel, DISPLAY_SWAP_XY);
        esp_lcd_panel_mirror(panel, DISPLAY_MIRROR_X, DISPLAY_MIRROR_Y);
        display_ = new SpiLcdDisplay(panel_io, panel,
            DISPLAY_WIDTH, DISPLAY_HEIGHT, DISPLAY_OFFSET_X, DISPLAY_OFFSET_Y,
            DISPLAY_MIRROR_X, DISPLAY_MIRROR_Y, DISPLAY_SWAP_XY);
    }

    void InitializeButtons() {
        boot_button_.OnClick([this]() {
            auto& app = Application::GetInstance();
            if (app.GetDeviceState() == kDeviceStateStarting) {
                EnterWifiConfigMode();
            }
            app.ToggleChatState();
        });
    }

    void InitializeTools() {
        motor_gpio_init();
        auto& mcp = McpServer::GetInstance();

        // Tool: motor move — use explicit std::vector<Property> for GCC 15.2.0 compat
        std::vector<Property> move_props = {
            Property("direction", kPropertyTypeString),
            Property("speed", kPropertyTypeInteger, 200)
        };
        mcp.AddTool("self.motor.move",
            "Control robot movement. direction: forward/backward/left/right/stop. speed: 0-255 (default 200).",
            PropertyList(move_props),
            [](const PropertyList& props) -> ReturnValue {
                std::string dir = props["direction"].value<std::string>();
                int spd = props["speed"].value<int>();
                if (spd < 0) spd = 0;
                if (spd > 255) spd = 255;
                if      (dir == "forward")  motor_forward(spd);
                else if (dir == "backward") motor_backward(spd);
                else if (dir == "left")     motor_left(spd);
                else if (dir == "right")    motor_right(spd);
                else                        motor_stop();
                return std::string("motor:") + dir;
            }
        );

        // Tool: stop
        mcp.AddTool("self.motor.stop",
            "Stop all motors immediately.",
            PropertyList(),
            [](const PropertyList&) -> ReturnValue {
                motor_stop();
                return std::string("stopped");
            }
        );
    }

public:
    RobowebESP32S3ST7789() : boot_button_(BOOT_BUTTON_GPIO) {
        InitializeI2c();
        InitializeSpi();
        InitializeLcdDisplay();
        InitializeButtons();
        InitializeTools();
        GetBacklight()->SetBrightness(100);
        ESP_LOGI(TAG, "Robotweb ESP32-S3 ST7789 ready");
    }

    virtual Led* GetLed() override {
        static SingleLed led(BUILTIN_LED_GPIO);
        return &led;
    }

    virtual AudioCodec* GetAudioCodec() override {
        static Es8311AudioCodec audio(i2c_bus_, AUDIO_CODEC_I2C_NUM,
            AUDIO_INPUT_SAMPLE_RATE, AUDIO_OUTPUT_SAMPLE_RATE,
            AUDIO_I2S_GPIO_MCLK, AUDIO_I2S_GPIO_BCLK,
            AUDIO_I2S_GPIO_WS, AUDIO_I2S_GPIO_DOUT, AUDIO_I2S_GPIO_DIN,
            AUDIO_CODEC_PA_PIN, AUDIO_CODEC_ES8311_ADDR, true, true);
        return &audio;
    }

    virtual Display* GetDisplay() override { return display_; }

    virtual Backlight* GetBacklight() override {
        static PwmBacklight backlight(DISPLAY_BACKLIGHT_PIN, DISPLAY_BACKLIGHT_OUTPUT_INVERT);
        return &backlight;
    }
};

DECLARE_BOARD(RobowebESP32S3ST7789);
