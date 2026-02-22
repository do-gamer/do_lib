#ifndef BINARY_STREAM_H
#define BINARY_STREAM_H
#include <string>
#include <vector>
#include <cstring>


class BinaryStream
{
    public:
    BinaryStream(const uint8_t *data_ptr) : data(data_ptr) { }

    std::string read_string()
    {
        // find the null terminator and construct the string in one allocation
        size_t start = position;
        while (data[position] != 0x00)
            ++position;
        size_t len = position - start;
        std::string out_str(reinterpret_cast<const char *>(&data[start]), len);
        // advance past the terminator
        ++position;
        return out_str;
    }

    void read_bytes(unsigned char *out, size_t size)
    {
        memcpy(out, &data[position], size);
        position += size;
    }

    template<typename T>
    inline T read()
    {
        T r;
        std::memcpy(&r, &data[position], sizeof(T));
        position += sizeof(T);
        return r;
    }

    template<typename T>
    inline T peek()
    {
        T r;
        std::memcpy(&r, &data[position], sizeof(T));
        return r;
    }

    int32_t read_s24()
    {
        auto b = (data[position] | data[position+1]<<8 | ((int8_t)data[position+2])<<16 );
        position += 3;
        return b;
    }

    uint32_t read_u32()
    {
        unsigned int result = read<uint8_t>();
        if ((result & 0x00000080) == 0)
        {
            return result;
        }
        result = (result & 0x0000007F) | read<uint8_t>() << 7;
        if ((result & 0x00004000) == 0)
        {
            return result;
        }
        result = (result & 0x00003FFF) | read<uint8_t>() << 14;
        if ((result & 0x00200000) == 0)
        {
            return result;
        }
        result = (result & 0x001FFFFF) | read<uint8_t>() << 21;
        if ((result & 0x10000000) == 0)
        {
            return result;
        }
        return (result & 0x0FFFFFFF) | read<uint8_t>() << 28;
    }

    inline uint32_t read_u30() { return read_u32(); }


    size_t position = 0;
    const uint8_t *data;
};


#endif /* BINARY_STREAM_H */
