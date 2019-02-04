#ifndef avcodec_h_
#define avcodec_h_

#include <atomic>
#include <mutex>
#include <unordered_map>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/imgutils.h>
#include <libswresample/swresample.h>
#include <libswscale/swscale.h>
}

// a wrapper around a single output AVStream
class OutputStream {
 public:
  OutputStream()
      : st(nullptr),
        enc(nullptr),
        next_pts(0),
        frame(nullptr),
        t(0),
        tincr(0),
        tincr2(0),
        sws_ctx(nullptr),
        codec(nullptr) {}
  ~OutputStream() {
    avcodec_close(enc);
    avcodec_free_context(&enc);
    if (frame->data[0]) { av_freep(frame->data); }
    av_frame_free(&frame);
    sws_freeContext(sws_ctx);
  }
  AVStream *st;
  AVCodecContext *enc;
  AVCodec *codec;

  /* pts of the next frame that will be generated */
  int64_t next_pts;

  AVFrame *frame;

  float t, tincr, tincr2;

  struct SwsContext *sws_ctx;
};

class InputStream {
 public:
  InputStream()
      : st(nullptr),
        dec(nullptr),
        codec(nullptr),
        frame(nullptr),
        id(0),
        swr_ctx(nullptr),
        ifmt_ctx(nullptr) {}
  ~InputStream() {
    avcodec_close(dec);
    avcodec_free_context(&dec);
    av_frame_free(&frame);
    swr_free(&swr_ctx);
    avformat_free_context(ifmt_ctx);
  }
  AVStream *st;
  AVCodecContext *dec;
  AVCodec *codec;
  AVFrame *frame;
  struct SwrContext *swr_ctx;
  AVFormatContext *ifmt_ctx;
  int id;
};

class AvCodec {
  AVFormatContext *ofmt_ctx;

  OutputStream video_st;
  OutputStream audio_st;
  std::unordered_map<int, std::shared_ptr<InputStream>> audio_input_st;
  std::atomic<int> selected_audio_id;
  std::thread audio_thread;
  std::mutex writing_mutex;

  std::string audio_format;
  std::string video_preset;
  std::string video_keyframe_group_size;
  std::string video_bufsize;
  std::string video_tune;
  bool audio_out;
  int audio_idx_start;

  void initialize_avformat_context(const char *format_name);
  void initialize_io_context(const char *output);
  std::shared_ptr<InputStream> initialize_audio_input_context(
      const std::string &format, const std::string &devname);
  void set_video_codec_params(double width, double height, int fps,
                              int bitrate);
  void set_audio_codec_params(double width, double height, int fps,
                              int bitrate);
  void initialize_video_codec_stream(const std::string &codec_profile);
  void initialize_audio_codec_stream(const std::string &codec_profile);
  SwsContext *initialize_sample_scaler(double width, double height);
  AVFrame *allocate_frame_buffer(double width, double height);
  void write_frame(OutputStream &stream);
  AVFrame *alloc_audio_frame(enum AVSampleFormat sample_fmt,
                             uint64_t channel_layout, int sample_rate,
                             int nb_samples);
  void open_audio_output();
  void open_audio_input(int id);
  void get_audio_frame(int id);
  void write_audio_frame(int id);
  void free(OutputStream &st);
  void log_packet(const AVFormatContext *fmt_ctx, const AVPacket *pkt);

  void audio_loop();

 public:
  AvCodec(double width, double height, double fps, int bitrate,
          const std::string &codec_profile, const std::string &server,
          const std::string &audio_format, const bool audio_out,
          const std::string &video_preset,
          const std::string &video_keyframe_group_size, int audio_idx_start,
          const std::string &video_bufsize, const std::string &video_tune);
  ~AvCodec();

  void init_audio(int id);
  void set_audio(int id) { selected_audio_id = id; }
  void del_audio(int id);

  void sws_scale_video(const uint8_t *const image_data[], const int stride[],
                       int image_rows);

  void write_frames();
  void rescale_video_frame();
};

#endif /* avcodec_h_ */
