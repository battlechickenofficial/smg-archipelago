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

const char* const kVersion = "0.1.0";

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
std::string g_building_text;

struct Pending {                     // an item to apply in a stage, where the player is
    int index;
    std::string kind;                // "GIVE", "TRAP" or "POWERUP"
    std::string what;
    int amount;
    std::string object;              // an enemy trap's object
};
std::deque<Pending> g_pending;
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

void client_line(const std::string& line) {
    std::vector<std::string> w = split(line, '\t');
    if (w.empty() || w[0].empty()) return;
    const std::string& cmd = w[0];
    if (cmd == "HELLO") {
        send_line(std::string("HELLO\t") + kVersion);
    } else if (cmd == "SEED" && w.size() >= 2) {
        if (w[1] != g_seed) forget_seed(w[1]);
    } else if (cmd == "BEGIN") {
        g_building = std::make_shared<Config>();
        g_building_text.clear();
    } else if (cmd == "RULE" || cmd == "LUIGIRULE" || cmd == "REDIRECT" || cmd == "KEEP" || cmd == "OPTION" ||
               cmd == "CONST") {
        config_line(w);
        g_building_text += line + "\n";
    } else if (cmd == "COMMIT") {
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
ULONGLONG g_next_try = 0;
std::string g_in;
std::string g_out;
int g_port = 43770;

void disconnect() {
    if (g_socket != INVALID_SOCKET) closesocket(g_socket);
    g_socket = INVALID_SOCKET;
    g_connecting = false;
    g_in.clear();
    g_out.clear();
    g_next_try = GetTickCount64() + 3000;
}

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
        log("connected to the Archipelago client");
        send_line(std::string("HELLO\t") + kVersion);
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
int g_spawned = 0;          // of the current group, for spreading them out
int g_spawn_wait = 0;

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

void apply_item(const Pending& p) {
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
}

// One enemy of a trap's group, dropped above the player, each a little further round a ring.
void spawn_next() {
    NebulaActor* player = game->player();
    if (player == nullptr || game->spawn == nullptr) return;
    float at[3];
    float down[3] = {0.0f, -1.0f, 0.0f};
    game->get_position(player, at);
    if (game->get_gravity != nullptr) game->get_gravity(player, down);
    const float up[3] = {-down[0], -down[1], -down[2]};
    // Two directions across "up", for the ring.
    float side[3] = {1.0f, 0.0f, 0.0f};
    if (std::fabs(up[0]) > 0.9f) {
        side[0] = 0.0f;
        side[2] = 1.0f;
    }
    float u[3] = {up[1] * side[2] - up[2] * side[1], up[2] * side[0] - up[0] * side[2], up[0] * side[1] - up[1] * side[0]};
    const float length = std::sqrt(u[0] * u[0] + u[1] * u[1] + u[2] * u[2]);
    for (float& x : u) x /= length > 0.0f ? length : 1.0f;
    const float v[3] = {up[1] * u[2] - up[2] * u[1], up[2] * u[0] - up[0] * u[2], up[0] * u[1] - up[1] * u[0]};
    const float angle = 2.39996f * static_cast<float>(g_spawned);  // the golden angle: no two land together
    const float radius = 120.0f + 25.0f * std::sqrt(static_cast<float>(g_spawned));
    const float height = 300.0f;
    float where[3];
    for (int i = 0; i < 3; ++i) {
        where[i] = at[i] + up[i] * height + (u[i] * std::cos(angle) + v[i] * std::sin(angle)) * radius;
    }
    const std::string object = g_spawns.front();
    g_spawns.pop_front();
    ++g_spawned;
    if (game->spawn(object.c_str(), where) == nullptr) log("could not drop a " + object + " here");
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
        g_pending.pop_front();
        if (p.index > g_given) {
            apply_item(p);
            g_given = p.index;
            game->store_set(me, "given", g_given);
        }
    }
    if (g_message_frames > 0) {
        if (--g_message_frames == 0) game->hide_message();
    } else if (!g_messages.empty()) {
        game->show_message(g_messages.front().c_str(), 0);
        g_messages.pop_front();
        g_message_frames = 180;
    }
    return data->value;
}

int32_t on_frame(const NebulaModEventData* data, void*) {
    pump();
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
    install(view_complete, &hook_view_complete, &orig_view_complete, "UserFile::isViewCompleteEnding");
    install(appear_luigi, &hook_appear_luigi, &orig_appear_luigi, "FileSelector::isUserFileAppearLuigi");
    file_is_luigi = reinterpret_cast<bool (*)(const void*, int)>(game->find_function("?isUserFileLuigi@FileSelector@@QEBA_NH@Z"));
    current_stage_name = reinterpret_cast<const char* (*)()>(game->find_function("?getCurrentStageName@MR@@YAPEBDXZ"));
    if (current_stage_name != nullptr) {
        install(set_scenario, &hook_set_scenario, &orig_set_scenario, "MR::setCurrentScenarioNo");
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
