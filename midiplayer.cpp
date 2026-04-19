/**
 * midi-hw-player  —  CLI MIDI Player for Hardware Synths
 *
 * Features:
 *   - ALSA sequencer output (USB-MIDI hardware)
 *   - SMF Type 0 / 1 / 2 parser (self-contained, no external lib)
 *   - GM / GS / XG SysEx initialization
 *   - Playlist (sequential / shuffle / repeat-one / repeat-all)
 *   - Interactive TUI via ncurses (play/pause/stop/next/prev/quit)
 *   - Tempo change (BPM ±5)
 *   - Volume master control
 *   - Port selection at startup
 *
 * Build:
 *   g++ -std=c++17 -O2 -o midi-hw-player midiplayer.cpp -lasound -lncursesw -lpthread
 */

#include <alsa/asoundlib.h>
#include <ncurses.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <optional>
#include <random>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace fs = std::filesystem;
using namespace std::chrono;

// ─────────────────────────────────────────────────────────────────────────────
// SMF Parser
// ─────────────────────────────────────────────────────────────────────────────

struct MidiEvent {
    uint32_t tick;      // absolute tick
    uint8_t  type;      // 0x80-0xEF: channel, 0xF0/0xF7: sysex, 0xFF: meta
    uint8_t  data[512]; // raw bytes (type byte inclusive for sysex/meta)
    uint32_t len;       // total bytes in data[]
};

struct SmfFile {
    uint16_t format;
    uint16_t num_tracks;
    uint16_t division;           // ticks/quarter or SMPTE
    uint32_t initial_tempo_us;   // microseconds per quarter note
    std::vector<MidiEvent> events; // merged, sorted by tick
};

static uint16_t read_u16be(const uint8_t* p) { return (p[0]<<8)|p[1]; }
static uint32_t read_u32be(const uint8_t* p) {
    return ((uint32_t)p[0]<<24)|((uint32_t)p[1]<<16)|((uint32_t)p[2]<<8)|p[3];
}
static uint32_t read_vlq(const uint8_t* data, size_t size, size_t& pos) {
    uint32_t v = 0;
    for(int i=0;i<4&&pos<size;++i){
        uint8_t b = data[pos++];
        v = (v<<7)|(b&0x7F);
        if(!(b&0x80)) break;
    }
    return v;
}

static bool parse_track(const uint8_t* trk, uint32_t trk_len,
                        std::vector<MidiEvent>& out, uint32_t& out_initial_tempo)
{
    size_t pos = 0;
    uint32_t tick = 0;
    uint8_t running_status = 0;

    while(pos < trk_len) {
        uint32_t delta = read_vlq(trk, trk_len, pos);
        tick += delta;

        if(pos >= trk_len) break;
        uint8_t st = trk[pos];

        MidiEvent ev{};
        ev.tick = tick;

        // SysEx
        if(st == 0xF0 || st == 0xF7) {
            running_status = 0;
            pos++;
            uint32_t slen = read_vlq(trk, trk_len, pos);
            ev.type = st;
            ev.data[0] = st;
            uint32_t copy = std::min(slen, (uint32_t)(sizeof(ev.data)-1));
            memcpy(ev.data+1, trk+pos, copy);
            ev.len = 1 + copy;
            pos += slen;
            out.push_back(ev);
            continue;
        }

        // Meta
        if(st == 0xFF) {
            running_status = 0;
            pos++;
            if(pos >= trk_len) break;
            uint8_t meta_type = trk[pos++];
            uint32_t mlen = read_vlq(trk, trk_len, pos);
            ev.type = 0xFF;
            ev.data[0] = 0xFF;
            ev.data[1] = meta_type;
            uint32_t copy = std::min(mlen, (uint32_t)(sizeof(ev.data)-2));
            memcpy(ev.data+2, trk+pos, copy);
            ev.len = 2 + copy;
            // Extract initial tempo from first Set Tempo on track 0
            if(meta_type == 0x51 && mlen >= 3 && out_initial_tempo == 500000) {
                out_initial_tempo = ((uint32_t)trk[pos]<<16)|
                                    ((uint32_t)trk[pos+1]<<8)|trk[pos+2];
            }
            pos += mlen;
            out.push_back(ev);
            continue;
        }

        // Channel events
        uint8_t status;
        if(st & 0x80) { status = st; running_status = st; pos++; }
        else          { status = running_status; }

        uint8_t cmd = status & 0xF0;
        ev.type = status;
        ev.data[0] = status;

        switch(cmd) {
            case 0x80: case 0x90: case 0xA0: case 0xB0: case 0xE0:
                if(pos+1 < trk_len) {
                    ev.data[1]=trk[pos]; ev.data[2]=trk[pos+1];
                    ev.len=3; pos+=2;
                }
                break;
            case 0xC0: case 0xD0:
                if(pos < trk_len) {
                    ev.data[1]=trk[pos]; ev.len=2; pos++;
                }
                break;
            case 0xF0:
                // Single-byte system realtime etc.
                ev.len=1;
                break;
            default:
                ev.len=1;
                break;
        }
        out.push_back(ev);
    }
    return true;
}

