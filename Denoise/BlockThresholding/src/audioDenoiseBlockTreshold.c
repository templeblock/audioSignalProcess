#include "audioDenoiseBlockTreshold.h"

#define _USE_MATH_DEFINES // to use macro M_PI in math.h
#include <math.h>

#define FORWARD_FFT 0
#define BACKWARD_FFT 1
#define SAFE_FREE(mem) do{if(!mem) free(mem);}while(0);

static const float m_lambda[3][5] = { { 1.5, 1.8, 2, 2.5, 2.5 },
									  { 1.8, 2, 2.5, 3.5, 3.5 },
									  { 2, 2.5, 3.5, 4.7, 4.7 } };

static void make_hanning_window(float *win, int32_t win_size)
{
	int32_t half_win = win_size / 2;
	for (int32_t i = 0; i < half_win; i++) {
		win[i] = 0.5 - 0.5*cos(2*M_PI*i/(win_size-1));
		win[win_size - 1 - i] = win[i];
	}
}

int32_t blockThreshold_init(MarsBlockThreshold_t *handle,
							int32_t time_win, int32_t fs)
{
	if (time_win<=0 || fs<=0 || !handle) {
		return MARS_ERROR_PARAMS;
	}

	// Compute hanning window
	handle->win_size = fs / 1000 * time_win;
	if (handle->win_size & 0x01) {
		handle->win_size += 1;// even window
	}
	handle->half_win_size = handle->win_size / 2;
	handle->win_hanning = (float *)malloc(sizeof(float) * (handle->win_size));
	if (!(handle->win_hanning)) {
		goto end;
	}
	make_hanning_window(handle->win_hanning, handle->win_size);

	//Compute block params
	handle->max_nblk_time = 8;
	handle->max_nblk_freq = 16;
	handle->nblk_time = 3;
	handle->nblk_freq = 5;
	handle->sigma_noise = 0.047;	
	handle->sigma_hanning_noise = handle->sigma_noise * sqrt(0.375);
	handle->macro_size = handle->half_win_size * handle->max_nblk_time;
	handle->have_nblk_time = 0;

	handle->SURE_matrix = (float **)malloc(sizeof(float *) * (handle->nblk_time));
	if (!(handle->SURE_matrix)) {
		goto end;
	}
	for (int32_t i = 0; i < handle->nblk_time; i++) {
		handle->SURE_matrix[i] = (float *)malloc(sizeof(float) * (handle->nblk_freq));
		memset(handle->SURE_matrix[i], 0, sizeof(float) * (handle->nblk_freq));
		if (!(handle->SURE_matrix[i])) {
			goto end;
		}
	}

	handle->inbuf = (float *)malloc(sizeof(float) * handle->win_size);
	if (!(handle->inbuf)) {
		goto end;
	}
	memset(handle->inbuf, 0, sizeof(float) * (handle->win_size));
	handle->inbuf_win = (float *)malloc(sizeof(float) * handle->win_size);
	if (!(handle->inbuf_win)) {
		goto end;
	}
	memset(handle->inbuf_win, 0, sizeof(float) * (handle->win_size));

	handle->outbuf = (float *)malloc(sizeof(float) * handle->macro_size);
	if (!(handle->outbuf)) {
		goto end;
	}
	memset(handle->outbuf, 0, sizeof(float) * (handle->macro_size));

	handle->stft_coef = (kiss_fft_cpx **)malloc(sizeof(kiss_fft_cpx *) * (handle->max_nblk_time));
	if (!(handle->stft_coef)) {
		goto end;
	}
	for (int32_t i = 0; i < handle->max_nblk_time; i++) {
		handle->stft_coef[i] = (kiss_fft_cpx *)malloc(sizeof(kiss_fft_cpx) * (handle->win_size));
		if (!(handle->stft_coef[i])) {
			goto end;
		}
	}
	handle->stft_thre = (kiss_fft_cpx **)malloc(sizeof(kiss_fft_cpx *) * (handle->max_nblk_time));
	if (!(handle->stft_thre)) {
		goto end;
	}
	for (int32_t i = 0; i < handle->max_nblk_time; i++) {
		handle->stft_thre[i] = (kiss_fft_cpx *)malloc(sizeof(kiss_fft_cpx) * (handle->win_size));
		if (!(handle->stft_thre[i])){
			goto end;
		}
	}
	handle->stft_coef_block = (kiss_fft_cpx **)malloc(sizeof(kiss_fft_cpx *) * (handle->max_nblk_time));
	if (!(handle->stft_coef_block)) {
		goto end;
	}
	for (int32_t i = 0; i < handle->max_nblk_time; i++){
		handle->stft_coef_block[i] = (kiss_fft_cpx *)malloc(sizeof(kiss_fft_cpx) * (handle->max_nblk_freq));
		if (!(handle->stft_coef_block[i])) {
			goto end;
		}
	}
	handle->stft_coef_block_norm = (kiss_fft_cpx **)malloc(sizeof(kiss_fft_cpx *) * (handle->max_nblk_time));
	if (!(handle->stft_coef_block_norm)) {
		goto end;
	}
	for (int32_t i = 0; i < handle->max_nblk_time; i++){
		handle->stft_coef_block_norm[i] = (kiss_fft_cpx *)malloc(sizeof(kiss_fft_cpx) * (handle->max_nblk_freq));
		if (!(handle->stft_coef_block_norm[i])) {
			goto end;
		}
	}

	handle->forward_fftr_cfg = kiss_fftr_alloc(handle->win_size, FORWARD_FFT, 0, 0);
	if (!(handle->forward_fftr_cfg)) {
		goto end;
	}
	handle->backward_fftr_cfg = kiss_fftr_alloc(handle->win_size, BACKWARD_FFT, 0, 0);
	if (!(handle->backward_fftr_cfg)) {
		goto end;
	}

	return MARS_OK;

end:
	SAFE_FREE(handle->win_hanning);
	if (handle->SURE_matrix) {
		for (int32_t i = 0; i < handle->nblk_time; i++) {
			SAFE_FREE(handle->SURE_matrix[i]);
		}
	}
	SAFE_FREE(handle->inbuf);
	SAFE_FREE(handle->outbuf);
	SAFE_FREE(handle->inbuf_win);
	if (handle->stft_coef) {
		for (int32_t i = 0; i < handle->max_nblk_time; i++) {
			SAFE_FREE(handle->stft_coef[i]);
		}
		SAFE_FREE(handle->stft_coef);
	}
	if (handle->stft_thre) {
		for (int32_t i = 0; i < handle->max_nblk_time; i++) {
			SAFE_FREE(handle->stft_thre[i]);
		}
		SAFE_FREE(handle->stft_thre);
	}
	if (handle->stft_coef_block) {
		for (int32_t i = 0; i < handle->max_nblk_time; i++) {
			SAFE_FREE(handle->stft_coef_block[i]);
		}
		SAFE_FREE(handle->stft_coef_block);
	}
	if (handle->stft_coef_block_norm) {
		for (int32_t i = 0; i < handle->max_nblk_time; i++) {
			SAFE_FREE(handle->stft_coef_block_norm[i]);
		}
		SAFE_FREE(handle->stft_coef_block_norm);
	}
	SAFE_FREE(handle->forward_fftr_cfg);
	SAFE_FREE(handle->backward_fftr_cfg);

	return MARS_ERROR_MEMORY;
}

