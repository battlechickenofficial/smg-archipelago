// SPDX-License-Identifier: GPL-3.0-only
// Super Mario Galaxy x Archipelago: the Nebula plugin.
//
// The plugin is a bridge. It holds no randomizer data of its own and changes nothing until an Archipelago client
// tells it what to do: which galaxies are locked and what opens them, which star select entry plays which mission,
// what each received item does. Those rules come from the multiworld's slot data, through the client, and are kept
// on disk per seed so that the game behaves the same while the client is away. With no seed ever received, or with
// the mod's "Archipelago" option off, every hook passes straight through to the game.
//
// The client listens on 127.0.0.1 (port from the mod's options); the plugin connects to it and they exchange
// tab-separated lines. See apworld/smg/client.py for the protocol.

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

#include <nebula_mod_api.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <map>
#include <memory>
#include <set>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace {

const char* const kVersion = "0.2.3";

const NebulaModApi* game = nullptr;
const NebulaPlugin* me = nullptr;

void log(const std::string& text) {
    if (game != nullptr && game->log != nullptr) game->log(me, ("[archipelago] " + text).c_str());
}

std::vector<std::string> split(const std::string& text, char by) {
    std::vector<std::string> parts;
    std::string part;
    std::istringstream in(text);
    while (std::getline(in, part, by)) parts.push_back(part);
    if (!text.empty() && text.back() == by) parts.emplace_back();
    return parts;
}

std::vector<std::string> list(const std::string& csv) {
    if (csv.empty() || csv == "-") return {};
    return split(csv, ',');
}

int to_int(const std::string& text, int otherwise = 0) {
    char* end = nullptr;
    const long value = std::strtol(text.c_str(), &end, 10);
    return end != text.c_str() ? static_cast<int>(value) : otherwise;
}

using Star = std::pair<std::string, int>;  // (galaxy, star number)

// ------------------------------------------------------------------------------------------------ the rules

// When a game flag is on, as the multiworld says. Everything listed must hold.
struct Rule {
    bool enforce = false;              // a galaxy: leave it if entered while this is off
    int min_stars = 0;                 // power stars owned
    std::vector<std::string> items;    // tokens received (galaxy unlocks, "@luigi")
    std::vector<std::string> flags;    // other game flags on (the game's own answer)
    std::vector<Star> stars;           // stars owned
};

struct Config {
    std::map<std::string, Rule> rules;
    bool has_luigi_rule = false;
    Rule luigi_rule;
    std::map<Star, Star> redirects;    // star select entry -> the mission it plays
    std::set<Star> keep;               // stars whose ownership stays where they are played
    bool messages = true;
    bool death_link = false;
    std::set<int> persist;             // power-ups that carry over between levels
    bool persist_items_only = false;   // only those received as items
    std::set<int> untimed;             // power-ups whose timer is refused
    std::map<std::string, float> constants;  // the player table, for both characters
    bool skip_cutscenes = false;              // answer Nebula's own cutscene skip with Yes, unasked
    std::set<std::string> skip_never_movies;  // movies never skipped (the ending)
    std::set<std::string> skip_never_stages;  // stages where nothing is skipped (where the ending plays)
    bool skip_prologue = false;               // close the new file's story time (picture book, Peach's letter)
    bool spin_from_start = false;             // the spin allowed before the story gives it
    struct PoolEntry {
        std::string object;  // the name the game makes it by
        int count;           // how many each stage keeps ready
        std::string cls;     // its class, for its after-placement setup
        std::string reset;   // how a defeated one comes back: "appear", "none", or a member to call (see the world's data)
        // How it's put out, step by step, each a name and its values (see the world's data): at_player, upright,
        // ground=<offset>, f32=<offset>=<value>, i32=<offset>=<value>, reappear, no_rail, warp=<near>=<far>.
        std::vector<std::vector<std::string>> place;
    };
    std::vector<PoolEntry> enemy_pool;  // trap enemies each stage keeps ready
    std::vector<std::pair<std::string, std::string>> texts;  // the game's messages by id, in place of its own
};

// Read by hooks on any thread; written on the game's thread. A new config is built aside and swapped in.
SRWLOCK g_lock = SRWLOCK_INIT;
std::shared_ptr<const Config> g_config;
std::set<std::string> g_tokens;
bool g_unlock_all = false;
bool g_enabled = true;

struct Shared {
    explicit Shared(SRWLOCK* lock) : lock(lock) { AcquireSRWLockShared(lock); }
    ~Shared() { ReleaseSRWLockShared(lock); }
    SRWLOCK* lock;
};
struct Exclusive {
    explicit Exclusive(SRWLOCK* lock) : lock(lock) { AcquireSRWLockExclusive(lock); }
    ~Exclusive() { ReleaseSRWLockExclusive(lock); }
    SRWLOCK* lock;
};

std::shared_ptr<const Config> config() {
    Shared hold(&g_lock);
    return g_enabled ? g_config : nullptr;
}

bool has_token(const std::string& token) {
    Shared hold(&g_lock);
    return g_unlock_all || g_tokens.count(token) != 0;
}

bool unlocked_all() {
    Shared hold(&g_lock);
    return g_unlock_all;
}

// `flag_on` asks the game for a flag without the plugin's rules in the way.
template <typename FlagOn>
bool rule_holds(const Rule& rule, FlagOn flag_on) {
    if (unlocked_all()) return true;
    for (const std::string& item : rule.items) {
        if (!has_token(item)) return false;
    }
    for (const std::string& flag : rule.flags) {
        if (!flag_on(flag.c_str())) return false;
    }
    for (const Star& star : rule.stars) {
        if (game->has_star(star.first.c_str(), star.second) == 0) return false;
    }
    if (rule.min_stars > 0 && game->value(NEBULA_VALUE_POWER_STARS) < rule.min_stars) return false;
    return true;
}

// ------------------------------------------------------------------------------------------------ saved state

// What the plugin keeps per seed, in the mod's store beside the mods.
std::string g_seed;
std::string g_config_text;               // the lines BEGIN..COMMIT last received
int g_given = -1;                        // the last consumable item index applied
std::set<std::string> g_reported;        // "galaxy:number:luigi" of every star collected this seed
struct Fix {                             // a star's ownership to put back where the multiworld wants it
    int luigi;
    Star entry;
    Star played;
    bool unset_played;
};
std::vector<Fix> g_fixes;

std::string load_text(const char* key) {
    std::vector<char> buffer(1 << 20);
    const int32_t n = game->store_get_text(me, key, buffer.data(), static_cast<int32_t>(buffer.size()));
    return std::string(buffer.data(), n > 0 ? static_cast<size_t>(n) : 0u);
}

void save_tokens() {
    std::string text;
    {
        Shared hold(&g_lock);
        for (const std::string& token : g_tokens) text += (text.empty() ? "" : ",") + token;
    }
    game->store_set_text(me, "tokens", text.c_str());
}

void save_reported() {
    std::string text;
    for (const std::string& star : g_reported) text += (text.empty() ? "" : ",") + star;
    game->store_set_text(me, "reported", text.c_str());
}

void save_fixes() {
    std::string text;
    for (const Fix& fix : g_fixes) {
        text += fix.entry.first + ":" + std::to_string(fix.entry.second) + ":" + fix.played.first + ":" +
                std::to_string(fix.played.second) + ":" + std::to_string(fix.unset_played ? 1 : 0) + ":" +
                std::to_string(fix.luigi) + ";";
    }
    game->store_set_text(me, "fixes", text.c_str());
}

// ------------------------------------------------------------------------------------------------ the client's lines

std::shared_ptr<Config> g_building;  // between BEGIN and COMMIT
std::string g_client_version;        // the mod version the connected client is made for
std::string g_building_text;

struct Pending {                     // an item to apply in a stage, where the player is
    int index;
    std::string kind;                // "GIVE", "TRAP" or "POWERUP"
    std::string what;
    int amount;
    std::string object;              // an enemy trap's object
};
std::deque<Pending> g_pending;
void forget_trap_actors();
std::deque<std::string> g_messages;
bool g_kill_requested = false;
bool g_dying_from_link = false;

void send_line(const std::string& line);

void report_all_stars() {
    for (const std::string& star : g_reported) {
        const std::vector<std::string> parts = split(star, ':');
        if (parts.size() == 3) send_line("STAR\t" + parts[0] + "\t" + parts[1] + "\t" + parts[2]);
    }
}

// Power-ups: one received and not yet taken (a stage may not have its model), and the one carried between levels.
int g_power_wanted = 0;
int g_power_held = 0;

void set_power_wanted(int mode) {
    if (g_power_wanted == mode) return;
    g_power_wanted = mode;
    game->store_set(me, "power_wanted", mode);
}

void set_power_held(int mode) {
    if (g_power_held == mode) return;
    g_power_held = mode;
    game->store_set(me, "power_held", mode);
}

std::set<int> int_set(const std::string& csv) {
    std::set<int> out;
    for (const std::string& n : list(csv)) out.insert(to_int(n));
    return out;
}

