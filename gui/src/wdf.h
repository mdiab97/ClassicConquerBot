#pragma once
// Lightweight WDF archive reader
#include <string>
#include <vector>
#include <unordered_map>
#include <fstream>
#include <cstring>

class WdfReader {
public:
    bool load(const std::string& wdf_path) {
        wdf_path_ = wdf_path;
        std::ifstream f(wdf_path, std::ios::binary);
        if (!f.is_open()) return false;

        // Header: id(4), count(4), offset(4)
        uint32_t magic, count, table_off;
        f.read((char*)&magic, 4);
        f.read((char*)&count, 4);
        f.read((char*)&table_off, 4);

        // Read entry table
        f.seekg(table_off);
        for (uint32_t i = 0; i < count; i++) {
            Entry e;
            uint32_t id;
            f.read((char*)&id, 4);
            f.read((char*)&e.offset, 4);
            f.read((char*)&e.size, 4);
            f.read((char*)&e.unknown, 4);
            entries_[id] = e;
        }
        return true;
    }

    bool get(const std::string& path, std::vector<uint8_t>& out) const {
        uint32_t id = hash_path(path);
        auto it = entries_.find(id);
        if (it == entries_.end()) return false;

        std::ifstream f(wdf_path_, std::ios::binary);
        if (!f.is_open()) return false;
        f.seekg(it->second.offset);
        out.resize(it->second.size);
        f.read((char*)out.data(), out.size());
        return true;
    }

    bool contains(const std::string& path) const {
        return entries_.count(hash_path(path)) > 0;
    }

    size_t size() const { return entries_.size(); }

private:
    struct Entry { uint32_t offset{0}, size{0}, unknown{0}; };

    static uint32_t hash_path(const std::string& path) {
        // Normalize: lowercase + forward slashes
        char buf[256]{};
        for (int i = 0; path[i] && i < 255; i++) {
            char c = path[i];
            if (c >= 'A' && c <= 'Z') c += 'a' - 'A';
            else if (c == '\\') c = '/';
            buf[i] = c;
        }
        return stringtoid(buf);
    }

    static uint32_t stringtoid(const char* str) {
        uint32_t eax, ebx, ecx, edx, edi, esi;
        uint64_t num;
        uint32_t m[0x46]{};
        char buf[0x100]{};

        size_t len = strlen(str);
        for (size_t i = 0; i < len && i < 0xFF; i++) buf[i] = str[i];

        int length = (int)((len % 4 == 0) ? len / 4 : len / 4 + 1);
        for (int i = 0; i < length; i++)
            m[i] = ((uint32_t*)buf)[i];
        int i = length;
        m[i++] = 0x9BE74448;
        m[i++] = 0x66F42C48;

        uint32_t v = 0xF4FA8928;
        edi = 0x7758B42B;
        esi = 0x37A8470E;

        for (ecx = 0; ecx < (uint32_t)i; ecx++) {
            ebx = 0x267B0B11;
            v = (v << 1) | (v >> 0x1F);
            ebx ^= v;
            eax = m[ecx];
            esi ^= eax;
            edi ^= eax;
            edx = ebx;
            edx += edi;
            edx |= 0x02040801;
            edx &= 0xBFEF7FDF;
            num = (uint64_t)edx * esi;
            eax = (uint32_t)num;
            edx = (uint32_t)(num >> 32);
            if (edx != 0) eax++;
            num = (uint64_t)eax + edx;
            eax = (uint32_t)num;
            if ((uint32_t)(num >> 32) != 0) eax++;
            edx = ebx;
            edx += esi;
            edx |= 0x00804021;
            edx &= 0x7DFEFBFF;
            esi = eax;
            num = (uint64_t)edi * edx;
            eax = (uint32_t)num;
            edx = (uint32_t)(num >> 32);
            num = (uint64_t)edx + edx;
            edx = (uint32_t)num;
            if ((uint32_t)(num >> 32) != 0) eax++;
            num = (uint64_t)eax + edx;
            eax = (uint32_t)num;
            if ((uint32_t)(num >> 32) != 0) eax += 2;
            edi = eax;
        }
        esi ^= edi;
        return esi;
    }

    std::string wdf_path_;
    std::unordered_map<uint32_t, Entry> entries_;
};
