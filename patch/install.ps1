<#
  Vancouver 2010 (PC) - Online-Patch
  Ersetzt das abgeschaltete Games for Windows - LIVE durch XLiveLessNess (XLLN, GPL).

  Aufruf (einfach):   Rechte Maustaste -> "Mit PowerShell ausfuehren"  (oder install.cmd)
  Aufruf (direkt):    powershell -ExecutionPolicy Bypass -File install.ps1 [-Name "Spieler"] [-DirectIp "1.2.3.4:39001"]
#>
param(
    [string]$Name,
    [string]$DirectIp,
    [switch]$NoFirewall,
    [switch]$NoDvd
)

$ErrorActionPreference = 'Stop'
$base = Split-Path -Parent $MyInvocation.MyCommand.Path

# --- Administrator-Rechte sicherstellen ---
$isAdmin = ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
if (-not $isAdmin) {
    $args = @('-NoProfile','-ExecutionPolicy','Bypass','-File',"`"$PSCommandPath`"")
    if ($Name)     { $args += @('-Name', $Name) }
    if ($DirectIp) { $args += @('-DirectIp', $DirectIp) }
    if ($NoFirewall) { $args += '-NoFirewall' }
    if ($NoDvd) { $args += '-NoDvd' }
    Start-Process powershell -Verb RunAs -ArgumentList $args
    return
}

Write-Host "== Vancouver 2010 Online-Patch ==" -ForegroundColor Cyan

# --- Spielordner finden ---
$gameDir = $null
foreach ($key in @('HKLM:\SOFTWARE\WOW6432Node\SEGA\Vancouver 2010','HKLM:\SOFTWARE\SEGA\Vancouver 2010')) {
    if (Test-Path $key) {
        $loc = (Get-ItemProperty $key).InstallLocation
        if ($loc -and (Test-Path (Join-Path $loc 'Vancouver.exe'))) { $gameDir = $loc; break }
    }
}
if (-not $gameDir) {
    foreach ($cand in @("$env:ProgramFiles(x86)\SEGA\Vancouver 2010", "$env:ProgramFiles\SEGA\Vancouver 2010")) {
        if (Test-Path (Join-Path $cand 'Vancouver.exe')) { $gameDir = $cand; break }
    }
}
if (-not $gameDir) { throw "Vancouver 2010 nicht gefunden. Bitte erst das Spiel installieren." }
Write-Host "Spielordner: $gameDir"

# --- DLL installieren ---
$dllSrc = Join-Path $base 'xlive.dll'
if (-not (Test-Path $dllSrc)) { throw "xlive.dll fehlt im Patch-Ordner." }
$dllDst = Join-Path $gameDir 'xlive.dll'
if (Test-Path $dllDst) { Copy-Item $dllDst "$dllDst.bak" -Force }
Copy-Item $dllSrc $dllDst -Force
Write-Host "xlive.dll installiert:" ([math]::Round((Get-Item $dllDst).Length/1KB)) "KB"

# --- Profil in XLLN-Konfiguration setzen ---
$cfgDir = Join-Path $env:LOCALAPPDATA 'XLiveLessNess'
New-Item -ItemType Directory -Force -Path $cfgDir | Out-Null
$cfg = Join-Path $cfgDir 'xlln-config-1.ini'
if (-not (Test-Path $cfg)) {
    Copy-Item (Join-Path $base 'xlln-config-template.ini') $cfg -Force
    Write-Host "XLLN-Konfiguration angelegt."
} else {
    Copy-Item $cfg "$cfg.bak" -Force
}

# Profilname immer erfragen, wenn er nicht als Parameter kam (kein Default, kein Hardcode).
$profileName = $Name
while (-not $profileName) {
    $profileName = Read-Host "Profilname fuer das Spiel (max. 15 Zeichen)"
    if ($profileName) { $profileName = $profileName.Trim() }
    if ($profileName -and $profileName.Length -gt 15) {
        Write-Host "Zu lang - bitte hoechstens 15 Zeichen." -ForegroundColor Yellow
        $profileName = $null
    }
}

if (-not $DirectIp) {
    $answer = Read-Host "Direktverbindung zu einem Host? IP:Port eingeben, oder Enter fuer automatische LAN-Suche"
    if ($answer) { $DirectIp = $answer.Trim() }
}

$text = Get-Content $cfg -Raw
$text = [regex]::Replace($text, '(?m)^xlive_username_p1\s*=.*$',              "xlive_username_p1 = $profileName")
$text = [regex]::Replace($text, '(?m)^xlive_user_live_enabled_p1\s*=.*$',     'xlive_user_live_enabled_p1 = 1')
$text = [regex]::Replace($text, '(?m)^xlive_user_online_enabled_p1\s*=.*$',   'xlive_user_online_enabled_p1 = 1')
$text = [regex]::Replace($text, '(?m)^xlive_user_auto_login_p1\s*=.*$',       'xlive_user_auto_login_p1 = 1')
if ($DirectIp) {
    $text = [regex]::Replace($text, '(?m)^xlln_direct_ip_connect_ip_port\s*=.*$', "xlln_direct_ip_connect_ip_port = $DirectIp")
    Write-Host "Direktverbindung gesetzt: $DirectIp"
}
[IO.File]::WriteAllText($cfg, $text, (New-Object System.Text.UTF8Encoding($false)))
Write-Host "Profil: $profileName (online, Auto-Login)"

# --- Disc-Check entfernen (optional) ---
$patcher = Join-Path $base 'patch-nodvd.ps1'
$patched = $false
if (Test-Path $patcher) {
    $apply = $NoDvd.IsPresent
    if (-not $apply) {
        $answer = Read-Host "Disc-Check entfernen (kein DVD/ISO mehr noetig)? (J/N)"
        if ($answer -match '^[JjYy]') { $apply = $true }
    }
    if ($apply) {
        try { & $patcher -GameDir $gameDir; $patched = $true }
        catch { Write-Host "Disc-Check-Patch fehlgeschlagen: $($_.Exception.Message) - DVD/ISO wird gebraucht." -ForegroundColor Yellow }
    }
}

# --- Firewall ---
if (-not $NoFirewall) {
    $exe = Join-Path $gameDir 'Vancouver.exe'
    foreach ($proto in @('UDP','TCP')) {
        $ruleName = "Vancouver 2010 (XLLN) $proto"
        if (-not (Get-NetFirewallRule -DisplayName $ruleName -ErrorAction SilentlyContinue)) {
            New-NetFirewallRule -DisplayName $ruleName -Direction Inbound -Action Allow -Program $exe -Protocol $proto -LocalPort 39000-39001 | Out-Null
            Write-Host "Firewall-Regel angelegt: $ruleName"
        }
    }
}

# --- Alten GFWL-Client entfernen (wird nicht mehr gebraucht) ---
$gfwl = Get-ItemProperty 'HKLM:\SOFTWARE\WOW6432Node\Microsoft\Windows\CurrentVersion\Uninstall\*','HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall\*' -ErrorAction SilentlyContinue |
        Where-Object { $_.DisplayName -match 'Games for Windows - LIVE' -or $_.DisplayName -match 'Games for Windows Live' }
if ($gfwl) {
    Write-Host ""
    Write-Host "Games for Windows - LIVE ist installiert ($($gfwl.DisplayName -join ', '))." -ForegroundColor Yellow
    Write-Host "Der Patch braucht es nicht - XLiveLessNess ersetzt es komplett."
    $answer = Read-Host "GFWL jetzt deinstallieren? (J/N)"
    if ($answer -match '^[JjYy]') {
        foreach ($g in $gfwl) {
            $uninst = $g.UninstallString
            if ($uninst) {
                if ($uninst -match 'MsiExec\.exe\s+/I(\{[0-9A-Fa-f-]+\})') {
                    $guid = $matches[1]
                    Start-Process msiexec.exe -ArgumentList "/x $guid /qn /norestart" -Wait
                } else {
                    Start-Process cmd.exe -ArgumentList "/c `"$uninst`" /S" -Wait
                }
                Write-Host "Deinstalliert: $($g.DisplayName)"
            }
        }
    } else {
        Write-Host "Uebersprungen - GFWL bleibt installiert, stoert aber nicht."
    }
}

Write-Host ""
Write-Host "Fertig." -ForegroundColor Green
if ($patched) { Write-Host "1) Spiel starten - kein DVD/ISO noetig (Disc-Check entfernt)." }
else { Write-Host "1) DVD einlegen oder ISO mounten (Disc-Check)." }
Write-Host "2) Spiel starten, START druecken -> Menue -> 'Play On Line'."
Write-Host "3) Ein Spieler: 'Create Game' (Lobby). Andere: Lobby-Liste (LAN/VPN) oder Direktverbindung."
Write-Host "   Direkt: xlln_direct_ip_connect_ip_port in %LOCALAPPDATA%\XLiveLessNess\xlln-config-1.ini (Spiel dabei schliessen)."
