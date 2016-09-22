/*
 *  Copyright (c) 2012 The WebRTC project authors. All Rights Reserved.
 *
 *  Use of this source code is governed by a BSD-style license
 *  that can be found in the LICENSE file in the root of the source
 *  tree. An additional intellectual property rights grant can be found
 *  in the file PATENTS.  All contributing project authors may
 *  be found in the AUTHORS file in the root of the source tree.
 */

#include <math.h>
#include <stdio.h>
#include <algorithm>
#include <limits>
#include <queue>

#include "webrtc/common_audio/include/audio_util.h"
#include "webrtc/common_audio/resampler/include/push_resampler.h"
#include "webrtc/common_audio/resampler/push_sinc_resampler.h"
#include "webrtc/common_audio/signal_processing/include/signal_processing_library.h"
#include "webrtc/modules/audio_processing/beamformer/mock_beamformer.h"
#include "webrtc/modules/audio_processing/common.h"
#include "webrtc/modules/audio_processing/include/audio_processing.h"
#include "webrtc/modules/audio_processing/test/test_utils.h"
#include "webrtc/modules/interface/module_common_types.h"
#include "webrtc/system_wrappers/interface/event_wrapper.h"
#include "webrtc/system_wrappers/interface/scoped_ptr.h"
#include "webrtc/system_wrappers/interface/trace.h"
#include "webrtc/test/testsupport/fileutils.h"
#include "webrtc/test/testsupport/gtest_disable.h"
#ifdef WEBRTC_ANDROID_PLATFORM_BUILD
#include "gtest/gtest.h"
#include "external/webrtc/webrtc/modules/audio_processing/test/unittest.pb.h"
#else
#include "testing/gtest/include/gtest/gtest.h"
#include "webrtc/audio_processing/unittest.pb.h"
#endif

namespace webrtc {
namespace {

// TODO(bjornv): This is not feasible until the functionality has been
// re-implemented; see comment at the bottom of this file. For now, the user has
// to hard code the |write_ref_data| value.
// When false, this will compare the output data with the results stored to
// file. This is the typical case. When the file should be updated, it can
// be set to true with the command-line switch --write_ref_data.
bool write_ref_data = false;
const int kChannels[] = {1, 2};
const size_t kChannelsSize = sizeof(kChannels) / sizeof(*kChannels);

const int kSampleRates[] = {8000, 16000, 32000};
const size_t kSampleRatesSize = sizeof(kSampleRates) / sizeof(*kSampleRates);

#if defined(WEBRTC_AUDIOPROC_FIXED_PROFILE)
// AECM doesn't support super-wb.
const int kProcessSampleRates[] = {8000, 16000};
#elif defined(WEBRTC_AUDIOPROC_FLOAT_PROFILE)
const int kProcessSampleRates[] = {8000, 16000, 32000};
#endif
const size_t kProcessSampleRatesSize = sizeof(kProcessSampleRates) /
    sizeof(*kProcessSampleRates);

void ConvertToFloat(const int16_t* int_data, ChannelBuffer<float>* cb) {
  ChannelBuffer<int16_t> cb_int(cb->samples_per_channel(),
                                cb->num_channels());
  Deinterleave(int_data,
               cb->samples_per_channel(),
               cb->num_channels(),
               cb_int.channels());
  S16ToFloat(cb_int.data(),
           cb->samples_per_channel() * cb->num_channels(),
           cb->data());
}

void ConvertToFloat(const AudioFrame& frame, ChannelBuffer<float>* cb) {
  ConvertToFloat(frame.data_, cb);
}

// Number of channels including the keyboard channel.
int TotalChannelsFromLayout(AudioProcessing::ChannelLayout layout) {
  switch (layout) {
    case AudioProcessing::kMono:
      return 1;
    case AudioProcessing::kMonoAndKeyboard:
    case AudioProcessing::kStereo:
      return 2;
    case AudioProcessing::kStereoAndKeyboard:
      return 3;
  }
  assert(false);
  return -1;
}

int TruncateToMultipleOf10(int value) {
  return (value / 10) * 10;
}

void MixStereoToMono(const float* stereo, float* mono,
                     int samples_per_channel) {
  for (int i = 0; i < samples_per_channel; ++i)
    mono[i] = (stereo[i * 2] + stereo[i * 2 + 1]) / 2;
}

void MixStereoToMono(const int16_t* stereo, int16_t* mono,
                     int samples_per_channel) {
  for (int i = 0; i < samples_per_channel; ++i)
    mono[i] = (stereo[i * 2] + stereo[i * 2 + 1]) >> 1;
}

void CopyLeftToRightChannel(int16_t* stereo, int samples_per_channel) {
  for (int i = 0; i < samples_per_channel; i++) {
    stereo[i * 2 + 1] = stereo[i * 2];
  }
}

void VerifyChannelsAreEqual(int16_t* stereo, int samples_per_channel) {
  for (int i = 0; i < samples_per_channel; i++) {
    EXPECT_EQ(stereo[i * 2 + 1], stereo[i * 2]);
  }
}

void SetFrameTo(AudioFrame* frame, int16_t value) {
  for (int i = 0; i < frame->samples_per_channel_ * frame->num_channels_; ++i) {
    frame->data_[i] = value;
  }
}

void SetFrameTo(AudioFrame* frame, int16_t left, int16_t right) {
  ASSERT_EQ(2, frame->num_channels_);
  for (int i = 0; i < frame->samples_per_channel_ * 2; i += 2) {
    frame->data_[i] = left;
    frame->data_[i + 1] = right;
  }
}

void ScaleFrame(AudioFrame* frame, float scale) {
  for (int i = 0; i < frame->samples_per_channel_ * frame->num_channels_; ++i) {
    frame->data_[i] = FloatS16ToS16(frame->data_[i] * scale);
  }
}

bool FrameDataAreEqual(const AudioFrame& frame1, const AudioFrame& frame2) {
  if (frame1.samples_per_channel_ != frame2.samples_per_channel_) {
    return false;
  }
  if (frame1.num_channels_ != frame2.num_channels_) {
    return false;
  }
  if (memcmp(frame1.data_, frame2.data_,
             frame1.samples_per_channel_ * frame1.num_channels_ *
                 sizeof(int16_t))) {
    return false;
  }
  return true;
}

void EnableAllAPComponents(AudioProcessing* ap) {
#if defined(WEBRTC_AUDIOPROC_FIXED_PROFILE)
  EXPECT_NOERR(ap->echo_control_mobile()->Enable(true));

  EXPECT_NOERR(ap->gain_control()->set_mode(GainControl::kAdaptiveDigital));
  EXPECT_NOERR(ap->gain_control()->Enable(true));
#elif defined(WEBRTC_AUDIOPROC_FLOAT_PROFILE)
  EXPECT_NOERR(ap->echo_cancellation()->enable_drift_compensation(true));
  EXPECT_NOERR(ap->echo_cancellation()->enable_metrics(true));
  EXPECT_NOERR(ap->echo_cancellation()->enable_delay_logging(true));
  EXPECT_NOERR(ap->echo_cancellation()->Enable(true));

  EXPECT_NOERR(ap->gain_control()->set_mode(GainControl::kAdaptiveAnalog));
  EXPECT_NOERR(ap->gain_control()->set_analog_level_limits(0, 255));
  EXPECT_NOERR(ap->gain_control()->Enable(true));
#endif

  EXPECT_NOERR(ap->high_pass_filter()->Enable(true));
  EXPECT_NOERR(ap->level_estimator()->Enable(true));
  EXPECT_NOERR(ap->noise_suppression()->Enable(true));

  EXPECT_NOERR(ap->voice_detection()->Enable(true));
}

// These functions are only used by ApmTest.Process.
template <class T>
T AbsValue(T a) {
  return a > 0 ? a: -a;
}

int16_t MaxAudioFrame(const AudioFrame& frame) {
  const int length = frame.samples_per_channel_ * frame.num_channels_;
  int16_t max_data = AbsValue(frame.data_[0]);
  for (int i = 1; i < length; i++) {
    max_data = std::max(max_data, AbsValue(frame.data_[i]));
  }

  return max_data;
}

#if defined(WEBRTC_AUDIOPROC_FLOAT_PROFILE)
void TestStats(const AudioProcessing::Statistic& test,
               const audioproc::Test::Statistic& reference) {
  EXPECT_EQ(reference.instant(), test.instant);
  EXPECT_EQ(reference.average(), test.average);
  EXPECT_EQ(reference.maximum(), test.maximum);
  EXPECT_EQ(reference.minimum(), test.minimum);
}

void WriteStatsMessage(const AudioProcessing::Statistic& output,
                       audioproc::Test::Statistic* msg) {
  msg->set_instant(output.instant);
  msg->set_average(output.average);
  msg->set_maximum(output.maximum);
  msg->set_minimum(output.minimum);
}
#endif

void OpenFileAndWriteMessage(const std::string filename,
                             const ::google::protobuf::MessageLite& msg) {
#if defined(WEBRTC_LINUX) && !defined(WEBRTC_ANDROID)
  FILE* file = fopen(filename.c_str(), "wb");
  ASSERT_TRUE(file != NULL);

  int32_t size = msg.ByteSize();
  ASSERT_GT(size, 0);
  scoped_ptr<uint8_t[]> array(new uint8_t[size]);
  ASSERT_TRUE(msg.SerializeToArray(array.get(), size));

  ASSERT_EQ(1u, fwrite(&size, sizeof(size), 1, file));
  ASSERT_EQ(static_cast<size_t>(size),
      fwrite(array.get(), sizeof(array[0]), size, file));
  fclose(file);
#else
  std::cout << "Warning: Writing new reference is only allowed on Linux!"
      << std::endl;
#endif
}

std::string ResourceFilePath(std::string name, int sample_rate_hz) {
  std::ostringstream ss;
  // Resource files are all stereo.
  ss << name << sample_rate_hz / 1000 << "_stereo";
  return test::ResourcePath(ss.str(), "pcm");
}

// Temporary filenames unique to this process. Used to be able to run these
// tests in parallel as each process needs to be running in isolation they can't
// have competing filenames.
std::map<std::string, std::string> temp_filenames;

std::string OutputFilePath(std::string name,
                           int input_rate,
                           int output_rate,
                           int reverse_rate,
                           int num_input_channels,
                           int num_output_channels,
                           int num_reverse_channels) {
  std::ostringstream ss;
  ss << name << "_i" << num_input_channels << "_" << input_rate / 1000
     << "_r" << num_reverse_channels << "_" << reverse_rate  / 1000 << "_";
  if (num_output_channels == 1) {
    ss << "mono";
  } else if (num_output_channels == 2) {
    ss << "stereo";
  } else {
    assert(false);
  }
  ss << output_rate / 1000 << "_pcm";

  std::string filename = ss.str();
  if (temp_filenames[filename] == "")
    temp_filenames[filename] = test::TempFilename(test::OutputPath(), filename);
  return temp_filenames[filename];
}

void OpenFileAndReadMessage(const std::string filename,
                            ::google::protobuf::MessageLite* msg) {
  FILE* file = fopen(filename.c_str(), "rb");
  ASSERT_TRUE(file != NULL);
  ReadMessageFromFile(file, msg);
  fclose(file);
}

// Reads a 10 ms chunk of int16 interleaved audio from the given (assumed
// stereo) file, converts to deinterleaved float (optionally downmixing) and
// returns the result in |cb|. Returns false if the file ended (or on error) and
// true otherwise.
//
// |int_data| and |float_data| are just temporary space that must be
// sufficiently large to hold the 10 ms chunk.
bool ReadChunk(FILE* file, int16_t* int_data, float* float_data,
               ChannelBuffer<float>* cb) {
  // The files always contain stereo audio.
  size_t frame_size = cb->samples_per_channel() * 2;
  size_t read_count = fread(int_data, sizeof(int16_t), frame_size, file);
  if (read_count != frame_size) {
    // Check that the file really ended.
    assert(feof(file));
    return false;  // This is expected.
  }

  S16ToFloat(int_data, frame_size, float_data);
  if (cb->num_channels() == 1) {
    MixStereoToMono(float_data, cb->data(), cb->samples_per_channel());
  } else {
    Deinterleave(float_data, cb->samples_per_channel(), 2,
                 cb->channels());
  }

  return true;
}

class ApmTest : public ::testing::Test {
 protected:
  ApmTest();
  virtual void SetUp();
  virtual void TearDown();

  static void SetUpTestCase() {
    Trace::CreateTrace();
    std::string trace_filename =
        test::TempFilename(test::OutputPath(), "audioproc_trace");
    ASSERT_EQ(0, Trace::SetTraceFile(trace_filename.c_str()));
  }

  static void TearDownTestCase() {
    Trace::ReturnTrace();
  }

  // Used to select between int and float interface tests.
  enum Format {
    kIntFormat,
    kFloatFormat
  };

  void Init(int sample_rate_hz,
            int output_sample_rate_hz,
            int reverse_sample_rate_hz,
            int num_reverse_channels,
            int num_input_channels,
            int num_output_channels,
            bool open_output_file);
  void Init(AudioProcessing* ap);
  void EnableAllComponents();
  bool ReadFrame(FILE* file, AudioFrame* frame);
  bool ReadFrame(FILE* file, AudioFrame* frame, ChannelBuffer<float>* cb);
  void ReadFrameWithRewind(FILE* file, AudioFrame* frame);
  void ReadFrameWithRewind(FILE* file, AudioFrame* frame,
                           ChannelBuffer<float>* cb);
  void ProcessWithDefaultStreamParameters(AudioFrame* frame);
  void ProcessDelayVerificationTest(int delay_ms, int system_delay_ms,
                                    int delay_min, int delay_max);
  void TestChangingChannels(int num_channels,
                            AudioProcessing::Error expected_return);
  void RunQuantizedVolumeDoesNotGetStuckTest(int sample_rate);
  void RunManualVolumeChangeIsPossibleTest(int sample_rate);
  void StreamParametersTest(Format format);
  int ProcessStreamChooser(Format format);
  int AnalyzeReverseStreamChooser(Format format);
  void ProcessDebugDump(const std::string& in_filename,
                        const std::string& out_filename,
                        Format format);
  void VerifyDebugDumpTest(Format format);

