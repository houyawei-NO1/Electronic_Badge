idf_component_register(
    SRCS "src/ui/ui.c"
         "src/ui/screens.c"
         "src/ui/styles.c"
         "src/ui/actions.c"
         "src/ui/images.c"
         "src/ui/vars.c"
         "src/ui/fonts/fonts.c"
    INCLUDE_DIRS "src/ui" "src/ui/fonts"
    REQUIRES lvgl
)
