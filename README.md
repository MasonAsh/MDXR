# MDXR

MDXR is my D3D12 deferred rendering engine written in C++. It features PBR
shading, HDR environment lighting, and ray traced shadows.

# Showcase

## PBR Shading

MDXR uses PBR shading. PBR textures are loaded directly from GLTF models,
including base color, normal, and combined occlusion/metal/roughness textures.

[PBR shading](docs/pbr-flight-helmet.png)

## HDR Environment Lighting

MDXR is able to load HDR cubemaps and render the scene with specular and diffuse
IBL.

[HDR environment lighting demo](docs/orbit-environment.mp4)

## Deferred Rendering

MDXR uses a deferred rendering architecture. The scene is rendered to base
color, normal, occlusion/metal/roughness, and depth gbuffers and lighting is
computed in a seperate pass.

[HDR environment lighting demo](docs/deferred-showcase.jpg)

## Ray Traced Shadows

MDXR uses inline raytracing queries from DXR 1.1 to power its shadows. It can
render dynamic shadows for point and directional lights.

[Point Shadows](docs/point-shadows.mp4)

[Directional Shadows](docs/directional-shadows.mp4)

## Bloom

MDXR achieves bloom by filtering pixels above a luminosity threshold and
applying a blur to those pixels. This will make pixels too bright to be
accurately represented by the monitor glow.

[Bloom](docs/bloom.png)
