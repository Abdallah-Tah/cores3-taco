#include "wifi_board.h"
#include "cores3_audio_codec.h"
#include "display/lcd_display.h"
#include "application.h"
#include "config.h"
#include "power_save_timer.h"
#include "i2c_device.h"
#include "axp2101.h"
extern "C" {
#include "lvgl_kawaii_face.h"
}

#include <esp_log.h>
#include <driver/i2c_master.h>
#include <esp_lcd_panel_io.h>
#include <esp_lcd_panel_ops.h>
#include <esp_lcd_ili9341.h>
#include <esp_timer.h>
#include "esp_video.h"
#include <cmath>
#include <cstring>

#define TAG "M5StackCoreS3Board"

class Pmic : public Axp2101 {
public:
    // Power Init
    Pmic(i2c_master_bus_handle_t i2c_bus, uint8_t addr) : Axp2101(i2c_bus, addr) {
        uint8_t data = ReadReg(0x90);
        data |= 0b10110100;
        WriteReg(0x90, data);
        WriteReg(0x99, (0b11110 - 5));
        WriteReg(0x97, (0b11110 - 2));
        WriteReg(0x69, 0b00110101);
        WriteReg(0x30, 0b111111);
        WriteReg(0x90, 0xBF);
        WriteReg(0x94, 33 - 5);
        WriteReg(0x95, 33 - 5);
    }

    void SetBrightness(uint8_t brightness) {
        brightness = ((brightness + 641) >> 5);
        WriteReg(0x99, brightness);
    }
};

class CustomBacklight : public Backlight {
public:
    CustomBacklight(Pmic *pmic) : pmic_(pmic) {}

    void SetBrightnessImpl(uint8_t brightness) override {
        pmic_->SetBrightness(target_brightness_);
        brightness_ = target_brightness_;
    }

private:
    Pmic *pmic_;
};

// A lightweight, native LVGL version of Taco's original animated face.  Keeping
// it in the board display avoids large bitmap assets and leaves the camera and
// audio buffers in PSRAM untouched.
class TacoFaceDisplay : public SpiLcdDisplay {
public:
    TacoFaceDisplay(esp_lcd_panel_io_handle_t panel_io, esp_lcd_panel_handle_t panel,
                    int width, int height, int offset_x, int offset_y, bool mirror_x,
                    bool mirror_y, bool swap_xy)
        : SpiLcdDisplay(panel_io, panel, width, height, offset_x, offset_y,
                        mirror_x, mirror_y, swap_xy) {}

    void SetupUI() override {
        SpiLcdDisplay::SetupUI();
        DisplayLockGuard lock(this);

        lv_obj_t* screen = lv_screen_active();
        face_ = lv_obj_create(screen);
        lv_obj_set_size(face_, 320, 190);
        lv_obj_align(face_, LV_ALIGN_CENTER, 0, 5);
        lv_obj_set_style_bg_color(face_, lv_color_hex(0x020817), 0);
        lv_obj_set_style_bg_opa(face_, LV_OPA_COVER, 0);
        lv_obj_set_style_border_color(face_, lv_color_hex(0x0AADE8), 0);
        lv_obj_set_style_border_width(face_, 2, 0);
        lv_obj_set_style_radius(face_, 90, 0);
        lv_obj_set_style_pad_all(face_, 0, 0);
        lv_obj_clear_flag(face_, LV_OBJ_FLAG_SCROLLABLE);

        left_eye_ = CreateEye(face_, 54, 55);
        right_eye_ = CreateEye(face_, 194, 55);
        left_pupil_ = CreatePupil(left_eye_);
        right_pupil_ = CreatePupil(right_eye_);
        left_brow_ = CreateBrow(face_, 58, 38);
        right_brow_ = CreateBrow(face_, 198, 38);

        mouth_ = lv_obj_create(face_);
        lv_obj_set_size(mouth_, 54, 9);
        lv_obj_set_pos(mouth_, 133, 145);
        StyleGlow(mouth_, 0x20D9FF, 8);

        // The standard emoji stays available internally, but Taco owns the
        // center of the screen. Status and subtitle bars remain above it.
        if (emoji_box_) lv_obj_add_flag(emoji_box_, LV_OBJ_FLAG_HIDDEN);
        // Base SetupUI creates an opaque content container. Keep Taco above that
        // layer, then restore the small status/subtitle overlays above the face.
        lv_obj_move_foreground(face_);
        if (top_bar_) lv_obj_move_foreground(top_bar_);
        if (status_bar_) lv_obj_move_foreground(status_bar_);
        if (bottom_bar_) lv_obj_move_foreground(bottom_bar_);
        if (low_battery_popup_) lv_obj_move_foreground(low_battery_popup_);
        animation_timer_ = lv_timer_create(AnimationTimer, 50, this);
        ApplyMood("taco_idle");
    }

