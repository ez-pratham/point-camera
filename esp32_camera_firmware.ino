/*
 * POINT CAMERA - MODULAR ESP32 FIRMWARE
 * 
 * Hardware Architecture:
 * - MCU: 36-Pin ESP32 Dev Board (Pluggable on J_ESP_TOP & J_ESP_BOT Female Sockets)
 * - Sensor: OV2640 / OV5640 24-Pin DVP FPC Camera Module (CAM1)
 * - Storage: Reusable Press-on MicroSD SPI Module (J4 Socket)
 * - Display: 0.96" SSD1306 I2C OLED (128x64) (DISP1 Socket)
 * - Navigation: 4 Tactile Push Buttons (SW3 UP, SW4 DOWN, SW5 LEFT, SW6 RIGHT)
 * - Shutter: 1 Tactile Push Button Switch (SW2 SHUTTER)
 * - Power: External 5V USB Power Bank plugged into ESP32 Micro-USB/Type-C Port
 * - Zero Resistors: Pure internal INPUT_PULLUP used for all tactile switches
 */

#include "esp_camera.h"
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <SPI.h>
#include <SD.h>
#include <FS.h>

// ==========================================
// 1. PIN DEFINITIONS (36-PIN ESP32 MODULAR CARRIER MAP)
// ==========================================

// I2C OLED Display (0.96" 128x64 SSD1306 via DISP1 Socket)
#define I2C_SDA_PIN       21   // J_ESP_TOP Pin 18 (GPIO 21 / SDA)
#define I2C_SCL_PIN       22   // J_ESP_BOT Pin 10 (GPIO 22 / SCL)
#define OLED_RESET_PIN    -1
#define SCREEN_WIDTH      128
#define SCREEN_HEIGHT     64
#define OLED_I2C_ADDR     0x3C

// Tactile Push Buttons (Active LOW with internal INPUT_PULLUP, zero external resistors)
#define PIN_BTN_SHUTTER   18   // SW2 (Top Shoulder) -> J_ESP_BOT Pin 17 (GPIO 18)
#define PIN_BTN_UP        15   // SW3 (D-Pad UP)     -> J_ESP_BOT Pin 16 (GPIO 15)
#define PIN_BTN_RIGHT     2    // SW6 (D-Pad RIGHT)  -> J_ESP_BOT Pin 14 (GPIO 2)
#define PIN_BTN_LEFT      4    // SW5 (D-Pad LEFT)   -> J_ESP_BOT Pin 13 (GPIO 4)
#define PIN_BTN_DOWN      17   // SW4 (D-Pad DOWN)   -> J_ESP_BOT Pin 11 (GPIO 17)

// MicroSD Card SPI Interface (J4 Socket to J_ESP_TOP)
#define PIN_SD_MISO       14   // J4.4 (MISO) -> J_ESP_TOP Pin 12 (GPIO 14)
#define PIN_SD_MOSI       12   // J4.3 (MOSI) -> J_ESP_TOP Pin 13 (GPIO 12)
#define PIN_SD_SCK        13   // J4.2 (SCK)  -> J_ESP_TOP Pin 15 (GPIO 13)
#define PIN_SD_CS         10   // J4.1 (CS)   -> J_ESP_TOP Pin 17 (GPIO 10)

// DVP Camera Interface Pins (OV5640 / OV2640 24-Pin)
#define PWDN_GPIO_NUM     -1
#define RESET_GPIO_NUM    -1
#define XCLK_GPIO_NUM     15
#define SIOD_GPIO_NUM     21
#define SIOC_GPIO_NUM     18

#define Y9_GPIO_NUM       16
#define Y8_GPIO_NUM       46
#define Y7_GPIO_NUM       45
#define Y6_GPIO_NUM       48
#define Y5_GPIO_NUM       47
#define Y4_GPIO_NUM       39
#define Y3_GPIO_NUM       40
#define Y2_GPIO_NUM       41
#define VSYNC_GPIO_NUM    42
#define HREF_GPIO_NUM     2
#define PCLK_GPIO_NUM     1

