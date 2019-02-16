#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <unistd.h>
#include <atomic>
#include <chrono>
#include <cmath>
#include <iomanip>
#include <iostream>
#include <random>
#include <thread>
#include <vector>

#include <opencv2/core/version.hpp>
#include <opencv2/highgui/highgui.hpp>
#include <opencv2/video/background_segm.hpp>
#include <opencv2/video/video.hpp>

#if !defined(CV_VERSION_EPOCH) && CV_VERSION_MAJOR >= 4
// for compat with older versions of opencv
#include <opencv2/videoio/videoio_c.h>
#endif /* !defined(CV_VERSION_EPOCH) && CV_VERSION_MAJOR >= 4 */

#define HTTP_IMPLEMENTATION

#include <clipp.h>
#include <http.h>
#include <picojson.h>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/spdlog.h>

#include "cam-manage.h"
#include "log.h"

using clipp::option, clipp::value;

std::atomic<bool> end_of_stream = false;

enum STREAM_STATE {
  stream_on,
  stream_off,
  stream_paused,
};

std::atomic<STREAM_STATE> stream_state = stream_off;

void check_stream_state(const std::string &url) {
  do {
    http_t *request = http_get(url.c_str(), NULL);
    if (!request) {
      log_error("Invalid request");
      exit(1);
    }

    http_status_t status = HTTP_STATUS_PENDING;
    int prev_size = -1;
    while (status == HTTP_STATUS_PENDING) {
      status = http_process(request);
      if (prev_size != static_cast<int>(request->response_size)) {
        prev_size = static_cast<int>(request->response_size);
      }
    }

    if (status == HTTP_STATUS_FAILED) {
      log_error("HTTP request failed ({}): {}", request->status_code,
                request->reason_phrase);
    } else {
      using picojson::object, picojson::parse;
      picojson::value jv;
      std::string err = parse(
          jv, std::string(static_cast<const char *>(request->response_data)));
      if (err.empty()) {
        if (jv.is<object>()) {
          object obj = jv.get<object>();
          std::string state = obj["state"].to_str();
          if (state == "on") {
            stream_state = stream_on;
          } else if (state == "off") {
            stream_state = stream_off;
            end_of_stream = true;
            log_info("shutting off stream because state=off");
          } else if (state == "paused") {
            stream_state = stream_paused;
          }
        } else {
          log_warn("unexpected response when checking stream state: {}",
                   static_cast<const char *>(request->response_data));
        }
      } else {
        log_error("error parsing response from stream check: {}", err);
      }
    }

    http_release(request);

    std::this_thread::sleep_for(std::chrono::seconds(1));
  } while (stream_state != stream_off && !end_of_stream);
}

cv::VideoCapture get_device(int camID, double width, double height,
                            double fps) {
  cv::VideoCapture cam(camID);
  if (!cam.isOpened()) {
    throw std::runtime_error("Failed to open video capture device");
  }

  cam.set(CV_CAP_PROP_FRAME_WIDTH, width);
  cam.set(CV_CAP_PROP_FRAME_HEIGHT, height);
  cam.set(CV_CAP_PROP_FPS, fps);

  return cam;
}

std::shared_ptr<cv::Mat> image_open(const std::string &fn) {
  const std::string paths[] = {"/opt/doge/artwork/", "./"};
  log_debug("image_open('{}')\n", fn.c_str());
  for (auto p : paths) {
    struct stat stat_result;
    std::string name(p + fn);
    if (stat(name.c_str(), &stat_result) == 0) {
      auto ptr = std::make_shared<cv::Mat>(
          cv::imread(name.c_str(), cv::IMREAD_UNCHANGED));
      if (ptr->empty()) {
        log_critical("can't read {}", fn.c_str());
        exit(1);
      }
      return ptr;
    }
  }
  log_critical("can't read {}", fn.c_str());
  exit(1);
}

