#include <iostream>
#include <sstream>
#include <thread>
#include <vector>

extern "C" {
#include <libavdevice/avdevice.h>
#include <libavutil/avassert.h>
#include <libavutil/error.h>
#include <libavutil/opt.h>
#include <libavutil/timestamp.h>
}

#include "avcodec.h"

static char _av_err[AV_ERROR_MAX_STRING_SIZE] = "\0";

const char *_av_err2str(int errnum) {
  _av_err[0] = '\0';
  av_make_error_string(_av_err, AV_ERROR_MAX_STRING_SIZE, errnum);
  return _av_err;
}

AvCodec::AvCodec(double width, double height, double fps, int bitrate,
                 const std::string &codec_profile, const std::string &server,
                 const std::string &audio_format, const bool audio_out,
                 const std::string &video_preset,
                 const std::string &video_keyframe_group_size,
                 int audio_idx_start, const std::string &video_bufsize,
                 const std::string &video_tune)
    : ofmt_ctx(nullptr),
      selected_audio_id(0),
      audio_format(audio_format),
      audio_out(audio_out),
      video_preset(video_preset),
      video_keyframe_group_size(video_keyframe_group_size),
      audio_idx_start(audio_idx_start),
      video_bufsize(video_bufsize),
      video_tune(video_tune) {
#if LIBAVCODEC_VERSION_INT < AV_VERSION_INT(58, 9, 100)
  av_register_all();
#endif
  avformat_network_init();

  const char *output = server.c_str();

  initialize_avformat_context("flv");
  initialize_io_context(output);

  video_st.codec = avcodec_find_encoder(AV_CODEC_ID_H264);
  video_st.st = avformat_new_stream(ofmt_ctx, video_st.codec);
  video_st.enc = avcodec_alloc_context3(video_st.codec);
  set_video_codec_params(width, height, fps, bitrate);
  initialize_video_codec_stream(codec_profile);
  video_st.st->codecpar->extradata = video_st.enc->extradata;
  video_st.st->codecpar->extradata_size = video_st.enc->extradata_size;

  av_dump_format(ofmt_ctx, 0, output, 1);

  video_st.sws_ctx = initialize_sample_scaler(width, height);
  video_st.frame = allocate_frame_buffer(width, height);

  int ret = avformat_write_header(ofmt_ctx, nullptr);
  if (ret < 0) {
    fprintf(stderr, "Could not write header (error '%s')\n", _av_err2str(ret));
    fflush(stderr);
    exit(1);
  }

  if (audio_out) {
    audio_st.codec = avcodec_find_encoder(AV_CODEC_ID_AAC);
    audio_st.st = avformat_new_stream(ofmt_ctx, audio_st.codec);
    audio_st.enc = avcodec_alloc_context3(audio_st.codec);

    set_audio_codec_params(width, height, fps, bitrate);
    initialize_audio_codec_stream(codec_profile);

    open_audio_output();

    init_audio(0);

    audio_thread = std::thread([this] { audio_loop(); });
  }
}

AvCodec::~AvCodec() {
  printf("AvCodec::~AvCodec\n");
  selected_audio_id = -2;
  av_write_trailer(ofmt_ctx);

  avio_close(ofmt_ctx->pb);
  avformat_free_context(ofmt_ctx);
  audio_thread.join();
}

void AvCodec::initialize_avformat_context(const char *format_name) {
  int ret =
      avformat_alloc_output_context2(&ofmt_ctx, nullptr, format_name, nullptr);
  if (ret < 0) {
    fprintf(stderr, "Could not allocate output format context (error '%s')\n",
            _av_err2str(ret));
    fflush(stderr);
    exit(1);
  }
}

void AvCodec::initialize_io_context(const char *output) {
  if (!(ofmt_ctx->oformat->flags & AVFMT_NOFILE)) {
    AVDictionary *options = nullptr;
    av_dict_set(&options, "reconnect", "1", 0);
    av_dict_set(&options, "reconnect_at_eof", "1", 0);
    av_dict_set(&options, "reconnect_streamed", "1", 0);
    av_dict_set(&options, "reconnect_delay_max", "2", 0);
    int ret =
        avio_open2(&ofmt_ctx->pb, output, AVIO_FLAG_WRITE, nullptr, &options);
    if (ret < 0) {
      fprintf(stderr, "Could not open output IO context (error '%s')\n",
              _av_err2str(ret));
      fflush(stderr);
      exit(1);
    }
  }
}

