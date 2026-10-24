// Combat effect sprites (FxRunner): camera-facing quads drawn procedurally, no textures.
// INSTANCE_CUSTOM: x = start (fx_time), y = lifetime (s), z = kind, w = random 0..1.
// Kinds: 0 muzzle flash, 1 fireball, 2 spark, 3 smoke, 4 dust. The instance basis scale is the size.
// Expired sprites collapse to a point. FxRunner swaps the render mode for blend_add (0-2) or
// blend_mix (3-4).
shader_type spatial;
render_mode blend_add, unshaded, depth_draw_never, cull_disabled, shadows_disabled;

global uniform float fx_time;

varying float v_age;
varying float v_kind;
varying float v_seed;

void vertex() {
	float age = (fx_time - INSTANCE_CUSTOM.x) / max(INSTANCE_CUSTOM.y, 0.001);
	v_age = age;
	v_kind = INSTANCE_CUSTOM.z;
	v_seed = INSTANCE_CUSTOM.w;
	float size = length(MODEL_MATRIX[0].xyz);
	float grow = 1.0;
	vec3 lift = vec3(0.0);
	if (v_kind < 0.5) {
		grow = 0.7 + 0.6 * age;
	} else if (v_kind < 1.5) {
		grow = 0.35 + 0.9 * sqrt(clamp(age, 0.0, 1.0));
		lift.y = age * size * 0.4;
	} else if (v_kind < 2.5) {
		grow = 1.0 - 0.5 * age;
	} else if (v_kind < 3.5) {
		grow = 0.5 + 1.3 * age;
		lift.y = age * INSTANCE_CUSTOM.y * 0.9;
	} else {
		grow = 0.6 + 1.0 * age;
		lift.y = age * INSTANCE_CUSTOM.y * 0.3;
	}
	if (age < 0.0 || age > 1.0) {
		grow = 0.0;
	}
	// Billboard: the quad in camera space around the instance origin, rotated by the seed.
	float a = v_seed * 6.2831;
	vec2 q = mat2(vec2(cos(a), sin(a)), vec2(-sin(a), cos(a))) * VERTEX.xy * size * grow;
	vec3 centre = (VIEW_MATRIX * vec4(MODEL_MATRIX[3].xyz + lift, 1.0)).xyz;
	POSITION = PROJECTION_MATRIX * vec4(centre + vec3(q, 0.0), 1.0);
	UV = VERTEX.xy + 0.5;
}

float hash(vec2 p) {
	return fract(sin(dot(p, vec2(127.1, 311.7))) * 43758.5453);
}

float noise(vec2 p) {
	vec2 i = floor(p);
	vec2 f = fract(p);
	vec2 u = f * f * (3.0 - 2.0 * f);
	return mix(mix(hash(i), hash(i + vec2(1.0, 0.0)), u.x), mix(hash(i + vec2(0.0, 1.0)), hash(i + vec2(1.0, 1.0)), u.x), u.y);
}

void fragment() {
	vec2 p = UV * 2.0 - 1.0;
	float r = length(p);
	float age = clamp(v_age, 0.0, 1.0);
	float n = noise(p * 3.0 + v_seed * 17.0);
	vec3 col;
	float alpha;
	if (v_kind < 0.5) {
		// Star-shaped flash: bright core, rays, gone fast.
		float ang = atan(p.y, p.x);
		float rays = 0.55 + 0.45 * cos(ang * 5.0 + v_seed * 9.0);
		float shape = smoothstep(1.0, 0.0, r / (0.35 + 0.65 * rays));
		col = mix(vec3(1.0, 0.55, 0.15), vec3(1.0, 0.95, 0.7), smoothstep(0.2, 0.9, shape));
		alpha = shape * (1.0 - age);
	} else if (v_kind < 1.5) {
		float shape = smoothstep(1.0, 0.25, r + (n - 0.5) * 0.6);
		col = mix(vec3(0.55, 0.12, 0.02), vec3(1.0, 0.7, 0.25), (1.0 - age) * shape);
		alpha = shape * (1.0 - age * age);
	} else if (v_kind < 2.5) {
		float shape = smoothstep(1.0, 0.0, r) * (0.6 + 0.4 * n);
		col = vec3(1.0, 0.85, 0.5);
		alpha = shape * (1.0 - age);
	} else if (v_kind < 3.5) {
		float shape = smoothstep(1.0, 0.3, r + (n - 0.5) * 0.7);
		col = vec3(0.22, 0.21, 0.2) + n * 0.08;
		alpha = shape * 0.75 * (1.0 - age) * smoothstep(0.0, 0.08, age);
	} else {
		float shape = smoothstep(1.0, 0.2, r + (n - 0.5) * 0.8);
		col = vec3(0.5, 0.43, 0.33) + n * 0.08;
		alpha = shape * 0.7 * (1.0 - age);
	}
	if (v_kind < 2.5) {
		ALBEDO = col * alpha;  // additive
	} else {
		ALBEDO = col;
		ALPHA = alpha;
	}
}
