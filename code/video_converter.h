#ifndef IMAGE_CONVERTER_H
#define IMAGE_CONVERTER_H

#ifdef __cplusplus
extern "C" {
#endif

// Converts an image (in any supported format) to a JPEG image.
// input_file  - path to the source image file (e.g., PNG, BMP, TIFF, etc.)
// output_file - path to the JPEG output file.
void convert_video_to_h265(const char* input_file, const char* output_file, int thread_count);

#ifdef __cplusplus
}
#endif

#endif // IMAGE_CONVERTER_H
