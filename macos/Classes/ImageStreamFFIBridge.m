#import <Foundation/Foundation.h>
#import <camera_desktop/camera_desktop-Swift.h>

void* _Nullable camera_desktop_get_image_stream_buffer(int camera_id) {
    CameraDesktopPlugin* plugin = CameraDesktopPlugin.sharedInstance;
    if (!plugin) return NULL;
    return [plugin getImageStreamBufferForCamera:camera_id];
}

void camera_desktop_register_image_stream_callback(
    int camera_id,
    void (* _Nonnull callback)(int32_t))
{
    CameraDesktopPlugin* plugin = CameraDesktopPlugin.sharedInstance;
    if (!plugin) return;
    [plugin registerImageStreamCallback:callback forCamera:camera_id];
}

void camera_desktop_unregister_image_stream_callback(int camera_id) {
    CameraDesktopPlugin* plugin = CameraDesktopPlugin.sharedInstance;
    if (!plugin) return;
    [plugin unregisterImageStreamCallbackForCamera:camera_id];
}
