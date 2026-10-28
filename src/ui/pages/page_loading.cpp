#include "page_loading.h"

void PageLoadingBase::setStatus(const char *status) {
    _status = status;
}

void PageLoadingBase::setModel(const char *model, uint16_t color) {
    _model      = model;
    _modelColor = color;
}
