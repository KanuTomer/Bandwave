# Shared source identity for the native review and the packaging gate.
$ErrorActionPreference='Stop'
$taskRoot=Split-Path $PSScriptRoot -Parent
$taskFiles=@((Get-ChildItem -LiteralPath (Join-Path $taskRoot 'src') -Filter 'widget_*.hpp').FullName)
$taskFiles+=Join-Path $taskRoot 'src/app.cpp'
$taskFiles+=Join-Path $taskRoot 'tests/widget_ui_review.cpp'
$taskFiles+=Join-Path $taskRoot 'scripts/export-icons.ps1'
$taskFiles+=Join-Path $taskRoot 'scripts/Convert-SvgPath.ps1'
$taskFiles+=Join-Path $taskRoot 'CMakeLists.txt'
$taskFiles+=Join-Path $taskRoot 'packaging/AppxManifest.xml'
$taskFiles+=(Get-ChildItem -LiteralPath (Join-Path $taskRoot 'assets/icons') -Filter '*.svg').FullName
# The app is reviewed with the widget: its code, layout and artwork are part of the same approval.
$taskFiles+=(Get-ChildItem -LiteralPath (Join-Path $taskRoot 'ui') -File | Where-Object {$_.Extension -in '.cpp','.hpp','.xaml'}).FullName
$taskFiles+=(Get-ChildItem -LiteralPath (Join-Path $taskRoot 'ui/Assets') -Filter '*.png').FullName
$taskRows=foreach($taskFile in ($taskFiles | Sort-Object -Unique)) { ($taskFile.Substring($taskRoot.Length+1).Replace('\','/'))+':'+(Get-FileHash -LiteralPath $taskFile -Algorithm SHA256).Hash }
$taskHasher=[Security.Cryptography.SHA256]::Create()
try { [BitConverter]::ToString($taskHasher.ComputeHash([Text.Encoding]::UTF8.GetBytes(($taskRows -join "`n")))).Replace('-','') } finally { $taskHasher.Dispose() }
