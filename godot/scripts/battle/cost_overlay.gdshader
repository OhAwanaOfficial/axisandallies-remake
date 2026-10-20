// Debug overlay (F1): the nav cost grid of one movement class, one texel per 1-unit cell.
shader_type spatial;
render_mode unshaded, cull_disabled, depth_draw_never, blend_mix;

uniform sampler2D cost_tex : filter_nearest, repeat_disable;  // RGBA colours per cell
uniform vec2 cell_count = vec2(256.0, 256.0);

varying vec3 world_pos;

void vertex() {
	world_pos = (MODEL_MATRIX * vec4(VERTEX, 1.0)).xyz;
}

void fragment() {
	vec4 c = texture(cost_tex, world_pos.xz / cell_count);
	// Thin cell grid lines when zoomed in.
	vec2 g = abs(fract(world_pos.xz) - 0.5);
	float line = step(0.47, max(g.x, g.y));
	ALBEDO = c.rgb;
	ALPHA = c.a + line * 0.08;
}
