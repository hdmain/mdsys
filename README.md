# mdsys

A terminal UI (TUI) manager for `systemd` services on Linux/WSL.  
Displays user or system services with status, RAM usage, and lets you control them from the keyboard.

## Install via apt

```bash
echo "deb [trusted=yes] https://hdmain.github.io/mdsys ./" \
  | sudo tee /etc/apt/sources.list.d/mdsys.list
sudo apt update
sudo apt install mdsys
```

Then run:

```bash
mdsys
```

## Register a program as a service

```bash
mdsys <program> <name>
```

Example — register a binary and immediately start it:

```bash
mdsys ./prot httpprot
systemctl start  httpprot
systemctl enable httpprot   # auto-start on boot
```

This creates `/etc/systemd/system/httpprot.service` (or `~/.config/systemd/user/` when not root), reloads systemd, and pins the service to the top of the TUI automatically.

## Keybindings

| Key | Action |
|-----|--------|
| `↑` / `↓` or `W` / `J` | Navigate |
| `Enter` | Open service details |
| `R` | Restart selected service |
| `S` | Start selected service |
| `K` | Stop selected service |
| `P` | Pin / unpin (persisted to `~/.config/mdsys/pinned`) |
| `C` | Open console log (`journalctl … \| less`) |
| `Tab` | Toggle system ↔ user mode |
| `U` | Refresh list |
| `Q` | Quit |

## Features

- Lists system or user services (`systemctl` / `systemctl --user`)
- Shows active state, sub-state, and live RAM (`MemoryCurrent`)
- **Pinned** category — pin important services to the top; pins are saved across sessions
- **Details** view — description, PID, memory, start timestamp
- **Console** — opens `journalctl` output for the selected service in `less`
- Register any binary as a systemd service with one command
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

**Build a .deb package:**

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
cd build && cpack -G DEB
```

## License

MIT