static std::optional<SmfFile> load_smf(const std::string& path)
{
    std::ifstream f(path, std::ios::binary);
    if(!f) return std::nullopt;

    std::vector<uint8_t> buf((std::istreambuf_iterator<char>(f)),
                              std::istreambuf_iterator<char>());
    if(buf.size() < 14) return std::nullopt;

    if(memcmp(buf.data(), "MThd", 4) != 0) return std::nullopt;
    uint32_t hdr_len = read_u32be(buf.data()+4);
    if(hdr_len < 6) return std::nullopt;

    SmfFile smf{};
    smf.format      = read_u16be(buf.data()+8);
    smf.num_tracks  = read_u16be(buf.data()+10);
    smf.division    = read_u16be(buf.data()+12);
    smf.initial_tempo_us = 500000; // default 120 BPM

    size_t pos = 8 + hdr_len;
    for(uint16_t t = 0; t < smf.num_tracks && pos+8 <= buf.size(); ++t) {
        if(memcmp(buf.data()+pos, "MTrk", 4) != 0) break;
        uint32_t trk_len = read_u32be(buf.data()+pos+4);
        pos += 8;
        if(pos + trk_len > buf.size()) break;

        std::vector<MidiEvent> trk_events;
        uint32_t tempo_holder = smf.initial_tempo_us;
        parse_track(buf.data()+pos, trk_len, trk_events, tempo_holder);
        if(t == 0) smf.initial_tempo_us = tempo_holder;

        for(auto& ev : trk_events)
            smf.events.push_back(ev);

        pos += trk_len;
    }

    // Sort: by tick, then meta < sysex < channel
    std::stable_sort(smf.events.begin(), smf.events.end(),
        [](const MidiEvent& a, const MidiEvent& b){
            if(a.tick != b.tick) return a.tick < b.tick;
            // meta first (tempo changes before notes)
            auto pri = [](uint8_t t){ return t==0xFF?0:(t==0xF0||t==0xF7)?1:2; };
            return pri(a.type) < pri(b.type);
        });

    return smf;
}

// ─────────────────────────────────────────────────────────────────────────────
// ALSA MIDI Output
// ─────────────────────────────────────────────────────────────────────────────

struct AlsaOut {
    snd_seq_t*    seq    = nullptr;
    int           port   = -1;
    int           client = -1;
    int           dest_client = -1;
    int           dest_port   = -1;

