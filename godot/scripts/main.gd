extends Label


func _ready() -> void:
	var bridge := SimBridge.new()
	var msg: String = bridge.hello()
	text = msg
	print("SimBridge.hello() -> ", msg)
