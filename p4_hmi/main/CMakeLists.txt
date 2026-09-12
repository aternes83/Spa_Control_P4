# No REQUIRES: `main` implicitly requires every component in the build, which is
# what pulls in the vendor BSP (added through EXTRA_COMPONENT_DIRS in the project
# CMakeLists) and LVGL. Naming them here instead would mean spelling the BSP's
# mangled directory name, which is a thing to get wrong for no gain.
idf_component_register(
    SRCS
        "main.c"
        "spa_state.c"
        "spalink_port.c"
        "bench_peer.c"
        "../../link/spalink_codec.c"
        "ui/ui.c"
        "ui/ui_dial.c"
        "ui/ui_icons.c"
        "ui/ui_model.c"
        "ui/ui_theme.c"
    INCLUDE_DIRS
        "."
        "ui"
        "../../link"
)
