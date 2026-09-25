# cmake -DINPUT=<file> -DOUTPUT=<file.cpp> -DSYMBOL=<ident> -DNAMESPACE=<ns> -P EmbedFile.cmake
# Writes INPUT as `const unsigned char SYMBOL[]` plus `SYMBOL_size` (see Embed.cmake).
file(READ "${INPUT}" hex HEX)
string(LENGTH "${hex}" hex_length)
math(EXPR size "${hex_length} / 2")
string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," bytes "${hex}")
# 20 bytes per line keeps the generated file diffable and the compiler's line buffer small.
string(REGEX REPLACE "((0x[0-9a-f][0-9a-f],){20})" "\\1\n" bytes "${bytes}")
file(WRITE "${OUTPUT}.tmp"
"// Generated from ${INPUT} by EmbedFile.cmake; do not edit.
#include <cstddef>

namespace ${NAMESPACE} {

extern const unsigned char ${SYMBOL}[];
extern const std::size_t ${SYMBOL}_size;
alignas(16) const unsigned char ${SYMBOL}[] = {
${bytes}
};
const std::size_t ${SYMBOL}_size = ${size};

} // namespace ${NAMESPACE}
")
file(RENAME "${OUTPUT}.tmp" "${OUTPUT}")
