/*
 * ============================================================
 *  TXT 电子书阅读器  -  ESP32-S3R8 + ST7735 + SD + W25Q128
 * ============================================================
 *
 * 硬件配置:
 *   MCU:      ESP32-S3R8 (双核 240MHz, 8MB PSRAM, 16MB Flash)
 *   屏幕:     ST7735 1.8" TFT 128×160 (软件SPI)
 *   SD卡:     Micro SD 模块 (独立硬件SPI)
 *   外部Flash: W25Q128 16MB (独立bit-bang SPI)
 *   按键:     5个 (上一页/下一页/返回退出/菜单/保存进度)
 *   用户LED:  GPIO48
 *
 * 功能:
 *   - SD卡根目录扫描 .txt 文件
 *   - UTF-8 中文文本分页阅读 (12×12 点阵中文字体)
 *   - 翻页/返回/菜单（亮度调节、跳转页码）
 *   - 阅读进度保存至 W25Q128 外部Flash
 *   - 开机自动恢复上次阅读位置
 *
 * 依赖库 (Arduino库管理器安装):
 *   - U8g2  (by oliver)      → 显示驱动 + 中文字体
 *   - SD    (by Arduino)     → SD卡文件读取
 *
 * ============================================================
 */

#include <Adafruit_GFX.h>
#include <Adafruit_ST7735.h>
#include <U8g2_for_Adafruit_GFX.h>
#include <SPI.h>
#include <SD.h>
#include <FS.h>
#include "esp_heap_caps.h"  // PSRAM malloc

// ============================================================
// 1. 引脚定义
// ============================================================

// ── TFT ST7735 (U8g2 软件SPI) ──  (恢复旧引脚)
#define PIN_TFT_SCK   12
#define PIN_TFT_MOSI  11
#define PIN_TFT_CS    10
#define PIN_TFT_DC    9
#define PIN_TFT_RST   8
#define PIN_TFT_BLK   7

// ── SD Card (独立硬件SPI) ──
#define PIN_SD_SCK    41
#define PIN_SD_MISO   42
#define PIN_SD_MOSI   15
#define PIN_SD_CS     16

// ── W25Q128 Flash (独立bit-bang SPI) ──
#define PIN_FLASH_SCK  18
#define PIN_FLASH_MISO 21
#define PIN_FLASH_MOSI 17
#define PIN_FLASH_CS   40

// ── 5个按键 (按下为LOW，使用内部上拉) ──
#define PIN_BTN_PREV  1   // 上一页
#define PIN_BTN_NEXT  2   // 下一页
#define PIN_BTN_BACK  3   // 返回/退出
#define PIN_BTN_MENU  4   // 菜单
#define PIN_BTN_SAVE  6   // 保存进度

// ── 板载LED ──
#define PIN_LED       48

// ============================================================
// 2. 常量定义
// ============================================================

#define SCREEN_W       128
#define SCREEN_H       160
#define FONT_H         12           // wqy12 字体实际像素高度
#define LINE_SPACING   2            // 行间距
#define LINE_H         (FONT_H + LINE_SPACING)  // 每行占14px
#define LINES_PER_PAGE 10           // (160-16标题栏)/14 = 10行，余4px边距
#define CJK_CHARS_PER_LINE 10       // 128px/12px ≈ 10个全角字
#define DISPLAY_UNITS_PER_LINE 20   // 每行显示单位 (CJK=2, ASCII=1)
#define MAX_FILES      60           // 最多扫描文件数
#define MAX_FILENAME   128         // 文件名最大长度
#define MAX_PAGES      10000       // 最大页数 (偏移量数组容量)
#define DEBOUNCE_MS    40          // 按键去抖时间
#define LONG_PRESS_MS  500         // 长按判定时间 (0.5秒)
#define MENU_ITEM_COUNT 3          // 菜单项数

// W25Q128 命令
#define FLASH_CMD_WREN    0x06
#define FLASH_CMD_WRDI    0x04
#define FLASH_CMD_RDSR    0x05
#define FLASH_CMD_WRSR    0x01
#define FLASH_CMD_READ    0x03
#define FLASH_CMD_FAST_RD 0x0B
#define FLASH_CMD_PP      0x02      // Page Program (256字节)
#define FLASH_CMD_SE      0x20      // Sector Erase (4KB)
#define FLASH_CMD_BE32    0x52      // Block Erase 32KB
#define FLASH_CMD_BE64    0xD8      // Block Erase 64KB
#define FLASH_CMD_CE      0xC7      // Chip Erase
#define FLASH_CMD_RDID    0x9F      // Read JEDEC ID

// 进度保存魔数
#define PROGRESS_MAGIC   0x424F4F4B  // "BOOK"

// ============================================================
// 3. Adafruit ST7735 显示对象 + U8g2 字体支持
// ============================================================
Adafruit_ST7735 tft = Adafruit_ST7735(
  PIN_TFT_CS,          // 片选
  PIN_TFT_DC,          // 数据/命令
  PIN_TFT_RST          // 复位
);
U8G2_FOR_ADAFRUIT_GFX u8f;

// ============================================================
// LEDC PWM 背光控制
// ============================================================
#define BLK_FREQ     5000
#define BLK_RES      8    // 8位分辨率 (0-255)

// ESP32 Arduino 3.x 使用新的 ledcAttach API
#if ESP_ARDUINO_VERSION >= ESP_ARDUINO_VERSION_VAL(3, 0, 0)
  #define LEDC_INIT(pin, freq, res)  ledcAttach(pin, freq, res)
#else
  #define LEDC_CHANNEL  0
  #define LEDC_INIT(pin, freq, res)  do { \
    ledcSetup(LEDC_CHANNEL, freq, res);  \
    ledcAttachPin(pin, LEDC_CHANNEL);    \
  } while(0)
#endif

// ============================================================
// 4. W25Q128 Flash 驱动 (bit-bang SPI)
// ============================================================
class W25Q128Driver {
private:
  uint8_t _cs, _sck, _mosi, _miso;

  void csLow()  { digitalWrite(_cs, LOW);  delayMicroseconds(1); }
  void csHigh() { digitalWrite(_cs, HIGH); delayMicroseconds(1); }

  uint8_t transfer(uint8_t data) {
    uint8_t rx = 0;
    for (int i = 7; i >= 0; i--) {
      digitalWrite(_mosi, (data >> i) & 0x01);
      digitalWrite(_sck, HIGH);
      if (digitalRead(_miso)) rx |= (1 << i);
      digitalWrite(_sck, LOW);
    }
    return rx;
  }

