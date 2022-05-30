for shader in assets/*.hlsl; do
    name="${shader##*/}"
    name="${name%.*}"
    dxc -E VSMain -T vs_6_0 ${shader} -Fo "data/${name}.cvert" -Zi -Fd "data/${name}.cvert.pdb"
    dxc -E PSMain -T ps_6_0 ${shader} -Fo "data/${name}.cpixel" -Zi -Fd "data/${name}.pixel.pdb"
done