param([switch]$UserApprovedUi)
$ErrorActionPreference='Stop'
$taskRoot=Split-Path $PSScriptRoot -Parent
if($UserApprovedUi){
    # Explicit human approval is a separate release path, never a fabricated
    # pass of the native screenshot checks below. Require the current demo.
    $taskFingerprint=& (Join-Path $PSScriptRoot 'Get-UiReviewFingerprint.ps1')
    $taskSourcePath=Join-Path $taskRoot 'out/ui-review/ReviewSource.json'
    $taskDemoExe=Join-Path $taskRoot 'out/ui-review/WidgetUiReview.exe'
    $taskBuiltExe=Join-Path $taskRoot 'build/bin/Release/EarbudControls.exe'
    if(-not(Test-Path -LiteralPath $taskSourcePath) -or -not(Test-Path -LiteralPath $taskDemoExe) -or -not(Test-Path -LiteralPath $taskBuiltExe)){throw 'Packaging blocked: the approved demo and production build must exist.'}
    $taskSource=Get-Content -LiteralPath $taskSourcePath -Raw | ConvertFrom-Json
    if($taskSource.sourceFingerprint -ne $taskFingerprint -or $taskSource.reviewExeSha256 -ne (Get-FileHash -LiteralPath $taskDemoExe -Algorithm SHA256).Hash){throw 'Packaging blocked: the source differs from the approved demo.'}
    [pscustomobject]@{
        approval='explicit-user-approval';approvedAtUtc=[DateTime]::UtcNow.ToString('o')
        sourceFingerprint=$taskFingerprint;reviewExeSha256=$taskSource.reviewExeSha256
        productionExeSha256=(Get-FileHash -LiteralPath $taskBuiltExe -Algorithm SHA256).Hash
        automatedNativeVisualPass=$false
    } | ConvertTo-Json | Set-Content (Join-Path $taskRoot 'artifacts/native-ui/package-approval.json') -Encoding utf8
    Write-Output 'Packaging authorised by explicit user UI approval; current demo source verified. No automated native visual pass claimed.'
    return
}
$taskGate=Join-Path $taskRoot 'artifacts/native-ui/review-status.json'
if(-not(Test-Path -LiteralPath $taskGate)){throw 'Packaging blocked: native Windows UI scrutiny has not passed. Use register-ui-review.ps1 and review the real widget first.'}
$taskReview=Get-Content -LiteralPath $taskGate -Raw | ConvertFrom-Json
$taskFingerprint=& (Join-Path $PSScriptRoot 'Get-UiReviewFingerprint.ps1')
if($taskReview.status -ne 'passed' -or $taskReview.sourceFingerprint -ne $taskFingerprint){throw 'Packaging blocked: the current UI has not passed native Windows scrutiny. Browser renders and older screenshots cannot approve it.'}
$taskBuiltExe=Join-Path $taskRoot 'build/bin/Release/EarbudControls.exe'
if(-not(Test-Path -LiteralPath $taskBuiltExe) -or $taskReview.productionExeSha256 -ne (Get-FileHash -LiteralPath $taskBuiltExe -Algorithm SHA256).Hash){throw 'Packaging blocked: the executable differs from the reviewed build.'}
foreach($taskSize in 'small','medium','large'){
    $taskViews=@('home') # 0.5: the widget has one view in every size
    foreach($taskView in $taskViews){
        $taskEvidence=@($taskReview.captures | Where-Object {$_.size -eq $taskSize -and $_.view -eq $taskView -and $_.renderer -eq 'windows-widgets' -and $_.result -eq 'passed'})
        if(-not $taskEvidence.Count){throw "Packaging blocked: missing native $taskSize / $taskView review."}
        foreach($taskCapture in $taskEvidence){
            if(-not(Test-Path -LiteralPath $taskCapture.path) -or (Get-FileHash -LiteralPath $taskCapture.path -Algorithm SHA256).Hash -ne $taskCapture.sha256){throw 'Packaging blocked: native capture evidence is missing or changed.'}
        }
    }
}
foreach($taskCheck in 'readable_insets','no_clipped_panels','hover_matches_target','no_obscuring_tooltips','navigation_and_focus'){
    if($taskReview.checks.$taskCheck -ne $true){throw "Packaging blocked: scrutiny check '$taskCheck' has not passed."}
}
Write-Output 'Native Windows UI scrutiny passed for the current source.'
