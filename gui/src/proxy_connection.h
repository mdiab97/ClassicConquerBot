#pragma once
#include <WinSock2.h>
#include <cstdint>
#include <string>
#include <vector>
#include <mutex>
#include <thread>
#include <atomic>

// AuthCipher (moved from app.h)
struct AuthCipher {
    static constexpr unsigned int Key[] = {
        0xE03D8976, 0xA15E0CD3, 0x2BF9B665, 0x31DEC40,
        0x6DC7BA57, 0x397E8736, 0x18F8AE92, 0x78E1CFC,
        0x408DB010, 0xAF24BDFA, 0xC406F203, 0x6289800,
        0xC9EDE8CB, 0x5C0297DD, 0x6F546005, 0xF7C0EC8F,
        0x320680DE, 0xCB01DD56, 0x8C76F546, 0x937273DB,
        0x2E343118, 0x7A5932FA, 0x535DFB4F, 0xBBAA69E3,
        0x6BDC2797, 0x10E40A7A, 0xDA3BF049, 0x89CFBA88,
        0x6D783B11, 0x67116416, 0x40C030DE, 0x7D84CA33
    };
    int counter{0};
    void reset() { counter = 0; }
    void decrypt(uint8_t* data, size_t size) {
        auto kb = reinterpret_cast<const uint8_t*>(Key);
        for (size_t i = 0; i < size; i++) {
            uint8_t b = data[i] ^ kb[counter++];
            counter %= 128;
            data[i] = ((b >> 4) | (b << 4)) ^ 0x3F;
        }
    }
    void encrypt(uint8_t* data, size_t size) {
        auto kb = reinterpret_cast<const uint8_t*>(Key);
        for (size_t i = 0; i < size; i++) {
            data[i] = (((data[i] ^ 0x3F) << 4) | ((data[i] ^ 0x3F) >> 4)) ^ kb[counter++];
            counter %= 128;
        }
    }
};

struct CipherSet {
    AuthCipher client_dec;
    AuthCipher server_enc;
    AuthCipher server_dec;
    AuthCipher client_enc;
    void reset() { client_dec.reset(); server_enc.reset(); server_dec.reset(); client_enc.reset(); }
};

// These types are defined in app.h BEFORE this header is included.
// Forward-declare for standalone compilation; the full defs are available at use sites.
struct HeroState;
struct NearbyEntity;
struct GroundItemInfo;
struct CapturedPacket;

struct EquipSlot {
    uint32_t item_id{0};
    uint32_t type_id{0};
    int plus{0};
    int gem1{0}, gem2{0};
};

struct InvItemInfo {
    uint32_t item_id{0};
    uint32_t type_id{0};
    int amount{0};
    int amount_limit{0};
    char name[17]{};
};

struct ProxyPortal {
    uint32_t id{0};
    int x{0}, y{0};
};

// Per-client proxy connection with full state
struct ProxyConnection {
    // Identity
    int account_index{-1};
    uint32_t dll_client_id{0};  // linked DLL IPC connection ID

    // Port pair
    uint16_t login_port{0};
    uint16_t game_port{0};

    // Listener sockets
    SOCKET login_listen{INVALID_SOCKET};
    SOCKET game_listen{INVALID_SOCKET};

    // Active session sockets
    SOCKET client_sock{INVALID_SOCKET};
    SOCKET server_sock{INVALID_SOCKET};
    bool is_game_session{false};  // true if currently relaying game (not login)

    // Cipher state
    CipherSet ciphers;

    // Game server address (from MsgConnectEx)
    uint32_t game_server_ip{0};
    uint16_t game_server_port{0};
    std::atomic<bool> has_game_addr{false};

    // Thread management
    std::thread accept_thread;
    std::atomic<bool> running{false};

    // Game state — full types available because app.h includes this after defining them
    uint32_t hero_id{0};
    HeroState hero_state;
    std::vector<NearbyEntity> entities;
    std::vector<GroundItemInfo> ground_items;
    std::vector<InvItemInfo> inventory;
    EquipSlot equip_slots[10]{};
    uint32_t map_idd{0};
    uint32_t doc_idd{0};
    std::vector<ProxyPortal> portals;
    std::mutex state_mutex;

    // Packet log
    std::vector<CapturedPacket> packets;
    size_t packet_max{2000};
    bool capture_on{true};
    std::mutex packet_mutex;

    // Inject queue
    std::mutex inject_mutex;
    std::vector<std::vector<uint8_t>> inject_to_server;

    // Cleanup
    void close_sockets() {
        if (client_sock != INVALID_SOCKET) { closesocket(client_sock); client_sock = INVALID_SOCKET; }
        if (server_sock != INVALID_SOCKET) { closesocket(server_sock); server_sock = INVALID_SOCKET; }
    }
    void close_listeners() {
        if (login_listen != INVALID_SOCKET) { closesocket(login_listen); login_listen = INVALID_SOCKET; }
        if (game_listen != INVALID_SOCKET) { closesocket(game_listen); game_listen = INVALID_SOCKET; }
    }
};
