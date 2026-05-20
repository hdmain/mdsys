#include <ncurses.h>
#include <pwd.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <cerrno>
#include <climits>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iomanip>
#include <mutex>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

// ---------------------------------------------------------------------------
// Color pairs
// ---------------------------------------------------------------------------
enum : int {
    CP_TITLEBAR   = 1,
    CP_KEYBINDS   = 2,
    CP_SVC_ACTIVE = 3,
    CP_SVC_DEAD   = 4,
    CP_PINSTAR    = 5,
    CP_CATEGORY   = 6,
    CP_ERR        = 7,
    CP_DETAIL_KEY = 8,
    CP_STATUS_OK  = 9,
};

static void initColors() {
    start_color();
    use_default_colors();
    init_pair(CP_TITLEBAR,   COLOR_WHITE,  COLOR_BLUE);
    init_pair(CP_KEYBINDS,   COLOR_BLACK,  COLOR_WHITE);
    init_pair(CP_SVC_ACTIVE, COLOR_GREEN,  -1);
    init_pair(CP_SVC_DEAD,   COLOR_WHITE,  -1);
    init_pair(CP_PINSTAR,    COLOR_YELLOW, -1);
    init_pair(CP_CATEGORY,   COLOR_CYAN,   -1);
    init_pair(CP_ERR,        COLOR_RED,    -1);
    init_pair(CP_DETAIL_KEY, COLOR_YELLOW, -1);
    init_pair(CP_STATUS_OK,  COLOR_GREEN,  -1);
}

// ---------------------------------------------------------------------------
// User / session context
// ---------------------------------------------------------------------------
struct UserContext {
    uid_t       processUid = 0;
    uid_t       targetUid  = 0;
    std::string username;
    std::string xdgRuntimeDir;
    std::string userSessionPrefix;
};

static uid_t findRealUid(std::string& nameOut) {
    const char* cands[] = { getenv("SUDO_USER"), getenv("LOGNAME"), getenv("USER") };
    for (const char* n : cands) {
        if (!n || std::string(n) == "root") continue;
        if (struct passwd* pw = getpwnam(n)) { nameOut = pw->pw_name; return pw->pw_uid; }
    }
    return 0;
}

static UserContext resolveUserContext() {
    UserContext ctx;
    ctx.processUid = getuid();
    const char* su = getenv("SUDO_UID"), *sn = getenv("SUDO_USER");
    if (su) {
        ctx.targetUid = (uid_t)strtoul(su, nullptr, 10);
        ctx.username  = sn ? sn : "";
        if (ctx.username.empty()) if (auto* pw = getpwuid(ctx.targetUid)) ctx.username = pw->pw_name;
    } else {
        ctx.targetUid = ctx.processUid;
        if (auto* pw = getpwuid(ctx.targetUid)) ctx.username = pw->pw_name;
        if (ctx.targetUid == 0) {
            std::string rn; uid_t ru = findRealUid(rn);
            if (ru) { ctx.targetUid = ru; ctx.username = rn; }
        }
    }
    ctx.xdgRuntimeDir = "/run/user/" + std::to_string(ctx.targetUid);
    std::string dbus = "unix:path=" + ctx.xdgRuntimeDir + "/bus";
    if (ctx.processUid == 0 && ctx.targetUid != 0) {
        ctx.userSessionPrefix = "runuser -u " + ctx.username + " -- env "
            "XDG_RUNTIME_DIR=" + ctx.xdgRuntimeDir +
            " DBUS_SESSION_BUS_ADDRESS=" + dbus + " ";
    } else {
        const char* ex = getenv("XDG_RUNTIME_DIR"), *ed = getenv("DBUS_SESSION_BUS_ADDRESS");
        ctx.userSessionPrefix =
            "XDG_RUNTIME_DIR="            + (ex ? std::string(ex) : ctx.xdgRuntimeDir) +
            " DBUS_SESSION_BUS_ADDRESS=" + (ed ? std::string(ed) : dbus) + " ";
    }
    return ctx;
}

// ---------------------------------------------------------------------------
// Globals
// ---------------------------------------------------------------------------
static UserContext          g_user;
static bool                 g_systemMode = false;
static std::set<std::string> g_pinned;

// ---------------------------------------------------------------------------
// Pinned persistence  (~/.config/mdsys/pinned)
// ---------------------------------------------------------------------------
static std::string pinnedFilePath() {
    const char* xdgCfg = getenv("XDG_CONFIG_HOME");
    std::string base = xdgCfg ? std::string(xdgCfg) : (std::string(getenv("HOME") ? getenv("HOME") : "/root") + "/.config");
    return base + "/mdsys/pinned";
}

