// import { spawn } from "node:child_process";
// import type { ChildProcessWithoutNullStreams } from "node:child_process";

// import os from "node:os";



// // ===== Config you can override via env =====
// const FPS = parseInt(process.env.FPS || "2", 10);
// const WIDTH = parseInt(process.env.WIDTH || "640", 10);
// const HEIGHT = parseInt(process.env.HEIGHT || "480", 10);
// const QUALITY = parseInt(process.env.QUALITY || "80", 10); // 1..100 for libcamera; mapped for ffmpeg
// const V4L2_DEVICE = process.env.V4L2_DEVICE || "/dev/video0";
// const USE_LIBCAMERA = process.env.PI === "1"; // set PI=1 on Raspberry Pi

// // ===== Utility: split a byte stream into complete JPEGs (SOI..EOI) =====
// function splitJpegs(buffer: Buffer): { frames: Buffer[]; leftover: Buffer } {
//   const frames: Buffer[] = [];
//   let start = -1;
//   for (let i = 0; i < buffer.length - 1; i++) {
//     const a = buffer[i], b = buffer[i + 1];
//     if (a === 0xff && b === 0xd8) start = i;           // SOI
//     if (a === 0xff && b === 0xd9 && start !== -1) {    // EOI
//       frames.push(buffer.subarray(start, i + 2));
//       start = -1;
//     }
//   }
//   // leftover = tail after the last full frame
//   if (frames.length === 0) return { frames, leftover: buffer };
//   const last = frames[frames.length - 1];
//   const end = buffer.lastIndexOf(last) + last.length;
//   return { frames, leftover: buffer.subarray(end) };
// }

// // ===== FrameSource interface =====
// interface FrameSource {
//   start(push: (jpeg: Buffer) => void): void;
//   stop(): void;
// }

// // ===== FFmpeg implementation (Arch dev) =====
// class FFmpegSource implements FrameSource {
//   private proc: ChildProcessWithoutNullStreams | null = null;
//   private leftover = Buffer.alloc(0);
//   private push: ((jpeg: Buffer) => void) | null = null;

//   start(push: (jpeg: Buffer) => void) {
//     if (this.proc) return;
//     this.push = push;

//     // Map QUALITY (1..100) to ffmpeg -q:v (2..31, lower is better)
//     const q = Math.max(2, Math.min(31, Math.round((100 - QUALITY) * 0.3 + 2)));

//     const args = [
//       "-hide_banner",
//       "-f", "video4linux2",
//       "-video_size", `${WIDTH}x${HEIGHT}`,
//       // Try native MJPEG from cam; if your webcam doesn't support this, remove the next two args
//       "-input_format", "mjpeg",
//       "-i", V4L2_DEVICE,
//       "-r", String(FPS),
//       "-f", "image2pipe",
//       "-q:v", String(q),
//       "-vcodec", "mjpeg",
//       "pipe:1",
//     ];

//     this.proc = spawn("ffmpeg", args, { stdio: ["ignore", "pipe", "inherit"] });
//     console.log(`[ffmpeg] pid=${this.proc.pid} OS=${os.platform()} ${os.arch()}`);

//     this.proc.stdout.on("data", (chunk: Buffer) => {
//       const data = Buffer.concat([this.leftover, chunk]);
//       const { frames, leftover } = splitJpegs(data);
//       this.leftover = leftover;
//       for (const f of frames) this.push && this.push(f);
//     });

//     this.proc.on("close", (code, signal) => {
//       console.log(`[ffmpeg] exit code=${code} signal=${signal}`);
//       this.proc = null;
//     });
//   }

//   stop() {
//     if (!this.proc) return;
//     this.proc.kill("SIGTERM");
//     this.proc = null;
//   }
// }

// // ===== libcamera implementation (Pi prod) =====
// class LibcameraSource implements FrameSource {
//   private proc: ChildProcessWithoutNullStreams | null = null;
//   private leftover = Buffer.alloc(0);
//   private push: ((jpeg: Buffer) => void) | null = null;

