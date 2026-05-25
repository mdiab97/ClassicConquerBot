#pragma once
#include "common/net/tcp_server.h"
#include "common/net/message.h"
#include <d3d11.h>
#include <string>
#include <vector>
#include <mutex>
#include <atomic>
#include <thread>
#include <memory>
#include <unordered_map>
#include "wdf.h"
#include <WinSock2.h>
#include <WS2tcpip.h>
#pragma comment(lib, "ws2_32.lib")

struct HeroState {
    uint32_t char_id{0};
    int pos_x{0}, pos_y{0};
    int64_t silver{0};
    int stamina{0}, max_stamina{0};
    int pk_mode{0};
    int hp{0}, max_hp{0}, mp{0}, max_mp{0};
    int level{0};
    int revive_countdown{0};
    int cmd_type{0}, cmd_status{0};
    int64_t status_flag{0};
    uint16_t bool_flags{0};
    uint32_t map_id{0};
    int map_w{0}, map_h{0};
    int world_w{0}, world_h{0};  // pixel dimensions of world space
    std::string char_name;

    bool is_dead()      const { return (bool_flags >> 0) & 1; }
    bool is_ghost()     const { return (bool_flags >> 1) & 1; }
    bool is_moving()    const { return (bool_flags >> 2) & 1; }
    bool is_jumping()   const { return (bool_flags >> 3) & 1; }
    bool is_attacking() const { return (bool_flags >> 4) & 1; }
    bool is_acting()    const { return (bool_flags >> 5) & 1; }
    bool is_npc_active()const { return (bool_flags >> 6) & 1; }
    bool is_flying()    const { return (bool_flags >> 7) & 1; }
    bool is_invisible() const { return (bool_flags >> 8) & 1; }
    bool is_poisoned()  const { return (bool_flags >> 9) & 1; }
    bool is_frozen()    const { return (bool_flags >> 10) & 1; }
    bool is_xp_full()   const { return (bool_flags >> 11) & 1; }
    bool is_red()       const { return (bool_flags >> 12) & 1; }
    bool is_black()     const { return (bool_flags >> 13) & 1; }
    bool is_vip()       const { return (bool_flags >> 14) & 1; }
};

struct NearbyEntity {
    uint32_t id{0};
    int x{0}, y{0};
    int type{0};        // 6=Hero, 7=Player
    int role_kind{0};   // 4=Monster, 5=Pet, 6=Guard, 8=Booth, 9=NPC
    int npc_sort{0};    // 0=None, 1=Shop, 2=Task, 3=Warehouse, 6=Forge
    uint16_t look_type{0};
    bool dead{false};
    char name[17]{};
};

struct GroundItemInfo {
    uint32_t item_id{0};
    uint32_t type_id{0};
    int x{0}, y{0};
};

struct DialogOptionInfo {
    int type{0};    // 1=Button, 2=Edit
    int id{0};
    std::string text;
};

struct CapturedPacket {
    bool outgoing;
    uint16_t type;
    std::vector<uint8_t> raw;
    int64_t timestamp;
};

struct ConnectedClient {
    uint32_t id{0};
    std::string name;
    HeroState state;
    std::vector<NearbyEntity> nearby;
    std::vector<GroundItemInfo> ground_items;
    bool connected{false};
};

// Include proxy_connection.h AFTER struct definitions so forward decls resolve
#include "proxy_connection.h"

class App {
public:
    bool init(HWND hwnd);
    void shutdown();
    void render_frame();

    ID3D11Device* device() const { return device_; }
    ID3D11DeviceContext* device_context() const { return device_context_; }
    IDXGISwapChain* swap_chain() const { return swap_chain_; }
    ID3D11RenderTargetView* rtv() const { return rtv_; }

    void resize(UINT width, UINT height);

private:
    void init_d3d(HWND hwnd);
    void cleanup_d3d();
    void create_rtv();

    void render_ui();

