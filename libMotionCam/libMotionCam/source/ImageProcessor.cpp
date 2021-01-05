#include "motioncam/ImageProcessor.h"
#include "motioncam/RawContainer.h"
#include "motioncam/CameraProfile.h"
#include "motioncam/Temperature.h"
#include "motioncam/Exceptions.h"
#include "motioncam/Util.h"
#include "motioncam/Logger.h"
#include "motioncam/Measure.h"
#include "motioncam/Settings.h"
#include "motioncam/ImageOps.h"

// Halide
#include "generate_edges.h"
#include "measure_image.h"
#include "deinterleave_raw.h"
#include "forward_transform.h"
#include "inverse_transform.h"
#include "fuse_image.h"
#include "camera_preview2.h"
#include "camera_preview3.h"

#include "preview_landscape2.h"
#include "preview_portrait2.h"
#include "preview_reverse_portrait2.h"
#include "preview_reverse_landscape2.h"
#include "preview_landscape4.h"
#include "preview_portrait4.h"
#include "preview_reverse_portrait4.h"
#include "preview_reverse_landscape4.h"
#include "preview_landscape8.h"
#include "preview_portrait8.h"
#include "preview_reverse_portrait8.h"
#include "preview_reverse_landscape8.h"

#include "postprocess.h"

#include <iostream>
#include <fstream>
#include <algorithm>
#include <memory>

#include <exiv2/exiv2.hpp>
#include <opencv2/ximgproc/edge_filter.hpp>
#include <opencv2/features2d.hpp>
#include <opencv2/xfeatures2d.hpp>

#ifdef DNG_SUPPORT

#include <dng/dng_host.h>
#include <dng/dng_negative.h>
#include <dng/dng_camera_profile.h>
#include <dng/dng_file_stream.h>
#include <dng/dng_memory_stream.h>
#include <dng/dng_image_writer.h>
#include <dng/dng_render.h>
#include <dng/dng_gain_map.h>

#endif

using std::ios;
using std::string;
using std::shared_ptr;
using std::vector;
using std::to_string;
using std::pair;

std::vector<Halide::Runtime::Buffer<float>> createWaveletBuffers(int width, int height) {
    std::vector<Halide::Runtime::Buffer<float>> buffers;
    
    for(int level = 0; level < 6; level++) {
        width = width / 2;
        height = height / 2;
        
        buffers.emplace_back(width, height, 4, 2);
    }
    
    return buffers;
}

extern "C" int extern_denoise(halide_buffer_t *in, int32_t width, int32_t height, int c, float weight, halide_buffer_t *out) {
    if (in->is_bounds_query()) {
        in->dim[0].min = 0;
        in->dim[1].min = 0;
        in->dim[2].min = 0;
        
        in->dim[0].extent = width;
        in->dim[1].extent = height;
        in->dim[2].extent = 2;
    }
    else {
        auto inputBuffers = createWaveletBuffers(width, height);
        
        forward_transform(in,
                          width,
                          height,
                          c,
                          inputBuffers[0],
                          inputBuffers[1],
                          inputBuffers[2],
                          inputBuffers[3],
                          inputBuffers[4],
                          inputBuffers[5]);

        cv::Mat hh(inputBuffers[0].height(),
                   inputBuffers[0].width(),
                   CV_32F,
                   inputBuffers[0].data() + 3*inputBuffers[0].stride(2));

        float noiseSigma = motioncam::estimateNoise(hh);
        
        inverse_transform(inputBuffers[0],
                          inputBuffers[1],
                          inputBuffers[2],
                          inputBuffers[3],
                          inputBuffers[4],
                          inputBuffers[5],
                          0,
                          65535,
                          65535,
                          weight*noiseSigma,
                          true,
                          1,
                          0,
                          out);
    }
    
    return 0;
}

extern "C" int extern_min_max(halide_buffer_t *in, int32_t width, int32_t height, halide_buffer_t *out) {
    if (in->is_bounds_query()) {
        in->dim[0].min = 0;
        in->dim[1].min = 0;
        
        in->dim[0].extent = width;
        in->dim[1].extent = height;
    }
    else {
        Halide::Runtime::Buffer<float> inBuf(*in);
        Halide::Runtime::Buffer<float> outBuf(*out);
        
        double min, max;
        
        cv::Mat m(inBuf.height(), inBuf.width(), CV_32F, inBuf.data());
        cv::minMaxLoc(m, &min, &max);

        outBuf.begin()[0] = min;
        outBuf.begin()[1] = max;
    }
    
    return 0;
}

namespace motioncam {
    const int DENOISE_LEVELS    = 6;
    const int EXPANDED_RANGE    = 16384;

    struct RawData {
        Halide::Runtime::Buffer<uint16_t> rawBuffer;
        Halide::Runtime::Buffer<uint8_t> previewBuffer;
        RawImageMetadata metadata;
    };

    template<typename T>
    static Halide::Runtime::Buffer<T> ToHalideBuffer(const cv::Mat& input) {
        return Halide::Runtime::Buffer<T>((T*) input.data, input.cols, input.rows);
    }

    struct NativeBufferContext {
        NativeBufferContext(NativeBuffer& buffer, bool write) : nativeBuffer(buffer)
        {
            nativeBufferData = nativeBuffer.lock(write);
        }
        
        Halide::Runtime::Buffer<uint8_t> getHalideBuffer() {
            return Halide::Runtime::Buffer<uint8_t>(nativeBufferData, (int) nativeBuffer.len());
        }
        
        ~NativeBufferContext() {
            nativeBuffer.unlock();
        }
        
    private:
        NativeBuffer& nativeBuffer;
        uint8_t* nativeBufferData;
    };

    ImageProgressHelper::ImageProgressHelper(const ImageProcessorProgress& progressListener, int numImages, int start) :
        mStart(start), mProgressListener(progressListener), mNumImages(numImages), mCurImage(0)
    {
        // Per fused image increment is numImages * numChannels over a 75% progress amount.
        mPerImageIncrement = 75.0 / (numImages * 4);
    }
    
    void ImageProgressHelper::postProcessCompleted() {
        mProgressListener.onProgressUpdate(mStart + 95);
    }
    
    void ImageProgressHelper::denoiseCompleted() {
        // Starting point is mStart, denoising takes 50%, progress should now be mStart + 50%
        mProgressListener.onProgressUpdate(mStart + 75);
    }

    void ImageProgressHelper::nextFusedImage() {
        ++mCurImage;
        mProgressListener.onProgressUpdate(static_cast<int>(mStart + (mPerImageIncrement * mCurImage)));
    }

    void ImageProgressHelper::imageSaved() {
        mProgressListener.onProgressUpdate(100);
        mProgressListener.onCompleted();
    }

