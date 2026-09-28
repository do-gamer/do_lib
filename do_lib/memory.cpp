#include "memory.h"
#include <cstring>
#include <cstdio>
#include <sys/mman.h>
#include <unistd.h>
#include <time.h>
#include <sstream>
#include <iostream>
#include <fstream>

#include "masked_bmh.h"


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
    if (!query || !mask)
        return 0ULL;

    const uintptr_t query_size = std::strlen(mask);
    if (query_size == 0)
        return 0ULL;

    const uintptr_t query_addr = reinterpret_cast<uintptr_t>(query);

    for (const auto &region : get_pages(area))
    {
        const uintptr_t region_size = region.end - region.start;

        if (query_size > region_size
            || (query_addr >= region.start && query_addr < region.end)
            || region.read != 'r'
            || region.name == "[vvar]" || region.name == "[vsyscall]" || region.name == "[vvar_vclock]")
        {
            continue;
        }

        // search in place (this is our own process): no copy of every region
        const size_t found = masked_bmh_search(
            reinterpret_cast<const uint8_t *>(region.start),
            region_size,
            reinterpret_cast<const uint8_t *>(query),
            mask,
            query_size,
            0,
            alignment);

        if (found != SIZE_MAX)
            return region.start + found;
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
