#include "video_converter.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/opt.h>
#include <libavutil/imgutils.h>
#include <libswscale/swscale.h>
}

#include <cstdio>

void convert_video_to_h265(const char* input_file, const char* output_file, int thread_count) {
    int ret = 0; // Declare at the top to avoid goto crossing initialization

    // Open the input file
    AVFormatContext* in_fmt_ctx = nullptr;
    if (avformat_open_input(&in_fmt_ctx, input_file, nullptr, nullptr) < 0) {
        fprintf(stderr, "Could not open input file '%s'\n", input_file);
        return;
    }
    if (avformat_find_stream_info(in_fmt_ctx, nullptr) < 0) {
        fprintf(stderr, "Failed to retrieve input stream information\n");
        avformat_close_input(&in_fmt_ctx);
        return;
    }

    // Find the best video stream
    int video_stream_index = av_find_best_stream(in_fmt_ctx, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    if (video_stream_index < 0) {
        fprintf(stderr, "Failed to find video stream in input file\n");
        avformat_close_input(&in_fmt_ctx);
        return;
    }
    AVStream* in_video_stream = in_fmt_ctx->streams[video_stream_index];

    // Open the decoder for the video stream
    const AVCodec* decoder = avcodec_find_decoder(in_video_stream->codecpar->codec_id);
    if (!decoder) {
        fprintf(stderr, "Decoder not found\n");
        avformat_close_input(&in_fmt_ctx);
        return;
    }
    AVCodecContext* dec_ctx = avcodec_alloc_context3(decoder);
    if (!dec_ctx) {
        fprintf(stderr, "Could not allocate decoder context\n");
        avformat_close_input(&in_fmt_ctx);
        return;
    }
    if (avcodec_parameters_to_context(dec_ctx, in_video_stream->codecpar) < 0) {
        fprintf(stderr, "Failed to copy decoder parameters to input decoder context\n");
        avcodec_free_context(&dec_ctx);
        avformat_close_input(&in_fmt_ctx);
        return;
    }
    if (avcodec_open2(dec_ctx, decoder, nullptr) < 0) {
        fprintf(stderr, "Failed to open decoder for stream\n");
        avcodec_free_context(&dec_ctx);
        avformat_close_input(&in_fmt_ctx);
        return;
    }

    // Allocate the output format context (using MP4 container)
    AVFormatContext* out_fmt_ctx = nullptr;
    if (avformat_alloc_output_context2(&out_fmt_ctx, nullptr, "mp4", output_file) < 0) {
        fprintf(stderr, "Could not create output context\n");
        avcodec_free_context(&dec_ctx);
        avformat_close_input(&in_fmt_ctx);
        return;
    }

    // Find the H.265 encoder (HEVC)
    const AVCodec* encoder = avcodec_find_encoder(AV_CODEC_ID_HEVC);
    if (!encoder) {
        fprintf(stderr, "Necessary encoder not found\n");
        avformat_free_context(out_fmt_ctx);
        avcodec_free_context(&dec_ctx);
        avformat_close_input(&in_fmt_ctx);
        return;
    }

    // Create a new video stream in the output file
    AVStream* out_stream = avformat_new_stream(out_fmt_ctx, nullptr);
    if (!out_stream) {
        fprintf(stderr, "Failed allocating output stream\n");
        avformat_free_context(out_fmt_ctx);
        avcodec_free_context(&dec_ctx);
        avformat_close_input(&in_fmt_ctx);
        return;
    }

    // Allocate and configure the encoder context
    AVCodecContext* enc_ctx = avcodec_alloc_context3(encoder);
    if (!enc_ctx) {
        fprintf(stderr, "Failed to allocate the encoder context\n");
        avformat_free_context(out_fmt_ctx);
        avcodec_free_context(&dec_ctx);
        avformat_close_input(&in_fmt_ctx);
        return;
    }

    // Set encoder parameters. You can tweak these values.
    enc_ctx->height = dec_ctx->height;
    enc_ctx->width = dec_ctx->width;
    enc_ctx->sample_aspect_ratio = dec_ctx->sample_aspect_ratio;
    // Use YUV420P pixel format (commonly used by H.265)
    enc_ctx->pix_fmt = AV_PIX_FMT_YUV420P;
    enc_ctx->time_base = av_inv_q(dec_ctx->framerate.num ? dec_ctx->framerate : in_video_stream->r_frame_rate);
    // Set preset options if supported
    av_opt_set(enc_ctx->priv_data, "preset", "medium", 0);
    // Set the number of threads via AVOptions
    av_opt_set_int(enc_ctx->priv_data, "threads", thread_count, 0);
    
    // Open the encoder
    if (avcodec_open2(enc_ctx, encoder, nullptr) < 0) {
        fprintf(stderr, "Cannot open video encoder for stream\n");
        avcodec_free_context(&enc_ctx);
        avformat_free_context(out_fmt_ctx);
        avcodec_free_context(&dec_ctx);
        avformat_close_input(&in_fmt_ctx);
        return;
    }

    // Copy encoder parameters to the output stream
    if (avcodec_parameters_from_context(out_stream->codecpar, enc_ctx) < 0) {
        fprintf(stderr, "Failed to copy encoder parameters to output stream\n");
        avcodec_free_context(&enc_ctx);
        avformat_free_context(out_fmt_ctx);
        avcodec_free_context(&dec_ctx);
        avformat_close_input(&in_fmt_ctx);
        return;
    }
    out_stream->time_base = enc_ctx->time_base;

    // Open the output file if needed
    if (!(out_fmt_ctx->oformat->flags & AVFMT_NOFILE)) {
        if (avio_open(&out_fmt_ctx->pb, output_file, AVIO_FLAG_WRITE) < 0) {
            fprintf(stderr, "Could not open output file '%s'\n", output_file);
            avcodec_free_context(&enc_ctx);
            avformat_free_context(out_fmt_ctx);
            avcodec_free_context(&dec_ctx);
            avformat_close_input(&in_fmt_ctx);
            return;
        }
    }

    // Write the stream header to the output file
    if (avformat_write_header(out_fmt_ctx, nullptr) < 0) {
        fprintf(stderr, "Error occurred when opening output file\n");
        if (!(out_fmt_ctx->oformat->flags & AVFMT_NOFILE))
            avio_closep(&out_fmt_ctx->pb);
        avcodec_free_context(&enc_ctx);
        avformat_free_context(out_fmt_ctx);
        avcodec_free_context(&dec_ctx);
        avformat_close_input(&in_fmt_ctx);
        return;
    }

    // Allocate frames and packets for conversion
    AVFrame* frame_decoded = av_frame_alloc();
    AVFrame* frame_converted = av_frame_alloc();
    AVPacket* packet_in = av_packet_alloc();
    AVPacket* packet_out = av_packet_alloc();

    // Prepare a scaler if the decoder pixel format isn't YUV420P (required by our encoder)
    struct SwsContext* sws_ctx = sws_getContext(dec_ctx->width, dec_ctx->height, dec_ctx->pix_fmt,
                                                enc_ctx->width, enc_ctx->height, enc_ctx->pix_fmt,
                                                SWS_BICUBIC, nullptr, nullptr, nullptr);
    if (!sws_ctx) {
        fprintf(stderr, "Could not initialize the conversion context\n");
        goto cleanup;
    }

    // Allocate buffer for the converted frame
    ret = av_image_alloc(frame_converted->data, frame_converted->linesize,
                         enc_ctx->width, enc_ctx->height, enc_ctx->pix_fmt, 32);
    if (ret < 0) {
        fprintf(stderr, "Could not allocate raw picture buffer\n");
        goto cleanup;
    }
    frame_converted->width  = enc_ctx->width;
    frame_converted->height = enc_ctx->height;
    frame_converted->format = enc_ctx->pix_fmt;
    frame_converted->pts = 0;

    // Main conversion loop: read, decode, convert, encode, and write
    while (av_read_frame(in_fmt_ctx, packet_in) >= 0) {
        if (packet_in->stream_index == video_stream_index) {
            ret = avcodec_send_packet(dec_ctx, packet_in);
            if (ret < 0) {
                fprintf(stderr, "Error sending packet for decoding\n");
                break;
            }
            while (ret >= 0) {
                ret = avcodec_receive_frame(dec_ctx, frame_decoded);
                if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF)
                    break;
                else if (ret < 0) {
                    fprintf(stderr, "Error during decoding\n");
                    goto cleanup;
                }

                // Convert the frame to the encoder's pixel format
                sws_scale(sws_ctx, frame_decoded->data, frame_decoded->linesize, 0, dec_ctx->height,
                          frame_converted->data, frame_converted->linesize);
                frame_converted->pts = frame_decoded->pts;

                // Encode the frame
                ret = avcodec_send_frame(enc_ctx, frame_converted);
                if (ret < 0) {
                    fprintf(stderr, "Error sending frame for encoding\n");
                    goto cleanup;
                }
                while (ret >= 0) {
                    ret = avcodec_receive_packet(enc_ctx, packet_out);
                    if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF)
                        break;
                    else if (ret < 0) {
                        fprintf(stderr, "Error during encoding\n");
                        goto cleanup;
                    }
                    // Rescale packet timestamp
                    av_packet_rescale_ts(packet_out, enc_ctx->time_base, out_stream->time_base);
                    packet_out->stream_index = out_stream->index;
                    // Write packet
                    if (av_interleaved_write_frame(out_fmt_ctx, packet_out) < 0) {
                        fprintf(stderr, "Error while writing output packet\n");
                        goto cleanup;
                    }
                    av_packet_unref(packet_out);
                }
                av_frame_unref(frame_decoded);
            }
        }
        av_packet_unref(packet_in);
    }

    // Write trailer to output file
    av_write_trailer(out_fmt_ctx);

cleanup:
    if (sws_ctx)
        sws_freeContext(sws_ctx);
    av_freep(&frame_converted->data[0]);
    av_frame_free(&frame_converted);
    av_frame_free(&frame_decoded);
    av_packet_free(&packet_in);
    av_packet_free(&packet_out);
    if (out_fmt_ctx && !(out_fmt_ctx->oformat->flags & AVFMT_NOFILE))
        avio_closep(&out_fmt_ctx->pb);
    avcodec_free_context(&enc_ctx);
    avcodec_free_context(&dec_ctx);
    avformat_close_input(&in_fmt_ctx);
    avformat_free_context(out_fmt_ctx);
}
