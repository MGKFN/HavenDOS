<#
    Stage-HavenDOS-Update.ps1 — put a HavenDOS update on the EFI System
    Partition from Windows, so `update` can find it.

    From v0.8.4 HavenDOS can read a new kernel off the ESP: the partition
    the firmware already boots from, which is FAT32, which HavenDOS already
    reads, and which Windows can mount with its own built-in `mountvol`.
    That is the whole upgrade route on a machine with no optical drive, no
    USB mass storage in HavenDOS yet, and no supported network adapter.

    It writes exactly two files:

        \EFI\HAVENDOS\update\havendos.elf    the new kernel
        \EFI\HAVENDOS\update\update.inf      version, size and CRC-32

    NOTHING ELSE ON THE ESP IS TOUCHED. It creates one directory and
    writes those two files. It does not go near \EFI\Microsoft, \EFI\BOOT,
    or the BCD.

    USAGE — put this file, havendos.elf and update.inf in the same folder,
    open an ADMINISTRATOR PowerShell there, and run:

        Set-ExecutionPolicy -Scope Process Bypass -Force
        .\Stage-HavenDOS-Update.ps1 -Kernel .\havendos.elf -Manifest .\update.inf

    Then boot the HavenDOS ISO, pick "Update installed system from the
    ESP", and type:  update

    Without -Manifest it computes one for you, and then -Version is
    required:

        .\Stage-HavenDOS-Update.ps1 -Kernel .\havendos.elf -Version 0.8.4

    Tested against Windows PowerShell 5.1 and PowerShell 7.
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string] $Kernel,

    [string] $Version,

    # Use a manifest that already exists -- the /update.inf from the ISO.
    # The script still checks it describes the kernel you passed.
    [string] $Manifest,

    # Where to mount the ESP while working. Removed afterwards.
    [string] $MountLetter = 'S'
)

$ErrorActionPreference = 'Stop'

function Require-Admin {
    $id = [Security.Principal.WindowsIdentity]::GetCurrent()
    $pr = New-Object Security.Principal.WindowsPrincipal($id)
    if (-not $pr.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
        throw "Run this in an Administrator PowerShell -- mountvol needs it."
    }
}

<#
    CRC-32, reflected polynomial 0xEDB88320. The same one fs/gpt.c
    computes and the same one python3's zlib computes, which is the point:
    the number in the manifest can be checked by anything.

    Done in C# through Add-Type rather than in PowerShell, for two
    reasons, both found the hard way:

      - Windows PowerShell 5.1 parses the literal 0xFFFFFFFF as Int32 -1,
        and [uint32](-1) throws "Value was either too large or too small
        for a UInt32". A PowerShell implementation has to spell every
        constant in decimal and cast on every step to be correct.
      - A per-byte PowerShell loop over a 2.8 MB kernel is minutes of
        work. The same loop in C# is milliseconds.
#>
if (-not ('HavenDOS.Crc32' -as [type])) {
    Add-Type -TypeDefinition @'
namespace HavenDOS {
    public static class Crc32 {
        static readonly uint[] T = new uint[256];
        static Crc32() {
            for (uint i = 0; i < 256; i++) {
                uint c = i;
                for (int k = 0; k < 8; k++)
                    c = ((c & 1) != 0) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
                T[i] = c;
            }
        }
        public static uint Compute(byte[] b) {
            uint c = 0xFFFFFFFFu;
            for (int i = 0; i < b.Length; i++)
                c = T[(c ^ b[i]) & 0xFF] ^ (c >> 8);
            return c ^ 0xFFFFFFFFu;
        }
    }
}
'@
}

function Get-Crc32([byte[]] $Bytes) { return [HavenDOS.Crc32]::Compute($Bytes) }

Require-Admin

if (-not (Test-Path -LiteralPath $Kernel)) { throw "No such file: $Kernel" }
$kpath = (Resolve-Path -LiteralPath $Kernel).Path
$bytes = [IO.File]::ReadAllBytes($kpath)
if ($bytes.Length -lt 4 -or
    $bytes[0] -ne 0x7F -or $bytes[1] -ne 0x45 -or
    $bytes[2] -ne 0x4C -or $bytes[3] -ne 0x46) {
    throw "$Kernel does not start with ELF magic. That is not a HavenDOS kernel."
}

$crc = Get-Crc32 $bytes
Write-Host ("kernel : {0}" -f $kpath)
Write-Host ("size   : {0} bytes" -f $bytes.Length)
Write-Host ("crc32  : {0:x8}" -f $crc)

if ($Manifest) {
    if (-not (Test-Path -LiteralPath $Manifest)) { throw "No such file: $Manifest" }
    $text = [IO.File]::ReadAllText((Resolve-Path -LiteralPath $Manifest).Path)

    if ($text -notmatch '(?m)^\s*size\s*=\s*(\d+)\s*$') {
        throw "That manifest has no size= line. Is it really update.inf?"
    }
    if ([int64]$Matches[1] -ne $bytes.Length) {
        throw ("The manifest describes a {0}-byte kernel and this one is {1} bytes. Refusing." `
               -f $Matches[1], $bytes.Length)
    }
    if ($text -match '(?m)^\s*crc32\s*=\s*(?:0[xX])?([0-9a-fA-F]+)\s*$') {
        if ([Convert]::ToUInt32($Matches[1], 16) -ne $crc) {
            throw "The manifest's crc32 does not match this kernel. Refusing."
        }
        Write-Host "manifest: size and crc32 both match this kernel"
    } else {
        Write-Host "manifest: size matches (it carries no crc32)"
    }
} else {
    if (-not $Version) {
        throw "Pass -Manifest .\update.inf, or -Version (for example -Version 0.8.4)."
    }
    $text = "HAVENDOS-UPDATE 1`r`n" +
            "# staged by Stage-HavenDOS-Update.ps1`r`n" +
            "version=$Version`r`n" +
            "size=$($bytes.Length)`r`n" +
            ("crc32={0:x8}`r`n" -f $crc) +
            "kernel=havendos.elf`r`n"
}

$drive = "${MountLetter}:"
if (Test-Path "$drive\") {
    throw "$drive is already in use. Pass another with -MountLetter, e.g. -MountLetter T"
}

Write-Host "`nmounting the EFI System Partition on $drive ..."
& mountvol $drive /S
if ($LASTEXITCODE -ne 0) { throw "mountvol failed. Are you Administrator?" }

try {
    $dir = "$drive\EFI\HAVENDOS\update"
    New-Item -ItemType Directory -Force -Path $dir | Out-Null

    [IO.File]::WriteAllBytes("$dir\havendos.elf", $bytes)
    [IO.File]::WriteAllText("$dir\update.inf", $text,
                            (New-Object Text.UTF8Encoding $false))

    # Read both back and check them. "The write returned success" is a
    # different claim from "the bytes are there", and the ESP is the one
    # partition on this machine where that distinction has teeth.
    $back = [IO.File]::ReadAllBytes("$dir\havendos.elf")
    if ($back.Length -ne $bytes.Length) { throw "the staged kernel is the wrong length" }
    if ((Get-Crc32 $back) -ne $crc)     { throw "the staged kernel reads back wrong" }

    Write-Host "`nstaged and verified by reading back:"
    Write-Host "  \EFI\HAVENDOS\update\havendos.elf"
    Write-Host "  \EFI\HAVENDOS\update\update.inf"
    Write-Host "`nNow boot the HavenDOS ISO, choose"
    Write-Host "  'Update installed system from the ESP'"
    Write-Host "and type:  update"
}
finally {
    Write-Host "`nunmounting $drive ..."
    & mountvol $drive /D
}