// The conversion functions use the following naming convention:
// S16:      int16_t [-32768, 32767]
// Float:    float   [-1.0, 1.0]
// FloatS16: float   [-32768.0, 32767.0]
static int16_t FloatToS16(float v) {
	if (v > 0)
		return v >= 1 ? (INT16_MAX) :
		(int16_t)(v * (INT16_MAX) + 0.5f);
	return v <= -1 ? (INT16_MIN) : 
		(int16_t)(v * (INT16_MIN) - 0.5);
}

static float S16ToFloat(int16_t v) {
	static const float kMaxInt16Inverse = 1.f / (INT16_MAX);
	static const float kMinInt16Inverse = 1.f / (INT16_MIN);
	return v * (v > 0 ? kMaxInt16Inverse : -kMinInt16Inverse);
}

static void blockThreshold_STFT(MarsBlockThreshold_t *handle)
{
	//filter with window
	for (int32_t i = 0; i < handle->win_size; i++) {
		(handle->inbuf_win)[i] = (handle->inbuf)[i] * (handle->win_hanning)[i];
	}

	kiss_fftr(handle->forward_fftr_cfg, handle->inbuf_win, 
			 handle->stft_coef[handle->have_nblk_time]);
}

static void blockThreshold_inverse_STFT(MarsBlockThreshold_t *handle)
{
	int32_t half_win_size = handle->half_win_size;

	for (int32_t i = 0; i < handle->max_nblk_time; i++) {
		kiss_fftri(handle->backward_fftr_cfg, handle->inbuf_win, handle->stft_thre[i]);
		for (int32_t j = 0; j < handle->win_size; j++) {
			handle->outbuf[half_win_size*i + j] += handle->inbuf_win[j] / (handle->win_size);
		}
	}
}

