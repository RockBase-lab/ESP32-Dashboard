#pragma once

#include "ui/canvas/draw_surface.h"

class Adafruit_GFX;

class GfxSurface final : public IDrawSurface {
public:
    explicit GfxSurface(Adafruit_GFX &gfx);

    // Map the logical dashboard colors (kDashboardAccent / kDashboardHighlight)
    // to colors the attached panel can actually display. Call once after
    // construction with the board's pigment colors. Default: pass-through.
    void setPaletteColors(uint16_t accent, uint16_t highlight);

    int16_t width() const override;
    int16_t height() const override;

    void drawPixel(int16_t x, int16_t y, uint16_t color) override;
    void drawLine(int16_t x0, int16_t y0, int16_t x1, int16_t y1, uint16_t color) override;
    void drawRect(int16_t x, int16_t y, int16_t w, int16_t h, uint16_t color) override;
    void fillRect(int16_t x, int16_t y, int16_t w, int16_t h, uint16_t color) override;
    void fillScreen(uint16_t color) override;
    void drawText(int16_t x, int16_t y, const std::string &text, uint16_t color,
                  TextAlign align = TextAlign::Left, uint8_t size = 1) override;
    int16_t measureText(const std::string &text, uint8_t size = 1) const override;

private:
    // Logical dashboard color → physical panel color mapping.
    uint16_t mapColor(uint16_t color) const;

    Adafruit_GFX &_gfx;
    uint16_t      _accent    = kDashboardAccent;     // pass-through by default
    uint16_t      _highlight = kDashboardHighlight;
};
