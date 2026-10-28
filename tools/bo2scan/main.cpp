// bo2scan: reads the Black Ops II Xbox 360 executable (optionally applying a
// title update .xexp) and produces what XenonRecomp needs that XenonAnalyse
// can't find on its own:
//   - the register save/restore helper addresses
//   - the setjmp/longjmp addresses
//   - the jump tables, written as a switch table TOML. BO2's compiler schedules
//     the table load as lis/rlwinm/addi/lwzx/mtctr/bctr, which XenonAnalyse's
//     Sonic Unleashed patterns (lis/addi/rlwinm/...) don't match.
//   - explicit boundaries for functions that contain jump tables but have no
//     .pdata entry, which XenonRecomp's analyser otherwise cuts short at the bctr.

#include <cstdio>
#include <algorithm>
#include <cstring>
#include <fstream>
#include <set>
#include <string>
#include <vector>

#include <disasm.h>
#include <file.h>
#include <image.h>
#include <xex_patcher.h>
#include <fmt/core.h>

namespace
{
    struct Pattern
    {
        const char* key;
        std::vector<uint32_t> words;
    };

    // Byte patterns from the XenonRecomp README.
    const Pattern kHelperPatterns[] =
    {
        { "restgprlr_14_address", { 0xE9C1FF68 } },
        { "savegprlr_14_address", { 0xF9C1FF68 } },
        { "restfpr_14_address",   { 0xC9CCFF70 } },
        { "savefpr_14_address",   { 0xD9CCFF70 } },
        { "restvmx_14_address",   { 0x3960FEE0, 0x7DCB60CE } },
        { "savevmx_14_address",   { 0x3960FEE0, 0x7DCB61CE } },
        { "restvmx_64_address",   { 0x3960FC00, 0x100B60CB } },
        { "savevmx_64_address",   { 0x3960FC00, 0x100B61CB } },
    };

    // setjmp stores f14/f15 into the jmp_buf in r3, longjmp reloads them from r7.
    const Pattern kJmpPatterns[] =
    {
        { "setjmp_address",  { 0xD9C30000, 0xD9E30008 } }, // stfd f14,0(r3); stfd f15,8(r3)
        { "longjmp_address", { 0xC9C70000, 0xC9E70008 } }, // lfd f14,0(r7); lfd f15,8(r7)
    };

    const uint32_t kSwitchPattern[] =
    {
        PPC_INST_LIS,
        PPC_INST_RLWINM,
        PPC_INST_ADDI,
        PPC_INST_LWZX,
        PPC_INST_MTCTR,
        PPC_INST_BCTR,
    };

    struct SwitchTable
    {
        size_t base{};
        uint32_t r{};
        size_t defaultLabel{};
        std::vector<uint32_t> labels;
    };

    struct FunctionRange
    {
        size_t base{};
        size_t size{};
    };

    uint32_t ReadBE32(const uint8_t* p)
    {
        return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | uint32_t(p[3]);
    }

    std::vector<FunctionRange> ReadPdata(const Image& image)
    {
        std::vector<FunctionRange> functions;
        const Section* pdata = image.Find(".pdata");
        if (pdata == nullptr)
            return functions;

        for (size_t offset = 0; offset + 8 <= pdata->size; offset += 8)
        {
            uint32_t begin = ReadBE32(pdata->data + offset);
            uint32_t data = ReadBE32(pdata->data + offset + 4);
            if (begin != 0)
                functions.push_back({ begin, ((data >> 8) & 0x3FFFFF) * 4 });
        }
        return functions;
    }

    bool MatchWords(const Section& section, size_t offset, const std::vector<uint32_t>& words)
    {
        if (offset + words.size() * 4 > section.size)
            return false;

        for (size_t i = 0; i < words.size(); i++)
        {
            if (ReadBE32(section.data + offset + i * 4) != words[i])
                return false;
        }
        return true;
    }

    // Leaf functions like setjmp have no .pdata entry, so walk back to the
    // zero padding the linker puts between functions, or to a known boundary
    // (the end of a .pdata function or a `bl` target).
    size_t FindFunctionStart(const Section& section, size_t offset, size_t limit = 64,
        const std::set<size_t>* boundaries = nullptr)
    {
        for (size_t i = 0; i < limit && offset >= 4; i++)
        {
            if (boundaries != nullptr && boundaries->count(section.base + offset))
                return section.base + offset;
            if (ReadBE32(section.data + offset - 4) == 0)
                return section.base + offset;
            offset -= 4;
        }
        return 0;
    }

    bool MatchSwitch(const Section& section, size_t offset)
    {
        if (offset + std::size(kSwitchPattern) * 4 > section.size)
            return false;

        ppc_insn insn;
        for (size_t i = 0; i < std::size(kSwitchPattern); i++)
        {
            ppc::Disassemble(section.data + offset + i * 4, section.base + offset + i * 4, insn);
            if (insn.opcode == nullptr || insn.opcode->id != kSwitchPattern[i])
                return false;
        }
        return true;
    }