void camera_main_loop(const int camID, const double width, const double height,
                      const double fps, Renderer *renderer,
                      CamThreads *cam_threads, CamSwitcher *cam_switcher,
                      const bool save_doges) {
  log_debug("entered camera_main_loop {}", camID);
  auto cam = get_device(camID, width, height, fps);
  std::vector<uint8_t> imgbuf(height * width * 3 + 16);
  cv::Mat image(height, width, CV_8UC3, imgbuf.data(), width * 3);

  auto bg = cv::createBackgroundSubtractorMOG2(300, 20, true);
  auto kernel = cv::getStructuringElement(cv::MORPH_ELLIPSE, cv::Point(3, 3));

  assert(cam.isOpened());

  // Read one frame
  cam >> image;
  if (image.empty()) {
    log_warn("got an empty image from camID=%d", camID);
    cam.release();
    cam_threads->remove(camID);
    return;
  }

  cam_switcher->mark_active(camID, 0);

  cv::Mat fgMask, hsvFrame, mask, image_copy;
  std::vector<cv::Mat> splitFrame;
  bool motion_before = false;
  bool motion_after = false;
  bool odd_run = false;
  double area_max = 0;
  auto start = std::chrono::system_clock::now();
  auto last_saved_at = std::chrono::system_clock::now();
  std::chrono::duration<double> elapsed_motion(0);
  size_t nFrames = 0;
  int64 t0 = cv::getTickCount();
  int64 processingTime = 0;
  do {
    try {
      cam >> image;
      if (image.empty()) {
        log_warn("got an empty image from camID=%d", camID);
        continue;
      }

      nFrames++;
      if (nFrames % 300 == 0) {
        const int N = 300;
        int64 t1 = cv::getTickCount();
        log_info(
            "rendering={} camID={} captured={} Avg FPS={} Avg time per "
            "frame={} Avg processing time={}",
            cam_switcher->get_top(), camID,
            cv::format("%5lld", static_cast<long long int>(nFrames)).c_str(),
            cv::format("%2.1f", static_cast<double>(cv::getTickFrequency() * N /
                                                    (t1 - t0)))
                .c_str(),
            cv::format("%3.2f ms",
                       static_cast<double>((t1 - t0) * 1000.0f /
                                           (N * cv::getTickFrequency())))
                .c_str(),
            cv::format("%3.2f ms",
                       static_cast<double>((processingTime)*1000.0f /
                                           (N * cv::getTickFrequency())))
                .c_str());

        t0 = t1;
        processingTime = 0;
      }

      odd_run = !odd_run;
      auto end = std::chrono::system_clock::now();
      if (odd_run) {
        cv::cvtColor(image, hsvFrame, cv::COLOR_BGR2HSV);
        cv::resize(hsvFrame, hsvFrame, cv::Size(640, 360), 0, 0,
                   cv::INTER_AREA);
        // Apply Doge mask
        cv::Scalar lower = cv::Scalar(10, 10, 10);
        cv::Scalar upper = cv::Scalar(40, 240, 240);

        cv::inRange(hsvFrame, lower, upper, mask);
        cv::bitwise_and(hsvFrame, hsvFrame, mask);

        // Split into separate channels
        cv::split(hsvFrame, splitFrame);
        // Take only the V (value) channel
        cv::GaussianBlur(splitFrame[2], hsvFrame, cv::Size(21, 21), 0);

        bg->apply(hsvFrame, fgMask);
        cv::morphologyEx(fgMask, fgMask, cv::MORPH_OPEN, kernel);

        std::vector<std::vector<cv::Point>> cnts;
        cv::findContours(fgMask, cnts, cv::RETR_EXTERNAL,
                         cv::CHAIN_APPROX_SIMPLE);

        motion_after = false;
        for (int i = 0; i < cnts.size(); i++) {
          if (contourArea(cnts[i]) < 500) {
            // Ignore tiny areas
            continue;
          }
          area_max = std::max(area_max, contourArea(cnts[i]));
          motion_after = true;
        }

        if (motion_after && !motion_before) {
          // Motion started
          start = end;
        }
        if (motion_after && motion_before) {
          // Motion continues
          elapsed_motion = end - start;
        }
        if (!motion_after && motion_before) {
          // Motion stopped
          elapsed_motion = end - start;
          log_info("camID={} motion stopped, lasted {}s", camID,
                   elapsed_motion.count());

          area_max = 0;
          if (elapsed_motion >= std::chrono::milliseconds(1200)) {
            cam_switcher->mark_active(camID, area_max);
          }
          elapsed_motion = std::chrono::seconds(0);
        }
        motion_before = motion_after;

        if (elapsed_motion >= std::chrono::milliseconds(1200)) {
          cam_switcher->mark_active(camID, area_max);
        }
      }

      image.copyTo(image_copy);
      if (cam_switcher->get_top() == camID) {
        // Is this camera current the top cam? If yes, render it.
        renderer->render(image, camID);

        if (save_doges) {
          auto since_last_save = end - last_saved_at;
          if (since_last_save >= std::chrono::minutes(1)) {
            last_saved_at = end;
            auto timet = std::chrono::system_clock::to_time_t(end);
            auto t = std::put_time(std::localtime(&timet), "%FZ%T");
            std::ostringstream ss;
            ss << "doge/" << camID << "-" << t << ".jpg";
            cv::imwrite(ss.str(), image_copy);
          }
        }
      } else if (save_doges) {
        auto since_last_save = end - last_saved_at;
        if (since_last_save >= std::chrono::minutes(1)) {
          last_saved_at = end;
          auto timet = std::chrono::system_clock::to_time_t(end);
          auto t = std::put_time(std::localtime(&timet), "%FZ%T");
          std::ostringstream ss;
          ss << "notdoge/" << camID << "-" << t << ".jpg";
          cv::imwrite(ss.str(), image_copy);
        }
      }
    } catch (cv::Exception &e) { log_error("caught exception: {}", e.what()); }
  } while (!end_of_stream && stream_state == stream_on);

  log_debug("end of camera_main_loop for camID={}", camID);

  cam_switcher->remove(camID);
  cam_threads->remove(camID);
  cam.release();
}

