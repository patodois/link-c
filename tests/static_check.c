/* REWRITE_LINK_STATIC: the whole library as static functions of this one translation unit. Compiled with -Werror
 * (Makefile `make test`, CMake), so a function the program does not call must not draw an unused-function warning. */
#define REWRITE_LINK_STATIC
#define REWRITE_LINK_IMPLEMENTATION
#include "../rewrite_link.h"

int main(void) { return rbl_seq_newer(2u, 1u) ? 0 : 1; }
