#include "FS.h"
#include <Arduino.h>
#include <WiFi.h>
#include <TFT_eSPI.h>
#include <SPI.h>
#include <SD.h>
#include <lvgl.h>
#include "secrets.h"

// Touch
#define CALIBRATION_FILE "/calibrationData"

// Display
TFT_eSPI tft = TFT_eSPI();
lv_display_t *disp;
#define TFT_HOR_RES 320 // px
#define TFT_VER_RES 480 // px
#define TFT_ROTATION LV_DISPLAY_ROTATION_270
#define LV_CUSTOM_FONT_DECLARE(LV_FONT_MONTSERRAT_30)
#define LVGL_DRAW_BUF_SIZE (TFT_HOR_RES * TFT_VER_RES / 10 * (LV_COLOR_DEPTH / 8))
#define CANVAS_DRAW_BUF_SIZE (50 * 50 * sizeof(lv_color_t))
lv_obj_t * canvasObj;
static uint32_t draw_buf[LVGL_DRAW_BUF_SIZE / 4]; // is this supposed to be static?
// static uint32_t buffer[LV_CANVAS_BUF_SIZE(TFT_HOR_RES, TFT_VER_RES, 8, stride_in_bytes)]

static lv_obj_t *scr_welcome;
static lv_obj_t *scr_systemsettings;
static lv_obj_t *scr_networksettings;
static lv_obj_t *scr_canvas;
lv_obj_t *previous_scr = NULL;
typedef enum
{
  SCREEN_WELCOME,
  SCREEN_CANVAS,
  SCREEN_NETWORK_SETTINGS,
  SCREEN_SYSTEM_SETTINGS
} screen_id_t;

// Network
#define LOCAL_HOSTNAME "friendbox"

// Storage
#define TEST_FILE_SIZE (4 * 1024 * 1024)
File testFile;
SPIClass sdspi = SPIClass(HSPI);

// Functions
void lv_create_network_settings();
void lv_create_welcome_screen();
void lv_create_canvas();
void lv_switch_screen(screen_id_t id);
static uint32_t systemTick(void);

#pragma region EventHandlers

int btn1_count = 0;
// Callback that is triggered when btn1 is clicked
static void event_handler_scr_welcome_login(lv_event_t *e)
{
  lv_event_code_t code = lv_event_get_code(e);
  if (code == LV_EVENT_CLICKED)
  {
    btn1_count++;
    LV_LOG_USER("Button clicked %d", (int)btn1_count);
  }
}

// Callback that is triggered when btn2 is clicked/toggled
static void event_handler_scr_welcome_netsettings(lv_event_t *e)
{
  lv_event_code_t code = lv_event_get_code(e);
  if (code == LV_EVENT_CLICKED)
  {
    lv_switch_screen(SCREEN_NETWORK_SETTINGS);
  }
}

// Callback that is triggered when btn2 is clicked/toggled
static void event_handler_scr_network_back(lv_event_t *e)
{
  lv_event_code_t code = lv_event_get_code(e);
  if (code == LV_EVENT_CLICKED)
  {
    lv_switch_screen(SCREEN_WELCOME);
  }
}

// Callback that is triggered when btn3 is clicked
static void event_handler_scr_welcome_syssettings(lv_event_t *e)
{
  lv_event_code_t code = lv_event_get_code(e);
  if (code == LV_EVENT_CLICKED)
  {
    // Open system settings menu
  }
}

// Callback that is triggered when btn4 is clicked
static void event_handler_shutdown(lv_event_t *e)
{
  lv_event_code_t code = lv_event_get_code(e);
  if (code == LV_EVENT_CLICKED)
  {
    Serial.println("Draw...");
    // esp_deep_sleep(3000000);
    lv_switch_screen(SCREEN_CANVAS);
  }
}

static lv_obj_t *slider_label;
// Callback that prints the current slider value on the TFT display and Serial Monitor for debugging purposes
static void slider_event_callback(lv_event_t *e)
{
  lv_obj_t *slider = (lv_obj_t *)lv_event_get_target(e);
  char buf[8];
  lv_snprintf(buf, sizeof(buf), "%d%%", (int)lv_slider_get_value(slider));
  lv_label_set_text(slider_label, buf);
  lv_obj_align_to(slider_label, slider, LV_ALIGN_OUT_BOTTOM_MID, 0, 10);
  LV_LOG_USER("Slider changed to %d%%", (int)lv_slider_get_value(slider));
  dacWrite(26, (int)lv_slider_get_value(slider));
}

/*void handleTouch() {
  uint16_t touchX, touchY;
  static uint16_t color;

  if (tft.getTouch(&touchX, &touchY)) {

    tft.setCursor(400, 5, 2);
    tft.printf("touchX: %i     ", touchX);
    tft.setCursor(400, 50, 2);
    tft.printf("touchY: %i    ", touchY);

    tft.drawPixel(touchX, touchY, color);
    color += 155;
  }
}*/