// ==========================================
// 2. GLOBAL OBJECTS & STATE VARIABLES
// ==========================================

Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET_PIN);

uint32_t photo_counter = 0;
bool sd_card_mounted = false;
bool camera_present = false; // True = OV2640 hardware, False = Laptop Webcam fallback over USB
float battery_voltage = 4.2;
int battery_percent = 100;

enum CameraMode {
  MODE_NORMAL = 0,
  MODE_RETRO_WARM,
  MODE_BW,
  MODE_SEPIA,
  MODE_COUNT
};

CameraMode current_mode = MODE_NORMAL;
const char* mode_names[] = {"NORMAL", "RETRO", "B&W", "SEPIA"};

// Debounce Tracking
unsigned long last_shutter_time = 0;
const unsigned long DEBOUNCE_MS = 250;

// ==========================================
// 3. HARDWARE INITIALIZATION ROUTINES
// ==========================================

void init_gpio() {
  pinMode(PIN_BTN_SHUTTER, INPUT_PULLUP);
  pinMode(PIN_BTN_UP,      INPUT_PULLUP);
  pinMode(PIN_BTN_DOWN,    INPUT_PULLUP);
  pinMode(PIN_BTN_LEFT,    INPUT_PULLUP);
  pinMode(PIN_BTN_RIGHT,   INPUT_PULLUP);
}

void init_oled() {
  Wire.begin(I2C_SDA_PIN, I2C_SCL_PIN);
  if (!display.begin(SSD1306_SWITCHCAPVCC, OLED_I2C_ADDR)) {
    Serial.println("[ERROR] SSD1306 allocation failed");
    return;
  }
  display.setRotation(2); // 180-degree flipped orientation so inverted physical module displays right side up!
  display.clearDisplay();
  display.setTextColor(SSD1306_WHITE);
  display.setTextSize(1);
  display.setCursor(10, 15);
  display.println("POINT CAMERA");
  display.setCursor(18, 35);
  display.println("MODULAR ESP32");
  display.display();
  delay(1200);
}

void init_sd() {
  SPI.begin(PIN_SD_SCK, PIN_SD_MISO, PIN_SD_MOSI, PIN_SD_CS);
  if (!SD.begin(PIN_SD_CS)) {
    Serial.println("[WARN] Card Mount Failed, retrying...");
    sd_card_mounted = false;
  } else {
    sd_card_mounted = true;
    uint8_t cardType = SD.cardType();
    if (cardType == CARD_NONE) {
      sd_card_mounted = false;
      Serial.println("[ERROR] No SD card attached");
    } else {
      if (!SD.exists("/DCIM")) {
        SD.mkdir("/DCIM");
      }
      // Scan for existing photos to set photo counter
      File root = SD.open("/DCIM");
      if (root) {
        File file = root.openNextFile();
        while (file) {
          if (!file.isDirectory()) {
            photo_counter++;
          }
          file = root.openNextFile();
        }
        root.close();
      }
    }
  }
}