    bool open(int dclient, int dport) {
        if(snd_seq_open(&seq, "default", SND_SEQ_OPEN_OUTPUT, 0) < 0) return false;
        snd_seq_set_client_name(seq, "midi-hw-player");
        port = snd_seq_create_simple_port(seq, "output",
                    SND_SEQ_PORT_CAP_READ|SND_SEQ_PORT_CAP_SUBS_READ,
                    SND_SEQ_PORT_TYPE_MIDI_GENERIC|SND_SEQ_PORT_TYPE_APPLICATION);
        if(port < 0) return false;
        client = snd_seq_client_id(seq);
        dest_client = dclient;
        dest_port   = dport;
        snd_seq_connect_to(seq, port, dclient, dport);
        return true;
    }

    void close() {
        if(seq) { snd_seq_close(seq); seq=nullptr; }
    }

    void send_raw(const uint8_t* data, uint32_t len) {
        if(!seq || len == 0) return;
        uint8_t st = data[0];

        if(st == 0xF0) {
            // SysEx
            snd_seq_event_t ev{};
            snd_seq_ev_clear(&ev);
            snd_seq_ev_set_sysex(&ev, len, (void*)data);
            snd_seq_ev_set_source(&ev, port);
            snd_seq_ev_set_dest(&ev, dest_client, dest_port);
            snd_seq_ev_set_direct(&ev);
            snd_seq_event_output_direct(seq, &ev);
            return;
        }

        if((st & 0xF0) == 0x80 || (st & 0xF0) == 0x90) {
            snd_seq_event_t ev{};
            snd_seq_ev_clear(&ev);
            if((st & 0xF0) == 0x90 && len>=3 && data[2]>0)
                snd_seq_ev_set_noteon(&ev, st&0x0F, data[1], data[2]);
            else
                snd_seq_ev_set_noteoff(&ev, st&0x0F, data[1], 0);
            snd_seq_ev_set_source(&ev, port);
            snd_seq_ev_set_dest(&ev, dest_client, dest_port);
            snd_seq_ev_set_direct(&ev);
            snd_seq_event_output_direct(seq, &ev);
            return;
        }

        // Generic: encode as bytes
        snd_seq_event_t ev{};
        snd_seq_ev_clear(&ev);
        ev.type = SND_SEQ_EVENT_SYSEX;
        snd_seq_ev_set_variable(&ev, len, (void*)data);
        snd_seq_ev_set_source(&ev, port);
        snd_seq_ev_set_dest(&ev, dest_client, dest_port);
        snd_seq_ev_set_direct(&ev);
        snd_seq_event_output_direct(seq, &ev);
    }

    // All Notes Off + Reset Controllers on all channels
    void all_notes_off() {
        if(!seq) return;
        for(int ch=0; ch<16; ++ch) {
            // CC 123: All Notes Off
            snd_seq_event_t ev{};
            snd_seq_ev_clear(&ev);
            snd_seq_ev_set_controller(&ev, ch, 123, 0);
            snd_seq_ev_set_source(&ev, port);
            snd_seq_ev_set_dest(&ev, dest_client, dest_port);
            snd_seq_ev_set_direct(&ev);
            snd_seq_event_output_direct(seq, &ev);
        }
        snd_seq_drain_output(seq);
    }

    // Send GM/GS/XG init SysEx
    void send_gm_reset() {
        // GM System On
        static const uint8_t gm_on[] = {0xF0,0x7E,0x7F,0x09,0x01,0xF7};
        send_raw(gm_on, sizeof(gm_on));
    }
    void send_gs_reset() {
        // Roland GS Reset
        static const uint8_t gs_rst[] = {0xF0,0x41,0x10,0x42,0x12,0x40,0x00,0x7F,0x00,0x41,0xF7};
        send_raw(gs_rst, sizeof(gs_rst));
    }
    void send_xg_reset() {
        // Yamaha XG System On
        static const uint8_t xg_on[]  = {0xF0,0x43,0x10,0x4C,0x00,0x00,0x7E,0x00,0xF7};
        send_raw(xg_on, sizeof(xg_on));
    }
};

