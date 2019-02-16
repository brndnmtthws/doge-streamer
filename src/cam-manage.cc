#include "cam-manage.h"
#include "log.h"

void CamThreads::add(int camID, std::shared_ptr<std::thread> &thread) {
  std::scoped_lock lock(mutex);
  threads[camID] = thread;
}

bool CamThreads::has_cam(int camID) {
  std::scoped_lock lock(mutex);
  return threads.count(camID) > 0;
}

void CamThreads::remove(int camID) {
  std::scoped_lock lock(mutex);
  if (threads.count(camID) > 0) {
    to_delete.push_back(threads[camID]);
    threads.erase(camID);
  }
}

void CamThreads::join_all() {
  std::scoped_lock lock(mutex);
  for (auto element : threads) { element.second->join(); }
}

void CamThreads::join_joinable() {
  std::scoped_lock lock(mutex);
  auto it = to_delete.begin();
  while (it != to_delete.end()) {
    auto ptr = *it;
    if (ptr->joinable()) {
      ptr->join();
      it = to_delete.erase(it);
    } else {
      ++it;
    }
  }
}

Renderer::Renderer(AvCodec *avCodec)
    : avCodec(avCodec),
      last_frame_rendered(std::chrono::high_resolution_clock::now()) {}

Renderer::~Renderer() {}

void Renderer::render(cv::Mat &image, int id) {
  if (image.empty()) return;
  auto now = std::chrono::high_resolution_clock::now();
  const auto min_delay = std::chrono::microseconds(
      std::lround(std::floor((1.0 / (avCodec->get_fps() + 1) * 1000000))));
  std::scoped_lock lock(mutex);
  auto difference = min_delay - (now - last_frame_rendered);
  if (difference > std::chrono::microseconds(0)) {
    log_debug("rendering too fast: sleeping for {} ticks", difference.count());
    std::this_thread::sleep_for(difference);
  }
  last_frame_rendered = std::chrono::high_resolution_clock::now();
  const int stride[] = {static_cast<int>(image.step[0])};
  avCodec->sws_scale_video(&image.data, stride, image.rows);
  avCodec->rescale_video_frame();
  avCodec->write_frames();
}

bool CamMotion::operator==(const CamMotion &b) const {
  return this->camID == b.camID;
}

bool CamMotionCompare::operator()(CamMotion &left, CamMotion &right) {
  return left.amount < right.amount;
}

bool CamSwitcher::empty() {
  std::scoped_lock lock(mutex);
  return pq.empty();
}

int CamSwitcher::get_top() {
  std::scoped_lock lock(mutex);
  if (pq.empty()) return -1;
  return pq.top().camID;
}

void CamSwitcher::mark_active(int camID, double amount) {
  std::scoped_lock lock(mutex);
  if (static_cast<int>(amount) == 0) { amount = ++last_active_amount; }
  CamMotion camMotion(camID, amount);
  pq.remove(camMotion);
  pq.push(camMotion);
}

void CamSwitcher::remove(int camID) {
  std::scoped_lock lock(mutex);
  pq.remove(CamMotion(camID, 0));
}