    cv::Mat ImageProcessor::postProcess(std::vector<Halide::Runtime::Buffer<uint16_t>>& inputBuffers,
                                        const int offsetX,
                                        const int offsetY ,
                                        const RawImageMetadata& metadata,
                                        const RawCameraMetadata& cameraMetadata,
                                        const PostProcessSettings& settings)
    {
        Measure measure("postProcess");
        
        cv::Mat cameraToSrgb;
        cv::Vec3f cameraWhite;
        
        if(settings.temperature > 0 || settings.tint > 0) {
            Temperature t(settings.temperature, settings.tint);
            
            createSrgbMatrix(cameraMetadata, t, cameraWhite, cameraToSrgb);
        }
        else {
            createSrgbMatrix(cameraMetadata, metadata.asShot, cameraWhite, cameraToSrgb);
        }

        Halide::Runtime::Buffer<float> shadingMapBuffer[4];
        Halide::Runtime::Buffer<float> cameraToSrgbBuffer = ToHalideBuffer<float>(cameraToSrgb);

        for(int i = 0; i < 4; i++) {
            shadingMapBuffer[i] = ToHalideBuffer<float>(metadata.lensShadingMap[i]);
        }

        cv::Mat output((inputBuffers[0].height() - offsetY)*2, (inputBuffers[0].width() - offsetX)*2, CV_8UC3);
        
        Halide::Runtime::Buffer<uint8_t> outputBuffer(
            Halide::Runtime::Buffer<uint8_t>::make_interleaved(output.data, output.cols, output.rows, 3));
        
        // Edges are garbage, don't process them
        outputBuffer.translate(0, offsetX);
        outputBuffer.translate(1, offsetY);

        for(int i = 0; i < 4; i++)
        {
            inputBuffers[i].set_host_dirty();
            shadingMapBuffer[i].set_host_dirty();
        }

        cameraToSrgbBuffer.set_host_dirty();
        
        postprocess(inputBuffers[0],
                    inputBuffers[1],
                    inputBuffers[2],
                    inputBuffers[3],
                    shadingMapBuffer[0],
                    shadingMapBuffer[1],
                    shadingMapBuffer[2],
                    shadingMapBuffer[3],
                    cameraWhite[0],
                    cameraWhite[1],
                    cameraWhite[2],
                    cameraToSrgbBuffer,
                    EXPANDED_RANGE,
                    static_cast<int>(cameraMetadata.sensorArrangment),
                    settings.gamma,
                    settings.shadows,
                    settings.tonemapVariance,
                    settings.blacks,
                    settings.exposure,
                    settings.whitePoint,
                    settings.contrast,
                    settings.blueSaturation,
                    settings.saturation,
                    settings.greenSaturation,
                    settings.sharpen0,
                    settings.sharpen1,
                    settings.chromaEps,
                    outputBuffer);
        
        outputBuffer.device_sync();
        outputBuffer.copy_to_host();

        return output;
    }

    float ImageProcessor::estimateShadows(const RawImageBuffer& buffer, const RawCameraMetadata& cameraMetadata, PostProcessSettings settings) {
        Halide::Runtime::Buffer<float> shadingMapBuffer[4];

        for(int i = 0; i < 4; i++) {
            shadingMapBuffer[i] = ToHalideBuffer<float>(buffer.metadata.lensShadingMap[i]);
        }

        float prevLw = 1e-5f;
        
        for(int16_t i = 2; i < 16; i+=2) {
            settings.shadows = i;
            
            auto previewBuffer = createPreview(buffer, 8, cameraMetadata, settings);

            cv::Mat temp(previewBuffer.height(), previewBuffer.width(), CV_8UC4, previewBuffer.data());
            cv::cvtColor(temp, temp, cv::COLOR_BGRA2GRAY);
                        
            float Lw = cv::mean(temp)[0];
            if(Lw / prevLw < 1.03)
                break;

            prevLw = Lw;
        }

        return std::max(2.0f, settings.shadows - 2);
    }

    float ImageProcessor::estimateExposureCompensation(const RawImageBuffer& buffer, const RawCameraMetadata& cameraMetadata) {
        auto rawBufferHistogram = calcHistogram(cameraMetadata, buffer, 1);
        const int maxBufferPixels = (int) (1e-4 * (buffer.width*buffer.height)/4.0);
        int maxRawBufferBin[3] = {0, 0, 0};
                
        for(int c = 0; c < rawBufferHistogram.rows; c++) {
            int sum = 0;
            
            for(int x = rawBufferHistogram.cols - 1; x >= 0; x--) {
                sum += rawBufferHistogram.at<uint32_t>(c, x);
                
                if(sum > maxBufferPixels) {
                    maxRawBufferBin[c] = x;
                    break;
                }
            }
        }
        
        int bin = cv::max(maxRawBufferBin[2], cv::max(maxRawBufferBin[0], maxRawBufferBin[1]));
                
        double m = rawBufferHistogram.cols / static_cast<double>(bin + 1);
        return static_cast<float>(cv::log(m) / cv::log(2.0));
    }

    void ImageProcessor::estimateBasicSettings(const RawImageBuffer& rawBuffer,
                                               const RawCameraMetadata& cameraMetadata,
                                               PostProcessSettings& outSettings)
    {
//        Measure measure("estimateBasicSettings()");
        
        // Start with basic initial values
        PostProcessSettings settings;
        
        // Calculate white balance from metadata
        CameraProfile cameraProfile(cameraMetadata);
        Temperature temperature;
        
        cameraProfile.temperatureFromVector(rawBuffer.metadata.asShot, temperature);
        
        settings.temperature    = static_cast<float>(temperature.temperature());
        settings.tint           = static_cast<float>(temperature.tint());
        settings.shadows        = estimateShadows(rawBuffer, cameraMetadata, settings);
        
        // Calculate blacks
        auto previewBuffer = createPreview(rawBuffer, 8, cameraMetadata, settings);

        cv::Mat preview(previewBuffer.height(), previewBuffer.width(), CV_8UC4, previewBuffer.data());
        cv::Mat histogram;
                
        vector<cv::Mat> inputImages     = { preview };
        const vector<int> channels      = { 0 };
        const vector<int> histBins      = { 255 };
        const vector<float> histRange   = { 0, 256 };
        
        cv::calcHist(inputImages, channels, cv::Mat(), histogram, histBins, histRange);
        
        // Estimate blacks
        const float maxDehazePercent = 0.07f; // Max 7% pixels
        const int maxEndBin = 8; // Max bin

        int allowPixels = static_cast<int>(maxDehazePercent * static_cast<float>(preview.cols * preview.rows));
        int pixels = 0;
        int endBin = 0;
        
        for(endBin = 0; endBin < maxEndBin; endBin++) {
            int binPx = histogram.at<float>(endBin);
            
            if(binPx + pixels > allowPixels)
                break;
            
            pixels += binPx;
        }

        settings.blacks = std::max(0.02f, static_cast<float>(endBin) / static_cast<float>(histogram.rows - 1));

        // Estimate white point
        allowPixels = static_cast<int>(0.005f * static_cast<float>(preview.cols * preview.rows));
        pixels = 0;
        
        for(endBin = histogram.rows - 1; endBin >= 192; endBin--) {
            int binPx = histogram.at<float>(endBin);
            
            if(binPx + pixels > allowPixels)
                break;
            
            pixels += binPx;
        }
        
        settings.whitePoint = static_cast<float>(endBin) / ((float) histogram.rows - 1);
        
        // Update estimated settings
        outSettings = settings;
    }