void forget_seed(const std::string& seed) {
    log("a new seed: " + seed);
    {
        Exclusive hold(&g_lock);
        g_config.reset();
        g_tokens.clear();
        g_unlock_all = false;
    }
    g_seed = seed;
    g_config_text.clear();
    g_given = -1;
    g_reported.clear();
    g_fixes.clear();
    g_pending.clear();
    g_power_wanted = 0;
    g_power_held = 0;
    game->store_set(me, "power_wanted", 0);
    game->store_set(me, "power_held", 0);
    game->store_set_text(me, "seed", seed.c_str());
    game->store_set_text(me, "config", "");
    game->store_set(me, "given", -1);
    game->store_set(me, "unlock_all", 0);
    save_tokens();
    save_reported();
    save_fixes();
}

// Config lines are the same whether they come from the client or from the store.
void config_line(const std::vector<std::string>& w) {
    if (!g_building) return;
    Config& c = *g_building;
    if (w[0] == "RULE" && w.size() >= 7) {
        Rule rule;
        rule.enforce = w[2] == "1";
        rule.min_stars = to_int(w[3]);
        rule.items = list(w[4]);
        rule.flags = list(w[5]);
        for (const std::string& star : list(w[6])) {
            const size_t colon = star.rfind(':');
            if (colon != std::string::npos) rule.stars.emplace_back(star.substr(0, colon), to_int(star.substr(colon + 1)));
        }
        c.rules[w[1]] = std::move(rule);
    } else if (w[0] == "LUIGIRULE" && w.size() >= 2) {
        c.has_luigi_rule = w[1] != "-";
        c.luigi_rule = Rule{};
        c.luigi_rule.items = list(w[1]);
    } else if (w[0] == "REDIRECT" && w.size() >= 5) {
        c.redirects[{w[1], to_int(w[2])}] = {w[3], to_int(w[4])};
    } else if (w[0] == "KEEP" && w.size() >= 3) {
        c.keep.insert({w[1], to_int(w[2])});
    } else if (w[0] == "OPTION" && w.size() >= 3) {
        if (w[1] == "messages") c.messages = w[2] == "1";
        if (w[1] == "death_link") c.death_link = w[2] == "1";
        if (w[1] == "persist") c.persist = int_set(w[2]);
        if (w[1] == "persist_items_only") c.persist_items_only = w[2] == "1";
        if (w[1] == "untimed") c.untimed = int_set(w[2]);
        if (w[1] == "skip_cutscenes") c.skip_cutscenes = w[2] == "1";
        if (w[1] == "skip_prologue") c.skip_prologue = w[2] == "1";
        if (w[1] == "spin_from_start") c.spin_from_start = w[2] == "1";
        if (w[1] == "enemy_pool") {
            c.enemy_pool.clear();
            for (const std::string& entry : list(w[2])) {
                const std::vector<std::string> p = split(entry, ':');  // object:count[:class]
                if (p.size() >= 2) {
                    Config::PoolEntry e{p[0], std::clamp(to_int(p[1]), 0, 200), p.size() >= 3 && !p[2].empty() ? p[2] : p[0],
                                        p.size() >= 4 && !p[3].empty() ? p[3] : "none", {}};
                    if (p.size() >= 5 && !p[4].empty() && p[4] != "-") {
                        for (const std::string& step : split(p[4], '|')) {
                            if (!step.empty()) e.place.push_back(split(step, '='));
                        }
                    }
                    c.enemy_pool.push_back(e);
                }
            }
        }
        if (w[1] == "skip_never_movies") {
            c.skip_never_movies.clear();
            for (const std::string& movie : list(w[2])) c.skip_never_movies.insert(movie);
        }
        if (w[1] == "skip_never_stages") {
            c.skip_never_stages.clear();
            for (const std::string& stage : list(w[2])) c.skip_never_stages.insert(stage);
        }
    } else if (w[0] == "TEXT" && w.size() >= 3) {
        c.texts.emplace_back(w[1], w[2]);
    } else if (w[0] == "CONST" && w.size() >= 3) {
        c.constants[w[1]] = static_cast<float>(std::atof(w[2].c_str()));
    }
}

// The player table as the game had it, before any seed's constants.
std::map<std::pair<int, std::string>, float> g_original_constants;

void apply_constants(const Config& c) {
    if (game->set_player_constant == nullptr || game->player_constant == nullptr) return;
    for (const auto& [key, value] : g_original_constants) {
        if (c.constants.count(key.second) == 0) game->set_player_constant(key.first, key.second.c_str(), 0, value);
    }
    for (const auto& [name, value] : c.constants) {
        for (int character = 0; character < 2; ++character) {
            const auto key = std::make_pair(character, name);
            if (g_original_constants.count(key) == 0) {
                g_original_constants[key] = game->player_constant(character, name.c_str(), 0);
            }
            if (game->set_player_constant(character, name.c_str(), 0, value) != 0) log("no player constant " + name);
        }
    }
}

void commit(bool from_store) {
    if (!g_building) return;
    apply_constants(*g_building);
    if (game->add_text != nullptr) {
        for (const auto& [id, text] : g_building->texts) game->add_text(id.c_str(), text.c_str());
    }
    {
        Exclusive hold(&g_lock);
        g_config = g_building;
    }
    log("rules: " + std::to_string(g_building->rules.size()) + ", redirects: " +
        std::to_string(g_building->redirects.size()) + (from_store ? " (saved)" : ""));
    g_building.reset();
    if (!from_store) {
        g_config_text = g_building_text;
        game->store_set_text(me, "config", g_config_text.c_str());
    }
    g_building_text.clear();
}

// What the player sees of the connection: whether the client is there, and whether it's in a multiworld.
bool g_status_messages = true;  // the mod option "status"
int g_server_state = -1;        // the client's last STATUS: -1 not told yet, 0 no multiworld, 1 in one
bool g_had_client = false;      // the client was connected (so losing it is worth saying)

void status_message(const std::string& text) {
    log(text);
    if (g_status_messages) g_messages.push_back(text);
}

void client_line(const std::string& line) {
    std::vector<std::string> w = split(line, '\t');
    if (w.empty() || w[0].empty()) return;
    const std::string& cmd = w[0];
    if (cmd == "HELLO") {
        send_line(std::string("HELLO\t") + kVersion);
    } else if (cmd == "STATUS" && w.size() >= 3) {
        const int state = w[1] == "1" ? 1 : 0;
        if (state != g_server_state) {
            if (state == 1) {
                status_message("Connected to Archipelago" + (w[2] != "-" ? " as " + w[2] : std::string()) + ".");
            } else {
                status_message(g_server_state == 1 ? "The Archipelago client lost the server."
                                                   : "Archipelago client found: connect it to a server.");
            }
            g_server_state = state;
        }
    } else if (cmd == "CLIENT" && w.size() >= 2) {
        g_client_version = w[1];
    } else if (cmd == "SEED" && w.size() >= 2) {
        if (w[1] != g_seed) forget_seed(w[1]);
    } else if (cmd == "BEGIN") {
        g_building = std::make_shared<Config>();
        g_building_text.clear();
    } else if (cmd == "RULE" || cmd == "LUIGIRULE" || cmd == "REDIRECT" || cmd == "KEEP" || cmd == "OPTION" ||
               cmd == "CONST" || cmd == "TEXT") {
        config_line(w);
        g_building_text += line + "\n";
    } else if (cmd == "COMMIT") {
        if (g_client_version != kVersion) {
            // An older client still running after an update speaks the old mod's language: its settings may be read wrong.
            log("the Archipelago client is for mod " + (g_client_version.empty() ? std::string("0.1") : g_client_version) +
                ", this mod is " + kVersion + ": restart the client from the Archipelago Launcher");
            g_messages.push_back("Restart the Archipelago client: it's older than this mod.");
        }
        commit(false);
        report_all_stars();
    } else if (cmd == "TOKEN" && w.size() >= 2) {
        bool added;
        {
            Exclusive hold(&g_lock);
            added = g_tokens.insert(w[1]).second;
        }
        if (added) save_tokens();
    } else if ((cmd == "GIVE" || cmd == "TRAP" || cmd == "POWERUP") && w.size() >= 3) {
        const int index = to_int(w[1], -1);
        const bool queued = std::any_of(g_pending.begin(), g_pending.end(), [&](const Pending& p) { return p.index == index; });
        if (index > g_given && !queued) {
            if (cmd == "POWERUP") g_pending.push_back({index, cmd, "", to_int(w[2]), ""});
            else if (w.size() >= 4) g_pending.push_back({index, cmd, w[2], to_int(w[3]), w.size() >= 5 ? w[4] : ""});
        }
    } else if (cmd == "MSG" && w.size() >= 2) {
        const std::shared_ptr<const Config> c = config();
        if (!c || c->messages) g_messages.push_back(w[1]);
        if (g_messages.size() > 20) g_messages.pop_front();
    } else if (cmd == "UNLOCKALL") {
        bool changed;
        {
            Exclusive hold(&g_lock);
            changed = !g_unlock_all;
            g_unlock_all = true;
        }
        if (changed) {
            game->store_set(me, "unlock_all", 1);
            g_messages.push_back("Everything is unlocked!");
        }
    } else if (cmd == "KILL") {
        g_kill_requested = true;
    }
}

