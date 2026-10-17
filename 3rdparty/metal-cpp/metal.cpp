/**
 * metal-cpp implementation translation unit.
 *
 * metal-cpp is a header-only C++ interface shipped with exactly one TU that
 * defines the framework symbols (see 3rdParty readme).  We build it as a static
 * library consumed by the Metal 4 backend target.
 *
 * The QuartzCore include is upstream's, and it is load-bearing: the presenter
 * reaches CAMetalLayer through metal-cpp, and its selector symbols are emitted
 * only by the TU that sees the QuartzCore headers with CA_PRIVATE_IMPLEMENTATION.
 */

#define NS_PRIVATE_IMPLEMENTATION
#define MTL_PRIVATE_IMPLEMENTATION
#define MTLFX_PRIVATE_IMPLEMENTATION
#define CA_PRIVATE_IMPLEMENTATION

#include "Foundation/Foundation.hpp"
#include "Metal/Metal.hpp"
#include "MetalFX/MetalFX.hpp"
#include "QuartzCore/QuartzCore.hpp"
