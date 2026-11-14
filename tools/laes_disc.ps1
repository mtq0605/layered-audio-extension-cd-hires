# SPDX-FileCopyrightText: 2026 mtq0605
# SPDX-License-Identifier: Apache-2.0
# laes_disc.ps1 (2026-11-03): makes a DVD or BD data-disc image (.iso) of an album of LAES files, with the disc-image service of Windows
# (IMAPI2). A data disc, not BD-Video / BDMV / DVD-Audio (those are licensed formats): any player that reads FLAC (or WAV) files from a
# disc plays the core master (it can be CD-compatible); an LAES player plays the masters. Nothing is converted or changed: the files are copied as they are.
#
#   powershell -ExecutionPolicy Bypass -File tools\laes_disc.ps1 -Album <folder> -Out <image.iso> -Disc BD25 [-Label <name>] [-Export <laes_export.exe>]
#
# -Disc: DVD5, DVD9, BD25, BD50 or BD100 (sets the capacity that is checked). BD: UDF 2.50. DVD: UDF 1.02 + ISO 9660 + Joliet.
# Every LAES file (*.flac, *.wav) of the folder is opened once with laes_export --list (file SHA-256, CRC-32, core master); a file that is not
# LAES or fails a check stops the run. The disc holds:
#   <Label>\<files>        the LAES files, in name order
#   <Label>\playlist.m3u8  the files in that order (relative paths, UTF-8)
#   <Label>\SHA256SUMS.txt the SHA-256 of every file (sha256sum format), for checking a copy
#   README.txt             what the disc is and how to play it
param([Parameter(Mandatory = $true)][string]$Album, [Parameter(Mandatory = $true)][string]$Out, [Parameter(Mandatory = $true)][ValidateSet('DVD5', 'DVD9', 'BD25', 'BD50', 'BD100')][string]$Disc,
      [string]$Label = '', [string]$Export = '')
$ErrorActionPreference = 'Stop'
$blocksOf = @{ DVD5 = 2295104; DVD9 = 4173824; BD25 = 12219392; BD50 = 24438784; BD100 = 48878592 }   # 2048-byte sectors (single / dual layer)
if (-not (Test-Path -LiteralPath $Album -PathType Container)) { throw "no folder $Album" }
if ($Label -eq '') { $Label = Split-Path -Leaf (Resolve-Path -LiteralPath $Album) }
if ($Export -eq '') { $Export = Join-Path $PSScriptRoot '..\build\laes_export.exe' }
if (-not (Test-Path -LiteralPath $Export)) { throw "laes_export.exe not found ($Export); give -Export" }
if (Test-Path -LiteralPath $Out) { throw "$Out exists; nothing is overwritten" }
$partOut = [IO.Path]::GetFullPath($Out) + '.part'   # the image is written here and renamed to -Out only when it is complete
if (Test-Path -LiteralPath $partOut) { throw "$partOut exists (left by an interrupted run?); nothing is overwritten" }

$tracks = @(Get-ChildItem -LiteralPath $Album -File | Where-Object { $_.Extension -in '.flac', '.wav' } | Sort-Object Name)
if ($tracks.Count -eq 0) { throw "no .flac or .wav files in $Album" }
foreach ($tr in $tracks) {   # each file must be LAES and pass its checks
    $listing = & $Export $tr.FullName --list 2>&1
    if ($LASTEXITCODE -ne 0 -or ($listing -match '^note:')) { throw "$($tr.Name): not LAES or a check failed: $($listing -join ' ')" }
    Write-Host "$($tr.Name): LAES ok, $(@($listing).Count) choices" }

$sizeAll = ($tracks | Measure-Object -Property Length -Sum).Sum
$capBytes = [int64]$blocksOf[$Disc] * 2048
Write-Host ("{0} files, {1:N0} MB of {2:N0} MB ({3})" -f $tracks.Count, ($sizeAll / 1MB), ($capBytes / 1MB), $Disc)
if ($sizeAll + 16MB -gt $capBytes) { throw "the album does not fit on $Disc" }

