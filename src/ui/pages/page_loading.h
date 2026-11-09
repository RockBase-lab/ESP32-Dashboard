#pragma once

#include "ui/hal.h"

// PageLoadingBase — abstract base for the startup / refreshing page.
//
// Subclasses implement create() and draw() with resolution-specific layout.
class PageLoadingBase : public UIPage {
public:
    // Set status text before calling draw().
    void setStatus(const char *status);

    // Set the device model line (drawn in the given color) before calling draw().
    void setModel(const char *model, uint16_t color);

protected:
    const char *_status = nullptr;

    const char *_model      = nullptr;
    uint16_t    _modelColor = 0;

    Adafruit_GFX *_gfx = nullptr;
    uint16_t      _w           = 0;
    uint16_t      _h           = 0;
    uint16_t      _colorAccent = 0;
    bool          _hasAccent   = false;
};
