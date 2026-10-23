// Batched selection rings and placeholder boxes (UnitBatches): flat player colour, with the player
// index read from INSTANCE_CUSTOM[channel].
shader_type spatial;
render_mode unshaded, shadows_disabled;

uniform vec4 colors[4] : source_color;
uniform int channel = 0;

varying vec3 color;

void vertex() {
	int p = int(INSTANCE_CUSTOM[channel] + 0.5) % 4;
	color = colors[p].rgb;
}

void fragment() {
	ALBEDO = color;
}
