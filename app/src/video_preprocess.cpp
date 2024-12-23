#ifdef _OPENMP
#include <omp.h>
#endif

#include <opencv2/opencv.hpp>
#include "video_preprocess.h"
#include "util/log.h"
#include "options.h"

static cv::Mat cached_leftMapX, cached_leftMapY, cached_rightMapX, cached_rightMapY;
static bool maps_loaded = false;


void apply_video_effects(AVFrame *frame, const char *map_path, const char *show_text) {
    // Enable OpenMP for OpenCV operations
    #ifdef _OPENMP
    cv::setNumThreads(omp_get_max_threads());
    #endif

    int text_height = 60;
    int original_height = frame->height;
    int display_height = show_text ? original_height + text_height : original_height;

    // Load mapping matrices only once
    if (!maps_loaded) {
        // Load mapping file
        if (!map_path) {
            if (show_text == NULL) {
                LOGE("No mapping file path provided. Use --opencv-map to specify the path.");
                return;                
            }
            LOGE("Text is enabled, Continue without mapping.");
        }else{
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
        }
        maps_loaded = true; // either map loaded or continue without mapping
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
    if (map_path) {
        cv::remap(left_half, left_mapped, cached_leftMapX, cached_leftMapY, cv::INTER_LINEAR);
        cv::remap(right_half, right_mapped, cached_rightMapX, cached_rightMapY, cv::INTER_LINEAR);
    } else {
        left_mapped = left_half;
        right_mapped = right_half;
    }

    // Create a larger result matrix to accommodate the text bar
    cv::Mat result = cv::Mat::zeros(display_height, frame->width, bgr.type());

    // Copy mapped halves to result (offset by 30 pixels if showing text)
    int y_offset = show_text ? text_height : 0;
    // Use ROI (Region of Interest) to ensure proper copying
    cv::Mat dest_roi = result(cv::Rect(0, y_offset, frame->width, original_height));
    
    // Copy the mapped halves into the ROI
    left_mapped.copyTo(dest_roi(cv::Rect(0, 0, bgr.cols/2, original_height)));
    right_mapped.copyTo(dest_roi(cv::Rect(bgr.cols/2, 0, bgr.cols - bgr.cols/2, original_height)));

    // If text should be shown, add a black bar and text at the top
    if (show_text != NULL) {
        // Create a black bar at the top
        cv::rectangle(result, 
                     cv::Point(0, 0), 
                     cv::Point(result.cols, text_height), 
                     cv::Scalar(0, 0, 0),  // Black in BGR
                     -1);  // Filled rectangle

        // Add white text
        cv::putText(result, 
                    show_text,
                    cv::Point(10, text_height - 10),  // Position (10px from left, 50px from top)
                    cv::FONT_HERSHEY_SIMPLEX,  // Font
                    1,  // Scale
                    cv::Scalar(255, 255, 255),  // White in BGR
                    1,    // Thickness
                    cv::LINE_AA);  // Anti-aliased
    }

    // Convert back to YUV
    cv::Mat yuv_output;
    cv::cvtColor(result, yuv_output, cv::COLOR_BGR2YUV);
    
    // Split back into planes
    std::vector<cv::Mat> output_planes;
    cv::split(yuv_output, output_planes);
    
    // Resize U,V back to original size
    cv::Mat u_small, v_small;
    cv::resize(output_planes[1], u_small, cv::Size(frame->width/2, display_height/2));
    cv::resize(output_planes[2], v_small, cv::Size(frame->width/2, display_height/2));

    // Update frame height
    frame->height = display_height;
    
    // Reallocate frame buffers if needed
    if (show_text != NULL) {
        av_frame_get_buffer(frame, 32);
    }

    // Copy data back to AVFrame
    memcpy(frame->data[0], output_planes[0].data, frame->linesize[0] * frame->height);
    memcpy(frame->data[1], u_small.data, frame->linesize[1] * frame->height/2);
    memcpy(frame->data[2], v_small.data, frame->linesize[2] * frame->height/2);
}