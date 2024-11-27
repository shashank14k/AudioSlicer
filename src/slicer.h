#pragma once
#include <iostream>
#include <string>
#include <algorithm>
#include <cmath>
#include <vector>
#include <cstring>
#include <sndfile.h>
#include "wave.h"

constexpr float BUFFER_LIMIT_DURATION = 60.0f;

class Segment{
    public:
        Segment(float start, float end, int vocal)
        : start(start), end(end), vocal(vocal) {};
    
        void extend(float step){
            end += step;
        }
    private:
        float start;
        float end;
        int vocal;

};


class AudioSegmentor {
public:
    AudioSegmentor(const char* fpath, std::string save_dir, int processing_sr, 
                   float min_vocal_length = 0.15f, 
                   float perc_vocal_windows = 0.2f)
        : save_dir(save_dir),
          processing_sr(processing_sr), 
          min_vocal_length(min_vocal_length), 
          perc_vocal_windows(perc_vocal_windows),
          window_pad(3)
    {
        fftp = get_fft_params_from_sr(processing_sr);
        start = -BUFFER_LIMIT_DURATION;
        sf_info = new SF_INFO();
        sndfile = sf_open(fpath, SFM_READ, sf_info);
        end = static_cast<double>(sf_info->frames) / sf_info->samplerate;
        min_buffer_len = static_cast<int>((min_vocal_length * processing_sr) / fftp->hop_length);
        n_vocal_windows = static_cast<int>(perc_vocal_windows * min_buffer_len);
        n_vocal_splits = 0;
    }
    ~AudioSegmentor(){};
    int split_vocals(){
        while(true){
            WaveArray* wave = load_next_buffer();
            if (wave==nullptr){
                if (start < end){
                    std::cerr<< "Slicing failed" << std::endl;
                    return 0;
                }
                else {
                    return 1;
                }
            }
            split_array(wave);
        }
    }


private:
    std::string save_dir;
    int processing_sr;        // Sampling rate for processing
    float perc_vocal_windows; // Percentage of vocal windows
    int window_pad;           // Padding for windows
    float min_vocal_length;   // Minimum vocal length
    int min_buffer_len;
    int n_vocal_windows;
    int n_vocal_splits;
    float max_dbf;
    float start;
    float end;
    SF_INFO *sf_info;
    SNDFILE* sndfile;
    FFTParams* fftp;
    WaveArray* load_next_buffer(){
        start += BUFFER_LIMIT_DURATION;
        while (start < end){
            long start_frame = static_cast<long>(start * sf_info->samplerate);
            long end_frame = static_cast<long>(std::min(start + BUFFER_LIMIT_DURATION, end) * sf_info->samplerate);
            long num_frames = end_frame - start_frame;
            if (sf_seek(sndfile, start_frame, SEEK_SET) == -1){
                std::cerr << "Failed to seek frames in audio file" << std::endl;
                return nullptr;
            }
            float *buffer = new float[num_frames * sf_info->channels];
            long frames_read = sf_readf_float(sndfile, buffer, num_frames);
            if (frames_read < num_frames) {
                std::cerr << "Warning: Expected to read " << num_frames << "frames, but could read" << frames_read << "frames." << std::endl;
                return nullptr;
            }
            float max_val = 1e-6;
            for (long i = 0; i < frames_read * sf_info->channels; ++i) {
                max_val = std::max(max_val, std::fabs(buffer[i]));
            }
            if (max_val > 0.0f) {
                for (long i = 0; i < frames_read * sf_info->channels; ++i) {
                    buffer[i] /= max_val;
                }
            }
            // Reset constructor instead of creating a new object everytime
            WaveArray *wave = new WaveArray(buffer, frames_read, sf_info->samplerate, sf_info->channels, fftp);
            int res = wave->resample(processing_sr);
            if (!res){
                std::cerr << "Resampling failed, attempting to slice at current sample rate " << sf_info->samplerate << std::endl;
            }
            return wave;
        }
        return nullptr;
    }