void AvCodec::init_audio(int id) {
  if (!audio_out) { return; }
  std::ostringstream ss;
  ss << ":" << id + audio_idx_start;
  // ss << "hw:" << id + audio_idx_start << ",0";
  std::string devname(ss.str());
  audio_input_st[id] = initialize_audio_input_context(audio_format, devname);
  open_audio_input(id);
}

void AvCodec::del_audio(int id) { audio_input_st.erase(id); }

std::shared_ptr<InputStream> AvCodec::initialize_audio_input_context(
    const std::string &format, const std::string &devname) {
  int stream_found;
  std::shared_ptr<InputStream> st = std::make_shared<InputStream>();

  avdevice_register_all();

  AVInputFormat *av_in_fmt = av_find_input_format(format.c_str());
  int ret =
      avformat_open_input(&st->ifmt_ctx, devname.c_str(), av_in_fmt, NULL);

  if (ret != 0) {
    fprintf(stderr, "Error opening audio device %s (error '%s')\n",
            devname.c_str(), _av_err2str(ret));
    fflush(stderr);
    exit(1);
  }

  ret = avformat_find_stream_info(st->ifmt_ctx, NULL);
  if (ret != 0) {
    avformat_close_input(&st->ifmt_ctx);
    fprintf(stderr, "Error finding stream info (error '%s')\n",
            _av_err2str(ret));
    fflush(stderr);
    exit(1);
  }

  // Find the audio stream
  stream_found = av_find_best_stream(st->ifmt_ctx, AVMEDIA_TYPE_AUDIO, -1, -1,
                                     &st->codec, 0);

  if (stream_found < 0) {
    avformat_close_input(&st->ifmt_ctx);
    exit(1);
  }

  st->codec = avcodec_find_decoder(
      st->ifmt_ctx->streams[stream_found]->codecpar->codec_id);

  if (st->codec == NULL) {
    std::cerr << "ERROR - Decoder not found. The codec is not supported."
              << std::endl;
    avformat_close_input(&st->ifmt_ctx);
    exit(1);
  }

  st->dec = avcodec_alloc_context3(st->codec);
  if (st->dec == NULL) {
    avformat_close_input(&st->ifmt_ctx);
    std::cerr << "ERROR - Could not allocate a decoding context." << std::endl;
    exit(1);
  }

  /** initialize the stream parameters with demuxer information */
  ret = avcodec_parameters_to_context(
      st->dec, st->ifmt_ctx->streams[stream_found]->codecpar);
  if (ret != 0) {
    fprintf(stderr, "Error initializing stream parameters (error '%s')\n",
            _av_err2str(ret));
    fflush(stderr);
    exit(1);
  }

  ret = avcodec_open2(st->dec, st->dec->codec, NULL);
  if (ret != 0) {
    avcodec_close(st->dec);
    avcodec_free_context(&st->dec);
    avformat_close_input(&st->ifmt_ctx);
    fprintf(stderr, "Error opening context with decoder (error '%s')\n",
            _av_err2str(ret));
    fflush(stderr);
    exit(1);
  }

  st->dec->channel_layout = av_get_default_channel_layout(st->dec->channels);

  st->frame = alloc_audio_frame(st->dec->sample_fmt, st->dec->channel_layout,
                                st->dec->sample_rate, st->dec->frame_size);

  std::cout << "This stream has " << st->dec->channels
            << " channels and a sample rate of " << st->dec->sample_rate
            << "Hz and frame_size=" << st->dec->frame_size << std::endl
            << "The data is in the format "
            << av_get_sample_fmt_name(st->dec->sample_fmt) << std::endl;

  return st;
}

