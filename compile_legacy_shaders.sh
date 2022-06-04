for shader in assets/*.hlsl; do
    name="${shader##*/}"
    name="${name%.*}"
    fxc -E VSMain -T vs_5_1 ${shader} -Fo "data/${name}.cvert" -Zi -Zss -Od -Fd "data/"
    fxc -E PSMain -T ps_5_1 ${shader} -Fo "data/${name}.cpixel" -Zi -Zss -Od -Fd "data/"
done