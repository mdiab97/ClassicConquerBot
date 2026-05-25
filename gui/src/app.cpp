#include "app.h"
#include "injector.h"
#include "imgui.h"
#include "imgui_impl_win32.h"
#include "imgui_impl_dx11.h"
#include <nlohmann/json.hpp>
#include <format>
#include <fstream>
#include <thread>
#include <filesystem>
#include "proto_lite.h"
#include <TlHelp32.h>

#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"

using namespace ccbot::net;

static constexpr uint16_t SERVER_PORT = 1411;

// ── D3D11 Setup ─────────────────────────────────────────────────────

void App::init_d3d(HWND hwnd) {
    DXGI_SWAP_CHAIN_DESC sd{};
    sd.BufferCount = 2;
    sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow = hwnd;
    sd.SampleDesc.Count = 1;
    sd.Windowed = TRUE;
    sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;

    D3D_FEATURE_LEVEL feature_level = D3D_FEATURE_LEVEL_11_0;
    D3D11CreateDeviceAndSwapChain(
        nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0,
        &feature_level, 1, D3D11_SDK_VERSION,
        &sd, &swap_chain_, &device_, nullptr, &device_context_);

    create_rtv();
}

void App::create_rtv() {
    ID3D11Texture2D* back_buffer = nullptr;
    swap_chain_->GetBuffer(0, IID_PPV_ARGS(&back_buffer));
    if (back_buffer) {
        device_->CreateRenderTargetView(back_buffer, nullptr, &rtv_);
        back_buffer->Release();
    }
}

void App::cleanup_d3d() {
    if (rtv_) { rtv_->Release(); rtv_ = nullptr; }
    if (swap_chain_) { swap_chain_->Release(); swap_chain_ = nullptr; }
    if (device_context_) { device_context_->Release(); device_context_ = nullptr; }
    if (device_) { device_->Release(); device_ = nullptr; }
}

void App::resize(UINT width, UINT height) {
    if (!device_ || width == 0 || height == 0) return;
    if (rtv_) { rtv_->Release(); rtv_ = nullptr; }
    swap_chain_->ResizeBuffers(0, width, height, DXGI_FORMAT_UNKNOWN, 0);
    create_rtv();
}

// ── Init / Shutdown ─────────────────────────────────────────────────

bool App::init(HWND hwnd) {
    init_d3d(hwnd);
    if (!device_) return false;

    // ImGui
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGui::StyleColorsDark();
    ImGui_ImplWin32_Init(hwnd);
    ImGui_ImplDX11_Init(device_, device_context_);

    // TCP Server
    server_ = std::make_unique<TcpServer>(SERVER_PORT);
    server_->on_connect = [this](auto c) { on_client_connect(c); };
    server_->on_disconnect = [this](auto c) { on_client_disconnect(c); };
    server_->on_message = [this](auto c, auto m) { on_client_message(c, m); };
    server_->start();

    add_log(std::format("TCP server listening on port {}", SERVER_PORT));

    // Resolve paths relative to this exe
    char exe_path[MAX_PATH]{};
    GetModuleFileNameA(nullptr, exe_path, MAX_PATH);
    auto exe_dir = std::filesystem::path(exe_path).parent_path();
    dll_path_ = (exe_dir / "d3d11_config.dll").string();
    config_path_ = (exe_dir / "config.json").string();

    load_config();
    load_item_db();
    add_log(std::format("DLL path: {}", dll_path_));
    if (!client_path_.empty())
        add_log(std::format("Client path: {}", client_path_));

    // Proxy connections are started per-account in on_client_connect

    return true;
}

void App::shutdown() {
    stop_all_proxies();
    if (server_) server_->stop();
    if (minimap_srv_) { minimap_srv_->Release(); minimap_srv_ = nullptr; }

    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();
    cleanup_d3d();
}

// ── Network Callbacks ───────────────────────────────────────────────

void App::on_client_connect(TcpConnection::Ptr conn) {
    std::lock_guard lock(clients_mutex_);
    ConnectedClient c;
    c.id = conn->id();
    c.connected = true;
    c.name = std::format("Client #{}", conn->id());
    clients_.push_back(c);
    add_log(std::format("[+] Client #{} connected", conn->id()));

    // Link to the first launched-but-unconnected account
    AccountProfile* linked_acc = nullptr;
    int linked_index = -1;
    for (int i = 0; i < (int)accounts_.size(); i++) {
        if (accounts_[i].launched && accounts_[i].client_id == 0) {
            accounts_[i].client_id = conn->id();
            accounts_[i].connected = true;
            linked_acc = &accounts_[i];
            linked_index = i;
            add_log(std::format("[+] Linked to account: {}", accounts_[i].username));
            break;
        }
    }

    // Start proxy for this account and tell the DLL which port to use
    if (linked_index >= 0) {
        start_proxy_for(linked_index);
        auto* pc = find_proxy_for(linked_index);
        if (pc) {
            pc->dll_client_id = conn->id();
            Message pm(MsgId::SetProxyPort);
            pm << pc->login_port;
            conn->send(pm);
            add_log(std::format("[PROXY] Account {} -> login port {}", linked_acc->username, pc->login_port));
        }
    }

    // Auto-login using the linked account's credentials
    if (linked_acc && !linked_acc->username.empty()) {
        Message m(MsgId::Login);
        m.push_string(linked_acc->username);
        m.push_string(linked_acc->password);
        m << linked_acc->server;
        conn->send(m);
        add_log(std::format("Auto-login: {}", linked_acc->username));
    } else if (login_user_[0] && login_pass_[0]) {
        // Fallback to legacy single-account login
        Message m(MsgId::Login);
        m.push_string(login_user_);
        m.push_string(login_pass_);
        m << login_server_;
        conn->send(m);
        add_log(std::format("Auto-login (legacy): {}", login_user_));
    }

    // Send data path
    {
        char exe_buf[MAX_PATH]{};
        GetModuleFileNameA(nullptr, exe_buf, MAX_PATH);
        std::string data_path = std::filesystem::path(exe_buf).parent_path().string();
        Message m(MsgId::CrossMapInit);
        m.push_string(data_path);
        conn->send(m);
    }
}

void App::on_client_disconnect(TcpConnection::Ptr conn) {
    std::lock_guard lock(clients_mutex_);
    for (auto& c : clients_) {
        if (c.id == conn->id()) {
            c.connected = false;
            break;
        }
    }
    // Unlink account and stop its proxy
    for (int i = 0; i < (int)accounts_.size(); i++) {
        if (accounts_[i].client_id == conn->id()) {
            accounts_[i].connected = false;
            accounts_[i].client_id = 0;
            accounts_[i].launched = false; // allow re-launch
            add_log(std::format("[-] Account {} disconnected", accounts_[i].username));
            stop_proxy_for(i);
            break;
        }
    }
    add_log(std::format("[-] Client #{} disconnected", conn->id()));
}

void App::on_client_message(TcpConnection::Ptr conn, Message msg) {
    std::lock_guard lock(clients_mutex_);

    switch (msg.header.id) {
    case MsgId::Handshake: {
        size_t offset = 0;
        std::string name = msg.pop_string(offset);
        for (auto& c : clients_) {
            if (c.id == conn->id()) {
                c.name = name;
                break;
            }
        }
        add_log(std::format("[{}] Handshake: {}", conn->id(), name));
        break;
    }
    case MsgId::PosUpdate: {
        int x = 0, y = 0;
        if (msg.body.size() >= 8) {
            std::memcpy(&x, msg.body.data(), 4);
            std::memcpy(&y, msg.body.data() + 4, 4);
        }
        for (auto& c : clients_) {
            if (c.id == conn->id()) {
                c.state.pos_x = x;
                c.state.pos_y = y;
                break;
            }
        }
        break;
    }
    case MsgId::StateUpdate: {
        // Re-enabled: each DLL sends its own state via IPC, works for multi-client
        size_t off = 0;
        auto rd = [&](auto& v) {
            if (off + sizeof(v) <= msg.body.size()) {
                std::memcpy(&v, msg.body.data() + off, sizeof(v));
                off += sizeof(v);
            }
        };
        HeroState s;
        rd(s.char_id); rd(s.pos_x); rd(s.pos_y); rd(s.silver);
        rd(s.stamina); rd(s.max_stamina); rd(s.pk_mode);
        rd(s.hp); rd(s.max_hp); rd(s.mp); rd(s.revive_countdown);
        rd(s.cmd_type); rd(s.cmd_status); rd(s.status_flag); rd(s.bool_flags);
        if (off + 4 <= msg.body.size()) {
            s.char_name = msg.pop_string(off);
        }
        if (off + sizeof(uint32_t) + sizeof(int)*5 <= msg.body.size()) {
            rd(s.map_id); rd(s.map_w); rd(s.map_h); rd(s.world_w); rd(s.world_h); rd(s.level);
        }
        // Update the specific client that sent this
        for (auto& c : clients_) {
            if (c.id == conn->id()) {
                // Check if proxy is active for this client — proxy has better HP/MP
                ProxyConnection* pc = nullptr;
                {
                    std::lock_guard plk(proxy_connections_mutex_);
                    for (auto& p : proxy_connections_) {
                        if (p && p->dll_client_id == conn->id() && p->hero_id != 0) {
                            pc = p.get();
                            break;
                        }
                    }
                }
                if (pc && pc->hero_id != 0 && s.char_id == pc->hero_id) {
                    s.hp = c.state.hp ? c.state.hp : s.hp;
                    s.max_hp = c.state.max_hp ? c.state.max_hp : s.max_hp;
                    s.mp = c.state.mp ? c.state.mp : s.mp;
                    s.max_mp = c.state.max_mp ? c.state.max_mp : s.max_mp;
                }
                c.state = s;
                break;
            }
        }
        if (!s.char_name.empty() && bot_char_name_ != s.char_name) {
            // Only auto-load bot config for the first client
            if (clients_.size() <= 1)
                load_bot_config(s.char_name);
        }
        // Link account
        if (!s.char_name.empty()) {
            for (auto& acc : accounts_) {
                if (acc.client_id == conn->id() && acc.char_name.empty()) {
                    acc.char_name = s.char_name;
                    break;
                }
            }
        }
        break;
    }
    case MsgId::InventoryList: {
        size_t off = 0;
        uint32_t ic = 0;
        if (msg.body.size() >= 4) { std::memcpy(&ic, msg.body.data(), 4); off = 4; }
        // Find proxy for this client and update its inventory
        ProxyConnection* pc_inv = nullptr;
        {
            std::lock_guard plk(proxy_connections_mutex_);
            for (auto& p : proxy_connections_)
                if (p && p->dll_client_id == conn->id()) { pc_inv = p.get(); break; }
        }
        std::vector<InvItemInfo> inv_items;
        for (uint32_t i = 0; i < ic && off + 32 <= msg.body.size(); i++) {
            InvItemInfo it{};
            std::memcpy(&it.item_id, msg.body.data() + off, 4); off += 4;
            std::memcpy(&it.type_id, msg.body.data() + off, 4); off += 4;
            std::memcpy(&it.amount, msg.body.data() + off, 4); off += 4;
            std::memcpy(&it.amount_limit, msg.body.data() + off, 4); off += 4;
            std::memcpy(it.name, msg.body.data() + off, 16); off += 16;
            it.name[16] = 0;
            inv_items.push_back(it);
        }
        if (pc_inv) {
            std::lock_guard slk(pc_inv->state_mutex);
            pc_inv->inventory = std::move(inv_items);
        }
        break;
    }
    case MsgId::ItemList: {
        size_t off = 0;
        uint32_t ic = 0;
        if (msg.body.size() >= 4) { std::memcpy(&ic, msg.body.data(), 4); off = 4; }
        std::vector<GroundItemInfo> items;
        for (uint32_t i = 0; i < ic && off + 16 <= msg.body.size(); i++) {
            GroundItemInfo it;
            std::memcpy(&it.item_id, msg.body.data() + off, 4); off += 4;
            std::memcpy(&it.type_id, msg.body.data() + off, 4); off += 4;
            std::memcpy(&it.x, msg.body.data() + off, 4); off += 4;
            std::memcpy(&it.y, msg.body.data() + off, 4); off += 4;
            items.push_back(it);
        }
        for (auto& c : clients_) {
            if (c.id == conn->id()) { c.ground_items = std::move(items); break; }
        }
        break;
    }
    case MsgId::NearbyList: {
        size_t off = 0;
        uint32_t count = 0;
        if (msg.body.size() >= 4) { std::memcpy(&count, msg.body.data(), 4); off = 4; }
        std::vector<NearbyEntity> list;
        for (uint32_t i = 0; i < count && off + 43 <= msg.body.size(); i++) {
            NearbyEntity e;
            std::memcpy(&e.id, msg.body.data() + off, 4); off += 4;
            std::memcpy(&e.x, msg.body.data() + off, 4); off += 4;
            std::memcpy(&e.y, msg.body.data() + off, 4); off += 4;
            std::memcpy(&e.type, msg.body.data() + off, 4); off += 4;
            std::memcpy(&e.role_kind, msg.body.data() + off, 4); off += 4;
            std::memcpy(&e.npc_sort, msg.body.data() + off, 4); off += 4;
            std::memcpy(&e.look_type, msg.body.data() + off, 2); off += 2;
            uint8_t d = 0; std::memcpy(&d, msg.body.data() + off, 1); off += 1;
            e.dead = d != 0;
            std::memcpy(e.name, msg.body.data() + off, 16); off += 16;
            e.name[16] = 0;
            list.push_back(e);
        }
        for (auto& c : clients_) {
            if (c.id == conn->id()) { c.nearby = std::move(list); break; }
        }
        break;
    }
    case MsgId::BotStatus: {
        break;
    }
    case MsgId::Log: {
        size_t off = 0;
        std::string text = msg.pop_string(off);
        if (text == "__SOUND_ALERT__") {
            MessageBeep(MB_ICONEXCLAMATION);  // system alert sound
        } else {
            add_log(text);
        }
        break;
    }
    case MsgId::CrossMapMaps: {
        size_t off = 0;
        uint32_t count = 0;
        if (msg.body.size() >= 4) { std::memcpy(&count, msg.body.data(), 4); off = 4; }
        crossmap_maps_.clear();
        for (uint32_t i = 0; i < count; i++) {
            MapEntry e;
            if (off + 4 <= msg.body.size()) { std::memcpy(&e.id, msg.body.data() + off, 4); off += 4; }
            e.name = msg.pop_string(off);
            crossmap_maps_.push_back(std::move(e));
        }
        crossmap_db_loaded_ = true;
        crossmap_selected_ = 0;
        break;
    }
    case MsgId::WarehouseStatus: {
        if (msg.body.size() >= 5) {
            warehouse_open_ = msg.body[0] != 0;
            std::memcpy(&warehouse_pkg_id_, msg.body.data() + 1, 4);
        }
        break;
    }
    case MsgId::PortalList: {
        size_t off = 0;
        uint32_t map_id = 0, count = 0;
        if (msg.body.size() >= 8) {
            std::memcpy(&map_id, msg.body.data(), 4); off = 4;
            std::memcpy(&count, msg.body.data() + off, 4); off += 4;
        }
        portal_map_id_ = map_id;
        portals_.clear();
        for (uint32_t i = 0; i < count; i++) {
            PortalInfo p;
            if (off + 20 <= msg.body.size()) {
                std::memcpy(&p.x, msg.body.data() + off, 4); off += 4;
                std::memcpy(&p.y, msg.body.data() + off, 4); off += 4;
                std::memcpy(&p.dest_map, msg.body.data() + off, 4); off += 4;
                std::memcpy(&p.dest_x, msg.body.data() + off, 4); off += 4;
                std::memcpy(&p.dest_y, msg.body.data() + off, 4); off += 4;
            }
            p.name = msg.pop_string(off);
            p.type = msg.pop_string(off);
            portals_.push_back(std::move(p));
        }
        break;
    }
    case MsgId::PortalDiscovered: {
        if (msg.body.size() >= 28) {
            size_t off = 0;
            uint32_t src_map = 0; int32_t px = 0, py = 0;
            uint32_t dest_map = 0; int32_t dx = 0, dy = 0;
            memcpy(&src_map, msg.body.data() + off, 4); off += 4;
            memcpy(&px, msg.body.data() + off, 4); off += 4;
            memcpy(&py, msg.body.data() + off, 4); off += 4;
            std::string name = msg.pop_string(off);
            memcpy(&dest_map, msg.body.data() + off, 4); off += 4;
            memcpy(&dx, msg.body.data() + off, 4); off += 4;
            memcpy(&dy, msg.body.data() + off, 4); off += 4;
            add_log(std::format("[DISCOVER] Portal '{}' ({},{}) on map {} -> map {} ({},{})",
                name, px, py, src_map, dest_map, dx, dy));
        }
        break;
    }
    case MsgId::DialogOptions: {
        size_t off = 0;
        uint8_t is_open = 0;
        uint32_t count = 0;
        if (msg.body.size() >= 5) {
            std::memcpy(&is_open, msg.body.data(), 1); off = 1;
            std::memcpy(&count, msg.body.data() + off, 4); off += 4;
        }
        dialog_open_ = (is_open != 0);
        dialog_options_.clear();
        for (uint32_t i = 0; i < count && off + 8 <= msg.body.size(); i++) {
            DialogOptionInfo opt;
            std::memcpy(&opt.type, msg.body.data() + off, 4); off += 4;
            std::memcpy(&opt.id, msg.body.data() + off, 4); off += 4;
            opt.text = msg.pop_string(off);
            dialog_options_.push_back(std::move(opt));
        }
        break;
    }
    case MsgId::NetControl:
        break;
    default:
        break;
    }
}

// ── UI Rendering ────────────────────────────────────────────────────

void App::add_log(const std::string& line) {
    log_lines_.push_back(line);
    if (log_lines_.size() > 500)
        log_lines_.erase(log_lines_.begin());
}

void App::load_minimap_index() {
    std::string path = client_path_ + "\\ani\\MiniMap.json";
    std::ifstream f(path);
    if (!f.is_open()) return;
    try {
        auto j = nlohmann::json::parse(f);
        for (auto& [k, v] : j.items()) {
            if (k == "hero") continue;
            try {
                uint32_t map_id = std::stoul(k);
                if (v.is_array() && !v.empty())
                    minimap_paths_[map_id] = v[0].get<std::string>();
            } catch (...) {}
        }
    } catch (...) {}
}

void App::load_gamemap_index() {
    std::string path = client_path_ + "\\ini\\GameMap.json";
    std::ifstream f(path);
    if (!f.is_open()) { path = client_path_ + "\\GameMap.json"; f.open(path); }
    if (!f.is_open()) return;
    try {
        auto j = nlohmann::json::parse(f);
        for (auto& entry : j) {
            uint32_t docId = entry.value("DocumentId", 0u);
            std::string fname = entry.value("FileName", "");
            int gs = entry.value("PuzzleGridSize", 256);
            if (docId && !fname.empty()) {
                for (char& c : fname) if (c == '/') c = '\\';
                gamemap_entries_[docId] = {fname, gs};
            }
        }
    } catch (...) {}
}

void App::load_minimap_texture(uint32_t map_id) {
    if (minimap_srv_) { minimap_srv_->Release(); minimap_srv_ = nullptr; }
    minimap_loaded_map_ = 0;
    minimap_tex_w_ = minimap_tex_h_ = 0;
    minimap_map_h_ = 0;
    minimap_real_w_ = 0;
    minimap_real_h_ = 0;

    // Load GameMap index if needed
    if (gamemap_entries_.empty()) load_gamemap_index();

    // Read cell dimensions and puzzle dimensions from dmap + puzzle file
    auto git = gamemap_entries_.find(map_id);
    if (git != gamemap_entries_.end()) {
        auto& gme = git->second;
        std::string dmap_path = client_path_ + "\\" + gme.filename;
        std::ifstream df(dmap_path, std::ios::binary);
        if (df.is_open()) {
            char dmap_hdr[280]{};
            df.read(dmap_hdr, 280);
            // Cell dimensions at offset 268
            uint32_t cw = 0, ch = 0;
            memcpy(&cw, dmap_hdr + 268, 4);
            memcpy(&ch, dmap_hdr + 272, 4);
            if (ch > 0 && ch < 10000) minimap_map_h_ = (int)ch;

            // Puzzle path at offset 8 (260-byte string)
            std::string puz_rel(dmap_hdr + 8);
            for (char& c : puz_rel) if (c == '/') c = '\\';
            std::string pul_path = client_path_ + "\\" + puz_rel;
            std::ifstream pf(pul_path, std::ios::binary);
            if (pf.is_open()) {
                // Puzzle header: "PUZZLE2\0" (8 bytes), ani_path (260 bytes), then puzzleW(4), puzzleH(4)
                // Total offset: 8 + 260 = 268 = 0x10C
                char pul_hdr[280]{};
                pf.read(pul_hdr, 280);
                uint32_t pw = 0, ph = 0;
                memcpy(&pw, pul_hdr + 0x108, 4); // offset 264
                memcpy(&ph, pul_hdr + 0x10C, 4); // offset 268
                if (pw > 0 && pw < 10000 && ph > 0 && ph < 10000) {
                    minimap_real_w_ = (float)(pw * gme.grid_size);
                    minimap_real_h_ = (float)(ph * gme.grid_size);
                }
            }
        }
    }

    auto it = minimap_paths_.find(map_id);
    if (it == minimap_paths_.end()) return;

    // Build full path — replace forward slashes
    std::string rel = it->second;
    for (char& c : rel) if (c == '/') c = '\\';
    std::string full = client_path_ + "\\" + rel;

    int w = 0, h = 0, ch = 0;
    unsigned char* data = stbi_load(full.c_str(), &w, &h, &ch, 4); // force RGBA
    if (!data) return;

    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = w;
    desc.Height = h;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;

    D3D11_SUBRESOURCE_DATA init{};
    init.pSysMem = data;
    init.SysMemPitch = w * 4;

    ID3D11Texture2D* tex = nullptr;
    HRESULT hr = device_->CreateTexture2D(&desc, &init, &tex);
    stbi_image_free(data);
    if (FAILED(hr) || !tex) return;

    D3D11_SHADER_RESOURCE_VIEW_DESC srv_desc{};
    srv_desc.Format = desc.Format;
    srv_desc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    srv_desc.Texture2D.MipLevels = 1;
    hr = device_->CreateShaderResourceView(tex, &srv_desc, &minimap_srv_);
    tex->Release();
    if (FAILED(hr)) { minimap_srv_ = nullptr; return; }

    minimap_loaded_map_ = map_id;
    minimap_tex_w_ = w;
    minimap_tex_h_ = h;
}

