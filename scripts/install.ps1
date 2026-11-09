param([string]$Version,[string]$PackagePath,[switch]$TrustCertificate,[switch]$CheckOnly)
$ErrorActionPreference='Stop'
. (Join-Path $PSScriptRoot 'Install-Helpers.ps1')
$taskRoot=Split-Path $PSScriptRoot -Parent
if(-not $PackagePath){
    if(-not $Version){$Version=& (Join-Path $PSScriptRoot 'Get-AppVersion.ps1')}
    $PackagePath=Join-Path $taskRoot "out/EarbudControls_$Version.msix"
}
$taskPackage=(Resolve-Path -LiteralPath $PackagePath).Path
$taskBundle=Split-Path $taskPackage -Parent
if(Test-Path -LiteralPath (Join-Path $taskBundle 'SHA256SUMS.txt')){Assert-BundleIntegrity $taskBundle}
$taskIdentity=Get-MsixIdentity $taskPackage
if($taskIdentity.Name -ne 'KanuTomer.Bandwave' -or $taskIdentity.Publisher -ne 'CN=C7E2D079-3748-4D25-83B1-3A84650F94BD' -or $taskIdentity.ProcessorArchitecture -ne 'x64'){throw 'Unexpected production package identity.'}
if($Version -and $Version -ne $taskIdentity.Version){throw 'Requested version differs from package version.'}
if([IO.Path]::GetFileName($taskPackage) -ne "EarbudControls_$($taskIdentity.Version).msix"){throw 'Package filename and manifest version differ.'}
$taskCertPath=Join-Path $taskBundle 'EarbudControls.Local.cer'
$taskCertificate=[Security.Cryptography.X509Certificates.X509Certificate2]::new($taskCertPath)
if($taskCertificate.Thumbprint -ne '7BC58E5A7540AE042F493DD007EBAD46E305E050' -or $taskCertificate.Subject -ne $taskIdentity.Publisher){throw 'Unexpected package certificate.'}
if($taskCertificate.NotAfter -le (Get-Date) -or $taskCertificate.NotBefore -gt (Get-Date)){throw 'The release signing certificate is outside its validity period.'}
$taskSignature=Get-AuthenticodeSignature -LiteralPath $taskPackage
if(-not $taskSignature.SignerCertificate -or $taskSignature.SignerCertificate.Thumbprint -ne $taskCertificate.Thumbprint -or [string]$taskSignature.Status -in @('NotSigned','HashMismatch','NotSupportedFileFormat')){throw 'Package signature is missing, damaged, or signed by another certificate.'}
$taskDevelopment=Get-ItemProperty -LiteralPath 'HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\AppModelUnlock' -ErrorAction SilentlyContinue
$taskArchitecture=if($env:PROCESSOR_ARCHITEW6432){$env:PROCESSOR_ARCHITEW6432}else{$env:PROCESSOR_ARCHITECTURE}
$taskProblems=@(Get-SetupProblems -OSBuild ([Environment]::OSVersion.Version.Build) -Architecture $taskArchitecture -DeveloperMode ($taskDevelopment.AllowDevelopmentWithoutDevLicense -eq 1) -WidgetsAvailable ([bool](Get-AppxPackage -Name MicrosoftWindows.Client.WebExperience)))
if($taskProblems.Count){throw ($taskProblems -join "`n")}
$taskNeedsRuntime=Test-RuntimeRequired @(Get-AppxPackage -Name Microsoft.WindowsAppRuntime.2)
$taskDependency=Join-Path $taskBundle 'Dependencies/Microsoft.WindowsAppRuntime.2.msix'
if(Test-Path -LiteralPath $taskDependency){
    $taskRuntimeIdentity=Get-MsixIdentity $taskDependency
    $taskRuntimeSignature=Get-AuthenticodeSignature -LiteralPath $taskDependency
    if($taskRuntimeIdentity.Name -ne 'Microsoft.WindowsAppRuntime.2' -or $taskRuntimeIdentity.ProcessorArchitecture -ne 'x64' -or [version]$taskRuntimeIdentity.Version -lt [version]'2.5.1.0' -or $taskRuntimeSignature.Status -ne 'Valid' -or $taskRuntimeSignature.SignerCertificate.Subject -notmatch 'Microsoft Corporation'){throw 'Invalid Microsoft runtime dependency.'}
}elseif($taskNeedsRuntime){throw 'Required runtime is missing. Extract the complete GitHub release ZIP, including Dependencies.'}
$taskTrusted=Test-Path -LiteralPath "Cert:\LocalMachine\TrustedPeople\$($taskCertificate.Thumbprint)"
Write-Output "Package $($taskIdentity.Version) validated. Runtime installation required: $taskNeedsRuntime. Certificate trust required: $(-not $taskTrusted)."
if($CheckOnly){Write-Output 'CheckOnly: no packages, certificates, settings or devices were changed.';return}
if(-not $taskTrusted){
    if(-not $TrustCertificate){throw 'Run Install.cmd, or rerun install.ps1 with -TrustCertificate to approve certificate-only elevation.'}
    $taskHelper=Join-Path $PSScriptRoot 'Certificate-Trust.ps1'
    $taskPowerShell=Join-Path $env:SystemRoot 'System32/WindowsPowerShell/v1.0/powershell.exe'
    $taskArguments='-NoProfile -ExecutionPolicy Bypass -File "'+$taskHelper+'" -CertificatePath "'+$taskCertPath+'" -Operation Import'
    $taskChild=Start-Process -FilePath $taskPowerShell -ArgumentList $taskArguments -Verb RunAs -WindowStyle Hidden -PassThru
    $taskChild.WaitForExit()
    if($taskChild.ExitCode -ne 0 -or -not(Test-Path -LiteralPath "Cert:\LocalMachine\TrustedPeople\$($taskCertificate.Thumbprint)")){throw 'Certificate trust was cancelled or failed. App was not installed.'}
}
if((Get-AuthenticodeSignature -LiteralPath $taskPackage).Status -ne 'Valid'){throw 'Windows could not validate the trusted package signature.'}
if($taskNeedsRuntime){Add-AppxPackage -Path $taskPackage -DependencyPath $taskDependency -ForceApplicationShutdown}else{Add-AppxPackage -Path $taskPackage -ForceApplicationShutdown}
$taskInstalled=Get-AppxPackage -Name KanuTomer.Bandwave
if(-not $taskInstalled -or [version]$taskInstalled.Version -ne [version]$taskIdentity.Version){throw 'Installed version could not be confirmed.'}
Write-Output "Installed Bandwave $($taskInstalled.Version). Open Win + W, select Add widgets, and add WF-C710N."
Write-Output 'Open Bandwave from Start for the companion controls. Pairing and widget placement are manual.'
Write-Output 'If the picker is stale after enabling Developer Mode, close Win + W and reopen it. A Windows sign-out or restart may be needed.'