    void SetEmotion(const char* emotion) override {
        if (!face_ || !emotion) return;
        DisplayLockGuard lock(this);
        ApplyMood(emotion);
    }

private:
    lv_obj_t* face_ = nullptr;
    lv_obj_t* left_eye_ = nullptr;
    lv_obj_t* right_eye_ = nullptr;
    lv_obj_t* left_pupil_ = nullptr;
    lv_obj_t* right_pupil_ = nullptr;
    lv_obj_t* left_brow_ = nullptr;
    lv_obj_t* right_brow_ = nullptr;
    lv_obj_t* mouth_ = nullptr;
    lv_timer_t* animation_timer_ = nullptr;
    bool talking_ = false;
    bool sleepy_ = false;
    bool surprised_ = false;
    uint32_t mood_color_ = 0x20D9FF;

    static void StyleGlow(lv_obj_t* obj, uint32_t color, int radius) {
        lv_obj_set_style_bg_color(obj, lv_color_hex(color), 0);
        lv_obj_set_style_bg_opa(obj, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(obj, 0, 0);
        lv_obj_set_style_radius(obj, radius, 0);
        lv_obj_set_style_shadow_color(obj, lv_color_hex(color), 0);
        lv_obj_set_style_shadow_width(obj, 10, 0);
        lv_obj_set_style_shadow_opa(obj, LV_OPA_40, 0);
        lv_obj_clear_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
    }

    static lv_obj_t* CreateEye(lv_obj_t* parent, int x, int y) {
        auto eye = lv_obj_create(parent);
        lv_obj_set_size(eye, 72, 64);
        lv_obj_set_pos(eye, x, y);
        StyleGlow(eye, 0xD9FAFF, 20);
        lv_obj_set_style_pad_all(eye, 0, 0);
        return eye;
    }

    static lv_obj_t* CreatePupil(lv_obj_t* eye) {
        auto pupil = lv_obj_create(eye);
        lv_obj_set_size(pupil, 20, 30);
        lv_obj_center(pupil);
        StyleGlow(pupil, 0x073B63, 8);
        return pupil;
    }

    static lv_obj_t* CreateBrow(lv_obj_t* parent, int x, int y) {
        auto brow = lv_obj_create(parent);
        lv_obj_set_size(brow, 64, 6);
        lv_obj_set_pos(brow, x, y);
        StyleGlow(brow, 0x20D9FF, 3);
        return brow;
    }

    static void AnimationTimer(lv_timer_t* timer) {
        static_cast<TacoFaceDisplay*>(lv_timer_get_user_data(timer))->Animate();
    }

    void Animate() {
        const uint32_t now = lv_tick_get();
        const float t = now / 1000.0f;
        const bool blink = !surprised_ && (now % 4300U) > 4130U;
        const int normal_h = sleepy_ ? 22 : (surprised_ ? 70 : 64);
        const int eye_h = blink ? 7 : normal_h;
        lv_obj_set_height(left_eye_, eye_h);
        lv_obj_set_height(right_eye_, eye_h);

        if (!blink && !sleepy_) {
            const int gaze_x = static_cast<int>(std::sin(t * 0.72f) * 8.0f);
            const int gaze_y = static_cast<int>(std::sin(t * 0.43f) * 4.0f);
            lv_obj_align(left_pupil_, LV_ALIGN_CENTER, gaze_x, gaze_y);
            lv_obj_align(right_pupil_, LV_ALIGN_CENTER, gaze_x, gaze_y);
        }

        if (talking_) {
            const int mouth_h = 10 + static_cast<int>((std::sin(t * 13.0f) + 1.0f) * 12.0f);
            lv_obj_set_size(mouth_, 44 + mouth_h, mouth_h);
            lv_obj_set_x(mouth_, 160 - (44 + mouth_h) / 2);
        }
    }

    void ApplyMood(const char* emotion) {
        talking_ = std::strstr(emotion, "speaking") || std::strstr(emotion, "talking");
        sleepy_ = std::strstr(emotion, "sleep") || std::strstr(emotion, "connecting");
        surprised_ = std::strstr(emotion, "surpris") || std::strstr(emotion, "listening");

        int left_angle = 0;
        int right_angle = 0;
        if (std::strstr(emotion, "angry") || std::strstr(emotion, "grumpy")) {
            left_angle = 140;
            right_angle = -140;
        } else if (std::strstr(emotion, "curious") || std::strstr(emotion, "thinking")) {
            left_angle = -70;
            right_angle = 40;
        }
        lv_obj_set_style_transform_rotation(left_brow_, left_angle, 0);
        lv_obj_set_style_transform_rotation(right_brow_, right_angle, 0);

        const int mouth_h = surprised_ ? 28 : (talking_ ? 20 : 9);
        const int mouth_w = surprised_ ? 28 : (talking_ ? 60 : 54);
        lv_obj_set_size(mouth_, mouth_w, mouth_h);
        lv_obj_set_pos(mouth_, 160 - mouth_w / 2, 145 - mouth_h / 2);
    }
};

// A full-screen companion face powered by lvgl_kawaii_face. The screen stays
// intentionally quiet: one expressive character, a tiny state hint, and no
// dashboard chrome competing for 320x240 pixels.
class CoreS3CompanionDisplay : public SpiLcdDisplay {
public:
    CoreS3CompanionDisplay(esp_lcd_panel_io_handle_t panel_io, esp_lcd_panel_handle_t panel,
                           int width, int height, int offset_x, int offset_y, bool mirror_x,
                           bool mirror_y, bool swap_xy)
        : SpiLcdDisplay(panel_io, panel, width, height, offset_x, offset_y, mirror_x, mirror_y,
                        swap_xy) {}

