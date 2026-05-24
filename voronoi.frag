#version 450

layout(set = 3, binding = 0, std140) uniform FragUniforms
{
    vec4 resolution; // x = width, y = height
} u;

layout(location = 0) in vec2 seed;
layout(location = 1) in vec4 color;

layout(location = 0) out vec4 out_color;

void main(void)
{
    float d = length(gl_FragCoord.xy - seed);
    gl_FragDepth = min(d / length(u.resolution.xy), 0.999999);
    out_color = color;
}
