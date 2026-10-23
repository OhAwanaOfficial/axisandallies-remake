// Batched units (UnitBatches): a MultiMesh instance per unit, animated from a vertex animation
// texture. VAT row = frame, column = vertex (UV2.x); RGB = position in unit space, A = the
// octahedral normal as two 12-bit halves. INSTANCE_CUSTOM: x = moving, y = phase 0..1.
// Beyond anim_lod_distance from the camera the animation steps frame by frame (one fetch per
// vertex instead of two). UnitBatches swaps the render mode below for the original material's cull
// mode, plus vertex_lighting on Low.
shader_type spatial;
render_mode cull_back;

uniform sampler2D albedo_tex : source_color, filter_linear_mipmap, repeat_enable;
uniform vec4 albedo : source_color = vec4(1.0);
uniform bool use_tex = true;
uniform sampler2D vat : filter_nearest, repeat_disable;
uniform vec4 clip_move = vec4(0.0, 1.0, 1.0, 0.0);  // first row, frames, frames per second
uniform vec4 clip_idle = vec4(0.0, 1.0, 1.0, 0.0);
uniform float anim_lod_distance = 120.0;

vec3 unpack_normal(float w) {
	float u = floor(w / 4096.0);
	vec2 p = vec2(u, w - u * 4096.0) / 4095.0 * 2.0 - 1.0;
	vec3 n = vec3(p.x, 1.0 - abs(p.x) - abs(p.y), p.y);
	if (n.y < 0.0) {
		n.xz = (1.0 - abs(n.zx)) * (step(0.0, n.xz) * 2.0 - 1.0);
	}
	return normalize(n);
}

void vertex() {
	int col = int(UV2.x + 0.5);
	vec4 clip = INSTANCE_CUSTOM.x > 0.5 ? clip_move : clip_idle;
	float fr = mod(TIME * clip.z + INSTANCE_CUSTOM.y * clip.y, clip.y);
	int f0 = int(fr);
	vec4 a = texelFetch(vat, ivec2(col, int(clip.x) + f0), 0);
	if (length(MODEL_MATRIX[3].xyz - CAMERA_POSITION_WORLD) < anim_lod_distance) {
		int f1 = f0 + 1 < int(clip.y) ? f0 + 1 : 0;
		vec4 b = texelFetch(vat, ivec2(col, int(clip.x) + f1), 0);
		float k = fr - float(f0);
		VERTEX = mix(a.xyz, b.xyz, k);
		NORMAL = normalize(mix(unpack_normal(a.w), unpack_normal(b.w), k));
	} else {
		VERTEX = a.xyz;
		NORMAL = unpack_normal(a.w);
	}
}

void fragment() {
	vec4 c = albedo;
	if (use_tex) {
		c *= texture(albedo_tex, UV);
	}
	ALBEDO = c.rgb;
	ROUGHNESS = 1.0;
}
