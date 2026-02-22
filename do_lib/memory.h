#ifndef MEMORY_H
#define MEMORY_H
#include <string>
#include <cstring>
#include <cstdint>
#include <vector>

namespace memory
{
    struct MemPage
    {
        MemPage(uintptr_t s,uintptr_t e, 
                char r, char w, char x, char c,
                uintptr_t offset, uintptr_t size,
                const std::string &name) :
            start(s), end(e),
            read(r), write(w), exec(x), cow(c),
            offset(offset), size(size),
            name(name)
        {
        }

        uintptr_t start, end;
        char read, write, exec, cow;
        uintptr_t offset;
        uintptr_t size;
        std::string name;
    };


    int unprotect(uint64_t address);

    uintptr_t query_memory(uint8_t *query, const char *mask, uint32_t alignment, const std::string &area = "");


    inline uintptr_t query_memory(uint8_t *query, uint32_t len, uint32_t alignment)
    {
        std::string mask(len, 'x');
        return query_memory(query, mask.c_str(), alignment);
    }

    uintptr_t find_pattern(const std::string &query, const std::string &segment);

    std::vector<MemPage> get_pages(const std::string &name = "");

    template<typename T>
    inline T read(uintptr_t addr)
    {
        T v;
        std::memcpy(&v, reinterpret_cast<const void *>(addr), sizeof(T));
        return v;
    }

    template <typename T, typename ... Offsets >
    inline T read(uintptr_t address, uintptr_t ofs, Offsets ... offsets)
    {
        uintptr_t next = 0;
        std::memcpy(&next, reinterpret_cast<const void *>(address), sizeof(next));
        return read<T>(next + ofs, offsets...);
    }

    template <typename T>
    inline void write(uintptr_t address, T value)
    {
        std::memcpy(reinterpret_cast<void *>(address), &value, sizeof(T));
    }

    template <typename T, typename ... Offsets >
    inline T write(uintptr_t address, T value, uintptr_t ofs, Offsets ... offsets)
    {
        uintptr_t next = 0;
        std::memcpy(&next, reinterpret_cast<const void *>(address), sizeof(next));
        return write<T>(next + ofs, value, offsets...);
    }

};

#endif // MEMORY_H
