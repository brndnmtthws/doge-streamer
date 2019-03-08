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
#include "log.h"

static char _av_err[AV_ERROR_MAX_STRING_SIZE] = "\0";

std::string _av_err2str(int errnum) {
  _av_err[0] = '\0';
  av_make_error_string(_av_err, AV_ERROR_MAX_STRING_SIZE, errnum);
  return std::string(_av_err);
}

AvCodec::AvCodec(double width, double height, double fps, int bitrate,
                 const std::string &codec_profile, const std::string &server,
                 const std::string &audio_format, const bool audio_out,
                 const std::string &video_preset,
                 const std::string &video_keyframe_group_size, int audio_idx,
                 const std::string &video_bufsize,
                 const std::string &video_minrate,
                 const std::string &video_maxrate,
                 const std::string &video_tune)
    : ofmt_ctx(nullptr),
      selected_audio_id(-1),
      audio_format(audio_format),
      audio_out(audio_out),
      video_preset(video_preset),
      video_keyframe_group_size(video_keyframe_group_size),
      audio_idx(audio_idx),
      video_bufsize(video_bufsize),
      video_minrate(video_minrate),
      video_maxrate(video_maxrate),
      video_tune(video_tune),
      bitrate(bitrate),
      fps(fps) {
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

  video_st.sws_ctx = initialize_sample_scaler(width, height);
  video_st.frame = allocate_frame_buffer(width, height);

  if (audio_out) {
    audio_st.codec = avcodec_find_encoder(AV_CODEC_ID_AAC);
    audio_st.st = avformat_new_stream(ofmt_ctx, audio_st.codec);
    audio_st.enc = avcodec_alloc_context3(audio_st.codec);

    set_audio_codec_params(width, height, fps, bitrate);
    initialize_audio_codec_stream(codec_profile);

    open_audio_output();

    init_fifo(&audio_st.fifo, audio_st.enc);
  }

  av_dump_format(ofmt_ctx, 0, output, 1);

  int ret = avformat_write_header(ofmt_ctx, nullptr);
  if (ret < 0) {
    log_critical("Could not write header (error '{}')", _av_err2str(ret));
    exit(1);
  }

  if (audio_out) {
    audio_thread = std::thread([this] { audio_loop(); });
  }
}

AvCodec::~AvCodec() {
  log_debug("AvCodec::~AvCodec");
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
    log_critical("Could not allocate output format context (error '{}')",
                 _av_err2str(ret));
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
      log_critical("Could not open output IO context (error '{}')",
                   _av_err2str(ret));
      exit(1);
    }
  }
}

void AvCodec::init_audio() {
  if (!audio_out) { return; }
  std::ostringstream ss;
  ss << ":" << audio_idx;
  // ss << "hw:" << id + audio_idx << ",0";
  std::string devname(ss.str());
  audio_input_st[audio_idx] =
      initialize_audio_input_context(audio_format, devname);
  open_audio_input(audio_idx);
  selected_audio_id = audio_idx;
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
    log_error("Error opening audio device {} (error '{}')", devname,
              _av_err2str(ret));
    exit(1);
  }

  ret = avformat_find_stream_info(st->ifmt_ctx, NULL);
  if (ret != 0) {
    avformat_close_input(&st->ifmt_ctx);
    log_critical("Error finding stream info (error '{}')", _av_err2str(ret));
    exit(1);
  }

  // Find the audio stream
  stream_found = av_find_best_stream(st->ifmt_ctx, AVMEDIA_TYPE_AUDIO, -1, -1,
                                     &st->codec, 0);

  if (stream_found < 0) {
    avformat_close_input(&st->ifmt_ctx);
    log_critical("Stream not found");
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
    log_critical("Error initializing stream parameters (error '{}')",
                 _av_err2str(ret));
    exit(1);
  }

  ret = avcodec_open2(st->dec, st->dec->codec, NULL);
  if (ret != 0) {
    avcodec_close(st->dec);
    avcodec_free_context(&st->dec);
    avformat_close_input(&st->ifmt_ctx);
    log_critical("Error opening context with decoder (error '{}')",
                 _av_err2str(ret));
    exit(1);
  }

  st->dec->channel_layout = av_get_default_channel_layout(st->dec->channels);

  std::cout << "This stream has " << st->dec->channels
            << " channels and a sample rate of " << st->dec->sample_rate
            << "Hz and frame_size=" << st->dec->frame_size << std::endl
            << "The data is in the format "
            << av_get_sample_fmt_name(st->dec->sample_fmt) << std::endl;

  return st;
}