// List available MIDI output ports
struct PortInfo { int client; int port; std::string name; };
static std::vector<PortInfo> list_midi_ports() {
    std::vector<PortInfo> ports;
    snd_seq_t* seq;
    if(snd_seq_open(&seq,"default",SND_SEQ_OPEN_OUTPUT,0)<0) return ports;

    snd_seq_client_info_t* cinfo;
    snd_seq_port_info_t*   pinfo;
    snd_seq_client_info_alloca(&cinfo);
    snd_seq_port_info_alloca(&pinfo);

    snd_seq_client_info_set_client(cinfo, -1);
    while(snd_seq_query_next_client(seq,cinfo)>=0) {
        int c = snd_seq_client_info_get_client(cinfo);
        snd_seq_port_info_set_client(pinfo, c);
        snd_seq_port_info_set_port(pinfo, -1);
        while(snd_seq_query_next_port(seq,pinfo)>=0) {
            uint32_t caps = snd_seq_port_info_get_capability(pinfo);
            if((caps & SND_SEQ_PORT_CAP_WRITE) &&
               !(caps & SND_SEQ_PORT_CAP_NO_EXPORT)) {
                PortInfo pi;
                pi.client = c;
                pi.port   = snd_seq_port_info_get_port(pinfo);
                pi.name   = std::string(snd_seq_client_info_get_name(cinfo))
                            + ":" + snd_seq_port_info_get_name(pinfo);
                ports.push_back(pi);
            }
        }
    }
    snd_seq_close(seq);
    return ports;
}

// ─────────────────────────────────────────────────────────────────────────────
// Playlist
// ─────────────────────────────────────────────────────────────────────────────

enum class RepeatMode { None, One, All };
enum class PlayState  { Stopped, Playing, Paused };

struct Playlist {
    std::vector<std::string> files;
    std::vector<int>         order; // play order indices
    int                      pos = 0;
    bool                     shuffle = false;
    RepeatMode               repeat  = RepeatMode::None;

    void build_order() {
        order.resize(files.size());
        std::iota(order.begin(), order.end(), 0);
        if(shuffle) {
            std::mt19937 rng(std::random_device{}());
            std::shuffle(order.begin(), order.end(), rng);
        }
    }

    std::string current() const {
        if(order.empty()) return "";
        return files[order[pos]];
    }

    bool next() {
        if(repeat == RepeatMode::One) return true;
        if(pos+1 < (int)order.size()) { pos++; return true; }
        if(repeat == RepeatMode::All) { pos=0; return true; }
        return false;
    }

    bool prev() {
        if(pos > 0) { pos--; return true; }
        if(repeat == RepeatMode::All) { pos=(int)order.size()-1; return true; }
        return false;
    }
};

// ─────────────────────────────────────────────────────────────────────────────
// Player Engine  (runs in a background thread)
// ─────────────────────────────────────────────────────────────────────────────

struct PlayerEngine {
    AlsaOut     alsa;
    Playlist    playlist;

    std::atomic<PlayState> state{PlayState::Stopped};
    std::atomic<bool>      cmd_next{false};
    std::atomic<bool>      cmd_prev{false};
    std::atomic<bool>      cmd_stop{false};
    std::atomic<bool>      quit   {false};

    std::atomic<double>    tempo_factor{1.0}; // 1.0 = original tempo
    std::atomic<int>       master_volume{100}; // 0-127

    // Current file info (read by UI thread)
    std::mutex   info_mtx;
    std::string  current_file;
    double       progress  = 0.0; // 0.0 - 1.0
    uint32_t     cur_tick  = 0;
    uint32_t     total_tick= 0;
    uint32_t     cur_bpm   = 120;
    std::string  song_title;

    std::thread  thread;

    void start() {
        thread = std::thread([this]{ run(); });
    }
    void stop_and_join() {
        quit = true;
        cmd_stop = true;
        if(thread.joinable()) thread.join();
        alsa.all_notes_off();
    }

