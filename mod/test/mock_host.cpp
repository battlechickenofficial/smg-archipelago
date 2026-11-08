// A stand-in for Nebula, to test the plugin without the game: loads the DLL, gives it a mock NebulaModApi, and runs
// events read from stdin. Prints every call that changes the game as "CALL ...".
//
//   frames <n>                 run n menu frames (the network runs in these), 16 ms apart
//   start <stage> <mission>    a stage starts (then stage frames run with `stage <n>`)
//   stage <n>                  run n stage frames (also menu frames)
//   star <galaxy> <n>          the player wins a star (the game marks it owned, then fires STAR_GET)
//   end                        the stage ends
//   luigi <0|1>                playing as Luigi
//   flag <name> <0|1>          a game flag's own answer
//   miss                       the player dies
//   owned                      print every star owned
//   power                      print the power-up
//   timer                      the power-up's time runs out (the game asks for none)
//   hit                        the player is hit and loses the power-up
//   reset                      a new stage's load resets the power-up
//   quit
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <nebula_mod_api.h>

#include <cstdio>
#include <fstream>
#include <iostream>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace {

struct Sub {
    NebulaModCallback callback;
    void* user;
};
std::vector<Sub> subs[NEBULA_MOD_EVENT_COUNT];
std::set<std::pair<std::string, int>> owned;
std::map<std::string, int> flags;
std::map<std::string, std::string> text_store;
std::map<std::string, int> number_store;
std::string store_path;
int luigi = 0;
bool in_stage = false;
std::string stage;
int power = 0;

void save_store() {
    std::ofstream out(store_path, std::ios::trunc | std::ios::binary);
    for (auto& [k, v] : number_store) out << "N\t" << k << "\t" << v << "\n";
    for (auto& [k, v] : text_store) {
        std::string escaped;
        for (char ch : v) {
            if (ch == '\n') escaped += "\\n";
            else if (ch == '\\') escaped += "\\\\";
            else escaped += ch;
        }
        out << "T\t" << k << "\t" << escaped << "\n";
    }
}

void load_store() {
    std::ifstream in(store_path, std::ios::binary);
    std::string line;
    while (std::getline(in, line)) {
        const size_t a = line.find('\t'), b = line.find('\t', a + 1);
        if (a == std::string::npos || b == std::string::npos) continue;
        const std::string kind = line.substr(0, a), key = line.substr(a + 1, b - a - 1), value = line.substr(b + 1);
        if (kind == "N") number_store[key] = std::stoi(value);
        else {
            std::string text;
            for (size_t i = 0; i < value.size(); ++i) {
                if (value[i] == '\\' && i + 1 < value.size()) {
                    ++i;
                    text += value[i] == 'n' ? '\n' : value[i];
                } else {
                    text += value[i];
                }
            }
            text_store[key] = text;
        }
    }
}

int32_t fire(NebulaModEvent event, int32_t value, const char* name) {
    NebulaModEventData data{value, nullptr, name};
    for (const Sub& sub : subs[event]) data.value = sub.callback(&data, sub.user);
    return data.value;
}

NebulaActor* const kPlayer = reinterpret_cast<NebulaActor*>(0x1);

void m_log(const NebulaPlugin*, const char* text) { std::printf("LOG %s\n", text); }
const char* m_option(const NebulaPlugin*, const char* id) {
    if (std::string(id) == "port") return "43779";
    return nullptr;
}
int32_t m_subscribe(NebulaModEvent event, NebulaModCallback callback, void* user) {
    subs[event].push_back({callback, user});
    return 0;
}
int32_t m_hook(void*, void*, void**) { return -1; }
void* m_find(const char*) { return nullptr; }
NebulaActor* m_player() { return in_stage ? kPlayer : nullptr; }
int32_t m_value(NebulaModValue what) {
    switch (what) {
    case NEBULA_VALUE_POWER_STARS: return static_cast<int32_t>(owned.size());
    case NEBULA_VALUE_LUIGI: return luigi;
    case NEBULA_VALUE_LIFE: return 3;
    case NEBULA_VALUE_MAX_LIFE: return 3;
    case NEBULA_VALUE_STAR_BITS: return 50;
    case NEBULA_VALUE_POWER_UP: return power;
    default: return 0;
    }
}
// As MarioActor::setPlayerMode: the event says what the mode becomes.
void set_mode(int mode) { power = fire(NEBULA_MOD_EVENT_POWER_UP, mode, nullptr); }
void m_set_value(NebulaModSet what, int32_t value) {
    std::printf("CALL set_value %d %d\n", what, value);
    if (what == NEBULA_SET_POWER_UP) set_mode(value);
}
NebulaActor* m_spawn(const char* name, const float* at) {
    std::printf("CALL spawn %s %.0f %.0f %.0f\n", name, at[0], at[1], at[2]);
    return kPlayer;
}
void m_get_position(NebulaActor*, float* xyz) { xyz[0] = 100.0f; xyz[1] = 0.0f; xyz[2] = 0.0f; }
void m_get_gravity(NebulaActor*, float* xyz) { xyz[0] = 0.0f; xyz[1] = -1.0f; xyz[2] = 0.0f; }
int32_t m_set_constant(int32_t character, const char* name, int32_t, float value) {
    std::printf("CALL set_player_constant %d %s %.0f\n", character, name, value);
    return 0;
}
float m_constant(int32_t, const char*, int32_t) { return 1200.0f; }
void m_kill_player() { std::printf("CALL kill_player\n"); fire(NEBULA_MOD_EVENT_PLAYER_MISS, 0, nullptr); }
void m_change_stage(const char* galaxy, int32_t mission) { std::printf("CALL change_stage %s %d\n", galaxy, mission); }
void m_back_to_dome() { std::printf("CALL back_to_dome\n"); }
int32_t m_is_flag_on(const char* name) {
    const auto it = flags.find(name);
    return it != flags.end() ? it->second : 0;
}
int32_t m_has_star(const char* galaxy, int32_t star) { return owned.count({galaxy, star}) ? 1 : 0; }
void m_set_star(const char* galaxy, int32_t star, int32_t have) {
    std::printf("CALL set_star %s %d %d\n", galaxy, star, have);
    if (have) {
        owned.insert({galaxy, star});
        fire(NEBULA_MOD_EVENT_STAR_GET, star, galaxy);  // as the game does
    } else {
        owned.erase({galaxy, star});
    }
}
void m_show_message(const char* text, int32_t) { std::printf("CALL show_message %s\n", text); }
void m_hide_message() {}
int32_t m_store_get(const NebulaPlugin*, const char* key, int32_t missing) {
    const auto it = number_store.find(key);
    return it != number_store.end() ? it->second : missing;
}
void m_store_set(const NebulaPlugin*, const char* key, int32_t value) { number_store[key] = value; save_store(); }
int32_t m_store_get_text(const NebulaPlugin*, const char* key, char* out, int32_t size) {
    const auto it = text_store.find(key);
    if (it == text_store.end()) { out[0] = 0; return 0; }
    const int32_t n = std::min<int32_t>(size - 1, static_cast<int32_t>(it->second.size()));
    std::memcpy(out, it->second.data(), static_cast<size_t>(n));
    out[n] = 0;
    return n;
}
void m_store_set_text(const NebulaPlugin*, const char* key, const char* text) { text_store[key] = text; save_store(); }
int32_t m_cutscene() { return 0; }

}  // namespace

