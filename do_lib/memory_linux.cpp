#include "memory.h"
#include <cstring>
#include <cstdio>
#include <sys/mman.h>
#include <unistd.h>
#include <time.h>
#include <sstream>
#include <iostream>
#include <fstream>

#include "../tools/masked_bmh.h"


int memory:: unprotect(uint64_t address)
{
    long pagesize = sysconf(_SC_PAGESIZE);
    void *m_address = (void *)((long)address & ~(pagesize - 1));
    return mprotect(m_address, pagesize, PROT_WRITE | PROT_READ | PROT_EXEC);
}

std::vector<memory::MemPage> memory::get_pages(const std::string &name)
{
    std::vector<MemPage> pages;
    if (std::ifstream maps_f { "/proc/self/maps" })
    {
        std::string line;
        while (std::getline(maps_f, line))
        {
            std::stringstream ss(line);

            uintptr_t start, end, offset, dev_major, dev_minor, inode;
            char skip, r, w, x, c;
            std::string path_name;

            ss >> std::hex >> start >> skip >> end >>
                r >> w >> x >> c >>
                offset >> dev_major >>
                skip >> dev_minor >> 
                inode >> path_name;

            if (!name.empty() && path_name.find(name) == std::string::npos)
            {
                continue;
            }

            pages.emplace_back(start, end, r, w, x, c, offset, 0, path_name);
        }
    }
    return pages;
}


uintptr_t memory::query_memory(uint8_t *query, const char *mask, uint32_t alignment, const std::string &area)
{
    uintptr_t query_size = strlen(mask);
    uintptr_t size = 0;

    for (auto &region : get_pages(area)) 
    {
        size = region.end - region.start;

        if (query_size > size 
            || (uintptr_t(query) > region.start && uintptr_t(query) < region.end) 
            || region.read == '-' 
            || region.name == "[vvar]"
        )
        {
            continue;
        }

        // reuse a buffer across regions to reduce allocations
        static std::vector<uint8_t> buf;
        try {
            buf.resize(size);
            std::memcpy(buf.data(), reinterpret_cast<const void *>(region.start), size);

            size_t offset = 0;
            while (true) {
                size_t found = masked_bmh_search(buf.data(), size,
                                                 reinterpret_cast<const uint8_t*>(query), mask, query_size,
                                                 offset, alignment);
                if (found == SIZE_MAX) break;
                return region.start + found;
            }
        } catch (const std::bad_alloc &) {
            // couldn't allocate buffer for this region; skip it
            continue;
        }
    }

    return 0ULL;
}

uintptr_t memory::find_pattern(const std::string &query, const std::string &segment)
{
    std::stringstream ss(query);
    std::string data{ };
    std::string mask{ };
    std::vector<uint8_t> bytes;

    while (std::getline(ss, data, ' ')) 
    {
        if (data.find('?') != std::string::npos) 
        {
            mask += "?";
            bytes.push_back(0);
        }
        else 
        {
            bytes.push_back(static_cast<uint8_t>(std::stoi(data, nullptr, 16)));
            mask += "x";
        }
    }
    return query_memory(&bytes.at(0), mask.c_str(), 1, segment);
}
