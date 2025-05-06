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

// 定义常量
#define VIDEO_PICTURE_QUEUE_SIZE 10
#define MAX_AUDIOQ_SIZE (5 * 16 * 1024)
#define MAX_VIDEOQ_SIZE (5 * 256 * 1024)
#define PIX_FMT_YUV420P AV_PIX_FMT_YUV420P

// 自定义事件类型
#define FF_REFRESH_EVENT (SDL_USEREVENT + 1)
#define FF_QUIT_EVENT (SDL_USEREVENT + 2)
#define FF_ALLOC_EVENT (SDL_USEREVENT + 3)

// 前向声明
typedef struct VideoState VideoState;

// 图像队列结构体
typedef struct VideoPicture {
    SDL_Texture *texture;  // 替换SDL_Overlay为SDL_Texture
    int width, height;
    int allocated;
} VideoPicture;

// 包队列结构体
typedef struct PacketQueue {
    AVPacketList *first_pkt, *last_pkt;
    int nb_packets;
    int size;
    SDL_mutex *mutex;
    SDL_cond *cond;
    int quit;  // 添加quit字段
} PacketQueue;

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
struct VideoState {
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
    
    // SDL2相关
    SDL_Window *window;
    SDL_Renderer *renderer;
    SDL_Texture *texture;
    SDL_Rect screen_rect;
    SDL_TimerID refresh_timer;
};

// 函数前向声明
static Uint32 sdl_refresh_timer_cb(Uint32 interval, void *opaque);
static void schedule_refresh(VideoState *is, int delay);
void SaveFrame(AVFrame *pFrame, int width, int height, int iFrame, const char *outdir);
int init_audio(AudioState *audio);
void audio_callback(void *userdata, Uint8 *stream, int len);
int audio_decode_frame(AudioState *audio);
int decode_thread(void *arg);
int video_thread(void *arg);
static int queue_picture(VideoState *is, AVFrame *pFrame);
void packet_queue_init(PacketQueue *q);
int packet_queue_put(PacketQueue *q, AVPacket *pkt);
int packet_queue_get(PacketQueue *q, AVPacket *pkt, int block);
static void alloc_picture(void *userdata);
static void video_refresh_timer(void *userdata);
void packet_queue_quit(PacketQueue *q);

// 声明变量
VideoState *global_video_state;
SDL_Window *screen;

