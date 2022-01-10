#define _CRT_SECURE_NO_WARNINGS

#include <stdlib.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>

#if defined(_WIN32)
// For QueryPerformanceCounter/QueryPerformanceFrequency
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

#include "fpng.h"
#include "lodepng.h"

#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"

#define QOI_IMPLEMENTATION
#include "qoi.h"

#define WUFFS_IMPLEMENTATION
#define WUFFS_CONFIG__STATIC_FUNCTIONS
#include "wuffs-v0.3.c"

typedef std::vector<uint8_t> uint8_vec;

typedef uint64_t timer_ticks;

template <typename S> static inline S maximum(S a, S b) { return (a > b) ? a : b; }
template <typename S> static inline S minimum(S a, S b) { return (a < b) ? a : b; }

class interval_timer
{
public:
	interval_timer();

	void start();
	void stop();

	double get_elapsed_secs() const;
	inline double get_elapsed_ms() const { return 1000.0f * get_elapsed_secs(); }

	static void init();
	static inline timer_ticks get_ticks_per_sec() { return g_freq; }
	static timer_ticks get_ticks();
	static double ticks_to_secs(timer_ticks ticks);
	static inline double ticks_to_ms(timer_ticks ticks) { return ticks_to_secs(ticks) * 1000.0f; }

private:
	static timer_ticks g_init_ticks, g_freq;
	static double g_timer_freq;

	timer_ticks m_start_time, m_stop_time;

	bool m_started, m_stopped;
};

timer_ticks interval_timer::g_init_ticks, interval_timer::g_freq;
double interval_timer::g_timer_freq;

#if defined(_WIN32)
inline void query_counter(timer_ticks* pTicks)
{
	QueryPerformanceCounter(reinterpret_cast<LARGE_INTEGER*>(pTicks));
}
inline void query_counter_frequency(timer_ticks* pTicks)
{
	QueryPerformanceFrequency(reinterpret_cast<LARGE_INTEGER*>(pTicks));
}
#elif defined(__APPLE__)
#include <sys/time.h>
inline void query_counter(timer_ticks* pTicks)
{
	struct timeval cur_time;
	gettimeofday(&cur_time, NULL);
	*pTicks = static_cast<unsigned long long>(cur_time.tv_sec) * 1000000ULL + static_cast<unsigned long long>(cur_time.tv_usec);
}
inline void query_counter_frequency(timer_ticks* pTicks)
{
	*pTicks = 1000000;
}
#elif defined(__GNUC__)
#include <sys/timex.h>
inline void query_counter(timer_ticks* pTicks)
{
	struct timeval cur_time;
	gettimeofday(&cur_time, NULL);
	*pTicks = static_cast<unsigned long long>(cur_time.tv_sec) * 1000000ULL + static_cast<unsigned long long>(cur_time.tv_usec);
}
inline void query_counter_frequency(timer_ticks* pTicks)
{
	*pTicks = 1000000;
}
#else
#error TODO
#endif

double get_timer()
{
	return interval_timer::ticks_to_secs(interval_timer::get_ticks());
}

interval_timer::interval_timer() : m_start_time(0), m_stop_time(0), m_started(false), m_stopped(false)
{
	if (!g_timer_freq)
		init();
}

void interval_timer::start()
{
	query_counter(&m_start_time);
	m_started = true;
	m_stopped = false;
}

void interval_timer::stop()
{
	assert(m_started);
	query_counter(&m_stop_time);
	m_stopped = true;
}

double interval_timer::get_elapsed_secs() const
{
	assert(m_started);
	if (!m_started)
		return 0;

	timer_ticks stop_time = m_stop_time;
	if (!m_stopped)
		query_counter(&stop_time);

	timer_ticks delta = stop_time - m_start_time;
	return delta * g_timer_freq;
}

void interval_timer::init()
{
	if (!g_timer_freq)
	{
		query_counter_frequency(&g_freq);
		g_timer_freq = 1.0f / g_freq;
		query_counter(&g_init_ticks);
	}
}

timer_ticks interval_timer::get_ticks()
{
	if (!g_timer_freq)
		init();
	timer_ticks ticks;
	query_counter(&ticks);
	return ticks - g_init_ticks;
}

double interval_timer::ticks_to_secs(timer_ticks ticks)
{
	if (!g_timer_freq)
		init();
	return ticks * g_timer_freq;
}

bool read_file_to_vec(const char* pFilename, uint8_vec& data)
{
	FILE* pFile = nullptr;
#ifdef _WIN32
	fopen_s(&pFile, pFilename, "rb");
#else
	pFile = fopen(pFilename, "rb");
#endif
	if (!pFile)
		return false;

	fseek(pFile, 0, SEEK_END);
#ifdef _WIN32
	int64_t filesize = _ftelli64(pFile);
#else
	int64_t filesize = ftello(pFile);
#endif
	if (filesize < 0)
	{
		fclose(pFile);
		return false;
	}
	fseek(pFile, 0, SEEK_SET);

	if (sizeof(size_t) == sizeof(uint32_t))
	{
		if (filesize > 0x70000000)
		{
			// File might be too big to load safely in one alloc
			fclose(pFile);
			return false;
		}
	}

	data.resize((size_t)filesize);

	if (filesize)
	{
		if (fread(&data[0], 1, (size_t)filesize, pFile) != (size_t)filesize)
		{
			fclose(pFile);
			return false;
		}
	}

	fclose(pFile);
	return true;
}

bool write_data_to_file_internal(const char* pFilename, const void* pData, size_t len)
{
	FILE* pFile = nullptr;
#ifdef _WIN32
	fopen_s(&pFile, pFilename, "wb");
#else
	pFile = fopen(pFilename, "wb");
#endif
	if (!pFile)
		return false;

	if (len)
	{
		if (fwrite(pData, 1, len, pFile) != len)
		{
			fclose(pFile);
			return false;
		}
	}

	return fclose(pFile) != EOF;
}

bool write_data_to_file(const char* pFilename, const void* pData, size_t len)
{
#ifdef _WIN32
	const uint32_t MAX_TRIES = 10;
#else
	const uint32_t MAX_TRIES = 1;
#endif

	for (uint32_t i = 0; i < MAX_TRIES; i++)
	{
		if (write_data_to_file_internal(pFilename, pData, len))
			return true;
#ifdef _WIN32
		Sleep(100);
#endif
	}
	return false;
}

struct color_rgba
{
	uint8_t m_c[4];
};

static void write_func_stbi(void* context, void* data, int size)
{
	if (!size)
		return;

	uint8_vec* pVec = (uint8_vec*)context;

	size_t cur_s = pVec->size();
	pVec->resize(cur_s + size);
	memcpy(pVec->data() + cur_s, data, size);
}

