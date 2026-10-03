/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Xenia Canary. All rights reserved.                          *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

// Kinect v1 camera over libusb. The protocol follows libfreenect
// (src/flags.c, src/cameras.c, Apache-2.0).
//
// UsbDk detaches the camera from its Windows driver only while it's open,
// WinUSB needs the camera bound to it.

#include "xenia/hid/kinect/usb_camera.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <mutex>
#include <vector>

#include "xenia/base/logging.h"
#include "xenia/base/mutex.h"
#include "xenia/base/threading.h"

#include "third_party/libusb/libusb/libusb.h"

namespace xe {
namespace hid {
namespace kinect {

namespace {

class UsbCameraWin;

void CloseOpenCameras();
void UnregisterOpenCamera(UsbCameraWin* camera);

constexpr uint16_t kVendorId = 0x045E;
constexpr uint16_t kCameraProductId = 0x02AE;
constexpr unsigned char kColorEndpoint = 0x81;
constexpr unsigned char kDepthEndpoint = 0x82;
constexpr uint8_t kDepthStreamFlag = 0x70;
constexpr uint8_t kColorStreamFlag = 0x80;
constexpr int kTransfersPerStream = 8;
constexpr int kPacketsPerTransfer = 32;
constexpr uint32_t kMaxTransferWarnings = 8;
constexpr uint8_t kVendorRequestIn =
    LIBUSB_ENDPOINT_IN | LIBUSB_REQUEST_TYPE_VENDOR;
constexpr uint8_t kVendorRequestOut =
    LIBUSB_ENDPOINT_OUT | LIBUSB_REQUEST_TYPE_VENDOR;
// Host-side limits for USB requests, replies and closing.
constexpr unsigned int kControlTimeoutMs = 1000;
constexpr auto kReplyTimeout = std::chrono::seconds(2);
constexpr auto kCloseTimeout = std::chrono::seconds(2);
constexpr auto kReplyPollInterval = std::chrono::milliseconds(1);
constexpr auto kCloseWaitInterval = std::chrono::milliseconds(10);
constexpr long kEventTimeoutUs = 50000;

// Command and reply buffers are bounded by the control transfer sizes.
constexpr size_t kMaxCommandBytes = 0x400;
constexpr size_t kMaxReplyBytes = 0x200;
// Stale replies are read until the camera has none left, up to this many.
constexpr int kMaxDrainedReplies = 16;
constexpr unsigned int kDrainTimeoutMs = 100;

// Packets and replies from the camera start with 'R', 'B'; commands sent to it
// with 'G', 'M'.
constexpr uint8_t kCameraMagic[2] = {'R', 'B'};
constexpr uint8_t kHostMagic[2] = {'G', 'M'};
// Added to the stream flag in the first and last packet of a frame.
constexpr uint8_t kPacketFrameStart = 1;
constexpr uint8_t kPacketFrameEnd = 5;

// Camera commands.
constexpr uint16_t kCommandWriteRegister = 0x03;
constexpr uint16_t kCommandReadFixedParameters = 0x04;
constexpr uint16_t kCommandReadAlgorithmParameters = 0x16;
// Zero plane values in the fixed parameters: four floats.
constexpr size_t kZeroPlaneOffset = 94;

// Opens the first device with the product ID, logging why it can't be opened.
libusb_device_handle* OpenDevice(libusb_context* context, uint16_t product_id) {
  libusb_device** devices;
  const ssize_t count = libusb_get_device_list(context, &devices);
  if (count < 0) {
    XELOGW("Kinect: cannot list USB devices ({})",
           libusb_error_name(static_cast<int>(count)));
    return nullptr;
  }
  libusb_device_handle* handle = nullptr;
  bool found = false;
  for (ssize_t i = 0; i < count && !handle; ++i) {
    libusb_device_descriptor descriptor;
    if (libusb_get_device_descriptor(devices[i], &descriptor) < 0 ||
        descriptor.idVendor != kVendorId ||
        descriptor.idProduct != product_id) {
      continue;
    }
    found = true;
    const int result = libusb_open(devices[i], &handle);
    if (result < 0) {
      XELOGW("Kinect: cannot open USB device {:04X} ({})", product_id,
             libusb_error_name(result));
      handle = nullptr;
    }
  }
  libusb_free_device_list(devices, 1);
  if (!found) {
    XELOGW("Kinect: USB device {:04X} not found", product_id);
  }
  return handle;
}

#pragma pack(push, 1)
struct CommandHeader {
  uint8_t magic[2];
  uint16_t length;
  uint16_t command;
  uint16_t tag;
};
static_assert(sizeof(CommandHeader) == 8);

struct PacketHeader {
  uint8_t magic[2];
  uint8_t pad;
  uint8_t flag;
  uint8_t unknown1;
  uint8_t sequence;
  uint8_t unknown2;
  uint8_t unknown3;
  uint32_t timestamp;
};
static_assert(sizeof(PacketHeader) == 12);
#pragma pack(pop)

// Assembles frames from isochronous packets of one stream.
class FrameAssembler {
 public:
  FrameAssembler(uint8_t flag, size_t frame_bytes,
                 const std::function<void()>* on_frame)
      : flag_(flag), frame_bytes_(frame_bytes), on_frame_(on_frame) {
    building_.reserve(frame_bytes);
  }