  const std::string output_path_;
  const std::string ref_path_;
  const std::string ref_filename_;
  scoped_ptr<AudioProcessing> apm_;
  AudioFrame* frame_;
  AudioFrame* revframe_;
  scoped_ptr<ChannelBuffer<float> > float_cb_;
  scoped_ptr<ChannelBuffer<float> > revfloat_cb_;
  int output_sample_rate_hz_;
  int num_output_channels_;
  FILE* far_file_;
  FILE* near_file_;
  FILE* out_file_;
};

ApmTest::ApmTest()
    : output_path_(test::OutputPath()),
      ref_path_(test::ProjectRootPath() + "data/audio_processing/"),
#if defined(WEBRTC_AUDIOPROC_FIXED_PROFILE)
      ref_filename_(ref_path_ + "output_data_fixed.pb"),
#elif defined(WEBRTC_AUDIOPROC_FLOAT_PROFILE)
      ref_filename_(ref_path_ + "output_data_float.pb"),
#endif
      frame_(NULL),
      revframe_(NULL),
      output_sample_rate_hz_(0),
      num_output_channels_(0),
      far_file_(NULL),
      near_file_(NULL),
      out_file_(NULL) {
  Config config;
  config.Set<ExperimentalAgc>(new ExperimentalAgc(false));
  apm_.reset(AudioProcessing::Create(config));
}

void ApmTest::SetUp() {
  ASSERT_TRUE(apm_.get() != NULL);

  frame_ = new AudioFrame();
  revframe_ = new AudioFrame();

#if defined(WEBRTC_AUDIOPROC_FIXED_PROFILE)
  Init(16000, 16000, 16000, 2, 2, 2, false);
#else
  Init(32000, 32000, 32000, 2, 2, 2, false);
#endif
}

void ApmTest::TearDown() {
  if (frame_) {
    delete frame_;
  }
  frame_ = NULL;

  if (revframe_) {
    delete revframe_;
  }
  revframe_ = NULL;

  if (far_file_) {
    ASSERT_EQ(0, fclose(far_file_));
  }
  far_file_ = NULL;

  if (near_file_) {
    ASSERT_EQ(0, fclose(near_file_));
  }
  near_file_ = NULL;

  if (out_file_) {
    ASSERT_EQ(0, fclose(out_file_));
  }
  out_file_ = NULL;
}

void ApmTest::Init(AudioProcessing* ap) {
  ASSERT_EQ(kNoErr,
            ap->Initialize(frame_->sample_rate_hz_,
                           output_sample_rate_hz_,
                           revframe_->sample_rate_hz_,
                           LayoutFromChannels(frame_->num_channels_),
                           LayoutFromChannels(num_output_channels_),
                           LayoutFromChannels(revframe_->num_channels_)));
}

void ApmTest::Init(int sample_rate_hz,
                   int output_sample_rate_hz,
                   int reverse_sample_rate_hz,
                   int num_input_channels,
                   int num_output_channels,
                   int num_reverse_channels,
                   bool open_output_file) {
  SetContainerFormat(sample_rate_hz, num_input_channels, frame_, &float_cb_);
  output_sample_rate_hz_ = output_sample_rate_hz;
  num_output_channels_ = num_output_channels;

  SetContainerFormat(reverse_sample_rate_hz, num_reverse_channels, revframe_,
                     &revfloat_cb_);
  Init(apm_.get());

  if (far_file_) {
    ASSERT_EQ(0, fclose(far_file_));
  }
  std::string filename = ResourceFilePath("far", sample_rate_hz);
  far_file_ = fopen(filename.c_str(), "rb");
  ASSERT_TRUE(far_file_ != NULL) << "Could not open file " <<
      filename << "\n";

  if (near_file_) {
    ASSERT_EQ(0, fclose(near_file_));
  }
  filename = ResourceFilePath("near", sample_rate_hz);
  near_file_ = fopen(filename.c_str(), "rb");
  ASSERT_TRUE(near_file_ != NULL) << "Could not open file " <<
        filename << "\n";

  if (open_output_file) {
    if (out_file_) {
      ASSERT_EQ(0, fclose(out_file_));
    }
    filename = OutputFilePath("out",
                              sample_rate_hz,
                              output_sample_rate_hz,
                              reverse_sample_rate_hz,
                              num_input_channels,
                              num_output_channels,
                              num_reverse_channels);
    out_file_ = fopen(filename.c_str(), "wb");
    ASSERT_TRUE(out_file_ != NULL) << "Could not open file " <<
          filename << "\n";
  }
}

void ApmTest::EnableAllComponents() {
  EnableAllAPComponents(apm_.get());
}

bool ApmTest::ReadFrame(FILE* file, AudioFrame* frame,
                        ChannelBuffer<float>* cb) {
  // The files always contain stereo audio.
  size_t frame_size = frame->samples_per_channel_ * 2;
  size_t read_count = fread(frame->data_,
                            sizeof(int16_t),
                            frame_size,
                            file);
  if (read_count != frame_size) {
    // Check that the file really ended.
    EXPECT_NE(0, feof(file));
    return false;  // This is expected.
  }

  if (frame->num_channels_ == 1) {
    MixStereoToMono(frame->data_, frame->data_,
                    frame->samples_per_channel_);
  }

  if (cb) {
    ConvertToFloat(*frame, cb);
  }
  return true;
}

bool ApmTest::ReadFrame(FILE* file, AudioFrame* frame) {
  return ReadFrame(file, frame, NULL);
}

// If the end of the file has been reached, rewind it and attempt to read the
// frame again.
void ApmTest::ReadFrameWithRewind(FILE* file, AudioFrame* frame,
                                  ChannelBuffer<float>* cb) {
  if (!ReadFrame(near_file_, frame_, cb)) {
    rewind(near_file_);
    ASSERT_TRUE(ReadFrame(near_file_, frame_, cb));
  }
}

void ApmTest::ReadFrameWithRewind(FILE* file, AudioFrame* frame) {
  ReadFrameWithRewind(file, frame, NULL);
}

void ApmTest::ProcessWithDefaultStreamParameters(AudioFrame* frame) {
  EXPECT_EQ(apm_->kNoError, apm_->set_stream_delay_ms(0));
  apm_->echo_cancellation()->set_stream_drift_samples(0);
  EXPECT_EQ(apm_->kNoError,
      apm_->gain_control()->set_stream_analog_level(127));
  EXPECT_EQ(apm_->kNoError, apm_->ProcessStream(frame));
}

int ApmTest::ProcessStreamChooser(Format format) {
  if (format == kIntFormat) {
    return apm_->ProcessStream(frame_);
  }
  return apm_->ProcessStream(float_cb_->channels(),
                             frame_->samples_per_channel_,
                             frame_->sample_rate_hz_,
                             LayoutFromChannels(frame_->num_channels_),
                             output_sample_rate_hz_,
                             LayoutFromChannels(num_output_channels_),
                             float_cb_->channels());
}

int ApmTest::AnalyzeReverseStreamChooser(Format format) {
  if (format == kIntFormat) {
    return apm_->AnalyzeReverseStream(revframe_);
  }
  return apm_->AnalyzeReverseStream(
      revfloat_cb_->channels(),
      revframe_->samples_per_channel_,
      revframe_->sample_rate_hz_,
      LayoutFromChannels(revframe_->num_channels_));
}

void ApmTest::ProcessDelayVerificationTest(int delay_ms, int system_delay_ms,
                                           int delay_min, int delay_max) {
  // The |revframe_| and |frame_| should include the proper frame information,
  // hence can be used for extracting information.
  AudioFrame tmp_frame;
  std::queue<AudioFrame*> frame_queue;
  bool causal = true;

  tmp_frame.CopyFrom(*revframe_);
  SetFrameTo(&tmp_frame, 0);

  EXPECT_EQ(apm_->kNoError, apm_->Initialize());
  // Initialize the |frame_queue| with empty frames.
  int frame_delay = delay_ms / 10;
  while (frame_delay < 0) {
    AudioFrame* frame = new AudioFrame();
    frame->CopyFrom(tmp_frame);
    frame_queue.push(frame);
    frame_delay++;
    causal = false;
  }
  while (frame_delay > 0) {
    AudioFrame* frame = new AudioFrame();
    frame->CopyFrom(tmp_frame);
    frame_queue.push(frame);
    frame_delay--;
  }
  // Run for 4.5 seconds, skipping statistics from the first 2.5 seconds.  We
  // need enough frames with audio to have reliable estimates, but as few as
  // possible to keep processing time down.  4.5 seconds seemed to be a good
  // compromise for this recording.
  for (int frame_count = 0; frame_count < 450; ++frame_count) {
    AudioFrame* frame = new AudioFrame();
    frame->CopyFrom(tmp_frame);
    // Use the near end recording, since that has more speech in it.
    ASSERT_TRUE(ReadFrame(near_file_, frame));
    frame_queue.push(frame);
    AudioFrame* reverse_frame = frame;
    AudioFrame* process_frame = frame_queue.front();
    if (!causal) {
      reverse_frame = frame_queue.front();
      // When we call ProcessStream() the frame is modified, so we can't use the
      // pointer directly when things are non-causal. Use an intermediate frame
      // and copy the data.
      process_frame = &tmp_frame;
      process_frame->CopyFrom(*frame);
    }
    EXPECT_EQ(apm_->kNoError, apm_->AnalyzeReverseStream(reverse_frame));
    EXPECT_EQ(apm_->kNoError, apm_->set_stream_delay_ms(system_delay_ms));
    EXPECT_EQ(apm_->kNoError, apm_->ProcessStream(process_frame));
    frame = frame_queue.front();
    frame_queue.pop();
    delete frame;

    if (frame_count == 250) {
      int median;
      int std;
      // Discard the first delay metrics to avoid convergence effects.
      EXPECT_EQ(apm_->kNoError,
                apm_->echo_cancellation()->GetDelayMetrics(&median, &std));
    }
  }

  rewind(near_file_);
  while (!frame_queue.empty()) {
    AudioFrame* frame = frame_queue.front();
    frame_queue.pop();
    delete frame;
  }
  // Calculate expected delay estimate and acceptable regions. Further,
  // limit them w.r.t. AEC delay estimation support.
  const int samples_per_ms = std::min(16, frame_->samples_per_channel_ / 10);
  int expected_median = std::min(std::max(delay_ms - system_delay_ms,
                                          delay_min), delay_max);
  int expected_median_high = std::min(std::max(
      expected_median + 96 / samples_per_ms, delay_min), delay_max);
  int expected_median_low = std::min(std::max(
      expected_median - 96 / samples_per_ms, delay_min), delay_max);
  // Verify delay metrics.
  int median;
  int std;
  EXPECT_EQ(apm_->kNoError,
            apm_->echo_cancellation()->GetDelayMetrics(&median, &std));
  EXPECT_GE(expected_median_high, median);
  EXPECT_LE(expected_median_low, median);
}

void ApmTest::StreamParametersTest(Format format) {
  // No errors when the components are disabled.
  EXPECT_EQ(apm_->kNoError, ProcessStreamChooser(format));

  // -- Missing AGC level --
  EXPECT_EQ(apm_->kNoError, apm_->gain_control()->Enable(true));
  EXPECT_EQ(apm_->kStreamParameterNotSetError,
            ProcessStreamChooser(format));

  // Resets after successful ProcessStream().
  EXPECT_EQ(apm_->kNoError,
            apm_->gain_control()->set_stream_analog_level(127));
  EXPECT_EQ(apm_->kNoError, ProcessStreamChooser(format));
  EXPECT_EQ(apm_->kStreamParameterNotSetError,
            ProcessStreamChooser(format));

  // Other stream parameters set correctly.
  EXPECT_EQ(apm_->kNoError, apm_->echo_cancellation()->Enable(true));
  EXPECT_EQ(apm_->kNoError,
            apm_->echo_cancellation()->enable_drift_compensation(true));
  EXPECT_EQ(apm_->kNoError, apm_->set_stream_delay_ms(100));
  apm_->echo_cancellation()->set_stream_drift_samples(0);
  EXPECT_EQ(apm_->kStreamParameterNotSetError,
            ProcessStreamChooser(format));
  EXPECT_EQ(apm_->kNoError, apm_->gain_control()->Enable(false));
  EXPECT_EQ(apm_->kNoError,
            apm_->echo_cancellation()->enable_drift_compensation(false));

  // -- Missing delay --
  EXPECT_EQ(apm_->kNoError, apm_->echo_cancellation()->Enable(true));
  EXPECT_EQ(apm_->kNoError, ProcessStreamChooser(format));
  EXPECT_EQ(apm_->kStreamParameterNotSetError,
            ProcessStreamChooser(format));

  // Resets after successful ProcessStream().
  EXPECT_EQ(apm_->kNoError, apm_->set_stream_delay_ms(100));
  EXPECT_EQ(apm_->kNoError, ProcessStreamChooser(format));
  EXPECT_EQ(apm_->kStreamParameterNotSetError,
            ProcessStreamChooser(format));

  // Other stream parameters set correctly.
  EXPECT_EQ(apm_->kNoError, apm_->gain_control()->Enable(true));
  EXPECT_EQ(apm_->kNoError,
            apm_->echo_cancellation()->enable_drift_compensation(true));
  apm_->echo_cancellation()->set_stream_drift_samples(0);
  EXPECT_EQ(apm_->kNoError,
            apm_->gain_control()->set_stream_analog_level(127));
  EXPECT_EQ(apm_->kStreamParameterNotSetError,
            ProcessStreamChooser(format));
  EXPECT_EQ(apm_->kNoError, apm_->gain_control()->Enable(false));

  // -- Missing drift --
  EXPECT_EQ(apm_->kStreamParameterNotSetError,
            ProcessStreamChooser(format));

  // Resets after successful ProcessStream().
  EXPECT_EQ(apm_->kNoError, apm_->set_stream_delay_ms(100));
  apm_->echo_cancellation()->set_stream_drift_samples(0);
  EXPECT_EQ(apm_->kNoError, ProcessStreamChooser(format));
  EXPECT_EQ(apm_->kStreamParameterNotSetError,
            ProcessStreamChooser(format));

  // Other stream parameters set correctly.
  EXPECT_EQ(apm_->kNoError, apm_->gain_control()->Enable(true));
  EXPECT_EQ(apm_->kNoError, apm_->set_stream_delay_ms(100));
  EXPECT_EQ(apm_->kNoError,
            apm_->gain_control()->set_stream_analog_level(127));
  EXPECT_EQ(apm_->kStreamParameterNotSetError,
            ProcessStreamChooser(format));

  // -- No stream parameters --
  EXPECT_EQ(apm_->kNoError,
            AnalyzeReverseStreamChooser(format));
  EXPECT_EQ(apm_->kStreamParameterNotSetError,
            ProcessStreamChooser(format));

  // -- All there --
  EXPECT_EQ(apm_->kNoError, apm_->set_stream_delay_ms(100));
  apm_->echo_cancellation()->set_stream_drift_samples(0);
  EXPECT_EQ(apm_->kNoError,
            apm_->gain_control()->set_stream_analog_level(127));
  EXPECT_EQ(apm_->kNoError, ProcessStreamChooser(format));
}

TEST_F(ApmTest, StreamParametersInt) {
  StreamParametersTest(kIntFormat);
}

TEST_F(ApmTest, StreamParametersFloat) {
  StreamParametersTest(kFloatFormat);
}

TEST_F(ApmTest, DefaultDelayOffsetIsZero) {
  EXPECT_EQ(0, apm_->delay_offset_ms());
  EXPECT_EQ(apm_->kNoError, apm_->set_stream_delay_ms(50));
  EXPECT_EQ(50, apm_->stream_delay_ms());
}

TEST_F(ApmTest, DelayOffsetWithLimitsIsSetProperly) {
  // High limit of 500 ms.
  apm_->set_delay_offset_ms(100);
  EXPECT_EQ(100, apm_->delay_offset_ms());
  EXPECT_EQ(apm_->kBadStreamParameterWarning, apm_->set_stream_delay_ms(450));
  EXPECT_EQ(500, apm_->stream_delay_ms());
  EXPECT_EQ(apm_->kNoError, apm_->set_stream_delay_ms(100));
  EXPECT_EQ(200, apm_->stream_delay_ms());

  // Low limit of 0 ms.
  apm_->set_delay_offset_ms(-50);
  EXPECT_EQ(-50, apm_->delay_offset_ms());
  EXPECT_EQ(apm_->kBadStreamParameterWarning, apm_->set_stream_delay_ms(20));
  EXPECT_EQ(0, apm_->stream_delay_ms());
  EXPECT_EQ(apm_->kNoError, apm_->set_stream_delay_ms(100));
  EXPECT_EQ(50, apm_->stream_delay_ms());
}

void ApmTest::TestChangingChannels(int num_channels,
                                   AudioProcessing::Error expected_return) {
  frame_->num_channels_ = num_channels;
  EXPECT_EQ(expected_return, apm_->ProcessStream(frame_));
  EXPECT_EQ(expected_return, apm_->AnalyzeReverseStream(frame_));
}

TEST_F(ApmTest, Channels) {
  // Testing number of invalid channels.
  TestChangingChannels(0, apm_->kBadNumberChannelsError);
  TestChangingChannels(3, apm_->kBadNumberChannelsError);
  // Testing number of valid channels.
  for (int i = 1; i < 3; i++) {
    TestChangingChannels(i, kNoErr);
    EXPECT_EQ(i, apm_->num_input_channels());
    // We always force the number of reverse channels used for processing to 1.
    EXPECT_EQ(1, apm_->num_reverse_channels());
  }
}

TEST_F(ApmTest, SampleRatesInt) {
  // Testing invalid sample rates
  SetContainerFormat(10000, 2, frame_, &float_cb_);
  EXPECT_EQ(apm_->kBadSampleRateError, ProcessStreamChooser(kIntFormat));
  // Testing valid sample rates
  int fs[] = {8000, 16000, 32000};
  for (size_t i = 0; i < sizeof(fs) / sizeof(*fs); i++) {
    SetContainerFormat(fs[i], 2, frame_, &float_cb_);
    EXPECT_NOERR(ProcessStreamChooser(kIntFormat));
    EXPECT_EQ(fs[i], apm_->input_sample_rate_hz());
  }
}

TEST_F(ApmTest, EchoCancellation) {
  EXPECT_EQ(apm_->kNoError,
            apm_->echo_cancellation()->enable_drift_compensation(true));
  EXPECT_TRUE(apm_->echo_cancellation()->is_drift_compensation_enabled());
  EXPECT_EQ(apm_->kNoError,
            apm_->echo_cancellation()->enable_drift_compensation(false));
  EXPECT_FALSE(apm_->echo_cancellation()->is_drift_compensation_enabled());

  EchoCancellation::SuppressionLevel level[] = {
    EchoCancellation::kLowSuppression,
    EchoCancellation::kModerateSuppression,
    EchoCancellation::kHighSuppression,
  };
  for (size_t i = 0; i < sizeof(level)/sizeof(*level); i++) {
    EXPECT_EQ(apm_->kNoError,
        apm_->echo_cancellation()->set_suppression_level(level[i]));
    EXPECT_EQ(level[i],
        apm_->echo_cancellation()->suppression_level());
  }

  EchoCancellation::Metrics metrics;
  EXPECT_EQ(apm_->kNotEnabledError,
            apm_->echo_cancellation()->GetMetrics(&metrics));

  EXPECT_EQ(apm_->kNoError,
            apm_->echo_cancellation()->enable_metrics(true));
  EXPECT_TRUE(apm_->echo_cancellation()->are_metrics_enabled());
  EXPECT_EQ(apm_->kNoError,
            apm_->echo_cancellation()->enable_metrics(false));
  EXPECT_FALSE(apm_->echo_cancellation()->are_metrics_enabled());

  int median = 0;
  int std = 0;
  EXPECT_EQ(apm_->kNotEnabledError,
            apm_->echo_cancellation()->GetDelayMetrics(&median, &std));

  EXPECT_EQ(apm_->kNoError,
            apm_->echo_cancellation()->enable_delay_logging(true));
  EXPECT_TRUE(apm_->echo_cancellation()->is_delay_logging_enabled());
  EXPECT_EQ(apm_->kNoError,
            apm_->echo_cancellation()->enable_delay_logging(false));
  EXPECT_FALSE(apm_->echo_cancellation()->is_delay_logging_enabled());

  EXPECT_EQ(apm_->kNoError, apm_->echo_cancellation()->Enable(true));
  EXPECT_TRUE(apm_->echo_cancellation()->is_enabled());
  EXPECT_EQ(apm_->kNoError, apm_->echo_cancellation()->Enable(false));
  EXPECT_FALSE(apm_->echo_cancellation()->is_enabled());

  EXPECT_EQ(apm_->kNoError, apm_->echo_cancellation()->Enable(true));
  EXPECT_TRUE(apm_->echo_cancellation()->is_enabled());
  EXPECT_TRUE(apm_->echo_cancellation()->aec_core() != NULL);
  EXPECT_EQ(apm_->kNoError, apm_->echo_cancellation()->Enable(false));
  EXPECT_FALSE(apm_->echo_cancellation()->is_enabled());
  EXPECT_FALSE(apm_->echo_cancellation()->aec_core() != NULL);
}

TEST_F(ApmTest, DISABLED_EchoCancellationReportsCorrectDelays) {
  // TODO(bjornv): Fix this test to work with DA-AEC.
  // Enable AEC only.
  EXPECT_EQ(apm_->kNoError,
            apm_->echo_cancellation()->enable_drift_compensation(false));
  EXPECT_EQ(apm_->kNoError,
            apm_->echo_cancellation()->enable_metrics(false));
  EXPECT_EQ(apm_->kNoError,
            apm_->echo_cancellation()->enable_delay_logging(true));
  EXPECT_EQ(apm_->kNoError, apm_->echo_cancellation()->Enable(true));
  Config config;
  config.Set<ReportedDelay>(new ReportedDelay(true));
  apm_->SetExtraOptions(config);

  // Internally in the AEC the amount of lookahead the delay estimation can
  // handle is 15 blocks and the maximum delay is set to 60 blocks.
  const int kLookaheadBlocks = 15;
  const int kMaxDelayBlocks = 60;
  // The AEC has a startup time before it actually starts to process. This
  // procedure can flush the internal far-end buffer, which of course affects
  // the delay estimation. Therefore, we set a system_delay high enough to
  // avoid that. The smallest system_delay you can report without flushing the
  // buffer is 66 ms in 8 kHz.
  //
  // It is known that for 16 kHz (and 32 kHz) sampling frequency there is an
  // additional stuffing of 8 ms on the fly, but it seems to have no impact on
  // delay estimation. This should be noted though. In case of test failure,
  // this could be the cause.
  const int kSystemDelayMs = 66;
  // Test a couple of corner cases and verify that the estimated delay is
  // within a valid region (set to +-1.5 blocks). Note that these cases are
  // sampling frequency dependent.
  for (size_t i = 0; i < kProcessSampleRatesSize; i++) {
    Init(kProcessSampleRates[i],
         kProcessSampleRates[i],
         kProcessSampleRates[i],
         2,
         2,
         2,
         false);
    // Sampling frequency dependent variables.
    const int num_ms_per_block = std::max(4,
                                          640 / frame_->samples_per_channel_);
    const int delay_min_ms = -kLookaheadBlocks * num_ms_per_block;
    const int delay_max_ms = (kMaxDelayBlocks - 1) * num_ms_per_block;

    // 1) Verify correct delay estimate at lookahead boundary.
    int delay_ms = TruncateToMultipleOf10(kSystemDelayMs + delay_min_ms);
    ProcessDelayVerificationTest(delay_ms, kSystemDelayMs, delay_min_ms,
                                 delay_max_ms);
    // 2) A delay less than maximum lookahead should give an delay estimate at
    //    the boundary (= -kLookaheadBlocks * num_ms_per_block).
    delay_ms -= 20;
    ProcessDelayVerificationTest(delay_ms, kSystemDelayMs, delay_min_ms,
                                 delay_max_ms);
    // 3) Three values around zero delay. Note that we need to compensate for
    //    the fake system_delay.
    delay_ms = TruncateToMultipleOf10(kSystemDelayMs - 10);
    ProcessDelayVerificationTest(delay_ms, kSystemDelayMs, delay_min_ms,
                                 delay_max_ms);
    delay_ms = TruncateToMultipleOf10(kSystemDelayMs);
    ProcessDelayVerificationTest(delay_ms, kSystemDelayMs, delay_min_ms,
                                 delay_max_ms);
    delay_ms = TruncateToMultipleOf10(kSystemDelayMs + 10);
    ProcessDelayVerificationTest(delay_ms, kSystemDelayMs, delay_min_ms,
                                 delay_max_ms);
    // 4) Verify correct delay estimate at maximum delay boundary.
    delay_ms = TruncateToMultipleOf10(kSystemDelayMs + delay_max_ms);
    ProcessDelayVerificationTest(delay_ms, kSystemDelayMs, delay_min_ms,
                                 delay_max_ms);
    // 5) A delay above the maximum delay should give an estimate at the
    //    boundary (= (kMaxDelayBlocks - 1) * num_ms_per_block).
    delay_ms += 20;
    ProcessDelayVerificationTest(delay_ms, kSystemDelayMs, delay_min_ms,
                                 delay_max_ms);
  }
}

TEST_F(ApmTest, EchoControlMobile) {
  // AECM won't use super-wideband.
  SetFrameSampleRate(frame_, 32000);
  EXPECT_NOERR(apm_->ProcessStream(frame_));
  EXPECT_EQ(apm_->kBadSampleRateError,
            apm_->echo_control_mobile()->Enable(true));
  SetFrameSampleRate(frame_, 16000);
  EXPECT_NOERR(apm_->ProcessStream(frame_));
  EXPECT_EQ(apm_->kNoError,
            apm_->echo_control_mobile()->Enable(true));
  SetFrameSampleRate(frame_, 32000);
  EXPECT_EQ(apm_->kUnsupportedComponentError, apm_->ProcessStream(frame_));

  // Turn AECM on (and AEC off)
  Init(16000, 16000, 16000, 2, 2, 2, false);
  EXPECT_EQ(apm_->kNoError, apm_->echo_control_mobile()->Enable(true));
  EXPECT_TRUE(apm_->echo_control_mobile()->is_enabled());

  // Toggle routing modes
  EchoControlMobile::RoutingMode mode[] = {
      EchoControlMobile::kQuietEarpieceOrHeadset,
      EchoControlMobile::kEarpiece,
      EchoControlMobile::kLoudEarpiece,
      EchoControlMobile::kSpeakerphone,
      EchoControlMobile::kLoudSpeakerphone,
  };
  for (size_t i = 0; i < sizeof(mode)/sizeof(*mode); i++) {
    EXPECT_EQ(apm_->kNoError,
        apm_->echo_control_mobile()->set_routing_mode(mode[i]));
    EXPECT_EQ(mode[i],
        apm_->echo_control_mobile()->routing_mode());
  }
  // Turn comfort noise off/on
  EXPECT_EQ(apm_->kNoError,
      apm_->echo_control_mobile()->enable_comfort_noise(false));
  EXPECT_FALSE(apm_->echo_control_mobile()->is_comfort_noise_enabled());
  EXPECT_EQ(apm_->kNoError,
      apm_->echo_control_mobile()->enable_comfort_noise(true));
  EXPECT_TRUE(apm_->echo_control_mobile()->is_comfort_noise_enabled());
  // Set and get echo path
  const size_t echo_path_size =
      apm_->echo_control_mobile()->echo_path_size_bytes();
  scoped_ptr<char[]> echo_path_in(new char[echo_path_size]);
  scoped_ptr<char[]> echo_path_out(new char[echo_path_size]);
  EXPECT_EQ(apm_->kNullPointerError,
            apm_->echo_control_mobile()->SetEchoPath(NULL, echo_path_size));
  EXPECT_EQ(apm_->kNullPointerError,
            apm_->echo_control_mobile()->GetEchoPath(NULL, echo_path_size));
  EXPECT_EQ(apm_->kBadParameterError,
            apm_->echo_control_mobile()->GetEchoPath(echo_path_out.get(), 1));
  EXPECT_EQ(apm_->kNoError,
            apm_->echo_control_mobile()->GetEchoPath(echo_path_out.get(),
                                                     echo_path_size));
  for (size_t i = 0; i < echo_path_size; i++) {
    echo_path_in[i] = echo_path_out[i] + 1;
  }
  EXPECT_EQ(apm_->kBadParameterError,
            apm_->echo_control_mobile()->SetEchoPath(echo_path_in.get(), 1));
  EXPECT_EQ(apm_->kNoError,
            apm_->echo_control_mobile()->SetEchoPath(echo_path_in.get(),
                                                     echo_path_size));
  EXPECT_EQ(apm_->kNoError,
            apm_->echo_control_mobile()->GetEchoPath(echo_path_out.get(),
                                                     echo_path_size));
  for (size_t i = 0; i < echo_path_size; i++) {
    EXPECT_EQ(echo_path_in[i], echo_path_out[i]);
  }

  // Process a few frames with NS in the default disabled state. This exercises
  // a different codepath than with it enabled.
  EXPECT_EQ(apm_->kNoError, apm_->set_stream_delay_ms(0));
  EXPECT_EQ(apm_->kNoError, apm_->ProcessStream(frame_));
  EXPECT_EQ(apm_->kNoError, apm_->set_stream_delay_ms(0));
  EXPECT_EQ(apm_->kNoError, apm_->ProcessStream(frame_));

  // Turn AECM off
  EXPECT_EQ(apm_->kNoError, apm_->echo_control_mobile()->Enable(false));
  EXPECT_FALSE(apm_->echo_control_mobile()->is_enabled());
}

TEST_F(ApmTest, GainControl) {
  // Testing gain modes
  EXPECT_EQ(apm_->kNoError,
      apm_->gain_control()->set_mode(
      apm_->gain_control()->mode()));

  GainControl::Mode mode[] = {
    GainControl::kAdaptiveAnalog,
    GainControl::kAdaptiveDigital,
    GainControl::kFixedDigital
  };
  for (size_t i = 0; i < sizeof(mode)/sizeof(*mode); i++) {
    EXPECT_EQ(apm_->kNoError,
        apm_->gain_control()->set_mode(mode[i]));
    EXPECT_EQ(mode[i], apm_->gain_control()->mode());
  }
  // Testing invalid target levels
  EXPECT_EQ(apm_->kBadParameterError,
      apm_->gain_control()->set_target_level_dbfs(-3));
  EXPECT_EQ(apm_->kBadParameterError,
      apm_->gain_control()->set_target_level_dbfs(-40));
  // Testing valid target levels
  EXPECT_EQ(apm_->kNoError,
      apm_->gain_control()->set_target_level_dbfs(
      apm_->gain_control()->target_level_dbfs()));

  int level_dbfs[] = {0, 6, 31};
  for (size_t i = 0; i < sizeof(level_dbfs)/sizeof(*level_dbfs); i++) {
    EXPECT_EQ(apm_->kNoError,
        apm_->gain_control()->set_target_level_dbfs(level_dbfs[i]));
    EXPECT_EQ(level_dbfs[i], apm_->gain_control()->target_level_dbfs());
  }

  // Testing invalid compression gains
  EXPECT_EQ(apm_->kBadParameterError,
      apm_->gain_control()->set_compression_gain_db(-1));
  EXPECT_EQ(apm_->kBadParameterError,
      apm_->gain_control()->set_compression_gain_db(100));

  // Testing valid compression gains
  EXPECT_EQ(apm_->kNoError,
      apm_->gain_control()->set_compression_gain_db(
      apm_->gain_control()->compression_gain_db()));

  int gain_db[] = {0, 10, 90};
  for (size_t i = 0; i < sizeof(gain_db)/sizeof(*gain_db); i++) {
    EXPECT_EQ(apm_->kNoError,
        apm_->gain_control()->set_compression_gain_db(gain_db[i]));
    EXPECT_EQ(gain_db[i], apm_->gain_control()->compression_gain_db());
  }

  // Testing limiter off/on
  EXPECT_EQ(apm_->kNoError, apm_->gain_control()->enable_limiter(false));
  EXPECT_FALSE(apm_->gain_control()->is_limiter_enabled());
  EXPECT_EQ(apm_->kNoError, apm_->gain_control()->enable_limiter(true));
  EXPECT_TRUE(apm_->gain_control()->is_limiter_enabled());

  // Testing invalid level limits
  EXPECT_EQ(apm_->kBadParameterError,
      apm_->gain_control()->set_analog_level_limits(-1, 512));
  EXPECT_EQ(apm_->kBadParameterError,
      apm_->gain_control()->set_analog_level_limits(100000, 512));
  EXPECT_EQ(apm_->kBadParameterError,
      apm_->gain_control()->set_analog_level_limits(512, -1));
  EXPECT_EQ(apm_->kBadParameterError,
      apm_->gain_control()->set_analog_level_limits(512, 100000));
  EXPECT_EQ(apm_->kBadParameterError,
      apm_->gain_control()->set_analog_level_limits(512, 255));

  // Testing valid level limits
  EXPECT_EQ(apm_->kNoError,
      apm_->gain_control()->set_analog_level_limits(
      apm_->gain_control()->analog_level_minimum(),
      apm_->gain_control()->analog_level_maximum()));

  int min_level[] = {0, 255, 1024};
  for (size_t i = 0; i < sizeof(min_level)/sizeof(*min_level); i++) {
    EXPECT_EQ(apm_->kNoError,
        apm_->gain_control()->set_analog_level_limits(min_level[i], 1024));
    EXPECT_EQ(min_level[i], apm_->gain_control()->analog_level_minimum());
  }

  int max_level[] = {0, 1024, 65535};
  for (size_t i = 0; i < sizeof(min_level)/sizeof(*min_level); i++) {
    EXPECT_EQ(apm_->kNoError,
        apm_->gain_control()->set_analog_level_limits(0, max_level[i]));
    EXPECT_EQ(max_level[i], apm_->gain_control()->analog_level_maximum());
  }

  // TODO(ajm): stream_is_saturated() and stream_analog_level()

  // Turn AGC off
  EXPECT_EQ(apm_->kNoError, apm_->gain_control()->Enable(false));
  EXPECT_FALSE(apm_->gain_control()->is_enabled());
}

void ApmTest::RunQuantizedVolumeDoesNotGetStuckTest(int sample_rate) {
  Init(sample_rate, sample_rate, sample_rate, 2, 2, 2, false);
  EXPECT_EQ(apm_->kNoError,
            apm_->gain_control()->set_mode(GainControl::kAdaptiveAnalog));
  EXPECT_EQ(apm_->kNoError, apm_->gain_control()->Enable(true));

  int out_analog_level = 0;
  for (int i = 0; i < 2000; ++i) {
    ReadFrameWithRewind(near_file_, frame_);
    // Ensure the audio is at a low level, so the AGC will try to increase it.
    ScaleFrame(frame_, 0.25);

    // Always pass in the same volume.
    EXPECT_EQ(apm_->kNoError,
        apm_->gain_control()->set_stream_analog_level(100));
    EXPECT_EQ(apm_->kNoError, apm_->ProcessStream(frame_));
    out_analog_level = apm_->gain_control()->stream_analog_level();
  }

  // Ensure the AGC is still able to reach the maximum.
  EXPECT_EQ(255, out_analog_level);
}

// Verifies that despite volume slider quantization, the AGC can continue to
// increase its volume.
TEST_F(ApmTest, QuantizedVolumeDoesNotGetStuck) {
  for (size_t i = 0; i < kSampleRatesSize; ++i) {
    RunQuantizedVolumeDoesNotGetStuckTest(kSampleRates[i]);
  }
}

void ApmTest::RunManualVolumeChangeIsPossibleTest(int sample_rate) {
  Init(sample_rate, sample_rate, sample_rate, 2, 2, 2, false);
  EXPECT_EQ(apm_->kNoError,
            apm_->gain_control()->set_mode(GainControl::kAdaptiveAnalog));
  EXPECT_EQ(apm_->kNoError, apm_->gain_control()->Enable(true));

  int out_analog_level = 100;
  for (int i = 0; i < 1000; ++i) {
    ReadFrameWithRewind(near_file_, frame_);
    // Ensure the audio is at a low level, so the AGC will try to increase it.
    ScaleFrame(frame_, 0.25);

    EXPECT_EQ(apm_->kNoError,
        apm_->gain_control()->set_stream_analog_level(out_analog_level));
    EXPECT_EQ(apm_->kNoError, apm_->ProcessStream(frame_));
    out_analog_level = apm_->gain_control()->stream_analog_level();
  }

  // Ensure the volume was raised.
  EXPECT_GT(out_analog_level, 100);
  int highest_level_reached = out_analog_level;
  // Simulate a user manual volume change.
  out_analog_level = 100;

  for (int i = 0; i < 300; ++i) {
    ReadFrameWithRewind(near_file_, frame_);
    ScaleFrame(frame_, 0.25);

    EXPECT_EQ(apm_->kNoError,
        apm_->gain_control()->set_stream_analog_level(out_analog_level));
    EXPECT_EQ(apm_->kNoError, apm_->ProcessStream(frame_));
    out_analog_level = apm_->gain_control()->stream_analog_level();
    // Check that AGC respected the manually adjusted volume.
    EXPECT_LT(out_analog_level, highest_level_reached);
  }
  // Check that the volume was still raised.
  EXPECT_GT(out_analog_level, 100);
}

TEST_F(ApmTest, ManualVolumeChangeIsPossible) {
  for (size_t i = 0; i < kSampleRatesSize; ++i) {
    RunManualVolumeChangeIsPossibleTest(kSampleRates[i]);
  }
}

#if !defined(WEBRTC_ANDROID) && !defined(WEBRTC_IOS)
TEST_F(ApmTest, AgcOnlyAdaptsWhenTargetSignalIsPresent) {
  const int kSampleRateHz = 16000;
  const int kSamplesPerChannel =
      AudioProcessing::kChunkSizeMs * kSampleRateHz / 1000;
  const int kNumInputChannels = 2;
  const int kNumOutputChannels = 1;
  const int kNumChunks = 700;
  const float kScaleFactor = 0.25f;
  Config config;
  std::vector<webrtc::Point> geometry;
  geometry.push_back(webrtc::Point(0.f, 0.f, 0.f));
  geometry.push_back(webrtc::Point(0.05f, 0.f, 0.f));
  config.Set<Beamforming>(new Beamforming(true, geometry));
  testing::NiceMock<MockBeamformer>* beamformer =
      new testing::NiceMock<MockBeamformer>(geometry);
  scoped_ptr<AudioProcessing> apm(AudioProcessing::Create(config, beamformer));
  EXPECT_EQ(kNoErr, apm->gain_control()->Enable(true));
  ChannelBuffer<float> src_buf(kSamplesPerChannel, kNumInputChannels);
  ChannelBuffer<float> dest_buf(kSamplesPerChannel, kNumOutputChannels);
  const int max_length = kSamplesPerChannel * std::max(kNumInputChannels,
                                                       kNumOutputChannels);
  scoped_ptr<int16_t[]> int_data(new int16_t[max_length]);
  scoped_ptr<float[]> float_data(new float[max_length]);
  std::string filename = ResourceFilePath("far", kSampleRateHz);
  FILE* far_file = fopen(filename.c_str(), "rb");
  ASSERT_TRUE(far_file != NULL) << "Could not open file " << filename << "\n";
  const int kDefaultVolume = apm->gain_control()->stream_analog_level();
  const int kDefaultCompressionGain =
      apm->gain_control()->compression_gain_db();
  bool is_target = false;
  EXPECT_CALL(*beamformer, is_target_present())
      .WillRepeatedly(testing::ReturnPointee(&is_target));
  for (int i = 0; i < kNumChunks; ++i) {
    ASSERT_TRUE(ReadChunk(far_file,
                          int_data.get(),
                          float_data.get(),
                          &src_buf));
    for (int j = 0; j < kNumInputChannels * kSamplesPerChannel; ++j) {
      src_buf.data()[j] *= kScaleFactor;
    }
    EXPECT_EQ(kNoErr,
              apm->ProcessStream(src_buf.channels(),
                                 src_buf.samples_per_channel(),
                                 kSampleRateHz,
                                 LayoutFromChannels(src_buf.num_channels()),
                                 kSampleRateHz,
                                 LayoutFromChannels(dest_buf.num_channels()),
                                 dest_buf.channels()));
  }
  EXPECT_EQ(kDefaultVolume,
            apm->gain_control()->stream_analog_level());
  EXPECT_EQ(kDefaultCompressionGain,
            apm->gain_control()->compression_gain_db());
  rewind(far_file);
  is_target = true;
  for (int i = 0; i < kNumChunks; ++i) {
    ASSERT_TRUE(ReadChunk(far_file,
                          int_data.get(),
                          float_data.get(),
                          &src_buf));
    for (int j = 0; j < kNumInputChannels * kSamplesPerChannel; ++j) {
      src_buf.data()[j] *= kScaleFactor;
    }
    EXPECT_EQ(kNoErr,
              apm->ProcessStream(src_buf.channels(),
                                 src_buf.samples_per_channel(),
                                 kSampleRateHz,
                                 LayoutFromChannels(src_buf.num_channels()),
                                 kSampleRateHz,
                                 LayoutFromChannels(dest_buf.num_channels()),
                                 dest_buf.channels()));
  }
  EXPECT_LT(kDefaultVolume,
            apm->gain_control()->stream_analog_level());
  EXPECT_LT(kDefaultCompressionGain,
            apm->gain_control()->compression_gain_db());
  ASSERT_EQ(0, fclose(far_file));
}
#endif

TEST_F(ApmTest, NoiseSuppression) {
  // Test valid suppression levels.
  NoiseSuppression::Level level[] = {
    NoiseSuppression::kLow,
    NoiseSuppression::kModerate,
    NoiseSuppression::kHigh,
    NoiseSuppression::kVeryHigh
  };
  for (size_t i = 0; i < sizeof(level)/sizeof(*level); i++) {
    EXPECT_EQ(apm_->kNoError,
        apm_->noise_suppression()->set_level(level[i]));
    EXPECT_EQ(level[i], apm_->noise_suppression()->level());
  }

  // Turn NS on/off
  EXPECT_EQ(apm_->kNoError, apm_->noise_suppression()->Enable(true));
  EXPECT_TRUE(apm_->noise_suppression()->is_enabled());
  EXPECT_EQ(apm_->kNoError, apm_->noise_suppression()->Enable(false));
  EXPECT_FALSE(apm_->noise_suppression()->is_enabled());
}

TEST_F(ApmTest, HighPassFilter) {
  // Turn HP filter on/off
  EXPECT_EQ(apm_->kNoError, apm_->high_pass_filter()->Enable(true));
  EXPECT_TRUE(apm_->high_pass_filter()->is_enabled());
  EXPECT_EQ(apm_->kNoError, apm_->high_pass_filter()->Enable(false));
  EXPECT_FALSE(apm_->high_pass_filter()->is_enabled());
}

TEST_F(ApmTest, LevelEstimator) {
  // Turn level estimator on/off
  EXPECT_EQ(apm_->kNoError, apm_->level_estimator()->Enable(false));
  EXPECT_FALSE(apm_->level_estimator()->is_enabled());

  EXPECT_EQ(apm_->kNotEnabledError, apm_->level_estimator()->RMS());

  EXPECT_EQ(apm_->kNoError, apm_->level_estimator()->Enable(true));
  EXPECT_TRUE(apm_->level_estimator()->is_enabled());

  // Run this test in wideband; in super-wb, the splitting filter distorts the
  // audio enough to cause deviation from the expectation for small values.
  frame_->samples_per_channel_ = 160;
  frame_->num_channels_ = 2;
  frame_->sample_rate_hz_ = 16000;

  // Min value if no frames have been processed.
  EXPECT_EQ(127, apm_->level_estimator()->RMS());

  // Min value on zero frames.
  SetFrameTo(frame_, 0);
  EXPECT_EQ(apm_->kNoError, apm_->ProcessStream(frame_));
  EXPECT_EQ(apm_->kNoError, apm_->ProcessStream(frame_));
  EXPECT_EQ(127, apm_->level_estimator()->RMS());

  // Try a few RMS values.
  // (These also test that the value resets after retrieving it.)
  SetFrameTo(frame_, 32767);
  EXPECT_EQ(apm_->kNoError, apm_->ProcessStream(frame_));
  EXPECT_EQ(apm_->kNoError, apm_->ProcessStream(frame_));
  EXPECT_EQ(0, apm_->level_estimator()->RMS());

  SetFrameTo(frame_, 30000);
  EXPECT_EQ(apm_->kNoError, apm_->ProcessStream(frame_));
  EXPECT_EQ(apm_->kNoError, apm_->ProcessStream(frame_));
  EXPECT_EQ(1, apm_->level_estimator()->RMS());

  SetFrameTo(frame_, 10000);
  EXPECT_EQ(apm_->kNoError, apm_->ProcessStream(frame_));
  EXPECT_EQ(apm_->kNoError, apm_->ProcessStream(frame_));
  EXPECT_EQ(10, apm_->level_estimator()->RMS());

  SetFrameTo(frame_, 10);
  EXPECT_EQ(apm_->kNoError, apm_->ProcessStream(frame_));
  EXPECT_EQ(apm_->kNoError, apm_->ProcessStream(frame_));
  EXPECT_EQ(70, apm_->level_estimator()->RMS());

  // Verify reset after enable/disable.
  SetFrameTo(frame_, 32767);
  EXPECT_EQ(apm_->kNoError, apm_->ProcessStream(frame_));
  EXPECT_EQ(apm_->kNoError, apm_->level_estimator()->Enable(false));
  EXPECT_EQ(apm_->kNoError, apm_->level_estimator()->Enable(true));
  SetFrameTo(frame_, 1);
  EXPECT_EQ(apm_->kNoError, apm_->ProcessStream(frame_));
  EXPECT_EQ(90, apm_->level_estimator()->RMS());

  // Verify reset after initialize.
  SetFrameTo(frame_, 32767);
  EXPECT_EQ(apm_->kNoError, apm_->ProcessStream(frame_));
  EXPECT_EQ(apm_->kNoError, apm_->Initialize());
  SetFrameTo(frame_, 1);
  EXPECT_EQ(apm_->kNoError, apm_->ProcessStream(frame_));
  EXPECT_EQ(90, apm_->level_estimator()->RMS());
}

TEST_F(ApmTest, VoiceDetection) {
  // Test external VAD
  EXPECT_EQ(apm_->kNoError,
            apm_->voice_detection()->set_stream_has_voice(true));
  EXPECT_TRUE(apm_->voice_detection()->stream_has_voice());
  EXPECT_EQ(apm_->kNoError,
            apm_->voice_detection()->set_stream_has_voice(false));
  EXPECT_FALSE(apm_->voice_detection()->stream_has_voice());

  // Test valid likelihoods
  VoiceDetection::Likelihood likelihood[] = {
      VoiceDetection::kVeryLowLikelihood,
      VoiceDetection::kLowLikelihood,
      VoiceDetection::kModerateLikelihood,
      VoiceDetection::kHighLikelihood
  };
  for (size_t i = 0; i < sizeof(likelihood)/sizeof(*likelihood); i++) {
    EXPECT_EQ(apm_->kNoError,
              apm_->voice_detection()->set_likelihood(likelihood[i]));
    EXPECT_EQ(likelihood[i], apm_->voice_detection()->likelihood());
  }

  /* TODO(bjornv): Enable once VAD supports other frame lengths than 10 ms
  // Test invalid frame sizes
  EXPECT_EQ(apm_->kBadParameterError,
      apm_->voice_detection()->set_frame_size_ms(12));

  // Test valid frame sizes
  for (int i = 10; i <= 30; i += 10) {
    EXPECT_EQ(apm_->kNoError,
        apm_->voice_detection()->set_frame_size_ms(i));
    EXPECT_EQ(i, apm_->voice_detection()->frame_size_ms());
  }
  */

  // Turn VAD on/off
  EXPECT_EQ(apm_->kNoError, apm_->voice_detection()->Enable(true));
  EXPECT_TRUE(apm_->voice_detection()->is_enabled());
  EXPECT_EQ(apm_->kNoError, apm_->voice_detection()->Enable(false));
  EXPECT_FALSE(apm_->voice_detection()->is_enabled());

  // Test that AudioFrame activity is maintained when VAD is disabled.
  EXPECT_EQ(apm_->kNoError, apm_->voice_detection()->Enable(false));
  AudioFrame::VADActivity activity[] = {
      AudioFrame::kVadActive,
      AudioFrame::kVadPassive,
      AudioFrame::kVadUnknown
  };
  for (size_t i = 0; i < sizeof(activity)/sizeof(*activity); i++) {
    frame_->vad_activity_ = activity[i];
    EXPECT_EQ(apm_->kNoError, apm_->ProcessStream(frame_));
    EXPECT_EQ(activity[i], frame_->vad_activity_);
  }

  // Test that AudioFrame activity is set when VAD is enabled.
  EXPECT_EQ(apm_->kNoError, apm_->voice_detection()->Enable(true));
  frame_->vad_activity_ = AudioFrame::kVadUnknown;
  EXPECT_EQ(apm_->kNoError, apm_->ProcessStream(frame_));
  EXPECT_NE(AudioFrame::kVadUnknown, frame_->vad_activity_);

  // TODO(bjornv): Add tests for streamed voice; stream_has_voice()
}

TEST_F(ApmTest, AllProcessingDisabledByDefault) {
  EXPECT_FALSE(apm_->echo_cancellation()->is_enabled());
  EXPECT_FALSE(apm_->echo_control_mobile()->is_enabled());
  EXPECT_FALSE(apm_->gain_control()->is_enabled());
  EXPECT_FALSE(apm_->high_pass_filter()->is_enabled());
  EXPECT_FALSE(apm_->level_estimator()->is_enabled());
  EXPECT_FALSE(apm_->noise_suppression()->is_enabled());
  EXPECT_FALSE(apm_->voice_detection()->is_enabled());
}

TEST_F(ApmTest, NoProcessingWhenAllComponentsDisabled) {
  for (size_t i = 0; i < kSampleRatesSize; i++) {
    Init(kSampleRates[i], kSampleRates[i], kSampleRates[i], 2, 2, 2, false);
    SetFrameTo(frame_, 1000, 2000);
    AudioFrame frame_copy;
    frame_copy.CopyFrom(*frame_);
    for (int j = 0; j < 1000; j++) {
      EXPECT_EQ(apm_->kNoError, apm_->ProcessStream(frame_));
      EXPECT_TRUE(FrameDataAreEqual(*frame_, frame_copy));
    }
  }
}

TEST_F(ApmTest, NoProcessingWhenAllComponentsDisabledFloat) {
  // Test that ProcessStream copies input to output even with no processing.
  const size_t kSamples = 80;
  const int sample_rate = 8000;
  const float src[kSamples] = {
    -1.0f, 0.0f, 1.0f
  };
  float dest[kSamples] = {};

  auto src_channels = &src[0];
  auto dest_channels = &dest[0];

  apm_.reset(AudioProcessing::Create());
  EXPECT_NOERR(apm_->ProcessStream(
      &src_channels, kSamples, sample_rate, LayoutFromChannels(1),
      sample_rate, LayoutFromChannels(1), &dest_channels));

  for (size_t i = 0; i < kSamples; ++i) {
    EXPECT_EQ(src[i], dest[i]);
  }
}

TEST_F(ApmTest, IdenticalInputChannelsResultInIdenticalOutputChannels) {
  EnableAllComponents();

  for (size_t i = 0; i < kProcessSampleRatesSize; i++) {
    Init(kProcessSampleRates[i],
         kProcessSampleRates[i],
         kProcessSampleRates[i],
         2,
         2,
         2,
         false);
    int analog_level = 127;
    ASSERT_EQ(0, feof(far_file_));
    ASSERT_EQ(0, feof(near_file_));
    while (ReadFrame(far_file_, revframe_) && ReadFrame(near_file_, frame_)) {
      CopyLeftToRightChannel(revframe_->data_, revframe_->samples_per_channel_);

      ASSERT_EQ(kNoErr, apm_->AnalyzeReverseStream(revframe_));

      CopyLeftToRightChannel(frame_->data_, frame_->samples_per_channel_);
      frame_->vad_activity_ = AudioFrame::kVadUnknown;

      ASSERT_EQ(kNoErr, apm_->set_stream_delay_ms(0));
      apm_->echo_cancellation()->set_stream_drift_samples(0);
      ASSERT_EQ(kNoErr,
          apm_->gain_control()->set_stream_analog_level(analog_level));
      ASSERT_EQ(kNoErr, apm_->ProcessStream(frame_));
      analog_level = apm_->gain_control()->stream_analog_level();

      VerifyChannelsAreEqual(frame_->data_, frame_->samples_per_channel_);
    }
    rewind(far_file_);
    rewind(near_file_);
  }
}

TEST_F(ApmTest, SplittingFilter) {
  // Verify the filter is not active through undistorted audio when:
  // 1. No components are enabled...
  SetFrameTo(frame_, 1000);
  AudioFrame frame_copy;
  frame_copy.CopyFrom(*frame_);
  EXPECT_EQ(apm_->kNoError, apm_->ProcessStream(frame_));
  EXPECT_EQ(apm_->kNoError, apm_->ProcessStream(frame_));
  EXPECT_TRUE(FrameDataAreEqual(*frame_, frame_copy));

  // 2. Only the level estimator is enabled...
  SetFrameTo(frame_, 1000);
  frame_copy.CopyFrom(*frame_);
  EXPECT_EQ(apm_->kNoError, apm_->level_estimator()->Enable(true));
  EXPECT_EQ(apm_->kNoError, apm_->ProcessStream(frame_));
  EXPECT_EQ(apm_->kNoError, apm_->ProcessStream(frame_));
  EXPECT_TRUE(FrameDataAreEqual(*frame_, frame_copy));
  EXPECT_EQ(apm_->kNoError, apm_->level_estimator()->Enable(false));

  // 3. Only VAD is enabled...
  SetFrameTo(frame_, 1000);
  frame_copy.CopyFrom(*frame_);
  EXPECT_EQ(apm_->kNoError, apm_->voice_detection()->Enable(true));
  EXPECT_EQ(apm_->kNoError, apm_->ProcessStream(frame_));
  EXPECT_EQ(apm_->kNoError, apm_->ProcessStream(frame_));
  EXPECT_TRUE(FrameDataAreEqual(*frame_, frame_copy));
  EXPECT_EQ(apm_->kNoError, apm_->voice_detection()->Enable(false));

  // 4. Both VAD and the level estimator are enabled...
  SetFrameTo(frame_, 1000);
  frame_copy.CopyFrom(*frame_);
  EXPECT_EQ(apm_->kNoError, apm_->level_estimator()->Enable(true));
  EXPECT_EQ(apm_->kNoError, apm_->voice_detection()->Enable(true));
  EXPECT_EQ(apm_->kNoError, apm_->ProcessStream(frame_));
  EXPECT_EQ(apm_->kNoError, apm_->ProcessStream(frame_));
  EXPECT_TRUE(FrameDataAreEqual(*frame_, frame_copy));
  EXPECT_EQ(apm_->kNoError, apm_->level_estimator()->Enable(false));
  EXPECT_EQ(apm_->kNoError, apm_->voice_detection()->Enable(false));

  // 5. Not using super-wb.
  frame_->samples_per_channel_ = 160;
  frame_->num_channels_ = 2;
  frame_->sample_rate_hz_ = 16000;
  // Enable AEC, which would require the filter in super-wb. We rely on the
  // first few frames of data being unaffected by the AEC.
  // TODO(andrew): This test, and the one below, rely rather tenuously on the
  // behavior of the AEC. Think of something more robust.
  EXPECT_EQ(apm_->kNoError, apm_->echo_cancellation()->Enable(true));
  // Make sure we have extended filter enabled. This makes sure nothing is
  // touched until we have a farend frame.
  Config config;
  config.Set<DelayCorrection>(new DelayCorrection(true));
  apm_->SetExtraOptions(config);
  SetFrameTo(frame_, 1000);
  frame_copy.CopyFrom(*frame_);
  EXPECT_EQ(apm_->kNoError, apm_->set_stream_delay_ms(0));
  apm_->echo_cancellation()->set_stream_drift_samples(0);
  EXPECT_EQ(apm_->kNoError, apm_->ProcessStream(frame_));
  EXPECT_EQ(apm_->kNoError, apm_->set_stream_delay_ms(0));
  apm_->echo_cancellation()->set_stream_drift_samples(0);
  EXPECT_EQ(apm_->kNoError, apm_->ProcessStream(frame_));
  EXPECT_TRUE(FrameDataAreEqual(*frame_, frame_copy));

  // Check the test is valid. We should have distortion from the filter
  // when AEC is enabled (which won't affect the audio).
  frame_->samples_per_channel_ = 320;
  frame_->num_channels_ = 2;
  frame_->sample_rate_hz_ = 32000;
  SetFrameTo(frame_, 1000);
  frame_copy.CopyFrom(*frame_);
  EXPECT_EQ(apm_->kNoError, apm_->set_stream_delay_ms(0));
  apm_->echo_cancellation()->set_stream_drift_samples(0);
  EXPECT_EQ(apm_->kNoError, apm_->ProcessStream(frame_));
  EXPECT_FALSE(FrameDataAreEqual(*frame_, frame_copy));
}

#ifdef WEBRTC_AUDIOPROC_DEBUG_DUMP
void ApmTest::ProcessDebugDump(const std::string& in_filename,
                               const std::string& out_filename,
                               Format format) {
  FILE* in_file = fopen(in_filename.c_str(), "rb");
  ASSERT_TRUE(in_file != NULL);
  audioproc::Event event_msg;
  bool first_init = true;

  while (ReadMessageFromFile(in_file, &event_msg)) {
    if (event_msg.type() == audioproc::Event::INIT) {
      const audioproc::Init msg = event_msg.init();
      int reverse_sample_rate = msg.sample_rate();
      if (msg.has_reverse_sample_rate()) {
        reverse_sample_rate = msg.reverse_sample_rate();
      }
      int output_sample_rate = msg.sample_rate();
      if (msg.has_output_sample_rate()) {
        output_sample_rate = msg.output_sample_rate();
      }

      Init(msg.sample_rate(),
           output_sample_rate,
           reverse_sample_rate,
           msg.num_input_channels(),
           msg.num_output_channels(),
           msg.num_reverse_channels(),
           false);
      if (first_init) {
        // StartDebugRecording() writes an additional init message. Don't start
        // recording until after the first init to avoid the extra message.
        EXPECT_NOERR(apm_->StartDebugRecording(out_filename.c_str()));
        first_init = false;
      }

    } else if (event_msg.type() == audioproc::Event::REVERSE_STREAM) {
      const audioproc::ReverseStream msg = event_msg.reverse_stream();

      if (msg.channel_size() > 0) {
        ASSERT_EQ(revframe_->num_channels_, msg.channel_size());
        for (int i = 0; i < msg.channel_size(); ++i) {
           memcpy(revfloat_cb_->channel(i), msg.channel(i).data(),
                  msg.channel(i).size());
        }
      } else {
        memcpy(revframe_->data_, msg.data().data(), msg.data().size());
        if (format == kFloatFormat) {
          // We're using an int16 input file; convert to float.
          ConvertToFloat(*revframe_, revfloat_cb_.get());
        }
      }
      AnalyzeReverseStreamChooser(format);

    } else if (event_msg.type() == audioproc::Event::STREAM) {
      const audioproc::Stream msg = event_msg.stream();
      // ProcessStream could have changed this for the output frame.
      frame_->num_channels_ = apm_->num_input_channels();

      EXPECT_NOERR(apm_->gain_control()->set_stream_analog_level(msg.level()));
      EXPECT_NOERR(apm_->set_stream_delay_ms(msg.delay()));
      apm_->echo_cancellation()->set_stream_drift_samples(msg.drift());
      if (msg.has_keypress()) {
        apm_->set_stream_key_pressed(msg.keypress());
      } else {
        apm_->set_stream_key_pressed(true);
      }

      if (msg.input_channel_size() > 0) {
        ASSERT_EQ(frame_->num_channels_, msg.input_channel_size());
        for (int i = 0; i < msg.input_channel_size(); ++i) {
           memcpy(float_cb_->channel(i), msg.input_channel(i).data(),
                  msg.input_channel(i).size());
        }
      } else {
        memcpy(frame_->data_, msg.input_data().data(), msg.input_data().size());
        if (format == kFloatFormat) {
          // We're using an int16 input file; convert to float.
          ConvertToFloat(*frame_, float_cb_.get());
        }
      }
      ProcessStreamChooser(format);
    }
  }
  EXPECT_NOERR(apm_->StopDebugRecording());
  fclose(in_file);
}

void ApmTest::VerifyDebugDumpTest(Format format) {
  const std::string in_filename = test::ResourcePath("ref03", "aecdump");
  std::string format_string;
  switch (format) {
    case kIntFormat:
      format_string = "_int";
      break;
    case kFloatFormat:
      format_string = "_float";
      break;
  }
  const std::string ref_filename = test::TempFilename(
      test::OutputPath(), std::string("ref") + format_string + "_aecdump");
  const std::string out_filename = test::TempFilename(
      test::OutputPath(), std::string("out") + format_string + "_aecdump");
  EnableAllComponents();
  ProcessDebugDump(in_filename, ref_filename, format);
  ProcessDebugDump(ref_filename, out_filename, format);

  FILE* ref_file = fopen(ref_filename.c_str(), "rb");
  FILE* out_file = fopen(out_filename.c_str(), "rb");
  ASSERT_TRUE(ref_file != NULL);
  ASSERT_TRUE(out_file != NULL);
  scoped_ptr<uint8_t[]> ref_bytes;
  scoped_ptr<uint8_t[]> out_bytes;

  size_t ref_size = ReadMessageBytesFromFile(ref_file, &ref_bytes);
  size_t out_size = ReadMessageBytesFromFile(out_file, &out_bytes);
  size_t bytes_read = 0;
  while (ref_size > 0 && out_size > 0) {
    bytes_read += ref_size;
    EXPECT_EQ(ref_size, out_size);
    EXPECT_EQ(0, memcmp(ref_bytes.get(), out_bytes.get(), ref_size));
    ref_size = ReadMessageBytesFromFile(ref_file, &ref_bytes);
    out_size = ReadMessageBytesFromFile(out_file, &out_bytes);
  }
  EXPECT_GT(bytes_read, 0u);
  EXPECT_NE(0, feof(ref_file));
  EXPECT_NE(0, feof(out_file));
  ASSERT_EQ(0, fclose(ref_file));
  ASSERT_EQ(0, fclose(out_file));
}

TEST_F(ApmTest, VerifyDebugDumpInt) {
  VerifyDebugDumpTest(kIntFormat);
}

TEST_F(ApmTest, VerifyDebugDumpFloat) {
  VerifyDebugDumpTest(kFloatFormat);
}
#endif

// TODO(andrew): expand test to verify output.
TEST_F(ApmTest, DebugDump) {
  const std::string filename =
      test::TempFilename(test::OutputPath(), "debug_aec");
  EXPECT_EQ(apm_->kNullPointerError,
            apm_->StartDebugRecording(static_cast<const char*>(NULL)));

#ifdef WEBRTC_AUDIOPROC_DEBUG_DUMP
  // Stopping without having started should be OK.
  EXPECT_EQ(apm_->kNoError, apm_->StopDebugRecording());

  EXPECT_EQ(apm_->kNoError, apm_->StartDebugRecording(filename.c_str()));
  EXPECT_EQ(apm_->kNoError, apm_->ProcessStream(frame_));
  EXPECT_EQ(apm_->kNoError, apm_->AnalyzeReverseStream(revframe_));
  EXPECT_EQ(apm_->kNoError, apm_->StopDebugRecording());

  // Verify the file has been written.
  FILE* fid = fopen(filename.c_str(), "r");
  ASSERT_TRUE(fid != NULL);

  // Clean it up.
  ASSERT_EQ(0, fclose(fid));
  ASSERT_EQ(0, remove(filename.c_str()));
#else
  EXPECT_EQ(apm_->kUnsupportedFunctionError,
            apm_->StartDebugRecording(filename.c_str()));
  EXPECT_EQ(apm_->kUnsupportedFunctionError, apm_->StopDebugRecording());

  // Verify the file has NOT been written.
  ASSERT_TRUE(fopen(filename.c_str(), "r") == NULL);
#endif  // WEBRTC_AUDIOPROC_DEBUG_DUMP
}

// TODO(andrew): expand test to verify output.
TEST_F(ApmTest, DebugDumpFromFileHandle) {
  FILE* fid = NULL;
  EXPECT_EQ(apm_->kNullPointerError, apm_->StartDebugRecording(fid));
  const std::string filename =
      test::TempFilename(test::OutputPath(), "debug_aec");
  fid = fopen(filename.c_str(), "w");
  ASSERT_TRUE(fid);

#ifdef WEBRTC_AUDIOPROC_DEBUG_DUMP
  // Stopping without having started should be OK.
  EXPECT_EQ(apm_->kNoError, apm_->StopDebugRecording());

  EXPECT_EQ(apm_->kNoError, apm_->StartDebugRecording(fid));
  EXPECT_EQ(apm_->kNoError, apm_->AnalyzeReverseStream(revframe_));
  EXPECT_EQ(apm_->kNoError, apm_->ProcessStream(frame_));
  EXPECT_EQ(apm_->kNoError, apm_->StopDebugRecording());

  // Verify the file has been written.
  fid = fopen(filename.c_str(), "r");
  ASSERT_TRUE(fid != NULL);

  // Clean it up.
  ASSERT_EQ(0, fclose(fid));
  ASSERT_EQ(0, remove(filename.c_str()));
#else
  EXPECT_EQ(apm_->kUnsupportedFunctionError,
            apm_->StartDebugRecording(fid));
  EXPECT_EQ(apm_->kUnsupportedFunctionError, apm_->StopDebugRecording());

  ASSERT_EQ(0, fclose(fid));
#endif  // WEBRTC_AUDIOPROC_DEBUG_DUMP
}

TEST_F(ApmTest, FloatAndIntInterfacesGiveSimilarResults) {
  audioproc::OutputData ref_data;
  OpenFileAndReadMessage(ref_filename_, &ref_data);

  Config config;
  config.Set<ExperimentalAgc>(new ExperimentalAgc(false));
  scoped_ptr<AudioProcessing> fapm(AudioProcessing::Create(config));
  EnableAllComponents();
  EnableAllAPComponents(fapm.get());
  for (int i = 0; i < ref_data.test_size(); i++) {
    printf("Running test %d of %d...\n", i + 1, ref_data.test_size());

    audioproc::Test* test = ref_data.mutable_test(i);
    // TODO(ajm): Restore downmixing test cases.
    if (test->num_input_channels() != test->num_output_channels())
      continue;

    const int num_render_channels = test->num_reverse_channels();
    const int num_input_channels = test->num_input_channels();
    const int num_output_channels = test->num_output_channels();
    const int samples_per_channel = test->sample_rate() *
        AudioProcessing::kChunkSizeMs / 1000;
    const int output_length = samples_per_channel * num_output_channels;

    Init(test->sample_rate(), test->sample_rate(), test->sample_rate(),
         num_input_channels, num_output_channels, num_render_channels, true);
    Init(fapm.get());

    ChannelBuffer<int16_t> output_cb(samples_per_channel, num_input_channels);
    ChannelBuffer<int16_t> output_int16(samples_per_channel,
                                        num_input_channels);

    int analog_level = 127;
    while (ReadFrame(far_file_, revframe_, revfloat_cb_.get()) &&
           ReadFrame(near_file_, frame_, float_cb_.get())) {
      frame_->vad_activity_ = AudioFrame::kVadUnknown;

      EXPECT_NOERR(apm_->AnalyzeReverseStream(revframe_));
      EXPECT_NOERR(fapm->AnalyzeReverseStream(
          revfloat_cb_->channels(),
          samples_per_channel,
          test->sample_rate(),
          LayoutFromChannels(num_render_channels)));

      EXPECT_NOERR(apm_->set_stream_delay_ms(0));
      EXPECT_NOERR(fapm->set_stream_delay_ms(0));
      apm_->echo_cancellation()->set_stream_drift_samples(0);
      fapm->echo_cancellation()->set_stream_drift_samples(0);
      EXPECT_NOERR(apm_->gain_control()->set_stream_analog_level(analog_level));
      EXPECT_NOERR(fapm->gain_control()->set_stream_analog_level(analog_level));

      EXPECT_NOERR(apm_->ProcessStream(frame_));
      Deinterleave(frame_->data_, samples_per_channel, num_output_channels,
                   output_int16.channels());

      EXPECT_NOERR(fapm->ProcessStream(
          float_cb_->channels(),
          samples_per_channel,
          test->sample_rate(),
          LayoutFromChannels(num_input_channels),
          test->sample_rate(),
          LayoutFromChannels(num_output_channels),
          float_cb_->channels()));

      FloatToS16(float_cb_->data(), output_length, output_cb.data());
      for (int j = 0; j < num_output_channels; ++j) {
        float variance = 0;
        float snr = ComputeSNR(output_int16.channel(j), output_cb.channel(j),
                               samples_per_channel, &variance);
  #if defined(WEBRTC_AUDIOPROC_FIXED_PROFILE)
        // There are a few chunks in the fixed-point profile that give low SNR.
        // Listening confirmed the difference is acceptable.
        const float kVarianceThreshold = 150;
        const float kSNRThreshold = 10;
  #else
        const float kVarianceThreshold = 20;
        const float kSNRThreshold = 20;
  #endif
        // Skip frames with low energy.
        if (sqrt(variance) > kVarianceThreshold) {
          EXPECT_LT(kSNRThreshold, snr);
        }
      }

      analog_level = fapm->gain_control()->stream_analog_level();
      EXPECT_EQ(apm_->gain_control()->stream_analog_level(),
                fapm->gain_control()->stream_analog_level());
      EXPECT_EQ(apm_->echo_cancellation()->stream_has_echo(),
                fapm->echo_cancellation()->stream_has_echo());
      EXPECT_NEAR(apm_->noise_suppression()->speech_probability(),
                  fapm->noise_suppression()->speech_probability(),
                  0.0005);

      // Reset in case of downmixing.
      frame_->num_channels_ = test->num_input_channels();
    }
    rewind(far_file_);
    rewind(near_file_);
  }
}

// TODO(andrew): Add a test to process a few frames with different combinations
// of enabled components.

TEST_F(ApmTest, Process) {
  GOOGLE_PROTOBUF_VERIFY_VERSION;
  audioproc::OutputData ref_data;

  if (!write_ref_data) {
    OpenFileAndReadMessage(ref_filename_, &ref_data);
  } else {
    // Write the desired tests to the protobuf reference file.
    for (size_t i = 0; i < kChannelsSize; i++) {
      for (size_t j = 0; j < kChannelsSize; j++) {
        for (size_t l = 0; l < kProcessSampleRatesSize; l++) {
          audioproc::Test* test = ref_data.add_test();
          test->set_num_reverse_channels(kChannels[i]);
          test->set_num_input_channels(kChannels[j]);
          test->set_num_output_channels(kChannels[j]);
          test->set_sample_rate(kProcessSampleRates[l]);
        }
      }
    }
  }

  EnableAllComponents();

  for (int i = 0; i < ref_data.test_size(); i++) {
    printf("Running test %d of %d...\n", i + 1, ref_data.test_size());

    audioproc::Test* test = ref_data.mutable_test(i);
    // TODO(ajm): We no longer allow different input and output channels. Skip
    // these tests for now, but they should be removed from the set.
    if (test->num_input_channels() != test->num_output_channels())
      continue;

    Init(test->sample_rate(),
         test->sample_rate(),
         test->sample_rate(),
         test->num_input_channels(),
         test->num_output_channels(),
         test->num_reverse_channels(),
         true);

    int frame_count = 0;
    int has_echo_count = 0;
    int has_voice_count = 0;
    int is_saturated_count = 0;
    int analog_level = 127;
    int analog_level_average = 0;
    int max_output_average = 0;
    float ns_speech_prob_average = 0.0f;

    while (ReadFrame(far_file_, revframe_) && ReadFrame(near_file_, frame_)) {
      EXPECT_EQ(apm_->kNoError, apm_->AnalyzeReverseStream(revframe_));

      frame_->vad_activity_ = AudioFrame::kVadUnknown;

      EXPECT_EQ(apm_->kNoError, apm_->set_stream_delay_ms(0));
      apm_->echo_cancellation()->set_stream_drift_samples(0);
      EXPECT_EQ(apm_->kNoError,
          apm_->gain_control()->set_stream_analog_level(analog_level));

      EXPECT_EQ(apm_->kNoError, apm_->ProcessStream(frame_));

      // Ensure the frame was downmixed properly.
      EXPECT_EQ(test->num_output_channels(), frame_->num_channels_);

      max_output_average += MaxAudioFrame(*frame_);

      if (apm_->echo_cancellation()->stream_has_echo()) {
        has_echo_count++;
      }

      analog_level = apm_->gain_control()->stream_analog_level();
      analog_level_average += analog_level;
      if (apm_->gain_control()->stream_is_saturated()) {
        is_saturated_count++;
      }
      if (apm_->voice_detection()->stream_has_voice()) {
        has_voice_count++;
        EXPECT_EQ(AudioFrame::kVadActive, frame_->vad_activity_);
      } else {
        EXPECT_EQ(AudioFrame::kVadPassive, frame_->vad_activity_);
      }

      ns_speech_prob_average += apm_->noise_suppression()->speech_probability();

      size_t frame_size = frame_->samples_per_channel_ * frame_->num_channels_;
      size_t write_count = fwrite(frame_->data_,
                                  sizeof(int16_t),
                                  frame_size,
                                  out_file_);
      ASSERT_EQ(frame_size, write_count);

      // Reset in case of downmixing.
      frame_->num_channels_ = test->num_input_channels();
      frame_count++;
    }
    max_output_average /= frame_count;
    analog_level_average /= frame_count;
    ns_speech_prob_average /= frame_count;

#if defined(WEBRTC_AUDIOPROC_FLOAT_PROFILE)
    EchoCancellation::Metrics echo_metrics;
    EXPECT_EQ(apm_->kNoError,
              apm_->echo_cancellation()->GetMetrics(&echo_metrics));
    int median = 0;
    int std = 0;
    EXPECT_EQ(apm_->kNoError,
              apm_->echo_cancellation()->GetDelayMetrics(&median, &std));

    int rms_level = apm_->level_estimator()->RMS();
    EXPECT_LE(0, rms_level);
    EXPECT_GE(127, rms_level);
#endif

    if (!write_ref_data) {
      const int kIntNear = 1;
      // When running the test on a N7 we get a {2, 6} difference of
      // |has_voice_count| and |max_output_average| is up to 18 higher.
      // All numbers being consistently higher on N7 compare to ref_data.
      // TODO(bjornv): If we start getting more of these offsets on Android we
      // should consider a different approach. Either using one slack for all,
      // or generate a separate android reference.
#if defined(WEBRTC_ANDROID)
      const int kHasVoiceCountOffset = 3;
      const int kHasVoiceCountNear = 3;
      const int kMaxOutputAverageOffset = 9;
      const int kMaxOutputAverageNear = 9;
#else
      const int kHasVoiceCountOffset = 0;
      const int kHasVoiceCountNear = kIntNear;
      const int kMaxOutputAverageOffset = 0;
      const int kMaxOutputAverageNear = kIntNear;
#endif
      EXPECT_NEAR(test->has_echo_count(), has_echo_count, kIntNear);
      EXPECT_NEAR(test->has_voice_count(),
                  has_voice_count - kHasVoiceCountOffset,
                  kHasVoiceCountNear);
      EXPECT_NEAR(test->is_saturated_count(), is_saturated_count, kIntNear);

      EXPECT_NEAR(test->analog_level_average(), analog_level_average, kIntNear);
      EXPECT_NEAR(test->max_output_average(),
                  max_output_average - kMaxOutputAverageOffset,
                  kMaxOutputAverageNear);

#if defined(WEBRTC_AUDIOPROC_FLOAT_PROFILE)
      audioproc::Test::EchoMetrics reference = test->echo_metrics();
      TestStats(echo_metrics.residual_echo_return_loss,
                reference.residual_echo_return_loss());
      TestStats(echo_metrics.echo_return_loss,
                reference.echo_return_loss());
      TestStats(echo_metrics.echo_return_loss_enhancement,
                reference.echo_return_loss_enhancement());
      TestStats(echo_metrics.a_nlp,
                reference.a_nlp());

      const double kFloatNear = 0.0005;
      audioproc::Test::DelayMetrics reference_delay = test->delay_metrics();
      EXPECT_NEAR(reference_delay.median(), median, kIntNear);
      EXPECT_NEAR(reference_delay.std(), std, kIntNear);

      EXPECT_NEAR(test->rms_level(), rms_level, kIntNear);

      EXPECT_NEAR(test->ns_speech_probability_average(),
                  ns_speech_prob_average,
                  kFloatNear);
#endif
    } else {
      test->set_has_echo_count(has_echo_count);
      test->set_has_voice_count(has_voice_count);
      test->set_is_saturated_count(is_saturated_count);

      test->set_analog_level_average(analog_level_average);
      test->set_max_output_average(max_output_average);

#if defined(WEBRTC_AUDIOPROC_FLOAT_PROFILE)
      audioproc::Test::EchoMetrics* message = test->mutable_echo_metrics();
      WriteStatsMessage(echo_metrics.residual_echo_return_loss,
                        message->mutable_residual_echo_return_loss());
      WriteStatsMessage(echo_metrics.echo_return_loss,
                        message->mutable_echo_return_loss());
      WriteStatsMessage(echo_metrics.echo_return_loss_enhancement,
                        message->mutable_echo_return_loss_enhancement());
      WriteStatsMessage(echo_metrics.a_nlp,
                        message->mutable_a_nlp());

      audioproc::Test::DelayMetrics* message_delay =
          test->mutable_delay_metrics();
      message_delay->set_median(median);
      message_delay->set_std(std);

      test->set_rms_level(rms_level);

      EXPECT_LE(0.0f, ns_speech_prob_average);
      EXPECT_GE(1.0f, ns_speech_prob_average);
      test->set_ns_speech_probability_average(ns_speech_prob_average);
#endif
    }

    rewind(far_file_);
    rewind(near_file_);
  }

  if (write_ref_data) {
    OpenFileAndWriteMessage(ref_filename_, ref_data);
  }
}

TEST_F(ApmTest, NoErrorsWithKeyboardChannel) {
  struct ChannelFormat {
    AudioProcessing::ChannelLayout in_layout;
    AudioProcessing::ChannelLayout out_layout;
  };
  ChannelFormat cf[] = {
    {AudioProcessing::kMonoAndKeyboard, AudioProcessing::kMono},
    {AudioProcessing::kStereoAndKeyboard, AudioProcessing::kMono},
    {AudioProcessing::kStereoAndKeyboard, AudioProcessing::kStereo},
  };
  size_t channel_format_size = sizeof(cf) / sizeof(*cf);

  scoped_ptr<AudioProcessing> ap(AudioProcessing::Create());
  // Enable one component just to ensure some processing takes place.
  ap->noise_suppression()->Enable(true);
  for (size_t i = 0; i < channel_format_size; ++i) {
    const int in_rate = 44100;
    const int out_rate = 48000;
    ChannelBuffer<float> in_cb(SamplesFromRate(in_rate),
                               TotalChannelsFromLayout(cf[i].in_layout));
    ChannelBuffer<float> out_cb(SamplesFromRate(out_rate),
                                ChannelsFromLayout(cf[i].out_layout));

    // Run over a few chunks.
    for (int j = 0; j < 10; ++j) {
      EXPECT_NOERR(ap->ProcessStream(
          in_cb.channels(),
          in_cb.samples_per_channel(),
          in_rate,
          cf[i].in_layout,
          out_rate,
          cf[i].out_layout,
          out_cb.channels()));
    }
  }
}

// Compares the reference and test arrays over a region around the expected
// delay. Finds the highest SNR in that region and adds the variance and squared
// error results to the supplied accumulators.
void UpdateBestSNR(const float* ref,
                   const float* test,
                   int length,
                   int expected_delay,
                   double* variance_acc,
                   double* sq_error_acc) {
  double best_snr = std::numeric_limits<double>::min();
  double best_variance = 0;
  double best_sq_error = 0;
  // Search over a region of eight samples around the expected delay.
  for (int delay = std::max(expected_delay - 4, 0); delay <= expected_delay + 4;
       ++delay) {
    double sq_error = 0;
    double variance = 0;
    for (int i = 0; i < length - delay; ++i) {
      double error = test[i + delay] - ref[i];
      sq_error += error * error;
      variance += ref[i] * ref[i];
    }

    if (sq_error == 0) {
      *variance_acc += variance;
      return;
    }
    double snr = variance / sq_error;
    if (snr > best_snr) {
      best_snr = snr;
      best_variance = variance;
      best_sq_error = sq_error;
    }
  }

  *variance_acc += best_variance;
  *sq_error_acc += best_sq_error;
}

// Used to test a multitude of sample rate and channel combinations. It works
// by first producing a set of reference files (in SetUpTestCase) that are
// assumed to be correct, as the used parameters are verified by other tests
// in this collection. Primarily the reference files are all produced at
// "native" rates which do not involve any resampling.

// Each test pass produces an output file with a particular format. The output
// is matched against the reference file closest to its internal processing
// format. If necessary the output is resampled back to its process format.
// Due to the resampling distortion, we don't expect identical results, but
// enforce SNR thresholds which vary depending on the format. 0 is a special
// case SNR which corresponds to inf, or zero error.
typedef std::tr1::tuple<int, int, int, double> AudioProcessingTestData;
class AudioProcessingTest
    : public testing::TestWithParam<AudioProcessingTestData> {
 public:
  AudioProcessingTest()
      : input_rate_(std::tr1::get<0>(GetParam())),
        output_rate_(std::tr1::get<1>(GetParam())),
        reverse_rate_(std::tr1::get<2>(GetParam())),
        expected_snr_(std::tr1::get<3>(GetParam())) {}

  virtual ~AudioProcessingTest() {}

  static void SetUpTestCase() {
    // Create all needed output reference files.
    const int kNativeRates[] = {8000, 16000, 32000};
    const size_t kNativeRatesSize =
        sizeof(kNativeRates) / sizeof(*kNativeRates);
    const int kNumChannels[] = {1, 2};
    const size_t kNumChannelsSize =
        sizeof(kNumChannels) / sizeof(*kNumChannels);
    for (size_t i = 0; i < kNativeRatesSize; ++i) {
      for (size_t j = 0; j < kNumChannelsSize; ++j) {
        for (size_t k = 0; k < kNumChannelsSize; ++k) {
          // The reference files always have matching input and output channels.
          ProcessFormat(kNativeRates[i],
                        kNativeRates[i],
                        kNativeRates[i],
                        kNumChannels[j],
                        kNumChannels[j],
                        kNumChannels[k],
                        "ref");
        }
      }
    }
  }

  // Runs a process pass on files with the given parameters and dumps the output
  // to a file specified with |output_file_prefix|.
  static void ProcessFormat(int input_rate,
                            int output_rate,
                            int reverse_rate,
                            int num_input_channels,
                            int num_output_channels,
                            int num_reverse_channels,
                            std::string output_file_prefix) {
    Config config;
    config.Set<ExperimentalAgc>(new ExperimentalAgc(false));
    scoped_ptr<AudioProcessing> ap(AudioProcessing::Create(config));
    EnableAllAPComponents(ap.get());
    ap->Initialize(input_rate,
                   output_rate,
                   reverse_rate,
                   LayoutFromChannels(num_input_channels),
                   LayoutFromChannels(num_output_channels),
                   LayoutFromChannels(num_reverse_channels));

    FILE* far_file = fopen(ResourceFilePath("far", reverse_rate).c_str(), "rb");
    FILE* near_file = fopen(ResourceFilePath("near", input_rate).c_str(), "rb");
    FILE* out_file = fopen(OutputFilePath(output_file_prefix,
                                          input_rate,
                                          output_rate,
                                          reverse_rate,
                                          num_input_channels,
                                          num_output_channels,
                                          num_reverse_channels).c_str(), "wb");
    ASSERT_TRUE(far_file != NULL);
    ASSERT_TRUE(near_file != NULL);
    ASSERT_TRUE(out_file != NULL);

    ChannelBuffer<float> fwd_cb(SamplesFromRate(input_rate),
                                num_input_channels);
    ChannelBuffer<float> rev_cb(SamplesFromRate(reverse_rate),
                                num_reverse_channels);
    ChannelBuffer<float> out_cb(SamplesFromRate(output_rate),
                                num_output_channels);

    // Temporary buffers.
    const int max_length =
        2 * std::max(out_cb.samples_per_channel(),
                     std::max(fwd_cb.samples_per_channel(),
                              rev_cb.samples_per_channel()));
    scoped_ptr<float[]> float_data(new float[max_length]);
    scoped_ptr<int16_t[]> int_data(new int16_t[max_length]);

    int analog_level = 127;
    while (ReadChunk(far_file, int_data.get(), float_data.get(), &rev_cb) &&
           ReadChunk(near_file, int_data.get(), float_data.get(), &fwd_cb)) {
      EXPECT_NOERR(ap->AnalyzeReverseStream(
          rev_cb.channels(),
          rev_cb.samples_per_channel(),
          reverse_rate,
          LayoutFromChannels(num_reverse_channels)));

      EXPECT_NOERR(ap->set_stream_delay_ms(0));
      ap->echo_cancellation()->set_stream_drift_samples(0);
      EXPECT_NOERR(ap->gain_control()->set_stream_analog_level(analog_level));

      EXPECT_NOERR(ap->ProcessStream(
          fwd_cb.channels(),
          fwd_cb.samples_per_channel(),
          input_rate,
          LayoutFromChannels(num_input_channels),
          output_rate,
          LayoutFromChannels(num_output_channels),
          out_cb.channels()));

      Interleave(out_cb.channels(),
                 out_cb.samples_per_channel(),
                 out_cb.num_channels(),
                 float_data.get());
      // Dump output to file.
      ASSERT_EQ(static_cast<size_t>(out_cb.length()),
                fwrite(float_data.get(), sizeof(float_data[0]),
                       out_cb.length(), out_file));

      analog_level = ap->gain_control()->stream_analog_level();
    }
    fclose(far_file);
    fclose(near_file);
    fclose(out_file);
  }

 protected:
  int input_rate_;
  int output_rate_;
  int reverse_rate_;
  double expected_snr_;
};

TEST_P(AudioProcessingTest, Formats) {
  struct ChannelFormat {
    int num_input;
    int num_output;
    int num_reverse;
  };
  ChannelFormat cf[] = {
    {1, 1, 1},
    {1, 1, 2},
    {2, 1, 1},
    {2, 1, 2},
    {2, 2, 1},
    {2, 2, 2},
  };
  size_t channel_format_size = sizeof(cf) / sizeof(*cf);

  for (size_t i = 0; i < channel_format_size; ++i) {
    ProcessFormat(input_rate_,
                  output_rate_,
                  reverse_rate_,
                  cf[i].num_input,
                  cf[i].num_output,
                  cf[i].num_reverse,
                  "out");
    int min_ref_rate = std::min(input_rate_, output_rate_);
    int ref_rate;
    if (min_ref_rate > 16000) {
      ref_rate = 32000;
    } else if (min_ref_rate > 8000) {
      ref_rate = 16000;
    } else {
      ref_rate = 8000;
    }
#ifdef WEBRTC_AUDIOPROC_FIXED_PROFILE
    ref_rate = std::min(ref_rate, 16000);
#endif

    FILE* out_file = fopen(OutputFilePath("out",
                                          input_rate_,
                                          output_rate_,
                                          reverse_rate_,
                                          cf[i].num_input,
                                          cf[i].num_output,
                                          cf[i].num_reverse).c_str(), "rb");
    // The reference files always have matching input and output channels.
    FILE* ref_file = fopen(OutputFilePath("ref",
                                          ref_rate,
                                          ref_rate,
                                          ref_rate,
                                          cf[i].num_output,
                                          cf[i].num_output,
                                          cf[i].num_reverse).c_str(), "rb");
    ASSERT_TRUE(out_file != NULL);
    ASSERT_TRUE(ref_file != NULL);

    const int ref_length = SamplesFromRate(ref_rate) * cf[i].num_output;
    const int out_length = SamplesFromRate(output_rate_) * cf[i].num_output;
    // Data from the reference file.
    scoped_ptr<float[]> ref_data(new float[ref_length]);
    // Data from the output file.
    scoped_ptr<float[]> out_data(new float[out_length]);
    // Data from the resampled output, in case the reference and output rates
    // don't match.
    scoped_ptr<float[]> cmp_data(new float[ref_length]);

    PushResampler<float> resampler;
    resampler.InitializeIfNeeded(output_rate_, ref_rate, cf[i].num_output);

    // Compute the resampling delay of the output relative to the reference,
    // to find the region over which we should search for the best SNR.
    float expected_delay_sec = 0;
    if (input_rate_ != ref_rate) {
      // Input resampling delay.
      expected_delay_sec +=
          PushSincResampler::AlgorithmicDelaySeconds(input_rate_);
    }
    if (output_rate_ != ref_rate) {
      // Output resampling delay.
      expected_delay_sec +=
          PushSincResampler::AlgorithmicDelaySeconds(ref_rate);
      // Delay of converting the output back to its processing rate for testing.
      expected_delay_sec +=
          PushSincResampler::AlgorithmicDelaySeconds(output_rate_);
    }
    int expected_delay = floor(expected_delay_sec * ref_rate + 0.5f) *
                         cf[i].num_output;

    double variance = 0;
    double sq_error = 0;
    while (fread(out_data.get(), sizeof(out_data[0]), out_length, out_file) &&
           fread(ref_data.get(), sizeof(ref_data[0]), ref_length, ref_file)) {
      float* out_ptr = out_data.get();
      if (output_rate_ != ref_rate) {
        // Resample the output back to its internal processing rate if necssary.
        ASSERT_EQ(ref_length, resampler.Resample(out_ptr,
                                                 out_length,
                                                 cmp_data.get(),
                                                 ref_length));
        out_ptr = cmp_data.get();
      }

      // Update the |sq_error| and |variance| accumulators with the highest SNR
      // of reference vs output.
      UpdateBestSNR(ref_data.get(),
                    out_ptr,
                    ref_length,
                    expected_delay,
                    &variance,
                    &sq_error);
    }

    std::cout << "(" << input_rate_ << ", "
                     << output_rate_ << ", "
                     << reverse_rate_ << ", "
                     << cf[i].num_input << ", "
                     << cf[i].num_output << ", "
                     << cf[i].num_reverse << "): ";
    if (sq_error > 0) {
      double snr = 10 * log10(variance / sq_error);
      EXPECT_GE(snr, expected_snr_);
      EXPECT_NE(0, expected_snr_);
      std::cout << "SNR=" << snr << " dB" << std::endl;
    } else {
      EXPECT_EQ(expected_snr_, 0);
      std::cout << "SNR=" << "inf dB" << std::endl;
    }

    fclose(out_file);
    fclose(ref_file);
  }
}

#if defined(WEBRTC_AUDIOPROC_FLOAT_PROFILE)
INSTANTIATE_TEST_CASE_P(
    CommonFormats, AudioProcessingTest, testing::Values(
        std::tr1::make_tuple(48000, 48000, 48000, 20),
        std::tr1::make_tuple(48000, 48000, 32000, 20),
        std::tr1::make_tuple(48000, 48000, 16000, 20),
        std::tr1::make_tuple(48000, 44100, 48000, 15),
        std::tr1::make_tuple(48000, 44100, 32000, 15),
        std::tr1::make_tuple(48000, 44100, 16000, 15),
        std::tr1::make_tuple(48000, 32000, 48000, 20),
        std::tr1::make_tuple(48000, 32000, 32000, 20),
        std::tr1::make_tuple(48000, 32000, 16000, 20),
        std::tr1::make_tuple(48000, 16000, 48000, 20),
        std::tr1::make_tuple(48000, 16000, 32000, 20),
        std::tr1::make_tuple(48000, 16000, 16000, 20),

        std::tr1::make_tuple(44100, 48000, 48000, 20),
        std::tr1::make_tuple(44100, 48000, 32000, 20),
        std::tr1::make_tuple(44100, 48000, 16000, 20),
        std::tr1::make_tuple(44100, 44100, 48000, 15),
        std::tr1::make_tuple(44100, 44100, 32000, 15),
        std::tr1::make_tuple(44100, 44100, 16000, 15),
        std::tr1::make_tuple(44100, 32000, 48000, 20),
        std::tr1::make_tuple(44100, 32000, 32000, 20),
        std::tr1::make_tuple(44100, 32000, 16000, 20),
        std::tr1::make_tuple(44100, 16000, 48000, 20),
        std::tr1::make_tuple(44100, 16000, 32000, 20),
        std::tr1::make_tuple(44100, 16000, 16000, 20),

        std::tr1::make_tuple(32000, 48000, 48000, 25),
        std::tr1::make_tuple(32000, 48000, 32000, 25),
        std::tr1::make_tuple(32000, 48000, 16000, 25),
        std::tr1::make_tuple(32000, 44100, 48000, 20),
        std::tr1::make_tuple(32000, 44100, 32000, 20),
        std::tr1::make_tuple(32000, 44100, 16000, 20),
        std::tr1::make_tuple(32000, 32000, 48000, 30),
        std::tr1::make_tuple(32000, 32000, 32000, 0),
        std::tr1::make_tuple(32000, 32000, 16000, 30),
        std::tr1::make_tuple(32000, 16000, 48000, 20),
        std::tr1::make_tuple(32000, 16000, 32000, 20),
        std::tr1::make_tuple(32000, 16000, 16000, 20),

        std::tr1::make_tuple(16000, 48000, 48000, 25),
        std::tr1::make_tuple(16000, 48000, 32000, 25),
        std::tr1::make_tuple(16000, 48000, 16000, 25),
        std::tr1::make_tuple(16000, 44100, 48000, 15),
        std::tr1::make_tuple(16000, 44100, 32000, 15),
        std::tr1::make_tuple(16000, 44100, 16000, 15),
        std::tr1::make_tuple(16000, 32000, 48000, 25),
        std::tr1::make_tuple(16000, 32000, 32000, 25),
        std::tr1::make_tuple(16000, 32000, 16000, 25),
        std::tr1::make_tuple(16000, 16000, 48000, 30),
        std::tr1::make_tuple(16000, 16000, 32000, 30),
        std::tr1::make_tuple(16000, 16000, 16000, 0)));

#elif defined(WEBRTC_AUDIOPROC_FIXED_PROFILE)
INSTANTIATE_TEST_CASE_P(
    CommonFormats, AudioProcessingTest, testing::Values(
        std::tr1::make_tuple(48000, 48000, 48000, 20),
        std::tr1::make_tuple(48000, 48000, 32000, 20),
        std::tr1::make_tuple(48000, 48000, 16000, 20),
        std::tr1::make_tuple(48000, 44100, 48000, 15),
        std::tr1::make_tuple(48000, 44100, 32000, 15),
        std::tr1::make_tuple(48000, 44100, 16000, 15),
        std::tr1::make_tuple(48000, 32000, 48000, 20),
        std::tr1::make_tuple(48000, 32000, 32000, 20),
        std::tr1::make_tuple(48000, 32000, 16000, 20),
        std::tr1::make_tuple(48000, 16000, 48000, 20),
        std::tr1::make_tuple(48000, 16000, 32000, 20),
        std::tr1::make_tuple(48000, 16000, 16000, 20),

        std::tr1::make_tuple(44100, 48000, 48000, 19),
        std::tr1::make_tuple(44100, 48000, 32000, 19),
        std::tr1::make_tuple(44100, 48000, 16000, 19),
        std::tr1::make_tuple(44100, 44100, 48000, 15),
        std::tr1::make_tuple(44100, 44100, 32000, 15),
        std::tr1::make_tuple(44100, 44100, 16000, 15),
        std::tr1::make_tuple(44100, 32000, 48000, 19),
        std::tr1::make_tuple(44100, 32000, 32000, 19),
        std::tr1::make_tuple(44100, 32000, 16000, 19),
        std::tr1::make_tuple(44100, 16000, 48000, 19),
        std::tr1::make_tuple(44100, 16000, 32000, 19),
        std::tr1::make_tuple(44100, 16000, 16000, 19),

        std::tr1::make_tuple(32000, 48000, 48000, 19),
        std::tr1::make_tuple(32000, 48000, 32000, 19),
        std::tr1::make_tuple(32000, 48000, 16000, 19),
        std::tr1::make_tuple(32000, 44100, 48000, 15),
        std::tr1::make_tuple(32000, 44100, 32000, 15),
        std::tr1::make_tuple(32000, 44100, 16000, 15),
        std::tr1::make_tuple(32000, 32000, 48000, 19),
        std::tr1::make_tuple(32000, 32000, 32000, 19),
        std::tr1::make_tuple(32000, 32000, 16000, 19),
        std::tr1::make_tuple(32000, 16000, 48000, 19),
        std::tr1::make_tuple(32000, 16000, 32000, 19),
        std::tr1::make_tuple(32000, 16000, 16000, 19),

        std::tr1::make_tuple(16000, 48000, 48000, 25),
        std::tr1::make_tuple(16000, 48000, 32000, 25),
        std::tr1::make_tuple(16000, 48000, 16000, 25),
        std::tr1::make_tuple(16000, 44100, 48000, 15),
        std::tr1::make_tuple(16000, 44100, 32000, 15),
        std::tr1::make_tuple(16000, 44100, 16000, 15),
        std::tr1::make_tuple(16000, 32000, 48000, 25),
        std::tr1::make_tuple(16000, 32000, 32000, 25),
        std::tr1::make_tuple(16000, 32000, 16000, 25),
        std::tr1::make_tuple(16000, 16000, 48000, 30),
        std::tr1::make_tuple(16000, 16000, 32000, 30),
        std::tr1::make_tuple(16000, 16000, 16000, 0)));
#endif

// TODO(henrike): re-implement functionality lost when removing the old main
//                function. See
//                https://code.google.com/p/webrtc/issues/detail?id=1981

}  // namespace
}  // namespace webrtc