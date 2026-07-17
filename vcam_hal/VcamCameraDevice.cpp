#include "VcamCameraDevice.h"

namespace vcam {

VcamCameraDevice::VcamCameraDevice(const std::string& cameraId, int facing,
                                     int width, int height)
    : m_cameraId(cameraId), m_facing(facing),
      m_defaultWidth(width), m_defaultHeight(height) {
    LOGI_DEV("VcamCameraDevice created: %s, facing=%d, %dx%d",
             cameraId.c_str(), facing, width, height);
}

VcamCameraDevice::~VcamCameraDevice() {
    LOGI_DEV("VcamCameraDevice destroyed: %s", m_cameraId.c_str());
}

} // namespace vcam
