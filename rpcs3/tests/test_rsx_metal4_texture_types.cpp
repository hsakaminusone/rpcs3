#include <gtest/gtest.h>

#include "Emu/RSX/MTL/MTLTextureCache.h"
#include "Emu/RSX/Common/TextureUtils.h"

// The guest's texture dimension is a contract with the shader, not an
// implementation detail: the guest's program declares the texture with a GLSL
// type (`texture1D`, `texture2D`, `texture3D`, `textureCube`), and Metal
// matches the bound resource against that declaration.
//
// A 1-D guest texture bound as a 2-D host texture is a type mismatch, the
// read returns undefined data, and the presenter blit -- the only draw in the
// frame that samples a 1-D texture -- replaces all three colour channels with
// it.  That is how the blit came to store black (handoff L230).  These tests
// ship with the release, so the mapping cannot regress unnoticed.
#ifdef RSX_ENABLE_METAL4

TEST(MTLTextureTypes, TheGuestDimensionMapsToTheHostTextureType)
{
	using dimension = rsx::texture_dimension_extended;

	EXPECT_EQ(MTL::TextureType1D, mtl::texture_cache::get_texture_type(dimension::texture_dimension_1d));
	EXPECT_EQ(MTL::TextureType2D, mtl::texture_cache::get_texture_type(dimension::texture_dimension_2d));
	EXPECT_EQ(MTL::TextureType3D, mtl::texture_cache::get_texture_type(dimension::texture_dimension_3d));
	EXPECT_EQ(MTL::TextureTypeCube, mtl::texture_cache::get_texture_type(dimension::texture_dimension_cubemap));
}

TEST(MTLTextureTypes, AOneDimensionalTextureHasExactlyOneRow)
{
	// The guest's height field carries no meaning for a 1-D texture, and Metal
	// rejects a 1-D texture with more than one row.
	const mtl::texture_extent extent = mtl::normalize_texture_extent(MTL::TextureType1D, 256, 7, 1, 1);
	EXPECT_EQ(256u, extent.width);
	EXPECT_EQ(1u, extent.height);
	EXPECT_EQ(1u, extent.depth);

	// The two-dimensional case is untouched: the height is the guest's.
	const mtl::texture_extent flat = mtl::normalize_texture_extent(MTL::TextureType2D, 256, 7, 1, 1);
	EXPECT_EQ(256u, flat.width);
	EXPECT_EQ(7u, flat.height);
}

TEST(MTLTextureTypes, TheDescriptorCarriesTheHostTextureType)
{
	// The descriptor is what the device sees, so the type has to survive the
	// construction: a 1-D descriptor with one row.
	MTL::TextureDescriptor* descriptor = mtl::texture_cache::make_descriptor(
		MTL::PixelFormat::PixelFormatBGRA8Unorm, MTL::TextureType1D, 256, 1, 1, 1);

	ASSERT_NE(nullptr, descriptor);
	EXPECT_EQ(MTL::TextureType1D, descriptor->textureType());
	EXPECT_EQ(256u, descriptor->width());
	EXPECT_EQ(1u, descriptor->height());
	descriptor->release();
}

#endif // RSX_ENABLE_METAL4