  void waitBusy() {
    csLow();
    transfer(FLASH_CMD_RDSR);
    while (transfer(0x00) & 0x01) { /* wait */ }
    csHigh();
  }

  void writeEnable() {
    csLow();
    transfer(FLASH_CMD_WREN);
    csHigh();
  }

public:
  void begin(uint8_t cs, uint8_t sck, uint8_t mosi, uint8_t miso) {
    _cs = cs; _sck = sck; _mosi = mosi; _miso = miso;
    pinMode(_cs, OUTPUT); csHigh();
    pinMode(_sck, OUTPUT); digitalWrite(_sck, LOW);
    pinMode(_mosi, OUTPUT); digitalWrite(_mosi, LOW);
    pinMode(_miso, INPUT);
  }

  uint32_t readJEDEC() {
    uint32_t id = 0;
    csLow();
    transfer(FLASH_CMD_RDID);
    id |= ((uint32_t)transfer(0)) << 16;
    id |= ((uint32_t)transfer(0)) << 8;
    id |= transfer(0);
    csHigh();
    return id;
  }

  void readBytes(uint32_t addr, uint8_t* buf, uint32_t len) {
    csLow();
    transfer(FLASH_CMD_READ);
    transfer((addr >> 16) & 0xFF);
    transfer((addr >> 8) & 0xFF);
    transfer(addr & 0xFF);
    for (uint32_t i = 0; i < len; i++) {
      buf[i] = transfer(0x00);
    }
    csHigh();
  }

  void writePage(uint32_t addr, const uint8_t* buf, uint16_t len) {
    if (len > 256) len = 256;
    writeEnable();
    csLow();
    transfer(FLASH_CMD_PP);
    transfer((addr >> 16) & 0xFF);
    transfer((addr >> 8) & 0xFF);
    transfer(addr & 0xFF);
    for (uint16_t i = 0; i < len; i++) {
      transfer(buf[i]);
    }
    csHigh();
    waitBusy();
  }

  void writeBytes(uint32_t addr, const uint8_t* buf, uint32_t len) {
    while (len > 0) {
      uint16_t chunk = (len > 256) ? 256 : (uint16_t)len;
      // 跨页时先擦除
      if ((addr & 0xFF) == 0) {
        eraseSector(addr);
      }
      writePage(addr, buf, chunk);
      addr += chunk;
      buf  += chunk;
      len  -= chunk;
    }
  }

  void eraseSector(uint32_t addr) {
    writeEnable();
    csLow();
    transfer(FLASH_CMD_SE);
    transfer((addr >> 16) & 0xFF);
    transfer((addr >> 8) & 0xFF);
    transfer(addr & 0xFF);
    csHigh();
    waitBusy();
  }

  void eraseChip() {
    writeEnable();
    csLow();
    transfer(FLASH_CMD_CE);
    csHigh();
    waitBusy();
    delay(100);  // 全片擦除需要时间
  }
};

W25Q128Driver flash;

// ============================================================
// 5. 进度/配置数据结构
// ============================================================
struct ProgressData {
  uint32_t magic;           // 魔数 0x424F4F4B
  uint8_t  brightness;      // 背光亮度 0-255
  char     lastFile[128];   // 上次阅读的文件名
  uint32_t lastPage;        // 上次阅读的页码
  uint32_t reserved[5];     // 保留扩展
};
// sizeof(ProgressData) = 4+1+128+4+20 = 157字节，加上padding

#define FLASH_PROGRESS_ADDR 0x000000  // 进度数据存储在 Flash 地址 0

// ============================================================
// 6. 按键状态机
// ============================================================

enum BtnEvent {
  BTN_NONE = 0,
  BTN_CLICK = 1,      // 短按
  BTN_LONG  = 2       // 长按
};

struct BtnTracker {
  uint8_t pin;
  enum { IDLE, DEBOUNCE, PRESSED, HELD } state;
  unsigned long timer;     // 状态切换时间戳
  int  lastRaw;            // 上一次原始电平
  bool eventReady;         // 有待消费的事件
  bool eventLong;          // 事件是长按(true)还是短按(false)
};

#define NUM_BTNS 5
BtnTracker btn[NUM_BTNS];

void btnInit() {
  const uint8_t pins[NUM_BTNS] = {PIN_BTN_PREV, PIN_BTN_NEXT, PIN_BTN_BACK, PIN_BTN_MENU, PIN_BTN_SAVE};
  for (int i = 0; i < NUM_BTNS; i++) {
    btn[i].pin   = pins[i];
    btn[i].state = BtnTracker::IDLE;
    btn[i].timer = 0;
    btn[i].lastRaw = HIGH;
    btn[i].eventReady = false;
    btn[i].eventLong  = false;
    pinMode(btn[i].pin, INPUT_PULLUP);
  }
}

// 每个循环调用一次，更新所有按键状态
void btnUpdateAll() {
  unsigned long now = millis();
  for (int i = 0; i < NUM_BTNS; i++) {
    BtnTracker &b = btn[i];
    int raw = digitalRead(b.pin);  // LOW = 按下

    switch (b.state) {
      case BtnTracker::IDLE:
        if (raw == LOW) {
          b.state = BtnTracker::DEBOUNCE;
          b.timer = now;
        }
        break;

      case BtnTracker::DEBOUNCE:
        if (raw == HIGH) {
          b.state = BtnTracker::IDLE;  // 抖动，退回
        } else if (now - b.timer >= DEBOUNCE_MS) {
          b.state = BtnTracker::PRESSED;
          b.timer = now;
        }
        break;

      case BtnTracker::PRESSED:
        if (raw == HIGH) {
          // 松开 → 短按事件
          b.state = BtnTracker::IDLE;
          b.eventReady = true;
          b.eventLong  = false;
        } else if (now - b.timer >= LONG_PRESS_MS) {
          // 超时 → 长按事件
          b.state = BtnTracker::HELD;
          b.eventReady = true;
          b.eventLong  = true;
        }
        break;

      case BtnTracker::HELD:
        if (raw == HIGH) {
          b.state = BtnTracker::IDLE;
        }
        break;
    }
    b.lastRaw = raw;
  }
}

