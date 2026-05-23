#version 450
#extension GL_ARB_shading_language_include : require
#include "voronoi_seed.h"

layout(location = 0) out vec4 out_color;


struct Seed {
    vec4 pos;
    vec4 color;
};

layout(set = 2, binding = 0, std430) readonly buffer SeedBuffer
{
    Seed seeds[];
} seed_buffer;

void main()
{
    vec2 p = gl_FragCoord.xy;

    float best1 = 1e30;
    float best2 = 1e30;

    vec2 best_pos = vec2(0.0);
    vec2 second_pos = vec2(0.0);

    vec3 best_color = vec3(0.02, 0.03, 0.06);

    for (int i = 0; i < VORONOI_SEED_COUNT; i++) {

        Seed s = seed_buffer.seeds[i];

        vec2 d = p - s.pos.xy;
        float dist2 = dot(d, d);

        if (dist2 < best1) {
            best2 = best1;
            second_pos = best_pos;

            best1 = dist2;
            best_pos = s.pos.xy;
            best_color = s.color.rgb;
        } else if (dist2 < best2) {
            best2 = dist2;
            second_pos = s.pos.xy;
        }
    }

    float seed_dist = max(length(second_pos - best_pos), 1.0);
    float edge_dist_px = abs(best2 - best1) / (2.0 * seed_dist);

    float line_width_px = 0.0001;

    float line = 1.0 - smoothstep(line_width_px, line_width_px + 1.0, edge_dist_px);

    vec3 line_color = vec3(0.015, 0.020, 0.035);
    vec3 color = mix(best_color, line_color, line);

    out_color = vec4(color, 1.0);
}