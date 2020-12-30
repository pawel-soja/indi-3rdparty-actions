/*
 Raspberry Pi High Quality Camera CCD Driver for Indi.
 Copyright (C) 2020 Lars Berntzon (lars.berntzon@cecilia-data.se).
 All rights reserved.

 This library is free software; you can redistribute it and/or
 modify it under the terms of the GNU Lesser General Public
 License as published by the Free Software Foundation; either
 version 2.1 of the License, or (at your option) any later version.

 This library is distributed in the hope that it will be useful,
 but WITHOUT ANY WARRANTY; without even the implied warranty of
 MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 Lesser General Public License for more details.

 You should have received a copy of the GNU Lesser General Public
 License along with this library; if not, write to the Free Software
 Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
 */

#include <stdio.h>
#include <mmal_logging.h>
#include <mmal_default_components.h>
#include <util/mmal_util.h>
#include <util/mmal_util_params.h>
#include <bcm_host.h>

#include "mmalcamera.h"
#include "mmalexception.h"
#include "mmalencoder.h"

MMALCamera::MMALCamera(int n) : MMALComponent(MMAL_COMPONENT_DEFAULT_CAMERA), cameraNum(n)
{
    MMAL_STATUS_T status;

    MMAL_PARAMETER_INT32_T camera_num_param = {{MMAL_PARAMETER_CAMERA_NUM, sizeof(camera_num_param)}, cameraNum};
    status = mmal_port_parameter_set(component->control, &camera_num_param.hdr);
    MMALException::throw_if(status, "Could not select camera");
    MMALException::throw_if(component->output_num == 0, "Camera doesn't have output ports");

    status = mmal_port_parameter_set_uint32(component->control, MMAL_PARAMETER_CAMERA_CUSTOM_SENSOR_CONFIG, 0);
    MMALException::throw_if(status, "Could not set sensor mode");

    // Enable the camera, and tell it its control callback function
    enable_port_with_callback(component->control);

    get_sensor_info();

    //  set up the camera configuration
    {
        MMAL_PARAMETER_CAMERA_CONFIG_T cam_config;
        cam_config.hdr.id = MMAL_PARAMETER_CAMERA_CONFIG;
        cam_config.hdr.size = sizeof cam_config;
        cam_config.max_stills_w = width;
        cam_config.max_stills_h = height;
        cam_config.stills_yuv422 = 0;
        cam_config.one_shot_stills = 1;
        cam_config.max_preview_video_w = 1024;  // Must really be set, even though we are not interested in a preview.
        cam_config.max_preview_video_h = 768;   // -''-
        cam_config.num_preview_video_frames = 1;
        cam_config.stills_capture_circular_buffer_height = 0;
        cam_config.fast_preview_resume = 0;
        cam_config.use_stc_timestamp = MMAL_PARAM_TIMESTAMP_MODE_RESET_STC;

        status = mmal_port_parameter_set(component->control, &cam_config.hdr);
        MMALException::throw_if(status, "Failed to set camera config");
    }

    set_capture_port_format();

    // Save cameras default FPS range.
    MMAL_PARAMETER_FPS_RANGE_T fps_range = {{MMAL_PARAMETER_FPS_RANGE, sizeof(fps_range)}, {0, 0}, {0, 0}};
    status = mmal_port_parameter_get(component->output[MMAL_CAMERA_CAPTURE_PORT], &fps_range.hdr);
    MMALException::throw_if(status, "Failed to get FPS range");

    fps_low = fps_range.fps_low;
    fps_high = fps_range.fps_high;

    fprintf(stderr, "MMALCamera: fps_low=%d/%d, fps_high=%d/%d\n", fps_low.num, fps_low.den, fps_high.num, fps_high.den);
}

MMALCamera::~MMALCamera()
{
    MMAL_STATUS_T status;

    if (component->output[MMAL_CAMERA_CAPTURE_PORT]->is_enabled) {
        status = mmal_port_disable(component->output[MMAL_CAMERA_CAPTURE_PORT]);
        MMALException::throw_if(status, "Failed to disable capture port");
    }

    if(component->control->is_enabled) {
        status = mmal_port_disable(component->control);
        MMALException::throw_if(status, "Failed to disable control port");
    }
}

/**
 * @brief MMALCamera::capture Main exposure method.
 *
 * @return MMAL_SUCCESS if all OK, something else otherwise
 *
 */
int MMALCamera::capture()
{
    int exit_code = 0;
    MMAL_STATUS_T status = MMAL_SUCCESS;

    status = mmal_component_enable(component);
    MMALException::throw_if(status, "camera component couldn't be enabled");

    // Start capturing.
    fprintf(stderr, "%s: Starting capture with speed %d\n", __FUNCTION__, shutter_speed);
    status = mmal_port_parameter_set_boolean(component->output[MMAL_CAMERA_CAPTURE_PORT], MMAL_PARAMETER_CAPTURE, 1);
    MMALException::throw_if(status, "Failed to start capture");

    return exit_code;
}