// 获取按键事件 (0=无事件, 1=短按, 2=长按)，消费后返回0
int btnGetEvent(int index) {
  if (index < 0 || index >= NUM_BTNS) return 0;
  BtnTracker &b = btn[index];
  if (b.eventReady) {
    b.eventReady = false;
    return b.eventLong ? BTN_LONG : BTN_CLICK;
  }
  return BTN_NONE;
}

// 便捷索引
#define BTN_IDX_PREV  0
#define BTN_IDX_NEXT  1
#define BTN_IDX_BACK  2
#define BTN_IDX_MENU  3
#define BTN_IDX_SAVE  4

// ============================================================
// 7. 应用状态机
// ============================================================

enum AppState {
  STATE_INIT,
  STATE_FILE_LIST,
  STATE_READING,
  STATE_MENU,
  STATE_BRIGHTNESS,
  STATE_JUMP_PAGE,
  STATE_EXIT_CONFIRM
};

AppState g_state = STATE_INIT;
AppState g_prevState = STATE_INIT;
bool g_screenDirty = true;   // 屏幕需要重绘标志

// ── 文件列表 ──
String  g_fileList[MAX_FILES];
int     g_fileCount = 0;
int     g_fileSelect = 0;       // 当前高亮文件索引
int     g_fileScroll = 0;       // 列表滚动偏移

// ── 阅读状态 ──
File    g_bookFile;
String  g_bookName;
uint32_t g_currentPage = 0;     // 当前页码 (从0开始)
uint32_t g_totalPages  = 0;     // 总页数
uint32_t *g_pageOffsets = NULL; // 每页起始字节偏移 (PSRAM)
bool     g_bookOpen = false;

// ── 当前页文本缓冲区 ──
#define MAX_PAGE_CHARS 512
char    g_pageText[MAX_PAGE_CHARS + 1];  // 当前页渲染文本
int     g_pageTextLen = 0;
bool    g_pageDirty = true;              // 页面内容已变化，需重新渲染

// ── 菜单 ──
int     g_menuIdx = 0;
const char* g_menuItems[MENU_ITEM_COUNT] = {
  "亮度调节",
  "跳转页码",
  "返回阅读"
};

// ── 亮度 ──
uint8_t g_brightness = 128;  // 0-255

// ── 跳页 ──
int     g_targetPage = 1;     // 用户输入的目标页码

// ── 退出确认 ──
bool    g_exitSelect = false; // false=否 true=是

// ============================================================
// 8. 显示辅助函数
// ============================================================

// 设置背光亮度
void setBrightness(uint8_t val) {
  g_brightness = val;
#if ESP_ARDUINO_VERSION >= ESP_ARDUINO_VERSION_VAL(3, 0, 0)
  ledcWrite(PIN_TFT_BLK, val);
#else
  ledcWrite(LEDC_CHANNEL, val);
#endif
}

// 绘制顶部标题栏
void drawHeader(const char* title) {
  tft.fillRect(0, 0, SCREEN_W, 16, ST7735_BLACK);
  u8f.setForegroundColor(ST7735_WHITE);
  u8f.setFontMode(1);
  u8f.setFont(u8g2_font_wqy12_t_gb2312);
  u8f.drawUTF8(0, 12, title);
  // 分隔线
  tft.drawFastHLine(0, 16, SCREEN_W, ST7735_WHITE);
}

// 在指定位置绘制字符串（处理中英文混合）
void drawStrAt(int x, int y, const char* str) {
  u8f.drawUTF8(x, y, str);
}

// ============================================================
// 9. SD卡文件扫描
// ============================================================

SPIClass sdSpi(HSPI);  // 使用HSPI(SPI2)，独立于TFT和Flash

void sdInit() {
  Serial.println("初始化 SD 卡...");
  sdSpi.begin(PIN_SD_SCK, PIN_SD_MISO, PIN_SD_MOSI, PIN_SD_CS);

  int retry = 3;
  while (retry > 0) {
    if (SD.begin(PIN_SD_CS, sdSpi, 4000000)) {
      Serial.println("SD 卡初始化成功");
      return;
    }
    Serial.printf("SD 卡初始化失败，重试 (%d)...\n", retry);
    delay(500);
    retry--;
  }
  Serial.println("SD 卡初始化失败！");
}

void scanFiles() {
  g_fileCount = 0;
  g_fileSelect = 0;
  g_fileScroll = 0;

  File root = SD.open("/");
  if (!root) {
    Serial.println("无法打开 SD 卡根目录");
    return;
  }

  File entry;
  while ((entry = root.openNextFile()) && g_fileCount < MAX_FILES) {
    if (!entry.isDirectory()) {
      String name = String(entry.name());
      name.toLowerCase();
      if (name.endsWith(".txt")) {
        g_fileList[g_fileCount] = String(entry.name());
        g_fileCount++;
        Serial.printf("  找到: %s\n", entry.name());
      }
    }
    entry.close();
  }
  root.close();

  // 按名称排序
  for (int i = 0; i < g_fileCount - 1; i++) {
    for (int j = i + 1; j < g_fileCount; j++) {
      if (g_fileList[i].compareTo(g_fileList[j]) > 0) {
        String tmp = g_fileList[i];
        g_fileList[i] = g_fileList[j];
        g_fileList[j] = tmp;
      }
    }
  }

  Serial.printf("共找到 %d 个 TXT 文件\n", g_fileCount);
}

// ============================================================
// 10. 进度保存/加载 (W25Q128)
// ============================================================

void saveProgressToFlash() {
  if (!g_bookOpen) return;

  ProgressData pd;
  memset(&pd, 0, sizeof(pd));
  pd.magic      = PROGRESS_MAGIC;
  pd.brightness = g_brightness;
  strncpy(pd.lastFile, g_bookName.c_str(), 127);
  pd.lastPage   = g_currentPage;

  flash.eraseSector(FLASH_PROGRESS_ADDR);
  flash.writeBytes(FLASH_PROGRESS_ADDR, (uint8_t*)&pd, sizeof(pd));

  Serial.printf("进度已保存: %s 第%d页 亮度%d\n",
                pd.lastFile, pd.lastPage + 1, pd.brightness);
}

void loadProgressFromFlash() {
  ProgressData pd;
  flash.readBytes(FLASH_PROGRESS_ADDR, (uint8_t*)&pd, sizeof(pd));

  if (pd.magic == PROGRESS_MAGIC) {
    g_brightness = pd.brightness;
    Serial.printf("读取进度: 文件=%s 页码=%d 亮度=%d\n",
                  pd.lastFile, pd.lastPage + 1, pd.brightness);
    setBrightness(g_brightness);
  } else {
    Serial.println("无有效进度记录，使用默认值");
    g_brightness = 128;
    setBrightness(g_brightness);
  }
}