//   start(push: (jpeg: Buffer) => void) {
//     if (this.proc) return;
//     this.push = push;

//     const args = [
//       "-t", "0",
//       "--codec", "mjpeg",
//       "--framerate", String(FPS),
//       "--width", String(WIDTH),
//       "--height", String(HEIGHT),
//       "--quality", String(QUALITY),
//       "-o", "-",
//     ];

//     this.proc = spawn("libcamera-vid", args, { stdio: ["ignore", "pipe", "inherit"] });
//     console.log(`[libcamera-vid] pid=${this.proc.pid}`);

//     this.proc.stdout.on("data", (chunk: Buffer) => {
//       const data = Buffer.concat([this.leftover, chunk]);
//       const { frames, leftover } = splitJpegs(data);
//       this.leftover = leftover;
//       for (const f of frames) this.push && this.push(f);
//     });

//     this.proc.on("close", (code, signal) => {
//       console.log(`[libcamera-vid] exit code=${code} signal=${signal}`);
//       this.proc = null;
//     });
//   }

//   stop() {
//     if (!this.proc) return;
//     this.proc.kill("SIGTERM");
//     this.proc = null;
//   }
// }

// // ===== Factory =====
// function createSource(): FrameSource {
//   return USE_LIBCAMERA ? new LibcameraSource() : new FFmpegSource();
// }

// // ===== The grabber you’ll use from your main() =====
// export class CameraGrabber {
//   private source: FrameSource;
//   private started = false;
//   private pendingResolvers: Array<(buf: Buffer) => void> = [];
//   private lastFrame: Buffer | null = null;

//   constructor() {
//     this.source = createSource();
//   }

//   start() {

//     if (this.started) return;
//     this.started = true;
//     this.source.start((jpeg: Buffer) => {
//       this.lastFrame = jpeg;
//       // Fulfil one waiter (FIFO). If nobody waiting, keep lastFrame for potential fallback.
//       const resolver = this.pendingResolvers.shift();
//       if (resolver) resolver(jpeg);
//     });
//   }

//   stop() {
//     if (!this.started) return;
//     this.started = false;
//     this.source.stop();
//     // Reject any waiters since we are stopping
//     while (this.pendingResolvers.length) {
//       const r = this.pendingResolvers.shift();
//       r && r(Buffer.from("")); // empty buffer to indicate no frame; caller can check length
//     }
//   }

//   /**
//    * Returns the next JPEG frame as a Buffer.
//    * - If a fresh frame arrives soon, resolves with that.
//    * - Optional timeout (ms). If timed out and we have a lastFrame, returns that (stale-but-usable).
//    * - If no frame at all, resolves with an empty Buffer.
//    */
//   async nextFrame(timeoutMs = 1500): Promise<Buffer> {
//     if (!this.started) this.start();

//     return new Promise<Buffer>((resolve) => {
//       let settled = false;

//       // Arm timeout
//       const timer = setTimeout(() => {
//         if (settled) return;
//         settled = true;
//         // fall back to lastFrame if available
//         if (this.lastFrame) resolve(this.lastFrame);
//         else resolve(Buffer.alloc(0));
//       }, timeoutMs);

//       // Queue resolver to be fulfilled by the next incoming frame
//       this.pendingResolvers.push((buf: Buffer) => {
//         if (settled) return;
//         settled = true;
//         clearTimeout(timer);
//         resolve(buf);
//       });
//     });
//   }
// }




































// import { spawn } from "node:child_process";
// import type { ChildProcessWithoutNullStreams } from "node:child_process";
// import os from "node:os";

