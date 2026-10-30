#include "Arduino.h"
#include <ESP_Video.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include "linux/videodev2.h"
#include "esp_video_isp_ioctl.h"
#include "esp_video_isp_pipeline.h"
#include "driver/jpeg_encode.h"

#define CAM_EN_PIN   0   // Pin 11 on Pi Cam = CAM_EN / oscillator enable
#define CAM_IO1_PIN  1   // Reset pin
#define I2C_SDA_PIN  7
#define I2C_SCL_PIN  8

ESPVideoClass video;
ESPVideoCaptureDevClass capture_dev;
const size_t kCaptureBufferCount = 2;

jpeg_encoder_handle_t jpeg_enc = NULL;
uint8_t *jpeg_out_buf = NULL;
size_t jpeg_out_buf_size = 0;

struct __attribute__((packed)) FrameHeader {
  char magic[4];       // 'C','A','M','0'
  uint32_t length;     // payload byte count
  uint16_t width;      // image width
  uint16_t height;     // image height
  uint16_t format;     // 2 = JPEG, 1 = RGB565
};

// Apply ISP White Balance gains to eliminate the raw Bayer green cast
void configure_isp_color() {
  esp_video_isp_wb_t wb = {
    .enable = true,
    .red_gain = 2.4f,   // Boost Red channel
    .blue_gain = 2.0f,  // Boost Blue channel
  };

  struct v4l2_ext_control ctrl = {};
  ctrl.id = V4L2_CID_USER_ESP_ISP_WB;
  ctrl.size = sizeof(wb);
  ctrl.p_u8 = (uint8_t *)&wb;

  struct v4l2_ext_controls ctrls = {};
  ctrls.ctrl_class = V4L2_CTRL_CLASS_USER;
  ctrls.count = 1;
  ctrls.controls = &ctrl;

  // Try setting on /dev/video0 (capture device)
  int fd = ::open(ESP_VIDEO_MIPI_CSI_DEVICE_NAME, O_RDWR);
  if (fd >= 0) {
    if (::ioctl(fd, VIDIOC_S_EXT_CTRLS, &ctrls) == 0) {
      Serial.println("ISP White Balance configured on /dev/video0 (R=2.4, B=2.0)");
    } else {
      Serial.println("Notice: VIDIOC_S_EXT_CTRLS on /dev/video0 failed, trying ISP device");
    }
    ::close(fd);
  }

  // Also try on /dev/video20 (ISP device)
  int isp_fd = ::open(ESP_VIDEO_ISP1_DEVICE_NAME, O_RDWR);
  if (isp_fd >= 0) {
    if (::ioctl(isp_fd, VIDIOC_S_EXT_CTRLS, &ctrls) == 0) {
      Serial.println("ISP White Balance configured on /dev/video20 (R=2.4, B=2.0)");
    }
    ::close(isp_fd);
  }
}

void setup() {
  Serial.begin(2000000);
  delay(2000);

  // 1. Initialize JPEG hardware encoder
  jpeg_encode_engine_cfg_t encode_eng_cfg = {
    .intr_priority = 0,
    .timeout_ms = 100,
  };
  if (jpeg_new_encoder_engine(&encode_eng_cfg, &jpeg_enc) != ESP_OK) {
    Serial.println("FAIL: jpeg_new_encoder_engine");
    return;
  }

  jpeg_encode_memory_alloc_cfg_t out_mem_cfg = {
    .buffer_direction = JPEG_ENC_ALLOC_OUTPUT_BUFFER,
  };
  jpeg_out_buf = (uint8_t *)jpeg_alloc_encoder_mem(512 * 1024, &out_mem_cfg, &jpeg_out_buf_size);
  if (!jpeg_out_buf) {
    Serial.println("FAIL: jpeg_alloc_encoder_mem");
    return;
  }

  // 2. Initialize Camera Video
  ESPVideoCamConfigClass cam_config;
  if (!cam_config.begin(0, I2C_SCL_PIN, I2C_SDA_PIN, 100000, CAM_IO1_PIN, CAM_EN_PIN)) {
    Serial.println("FAIL: cam_config.begin");
    return;
  }

  ESPVideoCSIConfigClass csi_config;
  if (!csi_config.begin(cam_config, false)) {
    Serial.println("FAIL: csi_config.begin");
    return;
  }

  if (!video.begin(csi_config)) {
    Serial.println("FAIL: video.begin(csi_config)");
    return;
  }

  if (!capture_dev.begin(ESP_VIDEO_MIPI_CSI_DEVICE_NAME, kCaptureBufferCount)) {
    Serial.println("FAIL: capture_dev.begin");
    return;
  }

  capture_dev.setFormat(ESP_VIDEO_FORMAT_RGB565);

  // Configure ISP White Balance
  configure_isp_color();

  // Try enabling ISP AGC pipeline
  esp_video_isp_pipeline_set_agc_status(ESP_VIDEO_ISP_PIPELINE_AGC_ENABLE);

  // Set sensor analog gain and exposure for indoor lighting
  // Gain index 34 = ~7.1x analog gain (range 0..41, default was 9=1.68x)
  capture_dev.setSensorGain(34);
  // Exposure 1750 lines (near max frame exposure of 1759 lines)
  capture_dev.setSensorExposure(1750);

  if (!capture_dev.startCapture()) {
    Serial.println("FAIL: startCapture");
    return;
  }
}

void loop() {
  if (!capture_dev.isOpened() || !capture_dev.isCaptureStarted()) {
    delay(100);
    return;
  }

  ESPVideoBufferClass buffer = capture_dev.captureBuffer();
  if (!buffer.valid()) {
    delay(5);
    return;
  }

  // Compress RGB565 to JPEG using hardware encoder
  jpeg_encode_cfg_t enc_config = {
    .height = (uint32_t)buffer.getHeight(),
    .width = (uint32_t)buffer.getWidth(),
    .src_type = JPEG_ENCODE_IN_FORMAT_RGB565,
    .sub_sample = JPEG_DOWN_SAMPLING_YUV422,
    .image_quality = 75,
    .pixel_reverse = false,
  };

  uint32_t jpeg_size = 0;
  esp_err_t ret = jpeg_encoder_process(jpeg_enc, &enc_config, buffer.data(), buffer.size(), jpeg_out_buf, jpeg_out_buf_size, &jpeg_size);

  if (ret == ESP_OK && jpeg_size > 0) {
    FrameHeader hdr;
    hdr.magic[0] = 'C';
    hdr.magic[1] = 'A';
    hdr.magic[2] = 'M';
    hdr.magic[3] = '0';
    hdr.length = jpeg_size;
    hdr.width = (uint16_t)buffer.getWidth();
    hdr.height = (uint16_t)buffer.getHeight();
    hdr.format = 2; // JPEG

    Serial.write((const uint8_t *)&hdr, sizeof(hdr));
    Serial.write(jpeg_out_buf, jpeg_size);
    Serial.flush();
  }
}