    // Network callbacks
    void on_client_connect(ccbot::net::TcpConnection::Ptr conn);
    void on_client_disconnect(ccbot::net::TcpConnection::Ptr conn);
    void on_client_message(ccbot::net::TcpConnection::Ptr conn, ccbot::net::Message msg);

    // D3D11
    ID3D11Device* device_{nullptr};
    ID3D11DeviceContext* device_context_{nullptr};
    IDXGISwapChain* swap_chain_{nullptr};
    ID3D11RenderTargetView* rtv_{nullptr};

    // Networking
    std::unique_ptr<ccbot::net::TcpServer> server_;

    // State
    std::mutex clients_mutex_;
    std::vector<ConnectedClient> clients_;
    std::vector<std::string> log_lines_;

    // DLL path (for launch+inject)
    std::string dll_path_;

    // Launch + config
    std::string client_path_;   // root folder of the game client
    std::string config_path_;   // config.json path (next to exe)
    bool launching_{false};

    // Action UI state
    int path_x_{0}, path_y_{0};

    // Multi-account system
    struct AccountProfile {
        std::string username;
        std::string password;
        int server{0};
        std::string char_name;   // filled after login
        uint32_t client_id{0};   // DLL connection ID (0 = not connected)
        bool launched{false};
        bool connected{false};
    };
    std::vector<AccountProfile> accounts_;
    int active_account_{-1};     // which account the right panel controls (-1 = none)
    char new_account_user_[64]{};
    char new_account_pass_[64]{};
    int new_account_server_{0};

    // Get the active client ID (from selected account or first connected)
    uint32_t get_active_client_id() {
        std::lock_guard lock(clients_mutex_);
        return get_active_client_id_unlocked();
    }

    // Call with clients_mutex_ already held
    uint32_t get_active_client_id_unlocked() {
        if (active_account_ >= 0 && active_account_ < (int)accounts_.size()) {
            uint32_t cid = accounts_[active_account_].client_id;
            if (cid) {
                for (auto& c : clients_)
                    if (c.id == cid && c.connected) return cid;
            }
        }
        for (auto& c : clients_)
            if (c.connected) return c.id;
        return 0;
    }

    // Get the active client's data (call with lock held)
    ConnectedClient* get_active_client_unlocked() {
        uint32_t cid = get_active_client_id_unlocked();
        if (!cid) return nullptr;
        for (auto& c : clients_)
            if (c.id == cid) return &c;
        return nullptr;
    }

    // Legacy compat
    char login_user_[64]{};
    char login_pass_[64]{};
    int login_server_{0};
    bool warehouse_open_{false};
    uint32_t warehouse_pkg_id_{0};

    // Dialog state
    bool dialog_open_{false};
    std::vector<DialogOptionInfo> dialog_options_;

    // Portal state (from DLL PortalList messages)
    struct PortalInfo { int x{0}, y{0}; uint32_t dest_map{0}; int dest_x{0}, dest_y{0}; std::string name; std::string type; };
    uint32_t portal_map_id_{0};
    std::vector<PortalInfo> portals_;

    // Cross-map travel
    struct MapEntry { uint32_t id{0}; std::string name; };
    std::vector<MapEntry> crossmap_maps_;
    bool crossmap_db_loaded_{false};
    int crossmap_selected_{0};

    void add_log(const std::string& line);
    void render_launch_panel();
    void render_minimap();
    void render_hero_panel();
    void render_nearby_panel();
    void render_big_map();
    void render_packet_panel();
    void render_quest_recorder();