    void SetupUI() override {
        SpiLcdDisplay::SetupUI();
        {
            DisplayLockGuard lock(this);
            auto screen = lv_screen_active();

            stage_ = lv_obj_create(screen);
            lv_obj_set_size(stage_, 320, 240);
            lv_obj_center(stage_);
            lv_obj_set_style_bg_color(stage_, lv_color_hex(0x05070D), 0);
            lv_obj_set_style_bg_opa(stage_, LV_OPA_COVER, 0);
            lv_obj_set_style_border_width(stage_, 0, 0);
            lv_obj_set_style_pad_all(stage_, 0, 0);
            lv_obj_clear_flag(stage_, LV_OBJ_FLAG_SCROLLABLE);

            auto brand = lv_label_create(stage_);
            lv_label_set_text(brand, "taco");
            lv_obj_set_style_text_color(brand, lv_color_hex(0x7D8A9A), 0);
            lv_obj_set_style_text_letter_space(brand, 2, 0);
            lv_obj_align(brand, LV_ALIGN_TOP_MID, 0, 8);

            face_panel_ = lv_obj_create(stage_);
            lv_obj_set_size(face_panel_, 292, 180);
            lv_obj_align(face_panel_, LV_ALIGN_CENTER, 0, 0);
            lv_obj_set_style_bg_opa(face_panel_, LV_OPA_TRANSP, 0);
            lv_obj_set_style_border_width(face_panel_, 0, 0);
            lv_obj_set_style_pad_all(face_panel_, 0, 0);
            lv_obj_clear_flag(face_panel_, LV_OBJ_FLAG_SCROLLABLE);

            state_label_ = lv_label_create(stage_);
            lv_label_set_text(state_label_, "tap to talk");
            lv_obj_set_style_text_color(state_label_, lv_color_hex(0x566274), 0);
            lv_obj_set_style_text_letter_space(state_label_, 1, 0);
            lv_obj_align(state_label_, LV_ALIGN_BOTTOM_MID, 0, -8);

            lv_obj_move_foreground(stage_);
        }

        face_config_t config = {
            .parent = face_panel_,
            .animation_speed = 30,
            .blink_interval = 3200,
            .auto_blink = true,
        };
        ESP_ERROR_CHECK(face_animation_init(&config));
        face_set_emotion(FACE_NEUTRAL, false);
    }

