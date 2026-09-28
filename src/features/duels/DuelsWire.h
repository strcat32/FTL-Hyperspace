#pragma once

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

// Little-endian binary encoding for network messages. No game headers, so the netcode can be unit-tested alone.
namespace Duels
{
    class Writer
    {
    public:
        void U8(uint8_t value) { data.push_back(value); }
        void U16(uint16_t value)
        {
            U8((uint8_t)(value & 0xff));
            U8((uint8_t)(value >> 8));
        }
        void U32(uint32_t value)
        {
            U16((uint16_t)(value & 0xffff));
            U16((uint16_t)(value >> 16));
        }
        void I8(int8_t value) { U8((uint8_t)value); }
        void I16(int16_t value) { U16((uint16_t)value); }
        void I32(int32_t value) { U32((uint32_t)value); }
        void Bool(bool value) { U8(value ? 1 : 0); }
        void F32(float value)
        {
            uint32_t bits;
            std::memcpy(&bits, &value, sizeof(bits));
            U32(bits);
        }
        void F64(double value)
        {
            uint64_t bits;
            std::memcpy(&bits, &value, sizeof(bits));
            U32((uint32_t)(bits & 0xffffffffu));
            U32((uint32_t)(bits >> 32));
        }
        // Strings are limited to 255 bytes (names, blueprint ids, short chat lines).
        void Str(const std::string &value)
        {
            size_t size = value.size() > 255 ? 255 : value.size();
            U8((uint8_t)size);
            data.insert(data.end(), value.begin(), value.begin() + size);
        }
        void Bytes(const uint8_t *bytes, size_t size) { data.insert(data.end(), bytes, bytes + size); }

        std::vector<uint8_t> data;
    };

    // Reads what Writer wrote. Reading past the end yields zeros and clears Ok(); check it once at the end.
    class Reader
    {
    public:
        Reader(const uint8_t *bytes, size_t size) : pos(bytes), end(bytes + size) {}
        explicit Reader(const std::vector<uint8_t> &bytes) : pos(bytes.data()), end(bytes.data() + bytes.size()) {}

        bool Ok() const { return ok; }
        size_t Remaining() const { return ok ? (size_t)(end - pos) : 0; }
        const uint8_t *Position() const { return pos; }

        uint8_t U8()
        {
            if (!Need(1)) return 0;
            return *pos++;
        }
        uint16_t U16()
        {
            uint16_t low = U8();
            uint16_t high = U8();
            return (uint16_t)(low | (high << 8));
        }
        uint32_t U32()
        {
            uint32_t low = U16();
            uint32_t high = U16();
            return low | (high << 16);
        }
        int8_t I8() { return (int8_t)U8(); }
        int16_t I16() { return (int16_t)U16(); }
        int32_t I32() { return (int32_t)U32(); }
        bool Bool() { return U8() != 0; }
        float F32()
        {
            uint32_t bits = U32();
            float value;
            std::memcpy(&value, &bits, sizeof(value));
            return value;
        }
        double F64()
        {
            uint64_t low = U32();
            uint64_t high = U32();
            uint64_t bits = low | (high << 32);
            double value;
            std::memcpy(&value, &bits, sizeof(value));
            return value;
        }
        std::string Str()
        {
            size_t size = U8();
            if (!Need(size)) return std::string();
            std::string value((const char*)pos, size);
            pos += size;
            return value;
        }
        bool Skip(size_t size)
        {
            if (!Need(size)) return false;
            pos += size;
            return true;
        }

    private:
        bool Need(size_t size)
        {
            if (!ok || (size_t)(end - pos) < size)
            {
                ok = false;
                return false;
            }
            return true;
        }

        const uint8_t *pos;
        const uint8_t *end;
        bool ok = true;
    };
}
