#!/usr/bin/env python3
# voice_extractor.py - minimal name/team/mode extractor for the IranCoJ voice relay.
#
# Reads the Call of Juarez: Bound in Blood dedicated-server process memory and
# writes players.json (name, team, mode_id) for the voice relay's team routing.
# Auto-detects Linux (/proc) and Windows (ReadProcessMemory).
#
# USAGE:
#   python3 voice_extractor.py                 # defaults below
#   python3 voice_extractor.py <ip> <port>     # override query ip:port
#
# Config defaults (edit here or pass as args):
OUT_PATH   = "players.json"     # where to write (relay reads this)
QUERY_IP   = "127.0.0.1"        # DS status-query address (localhost on the DS host)
QUERY_PORT = 27632              # DS status-query port
INTERVAL   = 1.0                # seconds between updates
#
# Notes:
#  * On Windows the DS is 32-bit - use 32-bit Python + run as Administrator.
#  * On Linux/Docker (the usual case) /proc reads work with no such caveat.
# ---------------------------------------------------------------------------

import os, sys, json, time, struct, socket, platform

MODULE_NAME   = "CoJ2_x86_ds.dll"
CONTAINER_OFF = 0xA162C8        # [module+off] -> player-pointer array
STRIDE        = 4
MAX_SLOTS     = 32
OFF_TEAM      = 0x20
OFF_NAME      = 0x208
PROC_MATCH    = "CoJ2Game_x86_ds"    # process cmdline substring
MODE_OFF      = 0xBB                  # mode_id byte in the status reply
QUERY_PACKET  = bytes.fromhex("050000000001000000c5787100d0149eaa01a60c005a06")


# ---------- Linux backend ----------
class LinuxMem:
    def __init__(self, pid):
        self.mem = f"/proc/{pid}/mem"
        self.maps = f"/proc/{pid}/maps"
        self._f = open(self.mem, "rb", 0)   # keep handle open (fast, like scoreboard)

    def base(self):
        with open(self.maps) as f:
            for line in f:
                if MODULE_NAME.lower() in line.lower():
                    return int(line.split("-")[0], 16)
        return None

    def read(self, addr, size):
        try:
            self._f.seek(addr)
            d = self._f.read(size)
            return d if len(d) == size else None
        except OSError:
            return None


def find_pid_linux():
    best = None
    for e in os.listdir("/proc"):
        if not e.isdigit():
            continue
        try:
            with open(f"/proc/{e}/cmdline", "rb") as f:
                cmd = f.read().decode(errors="replace")
        except OSError:
            continue
        if PROC_MATCH.lower() not in cmd.lower():
            continue
        rss = 0
        try:
            with open(f"/proc/{e}/statm") as f:
                rss = int(f.read().split()[1])
        except OSError:
            pass
        if best is None or rss > best[1]:
            best = (int(e), rss)
    return best[0] if best else None


