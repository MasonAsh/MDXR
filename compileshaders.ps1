$ExitStatus = 0
$CompileFlags = '-Zi', '-Qembed_debug', '-Od'
New-Item -ItemType Directory -Force -Path "build/ShaderDebug/" | Out-Null
Get-ChildItem "assets/" -Filter *.hlsl |
Foreach-Object {
    dxc.exe -HV 2021 -E VSMain -T vs_6_6 $_.FullName -Fo "data/$($_.BaseName).cvert" $CompileFlags
    $ExitStatus += $LASTEXITCODE

    $HasPixel = Select-String -Path $_.FullName -Pattern "PSMain"
    if ($HasPixel -ne $null) {
        dxc.exe -HV 2021 -E PSMain -T ps_6_6 $_.FullName -Fo "data/$($_.BaseName).cpixel" $CompileFlags
        $ExitStatus += $LASTEXITCODE
    }
}
Get-ChildItem "assets/" -Filter *.hlslc |
Foreach-Object {
    dxc.exe -HV 2021 -E CSMain -T cs_6_6 $_.FullName -Fo "data/$($_.BaseName).ccomp" $CompileFlags
}

Get-ChildItem "assets/" -Filter *.hlsllib |
Foreach-Object {
    dxc.exe -HV 2021 -T lib_6_6 $_.FullName -Fo "data/$($_.BaseName).clib" $CompileFlags
}

exit $ExitStatus;