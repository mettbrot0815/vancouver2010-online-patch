# Vancouver 2010 (PC) — TROUBLESHOOTING

Every entry is a real failure observed while getting this title online, with the cause we
actually found in the code or the log — not a guess. Order is roughly "most likely first".

Conventions: `<game>` is the install folder (usually `C:\Program Files (x86)\SEGA\Vancouver 2010`
or wherever you installed it). `<log>` is
`%LOCALAPPDATA%\XLiveLessNess\logs\xlln-debug-1.log`.

**Before anything else:** run `tools\collect-crash-run.cmd` **before** starting the game again.
XLLN rewrites its log on every launch, so the crashed run's log is gone after the next start.

---

## 1. "Could not find the Vancouver 2010 disc" (or a disc dialog at startup)

**Why.** `Vancouver.exe` does not link `GameUtil.dll`; it resolves `IsDiskInTray` at runtime
with `GetProcAddress` and aborts the start when it returns 0. One call site, one branch.

**Fix.** Apply the no-DVD patch (the installer asks "Disc-Check entfernen?" — answer **J**):

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File patch\patch-nodvd.ps1 -GameDir "<game>"
```

The patcher reads the export RVA from `GameUtil.dll`'s own PE export table, so it does not
depend on a hardcoded offset. It writes the export's first bytes to `B0 01 C3`
(`mov al,1 ; ret`) and keeps a backup as `GameUtil.dll.orig`.

**Check.** Re-running the patcher is safe and reports whether the export is already patched.
Undo with `-Restore`.

**Note.** A **"Could not start Vancouver 2010 as another instance is already running."** dialog
is a different message with the same window class. It is not a disc failure — close the other
game window first.

---

## 2. Lobby list is empty (nobody sees the host)

**Why, in order of likelihood.**

1. **No reboot after installing Radmin VPN.** XLLN enumerates the eligible adapters when the
   game starts; an adapter that came up after that is missing from the broadcast list. Reboot
   both PCs after the Radmin install, not just the game.
2. **Mixed `xlive.dll` builds.** An older build publishes a session port that no socket is
   listening on, so the join is refused and the lobby looks dead. Compare MD5 with
   `5470398b9d74a9842e6b7143caeb92ca`, size 1,688,576 bytes, on both PCs.
3. **Different networks.** Radmin must show a `26.x.x.x` address for both players and both must
   be in the same Radmin network.
4. **Loopback/other-adapter address poisoning (fixed in this build).** The game broadcasts title
   packets on *every* eligible adapter, including the loopback interface. An older XLLN cached
   the source of each copy unconditionally, so the peer's address could become `127.0.0.1` and
   the client answered itself. The log shows the tell:

   ```
   XLIVE WARN > ParseNetworkData TITLE_PACKET packet instanceId remote address different to cached value. Updating cache.
   ```

   This build refuses to cache unusable addresses (0.0.0.0, 127/8, broadcast, multicast) and
   never lets one displace a usable address. If you see that warning in a loop, you are running
   an older DLL.

**Fix / check.** Reboot both PCs, then verify the DLL hash, then try the direct address
(`xlln_direct_ip_connect_ip_port = <host>:39001` in the ini, game closed).

---

## 3. Join is refused, or "This game session is no longer available" right after joining

**Why.** The game builds its peer dial from the XNADDR's `wPortOnline`. A build that returns 0
there — or returns the boot-time port reservation instead of the port the title actually bound —
tells the joiner to dial a port nothing listens on. The join then resolves an unreachable
address and tears down after its probe.

**Fix.** Use the `xlive.dll` from this repository on **both** PCs. This build publishes the port
the title really bound (`XllnGetTitleDialPort_`: TCP listener, else the system-link socket, else
the newest UDP title socket, else the reservation) at every publish site: `XNetGetTitleXnAddr`,
the LIVE advertise, QOS replies and the local-instance branch of
`XllnNetEntityGetXnaddrXnkidByInstanceId`.

**Check.** In `<log>`, right after the join, the host should not log
`not delivered to port (...) as no Title Socket is listening on it`.

---

## 4. Joiner is dropped ~30 seconds after joining, or when the event starts (KNOWN ISSUE)

**Why.** About 31 s after a successful join the joiner closes its own session socket and calls
`XNetUnregisterInAddr`; from then on the host drops the title packets with
`not delivered to port (...) as no Title Socket is listening on it` and the game shows its
generic "session no longer available" dialog. This is an XLLN compatibility gap for this title,
not an installation error. Details and current status in
[KNOWN-ISSUES #2](KNOWN-ISSUES.md#2-starting-an-event-drops-the-joiner-after-about-30-seconds).

**Workaround.** Local 4-player on one PC, or a remote-play tool (Parsec, Steam Remote Play) for
the social part. Lobby chat, presence and the player list still work.

**What helps us.** The log plus the event log from that run — `tools\collect-crash-run.cmd`.

---

## 5. Crash when accepting an invite (KNOWN ISSUE)

**Symptom.** About 2 s after accepting a friend's invite the game window disappears, and the
host never sees a join answer. Event log: `Vancouver.exe`, faulting module `MSVCP140.dll`,
exception `c0000005`, and the process dump shows a **READ of `0x00000000`**.

**Why.** The C++ runtime faults on data handed to it, not because of the runtime. Our symbol work
on the matching build maps the involved frames to XLLN's net-entity association path
(`XllnNetEntityBroadcastUnknownUserAsk`, `XllnNetEntityRemoveStale`) and to
`XNetDnsLookup`; the pointer in `ECX`/`EBX` at the fault resolves to a `std::mutex`
(`xlive_mutex_xnet_connect_status`). A `std::mutex::lock` that faults as a read of 0 means the
mutex's internal pointer is null — i.e. locked before construction or after destruction. That is
the current best hypothesis, not a proven fix. See
[KNOWN-ISSUES #1](KNOWN-ISSUES.md#1-accepting-an-invite-crashes-in-msvcp140).

**Workaround.** Join from the **lobby list** instead of accepting an invite. Both players must be
on the same network and the lobby must be freshly created (a stale invite cannot be accepted).

**What helps us.** The dump, the log and the event record: `tools\collect-crash-run.cmd`, run
**before** relaunching.

---

## 6. "msvcp140.dll is missing", "vcruntime140.dll is missing", "d3dx9_38.dll is missing"

**Why.** Vancouver 2010 is a 32-bit game, so Windows loads the **32-bit (SysWOW64)** runtime even
on 64-bit Windows. Installing only the x64 redistributable does nothing.

**Fix.** Install, in this order:

1. [Visual C++ 2015-2022 Redistributable (x86)](https://aka.ms/vs/17/release/vc_redist.x86.exe)
2. [DirectX End-User Runtime (June 2010)](https://www.microsoft.com/download/details.aspx?id=8109)
   — the web installer is enough; it unpacks and installs `d3dx9_38.dll`.

**Check.**

```powershell
Get-Item "$env:SystemRoot\SysWOW64\msvcp140.dll","$env:SystemRoot\SysWOW64\vcruntime140.dll","$env:SystemRoot\SysWOW64\d3dx9_38.dll"
```

Three files must exist. We do not ship these DLLs; they are Microsoft redistributables and must
come from Microsoft.

---

## 7. Game will not start at all, or starts and closes immediately

Work down this list, in this order:

1. **Antivirus / Defender quarantine.** A replaced `xlive.dll` in a game folder is a common
   false positive. Check the protection history; restore and exclude `<game>` if necessary.
   Then re-verify the MD5 — a quarantined then restored file is sometimes truncated.
2. **Not running the correct `xlive.dll`.** Size and MD5 as in §2. A 64-bit or wrong-version DLL
   makes the game fail to start with no message.
3. **The game needs its own runtime files.** Some repacks omit `d3dx9_38.dll` next to the exe from
   the original install. Without the DirectX June 2010 runtime installed system-wide, the game
   exits silently.
4. **Read the log.** If `<log>` exists and ends mid-line at startup, the failure is inside XLLN —
   send the last 80 lines.

---

## 8. "Could not start Vancouver 2010 as another instance is already running."

**Not an error.** The game is single-instance (mutex `JVancouver 2010`). Close the running
window. This dialog is also a frequent false positive in automated checks, because it uses the
same `#32770` window class as the disc error.

