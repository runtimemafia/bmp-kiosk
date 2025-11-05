import "dotenv/config";                  // keep your .env flow
import { CameraGrabber } from "./camera";
import { scanUntilFound } from "./qr-scanner";
import { writeFileSync } from "node:fs";


const saveTest = (text: string) => {
  for(let i = 0; i < 10; i++){
    console.log(text);
  }
}

async function main() {
  const cam = new CameraGrabber();
  console.log("🔍 Looking for a QR... (show one to the camera)");

  

  const { text, tries } = await scanUntilFound(cam, { maxTries: -1, perTryTimeoutMs: 2000}, true, saveTest);

  if (text) {
    console.log(`✅ QR found in ${tries} tries:`, text);
  } else {
    console.log("❌ No QR detected (try moving closer, better light, or higher resolution).");
  }

  cam.stop();
}

main().catch(err => {
  console.error(err);
  process.exit(1);
});