static void savePinned() {
    std::string path = pinnedFilePath();
    // Ensure the directory exists (mkdir -p equivalent via sys/stat).
    std::string dir = path.substr(0, path.rfind('/'));
    mkdir(dir.c_str(), 0755);
    FILE* f = fopen(path.c_str(), "w");
    if (!f) return;
    for (const auto& u : g_pinned) fprintf(f, "%s\n", u.c_str());
    fclose(f);
}

static void loadPinned() {
    std::string path = pinnedFilePath();
    FILE* f = fopen(path.c_str(), "r");
    if (!f) return;
    char buf[512];
    while (fgets(buf, sizeof(buf), f)) {
        std::string line = buf;
        // strip newline
        while (!line.empty() && (line.back() == '\n' || line.back() == '\r'))
            line.pop_back();
        if (!line.empty()) g_pinned.insert(line);
    }
    fclose(f);
}

// ---------------------------------------------------------------------------
// Service data
// ---------------------------------------------------------------------------
struct Service {
    std::string        unit, loadState, activeState, subState, description, mainPid, startedAt;
    unsigned long long memBytes = 0;
};

static std::string trim(const std::string& s) {
    auto a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) return "";
    return s.substr(a, s.find_last_not_of(" \t\r\n") - a + 1);
}

static std::string shellQ(const std::string& s) {
    std::string q = "'";
    for (char c : s) q += (c == '\'') ? std::string("'\\''") : std::string(1, c);
    return q + "'";
}

static std::string runCmd(const std::string& cmd, int* ec = nullptr) {
    std::string out; FILE* p = popen(cmd.c_str(), "r");
    if (!p) { if (ec) *ec = -1; return out; }
    char buf[4096]; while (fgets(buf, sizeof(buf), p)) out += buf;
    int st = pclose(p); if (ec) *ec = st; return out;
}

static std::string fmtMem(unsigned long long b) {
    const char* U[] = {"B","KiB","MiB","GiB","TiB"};
    double v = (double)b; int i = 0;
    while (v >= 1024.0 && i < 4) { v /= 1024.0; ++i; }
    std::ostringstream o;
    if (i == 0) o << (unsigned long long)v << " " << U[i];
    else        o << std::fixed << std::setprecision(1) << v << " " << U[i];
    return o.str();
}

static std::string pfx()  { return g_systemMode ? "" : g_user.userSessionPrefix; }
static std::string flag() { return g_systemMode ? "" : "--user "; }

static std::vector<Service> loadServices(std::string& err) {
    err.clear();
    std::vector<Service> svcs;
    int ec = 0;
    std::string out = runCmd(pfx() + "systemctl " + flag() +
        "list-units --type=service --all --no-legend --no-pager 2>&1", &ec);
    if (ec) { err = (g_systemMode ? "system" : "user") + std::string(" mode failed: ") + trim(out); return svcs; }

    std::istringstream ss(out); std::string line;
    while (std::getline(ss, line)) {
        line = trim(line); if (line.empty()) continue;
        std::vector<std::string> cols; { std::istringstream ls(line); std::string t; while (ls >> t) cols.push_back(t); }

        // systemctl prefixes active lines with a status bullet (e.g. "●").
        // Unit names always contain a dot (.service, .timer, …).
        // Skip any leading tokens that don't look like a unit name.
        int uc = 0;
        while (uc < (int)cols.size() && cols[uc].find('.') == std::string::npos) ++uc;
        if (uc + 3 >= (int)cols.size()) continue;

        Service s;
        s.unit = cols[uc]; s.loadState = cols[uc+1]; s.activeState = cols[uc+2]; s.subState = cols[uc+3];
        for (std::size_t i = uc+4; i < cols.size(); ++i) { if (i > (std::size_t)(uc+4)) s.description += ' '; s.description += cols[i]; }

        int sec = 0;
        std::string sout = runCmd(pfx() + "systemctl " + flag() + "show " + shellQ(s.unit) +
            " --property=MemoryCurrent --property=MainPID"
            " --property=ExecMainStartTimestamp --property=Description", &sec);
        if (!sec) {
            std::istringstream ps(sout); std::string pl;
            while (std::getline(ps, pl)) {
                auto val = [&](const char* k) {
                    std::size_t kl = strlen(k);
                    return (pl.size() > kl && pl.substr(0, kl) == k) ? trim(pl.substr(kl)) : std::string();
                };
                std::string v;
                if (!(v = val("MemoryCurrent=")).empty() && v != "[not set]") s.memBytes = strtoull(v.c_str(), nullptr, 10);
                else if (!(v = val("MainPID=")).empty())                      s.mainPid = v;
                else if (!(v = val("ExecMainStartTimestamp=")).empty())        s.startedAt = v;
                else if (!(v = val("Description=")).empty() && s.description.empty()) s.description = v;
            }
        }
        svcs.push_back(s);
    }

    // Sort: pinned first, then active, then inactive; alphabetical within each group.
    std::sort(svcs.begin(), svcs.end(), [](const Service& a, const Service& b) {
        int pa = g_pinned.count(a.unit) ? 0 : (a.activeState == "active" ? 1 : 2);
        int pb = g_pinned.count(b.unit) ? 0 : (b.activeState == "active" ? 1 : 2);
        return pa != pb ? pa < pb : a.unit < b.unit;
    });
    return svcs;
}

