# Vancouver 2010 (PC) — online play without Games for Windows LIVE

Vancouver 2010's multiplayer ran on Games for Windows - LIVE, which Microsoft shut down. This
repository replaces GFWL with [XLiveLessNess](https://gitlab.com/GlitchyScripts/xlivelessness)
(XLLN), an open-source LGPL reimplementation: local profiles, sign-in, achievements,
LAN/LIVE lobby discovery, session joining and invites.

**This repository contains no game files.** You need your own retail copy of Vancouver 2010
(PC, 2009, version 1.0.0.1). Nothing here helps you obtain one, and nothing here is needed if
you only ever play single player.

---

## Status — read this before you install

Tested on Windows 10/11 x64 with two machines over Radmin VPN, Game version 1.0.0.1.

| Feature | State |
|---|---|
| Single player, local 4-player hotseat | works |
| Starting without a disc / ISO mounted | works (no-DVD patch) |
| Online profile, achievements, user cards | works |
| LAN/LIVE lobby list over Radmin VPN / ZeroTier / Tailscale | works |
| Joining a lobby from the list | works |
| Sending and accepting invites | works |
| **Accepting an invite on one tested PC** | **crash — see [KNOWN-ISSUES #1](KNOWN-ISSUES.md)** |
| **Starting an event after joining** | **fails — see [KNOWN-ISSUES #2](KNOWN-ISSUES.md)** |
| LIVE friends / invite list without an invite | not implemented by XLLN upstream |

The two open issues are honest limits of the current build, not installation mistakes. If you
hit them, [collect evidence](#if-it-breaks-collect-evidence-first) instead of reinstalling.

---

## What you need

- **Windows 10 or 11, 64-bit**, and a 32-bit game — Windows runs it from `SysWOW64`, so the
  32-bit runtime matters (see below).
- **Vancouver 2010 PC**, retail install, version 1.0.0.1. The reference `Vancouver.exe` is
  8,320,328 bytes. If yours differs, this patch is not tested against it.
- **Visual C++ 2015-2022 Redistributable (x86)** — provides `msvcp140.dll`, `vcruntime140.dll`.
  A fresh Windows without it fails at launch with a missing-DLL dialog. It must be **14.40 or
  newer**: a leftover VS2017-era runtime (`14.11`, which a retail Vancouver 2010 install can bring
  along) crashes this patch inside `msvcp140.dll` the first time a mutex is locked. Check with
  `(Get-Item "$env:SystemRoot\SysWOW64\msvcp140.dll").VersionInfo.FileVersion`.
- **DirectX End-User Runtime (June 2010)** — provides `d3dx9_38.dll`, needed by this 2009 title.
- **Radmin VPN** (free, <https://www.radmin-vpn.com/>) for play over the internet. ZeroTier or
  Tailscale work too; XLLN broadcasts on every eligible adapter.
- No admin rights are needed to *play*; the installer needs elevation once.

---

## Install (5 minutes)

1. Install Vancouver 2010 normally from your disc or a mounted image you own.
2. Download this repository: **Code → Download ZIP**, then unpack it somewhere permanent.
3. Right-click **`patch/install.cmd` → Run as administrator**. It will:
   - find the game via the SEGA registry key or the default install path,
   - back up any existing `xlive.dll` to `xlive.dll.bak`, then install this build,
   - create `%LOCALAPPDATA%\XLiveLessNess\xlln-config-1.ini` with an online auto-login profile,
   - ask for a profile name (max 15 characters) and an optional host `IP:port`,
   - ask whether to remove the disc check (answer **J**),
   - offer to uninstall a leftover GFWL client (answering N is fine),
   - add inbound firewall rules for `Vancouver.exe` on UDP/TCP 39000-39001.
4. Start the game from its normal shortcut. Press **START**, then **Play On Line**.

Nothing else is needed. If you prefer a terminal:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File patch/install.ps1 -Name "YourName" -NoDvd
```

---

## Play with a friend over the internet

### One-time, on both PCs

1. Install Radmin VPN and create/join one shared network (the free tier is enough).
2. **Reboot both PCs after installing Radmin.** This is not superstition: XLLN builds its
   broadcast list at startup, and without the reboot the lobby list stays empty over the Radmin
   adapter. It is the single most common "it does not work" cause. See
   [TROUBLESHOOTING #2](TROUBLESHOOTING.md#2-lobby-list-is-empty-nobody-sees-the-host).
3. Both players must run **exactly this `xlive.dll`** — 1,688,576 bytes, MD5
   `5470398b9d74a9842e6b7143caeb92ca`. Mixed builds corrupt memory when sessions are serialised,
   and the crash looks like a random access violation. Check with:

   ```powershell
   Get-Item "<game folder>\xlive.dll" | Select-Object Length
   (Get-FileHash "<game folder>\xlive.dll" -Algorithm MD5).Hash
   ```

### Host

1. Start the game → `PRESS START` → **Play On Line** → **LIVE Lobby**.
2. Host a Player Match and leave the lobby open.
3. Tell your friend your Radmin IP (shown in Radmin as a `26.x.x.x` address).

### Friend

1. Start the game → `PRESS START` → **Play On Line** → **LIVE Lobby**.
2. The host's lobby appears in the list. Join it.

### If the lobby list stays empty

Set the direct address instead (game closed while editing):

```ini
; %LOCALAPPDATA%\XLiveLessNess\xlln-config-1.ini
xlln_direct_ip_connect_ip_port = 26.x.x.x:39001
```

Both sides need the same patch; only the client needs the direct address.

### Ports, if you would rather not use a VPN

Host forwards **UDP and TCP 39000-39001** to its PC. The game itself talks on UDP 12756
(system-link) and the VDP ports 5000/45039; those stay inside the XLLN tunnel and are not
forwarded.

---

## If it breaks, collect evidence first

Do not reinstall, do not swap DLLs, do not "align" builds — that destroys the evidence and is
usually the wrong fix. Collect first:

1. Double-click **`tools/collect-crash-run.cmd`**. It is read-only toward the game folder, needs
   no admin, and writes `Desktop\vancouver-crash-<timestamp>.zip` with:
   - every XLLN log (plus a UTF-8 copy of each — the originals are UTF-16LE with no BOM),
   - any Windows Error Reporting dump for `Vancouver.exe`,
   - Application event log entries 1000/1001,
   - versions and MD5/SHA-256 of the game, `xlive.dll` and the VC++ runtime,
   - the WER `LocalDumps` configuration and the local IPv4 addresses.
2. **Run it before starting the game again.** XLLN rewrites its log on every launch, so the
   crashed run's log only exists until the next start.
3. Then work through [TROUBLESHOOTING.md](TROUBLESHOOTING.md).

Log location: `%LOCALAPPDATA%\XLiveLessNess\logs\xlln-debug-1.log` (UTF-16LE, no BOM,
rewritten every launch).

---

## Using a coding agent

[AGENT-PROMPT.md](AGENT-PROMPT.md) is a ready-to-paste prompt for an agent (Claude Code,
opencode, Cursor, Copilot Workspace, ...) running on the player's PC. It walks the agent through
prerequisites, installation, verification, multiplayer setup, the troubleshooting decision
table, evidence collection and a fixed reporting format. Paste the whole block; the agent does
the rest.

---

## Repository layout

```
patch/
  install.cmd               double-click installer (elevates itself)
  install.ps1               the installer: DLL, profile, disc check, firewall, GFWL cleanup
  patch-nodvd.ps1           disc-check removal, reads the PE export table, -Restore undoes it
  xlln-config-template.ini  default XLLN configuration
  xlive.dll                 XLiveLessNess build used and tested here (LGPL-2.1)
tools/
  collect-crash-run.ps1     one-click evidence collector
  collect-crash-run.cmd     double-click launcher for it
source/                     complete source of the shipped xlive.dll (LGPL compliance)
TROUBLESHOOTING.md          symptom -> cause -> fix, with the evidence behind each entry
KNOWN-ISSUES.md             the two problems this build does not solve yet
AGENT-PROMPT.md             prompt for an agent on the player's PC
LICENSE.md                  LGPL-2.1, the licence of XLiveLessNess
NOTICE.md                   provenance of the build, what is deliberately absent
```

---

## Legal and provenance

- `xlive.dll` is derived from [XLiveLessNess](https://gitlab.com/GlitchyScripts/xlivelessness)
  by GlitchyScripts, licensed **LGPL-2.1**. The complete corresponding source of this build is
  in `source/`, including our local modifications. See [NOTICE.md](NOTICE.md).
- No game data, no installers, no Microsoft redistributables are included or linked here.
- Not affiliated with EA, SEGA, Eurocom, Microsoft or the XLLN authors. Vancouver 2010 and
  Games for Windows - LIVE are their owners' trademarks. This is a compatibility patch for a
  game you already own.
- Nothing here bypasses a purchase. The no-DVD patch removes a **disc-presence check** from
  `GameUtil.dll` on your own installation so the game can start without the disc in the drive;
  it does not decrypt or download anything.
