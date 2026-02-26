import 'package:flutter/foundation.dart';

enum MediaType { photo, video }

class MediaEntry {
  final String path;
  final DateTime capturedAt;
  final MediaType type;

  MediaEntry({
    required this.path,
    required this.capturedAt,
    required this.type,
  });

  bool get isVideo => type == MediaType.video;
  bool get isPhoto => type == MediaType.photo;
}

class RecentMediaStore extends ChangeNotifier {
  static const int maxItems = 10;
  final List<MediaEntry> _items = [];

  List<MediaEntry> get items => List.unmodifiable(_items);
  int get count => _items.length;
  bool get isEmpty => _items.isEmpty;
  bool get isNotEmpty => _items.isNotEmpty;

  void add(String path, MediaType type) {
    _items.insert(
      0,
      MediaEntry(path: path, capturedAt: DateTime.now(), type: type),
    );
    if (_items.length > maxItems) {
      _items.removeLast();
    }
    notifyListeners();
  }
}
