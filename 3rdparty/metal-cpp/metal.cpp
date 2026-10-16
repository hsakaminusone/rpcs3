/**
 * metal-cpp implementation translation unit.
 *
 * metal-cpp is a header-only C++ interface shipped with exactly one TU that
 * defines the framework symbols (see 3rdParty readme).  We build it as a static
 * library consumed by the Metal 4 backend target.
 */

#define NS_PRIVATE_IMPLEMENTATION
#define MTL_PRIVATE_IMPLEMENTATION
#define MTLFX_PRIVATE_IMPLEMENTATION
#define CA_PRIVATE_IMPLEMENTATION

#include "Metal/Metal.hpp"
