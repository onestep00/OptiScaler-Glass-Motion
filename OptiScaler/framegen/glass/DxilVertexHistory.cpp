#include "pch.h"
#include "DxilVertexHistory.h"
#include <algorithm>
#include <array>
#include <cstdint>
#include <map>
#include <regex>
#include <sstream>
#include <stdexcept>
#include <vector>

namespace GlassFg
{
namespace
{
using Parts = std::vector<std::string>;
void need(bool value, const char* reason)
{
    if (!value)
        throw std::runtime_error(reason);
}
std::string trim(std::string value)
{
    const auto first = value.find_first_not_of(" \t\r\n");
    return first == std::string::npos ? std::string {}
                                      : value.substr(first, value.find_last_not_of(" \t\r\n") - first + 1);
}
Parts split(const std::string& value)
{
    Parts result;
    size_t start = 0;
    int depth = 0;
    bool quoted = false;
    for (size_t i = 0; i < value.size(); ++i)
    {
        const char c = value[i];
        if (c == '"' && (!i || value[i - 1] != '\\'))
            quoted = !quoted;
        if (quoted)
            continue;
        if (c == '(' || c == '[' || c == '{' || c == '<')
            ++depth;
        if (c == ')' || c == ']' || c == '}' || c == '>')
            --depth;
        need(depth >= 0, "Malformed metadata nesting");
        if (c == ',' && depth == 0)
        {
            result.push_back(trim(value.substr(start, i - start)));
            start = i + 1;
        }
    }
    need(depth == 0 && !quoted, "Unterminated metadata");
    result.push_back(trim(value.substr(start)));
    return result;
}
std::string join(const Parts& parts)
{
    std::string result;
    for (const auto& part : parts)
        result += (result.empty() ? "" : ", ") + part;
    return result;
}
unsigned number(const std::string& value, const char* prefix)
{
    const std::string p(prefix);
    need(value.starts_with(p), "Unexpected metadata type");
    const auto digits = value.substr(p.size());
    need(!digits.empty() && digits.find_first_not_of("0123456789") == std::string::npos, "Invalid metadata integer");
    const auto n = std::stoull(digits);
    need(n <= UINT32_MAX, "Metadata integer overflow");
    return static_cast<unsigned>(n);
}
struct Metadata
{
    std::map<unsigned, std::string> nodes;
    unsigned next = 0;
    std::string& get(const std::string& reference) { return nodes.at(number(reference, "!")); }
    std::string add(std::string value)
    {
        const auto index = next++;
        nodes.emplace(index, std::move(value));
        return "!" + std::to_string(index);
    }
    void append(std::string& reference, const std::string& value)
    {
        if (reference == "null")
            reference = add(value);
        else
            get(reference) += ", " + value;
    }
};
struct Signature
{
    Parts fields;
    unsigned id = 0, row = 0, rows = 0, columns = 0;
    explicit Signature(const std::string& source) : fields(split(source))
    {
        need(fields.size() == 11, "Unsupported signature metadata");
        id = number(fields[0], "i32 ");
        rows = number(fields[6], "i32 ");
        columns = number(fields[7], "i8 ");
        row = number(fields[8], "i32 ");
        need(rows && columns && columns <= 4 && row + rows <= 32, "Unsupported signature packing");
    }
};
unsigned extent(Metadata& metadata, const Parts& list)
{
    unsigned result = 0;
    for (const auto& node : list)
    {
        const Signature sig(metadata.get(node));
        result = std::max(result, sig.row + sig.rows);
    }
    return result;
}
unsigned nextId(Metadata& metadata, const Parts& list)
{
    unsigned result = 0;
    for (const auto& node : list)
        result = std::max(result, Signature(metadata.get(node)).id + 1);
    return result;
}
std::string single(const std::string& source, const std::regex& pattern, size_t group, const char* reason)
{
    const std::sregex_iterator begin(source.begin(), source.end(), pattern), end;
    need(begin != end, reason);
    auto it = begin;
    const auto value = (*it)[group].str();
    need(++it == end, reason);
    return value;
}
} // namespace

VertexHistoryShader RewriteVertexHistory(std::string_view disassembly)
{
    VertexHistoryShader result;
    try
    {
        need(!disassembly.empty() && disassembly.size() <= 2 * 1024 * 1024, "Invalid shader size");
        std::string source(disassembly);
        while (!source.empty() && source.back() == '\0')
            source.pop_back();
        need(source.find('\0') == std::string::npos, "Embedded NUL");
        need(source.find("%glass.") == std::string::npos && source.find("GLASS_PREVIOUS") == std::string::npos &&
                 source.find("GLASS_HISTORY_MISSING") == std::string::npos,
             "Already instrumented shader");
        single(source, std::regex(R"(define void @[^\n]+\{)"), 0, "Exactly one VS entry required");
        single(source, std::regex(R"(  ret void)"), 0, "Multiple shader returns unsupported");
        Metadata metadata;
        std::string body;
        std::istringstream lines(source);
        std::string line;
        const std::regex ordinary(R"(^!(\d+) = !\{(.*)\}$)"), any(R"(^!(\d+) = .*$)");
        while (std::getline(lines, line))
        {
            if (!line.empty() && line.back() == '\r')
                line.pop_back();
            std::smatch match;
            if (std::regex_match(line, match, any))
                metadata.next = std::max(metadata.next, number(match[1], "") + 1);
            if (std::regex_match(line, match, ordinary))
                metadata.nodes.emplace(number(match[1], ""), match[2]);
            else if (!line.starts_with("!dx.viewIdState"))
                body += line + "\n";
        }
        const auto model = single(source, std::regex(R"(!dx.shaderModel = !\{(!\d+)\})"), 1, "Missing shader model");
        need(metadata.get(model) == "!\"vs\", i32 6, i32 0", "Only DXIL VS 6.0 is currently validated");
        need(source.find("!dx.rootSignature") == std::string::npos, "Embedded root signature unsupported");
        const auto ep = single(source, std::regex(R"(!dx.entryPoints = !\{(!\d+)\})"), 1, "Missing entry point");
        auto entry = split(metadata.get(ep));
        need(entry.size() == 5, "Unsupported entry point");
        auto signatures = split(metadata.get(entry[2]));
        need(signatures.size() == 3 && signatures[2] == "null", "Unsupported vertex signature");
        auto inputs = split(metadata.get(signatures[0])), outputs = split(metadata.get(signatures[1]));
        auto resources = entry[3] == "null" ? Parts { "null", "null", "null", "null" } : split(metadata.get(entry[3]));
        need(resources.size() == 4 && resources[1] == "null", "Shaders with existing UAVs are not replay safe");
        for (const auto* op : { "dx.op.atomic", "dx.op.bufferStore", "dx.op.rawBufferStore", "dx.op.textureStore",
                                "dx.op.barrier", "dx.op.traceRay", "dx.op.callShader" })
            need(source.find(op) == std::string::npos, "Shader side effect unsupported");
        const auto zero = metadata.add("i32 0"), mask1 = metadata.add("i32 3, i32 1"),
                   mask15 = metadata.add("i32 3, i32 15");
        auto inputSystem = [&](const char* name, unsigned kind)
        {
            for (const auto& node : inputs)
            {
                Signature s(metadata.get(node));
                if (_stricmp(s.fields[1].c_str(), (std::string("!\"") + name + "\"").c_str()) != 0)
                    continue;
                need(s.fields[2] == "i8 5" && s.fields[3] == "i8 " + std::to_string(kind) && s.rows == 1 &&
                         s.columns == 1 && s.fields[9] == "i8 0",
                     "Invalid system input");
                if (s.fields[10] == "null")
                    s.fields[10] = mask1;
                else
                {
                    // Retain all existing extension tags; only mark X used.
                    auto flags = split(metadata.get(s.fields[10]));
                    need(flags.size() % 2 == 0, "Malformed signature flags");
                    bool found = false;
                    for (size_t i = 0; i < flags.size(); i += 2)
                        if (flags[i] == "i32 3")
                        {
                            flags[i + 1] = "i32 " + std::to_string(number(flags[i + 1], "i32 ") | 1u);
                            found = true;
                        }
                    if (!found)
                    {
                        flags.push_back("i32 3");
                        flags.push_back("i32 1");
                    }
                    s.fields[10] = metadata.add(join(flags));
                }
                metadata.get(node) = join(s.fields);
                return s.id;
            }
            const auto id = nextId(metadata, inputs), row = extent(metadata, inputs);
            need(row < 32, "No input register available");
            inputs.push_back(metadata.add("i32 " + std::to_string(id) + ", !\"" + name + "\", i8 5, i8 " +
                                          std::to_string(kind) + ", " + zero + ", i8 0, i32 1, i8 1, i32 " +
                                          std::to_string(row) + ", i8 0, " + mask1));
            return id;
        };
        const auto vertex = inputSystem("SV_VertexID", 1), instance = inputSystem("SV_InstanceID", 2);
        metadata.get(signatures[0]) = join(inputs);
        unsigned position = UINT32_MAX;
        for (const auto& node : outputs)
        {
            const Signature s(metadata.get(node));
            if (_stricmp(s.fields[1].c_str(), "!\"SV_Position\"") == 0)
            {
                need(position == UINT32_MAX && s.rows == 1 && s.columns == 4 && s.fields[9] == "i8 0",
                     "Invalid clip position");
                position = s.id;
            }
        }
        need(position != UINT32_MAX, "No clip position output");
        std::array<std::string, 4> values;
        for (unsigned c = 0; c < 4; ++c)
            values[c] = single(source,
                               std::regex("call void @dx.op.storeOutput.f32\\(i32 5, i32 " + std::to_string(position) +
                                          ", i32 0, i8 " + std::to_string(c) + ", float ([^\\)]+)\\)"),
                               1, "Position must have one scalar store per component");
        const auto row = extent(metadata, outputs), output = nextId(metadata, outputs);
        need(row + 2 <= 32, "No history varying registers available");
        outputs.push_back(metadata.add("i32 " + std::to_string(output) + ", !\"GLASS_PREVIOUS\", i8 9, i8 0, " + zero +
                                       ", i8 2, i32 1, i8 4, i32 " + std::to_string(row) + ", i8 0, " + mask15));
        outputs.push_back(metadata.add("i32 " + std::to_string(output + 1) +
                                       ", !\"GLASS_HISTORY_MISSING\", i8 9, i8 0, " + zero +
                                       ", i8 2, i32 1, i8 1, i32 " + std::to_string(row + 1) + ", i8 0, " + mask1));
        metadata.get(signatures[1]) = join(outputs);
        unsigned ids[3] {};
        for (unsigned cls = 0; cls < 3; ++cls)
        {
            if (resources[cls] != "null")
                for (const auto& node : split(metadata.get(resources[cls])))
                {
                    const auto fields = split(metadata.get(node));
                    need(fields.size() >= 7 && fields[3] != "i32 31", "Reserved shader space collision");
                    ids[cls] = std::max(ids[cls], number(fields[0], "i32 ") + 1);
                }
            std::string description = "i32 " + std::to_string(ids[cls]);
            if (cls == 0)
                description += ", %Glass.RawRead* undef, !\"GlassHistory\", i32 31, i32 0, i32 1, i32 11, i32 0, null";
            if (cls == 1)
                description += ", %Glass.RawWrite* undef, !\"GlassNext\", i32 31, i32 0, i32 1, i32 11, i1 false, i1 "
                               "false, i1 false, null";
            if (cls == 2)
                description += ", %Glass.Constants* undef, !\"GlassConstants\", i32 31, i32 0, i32 1, i32 32, null";
            metadata.append(resources[cls], metadata.add(description));
        }
        if (entry[3] == "null")
        {
            entry[3] = metadata.add(join(resources));
            need(body.find("!dx.resources") == std::string::npos, "Unexpected empty resource metadata");
            body += "!dx.resources = !{" + entry[3] + "}\n";
        }
        else
            metadata.get(entry[3]) = join(resources);
        auto flags = entry[4] == "null" ? Parts {} : split(metadata.get(entry[4]));
        need(flags.size() % 2 == 0, "Malformed shader flags");
        bool foundFlags = false;
        for (size_t i = 0; i < flags.size(); i += 2)
            if (flags[i] == "i32 0")
            {
                need(flags[i + 1].starts_with("i64 "), "Invalid shader flags");
                flags[i + 1] = "i64 " + std::to_string(std::stoull(flags[i + 1].substr(4)) | 65552ull);
                foundFlags = true;
            }
        if (!foundFlags)
        {
            flags.push_back("i32 0");
            flags.push_back("i64 65552");
        }
        entry[4] = metadata.add(join(flags));
        metadata.get(ep) = join(entry);
        // Eight constants: allocation base, vertex count, raw-index origin,
        // instance origin (normally zero), instance count, generation, current
        // frame and expected previous frame. StartVertex/StartInstanceLocation
        // are IA offsets and must not be subtracted from these system values.
        std::ostringstream code;
        code << "\n  br label %glass.entry\nglass.entry:\n";
        const char* names[] = { "srv", "uav", "cb" };
        for (unsigned cls = 0; cls < 3; ++cls)
            code << "  %glass." << names[cls] << " = call %dx.types.Handle @dx.op.createHandle(i32 57, i8 " << cls
                 << ", i32 " << ids[cls] << ", i32 0, i1 false)\n";
        code << "  %glass.v = call i32 @dx.op.loadInput.i32(i32 4, i32 " << vertex << ", i32 0, i8 0, i32 undef)\n"
             << "  %glass.i = call i32 @dx.op.loadInput.i32(i32 4, i32 " << instance << ", i32 0, i8 0, i32 undef)\n";
        for (unsigned i = 0; i < 2; ++i)
            code
                << "  %glass.c" << i
                << " = call %dx.types.CBufRet.i32 @dx.op.cbufferLoadLegacy.i32(i32 59, %dx.types.Handle %glass.cb, i32 "
                << i << ")\n";
        const char* constants[] = { "base", "count", "originv", "origini", "instances", "gen", "frame", "prevframe" };
        for (unsigned i = 0; i < 8; ++i)
            code << "  %glass." << constants[i] << " = extractvalue %dx.types.CBufRet.i32 %glass.c" << i / 4 << ", "
                 << i % 4 << "\n";
        code << R"(
  %glass.lv = sub i32 %glass.v, %glass.originv
  %glass.li = sub i32 %glass.i, %glass.origini
  %glass.vok = icmp ult i32 %glass.lv, %glass.count
  %glass.iok = icmp ult i32 %glass.li, %glass.instances
  %glass.ok = and i1 %glass.vok, %glass.iok
  br i1 %glass.ok, label %glass.read, label %glass.end
glass.read:
  %glass.offset = mul i32 %glass.li, %glass.count
  %glass.index0 = add i32 %glass.offset, %glass.lv
  %glass.index = add i32 %glass.index0, %glass.base
  %glass.address = shl i32 %glass.index, 5
  %glass.tagaddress = or i32 %glass.address, 16
  %glass.p = call %dx.types.ResRet.i32 @dx.op.bufferLoad.i32(i32 68, %dx.types.Handle %glass.srv, i32 %glass.address, i32 undef)
  %glass.tag = call %dx.types.ResRet.i32 @dx.op.bufferLoad.i32(i32 68, %dx.types.Handle %glass.srv, i32 %glass.tagaddress, i32 undef)
  %glass.oldframe = extractvalue %dx.types.ResRet.i32 %glass.tag, 0
  %glass.oldgen = extractvalue %dx.types.ResRet.i32 %glass.tag, 1
  %glass.frameok = icmp eq i32 %glass.oldframe, %glass.prevframe
  %glass.genok = icmp eq i32 %glass.oldgen, %glass.gen
  %glass.validbool = and i1 %glass.frameok, %glass.genok
  %glass.valid = select i1 %glass.validbool, float 0.000000e+00, float 1.000000e+00
)";
        for (unsigned c = 0; c < 4; ++c)
            code << "  %glass.p" << c << "i = extractvalue %dx.types.ResRet.i32 %glass.p, " << c << "\n"
                 << "  %glass.p" << c << " = bitcast i32 %glass.p" << c << "i to float\n"
                 << "  %glass.c" << c << "i = bitcast float " << values[c] << " to i32\n";
        code
            << R"(  call void @dx.op.bufferStore.i32(i32 69, %dx.types.Handle %glass.uav, i32 %glass.address, i32 undef, i32 %glass.c0i, i32 %glass.c1i, i32 %glass.c2i, i32 %glass.c3i, i8 15)
  call void @dx.op.bufferStore.i32(i32 69, %dx.types.Handle %glass.uav, i32 %glass.tagaddress, i32 undef, i32 %glass.frame, i32 %glass.gen, i32 undef, i32 undef, i8 3)
  br label %glass.end
glass.end:
)";
        for (unsigned c = 0; c < 4; ++c)
            code << "  %glass.o" << c << " = phi float [ %glass.p" << c << ", %glass.read ], [ " << values[c]
                 << ", %glass.entry ]\n";
        code << "  %glass.ov = phi float [ %glass.valid, %glass.read ], [ 1.000000e+00, %glass.entry ]\n";
        for (unsigned c = 0; c < 4; ++c)
            code << "  call void @dx.op.storeOutput.f32(i32 5, i32 " << output << ", i32 0, i8 " << c
                 << ", float %glass.o" << c << ")\n";
        code << "  call void @dx.op.storeOutput.f32(i32 5, i32 " << output + 1
             << ", i32 0, i8 0, float %glass.ov)\n  ret void";
        body.replace(body.find("  ret void"), 10, code.str());
        const std::pair<const char*, const char*> types[] = { { "dx.types.Handle", "{ i8* }" },
                                                              { "dx.types.CBufRet.i32", "{ i32, i32, i32, i32 }" },
                                                              { "dx.types.ResRet.i32", "{ i32, i32, i32, i32, i32 }" },
                                                              { "Glass.RawRead", "{ i32 }" },
                                                              { "Glass.RawWrite", "{ i32 }" },
                                                              { "Glass.Constants",
                                                                "{ i32, i32, i32, i32, i32, i32, i32, i32 }" } };
        for (const auto& [name, fields] : types)
            if (body.find(std::string("%") + name + " = type") == std::string::npos)
                body.insert(body.find("define void "), std::string("%") + name + " = type " + fields + "\n\n");
        for (const auto* declaration :
             { "%dx.types.Handle @dx.op.createHandle(i32, i8, i32, i32, i1)",
               "i32 @dx.op.loadInput.i32(i32, i32, i32, i8, i32)",
               "%dx.types.CBufRet.i32 @dx.op.cbufferLoadLegacy.i32(i32, %dx.types.Handle, i32)",
               "%dx.types.ResRet.i32 @dx.op.bufferLoad.i32(i32, %dx.types.Handle, i32, i32)",
               "void @dx.op.bufferStore.i32(i32, %dx.types.Handle, i32, i32, i32, i32, i32, i32, i8)" })
        {
            const auto text = std::string("declare ") + declaration;
            if (body.find(text) == std::string::npos)
                body += text + "\n";
        }
        for (const auto& [id, value] : metadata.nodes)
            body += "!" + std::to_string(id) + " = !{" + value + "}\n";
        result.assembly = std::move(body);
        result.previousRegister = row;
        result.missingRegister = row + 1;
    }
    catch (const std::exception& error)
    {
        result.error = error.what();
    }
    return result;
}
VertexHistoryShader RewriteMaterialMotion(std::string_view disassembly, MaterialSource sourceFactor,
                                          MaterialDestination destinationFactor)
{
    VertexHistoryShader result;
    try
    {
        need(!disassembly.empty() && disassembly.size() <= 2 * 1024 * 1024, "Invalid shader size");
        std::string source(disassembly);
        while (!source.empty() && source.back() == '\0')
            source.pop_back();
        need(source.find('\0') == std::string::npos && source.find("%glass.") == std::string::npos,
             "Malformed/already instrumented shader");
        single(source, std::regex(R"(define void @[^\n]+\{)"), 0, "Exactly one PS entry required");
        single(source, std::regex(R"(  ret void)"), 0, "Multiple shader returns unsupported");
        Metadata metadata;
        std::string body, line;
        std::istringstream lines(source);
        const std::regex ordinary(R"(^!(\d+) = !\{(.*)\}$)"), any(R"(^!(\d+) = .*$)");
        while (std::getline(lines, line))
        {
            if (!line.empty() && line.back() == '\r')
                line.pop_back();
            std::smatch match;
            if (std::regex_match(line, match, any))
                metadata.next = std::max(metadata.next, number(match[1], "") + 1);
            if (std::regex_match(line, match, ordinary))
                metadata.nodes.emplace(number(match[1], ""), match[2]);
            else if (!line.starts_with("!dx.viewIdState"))
                body += line + "\n";
        }
        const auto model = single(source, std::regex(R"(!dx.shaderModel = !\{(!\d+)\})"), 1, "Missing shader model");
        need(metadata.get(model) == "!\"ps\", i32 6, i32 0", "Only DXIL PS 6.0 is currently validated");
        need(source.find("!dx.rootSignature") == std::string::npos, "Embedded root signature unsupported");
        const auto ep = single(source, std::regex(R"(!dx.entryPoints = !\{(!\d+)\})"), 1, "Missing entry point");
        auto entry = split(metadata.get(ep));
        need(entry.size() == 5, "Unsupported entry point");
        auto signatures = split(metadata.get(entry[2]));
        need(signatures.size() == 3 && signatures[2] == "null", "Unsupported pixel signature");
        auto inputs = split(metadata.get(signatures[0])), outputs = split(metadata.get(signatures[1]));
        auto resources = entry[3] == "null" ? Parts { "null", "null", "null", "null" } : split(metadata.get(entry[3]));
        need(resources.size() == 4 && resources[1] == "null", "Pixel UAV side effects unsupported");
        for (const auto* op : { "dx.op.atomic", "dx.op.bufferStore", "dx.op.rawBufferStore", "dx.op.textureStore",
                                "dx.op.barrier", "dx.op.traceRay", "dx.op.callShader" })
            need(source.find(op) == std::string::npos, "Pixel shader side effect unsupported");
        const auto zero = metadata.add("i32 0"), mask15 = metadata.add("i32 3, i32 15"),
                   mask1 = metadata.add("i32 3, i32 1");
        unsigned position = UINT32_MAX;
        for (const auto& node : inputs)
        {
            Signature s(metadata.get(node));
            if (_stricmp(s.fields[1].c_str(), "!\"SV_Position\"") != 0)
                continue;
            need(position == UINT32_MAX && s.rows == 1 && s.columns == 4 && s.fields[9] == "i8 0",
                 "Unsupported pixel position");
            position = s.id;
            s.fields[10] = mask15;
            metadata.get(node) = join(s.fields);
        }
        if (position == UINT32_MAX)
        {
            position = nextId(metadata, inputs);
            const auto row = extent(metadata, inputs);
            need(row < 32, "No pixel position register available");
            inputs.push_back(metadata.add("i32 " + std::to_string(position) + ", !\"SV_Position\", i8 9, i8 3, " +
                                          zero + ", i8 4, i32 1, i8 4, i32 " + std::to_string(row) + ", i8 0, " +
                                          mask15));
        }
        const auto previous = nextId(metadata, inputs), row = extent(metadata, inputs);
        need(row + 2 <= 32, "No pixel history registers available");
        inputs.push_back(metadata.add("i32 " + std::to_string(previous) + ", !\"GLASS_PREVIOUS\", i8 9, i8 0, " + zero +
                                      ", i8 2, i32 1, i8 4, i32 " + std::to_string(row) + ", i8 0, " + mask15));
        inputs.push_back(metadata.add("i32 " + std::to_string(previous + 1) +
                                      ", !\"GLASS_HISTORY_MISSING\", i8 9, i8 0, " + zero +
                                      ", i8 2, i32 1, i8 1, i32 " + std::to_string(row + 1) + ", i8 0, " + mask1));
        metadata.get(signatures[0]) = join(inputs);
        std::array<std::array<std::string, 4>, 2> color;
        for (const auto& node : outputs)
        {
            const Signature s(metadata.get(node));
            need(_stricmp(s.fields[1].c_str(), "!\"SV_Target\"") == 0 && s.rows == 1 && s.fields[9] == "i8 0",
                 "Only color-export pixel shaders are replayed");
            const auto semanticIndex = number(metadata.get(s.fields[4]), "i32 ");
            if (semanticIndex >= color.size())
                continue;
            for (unsigned c = 0; c < s.columns; ++c)
            {
                const std::regex store("call void @dx.op.storeOutput.f32\\(i32 5, i32 " + std::to_string(s.id) +
                                       ", i32 0, i8 " + std::to_string(c) + ", float ([^\\)]+)\\)");
                std::sregex_iterator it(source.begin(), source.end(), store), end;
                if (it == end)
                    continue;
                size_t lastEnd = 0;
                for (; it != end; ++it)
                {
                    color[semanticIndex][c] = (*it)[1];
                    lastEnd = static_cast<size_t>(it->position() + it->length());
                }
                const auto returnAt = source.find("  ret void", lastEnd);
                need(returnAt != std::string::npos, "Color export does not precede final return");
                const auto tail = source.substr(lastEnd, returnAt - lastEnd);
                // Several REDengine shaders initialize a target to zero and
                // overwrite it in the same final block. The final store wins.
                // Never select an arbitrary branch-local value for the exit.
                need(!std::regex_search(
                         tail, std::regex(
                                   R"(\n\s*(br |switch |indirectbr |unreachable|; <label>|[A-Za-z_][A-Za-z_0-9.]*:))")),
                     "Branch-local final color store unsupported");
            }
        }
        auto component = [&](unsigned target, unsigned column)
        {
            need(!color[target][column].empty() && color[target][column] != "undef",
                 "Required material output missing");
            return color[target][column];
        };
        unsigned constantId = 0;
        if (resources[2] != "null")
            for (const auto& node : split(metadata.get(resources[2])))
            {
                const auto fields = split(metadata.get(node));
                need(fields.size() >= 7 && fields[3] != "i32 31", "Reserved shader constant space collision");
                constantId = std::max(constantId, number(fields[0], "i32 ") + 1);
            }
        metadata.append(
            resources[2],
            metadata.add(
                "i32 " + std::to_string(constantId) +
                ", %Glass.PixelConstants* undef, !\"GlassPixelConstants\", i32 31, i32 1, i32 1, i32 32, null"));
        if (entry[3] == "null")
        {
            entry[3] = metadata.add(join(resources));
            need(body.find("!dx.resources") == std::string::npos, "Unexpected empty resource metadata");
            body += "!dx.resources = !{" + entry[3] + "}\n";
        }
        else
            metadata.get(entry[3]) = join(resources);
        metadata.get(signatures[1]) =
            metadata.add("i32 0, !\"SV_Target\", i8 9, i8 16, " + zero + ", i8 0, i32 1, i8 4, i32 0, i8 0, " + mask15);
        metadata.get(ep) = join(entry);
        // Removing void color stores does not rename or remove any SSA value.
        body = std::regex_replace(body, std::regex(R"(  call void @dx.op.storeOutput\.[^\n]+\n)"), "");
        std::ostringstream code;
        code << "\n  br label %glass.pixel\nglass.pixel:\n";
        code << "  %glass.cb = call %dx.types.Handle @dx.op.createHandle(i32 57, i8 2, i32 " << constantId
             << ", i32 1, i1 false)\n";
        for (unsigned i = 0; i < 2; ++i)
            code
                << "  %glass.c" << i
                << " = call %dx.types.CBufRet.f32 @dx.op.cbufferLoadLegacy.f32(i32 59, %dx.types.Handle %glass.cb, i32 "
                << i << ")\n";
        for (unsigned c = 0; c < 4; ++c)
        {
            code << "  %glass.p" << c << " = call float @dx.op.loadInput.f32(i32 4, i32 " << previous << ", i32 0, i8 "
                 << c << ", i32 undef)\n";
            code << "  %glass.v" << c << " = extractvalue %dx.types.CBufRet.f32 %glass.c0, " << c << "\n";
        }
        for (unsigned c = 0; c < 3; ++c)
            code << "  %glass.s" << c << " = call float @dx.op.loadInput.f32(i32 4, i32 " << position << ", i32 0, i8 "
                 << c << ", i32 undef)\n";
        code << "  %glass.valid = call float @dx.op.loadInput.f32(i32 4, i32 " << previous + 1
             << ", i32 0, i8 0, i32 undef)\n";
        code << R"(  %glass.vok = fcmp oeq float %glass.valid, 0.000000e+00
  %glass.wok = fcmp ogt float %glass.p3, 0.000000e+00
  %glass.ok = and i1 %glass.vok, %glass.wok
  %glass.bad = xor i1 %glass.ok, true
  call void @dx.op.discard(i32 82, i1 %glass.bad)
)";
        for (unsigned c = 0; c < 2; ++c)
        {
            code << "  %glass.local" << c << " = fsub float %glass.s" << c << ", %glass.v" << c << "\n"
                 << "  %glass.uv" << c << " = fmul float %glass.local" << c << ", %glass.v" << c + 2 << "\n"
                 << "  %glass.ndc" << c << " = fdiv float %glass.p" << c << ", %glass.p3\n"
                 << "  %glass.half" << c << " = fmul float %glass.ndc" << c << ", "
                 << (c ? "-5.000000e-01" : "5.000000e-01") << "\n"
                 << "  %glass.prev" << c << " = fadd float %glass.half" << c << ", 5.000000e-01\n"
                 << "  %glass.raw" << c << " = fsub float %glass.prev" << c << ", %glass.uv" << c << "\n"
                 << "  %glass.j" << c << " = extractvalue %dx.types.CBufRet.f32 %glass.c1, " << c << "\n"
                 << "  %glass.mv" << c << " = fadd float %glass.raw" << c << ", %glass.j" << c << "\n";
        }
        for (unsigned c = 0; c < 3; ++c)
        {
            std::string transmission;
            switch (destinationFactor)
            {
            case MaterialDestination::Zero:
                transmission = "0.000000e+00";
                break;
            case MaterialDestination::One:
                transmission = "1.000000e+00";
                break;
            case MaterialDestination::Alpha:
                transmission = component(0, 3);
                break;
            case MaterialDestination::OneMinusAlpha:
                code << "  %glass.inv" << c << " = fsub float 1.000000e+00, " << component(0, 3) << "\n";
                transmission = "%glass.inv" + std::to_string(c);
                break;
            case MaterialDestination::SecondSourceRgb:
                transmission = component(1, c);
                break;
            default:
                need(false, "Unsupported destination factor");
            }
            code << "  %glass.t" << c << " = call float @dx.op.unary.f32(i32 7, float " << transmission << ")\n";
            std::string contribution = "0.000000e+00";
            if (sourceFactor == MaterialSource::One)
                contribution = component(0, c);
            else if (sourceFactor == MaterialSource::Alpha)
            {
                code << "  %glass.f" << c << " = fmul float " << component(0, c) << ", " << component(0, 3) << "\n";
                contribution = "%glass.f" + std::to_string(c);
            }
            else
                need(sourceFactor == MaterialSource::Zero, "Unsupported source factor");
            code << "  %glass.fhas" << c << " = fcmp one float " << contribution << ", 0.000000e+00\n"
                 << "  %glass.thas" << c << " = fcmp one float " << transmission << ", 1.000000e+00\n"
                 << "  %glass.has" << c << " = or i1 %glass.fhas" << c << ", %glass.thas" << c << "\n";
        }
        code << R"(  %glass.has01 = or i1 %glass.has0, %glass.has1
  %glass.hasall = or i1 %glass.has01, %glass.has2
  %glass.empty = xor i1 %glass.hasall, true
  call void @dx.op.discard(i32 82, i1 %glass.empty)
  %glass.sum01 = fadd float %glass.t0, %glass.t1
  %glass.sum = fadd float %glass.sum01, %glass.t2
  %glass.mean = fdiv float %glass.sum, 3.000000e+00
  %glass.alpha = fsub float 1.000000e+00, %glass.mean
  %glass.finite0 = call i1 @dx.op.isSpecialFloat.f32(i32 10, float %glass.mv0)
  %glass.finite1 = call i1 @dx.op.isSpecialFloat.f32(i32 10, float %glass.mv1)
  %glass.finite2 = call i1 @dx.op.isSpecialFloat.f32(i32 10, float %glass.alpha)
  %glass.finite01 = and i1 %glass.finite0, %glass.finite1
  %glass.finite = and i1 %glass.finite01, %glass.finite2
  %glass.nonfinite = xor i1 %glass.finite, true
  call void @dx.op.discard(i32 82, i1 %glass.nonfinite)
  call void @dx.op.storeOutput.f32(i32 5, i32 0, i32 0, i8 0, float %glass.mv0)
  call void @dx.op.storeOutput.f32(i32 5, i32 0, i32 0, i8 1, float %glass.mv1)
  call void @dx.op.storeOutput.f32(i32 5, i32 0, i32 0, i8 2, float %glass.alpha)
  call void @dx.op.storeOutput.f32(i32 5, i32 0, i32 0, i8 3, float %glass.s2)
  ret void)";
        body.replace(body.find("  ret void"), 10, code.str());
        for (const auto& [name, fields] : std::array<std::pair<const char*, const char*>, 3> {
                 { { "dx.types.Handle", "{ i8* }" },
                   { "dx.types.CBufRet.f32", "{ float, float, float, float }" },
                   { "Glass.PixelConstants", "{ float, float, float, float, float, float, float, float }" } } })
            if (body.find(std::string("%") + name + " = type") == std::string::npos)
                body.insert(body.find("define void "), std::string("%") + name + " = type " + fields + "\n\n");
        for (const auto* declaration :
             { "%dx.types.Handle @dx.op.createHandle(i32, i8, i32, i32, i1)",
               "%dx.types.CBufRet.f32 @dx.op.cbufferLoadLegacy.f32(i32, %dx.types.Handle, i32)",
               "float @dx.op.loadInput.f32(i32, i32, i32, i8, i32)", "float @dx.op.unary.f32(i32, float)",
               "void @dx.op.discard(i32, i1)", "i1 @dx.op.isSpecialFloat.f32(i32, float)" })
        {
            const auto text = std::string("declare ") + declaration;
            if (body.find(text) == std::string::npos)
                body += text + "\n";
        }
        for (const auto& [id, value] : metadata.nodes)
            body += "!" + std::to_string(id) + " = !{" + value + "}\n";
        result.assembly = std::move(body);
        result.previousRegister = row;
        result.missingRegister = row + 1;
    }
    catch (const std::exception& error)
    {
        result.error = error.what();
    }
    return result;
}
} // namespace GlassFg
