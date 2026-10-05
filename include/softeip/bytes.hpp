// Little-endian (CIP) / big-endian (sockaddr) serialization helpers.
#pragma once

#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <vector>

namespace softeip {

struct ParseError : std::runtime_error {
    using std::runtime_error::runtime_error;
};

class ByteWriter {
public:
    void u8(uint8_t v) { buf_.push_back(v); }
    void u16(uint16_t v) { u8(uint8_t(v)); u8(uint8_t(v >> 8)); }
    void u32(uint32_t v) { u16(uint16_t(v)); u16(uint16_t(v >> 16)); }
    void u16be(uint16_t v) { u8(uint8_t(v >> 8)); u8(uint8_t(v)); }
    void bytes(const void* p, size_t n)
    {
        auto b = static_cast<const uint8_t*>(p);
        buf_.insert(buf_.end(), b, b + n);
    }
    void bytes(const std::vector<uint8_t>& v) { buf_.insert(buf_.end(), v.begin(), v.end()); }
    void zeros(size_t n) { buf_.insert(buf_.end(), n, uint8_t(0)); }
    void patchU16(size_t offset, uint16_t v)
    {
        buf_[offset] = uint8_t(v);
        buf_[offset + 1] = uint8_t(v >> 8);
    }
    size_t size() const { return buf_.size(); }
    std::vector<uint8_t>& data() { return buf_; }

private:
    std::vector<uint8_t> buf_;
};

class ByteReader {
public:
    ByteReader(const uint8_t* p, size_t n) : p_(p), n_(n) {}

    uint8_t u8()
    {
        need(1);
        return p_[pos_++];
    }
    uint16_t u16()
    {
        uint16_t lo = u8();
        uint16_t hi = u8();
        return uint16_t(lo | (hi << 8));
    }
    uint32_t u32()
    {
        uint32_t lo = u16();
        uint32_t hi = u16();
        return lo | (hi << 16);
    }
    uint16_t u16be()
    {
        uint16_t hi = u8();
        uint16_t lo = u8();
        return uint16_t((hi << 8) | lo);
    }
    const uint8_t* take(size_t n)
    {
        need(n);
        const uint8_t* r = p_ + pos_;
        pos_ += n;
        return r;
    }
    void skip(size_t n) { take(n); }
    size_t remaining() const { return n_ - pos_; }
    const uint8_t* cur() const { return p_ + pos_; }

private:
    void need(size_t n) const
    {
        if (n_ - pos_ < n)
            throw ParseError("truncated packet");
    }

    const uint8_t* p_;
    size_t n_;
    size_t pos_ = 0;
};

} // namespace softeip
