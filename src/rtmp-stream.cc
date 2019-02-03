#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <unistd.h>
#include <atomic>
#include <chrono>
#include <iostream>
#include <random>
#include <thread>
#include <vector>

#include "3rdparty/clipp.h"
#include "cam-manage.h"

using clipp::option, clipp::value;

std::atomic<bool> end_of_stream = false;

cv::VideoCapture get_device(int camID, double width, double height,
                            double fps) {
  cv::VideoCapture cam(camID);
  if (!cam.isOpened()) {
    throw std::runtime_error("Failed to open video capture device");
  }

  cam.set(cv::CAP_PROP_FRAME_WIDTH, width);
  cam.set(cv::CAP_PROP_FRAME_HEIGHT, height);
  cam.set(cv::CAP_PROP_FPS, fps);

  return cam;
}

std::shared_ptr<cv::Mat> image_open(const std::string &fn) {
  const std::string paths[] = {"/opt/doge/artwork/", "./"};
  for (auto p : paths) {
    struct stat stat_result;
    std::string name(p + fn);
    if (stat(name.c_str(), &stat_result) == 0) {
      auto ptr = std::make_shared<cv::Mat>(
          cv::imread(name.c_str(), cv::IMREAD_UNCHANGED));
      if (ptr->empty()) {
        printf("can't read %s\n", fn.c_str());
        exit(1);
      }
      return ptr;
    }
  }
  printf("can't read %s\n", fn.c_str());
  exit(1);
}