static int doAction(const std::string& unit, const std::string& action) {
    int ec = 0; runCmd(pfx() + "systemctl " + flag() + action + " " + shellQ(unit), &ec); return ec;
}

// ---------------------------------------------------------------------------
// Display rows (category headers + service rows interleaved)
// ---------------------------------------------------------------------------
enum class RowKind { Header, Service };
struct DisplayRow { RowKind kind; std::string label; int svcIdx; };

static std::vector<DisplayRow> buildRows(const std::vector<Service>& svcs) {
    std::vector<DisplayRow> rows;
    bool seenPin = false, seenActive = false, seenInactive = false;
    for (int i = 0; i < (int)svcs.size(); ++i) {
        bool pinned = g_pinned.count(svcs[i].unit) > 0;
        bool active = svcs[i].activeState == "active";
        if (pinned && !seenPin)         { seenPin     = true; rows.push_back({RowKind::Header, "PINNED",   -1}); }
        if (!pinned && active && !seenActive)   { seenActive  = true; rows.push_back({RowKind::Header, "ACTIVE",   -1}); }
        if (!pinned && !active && !seenInactive){ seenInactive= true; rows.push_back({RowKind::Header, "INACTIVE", -1}); }
        rows.push_back({RowKind::Service, "", i});
    }
    return rows;
}

// ---------------------------------------------------------------------------
// Drawing helpers
// ---------------------------------------------------------------------------
static void drawTitleBar(int width) {
    attron(COLOR_PAIR(CP_TITLEBAR) | A_BOLD);
    std::string mode  = g_systemMode ? "SYSTEM" : ("USER:" + g_user.username);
    std::string left  = "  mdsys  [" + mode + "]";
    std::string right = "uid:" + std::to_string(g_user.processUid) + "  ";
    std::string mid(std::max(0, width - (int)left.size() - (int)right.size()), ' ');
    mvprintw(0, 0, "%s%s%s", left.c_str(), mid.c_str(), right.c_str());
    attroff(COLOR_PAIR(CP_TITLEBAR) | A_BOLD);
}

static void drawKeybindBar(int row, int width, bool inDetails) {
    attron(COLOR_PAIR(CP_KEYBINDS) | A_BOLD);
    std::string kb = inDetails
        ? "  ENTER:back  R:restart  S:start  K:stop  P:pin/unpin  C:console  TAB:mode  U:refresh  Q:quit"
        : "  UP/DOWN:select  ENTER:details  R:restart  S:start  K:stop  P:pin/unpin  C:console  TAB:mode  U:refresh  Q:quit";
    mvprintw(row, 0, "%-*s", width, kb.c_str());
    attroff(COLOR_PAIR(CP_KEYBINDS) | A_BOLD);
}

static void drawColHeaders(int row) {
    attron(A_BOLD | A_UNDERLINE);
    mvprintw(row, 0, "  %-3s %-36s %-10s %-10s %-10s",
             " ", "UNIT", "STATE", "SUBSTATE", "MEMORY");
    attroff(A_BOLD | A_UNDERLINE);
}

