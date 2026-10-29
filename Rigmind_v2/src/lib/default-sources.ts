// Cameras that are opened when none has been saved yet. The Camera page overwrites this by saving "stream.sources".
// camera id -> RTSP URI (credentials, if the camera needs any, go in the URI: rtsp://user:pass@host/path).
export const DEFAULT_STREAM_SOURCES: Record<string, string> = {
  "CAM-01": "rtsp://192.168.1.8:8556/feed0",
};
