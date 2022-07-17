$ExitStatus = 0
New-Item -ItemType Directory -Force -Path "build/ShaderDebug/" | Out-Null
Get-ChildItem "assets/" -Filter *.hlsl |
Foreach-Object {
    dxc.exe -HV 2021 -E VSMain -T vs_6_6 $_.FullName -Od -Fo "data/$($_.BaseName).cvert" -Zi -Qembed_debug
    $ExitStatus += $LASTEXITCODE

    $HasPixel = Select-String -Path $_.FullName -Pattern "PSMain"
    if ($HasPixel -ne $null) {
        dxc.exe -HV 2021 -E PSMain -T ps_6_6 $_.FullName -Od -Fo "data/$($_.BaseName).cpixel" -Zi -Qembed_debug
        $ExitStatus += $LASTEXITCODE
    }
}
Get-ChildItem "assets/" -Filter *.hlslc |
Foreach-Object {
    dxc.exe -HV 2021 -E CSMain -T cs_6_6 $_.FullName -Od -Fo "data/$($_.BaseName).ccomp" -Zi -Qembed_debug
}

exit $ExitStatus;