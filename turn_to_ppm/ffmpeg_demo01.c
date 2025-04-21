#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/avutil.h>
#include <libswscale/swscale.h>
#include <stdio.h>
#include <sys/stat.h>
#include <string.h>

// 声明SaveFrame函数
void SaveFrame(AVFrame *pFrame, int width, int height, int iFrame, const char *outdir);

int main(int argc, char *argv[])
{
/**
 * ! 打开文件
 */
    // 在新版本的FFmpeg中，av_register_all()已被弃用
    printf("FFmpeg版本: %s\n", av_version_info());

    // 检查命令行参数
    if (argc != 3) {
        printf("用法: %s <视频文件路径> <输出文件夹>\n", argv[0]);
        return -1;
    }

    const char *input_file = argv[1];
    const char *output_dir = argv[2];

    // 检查并创建输出目录
    struct stat st = {0};
    if (stat(output_dir, &st) == -1) {
        #ifdef _WIN32
            if (mkdir(output_dir) != 0) {
        #else
            if (mkdir(output_dir, 0700) != 0) {
        #endif
                printf("错误：无法创建输出目录 '%s'\n", output_dir);
                return -1;
            }
    }

    // 检查文件是否存在并且可读
    FILE *file = fopen(input_file, "rb");
    if (!file) {
        printf("错误：文件 '%s' 不存在或无法访问\n", input_file);
        return -1;
    }
    fclose(file);

    // 初始化格式上下文
    printf("正在打开文件: %s\n", input_file);
    
    // 创建格式上下文
    AVFormatContext *pFormatCtx = avformat_alloc_context();
    if (!pFormatCtx) {
        printf("无法分配格式上下文\n");
        return -1;
    }

    // 打开输入文件
    int ret = avformat_open_input(&pFormatCtx, input_file, NULL, NULL);
    if (ret < 0) {
        char errbuf[AV_ERROR_MAX_STRING_SIZE];
        av_strerror(ret, errbuf, AV_ERROR_MAX_STRING_SIZE);
        printf("无法打开文件: %s (错误码: %d)\n", errbuf, ret);
        avformat_free_context(pFormatCtx);
        return -1;
    }
    // 检查文件流信息
    if(avformat_find_stream_info(pFormatCtx, NULL) < 0){
        printf("无法获取视频流信息\n");
        return -1;
    }
    // 手工调试函数
    av_dump_format(pFormatCtx, 0, input_file, 0);

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
    // 获得视频流的编解码器参数
    AVCodecParameters *pCodecPar = pFormatCtx->streams[videoStream]->codecpar;

    // 找到编解码器
    AVCodec *pCodec;
    // 找到解码器
    pCodec = avcodec_find_decoder(pCodecPar->codec_id);

    if(pCodec == NULL){
        printf("无法找到解码器\n");
        return -1;
    }

    // 分配编解码器上下文
    pCodecCtx = avcodec_alloc_context3(pCodec);
    if(pCodecCtx == NULL){
        printf("无法分配编解码器上下文\n");
        return -1;
    }

    // 将编解码器参数复制到上下文
    if(avcodec_parameters_to_context(pCodecCtx, pCodecPar) < 0){
        printf("无法复制编解码器参数\n");
        return -1;
    }

    // 打开解码器
    if(avcodec_open2(pCodecCtx, pCodec, NULL) < 0){
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
    if(pFrame == NULL){
        printf("无法分配帧内存\n");
        return -1;
    }

    // 申请RGB帧内存
    pFrameRGB = av_frame_alloc();
    if(pFrameRGB == NULL){
        printf("无法分配内存\n");
        return -1;
    }

    uint8_t *buffer;
    int numBytes;
    // 手工申请内存空间
    numBytes = av_image_get_buffer_size(AV_PIX_FMT_RGB24, pCodecCtx->width, pCodecCtx->height, 1);
    buffer = (uint8_t *)av_malloc(numBytes * sizeof(uint8_t));

    // 帧和内存组合
    av_image_fill_arrays(pFrameRGB->data, pFrameRGB->linesize, buffer, AV_PIX_FMT_RGB24, pCodecCtx->width, pCodecCtx->height, 1);

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
            int ret = avcodec_send_packet(pCodecCtx, &packet);
            if (ret < 0) {
                printf("发送数据包失败\n");
                continue;
            }

            ret = avcodec_receive_frame(pCodecCtx, pFrame);
            if (ret < 0) {
                // 需要更多数据包或者出错
                continue;
            }

            // 创建转换上下文
            struct SwsContext *sws_ctx = sws_getContext(
                pCodecCtx->width, pCodecCtx->height, pCodecCtx->pix_fmt,
                pCodecCtx->width, pCodecCtx->height, AV_PIX_FMT_RGB24,
                SWS_BILINEAR, NULL, NULL, NULL
            );

            if (!sws_ctx) {
                printf("无法创建转换上下文\n");
                continue;
            }

            // 转换像素格式
            sws_scale(sws_ctx, (const uint8_t * const*)pFrame->data, pFrame->linesize, 0,
                      pCodecCtx->height, pFrameRGB->data, pFrameRGB->linesize);

            // 释放转换上下文
            sws_freeContext(sws_ctx);

            // 保存帧
            if(i++ <= 5) {
                SaveFrame(pFrameRGB, pCodecCtx->width, pCodecCtx->height, i, output_dir);
            }
        }
        // 释放资源
        av_packet_unref(&packet);
    }

    // 清理RGB图像
    av_free(buffer);
    av_frame_free(&pFrameRGB);

    // 清理YUV帧
    av_frame_free(&pFrame);

    // 关闭解码器
    avcodec_free_context(&pCodecCtx);

    // 关闭文件
    avformat_close_input(&pFormatCtx);

    return 0;
}

// 把RGB信息定稿到PPM格式的文件
void SaveFrame(AVFrame *pFrame, int width, int height, int iFrame, const char *outdir)
{
    FILE *pFile;
    char szFilename[512];  // 增加缓冲区大小以适应路径
    int  y;

    // 构建完整的文件路径
    #ifdef _WIN32
        snprintf(szFilename, sizeof(szFilename), "%s\\frame%d.ppm", outdir, iFrame);
    #else
        snprintf(szFilename, sizeof(szFilename), "%s/frame%d.ppm", outdir, iFrame);
    #endif

    pFile = fopen(szFilename, "wb");
    if(pFile == NULL) {
        printf("错误：无法创建文件 '%s'\n", szFilename);
        return;
    }

    // Write header
    fprintf(pFile, "P6\n%d %d\n255\n", width, height);

    // 一次向文件写入一行数据
    // PPM格式：包含一长串RGB数据的文件
    for(y=0; y<height; y++)
        fwrite(pFrame->data[0]+y*pFrame->linesize[0], 1, width*3, pFile);

    // Close file
    fclose(pFile);
    printf("已保存帧 %d 到 %s\n", iFrame, szFilename);
}
