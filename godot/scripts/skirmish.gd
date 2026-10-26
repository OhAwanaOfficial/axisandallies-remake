extends Node
## Autoload `Skirmish` (M6 close-out): the setup the skirmish screen hands to the battle. Empty when
## the battle runs as a development scene (its command-line options).
##   map: map spec (a tools/rmp.py map.json path or gen:SEED:SIZE), nation / enemy: nation codes,
##   money: starting money (0: the map's), seed, ai: the opponent's controller ("dummy"),
##   scripted: the slice player plays your side (headless play-test), slice_test: where that test
##   writes its replay ("" when not testing), defeat_test: the sides swapped (the dummy plays you,
##   the slice player the enemy; the test expects the defeat screen).

const NATIONS := ["gbr", "ger", "usa", "rus", "jpn"]
const NATION_NAMES := {"gbr": "Great Britain", "ger": "Germany", "usa": "United States", "rus": "Soviet Union", "jpn": "Japan"}
const MONEY := [0, 500, 1000, 2000, 5000]

var settings := {}
var back_test_code := -1      # the defeat test pressed New skirmish: its exit code, for the setup screen


static func defaults(map_spec: String) -> Dictionary:
	return {"map": map_spec, "nation": "gbr", "enemy": "ger", "money": 0, "seed": 1, "ai": "dummy", "scripted": false,
		"slice_test": "", "defeat_test": false}
