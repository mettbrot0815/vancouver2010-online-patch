<#
  Entfernt den Disc-Check von Vancouver 2010 (PC).
  Vancouver.exe laedt "IsDiskInTray" per GetProcAddress aus GameUtil.dll und
  bricht den Start ab, wenn die Funktion 0 zurueckgibt. Gepatcht wird die
  Funktion auf "return true" (mov al,1 ; ret).
  Die RVA wird aus der Export-Tabelle gelesen (kein festes Offset im Code),
  die Originalbytes werden byteweise geprueft.
  -Restore stellt die Sicherung (GameUtil.dll.orig) wieder her.
#>
param(
    [string]$GameDir,
    [switch]$Restore
)
$ErrorActionPreference = 'Stop'

function Find-GameDir {
    param([string]$hint)
    if ($hint -and (Test-Path (Join-Path $hint 'Vancouver.exe'))) { return $hint }
    if ($PSScriptRoot -and (Test-Path (Join-Path $PSScriptRoot 'Vancouver.exe'))) { return $PSScriptRoot }
    if ($PSScriptRoot -and (Test-Path (Join-Path $PSScriptRoot 'Vancouver 2010\Vancouver.exe'))) { return (Join-Path $PSScriptRoot 'Vancouver 2010') }
    foreach ($k in @('HKLM:\SOFTWARE\WOW6432Node\SEGA\Vancouver 2010','HKLM:\SOFTWARE\SEGA\Vancouver 2010')) {
        if (Test-Path $k) {
            $loc = (Get-ItemProperty $k).InstallLocation
            if ($loc -and (Test-Path (Join-Path $loc 'Vancouver.exe'))) { return $loc }
        }
    }
    return $null
}

function Get-PeInfo {
    param([byte[]]$Bytes)
    if ($Bytes[0] -ne 0x4D -or $Bytes[1] -ne 0x5A) { throw "GameUtil.dll ist keine PE-Datei." }
    $e = [BitConverter]::ToInt32($Bytes, 0x3C)
    $nsec = [BitConverter]::ToUInt16($Bytes, $e + 6)
    $optsz = [BitConverter]::ToUInt16($Bytes, $e + 20)
    $opt = $e + 24
    $magic = [BitConverter]::ToUInt16($Bytes, $opt)
    $pe32plus = ($magic -eq 0x20B)
    $secs = @()
    for ($i = 0; $i -lt $nsec; $i++) {
        $o = $opt + $optsz + $i * 40
        $secs += [pscustomobject]@{
            VA    = [BitConverter]::ToUInt32($Bytes, $o + 12)
            VSize = [BitConverter]::ToUInt32($Bytes, $o + 8)
            Raw   = [BitConverter]::ToUInt32($Bytes, $o + 20)
            RSize = [BitConverter]::ToUInt32($Bytes, $o + 16)
        }
    }
    # Data directory 0 = Export Table
    $dd = $opt + $(if ($pe32plus) { 112 } else { 96 })
    $expRva = [BitConverter]::ToUInt32($Bytes, $dd)
    $expSize = [BitConverter]::ToUInt32($Bytes, $dd + 4)
    return [pscustomobject]@{ Sections = $secs; ExportRva = $expRva; ExportSize = $expSize; Pe32Plus = $pe32plus }
}

function Convert-RvaToOffset {
    param([pscustomobject]$Pe, [uint32]$Rva)
    foreach ($s in $Pe.Sections) {
        if ($Rva -ge $s.VA -and $Rva -lt ($s.VA + [Math]::Max($s.VSize, $s.RSize))) {
            return [int]($s.Raw + ($Rva - $s.VA))
        }
    }
    return -1
}