    // Same idea as XenonAnalyse's ScanTable: walk back to the bounds check to
    // get the index register, the case count and the default label.
    bool ReadSwitch(const Image& image, const Section& section, size_t offset, SwitchTable& table)
    {
        ppc_insn insn;
        uint32_t cr = UINT32_MAX;
        size_t count = 0;

        for (size_t i = 1; i <= 32 && i * 4 <= offset; i++)
        {
            size_t at = offset - i * 4;
            ppc::Disassemble(section.data + at, section.base + at, insn);
            if (insn.opcode == nullptr)
                continue;

            auto id = insn.opcode->id;
            if (cr == UINT32_MAX)
            {
                if (id == PPC_INST_BGT || id == PPC_INST_BLE || id == PPC_INST_BGTLR || id == PPC_INST_BLELR)
                {
                    cr = insn.operands[0];
                    if (id == PPC_INST_BGT || id == PPC_INST_BLE)
                        table.defaultLabel = insn.operands[1];
                }
            }
            else if (id == PPC_INST_CMPLWI && insn.operands[0] == cr)
            {
                table.r = insn.operands[1];
                count = size_t(insn.operands[2]) + 1;
                break;
            }
        }

        if (count == 0)
            return false;

        ppc::Disassemble(section.data + offset, section.base + offset, insn);
        uint32_t address = insn.operands[1] << 16;
        ppc::Disassemble(section.data + offset + 8, section.base + offset + 8, insn);
        address += int16_t(insn.operands[2]);

        auto* entries = static_cast<const uint8_t*>(image.Find(address));
        if (entries == nullptr)
            return false;

        table.base = section.base + offset;
        for (size_t i = 0; i < count; i++)
            table.labels.push_back(ReadBE32(entries + i * 4));
        return true;
    }

    std::string SectionName(const Section& section)
    {
        // Section names are 8 bytes and not always NUL-terminated.
        std::string name;
        for (char c : section.name.substr(0, 8))
        {
            if (c < 0x20 || c > 0x7E)
                break;
            name += c;
        }
        return name;
    }
}

