#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <vector>
#include <algorithm>
#include <omp.h>

#include <src/time.h>
#include <src/metadata_table.h>
#include <src/image.h>
#include <src/renderEER.h>

//#define DEBUG_EER

//#define TIMING
#ifdef TIMING
	#define RCTIC(label) (timer.tic(label))
	#define RCTOC(label) (timer.toc(label))

	Timer timer;
	int TIMING_READ_EER = timer.setNew("read EER");
	int TIMING_BUILD_INDEX = timer.setNew("build index");
	int TIMING_UNPACK_RLE = timer.setNew("unpack RLE");
	int TIMING_RENDER_ELECTRONS = timer.setNew("render electrons");
#else
	#define RCTIC(label)
	#define RCTOC(label)
#endif

int EER_grouping = 40;
int EER_upsample = 2;

const char EERRenderer::EER_FOOTER_OK[]  = "ThermoFisherECComprOK000";
const char EERRenderer::EER_FOOTER_ERR[] = "ThermoFisherECComprERR00";
const int EERRenderer::EER_IMAGE_WIDTH = 4096;
const int EERRenderer::EER_IMAGE_HEIGHT = 4096;
const int EERRenderer::EER_IMAGE_PIXELS = EERRenderer::EER_IMAGE_WIDTH * EERRenderer::EER_IMAGE_HEIGHT;
const unsigned int EERRenderer::EER_LEN_FOOTER = 24;
const uint16_t EERRenderer::TIFF_COMPRESSION_EER = 65000;

template <typename T>
void EERRenderer::render8K(MultidimArray<T> &image, std::vector<unsigned int> &positions, std::vector<unsigned char> &symbols, int n_electrons)
{
	for (int i = 0; i < n_electrons; i++)
	{
		int x = ((positions[i] & 4095) << 1) | ((symbols[i] & 2) >> 1); // 4095 = 111111111111b, 3 = 00000010b
		int y = ((positions[i] >> 12) << 1) | ((symbols[i] & 8) >> 3); //  4096 = 2^12, 8 = 00001000b
			DIRECT_A2D_ELEM(image, y, x)++;
	}
}

template <typename T>
void EERRenderer::render4K(MultidimArray<T> &image, std::vector<unsigned int> &positions, std::vector<unsigned char> &symbols, int n_electrons)
{
	for (int i = 0; i < n_electrons; i++)
	{
		int x = positions[i] & 4095; // 4095 = 111111111111b
		int y = positions[i] >> 12; //  4096 = 2^12
			DIRECT_A2D_ELEM(image, y, x)++;
	}
}

EERRenderer::EERRenderer()
{
	ready = false;
	read_data = false;
	buf = NULL;
}

void EERRenderer::read(FileName _fn_movie)
{
	if (ready)
		REPORT_ERROR("Logic error: you cannot recycle EERRenderer for multiple files (now)");

#ifndef HAVE_TIFF
	REPORT_ERROR("To use EER, you have to re-compile RELION with libtiff.");
#else
	fn_movie = _fn_movie;

	// First of all, check the file size
	FILE *fh = fopen(fn_movie.c_str(), "r");
	if (fh == NULL)
		REPORT_ERROR("Failed to open " + fn_movie);

	fseek(fh, 0, SEEK_END);
	file_size = ftell(fh);
	fseek(fh, 0, SEEK_SET);

	// Try reading as TIFF; this handle is kept open
	ftiff = TIFFOpen(fn_movie.c_str(), "r");

	if (ftiff == NULL)
	{
		is_legacy = true;
		readLegacy(fh);
	}
	else
	{
		is_legacy = false;

		// Check width & size
		int width, height;
		TIFFGetField(ftiff, TIFFTAG_IMAGEWIDTH, &width);
		TIFFGetField(ftiff, TIFFTAG_IMAGELENGTH, &height);
#ifdef DEBUG_EER
		printf("EER in TIFF: %s size = %ld, width = %d, height = %d\n", fn_movie.c_str(), file_size, width, height);
#endif
		if (width != EER_IMAGE_WIDTH || height != EER_IMAGE_HEIGHT)
			REPORT_ERROR("Currently we support only 4096x4096 pixel EER movies.");

		// Find the number of frames
		nframes = 1;
		while (TIFFSetDirectory(ftiff, nframes) != 0) nframes++;

#ifdef DEBUG_EER
		printf("EER in TIFF: %s nframes = %d\n", fn_movie.c_str(), nframes);
#endif
	}

	fclose(fh);
	ready = true;
#endif
}