std::shared_ptr<cv::Mat> get_bg(
    const std::vector<std::shared_ptr<cv::Mat>> &bg_list) {
  std::mt19937 rng;
  rng.seed(std::random_device()());
  std::uniform_int_distribution<std::mt19937::result_type> dist(
      0, bg_list.size() - 1);
  size_t sicrit = dist(rng);
  return bg_list[sicrit];
}

void bg_main_loop(const double width, const double height, const double fps,
                  Renderer *renderer, CamSwitcher *cam_switcher) {
  log_debug("entered bg_main_loop");
  std::vector<uint8_t> imgbuf(height * width * 3 + 16);
  cv::Mat image(height, width, CV_8UC3, imgbuf.data(), width * 3);

  std::vector<std::shared_ptr<cv::Mat>> bg_list;
  for (int i = 0; i < 7; ++i) {
    struct stat stat_result;
    std::ostringstream ss;
    ss << "bg" << i + 1 << ".png";
    std::string fn = ss.str();
    auto bg = image_open(fn);
    cv::resize(*bg, *bg, cv::Size(width, height), 0, 0, cv::INTER_CUBIC);
    cv::cvtColor(*bg, *bg, cv::COLOR_RGBA2RGB);
    bg_list.push_back(bg);
  }
  auto bg_changed = std::chrono::system_clock::now();
  auto bg = get_bg(bg_list);
  while (!end_of_stream && stream_state != stream_off) {
    try {
      if (cam_switcher->empty()) {
        auto now = std::chrono::system_clock::now();
        if (now - bg_changed > std::chrono::minutes(7)) {
          bg = get_bg(bg_list);
          bg_changed = now;
        }
        renderer->render(*bg, -1);
      }
    } catch (cv::Exception &e) { log_error("caught exception: {}", e.what()); }
    std::this_thread::sleep_for(
        std::chrono::milliseconds(std::lround(std::ceil((1.0 / fps * 1000)))));
  }
}

