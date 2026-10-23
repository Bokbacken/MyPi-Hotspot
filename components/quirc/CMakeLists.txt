# Vendored quirc (QR-code recognition library, ISC licence) for on-device Wi-Fi QR decoding.
# Upstream: https://github.com/dlbeer/quirc, lib/ at commit 927d680904dc95fdff4cd9d022eb374b438ff8f2
# (2025-05-20), unmodified. See README.md and LICENSE.
idf_component_register(
    SRCS "lib/decode.c" "lib/identify.c" "lib/quirc.c" "lib/version_db.c"
    INCLUDE_DIRS "lib"
)
# The ESP32-P4 FPU is single precision; doubles would be emulated in software.
target_compile_definitions(${COMPONENT_LIB} PRIVATE QUIRC_FLOAT_TYPE=float QUIRC_USE_TGMATH)
target_compile_options(${COMPONENT_LIB} PRIVATE -Wno-unused-parameter -Wno-sign-compare)
