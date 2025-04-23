#!/bin/bash

# 创建输出目录

# 生成带音频的 MP4 格式测试视频
echo "生成带音频的 MP4 测试视频..."
ffmpeg -y \
    -f lavfi -i testsrc=size=176x144:rate=25:duration=5 \
    -f lavfi -i "sine=frequency=440:duration=5" \
    -c:v libx264 -profile:v baseline -level 3.0 \
    -c:a aac -b:a 128k \
    -pix_fmt yuv420p -movflags +faststart \
    ../input/test_176x144.mp4

echo "测试视频已生成在 test_videos 目录中"
echo "你可以使用以下命令测试播放器:"
echo "./ffmpeg_demo01 ../input/test_176x144.mp4 output"