void AvCodec::set_video_codec_params(double width, double height, int fps,
                                     int bitrate) {
  video_st.enc->codec_tag = 0;
  video_st.enc->codec_id = AV_CODEC_ID_H264;
  video_st.enc->codec_type = AVMEDIA_TYPE_VIDEO;
  video_st.enc->width = width;
  video_st.enc->height = height;
  video_st.enc->pix_fmt = AV_PIX_FMT_YUV420P;
  video_st.enc->time_base = (AVRational){1, fps};
  video_st.enc->framerate = (AVRational){fps, 1};
  video_st.enc->bit_rate = bitrate;
  video_st.st->time_base = video_st.enc->time_base;
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
  audio_st.enc->strict_std_compliance = FF_COMPLIANCE_EXPERIMENTAL;
  audio_st.enc->channel_layout =
      av_get_default_channel_layout(audio_st.enc->channels);
  audio_st.st->time_base = (AVRational){1, audio_st.enc->sample_rate};
  audio_st.enc->time_base = audio_st.st->time_base;
  ofmt_ctx->streams[1]->time_base = audio_st.st->time_base;
  if (ofmt_ctx->oformat->flags & AVFMT_GLOBALHEADER) {
    audio_st.enc->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
  }
}

void AvCodec::initialize_video_codec_stream(const std::string &codec_profile) {
  // Video
  int ret =
      avcodec_parameters_from_context(video_st.st->codecpar, video_st.enc);
  if (ret < 0) {
    log_critical(
        "Could not initialize video stream codec parameters (error '{}')",
        _av_err2str(ret));
    exit(1);
  }

  AVDictionary *codec_options = nullptr;
  av_dict_set(&codec_options, "profile", codec_profile.c_str(), 0);
  av_dict_set(&codec_options, "preset", video_preset.c_str(), 0);
  av_dict_set(&codec_options, "tune", video_tune.c_str(), 0);
  av_dict_set(&codec_options, "g", video_keyframe_group_size.c_str(), 0);
  av_dict_set(&codec_options, "bufsize", video_bufsize.c_str(), 0);
  av_dict_set(&codec_options, "minrate", video_minrate.c_str(), 0);
  av_dict_set(&codec_options, "maxrate", video_maxrate.c_str(), 0);
  av_dict_set(&codec_options, "b", std::to_string(bitrate).c_str(), 0);
  av_dict_set(&codec_options, "x264opts", "nal-hrd=cbr:force-cfr=1", 0);

  // open video encoder
  ret = avcodec_open2(video_st.enc, video_st.codec, &codec_options);
  av_dict_free(&codec_options);
  if (ret < 0) {
    log_critical("Could not open video encoder (error '{}')", _av_err2str(ret));
    exit(1);
  }
}

void AvCodec::initialize_audio_codec_stream(const std::string &codec_profile) {
  // Audio

  int ret =
      avcodec_parameters_from_context(audio_st.st->codecpar, audio_st.enc);
  if (ret < 0) {
    log_critical(
        "Could not initialize audio stream codec parameters (error '{}')",
        _av_err2str(ret));
    exit(1);
  }

  AVDictionary *codec_options = nullptr;

  // open video encoder
  ret = avcodec_open2(audio_st.enc, audio_st.codec, &codec_options);

  av_dict_set(&codec_options, "strict", "1", 0);

  av_dict_free(&codec_options);
  if (ret < 0) {
    log_critical("Could not open video encoder (error '{}')", _av_err2str(ret));
    exit(1);
  }
}

SwsContext *AvCodec::initialize_sample_scaler(double width, double height) {
  SwsContext *swsctx = sws_getContext(width, height, AV_PIX_FMT_BGR24, width,
                                      height, video_st.enc->pix_fmt,
                                      SWS_BICUBIC, nullptr, nullptr, nullptr);
  if (!swsctx) {
    log_critical("Could not initialize sample scaler");
    exit(1);
  }

  return swsctx;
}

AVFrame *AvCodec::allocate_frame_buffer(double width, double height) {
  AVFrame *frame = av_frame_alloc();

  av_image_alloc(frame->data, frame->linesize, width, height,
                 video_st.enc->pix_fmt, 1);
  frame->width = width;
  frame->height = height;
  frame->format = static_cast<int>(video_st.enc->pix_fmt);
  frame->key_frame = 0;
  frame->pts = 0;
  frame->pkt_dts = 0;
  frame->pkt_duration = 0;
  frame->pkt_pos = -1;

  return frame;
}