    // Quest data recorder — raw reference data
    struct QuestNpc {
        uint32_t id{0};
        std::string name;
        uint32_t map_idd{0};  // instance map
        uint32_t doc_idd{0};  // template map
        std::string map_label;
        int x{0}, y{0};
        std::string tag;      // user note like "entrance", "teleporter"
    };
    struct QuestItem {
        uint32_t type_id{0};
        std::string name;
        std::string tag;      // user note like "PeaceToken", "SoulJade", "MoonBox"
    };
    struct QuestMonster {
        std::string name;
        uint32_t map_idd{0};
        uint32_t doc_idd{0};
        std::string map_label;
        std::string tag;      // user note
    };
    struct QuestMap {
        uint32_t map_idd{0};
        uint32_t doc_idd{0};
        std::string label;    // "Wind Plain", "Peace Tactic", etc.
    };
    struct QuestDialog {
        uint32_t npc_id{0};
        std::string npc_name;
        uint32_t action{0};
        uint32_t option_id{0};
        uint32_t data{0};
        std::string text;
        std::string tag;
    };
    struct QuestData {
        std::string quest_name{"MoonBox"};
        std::vector<QuestNpc> npcs;
        std::vector<QuestItem> items;
        std::vector<QuestMonster> monsters;
        std::vector<QuestMap> maps;
        std::vector<QuestDialog> dialogs;
    };
    QuestData quest_data_;
    char quest_tag_buf_[64]{};
    uint32_t current_map_idd_{0};
    uint32_t current_doc_idd_{0};

    // Portal exploit test UI state
    int exploit_portal_id_{0};
    int exploit_x_{0}, exploit_y_{0};

    void save_quest_data();
    void load_quest_data();

    // ════════════════════════════════════════════════════════════════
    // Per-client proxy connections
    // ════════════════════════════════════════════════════════════════
    std::vector<std::unique_ptr<ProxyConnection>> proxy_connections_;
    std::mutex proxy_connections_mutex_;

    // Proxy lifecycle
    void start_proxy_for(int account_index);
    void stop_proxy_for(int account_index);
    void stop_all_proxies();
    ProxyConnection* get_active_proxy();
    ProxyConnection* find_proxy_for(int account_index);

    // Port allocation
    static constexpr uint16_t BASE_PROXY_PORT = 1412;
    uint16_t allocate_login_port(int account_index);
    bool is_port_available(uint16_t port);

    // Proxy internals
    void proxy_accept_loop(ProxyConnection* conn, sockaddr_in login_server_addr);
    void proxy_relay(ProxyConnection* conn, SOCKET client, SOCKET server, bool is_game);
    std::vector<uint8_t> process_proxy_data(std::vector<uint8_t>& buf, bool outgoing,
                                             CipherSet& ciphers, ProxyConnection& conn);
    void handle_proxy_packet(ProxyConnection& conn, uint16_t type, const std::vector<uint8_t>& raw, bool outgoing);
    void inject_packet_to_server(const std::vector<uint8_t>& plaintext);

    // Packet display config (GUI-only, not per-connection)
    bool packet_auto_scroll_{true};
    int packet_filter_dir_{0};       // 0=all, 1=send only, 2=recv only
    uint16_t packet_filter_type_{0}; // 0=all, else specific type
    char packet_inject_hex_[512]{};
    int packet_selected_{-1};   // selected packet index for detail view

    // Minimap texture
    void load_minimap_index();
    void load_minimap_texture(uint32_t map_id);
    std::unordered_map<uint32_t, std::string> minimap_paths_; // map_id -> image path
    ID3D11ShaderResourceView* minimap_srv_{nullptr};
    uint32_t minimap_loaded_map_{0};
    int minimap_tex_w_{0}, minimap_tex_h_{0};
    int minimap_map_h_{0};    // cell height from dmap
    float minimap_real_w_{0}; // puzzle realWidth (puzzleW * gridSize)
    float minimap_real_h_{0}; // puzzle realHeight (puzzleH * gridSize)

    struct GameMapEntry { std::string filename; int grid_size{256}; };
    std::unordered_map<uint32_t, GameMapEntry> gamemap_entries_; // docId -> entry
    void load_gamemap_index();

    // Item database
    std::unordered_map<uint32_t, std::string> item_names_;
    struct ItemLifeInfo { uint32_t life{0}; };
    std::unordered_map<uint32_t, ItemLifeInfo> item_life_db_;
    void load_item_db();

