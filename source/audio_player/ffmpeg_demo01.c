#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/avutil.h>
#include <libavutil/imgutils.h>
#include <libavutil/opt.h>
#include <libswscale/swscale.h>
#include <libswresample/swresample.h>
#include <stdio.h>
#include <sys/stat.h>
#include <string.h>
#include <SDL2/SDL.h>
#include <SDL2/SDL_thread.h>

// 音频回调结构体
typedef struct AudioState {
    AVFormatContext *format_ctx;
    AVCodecContext *audio_ctx;
    SwrContext *swr_ctx;        // 重采样上下文
    uint8_t *audio_buf;         // 音频缓冲区
    unsigned int audio_buf_size; // 缓冲区大小
    unsigned int audio_buf_index;// 当前播放位置
    AVFrame *audio_frame;       // 音频帧
    AVPacket audio_pkt;         // 音频包
    SDL_AudioSpec wanted_spec;  // SDL音频参数
    int audio_stream_idx;       // 添加音频流索引
} AudioState;

// 声明函数
void SaveFrame(AVFrame *pFrame, int width, int height, int iFrame, const char *outdir);
int init_audio(AudioState *audio);
void audio_callback(void *userdata, Uint8 *stream, int len);
int audio_decode_frame(AudioState *audio);

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
    int audioStream=-1;
    AVCodecContext *pCodecCtx;
    AVCodecContext *aCodecCtx;

    // 找到第一个视频流和音频流
    for(i=0; i<pFormatCtx->nb_streams; i++) {
        if(pFormatCtx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO && videoStream == -1) {
            videoStream=i;
        }
        if(pFormatCtx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_AUDIO && audioStream == -1) {
            audioStream=i;
        }
    }
    if(videoStream == -1){
        printf("无法找到视频流\n");
        return -1;
    }
    if(audioStream == -1){
        printf("无法找到音频流\n");
        return -1;
    }
    // 获得视频流的编解码器参数
    AVCodecParameters *pCodecPar = pFormatCtx->streams[videoStream]->codecpar;
    AVCodecParameters *aCodecPar = pFormatCtx->streams[audioStream]->codecpar;

    // 找到编解码器
    AVCodec *pCodec;
    AVCodec *aCodec;
    // 找到解码器
    pCodec = avcodec_find_decoder(pCodecPar->codec_id);
    aCodec = avcodec_find_decoder(aCodecPar->codec_id);

    if(pCodec == NULL || aCodec == NULL){
        printf("无法找到解码器\n");
        return -1;
    }

    // 分配编解码器上下文
    pCodecCtx = avcodec_alloc_context3(pCodec);
    aCodecCtx = pFormatCtx->streams[audioStream]->codec;
    if(pCodecCtx == NULL || aCodecCtx == NULL){
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
    if(avcodec_open2(aCodecCtx, aCodec, NULL) < 0){
        printf("无法打开解码器\n");
        return -1;
    }

    // 初始化SDL
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_TIMER) < 0) {
        printf("无法初始化SDL: %s\n", SDL_GetError());
        return -1;
    }

    // 创建音频状态结构体
    AudioState *audio_state = (AudioState *)malloc(sizeof(AudioState));
    if (!audio_state) {
        printf("无法分配音频状态内存\n");
        return -1;
    }

    // 初始化音频状态
    memset(audio_state, 0, sizeof(AudioState));
    audio_state->format_ctx = pFormatCtx;
    audio_state->audio_ctx = aCodecCtx;
    audio_state->audio_stream_idx = audioStream;
    audio_state->audio_frame = av_frame_alloc();

    // 初始化重采样上下文
    audio_state->swr_ctx = swr_alloc();
    if (!audio_state->swr_ctx) {
        printf("无法创建重采样上下文\n");
        return -1;
    }

    // 设置重采样参数
    av_opt_set_int(audio_state->swr_ctx, "in_channel_layout", aCodecCtx->channel_layout, 0);
    av_opt_set_int(audio_state->swr_ctx, "out_channel_layout", aCodecCtx->channel_layout, 0);
    av_opt_set_int(audio_state->swr_ctx, "in_sample_rate", aCodecCtx->sample_rate, 0);
    av_opt_set_int(audio_state->swr_ctx, "out_sample_rate", aCodecCtx->sample_rate, 0);
    av_opt_set_sample_fmt(audio_state->swr_ctx, "in_sample_fmt", aCodecCtx->sample_fmt, 0);
    av_opt_set_sample_fmt(audio_state->swr_ctx, "out_sample_fmt", AV_SAMPLE_FMT_S16, 0);

    if (swr_init(audio_state->swr_ctx) < 0) {
        printf("无法初始化重采样上下文\n");
        return -1;
    }

    // 分配音频缓冲区
    audio_state->audio_buf = (uint8_t *)av_malloc(192000); // 足够大的缓冲区

    // 初始化音频
    if(init_audio(audio_state) < 0){
        printf("初始化音频失败\n");
        return -1;
    }

