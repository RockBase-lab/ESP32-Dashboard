#include "gfx_surface.h"

#include <Adafruit_GFX.h>

GfxSurface::GfxSurface(Adafruit_GFX &gfx) : _gfx(gfx) {}

void GfxSurface::setPaletteColors(uint16_t accent, uint16_t highlight) {
    _accent    = accent;
    _highlight = highlight;
}

uint16_t GfxSurface::mapColor(uint16_t color) const {
    // Map the logical dashboard accent/highlight colors onto what the
    // attached panel can physically show. On BW panels both degrade to black
    // so accent-colored content stays visible instead of rendering white
    // (GxEPD2_BW treats any nonzero color as white).
    if (color == kDashboardAccent) {
        return _accent;
    }
    if (color == kDashboardHighlight) {
        return _highlight;
    }
    return color;
}

int16_t GfxSurface::width() const {
    return _gfx.width();
}

int16_t GfxSurface::height() const {
    return _gfx.height();
}

void GfxSurface::drawPixel(int16_t x, int16_t y, uint16_t color) {
    _gfx.drawPixel(x, y, mapColor(color));
}

void GfxSurface::drawLine(int16_t x0, int16_t y0, int16_t x1, int16_t y1, uint16_t color) {
    _gfx.drawLine(x0, y0, x1, y1, mapColor(color));
}

void GfxSurface::drawRect(int16_t x, int16_t y, int16_t w, int16_t h, uint16_t color) {
    _gfx.drawRect(x, y, w, h, mapColor(color));
}

void GfxSurface::fillRect(int16_t x, int16_t y, int16_t w, int16_t h, uint16_t color) {
    _gfx.fillRect(x, y, w, h, mapColor(color));
}

void GfxSurface::fillScreen(uint16_t color) {
    _gfx.fillScreen(mapColor(color));
}

void GfxSurface::drawText(int16_t x, int16_t y, const std::string &text, uint16_t color,
                          TextAlign align, uint8_t size) {
    _gfx.setFont();
    _gfx.setTextSize(size);
    _gfx.setTextWrap(false);
    int16_t x1 = 0;
    int16_t y1 = 0;
    uint16_t w = 0;
    uint16_t h = 0;
    _gfx.getTextBounds(text.c_str(), x, y, &x1, &y1, &w, &h);
    int16_t drawX = x;
    if (align == TextAlign::Center) {
        drawX = x - static_cast<int16_t>(w / 2);
    } else if (align == TextAlign::Right) {
        drawX = x - static_cast<int16_t>(w);
    }
    _gfx.setTextColor(mapColor(color));
    _gfx.setCursor(drawX, y);
    _gfx.print(text.c_str());
}

int16_t GfxSurface::measureText(const std::string &text, uint8_t size) const {
    const_cast<Adafruit_GFX &>(_gfx).setFont();
    const_cast<Adafruit_GFX &>(_gfx).setTextSize(size);
    const_cast<Adafruit_GFX &>(_gfx).setTextWrap(false);
    int16_t x1 = 0;
    int16_t y1 = 0;
    uint16_t w = 0;
    uint16_t h = 0;
    _gfx.getTextBounds(text.c_str(), 0, 0, &x1, &y1, &w, &h);
    return static_cast<int16_t>(w);
}