int main(int argc, char *argv[])
{
    VideoState *is;
    AVPacket packet;
    
    // 初始化FFmpeg
    av_register_all();
    avformat_network_init();
    
    printf("FFmpeg initialized\n");
    
    is = (VideoState *)av_mallocz(sizeof(VideoState));
    if (!is) {
        fprintf(stderr, "Could not allocate VideoState\n");
        return -1;
    }
    
    // 安全处理命令行参数
    if (argc < 2) {
        // 使用默认文件路径
        strncpy(is->filename, "input.mp4", sizeof(is->filename) - 1);
        fprintf(stderr, "No input file specified, using default: %s\n", is->filename);
    } else {
        strncpy(is->filename, argv[1], sizeof(is->filename) - 1);
    }
    is->filename[sizeof(is->filename) - 1] = '\0';

    is->pictq_mutex = SDL_CreateMutex();
    is->pictq_cond = SDL_CreateCond();

    // 初始化SDL2
    if(SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_TIMER)) {
        fprintf(stderr, "Could not initialize SDL - %s\n", SDL_GetError());
        return -1;
    }

    // 创建窗口
    is->window = SDL_CreateWindow("FFmpeg Player",
                                SDL_WINDOWPOS_UNDEFINED,
                                SDL_WINDOWPOS_UNDEFINED,
                                640, 480,
                                SDL_WINDOW_SHOWN);
    if(!is->window) {
        fprintf(stderr, "SDL: could not create window - exiting\n");
        return -1;
    }

    // 创建渲染器
    is->renderer = SDL_CreateRenderer(is->window, -1, SDL_RENDERER_ACCELERATED);
    if(!is->renderer) {
        fprintf(stderr, "SDL: could not create renderer - exiting\n");
        return -1;
    }

    // 设置渲染器背景色(黑色)
    SDL_SetRenderDrawColor(is->renderer, 0, 0, 0, 255);
    SDL_RenderClear(is->renderer);
    SDL_RenderPresent(is->renderer);

    // 获取窗口尺寸
    SDL_GetWindowSize(is->window, &is->screen_rect.w, &is->screen_rect.h);
    
    // 初始化队列
    packet_queue_init(&is->videoq);
    packet_queue_init(&is->audioq);
    
    is->videoStream = -1;
    is->audioStream = -1;
    is->pictq_size = 0;
    is->pictq_rindex = 0;
    is->pictq_windex = 0;
    is->quit = 0; // 确保初始化为0
    
    global_video_state = is;
    
    // 设置第一次刷新
    schedule_refresh(is, 40);
    
    is->parse_tid = SDL_CreateThread(decode_thread, "decode_thread", is);
    if (!is->parse_tid) {
        fprintf(stderr, "Could not create decode thread\n");
        av_free(is);
        return -1;
    }
  
    // 等待解码线程完成初始化
    while (!is->pFormatCtx && !is->quit) {
        SDL_Delay(10);
    }
    
    if (is->quit) {
        fprintf(stderr, "Decode thread failed to initialize\n");
        return -1;
    }
    
    // 添加事件处理循环
    while (!is->quit) {
        SDL_Event event;
        
        // 使用超时来避免无限等待
        if (SDL_WaitEventTimeout(&event, 100)) {
            switch (event.type) {
                case FF_REFRESH_EVENT:
                    video_refresh_timer(event.user.data1);
                    break;
                case SDL_KEYDOWN:
                    if (event.key.keysym.sym == SDLK_ESCAPE || 
                        event.key.keysym.sym == SDLK_q) {
                        is->quit = 1;
                    }
                    break;
                case SDL_QUIT:
                    is->quit = 1;
                    break;
                case FF_QUIT_EVENT:
                    is->quit = 1;
                    break;
                case FF_ALLOC_EVENT:
                    alloc_picture(event.user.data1);
                    break;
                default:
                    break;
            }
        }
        
        // 添加手动检查退出条件
        if (SDL_GetKeyboardState(NULL)[SDL_SCANCODE_ESCAPE] || 
            SDL_GetKeyboardState(NULL)[SDL_SCANCODE_Q]) {
            fprintf(stderr, "Quit requested via keyboard check\n");
            is->quit = 1;
        }
    }  
    
    fprintf(stderr, "Exiting event loop, cleaning up...\n");
    
    // 设置队列退出标志
    packet_queue_quit(&is->audioq);
    packet_queue_quit(&is->videoq);
    
    // 通过SDL事件机制通知子线程退出
    SDL_Event event;
    event.type = FF_QUIT_EVENT;
    event.user.data1 = is;
    SDL_PushEvent(&event);
    
    // 清理资源
    if (is->parse_tid) {
        SDL_WaitThread(is->parse_tid, NULL);
    }
    
    if (is->video_tid) {
        SDL_WaitThread(is->video_tid, NULL);
    }
    
    // 销毁队列
    if (is->pictq_mutex) {
        SDL_DestroyMutex(is->pictq_mutex);
    }
    
    if (is->pictq_cond) {
        SDL_DestroyCond(is->pictq_cond);
    }
    
    // 销毁视频资源
    for (int i = 0; i < VIDEO_PICTURE_QUEUE_SIZE; i++) {
        if (is->pictq[i].texture) {
            SDL_DestroyTexture(is->pictq[i].texture);
        }
    }
    
    // 销毁SDL资源
    if (is->renderer) {
        SDL_DestroyRenderer(is->renderer);
    }
    
    if (is->window) {
        SDL_DestroyWindow(is->window);
    }
    
    SDL_Quit();
    
    // 释放VideoState
    if (is) {
        if (is->pFormatCtx) {
            avformat_close_input(&is->pFormatCtx);
        }
        av_free(is);
    }
    
    return 0;
}

