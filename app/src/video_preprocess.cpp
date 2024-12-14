#include <opencv2/opencv.hpp>
#include "video_preprocess.h"
#include "util/log.h" 

void apply_video_effects(AVFrame *frame) {
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

    // split into left and right halves
    cv::Mat left_half = bgr(cv::Rect(0, 0, bgr.cols / 2, bgr.rows));
    cv::Mat right_half = bgr(cv::Rect(bgr.cols / 2, 0, bgr.cols - bgr.cols / 2, bgr.rows));

    // Create a new Mat for the result
    cv::Mat result = cv::Mat::zeros(bgr.size(), bgr.type());
    
    // Copy right half to left side and left half to right side
    right_half.copyTo(result(cv::Rect(0, 0, bgr.cols / 2, bgr.rows)));
    left_half.copyTo(result(cv::Rect(bgr.cols / 2, 0, bgr.cols - bgr.cols / 2, bgr.rows)));

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