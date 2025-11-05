import { spawn } from "node:child_process";
import type { ChildProcess } from "node:child_process";
import os from "node:os";

const BACKEND = (process.env.BACKEND === "libcamera"
  ? "libcamera"
  : process.env.BACKEND === "rpicam"
  ? "rpicam"
  : "ffmpeg") as "ffmpeg" | "libcamera" | "rpicam";
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

class RPiCamSource implements FrameSource {
  private proc: ChildProcess | null = null;
  private leftover: Buffer = Buffer.alloc(0);

  start(push: (jpeg: Buffer) => void): void {
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

    this.proc = spawn("rpicam-vid", args, { stdio: ["pipe", "pipe", "pipe"] });
    this.proc.stderr?.on("data", d => process.stderr.write(d));
    console.log(`[rpicam-vid] ${this.proc.pid} args=${args.join(" ")}`);

    this.proc.stdout?.on("data", (chunk: Buffer) => {
      const data = Buffer.concat([this.leftover, chunk]);
      const { frames, leftover } = splitJpegs(data);
      this.leftover = leftover;
      for (const f of frames) push(f);
    });

    this.proc.on("close", () => { this.proc = null; });
  }

  stop(): void {
    this.proc?.kill("SIGTERM");
    this.proc = null;
  }
}

function createSource(): FrameSource {
  if (BACKEND === "rpicam") return new RPiCamSource();
  if (BACKEND === "libcamera") return new LibcameraSource();
  return new FFmpegSource();
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
