class_name AlphaHit
extends TextureRect
## A TextureRect that only takes the mouse where its image is opaque: the control panel's art has
## see-through areas (above the centre frame) where clicks belong to the battlefield.

var hit_image: Image


func _has_point(p: Vector2) -> bool:
	if not Rect2(Vector2.ZERO, size).has_point(p):
		return false
	if hit_image == null:
		return true
	var x := clampi(int(p.x / size.x * hit_image.get_width()), 0, hit_image.get_width() - 1)
	var y := clampi(int(p.y / size.y * hit_image.get_height()), 0, hit_image.get_height() - 1)
	return hit_image.get_pixel(x, y).a > 0.2
