#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/avutil.h>
#include <libswscale/swscale.h>
#include <stdio.h>

int main(int argc, char *argv[])
{
    // 在新版本的FFmpeg中，av_register_all()已被弃用
    printf("FFmpeg版本: %s\n", av_version_info());
/**
 * ! 打开文件
 */
    AVFormatContext *pFormatCtx;
    // 打开video文件
    if(avformat_open_input(&pFormatCtx, argv[1], NULL, NULL) != 0){
        printf("无法打开视频文件\n");
        return -1;
    }
    // 检查文件流信息
    if(avformat_find_stream_info(pFormatCtx, NULL) < 0){
        printf("无法获取视频流信息\n");
        return -1;
    }
    // 手工调试函数
    av_dump_format(pFormatCtx, 0, argv[1], 0);

    int i;
    int videoStream=-1;
    AVCodecContext *pCodecCtx;

    // 找到第一个视频流
    for(i=0; i<pFormatCtx->nb_streams; i++) {
        if(pFormatCtx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
            videoStream=i;
            break;
        }
        if(videoStream == -1){
            printf("无法找到视频流\n");
            return -1;
        }
    }
    // 获得视频流的指针
    pCodecCtx = pFormatCtx->streams[videoStream]->codec;

    // 找到编解码器
    AVCodec *pCodec;
    // 找到解码器
    pCodec = avcodec_find_decoder(pCodecCtx->codec_id);

    if(pCodec == NULL){
        printf("无法找到解码器\n");
        return -1;
    }

    // 打开解码器
    if(avcodec_open(pCodecCtx, pCodec) < 0){
        printf("无法打开解码器\n");
        return -1;
    }

/**
 * ! 保存数据
 */
    AVFrame *pFrame;
    AVFrame *pFrameRGB;
    // 分配视频帧
    pFrame = av_frame_alloc();

    // 申请内存
    pFrameRGB = avcodec_alloc_frame();
    if(pFrameRGB == NULL){
        printf("无法分配内存\n");
        return -1;
    }

    uint8_t *buffer;
    int numBytes;
    // 手工申请内存空间
    numBytes = avpicture_get_size(AV_PIX_FMT_RGB24, pCodecCtx->width, pCodecCtx->height);
    buffer = (uint8_t *)av_malloc(numBytes * sizeof(uint8_t));

    // 帧和内存组合
    avpicture_fill((AVPicture *)pFrameRGB, buffer, AV_PIX_FMT_RGB24, pCodecCtx->width, pCodecCtx->height);

/**
 * ! 读取数据
 */
    int frameFinished;
    AVPacket packet;

    i = 0;
    while(av_read_frame(pFormatCtx, &packet) >= 0){
        // 是视频流
        if(packet.stream_index == videoStream){
            // 解码视频帧
            avcodec_decode_video2(pCodecCtx, pFrame, &frameFinished, &packet);
            // 解码成功
            if(frameFinished){
                // 转换像素格式
                img_convert((AVPicture *)pFrameRGB, AV_PIX_FMT_RGB24, (AVPicture *)pFrame, pCodecCtx->pix_fmt, pCodecCtx->width, pCodecCtx->height);
                // 保存帧
                if(i++ <= 5){
                    SaveFrame(pFrameRGB, pCodecCtx->width, pCodecCtx->height, i);
                }
            }
        }
        // 释放资源
        av_free_packet(&packet);
    }

    // 清理RGB图像
    av_free(buffer);
    av_free(pFrameRGB);

    // 清理YUV帧
    av_free(pFrame);

    // 关闭解码器
    avcodec_close(pCodecCtx);

    // 关闭文件
    avformat_close_input(&pFormatCtx);

    return 0;
}

// 把RGB信息定稿到PPM格式的文件
void SaveFrame(AVFrame *pFrame, int width, int height, int iFrame)
{
    FILE *pFile;
    char szFilename[32];
    int  y;

    // Open file
    sprintf(szFilename, "frame%d.ppm", iFrame);
    pFile=fopen(szFilename, "wb");
    if(pFile==NULL)
        return;

    // Write header
    fprintf(pFile, "P6\n%d %d\n255\n", width, height);

    // 一次向文件写入一行数据
    // PPM格式：包含一长串RGB数据的文件
    for(y=0; y<height; y++)
        fwrite(pFrame->data[0]+y*pFrame->linesize[0], 1, width*3, pFile);

    // Close file
    fclose(pFile);
}