#pragma endregion

void touchscreen_read(lv_indev_t *indev, lv_indev_data_t *data)
{
  uint16_t touchX, touchY;
  if (tft.getTouch(&touchX, &touchY))
  {
    // We need to do some coordinate translation here as LVGL cannot handle
    // tftespi coordinates literally.
    data->point.x = TFT_HOR_RES - touchY;
    data->point.y = touchX;
    data->state = LV_INDEV_STATE_PRESSED;

    // Debug
    Serial.print("Touch Data - X: ");
    Serial.print(touchX);
    Serial.print(" Y: ");
    Serial.println(touchY);

    if (lv_screen_active() == scr_canvas)
    {
      // Draw on canvas
      lv_point_t p;
      Serial.print("Translated Touch Data - X: ");
      Serial.print((TFT_HOR_RES - touchY));
      Serial.print(" Y: ");
      Serial.println(touchX);
      Serial.println("Drawing on canvas...");
      lv_canvas_set_px(canvasObj, touchX, touchY, lv_color_hex(0xffff00), LV_OPA_100);
    }
  }
  else
  {
    data->state = LV_INDEV_STATE_RELEASED;
  }
}

void lv_switch_screen(screen_id_t id)
{

  Serial.println("Switching screens...");
  // Identify previous screen and clean up to free memory.
  previous_scr = lv_screen_active();

  switch (id)
  {
  case SCREEN_WELCOME:
    lv_create_welcome_screen();
    break;
  case SCREEN_NETWORK_SETTINGS:
    Serial.println("Create networking settings");
    lv_create_network_settings();
    break;
  case SCREEN_SYSTEM_SETTINGS:
    Serial.println("System settings screen loaded");
    break;
  case SCREEN_CANVAS:
    Serial.println("Create canvas screen");
    lv_create_canvas();
    break;
  default:
    Serial.println("Unknown screen ID!");
    break;
  }

  // Set previous screen pointer to current.
  if (previous_scr != NULL)
  {
    Serial.println("Cleaning previous screen...");
    lv_obj_clean(previous_scr);
  }
  previous_scr = lv_screen_active();
}

void lv_create_canvas()
{
  // Load screen.
  scr_canvas = lv_obj_create(NULL);
  lv_scr_load(scr_canvas);

  /*
  // Setup malloc draw buffer for canvas.
  lv_draw_buf_t * canvas_draw_buf;
  canvas_draw_buf = lv_draw_buf_create(TFT_HOR_RES, TFT_VER_RES, LV_COLOR_FORMAT_RGB565, 0);
  */

  
  //Setup static draw buffer for canvas. We know this works!!
  //LV_DRAW_BUF_DEFINE_STATIC(canvas_draw_buf, 50, 50, LV_COLOR_FORMAT_RGB565);
  //LV_DRAW_BUF_INIT_STATIC(canvas_draw_buf);
  //Serial.println("Created draw buffer.");
  
  Serial.print("Free Heap Mem: ");
  Serial.println(ESP.getFreeHeap());
  // Set up dynamically allocated draw buffer.
  lv_color_t * canvas_draw_buf_data = (lv_color_t*)malloc(CANVAS_DRAW_BUF_SIZE);
  lv_draw_buf_t * canvas_draw_buf = (lv_draw_buf_t *)malloc(sizeof(lv_draw_buf_t));
  lv_draw_buf_init(
    canvas_draw_buf, 
    50, 
    50, 
    LV_COLOR_FORMAT_I4,
    0, 
    canvas_draw_buf_data, 
    CANVAS_DRAW_BUF_SIZE);

    /*Create a canvas and initialize its palette*/
    lv_obj_t * canvas = lv_canvas_create(lv_screen_active());
    // Static 
    //lv_canvas_set_draw_buf(canvas, &canvas_draw_buf);
    // malloc
    lv_canvas_set_draw_buf(canvas, canvas_draw_buf);
    lv_canvas_set_palette(canvas, 1, LV_COLOR_MAKE(0xFF,0x00,0x00));
    //lv_obj_set_size(canvas, TFT_HOR_RES, TFT_VER_RES); //GPT-ism, might not be needed.s
    lv_canvas_fill_bg(canvas, lv_color_make(0, 0, 1), LV_OPA_COVER);
    lv_obj_center(canvas);

    lv_layer_t layer;
    lv_canvas_init_layer(canvas, &layer);

    lv_draw_line_dsc_t dsc;
    lv_draw_line_dsc_init(&dsc);
    dsc.color = lv_palette_main(LV_PALETTE_RED);
    dsc.width = 4;
    dsc.round_end = 1;
    dsc.round_start = 1;
    dsc.p1.x = 15;
    dsc.p1.y = 15;
    dsc.p2.x = 35;
    dsc.p2.y = 10;
    lv_draw_line(&layer, &dsc);

    lv_canvas_finish_layer(canvas, &layer);

    canvasObj = canvas;
    Serial.print("Free Heap Mem: ");
    Serial.println(ESP.getFreeHeap());
}

