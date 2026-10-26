class_name UiArt
extends RefCounted
## The original game's UI art and font, read through SimBridge as the player's nation sees them:
## skins/<NATION>.rwd replaces about 165 UI files per nation (the control panel's insignia, frames,
## buttons), other nations' skins are skipped, the rest comes from Data.rwd. Nothing is copied
## out of the install.
##
## Resolution sets (UI/resolution.tgi: 800x600, 1024x768, 1280x960, filtering NEAREST): the 1024
## art is the base (UI/Game/..., Fonts/small/...), and UI/1280/... and Fonts/<font>/1280/... hold
## the files redrawn for 1280. The HUD picks 1024 or 1280, whichever is nearer the window height,
## lays everything out in 1024 units (the space of the original layouts, UI/Game/*.tgi) and scales
## the whole HUD by 1 or 1.25. A texture found in the 1280 folder is drawn at 1/1.25 of its pixel
## size, so it lands 1:1 on screen; a base texture is scaled up 1.25.

const BASE := Vector2(1024, 768)
## HUD textures share atlas pages, so consecutive controls draw in one batch: on GL Compatibility
## every texture change in the 2D pass is another draw call (about 33 for the HUD before, at
## roughly 80 us each on the dev laptop's Intel HD 3000). Each image is padded by its own edge
## pixels so filtering at the 1.25 scale doesn't bleed between neighbours.
const PAGE := 2048
const PAD := 2

var bridge: SimBridge
var nation := "gbr"
var set_name := "1024"   # "1024" or "1280"
var scale := 1.0         # HUD scale: 1.0 (1024 set) or 1.25 (1280 set), less if the window is narrower
var _tex := {}           # path -> Texture2D or null
var _hi := {}            # path -> true when it came from the 1280 folder
var _img := {}           # VFS path -> Image (hit tests)
var _font: Font
var _styles := {}
var _pages: Array[Image] = []
var _page_tex: Array[ImageTexture] = []
var _dirty := {}          # page index -> true until flush()
var _shelf := Vector3i.ZERO  # current page's packing cursor: x, y, shelf height


func _init(p_bridge: SimBridge, p_nation: String) -> void:
	bridge = p_bridge
	nation = p_nation.to_lower()


## Picks the set for a window of `view` pixels. Returns true when it changed.
func choose(view: Vector2) -> bool:
	var hi := absf(view.y - 960.0) < absf(view.y - 768.0)
	var want := "1280" if hi else "1024"
	var s := minf(1.25 if hi else 1.0, view.x / BASE.x)
	var changed := want != set_name
	if changed:
		_tex.clear()
		_hi.clear()
		_styles.clear()
		_pages.clear()
		_page_tex.clear()
		_dirty.clear()
		_shelf = Vector3i.ZERO
	set_name = want
	scale = s
	return changed


## A texture by its path in the data: "/UI/%s/Game/ControlPanel/Background.tga" (a set folder,
## with fallback to the base file) or an absolute one ("/UI/Game/BuildingButtons/IconCorps.png").
## null if the install has no such file.
func tex(path: String) -> Texture2D:
	if path == "":
		return null
	if _tex.has(path):
		return _tex[path]
	var t: Texture2D = null
	for c in _candidates(path):
		var img := image(c[0])
		if img != null:
			t = _pack(img)
			_hi[path] = c[1]
			break
	_tex[path] = t
	return t


## `img` on an atlas page (an AtlasTexture); a texture of its own if it is too big.
func _pack(src: Image) -> Texture2D:
	var img := src
	if img.get_format() != Image.FORMAT_RGBA8:
		img = src.duplicate() as Image
		img.convert(Image.FORMAT_RGBA8)
	var w := img.get_width() + 2 * PAD
	var h := img.get_height() + 2 * PAD
	if w > PAGE or h > PAGE:
		return ImageTexture.create_from_image(img)
	if _pages.is_empty() or _shelf.x + w > PAGE and _shelf.y + _shelf.z + h > PAGE or _shelf.y + h > PAGE:
		_new_page()
	if _shelf.x + w > PAGE:  # next shelf
		_shelf = Vector3i(0, _shelf.y + _shelf.z, 0)
		if _shelf.y + h > PAGE:
			_new_page()
	var page := _pages.size() - 1
	var at := Vector2i(_shelf.x, _shelf.y)
	var dst := _pages[page]
	var iw := img.get_width()
	var ih := img.get_height()
	dst.blit_rect(img, Rect2i(0, 0, iw, ih), at + Vector2i(PAD, PAD))
	# Extrude the edges into the padding.
	for k in PAD:
		dst.blit_rect(img, Rect2i(0, 0, iw, 1), at + Vector2i(PAD, k))
		dst.blit_rect(img, Rect2i(0, ih - 1, iw, 1), at + Vector2i(PAD, PAD + ih + k))
	for k in PAD:
		dst.blit_rect(dst, Rect2i(at.x + PAD, at.y, 1, h), at + Vector2i(k, 0))
		dst.blit_rect(dst, Rect2i(at.x + PAD + iw - 1, at.y, 1, h), at + Vector2i(PAD + iw + k, 0))
	_shelf.x += w
	_shelf.z = maxi(_shelf.z, h)
	_dirty[page] = true
	var a := AtlasTexture.new()
	a.atlas = _page_tex[page]
	a.region = Rect2(at + Vector2i(PAD, PAD), Vector2(iw, ih))
	return a


