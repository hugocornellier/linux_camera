#import <Foundation/Foundation.h>
#import <camera_desktop/camera_desktop-Swift.h>

void* _Nullable camera_desktop_get_image_stream_buffer(int64_t stream_handle) {
    return [ImageStreamHandleBridge getImageStreamBufferForHandle:stream_handle];
}

void camera_desktop_register_image_stream_callback(
    int64_t stream_handle,
    void (* _Nonnull callback)(int32_t))
{
    [ImageStreamHandleBridge registerImageStreamCallback:callback
                                               forHandle:stream_handle];
}

void camera_desktop_unregister_image_stream_callback(int64_t stream_handle) {
    [ImageStreamHandleBridge unregisterImageStreamCallbackForHandle:stream_handle];
}