static bool load_listing_file(const std::string& f, std::vector<std::string>& filenames)
{
	std::string filename(f);
	if (filename.size() == 0)
		return false;

	if (filename[0] == '@')
		filename.erase(0, 1);

	FILE* pFile = nullptr;
#ifdef _WIN32
	fopen_s(&pFile, filename.c_str(), "r");
#else
	pFile = fopen(filename.c_str(), "r");
#endif

	if (!pFile)
	{
		fprintf(stderr, "Failed opening listing file: \"%s\"\n", filename.c_str());
		return false;
	}

	uint32_t total_filenames = 0;

	for (; ; )
	{
		char buf[3072];
		buf[0] = '\0';

		char* p = fgets(buf, sizeof(buf), pFile);
		if (!p)
		{
			if (ferror(pFile))
			{
				fprintf(stderr, "Failed reading from listing file: \"%s\"\n", filename.c_str());

				fclose(pFile);
				return false;
			}
			else
				break;
		}

		std::string read_filename(p);
		while (read_filename.size())
		{
			if (read_filename[0] == ' ')
				read_filename.erase(0, 1);
			else
				break;
		}

		while (read_filename.size())
		{
			const char c = read_filename.back();
			if ((c == ' ') || (c == '\n') || (c == '\r'))
				read_filename.erase(read_filename.size() - 1, 1);
			else
				break;
		}

		if (read_filename.size())
		{
			filenames.push_back(read_filename);
			total_filenames++;
		}
	}

	fclose(pFile);

	printf("Successfully read %u filenames(s) from listing file \"%s\"\n", total_filenames, filename.c_str());

	return true;
}

#include <random>

class mrand
{
	std::mt19937 m_mt;

public:
	mrand() {	}

	mrand(uint32_t s) { seed(s); }
	void seed(uint32_t s) { m_mt.seed(s); }

	// between [l,h]
	int irand(int l, int h) { std::uniform_int_distribution<int> d(l, h); return d(m_mt); }

	uint32_t urand32() { return static_cast<uint32_t>(irand(INT32_MIN, INT32_MAX)); }

	bool bit() { return irand(0, 1) == 1; }

	uint8_t byte() { return static_cast<uint8_t>(urand32()); }

	// between [l,h)
	float frand(float l, float h) { std::uniform_real_distribution<float> d(l, h); return d(m_mt); }

	float gaussian(float mean, float stddev) { std::normal_distribution<float> d(mean, stddev); return d(m_mt); }
};

static bool fuzz_test_encoder(
	uint32_t source_width, uint32_t source_height, uint32_t source_chans,
	const color_rgba* pSource_image_buffer32,
	const uint8_t* pSource_image_buffer24,
	uint32_t fpng_flags)
{
	uint32_t total_source_pixels = source_width * source_height;

	mrand r;

	for (uint32_t fuzz_trial = 0; fuzz_trial < 1000; fuzz_trial++)
	{
		r.seed(fuzz_trial);
		srand(fuzz_trial);

		uint8_vec temp_buf(total_source_pixels * source_chans);
		memcpy(temp_buf.data(), (source_chans == 3) ? (void*)pSource_image_buffer24 : (void*)pSource_image_buffer32, total_source_pixels * source_chans);

		const double rand_fract = r.frand(0.000001f, .1f);
		const uint32_t rand_thresh = (uint32_t)((double)RAND_MAX * rand_fract);

		if (r.frand(0.0f, 1.0f) < .05f)
		{
			uint32_t dst_ofs = 0;
			uint32_t total_runs = 0;
			while (dst_ofs < temp_buf.size())
			{
				uint32_t bytes_left = (uint32_t)temp_buf.size() - dst_ofs;
				uint32_t run_size = r.irand(1, minimum<uint32_t>(bytes_left / source_chans, 32));
				uint8_t run_lits[4] = { (uint8_t)r.irand(0, 0xFF), (uint8_t)r.irand(0, 0xFF), (uint8_t)r.irand(0, 0xFF), (uint8_t)r.irand(0, 0xFF) };

				for (uint32_t i = 0; i < run_size; i++)
				{
					memcpy(temp_buf.data() + dst_ofs, run_lits, source_chans);
					dst_ofs += source_chans;
				}

				total_runs++;
			}

			printf("%u, %u color fill runs\n", fuzz_trial, total_runs);
		}
		else if (r.frand(0.0f, 1.0f) < .05f)
		{
			uint32_t dst_ofs = 0;
			uint32_t total_runs = 0;
			while (dst_ofs < temp_buf.size())
			{
				uint32_t bytes_left = (uint32_t)temp_buf.size() - dst_ofs;
				uint32_t run_size = r.irand(1, minimum<uint32_t>(bytes_left / source_chans, 32));
				uint8_t run_lits[4] = { (uint8_t)r.irand(0, 0xFF), (uint8_t)r.irand(0, 0xFF), (uint8_t)r.irand(0, 0xFF), (uint8_t)r.irand(0, 0xFF) };

				if (r.frand(0.0f, 1.0f) > .8f)
				{
					for (uint32_t i = 0; i < run_size; i++)
					{
						for (uint32_t j = 0; j < source_chans; j++)
							temp_buf[dst_ofs + j] ^= run_lits[j];

						dst_ofs += source_chans;
					}
				}
				else
				{
					dst_ofs += run_size * source_chans;
				}

				total_runs++;
			}

			printf("%u, %u color corrupt runs\n", fuzz_trial, total_runs);
		}
		else if (r.frand(0.0f, 1.0f) < .05f)
		{
			uint32_t dst_ofs = 0;
			uint32_t total_runs = 0;
			while (dst_ofs < temp_buf.size())
			{
				uint32_t bytes_left = (uint32_t)temp_buf.size() - dst_ofs;
				uint32_t run_size = r.irand(1, minimum<uint32_t>(bytes_left, 258));
				uint32_t run_lit = r.irand(0, 0xFF);

				memset(temp_buf.data() + dst_ofs, run_lit, run_size);

				dst_ofs += run_size;

				total_runs++;
			}

			printf("%u, %u fill runs\n", fuzz_trial, total_runs);
		}
		else if (r.frand(0.0f, 1.0f) < .15f)
		{
			uint32_t dst_ofs = 0;
			uint32_t total_runs = 0;
			while (dst_ofs < temp_buf.size())
			{
				uint32_t bytes_left = (uint32_t)temp_buf.size() - dst_ofs;
				uint32_t run_size = r.irand(1, minimum<uint32_t>(bytes_left, 32));

				if (r.frand(0.0f, 1.0f) > .1f)
				{
					uint32_t run_lit = r.irand(0, 0xFF);

					for (uint32_t i = 0; i < run_size; i++)
						temp_buf[dst_ofs + i] ^= run_lit;
				}

				dst_ofs += run_size;

				total_runs++;
			}

			printf("%u, %u corrupt runs\n", fuzz_trial, total_runs);
		}
		else if (r.frand(0.0f, 1.0f) < .005f)
		{
			for (uint32_t i = 0; i < temp_buf.size(); i++)
				temp_buf[i] = (uint8_t)rand();

			printf("%u, full random\n", fuzz_trial);
		}
		else
		{
			uint32_t total_bits_flipped = 0;

			for (uint32_t i = 0; i < temp_buf.size(); i++)
			{
				for (uint32_t j = 0; j < 8; j++)
				{
					if ((uint32_t)rand() <= rand_thresh)
					{
						temp_buf[i] ^= (1 << j);
						total_bits_flipped++;
					}
				}
			}

			printf("%u, %u bits flipped\n", fuzz_trial, total_bits_flipped);
		}

		std::vector<uint8_t> fpng_file_buf;
				
		if (!fpng::fpng_encode_image_to_memory(temp_buf.data(), source_width, source_height, source_chans, fpng_file_buf, fpng_flags))
		{
			fprintf(stderr, "fpng_encode_image_to_memory() failed!\n");
			return false;
		}

		printf("fpng size: %u\n", (uint32_t)fpng_file_buf.size());

#if 0
		char filename[256];
		sprintf(filename, "test%u.png", fuzz_trial);
		if (!write_data_to_file(filename, fpng_file_buf.data(), fpng_file_buf.size()))
		{
			fprintf(stderr, "Failed writing file\n");
			return false;
		}
#endif

		unsigned int lodepng_decoded_w = 0, lodepng_decoded_h = 0;
		unsigned char* lodepng_decoded_buffer = nullptr;
		int error = lodepng_decode_memory(&lodepng_decoded_buffer, &lodepng_decoded_w, &lodepng_decoded_h, (unsigned char*)fpng_file_buf.data(), fpng_file_buf.size(), LCT_RGBA, 8);
		if (error != 0)
		{
			fprintf(stderr, "lodepng failed decompressing FPNG's output PNG file!\n");
			return false;
		}

		for (uint32_t i = 0; i < total_source_pixels; i++)
		{
			bool equal = true;
			for (uint32_t j = 0; j < source_chans; j++)
			{
				if (lodepng_decoded_buffer[i * 4 + j] != temp_buf[i * source_chans + j])
					equal = false;
			}

			if (!equal)
			{
				fprintf(stderr, "lodepng verification failure!\n");
				return false;
			}
		}

		{
			std::vector<uint8_t> fpngd_decode_buffer;
			uint32_t channels_in_file;
			uint32_t decoded_width, decoded_height;

			uint32_t desired_chans = 4;// source_chans;
			int res = fpng::fpng_decode_memory(fpng_file_buf.data(), (uint32_t)fpng_file_buf.size(), fpngd_decode_buffer, decoded_width, decoded_height, channels_in_file, desired_chans);
			if (res != 0)
			{
				fprintf(stderr, "fpng::fpng_decode() failed with error %i!\n", res);
				return false;
			}

			if ((decoded_width != source_width) || (decoded_height != source_height))
			{
				fprintf(stderr, "fpng::fpng_decode() returned an invalid image\n");
				return false;
			}

			const uint32_t chans_to_verify = minimum(source_chans, desired_chans);
			for (uint32_t i = 0; i < total_source_pixels; i++)
			{
				bool equal = true;
								
				for (uint32_t j = 0; j < chans_to_verify; j++)
				{
					if (fpngd_decode_buffer[i * desired_chans + j] != temp_buf[i * source_chans + j])
						equal = false;
				}

				if ((desired_chans == 4) && (source_chans == 3))
				{
					if (fpngd_decode_buffer[i * desired_chans + 3] != 0xFF)
						equal = false;
				}

				if (!equal)
				{
					fprintf(stderr, "fpng verification failure!\n");
					return false;
				}
			}
		}

		free(lodepng_decoded_buffer);
	}

	return true;
}

