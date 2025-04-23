#!/bin/bash

INPUT_VIDEO="../input/13437831_1920_1080_25fps.mp4"
OUTPUT_VIDEO="../input/video_with_audio.mp4"

# 添加音频到视频
ffmpeg -i "$INPUT_VIDEO" \
    -f lavfi -i "sine=frequency=440:duration=6.60" \
    -c:v copy \
    -c:a aac -b:a 128k \
    -shortest \
    "$OUTPUT_VIDEO"

echo "已生成带音频的视频: $OUTPUT_VIDEO"