    void run() {
        while(!quit) {
            std::string file;
            {
                std::lock_guard<std::mutex> lk(info_mtx);
                file = playlist.current();
                current_file = file;
                song_title   = fs::path(file).stem().string();
                progress = 0; cur_tick = 0; total_tick = 0;
            }

            if(file.empty()) { std::this_thread::sleep_for(50ms); continue; }

            auto smf_opt = load_smf(file);
            if(!smf_opt) {
                // Skip bad file
                if(!playlist.next()) state = PlayState::Stopped;
                continue;
            }

            SmfFile& smf = *smf_opt;

            // Compute total ticks
            uint32_t max_tick = 0;
            for(auto& ev : smf.events) max_tick = std::max(max_tick, ev.tick);
            {
                std::lock_guard<std::mutex> lk(info_mtx);
                total_tick = max_tick;
            }

            state = PlayState::Playing;
            cmd_stop = cmd_next = cmd_prev = false;

            uint32_t tempo_us = smf.initial_tempo_us; // current tempo (µs/qn)
            uint32_t last_tick = 0;
            auto     last_time = steady_clock::now();

            for(size_t i = 0; i < smf.events.size() && !quit; ) {
                // Handle commands
                if(cmd_stop) { state=PlayState::Stopped; goto song_end; }
                if(cmd_next) { cmd_next=false; goto song_end; }
                if(cmd_prev) {
                    cmd_prev=false;
                    // re-queue prev
                    playlist.prev(); playlist.prev(); // undo the advance we'll do
                    goto song_end;
                }

                // Pause loop
                while(state == PlayState::Paused && !quit && !cmd_stop && !cmd_next && !cmd_prev) {
                    std::this_thread::sleep_for(20ms);
                    last_time = steady_clock::now(); // don't drift on resume
                }
                if(cmd_stop||cmd_next||cmd_prev||quit) {
                    if(cmd_stop) state=PlayState::Stopped;
                    goto song_end;
                }

                auto& ev = smf.events[i];

                // Compute wait time for this event's tick
                if(ev.tick > last_tick) {
                    uint32_t dtick = ev.tick - last_tick;
                    double   tf    = tempo_factor.load();
                    // tempo_us / ticks_per_qn = µs per tick
                    double   us_per_tick = (double)tempo_us / smf.division / tf;
                    double   wait_us     = dtick * us_per_tick;

                    auto target = last_time + microseconds((long long)wait_us);
                    auto now    = steady_clock::now();
                    if(target > now) std::this_thread::sleep_until(target);

                    last_time = target;
                    last_tick = ev.tick;
                    {
                        std::lock_guard<std::mutex> lk(info_mtx);
                        cur_tick = ev.tick;
                        progress = total_tick>0?(double)ev.tick/total_tick:0;
                        cur_bpm  = (uint32_t)(60000000.0/(tempo_us/tf));
                    }
                }

                // Meta: Tempo change
                if(ev.type == 0xFF && ev.len >= 5 && ev.data[1] == 0x51) {
                    tempo_us = ((uint32_t)ev.data[2]<<16)|
                               ((uint32_t)ev.data[3]<<8)|ev.data[4];
                    ++i; continue;
                }

                // Skip other meta / end of track
                if(ev.type == 0xFF) { ++i; continue; }

                // Apply master volume to Note On velocity
                uint8_t buf[512];
                memcpy(buf, ev.data, ev.len);
                uint32_t send_len = ev.len;

                if((buf[0] & 0xF0) == 0x90 && send_len >= 3 && buf[2] > 0) {
                    int mv  = master_volume.load();
                    buf[2]  = (uint8_t)std::min(127, (int)buf[2]*mv/100);
                }

                alsa.send_raw(buf, send_len);
                ++i;
            }

            song_end:
            alsa.all_notes_off();

            if(!cmd_stop && !quit) {
                if(!playlist.next()) {
                    state = PlayState::Stopped;
                }
                // else continue loop → next file
            }
        }
    }
};