void camera_main_loop(const int camID, const double width, const double height,
                      const double fps, Renderer *renderer,
                      CamThreads *cam_threads, CamSwitcher *cam_switcher) {
  printf("entered camera_main_loop %d\n", camID);
  auto cam = get_device(camID, width, height, fps);
  std::vector<uint8_t> imgbuf(height * width * 3 + 16);
  cv::Mat image(height, width, CV_8UC3, imgbuf.data(), width * 3);

  auto bg = cv::createBackgroundSubtractorMOG2(400, 25, true);
  auto kernel = cv::getStructuringElement(cv::MORPH_ELLIPSE, cv::Point(3, 3));

  // Read one frame
  cam >> image;
  if (image.empty()) {
    cam.release();
    cam_threads->remove(camID);
    return;
  }

  std::string dog_fn("dog.png");
  auto dog = image_open(dog_fn);
  std::vector<cv::Mat> dogout;
  cv::split(*dog, dogout);
  cv::bitwise_and(dogout[0], dogout[0], dogout[3]);
  cv::bitwise_and(dogout[1], dogout[1], dogout[3]);
  cv::bitwise_and(dogout[2], dogout[2], dogout[3]);
  cv::merge(dogout, *dog);
  cv::cvtColor(image, image, cv::COLOR_RGB2RGBA);
  cv::Mat dog2 = cv::Mat::zeros(image.size(), image.type());
  dog->copyTo(dog2(cv::Rect(10, 10, dog->cols, dog->rows)));

  cam_switcher->mark_active(camID, 0);

  cv::Mat fgMask, greyFrame;
  bool motion_before = false;
  bool motion_after = false;
  double area_max = 0;
  auto start = std::chrono::system_clock::now();
  std::chrono::duration<double> elapsed_motion(0);
  size_t nFrames = 0;
  int64 t0 = cv::getTickCount();
  int64 processingTime = 0;
  do {
    try {
      cam >> image;
      if (image.empty()) { break; }

      nFrames++;
      if (nFrames % 300 == 0) {
        const int N = 300;
        int64 t1 = cv::getTickCount();
        std::cout << "rendering=" << cam_switcher->get_top()
                  << " camID=" << camID << "  Frames captured: "
                  << cv::format("%5lld", static_cast<long long int>(nFrames))
                  << "  Average FPS: "
                  << cv::format("%2.1f",
                                static_cast<double>(cv::getTickFrequency() * N /
                                                    (t1 - t0)))
                  << "  Average time per frame: "
                  << cv::format("%3.2f ms", static_cast<double>(
                                                (t1 - t0) * 1000.0f /
                                                (N * cv::getTickFrequency())))
                  << "  Average processing time: "
                  << cv::format("%3.2f ms", static_cast<double>(
                                                (processingTime)*1000.0f /
                                                (N * cv::getTickFrequency())))
                  << std::endl;
        t0 = t1;
        processingTime = 0;
      }

      cv::cvtColor(image, greyFrame, cv::COLOR_BGR2GRAY);
      cv::resize(greyFrame, greyFrame, cv::Size(640, 360), 0, 0,
                 cv::INTER_CUBIC);
      cv::GaussianBlur(greyFrame, greyFrame, cv::Size(21, 21), 0);

      bg->apply(greyFrame, fgMask);
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

      auto end = std::chrono::system_clock::now();
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
        std::cout << "camID=" << camID << " motion stopped, lasted "
                  << elapsed_motion.count() << std::endl;

        area_max = 0;
        if (elapsed_motion >= std::chrono::seconds(1)) {
          cam_switcher->mark_active(camID, area_max);
        }
        elapsed_motion = std::chrono::seconds(0);
      }
      motion_before = motion_after;

      if (elapsed_motion >= std::chrono::seconds(1)) {
        cv::putText(image, "Doge Detected", cv::Point(70, 40),
                    cv::FONT_HERSHEY_SIMPLEX, 0.6, cv::Scalar(0, 0, 255, 200),
                    2);

        cv::cvtColor(image, image, cv::COLOR_RGB2RGBA);
        cv::addWeighted(image, 1, dog2, 0.8, 0, image);
        cv::cvtColor(image, image, cv::COLOR_RGBA2RGB);
        cam_switcher->mark_active(camID, area_max);
      }

      if (cam_switcher->get_top() == camID) {
        // Is this camera current the top cam? If yes, render it.
        renderer->render(image, camID);
      }

    } catch (cv::Exception &e) { printf("caught exception: %s\n", e.what()); }
  } while (!end_of_stream);

  printf("end of camera_main_loop for camID=%d\n", camID);

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
  printf("entered bg_main_loop\n");
  std::vector<uint8_t> imgbuf(height * width * 3 + 16);
  cv::Mat image(height, width, CV_8UC3, imgbuf.data(), width * 3);

  std::vector<std::shared_ptr<cv::Mat>> bg_list;
  for (int i = 0; i < 3; ++i) {
    struct stat stat_result;
    std::ostringstream ss;
    ss << "bg" << i + 1 << ".png";
    std::string fn = ss.str();
    auto bg = image_open(fn);
    cv::resize(*bg, *bg, cv::Size(width, height), 0, 0, cv::INTER_CUBIC);
    bg_list.push_back(bg);
  }
  auto bg_changed = std::chrono::system_clock::now();
  auto bg = get_bg(bg_list);
  do {
    try {
      if (cam_switcher->empty()) {
        auto now = std::chrono::system_clock::now();
        if (now - bg_changed > std::chrono::minutes(10)) {
          bg = get_bg(bg_list);
          bg_changed = now;
        }
        renderer->render(*bg, -1);
        std::this_thread::sleep_for(std::chrono::milliseconds(33));
      } else {
        std::this_thread::sleep_for(std::chrono::seconds(2));
      }
    } catch (cv::Exception &e) { printf("caught exception: %s\n", e.what()); }
  } while (!end_of_stream);
}

