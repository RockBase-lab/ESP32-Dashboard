#include "epd_ssd1619a_420c.h"

// SSD1619A (Holitech HINK-E042A07 / HINK-E042A13, 4.2" 400x300 B/W/R).
// Init/refresh sequence taken from the field-tested SSD1619A reference driver
// (createskyblue/ssd1619a-epd, verified on HINK-E042A13-A0).
// Data planes: 0x24 = black/white, 0x26 = red.

GxEPD2_420c_NM_SSD1619A::GxEPD2_420c_NM_SSD1619A(int16_t cs, int16_t dc, int16_t rst, int16_t busy)
    : GxEPD2_EPD(cs, dc, rst, busy, HIGH, 25000000, WIDTH, HEIGHT,
                 panel, hasColor, hasPartialUpdate, hasFastPartialUpdate)
{
}

void GxEPD2_420c_NM_SSD1619A::clearScreen(uint8_t value) { clearScreen(value, 0xFF); }

void GxEPD2_420c_NM_SSD1619A::clearScreen(uint8_t black_value, uint8_t color_value)
{
    writeScreenBuffer(black_value, color_value);
    refresh(false);
}

void GxEPD2_420c_NM_SSD1619A::writeScreenBuffer(uint8_t value) { writeScreenBuffer(value, 0xFF); }

void GxEPD2_420c_NM_SSD1619A::writeScreenBuffer(uint8_t black_value, uint8_t color_value)
{
    if (!_init_display_done) _InitDisplay();
    _writeScreenBuffer(0x24, black_value);
    _writeScreenBuffer(0x26, color_value);
    _initial_write = false;
}

void GxEPD2_420c_NM_SSD1619A::_writeScreenBuffer(uint8_t command, uint8_t value)
{
    _setPartialRamArea(0, 0, WIDTH, HEIGHT);
    _writeCommand(command);
    _startTransfer();
    for (uint32_t i = 0; i < uint32_t(WIDTH) * uint32_t(HEIGHT) / 8; i++) _transfer(value);
    _endTransfer();
}

void GxEPD2_420c_NM_SSD1619A::writeImage(const uint8_t bitmap[], int16_t x, int16_t y,
                                         int16_t w, int16_t h, bool invert, bool mirror_y, bool pgm)
{
    _writeScreenBuffer(0x26, 0xFF);  // keep red plane white
    _writeImage(0x24, bitmap, x, y, w, h, invert, mirror_y, pgm);
}

void GxEPD2_420c_NM_SSD1619A::_writeImage(uint8_t command, const uint8_t bitmap[],
                                          int16_t x, int16_t y, int16_t w, int16_t h,
                                          bool invert, bool mirror_y, bool pgm)
{
    if (_initial_write) writeScreenBuffer();
    delay(1);
    uint16_t wb = (w + 7) / 8;
    x -= x % 8;
    w = wb * 8;
    int16_t x1 = x < 0 ? 0 : x;
    int16_t y1 = y < 0 ? 0 : y;
    int16_t w1 = x + w < int16_t(WIDTH) ? w : int16_t(WIDTH) - x;
    int16_t h1 = y + h < int16_t(HEIGHT) ? h : int16_t(HEIGHT) - y;
    int16_t dx = x1 - x;
    int16_t dy = y1 - y;
    w1 -= dx;
    h1 -= dy;
    if ((w1 <= 0) || (h1 <= 0)) return;
    if (!_init_display_done) _InitDisplay();
    _setPartialRamArea(x1, y1, w1, h1);
    _writeCommand(command);
    _startTransfer();
    for (int16_t i = 0; i < h1; i++) {
        for (int16_t j = 0; j < w1 / 8; j++) {
            uint16_t idx = mirror_y
                ? j + dx / 8 + uint16_t((h1 - 1 - i)) * wb
                : j + dx / 8 + uint16_t(i + dy) * wb;
            uint8_t data = pgm ? pgm_read_byte(&bitmap[idx]) : bitmap[idx];
            if (invert) data = ~data;
            _transfer(data);
        }
    }
    _endTransfer();
    delay(1);
}

void GxEPD2_420c_NM_SSD1619A::writeImagePart(const uint8_t bitmap[],
        int16_t x_part, int16_t y_part, int16_t w_bitmap, int16_t h_bitmap,
        int16_t x, int16_t y, int16_t w, int16_t h, bool invert, bool mirror_y, bool pgm)
{
    _writeImagePart(0x24, bitmap, x_part, y_part, w_bitmap, h_bitmap, x, y, w, h,
                    invert, mirror_y, pgm);
}