  void AddPacket(const uint8_t* packet, size_t length) {
    if (length < sizeof(PacketHeader)) {
      return;
    }
    auto header = reinterpret_cast<const PacketHeader*>(packet);
    if (std::memcmp(header->magic, kCameraMagic, sizeof(kCameraMagic)) != 0) {
      return;
    }
    if (header->flag == (flag_ | kPacketFrameStart)) {
      building_.clear();
      synced_ = true;
    } else if (!synced_ ||
               header->sequence != static_cast<uint8_t>(sequence_ + 1)) {
      synced_ = false;
      return;
    }
    sequence_ = header->sequence;
    building_.insert(building_.end(), packet + sizeof(PacketHeader),
                     packet + length);
    if (building_.size() > frame_bytes_) {
      // Drops a frame that is longer than expected.
      building_.clear();
      synced_ = false;
      return;
    }
    if (header->flag == (flag_ | kPacketFrameEnd)) {
      synced_ = false;
      if (building_.size() == frame_bytes_) {
        {
          std::lock_guard<xe_mutex> lock(mutex_);
          latest_.swap(building_);
          ++latest_sequence_;
        }
        if (*on_frame_) {
          (*on_frame_)();
        }
      }
    }
  }

  uint32_t Copy(uint32_t after, std::vector<uint8_t>& out) {
    std::lock_guard<xe_mutex> lock(mutex_);
    if (latest_sequence_ == after || latest_.empty()) {
      return 0;
    }
    out = latest_;
    return latest_sequence_;
  }

 private:
  const uint8_t flag_;
  const size_t frame_bytes_;
  const std::function<void()>* const on_frame_;
  bool synced_ = false;
  uint8_t sequence_ = 0;
  std::vector<uint8_t> building_;
  xe_mutex mutex_;
  std::vector<uint8_t> latest_;
  uint32_t latest_sequence_ = 0;
};

class UsbCameraWin : public UsbCamera {
 public:
  ~UsbCameraWin() override {
    UnregisterOpenCamera(this);
    Close();
  }

  bool Initialize(bool use_usbdk, std::function<void()> on_frame) {
    on_frame_ = std::move(on_frame);
    if (libusb_init(&context_) < 0) {
      context_ = nullptr;
      return false;
    }
    if (use_usbdk) {
      const int result = libusb_set_option(context_, LIBUSB_OPTION_USE_USBDK);
      if (result < 0) {
        XELOGW("Kinect: UsbDk is not available ({}), using WinUSB",
               libusb_error_name(result));
      }
    }
    handle_ = OpenDevice(context_, kCameraProductId);
    if (!handle_) {
      return false;
    }
    if (libusb_claim_interface(handle_, 0) < 0) {
      XELOGE("Kinect: cannot claim the camera interface");
      return false;
    }
    DrainReplies();
    if (!ReadCalibration()) {
      XELOGE("Kinect: cannot read camera calibration");
      return false;
    }

    running_ = true;
    event_thread_ =
        xe::threading::Thread::Create({}, [this]() { EventLoop(); });
    if (!event_thread_) {
      XELOGE("Kinect: cannot create the USB event thread");
      return false;
    }
    event_thread_->set_name("Kinect USB Events");

    if (!StartIso(kDepthEndpoint, depth_) ||
        !WriteRegisters({{0x105, 0x00},     // Projector auto-cycle off.
                         {0x06, 0x00},      // Stop depth.
                         {0x12, 0x03},      // 11-bit packed depth.
                         {0x13, 0x01},      // 640x480.
                         {0x14, 0x1E},      // 30 fps.
                         {0x06, 0x02},      // Start depth.
                         {0x17, 0x00}})) {  // Depth mirroring off.
      XELOGE("Kinect: cannot start the depth stream");
      return false;
    }
    if (!StartIso(kColorEndpoint, color_) ||
        !WriteRegisters({{0x0C, 0x00},      // Bayer.
                         {0x0D, 0x01},      // 640x480.
                         {0x0E, 0x1E},      // 30 fps.
                         {0x05, 0x01},      // Start color.
                         {0x47, 0x00}})) {  // Color mirroring off.
      XELOGE("Kinect: cannot start the color stream");
      return false;
    }
    XELOGI("Kinect: camera streaming via {}", use_usbdk ? "UsbDk" : "WinUSB");
    return true;
  }

