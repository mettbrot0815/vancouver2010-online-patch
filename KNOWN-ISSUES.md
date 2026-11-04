# Known issues

Only two things are known to be broken in this build. Both are XLLN compatibility gaps for this
specific title, not installation problems, and both are independent of the no-DVD patch.

If you hit one, collect `tools\collect-crash-run.cmd` **before** starting the game again — the
log of the crashed run is rewritten on the next launch.

---

## 1. Accepting an invite crashes in `MSVCP140.dll`

**Reported by:** one PC in a two-PC test (German Windows, Windows 10/11 x64), build
1,691,648 bytes on that PC before it was aligned to this repository's build.

**Symptom.** About two seconds after clicking to accept a game invite, the game window closes.
The host never receives a join answer. Event log shows Event ID 1000:

```
Faulting application name: Vancouver.exe, version: 1.0.0.1
Faulting module name: MSVCP140.dll, version: 14.11.25325.0
Exception code: c0000005
Fault offset: 0001c1b9
```

A minidump written by Windows Error Reporting shows the access violation as a **READ of
`0x00000000`** inside `msvcp140.dll`, with `ECX` and `EBX` both holding the same address inside
`xlive.dll`'s `.data` section.

**What we know.** Working from a matching build (same `SizeOfImage`, same section layout) and its
PDB, the stack words next to the fault resolve to XLLN's net-entity association path —
`XllnNetEntityBroadcastUnknownUserAsk`, `XllnNetEntityRemoveStale` — and to `XNetDnsLookup`.
The `.data` pointer in the registers resolves to a namespace-scope `std::mutex`
(`xlive_mutex_xnet_connect_status`, guarding the connect-status map).

**Current hypothesis (not proven).** The address held in the registers is the DLL's **only**
`std::mutex` (`xlive_mutex_xnet_connect_status`); the connect-status map it guards sits `0xCE4`
bytes away, so the map is not the object being touched. `xlive.dll` imports `_Mtx_lock` and
`_Mtx_unlock` but **not** `_Mtx_init`: the mutex is constructed by inlined VS2022 header code,
while locking runs in whichever `msvcp140.dll` the PC has. The crashing PC reports
`MSVCP140.dll 14.11.25325.0` — VS2017 RTM, older than the toolset that built the DLL — so the
constructor and `_Mtx_lock` can disagree about the object's internal layout, which surfaces as a
read of address 0 on the first lock. That first lock is the connect-status poll the title does
immediately after accepting an invite, matching the ~2 s timing.

First thing to try: install the current Visual C++ 2015-2022 Redistributable (x86) and confirm
`msvcp140.dll` is `14.40` or newer. See
[TROUBLESHOOTING #6](TROUBLESHOOTING.md#6-msvcp140dll-is-missing-vcruntime140dll-is-missing-d3dx9_38dll-is-missing).

**Workaround.** Do not accept invites on the affected PC; join from the **lobby list** instead.
The lobby must be freshly created — a stale invite cannot be accepted anyway.

**What would settle it.** A dump plus log from a crash on the *current* build, so the address can
be resolved against the exact binary in `patch/`. `tools\collect-crash-run.cmd` collects both.

---

## 2. Starting an event drops the joiner after about 30 seconds

**Symptom.** The join itself succeeds: both players appear in the session, both windows list
both players, and the lobby behaves normally. As soon as an event starts, the joiner is thrown
out roughly 31 s after the join with:

> Attention! This game session is no longer available.

and the host's log starts reporting:

```
not delivered to port (...) as no Title Socket is listening on it
```

**What we know.** The joiner closes its own session socket and calls `XNetUnregisterInAddr` about
31 s after joining. After that the host can no longer deliver title packets to the joiner, and the
title raises its generic "session no longer available" dialog. The event start is what triggers
the joiner's session teardown; the join path itself is fine.

**Workaround.** Local 4-player hotseat on one PC, or Parsec / Steam Remote Play for remote play.
Presence, lobby chat, player list and achievements work across the network.

**Status.** Open. This is the reason the repository claims "joining a lobby works" but not
"playing an event works". Fixing it needs a trace of the joiner's session-teardown path, which
requires a crash-free two-PC run with logs from both sides.

---

## Not bugs

- **"Could not start Vancouver 2010 as another instance is already running."** — the game is
  single-instance. Close the other window.
- **Config changes reverting** — XLLN rewrites its ini on exit; edit it with the game closed.
- **Empty lobby list right after a Radmin install** — reboot, both PCs. See
  [TROUBLESHOOTING #2](TROUBLESHOOTING.md#2-lobby-list-is-empty-nobody-sees-the-host).
- **`XNetRegisterKey ... is already registered` in the log** — an upstream defect that older
  builds showed; it is off the join path and fixed in this build (unique session key ids).