void EERRenderer::readLegacy(FILE *fh)
{
	/* Load everything first */
	RCTIC(TIMING_READ_EER);
	buf = (unsigned char*)malloc(file_size);
	if (buf == NULL)
		REPORT_ERROR("Failed to allocate the buffer.");
	fread(buf, sizeof(char), file_size, fh);
	RCTOC(TIMING_READ_EER);

	/* Build frame index */
	RCTIC(TIMING_BUILD_INDEX);
	long long pos = file_size;
	while (pos > 0)
	{
		pos -= EER_LEN_FOOTER;
		if (strncmp(EER_FOOTER_OK, (char*)buf + pos, EER_LEN_FOOTER) == 0)
		{
			pos -= 8;
			long long frame_size = *(long long*)(buf + pos) * sizeof(long long);
			pos -= frame_size;
			frame_starts.push_back(pos);
			frame_sizes.push_back(frame_size);
#ifdef DEBUG_EER
			printf("Frame: LAST-%5d Start at: %08d Frame size: %lld\n", frame_starts.size(), pos, frame_size);
#endif
		}
		else // if (strncmp(EER_FOOTER_ERR, (char*)buf + pos, EER_LEN_FOOTER) == 0)
		{
			REPORT_ERROR("Broken frame");
		}
	}
	std::reverse(frame_starts.begin(), frame_starts.end());
	std::reverse(frame_sizes.begin(), frame_sizes.end());
	RCTOC(TIMING_BUILD_INDEX);

	nframes = frame_starts.size();
	read_data = true;
}

void EERRenderer::lazyReadFrames()
{
	#pragma omp critical(EERRenderer_lazyReadFrames)
	{
		if (!read_data) // cannot return from within omp critical
		{	
			frame_starts.resize(nframes, 0);
			frame_sizes.resize(nframes, 0);
			buf = (unsigned char*)malloc(file_size); // This is big enough
			if (buf == NULL)
				REPORT_ERROR("Failed to allocate the buffer for " + fn_movie);
			long long pos = 0;

			// Read everything
			for (int frame = 0; frame < nframes; frame++)
			{
				TIFFSetDirectory(ftiff, frame);
				const int nstrips = TIFFNumberOfStrips(ftiff);
				frame_starts[frame] = pos;

				for (int strip = 0; strip < nstrips; strip++)
				{
					const int strip_size = TIFFRawStripSize(ftiff, strip);
					if (pos + strip_size >= file_size)
						REPORT_ERROR("EER: buffer overflow when reading raw strips.");

					TIFFReadRawStrip(ftiff, strip, buf + pos, strip_size);
					pos += strip_size;
					frame_sizes[frame] += strip_size;
				}
	#ifdef DEBUG_EER
				printf("EER in TIFF: Read frame %d from %s, nstrips = %d, current pos in buffer = %lld / %lld\n", frame, fn_movie.c_str(), nstrips, pos, file_size);
	#endif
			}

			TIFFClose(ftiff);

			read_data = true;
		}
	}
}

EERRenderer::~EERRenderer()
{
	if (buf != NULL)
		free(buf);
}

int EERRenderer::getNFrames()
{
	if (!ready)
		REPORT_ERROR("EERRenderer::getNFrames called before ready.");

	return nframes;
}

int EERRenderer::getWidth()
{
	if (!ready)
		REPORT_ERROR("EERRenderer::getNFrames called before ready.");

	return EER_IMAGE_WIDTH * EER_upsample;
}

int EERRenderer::getHeight()
{
	if (!ready)
		REPORT_ERROR("EERRenderer::getNFrames called before ready.");

	return EER_IMAGE_HEIGHT * EER_upsample;
}

