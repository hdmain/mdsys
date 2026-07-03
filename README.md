# mdsys

A terminal UI (TUI) manager for `systemd` services on Linux/WSL.  
Displays user or system services with status, RAM usage, and lets you control them from the keyboard.

![mdsys preview](img/mdsyspreview.png)

## Install

### Debian / Ubuntu (apt)

```bash
echo "deb [trusted=yes] https://hdmain.github.io/mdsys ./" \
  | sudo tee /etc/apt/sources.list.d/mdsys.list
sudo apt update
sudo apt install mdsys
```

### Fedora / AlmaLinux / CentOS (dnf)

```bash
sudo tee /etc/yum.repos.d/mdsys.repo <<'EOF'
[mdsys]
name=mdsys
baseurl=https://hdmain.github.io/mdsys/rpm
enabled=1
gpgcheck=0
EOF
sudo dnf install mdsys
```

On CentOS 7 / older systems with `yum` only:

```bash
sudo yum install mdsys
```

### Arch Linux (pacman)

```bash
sudo tee -a /etc/pacman.conf <<'EOF'

[mdsys]
SigLevel = Optional TrustAll
Server = https://hdmain.github.io/mdsys/arch/
EOF
sudo pacman -Sy mdsys
```

Then run:

```bash
mdsys
```

## Register a program as a service

**Binary (simple):**

```bash
mdsys ./prot httpprot
```

**Binary with flags:**

```bash
mdsys -b ./myapp --port 8080 -n api-service
```

**Command** (npm, python, etc.):

```bash
mdsys -c npm start -n discordbot
```

Then start and enable:

```bash
systemctl start  discordbot
systemctl enable discordbot   # auto-start on boot
```

This creates `/etc/systemd/system/httpprot.service` (or `~/.config/systemd/user/` when not root), reloads systemd, and pins the service to the top of the TUI automatically.

## Keybindings

### List view

| Key | Action |
|-----|--------|
| `↑` / `↓` or `w` / `j` | Navigate |
| `Enter` | Open service details |
| `F` | Find service (incremental search) |
| `R` | Restart selected service |
| `S` | Start selected service |
| `K` | Stop selected service |
| `P` | Pin / unpin (persisted to `~/.config/mdsys/pinned`) |
| `C` | Open console log (`journalctl … \| less`) |
| `Tab` | Toggle system ↔ user mode |
| `U` | Refresh list |
| `Q` | Quit |
| `?` | Help |

### Details view

| Key | Action |
|-----|--------|
| `Enter` / `Q` / `Esc` | Back to list |
| `R` / `S` / `K` | Restart / start / stop |
| `P` | Pin / unpin |
| `C` | Console log |
| `Tab` | Toggle system ↔ user mode |
| `U` | Refresh list |
| `?` | Help |

Charts refresh live every 0.2 s (CPU and RAM).

### Find dialog (`F`)

| Key | Action |
|-----|--------|
| Type text | Filter by unit name or description (live) |
| `↑` / `↓` | Select match |
| `Enter` | Jump to selected service |
| `Esc` | Cancel |
| `Backspace` | Delete character |

## Features

- Lists system or user services (`systemctl` / `systemctl --user`)
- Shows active state, sub-state, and live RAM (`MemoryCurrent`)
- **Pinned** category — pin important services to the top; pins are saved across sessions
- **Find** — incremental search (`F`) by unit name or description
- **Details** view — executable path, PID, status, live CPU/RAM charts
- **Console** — opens `journalctl` output for the selected service in `less`
- Register any binary or command as a systemd service with one command
- Animated loading screen
- Color-coded TUI (green = active, dimmed = inactive, yellow = pinned)
- Auto-detects real user when run as root

## WSL notes

Make sure systemd is enabled in WSL before using mdsys.

1. Edit `/etc/wsl.conf`:
   ```ini
   [boot]
   systemd=true
   ```
2. Restart WSL from PowerShell:
   ```powershell
   wsl --shutdown
   ```
3. Open WSL again and verify:
   ```bash
   systemctl status
   # should show: State: running
   ```
4. Try mdsys:
   ```bash
   mdsys          # opens TUI — press Tab to switch system/user mode
   ```

## Build from source

**Dependencies (Ubuntu/Debian):**

```bash
sudo apt install build-essential cmake libncurses-dev
```

**Build:**

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
./build/mdsys
```

**Build packages:**

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
cd build && cpack -G DEB    # Debian/Ubuntu
cd build && cpack -G RPM    # Fedora/Alma/CentOS
cp packaging/arch/PKGBUILD . && makepkg -sf   # Arch Linux
```

## License

MIT