int main(int argc, char* argv[])
{
    std::vector<std::string> positional;
    std::string patchedPath;
    std::string switchPath;
    for (int i = 1; i < argc; i++)
    {
        std::string arg = argv[i];
        if (arg == "--patched" && i + 1 < argc)
            patchedPath = argv[++i];
        else if (arg == "--switch-tables" && i + 1 < argc)
            switchPath = argv[++i];
        else
            positional.push_back(arg);
    }

    if (positional.empty())
    {
        fmt::println("Usage: bo2scan <default.xex> [title update .xexp] [--patched out.xex] [--switch-tables out.toml]");
        return EXIT_FAILURE;
    }

    std::vector<uint8_t> file = LoadFile(positional[0]);
    if (file.empty())
    {
        fmt::println("ERROR: unable to read {}", positional[0]);
        return EXIT_FAILURE;
    }

    if (positional.size() >= 2)
    {
        std::vector<uint8_t> patch = LoadFile(positional[1]);
        std::vector<uint8_t> patched;
        if (patch.empty() ||
            XexPatcher::apply(file.data(), file.size(), patch.data(), patch.size(), patched, false) != XexPatcher::Result::Success)
        {
            fmt::println("ERROR: unable to apply title update {}", positional[1]);
            return EXIT_FAILURE;
        }
        file = std::move(patched);

        if (!patchedPath.empty())
        {
            std::ofstream stream(patchedPath, std::ios::binary);
            stream.write(reinterpret_cast<const char*>(file.data()), file.size());
            if (!stream)
            {
                fmt::println("ERROR: unable to write {}", patchedPath);
                return EXIT_FAILURE;
            }
            fmt::println("# Wrote patched XEX to {}", patchedPath);
        }
    }

    Image image = Image::ParseImage(file.data(), file.size());
    if (image.data == nullptr)
    {
        fmt::println("ERROR: {} is not a supported XEX", positional[0]);
        return EXIT_FAILURE;
    }

    fmt::println("# Image base 0x{:X}, size 0x{:X}, entry point 0x{:X}", image.base, image.size, image.entry_point);
    for (const auto& section : image.sections)
    {
        fmt::println("# Section {:<9} 0x{:08X} - 0x{:08X}{}", SectionName(section), section.base, section.base + section.size,
            (section.flags & SectionFlags_Code) ? " (code)" : "");
    }

    std::vector<size_t> helpers[std::size(kHelperPatterns)];
    std::vector<size_t> jmps[std::size(kJmpPatterns)];
    std::vector<SwitchTable> switches;
    size_t unreadSwitches = 0;

    for (const auto& section : image.sections)
    {
        if (!(section.flags & SectionFlags_Code))
            continue;

        for (size_t offset = 0; offset + 4 <= section.size; offset += 4)
        {
            for (size_t i = 0; i < std::size(kHelperPatterns); i++)
            {
                if (MatchWords(section, offset, kHelperPatterns[i].words))
                    helpers[i].push_back(section.base + offset);
            }

            for (size_t i = 0; i < std::size(kJmpPatterns); i++)
            {
                if (MatchWords(section, offset, kJmpPatterns[i].words))
                    jmps[i].push_back(FindFunctionStart(section, offset));
            }

            if (MatchSwitch(section, offset))
            {
                SwitchTable table;
                if (ReadSwitch(image, section, offset, table))
                    switches.push_back(std::move(table));
                else
                    unreadSwitches++;
            }
        }
    }

    fmt::println("\n# Paste into the [main] table of config/bo2_sp.toml. Check every value in a disassembler first.");
    auto printMatches = [](const Pattern& pattern, const std::vector<size_t>& found)
    {
        if (found.empty())
            fmt::println("# {} = NOT FOUND", pattern.key);
        else
            fmt::println("{} = 0x{:08X}{}", pattern.key, found.front(),
                found.size() > 1 ? fmt::format("  # {} matches, first one shown", found.size()) : "");
    };
    for (size_t i = 0; i < std::size(kHelperPatterns); i++)
        printMatches(kHelperPatterns[i], helpers[i]);
    for (size_t i = 0; i < std::size(kJmpPatterns); i++)
        printMatches(kJmpPatterns[i], jmps[i]);

    fmt::println("\n# {} jump tables found, {} matched the pattern but had no readable bounds check", switches.size(), unreadSwitches);

    std::vector<FunctionRange> pdata = ReadPdata(image);
    std::set<size_t> boundaries;
    for (const auto& fn : pdata)
    {
        boundaries.insert(fn.base);
        boundaries.insert(fn.base + fn.size);
    }
    for (const auto& section : image.sections)
    {
        if (!(section.flags & SectionFlags_Code))
            continue;
        for (size_t offset = 0; offset + 4 <= section.size; offset += 4)
        {
            uint32_t insn = ReadBE32(section.data + offset);
            if ((insn >> 26) == 18 && (insn & 3) == 1)
            {
                int32_t delta = int32_t(insn & 0x03FFFFFC);
                if (delta & 0x02000000)
                    delta -= 0x04000000;
                boundaries.insert(size_t(int64_t(section.base + offset) + delta));
            }
        }
    }

    std::vector<FunctionRange> extraFunctions;
    for (const auto& table : switches)
    {
        bool covered = false;
        for (const auto& fn : pdata)
        {
            if (table.base >= fn.base && table.base < fn.base + fn.size)
                covered = true;
        }
        if (covered)
            continue;

        const Section* section = nullptr;
        for (const auto& s : image.sections)
        {
            if (s == table.base)
                section = &s;
        }

        size_t start = FindFunctionStart(*section, table.base - section->base, 0x4000, &boundaries);
        size_t last = std::max<size_t>(table.base, table.defaultLabel);
        for (uint32_t label : table.labels)
            last = std::max<size_t>(last, label);

        // The function ends at the padding after its last case, or where the next function starts.
        size_t end = last;
        while (end < section->base + section->size && ReadBE32(section->data + (end - section->base)) != 0)
        {
            if (end > last && boundaries.count(end))
                break;
            end += 4;
        }

        if (start == 0)
        {
            fmt::println("# WARNING: could not find the start of the function containing the jump table at 0x{:X}", table.base);
            continue;
        }

        if (extraFunctions.empty() || extraFunctions.back().base != start)
            extraFunctions.push_back({ start, end - start });
        else
            extraFunctions.back().size = std::max(extraFunctions.back().size, end - start);
    }

    fmt::println("\n# Functions with jump tables and no .pdata entry. Paste into `functions` in config/bo2_sp.toml.");
    fmt::println("functions = [");
    for (const auto& fn : extraFunctions)
        fmt::println("    {{ address = 0x{:X}, size = 0x{:X} }},", fn.base, fn.size);
    fmt::println("]");

    if (!switchPath.empty())
    {
        std::string out = "# Generated by bo2scan\n";
        for (const auto& table : switches)
        {
            out += fmt::format("[[switch]]\nbase = 0x{:X}\nr = {}\ndefault = 0x{:X}\nlabels = [\n", table.base, table.r, table.defaultLabel);
            for (uint32_t label : table.labels)
                out += fmt::format("    0x{:X},\n", label);
            out += "]\n\n";
        }

        std::ofstream stream(switchPath);
        stream << out;
        if (!stream)
        {
            fmt::println("ERROR: unable to write {}", switchPath);
            return EXIT_FAILURE;
        }
        fmt::println("# Wrote switch tables to {}", switchPath);
    }

    return EXIT_SUCCESS;
}