void stream_video(double width, double height, double fps, int bitrate,
                  const std::string &codec_profile, const std::string &server,
                  const std::string &audio_format, const bool audio_out,
                  const std::string &video_preset,
                  const std::string &video_keyframe_s, int cam_idx_start,
                  int audio_idx_start) {
  AvCodec avCodec(width, height, fps, bitrate, codec_profile, server,
                  audio_format, audio_out, video_preset, video_keyframe_s,
                  audio_idx_start);
  CamThreads cam_threads;
  CamSwitcher cam_switcher;
  Renderer renderer(&avCodec);

  auto bg_image_thread = std::make_shared<std::thread>(
      [width, height, fps, &renderer, &cam_switcher] {
        bg_main_loop(width, height, fps, &renderer, &cam_switcher);
      });

  std::this_thread::sleep_for(std::chrono::seconds(60));

  do {
    try {
      for (int i = cam_idx_start; i < 5; ++i) {
        if (cam_threads.has_cam(i)) { continue; }
        {
          auto cam = get_device(i, width, height, fps);
          cam.release();
          std::this_thread::sleep_for(std::chrono::seconds(10));
        }
        avCodec.init_audio(i);
        auto ptr = std::make_shared<std::thread>(
            [i, width, height, fps, &renderer, &cam_threads, &cam_switcher] {
              camera_main_loop(i, width, height, fps, &renderer, &cam_threads,
                               &cam_switcher);
            });
        cam_threads.add(i, ptr);
        std::this_thread::sleep_for(std::chrono::seconds(10));
      }
    } catch (std::runtime_error &e) {
      printf("caught exception: %s\n", e.what());
    }
    cam_threads.join_joinable();
    std::this_thread::sleep_for(std::chrono::seconds(60));
  } while (!end_of_stream);

  cam_threads.join_all();
}

void my_handler(int s) {
  printf("Caught signal %d\n", s);
  end_of_stream = true;
  exit(1);
}

int main(int argc, char *argv[]) {
  int width = 1920, height = 1080, bitrate = 5000 * 1024;
  double fps = 30;
  std::string h264profile = "high";
  std::string audio_format = "alsa";
  std::string outputServer = "rtmp://localhost/live/stream";
  std::string video_preset = "veryfast";
  std::string video_keyframe_s = "3";
  int cam_idx_start = 0;
  int audio_idx_start = 0;
  bool dump_log = false;
  bool audio_out = false;

  auto cli =
      ((option("-o", "--output") & value("output", outputServer)) %
           "output RTMP server (default: rtmp://localhost/live/stream)",
       (option("-c", "--cam-index") & value("cam_idx_start", cam_idx_start)) %
           "starting cam index (default: 0)",
       (option("-s", "--audio-index") &
        value("audio_idx_start", audio_idx_start)) %
           "starting audio (sound card) index (default: 0)",
       (option("-f", "--fps") & value("fps", fps)) %
           "frames-per-second (default: 30)",
       (option("-w", "--width") & value("width", width)) %
           "video width (default: 1920)",
       (option("-h", "--height") & value("height", height)) %
           "video height (default: 1080)",
       (option("-b", "--bitrate") & value("bitrate", bitrate)) %
           "stream bitrate in kb/s (default: 5120000)",
       (option("-p", "--profile") & value("profile", h264profile)) %
           "H264 codec profile (baseline | high | high10 | high422 | "
           "high444 | main) (default: high)",
       (option("-k", "--keyframe-s") &
        value("video_keyframe_s", video_keyframe_s)) %
           "keyframe interval in secodns (default: 3)",
       (option("-t", "--preset") & value("video_preset", video_preset)) %
           "x264 encoding preset (default: veryfast)",
       (option("-a", "--audio-format") & value("audio-format", audio_format)) %
           "Audio input FFmpeg format (default: alsa)",
       (option("-a", "--audio-out") & value("audio_out", audio_out)) %
           "output audio (default: false)",
       (option("-l", "--log") & value("log", dump_log)) %
           "print debug output (default: false)");

  if (!parse(argc, argv, cli)) {
    std::cout << make_man_page(cli, argv[0]) << std::endl;
    return 1;
  }

  if (dump_log) { av_log_set_level(AV_LOG_DEBUG); }

  struct sigaction sigIntHandler;

  sigIntHandler.sa_handler = my_handler;
  sigemptyset(&sigIntHandler.sa_mask);
  sigIntHandler.sa_flags = 0;

  sigaction(SIGINT, &sigIntHandler, nullptr);

  stream_video(width, height, fps, bitrate, h264profile, outputServer,
               audio_format, audio_out, video_preset, video_keyframe_s,
               cam_idx_start, audio_idx_start);

  return 0;
}