void App::render_minimap() {
    // Load minimap index on first use
    if (minimap_paths_.empty() && !client_path_.empty())
        load_minimap_index();

    ImVec2 avail = ImGui::GetContentRegionAvail();
    float map_sz = avail.x - 8;
    if (map_sz < 50) map_sz = 50;
    if (map_sz > 350) map_sz = 350;

    ImVec2 p0 = ImGui::GetCursorScreenPos();
    ImDrawList* dl = ImGui::GetWindowDrawList();

    // Get data from active proxy connection
    HeroState hs;
    std::vector<NearbyEntity> entities;
    std::vector<GroundItemInfo> ground_items;
    uint32_t active_id = 0;
    {
        auto* pc = get_active_proxy();
        if (pc) {
            std::lock_guard slk(pc->state_mutex);
            hs = pc->hero_state;
            entities = pc->entities;
            ground_items = pc->ground_items;
            active_id = pc->dll_client_id;
        }
        if (!hs.char_id) {
            // Fallback to DLL state
            std::lock_guard lock(clients_mutex_);
            auto* _ac = get_active_client_unlocked();
            if (_ac && _ac->state.char_id) {
                hs = _ac->state;
                entities = _ac->nearby;
                ground_items = _ac->ground_items;
                active_id = _ac->id;
            }
        }
    }

    if (hs.char_id && hs.map_id > 0) {
        // Load/update minimap texture if map changed
        if (minimap_loaded_map_ != hs.map_id)
            load_minimap_texture(hs.map_id);

        // Calculate aspect-correct size
        float aspect = (minimap_tex_w_ > 0 && minimap_tex_h_ > 0)
            ? (float)minimap_tex_w_ / minimap_tex_h_ : 1.0f;
        float draw_w = map_sz, draw_h = map_sz;
        if (aspect > 1.0f) draw_h = map_sz / aspect;
        else draw_w = map_sz * aspect;

        ImVec2 p1 = {p0.x + draw_w, p0.y + draw_h};

        // Draw minimap image or fallback background
        if (minimap_srv_) {
            dl->AddImage((ImTextureID)minimap_srv_, p0, p1);
        } else {
            dl->AddRectFilled(p0, p1, IM_COL32(20, 20, 30, 255));
        }
        dl->AddRect(p0, p1, IM_COL32(60, 60, 80, 255));

        // MapDocument::MapToWorld → cell to image, normalized by realW/realH from puzzle
        int mh = minimap_map_h_ > 0 ? minimap_map_h_ : 1;
        float rw = minimap_real_w_ > 0 ? minimap_real_w_ : 1;
        float rh = minimap_real_h_ > 0 ? minimap_real_h_ : 1;

        auto map_pt = [&](int cx, int cy) -> ImVec2 {
            float nx = (32.0f * (cx - cy) + rw / 2.0f) / rw;
            float ny = (16.0f * (cx + cy - (mh - 1)) + rh / 2.0f) / rh;
            return {p0.x + nx * draw_w, p0.y + ny * draw_h};
        };

        // Draw entities (with black outline for visibility)
        for (auto& e : entities) {
            ImVec2 ep = map_pt(e.x, e.y);
            if (ep.x < p0.x || ep.x > p1.x || ep.y < p0.y || ep.y > p1.y) continue;
            ImU32 col;
            float r = 2.5f;
            bool is_monster = (e.id >= 400000 && e.id < 500000);
            bool is_npc = (e.id > 0 && e.id < 400000 && e.type != 7);
            if (is_monster) {
                col = e.dead ? IM_COL32(120, 40, 40, 180) : IM_COL32(255, 50, 50, 255);
                r = 2.0f;
            } else if (is_npc) {
                col = IM_COL32(255, 230, 0, 255);
                r = 3.0f;
            } else { // Player
                col = IM_COL32(60, 160, 255, 255);
                r = 2.5f;
            }
            dl->AddCircleFilled(ep, r + 1.2f, IM_COL32(0, 0, 0, 200));
            dl->AddCircleFilled(ep, r, col);
        }

        // Draw ground items (green diamonds)
        for (auto& gi : ground_items) {
            ImVec2 gp = map_pt(gi.x, gi.y);
            if (gp.x < p0.x || gp.x > p1.x || gp.y < p0.y || gp.y > p1.y) continue;
            dl->AddCircleFilled(gp, 2.2f, IM_COL32(0, 0, 0, 180));
            dl->AddCircleFilled(gp, 1.5f, IM_COL32(50, 220, 50, 255));
        }

        // Draw hero (white, larger, with outline)
        ImVec2 hp = map_pt(hs.pos_x, hs.pos_y);
        dl->AddCircleFilled(hp, 5.5f, IM_COL32(0, 0, 0, 220));
        dl->AddCircleFilled(hp, 4.0f, IM_COL32(255, 255, 255, 255));
        dl->AddCircle(hp, 7.0f, IM_COL32(255, 255, 255, 120));

        // Map name overlay
        char map_label[64];
        snprintf(map_label, sizeof(map_label), "Map %u (%dx%d)", hs.map_id, minimap_tex_w_, minimap_tex_h_);
        dl->AddText({p0.x + 4, p0.y + 2}, IM_COL32(180, 180, 180, 200), map_label);

        // Position overlay
        char pos_label[32];
        snprintf(pos_label, sizeof(pos_label), "%d, %d", hs.pos_x, hs.pos_y);
        ImVec2 tsz = ImGui::CalcTextSize(pos_label);
        dl->AddText({p1.x - tsz.x - 4, p1.y - tsz.y - 2}, IM_COL32(200, 200, 200, 200), pos_label);

        ImGui::Dummy({draw_w, draw_h});
    } else {
        ImVec2 p1 = {p0.x + map_sz, p0.y + map_sz};
        dl->AddRectFilled(p0, p1, IM_COL32(20, 20, 30, 255));
        dl->AddRect(p0, p1, IM_COL32(60, 60, 80, 255));
        dl->AddText({p0.x + map_sz * 0.3f, p0.y + map_sz * 0.45f},
            IM_COL32(100, 100, 100, 200), "No Data");
        ImGui::Dummy({map_sz, map_sz});
        return;
    }
}

void App::render_ui() {
    ImGuiIO& io = ImGui::GetIO();
    float display_w = io.DisplaySize.x;
    float display_h = io.DisplaySize.y;
    float left_w = 310.0f;
    float log_h = 160.0f;
    float right_w = display_w - left_w;

    // ═══════════════════════════════════════════════════════════════
    // LEFT PANEL: Minimap + Status + Clients
    // ═══════════════════════════════════════════════════════════════
    ImGui::SetNextWindowPos({0, 0});
    ImGui::SetNextWindowSize({left_w, display_h});
    ImGui::Begin("##LeftPanel", nullptr,
        ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
        ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoTitleBar);

    // Minimap
    if (ImGui::CollapsingHeader("Minimap", ImGuiTreeNodeFlags_DefaultOpen)) {
        render_minimap();

        // Legend
        ImGui::TextColored({1,1,1,1}, "*"); ImGui::SameLine();
        ImGui::TextDisabled("Hero"); ImGui::SameLine();
        ImGui::TextColored({1,0.2f,0.2f,1}, "*"); ImGui::SameLine();
        ImGui::TextDisabled("Monster"); ImGui::SameLine();
        ImGui::TextColored({0.3f,0.6f,1,1}, "*"); ImGui::SameLine();
        ImGui::TextDisabled("Player"); ImGui::SameLine();
        ImGui::TextColored({1,1,0,1}, "*"); ImGui::SameLine();
        ImGui::TextDisabled("NPC");
    }

    ImGui::Separator();

    // Hero status (live)
    if (ImGui::CollapsingHeader("Status", ImGuiTreeNodeFlags_DefaultOpen)) {
        HeroState hs;
        {
            auto* pc = get_active_proxy();
            if (pc) {
                std::lock_guard slk(pc->state_mutex);
                hs = pc->hero_state;
            }
            if (!hs.char_id) {
                std::lock_guard lock(clients_mutex_);
                auto* _ac = get_active_client_unlocked();
                if (_ac && _ac->state.char_id) hs = _ac->state;
            }
        }
        if (hs.char_id) {
            ImGui::Text("%s", hs.char_name.c_str());
            ImGui::SameLine();
            ImGui::TextDisabled("Lv%d", hs.level);
            if (hs.is_dead()) { ImGui::SameLine(); ImGui::TextColored({1,0.3f,0.3f,1}, " DEAD"); }

            // HP bar (red)
            {
                float frac = hs.max_hp > 0 ? (float)hs.hp / hs.max_hp : 0;
                char overlay[32]; snprintf(overlay, sizeof(overlay), "HP %d/%d", hs.hp, hs.max_hp);
                ImGui::PushStyleColor(ImGuiCol_PlotHistogram, ImVec4(0.8f, 0.15f, 0.15f, 1));
                ImGui::ProgressBar(frac, {-1, 16}, overlay);
                ImGui::PopStyleColor();
            }
            // MP bar (blue)
            {
                float frac = hs.max_mp > 0 ? (float)hs.mp / hs.max_mp : 0;
                char overlay[32]; snprintf(overlay, sizeof(overlay), "MP %d/%d", hs.mp, hs.max_mp);
                ImGui::PushStyleColor(ImGuiCol_PlotHistogram, ImVec4(0.15f, 0.3f, 0.85f, 1));
                ImGui::ProgressBar(frac, {-1, 16}, overlay);
                ImGui::PopStyleColor();
            }
            // Stamina bar (cyan)
            {
                float frac = hs.max_stamina > 0 ? (float)hs.stamina / hs.max_stamina : 0;
                char overlay[32]; snprintf(overlay, sizeof(overlay), "STA %d/%d", hs.stamina, hs.max_stamina);
                ImGui::PushStyleColor(ImGuiCol_PlotHistogram, ImVec4(0.0f, 0.7f, 0.7f, 1));
                ImGui::ProgressBar(frac, {-1, 14}, overlay);
                ImGui::PopStyleColor();
            }

            ImGui::Text("Pos: %d, %d", hs.pos_x, hs.pos_y);
            ImGui::SameLine();
            ImGui::TextDisabled("Map %u", hs.map_id);
            ImGui::Text("Silver: %lld", hs.silver);
        } else {
            ImGui::TextDisabled("Not connected");
        }
    }

    ImGui::Separator();

    // Active account indicator
    if (active_account_ >= 0 && active_account_ < (int)accounts_.size()) {
        auto& acc = accounts_[active_account_];
        ImGui::TextColored({0.4f,0.8f,1,1}, "Account: %s", acc.username.c_str());
    }

    ImGui::Separator();

    // Launch (compact)
    render_launch_panel();

    ImGui::End();

    // ═══════════════════════════════════════════════════════════════
    // RIGHT PANEL: Tabbed content
    // ═══════════════════════════════════════════════════════════════
    ImGui::SetNextWindowPos({left_w, 0});
    ImGui::SetNextWindowSize({right_w, display_h - log_h});
    ImGui::Begin("##RightPanel", nullptr,
        ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
        ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoTitleBar);

    if (ImGui::BeginTabBar("MainTabs")) {
        if (ImGui::BeginTabItem("Bot")) {
            render_bot_panel();
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Hero")) {
            render_hero_panel();
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Entities")) {
            render_nearby_panel();
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Quest")) {
            render_quest_recorder();
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Map")) {
            render_big_map();
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Packets")) {
            render_packet_panel();
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }

    ImGui::End();

    // ═══════════════════════════════════════════════════════════════
    // BOTTOM: Log
    // ═══════════════════════════════════════════════════════════════
    ImGui::SetNextWindowPos({left_w, display_h - log_h});
    ImGui::SetNextWindowSize({right_w, log_h});
    ImGui::Begin("##LogPanel", nullptr,
        ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
        ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoTitleBar);

    ImGui::Text("Log");
    ImGui::SameLine();
    if (ImGui::SmallButton("Clear")) log_lines_.clear();
    ImGui::Separator();
    ImGui::BeginChild("log_scroll", ImVec2(0, 0), ImGuiChildFlags_None);
    for (const auto& line : log_lines_) {
        ImGui::TextUnformatted(line.c_str());
    }
    if (ImGui::GetScrollY() >= ImGui::GetScrollMaxY())
        ImGui::SetScrollHereY(1.0f);
    ImGui::EndChild();

    ImGui::End();
}

// ── Packet type name helper ──────────────────────────────────────────
static const char* pkt_type_name(uint16_t type) {
    switch (type) {
        case 1004: return "Talk";      case 1005: return "Walk";
        case 1006: return "UserInfo";  case 1008: return "ItemInfo";
        case 1009: return "Item";      case 1010: return "Action";
        case 1014: return "Player";    case 1017: return "UserAttrib";
        case 1022: return "Interact";  case 1023: return "Team";
        case 1025: return "WpnSkill";  case 1051: return "Auth";
        case 1052: return "Connect";   case 1055: return "ConnectEx";
        case 1056: return "Trade";     case 1101: return "MapItem";
        case 1102: return "Package";   case 1103: return "MagicInfo";
        case 1105: return "MagicFX";   case 1110: return "MapInfo";
        case 2030: return "NpcInfo";   case 2031: return "Npc";
        case 2032: return "TaskDlg";   case 7001: return "Ping";
        case 7002: return "DateTime";  case 7004: return "Vip";
        case 7006: return "StreamReq"; case 7010: return "EventShop";
        case 7015: return "CompBank";
        default: return nullptr;
    }
}

void App::render_packet_panel() {
    auto* pc = get_active_proxy();
    if (!pc) {
        ImGui::TextDisabled("No proxy active");
        return;
    }

    if (pc->running.load())
        ImGui::TextColored({0.4f,1,0.4f,1}, "PROXY active (port %u/%u)", pc->login_port, pc->game_port);
    else
        ImGui::TextDisabled("Proxy: idle");
    ImGui::SameLine();

    // Controls
    ImGui::Checkbox("Capture", &pc->capture_on);
    ImGui::SameLine();
    if (ImGui::SmallButton("Clear")) {
        std::lock_guard lk(pc->packet_mutex);
        pc->packets.clear();
        packet_selected_ = -1;
    }
    ImGui::SameLine();
    ImGui::Checkbox("Auto-scroll", &packet_auto_scroll_);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(80);
    const char* dir_labels[] = {"All", "Send", "Recv"};
    ImGui::Combo("Dir##pktdir", &packet_filter_dir_, dir_labels, 3);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(80);
    int ft = (int)packet_filter_type_;
    if (ImGui::InputInt("Type##pkttype", &ft, 0, 0)) packet_filter_type_ = (uint16_t)ft;

    {
        std::lock_guard lk(pc->packet_mutex);
        ImGui::SameLine();
        ImGui::Text("(%zu pkts)", pc->packets.size());
    }

    ImGui::Separator();

    // Packet table (upper half) + detail pane (lower half)
    float total_h = ImGui::GetContentRegionAvail().y - 30; // reserve inject bar
    float table_h = total_h * 0.55f;
    float detail_h = total_h * 0.45f;
    if (table_h < 80) table_h = 80;
    if (detail_h < 60) detail_h = 60;

    ImGui::BeginChild("pkt_list", {0, table_h}, ImGuiChildFlags_Border);
    std::lock_guard lk(pc->packet_mutex);

    if (ImGui::BeginTable("##pkttable", 5,
            ImGuiTableFlags_ScrollY | ImGuiTableFlags_RowBg |
            ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_Resizable |
            ImGuiTableFlags_SizingFixedFit)) {
        ImGui::TableSetupColumn("Dir", ImGuiTableColumnFlags_WidthFixed, 35);
        ImGui::TableSetupColumn("Type", ImGuiTableColumnFlags_WidthFixed, 55);
        ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthFixed, 80);
        ImGui::TableSetupColumn("Size", ImGuiTableColumnFlags_WidthFixed, 45);
        ImGui::TableSetupColumn("Hex", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableHeadersRow();

        for (size_t i = 0; i < pc->packets.size(); i++) {
            auto& p = pc->packets[i];
            // Filter
            if (packet_filter_dir_ == 1 && !p.outgoing) continue;
            if (packet_filter_dir_ == 2 && p.outgoing) continue;
            if (packet_filter_type_ != 0 && p.type != packet_filter_type_) continue;

            ImGui::PushID((int)i);
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            bool selected = (packet_selected_ == (int)i);
            if (ImGui::Selectable(p.outgoing ? "SND" : "RCV", selected,
                    ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowOverlap)) {
                packet_selected_ = (int)i;
            }
            if (p.outgoing) ImGui::TableSetBgColor(ImGuiTableBgTarget_CellBg, IM_COL32(20,50,20,100));
            else ImGui::TableSetBgColor(ImGuiTableBgTarget_CellBg, IM_COL32(20,20,50,100));

            ImGui::TableNextColumn();
            ImGui::Text("%u", p.type);

            ImGui::TableNextColumn();
            const char* name = pkt_type_name(p.type);
            if (name) ImGui::Text("%s", name);
            else ImGui::TextDisabled("?");

            ImGui::TableNextColumn();
            ImGui::Text("%zu", p.raw.size());

            ImGui::TableNextColumn();
            // Show first ~40 bytes as hex
            char hex[256] = {};
            size_t show = p.raw.size() < 40 ? p.raw.size() : 40;
            for (size_t j = 0; j < show; j++)
                snprintf(hex + j * 3, sizeof(hex) - j * 3, "%02X ", p.raw[j]);
            if (p.raw.size() > 40) strncat(hex, "...", sizeof(hex) - strlen(hex) - 1);
            ImGui::TextUnformatted(hex);
            ImGui::PopID();
        }

        if (packet_auto_scroll_ && ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 20)
            ImGui::SetScrollHereY(1.0f);

        ImGui::EndTable();
    }
    ImGui::EndChild();

    // Detail pane — show decoded protobuf fields of selected packet
    ImGui::BeginChild("pkt_detail", {0, detail_h}, ImGuiChildFlags_Border);
    if (packet_selected_ >= 0 && packet_selected_ < (int)pc->packets.size()) {
        auto& sel = pc->packets[packet_selected_];
        auto* schema = proto::find_schema(sel.type);
        const char* tname = pkt_type_name(sel.type);

        ImGui::Text("%s %s (type=%u, size=%zu)",
            sel.outgoing ? ">>>" : "<<<",
            tname ? tname : "Unknown", sel.type, sel.raw.size());
        ImGui::Separator();

        if (sel.raw.size() > 4) {
            auto fields = proto::decode(sel.raw);
            if (fields.empty()) {
                ImGui::TextDisabled("(no protobuf fields decoded)");
            } else {
                if (ImGui::BeginTable("##fields", 4, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV)) {
                    ImGui::TableSetupColumn("Field#", ImGuiTableColumnFlags_WidthFixed, 45);
                    ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthFixed, 120);
                    ImGui::TableSetupColumn("Wire", ImGuiTableColumnFlags_WidthFixed, 50);
                    ImGui::TableSetupColumn("Value", ImGuiTableColumnFlags_WidthStretch);
                    ImGui::TableHeadersRow();

                    int row_idx = 0;
                    for (auto& f : fields) {
                        ImGui::PushID(row_idx++);
                        ImGui::TableNextRow();
                        ImGui::TableNextColumn();
                        ImGui::Text("%u", f.field_num);

                        ImGui::TableNextColumn();
                        const proto::FieldSchema* fs = nullptr;
                        if (schema) {
                            for (int si = 0; si < schema->field_count; si++) {
                                if (schema->fields[si].num == f.field_num) {
                                    fs = &schema->fields[si];
                                    break;
                                }
                            }
                        }
                        if (fs) ImGui::Text("%s", fs->name);
                        else ImGui::TextDisabled("field_%u", f.field_num);

                        ImGui::TableNextColumn();
                        const char* wt_names[] = {"varint","fix64","bytes","grp","grpend","fix32"};
                        ImGui::TextDisabled("%s", f.wire_type < 6 ? wt_names[f.wire_type] : "unk");

                        ImGui::TableNextColumn();
                        auto val_str = proto::format_field(f, fs);
                        ImGui::TextWrapped("%s", val_str.c_str());
                        ImGui::PopID();
                    }
                    ImGui::EndTable();
                }
            }
        } else {
            ImGui::TextDisabled("(packet too small for protobuf)");
        }
    } else {
        ImGui::TextDisabled("Select a packet to view decoded fields");
    }
    ImGui::EndChild();

    // Inject bar
    ImGui::SetNextItemWidth(-80);
    ImGui::InputText("##inject_hex", packet_inject_hex_, sizeof(packet_inject_hex_));
    ImGui::SameLine();
    if (ImGui::SmallButton("Inject")) {
        // Parse hex string to bytes and send
        std::vector<uint8_t> bytes;
        const char* p = packet_inject_hex_;
        while (*p) {
            while (*p == ' ') p++;
            if (!*p) break;
            unsigned int b = 0;
            if (sscanf(p, "%02X", &b) == 1) {
                bytes.push_back((uint8_t)b);
                p += 2;
            } else {
                p++;
            }
        }
        if (!bytes.empty()) {
            inject_packet_to_server(bytes);
        }
    }
}