void load_store() {
    g_seed = load_text("seed");
    if (g_seed.empty()) return;
    {
        Exclusive hold(&g_lock);
        g_tokens.clear();
        for (const std::string& token : list(load_text("tokens"))) g_tokens.insert(token);
        g_unlock_all = game->store_get(me, "unlock_all", 0) != 0;
    }
    g_given = game->store_get(me, "given", -1);
    g_power_wanted = game->store_get(me, "power_wanted", 0);
    g_power_held = game->store_get(me, "power_held", 0);
    for (const std::string& star : list(load_text("reported"))) g_reported.insert(star);
    for (const std::string& fix : split(load_text("fixes"), ';')) {
        const std::vector<std::string> p = split(fix, ':');
        if (p.size() == 6) g_fixes.push_back({to_int(p[5]), {p[0], to_int(p[1])}, {p[2], to_int(p[3])}, p[4] == "1"});
    }
    g_config_text = load_text("config");
    if (!g_config_text.empty()) {
        g_building = std::make_shared<Config>();
        for (const std::string& line : split(g_config_text, '\n')) {
            if (!line.empty()) config_line(split(line, '\t'));
        }
        commit(true);
    }
}

// ------------------------------------------------------------------------------------------------ the connection

SOCKET g_socket = INVALID_SOCKET;
bool g_connecting = false;
ULONGLONG g_next_ping = 0;  // the next PING: where the player is, so the client knows the game is alive
ULONGLONG g_next_try = 0;
std::string g_in;
std::string g_out;
int g_port = 43770;

void disconnect() {
    g_client_version.clear();
    if (g_had_client) {
        g_had_client = false;
        status_message("Lost the Archipelago client: items and checks wait until it's back.");
    }
    g_server_state = -1;
    g_next_ping = 0;
    if (g_socket != INVALID_SOCKET) closesocket(g_socket);
    g_socket = INVALID_SOCKET;
    g_connecting = false;
    g_in.clear();
    g_out.clear();
    g_next_try = GetTickCount64() + 3000;
}

std::string where_the_player_is();

void send_line(const std::string& line) {
    if (g_socket != INVALID_SOCKET) g_out += line + "\n";
}

void try_connect() {
    if (GetTickCount64() < g_next_try) return;
    g_next_try = GetTickCount64() + 3000;
    g_socket = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (g_socket == INVALID_SOCKET) return;
    u_long nonblocking = 1;
    ioctlsocket(g_socket, FIONBIO, &nonblocking);
    sockaddr_in to{};
    to.sin_family = AF_INET;
    to.sin_port = htons(static_cast<u_short>(g_port));
    inet_pton(AF_INET, "127.0.0.1", &to.sin_addr);
    if (connect(g_socket, reinterpret_cast<sockaddr*>(&to), sizeof(to)) == SOCKET_ERROR &&
        WSAGetLastError() != WSAEWOULDBLOCK) {
        disconnect();
        return;
    }
    g_connecting = true;
}

void pump() {
    if (g_socket == INVALID_SOCKET) {
        try_connect();
        return;
    }
    if (g_connecting) {
        fd_set writable, failed;
        FD_ZERO(&writable);
        FD_ZERO(&failed);
        FD_SET(g_socket, &writable);
        FD_SET(g_socket, &failed);
        timeval now{0, 0};
        if (select(0, nullptr, &writable, &failed, &now) <= 0) return;
        if (FD_ISSET(g_socket, &failed)) {
            disconnect();
            return;
        }
        g_connecting = false;
        g_had_client = true;
        log("connected to the Archipelago client");
        send_line(std::string("HELLO\t") + kVersion);
    }
    if (GetTickCount64() >= g_next_ping) {
        g_next_ping = GetTickCount64() + 2000;
        send_line(std::string("PING\t") + where_the_player_is());
    }
    char buffer[4096];
    for (;;) {
        const int n = recv(g_socket, buffer, sizeof(buffer), 0);
        if (n > 0) {
            g_in.append(buffer, static_cast<size_t>(n));
            continue;
        }
        if (n == 0 || WSAGetLastError() != WSAEWOULDBLOCK) {
            log("the Archipelago client went away");
            disconnect();
            return;
        }
        break;
    }
    size_t end;
    while ((end = g_in.find('\n')) != std::string::npos) {
        std::string line = g_in.substr(0, end);
        g_in.erase(0, end + 1);
        if (!line.empty() && line.back() == '\r') line.pop_back();
        client_line(line);
    }
    while (!g_out.empty()) {
        const int n = send(g_socket, g_out.data(), static_cast<int>(std::min<size_t>(g_out.size(), 65536)), 0);
        if (n > 0) {
            g_out.erase(0, static_cast<size_t>(n));
        } else {
            if (WSAGetLastError() != WSAEWOULDBLOCK) disconnect();
            break;
        }
    }
}

// ------------------------------------------------------------------------------------------------ hooks

bool (*orig_checker_is_on)(const void*, const char*) = nullptr;
bool (*orig_holder_is_on)(const void*, const char*) = nullptr;
bool (*orig_function_is_on)(const char*) = nullptr;
bool (*orig_galaxy_open)(const char*) = nullptr;
bool (*orig_view_complete)(const void*) = nullptr;
bool (*orig_appear_luigi)(const void*, int) = nullptr;
bool (*file_is_luigi)(const void*, int) = nullptr;
void (*orig_set_scenario)(int, int) = nullptr;
const char* (*current_stage_name)() = nullptr;

// The rule for a flag, if the multiworld has one.
const Rule* rule_for(const Config* c, const char* name) {
    if (c == nullptr || name == nullptr) return nullptr;
    const auto it = c->rules.find(name);
    return it != c->rules.end() ? &it->second : nullptr;
}

bool hook_checker_is_on(const void* self, const char* name) {
    const std::shared_ptr<const Config> c = config();
    if (const Rule* rule = rule_for(c.get(), name)) {
        return rule_holds(*rule, [self](const char* flag) { return orig_checker_is_on(self, flag); });
    }
    return orig_checker_is_on(self, name);
}

bool hook_holder_is_on(const void* self, const char* name) {
    const std::shared_ptr<const Config> c = config();
    if (const Rule* rule = rule_for(c.get(), name)) {
        return rule_holds(*rule, [self](const char* flag) { return orig_holder_is_on(self, flag); });
    }
    return orig_holder_is_on(self, name);
}

bool hook_function_is_on(const char* name) {
    const std::shared_ptr<const Config> c = config();
    if (const Rule* rule = rule_for(c.get(), name)) {
        return rule_holds(*rule, [](const char* flag) { return orig_function_is_on(flag); });
    }
    return orig_function_is_on(name);
}

// "Could it open now": a dome shows such a galaxy as new and lets the player in. The same rule as "is it open".
bool (*orig_checker_can_on)(const void*, const char*) = nullptr;
bool (*orig_holder_can_on)(const void*, const char*) = nullptr;
bool (*orig_function_can_on)(const char*) = nullptr;

bool hook_checker_can_on(const void* self, const char* name) {
    const std::shared_ptr<const Config> c = config();
    if (const Rule* rule = rule_for(c.get(), name)) {
        return rule_holds(*rule, [self](const char* flag) { return orig_checker_is_on != nullptr ? orig_checker_is_on(self, flag)
                                                                                                : game->is_event_flag_on(flag) != 0; });
    }
    return orig_checker_can_on(self, name);
}

bool hook_holder_can_on(const void* self, const char* name) {
    const std::shared_ptr<const Config> c = config();
    if (const Rule* rule = rule_for(c.get(), name)) {
        return rule_holds(*rule, [](const char* flag) { return game->is_event_flag_on(flag) != 0; });
    }
    return orig_holder_can_on(self, name);
}

bool hook_function_can_on(const char* name) {
    const std::shared_ptr<const Config> c = config();
    if (const Rule* rule = rule_for(c.get(), name)) {
        return rule_holds(*rule, [](const char* flag) { return game->is_event_flag_on(flag) != 0; });
    }
    return orig_function_can_on(name);
}

bool hook_galaxy_open(const char* name) {
    const std::shared_ptr<const Config> c = config();
    if (const Rule* rule = rule_for(c.get(), name)) {
        return rule_holds(*rule, [](const char* flag) { return game->is_event_flag_on(flag) != 0; });
    }
    return orig_galaxy_open(name);
}

bool luigi_open() {
    const std::shared_ptr<const Config> c = config();
    return c && c->has_luigi_rule ? rule_holds(c->luigi_rule, [](const char* flag) { return game->is_event_flag_on(flag) != 0; })
                                  : false;
}

bool has_luigi_rule() {
    const std::shared_ptr<const Config> c = config();
    return c && c->has_luigi_rule;
}

bool hook_view_complete(const void* self) {
    return has_luigi_rule() ? luigi_open() : orig_view_complete(self);
}

bool hook_appear_luigi(const void* self, int id) {
    if (!has_luigi_rule()) return orig_appear_luigi(self, id);
    return (file_is_luigi != nullptr && file_is_luigi(self, id)) || luigi_open();
}

// ---- Skipping cutscenes, with Nebula's own skip (NebulaCutsceneSkip): while a cutscene plays, its update sees + pressed
// once, and its Yes/No question is answered Yes without being shown. What it skips, and how, is all its own.
void (*orig_skip_update)() = nullptr;
bool (*orig_is_pressed_pause)(int) = nullptr;
bool (*orig_is_held_pause)(int) = nullptr;
void (*orig_appear_yes_no)(void*, const char16_t*) = nullptr;
bool (*orig_is_selected_yes)(const void*) = nullptr;
void (*orig_start_movie)(const char*) = nullptr;
bool (*skip_is_skipping)() = nullptr;
bool (*movie_is_active)() = nullptr;
bool (*opening_camera_is_on)() = nullptr;

