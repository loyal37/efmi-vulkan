# switch_renderer.ps1
# Switches the XXMI Launcher's EFMI profile between the DirectX 11 path (3DMigoto's d3d11.dll
# injected, game started with -force-d3d11) and the Vulkan path (efmi_vk_inject.dll injected as an
# extra library, XXMI DLL injection skipped, no -force-d3d11).
# It edits five values of the EFMI importer in "XXMI Launcher Config.json" and signs the Inject
# Libraries value with the launcher's own key pair, so the launcher accepts the change without its
# Reset/Keep dialog. Nothing else in the file is touched. With EFF installed in the game folder, its
# proxy vulkan-1.dll is renamed out of the way for Vulkan (the layer cannot hook through that proxy)
# and renamed back for DirectX 11.
#
# Layout, as the release describes it: this script sits in EFMI\EFMI-Vulkan (or in EFMI itself),
# the EFMI folder holds Mods and Core. The launcher may be anywhere: it is found from the EFMI
# folder's parent, the registry (its installer's entry, Windows' application usage records), the
# shortcuts it creates, or a running launcher, and its config must name this EFMI folder. Failing
# all that, the user drops the config file into the window and the place is kept in
# switch_renderer.cfg next to this script. No disk is ever scanned.
#
# Runs on Windows PowerShell 5.1 (built into Windows) and on PowerShell 7. The .bat calls it with
# -Mode toggle; the other parameters are for tests.

[CmdletBinding()]
param(
    [ValidateSet('dx11', 'vulkan', 'toggle', 'status')]
    [string]$Mode = 'toggle',
    [string]$ConfigPath = '',   # launcher config file to edit; empty = find it
    [string]$LauncherRoot = '', # XXMI Launcher folder (config and signing keys); empty = find it
    [string]$VulkanDll = '',    # efmi_vk_inject.dll to inject; empty = next to this script, else EFMI\EFMI-Vulkan\
    [string]$GameFolder = '',   # game folder for the EFF proxy file; empty = the launcher config's game_folder
    [string]$BackupDir = '',    # where the config backup goes; empty = backups\ next to this script
    [switch]$DryRun,            # show what would change, write and rename nothing
    [switch]$SkipRunningCheck   # tests on a config copy only: do not refuse while the launcher or game runs
)

$ErrorActionPreference = 'Stop'
try { [Console]::OutputEncoding = New-Object Text.UTF8Encoding($false) } catch {}

$ConfigFileName = 'XXMI Launcher Config.json'
$VulkanDllName  = 'efmi_vk_inject.dll'
$KeyFileRelPath = 'Resources\Security\private_key.der'
$EffMarker      = 'plugin\eff.dll'
$EffProxy       = 'vulkan-1.dll'
$ParkedSuffix   = '.eff-parked'
$Utf8NoBom      = New-Object Text.UTF8Encoding($false)
$ScriptDir      = $PSScriptRoot
$SettingsFile   = Join-Path $ScriptDir 'switch_renderer.cfg'

function Info([string]$msg) { Write-Host $msg }
function Warn([string]$msg) { Write-Host "[note] $msg" -ForegroundColor Yellow }
function Fail([string]$msg) { Write-Host ''; Write-Host "[error] $msg" -ForegroundColor Red; exit 1 }

# ---------- paths and settings ----------

