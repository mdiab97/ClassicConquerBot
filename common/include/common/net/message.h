#pragma once
#include <cstdint>
#include <vector>
#include <string>
#include <cstring>

namespace ccbot::net {

enum class MsgId : uint16_t {
    None = 0,
    Handshake = 1,
    HwidSpoof = 10,
    HwidResult = 11,
    ClientInfo = 20,
    PosUpdate = 30,
    StateUpdate = 31,    // DLL -> GUI: full hero state snapshot
    Jump = 50,
    Walk = 51,
    Run = 52,
    Attack = 53,
    PickUp = 54,
    MagicAttack = 55,
    MagicAttackTarget = 70,  // magic with target ID
    Revive = 71,
    PathfindTo = 72,      // GUI -> DLL: pathfind to x,y
    PathfindStop = 73,    // GUI -> DLL: stop pathfinding
    PathfindStatus = 74,  // DLL -> GUI: path status
    DebugCells = 75,      // GUI -> DLL: dump cells around hero
    Login = 80,           // GUI -> DLL: set username/password and login
    UseItem = 56,
    ListInventory = 57,
    InventoryList = 58,
    SearchHp = 59,       // GUI -> DLL: search for HP value
    RequestState = 60,   // GUI -> DLL: request state snapshot
    ListNearby = 61,     // GUI -> DLL: list nearby entities
    NearbyList = 62,     // DLL -> GUI: entity list response
    ListItems = 63,      // GUI -> DLL: list ground items
    ItemList = 64,       // DLL -> GUI: ground item list response
    ScanAll = 40,
    SearchInt = 41,
    SearchString = 42,
    BotStart = 90,
    BotStop = 91,
    BotStatus = 92,
    ScatterTest = 93,   // GUI -> DLL: cast scatter at (x,y) for data collection
    DumpEntity = 94,    // GUI -> DLL: dump entity memory by ID + optional search value
    DialogOptions = 95, // DLL -> GUI: current dialog options
    DialogAnswer = 96,  // GUI -> DLL: answer dialog with option ID
    PortalList = 97,    // DLL -> GUI: list of portals on current map
    PortalGo = 98,      // GUI -> DLL: navigate to portal index
    ActivateNpc = 99,    // GUI -> DLL: activate/talk to NPC by entity ID
    CrossMapInit = 110,  // GUI -> DLL: load gateway DB (sends data path)
    CrossMapTravel = 111,// GUI -> DLL: travel to dest map ID
    CrossMapStop = 112,  // GUI -> DLL: stop traveling
    CrossMapStatus = 113,// DLL -> GUI: travel status update
    CrossMapMaps = 114,  // DLL -> GUI: list of all reachable maps
    DepositAll = 115,    // GUI -> DLL: VIP deposit all items
    PortalExplore = 116, // GUI -> DLL: explore unknown portal by index
    PortalDiscovered = 117, // DLL -> GUI: discovered portal destination
    VipTeleport = 118,      // GUI -> DLL: VIP teleport (body: int32 city_idx)
    ExploreAll = 119,       // GUI -> DLL: explore all unknown portals recursively
    StopExploreAll = 120,   // GUI -> DLL: stop explore-all
    WarehouseDeposit = 121, // GUI -> DLL: deposit item (body: uint32 item_id)
    WarehouseStatus = 122,  // DLL -> GUI: warehouse state (body: uint8 open, uint32 pkg_id)
    DepositNow = 123,       // GUI -> DLL: force auto-deposit now
    StopDeposit = 124,      // GUI -> DLL: cancel auto-deposit
    Shutdown = 125,         // GUI -> DLL: gracefully exit
    // Network proxy
    NetPackets = 130,       // DLL -> GUI: batch of captured packets
    NetInject = 131,        // GUI -> DLL: inject a raw packet to server
    NetControl = 132,       // GUI -> DLL: enable/disable/clear capture
    SetProxyPort = 140, // GUI -> DLL: uint16_t login_port, uint16_t game_port
    Log = 100,
};

#pragma pack(push, 1)
struct MessageHeader {
    MsgId id{MsgId::None};
    uint32_t size{0};
};
#pragma pack(pop)

class Message {
public:
    MessageHeader header{};
    std::vector<uint8_t> body;

    Message() = default;
    explicit Message(MsgId id) { header.id = id; }

    size_t total_size() const { return sizeof(MessageHeader) + body.size(); }

    // Push data into the message body
    template <typename T>
    Message& operator<<(const T& data) {
        static_assert(std::is_trivially_copyable_v<T>);
        size_t old = body.size();
        body.resize(old + sizeof(T));
        std::memcpy(body.data() + old, &data, sizeof(T));
        header.size = static_cast<uint32_t>(body.size());
        return *this;
    }

    // Pop data from the message body
    template <typename T>
    Message& operator>>(T& data) {
        static_assert(std::is_trivially_copyable_v<T>);
        size_t new_size = body.size() - sizeof(T);
        std::memcpy(&data, body.data() + new_size, sizeof(T));
        body.resize(new_size);
        header.size = static_cast<uint32_t>(body.size());
        return *this;
    }

    // Push a string (length-prefixed)
    void push_string(const std::string& str) {
        uint32_t len = static_cast<uint32_t>(str.size());
        size_t old = body.size();
        body.resize(old + sizeof(uint32_t) + len);
        std::memcpy(body.data() + old, &len, sizeof(uint32_t));
        std::memcpy(body.data() + old + sizeof(uint32_t), str.data(), len);
        header.size = static_cast<uint32_t>(body.size());
    }

    // Pop a string (length-prefixed) from current read position
    std::string pop_string(size_t& offset) const {
        uint32_t len = 0;
        std::memcpy(&len, body.data() + offset, sizeof(uint32_t));
        offset += sizeof(uint32_t);
        std::string str(reinterpret_cast<const char*>(body.data() + offset), len);
        offset += len;
        return str;
    }
};

} // namespace ccbot::net