// ─────────────────────────────────────────────────────────────────────────────
// TUI
// ─────────────────────────────────────────────────────────────────────────────

static std::string format_time(double progress, uint32_t /*ticks*/) {
    // We don't have wall-clock duration easily; just show percentage
    char buf[16];
    snprintf(buf, sizeof(buf), "%3.0f%%", progress*100.0);
    return buf;
}

static std::string repeat_str(RepeatMode r) {
    switch(r) {
        case RepeatMode::None: return "Off";
        case RepeatMode::One:  return "One";
        case RepeatMode::All:  return "All";
    }
    return "";
}

static void draw_ui(WINDOW* win, PlayerEngine& eng, int selected_port_name_len,
                    const std::string& port_name)
{
    (void)selected_port_name_len;
    werase(win);
    int rows, cols;
    getmaxyx(win, rows, cols);

    // Title bar
    wattron(win, A_REVERSE);
    mvwhline(win, 0, 0, ' ', cols);
    mvwprintw(win, 0, 2, " midi-hw-player ");
    wattroff(win, A_REVERSE);

    // Song info
    std::string file, title;
    double prog; uint32_t bpm; int idx, total_files;
    {
        std::lock_guard<std::mutex> lk(eng.info_mtx);
        file  = eng.current_file;
        title = eng.song_title;
        prog  = eng.progress;
        bpm   = eng.cur_bpm;
        idx   = eng.playlist.pos;
        total_files = (int)eng.playlist.files.size();
    }

    // Track info
    int row = 2;
    mvwprintw(win, row++, 2, "Port : %s", port_name.c_str());

    // Song
    wattron(win, A_BOLD);
    std::string disp = title.empty() ? "(no file)" : title;
    if((int)disp.size() > cols-14) disp = disp.substr(0, cols-17) + "...";
    mvwprintw(win, row++, 2, "Song : %s", disp.c_str());
    wattroff(win, A_BOLD);

    mvwprintw(win, row++, 2, "Track: %d / %d", idx+1, total_files);
    mvwprintw(win, row++, 2, "BPM  : %u  (x%.2f)", bpm,
              eng.tempo_factor.load());
    mvwprintw(win, row++, 2, "Vol  : %d%%   Repeat: %s  Shuffle: %s",
              eng.master_volume.load(),
              repeat_str(eng.playlist.repeat).c_str(),
              eng.playlist.shuffle ? "On" : "Off");

    // Progress bar
    row++;
    int bar_w = cols - 14;
    if(bar_w < 5) bar_w = 5;
    int filled = (int)(prog * bar_w);
    mvwprintw(win, row, 2, "[");
    wattron(win, COLOR_PAIR(1));
    for(int i=0;i<filled;++i) waddch(win,'=');
    wattroff(win, COLOR_PAIR(1));
    for(int i=filled;i<bar_w;++i) waddch(win,'-');
    wprintw(win, "] %s", format_time(prog, 0).c_str());
    row++;

    // State
    row++;
    PlayState st = eng.state.load();
    const char* state_str = st==PlayState::Playing?"▶ Playing":
                            st==PlayState::Paused ?"⏸ Paused" :
                                                    "■ Stopped";
    wattron(win, A_BOLD|COLOR_PAIR(2));
    mvwprintw(win, row++, 2, "State: %s", state_str);
    wattroff(win, A_BOLD|COLOR_PAIR(2));

    // Playlist
    row++;
    mvwprintw(win, row++, 2, "── Playlist ──────────────────────────────");
    int max_show = rows - row - 4;
    if(max_show < 1) max_show = 1;
    int start = std::max(0, idx - max_show/2);
    int end   = std::min((int)eng.playlist.order.size(), start+max_show);
    for(int i=start; i<end && row<rows-3; ++i) {
        const auto& fname = fs::path(eng.playlist.files[eng.playlist.order[i]]).stem().string();
        std::string entry = fname;
        if((int)entry.size() > cols-8) entry = entry.substr(0,cols-11)+"...";
        if(i == idx) {
            wattron(win, A_REVERSE|A_BOLD);
            mvwprintw(win, row, 2, "▶ %s", entry.c_str());
            wattroff(win, A_REVERSE|A_BOLD);
        } else {
            mvwprintw(win, row, 2, "  %s", entry.c_str());
        }
        row++;
    }

    // Key help
    wattron(win, A_DIM);
    if(rows > 3) {
        mvwhline(win, rows-3, 0, ACS_HLINE, cols);
        mvwprintw(win, rows-2, 1,
            "SPC:Play/Pause  n/p:Next/Prev  s:Shuffle  r:Repeat  +/-:Tempo  Up/Dn:Volume  G/X/Y:Init  q:Quit");
    }
    wattroff(win, A_DIM);

    wrefresh(win);
}