// ── Quest Data Recorder ──────────────────────────────────────────────

void App::save_quest_data() {
    nlohmann::json j;
    j["quest_name"] = quest_data_.quest_name;

    j["npcs"] = nlohmann::json::array();
    for (auto& n : quest_data_.npcs)
        j["npcs"].push_back({{"id",n.id},{"name",n.name},{"map_idd",n.map_idd},{"doc_idd",n.doc_idd},
            {"map_label",n.map_label},{"x",n.x},{"y",n.y},{"tag",n.tag}});

    j["items"] = nlohmann::json::array();
    for (auto& it : quest_data_.items)
        j["items"].push_back({{"type_id",it.type_id},{"name",it.name},{"tag",it.tag}});

    j["monsters"] = nlohmann::json::array();
    for (auto& m : quest_data_.monsters)
        j["monsters"].push_back({{"name",m.name},{"map_idd",m.map_idd},{"doc_idd",m.doc_idd},
            {"map_label",m.map_label},{"tag",m.tag}});

    j["maps"] = nlohmann::json::array();
    for (auto& m : quest_data_.maps)
        j["maps"].push_back({{"map_idd",m.map_idd},{"doc_idd",m.doc_idd},{"label",m.label}});

    j["dialogs"] = nlohmann::json::array();
    for (auto& d : quest_data_.dialogs)
        j["dialogs"].push_back({{"npc_id",d.npc_id},{"npc_name",d.npc_name},{"action",d.action},
            {"option_id",d.option_id},{"data",d.data},{"text",d.text},{"tag",d.tag}});

    auto path = std::filesystem::path(config_path_).parent_path() / "quest_data.json";
    std::ofstream f(path);
    f << j.dump(2);
    add_log(std::format("Saved: {} npcs, {} items, {} monsters, {} maps, {} dialogs",
        quest_data_.npcs.size(), quest_data_.items.size(), quest_data_.monsters.size(),
        quest_data_.maps.size(), quest_data_.dialogs.size()));
}

void App::load_quest_data() {
    auto path = std::filesystem::path(config_path_).parent_path() / "quest_data.json";
    std::ifstream f(path);
    if (!f.is_open()) return;
    try {
        auto j = nlohmann::json::parse(f);
        quest_data_.quest_name = j.value("quest_name", "MoonBox");
        quest_data_.npcs.clear();
        for (auto& e : j.value("npcs", nlohmann::json::array()))
            quest_data_.npcs.push_back({e.value("id",0u),e.value("name",""),e.value("map_idd",0u),
                e.value("doc_idd",0u),e.value("map_label",""),e.value("x",0),e.value("y",0),e.value("tag","")});
        quest_data_.items.clear();
        for (auto& e : j.value("items", nlohmann::json::array()))
            quest_data_.items.push_back({e.value("type_id",0u),e.value("name",""),e.value("tag","")});
        quest_data_.monsters.clear();
        for (auto& e : j.value("monsters", nlohmann::json::array()))
            quest_data_.monsters.push_back({e.value("name",""),e.value("map_idd",0u),e.value("doc_idd",0u),
                e.value("map_label",""),e.value("tag","")});
        quest_data_.maps.clear();
        for (auto& e : j.value("maps", nlohmann::json::array()))
            quest_data_.maps.push_back({e.value("map_idd",0u),e.value("doc_idd",0u),e.value("label","")});
        quest_data_.dialogs.clear();
        for (auto& e : j.value("dialogs", nlohmann::json::array()))
            quest_data_.dialogs.push_back({e.value("npc_id",0u),e.value("npc_name",""),e.value("action",0u),
                e.value("option_id",0u),e.value("data",0u),e.value("text",""),e.value("tag","")});
        add_log(std::format("Loaded quest data: {} npcs, {} items", quest_data_.npcs.size(), quest_data_.items.size()));
    } catch (...) {}
}

void App::render_quest_recorder() {
    HeroState hs;
    std::vector<NearbyEntity> entities;
    uint32_t active_id = 0;
    {
        auto* pc = get_active_proxy();
        if (pc) {
            std::lock_guard slk(pc->state_mutex);
            hs = pc->hero_state;
            entities = pc->entities;
            active_id = pc->dll_client_id;
        }
        if (!hs.char_id) {
            std::lock_guard lock(clients_mutex_);
            auto* _ac = get_active_client_unlocked();
            if (_ac && _ac->state.char_id) {
                hs = _ac->state; entities = _ac->nearby; active_id = _ac->id;
            }
        }
    }

    // Header + current location
    if (ImGui::SmallButton("Save##qd")) save_quest_data();
    ImGui::SameLine();
    if (ImGui::SmallButton("Load##qd")) load_quest_data();
    ImGui::SameLine();
    if (ImGui::SmallButton("Clear All##qd")) quest_data_ = {};
    ImGui::SameLine();
    ImGui::TextDisabled("map=%u doc=%u pos=(%d,%d)", current_map_idd_, current_doc_idd_, hs.pos_x, hs.pos_y);

    // Tag input (reused across sections)
    ImGui::SetNextItemWidth(200);
    ImGui::InputText("Tag##qtag", quest_tag_buf_, sizeof(quest_tag_buf_));
    ImGui::SameLine();
    ImGui::TextDisabled("(label for next recorded entry)");

    ImGui::Separator();

    // ── Record Map ───────────────────────────────────────────────
    if (ImGui::CollapsingHeader("Maps", ImGuiTreeNodeFlags_DefaultOpen)) {
        if (ImGui::SmallButton("Record Current Map##recmap")) {
            bool exists = false;
            for (auto& m : quest_data_.maps)
                if (m.map_idd == current_map_idd_) { exists = true; break; }
            if (!exists) {
                quest_data_.maps.push_back({current_map_idd_, current_doc_idd_, quest_tag_buf_});
                add_log(std::format("[Q] Map: idd={} doc={} '{}'", current_map_idd_, current_doc_idd_, quest_tag_buf_));
            }
        }
        for (int i = 0; i < (int)quest_data_.maps.size(); i++) {
            auto& m = quest_data_.maps[i];
            ImGui::PushID(i + 900000);
            ImGui::BulletText("idd=%u doc=%u  \"%s\"", m.map_idd, m.doc_idd, m.label.c_str());
            ImGui::SameLine();
            if (ImGui::SmallButton("X##dm")) { quest_data_.maps.erase(quest_data_.maps.begin() + i); ImGui::PopID(); break; }
            ImGui::PopID();
        }
    }

    // ── Record NPCs ──────────────────────────────────────────────
    if (ImGui::CollapsingHeader("NPCs", ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::Text("ALL nearby (click to record):");
        for (auto& e : entities) {
            if (e.id == hs.char_id) continue;
            ImGui::PushID((int)e.id + 500000);
            const char* kind = "?";
            if (e.id >= 400000 && e.id < 500000) kind = "Mon";
            else if (e.type == 7) kind = "Plr";
            else kind = "NPC";
            if (ImGui::SmallButton(std::format("{} [{}] ({},{}) {} rk={}", e.name, e.id, e.x, e.y, kind, e.role_kind).c_str())) {
                bool exists = false;
                for (auto& n : quest_data_.npcs) if (n.id == e.id) { exists = true; break; }
                if (!exists) {
                    quest_data_.npcs.push_back({e.id, e.name, current_map_idd_, current_doc_idd_,
                        quest_tag_buf_, e.x, e.y, quest_tag_buf_});
                    add_log(std::format("[Q] NPC: {} id={} ({},{}) tag='{}'", e.name, e.id, e.x, e.y, quest_tag_buf_));
                }
            }
            ImGui::PopID();
        }
        ImGui::Separator();
        for (int i = 0; i < (int)quest_data_.npcs.size(); i++) {
            auto& n = quest_data_.npcs[i];
            ImGui::PushID(i + 910000);
            ImGui::BulletText("%s id=%u map=%u (%d,%d) [%s]", n.name.c_str(), n.id, n.map_idd, n.x, n.y, n.tag.c_str());
            ImGui::SameLine();
            if (ImGui::SmallButton("X##dn")) { quest_data_.npcs.erase(quest_data_.npcs.begin() + i); ImGui::PopID(); break; }
            ImGui::PopID();
        }
    }

    // ── Record Monsters ──────────────────────────────────────────
    if (ImGui::CollapsingHeader("Monsters")) {
        ImGui::Text("Nearby (click to record):");
        for (auto& e : entities) {
            if (e.id < 400000 || e.id >= 500000) continue;
            ImGui::PushID((int)e.id + 520000);
            if (ImGui::SmallButton(std::format("{} [{}] ({},{})", e.name, e.id, e.x, e.y).c_str())) {
                bool exists = false;
                for (auto& m : quest_data_.monsters) if (m.name == e.name) { exists = true; break; }
                if (!exists) {
                    quest_data_.monsters.push_back({e.name, current_map_idd_, current_doc_idd_,
                        quest_tag_buf_, quest_tag_buf_});
                    add_log(std::format("[Q] Monster: {} tag='{}'", e.name, quest_tag_buf_));
                }
            }
            ImGui::PopID();
        }
        ImGui::Separator();
        for (int i = 0; i < (int)quest_data_.monsters.size(); i++) {
            auto& m = quest_data_.monsters[i];
            ImGui::PushID(i + 920000);
            ImGui::BulletText("%s map=%u [%s]", m.name.c_str(), m.map_idd, m.tag.c_str());
            ImGui::SameLine();
            if (ImGui::SmallButton("X##dmo")) { quest_data_.monsters.erase(quest_data_.monsters.begin() + i); ImGui::PopID(); break; }
            ImGui::PopID();
        }
    }

    // ── Record Items ─────────────────────────────────────────────
    if (ImGui::CollapsingHeader("Items")) {
        std::vector<InvItemInfo> quest_inv;
        { auto* pc_qi = get_active_proxy(); if (pc_qi) { std::lock_guard slk(pc_qi->state_mutex); quest_inv = pc_qi->inventory; } }
        ImGui::Text("Inventory (click to record):");
        for (auto& it : quest_inv) {
            ImGui::PushID((int)it.item_id + 600000);
            auto nit = item_names_.find(it.type_id);
            const char* name = nit != item_names_.end() ? nit->second.c_str() : "???";
            if (ImGui::SmallButton(std::format("{} [{}] x{}", name, it.type_id, it.amount).c_str())) {
                bool exists = false;
                for (auto& qi : quest_data_.items) if (qi.type_id == it.type_id) { exists = true; break; }
                if (!exists) {
                    quest_data_.items.push_back({it.type_id, name, quest_tag_buf_});
                    add_log(std::format("[Q] Item: {} type={} tag='{}'", name, it.type_id, quest_tag_buf_));
                }
            }
            ImGui::PopID();
        }
        ImGui::Separator();
        for (int i = 0; i < (int)quest_data_.items.size(); i++) {
            auto& it = quest_data_.items[i];
            ImGui::PushID(i + 930000);
            ImGui::BulletText("%s type=%u [%s]", it.name.c_str(), it.type_id, it.tag.c_str());
            ImGui::SameLine();
            if (ImGui::SmallButton("X##di")) { quest_data_.items.erase(quest_data_.items.begin() + i); ImGui::PopID(); break; }
            ImGui::PopID();
        }
    }

    // ── Record Dialogs ───────────────────────────────────────────
    if (ImGui::CollapsingHeader("Dialogs")) {
        ImGui::Text("Recent MsgTaskDialog packets:");
        {
            auto* pc_dlg = get_active_proxy();
            std::vector<CapturedPacket> dialog_pkts;
            if (pc_dlg) {
                std::lock_guard lk(pc_dlg->packet_mutex);
                // Grab last 15 MsgTaskDialog packets
                for (int i = (int)pc_dlg->packets.size() - 1; i >= 0 && (int)dialog_pkts.size() < 15; i--)
                    if (pc_dlg->packets[i].type == 2032) dialog_pkts.push_back(pc_dlg->packets[i]);
            }
            int shown = 0;
            for (int i = 0; i < (int)dialog_pkts.size(); i++) {
                auto& p = dialog_pkts[i];
                auto fields = proto::decode(p.raw);
                uint32_t act = (uint32_t)proto::get_uint(fields, 6);
                uint32_t opt = (uint32_t)proto::get_uint(fields, 5);
                uint32_t dat = (uint32_t)proto::get_uint(fields, 4);
                std::string text = proto::get_string(fields, 7);

                ImGui::PushID(i + 700000);
                ImGui::Text("%s act=%u opt=%u dat=%u: %.60s",
                    p.outgoing ? ">>" : "<<", act, opt, dat, text.c_str());
                ImGui::SameLine();
                if (ImGui::SmallButton("Rec##dlg")) {
                    quest_data_.dialogs.push_back({0, "", act, opt, dat, text, quest_tag_buf_});
                    add_log(std::format("[Q] Dialog: act={} opt={} tag='{}'", act, opt, quest_tag_buf_));
                }
                ImGui::PopID();
                shown++;
            }
            if (shown == 0) ImGui::TextDisabled("(no dialogs yet)");
        }
        ImGui::Separator();
        for (int i = 0; i < (int)quest_data_.dialogs.size(); i++) {
            auto& d = quest_data_.dialogs[i];
            ImGui::PushID(i + 940000);
            ImGui::BulletText("act=%u opt=%u dat=%u [%s]: %.50s", d.action, d.option_id, d.data, d.tag.c_str(), d.text.c_str());
            ImGui::SameLine();
            if (ImGui::SmallButton("X##dd")) { quest_data_.dialogs.erase(quest_data_.dialogs.begin() + i); ImGui::PopID(); break; }
            ImGui::PopID();
        }
    }

    // ── Portal Exploit Test ──────────────────────────────────────
    ImGui::Separator();
    if (ImGui::CollapsingHeader("Portal Test (ChgMap)")) {
        // Get portals from active proxy
        std::vector<ProxyPortal> proxy_portals;
        uint32_t proxy_hero_id = 0;
        { auto* pc_pt = get_active_proxy(); if (pc_pt) { std::lock_guard slk(pc_pt->state_mutex); proxy_portals = pc_pt->portals; proxy_hero_id = pc_pt->hero_id; } }
        // Show detected portals
        if (!proxy_portals.empty()) {
            ImGui::Text("Nearby portals (click to select):");
            for (int i = 0; i < (int)proxy_portals.size(); i++) {
                auto& p = proxy_portals[i];
                ImGui::PushID(i + 950000);
                if (ImGui::SmallButton(std::format("Portal {} ({},{})", p.id, p.x, p.y).c_str())) {
                    exploit_portal_id_ = (int)p.id;
                    exploit_x_ = p.x;
                    exploit_y_ = p.y;
                }
                ImGui::PopID();
                if (i % 3 != 2 && i + 1 < (int)proxy_portals.size()) ImGui::SameLine();
            }
        } else {
            ImGui::TextDisabled("No portals detected (walk near portals)");
        }

        ImGui::SetNextItemWidth(100);
        ImGui::InputInt("Portal ID##pxp", &exploit_portal_id_);
        ImGui::SameLine();
        ImGui::SetNextItemWidth(80);
        ImGui::InputInt("X##pxx", &exploit_x_);
        ImGui::SameLine();
        ImGui::SetNextItemWidth(80);
        ImGui::InputInt("Y##pxy", &exploit_y_);
        ImGui::SameLine();
        if (ImGui::Button("Send ChgMap##pxsend")) {
            // Build MsgAction with ChgMap (action=7)
            proto::Fields fields;
            fields.push_back({1, proto::VARINT, 7, {}});                              // action = ChgMap
            fields.push_back({2, proto::VARINT, (uint64_t)proxy_hero_id, {}});       // playerid
            fields.push_back({3, proto::VARINT, (uint64_t)GetTickCount(), {}});       // timestamp
            fields.push_back({4, proto::VARINT, (uint64_t)exploit_x_, {}});           // x
            fields.push_back({5, proto::VARINT, (uint64_t)exploit_y_, {}});           // y
            fields.push_back({6, proto::VARINT, 0, {}});                              // dir
            fields.push_back({8, proto::VARINT, (uint64_t)exploit_portal_id_, {}});   // pkmode = portal ID

            auto pkt = proto::rebuild_packet(1010, fields);
            inject_packet_to_server(pkt);
            add_log(std::format("[TEST] ChgMap portal={} ({},{}) {} bytes",
                exploit_portal_id_, exploit_x_, exploit_y_, pkt.size()));
        }
        ImGui::SameLine();
        if (ImGui::Button("Use Hero Pos##pxhero")) {
            exploit_x_ = hs.pos_x;
            exploit_y_ = hs.pos_y;
        }
        ImGui::TextDisabled("Send ChgMap action with custom portal ID to test server-side validation");
    }
}

void App::inject_packet_to_server(const std::vector<uint8_t>& plaintext) {
    auto* pc = get_active_proxy();
    if (!pc) return;
    std::lock_guard lk(pc->inject_mutex);
    pc->inject_to_server.push_back(plaintext);
}

// ── Big Map (interactive, click-to-pathfind) ─────────────────────────

void App::render_big_map() {
    // Get client data from proxy
    HeroState hs;
    std::vector<NearbyEntity> entities;
    std::vector<GroundItemInfo> gitems;
    uint32_t active_id = 0;
    {
        auto* pc = get_active_proxy();
        if (pc) {
            std::lock_guard slk(pc->state_mutex);
            hs = pc->hero_state;
            entities = pc->entities;
            gitems = pc->ground_items;
            active_id = pc->dll_client_id;
        }
        if (!hs.char_id) {
            std::lock_guard lock(clients_mutex_);
            auto* _ac = get_active_client_unlocked();
            if (_ac && _ac->state.char_id) {
                hs = _ac->state;
                entities = _ac->nearby;
                gitems = _ac->ground_items;
                active_id = _ac->id;
            }
        }
    }

    if (!hs.char_id || !hs.map_id) {
        ImGui::TextDisabled("No map data");
        return;
    }

    // Ensure minimap loaded
    if (minimap_loaded_map_ != hs.map_id)
        load_minimap_texture(hs.map_id);

    int mh = minimap_map_h_ > 0 ? minimap_map_h_ : 1;
    float rw = minimap_real_w_ > 0 ? minimap_real_w_ : 1;
    float rh = minimap_real_h_ > 0 ? minimap_real_h_ : 1;

    // Fill available space, maintain aspect ratio
    ImVec2 avail = ImGui::GetContentRegionAvail();
    float aspect = (minimap_tex_w_ > 0 && minimap_tex_h_ > 0)
        ? (float)minimap_tex_w_ / minimap_tex_h_ : 1.0f;
    float draw_w = avail.x, draw_h = avail.x / aspect;
    if (draw_h > avail.y - 20) { draw_h = avail.y - 20; draw_w = draw_h * aspect; }

    ImVec2 p0 = ImGui::GetCursorScreenPos();
    ImVec2 p1 = {p0.x + draw_w, p0.y + draw_h};
    ImDrawList* dl = ImGui::GetWindowDrawList();

    // Draw map image
    if (minimap_srv_)
        dl->AddImage((ImTextureID)minimap_srv_, p0, p1);
    else
        dl->AddRectFilled(p0, p1, IM_COL32(20, 20, 30, 255));
    dl->AddRect(p0, p1, IM_COL32(80, 80, 100, 255));

    // Coordinate conversion (same as minimap)
    auto map_pt = [&](int cx, int cy) -> ImVec2 {
        float nx = (32.0f * (cx - cy) + rw / 2.0f) / rw;
        float ny = (16.0f * (cx + cy - (mh - 1)) + rh / 2.0f) / rh;
        return {p0.x + nx * draw_w, p0.y + ny * draw_h};
    };

    // Inverse: screen position → cell coordinates
    auto screen_to_cell = [&](ImVec2 screen, int& cx, int& cy) {
        float nx = (screen.x - p0.x) / draw_w;
        float ny = (screen.y - p0.y) / draw_h;
        // nx = (32*(cx-cy) + rw/2) / rw = 32*(cx-cy)/rw + 0.5
        // ny = (16*(cx+cy-(mh-1)) + rh/2) / rh = 16*(cx+cy-(mh-1))/rh + 0.5
        float diff = (nx - 0.5f) * rw / 32.0f;  // cx - cy
        float sum = (ny - 0.5f) * rh / 16.0f + (mh - 1);  // cx + cy
        cx = (int)((diff + sum) / 2.0f + 0.5f);
        cy = (int)((sum - diff) / 2.0f + 0.5f);
    };

    // Draw ground items
    for (auto& gi : gitems) {
        ImVec2 gp = map_pt(gi.x, gi.y);
        if (gp.x < p0.x || gp.x > p1.x || gp.y < p0.y || gp.y > p1.y) continue;
        dl->AddCircleFilled(gp, 3.0f, IM_COL32(0, 0, 0, 180));
        dl->AddCircleFilled(gp, 2.0f, IM_COL32(50, 220, 50, 255));
    }

    // Draw entities
    for (auto& e : entities) {
        ImVec2 ep = map_pt(e.x, e.y);
        if (ep.x < p0.x || ep.x > p1.x || ep.y < p0.y || ep.y > p1.y) continue;
        ImU32 col;
        float r = 3.5f;
        bool is_monster = (e.id >= 400000 && e.id < 500000);
        bool is_npc = (e.id > 0 && e.id < 400000 && e.type != 7);
        if (is_monster) {
            col = e.dead ? IM_COL32(120, 40, 40, 180) : IM_COL32(255, 50, 50, 255);
            r = 3.0f;
        } else if (is_npc) {
            col = IM_COL32(255, 230, 0, 255);
            r = 4.0f;
        } else {
            col = IM_COL32(60, 160, 255, 255);
            r = 3.5f;
        }
        dl->AddCircleFilled(ep, r + 1.5f, IM_COL32(0, 0, 0, 200));
        dl->AddCircleFilled(ep, r, col);

        // Show name on hover
        if (ImGui::IsMouseHoveringRect({ep.x - r, ep.y - r}, {ep.x + r, ep.y + r})) {
            ImGui::SetTooltip("%s (%d,%d) ID:%u", e.name, e.x, e.y, e.id);
        }
    }

    // Draw hero
    ImVec2 hp = map_pt(hs.pos_x, hs.pos_y);
    dl->AddCircleFilled(hp, 7.0f, IM_COL32(0, 0, 0, 220));
    dl->AddCircleFilled(hp, 5.0f, IM_COL32(255, 255, 255, 255));
    dl->AddCircle(hp, 9.0f, IM_COL32(255, 255, 255, 120));

    // Invisible button for click detection
    ImGui::SetCursorScreenPos(p0);
    ImGui::InvisibleButton("##bigmap", {draw_w, draw_h});

    // Click to pathfind
    if (ImGui::IsItemClicked(ImGuiMouseButton_Left) && active_id) {
        ImVec2 mouse = ImGui::GetMousePos();
        int cx, cy;
        screen_to_cell(mouse, cx, cy);
        if (cx >= 0 && cy >= 0 && cx < mh * 2 && cy < mh * 2) {
            Message m(MsgId::PathfindTo);
            m << cx << cy;
            server_->send_to(active_id, m);
            add_log(std::format("[MAP] Pathfind to ({}, {})", cx, cy));
        }
    }

    // Show coordinates on hover
    if (ImGui::IsItemHovered()) {
        ImVec2 mouse = ImGui::GetMousePos();
        int cx, cy;
        screen_to_cell(mouse, cx, cy);
        // Draw crosshair at cursor
        dl->AddLine({mouse.x - 6, mouse.y}, {mouse.x + 6, mouse.y}, IM_COL32(255, 255, 255, 150));
        dl->AddLine({mouse.x, mouse.y - 6}, {mouse.x, mouse.y + 6}, IM_COL32(255, 255, 255, 150));
        // Coordinate tooltip
        ImGui::SetTooltip("(%d, %d)", cx, cy);
    }

    // Info bar
    ImGui::Text("Map %u | Hero: (%d, %d) | %zu entities | Click to pathfind",
        hs.map_id, hs.pos_x, hs.pos_y, entities.size());
}

// ── Proxy packet → per-connection state ──────────────────────────────

void App::handle_proxy_packet(ProxyConnection& conn, uint16_t type, const std::vector<uint8_t>& raw, bool outgoing) {
    auto fields = proto::decode(raw);
    if (fields.empty()) return;

    auto& hs = conn.hero_state;

    switch (type) {
    case 1006: { // MsgUserInfo — initial hero data (from server)
        if (outgoing) break;
        hs.char_id = (uint32_t)proto::get_int(fields, 1);
        conn.hero_id = hs.char_id;
        hs.silver = proto::get_int(fields, 4);
        hs.hp = (int)proto::get_int(fields, 16);
        hs.max_hp = (int)proto::get_int(fields, 17);
        hs.mp = (int)proto::get_int(fields, 18);
        hs.max_mp = (int)proto::get_int(fields, 19);
        hs.stamina = (int)proto::get_int(fields, 20);
        hs.level = (int)proto::get_int(fields, 21);
        hs.pos_x = (int)proto::get_int(fields, 28);
        hs.pos_y = (int)proto::get_int(fields, 29);
        hs.char_name = proto::get_string(fields, 30);

        // Update the connected client's state from proxy data
        {
            std::lock_guard lock(clients_mutex_);
            for (auto& c : clients_) {
                if (c.id == conn.dll_client_id) {
                    c.state.char_id = hs.char_id;
                    c.state.char_name = hs.char_name;
                    c.state.silver = hs.silver;
                    c.state.hp = hs.hp;
                    c.state.max_hp = hs.max_hp;
                    c.state.mp = hs.mp;
                    c.state.max_mp = hs.max_mp;
                    c.state.stamina = hs.stamina;
                    c.state.max_stamina = 100;
                    c.state.level = hs.level;
                    c.state.pos_x = hs.pos_x;
                    c.state.pos_y = hs.pos_y;
                    break;
                }
            }
        }
        // Load per-character bot config
        if (!hs.char_name.empty() && bot_char_name_ != hs.char_name) {
            load_bot_config(hs.char_name);
        }
        // Link character to account
        if (!hs.char_name.empty() && conn.account_index >= 0 && conn.account_index < (int)accounts_.size()) {
            auto& acc = accounts_[conn.account_index];
            if (acc.char_name.empty()) {
                acc.char_name = hs.char_name;
                add_log(std::format("[ACCOUNT] {} -> {}", acc.username, hs.char_name));
            }
        }
        break;
    }
    case 1017: { // MsgUserAttrib — stat updates
        if (outgoing) break;
        for (auto& f : fields) {
            if (f.field_num == 1 && f.wire_type == proto::LENGTH_DELIMITED) {
                auto sub = proto::decode(f.bytes.data(), f.bytes.size());
                uint32_t pid = (uint32_t)proto::get_uint(sub, 1);
                uint32_t atype = (uint32_t)proto::get_uint(sub, 2);
                uint64_t data = proto::get_uint(sub, 3);
                if (pid != conn.hero_id) continue;

                switch (atype) {
                case 1:  hs.hp = (int)data; break;
                case 2:  hs.max_hp = (int)data; break;
                case 3:  hs.mp = (int)data; break;
                case 4:  hs.max_mp = (int)data; break;
                case 5:  hs.silver = (int64_t)data; break;
                case 10: hs.stamina = (int)data; hs.max_stamina = hs.is_vip() ? 150 : 100; break;
                case 14: hs.level = (int)data; break;
                case 27: hs.status_flag = (int64_t)data; break;
                }
            }
        }
        // Merge into connected client
        {
            std::lock_guard lock(clients_mutex_);
            for (auto& c : clients_) {
                if (c.id == conn.dll_client_id) {
                    c.state.hp = hs.hp; c.state.max_hp = hs.max_hp;
                    c.state.mp = hs.mp; c.state.max_mp = hs.max_mp;
                    c.state.silver = hs.silver; c.state.stamina = hs.stamina;
                    c.state.max_stamina = hs.max_stamina;
                    c.state.level = hs.level; c.state.status_flag = hs.status_flag;
                    break;
                }
            }
        }
        break;
    }
    case 1010: { // MsgAction — position/action updates
        if (outgoing) break;
        uint32_t pid = (uint32_t)proto::get_int(fields, 2);
        int action = (int)proto::get_int(fields, 1);

        if (action == 18 && pid != conn.hero_id) {
            conn.entities.erase(
                std::remove_if(conn.entities.begin(), conn.entities.end(),
                    [pid](auto& e) { return e.id == pid; }),
                conn.entities.end());
        }

        if (pid != conn.hero_id && action != 18) {
            int x = (int)proto::get_int(fields, 4);
            int y = (int)proto::get_int(fields, 5);
            if (x > 0 && y > 0) {
                for (auto& e : conn.entities) {
                    if (e.id == pid) { e.x = x; e.y = y; break; }
                }
            }
        }

        if (pid == conn.hero_id) {
            int x = (int)proto::get_int(fields, 4);
            int y = (int)proto::get_int(fields, 5);
            int old_x = hs.pos_x, old_y = hs.pos_y;
            if (x > 0 && y > 0) { hs.pos_x = x; hs.pos_y = y; }

            bool moved = (hs.pos_x != old_x || hs.pos_y != old_y);
            if (moved) {
                constexpr int VIEW_RANGE = 18;
                conn.entities.erase(
                    std::remove_if(conn.entities.begin(), conn.entities.end(),
                        [&](auto& e) {
                            int dx = std::abs(e.x - hs.pos_x), dy = std::abs(e.y - hs.pos_y);
                            return (dx > VIEW_RANGE || dy > VIEW_RANGE);
                        }),
                    conn.entities.end());
                conn.ground_items.erase(
                    std::remove_if(conn.ground_items.begin(), conn.ground_items.end(),
                        [&](auto& g) {
                            int dx = std::abs(g.x - hs.pos_x), dy = std::abs(g.y - hs.pos_y);
                            return (dx > VIEW_RANGE || dy > VIEW_RANGE);
                        }),
                    conn.ground_items.end());
            }

            std::lock_guard lock(clients_mutex_);
            for (auto& c : clients_) {
                if (c.id == conn.dll_client_id) {
                    c.state.pos_x = hs.pos_x; c.state.pos_y = hs.pos_y;
                    c.nearby = conn.entities;
                    c.ground_items = conn.ground_items;
                    break;
                }
            }
        }
        break;
    }
    case 1110: { // MsgMapInfo — map change
        if (outgoing) break;
        conn.map_idd = (uint32_t)proto::get_uint(fields, 1);
        conn.doc_idd = (uint32_t)proto::get_uint(fields, 2);
        current_map_idd_ = conn.map_idd;
        current_doc_idd_ = conn.doc_idd;
        hs.map_id = conn.doc_idd;
        conn.entities.clear();
        conn.ground_items.clear();
        conn.portals.clear();
        std::lock_guard lock(clients_mutex_);
        for (auto& c : clients_) {
            if (c.id == conn.dll_client_id) {
                c.state.map_id = hs.map_id;
                c.nearby.clear();
                c.ground_items.clear();
                break;
            }
        }
        break;
    }
    case 1014: { // MsgPlayer — entity spawned/updated
        if (outgoing) break;
        uint32_t pid = (uint32_t)proto::get_uint(fields, 1);
        if (pid == conn.hero_id) break;

        NearbyEntity ne;
        ne.id = pid;
        ne.x = (int)proto::get_uint(fields, 15);
        ne.y = (int)proto::get_uint(fields, 16);
        ne.look_type = (uint16_t)proto::get_uint(fields, 2);
        ne.role_kind = (int)proto::get_uint(fields, 29);
        ne.npc_sort = (int)proto::get_uint(fields, 30);
        auto name = proto::get_string(fields, 24);
        strncpy(ne.name, name.c_str(), 16);
        ne.name[16] = 0;
        uint32_t hp = (uint32_t)proto::get_uint(fields, 11);
        ne.dead = (hp == 0);

        // Determine type: if roleType > 0 it's NPC/Monster, else Player
        if (ne.role_kind > 0) ne.type = 6; // non-player
        else ne.type = 7; // player

        // Update or add
        bool updated = false;
        for (auto& e : conn.entities) {
            if (e.id == pid) { e = ne; updated = true; break; }
        }
        if (!updated) conn.entities.push_back(ne);

        // Sync to client
        {
            std::lock_guard lock(clients_mutex_);
            for (auto& c : clients_) {
                if (c.id == conn.dll_client_id) { c.nearby = conn.entities; break; }
            }
        }
        break;
    }
    case 1101: { // MsgMapItem — ground item / portal
        if (outgoing) break;
        uint32_t action = (uint32_t)proto::get_uint(fields, 6);
        uint32_t item_id = (uint32_t)proto::get_uint(fields, 1);
        uint32_t item_type = (uint32_t)proto::get_uint(fields, 2);
        int ix = (int)proto::get_uint(fields, 3);
        int iy = (int)proto::get_uint(fields, 4);

        // Portal tracking: action 10=SyncTrap, 11=DropTrap, type 24=portal
        if ((action == 10 || action == 11) && item_type == 24) {
            bool exists = false;
            for (auto& p : conn.portals)
                if (p.id == item_id) { p.x = ix; p.y = iy; exists = true; break; }
            if (!exists)
                conn.portals.push_back({item_id, ix, iy});
        }

        // Ground items
        if (action == 1 || action == 3) { // Create/drop
            GroundItemInfo gi;
            gi.item_id = item_id;
            gi.type_id = item_type;
            gi.x = ix; gi.y = iy;
            conn.ground_items.push_back(gi);
        } else if (action == 2 || action == 4) { // Delete/pick
            conn.ground_items.erase(
                std::remove_if(conn.ground_items.begin(), conn.ground_items.end(),
                    [item_id](auto& g) { return g.item_id == item_id; }),
                conn.ground_items.end());
        }

        {
            std::lock_guard lock(clients_mutex_);
            for (auto& c : clients_) {
                if (c.id == conn.dll_client_id) { c.ground_items = conn.ground_items; break; }
            }
        }
        break;
    }
    case 1005: { // MsgWalk
        break;
    }
    case 2030: { // MsgNpcInfo — NPC spawn (id + full data via MsgPlayer follows)
        // NpcInfo might carry position/name in additional fields not in the basic proto
        // For now just note it — the MsgPlayer with matching ID should populate the entity
        break;
    }
    case 7004: { // MsgVip — VIP status
        if (outgoing) break;
        bool vip = proto::get_uint(fields, 3) != 0;
        hs.max_stamina = vip ? 150 : 100;
        if (vip) hs.bool_flags |= (1 << 14);
        else hs.bool_flags &= ~(1 << 14);
        std::lock_guard lock(clients_mutex_);
        for (auto& c : clients_) {
            if (c.id == conn.dll_client_id) {
                c.state.max_stamina = hs.max_stamina;
                c.state.bool_flags = hs.bool_flags;
                break;
            }
        }
        break;
    }
    case 1008: { // MsgItemInfo — item added/updated in inventory or equipment
        if (outgoing) break;
        int mode = (int)proto::get_int(fields, 5);
        int pos = (int)proto::get_int(fields, 7);
        // mode: 1=Add, 2=Update, 3=Trade, 4=OtherPlayer
        if (mode == 1 || mode == 2) {
            uint32_t item_id = (uint32_t)proto::get_int(fields, 1);
            uint32_t type_id = (uint32_t)proto::get_int(fields, 2);
            int amount = (int)proto::get_int(fields, 3);
            int amount_limit = (int)proto::get_int(fields, 4);
            int plus = (int)proto::get_int(fields, 12);
            int gem1 = (int)proto::get_int(fields, 8);
            int gem2 = (int)proto::get_int(fields, 9);

            if (pos >= 1 && pos <= 9) {
                // Equipment slot
                conn.equip_slots[pos] = {item_id, type_id, plus, gem1, gem2};
            } else {
                // Inventory (pos=0 or bag)
                InvItemInfo it{};
                it.item_id = item_id;
                it.type_id = type_id;
                it.amount = amount;
                it.amount_limit = amount_limit;
                auto nit = item_names_.find(type_id);
                if (nit != item_names_.end())
                    strncpy(it.name, nit->second.c_str(), 16);
                it.name[16] = 0;

                bool found = false;
                for (auto& inv : conn.inventory) {
                    if (inv.item_id == item_id) { inv = it; found = true; break; }
                }
                if (!found) conn.inventory.push_back(it);
            }
        }
        break;
    }
    case 1009: { // MsgItem — item actions (drop, use, unequip, synch amount, etc)
        if (outgoing) break; // only process server confirmations
        int action = (int)proto::get_int(fields, 5);
        uint32_t item_id = (uint32_t)proto::get_int(fields, 1);

        switch (action) {
        case 3: // Drop
        case 4: // Use/consume
            conn.inventory.erase(
                std::remove_if(conn.inventory.begin(), conn.inventory.end(),
                    [item_id](auto& it) { return it.item_id == item_id; }),
                conn.inventory.end());
            break;
        case 25: { // SynchroAmount
            int amount = (int)proto::get_int(fields, 2);
            for (auto& it : conn.inventory) {
                if (it.item_id == item_id) { it.amount = amount; break; }
            }
            break;
        }
        }
        break;
    }
    case 1102: { // MsgPackage — bulk inventory data
        if (outgoing) break;
        uint32_t action = (uint32_t)proto::get_uint(fields, 2);
        // action 1 = list items
        if (action == 1) {
            conn.inventory.clear();
            for (auto& f : fields) {
                if (f.field_num == 5 && f.wire_type == proto::LENGTH_DELIMITED) {
                    auto sub = proto::decode(f.bytes.data(), f.bytes.size());
                    InvItemInfo it{};
                    it.item_id = (uint32_t)proto::get_uint(sub, 1);
                    it.type_id = (uint32_t)proto::get_uint(sub, 2);
                    it.amount = (int)proto::get_uint(sub, 10);
                    it.amount_limit = (int)proto::get_uint(sub, 11);
                    auto nit = item_names_.find(it.type_id);
                    if (nit != item_names_.end())
                        strncpy(it.name, nit->second.c_str(), 16);
                    it.name[16] = 0;
                    conn.inventory.push_back(it);
                }
            }
        }
        break;
    }
    }
}

// ── Per-client TCP Proxy ─────────────────────────────────────────────

bool App::is_port_available(uint16_t port) {
    SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == INVALID_SOCKET) return false;
    int opt = 1;
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, (char*)&opt, sizeof(opt));
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    bool ok = (bind(s, (sockaddr*)&addr, sizeof(addr)) != SOCKET_ERROR);
    closesocket(s);
    return ok;
}

