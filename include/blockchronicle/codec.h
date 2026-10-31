// Copyright 2026 Swadhin Goswami
// Licensed under the Apache License, Version 2.0. See LICENSE.

#ifndef BLOCKCHRONICLE_CODEC_H
#define BLOCKCHRONICLE_CODEC_H

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace blockchronicle {

// Explicit little-endian serialization. Nothing is memcpy'd from a struct:
// padding, alignment and endianness are what make that unsafe across builds.
class ByteWriter {
public:
    void u8(std::uint8_t value);
    void u16(std::uint16_t value);
    void u32(std::uint32_t value);
    void u64(std::uint64_t value);
    void bytes(const void* data, std::size_t length);
    void string16(const std::string& value); // u16 length + bytes

    const std::vector<std::uint8_t>& data() const { return buffer_; }
    std::size_t size() const { return buffer_.size(); }
    void clear() { buffer_.clear(); }

private:
    std::vector<std::uint8_t> buffer_;
};

// Bounds-checked reader. Any short read sets good() to false and every
// subsequent read fails, so a truncated file cannot produce a half-decoded
// structure.
class ByteReader {
public:
    ByteReader(const std::uint8_t* data, std::size_t size);

    bool u8(std::uint8_t& value);
    bool u16(std::uint16_t& value);
    bool u32(std::uint32_t& value);
    bool u64(std::uint64_t& value);
    bool bytes(void* out, std::size_t length);
    bool string16(std::string& value);

    bool good() const { return good_; }
    std::size_t offset() const { return offset_; }
    std::size_t remaining() const { return good_ ? size_ - offset_ : 0u; }

private:
    const std::uint8_t* data_;
    std::size_t size_;
    std::size_t offset_ = 0;
    bool good_ = true;
};

} // namespace blockchronicle

#endif // BLOCKCHRONICLE_CODEC_H
