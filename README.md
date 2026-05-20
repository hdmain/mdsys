# mdsys

`mdsys` is a terminal UI (TUI) manager for `systemd --user` services on Linux/WSL.
It displays your user services, status, and memory usage, and lets you control services from the keyboard.

## Features

- Lists user services from `systemctl --user`
- Shows active state, sub-state, and current RAM usage (`MemoryCurrent`)
- Details screen for a selected service (PID, start time, description)
- Quick actions:
  - `Enter`: open service details / return
  - `R`: restart selected service
  - `S`: start selected service
  - `K`: stop selected service
  - `U`: refresh list
  - `Q`: quit

## Build (WSL/Linux)

Install dependencies:

```bash
sudo apt update
sudo apt install -y build-essential cmake libncurses-dev
```

Build:

```bash
cmake -S . -B build
cmake --build build -j
```

Run:

```bash
./build/mdsys
```

## WSL notes

Make sure systemd is enabled in WSL:

1. Edit `/etc/wsl.conf`:
   ```ini
   [boot]
   systemd=true
   ```
2. Restart WSL from Windows:
   ```powershell
   wsl --shutdown
   ```
3. Start WSL again and run `mdsys`.