// 查找上次阅读的文件并尝试恢复
void restoreLastBook() {
  ProgressData pd;
  flash.readBytes(FLASH_PROGRESS_ADDR, (uint8_t*)&pd, sizeof(pd));

  if (pd.magic != PROGRESS_MAGIC) return;

  // 检查文件是否还存在
  bool found = false;
  int idx = 0;
  for (int i = 0; i < g_fileCount; i++) {
    if (g_fileList[i] == pd.lastFile) {
      found = true;
      idx = i;
      break;
    }
  }

  if (found) {
    g_fileSelect = idx;
    // 检查是否需要滚动
    if (g_fileSelect >= g_fileScroll + 8) {
      g_fileScroll = g_fileSelect - 7;
    }
  }
}

// ============================================================
// 11. 文本解析 - UTF-8 字符解码
// ============================================================

// 获取UTF-8字符字节长度
// 返回: 1=ASCII, 2/3/4=UTF-8序列长度, 0=无效
int utf8CharLen(uint8_t byte) {
  if (byte < 0x80) return 1;        // ASCII
  if ((byte & 0xE0) == 0xC0) return 2;
  if ((byte & 0xF0) == 0xE0) return 3;
  if ((byte & 0xF8) == 0xF0) return 4;
  return 0;  // 无效或中间字节
}

// 判断是否为CJK全角字符（简化判断）
bool isCJK(uint8_t byte) {
  // UTF-8 编码的CJK字符以 0xE4-0xE9 开头居多
  return (byte >= 0xE0);
}

// 判断是否为新行符
bool isNewline(uint8_t byte) {
  return (byte == '\n' || byte == '\r');
}

// 是否为空白（空格、制表符）
bool isWhitespace(uint8_t byte) {
  return (byte == ' ' || byte == '\t');
}

// ============================================================
// 12. 分页引擎 - 扫描文件建立页偏移表
// ============================================================

bool buildPageOffsets(File &file) {
  if (g_pageOffsets) {
    free(g_pageOffsets);        // 释放旧数据
    g_pageOffsets = NULL;
  }
  g_pageOffsets = (uint32_t*)ps_malloc(MAX_PAGES * sizeof(uint32_t));
  if (!g_pageOffsets) {
    Serial.println("PSRAM 分配失败！尝试使用普通内存...");
    g_pageOffsets = (uint32_t*)malloc(MAX_PAGES * sizeof(uint32_t));
  }
  if (!g_pageOffsets) {
    Serial.println("内存分配失败！");
    return false;
  }

  file.seek(0);
  int pageCount  = 0;
  int lineCount  = 0;
  int unitCount  = 0;
  uint32_t offset = 0;

  g_pageOffsets[0] = 0;  // 第一页从0开始

  while (file.available() && pageCount < MAX_PAGES) {
    int c = file.read();

    // 跳过 \r
    if (c == '\r') {
      offset++;
      continue;
    }

    // 遇到换行
    if (c == '\n') {
      lineCount++;
      unitCount = 0;
      offset++;
      if (lineCount >= LINES_PER_PAGE) {
        pageCount++;
        if (pageCount < MAX_PAGES) {
          g_pageOffsets[pageCount] = file.position();
        }
        lineCount = 0;
      }
      continue;
    }

    // UTF-8字符
    int clen = utf8CharLen(c);
    if (clen == 0) { offset++; continue; }

    // 跳过UTF-8后续字节
    for (int i = 1; i < clen; i++) {
      if (file.available()) { file.read(); offset++; }
    }

    // 计算显示宽度
    int width = isCJK(c) ? 2 : 1;

    // 先判断：当前行放不下这个字符，就换行/翻页，字符留给下一页
    if (unitCount + width > DISPLAY_UNITS_PER_LINE) {
      lineCount++;
      unitCount = 0;
      if (lineCount >= LINES_PER_PAGE) {
        pageCount++;
        if (pageCount < MAX_PAGES) {
          g_pageOffsets[pageCount] = offset - (clen - 1);  // 指向当前字符首字节（修复末行丢字）
        }
        lineCount = 0;
      }
    }

    unitCount += width;

    offset++;
  }

  g_totalPages = pageCount + 1;  // 至少有1页
  Serial.printf("分页完成: 共 %d 页\n", g_totalPages);

  return true;
}

// ============================================================
// 13. 渲染当前页文本到缓冲区（仅在页面变化时调用）
// ============================================================

void renderCurrentPage() {
  if (!g_bookOpen || !g_bookFile) return;
  if (g_currentPage >= g_totalPages) return;
  if (!g_pageDirty) return;  // 页面未变化，不需要重新渲染

  g_pageDirty = false;

  // 定位到当前页起始位置
  uint32_t startOffset = g_pageOffsets[g_currentPage];
  // 下一页起始位置（用于边界判断）
  uint32_t endOffset = (g_currentPage + 1 < g_totalPages)
                       ? g_pageOffsets[g_currentPage + 1]
                       : g_bookFile.size();

  g_bookFile.seek(startOffset);

  int lineCount = 0;
  int unitCount = 0;
  int bufPos    = 0;

  g_pageText[0] = '\0';

  while (g_bookFile.available() && lineCount < LINES_PER_PAGE && bufPos < MAX_PAGE_CHARS - 4) {
    // 检查是否已经超过当前页边界
    if (g_bookFile.position() >= endOffset) break;

    int c = g_bookFile.read();

    // 跳过 \r
    if (c == '\r') continue;

    // 换行符
    if (c == '\n') {
      if (bufPos < MAX_PAGE_CHARS - 1) g_pageText[bufPos++] = '\n';
      lineCount++;
      unitCount = 0;
      if (lineCount >= LINES_PER_PAGE) break;
      continue;
    }

    int clen = utf8CharLen(c);
    if (clen <= 0) continue;  // 无效字节，跳过

    // 读取完整UTF-8序列
    uint8_t seq[4];
    int actualLen = 0;
    seq[actualLen++] = c;
    for (int i = 1; i < clen; i++) {
      if (g_bookFile.available() && g_bookFile.position() < endOffset) {
        seq[actualLen++] = g_bookFile.read();
      } else {
        break;
      }
    }
    if (actualLen < clen) break;  // 不完整序列，退出

   // 计算宽度
   int width = isCJK(seq[0]) ? 2 : 1;

   // 先判断：当前行放不下这个字符，就换行；本页满则退回字符留给下一页
   if (unitCount + width > DISPLAY_UNITS_PER_LINE) {
       if (bufPos < MAX_PAGE_CHARS - 1) g_pageText[bufPos++] = '\n';
       lineCount++;
       unitCount = 0;
       if (lineCount >= LINES_PER_PAGE) {
           g_bookFile.seek(g_bookFile.position() - actualLen);  // 退回当前字符
           break;
       }
   }

   // 再写入字符
   for (int i = 0; i < actualLen && bufPos < MAX_PAGE_CHARS - 1; i++) {
       g_pageText[bufPos++] = seq[i];
   }
   unitCount += width;
  }

  g_pageText[bufPos] = '\0';
  g_pageTextLen = bufPos;
}

