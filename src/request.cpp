#include "request.hpp"
#include <cstdlib>
#include <set>
#if defined(__x86_64__)
#define RAPIDJSON_SSE2
#endif
#include "rapidjson/document.h"

namespace mvd {
namespace {
Json reference(const std::string& line) {
    std::vector<std::set<std::string>> objects;
    auto callback = [&](int, Json::parse_event_t event, Json& value) {
        using Event = Json::parse_event_t;
        if (event == Event::object_start) objects.emplace_back();
        else if (event == Event::object_end) objects.pop_back();
        else if (event == Event::key && !objects.back().insert(value.get<std::string>()).second)
            fail("invalid_request", "duplicate JSON object key");
        return true;
    };
    return Json::parse(line, callback);
}
struct CheckedAllocator {
    static const bool kNeedFree = true;
    void* Malloc(size_t size) {
        if (!size) return nullptr;
        if (void* memory = std::malloc(size)) return memory;
        throw std::bad_alloc();
    }
    void* Realloc(void* original, size_t, size_t size) {
        if (!size) { std::free(original); return nullptr; }
        if (void* memory = std::realloc(original, size)) return memory;
        throw std::bad_alloc();
    }
    static void Free(void* memory) { std::free(memory); }
};
using Pool = rapidjson::MemoryPoolAllocator<CheckedAllocator>;
using Document = rapidjson::GenericDocument<rapidjson::UTF8<>, Pool, CheckedAllocator>;
using WireValue = rapidjson::GenericValue<rapidjson::UTF8<>, Pool>;
}
Json parse_request_json(const std::string& line, bool* fast) {
    if (fast) *fast = false;
    if (line.find('\0') != std::string::npos) fail("invalid_request", "raw NUL in JSON request");
    // Validate raw UTF-8 before using RapidJSON's bulk string-copy path. Its
    // strict escape decoder also rejects unpaired UTF-16 surrogates. Padding
    // keeps all SSE2 string scans within owned memory, including tiny frames.
    try { utf8_length(line); }
    catch (const Error&) { return reference(line); }
    std::string input; input.reserve(line.size() + 32); input = line; input.append(32, '\0');
    Document document;
    document.ParseInsitu<rapidjson::kParseIterativeFlag>(input.data());
    if (document.HasParseError()) return reference(line);
    Json result;
    struct Pending { const WireValue* source; Json* target; };
    std::vector<Pending> pending{{&document, &result}};
    while (!pending.empty()) {
        auto [source, target] = pending.back(); pending.pop_back();
        if (source->IsObject()) {
            *target = Json::object();
            auto& object = target->get_ref<Json::object_t&>();
            for (auto member = source->MemberBegin(); member != source->MemberEnd(); ++member) {
                std::string name(member->name.GetString(), member->name.GetStringLength());
                auto [item, inserted] = object.try_emplace(std::move(name), nullptr);
                if (!inserted) fail("invalid_request", "duplicate JSON object key");
                pending.push_back({&member->value, &item->second});
            }
        } else if (source->IsArray()) {
            *target = Json::array(); auto& array = target->get_ref<Json::array_t&>();
            array.resize(source->Size());
            for (rapidjson::SizeType i = 0; i < source->Size(); ++i) pending.push_back({&(*source)[i], &array[i]});
        } else if (source->IsString()) *target = std::string(source->GetString(), source->GetStringLength());
        else if (source->IsBool()) *target = source->GetBool();
        else if (source->IsNull()) *target = nullptr;
        // Float IDs and numbers outside 64-bit integer range retain the exact
        // established decoder, including unusual exponents and signed zero.
        else if (source->IsDouble()) return reference(line);
        else if (source->IsUint64()) *target = source->GetUint64();
        else *target = source->GetInt64();
    }
    if (fast) *fast = true;
    return result;
}
}
