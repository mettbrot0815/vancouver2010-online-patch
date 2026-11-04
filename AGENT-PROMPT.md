# Agent prompt — install and repair Vancouver 2010 online play

Copy everything inside the block below into a coding agent running on the player's PC
(Claude Code, opencode, Cursor, ...). It is written to be followed literally: it checks the
machine before changing anything, verifies every step, and ends with a fixed report format so a
human can act on the result without asking follow-up questions.

The agent needs shell access (PowerShell) and permission to run an elevated installer once. It
must not need anything else.

```text
You are helping a non-expert player get Vancouver 2010 (PC) online again. Games for Windows -
LIVE is dead; this machine should use XLiveLessNess (XLLN), the LGPL replacement, from the
repository the user downloaded. You have PowerShell on Windows 10/11.

HARD RULES — do not break these, even if a step seems to fail:
1. The player owns the game. Never download, search for, or suggest obtaining game files, ISOs,
   cracks, or "pre-patched" copies. If the game is not installed, stop and say so.
2. Do not reinstall the game, do not uninstall other software, do not change Windows settings
   outside the steps below. No registry edits except what the installer itself does.
3. Do not swap xlive.dll versions between PCs and do not "align" builds by copying a DLL of a
   different size or hash. A hash mismatch is a finding to report, not something to fix blindly.
4. Before starting the game for any test, run tools\collect-crash-run.cmd if a crash is being
   investigated: XLLN rewrites its log on every launch, so the crashed run's log is destroyed by
   the next start. This ordering matters more than any other step in this document.
5. Never paraphrase a number a command produced. Copy it. If a value is unknown, write unknown.
6. If a step fails: stop, report the exact command and its output, then use only the fallback
   that step names. Do not invent a new approach.

STEP 0 — establish facts. Run and keep the output:
  ver
  whoami
  [Environment]::OSVersion.VersionString
  Get-ItemProperty 'HKLM:\SOFTWARE\WOW6432Node\SEGA\Vancouver 2010' -ErrorAction SilentlyContinue | Select-Object InstallLocation
  Get-ChildItem 'HKLM:\SOFTWARE\WOW6432Node\Microsoft\Windows\CurrentVersion\Uninstall' |
    ForEach-Object { (Get-ItemProperty $_.PSPath).DisplayName } | Where-Object { $_ -match 'Vancouver|Games for Windows' }

Find the game folder from InstallLocation, else from
  "$env:ProgramFiles(x86)\SEGA\Vancouver 2010", else "C:\Games\Vancouver 2010".
Confirm "<game>\Vancouver.exe" exists. Record its length: a tested copy is 8,320,328 bytes.
If the game is absent, stop and report that the player must install their own copy first.

STEP 1 — prerequisites. Report each, do not install anything without telling the user first:
  - 32-bit VC++ runtime:  Test-Path "$env:SystemRoot\SysWOW64\msvcp140.dll"
  - 32-bit vcruntime:      Test-Path "$env:SystemRoot\SysWOW64\vcruntime140.dll"
  - DirectX 9 runtime:     Test-Path "$env:SystemRoot\SysWOW64\d3dx9_38.dll"
  - free space on the game drive: (Get-PSDrive C).Free
Missing runtime files are the most common "game will not start" cause on a fresh Windows. The
fixes are the Microsoft Visual C++ 2015-2022 Redistributable (x86) and the DirectX End-User
Runtime (June 2010), both installed by the user. Do not download them yourself; tell the user
which two to install and wait.

STEP 2 — install the patch. From the repository folder:
  powershell -NoProfile -ExecutionPolicy Bypass -File patch\install.ps1 -Name "<profile>" -NoDvd
The installer elevates itself, finds the game, backs up an existing xlive.dll to
xlive.dll.bak, installs this build, writes the XLLN profile config, removes the disc check,
optionally offers to uninstall GFWL, and adds firewall rules.
It speaks German and asks up to three questions:
  - "Profilname fuer das Spiel"   -> only if -Name was not passed; you always pass it
  - "Direktverbindung zu einem Host?" -> press Enter (empty = LAN/VPN discovery)
  - "GFWL jetzt deinstallieren?"  -> answer with Enter (keep it; it does not interfere)
When run non-interactively the Direct-IP and GFWL prompts can be fed with empty lines:
  printf '\n\n' | powershell -NoProfile -ExecutionPolicy Bypass -File patch\install.ps1 -Name "<profile>" -NoDvd
If the process hangs on a prompt, kill it, report which prompt, and ask the user to answer it.
Profile name: max 15 characters, no spaces needed. Ask the user for it if you do not have it.

STEP 3 — verify the install, all four checks, and copy the actual values into the report:
  1. Length of "<game>\xlive.dll" must be 1688576.
  2. MD5 of "<game>\xlive.dll" must be 5470398b9d74a9842e6b7143caeb92ca:
       (Get-FileHash "<game>\xlive.dll" -Algorithm MD5).Hash
  3. Disc check removed: re-run patch\patch-nodvd.ps1 -GameDir "<game>" and confirm it reports
     the export already patched (it is idempotent, and it keeps GameUtil.dll.orig as backup).
  4. Firewall rules exist, inbound allow, both protocols:
       Get-NetFirewallRule -DisplayName 'Vancouver 2010 (XLLN) *' |
         Select-Object DisplayName,Enabled,Direction,Action
If (1) or (2) differ, STOP: this is the "mixed builds" failure and it must be reported with the
actual numbers, not repaired by downloading a different DLL.

STEP 4 — multiplayer. Both players need:
  - the same xlive.dll (same length, same MD5 as in STEP 3),
  - Radmin VPN (or ZeroTier/Tailscale), both in the same network, and BOTH PCs rebooted after
    the VPN was installed. The reboot is not optional: XLLN builds its broadcast adapter list at
    startup, and without the reboot the lobby list stays empty. If the user has not rebooted,
    say so and have them reboot before testing.
  - Host: Play On Line -> LIVE Lobby -> Player Match -> Host, leave the lobby open.
  - Friend: Play On Line -> LIVE Lobby -> the host's lobby appears in the list -> join.
  - Fallback when discovery still fails, game closed on the friend's PC:
      set xlln_direct_ip_connect_port or xlln_direct_ip_connect_ip_port (whichever exists in
      %LOCALAPPDATA%\XLiveLessNess\xlln-config-1.ini) to <host-ip>:39001,
      then relaunch the game. The host's VPN address looks like 26.x.x.x.
  - Ask the user to report honestly: did the lobby appear, did the join succeed, and did an
    event start. Do not claim success you did not observe. You cannot see their game window.

STEP 5 — when something fails, follow this decision table before changing anything:
  "Could not find the Vancouver 2010 disc"            -> TROUBLESHOOTING #1 (no-DVD patch)
  "another instance is already running"               -> not an error; close the other window
  empty lobby list                                    -> TROUBLESHOOTING #2 (reboot, hash, network)
  join refused / "session no longer available" at once -> TROUBLESHOOTING #3 (DLL must match)
  joiner dropped ~30 s after join or at event start   -> KNOWN ISSUES #2, no fix; report it
  crash ~2 s after accepting an invite                -> KNOWN ISSUES #1; collect evidence first
  missing msvcp140/vcruntime140/d3dx9_38              -> TROUBLESHOOTING #6 (x86 runtimes)
  game exits silently at start                        -> TROUBLESHOOTING #7
  settings revert                                     -> TROUBLESHOOTING #9 (edit with game closed)
  play works one direction only                       -> TROUBLESHOOTING #13 (firewall, inbound)
Do not apply more than one fix at a time; verify after each, so the next report can tell which
change mattered.

STEP 6 — evidence for the two open issues (invite crash, event-start drop). Only after the user
confirms a failure:
  1. If the game is still running, ask the user to close it.
  2. Run tools\collect-crash-run.cmd (or
     powershell -NoProfile -ExecutionPolicy Bypass -File tools\collect-crash-run.ps1).
     It needs no admin, writes nothing into the game folder, and deletes nothing.
  3. It creates Desktop\vancouver-crash-<timestamp>.zip and prints the path and SHA-256. Report
     both. Do not open, edit or trim the zip; send it as produced.
  4. Read Desktop\vancouver-crash-<timestamp>\report.txt. It contains the collected log
     timestamps, the dump list, the event log entries and the file hashes.
  5. Warn the user not to start the game again until the zip has been sent.

REPORT — finish with exactly these fields, one per line, copying values verbatim:
  windows:                 <version string>
  game folder:             <path>
  Vancouver.exe:           <bytes>  md5 <hash>
  xlive.dll:               <bytes>  md5 <hash>
  build matches expected:  yes / no — expected 1688576 bytes, 5470398b9d74a9842e6b7143caeb92ca
  runtimes:                msvcp140 <yes/no>  vcruntime140 <yes/no>  d3dx9_38 <yes/no>
  disc check:              patched / original / unknown
  firewall rules:          <rule names + enabled/direction/action, or "missing">
  profile name:            <as configured in %LOCALAPPDATA%\XLiveLessNess\xlln-config-1.ini>
  VPN:                     <Radmin/ZeroTier/Tailscale/none>, both PCs rebooted yes/no/unknown
  lobby visible:           yes / no / not tested
  join succeeded:          yes / no / not tested
  event start worked:      yes / no / not tested
  failure matches:         <TROUBLESHOOTING or KNOWN-ISSUES number, or "none">
  evidence zip:            <path>  sha256 <hash>   (or "not collected")
  open questions:          1-2 sentences maximum

Never write "should work", "probably fine" or "fixed" unless a command's output proved it. If you
did not run a check, write "not tested".
```

---

## Notes for the person sending this

- Send the repository link plus this file. The agent does the rest; it only needs the user to
  answer the profile name and to reboot after installing the VPN.
- If the agent starts inventing downloads or "repairing" the DLL, stop it: the only legitimate
  DLL is the one in `patch/`, and a hash mismatch is evidence, not a task.
- The prompt deliberately ends with a fixed report. Ask for that report verbatim — it makes
  remote diagnosis possible without another round trip.