  const UsbCameraCalibration& calibration() const override {
    return calibration_;
  }

  uint32_t CopyDepthFrame(uint32_t after,
                          std::vector<uint8_t>& packed_depth) override {
    return depth_frames_.Copy(after, packed_depth);
  }

  uint32_t CopyColorFrame(uint32_t after,
                          std::vector<uint8_t>& bayer) override {
    return color_frames_.Copy(after, bayer);
  }

 private:
  struct IsoStream {
    std::vector<libusb_transfer*> transfers;
    std::vector<std::unique_ptr<uint8_t[]>> buffers;
    uint32_t failed_transfers = 0;
  };

  void Close() {
    std::lock_guard<xe_mutex> lock(close_mutex_);
    if (handle_ && running_) {
      WriteRegisters({{0x06, 0x00}, {0x05, 0x00}});
    }
    stopping_ = true;
    for (IsoStream* stream : {&depth_, &color_}) {
      for (libusb_transfer* transfer : stream->transfers) {
        libusb_cancel_transfer(transfer);
      }
    }
    // Keep handling events until every transfer has come back.
    const auto deadline = std::chrono::steady_clock::now() + kCloseTimeout;
    while (in_flight_ > 0 && std::chrono::steady_clock::now() < deadline) {
      xe::threading::Sleep(kCloseWaitInterval);
    }
    running_ = false;
    if (event_thread_) {
      xe::threading::Wait(event_thread_.get(), false);
      event_thread_.reset();
    }
    if (in_flight_ > 0) {
      // Freeing a transfer that libusb still owns is unsafe, so leak them.
      XELOGW("Kinect: leaking {} USB transfers still in flight",
             in_flight_.load());
      for (IsoStream* stream : {&depth_, &color_}) {
        for (auto& buffer : stream->buffers) {
          static_cast<void>(buffer.release());
        }
      }
    } else {
      for (IsoStream* stream : {&depth_, &color_}) {
        for (libusb_transfer* transfer : stream->transfers) {
          libusb_free_transfer(transfer);
        }
      }
    }
    for (IsoStream* stream : {&depth_, &color_}) {
      stream->transfers.clear();
      stream->buffers.clear();
    }
    if (handle_) {
      libusb_release_interface(handle_, 0);
      libusb_close(handle_);
      handle_ = nullptr;
    }
    if (context_) {
      libusb_exit(context_);
      context_ = nullptr;
    }
  }

  void EventLoop() {
    timeval timeout = {0, kEventTimeoutUs};
    while (running_) {
      libusb_handle_events_timeout(context_, &timeout);
    }
  }

  // Throws away replies left over from an earlier session.
  void DrainReplies() {
    uint8_t reply[kMaxReplyBytes];
    for (int i = 0; i < kMaxDrainedReplies; ++i) {
      if (libusb_control_transfer(handle_, kVendorRequestIn, 0, 0, 0, reply,
                                  sizeof(reply), kDrainTimeoutMs) <= 0) {
        break;
      }
    }
  }