    void ImageProcessor::estimateSettings(const RawImageBuffer& rawBuffer,
                                          const RawCameraMetadata& cameraMetadata,
                                          PostProcessSettings& outSettings)
    {
        Measure measure("estimateSettings");
        
        // Start with basic initial values
        PostProcessSettings settings;
        
        // Calculate white balance from metadata
        CameraProfile cameraProfile(cameraMetadata);
        Temperature temperature;
        
        cameraProfile.temperatureFromVector(rawBuffer.metadata.asShot, temperature);
        
        settings.temperature    = static_cast<float>(temperature.temperature());
        settings.tint           = static_cast<float>(temperature.tint());
        settings.exposure       = estimateExposureCompensation(rawBuffer, cameraMetadata);
        settings.shadows        = estimateShadows(rawBuffer, cameraMetadata, settings);
        
        // Calculate blacks
        auto previewBuffer = createPreview(rawBuffer, 4, cameraMetadata, settings);
        
        cv::Mat preview(previewBuffer.height(), previewBuffer.width(), CV_8UC4, previewBuffer.data());
        cv::Mat histogram;
                
        vector<cv::Mat> inputImages     = { preview };
        const vector<int> channels      = { 0 };
        const vector<int> histBins      = { 255 };
        const vector<float> histRange   = { 0, 256 };
        
        cv::calcHist(inputImages, channels, cv::Mat(), histogram, histBins, histRange);
        
        // Estimate blacks
        const float maxDehazePercent = 0.07f; // Max 7% pixels
        const int16_t maxEndBin = 8; // Max bin

        int allowPixels = static_cast<int>(maxDehazePercent * static_cast<float>(preview.cols * preview.rows));
        int pixels = 0;
        int endBin = 0;
        
        for(endBin = 0; endBin < maxEndBin; endBin++) {
            int binPx = histogram.at<float>(endBin);
            
            if(binPx + pixels > allowPixels)
                break;
            
            pixels += binPx;
        }

        settings.blacks = std::max(0.02f, static_cast<float>(endBin) / static_cast<float>(histogram.rows - 1));

        // Estimate white point
        allowPixels = static_cast<int>(0.005 * (preview.cols * preview.rows));
        pixels = 0;
        
        for(endBin = histogram.rows - 1; endBin >= 192; endBin--) {
            int binPx = histogram.at<float>(endBin);
            
            if(binPx + pixels > allowPixels)
                break;
            
            pixels += binPx;
        }
        
        settings.whitePoint = static_cast<float>(endBin) / static_cast<float>(histogram.rows - 1);
        
        //
        // Scene luminance
        //

        cv::Mat tmp;

        cv::cvtColor(preview, tmp, cv::COLOR_BGRA2GRAY);
        tmp.convertTo(tmp, CV_32F, 1.0/255.0);
        cv::log(tmp + 0.001f, tmp);

        settings.sceneLuminance = static_cast<float>(cv::exp(1.0/(preview.cols*preview.rows) * cv::sum(tmp)[0]));

        //
        // Use faster method for noise estimate
        //

        auto rawImage = loadRawImage(rawBuffer, cameraMetadata);

        cv::Mat rawImageInput(rawImage->rawBuffer.height(),
                              rawImage->rawBuffer.width(),
                              CV_16U,
                              rawImage->rawBuffer.data());

        cv::Mat k(3, 3, CV_32F);

        k.at<float>(0, 0) =  1;
        k.at<float>(0, 1) = -2;
        k.at<float>(0, 2) =  1;

        k.at<float>(1, 0) = -2;
        k.at<float>(1, 1) =  4;
        k.at<float>(1, 2) = -2;

        k.at<float>(2, 0) =  1;
        k.at<float>(2, 1) = -2;
        k.at<float>(2, 2) =  1;

        cv::filter2D(rawImageInput, rawImageInput, CV_32F, k);

        const double pi = 3.14159265358979323846;
        double p = 1.0 / ( 6.0 * (rawImageInput.cols - 2.0) * (rawImageInput.rows - 2.0) );
        p = sqrt(0.5*pi) * p;

        auto sigma = p * cv::sum(cv::abs(rawImageInput));

        settings.noiseSigma = sigma[0];
        
        // Update estimated settings
        outSettings = settings;
    }

    void ImageProcessor::createSrgbMatrix(const RawCameraMetadata& cameraMetadata,
                                          const Temperature& temperature,
                                          cv::Vec3f& cameraWhite,
                                          cv::Mat& cameraToSrgb)
    {
        cv::Mat pcsToCamera, cameraToPcs;
        cv::Mat pcsToSrgb, srgbToPcs;
        
        CameraProfile cameraProfile(cameraMetadata);

        cameraProfile.cameraToPcs(temperature, pcsToCamera, cameraToPcs, cameraWhite);
        motioncam::CameraProfile::pcsToSrgb(pcsToSrgb, srgbToPcs);

        cameraToSrgb = pcsToSrgb * cameraToPcs;
    }

    void ImageProcessor::createSrgbMatrix(const RawCameraMetadata& cameraMetadata,
                                          const cv::Vec3f& asShot,
                                          cv::Vec3f& cameraWhite,
                                          cv::Mat& cameraToSrgb)
    {
        cv::Mat pcsToCamera, cameraToPcs;
        cv::Mat pcsToSrgb, srgbToPcs;
        
        CameraProfile cameraProfile(cameraMetadata);
        Temperature temperature;

        cv::Vec3f asShotVector = asShot;
        float max = math::max(asShotVector);
        
        if(max > 0) {
            asShotVector[0] = asShotVector[0] * (1.0f / max);
            asShotVector[1] = asShotVector[1] * (1.0f / max);
            asShotVector[2] = asShotVector[2] * (1.0f / max);
        }
        else {
            throw InvalidState("Camera white balance vector is zero");
        }

        cameraProfile.temperatureFromVector(asShotVector, temperature);

        cameraProfile.cameraToPcs(temperature, pcsToCamera, cameraToPcs, cameraWhite);
        motioncam::CameraProfile::pcsToSrgb(pcsToSrgb, srgbToPcs);

        cameraToSrgb = pcsToSrgb * cameraToPcs;
    }

    void ImageProcessor::cameraPreview(const RawImageBuffer& rawBuffer,
                                       const RawCameraMetadata& cameraMetadata,
                                       const int downscaleFactor,
                                       const float shadows,
                                       const float contrast,
                                       const float saturation,
                                       const float blacks,
                                       const float whitePoint,
                                       const float tonemapVariance,
                                       Halide::Runtime::Buffer<uint8_t>& inputBuffer,
                                       Halide::Runtime::Buffer<uint8_t>& outputBuffer)
    {
//        Measure measure("cameraPreview()");
        
        cv::Mat cameraToSrgb;
        cv::Vec3f cameraWhite;
        
        int width = rawBuffer.width / 2 / downscaleFactor;
        int height = rawBuffer.height / 2 / downscaleFactor;

        Halide::Runtime::Buffer<float> shadingMapBuffer[4];

        for(int i = 0; i < 4; i++) {
            shadingMapBuffer[i] = ToHalideBuffer<float>(rawBuffer.metadata.lensShadingMap[i]);
            shadingMapBuffer[i].set_host_dirty();
        }
                
        createSrgbMatrix(cameraMetadata, rawBuffer.metadata.asShot, cameraWhite, cameraToSrgb);
        
        Halide::Runtime::Buffer<float> cameraToSrgbBuffer = ToHalideBuffer<float>(cameraToSrgb);
        cameraToSrgbBuffer.set_host_dirty();

        auto camera_preview = &camera_preview2;
        if(downscaleFactor == 3)
            camera_preview = &camera_preview3;
        
        camera_preview(inputBuffer,
                       rawBuffer.rowStride,
                       static_cast<int>(rawBuffer.pixelFormat),
                       cameraToSrgbBuffer,
                       width,
                       height,
                       cameraMetadata.blackLevel[0],
                       cameraMetadata.blackLevel[1],
                       cameraMetadata.blackLevel[2],
                       cameraMetadata.blackLevel[3],
                       cameraMetadata.whiteLevel,
                       rawBuffer.metadata.colorCorrection[0],
                       rawBuffer.metadata.colorCorrection[1],
                       rawBuffer.metadata.colorCorrection[2],
                       rawBuffer.metadata.colorCorrection[3],
                       shadingMapBuffer[0],
                       shadingMapBuffer[1],
                       shadingMapBuffer[2],
                       shadingMapBuffer[3],
                       rawBuffer.metadata.asShot[0],
                       rawBuffer.metadata.asShot[1],
                       rawBuffer.metadata.asShot[2],
                       static_cast<int>(cameraMetadata.sensorArrangment),
                       tonemapVariance,
                       2.2f,
                       shadows,
                       blacks,
                       whitePoint,
                       contrast,
                       saturation,
                       outputBuffer);
        
        outputBuffer.device_sync();
    }

