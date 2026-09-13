# PlatformIO ststm32 20.0.0 makes the ELF and the BIN only. Some flash tools
# need the Intel HEX image, so write it after the link step.

Import("env")

env.AddPostAction(
    "$BUILD_DIR/${PROGNAME}.elf",
    env.VerboseAction(
        "$OBJCOPY -O ihex -R .eeprom "
        "$BUILD_DIR/${PROGNAME}.elf $BUILD_DIR/${PROGNAME}.hex",
        "Building $BUILD_DIR/${PROGNAME}.hex",
    ),
)