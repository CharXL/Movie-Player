#!/bin/bash

# 创建输出目录
mkdir -p test_videos

# 生成一个简单的测试视频 - H.264编码，176x144分辨率，5秒长度
echo "生成H.264测试视频..."
ffmpeg -y -f lavfi -i testsrc=size=176x144:rate=25:duration=5 \
       -c:v libx264 -profile:v baseline -level 3.0 -pix_fmt yuv420p \
       test_videos/test_176x144.264

# 生成一个简单的测试视频 - H.265/HEVC编码，176x144分辨率，5秒长度
echo "生成H.265测试视频..."
ffmpeg -y -f lavfi -i testsrc=size=176x144:rate=25:duration=5 \
       -c:v libx265 -preset fast -pix_fmt yuv420p \
       test_videos/test_176x144.265

# 生成一个简单的测试图像 - JPEG格式，176x144分辨率
echo "生成JPEG测试图像..."
ffmpeg -y -f lavfi -i testsrc=size=176x144:rate=1:duration=1 \
       -frames:v 1 -q:v 2 \
       test_videos/test_176x144.jpg

echo "所有测试文件已生成在 test_videos 目录中"
echo "你可以使用以下命令测试解码器:"
echo "./ffmpeg_demo01 test_videos/test_176x144.264"
echo "./ffmpeg_demo01 test_videos/test_176x144.265"
echo "./ffmpeg_demo01 test_videos/test_176x144.jpg"