    Halide::Runtime::Buffer<uint8_t> ImageProcessor::createPreview(const RawImageBuffer& rawBuffer,
                                                                   const int downscaleFactor,
                                                                   const RawCameraMetadata& cameraMetadata,
                                                                   const PostProcessSettings& settings)
    {
//        Measure measure("createPreview()");
        
        if(downscaleFactor != 2 && downscaleFactor != 4 && downscaleFactor != 8) {
            throw InvalidState("Invalid downscale factor");
        }
        
        cv::Mat cameraToSrgb;
        cv::Vec3f cameraWhite;
        
        if(settings.temperature > 0 || settings.tint > 0) {
            Temperature t(settings.temperature, settings.tint);
            
            createSrgbMatrix(cameraMetadata, t, cameraWhite, cameraToSrgb);
        }
        else {
            createSrgbMatrix(cameraMetadata, rawBuffer.metadata.asShot, cameraWhite, cameraToSrgb);
        }
        
        NativeBufferContext inputBufferContext(*rawBuffer.data, false);
        
        Halide::Runtime::Buffer<float> shadingMapBuffer[4];
        Halide::Runtime::Buffer<float> cameraToSrgbBuffer = ToHalideBuffer<float>(cameraToSrgb);
        
        if(rawBuffer.metadata.lensShadingMap.size() != 4) {
            throw InvalidState("Invalid lens shading map");
        }
        
        for(int i = 0; i < 4; i++) {
            shadingMapBuffer[i] = ToHalideBuffer<float>(rawBuffer.metadata.lensShadingMap[i]);
        }
        
        // Set up rotation based on orientation of image
        int width = rawBuffer.width / 2 / downscaleFactor; // Divide by 2 because we are not demosaicing the RAW data
        int height = rawBuffer.height / 2 / downscaleFactor;
        
        auto method = &preview_landscape2;
        
        switch(rawBuffer.metadata.screenOrientation) {
            case ScreenOrientation::REVERSE_PORTRAIT:
                if(downscaleFactor == 2)
                    method = &preview_reverse_portrait2;
                else if(downscaleFactor == 4)
                    method = &preview_reverse_portrait4;
                else
                    method = &preview_reverse_portrait8;

                std::swap(width, height);
                break;

            case ScreenOrientation::REVERSE_LANDSCAPE:
                if(downscaleFactor == 2)
                    method = &preview_reverse_landscape2;
                else if(downscaleFactor == 4)
                    method = &preview_reverse_landscape4;
                else
                    method = &preview_reverse_landscape8;

                break;

            case ScreenOrientation::PORTRAIT:
                if(downscaleFactor == 2)
                    method = &preview_portrait2;
                else if(downscaleFactor == 4)
                    method = &preview_portrait4;
                else
                    method = &preview_portrait8;

                std::swap(width, height);
                break;

            default:
            case ScreenOrientation::LANDSCAPE:
                if(downscaleFactor == 2)
                    method = &preview_landscape2;
                else if(downscaleFactor == 4)
                    method = &preview_landscape4;
                else
                    method = &preview_landscape8;
                break;
        }
       
        Halide::Runtime::Buffer<uint8_t> outputBuffer =
            Halide::Runtime::Buffer<uint8_t>::make_interleaved(width, height, 4);
        
        cameraToSrgbBuffer.set_host_dirty();

        for(auto& c : shadingMapBuffer)
            c.set_host_dirty();

        method(
            inputBufferContext.getHalideBuffer(),
            shadingMapBuffer[0],
            shadingMapBuffer[1],
            shadingMapBuffer[2],
            shadingMapBuffer[3],
            cameraWhite[0],
            cameraWhite[1],
            cameraWhite[2],
            cameraToSrgbBuffer,
            rawBuffer.width / 2 / downscaleFactor,
            rawBuffer.height / 2 / downscaleFactor,
            rawBuffer.rowStride,
            static_cast<int>(rawBuffer.pixelFormat),
            static_cast<int>(cameraMetadata.sensorArrangment),
            cameraMetadata.blackLevel[0],
            cameraMetadata.blackLevel[1],
            cameraMetadata.blackLevel[2],
            cameraMetadata.blackLevel[3],
            static_cast<uint16_t>(cameraMetadata.whiteLevel),
            settings.gamma,
            settings.shadows,
            settings.whitePoint,
            settings.tonemapVariance,
            settings.blacks,
            settings.exposure,
            settings.contrast,
            settings.blueSaturation,
            settings.saturation,
            settings.greenSaturation,
            settings.sharpen1,
            settings.flipped,
            outputBuffer);

        outputBuffer.device_sync();
        outputBuffer.copy_to_host();

        return outputBuffer;
    }
    
    std::shared_ptr<RawData> ImageProcessor::loadRawImage(const RawImageBuffer& rawBuffer,
                                                          const RawCameraMetadata& cameraMetadata,
                                                          const bool extendEdges,
                                                          const float scalePreview)
    {
        // Extend the image so it can be downscaled by 'LEVELS' for the denoising step
        int extendX = 0;
        int extendY = 0;

        int halfWidth  = rawBuffer.width / 2;
        int halfHeight = rawBuffer.height / 2;

        if(extendEdges) {
            const int T = pow(2, DENOISE_LEVELS);

            extendX = static_cast<int>(T * ceil(halfWidth / (double) T) - halfWidth);
            extendY = static_cast<int>(T * ceil(halfHeight / (double) T) - halfHeight);
        }
        
        auto rawData = std::make_shared<RawData>();

        NativeBufferContext inputBufferContext(*rawBuffer.data, false);
        
        rawData->previewBuffer  = Halide::Runtime::Buffer<uint8_t>(halfWidth + extendX, halfHeight + extendY);
        rawData->rawBuffer      = Halide::Runtime::Buffer<uint16_t>(halfWidth + extendX, halfHeight + extendY, 4);
        rawData->metadata       = rawBuffer.metadata;
                
        deinterleave_raw(inputBufferContext.getHalideBuffer(),
                         rawBuffer.rowStride,
                         static_cast<int>(rawBuffer.pixelFormat),
                         static_cast<int>(cameraMetadata.sensorArrangment),
                         halfWidth,
                         halfHeight,
                         extendX / 2,
                         extendY / 2,
                         cameraMetadata.whiteLevel,
                         cameraMetadata.blackLevel[0],
                         cameraMetadata.blackLevel[1],
                         cameraMetadata.blackLevel[2],
                         cameraMetadata.blackLevel[3],
                         scalePreview,
                         rawData->rawBuffer,
                         rawData->previewBuffer);
        
        return rawData;
    }

    __unused void ImageProcessor::measureImage(RawImageBuffer& rawBuffer, const RawCameraMetadata& cameraMetadata, float& outSceneLuminosity)
    {
//        Measure measure("measureImage");
        
        int halfWidth  = rawBuffer.width / 2;
        int halfHeight = rawBuffer.height / 2;

        NativeBufferContext inputBufferContext(*rawBuffer.data, false);
        Halide::Runtime::Buffer<float> shadingMapBuffer[4];
        Halide::Runtime::Buffer<uint32_t> histogramBuffer(2u << 7u, 3);
                        
        for(int i = 0; i < 4; i++) {
            shadingMapBuffer[i] = ToHalideBuffer<float>(rawBuffer.metadata.lensShadingMap[i]);
            shadingMapBuffer[i].set_host_dirty();
        }

        const double downscale = 4;

        measure_image(inputBufferContext.getHalideBuffer(),
                      rawBuffer.rowStride,
                      static_cast<int>(rawBuffer.pixelFormat),
                      halfWidth,
                      halfHeight,
                      downscale,
                      cameraMetadata.blackLevel[0],
                      cameraMetadata.blackLevel[1],
                      cameraMetadata.blackLevel[2],
                      cameraMetadata.blackLevel[3],
                      cameraMetadata.whiteLevel,
                      rawBuffer.metadata.colorCorrection[0],
                      rawBuffer.metadata.colorCorrection[1],
                      rawBuffer.metadata.colorCorrection[2],
                      rawBuffer.metadata.colorCorrection[3],
                      shadingMapBuffer[0],
                      shadingMapBuffer[1],
                      shadingMapBuffer[2],
                      shadingMapBuffer[3],
                      rawBuffer.metadata.asShot[0],
                      rawBuffer.metadata.asShot[1],
                      rawBuffer.metadata.asShot[2],
                      static_cast<int>(cameraMetadata.sensorArrangment),
                      histogramBuffer);

        histogramBuffer.device_sync();
        histogramBuffer.copy_to_host();

        cv::Mat histogram(histogramBuffer.height(), histogramBuffer.width(), CV_32S, histogramBuffer.data());

        // Normalize
        histogram.convertTo(histogram, CV_32F, 1.0 / (halfWidth/downscale * halfHeight/downscale));

        // Calculate mean per channel
        float mean[3] = { 0, 0, 0 };

        for(int c = 0; c < histogram.rows; c++) {
            for(int x = 0; x < histogram.cols; x++) {
                mean[c] = mean[c] + (static_cast<float>(x) * histogram.at<float>(c, x));
            }

            mean[c] /= 256.0f;
        }

        outSceneLuminosity = std::max(std::max(mean[0], mean[1]), mean[2]);
    }