void AvCodec::set_video_codec_params(double width, double height, int fps,
                                     int bitrate) {
  const AVRational dst_fps = {fps, 1};

  video_st.enc->codec_tag = 0;
  video_st.enc->codec_id = AV_CODEC_ID_H264;
  video_st.enc->codec_type = AVMEDIA_TYPE_VIDEO;
  video_st.enc->width = width;
  video_st.enc->height = height;
  video_st.enc->gop_size = 12;
  video_st.enc->pix_fmt = AV_PIX_FMT_YUV420P;
  video_st.enc->framerate = dst_fps;
  video_st.enc->time_base = av_inv_q(dst_fps);
  video_st.enc->bit_rate = bitrate;
  video_st.st->time_base = (AVRational){1, fps};
  video_st.enc->time_base = video_st.st->time_base;
  video_st.enc->gop_size = std::atoi(video_keyframe_group_size.c_str());
  if (ofmt_ctx->oformat->flags & AVFMT_GLOBALHEADER) {
    video_st.enc->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
  }
}
void AvCodec::set_audio_codec_params(double width, double height, int fps,
                                     int bitrate) {
  audio_st.st->id = 1;
  audio_st.st->index = 1;
  audio_st.enc->codec_id = AV_CODEC_ID_AAC;
  audio_st.enc->sample_fmt = audio_st.codec->sample_fmts[0];
  audio_st.enc->bit_rate = 128000;
  audio_st.enc->sample_rate = 44100;
  audio_st.enc->channels = 2;
  audio_st.enc->codec_type = AVMEDIA_TYPE_AUDIO;
  audio_st.enc->channel_layout =
      av_get_default_channel_layout(audio_st.enc->channels);
  audio_st.st->time_base = (AVRational){1, audio_st.enc->sample_rate};
  audio_st.enc->time_base = audio_st.st->time_base;
  if (ofmt_ctx->oformat->flags & AVFMT_GLOBALHEADER) {
    audio_st.enc->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
  }
}

void AvCodec::initialize_video_codec_stream(const std::string &codec_profile) {
  // Video
  int ret =
      avcodec_parameters_from_context(video_st.st->codecpar, video_st.enc);
  if (ret < 0) {
    fprintf(stderr,
            "Could not initialize video stream codec parameters (error '%s')\n",
            _av_err2str(ret));
    fflush(stderr);
    exit(1);
  }

  AVDictionary *codec_options = nullptr;
  av_dict_set(&codec_options, "profile", codec_profile.c_str(), 0);
  av_dict_set(&codec_options, "preset", video_preset.c_str(), 0);
  av_dict_set(&codec_options, "tune", video_tune.c_str(), 0);
  av_dict_set(&codec_options, "g", video_keyframe_group_size.c_str(), 0);
  av_dict_set(&codec_options, "video_bufsize", video_bufsize.c_str(), 0);

  // open video encoder
  ret = avcodec_open2(video_st.enc, video_st.codec, &codec_options);
  av_dict_free(&codec_options);
  if (ret < 0) {
    fprintf(stderr, "Could not open video encoder (error '%s')\n",
            _av_err2str(ret));
    fflush(stderr);
    exit(1);
  }
}

void AvCodec::initialize_audio_codec_stream(const std::string &codec_profile) {
  // Audio

  int ret =
      avcodec_parameters_from_context(audio_st.st->codecpar, audio_st.enc);
  if (ret < 0) {
    fprintf(stderr,
            "Could not initialize audio stream codec parameters (error '%s')\n",
            _av_err2str(ret));
    fflush(stderr);
    exit(1);
  }

  AVDictionary *codec_options = nullptr;

  // open video encoder
  ret = avcodec_open2(audio_st.enc, audio_st.codec, &codec_options);
  av_dict_free(&codec_options);
  if (ret < 0) {
    fprintf(stderr, "Could not open video encoder (error '%s')\n",
            _av_err2str(ret));
    fflush(stderr);
    exit(1);
  }
}

SwsContext *AvCodec::initialize_sample_scaler(double width, double height) {
  SwsContext *swsctx = sws_getContext(width, height, AV_PIX_FMT_BGR24, width,
                                      height, video_st.enc->pix_fmt,
                                      SWS_BICUBIC, nullptr, nullptr, nullptr);
  if (!swsctx) {
    fprintf(stderr, "Could not initialize sample scaler\n");
    fflush(stderr);
    exit(1);
  }

  return swsctx;
}

