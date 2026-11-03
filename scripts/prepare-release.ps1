param([switch]$UserApprovedUi,[switch]$SkipPackage)
$ErrorActionPreference='Stop'
$taskRoot=Split-Path $PSScriptRoot -Parent
. (Join-Path $PSScriptRoot 'Install-Helpers.ps1')
$taskVersion=& (Join-Path $PSScriptRoot 'Get-AppVersion.ps1')
$taskDisplay=([version]$taskVersion).ToString(3)
if(-not $SkipPackage){& (Join-Path $PSScriptRoot 'package.ps1') -UserApprovedUi:$UserApprovedUi}
$taskRuntimeZip=Join-Path $taskRoot '.deps/Microsoft.WindowsAppRuntime.Redist.2.5.zip'
if(-not(Test-Path -LiteralPath $taskRuntimeZip)){
    Invoke-WebRequest -Uri 'https://aka.ms/windowsappsdk/2.5/2.5.1/Microsoft.WindowsAppRuntime.Redist.2.5.zip' -OutFile $taskRuntimeZip
}
if((Get-FileHash -LiteralPath $taskRuntimeZip -Algorithm SHA256).Hash -ne '3C6D3B4D80A194BB4F9A9A91E00F074B7D539929AB53FB7DDE3EF9D8A21DDBB4'){throw 'Official runtime redistributable hash differs from the reviewed 2.5.1 download.'}
$taskStage=Join-Path $taskRoot ('out/release-stage-'+[guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $taskStage,(Join-Path $taskStage 'scripts'),(Join-Path $taskStage 'Dependencies'),(Join-Path $taskStage 'Licenses') | Out-Null
$taskPackageName="EarbudControls_$taskVersion.msix"
Copy-Item -LiteralPath (Join-Path $taskRoot "out/$taskPackageName"),(Join-Path $taskRoot 'out/EarbudControls.Local.cer'),(Join-Path $taskRoot 'LICENSE'),(Join-Path $taskRoot 'THIRD_PARTY_NOTICES.txt') -Destination $taskStage
foreach($taskScript in 'install.ps1','Install-Helpers.ps1','Certificate-Trust.ps1'){
    Copy-Item -LiteralPath (Join-Path $PSScriptRoot $taskScript) -Destination (Join-Path $taskStage 'scripts')
}
$taskCmd=([IO.File]::ReadAllText((Join-Path $taskRoot 'packaging/Install.cmd'))).Replace('__PACKAGE__',$taskPackageName)
[IO.File]::WriteAllText((Join-Path $taskStage 'Install.cmd'),$taskCmd.Replace("`r`n","`n").Replace("`n","`r`n"),[Text.Encoding]::ASCII)
Copy-Item -LiteralPath (Join-Path $taskRoot 'packaging/README-install.txt') -Destination (Join-Path $taskStage 'README.txt')
foreach($taskLicense in 'SonyHeadphonesClient-LICENSE.txt','fmt-LICENSE.txt','WindowsAppSDK-LICENSE.txt','LUCIDE-LICENSE.txt'){
    Copy-Item -LiteralPath (Join-Path $taskRoot "out/package/$taskLicense") -Destination (Join-Path $taskStage 'Licenses')
}
Copy-Item -LiteralPath (Join-Path $taskStage 'Licenses/WindowsAppSDK-LICENSE.txt') -Destination (Join-Path $taskStage 'Dependencies')
Add-Type -AssemblyName System.IO.Compression.FileSystem
$taskArchive=[IO.Compression.ZipFile]::OpenRead($taskRuntimeZip)
try{
    $taskEntry=$taskArchive.GetEntry('MSIX/win10-x64/Microsoft.WindowsAppRuntime.2.msix')
    if(-not $taskEntry){throw 'Official x64 framework missing from redistributable.'}
    [IO.Compression.ZipFileExtensions]::ExtractToFile($taskEntry,(Join-Path $taskStage 'Dependencies/Microsoft.WindowsAppRuntime.2.msix'))
}finally{$taskArchive.Dispose()}
$taskRuntime=Join-Path $taskStage 'Dependencies/Microsoft.WindowsAppRuntime.2.msix'
$taskRuntimeIdentity=Get-MsixIdentity $taskRuntime
$taskSignature=Get-AuthenticodeSignature -LiteralPath $taskRuntime
if($taskRuntimeIdentity.Name -ne 'Microsoft.WindowsAppRuntime.2' -or $taskRuntimeIdentity.ProcessorArchitecture -ne 'x64' -or $taskRuntimeIdentity.Version -ne '2.5.1.0' -or $taskSignature.Status -ne 'Valid' -or $taskSignature.SignerCertificate.Subject -notmatch 'Microsoft Corporation'){throw 'Official runtime package identity or signature failed.'}
$taskSums=foreach($taskFile in Get-ChildItem -LiteralPath $taskStage -Recurse -File | Sort-Object FullName){
    (Get-FileHash -LiteralPath $taskFile.FullName -Algorithm SHA256).Hash+'  '+$taskFile.FullName.Substring($taskStage.Length+1).Replace('\','/')
}
$taskSums | Set-Content -LiteralPath (Join-Path $taskStage 'SHA256SUMS.txt') -Encoding ascii
Assert-BundleIntegrity $taskStage
& (Join-Path $taskStage 'scripts/install.ps1') -PackagePath (Join-Path $taskStage $taskPackageName) -CheckOnly
# The single-file installer: the verified package, runtime and certificate embedded in one exe.
$taskPayload=Join-Path $taskRoot ('out/setup-payload-'+[guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $taskPayload | Out-Null
Copy-Item -LiteralPath (Join-Path $taskStage $taskPackageName) -Destination (Join-Path $taskPayload 'EarbudControls.msix')
Copy-Item -LiteralPath (Join-Path $taskStage 'EarbudControls.Local.cer'),$taskRuntime -Destination $taskPayload
$taskVswhere=Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
$taskVs=& $taskVswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
$taskCmake=Join-Path $taskVs 'Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe'
$taskSetupBuild=Join-Path $taskRoot 'build/setup'
& $taskCmake -S (Join-Path $taskRoot 'setup') -B $taskSetupBuild -G 'Visual Studio 17 2022' -A x64 "-DEARBUD_PAYLOAD=$taskPayload" "-DEARBUD_VERSION=$taskDisplay"
if($LASTEXITCODE -ne 0){throw 'Setup configuration failed.'}
& $taskCmake --build $taskSetupBuild --config Release
if($LASTEXITCODE -ne 0){throw 'Setup build failed.'}
$taskSetup=Join-Path $taskRoot "out/EarbudControls-Setup-$taskDisplay.exe"
Copy-Item -LiteralPath (Join-Path $taskSetupBuild 'bin/Release/EarbudControls-Setup.exe') -Destination $taskSetup -Force
$taskCert=Get-ChildItem Cert:\CurrentUser\My | Where-Object {$_.Thumbprint -eq '458DC5875423917D74F77A536EBBB42720D1B67A' -and $_.HasPrivateKey} | Select-Object -First 1
if(-not $taskCert){throw 'The release signing certificate is not in this user''s certificate store.'}
& (Join-Path ${env:ProgramFiles(x86)} 'Windows Kits\10\bin\10.0.26100.0\x64\signtool.exe') sign /fd SHA256 /sha1 $taskCert.Thumbprint /s My $taskSetup
if($LASTEXITCODE -ne 0){throw 'Setup signing failed.'}
$taskCheck=Join-Path $taskPayload 'check.txt'
$taskChecked=Start-Process -FilePath $taskSetup -ArgumentList '/check',"`"$taskCheck`"" -PassThru -Wait
if($taskChecked.ExitCode -ne 0){throw 'The built setup failed its own payload check.'}
((Get-FileHash -LiteralPath $taskSetup -Algorithm SHA256).Hash+'  '+[IO.Path]::GetFileName($taskSetup)) | Set-Content -LiteralPath ($taskSetup+'.sha256') -Encoding ascii
Write-Output "Setup: $taskSetup"
$taskOutput=Join-Path $taskRoot "out/EarbudControls-$taskDisplay-windows-x64.zip"
if(Test-Path -LiteralPath $taskOutput){Remove-Item -LiteralPath $taskOutput}
[IO.Compression.ZipFile]::CreateFromDirectory($taskStage,$taskOutput,[IO.Compression.CompressionLevel]::Optimal,$false)
((Get-FileHash -LiteralPath $taskOutput -Algorithm SHA256).Hash+'  '+[IO.Path]::GetFileName($taskOutput)) | Set-Content -LiteralPath ($taskOutput+'.sha256') -Encoding ascii
Write-Output "Release ZIP: $taskOutput"
Write-Output "Verified stage: $taskStage"
