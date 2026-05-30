#define bit_check(source, bit) (((source) & (1u << (unsigned)(bit))) != 0u)
#define bit_set(source, bit) ((byte)((source) | (1u << (unsigned)(bit))))
#define bit_clear(source, bit) ((byte)((source) & ~(1u << (unsigned)(bit))))
#define bit_def(source, bit, doit) ((doit) ? bit_set((source), (bit)) : bit_clear((source), (bit)))
