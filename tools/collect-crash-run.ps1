# collect-crash-run.ps1 - one-click evidence collector for the Vancouver 2010 invite crash.
#
# Why before the next launch: the XLiveLessNess debug log is truncated and rewritten every time
# the game starts, so the crashed run's log only survives until the next launch. The event log
# entries and the WER dumps survive, but this collects everything in one go anyway.
#
# Reads only. Nothing is written into the game folder, nothing is deleted, no admin required.
#
# Usage:
#   double-click collect-crash-run.cmd
#   powershell -NoProfile -ExecutionPolicy Bypass -File collect-crash-run.ps1
#
# Options:
#   -GameDir "D:\Games\Vancouver 2010"  where the game is installed
#   -OutRoot "C:\some\folder"           where the result folder is created (default: Desktop)
#   -EventDays 7                         how far back to scan the Application event log
#   -SkipDumps                           list dumps instead of copying them (slow upload)
#   -SkipZip                             keep the folder, do not create the zip
#
# Output: Desktop\vancouver-crash-<timestamp>\ plus a .zip next to it, with a printed SHA-256.

[CmdletBinding()]
param(
    [string]$GameDir = 'C:\Games\Vancouver 2010',
    [string]$OutRoot = [Environment]::GetFolderPath('Desktop'),
    [int]$EventDays = 7,
    [switch]$SkipDumps,
    [switch]$SkipZip
)

$ErrorActionPreference = 'Continue'
$ProgressPreference = 'SilentlyContinue'

if (-not $OutRoot) {
    $OutRoot = [Environment]::GetFolderPath('Desktop')
}
if (-not (Test-Path -LiteralPath $OutRoot)) {
    try {
        $null = New-Item -ItemType Directory -Force -Path $OutRoot -ErrorAction Stop
    } catch {
        Write-Host ('cannot create ' + $OutRoot + ', using the Desktop instead')
        $OutRoot = [Environment]::GetFolderPath('Desktop')
    }
}

$stamp  = Get-Date -Format 'yyyyMMdd-HHmmss'
$outDir = Join-Path $OutRoot ('vancouver-crash-' + $stamp)
$logDir  = Join-Path $outDir 'logs'
$dumpDir = Join-Path $outDir 'dumps'
$null = New-Item -ItemType Directory -Force -Path $logDir
$null = New-Item -ItemType Directory -Force -Path $dumpDir

$report = New-Object 'System.Collections.Generic.List[string]'

function Note([string]$text) {
    Write-Host $text
    [void]$report.Add($text)
}
function Head([string]$text) {
    Note ''
    Note ('== ' + $text + ' ==')
}
function Write-Text([string]$path, [string]$text) {
    $parent = Split-Path -Parent $path
    if ($parent -and -not (Test-Path -LiteralPath $parent)) {
        $null = New-Item -ItemType Directory -Force -Path $parent
    }
    [IO.File]::WriteAllText($path, $text, (New-Object Text.UTF8Encoding($false)))
}
function Safe-Hash([string]$path, [string]$algorithm) {
    # Get-FileHash is the easy path; files in use (a loaded DLL) can refuse it, so fall back to
    # hashing an explicitly shared read stream.
    try {
        return (Get-FileHash -LiteralPath $path -Algorithm $algorithm -ErrorAction Stop).Hash.ToLower()
    } catch { }
    try {
        $stream = [IO.File]::Open($path, [IO.FileMode]::Open, [IO.FileAccess]::Read, [IO.FileShare]::ReadWrite)
        try {
            $hasher = if ($algorithm -eq 'MD5') { [Security.Cryptography.MD5]::Create() } else { [Security.Cryptography.SHA256]::Create() }
            $digest = $hasher.ComputeHash($stream)
            return (($digest | ForEach-Object { $_.ToString('x2') }) -join '')
        } finally { $stream.Dispose() }
    } catch {
        return ('unavailable (' + $_.Exception.Message + ')')
    }
}
function Describe([string]$path, [string]$label) {
    if (-not (Test-Path -LiteralPath $path)) {
        Note ('  {0}: MISSING  ({1})' -f $label, $path)
        return
    }
    $f = Get-Item -LiteralPath $path
    Note ('  {0}: {1}' -f $label, $f.FullName)
    Note ('    bytes {0}  last write {1}' -f $f.Length, $f.LastWriteTime.ToString('yyyy-MM-dd HH:mm:ss'))
    try { Note ('    file version {0}' -f $f.VersionInfo.FileVersion) } catch { }
    Note ('    sha256 {0}' -f (Safe-Hash $path 'SHA256'))
    Note ('    md5    {0}' -f (Safe-Hash $path 'MD5'))
}