func _new_page() -> void:
	var img := Image.create(PAGE, PAGE, false, Image.FORMAT_RGBA8)
	_pages.append(img)
	_page_tex.append(ImageTexture.create_from_image(img))
	_shelf = Vector3i.ZERO


## Uploads the atlas pages changed since the last call (call after creating textures).
func flush() -> void:
	for page in _dirty:
		_page_tex[page].update(_pages[page])
	_dirty.clear()


## The size `path` is drawn at, in layout (1024) units.
func size_of(path: String) -> Vector2:
	var t := tex(path)
	if t == null:
		return Vector2.ZERO
	return t.get_size() / (1.25 if _hi.get(path, false) else 1.0)


## The decoded image at a VFS path (cached), or null.
func image(vfs_path: String) -> Image:
	var key := vfs_path.to_lower()
	if _img.has(key):
		return _img[key]
	var img: Image = null
	var bytes: PackedByteArray = bridge.read_file_for_nation(vfs_path, nation)
	if not bytes.is_empty():
		img = Image.new()
		var err := FAILED
		match vfs_path.get_extension().to_lower():
			"tga":
				err = img.load_tga_from_buffer(bytes)
			"png":
				err = img.load_png_from_buffer(bytes)
			"dds":
				err = img.load_dds_from_buffer(bytes)
			"bmp":
				err = img.load_bmp_from_buffer(bytes)
		if err != OK:
			img = null
		elif img.is_compressed():
			img.decompress()
	_img[key] = img
	return img


## The image behind a texture path (for alpha hit tests), or null.
func image_of(path: String) -> Image:
	for c in _candidates(path):
		var img := image(c[0])
		if img != null:
			return img
	return null


func _candidates(path: String) -> Array:
	var p := path.trim_prefix("/")
	if p.contains("%s"):
		return [[p.replace("%s", set_name), set_name == "1280"], [p.replace("%s/", ""), false]]
	return [[p, false]]


## The original font (Fonts/TrueType/A&A.ttf; font_small: size 12, bold, shadow), or the default.
func font() -> Font:
	if _font != null:
		return _font
	var bytes: PackedByteArray = bridge.read_file_for_nation("Fonts/TrueType/A&A.ttf", nation)
	if bytes.is_empty():
		_font = ThemeDB.fallback_font
	else:
		var f := FontFile.new()
		f.data = bytes
		f.antialiasing = TextServer.FONT_ANTIALIASING_GRAY
		_font = f
	return _font


## A small-font glyph icon (Fonts/small/money.png, ammo, fuel, unitlimit, building, ...).
func glyph(name: String) -> String:
	return "Fonts/small/%s/" + name + ".png"


## A nine-patch panel style from the original's tiled panels: `prefix` + Corners / Left / Right /
## Top / Bottom / Center (e.g. "/UI/%s/Game/Panel/Tile", "/UI/%s/Game/Panel/InfoTile"). The corner
## texture holds the four corners in its quadrants.
func panel_style(prefix: String, ext := "tga") -> StyleBoxTexture:
	var key := prefix + "." + ext
	if _styles.has(key):
		return _styles[key]
	var pieces := {}
	for part in ["Corners", "Left", "Right", "Top", "Bottom", "Center"]:
		pieces[part] = image_of(prefix + part + "." + ext)
	var corners: Image = pieces.Corners
	if corners == null:
		_styles[key] = null
		return null
	var c := corners.get_width() / 2
	var out := Image.create(3 * c, 3 * c, false, Image.FORMAT_RGBA8)
	var cimg := corners.duplicate() as Image
	cimg.convert(Image.FORMAT_RGBA8)
	out.blit_rect(cimg, Rect2i(0, 0, c, c), Vector2i(0, 0))
	out.blit_rect(cimg, Rect2i(c, 0, c, c), Vector2i(2 * c, 0))
	out.blit_rect(cimg, Rect2i(0, c, c, c), Vector2i(0, 2 * c))
	out.blit_rect(cimg, Rect2i(c, c, c, c), Vector2i(2 * c, 2 * c))
	for spec in [["Left", Vector2i(0, c)], ["Right", Vector2i(2 * c, c)], ["Top", Vector2i(c, 0)], ["Bottom", Vector2i(c, 2 * c)],
			["Center", Vector2i(c, c)]]:
		var src: Image = pieces[spec[0]]
		if src == null:
			continue
		var s := src.duplicate() as Image
		s.convert(Image.FORMAT_RGBA8)
		s.resize(c, c, Image.INTERPOLATE_BILINEAR)
		out.blit_rect(s, Rect2i(0, 0, c, c), spec[1])
	var sb := StyleBoxTexture.new()
	sb.texture = ImageTexture.create_from_image(out)
	sb.texture_margin_left = c
	sb.texture_margin_right = c
	sb.texture_margin_top = c
	sb.texture_margin_bottom = c
	sb.content_margin_left = c * 0.6
	sb.content_margin_right = c * 0.6
	sb.content_margin_top = c * 0.5
	sb.content_margin_bottom = c * 0.5
	_styles[key] = sb
	return sb
