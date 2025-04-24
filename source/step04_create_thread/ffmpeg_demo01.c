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
#include <SDL2/SDL_audio.h>
#include <SDL2/SDL_video.h>

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
// 视频结构体
typedef struct VideoState {
    AVFormatContext *pFormatCtx;
    int videoStream, audioStream;
    AVStream *audio_st;
    PacketQueue audioq;
    uint8_t *buffer;
    unsigned int audio_buf_size; // 缓冲区大小
    unsigned int audio_buf_index;// 当前播放位置
    AVPacket audio_pkt;
    uint8_t *audio_pkt_data;
    int audio_pkt_size;
    AVStream *video_st;
    PacketQueue videoq;
    VideoPicture pictq[VIDEO_PICTURE_QUEUE_SIZE];
    int pictq_size, pictq_rindex, pictq_windex;
    SDL_mutex *pictq_mutex;
    SDL_cond *pictq_cond;
    SDL_Thread *parse_tid;
    SDL_Thread *video_tid;
    char filename[1024];
    int quit;
} VideoState;
// 图像队列结构体
typedef struct VideoPicture {
    SDL_Overlay *bmp;
    int width, height;
    int allocated;
} VideoPicture;

// 声明函数
void SaveFrame(AVFrame *pFrame, int width, int height, int iFrame, const char *outdir);
int init_audio(AudioState *audio);
void audio_callback(void *userdata, Uint8 *stream, int len);
int audio_decode_frame(AudioState *audio);

// 声明变量
VideoState *global_video_state;