// /**
//  * Reads config from process.env (load .env in your entry: `import "dotenv/config"`)
//  * BACKEND: "ffmpeg" | "libcamera"
//  * FPS: number (default 2)
//  * WIDTH/HEIGHT: numbers; omit for full native resolution
//  * QUALITY: 1..100 (default 80) — for ffmpeg mapped to -q:v (2..31)
//  * DEVICE: /dev/video0 (ffmpeg only)
//  */
// const BACKEND = (process.env.BACKEND === "libcamera" ? "libcamera" : "ffmpeg") as "ffmpeg" | "libcamera";
// const FPS = Number.isFinite(Number(process.env.FPS)) ? Number(process.env.FPS) : 2;
// const WIDTH = process.env.WIDTH ? Number(process.env.WIDTH) : undefined;
// const HEIGHT = process.env.HEIGHT ? Number(process.env.HEIGHT) : undefined;
// const QUALITY = Number.isFinite(Number(process.env.QUALITY)) ? Number(process.env.QUALITY) : 80;
// const V4L2_DEVICE = process.env.DEVICE || "/dev/video0";

// /** Split a byte stream into complete JPEGs (SOI..EOI) and leftover tail */
// function splitJpegs(buffer: Buffer): { frames: Buffer[]; leftover: Buffer } {
//   const frames: Buffer[] = [];
//   let start = -1;
//   for (let i = 0; i < buffer.length - 1; i++) {
//     const a = buffer[i], b = buffer[i + 1];
//     if (a === 0xff && b === 0xd8) start = i;           // SOI
//     if (a === 0xff && b === 0xd9 && start !== -1) {    // EOI
//       frames.push(buffer.subarray(start, i + 2));
//       start = -1;
//     }
//   }
//   if (frames.length === 0) return { frames, leftover: buffer };
//   const last = frames[frames.length - 1];
//   const end = buffer.lastIndexOf(last) + last.length;
//   return { frames, leftover: buffer.subarray(end) };
// }

// interface FrameSource {
//   start(push: (jpeg: Buffer) => void): void;
//   stop(): void;
// }

// /** FFmpeg source (Arch/dev) */
// class FFmpegSource implements FrameSource {
//   private proc: ChildProcessWithoutNullStreams | null = null;
//   private leftover = Buffer.alloc(0);

//   start(push: (jpeg: Buffer) => void): void {
//     if (this.proc) return;

//     // Map QUALITY(1..100) to ffmpeg -q:v (2..31, lower is better)
//     const q = Math.max(2, Math.min(31, Math.round((100 - QUALITY) * 0.3 + 2)));

//     const args = [
//       "-hide_banner",
//       "-f", "video4linux2",
//       ...(WIDTH && HEIGHT ? ["-video_size", `${WIDTH}x${HEIGHT}`] : []),
//       // If your webcam doesn't support native MJPEG, remove the next two args:
//       "-input_format", "mjpeg",
//       "-i", V4L2_DEVICE,
//       "-r", String(FPS),
//       "-f", "image2pipe",
//       "-q:v", String(q),
//       "-vcodec", "mjpeg",
//       "pipe:1",
//     ];

//     this.proc = spawn("ffmpeg", args, { stdio: ["pipe", "pipe", "pipe"] });
//     this.proc.stderr.on("data", d => process.stderr.write(d)); // forward logs
//     console.log(`[ffmpeg] pid=${this.proc.pid} OS=${os.platform()} ${os.arch()} args=${args.join(" ")}`);

//     this.proc.stdout.on("data", (chunk: Buffer) => {
//       const data = Buffer.concat([this.leftover, chunk]);
//       const { frames, leftover } = splitJpegs(data);
//       this.leftover = leftover;
//       for (const f of frames) push(f);
//     });

//     this.proc.on("close", (code, signal) => {
//       console.log(`[ffmpeg] exit code=${code} signal=${signal}`);
//       this.proc = null;
//     });
//   }

//   stop(): void {
//     if (!this.proc) return;
//     this.proc.kill("SIGTERM");
//     this.proc = null;
//   }
// }

// /** libcamera source (Pi/prod) */
// class LibcameraSource implements FrameSource {
//   private proc: ChildProcessWithoutNullStreams | null = null;
//   private leftover = Buffer.alloc(0);

//   start(push: (jpeg: Buffer) => void): void {
//     if (this.proc) return;