# the small files, made in a folder of their own next to the image and removed at the end
$partMine = $false
$work = Join-Path (Split-Path -Parent ([IO.Path]::GetFullPath($Out))) ('laes_disc_work_' + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $work | Out-Null
try {
    $utf8 = New-Object System.Text.UTF8Encoding($false)
    [IO.File]::WriteAllLines((Join-Path $work 'playlist.m3u8'), [string[]](@('#EXTM3U') + ($tracks | ForEach-Object { $_.Name })), $utf8)
    $sums = foreach ($tr in $tracks) { (Get-FileHash -LiteralPath $tr.FullName -Algorithm SHA256).Hash.ToLower() + ' *' + $tr.Name }
    [IO.File]::WriteAllLines((Join-Path $work 'SHA256SUMS.txt'), [string[]]$sums, $utf8)
    $readme = @(
        "$Label",
        '',
        'LAES (LAE Lossless) audio files, one per track, in the folder of this name.',
        'Every FLAC or WAV player plays the core master of each file, which can be CD-compatible. A player with LAES support (for example foo_input_lae for foobar2000)',
        'also plays the lossless masters it contains, after checking them against their SHA-256.',
        'playlist.m3u8 lists the tracks in order; SHA256SUMS.txt holds the SHA-256 of every file, for checking a copy.')
    [IO.File]::WriteAllLines((Join-Path $work 'README.txt'), [string[]]$readme, $utf8)

    $fsi = New-Object -ComObject IMAPI2FS.MsftFileSystemImage
    $fsi.FreeMediaBlocks = $blocksOf[$Disc]
    if ($Disc -like 'BD*') { $fsi.FileSystemsToCreate = 4; $fsi.UDFRevision = 0x250 } else { $fsi.FileSystemsToCreate = 7; $fsi.UDFRevision = 0x102 }   # 4 UDF; 7 ISO 9660 + Joliet + UDF
    $volName = ($Label -replace '[^A-Za-z0-9_]', '_'); if ($volName.Length -gt 32) { $volName = $volName.Substring(0, 32) }
    $fsi.VolumeName = $volName
    $root = $fsi.Root; $root.AddDirectory($Label); $dirItem = $root.Item($Label)
    foreach ($tr in $tracks) { $dirItem.AddTree($tr.FullName, $false) }
    $root.AddTree((Join-Path $work 'README.txt'), $false)
    $dirItem.AddTree((Join-Path $work 'playlist.m3u8'), $false); $dirItem.AddTree((Join-Path $work 'SHA256SUMS.txt'), $false)
    $img = $fsi.CreateResultImage()

    # the image stream (COM IStream) to the .iso file
    if (-not ('LaesIsoWriter' -as [type])) {
        Add-Type -TypeDefinition @'
using System; using System.IO; using System.Runtime.InteropServices; using System.Runtime.InteropServices.ComTypes;
public static class LaesIsoWriter {
    public static void Save(object comStream, string path) {
        IStream s = (IStream)comStream; byte[] buf = new byte[1 << 20]; IntPtr pRead = Marshal.AllocHGlobal(4);
        try { using (FileStream fo = new FileStream(path, FileMode.CreateNew, FileAccess.Write)) {
            for (;;) { s.Read(buf, buf.Length, pRead); int n = Marshal.ReadInt32(pRead); if (n <= 0) break; fo.Write(buf, 0, n); } } }
        finally { Marshal.FreeHGlobal(pRead); } } }
'@
    }
    $partMine = $true   # from here on a .part file is this run's own
    [LaesIsoWriter]::Save($img.ImageStream, $partOut)
    $wantLen = [int64]$img.TotalBlocks * $img.BlockSize; $gotLen = (Get-Item -LiteralPath $partOut).Length
    if ($gotLen -ne $wantLen) { throw "the image is incomplete ($gotLen of $wantLen bytes); nothing written" }
    [IO.File]::Move($partOut, [IO.Path]::GetFullPath($Out)); $partMine = $false
    Write-Host ("{0}: {1:N0} MB, {2}, volume {3}" -f $Out, ((Get-Item -LiteralPath $Out).Length / 1MB), $(if ($Disc -like 'BD*') { 'UDF 2.50' } else { 'UDF 1.02 + ISO 9660 + Joliet' }), $volName)
}
finally {   # only the three small files this run made, by name, and this run's unfinished image
    if ($partMine -and (Test-Path -LiteralPath $partOut)) { Remove-Item -LiteralPath $partOut }
    foreach ($nm in 'playlist.m3u8', 'SHA256SUMS.txt', 'README.txt') { $pth = Join-Path $work $nm; if (Test-Path -LiteralPath $pth) { Remove-Item -LiteralPath $pth } }
    Remove-Item -LiteralPath $work
}