void GxEPD2_420c_NM_SSD1619A::_writeImagePart(uint8_t command, const uint8_t bitmap[],
        int16_t x_part, int16_t y_part, int16_t w_bitmap, int16_t h_bitmap,
        int16_t x, int16_t y, int16_t w, int16_t h, bool invert, bool mirror_y, bool pgm)
{
    if (_initial_write) writeScreenBuffer();
    delay(1);
    if ((w_bitmap < 0) || (h_bitmap < 0) || (w < 0) || (h < 0)) return;
    if ((x_part < 0) || (x_part >= w_bitmap)) return;
    if ((y_part < 0) || (y_part >= h_bitmap)) return;
    uint16_t wb_bitmap = (w_bitmap + 7) / 8;
    x_part -= x_part % 8;
    w = w_bitmap - x_part < w ? w_bitmap - x_part : w;
    h = h_bitmap - y_part < h ? h_bitmap - y_part : h;
    x -= x % 8;
    w = 8 * ((w + 7) / 8);
    int16_t x1 = x < 0 ? 0 : x;
    int16_t y1 = y < 0 ? 0 : y;
    int16_t w1 = x + w < int16_t(WIDTH) ? w : int16_t(WIDTH) - x;
    int16_t h1 = y + h < int16_t(HEIGHT) ? h : int16_t(HEIGHT) - y;
    int16_t dx = x1 - x;
    int16_t dy = y1 - y;
    w1 -= dx;
    h1 -= dy;
    if ((w1 <= 0) || (h1 <= 0)) return;
    if (!_init_display_done) _InitDisplay();
    _setPartialRamArea(x1, y1, w1, h1);
    _writeCommand(command);
    _startTransfer();
    for (int16_t i = 0; i < h1; i++) {
        for (int16_t j = 0; j < w1 / 8; j++) {
            uint32_t idx = mirror_y
                ? x_part / 8 + j + dx / 8 + uint32_t((h_bitmap - 1 - (y_part + i + dy))) * wb_bitmap
                : x_part / 8 + j + dx / 8 + uint32_t(y_part + i + dy) * wb_bitmap;
            uint8_t data = pgm ? pgm_read_byte(&bitmap[idx]) : bitmap[idx];
            if (invert) data = ~data;
            _transfer(data);
        }
    }
    _endTransfer();
    delay(1);
}

void GxEPD2_420c_NM_SSD1619A::writeImage(const uint8_t* black, const uint8_t* color,
        int16_t x, int16_t y, int16_t w, int16_t h, bool invert, bool mirror_y, bool pgm)
{
    if (black) _writeImage(0x24, black, x, y, w, h, invert, mirror_y, pgm);
    if (color) _writeImage(0x26, color, x, y, w, h, !invert, mirror_y, pgm);
}

void GxEPD2_420c_NM_SSD1619A::writeImagePart(const uint8_t* black, const uint8_t* color,
        int16_t x_part, int16_t y_part, int16_t w_bitmap, int16_t h_bitmap,
        int16_t x, int16_t y, int16_t w, int16_t h, bool invert, bool mirror_y, bool pgm)
{
    if (black) _writeImagePart(0x24, black, x_part, y_part, w_bitmap, h_bitmap, x, y, w, h,
                               invert, mirror_y, pgm);
    if (color) _writeImagePart(0x26, color, x_part, y_part, w_bitmap, h_bitmap, x, y, w, h,
                               !invert, mirror_y, pgm);
}

void GxEPD2_420c_NM_SSD1619A::writeNative(const uint8_t* data1, const uint8_t* data2,
        int16_t x, int16_t y, int16_t w, int16_t h, bool invert, bool mirror_y, bool pgm)
{
    (void)data2;
    if (data1) writeImage(data1, x, y, w, h, invert, mirror_y, pgm);
}

void GxEPD2_420c_NM_SSD1619A::drawImage(const uint8_t bitmap[], int16_t x, int16_t y,
                                        int16_t w, int16_t h, bool invert, bool mirror_y, bool pgm)
{
    writeImage(bitmap, x, y, w, h, invert, mirror_y, pgm);
    refresh(x, y, w, h);
}

