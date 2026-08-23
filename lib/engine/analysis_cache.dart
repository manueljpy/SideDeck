import 'package:path/path.dart' as p;
import 'package:sidedeck/engine/track_analyzer.dart';
import 'package:sqflite/sqflite.dart';

class AnalysisCache {
  AnalysisCache._();
  static final instance = AnalysisCache._();

  static const _analyzer = 'queenmary';
  Database? _db;

  static Future<void> _createTable(Database db) async {
    await db.execute('''
      CREATE TABLE analysis (
        path TEXT NOT NULL,
        analyzer TEXT NOT NULL,
        size INTEGER NOT NULL,
        mtime INTEGER NOT NULL,
        bpm REAL,
        key_idx INTEGER,
        beat_offset REAL,
        PRIMARY KEY (path, analyzer)
      )
    ''');
  }

  Future<Database> _open() async {
    if (_db != null) return _db!;
    final dbPath = await getDatabasesPath();
    _db = await openDatabase(
      p.join(dbPath, 'sidedeck_analysis.db'),
      version: 9,
      onCreate: (db, version) => _createTable(db),
      onUpgrade: (db, oldVersion, newVersion) async {
        await db.execute('DROP TABLE IF EXISTS analysis');
        await _createTable(db);
      },
    );
    return _db!;
  }

  Future<TrackAnalysis?> get(String path, int size, int mtime) async {
    final db = await _open();
    final rows = await db.query(
      'analysis',
      where: 'path = ? AND analyzer = ? AND size = ? AND mtime = ?',
      whereArgs: [path, _analyzer, size, mtime],
      limit: 1,
    );
    if (rows.isEmpty) return null;
    final row = rows.first;
    return TrackAnalysis(
      bpm: (row['bpm'] as num?)?.toDouble() ?? 0,
      key: (row['key_idx'] as num?)?.toInt() ?? -1,
      beatOffset: (row['beat_offset'] as num?)?.toDouble() ?? 0,
    );
  }

  Future<void> put({
    required String path,
    required int size,
    required int mtime,
    required double bpm,
    required int key,
    required double beatOffset,
  }) async {
    final db = await _open();
    await db.insert('analysis', {
      'path': path,
      'analyzer': _analyzer,
      'size': size,
      'mtime': mtime,
      'bpm': bpm,
      'key_idx': key,
      'beat_offset': beatOffset,
    }, conflictAlgorithm: ConflictAlgorithm.replace);
  }
}