// ---------------------------------------------------------------------------
// List view
// ---------------------------------------------------------------------------
static void drawList(const std::vector<Service>& svcs,
                     const std::vector<DisplayRow>& rows,
                     int selSvc, int width, int height,
                     const std::string& msg, const std::string& err) {
    // Layout:
    //  row 0  : title bar
    //  row 1  : column headers
    //  row 2  : separator
    //  rows 3..height-3 : list
    //  row height-2 : status
    //  row height-1 : keybind bar
    drawColHeaders(1);
    mvhline(2, 0, ACS_HLINE, width);

    int listTop    = 3;
    int listBottom = height - 2;
    int visible    = std::max(1, listBottom - listTop);

    // Find display row index of selected service.
    int selDispIdx = 0;
    for (int i = 0; i < (int)rows.size(); ++i)
        if (rows[i].kind == RowKind::Service && rows[i].svcIdx == selSvc) { selDispIdx = i; break; }

    // Compute scroll: keep selected in view.
    static int firstVis = 0;
    if (selDispIdx < firstVis)              firstVis = selDispIdx;
    if (selDispIdx >= firstVis + visible)   firstVis = selDispIdx - visible + 1;
    if (firstVis < 0) firstVis = 0;

    for (int r = 0; r < visible && (firstVis + r) < (int)rows.size(); ++r) {
        const DisplayRow& dr = rows[firstVis + r];
        int y = listTop + r;

        if (dr.kind == RowKind::Header) {
            attron(COLOR_PAIR(CP_CATEGORY) | A_BOLD);
            std::string hdr = "-- " + dr.label + " ";
            mvprintw(y, 0, "%s", hdr.c_str());
            mvhline(y, (int)hdr.size(), ACS_HLINE, width - (int)hdr.size());
            attroff(COLOR_PAIR(CP_CATEGORY) | A_BOLD);
        } else {
            const Service& s   = svcs[dr.svcIdx];
            bool sel    = (dr.svcIdx == selSvc);
            bool pinned = g_pinned.count(s.unit) > 0;
            bool active = s.activeState == "active";

            if (sel) attron(A_REVERSE);

            // Pin column
            if (pinned) {
                if (!sel) attron(COLOR_PAIR(CP_PINSTAR) | A_BOLD);
                mvaddstr(y, 0, "[*]");
                if (!sel) attroff(COLOR_PAIR(CP_PINSTAR) | A_BOLD);
            } else {
                mvaddstr(y, 0, "   ");
            }

            // Unit name
            std::string unit = s.unit.size() > 36 ? s.unit.substr(0, 33) + "..." : s.unit;
            if (!sel) attron(active ? COLOR_PAIR(CP_SVC_ACTIVE) : (COLOR_PAIR(CP_SVC_DEAD) | A_DIM));
            mvprintw(y, 4, "%-36s", unit.c_str());
            if (!sel) attroff(active ? COLOR_PAIR(CP_SVC_ACTIVE) : (COLOR_PAIR(CP_SVC_DEAD) | A_DIM));

            // State / substate / memory
            mvprintw(y, 41, " %-10s %-10s %-10s",
                s.activeState.substr(0, 10).c_str(),
                s.subState.substr(0, 10).c_str(),
                (s.memBytes == 0 ? "-" : fmtMem(s.memBytes)).substr(0, 10).c_str());

            if (sel) attroff(A_REVERSE);
        }
    }

    // Status row
    mvhline(height - 2, 0, ACS_HLINE, width);
    if (!err.empty()) {
        attron(COLOR_PAIR(CP_ERR) | A_BOLD);
        mvprintw(height - 1, 1, " ERROR: %-*s", width - 9, err.c_str());
        attroff(COLOR_PAIR(CP_ERR) | A_BOLD);
    } else {
        attron(COLOR_PAIR(CP_STATUS_OK));
        mvprintw(height - 1, 1, " %-*s", width - 2, msg.c_str());
        attroff(COLOR_PAIR(CP_STATUS_OK));
    }
}

// ---------------------------------------------------------------------------
// Details view
// ---------------------------------------------------------------------------
static void drawDetails(const Service& s, int width, int height,
                        const std::string& msg, const std::string& err) {
    bool pinned = g_pinned.count(s.unit) > 0;

    // Sub-header
    attron(A_BOLD);
    mvprintw(1, 2, "Service details");
    attroff(A_BOLD);
    mvhline(2, 0, ACS_HLINE, width);

    auto kv = [&](int row, const char* key, const std::string& val) {
        attron(COLOR_PAIR(CP_DETAIL_KEY) | A_BOLD);
        mvprintw(row, 4, "%-16s", key);
        attroff(COLOR_PAIR(CP_DETAIL_KEY) | A_BOLD);
        mvprintw(row, 21, "%s", val.empty() ? "-" : val.c_str());
    };

    kv(4,  "Unit:",        s.unit);
    kv(5,  "Description:", s.description);
    kv(6,  "Load state:",  s.loadState);
    kv(7,  "Active:",      s.activeState);
    kv(8,  "Substate:",    s.subState);
    kv(9,  "Main PID:",    (s.mainPid.empty() || s.mainPid == "0") ? "-" : s.mainPid);
    kv(10, "Memory:",      s.memBytes == 0 ? "-" : fmtMem(s.memBytes));
    kv(11, "Started at:",  s.startedAt);
    kv(12, "Pinned:",      pinned ? "yes [*] (P to unpin)" : "no  (P to pin)");

    mvhline(height - 2, 0, ACS_HLINE, width);
    if (!err.empty()) {
        attron(COLOR_PAIR(CP_ERR) | A_BOLD);
        mvprintw(height - 1, 1, " ERROR: %-*s", width - 9, err.c_str());
        attroff(COLOR_PAIR(CP_ERR) | A_BOLD);
    } else {
        mvprintw(height - 1, 1, " %s", msg.c_str());
    }
}