/**
 * ! 保存视频数据
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
 * ! 读取视频数据
 */
    int frameFinished;
    AVPacket packet;

    // 初始化SDL视频组件
    SDL_Window *window = NULL;
    SDL_Renderer *renderer = NULL;
    SDL_Texture *texture = NULL;

    // 创建窗口
    window = SDL_CreateWindow(
        "FFmpeg SDL2 Player",
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

    // 创建渲染器
    renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_ACCELERATED);
    if (!renderer) {
        printf("无法创建渲染器: %s\n", SDL_GetError());
        SDL_DestroyWindow(window);
        return -1;
    }

    // 创建纹理
    texture = SDL_CreateTexture(
        renderer,
        SDL_PIXELFORMAT_RGB24,
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

    // 创建转换上下文
    struct SwsContext *sws_ctx = sws_getContext(
        pCodecCtx->width, pCodecCtx->height, pCodecCtx->pix_fmt,
        pCodecCtx->width, pCodecCtx->height, AV_PIX_FMT_RGB24,
        SWS_BILINEAR, NULL, NULL, NULL
    );

    if (!sws_ctx) {
        printf("无法创建转换上下文\n");
        return -1;
    }

    SDL_Rect rect;
    rect.x = 0;
    rect.y = 0;
    rect.w = pCodecCtx->width;
    rect.h = pCodecCtx->height;

    SDL_Event event;
    int quit = 0;
    i = 0;

    while(!quit && av_read_frame(pFormatCtx, &packet) >= 0){
        // 处理SDL事件
        while (SDL_PollEvent(&event)) {
            if (event.type == SDL_QUIT) {
                quit = 1;
                break;
            }
        }

        // 是视频流
        if(packet.stream_index == videoStream){
            // 解码视频帧
            int ret = avcodec_send_packet(pCodecCtx, &packet);
            if (ret < 0) {
                printf("发送数据包失败\n");
                av_packet_unref(&packet);
                continue;
            }

            ret = avcodec_receive_frame(pCodecCtx, pFrame);
            if (ret < 0) {
                // 需要更多数据包或者出错
                av_packet_unref(&packet);
                continue;
            }

            // 转换像素格式
            sws_scale(sws_ctx, (const uint8_t * const*)pFrame->data, pFrame->linesize, 0,
                      pCodecCtx->height, pFrameRGB->data, pFrameRGB->linesize);

            // 更新纹理
            SDL_UpdateTexture(texture, NULL, pFrameRGB->data[0], pFrameRGB->linesize[0]);
            SDL_RenderClear(renderer);
            SDL_RenderCopy(renderer, texture, NULL, &rect);
            SDL_RenderPresent(renderer);

            // 保存帧
            if(i++ <= 5) {
                SaveFrame(pFrameRGB, pCodecCtx->width, pCodecCtx->height, i, output_dir);
            }

            // 控制帧率
            SDL_Delay(40); // 约25fps
        }

        // 释放资源
        av_packet_unref(&packet);
    }

    // 释放 SDL 资源
    sws_freeContext(sws_ctx);
    SDL_DestroyTexture(texture);
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);

    // 清理RGB图像
    av_free(buffer);
    av_frame_free(&pFrameRGB);

    // 清理YUV帧
    av_frame_free(&pFrame);

    // 关闭解码器
    avcodec_free_context(&pCodecCtx);
    avcodec_free_context(&aCodecCtx);

    // 清理音频资源
    if (audio_state) {
        if (audio_state->audio_frame) {
            av_frame_free(&audio_state->audio_frame);
        }
        if (audio_state->swr_ctx) {
            swr_free(&audio_state->swr_ctx);
        }
        if (audio_state->audio_buf) {
            av_free(audio_state->audio_buf);
        }
        free(audio_state);
    }

    // 关闭文件
    avformat_close_input(&pFormatCtx);

    // 关闭SDL
    SDL_Quit();

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
// 初始化音频
int init_audio(AudioState *audio) {
    // 设置音频参数
    audio->wanted_spec.freq = audio->audio_ctx->sample_rate;
    audio->wanted_spec.format = AUDIO_S16SYS;
    audio->wanted_spec.channels = audio->audio_ctx->channels;
    audio->wanted_spec.silence = 0;
    audio->wanted_spec.samples = 1024;
    audio->wanted_spec.callback = audio_callback;
    audio->wanted_spec.userdata = audio;

    // 打开音频设备
    SDL_AudioSpec obtained_spec;
    int audio_device_id = SDL_OpenAudioDevice(NULL, 0, &audio->wanted_spec, &obtained_spec, 0);
    if (audio_device_id <= 0) {
        fprintf(stderr, "SDL_OpenAudioDevice: %s\n", SDL_GetError());
        return -1;
    }

    // 开始播放音频
    SDL_PauseAudioDevice(audio_device_id, 0);

    // 初始化音频缓冲区索引
    audio->audio_buf_index = 0;
    audio->audio_buf_size = 0;

    // 初始化音频包
    av_init_packet(&audio->audio_pkt);

    return 0;
}
// 音频回调函数
void audio_callback(void *userdata, Uint8 *stream, int len) {
    AudioState *audio = (AudioState *)userdata;
    int len1, audio_size;

    // 首先清空流
    memset(stream, 0, len);

    while (len > 0) {
        if (audio->audio_buf_index >= audio->audio_buf_size) {
            // 需要更多数据
            audio_size = audio_decode_frame(audio);
            if (audio_size < 0) {
                // 错误处理，这里填充静音
                audio->audio_buf_size = 0;
                break;
            }
            audio->audio_buf_index = 0;
        }
        len1 = audio->audio_buf_size - audio->audio_buf_index;
        if (len1 > len)
            len1 = len;

        // 混合音频数据到输出流（而不是直接覆盖）
        SDL_MixAudioFormat(stream, audio->audio_buf + audio->audio_buf_index, AUDIO_S16SYS, len1, SDL_MIX_MAXVOLUME);

        len -= len1;
        stream += len1;
        audio->audio_buf_index += len1;
    }
}
// 解码音频
int audio_decode_frame(AudioState *audio) {
    int data_size = 0;
    int ret = 0;

    while (1) {
        // 如果没有包，读取新的音频包
        if (audio->audio_pkt.size <= 0) {
            ret = av_read_frame(audio->format_ctx, &audio->audio_pkt);
            if (ret < 0) {
                // 文件结尾或错误
                return -1;
            }

            // 确保是音频包
            if (audio->audio_pkt.stream_index != audio->audio_stream_idx) {
                av_packet_unref(&audio->audio_pkt);
                continue;
            }
        }

        // 发送包到解码器
        ret = avcodec_send_packet(audio->audio_ctx, &audio->audio_pkt);
        if (ret < 0) {
            if (ret == AVERROR(EAGAIN)) {
                // 解码器需要更多数据
                continue;
            }
            av_packet_unref(&audio->audio_pkt);
            return -1;
        }

        // 包已经被解码器接收，可以释放了
        av_packet_unref(&audio->audio_pkt);
        audio->audio_pkt.size = 0;

        // 接收解码后的帧
        ret = avcodec_receive_frame(audio->audio_ctx, audio->audio_frame);
        if (ret < 0) {
            if (ret == AVERROR(EAGAIN)) {
                // 需要更多数据
                continue;
            }
            return -1;
        }

        // 成功获取到帧，进行重采样
        // 计算输出样本数
        int out_samples = av_rescale_rnd(
            swr_get_delay(audio->swr_ctx, audio->audio_ctx->sample_rate) + audio->audio_frame->nb_samples,
            audio->wanted_spec.freq,
            audio->audio_ctx->sample_rate,
            AV_ROUND_UP);

        // 重采样转换
        uint8_t **out_buffer = &audio->audio_buf;
        ret = swr_convert(audio->swr_ctx,
            out_buffer,
            out_samples,
            (const uint8_t **)audio->audio_frame->data,
            audio->audio_frame->nb_samples);

        if (ret < 0) {
            fprintf(stderr, "Error while converting\n");
            return -1;
        }

        // 计算实际数据大小
        data_size = ret * audio->wanted_spec.channels * 2; // 2 for 16 bit samples
        audio->audio_buf_size = data_size;

        return data_size;
    }
}
