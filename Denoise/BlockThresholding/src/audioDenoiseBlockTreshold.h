#ifndef AUDIO_DENOISE_BLOCK_THRESHOLD_H_
#define AUDIO_DENOISE_BLOCK_THRESHOLD_H_

#include <stdint.h>
#include "../../../common/kiss_fft/kiss_fft.h"

#define MARS_OK				0x00
#define MARS_ERROR_MEMORY	0x01
#define MARS_ERROR_PARAMS	0x02
#define MARS_NEED_MORE_SAMPLES 0x10

typedef struct MarsBlockThreshold{
	int32_t win_size;	// window size 
	int32_t half_win_size; // half window size
	float *win_hanning; // hanning window

	int32_t max_nblk_time;
	int32_t max_nblk_freq;
	int32_t nblk_time;  // the number of block in time dimension
	int32_t nblk_freq;  // the number of block in frequency dimension
	int32_t macro_size; // the number of sample in one macro block
	int32_t have_nblk_time;

	float sigma_noise;  // assumption the sigma of gaussian white noise
	float *inbuf;       // internal buffer for keep one window size input samples
	kiss_fft_cpx *inbuf_win;   
	float *outbuf;      // internal buffer for keep one macro block output samples
	int32_t num_inbuf;  //the number of samples in inbuf
	int32_t output_ready; // flag for whether output or not

	kiss_fft_cpx **stft_coef;
	kiss_fft_cfg forward_fft_cfg;
	kiss_fft_cfg backward_fft_cfg;
}MarsBlockThreshold_t;

/*
 * time_win: ms
 * fs: sample rate
 */
int32_t blockThreshold_init(MarsBlockThreshold_t *handle,
							int32_t time_win, int32_t fs);

int32_t blockThreshold_denoise(MarsBlockThreshold_t *handle,
							    int16_t *in, int32_t in_len);

void blockThreshold_output(MarsBlockThreshold_t *handle,
							int16_t *out, int32_t *out_len);

void blockThreshold_flush(MarsBlockThreshold_t *handle, 
						int16_t *out, int32_t *out_len);

void blockThreshold_free(MarsBlockThreshold_t *handle);

#endif