---

## 9. Config edits are lost / the profile name reverts

**Why.** XLLN rewrites `xlln-config-1.ini` when the game exits, from in-memory state.

**Fix.** Edit `%LOCALAPPDATA%\XLiveLessNess\xlln-config-1.ini` only while the game is **closed**.
Relevant keys:

| Key | Meaning |
|---|---|
| `xlive_username_p1` | profile name, max 15 characters |
| `xlive_user_live_enabled_p1`, `xlive_user_online_enabled_p1`, `xlive_user_auto_login_p1` | set all to `1` for a local online profile with auto-login |
| `xlln_direct_ip_connect_ip_port` | `<host-ip>:39001` for direct connect, empty for LAN/VPN discovery |
| `xlive_specific_network_adapter` | adapter GUID to force one adapter (e.g. Radmin only) |
| `xlive_broadcast_address` | leave empty to broadcast on every eligible adapter |

---

## 10. Two players on one PC get the same identity, or a friend sees their own lobby

**Why (fixed in this build).** XLLN seeded its instance id from `srand(time(NULL))` — seconds
only — so two instances started in the same second generated the same instance id and each
treated the other's traffic as its own loopback. It also produced session key ids that were
constant (`0x8b8b...`), so `XNetRegisterKey` rejected the second key as already registered.

