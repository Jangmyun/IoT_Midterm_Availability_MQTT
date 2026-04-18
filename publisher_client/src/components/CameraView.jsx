// CameraView.jsx
// 카메라 프리뷰 + 프레임 차분 기반 모션 감지
//
// 감지 원리:
//   200ms 마다 캔버스에 현재 프레임 캡처 → 이전 프레임과 픽셀 차이 계산
//   변화율이 sensitivity(%) 초과 시 onMotion(pct) 콜백 호출

import { useRef, useEffect, useState, useCallback } from 'react';

const SAMPLE_MS  = 200;   // 감지 주기 (ms)
const PIXEL_DIFF = 30;    // 채널 합산 차이 임계치 (0~765)
const CANVAS_W   = 320;
const CANVAS_H   = 240;

export function CameraView({ onMotion, enabled, sensitivity = 2 }) {
  const videoRef    = useRef(null);
  const canvasRef   = useRef(null);
  const prevDataRef = useRef(null);
  const intervalRef = useRef(null);
  const [camStatus, setCamStatus] = useState('idle'); // idle | active | denied

  const startCamera = useCallback(async () => {
    try {
      const stream = await navigator.mediaDevices.getUserMedia({
        video: { facingMode: 'environment', width: CANVAS_W, height: CANVAS_H },
      });
      videoRef.current.srcObject = stream;
      setCamStatus('active');
    } catch {
      setCamStatus('denied');
    }
  }, []);

  const stopCamera = useCallback(() => {
    videoRef.current?.srcObject?.getTracks().forEach(t => t.stop());
    if (videoRef.current) videoRef.current.srcObject = null;
    prevDataRef.current = null;
    setCamStatus('idle');
  }, []);

  // 모션 감지 루프
  useEffect(() => {
    clearInterval(intervalRef.current);
    if (!enabled || camStatus !== 'active') return;

    intervalRef.current = setInterval(() => {
      const video  = videoRef.current;
      const canvas = canvasRef.current;
      if (!video || !canvas || video.readyState < 2) return;

      const ctx  = canvas.getContext('2d', { willReadFrequently: true });
      ctx.drawImage(video, 0, 0, CANVAS_W, CANVAS_H);
      const curr = ctx.getImageData(0, 0, CANVAS_W, CANVAS_H).data;

      if (prevDataRef.current) {
        let changed = 0;
        const total = CANVAS_W * CANVAS_H;
        for (let i = 0; i < curr.length; i += 4) {
          const d = Math.abs(curr[i]   - prevDataRef.current[i])
                  + Math.abs(curr[i+1] - prevDataRef.current[i+1])
                  + Math.abs(curr[i+2] - prevDataRef.current[i+2]);
          if (d > PIXEL_DIFF) changed++;
        }
        const pct = (changed / total) * 100;
        if (pct > sensitivity) onMotion?.(pct);
      }

      prevDataRef.current = new Uint8ClampedArray(curr);
    }, SAMPLE_MS);

    return () => clearInterval(intervalRef.current);
  }, [enabled, camStatus, sensitivity, onMotion]);

  return (
    <div className="camera-view">
      <div className="camera-preview-wrap">
        <video
          ref={videoRef}
          autoPlay
          playsInline
          muted
          className="camera-preview"
        />
        {/* 감지용 캔버스 — 화면에 표시하지 않음 */}
        <canvas
          ref={canvasRef}
          width={CANVAS_W}
          height={CANVAS_H}
          className="camera-canvas--hidden"
        />
        {camStatus === 'idle' && (
          <div className="camera-overlay">
            <span className="camera-overlay__icon">📷</span>
          </div>
        )}
        {camStatus === 'denied' && (
          <div className="camera-overlay camera-overlay--error">
            <span>카메라 권한이 거부됨</span>
            <small>브라우저 설정에서 카메라를 허용하세요</small>
          </div>
        )}
        {enabled && camStatus === 'active' && (
          <div className="camera-badge">
            <span className="dot dot--red" /> 감지 중
          </div>
        )}
      </div>

      <div className="camera-actions">
        {camStatus === 'idle' && (
          <button className="btn btn--primary" onClick={startCamera}>
            카메라 시작
          </button>
        )}
        {camStatus === 'active' && (
          <button className="btn btn--ghost" onClick={stopCamera}>
            카메라 중지
          </button>
        )}
      </div>
    </div>
  );
}
