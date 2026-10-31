// Copyright 2026 Swadhin Goswami
// Licensed under the Apache License, Version 2.0. See LICENSE.

#include "blockchronicle/codec.h"

#include <cstring>

namespace blockchronicle {
namespace {

void appendLe(std::vector<std::uint8_t>& buffer, std::uint64_t value, std::size_t width)
{
    for (std::size_t i = 0; i < width; ++i) {
        buffer.push_back(static_cast<std::uint8_t>((value >> (8u * i)) & 0xFFu));
    }
}

std::uint64_t readLe(const std::uint8_t* data, std::size_t width)
{
    std::uint64_t value = 0;
    for (std::size_t i = 0; i < width; ++i) {
        value |= static_cast<std::uint64_t>(data[i]) << (8u * i);
    }
    return value;
}

} // namespace

void ByteWriter::u8(std::uint8_t value)
{
    buffer_.push_back(value);
}

void ByteWriter::u16(std::uint16_t value)
{
    appendLe(buffer_, value, 2);
}

void ByteWriter::u32(std::uint32_t value)
{
    appendLe(buffer_, value, 4);
}

void ByteWriter::u64(std::uint64_t value)
{
    appendLe(buffer_, value, 8);
}

void ByteWriter::bytes(const void* data, std::size_t length)
{
    const std::uint8_t* bytes = static_cast<const std::uint8_t*>(data);
    buffer_.insert(buffer_.end(), bytes, bytes + length);
}

void ByteWriter::string16(const std::string& value)
{
    u16(static_cast<std::uint16_t>(value.size()));
    bytes(value.data(), value.size());
}

ByteReader::ByteReader(const std::uint8_t* data, std::size_t size)
    : data_(data)
    , size_(size)
{
}

bool ByteReader::u8(std::uint8_t& value)
{
    if (!good_ || remaining() < 1u) {
        good_ = false;
        return false;
    }
    value = data_[offset_];
    ++offset_;
    return true;
}

bool ByteReader::u16(std::uint16_t& value)
{
    if (!good_ || remaining() < 2u) {
        good_ = false;
        return false;
    }
    value = static_cast<std::uint16_t>(readLe(data_ + offset_, 2));
    offset_ += 2u;
    return true;
}

bool ByteReader::u32(std::uint32_t& value)
{
    if (!good_ || remaining() < 4u) {
        good_ = false;
        return false;
    }
    value = static_cast<std::uint32_t>(readLe(data_ + offset_, 4));
    offset_ += 4u;
    return true;
}

bool ByteReader::u64(std::uint64_t& value)
{
    if (!good_ || remaining() < 8u) {
        good_ = false;
        return false;
    }
    value = readLe(data_ + offset_, 8);
    offset_ += 8u;
    return true;
}

bool ByteReader::bytes(void* out, std::size_t length)
{
    if (!good_ || remaining() < length) {
        good_ = false;
        return false;
    }
    std::memcpy(out, data_ + offset_, length);
    offset_ += length;
    return true;
}

bool ByteReader::string16(std::string& value)
{
    std::uint16_t length = 0;
    if (!u16(length)) {
        return false;
    }
    if (remaining() < length) {
        good_ = false;
        return false;
    }
    value.assign(reinterpret_cast<const char*>(data_ + offset_), length);
    offset_ += length;
    return true;
}

} // namespace blockchronicle