    void SetEmotion(const char* emotion) override {
        if (!emotion) return;
        face_emotion_t face_emotion = FACE_NEUTRAL;
        const char* hint = "tap to talk";

        if (std::strstr(emotion, "listening") || std::strstr(emotion, "surpris")) {
            face_emotion = FACE_SURPRISED;
            hint = "listening";
        } else if (std::strstr(emotion, "speaking") || std::strstr(emotion, "happy")) {
            face_emotion = FACE_HAPPY;
            hint = "speaking";
        } else if (std::strstr(emotion, "thinking") || std::strstr(emotion, "curious")) {
            face_emotion = FACE_CONFUSED;
            hint = "thinking";
        } else if (std::strstr(emotion, "sleep") || std::strstr(emotion, "connecting")) {
            face_emotion = FACE_SLEEPY;
            hint = "connecting";
        } else if (std::strstr(emotion, "angry") || std::strstr(emotion, "grumpy")) {
            face_emotion = FACE_ANGRY;
        } else if (std::strstr(emotion, "sad")) {
            face_emotion = FACE_SAD;
        } else if (std::strstr(emotion, "love")) {
            face_emotion = FACE_LOVE;
        }

        face_set_emotion(face_emotion, true);
        DisplayLockGuard lock(this);
        if (state_label_) lv_label_set_text(state_label_, hint);
    }

private:
    lv_obj_t* stage_ = nullptr;
    lv_obj_t* face_panel_ = nullptr;
    lv_obj_t* state_label_ = nullptr;
};

class Aw9523 : public I2cDevice {
public:
    // Exanpd IO Init
    Aw9523(i2c_master_bus_handle_t i2c_bus, uint8_t addr) : I2cDevice(i2c_bus, addr) {
        WriteReg(0x02, 0b00000111);  // P0
        WriteReg(0x03, 0b10001111);  // P1
        WriteReg(0x04, 0b00011000);  // CONFIG_P0
        WriteReg(0x05, 0b00001100);  // CONFIG_P1
        WriteReg(0x11, 0b00010000);  // GCR P0 port is Push-Pull mode.
        WriteReg(0x12, 0b11111111);  // LEDMODE_P0
        WriteReg(0x13, 0b11111111);  // LEDMODE_P1
    }

    void ResetAw88298() {
        ESP_LOGI(TAG, "Reset AW88298");
        WriteReg(0x02, 0b00000011);
        vTaskDelay(pdMS_TO_TICKS(10));
        WriteReg(0x02, 0b00000111);
        vTaskDelay(pdMS_TO_TICKS(50));
    }

    void ResetIli9342() {
        ESP_LOGI(TAG, "Reset IlI9342");
        WriteReg(0x03, 0b10000001);
        vTaskDelay(pdMS_TO_TICKS(20));
        WriteReg(0x03, 0b10000011);
        vTaskDelay(pdMS_TO_TICKS(10));
    }
};

class Ft6336 : public I2cDevice {
public:
    struct TouchPoint_t {
        int num = 0;
        int x = -1;
        int y = -1;
    };
    