AVFrame *AvCodec::allocate_frame_buffer(double width, double height) {
  AVFrame *frame = av_frame_alloc();

  // std::vector<uint8_t> framebuf(
  //     av_image_get_buffer_size(video_st.enc->pix_fmt, width, height, 1));
  // av_image_fill_arrays(frame->data, frame->linesize, framebuf.data(),
  //                      video_st.enc->pix_fmt, width, height, 1);
  av_image_alloc(frame->data, frame->linesize, width, height,
                 video_st.enc->pix_fmt, 1);
  frame->width = width;
  frame->height = height;
  frame->format = static_cast<int>(video_st.enc->pix_fmt);
  frame->key_frame = 0;
  frame->pts = 0;

  return frame;
}

void AvCodec::write_frame(OutputStream &stream) {
  AVPacket pkt = {0};
  av_init_packet(&pkt);

  /* rescale output packet timestamp values from codec to stream timebase */
  av_packet_rescale_ts(&pkt, stream.enc->time_base, stream.st->time_base);

  int ret = avcodec_send_frame(stream.enc, stream.frame);
  if (ret < 0) {
    fprintf(stderr, "Could not send frame (error '%s')\n", _av_err2str(ret));
    return;
  }

  pkt.stream_index = stream.st->index;
  pkt.pos = -1;

  ret = avcodec_receive_packet(stream.enc, &pkt);
  if (ret < 0) {
    fprintf(stderr, "Could not encode frame (error '%s')\n", _av_err2str(ret));
    fflush(stderr);
    av_packet_unref(&pkt);
    return;
  }

  pkt.stream_index = stream.st->index;
  pkt.pos = -1;

  if (stream.enc->frame_number % 300 == 0) {
    printf(
        "Frame (%d) pts %lld key_frame %d [coded_picture_number "
        "%d,"
        "display_picture_number %d] %d %d\n",
        stream.enc->frame_number, static_cast<long long>(stream.frame->pts),
        stream.frame->key_frame, stream.frame->coded_picture_number,
        stream.frame->display_picture_number, pkt.stream_index,
        stream.st->index);
  }

  av_interleaved_write_frame(ofmt_ctx, &pkt);

  av_packet_unref(&pkt);
}

void AvCodec::sws_scale_video(const uint8_t *const image_data[],
                              const int stride[], int image_rows) {
  ::sws_scale(video_st.sws_ctx, image_data, stride, 0, image_rows,
              video_st.frame->data, video_st.frame->linesize);
}

void AvCodec::rescale_video_frame() {
  video_st.frame->pts = video_st.next_pts;
  video_st.next_pts +=
      av_rescale_q(1, video_st.enc->time_base, video_st.st->time_base);
  ;
}

AVFrame *AvCodec::alloc_audio_frame(enum AVSampleFormat sample_fmt,
                                    uint64_t channel_layout, int sample_rate,
                                    int nb_samples) {
  AVFrame *frame = av_frame_alloc();
  int ret;

  if (!frame) {
    fprintf(stderr, "Error allocating an audio frame\n");
    fflush(stderr);
    exit(1);
  }

  frame->format = sample_fmt;
  frame->channel_layout = channel_layout;
  frame->sample_rate = sample_rate;
  frame->nb_samples = nb_samples;

  printf("format=%d channel_layout=%lld sample_rate=%d nb_samples=%d\n",
         sample_fmt, static_cast<unsigned long long>(channel_layout),
         sample_rate, nb_samples);

  if (nb_samples) {
    ret = av_frame_get_buffer(frame, 0);
    if (ret < 0) {
      fprintf(stderr, "Error allocating an audio buffer (error '%s')\n",
              _av_err2str(ret));
      fflush(stderr);
      exit(1);
    }
  }

  return frame;
}

void AvCodec::open_audio_output() {
  AVCodecContext *c;
  int nb_samples;
  int ret;

  c = audio_st.enc;

  if (c->codec->capabilities & AV_CODEC_CAP_VARIABLE_FRAME_SIZE) {
    nb_samples = 10000;
  } else {
    nb_samples = c->frame_size;
  }

  audio_st.frame = alloc_audio_frame(c->sample_fmt, c->channel_layout,
                                     c->sample_rate, nb_samples);
  audio_st.frame->pts = audio_st.next_pts;

  /* copy the stream parameters to the muxer */
  ret = avcodec_parameters_from_context(audio_st.st->codecpar, c);
  if (ret < 0) {
    fprintf(stderr, "Could not copy the stream parameters\n");
    exit(1);
  }
}

