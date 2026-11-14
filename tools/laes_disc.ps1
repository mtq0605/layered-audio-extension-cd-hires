# SPDX-FileCopyrightText: 2026 mtq0605
# SPDX-License-Identifier: Apache-2.0
# laes_disc.ps1 (2026-11-03): makes a DVD or BD data-disc image (.iso) of an album of LAES files, with the disc-image service of Windows
# (IMAPI2). A data disc, not BD-Video / BDMV / DVD-Audio (those are licensed formats): any player that reads FLAC (or WAV) files from a
# disc plays the core master (it can be CD-compatible); an LAES player plays the masters. Nothing is converted or changed: the files are copied as they are.
#
#   powershell -ExecutionPolicy Bypass -File tools\laes_disc.ps1 -Album <folder> -Out <image.iso> -Disc BD25 [-Label <name>] [-Export <laes_export.exe>] [--overwrite]
#
# --overwrite (also -Overwrite): decision 21 (2026-11-15): an existing -Out is replaced only after two "yes" answers at a console, or
# with --overwrite; without a console it is refused. The image is still written to <out>.part first and replaces -Out only when complete.
# -Disc: DVD5, DVD9, BD25, BD50 or BD100 (sets the capacity that is checked). BD: UDF 2.50. DVD: UDF 1.02 + ISO 9660 + Joliet; when a
# file is larger than 2 GiB, UDF 1.02 only (the Windows disc-image service puts no file above 2 GiB into ISO 9660 / Joliet).
# Decision 46 (2026-11-15): files of any size are taken; only the total size is checked against the capacity of the disc type.
# Every LAES file (*.flac, *.wav) of the folder is opened once with laes_export --list (file SHA-256, CRC-32, core master); a file that is not
# LAES or fails a check stops the run. The disc holds:
#   <Label>\<files>        the LAES files, in name order
#   <Label>\playlist.m3u8  the files in that order (relative paths, UTF-8)
#   <Label>\SHA256SUMS.txt the SHA-256 of every file (sha256sum format), for checking a copy
#   README.txt             what the disc is and how to play it
[CmdletBinding(PositionalBinding = $false)]
param([Parameter(Mandatory = $true)][string]$Album, [Parameter(Mandatory = $true)][string]$Out, [Parameter(Mandatory = $true)][ValidateSet('DVD5', 'DVD9', 'BD25', 'BD50', 'BD100')][string]$Disc,
      [string]$Label = '', [string]$Export = '', [switch]$Overwrite, [Parameter(ValueFromRemainingArguments = $true)][string[]]$Rest)
$ErrorActionPreference = 'Stop'
foreach ($restArg in @($Rest)) { if ($restArg -eq '--overwrite') { $Overwrite = $true } elseif ($restArg) { throw "unknown argument $restArg" } }
$blocksOf = @{ DVD5 = 2295104; DVD9 = 4173824; BD25 = 12219392; BD50 = 24438784; BD100 = 48878592 }   # 2048-byte sectors (single / dual layer)
if (-not (Test-Path -LiteralPath $Album -PathType Container)) { throw "no folder $Album" }
if ($Label -eq '') { $Label = Split-Path -Leaf (Resolve-Path -LiteralPath $Album)
    if ($Label -match '[:\\/]') { throw "$Album is the root of a drive: give the disc's name with -Label <name>" } }   # FU20-6: 'C:\' is no folder name
