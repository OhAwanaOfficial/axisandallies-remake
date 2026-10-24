extends SceneTree
## Prints a resolved TGI record as JSON, as the view sees it through SimBridge.record().
## godot --headless --path godot --script res://scripts/tools/dump_record.gd -- TYPE IDS
## e.g. -- thing ger_tank_medium, -- audio <ids>, -- vfx <ids>. The game path is config/game_path.cfg.


func _init() -> void:
	var args := OS.get_cmdline_user_args()
	var game := ""
	var cfg := ProjectSettings.globalize_path("res://").path_join("../config/game_path.cfg").simplify_path()
	for line in FileAccess.get_file_as_string(cfg).split("\n"):
		var t := line.strip_edges()
		if t != "" and not t.begins_with("#"):
			game = t
	var bridge := SimBridge.new()
	if args.size() < 2 or not bridge.start(game, "gen:1:64", 1):
		print("usage: -- TYPE IDS   (%s)" % bridge.last_error())
		quit(2)
		return
	print(JSON.stringify(bridge.record(args[0], args[1]), "  "))
	quit()
