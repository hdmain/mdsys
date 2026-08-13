#include <ncurses.h>
#include <pwd.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <cctype>
#include <cerrno>
#include <climits>
#include <cstdint>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <iomanip>
#include <map>
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
    CP_CHART_RAM  = 10,
    CP_CHART_CPU  = 11,
    CP_CHART_GRID = 12,
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
    init_pair(CP_CHART_RAM,  COLOR_GREEN,  -1);
    init_pair(CP_CHART_CPU,  COLOR_CYAN,   -1);
    init_pair(CP_CHART_GRID, COLOR_WHITE,  -1);
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

static void mkdirP(const std::string& path) {
    for (std::size_t pos = 1; pos <= path.size(); ++pos) {
        if (pos == path.size() || path[pos] == '/') {
            std::string part = path.substr(0, pos);
            mkdir(part.c_str(), 0755);
        }
    }
}

static uid_t configOwnerUid() {
    const char* sudoUid = getenv("SUDO_UID");
    if (sudoUid) return (uid_t)strtoul(sudoUid, nullptr, 10);
    uid_t uid = getuid();
    if (uid != 0) return uid;
    std::string name;
    uid_t real = findRealUid(name);
    return real ? real : uid;
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
    if (xdgCfg && xdgCfg[0] != '\0')
        return std::string(xdgCfg) + "/mdsys/pinned";

    struct passwd* pw = getpwuid(configOwnerUid());
    std::string home = pw && pw->pw_dir ? pw->pw_dir : "/root";
    return home + "/.config/mdsys/pinned";
}

