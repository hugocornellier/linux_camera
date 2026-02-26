import 'dart:async';
import 'dart:io';

import 'package:camera/camera.dart';
import 'package:flutter/material.dart';
import 'package:media_kit/media_kit.dart';

import 'gallery_page.dart';
import 'photo_viewer_page.dart';
import 'recent_media.dart';
import 'video_player_page.dart';

void main() {
  WidgetsFlutterBinding.ensureInitialized();
  MediaKit.ensureInitialized();
  runApp(const CameraExampleApp());
}

class CameraExampleApp extends StatelessWidget {
  const CameraExampleApp({super.key});

  @override
  Widget build(BuildContext context) {
    return const MaterialApp(
      title: 'Camera Desktop Example',
      home: CameraExamplePage(),
    );
  }
}

enum CaptureMode { photo, video }

class CameraExamplePage extends StatefulWidget {
  const CameraExamplePage({super.key});

  @override
  State<CameraExamplePage> createState() => _CameraExamplePageState();
}

class _CameraExamplePageState extends State<CameraExamplePage> {
  CameraController? _controller;
  List<CameraDescription> _cameras = [];
  String? _errorMessage;
  bool _isInitialized = false;
  bool _isCapturing = false;

  CaptureMode _mode = CaptureMode.photo;
  bool _isRecording = false;
  Duration _recordingDuration = Duration.zero;
  Timer? _recordingTimer;

  final RecentMediaStore _mediaStore = RecentMediaStore();

  @override
  void initState() {
    super.initState();
    _initCamera();
  }

  Future<void> _initCamera() async {
    try {
      _cameras = await availableCameras();
      if (_cameras.isEmpty) {
        setState(() => _errorMessage = 'No cameras found');
        return;
      }

      final controller = CameraController(
        _cameras.first,
        ResolutionPreset.high,
        enableAudio: true,
      );

      await controller.initialize();
      if (!mounted) return;

      setState(() {
        _controller = controller;
        _isInitialized = true;
        _errorMessage = null;
      });
    } on CameraException catch (e) {
      setState(() => _errorMessage = 'Camera error: ${e.description}');
    } catch (e) {
      setState(() => _errorMessage = 'Error: $e');
    }
  }

  Future<void> _takePicture() async {
    if (_controller == null || !_isInitialized || _isCapturing) return;
    setState(() => _isCapturing = true);
    try {
      final file = await _controller!.takePicture();
      if (!mounted) return;
      _mediaStore.add(file.path, MediaType.photo);
      setState(() => _errorMessage = null);
    } on CameraException catch (e) {
      if (!mounted) return;
      ScaffoldMessenger.of(context).showSnackBar(
        SnackBar(content: Text('Capture failed: ${e.description}')),
      );
    } finally {
      if (mounted) setState(() => _isCapturing = false);
    }
  }

  Future<void> _startRecording() async {
    if (_controller == null || !_isInitialized || _isRecording) return;
    try {
      await _controller!.startVideoRecording();
      if (!mounted) return;
      setState(() {
        _isRecording = true;
        _recordingDuration = Duration.zero;
      });
      _recordingTimer = Timer.periodic(const Duration(seconds: 1), (_) {
        if (mounted) {
          setState(() => _recordingDuration += const Duration(seconds: 1));
        }
      });
    } on CameraException catch (e) {
      if (!mounted) return;
      ScaffoldMessenger.of(context).showSnackBar(
        SnackBar(content: Text('Recording failed: ${e.description}')),
      );
    }
  }

  Future<void> _stopRecording() async {
    if (!_isRecording) return;
    _recordingTimer?.cancel();
    try {
      final file = await _controller!.stopVideoRecording();
      if (!mounted) return;
      _mediaStore.add(file.path, MediaType.video);
      setState(() => _isRecording = false);
    } on CameraException catch (e) {
      if (!mounted) return;
      setState(() => _isRecording = false);
      ScaffoldMessenger.of(context).showSnackBar(
        SnackBar(content: Text('Stop recording failed: ${e.description}')),
      );
    }
  }

  String _formatTimer(Duration d) {
    final minutes = d.inMinutes.remainder(60).toString().padLeft(2, '0');
    final seconds = d.inSeconds.remainder(60).toString().padLeft(2, '0');
    return '$minutes:$seconds';
  }

  @override
  void dispose() {
    _recordingTimer?.cancel();
    _controller?.dispose();
    _mediaStore.dispose();
    super.dispose();
  }

  @override
  Widget build(BuildContext context) {
    return Scaffold(
      appBar: AppBar(
        title: const Text('Camera Desktop Example'),
        actions: [
          IconButton(
            icon: Badge(
              label: Text('${_mediaStore.count}'),
              isLabelVisible: _mediaStore.isNotEmpty,
              child: const Icon(Icons.photo_library),
            ),
            tooltip:
                'Gallery (${_mediaStore.count}/${RecentMediaStore.maxItems})',
            onPressed: () => Navigator.push(
              context,
              MaterialPageRoute(
                builder: (_) => GalleryPage(items: _mediaStore.items),
              ),
            ),
          ),
        ],
      ),
      body: Column(
        children: [
          Expanded(child: _buildPreview()),
          if (_isInitialized) _buildControlBar(),
          if (_mediaStore.isNotEmpty) _buildThumbnailStrip(),
          if (_errorMessage != null && !_isInitialized)
            Padding(
              padding: const EdgeInsets.all(8.0),
              child: Text(
                _errorMessage!,
                style: const TextStyle(color: Colors.red),
              ),
            ),
        ],
      ),
    );
  }

