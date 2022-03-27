#define bit_check(source, bit) ((source & (1 << bit)) != 0)
#define bit_set(source, bit) (source | (1 << bit))
#define bit_clear(source, bit) (source & ~(1 << bit))
#define bit_def(source, bit, doit) (doit ? bit_set(source, bit) : bit_clear(source, bit) )