  // Returns the reply payload size, or -1.
  int SendCommand(uint16_t command, const void* payload, size_t payload_bytes,
                  uint8_t* reply, size_t reply_capacity) {
    uint8_t out[kMaxCommandBytes] = {};
    auto header = reinterpret_cast<CommandHeader*>(out);
    std::memcpy(header->magic, kHostMagic, sizeof(kHostMagic));
    header->length = static_cast<uint16_t>(payload_bytes / 2);
    header->command = command;
    header->tag = tag_;
    std::memcpy(out + sizeof(CommandHeader), payload, payload_bytes);
    if (libusb_control_transfer(
            handle_, kVendorRequestOut, 0, 0, 0, out,
            static_cast<uint16_t>(sizeof(CommandHeader) + payload_bytes),
            kControlTimeoutMs) < 0) {
      return -1;
    }

    const uint16_t tag = tag_++;
    uint8_t in[kMaxReplyBytes];
    const auto deadline = std::chrono::steady_clock::now() + kReplyTimeout;
    while (std::chrono::steady_clock::now() < deadline) {
      const int length =
          libusb_control_transfer(handle_, kVendorRequestIn, 0, 0, 0, in,
                                  sizeof(in), kControlTimeoutMs);
      auto reply_header = reinterpret_cast<const CommandHeader*>(in);
      if (length >= static_cast<int>(sizeof(CommandHeader)) &&
          length != sizeof(in) &&
          std::memcmp(reply_header->magic, kCameraMagic,
                      sizeof(kCameraMagic)) == 0 &&
          reply_header->tag == tag && reply_header->command == command) {
        const size_t reply_bytes = length - sizeof(CommandHeader);
        std::memcpy(reply, in + sizeof(CommandHeader),
                    std::min(reply_bytes, reply_capacity));
        return static_cast<int>(reply_bytes);
      }
      xe::threading::Sleep(kReplyPollInterval);
    }
    return -1;
  }

  bool WriteRegisters(
      std::initializer_list<std::pair<uint16_t, uint16_t>> writes) {
    for (const auto& [reg, value] : writes) {
      const uint16_t payload[2] = {reg, value};
      uint8_t reply[16];
      if (SendCommand(kCommandWriteRegister, payload, sizeof(payload), reply,
                      sizeof(reply)) < 0) {
        XELOGE("Kinect: register {:04X} write failed", reg);
        return false;
      }
    }
    return true;
  }

  bool ReadCalibration() {
    uint8_t reply[kMaxReplyBytes];
    const uint16_t fixed_payload[5] = {};
    int length = SendCommand(kCommandReadFixedParameters, fixed_payload,
                             sizeof(fixed_payload), reply, sizeof(reply));
    if (length < static_cast<int>(kZeroPlaneOffset + 4 * sizeof(float))) {
      return false;
    }
    const uint8_t* zero_plane = reply + kZeroPlaneOffset;
    std::memcpy(&calibration_.dcmos_emitter_distance, zero_plane, 4);
    std::memcpy(&calibration_.dcmos_rcmos_distance, zero_plane + 4, 4);
    std::memcpy(&calibration_.reference_distance, zero_plane + 8, 4);
    std::memcpy(&calibration_.reference_pixel_size, zero_plane + 12, 4);

    // Algorithm parameters for 640x480 at 30 fps, after a 2-byte prefix.
    const uint16_t registration_payload[5] = {0x40, 0, 1, 30, 0};
    length = SendCommand(kCommandReadAlgorithmParameters, registration_payload,
                         sizeof(registration_payload), reply, sizeof(reply));
    if (length != 2 + static_cast<int>(calibration_.registration.size())) {
      return false;
    }
    std::memcpy(calibration_.registration.data(), reply + 2,
                calibration_.registration.size());

    const uint16_t shift_payload[5] = {0x00, 0, 1, 30, 0};
    length = SendCommand(kCommandReadAlgorithmParameters, shift_payload,
                         sizeof(shift_payload), reply, sizeof(reply));
    if (length < 4) {
      return false;
    }
    std::memcpy(&calibration_.const_shift, reply + 2, 2);
    return true;
  }

