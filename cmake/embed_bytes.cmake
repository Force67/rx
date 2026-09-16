# Embeds any file as a C array, for content a binary must carry rather than
# look up: the splash wordmark cannot fail to load on a machine that never
# unpacked an archive. Run with -P; see rx_embed_bytes in engine/ui/CMakeLists.
# The array is not NUL-terminated, so sizeof() is the byte count.
file(READ ${SOURCE} hex HEX)
string(REGEX REPLACE "(..)" "0x\\1," bytes "${hex}")
string(REGEX REPLACE "(0x..,0x..,0x..,0x..,0x..,0x..,0x..,0x..,)" "\\1\n" bytes "${bytes}")
get_filename_component(dir ${HEADER} DIRECTORY)
file(MAKE_DIRECTORY ${dir})
file(WRITE ${HEADER}
  "// generated from ${SOURCE}, do not edit\n"
  "static const unsigned char ${SYMBOL}[] = {\n${bytes}\n};\n")