    Ft6336(i2c_master_bus_handle_t i2c_bus, uint8_t addr) : I2cDevice(i2c_bus, addr) {
        uint8_t chip_id = ReadReg(0xA3);
        ESP_LOGI(TAG, "Get chip ID: 0x%02X", chip_id);
        read_buffer_ = new uint8_t[6];
    }

    ~Ft6336() {
        delete[] read_buffer_;
    }

    void UpdateTouchPoint() {
        ReadRegs(0x02, read_buffer_, 6);
        tp_.num = read_buffer_[0] & 0x0F;
        tp_.x = ((read_buffer_[1] & 0x0F) << 8) | read_buffer_[2];
        tp_.y = ((read_buffer_[3] & 0x0F) << 8) | read_buffer_[4];
    }

    inline const TouchPoint_t& GetTouchPoint() {
        return tp_;
    }

private:
    uint8_t* read_buffer_ = nullptr;
    TouchPoint_t tp_;
};

class M5StackCoreS3Board : public WifiBoard {
private:
    i2c_master_bus_handle_t i2c_bus_;
    Pmic* pmic_;
    Aw9523* aw9523_;
    Ft6336* ft6336_;
    LcdDisplay* display_;
    EspVideo* camera_;
    esp_timer_handle_t touchpad_timer_;
    PowerSaveTimer* power_save_timer_;

    void InitializePowerSaveTimer() {
        power_save_timer_ = new PowerSaveTimer(-1, 60, 300);
        power_save_timer_->OnEnterSleepMode([this]() {
            GetDisplay()->SetPowerSaveMode(true);
            GetBacklight()->SetBrightness(10);
        });
        power_save_timer_->OnExitSleepMode([this]() {
            GetDisplay()->SetPowerSaveMode(false);
            GetBacklight()->RestoreBrightness();
        });
        power_save_timer_->OnShutdownRequest([this]() {
            pmic_->PowerOff();
        });
        power_save_timer_->SetEnabled(true);
    }

    void InitializeI2c() {
        // Initialize I2C peripheral
        i2c_master_bus_config_t i2c_bus_cfg = {
            .i2c_port = (i2c_port_t)1,
            .sda_io_num = AUDIO_CODEC_I2C_SDA_PIN,
            .scl_io_num = AUDIO_CODEC_I2C_SCL_PIN,
            .clk_source = I2C_CLK_SRC_DEFAULT,
            .glitch_ignore_cnt = 7,
            .intr_priority = 0,
            .trans_queue_depth = 0,
            .flags = {
                .enable_internal_pullup = 1,
            },
        };
        ESP_ERROR_CHECK(i2c_new_master_bus(&i2c_bus_cfg, &i2c_bus_));
    }

    void I2cDetect() {
        uint8_t address;
        printf("     0  1  2  3  4  5  6  7  8  9  a  b  c  d  e  f\r\n");
        for (int i = 0; i < 128; i += 16) {
            printf("%02x: ", i);
            for (int j = 0; j < 16; j++) {
                fflush(stdout);
                address = i + j;
                esp_err_t ret = i2c_master_probe(i2c_bus_, address, pdMS_TO_TICKS(200));
                if (ret == ESP_OK) {
                    printf("%02x ", address);
                } else if (ret == ESP_ERR_TIMEOUT) {
                    printf("UU ");
                } else {
                    printf("-- ");
                }
            }
            printf("\r\n");
        }
    }

    void InitializeAxp2101() {
        ESP_LOGI(TAG, "Init AXP2101");
        pmic_ = new Pmic(i2c_bus_, 0x34);
    }

    void InitializeAw9523() {
        ESP_LOGI(TAG, "Init AW9523");
        aw9523_ = new Aw9523(i2c_bus_, 0x58);
        vTaskDelay(pdMS_TO_TICKS(50));
    }