// ---------------------------------------------------------------------------
// Navigation helpers: skip category header rows
// ---------------------------------------------------------------------------
static int nextSvc(const std::vector<DisplayRow>& rows, int curSvc, int delta) {
    // Find service positions in order
    std::vector<int> svcOrder;
    for (auto& r : rows) if (r.kind == RowKind::Service) svcOrder.push_back(r.svcIdx);
    if (svcOrder.empty()) return curSvc;
    auto it = std::find(svcOrder.begin(), svcOrder.end(), curSvc);
    if (it == svcOrder.end()) return svcOrder[0];
    int idx = (int)(it - svcOrder.begin()) + delta;
    idx = std::clamp(idx, 0, (int)svcOrder.size() - 1);
    return svcOrder[idx];
}

// ---------------------------------------------------------------------------
// Console log viewer  (suspends ncurses, runs journalctl | less, restores)
// ---------------------------------------------------------------------------
static void openConsole(const std::string& unit) {
    // Build journalctl command.  System mode uses no --user flag.
    // We pipe through `less -R` so the user can scroll freely.
    std::string jcmd = pfx() + "journalctl " + flag() +
                       "-u " + shellQ(unit) + " -n 500 --no-pager 2>&1 | less -R";

    def_prog_mode();   // save ncurses terminal state
    endwin();          // restore normal terminal

    system(jcmd.c_str());

    reset_prog_mode(); // restore ncurses state
    refresh();         // repaint
}

// ---------------------------------------------------------------------------
// Loading screen (animated, runs while background thread loads services)
// ---------------------------------------------------------------------------
static void drawLoadingScreen(int frame, int W, int H) {
    // Spinner chars
    static const char* spin = "|/-\\";

    // Bouncing bar: a "fill" block that slides left-right
    const int barInner = 20;
    // Each full cycle = barInner*2 frames
    int cycle = frame % (barInner * 2);
    int pos   = cycle < barInner ? cycle : (barInner * 2 - 1 - cycle);

    std::string bar = "[";
    for (int i = 0; i < barInner; ++i)
        bar += (i >= pos && i < pos + 4) ? '#' : '.';
    bar += "]";

    // ASCII logo lines
    static const char* logo[] = {
        " _ __ ___  __| |___ _   _ ___ ",
        "| '_ ` _ \\/ _` / __| | | / __|",
        "| | | | | | (_| \\__ \\ |_| \\__ \\",
        "|_| |_| |_|\\__,_|___/\\__, |___/",
        "                      |___/    ",
    };
    constexpr int logoH = 5;
    constexpr int logoW = 32;

    // Center everything
    int cx = W / 2;
    int logoRow = std::max(1, H / 2 - 5);

    attron(COLOR_PAIR(CP_TITLEBAR) | A_BOLD);
    for (int i = 0; i < logoH; ++i)
        mvprintw(logoRow + i, cx - logoW / 2, "%s", logo[i]);
    attroff(COLOR_PAIR(CP_TITLEBAR) | A_BOLD);

    int barRow = logoRow + logoH + 1;
    attron(COLOR_PAIR(CP_CATEGORY) | A_BOLD);
    mvprintw(barRow, cx - (int)bar.size() / 2, "%s", bar.c_str());
    attroff(COLOR_PAIR(CP_CATEGORY) | A_BOLD);

    // Spinner + label
    attron(A_BOLD);
    mvprintw(barRow + 2, cx - 10, " %c  Loading services...", spin[frame % 4]);
    attroff(A_BOLD);

    // Mode hint at bottom
    std::string hint = std::string("  Mode: ") + (g_systemMode ? "SYSTEM" : "USER") +
                       "   uid:" + std::to_string(g_user.processUid);
    attron(COLOR_PAIR(CP_KEYBINDS) | A_BOLD);
    mvprintw(H - 1, 0, "%-*s", W, hint.c_str());
    attroff(COLOR_PAIR(CP_KEYBINDS) | A_BOLD);
}