template <typename T>
long long EERRenderer::renderFrames(int frame_start, int frame_end, MultidimArray<T> &image)
{
	if (!ready)
		REPORT_ERROR("EERRenderer::renderNFrames called before ready.");

	lazyReadFrames();

	if (frame_start <= 0 || frame_start > getNFrames() ||
	    frame_end < frame_start || frame_end > getNFrames())
	{
		std::cerr << "EERRenderer::renderFrames(frame_start = " << frame_start << ", frame_end = " << frame_end << "),  NFrames = " << getNFrames() << std::endl;
		REPORT_ERROR("Invalid frame range was requested.");
	}

	// Make this 0-indexed
	frame_start--;
	frame_end--;

	long long total_n_electron = 0;

	std::vector<unsigned int> positions;
	std::vector<unsigned char> symbols;
	image.initZeros(getHeight(), getWidth());

	for (int iframe = frame_start; iframe <= frame_end; iframe++)
	{
		RCTIC(TIMING_UNPACK_RLE);
		long long pos = frame_starts[iframe];
		long long n_pix = 0, n_electron = 0;
		const int max_electrons = (frame_sizes[iframe] * 8 + 11) / 12;
		if (positions.size() < max_electrons)
		{
			positions.resize(max_electrons);
			symbols.resize(max_electrons);
		}

		// unpack every two symbols = 12 bit * 2 = 24 bit = 3 byte
		//  |bbbbBBBB|BBBBaaaa|AAAAAAAA|
		// With SIMD intrinsics at the SSSE3 level, we can unpack 10 symbols (120 bits) simultaneously.
		unsigned char p1, p2, s1, s2;

		// Because there is a footer, it is safe to go beyond the limit by two bytes.
		long long pos_limit = frame_starts[iframe] + frame_sizes[iframe];
		while (pos < pos_limit)
		{
			// symbol is bit tricky. 0000YyXx; Y and X must be flipped.
			p1 = buf[pos];
			s1 = (buf[pos + 1] & 0x0F) ^ 0x0A; // 0x0F = 00001111, 0x0A = 00001010

			p2 = (buf[pos + 1] >> 4) | (buf[pos + 2] << 4);
			s2 = (buf[pos + 2] >> 4) ^ 0x0A;

			// Note the order. Add p before checking the size and placing a new electron.
			n_pix += p1;
			if (n_pix == EER_IMAGE_PIXELS) break;
			if (p1 < 255)
			{
				positions[n_electron] = n_pix;
				symbols[n_electron] = s1;
				n_electron++;
				n_pix++;
			}

			n_pix += p2;
			if (n_pix == EER_IMAGE_PIXELS) break;
			if (p2 < 255)
			{
				positions[n_electron] = n_pix;
				symbols[n_electron] = s2;
				n_electron++;
				n_pix++;
			}
#ifdef DEBUG_EER_DETAIL
			printf("%d: %u %u, %u %u %d\n", pos, p1, s1, p2, s2, n_pix);
#endif
			pos += 3;
		}

		if (n_pix != EER_IMAGE_PIXELS)
			REPORT_ERROR("Number of pixels is not right.");

		RCTOC(TIMING_UNPACK_RLE);

		RCTIC(TIMING_RENDER_ELECTRONS);
		if (EER_upsample == 2)
			render8K(image, positions, symbols, n_electron);
		else if (EER_upsample == 1)
			render4K(image, positions, symbols, n_electron);
		else
			REPORT_ERROR("Invalid EER upsamle");
		RCTOC(TIMING_RENDER_ELECTRONS);

		total_n_electron += n_electron;
#ifdef DEBUG_EER
		printf("Decoded %lld electrons / %d pixels from frame %5d.\n", n_electron, n_pix, iframe);
#endif
	}
#ifdef DEBUG_EER
	printf("Decoded %lld electrons in total.\n", total_n_electron);
#endif

#ifdef TIMING
	timer.printTimes(false);
#endif

	return total_n_electron;
}

// Instantiate for Polishing
template long long EERRenderer::renderFrames<float>(int frame_start, int frame_end, MultidimArray<float> &image);
template long long EERRenderer::renderFrames<unsigned short>(int frame_start, int frame_end, MultidimArray<unsigned short> &image);
template long long EERRenderer::renderFrames<char>(int frame_start, int frame_end, MultidimArray<char> &image);