# ---------- Windows backend ----------
class WindowsMem:
    def __init__(self, pid):
        import ctypes
        from ctypes import wintypes
        self.ct = ctypes
        self.k32 = ctypes.WinDLL("kernel32", use_last_error=True)
        self.ps = ctypes.WinDLL("psapi", use_last_error=True)
        self.k32.OpenProcess.restype = wintypes.HANDLE
        self.k32.OpenProcess.argtypes = [wintypes.DWORD, wintypes.BOOL, wintypes.DWORD]
        self.k32.ReadProcessMemory.restype = wintypes.BOOL
        self.k32.ReadProcessMemory.argtypes = [wintypes.HANDLE, wintypes.LPCVOID,
                                               wintypes.LPVOID, ctypes.c_size_t,
                                               ctypes.POINTER(ctypes.c_size_t)]
        self.ps.EnumProcessModulesEx.restype = wintypes.BOOL
        self.ps.EnumProcessModulesEx.argtypes = [wintypes.HANDLE,
            ctypes.POINTER(wintypes.HMODULE), wintypes.DWORD,
            ctypes.POINTER(wintypes.DWORD), wintypes.DWORD]
        self.ps.GetModuleBaseNameW.restype = wintypes.DWORD
        self.ps.GetModuleBaseNameW.argtypes = [wintypes.HANDLE, wintypes.HMODULE,
                                               wintypes.LPWSTR, wintypes.DWORD]
        self.h = self.k32.OpenProcess(0x0410, False, pid)   # VM_READ|QUERY_INFO
        if not self.h:
            raise OSError("OpenProcess failed (need 32-bit Python + Administrator)")

    def base(self):
        ct = self.ct
        from ctypes import wintypes
        arr = (wintypes.HMODULE * 1024)()
        need = wintypes.DWORD()
        if not self.ps.EnumProcessModulesEx(self.h, arr, ct.sizeof(arr),
                                            ct.byref(need), 0x03):
            return None
        buf = ct.create_unicode_buffer(260)
        for i in range(int(need.value / ct.sizeof(wintypes.HMODULE))):
            self.ps.GetModuleBaseNameW(self.h, arr[i], buf, 260)
            if MODULE_NAME.lower() in buf.value.lower():
                return int(arr[i]) & 0xFFFFFFFF
        return None

    def read(self, addr, size):
        ct = self.ct
        buf = (ct.c_char * size)()
        got = ct.c_size_t(0)
        ok = self.k32.ReadProcessMemory(self.h, ct.c_void_p(addr & 0xFFFFFFFF),
                                        buf, size, ct.byref(got))
        return bytes(buf) if ok and got.value == size else None

    def list_modules(self):
        ct = self.ct
        from ctypes import wintypes
        arr = (wintypes.HMODULE * 2048)()
        need = wintypes.DWORD()
        if not self.ps.EnumProcessModulesEx(self.h, arr, ct.sizeof(arr),
                                            ct.byref(need), 0x03):
            err = ct.get_last_error()
            print(f"[debug] EnumProcessModulesEx failed, err={err}")
            return []
        names = []
        buf = ct.create_unicode_buffer(260)
        for i in range(int(need.value / ct.sizeof(wintypes.HMODULE))):
            self.ps.GetModuleBaseNameW(self.h, arr[i], buf, 260)
            names.append(buf.value)
        return names


def find_pid_windows():
    import ctypes
    from ctypes import wintypes
    ps = ctypes.WinDLL("psapi", use_last_error=True)
    k32 = ctypes.WinDLL("kernel32", use_last_error=True)
    k32.OpenProcess.restype = wintypes.HANDLE
    k32.OpenProcess.argtypes = [wintypes.DWORD, wintypes.BOOL, wintypes.DWORD]
    k32.CloseHandle.argtypes = [wintypes.HANDLE]
    ps.EnumProcesses.restype = wintypes.BOOL
    ps.EnumProcesses.argtypes = [ctypes.POINTER(wintypes.DWORD), wintypes.DWORD,
                                 ctypes.POINTER(wintypes.DWORD)]
    ps.EnumProcessModules.restype = wintypes.BOOL
    ps.EnumProcessModules.argtypes = [wintypes.HANDLE, ctypes.POINTER(wintypes.HMODULE),
                                      wintypes.DWORD, ctypes.POINTER(wintypes.DWORD)]
    ps.GetModuleBaseNameW.restype = wintypes.DWORD
    ps.GetModuleBaseNameW.argtypes = [wintypes.HANDLE, wintypes.HMODULE,
                                      wintypes.LPWSTR, wintypes.DWORD]
    arr = (wintypes.DWORD * 4096)()
    need = wintypes.DWORD()
    if not ps.EnumProcesses(arr, ctypes.sizeof(arr), ctypes.byref(need)):
        return None
    debug = os.environ.get("VX_DEBUG")
    seen = []
    for i in range(int(need.value / ctypes.sizeof(wintypes.DWORD))):
        pid = arr[i]
        if not pid:
            continue
        h = k32.OpenProcess(0x0410, False, pid)
        if not h:
            continue
        try:
            buf = ctypes.create_unicode_buffer(260)
            mod = (wintypes.HMODULE * 1)()
            got = wintypes.DWORD()
            if ps.EnumProcessModules(h, mod, ctypes.sizeof(mod), ctypes.byref(got)):
                ps.GetModuleBaseNameW(h, mod[0], buf, 260)
                nm = buf.value
                low = nm.lower()
                if "coj" in low:
                    seen.append((pid, nm))
                    # match any CoJ dedicated-server-ish exe
                    if "coj" in low and ("_ds" in low or "game" in low or "coj2" in low):
                        print(f"[extractor] matched process pid={pid} exe={nm}")
                        return pid
        finally:
            k32.CloseHandle(h)
    # Nothing matched -> show all CoJ-ish processes we found.
    if seen:
        print("[extractor] no exact match; CoJ-like processes seen:")
        for pid, nm in seen:
            print(f"    pid={pid} exe={nm}")
    else:
        print("[extractor] no CoJ process found. Is the DS running? "
              "(and are you the same/elevated user that owns it?)")
    return None