//     const args = [
//       "-t", "0",
//       "--codec", "mjpeg",
//       "--framerate", String(FPS),
//       ...(WIDTH ? ["--width", String(WIDTH)] : []),
//       ...(HEIGHT ? ["--height", String(HEIGHT)] : []),
//       "--quality", String(QUALITY),
//       "-o", "-",
//     ];

//     this.proc = spawn("libcamera-vid", args, { stdio: ["pipe", "pipe", "pipe"] });
//     this.proc.stderr.on("data", d => process.stderr.write(d));
//     console.log(`[libcamera-vid] pid=${this.proc.pid} args=${args.join(" ")}`);

//     this.proc.stdout.on("data", (chunk: Buffer) => {
//       const data = Buffer.concat([this.leftover, chunk]);
//       const { frames, leftover } = splitJpegs(data);
//       this.leftover = leftover;
//       for (const f of frames) push(f);
//     });

//     this.proc.on("close", (code, signal) => {
//       console.log(`[libcamera-vid] exit code=${code} signal=${signal}`);
//       this.proc = null;
//     });
//   }

//   stop(): void {
//     if (!this.proc) return;
//     this.proc.kill("SIGTERM");
//     this.proc = null;
//   }
// }

// function createSource(): FrameSource {
//   return BACKEND === "libcamera" ? new LibcameraSource() : new FFmpegSource();
// }

// /** Pull-based grabber: call nextFrame() to get one JPEG Buffer */
// export class CameraGrabber {
//   private source: FrameSource = createSource();
//   private started = false;
//   private pendingResolvers: Array<(buf: Buffer) => void> = [];
//   private lastFrame: Buffer | null = null;

//   start(): void {
//     if (this.started) return;
//     this.started = true;
//     this.source.start((jpeg: Buffer) => {
//       this.lastFrame = jpeg;
//       const resolve = this.pendingResolvers.shift();
//       if (resolve) resolve(jpeg);
//     });
//   }

//   stop(): void {
//     if (!this.started) return;
//     this.started = false;
//     this.source.stop();
//     // resolve any waiters with empty buffer (caller can check length)
//     while (this.pendingResolvers.length) {
//       const r = this.pendingResolvers.shift();
//       if (r) r(Buffer.alloc(0));
//     }
//   }

//   /**
//    * Returns the next JPEG frame as a Buffer.
//    * If timeout expires, returns lastFrame if available, else empty Buffer.
//    */
//   async nextFrame(timeoutMs = 1500): Promise<Buffer> {
//     if (!this.started) this.start();
//     return new Promise<Buffer>((resolve) => {
//       let settled = false;

//       const timer = setTimeout(() => {
//         if (settled) return;
//         settled = true;
//         resolve(this.lastFrame ?? Buffer.alloc(0));
//       }, timeoutMs);

//       this.pendingResolvers.push((buf: Buffer) => {
//         if (settled) return;
//         settled = true;
//         clearTimeout(timer);
//         resolve(buf);
//       });
//     });
//   }
// }






































// // src/camera.ts
// import { spawn } from "node:child_process";
// import type { ChildProcessWithoutNullStreams } from "node:child_process";
// import os from "node:os";

// // .env should be loaded in your entry via: import "dotenv/config"
// type Backend = "ffmpeg" | "libcamera";
// const BACKEND: Backend = (process.env.BACKEND === "libcamera" ? "libcamera" : "ffmpeg");
// const FPS = Number.isFinite(Number(process.env.FPS)) ? Number(process.env.FPS) : 2;
// const WIDTH = process.env.WIDTH ? Number(process.env.WIDTH) : undefined;
// const HEIGHT = process.env.HEIGHT ? Number(process.env.HEIGHT) : undefined;
// const QUALITY = Number.isFinite(Number(process.env.QUALITY)) ? Number(process.env.QUALITY) : 80;
// const V4L2_DEVICE = process.env.DEVICE || "/dev/video0";

// type Bytes = Uint8Array;

