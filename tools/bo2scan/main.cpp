// bo2scan: reads a Xbox 360 XEX (optionally applying a title update .xexp) and
// prints the values XenonRecomp's TOML config needs that can be found by
// pattern matching: the register save/restore helpers, longjmp candidates and
// a count of jump table sites to compare against XenonAnalyse's output.
// With a title update it also writes the patched XEX, which XenonAnalyse needs.

#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

#include <file.h>
#include <image.h>
#include <xex_patcher.h>
#include <fmt/core.h>

namespace
{
    struct Pattern
    {
        const char* key;
        std::vector<uint8_t> bytes;
    };

    // Byte patterns from the XenonRecomp README.
    const Pattern kPatterns[] =
    {
        { "restgprlr_14_address", { 0xE9, 0xC1, 0xFF, 0x68 } },
        { "savegprlr_14_address", { 0xF9, 0xC1, 0xFF, 0x68 } },
        { "restfpr_14_address",   { 0xC9, 0xCC, 0xFF, 0x70 } },
        { "savefpr_14_address",   { 0xD9, 0xCC, 0xFF, 0x70 } },
        { "restvmx_14_address",   { 0x39, 0x60, 0xFE, 0xE0, 0x7D, 0xCB, 0x60, 0xCE } },
        { "savevmx_14_address",   { 0x39, 0x60, 0xFE, 0xE0, 0x7D, 0xCB, 0x61, 0xCE } },
        { "restvmx_64_address",   { 0x39, 0x60, 0xFC, 0x00, 0x10, 0x0B, 0x60, 0xCB } },
        { "savevmx_64_address",   { 0x39, 0x60, 0xFC, 0x00, 0x10, 0x0B, 0x61, 0xCB } },
    };

    constexpr uint32_t kMtctrR0 = 0x7C0903A6;
    constexpr uint32_t kMflrR12 = 0x7D8802A6;

    uint32_t ReadBE32(const uint8_t* p)
    {
        return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | uint32_t(p[3]);
    }

    // Returns the target of a relative `bl`, or 0 if the instruction is not one.
    size_t BranchLinkTarget(uint32_t insn, size_t address)
    {
        if ((insn >> 26) != 18 || (insn & 3) != 1)
            return 0;

        int32_t offset = int32_t(insn & 0x03FFFFFC);
        if (offset & 0x02000000)
            offset -= 0x04000000;

        return size_t(int64_t(address) + offset);
    }

    // Walks back to the closest `mflr r12`, the usual start of a non-leaf function.
    size_t FindPrologue(const Section& section, size_t address)
    {
        for (size_t a = address; a >= section.base && a - section.base < section.size; a -= 4)
        {
            if (ReadBE32(section.data + (a - section.base)) == kMflrR12)
                return a;
        }
        return 0;
    }
}

int main(int argc, char* argv[])
{
    if (argc < 2)
    {
        fmt::println("Usage: bo2scan <default.xex> [title update .xexp] [patched .xex to write]");
        return EXIT_FAILURE;
    }

    std::vector<uint8_t> file = LoadFile(argv[1]);
    if (file.empty())
    {
        fmt::println("ERROR: unable to read {}", argv[1]);
        return EXIT_FAILURE;
    }

    if (argc >= 3)
    {
        std::vector<uint8_t> patch = LoadFile(argv[2]);
        std::vector<uint8_t> patched;
        if (patch.empty() ||
            XexPatcher::apply(file.data(), file.size(), patch.data(), patch.size(), patched, false) != XexPatcher::Result::Success)
        {
            fmt::println("ERROR: unable to apply title update {}", argv[2]);
            return EXIT_FAILURE;
        }
        file = std::move(patched);

        if (argc >= 4)
        {
            std::ofstream stream(argv[3], std::ios::binary);
            stream.write(reinterpret_cast<const char*>(file.data()), file.size());
            if (!stream)
            {
                fmt::println("ERROR: unable to write {}", argv[3]);
                return EXIT_FAILURE;
            }
            fmt::println("# Wrote patched XEX to {}", argv[3]);
        }
    }

    Image image = Image::ParseImage(file.data(), file.size());
    if (image.data == nullptr)
    {
        fmt::println("ERROR: {} is not a supported XEX", argv[1]);
        return EXIT_FAILURE;
    }

    fmt::println("# Image base 0x{:X}, size 0x{:X}, entry point 0x{:X}", image.base, image.size, image.entry_point);
    for (const auto& section : image.sections)
    {
        fmt::println("# Section {:<8} 0x{:08X} - 0x{:08X}{}", section.name, section.base, section.base + section.size,
            (section.flags & SectionFlags_Code) ? " (code)" : "");
    }

    size_t rtlUnwind = 0;
    size_t importCount = 0;
    for (const auto& symbol : image.symbols)
    {
        if (symbol.name.rfind("__imp__", 0) == 0)
            importCount++;
        if (symbol.name == "__imp__RtlUnwind")
            rtlUnwind = symbol.address;
    }
    fmt::println("# {} kernel/XAM imports", importCount);

    std::vector<size_t> matches[std::size(kPatterns)];
    std::vector<size_t> unwindCallers;
    size_t jumpTableSites = 0;

    for (const auto& section : image.sections)
    {
        if (!(section.flags & SectionFlags_Code))
            continue;

        for (size_t offset = 0; offset + 4 <= section.size; offset += 4)
        {
            const uint8_t* p = section.data + offset;
            size_t address = section.base + offset;
            uint32_t insn = ReadBE32(p);

            for (size_t i = 0; i < std::size(kPatterns); i++)
            {
                const auto& bytes = kPatterns[i].bytes;
                if (offset + bytes.size() <= section.size && memcmp(p, bytes.data(), bytes.size()) == 0)
                    matches[i].push_back(address);
            }

            if (insn == kMtctrR0)
                jumpTableSites++;

            if (rtlUnwind != 0 && BranchLinkTarget(insn, address) == rtlUnwind)
                unwindCallers.push_back(address);
        }
    }

    fmt::println("\n# Paste into the [main] table of config/bo2_sp.toml. Check every value in a disassembler first.");
    for (size_t i = 0; i < std::size(kPatterns); i++)
    {
        const auto& found = matches[i];
        if (found.empty())
        {
            fmt::println("# {} = NOT FOUND", kPatterns[i].key);
            continue;
        }

        fmt::println("{} = 0x{:08X}{}", kPatterns[i].key, found.front(),
            found.size() > 1 ? fmt::format("  # {} matches, first one shown", found.size()) : "");
    }

    fmt::println("\n# longjmp usually calls RtlUnwind; setjmp is usually right after longjmp.");
    if (rtlUnwind == 0)
        fmt::println("# RtlUnwind is not imported");
    for (size_t caller : unwindCallers)
    {
        size_t start = 0;
        for (const auto& section : image.sections)
        {
            if (section == caller)
                start = FindPrologue(section, caller);
        }
        fmt::println("# RtlUnwind called at 0x{:08X}, enclosing function probably starts near 0x{:08X}", caller, start);
    }

    fmt::println("\n# {} `mtctr r0` sites (possible jump tables). Compare with the XenonAnalyse output.", jumpTableSites);
    return EXIT_SUCCESS;
}