# ---------------------------------------------------------------- environment

Head 'environment'
Note ('  collected at   ' + (Get-Date).ToString('yyyy-MM-dd HH:mm:ss'))
Note ('  computer/user  ' + $env:COMPUTERNAME + ' / ' + $env:USERNAME)
try {
    $os = Get-CimInstance Win32_OperatingSystem -ErrorAction Stop
    Note ('  windows        ' + $os.Caption + ' build ' + $os.BuildNumber + ' (' + $os.OSArchitecture + ')')
} catch {
    Note ('  windows        ' + [Environment]::OSVersion.VersionString)
}
Note ('  powershell     ' + $PSVersionTable.PSVersion.ToString())
try {
    $isAdmin = (New-Object Security.Principal.WindowsPrincipal([Security.Principal.WindowsIdentity]::GetCurrent())).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
    Note ('  elevated       ' + $isAdmin)
} catch { Note '  elevated       unknown' }
Note ('  game folder    ' + $GameDir + '  exists: ' + (Test-Path -LiteralPath $GameDir))

$running = Get-Process -Name 'Vancouver' -ErrorAction SilentlyContinue
if ($running) {
    Note ''
    Note ('  WARNING: Vancouver.exe is running (PID ' + (($running | ForEach-Object { $_.Id }) -join ', ') + ').')
    Note '  The log being copied is the LIVE run, not the crashed one. Quit the game first,'
    Note '  then run this again if you want the crashed run''s log.'
}

# ---------------------------------------------------------------- debug log

