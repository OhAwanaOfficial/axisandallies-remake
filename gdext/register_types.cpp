#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/core/defs.hpp>
#include <godot_cpp/godot.hpp>

#include "sim_bridge.hpp"

using namespace godot;

static void initializeSimBridgeModule(ModuleInitializationLevel level) {
    if (level != MODULE_INITIALIZATION_LEVEL_SCENE) {
        return;
    }
    GDREGISTER_CLASS(SimBridge);
}

static void uninitializeSimBridgeModule(ModuleInitializationLevel level) {
    if (level != MODULE_INITIALIZATION_LEVEL_SCENE) {
        return;
    }
}

extern "C" {
GDExtensionBool GDE_EXPORT simbridge_init(GDExtensionInterfaceGetProcAddress getProcAddress,
                                          const GDExtensionClassLibraryPtr library,
                                          GDExtensionInitialization* initialization) {
    GDExtensionBinding::InitObject initObj(getProcAddress, library, initialization);
    initObj.register_initializer(initializeSimBridgeModule);
    initObj.register_terminator(uninitializeSimBridgeModule);
    initObj.set_minimum_library_initialization_level(MODULE_INITIALIZATION_LEVEL_SCENE);
    return initObj.init();
}
}
