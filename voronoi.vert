#version 450

layout(location = 0) in vec2 seed_pos;
layout(location = 1) in vec4 seed_color;

layout(location = 0) out vec2 seed;
layout(location = 1) out vec4 color;

void main(void)
{
    vec2 uv;

    uv.x = float(gl_VertexIndex & 1);
    uv.y = float((gl_VertexIndex >> 1) & 1);

    gl_Position = vec4(uv * 2.0 - 1.0, 0.0, 1.0);

    seed = seed_pos;
    color = seed_color;
}