void AvCodec::write_packet(AVPacket *pkt, OutputStream &stream) {
  av_packet_rescale_ts(pkt, stream.enc->time_base,
                       ofmt_ctx->streams[stream.st->index]->time_base);

  pkt->stream_index = stream.st->index;

  if (stream.enc->frame_number % 107 == 0) {
    log_debug("Frame ({}) stream_index={} index={} pkt_pts=%{}",
              stream.enc->frame_number, pkt->stream_index, stream.st->index,
              pkt->pts);
  }

  std::scoped_lock lock(writing_mutex);
  av_interleaved_write_frame(ofmt_ctx, pkt);
}

void AvCodec::sws_scale_video(const uint8_t *const image_data[],
                              const int stride[], int image_rows) {
  av_frame_make_writable(video_st.frame);
  ::sws_scale(video_st.sws_ctx, image_data, stride, 0, image_rows,
              video_st.frame->data, video_st.frame->linesize);
}

void AvCodec::rescale_video_frame() {
  video_st.frame->pts = video_st.next_pts;
  video_st.last_pts = video_st.frame->pts;
  video_st.next_pts += 1;
}

void AvCodec::open_audio_output() {
  AVCodecContext *c;
  int nb_samples;
  int ret;

  c = audio_st.enc;

  /* copy the stream parameters to the muxer */
  ret = avcodec_parameters_from_context(audio_st.st->codecpar, c);
  if (ret < 0) {
    log_critical("Could not copy the stream parameters");
    exit(1);
  }
}

void AvCodec::open_audio_input(int id) {
  InputStream &st = *audio_input_st[id];
  int ret;
  /* create resampler context */
  st.swr_ctx = swr_alloc();
  if (!st.swr_ctx) {
    log_critical("Could not allocate resampler context");
    exit(1);
  }

  /* set options */
  av_opt_set_int(st.swr_ctx, "in_channel_layout", st.dec->channel_layout, 0);
  av_opt_set_int(st.swr_ctx, "in_channel_count", st.dec->channels, 0);
  av_opt_set_int(st.swr_ctx, "in_sample_rate", st.dec->sample_rate, 0);
  av_opt_set_sample_fmt(st.swr_ctx, "in_sample_fmt", st.dec->sample_fmt, 0);
  av_opt_set_int(st.swr_ctx, "out_channel_layout", audio_st.enc->channel_layout,
                 0);
  av_opt_set_int(st.swr_ctx, "out_channel_count", audio_st.enc->channels, 0);
  av_opt_set_int(st.swr_ctx, "out_sample_rate", audio_st.enc->sample_rate, 0);
  av_opt_set_sample_fmt(st.swr_ctx, "out_sample_fmt", audio_st.enc->sample_fmt,
                        0);

  /* initialize the resampling context */
  if ((ret = swr_init(st.swr_ctx)) < 0) {
    log_critical("Failed to initialize the resampling context");
    exit(1);
  }
}

void AvCodec::write_frames() {
  AVPacket output_packet;
  int error;
  init_packet(&output_packet);

  int ret = avcodec_send_frame(video_st.enc, video_st.frame);
  if (ret < 0) {
    log_error("Could not send frame (error '%s')", _av_err2str(ret));
    av_packet_unref(&output_packet);
    return;
  }

  ret = avcodec_receive_packet(video_st.enc, &output_packet);
  if (ret < 0) {
    log_error("Could not encode frame (error '%s')", _av_err2str(ret));
    av_packet_unref(&output_packet);
    return;
  }

  write_packet(&output_packet, video_st);
}

void AvCodec::audio_loop() {
  while (selected_audio_id >= -1) {
    if (selected_audio_id < 0) {
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
    } else {
      if (av_compare_ts(audio_st.next_pts, audio_st.enc->time_base,
                        video_st.last_pts - 5, video_st.enc->time_base) <= 0) {
        // audio and video might be out of sync
        audio_st.next_pts =
            av_rescale_q(video_st.last_pts, video_st.enc->time_base,
                         audio_st.enc->time_base);
      }
      while (av_compare_ts(audio_st.next_pts, audio_st.enc->time_base,
                           video_st.next_pts, video_st.enc->time_base) <= 0) {
        const int output_frame_size = audio_st.enc->frame_size;
        InputStream &st = *audio_input_st[selected_audio_id];

        while (av_audio_fifo_size(audio_st.fifo) < output_frame_size) {
          read_decode_convert_and_store(audio_st.fifo, st.ifmt_ctx, st.dec,
                                        audio_st.enc, st.swr_ctx);
        }

        while (av_audio_fifo_size(audio_st.fifo) >= output_frame_size) {
          load_encode_and_write(audio_st.fifo, ofmt_ctx, audio_st.enc);
        }
      }
    }
  }
}