// ============================================================
// 14. 各种界面绘制函数
// ============================================================

// ── 文件列表界面 ──
void drawFileList() {
  tft.fillScreen(ST7735_BLACK);
  u8f.setForegroundColor(ST7735_WHITE);
  u8f.setBackgroundColor(ST7735_BLACK);
  u8f.setFontMode(1);
  
  // 标题
  drawHeader("[文件] TXT电子书");

  if (g_fileCount == 0) {
    u8f.setFont(u8g2_font_wqy12_t_gb2312);
    u8f.drawUTF8(10, 50, "未找到TXT文件");
    u8f.drawUTF8(10, 70, "请放入SD卡后重启");
    return;
  }

  // 文件列表 (每行14px，从y=28开始，显示8行)
  int visibleLines = 8;
  int startY = 2 * FONT_H + 4;  // ~28px
  u8f.setFont(u8g2_font_wqy12_t_gb2312);
  
  for (int i = 0; i < visibleLines && (g_fileScroll + i) < g_fileCount; i++) {
    int idx = g_fileScroll + i;
    int y = startY + i * LINE_H;

    if (idx == g_fileSelect) {
      // 高亮选中项
      tft.fillRect(0, y - FONT_H, SCREEN_W, LINE_H, ST7735_WHITE);
      u8f.setForegroundColor(ST7735_BLACK);
      u8f.drawUTF8(20, y, g_fileList[idx].c_str());
      u8f.setForegroundColor(ST7735_WHITE);
      // 选中箭头
      tft.fillTriangle(2, y - 6, 2, y, 10, y - 3, ST7735_BLACK);
    } else {
      u8f.drawUTF8(14, y, g_fileList[idx].c_str());
    }
  }

  // 底部提示
  int bottomY = SCREEN_H - 2;
  u8f.setFont(u8g2_font_wqy12_t_gb2312);
  u8f.drawUTF8(0, bottomY, "菜单:选择");
}

// ── 阅读界面 ──
void drawReading() {
  renderCurrentPage();

  tft.fillScreen(ST7735_BLACK);
  u8f.setForegroundColor(ST7735_WHITE);
  u8f.setBackgroundColor(ST7735_BLACK);
  u8f.setFontMode(1);
  u8f.setFont(u8g2_font_wqy12_t_gb2312);

  // 顶部标题栏: 文件名 + 页码
  char header[64];
  snprintf(header, sizeof(header), "%s  %u/%u",
           g_bookName.c_str(),
           (unsigned int)(g_currentPage + 1),
           (unsigned int)g_totalPages);
  u8f.drawUTF8(0, 12, header);
  tft.drawFastHLine(0, 16, SCREEN_W, ST7735_WHITE);

  // 正文内容
  int lineCount = 0;
  int lineStart = 0;
  int contentY = 16 + LINE_H;  // 标题栏下方第一行

  for (int i = 0; i <= g_pageTextLen; i++) {
    if (g_pageText[i] == '\n' || g_pageText[i] == '\0') {
      if (lineStart < i && lineCount < LINES_PER_PAGE) {
        int y = contentY + lineCount * LINE_H;
        char ch = g_pageText[i];
        g_pageText[i] = '\0';
        u8f.drawUTF8(0, y, &g_pageText[lineStart]);
        g_pageText[i] = ch;
        lineCount++;
      }
      lineStart = i + 1;
      if (g_pageText[i] == '\0') break;
    }
  }
}

// ── 菜单界面 ──
void drawMenu() {
  tft.fillScreen(ST7735_BLACK);
  u8f.setForegroundColor(ST7735_WHITE);
  u8f.setBackgroundColor(ST7735_BLACK);
  u8f.setFontMode(1);
  u8f.setFont(u8g2_font_wqy12_t_gb2312);

  drawHeader("[菜单] 选项");

  int startY = 2 * FONT_H + 4;
  for (int i = 0; i < MENU_ITEM_COUNT; i++) {
    int y = startY + i * (LINE_H + 4);
    if (i == g_menuIdx) {
      tft.fillRect(4, y - FONT_H, SCREEN_W - 8, LINE_H + 2, ST7735_WHITE);
      u8f.setForegroundColor(ST7735_BLACK);
      u8f.drawUTF8(10, y, g_menuItems[i]);
      u8f.setForegroundColor(ST7735_WHITE);
    } else {
      u8f.drawUTF8(8, y, g_menuItems[i]);
    }
  }

  int bottomY = SCREEN_H - 2;
  u8f.setFont(u8g2_font_wqy12_t_gb2312);
  u8f.drawUTF8(0, bottomY, "上下选择 确认 返回");
}

// ── 亮度调节界面 ──
void drawBrightness() {
  tft.fillScreen(ST7735_BLACK);
  u8f.setForegroundColor(ST7735_WHITE);
  u8f.setBackgroundColor(ST7735_BLACK);
  u8f.setFontMode(1);
  u8f.setFont(u8g2_font_wqy12_t_gb2312);

  drawHeader("[亮度] 背光调节");

  int barY = 60;
  int barW = 100;
  int barH = 16;
  int barX = (SCREEN_W - barW) / 2;

  // 亮度条外框
  tft.drawRect(barX, barY, barW, barH, ST7735_WHITE);
  // 亮度条填充
  int fillW = (int)((long)barW * g_brightness / 255);
  if (fillW > 0) {
    tft.fillRect(barX + 1, barY + 1, fillW, barH - 2, ST7735_WHITE);
  }

  // 百分比
  char pctStr[16];
  snprintf(pctStr, sizeof(pctStr), "%d%%", (int)(g_brightness * 100 / 255));
  u8f.drawUTF8(barX, barY + barH + 16, pctStr);

  // 按键提示
  u8f.setFont(u8g2_font_wqy12_t_gb2312);
  u8f.drawUTF8(0, SCREEN_H - 16, "< 减少    > 增加");
  u8f.drawUTF8(0, SCREEN_H - 4,  "菜单:确认  返回:取消");
}