void lv_create_network_settings()
{
  scr_networksettings = lv_obj_create(NULL);
  lv_scr_load(scr_networksettings);
  Serial.println("Created object");
  // Create a text label aligned center on top ("Hello, world!")
  lv_obj_t *text_label = lv_label_create(lv_screen_active());
  lv_label_set_long_mode(text_label, LV_LABEL_LONG_WRAP); // Breaks the long lines
  lv_obj_set_style_text_font(text_label, &lv_font_montserrat_30, 0);
  lv_label_set_text(text_label, "Welcome to Settingsbob.");
  lv_obj_set_width(text_label, 400); // Set smaller width to make the lines wrap
  lv_obj_set_style_text_align(text_label, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_align(text_label, LV_ALIGN_CENTER, 0, -110);

  // Instantiate btl label object
  lv_obj_t *btn_label;
  // Create a Button (btn1)
  lv_obj_t *btn1 = lv_button_create(lv_screen_active());
  lv_obj_add_event_cb(btn1, event_handler_scr_network_back, LV_EVENT_ALL, NULL);
  lv_obj_align(btn1, LV_ALIGN_CENTER, 0, -50);
  lv_obj_remove_flag(btn1, LV_OBJ_FLAG_PRESS_LOCK);

  btn_label = lv_label_create(btn1);
  lv_label_set_text(btn_label, "GET ME OUT");
  lv_obj_center(btn_label);

  Serial.print("Free Heap RAM:");
  Serial.println(ESP.getFreeHeap());
}

void lv_create_welcome_screen()
{
  scr_welcome = lv_obj_create(NULL);
  lv_scr_load(scr_welcome);
  // Create a text label aligned center on top ("Hello, world!")
  lv_obj_t *text_label = lv_label_create(lv_screen_active());
  lv_label_set_long_mode(text_label, LV_LABEL_LONG_WRAP); // Breaks the long lines
  lv_obj_set_style_text_font(text_label, &lv_font_montserrat_30, 0);
  lv_label_set_text(text_label, "Welcome to FriendBox!");
  lv_obj_set_width(text_label, 400); // Set smaller width to make the lines wrap
  lv_obj_set_style_text_align(text_label, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_align(text_label, LV_ALIGN_CENTER, 0, -110);

  // Instantiate btl label object
  lv_obj_t *btn_label;
  // Create a Button (btn1)
  lv_obj_t *btn1 = lv_button_create(lv_screen_active());
  lv_obj_add_event_cb(btn1, event_handler_scr_welcome_login, LV_EVENT_ALL, NULL);
  lv_obj_align(btn1, LV_ALIGN_CENTER, 0, -50);
  lv_obj_remove_flag(btn1, LV_OBJ_FLAG_PRESS_LOCK);

  btn_label = lv_label_create(btn1);
  lv_label_set_text(btn_label, "Login");
  lv_obj_center(btn_label);

  // Create network settings button (btn2)
  lv_obj_t *btn2 = lv_button_create(lv_screen_active());
  lv_obj_add_event_cb(btn2, event_handler_scr_welcome_netsettings, LV_EVENT_ALL, NULL);
  lv_obj_align(btn2, LV_ALIGN_CENTER, 0, 10);
  lv_obj_remove_flag(btn2, LV_OBJ_FLAG_PRESS_LOCK);

  btn_label = lv_label_create(btn2);
  lv_label_set_text(btn_label, "Network Settings");
  lv_obj_center(btn_label);

  // Create system settings button (btn3)
  lv_obj_t *btn3 = lv_button_create(lv_screen_active());
  lv_obj_add_event_cb(btn3, event_handler_scr_welcome_syssettings, LV_EVENT_ALL, NULL);
  lv_obj_align(btn3, LV_ALIGN_CENTER, 0, 60);
  lv_obj_remove_flag(btn3, LV_OBJ_FLAG_PRESS_LOCK);

  btn_label = lv_label_create(btn3);
  lv_label_set_text(btn_label, "System Settings");
  lv_obj_center(btn_label);

  // Create a shutdown Button (btn4)
  lv_obj_t *btn4 = lv_button_create(lv_screen_active());
  lv_obj_add_event_cb(btn4, event_handler_shutdown, LV_EVENT_ALL, NULL);
  lv_obj_align(btn4, LV_ALIGN_CENTER, 0, 115);
  lv_obj_remove_flag(btn4, LV_OBJ_FLAG_PRESS_LOCK);

  btn_label = lv_label_create(btn4);
  lv_label_set_text(btn_label, "Go to Canvas");
  lv_obj_center(btn_label);
}

void initNetwork(const char *netSSID, const char *netPassword, const char *hostname)
{
  tft.print("Connecting to ");
  tft.println(netSSID);
  WiFi.setHostname(hostname);
  WiFi.begin(netSSID, netPassword);
  while (WiFi.status() != WL_CONNECTED)
  {
    delay(500);
    tft.print(".");
  }
  tft.println("");
  tft.print("Connected to ");
  tft.println(netSSID);
  tft.print("IP Address: ");
  tft.print(WiFi.localIP());
  // What if the network cannot ever connect? How do we handle?
}

void initSD(int pin)
{

  sdspi.begin(14 /* SCK */, 32 /* MISO */, 13 /* MOSI */, 26 /* SS */);

  if (!SD.begin(pin, sdspi))
  {
    Serial.println("Card Mount Failed");
    return;
  }
  else
  {
    Serial.println("SD Card initialized.");
  }

  // open the file. note that only one file can be open at a time,
  // so you have to close this one before opening another.
  testFile = SD.open("/test.txt", FILE_WRITE);

  // if the file opened okay, write to it:
  if (testFile)
  {
    Serial.print("Writing to test.txt...");
    testFile.println("testing 1, 2, 3. Fuck ben and ");
    // close the file:
    testFile.close();
    Serial.println("done.");
  }
  else
  {
    // if the file didn't open, print an error:
    Serial.println("error opening test.txt");
  }

  // re-open the file for reading:
  testFile = SD.open("/test.txt");
  if (testFile)
  {
    Serial.println("/test.txt:");

    // read from the file until there's nothing else in it:
    while (testFile.available())
    {
      Serial.write(testFile.read());
    }
    // close the file:
    testFile.close();
  }
  else
  {
    // if the file didn't open, print an error:
    Serial.println("error opening test.txt");
  }
}

void initDisplay()
{
  // Gimme visuals
  Serial.println("Initializing TFT_eSPI...");
  tft.init();
  tft.fillScreen(TFT_BLACK);

  Serial.println("Initializing LVGL...");
  lv_init();
  lv_tick_set_cb(systemTick);
  disp = lv_tft_espi_create(TFT_HOR_RES, TFT_VER_RES, draw_buf, sizeof(draw_buf));
  tft.setRotation(3); // Duplciate of following command, but might be needed?
  lv_display_set_rotation(disp, TFT_ROTATION);

  // Gimme text
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.setCursor(0, 20, 2);
  tft.setTextSize(2);
  tft.println("Welcome to FriendBox.");
  tft.println("Please wait...");

  // Gimme touch
  uint16_t calibrationData[5];
  uint8_t calDataOK = 0;
  // check file system
  // Format regardless.
  // SPIFFS.format();
  if (!SPIFFS.begin())
  {
    Serial.println("No SPIFFS exists. Formatting file system...");
    SPIFFS.format();
    SPIFFS.begin();
  }

  // check if calibration file exists
  if (SPIFFS.exists(CALIBRATION_FILE))
  {
    File f = SPIFFS.open(CALIBRATION_FILE, "r");
    if (f)
    {
      if (f.readBytes((char *)calibrationData, 14) == 14)
        calDataOK = 1;
      f.close();
    }
  }
  if (calDataOK)
  {
    // calibration data valid
    tft.setTouch(calibrationData);
    Serial.println("Touch calibration data valid. Proceeding.");
  }
  else
  {
    // data not valid. recalibrate
    tft.calibrateTouch(calibrationData, TFT_WHITE, TFT_RED, 15);
    // store data
    File f = SPIFFS.open(CALIBRATION_FILE, "w");
    if (f)
    {
      f.write((const unsigned char *)calibrationData, 14);
      f.close();
    }
  }
  lv_indev_t *indev = lv_indev_create();
  lv_indev_set_type(indev, LV_INDEV_TYPE_POINTER);
  lv_indev_set_read_cb(indev, touchscreen_read);
}

void setup()
{
  // cawkins was here
  Serial.begin(115200);
  initDisplay();
  initSD(26);
  // initNetwork(NETWORK_SSID, NETWORK_PASS, LOCAL_HOSTNAME);
  lv_switch_screen(SCREEN_WELCOME);
}

void loop()
{
  // handleTouch();
  // Serial.println(tft.readPixel(100, 100));
  lv_timer_handler(); // let the GUI do its work
}

/*use Arduinos millis() as tick source*/
static uint32_t systemTick(void)
{
  return millis();
}