int AvCodec::add_samples_to_fifo(AVAudioFifo *fifo,
                                 uint8_t **converted_input_samples,
                                 const int frame_size) {
  int error;

  if ((error = av_audio_fifo_realloc(
           fifo, av_audio_fifo_size(fifo) + frame_size)) < 0) {
    log_error("Could not reallocate FIFO");
    return error;
  }

  if (av_audio_fifo_write(fifo,
                          reinterpret_cast<void **>(converted_input_samples),
                          frame_size) < frame_size) {
    log_error("Could not write data to FIFO");
    return AVERROR_EXIT;
  }
  return 0;
}

int AvCodec::read_decode_convert_and_store(
    AVAudioFifo *fifo, AVFormatContext *input_format_context,
    AVCodecContext *input_codec_context, AVCodecContext *output_codec_context,
    SwrContext *resampler_context) {
  AVFrame *input_frame = NULL;
  uint8_t **converted_input_samples = NULL;
  int data_present = 0;
  int ret = AVERROR_EXIT;

  if (init_input_frame(&input_frame)) { goto cleanup; }
  if (decode_audio_frame(input_frame, input_format_context, input_codec_context,
                         &data_present)) {
    goto cleanup;
  }
  if (data_present) {
    if (init_converted_samples(&converted_input_samples, output_codec_context,
                               input_frame->nb_samples)) {
      goto cleanup;
    }

    av_frame_make_writable(input_frame);

    if (convert_samples(
            const_cast<const uint8_t **>(input_frame->extended_data),
            converted_input_samples, input_frame->nb_samples,
            resampler_context)) {
      goto cleanup;
    }

    if (add_samples_to_fifo(fifo, converted_input_samples,
                            input_frame->nb_samples)) {
      goto cleanup;
    }
  }
  ret = 0;

cleanup:
  if (converted_input_samples) {
    av_freep(&converted_input_samples[0]);
    free(converted_input_samples);
    converted_input_samples = nullptr;
  }
  av_frame_free(&input_frame);

  return ret;
}

int AvCodec::init_input_frame(AVFrame **frame) {
  if (!(*frame = av_frame_alloc())) {
    log_error("Could not allocate input frame");
    return AVERROR(ENOMEM);
  }
  return 0;
}

int AvCodec::decode_audio_frame(AVFrame *frame,
                                AVFormatContext *input_format_context,
                                AVCodecContext *input_codec_context,
                                int *data_present) {
  AVPacket input_packet;
  int error;
  init_packet(&input_packet);

  if ((error = av_read_frame(input_format_context, &input_packet)) < 0) {
    log_error("Could not read frame (error '{}')", _av_err2str(error));
    return error;
  }

  if ((error = avcodec_send_packet(input_codec_context, &input_packet)) < 0) {
    log_error("Could not send packet for decoding (error '{}')",
              _av_err2str(error));
    return error;
  }

  error = avcodec_receive_frame(input_codec_context, frame);
  if (error == AVERROR(EAGAIN)) {
    error = 0;
    goto cleanup;
  } else if (error == AVERROR_EOF) {
    error = 0;
    goto cleanup;
  } else if (error < 0) {
    log_error("Could not decode frame (error '{}')", _av_err2str(error));
    goto cleanup;
  } else {
    *data_present = 1;
    goto cleanup;
  }

cleanup:
  av_packet_unref(&input_packet);
  return error;
}

int AvCodec::init_converted_samples(uint8_t ***converted_input_samples,
                                    AVCodecContext *output_codec_context,
                                    int frame_size) {
  int error;

  if (!(*converted_input_samples = static_cast<uint8_t **>(
            calloc(output_codec_context->channels,
                   sizeof(**converted_input_samples))))) {
    log_error("Could not allocate converted input sample pointers");
    return AVERROR(ENOMEM);
  }

  if ((error = av_samples_alloc(*converted_input_samples, NULL,
                                output_codec_context->channels, frame_size,
                                output_codec_context->sample_fmt, 0)) < 0) {
    log_error("Could not allocate converted input samples (error '{}')",
              _av_err2str(error));
    av_freep(&(*converted_input_samples)[0]);
    free(*converted_input_samples);
    *converted_input_samples = nullptr;
    return error;
  }
  return 0;
}

