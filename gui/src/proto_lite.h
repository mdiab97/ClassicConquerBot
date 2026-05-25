#pragma once
// ═══════════════════════════════════════════════════════════════════════
// PROTO_LITE — Lightweight protobuf wire format decoder/encoder
// ═══════════════════════════════════════════════════════════════════════
// Supports: varint, fixed32/64, length-delimited (string/bytes/submsg)
// No code generation needed — field schemas defined in C++ structs.

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>
#include <variant>
#include <unordered_map>

namespace proto {

// Wire types
enum WireType : uint8_t {
    VARINT = 0,
    FIXED64 = 1,
    LENGTH_DELIMITED = 2,
    FIXED32 = 5,
};

// A decoded field value
struct Value {
    uint32_t field_num{0};
    WireType wire_type{VARINT};
    uint64_t varint{0};         // for VARINT, FIXED32, FIXED64
    std::vector<uint8_t> bytes; // for LENGTH_DELIMITED

    int64_t as_int() const { return static_cast<int64_t>(varint); }
    uint64_t as_uint() const { return varint; }
    uint32_t as_uint32() const { return static_cast<uint32_t>(varint); }
    int32_t as_int32() const { return static_cast<int32_t>(varint); }
    std::string as_string() const { return {bytes.begin(), bytes.end()}; }
    float as_float() const { float f; uint32_t v = (uint32_t)varint; memcpy(&f,&v,4); return f; }
    double as_double() const { double d; memcpy(&d,&varint,8); return d; }
};

// Decoded message = list of fields
using Fields = std::vector<Value>;

// ── Decode ──────────────────────────────────────────────────────────

inline bool decode_varint(const uint8_t*& p, const uint8_t* end, uint64_t& out) {
    out = 0;
    int shift = 0;
    while (p < end) {
        uint8_t b = *p++;
        out |= (uint64_t)(b & 0x7F) << shift;
        if (!(b & 0x80)) return true;
        shift += 7;
        if (shift >= 64) return false;
    }
    return false;
}

inline Fields decode(const uint8_t* data, size_t size) {
    Fields fields;
    const uint8_t* p = data;
    const uint8_t* end = data + size;

    while (p < end) {
        uint64_t tag;
        if (!decode_varint(p, end, tag)) break;

        Value v;
        v.field_num = (uint32_t)(tag >> 3);
        v.wire_type = (WireType)(tag & 0x7);

        switch (v.wire_type) {
        case VARINT:
            if (!decode_varint(p, end, v.varint)) return fields;
            break;
        case FIXED64:
            if (p + 8 > end) return fields;
            memcpy(&v.varint, p, 8);
            p += 8;
            break;
        case FIXED32: {
            if (p + 4 > end) return fields;
            uint32_t tmp;
            memcpy(&tmp, p, 4);
            v.varint = tmp;
            p += 4;
            break;
        }
        case LENGTH_DELIMITED: {
            uint64_t len;
            if (!decode_varint(p, end, len)) return fields;
            if (p + len > end) return fields;
            v.bytes.assign(p, p + len);
            p += len;
            break;
        }
        default:
            return fields; // unknown wire type
        }

        fields.push_back(std::move(v));
    }
    return fields;
}

inline Fields decode(const std::vector<uint8_t>& data, size_t header_skip = 4) {
    if (data.size() <= header_skip) return {};
    return decode(data.data() + header_skip, data.size() - header_skip);
}

// ── Encode ──────────────────────────────────────────────────────────

inline void encode_varint(std::vector<uint8_t>& out, uint64_t val) {
    while (val > 0x7F) {
        out.push_back((uint8_t)(val & 0x7F) | 0x80);
        val >>= 7;
    }
    out.push_back((uint8_t)val);
}

inline void encode_tag(std::vector<uint8_t>& out, uint32_t field_num, WireType wt) {
    encode_varint(out, ((uint64_t)field_num << 3) | wt);
}

inline void encode_varint_field(std::vector<uint8_t>& out, uint32_t field_num, uint64_t val) {
    encode_tag(out, field_num, VARINT);
    encode_varint(out, val);
}

inline void encode_string_field(std::vector<uint8_t>& out, uint32_t field_num, const std::string& s) {
    encode_tag(out, field_num, LENGTH_DELIMITED);
    encode_varint(out, s.size());
    out.insert(out.end(), s.begin(), s.end());
}

inline void encode_bytes_field(std::vector<uint8_t>& out, uint32_t field_num, const std::vector<uint8_t>& b) {
    encode_tag(out, field_num, LENGTH_DELIMITED);
    encode_varint(out, b.size());
    out.insert(out.end(), b.begin(), b.end());
}

inline void encode_fixed32_field(std::vector<uint8_t>& out, uint32_t field_num, uint32_t val) {
    encode_tag(out, field_num, FIXED32);
    out.insert(out.end(), (uint8_t*)&val, (uint8_t*)&val + 4);
}

// Re-encode fields back to a protobuf body (no header)
inline std::vector<uint8_t> encode(const Fields& fields) {
    std::vector<uint8_t> out;
    for (auto& f : fields) {
        switch (f.wire_type) {
        case VARINT:
            encode_varint_field(out, f.field_num, f.varint);
            break;
        case FIXED64:
            encode_tag(out, f.field_num, FIXED64);
            out.insert(out.end(), (uint8_t*)&f.varint, (uint8_t*)&f.varint + 8);
            break;
        case FIXED32:
            encode_fixed32_field(out, f.field_num, (uint32_t)f.varint);
            break;
        case LENGTH_DELIMITED:
            encode_bytes_field(out, f.field_num, f.bytes);
            break;
        }
    }
    return out;
}

// Rebuild a full packet: [uint16 size][uint16 type][protobuf body]
inline std::vector<uint8_t> rebuild_packet(uint16_t msg_type, const Fields& fields) {
    auto body = encode(fields);
    uint16_t total = (uint16_t)(4 + body.size());
    std::vector<uint8_t> pkt(4 + body.size());
    memcpy(pkt.data(), &total, 2);
    memcpy(pkt.data() + 2, &msg_type, 2);
    memcpy(pkt.data() + 4, body.data(), body.size());
    return pkt;
}

// ── Field lookup helpers ────────────────────────────────────────────

inline const Value* find_field(const Fields& f, uint32_t num) {
    for (auto& v : f) if (v.field_num == num) return &v;
    return nullptr;
}

inline int64_t get_int(const Fields& f, uint32_t num, int64_t def = 0) {
    auto* v = find_field(f, num);
    return v ? v->as_int() : def;
}

inline uint64_t get_uint(const Fields& f, uint32_t num, uint64_t def = 0) {
    auto* v = find_field(f, num);
    return v ? v->as_uint() : def;
}

inline std::string get_string(const Fields& f, uint32_t num, const std::string& def = "") {
    auto* v = find_field(f, num);
    return v ? v->as_string() : def;
}

// ── Message type schema for display ─────────────────────────────────

enum FieldType { FT_INT32, FT_UINT32, FT_INT64, FT_UINT64, FT_STRING, FT_BYTES, FT_FIXED32, FT_FLOAT, FT_SUBMSG };

struct FieldSchema {
    uint32_t num;
    const char* name;
    FieldType type;
};

struct MsgSchema {
    uint16_t msg_type;
    const char* name;
    const FieldSchema* fields;
    int field_count;
};

// ── All message schemas ─────────────────────────────────────────────

#define SCHEMA(name, type, ...) \
    static constexpr FieldSchema name##_fields[] = { __VA_ARGS__ }; \
    static constexpr MsgSchema name##_schema = { type, #name, name##_fields, sizeof(name##_fields)/sizeof(name##_fields[0]) };

SCHEMA(MsgAuth, 1051,
    {1,"username",FT_STRING},{2,"password",FT_BYTES},{3,"server",FT_STRING},
    {4,"timestamp",FT_INT32},{5,"hiddentimestamp",FT_INT32},{6,"data",FT_BYTES})

SCHEMA(MsgTalk, 1004,
    {1,"color",FT_INT32},{2,"tone",FT_INT32},{4,"style",FT_INT32},
    {5,"sender",FT_STRING},{6,"recipient",FT_STRING},{7,"content",FT_STRING},{9,"mesh",FT_INT32})

SCHEMA(MsgWalk, 1005,
    {1,"playerid",FT_INT32},{2,"direction",FT_INT32},{3,"mode",FT_INT32})

SCHEMA(MsgUserInfo, 1006,
    {1,"userid",FT_INT32},{2,"mesh",FT_INT32},{3,"hair",FT_INT32},{4,"money",FT_INT64},
    {6,"experience",FT_INT64},{11,"strength",FT_INT32},{12,"agility",FT_INT32},
    {13,"vitality",FT_INT32},{14,"spirit",FT_INT32},{15,"attribpoints",FT_INT32},
    {16,"health",FT_INT32},{17,"maxhealth",FT_INT32},{18,"mana",FT_INT32},{19,"maxmana",FT_INT32},
    {20,"stamina",FT_INT32},{21,"level",FT_INT32},{22,"sublevel",FT_INT32},
    {23,"profession",FT_INT32},{24,"prevprofession",FT_INT32},{27,"map",FT_INT32},
    {28,"x",FT_INT32},{29,"y",FT_INT32},{30,"name",FT_STRING},{31,"spouse",FT_STRING})

SCHEMA(MsgItemInfo, 1008,
    {1,"itemid",FT_INT32},{2,"itemtypeid",FT_INT32},{3,"amount",FT_INT32},{4,"amountlimit",FT_INT32},
    {5,"mode",FT_INT32},{7,"position",FT_INT32},{8,"gem1",FT_INT32},{9,"gem2",FT_INT32},
    {10,"magic1",FT_INT32},{11,"magic2",FT_INT32},{12,"plus",FT_INT32},{15,"enchant",FT_INT32})

SCHEMA(MsgItem, 1009,
    {1,"itemid",FT_INT32},{2,"data1",FT_INT32},{3,"data2",FT_INT32},{4,"data3",FT_INT32},{5,"action",FT_INT32})

SCHEMA(MsgAction, 1010,
    {1,"action",FT_INT32},{2,"playerid",FT_INT32},{3,"timestamp",FT_INT32},
    {4,"x",FT_INT32},{5,"y",FT_INT32},{6,"dir",FT_INT32},{7,"shopid",FT_INT32},
    {8,"pkmode",FT_INT32},{10,"tox",FT_INT32},{11,"toy",FT_INT32})

SCHEMA(MsgPlayer, 1014,
    {1,"playerid",FT_UINT32},{2,"look",FT_UINT32},{3,"statusFlags",FT_UINT64},
    {4,"guildId",FT_UINT32},{6,"helmet",FT_UINT32},{7,"armor",FT_UINT32},
    {8,"rWeapon",FT_UINT32},{9,"lWeapon",FT_UINT32},{10,"garment",FT_UINT32},
    {11,"health",FT_UINT32},{13,"maxHealth",FT_UINT32},{14,"level",FT_UINT32},
    {15,"x",FT_UINT32},{16,"y",FT_UINT32},{17,"hair",FT_UINT32},{18,"facing",FT_UINT32},
    {19,"action",FT_UINT32},{24,"name",FT_STRING},{25,"scale",FT_UINT32},
    {29,"roleType",FT_UINT32},{30,"npcSort",FT_UINT32},{31,"title",FT_STRING},{32,"shopid",FT_UINT32})

SCHEMA(MsgUserAttrib, 1017,
    {1,"attributes",FT_SUBMSG})

SCHEMA(MsgInteract, 1022,
    {1,"playerId",FT_UINT32},{2,"targetId",FT_UINT32},{3,"x",FT_UINT32},{4,"y",FT_UINT32},
    {6,"action",FT_UINT32},{7,"damage",FT_UINT32},{8,"spellId",FT_UINT32},{9,"koCount",FT_UINT32})

SCHEMA(MsgTeam, 1023,
    {1,"action",FT_UINT32},{2,"data",FT_UINT32})

SCHEMA(MsgWeaponSkill, 1025,
    {1,"action",FT_UINT32},{3,"magicid",FT_UINT32},{5,"exp",FT_UINT64},{4,"level",FT_UINT32})

SCHEMA(MsgConnect, 1052,
    {1,"account",FT_INT32},{2,"session",FT_INT32},{4,"data",FT_STRING})

SCHEMA(MsgConnectEx, 1055,
    {1,"account",FT_INT32},{2,"session",FT_INT32},{3,"error",FT_INT32},
    {4,"host",FT_STRING},{5,"port",FT_INT32})

SCHEMA(MsgTrade, 1056,
    {1,"action",FT_UINT32},{2,"data",FT_UINT64})

SCHEMA(MsgMapItem, 1101,
    {1,"id",FT_FIXED32},{2,"type",FT_UINT32},{3,"x",FT_UINT32},{4,"y",FT_UINT32},
    {5,"data",FT_UINT32},{6,"action",FT_UINT32})

SCHEMA(MsgPackage, 1102,
    {1,"packageid",FT_UINT32},{2,"action",FT_UINT32},{3,"type",FT_UINT32},
    {4,"itemId",FT_UINT32},{5,"items",FT_SUBMSG},{6,"space",FT_UINT32})

SCHEMA(MsgMagicInfo, 1103,
    {1,"action",FT_UINT32},{3,"magicid",FT_UINT32},{5,"exp",FT_UINT64},{4,"level",FT_UINT32})

SCHEMA(MsgMagicEffect, 1105,
    {1,"playerid",FT_UINT32},{2,"targetid",FT_UINT32},{3,"x",FT_UINT32},{4,"y",FT_UINT32},
    {5,"spellId",FT_UINT32},{6,"spellLevel",FT_UINT32},{7,"targets",FT_SUBMSG})

SCHEMA(MsgMapInfo, 1110,
    {1,"mapidd",FT_UINT32},{2,"documentidd",FT_UINT32},{4,"mapflags",FT_UINT64})

SCHEMA(MsgNpcInfo, 2030,
    {1,"id",FT_UINT32},{5,"mode",FT_UINT32})

SCHEMA(MsgNpc, 2031,
    {1,"id",FT_UINT32},{5,"mode",FT_UINT32})

SCHEMA(MsgTaskDialog, 2032,
    {3,"taskId",FT_UINT32},{4,"data",FT_UINT32},{5,"optionId",FT_UINT32},
    {6,"action",FT_UINT32},{7,"text",FT_STRING})

SCHEMA(MsgPing, 7001,
    {1,"timestamp",FT_UINT64})

SCHEMA(MsgDateTime, 7002,
    {1,"year",FT_UINT32},{2,"month",FT_UINT32},{3,"day",FT_UINT32},{4,"dayOfYear",FT_UINT32},
    {5,"hour",FT_UINT32},{6,"minute",FT_UINT32},{7,"second",FT_UINT32})

SCHEMA(MsgVip, 7004,
    {1,"mode",FT_UINT32},{3,"vipStatus",FT_UINT32},{4,"vipInfo",FT_BYTES},
    {6,"teleportOption",FT_UINT32},{7,"toggleOption",FT_UINT32})

SCHEMA(MsgEventShop, 7010,
    {1,"action",FT_UINT32},{2,"itemsJson",FT_STRING},{3,"itemType",FT_UINT32})

SCHEMA(MsgComposeBank, 7015,
    {1,"action",FT_UINT32},{2,"data",FT_BYTES},{3,"itemSubtype",FT_UINT32},
    {4,"itemPlus",FT_UINT32},{5,"itemCount",FT_UINT32})

#undef SCHEMA

inline const MsgSchema* all_schemas[] = {
    &MsgAuth_schema, &MsgTalk_schema, &MsgWalk_schema, &MsgUserInfo_schema,
    &MsgItemInfo_schema, &MsgItem_schema, &MsgAction_schema, &MsgPlayer_schema,
    &MsgUserAttrib_schema, &MsgInteract_schema, &MsgTeam_schema, &MsgWeaponSkill_schema,
    &MsgConnect_schema, &MsgConnectEx_schema, &MsgTrade_schema, &MsgMapItem_schema,
    &MsgPackage_schema, &MsgMagicInfo_schema, &MsgMagicEffect_schema, &MsgMapInfo_schema,
    &MsgNpcInfo_schema, &MsgNpc_schema, &MsgTaskDialog_schema, &MsgPing_schema,
    &MsgDateTime_schema, &MsgVip_schema, &MsgEventShop_schema, &MsgComposeBank_schema,
};
constexpr int num_schemas = sizeof(all_schemas) / sizeof(all_schemas[0]);

inline const MsgSchema* find_schema(uint16_t msg_type) {
    for (int i = 0; i < num_schemas; i++)
        if (all_schemas[i]->msg_type == msg_type) return all_schemas[i];
    return nullptr;
}

// Format a field value as string for display
inline std::string format_field(const Value& v, const FieldSchema* fs) {
    if (fs) {
        switch (fs->type) {
        case FT_STRING: return "\"" + v.as_string() + "\"";
        case FT_BYTES: {
            std::string hex;
            for (auto b : v.bytes) { char h[4]; snprintf(h,4,"%02X ",b); hex += h; }
            return hex;
        }
        case FT_INT32: return std::to_string(v.as_int32());
        case FT_UINT32: return std::to_string(v.as_uint32());
        case FT_INT64: return std::to_string(v.as_int());
        case FT_UINT64: return std::to_string(v.as_uint());
        case FT_FIXED32: return std::to_string(v.as_uint32());
        case FT_FLOAT: return std::to_string(v.as_float());
        default: break;
        }
    }
    // Fallback by wire type
    if (v.wire_type == LENGTH_DELIMITED) return "\"" + v.as_string() + "\"";
    return std::to_string(v.as_int());
}

} // namespace proto