  Widget _buildPreview() {
    if (_errorMessage != null && !_isInitialized) {
      return Center(child: Text(_errorMessage!));
    }
    if (!_isInitialized || _controller == null) {
      return const Center(child: CircularProgressIndicator());
    }
    return Stack(
      children: [
        Center(child: CameraPreview(_controller!)),
        if (_isRecording)
          Positioned(
            top: 16,
            left: 0,
            right: 0,
            child: Center(
              child: Container(
                padding: const EdgeInsets.symmetric(
                  horizontal: 12,
                  vertical: 6,
                ),
                decoration: BoxDecoration(
                  color: Colors.black54,
                  borderRadius: BorderRadius.circular(16),
                ),
                child: Row(
                  mainAxisSize: MainAxisSize.min,
                  children: [
                    const Icon(
                      Icons.fiber_manual_record,
                      color: Colors.red,
                      size: 14,
                    ),
                    const SizedBox(width: 6),
                    Text(
                      'REC ${_formatTimer(_recordingDuration)}',
                      style: const TextStyle(
                        color: Colors.white,
                        fontSize: 14,
                        fontWeight: FontWeight.w600,
                      ),
                    ),
                  ],
                ),
              ),
            ),
          ),
      ],
    );
  }

  Widget _buildControlBar() {
    return Container(
      padding: const EdgeInsets.symmetric(vertical: 12, horizontal: 16),
      child: Row(
        mainAxisAlignment: MainAxisAlignment.center,
        children: [
          SegmentedButton<CaptureMode>(
            segments: const [
              ButtonSegment(
                value: CaptureMode.photo,
                icon: Icon(Icons.camera_alt),
              ),
              ButtonSegment(
                value: CaptureMode.video,
                icon: Icon(Icons.videocam),
              ),
            ],
            selected: {_mode},
            onSelectionChanged: _isRecording
                ? null
                : (selection) => setState(() => _mode = selection.first),
          ),
          const SizedBox(width: 24),
          _buildCaptureButton(),
        ],
      ),
    );
  }

  Widget _buildCaptureButton() {
    if (_mode == CaptureMode.photo) {
      return FloatingActionButton(
        onPressed: _isCapturing ? null : _takePicture,
        backgroundColor: Colors.white,
        foregroundColor: Colors.black87,
        child: _isCapturing
            ? const SizedBox(
                width: 24,
                height: 24,
                child: CircularProgressIndicator(strokeWidth: 2),
              )
            : const Icon(Icons.camera_alt),
      );
    }

    // Video mode
    if (_isRecording) {
      return FloatingActionButton(
        onPressed: _stopRecording,
        backgroundColor: Colors.red,
        foregroundColor: Colors.white,
        child: const Icon(Icons.stop),
      );
    }

    return FloatingActionButton(
      onPressed: _startRecording,
      backgroundColor: Colors.red,
      foregroundColor: Colors.white,
      child: const Icon(Icons.fiber_manual_record),
    );
  }

  Widget _buildThumbnailStrip() {
    final items = _mediaStore.items;
    return SizedBox(
      height: 72,
      child: ListView.builder(
        scrollDirection: Axis.horizontal,
        padding: const EdgeInsets.symmetric(horizontal: 8, vertical: 4),
        itemCount: items.length,
        itemBuilder: (context, index) {
          final item = items[index];
          return Padding(
            padding: const EdgeInsets.symmetric(horizontal: 4),
            child: GestureDetector(
              onTap: () => _onThumbnailTap(item, index),
              child: ClipRRect(
                borderRadius: BorderRadius.circular(8),
                child: SizedBox(
                  width: 64,
                  height: 64,
                  child: item.isVideo
                      ? _buildVideoThumbnail()
                      : _buildPhotoThumbnail(item),
                ),
              ),
            ),
          );
        },
      ),
    );
  }

  void _onThumbnailTap(MediaEntry item, int index) {
    if (item.isVideo) {
      Navigator.push(
        context,
        MaterialPageRoute(builder: (_) => VideoPlayerPage(path: item.path)),
      );
    } else {
      final photoItems = _mediaStore.items.where((e) => e.isPhoto).toList();
      final photoIndex = photoItems.indexOf(item);
      Navigator.push(
        context,
        MaterialPageRoute(
          builder: (_) => PhotoViewerPage(
            items: photoItems,
            initialIndex: photoIndex >= 0 ? photoIndex : 0,
          ),
        ),
      );
    }
  }

  Widget _buildVideoThumbnail() {
    return Container(
      color: Colors.grey.shade800,
      child: const Center(
        child: Icon(Icons.play_circle_fill, size: 28, color: Colors.white70),
      ),
    );
  }

  Widget _buildPhotoThumbnail(MediaEntry item) {
    return Image.file(
      File(item.path),
      width: 64,
      height: 64,
      fit: BoxFit.cover,
      cacheWidth: 120,
      errorBuilder: (_, error, stackTrace) => Container(
        width: 64,
        height: 64,
        color: Colors.grey.shade300,
        child: const Icon(Icons.broken_image, size: 20),
      ),
    );
  }
}
