# install_to_sd.ps1 - copy the hardware NRO (+ a default config) onto the
# Switch SD card and verify it.  Run it after plugging the SD into this PC.
#
#   .\tools\install_to_sd.ps1                    # just look for a card and report
#   .\tools\install_to_sd.ps1 -Apply             # copy to the auto-detected card
#   .\tools\install_to_sd.ps1 -Drive E: -Apply   # explicit drive letter
#   .\tools\install_to_sd.ps1 -Apply -Server 192.168.1.50 -User Administrator -Pass secret
param(
    [string]$Drive,
    [string]$Nro,
    [switch]$Apply,
    [string]$Server = "",
    [string]$Port = "3389",
    [string]$User = "Administrator",
    [string]$Pass = ""
)

$ErrorActionPreference = "Stop"
$proj = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)
if (-not $Nro) { $Nro = Join-Path $proj "dist\switch_rdp_b23_menu_hw.nro" }
if (-not (Test-Path $Nro)) { throw "missing NRO: $Nro" }

function Test-SdRoot([string]$root) {
    if (-not (Test-Path $root)) { return $false }
    foreach ($m in @("Nintendo", "atmosphere", "hbmenu.nro", "switch")) {
        if (Test-Path (Join-Path $root $m)) { return $true }
    }
    return $false
}

# --- find the card ------------------------------------------------------
if (-not $Drive) {
    $cands = @()
    foreach ($v in (Get-Volume | Where-Object { $_.DriveLetter -and $_.DriveType -ne "CD-ROM" })) {
        $root = "$($v.DriveLetter):\"
        if (Test-SdRoot $root) { $cands += $root }
    }
    if ($cands.Count -eq 0) {
        Write-Host "no SD card found.  Plug the SD in (or mount it) and run again."
        Write-Host "volumes seen:"
        Get-Volume | Where-Object { $_.DriveLetter } |
            Select-Object DriveLetter, FileSystemLabel, FileSystem, DriveType |
            Format-Table -AutoSize | Out-String | Write-Host
        exit 1
    }
    $Drive = $cands[0]
}
if (-not $Drive.EndsWith("\")) { $Drive += "\" }
if (-not (Test-SdRoot $Drive)) { throw "$Drive does not look like a Switch SD card" }

# --- what is there now --------------------------------------------------
$swDir = Join-Path $Drive "switch"
Write-Host "SD root   : $Drive"
Write-Host "hbmenu.nro: $(if (Test-Path (Join-Path $Drive 'hbmenu.nro')) { 'yes' } else { 'MISSING (album needs it to open hbmenu)' })"
Write-Host "switch dir: $(if (Test-Path $swDir) { 'yes' } else { 'missing' })"
if (Test-Path $swDir) {
    Get-ChildItem $swDir -Filter *.nro | Select-Object Name, Length, LastWriteTime |
        Format-Table -AutoSize | Out-String | Write-Host
}

$srcLen = (Get-Item $Nro).Length
Write-Host ("payload   : {0}  ({1} bytes)" -f (Split-Path $Nro -Leaf), $srcLen)

if (-not $Apply) {
    Write-Host ""
    Write-Host "dry run.  Re-run with -Apply to copy it to $swDir\switch_rdp.nro"
    exit 0
}

# --- copy ---------------------------------------------------------------
if (-not (Test-Path $swDir)) { New-Item -ItemType Directory -Path $swDir | Out-Null }
$dst = Join-Path $swDir "switch_rdp.nro"
Copy-Item $Nro $dst -Force
$dstLen = (Get-Item $dst).Length
if ($dstLen -ne $srcLen) { throw "copy incomplete: $dstLen != $srcLen" }
Write-Host "copied    : $dst ($dstLen bytes)"

# config: only created when absent, never overwritten
$cfgPath = Join-Path $Drive "switch_rdp.cfg"
if (Test-Path $cfgPath) {
    Write-Host "cfg kept  : $cfgPath (already exists)"
} else {
    $cfg = @"
# switch_rdp connection settings (created by install_to_sd.ps1)
# In the app press  UP UP DOWN DOWN LEFT RIGHT LEFT RIGHT B A  to connect.
server = $Server
port = $Port
user = $User
pass = $Pass
"@
    [System.IO.File]::WriteAllText($cfgPath, $cfg, (New-Object System.Text.UTF8Encoding $false))
    Write-Host "cfg wrote : $cfgPath"
}

# --- verify the copied file is a sane NRO -------------------------------
$fs = [System.IO.File]::OpenRead($dst)
try {
    $hdr = New-Object byte[] 0x40
    [void]$fs.Read($hdr, 0, 0x40)
} finally { $fs.Dispose() }
$magic = [System.Text.Encoding]::ASCII.GetString($hdr, 0x10, 4)
$size = [BitConverter]::ToUInt32($hdr, 0x18)
$brand = [System.Text.Encoding]::ASCII.GetString($hdr, 8, 8)
Write-Host ("verify    : magic={0} brand={1} header.size=0x{2:X} file={3} trailing={4}" -f
            $magic, $brand, $size, $dstLen, ($dstLen - $size))
if ($magic -ne "NRO0") { throw "copied file is not an NRO" }
Write-Host ""
Write-Host "On the console: album -> hold R while starting it -> hbmenu -> switch_rdp"
