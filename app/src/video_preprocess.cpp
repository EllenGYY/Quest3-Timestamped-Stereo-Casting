#ifdef _OPENMP
#include <omp.h>
#endif

#include <opencv2/opencv.hpp>
#include "video_preprocess.h"
#include "util/log.h"
#include "options.h"

static cv::Mat cached_leftMapX, cached_leftMapY, cached_rightMapX, cached_rightMapY;
static bool maps_loaded = false;

void apply_video_effects(AVFrame *frame, const char *map_path) {
    // Enable OpenMP for OpenCV operations
    #ifdef _OPENMP
    cv::setNumThreads(omp_get_max_threads());
    #endif

    // Load mapping matrices only once
    if (!maps_loaded) {
        // Load mapping file
        if (!map_path) {
            LOGE("No mapping file path provided. Use --opencv-map to specify the path.");
            return;
        }
        cv::FileStorage fs(map_path, cv::FileStorage::READ);
        if (!fs.isOpened()) {
            LOGE("Could not open mapping file: %s", map_path);
            return;
        }

        fs["leftMapX"] >> cached_leftMapX;
        fs["leftMapY"] >> cached_leftMapY;
        fs["rightMapX"] >> cached_rightMapX;
        fs["rightMapY"] >> cached_rightMapY;

        // Convert maps to float32 if needed
        if (cached_leftMapX.type() != CV_32F) cached_leftMapX.convertTo(cached_leftMapX, CV_32F);
        if (cached_leftMapY.type() != CV_32F) cached_leftMapY.convertTo(cached_leftMapY, CV_32F);
        if (cached_rightMapX.type() != CV_32F) cached_rightMapX.convertTo(cached_rightMapX, CV_32F);
        if (cached_rightMapY.type() != CV_32F) cached_rightMapY.convertTo(cached_rightMapY, CV_32F);

        fs.release();
        maps_loaded = true;
    }


    // Create separate planes for YUV data
    cv::Mat y(frame->height, frame->width, CV_8UC1, frame->data[0], frame->linesize[0]);
    cv::Mat u(frame->height/2, frame->width/2, CV_8UC1, frame->data[1], frame->linesize[1]);
    cv::Mat v(frame->height/2, frame->width/2, CV_8UC1, frame->data[2], frame->linesize[2]);
    
    // Create continuous copies and resize U,V to match Y
    cv::Mat y_cont = y.clone();
    cv::Mat u_cont_resized, v_cont_resized;
    cv::resize(u.clone(), u_cont_resized, y.size());
    cv::resize(v.clone(), v_cont_resized, y.size());
    
    // Merge planes into a single matrix
    std::vector<cv::Mat> yuv_planes = {y_cont, u_cont_resized, v_cont_resized};
    cv::Mat yuv;
    cv::merge(yuv_planes, yuv);
    
    // Convert to BGR
    cv::Mat bgr;
    cv::cvtColor(yuv, bgr, cv::COLOR_YUV2BGR);

    // Split into left and right halves
    cv::Mat left_half = bgr(cv::Rect(0, 0, bgr.cols / 2, bgr.rows));
    cv::Mat right_half = bgr(cv::Rect(bgr.cols / 2, 0, bgr.cols - bgr.cols / 2, bgr.rows));

    // Apply mapping to each half
    cv::Mat left_mapped, right_mapped;
    cv::remap(left_half, left_mapped, cached_leftMapX, cached_leftMapY, cv::INTER_LINEAR);
    cv::remap(right_half, right_mapped, cached_rightMapX, cached_rightMapY, cv::INTER_LINEAR);

    // Create a new Mat for the result
    cv::Mat result = cv::Mat::zeros(bgr.size(), bgr.type());
    
    // Copy mapped halves to result
    left_mapped.copyTo(result(cv::Rect(0, 0, bgr.cols / 2, bgr.rows)));
    right_mapped.copyTo(result(cv::Rect(bgr.cols / 2, 0, bgr.cols - bgr.cols / 2, bgr.rows)));

    // Create a new Mat for the result
    //cv::Mat result = cv::Mat::zeros(bgr.size(), bgr.type());
    
    // Copy right half to left side and left half to right side
    //right_half.copyTo(result(cv::Rect(0, 0, bgr.cols / 2, bgr.rows)));
    //left_half.copyTo(result(cv::Rect(bgr.cols / 2, 0, bgr.cols - bgr.cols / 2, bgr.rows)));
    // Convert back to YUV
    cv::Mat yuv_output;
    cv::cvtColor(result, yuv_output, cv::COLOR_BGR2YUV);
    
    // Split back into planes
    std::vector<cv::Mat> output_planes;
    cv::split(yuv_output, output_planes);
    
    // Resize U,V back to original size
    cv::Mat u_small, v_small;
    cv::resize(output_planes[1], u_small, u.size());
    cv::resize(output_planes[2], v_small, v.size());

    // Copy data back to AVFrame
    memcpy(frame->data[0], output_planes[0].data, frame->linesize[0] * frame->height);
    memcpy(frame->data[1], u_small.data, frame->linesize[1] * frame->height/2);
    memcpy(frame->data[2], v_small.data, frame->linesize[2] * frame->height/2);
}