    // Item icon cache
    std::unordered_map<uint32_t, std::string> item_icon_paths_;  // typeId -> icon file path
    std::unordered_map<uint32_t, ID3D11ShaderResourceView*> item_icon_cache_; // typeId -> texture
    void load_item_icon_index();
    ID3D11ShaderResourceView* get_item_icon(uint32_t type_id);
    ID3D11ShaderResourceView* load_dds_texture(const std::string& path);
    ID3D11ShaderResourceView* load_dds_from_memory(const uint8_t* data, size_t size);

    // WDF archive reader
    WdfReader data_wdf_;
    bool wdf_loaded_{false};
    void ensure_wdf_loaded();

    // Bot panel
    bool bot_running_{false};
    bool bot_auto_hp_pot_{true};
    float bot_hp_pct_{0.80f};
    bool bot_auto_mp_pot_{false};
    float bot_mp_pct_{0.50f};
    int bot_potion_threshold_{5};
    bool bot_pickup_hp_pots_{true};
    bool bot_pickup_mp_pots_{true};
    bool bot_loot_plus_{false};
    int bot_loot_min_quality_{0};
    std::vector<uint32_t> bot_pickup_types_;
    char bot_pickup_type_buf_[64]{};
    // Monster filter
    int bot_monster_filter_mode_{0}; // 0=all, 1=blacklist, 2=whitelist
    std::vector<std::string> bot_monster_filter_names_;
    char bot_monster_name_buf_[64]{};
    // Humanization
    int bot_reaction_min_{200};
    int bot_reaction_max_{600};
    int bot_attack_cd_{1000};
    int bot_pickup_cd_{500};
    int bot_action_range_{5};
    int bot_jump_scatter_{2};
    int bot_item_notice_min_{1000};
    int bot_item_notice_max_{3000};
    // Kiting
    int bot_hunt_mode_{0}; // 0=melee, 1=ranged, 2=scatter, 3=magic
    // Magic hunt mode
    int bot_magic_id_{1000};
    int bot_magic_cast_delay_{500};
    int bot_magic_safe_dist_{3};
    bool bot_kite_enabled_{false};
    int bot_kite_min_dist_{3};
    // Melee AoE cluster targeting
    bool bot_prefer_clusters_{false};
    int bot_cluster_radius_{3};
    int bot_cluster_min_stay_{2};
    // Auto-deposit
    bool bot_auto_deposit_{false};
    int bot_deposit_threshold_{39};
    // XP Skill
    bool bot_use_xp_skill_{true};
    int bot_xp_skill_id_{1110};
    int bot_xp_skill_delay_{500};
    int bot_attack_cd_xp_{100};
    // Scatter
    bool bot_use_scatter_{false};
    int bot_scatter_magic_id_{8001};
    int bot_scatter_cooldown_{1500};
    float bot_scatter_range_{12.0f};
    float bot_scatter_half_angle_{120.0f};
    int bot_scatter_min_targets_{2};
    bool bot_scatter_predict_hits_{true};
    int bot_explore_radius_{80};
    // Player safety
    bool bot_safety_enabled_{false};
    int bot_safety_flee_distance_{12};
    int bot_safety_max_encounters_{3};
    int bot_safety_encounter_window_{120};
    int bot_safety_player_action_{0};    // 0=flee, 1=safe spot, 2=disconnect
    int bot_safety_safe_x_{0}, bot_safety_safe_y_{0};
    int bot_safety_safe_map_{0};
    int bot_safety_wait_min_{60};
    int bot_safety_wait_max_{300};
    bool bot_safety_gm_detect_{true};
    int bot_safety_gm_action_{0};        // 0=disconnect, 1=safe spot
    int bot_safety_gm_wait_min_{300};
    int bot_safety_gm_wait_max_{900};
    bool bot_safety_gm_sound_{true};
    std::string bot_char_name_; // current character name for per-char config
    void render_bot_panel();
    void load_bot_config(const std::string& char_name);
    void save_bot_config(const std::string& char_name);

    void load_config();
    void save_config();
    void launch_and_inject();
};