// /** Split a byte stream into complete JPEGs (SOI..EOI) and leftover tail */
// function splitJpegs(buffer: Bytes): { frames: Bytes[]; leftover: Bytes } {
//   const frames: Bytes[] = [];
//   let start = -1;
//   for (let i = 0; i < buffer.length - 1; i++) {
//     const a = buffer[i], b = buffer[i + 1];
//     if (a === 0xff && b === 0xd8) start = i;           // SOI
//     if (a === 0xff && b === 0xd9 && start !== -1) {    // EOI
//       frames.push(buffer.subarray(start, i + 2));
//       start = -1;
//     }
//   }
//   if (frames.length === 0) return { frames, leftover: buffer };
//   const last = frames[frames.length - 1];
//   const end = buffer.lastIndexOf(last) + last.length;
//   return { frames, leftover: buffer.subarray(end) };
// }

// interface FrameSource {
//   start(push: (jpeg: Bytes) => void): void;
//   stop(): void;
// }

// /** FFmpeg source (Arch/dev) */
// class FFmpegSource implements FrameSource {
//   private proc: ChildProcessWithoutNullStreams | null = null;
//   private leftover: Bytes = new Uint8Array(0);

//   start(push: (jpeg: Bytes) => void): void {
//     if (this.proc) return;

//     // Map QUALITY(1..100) to ffmpeg -q:v (2..31, lower is better)
//     const q = Math.max(2, Math.min(31, Math.round((100 - QUALITY) * 0.3 + 2)));

//     const args = [
//       "-hide_banner",
//       "-f", "video4linux2",
//       ...(WIDTH && HEIGHT ? ["-video_size", `${WIDTH}x${HEIGHT}`] : []),
//       // If your webcam doesn't support native MJPEG, remove the next two args:
//       "-input_format", "mjpeg",
//       "-i", V4L2_DEVICE,
//       "-r", String(FPS),
//       "-f", "image2pipe",
//       "-q:v", String(q),
//       "-vcodec", "mjpeg",
//       "pipe:1",
//     ];

//     this.proc = spawn("ffmpeg", args, { stdio: ["pipe", "pipe", "pipe"] });
//     this.proc.stderr.on("data", d => process.stderr.write(d));
//     console.log(`[ffmpeg] pid=${this.proc.pid} OS=${os.platform()} ${os.arch()} args=${args.join(" ")}`);

//     this.proc.stdout.on("data", (chunk: Bytes) => {
//       const data = concatBytes(this.leftover, chunk);
//       const { frames, leftover } = splitJpegs(data);
//       this.leftover = leftover;
//       for (const f of frames) push(f);
//     });

//     this.proc.on("close", (code, signal) => {
//       console.log(`[ffmpeg] exit code=${code} signal=${signal}`);
//       this.proc = null;
//     });
//   }

//   stop(): void {
//     if (!this.proc) return;
//     this.proc.kill("SIGTERM");
//     this.proc = null;
//   }
// }

// /** libcamera source (Pi/prod) */
// class LibcameraSource implements FrameSource {
//   private proc: ChildProcessWithoutNullStreams | null = null;
//   private leftover: Bytes = new Uint8Array(0);

//   start(push: (jpeg: Bytes) => void): void {
//     if (this.proc) return;

//     const args = [
//       "-t", "0",
//       "--codec", "mjpeg",
//       "--framerate", String(FPS),
//       ...(WIDTH ? ["--width", String(WIDTH)] : []),
//       ...(HEIGHT ? ["--height", String(HEIGHT)] : []),
//       "--quality", String(QUALITY),
//       "-o", "-",
//     ];

//     this.proc = spawn("libcamera-vid", args, { stdio: ["pipe", "pipe", "pipe"] });
//     this.proc.stderr.on("data", d => process.stderr.write(d));
//     console.log(`[libcamera-vid] pid=${this.proc.pid} args=${args.join(" ")}`);

//     this.proc.stdout.on("data", (chunk: Bytes) => {
//       const data = concatBytes(this.leftover, chunk);
//       const { frames, leftover } = splitJpegs(data);
//       this.leftover = leftover;
//       for (const f of frames) push(f);
//     });