void MMALCamera::abort()
{
    MMAL_STATUS_T status = MMAL_SUCCESS;
    status = mmal_port_parameter_set_boolean(component->output[MMAL_CAMERA_CAPTURE_PORT], MMAL_PARAMETER_CAPTURE, 0);
    MMALException::throw_if(status, "Failed to abort capture");

    status = mmal_component_disable(component);
    MMALException::throw_if(status, "camera component couldn't be disabled");

    fprintf(stderr, "%s: Capture aborted\n", __FUNCTION__);
}

void MMALCamera::set_camera_parameters()
{
    MMAL_PARAMETER_AWBMODE_T awb = {{MMAL_PARAMETER_AWB_MODE,sizeof awb}, MMAL_PARAM_AWBMODE_AUTO};
    MMALException::throw_if(mmal_port_parameter_set(component->control, &awb.hdr), "Failed to set AWB mode");

    MMALException::throw_if(mmal_port_parameter_set_rational(component->control, MMAL_PARAMETER_SATURATION, MMAL_RATIONAL_T {10, 0}), "Failed to set saturation");

    MMALException::throw_if(mmal_port_parameter_set_rational(component->control, MMAL_PARAMETER_DIGITAL_GAIN, MMAL_RATIONAL_T {1, 1}), "Failed to set digital gain");

#ifdef USE_ISO
    MMALException::throw_if(mmal_port_parameter_set_uint32(component->control, MMAL_PARAMETER_ISO, iso), "Failed to set ISO");
    fprintf(stderr, "MMALCamera: ISO set to %d\n", iso);
#endif

    MMALException::throw_if(mmal_port_parameter_set_rational(component->control, MMAL_PARAMETER_BRIGHTNESS, MMAL_RATIONAL_T{50, 100}), "Failed to set brightness");

    MMAL_PARAMETER_EXPOSUREMODE_T exposure = {{MMAL_PARAMETER_EXPOSURE_MODE, sizeof exposure}, MMAL_PARAM_EXPOSUREMODE_OFF};
    MMALException::throw_if(mmal_port_parameter_set(component->control, &exposure.hdr), "Failed to set exposure mode");

    MMAL_PARAMETER_INPUT_CROP_T crop = {{MMAL_PARAMETER_INPUT_CROP, sizeof crop}, {0, 0, 0x1000, 0x1000}};
    MMALException::throw_if(mmal_port_parameter_set(component->control, &crop.hdr), "Failed to set ROI");


    component->port[MMAL_CAMERA_CAPTURE_PORT]->buffer_size = component->port[MMAL_CAMERA_CAPTURE_PORT]->buffer_size_recommended;

    MMALException::throw_if(mmal_port_parameter_set_boolean(component->output[MMAL_CAMERA_VIDEO_PORT], MMAL_PARAMETER_ZERO_COPY, MMAL_TRUE),
                            "Failed to turn on zero-copy for video port");

    MMALException::throw_if(mmal_port_parameter_set_boolean(component->output[MMAL_CAMERA_CAPTURE_PORT], MMAL_PARAMETER_ENABLE_RAW_CAPTURE, 1), "Failed to set raw capture");

    MMALException::throw_if(mmal_port_parameter_set_uint32(component->control, MMAL_PARAMETER_CAPTURE_STATS_PASS, MMAL_TRUE), "Failed to set CAPTURE_STATS_PASS");

    // Exposure time.
    MMALException::throw_if(mmal_port_parameter_set_uint32(component->control, MMAL_PARAMETER_SHUTTER_SPEED, shutter_speed), "Failed to set shutter speed");
    uint32_t actual_shutter_speed;
    actual_shutter_speed = get_shutter_speed();
    if (actual_shutter_speed < shutter_speed - 100000 || actual_shutter_speed > shutter_speed + 100000) {
        fprintf(stderr, "MMALCamera: Failed to set shutter speed, requested %d but actual value is %d\n", shutter_speed, actual_shutter_speed); 
    }

    // Exposure ranges
    MMAL_RATIONAL_T low, high;
    if(shutter_speed > 6000000) {
        low = {5, 1000};
        high = {166, 1000};
    }
    else if(shutter_speed > 1000000) {
        low = {167, 1000};
        high = {999, 1000};
    }
    else {
        low = fps_low;
        high = fps_high;
    }
    fprintf(stderr, "MMALCamera: seting fps range %d/%d -> %d/%d\n", low.num, low.den, high.num, high.den);
    MMAL_PARAMETER_FPS_RANGE_T fps_range = {{MMAL_PARAMETER_FPS_RANGE, sizeof(fps_range)}, low, high};
    MMALException::throw_if(mmal_port_parameter_set(component->output[MMAL_CAMERA_CAPTURE_PORT], &fps_range.hdr), "Failed to set FPS range");
    MMALException::throw_if(mmal_port_parameter_get(component->output[MMAL_CAMERA_CAPTURE_PORT], &fps_range.hdr), "Failed to get FPS range");
    if (fps_range.fps_low.num != low.num || fps_range.fps_low.den != low.den || 
        fps_range.fps_high.num != high.num || fps_range.fps_high.den != high.den) {
        fprintf(stderr, "%s: failed to set fps ranges: low range is %d/%d, high range is %d/%d\n", __FUNCTION__,
                fps_range.fps_low.num, fps_range.fps_low.den, fps_range.fps_high.num, fps_range.fps_high.den);
    }

    // Gain settings
    MMALException::throw_if(mmal_port_parameter_set_rational(component->control, MMAL_PARAMETER_ANALOG_GAIN, MMAL_RATIONAL_T {static_cast<int32_t>(gain * 65536), 65536}),
                            "Failed to set analog gain");
    fprintf(stderr, "MMALCamera: Gain set to %d/%d\n", (int)(gain*65536), 65536);

}