// ---------------------------------------------------------------------------
// Main
// ---------------------------------------------------------------------------
// ---------------------------------------------------------------------------
// Service registration  (mdsys <program> <name>)
// ---------------------------------------------------------------------------
static void mkdirP(const std::string& path) {
    // Create each path component in turn (simple iterative mkdir -p).
    for (std::size_t pos = 1; pos <= path.size(); ++pos) {
        if (pos == path.size() || path[pos] == '/') {
            std::string part = path.substr(0, pos);
            mkdir(part.c_str(), 0755);  // ignore EEXIST
        }
    }
}

static int registerService(const std::string& progArg, const std::string& rawName) {
    // Strip a trailing ".service" if the user accidentally typed it.
    std::string name = rawName;
    if (name.size() > 8 && name.substr(name.size() - 8) == ".service")
        name = name.substr(0, name.size() - 8);

    // ── Resolve executable to an absolute path ───────────────────────────
    char resolvedExec[PATH_MAX] = {};
    if (!realpath(progArg.c_str(), resolvedExec)) {
        // realpath fails if the file doesn't exist yet; try CWD-relative path.
        char cwd2[PATH_MAX] = {};
        if (!getcwd(cwd2, sizeof(cwd2))) { perror("getcwd"); return 1; }
        // Build absolute path manually; truncate safely if somehow > PATH_MAX.
        std::string abs = std::string(cwd2) + "/" + progArg;
        abs.copy(resolvedExec, sizeof(resolvedExec) - 1);
        resolvedExec[sizeof(resolvedExec) - 1] = '\0';
    }

    // ── Working directory (where the command was run) ────────────────────
    char cwd[PATH_MAX] = {};
    if (!getcwd(cwd, sizeof(cwd))) { perror("getcwd"); return 1; }

    // ── Effective username ───────────────────────────────────────────────
    std::string username;
    const char* sudoUser = getenv("SUDO_USER");
    if (sudoUser && *sudoUser && std::string(sudoUser) != "root") {
        username = sudoUser;
    } else {
        struct passwd* pw = getpwuid(getuid());
        username = pw ? pw->pw_name : "root";
    }

    // ── Service file destination ─────────────────────────────────────────
    bool asRoot = (getuid() == 0);
    std::string serviceFilePath;
    std::string systemctlFlag;

    if (asRoot) {
        serviceFilePath = "/etc/systemd/system/" + name + ".service";
        systemctlFlag   = "";
    } else {
        const char* home = getenv("HOME");
        if (!home) {
            struct passwd* pw = getpwuid(getuid());
            home = pw ? pw->pw_dir : nullptr;
        }
        if (!home) { fprintf(stderr, "error: cannot determine HOME\n"); return 1; }
        std::string dir = std::string(home) + "/.config/systemd/user";
        mkdirP(dir);
        serviceFilePath = dir + "/" + name + ".service";
        systemctlFlag   = "--user ";
    }

    // ── Check for overwrite ──────────────────────────────────────────────
    if (access(serviceFilePath.c_str(), F_OK) == 0) {
        fprintf(stderr,
                "warning: %s already exists. Overwrite? [y/N] ",
                serviceFilePath.c_str());
        int ch = getchar();
        if (ch != 'y' && ch != 'Y') { printf("Aborted.\n"); return 0; }
    }

    // ── Write unit file ──────────────────────────────────────────────────
    FILE* f = fopen(serviceFilePath.c_str(), "w");
    if (!f) {
        fprintf(stderr, "error: cannot write %s: %s\n",
                serviceFilePath.c_str(), strerror(errno));
        return 1;
    }
    fprintf(f,
        "[Unit]\n"
        "Description=%s\n"
        "After=network.target\n"
        "\n"
        "[Service]\n"
        "Type=simple\n"
        "WorkingDirectory=%s\n"
        "ExecStart=%s\n"
        "Restart=always\n"
        "RestartSec=5\n"
        "User=%s\n"
        "\n"
        "[Install]\n"
        "WantedBy=multi-user.target\n",
        name.c_str(), cwd, resolvedExec, username.c_str());
    fclose(f);

    // ── Reload systemd ───────────────────────────────────────────────────
    std::string reloadCmd = "systemctl " + systemctlFlag + "daemon-reload";
    (void)system(reloadCmd.c_str());

    // ── Auto-pin the newly created service ───────────────────────────────
    loadPinned();
    g_pinned.insert(name + ".service");
    savePinned();

    // ── Print summary ─────────────────────────────────────────────────────
    printf("\n");
    printf("  Created:          %s\n",  serviceFilePath.c_str());
    printf("  ExecStart:        %s\n",  resolvedExec);
    printf("  WorkingDirectory: %s\n",  cwd);
    printf("  User:             %s\n\n", username.c_str());
    printf("  Start now:   systemctl %sstart  %s\n", systemctlFlag.c_str(), name.c_str());
    printf("  Auto-start:  systemctl %senable %s\n", systemctlFlag.c_str(), name.c_str());
    printf("  Check logs:  journalctl %s-u %s -f\n\n",
           systemctlFlag.empty() ? "" : "--user ", name.c_str());
    return 0;
}