// ── 跳转页码界面 ──
void drawJumpPage() {
  tft.fillScreen(ST7735_BLACK);
  u8f.setForegroundColor(ST7735_WHITE);
  u8f.setBackgroundColor(ST7735_BLACK);
  u8f.setFontMode(1);
  u8f.setFont(u8g2_font_wqy12_t_gb2312);

  drawHeader("[跳转] 页码");

  char infoStr[64];
  snprintf(infoStr, sizeof(infoStr), "当前: %u / %u",
           (unsigned int)(g_currentPage + 1),
           (unsigned int)g_totalPages);
  u8f.drawUTF8(10, 50, infoStr);

  // 目标页码显示
  u8f.setFont(u8g2_font_wqy12_t_gb2312);
  char buf[32];
  snprintf(buf, sizeof(buf), "跳至: %d", g_targetPage);
  u8f.drawUTF8(10, 80, buf);

  // 操作提示
  u8f.setFont(u8g2_font_wqy12_t_gb2312);
  u8f.drawUTF8(0, SCREEN_H - 24, "<> 调整数值");
  u8f.drawUTF8(0, SCREEN_H - 12, "菜单:确认  返回:取消");
}

// ── 退出确认界面 ──
void drawExitConfirm() {
  tft.fillScreen(ST7735_BLACK);
  u8f.setForegroundColor(ST7735_WHITE);
  u8f.setBackgroundColor(ST7735_BLACK);
  u8f.setFontMode(1);
  u8f.setFont(u8g2_font_wqy12_t_gb2312);

  drawHeader("[!] 退出确认");

  int y = 55;
  u8f.drawUTF8(20, y, "确定退出吗？");
  u8f.drawUTF8(15, y + 20, "进度将自动保存");

  // 是/否
  int btnY = y + 50;
  if (!g_exitSelect) {
    tft.fillRect(20, btnY - 10, 35, 18, ST7735_WHITE);
    u8f.setForegroundColor(ST7735_BLACK);
    u8f.drawUTF8(25, btnY + 2, "是");
    u8f.setForegroundColor(ST7735_WHITE);
    u8f.drawUTF8(75, btnY + 2, "否");
  } else {
    u8f.drawUTF8(25, btnY + 2, "是");
    tft.fillRect(70, btnY - 10, 35, 18, ST7735_WHITE);
    u8f.setForegroundColor(ST7735_BLACK);
    u8f.drawUTF8(75, btnY + 2, "否");
    u8f.setForegroundColor(ST7735_WHITE);
  }

  u8f.setFont(u8g2_font_wqy12_t_gb2312);
  u8f.drawUTF8(0, SCREEN_H - 4, "<> 选择  菜单:确认");
}

// ============================================================
// 15. 文件操作
// ============================================================

void openBook(const char* filename) {
  if (g_bookOpen) {
    g_bookFile.close();
    g_bookOpen = false;
  }

  // 确保文件名以 "/" 开头（兼容 ESP32 SD 库路径格式）
  String path = String(filename);
  if (!path.startsWith("/")) {
    path = "/" + path;
  }

  g_bookFile = SD.open(path.c_str(), FILE_READ);
  if (!g_bookFile) {
    Serial.printf("无法打开文件: %s\n", path.c_str());
    return;
  }

  g_bookName   = path;
  g_bookOpen   = true;
  g_currentPage = 0;

  Serial.printf("打开文件: %s\n", path.c_str());
  Serial.println("正在分页，请稍候...");

  // 显示分页进度提示
  tft.fillScreen(ST7735_BLACK);
  u8f.setForegroundColor(ST7735_WHITE);
  u8f.setFontMode(1);
  u8f.setFont(u8g2_font_wqy12_t_gb2312);
  u8f.drawUTF8(10, 60, "正在分页...");
  u8f.drawUTF8(10, 80, "请稍候");

  // 建立页偏移表
  unsigned long t0 = millis();
  if (!buildPageOffsets(g_bookFile)) {
    Serial.println("分页失败！");
    tft.fillScreen(ST7735_BLACK);
    u8f.drawUTF8(10, 60, "打开失败！");
    u8f.drawUTF8(10, 80, "内存不足");
    delay(1500);
    g_bookFile.close();
    g_bookOpen = false;
    g_screenDirty = true;  // 返回文件列表后重绘
    return;
  }
  unsigned long t1 = millis();
  Serial.printf("分页耗时: %lu ms，共 %d 页\n", t1 - t0, g_totalPages);

  // 恢复上次阅读进度
  ProgressData pd;
  flash.readBytes(FLASH_PROGRESS_ADDR, (uint8_t*)&pd, sizeof(pd));
  if (pd.magic == PROGRESS_MAGIC && strcmp(pd.lastFile, g_bookName.c_str()) == 0) {
    if (pd.lastPage < g_totalPages) {
      g_currentPage = pd.lastPage;
      g_pageDirty = true;
      Serial.printf("恢复上次进度: 第 %d 页\n", g_currentPage + 1);
    }
  }

  g_pageDirty = true;
}

void closeBook() {
  if (g_bookOpen) {
    saveProgressToFlash();
    g_bookFile.close();
    g_bookOpen = false;
    g_bookName = "";
    g_currentPage = 0;
    g_totalPages = 0;
    if (g_pageOffsets) {
      free(g_pageOffsets);
      g_pageOffsets = NULL;
    }
    g_pageDirty = true;
  }
}

// ============================================================
// 16. 事件处理
// ============================================================

