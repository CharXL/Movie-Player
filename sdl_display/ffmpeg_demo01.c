#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/avutil.h>
#include <libavutil/imgutils.h>
#include <libswscale/swscale.h>
#include <stdio.h>
#include <sys/stat.h>
#include <string.h>
#include <SDL2/SDL.h>
#include <SDL2/SDL_thread.h>

// SDL 全局变量
SDL_Window *window = NULL;
SDL_Renderer *renderer = NULL;
SDL_Texture *texture = NULL;

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

    // 检查输出目录
    struct stat st = {0};
    if (stat(output_dir, &st) == -1) {
        return -1;
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

    // 分配视频帧
    AVFrame *pFrame = av_frame_alloc();
    if(pFrame == NULL){
        printf("无法分配帧内存\n");
        return -1;
    }

/**
 * ! 初始化SDL2
 */
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_TIMER) < 0) {
        printf("无法初始化SDL: %s\n", SDL_GetError());
        return -1;
    }

/**
 * ! 创建SDL2窗口和渲染器
 */
    window = SDL_CreateWindow(
        "Video Player",
        SDL_WINDOWPOS_UNDEFINED,
        SDL_WINDOWPOS_UNDEFINED,
        pCodecCtx->width,
        pCodecCtx->height,
        SDL_WINDOW_SHOWN
    );
    if (!window) {
        printf("无法创建窗口: %s\n", SDL_GetError());
        return -1;
    }

    renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_ACCELERATED);
    if (!renderer) {
        printf("无法创建渲染器: %s\n", SDL_GetError());
        SDL_DestroyWindow(window);
        return -1;
    }

    texture = SDL_CreateTexture(
        renderer,
        SDL_PIXELFORMAT_IYUV,
        SDL_TEXTUREACCESS_STREAMING,
        pCodecCtx->width,
        pCodecCtx->height
    );
    if (!texture) {
        printf("无法创建纹理: %s\n", SDL_GetError());
        SDL_DestroyRenderer(renderer);
        SDL_DestroyWindow(window);
        return -1;
    }

/**
 * ! 读取数据
 */
    int frameFinished;
    AVPacket packet;
    av_init_packet(&packet);

    SDL_Event event;
    int quit = 0;

    i = 0;
    while (av_read_frame(pFormatCtx, &packet) >= 0 && !quit) {
        if (packet.stream_index == videoStream) {
            // 解码视频帧
            int ret = avcodec_send_packet(pCodecCtx, &packet);
            if (ret < 0) {
                printf("发送数据包失败\n");
                continue;
            }

            ret = avcodec_receive_frame(pCodecCtx, pFrame);
            if (ret < 0) {
                continue;
            }

            // 更新纹理
            SDL_UpdateYUVTexture(texture, NULL,
                pFrame->data[0], pFrame->linesize[0],    // Y
                pFrame->data[1], pFrame->linesize[1],    // U
                pFrame->data[2], pFrame->linesize[2]     // V
            );

            // 渲染
            SDL_RenderClear(renderer);
            SDL_RenderCopy(renderer, texture, NULL, NULL);
            SDL_RenderPresent(renderer);

            // 控制帧率
            SDL_Delay(40);  // 约25fps
        }

        // 处理SDL事件
        while (SDL_PollEvent(&event)) {
            if (event.type == SDL_QUIT) {
                quit = 1;
                break;
            }
        }

        av_packet_unref(&packet);
    }

/**
 * ! 清理资源
 */
    // 清理SDL资源
    SDL_DestroyTexture(texture);
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    SDL_Quit();

    // 清理FFmpeg资源
    av_frame_free(&pFrame);
    avcodec_free_context(&pCodecCtx);
    avformat_close_input(&pFormatCtx);

    return 0;
}

/**
 * ! SDL输出到屏幕
 */
void sdl_player(AVCodecContext *pCodecCtx, AVFrame *pFrame, AVPacket packet) {
    if (!texture || !renderer) {
        return;
    }

    // 更新纹理
    SDL_UpdateYUVTexture(texture, NULL,
        pFrame->data[0], pFrame->linesize[0],    // Y
        pFrame->data[1], pFrame->linesize[1],    // U
        pFrame->data[2], pFrame->linesize[2]     // V
    );

    // 渲染到屏幕
    SDL_RenderClear(renderer);
    SDL_RenderCopy(renderer, texture, NULL, NULL);
    SDL_RenderPresent(renderer);

    // 事件处理
    SDL_Event event;
    while (SDL_PollEvent(&event)) {
        if (event.type == SDL_QUIT) {
            SDL_DestroyTexture(texture);
            SDL_DestroyRenderer(renderer);
            SDL_DestroyWindow(window);
            SDL_Quit();
            exit(0);
        }
    }
}