static int fuzz_test_encoder2(uint32_t fpng_flags)
{
	mrand r;

	const uint32_t MAX_IMAGE_DIM = 8193;

	for (uint32_t trial = 0; trial < 1000; trial++)
	{
		uint32_t width = r.irand(1, MAX_IMAGE_DIM + 1);
		uint32_t height = r.irand(1, MAX_IMAGE_DIM + 1);
		uint32_t num_chans = r.bit() ? 4 : 3;
		uint32_t total_pixels = width * height;
		uint32_t total_bytes = width * height * num_chans;

		std::vector<uint8_t> temp_buf(total_bytes);

		uint8_t* pDst = temp_buf.data();
		for (uint32_t i = 0; i < total_pixels; i++)
		//for (uint32_t i = 0; i < 1; i++)
		{
			uint32_t p = r.urand32();
			
			*pDst++ = (uint8_t)p;
			*pDst++ = (uint8_t)(p >> 8);
			*pDst++ = (uint8_t)(p >> 16);

			if (num_chans == 4)
				*pDst++ = (uint8_t)(p >> 24);
		}

		printf("Testing %ux%u %u\n", width, height, num_chans);

		std::vector<uint8_t> fpng_file_buf;

		if (!fpng::fpng_encode_image_to_memory(temp_buf.data(), width, height, num_chans, fpng_file_buf, fpng_flags))
		{
			fprintf(stderr, "fpng_encode_image_to_memory() failed!\n");
			return EXIT_FAILURE;
		}

		printf("fpng size: %u\n", (uint32_t)fpng_file_buf.size());

		std::vector<uint8_t> decomp_buf;
		uint32_t dec_width, dec_height, dec_chans;
		int res = fpng::fpng_decode_memory(fpng_file_buf.data(), (uint32_t)fpng_file_buf.size(), decomp_buf, dec_width, dec_height, dec_chans, num_chans);
		if (res != fpng::FPNG_DECODE_SUCCESS)
		{
			fprintf(stderr, "fpng::fpng_decode_memory() failed!\n");
			return EXIT_FAILURE;
		}

		if ((dec_width != width) || (dec_height != height) || (dec_chans != num_chans))
		{
			fprintf(stderr, "fpng::fpng_decode_memory() returned an invalid image!\n");
			return EXIT_FAILURE;
		}

		if (memcmp(decomp_buf.data(), temp_buf.data(), dec_width * dec_height * dec_chans) != 0)
		{
			fprintf(stderr, "Decoded image failed verification\n");
			return EXIT_FAILURE;
		}
	}

	return EXIT_SUCCESS;
}