void GxEPD2_420c_NM_SSD1619A::drawImagePart(const uint8_t bitmap[],
        int16_t x_part, int16_t y_part, int16_t w_bitmap, int16_t h_bitmap,
        int16_t x, int16_t y, int16_t w, int16_t h, bool invert, bool mirror_y, bool pgm)
{
    writeImagePart(bitmap, x_part, y_part, w_bitmap, h_bitmap, x, y, w, h, invert, mirror_y, pgm);
    refresh(x, y, w, h);
}

void GxEPD2_420c_NM_SSD1619A::drawImage(const uint8_t* black, const uint8_t* color,
        int16_t x, int16_t y, int16_t w, int16_t h, bool invert, bool mirror_y, bool pgm)
{
    writeImage(black, color, x, y, w, h, invert, mirror_y, pgm);
    refresh(x, y, w, h);
}

void GxEPD2_420c_NM_SSD1619A::drawImagePart(const uint8_t* black, const uint8_t* color,
        int16_t x_part, int16_t y_part, int16_t w_bitmap, int16_t h_bitmap,
        int16_t x, int16_t y, int16_t w, int16_t h, bool invert, bool mirror_y, bool pgm)
{
    writeImagePart(black, color, x_part, y_part, w_bitmap, h_bitmap, x, y, w, h,
                   invert, mirror_y, pgm);
    refresh(x, y, w, h);
}

void GxEPD2_420c_NM_SSD1619A::drawNative(const uint8_t* data1, const uint8_t* data2,
        int16_t x, int16_t y, int16_t w, int16_t h, bool invert, bool mirror_y, bool pgm)
{
    writeNative(data1, data2, x, y, w, h, invert, mirror_y, pgm);
    refresh(x, y, w, h);
}

void GxEPD2_420c_NM_SSD1619A::refresh(bool partial_update_mode)
{
    (void)partial_update_mode;
    _Update_Full();
}

void GxEPD2_420c_NM_SSD1619A::refresh(int16_t x, int16_t y, int16_t w, int16_t h)
{
    (void)x;
    (void)y;
    (void)w;
    (void)h;
    _Update_Full();
}

void GxEPD2_420c_NM_SSD1619A::powerOff() { _PowerOff(); }

void GxEPD2_420c_NM_SSD1619A::hibernate()
{
    _PowerOff();
    if (_rst >= 0) {
        _writeCommand(0x10);  // deep sleep
        _writeData(0x01);
        _hibernating = true;
        _init_display_done = false;
    }
}

void GxEPD2_420c_NM_SSD1619A::_setPartialRamArea(uint16_t x, uint16_t y, uint16_t w, uint16_t h)
{
    uint16_t xe = (x + w - 1) | 0x0007;
    uint16_t ye = y + h - 1;
    x &= 0xFFF8;
    _writeCommand(0x44);
    _writeData(x / 8);
    _writeData(xe / 8);
    _writeCommand(0x45);
    _writeData(y % 256);
    _writeData(y / 256);
    _writeData(ye % 256);
    _writeData(ye / 256);
    _writeCommand(0x4E);
    _writeData(x / 8);
    _writeCommand(0x4F);
    _writeData(y % 256);
    _writeData(y / 256);
}

void GxEPD2_420c_NM_SSD1619A::_PowerOn()
{
    // SSD1619A raises panel voltages implicitly in the update sequence.
    _power_is_on = true;
}

void GxEPD2_420c_NM_SSD1619A::_PowerOff()
{
    _power_is_on = false;
}

void GxEPD2_420c_NM_SSD1619A::_InitDisplay()
{
    if (_hibernating) _reset();
    _writeCommand(0x12);  // SW reset
    _waitWhileBusy("_InitDisplay swreset", 200);
    _writeCommand(0x01);  // driver output control: 300 gates
    _writeData(0x2B);
    _writeData(0x01);
    _writeData(0x00);
    _writeCommand(0x3C);  // border waveform
    _writeData(0x01);
    _writeCommand(0x18);  // temperature sensor
    _writeData(0x80);
    _writeCommand(0x11);  // data entry mode
    _writeData(0x03);
    _init_display_done = true;
}

void GxEPD2_420c_NM_SSD1619A::_Update_Full()
{
    _writeCommand(0x21);  // display update control 1
    _writeData(0x00);
    _writeCommand(0x22);  // display update control 2: full refresh
    _writeData(0xF7);
    _writeCommand(0x20);  // master activation
    _waitWhileBusy("_Update_Full", full_refresh_time);
    _PowerOff();
}
