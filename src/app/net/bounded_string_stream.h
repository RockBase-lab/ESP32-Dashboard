#pragma once

#include <Arduino.h>

// Ограниченный Print-поток: HTTPClient::writeToStream() пишет ответ в String,
// но без лимита. Здесь лимит есть — большой/враждебный ответ не
// сможет выесть кучу до того, как сработает автоматический PSRAM-malloc.
class BoundedStringStream final : public Stream {
public:
    BoundedStringStream(String &out, size_t maxBytes)
        : _out(out), _maxBytes(maxBytes) {}

    using Print::write;

    size_t write(uint8_t c) override {
        if (_out.length() >= _maxBytes) return 0;
        return _out.concat(static_cast<char>(c));
    }

    size_t write(const uint8_t *buffer, size_t size) override {
        if (!buffer || size == 0) return 0;
        const size_t used = _out.length();
        if (used >= _maxBytes) return 0;
        const size_t room = _maxBytes - used;
        const size_t count = size < room ? size : room;
        _out.concat(reinterpret_cast<const char *>(buffer), count);
        return count;
    }

    // Stream-часть: нам нужна только запись, чтение не используется.
    int available() override { return 0; }
    int read() override { return -1; }
    int peek() override { return -1; }
    void flush() override {}

private:
    String &_out;
    size_t  _maxBytes;
};
