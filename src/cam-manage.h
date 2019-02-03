#ifndef cam_manage_h
#define cam_manage_h

#include <mutex>
#include <queue>
#include <thread>
#include <unordered_map>

#include <opencv2/highgui.hpp>
#include <opencv2/imgproc.hpp>
#include <opencv2/video.hpp>
#include <opencv2/video/background_segm.hpp>

#include "avcodec.h"

class CamThreads {
  std::mutex mutex;
  std::unordered_map<int, std::shared_ptr<std::thread>> threads;
  std::vector<std::shared_ptr<std::thread>> to_delete;

 public:
  CamThreads() {}
  void add(int camID, std::shared_ptr<std::thread> &thread);
  bool has_cam(int camID);
  void remove(int camID);
  void join_all();
  void join_joinable();
};

class Renderer {
  std::mutex mutex;
  AvCodec &avCodec;

 public:
  Renderer(AvCodec &avCodec);
  ~Renderer();
  void render(cv::Mat &image, int id);
};

template <typename T, typename Comp>
class custom_priority_queue
    : public std::priority_queue<T, std::vector<T>, Comp> {
 public:
  bool remove(const T &value) {
    auto it = std::find(this->c.begin(), this->c.end(), value);
    if (it != this->c.end()) {
      this->c.erase(it);
      std::make_heap(this->c.begin(), this->c.end(), this->comp);
      return true;
    } else {
      return false;
    }
  }
};

class CamMotion {
  int camID;
  double amount;
  friend class CamSwitcher;
  friend class CamMotionCompare;

 public:
  CamMotion(int camID, double amount) : camID(camID), amount(amount) {}
  bool operator==(const CamMotion &b) const;
};

class CamMotionCompare {
 public:
  bool operator()(CamMotion &left, CamMotion &right);
};

class CamSwitcher {
  std::mutex mutex;
  custom_priority_queue<CamMotion, CamMotionCompare> pq;
  int last_active_amount;

 public:
  CamSwitcher() : last_active_amount(0) {}

  int get_top();
  bool empty();

  void mark_active(int camID, double amount);
  void remove(int camID);
};

#endif /* cam_manage_h */