bool g_in_skip_update = false;  // inside NebulaCutsceneSkip::update
bool g_skip_press = false;      // this update sees + pressed
bool g_skip_press_next = false; // the next one should
bool g_skip_answer = false;     // its question was ours: answer Yes
std::string g_stage_name;       // the last stage started (from its event, so known from its first frame)
std::string g_movie;            // the movie playing, by its path

void hook_skip_update() {
    g_in_skip_update = true;
    g_skip_press = g_skip_press_next;
    g_skip_press_next = false;
    orig_skip_update();
    g_skip_press = false;
    g_in_skip_update = false;
}

bool hook_is_pressed_pause(int channel) {
    if (g_in_skip_update && g_skip_press) return true;
    return orig_is_pressed_pause(channel);
}

bool hook_is_held_pause(int channel) {
    if (g_in_skip_update && g_skip_press) return false;  // a fresh press: one held before doesn't block it
    return orig_is_held_pause(channel);
}

void hook_appear_yes_no(void* self, const char16_t* text) {
    if (g_in_skip_update && g_skip_press) {
        g_skip_answer = true;  // never shown: the skip goes on as after Yes
        log("skipping a cutscene");
        return;
    }
    orig_appear_yes_no(self, text);
}

bool hook_is_selected_yes(const void* self) {
    if (g_in_skip_update && g_skip_answer) {
        g_skip_answer = false;
        return true;
    }
    return orig_is_selected_yes(self);
}

void hook_start_movie(const char* name) {
    g_movie = name != nullptr ? name : "";
    orig_start_movie(name);
}

// The spin: the game forbids it each stage until its story gives it (GameSequenceProgress).
void (*orig_swing_permission)(bool) = nullptr;

void hook_swing_permission(bool permission) {
    const std::shared_ptr<const Config> c = config();
    orig_swing_permission(permission || (c && c->spin_from_start));
}

// The new file's story time, closed as soon as it opens: the prologue goes on as if it had been read.
void (*orig_picture_book_appear)(void*) = nullptr;
void (*orig_letter_appear)(void*) = nullptr;
void (*picture_book_kill)(void*) = nullptr;
void (*letter_kill)(void*) = nullptr;

bool skip_prologue_now() {
    const std::shared_ptr<const Config> c = config();
    return c && c->skip_prologue;
}

void hook_picture_book_appear(void* self) {
    orig_picture_book_appear(self);
    if (skip_prologue_now() && picture_book_kill != nullptr) {
        log("skipping the story time");
        picture_book_kill(self);
    }
}

void hook_letter_appear(void* self) {
    orig_letter_appear(self);
    if (skip_prologue_now() && letter_kill != nullptr) {
        log("skipping Peach's letter");
        letter_kill(self);
    }
}

// Same-galaxy redirects, decided as the star select hands over: no second load.
Star g_hook_entry;
Star g_hook_target;

void hook_set_scenario(int placed, int selected) {
    const std::shared_ptr<const Config> c = config();
    if (c && current_stage_name != nullptr) {
        const char* stage = current_stage_name();
        if (stage != nullptr) {
            const auto it = c->redirects.find({stage, selected});
            if (it != c->redirects.end() && it->second.first == stage) {
                g_hook_entry = {stage, selected};
                g_hook_target = it->second;
                log("star select " + std::string(stage) + " " + std::to_string(selected) + " plays " +
                    std::to_string(it->second.second));
                orig_set_scenario(it->second.second, it->second.second);
                return;
            }
        }
    }
    orig_set_scenario(placed, selected);
}

template <typename Fn>
bool install(const char* const* symbols, Fn replacement, Fn* original, const char* what) {
    for (const char* const* symbol = symbols; *symbol != nullptr; ++symbol) {
        void* target = game->find_function(*symbol);
        if (target == nullptr) continue;
        if (game->hook(target, reinterpret_cast<void*>(replacement), reinterpret_cast<void**>(original)) == 0) {
            log(std::string("hooked ") + what);
            return true;
        }
    }
    log(std::string("could not hook ") + what);
    return false;
}

// ------------------------------------------------------------------------------------------------ the game's events

// Where the player is: the entry picked and the mission it plays, when they differ.
Star g_current_entry;
Star g_current_played;
bool g_in_redirect = false;
bool g_played_was_owned = false;
Star g_change_to;           // a cross-galaxy redirect waiting for the stage's first frame
Star g_change_entry;
bool g_change_pending = false;
bool g_change_issued = false;
bool g_kick = false;        // a locked galaxy entered: back to the dome on the first frame
int g_setting_star = 0;     // our own set_star calls fire STAR_GET too
int g_message_frames = 0;
int g_frame = 0;            // stage frames, all stages
int g_stage_frames = 0;     // stage frames since this stage started
bool g_stage_ending = false;
int g_last_damage = -1000;  // the frame of the last hit
bool g_applying = false;    // the plugin itself is changing the power-up
int g_power_try = -1000;    // the frame of the last attempt to give a power-up
std::deque<std::string> g_spawns;  // enemies of traps still to drop
NebulaActor* g_spawn_check = nullptr;  // the first of the last group, looked at again a second later
std::string g_spawn_check_object;
int g_spawn_check_frame = 0;
int g_spawned = 0;          // of the current group, for spreading them out
int g_spawn_wait = 0;

// Which dome a galaxy is in: the game asks it to choose where a cleared (or left) galaxy returns to.
uint32_t (*orig_dome_of)(const char*) = nullptr;

uint32_t hook_dome_of(const char* galaxy) {
    if (galaxy != nullptr && g_in_redirect && config() && g_current_played.first == galaxy &&
        g_current_entry.first != g_current_played.first) {
        return orig_dome_of(g_current_entry.first.c_str());
    }
    return orig_dome_of(galaxy);
}

void apply_fixes() {
    if (g_fixes.empty()) return;
    const int luigi = game->value(NEBULA_VALUE_LUIGI);
    // A played star's own place may be an entry finished since, with another mission: that one stays owned.
    std::set<Star> finished;
    for (const Fix& fix : g_fixes) {
        if (fix.luigi == luigi) finished.insert(fix.entry);
    }
    ++g_setting_star;
    for (const Fix& fix : g_fixes) {
        if (fix.luigi != luigi) continue;
        game->set_star(fix.entry.first.c_str(), fix.entry.second, 1);
        if (fix.unset_played && finished.count(fix.played) == 0) {
            game->set_star(fix.played.first.c_str(), fix.played.second, 0);
        }
    }
    --g_setting_star;
}

int32_t on_stage_start(const NebulaModEventData* data, void*) {
    g_kick = false;
    g_dying_from_link = false;
    g_stage_frames = 0;
    g_power_try = -1000;  // counted in this stage's frames
    g_stage_ending = false;
    g_spawns.clear();  // a group belongs to the stage it was dropped in
    g_spawn_check = nullptr;
    g_stage_name = data->name != nullptr ? data->name : "";
    const std::shared_ptr<const Config> c = config();
    if (!c || data->name == nullptr) return data->value;
    apply_fixes();
    const std::string stage = data->name;
    const Star here{stage, data->value};
    bool fresh_redirect = false;

    if (g_change_pending && g_change_issued && here == g_change_to) {
        // The second half of a cross-galaxy redirect.
        g_change_pending = false;
        g_in_redirect = true;
        fresh_redirect = true;
        g_current_entry = g_change_entry;
        g_current_played = here;
    } else if (!g_hook_entry.first.empty() && here == g_hook_target) {
        g_in_redirect = true;
        fresh_redirect = true;
        g_current_entry = g_hook_entry;
        g_current_played = here;
    } else if (g_in_redirect && here == g_current_played) {
        // Restarted after a miss: still the same entry.
    } else {
        g_in_redirect = false;
        g_change_pending = false;
        const auto it = c->redirects.find(here);
        if (it != c->redirects.end()) {
            g_change_pending = true;
            g_change_issued = false;
            g_change_entry = here;
            g_change_to = it->second;
            log(stage + " " + std::to_string(here.second) + " plays " + it->second.first + " " +
                std::to_string(it->second.second));
        }
    }
    g_hook_entry = {};
    if (fresh_redirect) g_played_was_owned = game->has_star(g_current_played.first.c_str(), g_current_played.second) != 0;

    // A galaxy's lock is on the galaxy entered: a mission it plays elsewhere is let in.
    const std::string entered = g_in_redirect ? g_current_entry.first : stage;
    if (const Rule* rule = rule_for(c.get(), entered.c_str())) {
        if (rule->enforce && !rule_holds(*rule, [](const char* flag) { return game->is_event_flag_on(flag) != 0; })) {
            log(entered + " is locked: back to the observatory");
            g_kick = true;
            g_messages.push_back("This galaxy is still locked.");
            g_in_redirect = false;
            g_change_pending = false;
        }
    }
    return data->value;
}