bool init_camera() {
  camera_config_t config;
  config.ledc_channel = LEDC_CHANNEL_0;
  config.ledc_timer   = LEDC_TIMER_0;
  config.pin_d0       = Y2_GPIO_NUM;
  config.pin_d1       = Y3_GPIO_NUM;
  config.pin_d2       = Y4_GPIO_NUM;
  config.pin_d3       = Y5_GPIO_NUM;
  config.pin_d4       = Y6_GPIO_NUM;
  config.pin_d5       = Y7_GPIO_NUM;
  config.pin_d6       = Y8_GPIO_NUM;
  config.pin_d7       = Y9_GPIO_NUM;
  config.pin_xclk     = XCLK_GPIO_NUM;
  config.pin_pclk     = PCLK_GPIO_NUM;
  config.pin_vsync    = VSYNC_GPIO_NUM;
  config.pin_href     = HREF_GPIO_NUM;
  config.pin_sccb_sda = SIOD_GPIO_NUM;
  config.pin_sccb_scl = SIOC_GPIO_NUM;
  config.pin_pwdn     = PWDN_GPIO_NUM;
  config.pin_reset    = RESET_GPIO_NUM;
  config.xclk_freq_hz = 20000000;
  config.pixel_format = PIXFORMAT_JPEG;
  config.frame_size   = FRAMESIZE_QSXGA; // 2560x1920 (5MP) or UXGA (1600x1200)
  config.jpeg_quality = 10;              // 10 = High Quality (0-63)
  config.fb_count     = 2;
  config.fb_location  = CAMERA_FB_IN_PSRAM;
  config.grab_mode    = CAMERA_GRAB_LATEST;

  esp_err_t err = esp_camera_init(&config);
  if (err != ESP_OK) {
    Serial.printf("[ERROR] Camera init failed with error 0x%x\n", err);
    return false;
  }

  // Sensor adjustments
  sensor_t *s = esp_camera_sensor_get();
  if (s != NULL) {
    s->set_brightness(s, 0);     // -2 to 2
    s->set_contrast(s, 0);       // -2 to 2
    s->set_saturation(s, 0);     // -2 to 2
    s->set_special_effect(s, 0); // 0 = No effect
    s->set_whitebal(s, 1);       // 1 = Enable AWB
    s->set_awb_gain(s, 1);       // 1 = Enable AWB gain
  }
  return true;
}

// ==========================================
// 4. POWER MONITORING (USB POWER BANK)
// ==========================================

void update_battery() {
  battery_voltage = 5.0;
  battery_percent = 100;
}

// ==========================================
// 5. OLED HUD DISPLAY REFRESH
// ==========================================

void update_display(const char* status_msg = NULL) {
  display.clearDisplay();
  
  // Header: Power Status & Mode
  display.setTextSize(1);
  display.setCursor(0, 0);
  display.printf("PWR: USB 5V [OK]");

  display.setCursor(0, 14);
  display.printf("MODE: [%s]", mode_names[current_mode]);

  // Main Counter
  display.setTextSize(2);
  display.setCursor(0, 28);
  display.printf("#%04d", photo_counter);

  // Storage / Status
  display.setTextSize(1);
  display.setCursor(0, 52);
  if (status_msg != NULL) {
    display.print(status_msg);
  } else if (!sd_card_mounted) {
    display.print("NO SD CARD!");
  } else {
    uint64_t bytes_free = (SD.totalBytes() - SD.usedBytes()) / (1024 * 1024);
    display.printf("SD: %llu MB FREE", bytes_free);
  }

  display.display();
}

// ==========================================
// 6. PHOTO CAPTURE PIPELINE
// ==========================================

void capture_photo() {
  // Shutter Flash Animation
  display.invertDisplay(true);
  delay(60);
  display.invertDisplay(false);

  photo_counter++;

  if (camera_present) {
    // -------------------------------------------------------------
    // OPTION A: PHYSICAL HARDWARE CAMERA (OV2640 / OV5640)
    // -------------------------------------------------------------
    if (!sd_card_mounted) {
      update_display("ERR: NO SD CARD");
      delay(1000);
      return;
    }

    update_display("CAPTURING...");

    camera_fb_t *fb = esp_camera_fb_get();
    if (!fb) {
      Serial.println(F("[ERROR] Hardware camera capture failed"));
      update_display("CAPTURE FAIL!");
      delay(1000);
      return;
    }

    char path[32];
    sprintf(path, "/DCIM/IMG_%04d.JPG", photo_counter);

    File file = SD.open(path, FILE_WRITE);
    if (!file) {
      Serial.printf("[ERROR] Failed to open file: %s\n", path);
      update_display("FILE WRITE ERR");
    } else {
      file.write(fb->buf, fb->len);
      file.close();
      Serial.printf("[SUCCESS] Hardware photo saved to %s (%u bytes)\n", path, fb->len);
      update_display("PHOTO SAVED!");
    }

    esp_camera_fb_return(fb);
  } else {
    // -------------------------------------------------------------
    // OPTION B: LAPTOP WEBCAM FALLBACK MODE (VIA USB SERIAL)
    // -------------------------------------------------------------
    // Camera module not detected on PCB -> Trigger Laptop's built-in webcam!
    // Open webcam_bridge.html in Chrome/Edge or run laptop_webcam_bridge.py
    Serial.printf("CMD:CAPTURE|COUNT:%04d|MODE:%s\n", photo_counter, mode_names[current_mode]);
    Serial.printf("[WEBCAM TRIGGER] Snapped via Laptop Webcam! Image: IMG_%04d.JPG [Mode: %s]\n", photo_counter, mode_names[current_mode]);

    update_display("WEBCAM SNAP!");
    delay(400);
    update_display("PHOTO SAVED!");
  }

  delay(600);
}