void stream_video(double width, double height, double fps, int bitrate,
                  const std::string &codec_profile, const std::string &server,
                  const std::string &audio_format, const bool audio_out,
                  const std::string &video_preset,
                  const std::string &video_keyframe_group_size,
                  int cam_idx_start, int cam_idx_stop, int audio_idx,
                  const std::string &video_bufsize,
                  const std::string &video_minrate,
                  const std::string &video_maxrate,
                  const std::string &video_tune, const bool save_doges) {
  AvCodec avCodec(width, height, fps, bitrate, codec_profile, server,
                  audio_format, audio_out, video_preset,
                  video_keyframe_group_size, audio_idx, video_bufsize,
                  video_minrate, video_maxrate, video_tune);
  CamThreads cam_threads;
  CamSwitcher cam_switcher;
  Renderer renderer(&avCodec);

  auto bg_image_thread = std::make_shared<std::thread>(
      [width, height, fps, &renderer, &cam_switcher] {
        bg_main_loop(width, height, fps, &renderer, &cam_switcher);
      });

  avCodec.init_audio();

  for (int i = 0; i < 20 && !end_of_stream; ++i) {
    std::this_thread::sleep_for(std::chrono::seconds(1));
  }

  while (!end_of_stream && stream_state != stream_off) {
    try {
      for (int i = cam_idx_start; i < cam_idx_stop && !end_of_stream; ++i) {
        if (stream_state == stream_paused) { continue; }
        if (cam_threads.has_cam(i)) { continue; }
        {
          auto cam = get_device(i, width, height, fps);
          cam.release();
          std::this_thread::sleep_for(std::chrono::seconds(5));
        }
        auto ptr = std::make_shared<std::thread>([i, width, height, fps,
                                                  &renderer, &cam_threads,
                                                  &cam_switcher, save_doges] {
          camera_main_loop(i, width, height, fps, &renderer, &cam_threads,
                           &cam_switcher, save_doges);
        });
        cam_threads.add(i, ptr);
        std::this_thread::sleep_for(std::chrono::seconds(20));
      }
    } catch (std::runtime_error &e) {
      log_error("caught exception: {}", e.what());
    }
    cam_threads.join_joinable();
    for (int i = 0; i < 60 && !end_of_stream; ++i) {
      std::this_thread::sleep_for(std::chrono::seconds(1));
    }
  }

  bg_image_thread->join();
  cam_threads.join_all();
}

void my_handler(int s) {
  log_info("Caught signal {}", s);
  end_of_stream = true;
  exit(1);
}

void init_logger(const std::string &log_level) {
  auto log_level_enum = spdlog::level::from_str(log_level);
  spdlog::set_level(log_level_enum);

  if (log_level_enum == spdlog::level::debug) {
    av_log_set_level(AV_LOG_DEBUG);
  } else if (log_level_enum <= spdlog::level::warn) {
    av_log_set_level(AV_LOG_VERBOSE);
  }

  auto console = spdlog::stdout_color_mt("console");
  auto err_logger = spdlog::stderr_color_mt("stderr");
  spdlog::set_pattern("[%Y-%m-%dT%H:%M:%S.%e] [%^%l%$] %v");
}

