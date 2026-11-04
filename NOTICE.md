# NOTICE — provenance, licence and what is deliberately absent

## The DLL

`patch/xlive.dll` is a build of **XLiveLessNess** by GlitchyScripts,
<https://gitlab.com/GlitchyScripts/xlivelessness>, licensed **LGPL-2.1**. The licence text is in
[LICENSE.md](LICENSE.md).

> Earlier internal build notes of ours said "GPL-2.0". That was wrong: the upstream
> `LICENSE.md` is the GNU **Lesser** GPL v2.1. This repository uses LGPL-2.1.

Shipped binary:

| | |
|---|---|
| file | `patch/xlive.dll` |
| size | 1,688,576 bytes |
| MD5 | `5470398b9d74a9842e6b7143caeb92ca` |
| SHA-256 | computed and printed by `tools/collect-crash-run.ps1`, and by `Get-FileHash` |
| version | XLLN 1.6.2.1 source line, upstream master plus the local fixes below |
| architecture | 32-bit (`Win32`) |
| debug logging | enabled (`XLLN_DEBUG`), log in `%LOCALAPPDATA%\XLiveLessNess\logs` |

## Corresponding source

`source/` contains the complete source tree the DLL was built from, as required by LGPL-2.1 §6 —
not a patch against an unstated version.

- Base: `https://gitlab.com/GlitchyScripts/xlivelessness.git`
- Commit the tree was taken from: recorded in `source/PROVENANCE.txt` (HEAD at build time)
- Changes on top of upstream: `source/LOCAL-CHANGES.patch` (uncommitted working-tree changes at
  build time) plus the local commits listed in `source/PROVENANCE.txt`

Local changes that matter for this title (all present in `source/`):

1. `XOnlineStartup` double-call guard — the game calls it on every menu change, and each call used
   to restart the LiveOverLAN thread, wiping advertised sessions (measured: 448 wipes in 24 s,
   which is why the LIVE lobby list was always empty).
2. Net-entity address cache guard — unusable peer addresses (0.0.0.0, 127/8, broadcast,
   multicast) are never cached and never displace a usable address. Previously a loopback
   broadcast could make a peer's address `127.0.0.1`, so the client answered itself.
3. Title UDP port tracking — the port the title actually bound is published in the XNADDR
   (`wPortOnline`) instead of 0 or the boot-time reservation, at every publish site.
   Without it, peers dial a port nothing listens on and the join is torn down.
4. Instance-id seeding — mixes process id, local instance index and tick count into the seed.
   Two instances started in the same second used to generate identical instance ids.
5. Session XNKID derivation — per-session ids instead of the constant `0x8b8b8b8b8b8b8b4b`,
   which made `XNetRegisterKey` reject every second key as already registered.
6. Search verdict log format — `%016I64x` for the 64-bit map key (the old `%08x` shifted every
   following vararg on Win32 and crashed the game).
7. Opt-in `xlln_title_bind_fallback` (default `0`) — only for running two instances on one PC
   during testing. Players do not need it and it changes nothing for them.

## Rebuilding

Upstream master needs two CMake edits on a machine without the DirectX SDK, and `XLLN_DEBUG`
must be defined or Release builds log nothing:

```powershell
git clone https://gitlab.com/GlitchyScripts/xlivelessness.git
# apply source/LOCAL-CHANGES.patch and the local commits
cmake -B build -G "Visual Studio 17 2022" -A Win32
cmake --build build --config Release --target XLiveLessNess
# result: bin/xlive.dll
```

- link `d3d9`, `d3d10`, `d3d11`, `dxgi`, `dxguid` instead of `DXSDK::DX9`;
- drop the `FindDirectXSDK` include;
- add `XLLN_DEBUG` to `target_compile_definitions`.

## Deliberately not in this repository

| Item | Why |
|---|---|
| `game.pak.zip` (3.28 GB), `Vancouver2010-Setup.exe` (3.28 GB) | game data — redistribution, not ours to publish |
| `d3dx9_38.dll`, `xinput1_3.dll` | Microsoft redistributables; install the DirectX June 2010 runtime instead |
| Windows Error Reporting dumps, XLLN logs, screenshots | contain the reporting players' machine details |
| Radmin IP addresses, local user paths, profile names | personal data; docs use `26.x.x.x` placeholders |

The repository is a compatibility patch. It contains no means of obtaining the game and does not
bypass a purchase: the no-DVD patch rewrites a disc-**presence** check in `GameUtil.dll` on an
installation you already own, nothing else.

## No warranty

Provided as is, without warranty. Not affiliated with EA, SEGA, Eurocom, Microsoft, GlitchyScripts
or the XLLN contributors. Vancouver 2010 and Games for Windows - LIVE remain their owners'
property.