// calculate the power of STFT in block [row_start:row_end, col_start:col_end]
static float power_STFT(kiss_fft_cpx **data,
						int32_t row_start, int32_t row_end,
						int32_t col_start, int32_t col_end)
{
	float sum = 0.0;
	
	for (int32_t row = row_start; row <= row_end; row++) {
		for (int32_t col = col_start; col <= col_end; col++) {
			sum += pow(data[row][col].r, 2) + pow(data[row][col].i, 2);
		}
	}

	return sum;
}

// calculate the energy of STFT in real dimension
static float energy_real_STFT(kiss_fft_cpx **data,
								int32_t row_start, int32_t row_end,
								int32_t col_start, int32_t col_end)
{
	float sum = 0.0;
	
	for (int32_t row = row_start; row <= row_end; row++){
		for (int32_t col = col_start; col <= col_end; col++){
			sum += pow(data[row][col].r, 2);
		}
	}

	return sum;
}

// implement scalar multiply: dst_matrix = src_matrix * a
static void scalar_multiply(kiss_fft_cpx **dst_matrix, kiss_fft_cpx **src_matrix,
							int32_t row_start, int32_t row_end, 
							int32_t col_start, int32_t col_end,
							float a)
{
	for (int32_t row = row_start; row <= row_end; row++) {
		for (int32_t col = col_start; col <= col_end; col++) {
			dst_matrix[row][col].r = src_matrix[row][col].r * a;
			dst_matrix[row][col].i = src_matrix[row][col].i * a;
		}
	}
}

static void blockThreshold_adaptive_block(MarsBlockThreshold_t *handle,
										  int32_t ith_half_macroblk_frq,
										  int32_t *seg_time, int32_t *seg_freq)
{
	float SURE_real = 0.0;
	float energy_real = 0.0;
	float size_blk = 0.0;
	float min_SURE_real = (handle->max_nblk_time) * (handle->max_nblk_freq);
	float lambda = 0.0;
	int32_t TT, FF;
	float norm = sqrt(2.0) / (sqrt(handle->win_size) * (handle->sigma_hanning_noise));

	//Get STFT coef macro block and block norm
	for (int32_t index_blk_time = 0; index_blk_time < handle->max_nblk_time; index_blk_time++) {
		int32_t index_blk_freq = 1 + ith_half_macroblk_frq * (handle->max_nblk_freq);
		for (int32_t i = 0; i < handle->max_nblk_freq; i++) {
			(handle->stft_coef_block)[index_blk_time][i] = 
								(handle->stft_coef)[index_blk_time][index_blk_freq + i];
			(handle->stft_coef_block_norm)[index_blk_time][i].r =
								(handle->stft_coef_block)[index_blk_time][i].r * norm;
			(handle->stft_coef_block_norm)[index_blk_time][i].i =
								(handle->stft_coef_block)[index_blk_time][i].i * norm;
		}
	}

	//Compute adaptive block
	for (int32_t T = 0; T < handle->nblk_time; T++) {//loop over time 
		TT = (handle->max_nblk_time) * pow(2.0, -T);
		for (int32_t F = 0; F < handle->nblk_freq; F++) {//loop over frequency
			FF = (handle->max_nblk_freq) * pow(2.0, -F);
			lambda = m_lambda[T][F];
			SURE_real = 0.0;
			for (int32_t ii = 0; ii < pow(2.0, T); ii++){
				for (int32_t jj = 0; jj < pow(2.0, F); jj++) {
					//scalar_multiply(handle->stft_coef_block_norm,
					//				handle->stft_coef_block,
					//				TT*ii, TT*(ii + 1) - 1,
					//				FF*jj, FF*(jj + 1) - 1,
					//				norm);
					size_blk = TT * FF;
					energy_real = energy_real_STFT(handle->stft_coef_block_norm,
													TT*ii, TT*(ii + 1) - 1,
													FF*jj, FF*(jj + 1) - 1);
					SURE_real += size_blk + 
						         (pow(lambda, 2) * pow(size_blk, 2) - 2 * lambda*size_blk*(size_blk - 2))
									/ energy_real*(energy_real>lambda*size_blk)
									+ (energy_real - 2 * size_blk)*(energy_real <= lambda*size_blk);
				}
			}
			// update segmentation based on SURE_real
			//if (SURE_real < min_SURE_real) {
			//	min_SURE_real = SURE_real;
			//	*seg_time = T;
			//	*seg_freq = F;
			//}
			handle->SURE_matrix[T][F] = SURE_real;
		}
	}

	// find mini SURE segmentation
	min_SURE_real = handle->SURE_matrix[0][0];
	*seg_time = 0;
	*seg_freq = 0;
	for (int32_t i = 0; i < handle->nblk_time; i++) {
		for (int32_t j = 0; j < handle->nblk_freq; j++) {
			if (handle->SURE_matrix[i][j] < min_SURE_real) {
				min_SURE_real = handle->SURE_matrix[i][j];
				*seg_time = i;
				*seg_freq = j;
			}
		}
	}
}

