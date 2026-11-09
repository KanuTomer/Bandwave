param([string]$Version,[switch]$UserApprovedUi)
$ErrorActionPreference='Stop'
$taskRoot=Split-Path $PSScriptRoot -Parent
if(-not $Version){$Version=& (Join-Path $PSScriptRoot 'Get-AppVersion.ps1')}
if($Version -ne (& (Join-Path $PSScriptRoot 'Get-AppVersion.ps1'))){throw 'Package version must match the production manifest.'}
& (Join-Path $PSScriptRoot 'Test-UiReviewGate.ps1') -UserApprovedUi:$UserApprovedUi
$taskExe=Join-Path $taskRoot 'build\bin\Release\EarbudControls.exe'
if(-not(Test-Path -LiteralPath $taskExe)){throw 'Run scripts/build.ps1 first.'}
$taskSdk=Join-Path ${env:ProgramFiles(x86)} 'Windows Kits\10\bin\10.0.26100.0\x64'
$taskStage=Join-Path $taskRoot 'out\package'
$taskAssets=Join-Path $taskStage 'Assets'
New-Item -Path $taskAssets,(Join-Path $taskStage 'Public') -ItemType Directory -Force | Out-Null
Copy-Item -LiteralPath $taskExe -Destination $taskStage -Force
# The Dusk app: its executable, loose XAML and earbud artwork. The package's framework dependency
# provides the Windows App Runtime, so its unpackaged bootstrapper DLL is not shipped.
$taskUiExe=Join-Path $taskRoot 'build\bin\Release\EarbudControls.UI.exe'
if(-not(Test-Path -LiteralPath $taskUiExe)){throw 'Run scripts/build.ps1 first: EarbudControls.UI.exe is missing.'}
Copy-Item -LiteralPath $taskUiExe,(Join-Path $taskRoot 'ui\Dusk.xaml') -Destination $taskStage -Force
Copy-Item -Path (Join-Path $taskRoot 'ui\Assets\*.png') -Destination $taskAssets -Force
Copy-Item -LiteralPath (Join-Path $taskRoot 'assets/icons/LUCIDE-LICENSE.txt') -Destination $taskStage -Force
$taskProbeBin=Join-Path $taskRoot 'build\bin\Release'
Copy-Item -LiteralPath (Join-Path $taskProbeBin 'SonyHeadphonesClient-LICENSE.txt'),(Join-Path $taskProbeBin 'fmt-LICENSE.txt') -Destination $taskStage -Force
Copy-Item -LiteralPath (Join-Path $taskRoot '.deps\nuget\widgets\license.txt') -Destination (Join-Path $taskStage 'WindowsAppSDK-LICENSE.txt') -Force
$taskManifest=[xml](Get-Content -LiteralPath (Join-Path $taskRoot 'packaging\AppxManifest.xml') -Raw)
$taskManifest.Package.Identity.Version=[version]::Parse($Version).ToString()
$taskManifest.Save((Join-Path $taskStage 'AppxManifest.xml'))

