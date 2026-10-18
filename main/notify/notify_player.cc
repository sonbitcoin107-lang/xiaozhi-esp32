#include "notify_player.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <memory>
#include <vector>

#include <esp_log.h>

#include "board.h"
#include "esp_ae_rate_cvt.h"
#include "esp_audio_types.h"
#include "esp_mp3_dec.h"
#include "http.h"
#include "ogg_demuxer.h"

namespace {
constexpr int kHttpTimeoutMs = 15000;
constexpr size_t kHttpReadBufferSize = 1024;
constexpr uint32_t kNotifyTaskStackSize = 10240;
constexpr UBaseType_t kNotifyTaskPriority = 2;
const char* TAG = "NotifyPlayer";

bool IsSupportedUrl(const std::string& url) {
    return url.compare(0, 7, "http://") == 0 || url.compare(0, 8, "https://") == 0;
}
}  // namespace

NotifyPlayer::NotifyPlayer(AudioService& audio_service) : audio_service_(audio_service) {}

NotifyPlayer::~NotifyPlayer() { Stop(); }

bool NotifyPlayer::Start(std::string audio_url, std::vector<NotifySubtitle> subtitles,
                         uint32_t playback_id, SubtitleCallback subtitle_callback,
                         FinishedCallback finished_callback) {
    if (playback_id == 0 || !IsSupportedUrl(audio_url)) {
        return false;
    }

    std::stable_sort(subtitles.begin(), subtitles.end(),
                     [](const NotifySubtitle& left, const NotifySubtitle& right) {
                         return left.start_ms < right.start_ms;
                     });

    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (active_ || worker_running_) {
            return false;
        }
        audio_url_ = std::move(audio_url);
        subtitles_ = std::move(subtitles);
        displayed_text_.clear();
        subtitle_callback_ = std::move(subtitle_callback);
        finished_callback_ = std::move(finished_callback);
        playback_id_ = playback_id;
        last_playback_position_ms_ = 0;
        underrun_count_ = 0;
        next_subtitle_index_ = 0;
        active_ = true;
        worker_running_ = true;
        cancelled_ = false;
        http_finished_ = false;
        stream_started_ = false;
        playback_drained_ = false;
        completion_reported_ = false;
    }

    BaseType_t created = xTaskCreate(WorkerEntry, "notify_http", kNotifyTaskStackSize, this,
                                     kNotifyTaskPriority, &task_handle_);
    if (created != pdPASS) {
        ESP_LOGE(TAG, "Failed to create notification HTTP task");
        std::lock_guard<std::mutex> lock(mutex_);
        active_ = false;
        worker_running_ = false;
        cancelled_ = true;
        task_handle_ = nullptr;
        return false;
    }
    return true;
}

void NotifyPlayer::Stop() {
    std::lock_guard<std::mutex> lock(mutex_);
    cancelled_ = true;
    active_ = false;
    subtitles_.clear();
    subtitle_callback_ = nullptr;
    finished_callback_ = nullptr;
}

bool NotifyPlayer::IsActive(uint32_t playback_id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    return active_ && (playback_id == 0 || playback_id == playback_id_);
}

bool NotifyPlayer::IsBusy() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return active_ || worker_running_;
}

bool NotifyPlayer::IsCancelled(uint32_t playback_id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    return cancelled_ || !active_ || playback_id != playback_id_;
}

void NotifyPlayer::OnPlaybackProgress(uint32_t playback_id, uint32_t media_position_ms) {
    SubtitleCallback callback;
    std::string text;
    bool subtitle_changed = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!active_ || playback_id != playback_id_) {
            return;
        }
        last_playback_position_ms_ = media_position_ms;
        while (next_subtitle_index_ < subtitles_.size() &&
               subtitles_[next_subtitle_index_].start_ms <= media_position_ms) {
            text = subtitles_[next_subtitle_index_].text;
            ++next_subtitle_index_;
            subtitle_changed = true;
        }
        if (!subtitle_changed || text == displayed_text_) {
            return;
        }
        displayed_text_ = text;
        callback = subtitle_callback_;
    }
    if (callback) {
        callback(playback_id, text);
    }
}

NotifyPlayer::FinishedCallback NotifyPlayer::CompleteLocked(uint32_t& playback_id) {
    if (completion_reported_ || !active_) {
        return nullptr;
    }
    completion_reported_ = true;
    active_ = false;
    playback_id = playback_id_;
    return finished_callback_;
}

