// 浏览器端验证：加载预览服务器上的外壳页面，检查渲染、资源、Range 音频与运行时错误。
// 仅用于开发验证，不参与打包。
import fs from "node:fs";
import path from "node:path";
import { fileURLToPath } from "node:url";
import { chromium } from "playwright";

const here = path.dirname(fileURLToPath(import.meta.url));
const base = process.env.PREVIEW_URL || "http://127.0.0.1:4173";
const outDir = path.join(here, ".local");
fs.mkdirSync(outDir, { recursive: true });

const audioDir = path.join(here, "public", "atlas", "audio");
const audioSample = fs.existsSync(audioDir) ? "/atlas/audio/" + findAudio(audioDir, "") : null;

// 音频按词条分目录存放，递归找一个真实文件
function findAudio(dir, prefix) {
  for (const entry of fs.readdirSync(dir, { withFileTypes: true })) {
    const rel = prefix ? `${prefix}/${entry.name}` : entry.name;
    if (entry.isDirectory()) {
      const found = findAudio(path.join(dir, entry.name), rel);
      if (found) return found;
    } else if (/\.(mp3|m4a|ogg)$/i.test(entry.name)) {
      return rel;
    }
  }
  return "";
}

let browser;
for (const options of [{ channel: "msedge" }, { channel: "chrome" }, {}]) {
  try {
    browser = await chromium.launch({ headless: true, ...options });
    break;
  } catch {
    // 换下一个浏览器实现
  }
}
if (!browser) {
  console.error("no chromium/edge available");
  process.exit(1);
}

const page = await browser.newPage({ viewport: { width: 1280, height: 860 } });
const errors = [];
const badResponses = [];

page.on("console", (msg) => {
  if (msg.type() === "error") errors.push(`console: ${msg.text()}`);
});
page.on("pageerror", (err) => errors.push(`pageerror: ${err.message}`));
page.on("response", (res) => {
  if (res.status() >= 400) badResponses.push(`${res.status()} ${res.url()}`);
});

const started = Date.now();
await page.goto(`${base}/`, { waitUntil: "load", timeout: 60000 });

// 等 React 挂载出内容
try {
  await page.waitForFunction(() => (document.getElementById("root")?.childElementCount ?? 0) > 0, {
    timeout: 30000,
  });
} catch {
  errors.push("root never rendered");
}

await page.waitForTimeout(1500);

const report = await page.evaluate(async (audio) => {
  const probe = async (url, headers = {}) => {
    try {
      const res = await fetch(url, { headers });
      const blob = await res.blob();
      return {
        status: res.status,
        bytes: blob.size,
        range: res.headers.get("content-range"),
      };
    } catch (err) {
      return { threw: String(err) };
    }
  };

  const hero = document.querySelector("img");
  return {
    title: document.title,
    href: location.href,
    rootChildren: document.getElementById("root")?.childElementCount ?? 0,
    text: (document.body.innerText || "").replace(/\s+/g, " ").slice(0, 400),
    stylesheets: document.styleSheets.length,
    firstImage: hero
      ? { src: hero.getAttribute("src"), complete: hero.complete, width: hero.naturalWidth }
      : null,
    resources: performance.getEntriesByType("resource").length,
    audioFull: audio ? await probe(audio) : null,
    audioRange: audio ? await probe(audio, { Range: "bytes=0-2047" }) : null,
  };
}, audioSample);

await page.screenshot({ path: path.join(outDir, "preview.png") });

// 开场动画有“跳过”，点掉后再拍一张真实首页
const skip = page.getByRole("button", { name: "跳过" });
if (await skip.count()) {
  await skip.first().click({ timeout: 5000 }).catch(() => null);
  await page.waitForTimeout(2500);
  await page.screenshot({ path: path.join(outDir, "preview-home.png") });
}

console.log(
  JSON.stringify(
    { ms: Date.now() - started, report, errors: errors.slice(0, 20), badResponses: badResponses.slice(0, 20) },
    null,
    2
  )
);

await browser.close();