// ── 文件列表事件处理 ──
void handleFileList(int event, int btnIdx) {
  switch (btnIdx) {
    case BTN_IDX_PREV:  // 上移选择
      if (event == BTN_CLICK) {
        if (g_fileSelect > 0) {
          g_fileSelect--;
          if (g_fileSelect < g_fileScroll && g_fileScroll > 0) {
            g_fileScroll--;
          }
          g_screenDirty = true;
        }
      }
      break;

    case BTN_IDX_NEXT:  // 下移选择
      if (event == BTN_CLICK) {
        if (g_fileSelect < g_fileCount - 1) {
          g_fileSelect++;
          if (g_fileSelect >= g_fileScroll + 8) {
            g_fileScroll++;
          }
          g_screenDirty = true;
        }
      }
      break;

    case BTN_IDX_MENU:  // 确认选择 / 打开选中文件
      // 短按或长按均触发打开，避免长按判定导致无反应
      if ((event == BTN_CLICK || event == BTN_LONG) && g_fileCount > 0) {
        openBook(g_fileList[g_fileSelect].c_str());
        if (g_bookOpen) {
          g_state = STATE_READING;
          g_screenDirty = true;
        }
      }
      break;

    case BTN_IDX_BACK:  // 显示退出确认
      if (event == BTN_CLICK) {
        g_exitSelect = false;
        g_prevState = STATE_FILE_LIST;
        g_state = STATE_EXIT_CONFIRM;
        g_screenDirty = true;
      }
      break;

    case BTN_IDX_SAVE:  // 文件中无保存操作
      break;
  }
}

// ── 阅读界面事件处理 ──
void handleReading(int event, int btnIdx) {
  // GPIO4 长按 (>=0.5秒) → 直接打开亮度调节菜单
  if (btnIdx == BTN_IDX_MENU && event == BTN_LONG) {
    g_prevState = STATE_READING;
    g_state = STATE_BRIGHTNESS;
    g_screenDirty = true;
    return;
  }

  switch (btnIdx) {
    case BTN_IDX_PREV:  // 上一页
      if (event == BTN_CLICK && g_currentPage > 0) {
        g_currentPage--;
        g_pageDirty = true;
        g_screenDirty = true;
      }
      break;

    case BTN_IDX_NEXT:  // 下一页
      if (event == BTN_CLICK && g_currentPage + 1 < g_totalPages) {
        g_currentPage++;
        g_pageDirty = true;
        g_screenDirty = true;
      }
      break;

    case BTN_IDX_MENU:  // 打开菜单
      if (event == BTN_CLICK) {
        g_menuIdx = 0;
        g_prevState = STATE_READING;
        g_state = STATE_MENU;
        g_screenDirty = true;
      }
      break;

    case BTN_IDX_BACK:  // 返回文件列表
      if (event == BTN_CLICK) {
        closeBook();
        g_state = STATE_FILE_LIST;
        g_screenDirty = true;
      }
      break;

    case BTN_IDX_SAVE:  // 保存进度
      if (event == BTN_CLICK) {
        saveProgressToFlash();
        // 闪烁LED提示
        digitalWrite(PIN_LED, LOW);
        delay(100);
        digitalWrite(PIN_LED, HIGH);
      }
      break;
  }
}

// ── 菜单事件处理 ──
void handleMenu(int event, int btnIdx) {
  // GPIO4 长按 (>=0.5秒) → 直接打开亮度调节菜单
  if (btnIdx == BTN_IDX_MENU && event == BTN_LONG) {
    g_state = STATE_BRIGHTNESS;
    g_screenDirty = true;
    return;
  }

  switch (btnIdx) {
    case BTN_IDX_PREV:  // 上移
      if (event == BTN_CLICK) {
        if (g_menuIdx > 0) {
          g_menuIdx--;
          g_screenDirty = true;
        }
      }
      break;

    case BTN_IDX_NEXT:  // 下移
      if (event == BTN_CLICK) {
        if (g_menuIdx < MENU_ITEM_COUNT - 1) {
          g_menuIdx++;
          g_screenDirty = true;
        }
      }
      break;

    case BTN_IDX_MENU:  // 确认
      if (event == BTN_CLICK) {
        switch (g_menuIdx) {
          case 0:  // 亮度调节
            g_state = STATE_BRIGHTNESS;
            g_screenDirty = true;
            break;
          case 1:  // 跳转页码
            g_targetPage = g_currentPage + 1;
            g_state = STATE_JUMP_PAGE;
            g_screenDirty = true;
            break;
          case 2:  // 返回阅读
            g_state = g_prevState;
            g_screenDirty = true;
            break;
        }
      }
      break;

    case BTN_IDX_BACK:  // 返回
      if (event == BTN_CLICK) {
        g_state = g_prevState;
        g_screenDirty = true;
      }
      break;

    case BTN_IDX_SAVE:
      break;
  }
}

// ── 亮度调节事件处理 ──
void handleBrightness(int event, int btnIdx) {
  switch (btnIdx) {
    case BTN_IDX_PREV:  // 减少亮度
      if (event == BTN_CLICK || event == BTN_LONG) {
        if (g_brightness > 10) {
          g_brightness = (g_brightness >= 20) ? g_brightness - 10 : 0;
        } else {
          g_brightness = 0;
        }
        setBrightness(g_brightness);
        g_screenDirty = true;
      }
      break;

    case BTN_IDX_NEXT:  // 增加亮度
      if (event == BTN_CLICK || event == BTN_LONG) {
        if (g_brightness < 245) {
          g_brightness += 10;
        } else {
          g_brightness = 255;
        }
        setBrightness(g_brightness);
        g_screenDirty = true;
      }
      break;

    case BTN_IDX_MENU:  // 确认退出
      if (event == BTN_CLICK) {
        saveProgressToFlash();
        g_state = g_prevState;
        g_screenDirty = true;
      }
      break;

    case BTN_IDX_BACK:  // 取消退出
      if (event == BTN_CLICK) {
        setBrightness(g_brightness);  // 不保存（亮度已在调节中即时生效）
        g_state = g_prevState;
        g_screenDirty = true;
      }
      break;

    case BTN_IDX_SAVE:
      break;
  }
}