int AvCodec::convert_samples(const uint8_t **input_data,
                             uint8_t **converted_data, const int frame_size,
                             SwrContext *resample_context) {
  int error;

  if ((error = swr_convert(resample_context, converted_data, frame_size,
                           input_data, frame_size)) < 0) {
    log_error("Could not convert input samples (error '{}')",
              _av_err2str(error));
    return error;
  }

  return 0;
}

void AvCodec::init_packet(AVPacket *packet) {
  av_init_packet(packet);
  packet->data = NULL;
  packet->size = 0;
  packet->pts = 0;
  packet->dts = 0;
  packet->duration = 0;
  packet->pos = -1;
}

int AvCodec::init_fifo(AVAudioFifo **fifo,
                       AVCodecContext *output_codec_context) {
  if (!(*fifo = av_audio_fifo_alloc(output_codec_context->sample_fmt,
                                    output_codec_context->channels, 1))) {
    log_error("Could not allocate FIFO");
    return AVERROR(ENOMEM);
  }
  return 0;
}

int AvCodec::load_encode_and_write(AVAudioFifo *fifo,
                                   AVFormatContext *output_format_context,
                                   AVCodecContext *output_codec_context) {
  AVFrame *output_frame;
  const int frame_size =
      FFMIN(av_audio_fifo_size(fifo), output_codec_context->frame_size);
  int data_written;

  if (init_output_frame(&output_frame, output_codec_context, frame_size)) {
    return AVERROR_EXIT;
  }

  if (av_audio_fifo_read(fifo, reinterpret_cast<void **>(output_frame->data),
                         frame_size) < frame_size) {
    log_error("Could not read data from FIFO");
    av_frame_free(&output_frame);
    return AVERROR_EXIT;
  }

  if (encode_audio_frame(output_frame, output_format_context,
                         output_codec_context, &data_written)) {
    av_frame_free(&output_frame);
    return AVERROR_EXIT;
  }
  av_frame_free(&output_frame);
  return 0;
}

int AvCodec::init_output_frame(AVFrame **frame,
                               AVCodecContext *output_codec_context,
                               int frame_size) {
  int error;

  if (!(*frame = av_frame_alloc())) {
    log_error("Could not allocate output frame");
    return AVERROR_EXIT;
  }

  (*frame)->pts = 0;
  (*frame)->pkt_dts = 0;
  (*frame)->pkt_duration = 0;
  (*frame)->nb_samples = frame_size;
  (*frame)->channel_layout = output_codec_context->channel_layout;
  (*frame)->format = output_codec_context->sample_fmt;
  (*frame)->sample_rate = output_codec_context->sample_rate;

  if ((error = av_frame_get_buffer(*frame, 0)) < 0) {
    log_error("Could not allocate output frame samples (error '{}')",
              _av_err2str(error));
    av_frame_free(frame);
    return error;
  }

  return 0;
}

int AvCodec::encode_audio_frame(AVFrame *frame,
                                AVFormatContext *output_format_context,
                                AVCodecContext *output_codec_context,
                                int *data_present) {
  AVPacket output_packet;
  int error;
  init_packet(&output_packet);

  if (frame) {
    frame->pts = audio_st.next_pts;
    audio_st.next_pts += frame->nb_samples;
  }

  error = avcodec_send_frame(output_codec_context, frame);
  if (error == AVERROR_EOF) {
    error = 0;
    av_packet_unref(&output_packet);
    return error;
  } else if (error < 0) {
    log_error("Could not send packet for encoding (error '{}')",
              _av_err2str(error));
    return error;
  }

  error = avcodec_receive_packet(output_codec_context, &output_packet);
  if (error == AVERROR(EAGAIN)) {
    error = 0;
    av_packet_unref(&output_packet);
    return error;
  } else if (error == AVERROR_EOF) {
    error = 0;
    av_packet_unref(&output_packet);
    return error;
  } else if (error < 0) {
    log_error("Could not encode frame (error '{}')", _av_err2str(error));
    av_packet_unref(&output_packet);
    return error;
  } else {
    *data_present = 1;
  }

  write_packet(&output_packet, audio_st);

  return error;
}
