/* Synthetic library: exercises the normal loader and PLT forwarding path. */
#define SOLIB_IMPORTS(X)
#define SOLIB_PLT(X)
#define SOLIB_ALIASES(X)
#define SOLIB_FUNCS(X) X(B4000000) X(B4000010)
#define SOLIB_EXPORTS(X) X("test_solib_function", 0xB4000000u)
#define SOLIB_DATA_BASE 0xB4010000u
#define SOLIB_DATA_SIZE 4096u
