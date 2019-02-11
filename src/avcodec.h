#ifndef avcodec_h_
#define avcodec_h_

#include <atomic>
#include <mutex>
#include <unordered_map>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/audio_fifo.h>
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
        last_pts(0),
        frame(nullptr),
        t(0),
        tincr(0),
        tincr2(0),
        sws_ctx(nullptr),
        codec(nullptr),
        fifo(nullptr) {}
  ~OutputStream() {
    avcodec_close(enc);
    avcodec_free_context(&enc);
    if (frame->data[0]) { av_freep(frame->data); }
    av_frame_free(&frame);
    sws_freeContext(sws_ctx);
    av_audio_fifo_free(fifo);
  }
  AVStream *st;
  AVCodecContext *enc;
  AVCodec *codec;
  AVAudioFifo *fifo;

  /* pts of the next frame that will be generated */
  std::atomic<int64_t> next_pts, last_pts;

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
  std::string video_minrate;
  std::string video_maxrate;
  std::string video_tune;
  bool audio_out;
  int audio_idx;

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
  void write_packet(AVPacket *pkt, OutputStream &stream);
  void open_audio_output();
  void open_audio_input(int id);

  void audio_loop();

  int add_samples_to_fifo(AVAudioFifo *fifo, uint8_t **converted_input_samples,
                          const int frame_size);
  int read_decode_convert_and_store(AVAudioFifo *fifo,
                                    AVFormatContext *input_format_context,
                                    AVCodecContext *input_codec_context,
                                    AVCodecContext *output_codec_context,
                                    SwrContext *resampler_context);
  int init_input_frame(AVFrame **frame);
  int decode_audio_frame(AVFrame *frame, AVFormatContext *input_format_context,
                         AVCodecContext *input_codec_context,
                         int *data_present);
  int convert_samples(const uint8_t **input_data, uint8_t **converted_data,
                      const int frame_size, SwrContext *resample_context);
  int init_converted_samples(uint8_t ***converted_input_samples,
                             AVCodecContext *output_codec_context,
                             int frame_size);
  void init_packet(AVPacket *packet);
  int init_fifo(AVAudioFifo **fifo, AVCodecContext *output_codec_context);
  int load_encode_and_write(AVAudioFifo *fifo,
                            AVFormatContext *output_format_context,
                            AVCodecContext *output_codec_context);
  int init_output_frame(AVFrame **frame, AVCodecContext *output_codec_context,
                        int frame_size);
  int encode_audio_frame(AVFrame *frame, AVFormatContext *output_format_context,
                         AVCodecContext *output_codec_context,
                         int *data_present);

 public:
  AvCodec(double width, double height, double fps, int bitrate,
          const std::string &codec_profile, const std::string &server,
          const std::string &audio_format, const bool audio_out,
          const std::string &video_preset,
          const std::string &video_keyframe_group_size, int audio_idx,
          const std::string &video_bufsize, const std::string &video_minrate,
          const std::string &video_maxrate, const std::string &video_tune);
  ~AvCodec();

  void init_audio();
  void del_audio(int id);

  void sws_scale_video(const uint8_t *const image_data[], const int stride[],
                       int image_rows);

  void write_frames();
  void rescale_video_frame();
};

#endif /* avcodec_h_ */