// 解码线程函数
int decode_thread(void *arg) {
    VideoState *is = (VideoState *)arg;
    
    // 打开输入文件
    if(avformat_open_input(&is->pFormatCtx, is->filename, NULL, NULL) != 0) {
        fprintf(stderr, "Could not open file %s\n", is->filename);
        is->quit = 1;
        return -1;
    }
    
    // 获取流信息
    if(avformat_find_stream_info(is->pFormatCtx, NULL) < 0) {
        fprintf(stderr, "Could not find stream information\n");
        is->quit = 1;
        return -1;
    }
    
    // 输出视频信息
    av_dump_format(is->pFormatCtx, 0, is->filename, 0);
    
    // 初始化视频和音频流索引
    is->videoStream = -1;
    is->audioStream = -1;
    
    // 查找视频流和音频流
    for(unsigned int i = 0; i < is->pFormatCtx->nb_streams; i++) {
        if(is->pFormatCtx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO && 
           is->videoStream < 0) {
            is->videoStream = i;
        }
        if(is->pFormatCtx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_AUDIO && 
           is->audioStream < 0) {
            is->audioStream = i;
        }
    }
    
    // 打开视频流
    if(is->videoStream >= 0) {
        if (stream_component_open(is, is->videoStream) < 0) {
            fprintf(stderr, "Could not open video stream\n");
            is->videoStream = -1;
        } else {
            // 创建视频线程
            is->video_tid = SDL_CreateThread(video_thread, "video_thread", is);
            if (!is->video_tid) {
                fprintf(stderr, "Could not create video thread\n");
                is->quit = 1;
                return -1;
            }
        }
    }
    
    // 打开音频流
    if(is->audioStream >= 0) {
        if (stream_component_open(is, is->audioStream) < 0) {
            fprintf(stderr, "Could not open audio stream\n");
            is->audioStream = -1;
        }
    }
    
    if(is->videoStream < 0 && is->audioStream < 0) {
        fprintf(stderr, "Could not open any streams\n");
        is->quit = 1;
        return -1;
    }
    
    // 开始读取包
    AVPacket packet;
    while(!is->quit) {
        if(is->audioq.size > MAX_AUDIOQ_SIZE || is->videoq.size > MAX_VIDEOQ_SIZE) {
            SDL_Delay(10);
            continue;
        }
        
        if(av_read_frame(is->pFormatCtx, &packet) < 0) {
            if(avio_feof(is->pFormatCtx->pb) == 0) {
                SDL_Delay(100);
                continue;
            } else {
                break;
            }
        }
        
        // 分发包到相应队列
        if(packet.stream_index == is->videoStream) {
            packet_queue_put(&is->videoq, &packet);
        } else if(packet.stream_index == is->audioStream) {
            packet_queue_put(&is->audioq, &packet);
        } else {
            av_packet_unref(&packet);
        }
    }
    
    return 0;
}

// 包队列初始化
void packet_queue_init(PacketQueue *q) {
    memset(q, 0, sizeof(PacketQueue));
    q->mutex = SDL_CreateMutex();
    q->cond = SDL_CreateCond();
    q->quit = 0; // 初始化为0
}

// 包队列放入
int packet_queue_put(PacketQueue *q, AVPacket *pkt) {
    AVPacketList *pkt_list;
    
    pkt_list = av_malloc(sizeof(AVPacketList));
    if(!pkt_list)
        return -1;
    pkt_list->pkt = *pkt;
    pkt_list->next = NULL;
    
    SDL_LockMutex(q->mutex);
    
    if(!q->last_pkt)
        q->first_pkt = pkt_list;
    else
        q->last_pkt->next = pkt_list;
    q->last_pkt = pkt_list;
    q->nb_packets++;
    q->size += pkt_list->pkt.size;
    SDL_CondSignal(q->cond);
    
    SDL_UnlockMutex(q->mutex);
    return 0;
}

// 包队列取出
int packet_queue_get(PacketQueue *q, AVPacket *pkt, int block) {
    AVPacketList *pkt_list;
    int ret;
    
    SDL_LockMutex(q->mutex);
    
    for(;;) {
        if(q->quit) {
            ret = -1;
            break;
        }
        
        pkt_list = q->first_pkt;
        if(pkt_list) {
            q->first_pkt = pkt_list->next;
            if(!q->first_pkt)
                q->last_pkt = NULL;
            q->nb_packets--;
            q->size -= pkt_list->pkt.size;
            *pkt = pkt_list->pkt;
            av_free(pkt_list);
            ret = 1;
            break;
        } else if(!block) {
            ret = 0;
            break;
        } else {
            SDL_CondWaitTimeout(q->cond, q->mutex, 100); // 添加超时时间
        }
    }
    SDL_UnlockMutex(q->mutex);
    return ret;
}