    __unused cv::Mat ImageProcessor::registerImage(const Halide::Runtime::Buffer<uint8_t>& referenceBuffer, const Halide::Runtime::Buffer<uint8_t>& toAlignBuffer, int scale) {
        Measure measure("registerImage()");

        cv::Mat referenceImage(referenceBuffer.height(), referenceBuffer.width(), CV_8U, (void*) referenceBuffer.data());
        cv::Mat toAlignImage(toAlignBuffer.height(), toAlignBuffer.width(), CV_8U, (void*) toAlignBuffer.data());
        auto detector = cv::ORB::create();
        
        std::vector<cv::KeyPoint> keypoints1, keypoints2;
        cv::Mat descriptors1, descriptors2;
        auto extractor = cv::xfeatures2d::BriefDescriptorExtractor::create();

        detector->detect(referenceImage, keypoints1);
        detector->detect(toAlignImage, keypoints2);

        extractor->compute(referenceImage, keypoints1, descriptors1);
        extractor->compute(toAlignImage, keypoints2, descriptors2);

        auto matcher = cv::BFMatcher::create(cv::NORM_HAMMING, false);
        
        std::vector< std::vector<cv::DMatch> > knn_matches;
        matcher->knnMatch( descriptors1, descriptors2, knn_matches, 2 );
        
        // Filter matches using the Lowe's ratio test
        const float ratio_thresh = 0.75f;
        std::vector<cv::DMatch> good_matches;

        for (auto& m : knn_matches)
        {
            if (m[0].distance < ratio_thresh * m[1].distance)
            {
                good_matches.push_back(m[0]);
            }
        }
        
        std::vector<cv::Point2f> obj;
        std::vector<cv::Point2f> scene;
        
        for(auto& m : good_matches)
        {
            obj.push_back( keypoints1[ m.queryIdx ].pt );
            scene.push_back( keypoints2[ m.trainIdx ].pt );
        }
        
        return findHomography( scene, obj, cv::RANSAC );
    }

    cv::Mat ImageProcessor::calcHistogram(const RawCameraMetadata& cameraMetadata, const RawImageBuffer& buffer, const int downscale) {
        NativeBufferContext inputBufferContext(*buffer.data, false);
        Halide::Runtime::Buffer<float> shadingMapBuffer[4];
        Halide::Runtime::Buffer<uint32_t> histogramBuffer(2u << 7u, 3);
                        
        for(int i = 0; i < 4; i++) {
            shadingMapBuffer[i] = ToHalideBuffer<float>(buffer.metadata.lensShadingMap[i]);
        }

        int halfWidth  = buffer.width / 2;
        int halfHeight = buffer.height / 2;

        measure_image(inputBufferContext.getHalideBuffer(),
                      buffer.rowStride,
                      static_cast<int>(buffer.pixelFormat),
                      halfWidth,
                      halfHeight,
                      downscale,
                      cameraMetadata.blackLevel[0],
                      cameraMetadata.blackLevel[1],
                      cameraMetadata.blackLevel[2],
                      cameraMetadata.blackLevel[3],
                      cameraMetadata.whiteLevel,
                      buffer.metadata.colorCorrection[0],
                      buffer.metadata.colorCorrection[1],
                      buffer.metadata.colorCorrection[2],
                      buffer.metadata.colorCorrection[3],
                      shadingMapBuffer[0],
                      shadingMapBuffer[1],
                      shadingMapBuffer[2],
                      shadingMapBuffer[3],
                      buffer.metadata.asShot[0],
                      buffer.metadata.asShot[1],
                      buffer.metadata.asShot[2],
                      static_cast<int>(cameraMetadata.sensorArrangment),
                      histogramBuffer);

        cv::Mat histogram(histogramBuffer.height(), histogramBuffer.width(), CV_32S, histogramBuffer.data());
        
        return histogram.clone();
    }

    __unused float ImageProcessor::matchExposures(const RawCameraMetadata& cameraMetadata, const RawImageBuffer& reference, const RawImageBuffer& toMatch)
    {
        auto refHistogram = calcHistogram(cameraMetadata, reference);
        auto toMatchHistogram = calcHistogram(cameraMetadata, toMatch);

        // Cumulitive histogram
        for(int c = 0; c < refHistogram.rows; c++) {
            for (int i = 1; i < refHistogram.cols; i++) {
                refHistogram.at<uint32_t>(c, i) += refHistogram.at<uint32_t>(c, i - 1);
                toMatchHistogram.at<uint32_t>(c, i) += toMatchHistogram.at<uint32_t>(c, i - 1);
            }
        }
        
        float exposureScale = 0.0f;

        for(int c = 0; c < refHistogram.rows; c++) {
            std::vector<float> matches;

            for(int i = 0; i < toMatchHistogram.cols; i++) {
                float a = toMatchHistogram.at<uint32_t>(c, i);

                for(int j = 1; j < refHistogram.cols; j++) {
                    float b = refHistogram.at<uint32_t>(c, j);

                    if(a <= b) {
                        double match = j / (i + 1.0);
                        matches.push_back(match);
                        break;
                    }
                }
            }
            
            if(matches.empty())
                exposureScale += 1.0f;
            else
                exposureScale += *max_element(std::begin(matches), std::end(matches));
        }

        // Average of channels
        return exposureScale / 3;
    }

    void ImageProcessor::process(const std::string& inputPath,
                                 const std::string& outputPath,
                                 const ImageProcessorProgress& progressListener)
    {
        Measure measure("process()");

        // Open RAW container
        RawContainer rawContainer(inputPath);
        
        if(rawContainer.getFrames().empty()) {
            progressListener.onError("No frames found");
            return;
        }
        
        auto referenceRawBuffer = rawContainer.getFrame(rawContainer.getReferenceImage());
        
        const int rawWidth  = referenceRawBuffer->width / 2;
        const int rawHeight = referenceRawBuffer->height / 2;

        const int T = pow(2, DENOISE_LEVELS);
        
        const int offsetX = static_cast<int>(T * ceil(rawWidth / (double) T) - rawWidth);
        const int offsetY = static_cast<int>(T * ceil(rawHeight / (double) T) - rawHeight);
        
        //
        // Denoise
        //
        
        ImageProgressHelper progressHelper(progressListener, static_cast<int>(rawContainer.getFrames().size()), 0);
        
        std::vector<Halide::Runtime::Buffer<uint16_t>> denoiseOutput;        
        denoiseOutput = denoise(rawContainer, progressHelper);
        
        progressHelper.denoiseCompleted();
        
        //
        // Post process
        //
                
        // Check if we should write a DNG file
#ifdef DNG_SUPPORT
        if(rawContainer.getWriteDNG()) {
            std::vector<cv::Mat> rawChannels;
            rawChannels.reserve(4);

            for(int i = 0; i < 4; i++) {
                rawChannels.emplace_back(denoiseOutput[i].height(), denoiseOutput[i].width(), CV_16U, denoiseOutput[i].data());
            }

            switch(rawContainer.getCameraMetadata().sensorArrangment) {
                default:
                case ColorFilterArrangment::RGGB:
                    // All good
                    break;

                case ColorFilterArrangment::GRBG:
                {
                    std::vector<cv::Mat> tmp = rawChannels;
                    
                    rawChannels[0] = tmp[1];
                    rawChannels[1] = tmp[0];
                    rawChannels[2] = tmp[3];
                    rawChannels[3] = tmp[2];
                }
                break;

                case ColorFilterArrangment::GBRG:
                {
                    std::vector<cv::Mat> tmp = rawChannels;
                    
                    rawChannels[0] = tmp[2];
                    rawChannels[1] = tmp[0];
                    rawChannels[2] = tmp[3];
                    rawChannels[3] = tmp[1];
                }
                break;

                case ColorFilterArrangment::BGGR:
                    std::swap(rawChannels[0], rawChannels[3]);
                    break;
            }

            cv::Mat rawImage = buildRawImage(rawChannels, offsetX, offsetY);
            
            size_t extensionStartIdx = outputPath.find_last_of('.');
            std::string rawOutputPath;
            
            if(extensionStartIdx != std::string::npos) {
                rawOutputPath = outputPath.substr(0, extensionStartIdx);
            }
            else {
                rawOutputPath = outputPath;
            }
            
            writeDng(rawImage, rawContainer.getCameraMetadata(), referenceRawBuffer->metadata, rawOutputPath + ".dng");
        }
#endif

        cv::Mat outputImage;
        
        outputImage = postProcess(
            denoiseOutput,
            offsetX,
            offsetY,
            referenceRawBuffer->metadata,
            rawContainer.getCameraMetadata(),
            rawContainer.getPostProcessSettings());

        progressHelper.postProcessCompleted();

        // Write image
        std::vector<int> writeParams = { cv::IMWRITE_JPEG_QUALITY, rawContainer.getPostProcessSettings().jpegQuality };
        cv::imwrite(outputPath, outputImage, writeParams);

        // Create thumbnail
        cv::Mat thumbnail;
        
        int width = 320;
        int height = (int) std::lround((outputImage.rows / (double) outputImage.cols) * width);
        
        cv::resize(outputImage, thumbnail, cv::Size(width, height));
        
        // Add exif data to the output image
        addExifMetadata(referenceRawBuffer->metadata,
                        thumbnail,
                        rawContainer.getCameraMetadata(),
                        rawContainer.getPostProcessSettings().flipped,
                        outputPath);
        
        progressHelper.imageSaved();
    }
    