//     this.proc.on("close", (code, signal) => {
//       console.log(`[libcamera-vid] exit code=${code} signal=${signal}`);
//       this.proc = null;
//     });
//   }

//   stop(): void {
//     if (!this.proc) return;
//     this.proc.kill("SIGTERM");
//     this.proc = null;
//   }
// }

// function createSource(): FrameSource {
//   return BACKEND === "libcamera" ? new LibcameraSource() : new FFmpegSource();
// }

// /** Pull-based grabber: call nextFrame() to get one JPEG (Uint8Array) */
// export class CameraGrabber {
//   private source: FrameSource = createSource();
//   private started = false;
//   private pendingResolvers: Array<(buf: Bytes) => void> = [];
//   private lastFrame: Bytes | null = null;

//   start(): void {
//     if (this.started) return;
//     this.started = true;
//     this.source.start((jpeg: Bytes) => {
//       this.lastFrame = jpeg;
//       const resolve = this.pendingResolvers.shift();
//       if (resolve) resolve(jpeg);
//     });
//   }

//   stop(): void {
//     if (!this.started) return;
//     this.started = false;
//     this.source.stop();
//     while (this.pendingResolvers.length) {
//       const r = this.pendingResolvers.shift();
//       if (r) r(new Uint8Array(0));
//     }
//   }

//   /**
//    * Returns the next JPEG as Uint8Array.
//    * If timeout expires, returns lastFrame if available, else empty array.
//    */
//   async nextFrame(timeoutMs = 1500): Promise<Bytes> {
//     if (!this.started) this.start();
//     return new Promise<Bytes>((resolve) => {
//       let settled = false;

//       const timer = setTimeout(() => {
//         if (settled) return;
//         settled = true;
//         resolve(this.lastFrame ?? new Uint8Array(0));
//       }, timeoutMs);

//       this.pendingResolvers.push((buf: Bytes) => {
//         if (settled) return;
//         settled = true;
//         clearTimeout(timer);
//         resolve(buf);
//       });
//     });
//   }
// }

// /** Small helper to concat Uint8Array/Buffer efficiently without type headaches */
// function concatBytes(a: Bytes, b: Bytes): Bytes {
//   if ((a as any).constructor?.name === "Buffer" || (b as any).constructor?.name === "Buffer") {
//     // Runtime is Buffer: use Buffer.concat and return as Uint8Array (compatible)
//     // eslint-disable-next-line @typescript-eslint/no-explicit-any
//     const BufferCtor: any = Buffer;
//     const res: Uint8Array = BufferCtor.concat([a as unknown as Buffer, b as unknown as Buffer]);
//     return res;
//   }
//   const out = new Uint8Array(a.length + b.length);
//   out.set(a, 0);
//   out.set(b, a.length);
//   return out;
// }








































import { spawn } from "node:child_process";
import type { ChildProcess } from "node:child_process";
import os from "node:os";

const BACKEND = (process.env.BACKEND === "libcamera" ? "libcamera" : "ffmpeg") as "ffmpeg" | "libcamera";
const FPS = Number(process.env.FPS ?? 2);
const WIDTH = process.env.WIDTH ? Number(process.env.WIDTH) : undefined;
const HEIGHT = process.env.HEIGHT ? Number(process.env.HEIGHT) : undefined;
const QUALITY = Number(process.env.QUALITY ?? 80);
const DEVICE = process.env.DEVICE ?? "/dev/video0";

type Bytes = Buffer;

function splitJpegs(buffer: Bytes): { frames: Bytes[]; leftover: Bytes } {
  const frames: Bytes[] = [];
  let start = -1;

  for (let i = 0; i < buffer.length - 1; i++) {
    if (buffer[i] === 0xff && buffer[i + 1] === 0xd8) start = i; // SOI
    if (buffer[i] === 0xff && buffer[i + 1] === 0xd9 && start !== -1) {
      frames.push(buffer.subarray(start, i + 2)); // EOI
      start = -1;
    }
  }

  if (frames.length === 0) return { frames, leftover: buffer };

  const last = frames[frames.length - 1];
  const end = buffer.lastIndexOf(last) + last.length;

  return { frames, leftover: buffer.subarray(end) };
}