// 添加一个函数来设置队列退出标志
void packet_queue_quit(PacketQueue *q) {
    SDL_LockMutex(q->mutex);
    q->quit = 1;
    SDL_CondSignal(q->cond);
    SDL_UnlockMutex(q->mutex);
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
    // 使用codecpar替代codec
    codecCtx = avcodec_alloc_context3(NULL);
    if (!codecCtx) {
        return -1;
    }
    if (avcodec_parameters_to_context(codecCtx, pFormatCtx->streams[stream_index]->codecpar) < 0) {
        avcodec_free_context(&codecCtx);
        return -1;
    }

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
    if(!codec || avcodec_open2(codecCtx, codec, NULL) < 0){
        fprintf(stderr, "Unsupported codec!\n");
        avcodec_free_context(&codecCtx);
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

// 修改video_thread函数
int video_thread(void *arg) {
    VideoState *is = (VideoState *)arg;
    AVPacket pkt1, *packet = &pkt1;
    AVFrame *pFrame;
    AVCodecContext *codecCtx;
    
    pFrame = av_frame_alloc();
    if (!pFrame) {
        fprintf(stderr, "Could not allocate video frame\n");
        return -1;
    }
    
    // 获取解码器上下文
    codecCtx = avcodec_alloc_context3(NULL);
    if (!codecCtx) {
        fprintf(stderr, "Could not allocate codec context\n");
        return -1;
    }
    
    if (avcodec_parameters_to_context(codecCtx, is->video_st->codecpar) < 0) {
        fprintf(stderr, "Could not copy codec parameters\n");
        return -1;
    }
    
    AVCodec *codec = avcodec_find_decoder(codecCtx->codec_id);
    if (!codec) {
        fprintf(stderr, "Unsupported codec\n");
        return -1;
    }
    
    if (avcodec_open2(codecCtx, codec, NULL) < 0) {
        fprintf(stderr, "Could not open codec\n");
        return -1;
    }

    for(;;) {
        if(is->quit) {
            break;
        }
        
        if(packet_queue_get(&is->videoq, packet, 1) < 0) {
            break;
        }
        
        // 发送包到解码器
        int ret = avcodec_send_packet(codecCtx, packet);
        if (ret < 0) {
            fprintf(stderr, "Error sending packet for decoding\n");
            av_packet_unref(packet);
            continue;
        }
        
        // 接收解码后的帧
        while (ret >= 0) {
            ret = avcodec_receive_frame(codecCtx, pFrame);
            if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
                break;
            } else if (ret < 0) {
                fprintf(stderr, "Error during decoding\n");
                break;
            }
            
            if(queue_picture(is, pFrame) < 0) {
                break;
            }
        }
        
        av_packet_unref(packet);
    }
    
    avcodec_free_context(&codecCtx);
    av_frame_free(&pFrame);
    return 0;
}

// 修改queue_picture函数
int queue_picture(VideoState *is, AVFrame *pFrame) {
    VideoPicture *vp;
    
    // 等待空闲的图像队列
    SDL_LockMutex(is->pictq_mutex);
    while(is->pictq_size >= VIDEO_PICTURE_QUEUE_SIZE && !is->quit) {
        SDL_CondWait(is->pictq_cond, is->pictq_mutex);
    }
    SDL_UnlockMutex(is->pictq_mutex);
    
    if(is->quit) {
        return -1;
    }
    
    // 获取写入位置
    vp = &is->pictq[is->pictq_windex];
    
    // 检查是否需要分配或重新分配纹理
    if(!vp->texture || vp->width != is->video_st->codecpar->width || 
       vp->height != is->video_st->codecpar->height) {
        
        // 已经有纹理，销毁它
        if(vp->texture) {
            SDL_DestroyTexture(vp->texture);
            vp->texture = NULL;
        }
        
        // 创建新纹理
        vp->texture = SDL_CreateTexture(
            is->renderer,
            SDL_PIXELFORMAT_IYUV,
            SDL_TEXTUREACCESS_STREAMING,
            is->video_st->codecpar->width,
            is->video_st->codecpar->height
        );
        
        if (!vp->texture) {
            fprintf(stderr, "SDL: could not create texture - %s\n", SDL_GetError());
            return -1;
        }
        
        vp->width = is->video_st->codecpar->width;
        vp->height = is->video_st->codecpar->height;
        vp->allocated = 1;
    }
    
    // 更新纹理
    if(vp->texture) {
        // 更新YUV平面
        SDL_UpdateYUVTexture(vp->texture, NULL,
                           pFrame->data[0], pFrame->linesize[0],
                           pFrame->data[1], pFrame->linesize[1],
                           pFrame->data[2], pFrame->linesize[2]);
        
        // 更新队列
        if(++is->pictq_windex == VIDEO_PICTURE_QUEUE_SIZE) {
            is->pictq_windex = 0;
        }
        
        SDL_LockMutex(is->pictq_mutex);
        is->pictq_size++;
        SDL_UnlockMutex(is->pictq_mutex);
    }
    
    return 0;
}

void alloc_picture(void *userdata) {
    VideoState *is = (VideoState *)userdata;
    VideoPicture *vp;

    vp = &is->pictq[is->pictq_windex];
    if(vp->texture){
        SDL_DestroyTexture(vp->texture);
    }

    vp->texture = SDL_CreateTexture(is->renderer,
                                  SDL_PIXELFORMAT_IYUV,
                                  SDL_TEXTUREACCESS_STREAMING,
                                  is->video_st->codecpar->width,
                                  is->video_st->codecpar->height);
    
    vp->width = is->video_st->codecpar->width;
    vp->height = is->video_st->codecpar->height;
    vp->allocated = 1;

    // 通知视频线程图像已分配
    SDL_LockMutex(is->pictq_mutex);
    SDL_CondSignal(is->pictq_cond);
    SDL_UnlockMutex(is->pictq_mutex);
}

/**
 * ! 显示视频
 */
static void schedule_refresh(VideoState *is, int delay) {
    SDL_Event event;
    event.type = FF_REFRESH_EVENT;
    event.user.data1 = is;
    
    SDL_RemoveTimer(is->refresh_timer);
    is->refresh_timer = SDL_AddTimer(delay, sdl_refresh_timer_cb, is);
    
    // 如果定时器创建失败，直接推送事件
    if (!is->refresh_timer) {
        SDL_PushEvent(&event);
    }
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
    
    if(is->video_st) {
        if(is->pictq_size == 0) {
            schedule_refresh(is, 1);
        } else {
            vp = &is->pictq[is->pictq_rindex];
            
            // 显示图像
            SDL_RenderClear(is->renderer);
            SDL_RenderCopy(is->renderer, vp->texture, NULL, &is->screen_rect);
            SDL_RenderPresent(is->renderer);
            
            // 计算下一帧延迟
            schedule_refresh(is, 40);
            
            // 更新队列
            if(++is->pictq_rindex == VIDEO_PICTURE_QUEUE_SIZE) {
                is->pictq_rindex = 0;
            }
            
            SDL_LockMutex(is->pictq_mutex);
            is->pictq_size--;
            SDL_CondSignal(is->pictq_cond);
            SDL_UnlockMutex(is->pictq_mutex);
        }
    } else {
        schedule_refresh(is, 100);
    }
}

// 显示视频
void video_display(VideoState *is) {
    SDL_Rect rect;
    VideoPicture *vp;
    float aspect_ratio;
    int w, h, x, y;

    vp = &is->pictq[is->pictq_rindex];
    if(vp->texture){
        if(is->video_st->codecpar->sample_aspect_ratio.num == 0){
            aspect_ratio = 0;
        }
        else{
            aspect_ratio = av_q2d(is->video_st->codecpar->sample_aspect_ratio) * 
                          is->video_st->codecpar->width / is->video_st->codecpar->height;
        }

        if(aspect_ratio <= 0.0){
            aspect_ratio = (float)is->video_st->codecpar->width / 
                          (float)is->video_st->codecpar->height;
        }

        h = is->screen_rect.h;
        w = ((int)rint(h * aspect_ratio)) & -3;
        if(w > is->screen_rect.w){
            w = is->screen_rect.w;
            h = ((int)rint(w / aspect_ratio)) & -3;
        }
        x = (is->screen_rect.w - w) / 2;
        y = (is->screen_rect.h - h) / 2;

        rect.x = x;
        rect.y = y;
        rect.w = w;
        rect.h = h;
        
        SDL_RenderClear(is->renderer);
        SDL_RenderCopy(is->renderer, vp->texture, NULL, &rect);
        SDL_RenderPresent(is->renderer);
    }
}

int decode_interrupt_cb(void *ctx) {
    return (global_video_state && global_video_state->quit);
}