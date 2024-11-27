#pragma once
#include <iostream>
#include <vector>
#include <map>
#include <string>
#include <algorithm>
#include <cmath>
#include <immintrin.h>
#include <samplerate.h>

// Check if libsndfile sends aligned memory and accordingly change SIMD
// How pragma directives work


constexpr double PAD_EPS = 1e-6 * 1e-6;
constexpr float BASE_DBFS = -36.0f;
constexpr float BASE_SIL = -6.0f;
constexpr float MIN_SIL_THRESH = -6.0f;
constexpr int BASE_FFT_SR = 48000;
constexpr int BASE_NFFT = 1024;
constexpr int BASE_WIN_LENGTH = 1024;
constexpr int BASE_HOP_LENGTH = 256;
constexpr int SIMD_SIZE = 8;

typedef struct
{
    int n_fft;
    int win_length;
    int hop_length;
}FFTParams;


FFTParams* get_fft_params_from_sr(int sr){
    float ratio = static_cast<float>(sr) / BASE_FFT_SR;
    FFTParams *fftp = new FFTParams;
    fftp->n_fft = static_cast<int>(BASE_NFFT * ratio);
    fftp->win_length = static_cast<int>(BASE_WIN_LENGTH * ratio);
    fftp->hop_length = static_cast<int>(BASE_HOP_LENGTH * ratio);
    return fftp;
}

class WaveArray{
    public:
    WaveArray(float *buffer, long n_frames, int sr, int n_channels, FFTParams* fftp): buffer(buffer), n_frames(n_frames), sr(sr), fftp(fftp)
    {
        duration = n_frames / sr;
        resampler = src_new(SRC_SINC_BEST_QUALITY, 1, nullptr);
        if (!resampler){
            std::cerr << "Failed to set resampler." << std::endl;
            delete[] buffer;
        }
        if (n_channels == 2){
            stereo_to_mono();
        }
    }
    ~WaveArray(){
        cleanup();
    }

    void cleanup(){
        if (buffer != nullptr){
            delete[] buffer;
            buffer = nullptr;
        }
        if (resampler != nullptr){
            src_delete(resampler);
            resampler = nullptr;
        }
    }

    float *compute_rms(){
        int pad_size = static_cast<int>(fftp->win_length/2);
        int num_windows = (n_frames - fftp->win_length) / fftp->hop_length + 1;
        int num_padded_windows = ((n_frames + 2 * pad_size) - fftp->win_length) / fftp->hop_length + 1;
        int extra_windows = static_cast<int>((num_padded_windows - num_windows) /  2);
        float *rms_buf = new float[num_padded_windows];
        if (rms_buf == nullptr){
            return nullptr;
        }
        double sum_q;
        for (int i = 0; i < num_padded_windows; i++){
            sum_q = 0;
            int start_idx = i * fftp->hop_length;
            int end_idx = start_idx + fftp->win_length;

            for (int j = start_idx; j < end_idx; j++){
                if (j < extra_windows || j > n_frames + extra_windows){
                    sum_q += PAD_EPS;
                }
                else{
                    sum_q += buffer[j - extra_windows] * buffer[j - extra_windows];
                }
            }
            rms_buf[i] = to_db(std::sqrt(sum_q / fftp->win_length));
        }
        rms_windows = num_padded_windows;
        return rms_buf;
    }