int main(int argc, char *argv[]) {
  int width = 1920, height = 1080, bitrate = 5 * 1024 * 1024;
  double fps = 30;
  std::string h264profile = "high";
  std::string audio_format = "alsa";
  std::string outputServer = "rtmp://localhost/live/stream";
  std::string video_preset = "medium";
  std::string video_keyframe_group_size = "90";
  std::string state_url = "";
  std::string video_bufsize = "10M";
  std::string video_minrate = "5M";
  std::string video_maxrate = "5M";
  std::string video_tune = "zerolatency";
  std::string log_level = "info";
  int cam_idx_start = 0;
  int cam_idx_stop = 4;
  int audio_idx = 0;
  bool audio_out = false;
  bool save_doges = false;

  auto cli =
      ((option("-o", "--output") & value("output", outputServer)) %
           "output RTMP server (default: rtmp://localhost/live/stream)",
       (option("-c", "--cam-index-start") &
        value("cam_idx_start", cam_idx_start)) %
           "starting cam index (default: 0)",
       (option("-x", "--cam-index-stop") &
        value("cam_idx_stop", cam_idx_stop)) %
           "stopping cam index (default: 4)",
       (option("-s", "--audio-index") & value("audio_idx", audio_idx)) %
           "audio (sound card) index (default: 0)",
       (option("-f", "--fps") & value("fps", fps)) %
           "frames-per-second (default: 30)",
       (option("-w", "--width") & value("width", width)) %
           "video width (default: 1920)",
       (option("-h", "--height") & value("height", height)) %
           "video height (default: 1080)",
       (option("-b", "--bitrate") & value("bitrate", bitrate)) %
           "stream bitrate in kb/s (default: 5242880)",
       (option("-p", "--profile") & value("profile", h264profile)) %
           "H264 codec profile (baseline | high | high10 | high422 | "
           "high444 | main) (default: high)",
       (option("-k", "--keyframe-group-size") &
        value("video_keyframe_group_size", video_keyframe_group_size)) %
           "keyframe group size in number of frames (default: 90)",
       (option("-z", "--video-bufsize") &
        value("video-bufsize", video_bufsize)) %
           "stream buffer size (default: 10M)",
       (option("--video-minrate") & value("video-minrate", video_minrate)) %
           "stream minimum bitrate (default: 5M)",
       (option("--video-maxrate") & value("video-maxrate", video_maxrate)) %
           "stream maximum bitrate (default: 5M)",
       (option("-n", "--video-tune") & value("video-tune", video_tune)) %
           "tune parameter for x264 (default: zerolatency)",
       (option("-t", "--preset") & value("video-tune", video_preset)) %
           "x264 encoding preset (default: medium)",
       (option("-u", "--audio-format") & value("audio-format", audio_format)) %
           "Audio input FFmpeg format (default: alsa)",
       (option("-a", "--audio-out").set(audio_out)) %
           "output audio (default: false)",
       (option("--save-doges").set(save_doges)) %
           "save doge images (default: false)",
       (option("-r", "--state-url") & value("state-url", state_url)) %
           "stream state url for pausing/deactivating stream (default: "
           "none)",
       (option("-l", "--log-level") & value("log-level", log_level)) %
           "set logging level to one of: trace, debug, info, warning, error, "
           "critical, off (default: info)");

  if (!parse(argc, argv, cli)) {
    std::cout << make_man_page(cli, argv[0]) << std::endl;
    return 1;
  }

  init_logger(log_level);

  log_info("🦊 Hello and welcome do the Doge streamer :D 🦊");
  log_info("🦊 Hello and welcome do the Doge streamer :D 🦊");
  log_info("🦊 Hello and welcome do the Doge streamer :D 🦊");
  log_info("🦊 Hello and welcome do the Doge streamer :D 🦊");

  std::thread check_thread;
  if (!state_url.empty()) {
    check_thread = std::thread([state_url] { check_stream_state(state_url); });
    std::this_thread::sleep_for(std::chrono::seconds(3));
    if (stream_state == stream_off) {
      log_warn("exiting because stream is off");
      check_thread.join();
      return 0;
    }
  } else {
    stream_state = stream_on;
  }

  struct sigaction sigIntHandler;

  sigIntHandler.sa_handler = my_handler;
  sigemptyset(&sigIntHandler.sa_mask);
  sigIntHandler.sa_flags = 0;

  sigaction(SIGINT, &sigIntHandler, nullptr);

  stream_video(width, height, fps, bitrate, h264profile, outputServer,
               audio_format, audio_out, video_preset, video_keyframe_group_size,
               cam_idx_start, cam_idx_stop, audio_idx, video_bufsize,
               video_minrate, video_maxrate, video_tune, save_doges);

  if (!state_url.empty()) { check_thread.join(); }
  return 0;
}