int main(int argc, char** argv) {
    if (argc < 3) {
        std::fprintf(stderr, "mock_host <plugin.dll> <store file>\n");
        return 2;
    }
    setvbuf(stdout, nullptr, _IONBF, 0);
    store_path = argv[2];
    load_store();
    HMODULE dll = LoadLibraryA(argv[1]);
    if (!dll) {
        std::fprintf(stderr, "cannot load %s\n", argv[1]);
        return 1;
    }
    auto start = reinterpret_cast<void (*)(const NebulaModApi*, const NebulaPlugin*)>(GetProcAddress(dll, "nebula_plugin_start"));
    static NebulaModApi api{};
    api.size = sizeof(api);
    api.version = NEBULA_MOD_API_VERSION;
    api.log = m_log;
    api.option = m_option;
    api.subscribe = m_subscribe;
    api.hook = m_hook;
    api.find_function = m_find;
    api.player = m_player;
    api.value = m_value;
    api.set_value = m_set_value;
    api.kill_player = m_kill_player;
    api.change_stage = m_change_stage;
    api.back_to_dome = m_back_to_dome;
    api.is_event_flag_on = m_is_flag_on;
    api.has_star = m_has_star;
    api.set_star = m_set_star;
    api.show_message = m_show_message;
    api.hide_message = m_hide_message;
    api.store_get = m_store_get;
    api.store_set = m_store_set;
    api.store_get_text = m_store_get_text;
    api.store_set_text = m_store_set_text;
    api.is_cutscene_playing = m_cutscene;
    api.spawn = m_spawn;
    api.get_position = m_get_position;
    api.get_gravity = m_get_gravity;
    api.set_player_constant = m_set_constant;
    api.player_constant = m_constant;
    static NebulaPlugin self{sizeof(NebulaPlugin), "archipelago", "0.1.0"};
    start(&api, &self);

    std::string line;
    while (std::getline(std::cin, line)) {
        std::istringstream in(line);
        std::string cmd;
        in >> cmd;
        if (cmd == "frames" || cmd == "stage") {
            int n = 0;
            in >> n;
            for (int i = 0; i < n; ++i) {
                fire(NEBULA_MOD_EVENT_FRAME, 0, nullptr);
                if (cmd == "stage" && in_stage) fire(NEBULA_MOD_EVENT_STAGE_FRAME, 0, nullptr);
                Sleep(16);
            }
        } else if (cmd == "start") {
            int mission = 1;
            in >> stage >> mission;
            in_stage = true;
            fire(NEBULA_MOD_EVENT_STAGE_START, mission, stage.c_str());
        } else if (cmd == "star") {
            std::string galaxy;
            int n = 0;
            in >> galaxy >> n;
            owned.insert({galaxy, n});
            fire(NEBULA_MOD_EVENT_STAR_GET, n, galaxy.c_str());
        } else if (cmd == "end") {
            fire(NEBULA_MOD_EVENT_STAGE_END, 0, stage.c_str());
            in_stage = false;
        } else if (cmd == "luigi") {
            in >> luigi;
        } else if (cmd == "flag") {
            std::string name;
            int on = 0;
            in >> name >> on;
            flags[name] = on;
        } else if (cmd == "miss") {
            fire(NEBULA_MOD_EVENT_PLAYER_MISS, 0, nullptr);
        } else if (cmd == "power") {
            std::printf("POWER %d\n", power);
        } else if (cmd == "timer") {
            set_mode(0);
        } else if (cmd == "hit") {
            fire(NEBULA_MOD_EVENT_PLAYER_DAMAGE, 1, nullptr);
            set_mode(0);
        } else if (cmd == "reset") {
            power = 0;
        } else if (cmd == "owned") {
            for (auto& [g, n] : owned) std::printf("OWNED %s %d\n", g.c_str(), n);
        } else if (cmd == "quit") {
            std::printf("DONE quit\n");
            break;
        }
        std::printf("DONE %s\n", cmd.c_str());
    }
    return 0;
}
