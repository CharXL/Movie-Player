#!/bin/bash

# 编译ffmpeg_demo01.c
echo "=== 编译ffmpeg_demo01 ==="

# 方法1: 直接指定所有库，确保正确的链接顺序
gcc -o ffmpeg_demo01 ffmpeg_demo01.c \
    -lavformat -lavcodec -lswscale -lavutil -lm

# 如果上面的命令失败，尝试方法2
if [ $? -ne 0 ]; then
    echo "=== 方法1失败，尝试方法2 ==="
    gcc -o ffmpeg_demo01 ffmpeg_demo01.c \
        $(pkg-config --cflags --libs libavformat libavcodec libswscale libavutil) \
        -lm
fi

# 如果编译成功，显示测试命令
if [ $? -eq 0 ]; then
    echo "=== 编译成功! ==="
    echo "使用以下命令测试:"
    echo "./ffmpeg_demo01 test_videos/test_176x144.264"
fi
