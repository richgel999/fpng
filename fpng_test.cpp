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

int main(int arg_c, char **arg_v)
{
	if (arg_c < 2)
	{
		printf("Usage: fpng_test filename\n");
		return EXIT_FAILURE;
	}

	const char* pFilename = nullptr;
	bool csv_flag = false;
	for (int i = 1; i < arg_c; i++)
	{
		const char* pArg = arg_v[i];
		if (pArg[0] == '-')
		{
			if (pArg[1] == 'c')
			{
				csv_flag = true;
			}
			else
			{
				fprintf(stderr, "Recognized option: %s\n", pArg);
				return EXIT_FAILURE;
			}
		}
		else
		{
			if (pFilename)
			{
				fprintf(stderr, "Too many filenames: %s\n", pArg);
				return EXIT_FAILURE;
			}
			pFilename = pArg;
		}
	}

	if (!csv_flag)
		printf("Filename: %s\n", pFilename);

	uint8_vec source_file_data;
	if (!read_file_to_vec(pFilename, source_file_data))
	{
		fprintf(stderr, "Failed reading source file data \"%s\"\n", pFilename);
		return EXIT_FAILURE;
	}

	unsigned int source_width = 0, source_height = 0;
	unsigned char* pSource_image_buffer = nullptr;
	unsigned error = lodepng_decode_memory(&pSource_image_buffer, &source_width, &source_height, source_file_data.data(), source_file_data.size(), LCT_RGBA, 8);
	if (error != 0)
	{
		fprintf(stderr, "Failed unpacking source file \"%s\"\n", pFilename);
		return EXIT_FAILURE;
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

	const uint32_t NUM_TIMES_TO_ENCODE = 3;
	interval_timer tm;

	// FPNG

	std::vector<uint8_t> fpng_file_buf;
	double fpng_best_time = 1e+9f;
	for (uint32_t i = 0; i < NUM_TIMES_TO_ENCODE; i++)
	{
		tm.start();
		if (!fpng::fpng_encode_image_to_memory((source_chans == 3) ? (const void *)pSource_pixels24 : (const void*)pSource_pixels32, source_width, source_height, source_chans, false, fpng_file_buf))
		{
			fprintf(stderr, "fpng_encode_image_to_memory() failed!\n");
			return EXIT_FAILURE;
		}
		fpng_best_time = minimum(fpng_best_time, tm.get_elapsed_secs());
	}

	if (!csv_flag)
		printf("FPNG: %f secs, %u bytes, %f MB\n", fpng_best_time, (uint32_t)fpng_file_buf.size(), fpng_file_buf.size() / (1024.0f * 1024.0f));

	if (!write_data_to_file("fpng.png", fpng_file_buf.data(), fpng_file_buf.size()))
	{
		fprintf(stderr, "Failed writing to file fpng.png\n");
		return EXIT_FAILURE;
	}

	// Verify FPNG's output data using lodepng
	{
		unsigned int lodepng_decoded_w = 0, lodepng_decoded_h = 0;
		unsigned char* lodepng_decoded_buffer = nullptr;
		error = lodepng_decode_memory(&lodepng_decoded_buffer, &lodepng_decoded_w, &lodepng_decoded_h, (unsigned char*)fpng_file_buf.data(), fpng_file_buf.size(), LCT_RGBA, 8);
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
		void* p = stbi_load_from_memory(fpng_file_buf.data(), (int)fpng_file_buf.size(), &x, &y, &c, 4);
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

	// lodepng

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
		printf("lodepng: %f secs, %u bytes, %f MB\n", lodepng_best_time, (uint32_t)lodepng_file_buf.size(), (double)lodepng_file_buf.size() / (1024.0f * 1024.0f));

	if (!write_data_to_file("lodepng.png", lodepng_file_buf.data(), lodepng_file_buf.size()))
	{
		fprintf(stderr, "Failed writing to file lodepng.png\n");
		return EXIT_FAILURE;
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
		printf("stbi: %f secs, %u bytes, %f MB\n", stbi_best_time, (uint32_t)stbi_file_buf.size(), (double)stbi_file_buf.size() / (1024.0f * 1024.0f));

	if (!write_data_to_file("stbi.png", stbi_file_buf.data(), stbi_file_buf.size()))
	{
		fprintf(stderr, "Failed writing to file stbi.png\n");
		return EXIT_FAILURE;
	}

	// QOI
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

	if (!write_data_to_file("qoi.qoi", pQOI_data, qoi_len))
	{
		fprintf(stderr, "Failed writing to file qoi.qoi\n");
		return EXIT_FAILURE;
	}

	if (!csv_flag)
		printf("qoi: %f secs, %i bytes, %f MB\n", qoi_best_time, qoi_len, (double)qoi_len / (1024.0f * 1024.0f));
		
	// Validate QOI's output file
	{
		qoi_desc qddesc;
		void* pQOI_decomp_data = qoi_decode(pQOI_data, qoi_len, &qddesc, 4);

		if (memcmp(pQOI_decomp_data, pSource_pixels32, total_source_pixels * 4) != 0)
		{
			fprintf(stderr, "QOI verification failure!\n");
			return EXIT_FAILURE;
		}
		free(pQOI_decomp_data);
		pQOI_decomp_data = nullptr;
	}

	if (csv_flag)
	{
		const double MB = 1024.0f * 1024.0f;

		printf("%s, %u, %u, %u,    %f, %u, %f,   %f, %u, %f,   %f, %u, %f,   %f, %u, %f\n",
			pFilename, source_width, source_height, source_chans,
			qoi_best_time, (uint32_t)qoi_len, (double)qoi_len / MB,
			fpng_best_time, (uint32_t)fpng_file_buf.size(), (double)fpng_file_buf.size() / MB,
			lodepng_best_time, (uint32_t)lodepng_file_buf.size(), (double)lodepng_file_buf.size() / MB,
			stbi_best_time, (uint32_t)stbi_file_buf.size(), (double)stbi_file_buf.size() / MB
			);
	}

	return EXIT_SUCCESS;
}