**Fix.** This build seeds from
`time(NULL) ^ (GetCurrentProcessId() << 8) ^ (xlln_local_instance_id << 24) ^ GetTickCount()`.
Nothing to configure; just do not mix this build with an older one.

---

## 11. Mixed builds between two PCs (heap corruption, random crashes)

**Symptom.** Random access violations in `msvcp140.dll`/`ucrtbase.dll`, sometimes only on one
side, and a join that half-works.

**Why.** Two different `xlive.dll` builds disagree about the layout of the structures they
serialise into session packets. XLLN writes those structures directly into network buffers; a
layout mismatch corrupts memory on the receiving side.

**Fix.** One build for everyone. Distribute this repository's `patch/xlive.dll`
(1,688,576 bytes, MD5 `5470398b9d74a9842e6b7143caeb92ca`) and verify it on each PC. During our
own testing a stale copy was still loaded while a new one was "deployed", because the game had
the file open — the deployment failed silently and both instances ran different builds.

---

## 12. The game asks to sign in to GFWL, or GFWL errors appear

**Why.** A leftover Games for Windows - LIVE client, or a shim still loading the old DLL.

**Fix.** XLLN replaces GFWL completely; the client is not needed. The installer offers to
uninstall it (answer N to keep it — it does not interfere). What matters is that
`<game>\xlive.dll` is the patched build: Windows loads the game-local DLL before the system one.

---

## 13. Firewall rules, and why the join works one way only

**Why.** Discovery is broadcast (UDP); the session itself is unicast. A blocked inbound rule
breaks one direction only, which looks like "he sees my lobby but I never see his".

**Check.**

```powershell
Get-NetFirewallRule -DisplayName 'Vancouver 2010 (XLLN) *' | Select-Object DisplayName,Enabled,Direction,Action
```

Both a UDP and a TCP rule should exist, inbound, allow, for `Vancouver.exe`.

---

## 14. Ports reference

| Port | Protocol | Who uses it |
|---|---|---|
| 39000 | UDP | XLLN discovery / broadcast (base port) |
| 39001 | UDP/TCP | XLLN session traffic, direct-IP connect |
| 12756 | UDP | title system-link traffic (inside the XLLN tunnel) |
| 5000, 45039 | UDP | title VDP sockets (inside the XLLN tunnel) |

Only 39000-39001 ever needs forwarding, and only if you play without a VPN.

---

## 15. Nothing above matches

Collect and send `tools\collect-crash-run.cmd`'s zip. It answers most of these questions by
itself: which DLL you run, which runtimes exist, which adapters and addresses are present, what
the event log says, and what the log's last lines were. Without it, every diagnosis starts with
guessing.