# Generate original application icons and a clearly illustrative picker preview.
Add-Type -AssemblyName System.Drawing
& (Join-Path $PSScriptRoot 'export-icons.ps1') -Destination (Join-Path $taskStage 'Public\Icons')
# The app icon: one earbud outline on black.
Remove-Item -Path (Join-Path $taskAssets 'Logo*.png') -Force -ErrorAction SilentlyContinue
& (Join-Path $PSScriptRoot 'export-app-icon.ps1') -Package $taskAssets
$taskBitmap=[System.Drawing.Bitmap]::new(600,400)
$taskGraphics=[System.Drawing.Graphics]::FromImage($taskBitmap)
$taskGraphics.Clear([System.Drawing.ColorTranslator]::FromHtml('#24262F'))
$taskFont=[System.Drawing.Font]::new('Segoe UI',[single]19)
$taskBrush=[System.Drawing.SolidBrush]::new([System.Drawing.Color]::White)
$taskGraphics.DrawString("WF-C710N Controls",$taskFont,$taskBrush,[single]28,[single]24)
$taskSmallFont=[System.Drawing.Font]::new('Segoe UI',[single]13)
$taskGraphics.DrawString("L 90%     R 0%     Case 50%",$taskSmallFont,$taskBrush,[single]30,[single]76)
foreach($taskMode in @(@('noise-cancelling','Noise cancelling'),@('ambient','Ambient sound'),@('off','Off'))){
    $taskIndex=[array]::IndexOf(@('noise-cancelling','ambient','off'),$taskMode[0]);$taskX=28+$taskIndex*182
    $taskPanel=[System.Drawing.SolidBrush]::new([System.Drawing.ColorTranslator]::FromHtml('#36383D'))
    $taskGraphics.FillRectangle($taskPanel,$taskX,116,170,132);$taskPanel.Dispose()
    $taskIcon=[System.Drawing.Bitmap]::FromFile((Join-Path $taskStage "Public\Icons\$($taskMode[0])-dark.png"))
    $taskGraphics.DrawImage($taskIcon,($taskX+61),132,48,48);$taskIcon.Dispose()
    $taskGraphics.DrawString($taskMode[1],$taskSmallFont,$taskBrush,[single]($taskX+8),[single]204)
}
$taskGraphics.DrawString("Equalizer · Custom 2   ›",$taskFont,$taskBrush,[single]30,[single]276)
$taskGraphics.DrawString("Presets and stepped EQ inside this widget",$taskSmallFont,$taskBrush,[single]30,[single]318)
$taskGraphics.DrawString("Illustrative preview",$taskSmallFont,$taskBrush,[single]30,[single]364)
$taskBitmap.Save((Join-Path $taskAssets 'Preview.png'),[System.Drawing.Imaging.ImageFormat]::Png)
$taskSmallFont.Dispose();$taskBrush.Dispose();$taskFont.Dispose();$taskGraphics.Dispose();$taskBitmap.Dispose()
Copy-Item -LiteralPath (Join-Path $taskAssets 'StoreLogo.png') -Destination (Join-Path $taskStage 'Public\Icon.png') -Force

# resources.pri indexes the logo variants; without it Windows puts the taskbar icon on a plate.
$taskPri=Join-Path $taskRoot 'out\priconfig.xml'
& (Join-Path $taskSdk 'makepri.exe') createconfig /cf $taskPri /dq en-US /pv 10.0.0 /o | Out-Null
if($LASTEXITCODE -ne 0){throw 'makepri createconfig failed.'}
& (Join-Path $taskSdk 'makepri.exe') new /pr $taskStage /cf $taskPri /mn (Join-Path $taskStage 'AppxManifest.xml') /of (Join-Path $taskStage 'resources.pri') /o | Out-Null
if($LASTEXITCODE -ne 0){throw 'makepri new failed.'}
$taskCert=Get-ChildItem Cert:\CurrentUser\My | Where-Object {$_.Subject -eq 'CN=C7E2D079-3748-4D25-83B1-3A84650F94BD' -and $_.HasPrivateKey -and $_.NotAfter -gt (Get-Date).AddDays(7)} | Sort-Object NotAfter -Descending | Select-Object -First 1
if(-not $taskCert){$taskCert=New-SelfSignedCertificate -Type CodeSigningCert -Subject 'CN=C7E2D079-3748-4D25-83B1-3A84650F94BD' -CertStoreLocation Cert:\CurrentUser\My -KeyExportPolicy NonExportable -NotAfter (Get-Date).AddYears(1)}
$taskOut=Join-Path $taskRoot 'out'
$taskPackage=Join-Path $taskOut "EarbudControls_$Version.msix"
& (Join-Path $taskSdk 'makeappx.exe') pack /d $taskStage /p $taskPackage /o
if($LASTEXITCODE -ne 0){throw 'MSIX validation/packaging failed.'}
& (Join-Path $taskSdk 'signtool.exe') sign /fd SHA256 /sha1 $taskCert.Thumbprint /s My $taskPackage
if($LASTEXITCODE -ne 0){throw 'MSIX signing failed.'}
Export-Certificate -Cert $taskCert -FilePath (Join-Path $taskOut 'EarbudControls.Local.cer') -Force | Out-Null
Write-Output "Signed package: $taskPackage"
Write-Output 'The private signing key stays in the Windows certificate store; no PFX is exported.'