uint32_t MMALCamera::get_shutter_speed()
{
    uint32_t actual_shutter_speed;
    MMALException::throw_if(mmal_port_parameter_get_uint32(component->control, MMAL_PARAMETER_SHUTTER_SPEED, &actual_shutter_speed), "Failed to get shutter speed");
    return actual_shutter_speed;
}

/**
 * @brief MMALCamera::set_capture_port_format Set format for the output capture port.
 */
void MMALCamera::set_capture_port_format()
{
    // Set our stills format on the stills (for encoder) port
    MMAL_ES_FORMAT_T *format {component->output[MMAL_CAMERA_CAPTURE_PORT]->format};

    // Special case for raw format.
    format->encoding = MMAL_ENCODING_OPAQUE; format->encoding_variant = 0;

    if (!mmal_util_rgb_order_fixed(component->output[MMAL_CAMERA_CAPTURE_PORT]))
    {
       if (format->encoding == MMAL_ENCODING_RGB24)
          format->encoding = MMAL_ENCODING_BGR24;
       else if (format->encoding == MMAL_ENCODING_BGR24)
          format->encoding = MMAL_ENCODING_RGB24;
    }

    format->encoding_variant = 0;
    format->es->video.width = width;
    format->es->video.height = height;
    format->es->video.crop.x = 0;
    format->es->video.crop.y = 0;
    format->es->video.crop.width = static_cast<int32_t>(width);
    format->es->video.crop.height = static_cast<int32_t>(height);
    format->es->video.frame_rate.num = 0;
    format->es->video.frame_rate.den = 1;
    format->es->video.par.num = 1;
    format->es->video.par.den = 1;

    MMALException::throw_if(mmal_port_format_commit(component->output[MMAL_CAMERA_CAPTURE_PORT]), "camera capture port format couldn't be set");
}

/**
 * @brief MMALCamera::get_sensor_size gets default size for camrea.
 * @param camera_num
 * @param camera_name
 * @param len Length of camera_name string
 * @param width
 * @param height
 */
void MMALCamera::get_sensor_info()
{
   MMAL_COMPONENT_T *camera_info;
   MMAL_STATUS_T status;

   // Try to get the camera name and maximum supported resolution
   status = mmal_component_create(MMAL_COMPONENT_DEFAULT_CAMERA_INFO, &camera_info);

   // Default to the OV5647 setup
   strncpy(cameraName, "OV5647", sizeof cameraName);

   MMAL_PARAMETER_CAMERA_INFO_T param;
   param.hdr.id = MMAL_PARAMETER_CAMERA_INFO;
   param.hdr.size = sizeof(param)-4;  // Deliberately undersize to check firmware version
   status = mmal_port_parameter_get(component->control, &param.hdr);

   if (status != MMAL_SUCCESS)
   {
       // Running on newer firmware
       param.hdr.size = sizeof(param);
       status = mmal_port_parameter_get(camera_info->control, &param.hdr);
       MMALException::throw_if(status, "Failed to get camera parameters.");
       MMALException::throw_if(param.num_cameras <= static_cast<uint32_t>(cameraNum), "Camera number not found.");
      // Take the parameters from the first camera listed.
      width = param.cameras[cameraNum].max_width;
      height = param.cameras[cameraNum].max_height;
      strncpy(cameraName, param.cameras[cameraNum].camera_name, sizeof cameraName);
      cameraName[sizeof cameraName - 1] = 0;
   }
   else {
       // default to OV5647 if nothing detected..
      width = 2592;
      height = 1944;
   }

   mmal_component_destroy(camera_info);
}
