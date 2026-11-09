<#
  Backup-HavenDOS-Rescue.ps1
  ==========================
  Saves everything HavenDOS's installer is capable of changing, so that it
  can be put back.

  HavenDOS's additive installer writes, outside the new partition it
  creates, exactly four sectors: the two GPT headers and one sector of each
  of the two partition-entry arrays. It also adds files to the EFI System
  Partition. Nothing else outside the new partition is touched -- that is
  checked byte for byte by its test suite and by an end-to-end run under
  UEFI firmware.

  This script saves more than those four sectors. It saves:

      * the first 34 sectors   -- protective MBR, GPT header, entry array
      * the last  34 sectors   -- the backup GPT header and entry array
      * the whole EFI System Partition, as an image

  That is enough to undo every change the installer can make, and it is
  small: the GPT copies are 17 KB each and an ESP is usually 100-300 MB.

  THIS SCRIPT ONLY READS. It opens the disk read-only and writes nothing to
  it. The restore procedure is deliberately NOT here -- it is in
  RESTORE.md, as commands you run from a Linux USB, because a restore is
  rare, serious, and better done deliberately than by running a script.

  Run it in an ADMINISTRATOR PowerShell:

      Set-ExecutionPolicy -Scope Process Bypass -Force
      .\Backup-HavenDOS-Rescue.ps1

  By default it saves to a HavenDOS-Rescue folder on your Desktop. PUT THAT
  FOLDER ON A USB STICK when it is done. A backup that lives only on the
  disk it is protecting is not a backup.
#>

[CmdletBinding()]
param(
    [int]    $DiskNumber = -1,
    [string] $OutDir     = "$env:USERPROFILE\Desktop\HavenDOS-Rescue"
)

$ErrorActionPreference = 'Stop'

function Fail($m) { Write-Host "`n  ERROR: $m`n" -ForegroundColor Red; exit 1 }

if (-not ([Security.Principal.WindowsPrincipal] `
          [Security.Principal.WindowsIdentity]::GetCurrent()
         ).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
    Fail "Run this in an Administrator PowerShell. Reading raw sectors needs it."
}

Write-Host ""
Write-Host "  HavenDOS rescue backup" -ForegroundColor Cyan
Write-Host "  ----------------------"
Write-Host ""

# ── which disk ────────────────────────────────────────────────────────
if ($DiskNumber -lt 0) {
    Get-Disk | Format-Table Number, FriendlyName,
        @{N='Size';E={'{0:N0} GB' -f ($_.Size/1GB)}},
        PartitionStyle, BusType -AutoSize | Out-String | Write-Host
    $DiskNumber = Read-Host "  Which disk number is the one with Windows on it"
    if ($DiskNumber -notmatch '^\d+$') { Fail "Not a number." }
    $DiskNumber = [int]$DiskNumber
}

$disk = Get-Disk -Number $DiskNumber
if ($disk.PartitionStyle -ne 'GPT') {
    Fail "Disk $DiskNumber is $($disk.PartitionStyle), not GPT. HavenDOS's additive installer only works on GPT disks, so this is not the disk you mean."
}

$sectorSize  = 512
$totalSector = [int64]($disk.Size / $sectorSize)
Write-Host "  Disk $DiskNumber : $($disk.FriendlyName)"
Write-Host "  $([int64]$disk.Size) bytes, $totalSector sectors of $sectorSize"
Write-Host ""

# ── the ESP ───────────────────────────────────────────────────────────
$esp = Get-Partition -DiskNumber $DiskNumber |
       Where-Object { $_.GptType -eq '{c12a7328-f81f-11d2-ba4b-00a0c93ec93b}' } |
       Select-Object -First 1
if (-not $esp) { Fail "No EFI System Partition on disk $DiskNumber." }

$espStart  = [int64]$esp.Offset
$espBytes  = [int64]$esp.Size
Write-Host "  EFI System Partition: offset $espStart, $([int]($espBytes/1MB)) MB"
Write-Host ""

New-Item -ItemType Directory -Force -Path $OutDir | Out-Null

# ── read raw sectors, read-only ───────────────────────────────────────
function Save-Range {
    param([int64]$Offset, [int64]$Length, [string]$Path, [string]$What)

    $h = New-Object System.IO.FileStream(
            "\\.\PhysicalDrive$DiskNumber",
            [System.IO.FileMode]::Open,
            [System.IO.FileAccess]::Read,          # READ ONLY
            [System.IO.FileShare]::ReadWrite)
    try {
        $h.Position = $Offset
        $out = [System.IO.File]::Create($Path)
        try {
            $buf  = New-Object byte[] (1MB)
            $left = $Length
            $sha  = [System.Security.Cryptography.SHA256]::Create()
            while ($left -gt 0) {
                $want = [Math]::Min([int64]$buf.Length, $left)
                $got  = $h.Read($buf, 0, [int]$want)
                if ($got -le 0) { throw "short read at offset $($h.Position)" }
                $out.Write($buf, 0, $got)
                $sha.TransformBlock($buf, 0, $got, $null, 0) | Out-Null
                $left -= $got
            }
            $sha.TransformFinalBlock(@(), 0, 0) | Out-Null
            $hex = ($sha.Hash | ForEach-Object { $_.ToString('x2') }) -join ''
            Write-Host ("  saved {0,-26} {1,12:N0} bytes  sha256 {2}" -f $What, $Length, $hex.Substring(0,16))
            return $hex
        } finally { $out.Dispose() }
    } finally { $h.Dispose() }
}

$manifest = [ordered]@{
    created      = (Get-Date).ToString('o')
    diskNumber   = $DiskNumber
    friendlyName = $disk.FriendlyName
    serialNumber = $disk.SerialNumber
    sizeBytes    = [int64]$disk.Size
    sectorSize   = $sectorSize
    totalSectors = $totalSector
    espOffset    = $espStart
    espBytes     = $espBytes
    espStartLba  = [int64]($espStart / $sectorSize)
    files        = [ordered]@{}
}

$manifest.files['gpt-primary.bin'] = Save-Range 0 (34 * $sectorSize) `
    (Join-Path $OutDir 'gpt-primary.bin') 'primary GPT (LBA 0-33)'

$backupOff = ($totalSector - 33) * $sectorSize
$manifest.files['gpt-backup.bin'] = Save-Range $backupOff (33 * $sectorSize) `
    (Join-Path $OutDir 'gpt-backup.bin') 'backup GPT (last 33)'
$manifest.backupGptStartLba = $totalSector - 33

$manifest.files['esp.img'] = Save-Range $espStart $espBytes `
    (Join-Path $OutDir 'esp.img') 'EFI System Partition'

# the partition table in a form a human can read, and sgdisk can rebuild from
Get-Partition -DiskNumber $DiskNumber |
    Select-Object PartitionNumber, DriveLetter, Offset, Size, Type, GptType |
    ConvertTo-Json -Depth 3 |
    Set-Content -Encoding UTF8 (Join-Path $OutDir 'partitions.json')

$manifest | ConvertTo-Json -Depth 4 |
    Set-Content -Encoding UTF8 (Join-Path $OutDir 'manifest.json')

Write-Host ""
Write-Host "  Written to $OutDir" -ForegroundColor Green
Write-Host ""
Write-Host "  COPY THAT FOLDER TO A USB STICK NOW." -ForegroundColor Yellow
Write-Host "  A backup that lives on the disk it protects is not a backup."
Write-Host ""
Write-Host "  RESTORE.md in the same folder as this script says how to put it"
Write-Host "  back, from a Linux USB, if you ever need to."
Write-Host ""
