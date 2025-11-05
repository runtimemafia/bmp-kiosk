import { CameraGrabber } from "./camera";
import jsQR from "jsqr";
import jpeg from "jpeg-js";

/**
 * Decode a JPEG Buffer to an {data,width,height} compatible with jsQR.
 * jsQR expects a Uint8ClampedArray in RGBA order.
 */
function decodeJpegToImageData(jpegBuf: Buffer): { data: Uint8ClampedArray; width: number; height: number } {
  const raw = jpeg.decode(jpegBuf, { useTArray: true }); // returns {data: Uint8Array RGBA, width, height}
  // Wrap as Uint8ClampedArray without copying:
  const data = new Uint8ClampedArray(raw.data.buffer, raw.data.byteOffset, raw.data.byteLength);
  return { data, width: raw.width, height: raw.height };
}

/**
 * Scan a single frame for a QR code.
 * @param cam CameraGrabber
 * @param timeoutMs how long to wait for a fresh frame
 * @returns decoded text or null
 */
export async function scanQrOnce(cam: CameraGrabber, timeoutMs = 2000): Promise<string | null> {
  const jpegBuf = await cam.nextFrame(timeoutMs);
  if (!jpegBuf || jpegBuf.length === 0) return null;

  const { data, width, height } = decodeJpegToImageData(jpegBuf);

  // Optional: you can tune jsQR options here
  // const options = { inversionAttempts: "attemptBoth" as const };
  const result = jsQR(data, width, height);
  return result?.data ?? null;
}

/**
 * Keep grabbing frames until a QR is found or maxTries exhausted.
 */
export async function scanUntilFound(
  cam: CameraGrabber,
  opts: { maxTries?: number; perTryTimeoutMs?: number } = {},
  keepScanning?: boolean,
  executeWhenFound?: Function
): Promise<{ text: string | null; tries: number }> {
  let { maxTries = 10, perTryTimeoutMs = 2000 } = opts;

  let tries = 0;

  // infinite scan if maxTries is -1 or 0
  const infinite = maxTries <= 0;

  while (infinite || tries < maxTries) {
    tries++;
    const text = await scanQrOnce(cam, perTryTimeoutMs);
    if(text){
        if(keepScanning && executeWhenFound){
            executeWhenFound(text);
        }else{
            return {text, tries}
        }
    }
  }

  return { text: null, tries };
}
