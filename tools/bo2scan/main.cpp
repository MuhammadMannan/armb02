// bo2scan: reads the Black Ops II Xbox 360 executable (optionally applying a
// title update .xexp) and produces what XenonRecomp needs that XenonAnalyse
// can't find on its own:
//   - the register save/restore helper addresses
//   - the setjmp/longjmp addresses
//   - the jump tables, written as a switch table TOML. BO2's compiler schedules
//     the table load as lis/rlwinm/addi/lwzx/mtctr/bctr, which XenonAnalyse's
//     Sonic Unleashed patterns (lis/addi/rlwinm/...) don't match.
//   - explicit boundaries for functions that contain jump tables or `bdz` chains
//     (small switches on CTR) but have no .pdata entry. XenonRecomp's analyser
//     otherwise cuts them short at the bctr or the last bdz.

#include <cstdio>
#include <algorithm>
#include <cstring>
#include <fstream>
#include <map>
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

    constexpr uint32_t kBctr = 0x4E800420;

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

    // A jump table ends in `mtctr; bctr`. Black Ops II's compiler orders and pads the address
    // arithmetic in several ways (lis/rlwinm/addi/lwzx; lis/addi/rlwinm/lhzx/lis/addi/nop/add; ...),
    // so rather than match fixed patterns, evaluate the straight-line code before the bctr:
    //   absolute: the target is loaded with lwzx from a known table address;
    //   offset:   a byte or halfword is loaded with lbzx/lhzx, optionally shifted with rlwinm,
    //             and added to a known code address.
    // Then, like XenonAnalyse's ScanTable, walk back to the bounds check for the index register,
    // the case count and the default label. Returns false if the bctr isn't a recognisable table.
    bool ReadSwitch(const Image& image, const Section& section, size_t bctr, SwitchTable& table, bool& isTable)
    {
        isTable = false;
        ppc_insn insn;

        // The straight-line block before the bctr: back to the nearest branch, at most 16 instructions.
        size_t first = bctr;
        while (first >= 4 && bctr - first < 16 * 4)
        {
            uint32_t op = ReadBE32(section.data + first - 4) >> 26;
            if (op == 16 || op == 18 || op == 19)
                break;
            first -= 4;
        }

        std::map<uint32_t, uint32_t> known;  // registers holding a known address
        uint32_t tableAddress = 0, loadSize = 0, scaled = UINT32_MAX, shift = 0, sum = UINT32_MAX, codeBase = 0, ctr = UINT32_MAX;
        size_t tableLis = 0;
        std::map<uint32_t, size_t> lisAt;
        std::map<uint32_t, uint32_t> scaledFrom;  // rlwinm rD,rS,2,0,29 before the load: rD -> rS
        uint32_t index = UINT32_MAX;              // the register the table load is indexed by
        for (size_t at = first; at < bctr; at += 4)
        {
            ppc::Disassemble(section.data + at, section.base + at, insn);
            if (insn.opcode == nullptr)
                return false;
            const uint32_t* o = insn.operands;
            switch (insn.opcode->id)
            {
            case PPC_INST_NOP:
                break;
            case PPC_INST_LIS:
                known[o[0]] = o[1] << 16;
                lisAt[o[0]] = section.base + at;
                break;
            case PPC_INST_ADDI:
                if (known.count(o[1]))
                {
                    known[o[0]] = known[o[1]] + int16_t(o[2]);
                    lisAt[o[0]] = lisAt[o[1]];
                }
                else
                    known.erase(o[0]);
                break;
            case PPC_INST_LWZX:
            case PPC_INST_LHZX:
            case PPC_INST_LBZX:
            {
                uint32_t reg = known.count(o[1]) ? o[1] : known.count(o[2]) ? o[2] : UINT32_MAX;
                if (reg == UINT32_MAX)
                    return false;
                uint32_t offsetReg = reg == o[1] ? o[2] : o[1];
                index = scaledFrom.count(offsetReg) ? scaledFrom[offsetReg] : offsetReg;
                tableAddress = known[reg];
                tableLis = lisAt[reg];
                loadSize = insn.opcode->id == PPC_INST_LWZX ? 4 : insn.opcode->id == PPC_INST_LHZX ? 2 : 1;
                known.erase(o[0]);
                scaled = o[0];
                break;
            }
            case PPC_INST_RLWINM:
                if (o[1] == scaled && loadSize != 0)
                {
                    shift = o[2];
                    scaled = o[0];
                }
                else
                {
                    known.erase(o[0]);
                    if (loadSize == 0 && o[2] == 2 && o[3] == 0 && o[4] == 29)
                        scaledFrom[o[0]] = o[1];
                }
                break;
            case PPC_INST_ADD:
                if (o[1] == scaled && known.count(o[2]))
                    codeBase = known[o[2]];
                else if (o[2] == scaled && known.count(o[1]))
                    codeBase = known[o[1]];
                else
                {
                    known.erase(o[0]);
                    break;
                }
                sum = o[0];
                break;
            case PPC_INST_MTCTR:
                ctr = o[0];
                break;
            default:
                if (insn.opcode->operands[0] != 0)  // most instructions write their first operand
                    known.erase(o[0]);
                break;
            }
        }

        bool absolute = loadSize == 4 && ctr == scaled && shift == 0;
        bool offset = loadSize != 0 && loadSize < 4 && ctr == sum;
        if (!absolute && !offset)
            return false;
        isTable = true;

        uint32_t cr = UINT32_MAX;
        size_t count = 0;
        for (size_t i = 1; i <= 40 && i * 4 <= bctr; i++)
        {
            size_t at = bctr - i * 4;
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

        auto* entries = static_cast<const uint8_t*>(image.Find(tableAddress));
        if (entries == nullptr)
            return false;

        // No bounds check in reach (zlib's inflate checks its mode elsewhere): an absolute table
        // that the code right after it is the lowest target of, as compilers lay them out, ends
        // where that code starts.
        if (count == 0 && absolute && index != UINT32_MAX)
        {
            uint32_t lowest = UINT32_MAX;
            for (size_t i = 0; i < 1024; i++)
            {
                uint32_t at = tableAddress + uint32_t(i * 4);
                if (at == lowest)
                {
                    count = i;
                    break;
                }
                uint32_t label = ReadBE32(entries + i * 4);
                if (label <= tableAddress || label < section.base || label >= section.base + section.size || (label & 3))
                    break;
                lowest = std::min(lowest, label);
            }
            if (count == 0)
                return false;
            table.r = index;
            table.defaultLabel = ReadBE32(entries);
        }

        if (count == 0)
            return false;

        // Keyed at the lis of the table address, as XenonAnalyse does; no other bctr lies between it and ours.
        table.base = tableLis;
        for (size_t i = 0; i < count; i++)
        {
            if (absolute)
                table.labels.push_back(ReadBE32(entries + i * 4));
            else if (loadSize == 2)
                table.labels.push_back(codeBase + ((uint32_t(entries[i * 2]) << 8 | entries[i * 2 + 1]) << shift));
            else
                table.labels.push_back(codeBase + (uint32_t(entries[i]) << shift));
        }
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
    std::string imagePath;
    for (int i = 1; i < argc; i++)
    {
        std::string arg = argv[i];
        if (arg == "--patched" && i + 1 < argc)
            patchedPath = argv[++i];
        else if (arg == "--switch-tables" && i + 1 < argc)
            switchPath = argv[++i];
        else if (arg == "--image" && i + 1 < argc)
            imagePath = argv[++i];
        else
            positional.push_back(arg);
    }

    if (positional.empty())
    {
        fmt::println("Usage: bo2scan <default.xex> [title update .xexp] [--patched out.xex] [--switch-tables out.toml] [--image out.bin]");
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
    if (!imagePath.empty())
    {
        // The loaded image as the game sees it in memory, for scripts/find_entry_points.py.
        std::ofstream stream(imagePath, std::ios::binary);
        stream.write(reinterpret_cast<const char*>(image.data.get()), image.size);
        if (!stream)
        {
            fmt::println("ERROR: unable to write {}", imagePath);
            return EXIT_FAILURE;
        }
        fmt::println("# Wrote the loaded image to {}", imagePath);
    }
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

            if (ReadBE32(section.data + offset) == kBctr)
            {
                SwitchTable table;
                bool isTable;
                if (ReadSwitch(image, section, offset, table, isTable))
                    switches.push_back(std::move(table));
                else if (isTable)
                {
                    fmt::println("# Jump table without a readable bounds check at bctr 0x{:X}", section.base + offset);
                    unreadSwitches++;
                }
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

    fmt::println("\n# {} jump tables found, {} looked like jump tables but had no readable bounds check", switches.size(), unreadSwitches);

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

    // Every jump table and every forward `bdz` is a site whose targets must stay
    // inside the function that contains it.
    struct JumpSite
    {
        size_t base{};
        size_t last{};
    };
    std::vector<JumpSite> sites;
    for (const auto& table : switches)
    {
        size_t last = std::max<size_t>(table.base, table.defaultLabel);
        for (uint32_t label : table.labels)
            last = std::max<size_t>(last, label);
        sites.push_back({ table.base, last });
    }
    for (const auto& section : image.sections)
    {
        if (!(section.flags & SectionFlags_Code))
            continue;
        for (size_t offset = 0; offset + 4 <= section.size; offset += 4)
        {
            uint32_t insn = ReadBE32(section.data + offset);
            uint32_t bo = (insn >> 21) & 0x1F;
            if ((insn >> 26) == 16 && (insn & 3) == 0 && (bo & 0x1E) == 0x12)
            {
                int32_t delta = int16_t(insn & 0xFFFC);
                if (delta > 0)
                    sites.push_back({ section.base + offset, section.base + offset + delta });
            }
        }
    }
    std::sort(sites.begin(), sites.end(), [](const JumpSite& a, const JumpSite& b) { return a.base < b.base; });

    std::vector<FunctionRange> extraFunctions;
    for (const auto& site : sites)
    {
        bool covered = false;
        for (const auto& fn : pdata)
        {
            if (site.base >= fn.base && site.base < fn.base + fn.size)
                covered = true;
        }
        if (covered)
            continue;

        const Section* section = nullptr;
        for (const auto& s : image.sections)
        {
            if (s == site.base)
                section = &s;
        }

        size_t start = FindFunctionStart(*section, site.base - section->base, 0x4000, &boundaries);
        size_t last = site.last;

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
            fmt::println("# WARNING: could not find the start of the function containing the jump at 0x{:X}", site.base);
            continue;
        }

        if (extraFunctions.empty() || extraFunctions.back().base != start)
            extraFunctions.push_back({ start, end - start });
        else
            extraFunctions.back().size = std::max(extraFunctions.back().size, end - start);
    }

    // A few .pdata functions with jump tables follow a couple of words of data that aren't zero
    // padding. XenonRecomp would analyse those words as the start of another function that runs
    // on into the real one and ends at its bctr, so the table's cases land outside it. Naming the
    // words as a function of their own keeps the analyser out.
    for (const auto& table : switches)
    {
        const FunctionRange* owner = nullptr;
        size_t previousEnd = 0;
        for (const auto& fn : pdata)
        {
            if (table.base >= fn.base && table.base < fn.base + fn.size)
                owner = &fn;
        }
        if (owner == nullptr)
            continue;
        for (const auto& fn : pdata)
        {
            if (fn.base + fn.size <= owner->base)
                previousEnd = std::max(previousEnd, fn.base + fn.size);
        }
        const Section* section = nullptr;
        for (const auto& s : image.sections)
        {
            if (s == owner->base)
                section = &s;
        }
        size_t gapStart = owner->base;
        while (gapStart > previousEnd && owner->base - gapStart < 16 &&
               ReadBE32(section->data + (gapStart - 4 - section->base)) != 0)
            gapStart -= 4;
        bool bounded = gapStart == previousEnd || ReadBE32(section->data + (gapStart - 4 - section->base)) == 0;
        if (gapStart < owner->base && bounded)
            extraFunctions.push_back({ gapStart, owner->base - gapStart });
    }
    std::sort(extraFunctions.begin(), extraFunctions.end(), [](const FunctionRange& a, const FunctionRange& b) { return a.base < b.base; });
    extraFunctions.erase(std::unique(extraFunctions.begin(), extraFunctions.end(),
        [](const FunctionRange& a, const FunctionRange& b) { return a.base == b.base; }), extraFunctions.end());

    fmt::println("\n# Functions with jump tables or bdz chains and no .pdata entry. Paste into `functions` in config/bo2_sp.toml.");
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