// ── 跳转页码事件处理 ──
void handleJumpPage(int event, int btnIdx) {
  switch (btnIdx) {
    case BTN_IDX_PREV:  // 减小目标页码
      if (event == BTN_CLICK) {
        if (g_targetPage > 1) {
          g_targetPage--;
          g_screenDirty = true;
        }
      }
      break;

    case BTN_IDX_NEXT:  // 增大目标页码
      if (event == BTN_CLICK) {
        if (g_targetPage < (int)g_totalPages) {
          g_targetPage++;
          g_screenDirty = true;
        }
      }
      break;

    case BTN_IDX_MENU:  // 确认跳转
      if (event == BTN_CLICK) {
        if (g_targetPage >= 1 && g_targetPage <= (int)g_totalPages) {
          g_currentPage = g_targetPage - 1;
          g_pageDirty = true;
          g_state = STATE_READING;
          g_screenDirty = true;
        }
      }
      break;

    case BTN_IDX_BACK:  // 取消
      if (event == BTN_CLICK) {
        g_state = STATE_READING;
        g_screenDirty = true;
      }
      break;

    case BTN_IDX_SAVE:
      break;
  }
}

// ── 退出确认事件处理 ──
void handleExitConfirm(int event, int btnIdx) {
  switch (btnIdx) {
    case BTN_IDX_PREV:  // 切换选择
      if (event == BTN_CLICK) {
        g_exitSelect = false;
        g_screenDirty = true;
      }
      break;

    case BTN_IDX_NEXT:  // 切换选择
      if (event == BTN_CLICK) {
        g_exitSelect = true;
        g_screenDirty = true;
      }
      break;

    case BTN_IDX_MENU:  // 确认
      if (event == BTN_CLICK) {
        if (!g_exitSelect) {
          // 选"是"：退出
          closeBook();
          Serial.println("正在退出...");
          // 实际上ESP32会进入深度睡眠或循环等待
          // 这里我们显示提示后进入空循环
          tft.fillScreen(ST7735_BLACK);
          u8f.setForegroundColor(ST7735_WHITE);
          u8f.setFontMode(1);
          u8f.setFont(u8g2_font_wqy12_t_gb2312);
          u8f.drawUTF8(20, 80, "可以安全断电");
          delay(2000);
          // 进入深度睡眠
          esp_deep_sleep_start();
        } else {
          // 选"否"：返回
          g_state = g_prevState;
          g_screenDirty = true;
        }
      }
      break;

    case BTN_IDX_BACK:  // 返回
      if (event == BTN_CLICK) {
        g_state = g_prevState;
        g_screenDirty = true;
      }
      break;

    case BTN_IDX_SAVE:
      break;
  }
}

// ============================================================
// 17. 初始化
// ============================================================

void setup() {
  Serial.begin(115200);
  delay(500);
  Serial.println("\n\n=== TXT 电子书阅读器 启动 ===");

  // ── LED ──
  pinMode(PIN_LED, OUTPUT);
  digitalWrite(PIN_LED, LOW);

  // ── 背光 PWM ──
  LEDC_INIT(PIN_TFT_BLK, BLK_FREQ, BLK_RES);
  setBrightness(128);

  // ── 显示 ──
  Serial.println("初始化显示屏...");
  tft.initR(INITR_BLACKTAB);
  tft.setRotation(2);   // 旋转180°（2 = 180°，1 = 90°，3 = 270°）
  // 旋转后显式确认实际宽高
  Serial.printf("屏幕实际尺寸: %d x %d\n", tft.width(), tft.height());
  tft.fillScreen(ST7735_BLACK);
  u8f.begin(tft);
  u8f.setFont(u8g2_font_wqy12_t_gb2312);

  // 开机画面
  tft.fillScreen(ST7735_BLACK);
  u8f.setForegroundColor(ST7735_WHITE);
  u8f.setFontMode(1);
  u8f.setFont(u8g2_font_wqy12_t_gb2312);
  u8f.drawUTF8(20, 40, "TXT电子书");
  u8f.drawUTF8(30, 60, "阅读器 V1.5");
  u8f.setFont(u8g2_font_wqy12_t_gb2312);
  u8f.drawUTF8(15, 100, "正在初始化...");

  // ── SD卡 ──
  sdInit();

  // ── 外部Flash ──
  Serial.println("初始化 W25Q128 Flash...");
  flash.begin(PIN_FLASH_CS, PIN_FLASH_SCK, PIN_FLASH_MOSI, PIN_FLASH_MISO);
  uint32_t jedec = flash.readJEDEC();
  Serial.printf("Flash JEDEC ID: 0x%06X\n", jedec);

  // ── 按键 ──
  btnInit();

  // ── 加载进度 ──
  loadProgressFromFlash();

  // ── 扫描文件 ──
  tft.fillScreen(ST7735_BLACK);
  u8f.setForegroundColor(ST7735_WHITE);
  u8f.setFontMode(1);
  u8f.setFont(u8g2_font_wqy12_t_gb2312);
  u8f.drawUTF8(20, 40, "TXT电子书");
  u8f.drawUTF8(30, 60, "阅读器 V1.0");
  u8f.setFont(u8g2_font_wqy12_t_gb2312);
  u8f.drawUTF8(10, 100, "正在扫描文件...");

  scanFiles();

  // ── 恢复上次书目 ──
  restoreLastBook();

  // ── 进入文件列表 ──
  g_state = STATE_FILE_LIST;
  digitalWrite(PIN_LED, HIGH);

  Serial.println("=== 初始化完成 ===");
}

// ============================================================
// 18. 主循环
// ============================================================

void loop() {
  // 更新所有按键状态
  btnUpdateAll();

  // 检查按键事件
  for (int i = 0; i < NUM_BTNS; i++) {
    int evt = btnGetEvent(i);
    if (evt == BTN_NONE) continue;

    // 分发事件
    switch (g_state) {
      case STATE_FILE_LIST:   handleFileList(evt, i);   break;
      case STATE_READING:     handleReading(evt, i);     break;
      case STATE_MENU:        handleMenu(evt, i);        break;
      case STATE_BRIGHTNESS:  handleBrightness(evt, i);  break;
      case STATE_JUMP_PAGE:   handleJumpPage(evt, i);   break;
      case STATE_EXIT_CONFIRM:handleExitConfirm(evt, i); break;
      default: break;
    }
  }

  // 刷新显示（仅在需要时）
  if (g_screenDirty) {
    switch (g_state) {
      case STATE_FILE_LIST:   drawFileList();    break;
      case STATE_READING:     drawReading();      break;
      case STATE_MENU:        drawMenu();         break;
      case STATE_BRIGHTNESS:  drawBrightness();   break;
      case STATE_JUMP_PAGE:   drawJumpPage();     break;
      case STATE_EXIT_CONFIRM:drawExitConfirm();  break;
      default: break;
    }
    g_screenDirty = false;
  }

  delay(10);  // 轻量延时，降低CPU占用
}