static void blockTreshold_compute_thre(MarsBlockThreshold_t *handle,
											int32_t ith_half_macro_freq,
											int32_t seg_time,
											int32_t seg_freq)
{
	int32_t TT = (handle->max_nblk_time) * pow(2.0, -seg_time);
	int32_t FF = (handle->max_nblk_freq) * pow(2.0, -seg_freq);
	float a = 0.0;
	float lambda = m_lambda[seg_time][seg_freq];

	for (int32_t ii = 0; ii < pow(2.0, seg_time); ii++) {
		for (int32_t jj = 0; jj < pow(2.0, seg_freq); jj++) {
			a = lambda * TT * FF * pow(handle->sigma_hanning_noise, 2) * (handle->win_size);
			a = 1.0 - a / power_STFT(handle->stft_coef_block,
									TT*ii, TT*(ii + 1) - 1,
									FF*jj, FF*(jj + 1) - 1);
			a = a * (a > 0);

			// udpate attenuation map
			int32_t idx_base = 1 + ith_half_macro_freq * (handle->max_nblk_freq);
			int32_t idx_row, idx_col;
			for (int32_t kk = 0; kk < TT; kk++) {
				for (int32_t ww = 0; ww < FF; ww++) {
					idx_row = ii * TT + kk;
					idx_col = jj * FF + ww;
					(handle->stft_thre)[idx_row][idx_base + idx_col].r = 
							(handle->stft_coef_block)[idx_row][idx_col].r * a;
					(handle->stft_thre)[idx_row][idx_base + idx_col].r =
							(handle->stft_coef_block)[idx_row][idx_base + idx_col].r * a;
				}
			}
 		}
	}
}

// repaire positive frequency by conjugate from negative frequency
static void blockThreshold_repair_positive_freq(MarsBlockThreshold_t *handle)
{
	int32_t fft_size = handle->win_size;
	int32_t half_num_freq = (fft_size + 1) / 2;
	for (int32_t i = 0; i < handle->max_nblk_time; i++) {
		for (int32_t j = 1; j < half_num_freq; j++) {
			handle->stft_thre[i][fft_size - j].r = handle->stft_thre[i][j].r;
			handle->stft_thre[i][fft_size - j].i = -(handle->stft_thre[i][j].i);
		}
	}
}

static void blockThreshold_wiener(MarsBlockThreshold_t *handle)
{
	float wiener = 1.0;
	for (int32_t t = 0; t < handle->max_nblk_time; t++){
		for (int32_t f = 0; f < handle->win_size; f++) {
			wiener = pow((handle->stft_thre)[t][f].r, 2) + pow((handle->stft_thre)[t][f].i, 2);
			wiener = wiener / (wiener + (handle->win_size) * pow(handle->sigma_hanning_noise, 2));
			handle->stft_coef[t][f].r *= wiener;
			handle->stft_coef[t][f].i *= wiener;
		}
	}
}

