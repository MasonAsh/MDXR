New-Item -ItemType Directory -Force -Path "build/ShaderDebug/" | Out-Null
Get-ChildItem "assets/" -Filter *.hlsl |
Foreach-Object {
    dxc.exe -E VSMain -T vs_6_0 $_.FullName -Fo "data/$($_.BaseName).cvert" -Zi -Zss -Od -Fd "build/ShaderDebug/"
    dxc.exe -E PSMain -T ps_6_0 $_.FullName -Fo "data/$($_.BaseName).cpixel" -Zi -Zss -Od -Fd "build/ShaderDebug/"
}