  static void LIBUSB_CALL OnTransfer(libusb_transfer* transfer) {
    auto self = static_cast<UsbCameraWin*>(transfer->user_data);
    const bool is_depth = transfer->endpoint == kDepthEndpoint;
    FrameAssembler* frames =
        is_depth ? &self->depth_frames_ : &self->color_frames_;
    IsoStream& stream = is_depth ? self->depth_ : self->color_;
    if (transfer->status != LIBUSB_TRANSFER_COMPLETED && !self->stopping_ &&
        stream.failed_transfers++ < kMaxTransferWarnings) {
      XELOGW("Kinect: {} transfer status {}", is_depth ? "depth" : "color",
             static_cast<int>(transfer->status));
    }
    if (transfer->status == LIBUSB_TRANSFER_COMPLETED) {
      const uint8_t* buffer = transfer->buffer;
      for (int i = 0; i < transfer->num_iso_packets; ++i) {
        const libusb_iso_packet_descriptor& packet =
            transfer->iso_packet_desc[i];
        if (packet.status == LIBUSB_TRANSFER_COMPLETED) {
          frames->AddPacket(buffer, packet.actual_length);
        }
        buffer += packet.length;
      }
    }
    if (self->stopping_ || transfer->status == LIBUSB_TRANSFER_NO_DEVICE) {
      --self->in_flight_;
      return;
    }
    const int result = libusb_submit_transfer(transfer);
    if (result < 0) {
      --self->in_flight_;
      if (stream.failed_transfers++ < kMaxTransferWarnings) {
        XELOGW("Kinect: cannot resubmit a {} transfer ({})",
               is_depth ? "depth" : "color", libusb_error_name(result));
      }
    }
  }

  bool StartIso(unsigned char endpoint, IsoStream& stream) {
    const int packet_size =
        libusb_get_max_iso_packet_size(libusb_get_device(handle_), endpoint);
    if (packet_size <= 0) {
      return false;
    }
    for (int i = 0; i < kTransfersPerStream; ++i) {
      libusb_transfer* transfer = libusb_alloc_transfer(kPacketsPerTransfer);
      auto buffer =
          std::make_unique<uint8_t[]>(packet_size * kPacketsPerTransfer);
      libusb_fill_iso_transfer(transfer, handle_, endpoint, buffer.get(),
                               packet_size * kPacketsPerTransfer,
                               kPacketsPerTransfer, OnTransfer, this, 0);
      libusb_set_iso_packet_lengths(transfer, packet_size);
      stream.transfers.push_back(transfer);
      stream.buffers.push_back(std::move(buffer));
      ++in_flight_;
      if (libusb_submit_transfer(transfer) < 0) {
        --in_flight_;
        return false;
      }
    }
    return true;
  }

  libusb_context* context_ = nullptr;
  libusb_device_handle* handle_ = nullptr;
  uint16_t tag_ = 0;
  UsbCameraCalibration calibration_;
  std::function<void()> on_frame_;
  FrameAssembler depth_frames_{kDepthStreamFlag, kDepthFrameBytes, &on_frame_};
  FrameAssembler color_frames_{kColorStreamFlag, kColorFrameBytes, &on_frame_};
  IsoStream depth_;
  IsoStream color_;
  std::atomic<bool> running_ = false;
  std::atomic<bool> stopping_ = false;
  std::atomic<int> in_flight_ = 0;
  std::unique_ptr<xe::threading::Thread> event_thread_;
  xe_mutex close_mutex_;

  friend void CloseOpenCameras();
};

// The emulator exits via std::quick_exit, which skips destructors; without
// this UsbDk keeps the camera detached from its driver.
xe_mutex g_open_cameras_mutex;
std::vector<UsbCameraWin*> g_open_cameras;

void CloseOpenCameras() {
  std::lock_guard<xe_mutex> lock(g_open_cameras_mutex);
  for (UsbCameraWin* camera : g_open_cameras) {
    camera->Close();
  }
}

void RegisterOpenCamera(UsbCameraWin* camera) {
  static std::once_flag handler_registered;
  std::call_once(handler_registered,
                 []() { std::at_quick_exit(CloseOpenCameras); });
  std::lock_guard<xe_mutex> lock(g_open_cameras_mutex);
  g_open_cameras.push_back(camera);
}

void UnregisterOpenCamera(UsbCameraWin* camera) {
  std::lock_guard<xe_mutex> lock(g_open_cameras_mutex);
  g_open_cameras.erase(
      std::remove(g_open_cameras.begin(), g_open_cameras.end(), camera),
      g_open_cameras.end());
}

}  // namespace

std::unique_ptr<UsbCamera> UsbCamera::Open(bool use_usbdk,
                                           std::function<void()> on_frame) {
  auto camera = std::make_unique<UsbCameraWin>();
  if (!camera->Initialize(use_usbdk, std::move(on_frame))) {
    return nullptr;
  }
  RegisterOpenCamera(camera.get());
  return camera;
}

}  // namespace kinect
}  // namespace hid
}  // namespace xe
