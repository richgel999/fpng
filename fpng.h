// fpng.h - R. Geldreich, Jr. - public domain or zlib license (see end of fpng.cpp for Public Domain waiver/optional license)
#pragma once

#include <stdlib.h>
#include <stdint.h>
#include <vector>

namespace fpng
{
	bool fpng_encode_image_to_memory(const void* pImage, int w, int h, int num_chans, bool flip, std::vector<uint8_t>& out_buf);

#ifndef FPNG_NO_STDIO
	bool fpng_encode_image_to_file(const char* pFilename, const void* pImage, int w, int h, int num_chans, bool flip);
#endif

} // namespace fpng