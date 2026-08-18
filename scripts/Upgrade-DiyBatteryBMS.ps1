<#
.SYNOPSIS
  Install or upgrade a DIY Battery BMS board over USB, keeping its settings.

.DESCRIPTION
  Works out what is on the board and does the right thing:

    Blank or running something else -> a fresh install. Writes the firmware,
      points the bootloader at it, and clears the settings area so the first
      boot starts clean and comes up as an access point.
    This project on 2.x's layout -> the migration. Replaces the partition
      table, keeps NVS where it is, and resets the boot slot.
    This project on the 3.x layout -> an ordinary update.

  For anything with settings on it, it reads the configuration out of NVS
  first, then reads it back afterwards and proves every value survived - the
  step a plain esptool command line cannot do for you.

  Why this exists. 3.0 changed the partition table, so upgrading from 2.x needs a
  serial flash of bootloader + partition table + firmware rather than an OTA. The
  settings survive that only because the nvs partition sits at the same offset
  and size in both tables. That is an assumption, not a guarantee: if a future
  release ever moves or resizes nvs, a flash that looks completely normal wipes
  every configured value. This script compares the two tables and refuses to
  flash when that is about to happen, which is the whole reason to use it instead
  of the example command in the release README.

  It never passes erase_flash, and it always takes a byte-level NVS backup first,
  so -Restore can put the old settings back if anything goes wrong.

.PARAMETER ReleaseDir
  Folder holding firmware.bin, bootloader.bin and partitions.bin - an unzipped
  release, or a PlatformIO build directory such as .pio\build\esp32-ESPCAN.

.PARAMETER Port
  Serial port, e.g. COM12. Auto-detected when a single candidate is present.

.PARAMETER Chip
  esp32, esp32s3 or esp32c3. Auto-detected from the board when omitted.

.PARAMETER BackupDir
  Where to write the NVS backup and config inventories.
  Default: .\upgrade-backups\<yyyyMMdd-HHmmss>

.PARAMETER PullOnly
  Read and report the configuration, then stop. Touches nothing.

.PARAMETER Restore
  Path to an nvs-before.bin from an earlier run; writes it back and exits.

.PARAMETER ExportProfile
  Read this board's settings and write a clone kit - the NVS image, a readable
  inventory, and a note listing the values that must not be shared with a second
  board. Use with -CloneFrom to set up another device without configuring it by
  hand.

.PARAMETER CloneFrom
  Write a clone kit's NVS image onto this board, so it comes up already
  configured. Prints the short list of fields to make unique afterwards.

.PARAMETER IncludeSystemKeys
  Also compare the IDF-managed namespaces (nvs.net80211, phy, dhcp_state).
  These legitimately change across a reboot, so they are reported but not
  compared unless asked for.

.PARAMETER Force
  Continue even when the nvs partition has moved or resized. Understand that
  this wipes the settings before using it.

.EXAMPLE
  .\Upgrade-DiyBatteryBMS.ps1 -ReleaseDir .\dist\unzipped -Port COM12

.EXAMPLE
  .\Upgrade-DiyBatteryBMS.ps1 -PullOnly
  Dump the current configuration without flashing anything.
#>
[CmdletBinding()]
param(
  [string] $ReleaseDir,
  [string] $Port,
  [ValidateSet('esp32', 'esp32s3', 'esp32c3')]
  [string] $Chip,
  [string] $BackupDir,
  [switch] $PullOnly,
  [string] $Restore,
  [string] $ExportProfile,
  [string] $CloneFrom,
  [switch] $IncludeSystemKeys,
  [switch] $Force
)

$ErrorActionPreference = 'Stop'

<# Anything that stops the script arrives here. Without this a refusal to flash
   prints a PowerShell stack trace over the explanation, which buries the one
   line the person running it needs to read. #>
trap {
  Write-Host ''
  Write-Host ("   STOPPED: {0}" -f $_.Exception.Message) -ForegroundColor Red
  Write-Host ''
  exit 1
}

# The app's own namespaces. Everything else in nvs belongs to the IDF - WiFi
# calibration, PHY data, the DHCP lease - and is expected to differ after a
# reboot, so comparing it would report noise as failure.
$script:AppNamespaces = @('smartbms', 'network')

function Write-Step   { param([string]$m) Write-Host "`n== $m" -ForegroundColor Cyan }
function Write-Ok     { param([string]$m) Write-Host "   $m" -ForegroundColor Green }
function Write-Warn   { param([string]$m) Write-Host "   $m" -ForegroundColor Yellow }
function Write-Err    { param([string]$m) Write-Host "   $m" -ForegroundColor Red }
function Write-Detail { param([string]$m) Write-Host "   $m" -ForegroundColor DarkGray }

# ---------------------------------------------------------------- esptool ----

function Resolve-EspTool {
  <# esptool ships with PlatformIO as a Python module. Prefer PlatformIO's own
     interpreter so the version matches the one that built the release, and fall
     back to anything on PATH for a machine without PlatformIO. #>
  $pioPython = Join-Path $env:USERPROFILE '.platformio\penv\Scripts\python.exe'
  if (Test-Path $pioPython) {
    return [pscustomobject]@{ Exe = $pioPython; Prefix = @('-m', 'esptool') }
  }
  foreach ($name in @('esptool.py', 'esptool')) {
    $cmd = Get-Command $name -ErrorAction SilentlyContinue
    if ($cmd) { return [pscustomobject]@{ Exe = $cmd.Source; Prefix = @() } }
  }
  foreach ($name in @('python', 'python3', 'py')) {
    $cmd = Get-Command $name -ErrorAction SilentlyContinue
    if ($cmd) {
      & $cmd.Source -m esptool version | Out-Null
      if ($LASTEXITCODE -eq 0) {
        return [pscustomobject]@{ Exe = $cmd.Source; Prefix = @('-m', 'esptool') }
      }
    }
  }
  throw 'esptool not found. Install PlatformIO, or "pip install esptool".'
}