uint16_t App::allocate_login_port(int account_index) {
    return BASE_PROXY_PORT + (uint16_t)(account_index * 2);
}

ProxyConnection* App::find_proxy_for(int account_index) {
    std::lock_guard lk(proxy_connections_mutex_);
    for (auto& p : proxy_connections_)
        if (p && p->account_index == account_index) return p.get();
    return nullptr;
}

ProxyConnection* App::get_active_proxy() {
    int idx = active_account_;
    if (idx < 0 || idx >= (int)accounts_.size()) {
        // Fallback: return first running proxy
        std::lock_guard lk(proxy_connections_mutex_);
        for (auto& p : proxy_connections_)
            if (p && p->running.load()) return p.get();
        return nullptr;
    }
    return find_proxy_for(idx);
}

void App::start_proxy_for(int account_index) {
    if (account_index < 0 || account_index >= (int)accounts_.size()) return;

    // Check if already running
    if (find_proxy_for(account_index)) return;

    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);

    uint16_t login_port = allocate_login_port(account_index);
    uint16_t game_port = login_port + 1;

    if (!is_port_available(login_port)) {
        add_log(std::format("[PROXY] Login port {} not available for account {}", login_port, account_index));
        return;
    }
    if (!is_port_available(game_port)) {
        add_log(std::format("[PROXY] Game port {} not available for account {}", game_port, account_index));
        return;
    }

    auto conn = std::make_unique<ProxyConnection>();
    conn->account_index = account_index;
    conn->login_port = login_port;
    conn->game_port = game_port;

    // Bind login listener
    conn->login_listen = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (conn->login_listen == INVALID_SOCKET) {
        add_log("[PROXY] Failed to create login listen socket");
        return;
    }
    int opt = 1;
    setsockopt(conn->login_listen, SOL_SOCKET, SO_REUSEADDR, (char*)&opt, sizeof(opt));
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(login_port);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (bind(conn->login_listen, (sockaddr*)&addr, sizeof(addr)) == SOCKET_ERROR) {
        add_log(std::format("[PROXY] Bind failed on login port {}", login_port));
        closesocket(conn->login_listen);
        return;
    }
    listen(conn->login_listen, 2);

    // Bind game listener
    conn->game_listen = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (conn->game_listen == INVALID_SOCKET) {
        add_log("[PROXY] Failed to create game listen socket");
        closesocket(conn->login_listen);
        return;
    }
    setsockopt(conn->game_listen, SOL_SOCKET, SO_REUSEADDR, (char*)&opt, sizeof(opt));
    sockaddr_in gaddr{};
    gaddr.sin_family = AF_INET;
    gaddr.sin_port = htons(game_port);
    gaddr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (bind(conn->game_listen, (sockaddr*)&gaddr, sizeof(gaddr)) == SOCKET_ERROR) {
        add_log(std::format("[PROXY] Bind failed on game port {}", game_port));
        closesocket(conn->login_listen);
        closesocket(conn->game_listen);
        return;
    }
    listen(conn->game_listen, 2);

    // Resolve login server address
    int server_idx = accounts_[account_index].server;
    sockaddr_in login_server_addr{};
    bool resolved = false;
    {
        std::string server_host;
        uint16_t server_port = 0;
        std::string path = client_path_ + "\\servers.json";
        std::ifstream f(path);
        if (f.is_open()) {
            try {
                auto j = nlohmann::json::parse(f);
                if (j.is_array() && !j.empty()) {
                    auto& group = j[0];
                    if (group.contains("servers") && !group["servers"].empty()) {
                        int idx = server_idx;
                        if (idx < 0 || idx >= (int)group["servers"].size()) idx = 0;
                        auto& srv = group["servers"][idx];
                        server_host = srv.value("address", "");
                        server_port = srv.value("port", 0);
                    }
                }
            } catch (...) {}
        }
        if (!server_host.empty() && server_port > 0) {
            addrinfo hints{}, *result = nullptr;
            hints.ai_family = AF_INET;
            hints.ai_socktype = SOCK_STREAM;
            if (getaddrinfo(server_host.c_str(), std::to_string(server_port).c_str(), &hints, &result) == 0 && result) {
                login_server_addr = *reinterpret_cast<sockaddr_in*>(result->ai_addr);
                freeaddrinfo(result);
                resolved = true;
                uint32_t rip = ntohl(login_server_addr.sin_addr.s_addr);
                add_log(std::format("[PROXY] Account {} login server: {} -> {}.{}.{}.{}:{}",
                    account_index, server_host,
                    (rip>>24)&0xFF, (rip>>16)&0xFF, (rip>>8)&0xFF, rip&0xFF, server_port));
            } else {
                add_log(std::format("[PROXY] DNS resolve failed for {}", server_host));
            }
        }
    }

    if (!resolved) {
        add_log("[PROXY] Failed to resolve login server");
        conn->close_listeners();
        return;
    }

    conn->running.store(true);
    auto* raw = conn.get();
    conn->accept_thread = std::thread(&App::proxy_accept_loop, this, raw, login_server_addr);

    std::lock_guard lk(proxy_connections_mutex_);
    proxy_connections_.push_back(std::move(conn));
    add_log(std::format("[PROXY] Started for account {} (login:{} game:{})",
        account_index, login_port, game_port));
}