    void PollTouchpad() {
        static bool was_touched = false;
        static int64_t touch_start_time = 0;
        const int64_t TOUCH_THRESHOLD_MS = 500;  // 触摸时长阈值，超过500ms视为长按
        
        ft6336_->UpdateTouchPoint();
        auto& touch_point = ft6336_->GetTouchPoint();
        
        // 检测触摸开始
        if (touch_point.num > 0 && !was_touched) {
            was_touched = true;
            touch_start_time = esp_timer_get_time() / 1000; // 转换为毫秒
        } 
        // 检测触摸释放
        else if (touch_point.num == 0 && was_touched) {
            was_touched = false;
            int64_t touch_duration = (esp_timer_get_time() / 1000) - touch_start_time;
            
            // The entire face is one generous talk target. Volume remains a
            // voice command, avoiding tiny controls on a 320x240 display.
            if (touch_duration < TOUCH_THRESHOLD_MS) {
                auto& app = Application::GetInstance();
                if (app.GetDeviceState() == kDeviceStateStarting) {
                    EnterWifiConfigMode();
                    return;
                }
                app.ToggleChatState();
            }
        }
    }

    void InitializeFt6336TouchPad() {
        ESP_LOGI(TAG, "Init FT6336");
        ft6336_ = new Ft6336(i2c_bus_, 0x38);
        
        // 创建定时器，20ms 间隔
        esp_timer_create_args_t timer_args = {
            .callback = [](void* arg) {
                M5StackCoreS3Board* board = (M5StackCoreS3Board*)arg;
                board->PollTouchpad();
            },
            .arg = this,
            .dispatch_method = ESP_TIMER_TASK,
            .name = "touchpad_timer",
            .skip_unhandled_events = true,
        };
        
        ESP_ERROR_CHECK(esp_timer_create(&timer_args, &touchpad_timer_));
        ESP_ERROR_CHECK(esp_timer_start_periodic(touchpad_timer_, 20 * 1000));
    }

    void InitializeSpi() {
        spi_bus_config_t buscfg = {};
        buscfg.mosi_io_num = GPIO_NUM_37;
        buscfg.miso_io_num = GPIO_NUM_NC;
        buscfg.sclk_io_num = GPIO_NUM_36;
        buscfg.quadwp_io_num = GPIO_NUM_NC;
        buscfg.quadhd_io_num = GPIO_NUM_NC;
        buscfg.max_transfer_sz = DISPLAY_WIDTH * DISPLAY_HEIGHT * sizeof(uint16_t);
        ESP_ERROR_CHECK(spi_bus_initialize(SPI3_HOST, &buscfg, SPI_DMA_CH_AUTO));
    }

    void InitializeIli9342Display() {
        ESP_LOGI(TAG, "Init IlI9342");

        esp_lcd_panel_io_handle_t panel_io = nullptr;
        esp_lcd_panel_handle_t panel = nullptr;

        ESP_LOGD(TAG, "Install panel IO");
        esp_lcd_panel_io_spi_config_t io_config = {};
        io_config.cs_gpio_num = GPIO_NUM_3;
        io_config.dc_gpio_num = GPIO_NUM_35;
        io_config.spi_mode = 2;
        io_config.pclk_hz = 40 * 1000 * 1000;
        io_config.trans_queue_depth = 10;
        io_config.lcd_cmd_bits = 8;
        io_config.lcd_param_bits = 8;
        ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi(SPI3_HOST, &io_config, &panel_io));

        ESP_LOGD(TAG, "Install LCD driver");
        esp_lcd_panel_dev_config_t panel_config = {};
        panel_config.reset_gpio_num = GPIO_NUM_NC;
        panel_config.rgb_ele_order = LCD_RGB_ELEMENT_ORDER_BGR;
        panel_config.bits_per_pixel = 16;
        ESP_ERROR_CHECK(esp_lcd_new_panel_ili9341(panel_io, &panel_config, &panel));
        
        esp_lcd_panel_reset(panel);
        aw9523_->ResetIli9342();

        esp_lcd_panel_init(panel);
        esp_lcd_panel_invert_color(panel, true);
        esp_lcd_panel_swap_xy(panel, DISPLAY_SWAP_XY);
        esp_lcd_panel_mirror(panel, DISPLAY_MIRROR_X, DISPLAY_MIRROR_Y);