# Comparable form of a path: full, backslashes, no trailing slash, lower case.
function ConvertTo-KeyPath([string]$p) {
    $p = $p.Trim().Trim('"').Replace('/', '\')
    try { $p = [IO.Path]::GetFullPath($p) } catch {}
    return $p.TrimEnd('\').ToLowerInvariant()
}

# The EFMI folder is the first folder upward that holds Mods and Core\EFMI.
function Find-EfmiRoot([string]$start) {
    $dir = $start
    for ($i = 0; $i -lt 4 -and $dir; $i++) {
        if ((Test-Path (Join-Path $dir 'Mods') -PathType Container) -and
            (Test-Path (Join-Path $dir 'Core\EFMI') -PathType Container)) { return $dir }
        $dir = Split-Path -Parent $dir
    }
    return $null
}

# Settings file next to the script: key = value lines, ; or # comments. Absent = defaults.
function Read-Settings([string]$path) {
    $s = @{ launcher_root = ''; dx11_inject_mode = 'DIRECT'; dx11_cmd_args = '-force-d3d11'; vulkan_keep_other_libraries = '0'; eff_park = '1' }
    if (-not (Test-Path $path)) { return $s }
    foreach ($line in [IO.File]::ReadAllLines($path, [Text.Encoding]::UTF8)) {
        $t = $line.Trim()
        if ($t -eq '' -or $t.StartsWith(';') -or $t.StartsWith('#')) { continue }
        $eq = $t.IndexOf('=')
        if ($eq -lt 1) { continue }
        $k = $t.Substring(0, $eq).Trim().ToLowerInvariant()
        $v = $t.Substring($eq + 1).Trim()
        if ($s.ContainsKey($k)) { $s[$k] = $v }
    }
    return $s
}

# Write one value back into the settings file, keeping its comments; appends the line if missing.
function Save-Setting([string]$path, [string]$key, [string]$value) {
    try {
        $lines = @()
        if (Test-Path $path) { $lines = [IO.File]::ReadAllLines($path, [Text.Encoding]::UTF8) }
        $done = $false
        for ($i = 0; $i -lt $lines.Count; $i++) {
            if ($lines[$i] -match ('^\s*' + [regex]::Escape($key) + '\s*=')) { $lines[$i] = "$key = $value"; $done = $true; break }
        }
        if (-not $done) { $lines += "$key = $value" }
        [IO.File]::WriteAllLines($path, [string[]]$lines, $Utf8NoBom)
        Info "Remembered in switch_renderer.cfg: $key = $value"
    } catch { Warn "Could not write switch_renderer.cfg ($($_.Exception.Message)); the search runs again next time." }
}

# The EFMI folder the launcher config points at (importer_folder, relative to the launcher folder when not absolute).
function Get-ConfigEfmiFolder($json, [string]$root) {
    $folder = [string]$json.Importers.EFMI.Importer.importer_folder
    if (-not $folder) { $folder = 'EFMI' }
    $folder = $folder.Replace('/', '\')
    if (-not [IO.Path]::IsPathRooted($folder)) { $folder = Join-Path $root $folder }
    try { $folder = [IO.Path]::GetFullPath($folder) } catch {}
    return $folder.TrimEnd('\')
}

# ---------- finding the launcher (no disk scan) ----------

# From any path that points at the launcher (its folder, its exe, its config, a shortcut target)
# to the folder that holds the config, walking up a few levels.
function Resolve-LauncherRoot([string]$path) {
    if (-not $path) { return $null }
    $p = $path.Trim().Trim('"')
    try { $p = [IO.Path]::GetFullPath($p) } catch { return $null }
    if (Test-Path $p -PathType Leaf) { $p = Split-Path -Parent $p }
    for ($i = 0; $i -lt 4 -and $p; $i++) {
        if (Test-Path (Join-Path $p $ConfigFileName) -PathType Leaf) { return $p }
        $p = Split-Path -Parent $p
    }
    return $null
}

# Does this launcher folder's config name our EFMI folder?
function Test-LauncherFor([string]$root, [string]$efmiRoot) {
    if (-not $root) { return $false }
    $cfgFile = Join-Path $root $ConfigFileName
    if (-not (Test-Path $cfgFile -PathType Leaf)) { return $false }
    try { $j = [IO.File]::ReadAllText($cfgFile, [Text.Encoding]::UTF8) | ConvertFrom-Json } catch { return $false }
    if (-not $j.Importers.EFMI.Importer) { return $false }
    return ((ConvertTo-KeyPath (Get-ConfigEfmiFolder $j $root)) -eq (ConvertTo-KeyPath $efmiRoot))
}

# Where Windows itself records the launcher, the way the launcher's own AppLocator finds other
# programs: the uninstall list (its installer registers "XXMI Launcher" with InstallLocation) and
# the application-usage keys (every exe that was run or switched to, so a portable copy counts too).
function Get-RegistryTargets {
    $out = @()
    foreach ($k in 'HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall',
                   'HKLM:\SOFTWARE\WOW6432Node\Microsoft\Windows\CurrentVersion\Uninstall',
                   'HKCU:\SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall') {
        foreach ($sub in Get-ChildItem $k -ErrorAction SilentlyContinue) {
            try { $p = Get-ItemProperty $sub.PSPath -ErrorAction Stop } catch { continue }
            if (-not ([string]$p.DisplayName -match '^XXMI Launcher')) { continue }
            if ($p.InstallLocation) { $out += [string]$p.InstallLocation }
            foreach ($v in @([string]$p.DisplayIcon, [string]$p.UninstallString)) {
                $m = [regex]::Match($v, '(?i)"?([A-Za-z]:\\[^"]*?XXMI Launcher\.exe)')
                if ($m.Success) { $out += $m.Groups[1].Value }
            }
        }
    }
    foreach ($k in 'HKCU:\SOFTWARE\Classes\Local Settings\Software\Microsoft\Windows\Shell\MuiCache',
                   'HKCU:\SOFTWARE\Microsoft\Windows\CurrentVersion\Explorer\FeatureUsage\AppSwitched',
                   'HKCU:\SOFTWARE\Microsoft\Windows\CurrentVersion\Explorer\FeatureUsage\ShowJumpView') {
        try { $p = Get-ItemProperty $k -ErrorAction Stop } catch { continue }
        foreach ($name in $p.PSObject.Properties.Name) {
            $m = [regex]::Match($name, '(?i)^([A-Za-z]:\\.*\\XXMI Launcher\.exe)')
            if ($m.Success) { $out += $m.Groups[1].Value }
        }
    }
    return $out
}

# Shortcuts the launcher creates (XXMI Launcher.lnk on the desktop, "<importer> Quick Start.lnk",
# start menu entries) and any other shortcut to its exe, where Windows keeps them.
function Get-ShortcutTargets {
    $out = @()
    try { $shell = New-Object -ComObject WScript.Shell } catch { return $out }
    $dirs = @()
    foreach ($name in 'Desktop', 'CommonDesktopDirectory', 'StartMenu', 'CommonStartMenu', 'Recent') {
        try { $d = [Environment]::GetFolderPath($name) } catch { $d = $null }
        if ($d) { $dirs += $d }
    }
    $dirs += (Join-Path $env:APPDATA 'Microsoft\Internet Explorer\Quick Launch')
    foreach ($d in $dirs) {
        if (-not (Test-Path $d)) { continue }
        foreach ($lnk in Get-ChildItem -Path $d -Filter '*.lnk' -Recurse -Depth 3 -ErrorAction SilentlyContinue) {
            try { $t = $shell.CreateShortcut($lnk.FullName).TargetPath } catch { continue }
            if (-not $t) { continue }
            $leaf = [IO.Path]::GetFileName($t)
            if (($leaf -ieq 'XXMI Launcher.exe') -or ($leaf -ieq $ConfigFileName) -or ($lnk.Name -match '^XXMI Launcher|Quick Start')) { $out += $t }
        }
    }
    return $out
}

# The launcher that manages $efmiRoot: the settings file, the EFMI folder's parent, the registry,
# the launcher's shortcuts, a running launcher; each checked against its config. Returns
# @{ Root; Source } or null when none fits.
function Find-Launcher([string]$efmiRoot, [hashtable]$settings) {
    $candidates = @()
    if ($settings.launcher_root) { $candidates += @{ Path = $settings.launcher_root; Source = 'switch_renderer.cfg' } }
    $parent = Split-Path -Parent $efmiRoot
    if ($parent) { $candidates += @{ Path = $parent; Source = "the EFMI folder's parent" } }
    foreach ($t in Get-RegistryTargets) { $candidates += @{ Path = $t; Source = 'the registry (install or usage record)' } }
    foreach ($t in Get-ShortcutTargets) { $candidates += @{ Path = $t; Source = 'a shortcut' } }
    foreach ($p in Get-Process -Name 'XXMI Launcher' -ErrorAction SilentlyContinue) { try { if ($p.Path) { $candidates += @{ Path = $p.Path; Source = 'the running launcher' } } } catch {} }
    $seen = @{}
    foreach ($c in $candidates) {
        $r = Resolve-LauncherRoot $c.Path
        if (-not $r) { continue }
        $k = ConvertTo-KeyPath $r
        if ($seen.ContainsKey($k)) { continue }
        $seen[$k] = $true
        if (Test-LauncherFor $r $efmiRoot) { return @{ Root = $r; Source = $c.Source } }
    }
    return $null
}

# ---------- signing with the launcher's own key ----------

function Import-LauncherKey([string]$derFile) {
    $der = [Convert]::FromBase64String(([IO.File]::ReadAllText($derFile)).Trim())
    $key = [System.Security.Cryptography.CngKey]::Import($der, [System.Security.Cryptography.CngKeyBlobFormat]::Pkcs8PrivateBlob)
    $ecdsa = New-Object System.Security.Cryptography.ECDsaCng($key)
    $ecdsa.HashAlgorithm = [System.Security.Cryptography.CngAlgorithm]::Sha256
    return $ecdsa
}

# ECDSA signatures: .NET gives r||s, the launcher (Python cryptography) stores DER SEQUENCE{INTEGER r, INTEGER s}.
function ConvertTo-DerInteger([byte[]]$raw) {
    $i = 0
    while ($i -lt $raw.Length - 1 -and $raw[$i] -eq 0) { $i++ }
    $body = @($raw[$i..($raw.Length - 1)])
    if ($body[0] -ge 0x80) { $body = @([byte]0) + $body }
    return @([byte]2, [byte]$body.Length) + $body
}

function ConvertTo-DerSignature([byte[]]$rs) {
    $half = [int]($rs.Length / 2)
    $body = @(ConvertTo-DerInteger ([byte[]]$rs[0..($half - 1)])) + @(ConvertTo-DerInteger ([byte[]]$rs[$half..($rs.Length - 1)]))
    if ($body.Length -ge 128) { throw 'unexpected ECDSA signature length' }
    return [byte[]](@([byte]0x30, [byte]$body.Length) + $body)
}

function ConvertFrom-DerSignature([byte[]]$der, [int]$half) {
    if ($der[0] -ne 0x30) { throw 'signature is not a DER sequence' }
    $pos = 2
    $out = New-Object byte[] ($half * 2)
    for ($k = 0; $k -lt 2; $k++) {
        if ($der[$pos] -ne 2) { throw 'signature is not a DER integer pair' }
        $len = [int]$der[$pos + 1]; $pos += 2
        $val = @($der[$pos..($pos + $len - 1)]); $pos += $len
        $j = 0
        while ($j -lt $val.Length - 1 -and $val[$j] -eq 0) { $j++ }
        $val = @($val[$j..($val.Length - 1)])
        [Array]::Copy([byte[]]$val, 0, $out, $k * $half + ($half - $val.Length), $val.Length)
    }
    return $out
}

function Test-LauncherSignature($ecdsa, [string]$value, [string]$signatureB64) {
    if (-not $signatureB64) { return $false }
    try {
        $rs = ConvertFrom-DerSignature ([Convert]::FromBase64String($signatureB64)) ([int]($ecdsa.KeySize / 8))
        return $ecdsa.VerifyData([Text.Encoding]::UTF8.GetBytes($value), $rs)
    } catch { return $false }
}

function New-LauncherSignature($ecdsa, [string]$value) {
    $data = [Text.Encoding]::UTF8.GetBytes($value)
    $rs = $ecdsa.SignData($data)
    if (-not $ecdsa.VerifyData($data, $rs)) { throw 'the new signature does not verify' }
    return [Convert]::ToBase64String((ConvertTo-DerSignature $rs))
}

# ---------- editing the JSON text in place ----------

# Index of the '}' that closes the '{' at $openIndex, skipping strings.
function Find-BlockEnd([string]$text, [int]$openIndex) {
    $depth = 0; $inStr = $false; $esc = $false
    for ($i = $openIndex; $i -lt $text.Length; $i++) {
        $c = $text[$i]
        if ($inStr) {
            if ($esc) { $esc = $false } elseif ($c -eq '\') { $esc = $true } elseif ($c -eq '"') { $inStr = $false }
            continue
        }
        if ($c -eq '"') { $inStr = $true; continue }
        if ($c -eq '{') { $depth++ }
        elseif ($c -eq '}') { $depth--; if ($depth -eq 0) { return $i } }
    }
    throw 'unbalanced braces in the config file'
}

# Span (open, close) of the object value of "$key" inside text[$from..$to).
function Find-ObjectSpan([string]$text, [int]$from, [int]$to, [string]$key) {
    $m = [regex]::Match($text.Substring($from, $to - $from), '"' + [regex]::Escape($key) + '"\s*:\s*\{')
    if (-not $m.Success) { throw "key `"$key`" not found in the config file" }
    $open = $from + $m.Index + $m.Length - 1
    return @($open, (Find-BlockEnd $text $open))
}

# Span of Importers.EFMI.Importer.
function Get-ImporterSpan([string]$text) {
    $s = Find-ObjectSpan $text 0 $text.Length 'Importers'
    $s = Find-ObjectSpan $text $s[0] $s[1] 'EFMI'
    return (Find-ObjectSpan $text $s[0] $s[1] 'Importer')
}

# Replace the scalar value of "$key" inside the Importer object; the key must occur exactly once there.
function Set-ImporterValue([string]$text, [string]$key, [string]$newLiteral) {
    $span = Get-ImporterSpan $text
    $seg = $text.Substring($span[0], $span[1] - $span[0])
    $ms = [regex]::Matches($seg, '("' + [regex]::Escape($key) + '"\s*:\s*)("(?:[^"\\]|\\.)*"|true|false|null|-?[0-9.]+)')
    if ($ms.Count -ne 1) { throw "key `"$key`" occurs $($ms.Count) times in the EFMI importer (launcher version not supported?)" }
    $g = $ms[0].Groups[2]
    $start = $span[0] + $g.Index
    return $text.Substring(0, $start) + $newLiteral + $text.Substring($start + $g.Length)
}

# JSON string literal the way Python's json.dumps writes it (non-ASCII as \uXXXX).
function ConvertTo-JsonString([string]$s) {
    $sb = New-Object Text.StringBuilder
    [void]$sb.Append('"')
    foreach ($ch in $s.ToCharArray()) {
        $code = [int]$ch
        if ($ch -eq '"') { [void]$sb.Append('\"') }
        elseif ($ch -eq '\') { [void]$sb.Append('\\') }
        elseif ($ch -eq "`n") { [void]$sb.Append('\n') }
        elseif ($ch -eq "`r") { [void]$sb.Append('\r') }
        elseif ($ch -eq "`t") { [void]$sb.Append('\t') }
        elseif ($code -lt 0x20 -or $code -gt 0x7E) { [void]$sb.Append('\u' + $code.ToString('x4')) }
        else { [void]$sb.Append($ch) }
    }
    [void]$sb.Append('"')
    return $sb.ToString()
}

# ---------- EFF proxy file in the game folder ----------

# The rename needed for $target: for Vulkan the EFF proxy is parked (vulkan-1.dll.eff-parked),
# for DirectX 11 it is put back. Returns a list with at most one @{ From; To; Kind }.
function Get-EffPlan([string]$target, [string]$game, [hashtable]$settings, [ref]$notes) {
    $plan = @()
    if ($settings.eff_park -ne '1') { return $plan }
    if (-not $game -or -not (Test-Path $game -PathType Container)) { $notes.Value += 'The game folder is unknown (game_folder is empty in the launcher config); the EFF proxy is left alone.'; return $plan }
    $effInstalled = Test-Path (Join-Path $game $EffMarker) -PathType Leaf
    $orig = Join-Path $game $EffProxy
    $parked = $orig + $ParkedSuffix
    $hasOrig = Test-Path $orig -PathType Leaf
    $hasParked = Test-Path $parked -PathType Leaf
    if ($target -eq 'vulkan') {
        if (-not $hasOrig) { return $plan }
        if (-not $effInstalled) { $notes.Value += "The game folder holds $EffProxy but no $EffMarker; it does not look like EFF's proxy and is left alone."; return $plan }
        if ($hasParked) { $notes.Value += "Both $EffProxy and $EffProxy$ParkedSuffix exist (EFF reinstalled?); sort that out by hand before switching."; return $plan }
        $plan += @{ From = $orig; To = $parked; Kind = 'park' }
    } else {
        if (-not $hasParked) { return $plan }
        if ($hasOrig) { $notes.Value += "$EffProxy$ParkedSuffix is still there but $EffProxy is back (EFF reinstalled?); both are left alone."; return $plan }
        $plan += @{ From = $parked; To = $orig; Kind = 'restore' }
    }
    return $plan
}

# ---------- main ----------

Info '==== EFMI: switch between DirectX 11 and Vulkan ===='
$settings = Read-Settings $SettingsFile

# 1. EFMI folder: this script is inside it (EFMI\EFMI-Vulkan, as the release is unpacked)
$efmi = Find-EfmiRoot $ScriptDir
if (-not $efmi) { Fail "This script is not inside an EFMI folder. Unpack the release into the EFMI folder of your XXMI Launcher (the one that holds Mods and Core), so that this script sits in EFMI\EFMI-Vulkan.`nCurrent place: $ScriptDir" }

# 2. launcher folder: the parent of EFMI, or wherever the registry, its shortcuts or its process say, or the user
$lroot = $null
$launcherSource = ''
if ($LauncherRoot) { $lroot = [IO.Path]::GetFullPath($LauncherRoot) }
elseif ($ConfigPath) { $lroot = Split-Path -Parent ([IO.Path]::GetFullPath($ConfigPath)) }
else {
    $found = Find-Launcher $efmi $settings
    if ($found) { $lroot = $found.Root; $launcherSource = $found.Source }
    else {
        Info ''
        Warn "No XXMI Launcher managing $efmi was found (looked at the EFMI folder's parent, the registry, the launcher's shortcuts and a running launcher; its `"$ConfigFileName`" must name this EFMI folder)."
        Info 'Drop "XXMI Launcher Config.json" (or the launcher folder, or XXMI Launcher.exe) into this window and press Enter; Enter alone quits.'
        $answer = ''
        try { $answer = Read-Host '>' } catch {}
        if (-not $answer -or -not $answer.Trim()) { Fail "No XXMI Launcher found. Put launcher_root = <folder holding `"$ConfigFileName`"> into switch_renderer.cfg next to this script." }
        $r = Resolve-LauncherRoot $answer
        if (-not $r) { Fail "No `"$ConfigFileName`" there: $answer" }
        if (-not (Test-LauncherFor $r $efmi)) { Fail "That launcher's config names another EFMI folder, not $efmi. Unpack the release into the EFMI folder the launcher actually uses." }
        $lroot = $r
        Save-Setting $SettingsFile 'launcher_root' $lroot
    }
}
$cfgFile = $(if ($ConfigPath) { [IO.Path]::GetFullPath($ConfigPath) } else { Join-Path $lroot $ConfigFileName })
if (-not (Test-Path $cfgFile -PathType Leaf)) { Fail "Launcher config not found: $cfgFile" }
$keyFile = Join-Path $lroot $KeyFileRelPath

# 3. current values
$text = [IO.File]::ReadAllText($cfgFile, [Text.Encoding]::UTF8)
try { $json = $text | ConvertFrom-Json } catch { Fail "The config file is not valid JSON: $cfgFile" }
$imp = $json.Importers.EFMI.Importer
if (-not $imp) { Fail 'The config has no EFMI importer. Install EFMI in the XXMI Launcher first.' }
foreach ($need in 'xxmi_dll_inject_mode', 'd3d11_mode_cmd_args', 'extra_libraries_enabled', 'extra_libraries', 'extra_libraries_signature') {
    if (-not $imp.PSObject.Properties[$need]) { Fail "The config has no `"$need`" entry; this XXMI Launcher version is too old for this script." }
}
$cfgEfmi = Get-ConfigEfmiFolder $json $lroot
if ((ConvertTo-KeyPath $cfgEfmi) -ne (ConvertTo-KeyPath $efmi)) { Fail "This launcher's config names the EFMI folder $cfgEfmi, not $efmi where this script is.`nUnpack the release into the EFMI folder the launcher actually uses, or set launcher_root in switch_renderer.cfg." }

Info "EFMI folder:     $efmi"
Info "Launcher config: $cfgFile$(if ($launcherSource) { " (from $launcherSource)" })"

$curMode  = [string]$imp.xxmi_dll_inject_mode
$curArgs  = [string]$imp.d3d11_mode_cmd_args
$curLibsOn = [bool]$imp.extra_libraries_enabled
$curLibs  = @(([string]$imp.extra_libraries) -split "`n" | ForEach-Object { $_.Trim() } | Where-Object { $_ })
$curSig   = [string]$imp.extra_libraries_signature
$hasVk    = @($curLibs | Where-Object { [IO.Path]::GetFileName($_) -ieq $VulkanDllName }).Count -gt 0
$game     = $(if ($GameFolder) { [IO.Path]::GetFullPath($GameFolder) } else { ([string]$imp.game_folder).Replace('/', '\') })

if ($curMode -ieq 'SKIP' -and $hasVk -and $curLibsOn) { $current = 'vulkan' }
elseif ($curMode -ine 'SKIP' -and -not ($hasVk -and $curLibsOn)) { $current = 'dx11' }
else { $current = 'mixed' }

Info ''
Info 'Now:'
Info "  XXMI DLL injection mode = $curMode"
Info "  Game arguments          = `"$curArgs`""
Info "  Inject Libraries        = $(if ($curLibsOn) { 'on' } else { 'off' })"
foreach ($l in $curLibs) { Info "      $l" }
if ($game -and (Test-Path $game -PathType Container)) {
    $effInstalled = Test-Path (Join-Path $game $EffMarker) -PathType Leaf
    $effState = ''
    if (Test-Path (Join-Path $game $EffProxy) -PathType Leaf) { $effState = ", proxy $EffProxy in place" }
    elseif (Test-Path (Join-Path $game ($EffProxy + $ParkedSuffix)) -PathType Leaf) { $effState = ", proxy $EffProxy parked" }
    Info "  EFF                     = $(if ($effInstalled) { 'installed' } else { 'not installed' })$effState"
}
switch ($current) {
    'vulkan' { Info '  => Vulkan (EFMI-Vulkan layer)' }
    'dx11'   { Info '  => DirectX 11 (3DMigoto)' }
    default  { Warn '  => mixed (the injection mode and Inject Libraries do not agree); run with -Mode dx11 or -Mode vulkan.' }
}

if ($Mode -eq 'status') { exit 0 }
if ($Mode -eq 'toggle') {
    if ($current -eq 'mixed') { Fail 'No automatic toggle from a mixed state.' }
    $target = $(if ($current -eq 'dx11') { 'vulkan' } else { 'dx11' })
} else { $target = $Mode }

$configChange = ($target -ne $current)
$effNotes = @()
$effPlan = @(Get-EffPlan $target $game $settings ([ref]$effNotes))
foreach ($n in $effNotes) { Warn $n }
if (-not $configChange -and $effPlan.Count -eq 0) { Info ''; Info "Already $target; nothing to change."; exit 0 }

# 4. nothing may be running: the launcher rewrites its config when it closes, the game holds its DLLs
$running = @(Get-Process -Name 'XXMI Launcher', 'Endfield' -ErrorAction SilentlyContinue)
if ($running.Count -gt 0 -and -not $DryRun -and -not $SkipRunningCheck) {
    Fail "Close first: $(($running | ForEach-Object { $_.ProcessName }) -join ', '). The launcher writes its config back when it closes, which would undo this change, and a running game keeps its DLLs locked."
}

# 5. new values
$stateFile = Join-Path $ScriptDir 'switch_renderer.state'
$parked = @(); $restored = $false
if ($configChange) {
    if ($target -eq 'vulkan') {
        # efmi_vk_inject.dll: next to this script (the release folder), else EFMI\EFMI-Vulkan\.
        $dll = $null
        foreach ($t in @($VulkanDll, (Join-Path $ScriptDir $VulkanDllName), (Join-Path $efmi "EFMI-Vulkan\$VulkanDllName"))) {
            if ($t -and (Test-Path $t -PathType Leaf)) { $dll = [IO.Path]::GetFullPath($t); break }
        }
        if (-not $dll) { Fail "$VulkanDllName not found. Unpack the release into the EFMI folder, so that EFMI\EFMI-Vulkan\$VulkanDllName exists." }
        if (-not (Test-Path (Join-Path (Split-Path -Parent $dll) 'efmi_vk.cfg'))) { Warn "No efmi_vk.cfg next to $dll; this does not look like a complete release." }
        # Relative to the launcher folder when it lies inside it (the launcher resolves relative entries there).
        $rootKey = (ConvertTo-KeyPath $lroot) + '\'
        $dllEntry = $(if ((ConvertTo-KeyPath $dll).StartsWith($rootKey)) { $dll.Substring($rootKey.Length) } else { $dll })
        $others = @($curLibs | Where-Object { [IO.Path]::GetFileName($_) -ine $VulkanDllName })
        $newMode = 'SKIP'; $newArgs = ''; $newLibsOn = $true
        if ($settings.vulkan_keep_other_libraries -eq '1') {
            $newLibs = @($dllEntry) + $others
            if ($others.Count -gt 0) { Warn "Other Inject Libraries stay, as switch_renderer.cfg says: $($others -join '; '). Whether they work or conflict under Vulkan is not known to this script." }
        } else {
            # Libraries set up for the DirectX 11 path are parked in the state file and come back with DirectX 11.
            $newLibs = @($dllEntry)
            if ($others.Count -gt 0) { $parked = $others; Warn "Other Inject Libraries are set aside under Vulkan and come back with DirectX 11 (kept in switch_renderer.state): $($others -join '; '). Set vulkan_keep_other_libraries = 1 in switch_renderer.cfg to inject them too." }
        }
    } else {
        $newLibs = @($curLibs | Where-Object { [IO.Path]::GetFileName($_) -ine $VulkanDllName })
        if (Test-Path $stateFile -PathType Leaf) {
            $have = @{}
            foreach ($l in $newLibs) { $have[(ConvertTo-KeyPath $l)] = $true }
            foreach ($l in [IO.File]::ReadAllLines($stateFile, [Text.Encoding]::UTF8)) {
                $l = $l.Trim()
                if ($l -and -not $have.ContainsKey((ConvertTo-KeyPath $l))) { $newLibs += $l; $have[(ConvertTo-KeyPath $l)] = $true }
            }
            $restored = $true
            Info 'Restoring the Inject Libraries set aside for Vulkan (switch_renderer.state).'
        }
        $newMode = $settings.dx11_inject_mode; $newArgs = $settings.dx11_cmd_args
        $newLibsOn = $(if ($newLibs.Count -gt 0) { $curLibsOn } else { $false })
        if (@('DIRECT', 'HOOK') -notcontains $newMode.ToUpperInvariant()) { Fail "dx11_inject_mode in switch_renderer.cfg must be DIRECT or HOOK, not `"$newMode`"." }
        $newMode = $newMode.ToUpperInvariant()
    }
    $newLibsText = $newLibs -join "`n"

    # 6. signature
    $newSig = ''
    if ($newLibsText) {
        if (-not (Test-Path $keyFile -PathType Leaf)) { Fail "The launcher's signing key is missing: $keyFile. Open the XXMI Launcher once and close it; it creates the key." }
        try { $ecdsa = Import-LauncherKey $keyFile } catch { Fail "Cannot read the launcher's signing key: $($_.Exception.Message)" }
        if (-not (Test-LauncherSignature $ecdsa $env:USERNAME ([string]$json.Security.user_signature))) {
            Warn "The launcher's key pair does not match user_signature in its config: the launcher will make a new key pair at its next start and show its Reset/Keep dialog; choose Keep."
        } elseif ($curLibsText = ($curLibs -join "`n")) {
            if (-not (Test-LauncherSignature $ecdsa $curLibsText $curSig)) { Warn 'The present Inject Libraries signature was already invalid (the launcher would have asked before); it is signed afresh now.' }
        }
        $newSig = New-LauncherSignature $ecdsa $newLibsText
    }
}

Info ''
Info "Switching to $(if ($target -eq 'vulkan') { 'Vulkan' } else { 'DirectX 11' }):"
if ($configChange) {
    Info "  XXMI DLL injection mode = $newMode"
    Info "  Game arguments          = `"$newArgs`""
    Info "  Inject Libraries        = $(if ($newLibsOn) { 'on' } else { 'off' })"
    foreach ($l in $newLibs) { Info "      $l" }
} else {
    Info '  The launcher config is already there; unchanged.'
}
foreach ($r in $effPlan) {
    if ($r.Kind -eq 'park') { Info "  EFF proxy set aside     = $($r.From)  ->  $([IO.Path]::GetFileName($r.To))" }
    else { Info "  EFF proxy restored      = $($r.From)  ->  $([IO.Path]::GetFileName($r.To))" }
}

if ($DryRun) { Info ''; Info '(dry run: nothing written, nothing renamed)'; exit 0 }

# 7. write: only the five values change, everything else stays byte for byte
if ($configChange) {
    $new = $text
    $new = Set-ImporterValue $new 'xxmi_dll_inject_mode'     (ConvertTo-JsonString $newMode)
    $new = Set-ImporterValue $new 'd3d11_mode_cmd_args'      (ConvertTo-JsonString $newArgs)
    $new = Set-ImporterValue $new 'extra_libraries_enabled'  $(if ($newLibsOn) { 'true' } else { 'false' })
    $new = Set-ImporterValue $new 'extra_libraries'          (ConvertTo-JsonString $newLibsText)
    $new = Set-ImporterValue $new 'extra_libraries_signature' (ConvertTo-JsonString $newSig)

    try { $check = $new | ConvertFrom-Json } catch { Fail 'The edited text is not valid JSON; nothing written.' }
    $ci = $check.Importers.EFMI.Importer
    if ([string]$ci.xxmi_dll_inject_mode -ne $newMode -or [string]$ci.d3d11_mode_cmd_args -ne $newArgs -or
        [string]$ci.extra_libraries -ne $newLibsText -or [bool]$ci.extra_libraries_enabled -ne $newLibsOn -or
        [string]$ci.extra_libraries_signature -ne $newSig) { Fail 'The edited text reads back differently from what was meant; nothing written.' }

    if (-not $BackupDir) { $BackupDir = Join-Path $ScriptDir 'backups' }
    if (-not (Test-Path $BackupDir)) { New-Item -ItemType Directory -Path $BackupDir | Out-Null }
    $stamp = Get-Date -Format 'yyyyMMdd-HHmmss-fff'
    $backup = Join-Path $BackupDir "XXMI Launcher Config.$stamp.json"
    try { Copy-Item -LiteralPath $cfgFile -Destination $backup -ErrorAction Stop } catch { Fail "Backup failed, nothing written: $backup`n$($_.Exception.Message)" }
    Get-ChildItem -Path $BackupDir -Filter 'XXMI Launcher Config.*.json' | Sort-Object LastWriteTime -Descending | Select-Object -Skip 10 | Remove-Item -ErrorAction SilentlyContinue

    try { [IO.File]::WriteAllText($cfgFile, $new, $Utf8NoBom) } catch { Fail "Cannot write the config (no permission? a launcher under Program Files needs the .bat run as administrator): $cfgFile`n$($_.Exception.Message)" }
    if ($parked.Count -gt 0) { [IO.File]::WriteAllLines($stateFile, [string[]]$parked, $Utf8NoBom) }
    if ($restored) { Remove-Item -LiteralPath $stateFile -ErrorAction SilentlyContinue }
    Info ''
    Info "Written. Backup: $backup"
}

# 8. EFF proxy: after the config, so a failed rename never leaves a half-switched launcher
foreach ($r in $effPlan) {
    try {
        Rename-Item -LiteralPath $r.From -NewName ([IO.Path]::GetFileName($r.To)) -ErrorAction Stop
        Info "Renamed: $($r.From)  ->  $([IO.Path]::GetFileName($r.To))"
    } catch {
        Warn "Rename failed: $($r.From): $($_.Exception.Message)"
        if ($target -eq 'vulkan') { Warn "EFF's proxy $EffProxy is still in place; EFMI-Vulkan will hook nothing and no Mod will show. Rename it by hand (add the suffix $ParkedSuffix) or run the .bat as administrator." }
        else { Warn "EFF's proxy was not restored; EFF will not load under DirectX 11. Remove the $ParkedSuffix suffix by hand or run the .bat as administrator." }
    }
}

Info ''
if ($target -eq 'vulkan') {
    Info 'Next: start the game from the XXMI Launcher. It starts with Vulkan and loads EFMI-Vulkan; the log is EFMI-Vulkan\efmi_vk_layer.log.'
} else {
    Info 'Next: start the game from the XXMI Launcher. It starts with DirectX 11 and 3DMigoto.'
}
exit 0