// ─────────────────────────────────────────────────────────────────────────────
// main
// ─────────────────────────────────────────────────────────────────────────────

static void print_usage(const char* prog) {
    std::cerr << "Usage: " << prog << " [options] <file.mid|dir> ...\n"
              << "  -p <N>     Use port index N (skip interactive selection)\n"
              << "  -s         Shuffle playlist\n"
              << "  -r         Repeat all\n"
              << "  --gm       Send GM System On before first song\n"
              << "  --gs       Send GS Reset before first song\n"
              << "  --xg       Send XG System On before first song\n"
              << "  --vol <N>  Master volume 0-100 (default 100)\n";
}

int main(int argc, char* argv[])
{
    if(argc < 2) { print_usage(argv[0]); return 1; }

    // Parse args
    std::vector<std::string> midi_files;
    int  port_idx    = -1;
    bool do_shuffle  = false;
    bool do_repeat   = false;
    bool send_gm     = false;
    bool send_gs     = false;
    bool send_xg     = false;
    int  init_vol    = 100;

    for(int i=1;i<argc;++i) {
        std::string a = argv[i];
        if(a == "-p" && i+1<argc)         { port_idx = std::stoi(argv[++i]); }
        else if(a == "-s")                 { do_shuffle = true; }
        else if(a == "-r")                 { do_repeat = true; }
        else if(a == "--gm")               { send_gm = true; }
        else if(a == "--gs")               { send_gs = true; }
        else if(a == "--xg")               { send_xg = true; }
        else if(a == "--vol" && i+1<argc)  { init_vol = std::stoi(argv[++i]); }
        else if(a[0] != '-') {
            // File or directory
            if(fs::is_directory(a)) {
                for(auto& p : fs::directory_iterator(a)) {
                    auto ext = p.path().extension().string();
                    if(ext==".mid"||ext==".MID"||ext==".midi"||ext==".MIDI")
                        midi_files.push_back(p.path().string());
                }
                std::sort(midi_files.end() - /* count */ 0, midi_files.end()); // sort added
            } else {
                midi_files.push_back(a);
            }
        } else {
            print_usage(argv[0]); return 1;
        }
    }

    // Sort all files
    std::sort(midi_files.begin(), midi_files.end());

    if(midi_files.empty()) {
        std::cerr << "No MIDI files specified.\n";
        return 1;
    }

    // List MIDI ports
    auto ports = list_midi_ports();
    if(ports.empty()) {
        std::cerr << "No MIDI output ports found. Check your USB-MIDI connection.\n";
        return 1;
    }

    // Port selection
    if(port_idx < 0) {
        std::cout << "Available MIDI output ports:\n";
        for(int i=0;i<(int)ports.size();++i)
            std::cout << "  [" << i << "] " << ports[i].name << "\n";
        std::cout << "Select port [0]: ";
        std::string line;
        std::getline(std::cin, line);
        if(!line.empty()) port_idx = std::stoi(line);
        else              port_idx = 0;
    }
    if(port_idx<0 || port_idx>=(int)ports.size()) {
        std::cerr << "Invalid port index.\n"; return 1;
    }
    auto& sel = ports[port_idx];

    // Open ALSA
    PlayerEngine eng;
    if(!eng.alsa.open(sel.client, sel.port)) {
        std::cerr << "Failed to open ALSA port.\n"; return 1;
    }

    // Send init SysEx
    std::this_thread::sleep_for(200ms);
    if(send_gs)      { eng.alsa.send_gs_reset(); std::this_thread::sleep_for(300ms); }
    else if(send_xg) { eng.alsa.send_xg_reset(); std::this_thread::sleep_for(300ms); }
    else if(send_gm) { eng.alsa.send_gm_reset(); std::this_thread::sleep_for(300ms); }

    // Build playlist
    eng.playlist.files   = midi_files;
    eng.playlist.shuffle = do_shuffle;
    eng.playlist.repeat  = do_repeat ? RepeatMode::All : RepeatMode::None;
    eng.playlist.build_order();
    eng.master_volume    = init_vol;

    eng.start();

    // ── ncurses TUI ──
    initscr();
    cbreak(); noecho(); keypad(stdscr, TRUE);
    nodelay(stdscr, TRUE);
    curs_set(0);
    start_color();
    init_pair(1, COLOR_GREEN,  COLOR_BLACK);
    init_pair(2, COLOR_CYAN,   COLOR_BLACK);
    init_pair(3, COLOR_YELLOW, COLOR_BLACK);

    std::string port_name = sel.name;

    while(!eng.quit) {
        int ch = getch();
        switch(ch) {
            case ' ':
            {
                PlayState s = eng.state.load();
                if(s == PlayState::Playing)
                    eng.state = PlayState::Paused;
                else if(s == PlayState::Paused)
                    eng.state = PlayState::Playing;
                else {
                    // Stopped → restart
                    eng.playlist.pos = 0;
                    eng.state = PlayState::Playing;
                    eng.cmd_stop = false;
                }
                break;
            }
            case 'n': case KEY_RIGHT:
                eng.cmd_next = true;
                break;
            case 'p': case KEY_LEFT:
                eng.cmd_prev = true;
                break;
            case 's':
                eng.playlist.shuffle = !eng.playlist.shuffle;
                eng.playlist.build_order();
                eng.playlist.pos = 0;
                break;
            case 'r':
            {
                auto rm = eng.playlist.repeat;
                if(rm == RepeatMode::None)      eng.playlist.repeat = RepeatMode::All;
                else if(rm == RepeatMode::All)  eng.playlist.repeat = RepeatMode::One;
                else                            eng.playlist.repeat = RepeatMode::None;
                break;
            }
            case '+': case '=':
            {
                double tf = eng.tempo_factor.load();
                tf = std::min(2.0, tf + 0.05);
                eng.tempo_factor = tf;
                break;
            }
            case '-': case '_':
            {
                double tf = eng.tempo_factor.load();
                tf = std::max(0.25, tf - 0.05);
                eng.tempo_factor = tf;
                break;
            }
            case '0':
                eng.tempo_factor = 1.0;
                break;
            case KEY_UP:
            {
                int v = eng.master_volume.load();
                eng.master_volume = std::min(100, v+5);
                break;
            }
            case KEY_DOWN:
            {
                int v = eng.master_volume.load();
                eng.master_volume = std::max(0, v-5);
                break;
            }
            case 'G': // GM reset
                eng.alsa.send_gm_reset();
                break;
            case 'X': // XG reset
                eng.alsa.send_xg_reset();
                break;
            case 'Y': // GS reset (Y for rolandGS)
                eng.alsa.send_gs_reset();
                break;
            case 'q': case 'Q':
                eng.quit = true;
                break;
            default: break;
        }

        draw_ui(stdscr, eng, (int)port_name.size(), port_name);
        std::this_thread::sleep_for(50ms);
    }

    endwin();

    eng.stop_and_join();
    eng.alsa.close();
    return 0;
}