void App::stop_proxy_for(int account_index) {
    std::unique_ptr<ProxyConnection> conn_ptr;
    {
        std::lock_guard lk(proxy_connections_mutex_);
        for (auto it = proxy_connections_.begin(); it != proxy_connections_.end(); ++it) {
            if (*it && (*it)->account_index == account_index) {
                conn_ptr = std::move(*it);
                proxy_connections_.erase(it);
                break;
            }
        }
    }
    if (!conn_ptr) return;

    conn_ptr->running.store(false);
    conn_ptr->close_listeners();
    conn_ptr->close_sockets();
    if (conn_ptr->accept_thread.joinable())
        conn_ptr->accept_thread.join();
    add_log(std::format("[PROXY] Stopped for account {}", account_index));
}

void App::stop_all_proxies() {
    std::vector<std::unique_ptr<ProxyConnection>> all;
    {
        std::lock_guard lk(proxy_connections_mutex_);
        all = std::move(proxy_connections_);
        proxy_connections_.clear();
    }
    for (auto& c : all) {
        if (!c) continue;
        c->running.store(false);
        c->close_listeners();
        c->close_sockets();
        if (c->accept_thread.joinable())
            c->accept_thread.join();
    }
}

// Process encrypted data: decrypt, parse, re-encrypt, return data to forward
std::vector<uint8_t> App::process_proxy_data(std::vector<uint8_t>& buf, bool outgoing,
                                              CipherSet& ciphers, ProxyConnection& conn) {
    auto& dec = outgoing ? ciphers.client_dec : ciphers.server_dec;
    auto& enc = outgoing ? ciphers.server_enc : ciphers.client_enc;
    std::vector<uint8_t> to_forward;

    while (buf.size() >= 4) {
        int saved_dec = dec.counter;
        uint8_t hdr[4];
        memcpy(hdr, buf.data(), 4);
        dec.decrypt(hdr, 4);

        uint16_t pkt_size = *reinterpret_cast<uint16_t*>(hdr);
        uint16_t pkt_type = *reinterpret_cast<uint16_t*>(hdr + 2);

        if (pkt_size < 4 || pkt_size > 60000) {
            std::vector<uint8_t> chunk(buf.begin(), buf.end());
            dec.counter = saved_dec;
            dec.decrypt(chunk.data(), chunk.size());

            CapturedPacket pkt;
            pkt.outgoing = outgoing;
            pkt.type = 0xFFFF;
            pkt.raw = chunk;
            pkt.timestamp = 0;
            if (conn.capture_on) {
                std::lock_guard lk(conn.packet_mutex);
                conn.packets.push_back(std::move(pkt));
            }

            enc.encrypt(chunk.data(), chunk.size());
            to_forward.insert(to_forward.end(), chunk.begin(), chunk.end());
            buf.clear();
            return to_forward;
        }

        if (buf.size() < pkt_size) {
            dec.counter = saved_dec;
            return to_forward;
        }

        std::vector<uint8_t> plain(buf.begin(), buf.begin() + pkt_size);
        dec.counter = saved_dec;
        dec.decrypt(plain.data(), pkt_size);

        CapturedPacket pkt;
        pkt.outgoing = outgoing;
        pkt.type = pkt_type;
        pkt.raw = plain;
        pkt.timestamp = 0;

        // Update per-connection state from packet
        {
            std::lock_guard slk(conn.state_mutex);
            handle_proxy_packet(conn, pkt_type, plain, outgoing);
        }

        if (conn.capture_on) {
            std::lock_guard lk(conn.packet_mutex);
            conn.packets.push_back(std::move(pkt));
            if (conn.packets.size() > conn.packet_max)
                conn.packets.erase(conn.packets.begin());
        }

        // Handle MsgConnectEx (1055) from server — rewrite host:port to game proxy
        if (pkt_type == 1055 && !outgoing && plain.size() > 4) {
            auto fields = proto::decode(plain);
            auto host = proto::get_string(fields, 4);
            auto port = (uint16_t)proto::get_int(fields, 5);
            if (!host.empty() && port > 0) {
                conn.game_server_ip = 0;
                conn.game_server_port = port;
                addrinfo hints{}, *res = nullptr;
                hints.ai_family = AF_INET;
                hints.ai_socktype = SOCK_STREAM;
                if (getaddrinfo(host.c_str(), std::to_string(port).c_str(), &hints, &res) == 0 && res) {
                    auto* sa = reinterpret_cast<sockaddr_in*>(res->ai_addr);
                    conn.game_server_ip = ntohl(sa->sin_addr.s_addr);
                    conn.has_game_addr.store(true);
                    freeaddrinfo(res);
                }

                add_log(std::format("[PROXY] ConnectEx: {}:{} -> rewriting to 127.0.0.1:{}",
                    host, port, conn.game_port));

                for (auto& f : fields) {
                    if (f.field_num == 4) {
                        std::string local = "127.0.0.1";
                        f.bytes.assign(local.begin(), local.end());
                    }
                    if (f.field_num == 5) {
                        f.varint = conn.game_port;
                    }
                }

                plain = proto::rebuild_packet(pkt_type, fields);
                pkt_size = (uint16_t)plain.size();
            }
        }

        enc.encrypt(plain.data(), pkt_size);
        to_forward.insert(to_forward.end(), plain.begin(), plain.end());
        buf.erase(buf.begin(), buf.begin() + pkt_size);
    }
    return to_forward;
}

void App::proxy_accept_loop(ProxyConnection* conn, sockaddr_in login_server_addr) {
    while (conn->running.load()) {
        fd_set rd;
        FD_ZERO(&rd);

        SOCKET max_sock = 0;
        if (conn->login_listen != INVALID_SOCKET) {
            FD_SET(conn->login_listen, &rd);
            if (conn->login_listen > max_sock) max_sock = conn->login_listen;
        }
        if (conn->game_listen != INVALID_SOCKET) {
            FD_SET(conn->game_listen, &rd);
            if (conn->game_listen > max_sock) max_sock = conn->game_listen;
        }

        if (max_sock == 0) break;

        timeval tv{0, 50000};
        int sel = select((int)max_sock + 1, &rd, nullptr, nullptr, &tv);
        if (sel == SOCKET_ERROR) break;
        if (sel == 0) continue;

        // Accept on login port
        if (conn->login_listen != INVALID_SOCKET && FD_ISSET(conn->login_listen, &rd)) {
            SOCKET client = accept(conn->login_listen, nullptr, nullptr);
            if (client != INVALID_SOCKET) {
                add_log(std::format("[PROXY:{}] Login client connected", conn->account_index));
                conn->is_game_session = false;
                // Connect to real login server
                SOCKET server = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
                if (server != INVALID_SOCKET &&
                    connect(server, (sockaddr*)&login_server_addr, sizeof(login_server_addr)) != SOCKET_ERROR) {
                    proxy_relay(conn, client, server, false);
                } else {
                    add_log(std::format("[PROXY:{}] Failed to connect to login server", conn->account_index));
                    closesocket(client);
                    if (server != INVALID_SOCKET) closesocket(server);
                }
            }
        }

        // Accept on game port
        if (conn->game_listen != INVALID_SOCKET && FD_ISSET(conn->game_listen, &rd)) {
            SOCKET client = accept(conn->game_listen, nullptr, nullptr);
            if (client != INVALID_SOCKET) {
                uint32_t rip = conn->game_server_ip;
                uint16_t rport = conn->game_server_port;
                if (rip == 0 || rport == 0) {
                    add_log(std::format("[PROXY:{}] Game server address not available", conn->account_index));
                    closesocket(client);
                } else {
                    add_log(std::format("[PROXY:{}] Game client connected -> {}.{}.{}.{}:{}",
                        conn->account_index, (rip>>24)&0xFF, (rip>>16)&0xFF, (rip>>8)&0xFF, rip&0xFF, rport));
                    conn->is_game_session = true;
                    sockaddr_in target{};
                    target.sin_family = AF_INET;
                    target.sin_port = htons(rport);
                    target.sin_addr.s_addr = htonl(rip);

                    SOCKET server = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
                    if (server != INVALID_SOCKET &&
                        connect(server, (sockaddr*)&target, sizeof(target)) != SOCKET_ERROR) {
                        proxy_relay(conn, client, server, true);
                    } else {
                        add_log(std::format("[PROXY:{}] Failed to connect to game server", conn->account_index));
                        closesocket(client);
                        if (server != INVALID_SOCKET) closesocket(server);
                    }
                }
            }
        }
    }
}

void App::proxy_relay(ProxyConnection* conn, SOCKET client, SOCKET server, bool is_game) {
    conn->ciphers.reset();
    conn->client_sock = client;
    conn->server_sock = server;

    u_long nb = 1;
    ioctlsocket(client, FIONBIO, &nb);
    ioctlsocket(server, FIONBIO, &nb);

    {
        uint32_t cip = 0;
        sockaddr_in peer{};
        int peerlen = sizeof(peer);
        if (getpeername(server, (sockaddr*)&peer, &peerlen) == 0)
            cip = ntohl(peer.sin_addr.s_addr);
        add_log(std::format("[PROXY:{}] Relay started ({}) -> {}.{}.{}.{}",
            conn->account_index, is_game ? "game" : "login",
            (cip>>24)&0xFF, (cip>>16)&0xFF, (cip>>8)&0xFF, cip&0xFF));
    }

    std::vector<uint8_t> send_buf, recv_buf;
    char tmp[65536];

    while (conn->running.load()) {
        fd_set rd;
        FD_ZERO(&rd);
        FD_SET(client, &rd);
        FD_SET(server, &rd);
        timeval tv{0, 10000};

        SOCKET mx = (client > server) ? client : server;
        int sel = select((int)mx + 1, &rd, nullptr, nullptr, &tv);
        if (sel == SOCKET_ERROR) break;

        // Drain injected packets
        {
            std::lock_guard lk(conn->inject_mutex);
            for (auto& pkt : conn->inject_to_server) {
                if (pkt.size() >= 4) {
                    uint16_t ptype = *reinterpret_cast<uint16_t*>(pkt.data() + 2);
                    CapturedPacket cp;
                    cp.outgoing = true; cp.type = ptype; cp.raw = pkt; cp.timestamp = 0;
                    std::lock_guard lk2(conn->packet_mutex);
                    conn->packets.push_back(std::move(cp));
                }
                std::vector<uint8_t> enc(pkt);
                conn->ciphers.server_enc.encrypt(enc.data(), enc.size());
                ::send(server, (char*)enc.data(), (int)enc.size(), 0);
            }
            conn->inject_to_server.clear();
        }

        // Client -> Server
        if (FD_ISSET(client, &rd)) {
            int n = recv(client, tmp, sizeof(tmp), 0);
            if (n <= 0) { add_log(std::format("[PROXY:{}] Client disconnected", conn->account_index)); break; }
            send_buf.insert(send_buf.end(), tmp, tmp + n);
            auto fwd = process_proxy_data(send_buf, true, conn->ciphers, *conn);
            if (!fwd.empty()) {
                int sent = 0;
                while (sent < (int)fwd.size()) {
                    int s = ::send(server, (char*)fwd.data() + sent, (int)fwd.size() - sent, 0);
                    if (s <= 0) break;
                    sent += s;
                }
                if (sent < (int)fwd.size()) { add_log(std::format("[PROXY:{}] Server send failed", conn->account_index)); break; }
            }
        }

        // Server -> Client
        if (FD_ISSET(server, &rd)) {
            int n = recv(server, tmp, sizeof(tmp), 0);
            if (n <= 0) { add_log(std::format("[PROXY:{}] Server disconnected", conn->account_index)); break; }
            recv_buf.insert(recv_buf.end(), tmp, tmp + n);
            auto fwd = process_proxy_data(recv_buf, false, conn->ciphers, *conn);
            if (!fwd.empty()) {
                int sent = 0;
                while (sent < (int)fwd.size()) {
                    int s = ::send(client, (char*)fwd.data() + sent, (int)fwd.size() - sent, 0);
                    if (s <= 0) break;
                    sent += s;
                }
                if (sent < (int)fwd.size()) { add_log(std::format("[PROXY:{}] Client send failed", conn->account_index)); break; }
            }
        }
    }

    closesocket(client);
    closesocket(server);
    conn->client_sock = INVALID_SOCKET;
    conn->server_sock = INVALID_SOCKET;
    add_log(std::format("[PROXY:{}] Relay ended ({})", conn->account_index, is_game ? "game" : "login"));
}

// ── Config persistence ───────────────────────────────────────────────

void App::load_item_db() {
    // Try to load from client_path/ini/itemtype.json
    std::string path = client_path_.empty() ? "" : client_path_ + "\\ini\\itemtype.json";
    if (path.empty() || !std::filesystem::exists(path)) return;

    try {
        std::ifstream f(path);
        auto data = nlohmann::json::parse(f);
        for (auto& item : data) {
            if (item.contains("id") && item.contains("name")) {
                uint32_t id = item["id"].get<uint32_t>();
                std::string name = item["name"].get<std::string>();
                item_names_[id] = name;
                uint32_t life_val = item.value("life", 0u);
                item_life_db_[id] = {life_val};
            }
        }
        add_log(std::format("Loaded {} item types from itemtype.json", item_names_.size()));
    } catch (const std::exception& e) {
        add_log(std::format("Failed to load itemtype.json: {}", e.what()));
    }
}

void App::load_item_icon_index() {
    if (client_path_.empty()) return;
    std::string path = client_path_ + "\\ani\\ItemMinIcon.json";
    std::ifstream f(path);
    if (!f.is_open()) return;
    try {
        auto j = nlohmann::json::parse(f);
        for (auto& [key, val] : j.items()) {
            // Key format: "Item123456"
            if (key.size() > 4 && key.substr(0, 4) == "Item") {
                uint32_t type_id = std::stoul(key.substr(4));
                if (val.is_array() && !val.empty()) {
                    std::string rel = val[0].get<std::string>();
                    for (char& c : rel) if (c == '/') c = '\\';
                    item_icon_paths_[type_id] = rel;
                }
            }
        }
    } catch (...) {}
}

ID3D11ShaderResourceView* App::load_dds_texture(const std::string& path) {
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    if (!f.is_open()) return nullptr;
    auto sz = f.tellg();
    if (sz < 128) return nullptr;
    f.seekg(0);
    std::vector<char> data(sz);
    f.read(data.data(), sz);

    // Parse DDS header
    if (memcmp(data.data(), "DDS ", 4) != 0) return nullptr;
    uint32_t height, width;
    memcpy(&height, data.data() + 12, 4);
    memcpy(&width, data.data() + 16, 4);

    // Check FourCC for DXT format
    uint32_t pf_flags;
    memcpy(&pf_flags, data.data() + 80, 4);
    char fourcc[5]{};
    memcpy(fourcc, data.data() + 84, 4);

    DXGI_FORMAT fmt = DXGI_FORMAT_UNKNOWN;
    uint32_t block_size = 0;
    if (memcmp(fourcc, "DXT1", 4) == 0) { fmt = DXGI_FORMAT_BC1_UNORM; block_size = 8; }
    else if (memcmp(fourcc, "DXT3", 4) == 0) { fmt = DXGI_FORMAT_BC2_UNORM; block_size = 16; }
    else if (memcmp(fourcc, "DXT5", 4) == 0) { fmt = DXGI_FORMAT_BC3_UNORM; block_size = 16; }
    else return nullptr; // unsupported format

    uint32_t blocks_w = (width + 3) / 4;
    uint32_t blocks_h = (height + 3) / 4;
    uint32_t pitch = blocks_w * block_size;
    uint32_t data_size = pitch * blocks_h;
    const char* pixels = data.data() + 128; // skip magic(4) + header(124)

    if (128 + data_size > (uint32_t)sz) return nullptr;

    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = width;
    desc.Height = height;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = fmt;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;

    D3D11_SUBRESOURCE_DATA init{};
    init.pSysMem = pixels;
    init.SysMemPitch = pitch;

    ID3D11Texture2D* tex = nullptr;
    HRESULT hr = device_->CreateTexture2D(&desc, &init, &tex);
    if (FAILED(hr) || !tex) return nullptr;

    D3D11_SHADER_RESOURCE_VIEW_DESC srv{};
    srv.Format = fmt;
    srv.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    srv.Texture2D.MipLevels = 1;
    ID3D11ShaderResourceView* view = nullptr;
    hr = device_->CreateShaderResourceView(tex, &srv, &view);
    tex->Release();
    if (FAILED(hr)) return nullptr;
    return view;
}

void App::ensure_wdf_loaded() {
    if (wdf_loaded_ || client_path_.empty()) return;
    std::string wdf_path = client_path_ + "\\data.wdf";
    if (data_wdf_.load(wdf_path)) {
        add_log(std::format("Loaded data.wdf ({} entries)", data_wdf_.size()));
    }
    wdf_loaded_ = true;
}

ID3D11ShaderResourceView* App::load_dds_from_memory(const uint8_t* data, size_t size) {
    if (size < 128 || memcmp(data, "DDS ", 4) != 0) return nullptr;

    uint32_t height, width;
    memcpy(&height, data + 12, 4);
    memcpy(&width, data + 16, 4);

    char fourcc[5]{};
    memcpy(fourcc, data + 84, 4);

    DXGI_FORMAT fmt = DXGI_FORMAT_UNKNOWN;
    uint32_t block_size = 0;
    if (memcmp(fourcc, "DXT1", 4) == 0) { fmt = DXGI_FORMAT_BC1_UNORM; block_size = 8; }
    else if (memcmp(fourcc, "DXT3", 4) == 0) { fmt = DXGI_FORMAT_BC2_UNORM; block_size = 16; }
    else if (memcmp(fourcc, "DXT5", 4) == 0) { fmt = DXGI_FORMAT_BC3_UNORM; block_size = 16; }
    else return nullptr;

    uint32_t blocks_w = (width + 3) / 4;
    uint32_t blocks_h = (height + 3) / 4;
    uint32_t pitch = blocks_w * block_size;
    uint32_t data_size = pitch * blocks_h;
    if (128 + data_size > size) return nullptr;

    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = width; desc.Height = height;
    desc.MipLevels = 1; desc.ArraySize = 1;
    desc.Format = fmt; desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;

    D3D11_SUBRESOURCE_DATA init{};
    init.pSysMem = data + 128;
    init.SysMemPitch = pitch;

    ID3D11Texture2D* tex = nullptr;
    if (FAILED(device_->CreateTexture2D(&desc, &init, &tex)) || !tex) return nullptr;

    D3D11_SHADER_RESOURCE_VIEW_DESC srv{};
    srv.Format = fmt;
    srv.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    srv.Texture2D.MipLevels = 1;
    ID3D11ShaderResourceView* view = nullptr;
    auto hr = device_->CreateShaderResourceView(tex, &srv, &view);
    tex->Release();
    return FAILED(hr) ? nullptr : view;
}

ID3D11ShaderResourceView* App::get_item_icon(uint32_t type_id) {
    auto it = item_icon_cache_.find(type_id);
    if (it != item_icon_cache_.end()) return it->second;

    if (item_icon_paths_.empty()) load_item_icon_index();

    auto pit = item_icon_paths_.find(type_id);
    if (pit == item_icon_paths_.end()) {
        item_icon_cache_[type_id] = nullptr;
        return nullptr;
    }

    // Try loose file first
    std::string rel = pit->second;
    std::string full_path = client_path_ + "\\" + rel;
    auto* srv = load_dds_texture(full_path);

    // Fallback to WDF archive
    if (!srv) {
        ensure_wdf_loaded();
        std::vector<uint8_t> wdf_data;
        if (data_wdf_.get(rel, wdf_data)) {
            srv = load_dds_from_memory(wdf_data.data(), wdf_data.size());
        }
    }

    item_icon_cache_[type_id] = srv;
    return srv;
}

void App::load_config() {
    try {
        std::ifstream f(config_path_);
        if (!f.is_open()) return;
        nlohmann::json j;
        f >> j;
        if (j.contains("client_path"))
            client_path_ = j["client_path"].get<std::string>();
        // Legacy single account
        if (j.contains("login_user"))
            strncpy_s(login_user_, j["login_user"].get<std::string>().c_str(), 63);
        if (j.contains("login_pass"))
            strncpy_s(login_pass_, j["login_pass"].get<std::string>().c_str(), 63);
        if (j.contains("login_server"))
            login_server_ = j["login_server"].get<int>();
        // Multi-account
        if (j.contains("accounts")) {
            accounts_.clear();
            for (auto& a : j["accounts"]) {
                AccountProfile ap;
                ap.username = a.value("username", "");
                ap.password = a.value("password", "");
                ap.server = a.value("server", 0);
                if (!ap.username.empty()) accounts_.push_back(ap);
            }
        }
        // Migrate legacy single account if no accounts exist
        if (accounts_.empty() && login_user_[0]) {
            AccountProfile ap;
            ap.username = login_user_;
            ap.password = login_pass_;
            ap.server = login_server_;
            accounts_.push_back(ap);
        }
    } catch (...) {}
}

void App::save_config() {
    try {
        nlohmann::json j;
        j["client_path"] = client_path_;
        // Legacy compat
        j["login_user"] = std::string(login_user_);
        j["login_pass"] = std::string(login_pass_);
        j["login_server"] = login_server_;
        // Multi-account
        j["accounts"] = nlohmann::json::array();
        for (auto& a : accounts_) {
            j["accounts"].push_back({
                {"username", a.username}, {"password", a.password}, {"server", a.server}
            });
        }
        std::ofstream f(config_path_);
        f << j.dump(2);
    } catch (...) {}
}

