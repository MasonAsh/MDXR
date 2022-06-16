New-Item -ItemType Directory -Force -Path "build/ShaderDebug/" | Out-Null
Get-ChildItem "assets/" -Filter *.hlsl |
Foreach-Object {
    dxc.exe -HV 2021 -E VSMain -T vs_6_6 $_.FullName -Od -Fo "data/$($_.BaseName).cvert" -Zi
    dxc.exe -HV 2021 -E PSMain -T ps_6_6 $_.FullName -Od -Fo "data/$($_.BaseName).cpixel" -Zi
    # dxc.exe -HV 2021 -E VSMain -T vs_6_6 $_.FullName -Fo "data/$($_.BaseName).cvert" -Zi -Fd "build/ShaderDebug/"
    # dxc.exe -HV 2021 -E PSMain -T ps_6_6 $_.FullName -Fo "data/$($_.BaseName).cpixel" -Zi -Fd "build/ShaderDebug/"
}