void AvCodec::open_audio_input(int id) {
  InputStream &st = *audio_input_st[id];
  int ret;
  /* create resampler context */
  st.swr_ctx = swr_alloc();
  if (!st.swr_ctx) {
    fprintf(stderr, "Could not allocate resampler context\n");
    exit(1);
  }

  /* set options */
  av_opt_set_int(st.swr_ctx, "in_channel_count", st.dec->channels, 0);
  av_opt_set_int(st.swr_ctx, "in_sample_rate", st.dec->sample_rate, 0);
  av_opt_set_sample_fmt(st.swr_ctx, "in_sample_fmt", st.dec->sample_fmt, 0);
  av_opt_set_int(st.swr_ctx, "out_channel_count", audio_st.enc->channels, 0);
  av_opt_set_int(st.swr_ctx, "out_sample_rate", audio_st.enc->sample_rate, 0);
  av_opt_set_sample_fmt(st.swr_ctx, "out_sample_fmt", audio_st.enc->sample_fmt,
                        0);

  /* initialize the resampling context */
  if ((ret = swr_init(st.swr_ctx)) < 0) {
    fprintf(stderr, "Failed to initialize the resampling context\n");
    exit(1);
  }
}

void AvCodec::get_audio_frame(int id) {
  InputStream &st = *audio_input_st[id];
  AVPacket av_packet_in = {0};
  av_init_packet(&av_packet_in);

  if (av_read_frame(st.ifmt_ctx, &av_packet_in) < 0) exit(1);

  if (avcodec_send_packet(st.dec, &av_packet_in) != 0) {
    printf("err 1\n");
    exit(1);
  }

  if (avcodec_receive_frame(st.dec, st.frame) != 0) {
    printf("err 2\n");
    exit(1);
  }

  av_packet_unref(&av_packet_in);
}

void AvCodec::write_audio_frame(int id) {
  AVCodecContext *c;
  int ret;
  int dst_nb_samples;
  InputStream &st = *audio_input_st[id];

  c = audio_st.enc;

  get_audio_frame(id);

  /* convert samples from native format to destination codec format, using the
   * resampler */
  /* compute destination number of samples */
  dst_nb_samples = av_rescale_rnd(
      swr_get_delay(st.swr_ctx, c->sample_rate) + st.frame->nb_samples,
      c->sample_rate, c->sample_rate, AV_ROUND_UP);
  // printf("%d %d\n", dst_nb_samples, st.frame->nb_samples);
  // av_assert0(dst_nb_samples == st.frame->nb_samples);

  /* when we pass a frame to the encoder, it may keep a reference to it
   * internally;
   * make sure we do not overwrite it here
   */
  ret = av_frame_make_writable(audio_st.frame);
  if (ret < 0) exit(1);

  /* convert to destination format */
  ret = swr_convert(st.swr_ctx, audio_st.frame->data, dst_nb_samples,
                    const_cast<const uint8_t **>(st.frame->data),
                    st.frame->nb_samples);
  if (ret < 0) {
    fprintf(stderr, "Error while converting\n");
    exit(1);
  }
  audio_st.frame->pts = audio_st.next_pts;
  audio_st.next_pts +=
      av_rescale_q(audio_st.frame->nb_samples, audio_st.enc->time_base,
                   audio_st.st->time_base);

  write_frame(audio_st);
}

void AvCodec::write_frames() {
  std::scoped_lock lock(writing_mutex);
  write_frame(video_st);
}

void AvCodec::audio_loop() {
  while (selected_audio_id >= -1) {
    if (selected_audio_id < 0) {
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
    } else {
      std::scoped_lock lock(writing_mutex);
      while (av_compare_ts(audio_st.next_pts, video_st.enc->time_base,
                           video_st.next_pts, video_st.enc->time_base) <= 0) {
        write_audio_frame(selected_audio_id);
      }
    }
  }
}
