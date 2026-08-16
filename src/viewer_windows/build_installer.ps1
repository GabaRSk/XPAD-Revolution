param(
    [Parameter(Mandatory = $true)]
    [string]$ViewerRoot
)

$ErrorActionPreference = 'Stop'
$csc = 'C:\Windows\Microsoft.NET\Framework64\v4.0.30319\csc.exe'
if (-not (Test-Path -LiteralPath $csc)) {
    throw 'Compilador .NET Framework não encontrado.'
}
$ViewerRoot = (Resolve-Path -LiteralPath $ViewerRoot).Path
$clientDll = Join-Path $PSScriptRoot 'dependencies\client_1.21.256\lib\netstandard2.0\Nefarius.ViGEm.Client.dll'
$driverInstaller = Join-Path $PSScriptRoot 'dependencies\ViGEmBus_1.22.0_x64_x86_arm64.exe'
if (-not (Test-Path -LiteralPath $clientDll) -or -not (Test-Path -LiteralPath $driverInstaller)) {
    throw 'Dependências oficiais do controle virtual não encontradas em dependencies\.'
}
$build = Join-Path $PSScriptRoot 'build'
if (-not (Test-Path -LiteralPath $build)) {
    New-Item -ItemType Directory -Path $build | Out-Null
}
$payload = Join-Path $build 'payload'
$payloadAssets = Join-Path $payload 'assets'
New-Item -ItemType Directory -Force -Path $payloadAssets | Out-Null
Copy-Item -LiteralPath $clientDll -Destination (Join-Path $payload 'Nefarius.ViGEm.Client.dll') -Force
Copy-Item -LiteralPath $driverInstaller -Destination (Join-Path $payload 'ViGEmBus_1.22.0_x64_x86_arm64.exe') -Force
Copy-Item -LiteralPath (Join-Path $ViewerRoot 'overlay.html'), (Join-Path $ViewerRoot 'TESTAR_OVERLAY.html'), `
    (Join-Path $ViewerRoot 'README_PC.txt'), (Join-Path $ViewerRoot 'OBS_URLS.txt'), `
    (Join-Path $PSScriptRoot 'THIRD_PARTY_NOTICES.txt') -Destination $payload -Force

Push-Location $PSScriptRoot
try {
    & $csc /nologo /target:winexe /optimize+ /platform:anycpu `
        /win32manifest:ViewerApp.manifest `
        /reference:System.dll /reference:System.Core.dll `
        /reference:System.Drawing.dll /reference:System.Windows.Forms.dll `
        /out:build\PS3xPADViewer.exe ViewerApp.cs VirtualGamepad.cs
    if ($LASTEXITCODE -ne 0) { throw 'Falha ao compilar PS3xPADViewer.exe.' }

    $compilerArgs = @(
        '/nologo', '/target:winexe', '/optimize+', '/platform:anycpu',
        '/win32manifest:Installer.manifest',
        '/reference:System.dll', '/reference:System.Core.dll',
        '/reference:System.Drawing.dll', '/reference:System.Windows.Forms.dll',
        '/out:build\Instalar_PS3xPAD_Viewer.exe',
        '/resource:build\PS3xPADViewer.exe,Payload.PS3xPADViewer.exe',
        '/resource:build\payload\Nefarius.ViGEm.Client.dll,Payload.Nefarius.ViGEm.Client.dll',
        '/resource:build\payload\ViGEmBus_1.22.0_x64_x86_arm64.exe,Payload.ViGEmBus_1.22.0_x64_x86_arm64.exe',
        '/resource:build\payload\overlay.html,Payload.overlay.html',
        '/resource:build\payload\TESTAR_OVERLAY.html,Payload.TESTAR_OVERLAY.html',
        '/resource:build\payload\README_PC.txt,Payload.README_PC.txt',
        '/resource:build\payload\OBS_URLS.txt,Payload.OBS_URLS.txt',
        '/resource:build\payload\THIRD_PARTY_NOTICES.txt,Payload.THIRD_PARTY_NOTICES.txt'
    )
    $assets = @(
        'base.png', 'circle_pressed.png', 'cross_pressed.png',
        'dpad_down_pressed.png', 'dpad_left_pressed.png', 'dpad_right_pressed.png', 'dpad_up_pressed.png',
        'l1_pressed.png', 'l2_pressed.png', 'l3_pressed.png', 'left_analog.png', 'options_pressed.png',
        'r1_pressed.png', 'r2_pressed.png', 'r3_pressed.png', 'right_analog.png', 'share_pressed.png',
        'square_pressed.png', 'triangle_pressed.png'
    )
    foreach ($asset in $assets) {
        Copy-Item -LiteralPath (Join-Path $ViewerRoot ('assets\' + $asset)) -Destination (Join-Path $payloadAssets $asset) -Force
        $compilerArgs += '/resource:build\payload\assets\' + $asset + ',Payload.assets.' + $asset
    }
    $compilerArgs += 'Installer.cs'
    & $csc @compilerArgs
    if ($LASTEXITCODE -ne 0) { throw 'Falha ao compilar Instalar_PS3xPAD_Viewer.exe.' }
}
finally {
    Pop-Location
}

Get-Item (Join-Path $build 'PS3xPADViewer.exe'), (Join-Path $build 'Instalar_PS3xPAD_Viewer.exe')