static void* wuffs_decode(void* pData, size_t data_len, uint32_t &width, uint32_t &height) 
{
	wuffs_png__decoder* pDec = wuffs_png__decoder__alloc();
	if (!pDec) 
		return nullptr;

	wuffs_png__decoder__set_quirk_enabled(pDec, WUFFS_BASE__QUIRK_IGNORE_CHECKSUM, true);

	wuffs_base__image_config ic;
	wuffs_base__io_buffer src = wuffs_base__ptr_u8__reader((uint8_t *)pData, data_len, true);
	wuffs_base__status status = wuffs_png__decoder__decode_image_config(pDec, &ic, &src);
	
	if (status.repr) 
	{
		free(pDec);
		return nullptr;
	}

	width = wuffs_base__pixel_config__width(&ic.pixcfg);
	height = wuffs_base__pixel_config__height(&ic.pixcfg);

	wuffs_base__pixel_config__set(&ic.pixcfg, WUFFS_BASE__PIXEL_FORMAT__RGBA_NONPREMUL, WUFFS_BASE__PIXEL_SUBSAMPLING__NONE, width, height);

	uint64_t workbuf_len = wuffs_png__decoder__workbuf_len(pDec).max_incl;
	if (workbuf_len > SIZE_MAX) 
	{
		free(pDec);
		return nullptr;
	}

	wuffs_base__slice_u8 workbuf_slice = wuffs_base__make_slice_u8( (uint8_t *)malloc((size_t)workbuf_len), (size_t)workbuf_len); 
	if (!workbuf_slice.ptr) 
	{
		free(pDec);
		return nullptr;
	}

	const uint64_t total_pixels = (uint64_t)width * (uint64_t)height;
	if (total_pixels > (SIZE_MAX >> 2U)) 
	{
		free(workbuf_slice.ptr);
		free(pDec);
		return nullptr;
	}

	void* pDecode_buf = malloc((size_t)(total_pixels * sizeof(uint32_t)));
	if (!pDecode_buf)
	{
		free(workbuf_slice.ptr);
		free(pDec);
		return nullptr;
	}

	wuffs_base__slice_u8 pixbuf_slice = wuffs_base__make_slice_u8((uint8_t*)pDecode_buf, (size_t)(total_pixels * sizeof(uint32_t)));

	wuffs_base__pixel_buffer pb;
	status = wuffs_base__pixel_buffer__set_from_slice(&pb, &ic.pixcfg, pixbuf_slice);
	
	if (status.repr) 
	{
		free(workbuf_slice.ptr);
		free(pDecode_buf);
		free(pDec);
		return nullptr;
	}

	status = wuffs_png__decoder__decode_frame(pDec, &pb, &src, WUFFS_BASE__PIXEL_BLEND__SRC, workbuf_slice, NULL);
	
	if (status.repr) 
	{
		free(workbuf_slice.ptr);
		free(pDecode_buf);
		free(pDec);
		return nullptr;
	}
			
	free(workbuf_slice.ptr);
	free(pDec);

	return pDecode_buf;
}

#if FPNG_TRAIN_HUFFMAN_TABLES
static int training_mode(const char* pFilename)
{
	if (pFilename[0] != '@')
	{
		fprintf(stderr, "Must specify list of files to read using @filelist.txt\n");
		return EXIT_FAILURE;
	}

	std::vector<std::string> files_to_process;

	if (!load_listing_file(std::string(pFilename), files_to_process))
		return EXIT_FAILURE;

	uint64_t opaque_freq[fpng::HUFF_COUNTS_SIZE], alpha_freq[fpng::HUFF_COUNTS_SIZE];
	memset(opaque_freq, 0, sizeof(opaque_freq));
	memset(alpha_freq, 0, sizeof(alpha_freq));

	uint32_t total_alpha_files = 0, total_opaque_files = 0, total_failed_loading = 0;

	for (uint32_t file_index = 0; file_index < files_to_process.size(); file_index++)
	{
		const char* pFilename = files_to_process[file_index].c_str();

		printf("Processing file \"%s\"\n", pFilename);

		uint8_vec source_file_data;
		if (!read_file_to_vec(pFilename, source_file_data))
		{
			fprintf(stderr, "Failed reading source file data \"%s\"\n", pFilename);
			return EXIT_FAILURE;
		}

		uint32_t source_width = 0, source_height = 0;
		uint8_t* pSource_image_buffer = nullptr;
		unsigned error = lodepng_decode_memory(&pSource_image_buffer, &source_width, &source_height, source_file_data.data(), source_file_data.size(), LCT_RGBA, 8);
		if (error != 0)
		{
			fprintf(stderr, "WARNING: Failed unpacking source file \"%s\" using lodepng! Skipping.\n", pFilename);
			total_failed_loading++;
			continue;
		}

		const color_rgba* pSource_pixels32 = (const color_rgba*)pSource_image_buffer;
		uint32_t total_source_pixels = source_width * source_height;
		bool has_alpha = false;
		for (uint32_t i = 0; i < total_source_pixels; i++)
		{
			if (pSource_pixels32[i].m_c[3] < 255)
			{
				has_alpha = true;
				break;
			}
		}

		const uint32_t source_chans = has_alpha ? 4 : 3;

		printf("Dimensions: %ux%u, Has Alpha: %u, Total Pixels: %u, bytes: %u (%f MB)\n", source_width, source_height, has_alpha, total_source_pixels, total_source_pixels * source_chans, total_source_pixels * source_chans / (1024.0f * 1024.0f));

		uint8_vec source_image_buffer24(total_source_pixels * 3);
		for (uint32_t i = 0; i < total_source_pixels; i++)
		{
			source_image_buffer24[i * 3 + 0] = pSource_pixels32[i].m_c[0];
			source_image_buffer24[i * 3 + 1] = pSource_pixels32[i].m_c[1];
			source_image_buffer24[i * 3 + 2] = pSource_pixels32[i].m_c[2];
		}
		const uint8_t* pSource_pixels24 = source_image_buffer24.data();

		memset(fpng::g_huff_counts, 0, sizeof(fpng::g_huff_counts));

		std::vector<uint8_t> fpng_file_buf;
		bool status = fpng::fpng_encode_image_to_memory((source_chans == 4) ? (const void*)pSource_pixels32 : (const void*)pSource_pixels24, source_width, source_height, source_chans, fpng_file_buf, fpng::FPNG_ENCODE_SLOWER);
		if (!status)
		{
			fprintf(stderr, "fpng_encode_image_to_memory() failed!\n");
			return EXIT_FAILURE;
		}

		// Sanity check the PNG file using lodepng
		{
			uint32_t lodepng_decoded_w = 0, lodepng_decoded_h = 0;
			uint8_t* lodepng_decoded_buffer = nullptr;

			int error = lodepng_decode_memory(&lodepng_decoded_buffer, &lodepng_decoded_w, &lodepng_decoded_h, (uint8_t*)fpng_file_buf.data(), fpng_file_buf.size(), LCT_RGBA, 8);
			if (error != 0)
			{
				fprintf(stderr, "lodepng_decode_memory() failed!\n");
				return EXIT_FAILURE;
			}

			if (memcmp(lodepng_decoded_buffer, pSource_pixels32, total_source_pixels * 4) != 0)
			{
				fprintf(stderr, "FPNG decode verification failed (using lodepng)!\n");
				return EXIT_FAILURE;
			}
			free(lodepng_decoded_buffer);
		}

		if (source_chans == 4)
		{
			for (uint32_t i = 0; i < fpng::HUFF_COUNTS_SIZE; i++)
				alpha_freq[i] += fpng::g_huff_counts[i];

			total_alpha_files++;
		}
		else
		{
			for (uint32_t i = 0; i < fpng::HUFF_COUNTS_SIZE; i++)
				opaque_freq[i] += fpng::g_huff_counts[i];

			total_opaque_files++;
		}

	} // filename_index

	printf("Total alpha files: %u\n", total_alpha_files);
	printf("Total opaque files: %u\n", total_opaque_files);
	printf("Total failed loading: %u\n", total_failed_loading);

	if (!total_alpha_files && !total_opaque_files)
	{
		fprintf(stderr, "No failed were loaded!\n");
		return EXIT_FAILURE;
	}

	if (total_opaque_files)
	{
		std::vector<uint8_t> dyn_prefix;
		uint64_t bit_buf = 0;
		int bit_buf_size = 0;
		uint32_t codes[fpng::HUFF_COUNTS_SIZE];
		uint8_t codesizes[fpng::HUFF_COUNTS_SIZE];
		
		bool status = fpng::create_dynamic_block_prefix(opaque_freq, 3, dyn_prefix, bit_buf, bit_buf_size, codes, codesizes);
		if (!status)
		{
			fprintf(stderr, "fpng::create_dynamic_block_prefix() failed!\n");
			return EXIT_FAILURE;
		}

		printf("\n");
		printf("static const uint8_t g_dyn_huff_3[] = {\n");
		for (uint32_t i = 0; i < dyn_prefix.size(); i++) 
		{ 
			printf("%u%c ", dyn_prefix[i], (i != (dyn_prefix.size() - 1)) ? ',' : ' '); 
			if ((i & 31) == 31) 
				printf("\n"); 
		}
		printf("};\n");
		printf("const uint32_t DYN_HUFF_3_BITBUF = %u, DYN_HUFF_3_BITBUF_SIZE = %u;\n", (uint32_t)bit_buf, (uint32_t)bit_buf_size);

		printf("static const struct { uint8_t m_code_size; uint16_t m_code; } g_dyn_huff_3_codes[288] = {\n");
		for (uint32_t i = 0; i < fpng::HUFF_COUNTS_SIZE; i++)
		{
			printf("{%u,%u}%c", codesizes[i], codes[i], (i != (fpng::HUFF_COUNTS_SIZE - 1)) ? ',' : ' ');
			if ((i & 31) == 31)
				printf("\n");
		}
		printf("};\n");
	}

	if (total_alpha_files)
	{
		std::vector<uint8_t> dyn_prefix;
		uint64_t bit_buf = 0;
		int bit_buf_size = 0;
		uint32_t codes[fpng::HUFF_COUNTS_SIZE];
		uint8_t codesizes[fpng::HUFF_COUNTS_SIZE];
		bool status = fpng::create_dynamic_block_prefix(alpha_freq, 4, dyn_prefix, bit_buf, bit_buf_size, codes, codesizes);
		if (!status)
		{
			fprintf(stderr, "fpng::create_dynamic_block_prefix() failed!\n");
			return EXIT_FAILURE;
		}

		printf("\n");
		printf("static const uint8_t g_dyn_huff_4[] = {\n");
		for (uint32_t i = 0; i < dyn_prefix.size(); i++)
		{
			printf("%u%c ", dyn_prefix[i], (i != (dyn_prefix.size() - 1)) ? ',' : ' ');
			if ((i & 31) == 31)
				printf("\n");
		}
		printf("};\n");
		printf("const uint32_t DYN_HUFF_4_BITBUF = %u, DYN_HUFF_4_BITBUF_SIZE = %u;\n", (uint32_t)bit_buf, (uint32_t)bit_buf_size);

		printf("static const struct { uint8_t m_code_size; uint16_t m_code; } g_dyn_huff_4_codes[288] = {\n");
		for (uint32_t i = 0; i < fpng::HUFF_COUNTS_SIZE; i++)
		{
			printf("{%u,%u}%c", codesizes[i], codes[i], (i != (fpng::HUFF_COUNTS_SIZE - 1)) ? ',' : ' ');
			if ((i & 31) == 31)
				printf("\n");
		}
		printf("};\n");
	}

	return EXIT_SUCCESS;
}
#else
static int training_mode(const char* pFilename)
{
	(void)pFilename;

	fprintf(stderr, "Must compile with FPNG_TRAIN_HUFFMAN_TABLES set to 1\n");

	return EXIT_FAILURE;
}
#endif

