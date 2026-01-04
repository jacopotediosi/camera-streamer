#include "device/buffer_lock.h"
#include "output/placeholder.h"

DEFINE_BUFFER_LOCK(snapshot_lock, 0);
DEFINE_BUFFER_LOCK(stream_lock, 0);
DEFINE_BUFFER_LOCK(video_lock, 0);

void output_init_placeholders()
{
  // Set JPEG placeholder for snapshot and stream locks
  buffer_lock_set_placeholder(&snapshot_lock, (void*)placeholder_jpeg, placeholder_jpeg_size, true);
  buffer_lock_set_placeholder(&stream_lock, (void*)placeholder_jpeg, placeholder_jpeg_size, true);
  
  // Set H264 placeholder for video lock
  buffer_lock_set_placeholder(&video_lock, (void*)placeholder_h264, placeholder_h264_size, true);
}
