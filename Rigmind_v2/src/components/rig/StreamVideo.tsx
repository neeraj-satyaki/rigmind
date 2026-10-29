import { useEffect, useRef } from "react";
import { nativeCall, onNativeEvent } from "@/lib/native-bridge";

const GATHER_TIMEOUT_MS = 2000;

/**
 * Live camera video from the rigmind-pipeline process (RTSP H.264 passed through to WebRTC).
 * Covers its parent (position relative); the browser decodes, the host and pipeline do not.
 * Vanilla ICE: one offer with complete candidates, one answer event back from the host.
 */
export function StreamVideo({ camId, uri }: { camId: string; uri: string }) {
  const ref = useRef<HTMLVideoElement>(null);

  useEffect(() => {
    const video = ref.current;
    if (!video) return;
    let closed = false;

    const pc = new RTCPeerConnection({ iceServers: [] });
    pc.addTransceiver("video", { direction: "recvonly" });
    pc.ontrack = (event) => {
      video.srcObject = event.streams[0] ?? null;
    };

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
      await nativeCall("stream.open", { cam: camId, uri });
      const offer = await pc.createOffer();
      await pc.setLocalDescription(offer);
      await waitForGathering(pc);
      if (closed) return;
      await nativeCall("stream.offer", { cam: camId, sdp: pc.localDescription?.sdp ?? offer.sdp });
    })().catch((e) => console.warn("stream failed", camId, e));

    return () => {
      closed = true;
      unsubscribe();
      pc.close();
      video.srcObject = null;
      nativeCall("stream.close", { cam: camId }).catch(() => {});
    };
  }, [camId, uri]);

  return <video ref={ref} autoPlay muted playsInline className="absolute inset-0 h-full w-full object-cover" />;
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