int32_t on_star_get(const NebulaModEventData* data, void*) {
    if (g_setting_star > 0 || data->name == nullptr || !config()) return data->value;
    const int luigi = game->value(NEBULA_VALUE_LUIGI);
    const Star got{data->name, data->value};
    const std::string key = got.first + ":" + std::to_string(got.second) + ":" + std::to_string(luigi);
    send_line("STAR\t" + got.first + "\t" + std::to_string(got.second) + "\t" + std::to_string(luigi));
    if (g_reported.insert(key).second) save_reported();

    if (g_in_redirect && got == g_current_played && g_current_entry != g_current_played) {
        const std::shared_ptr<const Config> c = config();
        const bool keep = c && c->keep.count(got) != 0;
        g_fixes.push_back({luigi, g_current_entry, g_current_played, !keep && !g_played_was_owned});
        save_fixes();
    }
    return data->value;
}

int32_t on_stage_end(const NebulaModEventData* data, void*) {
    g_stage_ending = true;
    // Its objects are going: the next stage's may take their places in memory before its own trap enemies are made.
    forget_trap_actors();
    if (config()) apply_fixes();
    return data->value;
}

// The power-up is about to change. Refuses a timer's end for untimed power-ups, and keeps track of what carries over.
int32_t on_power_up(const NebulaModEventData* data, void*) {
    const std::shared_ptr<const Config> c = config();
    if (!c) return data->value;
    const int current = game->value(NEBULA_VALUE_POWER_UP);
    int next = data->value;
    const bool hit = g_frame - g_last_damage < 10;
    const bool settling = g_stage_ending || g_stage_frames < 30 || game->is_cutscene_playing() != 0;
    if (next == 0 && current != 0 && c->untimed.count(current) != 0 && !g_applying && !hit && !settling &&
        game->value(NEBULA_VALUE_DEAD) == 0) {
        next = current;  // the same again: the game starts its time over
    }
    if (next != 0) {
        const bool keeps = c->persist.count(next) != 0 &&
                           (!c->persist_items_only || g_applying || (next == g_power_held && current == next));
        set_power_held(keeps ? next : 0);
    } else if (!settling || g_applying) {
        set_power_held(0);  // lost: a hit, water, light, a trap
    }
    return next;
}

int32_t on_player_damage(const NebulaModEventData* data, void*) {
    g_last_damage = g_frame;
    return data->value;
}

int32_t on_game_save(const NebulaModEventData* data, void*) {
    if (config()) apply_fixes();
    return data->value;
}

int32_t on_player_miss(const NebulaModEventData* data, void*) {
    const std::shared_ptr<const Config> c = config();
    if (c && c->death_link && !g_dying_from_link) send_line("DEATH");
    if (c) set_power_held(0);
    return data->value;
}

void (*g_change_item_status)(int) = nullptr;     // the game's own item pickup, by its item number
bool (*g_can_change_state)(NebulaActor*) = nullptr;  // whether the player can take one now

// False: not now (the player can't take it yet); it's tried again next frame.
bool apply_item(const Pending& p) {
    if (p.kind == "GIVE" && p.what == "life_up") {
        // The Life Mushroom, as if picked up: its own little scene and the extra health (a full refill if it's had).
        NebulaActor* player = game->player();
        if (player == nullptr || (g_can_change_state != nullptr && !g_can_change_state(player))) return false;
        if (g_change_item_status != nullptr) {
            g_change_item_status(p.amount);
        } else {
            game->set_value(NEBULA_SET_MAX_LIFE, 6);
            game->set_value(NEBULA_SET_LIFE, 6);
        }
        return true;
    }
    if (p.kind == "POWERUP") {
        set_power_wanted(p.amount);
        g_power_try = -1000;
    } else if (p.kind == "GIVE") {
        if (p.what == "lives") game->set_value(NEBULA_SET_ADD_LIVES, p.amount);
        else if (p.what == "star_bits") game->set_value(NEBULA_SET_ADD_STAR_BITS, p.amount);
        else if (p.what == "coins") game->set_value(NEBULA_SET_ADD_COINS, p.amount);
        else if (p.what == "heal") game->set_value(NEBULA_SET_LIFE, game->value(NEBULA_VALUE_MAX_LIFE));
    } else {
        if (p.what == "damage") {
            const int life = game->value(NEBULA_VALUE_LIFE);
            game->set_value(NEBULA_SET_LIFE, std::max(1, life - std::max(1, p.amount)));
        } else if (p.what == "power_up") {
            g_applying = true;
            game->set_value(NEBULA_SET_POWER_UP, NEBULA_POWER_UP_NONE);
            g_applying = false;
            set_power_held(0);
            set_power_wanted(0);
        } else if (p.what == "star_bits") {
            game->set_value(NEBULA_SET_ADD_STAR_BITS, -std::min(p.amount, game->value(NEBULA_VALUE_STAR_BITS)));
        } else if (p.what == "enemies" && !p.object.empty()) {
            g_spawned = 0;
            for (int i = 0; i < std::clamp(p.amount, 1, 50); ++i) g_spawns.push_back(p.object);
            log("enemy trap: " + std::to_string(p.amount) + " " + p.object);
        }
    }
    return true;
}

// The game's own calls for an actor's visibility. A level's enemies join the clipping system as the stage loads; one
// made in play is kept out of it (always run and drawn), as Nebula's own spawned objects are.
using ActorTest = bool (*)(NebulaActor*);
using ActorCall = void (*)(NebulaActor*);
ActorCall g_invalidate_clipping = nullptr;
ActorTest g_is_clipped = nullptr;
ActorTest g_is_hidden_model = nullptr;
ActorCall g_show_model = nullptr;

// ---- Enemy traps' enemies. The game takes on new objects only while a stage loads: as it finishes, it sizes its
// draw lists and connects every object to what moves and draws (SceneFunction::allocateDrawBufferActorList). An object
// made after that is never drawn or run, and drawing a model the stage had no room for crashes. So, as each stage loads,
// the mod makes the trap enemies it may need (each enemy, as many as its largest group) and puts them away hidden, as
// the game's own summoners do with theirs; a trap takes some, moves them next to the player and shows them.
void (*orig_allocate_draw_lists)() = nullptr;
void* (*(*g_get_creator)(const char*))(const char*) = nullptr;
void (*g_mount_archives)(const char*, const void*) = nullptr;
void (*g_init_without_iter)(void*) = nullptr;
void (*g_make_dead)(void*) = nullptr;
void (*g_set_zone)(int) = nullptr;
void (*g_clear_zone)() = nullptr;

std::map<std::string, std::vector<NebulaActor*>> g_pool;  // this stage's hidden enemies, by object name
std::map<std::string, size_t> g_pool_next;                // the next one to reuse when all are out
std::map<std::string, std::string> g_pool_class;          // each object's class
std::map<std::string, std::string> g_pool_reset;          // each object's way back from defeat
std::set<NebulaActor*> g_pool_used;                       // ones a trap has put out at least once
using Steps = std::vector<std::vector<std::string>>;
std::map<std::string, Steps> g_pool_place;                // each object's placement steps
std::set<NebulaActor*> g_no_rail;                         // ones kept where they are when sent to a rail they lack
std::map<NebulaActor*, std::pair<float, float>> g_warp;   // ones that, hiding, move round the player (nearest, furthest)
int g_warps = 0;

const std::vector<std::string>* find_step(const Steps& steps, const char* name) {
    for (const std::vector<std::string>& step : steps) {
        if (!step.empty() && step[0] == name) return &step;
    }
    return nullptr;
}

float to_float(const std::string& text, float otherwise) {
    char* end = nullptr;
    const float value = std::strtof(text.c_str(), &end);
    return end != text.c_str() ? value : otherwise;
}

// A field's place in an object, as the world gives it (0x.. or decimal); -1 for none worth trusting.
long field_offset(const std::string& text) {
    char* end = nullptr;
    const long value = std::strtol(text.c_str(), &end, 0);
    return end != text.c_str() && value >= 0x20 && value < 0x2000 ? value : -1;
}
std::set<std::string> g_pool_names;                       // names the game keeps pointers to

void fill_pool(const Config& c) {
    g_pool.clear();
    g_pool_next.clear();
    g_pool_class.clear();
    g_pool_reset.clear();
    g_pool_used.clear();
    g_pool_place.clear();
    g_no_rail.clear();
    g_warp.clear();
    if (c.enemy_pool.empty() || g_get_creator == nullptr || g_init_without_iter == nullptr || g_make_dead == nullptr) return;
    const char* stage = current_stage_name != nullptr ? current_stage_name() : nullptr;
    if (stage == nullptr || stage[0] == '\0' || std::strcmp(stage, "FileSelect") == 0) return;
    struct NoPlacement {  // a TableIter that points at nothing
        const void* table = nullptr;
        int32_t index = -1;
    } no_placement;
    int made = 0;
    for (const Config::PoolEntry& entry : c.enemy_pool) {
        const std::string& object = entry.object;
        const int count = entry.count;
        g_pool_class[object] = entry.cls;
        g_pool_reset[object] = entry.reset;
        g_pool_place[object] = entry.place;
        const std::vector<std::string>* warp = find_step(entry.place, "warp");
        const bool no_rail = find_step(entry.place, "no_rail") != nullptr;
        const char* name = g_pool_names.insert(object).first->c_str();
        auto create = g_get_creator(name);
        if (create == nullptr) {
            log("no " + object + " in this game: left out of enemy traps");
            continue;
        }
        if (g_mount_archives != nullptr) g_mount_archives(name, &no_placement);
        std::vector<NebulaActor*>& pool = g_pool[object];
        for (int i = 0; i < count; ++i) {
            if (g_set_zone != nullptr) g_set_zone(0);
            void* enemy = create(name);
            if (enemy != nullptr) {
                g_init_without_iter(enemy);
                g_make_dead(enemy);
                NebulaActor* actor = reinterpret_cast<NebulaActor*>(enemy);
                pool.push_back(actor);
                if (no_rail) g_no_rail.insert(actor);
                if (warp != nullptr) {
                    const float near_ = warp->size() >= 2 ? to_float((*warp)[1], 500.0f) : 500.0f;
                    const float far_ = warp->size() >= 3 ? to_float((*warp)[2], near_) : near_;
                    g_warp[actor] = {std::min(near_, far_), std::max(near_, far_)};
                }
                ++made;
            }
            if (g_clear_zone != nullptr) g_clear_zone();
        }
    }
    log(std::string("enemy traps: ") + std::to_string(made) + " enemies made ready in " + stage);
}