if ($Export -eq '') { $Export = Join-Path $PSScriptRoot '..\build\laes_export.exe' }
if (-not (Test-Path -LiteralPath $Export)) { throw "laes_export.exe not found ($Export); give -Export" }
if (Test-Path -LiteralPath $Out) {   # DEC 21: the same questions and refusal as every LAE tool (English, then Chinese)
    $zhAsk1 = [regex]::Unescape('\u5df2\u7ecf\u5b58\u5728\u3002\u8981\u66ff\u6362\u5417\uff1f\u8f93\u5165 yes \u7ee7\u7eed\uff1a')
    $zhAsk2a = [regex]::Unescape('\u786e\u5b9a\u8981\u66ff\u6362 '); $zhAsk2b = [regex]::Unescape(' \u5417\uff1f\u66ff\u6362\u540e\u65e0\u6cd5\u6062\u590d\u3002\u518d\u8f93\u5165\u4e00\u6b21 yes\uff1a')
    $zhRefuse = [regex]::Unescape('\u5df2\u7ecf\u5b58\u5728\uff0c\u6ca1\u6709\u66ff\u6362\uff08\u52a0 --overwrite \u624d\u66ff\u6362\uff09\u3002')
    $agreed = [bool]$Overwrite
    if (-not $agreed -and [Environment]::UserInteractive -and -not [Console]::IsInputRedirected -and -not [Console]::IsOutputRedirected) {
        Write-Host "`"$Out`" already exists. Replace it? Type yes to continue: "; Write-Host -NoNewline "`"$Out`" $zhAsk1"
        if ("$([Console]::ReadLine())".Trim() -eq 'yes') {
            Write-Host "Really replace `"$Out`"? It cannot be undone. Type yes again: "; Write-Host -NoNewline "$zhAsk2a`"$Out`"$zhAsk2b"
            $agreed = "$([Console]::ReadLine())".Trim() -eq 'yes' } }
    if (-not $agreed) { Write-Host "`"$Out`" already exists; it was not replaced (give --overwrite to replace it)."; Write-Host "`"$Out`" $zhRefuse"; exit 1 } }
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
    # DEC 46: IMAPI2 refuses a file above 2 GiB in ISO 9660 / Joliet ("too large for the ISO9660/Joliet file system", tested 2026-11-15),
    # so a DVD with such a file gets UDF 1.02 alone; every other DVD is made as before
    $bigTracks = @($tracks | Where-Object { $_.Length -gt 2147483648 })
    $udfOnly = ($Disc -like 'DVD*') -and ($bigTracks.Count -gt 0)
    if ($udfOnly) { Write-Host ("note: {0} is larger than 2 GiB, which ISO 9660 / Joliet cannot hold: the {1} image holds UDF 1.02 only; a player that reads only ISO 9660 does not see the files" -f $bigTracks[0].Name, $Disc) }
    if ($Disc -like 'BD*') { $fsi.FileSystemsToCreate = 4; $fsi.UDFRevision = 0x250 } elseif ($udfOnly) { $fsi.FileSystemsToCreate = 4; $fsi.UDFRevision = 0x102 } else { $fsi.FileSystemsToCreate = 7; $fsi.UDFRevision = 0x102 }   # 4 UDF; 7 ISO 9660 + Joliet + UDF
    # volume name: DVD (ISO 9660) only letters, digits and _; BD (UDF only) keeps the name as it is (FU20-6: a Chinese name was '___')
    if ($Disc -like 'BD*') { $volName = $Label } else { $volName = ($Label -replace '[^A-Za-z0-9_]', '_') }
    if ($volName.Length -gt 32) { $volName = $volName.Substring(0, 32) }
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
    if (Test-Path -LiteralPath $Out) { [IO.File]::Replace($partOut, [IO.Path]::GetFullPath($Out), [NullString]::Value) } else { [IO.File]::Move($partOut, [IO.Path]::GetFullPath($Out)) }   # DEC 21: replaced only when agreed above
    $partMine = $false
    Write-Host ("{0}: {1:N0} MB, {2}, volume {3}" -f $Out, ((Get-Item -LiteralPath $Out).Length / 1MB), $(if ($Disc -like 'BD*') { 'UDF 2.50' } elseif ($udfOnly) { 'UDF 1.02' } else { 'UDF 1.02 + ISO 9660 + Joliet' }), $volName)
}
finally {   # only the three small files this run made, by name, and this run's unfinished image
    if ($partMine -and (Test-Path -LiteralPath $partOut)) { Remove-Item -LiteralPath $partOut }
    foreach ($nm in 'playlist.m3u8', 'SHA256SUMS.txt', 'README.txt') { $pth = Join-Path $work $nm; if (Test-Path -LiteralPath $pth) { Remove-Item -LiteralPath $pth } }
    Remove-Item -LiteralPath $work
}
