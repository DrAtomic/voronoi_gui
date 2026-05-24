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
    vec2 delta = gl_FragCoord.xy - seed;
    float d2 = dot(delta, delta);

    float max_d2 = dot(u.resolution.xy, u.resolution.xy);
    gl_FragDepth = min(d2 / max_d2, 0.999999);
    out_color = color;
}