void hook_allocate_draw_lists() {
    const std::shared_ptr<const Config> c = config();
    if (c) fill_pool(*c);
    orig_allocate_draw_lists();
}

// A hidden one: never put out yet, or defeated and able to come back (it's put back in its starting state). Else the one
// out longest, which moves to the new spot. Null only when every one is defeated for good.
NebulaActor* take_from_pool(const std::string& object) {
    const auto it = g_pool.find(object);
    if (it == g_pool.end() || it->second.empty()) return nullptr;
    std::vector<NebulaActor*>& pool = it->second;
    const std::string& reset = g_pool_reset[object];
    size_t& next = g_pool_next[object];
    for (size_t i = 0; i < pool.size(); ++i) {
        NebulaActor* enemy = pool[(next + i) % pool.size()];
        if (game->is_dead(enemy) == 0) continue;
        const bool fresh = g_pool_used.count(enemy) == 0;
        if (!fresh && reset == "none") continue;
        if (!fresh && reset != "appear") {
            // A member that puts it back in its starting state, given no placement (as at its setup).
            struct NoPlacement {
                const void* table = nullptr;
                int32_t index = -1;
            } no_placement;
            if (auto start = reinterpret_cast<void (*)(NebulaActor*, const void*)>(game->find_function(reset.c_str()))) {
                start(enemy, &no_placement);
            } else {
                continue;
            }
        }
        next = (next + i + 1) % pool.size();
        g_pool_used.insert(enemy);
        return enemy;
    }
    for (size_t i = 0; i < pool.size(); ++i) {
        NebulaActor* enemy = pool[(next + i) % pool.size()];
        if (game->is_dead(enemy) != 0) continue;
        next = (next + i + 1) % pool.size();
        return enemy;
    }
    return nullptr;
}

std::string visibility(NebulaActor* actor) {
    std::string out;
    if (g_is_clipped != nullptr) out += g_is_clipped(actor) ? ", clipped" : ", not clipped";
    if (g_is_hidden_model != nullptr) out += g_is_hidden_model(actor) ? ", model hidden" : ", model shown";
    return out;
}

// Where the player stands: the planet's up there, and two directions across it.
struct PlayerFrame {
    float at[3];
    float up[3];
    float u[3];
    float v[3];
};

bool player_frame(PlayerFrame& f) {
    NebulaActor* player = game->player();
    if (player == nullptr) return false;
    float down[3] = {0.0f, -1.0f, 0.0f};
    game->get_position(player, f.at);
    if (game->get_gravity != nullptr) game->get_gravity(player, down);
    for (int i = 0; i < 3; ++i) f.up[i] = -down[i];
    float side[3] = {1.0f, 0.0f, 0.0f};
    if (std::fabs(f.up[0]) > 0.9f) {
        side[0] = 0.0f;
        side[2] = 1.0f;
    }
    const float* up = f.up;
    float* u = f.u;
    u[0] = up[1] * side[2] - up[2] * side[1];
    u[1] = up[2] * side[0] - up[0] * side[2];
    u[2] = up[0] * side[1] - up[1] * side[0];
    const float length = std::sqrt(u[0] * u[0] + u[1] * u[1] + u[2] * u[2]);
    for (int i = 0; i < 3; ++i) u[i] /= length > 0.0f ? length : 1.0f;
    f.v[0] = up[1] * u[2] - up[2] * u[1];
    f.v[1] = up[2] * u[0] - up[0] * u[2];
    f.v[2] = up[0] * u[1] - up[1] * u[0];
    return true;
}

// The floor below a spot round the player (an angle and a distance across the planet's up); the spot itself if
// there's no floor there.
bool floor_round(const PlayerFrame& f, float angle, float radius, float* out) {
    float spot[3], from[3], to[3];
    for (int i = 0; i < 3; ++i) {
        spot[i] = f.at[i] + (f.u[i] * std::cos(angle) + f.v[i] * std::sin(angle)) * radius;
        from[i] = spot[i] + f.up[i] * 300.0f;
        to[i] = spot[i] - f.up[i] * 600.0f;
    }
    if (game->raycast != nullptr && game->raycast(from, to, out) != 0) return true;
    for (int i = 0; i < 3; ++i) out[i] = spot[i];
    return false;
}

ActorCall g_base_mtx = nullptr;  // LiveActor's own: its base matrix from its position and rotation

// One enemy of a trap's group, on the ground round the player, each a little further round a ring; then its own
// placement steps, from the world.
void spawn_next() {
    PlayerFrame f;
    if (!player_frame(f)) return;
    const std::string object = g_spawns.front();
    g_spawns.pop_front();
    const int n = g_spawned++;
    static const Steps no_steps;
    const auto placed = g_pool_place.find(object);
    const Steps& steps = placed != g_pool_place.end() ? placed->second : no_steps;
    // Round the player, on the ground: a ring that widens with the group, each one found on the floor below its spot.
    // "at_player": the first right where the player is, the rest further out by its spacing.
    const float angle = 2.39996f * static_cast<float>(n);  // the golden angle: no two land together
    float radius = 250.0f + 60.0f * std::sqrt(static_cast<float>(n));
    if (const std::vector<std::string>* at_player = find_step(steps, "at_player")) {
        radius = (at_player->size() >= 2 ? to_float((*at_player)[1], 400.0f) : 400.0f) * std::sqrt(static_cast<float>(n));
    }
    float ground[3], where[3];
    const bool on_ground = floor_round(f, angle, radius, ground);
    for (int i = 0; i < 3; ++i) where[i] = ground[i] + f.up[i] * 20.0f;

    NebulaActor* enemy = take_from_pool(object);
    if (enemy == nullptr) {
        if (g_spawned == 1) {
            log(g_pool.count(object) != 0
                    ? "every " + object + " in this stage is defeated, and they can't come back"
                    : "no " + object + " ready in this stage: switched off in the YAML, or the stage loaded before the client "
                      "connected");
        }
        return;
    }
    game->set_position(enemy, where);
    if (game->set_velocity != nullptr) {
        const float still[3] = {0.0f, 0.0f, 0.0f};
        game->set_velocity(enemy, still);  // nothing left over from how it was last defeated
    }
    bool reappear = false;
    for (const std::vector<std::string>& step : steps) {
        if (step.empty()) continue;
        char* base = reinterpret_cast<char*>(enemy);
        const long at = step.size() >= 2 ? field_offset(step[1]) : -1;
        if (step[0] == "upright") {
            // Stood up to the planet here: the rotation that turns its up to the planet's (as its base matrix reads it).
            const float to_degrees = 57.2957795f;
            const float rotation[3] = {std::asin(std::clamp(f.up[2], -1.0f, 1.0f)) * to_degrees, 0.0f,
                                       std::atan2(-f.up[0], f.up[1]) * to_degrees};
            if (game->set_rotation != nullptr) game->set_rotation(enemy, rotation);
            if (g_base_mtx != nullptr) g_base_mtx(enemy);
        } else if (step[0] == "ground" && at >= 0) {
            std::memcpy(base + at, ground, sizeof(ground));  // the floor it's to come down on
        } else if (step[0] == "f32" && at >= 0 && step.size() >= 3) {
            const float value = to_float(step[2], 0.0f);
            std::memcpy(base + at, &value, sizeof(value));
        } else if (step[0] == "i32" && at >= 0 && step.size() >= 3) {
            const int32_t value = to_int(step[2]);
            std::memcpy(base + at, &value, sizeof(value));
        } else if (step[0] == "reappear") {
            reappear = true;  // its appear() sets it going from where it now is, even when it's already out
        }
    }
    // Turned upright to the planet where it now stands, as a placed one is after placement.
    using AfterPlacement = void (*)(NebulaActor*);
    const auto cls = g_pool_class.find(object);
    const std::string after = "?initAfterPlacement@" + (cls != g_pool_class.end() ? cls->second : object) + "@@UEAAXXZ";
    if (auto setup = reinterpret_cast<AfterPlacement>(game->find_function(after.c_str()))) setup(enemy);
    // One reused while out is already there: it just moves (unless its steps say to start it over).
    if (reappear || game->is_dead(enemy) != 0) game->appear(enemy);
    const std::string before = visibility(enemy);
    if (g_invalidate_clipping != nullptr) g_invalidate_clipping(enemy);
    if (g_is_hidden_model != nullptr && g_show_model != nullptr && g_is_hidden_model(enemy)) g_show_model(enemy);
    if (g_spawned == 1) {
        g_spawn_check = enemy;
        g_spawn_check_object = object;
        g_spawn_check_frame = g_stage_frames + 60;
        log("dropping " + object + (on_ground ? " on the ground" : " (no ground found)") + " round the player" + before);
    }
}