function Get-EspToolVerbs {
  <# esptool 5 renamed every subcommand from read_flash to read-flash. Ask the
     tool which era it is from rather than guessing and failing on the first
     call. #>
  param($Tool)
  $out = & $Tool.Exe @($Tool.Prefix + @('version'))
  $text = ($out | Out-String)
  $major = 4
  $m = [regex]::Match($text, 'v?(\d+)\.\d+')
  if ($m.Success) { $major = [int]$m.Groups[1].Value }
  if ($major -ge 5) {
    return [pscustomobject]@{ Read = 'read-flash'; Write = 'write-flash'; Id = 'flash-id'; Version = $text.Trim() }
  }
  return [pscustomobject]@{ Read = 'read_flash'; Write = 'write_flash'; Id = 'flash_id'; Version = $text.Trim() }
}

function Invoke-EspTool {
  param($Tool, [string[]]$Arguments, [switch]$Quiet)
  $all = $Tool.Prefix + $Arguments
  if ($Quiet) { $out = & $Tool.Exe @all | Out-String }
  else        { $out = & $Tool.Exe @all | Tee-Object -Variable teed | Out-String }
  if ($LASTEXITCODE -ne 0) {
    throw ("esptool failed (exit {0}): {1}" -f $LASTEXITCODE, ($Arguments -join ' '))
  }
  return $out
}

function Find-DevicePort {
  $ports = @()
  try { $ports = @(Get-CimInstance Win32_SerialPort | Select-Object -ExpandProperty DeviceID) } catch { }
  if ($ports.Count -eq 0) { $ports = @([System.IO.Ports.SerialPort]::GetPortNames()) }
  $ports = @($ports | Sort-Object -Unique)
  if ($ports.Count -eq 0) { throw 'No serial ports found. Is the board plugged in?' }
  if ($ports.Count -gt 1) {
    throw ("Several serial ports present ({0}). Pick one with -Port." -f ($ports -join ', '))
  }
  return $ports[0]
}

# ------------------------------------------------------- partition tables ----

function Read-PartitionTable {
  <# 32 bytes per entry, 0xAA 0x50 magic, terminated by anything else. #>
  param([byte[]] $Bytes)
  $out = @()
  for ($i = 0; ($i + 32) -le $Bytes.Length; $i += 32) {
    if ($Bytes[$i] -ne 0xAA -or $Bytes[$i + 1] -ne 0x50) { break }
    $nameBytes = $Bytes[($i + 12)..($i + 27)]
    $nul = [Array]::IndexOf($nameBytes, [byte]0)
    if ($nul -lt 0) { $nul = $nameBytes.Length }
    $out += [pscustomobject]@{
      Name    = [Text.Encoding]::ASCII.GetString($nameBytes, 0, $nul)
      Type    = $Bytes[$i + 2]
      SubType = $Bytes[$i + 3]
      Offset  = [BitConverter]::ToUInt32($Bytes, $i + 4)
      Size    = [BitConverter]::ToUInt32($Bytes, $i + 8)
    }
  }
  return $out
}

