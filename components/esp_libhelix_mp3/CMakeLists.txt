idf_component_register(
    SRC_DIRS
        "libhelix-mp3/."
        "libhelix-mp3/real"
    INCLUDE_DIRS
        "libhelix-mp3/pub"
    PRIV_INCLUDE_DIRS
        "libhelix-mp3/real")

# 局部 -O2：Helix 负责 22 kHz 流的实时解码，固件默认的 -Og 会拖慢解码。
target_compile_options(${COMPONENT_LIB} PRIVATE -Wno-unused-but-set-variable -O2)
