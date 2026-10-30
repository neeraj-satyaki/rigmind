import { useEffect, useRef, useState } from "react";
import RigMindMark from "@/components/loaders/RigMindMark";
import { LiveDetections } from "@/components/rig/LiveDetections";
import { useCamera } from "@/lib/camera-status";
import { nativeCall, onNativeEvent } from "@/lib/native-bridge";
import { currentOverlayPrefs } from "@/lib/detections";

const GATHER_TIMEOUT_MS = 2000;
const FRAME_STALL_MS = 6000;

/** Grey panel, animated white RigMind mark and a label. Shown while the stream connects or the camera is down. */
export function StreamConnecting({ label = "Connecting", detail }: { label?: string; detail?: string }) {
  return (
    <div
      role="status"
      aria-live="polite"
      className="absolute inset-0 z-10 flex flex-col items-center justify-center gap-3 bg-[#3B4350] text-white"
    >
      <RigMindMark size={64} tone="light" />
      <span className="text-[11px] font-medium uppercase tracking-[0.14em]">{label}</span>
      {detail && <span className="max-w-[80%] text-center text-[11px] text-white/70">{detail}</span>}
    </div>
  );
}

/**
 * Live camera video from the rigmind-pipeline process (`raw`: the incoming picture only, no detection drawn on it) (RTSP H.264 decoded on the GPU and re-encoded for WebRTC).
 * Covers its parent (position relative); the browser decodes, the host does not.
 * The camera state comes from the host: the panel shows "Connecting" until the first frame is playing, and again
 * whenever the camera drops and the host is polling to reconnect. A new WebRTC session is made on every (re)connect.
 * Vanilla ICE: one offer with complete candidates, one answer event back from the host.
 */
export function StreamVideo({ camId, uri, raw = false }: { camId: string; uri: string; raw?: boolean }) {
  const ref = useRef<HTMLVideoElement>(null);
  const camera = useCamera(camId);
  const [playing, setPlaying] = useState(false);
  const [session, setSession] = useState(0);
  const state = camera?.state ?? "connecting";

  // Open the camera (the host keeps it open if it already is) and ask for its status.
  useEffect(() => {
    nativeCall("stream.open", { cam: camId, uri }).catch((e) => console.warn("stream.open failed", camId, e));
  }, [camId, uri]);

  // Raw: the camera picture as it comes in, with no PPE/IDSS boxes drawn into the video (Connect a camera). The saved overlay
  // choice is put back when the raw view goes away.
  useEffect(() => {
    if (!raw) return;
    nativeCall("stream.overlay", { cam: camId, ppe: false, idss: false }).catch((e) => console.warn("raw view not applied", e));
    return () => {
      nativeCall("stream.overlay", { cam: camId, ...currentOverlayPrefs() }).catch(() => {});
    };
  }, [raw, camId]);

  // One WebRTC session per (re)connect of the camera.
  useEffect(() => {
    const video = ref.current;
    if (!video || state !== "online") {
      setPlaying(false);
      return;
    }
    let closed = false;
    setPlaying(false);

    const pc = new RTCPeerConnection({ iceServers: [] });
    pc.addTransceiver("video", { direction: "recvonly" });
    pc.ontrack = (event) => {
      // Live view: play frames as soon as they arrive. The host paces and time-stamps the video at its real send cadence, so
      // the browser's adaptive jitter buffer (which otherwise grows towards a second on a bursty feed) has nothing to absorb.
      const receiver = event.receiver as RTCRtpReceiver & { jitterBufferTarget?: number | null; playoutDelayHint?: number };
      try {
        receiver.jitterBufferTarget = 0;
      } catch {
        /* not supported by this WebView2 runtime */
      }
      try {
        receiver.playoutDelayHint = 0;
      } catch {
        /* not supported */
      }
      video.srcObject = event.streams[0] ?? null;
    };
    const onPlaying = () => setPlaying(true);
    video.addEventListener("playing", onPlaying);

    const unsubscribe = onNativeEvent((msg) => {
      if (msg.cam !== camId) return;
      if (msg.ev === "answer" && typeof msg["sdp"] === "string") {
        pc.setRemoteDescription({ type: "answer", sdp: msg["sdp"] }).catch((e) =>
          console.warn("stream answer rejected", e),
        );
      } else if (msg.ev === "error") {
        console.warn("stream error", camId, msg["message"]);
      }
    });

    (async () => {
      const offer = await pc.createOffer();
      await pc.setLocalDescription(offer);
      await waitForGathering(pc);
      if (closed) return;
      await nativeCall("stream.offer", { cam: camId, sdp: pc.localDescription?.sdp ?? offer.sdp });
    })().catch((e) => console.warn("stream failed", camId, e));

    // The host drops the viewer if the pipeline restarts; if the picture stops, start a new session.
    let lastTime = -1;
    let lastMove = Date.now();
    const watchdog = setInterval(() => {
      if (video.currentTime !== lastTime) {
        lastTime = video.currentTime;
        lastMove = Date.now();
      } else if (Date.now() - lastMove > FRAME_STALL_MS) {
        setSession((n) => n + 1);
      }
    }, 1000);

    return () => {
      closed = true;
      clearInterval(watchdog);
      unsubscribe();
      video.removeEventListener("playing", onPlaying);
      pc.close();
      video.srcObject = null;
      // The viewer leaves; the camera keeps recording and raising alarms.
      nativeCall("stream.detach", { cam: camId }).catch(() => {});
    };
  }, [camId, state, session]);

  let overlay: React.ReactNode = null;
  if (state === "offline") {
    const retry = camera && camera.attempt > 0 ? `Retrying · attempt ${camera.attempt}` : "Retrying";
    overlay = <StreamConnecting label="Camera offline" detail={[camera?.reason, retry].filter(Boolean).join(" · ")} />;
  } else if (!playing) {
    overlay = <StreamConnecting label="Connecting" />;
  }

  return (
    <>
      <video ref={ref} autoPlay muted playsInline className="absolute inset-0 h-full w-full bg-black object-contain" />
      {playing && state === "online" && !raw && <LiveDetections camId={camId} video={ref} />}
      {overlay}
    </>
  );
}

function waitForGathering(pc: RTCPeerConnection): Promise<void> {
  if (pc.iceGatheringState === "complete") return Promise.resolve();
  return new Promise<void>((resolve) => {
    const check = () => {
      if (pc.iceGatheringState === "complete") {
        pc.removeEventListener("icegatheringstatechange", check);
        resolve();
      }
    };
    pc.addEventListener("icegatheringstatechange", check);
    setTimeout(() => {
      pc.removeEventListener("icegatheringstatechange", check);
      resolve();
    }, GATHER_TIMEOUT_MS);
  });
}