int main(int argc, char *argv[])
{
    SDL_Event event;
    VideoState *is;
    is = (VideoState *)av_mallocz(sizeof(VideoState));

    pstrcpy(is->filename, sizeof(is->filename), argv[1]);

    is->pictq_mutex = SDL_CreateMutex();
    is->pictq_cond = SDL_CreateCond();

/**
 * ! 第一个线程
 */
    schedule_refresh(is, 40);

    is->parse_tid = SDL_CreateThread(decode_thread, "decode_thread", is);
    if (!is->parse_tid) {
        av_free(is);
        return -1;
    }

    for(;;){
        if(is->quit){
            break;
        }
        // 进行seek操作
        if(is->audio.size > MAX_AUDIOQ_SIZE || is->videoq.size > MAX_VIDEOQ_SIZE){
            SDL_Delay(10);
            continue;
        }
        if(av_read_frame(is->pFormatCtx, packet) < 0){
            if(url_feof(is->pFormatCtx->pb) == 0){
                SDL_Delay(100);
                continue;
            }
            else{
                break;
            }
        }
        // packet来自video
        if(packet->stream_index == is->videoStream){
            packet_queue_put(&is->videoq, packet);
        }
        else if(packet->stream_index == is->audioStream){
            packet_queue_put(&is->audioq, packet);
        }
        else{
            av_free_packet(packet);
        }
    }

    while(is->quit){
        SDL_Delay(100);
    }

    fail:
    if(1){
        SDL_Event event;
        event.type = FF_QUIT_EVENT;
        event.user.data1 = is;
        SDL_PushEvent(&event);
    }

    return 0;

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

int stream_component_open(VideoState *is, int stream_index) {
    AVFormatContext *pFormatCtx = is->pFormatCtx;
    AVCodecContext *codecCtx;
    AVCodec *codec;
    SDL_AudioSpec wanted_spec, spec;

    if(stream_index < 0 || stream_index >= pFormatCtx->nb_streams){
        return -1;
    }
    // 获取解码器上下文
    codecCtx = pFormatCtx->streams[stream_index]->codec;
    if(codecCtx->codec_type == AVMEDIA_TYPE_AUDIO){
        // 从编解码信息设置音频设置
        wanted_spec.freq = codecCtx->sample_rate;
        wanted_spec.callback = audio_callback;
        wanted_spec.userdata = is;

        if(SDL_OpenAudio(&wanted_spec, &spec) < 0){
            fprintf(stderr, "SDL_OpenAudio: %s\n", SDL_GetError());
            return -1;
        }
    }
    codec = avcodec_find_decoder(codecCtx->codec_id);
    if(!codec || avcodec_open(codecCtx, codec) < 0){
        fprintf(stderr, "Unsupported codec!\n");
        return -1;
    }

    switch(codecCtx->codec_type){
        case AVMEDIA_TYPE_AUDIO:
            is->audioStream = stream_index;
            is->audio_st = pFormatCtx->streams[stream_index];
            is->audio_buf_size = 0;
            is->audio_buf_index = 0;
            memset(&is->audio_pkt, 0, sizeof(is->audio_pkt));
            packet_queue_init(&is->audioq);
            SDL_PauseAudio(0);
            break;
        case AVMEDIA_TYPE_VIDEO:
            is->videoStream = stream_index;
            is->video_st = pFormatCtx->streams[stream_index];
            packet_queue_init(&is->videoq);
            is->pictq_mutex = SDL_CreateMutex();
            is->pictq_cond = SDL_CreateCond();
            break;
        default:
            break;
    }

    return 0;
}

/**
 * ! 得到帧
 */
int video_thread(void *arg){
    VideoState *is = (VideoState *)arg;
    AVPacket pkt1, *packet = &pkt1;
    int len1, frameFinished;
    AVFrame *pFrame;

    pFrame = av_frame_alloc();

    for(;;){
        if(packet_queue_get(&is->videoq, packet, 1) < 0){
            // 退出
            break;
        }
        // 解码视频帧
        len1 = avcodec_decode_video2(is->video_st->codec, pFrame, &frameFinished, packet);

        // 是否取到视频帧
        if(frameFinished){
            if(queue_picture(is, pFrame) < 0){
                break;
            }
        }
        av_free_packet(packet);
    }
    av_free(pFrame);
    return 0;
}

/**
 * ! 把帧队列化
 */
int queue_picture(VideoState *is, AVFrame *pFrame){
    VideoPicture *vp;
    int dst_pix_fmt;
    AVPicture pict;

    // 等待空闲的图像队列
    SDL_LockMutex(is->pictq_mutex);
    while(is->pictq_size >= VIDEO_PICTURE_QUEUE_SIZE && !is->quit){
        SDL_CondWait(is->pictq_cond, is->pictq_mutex);
    }
    SDL_UnlockMutex(is->pictq_mutex);

    if(is->quit){
        return -1;
    }

    // 分配图像内存
    vp = &is->pictq[is->pictq_windex];
    if(!vp->bmp || vp->width != is->video_st->codec->width || vp->height != is->video_st->codec->height){
        SDL_Event event;

        vp->allocated = 0;
        event.type = FF_ALLOC_EVENT;
        event.user.data1 = is;
        SDL_PushEvent(&event);

        // 等待SDL分配图像内存
        SDL_LockMutex(is->pictq_mutex);
        while(!vp->allocated && !is->quit){
            SDL_CondWait(is->pictq_cond, is->pictq_mutex);
        }
        SDL_UnlockMutex(is->pictq_mutex);

        if(is->quit){
            return -1;
        }
    }
    if(vp->bmp){
        // 转换像素格式
        SDL_LockYUVOverlay(vp->bmp, 0);
    
        dst_pix_fmt = PIX_FMT_YUV420P;
        pict.data[0] = vp->bmp->pixels[0];
        pict.data[1] = vp->bmp->pixels[2];
        pict.data[2] = vp->bmp->pixels[1];
        pict.linesize[0] = vp->bmp->pitches[0];
        pict.linesize[1] = vp->bmp->pitches[2];
        pict.linesize[2] = vp->bmp->pitches[1];

        // 将图像转为SDL使用的YUV格式
        img_convert(&pict, dst_pix_fmt, (AVPicture *)pFrame, is->video_st->codec->pix_fmt, is->video_st->codec->width, is->video_st->codec->height);

        SDL_UnlockYUVOverlay(vp->bmp, 0);
        // 更新队列
        if(++is->pictq_windex == VIDEO_PICTURE_QUEUE_SIZE){
            is->pictq_windex = 0;
        }
        SDL_LockMutex(is->pictq_mutex);
        is->pictq_size++;
        SDL_UnlockMutex(is->pictq_mutex);
    }
    return 0;
}

void alloc_picture(void *userdata, VideoPicture *vp){
    VideoState *is = (VideoState *)userdata;
    VideoPicture *vp;

    vp = &is->pictq[is->pictq_windex];
    if(vp->bmp){
        // 释放旧的图像
        SDL_FreeYUVOverlay(vp->bmp);
    }

    // 为图像分配内存
    vp->bmp = SDL_CreateYUVOverlay(
        is->video_st->codec->width,
        is->video_st->codec->height,
        SDL_YV12_OVERLAY,
        screen
    );
    vp->width = is->video_st->codec->width;
    vp->height = is->video_st->codec->height;
    vp->allocated = 1;

    // 通知视频线程图像已分配
    SDL_LockMutex(is->pictq_mutex);

    vp->allocated = 1;
    SDL_CondSignal(is->pictq_cond);
    SDL_UnlockMutex(is->pictq_mutex);
}

/**
 * ! 显示视频
 */
static void schedule_refresh(VideoState *is, int delay) {
    SDL_AddTimer(delay, sdl_refresh_timer_cb, is);
}

static Uint32 sdl_refresh_timer_cb(Uint32 interval, void *opaque) {
    SDL_Event event;
    event.type = FF_REFRESH_EVENT;
    event.user.data1 = opaque;
    SDL_PushEvent(&event);
    return 0;
}

void video_refresh_timer(void *userdata) {
    VideoState *is = (VideoState *)userdata;
    VideoPicture *vp;
    if(is->video_st){
        if(is->pictq_size == 0){
            schedule_refresh(is, 1);
        }
        else{
            vp = &is->pictq[is->pictq_rindex];

            // 计算延迟
            schedule_refresh(is, 80);

            // 显示图像
            SDL_LockYUVOverlay(vp->bmp, 0);
            SDL_DisplayYUVOverlay(vp->bmp, &is->screen_rect);
            SDL_UnlockYUVOverlay(vp->bmp, 0);

            // 更新队列
            if(++is->pictq_rindex == VIDEO_PICTURE_QUEUE_SIZE){
                is->pictq_rindex = 0;
            }
            SDL_LockMutex(is->pictq_mutex);
            is->pictq_size--;
            SDL_CondSignal(is->pictq_cond);
            SDL_UnlockMutex(is->pictq_mutex);
        }
    }
    else{
        schedule_refresh(is, 100);
    }
}
// 显示视频
void video_display(VideoState *is) {
    SDL_Rect rect;
    VideoPicture *vp;
    AVPicture pict;
    float aspect_ratio;
    int w, h, x, y;
    int i;

    vp = &is->pictq[is->pictq_rindex];
    if(vp->bmp){
        if(is->video_st->codec->sample_aspect_ratio.num == 0){
            aspect_ratio = 0;
        }
        else{
            aspect_ratio = av_q2d(is->video_st->codec->sample_aspect_ratio) * is->video_st->codec->width / is->video_st->codec->height;
        }

        if(aspect_ratio <= 0.0){
            aspect_ratio = (float)is->video_st->codec->width / (float)is->video_st->codec->height;
        }

        h = screen->h;
        w = ((int)rint(h * aspect_ratio)) & -3;
        if(w > screen->w){
            w = screen->w;
            h = ((int)rint(w / aspect_ratio)) & -3;
        }
        x = (screen->w - w) / 2;
        y = (screen->h - h) / 2;

        rect.x = x;
        rect.y = y;
        rect.w = w;
        rect.h = h;
        SDL_DisplayYUVOverlay(vp->bmp, &rect);
    }
}   
int decode_interrupt_cb(void *ctx) {
    return (global_video_state && global_video_state->quit);
}