// "no_rail": a trap enemy made with no placement has no rail; sent to its rail's start (as a Magikoopa is each time it
// appears), it stays where it is.
ActorTest g_is_exist_rail = nullptr;
void (*orig_rail_start)(NebulaActor*) = nullptr;
void hook_rail_start(NebulaActor* actor) {
    if (g_no_rail.count(actor) != 0 && !g_is_exist_rail(actor)) return;
    orig_rail_start(actor);
}

// "warp": one that moves along its rail while hidden (a Magikoopa between its spells) has none, so as it hides it's
// moved to somewhere else round the player, between its nearest and furthest, where it appears again.
void (*orig_hide_model_calc_anim)(NebulaActor*) = nullptr;
void hook_hide_model_calc_anim(NebulaActor* actor) {
    orig_hide_model_calc_anim(actor);
    const auto it = g_warp.find(actor);
    if (it == g_warp.end() || g_is_exist_rail(actor)) return;
    PlayerFrame f;
    if (!player_frame(f)) return;
    ++g_warps;
    const float angle = 2.39996f * static_cast<float>(g_warps) + static_cast<float>(g_frame % 628) * 0.01f;
    const float mix = static_cast<float>((g_frame * 7919 + g_warps * 104729) % 1000) / 1000.0f;
    const float radius = it->second.first + (it->second.second - it->second.first) * mix;
    float ground[3], where[3];
    floor_round(f, angle, radius, ground);
    for (int i = 0; i < 3; ++i) where[i] = ground[i] + f.up[i] * 20.0f;
    game->set_position(actor, where);
}

void forget_trap_actors() {
    g_no_rail.clear();
    g_warp.clear();
}

// A second after a group starts: where its first one is, for the log.
void check_spawn() {
    if (g_spawn_check == nullptr || g_stage_frames < g_spawn_check_frame) return;
    NebulaActor* enemy = g_spawn_check;
    g_spawn_check = nullptr;
    float at[3];
    game->get_position(enemy, at);
    log(g_spawn_check_object + " after a second: " + (game->is_dead(enemy) != 0 ? "gone" : "here") + ", " +
        std::to_string(static_cast<int>(game->distance_to_player(enemy))) + " from the player" + visibility(enemy));
}

// Gives a received power-up once the stage can take it, and the carried one back after a level change.
void keep_power_up() {
    if (game->player() == nullptr || game->is_cutscene_playing() != 0 || g_stage_frames < 45) return;
    if (g_stage_frames - g_power_try < 60) return;
    const int target = g_power_wanted != 0 ? g_power_wanted : g_power_held;
    if (target == 0) return;
    if (game->value(NEBULA_VALUE_POWER_UP) != target) {
        g_power_try = g_stage_frames;
        g_applying = true;
        game->set_value(NEBULA_SET_POWER_UP, target);
        g_applying = false;
    }
    if (g_power_wanted != 0 && game->value(NEBULA_VALUE_POWER_UP) == g_power_wanted) set_power_wanted(0);
}

int32_t on_stage_frame(const NebulaModEventData* data, void*) {
    ++g_frame;
    ++g_stage_frames;
    if (!config()) return data->value;
    keep_power_up();
    check_spawn();
    if (!g_spawns.empty() && game->player() != nullptr && !game->is_cutscene_playing() && --g_spawn_wait <= 0) {
        g_spawn_wait = 4;
        spawn_next();
    }
    if (g_kick) {
        g_kick = false;
        game->back_to_dome();
    } else if (g_change_pending && !g_change_issued) {
        g_change_issued = true;
        game->change_stage(g_change_to.first.c_str(), g_change_to.second);
    }
    if (g_kill_requested && game->player() != nullptr) {
        g_kill_requested = false;
        g_dying_from_link = true;
        game->kill_player();
    }
    if (!g_pending.empty() && game->player() != nullptr && !game->is_cutscene_playing()) {
        const Pending p = g_pending.front();
        if (p.index <= g_given) {
            g_pending.pop_front();
        } else if (apply_item(p)) {
            g_pending.pop_front();
            g_given = p.index;
            game->store_set(me, "given", g_given);
        }
    }
    if (g_message_frames > 0) {
        if (--g_message_frames == 0) game->hide_message();
    } else if (!g_messages.empty()) {
        // show_message shows a message by its id: the text is made the mod's own message first.
        if (game->add_text != nullptr) game->add_text("ArchipelagoMessage", g_messages.front().c_str());
        game->show_message("ArchipelagoMessage", 0);
        g_messages.pop_front();
        g_message_frames = 180;
    }
    return data->value;
}

int g_menu_frames = 0;
int g_skip_try = -1000;

// Whether to press + for Nebula's skip this frame: a cutscene plays that isn't the ending. Nebula itself decides
// whether that cutscene can be skipped, and does nothing when it can't; asked again every half second.
void think_about_skipping() {
    const std::shared_ptr<const Config> c = config();
    if (!c || !c->skip_cutscenes || orig_skip_update == nullptr) return;
    if (skip_is_skipping != nullptr && skip_is_skipping()) return;
    if (g_menu_frames - g_skip_try < 30) return;
    const bool stage_live = g_stage_frames > 0 && !g_stage_ending;
    const bool movie = movie_is_active != nullptr && movie_is_active();
    if (movie && c->skip_never_movies.count(g_movie) != 0) return;
    if (c->skip_never_stages.count(g_stage_name) != 0) return;
    const bool opening = stage_live && opening_camera_is_on != nullptr && opening_camera_is_on();
    if (!movie && !opening && game->is_cutscene_playing() == 0) return;
    g_skip_try = g_menu_frames;
    g_skip_press_next = true;
}

std::string where_the_player_is() {
    if (g_stage_frames <= 0 || g_stage_ending || g_stage_name.empty()) return "in the menus";
    if (g_stage_name == "FileSelect") return "at the file select";
    return "in " + g_stage_name;
}

int32_t on_frame(const NebulaModEventData* data, void*) {
    ++g_menu_frames;
    pump();
    think_about_skipping();
    return data->value;
}

}  // namespace