    void ImageProcessor::addExifMetadata(const RawImageMetadata& metadata,
                                         const cv::Mat& thumbnail,
                                         const RawCameraMetadata& cameraMetadata,
                                         const bool isFlipped,
                                         const std::string& inputOutput)
    {
        auto image = Exiv2::ImageFactory::open(inputOutput);
        if(image.get() == nullptr)
            return;
        
        image->readMetadata();
        
        Exiv2::ExifData& exifData = image->exifData();
        
        // sRGB color space
        exifData["Exif.Photo.ColorSpace"]       = uint16_t(1);
        
        // Capture settings
        exifData["Exif.Photo.ISOSpeedRatings"]  = uint16_t(metadata.iso);
        exifData["Exif.Photo.ExposureTime"]     = Exiv2::floatToRationalCast(metadata.exposureTime / ((float) 1e9));
        
        switch(metadata.screenOrientation)
        {
            default:
            case ScreenOrientation::LANDSCAPE:
                exifData["Exif.Image.Orientation"] = isFlipped ? uint16_t(2) : uint16_t(1);
                break;
                
            case ScreenOrientation::PORTRAIT:
                exifData["Exif.Image.Orientation"] = isFlipped ? uint16_t(5) : uint16_t(6);
                break;
                                
            case ScreenOrientation::REVERSE_LANDSCAPE:
                exifData["Exif.Image.Orientation"] = isFlipped ? uint16_t(4) : uint16_t(3);
                break;
                
            case ScreenOrientation::REVERSE_PORTRAIT:
                exifData["Exif.Image.Orientation"] = isFlipped ? uint16_t(7) : uint16_t(8);
                break;
        }
                
        if(!cameraMetadata.apertures.empty())
            exifData["Exif.Photo.ApertureValue"] = Exiv2::floatToRationalCast(cameraMetadata.apertures[0]);

        if(!cameraMetadata.focalLengths.empty())
            exifData["Exif.Photo.FocalLength"] = Exiv2::floatToRationalCast(cameraMetadata.focalLengths[0]);
        
        // Misc bits
        exifData["Exif.Photo.LensModel"]   = "MotionCam";
        exifData["Exif.Photo.LensMake"]    = "MotionCam";
        
        exifData["Exif.Photo.SceneType"]    = uint8_t(1);
        exifData["Exif.Image.XResolution"]  = Exiv2::Rational(72, 1);
        exifData["Exif.Image.YResolution"]  = Exiv2::Rational(72, 1);
        exifData["Exif.Photo.WhiteBalance"] = uint8_t(0);
        
        // Set thumbnail
        Exiv2::ExifThumb exifThumb(exifData);
        std::vector<uint8_t> thumbnailBuffer;
        
        cv::imencode(".jpg", thumbnail, thumbnailBuffer);
        
        exifThumb.setJpegThumbnail(thumbnailBuffer.data(), thumbnailBuffer.size());
        
        image->writeMetadata();
    }

    double ImageProcessor::measureSharpness(const RawImageBuffer& rawBuffer) {
//        Measure measure("measureSharpness()");
        
        int halfWidth  = rawBuffer.width / 2;
        int halfHeight = rawBuffer.height / 2;

        NativeBufferContext inputBufferContext(*rawBuffer.data, false);
        Halide::Runtime::Buffer<uint16_t> outputBuffer(halfWidth, halfHeight);
                
        generate_edges(inputBufferContext.getHalideBuffer(),
                       rawBuffer.rowStride,
                       static_cast<int>(rawBuffer.pixelFormat),
                       halfWidth,
                       halfHeight,
                       outputBuffer);
        
        outputBuffer.device_sync();
        outputBuffer.copy_to_host();
        
        cv::Mat output(outputBuffer.height(), outputBuffer.width(), CV_16U, outputBuffer.data());

        return cv::mean(output)[0];
    }

