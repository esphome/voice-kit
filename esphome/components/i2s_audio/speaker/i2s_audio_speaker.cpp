#include "i2s_audio_speaker.h"

#ifdef USE_ESP32

#include <driver/i2s.h>

#include "esphome/core/audio.h"
#include "esphome/core/application.h"
#include "esphome/core/hal.h"
#include "esphome/core/log.h"

namespace esphome {
namespace i2s_audio {

static const uint8_t NUMBER_OF_CHANNELS = 2;  // Hard-coded expectation of stereo (2 channel) audio
static const size_t DMA_BUFFER_SIZE = 512;
static const size_t SAMPLES_IN_ONE_DMA_BUFFER = DMA_BUFFER_SIZE * NUMBER_OF_CHANNELS;
static const size_t DMA_BUFFERS_COUNT = 4;
static const size_t SAMPLES_IN_ALL_DMA_BUFFERS = SAMPLES_IN_ONE_DMA_BUFFER * DMA_BUFFERS_COUNT;
static const size_t OUTPUT_BUFFER_SAMPLES = 8192;  // Audio samples - keep small for fast pausing
static const size_t TASK_DELAY_MS = 10;

static const char *const TAG = "i2s_audio.speaker";

// Lists the Q15 fixed point scaling factor for volume reduction.
// Has 100 values representing silence and a reduction [49, 48.5, ... 0.5, 0] dB.
// dB to PCM scaling factor formula: floating_point_scale_factor = 2^(-db/6.014)
// float to Q15 fixed point formula: q15_scale_factor = floating_point_scale_factor * 2^(15)
static const std::vector<int16_t> q15_volume_scaling_factors = {
    0,     116,   122,   130,   137,   146,   154,   163,   173,   183,   194,   206,   218,   231,   244,
    259,   274,   291,   308,   326,   345,   366,   388,   411,   435,   461,   488,   517,   548,   580,
    615,   651,   690,   731,   774,   820,   868,   920,   974,   1032,  1094,  1158,  1227,  1300,  1377,
    1459,  1545,  1637,  1734,  1837,  1946,  2061,  2184,  2313,  2450,  2596,  2750,  2913,  3085,  3269,
    3462,  3668,  3885,  4116,  4360,  4619,  4893,  5183,  5490,  5816,  6161,  6527,  6914,  7324,  7758,
    8218,  8706,  9222,  9770,  10349, 10963, 11613, 12302, 13032, 13805, 14624, 15491, 16410, 17384, 18415,
    19508, 20665, 21891, 23189, 24565, 26022, 27566, 29201, 30933, 32767};

enum SpeakerTaskNotificationBits : uint32_t {
  COMMAND_START = (1 << 0),            // Starts the main task purpose
  COMMAND_STOP = (1 << 1),             // stops the main task
  COMMAND_STOP_GRACEFULLY = (1 << 2),  // Stops the task once all data has been written
  MESSAGE_RING_BUFFER_AVAILABLE_TO_WRITE = (1 << 5),
  STATE_STARTING = (1 << 10),
  STATE_RUNNING = (1 << 11),
  STATE_STOPPING = (1 << 12),
  STATE_STOPPED = (1 << 13),
  ERR_INVALID_STATE = (1 << 16),
  ERR_INVALID_ARG = (1 << 17),
  ERR_INVALID_SIZE = (1 << 18),
  ERR_NO_MEM = (1 << 19),
  ERR_FAIL = (1 << 20),
  ERR_TASK_FAILED_TO_START = (1 << 21),
  ERROR_BITS = ERR_INVALID_STATE | ERR_INVALID_ARG | ERR_INVALID_SIZE | ERR_NO_MEM | ERR_FAIL,
  ALL_BITS = 0x00FFFFFF,  // All valid event group bits
};

// Possible errors are: ESP_ERR_INVALID_STATE, ESP_ERR_INVALID_ARG, ESP_ERR_INVALID_SIZE, ESP_ERR_NO_MEM, ESP_FAIL (IO
// error)

bool I2SAudioSpeaker::set_event_group_error_(esp_err_t err) {
  switch (err) {
    case ESP_OK:
      return true;
    case ESP_ERR_INVALID_STATE:
      xEventGroupSetBits(this->event_group_, SpeakerTaskNotificationBits::ERR_INVALID_STATE);
      return false;
    case ESP_ERR_INVALID_ARG:
      xEventGroupSetBits(this->event_group_, SpeakerTaskNotificationBits::ERR_INVALID_ARG);
      return false;
    case ESP_ERR_INVALID_SIZE:
      xEventGroupSetBits(this->event_group_, SpeakerTaskNotificationBits::ERR_INVALID_SIZE);
      return false;
    case ESP_ERR_NO_MEM:
      xEventGroupSetBits(this->event_group_, SpeakerTaskNotificationBits::ERR_NO_MEM);
      return false;
    default:
      xEventGroupSetBits(this->event_group_, SpeakerTaskNotificationBits::ERR_FAIL);
      return false;
  }
}

esp_err_t err_bit_to_esp_err(uint32_t bit) {
  switch (bit) {
    case SpeakerTaskNotificationBits::ERR_INVALID_STATE:
      return ESP_ERR_INVALID_STATE;
    case SpeakerTaskNotificationBits::ERR_INVALID_ARG:
      return ESP_ERR_INVALID_ARG;
    case SpeakerTaskNotificationBits::ERR_INVALID_SIZE:
      return ESP_ERR_INVALID_SIZE;
    case SpeakerTaskNotificationBits::ERR_NO_MEM:
      return ESP_ERR_NO_MEM;
    default:
      return ESP_FAIL;
  }
}

void I2SAudioSpeaker::q15_multiplication(const int16_t *input, int16_t *output, size_t len, int16_t c) {
  for (int i = 0; i < len; i++) {
    int32_t acc = (int32_t) input[i] * (int32_t) c;
    output[i] = (int16_t) (acc >> 15);
  }
}

void I2SAudioSpeaker::setup() {
  ESP_LOGCONFIG(TAG, "Setting up I2S Audio Speaker...");

  if (this->event_group_ == nullptr) {
    this->event_group_ = xEventGroupCreate();
  }

  if (this->event_group_ == nullptr) {
    ESP_LOGE(TAG, "Failed to create event group");
    this->mark_failed();
    return;
  }

  AudioStreamInfo audio_stream_info;
  audio_stream_info.channels = 1;
  audio_stream_info.bits_per_sample = (uint8_t) this->bits_per_sample_;
  audio_stream_info.sample_rate = 16000;
  this->set_audio_stream_info(audio_stream_info);
}

void I2SAudioSpeaker::start() {
  if (this->is_failed())
    return;
  if ((this->state_ == speaker::STATE_STARTING) || (this->state_ == speaker::STATE_RUNNING))
    return;

  if (this->speaker_task_handle_ == nullptr) {
    xTaskCreate(I2SAudioSpeaker::speaker_task, "speaker_task", 8192, (void *) this, 23, &this->speaker_task_handle_);
  }

  if (this->speaker_task_handle_ != nullptr) {
    xEventGroupSetBits(this->event_group_, SpeakerTaskNotificationBits::COMMAND_START);
    this->task_created_ = true;
  } else {
    xEventGroupSetBits(this->event_group_, SpeakerTaskNotificationBits::ERR_TASK_FAILED_TO_START);
  }
}

void I2SAudioSpeaker::set_volume(float volume) {
  this->volume_ = volume;
  ssize_t decibel_index = remap<ssize_t, float>(volume, 0.0f, 1.0f, 0, q15_volume_scaling_factors.size() - 1);
  this->q15_volume_factor_ = q15_volume_scaling_factors[decibel_index];
}

esp_err_t I2SAudioSpeaker::start_i2s_driver_() {
  if (!this->parent_->try_lock()) {
    return ESP_ERR_INVALID_STATE;
  }

  i2s_driver_config_t config = {
      .mode = (i2s_mode_t) (this->i2s_mode_ | I2S_MODE_TX),
      .sample_rate = this->sample_rate_,
      .bits_per_sample = this->bits_per_sample_,
      .channel_format = this->channel_,
      .communication_format = this->i2s_comm_fmt_,
      .intr_alloc_flags = ESP_INTR_FLAG_LEVEL1,
      .dma_buf_count = DMA_BUFFERS_COUNT,
      .dma_buf_len = DMA_BUFFER_SIZE,
      .use_apll = this->use_apll_,
      .tx_desc_auto_clear = true,
      .fixed_mclk = I2S_PIN_NO_CHANGE,
      .mclk_multiple = I2S_MCLK_MULTIPLE_256,
      .bits_per_chan = this->bits_per_channel_,
#if SOC_I2S_SUPPORTS_TDM
      .chan_mask = (i2s_channel_t) (I2S_TDM_ACTIVE_CH0 | I2S_TDM_ACTIVE_CH1),
      .total_chan = 2,
      .left_align = false,
      .big_edin = false,
      .bit_order_msb = false,
      .skip_msk = false,
#endif
  };

  esp_err_t err = i2s_driver_install(this->parent_->get_port(), &config, 0, nullptr);
  if (err != ESP_OK) {
    // Failed to install the driver, so unlock the I2S port
    this->parent_->unlock();
    return err;
  }

  i2s_pin_config_t pin_config = this->parent_->get_pin_config();
  pin_config.data_out_num = this->dout_pin_;

  err = i2s_set_pin(this->parent_->get_port(), &pin_config);
  if (err != ESP_OK) {
    // Failed to set the data out pin, so uninstall the driver and unlock the I2S port
    i2s_driver_uninstall(this->parent_->get_port());
    this->parent_->unlock();
  }

  return err;
}

esp_err_t I2SAudioSpeaker::set_i2s_stream_info_(AudioStreamInfo &audio_stream_info) {
  if (this->i2s_mode_ & I2S_MODE_MASTER) {
    // We control the I2S bus, so we modify the sample rate and bits per sample to match the incoming audio
    this->sample_rate_ = audio_stream_info.sample_rate;
    this->bits_per_sample_ = (i2s_bits_per_sample_t) audio_stream_info.bits_per_sample;
  }

  if (audio_stream_info.channels == 1) {
    return i2s_set_clk(this->parent_->get_port(), this->sample_rate_, this->bits_per_sample_, I2S_CHANNEL_MONO);
  } else if (audio_stream_info.channels == 2) {
    return i2s_set_clk(this->parent_->get_port(), this->sample_rate_, this->bits_per_sample_, I2S_CHANNEL_STEREO);
  }

  return ESP_ERR_INVALID_ARG;
}

void I2SAudioSpeaker::delete_task_() {
  if (this->audio_ring_buffer_ != nullptr) {
    xEventGroupWaitBits(this->event_group_,
                        MESSAGE_RING_BUFFER_AVAILABLE_TO_WRITE,  // Bit message to read
                        pdFALSE,                                 // Don't clear the bits on exit
                        pdTRUE,                                  // Don't wait for all the bits,
                        portMAX_DELAY);                          // Block indefinitely until a command bit is set

    this->audio_ring_buffer_.reset();  // Deallocates the ring buffer stored in the unique_ptr
    this->audio_ring_buffer_ = nullptr;
  }

  if (this->data_buffer_ != nullptr) {
    ExternalRAMAllocator<uint8_t> allocator(ExternalRAMAllocator<uint8_t>::ALLOW_FAILURE);
    allocator.deallocate(this->data_buffer_, 0);  // SAMPLES_IN_ALL_DMA_BUFFERS * bytes_per_sample);
  }

  xEventGroupSetBits(this->event_group_, SpeakerTaskNotificationBits::STATE_STOPPED);

  this->task_created_ = false;
  vTaskDelete(NULL);
}

void I2SAudioSpeaker::speaker_task(void *params) {
  I2SAudioSpeaker *this_speaker = (I2SAudioSpeaker *) params;
  uint32_t event_group_bits =
      xEventGroupWaitBits(this_speaker->event_group_,
                          SpeakerTaskNotificationBits::COMMAND_START | SpeakerTaskNotificationBits::COMMAND_STOP |
                              SpeakerTaskNotificationBits::COMMAND_STOP_GRACEFULLY,  // Bit message to read
                          pdTRUE,                                                    // Clear the bits on exit
                          pdFALSE,                                                   // Don't wait for all the bits,
                          portMAX_DELAY);  // Block indefinitely until a bit is set

  if (event_group_bits &
      (SpeakerTaskNotificationBits::COMMAND_STOP | SpeakerTaskNotificationBits::COMMAND_STOP_GRACEFULLY)) {
    // Received a stop signal before the task was requested to start
    this_speaker->delete_task_();
  }

  xEventGroupSetBits(this_speaker->event_group_, SpeakerTaskNotificationBits::STATE_STARTING);

  AudioStreamInfo audio_stream_info = this_speaker->audio_stream_info_;
  ssize_t bytes_per_sample = audio_stream_info.get_bytes_per_sample();

  ExternalRAMAllocator<uint8_t> allocator(ExternalRAMAllocator<uint8_t>::ALLOW_FAILURE);
  this_speaker->data_buffer_ = allocator.allocate(SAMPLES_IN_ALL_DMA_BUFFERS * bytes_per_sample);

  if (this_speaker->audio_ring_buffer_ == nullptr) {
    // Allocate ring buffer
    this_speaker->audio_ring_buffer_ = RingBuffer::create(OUTPUT_BUFFER_SAMPLES * bytes_per_sample);
  }

  if ((this_speaker->data_buffer_ == nullptr) || (this_speaker->audio_ring_buffer_ == nullptr)) {
    // Failed to allocate a buffer
    xEventGroupSetBits(this_speaker->event_group_, SpeakerTaskNotificationBits::ERR_NO_MEM);
    this_speaker->delete_task_();
  }

  if (!this_speaker->set_event_group_error_(this_speaker->start_i2s_driver_())) {
    // Failed to start I2S driver
    this_speaker->delete_task_();
  }

  if (this_speaker->set_event_group_error_(this_speaker->set_i2s_stream_info_(audio_stream_info))) {
    // Successfully set the I2S stream info, ready to write audio data to the I2S port

    xEventGroupSetBits(this_speaker->event_group_,
                       SpeakerTaskNotificationBits::STATE_RUNNING |
                           SpeakerTaskNotificationBits::MESSAGE_RING_BUFFER_AVAILABLE_TO_WRITE);

    bool stop_gracefully = false;
    uint32_t last_data_received_time = millis();

    while ((millis() - last_data_received_time) <= this_speaker->timeout_) {
      event_group_bits = xEventGroupGetBits(this_speaker->event_group_);

      if (event_group_bits & SpeakerTaskNotificationBits::COMMAND_STOP) {
        i2s_zero_dma_buffer(this_speaker->parent_->get_port());
        break;
      }
      if (event_group_bits & SpeakerTaskNotificationBits::COMMAND_STOP_GRACEFULLY) {
        stop_gracefully = true;
      }

      size_t bytes_to_read = SAMPLES_IN_ALL_DMA_BUFFERS * bytes_per_sample;
      size_t bytes_read = this_speaker->audio_ring_buffer_->read((void *) this_speaker->data_buffer_, bytes_to_read,
                                                                 pdMS_TO_TICKS(TASK_DELAY_MS));

      if (bytes_read > 0) {
        last_data_received_time = millis();
        size_t bytes_written = 0;

        if ((audio_stream_info.bits_per_sample <= 16) && (this_speaker->q15_volume_factor_ < INT16_MAX)) {
          // Scale samples by the volume factor in place
          q15_multiplication((int16_t *) this_speaker->data_buffer_, (int16_t *) this_speaker->data_buffer_,
                             bytes_read / sizeof(int16_t), this_speaker->q15_volume_factor_);
        }

        if (audio_stream_info.bits_per_sample == (uint8_t) this_speaker->bits_per_sample_) {
          i2s_write(this_speaker->parent_->get_port(), this_speaker->data_buffer_, bytes_read, &bytes_written,
                    portMAX_DELAY);
        } else if (audio_stream_info.bits_per_sample < (uint8_t) this_speaker->bits_per_sample_) {
          i2s_write_expand(this_speaker->parent_->get_port(), this_speaker->data_buffer_, bytes_read,
                           audio_stream_info.bits_per_sample, this_speaker->bits_per_sample_, &bytes_written,
                           portMAX_DELAY);
        }  // TODO: Unhandled case where the incoming stream has more bits per sample than the outgoing stream

        if (bytes_written != bytes_read) {
          xEventGroupSetBits(this_speaker->event_group_, SpeakerTaskNotificationBits::ERR_INVALID_SIZE);
        }

      } else {
        // No data received
        i2s_zero_dma_buffer(this_speaker->parent_->get_port());

        if (stop_gracefully) {
          break;
        }
      }
    }
  }
  xEventGroupSetBits(this_speaker->event_group_, SpeakerTaskNotificationBits::STATE_STOPPING);

  i2s_stop(this_speaker->parent_->get_port());
  i2s_driver_uninstall(this_speaker->parent_->get_port());

  this_speaker->parent_->unlock();
  this_speaker->delete_task_();
}

void I2SAudioSpeaker::stop() { this->stop_(false); }

void I2SAudioSpeaker::finish() { this->stop_(true); }

void I2SAudioSpeaker::stop_(bool wait_on_empty) {
  if (this->is_failed())
    return;
  if (this->state_ == speaker::STATE_STOPPED)
    return;

  if (wait_on_empty) {
    xEventGroupSetBits(this->event_group_, SpeakerTaskNotificationBits::COMMAND_STOP_GRACEFULLY);
  } else {
    xEventGroupSetBits(this->event_group_, SpeakerTaskNotificationBits::COMMAND_STOP);
  }
}

void I2SAudioSpeaker::loop() {
  uint32_t event_group_bits = xEventGroupGetBits(this->event_group_);

  if (event_group_bits & SpeakerTaskNotificationBits::ERROR_BITS) {
    uint32_t error_bits = event_group_bits & SpeakerTaskNotificationBits::ERROR_BITS;
    ESP_LOGW(TAG, "Error writing to I2S: %s", esp_err_to_name(err_bit_to_esp_err(error_bits)));
    this->status_set_warning();
  }

  if (event_group_bits & SpeakerTaskNotificationBits::STATE_STARTING) {
    ESP_LOGD(TAG, "Starting Speaker");
    this->state_ = speaker::STATE_STARTING;
    xEventGroupClearBits(this->event_group_, SpeakerTaskNotificationBits::STATE_STARTING);
  }
  if (event_group_bits & SpeakerTaskNotificationBits::STATE_RUNNING) {
    ESP_LOGD(TAG, "Started Speaker");
    this->state_ = speaker::STATE_RUNNING;
    xEventGroupClearBits(this->event_group_, SpeakerTaskNotificationBits::STATE_RUNNING);
  }
  if (event_group_bits & SpeakerTaskNotificationBits::STATE_STOPPING) {
    ESP_LOGD(TAG, "Stopping Speaker");
    this->state_ = speaker::STATE_STOPPING;
    xEventGroupClearBits(this->event_group_, SpeakerTaskNotificationBits::STATE_STOPPING);
  }
  if (event_group_bits & SpeakerTaskNotificationBits::STATE_STOPPED) {
    if (!this->task_created_) {
      ESP_LOGD(TAG, "Stopped Speaker");
      this->state_ = speaker::STATE_STOPPED;
      xEventGroupClearBits(this->event_group_, SpeakerTaskNotificationBits::ALL_BITS);
      this->speaker_task_handle_ = nullptr;
    }
  }
}

size_t I2SAudioSpeaker::play(const uint8_t *data, size_t length, TickType_t ticks_to_wait) {
  if (this->is_failed()) {
    ESP_LOGE(TAG, "Cannot play audio, speaker failed to setup");
    return 0;
  }
  if (this->state_ != speaker::STATE_RUNNING && this->state_ != speaker::STATE_STARTING) {
    this->start();
  }

  // Wait for the ring buffer to be available
  uint32_t event_bits =
      xEventGroupWaitBits(this->event_group_, SpeakerTaskNotificationBits::MESSAGE_RING_BUFFER_AVAILABLE_TO_WRITE,
                          pdFALSE, pdFALSE, pdMS_TO_TICKS(10));

  if (event_bits & SpeakerTaskNotificationBits::MESSAGE_RING_BUFFER_AVAILABLE_TO_WRITE) {
    // Ring buffer is available to write
    xEventGroupClearBits(this->event_group_, SpeakerTaskNotificationBits::MESSAGE_RING_BUFFER_AVAILABLE_TO_WRITE);

    size_t bytes_written = this->audio_ring_buffer_->write_without_replacement((void *) data, length, ticks_to_wait);

    xEventGroupSetBits(this->event_group_, SpeakerTaskNotificationBits::MESSAGE_RING_BUFFER_AVAILABLE_TO_WRITE);

    return bytes_written;
  }

  return 0;
}

bool I2SAudioSpeaker::has_buffered_data() const {
  if (this->audio_ring_buffer_.get() != nullptr) {
    return this->audio_ring_buffer_->available() > 0;
  }
  return false;
}

}  // namespace i2s_audio
}  // namespace esphome

#endif  // USE_ESP32