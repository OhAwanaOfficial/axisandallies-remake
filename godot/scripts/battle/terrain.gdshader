// Terrain: one texture-array layer per terrain variation, chosen per 2-unit tile from an index
// texture, blended across tile borders. Variations without a cached texture use their data colour.
shader_type spatial;

uniform sampler2DArray layers : source_color, filter_linear_mipmap_anisotropic, repeat_enable;
uniform sampler2D tile_layer : filter_nearest, repeat_disable;  // R8: layer index / 255
uniform vec2 tile_count = vec2(128.0, 128.0);
uniform float tile_size = 2.0;
uniform float layer_repeat[64];   // world units per texture repeat
uniform vec3 layer_color[64];     // data terrain_color
uniform float layer_has_texture[64];
uniform float blend_width : hint_range(0.0, 0.5) = 0.3;

varying vec3 world_pos;

void vertex() {
	world_pos = (MODEL_MATRIX * vec4(VERTEX, 1.0)).xyz;
}

int layer_at(ivec2 t) {
	ivec2 c = clamp(t, ivec2(0), ivec2(tile_count) - ivec2(1));
	return int(round(texelFetch(tile_layer, c, 0).r * 255.0));
}

vec3 layer_rgb(int l, vec2 wp) {
	vec3 tex = texture(layers, vec3(wp / max(layer_repeat[l], 0.001), float(l))).rgb;
	return mix(layer_color[l], tex, layer_has_texture[l]);
}

void fragment() {
	vec2 t = world_pos.xz / tile_size - 0.5;
	ivec2 i0 = ivec2(floor(t));
	vec2 f = fract(t);
	vec2 w = smoothstep(vec2(0.5 - blend_width), vec2(0.5 + blend_width), f);
	vec3 c00 = layer_rgb(layer_at(i0), world_pos.xz);
	vec3 c10 = layer_rgb(layer_at(i0 + ivec2(1, 0)), world_pos.xz);
	vec3 c01 = layer_rgb(layer_at(i0 + ivec2(0, 1)), world_pos.xz);
	vec3 c11 = layer_rgb(layer_at(i0 + ivec2(1, 1)), world_pos.xz);
	ALBEDO = mix(mix(c00, c10, w.x), mix(c01, c11, w.x), w.y);
	ROUGHNESS = 0.95;
	SPECULAR = 0.1;
}
