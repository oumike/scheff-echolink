# GSM 06.10 full-rate codec (libgsm 1.0.22, Jutta Degener & Carsten Bormann,
# TU Berlin). Sources are unmodified; see COPYRIGHT. The toast CLI and
# debug/print helpers are omitted.
idf_component_register(
    SRCS "src/add.c" "src/code.c" "src/decode.c" "src/gsm_create.c"
         "src/gsm_decode.c" "src/gsm_destroy.c" "src/gsm_encode.c"
         "src/gsm_option.c" "src/long_term.c" "src/lpc.c"
         "src/preprocess.c" "src/rpe.c" "src/short_term.c" "src/table.c"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "private")

# SASR: the RISC-V toolchain does arithmetic right shifts on signed values.
target_compile_definitions(${COMPONENT_LIB} PRIVATE SASR NeedFunctionPrototypes=1)
target_compile_options(${COMPONENT_LIB} PRIVATE
    -O2 -Wno-comment -Wno-unused-variable -Wno-unused-but-set-variable
    -Wno-implicit-fallthrough -Wno-sign-compare -Wno-parentheses)
