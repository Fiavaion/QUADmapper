/*
 * stb_dxt implementation (BC1/BC3 block compressor, used by Convert to HAP, QM-082)
 *
 * stb_dxt.h is the unmodified v1.12 header from
 * https://github.com/nothings/stb/blob/master/stb_dxt.h
 */

#include <string.h>  // stb_dxt.h uses memcpy/memset without including it

#ifdef _MSC_VER
#pragma warning(push, 0)  // third-party code: its narrowing casts are intended
#endif

#define STB_DXT_IMPLEMENTATION
#include "stb_dxt.h"

#ifdef _MSC_VER
#pragma warning(pop)
#endif
