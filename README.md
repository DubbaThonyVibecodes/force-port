# Force port on program utility

Forces a Windows game onto the network interface you choose. Written for
Command & Conquer 3: Tiberium Wars, which ignores its own "IP address" setting
and binds to the numerically lowest address of the machine.

Nothing in the game's files is modified. A small DLL is loaded into the running
game and corrects the game's network calls as they happen.

You may try to use it with other games that suffer from similar networking issues.
Be warned, if any anticheat is present it will likely detect injected DLL.

## Download

Head to github releases section and download newest release .zip file containing 
exe, injected dll and ini file. [1.0.0](https://github.com/DubbaThonyVibecodes/force-port/releases/download/1.0.0/portrebind.zip)

## Install

Copy these three files from `dist/` to any folder on the Windows machine.
I recommend either creating C:\Program Files\ directory for installing for all
users, or one in `%AppData%`, to install for single user. Location doesn't
matter, as long as windows allows you to access it. DO NOT put it into game folder.

You need these 3 files:

    portrebind.exe   the launcher
    portrebind.dll   the part that ends up inside the game
    portrebind.ini   settings

Needs Windows 10 or 11.

## Set up

### Steam

Find game to apply the patch to, right click on it, properties, in general tab
search for sertup options. In text field type in path to this program and add
%command%. If you use additional parameters to game already, add them after
%command%. For example, on my personal system it is:
`G:\tools\cnc3portfix\portrebind.exe %command% -runver 1.9`

### Standalone

Set `game` value in `portrebind.ini` to the full path of `CNC3.exe`.

## Run

### Steam

Run game normally

### Standalone

Double click the portrebind.exe

### Backup option

If you use launcher that doesnt allow setting command, make a shortcut to
program that adds --wait or run `portrebind.exe --wait` from terminal.
It will wait for `cnc3game.dat` to appear in processes and it attaches
to that.


# Do I need it?

If you start game in LAN and it looks like it should be working, but you don't
see others in lan, and you have more than 1 network interface, this tool may
solve your problem.

## How to check

First you need to check what ports game uses. Start the game without this
tool, go into the LAN lobby (the game opens its port only there), leave it
running, open up a power shell, and type in:

```powershell
$p = (Get-Process cnc3game*).Id
Get-NetUDPEndpoint -OwningProcess $p | Format-Table LocalAddress, LocalPort
```

For another game, replace `cnc3game*` with the name of its process.
`LocalAddress` is the interface the game is using.

If you can see your game not using your local networking interface,
such as 192.168.1.X, this is exactly the problem.
Note that 0.0.0.0 means "all interfaces" and is generally speaking
also correct, and shouldn't require this tool, but in some cases,
and command and conquer is such case, this is missleading since it
actually tries to use 0.0.0.0 but it doesn't actually talk to all
interfaces. 0.0.0.0 may be fine in other games.


# Checking that it works / troubleshooting

Tick "Write a log file" in the dialog (or set `log = 1` in the ini) and
everything portrebind changes is written to `portrebind.log` next to the exe.
A working run looks roughly like this (addresses and port are made up):

    21:04:11.120 [pid 4242] loaded into C:\...\RetailExe\1.9\cnc3game.dat (ip=192.168.1.0/24)
    21:04:19.871 [pid 4242] forcing local address 192.168.1.10
    21:04:19.875 [pid 4242] gethostbyname(own name): 3 address(es) replaced by 192.168.1.10
    21:04:19.880 [pid 4242] bind udp 10.0.1.2:8086 -> 192.168.1.10:8086, result 0 (error 0)

- **No `loaded into ...cnc3game.dat` line**: the DLL never got into the real game
  process. Use the `--wait` method.
- **`loaded` line but no `bind` lines**: the game has not opened its network
  port yet (enter the LAN lobby), or it uses a call that is not hooked.
- **Antivirus complains**: loading a DLL into another process is also what
  cheats and malware do, so heuristics may flag portrebind. Add an exclusion
  for its folder.
- **Online play broke** (if you use it and `ip` points at a LAN/VPN-only
  interface): set `rewrite_any = none`, then only binds to a wrong specific
  address are corrected.

# How it works

`portrebind.exe` starts the game suspended, makes it load `portrebind.dll`
(by starting a thread inside the game that calls `LoadLibraryW`), then lets it
run. The DLL patches these Windows functions inside the game process:

| Function | What the patch does |
|---|---|
| `bind` | A bind to another local address is redirected to the configured one. Loopback is left alone; `0.0.0.0` is handled per `rewrite_any`. |
| `sendto`, `connect` | A socket used without `bind` is bound to the configured address first. |
| `gethostbyname`, `getaddrinfo`, `GetAdaptersInfo`, `GetIpAddrTable` | When the game asks for the machine's addresses it only sees the configured one, so that is also what it announces to other players. |
| `CreateProcessInternalW` | Child processes get the DLL too. `CNC3.exe` is only a launcher; the game is `cnc3game.dat`. |

Only IPv4 is touched. The patching itself is done by
[MinHook](https://github.com/TsudaKageyu/minhook) (in `third_party/`, BSD licence).

Source map:

    src/dll.c        the hooks - the interesting part
    src/inject.c     loading a DLL into another process
    src/launcher.c   portrebind.exe
    src/dialog.c     the address dialog; its layout is in src/launcher.rc
    src/common.c     ini file and "which address" logic shared by both

## Build (on Linux)

Needs `make` and the mingw-w64 cross compiler (`i686-w64-mingw32-gcc`).

    make          # -> dist/portrebind.exe, .dll, .ini
    make test     # runs test/testapp.c through portrebind under Wine, in Docker,
                  # including clicking through the dialog on a fake screen

Everything is built 32-bit because the game is 32-bit; a DLL can only be loaded
into a process of the same bitness.