interface FrameSource {
  start(push: (jpeg: Bytes) => void): void;
  stop(): void;
}

class FFmpegSource implements FrameSource {
  private proc: ChildProcess | null = null;
  private leftover: Bytes = Buffer.alloc(0);

  start(push: (jpeg: Bytes) => void) {
    if (this.proc) return;

    const q = Math.max(2, Math.min(31, Math.round((100 - QUALITY) * 0.3 + 2)));

    const args = [
      "-hide_banner",
      "-f", "video4linux2",
      ...(WIDTH && HEIGHT ? ["-video_size", `${WIDTH}x${HEIGHT}`] : []),
      "-input_format", "mjpeg",
      "-i", DEVICE,
      "-r", String(FPS),
      "-f", "image2pipe",
      "-q:v", String(q),
      "-vcodec", "mjpeg",
      "pipe:1",
    ];

    this.proc = spawn("ffmpeg", args, { stdio: ["pipe", "pipe", "pipe"] });
    this.proc.stderr?.on("data", d => process.stderr.write(d));

    console.log(`[ffmpeg] ${this.proc.pid} ${os.platform()} ${os.arch()}`);

    this.proc.stdout?.on("data", (chunk: Bytes) => {
      const data = Buffer.concat([this.leftover, chunk]);
      const { frames, leftover } = splitJpegs(data);
      this.leftover = leftover;
      for (const f of frames) push(f);
    });

    this.proc.on("close", () => { this.proc = null; });
  }

  stop() {
    this.proc?.kill("SIGTERM");
    this.proc = null;
  }
}

class LibcameraSource implements FrameSource {
  private proc: ChildProcess | null = null;
  private leftover: Bytes = Buffer.alloc(0);

  start(push: (jpeg: Bytes) => void) {
    if (this.proc) return;

    const args = [
      "-t", "0",
      "--codec", "mjpeg",
      "--framerate", String(FPS),
      ...(WIDTH ? ["--width", String(WIDTH)] : []),
      ...(HEIGHT ? ["--height", String(HEIGHT)] : []),
      "--quality", String(QUALITY),
      "-o", "-",
    ];

    this.proc = spawn("libcamera-vid", args, { stdio: ["pipe", "pipe", "pipe"] });
    this.proc.stderr?.on("data", d => process.stderr.write(d));

    console.log(`[libcamera-vid] ${this.proc.pid}`);

    this.proc.stdout?.on("data", (chunk: Bytes) => {
      const data = Buffer.concat([this.leftover, chunk]);
      const { frames, leftover } = splitJpegs(data);
      this.leftover = leftover;
      for (const f of frames) push(f);
    });

    this.proc.on("close", () => { this.proc = null; });
  }

  stop() {
    this.proc?.kill("SIGTERM");
    this.proc = null;
  }
}

function createSource(): FrameSource {
  return BACKEND === "libcamera" ? new LibcameraSource() : new FFmpegSource();
}

export class CameraGrabber {
  private source = createSource();
  private started = false;
  private pending: Array<(buf: Bytes) => void> = [];
  private last: Bytes | null = null;

  start() {
    if (this.started) return;
    this.started = true;

    this.source.start(jpeg => {
      this.last = jpeg;
      const resolve = this.pending.shift();
      if (resolve) resolve(jpeg);
    });
  }

  stop() {
    this.started = false;
    this.source.stop();
    this.pending.forEach(r => r(Buffer.alloc(0)));
    this.pending = [];
  }

  async nextFrame(timeout = 1500): Promise<Bytes> {
    if (!this.started) this.start();

    return new Promise<Bytes>(resolve => {
      let done = false;
      const t = setTimeout(() => {
        if (!done) { done = true; resolve(this.last ?? Buffer.alloc(0)); }
      }, timeout);

      this.pending.push(buf => {
        if (done) return;
        done = true;
        clearTimeout(t);
        resolve(buf);
      });
    });
  }
}