Head 'XLiveLessNess debug log (rewritten on the next game launch)'
$logRoots = @(
    (Join-Path $env:LOCALAPPDATA 'XLiveLessNess\logs'),
    (Join-Path $env:APPDATA 'XLiveLessNess\logs'),
    (Join-Path $GameDir 'XLiveLessNess\logs')
)
$logIndex = 0
$logFound = 0
$logSummaries = New-Object 'System.Collections.Generic.List[string]'
foreach ($root in $logRoots) {
    if (-not $root -or -not (Test-Path -LiteralPath $root)) { continue }
    $logIndex++
    $files = @(Get-ChildItem -LiteralPath $root -Recurse -File -ErrorAction SilentlyContinue | Where-Object { $_.Extension -eq '.log' })
    if ($files.Count -eq 0) {
        Note ('  ' + $root + ': no .log files')
        continue
    }
    foreach ($f in $files) {
        $logFound++
        $rel = $f.FullName.Substring($root.Length).TrimStart('\')
        $destRel = ('set{0}\{1}' -f $logIndex, $rel)
        $dest = Join-Path $logDir $destRel
        $destParent = Split-Path -Parent $dest
        if (-not (Test-Path -LiteralPath $destParent)) { $null = New-Item -ItemType Directory -Force -Path $destParent }
        Copy-Item -LiteralPath $f.FullName -Destination $dest -Force -ErrorAction SilentlyContinue

        # The log is UTF-16LE without a BOM. Write a UTF-8 copy so no iconv is needed anywhere.
        $text = $null
        try {
            $bytes = [IO.File]::ReadAllBytes($f.FullName)
            if ($bytes.Length -ge 2 -and $bytes[0] -eq 0xFF -and $bytes[1] -eq 0xFE) {
                $text = [Text.Encoding]::Unicode.GetString($bytes, 2, $bytes.Length - 2)
            } elseif ($bytes.Length -ge 3 -and $bytes[0] -eq 0xEF -and $bytes[1] -eq 0xBB -and $bytes[2] -eq 0xBF) {
                $text = [Text.Encoding]::UTF8.GetString($bytes, 3, $bytes.Length - 3)
            } else {
                $text = [Text.Encoding]::Unicode.GetString($bytes)
            }
            if ($text -and $text.IndexOf([char]0) -ge 0) {
                $text = [Text.Encoding]::UTF8.GetString($bytes)
            }
        } catch {
            $text = $null
        }
        if ($text -ne $null) {
            Write-Text ($dest + '.utf8.txt') $text
        }

        $lineCount = 0
        if ($text) { $lineCount = ($text -split "`n").Count }
        $logSummaries.Add(('  set{0}\{1}  bytes {2}  lines {3}  last write {4}  sha256 {5}' -f `
            $logIndex, $rel, $f.Length, $lineCount, $f.LastWriteTime.ToString('yyyy-MM-dd HH:mm:ss'), (Safe-Hash $f.FullName 'SHA256')))
    }
}
foreach ($line in $logSummaries) { Note $line }
if ($logFound -eq 0) {
    Note '  MISSING: no xlln-debug log found anywhere.'
    Note '  If the game was relaunched after the crash, that run''s log is already gone.'
}

# raw copies of the primary log for the record
if ($logFound -gt 0) {
    Note ''
    Note '  last 5 log files by write time, newest first:'
    Get-ChildItem -LiteralPath $logDir -Recurse -File -ErrorAction SilentlyContinue |
        Where-Object { $_.Extension -eq '.log' } |
        Sort-Object LastWriteTime -Descending |
        Select-Object -First 5 |
        ForEach-Object { Note ('    ' + $_.LastWriteTime.ToString('yyyy-MM-dd HH:mm:ss') + '  ' + $_.FullName.Substring($logDir.Length).TrimStart('\')) }
}

# ---------------------------------------------------------------- dumps

Head 'Windows Error Reporting dumps'
$dumpCandidates = New-Object 'System.Collections.Generic.List[string]'
foreach ($key in @('HKCU:\SOFTWARE\Microsoft\Windows\Windows Error Reporting\LocalDumps',
                   'HKLM:\SOFTWARE\Microsoft\Windows\Windows Error Reporting\LocalDumps')) {
    try {
        $rootItem = Get-ItemProperty -Path $key -ErrorAction Stop
        if ($rootItem.DumpFolder) { $dumpCandidates.Add([Environment]::ExpandEnvironmentVariables($rootItem.DumpFolder)) }
        Get-ChildItem -Path $key -ErrorAction SilentlyContinue | ForEach-Object {
            $sub = Get-ItemProperty -Path $_.PSPath -ErrorAction SilentlyContinue
            if ($sub.DumpFolder) { $dumpCandidates.Add([Environment]::ExpandEnvironmentVariables($sub.DumpFolder)) }
        }
    } catch { }
}
$dumpCandidates.Add((Join-Path $env:LOCALAPPDATA 'CrashDumps'))
$dumpCandidates.Add((Join-Path $env:USERPROFILE 'Desktop\crash-dumps'))
$dumpCandidates.Add((Join-Path $env:USERPROFILE 'Desktop\crash-tools'))
$dumpCandidates.Add('C:\Windows\Minidump')
$dumpCandidates.Add((Join-Path $env:PROGRAMDATA 'Microsoft\Windows\WER\ReportArchive'))
$dumpCandidates.Add((Join-Path $env:PROGRAMDATA 'Microsoft\Windows\WER\ReportQueue'))

$dumpIndex = 0
$dumpCount = 0
$dumpLines = New-Object 'System.Collections.Generic.List[string]'
foreach ($dir in ($dumpCandidates | Where-Object { $_ } | Select-Object -Unique)) {
    if (-not (Test-Path -LiteralPath $dir)) { continue }
    $items = @(Get-ChildItem -LiteralPath $dir -Recurse -File -ErrorAction SilentlyContinue | Where-Object { $_.Extension -eq '.dmp' -or $_.Extension -eq '.wer' })
    foreach ($item in $items) {
        $dumpCount++
        $dumpIndex++
        $line = ('  {0}  bytes {1}  last write {2}  {3}' -f $item.FullName, $item.Length, $item.LastWriteTime.ToString('yyyy-MM-dd HH:mm:ss'), $item.Name)
        $dumpLines.Add($line)
        if (-not $SkipDumps) {
            $dest = Join-Path $dumpDir ('{0:D2}_{1}' -f $dumpIndex, $item.Name)
            Copy-Item -LiteralPath $item.FullName -Destination $dest -Force -ErrorAction SilentlyContinue
        }
    }
}
if ($dumpCount -eq 0) {
    Note '  no .dmp / .wer files found in any known location'
} else {
    foreach ($line in $dumpLines) { Note $line }
    if ($SkipDumps) {
        Note '  (dumps listed, not copied: -SkipDumps was used)'
    } else {
        Note ('  copied ' + $dumpCount + ' file(s) into the dumps folder (a full dump can be several hundred MB)')
    }
}
Write-Text (Join-Path $outDir 'dumps.txt') (($dumpLines -join "`r`n") + "`r`n")

# ---------------------------------------------------------------- event log

Head ('Application event log, Event ID 1000/1001, last ' + $EventDays + ' day(s)')
$since = (Get-Date).AddDays(-1 * [Math]::Abs($EventDays))
$events = @()
foreach ($id in @(1000, 1001)) {
    try {
        $events += @(Get-WinEvent -FilterHashtable @{ LogName = 'Application'; Id = $id; StartTime = $since } -ErrorAction Stop)
    } catch {
        # "No events were found" is normal and must not look like a failure.
    }
}
$eventText = New-Object 'System.Text.StringBuilder'
if ($events.Count -eq 0) {
    [void]$eventText.AppendLine('no Event ID 1000/1001 entries in the window')
    Note '  none found in the window'
} else {
    Note ('  ' + $events.Count + ' entry/entries')
    foreach ($ev in ($events | Sort-Object TimeCreated -Descending)) {
        [void]$eventText.AppendLine('RecordId : ' + $ev.RecordId)
        [void]$eventText.AppendLine('Time     : ' + $ev.TimeCreated.ToString('yyyy-MM-dd HH:mm:ss'))
        [void]$eventText.AppendLine('Provider : ' + $ev.ProviderName)
        [void]$eventText.AppendLine('EventId  : ' + $ev.Id)
        [void]$eventText.AppendLine('Level    : ' + $ev.LevelDisplayName)
        [void]$eventText.AppendLine('Message  :')
        [void]$eventText.AppendLine($ev.Message)
        [void]$eventText.AppendLine('')
        $firstLine = ($ev.Message -split "`r?`n")[0]
        Note ('    ' + $ev.TimeCreated.ToString('yyyy-MM-dd HH:mm:ss') + '  id ' + $ev.Id + '  ' + $firstLine)
    }
}
Write-Text (Join-Path $outDir 'crash-events.txt') $eventText.ToString()

# ---------------------------------------------------------------- files

Head 'versions and hashes'
Describe (Join-Path $GameDir 'Vancouver.exe') 'Vancouver.exe'
Describe (Join-Path $GameDir 'xlive.dll') 'xlive.dll'
Describe (Join-Path $GameDir 'd3dx9_38.dll') 'd3dx9_38.dll (D3D9 shim, if present)'
Describe (Join-Path $env:SystemRoot 'System32\msvcp140.dll') 'System32\msvcp140.dll (64-bit)'
Describe (Join-Path $env:SystemRoot 'SysWOW64\msvcp140.dll') 'SysWOW64\msvcp140.dll (32-bit, what the game loads)'
Describe (Join-Path $env:SystemRoot 'SysWOW64\vcruntime140.dll') 'SysWOW64\vcruntime140.dll'

# ---------------------------------------------------------------- registry config

Head 'WER LocalDumps registry configuration'
$regText = New-Object 'System.Text.StringBuilder'
foreach ($key in @('HKCU:\SOFTWARE\Microsoft\Windows\Windows Error Reporting\LocalDumps',
                   'HKLM:\SOFTWARE\Microsoft\Windows\Windows Error Reporting\LocalDumps')) {
    if (-not (Test-Path -Path $key)) {
        [void]$regText.AppendLine($key + ' : absent')
        Note ('  ' + $key + ' : absent')
        continue
    }
    [void]$regText.AppendLine('[' + $key + ']')
    Note ('  [' + $key + ']')
    $entries = @($key) + @(Get-ChildItem -Path $key -Recurse -ErrorAction SilentlyContinue | ForEach-Object { $_.Name })
    foreach ($subKey in $entries) {
        $entry = Get-ItemProperty -Path $subKey -ErrorAction SilentlyContinue
        if (-not $entry) { continue }
        $header = '  [' + $subKey + ']'
        [void]$regText.AppendLine($header)
        Note $header
        foreach ($prop in $entry.PSObject.Properties) {
            if ($prop.Name -like 'PS*') { continue }
            $line = ('    ' + $prop.Name + ' = ' + $prop.Value)
            [void]$regText.AppendLine($line)
            Note $line
        }
    }
}
Write-Text (Join-Path $outDir 'wer-localdumps.txt') $regText.ToString()

# ---------------------------------------------------------------- network

Head 'local IPv4 addresses (Radmin VPN matters for this crash)'
try {
    $addrs = Get-NetIPAddress -AddressFamily IPv4 -ErrorAction Stop |
        Select-Object InterfaceAlias, IPAddress, PrefixLength |
        Format-Table -AutoSize | Out-String -Width 200
    foreach ($line in ($addrs -split "`r?`n")) { if ($line.Trim()) { Note ('  ' + $line.TrimEnd()) } }
    Write-Text (Join-Path $outDir 'ipv4-addresses.txt') $addrs
} catch {
    Note '  Get-NetIPAddress unavailable'
}
try {
    $adapters = Get-NetAdapter -ErrorAction Stop |
        Select-Object Name, InterfaceDescription, Status |
        Format-Table -AutoSize | Out-String -Width 200
    Write-Text (Join-Path $outDir 'net-adapters.txt') $adapters
} catch { }

# ---------------------------------------------------------------- write report

Note ''
Note ('result folder: ' + $outDir)

$header = @(
    'Vancouver 2010 / XLiveLessNess invite-accept crash - evidence collected on the player''s PC',
    'Collected by collect-crash-run.ps1. Read-only run: nothing in the game folder was touched.',
    'Not collected on purpose: xlive.ini / the direct-IP settings, because they can contain a password.',
    ''
)
Write-Text (Join-Path $outDir 'report.txt') (($header -join "`r`n") + ($report -join "`r`n") + "`r`n")

$readme = @(
    'vancouver-crash-<timestamp>',
    '',
    'report.txt          what was collected, with sizes, timestamps and hashes',
    'logs\set1\...       copy of every XLiveLessNess log found, plus a .utf8.txt next to each',
    '                    log: the original is UTF-16LE without a BOM; the .utf8.txt is the same',
    '                    text in UTF-8, use that one',
    'dumps\*.dmp         Windows Error Reporting dumps (present only if WER LocalDumps is on)',
    'dumps.txt           every dump found, whether or not it was copied',
    'crash-events.txt    Application event log entries 1000/1001 (P1..P10 live in the message)',
    'wer-localdumps.txt  the WER LocalDumps registry configuration',
    'ipv4-addresses.txt  local addresses; the Radmin VPN adapter address is the one that matters',
    'net-adapters.txt    adapter list, to identify the VPN interface',
    '',
    'Send the whole .zip back. Do not launch the game before it is sent: the log is rewritten',
    'on the next launch.'
)
Write-Text (Join-Path $outDir 'README.txt') (($readme -join "`r`n") + "`r`n")

# ---------------------------------------------------------------- zip

if (-not $SkipZip) {
    $zipPath = $outDir + '.zip'
    $zipped = $false
    try {
        Compress-Archive -Path $outDir -DestinationPath $zipPath -Force -ErrorAction Stop
        $zipped = $true
    } catch {
        Note ('Compress-Archive failed: ' + $_.Exception.Message)
        try {
            & tar -a -c -f $zipPath -C (Split-Path -Parent $outDir) (Split-Path -Leaf $outDir)
            if ($LASTEXITCODE -eq 0) { $zipped = $true }
        } catch {
            Note 'tar fallback also failed; send the folder contents instead.'
        }
    }
    if ($zipped -and (Test-Path -LiteralPath $zipPath)) {
        $zipItem = Get-Item -LiteralPath $zipPath
        Note ''
        Note 'DONE - one file to send back:'
        Note ('  ' + $zipItem.FullName)
        Note ('  bytes  ' + $zipItem.Length + '  (' + [Math]::Round($zipItem.Length / 1MB, 1) + ' MB)')
        Note ('  sha256 ' + (Safe-Hash $zipPath 'SHA256'))
    }
} else {
    Note ''
    Note 'DONE - -SkipZip was used, so the folder is not zipped.'
}

Note ''
Note 'Do not launch the game until this has been sent: the log is rewritten on the next launch.'
exit 0