function Get-ExportRva {
    param([byte[]]$Bytes, [pscustomobject]$Pe, [string]$Name)
    if ($Pe.ExportRva -eq 0) { throw "GameUtil.dll hat keine Export-Tabelle." }
    $eo = Convert-RvaToOffset -Pe $Pe -Rva $Pe.ExportRva
    if ($eo -lt 0) { throw "Export-Tabelle liegt in keiner Section." }
    $nfunc  = [BitConverter]::ToUInt32($Bytes, $eo + 20)
    $nname  = [BitConverter]::ToUInt32($Bytes, $eo + 24)
    $aof    = [BitConverter]::ToUInt32($Bytes, $eo + 28)
    $aon    = [BitConverter]::ToUInt32($Bytes, $eo + 32)
    $aono   = [BitConverter]::ToUInt32($Bytes, $eo + 36)
    $base   = [BitConverter]::ToUInt32($Bytes, $eo + 16)
    $ofOff  = Convert-RvaToOffset -Pe $Pe -Rva $aof
    $onOff  = Convert-RvaToOffset -Pe $Pe -Rva $aon
    $onoOff = Convert-RvaToOffset -Pe $Pe -Rva $aono
    for ($j = 0; $j -lt $nname; $j++) {
        $nameRva = [BitConverter]::ToUInt32($Bytes, $onOff + $j * 4)
        $nameOff = Convert-RvaToOffset -Pe $Pe -Rva $nameRva
        if ($nameOff -lt 0) { continue }
        $end = $nameOff
        while ($Bytes[$end] -ne 0) { $end++ }
        $str = [Text.Encoding]::ASCII.GetString($Bytes, $nameOff, $end - $nameOff)
        if ($str -ne $Name) { continue }
        $ordIndex = [BitConverter]::ToUInt16($Bytes, $onoOff + $j * 2)
        if ($ordIndex -ge $nfunc) { continue }
        return [BitConverter]::ToUInt32($Bytes, $ofOff + $ordIndex * 4)
    }
    return 0
}

function Format-Bytes {
    param([byte[]]$Bytes)
    return (($Bytes | ForEach-Object { '{0:X2}' -f $_ }) -join ' ')
}

$dir = Find-GameDir -hint $GameDir
if (-not $dir) { throw "Spielordner nicht gefunden (Vancouver.exe)." }
$dll = Join-Path $dir 'GameUtil.dll'
$bak = "$dll.orig"
if (-not (Test-Path $dll)) { throw "GameUtil.dll fehlt in $dir" }

if ($Restore) {
    if (-not (Test-Path $bak)) { throw "Keine Sicherung vorhanden ($bak)." }
    Copy-Item $bak $dll -Force
    Write-Host "Disc-Check wiederhergestellt (Original-DLL zurueck)." -ForegroundColor Green
    return
}

$b = [IO.File]::ReadAllBytes($dll)
$pe = Get-PeInfo -Bytes $b
$rva = Get-ExportRva -Bytes $b -Pe $pe -Name 'IsDiskInTray'
if ($rva -eq 0) { throw "Export 'IsDiskInTray' nicht gefunden - unerwartete GameUtil.dll." }
$off = Convert-RvaToOffset -Pe $pe -Rva $rva
if ($off -lt 0) { throw ("RVA 0x{0:X} liegt in keiner Section." -f $rva) }

$patched = @(0xB0, 0x01, 0xC3)                       # mov al,1 ; ret
$signature = @(0x53, 0x56, 0x57, 0x6A, 0x00, 0x8B)   # push ebx/esi/edi; push 0; mov edi,edx

if (($b[$off] -eq 0xB0) -and ($b[$off+1] -eq 0x01) -and ($b[$off+2] -eq 0xC3)) {
    Write-Host "Disc-Check ist bereits gepatcht ($dll)." -ForegroundColor Green
    return
}

# byteweise vergleichen (Compare-Object vergleicht Mengen, nicht die Reihenfolge)
$current = $b[$off..($off + $signature.Count - 1)]
for ($i = 0; $i -lt $signature.Count; $i++) {
    if ($current[$i] -ne $signature[$i]) {
        throw ("Signatur passt nicht bei Offset 0x{0:X}: {1} (erwartet {2})" -f $off, (Format-Bytes $current), (Format-Bytes $signature))
    }
}

if (-not (Test-Path $bak)) { Copy-Item $dll $bak -Force; Write-Host "Sicherung: $bak" }
$b[$off]   = $patched[0]
$b[$off+1] = $patched[1]
$b[$off+2] = $patched[2]
[IO.File]::WriteAllBytes($dll, $b)
Write-Host ("Disc-Check entfernt: {0} (IsDiskInTray, RVA 0x{1:X}, Datei-Offset 0x{2:X})" -f $dll, $rva, $off) -ForegroundColor Green
Write-Host "Rueckgaengig: patch-nodvd.ps1 -Restore"