static bool savePinned(std::string* errOut = nullptr) {
    const std::string path = pinnedFilePath();
    const std::string dir  = path.substr(0, path.rfind('/'));
    mkdirP(dir);

    FILE* f = fopen(path.c_str(), "w");
    if (!f) {
        if (errOut) *errOut = std::string("cannot write ") + path + ": " + strerror(errno);
        return false;
    }
    for (const auto& u : g_pinned) {
        if (fprintf(f, "%s\n", u.c_str()) < 0) {
            if (errOut) *errOut = std::string("write failed: ") + path;
            fclose(f);
            return false;
        }
    }
    if (fflush(f) != 0) {
        if (errOut) *errOut = std::string("flush failed: ") + path;
        fclose(f);
        return false;
    }
    fclose(f);
    return true;
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
    std::string        unit, loadState, activeState, subState, description, execPath, mainPid, startedAt;
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

static std::string parseSystemdExecPath(const std::string& v) {
    const std::string key = "path=";
    auto pos = v.find(key);
    if (pos == std::string::npos) return "";
    pos += key.size();
    if (pos < v.size() && v[pos] == '"') {
        ++pos;
        auto end = v.find('"', pos);
        return end != std::string::npos ? v.substr(pos, end - pos) : std::string();
    }
    auto end = v.find_first_of(" ;}", pos);
    if (end == std::string::npos) end = v.size();
    return trim(v.substr(pos, end - pos));
}

static std::string readProcExe(const std::string& pidStr) {
    unsigned long pid = strtoul(pidStr.c_str(), nullptr, 10);
    if (pid == 0) return "";
    char buf[PATH_MAX + 1];
    const std::string link = "/proc/" + std::to_string(pid) + "/exe";
    const ssize_t n = readlink(link.c_str(), buf, PATH_MAX);
    if (n <= 0) return "";
    buf[n] = '\0';
    return buf;
}

static std::string resolveExecPath(const std::string& mainPid,
                                   const std::string& execMainStart,
                                   const std::string& execStart) {
    std::string path = readProcExe(mainPid);
    if (!path.empty()) return path;
    path = parseSystemdExecPath(execMainStart);
    if (!path.empty()) return path;
    return parseSystemdExecPath(execStart);
}

static std::string pfx()  { return g_systemMode ? "" : g_user.userSessionPrefix; }
static std::string flag() { return g_systemMode ? "" : "--user "; }

// ---------------------------------------------------------------------------
// Live service statistics (details view, 0.2s refresh)
// ---------------------------------------------------------------------------
struct LiveStats {
    static constexpr std::size_t kMaxHistory = 256;
    static constexpr int         kSampleMs   = 200;
    std::vector<double>          memHistory;
    std::vector<double>          cpuHistory;
    unsigned long long           lastCpuNsec = 0;
    bool                         haveCpuBaseline = false;
    std::chrono::steady_clock::time_point lastSample{};
    std::chrono::steady_clock::time_point nextSample{};
    double                       lastMem = 0;
    double                       lastCpu = 0;
    double                       scaleMemMax = 1024.0 * 1024.0;
    double                       scaleCpuMax = 100.0;
};

static void pushHistory(std::vector<double>& hist, double v) {
    hist.push_back(v);
    while (hist.size() > LiveStats::kMaxHistory)
        hist.erase(hist.begin());
}

static double smoothScale(double prev, double observed, double floorVal) {
    const double target = std::max(observed * 1.12, floorVal);
    if (prev < 1e-6) return target;
    if (target > prev) return prev * 0.35 + target * 0.65;
    return prev * 0.92 + target * 0.08;
}

static std::vector<double> chartSamples(const std::vector<double>& hist, int width) {
    std::vector<double> out((std::size_t)std::max(1, width), 0.0);
    if (hist.empty()) return out;
    const int n = (int)hist.size();
    const int w = (int)out.size();
    if (n >= w) {
        for (int c = 0; c < w; ++c)
            out[(std::size_t)c] = hist[(std::size_t)(n - w + c)];
        return out;
    }
    const int pad = w - n;
    for (int c = 0; c < w; ++c)
        out[(std::size_t)c] = (c < pad) ? 0.0 : hist[(std::size_t)(c - pad)];
    return out;
}

static int valueToRow(double v, double maxVal, int plotH) {
    if (maxVal < 1e-9) maxVal = 1.0;
    v = std::clamp(v / maxVal, 0.0, 1.0);
    const int row = (int)(v * (double)(plotH - 1) + 0.5);
    return plotH - 1 - std::clamp(row, 0, plotH - 1);
}

static void drawSeries(int plotY, int plotX, int plotW, int plotH,
                       const std::vector<double>& samples, double maxVal, int colorPair) {
    if (plotW < 1 || plotH < 1) return;

    auto plotPoint = [&](int x, int y) {
        attron(COLOR_PAIR(colorPair) | A_BOLD);
        mvaddch(plotY + y, plotX + x, '*');
        attroff(COLOR_PAIR(colorPair) | A_BOLD);
    };

    int prevX = -1, prevY = -1;
    for (int x = 0; x < plotW; ++x) {
        const int y = valueToRow(samples[(std::size_t)x], maxVal, plotH);
        if (prevX < 0) {
            plotPoint(x, y);
        } else {
            int x0 = prevX, y0 = prevY, x1 = x, y1 = y;
            const int dx = std::abs(x1 - x0);
            const int sx = x0 < x1 ? 1 : -1;
            int err = dx - std::abs(y1 - y0);
            int cx = x0, cy = y0;
            while (true) {
                plotPoint(cx, cy);
                if (cx == x1 && cy == y1) break;
                const int e2 = 2 * err;
                if (e2 > -std::abs(y1 - y0)) { err -= std::abs(y1 - y0); cx += sx; }
                if (e2 < dx) { err += dx; cy += (y0 < y1 ? 1 : -1); }
            }
        }
        prevX = x;
        prevY = y;
    }
}

struct ChartRect { int x, y, w, h; };

struct ChartLayout {
    ChartRect ram;
    ChartRect cpu;
    bool      sideBySide = false;
};

static ChartLayout computeChartLayout(int width, int height, int areaTop, int areaBottom) {
    ChartLayout L;
    const int marginX = 2;
    const int gap     = 2;
    const int availW  = std::max(20, width - marginX * 2);
    const int availH  = std::max(6, areaBottom - areaTop);

    L.sideBySide = availW >= 56 && availH >= 5;
    if (L.sideBySide) {
        const int panelW = (availW - gap) / 2;
        const int panelH = availH;
        L.ram = {marginX, areaTop, panelW, panelH};
        L.cpu = {marginX + panelW + gap, areaTop, availW - panelW - gap, panelH};
    } else {
        const int panelH = (availH - gap) / 2;
        L.ram = {marginX, areaTop, availW, panelH};
        L.cpu = {marginX, areaTop + panelH + gap, availW, availH - panelH - gap};
    }
    L.ram.w = std::max(12, L.ram.w);
    L.ram.h = std::max(5, L.ram.h);
    L.cpu.w = std::max(12, L.cpu.w);
    L.cpu.h = std::max(5, L.cpu.h);
    return L;
}

static void drawPanelBorder(int y, int x, int w, int h, int colorPair) {
    if (w < 4 || h < 3) return;
    attron(COLOR_PAIR(colorPair));
    mvaddch(y, x, ACS_ULCORNER);
    for (int i = 1; i < w - 1; ++i) mvaddch(y, x + i, ACS_HLINE);
    mvaddch(y, x + w - 1, ACS_URCORNER);
    for (int r = 1; r < h - 1; ++r) {
        mvaddch(y + r, x, ACS_VLINE);
        mvaddch(y + r, x + w - 1, ACS_VLINE);
    }
    mvaddch(y + h - 1, x, ACS_LLCORNER);
    for (int i = 1; i < w - 1; ++i) mvaddch(y + h - 1, x + i, ACS_HLINE);
    mvaddch(y + h - 1, x + w - 1, ACS_LRCORNER);
    attroff(COLOR_PAIR(colorPair));
}

static void clearRect(int y, int x, int w, int h) {
    attrset(A_NORMAL);
    for (int r = 0; r < h; ++r)
        for (int c = 0; c < w; ++c)
            mvaddch(y + r, x + c, ' ');
}

static void wipeRows(int fromRow, int toRow, int width) {
    attrset(A_NORMAL);
    for (int y = fromRow; y < toRow; ++y)
        for (int x = 0; x < width; ++x)
            mvaddch(y, x, ' ');
}

static void drawPanelChart(const ChartRect& rc, const std::vector<double>& hist,
                           double maxVal, const char* title, const char* valueLine,
                           int colorPair, bool isCpu) {
    if (rc.w < 10 || rc.h < 6) return;
    if (maxVal < 1e-9) maxVal = 1.0;

    drawPanelBorder(rc.y, rc.x, rc.w, rc.h, colorPair);

    attron(COLOR_PAIR(colorPair) | A_BOLD);
    std::string hdr = std::string(" ") + title + "  " + valueLine;
    if ((int)hdr.size() > rc.w - 4) hdr = hdr.substr(0, (std::size_t)std::max(0, rc.w - 7)) + "...";
    mvprintw(rc.y, rc.x + 1, "%-*s", rc.w - 2, hdr.c_str());
    attroff(COLOR_PAIR(colorPair) | A_BOLD);

    const int axisW  = 5;
    const int plotX  = rc.x + 1 + axisW;
    const int plotY  = rc.y + 1;
    const int plotW  = rc.w - 2 - axisW;
    const int plotH  = rc.h - 2;
    if (plotW < 4 || plotH < 3) return;

    clearRect(plotY, rc.x + 1, rc.w - 2, plotH);

    char scaleTop[12], scaleMid[12];
    if (isCpu) {
        snprintf(scaleTop, sizeof(scaleTop), "%4.0f%%", maxVal);
        snprintf(scaleMid, sizeof(scaleMid), "%4.0f%%", maxVal * 0.5);
    } else if (maxVal >= 1024.0 * 1024.0) {
        snprintf(scaleTop, sizeof(scaleTop), "%3.0fM", maxVal / (1024.0 * 1024.0));
        snprintf(scaleMid, sizeof(scaleMid), "%3.0fM", maxVal * 0.5 / (1024.0 * 1024.0));
    } else if (maxVal >= 1024.0) {
        snprintf(scaleTop, sizeof(scaleTop), "%3.0fK", maxVal / 1024.0);
        snprintf(scaleMid, sizeof(scaleMid), "%3.0fK", maxVal * 0.5 / 1024.0);
    } else {
        snprintf(scaleTop, sizeof(scaleTop), "%4.0f", maxVal);
        snprintf(scaleMid, sizeof(scaleMid), "%4.0f", maxVal * 0.5);
    }

    attron(COLOR_PAIR(CP_CHART_GRID) | A_DIM);
    mvprintw(plotY, rc.x + 1, "%s", scaleTop);
    mvprintw(plotY + plotH / 2, rc.x + 1, "%s", scaleMid);
    mvprintw(plotY + plotH - 1, rc.x + 1, "   0");
    attroff(COLOR_PAIR(CP_CHART_GRID) | A_DIM);

    for (int gy = 1; gy <= 3; ++gy) {
        const int row = plotY + (plotH * gy) / 4;
        for (int gx = 0; gx < plotW; ++gx)
            mvaddch(row, plotX + gx, ACS_HLINE | COLOR_PAIR(CP_CHART_GRID) | A_DIM);
    }

    std::vector<double> samples = chartSamples(hist, plotW);
    drawSeries(plotY, plotX, plotW, plotH, samples, maxVal, colorPair);

    attron(COLOR_PAIR(CP_CHART_GRID) | A_DIM);
    mvaddch(rc.y + rc.h - 1, rc.x + rc.w - 2, ACS_RARROW);
    attroff(COLOR_PAIR(CP_CHART_GRID) | A_DIM);
}

static void tickLiveStats(const std::string& unit, LiveStats& st) {
    auto now = std::chrono::steady_clock::now();
    if (st.nextSample.time_since_epoch().count() != 0 && now < st.nextSample)
        return;
    st.nextSample = now + std::chrono::milliseconds(LiveStats::kSampleMs);

    double memBytes = 0, cpuPct = 0;
    int ec = 0;
    std::string out = runCmd(pfx() + "systemctl " + flag() + "show " + shellQ(unit) +
        " --property=MemoryCurrent --property=CPUUsageNSec 2>&1", &ec);
    if (ec) return;

    unsigned long long cpuNsec = 0;
    std::istringstream ps(out);
    std::string pl;
    while (std::getline(ps, pl)) {
        auto val = [&](const char* k) {
            std::size_t kl = strlen(k);
            return (pl.size() > kl && pl.substr(0, kl) == k) ? trim(pl.substr(kl)) : std::string();
        };
        std::string v;
        if (!(v = val("MemoryCurrent=")).empty() && v != "[not set]")
            memBytes = (double)strtoull(v.c_str(), nullptr, 10);
        else if (!(v = val("CPUUsageNSec=")).empty() && v != "[not set]")
            cpuNsec = strtoull(v.c_str(), nullptr, 10);
    }

    if (st.haveCpuBaseline) {
        auto dtUs = std::chrono::duration_cast<std::chrono::microseconds>(now - st.lastSample).count();
        if (dtUs >= 50'000 && cpuNsec >= st.lastCpuNsec) {
            unsigned long long delta = cpuNsec - st.lastCpuNsec;
            long ncpu = sysconf(_SC_NPROCESSORS_ONLN);
            if (ncpu < 1) ncpu = 1;
            cpuPct = 100.0 * (double)delta / ((double)dtUs * 1000.0 * (double)ncpu);
            cpuPct = std::clamp(cpuPct, 0.0, 100.0 * (double)ncpu);
        }
    }
    st.lastCpuNsec     = cpuNsec;
    st.lastSample      = now;
    st.haveCpuBaseline = true;
    st.lastMem         = memBytes;
    st.lastCpu         = cpuPct;

    pushHistory(st.memHistory, memBytes);
    pushHistory(st.cpuHistory, cpuPct);

    st.scaleMemMax = smoothScale(st.scaleMemMax, memBytes, 512.0 * 1024.0);
    st.scaleCpuMax = smoothScale(st.scaleCpuMax, cpuPct, 5.0);
    st.scaleCpuMax = std::clamp(st.scaleCpuMax, 5.0, 100.0);
}

// List enrich: skip Exec* — resolve binary path lazily in details only.
static const char* kShowPropertiesList =
    "--property=Id --property=MemoryCurrent --property=MainPID"
    " --property=ExecMainStartTimestamp --property=Description";

static const char* kShowPropertiesExec =
    "--property=MainPID --property=ExecMainStart --property=ExecStart";

static std::string showLineValue(const std::string& line, const char* key) {
    const std::size_t kl = strlen(key);
    return (line.size() > kl && line.substr(0, kl) == key) ? trim(line.substr(kl)) : std::string();
}

static void applyShowBlock(const std::string& block, Service& s) {
    std::istringstream ps(block);
    std::string pl;
    while (std::getline(ps, pl)) {
        std::string v;
        if (!(v = showLineValue(pl, "Id=")).empty()) { /* matched via map */ }
        else if (!(v = showLineValue(pl, "MemoryCurrent=")).empty() && v != "[not set]")
            s.memBytes = strtoull(v.c_str(), nullptr, 10);
        else if (!(v = showLineValue(pl, "MainPID=")).empty())
            s.mainPid = v;
        else if (!(v = showLineValue(pl, "ExecMainStartTimestamp=")).empty())
            s.startedAt = v;
        else if (!(v = showLineValue(pl, "Description=")).empty() && s.description.empty())
            s.description = v;
    }
}

static void forEachShowBlock(const std::string& out, const std::function<void(const std::string&)>& fn) {
    std::string block;
    std::istringstream ss(out);
    std::string line;
    auto flush = [&]() {
        const std::string trimmed = trim(block);
        if (!trimmed.empty()) fn(trimmed);
        block.clear();
    };
    while (std::getline(ss, line)) {
        if (line.empty()) flush();
        else {
            if (!block.empty()) block += '\n';
            block += line;
        }
    }
    flush();
}

static void sortServices(std::vector<Service>& svcs) {
    std::sort(svcs.begin(), svcs.end(), [](const Service& a, const Service& b) {
        int pa = g_pinned.count(a.unit) ? 0 : (a.activeState == "active" ? 1 : 2);
        int pb = g_pinned.count(b.unit) ? 0 : (b.activeState == "active" ? 1 : 2);
        return pa != pb ? pa < pb : a.unit < b.unit;
    });
}

static void enrichServiceIndices(std::vector<Service>& svcs, const std::vector<std::size_t>& indices) {
    if (indices.empty()) return;

    constexpr std::size_t kBatchSize = 64;
    std::map<std::string, std::size_t> idxByUnit;
    for (std::size_t i : indices)
        idxByUnit[svcs[i].unit] = i;

    for (std::size_t batch = 0; batch < indices.size(); batch += kBatchSize) {
        const std::size_t end = std::min(batch + kBatchSize, indices.size());
        std::string cmd = pfx() + "systemctl " + flag() + "show ";
        for (std::size_t b = batch; b < end; ++b)
            cmd += shellQ(svcs[indices[b]].unit) + " ";
        cmd += kShowPropertiesList;
        cmd += " 2>&1";

        int ec = 0;
        const std::string out = runCmd(cmd, &ec);
        if (ec) continue;

        forEachShowBlock(out, [&](const std::string& block) {
            std::string unitId;
            std::istringstream bs(block);
            std::string bl;
            while (std::getline(bs, bl)) {
                if (!(unitId = showLineValue(bl, "Id=")).empty()) break;
            }
            if (unitId.empty()) return;
            auto it = idxByUnit.find(unitId);
            if (it != idxByUnit.end())
                applyShowBlock(block, svcs[it->second]);
        });
    }
}

static void ensureExecPath(Service& s) {
    if (!s.execPath.empty()) return;
    s.execPath = readProcExe(s.mainPid);
    if (!s.execPath.empty()) return;

    int ec = 0;
    std::string out = runCmd(pfx() + "systemctl " + flag() + "show " + shellQ(s.unit) +
        " " + kShowPropertiesExec + " 2>&1", &ec);
    if (ec) return;

    std::string execMainStart, execStart, mainPid = s.mainPid;
    std::istringstream ps(out);
    std::string pl;
    while (std::getline(ps, pl)) {
        std::string v;
        if (!(v = showLineValue(pl, "MainPID=")).empty())
            mainPid = v;
        else if (!(v = showLineValue(pl, "ExecMainStart=")).empty())
            execMainStart = v;
        else if (!(v = showLineValue(pl, "ExecStart=")).empty() && execStart.empty())
            execStart = v;
    }
    if (s.mainPid.empty()) s.mainPid = mainPid;
    s.execPath = resolveExecPath(mainPid, execMainStart, execStart);
}

static std::vector<Service> parseListUnits(std::string& err) {
    err.clear();
    std::vector<Service> svcs;
    int ec = 0;
    std::string out = runCmd(pfx() + "systemctl " + flag() +
        "list-units --type=service --all --no-legend --no-pager 2>&1", &ec);
    if (ec) {
        err = (g_systemMode ? "system" : "user") + std::string(" mode failed: ") + trim(out);
        return svcs;
    }

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
        for (std::size_t i = uc+4; i < cols.size(); ++i) {
            if (i > (std::size_t)(uc+4)) s.description += ' ';
            s.description += cols[i];
        }
        svcs.push_back(s);
    }
    sortServices(svcs);
    return svcs;
}

