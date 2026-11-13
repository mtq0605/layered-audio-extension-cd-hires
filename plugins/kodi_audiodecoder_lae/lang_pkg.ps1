# SPDX-FileCopyrightText: 2026 mtq0605
# SPDX-License-Identifier: Apache-2.0
# usage: lang_pkg.ps1 <package folder audiodecoder.lae> <en | zh>
# Makes the add-on folder a standalone language version (English and Chinese builds, each on its own):
#   en: only the English texts (resource.language.en_gb, the en_GB summary and description);
#   zh: the Chinese texts in resource.language.en_gb (Kodi shows that file whatever its own language is) and in the summary and description.
# Called by build_kodi.cmd after the add-on folder was copied; the sources keep both languages.
param([Parameter(Mandatory)][string]$PkgDir, [Parameter(Mandatory)][ValidateSet('en', 'zh')][string]$Lang)
$ErrorActionPreference = 'Stop'
$langRoot = Join-Path $PkgDir 'resources\language'
$enFile = Join-Path $langRoot 'resource.language.en_gb\strings.po'
$zhDir = Join-Path $langRoot 'resource.language.zh_cn'
$xmlFile = Join-Path $PkgDir 'addon.xml'
$utf8 = New-Object System.Text.UTF8Encoding($false)
$xmlLines = [System.IO.File]::ReadAllLines($xmlFile, $utf8)
if ($Lang -eq 'zh') {
    # en_gb strings.po from the Chinese file: each msgid takes the Chinese text, msgstr stays empty (Kodi shows the msgid of en_gb)
    $zhLines = [System.IO.File]::ReadAllLines((Join-Path $zhDir 'strings.po'), $utf8)
    $outLines = New-Object System.Collections.Generic.List[string]
    for ($lineNo = 0; $lineNo -lt $zhLines.Count; $lineNo++) {
        $poLine = $zhLines[$lineNo]
        if ($poLine -like 'msgid "*' -and $poLine -ne 'msgid ""' -and $lineNo + 1 -lt $zhLines.Count -and $zhLines[$lineNo + 1] -like 'msgstr "*') {
            $outLines.Add('msgid ' + $zhLines[$lineNo + 1].Substring(7)); $outLines.Add('msgstr ""'); $lineNo++ }
        elseif ($poLine -eq '"Language: zh_CN\n"') { $outLines.Add('"Language: en_GB\n"') }
        else { $outLines.Add($poLine) } }
    [System.IO.File]::WriteAllLines($enFile, $outLines, $utf8)
    # addon.xml: the en_GB summary and description get the Chinese texts
    $zhSummary = ($xmlLines | Where-Object { $_ -match '<summary lang="zh_CN">' }) -replace 'lang="zh_CN"', 'lang="en_GB"'
    $zhDescription = ($xmlLines | Where-Object { $_ -match '<description lang="zh_CN">' }) -replace 'lang="zh_CN"', 'lang="en_GB"'
    $xmlLines = $xmlLines | ForEach-Object { if ($_ -match '<summary lang="en_GB">') { $zhSummary } elseif ($_ -match '<description lang="en_GB">') { $zhDescription } else { $_ } } }
$xmlLines = $xmlLines | Where-Object { $_ -notmatch '<(summary|description) lang="zh_CN">' }
[System.IO.File]::WriteAllLines($xmlFile, [string[]]$xmlLines, $utf8)
Remove-Item -LiteralPath $zhDir -Recurse -Force   # inside the package copy only
"language version: $Lang"