// ---------------------------------------------------------------------------
// Main
// ---------------------------------------------------------------------------
int main(int argc, char* argv[]) {
    // ── Non-TUI mode: mdsys <program> <service-name> ─────────────────────
    if (argc == 3) {
        return registerService(argv[1], argv[2]);
    }
    if (argc > 1 && (std::string(argv[1]) == "-h" || std::string(argv[1]) == "--help")) {
        printf("Usage:\n");
        printf("  mdsys                      launch TUI service manager\n");
        printf("  mdsys <program> <name>     register <program> as systemd service <name>\n");
        printf("  mdsys -h                   show this help\n\n");
        printf("Examples:\n");
        printf("  mdsys ./server myserver    creates /etc/systemd/system/myserver.service\n");
        return 0;
    }

    g_user       = resolveUserContext();
    g_systemMode = (g_user.processUid == 0);
    loadPinned();

    initscr();
    cbreak();
    noecho();
    keypad(stdscr, TRUE);
    curs_set(0);
    nodelay(stdscr, TRUE);   // non-blocking getch during loading

    if (has_colors()) initColors();

    // Kick off service loading in a background thread.
    std::string err;
    std::vector<Service> svcs;
    std::atomic<bool> loadDone{false};

    std::thread loader([&]() {
        svcs = loadServices(err);
        loadDone.store(true, std::memory_order_release);
    });

    // Animate until loading finishes (or user presses Q to abort).
    int frame = 0;
    bool aborted = false;
    while (!loadDone.load(std::memory_order_acquire)) {
        int W = 0, H = 0;
        getmaxyx(stdscr, H, W);
        clear();
        drawLoadingScreen(frame++, W, H);
        refresh();
        std::this_thread::sleep_for(std::chrono::milliseconds(60));
        int ch = getch();
        if (ch == 'q' || ch == 'Q') { aborted = true; break; }
    }
    loader.join();

    if (aborted) { endwin(); return 0; }

    nodelay(stdscr, FALSE);  // back to blocking input

    std::vector<DisplayRow> rows = buildRows(svcs);
    std::string msg = "Loaded " + std::to_string(svcs.size()) + " service(s).";

    int  selSvc     = svcs.empty() ? 0 : rows[0].kind == RowKind::Service ? rows[0].svcIdx : nextSvc(rows, 0, 0);
    bool inDetails  = false;

    auto reload = [&](const std::string& newMsg = "") {
        // Show loading animation while reloading.
        std::atomic<bool> done{false};
        std::vector<Service> tmp;
        std::string tmpErr;
        std::thread t([&]() { tmp = loadServices(tmpErr); done.store(true, std::memory_order_release); });
        nodelay(stdscr, TRUE);
        int f = 0;
        while (!done.load(std::memory_order_acquire)) {
            int W = 0, H = 0; getmaxyx(stdscr, H, W);
            clear(); drawLoadingScreen(f++, W, H); refresh();
            std::this_thread::sleep_for(std::chrono::milliseconds(60));
        }
        t.join();
        nodelay(stdscr, FALSE);
        svcs = std::move(tmp);
        err  = std::move(tmpErr);
        rows = buildRows(svcs);
        if (!newMsg.empty()) msg = newMsg;
        if (!svcs.empty()) selSvc = std::clamp(selSvc, 0, (int)svcs.size() - 1);
    };

    while (true) {
        int W = 0, H = 0;
        getmaxyx(stdscr, H, W);
        clear();

        drawTitleBar(W);

        if (svcs.empty()) {
            mvhline(2, 0, ACS_HLINE, W);
            mvprintw(4, 2, "No services found.");
            if (!err.empty()) { attron(COLOR_PAIR(CP_ERR)); mvprintw(5, 2, "%s", err.c_str()); attroff(COLOR_PAIR(CP_ERR)); }
            mvprintw(7, 2, "TAB: toggle system/user    U: refresh    Q: quit");
        } else {
            if (inDetails) {
                drawDetails(svcs[selSvc], W, H - 1, msg, err);
            } else {
                drawList(svcs, rows, selSvc, W, H - 1, msg, err);
            }
        }

        drawKeybindBar(H - 1, W, inDetails);
        refresh();

        int ch = getch();

        if (ch == 'q' || ch == 'Q') break;

        // Tab: toggle system / user mode
        if (ch == '\t') {
            g_systemMode = !g_systemMode;
            inDetails = false; selSvc = 0;
            reload(std::string(g_systemMode ? "System" : "User") + " mode — " +
                   std::to_string(svcs.size()) + " service(s).");
            if (!svcs.empty()) selSvc = nextSvc(rows, 0, 0);
            continue;
        }

        // Refresh
        if (ch == 'u' || ch == 'U') {
            reload("Refreshed — " + std::to_string(svcs.size()) + " service(s).");
            continue;
        }

        if (svcs.empty()) continue;

        if (!inDetails) {
            if (ch == KEY_UP   || ch == 'w' || ch == 'W')
                selSvc = nextSvc(rows, selSvc, -1);
            else if (ch == KEY_DOWN || ch == 'j' || ch == 'J')
                selSvc = nextSvc(rows, selSvc,  1);
            else if (ch == KEY_PPAGE)
                selSvc = nextSvc(rows, selSvc, -10);
            else if (ch == KEY_NPAGE)
                selSvc = nextSvc(rows, selSvc,  10);
            else if (ch == '\n' || ch == KEY_ENTER || ch == 10 || ch == 13)
                inDetails = true;
            else if (ch == 'p' || ch == 'P') {
                const std::string& u = svcs[selSvc].unit;
                if (g_pinned.count(u)) { g_pinned.erase(u);  msg = "Unpinned: " + u; }
                else                   { g_pinned.insert(u); msg = "Pinned: "   + u; }
                savePinned();
                err.clear();
                svcs = loadServices(err);
                rows = buildRows(svcs);
                selSvc = std::clamp(selSvc, 0, (int)svcs.size() - 1);
            }
            else if (ch == 'c' || ch == 'C') {
                openConsole(svcs[selSvc].unit);
            }
            else if (ch == 'r' || ch == 'R' || ch == 's' || ch == 'S' || ch == 'k' || ch == 'K') {
                std::string act = (ch=='r'||ch=='R') ? "restart" : (ch=='s'||ch=='S') ? "start" : "stop";
                int ec = doAction(svcs[selSvc].unit, act);
                std::this_thread::sleep_for(std::chrono::milliseconds(300));
                if (ec == 0) { err.clear(); reload(act + " OK: " + svcs[selSvc].unit); }
                else         { reload(); err = act + " FAILED (exit " + std::to_string(ec) + "): " + svcs[selSvc].unit; }
            }
        } else {
            if (ch == '\n' || ch == KEY_ENTER || ch == 10 || ch == 13 || ch == 27)
                inDetails = false;
            else if (ch == 'p' || ch == 'P') {
                const std::string& u = svcs[selSvc].unit;
                if (g_pinned.count(u)) { g_pinned.erase(u);  msg = "Unpinned: " + u; }
                else                   { g_pinned.insert(u); msg = "Pinned: "   + u; }
                savePinned();
                err.clear();
                svcs = loadServices(err);
                rows = buildRows(svcs);
                selSvc = std::clamp(selSvc, 0, (int)svcs.size() - 1);
            }
            else if (ch == 'c' || ch == 'C') {
                openConsole(svcs[selSvc].unit);
            }
            else if (ch == 'r' || ch == 'R' || ch == 's' || ch == 'S' || ch == 'k' || ch == 'K') {
                std::string act = (ch=='r'||ch=='R') ? "restart" : (ch=='s'||ch=='S') ? "start" : "stop";
                int ec = doAction(svcs[selSvc].unit, act);
                std::this_thread::sleep_for(std::chrono::milliseconds(300));
                if (ec == 0) { err.clear(); reload(act + " OK: " + svcs[selSvc].unit); }
                else         { reload(); err = act + " FAILED (exit " + std::to_string(ec) + "): " + svcs[selSvc].unit; }
            }
        }
    }

    endwin();
    return 0;
}