    std::vector<Halide::Runtime::Buffer<uint16_t>> ImageProcessor::denoise(const RawContainer& rawContainer, ImageProgressHelper& progressHelper) {
        Measure measure("denoise()");
        
        typedef Halide::Runtime::Buffer<float> WaveletBuffer;
        
        //
        // Read the reference
        //
        
        std::shared_ptr<RawImageBuffer> referenceRawBuffer =
            rawContainer.loadFrame(rawContainer.getReferenceImage());

        auto reference = loadRawImage(*referenceRawBuffer, rawContainer.getCameraMetadata());

        //
        // Transform reference image for all channels
        //
        
        std::vector<Halide::Runtime::Buffer<uint16_t>> denoiseOutput;
        cv::Mat referenceFlowImage(reference->previewBuffer.height(), reference->previewBuffer.width(), CV_8U, reference->previewBuffer.data());
        
        vector<vector<WaveletBuffer>> outputWavelet;
        vector<vector<WaveletBuffer>> refWavelet;

        vector<float> noiseSigma;
        
        reference->rawBuffer.set_host_dirty();

        for(int c = 0; c < 4; c++) {
            refWavelet.push_back(
                 createWaveletBuffers(reference->rawBuffer.width(), reference->rawBuffer.height()));
            
            outputWavelet.push_back(
                 createWaveletBuffers(reference->rawBuffer.width(), reference->rawBuffer.height()));
                        
            forward_transform(reference->rawBuffer,
                              reference->rawBuffer.width(),
                              reference->rawBuffer.height(),
                              c,
                              refWavelet[c][0],
                              refWavelet[c][1],
                              refWavelet[c][2],
                              refWavelet[c][3],
                              refWavelet[c][4],
                              refWavelet[c][5]);
            
            //
            // Create noise map
            //
            
            refWavelet[c][0].device_sync();
            refWavelet[c][0].copy_to_host();
            
            int offset = 3 * refWavelet[c][0].stride(2);
            
            cv::Mat hh(refWavelet[c][0].height(), refWavelet[c][0].width(), CV_32F, refWavelet[c][0].data() + offset);
            noiseSigma.push_back(estimateNoise(hh));
            progressHelper.nextFusedImage();
        }
  
        const int width = reference->rawBuffer.width();
        const int height = reference->rawBuffer.height();

        // Don't need this anymore
        reference->rawBuffer = Halide::Runtime::Buffer<uint16_t>();
        
        //
        // Fuse with other images
        //
        
        auto containerFrames = rawContainer.getFrames();
        std::vector<string> processFrames;

        // Get all frames we want to merge
        auto framesIt = containerFrames.begin();
        while(framesIt != containerFrames.end()) {
            // Add frames where exposure compensation is the same
            if(rawContainer.getFrame(*framesIt)->metadata.exposureCompensation == referenceRawBuffer->metadata.exposureCompensation) {
                processFrames.push_back(*framesIt);
            }

            ++framesIt;
        }
        
        auto it = processFrames.begin();
        bool resetOutput = true;
        
        while(it != processFrames.end()) {
            // Skip reference frame
            if(rawContainer.getReferenceImage() == *it) {
                ++it;
                continue;
            }
            
            auto current = loadRawImage(*rawContainer.loadFrame(*it), rawContainer.getCameraMetadata());

            // Calculate movement between frames
            cv::Mat flow;

            cv::Ptr<cv::DISOpticalFlow> opticalFlow =
                cv::DISOpticalFlow::create(cv::DISOpticalFlow::PRESET_FAST);

            cv::Mat currentFlowImage(current->previewBuffer.height(),
                                     current->previewBuffer.width(),
                                     CV_8U,
                                     current->previewBuffer.data());

            opticalFlow->setPatchSize(16);
            opticalFlow->setPatchStride(8);
            opticalFlow->setUseSpatialPropagation(true);
            opticalFlow->setGradientDescentIterations(16);
            opticalFlow->setVariationalRefinementIterations(5);
            
            opticalFlow->calc(referenceFlowImage, currentFlowImage, flow);
            
            // Clear memory
            opticalFlow = nullptr;
            
            // Calculate stddev of the flow map
            std::vector<cv::Mat> flowComponents;
            cv::Mat flowMag;
            cv::Scalar flowMean, flowStdDev;
            cv::Mat flowSmall;
            
            cv::resize(flow, flowSmall, cv::Size(flow.cols/4, flow.rows/4));
            cv::split(flowSmall, flowComponents);
            cv::magnitude(flowComponents[0], flowComponents[1], flowMag);
            cv::meanStdDev(flowMag, flowMean, flowStdDev);
        
            // Start with safe values
            int differenceWeight = 16;
            int weight = 8;
            
            // For higher ISO/slower shutter speeds, increase weights a bit.
            if(reference->metadata.iso >= 800 &&
               reference->metadata.exposureTime >= 8000000)
            {
                // If there is little motion in the scene, crank up the values
                if(flowStdDev[0] < 10) {
                    differenceWeight = 16;
                    weight = 16;
                }
            }
            // For low ISO/fast shutter speeds, reduce weights.
            else if(reference->metadata.iso <= 200 &&
                    reference->metadata.exposureTime <= 1250000)
            {
                differenceWeight = 4;
                weight = 4;
            }
            // For scenes with a lot motion, be a bit more careful
            else if(flowStdDev[0] > 10)
            {
                differenceWeight = 2;
                weight = 8;
            }
            
            Halide::Runtime::Buffer<float> flowBuffer =
                Halide::Runtime::Buffer<float>::make_interleaved((float*) flow.data, flow.cols, flow.rows, 2);
            
            current->rawBuffer.set_host_dirty();
            flowBuffer.set_host_dirty();

            for(int c = 0; c < 4; c++) {
                fuse_image(current->rawBuffer,
                           current->rawBuffer.width(),
                           current->rawBuffer.height(),
                           c,
                           flowBuffer,
                           refWavelet[c][0],
                           refWavelet[c][1],
                           refWavelet[c][2],
                           refWavelet[c][3],
                           refWavelet[c][4],
                           refWavelet[c][5],
                           outputWavelet[c][0],
                           outputWavelet[c][1],
                           outputWavelet[c][2],
                           outputWavelet[c][3],
                           outputWavelet[c][4],
                           outputWavelet[c][5],
                           noiseSigma[c],
                           static_cast<float>(differenceWeight),
                           static_cast<float>(weight),
                           resetOutput,
                           outputWavelet[c][0],
                           outputWavelet[c][1],
                           outputWavelet[c][2],
                           outputWavelet[c][3],
                           outputWavelet[c][4],
                           outputWavelet[c][5]);
                
                progressHelper.nextFusedImage();
            }

            rawContainer.releaseFrame(*it);

            resetOutput = false;
            ++it;
        }
        
        // Clean up
        referenceFlowImage = cv::Mat();
        
        // If we only have one image, use reference wavelet for output
        if(processFrames.size() == 1) {
            outputWavelet = refWavelet;
        }

        // Invert output wavelet
        for(int c = 0; c < 4; c++) {
            Halide::Runtime::Buffer<uint16_t> outputBuffer(width, height);
            
            noiseSigma[c] = noiseSigma[c] / sqrt(processFrames.size());
            
            inverse_transform(outputWavelet[c][0],
                              outputWavelet[c][1],
                              outputWavelet[c][2],
                              outputWavelet[c][3],
                              outputWavelet[c][4],
                              outputWavelet[c][5],
                              rawContainer.getCameraMetadata().blackLevel[c],
                              rawContainer.getCameraMetadata().whiteLevel,
                              EXPANDED_RANGE,
                              noiseSigma[c],
                              false,
                              static_cast<int>(processFrames.size()),
                              rawContainer.getPostProcessSettings().spatialDenoiseAggressiveness,
                              outputBuffer);
            
            outputBuffer.device_sync();

            // Clean up
            denoiseOutput.push_back(outputBuffer);
        }

        return denoiseOutput;
    }

#ifdef DNG_SUPPORT
    cv::Mat ImageProcessor::buildRawImage(vector<cv::Mat> channels, int cropX, int cropY) {
        const uint32_t height = channels[0].rows * 2;
        const uint32_t width  = channels[1].cols * 2;
        
        cv::Mat outputImage(height, width, CV_16U);
        
        for (int y = 0; y < height; y+=2) {
            auto* outRow1 = outputImage.ptr<uint16_t>(y);
            auto* outRow2 = outputImage.ptr<uint16_t>(y + 1);
            
            int ry = y / 2;
            
            const uint16_t* inC0 = channels[0].ptr<uint16_t>(ry);
            const uint16_t* inC1 = channels[1].ptr<uint16_t>(ry);
            const uint16_t* inC2 = channels[2].ptr<uint16_t>(ry);
            const uint16_t* inC3 = channels[3].ptr<uint16_t>(ry);
            
            for(int x = 0; x < width; x+=2) {
                int rx = x / 2;
                
                outRow1[x]      = inC0[rx];
                outRow1[x + 1]  = inC1[rx];
                outRow2[x]      = inC2[rx];
                outRow2[x + 1]  = inC3[rx];
            }
        }
        
        return outputImage(cv::Rect(cropX, cropY, width - cropX*2, height - cropY*2)).clone();
    }

