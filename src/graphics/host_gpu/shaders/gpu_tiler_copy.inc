// Linear buffer <-> storage image copy with the semantics of one vkCmdCopyBufferToImage / vkCmdCopyImageToBuffer region on a
// colour 2D image whose texel is 4, 8 or 16 bytes (graphics/host_gpu/renderer/image/imageCopyKernels.cpp). The image is seen through
// an unsigned integer view of the same format class, so the copy moves bits and converts nothing.
//
// The including file defines:
//   FMT           the image format layout qualifier (r32ui, rgba16ui, rgba32ui)
//   TEXEL_WORDS   texel size in 32-bit words (1, 2, 4)
//   UPLOAD        defined for buffer -> image, not defined for image -> buffer
layout(local_size_x = 16, local_size_y = 8, local_size_z = 1) in;

#ifdef UPLOAD
layout(set = 0, binding = 0, std430) readonly buffer Linear { uint words[]; } linear;
layout(set = 0, binding = 1, FMT) uniform writeonly uimage2DArray image;
#else
layout(set = 0, binding = 0, std430) buffer Linear { uint words[]; } linear;
layout(set = 0, binding = 1, FMT) uniform readonly uimage2DArray image;
#endif

layout(push_constant, std430) uniform Region {
	uint buffer_word;  // first word of the region inside the bound buffer range
	uint row_words;    // words between two rows
	uint slice_words;  // words between two layers
	uint width;
	uint height;
	uint layers;
	int  x;            // image offset
	int  y;
	int  layer;        // first array layer
} region;

void main() {
	uvec3 p = gl_GlobalInvocationID;
	if (p.x >= region.width || p.y >= region.height || p.z >= region.layers) {
		return;
	}
	uint word = region.buffer_word + p.z * region.slice_words + p.y * region.row_words + p.x * uint(TEXEL_WORDS);
	ivec3 texel = ivec3(region.x + int(p.x), region.y + int(p.y), region.layer + int(p.z));
#ifdef UPLOAD
#if TEXEL_WORDS == 1
	uvec4 value = uvec4(linear.words[word], 0u, 0u, 0u);
#elif TEXEL_WORDS == 2
	uint lo = linear.words[word];
	uint hi = linear.words[word + 1u];
	uvec4 value = uvec4(lo & 0xffffu, lo >> 16u, hi & 0xffffu, hi >> 16u);
#else
	uvec4 value = uvec4(linear.words[word], linear.words[word + 1u], linear.words[word + 2u], linear.words[word + 3u]);
#endif
	imageStore(image, texel, value);
#else
	uvec4 value = imageLoad(image, texel);
#if TEXEL_WORDS == 1
	linear.words[word] = value.x;
#elif TEXEL_WORDS == 2
	linear.words[word]      = value.x | (value.y << 16u);
	linear.words[word + 1u] = value.z | (value.w << 16u);
#else
	linear.words[word]      = value.x;
	linear.words[word + 1u] = value.y;
	linear.words[word + 2u] = value.z;
	linear.words[word + 3u] = value.w;
#endif
#endif
}