void App::load_bot_config(const std::string& char_name) {
    bot_char_name_ = char_name;
    try {
        auto path = std::filesystem::path(config_path_).parent_path() / ("bot_" + char_name + ".json");
        std::ifstream f(path);
        if (!f.is_open()) return;
        nlohmann::json j;
        f >> j;
        if (j.contains("hp_pct")) bot_hp_pct_ = j["hp_pct"].get<float>();
        if (j.contains("auto_hp_pot")) bot_auto_hp_pot_ = j["auto_hp_pot"].get<bool>();
        if (j.contains("auto_mp_pot")) bot_auto_mp_pot_ = j["auto_mp_pot"].get<bool>();
        if (j.contains("mp_pct")) bot_mp_pct_ = j["mp_pct"].get<float>();
        if (j.contains("potion_threshold")) bot_potion_threshold_ = j["potion_threshold"].get<int>();
        if (j.contains("loot_plus")) bot_loot_plus_ = j["loot_plus"].get<bool>();
        if (j.contains("loot_min_quality")) bot_loot_min_quality_ = j["loot_min_quality"].get<int>();
        if (j.contains("reaction_min")) bot_reaction_min_ = j["reaction_min"].get<int>();
        if (j.contains("reaction_max")) bot_reaction_max_ = j["reaction_max"].get<int>();
        if (j.contains("attack_cd")) bot_attack_cd_ = j["attack_cd"].get<int>();
        if (j.contains("pickup_cd")) bot_pickup_cd_ = j["pickup_cd"].get<int>();
        if (j.contains("action_range")) bot_action_range_ = j["action_range"].get<int>();
        if (j.contains("jump_scatter")) bot_jump_scatter_ = j["jump_scatter"].get<int>();
        if (j.contains("item_notice_min")) bot_item_notice_min_ = j["item_notice_min"].get<int>();
        if (j.contains("item_notice_max")) bot_item_notice_max_ = j["item_notice_max"].get<int>();
        if (j.contains("auto_deposit")) bot_auto_deposit_ = j["auto_deposit"].get<bool>();
        if (j.contains("deposit_threshold")) bot_deposit_threshold_ = j["deposit_threshold"].get<int>();
        if (j.contains("hunt_mode")) bot_hunt_mode_ = j["hunt_mode"].get<int>();
        if (j.contains("pickup_hp_pots")) bot_pickup_hp_pots_ = j["pickup_hp_pots"].get<bool>();
        if (j.contains("pickup_mp_pots")) bot_pickup_mp_pots_ = j["pickup_mp_pots"].get<bool>();
        if (j.contains("magic_id")) bot_magic_id_ = j["magic_id"].get<int>();
        if (j.contains("magic_cast_delay")) bot_magic_cast_delay_ = j["magic_cast_delay"].get<int>();
        if (j.contains("magic_safe_dist")) bot_magic_safe_dist_ = j["magic_safe_dist"].get<int>();
        if (j.contains("kite_enabled")) bot_kite_enabled_ = j["kite_enabled"].get<bool>();
        if (j.contains("monster_filter_mode")) bot_monster_filter_mode_ = j["monster_filter_mode"].get<int>();
        if (j.contains("monster_filter_names")) {
            bot_monster_filter_names_.clear();
            for (auto& n : j["monster_filter_names"])
                bot_monster_filter_names_.push_back(n.get<std::string>());
        }
        if (j.contains("kite_min_dist")) bot_kite_min_dist_ = j["kite_min_dist"].get<int>();
        if (j.contains("prefer_clusters")) bot_prefer_clusters_ = j["prefer_clusters"].get<bool>();
        if (j.contains("cluster_radius")) bot_cluster_radius_ = j["cluster_radius"].get<int>();
        if (j.contains("cluster_min_stay")) bot_cluster_min_stay_ = j["cluster_min_stay"].get<int>();
        if (j.contains("use_xp_skill")) bot_use_xp_skill_ = j["use_xp_skill"].get<bool>();
        else if (j.contains("use_cyclone")) bot_use_xp_skill_ = j["use_cyclone"].get<bool>();
        if (j.contains("xp_skill_id")) bot_xp_skill_id_ = j["xp_skill_id"].get<int>();
        else if (j.contains("cyclone_magic_id")) bot_xp_skill_id_ = j["cyclone_magic_id"].get<int>();
        if (j.contains("xp_skill_delay")) bot_xp_skill_delay_ = j["xp_skill_delay"].get<int>();
        else if (j.contains("cyclone_delay")) bot_xp_skill_delay_ = j["cyclone_delay"].get<int>();
        if (j.contains("attack_cd_xp")) bot_attack_cd_xp_ = j["attack_cd_xp"].get<int>();
        else if (j.contains("attack_cd_cyclone")) bot_attack_cd_xp_ = j["attack_cd_cyclone"].get<int>();
        if (j.contains("use_scatter")) bot_use_scatter_ = j["use_scatter"].get<bool>();
        if (j.contains("scatter_magic_id")) bot_scatter_magic_id_ = j["scatter_magic_id"].get<int>();
        if (j.contains("scatter_cooldown")) bot_scatter_cooldown_ = j["scatter_cooldown"].get<int>();
        if (j.contains("scatter_range")) bot_scatter_range_ = j["scatter_range"].get<float>();
        if (j.contains("scatter_half_angle")) bot_scatter_half_angle_ = j["scatter_half_angle"].get<float>();
        if (j.contains("scatter_min_targets")) bot_scatter_min_targets_ = j["scatter_min_targets"].get<int>();
        if (j.contains("scatter_predict_hits")) bot_scatter_predict_hits_ = j["scatter_predict_hits"].get<bool>();
        if (j.contains("explore_radius")) bot_explore_radius_ = j["explore_radius"].get<int>();
        if (j.contains("pickup_types")) {
            bot_pickup_types_.clear();
            for (auto& v : j["pickup_types"]) bot_pickup_types_.push_back(v.get<uint32_t>());
        }
        // Player safety
        if (j.contains("safety_enabled")) bot_safety_enabled_ = j["safety_enabled"].get<bool>();
        if (j.contains("safety_flee_distance")) bot_safety_flee_distance_ = j["safety_flee_distance"].get<int>();
        if (j.contains("safety_max_encounters")) bot_safety_max_encounters_ = j["safety_max_encounters"].get<int>();
        if (j.contains("safety_encounter_window")) bot_safety_encounter_window_ = j["safety_encounter_window"].get<int>();
        if (j.contains("safety_player_action")) bot_safety_player_action_ = j["safety_player_action"].get<int>();
        if (j.contains("safety_safe_x")) bot_safety_safe_x_ = j["safety_safe_x"].get<int>();
        if (j.contains("safety_safe_y")) bot_safety_safe_y_ = j["safety_safe_y"].get<int>();
        if (j.contains("safety_safe_map")) bot_safety_safe_map_ = j["safety_safe_map"].get<int>();
        if (j.contains("safety_wait_min")) bot_safety_wait_min_ = j["safety_wait_min"].get<int>();
        if (j.contains("safety_wait_max")) bot_safety_wait_max_ = j["safety_wait_max"].get<int>();
        if (j.contains("safety_gm_detect")) bot_safety_gm_detect_ = j["safety_gm_detect"].get<bool>();
        if (j.contains("safety_gm_action")) bot_safety_gm_action_ = j["safety_gm_action"].get<int>();
        if (j.contains("safety_gm_wait_min")) bot_safety_gm_wait_min_ = j["safety_gm_wait_min"].get<int>();
        if (j.contains("safety_gm_wait_max")) bot_safety_gm_wait_max_ = j["safety_gm_wait_max"].get<int>();
        if (j.contains("safety_gm_sound")) bot_safety_gm_sound_ = j["safety_gm_sound"].get<bool>();
        add_log(std::format("Loaded bot config for '{}'", char_name));
    } catch (...) {}
}

void App::save_bot_config(const std::string& char_name) {
    if (char_name.empty()) return;
    try {
        nlohmann::json j;
        j["hp_pct"] = bot_hp_pct_;
        j["auto_hp_pot"] = bot_auto_hp_pot_;
        j["auto_mp_pot"] = bot_auto_mp_pot_;
        j["mp_pct"] = bot_mp_pct_;
        j["potion_threshold"] = bot_potion_threshold_;
        j["loot_plus"] = bot_loot_plus_;
        j["loot_min_quality"] = bot_loot_min_quality_;
        j["reaction_min"] = bot_reaction_min_;
        j["reaction_max"] = bot_reaction_max_;
        j["attack_cd"] = bot_attack_cd_;
        j["pickup_cd"] = bot_pickup_cd_;
        j["action_range"] = bot_action_range_;
        j["jump_scatter"] = bot_jump_scatter_;
        j["item_notice_min"] = bot_item_notice_min_;
        j["item_notice_max"] = bot_item_notice_max_;
        j["auto_deposit"] = bot_auto_deposit_;
        j["deposit_threshold"] = bot_deposit_threshold_;
        j["hunt_mode"] = bot_hunt_mode_;
        j["pickup_hp_pots"] = bot_pickup_hp_pots_;
        j["pickup_mp_pots"] = bot_pickup_mp_pots_;
        j["magic_id"] = bot_magic_id_;
        j["magic_cast_delay"] = bot_magic_cast_delay_;
        j["magic_safe_dist"] = bot_magic_safe_dist_;
        j["kite_enabled"] = bot_kite_enabled_;
        j["monster_filter_mode"] = bot_monster_filter_mode_;
        j["monster_filter_names"] = bot_monster_filter_names_;
        j["kite_min_dist"] = bot_kite_min_dist_;
        j["prefer_clusters"] = bot_prefer_clusters_;
        j["cluster_radius"] = bot_cluster_radius_;
        j["cluster_min_stay"] = bot_cluster_min_stay_;
        j["use_xp_skill"] = bot_use_xp_skill_;
        j["xp_skill_id"] = bot_xp_skill_id_;
        j["xp_skill_delay"] = bot_xp_skill_delay_;
        j["attack_cd_xp"] = bot_attack_cd_xp_;
        j["use_scatter"] = bot_use_scatter_;
        j["scatter_magic_id"] = bot_scatter_magic_id_;
        j["scatter_cooldown"] = bot_scatter_cooldown_;
        j["scatter_range"] = bot_scatter_range_;
        j["scatter_half_angle"] = bot_scatter_half_angle_;
        j["scatter_min_targets"] = bot_scatter_min_targets_;
        j["scatter_predict_hits"] = bot_scatter_predict_hits_;
        j["explore_radius"] = bot_explore_radius_;
        j["pickup_types"] = bot_pickup_types_;
        // Player safety
        j["safety_enabled"] = bot_safety_enabled_;
        j["safety_flee_distance"] = bot_safety_flee_distance_;
        j["safety_max_encounters"] = bot_safety_max_encounters_;
        j["safety_encounter_window"] = bot_safety_encounter_window_;
        j["safety_player_action"] = bot_safety_player_action_;
        j["safety_safe_x"] = bot_safety_safe_x_;
        j["safety_safe_y"] = bot_safety_safe_y_;
        j["safety_safe_map"] = bot_safety_safe_map_;
        j["safety_wait_min"] = bot_safety_wait_min_;
        j["safety_wait_max"] = bot_safety_wait_max_;
        j["safety_gm_detect"] = bot_safety_gm_detect_;
        j["safety_gm_action"] = bot_safety_gm_action_;
        j["safety_gm_wait_min"] = bot_safety_gm_wait_min_;
        j["safety_gm_wait_max"] = bot_safety_gm_wait_max_;
        j["safety_gm_sound"] = bot_safety_gm_sound_;
        auto path = std::filesystem::path(config_path_).parent_path() / ("bot_" + char_name + ".json");
        std::ofstream f(path);
        f << j.dump(2);
    } catch (...) {}
}

// ── Launch + inject ─────────────────────────────────────────────────

void App::launch_and_inject() {
    if (client_path_.empty()) {
        add_log("Client path not set");
        return;
    }

    std::string exe_path = client_path_ + "\\bin\\64\\ImConquer.exe";
    if (!std::filesystem::exists(exe_path)) {
        add_log(std::format("Exe not found: {}", exe_path));
        return;
    }

    if (!std::filesystem::exists(dll_path_)) {
        add_log(std::format("DLL not found: {}", dll_path_));
        return;
    }

    launching_ = true;
    add_log("Launching game...");

    std::thread([this, exe_path]() {
        // Create the game process
        std::string cmd_line = "\"" + exe_path + "\"";
        STARTUPINFOA si{};
        si.cb = sizeof(si);
        PROCESS_INFORMATION pi{};

        BOOL ok = CreateProcessA(
            nullptr,
            cmd_line.data(),
            nullptr, nullptr, FALSE, 0, nullptr,
            client_path_.c_str(),
            &si, &pi);

        if (!ok) {
            add_log(std::format("CreateProcess failed (error {})", GetLastError()));
            launching_ = false;
            return;
        }

        add_log(std::format("Game started (PID: {}), waiting before injection...", pi.dwProcessId));

        // Wait for the game to initialize
        Sleep(5000);

        // Inject
        add_log("Injecting DLL...");
        bool result = ccbot::inject_manual_map(dll_path_, pi.dwProcessId, [this](const char* msg) {
            add_log(std::string("[inject] ") + msg);
        });

        if (result)
            add_log("Injection succeeded");
        else
            add_log("Injection FAILED");

        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);
        launching_ = false;
    }).detach();
}

// ── Launch panel ────────────────────────────────────────────────────

void App::render_launch_panel() {
    if (ImGui::CollapsingHeader("Launch", ImGuiTreeNodeFlags_DefaultOpen)) {
        // Client path (compact)
        static char path_buf[512]{};
        if (path_buf[0] == '\0' && !client_path_.empty())
            strncpy_s(path_buf, client_path_.c_str(), sizeof(path_buf) - 1);
        ImGui::SetNextItemWidth(-1);
        if (ImGui::InputText("##cpath", path_buf, sizeof(path_buf)))
            client_path_ = path_buf;

        bool exe_ok = !client_path_.empty() && std::filesystem::exists(client_path_ + "\\bin\\64\\ImConquer.exe");
        bool dll_ok = std::filesystem::exists(dll_path_);

        // Accounts list
        ImGui::Separator();
        static const char* servers[] = {"Classic (US)", "Classic (EU)", "Classic Murica (US)"};

        // Take a snapshot of account data to avoid iterator issues
        int delete_idx = -1;
        int num_accounts = (int)accounts_.size();
        for (int i = 0; i < num_accounts; i++) {
            // Copy strings to avoid reference invalidation
            std::string uname = accounts_[i].username;
            std::string cname = accounts_[i].char_name;
            std::string upass = accounts_[i].password;
            int usrv = accounts_[i].server;
            bool launched = accounts_[i].launched;

            ImGui::PushID(i + 100000);
            bool is_active = (active_account_ == i);

            // Check connection via client_id
            uint32_t cid = accounts_[i].client_id;
            bool connected = false;
            if (cid != 0) {
                std::lock_guard lock(clients_mutex_);
                for (auto& c : clients_) {
                    if (c.id == cid && c.connected) { connected = true; break; }
                }
            }
            accounts_[i].connected = connected;
            if (!connected) cid = 0;

            // Status indicator
            if (connected) ImGui::TextColored({0,0.8f,0,1}, "*");
            else if (launched) ImGui::TextColored({1,1,0,1}, "*");
            else ImGui::TextColored({0.4f,0.4f,0.4f,1}, "o");
            ImGui::SameLine();

            // Selectable name
            ImVec4 col = is_active ? ImVec4(0.3f,0.7f,1,1) : ImVec4(0.9f,0.9f,0.9f,1);
            if (ImGui::Selectable(std::format("{}##acc", uname).c_str(), is_active, 0, {100, 0})) {
                active_account_ = i;
                strncpy_s(login_user_, uname.c_str(), 63);
                strncpy_s(login_pass_, upass.c_str(), 63);
                login_server_ = usrv;
                // Load this account's bot config when switching
                if (!cname.empty()) {
                    load_bot_config(cname);
                }
                // Force minimap reload
                minimap_loaded_map_ = 0;
            }

            if (!cname.empty()) {
                ImGui::SameLine();
                ImGui::TextDisabled("%s", cname.c_str());
            }

            // Action buttons
            ImGui::SameLine(ImGui::GetContentRegionAvail().x - 90);

            if (!launched && exe_ok && dll_ok) {
                // Launch button (blue)
                ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.15f, 0.4f, 0.75f, 1));
                if (ImGui::SmallButton("Launch##go")) {
                    active_account_ = i;
                    strncpy_s(login_user_, uname.c_str(), 63);
                    strncpy_s(login_pass_, upass.c_str(), 63);
                    login_server_ = usrv;
                    save_config();
                    launch_and_inject();
                    accounts_[i].launched = true;
                }
                ImGui::PopStyleColor();
            } else if (launched && !connected) {
                ImGui::TextColored({1,1,0,0.5f}, "...");
            } else if (connected && cid && !cname.empty()) {
                // Bot Start/Stop toggle (green/red)
                bool this_running = (is_active && bot_running_);
                if (this_running) {
                    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.75f, 0.12f, 0.12f, 1));
                    if (ImGui::SmallButton("Stop##bot")) {
                        server_->send_to(cid, Message(MsgId::BotStop));
                        if (is_active) bot_running_ = false;
                    }
                    ImGui::PopStyleColor();
                } else {
                    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.12f, 0.6f, 0.12f, 1));
                    if (ImGui::SmallButton("Start##bot")) {
                        active_account_ = i;
                        load_bot_config(cname);
                        save_bot_config(cname);
                        auto cfg_path = std::filesystem::path(config_path_).parent_path() / ("bot_" + cname + ".json");
                        Message m(MsgId::BotStart);
                        m.push_string(cfg_path.string());
                        m.push_string(client_path_ + "\\ini\\itemtype.json");
                        server_->send_to(cid, m);
                        if (is_active) bot_running_ = true;
                    }
                    ImGui::PopStyleColor();
                }
                // Close button — gracefully shut down the DLL
                ImGui::SameLine();
                ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.5f, 0.1f, 0.1f, 1));
                if (ImGui::SmallButton("X##close")) {
                    server_->send_to(cid, Message(MsgId::BotStop));
                    server_->send_to(cid, Message(MsgId::Shutdown));
                    add_log(std::format("Shutdown sent to {}", cname));
                    accounts_[i].launched = false;
                    accounts_[i].connected = false;
                    accounts_[i].client_id = 0;
                    accounts_[i].char_name.clear();
                }
                ImGui::PopStyleColor();
            } else if (connected && cid) {
                ImGui::TextColored({1,1,0,0.5f}, "...");
            }

            // Delete account button (only when not connected)
            if (!connected) {
                ImGui::SameLine();
                if (ImGui::SmallButton("-##del")) {
                    delete_idx = i;
                }
            }

            ImGui::PopID();
        }

        // Handle deletion outside the loop
        if (delete_idx >= 0 && delete_idx < (int)accounts_.size()) {
            accounts_.erase(accounts_.begin() + delete_idx);
            if (active_account_ >= (int)accounts_.size()) active_account_ = (int)accounts_.size() - 1;
            save_config();
        }

        // Add account
        ImGui::Separator();
        ImGui::SetNextItemWidth(80);
        ImGui::InputText("##newuser", new_account_user_, sizeof(new_account_user_));
        ImGui::SameLine();
        ImGui::SetNextItemWidth(80);
        ImGui::InputTextWithHint("##newpass", "pass", new_account_pass_, sizeof(new_account_pass_), ImGuiInputTextFlags_Password);
        ImGui::SameLine();
        ImGui::SetNextItemWidth(60);
        ImGui::Combo("##newsrv", &new_account_server_, servers, 3);
        ImGui::SameLine();
        if (ImGui::SmallButton("Add##acc")) {
            if (new_account_user_[0]) {
                AccountProfile ap;
                ap.username = new_account_user_;
                ap.password = new_account_pass_;
                ap.server = new_account_server_;
                accounts_.push_back(ap);
                new_account_user_[0] = 0;
                new_account_pass_[0] = 0;
                save_config();
            }
        }

        // Launch All button
        if (accounts_.size() > 1) {
            ImGui::SameLine();
            if (ImGui::SmallButton("Launch All##launchall")) {
                for (int i = 0; i < (int)accounts_.size(); i++) {
                    if (!accounts_[i].launched && exe_ok && dll_ok) {
                        strncpy_s(login_user_, accounts_[i].username.c_str(), 63);
                        strncpy_s(login_pass_, accounts_[i].password.c_str(), 63);
                        login_server_ = accounts_[i].server;
                        save_config();
                        launch_and_inject();
                        accounts_[i].launched = true;
                        Sleep(2000); // stagger launches
                    }
                }
            }
        }

        if (ImGui::SmallButton("Save##cfg")) { save_config(); }
    }
}

