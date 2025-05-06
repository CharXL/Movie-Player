#!/bin/bash

# 编译ffmpeg_demo01.c
echo "=== 编译ffmpeg_demo01 ==="

# 方法1: 直接指定所有库，确保正确的链接顺序
gcc -o ffmpeg_demo01 ffmpeg_demo01.c \
    -lavformat -lavcodec -lswscale -lavutil -lswresample -lz -lm \
    -lSDL2 -lSDL2main \
    -I/usr/include/SDL2 \
    -D_REENTRANT \
    -Wall -g


# 如果编译成功，显示测试命令
if [ $? -eq 0 ]; then
    echo "=== 编译成功! ==="
    echo "使用以下命令测试:"
    echo "./ffmpeg_demo01 ../../input/test_176x144.264 ../../output"
fi