    void ImageProcessor::writeDng(cv::Mat& rawImage,
                                  const RawCameraMetadata& cameraMetadata,
                                  const RawImageMetadata& imageMetadata,
                                  const std::string& outputPath)
    {
        Measure measure("writeDng()");
        
        const int width  = rawImage.cols;
        const int height = rawImage.rows;
        
        dng_host host;

        host.SetSaveLinearDNG(false);
        host.SetSaveDNGVersion(dngVersion_SaveDefault);
        
        AutoPtr<dng_negative> negative(host.Make_dng_negative());
        
        // Create lens shading map for each channel
        for(int c = 0; c < 4; c++) {
            dng_point channelGainMapPoints(imageMetadata.lensShadingMap[c].rows, imageMetadata.lensShadingMap[c].cols);
            
            AutoPtr<dng_gain_map> gainMap(new dng_gain_map(host.Allocator(),
                                                           channelGainMapPoints,
                                                           dng_point_real64(1.0 / (imageMetadata.lensShadingMap[c].rows), 1.0 / (imageMetadata.lensShadingMap[c].cols)),
                                                           dng_point_real64(0, 0),
                                                           1));
            
            for(int y = 0; y < imageMetadata.lensShadingMap[c].rows; y++) {
                for(int x = 0; x < imageMetadata.lensShadingMap[c].cols; x++) {
                    gainMap->Entry(y, x, 0) = imageMetadata.lensShadingMap[c].at<float>(y, x);
                }
            }
            
            int left = 0;
            int top  = 0;
            
            if(c == 0) {
                left = 0;
                top = 0;
            }
            else if(c == 1) {
                left = 1;
                top = 0;
            }
            else if(c == 2) {
                left = 0;
                top = 1;
            }
            else if(c == 3) {
                left = 1;
                top = 1;
            }
            
            dng_rect gainMapArea(top, left, height, width);
            AutoPtr<dng_opcode> gainMapOpCode(new dng_opcode_GainMap(dng_area_spec(gainMapArea, 0, 1, 2, 2), gainMap));
            
            negative->OpcodeList2().Append(gainMapOpCode);
        }
        
        negative->SetModelName("MotionCam");
        negative->SetLocalName("MotionCam");
        
        // We always use RGGB at this point
        negative->SetColorKeys(colorKeyRed, colorKeyGreen, colorKeyBlue);
                
        negative->SetBayerMosaic(1);
        negative->SetColorChannels(3);
        
        negative->SetQuadBlacks(0, 0, 0, 0);
        negative->SetWhiteLevel(EXPANDED_RANGE);
        
        // Square pixels
        negative->SetDefaultScale(dng_urational(1,1), dng_urational(1,1));
        
        negative->SetDefaultCropSize(width, height);
        negative->SetNoiseReductionApplied(dng_urational(1,1));
        negative->SetCameraNeutral(dng_vector_3(imageMetadata.asShot[0], imageMetadata.asShot[1], imageMetadata.asShot[2]));

        dng_orientation orientation;
        
        switch(imageMetadata.screenOrientation)
        {
            default:
            case ScreenOrientation::PORTRAIT:
                orientation = dng_orientation::Rotate90CW();
                break;
            
            case ScreenOrientation::REVERSE_PORTRAIT:
                orientation = dng_orientation::Rotate90CCW();
                break;
                
            case ScreenOrientation::LANDSCAPE:
                orientation = dng_orientation::Normal();
                break;
                
            case ScreenOrientation::REVERSE_LANDSCAPE:
                orientation = dng_orientation::Rotate180();
                break;
        }
        
        negative->SetBaseOrientation(orientation);

        // Set up camera profile
        AutoPtr<dng_camera_profile> cameraProfile(new dng_camera_profile());
        
        // Color matrices
        cv::Mat color1 = cameraMetadata.colorMatrix1;
        cv::Mat color2 = cameraMetadata.colorMatrix2;
        
        dng_matrix_3by3 dngColor1 = dng_matrix_3by3( color1.at<float>(0, 0), color1.at<float>(0, 1), color1.at<float>(0, 2),
                                                    color1.at<float>(1, 0), color1.at<float>(1, 1), color1.at<float>(1, 2),
                                                    color1.at<float>(2, 0), color1.at<float>(2, 1), color1.at<float>(2, 2) );
        
        dng_matrix_3by3 dngColor2 = dng_matrix_3by3( color2.at<float>(0, 0), color2.at<float>(0, 1), color2.at<float>(0, 2),
                                                    color2.at<float>(1, 0), color2.at<float>(1, 1), color2.at<float>(1, 2),
                                                    color2.at<float>(2, 0), color2.at<float>(2, 1), color2.at<float>(2, 2) );
        
        cameraProfile->SetColorMatrix1(dngColor1);
        cameraProfile->SetColorMatrix2(dngColor2);
        
        // Forward matrices
        cv::Mat forward1 = cameraMetadata.forwardMatrix1;
        cv::Mat forward2 = cameraMetadata.forwardMatrix2;
        
        dng_matrix_3by3 dngForward1 = dng_matrix_3by3( forward1.at<float>(0, 0), forward1.at<float>(0, 1), forward1.at<float>(0, 2),
                                                       forward1.at<float>(1, 0), forward1.at<float>(1, 1), forward1.at<float>(1, 2),
                                                       forward1.at<float>(2, 0), forward1.at<float>(2, 1), forward1.at<float>(2, 2) );
        
        dng_matrix_3by3 dngForward2 = dng_matrix_3by3( forward2.at<float>(0, 0), forward2.at<float>(0, 1), forward2.at<float>(0, 2),
                                                       forward2.at<float>(1, 0), forward2.at<float>(1, 1), forward2.at<float>(1, 2),
                                                       forward2.at<float>(2, 0), forward2.at<float>(2, 1), forward2.at<float>(2, 2) );
        
        cameraProfile->SetForwardMatrix1(dngForward1);
        cameraProfile->SetForwardMatrix2(dngForward2);
        
        uint32_t illuminant1 = 0;
        uint32_t illuminant2 = 0;
        
        // Convert to DNG format
        switch(cameraMetadata.colorIlluminant1) {
            case color::StandardA:
                illuminant1 = lsStandardLightA;
                break;
            case color::StandardB:
                illuminant1 = lsStandardLightB;
                break;
            case color::StandardC:
                illuminant1 = lsStandardLightC;
                break;
            case color::D50:
                illuminant1 = lsD50;
                break;
            case color::D55:
                illuminant1 = lsD55;
                break;
            case color::D65:
                illuminant1 = lsD65;
                break;
            case color::D75:
                illuminant1 = lsD75;
                break;
        }
        
        switch(cameraMetadata.colorIlluminant2) {
            case color::StandardA:
                illuminant2 = lsStandardLightA;
                break;
            case color::StandardB:
                illuminant2 = lsStandardLightB;
                break;
            case color::StandardC:
                illuminant2 = lsStandardLightC;
                break;
            case color::D50:
                illuminant2 = lsD50;
                break;
            case color::D55:
                illuminant2 = lsD55;
                break;
            case color::D65:
                illuminant2 = lsD65;
                break;
            case color::D75:
                illuminant2 = lsD75;
                break;
        }
        
        cameraProfile->SetCalibrationIlluminant1(illuminant1);
        cameraProfile->SetCalibrationIlluminant2(illuminant2);
        
        cameraProfile->SetName("MotionCam");
        cameraProfile->SetEmbedPolicy(pepAllowCopying);
        
        // This ensures profile is saved
        cameraProfile->SetWasReadFromDNG();
        
        negative->AddProfile(cameraProfile);
        
        // Finally add the raw data to the negative
        dng_rect dngArea(height, width);
        dng_pixel_buffer dngBuffer;

        AutoPtr<dng_image> dngImage(host.Make_dng_image(dngArea, 1, ttShort));

        dngBuffer.fArea         = dngArea;
        dngBuffer.fPlane        = 0;
        dngBuffer.fPlanes       = 1;
        dngBuffer.fRowStep      = dngBuffer.fPlanes * width;
        dngBuffer.fColStep      = dngBuffer.fPlanes;
        dngBuffer.fPixelType    = ttShort;
        dngBuffer.fPixelSize    = TagTypeSize(ttShort);
        dngBuffer.fData         = rawImage.ptr();
        
        dngImage->Put(dngBuffer);
        
        // Build the DNG images
        negative->SetStage1Image(dngImage);
        negative->BuildStage2Image(host);
        negative->BuildStage3Image(host);
        
        negative->SynchronizeMetadata();
        
        // Create stream writer for output file
        dng_file_stream dngStream(outputPath.c_str(), true);
        
        // Write DNG file to disk
        AutoPtr<dng_image_writer> dngWriter(new dng_image_writer());
        
        dngWriter->WriteDNG(host, dngStream, *negative.Get(), nullptr, ccUncompressed);
    }
#endif // DNG_SUPPORT
}