int main(int arg_c, char **arg_v)
{
	fpng::fpng_init();
	
	if (arg_c < 2)
	{
		printf("Usage: fpng_test [filename.png] <alpha_filename.png>\n");
		printf("Loads image and compresses it with fpng, lodepng, stb_image_write and QOI. Wrotes fpng.png, lodepng.png, stbi.png, and qoi.qoi.\n");
		printf("Also decompresses the FPNG compressed file with several decompressors (lodepng, stb_image and fpng) and validates the decompressed data against the original source image.\n");
		printf("\nOptions:\n");
		printf("-s: 2 pass compression\n");
		printf("-u: Use uncompressed Deflate blocks\n");
		printf("-c: Write comma seperated values to stdout\n");
		printf("-e: Fuzz encoder/decoder by randomly modifying an input image's pixels\n");
		printf("-f: Decompress specified PNG image using FPNG, then exit\n");
		printf("-a: Swizzle input image's green to alpha, for testing 32bpp correlation alpha\n");
		printf("-t: Train Huffman tables on @filelist.txt (must compile with FPNG_TRAIN_HUFFMAN_TABLES=1)\n");
		return EXIT_FAILURE;
	}

	const char* pFilename = nullptr;
	const char* pAlpha_filename = nullptr;
	bool csv_flag = false;
	bool slower_encoding = false;
	bool force_uncompressed = false;
	bool fuzz_encoder = false;
	bool fuzz_encoder2 = false;
	bool fuzz_decoder = false;
	bool swizzle_green_to_alpha = false;
	bool training_mode_flag = false;

	for (int i = 1; i < arg_c; i++)
	{
		const char* pArg = arg_v[i];
		if (pArg[0] == '-')
		{
			if (pArg[1] == 'u')
			{
				force_uncompressed = true;
			}
			else if (pArg[1] == 's')
			{
				slower_encoding = true;
			}
			else if (pArg[1] == 'c')
			{
				csv_flag = true;
			}
			else if (pArg[1] == 'e')
			{
				fuzz_encoder = true;
			}
			else if (pArg[1] == 'E')
			{
				fuzz_encoder2 = true;
			}
			else if (pArg[1] == 'f')
			{
				fuzz_decoder = true;
			}
			else if (pArg[1] == 'a')
			{
				swizzle_green_to_alpha = true;
			}
			else if (pArg[1] == 't')
			{
				training_mode_flag = true;
			}
			else
			{
				fprintf(stderr, "Unrecognized option: %s\n", pArg);
				return EXIT_FAILURE;
			}
		}
		else
		{
			if ((pFilename) && (pAlpha_filename))
			{
				fprintf(stderr, "Too many filenames\n");
				return EXIT_FAILURE;
			}
			
			if (pFilename)
				pAlpha_filename = pArg;
			else
				pFilename = pArg;
		}
	}

	uint32_t fpng_flags = 0;
	if (slower_encoding)
		fpng_flags |= fpng::FPNG_ENCODE_SLOWER;
	if (force_uncompressed)
		fpng_flags |= fpng::FPNG_FORCE_UNCOMPRESSED;

	if (fuzz_encoder2)
		return fuzz_test_encoder2(fpng_flags);

	if (training_mode_flag)
		return training_mode(pFilename);

	if (!csv_flag)
	{
		printf("SSE 4.1 supported: %u\n", fpng::fpng_cpu_supports_sse41());

		printf("Filename: %s\n", pFilename);
		if (pAlpha_filename)
			printf("Alpha filename: %s\n", pFilename);
	}

	uint8_vec source_file_data;
	if (!read_file_to_vec(pFilename, source_file_data))
	{
		fprintf(stderr, "Failed reading source file data \"%s\"\n", pFilename);
		return EXIT_FAILURE;
	}

	if (fuzz_decoder)
	{
		std::vector<uint8_t> fpngd_decode_buffer;
		uint32_t channels_in_file;
		uint32_t decoded_width, decoded_height;

		int res = fpng::fpng_decode_memory(source_file_data.data(), (uint32_t)source_file_data.size(), fpngd_decode_buffer, decoded_width, decoded_height, channels_in_file, 3);
		if (res != 0)
		{
			fprintf(stderr, "fpng::fpng_decode() failed with error %i!\n", res);
			return EXIT_FAILURE;
		}

		if (lodepng_encode_file("out.png", fpngd_decode_buffer.data(), decoded_width, decoded_height, (channels_in_file == 4) ? LCT_RGBA : LCT_RGB, 8) != 0)
		{
			fprintf(stderr, "lodepng_encode_file() failed with error %i!\n", res);
			return EXIT_FAILURE;
		}

		printf("Wrote out.png %ux%u %u\n", decoded_width, decoded_height, channels_in_file);

		return EXIT_SUCCESS;
	}
		
	uint32_t source_width = 0, source_height = 0;
	uint8_t* pSource_image_buffer = nullptr;
	unsigned error = lodepng_decode_memory(&pSource_image_buffer, &source_width, &source_height, source_file_data.data(), source_file_data.size(), LCT_RGBA, 8);
	if (error != 0)
	{
		fprintf(stderr, "Failed unpacking source file \"%s\"\n", pFilename);
		return EXIT_FAILURE;
	}

	if (pAlpha_filename)
	{
		uint32_t alpha_source_width = 0, alpha_source_height = 0;
		uint8_t* pAlpha_source_image_buffer = nullptr;
		error = lodepng_decode_file(&pAlpha_source_image_buffer, &alpha_source_width, &alpha_source_height, pAlpha_filename, LCT_RGBA, 8);
		if (error != 0)
		{
			fprintf(stderr, "Failed unpacking alpha source file \"%s\"\n", pAlpha_filename);
			return EXIT_FAILURE;
		}
		if (!csv_flag)
			printf("Alpha Dimensions: %ux%u\n", alpha_source_width, alpha_source_height);
		for (uint32_t y = 0; y < minimum(alpha_source_height, source_height); y++)
		{
			for (uint32_t x = 0; x < minimum(alpha_source_width, source_width); x++)
			{
				uint32_t a = pAlpha_source_image_buffer[(x + y * alpha_source_width) * 4 + 1];
				pSource_image_buffer[(x + y * source_width) * 4 + 3] = a;
			}
		}
		free(pAlpha_source_image_buffer);
	}
	else if (swizzle_green_to_alpha)
	{
		for (uint32_t y = 0; y < source_height; y++)
			for (uint32_t x = 0; x < source_width; x++)
				pSource_image_buffer[(x + y * source_width) * 4 + 3] = pSource_image_buffer[(x + y * source_width) * 4 + 1];
	}

	const color_rgba* pSource_pixels32 = (const color_rgba*)pSource_image_buffer;
	uint32_t total_source_pixels = source_width * source_height;
	bool has_alpha = false;
	for (uint32_t i = 0; i < total_source_pixels; i++)
	{
		if (pSource_pixels32[i].m_c[3] < 255)
		{
			has_alpha = true;
			break;
		}
	}

	const uint32_t source_chans = has_alpha ? 4 : 3;

	if (!csv_flag)
		printf("Dimensions: %ux%u, Has Alpha: %u, Total Pixels: %u, bytes: %u (%f MB)\n", source_width, source_height, has_alpha, total_source_pixels, total_source_pixels * source_chans, total_source_pixels * source_chans / (1024.0f * 1024.0f));

	uint8_vec source_image_buffer24(total_source_pixels * 3);
	for (uint32_t i = 0; i < total_source_pixels; i++)
	{
		source_image_buffer24[i * 3 + 0] = pSource_pixels32[i].m_c[0];
		source_image_buffer24[i * 3 + 1] = pSource_pixels32[i].m_c[1];
		source_image_buffer24[i * 3 + 2] = pSource_pixels32[i].m_c[2];
	}

	const uint8_t* pSource_pixels24 = source_image_buffer24.data();
	
	const uint32_t NUM_TIMES_TO_ENCODE = csv_flag ? 3 : 1;
	const uint32_t NUM_TIMES_TO_DECODE = 5;
	interval_timer tm;

	if (!csv_flag)
		printf("** Encoding:\n");

	// Compress with FPNG
	if (fuzz_encoder)
	{
		bool status = fuzz_test_encoder(source_width, source_height, source_chans, pSource_pixels32, pSource_pixels24, fpng_flags);
		if (!status)
			fprintf(stderr, "fuzz_test_encoder() failed!\n");

		return status ? EXIT_SUCCESS : EXIT_FAILURE;
	}

	std::vector<uint8_t> fpng_file_buf;
	double fpng_best_time = 1e+9f;
	for (uint32_t i = 0; i < NUM_TIMES_TO_ENCODE; i++)
	{
		tm.start();
		if (!fpng::fpng_encode_image_to_memory((source_chans == 3) ? (const void *)pSource_pixels24 : (const void*)pSource_pixels32, source_width, source_height, source_chans, fpng_file_buf, fpng_flags))
		{
			fprintf(stderr, "fpng_encode_image_to_memory() failed!\n");
			return EXIT_FAILURE;
		}
		fpng_best_time = minimum(fpng_best_time, tm.get_elapsed_secs());
	}

	if (!csv_flag)
		printf("FPNG:    %4.6f secs, %u bytes, %4.3f MB, %4.3f MP/sec\n", fpng_best_time, (uint32_t)fpng_file_buf.size(), fpng_file_buf.size() / (1024.0f * 1024.0f), total_source_pixels / (1024.0f * 1024.0f) / fpng_best_time);

	if (!csv_flag)
	{
		if (!write_data_to_file("fpng.png", fpng_file_buf.data(), fpng_file_buf.size()))
		{
			fprintf(stderr, "Failed writing to file fpng.png\n");
			return EXIT_FAILURE;
		}

#if 0
		std::vector<uint8_t> out;
		uint32_t width, height, channels_in_file;
		int res = fpng::fpng_decode_file("fpng.png", out, width, height, channels_in_file, 4);
		if (res != fpng::FPNG_DECODE_SUCCESS)
		{
			fprintf(stderr, "fpng::fpng_decode_file() failed!\n");
			return EXIT_FAILURE;
		}
#endif
	}
	
	double fpng_decode_time = 0.0f, lodepng_decode_time = 0.0f, stbi_decode_time = 0.0f, qoi_decode_time = 0.0f, wuffs_decode_time = 0.0f;

	// Decode the file using our decompressor
	{
		std::vector<uint8_t> fpngd_decode_buffer;
		uint32_t channels_in_file;
		uint32_t decoded_width, decoded_height;

		fpng_decode_time = 1e+9f;

		int res;
		for (uint32_t i = 0; i < NUM_TIMES_TO_DECODE; i++)
		{
			fpngd_decode_buffer.clear();
						
			tm.start();
			res = fpng::fpng_decode_memory(fpng_file_buf.data(), (uint32_t)fpng_file_buf.size(), fpngd_decode_buffer, decoded_width, decoded_height, channels_in_file, 4);
			if (res != 0)
				break;
			fpng_decode_time = minimum(tm.get_elapsed_secs(), fpng_decode_time);
		}

		if (res != 0)
		{
			fprintf(stderr, "fpng::fpng_decode() failed with error %i!\n", res);
			return EXIT_FAILURE;
		}

		if ((decoded_width != source_width) || (decoded_height != source_height))
		{
			fprintf(stderr, "fpng::fpng_decode() returned an invalid image\n");
			return EXIT_FAILURE;
		}

		if (memcmp(fpngd_decode_buffer.data(), pSource_pixels32, total_source_pixels * 4) != 0)
		{
			fprintf(stderr, "FPNG decode verification failed (using FPNG)!\n");
			return EXIT_FAILURE;
		}
	}

	// Test 4->3 conversion in FPNG decoder
	if (source_chans == 4)
	{
		std::vector<uint8_t> fpngd_decode_buffer2;
		uint32_t channels_in_file2;
		uint32_t decoded_width2, decoded_height2;

		int res = fpng::fpng_decode_memory(fpng_file_buf.data(), (uint32_t)fpng_file_buf.size(), fpngd_decode_buffer2, decoded_width2, decoded_height2, channels_in_file2, 3);
		if (res != 0)
		{
			fprintf(stderr, "fpng::fpng_decode() failed with error %i!\n", res);
			return EXIT_FAILURE;
		}

		if ((channels_in_file2 != 4) || (decoded_width2 != source_width) || (decoded_height2 != source_height))
		{
			fprintf(stderr, "fpng::fpng_decode() returned an invalid image\n");
			return EXIT_FAILURE;
		}

		if (memcmp(fpngd_decode_buffer2.data(), pSource_pixels24, total_source_pixels * 3) != 0)
		{
			fprintf(stderr, "FPNG decode verification failed (using FPNG)!\n");
			return EXIT_FAILURE;
		}
	}

	// Test 3->4 conversion in FPNG decoder
	if (source_chans == 3)
	{
		std::vector<uint8_t> fpngd_decode_buffer2;
		uint32_t channels_in_file2;
		uint32_t decoded_width2, decoded_height2;

		int res = fpng::fpng_decode_memory(fpng_file_buf.data(), (uint32_t)fpng_file_buf.size(), fpngd_decode_buffer2, decoded_width2, decoded_height2, channels_in_file2, 4);
		if (res != 0)
		{
			fprintf(stderr, "fpng::fpng_decode() failed with error %i!\n", res);
			return EXIT_FAILURE;
		}

		if ((channels_in_file2 != 3) || (decoded_width2 != source_width) || (decoded_height2 != source_height))
		{
			fprintf(stderr, "fpng::fpng_decode() returned an invalid image\n");
			return EXIT_FAILURE;
		}

		if (memcmp(fpngd_decode_buffer2.data(), pSource_pixels32, total_source_pixels * 4) != 0)
		{
			fprintf(stderr, "FPNG decode verification failed (using FPNG)!\n");
			return EXIT_FAILURE;
		}
	}

	// Verify FPNG's output data using lodepng
	{
		uint32_t lodepng_decoded_w = 0, lodepng_decoded_h = 0;
		uint8_t* lodepng_decoded_buffer = nullptr;
		
		lodepng_decode_time = 1e+9f;

		for (uint32_t i = 0; i < NUM_TIMES_TO_DECODE; i++)
		{
			if (lodepng_decoded_buffer)
			{
				free(lodepng_decoded_buffer);
				lodepng_decoded_buffer = nullptr;
			}

			tm.start();
			error = lodepng_decode_memory(&lodepng_decoded_buffer, &lodepng_decoded_w, &lodepng_decoded_h, (uint8_t*)fpng_file_buf.data(), fpng_file_buf.size(), LCT_RGBA, 8);
			if (error != 0)
				break;
			lodepng_decode_time = minimum(tm.get_elapsed_secs(), lodepng_decode_time);
		}
				
		if (error != 0)
		{
			fprintf(stderr, "lodepng failed decompressing FPNG's output PNG file!\n");
			return EXIT_FAILURE;
		}

		if (memcmp(lodepng_decoded_buffer, pSource_pixels32, total_source_pixels * 4) != 0)
		{
			fprintf(stderr, "FPNG decode verification failed (using lodepng)!\n");
			return EXIT_FAILURE;
		}
		free(lodepng_decoded_buffer);
	}

	// Verify FPNG's output data using stb_image.h
	{
		int x, y, c;
		
		void* p = nullptr;

		stbi_decode_time = 1e+9f;
		for (uint32_t i = 0; i < NUM_TIMES_TO_DECODE; i++)
		{
			if (p)
			{
				free(p);
				p = nullptr;
			}

			tm.start();
			p = stbi_load_from_memory(fpng_file_buf.data(), (int)fpng_file_buf.size(), &x, &y, &c, 4);
			if (!p)
				break;
			
			stbi_decode_time = minimum(stbi_decode_time, tm.get_elapsed_secs());
		}

		if (!p)
		{
			fprintf(stderr, "stb_image.h failed decompressing FPNG's output PNG file!\n");
			return EXIT_FAILURE;
		}

		if (memcmp(p, pSource_pixels32, total_source_pixels * 4) != 0)
		{
			fprintf(stderr, "FPNG decode verification failed (using stb_image.h)!\n");
			return EXIT_FAILURE;
		}
		free(p);
	}

	// Verify FPNG's output data using wuffs
	{
		void* p = nullptr;

		//static void* 

		wuffs_decode_time = 1e+9f;
		for (uint32_t i = 0; i < NUM_TIMES_TO_DECODE; i++)
		{
			if (p)
			{
				free(p);
				p = nullptr;
			}

			tm.start();
			
			uint32_t w, h;
			p = wuffs_decode(fpng_file_buf.data(), fpng_file_buf.size(), w, h);
			if (!p)
				break;

			if ((w != source_width) || (h != source_height))
			{
				fprintf(stderr, "wuffs failed decompressing FPNG's output PNG file!\n");
				return EXIT_FAILURE;
			}

			wuffs_decode_time = minimum(wuffs_decode_time, tm.get_elapsed_secs());
		}

		if (!p)
		{
			fprintf(stderr, "wuffs failed decompressing FPNG's output PNG file!\n");
			return EXIT_FAILURE;
		}

		if (memcmp(p, pSource_pixels32, total_source_pixels * 4) != 0)
		{
			fprintf(stderr, "FPNG decode verification failed (using wuffs)!\n");
			return EXIT_FAILURE;
		}
		free(p);
	}
		
	// Compress with lodepng

	uint8_vec lodepng_file_buf;
	double lodepng_best_time = 1e+9f;

	for (uint32_t i = 0; i < NUM_TIMES_TO_ENCODE; i++)
	{
		tm.start();
		lodepng_file_buf.resize(0);
		error = lodepng::encode(lodepng_file_buf, pSource_image_buffer, source_width, source_height, LCT_RGBA, 8);
		lodepng_best_time = minimum(lodepng_best_time, tm.get_elapsed_secs());

		if (error != 0)
		{
			fprintf(stderr, "lodepng::encode() failed!\n");
			return EXIT_FAILURE;
		}
	}
	
	if (!csv_flag)
		printf("lodepng: %4.6f secs, %u bytes, %4.3f MB, %4.3f MP/sec\n", lodepng_best_time, (uint32_t)lodepng_file_buf.size(), (double)lodepng_file_buf.size() / (1024.0f * 1024.0f), total_source_pixels / (1024.0f * 1024.0f) / lodepng_best_time);

	if (!csv_flag)
	{
		if (!write_data_to_file("lodepng.png", lodepng_file_buf.data(), lodepng_file_buf.size()))
		{
			fprintf(stderr, "Failed writing to file lodepng.png\n");
			return EXIT_FAILURE;
		}
	}

	// stb_image_write.h
	uint8_vec stbi_file_buf;
	stbi_file_buf.reserve(total_source_pixels * source_chans);
	double stbi_best_time = 1e+9f;
	
	for (uint32_t i = 0; i < NUM_TIMES_TO_ENCODE; i++)
	{
		stbi_file_buf.resize(0);

		tm.start();
		
		int res = stbi_write_png_to_func(write_func_stbi, &stbi_file_buf, source_width, source_height, source_chans, (source_chans == 3) ? (void *)pSource_pixels24 : (void*)pSource_pixels32, source_width * source_chans);
		if (!res)
		{
			fprintf(stderr, "stbi_write_png_to_func() failed!\n");
			return EXIT_FAILURE;
		}

		stbi_best_time = minimum(stbi_best_time, tm.get_elapsed_secs());
	}

	if (!csv_flag)
		printf("stbi:    %4.6f secs, %u bytes, %4.3f MB, %4.3f MP/sec\n", stbi_best_time, (uint32_t)stbi_file_buf.size(), (double)stbi_file_buf.size() / (1024.0f * 1024.0f), (total_source_pixels / (1024.0f * 1024.0f)) / stbi_best_time);
	
	if (!csv_flag)
	{
		if (!write_data_to_file("stbi.png", stbi_file_buf.data(), stbi_file_buf.size()))
		{
			fprintf(stderr, "Failed writing to file stbi.png\n");
			return EXIT_FAILURE;
		}
	}

	// Compress with QOI
	qoi_desc qdesc;
	qdesc.channels = source_chans;
	qdesc.width = source_width;
	qdesc.height = source_height;
	qdesc.colorspace = QOI_SRGB;

	int qoi_len = 0;
	
	void* pQOI_data = nullptr;
	double qoi_best_time = 1e+9f;
	for (uint32_t i = 0; i < NUM_TIMES_TO_ENCODE; i++)
	{
		if (pQOI_data)
			free(pQOI_data);

		tm.start();

		pQOI_data = qoi_encode((source_chans == 4) ? (void*)pSource_pixels32 : (void*)pSource_pixels24, &qdesc, &qoi_len);
		
		qoi_best_time = minimum(qoi_best_time, tm.get_elapsed_secs());
	}

	if (!csv_flag)
	{
		if (!write_data_to_file("qoi.qoi", pQOI_data, qoi_len))
		{
			fprintf(stderr, "Failed writing to file qoi.qoi\n");
			return EXIT_FAILURE;
		}
	}

	if (!csv_flag)
		printf("qoi:     %4.6f secs, %i bytes, %4.3f MB, %4.3f MP/sec\n", qoi_best_time, qoi_len, (double)qoi_len / (1024.0f * 1024.0f), (total_source_pixels / (1024.0f * 1024.0f)) / qoi_best_time);
		
	// Validate QOI's output file
	{
		qoi_desc qddesc;
		tm.start();
		void* pQOI_decomp_data = qoi_decode(pQOI_data, qoi_len, &qddesc, 4);
		qoi_decode_time = tm.get_elapsed_secs();
				
		if (memcmp(pQOI_decomp_data, pSource_pixels32, total_source_pixels * 4) != 0)
		{
			fprintf(stderr, "QOI verification failure!\n");
			return EXIT_FAILURE;
		}
		free(pQOI_decomp_data);
	}

	free(pQOI_data);
	pQOI_data = nullptr;

	if (!csv_flag)
	{
		printf("** Decoding:\n");
		printf("FPNG:    %3.6f secs, %4.3f MP/sec\n", fpng_decode_time, (total_source_pixels / (1024.0f * 1024.0f)) / fpng_decode_time);
		printf("lodepng: %3.6f secs, %4.3f MP/sec\n", lodepng_decode_time, (total_source_pixels / (1024.0f * 1024.0f)) / lodepng_decode_time);
		printf("stbi:    %3.6f secs, %4.3f MP/sec\n", stbi_decode_time, (total_source_pixels / (1024.0f * 1024.0f)) / stbi_decode_time);
		printf("wuffs:   %3.6f secs, %4.3f MP/sec\n", wuffs_decode_time, (total_source_pixels / (1024.0f * 1024.0f)) / wuffs_decode_time);
		printf("qoi:     %3.6f secs, %4.3f MP/sec\n", qoi_decode_time, (total_source_pixels / (1024.0f * 1024.0f)) / qoi_decode_time);
	}

	if (csv_flag)
	{
		const double MB = 1024.0f * 1024.0f;

		const double source_megapixels = total_source_pixels / (1024.0f * 1024.0f);

		printf("%s, %u, %u, %u,    %f, %f, %f, %4.1f, %4.1f,    %f, %f, %f, %4.1f, %4.1f,    %f, %f, %f, %4.1f, %4.1f,    %f, %f, %f, %4.1f, %4.1f\n",
			pFilename, source_width, source_height, source_chans,
			qoi_best_time, (double)qoi_len / MB, qoi_decode_time, source_megapixels / qoi_best_time, source_megapixels / qoi_decode_time,
			fpng_best_time, (double)fpng_file_buf.size() / MB, fpng_decode_time, source_megapixels / fpng_best_time, source_megapixels / fpng_decode_time,
			lodepng_best_time, (double)lodepng_file_buf.size() / MB, lodepng_decode_time, source_megapixels / lodepng_best_time, source_megapixels / lodepng_decode_time,
			stbi_best_time, (double)stbi_file_buf.size() / MB, stbi_decode_time, source_megapixels / stbi_best_time, source_megapixels / stbi_decode_time
			);
	}

	free(pSource_image_buffer);
	pSource_image_buffer = nullptr;

	return EXIT_SUCCESS;
}