void App::render_hero_panel() {
    if (ImGui::CollapsingHeader("Hero Control", ImGuiTreeNodeFlags_DefaultOpen)) {
        std::lock_guard lock(clients_mutex_);

        auto* active = get_active_client_unlocked();
        uint32_t active_id = active ? active->id : 0;

        if (!active) {
            ImGui::TextDisabled("No client connected");
            return;
        }

        auto& s = active->state;
        auto send = [&](MsgId id) { server_->send_to(active_id, Message(id)); };

        // ── State Display ───────────────────────────────────────────
        if (ImGui::TreeNodeEx("State", ImGuiTreeNodeFlags_DefaultOpen)) {
            ImGui::Text("%s  ID:%u  Lv%d", s.char_name.c_str(), s.char_id, s.level);
            ImGui::Text("Pos: %d, %d   Map: %u   Silver: %lld", s.pos_x, s.pos_y, s.map_id, s.silver);

            // HP (red)
            {
                float f = s.max_hp > 0 ? (float)s.hp / s.max_hp : 0;
                char ov[32]; snprintf(ov, 32, "HP %d/%d", s.hp, s.max_hp);
                ImGui::PushStyleColor(ImGuiCol_PlotHistogram, ImVec4(0.8f, 0.15f, 0.15f, 1));
                ImGui::ProgressBar(f, {-1, 16}, ov);
                ImGui::PopStyleColor();
            }
            // MP (blue)
            {
                float f = s.max_mp > 0 ? (float)s.mp / s.max_mp : 0;
                char ov[32]; snprintf(ov, 32, "MP %d/%d", s.mp, s.max_mp);
                ImGui::PushStyleColor(ImGuiCol_PlotHistogram, ImVec4(0.15f, 0.3f, 0.85f, 1));
                ImGui::ProgressBar(f, {-1, 16}, ov);
                ImGui::PopStyleColor();
            }
            // Stamina (cyan)
            {
                float f = s.max_stamina > 0 ? (float)s.stamina / s.max_stamina : 0;
                char ov[32]; snprintf(ov, 32, "STA %d/%d", s.stamina, s.max_stamina);
                ImGui::PushStyleColor(ImGuiCol_PlotHistogram, ImVec4(0.0f, 0.7f, 0.7f, 1));
                ImGui::ProgressBar(f, {-1, 14}, ov);
                ImGui::PopStyleColor();
            }

            // Flags (compact)
            auto flag = [](const char* label, bool val) {
                if (val) ImGui::TextColored({0.3f,1,0.3f,1}, "%s", label);
                else ImGui::TextColored({0.4f,0.4f,0.4f,1}, "%s", label);
                ImGui::SameLine();
            };
            flag("Dead", s.is_dead());
            if (s.is_dead() && s.revive_countdown <= 0) {
                if (ImGui::SmallButton("Revive")) {
                    server_->send_to(active_id, Message(MsgId::Revive));
                }
                ImGui::SameLine();
            }
            flag("Ghost", s.is_ghost());
            flag("Fly", s.is_flying());
            flag("XP", s.is_xp_full());
            flag("VIP", s.is_vip());
            ImGui::NewLine();
            ImGui::TreePop();
        }

        ImGui::Separator();

        // ── Pathfinding ───────────────────────────────────────────
        if (ImGui::TreeNodeEx("Pathfinding", ImGuiTreeNodeFlags_DefaultOpen)) {
            ImGui::SetNextItemWidth(80); ImGui::InputInt("##px", &path_x_);
            ImGui::SameLine(); ImGui::SetNextItemWidth(80); ImGui::InputInt("##py", &path_y_);
            ImGui::SameLine();
            if (ImGui::Button("Go To", {60, 0})) {
                Message m(MsgId::PathfindTo); m << path_x_ << path_y_;
                server_->send_to(active_id, m);
                add_log(std::format("Pathfind to {},{}", path_x_, path_y_));
            }
            ImGui::SameLine();
            if (ImGui::Button("Stop", {50, 0})) {
                server_->send_to(active_id, Message(MsgId::PathfindStop));
            }
            ImGui::SameLine();
            if (ImGui::Button("Here##path")) {
                path_x_ = s.pos_x; path_y_ = s.pos_y;
            }
            ImGui::SameLine();
            if (ImGui::Button("Dump Map")) {
                server_->send_to(active_id, Message(MsgId::ScanAll));
                add_log("GameMap dump (check DLL console)");
            }
            ImGui::TreePop();
        }
        // ── Scatter Lab ─────────────────────────────────────────────
        if (ImGui::TreeNodeEx("Scatter Lab")) {
            static int scatter_x = 0, scatter_y = 0;
            ImGui::SetNextItemWidth(80); ImGui::InputInt("##sx", &scatter_x);
            ImGui::SameLine(); ImGui::SetNextItemWidth(80); ImGui::InputInt("##sy", &scatter_y);
            ImGui::SameLine();
            if (ImGui::Button("Cast Scatter", {110, 0})) {
                Message m(MsgId::ScatterTest);
                m << scatter_x << scatter_y;
                server_->send_to(active_id, m);
                add_log(std::format("Scatter test at {},{}", scatter_x, scatter_y));
            }
            ImGui::SameLine();
            if (ImGui::Button("Here##scatter")) {
                scatter_x = s.pos_x; scatter_y = s.pos_y;
            }
            ImGui::TextDisabled("Cast scatter at target pos. Check DLL console for hit/miss data.");
            ImGui::TextDisabled("Results saved to scatter_log.json in game directory.");
            ImGui::TreePop();
        }

        // ── Equipment ────────────────────────────────────────────────
        if (ImGui::TreeNodeEx("Equipment", ImGuiTreeNodeFlags_DefaultOpen)) {
            // Get equipment from proxy
            EquipSlot equip_slots[10]{};
            { auto* pc_eq = get_active_proxy(); if (pc_eq) { std::lock_guard slk(pc_eq->state_mutex); std::memcpy(equip_slots, pc_eq->equip_slots, sizeof(equip_slots)); } }
            // Quality color helper: last digit of type_id
            auto quality_color = [](uint32_t type_id) -> ImU32 {
                int q = type_id % 10;
                switch (q) {
                    case 9: return IM_COL32(255, 80, 0, 255);    // Super - orange
                    case 8: return IM_COL32(220, 30, 30, 255);   // Elite - red
                    case 7: return IM_COL32(50, 80, 220, 255);   // Unique - blue
                    case 6: return IM_COL32(40, 160, 220, 255);  // Refined - cyan
                    default: return IM_COL32(0, 0, 0, 0);        // no halo
                }
            };
            auto quality_name = [](uint32_t type_id) -> const char* {
                int q = type_id % 10;
                switch (q) {
                    case 9: return "Super"; case 8: return "Elite";
                    case 7: return "Unique"; case 6: return "Refined";
                    case 5: return "Normal"; default: return "";
                }
            };

            static const char* slot_names[] = {"", "Helmet", "Necklace", "Armor", "R.Weapon",
                "L.Weapon", "Ring", "Gourd", "Boots", "Garment"};
            float icon_sz = 40.0f;
            for (int i = 1; i <= 9; i++) {
                auto& eq = equip_slots[i];
                ImGui::PushID(i + 90000);
                if (eq.type_id) {
                    // Draw quality halo border
                    ImU32 halo = quality_color(eq.type_id);
                    ImVec2 cp = ImGui::GetCursorScreenPos();
                    if (halo & 0xFF000000) {
                        ImGui::GetWindowDrawList()->AddRectFilled(
                            {cp.x - 2, cp.y - 2}, {cp.x + icon_sz + 2, cp.y + icon_sz + 2},
                            halo, 4.0f);
                    }
                    // Draw icon
                    auto* icon = get_item_icon(eq.type_id);
                    if (icon) {
                        ImGui::Image((ImTextureID)icon, {icon_sz, icon_sz});
                    } else {
                        ImGui::Button("##empty", {icon_sz, icon_sz});
                    }
                    ImGui::SameLine();
                    auto nit = item_names_.find(eq.type_id);
                    const char* qn = quality_name(eq.type_id);
                    if (nit != item_names_.end()) {
                        if (eq.plus > 0)
                            ImGui::Text("%s: %s%s (+%d)", slot_names[i], qn, nit->second.c_str(), eq.plus);
                        else
                            ImGui::Text("%s: %s%s", slot_names[i], qn, nit->second.c_str());
                    } else {
                        ImGui::Text("%s: %u", slot_names[i], eq.type_id);
                    }
                } else {
                    ImGui::BeginDisabled();
                    ImGui::Button("--", {icon_sz, icon_sz});
                    ImGui::EndDisabled();
                    ImGui::SameLine();
                    ImGui::TextDisabled("%s: empty", slot_names[i]);
                }
                ImGui::PopID();
            }
            ImGui::TreePop();
        }

        ImGui::Separator();

        // ── Inventory (live) ────────────────────────────────────────
        if (ImGui::TreeNodeEx("Inventory")) {
            std::vector<InvItemInfo> inventory_items;
            { auto* pc_inv = get_active_proxy(); if (pc_inv) { std::lock_guard slk(pc_inv->state_mutex); inventory_items = pc_inv->inventory; } }
            ImGui::Text("%zu items", inventory_items.size());
            ImGui::SameLine();
            if (ImGui::SmallButton("Deposit All (VIP)")) {
                server_->send_to(active_id, Message(MsgId::DepositAll));
            }

            if (!inventory_items.empty()) {
                float icon_sz = 32.0f;
                ImGui::BeginChild("inv_list", ImVec2(0, 200), ImGuiChildFlags_Borders);
                // Grid of icons
                float avail_w = ImGui::GetContentRegionAvail().x;
                int cols = (int)(avail_w / (icon_sz + 8));
                if (cols < 1) cols = 1;
                int col = 0;
                auto quality_color = [](uint32_t tid) -> ImU32 {
                    int q = tid % 10;
                    switch (q) {
                        case 9: return IM_COL32(255, 80, 0, 255);
                        case 8: return IM_COL32(220, 30, 30, 255);
                        case 7: return IM_COL32(50, 80, 220, 255);
                        case 6: return IM_COL32(40, 160, 220, 255);
                        default: return IM_COL32(60, 60, 60, 255);
                    }
                };
                for (const auto& it : inventory_items) {
                    ImGui::PushID((int)it.item_id + 300000);
                    ImVec2 cp = ImGui::GetCursorScreenPos();
                    ImU32 border = quality_color(it.type_id);
                    ImGui::GetWindowDrawList()->AddRectFilled(
                        {cp.x - 1, cp.y - 1}, {cp.x + icon_sz + 1, cp.y + icon_sz + 1},
                        border, 2.0f);

                    auto* icon = get_item_icon(it.type_id);
                    if (icon) {
                        ImGui::Image((ImTextureID)icon, {icon_sz, icon_sz});
                    } else {
                        ImGui::Button("?", {icon_sz, icon_sz});
                    }

                    // Tooltip on hover
                    if (ImGui::IsItemHovered()) {
                        auto db_it = item_names_.find(it.type_id);
                        const char* name = db_it != item_names_.end() ? db_it->second.c_str() : "???";
                        ImGui::SetTooltip("%s\nType: %u  Qty: %d/%d\nID: %u",
                            name, it.type_id, it.amount, it.amount_limit, it.item_id);
                    }

                    // Amount overlay
                    if (it.amount > 1) {
                        char amt[16]; snprintf(amt, 16, "%d", it.amount);
                        ImVec2 tsz = ImGui::CalcTextSize(amt);
                        ImGui::GetWindowDrawList()->AddText(
                            {cp.x + icon_sz - tsz.x, cp.y + icon_sz - tsz.y},
                            IM_COL32(255, 255, 255, 255), amt);
                    }

                    ImGui::PopID();
                    col++;
                    if (col < cols) ImGui::SameLine();
                    else col = 0;
                }
                ImGui::EndChild();
            }
            ImGui::TreePop();
        }

        // ── Cross-Map Travel ────────────────────────────────────────
        if (ImGui::TreeNodeEx("Cross-Map Travel")) {
            if (!crossmap_db_loaded_) {
                if (ImGui::Button("Load Gateway DB")) {
                    // Send the GUI exe directory (where Gateways.json will be stored)
                    char exe_buf[MAX_PATH]{};
                    GetModuleFileNameA(nullptr, exe_buf, MAX_PATH);
                    std::string data_path = std::filesystem::path(exe_buf).parent_path().string();
                    Message m(MsgId::CrossMapInit);
                    m.push_string(data_path);
                    server_->send_to(active_id, m);
                }
            } else {
                ImGui::Text("Current: %s (%u)", s.char_name.empty() ? "???" : "loaded",
                    0u); // map_id not in state yet, but DLL logs it
                ImGui::Text("%zu maps in gateway DB", crossmap_maps_.size());

                if (!crossmap_maps_.empty()) {
                    // Combo box for destination selection
                    if (ImGui::BeginCombo("Destination",
                        crossmap_selected_ < (int)crossmap_maps_.size()
                            ? std::format("{} ({})", crossmap_maps_[crossmap_selected_].name,
                                crossmap_maps_[crossmap_selected_].id).c_str()
                            : "Select...")) {
                        for (int i = 0; i < (int)crossmap_maps_.size(); i++) {
                            auto& e = crossmap_maps_[i];
                            bool selected = (crossmap_selected_ == i);
                            auto label = std::format("{} ({})", e.name, e.id);
                            if (ImGui::Selectable(label.c_str(), selected))
                                crossmap_selected_ = i;
                            if (selected) ImGui::SetItemDefaultFocus();
                        }
                        ImGui::EndCombo();
                    }

                    ImGui::SameLine();
                    if (ImGui::Button("Travel")) {
                        if (crossmap_selected_ < (int)crossmap_maps_.size()) {
                            Message m(MsgId::CrossMapTravel);
                            uint32_t dest = crossmap_maps_[crossmap_selected_].id;
                            m << dest;
                            server_->send_to(active_id, m);
                            add_log(std::format("Cross-map travel to {} ({})",
                                crossmap_maps_[crossmap_selected_].name, dest));
                        }
                    }
                    ImGui::SameLine();
                    if (ImGui::Button("Stop##crossmap")) {
                        server_->send_to(active_id, Message(MsgId::CrossMapStop));
                        add_log("Cross-map travel stopped");
                    }
                }
            }
            ImGui::TreePop();
        }

        // ── Map Portals ─────────────────────────────────────────────
        if (ImGui::TreeNodeEx("Map Portals")) {
            if (ImGui::Button("Load Portals")) {
                server_->send_to(active_id, Message(MsgId::PortalList));
            }
            ImGui::SameLine();
            ImGui::Text("Map %u (%zu portals)", portal_map_id_, portals_.size());

            if (ImGui::Button("Explore All Unknown")) {
                server_->send_to(active_id, Message(MsgId::ExploreAll));
                add_log("Started exploring all unknown portals");
            }
            ImGui::SameLine();
            if (ImGui::Button("Stop Explore")) {
                server_->send_to(active_id, Message(MsgId::StopExploreAll));
                add_log("Stopped explore-all");
            }

            for (size_t i = 0; i < portals_.size(); i++) {
                auto& p = portals_[i];
                ImGui::PushID(static_cast<int>(i) + 400000);
                if (ImGui::Button("Go##portal", ImVec2(30, 0))) {
                    Message m(MsgId::PortalGo);
                    int32_t idx = static_cast<int32_t>(i);
                    m << idx;
                    server_->send_to(active_id, m);
                    add_log(std::format("Navigate to portal: {} ({},{})", p.name, p.x, p.y));
                }
                ImGui::SameLine();
                if (ImGui::Button("Explore##portal", ImVec2(55, 0))) {
                    Message m(MsgId::PortalExplore);
                    int32_t idx = static_cast<int32_t>(i);
                    m << idx;
                    server_->send_to(active_id, m);
                    add_log(std::format("Exploring portal: {} ({},{})", p.name, p.x, p.y));
                }
                ImGui::SameLine();
                if (p.dest_map)
                    ImGui::Text("%s (%d,%d) -> map %u (%d,%d)", p.name.c_str(), p.x, p.y, p.dest_map, p.dest_x, p.dest_y);
                else
                    ImGui::Text("%s (%d,%d) [unknown dest]", p.name.c_str(), p.x, p.y);
                ImGui::PopID();
            }
            ImGui::TreePop();
        }

        // ── NPC Dialog ─────────────────────────────────────────────
        if (ImGui::TreeNodeEx("NPC Dialog")) {
            if (ImGui::Button("Read Dialog")) {
                server_->send_to(active_id, Message(MsgId::DialogOptions));
            }
            ImGui::SameLine();
            ImGui::Text("(%zu options)", dialog_options_.size());

            if (dialog_open_) {
                ImGui::TextColored(ImVec4(0.2f, 1.0f, 0.2f, 1.0f), "Dialog OPEN");
            } else {
                ImGui::TextDisabled("No dialog");
            }

            for (size_t i = 0; i < dialog_options_.size(); i++) {
                auto& opt = dialog_options_[i];
                const char* type_str = opt.type == 1 ? "Button" : opt.type == 2 ? "Edit" : "???";
                ImGui::PushID(static_cast<int>(i) + 300000);
                if (ImGui::Button(opt.text.empty() ? "(empty)" : opt.text.c_str())) {
                    Message m(MsgId::DialogAnswer);
                    int32_t oid = opt.id;
                    m << oid;
                    server_->send_to(active_id, m);
                    add_log(std::format("Dialog answer: id={} text=\"{}\"", opt.id, opt.text));
                }
                ImGui::SameLine();
                ImGui::TextDisabled("[%s id=%d]", type_str, opt.id);
                ImGui::PopID();
            }
            ImGui::TreePop();
        }

    }
}

void App::render_nearby_panel() {
    {
        // Copy data from proxy, render outside lock
        std::vector<NearbyEntity> nearby_copy;
        std::vector<GroundItemInfo> items_copy;
        uint32_t active_id = 0;
        int hero_x = 0, hero_y = 0;
        bool has_active = false;
        {
            auto* pc = get_active_proxy();
            if (pc) {
                std::lock_guard slk(pc->state_mutex);
                active_id = pc->dll_client_id;
                hero_x = pc->hero_state.pos_x;
                hero_y = pc->hero_state.pos_y;
                nearby_copy = pc->entities;
                items_copy = pc->ground_items;
                has_active = (pc->hero_id != 0);
            }
            if (!has_active) {
                std::lock_guard lock(clients_mutex_);
                auto* _ac = get_active_client_unlocked();
                if (_ac) {
                    active_id = _ac->id;
                    hero_x = _ac->state.pos_x;
                    hero_y = _ac->state.pos_y;
                    nearby_copy = _ac->nearby;
                    items_copy = _ac->ground_items;
                    has_active = true;
                }
            }
        }

        if (!has_active) {
            ImGui::TextDisabled("No client connected");
        }
        else {

        ImGui::Text("%zu entities, %zu ground items", nearby_copy.size(), items_copy.size());

        if (!nearby_copy.empty()) {

            ImGui::BeginChild("nearby_list", ImVec2(0, 200), ImGuiChildFlags_Borders);
            if (ImGui::BeginTable("nearby", 8, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg)) {
            ImGui::TableSetupColumn("ID", ImGuiTableColumnFlags_WidthFixed, 70);
            ImGui::TableSetupColumn("Name");
            ImGui::TableSetupColumn("Pos", ImGuiTableColumnFlags_WidthFixed, 80);
            ImGui::TableSetupColumn("Kind", ImGuiTableColumnFlags_WidthFixed, 55);
            ImGui::TableSetupColumn("Look", ImGuiTableColumnFlags_WidthFixed, 45);
            ImGui::TableSetupColumn("Dist", ImGuiTableColumnFlags_WidthFixed, 40);
            ImGui::TableSetupColumn("Status", ImGuiTableColumnFlags_WidthFixed, 45);
            ImGui::TableSetupColumn("##actions", ImGuiTableColumnFlags_WidthFixed, 90);
            ImGui::TableHeadersRow();

            for (const auto& e : nearby_copy) {
                double dist = std::sqrt(static_cast<double>(
                    (e.x - hero_x) * (e.x - hero_x) + (e.y - hero_y) * (e.y - hero_y)));

                const char* type_str = "?";
                if (e.id >= 400000 && e.id < 500000) {
                    if (strstr(e.name, "Guard") || strstr(e.name, "Patrol"))
                        type_str = "Guard";
                    else
                        type_str = "Monster";
                }
                else if (e.id >= 1 && e.id < 400000) {
                    switch (e.npc_sort) {
                        case 1:  type_str = "Shop"; break;
                        case 2:  type_str = "Task"; break;
                        case 3:  type_str = "Storage"; break;
                        case 6:  type_str = "Forge"; break;
                        default: type_str = "NPC"; break;
                    }
                }
                else if (e.id >= 900000 && e.id < 1000000) type_str = "Pet";
                else if (e.id >= 1000000)               type_str = "Player";
                else if (e.role_kind == 8)              type_str = "Booth";
                else                                    type_str = "Other";

                ImGui::TableNextRow();
                ImGui::TableNextColumn(); ImGui::Text("%u", e.id);
                ImGui::TableNextColumn(); ImGui::TextUnformatted(e.name);
                ImGui::TableNextColumn(); ImGui::Text("%d,%d", e.x, e.y);
                ImGui::TableNextColumn(); ImGui::Text("%s", type_str);
                ImGui::TableNextColumn(); ImGui::Text("%u", e.look_type);
                ImGui::TableNextColumn(); ImGui::Text("%.0f", dist);
                ImGui::TableNextColumn();
                if (e.dead)
                    ImGui::TextColored({1,0.3f,0.3f,1}, "Dead");
                else
                    ImGui::TextColored({0.3f,1,0.3f,1}, "Alive");

                ImGui::TableNextColumn();
                ImGui::PushID(static_cast<int>(e.id));
                if (ImGui::SmallButton("Atk")) {
                    Message m(MsgId::Attack);
                    m << e.id;
                    server_->send_to(active_id, m);
                    add_log(std::format("Attack {} ({})", e.name, e.id));
                }
                ImGui::SameLine();
                if (ImGui::SmallButton("GoTo")) {
                    Message m(MsgId::Jump);
                    m << e.x << e.y;
                    server_->send_to(active_id, m);
                }
                if (e.id < 400000) { // NPC range
                    ImGui::SameLine();
                    if (ImGui::SmallButton("Talk")) {
                        Message m(MsgId::ActivateNpc);
                        m << e.id;
                        server_->send_to(active_id, m);
                        add_log(std::format("Talk to {} ({})", e.name, e.id));
                    }
                }
                ImGui::PopID();
            }
            ImGui::EndTable();
            } // if BeginTable nearby
            ImGui::EndChild();
        }

        // Ground items
        if (!items_copy.empty()) {
            ImGui::Separator();
            ImGui::Text("Ground Items (%zu):", items_copy.size());
            ImGui::BeginChild("item_list", ImVec2(0, 150), ImGuiChildFlags_Borders);
            if (ImGui::BeginTable("items", 6, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg)) {
            ImGui::TableSetupColumn("Name");
            ImGui::TableSetupColumn("Type", ImGuiTableColumnFlags_WidthFixed, 80);
            ImGui::TableSetupColumn("Pos", ImGuiTableColumnFlags_WidthFixed, 80);
            ImGui::TableSetupColumn("Dist", ImGuiTableColumnFlags_WidthFixed, 40);
            ImGui::TableSetupColumn("##pick", ImGuiTableColumnFlags_WidthFixed, 50);
            ImGui::TableSetupColumn("##goto", ImGuiTableColumnFlags_WidthFixed, 50);
            ImGui::TableHeadersRow();

            for (const auto& it : items_copy) {
                double dist = std::sqrt(static_cast<double>(
                    (it.x - hero_x) * (it.x - hero_x) + (it.y - hero_y) * (it.y - hero_y)));

                // Resolve name from item database
                auto name_it = item_names_.find(it.type_id);
                const char* name = (name_it != item_names_.end()) ? name_it->second.c_str() : "???";

                ImGui::TableNextRow();
                ImGui::TableNextColumn(); ImGui::Text("%s", name);
                ImGui::TableNextColumn(); ImGui::Text("%u", it.type_id);
                ImGui::TableNextColumn(); ImGui::Text("%d,%d", it.x, it.y);
                ImGui::TableNextColumn(); ImGui::Text("%.0f", dist);
                ImGui::TableNextColumn();
                ImGui::PushID(static_cast<int>(it.item_id) + 100000);
                if (ImGui::SmallButton("Pick")) {
                    Message m(MsgId::PickUp);
                    m << it.item_id << it.x << it.y;
                    server_->send_to(active_id, m);
                    add_log(std::format("PickUp {} at {},{}", it.item_id, it.x, it.y));
                }
                ImGui::TableNextColumn();
                if (ImGui::SmallButton("Go")) {
                    Message m(MsgId::Jump);
                    m << it.x << it.y;
                    server_->send_to(active_id, m);
                }
                ImGui::PopID();
            }
            ImGui::EndTable();
            } // if BeginTable items
            ImGui::EndChild();
        }
        } // else active
    }
}

