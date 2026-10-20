idf_component_register(
    SRCS "minimp3.c"
    INCLUDE_DIRS "."
)

# 固件默认用调试优化(-Og)编译，实测 minimp3 解码只有实时的 44%，22 kHz 流会
# 触发看门狗并断续/无声。这里只给解码器组件开 -O2，其余组件保持原优化等级。
target_compile_options(${COMPONENT_LIB} PRIVATE -O2)
