# RigMind

Source for the RigMind safety monitoring app.,
,
- RigMind_Desktop/: C++ native host, GStreamer pipeline, EWS plugin, tests, installer scripts.,
- Rigmind_v2/: TanStack UI (TSX), served inside WebView2 (Windows) or the Linux host.,
,
Not in this repo (too large or generated): detection models (export ppe.onnx, idss_model.onnx), BitNet runtime and model, third_party/bitnet.cpp, WebView2 SDK build outputs. See RigMind_Desktop/scripts/package_linux.sh.
