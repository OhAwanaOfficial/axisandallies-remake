// Regiment banners (Banners): per regiment a flag and three bars above its units, a constant
// size on screen. INSTANCE_CUSTOM: x = fill 0..1, y = kind (0 flag, 1 health, 2 morale,
// 3 efficiency), z = player, w = flags (1 routing, 2 entrenched, 4 selected).
shader_type spatial;
render_mode unshaded, depth_draw_never, depth_test_disabled, cull_disabled, shadows_disabled;

uniform vec4 player_colors[8];
uniform float viewport_height = 720.0;
uniform float ui_scale = 1.0;
global uniform float fx_time;

varying float v_fill;
varying float v_kind;
varying float v_player;
varying float v_flags;
varying vec2 v_px;     // the quad's size in pixels
varying float v_depth;

// Pixel rectangle (centre x, centre y, width, height) of each part, y up from the anchor.
vec4 part(float kind) {
	if (kind < 0.5) return vec4(0.0, 22.0, 16.0, 11.0);
	if (kind < 1.5) return vec4(0.0, 12.0, 34.0, 4.0);
	if (kind < 2.5) return vec4(0.0, 7.0, 34.0, 4.0);
	return vec4(0.0, 2.0, 34.0, 4.0);
}

void vertex() {
	v_fill = INSTANCE_CUSTOM.x;
	v_kind = INSTANCE_CUSTOM.y;
	v_player = INSTANCE_CUSTOM.z;
	v_flags = INSTANCE_CUSTOM.w;
	vec4 r = part(v_kind) * ui_scale;
	v_px = r.zw;
	vec4 view = VIEW_MATRIX * vec4(MODEL_MATRIX[3].xyz, 1.0);
	vec4 clip = PROJECTION_MATRIX * view;
	// Offset in clip space by pixels: ndc = 2 px / viewport height (x scaled by the aspect).
	vec2 px = r.xy + VERTEX.xy * r.zw;
	// The Compatibility renderer flips the projection's Y when drawing into a render target.
	float flip = sign(PROJECTION_MATRIX[1][1]);
	float aspect = abs(PROJECTION_MATRIX[1][1] / PROJECTION_MATRIX[0][0]);
	clip.xy += vec2(px.x / aspect * sign(PROJECTION_MATRIX[0][0]), px.y * flip) * 2.0 / viewport_height * clip.w;
	POSITION = clip;
	v_depth = -view.z;
	UV = VERTEX.xy + 0.5;
}

void fragment() {
	vec2 p = UV * v_px;  // pixels inside the part
	bool border = p.x < 1.0 || p.y < 1.0 || p.x > v_px.x - 1.0 || p.y > v_px.y - 1.0;
	int pl = int(v_player + 0.5);
	vec3 pc = player_colors[pl].rgb;
	bool routing = mod(v_flags, 2.0) > 0.5;
	bool entrenched = mod(floor(v_flags / 2.0), 2.0) > 0.5;
	bool selected = v_flags >= 3.5;
	vec3 col;
	if (v_kind < 0.5) {
		col = pc;
		if (entrenched && p.y < 4.0) col = vec3(0.55, 0.45, 0.3);   // sandbags
		if (routing && fract(fx_time * 2.0) < 0.5) col = vec3(1.0);  // blinks white
		if (border) col = selected ? vec3(1.0, 1.0, 0.6) : vec3(0.05);
	} else {
		vec3 fillc;
		if (v_kind < 1.5) fillc = mix(vec3(0.9, 0.15, 0.1), vec3(0.2, 0.85, 0.2), smoothstep(0.25, 0.75, v_fill));
		else if (v_kind < 2.5) fillc = routing ? vec3(1.0, 0.3, 0.1) : vec3(0.3, 0.6, 1.0);
		else fillc = vec3(0.95, 0.85, 0.35);
		col = UV.x <= v_fill ? fillc : vec3(0.12);
		if (border) col = selected ? vec3(1.0, 1.0, 0.6) : vec3(0.0);
	}
	ALBEDO = col;
}