# ---------- helpers ----------
def u32(mem, addr):
    r = mem.read(addr, 4)
    return struct.unpack("<I", r)[0] if r else None

def i32(mem, addr):
    r = mem.read(addr, 4)
    return struct.unpack("<i", r)[0] if r else None

def read_name(mem, ptr, max_chars=24):
    if not ptr:
        return None
    raw = mem.read(ptr, max_chars * 2)
    if not raw:
        return None
    out = bytearray()
    for k in range(0, len(raw) - 1, 2):
        if raw[k] == 0 and raw[k+1] == 0:
            break
        out += raw[k:k+2]
    try:
        s = out.decode("utf-16-le").strip()
    except Exception:
        return None
    return s or None


_dbg_done = False
def read_players(mem):
    global _dbg_done
    base = mem.base()
    if not base:
        if not _dbg_done and hasattr(mem, "list_modules"):
            _dbg_done = True
            mods = mem.list_modules()
            print(f"[debug] {len(mods)} modules in target process:")
            for nm in mods:
                if "coj" in nm.lower() or "_ds" in nm.lower() or ".dll" in nm.lower() or ".exe" in nm.lower():
                    print("   ", nm)
        return []
    container = u32(mem, base + CONTAINER_OFF)
    if not container:
        return []
    players = []
    for slot in range(MAX_SLOTS):
        obj = u32(mem, container + slot * STRIDE)
        if not obj:
            continue
        name = read_name(mem, u32(mem, obj + OFF_NAME))
        if not name:
            continue
        team = i32(mem, obj + OFF_TEAM)
        if team is None:
            continue
        players.append({"name": name, "team": team})
    return players


def query_mode():
    try:
        s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        s.settimeout(1.0)
        s.sendto(QUERY_PACKET, (QUERY_IP, QUERY_PORT))
        data, _ = s.recvfrom(2048)
        s.close()
        return data[MODE_OFF] if len(data) > MODE_OFF else None
    except Exception:
        return None


def attach():
    if platform.system() == "Windows":
        pid = find_pid_windows()
        return WindowsMem(pid) if pid else None
    pid = find_pid_linux()
    return LinuxMem(pid) if pid else None


def main():
    global QUERY_IP, QUERY_PORT
    if len(sys.argv) >= 3:
        QUERY_IP, QUERY_PORT = sys.argv[1], int(sys.argv[2])

    print(f"[extractor] {platform.system()} out={OUT_PATH} query={QUERY_IP}:{QUERY_PORT}")
    mem = None
    last_mode = -1

    while True:
        try:
            if mem is None:
                mem = attach()
                if mem is None:
                    time.sleep(2.0)
                    continue
                print("[extractor] attached to DS")

            m = query_mode()
            if m is not None:
                last_mode = m

            players = read_players(mem)

            doc = {"ok": True, "mode_id": last_mode,
                   "timestamp": time.time(), "players": players}
            tmp = OUT_PATH + ".tmp"
            with open(tmp, "w") as f:
                json.dump(doc, f)
            os.replace(tmp, OUT_PATH)
        except Exception as e:
            print(f"[extractor] error: {e}")
            mem = None
        time.sleep(INTERVAL)


if __name__ == "__main__":
    main()