NEBULA_PLUGIN_ENTRY(api, self) {
    game = api;
    me = self;
    const char* enabled = game->option(me, "archipelago");
    g_enabled = enabled == nullptr || std::strcmp(enabled, "off") != 0;
    if (!g_enabled) {
        log("off in the mod's options");
        return;
    }
    if (const char* port = game->option(me, "port")) g_port = to_int(port, 43770);
    if (const char* status = game->option(me, "status")) g_status_messages = std::strcmp(status, "off") != 0;

    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) log("no network: WSAStartup failed");

    load_store();

    static const char* const checker_is_on[] = {"?isOn@GameEventFlagChecker@@QEBA_NPEBD@Z", nullptr};
    static const char* const holder_is_on[] = {"?isOnGameEventFlag@GameDataHolder@@QEBA_NPEBD@Z", nullptr};
    static const char* const function_is_on[] = {"?isOnGameEventFlag@GameDataFunction@@YA_NPEBD@Z", nullptr};
    static const char* const galaxy_open[] = {"?isOnGameEventFlagGalaxyOpen@MR@@YA_NPEBD@Z", nullptr};
    static const char* const view_complete[] = {"?isViewCompleteEnding@UserFile@@QEBA_NXZ", nullptr};
    static const char* const appear_luigi[] = {"?isUserFileAppearLuigi@FileSelector@@QEBA_NH@Z", nullptr};
    static const char* const set_scenario[] = {"?setCurrentScenarioNo@MR@@YAXHH@Z", nullptr};
    install(checker_is_on, &hook_checker_is_on, &orig_checker_is_on, "GameEventFlagChecker::isOn");
    install(holder_is_on, &hook_holder_is_on, &orig_holder_is_on, "GameDataHolder::isOnGameEventFlag");
    install(function_is_on, &hook_function_is_on, &orig_function_is_on, "GameDataFunction::isOnGameEventFlag");
    install(galaxy_open, &hook_galaxy_open, &orig_galaxy_open, "MR::isOnGameEventFlagGalaxyOpen");
    static const char* const checker_can_on[] = {"?canOn@GameEventFlagChecker@@QEBA_NPEBD@Z", nullptr};
    static const char* const holder_can_on[] = {"?canOnGameEventFlag@GameDataHolder@@QEBA_NPEBD@Z", nullptr};
    static const char* const function_can_on[] = {"?canOnGameEventFlag@GameDataFunction@@YA_NPEBD@Z", nullptr};
    static const char* const dome_of[] = {"?getIncludedGrandGalaxyId@GameDataConst@@YAIPEBD@Z", nullptr};
    static const char* const picture_book_appear[] = {"?appear@ProloguePictureBook@@UEAAXXZ", nullptr};
    static const char* const letter_appear[] = {"?appear@PrologueLetter@@UEAAXXZ", nullptr};
    install(checker_can_on, &hook_checker_can_on, &orig_checker_can_on, "GameEventFlagChecker::canOn");
    install(holder_can_on, &hook_holder_can_on, &orig_holder_can_on, "GameDataHolder::canOnGameEventFlag");
    install(function_can_on, &hook_function_can_on, &orig_function_can_on, "GameDataFunction::canOnGameEventFlag");
    install(dome_of, &hook_dome_of, &orig_dome_of, "GameDataConst::getIncludedGrandGalaxyId");
    g_invalidate_clipping = reinterpret_cast<ActorCall>(game->find_function("?invalidateClipping@MR@@YAXPEAVLiveActor@@@Z"));
    g_get_creator = reinterpret_cast<void* (*(*)(const char*))(const char*)>(
        game->find_function("?getCreator@NameObjFactory@@YAP6APEAVNameObj@@PEBD@Z0@Z"));
    g_mount_archives = reinterpret_cast<void (*)(const char*, const void*)>(
        game->find_function("?requestMountObjectArchives@NameObjFactory@@YAXPEBDAEBVTableIter@@@Z"));
    g_init_without_iter = reinterpret_cast<void (*)(void*)>(game->find_function("?initWithoutIter@NameObj@@QEAAXXZ"));
    g_make_dead = reinterpret_cast<void (*)(void*)>(game->find_function("?makeActorDead@LiveActor@@UEAAXXZ"));
    g_set_zone = reinterpret_cast<void (*)(int)>(game->find_function("?setCurrentPlacementZoneId@MR@@YAXH@Z"));
    g_clear_zone = reinterpret_cast<void (*)()>(game->find_function("?clearCurrentPlacementZoneId@MR@@YAXXZ"));
    static const char* const allocate_draw_lists[] = {"?allocateDrawBufferActorList@SceneFunction@@SAXXZ", nullptr};
    if (g_get_creator != nullptr && g_init_without_iter != nullptr && g_make_dead != nullptr) {
        install(allocate_draw_lists, &hook_allocate_draw_lists, &orig_allocate_draw_lists, "SceneFunction::allocateDrawBufferActorList");
    } else {
        log("enemy traps need functions this game doesn't have: they do nothing");
    }
    g_base_mtx = reinterpret_cast<ActorCall>(game->find_function("?calcAndSetBaseMtx@LiveActor@@UEAAXXZ"));
    g_is_exist_rail = reinterpret_cast<ActorTest>(game->find_function("?isExistRail@MR@@YA_NPEBVLiveActor@@@Z"));
    if (g_is_exist_rail != nullptr) {
        static const char* const rail_start[] = {"?moveCoordAndTransToRailStartPoint@MR@@YAXPEAVLiveActor@@@Z", nullptr};
        static const char* const hide_calc_anim[] = {"?hideModelAndOnCalcAnim@MR@@YAXPEAVLiveActor@@@Z", nullptr};
        install(rail_start, &hook_rail_start, &orig_rail_start, "MR::moveCoordAndTransToRailStartPoint");
        install(hide_calc_anim, &hook_hide_model_calc_anim, &orig_hide_model_calc_anim, "MR::hideModelAndOnCalcAnim");
    }
    g_change_item_status = reinterpret_cast<void (*)(int)>(game->find_function("?changeItemStatus@MarioAccess@@YAXH@Z"));
    g_can_change_state = reinterpret_cast<bool (*)(NebulaActor*)>(
        game->find_function("?isEnableNerveChange@MarioActor@@QEBA_NXZ"));
    g_is_clipped = reinterpret_cast<ActorTest>(game->find_function("?isClipped@MR@@YA_NPEBVLiveActor@@@Z"));
    g_is_hidden_model = reinterpret_cast<ActorTest>(game->find_function("?isHiddenModel@MR@@YA_NPEBVLiveActor@@@Z"));
    g_show_model = reinterpret_cast<ActorCall>(game->find_function("?showModel@MR@@YAXPEAVLiveActor@@@Z"));
    static const char* const swing_permission[] = {"?setPlayerSwingPermission@MR@@YAX_N@Z", nullptr};
    install(swing_permission, &hook_swing_permission, &orig_swing_permission, "MR::setPlayerSwingPermission");
    picture_book_kill = reinterpret_cast<void (*)(void*)>(game->find_function("?kill@ProloguePictureBook@@UEAAXXZ"));
    letter_kill = reinterpret_cast<void (*)(void*)>(game->find_function("?kill@PrologueLetter@@UEAAXXZ"));
    if (picture_book_kill != nullptr) {
        install(picture_book_appear, &hook_picture_book_appear, &orig_picture_book_appear, "ProloguePictureBook::appear");
    }
    if (letter_kill != nullptr) {
        install(letter_appear, &hook_letter_appear, &orig_letter_appear, "PrologueLetter::appear");
    }
    install(view_complete, &hook_view_complete, &orig_view_complete, "UserFile::isViewCompleteEnding");
    install(appear_luigi, &hook_appear_luigi, &orig_appear_luigi, "FileSelector::isUserFileAppearLuigi");
    file_is_luigi = reinterpret_cast<bool (*)(const void*, int)>(game->find_function("?isUserFileLuigi@FileSelector@@QEBA_NH@Z"));
    current_stage_name = reinterpret_cast<const char* (*)()>(game->find_function("?getCurrentStageName@MR@@YAPEBDXZ"));
    if (current_stage_name != nullptr) {
        install(set_scenario, &hook_set_scenario, &orig_set_scenario, "MR::setCurrentScenarioNo");
    }

    static const char* const skip_update[] = {"?update@NebulaCutsceneSkip@@YAXXZ", nullptr};
    static const char* const pressed_pause[] = {"?isPressedPause@MR@@YA_NH@Z", nullptr};
    static const char* const held_pause[] = {"?isHeldPause@MR@@YA_NH@Z", nullptr};
    static const char* const appear_yes_no[] = {"?appearYesNoWithText@SysInfoWindow@@QEAAXPEB_S@Z", nullptr};
    static const char* const selected_yes[] = {"?isSelectedYes@SysInfoWindow@@QEBA_NXZ", nullptr};
    static const char* const start_movie[] = {"?startMoviePlayer@MR@@YAXPEBD@Z", nullptr};
    skip_is_skipping = reinterpret_cast<bool (*)()>(game->find_function("?isSkipping@NebulaCutsceneSkip@@YA_NXZ"));
    movie_is_active = reinterpret_cast<bool (*)()>(game->find_function("?isActiveMoviePlayer@MR@@YA_NXZ"));
    opening_camera_is_on =
        reinterpret_cast<bool (*)()>(game->find_function("?isExecScenarioOpeningCamera@GameSceneFunction@@YA_NXZ"));
    // All or none: half of them would press + with nothing to answer the question.
    const bool skip_hooks = game->find_function(skip_update[0]) != nullptr && game->find_function(pressed_pause[0]) != nullptr &&
                            game->find_function(held_pause[0]) != nullptr && game->find_function(appear_yes_no[0]) != nullptr &&
                            game->find_function(selected_yes[0]) != nullptr && game->find_function(start_movie[0]) != nullptr;
    if (skip_hooks) {
        install(pressed_pause, &hook_is_pressed_pause, &orig_is_pressed_pause, "MR::isPressedPause");
        install(held_pause, &hook_is_held_pause, &orig_is_held_pause, "MR::isHeldPause");
        install(appear_yes_no, &hook_appear_yes_no, &orig_appear_yes_no, "SysInfoWindow::appearYesNoWithText");
        install(selected_yes, &hook_is_selected_yes, &orig_is_selected_yes, "SysInfoWindow::isSelectedYes");
        install(start_movie, &hook_start_movie, &orig_start_movie, "MR::startMoviePlayer");
        if (orig_is_pressed_pause == nullptr || orig_is_held_pause == nullptr || orig_appear_yes_no == nullptr ||
            orig_is_selected_yes == nullptr || orig_start_movie == nullptr ||
            !install(skip_update, &hook_skip_update, &orig_skip_update, "NebulaCutsceneSkip::update")) {
            orig_skip_update = nullptr;  // cutscene skipping stays off
        }
    } else {
        log("this Nebula has no cutscene skip to drive: Skip Cutscenes does nothing");
    }

    game->subscribe(NEBULA_MOD_EVENT_FRAME, on_frame, nullptr);
    game->subscribe(NEBULA_MOD_EVENT_STAGE_START, on_stage_start, nullptr);
    game->subscribe(NEBULA_MOD_EVENT_STAGE_FRAME, on_stage_frame, nullptr);
    game->subscribe(NEBULA_MOD_EVENT_STAGE_END, on_stage_end, nullptr);
    game->subscribe(NEBULA_MOD_EVENT_STAR_GET, on_star_get, nullptr);
    game->subscribe(NEBULA_MOD_EVENT_GAME_SAVE, on_game_save, nullptr);
    game->subscribe(NEBULA_MOD_EVENT_PLAYER_MISS, on_player_miss, nullptr);
    game->subscribe(NEBULA_MOD_EVENT_PLAYER_DAMAGE, on_player_damage, nullptr);
    game->subscribe(NEBULA_MOD_EVENT_POWER_UP, on_power_up, nullptr);
    log(std::string("started ") + kVersion + (g_seed.empty() ? ", no seed yet" : ", seed " + g_seed) + ", port " +
        std::to_string(g_port));
}