        display_ = new CoreS3CompanionDisplay(
            panel_io, panel, DISPLAY_WIDTH, DISPLAY_HEIGHT, DISPLAY_OFFSET_X, DISPLAY_OFFSET_Y,
            DISPLAY_MIRROR_X, DISPLAY_MIRROR_Y, DISPLAY_SWAP_XY);
    }

     void InitializeCamera() {
        static esp_cam_ctlr_dvp_pin_config_t dvp_pin_config = {
            .data_width = CAM_CTLR_DATA_WIDTH_8,
            .data_io = {
                [0] = CAMERA_PIN_D0,
                [1] = CAMERA_PIN_D1,
                [2] = CAMERA_PIN_D2,
                [3] = CAMERA_PIN_D3,
                [4] = CAMERA_PIN_D4,
                [5] = CAMERA_PIN_D5,
                [6] = CAMERA_PIN_D6,
                [7] = CAMERA_PIN_D7,
            },
            .vsync_io = CAMERA_PIN_VSYNC,
            .de_io = CAMERA_PIN_HREF,
            .pclk_io = CAMERA_PIN_PCLK,
            .xclk_io = CAMERA_PIN_XCLK,
        };

        esp_video_init_sccb_config_t sccb_config = {
            .init_sccb = false,
            .i2c_handle = i2c_bus_,
            .freq = 100000,
        };

        esp_video_init_dvp_config_t dvp_config = {
            .sccb_config = sccb_config,
            .reset_pin = CAMERA_PIN_RESET,
            .pwdn_pin = CAMERA_PIN_PWDN,
            .dvp_pin = dvp_pin_config,
            .xclk_freq = XCLK_FREQ_HZ,
        };

        esp_video_init_config_t video_config = {
            .dvp = &dvp_config,
        };

        camera_ = new EspVideo(video_config);
    }

public:
    M5StackCoreS3Board() {
        InitializePowerSaveTimer();
        InitializeI2c();
        InitializeAxp2101();
        InitializeAw9523();
        I2cDetect();
        InitializeSpi();
        InitializeIli9342Display();
        InitializeCamera();
        InitializeFt6336TouchPad();
        GetBacklight()->RestoreBrightness();
    }

    virtual AudioCodec* GetAudioCodec() override {
        static CoreS3AudioCodec audio_codec(i2c_bus_,
            AUDIO_INPUT_SAMPLE_RATE,
            AUDIO_OUTPUT_SAMPLE_RATE,
            AUDIO_I2S_GPIO_MCLK,
            AUDIO_I2S_GPIO_BCLK,
            AUDIO_I2S_GPIO_WS,
            AUDIO_I2S_GPIO_DOUT,
            AUDIO_I2S_GPIO_DIN,
            AUDIO_CODEC_AW88298_ADDR,
            AUDIO_CODEC_ES7210_ADDR,
            AUDIO_INPUT_REFERENCE);
        return &audio_codec;
    }

    virtual Display* GetDisplay() override {
        return display_;
    }

    virtual Camera* GetCamera() override {
        return camera_;
    }

    virtual bool GetBatteryLevel(int &level, bool& charging, bool& discharging) override {
        static bool last_discharging = false;
        charging = pmic_->IsCharging();
        discharging = pmic_->IsDischarging();
        if (discharging != last_discharging) {
            power_save_timer_->SetEnabled(discharging);
            last_discharging = discharging;
        }

        level = pmic_->GetBatteryLevel();
        return true;
    }

    virtual void SetPowerSaveLevel(PowerSaveLevel level) override {
        if (level != PowerSaveLevel::LOW_POWER) {
            power_save_timer_->WakeUp();
        }
        WifiBoard::SetPowerSaveLevel(level);
    }

    virtual Backlight *GetBacklight() override {
        static CustomBacklight backlight(pmic_);
        return &backlight;
    }
};

DECLARE_BOARD(M5StackCoreS3Board);