void App::render_bot_panel() {
    if (ImGui::CollapsingHeader("Bot")) {
        // Find active client
        uint32_t active_id = 0;
        {
            std::lock_guard lock(clients_mutex_);
            auto* _ac = get_active_client_unlocked();
            if (_ac) active_id = _ac->id;
        }

        if (!active_id) {
            ImGui::TextDisabled("No client connected");
            return;
        }

        // Start / Stop button
        if (bot_running_) {
            if (ImGui::Button("Stop Bot", ImVec2(120, 30))) {
                Message m(MsgId::BotStop);
                server_->send_to(active_id, m);
                bot_running_ = false;
                add_log("Bot stopped");
            }
            ImGui::SameLine();
            ImGui::TextColored({0.3f, 1.0f, 0.3f, 1.0f}, "RUNNING");
        } else {
            if (ImGui::Button("Start Bot", ImVec2(120, 30))) {
                // Save config to file, send DLL just the path
                save_bot_config(bot_char_name_);
                auto cfg_path = std::filesystem::path(config_path_).parent_path() / ("bot_" + bot_char_name_ + ".json");
                Message m(MsgId::BotStart);
                m.push_string(cfg_path.string());
                // Also send the itemtype.json path for potion DB
                auto item_path = client_path_ + "\\ini\\itemtype.json";
                m.push_string(item_path);
                server_->send_to(active_id, m);
                bot_running_ = true;
                add_log(std::format("Bot started (config: {})", cfg_path.string()));
            }
            ImGui::SameLine();
            ImGui::TextColored({0.6f, 0.6f, 0.6f, 1.0f}, "STOPPED");
        }

        // ── Hunt Mode ─────────────────────────────────────────────
        ImGui::Separator();
        ImGui::Text("Hunt Mode:");
        ImGui::RadioButton("Melee##hm", &bot_hunt_mode_, 0); ImGui::SameLine();
        ImGui::RadioButton("Ranged##hm", &bot_hunt_mode_, 1); ImGui::SameLine();
        ImGui::RadioButton("Scatter##hm", &bot_hunt_mode_, 2); ImGui::SameLine();
        ImGui::RadioButton("Magic##hm", &bot_hunt_mode_, 3);

        // ── Common Settings ──────────────────────────────────────
        if (ImGui::TreeNodeEx("General", ImGuiTreeNodeFlags_DefaultOpen)) {
            ImGui::Checkbox("Auto HP Pot##ahp", &bot_auto_hp_pot_);
            if (bot_auto_hp_pot_) {
                ImGui::SameLine(); ImGui::SetNextItemWidth(120);
                ImGui::SliderFloat("##hppct", &bot_hp_pct_, 0.10f, 1.00f, "HP %.0f%%");
            }
            ImGui::Checkbox("Auto MP Pot##amp", &bot_auto_mp_pot_);
            if (bot_auto_mp_pot_) {
                ImGui::SameLine(); ImGui::SetNextItemWidth(120);
                ImGui::SliderFloat("##mppct", &bot_mp_pct_, 0.10f, 1.00f, "MP %.0f%%");
            }
            ImGui::SetNextItemWidth(120);
            ImGui::InputInt("Potion Pickup Threshold", &bot_potion_threshold_);
            ImGui::Checkbox("Pickup HP Pots##php", &bot_pickup_hp_pots_);
            ImGui::SameLine();
            ImGui::Checkbox("Pickup MP Pots##pmp", &bot_pickup_mp_pots_);
            ImGui::SetNextItemWidth(200);
            ImGui::SliderInt("Attack Cooldown (ms)", &bot_attack_cd_, 100, 3000);
            ImGui::SetNextItemWidth(200);
            ImGui::SliderInt("Pickup Cooldown (ms)", &bot_pickup_cd_, 100, 2000);
            ImGui::SetNextItemWidth(200);
            ImGui::SliderInt("Action Range", &bot_action_range_, 1, 10);

            ImGui::Checkbox("Loot +1 Items", &bot_loot_plus_);
            ImGui::SameLine();
            const char* quality_labels[] = {"Off", "Normal(5)+", "Refined(6)+", "Unique(7)+", "Elite(8)+", "Super(9)"};
            int quality_idx = 0;
            if (bot_loot_min_quality_ >= 9) quality_idx = 5;
            else if (bot_loot_min_quality_ >= 8) quality_idx = 4;
            else if (bot_loot_min_quality_ >= 7) quality_idx = 3;
            else if (bot_loot_min_quality_ >= 6) quality_idx = 2;
            else if (bot_loot_min_quality_ >= 5) quality_idx = 1;
            ImGui::SetNextItemWidth(140);
            if (ImGui::Combo("Loot Quality##lq", &quality_idx, quality_labels, 6)) {
                const int qv[] = {0, 5, 6, 7, 8, 9};
                bot_loot_min_quality_ = qv[quality_idx];
            }
            ImGui::TreePop();
        }

        // ── XP Skill ─────────────────────────────────────────────
        if (ImGui::TreeNode("XP Skill")) {
            ImGui::Checkbox("Use XP Skill", &bot_use_xp_skill_);
            if (bot_use_xp_skill_) {
                ImGui::SetNextItemWidth(120);
                ImGui::InputInt("Magic ID##xp", &bot_xp_skill_id_);
                ImGui::SameLine(); ImGui::TextDisabled("1110=Cyclone 1120=Fly");
                ImGui::SetNextItemWidth(200);
                ImGui::SliderInt("Cast Delay##xp", &bot_xp_skill_delay_, 100, 2000);
                ImGui::SetNextItemWidth(200);
                ImGui::SliderInt("Atk CD during XP##xp", &bot_attack_cd_xp_, 50, 1000);
            }
            ImGui::TreePop();
        }

        // ── Auto-Deposit ─────────────────────────────────────────
        if (ImGui::TreeNode("Auto-Deposit")) {
            ImGui::Checkbox("Auto Deposit When Full", &bot_auto_deposit_);
            if (bot_auto_deposit_) {
                ImGui::SetNextItemWidth(120);
                ImGui::SliderInt("At # items##dep", &bot_deposit_threshold_, 20, 40);
            }
            if (ImGui::SmallButton("Deposit Now##dep")) {
                server_->send_to(active_id, Message(MsgId::DepositNow));
            }
            ImGui::SameLine();
            if (ImGui::SmallButton("Stop##dep")) {
                server_->send_to(active_id, Message(MsgId::StopDeposit));
            }
            ImGui::TreePop();
        }

        // ── Player Safety ────────────────────────────────────────
        if (ImGui::TreeNode("Player Safety")) {
            ImGui::Checkbox("Enable Player Avoidance", &bot_safety_enabled_);
            if (bot_safety_enabled_) {
                ImGui::SetNextItemWidth(200);
                ImGui::SliderInt("Flee Distance##sf", &bot_safety_flee_distance_, 3, 12);
                ImGui::SetNextItemWidth(200);
                ImGui::SliderInt("Max Encounters##sf", &bot_safety_max_encounters_, 1, 10);
                ImGui::SetNextItemWidth(200);
                ImGui::SliderInt("Encounter Window (sec)##sf", &bot_safety_encounter_window_, 30, 600);

                ImGui::Separator();
                ImGui::Text("On Max Encounters:");
                ImGui::RadioButton("Flee Only##pa", &bot_safety_player_action_, 0); ImGui::SameLine();
                ImGui::RadioButton("Go to Safe Spot##pa", &bot_safety_player_action_, 1); ImGui::SameLine();
                ImGui::RadioButton("Disconnect##pa", &bot_safety_player_action_, 2);

                if (bot_safety_player_action_ == 1) {
                    ImGui::TextDisabled("Safe spot (travels there, waits, returns to hunt)");
                    ImGui::SetNextItemWidth(80);
                    ImGui::InputInt("Safe X##sf", &bot_safety_safe_x_); ImGui::SameLine();
                    ImGui::SetNextItemWidth(80);
                    ImGui::InputInt("Safe Y##sf", &bot_safety_safe_y_); ImGui::SameLine();
                    ImGui::SetNextItemWidth(100);
                    ImGui::InputInt("Map ID##sf", &bot_safety_safe_map_);
                }

                if (bot_safety_player_action_ > 0) {
                    ImGui::SetNextItemWidth(200);
                    ImGui::SliderInt("Wait Min (sec)##sf", &bot_safety_wait_min_, 10, 600);
                    ImGui::SetNextItemWidth(200);
                    ImGui::SliderInt("Wait Max (sec)##sf", &bot_safety_wait_max_, bot_safety_wait_min_, 1800);
                    if (bot_safety_player_action_ == 2)
                        ImGui::TextDisabled("Auto-reconnects and resumes hunting after wait");
                }

                ImGui::Separator();
                ImGui::TextColored({1,0.3f,0.3f,1}, "GM/PM Detection");
                ImGui::Checkbox("Detect GM/PM##gm", &bot_safety_gm_detect_);
                if (bot_safety_gm_detect_) {
                    ImGui::Text("GM Action:");
                    ImGui::RadioButton("Disconnect##gm", &bot_safety_gm_action_, 0); ImGui::SameLine();
                    ImGui::RadioButton("Safe Spot + Wait##gm", &bot_safety_gm_action_, 1);
                    ImGui::Checkbox("Play Alert Sound##gm", &bot_safety_gm_sound_);
                    ImGui::SetNextItemWidth(200);
                    ImGui::SliderInt("GM Wait Min (sec)##gm", &bot_safety_gm_wait_min_, 60, 1800);
                    ImGui::SetNextItemWidth(200);
                    ImGui::SliderInt("GM Wait Max (sec)##gm", &bot_safety_gm_wait_max_, bot_safety_gm_wait_min_, 3600);
                    ImGui::TextDisabled("IDs < 1000020 or names with [PM]/[GM]/[Admin]");
                    ImGui::TextDisabled("Auto-reconnects and resumes hunting after wait");
                }
            }
            ImGui::TreePop();
        }

        // ── Monster Filter ───────────────────────────────────────
        if (ImGui::TreeNode("Monster Filter")) {
            ImGui::RadioButton("Hunt All##mf", &bot_monster_filter_mode_, 0); ImGui::SameLine();
            ImGui::RadioButton("Blacklist##mf", &bot_monster_filter_mode_, 1); ImGui::SameLine();
            ImGui::RadioButton("Whitelist##mf", &bot_monster_filter_mode_, 2);
            if (bot_monster_filter_mode_ != 0) {
                ImGui::SetNextItemWidth(150);
                ImGui::InputText("##mfname", bot_monster_name_buf_, sizeof(bot_monster_name_buf_));
                ImGui::SameLine();
                if (ImGui::SmallButton("Add##mf")) {
                    std::string name = bot_monster_name_buf_;
                    if (!name.empty()) {
                        bool dup = false;
                        for (auto& n : bot_monster_filter_names_) if (n == name) { dup = true; break; }
                        if (!dup) bot_monster_filter_names_.push_back(name);
                        bot_monster_name_buf_[0] = 0;
                    }
                }
                for (int i = 0; i < (int)bot_monster_filter_names_.size(); i++) {
                    ImGui::BulletText("%s", bot_monster_filter_names_[i].c_str());
                    ImGui::SameLine();
                    char lbl[32]; snprintf(lbl, sizeof(lbl), "X##mf%d", i);
                    if (ImGui::SmallButton(lbl)) { bot_monster_filter_names_.erase(bot_monster_filter_names_.begin() + i); i--; }
                }
            }
            ImGui::TreePop();
        }

        // ── Mode-Specific Settings ───────────────────────────────
        ImGui::Separator();
        if (bot_hunt_mode_ == 0) {
            // Melee settings
            ImGui::TextColored({1,0.8f,0.3f,1}, "Melee Settings");
            ImGui::Checkbox("Prefer Clusters (AoE)", &bot_prefer_clusters_);
            if (bot_prefer_clusters_) {
                ImGui::SetNextItemWidth(200);
                ImGui::SliderInt("Cluster Radius##cl", &bot_cluster_radius_, 1, 8);
                ImGui::SetNextItemWidth(200);
                ImGui::SliderInt("Min Stay Count##cl", &bot_cluster_min_stay_, 1, 10);
            }
            ImGui::SetNextItemWidth(200);
            ImGui::SliderInt("Jump Scatter##ml", &bot_jump_scatter_, 0, 5);
        }
        else if (bot_hunt_mode_ == 1) {
            // Ranged settings
            ImGui::TextColored({0.4f,0.8f,1,1}, "Ranged Settings");
            ImGui::Checkbox("Kite (maintain distance)", &bot_kite_enabled_);
            ImGui::SetNextItemWidth(200);
            ImGui::SliderInt("Min Safe Distance##rng", &bot_kite_min_dist_, 2, 12);
            ImGui::SetNextItemWidth(200);
            ImGui::SliderInt("Jump Scatter##rng", &bot_jump_scatter_, 0, 5);
        }
        else if (bot_hunt_mode_ == 2) {
            // Scatter settings
            ImGui::TextColored({0.5f,1,0.5f,1}, "Scatter Settings");
            ImGui::SetNextItemWidth(120);
            ImGui::InputInt("Scatter Magic ID##sc", &bot_scatter_magic_id_);
            ImGui::SetNextItemWidth(200);
            ImGui::SliderInt("Cooldown (ms)##sc", &bot_scatter_cooldown_, 300, 5000);
            ImGui::SetNextItemWidth(200);
            ImGui::SliderFloat("Range##sc", &bot_scatter_range_, 3.0f, 20.0f, "%.1f");
            ImGui::SetNextItemWidth(200);
            ImGui::SliderFloat("Half Angle (deg)##sc", &bot_scatter_half_angle_, 30.0f, 180.0f, "%.0f");
            ImGui::SetNextItemWidth(200);
            ImGui::SliderInt("Min Targets##sc", &bot_scatter_min_targets_, 1, 10);
            ImGui::SetNextItemWidth(200);
            ImGui::SliderInt("Explore Radius##sc", &bot_explore_radius_, 20, 200);
            ImGui::Checkbox("Predict Hits (skip killed)##sc", &bot_scatter_predict_hits_);
            ImGui::SameLine();
            ImGui::TextDisabled("(blacklist hit targets to keep moving)");
            // Scatter mode forces these:
            bot_use_scatter_ = true;
            bot_kite_enabled_ = false;
        }
        else if (bot_hunt_mode_ == 3) {
            // Magic settings
            ImGui::TextColored({0.8f,0.4f,1,1}, "Magic Settings");
            ImGui::SetNextItemWidth(120);
            ImGui::InputInt("Magic ID##mg", &bot_magic_id_);
            ImGui::SameLine();
            ImGui::TextDisabled("(1000=Thunder, 1001=Fire, 1002=Tornado)");
            ImGui::SetNextItemWidth(200);
            ImGui::SliderInt("Cast Delay (ms)##mg", &bot_magic_cast_delay_, 100, 3000);
            ImGui::SetNextItemWidth(200);
            ImGui::SliderInt("Safe Distance##mg", &bot_magic_safe_dist_, 1, 10);
            ImGui::SameLine();
            ImGui::TextDisabled("(kite if monster closer)");
            // Force MP pot on
            bot_auto_mp_pot_ = true;
        }

        // Pickup types list
        ImGui::Separator();
        ImGui::Text("Pickup Types (%zu):", bot_pickup_types_.size());

        // Search by name with autocomplete dropdown
        ImGui::SetNextItemWidth(200);
        ImGui::InputText("##pickup_search", bot_pickup_type_buf_, sizeof(bot_pickup_type_buf_));
        ImGui::SameLine();
        ImGui::TextDisabled("(search by name or ID)");

        // Show matching items as a list when there's input
        if (bot_pickup_type_buf_[0] != '\0') {
            std::string query(bot_pickup_type_buf_);
            // Convert query to lowercase for case-insensitive search
            std::string query_lower = query;
            for (auto& c : query_lower) c = static_cast<char>(std::tolower(c));

            // Check if it's a numeric ID
            bool is_numeric = true;
            for (char c : query) { if (!std::isdigit(c)) { is_numeric = false; break; } }

            if (is_numeric) {
                uint32_t val = static_cast<uint32_t>(std::atoi(bot_pickup_type_buf_));
                if (val > 0) {
                    auto name_it = item_names_.find(val);
                    std::string label = name_it != item_names_.end()
                        ? std::format("Add: {} ({})", val, name_it->second)
                        : std::format("Add: {} (???)", val);
                    if (ImGui::Button(label.c_str())) {
                        bool exists = false;
                        for (auto t : bot_pickup_types_) { if (t == val) { exists = true; break; } }
                        if (!exists) {
                            bot_pickup_types_.push_back(val);
                            add_log(std::format("Added pickup: {} ({})", val,
                                name_it != item_names_.end() ? name_it->second : "???"));
                        }
                    }
                }
            } else {
                // Check for quoted exact match: "ItemName"
                bool exact_match = false;
                std::string search_term = query_lower;
                if (query.size() >= 2 && query.front() == '"' && query.back() == '"') {
                    exact_match = true;
                    search_term = query.substr(1, query.size() - 2);
                    for (auto& c : search_term) c = static_cast<char>(std::tolower(c));
                }

                // Search by name - show up to 20 matches
                ImGui::BeginChild("##pickup_results", ImVec2(350, 200), ImGuiChildFlags_Borders);
                int shown = 0;
                for (auto& [id, name] : item_names_) {
                    if (shown >= 20) break;
                    std::string name_lower = name;
                    for (auto& c : name_lower) c = static_cast<char>(std::tolower(c));

                    bool matches = exact_match
                        ? (name_lower == search_term)
                        : (name_lower.find(search_term) != std::string::npos);
                    if (!matches) continue;

                    ImGui::PushID(static_cast<int>(id));
                    if (ImGui::Selectable(std::format("{} ({})", name, id).c_str())) {
                        bool exists = false;
                        for (auto t : bot_pickup_types_) { if (t == id) { exists = true; break; } }
                        if (!exists) {
                            bot_pickup_types_.push_back(id);
                            add_log(std::format("Added pickup: {} ({})", id, name));
                        }
                        // Don't clear search - user can add more items with similar names
                    }
                    ImGui::PopID();
                    shown++;
                }
                if (shown == 0) ImGui::TextDisabled("No matches");
                ImGui::EndChild();
            }
        }

        // Show current pickup types with remove buttons
        int remove_idx = -1;
        for (int i = 0; i < static_cast<int>(bot_pickup_types_.size()); i++) {
            ImGui::PushID(i + 300000);
            auto name_it = item_names_.find(bot_pickup_types_[i]);
            const char* name = (name_it != item_names_.end()) ? name_it->second.c_str() : "???";
            ImGui::Text("%u (%s)", bot_pickup_types_[i], name);
            ImGui::SameLine();
            if (ImGui::SmallButton("X")) {
                remove_idx = i;
            }
            ImGui::PopID();
        }
        if (remove_idx >= 0) {
            bot_pickup_types_.erase(bot_pickup_types_.begin() + remove_idx);
        }
    }
}

void App::render_frame() {
    ImGui_ImplDX11_NewFrame();
    ImGui_ImplWin32_NewFrame();
    ImGui::NewFrame();

    render_ui();

    ImGui::Render();

    const float clear_color[] = {0.1f, 0.1f, 0.1f, 1.0f};
    device_context_->OMSetRenderTargets(1, &rtv_, nullptr);
    device_context_->ClearRenderTargetView(rtv_, clear_color);
    ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());

    swap_chain_->Present(1, 0);
}