// Progressive catalog: list appears ASAP, then pinned enrich, then the rest.
struct ServiceCatalog {
    std::mutex              mtx;
    std::vector<Service>    svcs;
    std::string             err;
    std::atomic<bool>       listReady{false};
    std::atomic<bool>       enrichDone{false};
    std::atomic<uint64_t>   generation{0};

    void publish(std::vector<Service> next, const std::string& nextErr) {
        std::lock_guard<std::mutex> lk(mtx);
        svcs = std::move(next);
        err  = nextErr;
        generation.fetch_add(1, std::memory_order_release);
    }

    void snapshot(std::vector<Service>& out, std::string& outErr) {
        std::lock_guard<std::mutex> lk(mtx);
        out    = svcs;
        outErr = err;
    }
};

static void loadServicesProgressive(ServiceCatalog& cat, std::atomic<bool>& cancel) {
    std::string err;
    std::vector<Service> svcs = parseListUnits(err);
    if (cancel.load(std::memory_order_acquire)) return;
    cat.publish(svcs, err);
    cat.listReady.store(true, std::memory_order_release);
    if (err.empty() && !svcs.empty() && !cancel.load(std::memory_order_acquire)) {
        std::vector<std::size_t> pinned, rest;
        for (std::size_t i = 0; i < svcs.size(); ++i) {
            if (g_pinned.count(svcs[i].unit)) pinned.push_back(i);
            else rest.push_back(i);
        }
        enrichServiceIndices(svcs, pinned);
        if (cancel.load(std::memory_order_acquire)) return;
        cat.publish(svcs, err);

        constexpr std::size_t kChunk = 64;
        for (std::size_t off = 0; off < rest.size(); off += kChunk) {
            if (cancel.load(std::memory_order_acquire)) return;
            std::vector<std::size_t> chunk(
                rest.begin() + (std::ptrdiff_t)off,
                rest.begin() + (std::ptrdiff_t)std::min(off + kChunk, rest.size()));
            enrichServiceIndices(svcs, chunk);
            cat.publish(svcs, err);
        }
    }
    cat.enrichDone.store(true, std::memory_order_release);
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
        ? "  ENTER/Q:back  R:restart  S:start  K:stop  P:pin  C:console  V:live  TAB:mode  U:refresh  ?:help"
        : "  UP/DOWN:select  ENTER:details  F:find  R/S/K  P:pin  C:console  V:live  TAB:mode  U:refresh  Q:quit  ?:help";
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
static void drawDetails(Service& s, int width, int height,
                        const std::string& msg, const std::string& err,
                        const LiveStats& stats) {
    bool pinned = g_pinned.count(s.unit) > 0;

    attron(A_BOLD);
    mvprintw(1, 2, "Service details");
    attroff(A_BOLD);
    mvhline(2, 0, ACS_HLINE, width);

    double liveMem = stats.lastMem;
    double liveCpu = stats.lastCpu;

    // Compact header — frees vertical space for charts.
    std::string unit = s.unit;
    if ((int)unit.size() > width - 14) unit = unit.substr(0, (std::size_t)std::max(0, width - 17)) + "...";
    attron(COLOR_PAIR(CP_DETAIL_KEY) | A_BOLD);
    mvprintw(3, 2, "Unit:");
    attroff(COLOR_PAIR(CP_DETAIL_KEY) | A_BOLD);
    mvprintw(3, 8, "%s", unit.c_str());

    ensureExecPath(s);
    std::string bin = s.execPath.empty() ? "-" : s.execPath;
    if ((int)bin.size() > width - 14) bin = bin.substr(0, (std::size_t)std::max(0, width - 17)) + "...";
    attron(COLOR_PAIR(CP_DETAIL_KEY) | A_BOLD);
    mvprintw(4, 2, "Exec:");
    attroff(COLOR_PAIR(CP_DETAIL_KEY) | A_BOLD);
    mvprintw(4, 8, "%s", bin.c_str());

    const char* pid = (s.mainPid.empty() || s.mainPid == "0") ? "-" : s.mainPid.c_str();
    mvprintw(5, 2, "%s / %s   PID %s   load %s",
             s.activeState.c_str(), s.subState.c_str(), pid, s.loadState.c_str());

    char nowLine[80];
    snprintf(nowLine, sizeof(nowLine), "Live 0.2s  CPU %5.1f%%   RAM %s   pinned %s",
             liveCpu,
             liveMem > 0 ? fmtMem((unsigned long long)liveMem).c_str() : "-",
             pinned ? "yes" : "no");
    attron(A_BOLD);
    mvprintw(6, 2, "%s", nowLine);
    attroff(A_BOLD);
    mvhline(7, 0, ACS_HLINE, width);

    const int chartAreaTop    = 8;
    const int chartAreaBottom = height - 3;
    ChartLayout layout        = computeChartLayout(width, height, chartAreaTop, chartAreaBottom);

    char ramVal[32], cpuVal[32];
    if (liveMem > 0)
        snprintf(ramVal, sizeof(ramVal), "%s", fmtMem((unsigned long long)liveMem).c_str());
    else
        snprintf(ramVal, sizeof(ramVal), "-");
    snprintf(cpuVal, sizeof(cpuVal), "%.1f%%", liveCpu);

    drawPanelChart(layout.ram, stats.memHistory, stats.scaleMemMax, "RAM", ramVal, CP_CHART_RAM, false);
    drawPanelChart(layout.cpu, stats.cpuHistory, stats.scaleCpuMax, "CPU", cpuVal, CP_CHART_CPU, true);

    if (chartAreaBottom < height - 2) {
        std::string meta = "Started: " + (s.startedAt.empty() ? "-" : s.startedAt);
        if ((int)meta.size() > width - 4) meta = meta.substr(0, (std::size_t)width - 7) + "...";
        attron(COLOR_PAIR(CP_CHART_GRID) | A_DIM);
        mvprintw(chartAreaBottom, 2, "%s", meta.c_str());
        attroff(COLOR_PAIR(CP_CHART_GRID) | A_DIM);
    }

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
// Find dialog (F key) — incremental search over unit name / description
// ---------------------------------------------------------------------------
static std::string lowerCopy(std::string s) {
    for (char& c : s) c = (char)std::tolower((unsigned char)c);
    return s;
}

static bool serviceMatchesQuery(const Service& s, const std::string& qLower) {
    if (qLower.empty()) return false;
    return lowerCopy(s.unit).find(qLower) != std::string::npos ||
           lowerCopy(s.description).find(qLower) != std::string::npos;
}

static std::vector<int> findMatchingServices(const std::vector<Service>& svcs, const std::string& query) {
    std::vector<int> out;
    const std::string q = lowerCopy(trim(query));
    if (q.empty()) return out;
    for (int i = 0; i < (int)svcs.size(); ++i)
        if (serviceMatchesQuery(svcs[i], q)) out.push_back(i);
    return out;
}

static bool runFindDialog(const std::vector<Service>& svcs, int& selSvc) {
    if (svcs.empty()) return false;

    timeout(-1);
    curs_set(1);

    std::string query;
    int pick = 0;
    bool accepted = false;

    WINDOW* win = nullptr;
    int dlgW = 0, dlgH = 0, dlgY = 0, dlgX = 0;

    auto destroyWin = [&]() {
        if (win) { delwin(win); win = nullptr; }
    };

    auto createWin = [&]() {
        destroyWin();
        int scrH = 0, scrW = 0;
        getmaxyx(stdscr, scrH, scrW);
        dlgW = std::clamp(std::min(scrW - 4, 72), 32, std::max(32, scrW - 2));
        dlgH = std::clamp(std::min(scrH - 2, 18), 10, std::max(10, scrH - 2));
        dlgY = (scrH - dlgH) / 2;
        dlgX = (scrW - dlgW) / 2;
        win = newwin(dlgH, dlgW, dlgY, dlgX);
        keypad(win, TRUE);
    };

    auto drawDlg = [&](const std::vector<int>& matches) {
        werase(win);
        box(win, 0, 0);

        wattron(win, COLOR_PAIR(CP_DETAIL_KEY) | A_BOLD);
        mvwprintw(win, 1, 2, " Find service ");
        wattroff(win, COLOR_PAIR(CP_DETAIL_KEY) | A_BOLD);

        mvwprintw(win, 2, 2, "> %-*s", dlgW - 6, query.c_str());
        wmove(win, 2, 4 + (int)query.size());

        for (int i = 1; i < dlgW - 1; ++i)
            mvwaddch(win, 3, i, ACS_HLINE | COLOR_PAIR(CP_CHART_GRID) | A_DIM);

        const int listY   = 4;
        const int listMax = std::max(1, dlgH - 7);

        if (query.empty()) {
            wattron(win, A_DIM);
            mvwprintw(win, listY, 2, "Type unit or description...");
            wattroff(win, A_DIM);
        } else if (matches.empty()) {
            wattron(win, COLOR_PAIR(CP_ERR));
            mvwprintw(win, listY, 2, "No matches");
            wattroff(win, COLOR_PAIR(CP_ERR));
        } else {
            pick = std::clamp(pick, 0, (int)matches.size() - 1);
            int first = 0;
            if (pick >= listMax) first = pick - listMax + 1;

            for (int row = 0; row < listMax && first + row < (int)matches.size(); ++row) {
                const int mi = first + row;
                const Service& s = svcs[matches[mi]];
                const bool sel = (mi == pick);
                const bool active = s.activeState == "active";

                if (sel) wattron(win, A_REVERSE);

                std::string unit = s.unit;
                if ((int)unit.size() > dlgW - 18) unit = unit.substr(0, (std::size_t)std::max(0, dlgW - 21)) + "...";

                if (!sel) {
                    if (active) wattron(win, COLOR_PAIR(CP_SVC_ACTIVE));
                    else        wattron(win, COLOR_PAIR(CP_SVC_DEAD) | A_DIM);
                }
                mvwprintw(win, listY + row, 2, " %s %-*s",
                          g_pinned.count(s.unit) ? "[*]" : "   ",
                          dlgW - 16, unit.c_str());
                if (!sel) {
                    if (active) wattroff(win, COLOR_PAIR(CP_SVC_ACTIVE));
                    else        wattroff(win, COLOR_PAIR(CP_SVC_DEAD) | A_DIM);
                }

                if (sel) wattroff(win, A_REVERSE);
            }

            wattron(win, A_DIM);
            mvwprintw(win, dlgH - 3, 2, "%zu match(es)", matches.size());
            wattroff(win, A_DIM);
        }

        wattron(win, COLOR_PAIR(CP_KEYBINDS) | A_DIM);
        mvwprintw(win, dlgH - 2, 2, "ENTER:go  ESC:cancel  UP/DOWN:select");
        wattroff(win, COLOR_PAIR(CP_KEYBINDS) | A_DIM);
        wrefresh(win);
    };

    createWin();

    for (bool running = true; running; ) {
        std::vector<int> matches = findMatchingServices(svcs, query);
        drawDlg(matches);

        const int ch = wgetch(win);
        if (ch == KEY_RESIZE) {
            createWin();
            continue;
        }
        if (ch == 27) {
            running = false;
        } else if (ch == '\n' || ch == KEY_ENTER || ch == 10 || ch == 13) {
            if (!matches.empty()) {
                selSvc = matches[pick];
                accepted = true;
                running = false;
            }
        } else if (ch == KEY_UP) {
            if (!matches.empty()) pick = std::max(0, pick - 1);
        } else if (ch == KEY_DOWN) {
            if (!matches.empty()) pick = std::min((int)matches.size() - 1, pick + 1);
        } else if (ch == 127 || ch == KEY_BACKSPACE || ch == '\b') {
            if (!query.empty()) {
                query.pop_back();
                pick = 0;
            }
        } else if (ch >= 32 && ch < 127) {
            query.push_back((char)ch);
            pick = 0;
        }
    }

    destroyWin();
    touchwin(stdscr);
    curs_set(0);
    return accepted;
}

// ---------------------------------------------------------------------------
// Help dialog (? key)
// ---------------------------------------------------------------------------
static void runHelpDialog(bool /*inDetails*/) {
    timeout(-1);
    curs_set(0);

    std::vector<std::string> lines;
    lines.push_back("mdsys " MDSYS_VERSION " — keyboard shortcuts");
    lines.push_back("");
    lines.push_back("List view");
    lines.push_back("  UP/DOWN, w/j     navigate");
    lines.push_back("  PgUp/PgDn        page up / down");
    lines.push_back("  Enter            open service details");
    lines.push_back("  F                find service");
    lines.push_back("  R  S  K          restart / start / stop");
    lines.push_back("  P                pin / unpin service");
    lines.push_back("  C                console log (journalctl)");
    lines.push_back("  V                live console (follow, journalctl -f)");
    lines.push_back("  Tab              toggle system / user mode");
    lines.push_back("  U                refresh service list");
    lines.push_back("  Q                quit");
    lines.push_back("");
    lines.push_back("Details view");
    lines.push_back("  Enter / Q / Esc  back to list");
    lines.push_back("  R  S  K          restart / start / stop");
    lines.push_back("  P                pin / unpin");
    lines.push_back("  C                console log");
    lines.push_back("  V                live console (follow)");
    lines.push_back("  Tab / U          mode toggle / refresh");
    lines.push_back("  Charts           live CPU & RAM (0.2s)");
    lines.push_back("");
    lines.push_back("Find dialog (F)");
    lines.push_back("  type text        filter by name / description");
    lines.push_back("  UP/DOWN          select match");
    lines.push_back("  Enter            jump to service");
    lines.push_back("  Esc              cancel");

    WINDOW* win = nullptr;
    int dlgW = 0, dlgH = 0, scroll = 0;

    auto destroyWin = [&]() {
        if (win) { delwin(win); win = nullptr; }
    };

    auto createWin = [&]() {
        destroyWin();
        int scrH = 0, scrW = 0;
        getmaxyx(stdscr, scrH, scrW);
        dlgW = std::clamp(std::min(scrW - 4, 64), 36, std::max(36, scrW - 2));
        dlgH = std::clamp(std::min(scrH - 2, 24), 12, std::max(12, scrH - 2));
        const int dlgY = (scrH - dlgH) / 2;
        const int dlgX = (scrW - dlgW) / 2;
        win = newwin(dlgH, dlgW, dlgY, dlgX);
        keypad(win, TRUE);
        scroll = std::clamp(scroll, 0, std::max(0, (int)lines.size() - (dlgH - 4)));
    };

    auto drawDlg = [&]() {
        werase(win);
        box(win, 0, 0);

        wattron(win, COLOR_PAIR(CP_DETAIL_KEY) | A_BOLD);
        mvwprintw(win, 1, 2, " Help ");
        wattroff(win, COLOR_PAIR(CP_DETAIL_KEY) | A_BOLD);

        const int viewH = dlgH - 4;
        scroll = std::clamp(scroll, 0, std::max(0, (int)lines.size() - viewH));

        for (int row = 0; row < viewH; ++row) {
            const int idx = scroll + row;
            if (idx >= (int)lines.size()) break;
            const std::string& line = lines[(std::size_t)idx];
            if (line.empty()) continue;
            const bool header = line.find("  ") != 0 && line.find("mdsys") != 0;
            if (header)
                wattron(win, COLOR_PAIR(CP_CATEGORY) | A_BOLD);
            else
                wattron(win, A_NORMAL);
            mvwprintw(win, 2 + row, 2, "%-*s", dlgW - 4,
                      line.size() > (std::size_t)dlgW - 4
                          ? (line.substr(0, (std::size_t)std::max(0, dlgW - 7)) + "...").c_str()
                          : line.c_str());
            if (header)
                wattroff(win, COLOR_PAIR(CP_CATEGORY) | A_BOLD);
            else
                wattroff(win, A_NORMAL);
        }

        wattron(win, COLOR_PAIR(CP_KEYBINDS) | A_DIM);
        mvwprintw(win, dlgH - 2, 2, "UP/DOWN:scroll  Esc/?/Q:close");
        wattroff(win, COLOR_PAIR(CP_KEYBINDS) | A_DIM);
        wrefresh(win);
    };

    createWin();

    for (bool running = true; running; ) {
        drawDlg();
        const int ch = wgetch(win);
        if (ch == KEY_RESIZE) {
            createWin();
            continue;
        }
        const int viewH = dlgH - 4;
        if (ch == 27 || ch == '?' || ch == 'q' || ch == 'Q')
            running = false;
        else if (ch == KEY_UP)
            scroll = std::max(0, scroll - 1);
        else if (ch == KEY_DOWN)
            scroll = std::min(std::max(0, (int)lines.size() - viewH), scroll + 1);
        else if (ch == KEY_PPAGE)
            scroll = std::max(0, scroll - viewH);
        else if (ch == KEY_NPAGE)
            scroll = std::min(std::max(0, (int)lines.size() - viewH), scroll + viewH);
    }

    destroyWin();
    touchwin(stdscr);
    curs_set(0);
}

// ---------------------------------------------------------------------------
// Console log viewers  (suspend ncurses, run journalctl | less, restore)
//   C = snapshot console (less -R +G)
//   V = live/follow console (less -R +F)
// ---------------------------------------------------------------------------
static void openConsole(const std::string& unit) {
    // Build journalctl command.  System mode uses no --user flag.
    // less -R: ANSI colours; +G: open scrolled to the bottom (newest lines).
    std::string jcmd = pfx() + "journalctl " + flag() +
                       "-u " + shellQ(unit) + " -n 500 --no-pager 2>&1 | less -R +G";

    def_prog_mode();   // save ncurses terminal state
    endwin();          // restore normal terminal

    system(jcmd.c_str());

    reset_prog_mode(); // restore ncurses state
    refresh();         // repaint
}

static void openLiveConsole(const std::string& unit) {
    // Follow mode: less -R +F opens in "follow" (tail -f style). New lines
    // stream in continuously. q/ESC detaches (Ctrl-C stops follow but keeps
    // browsing); useful to inspect service logs in real time.
    std::string jcmd = pfx() + "journalctl " + flag() +
                       "-u " + shellQ(unit) + " -f -n 500 --no-pager 2>&1 | less -R +F";

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
static std::string systemdQuoteArg(const std::string& arg) {
    if (arg.find_first_of(" \t\"\\") == std::string::npos) return arg;
    std::string out = "\"";
    for (char c : arg) {
        if (c == '\\' || c == '"') out += '\\';
        out += c;
    }
    return out + "\"";
}

static std::string buildExecStartFromBinary(const std::string& progArg,
                                            const std::vector<std::string>& extraArgs) {
    char resolvedExec[PATH_MAX] = {};
    if (!realpath(progArg.c_str(), resolvedExec)) {
        char cwd2[PATH_MAX] = {};
        if (!getcwd(cwd2, sizeof(cwd2))) {
            perror("getcwd");
            return "";
        }
        std::string abs = std::string(cwd2) + "/" + progArg;
        abs.copy(resolvedExec, sizeof(resolvedExec) - 1);
        resolvedExec[sizeof(resolvedExec) - 1] = '\0';
    }
    std::string execStart = resolvedExec;
    for (const auto& a : extraArgs) {
        execStart += ' ';
        execStart += systemdQuoteArg(a);
    }
    return execStart;
}

// Build ExecStart for -c mode (e.g. "npm start"). Prefer absolute path to argv[0].
static std::string buildExecStartFromCommand(const std::vector<std::string>& parts) {
    if (parts.empty()) return "";

    std::string whichOut = trim(runCmd("command -v " + shellQ(parts[0]) + " 2>/dev/null"));
    if (!whichOut.empty()) {
        std::string execStart = whichOut;
        for (std::size_t i = 1; i < parts.size(); ++i) {
            execStart += ' ';
            execStart += systemdQuoteArg(parts[i]);
        }
        return execStart;
    }

    std::string joined;
    for (std::size_t i = 0; i < parts.size(); ++i) {
        if (i > 0) joined += ' ';
        joined += parts[i];
    }
    return "/bin/sh -c " + shellQ(joined);
}

static int registerServiceExec(const std::string& rawName, const std::string& execStart) {
    if (execStart.empty()) {
        fprintf(stderr, "error: empty ExecStart\n");
        return 1;
    }

    std::string name = rawName;
    if (name.size() > 8 && name.substr(name.size() - 8) == ".service")
        name = name.substr(0, name.size() - 8);

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
        name.c_str(), cwd, execStart.c_str(), username.c_str());
    fclose(f);

    // ── Reload systemd ───────────────────────────────────────────────────
    std::string reloadCmd = "systemctl " + systemctlFlag + "daemon-reload";
    (void)system(reloadCmd.c_str());

    // ── Auto-pin the newly created service ───────────────────────────────
    loadPinned();
    g_pinned.insert(name + ".service");
    {
        std::string pinErr;
        if (!savePinned(&pinErr))
            fprintf(stderr, "warning: pin not saved: %s\n", pinErr.c_str());
    }

    // ── Print summary ─────────────────────────────────────────────────────
    printf("\n");
    printf("  Created:          %s\n",  serviceFilePath.c_str());
    printf("  ExecStart:        %s\n",  execStart.c_str());
    printf("  WorkingDirectory: %s\n",  cwd);
    printf("  User:             %s\n\n", username.c_str());
    printf("  Start now:   systemctl %sstart  %s\n", systemctlFlag.c_str(), name.c_str());
    printf("  Auto-start:  systemctl %senable %s\n", systemctlFlag.c_str(), name.c_str());
    printf("  Check logs:  journalctl %s-u %s -f\n\n",
           systemctlFlag.empty() ? "" : "--user ", name.c_str());
    return 0;
}

static int registerService(const std::string& progArg, const std::string& rawName,
                           const std::vector<std::string>& extraArgs = {}) {
    std::string execStart = buildExecStartFromBinary(progArg, extraArgs);
    if (execStart.empty()) return 1;
    return registerServiceExec(rawName, execStart);
}

static int registerServiceCommand(const std::vector<std::string>& commandParts,
                                  const std::string& rawName) {
    std::string execStart = buildExecStartFromCommand(commandParts);
    if (execStart.empty()) {
        fprintf(stderr, "error: empty command\n");
        return 1;
    }
    return registerServiceExec(rawName, execStart);
}

// ---------------------------------------------------------------------------
// Main
// ---------------------------------------------------------------------------
static void printHelp() {
    printf(
        "mdsys " MDSYS_VERSION " — systemd TUI service manager\n"
        "\n"
        "Usage:\n"
        "  mdsys                              launch TUI\n"
        "  mdsys <binary> <name>                register binary (simple)\n"
        "  mdsys -b <binary> -n <name>          register binary with flags\n"
        "  mdsys -b <binary> [args] -n <name>   binary + extra args\n"
        "  mdsys -c <cmd> [args] -n <name>      register shell command\n"
        "  mdsys -h | --help                    show this help\n"
        "  mdsys -v | --version                 show version\n"
        "\n"
        "Flags:\n"
        "  -b, --binary  <path>   path to a binary executable\n"
        "  -c, --command           run a command (words until -n are the command)\n"
        "  -n, --name    <name>   service name (without .service)\n"
        "  -h, --help             show this message\n"
        "  -v, --version          show version\n"
        "\n"
        "Examples:\n"
        "  mdsys ./myapp api-service\n"
        "  mdsys -b ./myapp -n api-service\n"
        "  mdsys -b ./myapp --port 8080 -n api-service\n"
        "  mdsys -c npm start -n discordbot\n"
        "  mdsys -c python3 app.py --port 3000 -n myapp\n"
        "\n"
        "With -b, tokens between -b and -n (except flags) are passed to the binary.\n"
        "With -c, tokens after -c until -n form the command (resolved via PATH).\n"
    );
}

int main(int argc, char* argv[]) {
    // ── CLI mode ──────────────────────────────────────────────────────────
    if (argc > 1) {
        std::string first = argv[1];

        if (first == "-h" || first == "--help")    { printHelp(); return 0; }
        if (first == "-v" || first == "--version") {
            printf("mdsys " MDSYS_VERSION "\n"); return 0;
        }

        // Simple mode: mdsys <binary> <name>  (neither arg starts with '-')
        if (argc == 3 && first[0] != '-') {
            return registerService(argv[1], argv[2]);
        }

        // Standard mode: -b <binary> [args] -n <name>  or  -c <cmd> [args] -n <name>
        enum class RegMode { None, Binary, Command };
        RegMode mode = RegMode::None;
        std::string binArg, nameArg;
        std::vector<std::string> extraArgs;
        std::vector<std::string> commandParts;

        for (int i = 1; i < argc; ++i) {
            std::string a = argv[i];
            if (a == "-b" || a == "--binary") {
                if (i + 1 >= argc) {
                    fprintf(stderr, "error: -b requires a path\n");
                    printHelp();
                    return 1;
                }
                if (mode == RegMode::Command) {
                    fprintf(stderr, "error: use only one of -b or -c\n");
                    return 1;
                }
                mode = RegMode::Binary;
                binArg = argv[++i];
            } else if (a == "-c" || a == "--command") {
                if (mode == RegMode::Binary) {
                    fprintf(stderr, "error: use only one of -b or -c\n");
                    return 1;
                }
                mode = RegMode::Command;
            } else if (a == "-n" || a == "--name") {
                if (i + 1 >= argc) {
                    fprintf(stderr, "error: -n requires a service name\n");
                    printHelp();
                    return 1;
                }
                nameArg = argv[++i];
            } else if (a == "-h" || a == "--help") {
                printHelp(); return 0;
            } else if (a == "-v" || a == "--version") {
                printf("mdsys " MDSYS_VERSION "\n"); return 0;
            } else if (mode == RegMode::Command) {
                commandParts.push_back(a);
            } else if (mode == RegMode::Binary) {
                extraArgs.push_back(a);
            } else {
                fprintf(stderr, "error: unexpected argument '%s' (use -b or -c)\n", a.c_str());
                printHelp();
                return 1;
            }
        }

        if (!nameArg.empty()) {
            if (mode == RegMode::Binary && !binArg.empty())
                return registerService(binArg, nameArg, extraArgs);
            if (mode == RegMode::Command && !commandParts.empty())
                return registerServiceCommand(commandParts, nameArg);
            if (mode == RegMode::Command)
                fprintf(stderr, "error: -c requires a command before -n\n");
            else if (mode == RegMode::Binary)
                fprintf(stderr, "error: -b requires a binary path\n");
            else
                fprintf(stderr, "error: use -b or -c with -n\n");
            printHelp();
            return 1;
        }

        fprintf(stderr, "error: missing -n <name>\n\n");
        printHelp();
        return 1;
    }

    g_user       = resolveUserContext();
    g_systemMode = (g_user.processUid == 0);
    loadPinned();

    initscr();
    cbreak();
    noecho();
    keypad(stdscr, TRUE);
    curs_set(0);
    leaveok(stdscr, TRUE);
    scrollok(stdscr, FALSE);
    idlok(stdscr, FALSE);
    nodelay(stdscr, TRUE);   // non-blocking getch during loading

    if (has_colors()) initColors();

    // Progressive load: show list ASAP, enrich pinned first, rest in background.
    ServiceCatalog catalog;
    std::atomic<bool> loadCancel{false};
    std::thread loader([&]() { loadServicesProgressive(catalog, loadCancel); });

    int frame = 0;
    bool aborted = false;
    while (!catalog.listReady.load(std::memory_order_acquire)) {
        int W = 0, H = 0;
        getmaxyx(stdscr, H, W);
        clear();
        drawLoadingScreen(frame++, W, H);
        refresh();
        std::this_thread::sleep_for(std::chrono::milliseconds(40));
        int ch = getch();
        if (ch == 'q' || ch == 'Q') {
            aborted = true;
            loadCancel.store(true, std::memory_order_release);
            break;
        }
    }

    if (aborted) {
        loader.join();
        endwin();
        return 0;
    }

    nodelay(stdscr, FALSE);

    std::string err;
    std::vector<Service> svcs;
    catalog.snapshot(svcs, err);
    uint64_t seenGen = catalog.generation.load(std::memory_order_acquire);

    std::vector<DisplayRow> rows = buildRows(svcs);
    std::string msg = "Loaded " + std::to_string(svcs.size()) + " service(s).";
    if (!catalog.enrichDone.load(std::memory_order_acquire))
        msg += "  (loading details...)";

    int  selSvc     = svcs.empty() ? 0 : rows[0].kind == RowKind::Service ? rows[0].svcIdx : nextSvc(rows, 0, 0);
    bool inDetails  = false;
    LiveStats liveStats;
    std::mutex liveStatsMtx;
    std::thread liveStatsThread;
    std::atomic<bool> liveStatsRun{false};

    auto stopLiveStats = [&]() {
        liveStatsRun.store(false);
        if (liveStatsThread.joinable()) liveStatsThread.join();
    };

    auto startLiveStats = [&](const std::string& unit) {
        stopLiveStats();
        {
            std::lock_guard<std::mutex> lk(liveStatsMtx);
            liveStats = LiveStats{};
        }
        liveStatsRun.store(true);
        liveStatsThread = std::thread([&, unit]() {
            while (liveStatsRun.load()) {
                {
                    std::lock_guard<std::mutex> lk(liveStatsMtx);
                    tickLiveStats(unit, liveStats);
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
            }
        });
    };

    auto applyCatalogSnapshot = [&](bool updateStatusMsg = true) {
        const std::string keepUnit = (!svcs.empty() && selSvc >= 0 && selSvc < (int)svcs.size())
            ? svcs[selSvc].unit : std::string();
        catalog.snapshot(svcs, err);
        seenGen = catalog.generation.load(std::memory_order_acquire);
        sortServices(svcs);
        rows = buildRows(svcs);

        if (updateStatusMsg) {
            msg = "Loaded " + std::to_string(svcs.size()) + " service(s).";
            if (!catalog.enrichDone.load(std::memory_order_acquire))
                msg += "  (loading details...)";
        }

        if (!keepUnit.empty()) {
            selSvc = 0;
            for (int i = 0; i < (int)svcs.size(); ++i) {
                if (svcs[i].unit == keepUnit) { selSvc = i; break; }
            }
        } else if (!svcs.empty()) {
            selSvc = nextSvc(rows, 0, 0);
        }
        if (!svcs.empty()) selSvc = std::clamp(selSvc, 0, (int)svcs.size() - 1);
    };

    auto stopLoader = [&]() {
        loadCancel.store(true, std::memory_order_release);
        if (loader.joinable()) loader.join();
        loadCancel.store(false, std::memory_order_release);
        {
            std::lock_guard<std::mutex> lk(catalog.mtx);
            catalog.svcs.clear();
            catalog.err.clear();
        }
        catalog.listReady.store(false, std::memory_order_release);
        catalog.enrichDone.store(false, std::memory_order_release);
        catalog.generation.store(0, std::memory_order_release);
        seenGen = 0;
    };

    auto reload = [&](const std::string& newMsg = "") {
        inDetails = false;
        stopLiveStats();
        stopLoader();
        loader = std::thread([&]() { loadServicesProgressive(catalog, loadCancel); });

        nodelay(stdscr, TRUE);
        int f = 0;
        while (!catalog.listReady.load(std::memory_order_acquire)) {
            int W = 0, H = 0; getmaxyx(stdscr, H, W);
            clear(); drawLoadingScreen(f++, W, H); refresh();
            std::this_thread::sleep_for(std::chrono::milliseconds(40));
        }
        nodelay(stdscr, FALSE);
        applyCatalogSnapshot(false);
        if (newMsg == "mode")
            msg = std::string(g_systemMode ? "System" : "User") + " mode — " +
                  std::to_string(svcs.size()) + " service(s).";
        else if (newMsg == "Refreshed")
            msg = "Refreshed — " + std::to_string(svcs.size()) + " service(s).";
        else if (!newMsg.empty())
            msg = newMsg;
        else
            msg = "Loaded " + std::to_string(svcs.size()) + " service(s).";
        if (!catalog.enrichDone.load(std::memory_order_acquire))
            msg += "  (loading details...)";
    };

    auto pinToggle = [&]() {
        if (svcs.empty()) return;
        const std::string u = svcs[selSvc].unit;
        if (g_pinned.count(u)) { g_pinned.erase(u);  msg = "Unpinned: " + u; }
        else                   { g_pinned.insert(u); msg = "Pinned: "   + u; }
        std::string pinErr;
        if (!savePinned(&pinErr)) { err = pinErr; msg.clear(); }
        else err.clear();
        sortServices(svcs);
        rows = buildRows(svcs);
        for (int i = 0; i < (int)svcs.size(); ++i)
            if (svcs[i].unit == u) { selSvc = i; break; }
    };

    while (true) {
        // Pull progressive enrich updates without blocking UI.
        const uint64_t gen = catalog.generation.load(std::memory_order_acquire);
        if (gen != seenGen && !inDetails)
            applyCatalogSnapshot(msg.find("loading details") != std::string::npos ||
                                 msg.find("Loaded ") == 0 ||
                                 msg.find("Refreshed") == 0 ||
                                 msg.find(" mode — ") != std::string::npos);

        int W = 0, H = 0;
        getmaxyx(stdscr, H, W);

        drawTitleBar(W);
        wipeRows(1, H - 1, W);

        if (svcs.empty()) {
            mvhline(2, 0, ACS_HLINE, W);
            mvprintw(4, 2, "No services found.");
            if (!err.empty()) { attron(COLOR_PAIR(CP_ERR)); mvprintw(5, 2, "%s", err.c_str()); attroff(COLOR_PAIR(CP_ERR)); }
            mvprintw(7, 2, "TAB: toggle system/user    U: refresh    ?: help    Q: quit");
        } else if (inDetails) {
            LiveStats statsSnap;
            {
                std::lock_guard<std::mutex> lk(liveStatsMtx);
                statsSnap = liveStats;
            }
            drawDetails(svcs[selSvc], W, H - 1, msg, err, statsSnap);
        } else {
            drawList(svcs, rows, selSvc, W, H - 1, msg, err);
        }

        drawKeybindBar(H - 1, W, inDetails);
        refresh();

        const bool enriching = !catalog.enrichDone.load(std::memory_order_acquire);
        if (inDetails)
            timeout(200);
        else if (enriching)
            timeout(100);
        else
            timeout(-1);

        int ch = getch();
        if (ch == ERR) {
            if (inDetails || enriching) continue;
            continue;
        }

        if (ch == 'q' || ch == 'Q') {
            if (inDetails) {
                inDetails = false;
                stopLiveStats();
                continue;
            }
            break;
        }

        // Tab: toggle system / user mode
        if (ch == '\t') {
            g_systemMode = !g_systemMode;
            inDetails = false;
            stopLiveStats();
            selSvc = 0;
            reload("mode");
            if (!svcs.empty()) selSvc = nextSvc(rows, 0, 0);
            continue;
        }

        // Refresh
        if (ch == 'u' || ch == 'U') {
            reload("Refreshed");
            continue;
        }

        if (ch == '?') {
            runHelpDialog(inDetails);
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
            else if (ch == '\n' || ch == KEY_ENTER || ch == 10 || ch == 13) {
                inDetails = true;
                startLiveStats(svcs[selSvc].unit);
            }
            else if (ch == 'f' || ch == 'F') {
                if (runFindDialog(svcs, selSvc))
                    msg = "Find: " + svcs[selSvc].unit;
            }
            else if (ch == 'p' || ch == 'P') {
                pinToggle();
            }
            else if (ch == 'c' || ch == 'C') {
                openConsole(svcs[selSvc].unit);
            }
            else if (ch == 'v' || ch == 'V') {
                openLiveConsole(svcs[selSvc].unit);
            }
            else if (ch == 'r' || ch == 'R' || ch == 's' || ch == 'S' || ch == 'k' || ch == 'K') {
                std::string act = (ch=='r'||ch=='R') ? "restart" : (ch=='s'||ch=='S') ? "start" : "stop";
                int ec = doAction(svcs[selSvc].unit, act);
                std::this_thread::sleep_for(std::chrono::milliseconds(300));
                if (ec == 0) { err.clear(); reload(act + " OK: " + svcs[selSvc].unit); }
                else         { reload(); err = act + " FAILED (exit " + std::to_string(ec) + "): " + svcs[selSvc].unit; }
            }
        } else {
            if (ch == '\n' || ch == KEY_ENTER || ch == 10 || ch == 13 || ch == 27) {
                inDetails = false;
                stopLiveStats();
            }
            else if (ch == 'p' || ch == 'P') {
                pinToggle();
            }
            else if (ch == 'c' || ch == 'C') {
                openConsole(svcs[selSvc].unit);
            }
            else if (ch == 'v' || ch == 'V') {
                openLiveConsole(svcs[selSvc].unit);
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

    stopLiveStats();
    stopLoader();
    endwin();
    return 0;
}