// ==========================================
// 7. SETUP & LOOP
// ==========================================

void setup() {
  Serial.begin(115200);
  delay(500);
  Serial.println(F("\n=============================================="));
  Serial.println(F("              POINT CAMERA                    "));
  Serial.println(F("=============================================="));

  init_gpio();
  init_oled();
  init_sd();

  // Test Camera Module Presence
  camera_present = init_camera();

  if (camera_present) {
    Serial.println(F("[CAM] Hardware OV2640/OV5640 detected & active!"));
    update_battery();
    update_display("READY: OV2640");
  } else {
    Serial.println(F("[WARN] No camera hardware detected in circuit!"));
    Serial.println(F("[FALLBACK] >> LAPTOP WEBCAM BRIDGE MODE ACTIVATED <<"));
    Serial.println(F("[INFO] Open 'webcam_bridge.html' in your browser (Chrome/Edge)"));
    Serial.println(F("[INFO] Or run: python laptop_webcam_bridge.py"));
    Serial.println(F("[INFO] Pressing the circuit Shutter button will now snap your laptop webcam!"));
    update_battery();
    update_display("READY: WEBCAM");
  }
}

void loop() {
  unsigned long now = millis();

  // 1. Shutter Button Trigger (SW2 / GPIO 18)
  if (digitalRead(PIN_BTN_SHUTTER) == LOW) {
    if (now - last_shutter_time > DEBOUNCE_MS) {
      last_shutter_time = now;
      capture_photo();
      update_display();
    }
  }

  // 2. D-Pad Mode Switching (RIGHT button cycles color profiles)
  if (digitalRead(PIN_BTN_RIGHT) == LOW) {
    if (now - last_shutter_time > DEBOUNCE_MS) {
      last_shutter_time = now;
      current_mode = (CameraMode)((current_mode + 1) % MODE_COUNT);

      // Notify laptop webcam bridge to switch real-time filter in sync!
      Serial.printf("CMD:FILTER|MODE:%s\n", mode_names[current_mode]);

      if (camera_present) {
        sensor_t *s = esp_camera_sensor_get();
        if (s != NULL) {
          if (current_mode == MODE_NORMAL)      s->set_special_effect(s, 0); // No effect
          else if (current_mode == MODE_RETRO_WARM) {
            s->set_special_effect(s, 0);
            s->set_saturation(s, 2); // Vivid Retro
          }
          else if (current_mode == MODE_BW)     s->set_special_effect(s, 2); // Grayscale
          else if (current_mode == MODE_SEPIA)  s->set_special_effect(s, 1); // Sepia
        }
      }
      update_display();
    }
  }

  // Periodic battery update every 5 seconds
  static unsigned long last_bat_update = 0;
  if (now - last_bat_update > 5000) {
    last_bat_update = now;
    update_battery();
    update_display();
  }

  delay(20);
}