    std::string get_save_filename(){
        std::string fname;
        fname = save_dir + "/vocal_" + std::to_string(n_vocal_splits) + ".wav";
        return fname;
    }

    void save_vocals(WaveArray *wave, int vocal_start_window, int vocal_end_window){
        int start_sample = vocal_start_window * fftp->hop_length;
        int end_sample = vocal_end_window * fftp->hop_length;
        int save_frames = end_sample - start_sample;
        SF_INFO *sfinfo = new SF_INFO();
        sfinfo->samplerate = processing_sr;
        sfinfo->frames = save_frames;
        sfinfo->channels = 1;
        sfinfo->format = SF_FORMAT_WAV | SF_FORMAT_FLOAT;
        std::string fname = get_save_filename();

        SNDFILE* outfile = sf_open(fname.c_str(), SFM_WRITE, sfinfo);
        if (!outfile) {
            throw std::runtime_error("Error opening file for writing.");
        }
        float* buf = wave->get_buffer();
        float *save_buffer = new float[end_sample - start_sample];
        std::memcpy(save_buffer, buf + start_sample, save_frames * sizeof(float));
        std::cout << "Writing split " << n_vocal_splits << "starting at " << start_sample << "to " << fname << std::endl;
        sf_write_float(outfile, save_buffer, save_frames);
        sf_close(outfile);

    }

    int get_sil_start_idx(std::vector<int> vocal_window, int curr_idx){
        int sil_start = curr_idx;
        while (vocal_window[sil_start] != 1 && sil_start >= curr_idx - min_buffer_len){
            sil_start --;
        }
        return std::max(sil_start + window_pad, curr_idx);
    }

    int get_vocal_start_idx(std::vector<int> vocal_window, int curr_idx){
        int vocal_start = curr_idx;
        while (vocal_window[vocal_start] !=0 && vocal_start >= curr_idx - min_buffer_len){
            vocal_start --;
        }
        return std::max(vocal_start + window_pad, curr_idx);
    }
    
    void split_array(WaveArray* wave){
        std::vector<int> vocal_windows = wave->get_vocal_windows();
        if (vocal_windows.empty()){
            std::cerr << "Failed to slice audio " << std::endl;
            wave->cleanup();
        }
        int window_sum = 0;
        int ptr = 0;
        int vocal, curr_vocal, change, curr_start_idx;
        float curr_window_duration;
        for (int i = ptr; i < ptr + min_buffer_len; i++){
            window_sum += vocal_windows[i];
        }
        vocal = window_sum > n_vocal_windows ? 1 : 0;
        ptr += min_buffer_len;
        curr_vocal = vocal;
        curr_start_idx = 0;
        curr_window_duration = (static_cast<float>(fftp->hop_length) * min_buffer_len) / processing_sr;
        
        while (ptr < vocal_windows.size()){
            window_sum = window_sum + vocal_windows[ptr] - vocal_windows[ptr - min_buffer_len];
            vocal = window_sum > n_vocal_windows ? 1 : 0;
            if (vocal != curr_vocal && curr_window_duration > min_vocal_length){
                change = 1;
            }
            else{
                change = 0;
            }

            if (!change){
                curr_window_duration += static_cast<float>(fftp->hop_length) / processing_sr;
            }
            else{
                if (curr_vocal){
                    int vocal_end = get_sil_start_idx(vocal_windows, ptr);
                    save_vocals(wave, curr_start_idx, vocal_end);
                    curr_start_idx = vocal_end;
                    n_vocal_splits += 1;
                }
                else{
                    int sil_end = get_vocal_start_idx(vocal_windows, ptr);
                    curr_start_idx = sil_end;
                }
                curr_vocal = vocal;
                curr_window_duration = static_cast<float>(fftp->hop_length) / processing_sr; 
            }
            ptr += 1;
        }
        if (curr_vocal){
            save_vocals(wave, curr_start_idx, ptr);
            n_vocal_splits += 1;
        }
    }
};