void NotifyPlayer::OnPlaybackDrained() {
    FinishedCallback callback;
    uint32_t playback_id = 0;
    uint32_t underrun_count = 0;
    uint32_t media_position_ms = 0;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!active_) {
            return;
        }
        playback_drained_ = true;
        if (http_finished_) {
            callback = CompleteLocked(playback_id);
        } else if (stream_started_) {
            underrun_count = ++underrun_count_;
            media_position_ms = last_playback_position_ms_;
        }
    }
    if (underrun_count != 0) {
        ESP_LOGW(TAG, "Notification playback underrun #%lu at %lu ms",
                 static_cast<unsigned long>(underrun_count),
                 static_cast<unsigned long>(media_position_ms));
    }
    if (callback) {
        callback(playback_id, true);
    }
}

void NotifyPlayer::WorkerEntry(void* arg) {
    auto* player = static_cast<NotifyPlayer*>(arg);
    player->WorkerTask();
    vTaskDelete(nullptr);
}

void NotifyPlayer::WorkerTask() {
    std::string audio_url;
    uint32_t playback_id = 0;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        audio_url = audio_url_;
        playback_id = playback_id_;
    }

    bool success = false;
    auto http = Board::GetInstance().GetNetwork()->CreateHttp(0);
    if (http) {
        http->SetTimeout(kHttpTimeoutMs);
        http->SetHeader("Accept", "audio/ogg, application/ogg, audio/mpeg, audio/mp3, */*");
        http->SetHeader("Accept-Encoding", "identity");
        auto opened = http->Open("GET", audio_url);
        if (opened) {
            auto status = http->GetStatusCode();
            if (status && *status >= 200 && *status < 300 && !IsCancelled(playback_id)) {
                std::array<char, kHttpReadBufferSize> buffer;
                auto first_read = http->Read(buffer.data(), buffer.size());
                if (!first_read || *first_read <= 0) {
                    ESP_LOGE(TAG, "Notification HTTP first read failed");
                } else {
                    const bool is_ogg = (*first_read >= 4 &&
                                         buffer[0] == 'O' && buffer[1] == 'g' &&
                                         buffer[2] == 'g' && buffer[3] == 'S');

                    if (is_ogg) {
                        ESP_LOGI(TAG, "Detected OGG OPUS stream, starting OggDemuxer");
                        auto demuxer = std::make_unique<OggDemuxer>();
                        uint32_t media_position_ms = 0;
                        bool packet_error = false;
                        demuxer->OnPacket(
                            [this, playback_id, &media_position_ms, &packet_error](
                                const uint8_t* data, int sample_rate, int frame_duration_ms, size_t size) {
                                if (packet_error || IsCancelled(playback_id)) {
                                    packet_error = true;
                                    return;
                                }

                                auto packet = std::make_unique<AudioStreamPacket>();
                                packet->sample_rate = sample_rate;
                                packet->frame_duration = frame_duration_ms;
                                packet->playback_id = playback_id;
                                packet->media_position_ms = media_position_ms;
                                packet->payload.assign(data, data + size);

                                {
                                    std::lock_guard<std::mutex> lock(mutex_);
                                    if (cancelled_ || !active_ || playback_id != playback_id_) {
                                        packet_error = true;
                                        return;
                                    }
                                    playback_drained_ = false;
                                }

                                if (!audio_service_.PushPacketToDecodeQueue(std::move(packet), true)) {
                                    packet_error = true;
                                    return;
                                }
                                {
                                    std::lock_guard<std::mutex> lock(mutex_);
                                    stream_started_ = true;
                                }
                                media_position_ms += frame_duration_ms;
                            });

                        demuxer->Process(reinterpret_cast<const uint8_t*>(buffer.data()), *first_read);
                        if (demuxer->HasError()) {
                            packet_error = true;
                        }

                        while (!packet_error && !IsCancelled(playback_id)) {
                            auto size = http->Read(buffer.data(), buffer.size());
                            if (!size) {
                                ESP_LOGE(TAG, "Notification HTTP read failed: %s",
                                         size.error().ToString().c_str());
                                break;
                            }
                            if (*size == 0) {
                                success = demuxer->Finish();
                                if (!success) {
                                    ESP_LOGE(TAG,
                                             "Notification Ogg stream ended before a complete audio stream");
                                }
                                break;
                            }
                            demuxer->Process(reinterpret_cast<const uint8_t*>(buffer.data()), *size);
                            if (demuxer->HasError()) {
                                packet_error = true;
                            }
                        }
                    } else {
                        ESP_LOGI(TAG, "Detected MP3 audio stream, starting MP3 decoder");
                        void* mp3_decoder = nullptr;
                        auto mp3_err = esp_mp3_dec_open(nullptr, 0, &mp3_decoder);
                        if (mp3_err != ESP_AUDIO_ERR_OK || mp3_decoder == nullptr) {
                            ESP_LOGE(TAG, "Failed to create MP3 decoder: %d", mp3_err);
                        } else {
                            esp_ae_rate_cvt_handle_t resampler = nullptr;
                            uint32_t resampler_src_rate = 0;
                            const int target_rate = audio_service_.GetOutputSampleRate();
                            std::vector<int16_t> pcm_raw(2304);
                            std::vector<uint8_t> in_ring;
                            in_ring.reserve(4096);
                            uint32_t media_position_ms = 0;

                            size_t offset = 0;
                            if (*first_read >= 10 && buffer[0] == 'I' && buffer[1] == 'D' && buffer[2] == '3') {
                                uint32_t id3_size = ((static_cast<uint8_t>(buffer[6]) & 0x7F) << 21) |
                                                    ((static_cast<uint8_t>(buffer[7]) & 0x7F) << 14) |
                                                    ((static_cast<uint8_t>(buffer[8]) & 0x7F) << 7)  |
                                                    (static_cast<uint8_t>(buffer[9]) & 0x7F);
                                uint32_t total_id3 = id3_size + 10;
                                ESP_LOGI(TAG, "Skipping ID3v2 header: %lu bytes",
                                         static_cast<unsigned long>(total_id3));
                                if (total_id3 < static_cast<uint32_t>(*first_read)) {
                                    offset = total_id3;
                                } else {
                                    uint32_t remaining = total_id3 - static_cast<uint32_t>(*first_read);
                                    offset = *first_read;
                                    while (remaining > 0 && !IsCancelled(playback_id)) {
                                        size_t to_read = std::min(remaining, static_cast<uint32_t>(buffer.size()));
                                        auto skip_size = http->Read(buffer.data(), to_read);
                                        if (!skip_size || *skip_size == 0) break;
                                        remaining -= *skip_size;
                                    }
                                }
                            }

                            if (offset < static_cast<size_t>(*first_read)) {
                                in_ring.insert(in_ring.end(), buffer.begin() + offset,
                                               buffer.begin() + *first_read);
                            }

                            bool mp3_error = false;
                            bool eof_reached = false;

                            while (!mp3_error && !IsCancelled(playback_id)) {
                                while (in_ring.size() >= 4 && !IsCancelled(playback_id)) {
                                    esp_audio_dec_in_raw_t raw = {
                                        .buffer = in_ring.data(),
                                        .len = static_cast<uint32_t>(in_ring.size()),
                                        .consumed = 0,
                                        .frame_recover = ESP_AUDIO_DEC_RECOVERY_NONE,
                                    };
                                    esp_audio_dec_out_frame_t out_frame = {
                                        .buffer = reinterpret_cast<uint8_t*>(pcm_raw.data()),
                                        .len = static_cast<uint32_t>(pcm_raw.size() * sizeof(int16_t)),
                                        .decoded_size = 0,
                                    };
                                    esp_audio_dec_info_t dec_info = {};
                                    auto ret = esp_mp3_dec_decode(mp3_decoder, &raw, &out_frame, &dec_info);

                                    if (ret == ESP_AUDIO_ERR_OK && out_frame.decoded_size > 0) {
                                        if (raw.consumed > 0) {
                                            if (raw.consumed >= in_ring.size()) {
                                                in_ring.clear();
                                            } else {
                                                in_ring.erase(in_ring.begin(), in_ring.begin() + raw.consumed);
                                            }
                                        }

                                        uint32_t num_samples = out_frame.decoded_size / sizeof(int16_t);
                                        uint8_t channels = (dec_info.channel > 0) ? dec_info.channel : 2;
                                        uint32_t src_rate = (dec_info.sample_rate > 0) ? dec_info.sample_rate : 44100;

                                        std::vector<int16_t> mono_pcm;
                                        if (channels >= 2) {
                                            uint32_t mono_len = num_samples / channels;
                                            mono_pcm.resize(mono_len);
                                            for (size_t i = 0; i < mono_len; ++i) {
                                                int32_t sum = static_cast<int32_t>(pcm_raw[i * channels]) +
                                                              static_cast<int32_t>(pcm_raw[i * channels + 1]);
                                                mono_pcm[i] = static_cast<int16_t>(sum / 2);
                                            }
                                        } else {
                                            mono_pcm.assign(pcm_raw.begin(), pcm_raw.begin() + num_samples);
                                        }

                                        if (src_rate != static_cast<uint32_t>(target_rate)) {
                                            if (resampler == nullptr || resampler_src_rate != src_rate) {
                                                if (resampler != nullptr) {
                                                    esp_ae_rate_cvt_close(resampler);
                                                    resampler = nullptr;
                                                }
                                                esp_ae_rate_cvt_cfg_t cvt_cfg = {
                                                    .src_rate = src_rate,
                                                    .dest_rate = static_cast<uint32_t>(target_rate),
                                                    .channel = ESP_AUDIO_MONO,
                                                    .bits_per_sample = ESP_AUDIO_BIT16,
                                                    .complexity = 2,
                                                    .perf_type = ESP_AE_RATE_CVT_PERF_TYPE_SPEED,
                                                };
                                                esp_ae_rate_cvt_open(&cvt_cfg, &resampler);
                                                resampler_src_rate = src_rate;
                                            }
                                            if (resampler != nullptr) {
                                                uint32_t max_out = 0;
                                                esp_ae_rate_cvt_get_max_out_sample_num(resampler, mono_pcm.size(),
                                                                                       &max_out);
                                                std::vector<int16_t> resampled(max_out);
                                                uint32_t actual_out = max_out;
                                                esp_ae_rate_cvt_process(resampler,
                                                                        (esp_ae_sample_t)mono_pcm.data(),
                                                                        mono_pcm.size(),
                                                                        (esp_ae_sample_t)resampled.data(),
                                                                        &actual_out);
                                                resampled.resize(actual_out);
                                                mono_pcm = std::move(resampled);
                                            }
                                        }

                                        uint32_t frame_dur = (src_rate > 0)
                                                                 ? ((num_samples / channels) * 1000 / src_rate)
                                                                 : 26;
                                        {
                                            std::lock_guard<std::mutex> lock(mutex_);
                                            if (cancelled_ || !active_ || playback_id != playback_id_) {
                                                mp3_error = true;
                                                break;
                                            }
                                            playback_drained_ = false;
                                        }

                                        if (!audio_service_.PushPcmToPlaybackQueue(std::move(mono_pcm),
                                                                                   playback_id,
                                                                                   media_position_ms, true)) {
                                            mp3_error = true;
                                            break;
                                        }

                                        {
                                            std::lock_guard<std::mutex> lock(mutex_);
                                            stream_started_ = true;
                                        }
                                        media_position_ms += frame_dur;
                                    } else {
                                        if (raw.consumed > 0) {
                                            if (raw.consumed >= in_ring.size()) {
                                                in_ring.clear();
                                            } else {
                                                in_ring.erase(in_ring.begin(), in_ring.begin() + raw.consumed);
                                            }
                                        } else {
                                            if (in_ring.size() > 4096) {
                                                auto sync_it = std::find_if(in_ring.begin() + 1, in_ring.end(),
                                                                            [](uint8_t b) { return b == 0xFF; });
                                                if (sync_it != in_ring.end()) {
                                                    in_ring.erase(in_ring.begin(), sync_it);
                                                } else {
                                                    in_ring.clear();
                                                }
                                            }
                                            break;
                                        }
                                    }
                                }

                                if (eof_reached || mp3_error || IsCancelled(playback_id)) {
                                    break;
                                }

                                auto size = http->Read(buffer.data(), buffer.size());
                                if (!size) {
                                    ESP_LOGE(TAG, "Notification HTTP read failed: %s",
                                             size.error().ToString().c_str());
                                    break;
                                }
                                if (*size == 0) {
                                    eof_reached = true;
                                    success = stream_started_;
                                    continue;
                                }
                                in_ring.insert(in_ring.end(), buffer.begin(), buffer.begin() + *size);
                            }

                            if (mp3_decoder != nullptr) {
                                esp_mp3_dec_close(mp3_decoder);
                                mp3_decoder = nullptr;
                            }
                            if (resampler != nullptr) {
                                esp_ae_rate_cvt_close(resampler);
                                resampler = nullptr;
                            }
                        }
                    }
                }
            } else if (!status) {
                ESP_LOGE(TAG, "Notification HTTP status failed: %s",
                         status.error().ToString().c_str());
            } else {
                ESP_LOGE(TAG, "Notification HTTP request returned status %d", *status);
            }
        } else {
            ESP_LOGE(TAG, "Failed to open notification HTTP request: %s",
                     opened.error().ToString().c_str());
        }
        http->Close();
        http.reset();
    }

    FinishedCallback callback;
    uint32_t completed_playback_id = 0;
    bool report_success = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        worker_running_ = false;
        task_handle_ = nullptr;
        if (!cancelled_ && active_ && playback_id == playback_id_) {
            if (!success) {
                callback = CompleteLocked(completed_playback_id);
            } else {
                http_finished_ = true;
                if (playback_drained_) {
                    callback = CompleteLocked(completed_playback_id);
                    report_success = true;
                }
            }
        }
    }
    if (callback) {
        callback(completed_playback_id, report_success);
    }
}