    float *compute_rms_simd(){
        int pad_size = static_cast<int>(fftp->win_length/2);
        int num_windows = (n_frames - fftp->win_length) / fftp->hop_length + 1;
        int num_padded_windows = ((n_frames + 2 * pad_size) - fftp->win_length) / fftp->hop_length + 1;
        int extra_windows = static_cast<int>((num_padded_windows - num_windows) /  2);
        float *rms_buf = new float[num_padded_windows];
        if (rms_buf == nullptr){
            return nullptr;
        }  
        double sum_q;
        for (int i = 0; i < num_padded_windows; i++){
            sum_q = 0;
            int start_idx = i * fftp->hop_length;
            int end_idx = start_idx + fftp->win_length;
            int remainder = (end_idx - start_idx) % SIMD_SIZE;
            
            __m256 sum = _mm256_setzero_ps();
            for (int j = start_idx; j < end_idx - remainder; j++){
                __m256 sub_window  = _mm256_setzero_ps();
                if (j > extra_windows && j < n_frames + extra_windows){
                    sub_window = _mm256_loadu_ps(buffer + j - extra_windows);
                }
                else{
                    sub_window = _mm256_set1_ps(PAD_EPS);
                }
                __m256 squared = _mm256_mul_ps(sub_window, sub_window);
                sum = _mm256_add_ps(sum, squared);
            }
            float partial_sum[SIMD_SIZE];
            _mm256_storeu_ps(partial_sum, sum);
            for (int j = 0; j < SIMD_SIZE; j++){
                sum_q += partial_sum[j];
            }
            for (int j = end_idx - remainder; j < end_idx; j++){
                if (j < extra_windows || j > n_frames + extra_windows){
                    sum_q += PAD_EPS;
                }
                else{
                    sum_q += buffer[j - extra_windows] * buffer[j - extra_windows];
                }
            }
            rms_buf[i] = to_db(std::sqrt(sum_q / fftp->win_length));
        }
        rms_windows = num_padded_windows;
        return rms_buf;
    }
    int resample(int target_sr){
        double ratio = static_cast<double>(target_sr) / sr;
        int resampled_len = static_cast<int>(ratio * n_frames);
        float *resampled_buffer = new float[resampled_len];
        SRC_DATA src_data = {buffer, resampled_buffer, n_frames, resampled_len, 0, 0, 0, ratio};
        int result = src_process(resampler, &src_data);
        if (result != 0) {
            std::cerr << "Resampling failed" << result << std::endl;
            delete[] resampled_buffer;
            return 0;
        }
        else{
            delete[] buffer;
            buffer = resampled_buffer;
            n_frames = resampled_len;
            sr = target_sr;
            return 1;
        }
    }

    // Optimize using SIMD
    std::vector<int> get_vocal_windows(){
        float *rms = compute_rms();
        if (rms == nullptr){
            std::cerr << "Failed to compute RMS" << std::endl;
            return {};
        }
        set_dbfs(rms, rms_windows);
        std::vector<int> vocal_windows(rms_windows, 0);
        float sil_dbfs_thresh = compute_silence_threshold();
        for (int i = 0; i < rms_windows; i++){
            if (rms[i] > sil_dbfs_thresh){
                vocal_windows[i] = 1;
            }
        }
        delete[] rms;
        return vocal_windows;
    }
    float get_duration(){
        return duration;
    }
    float get_dbfs(){
        return dbfs;
    }
    int get_sr(){
        return sr;
    }
    int get_n_frames(){
        return n_frames;
    }
    float * get_buffer(){
        return buffer;
    }

    private:
    float duration;
    float dbfs;
    float *buffer;
    int sr;
    long n_frames;
    long rms_windows;
    FFTParams *fftp;
    SRC_STATE* resampler;
    void stereo_to_mono(){
        float *mono_buffer = new float[n_frames];
        for (long i = 0; i < n_frames; i++){
            mono_buffer[i] = (buffer[i * 2] + buffer[i * 2 + 1]) / 2.0f;
        }
        delete[] buffer;
        buffer = mono_buffer;
    }

    void stereo_to_mono_simd(){
        float *mono_buffer = new float[n_frames];
        int remainder = n_frames % SIMD_SIZE;
        for (int i = 0; i < n_frames - remainder; i++){
            // Why ref ptr
            __m256 left = _mm256_loadu_ps(buffer + i * 2);
            __m256 right = _mm256_loadu_ps(buffer + (i * 2 + 8));
            __m256 sum = _mm256_add_ps(left, right);
            __m256 avg = _mm256_mul_ps(sum, _mm256_set1_ps(0.5f));
            _mm256_storeu_ps(&mono_buffer[i], avg);

        }
        for (int i = n_frames - remainder; i < n_frames; i++){
            mono_buffer[i] = (buffer[i * 2] + buffer[i * 2 + 1]) / 2.0f;
        }
        delete[] buffer;
        buffer = mono_buffer;
    }

    void reset_buffer(float *new_buffer, long new_n_frames, int new_sr){
        delete[] buffer;
        buffer = new_buffer;
        n_frames = new_n_frames;
        sr = new_sr;
    }

    void set_dbfs(float *rms_buf, int size){
        double av = 0;
        for (int i = 0; i < size; i++){
            av += rms_buf[i];
        }
        dbfs = av / size;
    }

    float compute_silence_threshold(){
        float perc_diff = (BASE_DBFS - dbfs) / BASE_DBFS;
        float sil_thresh = BASE_SIL + (BASE_SIL * perc_diff);
        return std::min(sil_thresh, MIN_SIL_THRESH) + dbfs;
    }

    float to_db(float val) {
        return 20 * std::log10(std::max(val, std::numeric_limits<float>::min()));
    }   

    
};