static void blockThreshold_core(MarsBlockThreshold_t *handle)
{
	float L_pi = 8.0;
	float Lambda_pi = 2.5;
	float a = 0.0;
	int32_t half_nb_macroblk_frq = (handle->win_size - 1) / 2 / (handle->max_nblk_freq);
	int32_t seg_time = 0;
	int32_t seg_freq = 0;
	int32_t idx_freq_last = 0;

	// DC part
	a = 1 - (Lambda_pi*L_pi*pow(handle->sigma_hanning_noise,2)*(handle->win_size)) 
			/ power_STFT(handle->stft_coef, 0, handle->max_nblk_time-1, 0, 0);
	if (a < 0) {
		a = 0;
	}
	scalar_multiply(handle->stft_thre, handle->stft_coef, 0, handle->max_nblk_time-1, 0, 0, a);

	// negative frequency part
	for (int32_t i = 0; i < half_nb_macroblk_frq; i++){
		//adaptive block
		blockThreshold_adaptive_block(handle, i, &seg_time, &seg_freq);

		//compute the attenuation map base on adaptive block segmenation
		blockTreshold_compute_thre(handle, i, seg_time, seg_freq);
	}

	// for last few frequency that do not match 2D MarcroBlock
	idx_freq_last = 1 + half_nb_macroblk_frq * (handle->max_nblk_freq);
	if (idx_freq_last <= (handle->win_size + 1) / 2){
		for (int32_t i = idx_freq_last; i < (handle->win_size + 1) / 2; i++) {
			a = Lambda_pi*L_pi*pow(handle->sigma_hanning_noise, 2)*(handle->win_size);
			a = 1 - a / power_STFT(handle->stft_coef, 0, handle->max_nblk_time-1, i, i);
			scalar_multiply(handle->stft_thre, handle->stft_coef,
							0, handle->max_nblk_time - 1,
							i, i, a);
		}
	}

	// positive frequency part, conjugate from negative frequency
	blockThreshold_repair_positive_freq(handle);

	// wiener filter
	blockThreshold_wiener(handle);
}

int32_t blockThreshold_denoise(MarsBlockThreshold_t *handle,
								int16_t *in, int32_t in_len)
{
	if ((in_len != handle->half_win_size) || (!in)) {
		return MARS_ERROR_PARAMS;
	}

	// Prepare inbuf
	int32_t half_win_size = handle->half_win_size;
	memcpy(handle->inbuf, handle->inbuf + half_win_size, sizeof(float) * half_win_size);
	for (int32_t i = 0; i < half_win_size; i++) {
		handle->inbuf[half_win_size + i] = S16ToFloat(in[i]);
	}

	// do STFT
	blockThreshold_STFT(handle);

	(handle->have_nblk_time)++;

	if (handle->have_nblk_time != handle->max_nblk_time) {
		return MARS_NEED_MORE_SAMPLES;
	}
	// block thresholding
	blockThreshold_core(handle);

	// do inverse STFT
	blockThreshold_inverse_STFT(handle);
	
	handle->have_nblk_time = 0;

	return MARS_CAN_OUTPUT;
}

int32_t blockThreshold_output(MarsBlockThreshold_t *handle,
						   int16_t *out, int32_t out_len)
{
	if (out_len < handle->macro_size) {
		return 0;
	}

	for (int32_t i = 0; i < handle->macro_size; i++) {
		out[i] = FloatToS16((handle->outbuf)[i]);
	}

	return handle->macro_size;
}

void blockThreshold_free(MarsBlockThreshold_t *handle)
{
	SAFE_FREE(handle->win_hanning);
	if (handle->SURE_matrix) {
		for (int32_t i = 0; i < handle->nblk_time; i++) {
			SAFE_FREE(handle->SURE_matrix[i]);
		}
	}
	SAFE_FREE(handle->inbuf);
	SAFE_FREE(handle->outbuf);
	SAFE_FREE(handle->inbuf_win);
	if (handle->stft_coef) {
		for (int32_t i = 0; i < handle->max_nblk_time; i++) {
			SAFE_FREE(handle->stft_coef[i]);
		}
		SAFE_FREE(handle->stft_coef);
	}
	if (handle->stft_thre) {
		for (int32_t i = 0; i < handle->max_nblk_time; i++) {
			SAFE_FREE(handle->stft_thre[i]);
		}
		SAFE_FREE(handle->stft_thre);
	}
	if (handle->stft_coef_block) {
		for (int32_t i = 0; i < handle->max_nblk_time; i++) {
			SAFE_FREE(handle->stft_coef_block[i]);
		}
		SAFE_FREE(handle->stft_coef_block);
	}
	if (handle->stft_coef_block_norm) {
		for (int32_t i = 0; i < handle->max_nblk_time; i++) {
			SAFE_FREE(handle->stft_coef_block_norm[i]);
		}
		SAFE_FREE(handle->stft_coef_block_norm);
	}
	SAFE_FREE(handle->forward_fftr_cfg);
	SAFE_FREE(handle->backward_fftr_cfg);
}

int32_t blockThreshold_max_output(const MarsBlockThreshold_t *handle)
{
	return handle->macro_size;
}

int32_t blockThreshold_samples_per_time(const MarsBlockThreshold_t *handle)
{
	return handle->half_win_size;
}