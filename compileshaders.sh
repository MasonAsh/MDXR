for shader in assets/*.hlsl; do
    name="${shader##*/}"
    name="${name%.*}"
    dxc -E VSMain -T vs_6_0 ${shader} -Fo "data/${name}.cvert" -Zi -Zss -Od -Fd "data/"
    dxc -E PSMain -T ps_6_0 ${shader} -Fo "data/${name}.cpixel" -Zi -Zss -Od -Fd "data/"
done