function Format-Partition {
  param($P)
  return ("{0,-9} type={1} sub=0x{2:x2} {3,10} + {4,9} -> {5}" -f `
    $P.Name, $P.Type, $P.SubType, ('0x{0:x}' -f $P.Offset), ('0x{0:x}' -f $P.Size),
    ('0x{0:x}' -f ($P.Offset + $P.Size)))
}

# ------------------------------------------------------------------- NVS ----

function Read-NvsEntries {
  <# Walks the NVS pages and returns one object per live entry.

     Format: 4096-byte pages; 32-byte header (state, seq, version); a 32-byte
     entry-state bitmap holding 2 bits for each of 126 entries (0b11 empty,
     0b10 written, 0b00 erased); then the entries themselves, 32 bytes each -
     namespace index, type, span, chunk index, CRC, a 16-byte key, and 8 bytes
     that are either the value or, for variable-length types, its size.

     A namespace declaration is an entry with namespace index 0 and type u8,
     where the key is the namespace name and the value its index. #>
  param([byte[]] $Bytes)

  $types = @{
    0x01 = 'u8'; 0x11 = 'i8'; 0x02 = 'u16'; 0x12 = 'i16';
    0x04 = 'u32'; 0x14 = 'i32'; 0x08 = 'u64'; 0x18 = 'i64';
    0x21 = 'str'; 0x41 = 'blob'; 0x42 = 'blobdata'; 0x48 = 'blobidx'
  }
  $widths = @{ 'u8' = 1; 'i8' = 1; 'u16' = 2; 'i16' = 2; 'u32' = 4; 'i32' = 4; 'u64' = 8; 'i64' = 8 }

  $namespaces = @{}
  $entries = @()
  $pageCount = [math]::Floor($Bytes.Length / 4096)

  for ($p = 0; $p -lt $pageCount; $p++) {
    $base = $p * 4096
    $state = [BitConverter]::ToUInt32($Bytes, $base)
    if ($state -eq 0xFFFFFFFF) { continue }        # never written

    $i = 0
    while ($i -lt 126) {
      $bmByte = $Bytes[$base + 32 + [math]::Floor($i / 4)]
      $st = ($bmByte -shr (($i % 4) * 2)) -band 0x3
      if ($st -ne 0x2) { $i++; continue }          # only "written" entries count

      $e = $base + 64 + ($i * 32)
      $ns = $Bytes[$e]
      $rawType = $Bytes[$e + 1]
      $span = $Bytes[$e + 2]
      if ($span -lt 1) { $span = 1 }

      $keyBytes = $Bytes[($e + 8)..($e + 23)]
      $nul = [Array]::IndexOf($keyBytes, [byte]0)
      if ($nul -lt 0) { $nul = $keyBytes.Length }
      $key = [Text.Encoding]::ASCII.GetString($keyBytes, 0, $nul)

      $typeName = 'unknown-0x{0:x2}' -f $rawType
      if ($types.ContainsKey([int]$rawType)) { $typeName = $types[[int]$rawType] }

      if ($ns -eq 0 -and $typeName -eq 'u8') {
        $namespaces[[int]$Bytes[$e + 24]] = $key      # namespace declaration
        $i++
        continue
      }

      $value = $null
      $isText = $false
      $validUtf8 = $true
      if ($typeName -eq 'str') {
        $size = [BitConverter]::ToUInt16($Bytes, $e + 24)
        $dataStart = $base + 64 + (($i + 1) * 32)
        if ($size -gt 0 -and ($dataStart + $size) -le $Bytes.Length) {
          $raw = $Bytes[$dataStart..($dataStart + $size - 1)]
          $z = [Array]::IndexOf($raw, [byte]0)
          if ($z -lt 0) { $z = $raw.Length }
          $strict = [Text.UTF8Encoding]::new($false, $true)
          try   { $value = $strict.GetString($raw, 0, $z) }
          catch { $validUtf8 = $false
                  $value = '0x' + (($raw[0..($z - 1)] | ForEach-Object { $_.ToString('x2') }) -join '') }
          $isText = $true
        }
        else { $value = ''; $isText = $true }
      }
      elseif ($widths.ContainsKey($typeName)) {
        # The value union is 8 bytes however narrow the type is: mask to the
        # type's own width, and sign-extend the signed ones.
        $raw64 = [BitConverter]::ToUInt64($Bytes, $e + 24)
        $w = $widths[$typeName]
        if ($w -lt 8) {
          $mask = ([uint64]1 -shl ($w * 8)) - 1
          $v = $raw64 -band $mask
          if ($typeName.StartsWith('i') -and $v -ge ([uint64]1 -shl ($w * 8 - 1))) {
            $value = [int64]$v - [int64]([uint64]1 -shl ($w * 8))
          }
          else { $value = $v }
        }
        else { $value = $raw64 }
      }
      else {
        $value = '(binary)'
      }

      $entries += [pscustomobject]@{
        NamespaceIndex = $ns
        Key            = $key
        Type           = $typeName
        Value          = $value
        IsText         = $isText
        ValidUtf8      = $validUtf8
      }
      $i += $span
    }
  }

  foreach ($en in $entries) {
    $name = "ns$($en.NamespaceIndex)"
    if ($namespaces.ContainsKey([int]$en.NamespaceIndex)) { $name = $namespaces[[int]$en.NamespaceIndex] }
    $en | Add-Member -NotePropertyName Namespace -NotePropertyValue $name -Force
    $en | Add-Member -NotePropertyName Id -NotePropertyValue ("{0}/{1}" -f $name, $en.Key) -Force
  }
  return $entries
}

function Show-Config {
  param($Entries, [switch]$All)
  $shown = $Entries
  if (-not $All) { $shown = $Entries | Where-Object { $script:AppNamespaces -contains $_.Namespace } }
  foreach ($g in ($shown | Group-Object Namespace | Sort-Object Name)) {
    Write-Host "   [$($g.Name)]" -ForegroundColor White
    foreach ($e in ($g.Group | Sort-Object Key)) {
      $shownVal = $e.Value
      if ($e.IsText) { $shownVal = "'$($e.Value)'" }
      $flag = ''
      if ($e.IsText -and -not $e.ValidUtf8) { $flag = '   <-- not valid UTF-8' }
      Write-Host ("     {0,-16} {1,-8} = {2}{3}" -f $e.Key, $e.Type, $shownVal, $flag)
    }
  }
}

function Compare-Config {
  <# Returns $true when nothing the app owns was lost or altered. #>
  param($Before, $After, [switch]$IncludeSystem)

  $scope = { param($e) $script:AppNamespaces -contains $e.Namespace }
  $b = @{}; $a = @{}
  foreach ($e in $Before) { if ($IncludeSystem -or (& $scope $e)) { $b[$e.Id] = $e } }
  foreach ($e in $After)  { if ($IncludeSystem -or (& $scope $e)) { $a[$e.Id] = $e } }

  $lost = @(); $changed = @(); $added = @(); $kept = 0
  foreach ($id in ($b.Keys | Sort-Object)) {
    if (-not $a.ContainsKey($id)) { $lost += $b[$id]; continue }
    if ([string]$a[$id].Value -ne [string]$b[$id].Value) {
      $changed += [pscustomobject]@{ Id = $id; From = $b[$id].Value; To = $a[$id].Value }
    }
    else { $kept++ }
  }
  foreach ($id in ($a.Keys | Sort-Object)) { if (-not $b.ContainsKey($id)) { $added += $a[$id] } }

  Write-Host ''
  Write-Ok ("{0} setting(s) identical after the flash" -f $kept)
  if ($added.Count -gt 0) {
    Write-Detail ("{0} new key(s) written by the new firmware:" -f $added.Count)
    foreach ($e in $added) { Write-Detail ("     + {0} = {1}" -f $e.Id, $e.Value) }
  }
  if ($changed.Count -gt 0) {
    Write-Warn ("{0} value(s) changed:" -f $changed.Count)
    foreach ($c in $changed) { Write-Warn ("     ~ {0}: {1} -> {2}" -f $c.Id, $c.From, $c.To) }
  }
  if ($lost.Count -gt 0) {
    Write-Err ("{0} setting(s) LOST:" -f $lost.Count)
    foreach ($e in $lost) { Write-Err ("     - {0} (was {1})" -f $e.Id, $e.Value) }
  }
  return (($lost.Count -eq 0) -and ($changed.Count -eq 0))
}

# ============================================================== main flow ====

$tool = Resolve-EspTool
$verbs = Get-EspToolVerbs -Tool $tool
Write-Detail ("esptool: {0}" -f ($verbs.Version -split "`n")[0])

if (-not $Port) { $Port = Find-DevicePort }
Write-Detail ("port: {0}" -f $Port)

if (-not $Chip) {
  Write-Step 'Identifying the board'
  $idOut = Invoke-EspTool -Tool $tool -Quiet -Arguments @('--port', $Port, '--no-stub', $verbs.Id)
  $Chip = 'esp32'
  if ($idOut -match 'ESP32-S3') { $Chip = 'esp32s3' }
  elseif ($idOut -match 'ESP32-C3') { $Chip = 'esp32c3' }
  $detail = [regex]::Match($idOut, 'Chip type:\s*(.+)')
  if ($detail.Success) { Write-Ok ("chip: {0}" -f $detail.Groups[1].Value.Trim()) }
  $fs = [regex]::Match($idOut, 'Detected flash size:\s*(\S+)')
  if ($fs.Success) { Write-Ok ("flash: {0}" -f $fs.Groups[1].Value) }
}

# The bootloader lives at 0x1000 on the original ESP32 and at 0x0 on the newer
# chips, and putting it at the wrong one is unbootable.
$bootOffset = '0x1000'
if ($Chip -ne 'esp32') { $bootOffset = '0x0' }

if (-not $BackupDir) {
  $BackupDir = Join-Path (Get-Location) ('upgrade-backups\{0}' -f (Get-Date -Format 'yyyyMMdd-HHmmss'))
}
if (-not (Test-Path $BackupDir)) { New-Item -ItemType Directory -Path $BackupDir -Force | Out-Null }

<# Values that identify one board rather than describing a setup. Everything
   else in the app's namespaces is tuning and is meant to be shared.

   MQTTClientID is the one that actually breaks things: MQTT requires client ids
   to be unique, and a broker disconnects the existing session when a second
   client connects with the same one - so two boards sharing it knock each other
   off the broker, over and over. Leaving it EMPTY is better than copying it,
   because the firmware then derives DiyBatteryBMS_<mac> per board.

   WifiHostName is the mDNS name and the SoftAP SSID, so two boards sharing it
   means <name>.local resolves to whichever answers first.

   MQTTTopic is listed because it is a judgement call rather than a fault: a
   replacement board should keep it and inherit the history, a second board
   running alongside needs its own or the two overwrite each other's readings. #>
$script:PerDeviceKeys = @(
  [pscustomobject]@{ Id = 'network/MQTTClientID'; Why = 'MQTT requires a unique client id - shared ids disconnect each other in a loop'; Fix = 'clear it, and the firmware derives DiyBatteryBMS_<mac> per board' },
  [pscustomobject]@{ Id = 'network/WifiHostName'; Why = 'mDNS name and SoftAP SSID'; Fix = 'give the second board its own name' },
  [pscustomobject]@{ Id = 'network/MQTTTopic';    Why = 'both boards publish here'; Fix = 'keep it for a replacement, change it for a board running alongside' }
)

<# GPIO assignments describe the board a setup was wired on, not the setup. They
   clone perfectly happily onto a different board model and are then simply
   wrong - and on a chip with a different pinout they can name a pin that does
   not exist or is reserved, which the firmware rejects at boot with
   "Forbidden or zero GPIO". Worth a warning rather than a refusal: cloning
   between two identical boards is the common case and wants them carried. #>
$script:PinKeys = @(
  'smartbms/CAN_TX_PIN', 'smartbms/CAN_RX_PIN', 'smartbms/CAN_EN_PIN', 'smartbms/CAN_CS_PIN',
  'smartbms/VictronRX', 'smartbms/VictronTX', 'smartbms/onewirepin', 'smartbms/fanpin'
)

function Get-SetPins {
  param($Entries)
  return @($Entries | Where-Object { $script:PinKeys -contains $_.Id -and [string]$_.Value -ne '0' })
}

function Write-CloneNotes {
  param($Entries, [string]$Path, [string]$SourceChip)
  $lines = @()
  $lines += 'DIY Battery BMS - clone kit'
  $lines += ('Taken from a {0} board on {1}' -f $SourceChip, (Get-Date -Format 'yyyy-MM-dd HH:mm:ss'))
  $lines += ''
  $lines += 'To set up another board without configuring it by hand:'
  $lines += '  .\Upgrade-DiyBatteryBMS.ps1 -CloneFrom .\nvs-clone.bin -Port COMx'
  $lines += ''
  $lines += 'Flash the firmware first if the board is new or on an older release,'
  $lines += 'then apply this. Order does not matter as long as both are done.'
  $lines += ''
  $lines += 'THEN CHANGE THESE, or the two boards will fight:'
  foreach ($k in $script:PerDeviceKeys) {
    $cur = $Entries | Where-Object { $_.Id -eq $k.Id }
    $val = '(not set)'
    if ($cur) { $val = [string]$cur.Value }
    $lines += ('  {0}' -f $k.Id)
    $lines += ('      copied value : {0}' -f $val)
    $lines += ('      why          : {0}' -f $k.Why)
    $lines += ('      what to do   : {0}' -f $k.Fix)
  }
  $pins = Get-SetPins -Entries $Entries
  $lines += ''
  if ($pins.Count -gt 0) {
    $lines += ('GPIO pins in this kit, only valid on the same board model ({0}):' -f $SourceChip)
    foreach ($p in $pins) { $lines += ('  {0} = {1}' -f $p.Id, $p.Value) }
    $lines += '  Cloning onto a different model carries these across unchanged. Check them'
    $lines += '  against how the second board is actually wired before trusting it.'
  }
  else {
    $lines += ('No GPIO pins are assigned in this kit, so it is safe to apply to a board')
    $lines += ('of any model. Wiring-specific pins would need checking; there are none.')
  }
  $lines += ''
  $lines += 'Not carried across, and not needed:'
  $lines += '  The Home Assistant device id is built from each board''s own MAC, so'
  $lines += '  entities stay separate with no action from you.'
  $lines += '  WiFi and PHY calibration in the image belongs to the source radio.'
  $lines += '  The IDF checks it against the running chip and recalibrates when it'
  $lines += '  does not match, so it corrects itself on first boot.'
  $lines += ''
  $lines += 'Contains WiFi and MQTT passwords, and the Victron BLE key, in clear.'
  $lines += 'Treat it like a password file.'
  Set-Content -Path $Path -Value $lines -Encoding UTF8
}

# -------------------------------------------------------- board identity ----

function Get-BoardState {
  <# What is on this board, so the right thing happens without being told.

     Four cases, and they need different handling:
       Blank   - erased or brand new, no partition table at all
       Foreign - a partition table, but not this project's and no settings of
                 ours in nvs. Something else lives here.
       Legacy  - this project on 2.x's default.csv layout: app0 is 0x140000 and
                 there is a spiffs partition. The upgrade that needs care.
       Current - this project on the 3.x table already. An ordinary update. #>
  param($Tool, $Verbs, [string]$Port, [string]$WorkDir)

  $tablePath = Join-Path $WorkDir 'partitions-before.bin'
  Invoke-EspTool -Tool $Tool -Quiet -Arguments @('--port', $Port, '--baud', '921600',
    $Verbs.Read, '0x8000', '0xC00', $tablePath) | Out-Null
  $table = @(Read-PartitionTable -Bytes ([IO.File]::ReadAllBytes($tablePath)))

  $state = [pscustomobject]@{
    Kind = 'Blank'; Table = $table; Nvs = $null; App0 = $null; Otadata = $null
    Entries = @(); HasOurSettings = $false; TablePath = $tablePath; NvsPath = $null
  }
  if ($table.Count -eq 0) { return $state }

  $state.Nvs     = $table | Where-Object { $_.Name -eq 'nvs' }
  $state.App0    = $table | Where-Object { $_.Name -eq 'app0' }
  $state.Otadata = $table | Where-Object { $_.Name -eq 'otadata' }

  if ($state.Nvs) {
    $nvsPath = Join-Path $WorkDir 'nvs-before.bin'
    Invoke-EspTool -Tool $Tool -Quiet -Arguments @('--port', $Port, '--baud', '921600', $Verbs.Read,
      ('0x{0:x}' -f $state.Nvs.Offset), ('0x{0:x}' -f $state.Nvs.Size), $nvsPath) | Out-Null
    $state.NvsPath = $nvsPath
    $state.Entries = @(Read-NvsEntries -Bytes ([IO.File]::ReadAllBytes($nvsPath)))
    # Our namespaces are the only reliable marker. A partition layout can be
    # coincidental; "smartbms" and "network" holding keys is not.
    $state.HasOurSettings = @($state.Entries | Where-Object { $script:AppNamespaces -contains $_.Namespace }).Count -gt 0
  }

  if (-not $state.HasOurSettings) { $state.Kind = 'Foreign'; return $state }

  $hasSpiffs = @($table | Where-Object { $_.Name -eq 'spiffs' }).Count -gt 0
  if ($hasSpiffs -or ($state.App0 -and $state.App0.Size -le 0x140000)) { $state.Kind = 'Legacy' }
  else { $state.Kind = 'Current' }
  return $state
}

function Clear-FlashRegion {
  <# Erase rather than write zeros: NVS and otadata both treat 0xFF as "never
     used", which is the state each of them wants to start from. #>
  param($Tool, $Verbs, [string]$Port, [string]$ChipName, $Partition, [string]$What)
  $erase = 'erase-region'
  if ($Verbs.Read -eq 'read_flash') { $erase = 'erase_region' }
  Write-Detail ("erasing {0} at 0x{1:x} + 0x{2:x}" -f $What, $Partition.Offset, $Partition.Size)
  Invoke-EspTool -Tool $Tool -Quiet -Arguments @('--chip', $ChipName, '--port', $Port, '--baud', '921600',
    $erase, ('0x{0:x}' -f $Partition.Offset), ('0x{0:x}' -f $Partition.Size)) | Out-Null
}

# ---- export a clone kit and stop ----
if ($ExportProfile) {
  if (-not (Test-Path $ExportProfile)) { New-Item -ItemType Directory -Path $ExportProfile -Force | Out-Null }
  $ExportProfile = (Resolve-Path $ExportProfile).Path

  Write-Step 'Reading the partition table'
  $pp = Join-Path $ExportProfile 'partitions.bin'
  Invoke-EspTool -Tool $tool -Quiet -Arguments @('--port', $Port, '--baud', '921600', $verbs.Read, '0x8000', '0xC00', $pp) | Out-Null
  $tbl = Read-PartitionTable -Bytes ([IO.File]::ReadAllBytes($pp))
  $nvsPart = $tbl | Where-Object { $_.Name -eq 'nvs' }
  if (-not $nvsPart) { throw 'No nvs partition on this board.' }
  Write-Detail (Format-Partition $nvsPart)

  Write-Step 'Pulling the settings'
  $img = Join-Path $ExportProfile 'nvs-clone.bin'
  Invoke-EspTool -Tool $tool -Quiet -Arguments @('--port', $Port, '--baud', '921600', $verbs.Read,
    ('0x{0:x}' -f $nvsPart.Offset), ('0x{0:x}' -f $nvsPart.Size), $img) | Out-Null
  $entries = Read-NvsEntries -Bytes ([IO.File]::ReadAllBytes($img))
  Write-Ok ("{0} entries, image {1:N0} bytes" -f $entries.Count, (Get-Item $img).Length)
  Show-Config -Entries $entries

  $entries | Select-Object Namespace, Key, Type, Value, ValidUtf8 |
    ConvertTo-Json -Depth 4 | Set-Content -Path (Join-Path $ExportProfile 'profile.json') -Encoding UTF8
  Write-CloneNotes -Entries $entries -Path (Join-Path $ExportProfile 'CLONE-NOTES.txt') -SourceChip $Chip

  # Recorded so -CloneFrom can tell whether it is going onto the same kind of
  # board, and warn about pin numbers if it is not.
  [pscustomobject]@{
    SourceChip = $Chip
    NvsOffset  = ('0x{0:x}' -f $nvsPart.Offset)
    NvsSize    = ('0x{0:x}' -f $nvsPart.Size)
    TakenUtc   = (Get-Date).ToUniversalTime().ToString('s')
    SetPins    = @(Get-SetPins -Entries $entries | ForEach-Object { "$($_.Id)=$($_.Value)" })
  } | ConvertTo-Json -Depth 4 | Set-Content -Path (Join-Path $ExportProfile 'kit.json') -Encoding UTF8

  $kitPins = Get-SetPins -Entries $entries
  if ($kitPins.Count -gt 0) {
    Write-Warn ("{0} GPIO pin(s) are set in this kit - only valid on another {1} wired the same way:" -f $kitPins.Count, $Chip)
    foreach ($p in $kitPins) { Write-Warn ("     {0} = {1}" -f $p.Id, $p.Value) }
  }
  else {
    Write-Ok 'No GPIO pins assigned, so this kit is safe on any board model.'
  }

  Write-Host ''
  Write-Ok ("Clone kit written to {0}" -f $ExportProfile)
  Write-Detail '  nvs-clone.bin    the settings, ready to write to another board'
  Write-Detail '  profile.json     the same values, readable'
  Write-Detail '  partitions.bin   the table this came from, so -CloneFrom can check it fits'
  Write-Detail '  CLONE-NOTES.txt  what to change on the second board, and why'
  Write-Host ''
  Write-Warn 'Apply it with:'
  Write-Warn ("  .\Upgrade-DiyBatteryBMS.ps1 -CloneFrom '{0}' -Port <target COM port>" -f $img)
  Write-Host ''
  Write-Warn 'The kit holds your WiFi and MQTT passwords and the BLE key in clear.'
  exit 0
}

# ---- apply a clone kit and stop ----
if ($CloneFrom) {
  if (-not (Test-Path $CloneFrom)) { throw "Clone image not found: $CloneFrom" }
  $CloneFrom = (Resolve-Path $CloneFrom).Path

  Write-Step 'Checking the target board'
  $tp = Join-Path $BackupDir 'partitions-target.bin'
  Invoke-EspTool -Tool $tool -Quiet -Arguments @('--port', $Port, '--baud', '921600', $verbs.Read, '0x8000', '0xC00', $tp) | Out-Null
  $tTbl = Read-PartitionTable -Bytes ([IO.File]::ReadAllBytes($tp))
  $tNvs = $tTbl | Where-Object { $_.Name -eq 'nvs' }
  if (-not $tNvs) { throw 'The target board has no nvs partition. Flash the firmware first.' }
  Write-Detail (Format-Partition $tNvs)

  $imgSize = (Get-Item $CloneFrom).Length
  if ($imgSize -ne $tNvs.Size) {
    throw ("Clone image is {0} bytes but the target's nvs is {1}. These boards do not share a partition layout - flash the matching firmware release first." -f $imgSize, $tNvs.Size)
  }

  # Pins describe wiring, not configuration, so say so when the kit came off a
  # different kind of board and is carrying some.
  $kitMeta = Join-Path (Split-Path $CloneFrom -Parent) 'kit.json'
  if (Test-Path $kitMeta) {
    $meta = Get-Content $kitMeta -Raw | ConvertFrom-Json
    if ($meta.SourceChip -and $meta.SourceChip -ne $Chip) {
      Write-Warn ("This kit was taken from an {0} and this board is an {1}." -f $meta.SourceChip, $Chip)
      if ($meta.SetPins -and @($meta.SetPins).Count -gt 0) {
        Write-Warn 'It carries GPIO assignments that describe the other board''s wiring:'
        foreach ($p in $meta.SetPins) { Write-Warn ("     {0}" -f $p) }
        Write-Warn 'Check these against how this board is wired once it is up.'
      }
      else {
        Write-Detail 'No GPIO pins are set in the kit, so nothing wiring-specific comes across.'
      }
    }
  }

  # Keep whatever is already there, so a mistake is recoverable
  Write-Step 'Backing up the target board first'
  $tBackup = Join-Path $BackupDir 'nvs-target-before.bin'
  Invoke-EspTool -Tool $tool -Quiet -Arguments @('--port', $Port, '--baud', '921600', $verbs.Read,
    ('0x{0:x}' -f $tNvs.Offset), ('0x{0:x}' -f $tNvs.Size), $tBackup) | Out-Null
  Write-Ok ("saved {0}" -f $tBackup)

  Write-Step 'Writing the settings'
  Invoke-EspTool -Tool $tool -Arguments @('--chip', $Chip, '--port', $Port, '--baud', '921600',
    $verbs.Write, ('0x{0:x}' -f $tNvs.Offset), $CloneFrom) | Out-Null

  Write-Step 'Reading them back'
  Start-Sleep -Seconds 8
  $verify = Join-Path $BackupDir 'nvs-target-after.bin'
  Invoke-EspTool -Tool $tool -Quiet -Arguments @('--port', $Port, '--baud', '921600', $verbs.Read,
    ('0x{0:x}' -f $tNvs.Offset), ('0x{0:x}' -f $tNvs.Size), $verify) | Out-Null
  $applied = Read-NvsEntries -Bytes ([IO.File]::ReadAllBytes($verify))
  $expected = Read-NvsEntries -Bytes ([IO.File]::ReadAllBytes($CloneFrom))
  Write-Ok ("{0} entries on the board, {1} in the kit" -f $applied.Count, $expected.Count)
  Show-Config -Entries $applied

  $missing = @()
  foreach ($e in $expected) {
    if ($script:AppNamespaces -notcontains $e.Namespace) { continue }
    $m = $applied | Where-Object { $_.Id -eq $e.Id -and [string]$_.Value -eq [string]$e.Value }
    if (-not $m) { $missing += $e }
  }
  Write-Host ''
  if ($missing.Count -gt 0) {
    Write-Err ("{0} setting(s) did not take:" -f $missing.Count)
    foreach ($e in $missing) { Write-Err ("     {0} = {1}" -f $e.Id, $e.Value) }
    Write-Warn ("Put the board back with: -Restore '{0}'" -f $tBackup)
    exit 1
  }
  Write-Ok 'Every setting in the kit is on the board. It will come up configured.'

  Write-Host ''
  Write-Warn 'Now change these on this board, or the two will fight:'
  foreach ($k in $script:PerDeviceKeys) {
    $cur = $applied | Where-Object { $_.Id -eq $k.Id }
    $val = '(not set)'
    if ($cur) { $val = [string]$cur.Value }
    Write-Warn ("  {0} = '{1}'" -f $k.Id, $val)
    Write-Detail ("      {0}" -f $k.Why)
    Write-Detail ("      {0}" -f $k.Fix)
  }
  Write-Host ''
  Write-Detail 'Both are on the WiFi/BLE/MQTT tab. Power-cycle the board first.'
  exit 0
}

# ---- restore mode: put a previous backup back and stop ----
if ($Restore) {
  if (-not (Test-Path $Restore)) { throw "Backup not found: $Restore" }
  Write-Step 'Restoring a previous NVS backup'
  $partPath = Join-Path $BackupDir 'partitions-current.bin'
  Invoke-EspTool -Tool $tool -Quiet -Arguments @('--port', $Port, '--baud', '921600', $verbs.Read, '0x8000', '0xC00', $partPath) | Out-Null
  $nvs = Read-PartitionTable -Bytes ([IO.File]::ReadAllBytes($partPath)) | Where-Object { $_.Name -eq 'nvs' }
  if (-not $nvs) { throw 'No nvs partition on the device.' }
  $size = (Get-Item $Restore).Length
  if ($size -ne $nvs.Size) {
    throw ("Backup is {0} bytes but nvs is {1} - refusing to write a mismatched image." -f $size, $nvs.Size)
  }
  Invoke-EspTool -Tool $tool -Arguments @('--chip', $Chip, '--port', $Port, '--baud', '921600',
    $verbs.Write, ('0x{0:x}' -f $nvs.Offset), $Restore) | Out-Null
  Write-Ok 'NVS restored. Power-cycle the board.'
  exit 0
}

# ---- 1. work out what is on the board ----
Write-Step 'Looking at the board'
$state = Get-BoardState -Tool $tool -Verbs $verbs -Port $Port -WorkDir $BackupDir
foreach ($p in $state.Table) { Write-Detail (Format-Partition $p) }
if ($state.Table.Count -eq 0) { Write-Detail 'no partition table at 0x8000' }

$curNvs  = $state.Nvs
$curApp0 = $state.App0
$before  = $state.Entries
$nvsBefore = $state.NvsPath

switch ($state.Kind) {
  'Blank'   { Write-Ok 'Blank board - nothing installed. This will be a fresh install.' }
  'Foreign' { Write-Warn 'A partition table is present but none of this project''s settings are in nvs.'
              Write-Warn 'Something else is installed here, and installing will replace it.' }
  'Legacy'  { Write-Ok 'DIY Battery BMS on the 2.x partition layout. This is the upgrade that needs the table replaced.' }
  'Current' { Write-Ok 'DIY Battery BMS already on the 3.x layout. An ordinary update.' }
}

$isInstall = ($state.Kind -eq 'Blank') -or ($state.Kind -eq 'Foreign')

if ($state.HasOurSettings) {
  Write-Step 'Pulling the configuration out of NVS'
  Write-Ok ("{0} entries read, backup at {1}" -f $before.Count, $nvsBefore)
  Show-Config -Entries $before -All:$IncludeSystemKeys

  $beforeJson = Join-Path $BackupDir 'config-before.json'
  $before | Select-Object Namespace, Key, Type, Value, ValidUtf8 | ConvertTo-Json -Depth 4 | Set-Content -Path $beforeJson -Encoding UTF8
  Write-Detail ("inventory: {0}" -f $beforeJson)

  $badUtf8 = @($before | Where-Object { $_.IsText -and -not $_.ValidUtf8 })
  if ($badUtf8.Count -gt 0) {
    Write-Warn ("{0} stored string(s) are not valid UTF-8 - 3.0 and later fall back to defaults for these:" -f $badUtf8.Count)
    foreach ($e in $badUtf8) { Write-Warn ("     {0} = {1}" -f $e.Id, $e.Value) }
  }
}
else {
  Write-Detail 'No settings of ours to preserve.'
}

if ($PullOnly) {
  Write-Host ''
  Write-Ok 'Pull complete. Nothing was written.'
  exit 0
}

# ---- 2. check the release ----
if (-not $ReleaseDir) { throw 'Give -ReleaseDir (an unzipped release, or a .pio\build\<env> folder), or use -PullOnly.' }
$ReleaseDir = (Resolve-Path $ReleaseDir).Path

$fwPath   = Join-Path $ReleaseDir 'firmware.bin'
$blPath   = Join-Path $ReleaseDir 'bootloader.bin'
$partPath = Join-Path $ReleaseDir 'partitions.bin'
foreach ($f in @($fwPath, $blPath, $partPath)) {
  if (-not (Test-Path $f)) { throw ("Missing {0} in {1}" -f (Split-Path $f -Leaf), $ReleaseDir) }
}

Write-Step 'Checking the release against this board'
$newTable = @(Read-PartitionTable -Bytes ([IO.File]::ReadAllBytes($partPath)))
if ($newTable.Count -eq 0) { throw 'partitions.bin in the release is not a partition table.' }
$newNvs  = $newTable | Where-Object { $_.Name -eq 'nvs' }
$newApp0 = $newTable | Where-Object { $_.Name -eq 'app0' }
$newOta  = $newTable | Where-Object { $_.Name -eq 'otadata' }
if (-not $newNvs) { throw 'The release partition table has no nvs partition.' }

if ($state.HasOurSettings) {
  Write-Detail ("device nvs : 0x{0:x} + 0x{1:x}" -f $curNvs.Offset, $curNvs.Size)
  Write-Detail ("release nvs: 0x{0:x} + 0x{1:x}" -f $newNvs.Offset, $newNvs.Size)

  # Only worth refusing over when there is something to lose. On a blank or
  # foreign board a relocated nvs costs nothing.
  $nvsMoved = ($newNvs.Offset -ne $curNvs.Offset) -or ($newNvs.Size -ne $curNvs.Size)
  if ($nvsMoved) {
    Write-Err 'The release moves or resizes the nvs partition.'
    Write-Err 'Flashing it will leave the settings unreadable - they are not where the new firmware looks.'
    Write-Err ("A backup is already saved at {0}, but restoring it into a relocated nvs will not help." -f $nvsBefore)
    if (-not $Force) { throw 'Refusing to flash. Re-run with -Force if this is genuinely what you want.' }
    Write-Warn '-Force given, continuing anyway.'
  }
  else {
    Write-Ok 'nvs is unchanged - settings will survive the flash.'
  }
}

$fwSize = (Get-Item $fwPath).Length
if ($newApp0 -and $fwSize -gt $newApp0.Size) {
  throw ("firmware.bin is {0} bytes but app0 in the release table is only {1}." -f $fwSize, $newApp0.Size)
}
if ($newApp0) {
  Write-Ok ("firmware.bin {0:N0} bytes, app0 {1:N0} bytes ({2:N1}% used)" -f `
    $fwSize, $newApp0.Size, (100 * $fwSize / $newApp0.Size))
}
if ($curApp0 -and $newApp0 -and $curApp0.Size -ne $newApp0.Size) {
  Write-Detail ("app0 changes from 0x{0:x} to 0x{1:x}" -f $curApp0.Size, $newApp0.Size)
  if ($state.Kind -eq 'Legacy') {
    Write-Detail ("{0:N0} bytes of firmware could never fit 2.x's {1:N0} byte slot, which is why an OTA cannot do this upgrade." -f $fwSize, $curApp0.Size)
  }
}

# ---- 3. flash ----
Write-Step 'Flashing'
if ($isInstall) { Write-Detail 'Fresh install: bootloader, partition table and firmware.' }
else            { Write-Detail 'bootloader, partition table and firmware only. No erase, so nvs is left alone.' }
Invoke-EspTool -Tool $tool -Arguments @(
  '--chip', $Chip, '--port', $Port, '--baud', '921600',
  $verbs.Write, '-z', '--flash-mode', 'dio', '--flash-size', 'detect',
  $bootOffset, $blPath,
  '0x8000', $partPath,
  '0x10000', $fwPath) | Out-Null
Write-Ok 'Flash written and verified by esptool.'

<# otadata decides which app slot the bootloader starts, and it is NOT touched
   by writing app0. A 2.x board that has ever taken an over-the-air update is
   booting from app1, and app1 in the new table lands where the old spiffs used
   to be - so the board would come up on whatever bytes happen to be there
   rather than on the firmware just written. Erasing otadata resets the choice
   to app0, which is the slot the firmware went into.

   Also done for a fresh install, where the region may hold a previous
   occupant's ota state. Never done for an ordinary 3.x update, where the
   running slot is already correct. #>
if ($isInstall -or $state.Kind -eq 'Legacy') {
  Write-Step 'Pointing the bootloader at the firmware just written'
  $otaTarget = $newOta
  if (-not $otaTarget) { $otaTarget = $state.Otadata }
  if ($otaTarget) {
    Clear-FlashRegion -Tool $tool -Verbs $verbs -Port $Port -ChipName $Chip -Partition $otaTarget -What 'otadata'
    Write-Ok 'otadata cleared - the board will boot the slot that was just flashed.'
  }
  else { Write-Warn 'No otadata partition found; leaving boot selection alone.' }
}

<# A foreign board's nvs region holds another project's data, or in a fresh
   install whatever was at that offset before. NVS would detect that as
   unusable and format it on first boot anyway - doing it here means the first
   boot is clean rather than recovering. Never on an upgrade: that region is
   the settings. #>
if ($isInstall -and $newNvs) {
  Write-Step 'Clearing the settings area for a fresh start'
  Clear-FlashRegion -Tool $tool -Verbs $verbs -Port $Port -ChipName $Chip -Partition $newNvs -What 'nvs'
}

# ---- 4. check what came up ----
Write-Step 'Waiting for the board to boot'
Start-Sleep -Seconds 12      # first boot after a flash also re-inits NVS pages

if ($isInstall) {
  Write-Step 'Reading the board back'
  $instPart = Join-Path $BackupDir 'partitions-after.bin'
  Invoke-EspTool -Tool $tool -Quiet -Arguments @('--port', $Port, '--baud', '921600', $verbs.Read, '0x8000', '0xC00', $instPart) | Out-Null
  $instTable = @(Read-PartitionTable -Bytes ([IO.File]::ReadAllBytes($instPart)))
  foreach ($p in $instTable) { Write-Detail (Format-Partition $p) }
  $instNvs = $instTable | Where-Object { $_.Name -eq 'nvs' }
  $fresh = @()
  if ($instNvs) {
    $instNvsPath = Join-Path $BackupDir 'nvs-after.bin'
    Invoke-EspTool -Tool $tool -Quiet -Arguments @('--port', $Port, '--baud', '921600', $verbs.Read,
      ('0x{0:x}' -f $instNvs.Offset), ('0x{0:x}' -f $instNvs.Size), $instNvsPath) | Out-Null
    $fresh = @(Read-NvsEntries -Bytes ([IO.File]::ReadAllBytes($instNvsPath)))
  }
  $ours = @($fresh | Where-Object { $script:AppNamespaces -contains $_.Namespace })
  Write-Host ''
  if ($ours.Count -gt 0) {
    Write-Ok ("Installed. The firmware booted and wrote {0} default setting(s)." -f $ours.Count)
  }
  else {
    Write-Warn 'Installed, but the firmware has not written its defaults yet.'
    Write-Detail 'Give it a moment and power-cycle; if nothing appears, watch the serial log.'
  }
  Write-Host ''
  Write-Detail 'It has no WiFi credentials, so it starts an access point named after its'
  Write-Detail 'hostname (DIY-BATTERY by default). Join that and open http://192.168.4.1'
  Write-Detail 'to configure it - or apply a saved setup with -CloneFrom.'
  Write-Detail ("Backups kept in {0}" -f $BackupDir)
  exit 0
}

Write-Step 'Reading the configuration back'
$nvsAfter = Join-Path $BackupDir 'nvs-after.bin'
$afterPartPath = Join-Path $BackupDir 'partitions-after.bin'
Invoke-EspTool -Tool $tool -Quiet -Arguments @('--port', $Port, '--baud', '921600', $verbs.Read, '0x8000', '0xC00', $afterPartPath) | Out-Null
$afterTable = Read-PartitionTable -Bytes ([IO.File]::ReadAllBytes($afterPartPath))
$afterNvs = $afterTable | Where-Object { $_.Name -eq 'nvs' }
Invoke-EspTool -Tool $tool -Quiet -Arguments @('--port', $Port, '--baud', '921600', $verbs.Read,
  ('0x{0:x}' -f $afterNvs.Offset), ('0x{0:x}' -f $afterNvs.Size), $nvsAfter) | Out-Null
$after = Read-NvsEntries -Bytes ([IO.File]::ReadAllBytes($nvsAfter))
Write-Ok ("{0} entries read back" -f $after.Count)

$afterJson = Join-Path $BackupDir 'config-after.json'
$after | Select-Object Namespace, Key, Type, Value, ValidUtf8 | ConvertTo-Json -Depth 4 | Set-Content -Path $afterJson -Encoding UTF8

Write-Step 'Comparing before and after'
$clean = Compare-Config -Before $before -After $after -IncludeSystem:$IncludeSystemKeys

Write-Host ''
if ($clean) {
  Write-Ok 'Upgrade complete. Every configured setting survived.'
  Write-Detail ("Backups kept in {0}" -f $BackupDir)
  exit 0
}
Write-Err 'Upgrade finished, but the configuration did not come through unchanged.'
Write-Host ''
Write-Host '   To put the old settings back:' -ForegroundColor Yellow
Write-Host ("     .\Upgrade-DiyBatteryBMS.ps1 -Restore '{0}' -Port {1}" -f $nvsBefore, $Port) -ForegroundColor Yellow
exit 1
